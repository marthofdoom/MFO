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

import json
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
# 0x803 RETIRED (v1.1): was FID_ADDON_SENTINEL, the MFO.esp addon-manifest join
# keyword. Add-ons now self-declare with their OWN keyword (editor-id suffix
# "_MFOAddonManifest"), so a manifest references NO MFO.esp form and needs no
# MFO.esp master (the Vortex fix). Id stays retired, never recycled (INVARIANTS #41).
FID_STARTUP_QUEST  = OWN | 0x804   # reserved; the DLL does the granting
FID_MCM_QUEST      = OWN | 0x808   # carries the MCM Helper config script
FID_COMMAND_QUEST  = OWN | 0x80A   # M9: carries the alias MFO fills with a target
FID_PROBE_GLOB     = OWN | 0x80B   # M9 PoC: GetGlobalValue switchboard for the probes
FID_LOOT_QUEST     = OWN | 0x80C   # Option A: delivery route for the travel-to-loot package
FID_TRADE_QUEST    = OWN | 0x80E   # #21 econ: carries MFO_Trade (the merchant-read/transaction bridge)
# 0x80D, 0x80F  reserved: more command aliases / globals
# 0x810+       reserved: player-side perks, if that is ever ruled in
FID_CAST_PACKAGE   = OWN | 0x820   # M9: PACK instance riding vanilla UseMagic
FID_POC_PACK_BASE  = OWN | 0x821   # M9 PoC: one PACK per probe, 0x821+
FID_TRAVEL_PACKAGE = OWN | 0x828   # Option A: PACK riding vanilla Travel (walk to a loot ref) -- slot 0
# P7 multi-follower excursions: three more travel packages, one per extra loot
# slot. Each is byte-identical to slot 0's but names its OWN target alias (3/5/7).
FID_TRAVEL_PACKAGE_1 = OWN | 0x900  # P7: slot 1 (targets loot-quest alias 3)
FID_TRAVEL_PACKAGE_2 = OWN | 0x901  # P7: slot 2 (targets loot-quest alias 5)
FID_TRAVEL_PACKAGE_3 = OWN | 0x902  # P7: slot 3 (targets loot-quest alias 7)
# 0x821+       reserved: one PACK per action verb (attack, travel, hold, activate)
# 0x829-0x82F  reserved: probe-ladder headroom (FID_POC_PACK_BASE + idx - 1)
FID_RETREAT_QUEST   = OWN | 0x830  # RETREAT PROBE: travel-to-player delivery route
FID_RETREAT_PACKAGE = OWN | 0x831  # RETREAT PROBE: Travel + kIgnoreCombat -> alias 1 (the player)
FID_CAST_STYLE      = OWN | 0x832  # P1 PROBE: caster-forward CSTY the DLL swaps onto a
                                   # latched follower's live CombatController (bProbeCastStyle)
FID_MELEE_STYLE     = OWN | 0x833  # melee-dominant CSTY: swapped in when act.equip_melee
                                   # wins the hand so the AI stops re-drawing the bow (v1.0.33)
FID_RANGED_STYLE    = OWN | 0x834  # ranged-dominant CSTY: swapped in for act.equip_ranged
# FORCED SELF-CAST (Docs/SPEC-self-cast-forced.md): the dedicated no-QNAM
# targType-6 self package, delivered by the command quest's own alias 2. §0.22
# proved probe 6's t6+no-QNAM self-cast CASTS cleanly (and REVOKED #67); the
# shipped MFO_CastPackage cannot serve self because it carries a QNAM (its foe
# target is t4 -> alias 1), and writing t6 into a QNAM-carrying record at runtime
# is the rev-4 crash cell. So self gets its OWN record, authored t6 with NO QNAM
# -- probe 6's proven-clean shape. Wiring is DLL-gated (bCastSelf) until the
# production path is deck-confirmed.
FID_CAST_PACKAGE_SELF = OWN | 0x835  # UseMagic PACK, targType-6 self, no QNAM
# APMF LOOT-TRAVEL (ch.9 0x49 route, Docs/ALLOWANCE-TEMPLATE.md T3 in the APMF
# repo): one Travel PACK per concurrent loot slot, mirroring FID_TRAVEL_PACKAGE{,
# _1,_2,_3}'s per-slot shape but with a RUNTIME-HANDLE Location input (PLDT type
# 0 "Near Reference") instead of an alias (type 8) -- APMF's 0x49 hook hands the
# package directly to the claimed actor, bypassing MFO's own alias/quest-priority
# arbitration entirely, so a follower package-locked by an outranking custom AI
# framework (the Cicero case) still gets walked to the loot. No QNAM: the
# Location input names no alias. See make_apmf_loot_travel_package().
FID_APMF_LOOT_TRAVEL_PACKAGE_0 = OWN | 0x836
FID_APMF_LOOT_TRAVEL_PACKAGE_1 = OWN | 0x837
FID_APMF_LOOT_TRAVEL_PACKAGE_2 = OWN | 0x838
FID_APMF_LOOT_TRAVEL_PACKAGE_3 = OWN | 0x839
# APMF RETREAT (ch.9 0x49 route): the flee-to-player counterpart of the four
# APMF loot-travel packages above, but only ONE record needed -- retreat's
# destination is ALWAYS the player (unlike loot's per-slot corpse), so a
# single shared package covers every follower fleeing concurrently with no
# per-follower target collision (see make_apmf_retreat_package()).
FID_APMF_RETREAT_PACKAGE = OWN | 0x83A
# APMF ANIMATED HEAL (ch.9 0x49 route, OPT-IN bHealAnimPackage, default OFF): the
# forced-casting package (ENGINE_NOTES 0.17/0.21, "M9 PROVEN -- packages produce
# ANIMATED casts at a CHOSEN target") revived to ANIMATE follower heals, but
# delivered via APMF's 0x49 package-offer channel (the same OfferPackage route
# loot-travel/retreat use) instead of the alias/command-quest route the original
# M9 arc used -- so the AI DECIDES to cast the heal (animated), while APMF's hold
# does not stop locomotion and the cast facet never claims movement. TWO records,
# each a UseMagic (000504F5) instance whose Spell input the DLL sets at runtime
# (Packages.cpp SetPackageSpell), differing ONLY in the target slot:
#   _SELF   = t6 (self), NO QNAM  -- probe 6's field-proven-clean self shape (0.22)
#   _PLAYER = t0 -> the PLAYER (0x14) STATIC, NO QNAM -- probe 1's field-proven
#             t0->PlayerRef shape; the player is a fixed form so NO runtime target
#             write is needed (only the Spell input is mutated).
# Ally/runtime-actor heals are NOT animated here (they'd need a runtime t0-handle
# write, unproven) and stay on the byte-identical kInstant path. See
# make_apmf_heal_packages() / native/Packages.cpp HealAnimFill.
FID_APMF_HEAL_SELF_PACKAGE   = OWN | 0x83B
FID_APMF_HEAL_PLAYER_PACKAGE = OWN | 0x83C
NEXT_OBJECT_ID     = 0x903         # first never-used local id (0x836-0x839 = APMF loot-travel packages,
                                   # 0x83A = APMF retreat package, 0x83B/0x83C = APMF animated-heal
                                   # packages, 0x900-0x902 = P7 travel packages)

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

    # ── group 2 ────────────────────────────────────────────────────────────
    # 6. THE WARD, PROPERLY. Concentration + Self is only ever shipped with
    #    targType 6 (2/2 vanilla), and probe 4 failed because it used t0 -- an
    #    empty cell. t6 has never had a CLEAN test: rev 4 crashed with t6 AND
    #    with QNAM emitted after PKCU (0 of 2,109 vanilla), i.e. two novel axes
    #    at once. The ordering is fixed globally now, so this is t6 alone.
    #    No QNAM: nothing here names an alias.
    (6, 0x000E8449, "WardOnSelf_t6",       ("t6", 0)),

    # 7. THE MAGICKA QUESTION. Thunderbolt costs 343 -- expensive enough that
    #    consumption cannot be ambiguous. Rides probe 5's PROVEN shape (t4 ->
    #    alias 1) so the only new variable is the price tag. If he casts it
    #    repeatedly on a warrior's magicka pool, package casts are FREE and
    #    §5.3's competence gate is decorative for this actuator.
    (7, 0x0010F7EE, "ThunderboltAtAlias1", ("t4", 1)),

    # 8. CONCENTRATION, THE SAFE WAY. HealingHands is Concentration +
    #    TargetActor, cost 25, and it HEALS -- so it tests the concentration
    #    axis at a t0 reference, a cell vanilla DOES ship under ALPC, without
    #    going anywhere near the t6 cell that crashed. If this casts,
    #    concentration works under alias delivery and probe 6's outcome only
    #    decides whether SELF-delivery concentration is separately reachable.
    (8, 0x0004D3F2, "HealingHandsAtPlayer", ("t0", FREF_PLAYER)),
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

# Command-quest priority. See make_command_quest() -- this is the #69 dial.
# 60 outranks the follower framework (measured 50, ENGINE_NOTES 0.25) so an
# alias-claimed follower runs MFO's package.
QUEST_PRIORITY = int(os.environ.get("MFO_QUEST_PRIORITY", "60"))

# Loot quest STATIC priority -- its OWN dial, NOT the command-quest #69 knob
# above (an MFO_QUEST_PRIORITY override for a command experiment must not silently
# break loot travel). Both default 60. The v0.8.4-v0.8.7 dynamic scheme (ship 25,
# raise to 60 at runtime) was deck-DISPROVEN: the WALK diagnostic showed prio
# reads 60 but the follower stays on his PlayerFollowerPackage, because the engine
# locks an actor's owning quest when the ALIAS IS FILLED and a later priority bump
# never re-arbitrates (ENGINE_NOTES 0.36). So MFO claims at fill time (static 60)
# and RELEASES by evicting the follower from the alias, never by touching the number.
LOOT_PRIORITY = int(os.environ.get("MFO_LOOT_PRIORITY", "60"))

# Retreat-probe quest STATIC priority -- its own dial for the same reason the
# loot dial is separate. 60, claim-at-fill, release-by-eviction: the 0.36
# measurement (alias-claim drives arbitration, runtime priority flips never
# re-arbitrate) applies verbatim, so this quest copies the loot quest's claim
# model rather than re-deriving one.
RETREAT_PRIORITY = int(os.environ.get("MFO_RETREAT_PRIORITY", "60"))

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
    granted = record('KYWD', FID_GRANTED_KYWD, 0, body)
    # §18.6 ADDON API (v1.1): the registration sentinel is RETIRED from MFO.esp.
    # An add-on now self-declares with its OWN keyword (editor-id suffix
    # "_MFOAddonManifest") as its manifest FLST's first entry — no MFO.esp
    # reference, no MFO.esp master (the Vortex fix). Contract: Docs/ADDON-API.md.
    return group('KYWD', granted)


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
    # VMAD MUST precede FULL: the vanilla QUST subrecord order (dumped from
    # Skyrim.esm) is EDID, VMAD, FULL, DNAM, ... The game loads any order, but
    # xEdit validates strictly against the record definition and errors/crashes
    # on EDID FULL VMAD.
    body = subrec('EDID', zstr("MFO_MCMQuest")) + subrec('VMAD', vmad.build()) + subrec('FULL', zstr("MFO MCM"))
    # kEnabled | kStartsEnabled, deliberately NOT run-once: SkyUI cannot
    # re-register a run-once quest.
    body += subrec('DNAM', qust_dnam(0x0011)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_MCM_QUEST, 0, body)



# ── M9: the command quest + its alias ───────────────────────────────────────
def make_trade_quest():
    # #21 econ bridge. Start-game-enabled, NOT run-once (so it re-registers on an
    # existing save via the SEQ, exactly like the MCM quest). Carries MFO_Trade
    # (extends Quest) with zero VMAD properties -- the DLL drives it by dispatching
    # RunTrade(token) to this quest's handle, and MFO_Trade pulls the follower /
    # vendor / lists back through MFO-registered Papyrus natives. No alias needed:
    # everything the script touches comes from the token'd TradeOrder in native.
    vmad = VMADBuilder()
    vmad.add_script("MFO_Trade", [])
    # VMAD before FULL — vanilla QUST order EDID, VMAD, FULL, DNAM (xEdit strict).
    body = subrec('EDID', zstr("MFO_TradeQuest")) + subrec('VMAD', vmad.build()) + subrec('FULL', zstr("MFO Trade"))
    body += subrec('DNAM', qust_dnam(0x0011)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', FID_TRADE_QUEST, 0, body)


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
    # PRIORITY LIVES HERE -- QUST DNAM byte 2, authored into the record. Not
    # load order, not the DLL. Vanilla spreads it deliberately: default 30,
    # DialogueFollower 50, scene quests 80-96.
    #
    # 60 was chosen to outrank DialogueFollower. It may also be what CAUSES
    # #69: MFO's quest wins arbitration and then supplies nothing when no alias
    # package is valid, so the follower stands still. Overridable for exactly
    # that test -- MFO_QUEST_PRIORITY=25 puts MFO below the follower quest.
    body += subrec('DNAM', qust_dnam(0x0011, priority=QUEST_PRIORITY))
    body += subrec('NEXT', b'')
    # ANAM (next alias id) goes BEFORE the alias blocks. All 1,607 vanilla
    # quests that have aliases do it that way -- 1607 before, 0 after. The
    # generator's own doctrine is to mirror vanilla order exactly rather than
    # rely on the engine's Load() tolerating a reordering.
    body += subrec('ANAM', struct.pack('<I', 3))   # aliases 0,1,2 (2 = self-cast)

    # ── alias 0: the actor MFO is commanding, and the package it carries ──
    # Flags: Optional (0x02) so the quest starts unfilled, Allow Reuse In Quest
    # (0x08) because MFO refills it every action, Allow Reserved (0x200) so a
    # follower another quest has reserved -- which is EVERY framework follower --
    # can still be taken.
    body += subrec('ALST', struct.pack('<I', 0))
    body += subrec('ALID', zstr("MFO_CommandActor"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    # NO AUTHORED FILL AT ALL -- this is DialogueFollower's shape (000750BA),
    # whose Follower/Animal aliases have no ALFR, no ALUA and no conditions:
    # bare Optional slots with packages on ALPC, filled and cleared from code.
    # Bethesda's own follower system, running in every playthrough.
    #
    # The DLL fills alias 0 with TESQuest::ForceRefTo when it wants an action
    # and clears it when the action ends. That is the only mechanism that can
    # claim a follower ON DEMAND -- alias fill is bound to quest promotion, so
    # nothing authored in the record can do it (§0.26).
    if POC_ENABLED:
        # PROBES ONLY. The ungated production package would sit at the top of
        # the ALPC priority stack and shadow every gated probe below it.
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

    # ── alias 2: the SELF-CAST carrier (SPEC-self-cast-forced). ──
    # Same DialogueFollower shape as alias 0 -- a bare Optional slot the DLL
    # fills on demand -- but its ALPC is MFO_CastPackageSelf (t6 self, NO QNAM,
    # §0.22's proven-clean probe-6 shape), NOT the foe package. A follower is
    # only ever in alias 0 (foe cast) OR alias 2 (self cast), never both, so
    # each alias still carries exactly one package -- no ALPC arbitration, the
    # same "one package per alias" discipline loot/retreat keep. Self needs no
    # target alias: t6 aims the caster at himself, authored in the record.
    body += subrec('ALST', struct.pack('<I', 2))
    body += subrec('ALID', zstr("MFO_CommandSelfActor"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    if not POC_ENABLED:
        # Probe builds keep alias 2 bare (the probe ladder rides alias 0); a
        # release build carries the self package here.
        body += subrec('ALPC', struct.pack('<I', FID_CAST_PACKAGE_SELF))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')
    return record('QUST', FID_COMMAND_QUEST, 0, body)


def make_loot_quest():
    """Option A: the delivery route for the TRAVEL package -- walk a follower to
    a loot ref, so looting means GOING to the item, not teleport-transfer.

    Same DialogueFollower-derived shape as make_command_quest, but a SEPARATE
    quest so its alias 0 carries ONLY the travel package -- no ALPC arbitration
    against the cast package (a follower is only ever loot-travelling OR
    cast-commanded, never both, so two one-package quests beat one two-package
    alias that needs conditions to disambiguate). alias 0 = the follower;
    alias 1 = the loot ref the DLL fills, which the travel package's PLDT t8
    points at.

    STATIC priority 60 (matches the working command quest). v0.8.4-v0.8.7 shipped
    this at 25 and RAISED it to 60 at runtime -- the deck WALK diagnostic proved
    that fails: prio reads 60 but the follower stays on his PlayerFollowerPackage
    (onTravelPkg=false), because the engine locks in an actor's owning quest when
    the ALIAS IS FILLED and a later priority bump does NOT re-arbitrate. So we
    fill the alias with priority ALREADY 60 (MFO wins the claim -> travels), and
    RELEASE by evicting the follower from the alias (dropping the number would not
    un-claim him either). Rooting is BOUNDED, not impossible: alias 1 is filled
    before alias 0 (no claim without a destination), and the only claimed-with-no-
    destination state is a corpse deleted mid-leg -- caught on the next serviced
    tick (Holding -> retarget or evict, worst case one fBatchLinger), and combat
    overrides it regardless (0.24/0.25/0.34/0.36).
    """
    body  = subrec('EDID', zstr("MFO_LootQuest"))
    body += subrec('FULL', zstr("MFO Loot"))
    body += subrec('DNAM', qust_dnam(0x0011, priority=LOOT_PRIORITY))
    body += subrec('NEXT', b'')
    body += subrec('ANAM', struct.pack('<I', 8))   # P7: 4 loot slots x (actor + target)

    # P7 multi-follower excursions: four (actor, target) alias PAIRS -- one slot
    # per concurrent looter. INTERLEAVED so slot 0 is aliases 0/1, byte-identical
    # to the shipped single-slot ESP (the DLL's old kAliasLootActor/Target 0/1
    # still resolve). Even alias 2k = the follower, carrying slot k's travel
    # package (each package walks to its OWN target alias). Odd alias 2k+1 = the
    # loot ref, no package, DLL-filled at runtime with the legality-gated ref.
    FNAM = struct.pack('<I', 0x0002 | 0x0008 | 0x0200)   # Optional | ... | AllowReserved
    packages = [FID_TRAVEL_PACKAGE, FID_TRAVEL_PACKAGE_1, FID_TRAVEL_PACKAGE_2, FID_TRAVEL_PACKAGE_3]
    for slot in range(4):
        suffix = "" if slot == 0 else str(slot)          # slot 0 keeps the shipped names
        # actor alias (2*slot): the follower, carrying this slot's travel package
        body += subrec('ALST', struct.pack('<I', 2 * slot))
        body += subrec('ALID', zstr("MFO_LootActor" + suffix))
        body += subrec('FNAM', FNAM)
        body += subrec('ALPC', struct.pack('<I', packages[slot]))
        body += subrec('VTCK', struct.pack('<I', 0))
        body += subrec('ALED', b'')
        # target alias (2*slot+1): the loot ref the slot's package PLDT t8 points at
        body += subrec('ALST', struct.pack('<I', 2 * slot + 1))
        body += subrec('ALID', zstr("MFO_LootTarget" + suffix))
        body += subrec('FNAM', FNAM)
        body += subrec('VTCK', struct.pack('<I', 0))
        body += subrec('ALED', b'')
    return record('QUST', FID_LOOT_QUEST, 0, body)


def make_retreat_quest():
    """RETREAT PROBE: the delivery route for a travel-to-PLAYER package that
    carries kIgnoreCombat -- the record half of the question "can an alias
    Travel package pull a follower AWAY from a live combat controller?".

    Byte-for-byte the make_loot_quest shape (the shipped, deck-proven claim
    model): STATIC priority 60, claim by filling alias 0 (the follower) while
    the priority is ALREADY 60, release by EVICTING him from the alias
    (force-fill the player) -- never a runtime priority flip, never a package
    condition (ENGINE_NOTES 0.24/0.25/0.36). alias 1 is the destination and the
    DLL fills it with the PLAYER before alias 0, same no-rooting order as loot.

    ONE package on the alias, its own quest -- same reasoning as loot: a
    one-package quest needs no ALPC arbitration or conditions.

    START-GAME-ENABLED, NOT run-once, and therefore MUST be in the SEQ -- a
    missing SEQ entry means the quest never starts on an existing save and
    every fill declines QuestStopped-style with no other symptom.
    """
    body  = subrec('EDID', zstr("MFO_RetreatQuest"))
    body += subrec('FULL', zstr("MFO Retreat"))
    body += subrec('DNAM', qust_dnam(0x0011, priority=RETREAT_PRIORITY))
    body += subrec('NEXT', b'')
    body += subrec('ANAM', struct.pack('<I', 2))

    # ── alias 0: the follower, carrying the retreat-travel package ──
    body += subrec('ALST', struct.pack('<I', 0))
    body += subrec('ALID', zstr("MFO_RetreatActor"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    body += subrec('ALPC', struct.pack('<I', FID_RETREAT_PACKAGE))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')

    # ── alias 1: the destination (the DLL fills it with the player). ──
    body += subrec('ALST', struct.pack('<I', 1))
    body += subrec('ALID', zstr("MFO_RetreatTarget"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')
    return record('QUST', FID_RETREAT_QUEST, 0, body)


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


def alias_fill_ctda():
    """CTDA: GetGlobalValue(MFO_ProbeSelect) > 0 -- gates the ALIAS FILL itself.

    THE POINT: with the global at 0 the alias does not fill, so MFO's quest
    never CLAIMS the follower, so he behaves entirely normally. #69/§0.25 --
    arbitration is by which quest's alias claims the actor, so an EMPTY alias
    is the only way to be out of the way.

    Without this the PoC is untestable: a permanently-filled alias at priority
    60 with no valid package roots the follower, and a rooted follower cannot
    walk into the fight the movement test needs.

    Precedent: 42 vanilla aliases gate an ALFR specific-reference fill with
    conditions (JailQuest's prison chests among them).

    op 0x40 = Greater than. Same byte layout as probe_ctda.
    """
    return subrec('CTDA', struct.pack('<B3xfHHIIIIi',
                                      0x40, 0.0, 74, 0,
                                      FID_PROBE_GLOB, 0, 0, 0, -1))


def build_usemagic(fid, edid, spell, target, bounds, ctda=b'', qnam=None, waiver=None):
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
        # PRODUCTION SELF-CAST (waiver 't6-self'): §0.22 proved probe 6's t6 +
        # NO-QNAM self-cast casts cleanly and REVOKED #67 -- t6 was never the
        # crash, the QNAM was. This record IS that proven shape, so it ships.
        # It stays FENCED: t6 forbids a QNAM (the record names no alias), and the
        # DLL keeps the route behind bCastSelf until the production path (arbitrary
        # self spell, in combat, via alias fill/evict) is deck-confirmed
        # (Docs/SPEC-self-cast-forced.md).
        if waiver == 't6-self':
            if qnam is not None:
                raise SystemExit(f"REFUSED {edid}: a t6 self record must carry NO "
                                 "QNAM -- a QNAM on a record whose inputs name no "
                                 "alias is the rev-4 crash cell (ENGINE_NOTES 0.22)")
        # The 't6' probe waiver is a PROBE instrument -- that is how precedent
        # gets made -- and must never reach a release build.
        elif waiver == 't6':
            if not POC_ENABLED:
                raise SystemExit(f"REFUSED {edid}: a 't6' probe waiver is a PROBE "
                                 "instrument and must never reach a release build.")
        else:
            raise SystemExit(f"REFUSED {edid}: targType 6 in the target slot of an "
                         "alias-delivered package without a waiver -- ENGINE_NOTES "
                         "0.22 (the QNAM, not the t6, was the rev-4 crash)")
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
    # PLDT: type 12 (no location) roots the actor -- field-confirmed, he stops
    # even in combat. MFO_PLDT_PLAYER=1 swaps in Ancano's shape instead:
    # type 0 "near reference", the PLAYER, radius 500. 20 vanilla UseMagic
    # instances use type 0 with a radius; whether that frees movement or merely
    # means "walk there first" is the open question (§0.27).
    if os.environ.get("MFO_PLDT_PLAYER") == "1":
        body += pack_input("Location", 'PLDT', struct.pack('<IiI', 0, FREF_PLAYER, 500))
    else:
        body += pack_input("Location", 'PLDT', struct.pack('<IiI', 12, 0, 10000))
    body += pack_input("TargetSelector", 'PTDA', struct.pack('<IIi', 1, spell, 0))
    if tkind == 't0':
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 0, tval, 0))
    elif tkind == 't6':
        # SELF. Byte shape verified from WCollegePracticeCastWard (00064B17).
        # This branch did not exist: the waiver removed the REFUSAL while the
        # emission path was still two-way, so ('t6', 0) silently became
        # t4 -> alias 0 -- the union of two known-bad shapes. A guard doing
        # double duty as "no precedent" AND "not implemented" hides the second.
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 6, 0, 0))
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


def make_cast_self_package():
    """The FORCED SELF-CAST package (Docs/SPEC-self-cast-forced.md): cast the
    chosen spell ON THE FOLLOWER HIMSELF.

    BYTE-IDENTICAL to make_cast_package EXCEPT the target slot: t6 (self) with
    NO QNAM, instead of t4 -> alias 1 with QNAM. That is the whole difference,
    and it is the whole point: §0.22 proved probe 6's t6 + no-QNAM self ward
    CASTS cleanly and REVOKED #67 -- the rev-4 crash was the QNAM (a QNAM on a
    record whose inputs name no alias), not the t6. The shipped MFO_CastPackage
    carries a QNAM (it aims at alias 1), so it can never serve self without
    writing t6 into a QNAM-carrying record at runtime -- the crash cell. This
    dedicated record is authored t6/no-QNAM statically, so the DLL never writes
    a targType at runtime for self; it only points the Spell input.

    The self target needs no alias (t6 IS the caster), so this record has no
    QNAM and no alias-1 dependency. Delivered by command-quest alias 2. BOUNDS_FF
    matches the foe package; the real per-stream bound (concentration hold,
    heal-topped, linger) is enforced by the DLL's CastHold + InterruptCast,
    exactly as it is for the foe cast.
    """
    return build_usemagic(FID_CAST_PACKAGE_SELF, "MFO_CastPackageSelf",
                          FREF_SEED_SPELL, ('t6', 0), BOUNDS_FF,
                          waiver='t6-self')


def build_travel(fid, edid, alias_idx, radius, qnam, pkdt_flags=0x00002000, runtime_target=False):
    """One PACK instance riding vanilla Travel (00016FAA): walk to the ref an
    alias holds, then stop. Byte shape mirrored VERBATIM from the shipped
    alias-delivered exemplar VC01FalionAtSummoningCircle (0010FF16) -- PLDT
    type 8 -> alias, QNAM naming the owner quest -- the 263-instance WERoad
    movement pattern (ENGINE_NOTES 0.17/0.20).

    The Travel template (00016FAA) has 3 inputs: 0 Location (PLDT), 2/4 the two
    Bools (RideHorseIfPossible, PreferPreferredPath); the settable UNAM run is
    0/2/4 and XNAM is 3 (dumped from Skyrim.esm, not guessed).
    """
    body  = subrec('EDID', zstr(edid))
    # PKDT flags = 0x00002000 = PREFERRED SPEED ENABLE. NOT kIgnoreCombat, so a
    # fight can still pull the follower off looting (the "no logistics during
    # combat" rule holds -- ignore-combat is a different, higher bit).
    #
    # GAIT is byte 6 = preferredSpeed (enum 0=Walk, 1=Jog, 2=Run, 3=FastWalk),
    # set to 2 = Run so the follower hustles over and rejoins, not strolls.
    # ("Walk-to-loot" names the move, not the gait.)
    #
    # v0.8.2 SHIPPED byte6=2 with flags=0 and the follower still WALKED (deck:
    # ~89 u/s, walk speed). ROOT CAUSE: preferredSpeed is INERT unless the
    # 0x2000 flag is set -- and the VC01-exemplar comment had mislabelled 0x2000
    # as "AlwaysSneak" and deliberately cleared it. Proven by scanning all 5,961
    # Skyrim.esm PACK records: WITHOUT 0x2000, byte6 is 2 in 4,386/4,502 (the
    # inert default); WITH 0x2000 it spreads 124/282/703/350 across Walk/Jog/Run/
    # FastWalk. So 0x2000 is Preferred-Speed-enable, full stop. interruptFlags
    # 0x0054 and the byte tail otherwise verbatim from the exemplar.
    #
    # pkdt_flags default 0x00002000 keeps the shipped loot-travel record
    # byte-identical (<I of 0x00002000 == the old literal '00200000'). The
    # RETREAT PROBE passes 0x00102000 = kIgnoreCombat (0x00100000, the same bit
    # build_usemagic authors on the combat-capable cast package, verbatim from
    # TG08BMercerCombatOverrideCastAtBrynjolf 000FCC26) | 0x2000 preferred-speed
    # enable. Tail '1200028054000000' = type 18, byte6=2 (Run), interruptFlags
    # 0x0054 -- unchanged in both.
    body += subrec('PKDT', struct.pack('<I', pkdt_flags) + bytes.fromhex('1200028054000000'))
    # PSDT: any time, any day -- the same 3,855-of-5,961 default build_usemagic uses.
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    # NO CTDA. The package is unconditional -- it runs whenever the loot quest's
    # alias claims the follower. RELEASE is done by QUEST PRIORITY, not a package
    # condition (see make_loot_quest / native LootTravelClear): a gate that only
    # invalidates the PACKAGE leaves the follower claimed-with-nothing and ROOTS
    # him (ENGINE_NOTES 0.24/0.25, deck-measured). To release we drop the quest
    # BELOW the follower framework so the framework reclaims him.
    # QNAM (owner quest) is MANDATORY for an alias-valued input (626/626 vanilla) --
    # and ONLY for one: runtime_target's Location is type 0 (a live ref, never an
    # alias), so it names no alias and needs no QNAM (qnam is None for that call).
    assert (qnam is None) == runtime_target, \
        "QNAM is mandatory for the alias-valued Location (runtime_target=False) " \
        "and forbidden for the runtime-handle Location (runtime_target=True)"
    if qnam is not None:
        body += subrec('QNAM', struct.pack('<I', qnam))
    body += subrec('PKCU', struct.pack('<III', 3, FREF_TMPL_TRAVEL, 3))
    if runtime_target:
        # APMF LOOT-TRAVEL route: PLDT type 0 ("Near Reference", RE::PackageLocation
        # ::Type::kNearReference) authored with a placeholder non-null ref -- 0 of
        # 4,048 vanilla type-0 PLDTs ship a null target (see the MFO_PLDT_PLAYER
        # experiment above) -- FREF_PLAYER, harmless, since the DLL always
        # overwrites PackageLocation::data.refHandle with the real loot ref before
        # ever claiming the actor via APMFBridge::OfferPackage (Packages.cpp
        # SetAPMFLootTravelTarget). UNVERIFIED IN THE FIELD pending the Cicero
        # test: the read/write shape (BGSPackageDataLocation, pointer at
        # IPackageData*+0x10, PackageLocation::data.refHandle) is cross-checked
        # against the public CommonLibSSE-NG headers and MFO's own prior
        # PackageTarget precedent (SetInputs, the analogous PTDA route), not yet
        # field-observed for a Location input specifically.
        body += pack_input("Location", 'PLDT', struct.pack('<IiI', 0, FREF_PLAYER, radius))
    else:
        # input 0: Location -> PLDT type 8 (reference alias) / alias index / radius.
        body += pack_input("Location", 'PLDT', struct.pack('<III', 8, alias_idx, radius))
    # inputs 2 and 4: the two Bools, both false -- as every WERoad travel ships.
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    body += pack_input("Bool", 'CNAM', struct.pack('<B', 0))
    # settable-slot run 0/2/4, then XNAM 3 -- verbatim from the exemplar.
    body += subrec('UNAM', struct.pack('<B', 0))
    body += subrec('UNAM', struct.pack('<B', 2))
    body += subrec('UNAM', struct.pack('<B', 4))
    body += subrec('XNAM', struct.pack('<B', 3))
    # Empty on-begin/end/change blocks, as vanilla ships them.
    for blk in ('POBA', 'POEA', 'POCA'):
        body += subrec(blk, b'')
        body += subrec('INAM', struct.pack('<I', 0))
        body += subrec('PDTO', struct.pack('<II', 0, 0))
    return record('PACK', fid, 0, body)


def make_travel_package():
    """Option A's loot-travel package: walk to the ref in MFO_LootQuest alias 1.
    Radius 128 (~arm's reach) so the engine stops the follower ON the loot; the
    DLL then detects arrival by distance and runs the existing inventory transfer.
    """
    # P7: one travel package per loot slot. Interleaved alias layout keeps slot 0
    # on aliases 0/1 (byte-identical to the shipped single-slot ESP); slots 1-3
    # each name their own target alias (3/5/7). Only the FormID, EDID and target
    # alias index differ between them.
    return (build_travel(FID_TRAVEL_PACKAGE,   "MFO_TravelPackage",  alias_idx=1, radius=128, qnam=FID_LOOT_QUEST)
          + build_travel(FID_TRAVEL_PACKAGE_1, "MFO_TravelPackage1", alias_idx=3, radius=128, qnam=FID_LOOT_QUEST)
          + build_travel(FID_TRAVEL_PACKAGE_2, "MFO_TravelPackage2", alias_idx=5, radius=128, qnam=FID_LOOT_QUEST)
          + build_travel(FID_TRAVEL_PACKAGE_3, "MFO_TravelPackage3", alias_idx=7, radius=128, qnam=FID_LOOT_QUEST))


def make_apmf_loot_travel_package():
    """APMF ch.9 (0x49 package-offer) loot-travel packages -- ONE per concurrent
    loot slot (kMaxLootSlots), mirroring make_travel_package()'s per-slot
    instances but with the RUNTIME-HANDLE route (runtime_target=True) instead of
    an alias: APMF's 0x49 hook hands this package directly to the actor holding
    a live kIntent_OfferPackage claim naming it, bypassing MFO's own alias/
    priority arbitration entirely -- the fix for a follower package-locked by an
    outranking custom AI framework (the Cicero case), whose own package would
    otherwise beat MFO_LootQuest's static priority 60. No QNAM (no alias-valued
    input); radius/gait identical to the alias route's slots.
    """
    ids = (FID_APMF_LOOT_TRAVEL_PACKAGE_0, FID_APMF_LOOT_TRAVEL_PACKAGE_1,
           FID_APMF_LOOT_TRAVEL_PACKAGE_2, FID_APMF_LOOT_TRAVEL_PACKAGE_3)
    out = b''
    for i, fid in enumerate(ids):
        out += build_travel(fid, f"MFO_APMFLootTravelPackage{i}", alias_idx=0,
                            radius=128, qnam=None, runtime_target=True)
    return out


def make_retreat_package():
    """RETREAT PROBE: walk to the ref in MFO_RetreatQuest alias 1 (the player),
    IGNORING COMBAT. Identical to the shipped loot-travel record except:
      * PKDT general flags 0x00102000 -- kIgnoreCombat (0x00100000) on top of
        the preferred-speed enable (0x2000). kIgnoreCombat is the bit all six
        vanilla fight-during-combat UseMagic instances set and the cast package
        already ships; this is its first pairing with Travel in MFO.
      * radius 150 (~at the player's side), not 128 (~on the corpse).
    Byte 6 stays 2 (Run) -- a retreat at a stroll would be ambiguous data.
    """
    return build_travel(FID_RETREAT_PACKAGE, "MFO_RetreatPackage",
                        alias_idx=1, radius=150, qnam=FID_RETREAT_QUEST,
                        pkdt_flags=0x00102000)


def make_apmf_retreat_package():
    """APMF ch.9 (0x49 package-offer) retreat package -- the flee-to-player
    counterpart of make_apmf_loot_travel_package(), routing act.flee (and the
    opt-in auto-retreat leash safety, which shares native RetreatFill) through
    APMF's 0x49 hook instead of MFO_RetreatQuest's alias/static-priority-60
    race -- the SAME fix for a follower package-locked by an outranking custom
    AI framework (the Cicero case). Unlike loot's per-slot corpses, retreat's
    destination is ALWAYS the player, so ONE record covers every follower
    (no per-follower runtime-target collision the way 4 loot slots guard
    against). Authored with the SAME kIgnoreCombat + preferred-speed flags
    (0x00102000) and radius (150) as the legacy MFO_RetreatPackage above --
    identical behaviour, different delivery channel. RUNTIME-HANDLE Location
    (PLDT type 0, "Near Reference") like the loot-travel packages: 0x49
    delivers with no alias fill, so there is no alias to carry the target.
    The authored placeholder (FREF_PLAYER, via build_travel's runtime_target
    branch) already IS the correct permanent target for retreat specifically
    (unlike loot's per-corpse case) -- the DLL's runtime write at engage time
    (Packages.cpp's SetAPMFLootTravelTarget, reused verbatim) is a defensive
    reassertion, not a real per-follower retarget. No QNAM (no alias-valued
    input, runtime-handle route).
    """
    return build_travel(FID_APMF_RETREAT_PACKAGE, "MFO_APMFRetreatPackage",
                        alias_idx=0, radius=150, qnam=None, runtime_target=True,
                        pkdt_flags=0x00102000)


def make_apmf_heal_packages():
    """APMF ch.9 (0x49 package-offer) ANIMATED-HEAL packages (OPT-IN
    bHealAnimPackage, default OFF).

    The M9 FORCED-CASTING PACKAGE (ENGINE_NOTES 0.17/0.21), revived to make a
    follower's OWN AI cast a HEAL with a real animation -- the mechanism offense
    already gets for free (AI-discretionary) but heals never did, because the AI
    won't CHOOSE to self/party-heal, so MFO force-applies via kInstant (no anim,
    0.15/0.16). The original M9 arc abandoned the package route because the
    alias/command-quest delivery TOOK OVER the follower (lost movement + other
    hand); APMF's 0x49 package-offer channel fixes that -- it hands the package
    directly to the claimed actor while its hold does NOT stop locomotion.

    TWO records, both UseMagic (000504F5, the same template make_cast_package
    rides), each with its Spell input set at runtime by the DLL
    (Packages.cpp SetPackageSpell) and a STATICALLY-authored target -- so, unlike
    the loot/retreat runtime-handle Location write, NO unproven runtime target
    mutation is needed:

      _SELF   -- t6 (self), NO QNAM. Probe 6's field-proven-clean shape (0.22,
                 which REVOKED #67): a t6 + no-QNAM self record casts cleanly.
                 Byte-identical to make_cast_self_package's target slot.
      _PLAYER -- t0 -> the PLAYER (FREF_PLAYER 0x14), STATIC, NO QNAM. Probe 1's
                 field-proven t0->PlayerRef shape (0.21). The player is a fixed
                 form, authored at generation time -- the DLL only writes the
                 Spell input, never a target.

    Both carry the SAME kIgnoreCombat PKDT + BOUNDS_FF as make_cast_package, so
    they cast during a fight exactly as the proven foe cast package does. Ally /
    runtime-actor heals are deliberately NOT covered here (that needs a runtime
    t0-handle target write, which is unproven) -- they stay on the kInstant path.
    """
    body  = build_usemagic(FID_APMF_HEAL_SELF_PACKAGE, "MFO_APMFHealSelfPackage",
                           FREF_SEED_SPELL, ('t6', 0), BOUNDS_FF, waiver='t6-self')
    body += build_usemagic(FID_APMF_HEAL_PLAYER_PACKAGE, "MFO_APMFHealPlayerPackage",
                           FREF_SEED_SPELL, ('t0', FREF_PLAYER), BOUNDS_FF)
    return body


def make_poc_packages():
    """The probe ladder -- see POC_PROBES. One axis of novelty per probe,
    exactly one valid at a time via the MFO_ProbeSelect gate."""
    out = b''
    for idx, sp, label, (tkind, tval) in POC_PROBES:
        if tkind == 't0' and tval is None:
            tval = POC_ACTOR_REF
        bounds = BOUNDS_CONC if sp in (0x000E8449, 0x0004D3F2) else BOUNDS_FF
        # t6 names no alias, so no QNAM -- see build_usemagic's guard.
        out += build_usemagic(
            FID_POC_PACK_BASE + idx - 1, f"MFO_PoC{idx}_{label}",
            sp, (tkind, tval), bounds,
            ctda=probe_ctda(idx),
            qnam=FID_COMMAND_QUEST if tkind == 't4' else None,
            # Probe 6 exists to TEST the refused shape, cleanly, once: rev 4
            # crashed with t6 AND with QNAM misordered, so t6 alone has never
            # been tried. If it casts, the guard is wrong and #67 relaxes; if
            # it crashes, the guard is proven and stays forever.
            waiver='t6' if tkind == 't6' else None)
    return out


def make_pack():
    body = make_cast_package()
    body += make_cast_self_package()   # SPEC-self-cast-forced: t6 self, no QNAM
    if POC_ENABLED:
        body += make_poc_packages()
    body += make_travel_package()
    body += make_apmf_loot_travel_package()
    body += make_apmf_retreat_package()
    body += make_apmf_heal_packages()   # OPT-IN animated heal (bHealAnimPackage)
    body += make_retreat_package()
    return group('PACK', body)


# ── CSTY: MFO's combat styles (stance ownership) ───────────────────────────
def _csty_record(edid, fid, csgd_floats, cscr):
    """One CSTY record. Byte shape MIRRORED from vanilla humanoid styles (dumped
    from Skyrim.esm per doctrine, never format docs): EDID, CSGD 40 bytes (10
    floats, the CommonLibSSE-NG CombatStyleGeneralData layout), CSME 28 bytes
    (7 floats -- the on-disk record carries one float FEWER than the runtime
    struct's 8; mirror the disk, the loader zero-fills), CSCR 16, CSLR 4, CSFL 28
    (same 7-of-8 truncation), DATA 4 (flags, 1 = dueling).

    TWO axes vary between MFO's styles: the CSGD weapon-SCORING (which weapon the
    style prefers) AND the CSCR close-range POSITIONING (`cscr`, 16 raw bytes taken
    byte-verbatim from the MATCHING vanilla style -- see make_csty). Everything
    else stays byte-verbatim csHumanMagic. CSCR must be per-style: it was verbatim
    csHumanMagic for all three, so a follower forced to MELEE kept the mage's low
    circle / high fallback and drifted backwards instead of closing (marth: battle
    pathfinding "worse")."""
    assert len(cscr) == 16, "CSCR is 16 bytes (circleMult, fallbackMult, flankDistance, stalkTime)"
    csgd = struct.pack('<10f', *csgd_floats)
    csme = struct.pack('<7f', 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0)
    cslr = struct.pack('<f', 0.2)
    csfl = struct.pack('<7f', 0.33, 1.0, 0.5, 0.5, 0.5, 0.5, 0.5)
    body = (subrec('EDID', zstr(edid))
            + subrec('CSGD', csgd) + subrec('CSME', csme) + subrec('CSCR', cscr)
            + subrec('CSLR', cslr) + subrec('CSFL', csfl)
            + subrec('DATA', struct.pack('<I', 1)))
    return record('CSTY', fid, 0, body)


def make_csty():
    """MFO's three combat styles, ONE CSTY group. The DLL swaps one onto a
    follower's LIVE per-combat CombatController (offset 0x38 -- never the base
    record) so the engine stops fighting MFO over which weapon the follower
    holds; the swap reverts with the controller at battle end (CombatStyle.cpp).

    CSGD field order: offensiveMult, defensiveMult, groupOffensiveMult,
    meleeScoreMult, magicScoreMult, rangedScoreMult, shoutScoreMult,
    unarmedScoreMult, staffScoreMult, avoidThreatChance.

    - MFO_CastStyle  (0x832, P1 probe, bProbeCastStyle-gated): magic dominant.
    - MFO_MeleeStyle (0x833): melee dominant, ranged/magic starved,
      avoidThreatChance 0 -- swapped in when act.equip_melee wins the hand so an
      archer's own AI stops re-drawing the bow and instead closes and swings.
    - MFO_RangedStyle(0x834): ranged dominant, avoidThreatChance up -- swapped in
      for act.equip_ranged so a follower (even a mage handed a bow) kites/shoots.
    The stance follows the winning EQUIP gambit, not the follower's class, so
    "make my mage melee" / "make my mage shoot" fall out for free."""
    # CSCR (close-range positioning) taken BYTE-VERBATIM from the MATCHING vanilla
    # humanoid style, dumped from Skyrim.esm (doctrine: mirror the disk). Order is
    # circleMult, fallbackMult, flankDistance, stalkTime. The old code used the
    # csHumanMagic values (0.3/0.5/0.2/0.2) for ALL THREE, so a forced-melee
    # follower kept the mage's low circle + high fallback and drifted backwards
    # instead of closing -- the "battle pathfinding worse" report. Now each style
    # positions like its vanilla counterpart:
    CSCR_MAGE   = bytes.fromhex('9a99993e0000003fcdcc4c3ecdcc4c3e')  # csHumanMagic     0003BE1C: circle 0.30 / fallback 0.50
    CSCR_MELEE  = bytes.fromhex('48e13a3f3d0ad73ecdcc4c3ecdcc4c3e')  # csHumanMeleeLvl1 0003BE1B: circle 0.73 / fallback 0.42
    CSCR_RANGED = bytes.fromhex('6666e63e6666263fcdcc4c3ecdcc4c3e')  # csHumanMissile   0003BE1D: circle 0.45 / fallback 0.65
    cast   = _csty_record("MFO_CastStyle",   FID_CAST_STYLE,
                          (1.0, 0.5, 1.0, 0.1, 10.0, 0.2, 1.0, 0.1, 1.0, 0.2), CSCR_MAGE)
    melee  = _csty_record("MFO_MeleeStyle",  FID_MELEE_STYLE,
                          (1.0, 0.5, 1.0, 10.0, 0.1, 0.1, 1.0, 0.1, 0.1, 0.0), CSCR_MELEE)
    ranged = _csty_record("MFO_RangedStyle", FID_RANGED_STYLE,
                          (1.0, 0.5, 1.0, 0.1, 0.1, 10.0, 1.0, 0.1, 0.1, 0.5), CSCR_RANGED)
    return group('CSTY', cast + melee + ranged)


def make_qust():
    return group('QUST', make_startup_quest() + make_mcm_quest()
                 + make_command_quest() + make_loot_quest() + make_retreat_quest()
                 + make_trade_quest())


# ═══════════════════════════════════════════════════════════════════════════
# MFO_Progression.esl — the follower-progression addon (component 2).
# A SEPARATE plugin, generated beside MFO.esp: detection anchor + the
# CONFIG/DATA AUTHORITY the allocator reads (economy GLOBs, per-class
# auto-pick FormLists). The DLL reads every GLOB's RECORD DEFAULT at
# kDataLoaded (design doc §10 — GLOB values are save-persisted, so a
# post-load read could see a stale saved number). All numbers here are
# author-tunable in xEdit without touching the DLL.
#
# FormID band: FROZEN generator<->DLL contract with native/Progression.h
# (0x800/0x801) and native/ProgAllocator.h (everything else). §18.6: the ESL
# now masters TWO plugins — Skyrim.esm (index 0x00) and MFO.esp (index 0x01),
# the latter so the manifest FLST can point at MFO.esp's addon sentinel — so
# the ESL's OWN forms move to master index 0x02 (OWN_PROG). ESL-legal locals
# stay 0x800-0xFFF. The DLL resolves everything by (localID, plugin name), so
# the prefix shift is invisible to it; the co-save stores runtime FormIDs.
# ═══════════════════════════════════════════════════════════════════════════

OWN_PROG = 0x01000000                  # ESL own-form prefix: 1 master (Skyrim.esm) => index 0x01
# v1.1 Vortex fix: the addon no longer masters MFO.esp. Its manifest self-
# declares via its OWN keyword (PGID_MANIFEST_KYWD, editor-id "_MFOAddonManifest"
# suffix), so there is NO cross-master reference and Skyrim.esm is the sole
# master. Own-form prefix shifts 0x02 -> 0x01. Runtime FormIDs are FE-prefixed
# (light plugin) regardless, so co-saves resolve unchanged (LookupAddonForm by
# {plugin, localID}); only the file-internal master index moved.

PROG_VERSION_STAMP = 1.0               # MFOP_Version FLTV — the addon version

PGID_VERSION            = OWN_PROG | 0x800  # GLOB detection anchor + version stamp
PGID_RESERVED           = OWN_PROG | 0x801  # GLOB spare future gate (design §10)
# §18.6 Stage 3: 0x802 REPURPOSED — was the unread MFOP_PerkPointsPerLevel
# (§17 legacy); now MFOP_LevelsPerPerkPoint, the perk divisor floor(level/N).
PGID_LEVELS_PER_PERK    = OWN_PROG | 0x802  # GLOB perk divisor (1 point / N levels; default 2)
PGID_SKILL_PER_LEVEL    = OWN_PROG | 0x803  # GLOB auto-scale skill points per level (default 2)
PGID_SHARED_DIVISOR     = OWN_PROG | 0x804  # GLOB benched growth divisor (2 = half rate, §15)
PGID_RESPEC_RAPPORT     = OWN_PROG | 0x805  # GLOB respec cost in rapport (§15: 500)
PGID_VETERAN_MULT       = OWN_PROG | 0x806  # GLOB veteran catch-up multiplier (UNREAD legacy, §17)
PGID_SKILL_CAP          = OWN_PROG | 0x807  # GLOB auto-scale base-AV ceiling
PGID_DEV_CMD            = OWN_PROG | 0x808  # GLOB dev-harness verb selector (console: set MFOP_DevCmd to N)
PGID_MANUAL_SKILL       = OWN_PROG | 0x809  # GLOB manual skill points / level (§16; default 2)
# 0x80A-0x80F reserved: more economy knobs
PGID_SKILLS_MELEE       = OWN_PROG | 0x810  # FLST ordered AVIF skill priority per class
PGID_SKILLS_RANGED      = OWN_PROG | 0x811
PGID_SKILLS_MAGE        = OWN_PROG | 0x812
# 0x813-0x817 reserved: more classes / attribute lists
PGID_PERKS_MELEE        = OWN_PROG | 0x818  # FLST ordered PERK priority per class — SHIPPED
PGID_PERKS_RANGED       = OWN_PROG | 0x819  #      EMPTY (this plugin can only master
PGID_PERKS_MAGE         = OWN_PROG | 0x81A  #      Skyrim.esm, and overhauls replace the
                                            #      perks); an overhaul patch fills them in
                                            #      xEdit, the DLL falls back name-agnostic
# 0x81B-0x81F reserved: more perk lists
PGID_ENROLLED_KYWD      = OWN_PROG | 0x820  # KYWD enrollment tag — RESERVED for
                                            #      conditions/SPID use; v1 DLL does not
                                            #      stamp it (no probe data for base
                                            #      keyword-array writes)
# §18.6 the ADDON MANIFEST — ONE FLST the DLL enumerates; entry[0] = the MFO.esp
# sentinel keyword, entry[1] = the classes-list FLST (Stage 2), entries[2..] =
# every economy GLOB the DLL reads (Stage 3, PROG_MANIFEST_ECONOMY, matched by
# editor-id suffix). The DLL type-dispatches manifest entries: the sentinel is
# skipped, the ONE FLST is the classes list, each GLOB is an economy knob.
PGID_MANIFEST           = OWN_PROG | 0x821
# v1.1 SELF-DECLARATION KEYWORD — the add-on's OWN join key (editor-id ends
# "_MFOAddonManifest"). Manifest FLST entry[0]; the DLL enumerates every FLST
# whose front form is a keyword with this edid suffix (keyword editor-ids
# PERSIST at runtime, unlike GLOB/FLST edids). Replaces the retired MFO.esp
# sentinel — no cross-master reference (the Vortex fix).
PGID_MANIFEST_KYWD      = OWN_PROG | 0x822
# v1.1 residual #3 — the ENTRY-POINT VERDICTS self-declaration keyword. The
# add-on's perk-effectiveness verdicts (effective/marginal/dead per BGSEntryPoint
# index) are ADD-ON DATA now, not a DLL table. They ride a sub-FLST whose FIRST
# entry is THIS keyword (editor-id ends "_MFOEntryPointVerdicts" — keyword edids
# persist at runtime, the same reason the manifest keyword is matched by suffix),
# followed by 92 POSITIONAL GLOBs (index = entry-point index, value = 0 effective
# / 1 marginal / 2 dead). Referenced from the manifest FLST so the DLL ties the
# verdicts to this add-on. Delete the ESL and the DLL holds ZERO verdicts.
PGID_VERDICTS_KYWD      = OWN_PROG | 0x823  # KYWD MFOP_MFOEntryPointVerdicts

# §18.6 Stage 2 — N-DECLARED CLASSES. The manifest points at ONE classes-list
# FLST; its entries are class-def FLSTs. Each class-def FLST declares: ONE MESG
# (display name = its FULL), its AVIF skills (order = weight), an OPTIONAL PERK
# priority list (shipped empty), and ONE GLOB whose editor id ends "_Stance"
# (the #65 combat-stance mirror, 1=Melee 2=Ranged 3=Mage). New frozen band.
PGID_CLASSNAME_MELEE    = OWN_PROG | 0x830  # MESG display name
PGID_CLASSNAME_RANGED   = OWN_PROG | 0x831
PGID_CLASSNAME_MAGE     = OWN_PROG | 0x832
# v1.1 Phase 6c: the hosted board-tab TITLE, self-declared as a MESG (FULL =
# "Progression"). Referenced from the manifest FLST so the DLL captures its FULL
# into the generic manifest (boardTab.label) and the Field-Orders board titles
# the tab from it — no "Progression" string in the DLL. Same MESG shape as a
# class display name (the reason it sits in this band).
PGID_BOARDTAB_LABEL     = OWN_PROG | 0x833  # MESG board-tab title
# 0x834-0x83F reserved: more class display names
PGID_STANCE_MELEE       = OWN_PROG | 0x840  # GLOB _Stance mirror (1 = Melee)
PGID_STANCE_RANGED      = OWN_PROG | 0x841  #                     (2 = Ranged)
PGID_STANCE_MAGE        = OWN_PROG | 0x842  #                     (3 = Mage/Cast)
# v1.1 §HMS class ratios lifted OUT of the DLL (HmsProfile) into manifest DATA.
# Per class-def FLST, 4 GLOBs appended after its _Stance (POSITIONAL — GLOB
# editor-ids are discarded at runtime, so the DLL identifies them by ORDER):
# [+0]=weight Health, [+1]=weight Magicka, [+2]=weight Stamina, [+3]=primary
# pool (0=H 1=M 2=S). Band 0x843-0x84E (4 per class × 3 classes).
PGID_HMS_BASE           = OWN_PROG | 0x843  # Melee H,M,S,primary = 0x843-0x846
                                            # Ranged = 0x847-0x84A, Mage = 0x84B-0x84E
PGID_CLASSDEF_MELEE     = OWN_PROG | 0x850  # FLST class-def (MESG + AVIF + PERK + _Stance)
PGID_CLASSDEF_RANGED    = OWN_PROG | 0x851
PGID_CLASSDEF_MAGE      = OWN_PROG | 0x852
# 0x853-0x85E reserved: more class-def FLSTs
PGID_CLASSES            = OWN_PROG | 0x85F  # FLST classes list — manifest entry[1]
# v1.1 residual #3 — the entry-point VERDICTS sub-FLST (manifest entry[3]): its
# entry[0] is PGID_VERDICTS_KYWD, then the 92 POSITIONAL verdict GLOBs in entry-
# point-index order. The DLL identifies it by its keyword front (not the classes
# list), reads it via Progression::ReadEntryPointVerdicts. No DLL default.
PGID_VERDICTS_FLST      = OWN_PROG | 0x860
# 92 verdict GLOBs — one per BGSEntryPoint index, POSITIONAL (GLOB edids are
# discarded at runtime, so ORDER carries the index; the HMS-weights idiom). Band
# 0x900-0x95B. Value = the perk-effectiveness VERDICT (0 effective/1 marginal/
# 2 dead) — the judgment lifted OUT of the DLL (native/Progression.cpp) into data.
PGID_VERDICTS_GLOB_BASE = OWN_PROG | 0x900

# The addon's OWN MCM Helper quest — carries MFOP_MCM (extends MCM_ConfigBase),
# rendered from Data/MCM/Config/MFO_Progression/config.json. Start-game-enabled,
# NOT run-once, so it MUST ride the addon's own SEQ (SEQ/MFO_Progression.seq).
# Everything the tab needs ships in the ESL + its Scripts/config — MFO.esp and
# MFO.dll carry NO reference to it (the DLL discovers the economy GLOBs
# generically off the manifest, exactly as it would a third-party addon).
PGID_MCM_QUEST          = OWN_PROG | 0x870  # QUST MFOP_MCMQuest (MFOP_MCM script)

PROG_NEXT_OBJECT_ID     = 0x95C   # past the 92 verdict GLOBs (0x900-0x95B)

# Vanilla AVIF forms (Skyrim.esm) for the class-skill lists — DUMPED from the
# shipped master (doctrine: mirror the disk, never a wiki). AVOneHanded ..
# AVEnchanting sit in one contiguous run at 0x44C-0x45D.
AVIF_ONEHANDED   = 0x0000044C
AVIF_TWOHANDED   = 0x0000044D
AVIF_MARKSMAN    = 0x0000044E
AVIF_BLOCK       = 0x0000044F
AVIF_HEAVYARMOR  = 0x00000451
AVIF_LIGHTARMOR  = 0x00000452
AVIF_SNEAK       = 0x00000455
AVIF_ALTERATION  = 0x00000458
AVIF_CONJURATION = 0x00000459
AVIF_DESTRUCTION = 0x0000045A
AVIF_RESTORATION = 0x0000045C
# The dump shows NO "AVIllusion" record: vanilla reuses the Morrowind-era
# Mysticism slot — AVIF 0x45B keeps EDID AVMysticism but IS the Illusion
# skill (FULL renamed in game; it sits at the kIllusion position in the
# 0x458-0x45D school run). The DLL maps list entries through the LIVE
# ActorValueList (GetActorValue(kIllusion)->GetFormID()), so a load order
# that moves the skill still maps — and an entry that maps to nothing is
# skipped with a named warn, never guessed.
AVIF_ILLUSION    = 0x0000045B

# Class skill priority, ORDER = weight (triangular over the DLL's post-prune
# list; with both weapon and both armor siblings listed, the DLL keeps the
# follower's DOMINANT one — see ProgAllocator.cpp WeightsFor). Melee prunes to
# the design's exact 40/30/20/10.
PROG_CLASS_SKILLS = [
    (PGID_SKILLS_MELEE,  "MFOP_ClassSkills_Melee",
     [AVIF_ONEHANDED, AVIF_TWOHANDED, AVIF_HEAVYARMOR, AVIF_LIGHTARMOR, AVIF_BLOCK, AVIF_MARKSMAN]),
    (PGID_SKILLS_RANGED, "MFOP_ClassSkills_Ranged",
     [AVIF_MARKSMAN, AVIF_LIGHTARMOR, AVIF_SNEAK, AVIF_ONEHANDED]),
    (PGID_SKILLS_MAGE,   "MFOP_ClassSkills_Mage",
     [AVIF_DESTRUCTION, AVIF_ALTERATION, AVIF_RESTORATION, AVIF_CONJURATION, AVIF_ILLUSION]),
]

# (fid, edid, default, FNAM type). Type 'f' (float) on the fractional
# MULTIPLIER knobs — auto skill/lvl and the shared-growth divisor — so an
# author can set 0.5/2.5-style values in the CK/xEdit (a short-typed GLOB
# floors them in the editor UI). The DLL reads g->value (a float) either way,
# so no DLL change rides this. Whole-number knobs (the perk divisor and the
# manual rate are level COUNTS, respec/cap are whole) and the version/dev
# stamps stay 's' (short), the vanilla quest-control shape.
# §18.6 Stage 3: the DLL matches every economy GLOB by editor-id SUFFIX (the
# trailing "_LevelsPerPerkPoint", "_SkillPointsPerLevel", … part), NOT by
# fixed local id — the editor id here is the CONTRACT; the local id only has
# to be a stable, unique own-form id. Defaults (marth 2026-08-17): skill and
# manual rates → 2, perk divisor → 2.
PROG_GLOBS = [
    (PGID_VERSION,        "MFOP_Version",                 PROG_VERSION_STAMP, 's'),
    (PGID_RESERVED,       "MFOP_Reserved",                0.0,   's'),
    (PGID_LEVELS_PER_PERK,"MFOP_LevelsPerPerkPoint",      2.0,   's'),
    (PGID_SKILL_PER_LEVEL,"MFOP_SkillPointsPerLevel",     5.0,   'f'),
    (PGID_MANUAL_SKILL,   "MFOP_ManualSkillPointsPerLevel",5.0,  's'),
    (PGID_SHARED_DIVISOR, "MFOP_SharedGrowthDivisor",     2.0,   'f'),
    (PGID_RESPEC_RAPPORT, "MFOP_RespecRapportCost",       500.0, 's'),
    (PGID_VETERAN_MULT,   "MFOP_VeteranCatchupMult",      1.0,   'f'),
    (PGID_SKILL_CAP,      "MFOP_SkillCap",                100.0, 's'),
    (PGID_DEV_CMD,        "MFOP_DevCmd",                  0.0,   's'),
]

# §18.6 Stage 3: the economy GLOBs the DLL reads — APPENDED to the addon
# MANIFEST FLST (after entry[1] the classes-list) so the DLL enumerates them
# off the manifest, never by fixed id. Order here is the manifest entry order
# (last-writer-wins is per-manifest anyway, and this addon is the sole one).
PROG_MANIFEST_ECONOMY = [
    PGID_LEVELS_PER_PERK, PGID_SKILL_PER_LEVEL, PGID_MANUAL_SKILL,
    PGID_SHARED_DIVISOR, PGID_RESPEC_RAPPORT, PGID_SKILL_CAP, PGID_DEV_CMD,
]

PROG_PERK_LISTS = [
    (PGID_PERKS_MELEE,  "MFOP_ClassPerks_Melee"),
    (PGID_PERKS_RANGED, "MFOP_ClassPerks_Ranged"),
    (PGID_PERKS_MAGE,   "MFOP_ClassPerks_Mage"),
]

# §18.6 Stage 2 — the N-declared classes, as the reference addon ships them
# (the worked API example). Each row → ONE class-def FLST + its MESG name +
# its _Stance GLOB. The classes-list FLST (PGID_CLASSES) references the three
# class-def FLSTs IN THIS ORDER — and that order is load-bearing: PRGN v<3
# saves stored a fixed ordinal (1=Melee 2=Ranged 3=Mage) that the DLL migrates
# to the k-th declared class, so Melee/Ranged/Mage MUST stay rows 0/1/2. The
# AVIF skill orders mirror PROG_CLASS_SKILLS exactly (order = weight); the
# stance value mirrors the old combatClassOverride ordinal.
# v1.1: each row also declares its HMS profile as DATA (weights H/M/S + primary
# pool 0=H/1=M/2=S), lifted verbatim from the DLL's old hardcoded HmsProfile
# switch (Melee 60/5/35 primary Health; Ranged 40/5/55 primary Stamina; Mage
# 15/80/5 primary Magicka). Weights are whole percentages (the DLL model holds
# them raw; normalization is the consumer's job). Emitted as the 4 POSITIONAL
# GLOBs appended to the class-def FLST after _Stance (PGID_HMS_BASE band).
#   (defFid, defEdid, nameFid, nameEdid, display, stanceFid, stanceEdid, stanceVal,
#    [AVIF skills], [hmsH, hmsM, hmsS], primaryPool)
PROG_CLASSES = [
    (PGID_CLASSDEF_MELEE,  "MFOP_ClassDef_Melee",  PGID_CLASSNAME_MELEE,  "MFOP_ClassName_Melee",
     "Melee",  PGID_STANCE_MELEE,  "MFOP_ClassMelee_Stance",  1,
     [AVIF_ONEHANDED, AVIF_TWOHANDED, AVIF_HEAVYARMOR, AVIF_LIGHTARMOR, AVIF_BLOCK, AVIF_MARKSMAN],
     [60, 5, 35], 0),
    (PGID_CLASSDEF_RANGED, "MFOP_ClassDef_Ranged", PGID_CLASSNAME_RANGED, "MFOP_ClassName_Ranged",
     "Ranged", PGID_STANCE_RANGED, "MFOP_ClassRanged_Stance", 2,
     [AVIF_MARKSMAN, AVIF_LIGHTARMOR, AVIF_HEAVYARMOR, AVIF_SNEAK, AVIF_ONEHANDED],
     [40, 5, 55], 2),
    (PGID_CLASSDEF_MAGE,   "MFOP_ClassDef_Mage",   PGID_CLASSNAME_MAGE,   "MFOP_ClassName_Mage",
     "Mage",   PGID_STANCE_MAGE,   "MFOP_ClassMage_Stance",   3,
     [AVIF_DESTRUCTION, AVIF_ALTERATION, AVIF_RESTORATION, AVIF_CONJURATION, AVIF_ILLUSION],
     [15, 80, 5], 1),
]

# v1.1 residual #3 — the PERK-EFFECTIVENESS VERDICTS, now ADD-ON DATA. This is
# the effective/marginal/dead judgment the DLL used to compile in (the old
# native/Progression.cpp kEntryPoints[92] table); it lives HERE in the add-on
# generator now, and the DLL carries no default. One row per BGSEntryPoint index
# (0-91, engine-frozen order), verdict 'E' effective / 'M' marginal / 'D' dead.
# Emitted as 92 POSITIONAL GLOBs (value 0/1/2) in PGID_VERDICTS_GLOB_BASE order,
# referenced by the verdicts sub-FLST. Values reproduce the shipped table exactly
# (the board's perk-effectiveness display is unchanged). Combat/defense = E;
# lockpick/craft/commerce/player-UI = D; the named marginal set (+ unproven-on-
# NPC calls) = M (flagged, never silently killed, so over-filtering stays
# diagnosable). The engine enum NAMES stay a general fact in the DLL; only these
# verdicts moved. Editor-ids are informational (the DLL reads them by ORDER).
VERDICT_CODE = {'E': 0, 'M': 1, 'D': 2}
PROG_ENTRYPOINT_VERDICTS = [
    "CalculateWeaponDamage E", "CalculateMyCriticalHitChance E",
    "CalculateMyCriticalHitDamage E", "CalculateMineExplodeChance D",
    "AdjustLimbDamage E", "AdjustBookSkillPoints D", "ModRecoveredHealth E",
    "GetShouldAttack M", "ModBuyPrices D", "AddLeveledListOnDeath D",
    "GetMaxCarryWeight M", "ModAddictionChance D", "ModAddictionDuration D",
    "ModPositiveChemDuration D", "Activate D", "IgnoreRunningDuringDetection E",
    "IgnoreBrokenLock D", "ModEnemyCriticalHitChance E", "ModSneakAttackMult E",
    "ModMaxPlaceableMines D", "ModBowZoom D", "ModRecoverArrowChance M",
    "ModSkillUse D", "ModTelekinesisDistance M", "ModTelekinesisDamageMult M",
    "ModTelekinesisDamage M", "ModBashingDamage E", "ModPowerAttackStamina E",
    "ModPowerAttackDamage E", "ModSpellMagnitude E", "ModSpellDuration E",
    "ModSecondaryValueWeight M", "ModArmorWeight M", "ModIncomingStagger E",
    "ModTargetStagger E", "ModAttackDamage E", "ModIncomingDamage E",
    "ModTargetDamageResistance E", "ModSpellCost E", "ModPercentBlocked E",
    "ModShieldDeflectArrowChance E", "ModIncomingSpellMagnitude E",
    "ModIncomingSpellDuration E", "ModPlayerIntimidation D", "ModPlayerReputation D",
    "ModFavorPoints D", "ModBribeAmount D", "ModDetectionLight E",
    "ModDetectionMovement E", "ModSoulGemRecharge D", "SetSweepAttack E",
    "ApplyCombatHitSpell E", "ApplyBashingSpell E", "ApplyReanimateSpell E",
    "SetBooleanGraphVariable M", "ModSpellCastingSoundEvent D",
    "ModPickpocketChance D", "ModDetectionSneakSkill E", "ModFallingDamage E",
    "ModLockpickSweetSpot D", "ModSellPrices D", "CanPickpocketEquippedItem D",
    "ModLockpickLevelAllowed D", "SetLockpickStartingArc D", "SetProgressionPicking D",
    "MakeLockpicksUnbreakable D", "ModAlchemyEffectiveness D", "ApplyWeaponSwingSpell E",
    "ModCommandedActorLimit M", "ApplySneakingSpell E", "ModPlayerMagicSlowdown D",
    "ModWardMagickaAbsorptionPct E", "ModInitialIngredientEffectsLearned D",
    "PurifyAlchemyIngredients D", "FilterActivation D", "CanDualCastSpell M",
    "ModTemperingHealth D", "ModEnchantmentPower D", "ModSoulPctCapturedToWeapon D",
    "ModSoulGemEnchanting D", "ModNumberAppliedEnchantmentsAllowed D",
    "SetActivateLabel D", "ModShoutOK D", "ModPoisonDoseCount D",
    "ShouldApplyPlacedItem M", "ModArmorRating E", "ModLockpickingCrimeChance D",
    "ModIngredientsHarvested D", "ModSpellRange_TargetLoc M", "ModPotionsCreated D",
    "ModLockpickingKeyRewardChance D", "AllowMountActor M",
]
assert len(PROG_ENTRYPOINT_VERDICTS) == 92, \
    f"entry-point verdict table must be 92 rows, is {len(PROG_ENTRYPOINT_VERDICTS)}"


def make_prog_tes4(esl=True):
    # v1.1: ONE master — Skyrim.esm (index 0x00) for the AVIF skill forms. The
    # add-on no longer masters MFO.esp: its manifest self-declares via its OWN
    # keyword (PGID_MANIFEST_KYWD), so there is NO cross-master reference. That
    # kills the ESL-masters-ESP load-order cycle Vortex choked on (the whole
    # reason the .esp "Vortex variant" existed). Own-form prefix = 0x01.
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', PROG_NEXT_OBJECT_ID)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("marth"))
    body += subrec('SNAM', zstr("MFO follower-progression addon (optional; detected at runtime)"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    # ESL flag (0x200) for the light MFO_Progression.esl; CLEARED for the regular
    # MFO_Progression.esp. With no MFO.esp master the ESL is now Vortex-clean, so
    # the .esp variant is redundant (kept emitted for continuity; a later phase
    # may drop it). Both carry identical records + 0x01-prefixed own form ids.
    return record('TES4', 0, 0x00000200 if esl else 0, body)


def prog_glob(fid, edid, value, fnam):
    # The make_glob shape (mirrored from vanilla quest-control globals):
    # EDID + FNAM type char ('s' short / 'f' float — FLTV stores a float32
    # either way; FNAM only tells the editor how to present it) + FLTV.
    assert fnam in ('s', 'f'), f"{edid}: FNAM must be 's' or 'f'"
    body = subrec('EDID', zstr(edid)) + subrec('FNAM', fnam.encode('ascii'))
    body += subrec('FLTV', struct.pack('<f', float(value)))
    return record('GLOB', fid, 0, body)


def prog_flst(fid, edid, forms):
    # FLST shape dumped from Skyrim.esm (CWMission07StewardVoiceTypes
    # 00017334): EDID then one LNAM (u32 form) per entry. An EMPTY list is
    # legal — the shipped perk-priority lists are exactly that.
    body = subrec('EDID', zstr(edid))
    for f in forms:
        body += subrec('LNAM', struct.pack('<I', f))
    return record('FLST', fid, 0, body)


def prog_mesg(fid, edid, full):
    # §18.6 Stage 2: a class-def's display name. MESG shape dumped from
    # Skyrim.esm (a minimal message needs EDID + FULL; DESC is the body text,
    # DNAM the flags). The DLL reads ONLY the FULL (BGSMessage::GetFullName);
    # DESC mirrors it and DNAM=0 marks it a plain message (not a message box),
    # so it never pops on screen. Strings are inline (this plugin is not
    # localized), ASCII-only like every other zstr here.
    # Vanilla MESG subrecord order (dumped from Skyrim.esm, all 40 records):
    # EDID, DESC, FULL, INAM, DNAM. DESC comes BEFORE FULL, INAM is a 4-byte
    # icon formid (0), DNAM is a 4-byte UInt32 flags. The old EDID FULL DESC DNAM
    # order (FULL before DESC, no INAM) made xEdit error + evarianttypecasterror.
    body = subrec('EDID', zstr(edid))
    body += subrec('DESC', zstr(full))
    body += subrec('FULL', zstr(full))
    body += subrec('INAM', struct.pack('<I', 0))   # icon formid, unused
    body += subrec('DNAM', struct.pack('<I', 0))   # flags: 0 = not a message box
    return record('MESG', fid, 0, body)


def make_prog_mcm_quest():
    # The addon's OWN MCM Helper quest — same shape as MFO.esp's MFO_MCMQuest
    # (make_mcm_quest), mirrored into the ESL. Start-game-enabled, NOT run-once
    # (SkyUI cannot re-register a run-once quest); zero VMAD properties, MCM
    # Helper renders from Data/MCM/Config/MFO_Progression/config.json and derives
    # modName from this plugin's stem. The VMAD script attach is just a name
    # string, so it needs no new master (the ESL masters only Skyrim.esm). MUST
    # be listed in SEQ/MFO_Progression.seq or it never starts on
    # an existing save.
    vmad = VMADBuilder()
    vmad.add_script("MFOP_MCM", [])
    # VMAD before FULL — vanilla QUST order EDID, VMAD, FULL, DNAM (xEdit strict).
    body = subrec('EDID', zstr("MFOP_MCMQuest")) + subrec('VMAD', vmad.build())
    body += subrec('FULL', zstr("MFO Progression"))
    body += subrec('DNAM', qust_dnam(0x0011)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', PGID_MCM_QUEST, 0, body)


def make_progression_esl(esl=True):
    data = make_prog_tes4(esl)
    # Top-group order mirrors Skyrim.esm's relative order: KYWD < GLOB < QUST
    # < FLST < MESG (QUST sorts before FLST in the shipped master).
    kywd_enrolled = record('KYWD', PGID_ENROLLED_KYWD, 0,
                           subrec('EDID', zstr("MFOP_Enrolled")) + subrec('CNAM', struct.pack('<I', 0)))
    # v1.1 self-declaration keyword — the add-on's OWN join key (edid suffix
    # "_MFOAddonManifest"). The DLL enumerates every FLST whose front form is a
    # keyword with this suffix; keyword editor-ids persist at runtime. No MFO.esp
    # reference — this is what retires the sentinel and unblocks Vortex.
    kywd_manifest = record('KYWD', PGID_MANIFEST_KYWD, 0,
                           subrec('EDID', zstr("MFOP_MFOAddonManifest")) + subrec('CNAM', struct.pack('<I', 0)))
    # v1.1 residual #3 — the verdicts sub-FLST's self-declaration keyword (edid
    # suffix "_MFOEntryPointVerdicts"). Same KYWD shape as the manifest keyword.
    kywd_verdicts = record('KYWD', PGID_VERDICTS_KYWD, 0,
                           subrec('EDID', zstr("MFOP_MFOEntryPointVerdicts")) + subrec('CNAM', struct.pack('<I', 0)))
    data += group('KYWD', kywd_enrolled + kywd_manifest + kywd_verdicts)
    glob_body = b''
    for fid, edid, value, fnam in PROG_GLOBS:
        glob_body += prog_glob(fid, edid, value, fnam)
    # §18.6 Stage 2: each class's #65 combat-stance mirror (whole-number GLOB).
    for _df, _de, _nf, _ne, _disp, stanceFid, stanceEdid, stanceVal, _sk, _hw, _pp in PROG_CLASSES:
        glob_body += prog_glob(stanceFid, stanceEdid, float(stanceVal), 's')
    # v1.1 §HMS class ratios as DATA — 4 POSITIONAL GLOBs per class (H,M,S,primary)
    # appended after its _Stance in the class-def FLST (below). Editor-ids are
    # informational only (the DLL reads them by order, not by suffix).
    for ci, (_df, _de, _nf, _ne, _disp, _sf, _se, _sv, _sk, hw, pp) in enumerate(PROG_CLASSES):
        base = PGID_HMS_BASE + ci * 4
        glob_body += prog_glob(base + 0, f"{_de}_HmsWeightH", float(hw[0]), 's')
        glob_body += prog_glob(base + 1, f"{_de}_HmsWeightM", float(hw[1]), 's')
        glob_body += prog_glob(base + 2, f"{_de}_HmsWeightS", float(hw[2]), 's')
        glob_body += prog_glob(base + 3, f"{_de}_HmsPrimary", float(pp),    's')
    # v1.1 residual #3 — the 92 POSITIONAL verdict GLOBs (index = entry-point
    # index, value = 0/1/2). Editor-ids are informational only (the DLL reads by
    # ORDER, like the HMS globs). Named for the engine entry point they verdict.
    for i, row in enumerate(PROG_ENTRYPOINT_VERDICTS):
        name, code = row.rsplit(' ', 1)
        glob_body += prog_glob(PGID_VERDICTS_GLOB_BASE + i,
                               f"MFOP_EPVerdict_{name}", float(VERDICT_CODE[code]), 's')
    data += group('GLOB', glob_body)
    # The addon's own MCM quest (its economy tab).
    data += group('QUST', make_prog_mcm_quest())
    flst_body = b''
    # §18.6 the addon MANIFEST — enumerated by the DLL; entry[0] is the add-on's
    # OWN self-declaration keyword (PGID_MANIFEST_KYWD, edid suffix
    # "_MFOAddonManifest" — NO MFO.esp reference), entry[1] the classes-list FLST
    # (Stage 2), then every economy GLOB the DLL reads (Stage 3).
    # v1.1 Phase 6c: PGID_BOARDTAB_LABEL (a MESG) precedes the classes list so
    # the DLL's manifest walk captures its FULL for the hosted board tab's title
    # before it stops on the classes FLST (BuildGenericManifests breaks there).
    # v1.1 residual #3: PGID_VERDICTS_FLST (the entry-point verdicts sub-FLST)
    # follows the classes list so the DLL's manifest walk breaks on the classes
    # FLST first; the verdicts sub-FLST is found by its keyword front (order-
    # independent) and read via Progression::ReadEntryPointVerdicts.
    flst_body += prog_flst(PGID_MANIFEST, "MFOP_AddonManifest",
                           [PGID_MANIFEST_KYWD, PGID_BOARDTAB_LABEL, PGID_CLASSES,
                            PGID_VERDICTS_FLST]
                           + PROG_MANIFEST_ECONOMY)
    for fid, edid, forms in PROG_CLASS_SKILLS:
        flst_body += prog_flst(fid, edid, forms)
    for fid, edid in PROG_PERK_LISTS:
        flst_body += prog_flst(fid, edid, [])
    # §18.6 Stage 2: the classes-list FLST (manifest entry[1]) → the three
    # class-def FLSTs in Melee/Ranged/Mage order (the PRGN v<3 migration
    # ordinal depends on this order). Each class-def references its MESG name,
    # its AVIF skills (order = weight), its _Stance GLOB, then the 4 POSITIONAL
    # HMS GLOBs (H,M,S,primary — v1.1 class ratios as data); the PERK priority is
    # shipped empty (an overhaul patch fills it in xEdit).
    flst_body += prog_flst(PGID_CLASSES, "MFOP_Classes",
                           [defFid for defFid, *_ in PROG_CLASSES])
    for ci, (defFid, defEdid, nameFid, _ne, _disp, stanceFid, _se, _sv, skills, _hw, _pp) in enumerate(PROG_CLASSES):
        hmsFids = [PGID_HMS_BASE + ci * 4 + k for k in range(4)]
        flst_body += prog_flst(defFid, defEdid, [nameFid] + skills + [stanceFid] + hmsFids)
    # v1.1 residual #3: the entry-point VERDICTS sub-FLST — entry[0] its
    # self-declaration keyword, then the 92 POSITIONAL verdict GLOBs in order.
    verdict_glob_fids = [PGID_VERDICTS_GLOB_BASE + i for i in range(len(PROG_ENTRYPOINT_VERDICTS))]
    flst_body += prog_flst(PGID_VERDICTS_FLST, "MFOP_EntryPointVerdicts",
                           [PGID_VERDICTS_KYWD] + verdict_glob_fids)
    data += group('FLST', flst_body)
    # MESG group last — mirrors Skyrim.esm's late top-group position (MESG
    # sorts after FLST/PERK/AVIF). One display-name message per class.
    mesg_body = b''
    for _df, _de, nameFid, nameEdid, disp, _sf, _se, _sv, _sk, _hw, _pp in PROG_CLASSES:
        mesg_body += prog_mesg(nameFid, nameEdid, disp)
    # v1.1 Phase 6c: the hosted board tab's self-declared title (its FULL is the
    # tab caption — no "Progression" literal in the DLL).
    mesg_body += prog_mesg(PGID_BOARDTAB_LABEL, "MFOP_BoardTabLabel", "Progression")
    data += group('MESG', mesg_body)
    return data


# ── the addon's OWN MCM (MCM Helper) — GlobalValue-bound economy sliders ──────
# The addon ships its own tab entirely inside the ESL + this config; MFO.esp /
# MFO.dll carry NO reference to it. Each slider binds sourceType "GlobalValue"
# to one economy GLOB by "MFO_Progression.esl|0xLOCALID" — moving the slider
# writes the GLOB's RUNTIME value, and the DLL re-reads it on MCM close
# (ProgAllocator::OnMenuClose, discovered generically off the manifest). No
# ModSetting store / Config.cpp wiring is involved (that is MFO.esp's MCM only).
# defaultValue mirrors each GLOB's record default in PROG_GLOBS.
#   (localFid, label, help, min, max, step, default)
PROG_MCM_PLUGIN = "MFO_Progression.esl"
PROG_MCM_SLIDERS = [
    (PGID_LEVELS_PER_PERK, "Levels per perk point",
     "Follower earns one perk point every N levels. Lower = more perks.",
     1, 10, 1, 2),
    (PGID_SKILL_PER_LEVEL, "Auto skill points per level",
     "Skill points auto-scaled onto a class follower each level (class builds). "
     "0 disables class auto-scaling.",
     0, 10, 1, 5),
    (PGID_MANUAL_SKILL, "Manual skill points per level",
     "Skill points a follower on MANUAL allocation earns each level (you spend "
     "them by hand for mage / multiclass builds). 0 disables the manual pool.",
     0, 10, 1, 5),
    (PGID_SHARED_DIVISOR, "Benched growth divisor",
     "A benched (not-following) follower grows at 1/N the rate. 1 = full rate, "
     "2 = half.",
     1, 10, 1, 2),
    (PGID_RESPEC_RAPPORT, "Respec rapport cost",
     "Rapport spent to respec a follower's perks.",
     0, 2000, 25, 500),
    (PGID_SKILL_CAP, "Skill cap",
     "Ceiling the auto-scaler may raise a follower's base skill to.",
     50, 100, 5, 100),
]


# INI key per slider (perk-bug fix 2026-08-17): the MCM binds to a ModSetting,
# NOT a GlobalValue. GlobalValue writes the GLOB's save-persisted runtime value,
# which a stale save then feeds back into the economy (the doubled-perk-pool
# bug). ModSetting persists to Data/MCM/Settings/MFO_Progression.ini — NOT in
# the save — and MFO.dll reads THAT as its live economy override (matched by the
# key TAIL, e.g. "iLevelsPerPerkPoint" -> LevelsPerPerkPoint), agnostic of the
# addon. The ":Economy" suffix is the INI section MCM Helper writes under.
PROG_MCM_KEYS = {
    PGID_LEVELS_PER_PERK: "iLevelsPerPerkPoint",
    PGID_SKILL_PER_LEVEL: "iSkillPointsPerLevel",
    PGID_MANUAL_SKILL:    "iManualSkillPointsPerLevel",
    PGID_SHARED_DIVISOR:  "iSharedGrowthDivisor",
    PGID_RESPEC_RAPPORT:  "iRespecRapportCost",
    PGID_SKILL_CAP:       "iSkillCap",
}


# ModSettingBool toggles, rendered after the sliders. Not GLOB-bound: the DLL
# reads them from the addon INI by key tail (ProgAllocator KeyEndsWith
# "CancelEngineAwards"), the same ApplyEconomyOverride path as the sliders.
# (marth 2026-08-20: revert engine awards so MFO's full 5/level award is not
# inflated by the engine's own per-level skill growth.)
#   (iniKey, label, help, default)
PROG_MCM_TOGGLES = [
    ("bCancelEngineAwards", "Cancel engine skill leveling",
     "ON (default): MFO reverts the engine's per-level skill gains and applies "
     "only its own class/manual award (pure MFO, no inflation). OFF: engine "
     "leveling stacks on top of MFO's award (compat).",
     1),
    # v1.1: §15 Shared Growth toggle moved OFF MFO.esp's MCM (was a hidden DLL
    # Config global) to here — the add-on owns its own progression policy. Read
    # by ApplyEconomyOverride (key tail "SharedGrowth"), same path as above.
    ("bSharedGrowth", "Shared growth (benched followers)",
     "ON (default): a benched follower banks the levels you gain and converts "
     "them at the benched-growth divisor (slow catch-up); an active follower "
     "earns at your rate. OFF: every enrolled follower matches your level "
     "outright, no catch-up cost.",
     1),
]


def prog_mcm_config():
    """The MFO_Progression config.json dict — one Economy page of ModSettingInt
    sliders (persist to the addon's own INI, which the DLL reads live). minMcmVersion 9."""
    content = [{"text": "Economy", "type": "header"}]
    for fid, label, help_, mn, mx, step, dflt in PROG_MCM_SLIDERS:
        content.append({
            "id": f"{PROG_MCM_KEYS[fid]}:Economy",   # control-level (MFO/MEO shape)
            "text": label,
            "type": "slider",
            "help": help_,
            "valueOptions": {
                "min": mn, "max": mx, "step": step,
                "sourceType": "ModSettingInt",
                "defaultValue": dflt,
            },
        })
    for key, label, help_, dflt in PROG_MCM_TOGGLES:
        content.append({
            "id": f"{key}:Economy",
            "text": label,
            "type": "toggle",
            "help": help_,
            "valueOptions": {
                "sourceType": "ModSettingBool",
                "defaultValue": dflt,
            },
        })
    return {
        "modName": "MFO_Progression",
        "displayName": "MFO — Follower Progression",
        "minMcmVersion": 9,
        "cursorFillMode": "topToBottom",
        "pages": [{
            "pageDisplayName": "Economy",
            "cursorFillMode": "topToBottom",
            "content": content,
        }],
    }


def write_prog_mcm_files(out_dir):
    """Emit Data/MCM/Config/MFO_Progression/{config.json,settings.ini}.
    settings.ini seeds the ModSetting defaults under [Economy] — the store MCM
    Helper writes and MFO.dll reads live (ApplyEconomyOverride)."""
    cdir = os.path.join(out_dir, 'MCM', 'Config', 'MFO_Progression')
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(cdir, 'config.json'), 'w') as f:
        json.dump(prog_mcm_config(), f, indent='\t')
    # [Economy] key=default for each ModSettingInt slider. MCM Helper rewrites
    # this as the player moves sliders; the DLL re-reads it on menu close. NOT
    # save-persisted (unlike a GlobalValue), so no stale-save economy.
    with open(os.path.join(cdir, 'settings.ini'), 'w', encoding='utf-8-sig') as f:
        f.write("[Economy]\n")
        for fid, label, help_, mn, mx, step, dflt in PROG_MCM_SLIDERS:
            f.write(f"{PROG_MCM_KEYS[fid]}={int(dflt)}\n")
        for key, label, help_, dflt in PROG_MCM_TOGGLES:
            f.write(f"{key}={int(dflt)}\n")
    return cdir


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_kywd()
    # GLOB between KYWD and MGEF -- vanilla top-group order (KYWD .. GLOB ..
    # MGEF), and only emitted when something references it (PoC probes).
    if POC_ENABLED:
        data += make_glob()
    data += make_mgef()
    data += make_spel()
    data += make_qust()
    data += make_pack()
    # CSTY after PACK -- vanilla's own top-group order (Skyrim.esm: ... QUST,
    # IDLE, PACK, CSTY, LSCR ...), verified by walking the shipped master.
    data += make_csty()

    out_path = os.path.join(out_dir, "MFO.esp")
    with open(out_path, 'wb') as f:
        f.write(data)

    # The progression addon — a SEPARATE, OPTIONAL plugin (detected at
    # runtime by the DLL; absent = feature off, never an error). It now carries
    # its OWN MCM quest, so it ALSO gets its own SEQ + MCM config (below).
    prog_path = os.path.join(out_dir, "MFO_Progression.esl")
    prog_data = make_progression_esl(esl=True)
    with open(prog_path, 'wb') as f:
        f.write(prog_data)
    # Vortex variant: the SAME records as a REGULAR .esp (ESL flag cleared), so a
    # light plugin no longer masters a regular ESP (that cycle is unfixable in
    # Vortex). Same 2 masters -> same 0x02 form-id prefix -> saves resolve via the
    # DLL's LookupAddonForm .esl<->.esp fallback. Ship as the optional "Vortex
    # version" until the v1.1 self-declarative rebuild.
    prog_esp_path = os.path.join(out_dir, "MFO_Progression.esp")
    with open(prog_esp_path, 'wb') as f:
        f.write(make_progression_esl(esl=False))

    # The addon's own MCM config (ModSetting economy sliders) — everything the
    # tab needs ships with the ESL; MFO.esp / MFO.dll stay ignorant of it.
    prog_mcm_dir = write_prog_mcm_files(out_dir)

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
        f.write(struct.pack('<I', FID_LOOT_QUEST))
        f.write(struct.pack('<I', FID_RETREAT_QUEST))
        f.write(struct.pack('<I', FID_TRADE_QUEST))

    # The addon's OWN SEQ (its MCM quest, same reasoning as MFO.seq).
    prog_seq_path = os.path.join(seq_dir, "MFO_Progression.seq")
    with open(prog_seq_path, 'wb') as f:
        f.write(struct.pack('<I', PGID_MCM_QUEST))

    print(f"MFO {VERSION}")
    print(f"Written: {out_path} ({len(data):,} bytes)")
    print(f"Written: {seq_path} (5 start-game-enabled quests: MCM, Command, Loot, Retreat, Trade)")
    print(f"Written: {prog_path} ({len(prog_data):,} bytes) — optional progression addon")
    print(f"Written: {prog_seq_path} (1 start-game-enabled quest: MFOP MCM)")
    print(f"Written: {prog_mcm_dir}/config.json + settings.ini "
          f"({len(PROG_MCM_SLIDERS)} ModSetting economy sliders)")
    print()
    print("Records:")
    print(f"  TES4  header     master: Skyrim.esm, ESL flagged, NEXT_OBJECT_ID 0x{NEXT_OBJECT_ID:03X}")
    print(f"  KYWD  0x{FID_GRANTED_KYWD & 0xFFF:03X}        MFO_GrantedSpell")
    print(f"  MGEF  0x{FID_ORDERS_MGEF & 0xFFF:03X}        MFO_FieldOrdersMGEF")
    print(f"  SPEL  0x{FID_ORDERS_SPELL & 0xFFF:03X}        MFO_FieldOrdersPower (lesser power)")
    print(f"  QUST  0x{FID_STARTUP_QUEST & 0xFFF:03X}        MFO_StartupQuest (run once, no VMAD)")
    print(f"  QUST  0x{FID_MCM_QUEST & 0xFFF:03X}        MFO_MCMQuest (MFO_MCM script)")
    print(f"  QUST  0x{FID_COMMAND_QUEST & 0xFFF:03X}        MFO_CommandQuest (3 aliases: foe carrier/foe target/self carrier, DLL-filled)")
    print(f"  QUST  0x{FID_LOOT_QUEST & 0xFFF:03X}        MFO_LootQuest (8 aliases = 4 loot slots, DLL-filled; static prio {LOOT_PRIORITY})")
    print(f"  QUST  0x{FID_RETREAT_QUEST & 0xFFF:03X}        MFO_RetreatQuest (2 aliases, DLL-filled; static prio {RETREAT_PRIORITY})")
    print(f"  QUST  0x{FID_TRADE_QUEST & 0xFFF:03X}        MFO_TradeQuest (MFO_Trade script; econ bridge)")
    print(f"  PACK  0x{FID_CAST_PACKAGE & 0xFFF:03X}        MFO_CastPackage -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X}"
          + ("  [NOT attached under POC]" if POC_ENABLED else ""))
    print(f"  PACK  0x{FID_CAST_PACKAGE_SELF & 0xFFF:03X}        MFO_CastPackageSelf -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X} (t6 self, NO QNAM; DLL bCastSelf-gated)")
    print(f"  PACK  0x{FID_TRAVEL_PACKAGE & 0xFFF:03X}        MFO_TravelPackage -> vanilla Travel {FREF_TMPL_TRAVEL:08X}")
    print(f"  PACK  0x{FID_APMF_LOOT_TRAVEL_PACKAGE_0 & 0xFFF:03X}-0x{FID_APMF_LOOT_TRAVEL_PACKAGE_3 & 0xFFF:03X}    "
          f"MFO_APMFLootTravelPackage0-3 -> vanilla Travel {FREF_TMPL_TRAVEL:08X} (runtime-handle Location, "
          f"ch.9 0x49 route, APMFBridge::OfferPackage)")
    print(f"  PACK  0x{FID_RETREAT_PACKAGE & 0xFFF:03X}        MFO_RetreatPackage -> vanilla Travel {FREF_TMPL_TRAVEL:08X} + kIgnoreCombat")
    print(f"  PACK  0x{FID_APMF_RETREAT_PACKAGE & 0xFFF:03X}        MFO_APMFRetreatPackage -> vanilla Travel {FREF_TMPL_TRAVEL:08X} + kIgnoreCombat "
          f"(runtime-handle Location, ch.9 0x49 route, APMFBridge::OfferPackage)")
    print(f"  PACK  0x{FID_APMF_HEAL_SELF_PACKAGE & 0xFFF:03X}        MFO_APMFHealSelfPackage -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X} "
          f"(t6 self, NO QNAM; OPT-IN bHealAnimPackage, ch.9 0x49 route)")
    print(f"  PACK  0x{FID_APMF_HEAL_PLAYER_PACKAGE & 0xFFF:03X}        MFO_APMFHealPlayerPackage -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X} "
          f"(t0 -> player, NO QNAM; OPT-IN bHealAnimPackage, ch.9 0x49 route)")
    print(f"  CSTY  0x{FID_CAST_STYLE & 0xFFF:03X}        MFO_CastStyle (P1 probe: caster-forward, bProbeCastStyle-gated)")
    print(f"  CSTY  0x{FID_MELEE_STYLE & 0xFFF:03X}        MFO_MeleeStyle (equip_melee stance -- default ON)")
    print(f"  CSTY  0x{FID_RANGED_STYLE & 0xFFF:03X}        MFO_RangedStyle (equip_ranged stance -- default ON)")
    if POC_ENABLED:
        print(f"  GLOB  0x{FID_PROBE_GLOB & 0xFFF:03X}        MFO_ProbeSelect (console: set MFO_ProbeSelect to N; 0 = all probes off)")
        for idx, sp, label, (tkind, tval) in POC_PROBES:
            print(f"  PACK  0x{(FID_POC_PACK_BASE + idx - 1) & 0xFFF:03X}        MFO_PoC{idx}_{label} (gate =={idx}, target {tkind})")
    print()
    print("MFO_Progression.esl records:")
    print(f"  TES4  header     master: Skyrim.esm, ESL flagged, NEXT_OBJECT_ID 0x{PROG_NEXT_OBJECT_ID:03X}")
    print(f"  KYWD  0x{PGID_ENROLLED_KYWD & 0xFFF:03X}        MFOP_Enrolled (reserved tag; DLL does not stamp it in v1)")
    print(f"  KYWD  0x{PGID_MANIFEST_KYWD & 0xFFF:03X}        MFOP_MFOAddonManifest (self-declaration join key; manifest entry[0])")
    for fid, edid, value, fnam in PROG_GLOBS:
        print(f"  GLOB  0x{fid & 0xFFF:03X}        {edid} = {value:g} ({'float' if fnam == 'f' else 'short'})")
    for fid, edid, forms in PROG_CLASS_SKILLS:
        print(f"  FLST  0x{fid & 0xFFF:03X}        {edid} ({len(forms)} AVIF entries, order = weight)")
    for fid, edid in PROG_PERK_LISTS:
        print(f"  FLST  0x{fid & 0xFFF:03X}        {edid} (shipped EMPTY — xEdit extension point)")
    # §18.6 Stage 2 — the N-declared classes.
    for _df, _de, nameFid, nameEdid, disp, _sf, _se, _sv, _sk, _hw, _pp in PROG_CLASSES:
        print(f"  MESG  0x{nameFid & 0xFFF:03X}        {nameEdid} (FULL \"{disp}\" — class display name)")
    print(f"  MESG  0x{PGID_BOARDTAB_LABEL & 0xFFF:03X}        MFOP_BoardTabLabel (FULL \"Progression\" — hosted board-tab title)")
    for _df, _de, _nf, _ne, disp, stanceFid, stanceEdid, stanceVal, _sk, _hw, _pp in PROG_CLASSES:
        print(f"  GLOB  0x{stanceFid & 0xFFF:03X}        {stanceEdid} = {stanceVal} (#65 stance mirror)")
    for ci, (_df, _de, _nf, _ne, disp, _sf, _se, _sv, _sk, hw, pp) in enumerate(PROG_CLASSES):
        base = PGID_HMS_BASE + ci * 4
        print(f"  GLOB  0x{base & 0xFFF:03X}-0x{(base + 3) & 0xFFF:03X}    {_de} HMS weights {hw[0]}/{hw[1]}/{hw[2]}, primary {pp} (v1.1 class ratios as data)")
    print(f"  QUST  0x{PGID_MCM_QUEST & 0xFFF:03X}        MFOP_MCMQuest (MFOP_MCM script; addon MCM tab, in SEQ)")
    for defFid, defEdid, _nf, _ne, _disp, _sf, _se, _sv, skills, _hw, _pp in PROG_CLASSES:
        print(f"  FLST  0x{defFid & 0xFFF:03X}        {defEdid} (class-def: MESG + {len(skills)} AVIF + _Stance + 4 HMS GLOB)")
    print(f"  FLST  0x{PGID_CLASSES & 0xFFF:03X}        MFOP_Classes ({len(PROG_CLASSES)} class-def(s); manifest entry[1])")
    print(f"  FLST  0x{PGID_MANIFEST & 0xFFF:03X}        MFOP_AddonManifest ({4 + len(PROG_MANIFEST_ECONOMY)} entries: "
          f"self-declaration keyword + MFOP_BoardTabLabel + MFOP_Classes + MFOP_EntryPointVerdicts + "
          f"{len(PROG_MANIFEST_ECONOMY)} economy GLOB(s))")
    print(f"  FLST  0x{PGID_VERDICTS_FLST & 0xFFF:03X}        MFOP_EntryPointVerdicts "
          f"(v1.1 residual #3: 92 positional verdict GLOBs; perk-effectiveness judgment as add-on DATA)")


if __name__ == "__main__":
    main()
