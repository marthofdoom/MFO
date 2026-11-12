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
import math
import os
import re
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
FID_MELEE_SINGLE_STYLE = OWN | 0x916  # MFO_MeleeStyle's twin WITHOUT kAllowDualWielding: the melee stance of every
                                   # follower whose perks do not vote dual wield (dual-wield gate, 2026-10-27)
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
# 0x83B/0x83C RETIRED: were FID_APMF_HEAL_SELF_PACKAGE / FID_APMF_HEAL_PLAYER_PACKAGE,
# the APMF ANIMATED HEAL packages (make_apmf_heal_packages() / native/Packages.cpp
# HealAnimFill) -- the opt-in bHealAnimPackage route, removed in favor of a new
# executor. Ids stay retired, never recycled (INVARIANTS #41).
# LOTD AWARENESS (feat/mfo-lotd, ClickUp 86e3edghj): the DLL writes 1 here at
# kDataLoaded and at every load when Legacy of the Dragonborn is detected, 0
# otherwise. The MCM hiddenToggle reads it (GlobalValue "MFO.esp|903") and hides
# the bLootLOTD control when LOTD is absent. Nothing else reads it.
FID_LOTD_DETECTED_GLOB = OWN | 0x903
# POSITIONAL LOCKPICK SOUNDS (marth 2026-10-01): SNDR copies of Skyrim.esm
# UILockpickingPickMovement 000C1911 / UILockpickingPickBreak 000C1916 with a
# positional mono output model. native/Forms.h kSndLockpickPickMovement /
# kSndLockpickPickBreak; native/logistics/Lockpick.cpp plays them at the follower.
# See LOCKPICK_SNDR_PENDING / make_lockpick_sndr() below.
FID_SND_PICK_MOVEMENT = OWN | 0x904
FID_SND_PICK_BREAK    = OWN | 0x905
# TOWN PHASE 2 R7 (inn + temple errands): the ch.9 "work" packages the town errands offer through Harbinger (APMFBridge::
# OfferErrandWork). FOUR per vanilla template, one per concurrent follower (the DLL writes the run-time target / location into
# the slot's record before the claim, so two followers never share one record): Activate 00019B2D (the shrine), Sandbox
# 0001C254 (stay in the inn), Eat 00019714 (the meal), Sleep 00019717 (the bed). Sixteen records, 0x906-0x915, from
# NEXT_OBJECT_ID_WITH_SNDR; native/Forms.h kErrandWork* mirrors them. See make_errand_work_packages().
FID_ERRAND_ACTIVATE_0 = OWN | 0x906   # 0x906-0x909
FID_ERRAND_SANDBOX_0  = OWN | 0x90A   # 0x90A-0x90D
FID_ERRAND_EAT_0      = OWN | 0x90E   # 0x90E-0x911
FID_ERRAND_SLEEP_0    = OWN | 0x912   # 0x912-0x915
ERRAND_WORK_SLOTS     = 4
NEXT_OBJECT_ID     = 0x917         # first never-used local id (0x916 since R7: the sixteen work packages are always emitted;
                                   # was 0x904. 0x836-0x839 = APMF loot-travel packages,
                                   # 0x83A = APMF retreat package, 0x83B/0x83C = retired (see above),
                                   # 0x900-0x902 = P7 travel packages, 0x903 = MFO_LOTDDetected)
                                   # 0x904/0x905 are CLAIMED by the lockpick SNDRs (Forms.h) but only
                                   # emitted once LOCKPICK_SNDR_PENDING is complete; main() then writes
                                   # NEXT_OBJECT_ID_WITH_SNDR into the header.
NEXT_OBJECT_ID_WITH_SNDR = 0x917

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
FREF_TMPL_ACTIVATE     = 0x00019B2D   # R7: Target (SingleRef, uid 0), Number to Activate (Int, uid 2)
FREF_TMPL_SANDBOX      = 0x0001C254   # R7: THREE inputs are named "Location" (uids 0, 9, 15); only uid 0 carries a value
FREF_TMPL_EAT          = 0x00019714   # R7: "Eat Location" (uid 0)
FREF_TMPL_SLEEP        = 0x00019717   # R7: "Sleep Location" (uid 0)

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
    return record('GLOB', FID_PROBE_GLOB, 0, body)


def make_lotd_glob():
    # LOTD awareness: the "Legacy of the Dragonborn is installed" flag the MCM's
    # hiddenToggle reads. Same short-GLOB shape as make_glob above (EDID + FNAM
    # 's' + FLTV), default 0.0. The DLL owns its value (SetValue at kDataLoaded
    # and every load -- a GLOB value is save-persisted).
    body = subrec('EDID', zstr("MFO_LOTDDetected")) + subrec('FNAM', b's')
    body += subrec('FLTV', struct.pack('<f', 0.0))
    return record('GLOB', FID_LOTD_DETECTED_GLOB, 0, body)


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


# ── TOWN PHASE 2 R7: the errand WORK packages (ch.9 0x49 route, runtime target / location) ─────────────────────────
# Four vanilla templates, each instance mirrored from a vanilla exemplar dumped from Skyrim.esm with tools/esp_inspect.py
# (2026-10-19), so the byte shape is the one the engine already loads: Activate <- GilfreSawmillSawLog8x14 (00100FA7),
# Sandbox <- WinterholdKraldarSandboxHome (00000E8C), Eat <- GuardRiftenEatDayShift20x4 (0010C524), Sleep <-
# DefaultSleepEditorLoc17x5 (0010FB3E). Only the inputs MFO sets differ from the exemplar:
#   * the Location (Sandbox / Eat / Sleep) is PLDT type 0 "Near Reference" with a non-null placeholder ref (FREF_PLAYER, as
#     the APMF loot-travel packages do) and a radius; the DLL overwrites PackageLocation::data.refHandle / rad at run time
#     BEFORE it claims the actor (Packages::SetPackageLocation). No QNAM (no alias-valued input).
#   * the Activate Target is PTDA targType 0 (a placed reference), placeholder FREF_PLAYER; the DLL writes the shrine's
#     handle (Packages::SetPackageTargetRef). The placeholder is never the live target: the DLL claims only after both guards pass.
#   * the Sandbox Bools: Idle Markers / Sitting / Conversation ON; Allow Eating (the Eat package handles the meal), Allow Sleeping, Wandering, Special
#     Furniture, "Unlock On Arrival?" (do not unlock the inn doors), Prefered Path, RideHorse OFF; Energy 50 (the exemplar's).
# All four are PSDT "any time", no conditions, no QNAM; the interrupt flags are the exemplar's.
def _work_pkdt(flags, interrupt):
    return subrec('PKDT', struct.pack('<IBBBBHH', flags, 0x12, 0, 2, 0, interrupt, 0))


def _work_inputs(pairs):
    out = b''
    for sig, data in pairs:
        out += subrec(sig, data)
    return out


def _work_tail(unams, xnam):
    out = b''
    for u in unams:
        out += subrec('UNAM', struct.pack('<B', u))
    out += subrec('XNAM', struct.pack('<B', xnam))
    for blk in ('POBA', 'POEA', 'POCA'):
        out += subrec(blk, b'')
        out += subrec('INAM', struct.pack('<I', 0))
        out += subrec('PDTO', struct.pack('<II', 0, 0))
    return out


def _work_pack(fid, edid, pkdt, n_inputs, template, version, inputs, tail):
    body  = subrec('EDID', zstr(edid))
    body += pkdt
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    body += subrec('PKCU', struct.pack('<III', n_inputs, template, version))
    body += inputs
    body += tail
    return record('PACK', fid, 0, body)


def _loc_input(radius):
    return [('ANAM', zstr("Location")), ('PLDT', struct.pack('<IiI', 0, FREF_PLAYER, radius))]


def _bool(v):
    return [('ANAM', zstr("Bool")), ('CNAM', struct.pack('<B', v))]


def make_errand_activate_package(fid, edid):
    inputs = _work_inputs([('ANAM', zstr("SingleRef")), ('PTDA', struct.pack('<III', 0, FREF_PLAYER, 0)),
                           ('ANAM', zstr("Int")), ('CNAM', struct.pack('<I', 1))])
    return _work_pack(fid, edid, _work_pkdt(0, 0xFEFF), 2, FREF_TMPL_ACTIVATE, 13, inputs, _work_tail([0, 2], 3))


def make_errand_sandbox_package(fid, edid, radius=600):
    # Bool order = the exemplar's UNAM run: 0 Location, 14 Unlock On Arrival?, 1 Allow Eating, 3 Allow Sleeping, 4 Allow
    # Conversation, 5 Allow Idle Markers, 6 Allow Sitting, 31 Allow Special Furniture, 7 Allow Wandering, 25 Prefered Path
    # Only?, 27 RideHorseIfPossible, 29 Energy (Float).
    pairs = _loc_input(radius)
    for v in (0, 0, 0, 1, 1, 1, 0, 0, 0, 0):   # Allow Eating OFF: the Eat package handles the meal; Sandbox eating over a long stay would eat the bought stock
        pairs += _bool(v)
    pairs += [('ANAM', zstr("Float")), ('CNAM', struct.pack('<f', 50.0))]
    return _work_pack(fid, edid, _work_pkdt(0, 0xFEFF), 12, FREF_TMPL_SANDBOX, 10, _work_inputs(pairs),
                      _work_tail([0, 14, 1, 3, 4, 5, 6, 31, 7, 25, 27, 29], 32))


def make_errand_eat_package(fid, edid, radius=300):
    # Everything after the Location verbatim from GuardRiftenEatDayShift20x4 (21 inputs).
    pairs = _loc_input(radius)
    pairs += _bool(0)
    for tgt in (0x0000000F, 0x0000001B):   # the two TargetSelector / ObjectList pairs (Gold001, DefaultAshPile1 as vanilla)
        pairs += [('ANAM', zstr("TargetSelector")), ('PTDA', struct.pack('<III', 2, tgt, 0)),
                  ('ANAM', zstr("ObjectList")), ('CNAM', struct.pack('<I', 0))]
    pairs += [('ANAM', zstr("Int")), ('CNAM', struct.pack('<I', 1))]
    for v in (1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0):
        pairs += _bool(v)
    pairs += [('ANAM', zstr("Float")), ('CNAM', struct.pack('<f', 300.0))]
    pairs += _bool(0)
    pairs += [('ANAM', zstr("Float")), ('CNAM', struct.pack('<f', 0.0))]
    unams = [0, 0x15, 1, 4, 5, 6, 0x0a, 0x0c, 0x10, 0x0e, 0x17, 0x19, 0x1a, 0x1b, 0x1c, 0x21, 0x1d, 0x1e, 0x23, 0x08, 0x20]
    return _work_pack(fid, edid, _work_pkdt(0x00200000, 0xFCF7), 21, FREF_TMPL_EAT, 8, _work_inputs(pairs), _work_tail(unams, 0x24))


def make_errand_sleep_package(fid, edid, radius=500):
    # Everything after the Location verbatim from DefaultSleepEditorLoc17x5 (17 inputs).
    pairs = _loc_input(radius)
    pairs += [('ANAM', zstr("TargetSelector")), ('PTDA', struct.pack('<III', 2, 0x0000001A, 0)),
              ('ANAM', zstr("ObjectList")), ('CNAM', struct.pack('<I', 0))]
    for v in (0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1):
        pairs += _bool(v)
    pairs += [('ANAM', zstr("Float")), ('CNAM', struct.pack('<f', 300.0)),
              ('ANAM', zstr("Float")), ('CNAM', struct.pack('<f', 50.0))]
    unams = [0, 1, 2, 6, 8, 0x0f, 0x0d, 0x0b, 0x11, 0x12, 0x13, 0x14, 0x15, 0x19, 0x16, 0x1a, 0x18]
    return _work_pack(fid, edid, _work_pkdt(0x00200000, 0xFEFF), 17, FREF_TMPL_SLEEP, 6, _work_inputs(pairs), _work_tail(unams, 5))


def make_errand_work_packages():
    out = b''
    for i in range(ERRAND_WORK_SLOTS):
        out += make_errand_activate_package(FID_ERRAND_ACTIVATE_0 + i, f"MFO_ErrandActivate{i}")
    for i in range(ERRAND_WORK_SLOTS):
        out += make_errand_sandbox_package(FID_ERRAND_SANDBOX_0 + i, f"MFO_ErrandSandbox{i}")
    for i in range(ERRAND_WORK_SLOTS):
        out += make_errand_eat_package(FID_ERRAND_EAT_0 + i, f"MFO_ErrandEat{i}")
    for i in range(ERRAND_WORK_SLOTS):
        out += make_errand_sleep_package(FID_ERRAND_SLEEP_0 + i, f"MFO_ErrandSleep{i}")
    return out


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
    body += make_retreat_package()
    body += make_errand_work_packages()   # R7: 16 town-errand work packages (Activate / Sandbox / Eat / Sleep x 4 slots)
    return group('PACK', body)


# ── CSTY: MFO's combat styles (stance ownership) ───────────────────────────
def _csty_record(edid, fid, csgd_floats, cscr, data_flags=1):
    """One CSTY record. Byte shape MIRRORED from vanilla humanoid styles (dumped
    from Skyrim.esm per doctrine, never format docs): EDID, CSGD 40 bytes (10
    floats, the CommonLibSSE-NG CombatStyleGeneralData layout), CSME 28 bytes
    (7 floats -- the on-disk record carries one float FEWER than the runtime
    struct's 8; mirror the disk, the loader zero-fills), CSCR 16, CSLR 4, CSFL 28
    (same 7-of-8 truncation), DATA 4 (flags, 1 = dueling).

    `data_flags` is the DATA subrecord's flag word = the runtime
    `TESCombatStyle::flags` member (pinned CommonLibSSE-NG 3.7.0,
    RE/T/TESCombatStyle.h, `enum class FLAG // DATA`: kDuelingStyle 1<<0,
    kFlankingStyle 1<<1, kAllowDualWielding 1<<2). NOTE the header also lists a
    RECORD-HEADER bit `RecordFlags::kAllowDualWielding = 1<<19`; the disk says
    the DATA bit is the one the engine honours: a census of all 145 Skyrim.esm
    CSTY records (2026-09-13, tools/esp_inspect.py) found 22 with DATA bit 2 set,
    of which 7 -- csAlikrBerserker, csForswornBerserkerLow, csTGStandard,
    csDraugrMagicAllowDual ... -- carry NO header bit at all, and those are the
    game's actual dual-wielders. So MFO sets DATA only; record flags stay 0.

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
            + subrec('DATA', struct.pack('<I', data_flags)))
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
    # DUAL-WIELD GATE (marth 2026-10-27, "perk/class gated"): the paragraph below
    # described the 2026-09-13 global exposure. It is closed: MFO_MeleeStyle (0x833,
    # DATA 1|4) is now swapped in ONLY for a follower whose perks vote dual wield
    # (Scheduler.cpp -> CombatStyle::Want a_allowDual); every other forced-melee
    # follower gets MFO_MeleeSingleStyle (0x916), byte-identical to the melee style
    # except DATA = 1 (dueling only).
    # MFO_MeleeStyle DATA = dueling | ALLOW DUAL WIELDING (1 | 4 = 5, the exact
    # word csThalmorMeleeDual / csAlikrBerserker carry). 2026-09-13, weapon style
    # by perks: the engine dual-wields an NPC only under a style that allows it,
    # and the DLL force-equips a second one-hander into the LEFT hand for a
    # follower whose perks vote dual wield (cast/Equip.cpp EquipWeapon). THE ONE
    # GLOBAL CHANGE OF THAT FEATURE, and deliberately not gated per follower: a
    # second CSTY form would move FormIDs (Forms.h is a frozen contract) and the
    # DLL swaps ONE melee style onto every follower it forces melee. Exposure,
    # stated not hidden: an UNPERKED follower carrying two one-handers may now be
    # dual-wielded by its OWN combat AI (MFO's equip gate denies spells/staves
    # only). The economy sells a follower's spare one-hander at the next vendor,
    # so that window is loot-to-vendor. The DLL side never fills a left hand
    # unless the perks vote for it.
    melee  = _csty_record("MFO_MeleeStyle",  FID_MELEE_STYLE,
                          (1.0, 0.5, 1.0, 10.0, 0.1, 0.1, 1.0, 0.1, 0.1, 0.0), CSCR_MELEE,
                          data_flags=1 | 4)
    single = _csty_record("MFO_MeleeSingleStyle", FID_MELEE_SINGLE_STYLE,
                          (1.0, 0.5, 1.0, 10.0, 0.1, 0.1, 1.0, 0.1, 0.1, 0.0), CSCR_MELEE)
    ranged = _csty_record("MFO_RangedStyle", FID_RANGED_STYLE,
                          (1.0, 0.5, 1.0, 0.1, 0.1, 10.0, 1.0, 0.1, 0.1, 0.5), CSCR_RANGED)
    return group('CSTY', cast + melee + ranged + single)


# ── SNDR: positional lockpick sounds ────────────────────────────────────────
# SUBRECORD ORDER SOURCE: xEdit's TES5 definition, TES5Edit/TES5Edit branch
# dev-4.1.6, Core/wbDefinitionsTES5.pas `wbRecord(SNDR, 'Sound Descriptor', ...)`
# (fetched 2026-10-01), plus wbSoundDescriptorSounds in Core/wbDefinitionsCommon.pas
# (= wbRArray('Sounds', wbString(ANAM, 'Sound'))). The order is:
#   EDID, CNAM (u32 Descriptor Type, 0x1EEF540A = Standard), GNAM (SNCT Category),
#   SNAM (SNDR Alternate Sound For), ANAM* (one zstring per sound file),
#   ONAM (SOPM Output Model), FNAM (u32 flags, ONLY below form version 35),
#   CTDA* (conditions), LNAM (4 bytes: unknown, looping, unknown, rumble),
#   BNAM (s8 % freq shift, s8 % freq variance, u8 priority, u8 dB variance,
#         u16 static attenuation in dB*100).
# NOTE: CNAM is the descriptor TYPE, not the category. The category is GNAM.
# Top-group order: xEdit wbAddGroupOrder puts SNDR after CSTY (... PACK, CSTY,
# ..., MOVT, SNDR, DUAL, SNCT, SOPM), so the SNDR group goes last in MFO.esp.
#
# WHY: every vanilla UILockpicking* SNDR (000C1910-000C1919) uses output model
# 000B75FB SOMUIDefault (DefinedSpeakers, 2D), so it plays at full volume in the
# player's ears wherever the follower is (local xEdit check, ClickUp 86e3haxqw).
# These copies keep the vanilla sound files, LNAM and BNAM, and swap the output
# model and the category to the positional object-sound pair (see VALUES below).
#
# ONAM: Skyrim.esm 0005A28A SOMMono01400 (positional mono, 1400u attenuation),
# named in the local agent's ClickUp 86e3haxqw comment. Skyrim.esm is MFO.esp's
# only master, so its forms are written with master index 0x00.
FREF_SOPM_MONO01400 = 0x0005A28A
ABSENT = 'ABSENT'   # "this subrecord is not present on the vanilla record"

# ============================================================================
# LOCKPICK_SNDR_PENDING -- every value here is COPIED from the vanilla record EXCEPT gnam
# (AudioCategorySFX, marth's instruction) and the ONAM written in _sndr_record (SOMMono01400).
# None = not read from Skyrim.esm yet. While ANY value is None the generator
# REFUSES to emit both records and writes MFO.esp exactly as before. NEVER fill
# a value from memory or a guess: read it from Skyrim.esm (xEdit or a dump).
#   'flags': record header flags (u32) of the vanilla record.
#   'cnam' : CNAM u32 descriptor type, or ABSENT.
#   'gnam' : GNAM SNCT category FormID as stored in Skyrim.esm (0x00xxxxxx), or ABSENT.
#   'snam' : SNAM alternate-sound-for SNDR FormID, or ABSENT.
#   'anam' : list of every ANAM string, EXACT case and order, exactly as stored
#            (whether it starts with "Sound\" or is relative to Data\Sound is
#            whatever the vanilla bytes say), backslashes as in the record.
#   'fnam' : FNAM raw bytes (hex string), or ABSENT (expected ABSENT when the
#            vanilla record's form version is 35 or higher).
#   'ctda' : list of raw CTDA subrecord payloads as hex strings ([] = none).
#   'lnam' : LNAM raw 4 bytes as a hex string, e.g. "00000000".
#   'bnam' : BNAM raw 6 bytes as a hex string.
# ============================================================================
# VALUES: read from Skyrim.esm by the local agent, posted by marth 2026-10-01
# (coordinator relay). Both vanilla records: flags 0, CNAM 0x1EEF540A (standard
# descriptor type hash), no SNAM, no FNAM, no CTDA. ANAM strings are stored WITH
# the "Data\\Sound\\" prefix (77 / 71 bytes per subrecord incl. the NUL), in the
# vanilla order (it starts at _03). LNAM and BNAM are kept from the originals.
# GNAM is NOT copied: marth 2026-10-01, "For a positional copy, use GNAM 000172A1
# AudioCategorySFX and ONAM 0005A28A SOMMono01400, as the vanilla positional object
# sound NPCHumanWoodPickup does." (NPCHumanWoodPickup 00100638: CNAM 1EEF540A,
# GNAM 000172A1, ONAM 0005A28A, LNAM 00000000, BNAM 00 04 80 00 08 01.)
FREF_SNCT_AUDIOCATEGORYSFX = 0x000172A1
LOCKPICK_SNDR_PENDING = {
    FID_SND_PICK_MOVEMENT: {
        'edid':   "MFO_LockpickPickMovementSD",
        'source': "Skyrim.esm 000C1911 UILockpickingPickMovement",
        'flags': 0x00000000, 'cnam': 0x1EEF540A, 'gnam': FREF_SNCT_AUDIOCATEGORYSFX, 'snam': ABSENT,
        'anam': [
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_03.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_04.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_05.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_06.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_07.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_08.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_09.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_10.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_11.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_12.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_01.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickMovement\\UI_Lockpicking_PickMovement_02.wav",
        ],
        'fnam': ABSENT, 'ctda': [],
        'lnam': "00 00 00 00",
        'bnam': "00 0A 80 06 95 02",   # bytes [0,10,128,6,149,2]
    },
    FID_SND_PICK_BREAK: {
        'edid':   "MFO_LockpickPickBreakSD",
        'source': "Skyrim.esm 000C1916 UILockpickingPickBreak",
        'flags': 0x00000000, 'cnam': 0x1EEF540A, 'gnam': FREF_SNCT_AUDIOCATEGORYSFX, 'snam': ABSENT,
        'anam': [
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickBreak\\UI_Lockpicking_PickBreak_03.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickBreak\\UI_Lockpicking_PickBreak_01.wav",
            "Data\\Sound\\FX\\UI\\Lockpicking\\PickBreak\\UI_Lockpicking_PickBreak_02.wav",
        ],
        'fnam': ABSENT, 'ctda': [],
        'lnam': "00 00 00 00",
        'bnam': "00 00 80 02 8C 01",   # bytes [0,0,128,2,140,1]
    },
}
_SNDR_COPIED_KEYS = ('flags', 'cnam', 'gnam', 'snam', 'anam', 'fnam', 'ctda', 'lnam', 'bnam')


def lockpick_sndr_missing():
    """[(edid, key), ...] for every copied value still None."""
    return [(spec['edid'], k) for spec in LOCKPICK_SNDR_PENDING.values()
            for k in _SNDR_COPIED_KEYS if spec[k] is None]


def _hexbytes(v, n=None, what=''):
    b = bytes.fromhex(v.replace(' ', ''))
    if n is not None and len(b) != n:
        raise SystemExit(f"LOCKPICK_SNDR_PENDING: {what} must be {n} bytes, got {len(b)}")
    return b


def _sndr_record(fid, spec):
    e = spec['edid']
    body = subrec('EDID', zstr(e))
    if spec['cnam'] != ABSENT:
        body += subrec('CNAM', struct.pack('<I', spec['cnam']))
    if spec['gnam'] != ABSENT:
        body += subrec('GNAM', struct.pack('<I', spec['gnam']))
    if spec['snam'] != ABSENT:
        body += subrec('SNAM', struct.pack('<I', spec['snam']))
    if not spec['anam']:
        raise SystemExit(f"LOCKPICK_SNDR_PENDING: {e} has no ANAM sound files")
    for path in spec['anam']:
        body += subrec('ANAM', zstr(path))   # exact bytes + NUL (77 / 71 bytes)
    body += subrec('ONAM', struct.pack('<I', FREF_SOPM_MONO01400))   # changed (with GNAM): positional output
    if spec['fnam'] != ABSENT:
        body += subrec('FNAM', _hexbytes(spec['fnam'], what=f"{e} FNAM"))
    for c in spec['ctda']:
        body += subrec('CTDA', _hexbytes(c, 32, f"{e} CTDA"))
    body += subrec('LNAM', _hexbytes(spec['lnam'], 4, f"{e} LNAM"))
    body += subrec('BNAM', _hexbytes(spec['bnam'], 6, f"{e} BNAM"))
    return record('SNDR', fid, spec['flags'], body)


def make_lockpick_sndr():
    """The SNDR group, or b'' (and a loud message) while any copied value is None."""
    missing = lockpick_sndr_missing()
    if missing:
        print("SNDR  REFUSED: LOCKPICK_SNDR_PENDING is incomplete, so the two positional lockpick")
        print("      sounds (0x904/0x905) are NOT emitted and MFO.esp is written as before.")
        print("      Missing: " + ", ".join(f"{e}.{k}" for e, k in missing))
        return b''
    return group('SNDR', b''.join(_sndr_record(fid, spec)
                                  for fid, spec in sorted(LOCKPICK_SNDR_PENDING.items())))


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
# (0x800/0x801) and native/progression/ProgAllocator.h (everything else). The ESL
# masters ONE plugin, Skyrim.esm (index 0x00), so its OWN forms sit at index 0x01
# (OWN_PROG below; the earlier two-master §18.6 layout with MFO.esp and prefix 0x02
# is retired, see the 1.1 note under OWN_PROG). ESL-legal locals
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

# MFOP_Version FLTV — the addon version encoded as MAJOR.MINORPATCH (add-on 1.1.4 -> 1.104,
# marth 2026-10-01). The value is a float32 GLOB, so a dotted x.y.z cannot be stored; nothing in
# the DLL gates on it. Bump by hand with the add-on (no machine-readable add-on version exists;
# VERSION is MFO's own, 2.x).
PROG_VERSION_STAMP = 1.106

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
PGID_LEVELS_PER_CRAFT   = OWN_PROG | 0x80A  # GLOB crafting perk divisor (1 craft point / N levels; default 4; <=0 disables)
# 0x80A = MFOP_LevelsPerCraftPerkPoint; 0x80B-0x80F reserved: more economy knobs
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
# follower's DOMINANT one — see progression/SkillScale.cpp WeightsFor). Melee prunes to
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
    (PGID_LEVELS_PER_CRAFT,"MFOP_LevelsPerCraftPerkPoint", 4.0,   's'),
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
    PGID_LEVELS_PER_CRAFT,
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
# lockpick/craft/commerce/player-UI = D, except the three the town features read
# (8 ModBuyPrices, 60 ModSellPrices, 76 ModTemperingHealth = E, add-on 1.1.6); the named marginal set (+ unproven-on-
# NPC calls) = M (flagged, never silently killed, so over-filtering stays
# diagnosable). The engine enum NAMES stay a general fact in the DLL; only these
# verdicts moved. Editor-ids are informational (the DLL reads them by ORDER).
VERDICT_CODE = {'E': 0, 'M': 1, 'D': 2}
PROG_ENTRYPOINT_VERDICTS = [
    "CalculateWeaponDamage E", "CalculateMyCriticalHitChance E",
    "CalculateMyCriticalHitDamage E", "CalculateMineExplodeChance D",
    "AdjustLimbDamage E", "AdjustBookSkillPoints D", "ModRecoveredHealth E",
    "GetShouldAttack M", "ModBuyPrices E", "AddLeveledListOnDeath D",
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
    "ModLockpickSweetSpot D", "ModSellPrices E", "CanPickpocketEquippedItem D",
    "ModLockpickLevelAllowed D", "SetLockpickStartingArc D", "SetProgressionPicking D",
    "MakeLockpicksUnbreakable D", "ModAlchemyEffectiveness D", "ApplyWeaponSwingSpell E",
    "ModCommandedActorLimit M", "ApplySneakingSpell E", "ModPlayerMagicSlowdown D",
    "ModWardMagickaAbsorptionPct E", "ModInitialIngredientEffectsLearned D",
    "PurifyAlchemyIngredients D", "FilterActivation D", "CanDualCastSpell M",
    "ModTemperingHealth E", "ModEnchantmentPower D", "ModSoulPctCapturedToWeapon D",
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
    (PGID_LEVELS_PER_CRAFT, "Levels per crafting perk point",
     "Separate crafting perk stream (Smithing, Alchemy, Enchanting): one crafting "
     "point every N levels, spent before the general pool. 0 disables the stream "
     "(crafting perks then cost general points).",
     0, 10, 1, 4),
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
    PGID_LEVELS_PER_CRAFT: "iLevelsPerCraftPerkPoint",
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


# ── MFO_Classes.esl — the CLASS add-on (card C7, class update, ClickUp 86e3dhbd3) ──
# A SEPARATE, OPTIONAL, self-contained ESL like MFO_Progression.esl: it masters
# ONLY Skyrim.esm (own-form prefix 0x01), declares itself with its OWN manifest
# keyword (edid suffix "_MFOClassesManifest", Classes.h PRESENCE CONTRACT), and
# touches NOTHING in MFO.esp (which stays byte-identical). Forms.h FormIDs stay
# frozen: every id below is this add-on's own local id. Absent = the DLL's class
# tree is inert (native/classes/Presence.cpp).
#
# THE CLASS TABLE IS READ FROM THE DLL'S SOURCE (ONE source of truth, no 35-row
# hand copy): native/classes/ClassTable.cpp (rows) + native/classes/Classes.h
# (the Id enum order and the Flag:: bit values). load_class_table() parses them
# and ASSERTS the result (35 rows, enum order == row order, tier counts, slug ==
# passive id, T1 roots 1..3) so a table edit that the parser cannot follow fails
# the generator loudly instead of shipping a stale ESL.
#
# LOCAL ID PLAN (ESL-legal 0x800-0xFFF; ADD ONLY, never renumber):
#   0x800 KYWD manifest join key      0x801 FLST manifest (entry[0] = 0x800)
#   0x802 FLST passive factions (35, in class-table order = the Id bit index)
#   0x803 FLST class styles index: the 35 per-class style FLSTs in class-Id order
#   0x880 + i  FLST MFOC_ClassStyles_<Name>: [mask GLOB, that class's CSTYs in slot-bit order]
#   0x8C0 + i  GLOB MFOC_StyleMask_<Name>: bitmask 1 melee, 2 melee dual, 4 ranged, 8 cast
#   0x808-0x80A KYWD Marked/Flanked/Guarded (carried by the three marker MGEFs)
#   0x810-0x812 MGEF MFOC_Marked / MFOC_Flanked / MFOC_Guarded
#   0x820-0x824 GLOB Version, XpRate, PriestSunDps, SaintHealPerSec, SaintUndeadDps
#   0x830 FACT MFOC_OffClassWeapon     0x831 FACT MFOC_MarkPending
#   0x840 + i  FACT MFOC_P_<Name> (i = class Id ordinal 0..34 -> 0x840-0x862)
#   0x900 +    CSTY MFOC_Style_<Name>_<Melee|MeleeDual|Ranged|Cast>, one per (class, usable
#              stance), allocated sequentially in class then slot order; look them up through the
#              per-class FLST, NEVER by position (stances come from the table's weapon bits)
#   0xA00 PERK MFOC_ReferencePerk (the audited reference shape, NEVER granted)
#   0xA10 + i  PERK MFOC_Passive_<Name> (card C6; only classes with a perk effect; i = Id ordinal)
#   0xA40-0xA42 MGEF ability effects (stamina, stamina regen, speed)
#   0xA50-0xA51 SPEL Mercenary full / quarter    0xA60-0xA73 SPEL Scout speed per level 1..20    0xA74 SPEL Scout quarter
#   0xA80-0xA82 SPEL marker carriers (Marked / Flanked / Guarded)
#   0xA84 PERK MFOC_OffClassHalf (card G16: ModAttackDamage x0.5 while the follower's rank in MFOC_OffClassWeapon is >= 1)
#   0xA83 QUST MFOC_MCMQuest (card C11: the add-on's own MCM Helper quest, script MFOC_MCM, listed in SEQ/MFO_Classes.seq)
OWN_CLS = 0x01000000
CGID_MANIFEST_KYWD = OWN_CLS | 0x800
CGID_MANIFEST      = OWN_CLS | 0x801
CGID_FACTION_LIST  = OWN_CLS | 0x802
CGID_STYLE_LIST    = OWN_CLS | 0x803
CGID_MARKED_KYWD   = OWN_CLS | 0x808
CGID_FLANKED_KYWD  = OWN_CLS | 0x809
CGID_GUARDED_KYWD  = OWN_CLS | 0x80A
CGID_MARKED_MGEF   = OWN_CLS | 0x810
CGID_FLANKED_MGEF  = OWN_CLS | 0x811
CGID_GUARDED_MGEF  = OWN_CLS | 0x812
CGID_VERSION_GLOB  = OWN_CLS | 0x820
CGID_XP_RATE_GLOB  = OWN_CLS | 0x821
CGID_PRIEST_SUN    = OWN_CLS | 0x822
CGID_SAINT_HEAL    = OWN_CLS | 0x823
CGID_SAINT_UNDEAD  = OWN_CLS | 0x824
CGID_OFFCLASS_FACT = OWN_CLS | 0x830
CGID_MARKPEND_FACT = OWN_CLS | 0x831
CGID_PASSIVE_BASE  = OWN_CLS | 0x840
CGID_STYLE_FLST_BASE = OWN_CLS | 0x880
CGID_STYLE_MASK_BASE = OWN_CLS | 0x8C0
CGID_STYLE_BASE    = OWN_CLS | 0x900
CGID_REFERENCE_PERK = OWN_CLS | 0xA00
# Card C6 (passive carriers). Add-only, same ESL-legal 0x800-0xFFF range. The DLL (native/classes/Carriers.cpp)
# resolves these BY LOCAL ID and type-checks every one at the first reconcile, so keep both in lockstep.
CGID_PASSIVE_PERK_BASE = OWN_CLS | 0xA10   # + class Id ordinal (0xA10-0xA32); only classes with a perk effect have one
CGID_AB_STAMINA_MGEF   = OWN_CLS | 0xA40   # ability MGEFs (Peak Value Modifier, Constant, Self)
CGID_AB_STAMINARATE_MGEF = OWN_CLS | 0xA41
CGID_AB_SPEED_MGEF     = OWN_CLS | 0xA42
CGID_MERC_FULL_SPELL   = OWN_CLS | 0xA50   # Mercenary: +40 stamina, +25 regen (full)
CGID_MERC_QUARTER_SPELL = OWN_CLS | 0xA51  # Mercenary: the rounded quarter
CGID_SCOUT_SPEED_BASE  = OWN_CLS | 0xA60   # + (level - 1), level 1..20: Scout speed, scaled
CGID_SCOUT_QUARTER_SPELL = OWN_CLS | 0xA74 # Scout: the mastered-unequipped quarter
CGID_MARKED_SPELL      = OWN_CLS | 0xA80   # marker carrier SPELs (B619)
CGID_FLANKED_SPELL     = OWN_CLS | 0xA81
CGID_GUARDED_SPELL     = OWN_CLS | 0xA82
CGID_MCM_QUEST     = OWN_CLS | 0xA83   # card C11
CGID_OFFCLASS_PERK = OWN_CLS | 0xA84   # card G16 (off-class weapon rule): granted by classes/Carriers.cpp to every follower with a class equipped
# Zero-avoid TWINS of the class CSTYs (heavy armor / Living Legend's held guard, see CLS_AVOID_THREAT). Add-only ids.
CGID_STYLE_TWIN_BASE = OWN_CLS | 0xA85         # one twin per class style with avoidThreat > 0 (<= 95: 0xA85-0xAE3)
CGID_STYLE_TWIN_FLST_BASE = OWN_CLS | 0xB00    # + class Id: the twin lookup, SAME shape as MFOC_ClassStyles_<Name>
CLS_NEXT_OBJECT_ID = 0xB23
CLS_VERSION_STAMP  = 1.000

# RANK SPACE (marth 11-10: caps T1 10 / T2 15 / T3 20 / T4 25; the T3 strong effect
# and the T4 unique scale linearly 25% at L1 -> 100% at the cap; a mastered-but-
# unequipped passive keeps 1/4 of the FULL value). Each MFOC_P_<class> faction
# carries ONE per-reference rank (SetFactionRank; a faction needs NO RNAM rank
# titles for a rank to be readable, the vanilla follower factions declare none):
#   rank <= 0 / not a member  OFF
#   rank 1..cap               ACTIVE at class level N: the equipped class, or a
#                             T4 parent held at full (its level is then the cap)
#   rank 100                  MASTERED AND UNEQUIPPED = 1/4 of the full value
# 100 sits above every cap (max 25) and inside the signed-byte rank range (<=127),
# so "active at level >= L" is the two-sided GetFactionRank in [L, 99]: the 100
# marker never satisfies a level test by accident. The per-level magnitude
# carrier (one entry reading the level, or a ladder of entries) is C6's decision.
CLASS_LEVEL_CAP = {1: 10, 2: 15, 3: 20, 4: 25, 5: 25}   # T5 (Dragonborn) has no passive: 25 is a placeholder ceiling
RANK_OFF = 0
RANK_ACTIVE_MAX = 99
RANK_MASTERED_UNEQUIPPED = 100
SCALE_FLOOR = 0.25        # strong-effect fraction at class level 1
MASTERED_FRACTION = 0.25  # mastered-unequipped fraction of the full value


def class_scale(level, cap):
    """Linear 25% at L1 -> 100% at the cap (marth 11-10). Reference formula for
    C6; the generator itself only needs the rank space above."""
    assert 1 <= level <= cap and cap > 1
    return SCALE_FLOOR + (1.0 - SCALE_FLOOR) * (level - 1) / (cap - 1)


# ── the class table, parsed from the DLL source ────────────────────────────
CLS_SRC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "native", "classes")
CLS_TIER_COUNTS = {1: 3, 2: 9, 3: 14, 4: 8, 5: 1}   # 35 classes (Classes.h static_assert)


def _cls_source():
    with open(os.path.join(CLS_SRC_DIR, "Classes.h"), encoding='utf-8') as f:
        hdr = f.read()
    with open(os.path.join(CLS_SRC_DIR, "ClassTable.cpp"), encoding='utf-8') as f:
        src = f.read()
    return hdr, src


def _cls_named_bits(text, ns):
    """{'kName': value} for 'inline constexpr std::uint16_t kName = 1 << N;' lines inside
    'namespace <ns> { ... }' of Classes.h."""
    m = re.search(r'namespace ' + ns + r'\s*\{(.*?)\n\s*\}', text, re.S)
    if not m:
        raise SystemExit(f"load_class_table: Classes.h has no 'namespace {ns}' block")
    return {n: 1 << int(b) for n, b in
            re.findall(r'inline constexpr std::uint16_t (k\w+)\s*=\s*1 << (\d+);', m.group(1))}


def class_flag_bits():
    """{'kShortLeash': 1, ...} from the Flag:: namespace of Classes.h."""
    hdr, _src = _cls_source()
    return _cls_named_bits(hdr, 'Flag')


def _split_top(text):
    """Split on commas at paren depth 0 (outside quotes)."""
    out, depth, cur, q = [], 0, '', False
    for ch in text:
        if ch == '"':
            q = not q
        if not q:
            if ch in '(<[{':
                depth += 1
            elif ch in ')>]}':
                depth -= 1
            elif ch == ',' and depth == 0:
                out.append(cur.strip())
                cur = ''
                continue
        cur += ch
    out.append(cur.strip())
    return out


def load_class_table():
    """[{'idx','id','slug','name','tier','line','root','weapons','passive','flags'}]
    in class-Id order, parsed from native/classes/{Classes.h,ClassTable.cpp}."""
    hdr, src = _cls_source()

    def fail(msg):
        raise SystemExit("load_class_table: " + msg)

    # Id enum order == the bit index into every mastered/unlocked bitset.
    m = re.search(r'enum class Id[^{]*\{(.*?)\bkCount\b', hdr, re.S)
    enum_ids = re.findall(r'[A-Za-z_]\w*', re.sub(r'//[^\n]*', '', m.group(1)))
    names = dict(_cls_named_bits(hdr, 'Flag'))
    names.update(_cls_named_bits(hdr, 'Weapon'))
    rm = re.search(r'constexpr RootT1 kRootMelee = (\d+), kRootRanged = (\d+), kRootMage = (\d+);', src)
    names.update(zip(('kRootMelee', 'kRootRanged', 'kRootMage'), map(int, rm.groups())))
    # Named flag/weapon-set aliases the cpp defines (kDisSpell, kMelee1H2H, kThiefSet, ...).
    aliases = dict(re.findall(r'constexpr std::uint16_t (k\w+)\s*=\s*([^;]+);', src))

    def value(expr, seen=()):
        v = 0
        for tok in (t.strip() for t in expr.split('|')):
            if tok.isdigit():
                v |= int(tok)
            elif tok in names:
                v |= names[tok]
            elif tok in aliases and tok not in seen:
                v |= value(aliases[tok], seen + (tok,))
            else:
                fail(f"unknown flag/weapon token '{tok}' in ClassTable.cpp")
        return v

    rows = []
    for line in src.splitlines():
        if not line.lstrip().startswith('Mk(Id::'):
            continue
        body = line.strip()[len('Mk('):]
        end = body.rfind(')')
        a = _split_top(body[:end])
        if len(a) < 13 or not a[0].startswith('Id::') or not a[4].startswith('Line::'):
            fail(f"cannot parse a class row: {line.strip()[:90]}")
        rows.append({'id': a[0][4:], 'slug': a[1].strip('"'), 'name': a[2].strip('"'), 'tier': int(a[3]),
                     'line': a[4][6:], 'root': value(a[5]), 'weapons': value(a[7]),
                     'passive': a[9].strip('"'), 'flags': value(a[12])})
    # Consistency checks: a table edit the parser cannot follow must fail HERE.
    assert len(rows) == 35 and len(enum_ids) == 35, \
        f"class table: {len(rows)} rows / {len(enum_ids)} Id enumerators, expected 35 / 35"
    for i, (row, eid) in enumerate(zip(rows, enum_ids)):
        row['idx'] = i
        assert row['id'] == eid, f"class row {i}: Mk(Id::{row['id']}) but the Id enum has {eid} at {i}"
        assert row['passive'] == 'passive.' + row['slug'], f"class {row['slug']}: passive id {row['passive']}"
        if row['tier'] == 1:
            assert row['root'] == i + 1, f"T1 class {row['slug']}: root {row['root']} != combatClassOverride {i + 1}"
    counts = {t: sum(1 for r in rows if r['tier'] == t) for t in CLS_TIER_COUNTS}
    assert counts == CLS_TIER_COUNTS, f"class table tier counts {counts}, expected {CLS_TIER_COUNTS}"
    assert rows[-1]['id'] == 'Dragonborn' and rows[-1]['root'] == 0
    assert len({cls_edid_name(r) for r in rows}) == 35, "class EDID names collide"
    return rows


def cls_edid_name(row):
    """Display name -> an EDID-safe PascalCase word ('Arch-Mage' -> 'ArchMage')."""
    return re.sub(r'[^A-Za-z0-9]', '', row['name'])


# ── CTDA + PERK writer (the NEW byte shape; C6 builds every passive on it) ──
# SHAPES VERIFIED against Skyrim.esm (tools/esp_inspect.py dumps + a census of all
# 375 PERK records, 2026-11-09), never from a format doc:
#   PERK = EDID, FULL, DESC, CTDA* (perk-level), DATA(5: trait,level,ranks,playable,
#          hidden), then per ENTRY: PRKE(3: type 2=entry point, rank, priority),
#          DATA(3: entry point, function, tab count), [PRKC(tab idx) CTDA+]*,
#          EPFT(1: 1=float), EPFD(float), PRKF(empty).
#   CTDA = 32 bytes: u8 type(op bits 0x20/0x40/0x80; 0x01 = OR with the next, 0x04 =
#          use-global), 3 pad, f32 value, u16 function, 2 pad, u32 param1, u32
#          param2, u32 runOn (0 = Subject), u32 reference, u32 param3 (0xFFFFFFFF
#          as vanilla writes it).
#   The tab index is the entry point's evaluation context and its MEANING DIFFERS PER
#   ENTRY POINT (vanilla census of every condition on tabs of the six EPs below,
#   2026-11-09; see PERK_ENTRY_TABS): e.g. tab 1 is the WEAPON for EP 18/28/35/37, the
#   SPELL for EP 29, the ATTACKER actor for EP 36; tab 2 is the TARGET for EP 29/35.
CTDA_OPS = {'==': 0x00, '!=': 0x20, '>': 0x40, '>=': 0x60, '<': 0x80, '<=': 0xA0}
# Condition function ids (CommonLibSSE-NG TESCondition.h FunctionID, mit-3.7 fork),
# EACH ALSO FOUND in vanilla Skyrim.esm CTDA records (census 2026-11-09):
COND_GET_FACTION_RANK   = 73    # param1 = FACT
COND_GET_RANDOM_PERCENT = 77    # vanilla uses '<=' N.0 (NN01Perk: a0 .. 25.0)
COND_HAS_PERK           = 448   # param1 = PERK
COND_HAS_KEYWORD        = 560   # param1 = KYWD (a weapon keyword on a weapon tab, an actor keyword on an actor tab)
COND_IS_ATTACKING       = 672   # no params (vanilla IDLE BearSleep / PlayerStaggerBow)
COND_HAS_MAGIC_EFFECT_KEYWORD = 699   # param1 = KYWD carried by an active MGEF (vanilla Detect* MGEFs)
COND_SPELL_HAS_KEYWORD  = 693   # EPMagic_SpellHasKeyword, param1 = KYWD, on a SPELL tab (AugmentedFlames, Animage, Regeneration)
COND_SPELL_HAS_SKILL    = 696   # EPMagic_SpellHasSkill, param1 = actor-value index, on a SPELL tab (Animage: 0x15 Illusion)
COND_IS_UNDEAD          = 715   # no params, on a TARGET tab (Animage, KindredMage, Necromage)

# entry point -> (tab kind for tab 0, tab 1, tab 2). The index is the BGSEntryPoint enum
# (PROG_ENTRYPOINT_VERDICTS row order): 18 ModSneakAttackMult, 28 ModPowerAttackDamage,
# 29 ModSpellMagnitude, 35 ModAttackDamage, 36 ModIncomingDamage, 37
# ModTargetDamageResistance. Float-valued entries (EPFT 1; functions 1 Set / 2 Add /
# 3 Multiply), tab count 3 in vanilla. A tab kind is recorded ONLY where vanilla
# conditions on that tab prove it (the keyword / function census of Skyrim.esm):
#   18  tab1 weapon (WeapTypeBow / WeapTypeDagger HasKeyword)
#   28  tab0 owner (func 697), tab1 weapon (WeapType*)
#   29  tab0 owner (HasPerk), tab1 SPELL (MagicAlch* keywords, func 693/713/696), tab2 target (ActorType*, IsUndead)
#   35  tab0 owner (funcs 448/80/72/597/697), tab1 weapon (WeapType*), tab2 target (races/ids)
#   36  tab0 owner (funcs 682/640/14/77), tab1 ATTACKER actor (PlayerKeyword, ActorTypeDragon)
#   37  tab0 owner (HasPerk), tab1 weapon (WeapTypeWarhammer / Mace)
# None = vanilla never conditions that tab in the census, so the meaning is UNVERIFIED and
# perk_entry REFUSES a condition there (an owner condition can go on the perk-level
# conditions of perk_record instead). An entry point NOT in this table is REFUSED too.
PERK_ENTRY_TABS = {
    18: ('owner', 'weapon',   None),
    27: ('owner', 'weapon'),
    28: ('owner', 'weapon',   None),
    29: ('owner', 'spell',    'target'),
    30: ('owner', 'spell',    'target'),
    33: ('owner', None),
    35: ('owner', 'weapon',   'target'),
    36: ('owner', 'attacker', None),
    37: ('owner', 'weapon',   None),
    38: ('owner', 'spell'),
    39: ('owner',),
}
# Card C6 added 27 / 30 / 33 / 38 / 39 (census of Skyrim.esm 2026-11-09, tab counts from every vanilla entry of the entry point:
# 27 = 2, 30 = 3, 33 = 2, 38 = 2, 39 = 1; 18 = 3 and 28 = 3 as before):
#   27  ModPowerAttackStamina  tab1 weapon (WeapType* HasKeyword, FightingStance / ChampionsStance / EagleEye30)
#   30  ModSpellDuration       tab1 SPELL (funcs 693/696/713), tab2 target (715 / 560)
#   33  ModIncomingStagger     tab0 owner (func 682 WornHasKeyword, TowerOfStrength)
#   38  ModSpellCost           tab1 SPELL (funcs 713 x50, 696 x5, 560 x3)
#   39  ModPercentBlocked      tab0 owner (funcs 569 IsBlocking / 448 HasPerk / 682, ShieldWall*)
# TAB 0 = OWNER IS INFERRED, NOT CENSUS-PROVEN, for (18, 0), (27, 0), (30, 0) and (38, 0): vanilla never puts a condition on
# those tabs, but tab 0 is the owner in EVERY entry point where vanilla does condition it (28 29 33 35 36 37 39 41 57, 9 of 9,
# no counterexample), so a rank condition there is written. If the inference were wrong the faction-rank test would run
# against the wrong subject and fail CLOSED (the entry never fires), never open; the field test is the effect itself.
PERK_FUNC_FLOAT = {1: 'Set', 2: 'Add', 3: 'Multiply'}


class Cond(bytes):
    """A CTDA subrecord that remembers which tab KIND it was written for."""
    kind = None


def ctda(func, param1=0, op='==', value=1.0, kind=None, or_next=False, use_global=False, run_on=0, param2=0, ref=0):
    """One 32-byte CTDA subrecord (see the shape comment above). `kind` ('owner',
    'weapon', 'spell', 'target', 'attacker') is REQUIRED: perk_entry refuses a
    condition whose kind does not match the tab it is placed on."""
    assert kind in ('owner', 'weapon', 'spell', 'target', 'attacker'), f"ctda: condition kind {kind!r} not declared"
    typ = CTDA_OPS[op] | (0x01 if or_next else 0) | (0x04 if use_global else 0)
    c = Cond(subrec('CTDA', struct.pack('<B3xfH2xIIIII', typ, float(value), func, param1, param2, run_on, ref, 0xFFFFFFFF)))
    c.kind = kind
    return c


def class_rank_conditions(faction_fid, lo=1, hi=RANK_ACTIVE_MAX):
    """AND-pair on the perk OWNER: the follower's rank in faction_fid is in [lo, hi]. The
    default (1..99) means 'ACTIVE at any class level'; (L, 99) means 'level >= L';
    (100, 100) means 'mastered, unequipped'."""
    assert RANK_OFF < lo <= hi <= RANK_MASTERED_UNEQUIPPED
    return [ctda(COND_GET_FACTION_RANK, faction_fid, '>=', lo, kind='owner'),
            ctda(COND_GET_FACTION_RANK, faction_fid, '<=', hi, kind='owner')]


def perk_entry(entry_point, function, value, tab_conditions=None, priority=0, rank=0):
    """One entry-point entry. tab_conditions = {tab index: [ctda(...), ...]}; tabs with no
    conditions are omitted exactly as vanilla omits them. A condition on a tab whose kind
    is unverified (None) or differs from the condition's declared kind is REFUSED."""
    assert entry_point in PERK_ENTRY_TABS, \
        f"entry point {entry_point}: no vanilla-verified tab kinds (PERK_ENTRY_TABS); dump a vanilla perk using it first"
    assert function in PERK_FUNC_FLOAT, f"function {function}: only float Set/Add/Multiply are verified"
    kinds = PERK_ENTRY_TABS[entry_point]
    tabs = tab_conditions or {}
    assert all(0 <= t < len(kinds) for t in tabs), f"tab out of range for entry point {entry_point}"
    out = subrec('PRKE', struct.pack('<BBB', 2, rank, priority))
    out += subrec('DATA', struct.pack('<BBB', entry_point, function, len(kinds)))
    for t in sorted(tabs):
        assert tabs[t], f"tab {t} has no conditions"
        for c in tabs[t]:
            assert kinds[t] is not None, \
                f"entry point {entry_point} tab {t}: meaning unverified in vanilla, no condition allowed"
            assert getattr(c, 'kind', None) == kinds[t], \
                f"entry point {entry_point} tab {t} is the {kinds[t]} tab but the condition is for the {getattr(c, 'kind', None)}"
        out += subrec('PRKC', struct.pack('<B', t)) + b''.join(tabs[t])
    out += subrec('EPFT', struct.pack('<B', 1)) + subrec('EPFD', struct.pack('<f', float(value)))
    return out + subrec('PRKF', b'')


def perk_record(fid, edid, full, desc, entries, perk_conditions=None):
    """A PERK. DATA = trait 0, level 0, ranks 1, playable 1, hidden 0 (the exact
    bytes crReduceDamage075 / Overdraw00 carry). FULL/DESC are inline zstrings
    (this plugin is not localized, the MESG precedent)."""
    body = subrec('EDID', zstr(edid)) + subrec('FULL', zstr(full)) + subrec('DESC', zstr(desc))
    body += b''.join(perk_conditions or [])
    body += subrec('DATA', struct.pack('<BBBBB', 0, 0, 1, 1, 0))
    body += b''.join(entries)
    return record('PERK', fid, 0, body)


def make_reference_perk():
    """The ONE audited sample (MFOC_ReferencePerk). It exercises every condition
    kind C6 needs: faction rank (active range and the mastered marker), IsAttacking,
    HasKeyword on the weapon tab, HasMagicEffectKeyword on the target tab, and
    GetRandomPercent. Every magnitude is the NEUTRAL 1.0 and the perk is never
    granted, so it is inert by construction: a byte-shape reference for the audit,
    not a passive."""
    knight = CGID_PASSIVE_BASE + 3          # MFOC_P_Knight (Id ordinal 3), a real faction
    e1 = perk_entry(35, 3, 1.0, {
        0: class_rank_conditions(knight) + [ctda(COND_IS_ATTACKING, op='==', value=1.0, kind='owner')],
        1: [ctda(COND_HAS_KEYWORD, 0x0001E715, '==', 1.0, kind='weapon')],       # WeapTypeBow (Skyrim.esm)
        2: [ctda(COND_HAS_MAGIC_EFFECT_KEYWORD, CGID_MARKED_KYWD, '==', 1.0, kind='target')],   # target carries the Marked effect
    })
    e2 = perk_entry(36, 3, 1.0, {
        0: class_rank_conditions(knight, RANK_MASTERED_UNEQUIPPED, RANK_MASTERED_UNEQUIPPED)
           + [ctda(COND_GET_RANDOM_PERCENT, 0, '<=', 25.0, kind='owner')],
    })
    return perk_record(CGID_REFERENCE_PERK, "MFOC_ReferencePerk", "MFOC Reference Perk",
                       "Byte-shape reference for the class passives. Never granted; neutral magnitudes.",
                       [e1, e2])


# ── C6: the passive carriers (PERK per class, ability SPELs, marker SPELs) ──────────────────────
# ONE perk per class passive, granted ONCE to each managed follower by the DLL (classes/Carriers.cpp).
# Every effect is gated by the follower's rank in MFOC_P_<class> (RANK SPACE above), written as a
# condition on the ENTRY's tab 0 (the perk owner). Perk-LEVEL conditions are NOT used: they gate only
# the perk's availability in the tree UI, never the entries at run time.
#
# THE MAGNITUDE ROAD (marth 11-10: the T3 STRONG effect and the T4 UNIQUE scale linearly with class level,
# 25% at L1 -> 100% at the cap; the mastered-unequipped quarter is of the FULL value). No vanilla entry
# function reads a level, so a scaled effect is a LADDER: one entry per level, each conditioned on
# rank == L (two GetFactionRank CTDAs, class_rank_conditions(f, L, L)), carrying class_scale(L, cap) of
# the full delta, plus ONE entry at rank 100 carrying the quarter. The entries of one effect are
# pairwise disjoint (levels 1..cap, 100), so exactly one applies and Add / Multiply never stack.
# A FLAT effect (T1/T2 passive, T3 smaller effect) is two entries: rank [1, 99] full, rank 100 quarter.
# Entry count per scaled effect = cap + 1 (T3 21, T4 26); per flat effect 2. QUARTER ROUNDING (marth
# "equipped strength is good, halve passive with rounding"): quarter = round-half-up of 25% of the full
# delta in whole percentage points (Multiply) or whole levels (Add).
AV_CONJURATION, AV_DESTRUCTION, AV_ILLUSION, AV_RESTORATION = 19, 20, 21, 22   # ActorValue enum (CommonLibSSE-NG ActorValues.h)
AV_STAMINA, AV_SPEEDMULT, AV_STAMINARATEMULT = 26, 30, 157
# Vanilla keywords, each dumped from Skyrim.esm (tools/esp_inspect.py) and used by vanilla conditions.
KW_WEAP_SWORD, KW_WEAP_WARAXE, KW_WEAP_DAGGER, KW_WEAP_MACE, KW_WEAP_BOW = 0x0001E711, 0x0001E712, 0x0001E713, 0x0001E714, 0x0001E715
KW_WEAP_WARHAMMER, KW_WEAP_GREATSWORD, KW_WEAP_BATTLEAXE = 0x0006D930, 0x0006D931, 0x0006D932
KW_MAGIC_SHOUT, KW_MAGIC_RESTORE_HEALTH, KW_ACTOR_DAEDRA = 0x00046B99, 0x0001CEB0, 0x00013797
KW_SUMMONS = (0x00024823, 0x00024829, 0x0002482A, 0x0002482B, 0x001091CF)   # MagicSummonFire/Frost/Shock/Undead/Familiar
# (a Crossbow carries WeapTypeBow too: Dawnguard.esm DLC1CrossBowEnhanced KWDA = 0x1E715 0x1E719.)


def _or_keywords(kind, fids):
    """HasKeyword == 1 for any of fids (an OR chain: every CTDA but the last carries the OR bit)."""
    return [ctda(COND_HAS_KEYWORD, f, '==', 1.0, kind=kind, or_next=(i < len(fids) - 1)) for i, f in enumerate(fids)]


def _spell_school(av, no_shout=False):
    c = [ctda(COND_SPELL_HAS_SKILL, av, '==', 1.0, kind='spell')]
    if no_shout:   # Animage / KindredMage shape: a shout is not an Illusion spell here
        c.append(ctda(COND_SPELL_HAS_KEYWORD, KW_MAGIC_SHOUT, '==', 0.0, kind='spell'))
    return c


MELEE_WEAPON_KWS = (KW_WEAP_SWORD, KW_WEAP_WARAXE, KW_WEAP_DAGGER, KW_WEAP_MACE, KW_WEAP_WARHAMMER, KW_WEAP_GREATSWORD, KW_WEAP_BATTLEAXE)


def _fx(kind, ep, fn, delta, tabs=None, chance=None):
    """One effect. kind 'flat' | 'strong'. fn 3 = Multiply (entry value 1 + delta * strength), 2 = Add (delta * strength).
    chance: Living Legend's auto-block (entry value 0.0 = no damage; the strength scales the GetRandomPercent
    threshold, in percent, instead of the value)."""
    return dict(kind=kind, ep=ep, fn=fn, delta=delta, tabs=tabs or {}, chance=chance)


# slug -> (perk FULL, [effects]). DESC is generated from CLASS_CARRIER_NOTES. Classes absent here carry NO perk.
CLASS_PASSIVE_FX = {
    'knight':       ("Knight passive",       [_fx('flat', 39, 3, +0.15)]),
    'warrior':      ("Warrior passive",      [_fx('flat', 35, 3, +0.15, {1: _or_keywords('weapon', MELEE_WEAPON_KWS)})]),
    'archer':       ("Archer passive",       [_fx('flat', 35, 3, +0.15, {1: _or_keywords('weapon', (KW_WEAP_BOW,))})]),
    'evoker':       ("Evoker passive",       [_fx('flat', 29, 3, +0.15, {1: _spell_school(AV_DESTRUCTION)})]),
    'conjurer':     ("Conjurer passive",     [_fx('flat', 38, 3, -0.15, {1: _spell_school(AV_CONJURATION)})]),
    'healer':       ("Healer passive",       [_fx('flat', 38, 3, -0.15, {1: _spell_school(AV_RESTORATION)})]),
    'warlock':      ("Warlock passive",      [_fx('flat', 29, 2, +8.0, {1: _spell_school(AV_ILLUSION, True)})]),
    'great-knight': ("Shieldwall",           [_fx('strong', 33, 3, -0.50), _fx('flat', 39, 3, +0.10)]),
    'champion':     ("Champion passive",     [_fx('strong', 28, 3, +0.20), _fx('flat', 27, 3, -0.10)]),
    'blademaster':  ("Blademaster passive",  [_fx('strong', 35, 3, +0.15), _fx('flat', 27, 3, -0.10)]),
    'ranger':       ("Ranger passive",       [_fx('strong', 37, 3, -0.15, {1: _or_keywords('weapon', (KW_WEAP_BOW,))})]),
    'master-thief': ("Master Thief passive", [_fx('strong', 18, 3, +0.50, {1: _or_keywords('weapon', (KW_WEAP_DAGGER, KW_WEAP_BOW))})]),
    'wizard':       ("Wizard passive",       [_fx('flat', 38, 3, -0.08)]),
    'priest':       ("Priest passive",       [_fx('strong', 29, 3, +0.20,
                                                {1: [ctda(COND_SPELL_HAS_KEYWORD, KW_MAGIC_RESTORE_HEALTH, '==', 1.0, kind='spell')]})]),
    'spellsword':   ("Spellstrike",          [_fx('flat', 38, 3, -0.10, {1: _spell_school(AV_DESTRUCTION)})]),
    'sorcerer':     ("Sorcerer passive",     [_fx('strong', 30, 3, +1.00,
                                                {1: [ctda(COND_SPELL_HAS_KEYWORD, f, '==', 1.0, kind='spell', or_next=(i < len(KW_SUMMONS) - 1))
                                                     for i, f in enumerate(KW_SUMMONS)]})]),
    'templar':      ("Templar passive",      [_fx('flat', 38, 3, -0.10, {1: _spell_school(AV_RESTORATION)})]),
    'nightblade':   ("Nightblade passive",   [_fx('strong', 29, 2, +16.0, {1: _spell_school(AV_ILLUSION, True)})]),
    'witchhunter':  ("Witchhunter passive",  [_fx('strong', 29, 2, +18.0, {1: _spell_school(AV_ILLUSION, True),
                                                2: [ctda(COND_IS_UNDEAD, 0, '==', 1.0, kind='target', or_next=True),
                                                    ctda(COND_HAS_KEYWORD, KW_ACTOR_DAEDRA, '==', 1.0, kind='target')]})]),
    'scout':        ("Scout passive",        [_fx('flat', 38, 3, -0.15, {1: _spell_school(AV_RESTORATION)})]),
    'living-legend': ("Living Legend passive", [_fx('strong', 36, 3, 0.0, chance=40.0)]),
}

# What each class carries, for the perk DESC, the report and the DLL's one-time log. 'real' = a carrier ships;
# 'missing' = NO road yet (the DLL logs it once, loudly, a documented degrade, never a mask).
CLASS_CARRIER_NOTES = {
    'knight': "Blocking stops 15% more damage (ModPercentBlocked x1.15).",
    'warrior': "Melee weapon damage +15% (ModAttackDamage on the seven melee WeapType keywords).",
    'mercenary': "Ability spells: +40 stamina, +25% stamina regeneration.",
    'archer': "Bow and crossbow damage +15% (ModAttackDamage, WeapTypeBow).",
    'evoker': "Destruction magnitude +15% (ModSpellMagnitude, spell skill Destruction).",
    'conjurer': "Conjuration spells cost 15% less (ModSpellCost). MISSING: the low chance of being targeted.",
    'healer': "Restoration spells cost 15% less (ModSpellCost). MISSING: the low chance of being targeted.",
    'warlock': "Illusion level-capped spells reach 8 levels higher (ModSpellMagnitude Add, like Animage).",
    'great-knight': "Shieldwall: staggered 50% less (scaled by level) and blocking stops 10% more damage.",
    'champion': "Power attacks +20% damage (scaled by level) and 10% less power attack stamina.",
    'blademaster': "+15% weapon damage (scaled by level). PARTIAL: only the power attack stamina cost -10% is carried, not every stamina cost.",
    'ranger': "+15% armor penetration with bows (ModTargetDamageResistance x0.85, scaled by level).",
    'master-thief': "Sneak attacks x1.5 with dagger and bow (ModSneakAttackMult, scaled by level).",
    'wizard': "All spells cost 8% less (ModSpellCost). MISSING: Destruction +20% on a controlled foe (no controlled marker exists).",
    'priest': "Healing +20% (ModSpellMagnitude on MagicRestoreHealth, scaled by level). MISSING: the sun cloak.",
    'spellsword': "Spellstrike: Destruction 10% cheaper (NOT gated on a weapon in hand). MISSING: the +10 element damage on melee hits.",
    'sorcerer': "Summons last twice as long (ModSpellDuration on the MagicSummon* keywords, scaled by level). MISSING: 20% less damage while all summons are out.",
    'templar': "Restoration 10% cheaper (ModSpellCost). MISSING: the weapon always counting as silver.",
    'nightblade': "Illusion level-capped spells reach 16 levels higher (scaled by level). MISSING: +15% melee damage on controlled foes.",
    'witchhunter': "Illusion level-capped spells reach 18 levels higher on undead and Daedra (scaled by level). MISSING: the immunity lift and the on-hit drain.",
    'scout': "+20% movement speed (ability spells, scaled by level) and Restoration 15% cheaper (ModSpellCost).",
    'living-legend': "Unique: an incoming hit lands as nothing 40% of the time (scaled by level) while attacking (ModIncomingDamage x0, IsAttacking + GetRandomPercent).",
}


def _quarter(delta, fn):
    """Round-half-up quarter of a full delta: whole percentage points for Multiply, whole levels for Add."""
    if fn == 3:
        return math.copysign(math.floor(abs(delta) * 25.0 + 0.5) / 100.0, delta)
    return math.copysign(math.floor(abs(delta) * 0.25 + 0.5), delta)


def class_effect_entries(fx, faction_fid, cap):
    """The perk entries of ONE effect (see the road comment above). Returns a list of perk_entry bytes."""
    out = []
    def one(lo, hi, strength, quarter):
        owner = list(class_rank_conditions(faction_fid, lo, hi))
        if fx['chance'] is not None:    # Living Legend: IsAttacking + a random threshold that carries the strength
            pct = (math.floor(fx['chance'] * 0.25 + 0.5) if quarter else fx['chance'] * strength)
            owner += [ctda(COND_IS_ATTACKING, op='==', value=1.0, kind='owner'),
                      ctda(COND_GET_RANDOM_PERCENT, 0, '<=', float(pct), kind='owner')]
            value = 0.0
        else:
            d = _quarter(fx['delta'], fx['fn']) if quarter else fx['delta'] * strength
            value = (1.0 + d) if fx['fn'] == 3 else d
        tabs = {0: owner}
        tabs.update(fx['tabs'])
        out.append(perk_entry(fx['ep'], fx['fn'], value, tabs))
    if fx['kind'] == 'flat':
        one(1, RANK_ACTIVE_MAX, 1.0, False)
    else:
        for lvl in range(1, cap + 1):
            one(lvl, lvl, class_scale(lvl, cap), False)
    one(RANK_MASTERED_UNEQUIPPED, RANK_MASTERED_UNEQUIPPED, 0.25, True)
    return out


def make_offclass_perk():
    """Card G16 (ClickUp 1dfcuabcn02): the off-class weapon damage penalty. ONE ModAttackDamage entry (entry point 35, tab 0 = the
    perk owner, function 3 Multiply) of 0.5, conditioned on the owner's rank in MFOC_OffClassWeapon being >= 1. MFO writes that
    rank (classes/Carriers.cpp SetOffClassWeapon) only while the follower HOLDS the fallback weapon the equip authority allowed
    (an off-class weapon he carries because he would otherwise be unarmed). Rank 0 / not a member = the entry never fires."""
    e = perk_entry(35, 3, 0.5, {0: [ctda(COND_GET_FACTION_RANK, CGID_OFFCLASS_FACT, '>=', 1.0, kind='owner')]})
    return perk_record(CGID_OFFCLASS_PERK, "MFOC_OffClassHalf", "Off-class weapon",
                       "Damage is halved while the follower fights with a weapon outside his class (he had no other).", [e])


def make_class_passive_perks(table):
    """One PERK per class that has a perk effect. FULL = the passive's name, DESC = CLASS_CARRIER_NOTES."""
    recs, counts = b'', {}
    by_slug = {r['slug']: r for r in table}
    for slug in list(CLASS_PASSIVE_FX) + list(CLASS_CARRIER_NOTES):
        if slug not in by_slug:
            raise SystemExit(f"C6 carrier plan names class '{slug}', which is not in native/classes/ClassTable.cpp")
    for slug, (full, fxs) in CLASS_PASSIVE_FX.items():
        if slug not in CLASS_CARRIER_NOTES:
            raise SystemExit(f"C6 carrier plan: class '{slug}' has a perk but no CLASS_CARRIER_NOTES entry")
        row = by_slug[slug]
        cap = CLASS_LEVEL_CAP[row['tier']]
        faction = CGID_PASSIVE_BASE + row['idx']
        entries = [e for fx in fxs for e in class_effect_entries(fx, faction, cap)]
        counts[slug] = len(entries)
        recs += perk_record(CGID_PASSIVE_PERK_BASE + row['idx'], "MFOC_Passive_" + cls_edid_name(row), full,
                            CLASS_CARRIER_NOTES[slug], entries)
    return recs, counts


# ── ability SPELs + marker carrier SPELs ────────────────────────────────────
def cls_ability_mgef(fid, edid, av):
    """A Peak Value Modifier MGEF for a Constant-Effect self ability. The vanilla twins are AbFortifyStamina
    (0x00091F77: flags 0x00208802, archetype 34, AV 26) and AbFortifyStaminaRate (0x00105F21: AV 157); casting
    type 0 (Constant), delivery 0 (Self). Every other DATA field stays the inert-self base (make_mgef)."""
    d = bytearray(mgef_inert_self_data())
    struct.pack_into('<I', d, 0, 0x00208802)
    struct.pack_into('<I', d, 64, 34)
    struct.pack_into('<i', d, 68, av)
    struct.pack_into('<I', d, 80, 0)
    struct.pack_into('<I', d, 84, 0)
    body = subrec('EDID', zstr(edid)) + subrec('FULL', zstr("")) + subrec('MDOB', struct.pack('<I', 0))
    body += subrec('DATA', bytes(d)) + subrec('SNDD', b'') + subrec('DNAM', zstr(""))
    return record('MGEF', fid, 0, body)


FREF_EITHER_HAND = 0x00013F44   # EQUP EitherHand, what every vanilla ability / spell SPEL carries


def cls_ability_spel(fid, edid, full, desc, effects):
    """An Ability SPEL (SPIT type 4, cast type 0 Constant, delivery 0 Self, flags 0x00250001: exactly the vanilla
    PerkWindWalkerStamina 0x00105F20 bytes). effects = [(mgef_fid, magnitude)]."""
    body = subrec('EDID', zstr(edid)) + subrec('OBND', b'\x00' * 12) + subrec('FULL', zstr(full))
    body += subrec('ETYP', struct.pack('<I', FREF_EITHER_HAND)) + subrec('DESC', zstr(desc))
    body += subrec('SPIT', struct.pack('<fIIfIIffI', 0.0, 0x00250001, 4, 0.0, 0, 0, 0.0, 0.0, 0))
    for mg, mag in effects:
        body += subrec('EFID', struct.pack('<I', mg)) + subrec('EFIT', struct.pack('<fII', float(mag), 0, 0))
    return record('SPEL', fid, 0, body)


def cls_marker_spel(fid, edid, full, desc, mgef_fid, casting, delivery, duration):
    """A carrier SPELL for a marker MGEF (B619). Type 0 Spell, the SAME casting type and delivery as its MGEF
    (Marked FF + Contact, Flanked FF + Target Actor, Guarded Concentration + Aimed), flags 1 (manual cost), cost 0."""
    body = subrec('EDID', zstr(edid)) + subrec('OBND', b'\x00' * 12) + subrec('FULL', zstr(full))
    body += subrec('ETYP', struct.pack('<I', FREF_EITHER_HAND)) + subrec('DESC', zstr(desc))
    body += subrec('SPIT', struct.pack('<fIIfIIffI', 0.0, 1, 0, 0.0, casting, delivery, 0.0, 0.0, 0))
    body += subrec('EFID', struct.pack('<I', mgef_fid)) + subrec('EFIT', struct.pack('<fII', 0.0, 0, duration))
    return record('SPEL', fid, 0, body)


MERC_STAMINA, MERC_REGEN = 40.0, 25.0       # full values; the quarter is round-half-up of 25%
SCOUT_SPEED = 20.0                          # full SpeedMult percent, scaled by class_scale(level, 20)


def make_class_abilities():
    """(MGEF group bytes, SPEL group bytes) for the actor-value carriers: Mercenary (2 spells) and Scout (21)."""
    mg = (cls_ability_mgef(CGID_AB_STAMINA_MGEF, "MFOC_AbStamina", AV_STAMINA)
          + cls_ability_mgef(CGID_AB_STAMINARATE_MGEF, "MFOC_AbStaminaRate", AV_STAMINARATEMULT)
          + cls_ability_mgef(CGID_AB_SPEED_MGEF, "MFOC_AbSpeed", AV_SPEEDMULT))
    q = lambda v: float(math.floor(v * 0.25 + 0.5))
    sp = cls_ability_spel(CGID_MERC_FULL_SPELL, "MFOC_Mercenary_Full", "Mercenary passive",
                          "Mercenary: +40 stamina, +25% stamina regeneration.",
                          [(CGID_AB_STAMINA_MGEF, MERC_STAMINA), (CGID_AB_STAMINARATE_MGEF, MERC_REGEN)])
    sp += cls_ability_spel(CGID_MERC_QUARTER_SPELL, "MFOC_Mercenary_Quarter", "Mercenary passive (mastered)",
                           "Mercenary, mastered: a quarter of the passive.",
                           [(CGID_AB_STAMINA_MGEF, q(MERC_STAMINA)), (CGID_AB_STAMINARATE_MGEF, q(MERC_REGEN))])
    for lvl in range(1, CLASS_LEVEL_CAP[3] + 1):
        mag = round(SCOUT_SPEED * class_scale(lvl, CLASS_LEVEL_CAP[3]), 2)
        sp += cls_ability_spel(CGID_SCOUT_SPEED_BASE + lvl - 1, f"MFOC_Scout_Speed{lvl:02d}", "Scout passive",
                               f"Scout, level {lvl}: +{mag:g}% movement speed.", [(CGID_AB_SPEED_MGEF, mag)])
    sp += cls_ability_spel(CGID_SCOUT_QUARTER_SPELL, "MFOC_Scout_Quarter", "Scout passive (mastered)",
                           "Scout, mastered: a quarter of the passive.", [(CGID_AB_SPEED_MGEF, q(SCOUT_SPEED))])
    return mg, sp


def make_marker_spells():
    return (cls_marker_spel(CGID_MARKED_SPELL, "MFOC_MarkedSpell", "Marked", "Carrier for the Marked effect (10 s).",
                            CGID_MARKED_MGEF, CAST_FF, DELIV_CONTACT, 10)
            + cls_marker_spel(CGID_FLANKED_SPELL, "MFOC_FlankedSpell", "Flanked", "Carrier for the Flanked effect (5 s).",
                              CGID_FLANKED_MGEF, CAST_FF, DELIV_TARGET_ACTOR, 5)
            + cls_marker_spel(CGID_GUARDED_SPELL, "MFOC_GuardedSpell", "Guarded", "Carrier for the Guarded effect (concentration).",
                              CGID_GUARDED_MGEF, CAST_CONCENTRATION, DELIV_AIMED, 0))


# ── FACT / MGEF / CSTY ──────────────────────────────────────────────────────
def cls_fact(fid, edid):
    """A marker faction: EDID, DATA (flags 0), CRVA. Shape = vanilla form-version-44
    FACT (EDID DATA CRVA, CRVA 20 bytes '01 01' then zeros, as 926 vanilla records)."""
    body = subrec('EDID', zstr(edid)) + subrec('DATA', struct.pack('<I', 0))
    body += subrec('CRVA', bytes([1, 1]) + b'\x00' * 18)
    return record('FACT', fid, 0, body)


CAST_FF, CAST_CONCENTRATION = 1, 2          # MGEF DATA casting type (offset 80)
DELIV_CONTACT, DELIV_AIMED, DELIV_TARGET_ACTOR = 1, 2, 3   # MGEF DATA delivery (offset 84)


def cls_marker_mgef(fid, edid, full, kywd_fid, casting, delivery):
    """A SCRIPT-LESS marker MGEF: Script archetype, no VMAD, hidden in the UI,
    carrying one keyword (so a perk can test HasMagicEffectKeyword). DATA is the
    152-byte layout of make_mgef (mgef_inert_self_data) with the casting type and
    delivery set per marker (see the callers for the vanilla precedent each follows).
    Order = vanilla EDID FULL MDOB KSIZ KWDA DATA SNDD DNAM. DNAM is an inline
    zstring (non-localized plugin). Any SPEL carrying one must use the SAME casting
    type and delivery (vanilla pairs them on both records)."""
    d = bytearray(mgef_inert_self_data())
    struct.pack_into('<I', d, 80, casting)
    struct.pack_into('<I', d, 84, delivery)
    body = subrec('EDID', zstr(edid)) + subrec('FULL', zstr(full)) + subrec('MDOB', struct.pack('<I', 0))
    body += subrec('KSIZ', struct.pack('<I', 1)) + subrec('KWDA', struct.pack('<I', kywd_fid))
    body += subrec('DATA', bytes(d)) + subrec('SNDD', b'') + subrec('DNAM', zstr(""))
    return record('MGEF', fid, 0, body)


# Class CSTY bases: the MFO stance styles, VERBATIM (make_csty). Asserted against
# make_csty's own bytes in make_class_styles, so a retune there fails here.
CLS_CSCR = {1: bytes.fromhex('48e13a3f3d0ad73ecdcc4c3ecdcc4c3e'),    # csHumanMeleeLvl1
            2: bytes.fromhex('6666e63e6666263fcdcc4c3ecdcc4c3e'),    # csHumanMissile
            3: bytes.fromhex('9a99993e0000003fcdcc4c3ecdcc4c3e')}    # csHumanMagic
CLS_CSGD = {1: (1.0, 0.5, 1.0, 10.0, 0.1, 0.1, 1.0, 0.1, 0.1, 0.0),
            2: (1.0, 0.5, 1.0, 0.1, 0.1, 10.0, 1.0, 0.1, 0.1, 0.5),
            3: (1.0, 0.5, 1.0, 0.1, 10.0, 0.2, 1.0, 0.1, 1.0, 0.2)}


# Per-class Avoid Threat chance (CSGD's tenth value). The engine's own side step (CombatBehaviorDodgeThreat) rolls against this
# chance, read through Actor::GetCombatStyle, which Harbinger ch.33 answers for the class's actor-wide style claim; a dodge
# framework that scores from the style reads the same value. Vanilla Skyrim.esm gives EVERY human combat style 0.20 (read
# 2026-11-12: csHumanMeleeLvl1-7, csHumanMissile, csHumanMagic, the bosses, the WE* styles), so that is the default.
# marth 2026-11-12 (Q-S-3, superseding the first numbers): "0.2 for both mage and archer (and any other valid combat style we
# have), 0.35+ for mercenary line"; "the thief 0.25 and master thief 0.3"; "They can have a bow drawn boost, but not that high.
# Knight line does not get dodging, no class with heavy armor equipped dodges".
#   Mercenary line 0.35 / 0.50 / 0.55 (Mercenary, Blademaster, Living Legend; marth 11-12), Thief 0.25, Master Thief 0.30, the Knight
#   line (Knight, Great Knight, Ebony Warrior) 0.0, every other class the vanilla 0.20. A SET, applied to every stance of the
#   class (it replaces the MFO stance bases 0 / 0.5 / 0.2 and the old Disengage +0.20).
#   BOW DRAWN: the ranged-stance style of a class with a value above 0 gets +AVOID_THREAT_BOW_BOOST (0.10, ADDED; marth 11-12).
#   HEAVY ARMOR: a follower WEARING heavy body armor dodges not at all, whatever his class. That is runtime, not data: every
#   style with an Avoid Threat above 0 has a ZERO-AVOID TWIN (same record, avoidThreat 0; make_class_styles) and
#   apmf/StyleClaims.cpp claims the twin while he wears heavy armor.
AVOID_THREAT_VANILLA = 0.20
AVOID_THREAT_BOW_BOOST = 0.10
CLS_AVOID_THREAT = {'mercenary': 0.35, 'blademaster': 0.50, 'living-legend': 0.55,
                    'thief': 0.25, 'master-thief': 0.30,
                    'knight': 0.0, 'great-knight': 0.0, 'ebony-warrior': 0.0}


def class_csty_params(stance, flags, flagbits, slug=None):
    """(csgd tuple, cscr bytes) for a class: the stance base plus TWO small,
    table-driven positioning offsets. C7 BASELINE, not tuning: C8 / the field
    retune these (xEdit-editable, one record per class per wield).
      Flag.kShortLeash (the wall classes)  circle -0.10, fallback -0.15  (hold the line)
      Flag.kDisengage  (Thief + mage line) fallback +0.15.
    Avoid Threat is SET per class (CLS_AVOID_THREAT, else the vanilla 0.20), +0.10 in the ranged stance when above 0."""
    csgd = list(CLS_CSGD[stance])
    circle, fallback, flank, stalk = struct.unpack('<4f', CLS_CSCR[stance])
    if flags & flagbits['kShortLeash']:
        circle, fallback = max(0.0, circle - 0.10), max(0.0, fallback - 0.15)
    if flags & flagbits['kDisengage']:
        fallback = min(1.0, fallback + 0.15)
    csgd[9] = CLS_AVOID_THREAT.get(slug, AVOID_THREAT_VANILLA)
    if stance == 2 and csgd[9] > 0.0:
        csgd[9] += AVOID_THREAT_BOW_BOOST
    return tuple(csgd), struct.pack('<4f', circle, fallback, flank, stalk)


def class_weapon_bits():
    """{'kDagger': 1, ...} from the Weapon:: namespace of Classes.h."""
    hdr, _src = _cls_source()
    return _cls_named_bits(hdr, 'Weapon')


# STYLE SLOTS: a class's styles are looked up through its OWN FLST, never by position in
# a global run (card C8 consumes this; the DLL must not hard-code CSTY ids).
#   MFOC_ClassStyles          (0x803)  35 entries, the per-class FLSTs in class-Id order
#   MFOC_ClassStyles_<Name>   (0x880+Id) entry[0] = GLOB MFOC_StyleMask_<Name> (0x8C0+Id),
#                              entry[1..] = that class's CSTYs in ASCENDING SLOT-BIT order,
#                              exactly the slots the GLOB's bitmask names:
#                              1 = melee single-wield, 2 = melee DUAL twin (kAllowDualWielding),
#                              4 = ranged, 8 = cast. The mask GLOB's value is read at runtime
#                              (GLOB values persist, editor-ids do not); popcount(mask) ==
#                              entries - 1 (tools/audit_esp.py checks it).
STYLE_SLOT_MELEE, STYLE_SLOT_MELEE_DUAL, STYLE_SLOT_RANGED, STYLE_SLOT_CAST = 1, 2, 4, 8


def class_stances(row, wb):
    """The stances (1 melee, 2 ranged, 3 cast) a class can actually use, DERIVED from its
    table data, never hand-picked:
      - its root stance (State.h combatClassOverride; T1 and the pure lines), plus
      - every stance its ALLOWED-WEAPON bits can reach (dagger / 1H / 2H -> melee, bow /
        crossbow -> ranged, staff -> cast; kUnrestricted adds nothing, T1 keeps its root), plus
      - cast for every hybrid (Line::Hybrid: weapon side + school), plus
      - all three when the class has NO fixed root (Dragonborn)."""
    st = set()
    if row['root']:
        st.add(row['root'])
    w = row['weapons']
    if not (w & wb['kUnrestricted']):
        if w & (wb['kDagger'] | wb['kOneHand'] | wb['kTwoHand']):
            st.add(1)
        if w & (wb['kBow'] | wb['kCrossbow']):
            st.add(2)
        if w & wb['kStaff']:
            st.add(3)
    if row['line'] == 'Hybrid':
        st.add(3)
    if row['root'] == 0:
        st |= {1, 2, 3}
    return sorted(st)


def make_class_styles(table, flagbits):
    """CSTY records, one per (class, usable stance); a DUAL twin (DATA 1|4) only for the
    MELEE stance, where kAllowDualWielding matters. Returns (CSTY records, {idx: (mask,
    [fids in slot-bit order])}); CSTY ids are allocated sequentially from CGID_STYLE_BASE."""
    mfo = make_csty()
    for edid, fid, stance, df in (("MFO_MeleeStyle", FID_MELEE_STYLE, 1, 1 | 4),
                                  ("MFO_MeleeSingleStyle", FID_MELEE_SINGLE_STYLE, 1, 1),
                                  ("MFO_RangedStyle", FID_RANGED_STYLE, 2, 1),
                                  ("MFO_CastStyle", FID_CAST_STYLE, 3, 1)):
        assert _csty_record(edid, fid, CLS_CSGD[stance], CLS_CSCR[stance], df) in mfo, \
            f"class CSTY base for {edid} drifted from make_csty -- update CLS_CSGD/CLS_CSCR with it"
    wb = class_weapon_bits()
    body, per_class, nxt = b'', {}, CGID_STYLE_BASE
    body_tw, tnext = b'', CGID_STYLE_TWIN_BASE
    for row in table:
        nm = cls_edid_name(row)
        mask, fids, twins = 0, [], []
        for stance in class_stances(row, wb):
            csgd, cscr = class_csty_params(stance, row['flags'], flagbits, row['slug'])
            variants = ([(STYLE_SLOT_MELEE, "Melee", 1), (STYLE_SLOT_MELEE_DUAL, "MeleeDual", 1 | 4)]
                        if stance == 1 else
                        [(STYLE_SLOT_RANGED, "Ranged", 1)] if stance == 2 else [(STYLE_SLOT_CAST, "Cast", 1)])
            for slot, label, df in variants:
                body += _csty_record(f"MFOC_Style_{nm}_{label}", nxt, csgd, cscr, data_flags=df)
                mask |= slot
                fids.append(nxt)
                nxt += 1
                if csgd[9] > 0.0:   # the zero-avoid twin: byte-identical but avoidThreat 0 (the twin entry IS the style when none is needed)
                    tcsgd = csgd[:9] + (0.0,)
                    body_tw += _csty_record(f"MFOC_StyleNoDodge_{nm}_{label}", tnext, tcsgd, cscr, data_flags=df)
                    twins.append(tnext)
                    tnext += 1
                else:
                    twins.append(fids[-1])
        per_class[row['idx']] = (mask, fids, twins)
    assert nxt <= OWN_CLS | 0xA00, "class CSTY run overran the 0xA00 perk id"
    assert tnext <= OWN_CLS | 0xB00, "class CSTY twin run overran the 0xB00 twin list ids"
    return body + body_tw, per_class


# ── the class add-on's OWN MCM (card C11) ───────────────────────────────────────────────────────────────
# Same shape as the progression add-on's (make_prog_mcm_quest / prog_mcm_config): a start-game-enabled quest whose VMAD names an
# empty MCM_ConfigBase shim (MFOC_MCM), a config.json under MCM/Config/MFO_Classes, a settings.ini of defaults and a SEQ. The
# controls are ModSetting*, so MCM Helper writes Data/MCM/Settings/MFO_Classes.ini, which MFO.dll's Config::Read ingests after the
# MFO store (live on MCM close). Key names are FROZEN once shipped (MCM Helper persistence identity): they are Config.cpp's.
# (key, label, help, default); toggles and floats are kept apart because the sourceType differs.
CLS_MCM_TOGGLES = [
    ("bClassTree", "Class tree",
     "ON (default): the 35-class tree (tier 1 to 4, mastery, free switching) is active for the followers you give a class. OFF: "
     "today's three classes (Melee, Ranged, Mage) exactly; stored class progress is kept untouched. Needs this plugin loaded.", 1),
    ("bClassStrict", "Strict class adherence",
     "ON (default): a follower with a class equipped follows it strictly (his gambits, weapon and spell choices stay inside the "
     "class). OFF: the class only adds its passive. Only means anything while the class tree is ON.", 1),
    ("bClassInfo", "Class info in combat",
     "ON (default): show the follower's class and level in the combat display. OFF: hide it. (The setting is stored now; the "
     "display itself is not built yet.)", 1),
]
CLS_MCM_FLOATS = [
    ("fClassXpRate", "Class XP rate",
     "Multiplier on all class XP. 1.0 (default) is the sized pace: a tier 1 class masters in about 3 hours of play, tier 2 in 6, "
     "tier 3 in 10, tier 4 in 13. 2.0 halves those hours, 0 stops class XP.", 0.0, 5.0, 0.1, 1.0),
    ("fPriestSunPerSec", "Priest sun damage per second",
     "Damage per second the Priest's sun does to undead and Daedra within 6 metres (default 2).", 0.0, 20.0, 0.5, 2.0),
    ("fSaintHealPerSec", "Saint aura heal per second",
     "Health per second the Saint's healing aura gives allies (default 30).", 0.0, 200.0, 1.0, 30.0),
    ("fSaintUndeadPerSec", "Saint aura damage per second",
     "Damage per second the Saint's aura does to undead (default 30).", 0.0, 200.0, 1.0, 30.0),
]


def make_cls_mcm_quest():
    """MFOC_MCMQuest: start-game-enabled, NOT run-once, zero VMAD properties, script MFOC_MCM (an empty MCM_ConfigBase shim)."""
    vmad = VMADBuilder()
    vmad.add_script("MFOC_MCM", [])
    body = subrec('EDID', zstr("MFOC_MCMQuest")) + subrec('VMAD', vmad.build())
    body += subrec('FULL', zstr("MFO Classes"))
    body += subrec('DNAM', qust_dnam(0x0011)) + subrec('NEXT', b'') + subrec('ANAM', struct.pack('<I', 0))
    return record('QUST', CGID_MCM_QUEST, 0, body)


def cls_mcm_config():
    content = [{"text": "Classes", "type": "header"}]
    for key, label, help_, dflt in CLS_MCM_TOGGLES:
        content.append({"id": f"{key}:Classes", "text": label, "type": "toggle", "help": help_,
                        "valueOptions": {"sourceType": "ModSettingBool", "defaultValue": dflt}})
    for key, label, help_, mn, mx, step, dflt in CLS_MCM_FLOATS:
        content.append({"id": f"{key}:Classes", "text": label, "type": "slider", "help": help_,
                        "valueOptions": {"min": mn, "max": mx, "step": step, "sourceType": "ModSettingFloat",
                                         "defaultValue": dflt}})
    return {
        "modName": "MFO_Classes",
        "displayName": "MFO Classes",
        "minMcmVersion": 9,
        "cursorFillMode": "topToBottom",
        "pages": [{"pageDisplayName": "Classes", "cursorFillMode": "topToBottom", "content": content}],
    }


def write_cls_mcm_files(out_dir):
    """Emit Data/MCM/Config/MFO_Classes/{config.json,settings.ini} (the defaults MCM Helper registers from)."""
    cdir = os.path.join(out_dir, 'MCM', 'Config', 'MFO_Classes')
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(cdir, 'config.json'), 'w') as f:
        json.dump(cls_mcm_config(), f, indent='\t')
    with open(os.path.join(cdir, 'settings.ini'), 'w', encoding='utf-8-sig') as f:
        f.write("[Classes]\n")
        for key, label, help_, dflt in CLS_MCM_TOGGLES:
            f.write(f"{key}={int(dflt)}\n")
        for key, label, help_, mn, mx, step, dflt in CLS_MCM_FLOATS:
            f.write(f"{key}={dflt:.6f}\n")
    return cdir


def make_classes_tes4():
    # Same header as make_prog_tes4: ONE master (Skyrim.esm), ESL flag 0x200.
    hedr = struct.pack('<f', 1.70) + struct.pack('<I', 100) + struct.pack('<I', CLS_NEXT_OBJECT_ID)
    body = subrec('HEDR', hedr) + subrec('CNAM', zstr("marth"))
    body += subrec('SNAM', zstr("MFO class add-on (optional; detected at runtime)"))
    body += subrec('MAST', zstr("Skyrim.esm")) + subrec('DATA', struct.pack('<Q', 0))
    return record('TES4', 0, 0x00000200, body)


# (fid, edid, default, FNAM). The MCM-tunable values the plan names (C4 XP rate,
# C11 /s values). The editor-id is the contract (matched by suffix like the
# economy GLOBs). Defaults: XP rate 1.0 (a multiplier on the sized curve), Priest
# sun 2/s, Saint +30 HP/s to allies and 30/s on undead (class-walkthrough ruling).
CLS_GLOBS = [
    (CGID_XP_RATE_GLOB, "MFOC_XpRate",          1.0,  'f'),
    (CGID_PRIEST_SUN,   "MFOC_PriestSunDps",    2.0,  'f'),
    (CGID_SAINT_HEAL,   "MFOC_SaintHealPerSec", 30.0, 'f'),
    (CGID_SAINT_UNDEAD, "MFOC_SaintUndeadDps",  30.0, 'f'),
]


def make_classes_esl():
    table = load_class_table()
    flagbits = class_flag_bits()
    data = make_classes_tes4()
    # Top-group order = Skyrim.esm's (walked 2026-11-09): KYWD GLOB FACT MGEF CSTY FLST PERK.
    kw = lambda fid, edid: record('KYWD', fid, 0, subrec('EDID', zstr(edid)) + subrec('CNAM', struct.pack('<I', 0)))
    data += group('KYWD', kw(CGID_MANIFEST_KYWD, "MFOC_MFOClassesManifest")
                  + kw(CGID_MARKED_KYWD, "MFOC_MarkedKeyword")
                  + kw(CGID_FLANKED_KYWD, "MFOC_FlankedKeyword")
                  + kw(CGID_GUARDED_KYWD, "MFOC_GuardedKeyword"))
    ability_mgefs, ability_spels = make_class_abilities()
    styles, per_class = make_class_styles(table, flagbits)   # needed now: the mask GLOBs live in the GLOB group
    globs = prog_glob(CGID_VERSION_GLOB, "MFOC_Version", CLS_VERSION_STAMP, 's')
    for fid, edid, value, fnam in CLS_GLOBS:
        globs += prog_glob(fid, edid, value, fnam)
    for row in table:
        globs += prog_glob(CGID_STYLE_MASK_BASE + row['idx'], "MFOC_StyleMask_" + cls_edid_name(row),
                           float(per_class[row['idx']][0]), 's')
    data += group('GLOB', globs)
    facts = cls_fact(CGID_OFFCLASS_FACT, "MFOC_OffClassWeapon") + cls_fact(CGID_MARKPEND_FACT, "MFOC_MarkPending")
    faction_fids = []
    for row in table:
        fid = CGID_PASSIVE_BASE + row['idx']
        faction_fids.append(fid)
        facts += cls_fact(fid, "MFOC_P_" + cls_edid_name(row))
    data += group('FACT', facts)
    # Casting type + delivery follow the VANILLA pairing (census of Skyrim.esm 2026-11-09, SPEL and
    # MGEF carry the same pair):
    #  - Marked  = Fire-and-Forget + Contact: every ApplyCombatHitSpell (entry point 52) spell, 4/4,
    #    is FF + Contact on both records, and a mark is applied by the marker-holder's hit.
    #  - Guarded = Concentration + Aimed: every cloak-applied spell (MGEF archetype Cloak), 13/13
    #    SPELs and 18 MGEFs, is Concentration + Aimed; Guarded is the ally-side aura effect.
    #  - Flanked = Fire-and-Forget + Target Actor: it is set by a DLL cast (CastSpellImmediate onto
    #    the foe, no hit or aura road), and FF + Target Actor is a delivery vanilla Script-archetype
    #    MGEFs use (3 of them).
    data += group('MGEF', cls_marker_mgef(CGID_MARKED_MGEF, "MFOC_Marked", "Marked", CGID_MARKED_KYWD,
                                          CAST_FF, DELIV_CONTACT)
                  + cls_marker_mgef(CGID_FLANKED_MGEF, "MFOC_Flanked", "Flanked", CGID_FLANKED_KYWD,
                                    CAST_FF, DELIV_TARGET_ACTOR)
                  + cls_marker_mgef(CGID_GUARDED_MGEF, "MFOC_Guarded", "Guarded", CGID_GUARDED_KYWD,
                                    CAST_CONCENTRATION, DELIV_AIMED)
                  + ability_mgefs)
    data += group('SPEL', ability_spels + make_marker_spells())   # Skyrim.esm order: MGEF SPEL ... QUST ... CSTY
    data += group('QUST', make_cls_mcm_quest())                   # card C11: the add-on's own MCM quest
    data += group('CSTY', styles)
    flst = prog_flst(CGID_MANIFEST, "MFOC_AddonManifest",
                     [CGID_MANIFEST_KYWD, CGID_FACTION_LIST, CGID_STYLE_LIST]
                     + [fid for fid, _e, _v, _f in CLS_GLOBS])
    flst += prog_flst(CGID_FACTION_LIST, "MFOC_PassiveFactions", faction_fids)
    # Per-class style lookup (see the STYLE SLOTS comment): entry[0] the mask GLOB, then the CSTYs.
    # Entries 35..69 (append-only): the zero-avoid TWIN lists, same class order and the same shape as the lists above; an
    # entry equals its base style when that style has no Avoid Threat to remove. A DLL that reads only 35 entries is unharmed.
    flst += prog_flst(CGID_STYLE_LIST, "MFOC_ClassStyles",
                      [CGID_STYLE_FLST_BASE + row['idx'] for row in table]
                      + [CGID_STYLE_TWIN_FLST_BASE + row['idx'] for row in table])
    for row in table:
        mask, fids, twins = per_class[row['idx']]
        flst += prog_flst(CGID_STYLE_FLST_BASE + row['idx'], "MFOC_ClassStyles_" + cls_edid_name(row),
                          [CGID_STYLE_MASK_BASE + row['idx']] + fids)
        flst += prog_flst(CGID_STYLE_TWIN_FLST_BASE + row['idx'], "MFOC_ClassStylesNoDodge_" + cls_edid_name(row),
                          [CGID_STYLE_MASK_BASE + row['idx']] + twins)
    data += group('FLST', flst)
    perks, perk_counts = make_class_passive_perks(table)
    data += group('PERK', make_reference_perk() + perks + make_offclass_perk())
    print(f"  class passives: {len(perk_counts)} perk(s), {sum(perk_counts.values())} entr(ies); "
          + ", ".join(f"{k} {v}" for k, v in perk_counts.items()))
    return data


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    sndr = make_lockpick_sndr()
    next_id = NEXT_OBJECT_ID_WITH_SNDR if sndr else NEXT_OBJECT_ID
    data = make_tes4(next_id)
    data += make_kywd()
    # GLOB between KYWD and MGEF -- vanilla top-group order (KYWD .. GLOB ..
    # MGEF). ONE group: the PoC probe switchboard (only when the probes are
    # built) and the LOTD-detected flag (always), in FormID order.
    globs = make_glob() if POC_ENABLED else b''
    globs += make_lotd_glob()
    data += group('GLOB', globs)
    data += make_mgef()
    data += make_spel()
    data += make_qust()
    data += make_pack()
    # CSTY after PACK -- vanilla's own top-group order (Skyrim.esm: ... QUST,
    # IDLE, PACK, CSTY, LSCR ...), verified by walking the shipped master.
    data += make_csty()
    # SNDR after CSTY -- xEdit's TES5 group order (CSTY ... MOVT, SNDR, DUAL, ...).
    data += sndr

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

    # The class add-on (card C7) — a third SEPARATE optional plugin. Its only quest is the MCM one (card C11), so it ships
    # its own SEQ + MCM config below.
    cls_path = os.path.join(out_dir, "MFO_Classes.esl")
    cls_data = make_classes_esl()
    with open(cls_path, 'wb') as f:
        f.write(cls_data)

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

    # The class add-on's own SEQ + MCM config (card C11), same reasoning as MFO_Progression.seq.
    cls_seq_path = os.path.join(seq_dir, "MFO_Classes.seq")
    with open(cls_seq_path, 'wb') as f:
        f.write(struct.pack('<I', CGID_MCM_QUEST))
    cls_mcm_dir = write_cls_mcm_files(out_dir)

    print(f"MFO {VERSION}")
    print(f"Written: {out_path} ({len(data):,} bytes)")
    print(f"Written: {seq_path} (5 start-game-enabled quests: MCM, Command, Loot, Retreat, Trade)")
    print(f"Written: {prog_path} ({len(prog_data):,} bytes) — optional progression addon")
    print(f"Written: {prog_seq_path} (1 start-game-enabled quest: MFOP MCM)")
    print(f"Written: {cls_path} ({len(cls_data):,} bytes) — optional class add-on (35 passive FACTs, per-stance class CSTYs, 3 marker MGEFs, 1 reference PERK)")
    print(f"Written: {prog_mcm_dir}/config.json + settings.ini "
          f"({len(PROG_MCM_SLIDERS)} ModSetting economy sliders)")
    print()
    print("Records:")
    print(f"  TES4  header     master: Skyrim.esm, ESL flagged, NEXT_OBJECT_ID 0x{next_id:03X}")
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
    print(f"  PACK  0x{FID_ERRAND_ACTIVATE_0 & 0xFFF:03X}-0x{(FID_ERRAND_SLEEP_0 + ERRAND_WORK_SLOTS - 1) & 0xFFF:03X}    "
          f"MFO_ErrandActivate/Sandbox/Eat/Sleep0-3 -> vanilla {FREF_TMPL_ACTIVATE:08X}/{FREF_TMPL_SANDBOX:08X}/{FREF_TMPL_EAT:08X}/"
          f"{FREF_TMPL_SLEEP:08X} (runtime Location / Target, ch.9 0x49 route, town errands R7)")
    print(f"  CSTY  0x{FID_CAST_STYLE & 0xFFF:03X}        MFO_CastStyle (P1 probe: caster-forward, bProbeCastStyle-gated)")
    print(f"  CSTY  0x{FID_MELEE_STYLE & 0xFFF:03X}        MFO_MeleeStyle (equip_melee stance -- default ON)")
    print(f"  CSTY  0x{FID_RANGED_STYLE & 0xFFF:03X}        MFO_RangedStyle (equip_ranged stance -- default ON)")
    print(f"  CSTY  0x{FID_MELEE_SINGLE_STYLE & 0xFFF:03X}        MFO_MeleeSingleStyle (melee stance without kAllowDualWielding)")
    if sndr:
        for fid, spec in sorted(LOCKPICK_SNDR_PENDING.items()):
            print(f"  SNDR  0x{fid & 0xFFF:03X}        {spec['edid']} (copy of {spec['source']}, "
                  f"ONAM -> SOMMono01400 {FREF_SOPM_MONO01400:08X}, GNAM -> AudioCategorySFX)")
    else:
        print("  SNDR  0x904/0x905  NOT EMITTED (LOCKPICK_SNDR_PENDING incomplete)")
    print(f"  GLOB  0x{FID_LOTD_DETECTED_GLOB & 0xFFF:03X}        MFO_LOTDDetected (the DLL sets 1 when LOTD is detected; MCM hiddenToggle)")
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
