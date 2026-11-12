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
        (no args = audit all emitted plugins)
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

OWN_PREFIX = 0x01
ESL_LO, ESL_HI = 0x800, 0xFFF

# Expected master list per plugin (ORDER = the master index each own/cross ref
# assumes). v1.1: MFO_Progression.esl masters ONLY Skyrim.esm — its manifest now
# self-declares via its OWN keyword (edid suffix "_MFOAddonManifest"), so there
# is NO MFO.esp reference (the Vortex fix) and own records use master index 0x01.
# The own-file prefix each plugin must use = len(its masters).
EXPECTED_MASTERS = {
    "MFO.esp":              ["Skyrim.esm"],
    "MFO_Progression.esl":  ["Skyrim.esm"],
    "MFO_Classes.esl":      ["Skyrim.esm"],
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
    0x916: ('CSTY', "MFO_MeleeSingleStyle", ['EDID', 'CSGD']),   # MeleeStyle twin, DATA 1 (no dual wield)
    # LOTD awareness: the detected flag the DLL writes and the MCM hiddenToggle reads.
    0x903: ('GLOB', "MFO_LOTDDetected",     ['EDID', 'FNAM', 'FLTV']),
    # Positional lockpick sounds: SNDR copies the DLL plays at the follower (Lockpick.cpp).
    0x904: ('SNDR', "MFO_LockpickPickMovementSD", ['EDID', 'CNAM', 'GNAM', 'ANAM', 'ONAM', 'LNAM', 'BNAM']),
    0x905: ('SNDR', "MFO_LockpickPickBreakSD",    ['EDID', 'CNAM', 'GNAM', 'ANAM', 'ONAM', 'LNAM', 'BNAM']),
}

# TOWN PHASE 2 R7: the errand WORK packages (Forms.h kErrandActivate0 / Sandbox0 / Eat0 / Sleep0, four slots each, ch.9 route). The Activate carries
# its Target as a PTDA, the other three their Location as a PLDT -- the run-time writes (Packages::SetPackage*) need those inputs to exist.
for _i in range(4):
    REQUIRED[0x906 + _i] = ('PACK', f"MFO_ErrandActivate{_i}", ['EDID', 'PKDT', 'PKCU', 'PTDA'])
    REQUIRED[0x90A + _i] = ('PACK', f"MFO_ErrandSandbox{_i}",  ['EDID', 'PKDT', 'PKCU', 'PLDT'])
    REQUIRED[0x90E + _i] = ('PACK', f"MFO_ErrandEat{_i}",      ['EDID', 'PKDT', 'PKCU', 'PLDT'])
    REQUIRED[0x912 + _i] = ('PACK', f"MFO_ErrandSleep{_i}",    ['EDID', 'PKDT', 'PKCU', 'PLDT'])

# Quests that are start-game-enabled but NOT run-once must appear in the SEQ or
# they never start on an existing save.
SEQ_EXPECTED = {0x808, 0x80A, 0x80C, 0x830, 0x80E}

# MFO_Progression.esl — what native/Progression.h (0x800/0x801) and
# native/progression/ProgAllocator.h (the rest) hardcode. Keep in lockstep with the
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
    0x80A: ('GLOB', "MFOP_LevelsPerCraftPerkPoint", ['EDID', 'FNAM', 'FLTV']),
    # Class skill lists carry entries (LNAM); the perk lists ship EMPTY on
    # purpose (xEdit extension point) so LNAM is not required there.
    0x810: ('FLST', "MFOP_ClassSkills_Melee",   ['EDID', 'LNAM']),
    0x811: ('FLST', "MFOP_ClassSkills_Ranged",  ['EDID', 'LNAM']),
    0x812: ('FLST', "MFOP_ClassSkills_Mage",    ['EDID', 'LNAM']),
    0x818: ('FLST', "MFOP_ClassPerks_Melee",    ['EDID']),
    0x819: ('FLST', "MFOP_ClassPerks_Ranged",   ['EDID']),
    0x81A: ('FLST', "MFOP_ClassPerks_Mage",     ['EDID']),
    0x820: ('KYWD', "MFOP_Enrolled",            ['EDID']),
    # §18.6 the addon MANIFEST the DLL enumerates (entry[0]=self-declaration
    # keyword, entry[1]=the classes-list FLST, entries[2..]=the economy GLOBs).
    0x821: ('FLST', "MFOP_AddonManifest",       ['EDID', 'LNAM']),
    # v1.1 self-declaration keyword — the manifest's own join key (edid suffix
    # "_MFOAddonManifest"); the DLL matches it by that suffix at runtime.
    0x822: ('KYWD', "MFOP_MFOAddonManifest",    ['EDID']),
    # v1.1 residual #3: the entry-point verdicts self-declaration keyword and its
    # sub-FLST (the 92 POSITIONAL verdict GLOBs; perk-effectiveness judgment moved
    # out of the DLL into add-on data). LNAM count checked below (1 kywd + 92).
    0x823: ('KYWD', "MFOP_MFOEntryPointVerdicts", ['EDID']),
    0x860: ('FLST', "MFOP_EntryPointVerdicts",  ['EDID', 'LNAM']),
    # §18.6 Stage 2 — N-declared classes: MESG display names, _Stance mirrors,
    # class-def FLSTs, and the classes-list FLST the manifest points at.
    0x830: ('MESG', "MFOP_ClassName_Melee",     ['EDID', 'FULL']),
    0x831: ('MESG', "MFOP_ClassName_Ranged",    ['EDID', 'FULL']),
    0x832: ('MESG', "MFOP_ClassName_Mage",      ['EDID', 'FULL']),
    0x840: ('GLOB', "MFOP_ClassMelee_Stance",   ['EDID', 'FNAM', 'FLTV']),
    0x841: ('GLOB', "MFOP_ClassRanged_Stance",  ['EDID', 'FNAM', 'FLTV']),
    0x842: ('GLOB', "MFOP_ClassMage_Stance",    ['EDID', 'FNAM', 'FLTV']),
    # v1.1 §HMS class ratios as DATA — 4 POSITIONAL GLOBs per class-def (H,M,S,
    # primary), appended after _Stance in the class-def FLST. Melee 0x843-0x846,
    # Ranged 0x847-0x84A, Mage 0x84B-0x84E.
    0x843: ('GLOB', "MFOP_ClassDef_Melee_HmsWeightH",  ['EDID', 'FNAM', 'FLTV']),
    0x844: ('GLOB', "MFOP_ClassDef_Melee_HmsWeightM",  ['EDID', 'FNAM', 'FLTV']),
    0x845: ('GLOB', "MFOP_ClassDef_Melee_HmsWeightS",  ['EDID', 'FNAM', 'FLTV']),
    0x846: ('GLOB', "MFOP_ClassDef_Melee_HmsPrimary",  ['EDID', 'FNAM', 'FLTV']),
    0x847: ('GLOB', "MFOP_ClassDef_Ranged_HmsWeightH", ['EDID', 'FNAM', 'FLTV']),
    0x848: ('GLOB', "MFOP_ClassDef_Ranged_HmsWeightM", ['EDID', 'FNAM', 'FLTV']),
    0x849: ('GLOB', "MFOP_ClassDef_Ranged_HmsWeightS", ['EDID', 'FNAM', 'FLTV']),
    0x84A: ('GLOB', "MFOP_ClassDef_Ranged_HmsPrimary", ['EDID', 'FNAM', 'FLTV']),
    0x84B: ('GLOB', "MFOP_ClassDef_Mage_HmsWeightH",   ['EDID', 'FNAM', 'FLTV']),
    0x84C: ('GLOB', "MFOP_ClassDef_Mage_HmsWeightM",   ['EDID', 'FNAM', 'FLTV']),
    0x84D: ('GLOB', "MFOP_ClassDef_Mage_HmsWeightS",   ['EDID', 'FNAM', 'FLTV']),
    0x84E: ('GLOB', "MFOP_ClassDef_Mage_HmsPrimary",   ['EDID', 'FNAM', 'FLTV']),
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

# MFO_Classes.esl (card C7) -- the class add-on. The DLL (native/classes/Presence.cpp) keys on the
# manifest keyword's editor-id suffix "_MFOClassesManifest"; everything else is found through the
# manifest FLST / index FLSTs or, for the C6/C8 cards, by local id. Keep in lockstep with the
# generator's CGID_* table. 0x840+i = the 35 passive factions, 0x880+i / 0x8C0+i = the per-class style FLST / mask GLOB, 0x900+ = the CSTYs.
CLS_REQUIRED = {
    0x800: ('KYWD', "MFOC_MFOClassesManifest", ['EDID']),
    0x801: ('FLST', "MFOC_AddonManifest",      ['EDID', 'LNAM']),
    0x802: ('FLST', "MFOC_PassiveFactions",    ['EDID', 'LNAM']),
    0x803: ('FLST', "MFOC_ClassStyles",        ['EDID', 'LNAM']),
    0x808: ('KYWD', "MFOC_MarkedKeyword",      ['EDID']),
    0x809: ('KYWD', "MFOC_FlankedKeyword",     ['EDID']),
    0x80A: ('KYWD', "MFOC_GuardedKeyword",     ['EDID']),
    0x810: ('MGEF', "MFOC_Marked",             ['EDID', 'KSIZ', 'KWDA', 'DATA']),
    0x811: ('MGEF', "MFOC_Flanked",            ['EDID', 'KSIZ', 'KWDA', 'DATA']),
    0x812: ('MGEF', "MFOC_Guarded",            ['EDID', 'KSIZ', 'KWDA', 'DATA']),
    0x820: ('GLOB', "MFOC_Version",            ['EDID', 'FNAM', 'FLTV']),
    0x821: ('GLOB', "MFOC_XpRate",             ['EDID', 'FNAM', 'FLTV']),
    0x822: ('GLOB', "MFOC_PriestSunDps",       ['EDID', 'FNAM', 'FLTV']),
    0x823: ('GLOB', "MFOC_SaintHealPerSec",    ['EDID', 'FNAM', 'FLTV']),
    0x824: ('GLOB', "MFOC_SaintUndeadDps",     ['EDID', 'FNAM', 'FLTV']),
    0x830: ('FACT', "MFOC_OffClassWeapon",     ['EDID', 'DATA', 'CRVA']),
    0x831: ('FACT', "MFOC_MarkPending",        ['EDID', 'DATA', 'CRVA']),
    0xA00: ('PERK', "MFOC_ReferencePerk",      ['EDID', 'FULL', 'DESC', 'DATA', 'PRKE', 'EPFT', 'EPFD', 'PRKF']),
    # Card C6 carriers (the DLL, classes/Carriers.cpp, resolves these by local id and type-checks them).
    0xA40: ('MGEF', "MFOC_AbStamina",          ['EDID', 'DATA']),
    0xA41: ('MGEF', "MFOC_AbStaminaRate",      ['EDID', 'DATA']),
    0xA42: ('MGEF', "MFOC_AbSpeed",            ['EDID', 'DATA']),
    0xA50: ('SPEL', "MFOC_Mercenary_Full",     ['EDID', 'SPIT', 'EFID', 'EFIT']),
    0xA51: ('SPEL', "MFOC_Mercenary_Quarter",  ['EDID', 'SPIT', 'EFID', 'EFIT']),
    0xA74: ('SPEL', "MFOC_Scout_Quarter",      ['EDID', 'SPIT', 'EFID', 'EFIT']),
    0xA80: ('SPEL', "MFOC_MarkedSpell",        ['EDID', 'SPIT', 'EFID', 'EFIT']),
    0xA81: ('SPEL', "MFOC_FlankedSpell",       ['EDID', 'SPIT', 'EFID', 'EFIT']),
    0xA82: ('SPEL', "MFOC_GuardedSpell",       ['EDID', 'SPIT', 'EFID', 'EFIT']),
    # Card C11: the add-on's own MCM Helper quest (start-game-enabled, NOT run-once, script MFOC_MCM) -- listed in SEQ/MFO_Classes.seq.
    0xA83: ('QUST', "MFOC_MCMQuest",           ['EDID', 'DNAM', 'VMAD']),
    # Card G16: the off-class weapon damage penalty perk (ModAttackDamage x0.5 on rank >= 1 in MFOC_OffClassWeapon 0x830).
    0xA84: ('PERK', "MFOC_OffClassHalf",       ['EDID', 'FULL', 'DESC', 'DATA', 'PRKE', 'EPFT', 'EPFD', 'PRKF']),
}
CLS_SEQ_EXPECTED = {0xA83}
# Card C6 audit tables. Ability SPELs: 0xA50/0xA51 Mercenary, 0xA60-0xA73 Scout speed per level 1..20, 0xA74 Scout quarter.
CLS_ABILITY_SPELLS = [0xA50, 0xA51] + list(range(0xA60, 0xA74)) + [0xA74]
CLS_ABILITY_MGEF_AV = {0xA40: 26, 0xA41: 157, 0xA42: 30}     # Stamina, StaminaRateMult, SpeedMult (ActorValues.h)
CLS_MARKER_SPELLS = {0xA80: 0x810, 0xA81: 0x811, 0xA82: 0x812}   # carrier SPEL -> its MGEF (same casting type + delivery)
# Entry points whose tab kinds are proven (or, for tab 0, inferred) in the generator's PERK_ENTRY_TABS -> tab count.
CLS_PERK_EP_TABS = {18: 3, 27: 2, 28: 3, 29: 3, 30: 3, 33: 2, 35: 3, 36: 3, 37: 3, 38: 2, 39: 1}
CLS_PERK_FUNCS = {73, 77, 448, 560, 672, 693, 696, 715}       # every condition function a class perk may use (all seen in vanilla)
CLS_PERK_BASE, CLS_PASSIVE_FACT_BASE = 0xA10, 0x840
CLS_PASSIVE_COUNT = 35      # FACTs 0x840..0x862, one per class in Id order
CLS_MANIFEST_ENTRIES = 3 + 4   # keyword + 2 index FLSTs + 4 MCM GLOBs

# basename -> (required table, seq expectations or None [no SEQ file at all])
PROFILES = {
    "MFO.esp":             (REQUIRED, SEQ_EXPECTED),
    "MFO_Progression.esl": (PROG_REQUIRED, PROG_SEQ_EXPECTED),
    "MFO_Classes.esl":     (CLS_REQUIRED, CLS_SEQ_EXPECTED),
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
    out['#BODY'] = body
    return out


def ordered_subrecords(body):
    """[(type, bytes)] in file order (repeats kept): the class PERK audit needs the PRKE / PRKC / CTDA sequence."""
    out, i = [], 0
    while i + 6 <= len(body):
        t = body[i:i + 4].decode('ascii', 'replace')
        ln = struct.unpack('<H', body[i + 4:i + 6])[0]
        if i + 6 + ln > len(body):
            break
        out.append((t, body[i + 6:i + 6 + ln]))
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


def audit_class_carriers(by_local, errors):
    """Card C6: the ability MGEF / SPEL shapes, the marker carrier SPELs, and every class passive PERK."""
    for local, av in CLS_ABILITY_MGEF_AV.items():
        got = by_local.get(local)
        if not got or got[0] != 'MGEF':
            continue   # the REQUIRED check already reported it
        d = got[1].get('DATA', b'')
        if len(d) != 152:
            errors.append(f"MGEF 0x{local:03X}: DATA is {len(d)} bytes, expected 152")
            continue
        arch, mav = struct.unpack('<Ii', d[64:72])
        cast, deliv = struct.unpack('<II', d[80:88])
        if (arch, mav, cast, deliv) != (34, av, 0, 0):
            errors.append(f"MGEF 0x{local:03X}: archetype {arch} AV {mav} casting {cast} delivery {deliv}, expected "
                          f"Peak Value Modifier (34), AV {av}, Constant (0), Self (0)")
        if 'VMAD' in got[1]:
            errors.append(f"MGEF 0x{local:03X}: carries a VMAD, ability effects must be script-less")
    for local in CLS_ABILITY_SPELLS:
        got = by_local.get(local)
        if not got or got[0] != 'SPEL':
            errors.append(f"ability SPEL 0x{local:03X} missing")
            continue
        spit = got[1].get('SPIT', b'')
        if len(spit) != 36 or struct.unpack('<IIIfII', spit[:24])[1:3] != (0x00250001, 4) or struct.unpack('<II', spit[16:24]) != (0, 0):
            errors.append(f"SPEL 0x{local:03X}: SPIT is not the vanilla Ability shape (flags 0x00250001, type 4, Constant, Self)")
        body = got[1].get('#BODY', b'')
        efids = [struct.unpack('<I', d)[0] & 0xFFFFFF for t, d in ordered_subrecords(body) if t == 'EFID']
        if not efids or any(e not in CLS_ABILITY_MGEF_AV for e in efids):
            errors.append(f"SPEL 0x{local:03X}: EFID {[hex(e) for e in efids]} is not one of the ability MGEFs")
    for local, mg in CLS_MARKER_SPELLS.items():
        sp, ef = by_local.get(local), by_local.get(mg)
        if not sp or not ef or sp[0] != 'SPEL' or ef[0] != 'MGEF':
            continue
        spit, d = sp[1].get('SPIT', b''), ef[1].get('DATA', b'')
        if len(spit) != 36 or len(d) != 152:
            errors.append(f"SPEL 0x{local:03X}: bad SPIT / carried MGEF DATA length")
            continue
        s_type = struct.unpack('<I', spit[8:12])[0]
        s_cast, s_deliv = struct.unpack('<II', spit[16:24])
        m_cast, m_deliv = struct.unpack('<II', d[80:88])
        if s_type != 0 or (s_cast, s_deliv) != (m_cast, m_deliv):
            errors.append(f"SPEL 0x{local:03X}: type {s_type} casting/delivery {(s_cast, s_deliv)} must be a Spell with its MGEF's {(m_cast, m_deliv)}")
    nperks = 0
    for i in range(CLS_PASSIVE_COUNT):
        got = by_local.get(CLS_PERK_BASE + i)
        if not got:
            continue
        nperks += 1
        local = CLS_PERK_BASE + i
        if got[0] != 'PERK':
            errors.append(f"0x{local:03X}: expected a PERK, found {got[0]}")
            continue
        faction = CLS_PASSIVE_FACT_BASE + i
        subs = ordered_subrecords(got[1].get('#BODY', b''))
        types = [t for t, _ in subs]
        if 'PRKE' not in types or 'CTDA' in types[:types.index('PRKE')]:
            errors.append(f"PERK 0x{local:03X}: a perk-LEVEL CTDA (before the first PRKE) or no entries; perk-level conditions are never "
                          f"evaluated at run time, every gate must sit on an entry's tab 0")
            continue
        data = [d for t, d in subs if t == 'DATA']
        if not data or data[0] != bytes([0, 0, 1, 1, 0]):
            errors.append(f"PERK 0x{local:03X}: first DATA is not trait 0 / level 0 / ranks 1 / playable 1 / hidden 0")
        # walk the entries
        entry, entries = None, []
        for t, d in subs:
            if t == 'PRKE':
                entry = {'prke': d, 'data': None, 'tabs': {}, 'cur': None, 'eft': None, 'epfd': None}
                entries.append(entry)
            elif entry is None:
                continue
            elif t == 'DATA' and len(d) == 3 and entry['data'] is None:
                entry['data'] = d
            elif t == 'PRKC':
                entry['cur'] = d[0]
                entry['tabs'].setdefault(d[0], [])
            elif t == 'CTDA':
                if entry['cur'] is None or len(d) != 32:
                    errors.append(f"PERK 0x{local:03X}: a CTDA outside a PRKC tab, or not 32 bytes")
                else:
                    entry['tabs'][entry['cur']].append(d)
            elif t == 'EPFT':
                entry['eft'] = d
            elif t == 'EPFD':
                entry['epfd'] = d
        if not entries:
            errors.append(f"PERK 0x{local:03X}: no entries")
        for n, e in enumerate(entries):
            where = f"PERK 0x{local:03X} entry {n}"
            if e['prke'] != bytes([2, 0, 0]):
                errors.append(f"{where}: PRKE {e['prke'].hex()} is not (entry point, rank 0, priority 0)")
            if not e['data']:
                errors.append(f"{where}: no DATA")
                continue
            ep, fn, ntabs = e['data']
            if CLS_PERK_EP_TABS.get(ep) != ntabs:
                errors.append(f"{where}: entry point {ep} with {ntabs} tab(s) is not in the verified table {CLS_PERK_EP_TABS}")
            if fn not in (1, 2, 3):
                errors.append(f"{where}: function {fn} is not float Set / Add / Multiply")
            if e['eft'] != b'\x01' or not e['epfd'] or len(e['epfd']) != 4:
                errors.append(f"{where}: EPFT / EPFD is not a float value")
            for tab, conds in e['tabs'].items():
                if tab >= ntabs:
                    errors.append(f"{where}: tab {tab} >= tab count {ntabs}")
                for c in conds:
                    func = struct.unpack('<H', c[8:10])[0]
                    if func not in CLS_PERK_FUNCS:
                        errors.append(f"{where}: condition function {func} is not one of {sorted(CLS_PERK_FUNCS)}")
            ranks = []
            for c in e['tabs'].get(0, []):
                typ, val = c[0], struct.unpack('<f', c[4:8])[0]
                func, p1 = struct.unpack('<H', c[8:10])[0], struct.unpack('<I', c[12:16])[0]
                if func == 73:
                    if (p1 & 0xFFFFFF) != faction or p1 >> 24 != 1:
                        errors.append(f"{where}: rank condition names faction 0x{p1:08X}, expected the class's own 0x{faction:03X}")
                    ranks.append((typ & 0xE0, val))
            lo = [v for o, v in ranks if o == 0x60]
            hi = [v for o, v in ranks if o == 0xA0]
            if len(ranks) != 2 or len(lo) != 1 or len(hi) != 1 or not (1 <= lo[0] <= hi[0] <= 100):
                errors.append(f"{where}: tab 0 must carry exactly the rank range pair (>= lo, <= hi) with 1 <= lo <= hi <= 100, found {ranks}")
    if nperks == 0:
        errors.append("no class passive PERK (0xA10..0xA32) found")


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
    elif base == "MFO_Classes.esl":
        # Class add-on: the passive factions / class styles come in fixed id runs, and the
        # manifest + index FLSTs must carry exactly the entries the generator wrote.
        for i in range(CLS_PASSIVE_COUNT):
            got = by_local.get(0x840 + i)
            if not got or got[0] != 'FACT' or not got[1].get('EDID', b'').startswith(b'MFOC_P_'):
                errors.append(f"0x{0x840 + i:03X}: expected a FACT named MFOC_P_*")
        # Class styles: every class has a per-class FLST (0x880+i) whose entry[0] is its mask GLOB
        # (0x8C0+i) and whose remaining entries are CSTYs, one per set mask bit
        # (1 melee, 2 melee dual, 4 ranged, 8 cast). Every CSTY carries CSGD.
        for local, (t, subs) in by_local.items():
            if t == 'CSTY' and 'CSGD' not in subs:
                errors.append(f"CSTY 0x{local:03X}: no CSGD")
        for i in range(CLS_PASSIVE_COUNT):
            fl, gl = by_local.get(0x880 + i), by_local.get(0x8C0 + i)
            if not fl or fl[0] != 'FLST' or not gl or gl[0] != 'GLOB':
                errors.append(f"class {i}: missing style FLST 0x{0x880 + i:03X} / mask GLOB 0x{0x8C0 + i:03X}")
                continue
            mask = int(struct.unpack('<f', gl[1]['FLTV'][:4])[0])
            if mask == 0 or mask & ~0xF or (mask & 2 and not mask & 1):
                errors.append(f"class {i}: style mask {mask} invalid (bits 1/2 melee single/dual, 4 ranged, 8 cast; dual needs melee)")
            if fl[1].get('#LNAM', 0) != 1 + bin(mask).count('1'):
                errors.append(f"class {i}: style FLST has {fl[1].get('#LNAM', 0)} entries, mask {mask} wants {1 + bin(mask).count('1')}")
        # Zero-avoid TWIN lists (0xB00+i, entries 35..69 of 0x803): the same shape as 0x880+i, each entry a CSTY whose
        # avoidThreat is 0 (the twin of a style with Avoid Threat, or the style itself when it already has none).
        for i in range(CLS_PASSIVE_COUNT):
            tl, fl = by_local.get(0xB00 + i), by_local.get(0x880 + i)
            if not tl or tl[0] != 'FLST' or not fl:
                errors.append(f"class {i}: missing twin style FLST 0x{0xB00 + i:03X}")
            elif tl[1].get('#LNAM', 0) != fl[1].get('#LNAM', 0):
                errors.append(f"class {i}: twin style FLST has {tl[1].get('#LNAM', 0)} entries, the style FLST {fl[1].get('#LNAM', 0)}")
        for local, (t, subs) in by_local.items():
            if t == 'CSTY' and 0xA85 <= local < 0xB00 and 'CSGD' in subs \
                    and struct.unpack('<f', subs['CSGD'][36:40])[0] != 0.0:
                errors.append(f"CSTY 0x{local:03X}: a zero-avoid twin with a non-zero avoidThreat")
        for local, want in ((0x801, CLS_MANIFEST_ENTRIES), (0x802, CLS_PASSIVE_COUNT), (0x803, 2 * CLS_PASSIVE_COUNT)):
            if local in by_local:
                n = by_local[local][1].get('#LNAM', 0)
                if n != want:
                    errors.append(f"FLST 0x{local:03X} has {n} LNAM entr(ies), expected {want}")
        # Marker MGEF: Script archetype (DATA offset 64 == 1), no VMAD (script-less).
        for local in (0x810, 0x811, 0x812):
            if local in by_local:
                subs = by_local[local][1]
                d = subs.get('DATA', b'')
                if len(d) != 152 or struct.unpack('<I', d[64:68])[0] != 1:
                    errors.append(f"MGEF 0x{local:03X}: DATA is not the 152-byte Script-archetype layout")
                if 'VMAD' in subs:
                    errors.append(f"MGEF 0x{local:03X}: carries a VMAD, markers must be script-less")
        if 0x820 in by_local:
            fltv = by_local[0x820][1].get('FLTV')
            if not fltv or struct.unpack('<f', fltv[:4])[0] <= 0.0:
                errors.append("GLOB 0x820 MFOC_Version FLTV must be a positive stamp")
        audit_class_carriers(by_local, errors)
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
        # the DLL reads off it — entry[0] self-declaration keyword, entry[1] the
        # board-tab label MESG (v1.1 Phase 6c), entry[2] the classes-list FLST,
        # entries[3..] the 8 economy GLOBs (perk divisor, skill/lvl, manual/lvl,
        # shared divisor, respec, cap, dev-cmd, craft divisor). A short manifest means the DLL
        # falls back to DLL defaults for the missing knobs -- silent, so audit it.
        # v1.1 residual #3: entry[3] is now the entry-point VERDICTS sub-FLST.
        MANIFEST_ENTRIES = 4 + 8
        if 0x821 in by_local:
            n = by_local[0x821][1].get('#LNAM', 0)
            if n != MANIFEST_ENTRIES:
                errors.append(f"FLST 0x821 MFOP_AddonManifest has {n} LNAM entr(ies), "
                              f"expected {MANIFEST_ENTRIES} (self-declaration keyword + "
                              f"board-tab label MESG + classes-list + verdicts-list + "
                              f"8 economy GLOBs)")

        # v1.1 residual #3: the entry-point VERDICTS sub-FLST (0x860) carries its
        # self-declaration keyword + 92 POSITIONAL verdict GLOBs. A short list
        # means the DLL reads no verdict for the missing entry points (they flag
        # marginal) -- silent, so audit the count. The DLL reads by ORDER off it.
        VERDICT_FLST_ENTRIES = 1 + 92
        if 0x860 in by_local:
            n = by_local[0x860][1].get('#LNAM', 0)
            if n != VERDICT_FLST_ENTRIES:
                errors.append(f"FLST 0x860 MFOP_EntryPointVerdicts has {n} LNAM entr(ies), "
                              f"expected {VERDICT_FLST_ENTRIES} (self-declaration keyword + "
                              f"92 positional verdict GLOBs)")

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
        os.path.join(ROOT, "out", "MFO_Classes.esl"),
    ]
    rc = 0
    for esp in targets:
        rc |= audit_one(esp)
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
