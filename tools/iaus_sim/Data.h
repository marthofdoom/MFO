#pragma once
// iaus_sim/Data.h -- the list's balance as the simulator reads it (sim v2). HOST-ONLY, never part of the DLL.
//
// One file per list: ~/.cache/mfo-list-balance/<list>.sim.json (tools/list_balance/compile_world.py). It is TEST
// INPUT ONLY. With no list the BUILT-IN set is used: Skyrim.esm's own game-setting values plus a small synthetic
// roster (fixtures written by hand for the simulator; never game data and never read by native/).
//
// Every formula below names its source and proof level in Docs/research/2026-11/iaus-sim-v2.md ("Model table").
#include "iaus/Core.h"
#include "iaus/Snapshot.h"
#include "iaus_sim/Json.h"
#include "cast/SeatRows.h"   // SIM V4: native's own pure seat table and zero-effect rules (standard types only; the harness reads the same file)

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <optional>
#include <vector>

namespace Sim {

    using namespace MFO::Iaus;

    // ── T2 BATCH S (iaus-sim-v2.md section 31): every T2 mechanism the manifest's Sim column listed as OPEN. Each is ONE switch,
    // ON by default. With every switch off (`--t2 off`, which includes `--feet-stop`) the lab is the one of section 30, draw for draw
    // (78.11 at g3 / 76.69 at --stealth off for the Beta 1 set). A switch never draws from the fight's random stream while it is off.
    struct T2 {
        bool feet = true;        // Q-SIM-5: a weapon swap, a potion and a cast do not stop his feet (false = --feet-stop, the lab before)
        bool c8 = true;          // card C8: ch.18 share window (ceiling / floor), ch.23 leash, the engine's own power attacks for a class with no rule
        bool dodge = true;       // Q-melee-7: the class style's Avoid Threat rolls a side step against an incoming projectile
        bool aversion = true;    // card H2: ch.32 aversion (Conjurer, Healer 0.25, equipped class)
        bool aim = true;         // card H5: ch.29 miss budget (a corrected shot when the window's misses reach the budget)
        bool block = true;       // ch.31 held guard: ClassGate::block from the class's Block row, 2.5 s held, the held-guard gate
        bool range = true;       // card H4: the distance a mage fights at follows the spell in his hand; the mage back-off
        bool hands = true;       // Q-I-2 (marth 11-12): the range follows the shortest equipped spell of one delivery type over BOTH hands; touch + distance = the distance range, a touch cast closes and backs out (needs range)
        bool backoff = true;     // Q-T2-3: the Mercenary's back-off (two or more foes press him)
        bool step = true;        // gap G18: the hidden step
        bool c13built = true;    // card C13 AS BUILT (near-inert, Q112): no engine lever, so no free entry. false (--c13 lever) = the H10 guarantee the lab carried before
        bool unweighted = true;  // Q91: a spell with no effect IAUS can value is dropped (Prune::Drop::Unweighted)
        bool reanimate = true;   // card G7: reanimate spells are kept (table rows cast them at a corpse), corpses are raised
        bool dnd = false;        // --disengage-dnd (marth 11-12, NOT built in the game): a disengage makes him no valid target for 6 s
        void AllOff() { feet = c8 = dodge = aversion = aim = block = range = hands = backoff = step = c13built = unweighted = reanimate = false; }
    };
    inline T2 g_t2;
    // ── SIM V3 (Docs/research/2026-11/iaus-sim-v3.md): THE POSITIONAL WORLD. marth 11-13: "We've proved the sim doesn't work without
    // distance and movement". Each mechanism is ONE switch, ON by default. `--world v2` turns every one off and the lab is v2, draw
    // for draw (run.sh pins it). A switch never draws from the fight's random stream R while it is off; the draws v3 adds come from a
    // stream of their own (Fight::V), so one v3 switch never shifts the draws of another mechanism.
    struct V3 {
        bool height = true;      // z and 3D distance: a dragon flies a circuit at altitude, a perched foe stands on a ledge; the game's own OutOfMeleeReach test
        bool chase = true;       // Confidence::ChaseRadius (fChaseMin 600 .. fChaseMax 3000 by confidence), 3D, with the held-foe waiver (iaus/Candidates.cpp)
        bool lof = true;         // the line-of-fire hold: a hostile cast is held while the player or a follower stands in the corridor
        bool delivery = true;    // cast delivery: claim-to-fire latency, the inactive controller, claim thrash
        bool hands = true;       // the hands: a cast takes a hand, a stream holds it, a two-handed weapon or a bow is put away and drawn again
        bool stance = true;      // the engine's movement follows the stance (dry caster = melee), attack slots around a target, foe fallback
        bool sidestep = true;    // a caster whose cast is held by the line of fire side steps (ASSUMPTION, load-bearing: its own switch; --side-step random|away)
        bool casterhold = true;  // native 0ed9c705 / 6c5ba58d mirrored: ClassGate::casterLine + Snapshot::spellsHoldPrev (false = the game before 11-13)
        bool blockdir = true;    // a guard stops a blow only inside its arc; every blow on the subject is tallied by direction
        bool areas = true;       // an ACTIVE area (cloak = moves with the caster, circle / rune / wall = fixed at the cast point) lives in the world while it lasts
        bool perks = true;       // Snapshot::perks spell group (spellsRead, spellCost / spellMagnitude / spellCount per school), perk-effects.md section 9
        bool hittail = true;     // a foe's physical hit on the party is drawn from the FIELD's hit-size spread (World.h kFieldHit; iaus-sim-v3.md 15.5), mean 1: the same damage a minute, in fewer and harder hits
        bool burst = true;       // native 2de581de..ad4578a8 mirrored (heal timing, 11-17g): Combatant::grossPerSec / maxHit filled from sampled laps through the game's own
                                 // Estimate::LapGross / BurstReads, so Facts::recipientHitsToDown and t.dyingHitsToDown / urgencyHitsDiv / the three windows are live (iaus-sim-v4.md 9.1)
        void AllOff() { height = chase = lof = delivery = hands = stance = sidestep = casterhold = blockdir = areas = perks = hittail = burst = false; }
        [[nodiscard]] bool Any() const { return height || chase || lof || delivery || hands || stance || sidestep || casterhold || blockdir || areas || perks || hittail || burst; }   // review F-B of 610e2952: hittail was left out, so `--world v2 --v3 hittail=on` was tagged v2
    };
    inline V3 g_v3;
    inline int   g_waiver = 2;            // --waiver none|self|player: the chase cap's held-foe waiver. 2 = the game today (marth 11-13: the foe fights HIM or the player), 1 = the game of 10-19 (HIM only), 0 = none
    inline bool  g_deliveryOld = false;   // --delivery old: the game before fix/heal-claim-gates and fix/iaus-claim-thrash (a claim on an inactive controller never casts; a newer pick takes the hand)
    // CAST DELIVERY, MEASURED (sim mirror batch 11-17; tools/iaus_sim/calib_cast_delay.py over _evidence/deck-run-1117/part4 and
    // _evidence/deck-run-1117b-full: MFO.log [iaus] pick=cast_* joined to APMF.log [ctcensus] OPEN / FIRED / CLOSE claim windows, repeats from
    // MFO.log [cast] CFC-fired; 122 windows of ONE follower, Jesper, the only IAUS caster of both sessions; substrate windows and windows that
    // overlap a main-thread stall left out). It replaces the two single-session figures (2.4 s offense / 4.5 s heal) of sim v3's first cut.
    //   first    claim -> the first fire of a claim that fired, deciles (0, 10, .. 100 %), seconds. kPickToOpen is added (pick -> the claim at Harbinger)
    //   repeat   fire -> the next fire while the claim stands (the engine casts again; in the lab: his next pick of that spell at that target
    //            within kRepeatWindowSec of its release). No table (a stream, a buff): the first table
    //   unfired  the share of claims that closed with NO fire, over fired + unfired, where unfired = replaced by a newer claim, a caster built and answered
    //            yes but never released, or no caster built. LEFT OUT of both sides (review F1 of 5749026a): refused (CheckStartCast said NO each time: sight,
    //            line of fire, range, which the lab holds itself), target-died (the target died inside the window or within 1.5 s of its close: the lab's
    //            own castLost), and shape-gap (no caster built for a spell shape Harbinger never builds one for: the three cloaks "no-row", Frostbite and
    //            Creeping Cold "av26 row-without-caster", Turn Lesser Undead "TargetEffect no-row": a framework gap that is the same every cast, listed
    //            for Harbinger, not a per-cast chance). --deliver-unfired <k> scales it (0 = all fire)
    //   stood    how long such a claim stood before it closed (the body is the claim's for that long)
    // Rows: [nature 0 offense / control, 1 heal, 2 a cloak][0 fire-and-forget, 1 concentration]; every other buff / ward / summon: kUnsampled. n = samples behind each table.
    struct DeliveryRow { float first[11]; float repeat[11]; bool hasRepeat; float unfired; float stood[11]; int nFirst, nRepeat, nWindows; };
    namespace Delivery {
        inline constexpr float kPickToOpen = 0.11f;        // MEASURED median, n=57 (p25 0.06, p75 0.14)
        inline constexpr float kRepeatWindowSec = 2.0f;    // ASSUMPTION: the claim still stands when he picks the same cast again this soon after its release (cast recover 0.45 s + one decision lap of a party of up to four)
        inline constexpr float kStoodPooled[11] = { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f };   // n=16
        inline constexpr DeliveryRow kRow[3][2] = {
            {   // offense
                { { 1.00f, 1.14f, 1.29f, 1.30f, 1.52f, 1.72f, 2.06f, 2.61f, 3.82f, 4.96f, 24.09f }, { 0.66f, 0.96f, 1.00f, 1.07f, 1.13f, 1.22f, 1.32f, 1.78f, 2.12f, 2.86f, 3.55f }, true, 0.344f,   // 11 / 32
                  { 0.21f, 0.64f, 0.65f, 2.77f, 2.82f, 3.12f, 3.25f, 3.43f, 3.45f, 6.14f, 8.11f }, 21, 39, 32 },
                { { 0.29f, 0.30f, 0.44f, 0.56f, 0.59f, 0.61f, 1.05f, 1.34f, 1.69f, 2.30f, 3.32f }, {}, false, 0.182f,   // 2 / 11 (stood: pooled)
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 9, 0, 11 } },
            {   // heal (stood: the pooled table, one unfired window each)
                { { 0.96f, 1.48f, 1.77f, 1.89f, 2.11f, 2.23f, 2.30f, 2.37f, 3.00f, 3.96f, 5.26f }, { 0.75f, 0.77f, 0.81f, 1.01f, 1.03f, 1.04f, 1.05f, 1.05f, 1.07f, 1.13f, 2.98f }, true, 0.111f,   // 1 / 9
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 8, 36, 9 },
                { { 0.39f, 0.59f, 0.63f, 0.75f, 0.95f, 1.04f, 1.08f, 1.16f, 1.62f, 3.31f, 3.64f }, {}, false, 0.091f,   // 1 / 11
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 10, 9, 11 } },
            {   // A CLOAK (the only buff shape sampled: Flame / Frost Cloak and one Candlelight claim; both columns the same row). 1 / 8 once the shape gaps are out
                { { 1.28f, 1.29f, 1.36f, 1.51f, 1.60f, 1.66f, 2.30f, 2.88f, 3.31f, 3.81f, 4.36f }, {}, false, 0.125f,
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 7, 0, 8 },
                { { 1.28f, 1.29f, 1.36f, 1.51f, 1.60f, 1.66f, 2.30f, 2.88f, 3.31f, 3.81f, 4.36f }, {}, false, 0.125f,
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 7, 0, 8 } },
        };
        // NEVER SAMPLED (a summon, a ward, an armor spell, any buff that is not a cloak): no claim of that shape is in the logs, so the cloak row is NOT
        // applied to it (review F1c). ASSUMPTION: it fires, after the pooled first-fire time of every fire-and-forget claim that fired (n=36).
        inline constexpr DeliveryRow kUnsampled = { { 0.96f, 1.15f, 1.29f, 1.41f, 1.71f, 1.90f, 2.27f, 2.63f, 3.45f, 4.80f, 24.09f }, {}, false, 0.0f,
                  { 0.21f, 0.65f, 2.77f, 2.81f, 3.07f, 3.18f, 3.26f, 3.44f, 3.64f, 6.35f, 8.11f }, 36, 0, 0 };
        // u in [0, 1) -> the value at that quantile (linear between deciles)
        [[nodiscard]] inline float At(const float (&d)[11], float u) { const float x = std::clamp(u, 0.0f, 0.99999f) * 10.0f; const int i = static_cast<int>(x); return d[i] + (d[i + 1] - d[i]) * (x - static_cast<float>(i)); }
    }
    inline float g_deliverSec[2] = { -1.0f, -1.0f };   // --deliver-sec o,h: a FIXED claim-to-fire for an offense cast / a heal, no unfired claims (sim v3's first cut ran 2.4,4.5). < 0 (the default) = the measured tables above
    // --mirror off: the lab WITHOUT the game changes of 11-16 / 11-17 that this batch mirrored on the sim's side (the chase-waived mark, the foes kept past the
    // cap as noApproach, the entry foe, the left-hand share of hand_lost, the cast deferrals, the heal's full-health release, the 128 approach up a
    // ledge path). For before / after runs of the field scenarios only; what the game's own generator does (compiled in) is not switched.
    inline bool  g_mirror = true;
    // SIM V4 batch C: WHICH SPELL-WORTH RULES THE FOLLOWER'S READER PLAYS (`--spell-rules today|old|all`). today (0, the default) = main after fix/spell-worth
    // (2026-11-24): a rider spell is measured by its direct damage, an unmeasured one reads a share of his best, a level cap is read per foe. old (1) = the
    // game of the 11-23 field run, for the regret report's "before" column: ANY indirect effect made the spell unmeasured, an unmeasured spell's damage terms
    // did not apply, the book's level cap was not read (Policy.h SpellMeasured / SimGenerate). all (2) = the lab before this batch: every spell measured.
    inline int   g_spellRules = 0;
    // --engage-on-sight: Harbinger's ch.21 combat ENTRY is modelled (the second id of Snapshot.h ChaseWaived). OFF by default, as the game ships it
    // ([CombatEntry] EngageOnSight, Config.cpp:328); a scenario that models engage-on-sight sets FightSpec::engageOnSight.
    inline bool  g_engageOnSight = false;
    inline float g_deliverUnfired = 1.0f;              // --deliver-unfired k: scales every row's unfired share (0 = every claim fires)
    inline int   g_sideStep = 1;          // --side-step random|away: which way a held caster steps across the line. 1 random (the default: no source says he knows where the friend is), 2 away from the friend in his way
    inline bool  g_casterPressure = false;   // --caster-pressure: the caster cells' objective also plays the "dive" scenarios (foes that start on the caster); OFF by default so the headline keeps its composition
    inline float g_blockArc = 180.0f;     // --block-arc: the full angle in front of him a guard covers (ASSUMPTION: no engine source in Docs)
    inline float g_aimMiss = 0.12f;      // --aim-miss: the lab's chance that an UNCORRECTED aimed shot misses a moving target (MODEL; TM::aimMiss before)
    inline float g_dodgeFlight = 0.2f;    // MODEL: a side step clears a projectile only when it flies at least this long (a reaction time; --dodge-flight, sensitivity in section 31.4)
    inline float g_aimScatter = 0.0f;     // --aim-scatter P: every UNCORRECTED follower shot also misses with this chance, moving target or not. The engine scatters by
                                          // aimController+0xA0 x fCombatAimProjectileRandomOffset (class-m0-re.md); how often that misses is not measured, so the default is none
    inline bool  g_mixPartial = false;    // --mix-partial: the ch.18 share is read on a window that is not full yet. Harbinger's Share() answers "no share" (no deny) until 8
                                          // attacks fill the window (review of 0880667, F3), so the default waits for 8. The switch measures the other reading
    inline bool  g_noG3 = false;          // the disengage report's 'none' mode: card G3's hide run and flee are never asked for (every other road stands)
    // BATCH 3 ("sim decides the edge", iaus-sim-v2.md 35). Each switch OFF reproduces the earlier lab byte-identically.
    inline bool  g_rangedPerks = true;    // a shot is multiplied by the ModAttackDamage perks of the BOW's skill (Archery), not the melee weapon's: a BUG FIX, the default
                                          // (marth 11-12); --ranged-perks off is the lab before it, kept only for the byte-identical baseline proof
    inline bool  g_pointBlank = false;    // --point-blank: under fCombatRangedAttackHoldTimeMinDistance the bow hold is fCombatRangedAttackHoldTimeAbsoluteMin
    inline float g_drawSpoil = 0.0f;      // --draw-spoil P: a melee hit that lands on a drawing archer spoils the draw with chance P (MODEL, no engine source)
    inline bool  g_foeStyles = false;     // --foe-styles: a melee foe backpedals after a blow with the chance its CSTY close-range Fallback Mult gives, then re-engages (Part C)
    inline std::string g_namedFollower;   // --named-follower <label>: the SUBJECT is built from that named record (skills, health, race, gear; the class kit fills what it lacks)
    inline float g_spaceSize = 0.0f;      // --space R: an ENCLOSED fight (the encounter's cell is an interior) is bounded by a disc of radius >= R around its start (Part C); 0 = open everywhere
    inline int   g_spaceForce = -1;       // --space-force enclosed|open: every fight that type (the sensitivity axis); -1 = by the encounter's cell
    inline bool  g_buffDurReal = false;   // --buff-dur real: a cast buff is up for its record duration (the lab used max(30 s, duration))
    inline bool  g_fillSwing = true;      // --no-fill-swing: Combatant::swinging is not filled ([Debug] bIausFillSwing ships ON since 11-17b, as Armor / Hit / Loss / Spells: the lab's default is the shipped one; without it no Block candidate exists)

    // Actor values (CommonLib RE::ActorValue, the same numbers the records store).
    inline constexpr int kAvOneHanded = 6, kAvTwoHanded = 7, kAvMarksman = 8, kAvBlock = 9, kAvHeavy = 11, kAvLight = 12,
                         kAvAlteration = 18, kAvConjuration = 19, kAvDestruction = 20, kAvIllusion = 21, kAvRestoration = 22,
                         kAvHealth = 24, kAvMagicka = 25, kAvStamina = 26, kAvDamageResist = 39, kAvPoisonResist = 40,
                         kAvResistFire = 41, kAvResistShock = 42, kAvResistFrost = 43, kAvResistMagic = 44, kAvWardPower = 63;
    // MGEF archetypes (RE::EffectArchetypes).
    inline constexpr int kArValueMod = 0, kArScript = 1, kArAbsorb = 4, kArDualValueMod = 5, kArCalm = 6, kArDemoralize = 7,
                         kArFrenzy = 8, kArSummon = 18, kArParalysis = 21, kArReanimate = 22, kArTurnUndead = 24,
                         kArStagger = 33, kArPeakValueMod = 34, kArCloak = 35, kArLight = 12, kArBoundWeapon = 17;
    // The effect archetypes whose damage is not in the spell's own effect list (native iaus/Fills.cpp MeasureSpell: kScript 1, kCloak 35, kEnhanceWeapon 39,
    // kSpawnHazard 40; the numbers are RE::EffectArchetypes::ArchetypeID's, the fork's EffectArchetypes.h). To add one: this row, after native's switch.
    inline constexpr int kArEnhanceWeapon = 39, kArSpawnHazard = 40;
    inline constexpr int kIndirectArch[] = { kArScript, kArCloak, kArEnhanceWeapon, kArSpawnHazard };

    struct Rng {
        std::mt19937_64 g;
        float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); }
        int   I(int a, int b) { return b <= a ? a : std::uniform_int_distribution<int>(a, b)(g); }
        bool  P(float p) { return U(0.0f, 1.0f) < p; }
    };

    struct Weapon {
        std::string name;
        float dmg = 0, speed = 1, reach = 1, stagger = 0, crit = 0, weight = 8;
        int   anim = 0;     // 0 fist, 1 sword, 2 dagger, 3 axe, 4 mace, 5 greatsword, 6 battleaxe / hammer, 7 bow, 8 staff, 9 crossbow
        int   skill = kAvOneHanded;
        bool  silver = false;
        [[nodiscard]] bool Has() const { return dmg > 0; }
        [[nodiscard]] bool TwoH() const { return anim == 5 || anim == 6; }
        [[nodiscard]] bool Ranged() const { return anim == 7 || anim == 9; }
    };

    // SUMMONS AND HAZARDS, sim bundle A (iaus-sim-v4.md 16). Every number is a MODEL or a placeholder.
    // To change how a summon spawns or its cap: kSummon. A new summon kind: nothing, it is the list's NPC record.
    struct SummonModel {
        float limitBase = 1;                                            // iMaxSummonedCreatures' code default (cast/Summon.cpp SummonLimit), when the list's gmst has none
        float spawnAheadU = 150;                                        // MODEL: the caster aims this far before him, towards his target
        float deadListedSec = 3;                                        // MODEL: a dead creature stays in the engine's commanded list this long (it counts against the limit)
        float genericHpBase = 60, genericHpPerLevel = 12, genericUnarmedBase = 8, genericUnarmedPerLevel = 0.8f, genericRun = 400;   // MODEL: the generic creature (a world with no actor for the spell)
    };
    inline constexpr SummonModel kSummon;
    // To change how a spawned hazard is placed or ticked: kHazard. A new hazard kind: nothing, it is the list's record (tools/list_balance/extract_world.py hazard_row).
    struct HazardModel {
        float wallStepSec = 0.5f;                                       // MODEL: a stream lays one hazard this often
        float floorBandU = 128;                                         // MODEL: it reaches this far up and down
        float minInterval = 0.25f;                                      // a record's interval 0 is not "every step"
        float maxLifeSec = 60;                                          // MODEL: a record with lifetime 0 (stands until removed) stands this long
        int   maxLive = 64;                                             // memory bound of one fight
    };
    inline constexpr HazardModel kHazard;
    struct SimSpell;
    struct HazardSpec { float radius = 0, lifetime = 0, interval = 1; int limit = 0; bool viaImpact = false, drops = false, pcOnly = false; std::shared_ptr<const SimSpell> spell; };   // radius in units. drops: it falls to the ground (flag 16). spell: per TICK (Classify)

    // One spell as the world resolves it. `e` is the SpellBook entry IAUS sees (the classification mirrors
    // Prune::ClassifyNature's precedence, as sim v1 did).
    struct SimSpell {
        SpellEntry e;
        Prune::Drop foeDrop = Prune::Drop::Keep;   // a FOE's verdict: e.drop without the rules that are IAUS's alone (Classify, placed-spells L1)
        int   school = -1;          // skill AV of the costliest effect
        int   skillReq = 0;         // that effect's minimum skill (the spell's tier)
        float cost0 = 0;            // record cost before the caster's skill
        float castSec = 0.5f;       // charge time before release (concentration: the stream's length)
        float dmg = 0;              // health damage at impact
        float dot = 0, dotDur = 0;  // health damage per second, for dotDur
        float mpDmg = 0, spDmg = 0; // magicka / stamina damage at impact
        float heal = 0, hot = 0, hotDur = 0;
        float armor = 0, buffDur = 0;      // +damage resist (armor spells)
        float resistBuff = 0; int resistBuffAv = -1;
        bool  ctrl = false, ctrlPar = false, ctrlCalm = false, ctrlFlee = false, ctrlFrenzy = false, turnUndead = false;
        float ctrlDur = 0, ctrlMaxLevel = 1e9f;   // illusion: the magnitude is the highest level affected
        bool  stagger = false;
        // SIM V4 `caststart` (iaus-sim-v4.md 5): the engine row his claim would key, as native cast/Archetype.cpp predicts it: the row of the spell's
        // costliest effect that has one. Data.h SimRow is the lab's copy of that table's rows (run.sh stops the build when native's unseated list moves).
        int   row = 0;
        bool  reanimate = false, summon = false;
        float summonDur = 60;
        std::vector<int> summonIdx;             // per Data::buckets: the actor it summons (Data::summonSpecs). Empty = no list data: the generic creature
        std::optional<HazardSpec> hazard;       // what it leaves lying (the first hazard of its effects)
        float recovery = 0;         // shout recovery (its cooldown)
        // SIM V4 batch C (the fix/spell-worth mirror, native iaus/Fills.cpp MeasureSpell): the spell has an effect whose damage lives ELSEWHERE than in its own
        // effect list: a hazard it spawns, a cloak's second spell, a script, an enchantment it puts on the weapon (kIndirectArch). Read from the record's
        // effect archetypes, never from a name. With no direct health damage beside it, an OFFENSE spell's damage is unmeasured (Policy.h SpellMeasured).
        bool  indirect = false;
        bool  healOther = false;    // a heal that is not self-only
        std::string element;        // "fire" / "frost" / "shock" / ""
        // SIM V3. An ACTIVE area (iaus-sim-v3.md 6): 0 none, 1 it moves with the caster (a cloak: effect archetype Cloak, FACT), 2 it stays at
        // the cast point (a circle: MODEL, read from the editor id). areaDps = health per second to a foe inside, areaArmor = armor to a friend inside.
        int   areaKind = 0; float areaDps = 0, areaArmor = 0, areaDur = 0;
        // The perk-effect mirror (perk-effects.md 9): what his perks multiply this spell's cost and magnitude by (ModSpellCost / ModSpellMagnitude), set when a follower is built.
        float perkCost = 1, perkMag = 1;
    };

    struct Style {   // CSTY (CSGD / CSME / CSCR / CSLR / DATA)
        float off = 1, def = 1, groupOff = 1, wMelee = 1, wMagic = 1, wRanged = 1, wShout = 1, wUnarmed = 1, wStaff = 1, avoidThreat = 0.2f;
        float atkStaggered = 1, powStaggered = 1, powBlocking = 1, bash = 1, bashRecoil = 1, bashAtk = 1, bashPow = 1, special = 1;
        float circle = 0.2f, fallback = 0.2f, flankDist = 0.2f, stalk = 0.2f, strafe = 0.2f;
        bool  dueling = false, flanking = false, dualWield = false;
    };

    struct ActorSpec {
        std::string name, race;
        int   level = 1;
        float hp = 100, mp = 50, sp = 100, hpR = 0.7f, mpR = 3, spR = 5;
        std::array<float, 18> skill{};   // index = AV - 6
        Weapon melee, ranged;
        bool  staff = false, shield = false;
        float ammoDmg = 0, unarmed = 4, unarmedReach = 96, atkDmgMult = 1;
        float ar = 0; int pieces = 0;
        float rFire = 0, rFrost = 0, rShock = 0, rMagic = 0, rPoison = 0;
        std::vector<int> spells;         // into Data::foeSpells (foes) -- followers carry their own SimSpell list
        Style st; bool hasStyle = false;
        int   conf = 2, aggr = 1;
        float run = 370, mass = 1;
        bool  undead = false, dragon = false, daedra = false, animal = false, creature = false, immunePar = false;
        float atkMult = 1, powMult = 1;  // perk multipliers (ModAttackDamage / ModPowerAttackDamage)
        float rangedAtkMult = -1;        // ModAttackDamage at the BOW's skill (a follower build only; -1 = not computed): --ranged-perks
        [[nodiscard]] float Skill(int av) const { return av >= 6 && av < 24 ? skill[static_cast<std::size_t>(av - 6)] : 15.0f; }
        [[nodiscard]] float Resist(int av) const {
            return av == kAvResistFire ? rFire : av == kAvResistFrost ? rFrost : av == kAvResistShock ? rShock : av == kAvResistMagic ? rMagic : av == kAvPoisonResist ? rPoison : 0.0f; }
    };

    struct EncMember { int spec; float dx, dy; };
    struct Encounter { std::string cell; bool interior = false; std::vector<std::vector<EncMember>> byBucket; };

    struct FRace { std::string edid; float hp = 100, mp = 100, sp = 100, hpR = 0.7f, mpR = 3, spR = 5, unarmed = 4, run = 370, mass = 1; std::vector<std::pair<int, int>> boosts; };
    struct FClass { std::array<float, 18> skillW{}; float hW = 1, mW = 1, sW = 1; std::string edid; };
    using Thr = std::vector<std::pair<float, float>>;   // (skill threshold, value), ascending

    // The game settings the world model reads, each with the value Skyrim.esm ships (the built-in fallback).
    struct Rules {
        float dmgSkillMin = 1, dmgSkillMax = 3, dmgWeaponMult = 1;            // fDamageSkillMin/Max (NPC), fDamageWeaponMult
        float armorScale = 0.12f, armorMax = 80, armorBasePiece = 0.03f;      // fArmorScalingFactor, fMaxArmorRating, fArmorBaseFactor
        float armorSkillLo = 1, armorSkillHi = 2.5f;                          // fArmorRatingBase, fArmorRatingMax (NPC)
        float powBonus = 1, powStaBase = 20, powStaMult = 1, powStaPenalty = 2;   // fPowerAttackDefaultBonus, fStaminaAttackWeaponBase/Mult, fPowerAttackStaminaPenalty
        float combatMp = 0.33f, combatSp = 0.35f, combatHp = 0.7f;            // fCombatMagicka/StaminaRegenRateMult; health: AV CombatHealthRegenMult (UNPROVEN)
        float delayHp = 0.5f, delayMp = 0.5f, delaySp = 0.5f;                 // fDamaged*RegenDelay
        float costBase = 0.005f, costMult = 0.5f, costScale = 0.5f;           // fMagicCasterSkillCostBase/Mult, fMagicSkillCostScale (NPC)
        float bowDraw = 1.6667f;                                              // fBowDrawTime
        float diffToPC = 1, diffByPC = 1;                                     // fDiffMultHPToPCN, fDiffMultHPByPCN
        float hpPerLevel = 5, hmsPerLevel = 10, skillPerLevel = 8, skillStart = 15;   // fNPCHealthLevelBonus, iAVDhmsLevelUp, iAVDSkillsLevelUp, iAVDSkillStart
        float maxResist = 85;                                                 // fPlayerMaxResistance (also bounds NPC resist in practice)
        float combatDistance = 128;                                           // fCombatDistance (reach unit)
        float blockPowMult = 0.5f;                                            // fBlockPowerAttackMult
        float rangedMin = 256, rangedDist = 1024;                             // fCombatRangedMinimumRange, fCombatRangedDistance
        float holdMinDist = 256, holdAbsMin = 0.1f;                           // fCombatRangedAttackHoldTimeMinDistance, fCombatRangedAttackHoldTimeAbsoluteMin (--point-blank)
        float bleedoutDefault = 0.15f;                                        // fBleedoutDefault
    };

    // Chocolate Poise as the list configures it (tools/list_balance/extract_combat_mods.py; formula = the mod's own source,
    // doodlum/skyrim-poise PoiseAV.cpp / HitEventHandler.cpp). on = false: the vanilla stagger MODEL (row 20).
    struct PoiseRules {
        bool on = false; int staggerMode = 1;
        float baseMult = 50, armorMult = 0.2f, regen = 0.045f, bash = 1.5f, bow = 1, creature = 2.5f, melee = 1, unarmed = 3, toPC = 1, toNPC = 1, weight = 2, magicHealth = 0;
        std::map<std::string, float> races;        // race EditorID -> the multiplier that replaces the race's mass
        float weap[10] = {}; bool hasWeap[10] = {}; // by Weapon::anim: the "Weapons.Damage" base the weight is lerped from
        float shield = 0; bool hasShield = false;
    };
    // Dynamic Follower Scaling (FollowerScaling.dll): every weapon hit of an actor in CurrentFollowerFaction is scaled by the
    // number of LOADED actors in that faction. mode 1: (1 - reduction)^(n - 1); mode 2: 1 / n.
    struct FollowerScaling { bool on = false; int mode = 1; float reduction = 0; };
    // One stealth perk row (tools/list_balance/compile_world.py "STEALTH perk tables"): a skill gate, a value, its ranked chain.
    struct StealthRow { int av = 15; float thr = 0, v = 1; std::string chain; };
    struct Data {
        std::string name = "builtin";
        PoiseRules poise; FollowerScaling dfs;
        std::map<std::string, double> gmst;
        Rules rules;
        std::vector<ActorSpec> specs;
        std::vector<ActorSpec> summonSpecs;                                  // the actors the list's summon spells summon (their own table: the foes' role picks never read them)
        std::vector<std::array<float, 3>> summonLimitRows;                   // ModCommandedActorLimit perks: Conjuration threshold, value, function (1 set, 2 add, 3 multiply)
        std::map<std::string, std::vector<std::pair<int, int>>> named;   // batch 3 sanity cells (compile_world.py named): label -> (level, spec index); TEST INPUT
        std::vector<SimSpell> foeSpells;
        std::vector<Encounter> encounters;
        std::vector<int> buckets;
        std::vector<SimSpell> spells;   // the castable pool a follower's book is drawn from (taught by a BOOK)
        std::vector<SimSpell> scrolls;  // SCRL records (the list's scrolls a follower may carry)
        std::vector<SimSpell> reanimSpells;   // card G7: the list's Reanimate spells (table rows only; AddPoolSpell)
        int keptReanim = 0, dropUnweighted = 0, dropUnanimated = 0, dropSelfBlast = 0;
        std::vector<FRace> races;
        FClass cls[3];                  // melee, ranged, mage
        std::vector<Weapon> wOneHand, wDagger, wTwoHand, wBow, wCrossbow;
        std::map<std::string, std::vector<float>> armor;   // "heavy_body" ... -> ladder of base ratings
        std::vector<float> arrows, bolts;
        std::vector<std::pair<float, float>> pots[3];      // health / magicka / stamina: (magnitude, duration)
        std::map<std::string, Thr> pAttack, pPower, pSpellMag, pSpellCost, pArmor;
        std::map<std::string, std::vector<StealthRow>> sneakAtk;          // EP 18 ModSneakAttackMult rows by weapon kind
        std::vector<StealthRow> sneakSkillRows, sneakNoiseRows;            // EP 57 ModDetectionSneakSkill (add), EP 48 ModDetectionMovement (multiply)
        int natures[7] = {};
        int rawSpells = 0, taughtSpells = 0, dropPlace = 0, dropNotCast = 0, dropReanim = 0, dropUnknown = 0;
        std::map<std::string, std::vector<int>> roles;     // "draugr", "troll" ... -> spec indices (scenarios)
        [[nodiscard]] double G(const char* k, double d) const { auto i = gmst.find(k); return i == gmst.end() ? d : i->second; }
    };

    [[nodiscard]] inline float PerkBest(const std::map<std::string, Thr>& t, const std::string& key, float skill, float none = 1.0f) {
        auto i = t.find(key); if (i == t.end()) return none;
        float v = none; bool any = false;
        for (const auto& p : i->second) if (p.first <= skill && (!any || (none >= 1.0f ? p.second > v : p.second < v))) { v = p.second; any = true; }
        return v;
    }

    // ── stealth (the class playstyle lab; every row is in iaus-classes.md "Stealth model") ──
    // The best row of every chain the actor's skill reaches, combined over the chains (mul: a product, else a sum).
    [[nodiscard]] inline float StealthCombine(const std::vector<StealthRow>& rows, const ActorSpec& a, bool mul, bool lowBest = false) {
        std::map<std::string, float> best;
        for (const StealthRow& r : rows) { if (a.Skill(r.av) + 0.01f < r.thr) continue; auto it = best.find(r.chain);
            if (it == best.end()) best[r.chain] = r.v; else it->second = lowBest ? std::min(it->second, r.v) : std::max(it->second, r.v); }
        float o = mul ? 1.0f : 0.0f; for (const auto& kv : best) o = mul ? o * kv.second : o + kv.second; return o;
    }
    [[nodiscard]] inline const char* SneakKind(const Weapon& w) { return !w.Has() ? "hand" : w.anim == 2 ? "dagger" : (w.anim == 1 || w.anim == 3 || w.anim == 4) ? "oneHand" : w.TwoH() ? "twoHand" : w.anim == 7 ? "bow" : w.anim == 9 ? "crossbow" : "hand"; }
    // The engine's sneak attack multiplier of one weapon. PROVEN (read, 1.6.1170): AE 44016 multiplies the hit by AE 18100's value, a
    // game-setting table indexed by the weapon's animation type (fCombatSneak*Mult), then by perk entry point 0x12 ModSneakAttackMult.
    // MODEL: which perks the follower holds (every playable chain his skill reaches, as the other perk tables assume). A chain that
    // has a row for a named weapon kind is not also read through its unconditioned ("all") row.
    [[nodiscard]] inline float SneakAttackMult(const Data& D, const ActorSpec& a, const Weapon& w) {
        static const char* g[10] = { "fCombatSneakHandMult", "fCombatSneak1HSwordMult", "fCombatSneak1HDaggerMult", "fCombatSneak1HAxeMult", "fCombatSneak1HMaceMult", "fCombatSneak2HSwordMult", "fCombatSneak2HAxeMult", "fCombatSneakBowMult", "fCombatSneakStaffMult", "fCombatSneakCrossbowMult" };
        static const double dv[10] = { 2, 3, 3, 3, 3, 2, 2, 2, 1, 2 };
        const int an = w.Has() ? std::clamp(w.anim, 0, 9) : 0;
        std::vector<StealthRow> rows; std::set<std::string> named;
        for (const auto& kv : D.sneakAtk) if (kv.first != "all") for (const StealthRow& r : kv.second) named.insert(r.chain);
        if (auto i = D.sneakAtk.find(SneakKind(w)); i != D.sneakAtk.end()) rows = i->second;
        if (auto i = D.sneakAtk.find("all"); i != D.sneakAtk.end()) for (const StealthRow& r : i->second) if (!named.count(r.chain)) rows.push_back(r);
        return static_cast<float>(D.G(g[an], dv[an])) * StealthCombine(rows, a, true);
    }

    // ── combat formulas ─────────────────────────────────────────────────────────────────────────────────
    // Weapon damage before the target's armor: (weapon + ammo) x fDamageWeaponMult x lerp(fDamageSkillMin, fDamageSkillMax,
    // skill / 100) x ModAttackDamage perks; a power attack x (1 + fPowerAttackDefaultBonus) x ModPowerAttackDamage perks.
    [[nodiscard]] inline float WeaponDamage(const Rules& r, const ActorSpec& a, const Weapon& w, bool power) {
        float d;
        if (w.Has()) d = (w.dmg + (w.Ranged() ? a.ammoDmg : 0.0f)) * r.dmgWeaponMult * (r.dmgSkillMin + (r.dmgSkillMax - r.dmgSkillMin) * std::clamp(a.Skill(w.skill), 0.0f, 100.0f) / 100.0f);
        else d = a.unarmed * a.atkDmgMult;
        d *= a.atkMult;
        if (power) d *= (1.0f + r.powBonus) * a.powMult;
        return d;
    }
    // Physical mitigation: min(fMaxArmorRating, rating x fArmorScalingFactor + worn pieces x fArmorBaseFactor x 100) percent.
    [[nodiscard]] inline float ArmorMitigation(const Rules& r, float rating, int pieces) {
        return std::clamp((rating * r.armorScale + static_cast<float>(pieces) * r.armorBasePiece * 100.0f), 0.0f, r.armorMax) / 100.0f;
    }
    // Magicka cost for an NPC caster: cost x max(0, 1 - fMagicCasterSkillCostMult x (fMagicCasterSkillCostBase x skill)^fMagicSkillCostScale).
    // The SHAPE is UNPROVEN (it reproduces the documented player curve with the player's settings); see the doc.
    [[nodiscard]] inline float SkillCostMult(const Rules& r, float skill) {
        return std::max(0.0f, 1.0f - r.costMult * std::pow(std::max(0.0f, r.costBase * skill), r.costScale));
    }
    [[nodiscard]] inline float PowerStamina(const Rules& r, const Weapon& w) { return (r.powStaBase + w.weight * r.powStaMult) * r.powStaPenalty; }

    // SIM V4: THE ENGINE ROWS THE LAB TELLS APART (native cast/Actuation.h EngineRow, cast/Archetype.cpp kRows). Seated = any row Harbinger seats a
    // caster for. SEATS TODAY (Harbinger ABI 46, 2026-11-23): Stagger, Disarm, TargetEffect and Paralyze ARE seated, so by default no row is refused.
    // `--seats old` is the game before ABI 46: kSimOldUnseated never start (cast/Archetype.cpp kUnseatedRows as it was). A Script row is never
    // refused: it takes the measured unfired chance like any other, with the SHORT window and a fight-long hold after one failure (review F5).
    // MIRRORED from MFO's adopt-seats (main 5b17b58e, native/cast/SeatRows.h, which the lab includes): whether a row is served is SeatRows::RowSeated
    // itself (SimRowSeated below); the keyed-effect will-work and the two-zero-fires hold are Fight::WillWorkFor / NoteZeroEffect (World.h).
    // NOT MIRRORED (the lab has no such effect or record): Disarm's right-hand-weapon need, Command's summoned need, effect and magic effect
    // CONDITIONS (the lab's stand-in is the foe's own flags), the approximate-row exemption. To add a row: a value here and its key in SimRowOf.
    enum SimRow : int { kRowNone = 0, kRowSeated, kRowStagger, kRowDisarm, kRowTargetEffect, kRowParalyze, kRowScript, kRowReanimate };
    inline constexpr const char* kSimRowName[] = { "none", "seated", "Stagger", "Disarm", "TargetEffect", "Paralyze", "Script", "Reanimate" };
    inline constexpr int kSimOldUnseated[] = { kRowStagger, kRowDisarm, kRowTargetEffect, kRowParalyze, kRowReanimate };
    [[nodiscard]] inline bool RowNeverStarts(int row) { for (int r : kSimOldUnseated) if (r == row) return true; return false; }   // read only under --seats old
    // The four hostile rows Harbinger ABI 46 seats, as native names them (cast/Archetype.cpp HostileSeatRow).
    [[nodiscard]] inline bool SimHostileSeatRow(int row, MFO::SeatRows::Row& out) {
        switch (row) { case kRowStagger: out = MFO::SeatRows::Row::Stagger; return true; case kRowDisarm: out = MFO::SeatRows::Row::Disarm; return true;
                       case kRowTargetEffect: out = MFO::SeatRows::Row::TargetEffect; return true; case kRowParalyze: out = MFO::SeatRows::Row::Paralyze; return true; default: return false; }
    }
    // Is the row served: native's own rule (SeatRows::RowSeated) on what Harbinger reports. `old` = an ABI under 46 (no coverage word), today = ABI 46
    // with the four rows and the equip admission covered. Reanimate is not one of the four: it is unseated only in the old game (kSimOldUnseated).
    [[nodiscard]] inline bool SimRowSeated(int row, bool old) {
        MFO::SeatRows::Row r{}; if (!SimHostileSeatRow(row, r)) return !(old && RowNeverStarts(row));
        std::uint32_t cov = MFO::SeatRows::kTypeEquipBit; for (const MFO::SeatRows::RowInfo& i : MFO::SeatRows::kRows) cov |= i.coverageBit;
        return MFO::SeatRows::RowSeated(old ? MFO::SeatRows::kMinAbiCoverage - 1 : MFO::SeatRows::kMinAbiCoverage, cov, r);
    }
    // cast/CastOn.cpp OffenseNeverStartedFightLong, by name: the SHORT window and a fight-long hold after ONE failure (also for a spell with no row).
    [[nodiscard]] inline bool RowFightLong(int row) { return row == kRowNone || row == kRowScript; }
    inline constexpr float kNeverShortSec = 4.0f, kNeverLongSec = 6.5f;   // apmf/APMFBridge.h kHealClaimNeverObservedCapMs 4000 / kHoldLastSeenCapMs 6500
    // One effect's row (kRows, the keys the lab's extract can tell: archetype, AV, self delivered, hostile).
    [[nodiscard]] inline int SimRowOf(int arch, int av, bool self, bool hostile) {
        const bool vm = arch == 0 || arch == 4 || arch == 5 || arch == 34;   // ValueModifier and its aliases
        if (arch == 33 && hostile && !self) return kRowStagger;
        if (arch == 21 && hostile && !self) return kRowParalyze;
        if (arch == 24 && hostile && !self) return kRowTargetEffect;   // TurnUndead (Banish and CommandSummoned key the same row)
        if (arch == 1) return kRowScript;
        if (arch == 22 && !self && !hostile) return kRowReanimate;
        if (vm && hostile && !self) return av == 24 ? kRowSeated : kRowNone;   // health: Offensive. Magicka / stamina: no caster
        if (vm && !hostile && self && (av == 24 || av == 25)) return kRowSeated;   // Restore
        if (arch == 18 || arch == 35) return kRowSeated;   // Summon, Cloak
        return kRowNone;
    }

    inline bool g_bookCensus = false;   // --book-census: one line per taught spell, its nature and IAUS verdict (tools/list_balance/book_census.py reads them)
    // ── spell classification (one reader for the follower pool and the foes' spells) ────────────────────
    inline void Classify(const J& sp, SimSpell& out) {
        const auto& fx = sp["effects"].a; SpellEntry& e = out.e;
        const auto archs = [&](int a) { return std::any_of(fx.begin(), fx.end(), [a](const J& x) { return static_cast<int>(x["archetype"].num()) == a; }); };
        bool summon = false, reanimate = false, ward = false, control = false, cloak = false, hostileAny = false, heal = false, weighable = false, anyLight = false, healthAv = false;
        double autoCost = 0, topCost = -1, topHostCost = -1, rowCost = -1; int topArch = -1, resist = -1;
        const std::string& dl = sp["delivery"].s; const bool self = dl == "Self";
        const bool conc = sp["castType"].s == "Concentration";
        static const std::map<std::string, int> kSchool = { {"Alteration", 18}, {"Conjuration", 19}, {"Destruction", 20}, {"Illusion", 21}, {"Restoration", 22} };
        for (const J& x : fx) {
            const int av = static_cast<int>(x["av"].num(-1)), arch = static_cast<int>(x["archetype"].num());
            const double mag = x["mag"].num(), d = x["dur"].num();
            const double ec = x["baseCost"].num() * std::pow(std::max(mag, 1.0) * std::max(d, 1.0), 1.1);
            autoCost += ec;
            const bool host = x["hostile"].yes(), detr = x["detrimental"].yes();
            if (ec > topCost) { topCost = ec; topArch = arch; out.skillReq = static_cast<int>(x["minSkill"].num());
                auto si = kSchool.find(x["skill"].s); out.school = si == kSchool.end() ? -1 : si->second; }
            if (const int er = SimRowOf(arch, av, self, x["hostile"].yes()); er != kRowNone && ec > rowCost) { rowCost = ec; out.row = er; }
            if (arch == kArSummon) { summon = true; out.summonDur = static_cast<float>(d > 0 ? d : 60); if (out.summonIdx.empty()) for (const J& si : x["summon"].a) out.summonIdx.push_back(static_cast<int>(si.num(-1))); }
            if (const J& h = x["hazard"]; h.has() && !out.hazard) {   // its spell goes through this same classifier, then becomes what ONE TICK does: the engine does not stack one spell of one caster on one victim, so a tick adds its interval's share
                HazardSpec z; const int fl = static_cast<int>(h["flags"].num()); z.viaImpact = h["via"].s == "impact"; z.drops = fl & 16; z.pcOnly = fl & 1; z.limit = static_cast<int>(h["limit"].num());
                z.interval = std::max(kHazard.minInterval, static_cast<float>(h["interval"].num())); const double area = x["area"].num();
                z.radius = Prune::BlastUnits(static_cast<float>((fl & 8) && area > 0 ? area : h["radius"].num()), 0.0f, false);   // MODEL: the record's radius is in feet, as an effect's area is
                z.lifetime = static_cast<float>((fl & 2) && d > 0 ? d : h["lifetime"].num()); z.lifetime = z.lifetime > 0 ? z.lifetime : kHazard.maxLifeSec;
                auto hs = std::make_shared<SimSpell>(); Classify(h["spell"], *hs);
                const float conc2 = h["spell"]["castType"].s == "Concentration" ? 0.5f : 1.0f;   // Classify made a stream a 2 s lump
                hs->dmg *= z.interval * conc2; hs->mpDmg *= z.interval * conc2; hs->spDmg *= z.interval * conc2; if (hs->dotDur > 0) hs->dot *= z.interval / hs->dotDur;
                hs->e.name = h["edid"].s; hs->e.areaUnits = 0; z.spell = std::move(hs);
                if (z.radius > 0) out.hazard = std::move(z); }
            if (arch == kArReanimate) { reanimate = true; out.ctrlMaxLevel = static_cast<float>(mag); out.summonDur = static_cast<float>(d > 0 ? d : 60); }   // card G7: the magnitude is the highest level it raises
            weighable |= Prune::EffectWeighable(arch); anyLight |= arch == kArLight; healthAv |= av == kAvHealth;
            if (arch == kArCalm || arch == kArDemoralize || arch == kArFrenzy || arch == kArParalysis || arch == kArTurnUndead) {
                control = true; out.ctrlDur = std::max(out.ctrlDur, static_cast<float>(d));
                if (arch == kArParalysis) out.ctrlPar = true; else out.ctrlMaxLevel = static_cast<float>(mag);
                if (arch == kArCalm) out.ctrlCalm = true;
                if (arch == kArDemoralize || arch == kArTurnUndead) out.ctrlFlee = true;
                if (arch == kArFrenzy) out.ctrlFrenzy = true;
                if (arch == kArTurnUndead) { out.turnUndead = true; e.undeadOnly = true; }
            }
            if (arch == kArStagger) out.stagger = true;
            for (int ia : kIndirectArch) if (arch == ia) out.indirect = true;
            if (arch == kArCloak && self) { cloak = true; out.areaKind = 1; out.areaDps = std::max(out.areaDps, static_cast<float>(mag)); out.areaDur = std::max(out.areaDur, static_cast<float>(d)); }   // SIM V3: a cloak is an area that moves with him
            if (host || detr) { hostileAny = true; if (ec > topHostCost) { topHostCost = ec; resist = static_cast<int>(x["resist"].num(-1)); } }
            const bool vm = arch == kArValueMod || arch == kArPeakValueMod || arch == kArAbsorb || arch == kArDualValueMod;
            if ((host || detr) && vm) {
                if (av == kAvHealth) { if (d > 1.0 && !conc) { out.dot += static_cast<float>(mag); out.dotDur = std::max(out.dotDur, static_cast<float>(std::min(d, 30.0))); } else out.dmg += static_cast<float>(mag); }
                else if (av == kAvMagicka) out.mpDmg += static_cast<float>(mag);
                else if (av == kAvStamina) out.spDmg += static_cast<float>(mag);
            }
            // card I6: the ward shape is cast/Archetype.cpp anyWard (a ValueModifier-family effect on WardPower: archetypes 0 / 4 / 5 / 32 / 34)
            if (av == kAvWardPower && (arch == kArValueMod || arch == kArAbsorb || arch == kArDualValueMod || arch == 32 || arch == kArPeakValueMod)) ward = true;
            if (!host && !detr && (arch == kArValueMod || arch == kArPeakValueMod)) {
                if (av == kAvWardPower) {}
                else if (av == kAvHealth) { heal = true; if (d > 1.0 && !conc) { out.hot += static_cast<float>(mag); out.hotDur = std::max(out.hotDur, static_cast<float>(std::min(d, 30.0))); } else out.heal += static_cast<float>(mag); }
                else if (av == kAvDamageResist) { out.armor += static_cast<float>(mag); out.buffDur = std::max(out.buffDur, static_cast<float>(d)); e.meleeBuff = true; }
                else if (av >= kAvPoisonResist && av <= kAvResistMagic) { out.resistBuff = static_cast<float>(mag); out.resistBuffAv = av; out.buffDur = std::max(out.buffDur, static_cast<float>(d)); }
            }
            e.areaUnits = std::max(e.areaUnits, Prune::BlastUnits(static_cast<float>(x["area"].num()), 0.0f, false));
            out.buffDur = std::max(out.buffDur, static_cast<float>(d));
        }
        // THE GAME'S OWN NATURE RULE (placed-spells L1): Prune::ClassifyNature, fed as SpellBook.cpp StaticOf feeds it. The shape precedence is cast/Archetype.cpp's.
        const bool anyScript = archs(kArScript), anyBound = archs(kArBoundWeapon), anyCloak = archs(kArCloak);
        const bool spellShape = !summon && !reanimate;
        const bool buffKind = !hostileAny && !control && !healthAv;      // CasterConsent::ClassifySpell
        Prune::NatureIn ni;
        ni.kind = (hostileAny || control) ? Prune::Kind::Offense : healthAv ? Prune::Kind::Heal : Prune::Kind::Buff;
        ni.summon = summon; ni.reanimate = reanimate && !summon;
        ni.ward = spellShape && !anyScript && !anyBound && !anyCloak && ward;
        ni.selfCloak = spellShape && !anyScript && !anyBound && anyCloak && self;
        ni.control = control;
        ni.scriptCostliest = spellShape && anyScript && topArch == kArScript;
        ni.natureUndeclared = buffKind && spellShape && !self && std::all_of(fx.begin(), fx.end(), [](const J& x) { return static_cast<int>(x["archetype"].num()) == kArScript; });
        e.nature = Prune::ClassifyNature(ni);
        if (e.nature == Prune::Nature::Unknown || fx.empty()) { e.nature = Prune::Nature::Unknown; e.drop = Prune::Drop::UnknownNature; }
        if (ni.reanimate) out.reanimate = true;
        if (cloak && ni.selfCloak) e.meleeBuff = true;
        out.summon = e.nature == Prune::Nature::Summon;
        out.ctrl = e.nature == Prune::Nature::Control;
        e.resistAv = (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control) ? resist : -1;
        out.element = resist == kAvResistFire ? "fire" : resist == kAvResistFrost ? "frost" : resist == kAvResistShock ? "shock" : "";
        e.role = Prune::RoleOf(e.nature, e.resistAv);
        e.selfDelivery = self; e.touch = dl == "Touch"; e.concentration = conc;
        if (self && !cloak && e.areaUnits > 0.0f && sp["edid"].s.find("Circle") != std::string::npos) { out.areaKind = 2; out.areaArmor = out.armor; out.areaDur = out.buffDur; }   // SIM V3 (MODEL): a circle stays where it was cast
        // Actuation::AimedReachOf (cast/CanAct.cpp): the record's range counts only from kMinSaneReach (300) up, else the projectile's
        // (not extracted: 4096 stands in). Requiem's target heals carry range 30, which the game does not read as a reach.
        e.reach = (e.selfDelivery || e.touch) ? 0.0f : static_cast<float>(sp["range"].num()) >= 300.0f ? static_cast<float>(sp["range"].num()) : 4096.0f;
        // THE GAME'S OWN PLACEMENT RULE (placed-spells L1): the same Prune::IsPlacement SpellBook.cpp calls, fed with what the extractor read off the records
        // ("placed", list_balance/extract_world.py placed_row). A spell row without "placed" is a hand-written one (EnvTest): no impact hazard, no projectile.
        {
            const J& pl = sp["placed"];
            const bool outranked = !spellShape || anyScript || anyBound || anyCloak || ward || anyLight || (self && archs(kArSpawnHazard));   // cast/Archetype.cpp: the shapes tested before Wall
            Prune::PlacementIn in;
            in.targetLocationDelivery = dl == "TargetLocation";
            in.wall      = !outranked && Prune::IsWallShape(pl["drops"].yes(), conc, self);
            in.rune      = !outranked && !in.wall && in.targetLocationDelivery && pl["proj"].yes();
            in.tlBurst   = !outranked && !in.wall && in.targetLocationDelivery && !pl["proj"].yes();
            in.summon    = summon;
            // A FOE's spells are the engine AI's, not IAUS's: the two kinds only IAUS's book leaves out (a wall, a self blast) stay castable for a foe (Load.h reads foeDrop).
            { Prune::PlacementIn foe = in; foe.wall = false; out.foeDrop = e.drop; if (Prune::IsPlacement(foe)) out.foeDrop = Prune::Drop::Placement; }
            in.selfBlast = Prune::IsSelfBlastPlacement(self, conc, e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control,
                                                       e.areaUnits > 0.0f || pl["payload"].num() > 0.0, pl["impact"].yes(), pl["hazHostile"].yes());
            if (Prune::IsPlacement(in)) { Prune::StaticIn st; st.nature = Prune::Nature::Buff; st.targetLocation = true; st.selfBlast = in.selfBlast; e.drop = Prune::StaticDrop(st); }
            // cast/SpellSupport.cpp CastRoadUnsupported (Drop::Unanimated, tested before Placement as in StaticDrop): a Buff-kind Light aimed or at a location (Magelight).
            if (buffKind && !(conc && !self) && spellShape && anyLight && (dl == "Aimed" || in.targetLocationDelivery)) e.drop = Prune::Drop::Unanimated;
        }
        if (sp["castType"].s == "ConstantEffect") e.drop = out.foeDrop = Prune::Drop::NotCastable;
        // Q91 (batch I item 6, Prune::WeighableSpell): no effect IAUS can value (Script, a scripted spawn, kNone, an unknown id) = hidden from IAUS
        if (g_t2.unweighted && !fx.empty() && !weighable) for (Prune::Drop* d : { &e.drop, &out.foeDrop }) if (*d == Prune::Drop::Keep) *d = Prune::Drop::Unweighted;
        out.healOther = heal && !self;
        out.cost0 = static_cast<float>(sp["cost"].num());
        if (out.cost0 <= 0 && sp["autoCalc"].yes()) out.cost0 = static_cast<float>(autoCost);   // APPROXIMATION, only when the record stores 0 and does not override
        e.cost = out.cost0;
        out.recovery = static_cast<float>(sp["recovery"].num());
        // Timing: a fire-and-forget spell charges for SPIT chargeTime (0.5 s is the usual record value); a concentration
        // spell is modelled as a 2 s stream (its per-second magnitude and cost x 2: MODEL).
        if (conc) { out.castSec = 2.0f; out.dmg *= 2.0f; out.heal *= 2.0f; out.mpDmg *= 2.0f; out.spDmg *= 2.0f; out.cost0 *= 2.0f; e.cost = out.cost0; }
        else out.castSec = std::clamp(static_cast<float>(sp["chargeTime"].num(0.5)), 0.25f, 4.0f);
        e.name = sp["edid"].s;
    }

    // ── loading ─────────────────────────────────────────────────────────────────────────────────────────
    inline Weapon ReadWeapon(const J& w) {
        Weapon x; if (!w.has()) return x;
        x.name = w["name"].s; x.dmg = static_cast<float>(w["dmg"].num()); x.speed = static_cast<float>(w["speed"].num(1)); x.reach = static_cast<float>(w["reach"].num(1));
        x.stagger = static_cast<float>(w["stagger"].num()); x.anim = static_cast<int>(w["anim"].num()); x.skill = static_cast<int>(w["skill"].num(kAvOneHanded));
        x.crit = static_cast<float>(w["crit"].num()); x.silver = w["silver"].yes(); x.weight = static_cast<float>(w["weight"].num(8));
        if (x.speed <= 0.05f) x.speed = 1.0f;
        if (x.skill < 6 || x.skill > 8) x.skill = x.Ranged() ? kAvMarksman : x.TwoH() ? kAvTwoHanded : kAvOneHanded;
        return x;
    }

    inline void ApplyRules(Data& d) {
        Rules& r = d.rules; const Rules v;   // v = Skyrim.esm's values
        auto g = [&](const char* k, float dv) { return static_cast<float>(d.G(k, dv)); };
        r.dmgSkillMin = g("fDamageSkillMin", v.dmgSkillMin); r.dmgSkillMax = g("fDamageSkillMax", v.dmgSkillMax); r.dmgWeaponMult = g("fDamageWeaponMult", v.dmgWeaponMult);
        r.armorScale = g("fArmorScalingFactor", v.armorScale); r.armorMax = g("fMaxArmorRating", v.armorMax); r.armorBasePiece = g("fArmorBaseFactor", v.armorBasePiece);
        r.armorSkillLo = g("fArmorRatingBase", v.armorSkillLo); r.armorSkillHi = g("fArmorRatingMax", v.armorSkillHi);
        r.powBonus = g("fPowerAttackDefaultBonus", v.powBonus); r.powStaBase = g("fStaminaAttackWeaponBase", v.powStaBase); r.powStaMult = g("fStaminaAttackWeaponMult", v.powStaMult);
        r.powStaPenalty = g("fPowerAttackStaminaPenalty", v.powStaPenalty);
        r.combatMp = g("fCombatMagickaRegenRateMult", v.combatMp); r.combatSp = g("fCombatStaminaRegenRateMult", v.combatSp);
        r.delayHp = g("fDamagedHealthRegenDelay", v.delayHp); r.delayMp = g("fDamagedMagickaRegenDelay", v.delayMp); r.delaySp = g("fDamagedStaminaRegenDelay", v.delaySp);
        r.costBase = g("fMagicCasterSkillCostBase", v.costBase); r.costMult = g("fMagicCasterSkillCostMult", v.costMult); r.costScale = g("fMagicSkillCostScale", v.costScale);
        r.holdMinDist = g("fCombatRangedAttackHoldTimeMinDistance", v.holdMinDist); r.holdAbsMin = g("fCombatRangedAttackHoldTimeAbsoluteMin", v.holdAbsMin); r.bowDraw = g("fBowDrawTime", v.bowDraw); r.diffToPC = g("fDiffMultHPToPCN", v.diffToPC); r.diffByPC = g("fDiffMultHPByPCN", v.diffByPC);
        r.hpPerLevel = g("fNPCHealthLevelBonus", v.hpPerLevel); r.hmsPerLevel = g("iAVDhmsLevelUp", v.hmsPerLevel); r.skillPerLevel = g("iAVDSkillsLevelUp", v.skillPerLevel);
        r.skillStart = g("iAVDSkillStart", v.skillStart); r.maxResist = g("fPlayerMaxResistance", v.maxResist); r.combatDistance = g("fCombatDistance", v.combatDistance);
        r.blockPowMult = g("fBlockPowerAttackMult", v.blockPowMult); r.rangedMin = g("fCombatRangedMinimumRange", v.rangedMin); r.rangedDist = g("fCombatRangedDistance", v.rangedDist);
        r.bleedoutDefault = g("fBleedoutDefault", v.bleedoutDefault);
    }

    inline void AddPoolSpell(Data& b, SimSpell&& s) {
        if (s.e.drop == Prune::Drop::Keep) { ++b.natures[static_cast<int>(s.e.nature)]; b.spells.push_back(std::move(s)); }
        // Card G7: a Reanimate spell is KEPT, in its own list. IAUS still has no family for it (Prune: nature Unknown, as in the game), so
        // its book entry stays dropped for IAUS; a table row with cond.corpse_within casts it at a corpse. It enters a book only by name
        // (a class row's Reanimate role, --teach reanimate), never by the random draw, so no other class's book changes.
        else if (s.reanimate && g_t2.reanimate && s.e.drop == Prune::Drop::UnknownNature) { ++b.keptReanim; b.reanimSpells.push_back(std::move(s)); }
        else { ++b.natures[0]; switch (s.e.drop) { case Prune::Drop::Placement: ++b.dropPlace; break; case Prune::Drop::NotCastable: ++b.dropNotCast; break; case Prune::Drop::Unweighted: ++b.dropUnweighted; break; case Prune::Drop::Unanimated: ++b.dropUnanimated; break; case Prune::Drop::SelfBlast: ++b.dropSelfBlast; break; default: ++(s.reanimate ? b.dropReanim : b.dropUnknown); } }
    }

    inline void TagRoles(Data& d) {
        auto has = [](const std::string& s, const char* k) { return s.find(k) != std::string::npos; };
        for (int i = 0; i < static_cast<int>(d.specs.size()); ++i) {
            const ActorSpec& a = d.specs[static_cast<std::size_t>(i)];
            bool healer = false, caster = false;
            for (int si : a.spells) { const SimSpell& s = d.foeSpells[static_cast<std::size_t>(si)]; healer |= s.healOther; caster |= s.e.nature == Prune::Nature::Offense; }
            auto add = [&](const char* r) { d.roles[r].push_back(i); };
            if (has(a.race, "Draugr")) add(a.ranged.Has() && !a.melee.Has() ? "draugr_archer" : "draugr");
            if (has(a.race, "Troll")) add(has(a.race, "Frost") ? "frost_troll" : "troll");
            if (has(a.race, "AtronachFrost")) add("frost_atronach");
            if (has(a.race, "AtronachFlame")) add("flame_atronach");
            if (a.dragon && !a.undead && has(a.race, "DragonRace")) add("dragon");
            if (has(a.race, "Wolf")) add("wolf");
            if (has(a.race, "Giant")) add("giant");
            if (!a.creature && has(a.name, "Bandit")) add(caster ? "bandit_mage" : (a.ranged.Has() && (!a.melee.Has() || has(a.name, "Missile") || has(a.name, "Archer"))) ? "bandit_archer" : "bandit");
            if (!a.creature && healer) add("healer");
            if (!a.creature && caster && !has(a.name, "Bandit")) add("mage");
        }
    }

    inline Data LoadData(const char* path);   // below
    inline Data BuiltinData();                 // Builtin.h

}
