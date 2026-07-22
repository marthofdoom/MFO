#!/usr/bin/env python3
"""esp_inspect.py — a reusable TES5 plugin record inspector.

Replaces the throwaway parsers. Walks any .esp/.esm/.esl, descends nested
GRUPs, transparently inflates zlib-compressed records, and DECODES the
structures MFO actually reasons about instead of printing raw hex at you.

DOCTRINE (same as MFO_GenerateESP.py): never trust a format doc — dump a
working record and mirror it. Every byte layout decoded below was checked
against Skyrim.esm before it was written down, and the ones that could not
be confirmed from data are labelled `?` rather than guessed at confidently.
`--selftest` pins the facts that were confirmed.

MODES
  (none)                  plugin summary: masters, record counts by type
  --list TYPE             every record of TYPE with FormID + EDID
  --dump FORMID|EDID      full ordered subrecord dump, hex + decoded
  --templates             every PACK type-19 template + its public inputs
  --find-template FORMID  every PACK instance whose PKCU points at a template
  --selftest              assert known-good facts

OPTIONS
  --master PATH           resolve FormIDs into this master (repeatable)
  --filter SUBSTR         restrict --list/--templates by EDID substring
  --limit N               cap rows (default 200; 0 = no cap)
  --raw                   in --dump, always show hex even when decoded

EXAMPLES
  esp_inspect.py Skyrim.esm --dump 000504F5
  esp_inspect.py Skyrim.esm --templates --filter UseMagic
  esp_inspect.py Skyrim.esm --find-template 000504F5
  esp_inspect.py out/MFO.esp --master Skyrim.esm --dump 01000820
"""

import argparse
import os
import re
import struct
import sys
import zlib

REC_HDR = 24
GRUP_HDR = 24
FLAG_COMPRESSED = 0x00040000
FLAG_DELETED = 0x00000020
FLAG_IGNORED = 0x00001000
FLAG_ESL = 0x00000200

_u32 = struct.Struct('<I')
_u16 = struct.Struct('<H')


# ══ enums / flag tables ════════════════════════════════════════════════════
# Each table's comment records HOW its values were established, and every
# table pairs with a *_CONFIRMED set. Anything not in that set prints with a
# trailing '?', and a set bit with no name at all prints as bit<N>. So the
# output distinguishes "this was checked against Skyrim.esm" from "this is a
# conventional name nobody here has verified" — per INVARIANTS 62, and
# because a plausible-looking name is how a wrong reading survives review.
#
# The measurements quoted below are reproducible from this file: they were
# taken by walking all 5,961 PACK / 1,811 QUST records of Skyrim.esm and
# resolving each union member against the record table.

GRUP_TYPES = {
    0: 'top', 1: 'world children', 2: 'interior cell block',
    3: 'interior cell sub-block', 4: 'exterior cell block',
    5: 'exterior cell sub-block', 6: 'cell children', 7: 'topic children',
    8: 'cell persistent children', 9: 'cell temporary children',
    10: 'quest children',
}

# PKDT.type (u8).
# MEASURED over all 5,961 PACK records in Skyrim.esm: the ONLY values that
# occur are 18 (5,857) and 19 (104). The 104 matches ENGINE_NOTES 0.17's
# template count exactly. Skyrim moved package behaviour into the TEMPLATE
# (PKCU) and left this byte as a two-valued discriminator, so the long
# Oblivion-era type list some docs print is DEAD for Skyrim — do not restore
# it, it will only make a garbage value look like a real answer.
PACK_TYPES = {
    18: 'Package (instance)',
    19: 'Package TEMPLATE',
}
PACK_TYPES_CONFIRMED = {18, 19}

# PKDT general flags (u32).
# CONFIRMED bit 0 from data: of the 85 vanilla packages that set it, the EDIDs
# are shopkeepers, traders, innkeepers, animal trainers and blacksmiths
# (RiftenBersiShopkeeper, MS13LucanValeriusTraderServices, NightgateInnkeeper).
# 0x04 and 0x08 are named in MFO_GenerateESP's own PKDT comment.
# EVERYTHING ELSE IS AN UNVERIFIED CONVENTIONAL NAME and prints with a '?'.
# Vanilla additionally sets bits 4, 9, 18, 20, 27 and 29, which no name here
# covers — they print as bit<N>. Do not invent names for them.
PKDT_FLAGS = {
    0x00000001: 'OffersServices', 0x00000004: 'MustComplete',
    0x00000008: 'MaintainSpeedAtGoal', 0x00000080: 'OncePerDay',
    0x00000400: 'DoOnce', 0x00001000: 'SkipFalloutBehavior',
    0x00002000: 'AlwaysSneak', 0x00004000: 'AllowSwimming',
    0x00010000: 'IgnoreCombat', 0x00020000: 'WeaponsUnequipped',
    0x00080000: 'WeaponDrawn', 0x00200000: 'NoCombatAlert',
    0x00800000: 'WearSleepOutfit',
}
PKDT_FLAGS_CONFIRMED = {0x00000001, 0x00000004, 0x00000008}

# PKDT interrupt flags (u16). NONE of these names is confirmed — all print
# with '?'. Note bits 10..15 are set together in exactly 4,013 records, which
# looks like one "all interrupts" idiom rather than six independent flags.
PKDT_INTERRUPT_FLAGS = {
    0x0001: 'HellosToPlayer', 0x0002: 'RandomConversations',
    0x0004: 'ObserveCombatBehavior', 0x0008: 'GreetCorpseBehavior',
    0x0010: 'ReactionToPlayerActions', 0x0020: 'FriendlyFireComments',
    0x0040: 'AggroRadiusBehavior', 0x0080: 'AllowIdleChatter',
    0x0200: 'WorldInteractions',
}
PKDT_INTERRUPT_CONFIRMED = set()

# PTDA.targType (i32) — union member decided by resolving every vanilla value.
# MEASURED in Skyrim.esm (count / what the union actually held):
#   0: 1498  REFR 917, ACHR 38, PlayerRef(0x14) 543   -> a placed reference
#   1:  173  100% base forms: SHOU 50, SPEL 47, FACT 44, FLST 17, NPC_ 6
#   2: 1388  ACTI 448, MISC 440, + small enum values   -> base object/type
#   3:  445  KYWD 236, else 0                          -> linked by keyword
#   4:  236  small ints 0..358 and -1, NEVER a FormID  -> ALIAS INDEX
#   6:   91  always 0 (no operand)                     -> self
#   5, 7: never occur in vanilla.
PTDA_TYPES = {
    0: 'specific reference', 1: 'object', 2: 'object type',
    3: 'linked reference', 4: 'reference ALIAS', 5: 'unused in vanilla',
    6: 'self', 7: 'unused in vanilla',
}
PTDA_TYPES_CONFIRMED = {0, 1, 3, 4, 6}
PTDA_FORMID_TYPES = {0, 1, 2, 3}   # union member is a FormID for these
PTDA_ALIAS_TYPE = 4

# PLDT.locType (i32) — same treatment.
# MEASURED in Skyrim.esm:
#   0: 4048  REFR 3290, ACHR 37, PlayerRef 721   -> near a placed reference
#   1:  448  100% CELL                           -> in cell
#   2:  341  always 0 (no operand)
#   3:  605  always 0 (no operand)
#   6:  416  KYWD 331, else 0                    -> near linked reference
#   8:  585  small ints and -1, NEVER a FormID   -> ALIAS INDEX
#   9:    1  a DOOR FormID (single occurrence — name not established)
#  12:  394  always 0; this is what the TEMPLATES carry as their default
#   4, 5, 7, 10, 11: never occur in vanilla.
PLDT_TYPES = {
    0: 'near reference', 1: 'in cell', 2: 'near package start location',
    3: 'near editor location', 6: 'near linked reference',
    8: 'reference ALIAS', 9: 'near reference (single vanilla use)',
    12: 'no location (template default)',
}
PLDT_TYPES_CONFIRMED = {0, 1, 2, 3, 6, 8, 12}
PLDT_FORMID_TYPES = {0, 1, 6, 9}
PLDT_ALIAS_TYPE = 8

# FormIDs the ENGINE hardcodes, which exist in no plugin's record table.
# PlayerRef is the one that matters: it is the target of 543 vanilla package
# PTDAs and the location of 721 PLDTs, and resolving it to "not found" was
# actively misleading.
HARDCODED_FIDS = {
    0x00000014: 'PlayerRef [engine-hardcoded, no record]',
}

# QUST.DNAM flags (u16). CONFIRMED: 0x0001 Enabled, 0x0010 StartsEnabled and
# 0x0100 RunOnce — all three are pinned by MFO_GenerateESP.qust_dnam, which
# was decoded from all 1,811 vanilla QUSTs (this tool counts 1,811 QUST
# records in Skyrim.esm, so that survey covered the whole set). The generator
# specifically records that run-once is 0x0100 and NOT 0x0004. The rest are
# unverified conventional names and print with '?'.
QUST_FLAGS = {
    0x0001: 'Enabled', 0x0002: 'Completed', 0x0004: 'AddIdleToHello',
    0x0008: 'AllowRepeatedStages', 0x0010: 'StartsEnabled',
    0x0020: 'DisplayedInHUD', 0x0040: 'Failed', 0x0080: 'StageWait',
    0x0100: 'RunOnce', 0x0200: 'ExcludeFromDialogueExport',
    0x0400: 'WarnOnAliasFillFailure', 0x0800: 'Active',
    0x1000: 'RepeatsConditions', 0x2000: 'KeepInstance',
    0x4000: 'WantDormant', 0x8000: 'HasDialogueData',
}
QUST_FLAGS_CONFIRMED = {0x0001, 0x0010, 0x0100}

QUST_TYPES = {
    0: 'None', 1: 'Main', 2: 'MagesGuild', 3: 'ThievesGuild',
    4: 'DarkBrotherhood', 5: 'CompanionQuests', 6: 'Miscellaneous',
    7: 'Daedric', 8: 'SideQuest', 9: 'CivilWar', 10: 'DLC01Vampire',
    11: 'DLC02Dragonborn',
}

# Quest alias FNAM flags (u32). CONFIRMED: 0x0002 Optional, 0x0008
# AllowReuseInQuest, 0x0200 AllowReserved — the three MFO_CommandQuest sets,
# named identically in its generator comments. The rest print with '?'.
ALIAS_FLAGS = {
    0x00000001: 'ReservesLocationReference', 0x00000002: 'Optional',
    0x00000004: 'QuestObject', 0x00000008: 'AllowReuseInQuest',
    0x00000010: 'AllowDead', 0x00000020: 'InLoadedArea',
    0x00000040: 'Essential', 0x00000080: 'AllowDisabled',
    0x00000100: 'StoresText', 0x00000200: 'AllowReserved',
    0x00000400: 'Protected', 0x00000800: 'NoFillType',
    0x00001000: 'AllowDestroyed', 0x00002000: 'Closest',
    0x00004000: 'UsesStoredText', 0x00008000: 'InitiallyDisabled',
    0x00010000: 'AllowCleared', 0x00020000: 'ClearsNameWhenRemoved',
    0x00040000: 'DontGiveName', 0x00080000: 'Unknown19',
    0x00100000: 'SimpleTarget', 0x00200000: 'Unknown21',
}
ALIAS_FLAGS_CONFIRMED = {0x00000002, 0x00000008, 0x00000200}

SPEL_TYPES = {
    0: 'Spell', 1: 'Disease', 2: 'Power', 3: 'LesserPower', 4: 'Ability',
    5: 'Poison', 6: 'Addiction', 7: 'Voice',
}

# Subrecords whose entire payload is one FormID, per record signature.
FORMID_SUBS = {
    ('PACK', 'QNAM'), ('PACK', 'CNAM_'), ('QUST', 'ALFR'), ('QUST', 'ALPC'),
    ('QUST', 'ALCO'), ('QUST', 'ALDN'), ('QUST', 'ALFA'), ('QUST', 'ALSP'),
    ('QUST', 'ALUA'), ('SPEL', 'EFID'), ('SPEL', 'ETYP'), ('SPEL', 'MDOB'),
    ('MGEF', 'MDOB'), ('KYWD', 'CNAM'),
}


def _flags(val, table, confirmed=None):
    """Decode a bitfield, accounting for EVERY set bit.

    A name that has not been confirmed against real records prints with a
    trailing '?', and a set bit with no name at all prints as bit<N>. The
    point is that the output never asserts something that was not checked —
    a plausible-looking flag name is exactly how a wrong reading survives.
    """
    if val == 0:
        return 'none'
    names, left = [], val
    for bit in sorted(table):
        if val & bit:
            nm = table[bit]
            if confirmed is not None and bit not in confirmed:
                nm += '?'
            names.append(nm)
            left &= ~bit
    for b in range(64):
        if left >> b & 1:
            names.append(f'bit{b}')
    return ' | '.join(names)


def _enum(val, table, confirmed=None):
    name = table.get(val)
    if name is None:
        return f'{val} (?)'
    if confirmed is not None and val not in confirmed:
        return f'{val} = {name}?'
    return f'{val} = {name}'


def _zstr(b):
    return b.split(b'\x00', 1)[0].decode('cp1252', 'replace')


def _printable(b):
    return ''.join(chr(c) if 32 <= c < 127 else '.' for c in b)


def hexdump(data, indent='        ', limit=None):
    out, n = [], len(data)
    shown = n if limit is None else min(n, limit)
    for i in range(0, shown, 16):
        chunk = data[i:i + 16]
        h = ' '.join(f'{c:02X}' for c in chunk)
        out.append(f'{indent}{i:04X}  {h:<47}  |{_printable(chunk)}|')
    if shown < n:
        out.append(f'{indent}...  ({n - shown} more bytes)')
    return out


# ══ plugin model ═══════════════════════════════════════════════════════════

class Record:
    __slots__ = ('sig', 'fid', 'flags', 'off', 'size', 'plugin', 'group')

    def __init__(self, sig, fid, flags, off, size, plugin, group):
        self.sig, self.fid, self.flags = sig, fid, flags
        self.off, self.size, self.plugin, self.group = off, size, plugin, group

    @property
    def compressed(self):
        return bool(self.flags & FLAG_COMPRESSED)

    def body(self):
        """Raw record body, inflated if the compressed flag is set.

        A compressed body is: uint32 decompressed size, then a zlib stream.
        """
        raw = self.plugin.data[self.off + REC_HDR:self.off + REC_HDR + self.size]
        if not self.compressed:
            return raw
        if len(raw) < 4:
            return b''
        try:
            return zlib.decompress(raw[4:])
        except zlib.error as e:
            print(f'  WARN: {self.sig} {self.fid:08X} zlib failure: {e}', file=sys.stderr)
            return b''

    def subs(self):
        """Ordered [(type, bytes)]. Order and repetition are LOAD-BEARING for
        PACK/QUST, so this never collapses to a dict.

        Handles the XXXX escape: a subrecord whose length field cannot hold
        the real size is preceded by XXXX carrying a uint32 size.
        """
        body, out, i, n = self.body(), [], 0, None
        pos, end = 0, len(body)
        while pos + 6 <= end:
            t = body[pos:pos + 4].decode('ascii', 'replace')
            ln = _u16.unpack_from(body, pos + 4)[0]
            pos += 6
            if t == 'XXXX':
                n = _u32.unpack_from(body, pos)[0]
                pos += ln
                continue
            if n is not None:
                ln, n = n, None
            if pos + ln > end:
                out.append((t, body[pos:end]))
                break
            out.append((t, body[pos:pos + ln]))
            pos += ln
        return out

    def edid(self):
        for t, d in self.subs():
            if t == 'EDID':
                return _zstr(d)
            # EDID is always first when present; bail early on big records.
            return ''
        return ''


class Plugin:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, 'rb') as f:
            self.data = f.read()
        self.records = []
        self.by_fid = {}
        self.by_sig = {}
        self.masters = []
        self.header = None
        self._edid_cache = {}
        self._scan()

    def _scan(self):
        """Header-only walk: never inflates a body. Descends every GRUP by
        stepping over its 24-byte header, which is why nesting is free."""
        data, n, off = self.data, len(self.data), 0
        stack = []          # [(end_offset, label)]
        while off + REC_HDR <= n:
            while stack and off >= stack[-1][0]:
                stack.pop()
            sig = data[off:off + 4]
            size = _u32.unpack_from(data, off + 4)[0]
            if sig == b'GRUP':
                label = data[off + 8:off + 12]
                gtype = struct.unpack_from('<i', data, off + 12)[0]
                lbl = label.decode('ascii', 'replace') if gtype == 0 else \
                    f'{GRUP_TYPES.get(gtype, gtype)}'
                if size < GRUP_HDR:
                    break
                stack.append((off + size, lbl))
                off += GRUP_HDR
                continue
            flags = _u32.unpack_from(data, off + 8)[0]
            fid = _u32.unpack_from(data, off + 12)[0]
            grp = stack[-1][1] if stack else ''
            rec = Record(sig.decode('ascii', 'replace'), fid, flags, off,
                         size, self, grp)
            if rec.sig == 'TES4':
                self.header = rec
            else:
                self.records.append(rec)
                self.by_fid[fid] = rec
                self.by_sig.setdefault(rec.sig, []).append(rec)
            off += REC_HDR + size
        if self.header is not None:
            for t, d in self.header.subs():
                if t == 'MAST':
                    self.masters.append(_zstr(d))

    def edid_of(self, fid):
        if fid in self._edid_cache:
            return self._edid_cache[fid]
        rec = self.by_fid.get(fid)
        val = rec.edid() if rec else None
        self._edid_cache[fid] = val
        return val

    def build_edid_index(self):
        """FULL index — inflates every record. Only built when a mode needs
        name->FormID (e.g. `--dump UseMagic`)."""
        idx = {}
        for r in self.records:
            e = r.edid()
            if e:
                idx.setdefault(e.lower(), r)
                self._edid_cache[r.fid] = e
        return idx


class Resolver:
    """Maps a FormID as stored in one plugin onto the plugin that owns it.

    High byte = index into [masters..., self]. A master's OWN records use
    high byte == len(that master's masters) — 0x00 for Skyrim.esm, which has
    none. That is the whole rule.
    """

    def __init__(self):
        self.loaded = {}

    def add(self, plug):
        self.loaded[plug.name.lower()] = plug

    def owner(self, fid, plug):
        idx = fid >> 24
        if idx < len(plug.masters):
            m = self.loaded.get(plug.masters[idx].lower())
            if m is None:
                return None, plug.masters[idx], fid & 0xFFFFFF
            local = (len(m.masters) << 24) | (fid & 0xFFFFFF)
            return m, m.name, local
        return plug, plug.name, fid

    def name(self, fid, plug):
        if fid == 0:
            return None
        if fid in HARDCODED_FIDS:
            return HARDCODED_FIDS[fid]
        owner, oname, local = self.owner(fid, plug)
        if owner is None:
            return f'<{oname}>'
        rec = owner.by_fid.get(local)
        if rec is None:
            return None
        e = owner.edid_of(local)
        tag = f'{rec.sig}'
        return f'{e} [{tag}]' if e else f'[{tag}]'


def fmt_fid(fid, plug, res):
    if fid == 0:
        return '00000000 (NONE)'
    nm = res.name(fid, plug) if res else None
    return f'{fid:08X}' + (f'  {nm}' if nm else '')


# ══ subrecord decoders ═════════════════════════════════════════════════════
# Keyed (record_sig, subrecord). '*' matches any record signature.

def d_pkdt(d, plug, res):
    """PKDT — always 12 bytes (all 5,961 vanilla PACKs).

        0..3   general flags    u32   clean bitfield
        4      type             u8    ONLY 18 or 19 in vanilla
        5      interrupt over   u8    only 0 (5747) or 4 (214)
        6      preferred speed  u8    only 0..3, plus one 255
        7      PADDING          u8    NOT a field — see below
        8..9   interrupt flags  u16   clean bitfield (clusters on bit combos)
        10..11 PADDING          u16   zero in 5878/5961, junk in the rest

    Offsets 7 and 10 are CK PADDING, not data. Byte 7 takes 152 distinct
    values with a flat tail and is non-zero in 62% of vanilla records, which
    is the signature of uninitialised memory rather than an enum. This is
    why MFO_GenerateESP's PKDT carries a mysterious `0x47`: it was copied
    verbatim from MG07AncanoCastAtEye (`00 00 00 00 12 00 02 47 00 00 00 00`)
    per the dump-and-mirror doctrine. Copying it was RIGHT and it is inert.
    """
    if len(d) < 12:
        return [f'len {len(d)} (expected 12)']
    fl, ty, io, spd, pad1, ifl, pad2 = struct.unpack_from('<IBBBBHH', d, 0)
    lines = [
        f'flags     0x{fl:08X}  {_flags(fl, PKDT_FLAGS, PKDT_FLAGS_CONFIRMED)}',
        f'type      {_enum(ty, PACK_TYPES, PACK_TYPES_CONFIRMED)}',
        f'interruptOverride {io}   preferredSpeed {spd}',
        f'interruptFlags 0x{ifl:04X}  '
        f'{_flags(ifl, PKDT_INTERRUPT_FLAGS, PKDT_INTERRUPT_CONFIRMED)}',
    ]
    if pad1 or pad2:
        lines.append(f'padding   @7=0x{pad1:02X} @10=0x{pad2:04X}  '
                     f'(CK junk, not a field — do not interpret)')
    return lines


def d_pkcu(d, plug, res):
    """PKCU — dataInputCount u32, template FormID u32, version u32."""
    if len(d) < 12:
        return [f'len {len(d)} (expected 12)']
    cnt, tmpl, ver = struct.unpack_from('<III', d, 0)
    lines = [f'inputs    {cnt}']
    if tmpl == 0:
        lines.append('template  00000000  (none — this record IS a template)')
    else:
        lines.append(f'template  {fmt_fid(tmpl, plug, res)}')
    lines.append(f'version   {ver}')
    return lines


def _alias_operand(val, label):
    if val == 0xFFFFFFFF:
        return f'{label}    ALIAS INDEX -1  (none)'
    return (f'{label}    ALIAS INDEX {val}  — indexes the PACK\'s QNAM quest; '
            f'QNAM is REQUIRED')


def d_ptda(d, plug, res):
    """PTDA — targType i32, union u32, count/distance i32."""
    if len(d) < 12:
        return [f'len {len(d)} (expected 12)']
    ty, val, cnt = struct.unpack_from('<iIi', d, 0)
    lines = [f'targType  {_enum(ty, PTDA_TYPES, PTDA_TYPES_CONFIRMED)}']
    if ty == PTDA_ALIAS_TYPE:
        lines.append(_alias_operand(val, 'target'))
    elif ty == 6:
        lines.append('target    self (no operand)')
    elif ty in PTDA_FORMID_TYPES:
        lines.append(f'target    {fmt_fid(val, plug, res)}')
    else:
        lines.append(f'target    raw 0x{val:08X} / {val}')
    lines.append(f'count/dist {cnt}')
    return lines


def d_pldt(d, plug, res):
    """PLDT — locType i32, union u32, radius i32."""
    if len(d) < 12:
        return [f'len {len(d)} (expected 12)']
    ty, val, rad = struct.unpack_from('<iIi', d, 0)
    lines = [f'locType   {_enum(ty, PLDT_TYPES, PLDT_TYPES_CONFIRMED)}']
    if ty == PLDT_ALIAS_TYPE:
        lines.append(_alias_operand(val, 'location'))
    elif ty in PLDT_FORMID_TYPES and val:
        lines.append(f'location  {fmt_fid(val, plug, res)}')
    elif val == 0:
        lines.append('location  none (this type takes no operand)')
    else:
        lines.append(f'location  raw 0x{val:08X} / {val}')
    lines.append(f'radius    {rad}')
    return lines


def d_qust_dnam(d, plug, res):
    """QUST DNAM (QUEST_DATA) — 12 bytes, decoded from all 1811 vanilla QUSTs.
        0..1 flags u16 / 2 priority u8 / 3 pad u8 / 4..7 delay f32 /
        8..11 type u32
    """
    if len(d) < 12:
        return [f'len {len(d)} (expected 12)']
    fl, pri, pad, delay, ty = struct.unpack_from('<HBBfI', d, 0)
    return [
        f'flags     0x{fl:04X}  {_flags(fl, QUST_FLAGS, QUST_FLAGS_CONFIRMED)}',
        f'priority  {pri}',
        f'pad       {pad}',
        f'delay     {delay}',
        f'type      {_enum(ty, QUST_TYPES)}',
    ]


def d_alias_fnam(d, plug, res):
    if len(d) < 4:
        return [f'len {len(d)}']
    fl = _u32.unpack_from(d, 0)[0]
    return [f'flags     0x{fl:08X}  {_flags(fl, ALIAS_FLAGS, ALIAS_FLAGS_CONFIRMED)}']


def d_alst(d, plug, res):
    return [f'alias id  {_u32.unpack_from(d, 0)[0]}  (REFERENCE alias)'] if len(d) >= 4 else []


def d_alls(d, plug, res):
    return [f'alias id  {_u32.unpack_from(d, 0)[0]}  (LOCATION alias)'] if len(d) >= 4 else []


def d_aled(d, plug, res):
    return ['-- end of alias --']


def d_pack_anam(d, plug, res):
    return [f'input type "{_zstr(d)}"']


def d_bnam(d, plug, res):
    return [f'input name "{_zstr(d)}"']


def d_unam(d, plug, res):
    if len(d) == 1:
        return [f'input index {d[0]}']
    return [f'input index {int.from_bytes(d, "little")}']


def d_pnam(d, plug, res):
    """PNAM is OVERLOADED in PACK and the two meanings are the same size.

      * after BNAM  -> u32 input flags
      * after ANAM "Procedure" -> the procedure NAME as a zstring

    UseMagic's procedure PNAM is b'UseMagic\\x00', which read as u32 is a
    convincing-looking 0x4D657355. Disambiguate on the bytes, not on hope.
    """
    if d and d[-1] == 0 and all(32 <= c < 127 for c in d[:-1]) and len(d) > 1:
        return [f'procedure name "{_zstr(d)}"']
    if len(d) >= 4:
        return [f'input flags 0x{_u32.unpack_from(d, 0)[0]:08X}']
    return []


def d_pack_cnam(d, plug, res):
    """CNAM carries the VALUE of the preceding ANAM-typed input. Its meaning
    depends on that type, so print every plausible reading."""
    if len(d) == 1:
        return [f'u8 {d[0]}   (Bool -> {"true" if d[0] else "false"})']
    if len(d) == 4:
        i = struct.unpack_from('<i', d, 0)[0]
        f = struct.unpack_from('<f', d, 0)[0]
        return [f'int {i}   float {f:g}   (read per the preceding ANAM type)']
    return []


def d_spit(d, plug, res):
    if len(d) < 36:
        return [f'len {len(d)} (expected 36)']
    cost, fl, ty, chg, cast, deliv, dur, rng, perk = struct.unpack_from('<fIIfIIffI', d, 0)
    return [
        f'cost {cost:g}  flags 0x{fl:08X}',
        f'type      {_enum(ty, SPEL_TYPES)}',
        f'chargeTime {chg:g}  castType {cast}  delivery {deliv}',
        f'castDuration {dur:g}  range {rng:g}  castingPerk {perk:08X}',
    ]


def d_hedr(d, plug, res):
    if len(d) < 12:
        return []
    ver, num, nxt = struct.unpack_from('<fII', d, 0)
    return [f'version {ver}  records {num}  NEXT_OBJECT_ID 0x{nxt:X}']


def d_zstring(d, plug, res):
    return [f'"{_zstr(d)}"']


def d_formid(d, plug, res):
    if len(d) < 4:
        return []
    return [fmt_fid(_u32.unpack_from(d, 0)[0], plug, res)]


DECODERS = {
    ('PACK', 'PKDT'): d_pkdt,
    ('PACK', 'PKCU'): d_pkcu,
    ('PACK', 'PTDA'): d_ptda,
    ('PACK', 'PLDT'): d_pldt,
    ('PACK', 'ANAM'): d_pack_anam,
    ('PACK', 'BNAM'): d_bnam,
    ('PACK', 'UNAM'): d_unam,
    ('PACK', 'XNAM'): d_unam,
    ('PACK', 'PNAM'): d_pnam,
    ('PACK', 'CNAM'): d_pack_cnam,
    ('QUST', 'DNAM'): d_qust_dnam,
    ('QUST', 'FNAM'): d_alias_fnam,
    ('QUST', 'ALST'): d_alst,
    ('QUST', 'ALLS'): d_alls,
    ('QUST', 'ALID'): d_zstring,
    ('QUST', 'ALED'): d_aled,
    ('SPEL', 'SPIT'): d_spit,
    ('TES4', 'HEDR'): d_hedr,
    ('*', 'EDID'): d_zstring,
    ('*', 'FULL'): d_zstring,
    ('*', 'DESC'): d_zstring,
    ('*', 'MAST'): d_zstring,
    ('*', 'CNAM'): d_zstring,
    ('*', 'SNAM'): d_zstring,
}

# Subrecords small enough that hex adds nothing once decoded.
QUIET_HEX = {'EDID', 'FULL', 'DESC', 'ALID', 'MAST', 'ALED'}


def decode(sig, sub, data, plug, res):
    fn = DECODERS.get((sig, sub))
    if fn is None and (sig, sub) in FORMID_SUBS:
        fn = d_formid
    if fn is None and (sig, sub) not in DECODERS:
        fn = DECODERS.get(('*', sub))
    if fn is None:
        return None
    try:
        return fn(data, plug, res)
    except Exception as e:                                  # noqa: BLE001
        return [f'<decoder error: {e}>']


# ══ structured views ═══════════════════════════════════════════════════════

# ANAM values that introduce the procedure tree rather than a data input.
STRUCTURAL_ANAM = {'Sequence', 'Procedure'}
VALUE_SUBS = ('CNAM', 'PTDA', 'PLDT', 'TPIC', 'PDTO', 'PFO2', 'BNAM')


def pack_inputs(rec):
    """Split a PACK's subrecord stream into declared inputs and input VALUES.

    Returns (declared, values, slots, procedure).

    A TEMPLATE declares its public inputs as UNAM(index)+BNAM(name)+
    PNAM(flags). BOTH templates and instances then carry a run of BARE UNAMs
    naming the declared indices that are settable, and supply values as
    ANAM(type)+<value subrecord>.

    THE MAPPING IS NOT THE IDENTITY, and this is the thing raw hex hides.
    UseMagic declares 13 inputs (0..12) but `PKCU.inputs` is 11 and the bare
    UNAM run is [2..12]. So supplied value k belongs to declared slot
    slots[k] — for UseMagic, value 1 is slot 3 "Spell" and value 2 is slot 4
    "Target". Reading the value list as if it started at slot 0 would put the
    spell in "Destination", which is exactly the class of silent breakage
    this tool exists to prevent.
    """
    declared, values, slots = [], [], []
    procedure = None
    subs = rec.subs()
    i, pending = 0, None
    while i < len(subs):
        t, d = subs[i]
        if t == 'UNAM':
            idx = d[0] if len(d) == 1 else int.from_bytes(d, 'little')
            j = i + 1
            if j < len(subs) and subs[j][0] == 'BNAM':
                nm = _zstr(subs[j][1])
                fl = None
                j += 1
                if j < len(subs) and subs[j][0] == 'PNAM':
                    fl = _u32.unpack_from(subs[j][1], 0)[0]
                    j += 1
                declared.append((idx, nm, fl))
                i = j
                continue
            slots.append(idx)
        elif t == 'ANAM':
            nm = _zstr(d)
            if nm in STRUCTURAL_ANAM:
                pending = None
                # ANAM "Procedure" [+ CITC] + PNAM carries the procedure name.
                for k in range(i + 1, min(i + 4, len(subs))):
                    if subs[k][0] == 'PNAM':
                        procedure = _zstr(subs[k][1])
                        break
                    if subs[k][0] == 'ANAM':
                        break
            else:
                pending = nm
        elif pending is not None and t in VALUE_SUBS:
            values.append((pending, t, d))
            pending = None
        i += 1
    if pending is not None:
        values.append((pending, None, b''))
    return declared, values, slots, procedure


def alias_blocks(rec):
    """Group a QUST's subrecord stream into ALST/ALLS .. ALED alias blocks."""
    blocks, cur = [], None
    for t, d in rec.subs():
        if t in ('ALST', 'ALLS'):
            if cur is not None:
                blocks.append(cur)
            cur = {'kind': 'reference' if t == 'ALST' else 'location',
                   'id': _u32.unpack_from(d, 0)[0] if len(d) >= 4 else None,
                   'subs': []}
            continue
        if cur is None:
            continue
        if t == 'ALED':
            blocks.append(cur)
            cur = None
            continue
        cur['subs'].append((t, d))
    if cur is not None:
        blocks.append(cur)
    return blocks


# ══ modes ══════════════════════════════════════════════════════════════════

def mode_summary(plug, res):
    print(f'{plug.path}')
    print(f'  {len(plug.data):,} bytes, {len(plug.records):,} records')
    if plug.header:
        fl = _u32.unpack_from(plug.data, plug.header.off + 8)[0]
        print(f'  TES4 flags 0x{fl:08X}' + ('  [ESL]' if fl & FLAG_ESL else ''))
        for t, d in plug.header.subs():
            if t in ('HEDR', 'CNAM', 'SNAM'):
                for ln in decode('TES4', t, d, plug, res) or []:
                    print(f'  {t}  {ln}')
    print(f'  masters ({len(plug.masters)}): '
          f'{", ".join(plug.masters) if plug.masters else "none"}')
    print()
    print('  record counts by type:')
    for sig in sorted(plug.by_sig, key=lambda s: (-len(plug.by_sig[s]), s)):
        print(f'    {sig}  {len(plug.by_sig[sig]):>7,}')


def mode_list(plug, res, sig, filt, limit):
    recs = plug.by_sig.get(sig.upper(), [])
    print(f'{sig.upper()}: {len(recs)} record(s) in {plug.name}')
    shown = 0
    for r in recs:
        e = r.edid()
        if filt and filt.lower() not in e.lower():
            continue
        extra = ''
        if r.sig == 'PACK':
            ty = pack_type_of(r)
            extra = f'  type={ty:<3} {PACK_TYPES.get(ty, "?")}'
        flagbits = []
        if r.compressed:
            flagbits.append('zlib')
        if r.flags & FLAG_DELETED:
            flagbits.append('DELETED')
        if r.flags & FLAG_IGNORED:
            flagbits.append('IGNORED')
        fs = f'  [{",".join(flagbits)}]' if flagbits else ''
        print(f'  {r.fid:08X}  {e or "<no EDID>":<44}{extra}{fs}')
        shown += 1
        if limit and shown >= limit:
            print(f'  ... (--limit {limit} reached)')
            break
    if filt:
        print(f'  {shown} shown (filter "{filt}")')


def pack_type_of(rec):
    for t, d in rec.subs():
        if t == 'PKDT' and len(d) >= 5:
            return d[4]
    return -1


def pack_template_of(rec):
    for t, d in rec.subs():
        if t == 'PKCU' and len(d) >= 8:
            return _u32.unpack_from(d, 4)[0]
    return 0


def mode_dump(plug, res, rec, raw):
    print(f'{rec.sig}  {rec.fid:08X}  {rec.edid() or "<no EDID>"}')
    print(f'  in {plug.name}   group "{rec.group}"   '
          f'offset 0x{rec.off:X}   body {rec.size} bytes'
          + ('  [zlib]' if rec.compressed else ''))
    print(f'  record flags 0x{rec.flags:08X}'
          + ('  DELETED' if rec.flags & FLAG_DELETED else '')
          + ('  IGNORED' if rec.flags & FLAG_IGNORED else ''))
    subs = rec.subs()
    print(f'  {len(subs)} subrecord(s)')
    print()
    for n, (t, d) in enumerate(subs):
        print(f'  [{n:>3}] {t}  ({len(d)} bytes)')
        dec = decode(rec.sig, t, d, plug, res)
        if dec:
            for ln in dec:
                print(f'        {ln}')
        if d and (raw or not dec or t not in QUIET_HEX):
            for ln in hexdump(d, limit=None if raw else 64):
                print(ln)
    if rec.sig == 'PACK':
        print()
        dump_pack_view(rec, plug, res)
    if rec.sig == 'QUST':
        print()
        dump_quest_view(rec, plug, res)


def uses_alias(values):
    """True if any supplied value points at a reference alias — the condition
    that makes QNAM mandatory."""
    for _, vsub, vd in values:
        if len(vd) < 4:
            continue
        ty = struct.unpack_from('<i', vd, 0)[0]
        if vsub == 'PTDA' and ty == PTDA_ALIAS_TYPE:
            return True
        if vsub == 'PLDT' and ty == PLDT_ALIAS_TYPE:
            return True
    return False


def dump_pack_view(rec, plug, res):
    ty = pack_type_of(rec)
    tmpl = pack_template_of(rec)
    print('  ── PACKAGE VIEW ' + '─' * 56)
    print(f'  type      {_enum(ty, PACK_TYPES, PACK_TYPES_CONFIRMED)}')
    tdecl = []
    if tmpl:
        print(f'  template  {fmt_fid(tmpl, plug, res)}')
        owner, _, local = res.owner(tmpl, plug) if res else (None, None, tmpl)
        trec = owner.by_fid.get(local) if owner else None
        if trec is not None:
            tdecl = pack_inputs(trec)[0]
        else:
            print('            (template not loaded — pass --master to name '
                  'its input slots)')
    else:
        print('  template  none — THIS RECORD IS A TEMPLATE')
    qnam = None
    for t, d in rec.subs():
        if t == 'QNAM' and len(d) >= 4:
            qnam = _u32.unpack_from(d, 0)[0]
    print(f'  QNAM      {fmt_fid(qnam, plug, res) if qnam else "ABSENT"}')
    declared, values, slots, proc = pack_inputs(rec)
    if proc:
        print(f'  procedure "{proc}"')
    if declared:
        print(f'\n  declared public inputs ({len(declared)}):')
        for idx, nm, fl in declared:
            print(f'    {idx:>3}  {nm:<24} flags 0x{(fl or 0):08X}')
    # Slot names come from the TEMPLATE's declarations when this is an
    # instance, and from the record's own when it is a template.
    names = {i: n for i, n, _ in (tdecl or declared)}
    if values:
        print(f'\n  supplied input values ({len(values)}):')
        print(f'    val  slot  name                 type')
        for k, (vtype, vsub, vd) in enumerate(values):
            slot = slots[k] if k < len(slots) else None
            nm = names.get(slot, '') if slot is not None else ''
            det = decode('PACK', vsub, vd, plug, res) if vsub else None
            first = det[0] if det else vd.hex().upper()
            print(f'    {k:>3}  {"-" if slot is None else slot:>4}  '
                  f'{nm:<20} {vtype:<15} {vsub or "-":<5} {first}')
            for ln in (det or [])[1:]:
                print(f'{"":>38}{"":<21}{ln}')
    if slots:
        print(f'\n  settable slots (bare UNAM run): {slots}')
        if len(values) not in (len(slots), len(slots) + 1):
            print(f'  NOTE: {len(values)} values vs {len(slots)} slots — the '
                  f'value/slot mapping above may be misaligned.')
    if qnam is None and uses_alias(values):
        print('\n  *** WARNING: an input targets a reference ALIAS but this '
              'record has NO QNAM.')
        print('      The alias value is an INDEX; QNAM names the quest it '
              'indexes into. All 626 vanilla')
        print('      packages that reference an alias carry QNAM.')


def dump_quest_view(rec, plug, res):
    print('  ── QUEST VIEW ' + '─' * 58)
    blocks = alias_blocks(rec)
    if not blocks:
        print('  no aliases')
        return
    for b in blocks:
        name = pkgs = fill = None
        flags = 0
        extra = []
        for t, d in b['subs']:
            if t == 'ALID':
                name = _zstr(d)
            elif t == 'FNAM' and len(d) >= 4:
                flags = _u32.unpack_from(d, 0)[0]
            elif t == 'ALFR' and len(d) >= 4:
                fill = ('ALFR specific reference',
                        _u32.unpack_from(d, 0)[0])
            elif t == 'ALPC' and len(d) >= 4:
                pkgs = (pkgs or []) + [_u32.unpack_from(d, 0)[0]]
            elif t in ('ALCO', 'ALDN', 'ALSP', 'ALFA', 'VTCK', 'ALUA'):
                if len(d) >= 4:
                    extra.append((t, _u32.unpack_from(d, 0)[0]))
        print(f'  alias {b["id"]}  "{name or ""}"  ({b["kind"]})')
        print(f'    FNAM  0x{flags:08X}  {_flags(flags, ALIAS_FLAGS, ALIAS_FLAGS_CONFIRMED)}')
        if fill:
            print(f'    fill  {fill[0]} -> {fmt_fid(fill[1], plug, res)}')
        else:
            print('    fill  none authored (filled at runtime, or unfilled)')
        for p in pkgs or []:
            print(f'    ALPC  package {fmt_fid(p, plug, res)}')
        for t, v in extra:
            if v:
                print(f'    {t}   {fmt_fid(v, plug, res)}')


def mode_templates(plug, res, filt, limit):
    packs = plug.by_sig.get('PACK', [])
    tmpls = [r for r in packs if pack_type_of(r) == 19]
    print(f'PACK templates (type 19) in {plug.name}: {len(tmpls)} of '
          f'{len(packs)} PACK records')
    print()
    shown = 0
    for r in tmpls:
        e = r.edid()
        if filt and filt.lower() not in e.lower():
            continue
        declared, values, slots, proc = pack_inputs(r)
        print(f'  {r.fid:08X}  {e}   ({len(declared)} public input(s), '
              f'{len(slots)} settable'
              + (f', procedure "{proc}"' if proc else '') + ')')
        for idx, nm, fl in declared:
            mark = '*' if idx in slots else ' '
            print(f'    {mark} {idx:>3}  {nm:<26} flags 0x{(fl or 0):08X}')
        shown += 1
        if limit and shown >= limit:
            print(f'  ... (--limit {limit} reached)')
            break
    if filt:
        print(f'\n  {shown} shown (filter "{filt}")')


def mode_find_template(plug, res, target, limit):
    print(f'PACK instances whose PKCU.template == {fmt_fid(target, plug, res)}'
          f'  (searching {plug.name})')
    print()
    hits = 0
    for r in plug.by_sig.get('PACK', []):
        t = pack_template_of(r)
        if t != target:
            continue
        _, values, slots, _ = pack_inputs(r)
        qn = None
        for st, sd in r.subs():
            if st == 'QNAM' and len(sd) >= 4:
                qn = _u32.unpack_from(sd, 0)[0]
        alias = uses_alias(values)
        print(f'  {r.fid:08X}  {r.edid() or "<no EDID>":<40} '
              f'type={pack_type_of(r)}  {len(values):>2} value(s)'
              + ('  ALIAS' if alias else '       ')
              + (f'  QNAM={qn:08X}' if qn else '  QNAM=-')
              + ('   <-- ALIAS TARGET WITHOUT QNAM' if alias and not qn else ''))
        hits += 1
        if limit and hits >= limit:
            print(f'  ... (--limit {limit} reached)')
            break
    print(f'\n  {hits} instance(s)')


# ══ selftest ═══════════════════════════════════════════════════════════════

SKYRIM_DEFAULT = '/mnt/gaming/modlists/custom-modlist/Stock Game/Data/Skyrim.esm'
UNKNOWN = object()


def mode_selftest(args):
    """Assert facts this project has already established in the field.

    Each assertion is something a regression in the parser would break, and
    every value here is one the docs or the generator already pin.
    """
    checks, fails = [], []

    def ck(name, got, want):
        ok = got == want
        checks.append((ok, name, got, want))
        if not ok:
            fails.append(name)

    sky_path = args.master[0] if args.master else SKYRIM_DEFAULT
    if not os.path.exists(sky_path):
        print(f'SKIP: {sky_path} not found')
        return 1
    sky = Plugin(sky_path)
    res = Resolver()
    res.add(sky)

    ck('Skyrim.esm has 0 masters', len(sky.masters), 0)

    usemagic = sky.by_fid.get(0x000504F5)
    ck('UseMagic 000504F5 exists', usemagic is not None, True)
    if usemagic:
        ck('UseMagic is a PACK', usemagic.sig, 'PACK')
        ck('UseMagic EDID', usemagic.edid(), 'UseMagic')
        ck('UseMagic PKDT type == 19 (template)', pack_type_of(usemagic), 19)
        ck('UseMagic PKCU.template == 0 (is a template)',
           pack_template_of(usemagic), 0)
        decl, vals, slots, proc = pack_inputs(usemagic)
        byidx = {i: n for i, n, _ in decl}
        ck('UseMagic declares 13 public inputs', len(decl), 13)
        ck('UseMagic procedure is "UseMagic"', proc, 'UseMagic')
        ck('UseMagic slot 3 is "Spell"', byidx.get(3), 'Spell')
        ck('UseMagic slot 4 is "Target"', byidx.get(4), 'Target')
        ck('UseMagic slot 6 is "CastTimeMin"', byidx.get(6), 'CastTimeMin')
        # The mapping that raw hex hides: 13 declared, 11 settable, and the
        # settable run starts at 2 -- so value 1 is Spell, value 2 is Target.
        ck('UseMagic settable slots are 2..12', slots, list(range(2, 13)))
        ck('UseMagic PKCU.inputs == len(settable slots)',
           len(slots), 11)
        ck('UseMagic value 1 maps to slot "Spell"',
           byidx.get(slots[1]) if len(slots) > 1 else None, 'Spell')
        ck('UseMagic value 2 maps to slot "Target"',
           byidx.get(slots[2]) if len(slots) > 2 else None, 'Target')

    # The other templates M9 rides (ENGINE_NOTES 0.17 table).
    for fid, edid in ((0x0001C338, 'UseWeapon'), (0x000503D0, 'HoldPosition'),
                      (0x00016FAA, 'Travel'), (0x00019B2D, 'Activate')):
        r = sky.by_fid.get(fid)
        ck(f'{edid} {fid:08X} is a type-19 template',
           (r.sig, r.edid(), pack_type_of(r)) if r else None,
           ('PACK', edid, 19))

    # zlib path: Skyrim.esm compresses records, and a wrong inflate is silent.
    ncomp = sum(1 for r in sky.records if r.compressed)
    ck('Skyrim.esm contains compressed records', ncomp > 0, True)
    ck('CosnachREF 000198FA is an ACHR',
       sky.by_fid[0x000198FA].sig if 0x000198FA in sky.by_fid else None, 'ACHR')
    ck('HealSelf 00012FCC is a SPEL',
       sky.by_fid[0x00012FCC].sig if 0x00012FCC in sky.by_fid else None, 'SPEL')

    # ── MFO.esp ──
    mfo_path = args.plugin
    if mfo_path and os.path.exists(mfo_path) and mfo_path != sky_path:
        mfo = Plugin(mfo_path)
        res.add(mfo)
        ck('MFO masters == [Skyrim.esm]', mfo.masters, ['Skyrim.esm'])
        pk = [r for r in mfo.by_sig.get('PACK', [])]
        ck('MFO has exactly 1 PACK', len(pk), 1)
        if pk:
            p = pk[0]
            names = [t for t, _ in p.subs()]
            ck('MFO PACK carries QNAM', 'QNAM' in names, True)
            ck('MFO PACK type == 18 (instance)', pack_type_of(p), 18)
            ck('MFO PACK rides UseMagic 000504F5',
               f'{pack_template_of(p):08X}', '000504F5')
            qn = [d for t, d in p.subs() if t == 'QNAM'][0]
            ck('MFO PACK QNAM -> MFO_CommandQuest 0100080A',
               f'{_u32.unpack_from(qn, 0)[0]:08X}', '0100080A')
            # The alias target that MAKES QNAM mandatory.
            _, vals, slots, _ = pack_inputs(p)
            alias_t = [v for v in vals if v[1] == 'PTDA'
                       and struct.unpack_from('<i', v[2], 0)[0] == PTDA_ALIAS_TYPE]
            ck('MFO PACK targets a reference ALIAS (PTDA type 4)',
               len(alias_t), 1)
            ck('MFO PACK supplies 11 input values', len(vals), 11)
            ck('MFO PACK settable slots match UseMagic (2..12)',
               slots, list(range(2, 13)))
            # The two slots MFO substitutes, checked by NAME through the
            # template rather than by position.
            um = sky.by_fid.get(0x000504F5)
            byidx = {i: n for i, n, _ in pack_inputs(um)[0]} if um else {}
            spell_val = vals[1] if len(vals) > 1 else None
            targ_val = vals[2] if len(vals) > 2 else None
            ck('MFO value 1 lands on template slot "Spell"',
               byidx.get(slots[1]) if len(slots) > 1 else None, 'Spell')
            ck('MFO "Spell" slot holds PTDA type 1 -> HealSelf 00012FCC',
               (spell_val[1], struct.unpack_from('<iI', spell_val[2], 0))
               if spell_val else None, ('PTDA', (1, 0x00012FCC)))
            ck('MFO value 2 lands on template slot "Target"',
               byidx.get(slots[2]) if len(slots) > 2 else None, 'Target')
            ck('MFO "Target" slot holds PTDA type 4 -> alias 0',
               (targ_val[1], struct.unpack_from('<iI', targ_val[2], 0))
               if targ_val else None, ('PTDA', (4, 0)))
        q = mfo.by_fid.get(0x0100080A)
        ck('MFO_CommandQuest 0100080A is a QUST', q.sig if q else None, 'QUST')
        if q:
            blocks = alias_blocks(q)
            ck('MFO_CommandQuest has 2 aliases', len(blocks), 2)
            if blocks:
                sub = dict(blocks[0]['subs'])
                ck('alias 0 is MFO_CommandActor',
                   _zstr(sub.get('ALID', b'')), 'MFO_CommandActor')
                ck('alias 0 carries ALPC -> the cast package',
                   f'{_u32.unpack_from(sub["ALPC"], 0)[0]:08X}'
                   if 'ALPC' in sub else None, '01000820')
                fl = _u32.unpack_from(sub['FNAM'], 0)[0] if 'FNAM' in sub else 0
                ck('alias 0 FNAM = Optional|AllowReuse|AllowReserved',
                   fl, 0x0002 | 0x0008 | 0x0200)
            dn = [d for t, d in q.subs() if t == 'DNAM']
            if dn:
                f2, pri = struct.unpack_from('<HB', dn[0], 0)
                ck('MFO_CommandQuest DNAM flags', f'0x{f2:04X}', '0x0011')
                ck('MFO_CommandQuest priority == 60', pri, 60)

    width = max(len(c[1]) for c in checks)
    for ok, name, got, want in checks:
        mark = 'ok  ' if ok else 'FAIL'
        line = f'  {mark}  {name:<{width}}'
        print(line if ok else f'{line}   got {got!r}, want {want!r}')
    print()
    if fails:
        print(f'SELFTEST FAIL — {len(fails)} of {len(checks)}: '
              + '; '.join(fails))
        return 1
    # A PASS that silently skipped half its assertions is worse than a FAIL:
    # it reports a state nobody checked. Without a plugin the MFO-side
    # assertions never run, so say so and exit nonzero.
    if not args.plugin:
        print(f'SELFTEST INCOMPLETE — {len(checks)} vanilla assertions passed, '
              'but every MFO-side check was SKIPPED (no plugin given).')
        print('Run:  tools/esp_inspect.py out/MFO.esp --selftest')
        return 1
    print(f'SELFTEST PASS — {len(checks)} assertions')
    return 0


# ══ cli ════════════════════════════════════════════════════════════════════

def parse_fid(s):
    s = s.strip().lower().replace('0x', '')
    if re.fullmatch(r'[0-9a-f]{1,8}', s):
        return int(s, 16)
    return None


def main():
    ap = argparse.ArgumentParser(
        description='Inspect a Skyrim plugin (.esp/.esm/.esl).',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split('EXAMPLES')[-1])
    ap.add_argument('plugin', nargs='?', help='plugin to inspect')
    ap.add_argument('--list', metavar='TYPE', help='list records of TYPE')
    ap.add_argument('--dump', metavar='FORMID|EDID', help='full subrecord dump')
    ap.add_argument('--templates', action='store_true',
                    help='list PACK type-19 templates + their public inputs')
    ap.add_argument('--find-template', metavar='FORMID',
                    help='PACK instances whose PKCU points at this template')
    ap.add_argument('--master', action='append', default=[], metavar='PATH',
                    help='master to resolve FormIDs against (repeatable)')
    ap.add_argument('--filter', metavar='SUBSTR', help='EDID substring filter')
    ap.add_argument('--limit', type=int, default=200, help='row cap (0 = all)')
    ap.add_argument('--raw', action='store_true', help='always show full hex')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()

    if args.selftest:
        return mode_selftest(args)
    if not args.plugin:
        ap.error('a plugin path is required')
    if not os.path.exists(args.plugin):
        print(f'not found: {args.plugin}', file=sys.stderr)
        return 2

    plug = Plugin(args.plugin)
    res = Resolver()
    res.add(plug)
    for m in args.master:
        if not os.path.exists(m):
            print(f'master not found: {m}', file=sys.stderr)
            return 2
        res.add(Plugin(m))

    limit = args.limit if args.limit > 0 else 0

    if args.list:
        mode_list(plug, res, args.list, args.filter, limit)
    elif args.dump:
        fid = parse_fid(args.dump)
        rec = plug.by_fid.get(fid) if fid is not None else None
        if rec is None:
            # Fall back to a name lookup, and to the masters, before failing.
            if fid is not None:
                for p in res.loaded.values():
                    if fid in p.by_fid:
                        rec, plug = p.by_fid[fid], p
                        break
            if rec is None:
                print(f'  (no FormID match — scanning EDIDs of {plug.name})',
                      file=sys.stderr)
                idx = plug.build_edid_index()
                rec = idx.get(args.dump.lower())
            if rec is None:
                print(f'not found: {args.dump}', file=sys.stderr)
                return 2
        mode_dump(rec.plugin, res, rec, args.raw)
    elif args.templates:
        mode_templates(plug, res, args.filter, limit)
    elif args.find_template:
        t = parse_fid(args.find_template)
        if t is None:
            print(f'not a FormID: {args.find_template}', file=sys.stderr)
            return 2
        mode_find_template(plug, res, t, limit)
    else:
        mode_summary(plug, res)
    return 0


if __name__ == '__main__':
    sys.exit(main())
