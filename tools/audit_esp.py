#!/usr/bin/env python3
"""MFO plugin audit — structural + FormID validation. PASS is a merge gate.

A FAIL here is guaranteed runtime breakage. The engine drops malformed
records SILENTLY, so this parses the emitted plugin back and checks it
against what the DLL will look for.

PROFILES (selected by the file's basename):
  MFO.esp             — the main plugin (default)
  MFO_Progression.esl — the optional progression addon (component 2):
                        economy GLOBs + class FormLists; no quests, no SEQ

Checks:
  1. TES4 parses; ESL flag set; exactly one master (Skyrim.esm)
  2. Every own record uses the own-file master index prefix (0x01)
  3. Every own local id is inside the ESL-legal range 0x800-0xFFF
  4. NEXT_OBJECT_ID is above every id actually emitted
  5. No duplicate FormIDs
  6. Every FormID the DLL hardcodes is actually present
  7. Records the DLL needs carry their required subrecords
  8. SEQ lists the start-game-enabled non-run-once quests (MFO.esp only)

Usage:  python3 tools/audit_esp.py [out/MFO.esp] [out/MFO_Progression.esl ...]
        (no args = audit both emitted plugins)
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

OWN_PREFIX = 0x01
ESL_LO, ESL_HI = 0x800, 0xFFF

# Expected master list per plugin (ORDER = the master index each own/cross ref
# assumes). §18.6: MFO_Progression.esl masters MFO.esp so its manifest FLST can
# point at MFO.esp's addon sentinel keyword — so its own records move to master
# index 0x02. The own-file prefix each plugin must use = len(its masters).
EXPECTED_MASTERS = {
    "MFO.esp":              ["Skyrim.esm"],
    "MFO_Progression.esl":  ["Skyrim.esm", "MFO.esp"],
}

# What native/ hardcodes. Keep in lockstep with MFO_GenerateESP.py AND with the
# DLL's lookups -- a mismatch here is the "form not found" class of bug whose
# only symptom is a feature silently missing.
REQUIRED = {
    0x800: ('MGEF', "MFO_FieldOrdersMGEF",  ['EDID', 'DATA']),
    0x801: ('SPEL', "MFO_FieldOrdersPower", ['EDID', 'OBND', 'ETYP', 'DESC', 'SPIT', 'EFID', 'EFIT']),
    0x802: ('KYWD', "MFO_GrantedSpell",     ['EDID']),
    0x804: ('QUST', "MFO_StartupQuest",     ['EDID', 'DNAM']),
    0x808: ('QUST', "MFO_MCMQuest",         ['EDID', 'DNAM', 'VMAD']),
    # Option A (walk-to-loot): the DLL hard-depends on these. Left unchecked, a
    # dropped alias/PLDT would silently break travel (audit finding).
    0x80A: ('QUST', "MFO_CommandQuest",     ['EDID', 'DNAM', 'ALST', 'ALPC']),
    0x80C: ('QUST', "MFO_LootQuest",        ['EDID', 'DNAM', 'ALST', 'ALPC']),
    0x820: ('PACK', "MFO_CastPackage",      ['EDID', 'PKDT', 'PKCU']),
    0x828: ('PACK', "MFO_TravelPackage",    ['EDID', 'PKDT', 'PKCU', 'PLDT']),
    # RETREAT PROBE: travel-to-player under kIgnoreCombat.
    0x830: ('QUST', "MFO_RetreatQuest",     ['EDID', 'DNAM', 'ALST', 'ALPC']),
    0x831: ('PACK', "MFO_RetreatPackage",   ['EDID', 'PKDT', 'PKCU', 'PLDT']),
    # #21 econ bridge: carries MFO_Trade (VMAD), no aliases (script pulls from natives).
    0x80E: ('QUST', "MFO_TradeQuest",       ['EDID', 'DNAM', 'VMAD']),
    # P1 probe: the caster-forward style the DLL swaps onto a latched follower's
    # live CombatController (bProbeCastStyle). CSGD is the record's whole point.
    0x832: ('CSTY', "MFO_CastStyle",        ['EDID', 'CSGD']),
    # Stance-ownership styles (bWeaponStyleControl, default ON): swapped onto a
    # follower's live CombatController when an equip gambit wins the hand. CSGD
    # (the weapon-scoring axis) is each record's whole point.
    0x833: ('CSTY', "MFO_MeleeStyle",       ['EDID', 'CSGD']),
    0x834: ('CSTY', "MFO_RangedStyle",      ['EDID', 'CSGD']),
}

# Quests that are start-game-enabled but NOT run-once must appear in the SEQ or
# they never start on an existing save.
SEQ_EXPECTED = {0x808, 0x80A, 0x80C, 0x830, 0x80E}

# MFO_Progression.esl — what native/Progression.h (0x800/0x801) and
# native/ProgAllocator.h (the rest) hardcode. Keep in lockstep with the
# generator's PROG_* tables AND the DLL's lookups.
PROG_REQUIRED = {
    0x800: ('GLOB', "MFOP_Version",             ['EDID', 'FNAM', 'FLTV']),
    0x801: ('GLOB', "MFOP_Reserved",            ['EDID', 'FNAM', 'FLTV']),
    # §18.6 Stage 3: 0x802 repurposed (was MFOP_PerkPointsPerLevel) → the perk
    # divisor; 0x809 added (manual rate). The DLL now finds every economy GLOB
    # by editor-id SUFFIX off the manifest, not by these ids — but the ids stay
    # frozen own-form ids and the editor id is the suffix contract, so keep them
    # in lockstep with PROG_GLOBS.
    0x802: ('GLOB', "MFOP_LevelsPerPerkPoint",  ['EDID', 'FNAM', 'FLTV']),
    0x803: ('GLOB', "MFOP_SkillPointsPerLevel", ['EDID', 'FNAM', 'FLTV']),
    0x804: ('GLOB', "MFOP_SharedGrowthDivisor", ['EDID', 'FNAM', 'FLTV']),
    0x805: ('GLOB', "MFOP_RespecRapportCost",   ['EDID', 'FNAM', 'FLTV']),
    0x806: ('GLOB', "MFOP_VeteranCatchupMult",  ['EDID', 'FNAM', 'FLTV']),
    0x807: ('GLOB', "MFOP_SkillCap",            ['EDID', 'FNAM', 'FLTV']),
    0x808: ('GLOB', "MFOP_DevCmd",              ['EDID', 'FNAM', 'FLTV']),
    0x809: ('GLOB', "MFOP_ManualSkillPointsPerLevel", ['EDID', 'FNAM', 'FLTV']),
    # Class skill lists carry entries (LNAM); the perk lists ship EMPTY on
    # purpose (xEdit extension point) so LNAM is not required there.
    0x810: ('FLST', "MFOP_ClassSkills_Melee",   ['EDID', 'LNAM']),
    0x811: ('FLST', "MFOP_ClassSkills_Ranged",  ['EDID', 'LNAM']),
    0x812: ('FLST', "MFOP_ClassSkills_Mage",    ['EDID', 'LNAM']),
    0x818: ('FLST', "MFOP_ClassPerks_Melee",    ['EDID']),
    0x819: ('FLST', "MFOP_ClassPerks_Ranged",   ['EDID']),
    0x81A: ('FLST', "MFOP_ClassPerks_Mage",     ['EDID']),
    0x820: ('KYWD', "MFOP_Enrolled",            ['EDID']),
    # §18.6 the addon MANIFEST the DLL enumerates (entry[0]=MFO.esp sentinel,
    # entry[1]=the classes-list FLST, entries[2..]=the economy GLOBs — Stage 3).
    0x821: ('FLST', "MFOP_AddonManifest",       ['EDID', 'LNAM']),
    # §18.6 Stage 2 — N-declared classes: MESG display names, _Stance mirrors,
    # class-def FLSTs, and the classes-list FLST the manifest points at.
    0x830: ('MESG', "MFOP_ClassName_Melee",     ['EDID', 'FULL']),
    0x831: ('MESG', "MFOP_ClassName_Ranged",    ['EDID', 'FULL']),
    0x832: ('MESG', "MFOP_ClassName_Mage",      ['EDID', 'FULL']),
    0x840: ('GLOB', "MFOP_ClassMelee_Stance",   ['EDID', 'FNAM', 'FLTV']),
    0x841: ('GLOB', "MFOP_ClassRanged_Stance",  ['EDID', 'FNAM', 'FLTV']),
    0x842: ('GLOB', "MFOP_ClassMage_Stance",    ['EDID', 'FNAM', 'FLTV']),
    0x850: ('FLST', "MFOP_ClassDef_Melee",      ['EDID', 'LNAM']),
    0x851: ('FLST', "MFOP_ClassDef_Ranged",     ['EDID', 'LNAM']),
    0x852: ('FLST', "MFOP_ClassDef_Mage",       ['EDID', 'LNAM']),
    0x85F: ('FLST', "MFOP_Classes",             ['EDID', 'LNAM']),
    # The addon's OWN MCM quest — carries MFOP_MCM (extends MCM_ConfigBase); the
    # tab ships entirely inside the ESL + its config. Start-game-enabled, NOT
    # run-once, so it MUST be in SEQ/MFO_Progression.seq (checked below).
    0x870: ('QUST', "MFOP_MCMQuest",            ['EDID', 'DNAM', 'VMAD']),
}

# The addon's start-game-enabled non-run-once quests, listed in its own SEQ
# (SEQ/MFO_Progression.seq) — same rule as the main plugin's SEQ_EXPECTED.
PROG_SEQ_EXPECTED = {0x870}

# basename -> (required table, seq expectations or None [no SEQ file at all])
PROFILES = {
    "MFO.esp":             (REQUIRED, SEQ_EXPECTED),
    "MFO_Progression.esl": (PROG_REQUIRED, PROG_SEQ_EXPECTED),
}

GRUP_HDR = 24
REC_HDR = 24


def parse_subrecords(body):
    """Return {type: bytes} for a record body (FIRST occurrence of each type),
    plus '#LNAM' = the COUNT of LNAM entries (FLST entry count — the manifest
    check needs it, since setdefault keeps only the first LNAM). Handles no
    compressed records (we emit none) and stops cleanly on truncation."""
    out = {}
    lnam_count = 0
    i = 0
    while i + 6 <= len(body):
        t = body[i:i + 4].decode('ascii', 'replace')
        ln = struct.unpack('<H', body[i + 4:i + 6])[0]
        if i + 6 + ln > len(body):
            break
        out.setdefault(t, body[i + 6:i + 6 + ln])
        if t == 'LNAM':
            lnam_count += 1
        i += 6 + ln
    out['#LNAM'] = lnam_count
    return out


def walk(data):
    """Yield (type, formid, flags, subrecords) for every record, descending
    into GRUPs. Emitted plugins are shallow, but descend properly anyway."""
    recs = []

    def _walk(buf, off, end):
        while off + REC_HDR <= end:
            t = buf[off:off + 4].decode('ascii', 'replace')
            size = struct.unpack('<I', buf[off + 4:off + 8])[0]
            if t == 'GRUP':
                _walk(buf, off + GRUP_HDR, off + size)
                off += size
                continue
            flags = struct.unpack('<I', buf[off + 8:off + 12])[0]
            fid = struct.unpack('<I', buf[off + 12:off + 16])[0]
            body = buf[off + REC_HDR:off + REC_HDR + size]
            recs.append((t, fid, flags, parse_subrecords(body)))
            off += REC_HDR + size

    _walk(data, 0, len(data))
    return recs


def audit_one(esp):
    base = os.path.basename(esp)
    if base not in PROFILES:
        print(f"FAIL: no audit profile for '{base}' (know: {', '.join(PROFILES)})")
        return 1
    required, seq_expected = PROFILES[base]
    is_main = (base == "MFO.esp")
    exp_masters = EXPECTED_MASTERS.get(base, ["Skyrim.esm"])
    own_prefix  = len(exp_masters)   # own records sit at master index == #masters

    if not os.path.exists(esp):
        print(f"FAIL: {esp} not found -- run MFO_GenerateESP.py first")
        return 1

    data = open(esp, 'rb').read()
    recs = walk(data)
    errors, warnings = [], []

    # 1. TES4
    tes4 = [r for r in recs if r[0] == 'TES4']
    if len(tes4) != 1:
        errors.append(f"expected exactly 1 TES4, found {len(tes4)}")
    else:
        _, _, flags, subs = tes4[0]
        if not (flags & 0x200):
            errors.append("TES4 missing ESL flag 0x200 -- plugin will consume a full load slot")
        raw = data
        mast_count = raw.count(b'MAST')
        if mast_count != len(exp_masters):
            errors.append(f"expected exactly {len(exp_masters)} master(s) {exp_masters}, "
                          f"found {mast_count} MAST subrecords")
        for m in exp_masters:
            if (m.encode('latin1') + b'\x00') not in raw:
                errors.append(f"expected master '{m}' not present")
        hedr = subs.get('HEDR')
        next_id = struct.unpack('<I', hedr[8:12])[0] if hedr and len(hedr) >= 12 else None

    own = [r for r in recs if r[0] != 'TES4']
    if not own:
        errors.append("no records emitted besides TES4")

    seen = {}
    for t, fid, flags, subs in own:
        prefix = (fid >> 24) & 0xFF
        local = fid & 0xFFFFFF

        # 2. prefix
        if prefix != own_prefix:
            errors.append(f"{t} {fid:08X}: master-index prefix 0x{prefix:02X}, expected 0x{own_prefix:02X} "
                          f"({len(exp_masters)} master(s)) -- WRONG PREFIX COLLIDES WITH A MASTER'S RECORDS")
        # 3. ESL range
        if not (ESL_LO <= local <= ESL_HI):
            errors.append(f"{t} {fid:08X}: local id 0x{local:X} outside ESL range "
                          f"0x{ESL_LO:X}-0x{ESL_HI:X} -- the ESL flag is a lie and the game truncates")
        # 5. duplicates
        if fid in seen:
            errors.append(f"duplicate FormID {fid:08X} ({seen[fid]} and {t})")
        seen[fid] = t

        # 4. NEXT_OBJECT_ID
        if next_id is not None and local >= next_id:
            errors.append(f"{t} {fid:08X}: local id 0x{local:X} >= NEXT_OBJECT_ID 0x{next_id:X}")

    # 6 + 7. required records and their subrecords
    by_local = {(fid & 0xFFFFFF): (t, subs) for t, fid, _, subs in own}
    for local, (want_type, want_edid, want_subs) in required.items():
        if local not in by_local:
            errors.append(f"REQUIRED record 0x{local:03X} ({want_edid}) MISSING -- "
                          f"the DLL looks this up by local id and will log 'form not found'")
            continue
        got_type, subs = by_local[local]
        if got_type != want_type:
            errors.append(f"0x{local:03X}: expected {want_type}, found {got_type}")
        edid = subs.get('EDID', b'').rstrip(b'\x00').decode('ascii', 'replace')
        if edid != want_edid:
            errors.append(f"0x{local:03X}: EDID '{edid}', expected '{want_edid}'")
        for s in want_subs:
            if s not in subs:
                errors.append(f"0x{local:03X} ({want_edid}): missing required subrecord {s}")

    if is_main:
        # SPEL sanity: a lesser power must be SPIT type 3. Type 4 is an Ability,
        # which is passive and never fires a cast event -- the board would never open.
        if 0x801 in by_local:
            spit = by_local[0x801][1].get('SPIT')
            if spit and len(spit) >= 12:
                stype = struct.unpack('<I', spit[8:12])[0]
                if stype != 3:
                    errors.append(f"SPEL 0x801: SPIT type {stype}, expected 3 (Lesser Power). "
                                  f"Type 4 is an Ability and never fires TESSpellCastEvent")

        # RETREAT PROBE sanity: the probe's entire question is whether kIgnoreCombat
        # (0x00100000) on a Travel package survives a live combat controller. A
        # record missing the bit measures nothing while looking like a clean run.
        if 0x831 in by_local:
            pkdt = by_local[0x831][1].get('PKDT')
            if pkdt and len(pkdt) >= 7:
                pflags = struct.unpack('<I', pkdt[:4])[0]
                if pflags != 0x00102000:
                    errors.append(f"PACK 0x831: PKDT flags {pflags:08X}, expected 00102000 "
                                  f"(kIgnoreCombat 0x00100000 | preferred-speed 0x2000)")
                if pkdt[6] != 2:
                    errors.append(f"PACK 0x831: PKDT byte6 (preferredSpeed) {pkdt[6]}, expected 2 (Run)")
    else:
        # Progression addon: the DLL keys DETECTION on MFOP_Version resolving
        # with a non-zero record default (§10). A zero stamp would read as
        # "version 0" and muddy the MCM Detected line.
        if 0x800 in by_local:
            fltv = by_local[0x800][1].get('FLTV')
            if fltv and len(fltv) >= 4:
                val = struct.unpack('<f', fltv[:4])[0]
                if val <= 0.0:
                    errors.append(f"GLOB 0x800 MFOP_Version FLTV is {val:g} -- must be a positive stamp")
        # The fractional multiplier knobs must be FLOAT-typed ('f') so authors
        # can set 0.5/2.5-style values in the CK/xEdit (a short-typed GLOB
        # floors them in the editor UI). Keep in lockstep with PROG_GLOBS.
        # §18.6 Stage 3: 0x802 is now MFOP_LevelsPerPerkPoint, a whole-number
        # level COUNT ('s'), so it drops off this list — only the genuinely
        # fractional multiplier knobs stay float-typed.
        for local, edid in ((0x803, "MFOP_SkillPointsPerLevel"),
                            (0x804, "MFOP_SharedGrowthDivisor"),
                            (0x806, "MFOP_VeteranCatchupMult")):
            if local in by_local:
                fnam = by_local[local][1].get('FNAM')
                if fnam != b'f':
                    errors.append(f"GLOB 0x{local:03X} {edid}: FNAM {fnam!r}, expected b'f' "
                                  f"(float-typed so fractional values are authorable)")
        # §18.6 Stage 3: the addon MANIFEST FLST must carry the economy GLOBs
        # the DLL reads off it — entry[0] MFO.esp sentinel, entry[1] the
        # classes-list FLST, entries[2..] the 7 economy GLOBs (perk divisor,
        # skill/lvl, manual/lvl, shared divisor, respec, cap, dev-cmd). A short
        # manifest means the DLL falls back to DLL defaults for the missing
        # knobs -- silent, so audit it.
        MANIFEST_ENTRIES = 2 + 7
        if 0x821 in by_local:
            n = by_local[0x821][1].get('#LNAM', 0)
            if n != MANIFEST_ENTRIES:
                errors.append(f"FLST 0x821 MFOP_AddonManifest has {n} LNAM entr(ies), "
                              f"expected {MANIFEST_ENTRIES} (MFO.esp sentinel + classes-list "
                              f"+ 7 economy GLOBs)")

        # Class-skill lists: every LNAM entry must point into Skyrim.esm
        # (master index 0x00) -- the ESL cannot legally reference anything else.
        for local in (0x810, 0x811, 0x812):
            if local not in by_local:
                continue
            body_subs = by_local[local][1]
            lnam = body_subs.get('LNAM')
            if lnam and len(lnam) >= 4:
                fid = struct.unpack('<I', lnam[:4])[0]
                if (fid >> 24) != 0x00:
                    errors.append(f"FLST 0x{local:03X}: LNAM {fid:08X} not a Skyrim.esm form")

    # 8. SEQ -- each plugin that has start-game-enabled non-run-once quests
    # needs its OWN SEQ (Data/SEQ/<plugin-stem>.seq). A profile with
    # seq_expected is None carries no quests, so a SEQ would itself be a bug.
    if seq_expected is not None:
        seq_name = os.path.splitext(base)[0] + ".seq"
        seq_path = os.path.join(os.path.dirname(esp), "SEQ", seq_name)
        if not os.path.exists(seq_path):
            errors.append(f"SEQ/{seq_name} missing -- start-game-enabled non-run-once quests never start "
                          "on an existing save without it")
        else:
            raw = open(seq_path, 'rb').read()
            if len(raw) % 4:
                errors.append(f"SEQ/{seq_name} length {len(raw)} is not a multiple of 4")
            listed = {struct.unpack('<I', raw[i:i + 4])[0] & 0xFFFFFF for i in range(0, len(raw) - 3, 4)}
            for want in seq_expected:
                if want not in listed:
                    errors.append(f"SEQ does not list 0x{want:03X} -- that quest will never start")

    print(f"MFO ESP audit -- {esp}")
    print(f"  {len(own)} own record(s), {len(recs)} total")
    for w in warnings:
        print(f"  WARN: {w}")
    if errors:
        print()
        for e in errors:
            print(f"  FAIL: {e}")
        print(f"\nFAIL ({len(errors)} error(s))")
        return 1
    print("\nPASS")
    return 0


def main():
    targets = sys.argv[1:] if len(sys.argv) > 1 else [
        os.path.join(ROOT, "out", "MFO.esp"),
        os.path.join(ROOT, "out", "MFO_Progression.esl"),
    ]
    rc = 0
    for esp in targets:
        rc |= audit_one(esp)
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
