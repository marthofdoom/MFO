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
# 0x80B-0x80F  reserved: more command aliases / globals
# 0x810+       reserved: player-side perks, if that is ever ruled in
FID_CAST_PACKAGE   = OWN | 0x820   # M9: PACK instance riding vanilla UseMagic
# 0x821+       reserved: one PACK per action verb (attack, travel, hold, activate)
NEXT_OBJECT_ID     = 0x900         # first never-used local id

# Vanilla refs
FREF_EQUP_VOICE = 0x00025BEE       # EQUP "Voice" — required ETYP on a lesser power

# ── M9: vanilla PACKAGE TEMPLATES (PACK type 19) ───────────────────────────
# Skyrim ships a template for every action MFO needs, so MFO authors NONE.
# Verified by dumping Skyrim.esm (ENGINE_NOTES §0.17): 104 templates exist.
FREF_TMPL_USEMAGIC     = 0x000504F5   # Spell, Target, CastTime, Cooldown, NumToCast
FREF_TMPL_USEWEAPON    = 0x0001C338
FREF_TMPL_HOLDPOSITION = 0x000503D0
FREF_TMPL_TRAVEL       = 0x00016FAA
FREF_TMPL_ACTIVATE     = 0x00019B2D

# The first proof rides the SEEDED heal. Runtime-varying the spell is the next
# step (§0.17): either mutate the live TESPackage's inputs, or fabricate the
# record on the fly, which is where this is heading.
# FastHealing. Fire-and-forget, and it is what vanilla's own self-heal package
# uses -- but NOT because concentration is forbidden. That was a wrong call:
# 12 of the 46 vanilla UseMagic packages cast CONCENTRATION spells, including
# two with Flames. Concentration was never the crash.
FREF_HEAL_SELF = 0x0002F3B8

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
        # package. If they cast, the architecture is proven end to end and
        # everything after it is plumbing.
        body += subrec('ALFR', struct.pack('<I', POC_ACTOR_REF))
    body += subrec('ALPC', struct.pack('<I', FID_CAST_PACKAGE))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')

    # ── alias 1: the foe, for targeted verbs. No packages. ──
    body += subrec('ALST', struct.pack('<I', 1))
    body += subrec('ALID', zstr("MFO_CommandTarget"))
    body += subrec('FNAM', struct.pack('<I', 0x0002 | 0x0008 | 0x0200))
    body += subrec('VTCK', struct.pack('<I', 0))
    body += subrec('ALED', b'')
    return record('QUST', FID_COMMAND_QUEST, 0, body)


# ── M9: the cast package ────────────────────────────────────────────────────
def pack_input(kind, payload_type, payload):
    """One templated-package data input: ANAM names the type, then its value."""
    return subrec('ANAM', zstr(kind)) + subrec(payload_type, payload)


def make_cast_package():
    """A PACK INSTANCE riding vanilla `UseMagic` (000504F5).

    Byte layout copied from a shipped vanilla instance, `MG07AncanoCastAtEye`
    (0010F819), rather than invented -- that record is 524 bytes and proven in
    the base game. MFO substitutes two slots:

        PTDA type=1  -> the SPELL   (vanilla put MG08AncanoEyeSpell here)
        PTDA type=4  -> the TARGET, a REFERENCE ALIAS instead of vanilla's
                        type=0 specific reference, because MFO cannot name the
                        actor at author time.

    The remaining inputs are the vanilla values, deliberately: they are a
    known-good configuration and every one MFO changes is a variable it then
    has to defend. CastTime/Cooldown/NumToCast are exactly the bounds §4.5c
    requires an action to carry.
    """
    body  = subrec('EDID', zstr("MFO_CastPackage"))
    # PKDT: flags=0, type=18 (package), then vanilla's trailing bytes.
    # PKDT: kIgnoreCombat (0x00100000), NOT kMustComplete.
    #
    # This was 0x04 (kMustComplete) on the reasoning that §4.5c wants an action
    # that runs start to finish. The record survey says otherwise, decisively:
    # of the 46 vanilla UseMagic instances, kMustComplete appears on exactly ONE
    # -- a stationary non-combat channeling thrall -- while ALL SIX records
    # whose intent is to cast DURING A FIGHT set kIgnoreCombat. A gambit fires
    # in combat by definition, so MFO is in the six, not the one.
    #
    # Byte tail copied from TG08BMercerCombatOverrideCastAtBrynjolf, which is
    # MFO's exact shape (see below), not from MG07AncanoCastAtEye.
    body += subrec('PKDT', struct.pack('<IBB', 0x00100000, 18, 0x04) + bytes.fromhex('038800000000'))
    # PSDT: any time, any day -- the DLL decides when, not the schedule.
    body += subrec('PSDT', bytes.fromhex('ffff00ffff00000000000000'))
    body += subrec('PKCU', struct.pack('<III', 11, FREF_TMPL_USEMAGIC, 1))
    # QNAM -- TESPackage::ownerQuest. NON-NEGOTIABLE when any input uses
    # PTDA targType=4: the alias VALUE is an index, and this is the quest it
    # indexes into. A survey of Skyrim.esm found 627 packages targeting a
    # reference alias (PTDA targType=4) and all of them carry QNAM; widening to
    # "targets an alias EITHER by PTDA type 4 or PLDT type 8" gives 626/626.
    # Both queries agree: an alias reference of any kind demands QNAM. The
    # record this was copied from
    # (MG07AncanoCastAtEye) omits it precisely BECAUSE its target is a specific
    # reference, not an alias -- the target type changed here and the
    # consequence did not follow.
    # QNAM ONLY when an input names an alias. With a targType-0 specific
    # reference nothing indexes into a quest, and all nine vanilla
    # alias-delivered UseMagic packages carry no QNAM at all.
    if not POC_ENABLED:
        body += subrec('QNAM', struct.pack('<I', FID_COMMAND_QUEST))

    # 11 inputs, in the template's declared order.
    # PLDT type=12 ("near self"), target 0, generous radius -- the UseMagic
    # TEMPLATE's own default is type=12/0/500, and TG08B's cast package uses
    # type=12/0/10000. Type 0 means "near reference" and REQUIRES a reference:
    # zero of the 4,048 type-0 PLDTs in Skyrim.esm have a null target, which is
    # exactly what MFO was emitting.
    body += pack_input("Location", 'PLDT', struct.pack('<IiI', 12, 0, 10000))
    # Spell.
    body += pack_input("TargetSelector", 'PTDA', struct.pack('<IIi', 1, FREF_HEAL_SELF, 0))
    # Target -- MATCH VANILLA'S ALIAS-DELIVERED SHAPE EXACTLY.
    #
    # All NINE vanilla UseMagic packages that are delivered by an alias (ALPC)
    # use targType 0, a SPECIFIC REFERENCE, and carry NO QNAM. Vanilla's
    # self-casting packages (targType 6) are never alias-delivered. MFO's crash
    # came from a combination with ZERO vanilla precedent: alias-delivered PLUS
    # self target PLUS QNAM.
    #
    # For the PoC, which already hardcodes the actor, targType 0 is available
    # and is the most conservative possible record: byte-shaped like
    # MG07AncanoCastAtEye, a package that ships and works.
    #
    # Generalising to an arbitrary follower is the NEXT problem, and it is why
    # the target-selection question is not settled by this PoC.
    # (previous, crashed: targType 6 self)
    #
    # This was `targType 4 -> alias 0`, reasoning that since the alias holds the
    # follower, alias 0 IS himself. FIELD-REFUTED: the package took ownership of
    # Cosnach -- he rooted in place, unresponsive -- and never cast, with no
    # animation, no effect and NO MAGICKA SPENT, i.e. the procedure never even
    # attempted it.
    #
    # Vanilla never self-targets that way. Of the 46 UseMagic instances, 8 point
    # at a reference alias and every one of them names a DIFFERENT actor; the
    # seven that cast on themselves all use targType 6 with value 0 --
    # WCollegeColettePracticeHeal13x2 is literally "practice healing on self",
    # alongside DA16ErandurCastSpell and SprigganCallOverride.
    if POC_ENABLED:
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 0, POC_ACTOR_REF, 0))
    else:
        body += pack_input("SingleRef", 'PTDA', struct.pack('<IIi', 6, 0, 0))
    body += pack_input("Bool",  'CNAM', struct.pack('<B', 0))          # HoldWhenBlocked
    body += pack_input("Float", 'CNAM', struct.pack('<f', 1.0))        # CastTimeMin
    body += pack_input("Float", 'CNAM', struct.pack('<f', 3.0))        # CastTimeMax
    body += pack_input("Float", 'CNAM', struct.pack('<f', 1.0))        # CooldownTimeMin
    body += pack_input("Float", 'CNAM', struct.pack('<f', 3.0))        # CooldownTimeMax
    body += pack_input("Int",   'CNAM', struct.pack('<i', 1))          # NumToCastMin
    body += pack_input("Int",   'CNAM', struct.pack('<i', 1))          # NumToCastMax
    body += pack_input("Bool",  'CNAM', struct.pack('<B', 0))          # DualCast

    # Inherited-input markers, verbatim from the vanilla instance.
    for i in range(0x02, 0x0d):
        body += subrec('UNAM', struct.pack('<B', i))
    body += subrec('XNAM', struct.pack('<B', 0x0d))

    # Empty on-begin/end/change blocks, as vanilla ships them.
    for blk in ('POBA', 'POEA', 'POCA'):
        body += subrec(blk, b'')
        body += subrec('INAM', struct.pack('<I', 0))
        body += subrec('PDTO', struct.pack('<II', 0, 0))
    return record('PACK', FID_CAST_PACKAGE, 0, body)


def make_pack():
    return group('PACK', make_cast_package())


def make_qust():
    return group('QUST', make_startup_quest() + make_mcm_quest() + make_command_quest())


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(out_dir, exist_ok=True)

    data = make_tes4(NEXT_OBJECT_ID)
    data += make_kywd()
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
    print(f"  QUST  0x{FID_COMMAND_QUEST & 0xFFF:03X}        MFO_CommandQuest (1 alias, DLL-filled)")
    print(f"  PACK  0x{FID_CAST_PACKAGE & 0xFFF:03X}        MFO_CastPackage -> vanilla UseMagic {FREF_TMPL_USEMAGIC:08X}")


if __name__ == "__main__":
    main()
