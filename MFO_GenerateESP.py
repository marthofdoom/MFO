#!/usr/bin/env python3
"""MFO.esp generator — emits the plugin and SEQ/MFO.seq from pure Python.

Never hand-edit the ESP; change this file and regenerate.

DOCTRINE (Linux-Native-Tools, and the reason this file exists):
  Never trust format docs — dump a working record and mirror it. Every byte
  layout below is forked from MEO/MRO's SHIPPED, in-game-proven generators,
  which in turn were verified against records parsed out of Skyrim.esm. The
  engine drops malformed records SILENTLY; there is no error to catch.

  If you add a record type not already here: dump a vanilla record that does
  what you want (tools/dump_record.py) and mirror its subrecord list, order,
  and byte layout exactly.

FormID band is a FROZEN generator<->DLL contract (DESIGN.md 8.2). One master
(Skyrim.esm) => own-file master index 0x01. ESL-legal range 0x800-0xFFF.

Usage:  python3 MFO_GenerateESP.py [out_dir]     (default: ./out)
"""

import os
import struct
import sys
from io import BytesIO

VERSION = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "VERSION")).read().strip()

# ── FormID band — FROZEN. Never renumber; forms are only ever ADDED. ──
# One master (Skyrim.esm) => prefix 0x01. Cross-check: DESIGN.md 8.2.
OWN = 0x01000000

FID_ORDERS_MGEF    = OWN | 0x800   # Field Orders magic effect
FID_ORDERS_SPELL   = OWN | 0x801   # Field Orders lesser power (the board opener)
FID_GRANTED_KYWD   = OWN | 0x802   # tags spells MFO tutored, for the revoke backstop
FID_STARTUP_QUEST  = OWN | 0x804   # reserved; the DLL does the granting
FID_MCM_QUEST      = OWN | 0x808   # carries the MCM Helper config script
# 0x80A-0x80F  reserved: command QUST + alias pool + command globals (Tier B, M9)
# 0x810+       reserved: player-side perks, if that is ever ruled in
# 0x820+       reserved: MFO's own conditioned PACKAGEs (Tier B, M9)
NEXT_OBJECT_ID     = 0x900         # first never-used local id

# Vanilla refs
FREF_EQUP_VOICE = 0x00025BEE       # EQUP "Voice" — required ETYP on a lesser power

# ── binary helpers (forked from MEO/MRO — byte-for-byte valid) ──
FORM_VERSION = 44


def subrec(t, d):
    return t.encode('ascii') + struct.pack('<H', len(d)) + d


def record(t, fid, fl, d):
    return (t.encode('ascii') + struct.pack('<I', len(d)) + struct.pack('<I', fl)
            + struct.pack('<I', fid) + struct.pack('<I', 0)
            + struct.pack('<H', FORM_VERSION) + struct.pack('<H', 0) + d)


def group(label, data):
    return (b'GRUP' + struct.pack('<I', 24 + len(data)) + label.encode('ascii')
            + struct.pack('<iII', 0, 0, 0) + data)


def zstr(s):
    # ASCII ONLY, everywhere — multibyte UTF-8 desyncs Papyrus error lines and
    # renders as mojibake in game.
    return s.encode('ascii') + b'\x00'


VMAD_VERSION, OBJECT_FORMAT = 5, 2


class VMADBuilder:
    def __init__(self):
        self.scripts = []

    def add_script(self, name, props):
        self.scripts.append((name, props))

    def build(self):
        b = BytesIO()
        b.write(struct.pack('<HHH', VMAD_VERSION, OBJECT_FORMAT, len(self.scripts)))
        for name, props in self.scripts:
            e = name.encode('ascii')
            b.write(struct.pack('<H', len(e)) + e + struct.pack('<B', 0) + struct.pack('<H', len(props)))
            for pn, pv in props:
                pe = pn.encode('ascii')
                b.write(struct.pack('<H', len(pe)) + pe + bytes([pv[0]]) + struct.pack('<B', 1) + pv[1:])
        return b.getvalue()


def make_tes4(next_id):
    # TES4 flags 0x200 = ESL. A survey of 3,288 LoreRim plugins showed 94% use
    # exactly this. ESL is a LIE unless every local id is inside 0x800-0xFFF.
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', next_id)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("marth")) + subrec('SNAM', zstr("marth's Follower Overhaul"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0, 0x00000200, body)


# ── MGEF ────────────────────────────────────────────────────────────────────
def mgef_inert_self_data():
    """Inert AbBlank-style clone: Script archetype, no VMAD, self-targeted.

    The effect does nothing. It exists only so the Field Orders SPEL has an
    effect to carry — the DLL observes the cast via TESSpellCastEvent (which
    fires for lesser powers) and opens the board. DATA is 152 bytes; the
    offsets below are the vanilla layout, not guesses.
    """
    d = bytearray(152)
    struct.pack_into('<I', d, 0, 0x8000)        # flags
    struct.pack_into('<I', d, 12, 0xFFFFFFFF)
    struct.pack_into('<I', d, 16, 0xFFFFFFFF)
    struct.pack_into('<I', d, 64, 1)
    struct.pack_into('<i', d, 68, -1)
    struct.pack_into('<I', d, 80, 1)            # casting type: constant
    struct.pack_into('<I', d, 84, 0)            # delivery: self
    struct.pack_into('<i', d, 88, -1)
    struct.pack_into('<f', d, 112, 1.0)
    return bytes(d)


def make_mgef():
    body = subrec('EDID', zstr("MFO_FieldOrdersMGEF")) + subrec('FULL', zstr(""))
    body += subrec('MDOB', struct.pack('<I', 0))
    body += subrec('DATA', mgef_inert_self_data()) + subrec('SNDD', b'')
    body += subrec('DNAM', struct.pack('<I', 0))
    return group('MGEF', record('MGEF', FID_ORDERS_MGEF, 0, body))


# ── SPEL ────────────────────────────────────────────────────────────────────
def spit_lesser_power():
    # cost 0, flags 0, TYPE 3 (Lesser Power), chargeTime 0, castType 1 (FF),
    # delivery 0 (self), castDuration 0, range 0, castingPerk 0.
    # TYPE MATTERS: 3 = Lesser Power (a usable shout-slot power). Type 4 is an
    # Ability, which is passive and would never fire a cast event.
    return struct.pack('<fIIfIIffI', 0.0, 0, 3, 0.0, 1, 0, 0.0, 0.0, 0)


def make_spel():
    # OBND/ETYP/DESC are all REQUIRED — a SPEL missing them silently does
    # nothing in game (DEBUGGING.md, records table).
    body = subrec('EDID', zstr("MFO_FieldOrdersPower")) + subrec('OBND', b'\x00' * 12)
    body += subrec('FULL', zstr("Field Orders"))
    body += subrec('MDOB', struct.pack('<I', 0))
    body += subrec('ETYP', struct.pack('<I', FREF_EQUP_VOICE))
    body += subrec('DESC', zstr("Look at a follower and use this to open their Gambit Board."))
    body += subrec('SPIT', spit_lesser_power())
    body += subrec('EFID', struct.pack('<I', FID_ORDERS_MGEF))
    body += subrec('EFIT', struct.pack('<fII', 0.0, 0, 0))
    return group('SPEL', record('SPEL', FID_ORDERS_SPELL, 0, body))


# ── KYWD ────────────────────────────────────────────────────────────────────
def make_kywd():
    # Tags spells MFO tutored, so PO3.RemoveAddedSpells(actor, "MFO.esp", [kw])
    # can revoke everything this mod ever taught, independent of the ledger
    # (DESIGN.md 5.4 -- the backstop, not the primary path).
    body = subrec('EDID', zstr("MFO_GrantedSpell")) + subrec('CNAM', struct.pack('<I', 0))
    return group('KYWD', record('KYWD', FID_GRANTED_KYWD, 0, body))


# ── QUST ────────────────────────────────────────────────────────────────────
def qust_dnam(flags=0x0001 | 0x0004):
    # 0x0001 Start Game Enabled, 0x0004 Run Once. Layout verified against MRO's
    # shipped, in-game-proven MCM quest — flags live at offset 4.
    return struct.pack('<B', 20) + b'\x01\x00\xff' + struct.pack('<HHI', flags, 0, 0)


def make_startup_quest():
    # Deliberately carries NO VMAD. The DLL grants the Field Orders power
    # itself (Actor::AddSpell at kPostLoadGame/kNewGame). Attaching a script we
    # do not ship spams a Papyrus error every load — MEO shipped that bug in
    # every 1.0.x zip (INVARIANTS #43). The record exists to reserve its
    # FormID, which is frozen.
    body = subrec('EDID', zstr("MFO_StartupQuest")) + subrec('FULL', zstr("MFO Startup"))
    body += subrec('DNAM', qust_dnam()) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_STARTUP_QUEST, 0, body)


def make_mcm_quest():
    # Start-game-enabled, NOT run-once (SkyUI cannot re-register a run-once
    # quest). Zero VMAD properties: MCM Helper renders from
    # Data/MCM/Config/MFO/config.json, persists to Data/MCM/Settings/MFO.ini,
    # and derives modName from this plugin's stem.
    vmad = VMADBuilder()
    vmad.add_script("MFO_MCM", [])
    body = subrec('EDID', zstr("MFO_MCMQuest")) + subrec('FULL', zstr("MFO MCM")) + subrec('VMAD', vmad.build())
    body += subrec('DNAM', qust_dnam(0x0001)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_MCM_QUEST, 0, body)


def make_qust():
    return group('QUST', make_startup_quest() + make_mcm_quest())


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_kywd()
    data += make_mgef()
    data += make_spel()
    data += make_qust()

    out_path = os.path.join(out_dir, "MFO.esp")
    with open(out_path, 'wb') as f:
        f.write(data)

    # SEQ: a start-game-enabled quest WITHOUT the Run Once flag never starts on
    # an existing save unless it is listed in Data/SEQ/<plugin>.seq (a flat
    # array of uint32 FormIDs as stored in the plugin). Without this the MCM
    # quest never runs and SkyUI has nothing to register — which is exactly why
    # the run-once startup quest works and the MCM one would not.
    seq_dir = os.path.join(out_dir, "SEQ")
    os.makedirs(seq_dir, exist_ok=True)
    seq_path = os.path.join(seq_dir, "MFO.seq")
    with open(seq_path, 'wb') as f:
        f.write(struct.pack('<I', FID_MCM_QUEST))

    print(f"MFO {VERSION}")
    print(f"Written: {out_path} ({len(data):,} bytes)")
    print(f"Written: {seq_path} (1 start-game-enabled quest)")
    print()
    print("Records:")
    print(f"  TES4  header     master: Skyrim.esm, ESL flagged, NEXT_OBJECT_ID 0x{NEXT_OBJECT_ID:03X}")
    print(f"  KYWD  0x{FID_GRANTED_KYWD & 0xFFF:03X}        MFO_GrantedSpell")
    print(f"  MGEF  0x{FID_ORDERS_MGEF & 0xFFF:03X}        MFO_FieldOrdersMGEF")
    print(f"  SPEL  0x{FID_ORDERS_SPELL & 0xFFF:03X}        MFO_FieldOrdersPower (lesser power)")
    print(f"  QUST  0x{FID_STARTUP_QUEST & 0xFFF:03X}        MFO_StartupQuest (run once, no VMAD)")
    print(f"  QUST  0x{FID_MCM_QUEST & 0xFFF:03X}        MFO_MCMQuest (MFO_MCM script)")


if __name__ == "__main__":
    main()
