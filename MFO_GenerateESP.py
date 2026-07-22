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
FID_COMMAND_QUEST  = OWN | 0x80A   # M9: carries the alias MFO fills with a target
FID_PROBE_GLOB     = OWN | 0x80B   # M9 PoC: GetGlobalValue switchboard for the probes
# 0x80C-0x80F  reserved: more command aliases / globals
# 0x810+       reserved: player-side perks, if that is ever ruled in
FID_CAST_PACKAGE   = OWN | 0x820   # M9: PACK instance riding vanilla UseMagic
FID_POC_PACK_BASE  = OWN | 0x821   # M9 PoC: one PACK per probe, 0x821+
# 0x821+       reserved: one PACK per action verb (attack, travel, hold, activate)
NEXT_OBJECT_ID     = 0x900         # first never-used local id

# Vanilla refs
FREF_EQUP_VOICE = 0x00025BEE       # EQUP "Voice" — required ETYP on a lesser power
FREF_PLAYER     = 0x00000014       # PlayerRef — always loaded, near the follower

# ── M9: vanilla PACKAGE TEMPLATES (PACK type 19) ───────────────────────────
# Skyrim ships a template for every action MFO needs, so MFO authors NONE.
# Verified by dumping Skyrim.esm (ENGINE_NOTES §0.17): 104 templates exist.
FREF_TMPL_USEMAGIC     = 0x000504F5   # Spell, Target, CastTime, Cooldown, NumToCast
FREF_TMPL_USEWEAPON    = 0x0001C338
FREF_TMPL_HOLDPOSITION = 0x000503D0
FREF_TMPL_TRAVEL       = 0x00016FAA
FREF_TMPL_ACTIVATE     = 0x00019B2D

# The seeded spell for the main package. Magelight, NOT a heal: the main
# package's shipped shape is now "Aimed spell at the foe alias" (the only
# alias-targeted shape with vanilla precedent -- see make_cast_package), and
# seeding a Self-delivery spell into an alias-targeted package would be a
# delivery/target mismatch vanilla never ships (0 of 46 UseMagic instances
# pair a Self spell with an alias target aimed at someone else... the one
# TG08BMercerStatueSceneCast exception is a scene shockwave). The DLL will
# vary the Spell input per action; the RECORD ships the max-precedent pairing.
FREF_SEED_SPELL = 0x00043323       # Magelight -- FF/Aimed, harmless, visible

# ── M9 PoC: the probe LADDER -- one axis of novelty per probe ──────────────
# Spell properties read from Skyrim.esm, NOT from a wiki -- and the load order
# can redefine any of them (#66).
#
# Every probe is gated by CTDA `GetGlobalValue(MFO_ProbeSelect) == index`, so
# exactly ONE is valid at a time. Without gates the engine runs the FIRST
# valid package in ALPC order and the rest are dead records: of vanilla's 740
# aliases carrying >=2 packages, the unconditioned fallback sits at the BOTTOM
# of the list in 169+ (CU/CCU/CCCU...) against ~2 the other way round, and
# MG08's alias 0 orders its stage-gated package above its unconditioned one.
# In game:  set MFO_ProbeSelect to N   then  prid 198FA / evp
#
# The ladder, and what each step's failure would mean:
#   1  Magelight -> PlayerRef.   Byte-shape of the nine vanilla alias-
#      delivered UseMagic packages (targType 0 + Aimed spell; 9/9), target
#      swapped from an XMarker to the player (t0->player: 6 in-template).
#      If THIS one does not cast, alias-delivered casting itself is broken.
#   2  FastHealing -> PlayerRef. Self-delivery spell, target as an anchor on
#      ANOTHER ref -- the dunReachwaterRock Gauldur shape (1 instance). If 1
#      casts and 2 does not, Self-delivery under ALPC is the blocker.
#   3  FastHealing -> CosnachREF. Target resolves to the RUNNER -- zero
#      vanilla precedent, and the t4->alias0 probe already stalled silently
#      on the same resolved object. PREDICTION: owned-but-inert (#65). This
#      probe exists to confirm or refute that prediction cheaply, because
#      cast_self NEEDS some shape and vanilla ships none under ALPC.
#   4  CollegePracticeWard -> PlayerRef. Concentration axis, cost 0.
#   5  Magelight -> alias 1 (t4 + QNAM), alias 1 force-filled with the
#      player. THE GENERALIZED SHAPE -- alias-delivered package targeting a
#      different alias, which is how cast_target must work for an arbitrary
#      foe. Mechanism precedent: 356 alias-delivered packages with an
#      alias-valued input, all with QNAM (CWFinaleLeaderExecuteEnemyLeader is
#      the attack-verb exemplar); within UseMagic the t4 target has 7
#      non-delivered instances, so this is the one-axis novelty to prove.
POC_PROBES = [
    # (index, spell FormID, label,               target)
    (1, 0x00043323, "MagelightAtPlayer",   ("t0", FREF_PLAYER)),
    (2, 0x0002F3B8, "FastHealingAnchorPlayer", ("t0", FREF_PLAYER)),
    (3, 0x0002F3B8, "FastHealingSelfRef",  ("t0", None)),   # None -> POC_ACTOR_REF
    (4, 0x000E8449, "WardAnchorPlayer",    ("t0", FREF_PLAYER)),
    (5, 0x00043323, "MagelightAtAlias1",   ("t4", 1)),
]

# ── M9 PROOF OF CONCEPT ────────────────────────────────────────────────────
# CosnachREF, an ACHR (placed reference) -- verified by dumping Skyrim.esm.
# A Specific Reference alias fill (ALFR) takes exactly this, which is the same
# subrecord vanilla uses to put the player in an alias (MQGreybeardCall).
#
# HARDCODED ON PURPOSE, and only while the architecture is unproven: it removes
# every runtime variable so a failure means the RECORDS are wrong, not the
# code. The moment a follower casts from this package, the fill becomes
# targeted and this constant goes away.
POC_ACTOR_REF = 0x000198FA
# OFF unless explicitly asked for. It was True in the tracked generator, which
# means the next RELEASE would have force-filled Cosnach into a priority-60
# package for EVERY PLAYER at quest start. v0.6.0 predates it and is clean;
# main was not. Test builds set MFO_POC=1.
POC_ENABLED   = os.environ.get("MFO_POC") == "1"

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


# ── GLOB ────────────────────────────────────────────────────────────────────
def make_glob():
    # The probe switchboard. Layout mirrored from vanilla DA16ErandurCheckpoint
    # (00017418-style quest-control global): EDID + FNAM 's' (short) + FLTV
    # float. Default 0.0 = NO probe package valid, follower behaves normally.
    # In game: `set MFO_ProbeSelect to N`, then re-evaluate with `prid 198FA`
    # + `evp` (or wait for the engine's own package re-evaluation).
    body = subrec('EDID', zstr("MFO_ProbeSelect")) + subrec('FNAM', b's')
    body += subrec('FLTV', struct.pack('<f', 0.0))
    return group('GLOB', record('GLOB', FID_PROBE_GLOB, 0, body))


# ── KYWD ────────────────────────────────────────────────────────────────────
def make_kywd():
    # Tags spells MFO tutored, so PO3.RemoveAddedSpells(actor, "MFO.esp", [kw])
    # can revoke everything this mod ever taught, independent of the ledger
    # (DESIGN.md 5.4 -- the backstop, not the primary path).
    body = subrec('EDID', zstr("MFO_GrantedSpell")) + subrec('CNAM', struct.pack('<I', 0))
    return group('KYWD', record('KYWD', FID_GRANTED_KYWD, 0, body))


# ── QUST ────────────────────────────────────────────────────────────────────
def qust_dnam(flags=0x0011, priority=30, qtype=0):
    """QUEST_DATA. Layout DECODED from all 1811 vanilla QUSTs, not inferred.

        0..1  flags     uint16   0x0011 = kEnabled | kStartsEnabled
        2     priority  uint8    the CK 0-100 scale; 30 is vanilla's default
        3     unused    uint8    zero in 1697/1811
        4..7  delay     float    0.0 in ALL 1811
        8..11 type      uint32   0 = None/Misc

    The previous version wrote `<B 20> + b'\x01\x00\xff' + <HHI>`, which put a
    literal 20 in flags-low, 0xFF garbage in the unused byte, and the caller's
    flags into the DELAY FLOAT where they were inert. Every MFO quest shipped
    flags=0x0114 and priority=0; they started only because kStartsEnabled
    happened to land on a set bit.

    Priority is not cosmetic for M9: it is the lever that decides whose alias
    package wins when another follower framework also offers one. Vanilla
    spreads it deliberately -- DialogueFollower 50, scene quests 80-96.
    """
    return struct.pack('<HBBfI', flags, priority, 0, 0.0, qtype)


def make_startup_quest():
    # Deliberately carries NO VMAD. The DLL grants the Field Orders power
    # itself (Actor::AddSpell at kPostLoadGame/kNewGame). Attaching a script we
    # do not ship spams a Papyrus error every load — MEO shipped that bug in
    # every 1.0.x zip (INVARIANTS #43). The record exists to reserve its
    # FormID, which is frozen.
    body = subrec('EDID', zstr("MFO_StartupQuest")) + subrec('FULL', zstr("MFO Startup"))
    # kEnabled | kStartsEnabled | kRunOnce. RUN ONCE IS 0x0100, not 0x0004 --
    # the old code's 0x0004 was never the run-once bit, it just happened to sit
    # in a field the game ignored.
    body += subrec('DNAM', qust_dnam(0x0011 | 0x0100)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_STARTUP_QUEST, 0, body)


def make_mcm_quest():
    # Start-game-enabled, NOT run-once (SkyUI cannot re-register a run-once
    # quest). Zero VMAD properties: MCM Helper renders from
    # Data/MCM/Config/MFO/config.json, persists to Data/MCM/Settings/MFO.ini,
    # and derives modName from this plugin's stem.
    vmad = VMADBuilder()
    vmad.add_script("MFO_MCM", [])
    body = subrec('EDID', zstr("MFO_MCMQuest")) + subrec('FULL', zstr("MFO MCM")) + subrec('VMAD', vmad.build())
    # kEnabled | kStartsEnabled, deliberately NOT run-once: SkyUI cannot
    # re-register a run-once quest.
    body += subrec('DNAM', qust_dnam(0x0011)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_MCM_QUEST, 0, body)



# ── M9: the command quest + its alias ───────────────────────────────────────
def make_command_quest():
    """The delivery route for a package onto a follower MFO does not own.

    Structure copied from vanilla `CRTwinsPostQuest` (0010FE25), whose alias
    "OldQG" attaches two packages via `ALPC`. That is the mechanism: **an alias
    carries packages, and they apply to whoever fills it.**

    ALIAS 0 IS THE FOLLOWER, not the target. The package is delivered BY being
    on the actor's alias, so the actor must be the one in it. For a self-cast
    the package also TARGETS alias 0, which is why the first proof is a self
    heal -- one alias does both jobs.

    ALIAS 1 is the foe, reserved for `act.cast_target` and `act.attack`. It
    carries no packages; it exists to be pointed at.
    """
    body  = subrec('EDID', zstr("MFO_CommandQuest"))
    body += subrec('FULL', zstr("MFO Command"))
    # Start Game Enabled, NOT run-once: it must survive to be refilled.
    # Priority 60: above vanilla's default 30 and above DialogueFollower's 50,
    # below the scene quests at 80-96. Chosen ONCE and deliberately -- §4.6
    # forbids escalating it in a fight with another mod.
    body += subrec('DNAM', qust_dnam(0x0011, priority=60))
    body += subrec('NEXT', b'')
    # ANAM (next alias id) goes BEFORE the alias blocks. All 1,607 vanilla
    # quests that have aliases do it that way -- 1607 before, 0 after. The
    # generator's own doctrine is to mirror vanilla order exactly rather than
    # rely on the engine's Load() tolerating a reordering.
    body += subrec('ANAM', struct.pack('<I', 2))

    # ── alias 0: the actor MFO is commanding, and the package it carries ──
    # Flags: Optional (0x02) so the quest starts unfilled, Allow Reuse In Quest
    # (0x08) because MFO refills it every action, Allow Reserved (0x200) so a
    # follower another quest has reserved -- which is EVERY framework follower --
    # can still be taken.
    body += subrec('ALST', struct.pack('<I', 0))
    body += subrec('ALID', zstr("MFO_CommandActor"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    if POC_ENABLED:
        # SPECIFIC REFERENCE fill. No conditions, no runtime, no DLL: the quest
        # starts, the alias already holds this actor, and ALPC hands them the
        # packages. Which ONE is valid is chosen by MFO_ProbeSelect -- every
        # probe carries a GetGlobalValue CTDA, because the engine runs the
        # FIRST valid package in ALPC order and unconditioned packages above
        # others make them dead records (vanilla stacks gated packages above
        # the unconditioned fallback: CU/CCU/CCCU 169+, the reverse ~2).
        body += subrec('ALFR', struct.pack('<I', POC_ACTOR_REF))
        # Probes only. The main package is deliberately NOT attached under
        # POC: it is ungated, so it would shadow every probe below it.
        for idx, sp, label, tgt in POC_PROBES:
            body += subrec('ALPC', struct.pack('<I', FID_POC_PACK_BASE + idx - 1))
    else:
        body += subrec('ALPC', struct.pack('<I', FID_CAST_PACKAGE))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')

    # ── alias 1: the foe, for targeted verbs. No packages. ──
    body += subrec('ALST', struct.pack('<I', 1))
    body += subrec('ALID', zstr("MFO_CommandTarget"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    if POC_ENABLED:
        # Probe 5 targets alias 1 (PTDA t4), so the alias must hold someone.
        # The player: always loaded, always near the follower, and a vanilla
        # ALFR precedent (72 vanilla aliases force-fill PlayerRef).
        body += subrec('ALFR', struct.pack('<I', FREF_PLAYER))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')
    return record('QUST', FID_COMMAND_QUEST, 0, body)


# ── M9: the cast package ────────────────────────────────────────────────────
def pack_input(kind, payload_type, payload):
    """One templated-package data input: ANAM names the type, then its value."""
    return subrec('ANAM', zstr(kind)) + subrec(payload_type, payload)


def probe_ctda(index):
    """CTDA: GetGlobalValue(MFO_ProbeSelect) == index.

    Byte layout mirrored from a shipped func-74 package condition,
    KodrirCarryBucket11x2 (0010F5DB):
        op u8 (0x00 = Equal, no flags; vanilla's 0x60 is GreaterOrEqual),
        pad3, comparison f32, function u16 (74 = GetGlobalValue -- 232
        occurrences on vanilla PACK conditions), pad2, param1 = the GLOB,
        param2 = 0, runOn = 0 (subject), reference = 0, unknown = -1.
    Position in the record: after PSDT, before QNAM/PKCU -- where all 2,109
    vanilla QNAM-carrying packages and the Nirya/GetShield/CWFinale
    alias-delivered exemplars put their conditions.
    """
    return subrec('CTDA', struct.pack('<B3xfHHIIIIi',
                                      0x00, float(index), 74, 0,
                                      FID_PROBE_GLOB, 0, 0, 0, -1))


# Concentration + Self delivery has no reachable shape under alias delivery:
# vanilla only ever ships it with targType 6, and targType 6 in an
# alias-delivered package's target slot is a zero cell that CTD'd (ENGINE_NOTES
# §0.20/§0.21, INVARIANTS #67). Refuse rather than emit.
def refuse_concentration_self(casting_type, delivery):
    return casting_type == 2 and delivery == 0


def build_usemagic(fid, edid, spell, target, bounds, ctda=b'', qnam=None):
    """One PACK instance riding vanilla `UseMagic` (000504F5).

    Subrecord ORDER is part of the shape: EDID, PKDT, PSDT, [CTDA], [QNAM],
    PKCU, values, UNAM run, XNAM, POBA/POEA/POCA. All 2,109 vanilla packages
    that carry QNAM put it immediately BEFORE PKCU -- zero put it after. The
    crashed record of 2026-07-22 had it AFTER, one of its three unprecedented
    axes.

    target: ('t0', refFormID)  -- specific reference, the nine vanilla
                                  alias-delivered UseMagic packages' shape
            ('t4', aliasIdx)   -- reference alias; QNAM becomes MANDATORY
                                  (626/626 vanilla packages with an
                                  alias-valued input carry it; 0 do not)
            ('t6', 0) is REFUSED here: zero vanilla precedent in the target
            slot of an alias-delivered package, and it is the shape that
            CTD'd in the field (ENGINE_NOTES 0.20).
    bounds: (castMin, castMax, coolMin, coolMax, numMin, numMax) -- 4.5c's
            completion bound. FF default (0, 1, 1, 3, 1, 1); Concentration
            carries its channel seconds in CastTime.
    """
    tkind, tval = target
    if tkind == 't6':
        raise SystemExit(f"REFUSED {edid}: targType 6 in the target slot of an "
                         "alias-delivered package -- 0 vanilla precedents, "
                         "field CTD (ENGINE_NOTES 0.20 / INVARIANTS 66)")
    if tkind == 't4' and qnam is None:
        raise SystemExit(f"REFUSED {edid}: alias-valued target without QNAM -- "
                         "626/626 vanilla packages with an alias input carry "
                         "QNAM, 0 omit it")
    body  = subrec('EDID', zstr(edid))
    # PKDT: kIgnoreCombat (0x00100000), NOT kMustComplete -- all six vanilla
    # UseMagic instances meant to fire during a fight set kIgnoreCombat;
    # kMustComplete appears once, on a stationary channeling thrall. Byte
    # tail verbatim from TG08BMercerCombatOverrideCastAtBrynjolf (000FCC26).
    body += subrec('PKDT', struct.pack('<IBB', 0x00100000, 18, 0x04) + bytes.fromhex('038800000000'))
    # PSDT: any time, any day. 3,855 of Skyrim.esm's 5,961 PACKs ship exactly
    # these bytes.
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    body += ctda
    if qnam is not None:
        body += subrec('QNAM', struct.pack('<I', qnam))
    body += subrec('PKCU', struct.pack('<III', 11, FREF_TMPL_USEMAGIC, 1))

    # 11 values, in the template's SETTABLE order (2..12) -- the mapping is
    # not the identity: value 1 is declared slot 3 "Spell", value 2 is slot 4
    # "Target" (esp_inspect --selftest pins this).
    # PLDT type=12 ("no location"), radius 10000 -- the template default is
    # 12/0/500; TG08B's combat override uses 12/0/10000. Type 0 REQUIRES a
    # reference: 0 of 4,048 vanilla type-0 PLDTs have a null target.
    body += pack_input("Location", 'PLDT', struct.pack('<IiI', 12, 0, 10000))
    body += pack_input("TargetSelector", 'PTDA', struct.pack('<IIi', 1, spell, 0))
    if tkind == 't0':
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 0, tval, 0))
    else:
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 4, tval, 0))
    cmin, cmax, komin, komax, nmin, nmax = bounds
    body += pack_input("Bool",  'CNAM', struct.pack('<B', 0))          # HoldWhenBlocked
    body += pack_input("Float", 'CNAM', struct.pack('<f', cmin))       # CastTimeMin
    body += pack_input("Float", 'CNAM', struct.pack('<f', cmax))       # CastTimeMax
    body += pack_input("Float", 'CNAM', struct.pack('<f', komin))      # CooldownTimeMin
    body += pack_input("Float", 'CNAM', struct.pack('<f', komax))      # CooldownTimeMax
    body += pack_input("Int",   'CNAM', struct.pack('<i', nmin))       # NumToCastMin
    body += pack_input("Int",   'CNAM', struct.pack('<i', nmax))       # NumToCastMax
    body += pack_input("Bool",  'CNAM', struct.pack('<B', 0))          # DualCast

    # Bare-UNAM run naming the settable slots, then XNAM -- verbatim from the
    # vanilla instances.
    for i in range(0x02, 0x0d):
        body += subrec('UNAM', struct.pack('<B', i))
    body += subrec('XNAM', struct.pack('<B', 0x0d))

    # Empty on-begin/end/change blocks, as vanilla ships them.
    for blk in ('POBA', 'POEA', 'POCA'):
        body += subrec(blk, b'')
        body += subrec('INAM', struct.pack('<I', 0))
        body += subrec('PDTO', struct.pack('<II', 0, 0))
    return record('PACK', fid, 0, body)


# FF-spell bounds: CastTime 0/1 (24 of 33 vanilla FF instances use min 0; max
# is 0 or 1), Cooldown 1/3 (22/33), NumToCast 1/1 (14/33 -- the other common
# value, (1,0), has unmeasured semantics and 4.5c wants a bounded action).
BOUNDS_FF = (0.0, 1.0, 1.0, 3.0, 1, 1)
# Concentration: CastTime IS the channel duration (vanilla practice packages
# use 2/3 and 2/5 seconds; the "forever" thralls use 1e5..1e8, which 4.5c
# forbids MFO to ship).
BOUNDS_CONC = (2.0, 5.0, 1.0, 3.0, 1, 1)


def make_cast_package():
    """The main (DLL-era) package: cast at the foe in alias 1.

    This is the CWFinaleLeaderExecuteEnemyLeader shape -- the one alias-
    targeted delivery pattern Bethesda ships (alias-delivered package, alias-
    valued target, QNAM): 356 instances across Skyrim.esm, 0 without QNAM.
    Within UseMagic specifically the t4 target has 7 instances, none of them
    alias-DELIVERED, so probe 5 must prove the in-template combination before
    the DLL relies on it.

    The previous shape here -- targType 6 self under alias delivery -- is the
    exact record that CTD'd in the field, and build_usemagic now refuses it.
    cast_self has NO proven package shape yet; until a probe passes, the DLL
    must keep degrading cast_self to the silent path and say so (0.16).
    """
    return build_usemagic(FID_CAST_PACKAGE, "MFO_CastPackage",
                          FREF_SEED_SPELL, ('t4', 1), BOUNDS_FF,
                          qnam=FID_COMMAND_QUEST)


def make_poc_packages():
    """The probe ladder -- see POC_PROBES. One axis of novelty per probe,
    exactly one valid at a time via the MFO_ProbeSelect gate."""
    out = b''
    for idx, sp, label, (tkind, tval) in POC_PROBES:
        if tkind == 't0' and tval is None:
            tval = POC_ACTOR_REF
        bounds = BOUNDS_CONC if sp == 0x000E8449 else BOUNDS_FF
        out += build_usemagic(
            FID_POC_PACK_BASE + idx - 1, f"MFO_PoC{idx}_{label}",
            sp, (tkind, tval), bounds,
            ctda=probe_ctda(idx),
            qnam=FID_COMMAND_QUEST if tkind == 't4' else None)
    return out


def make_pack():
    body = make_cast_package()
    if POC_ENABLED:
        body += make_poc_packages()
    return group('PACK', body)


def make_qust():
    return group('QUST', make_startup_quest() + make_mcm_quest() + make_command_quest())


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_kywd()
    # GLOB between KYWD and MGEF -- vanilla top-group order (KYWD .. GLOB ..
    # MGEF), and only emitted when something references it.
    if POC_ENABLED:
        data += make_glob()
    data += make_mgef()
    data += make_spel()
    data += make_qust()
    data += make_pack()

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
        f.write(struct.pack('<I', FID_COMMAND_QUEST))

    print(f"MFO {VERSION}")
    print(f"Written: {out_path} ({len(data):,} bytes)")
    print(f"Written: {seq_path} (2 start-game-enabled quests)")
    print()
    print("Records:")
    print(f"  TES4  header     master: Skyrim.esm, ESL flagged, NEXT_OBJECT_ID 0x{NEXT_OBJECT_ID:03X}")
    print(f"  KYWD  0x{FID_GRANTED_KYWD & 0xFFF:03X}        MFO_GrantedSpell")
    print(f"  MGEF  0x{FID_ORDERS_MGEF & 0xFFF:03X}        MFO_FieldOrdersMGEF")
    print(f"  SPEL  0x{FID_ORDERS_SPELL & 0xFFF:03X}        MFO_FieldOrdersPower (lesser power)")
    print(f"  QUST  0x{FID_STARTUP_QUEST & 0xFFF:03X}        MFO_StartupQuest (run once, no VMAD)")
    print(f"  QUST  0x{FID_MCM_QUEST & 0xFFF:03X}        MFO_MCMQuest (MFO_MCM script)")
    print(f"  QUST  0x{FID_COMMAND_QUEST & 0xFFF:03X}        MFO_CommandQuest (2 aliases, DLL-filled)")
    print(f"  PACK  0x{FID_CAST_PACKAGE & 0xFFF:03X}        MFO_CastPackage -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X}"
          + ("  [NOT attached under POC]" if POC_ENABLED else ""))
    if POC_ENABLED:
        print(f"  GLOB  0x{FID_PROBE_GLOB & 0xFFF:03X}        MFO_ProbeSelect (console: set MFO_ProbeSelect to N; 0 = all probes off)")
        for idx, sp, label, (tkind, tval) in POC_PROBES:
            print(f"  PACK  0x{(FID_POC_PACK_BASE + idx - 1) & 0xFFF:03X}        MFO_PoC{idx}_{label} (gate =={idx}, target {tkind})")


if __name__ == "__main__":
    main()
