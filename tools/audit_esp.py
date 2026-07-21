#!/usr/bin/env python3
"""MFO.esp audit — structural + FormID validation. PASS is a merge gate.

A FAIL here is guaranteed runtime breakage. The engine drops malformed
records SILENTLY, so this parses the emitted plugin back and checks it
against what the DLL will look for.

Checks:
  1. TES4 parses; ESL flag set; exactly one master (Skyrim.esm)
  2. Every own record uses the own-file master index prefix (0x01)
  3. Every own local id is inside the ESL-legal range 0x800-0xFFF
  4. NEXT_OBJECT_ID is above every id actually emitted
  5. No duplicate FormIDs
  6. Every FormID the DLL hardcodes is actually present
  7. Records the DLL needs carry their required subrecords
  8. SEQ lists the start-game-enabled non-run-once quest

Usage:  python3 tools/audit_esp.py [out/MFO.esp]
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

OWN_PREFIX = 0x01
ESL_LO, ESL_HI = 0x800, 0xFFF

# What native/ hardcodes. Keep in lockstep with MFO_GenerateESP.py AND with the
# DLL's lookups -- a mismatch here is the "form not found" class of bug whose
# only symptom is a feature silently missing.
REQUIRED = {
    0x800: ('MGEF', "MFO_FieldOrdersMGEF",  ['EDID', 'DATA']),
    0x801: ('SPEL', "MFO_FieldOrdersPower", ['EDID', 'OBND', 'ETYP', 'DESC', 'SPIT', 'EFID', 'EFIT']),
    0x802: ('KYWD', "MFO_GrantedSpell",     ['EDID']),
    0x804: ('QUST', "MFO_StartupQuest",     ['EDID', 'DNAM']),
    0x808: ('QUST', "MFO_MCMQuest",         ['EDID', 'DNAM', 'VMAD']),
}

# Quests that are start-game-enabled but NOT run-once must appear in the SEQ or
# they never start on an existing save.
SEQ_EXPECTED = {0x808}

GRUP_HDR = 24
REC_HDR = 24


def parse_subrecords(body):
    """Return {type: bytes} for a record body. Handles no compressed records
    (we emit none) and stops cleanly on truncation rather than guessing."""
    out = {}
    i = 0
    while i + 6 <= len(body):
        t = body[i:i + 4].decode('ascii', 'replace')
        ln = struct.unpack('<H', body[i + 4:i + 6])[0]
        if i + 6 + ln > len(body):
            break
        out.setdefault(t, body[i + 6:i + 6 + ln])
        i += 6 + ln
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


def main():
    esp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "MFO.esp")
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
        masters = [v for k, v in subs.items() if k == 'MAST']
        raw = data
        mast_count = raw.count(b'MAST')
        if mast_count != 1:
            errors.append(f"expected exactly 1 master, found {mast_count} MAST subrecords")
        if b'Skyrim.esm\x00' not in raw:
            errors.append("master is not Skyrim.esm")
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
        if prefix != OWN_PREFIX:
            errors.append(f"{t} {fid:08X}: master-index prefix 0x{prefix:02X}, expected 0x{OWN_PREFIX:02X} "
                          f"(1 master) -- WRONG PREFIX COLLIDES WITH A MASTER'S RECORDS")
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
    for local, (want_type, want_edid, want_subs) in REQUIRED.items():
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

    # SPEL sanity: a lesser power must be SPIT type 3. Type 4 is an Ability,
    # which is passive and never fires a cast event -- the board would never open.
    if 0x801 in by_local:
        spit = by_local[0x801][1].get('SPIT')
        if spit and len(spit) >= 12:
            stype = struct.unpack('<I', spit[8:12])[0]
            if stype != 3:
                errors.append(f"SPEL 0x801: SPIT type {stype}, expected 3 (Lesser Power). "
                              f"Type 4 is an Ability and never fires TESSpellCastEvent")

    # 8. SEQ
    seq_path = os.path.join(os.path.dirname(esp), "SEQ", "MFO.seq")
    if not os.path.exists(seq_path):
        errors.append("SEQ/MFO.seq missing -- start-game-enabled non-run-once quests never start "
                      "on an existing save without it")
    else:
        raw = open(seq_path, 'rb').read()
        if len(raw) % 4:
            errors.append(f"SEQ/MFO.seq length {len(raw)} is not a multiple of 4")
        listed = {struct.unpack('<I', raw[i:i + 4])[0] & 0xFFFFFF for i in range(0, len(raw) - 3, 4)}
        for want in SEQ_EXPECTED:
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


if __name__ == "__main__":
    sys.exit(main())
