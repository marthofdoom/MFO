#pragma once
// iaus_sim/World.h -- the fight as a plain, COPYABLE value (sim v2). HOST-ONLY.
//
// A Fight holds every actor, the followers' IAUS memory and the tallies. It is copied freely: the expert oracle
// (Oracle.h) clones it to roll candidate actions forward. Nothing in it points into itself.
//
// TIME: the world steps at 30 Hz. The followers' decision layer is served round-robin, one follower every 133 ms
// (the worker pump, Diagnostics kPumpMs), so each follower decides every 133 ms x followers (Candidates.cpp).
//
// WHO SWINGS: act.attack only names the target (Vocabulary.h kActAttack "target = the chosen foe"): the ENGINE's
// combat AI closes, swings, blocks and shoots. Brain() below models that AI for every actor (foes, the player
// model, and a follower whenever his decision layer is not holding his body), from the combat style record.
#include "iaus/Generate.h"
#include "iaus_sim/Load.h"
#include "iaus_sim/Detect.h"

#include "iaus/Reach.h"
#include "iaus/Estimate.h"    // SIM V3 `burst`: LapGross / BurstReads, the game's own pure lap maths
#include "iaus/Reposition.h"   // batch I: the spell range, the back-off need, the hidden step (the game's own pure functions)
#include <unordered_map>
#include "iaus_sim/Playstyles.h"

namespace Sim {

    // ── timing (seconds). MODEL values unless a source is named; every row is in the doc's model table ──
    struct TM {
        static constexpr float dt = 1.0f / 30.0f;
        static constexpr float pump = 0.133f;          // PROVEN (code): Diagnostics kPumpMs
        static constexpr float swing1H = 1.00f;        // one-handed swing, start to recovered, at weapon speed 1.0
        static constexpr float swing2H = 1.40f;        // two-handed (the engine plays it x fWeaponTwoHandedAnimationSpeedMult)
        static constexpr float swingCreature = 1.30f;
        static constexpr float hitFrac = 0.45f;        // the hit frame, as a fraction of the swing
        static constexpr float powerMult = 1.55f, powerHitFrac = 0.55f;
        static constexpr float bash = 0.9f, bashHitFrac = 0.35f;
        static constexpr float bowNock = 0.55f, bowHoldMin = 0.25f, bowHoldMax = 1.5f, bowRecover = 0.35f;   // + fBowDrawTime / speed (GMST); hold = fCombatRangedAttackHoldTime*
        static constexpr float crossbowReload = 1.8f;
        static constexpr float castRecover = 0.45f;    // after the release
        static constexpr float drink = 1.2f, drinkAt = 0.6f;
        static constexpr float swap = 1.3f;            // sheathe + draw the other category
        static constexpr float taunt = 1.2f;           // the taunt idle
        static constexpr float staggerSec = 1.1f, bashStagger = 0.8f, recoil = 0.6f;
        static constexpr float thinkMin = 0.05f, thinkMax = 0.45f;   // the engine AI pause between attacks, / offensive mult. CALIBRATED: field hit cadence p50 1.21 s (calib_logs.py)
        static constexpr float arrowSpeed = 3600.0f, spellSpeed = 2400.0f;
        static constexpr float downSec = 9.5f;         // bleedout before a follower stands again. CALIBRATED: field p50 9.5 s (n=50)
        static constexpr float tauntDur = 15.0f, tauntCooldown = 30.0f, tauntRadius = 600.0f;   // fTauntDuration 15 s (code); cooldown 30 s (ruling Q69)
        static constexpr float retreatSec = 2.0f;
        static constexpr float aimMiss = 0.12f;        // an aimed projectile at a moving target
    };

    // THE MELEE REACH IS LEARNED (ruling Q68; marth 2026-11-17 "Edge reach + split uses", native/iaus/ReachBook.h, the game's own book):
    // per weapon, from hits and misses, as a reach to the foe's EDGE. 0 = off (the constant fMeleeReach 200), 1 = cold (every fight starts
    // at the seed), 2 = warm (the session's earlier swings are replayed into the book first: 20 outcomes around the weapon's true reach with
    // 4% noise). The game's book lives for the session. MODEL: the lab's fighters swing ONE blow at a time (no combo framework), so every
    // outcome is a chain of one swing: the sequence learner of key 1 and the weapon's learner see the same outcomes and the two reads
    // (Snapshot::weaponEdge, comboEdge) agree. The lab's foes carry no body radius (Combatant::bodyRadius 0 = the humanoid kRefRadius).
    inline int g_learnReach = 2;
    inline constexpr std::uint32_t kReachWeapon = 1;   // the book's weapon key: one melee weapon per follower in the lab
    inline constexpr float kReachSeedEdge = 200.0f - MFO::Iaus::ReachBook::kRefRadius;   // Fills.cpp LearnReach: fMeleeReach (200) less a humanoid radius
    // MODEL, UNPROVEN: a calmed foe that the party damages fights again (the common in-game reading: one free blow, then he is
    // back). false = the first model (a Calm holds for its whole duration whatever happens), kept for comparison (--calm-holds).
    inline bool g_consumables = false;   // --consumables on (tune 2.11.1, Setup.h FillConsumables): gold, item values, buff potions and food in every IAUS kit
    inline bool g_calmBreaks = true;
    // FOLLOWER DEATH (marth 2026-11-10: "Lorerim by default has follower death. I personally have mine turned off"). false (the
    // default, his setup and the one the field calibration comes from): a follower at 0 health goes down and stands up after
    // TM::downSec. true (--follower-death): he is dead for good, and Utility counts that death three times a down (MODEL).
    // g_mortalKnown: the snapshot tells IAUS which of the two it is (Snapshot::mortal); --mortal-blind hides it.
    inline bool g_followerDeath = false, g_mortalKnown = true;
    // --regroup-road: the ch.24 regroup client is modelled as existing (Snapshot::regroupRoad). MODEL: a regroup order is the
    // oracle's own move order (3 s toward the anchor, no blows on the way), which is harsher than ch.24 (he keeps fighting).
    inline bool g_regroupRoad = false;
    // --roads ground,block,bash: the Harbinger capabilities of doc section 26 modelled as existing (the oracle's own extended actions).
    inline bool g_groundRoad = false, g_blockRoad = false, g_bashRoad = false;
    // STEALTH AND DISENGAGE ROADS (Docs/research/2026-11/iaus-classes.md section 6, iaus-sim-v2.md section 30). Each level models the
    // game's own switches as ON; detection itself is always the ENGINE's formula (Detect.h), never a lab rule.
    //   0 off  : nobody sneaks in a fight ([Debug] bSneakInCombat=0, the game's default). Draw for draw the lab before the stealth model.
    //   1 g1   : a sneak class holds the stance for the whole fight (card G1). The engine hides him and multiplies his blows by its
    //            own rules; IAUS reads the verdict (Combatant::detectsMe, Snapshot::selfSneaking) but has no hold row.
    //   2 hide : + G2 Layer 1's hold row (Snapshot::hideRoad) and the free stealth entry of card C13.
    //   3 g3   : + card G3 (Snapshot::concealRoad): the hide run of a sneak class and the flee of every other disengage class.
    inline int g_stealth = 3;   // the lab's default is every road T2 builds (the tuning target); --stealth off is the game with its switches at their defaults
    // --blow-stands: a reading the game does NOT have (MFO-B784). Main's 3db7882 keeps the hide hold's stance claim until the blow
    // RESOLVES (scheduler/IausSource.cpp NoteBlow), so false (default) is the game: the stance outlives the blow. true measures what an
    // early release would cost: he stands before the hit lands and the engine's sneak attack (attacker sneaking) does not apply.
    inline bool g_blowStands = false;
    // --light-scale N: the engine's cached light level of an actor standing in full daylight (MODEL: 100). The level's real scale is
    // not known (iLightLevelMax is 300 in Skyrim.esm; probe P4 of g2-hide-design.md is not built), and the light term is the only
    // thing that can outweigh a high Sneak skill, so every stealth number should be read with this sensitivity in mind.
    inline float g_lightScale = 100.0f;
    // The T2 batch S switches (g_t2) live in Data.h: the loader reads two of them.
    // Hybrid / T4 go-in rules (native/classes/Classes.h GoIn, same order).
    enum : int { kGoNone = 0, kGoNever, kGoBias, kGoSummons, kGoParty, kGoControlled };
    enum class Act : std::uint8_t { None = 0, Swing, Power, Bash, Shoot, Cast, Drink, Swap, Taunt, Block };
    enum class Role : std::uint8_t { Player = 0, Follower, Summon, Foe };

    struct Effect { int kind = 0; float v = 0; int av = -1; double until = 0; std::uint32_t src = 0; int buff = -1; };   // 0 dot, 1 hot, 2 armor, 3 resist, 4 magicka per second, 5 stamina per second (a restore-over-time potion),
    // 6 fortify a pool (av 0 health, 1 magicka, 2 stamina: max and current up by v while it lasts), 7 fortify weapon damage (v percent). buff >= 0: a
    // buff POTION's effect (--consumables, iaus/PotionBuff.h BuffKind), so Kit::buff[kind].active follows it.
    struct Shot { double at = 0; std::uint32_t src = 0, tgt = 0; float dmg = 0; int spell = -1; bool power = false, fromIaus = false; float x = 0, y = 0; float mag = 1; bool corrected = false, dodged = false; };

    struct Body {
        Combatant c;
        std::shared_ptr<const ActorSpec> spec;
        Role  role = Role::Foe;
        int   ctl = -1;                 // follower: index into Fight::ctl
        bool  alive = true;
        // activity
        Act   act = Act::None;
        double actEnd = 0, hitAt = -1;  // free again at actEnd; the pending impact / release at hitAt (< 0 none)
        std::uint32_t actTarget = 0; int actSpell = -1; bool actIaus = false; float actCost = 0; float swingDist = 0;
        double think = 0, retargetAt = 0;
        double blockUntil = 0, staggerUntil = 0, ctrlUntil = 0, tauntUntil = 0, fleeUntil = 0, downUntil = 0, frenzyUntil = 0, retreatUntil = 0;
        double backUntil = 0;   // --foe-styles: a foe's backpedal after a blow
        std::uint32_t tauntBy = 0, lastAttacker = 0;
        double regenAt[3] = { 0, 0, 0 };
        std::vector<Effect> fx;
        std::vector<double> spellReady;  // per spec spell (foes) / per book entry (follower engine casts)
        float recentDmg = 0;             // decaying incoming damage per second (2 s window)
        // SIM V3 `burst` (native iaus/Fills.cpp Track, 11-17g): his GROSS damage per sampled lap. One sample every TM::pump (each follower's lap samples every
        // actor, Fills kLossMinDtSec 0.10 s, and a lap is served every 133 ms), the maths is the game's own Estimate::LapGross / BurstReads.
        std::vector<MFO::Iaus::Estimate::BurstSample> burst;   // newest last, pruned past the longer window
        float    burstHp = -1.0f;        // his health at the last sample (< 0 = never sampled)
        unsigned burstN = 0;             // hit events on him since the last sample (the sink's g_taken; a hit the party caused is not one, review F1 of b98052f0)
        float    burstFloor = 0.0f;      // the least those hits did: each attacker's own hit estimate after HIS armor / resistance estimate
        float dmgDealt = 0;
        int   arrows = 60, potions = 0;
        bool  rangedMode = false, casterMode = false;
        double airUntil = 0, groundUntil = 0;   // dragons
        float armorBuff = 0;
        bool  calmed = false;            // held by a Calm (not a paralysis): a blow from the party ends it when g_calmBreaks
        float poise = 0, poiseMax = 0;   // Chocolate Poise meter (0 max = the list has no poise mod)
        float dmgScale = 1;              // scenario strength scaling of a foe's damage (Setup.h targetRatio)
        std::string tag;                 // scenario role ("healer", "archer", "boss" ...)
        // stealth (g_stealth > 0 and a sneak class): his stance, the foes' group record of him and the per-pair levels
        bool  stealthy = false, sneaking = false;
        // The foe group's CombatTarget record of him (calculate-detection-re.md section 5). ONE foe group per fight (MODEL: an
        // encounter is one combat group), so one record per follower. level: the group's detectLevel as stored (a positive level
        // under the notice bar is stored as 0). sp: stealthPoints. lastKnown: lastKnownTimeStamp; (lx, ly): lastKnownLoc.
        struct Rec { int level = -1000; float sp = 100.0f; bool known = false, lost = false; double lastKnown = -1e9, lostAt = -1e9; float lx = 0, ly = 0; } rec;
        std::vector<std::pair<std::uint32_t, int>> pairLvl;   // foe id -> the last level the detection pass computed for (that foe, him): what the sneak attack test reads
        double movedAt = -1, cloakUntil = 0, cloakReady = 0, holdUntil = 0, standUntil = 0, noiseUntil = 0;
        float  noise = 0;               // his actionValue while now < noiseUntil: the sound of a swing, a shot, a cast (MODEL, Detect.h In::action)
        bool   ranLast = false;         // his last step was at the run speed (Detect.h In::running)
        double dndUntil = 0;            // --disengage-dnd: no foe may pick him until then while another valid target stands (MODEL of a variant that is not built)
        bool   corpseUsed = false;      // card G7: this corpse was raised once (a body is raised one time)
        float  hx = -1, hy = 0;         // where he faces (unit). A fighter faces his combat target; without one, the way he last moved
        // ── SIM V3 (iaus-sim-v3.md). All of it is inert while its switch is off ──
        bool   perchPath = false; double climbAt = -1e9;             // height: his ledge has a way up / a follower: the last step he was on that way (the climb MODEL, Brain)
        bool   airborne = false, perch = false;                      // height: on the wing (a dragon's circuit) / standing on a ledge (c.z holds the height)
        float  fcx = 0, fcy = 0, fr0 = 0, frFar = 0, fAlt = 0, fTh0 = 0, fDir = 1; double fT0 = 0, fT = 12;   // the circuit: centre, start radius, far radius, altitude, start bearing, turn sense, take-off time, air time
        double claimAt = 0, claimUntil = 0;                          // delivery: the cast's claim was made at / its charge begins at (the wait between is the claim-to-fire latency)
        bool   ctrlLive = false;                                     // delivery: his combat controller is active (the engine's detection has seen a foe)
        bool   lofWaited = false, castStarted = false;               // lof: this cast has been held at least once (one count per cast) / its 0x06 start test has passed
        double strafeUntil = 0; float sgx = 0, sgy = 0;              // stance: a held caster's side step goal
        struct Stream { bool live = false; int spell = -1; std::uint32_t target = 0; double end = 0, nextBeat = 0; bool iaus = false, beats = false; } stream;   // hands: a concentration stream on his left hand
        double reDrawUntil = 0;                                      // hands: his two-handed weapon / bow is being drawn again until then
        [[nodiscard]] bool Party() const { return role != Role::Foe; }
    };

    struct Occluder { float x, y, r; };
    // SIM V3: an ACTIVE area (ClickUp 1dfcuabd246). follows = it moves with its owner (a cloak); else it stays at the cast point.
    struct Area { float x = 0, y = 0, r = 0; double until = 0; std::uint32_t src = 0; bool follows = false, hostile = false; float perSec = 0, armor = 0; int av = -1; int spell = -1; };
    struct Spawn { double at = 0; std::shared_ptr<const ActorSpec> spec; float x = 0, y = 0; std::string tag; float dmgScale = 1; };   // a foe that joins later (waves)

    // Per-fight counters. `subject` = follower 0 unless stated.
    struct Tally {
        long ticks = 0, idle = 0, hold = 0, act = 0, fired = 0, switches = 0, badFacts = 0, invalid = 0, postureSwaps = 0;
        long hist[kFamilies] = {};
        double actSec[kFamilies] = {};          // seconds his decisions spent per family (attack: one serve interval per fire)
        double bodySec[10] = {}, bodyIausSec[10] = {}, upSec = 0;   // what his BODY did, by Act (the behaviour signature); Iaus = started by a decision
        long lowEpisodes = 0, lowCandEpisodes = 0, lowHealed = 0, lowCandTicks = 0, lowHoldTicks = 0; double lowHealSecSum = 0;
        long noCandTicks = 0, candTicks = 0, idleCand = 0, reachTicks = 0, idleReach = 0, taunts = 0, tauntRedundant = 0, tauntFail = 0;
        double dSum = 0, dMax = 0; long dN = 0;
        // absolute outcomes
        bool   won = false, lost = false, playerDied = false; double dur = 0, firstKill = -1; long foesFaced = 0;
        long   downs = 0, partyDowns = 0;
        double dmgDone = 0, dmgTaken = 0, partyDmgTaken = 0, playerDmgTaken = 0, healDone = 0;
        double mpSum = 0, hpSum = 0, spSum = 0; long resN = 0; double mpDrySec = 0;
        // intelligence counters
        double overkill = 0, iausDmg = 0;                 // damage beyond the target's remaining health, of IAUS-chosen hits
        double overheal = 0, healTotal = 0; long healCasts = 0, doubleHeals = 0;
        long   foeCasts = 0, foeCastsInterrupted = 0, interruptsByFollowers = 0;
        long   elemCasts = 0, resistedCasts = 0, weakCasts = 0, weakOpp = 0, immuneCasts = 0;
        long   splashAllyHits = 0, areaCasts = 0;
        long   ung2[6] = {};   // the unguarded ones by cause (World.h, the tally)
        long   inc2 = 0, guard2 = 0;   // Q-S-5: melee blows that LAND on the subject while two or more foes are taunted onto him, and how many met his guard
        long   ctrlCasts = 0, ctrlFails = 0, ctrlOnControlled = 0, hitsOnControlled = 0, powOnControlled = 0, powerAttacks = 0, powerWhiffs = 0, regroups = 0, grounds = 0, blocks = 0, bashes = 0;
        double playerPressureSec = 0, peelSum = 0; long peelN = 0, peelMiss = 0;
        long   tgtSwitches = 0; double engagedSec = 0;
        long   tauntsOutmatched = 0, tauntThenDown = 0, tauntSaves = 0;
        long   deathsWithHeal = 0;                        // a party member went down while the subject had an affordable heal for him
        long   killOrderGood = 0, killOrderBad = 0;       // priority foes (healer / caster / archer) killed before / after grunts
        double retreatSec = 0, kiteDmgTaken = 0;
        long   scrollsUsed = 0, extActions = 0, tableSkipped = 0; double moveOrderSec = 0;   // the extended action set (oracle / phase 2)
        // the class playstyle lab: stealth, position and rule tallies of the subject
        long   wpnHits = 0, sneakHits = 0, flankHits = 0, hides = 0, firstHits = 0, sneakOpens = 0, healAtN = 0;
        // card G3 and G2 in the lab: hide runs begun (hides) / ended hidden / failed, flee runs begun / ended healed past the line / on arrival / failed, runs he went down on,
        // blows the group record called "unaware" (stealthTry) and how many of those the engine's per-pair level refused the multiplier (stealthMiss)
        long   hideOk = 0, hideFail = 0, flees = 0, fleeHealed = 0, fleeArrived = 0, fleeFail = 0, runDowns = 0, stealthTry = 0, stealthMiss = 0;
        double runSec = 0, holdSec = 0, fleeHpGain = 0;
        double sneakDmg = 0, hiddenSec = 0, sneakSec = 0, tgtDistSum = 0, playerDistSum = 0, contactSec = 0, goInBadSec = 0, summonSec = 0, undeadDmg = 0, healAtSum = 0, cloakSec = 0;
        long   posN = 0, tgtDistN = 0, arrowsShot = 0, arrowsHit = 0;
        double drawnSec[4] = { 0, 0, 0, 0 }, nearOnMeSec[3] = { 0, 0, 0 }, swapSec = 0; long drawSpoiled = 0, pointBlankShots = 0;   // --posture-report: bow / melee / other drawn; a foe ON HIM within 256 / 540 / beyond (time); draws a hit spoiled; shots under 256
        long   wastedCasts = 0, hitsWhileCtl = 0, escaped = 0, wwDeclined = 0;   // wwDeclined: a walk try spent on a cast Fire refuses (will not work)
        double playerMinHp = 1, mpAtLate = -1;     // the player's lowest health fraction; the subject's magicka fraction when a late foe arrived                           // released at a dead / out-of-sight target
        // T2 batch S tallies of the subject (section 31)
        long   dodgeTries = 0, dodges = 0, aimCorrected = 0, blockRenewals = 0, powRefused = 0, backOffs = 0, backOffArrived = 0, steps = 0, stepArrived = 0, stepRevealed = 0;
        long   averted = 0, avertKept = 0, raises = 0, mixPowerDenied = 0, mixNormalDenied = 0, mixFloorLifted = 0, leashTurns = 0, engPower = 0, dndRuns = 0;
        double guardSec = 0, feetSec = 0;
        // card G3's usefulness (section 31.9): per run of the subject, the damage he took on it and how long until no foe stood within 400 units of him
        long   runsBegun = 0, runsSafe = 0; double runDmg = 0, runSafeSec = 0, runLenSec = 0, runDmgAfter5 = 0;
        // SIM BATCH (section 32): THE SUBJECT'S HEALTH-DECLINE RECORD. Band k = his health in [k/10, (k+1)/10) of his maximum.
        //   decBandSec / decBandLoss: seconds he spent in the band while a foe had him as its target, and the health (as a share of his
        //     maximum) he lost there: loss / sec = how fast he falls at that health = the decline curve.
        //   decLethal: of those seconds, the ones in which the damage of his last ~2 s (Body::recentDmg) was at least the health he had
        //     left: at that pace he is down inside the time a potion needs (the serve lap + the 0.6 s to the gulp).
        //   decCross[k] / decDownAfter[k] / decToDownSec[k]: fights in which he first fell under line k/10, in how many of those he then
        //     went down, and the summed seconds from the crossing to that first down.
        //   drinks: health potions begun, his health share when he began, the share of the potion that ran over his maximum, his downs
        //     with a health potion still in his kit, and his downs between the start of a gulp and its effect. potFrac: potion / max health.
        double decBandSec[10] = {}, decBandLoss[10] = {}, decLethal[10] = {}, decToDownSec[10] = {};
        long   decCross[10] = {}, decDownAfter[10] = {};
        long   drinksH = 0, downsWithPotH = 0, downsMidDrink = 0, potFracN = 0; double drinkAtSum = 0, potHeal = 0, potWaste = 0, potFracSum = 0;
        // ── SIM V3 tallies of the subject (iaus-sim-v3.md section 4, 5, 6) ──
        // DELIVERY: casts his decisions fired (castPick) and what became of each: released (castFired), released at a target he could not
        // see (castNoLos, a subset of fired), held by the line of fire at least once (castLofHeld) and for how long (castLofSec), given up
        // after the hold cap (castLofExpired), claimed on an inactive controller and never cast (castNotBuilt), sent down the direct road
        // (castDirect), replaced by a newer pick while its claim waited (castThrash), ended by a stagger / his fall / the target's death (castLost).
        long   castPick = 0, castFired = 0, castNoLos = 0, castLofHeld = 0, castLofExpired = 0, castNotBuilt = 0, castDirect = 0, castThrash = 0, castLost = 0;
        // sim mirror batch 11-17: claims that closed with no fire (the measured unfired share, Data.h Delivery) and the seconds they stood, self buffs /
        // heals deferred for want of a combat controller (cast/CastOn.cpp, laps), heal claims released because the recipient was full before the charge began.
        long   castUnfired = 0, castDeferred = 0, healFullExit = 0, castRepeat = 0; double unfiredSec = 0;   // castRepeat: picks that drew the repeat table (the claim stood and had fired)
        // THE SELF-HEAL POINT (the caster survival band, Playstyles.h): his health share each time a decision of his began to mend HIMSELF (a heal cast at himself, a health potion).
        long   selfHealLowN = 0;   // ... and how many of those began under a quarter of his health (the oracle-relative survival band)
        long   healLateN = 0;      // his heals on ANOTHER that began with the recipient within one hard hit of down (Policy.h kOneHitLine): the heal-timing band of the healers (healLate%)
        long   selfHealN = 0; double selfHealAtSum = 0; long healYield = 0;   // healYield: his channelled heal gave its hand to a non-heal pick under threat (11-17b)
        // THE FIELD SCENARIOS' READS (11-17): his first damage on a foe (per fight: N is 0 or 1) and what his cast decisions picked: offense (the spell's own
        // damage and cost) and heals (the heal's size, how many were channelled, how many were for himself).
        long   firstHitN = 0, offPickN = 0, healPickN = 0, healPickConc = 0, healPickSelf = 0; double firstHitSum = 0, offPickDmg = 0, offPickCost = 0, healPickSize = 0;
        long   beats = 0, beatLof = 0, beatNoLos = 0, streams = 0; double castLofSec = 0, claimSec = 0, streamSec = 0;
        // CHASE CAP: his decision laps with a live foe (capLaps), laps on which the cap removed at least one live foe (capDropLaps), laps
        // on which it removed EVERY live foe (capStarveLaps), foes kept by the waiver (capWaived), the cap's sum (capSum / capLaps = mean).
        long   capLaps = 0, capDropLaps = 0, capStarveLaps = 0, capWaived = 0, capKept = 0; double capSum = 0;   // capKept: foes kept past the cap as noApproach (11-17 A3)
        // GUARD BY DIRECTION: melee blows by a foe that land on him (inc), met by his guard inside its arc (incGuard), guard up but the
        // blow came from outside the arc (incArc), no guard up when it landed (incDown).
        long   inc = 0, incGuard = 0, incArc = 0, incDown = 0;
        // HANDS: seconds his two-handed weapon / bow was denied by a cast, a stream or the re-draw, and the weapon damage that time was worth at his rate.
        double handLostSec = 0, handLostDmg = 0; long reDraws = 0;
        // AREAS: seconds an area of his was up, foe-seconds inside his hostile area, ally-seconds inside it, seconds he stood outside his own protective one, casts of an area spell while one of his was still up.
        double areaUpSec = 0, areaFoeSec = 0, areaAllySec = 0, areaOutSec = 0; long areaRecast = 0, areaOwnCasts = 0;
        double minTgtDist = 1e9; long strafes = 0;   // the nearest he came to his target (the last fight's value is kept by Agg as a sum)
        std::vector<std::string> killOrder;
    };

    struct GRow {                 // one default gambit row, in board terms
        std::string cond, act;
        float param = 0;
        int   role = 0;           // MFO::Classes::SpellRole ordinal (cast rows)
        float cooldown = 0;
        bool  applied = true;     // false: a T1 row kept as data only (the T1 classes keep today's kit)
    };
    // SpellRole ordinals (native/classes/Classes.h, frozen order).
    enum : int { kRoleNone = 0, kRoleAttack, kRoleDestruction, kRoleFire, kRoleFrost, kRoleShock, kRoleArea, kRoleHeal, kRoleSummon, kRoleBoundWeapon, kRoleWard, kRoleArmor,
                 kRoleParalyze, kRoleFear, kRoleCalm, kRoleReanimate, kRoleTurnUndead };

    // The worth in J of the k-th potion still in the bag at the end of a fight, per kind (0 health, 1 magicka, 2 stamina), k = 1..kPotWorthMax
    // (a bag deeper than that prices the extra ones as the last). Filled from PotionWorth.h or --potion-worth <file> (Setup.h).
    inline constexpr int kPotWorthMax = 4;
    struct PotWorth { std::array<std::array<float, kPotWorthMax>, 3> m{}; };

    struct Ctl {   // one follower with a decision layer (IAUS, or his gambit table)
        mutable std::unordered_map<std::uint32_t, double> occSince;   // foe id -> when its occlusion began (the keep-current-attack hold)
        int   body = -1;
        Cls   fam = Cls::Melee;
        ClassGate gate;
        Profile profile;
        std::shared_ptr<const FollowerBuild> build;
        Book  book;                        // live copy (costs, staff charge, activeOn, liveSummon)
        std::vector<std::vector<std::pair<std::uint32_t, double>>> active;   // per entry: (recipient, until)
        std::vector<double> summonUntil;
        Mind  mind;
        MFO::Iaus::ReachBook::Book reach;     // the learned EDGE reach of his melee weapon (iaus/ReachBook.h: the weapon's learner + the sequence learners; no light / power key)
        // THE CLASS PACKAGE (card C8): the combat style the DLL swaps on per stance (0 melee, 1 ranged, 2 cast), the ch.18
        // power-attack share (a scale on the engine AI's own power attacks) and the ch.23 pursuit leash (units from the player, 0 = none).
        Style pkg[3]; float powerShare = 1.0f, leash = 0.0f;
        // T2 batch S (section 31). The ch.18 rule of his class (-1 = none declared) and the window of his last 8 STARTED attacks
        // (apmf/AttackMix.cpp: Power / (Power + Normal)); the ch.32 aversion weight; the ch.29 miss budget (-1 = none) and the
        // outcome ring of his last 10 shots; whether a class package (tier 2+) is in force (the class CSTY applies in full, Q-melee-6).
        float  shareMax = -1.0f, shareMin = -1.0f; std::uint8_t atkWin[8] = {}; int atkN = 0, atkAt = 0;
        float  aversion = 0.0f; int aimBudget = -1; std::uint8_t aimWin[10] = {}; int aimN = 0, aimAt = 0, aimFlying = 0;
        bool   pkgClass = false;
        std::uint32_t handSpell = 0;               // H4: the spell in his hand = the last book spell he cast (MODEL; Snapshot::equippedSpell)
        std::uint32_t chosenForm = 0;              // Q-I-2: the cast ranked first on his last decision (Snapshot::chosenCast)
        std::uint32_t handSpell2 = 0;              // Q-I-2: the other hand = the spell cast before it, when it was a different one (MODEL; Snapshot::equippedSpellOther)
        int    blockRenewals = 0; double blockRestUntil = 0;   // apmf/BlockClaims.cpp: one renewal, then kRest 1.5 s after any hold ends
        double runSafeAt = -1; bool runMeasured = false;       // section 31.9: this run's time to safety
        std::vector<GRow> rows;            // table mode: his class's default gambit rows
        PostureKind drawn = PostureKind::None;
        std::map<std::uint64_t, double> firedAt;   // the Cooldowns store (Kind::IausAction): CooldownId -> last fired
        std::vector<SpellTarget> wants;            // G10: the pairs the last decision ranked on top
        std::vector<SpellTarget> asked;            // every pair ever asked (--g10-sticky: the answers are remembered)
        std::vector<SpellTarget> wwMirror;         // cast/WillWork's mirror: pairs with a measured verdict (the first ask or Fire is the prefetch)
        std::uint32_t steer = 0;                   // act.attack's target for the engine AI
        std::uint8_t rank = 3; std::uint64_t seed = 0; double window = 1.5;
        float potH = 100, potM = 100, potS = 100, potDur[3] = { 0, 0, 0 };   // potDur > 0: pot* per second for that long (FollowerBuild::potDur)
        // --consumables (tune 2.11.1): the gold value of every buff potion and food he used up in this fight (Utility charges it at J per gold), and
        // the uses per kind (the logistics world's use counts). Per buff kind: the potion's magnitude and duration (the kit fill, Setup.h).
        double consumedGold = 0; std::array<long, MFO::Iaus::kBuffKinds> buffUses{}; std::array<long, 3> foodUses{};
        std::array<float, MFO::Iaus::kBuffKinds> buffMag{}, buffDur{};
        double lowSince = -1; bool lowHealed = false, lowHadCand = false;
        double pressureSince = -1;                 // the player came under attack at (peel latency)
        double commitUntil = 0;                    // oracle rollouts: he keeps the option he was given until then
        float  goalX = 0, goalY = 0; double moveUntil = 0; bool sprint = false, moveFight = false;   // a deliberate reposition order (the extended action set)
        int    potions0 = 0;                       // potions at the start (Utility: resources left)
        int    pot0[3] = { 0, 0, 0 };              // the same by kind: health, magicka, stamina (Utility: what a drunk potion was worth, Oracle.h PotionCost)
        const PotWorth* worth = nullptr;           // his class cell's row of the potion-worth table (nullptr = no charge)
        Key    lastKey{}; bool hasLastKey = false; // oracle arm: the action he is on (switch counting)
        std::uint32_t lastTarget = 0;
        // class traits the world applies (the class's own design data: SimClass, RealClasses.cpp)
        unsigned forbid = 0;   // the class's oracle constraint bits (Playstyles.h kPs*)
        float sneakAtkClass = 1.0f, noiseClass = 1.0f; bool flank = false, cloak = false, sneakClass = false, nearPlayer = false; int entryOdds = 0, goIn = 0, partyPct = 0;
        float armorWeight = 0;   // the weight of what he wears (MODEL: 9 per heavy piece, 3 per light one); his weapon's weight is added per stance
        // CARD G3 (native/Concealment.cpp, mirrored): the pick and the run. phase: 0 none, 1 hide run (on the way), 2 hiding at the
        // point, 3 flee run. stanceHeld = the hide ended HIDDEN and its stance claim stands (g_stances): no new pick until a blow or a reveal.
        struct Pick { bool valid = false, flee = false; float x = 0, y = 0; double readyAt = -1e9, askedAt = -1e9; int kind = 0; std::uint32_t target = 0; };   // kind (batch I, Concealment::Kind): 0 hide, 1 flee, 2 back-off, 3 step. ONE slot per follower (MFO-B830)
        mutable Pick pick;
        int    runPhase = 0 /* 4 back-off, 5 hidden step (batch I) */, hiddenReads = 0; bool stanceHeld = false, runFromOracle = false;
        double runStart = 0, runArrive = 0, runServedAt = -1; float runX = 0, runY = 0, runHp = 0, fleeLine = 0.30f; bool runServedHold = false;
        // SIM V3. The HP trend ring of Confidence.h (NoteHealth: one sample per own service, 0.25 s minimum step, 32 kept) and the
        // posture latch of Candidates::Build (last decision's Generated::spellsHold).
        mutable std::array<std::pair<double, float>, 32> trend{}; mutable std::size_t trendHead = 0, trendCount = 0;
        bool   spellsHold = false;
        // sim mirror batch 11-17. His last RELEASED decision cast (the claim that stands: a re-pick of it draws the repeat table, Data.h Delivery),
        // and whether the foe his act.attack names was kept past the chase cap without a waiver (Combatant::noApproach: he shoots, he does not walk to it).
        std::uint32_t lastFireForm = 0, lastFireTarget = 0; double lastFireAt = -1e9;
        bool   steerNoApproach = false;
        // fix/field-1117b-choice (iaus/Candidates.cpp g_fireRefused, cast/Hands.cpp g_healerThreat): the (spell, foe) pairs his generator refused for a party
        // member in the line of fire, with when (fed back as will-not-work notes for Tuning::fireLineKeepSec), and whether a foe is hurting him now.
        struct FireRefusal { std::uint32_t spell = 0, foe = 0; double at = 0; }; std::vector<FireRefusal> fireRefused;
        bool   healerThreat = false;
        // Harbinger's Standing combat ENTRY foe (APMFBridge::CombatEntryStateOf, the second id of Snapshot.h ChaseWaived). Only a fight that models ch.21
        // EngageOnSight has one (Fight::entryRoad, OFF by default as the game ships it). MODEL: the foe his
        // combat was entered on = his engine target at his first decision; the entry is over when that foe falls or his engine target moves on.
        mutable std::uint32_t entryFoe = 0; mutable bool entryEnded = false;
    };

    struct Policy;   // decides for one follower when he is served (Policy.h)
    struct Fight; struct Body;
    inline void FillEstimate(const Fight& F, const Body& o, Combatant& k);   // Policy.h: the estimate inputs of one actor, as the game's gather reads them
    inline constexpr double kBurstKeepSec = 30.0;   // Body::burst keeps its samples this long: past the longer of Tuning::grossWindowSec / maxHitWindowSec at any searched value (4 and 8 s shipped)

    struct Fight {
        const Data* D = nullptr;
        Rng R;
        std::vector<Body> b;
        std::vector<Ctl> ctl;
        std::vector<Occluder> occ;
        std::vector<Shot> shots;
        std::vector<Area> areas;                   // SIM V3: the live areas
        Rng V;                                     // SIM V3: the draws v3 adds (flight path, stream length), never R
        // PER-ACTOR RANDOM STREAMS, ROLLOUT ONLY (marth 11-17: "128 + fix the noise"; iaus-sim-v3.md 14.2). A real fight draws from R and V in
        // world order, as always (split = false: RU / RP / VU / VP are R.U / R.P / V.U / V.P, so no fight and no baseline moves). Inside an
        // oracle rollout (Oracle.h sets split and splitSeed on the copy) every draw comes from a stream of its own per (actor, draw site): the
        // n-th draw of that actor at that site is the same number in every option's rollout of one stream index. So what an option does not
        // touch plays out the same (a far foe's target pick, his swing timing, another follower's hit roll), and two options are compared on
        // their own consequences instead of on a reshuffled fight. The actor is the body the draw belongs to (the one who dodges, blocks,
        // thinks, aims); the site is the call's own number (1..kDrawSites, given once below and never reused).
        bool split = false; std::uint64_t splitSeed = 0; std::vector<std::uint64_t> xs;   // xs: the streams' states, [body index x kDrawSites + site], 0 = not started
        static constexpr int kDrawSites = 37;
        [[nodiscard]] float Unit(const Body& o, int site) {   // [0, 1) from the stream of (o, site); a body not yet in the list (Add) takes the next index
            const std::size_t bi = (&o >= b.data() && &o < b.data() + b.size()) ? static_cast<std::size_t>(&o - b.data()) : b.size(), key = bi * static_cast<std::size_t>(kDrawSites + 1) + static_cast<std::size_t>(site);
            if (xs.size() <= key) xs.resize((bi + 4) * static_cast<std::size_t>(kDrawSites + 1), 0);
            std::uint64_t& st = xs[key]; if (!st) st = Mix64(splitSeed ^ Mix64(0x51ED270B0F4C1E5DULL + key)) | 1ULL;
            st += 0x9E3779B97F4A7C15ULL; return static_cast<float>(Mix64(st) >> 40) * (1.0f / 16777216.0f);
        }
        [[nodiscard]] float RU(const Body& o, int site, float lo, float hi) { return split ? lo + (hi - lo) * Unit(o, site) : R.U(lo, hi); }
        [[nodiscard]] bool  RP(const Body& o, int site, float p) { return split ? Unit(o, site) < p : R.P(p); }
        [[nodiscard]] float VU(const Body& o, int site, float lo, float hi) { return split ? lo + (hi - lo) * Unit(o, site) : V.U(lo, hi); }
        [[nodiscard]] bool  VP(const Body& o, int site, float p) { return split ? Unit(o, site) < p : V.P(p); }
        std::vector<Spawn> late;
        float foeHpTotal = 0;                      // every foe's full health, late ones included (Utility)
        double now = 0, nextPump = 0, maxSec = 150;
        double burstAt = 0;                        // SIM V3 `burst`: the next gross-damage sample of every actor (Body::burst)
        double decCrossAt[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }; bool decDowned = false;   // the decline record's per-fight state (Tally::decCross)
        int rr = 0;
        bool tauntVisible = true, engineCasts = true, g10Sticky = false, over = false;
        bool entryRoad = false;                    // ch.21 EngageOnSight is modelled in this fight (ships OFF: Data.h g_engageOnSight, FightSpec::engageOnSight)
        bool scrollRoad = false;                   // card S3 prototype: scrolls can be read (Snapshot::scrollRoad)
        float arenaX = 0, arenaY = 0, arenaR = 0; bool arenaSet = false;   // --space: the enclosed fight's bounds (0 = open)
        float light = 0.6f; bool interior = false; double nextDetect = 0; Detect::Gmst dg;   // stealth: the cell's light on him, 0 dark .. 1 daylight (MODEL)
        std::uint64_t avSeed = 0;                  // ch.32: the per (foe, follower, 6 s window) draws come from a hash of this, never from R
        bool estimates = true;                     // fill the Snapshot's estimate inputs (iaus/Estimate.h) as the game's gather will   // card I6: the game fills Combatant::taunted
        Tally t;
        std::uint32_t nextId = 1;
        std::FILE* trace = nullptr;   // --trace: one line per event (never set in a rollout copy)
        double lastTauntBy0 = -1e9;   // the subject's last landed taunt (taunt-then-down tally)

        // ── lookup ──
        [[nodiscard]] Body* Find(std::uint32_t id) { for (auto& x : b) if (x.c.id == id) return &x; return nullptr; }
        [[nodiscard]] const Body* Find(std::uint32_t id) const { for (auto& x : b) if (x.c.id == id) return &x; return nullptr; }
        [[nodiscard]] static bool Up(const Body& x) { return x.alive && !x.c.downed; }
        [[nodiscard]] Body& Player() { return b[0]; }
        // SIM V3 (height): 3D between the two origins whenever either stands off the ground (the game's GetPosition().GetDistance,
        // iaus/Candidates.cpp:344). With every z at 0 (v2, or a fight with nobody in the air) it is the 2D value of v2, bit for bit.
        [[nodiscard]] static float Dist(const Body& a, const Body& o) { return a.c.z == 0.0f && o.c.z == 0.0f ? std::hypot(a.c.x - o.c.x, a.c.y - o.c.y) : std::hypot(a.c.x - o.c.x, a.c.y - o.c.y, a.c.z - o.c.z); }
        [[nodiscard]] bool Sees(const Body& a, const Body& o) const {
            for (const Occluder& oc : occ) {   // segment / circle
                const float dx = o.c.x - a.c.x, dy = o.c.y - a.c.y, l2 = dx * dx + dy * dy; if (l2 < 1) continue;
                const float tt = std::clamp(((oc.x - a.c.x) * dx + (oc.y - a.c.y) * dy) / l2, 0.0f, 1.0f);
                if (std::hypot(a.c.x + tt * dx - oc.x, a.c.y + tt * dy - oc.y) < oc.r) return false;
            }
            return true;
        }
        [[nodiscard]] const Weapon& MeleeOf(const Body& x) const { return x.spec->melee; }
        [[nodiscard]] float Reach(const Body& x) const {   // hit reach in units: fCombatDistance x weapon reach, plus both bodies' radii (MODEL 70)
            const float r = x.spec->melee.Has() ? D->rules.combatDistance * x.spec->melee.reach : std::max(96.0f, x.spec->unarmedReach);
            return r + 70.0f + (x.spec->mass > 2.0f ? 60.0f : 0.0f);
        }
        // The combat style in force: a follower's is his class package for the stance his drawn weapon gives (the DLL swaps
        // it with the equip gambit); everyone else's is his record's.
        [[nodiscard]] const Style& StyleOf(const Body& x) const {
            if (x.ctl < 0) return x.spec->st;
            const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
            int stance = (c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver) ? 0 : c.drawn == PostureKind::Bow ? 1 : (c.gate.spellsOnClass || c.drawn == PostureKind::Staff) ? 2 : 0;
            // SIM V3 (stance): hands on spells and nothing he can pay for = the cast latch reads DRY and the stance is MELEE (CombatScan.cpp:1512-1576)
            if (g_v3.stance && stance == 2 && c.drawn == PostureKind::None && !CanAffordOffense(c, x) && !x.stream.live && x.act != Act::Cast) stance = 0;
            return c.pkg[stance];
        }
        [[nodiscard]] bool Busy(const Body& x) const { return now < x.actEnd || now < x.staggerUntil || now < x.ctrlUntil || now < x.downUntil || (x.stream.live && StreamHolds(x)) || now < x.reDrawUntil; }
        [[nodiscard]] bool Controlled(const Body& x) const { return now < x.ctrlUntil || now < x.fleeUntil; }
        [[nodiscard]] float ArmorOf(const Body& x) const { return x.spec->ar + x.armorBuff; }
        // --consumables: a fortify-skill / fortify-attack potion raises his weapon damage by its percent while it lasts (1 = none, exactly).
        [[nodiscard]] static float FortifyDmg(const Body& x) { float p = 0.0f; for (const Effect& e : x.fx) if (e.kind == 7) p += e.v; return p > 0.0f ? 1.0f + p / 100.0f : 1.0f; }
        [[nodiscard]] float ResistOf(const Body& x, int av) const {
            float r = x.spec->Resist(av);
            for (const Effect& e : x.fx) if (e.kind == 3 && e.av == av && e.until > now) r += e.v;
            return x.role == Role::Player ? std::clamp(r, -100.0f, D->rules.maxResist) : std::clamp(r, -100.0f, 100.0f);   // I8 research 2.3: only a PLAYER target is capped (fPlayerMaxResistance); 100 = immune
        }

        // ── stealth: the ENGINE's detection per (foe, follower) pair and the foe group's record of him (Detect.h) ──
        static void Face(Body& x, float tx, float ty) { const float dx = tx - x.c.x, dy = ty - x.c.y, l = std::hypot(dx, dy); if (l > 1.0f) { x.hx = dx / l; x.hy = dy / l; } }
        [[nodiscard]] static float AngleDeg(const Body& o, const Body& x) {
            const float bx = x.c.x - o.c.x, by = x.c.y - o.c.y, lb = std::hypot(bx, by); if (lb < 1.0f) return 0.0f;
            return std::acos(std::clamp((o.hx * bx + o.hy * by) / lb, -1.0f, 1.0f)) * 57.29578f;
        }
        [[nodiscard]] float GearWeight(const Body& x) const {   // MODEL: what he wears + the weapon in his hands
            if (x.ctl < 0) return 0.0f; const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
            return c.armorWeight + (c.drawn == PostureKind::Bow ? x.spec->ranged.weight : x.spec->melee.Has() ? x.spec->melee.weight : 0.0f);
        }
        // The engine's level of (observer o, target x) right now: every input of calculate-detection-re.md section 4 the lab has.
        [[nodiscard]] Detect::Out Level(const Body& o, const Body& x) const {
            Detect::In in; in.dist = Dist(o, x); in.angleDeg = AngleDeg(o, x); in.losGeom = Sees(o, x); in.exterior = !interior; in.flying = o.c.outOfMeleeReach || x.c.outOfMeleeReach;   // STAND-IN (MFO-B788): 'out of melee reach' is read as flying, which triples the range against an actor on a ledge too. It errs against the follower
            const Body* ot = Find(o.c.targetId); in.inCombatO = true; in.oHasTarget = ot && Up(*ot); in.targetIsOsTarget = ot == &x;
            const Ctl* c = x.ctl >= 0 ? &ctl[static_cast<std::size_t>(x.ctl)] : nullptr;
            in.skillO = o.spec->Skill(15); in.skillT = x.spec->Skill(15) + StealthCombine(D->sneakSkillRows, *x.spec, false);   // perk entry 57 (the list's rows add)
            in.sneakingT = x.sneaking; in.moving = now - x.movedAt < 0.2; in.running = in.moving && x.ranLast;
            in.noiseMult = c && x.sneaking ? c->noiseClass : 1.0f;                                   // the class passive (W Thief: sneaking 30% quieter)
            in.moveMod = StealthCombine(D->sneakNoiseRows, *x.spec, true, true);                     // perk entry 48
            in.weight = GearWeight(x); in.action = now < x.noiseUntil ? x.noise : 0.0f;
            in.light = g_lightScale * light;   // MODEL: the cell's light on him as the cached light level, 0 dark .. g_lightScale in daylight, after the interior / exterior mod
            return Detect::Calculate(dg, in);
        }
        [[nodiscard]] int PairLevel(const Body& o, const Body& x) const { for (const auto& k : x.pairLvl) if (k.first == o.c.id) return k.second; return -1000; }
        // Does the foe group still hold him as a target (known and not lost)? This is what the engine's target selector admits.
        [[nodiscard]] bool GroupHas(const Body& x) const { if (!x.stealthy) return true; if (now < x.cloakUntil) return false; return x.rec.known && !x.rec.lost; }
        // MFO's read of the same record (native/iaus/Hide.cpp Verdict): Yes only for a record that is not lost and holds a positive level.
        [[nodiscard]] bool GroupDetects(const Body& x) const { if (!x.stealthy) return true; if (now < x.cloakUntil) return false; return !x.rec.lost && x.rec.level > 0; }
        [[nodiscard]] bool Unseen(const Body& x) const { return x.stealthy && x.sneaking && !GroupDetects(x); }
        // The engine's ForceDetect (AE 37765; a hit is one of its 9 callers): the victim's side now has him at level 100. The next
        // detection pass scores the pair again by the formula, so a blow from the dark at range can be lost again at once.
        void ForceDetect(Body& x, const Body& o) {
            if (!x.stealthy) return;
            x.rec.level = 100; x.rec.sp = 0.0f; x.rec.known = true; x.rec.lost = false; x.rec.lastKnown = now; x.rec.lx = x.c.x; x.rec.ly = x.c.y;
            bool f = false; for (auto& k : x.pairLvl) if (k.first == o.c.id) { k.second = 100; f = true; } if (!f) x.pairLvl.push_back({ o.c.id, 100 });
        }
        void Action(Body& x, float level) { if (x.stealthy) { x.noise = level; x.noiseUntil = now + static_cast<double>(dg.actionTimer); } }   // MODEL: how loud and how long (the card: UNREAD)
        // One detection pass (MODEL cadence: fDetectionUpdateTimeMin, the job's own cadence is UNREAD): every (foe, sneak follower) pair
        // through the formula, then the group's record by the card's section 5: the notice bar, the timed loss, the immediate loss
        // when a foe looks and fails, the search loss near the last known point.
        void DetectPass() {
            for (Body& x : b) { if (!x.stealthy || !Up(x)) continue;
                Ctl& c = ctl[static_cast<std::size_t>(x.ctl)]; Body::Rec& r = x.rec;
                if (now < x.cloakUntil) { x.pairLvl.clear(); continue; }   // card C13 / H10: nobody has him until his first blow / for the cloak's 3 s
                int maxLvl = -100000; bool coneFail = false, nearLast = false, any = false; x.pairLvl.clear();
                for (const Body& o : b) { if (o.role != Role::Foe || !Up(o)) continue;
                    const Detect::Out d = Level(o, x); x.pairLvl.push_back({ o.c.id, d.level }); any = true; maxLvl = std::max(maxLvl, d.level);
                    if (d.level < 0 && d.inViewCone) coneFail = true;
                    if (d.level < 0 && !d.inViewCone && r.known && std::hypot(o.c.x - r.lx, o.c.y - r.ly) < dg.lostNoticedDist) nearLast = true; }
                if (!any) continue;
                if (r.lost && r.sp <= 0.0f && now - r.lostAt >= static_cast<double>(dg.spRegenLostWait)) r.sp = dg.spMax;   // MODEL (the refill is UNREAD, card R4): full again fCombatStealthPointRegenLostWaitTime after the loss
                const float bar = x.sneaking ? dg.barSneak : dg.bar;
                if (maxLvl > 0 && (r.sp <= 0.0f || static_cast<float>(maxLvl) > bar)) { r.level = maxLvl; r.sp = 0.0f; r.known = true; r.lost = false; r.lastKnown = now; r.lx = x.c.x; r.ly = x.c.y; }
                else { r.level = maxLvl > 0 ? 0 : maxLvl;
                    if (r.known && !r.lost && ((maxLvl < 0 && (coneFail || nearLast)) || now - r.lastKnown > static_cast<double>(dg.lostTime))) { r.lost = true; r.lostAt = now; } }
                if (trace && std::getenv("SIM_DETECT")) { std::fprintf(trace, "%7.2f  DETECT %s: max level %d  record level %d sp %.0f known %d lost %d  sneaking %d  |", now, x.c.name.c_str(), maxLvl, r.level, r.sp, int(r.known), int(r.lost), int(x.sneaking));
                    for (const Body& o : b) if (o.role == Role::Foe && Up(o)) { const Detect::Out d = Level(o, x); std::fprintf(trace, " %u: d %.0f ang %.0f los %d cone %d sound %.1f light %.1f skill %.1f = %.1f", o.c.id, Dist(o, x), AngleDeg(o, x), int(d.los), int(d.inViewCone), d.sound, d.light, d.skill, d.score); }
                    std::fprintf(trace, "\n"); }
                if (c.cloak && g_stealth >= 2 && now > 4.0 && now >= x.cloakReady && GroupDetects(x)) { x.cloakUntil = now + 3.0; x.cloakReady = now + 30.0; r.lost = true; r.lostAt = now; r.level = -1; x.pairLvl.clear(); }   // H10: once per 30 s, invisible for 3 s (an invisible target is lost at once even when known)
            }
        }
        // ── card G3's concealment pick (native/Concealment.cpp PickStep, mirrored): 12 bearings on rings of 320 and 600 units, each
        // a straight walk from him that stops at the first solid thing; never beside a foe, never a walk shorter than 160, inside the
        // leash. HIDE: every listed foe's line to the point is cut by geometry, the nearest such point wins. FLEE: at least 150 units
        // farther from the nearest foe than he is now, the farthest from the foes wins.
        [[nodiscard]] bool PointHidden(float px, float py, const Body& o) const {
            for (const Occluder& oc : occ) { const float dx = px - o.c.x, dy = py - o.c.y, l2 = dx * dx + dy * dy; if (l2 < 1) continue;
                const float tt = std::clamp(((oc.x - o.c.x) * dx + (oc.y - o.c.y) * dy) / l2, 0.0f, 1.0f); if (std::hypot(o.c.x + tt * dx - oc.x, o.c.y + tt * dy - oc.y) < oc.r) return true; }
            return false;
        }
        [[nodiscard]] bool Standable(float px, float py) const { for (const Occluder& oc : occ) if (std::hypot(px - oc.x, py - oc.y) < oc.r + 40.0f) return false; return true; }
        [[nodiscard]] bool ConcealPick(const Body& x, const Ctl& c, bool flee, float& gx, float& gy) const {
            std::vector<const Body*> foes; float nearestNow = 1e9f;
            for (const Body& o : b) if (o.role == Role::Foe && Up(o) && foes.size() < 6) { foes.push_back(&o); nearestNow = std::min(nearestNow, Dist(o, x)); }
            if (foes.empty()) return false;
            bool have = false; float bestCost = 0, bestFoe = 0;
            for (int i = 0; i < 24; ++i) {
                const float ang = static_cast<float>(i % 12) * (6.2831853f / 12.0f), want = i < 12 ? 320.0f : 600.0f, ux = std::cos(ang), uy = std::sin(ang);
                float cost = 0; while (cost + 20.0f <= want && Standable(x.c.x + ux * (cost + 20.0f), x.c.y + uy * (cost + 20.0f))) cost += 20.0f;   // FindEmptySpace: the walk stops at the wall
                if (cost < 160.0f) continue;
                const float px = x.c.x + ux * cost, py = x.c.y + uy * cost;
                if (c.leash > 0.0f && std::hypot(px - b[0].c.x, py - b[0].c.y) > c.leash - 200.0f) continue;
                float nearest = 1e9f; bool seen = false;
                for (const Body* o : foes) { nearest = std::min(nearest, std::hypot(o->c.x - px, o->c.y - py)); if (!flee && !PointHidden(px, py, *o)) seen = true; }
                if (nearest < 250.0f) continue;
                if (flee) { if (nearest < nearestNow + 150.0f) continue; if (!have || nearest > bestFoe) { have = true; gx = px; gy = py; bestFoe = nearest; } }
                else { if (seen) continue; if (!have || cost < bestCost) { have = true; gx = px; gy = py; bestCost = cost; } }
            }
            return have;
        }
        // The HIDDEN STEP's pick (native/Concealment.cpp PickStep, Kind::Step, mirrored; gap G18): 10 candidates toward the foe the step is
        // for, at 0, +-25 and +-50 degrees off the line to him on rings of 200 and 420 units; each a straight walk that stops at the first
        // solid thing. Refused: a walk under 160 units, a point outside the leash margin, a point that is not at least 100 units nearer
        // the foe, a point within 250 units of ANOTHER foe. A bow takes the shortest walk whose point that foe's eye can reach (a line
        // of fire); a blade takes the point nearest the foe.
        [[nodiscard]] bool StepPick(const Body& x, const Ctl& c, std::uint32_t target, bool bow, float& gx, float& gy) const {
            const Body* tf = Find(target); if (!tf || !Up(*tf)) return false;
            const float targetNow = Dist(x, *tf), base = std::atan2(tf->c.y - x.c.y, tf->c.x - x.c.x);
            static constexpr float kOff[5] = { 0.0f, 0.4363f, -0.4363f, 0.8727f, -0.8727f };
            bool have = false; float bestCost = 0, bestFoe = 0;
            for (int i = 0; i < 10; ++i) {
                const float ang = base + kOff[i % 5], want = i < 5 ? 200.0f : 420.0f, ux = std::cos(ang), uy = std::sin(ang);
                float cost = 0; while (cost + 20.0f <= want && Standable(x.c.x + ux * (cost + 20.0f), x.c.y + uy * (cost + 20.0f))) cost += 20.0f;
                if (cost < 160.0f) continue;
                const float px = x.c.x + ux * cost, py = x.c.y + uy * cost;
                if (c.leash > 0.0f && std::hypot(px - b[0].c.x, py - b[0].c.y) > c.leash - 200.0f) continue;
                const float toTarget = std::hypot(tf->c.x - px, tf->c.y - py);
                if (toTarget > targetNow - 100.0f) continue;
                bool beside = false; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && &o != tf && std::hypot(o.c.x - px, o.c.y - py) < 250.0f) { beside = true; break; }
                if (beside) continue;
                if (bow) { if (PointHidden(px, py, *tf)) continue; if (!have || cost < bestCost) { have = true; gx = px; gy = py; bestCost = cost; bestFoe = toTarget; } }
                else if (!have || toTarget < bestFoe) { have = true; gx = px; gy = py; bestCost = cost; bestFoe = toTarget; }
            }
            return have;
        }

        // ── SIM V3 (iaus-sim-v3.md): the positional world's own questions. Each is asked only while its switch is on ──
        // HANDS. His drawn weapon needs both hands (a bow, a crossbow, a two-hander; a staff is one-handed): a cast puts it away (field 11-12, Auri).
        [[nodiscard]] bool TwoHandWeapon(const Body& x) const {
            if (x.ctl < 0) return false; const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
            return c.drawn == PostureKind::Bow || ((c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver) && x.spec->melee.TwoH());
        }
        // A spell stands on his left hand (cast/Actuation_internal.h:162: a self cast, a heal, a buff and any concentration spell are forced LEFT).
        [[nodiscard]] bool LeftHeld(const Body& x) const { return g_v3.hands && x.ctl >= 0 && (x.stream.live || (x.act == Act::Cast && now < x.actEnd)); }
        // A live stream keeps his whole body, except a one-handed blade in the other hand (MODEL: he goes on swinging it).
        [[nodiscard]] bool StreamHolds(const Body& x) const {
            if (!x.stream.live) return false; const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
            return !((c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver) && x.spec->melee.Has() && !x.spec->melee.TwoH());
        }
        void EndStream(Body& x, bool lost) {
            if (!x.stream.live) return; x.stream.live = false;
            if (lost && x.ctl == 0 && x.stream.iaus) ++t.castLost;
            if (TwoHandWeapon(x)) { x.reDrawUntil = now + 0.65; if (x.ctl == 0) ++t.reDraws; }   // ASSUMPTION 0.65 s: half the v2 swap (TM::swap is sheathe + draw)
            x.actEnd = std::max(x.actEnd, now + TM::castRecover);
        }
        // STANCE (scheduler/CombatScan.cpp:1512-1576, the cast latch: "magicka >= cost -> Cast, else (dry) Melee"): can he pay for a kept
        // offense / control spell of his own book right now?
        [[nodiscard]] bool CanAffordOffense(const Ctl& c, const Body& x) const {
            for (const SpellEntry& e : c.book.entries) if (e.source == SpellSource::Spell && e.drop == Prune::Drop::Keep && (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control) && e.cost <= x.c.mp) return true;
            return false;
        }
        // CHASE CAP (native/Confidence.h ChaseRadius, mirrored constant for constant): fChaseMin 600 + Of x (fChaseMax 3000 - 600),
        // Of = Vitality (0.75 hp + 0.25 stamina) x Trend x FoeMultiplier(load). Called once per own service: it also pushes the trend sample (NoteHealth).
        [[nodiscard]] float ChaseCap(const Body& x, const Ctl& c) const {
            const float hp = x.c.hpMax > 0.0f ? std::clamp(x.c.hp / x.c.hpMax, 0.0f, 1.0f) : 0.0f, st = x.c.spMax > 0.0f ? std::clamp(x.c.sp / x.c.spMax, 0.0f, 1.0f) : 1.0f;
            constexpr std::size_t kRing = 32; bool push = true;
            if (c.trendCount > 0) { const double dt = now - c.trend[c.trendHead].first; if (dt < 0.0 || dt > 10.0) c.trendCount = 0; else if (dt < 0.25) push = false; }   // kTrendMaxGap / kTrendMinStep
            if (push) { c.trendHead = c.trendCount == 0 ? 0 : (c.trendHead + 1) % kRing; c.trend[c.trendHead] = { now, hp }; c.trendCount = std::min(c.trendCount + 1, kRing); }
            float rate = 0.0f;   // HpLossRate
            if (c.trendCount >= 2) { const auto& nw = c.trend[c.trendHead]; const auto& nw1 = c.trend[(c.trendHead + kRing - 1) % kRing];
                const double window = std::max(5.0, 2.5 * (nw.first - nw1.first)); const std::pair<double, float>* old = &nw1;
                for (std::size_t k = 2; k < c.trendCount; ++k) { const auto& e = c.trend[(c.trendHead + kRing - k) % kRing]; if (nw.first - e.first > window) break; old = &e; }
                const double span = nw.first - old->first;
                if (span >= 1.0) rate = std::max(0.0f, std::min(0.25f, old->second - nw.second) / static_cast<float>(std::max(span, 5.0))); }
            const float trend = rate <= 0.002f ? 1.0f : std::clamp((hp / rate) / 20.0f, 0.30f, 1.0f);
            float load = 0.0f; int n = 0;   // CombatSense::FoeLoad: sum of FoeWeight over the group's live foes; an empty group in combat counts one even foe
            for (const Body& o : b) if (o.role == Role::Foe && Up(o)) { ++n;
                load += std::clamp(std::sqrt(static_cast<float>(std::max(1, o.c.level)) / static_cast<float>(std::max(1, x.c.level)) * std::max(0.0f, o.c.hp) / std::max(1.0f, x.c.hpMax)), 0.5f, 2.0f); }
            if (n == 0) load = 1.0f;
            const float of = std::clamp((0.75f * hp + 0.25f * st) * trend * std::clamp(0.90f / (1.0f + 0.25f * load), 0.15f, 0.90f), 0.0f, 1.0f);
            return 600.0f + of * (3000.0f - 600.0f);   // Config.h g_chaseMin / g_chaseMax (the INI defaults)
        }
        // LINE OF FIRE (native/Sightline.cpp:429-477 TeammateInFireLine, kFireLinePad 80; APMF_API.h:801 kCastGuard_LineOfFire): a friendly
        // (anyone not hostile to the caster: the player, a follower, a summon; not the caster, not the target) within 80 units of the segment caster -> aim, measured in 3D and
        // only between the two ends, or within the spell's area + 64 of the aim. Returns the blocker (nullptr = clear).
        [[nodiscard]] const Body* LofBlocker(const Body& x, const Body& tg, float area) const {
            const float ax = x.c.x, ay = x.c.y, az = x.c.z, abx = tg.c.x - ax, aby = tg.c.y - ay, abz = tg.c.z - az, len2 = abx * abx + aby * aby + abz * abz;
            for (const Body& o : b) {
                if (&o == &x || &o == &tg || o.Party() != x.Party() || !Up(o)) continue;   // a FRIENDLY = anyone not hostile to the caster (APMF_API.h:801-803): the player, followers, summons
                const float apx = o.c.x - ax, apy = o.c.y - ay, apz = o.c.z - az;
                if (area > 0.0f && std::hypot(o.c.x - tg.c.x, o.c.y - tg.c.y, o.c.z - tg.c.z) <= area + 64.0f) return &o;
                if (len2 <= 1.0f) { if (std::hypot(apx, apy, apz) <= 80.0f) return &o; continue; }
                const float tt = (apx * abx + apy * aby + apz * abz) / len2; if (tt < 0.0f || tt > 1.0f) continue;
                if (std::hypot(apx - tt * abx, apy - tt * aby, apz - tt * abz) <= 80.0f) return &o;
            }
            return nullptr;
        }
        // The same question from a point he could stand at (the oracle's ext:clear_line).
        [[nodiscard]] bool LofClearFrom(const Body& x, float px, float py, const Body& tg) const {
            const float abx = tg.c.x - px, aby = tg.c.y - py, len2 = abx * abx + aby * aby; if (len2 <= 1.0f) return true;
            for (const Body& o : b) { if (&o == &x || &o == &tg || o.Party() != x.Party() || !Up(o)) continue;
                const float apx = o.c.x - px, apy = o.c.y - py, tt = (apx * abx + apy * aby) / len2; if (tt < 0.0f || tt > 1.0f) continue;
                if (std::hypot(apx - tt * abx, apy - tt * aby) <= 80.0f) return false; }
            return true;
        }
        // A cast of his that the guard would hold: hostile, not Self delivered, he is a follower (Harbinger guards the managed followers).
        [[nodiscard]] bool LofGuarded(const Body& x, const SimSpell& s) const {
            return g_v3.lof && x.ctl >= 0 && !s.e.selfDelivery && !s.reanimate && (s.e.nature == Prune::Nature::Offense || s.e.nature == Prune::Nature::Control);
        }
        // A held caster side steps across the line (ASSUMPTION, iaus-sim-v3.md 3.3; its own switch `sidestep`): 160 units to a RANDOM side
        // (--side-step away: to the side the friend in his way is not on), with the chance his style's long-range strafe mult gives (CSLR).
        void StrafeFrom(Body& x, const Body& tg, const Body& blocker) {
            if (!g_v3.sidestep || now < x.strafeUntil) return;
            if (!VP(x, 1, std::clamp(2.0f * StyleOf(x).strafe, 0.1f, 1.0f))) { x.strafeUntil = now + 0.4; x.sgx = x.c.x; x.sgy = x.c.y; return; }   // no step this time; asked again in 0.4 s
            const float dx = tg.c.x - x.c.x, dy = tg.c.y - x.c.y, l = std::max(1.0f, std::hypot(dx, dy)); float px = -dy / l, py = dx / l;
            if (g_sideStep == 2 ? (blocker.c.x - x.c.x) * px + (blocker.c.y - x.c.y) * py > 0.0f : VP(x, 2, 0.5f)) { px = -px; py = -py; }
            x.sgx = x.c.x + px * 160.0f; x.sgy = x.c.y + py * 160.0f; x.strafeUntil = now + 0.8; if (x.ctl == 0) ++t.strafes;
        }
        // GUARD ARC (ASSUMPTION g_blockArc, iaus-sim-v3.md 5): the attacker stands inside the arc in front of the blocker.
        [[nodiscard]] static bool InArc(const Body& d, const Body& a) { return AngleDeg(d, a) <= 0.5f * g_blockArc; }
        // HEIGHT: a dragon takes off on a circuit around the party (ASSUMPTION, iaus-sim-v3.md 3.1: rFar from the two field figures,
        // "2184 to 13242 u" (ClickUp 1dfcuabd44c) and "an airborne Alduin 5-11k away" (native/CombatSense.h:124)).
        void TakeOff(Body& x) {
            float cx = 0, cy = 0; int n = 0; for (const Body& o : b) if (o.Party() && Up(o)) { cx += o.c.x; cy += o.c.y; ++n; }
            if (n) { cx /= static_cast<float>(n); cy /= static_cast<float>(n); }
            x.airborne = true; x.fcx = cx; x.fcy = cy; x.fr0 = std::max(200.0f, std::hypot(x.c.x - cx, x.c.y - cy)); x.fTh0 = std::atan2(x.c.y - cy, x.c.x - cx);
            x.frFar = std::max(x.fr0 + 500.0f, VU(x, 3, 4000.0f, 11000.0f)); x.fAlt = VU(x, 4, 900.0f, 2400.0f); x.fDir = VP(x, 5, 0.5f) ? 1.0f : -1.0f;
            x.fT = static_cast<double>(VU(x, 6, 12.0f, 20.0f)); x.fT0 = now; x.airUntil = now + x.fT;
        }
        void FlightStep(Body& x) {
            const float u = std::clamp(static_cast<float>((now - x.fT0) / x.fT), 0.0f, 1.0f), sn = std::sin(3.14159265f * u);
            const float r = x.fr0 + (x.frFar - x.fr0) * sn * sn, th = x.fTh0 + x.fDir * 3.14159265f * u;
            const float nx = x.fcx + r * std::cos(th), ny = x.fcy + r * std::sin(th);
            Face(x, nx + (nx - x.c.x), ny + (ny - x.c.y)); x.c.x = nx; x.c.y = ny; x.movedAt = now; x.ranLast = true;
            x.c.z = x.fAlt * std::clamp(std::min(u, 1.0f - u) * static_cast<float>(x.fT) / 1.5f, 0.0f, 1.0f);
            x.c.outOfMeleeReach = x.c.z > 120.0f + 200.0f;   // CombatSense::OutOfMeleeReach: airborne and |dz| > body height (kNominalBodyHeight 120) + fMeleeReach 200
        }
        // ATTACK SLOTS (ASSUMPTION, iaus-sim-v3.md 3.3): two or more melee attackers on one target stand 50 degrees apart on a ring
        // around it. Returns the point x should stand at to strike tg (tg's own place when he is the only one).
        void SlotPoint(const Body& x, const Body& tg, float ring, float& gx, float& gy) const {
            gx = tg.c.x; gy = tg.c.y; int n = 0, mine = 0; float mx = 0, my = 0;
            for (const Body& o : b) { if (!Up(o) || o.c.targetId != tg.c.id || o.Party() == tg.Party() || o.rangedMode || o.casterMode || o.airborne) continue;
                const float dx = o.c.x - tg.c.x, dy = o.c.y - tg.c.y, l = std::hypot(dx, dy); if (l > 900.0f) continue;   // only the ones already around him
                if (o.c.id < x.c.id) ++mine; ++n; if (l > 1.0f) { mx += dx / l; my += dy / l; } }
            if (n < 2 || std::hypot(mx, my) < 1e-3f) return;
            const float ang = std::atan2(my, mx) + (static_cast<float>(mine) - 0.5f * static_cast<float>(n - 1)) * 0.8727f;
            gx = tg.c.x + ring * std::cos(ang); gy = tg.c.y + ring * std::sin(ang);
        }

        // ── building ──
        Body& Add(Role role, std::shared_ptr<const ActorSpec> sp, float x, float y, float hpPct = 1.0f) {
            Body n; n.role = role; n.spec = std::move(sp); n.c.id = role == Role::Player ? 2u : role == Role::Foe ? 100u + nextId++ : 10u + nextId++;
            n.c.name = n.spec->name; n.c.level = n.spec->level; n.c.hpMax = n.spec->hp; n.c.mpMax = n.spec->mp; n.c.spMax = n.spec->sp;
            n.c.hp = n.c.hpMax * hpPct; n.c.mp = n.c.mpMax; n.c.sp = n.c.spMax; n.c.x = x; n.c.y = y; n.c.isPlayer = role == Role::Player;
            n.c.undead = n.spec->undead; n.c.unholy = n.spec->undead; n.spellReady.assign(n.spec->spells.size(), 0.0);
            n.think = RU(n, 7, 0.0f, 0.8f); n.arrows = n.spec->ranged.Has() ? 60 : 0;
            if (D->poise.on) {   // PoiseAV::GetBaseActorValue: (the race's JSON multiplier, else its mass) x BaseMult + worn armor weight x ArmorMult (weight MODEL: 6 per piece)
                const PoiseRules& p = D->poise; const auto ri = p.races.find(n.spec->race);
                n.poiseMax = std::max(0.0f, (ri != p.races.end() ? ri->second : n.spec->mass) * p.baseMult + static_cast<float>(n.spec->pieces) * 6.0f * p.armorMult); n.poise = n.poiseMax;
            }
            b.push_back(std::move(n)); return b.back();
        }

        // ── damage and healing ──
        void Down(Body& x, const Body* by) {
            if (trace) std::fprintf(trace, "%7.2f  DOWN %s (%u) by %s\n", now, x.c.name.c_str(), x.c.id, by ? by->c.name.c_str() : "-");
            if (x.role == Role::Foe || x.role == Role::Summon) {
                x.alive = false; x.c.hp = 0;
                if (x.role == Role::Foe) { if (t.firstKill < 0) t.firstKill = now; t.killOrder.push_back(x.tag.empty() ? "grunt" : x.tag); }
            } else if (x.role == Role::Player) { x.alive = false; t.playerDied = true; }
            else {
                if (g_v3.delivery && x.ctl == 0 && x.act == Act::Cast && x.actIaus && x.hitAt >= 0) ++t.castLost;
                if (x.stream.live) { x.stream.live = false; if (x.ctl == 0 && x.stream.iaus) ++t.castLost; } x.reDrawUntil = 0;
                x.c.downed = true; x.downUntil = now + TM::downSec; x.c.hp = 0; x.act = Act::None; x.hitAt = -1; x.actEnd = now; ++t.partyDowns;
                if (g_followerDeath) { x.alive = false; x.downUntil = 1e18; t.partyDowns += 2; }   // dead for good: three downs' worth in Utility
                if (x.ctl == 0) { ++t.downs; if (now - lastTauntBy0 < 8.0) ++t.tauntThenDown;
                    if (ctl[0].book.kit.potionsHealth > 0) ++t.downsWithPotH;
                    if (!decDowned) { decDowned = true; for (int k = 1; k < 10; ++k) if (decCrossAt[k] >= 0) { ++t.decDownAfter[k]; t.decToDownSec[k] += now - decCrossAt[k]; } } }
            }
            (void)by;
        }
        // SIM V3 `burst`: the game's hit sink (iaus/Fills.cpp, TESHitEvent on a follower or the player that no party member caused) and the per-lap sample.
        // Called where the lab lands a HIT (a weapon blow, a spell's impact, a bash); a tick of a lasting effect raises no event and shows as the net drop.
        void NoteHit(Body& x, const Body& by) {
            if (!g_v3.burst || !estimates || by.Party() || !x.Party() || x.role == Role::Summon) return;
            Combatant ae, xe; FillEstimate(*this, by, ae); FillEstimate(*this, x, xe);
            const float est = ae.hitIsMagic ? ae.hit * (std::max)(0.0f, 1.0f - xe.magicResist / 100.0f) : ae.hit * (1.0f - xe.armor);
            ++x.burstN; if (std::isfinite(est) && est > 0.0f) x.burstFloor += est;
        }
        void SampleBursts() {
            for (Body& x : b) {
                if (!x.alive) continue;
                if (x.burstHp >= 0.0f) {
                    const float gross = MFO::Iaus::Estimate::LapGross(x.burstHp - x.c.hp, x.burstN, x.burstFloor);
                    if (std::isfinite(gross) && gross > 0.0f) x.burst.push_back({ now, gross });
                }
                x.burstN = 0; x.burstFloor = 0.0f; x.burstHp = x.c.hp;
                std::erase_if(x.burst, [&](const MFO::Iaus::Estimate::BurstSample& s) { return now - s.t > kBurstKeepSec; });
            }
        }
        // returns the health actually removed
        float Hurt(Body& x, float dmg, Body* by, bool fromIaus = false) {
            if (g_calmBreaks && x.calmed && by && by->Party() && dmg > 0.0f && now < x.ctrlUntil) { x.ctrlUntil = now; x.calmed = false; }
            if (!x.alive || x.c.downed || dmg <= 0) return 0;
            const float before = x.c.hp; x.c.hp -= dmg; x.regenAt[0] = now + D->rules.delayHp; x.recentDmg += dmg;
            const float dealt = std::min(before, dmg);
            if (by) { by->dmgDealt += dealt; x.lastAttacker = by->c.id; if (by->ctl == 0 && x.role == Role::Foe && dealt > 0.0f && t.firstHitN == 0) { t.firstHitN = 1; t.firstHitSum = now; }
            if (by->ctl == 0) { t.dmgDone += dealt; if (x.spec->undead) t.undeadDmg += dealt; } }
            if (fromIaus && by && by->ctl == 0) { t.iausDmg += dmg; t.overkill += dmg - dealt; }
            if (x.role == Role::Player) t.playerDmgTaken += dealt;
            if (x.Party()) t.partyDmgTaken += dealt;
            if (x.ctl == 0) { t.dmgTaken += dealt; if (now < x.retreatUntil) t.kiteDmgTaken += dealt;
                { const float hm = std::max(1.0f, x.c.hpMax); t.decBandLoss[std::clamp(static_cast<int>(before / hm * 10.0f), 0, 9)] += static_cast<double>(dealt / hm);   // the decline record: the loss is booked to the band he was in
                  const float fr = std::max(0.0f, x.c.hp) / hm; for (int k = 9; k >= 1; --k) if (fr < static_cast<float>(k) * 0.1f && decCrossAt[k] < 0) { decCrossAt[k] = now; ++t.decCross[k]; }
                  if (x.act == Act::Drink && x.actSpell == 0 && x.hitAt >= 0 && x.c.hp <= 0) ++t.downsMidDrink; }
                const Ctl& c0 = ctl[0]; if (c0.runPhase >= 1 && c0.runPhase <= 3) { const double fr = static_cast<double>(dealt / std::max(1.0f, x.c.hpMax)); t.runDmg += fr; if (now - c0.runStart > 5.0) t.runDmgAfter5 += fr; } }   // section 31.9: what a G3 run costs him
            if (x.c.hp <= 0) {
                if (x.Party() && x.role != Role::Summon && !ctl.empty()) {   // could the subject have healed him?
                    const Ctl& c0 = ctl[0]; const Body& s0 = b[static_cast<std::size_t>(c0.body)];
                    if (Up(s0)) for (std::size_t i = 0; i < c0.book.entries.size(); ++i) { const SpellEntry& e = c0.book.entries[i];
                        if (e.nature == Prune::Nature::Heal && c0.gate.spellsOnClass && e.cost <= s0.c.mp && (&x == &s0 || !e.selfDelivery)) { ++t.deathsWithHeal; break; } }
                }
                Down(x, by);
            }
            return dealt;
        }
        void Heal(Body& x, float amt, const Body* by, bool fromIaus) {
            if (!x.alive || amt <= 0) return;
            const float room = x.c.hpMax - x.c.hp, used = std::min(room, amt); x.c.hp += used;
            if (x.c.downed && x.c.hp > 0.1f * x.c.hpMax) { x.c.downed = false; x.downUntil = 0; }
            if (fromIaus && by && by->ctl == 0) { t.healTotal += amt; t.overheal += amt - used; t.healDone += used; }
        }
        void Stagger(Body& x, float sec, Body* by) {
            if (!Up(x) || (!D->poise.on && x.spec->mass >= 6.0f)) return;   // dragons and the like do not stagger (MODEL); under a poise mod the race's meter decides
            if (x.act == Act::Cast && x.hitAt >= 0) { if (x.role == Role::Foe) { ++t.foeCastsInterrupted; if (by && by->role == Role::Follower) ++t.interruptsByFollowers; } }
            if (x.act == Act::Cast && x.ctl == 0 && x.actIaus && (x.hitAt >= 0 || now < x.actEnd) && g_v3.delivery) ++t.castLost;
            if (x.stream.live) EndStream(x, true);
            x.act = Act::None; x.hitAt = -1; x.staggerUntil = now + sec; x.actEnd = now + sec; x.blockUntil = 0;
        }
        // Chocolate Poise, HitEventHandler::RecalculateStagger: the poise damage of one weapon hit. A weapon: lerp(type base, weapon
        // weight, WeightContribution) x Melee / Bow mult; a weaponless creature: physical damage x CreatureMult; a bash: the shield
        // (weight MODEL 10) or the weapon x BashMult. Then x (1 + the attack's stagger offset: ATKD, 1.0 on the usual power attack, 0.5
        // on a bash: MODEL from the modal record values), x (1 - blocked), x total / physical damage, x min(1 - armor, 0.8).
        [[nodiscard]] float PoiseDamage(const Body& a, const Body& d, float rawDmg, bool power, bool ranged, bool bash, float blockedFrac) const {
            const PoiseRules& p = D->poise; const Weapon& w = ranged ? a.spec->ranged : a.spec->melee;
            auto wd = [&](const Weapon& x) { return (x.anim >= 0 && x.anim < 10 && p.hasWeap[x.anim]) ? p.weap[x.anim] + (x.weight - p.weap[x.anim]) * p.weight : x.weight; };
            float s;
            if (bash) s = (a.spec->shield ? (p.hasShield ? p.shield + (10.0f - p.shield) * p.weight : 10.0f) : w.Has() ? wd(w) : 5.0f) * p.bash * 1.5f;
            else if (ranged) s = wd(w) * p.bow;
            else if (w.Has()) s = wd(w) * p.melee;
            else s = rawDmg * p.creature;
            if (power) s *= 2.0f;
            const float mit = ArmorMitigation(D->rules, ArmorOf(d), d.spec->pieces);
            return s * (1.0f - blockedFrac) * (1.0f - mit) * std::min(1.0f - mit, 0.8f);
        }
        // PoiseAV::DamageAndCheckPoise: the meter empties -> ONE stagger whose length follows the blow's share of the meter (MODEL
        // 0.5 s + 1.0 s x share), then the meter is full again; no poise damage while staggered (StaggerMode 1); regen in Regen().
        void PoiseHit(Body& a, Body& d, float amt) {
            const PoiseRules& p = D->poise;
            if (!(amt > 0.0f) || !Up(d) || !(d.poiseMax > 0.0f)) return;
            if (p.staggerMode == 1 && now < d.staggerUntil) return;
            if (a.role == Role::Player || a.role == Role::Follower) amt *= D->rules.diffByPC;   // Settings::GetDamageMultiplier
            amt *= d.role == Role::Player ? p.toPC : p.toNPC;
            d.poise -= amt;
            if (d.poise <= 0.0f) { d.poise = d.poiseMax; Stagger(d, 0.5f + 1.0f * std::clamp(amt / d.poiseMax, 0.0f, 1.0f), &a); }
        }
        // Dynamic Follower Scaling: the multiplier on a follower's weapon hit (1 when the list has no such mod).
        [[nodiscard]] float FollowerScale() const {
            if (!D->dfs.on) return 1.0f;
            int n = 0; for (const Body& o : b) if (o.role == Role::Follower && o.alive) ++n;
            if (n <= 1) return 1.0f;
            return D->dfs.mode == 2 ? 1.0f / static_cast<float>(n) : std::pow(std::max(0.0f, 1.0f - D->dfs.reduction), static_cast<float>(n - 1));
        }
        // one physical hit that has connected in time and space
        // THE FIELD'S HIT-SIZE SPREAD (SIM V3 `hittail`; CALIBRATED, iaus-sim-v3.md 15.5). A foe's physical hit on the party is multiplied by one
        // of these, drawn evenly. They are the 16 hit windows with damage that foes landed on followers in _evidence/deck-run-1117f/MFO.log
        // ([iaus-fill] hit, 19:44:45 to 19:45:51, one fight), each as the health it took before armor (took / (1 - the target's armor share,
        // both printed on the line)) over the MEDIAN of its own attacker kind (eleven arrows of the Conqueror and Ravager Archers, median 53; five
        // blows of one Conqueror Berserker, median 54), then divided by their mean (1.729) so the mean is 1. Arrows: 1.32 3.13 0.91 1.74 1 3.55
        // 1 1 1 2.08 1. Blows: 0.67 6.76 1 1 0.5. MEAN 1 ON PURPOSE: at matched pressure the lab's damage per second already agrees with the
        // field (15.4), what differed is how it arrives. So a usual hit is 0.58 of the lab's old one and one hit in four is 1.2 to 3.9 times it.
        // n = 16, one fight, three followers: re-derive with calib_damage.py when more sessions carry the probe (bIausFillProbe).
        static constexpr float kFieldHit[16] = { 0.76f, 1.81f, 0.53f, 1.01f, 0.58f, 2.05f, 0.58f, 0.58f, 0.58f, 1.20f, 0.58f, 0.39f, 3.91f, 0.58f, 0.58f, 0.29f };
        float Physical(Body& a, Body& d, float dmg, bool power, bool ranged, bool fromIaus) {
            if (g_v3.hittail && a.role == Role::Foe && d.Party()) dmg *= kFieldHit[std::min(15, static_cast<int>(VU(a, 37, 0.0f, 16.0f)))];
            const Rules& r = D->rules; bool blocked = false; float blockedFrac = 0.0f; const float rawDmg = dmg;
            if (a.role == Role::Follower) dmg *= FollowerScale();
            bool sneakAtk = false, flankAtk = false;
            if (a.stealthy && d.role == Role::Foe) {   // AE 44016: the attacker sneaks and the target's own level of him is not above 0 -> x the weapon's sneak multiplier x perk entry 18
                const Ctl& ac = ctl[static_cast<std::size_t>(a.ctl)];
                const float sm = SneakAttackMult(*D, *a.spec, ranged ? a.spec->ranged : a.spec->melee) * ac.sneakAtkClass;
                const bool groupUnaware = a.sneaking && !GroupDetects(a);   // what MFO read when it named this blow (Facts::foeUnaware)
                if (a.sneaking && (now < a.cloakUntil || PairLevel(d, a) <= 0)) { dmg *= sm; sneakAtk = true; }
                else if (ac.flank && g_stealth >= 2) { const Body* dt = Find(d.c.targetId); if (dt && dt != &a && dt->Party()) { dmg *= 1.0f + 0.5f * (sm - 1.0f); flankAtk = true; } }   // G4: half the bonus on a foe engaged with an ally
                if (a.ctl == 0 && groupUnaware) { ++t.stealthTry; if (!sneakAtk) ++t.stealthMiss; }   // the group record said unaware, the pair level refused the multiplier
                if (a.cloakUntil > now) a.cloakUntil = now;   // the blow ends the free entry / the cloak
                ForceDetect(a, d);
            }
            if (a.ctl == 0) { ++t.wpnHits; if (sneakAtk) ++t.sneakHits; if (flankAtk) ++t.flankHits; if (!t.firstHits) { t.firstHits = 1; t.sneakOpens = sneakAtk ? 1 : 0; } }
            if (d.ctl == 0 && !ranged && a.role == Role::Foe) { int held = 0; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && o.tauntBy == d.c.id && now < o.tauntUntil) ++held;
                if (held >= 2) { ++t.inc2; if (now < d.blockUntil) ++t.guard2;
                    else { const Ctl& dc = ctl[0];   // WHY the blow found no guard (--guard-why): 0 staggered, 1 the rest after a hold, 2 hurt with a potion (drink first), 3 drinking, 4 mid blow / other action, 5 free
                        const int why = now < d.staggerUntil ? 0 : now < dc.blockRestUntil ? 1 : d.act == Act::Drink && now < d.actEnd ? 3 : (d.c.hp < dc.gate.drinkHealthLine * d.c.hpMax && dc.book.kit.potionsHealth > 0) ? 2 : now < d.actEnd ? 4 : 5;
                        ++t.ung2[why]; } } }   // Q-S-5 tally (the guard is up when the blow lands: the same test as the block below)
            // SIM V3 (blockdir): the guard covers an arc in front of him (InArc). Every melee blow a foe lands on the subject is tallied by what it met.
            const bool guardUp = now < d.blockUntil && !ranged, inArc = !g_v3.blockdir || InArc(d, a);
            if (d.ctl == 0 && !ranged && a.role == Role::Foe) { ++t.inc; if (guardUp && inArc) ++t.incGuard; else if (guardUp) ++t.incArc; else ++t.incDown; }
            if (guardUp && inArc) {   // MODEL block: shield 0.45 + 0.30 x skill, weapon 0.30 + 0.20 x skill; a power attack halves it (fBlockPowerAttackMult)
                const bool shieldUp = d.spec->shield && !LeftHeld(d);   // SIM V3 (hands): a spell on his left hand = no shield on that arm
                const float sk = d.spec->Skill(kAvBlock) / 100.0f; float bf = shieldUp ? 0.45f + 0.30f * sk : 0.30f + 0.20f * sk;
                if (power) bf *= r.blockPowMult;
                blockedFrac = std::min(0.85f, bf); dmg *= 1.0f - blockedFrac; blocked = true;
                if (!power && shieldUp && RP(d, 8, 0.25f)) Stagger(a, TM::recoil, &d);
            }
            dmg *= a.dmgScale * (1.0f - ArmorMitigation(r, ArmorOf(d), d.spec->pieces));
            if (d.role == Role::Player) dmg *= r.diffToPC;
            if (a.role == Role::Player) dmg *= r.diffByPC;
            if (a.ctl == 0) { if (Controlled(d)) { ++t.hitsOnControlled; if (power) ++t.powOnControlled; }
                for (const Body& o : b) if (o.role == Role::Foe && Up(o) && now < o.ctrlUntil) { ++t.hitsWhileCtl; break; } }
            NoteHit(d, a);
            const float dealt = Hurt(d, dmg, &a, fromIaus);
            // --draw-spoil P (MODEL, no engine source: whether a plain hit interrupts an NPC's draw is not decoded; a stagger already does):
            // a melee hit that lands on a body mid-draw spoils the draw with chance P (the arrow is not fired, he starts again).
            if (g_drawSpoil > 0.0f && !ranged && Up(d) && d.act == Act::Shoot && d.hitAt >= 0 && RP(d, 9, g_drawSpoil)) { d.act = Act::None; d.hitAt = -1; d.actEnd = now; if (d.ctl == 0) ++t.drawSpoiled; }
            if (sneakAtk && a.ctl == 0) t.sneakDmg += dealt;
            if (trace) std::fprintf(trace, "%7.2f  hit  %s -> %s (%u) %.0f%s%s%s%s%s  hp %.0f/%.0f\n", now, a.c.name.c_str(), d.c.name.c_str(), d.c.id, dealt, power ? " POWER" : "", blocked ? " blocked" : "", ranged ? " arrow" : "", sneakAtk ? " SNEAK" : "", flankAtk ? " flank" : "", d.c.hp, d.c.hpMax);
            if (D->poise.on) PoiseHit(a, d, PoiseDamage(a, d, rawDmg, power, ranged, false, blockedFrac));
            else if (power && !blocked && Up(d)) Stagger(d, TM::staggerSec * std::clamp(a.spec->mass / d.spec->mass, 0.6f, 1.4f), &a);
            return dealt;
        }
        // a spell's effect on one target (the magnitude already carries the caster's perks)
        void SpellHit(Body& a, const SimSpell& s, Body& d, bool fromIaus, float mag = 1.0f) {
            if (!Up(d)) return;
            const Rules& r = D->rules;
            const float el = std::clamp(ResistOf(d, s.e.resistAv) / 100.0f, -1.0f, 1.0f), mr = std::clamp(ResistOf(d, kAvResistMagic) / 100.0f, -1.0f, 1.0f);   // mult = (1 - p)(1 - s), 0 at 100 (AE 26507)
            float k = mag * a.dmgScale * (1.0f - el) * (1.0f - mr);
            if (d.role == Role::Player) k *= r.diffToPC;
            if (a.role == Role::Player) k *= r.diffByPC;
            if (s.dmg > 0) { NoteHit(d, a); Hurt(d, s.dmg * k, &a, fromIaus); }
            if (a.stealthy && d.role == Role::Foe && (s.dmg > 0 || s.ctrl)) { if (a.cloakUntil > now) a.cloakUntil = now; ForceDetect(a, d); }
            if (s.dmg > 0 && D->poise.on && D->poise.magicHealth > 0.0f) PoiseHit(a, d, s.dmg * k * D->poise.magicHealth);   // ActiveEffectHandler: health damage x the JSON's factor
            if (trace) std::fprintf(trace, "%7.2f  spell %s: %s -> %s (%u) dmg %.0f (x%.2f)%s  hp %.0f/%.0f\n", now, s.e.name.c_str(), a.c.name.c_str(), d.c.name.c_str(), d.c.id, s.dmg * k, k, s.ctrl ? " control" : "", d.c.hp, d.c.hpMax);
            if (s.dot > 0 && Up(d)) d.fx.push_back({ 0, s.dot * k, -1, now + s.dotDur, a.c.id });
            if (s.mpDmg > 0) d.c.mp = std::max(0.0f, d.c.mp - s.mpDmg * k);
            if (s.spDmg > 0) d.c.sp = std::max(0.0f, d.c.sp - s.spDmg * k);
            if (s.stagger && Up(d)) Stagger(d, TM::bashStagger, &a);
            if (s.ctrl && Up(d)) {
                const bool works = CtrlWorks(s, d);
                if (a.ctl == 0) { ++t.ctrlCasts; if (!works) ++t.ctrlFails; if (Controlled(d)) ++t.ctrlOnControlled; }
                if (works) { const double until = now + std::clamp<double>(s.ctrlDur, 1.0, 60.0);
                    if (s.ctrlPar) { d.ctrlUntil = until; d.c.paralysed = true; d.act = Act::None; d.hitAt = -1; }
                    else if (s.ctrlCalm) { d.ctrlUntil = until; d.calmed = true; d.act = Act::None; d.hitAt = -1; }
                    else if (s.ctrlFrenzy) d.frenzyUntil = until;
                    else { d.fleeUntil = until; d.c.fleeing = true; } }
            }
        }
        [[nodiscard]] bool CtrlWorks(const SimSpell& s, const Body& d) const {
            if (s.ctrlPar) return !d.spec->immunePar;
            if (s.turnUndead && !d.spec->undead) return false;
            if (d.spec->dragon || (d.spec->daedra && !s.turnUndead && (s.ctrlCalm || s.ctrlFlee || s.ctrlFrenzy) && d.spec->creature && d.spec->immunePar)) return false;
            return static_cast<float>(d.c.level) <= s.ctrlMaxLevel;
        }
        [[nodiscard]] Tri WillWork(const SimSpell& s, const Body& d) const {   // what Actuation::WillWork would answer (G10)
            if (s.ctrl) return CtrlWorks(s, d) ? Tri::Yes : Tri::No;
            if (s.e.nature == Prune::Nature::Offense && s.e.resistAv >= 0 && d.spec->Resist(s.e.resistAv) >= 100.0f) return Tri::No;
            return Tri::Yes;
        }

        // ── the engine's combat AI (MODEL from the combat style) ──
        void Move(Body& x, float tx, float ty, float speed) {
            const float dx = tx - x.c.x, dy = ty - x.c.y, d = std::hypot(dx, dy); if (d < 1e-3f) return;
            if (x.airborne || x.perch) return;   // SIM V3: on the wing he flies his circuit (FlightStep), on a ledge he stays on it; nothing walks him
            x.ranLast = speed >= 0.9f * x.spec->run;   // MODEL: combat movement is at the engine's RUN speed and a crouched run is still IsRunning (probe P1 logs it); a backpedal is not
            if (x.sneaking) speed *= 0.55f;   // MODEL: the sneak gait (the crouched run)
            const float k = std::min(speed * TM::dt, d) / d; x.c.x += dx * k; x.c.y += dy * k; x.movedAt = now;
            if (arenaR > 0.0f) { const float ax = x.c.x - arenaX, ay = x.c.y - arenaY, ad = std::hypot(ax, ay);   // --space: a wall stops him (backpedal, kite, flee)
                if (ad > arenaR) { x.c.x = arenaX + ax * arenaR / ad; x.c.y = arenaY + ay * arenaR / ad; } }
        }
        // WHO A FIGHTER PICKS. UNSOURCED (review F6 of d44d42a3): the weights below (nearness, x1.8 for his last attacker, x1.25 for the player,
        // the +-15% draw) are v2's MODEL of the engine's target selection, which is not decoded. They decide how often a foe goes for a
        // caster, and so what a blade in a caster's hand costs him (iaus-sim-v3.md 12.1). --caster-pressure adds fights where they do.
        [[nodiscard]] Body* PickTarget(Body& x) {
            Body* best = nullptr; Body* second = nullptr; float bs = -1, bs2 = -1;
            const bool frenzy = now < x.frenzyUntil;
            for (auto& o : b) {
                if (&o == &x || !Up(o)) continue;
                if (!frenzy && o.Party() == x.Party()) continue;
                if (x.role == Role::Foe && !GroupHas(o)) continue;   // the target selector admits only a target its group knows and has not lost
                float s = 1000.0f / (Dist(x, o) + 300.0f);
                if (o.c.id == x.lastAttacker) s *= 1.8f;
                if (x.role == Role::Foe && o.role == Role::Player) s *= 1.25f;
                if (x.role == Role::Player) s *= 1.0f + (1.0f - o.c.hp / std::max(1.0f, o.c.hpMax));   // the player model finishes what it started
                if (o.c.outOfMeleeReach && !x.spec->ranged.Has() && x.spec->spells.empty()) s *= 0.1f;
                s *= RU(x, 10, 0.85f, 1.15f);
                if (s > bs) { second = best; bs2 = bs; bs = s; best = &o; } else if (s > bs2) { bs2 = s; second = &o; }
            }
            // ch.32 AVERSION (Harbinger ABI v41, apmf/AversionClaims.cpp; FACT = Harbinger's contract): when the foe's own selection lands on
            // the claimant, in the claim's share of 6 s windows it takes its NEXT BEST valid target; with none he is picked as always.
            // Review H2-1 / Q101: the next best is used only when it passes the game's own tests. In the lab those are: up (the loop),
            // not fleeing, and reachable (a melee-only foe cannot take a target that is out of melee reach). Pathing is not modelled.
            if (x.role == Role::Foe && best && AvertedNow(x, *best)) {
                const bool ok = second && !second->c.fleeing && !(second->c.outOfMeleeReach && !x.spec->ranged.Has() && x.spec->spells.empty());
                if (best->ctl == 0) ++(ok ? t.averted : t.avertKept);
                if (ok) return second;
            }
            return best;
        }
        [[nodiscard]] static std::uint64_t Mix64(std::uint64_t z) { z += 0x9E3779B97F4A7C15ULL; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
        // His weight in a foe's selection right now: the class claim (card H2), or 1 during the D&D window (--disengage-dnd).
        [[nodiscard]] float AversionOf(const Body& o) const { if (o.ctl < 0) return 0.0f; if (g_t2.dnd && now < o.dndUntil) return 1.0f; return g_t2.aversion ? ctl[static_cast<std::size_t>(o.ctl)].aversion : 0.0f; }
        // Harbinger draws "averted or not" per (foe, follower) for a 6 s window (question 95), so the share of TIME is the weight. The
        // draw is a hash of the fight's seed, the pair and the window: it never touches R, so a fight's other draws are the same with it on or off.
        [[nodiscard]] bool AvertedNow(const Body& foe, const Body& o) const {
            const float w = AversionOf(o); if (!(w > 0.0f)) return false; if (w >= 1.0f) return true;
            const std::uint64_t h = Mix64(avSeed ^ Mix64(static_cast<std::uint64_t>(foe.c.id) * 0x10001ULL + o.c.id) ^ Mix64(static_cast<std::uint64_t>(now / 6.0) + 77u));
            return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0) < static_cast<double>(w);
        }
        // ch.18 ATTACK SELECTION (Harbinger ABI v34, apmf/AttackMix.cpp; FACT = that file's contract): DENY ONLY, over the follower's last 8
        // STARTED attacks. Share above the ceiling: Power is denied. Share under the floor: Normal is denied, and that deny is lifted for
        // a roll in which no undenied Power attack would be drawn (stamina cannot pay), so a regular attack still fires. Bash is not governed.
        // MODEL: under the floor a denied Normal BECOMES a Power attack whenever stamina can pay. In the engine the roll among the undenied
        // attacks can also come up empty or pick another class, so the lab's floor is the strongest reading of the rule.
        [[nodiscard]] Act MixGate(Body& x, Ctl& c, Act drawn, const Weapon& w) {
            if (c.shareMax < 0.0f && c.shareMin < 0.0f) return drawn;
            if (c.atkN < 8 && !g_mixPartial) return drawn;   // no share, so no deny, until 8 started attacks fill the window
            int p = 0; for (int i = 0; i < c.atkN; ++i) p += c.atkWin[i];
            const float share = c.atkN ? static_cast<float>(p) / static_cast<float>(c.atkN) : 0.0f;
            if (c.shareMax >= 0.0f && drawn == Act::Power && share > c.shareMax) { if (x.ctl == 0) ++t.mixPowerDenied; return Act::Swing; }
            if (c.shareMin >= 0.0f && drawn == Act::Swing && share < c.shareMin) {
                if (x.c.sp >= PowerStamina(D->rules, w)) { if (x.ctl == 0) ++t.mixNormalDenied; return Act::Power; }
                if (x.ctl == 0) ++t.mixFloorLifted;
            }
            return drawn;
        }
        // ch.23 PURSUIT LEASH (apmf/PursuitLeash.cpp: anchor the player, no pursuit past the radius). true = he is outside it and so is
        // his target: he walks back toward the player instead of closing.
        [[nodiscard]] bool LeashTurn(Body& x, const Ctl* c, const Body& tg) {
            if (!c || !(c->leash > 0.0f) || Dist(x, b[0]) <= c->leash || Dist(tg, b[0]) <= c->leash) return false;
            Move(x, b[0].c.x, b[0].c.y, x.spec->run); if (x.ctl == 0) ++t.leashTurns; return true;
        }
        // Card H4 (native/iaus/Reposition.h SpellRange, the game's own function): the distance the spell in his hand is fought at. 0 = none
        // (no spell in hand, a heal in hand, a weapon drawn, a class that does not cast): the engine's own movement, as before.
        [[nodiscard]] float HandRange(const Body& x, const Ctl& c, const Body* = nullptr) const {
            if (!c.handSpell || !(c.drawn == PostureKind::None || c.drawn == PostureKind::Staff) || (c.gate.resolved && !c.gate.spellsOnClass)) return 0.0f;
            return Reposition::HandsRange(c.book, c.handSpell, g_t2.hands ? c.handSpell2 : 0u, Reach(x), TouchSpent(c), c.chosenForm).range.desired;   // Q-I-2: the touch range only while the touch spell is the cast IAUS chose
        }
        // Q-I-2 (iaus/Candidates.cpp, mirrored): the touch spell of his hands fired within Reposition::kTouchSpentSec (Mind::firedAt, the last fired decision).
        [[nodiscard]] bool TouchSpent(const Ctl& c) const {
            if (!g_t2.hands || !c.handSpell2) return false;
            const std::uint32_t tf = Reposition::HandsRange(c.book, c.handSpell, c.handSpell2, 100.0f, false, 0u).touchForm;
            const double ago = now - c.mind.firedAt;
            return tf != 0 && c.mind.hasRunning && c.mind.running.form == tf && ago >= 0.0 && ago <= static_cast<double>(c.gate.touchSpentSec);
        }
        // DODGE (Q-melee-7, MFO-B806). FACT (research in Harbinger Docs, batch M): the engine's side step (CombatBehaviorDodgeThreat) rolls
        // against the combat style's Avoid Threat chance, and since Harbinger v45 a CLASS style reaches the engine in full (Q-melee-6).
        // MODEL: one roll per projectile launched at him, only for a follower with a class package, only when no blow of his own is committed and the
        // projectile flies at least g_dodgeFlight seconds; a rolled step clears a NON-area projectile and costs him 0.4 s. A blast is not stepped out of.
        void TryDodge(Shot& sh, double flight, bool area) {
            if (!g_t2.dodge || area) return;
            Body* d = Find(sh.tgt); if (!d || d->ctl < 0 || !Up(*d) || d->hitAt >= 0 || now < d->staggerUntil || now < d->ctrlUntil || d->act == Act::Block) return;   // MODEL: not while his own blow, shot or cast is committed, staggered, held, or behind his guard
            Ctl& c = ctl[static_cast<std::size_t>(d->ctl)]; if (!c.pkgClass || c.runPhase == 1 || c.runPhase == 2 || c.runPhase == 5) return;
            const float p = StyleOf(*d).avoidThreat; if (!(p > 0.0f) || flight < static_cast<double>(g_dodgeFlight)) return;
            if (d->ctl == 0) ++t.dodgeTries;
            if (!RP(*d, 11, std::clamp(p, 0.0f, 1.0f))) return;
            sh.dodged = true; if (d->ctl == 0) ++t.dodges;
            const Body* a = Find(sh.src); float ux = 0, uy = 1; if (a) { const float dx = d->c.x - a->c.x, dy = d->c.y - a->c.y, l = std::max(1.0f, std::hypot(dx, dy)); ux = -dy / l; uy = dx / l; }
            if (RP(*d, 12, 0.5f)) { ux = -ux; uy = -uy; }
            if (Standable(d->c.x + ux * 90.0f, d->c.y + uy * 90.0f)) { d->c.x += ux * 90.0f; d->c.y += uy * 90.0f; IntoRoom(*d, arenaR); d->movedAt = now; d->ranLast = true; }
            d->act = Act::None; d->hitAt = -1; d->actEnd = now + 0.4;
            if (trace) std::fprintf(trace, "%7.2f  DODGE %s side steps a projectile (avoid threat %.2f)\n", now, d->c.name.c_str(), p);
        }
        // CARD G7 (Vocabulary.h cond.corpse_within; Harbinger ABI v42 kCastFlag_CorpseTarget): the corpse a Reanimate spell is cast at.
        // FACT (the opcode's contract, questions 102-104): a dead non-teammate HUMANOID within `within` units (0 = the scan radius), the
        // spell must be able to raise it (its level cap), the highest level wins, the nearest on a tie. MODEL: humanoid = not a creature,
        // undead, daedra, dragon or animal spec; a body is raised once; 4096 units stand for the scan radius.
        [[nodiscard]] const Body* CorpseFor(const Body& x, float within, const SimSpell* s) const {
            const Body* best = nullptr;
            for (const Body& o : b) { if (o.role != Role::Foe || o.alive || o.corpseUsed) continue;
                const ActorSpec& sp = *o.spec; if (sp.creature || sp.undead || sp.daedra || sp.dragon || sp.animal) continue;
                const float d = Dist(x, o); if (d > 4096.0f || (within > 0.0f && d > within)) continue;
                if (s && static_cast<float>(o.c.level) > s->ctrlMaxLevel) continue;
                if (!best || o.c.level > best->c.level || (o.c.level == best->c.level && d < Dist(x, *best))) best = &o; }
            return best;
        }
        void StartSwing(Body& x, Body& tg, Act kind, bool fromIaus) {
            const Weapon& w = x.spec->melee; const bool power = kind == Act::Power;
            float base = !w.Has() ? TM::swingCreature : w.TwoH() ? TM::swing2H : TM::swing1H;
            float dur = base / std::max(0.3f, w.Has() ? w.speed : 1.0f), hf = TM::hitFrac;
            if (power) { dur *= TM::powerMult; hf = TM::powerHitFrac; x.c.sp = std::max(0.0f, x.c.sp - PowerStamina(D->rules, w)); x.regenAt[2] = now + D->rules.delaySp + dur; if (x.ctl == 0) ++t.powerAttacks; }
            if (kind == Act::Bash) { dur = TM::bash; hf = TM::bashHitFrac; x.c.sp = std::max(0.0f, x.c.sp - 20.0f); }
            x.act = kind; x.actTarget = tg.c.id; x.hitAt = now + dur * hf; x.actEnd = now + dur; x.actIaus = fromIaus; x.swingDist = Dist(x, tg);
            if (x.ctl >= 0 && kind != Act::Bash) { Ctl& c = ctl[static_cast<std::size_t>(x.ctl)]; c.atkWin[c.atkAt] = power ? 1 : 0; c.atkAt = (c.atkAt + 1) % 8; c.atkN = std::min(8, c.atkN + 1); }   // ch.18: one STARTED attack
            Action(x, w.anim == 2 ? dg.soundSilent : dg.soundNormal);   // MODEL: a dagger's detection sound level is Silent, any other swing Normal
            // the defender may read the swing and block it (MODEL: 0.20 x defensive mult, + 0.25 with a shield)
            if (kind != Act::Bash && Up(tg) && !Busy(tg) && tg.spec->hasStyle && (tg.spec->shield || tg.spec->melee.Has()) && !tg.rangedMode && !tg.casterMode && tg.c.targetId == x.c.id) {
                const float p = std::clamp(0.20f * StyleOf(tg).def + (tg.spec->shield && !LeftHeld(tg) ? 0.25f : 0.0f), 0.0f, 0.75f);
                if (RP(tg, 13, p)) { tg.act = Act::Block; tg.blockUntil = x.hitAt + 0.15; tg.actEnd = tg.blockUntil; tg.c.blocking = true; }
            }
        }
        // Releases the pending action of x at its hit frame.
        void Resolve(Body& x) {
            const Act kind = x.act; x.hitAt = -1;
            Body* tg = Find(x.actTarget);
            if (kind == Act::Swing || kind == Act::Power || kind == Act::Bash) {
                const bool whiff = tg && Up(*tg) && (Dist(x, *tg) > Reach(x) * 1.2f || tg->c.outOfMeleeReach);
                if (tg && Up(*tg) && x.ctl >= 0 && kind != Act::Bash && g_learnReach) ctl[static_cast<std::size_t>(x.ctl)].reach.Observe(x.c.id, kReachWeapon, kReachSeedEdge, 1u, std::max(0.0f, x.swingDist - (tg->c.bodyRadius > 0.0f ? tg->c.bodyRadius : MFO::Iaus::ReachBook::kRefRadius)), !whiff);   // Fills.cpp LearnReach: one outcome, a chain of one swing, the EDGE distance at its start
                if (whiff && x.ctl == 0 && kind == Act::Power && x.actIaus) ++t.powerWhiffs;
                if (!tg || !Up(*tg) || whiff) return;   // whiff
                if (kind == Act::Bash) { NoteHit(*tg, x); Hurt(*tg, 3.0f, &x); if (D->poise.on) PoiseHit(x, *tg, PoiseDamage(x, *tg, 0.0f, false, false, true, 0.0f)); else Stagger(*tg, TM::bashStagger, &x); return; }
                Physical(x, *tg, WeaponDamage(D->rules, *x.spec, x.spec->melee, kind == Act::Power) * FortifyDmg(x), kind == Act::Power, false, x.actIaus);
            } else if (kind == Act::Shoot) {
                if (!tg) return;
                --x.arrows; if (x.ctl == 0) ++t.arrowsShot;
                float shotDmg = WeaponDamage(D->rules, *x.spec, x.spec->ranged, false) * FortifyDmg(x);
                // --ranged-perks (FACT: a ModAttackDamage entry is conditioned on the weapon it fires for, Overdraw on a bow, Armsman on a
                // one-hander; the lab applied the MELEE weapon's chain to every shot): the shot carries the bow skill's chain instead.
                if (g_rangedPerks && x.spec->rangedAtkMult > 0.0f && x.spec->atkMult > 0.0f) shotDmg = shotDmg / x.spec->atkMult * x.spec->rangedAtkMult;
                if (x.ctl == 0 && Dist(x, *tg) < D->rules.holdMinDist) ++t.pointBlankShots;
                Shot sh{ now + Dist(x, *tg) / TM::arrowSpeed, x.c.id, tg->c.id, shotDmg, -1, false, x.actIaus, tg->c.x, tg->c.y, 1 };
                // CARD H5 (ch.29, apmf/AimClaims.cpp; FACT = that file's contract): over his last 10 shots, when the misses plus the shots
                // in flight reach the class's budget, the next launch is re-aimed at the engine's own unscattered solution.
                if (x.ctl >= 0 && g_t2.aim) { Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
                    if (c.aimBudget >= 0) { int miss = 0; for (int i = 0; i < c.aimN; ++i) miss += c.aimWin[i];
                        if (miss + c.aimFlying >= c.aimBudget) { sh.corrected = true; if (x.ctl == 0) ++t.aimCorrected; } ++c.aimFlying; } }
                TryDodge(sh, sh.at - now, false);
                shots.push_back(sh);
            } else if (kind == Act::Cast) {
                if (HoldCast(x, tg)) return;   // SIM V3 (lof): held, or given up
                ReleaseCast(x, tg);
            } else if (kind == Act::Drink && x.ctl >= 0 && x.actSpell >= 10) {   // --consumables: a buff potion (10 + kind) or a restore food (20 + family)
                Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
                if (x.actSpell >= 20) { const int fam = x.actSpell - 20; const float m = c.book.kit.restoreFood[static_cast<std::size_t>(fam)].magnitude;
                    if (fam == 0) Heal(x, m, nullptr, false); else if (fam == 1) x.c.sp = std::min(x.c.spMax, x.c.sp + m); else x.c.mp = std::min(x.c.mpMax, x.c.mp + m); }
                else { const int bk = x.actSpell - 10; const auto K = static_cast<MFO::Iaus::BuffKind>(bk); const float m = c.buffMag[static_cast<std::size_t>(bk)];
                    const double until = now + static_cast<double>(std::max(1.0f, c.buffDur[static_cast<std::size_t>(bk)]));
                    if (MFO::Iaus::IsResist(K)) x.fx.push_back({ 3, m, MFO::Iaus::ResistAvOf(K), until, x.c.id, bk });
                    else if (K == MFO::Iaus::BuffKind::FortifyHealth || K == MFO::Iaus::BuffKind::FortifyMagicka || K == MFO::Iaus::BuffKind::FortifyStamina) {
                        const int pool = K == MFO::Iaus::BuffKind::FortifyHealth ? 0 : K == MFO::Iaus::BuffKind::FortifyMagicka ? 1 : 2;
                        float& mx = pool == 0 ? x.c.hpMax : pool == 1 ? x.c.mpMax : x.c.spMax; float& cur = pool == 0 ? x.c.hp : pool == 1 ? x.c.mp : x.c.sp;
                        mx += m; cur += m; x.fx.push_back({ 6, m, pool, until, x.c.id, bk }); }
                    else if (K == MFO::Iaus::BuffKind::FortifySkill || K == MFO::Iaus::BuffKind::FortifyAttack) x.fx.push_back({ 7, m, -1, until, x.c.id, bk });
                    c.book.kit.buff[static_cast<std::size_t>(bk)].active = true; }
            } else if (kind == Act::Drink && x.ctl >= 0) {
                const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
                // A potion with a duration restores its magnitude PER SECOND for that long (the record's ValueModifier effect); without one, at once.
                const float dur = c.potDur[std::clamp(x.actSpell, 0, 2)];
                if (dur > 0.0f) { x.fx.push_back({ x.actSpell == 0 ? 1 : x.actSpell == 1 ? 4 : 5, x.actSpell == 0 ? c.potH : x.actSpell == 1 ? c.potM : c.potS, -1, now + static_cast<double>(dur), x.c.id });
                    if (x.ctl == 0 && x.actSpell == 0) t.potHeal += c.potH * dur; }   // what ran over his maximum is not tallied for a potion over time (potWaste counts the at-once ones)
                else if (x.actSpell == 0) { if (x.ctl == 0) { t.potHeal += c.potH; t.potWaste += std::max(0.0f, c.potH - (x.c.hpMax - x.c.hp)); } Heal(x, c.potH, nullptr, false); } else if (x.actSpell == 1) x.c.mp = std::min(x.c.mpMax, x.c.mp + c.potM); else x.c.sp = std::min(x.c.spMax, x.c.sp + c.potS);
            } else if (kind == Act::Taunt) {
                // ONE attempt rolls on EVERY candidate foe (Taunt.cpp): the foes fighting the party inside the radius that can sense him.
                // p = 0.5 + 0.05 per level he has over the foe, in [0.1, 0.9] (the executor's own rule, Estimate::TauntChance).
                bool any = false;
                for (Body& o : b) { if (o.role != Role::Foe || !Up(o) || Dist(x, o) > TM::tauntRadius || now < o.tauntUntil || !Sees(o, x)) continue;
                    const Body* ot = Find(o.c.targetId); if (!ot || !ot->Party() || ot == &x) continue;
                    if (RP(o, 14, std::clamp(0.5f + 0.05f * static_cast<float>(x.c.level - o.c.level), 0.1f, 0.9f))) { o.tauntUntil = now + TM::tauntDur; o.tauntBy = x.c.id; o.c.taunted = true; o.c.targetId = x.c.id; any = true; } }
                if (any) { x.think = now; if (x.ctl == 0) lastTauntBy0 = now; } else if (x.ctl == 0) ++t.tauntFail;
            }
        }
        [[nodiscard]] const SimSpell* SpellOf(const Body& x, int idx) const {
            if (idx < 0) return nullptr;
            if (x.ctl >= 0) { const auto& sp = ctl[static_cast<std::size_t>(x.ctl)].build->spells; return static_cast<std::size_t>(idx) < sp.size() ? &sp[static_cast<std::size_t>(idx)] : nullptr; }
            return static_cast<std::size_t>(idx) < x.spec->spells.size() ? &D->foeSpells[static_cast<std::size_t>(x.spec->spells[static_cast<std::size_t>(idx)])] : nullptr;
        }
        // SIM V3 (lof): the release of a guarded cast while a friend stands in the corridor. true = no release now (held 0.25 s, the
        // guard's own refresh; or given up at the 6 s hold cap, cast/Hands.cpp kInFlightHoldCap). A held cast is not paid for.
        // The guard answers at the START of a cast (0x06 CheckStartCast, APMF_API.h:806,618) and at its release (0x0A).
        //   an ENGINE-AI cast: a blocked start is never made (BrainCast passes the spell over, the AI picks something else); a blocked
        //     release is refused and the cast is over. No hold: nothing stands behind it.
        //   a CLAIMED cast (a decision's): MFO's claim stands while the guard says no, so the start (StartTest) and the release wait in
        //     0.25 s steps until the line is clear or the claim is 6 s old (kInFlightHoldCap).
        [[nodiscard]] bool HoldCast(Body& x, Body* tg) {
            const SimSpell* s = SpellOf(x, x.actSpell); if (!s || !tg || !Up(*tg) || !LofGuarded(x, *s)) return false;
            const Body* blocker = LofBlocker(x, *tg, s->e.areaUnits); if (!blocker) return false;
            const bool subj = x.ctl == 0 && x.actIaus;
            if (!x.actIaus) { x.actEnd = now + TM::castRecover; StrafeFrom(x, *tg, *blocker); if (trace) std::fprintf(trace, "%7.2f  LOF %s's own cast of %s is refused at the release: %s stands in the line\n", now, x.c.name.c_str(), s->e.name.c_str(), blocker->c.name.c_str()); return true; }
            if (now - x.claimAt >= 6.0) { if (subj) ++t.castLofExpired; x.actEnd = now + TM::castRecover; if (trace) std::fprintf(trace, "%7.2f  LOF %s gives up %s: %s stood in the line for 6 s\n", now, x.c.name.c_str(), s->e.name.c_str(), blocker->c.name.c_str()); return true; }
            if (subj) { if (!x.lofWaited) ++t.castLofHeld; t.castLofSec += 0.25; }
            if (trace && !x.lofWaited) std::fprintf(trace, "%7.2f  LOF %s holds %s: %s stands in the line to %s\n", now, x.c.name.c_str(), s->e.name.c_str(), blocker->c.name.c_str(), tg->c.name.c_str());
            x.lofWaited = true; x.hitAt = now + 0.25; x.actEnd = std::max(x.actEnd, x.hitAt + static_cast<double>(TM::castRecover));
            StrafeFrom(x, *tg, *blocker);
            return true;
        }
        // The 0x06 start test of a CLAIMED cast, when its charge would begin (Body::claimUntil). Blocked: the charge does not begin, the
        // claim waits (the same tallies as a held release). Clear: it begins, and the release is tested again.
        void StartTest(Body& x) {
            const SimSpell* s = SpellOf(x, x.actSpell); Body* tg = Find(x.actTarget);
            if (!s || !tg || !Up(*tg) || !LofGuarded(x, *s)) { x.castStarted = true; return; }
            const Body* blocker = LofBlocker(x, *tg, s->e.areaUnits); if (!blocker) { x.castStarted = true; return; }
            const bool subj = x.ctl == 0;
            if (now - x.claimAt >= 6.0) { if (subj) ++t.castLofExpired; x.hitAt = -1; x.actEnd = now + TM::castRecover; if (trace) std::fprintf(trace, "%7.2f  LOF %s gives up %s before it began: %s stood in the line for 6 s\n", now, x.c.name.c_str(), s->e.name.c_str(), blocker->c.name.c_str()); return; }
            if (subj) { if (!x.lofWaited) ++t.castLofHeld; t.castLofSec += 0.25; }
            if (trace && !x.lofWaited) std::fprintf(trace, "%7.2f  LOF %s may not begin %s: %s stands in the line to %s\n", now, x.c.name.c_str(), s->e.name.c_str(), blocker->c.name.c_str(), tg->c.name.c_str());
            x.lofWaited = true; x.claimUntil += 0.25; x.hitAt += 0.25; x.actEnd = std::max(x.actEnd, x.hitAt + static_cast<double>(TM::castRecover));
            StrafeFrom(x, *tg, *blocker);
        }
        // SIM V3 (hands): a concentration spell of a follower is a STREAM on his left hand. Length: cast/Actuation_internal.h DrawConcCap
        // (offense 2..6 s, heal and utility 8..15 s). beats = its effect and its cost come per 1 s beat (kConcApplyPeriod).
        void StartStream(Body& x, std::uint32_t target, int idx, bool iaus, bool beats, bool offense) {
            x.stream.live = true; x.stream.spell = idx; x.stream.target = target; x.stream.iaus = iaus; x.stream.beats = beats;
            x.stream.end = now + static_cast<double>(offense ? VU(x, 15, 2.0f, 6.0f) : VU(x, 16, 8.0f, 15.0f)); x.stream.nextBeat = now;
            x.actEnd = now; if (x.ctl == 0 && iaus) ++t.streams;
            if (trace) std::fprintf(trace, "%7.2f  STREAM %s: %s on his left hand for up to %.1f s\n", now, x.c.name.c_str(), SpellOf(x, idx)->e.name.c_str(), x.stream.end - now);
        }
        void StreamStep(Body& x) {
            if (!Up(x)) { x.stream.live = false; return; }
            if (x.ctl == 0) t.streamSec += TM::dt;
            if (now >= x.stream.end) { EndStream(x, false); return; }
            if (!x.stream.beats || now < x.stream.nextBeat) return;
            x.stream.nextBeat += 1.0;
            const SimSpell* s = SpellOf(x, x.stream.spell); Ctl& c = ctl[static_cast<std::size_t>(x.ctl)]; if (!s) { EndStream(x, false); return; }
            const float cost = 0.5f * c.book.entries[static_cast<std::size_t>(x.stream.spell)].cost;   // the lab's entry is a 2 s lump (Data.h): one beat is half of it
            if (cost > x.c.mp) { EndStream(x, false); return; }
            const bool subj = x.ctl == 0 && x.stream.iaus;
            Body* tg = Find(x.stream.target);
            if (s->e.nature == Prune::Nature::Heal) {
                Body* r = (tg && !s->e.selfDelivery && tg->alive) ? tg : &x;
                if (r->c.hp >= 0.9995f * r->c.hpMax || (r != &x && !Sees(x, *r))) { EndStream(x, false); return; }   // the heal stops at full (Actuation: 0.9995); an occluded recipient is released
                x.c.mp -= cost; x.regenAt[1] = now + D->rules.delayMp; if (subj) ++t.beats;
                Heal(*r, 0.5f * s->heal, &x, x.stream.iaus); return;
            }
            if (!tg || !Up(*tg)) { EndStream(x, false); return; }
            // cast/DirectTarget.cpp:234-239: every apply is re-checked. Occluded or a friend in the line = Declined, the stream goes stale and is cut.
            if (!Sees(x, *tg)) { if (subj) ++t.beatNoLos; EndStream(x, false); return; }
            if (LofGuarded(x, *s)) if (const Body* blocker = LofBlocker(x, *tg, s->e.areaUnits)) { if (subj) ++t.beatLof; StrafeFrom(x, *tg, *blocker); EndStream(x, false); return; }
            if (s->e.reach > 0.0f && Dist(x, *tg) > s->e.reach * 1.15f) { EndStream(x, false); return; }
            x.c.mp -= cost; x.regenAt[1] = now + D->rules.delayMp; if (subj) ++t.beats;
            SpellHit(x, *s, *tg, x.stream.iaus, 0.5f);
        }
        // SIM V3 (areas): every live area, once per step. A cloak follows its owner; the rest stay where they were cast.
        void AreaStep() {
            for (std::size_t i = 0; i < areas.size();) {
                Area& a = areas[i]; Body* o = Find(a.src);
                if (now >= a.until || !o || !o->alive) { areas[i] = areas.back(); areas.pop_back(); continue; }
                if (a.follows) { a.x = o->c.x; a.y = o->c.y; }
                const bool mine = o->ctl == 0; if (mine) t.areaUpSec += TM::dt;
                for (Body& q : b) { if (!Up(q) || std::hypot(q.c.x - a.x, q.c.y - a.y) > a.r) continue;
                    if (a.hostile) {
                        if (q.Party() != o->Party()) { if (mine) t.areaFoeSec += TM::dt;
                            const float el = a.av >= 0 ? std::clamp(ResistOf(q, a.av) / 100.0f, -1.0f, 1.0f) : 0.0f; Hurt(q, a.perSec * TM::dt * (1.0f - el) * (1.0f - std::clamp(ResistOf(q, kAvResistMagic) / 100.0f, -1.0f, 1.0f)), o, false); }
                        else if (&q != o && mine) t.areaAllySec += TM::dt;   // an ally stands in his hostile area (a cloak does not burn a friend; the tally is the positioning question)
                    } else if (q.Party() == o->Party()) q.armorBuff = std::max(q.armorBuff, a.armor);
                }
                if (mine && !a.hostile && !a.follows && Up(*o) && std::hypot(o->c.x - a.x, o->c.y - a.y) > a.r) t.areaOutSec += TM::dt;
                ++i;
            }
        }
        void ReleaseCast(Body& x, Body* tg) {
            const SimSpell* s = SpellOf(x, x.actSpell); if (!s) return;
            if (x.ctl == 0 && x.actIaus) { ++t.castFired; t.claimSec += now - x.claimAt; }
            if (x.ctl >= 0 && x.actIaus) { Ctl& rc = ctl[static_cast<std::size_t>(x.ctl)]; rc.lastFireForm = s->e.form; rc.lastFireTarget = x.actTarget; rc.lastFireAt = now; }   // the claim that stands (Data.h Delivery: repeat)
            const bool stream = g_v3.hands && x.ctl >= 0 && s->e.concentration && s->e.source == SpellSource::Spell && !s->reanimate;
            if (stream && (s->e.nature == Prune::Nature::Offense || s->e.nature == Prune::Nature::Control || s->e.nature == Prune::Nature::Heal)) {
                if (x.ctl == 0 && x.actIaus && s->e.nature != Prune::Nature::Heal && tg && Up(*tg) && !Sees(x, *tg)) ++t.castNoLos;
                StartStream(x, tg ? tg->c.id : x.c.id, x.actSpell, x.actIaus, true, s->e.nature != Prune::Nature::Heal); return; }
            // pay at the release (an interrupted charge costs nothing)
            if (x.ctl >= 0) { Ctl& c = ctl[static_cast<std::size_t>(x.ctl)]; SpellEntry& e = c.book.entries[static_cast<std::size_t>(x.actSpell)];
                if (e.source == SpellSource::Staff) { e.charge -= 1.0f; e.charged = e.charge > 0.5f; c.book.kit.staffCharged = e.charged; }
                else if (e.source == SpellSource::Scroll) { e.count = std::max(0, e.count - 1); if (x.ctl == 0) ++t.scrollsUsed; }
                else { x.c.mp = std::max(0.0f, x.c.mp - x.actCost); x.regenAt[1] = now + D->rules.delayMp; } }
            else { x.c.mp = std::max(0.0f, x.c.mp - x.actCost); x.regenAt[1] = now + D->rules.delayMp; }
            const bool iaus = x.actIaus; const Prune::Nature na = s->e.nature;
            if (stream) StartStream(x, x.c.id, x.actSpell, iaus, false, false);   // a concentration buff / utility: its effect below, then the hand is held for the stream (field 11-12, Auri)
            else if (g_v3.hands && TwoHandWeapon(x)) { x.reDrawUntil = x.actEnd + 0.65; if (x.ctl == 0) ++t.reDraws; }   // the bow / two-hander comes back after the cast (ASSUMPTION 0.65 s)
            if (g_v3.areas && s->areaKind != 0) {   // an ACTIVE area: a cloak moves with him, a circle / rune / wall stays at the cast point
                const std::uint32_t me = x.c.id; const int si = x.actSpell;
                if (x.ctl == 0 && iaus) { ++t.areaOwnCasts; for (const Area& a : areas) if (a.src == me) { ++t.areaRecast; break; } }
                for (std::size_t i = 0; i < areas.size();) { if (areas[i].src == me && areas[i].spell == si) { areas[i] = areas.back(); areas.pop_back(); } else ++i; }   // the same spell again replaces its area
                const double until = now + static_cast<double>(s->areaDur > 0.0f ? s->areaDur : 60.0f);
                areas.push_back({ x.c.x, x.c.y, s->e.areaUnits, until, me, s->areaKind == 1, s->areaDps > 0.0f, s->areaDps, s->areaArmor, s->e.resistAv, si });
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].active[static_cast<std::size_t>(si)].push_back({ me, until });
                if (trace) std::fprintf(trace, "%7.2f  AREA %s: %s, %.0f units, %s, %.0f s\n", now, x.c.name.c_str(), s->e.name.c_str(), s->e.areaUnits, s->areaKind == 1 ? "moves with him" : "fixed here", until - now);
                return;
            }
            if (s->reanimate) {   // card G7: the corpse stands up as his thrall for the spell's duration (MODEL: the dead foe's own spec at full health)
                if (!tg || tg->alive || tg->corpseUsed || tg->role != Role::Foe) { if (x.ctl == 0 && iaus) ++t.wastedCasts; return; }
                tg->corpseUsed = true; const double until = now + s->summonDur; const auto sp = tg->spec; const float cx = tg->c.x, cy = tg->c.y; const bool party = x.Party();
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].summonUntil[static_cast<std::size_t>(x.actSpell)] = until;
                if (x.ctl == 0) ++t.raises;
                if (trace) std::fprintf(trace, "%7.2f  RAISE %s raises %s (level %d) for %.0f s\n", now, x.c.name.c_str(), sp->name.c_str(), sp->level, s->summonDur);
                Body& n = Add(party ? Role::Summon : Role::Foe, sp, cx, cy); n.downUntil = 0; n.airUntil = -until;   // `x`, `tg` are stale after Add
                return;
            }
            if (na == Prune::Nature::Heal) {
                Body* r = (tg && !s->e.selfDelivery) ? tg : &x; if (!r->alive) r = &x;
                if (iaus && x.ctl == 0) { ++t.healCasts; }
                Heal(*r, s->heal, &x, iaus);
                if (s->hot > 0) r->fx.push_back({ 1, s->hot, -1, now + s->hotDur, x.c.id });
                return;
            }
            if (na == Prune::Nature::Buff || na == Prune::Nature::Ward) {
                Body* r = (tg && tg->Party() == x.Party()) ? tg : &x;
                // --buff-dur real: up for the record's duration (a record with none keeps the lab's 30 s, MODEL); off: max(30 s, duration) as before
                const float up = g_buffDurReal ? (s->buffDur > 0.0f ? s->buffDur : 30.0f) : std::max(30.0f, s->buffDur);
                if (s->armor > 0) r->fx.push_back({ 2, s->armor, -1, now + up, x.c.id });
                if (s->resistBuffAv >= 0) r->fx.push_back({ 3, s->resistBuff, s->resistBuffAv, now + up, x.c.id });
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].active[static_cast<std::size_t>(x.actSpell)].push_back({ r->c.id, now + (g_buffDurReal ? static_cast<double>(up) : std::max(30.0, static_cast<double>(s->buffDur))) });
                return;
            }
            if (na == Prune::Nature::Summon) {
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].summonUntil[static_cast<std::size_t>(x.actSpell)] = now + s->summonDur;
                auto sp = std::make_shared<ActorSpec>(); sp->name = "Summon"; sp->race = "AtronachFlameRace"; sp->level = x.c.level; sp->creature = sp->daedra = true;
                sp->hp = 60.0f + 12.0f * static_cast<float>(x.c.level); sp->unarmed = 8.0f + 0.8f * static_cast<float>(x.c.level); sp->hpR = 0; sp->mp = 0; sp->run = 400;   // MODEL: a level-scaled atronach
                const float ox = x.c.x + 150, oy = x.c.y + 60; const double until = now + s->summonDur; const bool party = x.Party();
                Body& n = Add(party ? Role::Summon : Role::Foe, sp, ox, oy); n.downUntil = 0; n.airUntil = -until;   // airUntil < 0 = expiry time of a summon
                return;
            }
            if (!tg) { if (x.ctl == 0 && iaus) ++t.wastedCasts; return; }
            if (x.ctl == 0 && iaus && (!Up(*tg) || !Sees(x, *tg))) { ++t.wastedCasts; if (Up(*tg)) ++t.castNoLos; }
            Shot sh{ now + (s->e.concentration || s->e.touch ? 0.0 : Dist(x, *tg) / TM::spellSpeed), x.c.id, tg->c.id, 0, x.actSpell, false, iaus, tg->c.x, tg->c.y, 1 };
            TryDodge(sh, sh.at - now, s->e.areaUnits > 0 || s->e.concentration || s->e.touch);
            shots.push_back(sh);
        }
        void Land(const Shot& sh) {
            Body* a = Find(sh.src); Body* d = Find(sh.tgt); if (!a) return;
            if (sh.spell < 0) {   // an arrow
                auto aimNote = [&](bool miss) { if (a->ctl < 0 || !g_t2.aim) return; Ctl& c = ctl[static_cast<std::size_t>(a->ctl)]; if (c.aimBudget < 0) return;
                    c.aimFlying = std::max(0, c.aimFlying - 1); c.aimWin[c.aimAt] = miss ? 1 : 0; c.aimAt = (c.aimAt + 1) % 10; c.aimN = std::min(10, c.aimN + 1); };   // H5: a launch with no hit event is a miss
                if (!d || !Up(*d)) { aimNote(true); return; }
                if (sh.dodged) { aimNote(true); return; }
                const float moved = std::hypot(d->c.x - sh.x, d->c.y - sh.y);
                if (!Sees(*a, *d) || (moved > 40.0f && !sh.corrected && RP(*a, 17, a->ctl >= 0 ? g_aimMiss : TM::aimMiss))) { aimNote(true); return; }
                if (g_aimScatter > 0.0f && a->ctl >= 0 && !sh.corrected && RP(*a, 18, g_aimScatter)) { aimNote(true); return; }   // --aim-scatter: the engine's own random offset as a flat miss chance (MODEL, default none)
                aimNote(false);
                if (a->ctl == 0) ++t.arrowsHit;
                Physical(*a, *d, sh.dmg, false, true, sh.fromIaus); return;
            }
            const SimSpell* s = SpellOf(*a, sh.spell); if (!s) return;
            if (!d) return;
            if (!s->e.concentration && !s->e.touch && (!Sees(*a, *d))) return;   // the bolt hit the pillar
            if (sh.dodged) return;   // he stepped out of it (TryDodge)
            const bool aimedMiss = !s->e.concentration && s->e.areaUnits <= 0 && Up(*d) && std::hypot(d->c.x - sh.x, d->c.y - sh.y) > 40.0f && RP(*a, 19, TM::aimMiss);
            if (s->e.areaUnits > 0) {   // everyone inside the blast, friend or foe (marth: never splash the party)
                if (a->ctl == 0 && sh.fromIaus) ++t.areaCasts;
                const float cx = Up(*d) ? d->c.x : sh.x, cy = Up(*d) ? d->c.y : sh.y;
                for (auto& o : b) { if (!Up(o) || &o == a) continue; if (std::hypot(o.c.x - cx, o.c.y - cy) > s->e.areaUnits) continue;
                    if (o.Party() == a->Party()) { if (a->ctl == 0 && sh.fromIaus) ++t.splashAllyHits; SpellHit(*a, *s, o, false, 0.5f); }   // friendly fire at half (MODEL)
                    else SpellHit(*a, *s, o, sh.fromIaus); }
            } else if (!aimedMiss) SpellHit(*a, *s, *d, sh.fromIaus);
        }
        // Begin a cast (engine AI or IAUS). cost = what the release will take.
        void StartCast(Body& x, int idx, const SimSpell& s, std::uint32_t target, float cost, bool fromIaus) {
            x.act = Act::Cast; x.actSpell = idx; x.actTarget = target; x.actCost = cost; x.actIaus = fromIaus;
            x.hitAt = now + s.castSec; x.actEnd = x.hitAt + TM::castRecover; x.c.caster = true; Action(x, dg.soundNormal);
            x.claimAt = now; x.claimUntil = now; x.lofWaited = false; x.reDrawUntil = 0; x.castStarted = false;
            if (x.ctl == 0 && fromIaus) ++t.castPick;
            if (x.ctl >= 0) {   // SIM V3: a follower's cast is a claim that Harbinger builds and the engine fires (iaus-sim-v3.md 4.2, 4.3)
                // hands: a stream has no charge of its own (the lab's 2 s is the lump of v2); MODEL 0.25 s to raise the hand
                const bool direct = g_v3.hands && s.e.concentration && s.e.source == SpellSource::Spell;   // a stream goes down the DIRECT road (CastTargetDirect / CastSelfDirect, Docs/CAST-DELIVERY.md): no claim to wait for
                const float charge = direct ? 0.25f : s.castSec;
                float lat = charge;
                if (g_v3.delivery && fromIaus) {
                    if (!x.ctrlLive && g_deliveryOld) {   // before fix/heal-claim-gates: the controller is INACTIVE, the behaviour tree builds no caster, the claim stands with no cast until the hold cap
                        x.hitAt = -1; x.actEnd = now + 6.0; x.claimUntil = x.actEnd; if (x.ctl == 0) ++t.castNotBuilt;
                        if (trace) std::fprintf(trace, "%7.2f  NOT BUILT %s: %s claimed on an inactive controller, no cast for 6 s\n", now, x.c.name.c_str(), s.e.name.c_str());
                        return; }
                    const int nat = s.e.nature == Prune::Nature::Heal ? 1 : (s.e.nature == Prune::Nature::Offense || s.e.nature == Prune::Nature::Control) ? 0 : 2;
                    if (!x.ctrlLive) { lat = std::max(charge, 1.0f); if (x.ctl == 0) ++t.castDirect; }   // main: the DIRECT road (ComposedCast.cpp:785; MODEL 1 s)
                    else if (g_deliverSec[0] >= 0.0f) { if (!direct) lat = std::max(charge, g_deliverSec[nat == 1 ? 1 : 0]); }   // --deliver-sec: the fixed figures of sim v3's first cut (a stream had no claim to wait for there)
                    else {
                        // THE CLAIMED ROAD, MEASURED (Data.h Delivery; the follower's own AI performs the cast, ComposedCast::Try). Draws come from V, never R.
                        const DeliveryRow& row = nat == 2 && s.areaKind != 1 ? Delivery::kUnsampled : Delivery::kRow[nat][s.e.concentration ? 1 : 0]; Ctl& dc = ctl[static_cast<std::size_t>(x.ctl)];   // the buff row is a CLOAK's only (Data.h)
                        const bool again = row.hasRepeat && dc.lastFireForm == s.e.form && dc.lastFireTarget == target && now - dc.lastFireAt <= static_cast<double>(Delivery::kRepeatWindowSec);
                        if (again && x.ctl == 0) ++t.castRepeat;
                        if (again) lat = std::max(charge, static_cast<float>(dc.lastFireAt - now) + Delivery::At(row.repeat, VU(x, 20, 0.0f, 1.0f)));   // the claim stands and has fired: the engine casts again on its own beat
                        else if (VP(x, 21, std::clamp(row.unfired * g_deliverUnfired, 0.0f, 1.0f))) {   // the claim closes with no fire: his body is the claim's for as long as it stood
                            const float stood = Delivery::At(row.stood, VU(x, 22, 0.0f, 1.0f));
                            x.hitAt = -1; x.actEnd = now + static_cast<double>(stood); x.claimUntil = x.actEnd; if (x.ctl == 0) { ++t.castUnfired; t.unfiredSec += stood; }
                            if (trace) std::fprintf(trace, "%7.2f  UNFIRED %s: the claim for %s stands %.1f s and closes with no cast\n", now, x.c.name.c_str(), s.e.name.c_str(), stood);
                            return; }
                        else lat = std::max(charge, Delivery::kPickToOpen + Delivery::At(row.first, VU(x, 23, 0.0f, 1.0f)));
                    }
                }
                x.hitAt = now + static_cast<double>(lat); x.claimUntil = x.hitAt - static_cast<double>(charge); x.actEnd = x.hitAt + TM::castRecover;
            }
            if (x.ctl >= 0 && s.e.source == SpellSource::Spell) { Ctl& hc = ctl[static_cast<std::size_t>(x.ctl)]; if (hc.handSpell != s.e.form) hc.handSpell2 = hc.handSpell; hc.handSpell = s.e.form; }   // H4: the spell now in his hand (MODEL: a hand keeps the last spell cast from it)
            if (trace) std::fprintf(trace, "%7.2f  cast %s begins %s -> %u (%s, %.1fs, cost %.0f, mp %.0f)\n", now, x.c.name.c_str(), s.e.name.c_str(), target, fromIaus ? "decision" : "own AI", s.castSec, cost, x.c.mp);
            if (x.role == Role::Foe && (s.e.nature == Prune::Nature::Offense || s.e.nature == Prune::Nature::Heal || s.ctrl)) ++t.foeCasts;
            if (static_cast<std::size_t>(idx) < x.spellReady.size()) x.spellReady[static_cast<std::size_t>(idx)] = x.actEnd + std::max(0.0f, s.recovery);
        }
        // The engine AI's own spell choice: a heal when someone on its side is hurt, else a damage spell it can pay for.
        [[nodiscard]] bool BrainCast(Body& x, Body* tg) {
            const std::size_t n = x.ctl >= 0 ? ctl[static_cast<std::size_t>(x.ctl)].build->spells.size() : x.spec->spells.size();
            if (x.spellReady.size() < n) x.spellReady.resize(n, 0.0);
            int best = -1; float bw = 0; std::uint32_t btgt = 0; float bcost = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const SimSpell* s = SpellOf(x, static_cast<int>(i)); if (!s || now < x.spellReady[i]) continue;
                float cost = s->e.cost;
                if (x.ctl >= 0) { const SpellEntry& e = ctl[static_cast<std::size_t>(x.ctl)].book.entries[i]; if (e.source != SpellSource::Spell) continue; if (!ctl[static_cast<std::size_t>(x.ctl)].gate.spellsOnClass) continue; cost = e.cost; }
                if (cost > x.c.mp) continue;
                float w = 0; std::uint32_t tid = 0;
                if (s->e.nature == Prune::Nature::Heal && x.role == Role::Foe) {   // foes heal themselves and each other; a follower's heals are IAUS's to cast
                    Body* r = nullptr; float worst = 0.5f;
                    for (auto& o : b) if (Up(o) && o.Party() == x.Party() && (&o == &x || s->healOther) && o.c.hp / o.c.hpMax < worst && Dist(x, o) < 2000) { worst = o.c.hp / o.c.hpMax; r = &o; }
                    if (r) { w = 60.0f * (1.0f - worst) * 4.0f; tid = r->c.id; }
                } else if (s->e.nature == Prune::Nature::Offense && tg && s->dmg + s->dot * s->dotDur > 0) {
                    const float d = Dist(x, *tg); if (!s->e.touch && d > s->e.reach && s->e.reach > 0) continue; if (s->e.touch && d > Reach(x)) continue;
                    if (!Sees(x, *tg)) continue;
                    if (LofGuarded(x, *s)) if (const Body* blocker = LofBlocker(x, *tg, s->e.areaUnits)) { StrafeFrom(x, *tg, *blocker); continue; }   // SIM V3: 0x06 says no, the AI's start is refused and it picks something else
                    if (tg->c.outOfMeleeReach && s->e.touch) continue;
                    w = s->dmg + s->dot * s->dotDur; tid = tg->c.id;
                }
                w *= RU(x, 24, 0.6f, 1.4f);
                if (w > bw) { bw = w; best = static_cast<int>(i); btgt = tid; bcost = cost; }
            }
            if (best < 0) return false;
            StartCast(x, best, *SpellOf(x, best), btgt, bcost, false); return true;
        }
        [[nodiscard]] float FleeBelow(const Body& x) const {   // AIDT confidence: 0 cowardly .. 4 foolhardy (MODEL thresholds)
            if (x.Party()) return 0.0f;
            switch (x.spec->conf) { case 0: return 0.75f; case 1: return 0.40f; case 2: return 0.20f; case 3: return 0.08f; default: return 0.0f; }
        }
        // Card G3's drive, one step: to the point at the run speed, then stay. MODEL: the drive is a movement package, so his feet keep
        // going through a weapon swap, a potion and a cast (upper-body actions; StepWorld calls this for a body busy with one of them).
        // Everywhere else in the lab those three still hold the body, as before the stealth model.
        void RunMove(Body& x, const Ctl& c) {
            if (std::hypot(c.runX - x.c.x, c.runY - x.c.y) > 40.0f) { Move(x, c.runX, c.runY, x.spec->run); Face(x, c.runX, c.runY); }
            if (x.ctl == 0) t.runSec += TM::dt;
        }
        void Brain(Body& x) {
            const Rules& r = D->rules; const Style& st = StyleOf(x);
            if (x.spec->dragon) {   // MODEL: 12 s on the wing, 15 s on the ground
                if (x.airUntil == 0 && x.groundUntil == 0) x.groundUntil = now + RU(x, 25, 6, 14);
                const bool inAir = g_v3.height ? x.airborne : x.c.outOfMeleeReach;
                if (inAir && now >= x.airUntil) { x.c.outOfMeleeReach = false; x.airborne = false; x.c.z = 0.0f; x.groundUntil = now + 15; }
                else if (!inAir && now >= x.groundUntil) { if (g_v3.height) TakeOff(x); else { x.c.outOfMeleeReach = true; x.airUntil = now + 12; } }
            }
            Ctl* c = x.ctl >= 0 ? &ctl[static_cast<std::size_t>(x.ctl)] : nullptr;
            if (x.role == Role::Player && x.potions > 0 && x.c.hp < 0.35f * x.c.hpMax) {   // the player model drinks (MODEL: 40% of his health, 1 s)
                --x.potions; Heal(x, 0.4f * x.c.hpMax, nullptr, false); x.act = Act::Drink; x.actEnd = now + 1.0; x.hitAt = -1; return; }
            // flee (a fear effect, or low health for the unconfident)
            Body* threat = nullptr; { float bd = 1e9f; for (auto& o : b) if (Up(o) && o.Party() != x.Party()) { const float d = Dist(x, o); if (d < bd) { bd = d; threat = &o; } } }
            if (!threat) return;
            if (now < x.fleeUntil || (x.c.hp < FleeBelow(x) * x.c.hpMax && now >= x.think) || now < x.retreatUntil) {
                if (now >= x.fleeUntil && now >= x.retreatUntil) { x.fleeUntil = now + RU(x, 26, 4, 7); x.c.fleeing = true; }
                Move(x, x.c.x + (x.c.x - threat->c.x), x.c.y + (x.c.y - threat->c.y), x.spec->run); Face(x, x.c.x + (x.c.x - threat->c.x), x.c.y + (x.c.y - threat->c.y));
                if (now < x.retreatUntil && x.ctl == 0) t.retreatSec += TM::dt;
                return;
            }
            x.c.fleeing = false;
            if (c && c->runPhase == 4) {   // batch I back-off (Concealment Kind::BackOff): the drive walks him to the point and DENIES NOTHING. MODEL: he
                RunMove(x, *c);            // moves every step, and a foe already inside his swing reach is still swung at when his blade is free
                const Body* cur = Find(c->steer ? c->steer : x.c.targetId);
                if (!(cur && Up(*cur) && !cur->Party() && now >= x.think && (c->drawn == PostureKind::Melee || c->drawn == PostureKind::Silver) && Dist(x, *cur) <= Reach(x) * 0.9f)) return;
            } else
            if (c && c->runPhase) { RunMove(x, *c); return; }   // card G3: the H3 drive owns his body (ch.24 on the point + the ch.7 deny): he runs there and holds, no swing, no shot
            if (c && now < x.holdUntil) { if (x.ctl == 0) t.holdSec += TM::dt; return; }   // G2's hold row: still and quiet (ch.1 movement block + ch.7 deny)
            if (c && now < c->moveUntil) {   // a reposition order: he goes there first (sprint = x1.5, stamina 8 / s: MODEL)
                const float gd = std::hypot(c->goalX - x.c.x, c->goalY - x.c.y);
                // A ch.24 regroup (moveFight) bounds his combat area to the anchor: outside it he walks back, but a foe already in
                // his swing reach is still fought (MODEL of "keeps fighting the whole way"), and inside 300 units of it the order is done.
                bool engaged = false;
                if (c->moveFight) { if (gd <= 300.0f) c->moveUntil = 0; else if (c->drawn == PostureKind::Melee || c->drawn == PostureKind::Silver) for (const Body& o : b) if (o.role == Role::Foe && Up(o) && Dist(x, o) <= Reach(x)) { engaged = true; break; } }
                if (now < c->moveUntil && !engaged && gd > 40.0f) { const bool sp = c->sprint && x.c.sp > 5.0f; if (sp) { x.c.sp -= 8.0f * TM::dt; x.regenAt[2] = now + D->rules.delaySp; }
                    Move(x, c->goalX, c->goalY, x.spec->run * (sp ? 1.5f : 1.0f)); Face(x, c->goalX, c->goalY); if (x.ctl == 0) t.moveOrderSec += TM::dt; return; }
                if (!engaged) c->moveUntil = 0;
            }
            if (c && g_v3.sidestep && now < x.strafeUntil && std::hypot(x.sgx - x.c.x, x.sgy - x.c.y) > 20.0f) { Move(x, x.sgx, x.sgy, x.spec->run); return; }   // SIM V3: the side step of a held caster
            // target
            Body* tg = Find(x.c.targetId);
            const bool taunted = now < x.tauntUntil;
            if (taunted) { Body* tb = Find(x.tauntBy); if (tb && Up(*tb)) tg = tb; else x.tauntUntil = 0; }
            else if (c && c->steer) { Body* s = Find(c->steer); if (s && Up(*s) && !s->Party()) tg = s; else c->steer = 0; }
            if (tg && x.role == Role::Foe && !taunted && !GroupHas(*tg)) tg = nullptr;   // its group lost him
            if (tg && x.role == Role::Foe && !taunted && g_t2.dnd && now < tg->dndUntil) x.retargetAt = now;   // --disengage-dnd: he is no valid target now, the foe selects again (PickTarget keeps him only when nobody else stands)
            if (!tg || !Up(*tg) || (tg->Party() == x.Party() && now >= x.frenzyUntil) || (!taunted && !(c && c->steer) && now >= x.retargetAt)) {
                tg = PickTarget(x); x.retargetAt = now + RU(x, 27, 3, 7);
            }
            if (!tg) return;
            if (x.c.targetId != tg->c.id) { x.c.targetId = tg->c.id; if (x.ctl == 0) ++t.tgtSwitches; }
            Face(x, tg->c.x, tg->c.y);   // a fighter faces his combat target (the view cone of Detect.h)
            const float d = Dist(x, *tg); const bool los = Sees(x, *tg);
            // mode: what the combat style and the kit favour at this distance
            const bool canCast = (x.ctl < 0 || engineCasts) && (x.ctl >= 0 ? !ctl[static_cast<std::size_t>(x.ctl)].build->spells.empty() && c->drawn != PostureKind::Melee && c->drawn != PostureKind::Bow && c->drawn != PostureKind::Silver : !x.spec->spells.empty());
            const bool hasRanged = x.spec->ranged.Has() && x.arrows > 0 && (!c || c->drawn == PostureKind::Bow);
            const bool hasMelee = !c || c->drawn == PostureKind::Melee || c->drawn == PostureKind::Silver || c->drawn == PostureKind::None;
            x.casterMode = canCast && st.wMagic >= 0.9f;
            x.rangedMode = !x.casterMode && hasRanged && (c ? true : (st.wRanged > st.wMelee || d > 700.0f || tg->c.outOfMeleeReach || !x.spec->melee.Has()));
            x.c.caster = x.casterMode;
            if (c && c->drawn == PostureKind::Staff && now >= x.think) {   // a drawn staff is fired by the engine AI while it has a charge
                for (std::size_t i = 0; i < c->book.entries.size(); ++i) { const SpellEntry& e = c->book.entries[i];
                    if (e.source != SpellSource::Staff || !e.charged) continue;
                    if (!los || d > 2000.0f) { Move(x, tg->c.x, tg->c.y, x.spec->run); return; }
                    if (LofGuarded(x, c->build->spells[i])) if (const Body* blocker = LofBlocker(x, *tg, c->build->spells[i].e.areaUnits)) { StrafeFrom(x, *tg, *blocker); break; }   // SIM V3: the staff's start is refused too
                    StartCast(x, static_cast<int>(i), c->build->spells[i], tg->c.id, 0.0f, false); x.think = x.actEnd + RU(x, 28, TM::thinkMin, TM::thinkMax); return; }
            }
            if (x.casterMode) {
                if (now >= x.think && BrainCast(x, tg)) { x.think = x.actEnd + RU(x, 29, TM::thinkMin, TM::thinkMax) / std::max(0.25f, st.off); return; }
                // nothing castable now: keep the standoff, or fall back to the blade when out of magicka (a dagger)
                bool anyAfford = false; { const std::size_t n = x.ctl >= 0 ? c->build->spells.size() : x.spec->spells.size();
                    for (std::size_t i = 0; i < n; ++i) { const SimSpell* s = SpellOf(x, static_cast<int>(i)); const float cost = x.ctl >= 0 ? c->book.entries[i].cost : (s ? s->e.cost : 1e9f);
                        if (s && s->e.nature == Prune::Nature::Offense && cost <= x.c.mp && !(x.ctl >= 0 && c->book.entries[i].source != SpellSource::Spell)) anyAfford = true; } }
                if (anyAfford || (x.Party() && x.c.mp > 0.15f * x.c.mpMax)) {
                    const float want = 750.0f;
                    float far = want * 1.4f;   // the lab's standoff before card H4
                    if (g_t2.range && c) { const float des = HandRange(x, *c, tg); if (des > 0.0f) far = std::min(far, des * Reposition::kCloseSlack); }   // H4 is a BOUND (apmf/RangeApproach.cpp, Reposition.h RangeToHold: "stay inside R"): it can make him close sooner (a touch spell, a short stream), it never holds him back from the engine's own standoff
                    if (!los || d > far) { if (g_t2.c8 && LeashTurn(x, c, *tg)) return; Move(x, tg->c.x, tg->c.y, x.spec->run); }
                    else if (d < 320.0f) Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f);
                    return;
                }
                x.casterMode = false;
            }
            if (x.rangedMode) {
                // 11-17 A3 (iaus/Candidates.cpp noApproach, scheduler/IausSource.cpp ApproachEligible): a foe his act.attack names past the chase cap is
                // SHOT AT from where he stands, never walked to (the cap limits movement, not shots). MODEL: with a clear line he shoots at any
                // distance the pick allowed (Tuning::bowWeaponRange); without one he waits.
                const bool far = g_v3.chase && c && c->steerNoApproach && c->steer == tg->c.id;
                if (far && !los) return;
                if (!far && (!los || d > r.rangedDist * 1.6f)) { if (g_t2.c8 && LeashTurn(x, c, *tg)) return; Move(x, tg->c.x, tg->c.y, x.spec->run); return; }
                if (d < r.rangedMin && st.fallback > 0.05f) { Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f); }
                if (now >= x.think) {
                    const Weapon& w = x.spec->ranged; float draw = w.anim == 9 ? TM::crossbowReload : TM::bowNock + r.bowDraw / std::max(0.3f, w.speed) * 0.6f + RU(x, 30, TM::bowHoldMin, TM::bowHoldMax) * 0.5f;
                    // --point-blank (GMST: the hold time has a minimum DISTANCE, 256, and an absolute minimum, 0.1 s; how the engine blends
                    // them is not decoded, so MODEL: under the distance the hold is the absolute minimum). The draw above already took its R.U.
                    if (g_pointBlank && w.anim != 9 && d < r.holdMinDist) draw = TM::bowNock + r.bowDraw / std::max(0.3f, w.speed) * 0.6f + r.holdAbsMin * 0.5f;
                    x.act = Act::Shoot; x.actTarget = tg->c.id; x.hitAt = now + draw; x.actEnd = x.hitAt + TM::bowRecover; x.actIaus = false; Action(x, dg.soundSilent);   // MODEL: a bow is Silent
                    x.think = x.actEnd + RU(x, 31, TM::thinkMin, TM::thinkMax) * 0.5f / std::max(0.25f, st.off);
                }
                return;
            }
            if (!hasMelee) return;
            // --foe-styles (Part C): a foe stepping back after its blow (CSCR Fallback Mult) moves away at the back-pedal speed, then re-engages
            if ((g_foeStyles || g_v3.stance) && x.role == Role::Foe && now < x.backUntil) { Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f); return; }
            // melee
            const float reach = Reach(x);
            if (c && c->leash > 0.0f && d > reach && Dist(x, b[0]) > c->leash && Dist(*tg, b[0]) > c->leash) { Move(x, b[0].c.x, b[0].c.y, x.spec->run); return; }   // ch.23: no pursuit past the leash
            // THE 128 APPROACH UP A LEDGE (field 11-17, scheduler/IausSource.cpp: when every foe is out of the blade's reach the offense approach is filed at the
            // touch standoff 128 on the foe's reference and the ENGINE paths it, "which is what climbs a tower"). MODEL: a ledge with a way up
            // (FightSpec::perchPath) is climbed once he is within 128 units of the point under it, along a path three times its height (ASSUMPTION: a
            // stair / ramp), at his run speed; he comes down the same way when he turns to another foe (StepWorld). A ledge with no way up stays out of reach.
            if (c && g_v3.height && g_mirror && tg->perch && tg->perchPath && c->steer == tg->c.id && std::hypot(tg->c.x - x.c.x, tg->c.y - x.c.y) <= 128.0f && x.c.z < tg->c.z) {
                x.c.z = std::min(tg->c.z, x.c.z + x.spec->run * TM::dt / 3.0f); x.climbAt = now; x.movedAt = now; if (x.c.z < tg->c.z) return; }
            else if (c && x.c.z > 0.0f && tg->perch && c->steer == tg->c.id) x.climbAt = now;   // on the ledge with him: he stays up
            float slotX = tg->c.x, slotY = tg->c.y; if (g_v3.stance) SlotPoint(x, *tg, reach * 0.8f, slotX, slotY);   // SIM V3: attackers spread around their target
            if (d > reach * 0.9f || tg->c.outOfMeleeReach) { if (!tg->c.outOfMeleeReach) Move(x, slotX, slotY, x.spec->run); else if (d > 400) Move(x, tg->c.x, tg->c.y, x.spec->run * 0.5f); return; }
            if (now < x.think) { if ((slotX != tg->c.x || slotY != tg->c.y) && std::hypot(slotX - x.c.x, slotY - x.c.y) > 60.0f) Move(x, slotX, slotY, x.spec->run * 0.55f); return; }
            const bool tgBlocking = now < tg->blockUntil, tgStag = now < tg->staggerUntil, tgCasting = tg->act == Act::Cast && tg->hitAt >= 0;
            const Weapon& w = x.spec->melee;
            Act kind = Act::Swing;
            // Before batch S the lab let only a class with a power row or share power attack on its own. Card C8 as built (apmf/AttackMix.cpp):
            // a class with NO ch.18 rule files nothing, so the engine draws its own power attacks; a rule only denies (MixGate).
            const bool mix = c && g_t2.c8;
            const bool engPower = mix || !c || c->gate.powerAttack || !c->gate.resolved;
            if (x.spec->shield && (tgCasting || tgBlocking) && RP(x, 32, std::clamp(0.18f * st.bash, 0.0f, 0.6f)) && x.c.sp >= 20) kind = Act::Bash;
            else if (engPower && x.c.sp >= PowerStamina(r, w) && RP(x, 33, std::clamp(0.12f * st.off * (c ? c->powerShare : 1.0f) * (tgBlocking ? 1.5f * st.powBlocking : 1.0f) * (tgStag ? 1.3f * st.powStaggered : 1.0f), 0.0f, 0.6f))) kind = Act::Power;
            if (mix && kind != Act::Bash) kind = MixGate(x, *c, kind, w);
            if (x.ctl == 0 && kind == Act::Power) ++t.engPower;
            StartSwing(x, *tg, kind, false);
            x.think = x.actEnd + RU(x, 34, TM::thinkMin, TM::thinkMax) / std::max(0.25f, st.off);
            // --foe-styles (Part C, MODEL mapping of a record value): after the blow a foe backpedals with chance = its combat style's close-range
            // Fallback Mult (CSCR[1], read from the world extract: Load.h Style::fallback), for U(0.5, 1.5) s; an aggressive style (low fallback,
            // high offensive mult: the think pause above) presses on. Followers keep their own package movement.
            if ((g_foeStyles || g_v3.stance) && x.role == Role::Foe && RP(x, 35, std::clamp(st.fallback, 0.0f, 0.9f))) x.backUntil = x.actEnd + RU(x, 36, 0.5f, 1.5f);
        }
        // Q-SIM-5 (marth 11-11: "why wouldnt a follower be able to move and do an action. I believe he should"): what his FEET do while
        // his hands are busy with a weapon swap, a potion or a cast. Every deliberate movement Brain() would make, and nothing else: no
        // target pick, no action, no draw from R. No slowdown is applied: no sourced engine value for one exists in Docs/research (MODEL,
        // question Q-S-1). --feet-stop never calls this.
        void Feet(Body& x) {
            Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
            if (c.runPhase) { RunMove(x, c); return; }
            if (x.ctl == 0) t.feetSec += TM::dt;
            if (g_v3.sidestep && now < x.strafeUntil) { if (std::hypot(x.sgx - x.c.x, x.sgy - x.c.y) > 20.0f) Move(x, x.sgx, x.sgy, x.spec->run); return; }   // SIM V3: the side step of a held caster
            const Body* threat = nullptr; { float bd = 1e9f; for (const Body& o : b) if (Up(o) && o.Party() != x.Party()) { const float d = Dist(x, o); if (d < bd) { bd = d; threat = &o; } } }
            if (!threat) return;
            if (now < x.fleeUntil || now < x.retreatUntil) {
                Move(x, x.c.x + (x.c.x - threat->c.x), x.c.y + (x.c.y - threat->c.y), x.spec->run); Face(x, x.c.x + (x.c.x - threat->c.x), x.c.y + (x.c.y - threat->c.y));
                if (now < x.retreatUntil && x.ctl == 0) t.retreatSec += TM::dt;
                return;
            }
            if (now < x.holdUntil) return;
            if (now < c.moveUntil) {
                const float gd = std::hypot(c.goalX - x.c.x, c.goalY - x.c.y);
                if (c.moveFight && gd <= 300.0f) c.moveUntil = 0;
                else if (gd > 40.0f) { Move(x, c.goalX, c.goalY, x.spec->run); Face(x, c.goalX, c.goalY); if (x.ctl == 0) t.moveOrderSec += TM::dt; return; }
            }
            const Body* tg = Find(c.steer ? c.steer : x.c.targetId); if (!tg || !Up(*tg) || tg->Party()) return;
            const float d = Dist(x, *tg); const bool los = Sees(x, *tg); const Rules& r = D->rules;
            if (x.casterMode) {   // the stance his last free step fought in (a caster with a blade drawn closes like a blade, also while he casts)
                float far = 750.0f * 1.4f; if (g_t2.range) { const float des = HandRange(x, c, tg); if (des > 0.0f) far = std::min(far, des * Reposition::kCloseSlack); }
                if (!los || d > far) { if (g_t2.c8 && LeashTurn(x, &c, *tg)) return; Move(x, tg->c.x, tg->c.y, x.spec->run); }
                else if (d < 320.0f) Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f);
            } else if (c.drawn == PostureKind::Bow) {
                if (!los || d > r.rangedDist * 1.6f) { if (g_t2.c8 && LeashTurn(x, &c, *tg)) return; Move(x, tg->c.x, tg->c.y, x.spec->run); }
                else if (d < r.rangedMin && StyleOf(x).fallback > 0.05f) Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f);
            } else if (c.drawn != PostureKind::Staff) {   // Melee, Silver, or bare hands (Brain's hasMelee)
                const float reach = Reach(x);
                if (c.leash > 0.0f && d > reach && Dist(x, b[0]) > c.leash && Dist(*tg, b[0]) > c.leash) { Move(x, b[0].c.x, b[0].c.y, x.spec->run); return; }
                if (d > reach * 0.9f && !tg->c.outOfMeleeReach) Move(x, tg->c.x, tg->c.y, x.spec->run);
            }
        }

        // ── one 30 Hz step of the world (the followers' decision layer is served by the caller's policy) ──
        void Regen(Body& x) {
            const Rules& r = D->rules;
            if (now >= x.regenAt[0]) x.c.hp = std::min(x.c.hpMax, x.c.hp + x.c.hpMax * x.spec->hpR / 100.0f * r.combatHp * TM::dt);
            if (now >= x.regenAt[1]) x.c.mp = std::min(x.c.mpMax, x.c.mp + x.c.mpMax * x.spec->mpR / 100.0f * r.combatMp * TM::dt);
            if (now >= x.regenAt[2]) x.c.sp = std::min(x.c.spMax, x.c.sp + x.c.spMax * x.spec->spR / 100.0f * r.combatSp * TM::dt);
            if (x.poiseMax > 0.0f && now >= x.staggerUntil) x.poise = std::min(x.poiseMax, x.poise + x.poiseMax * D->poise.regen * TM::dt);
            x.recentDmg *= 1.0f - TM::dt / 2.0f; x.armorBuff = 0;
            for (std::size_t i = 0; i < x.fx.size();) {
                Effect& e = x.fx[i];
                if (e.until <= now) {
                    if (e.kind == 6) { float& mx = e.av == 0 ? x.c.hpMax : e.av == 1 ? x.c.mpMax : x.c.spMax; float& cur = e.av == 0 ? x.c.hp : e.av == 1 ? x.c.mp : x.c.sp; mx -= e.v; cur = std::min(cur, mx); }
                    if (e.buff >= 0 && x.ctl >= 0) { const int bk = e.buff; bool still = false; for (const Effect& o : x.fx) still = still || (&o != &e && o.buff == bk && o.until > now);
                        ctl[static_cast<std::size_t>(x.ctl)].book.kit.buff[static_cast<std::size_t>(bk)].active = still; }
                    x.fx[i] = x.fx.back(); x.fx.pop_back(); continue; }
                if (e.kind == 0) Hurt(x, e.v * TM::dt, Find(e.src)); else if (e.kind == 1) Heal(x, e.v * TM::dt, nullptr, false); else if (e.kind == 4) x.c.mp = std::min(x.c.mpMax, x.c.mp + e.v * TM::dt); else if (e.kind == 5) x.c.sp = std::min(x.c.spMax, x.c.sp + e.v * TM::dt); else if (e.kind == 2) x.armorBuff = std::max(x.armorBuff, e.v);
                ++i;
            }
        }
        // Is the class's go-in rule (card C9) satisfied right now? true = he may stand in melee.
        [[nodiscard]] bool GoInOk(const Ctl& c, const Body& x) const {
            const float hp = x.c.hp / std::max(1.0f, x.c.hpMax), mp = x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 0.0f;
            auto partyOk = [&]() { for (const Body& o : b) if (o.Party() && o.role != Role::Summon && o.alive && o.c.hp < 0.01f * static_cast<float>(c.partyPct ? c.partyPct : 70) * o.c.hpMax) return false; return true; };
            auto cheapest = [&](Prune::Nature n) { float m = 1e9f; for (const auto& e : c.book.entries) if (e.nature == n && e.source == SpellSource::Spell) m = std::min(m, e.cost); return m; };
            switch (c.goIn) {
            case kGoNever: { for (const Body& o : b) if (o.role == Role::Foe && Up(o) && !Controlled(o)) return false; return true; }   // ruling Q10: melee only when ALL foes are controlled
            case kGoBias: return !(hp < 0.40f && mp > 0.30f);                                   // a low-health spellsword with magicka left goes back to spell range
            case kGoSummons: { bool knows = false, live = false; for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].nature == Prune::Nature::Summon) { knows = true; live |= c.summonUntil[i] > now; } return hp >= 0.70f && (!knows || live); }
            case kGoParty: return partyOk() || x.c.mp < cheapest(Prune::Nature::Heal);
            case kGoControlled: { const Body* tg = Find(c.steer ? c.steer : x.c.targetId); if (tg && Controlled(*tg)) return true;
                for (std::size_t i = 0; i < c.book.entries.size(); ++i) { const SpellEntry& e = c.book.entries[i]; if (e.nature != Prune::Nature::Control || e.cost > x.c.mp) continue;
                    for (const Body& o : b) if (o.role == Role::Foe && Up(o) && !Controlled(o) && WillWork(c.build->spells[i], o) == Tri::Yes) return false; }
                return true; }
            default: return c.partyPct > 0 && c.fam == Cls::Ranged ? partyOk() : true;            // Scout: the party-healthy rule, else he stays ranged
            }
        }
        // Where the subject stands and what state he is in, once per step (the class playstyle metrics).
        void PlaystyleTally(const Ctl& c, const Body& x) {
            ++t.posN; t.playerDistSum += Dist(x, b[0]);
            bool contact = false; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && Dist(x, o) < 260.0f) { contact = true; break; }
            if (contact) { t.contactSec += TM::dt; if ((c.goIn != kGoNone || (c.partyPct > 0 && c.fam == Cls::Ranged)) && !GoInOk(c, x)) t.goInBadSec += TM::dt; }
            if (const Body* tg = Find(c.steer ? c.steer : x.c.targetId); tg && Up(*tg) && !tg->Party()) { const float td = Dist(x, *tg); t.tgtDistSum += td; ++t.tgtDistN; t.minTgtDist = std::min(t.minTgtDist, static_cast<double>(td)); }
            for (std::size_t i = 0; i < c.summonUntil.size(); ++i) if (c.summonUntil[i] > now) { t.summonSec += TM::dt; break; }
            if (x.sneaking) t.sneakSec += TM::dt; if (now < x.cloakUntil) t.cloakSec += TM::dt;
            if (Unseen(x)) t.hiddenSec += TM::dt;
            t.drawnSec[c.drawn == PostureKind::Bow ? 0 : (c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver) ? 1 : c.drawn == PostureKind::None ? 2 : 3] += TM::dt;
            { float nd = 1e9f; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && o.c.targetId == x.c.id) nd = std::min(nd, Dist(x, o));
              t.nearOnMeSec[nd < 256.0f ? 0 : nd < 540.0f ? 1 : 2] += TM::dt; }
        }
        // --space R: the enclosed fight's disc, set ONCE before the first decision (review F1 of ca46a3e2: it was set in the first StepWorld,
        // after the first Serve at now = 0, so the first decision saw open-space distances). RunFight calls it before its loop.
        void SetArena() {
            if (!arenaSet) { arenaSet = true;   // --space R (Part C, MODEL): an enclosed fight is a disc of radius R around the start centroid; whoever starts
                // outside it starts at its edge (a room: the line of fire is no longer than 2R, backpedal / kiting / flight stop at the wall)
                const bool enclosed = g_spaceForce >= 0 ? g_spaceForce == 1 : interior;
                if (g_spaceSize > 0.0f && enclosed && !b.empty()) { float cx = 0, cy = 0; for (const Body& o : b) { cx += o.c.x; cy += o.c.y; } cx /= float(b.size()); cy /= float(b.size());
                    arenaX = cx; arenaY = cy; arenaR = g_spaceSize; const float in = std::max(0.5f * g_spaceSize, g_spaceSize - 50.0f);
                    for (Body& o : b) { const float ox = o.c.x - cx, oy = o.c.y - cy, od = std::hypot(ox, oy); if (od > in) { o.c.x = cx + ox * in / od; o.c.y = cy + oy * in / od; } } } }
        }
        // review F1: a body placed or moved outside the room (a late spawn, a dodge step) is put back inside it (a late spawn at the starting
        // ring, a dodge step stops at the wall like every other move). No room = nothing.
        void IntoRoom(Body& o, float r) { if (!(arenaR > 0.0f)) return; const float ox = o.c.x - arenaX, oy = o.c.y - arenaY, od = std::hypot(ox, oy); if (od > r) { o.c.x = arenaX + ox * r / od; o.c.y = arenaY + oy * r / od; } }
        void StepWorld() {
            SetArena();
            if (g_v3.burst && estimates && now >= burstAt) { burstAt += TM::pump; SampleBursts(); }
            for (std::size_t i = 0; i < b.size(); ++i) {
                Body& x = b[i]; if (!x.alive) continue;
                if (x.c.downed) { if (now >= x.downUntil) { x.c.downed = false; x.c.hp = std::max(x.c.hp, D->rules.bleedoutDefault * x.c.hpMax); } continue; }
                if (x.role == Role::Summon && x.airUntil < 0 && now >= -x.airUntil) { x.alive = false; continue; }
                Regen(x); if (!Up(x)) continue;
                if (x.airborne) FlightStep(x);   // SIM V3 (height): the circuit, in 3D
                if (x.ctl >= 0 && x.c.z > 0.0f && now - x.climbAt > 0.25) x.c.z = std::max(0.0f, x.c.z - x.spec->run * TM::dt / 3.0f);   // the climb MODEL: off the way up, he walks down
                if (g_v3.delivery && x.ctl >= 0 && !x.ctrlLive) {   // ASSUMPTION (iaus-sim-v3.md 4.2): the engine's detection activates his controller when a foe stands within 2000 units in his sight, or he is hit
                    if (x.lastAttacker) x.ctrlLive = true; else for (const Body& o : b) if (o.role == Role::Foe && Up(o) && Dist(x, o) < 2000.0f && Sees(x, o)) { x.ctrlLive = true; break; } }
                if (x.role == Role::Foe && now < x.fleeUntil && Dist(x, b[0]) > 4096.0f) { x.alive = false; ++t.escaped; if (trace) std::fprintf(trace, "%7.2f  ESCAPED %s (%u)\n", now, x.c.name.c_str(), x.c.id); continue; }   // a routed foe past the chase cap has left the fight
                if (now >= x.ctrlUntil) x.c.paralysed = false;
                if (now >= x.fleeUntil) x.c.fleeing = now < x.retreatUntil;
                if (now >= x.blockUntil) x.c.blocking = false;
                if (now >= x.tauntUntil) x.c.taunted = false;
                // field 1117 A2 (cast/HealRoad.cpp: "the recipient is at full health"): a heal claim is RELEASED when its recipient is full, unless a cast is
                // in flight at that mark (it fires, then the claim goes). In the lab: a decision heal whose charge has not begun (the claim still waits) is
                // dropped the step its recipient reads full (the stream's own 0.9995 line); a charge already running is let go. No magicka is spent.
                if (g_v3.delivery && g_mirror && !g_deliveryOld && x.ctl >= 0 && x.act == Act::Cast && x.actIaus && x.hitAt >= 0 && now < x.claimUntil) {
                    const SimSpell* hs = SpellOf(x, x.actSpell); const Body* hr = hs && hs->e.nature == Prune::Nature::Heal ? Find(x.actTarget) : nullptr;
                    if (hr && hr->c.hp >= 0.9995f * hr->c.hpMax) { x.hitAt = -1; x.actEnd = now; if (x.ctl == 0) ++t.healFullExit;
                        if (trace) std::fprintf(trace, "%7.2f  HEAL RELEASED %s: %s is at full health before the cast began\n", now, x.c.name.c_str(), hr->c.name.c_str()); } }
                if (g_v3.lof && x.ctl >= 0 && x.act == Act::Cast && x.actIaus && x.hitAt >= 0 && !x.castStarted && now >= x.claimUntil && now >= x.staggerUntil && now >= x.ctrlUntil) StartTest(b[i]);   // SIM V3: the guard's 0x06 answer for a claimed cast
                if (x.hitAt >= 0 && now >= x.hitAt && now >= x.staggerUntil && now >= x.ctrlUntil) Resolve(b[i]);
            }
            for (std::size_t i = 0; i < late.size();) { if (late[i].at <= now) { const Spawn sp = late[i]; late[i] = late.back(); late.pop_back(); Body& n = Add(Role::Foe, sp.spec, sp.x, sp.y); n.tag = sp.tag; n.dmgScale = sp.dmgScale; IntoRoom(n, std::max(0.5f * arenaR, arenaR - 50.0f));
                    if (!ctl.empty() && t.mpAtLate < 0) { const Body& s0 = b[static_cast<std::size_t>(ctl[0].body)]; t.mpAtLate = s0.c.mpMax > 0 ? s0.c.mp / s0.c.mpMax : 1.0; } } else ++i; }
            if (g_stealth > 0) {
                for (Body& x : b) if (x.stealthy && x.ctl >= 0) x.sneaking = Up(x) && now >= x.standUntil;   // G1 holds the stance all fight, the hide run included (a crouched run: the drive asks for the run speed, the stance is the claim's)
                if (now >= nextDetect) { nextDetect = now + static_cast<double>(dg.updateMin); DetectPass(); }
            }
            for (std::size_t i = 0; i < shots.size();) { if (shots[i].at <= now) { const Shot s = shots[i]; shots[i] = shots.back(); shots.pop_back(); Land(s); } else ++i; }
            if (g_v3.hands) for (std::size_t i = 0; i < b.size(); ++i) if (b[i].stream.live) StreamStep(b[i]);
            if (g_v3.areas && !areas.empty()) AreaStep();
            for (std::size_t i = 0; i < b.size(); ++i) { Body& x = b[i]; if (!Up(x)) continue;
                if (Busy(x)) {
                    if (g_v3.blockdir && x.act == Act::Block && now < x.blockUntil) { const Ctl* bc = x.ctl >= 0 ? &ctl[static_cast<std::size_t>(x.ctl)] : nullptr;   // SIM V3: behind his guard he keeps facing his combat target (MODEL)
                        if (const Body* bt = Find(bc && bc->steer ? bc->steer : x.c.targetId); bt && Up(*bt)) Face(x, bt->c.x, bt->c.y); }
                    if (x.ctl >= 0 && ((now < x.actEnd && (x.act == Act::Swap || x.act == Act::Drink || x.act == Act::Cast)) || x.stream.live || now < x.reDrawUntil) && now >= x.staggerUntil && now >= x.ctrlUntil) {
                        if (g_t2.feet) Feet(x);                                                           // Q-SIM-5: his feet go on under every deliberate movement
                        else if (ctl[static_cast<std::size_t>(x.ctl)].runPhase) RunMove(x, ctl[static_cast<std::size_t>(x.ctl)]);   // --feet-stop: only a G3 run did (section 30.2)
                    }
                    if (x.ctl == 0 && x.act == Act::Block && x.actIaus) t.guardSec += TM::dt;
                    continue; }
                if (x.act != Act::None) { x.act = Act::None; x.c.blocking = false; } Brain(b[i]); }
            // tallies that integrate over time
            Body& pl = Player(); bool pressed = false; bool anyFoe = false, anyParty = false;
            for (auto& x : b) { if (!Up(x)) continue; if (x.role == Role::Foe) { anyFoe = true; if (x.c.targetId == pl.c.id && Dist(x, pl) < 1500) pressed = true; } else if (x.role == Role::Player || x.role == Role::Follower) anyParty = true; }
            if (pressed) t.playerPressureSec += TM::dt;
            if (pl.alive) t.playerMinHp = std::min(t.playerMinHp, double(pl.c.hp / std::max(1.0f, pl.c.hpMax))); else t.playerMinHp = 0;
            if (!ctl.empty()) { const Body& s0 = b[static_cast<std::size_t>(ctl[0].body)]; if (Up(s0)) { t.upSec += TM::dt; const std::size_t ai = now < s0.actEnd ? static_cast<std::size_t>(s0.act) : 0; t.bodySec[ai] += TM::dt; if (ai && s0.actIaus) t.bodyIausSec[ai] += TM::dt; } }
            if (!ctl.empty()) { Ctl& c0 = ctl[0]; const Body& s0 = b[static_cast<std::size_t>(c0.body)];
                if (c0.runMeasured && c0.runSafeAt < 0) {   // section 31.9: the first moment after a G3 run began with no foe inside 400 units of him (20 s cap, or he went down)
                    bool near = false; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && Dist(o, s0) < 400.0f) { near = true; break; }
                    if (!Up(s0) || now - c0.runStart > 20.0) c0.runMeasured = false;
                    else if (!near) { c0.runSafeAt = now; ++t.runsSafe; t.runSafeSec += now - c0.runStart; c0.runMeasured = false; } }
                if (Up(s0)) PlaystyleTally(c0, s0);
                if (g_v3.hands && Up(s0) && TwoHandWeapon(s0) && (s0.stream.live || (s0.act == Act::Cast && now < s0.actEnd) || now < s0.reDrawUntil)) {   // the weapon hand lost (ClickUp 1dfcuabd22x)
                    const bool bow = c0.drawn == PostureKind::Bow; const Weapon& w = bow ? s0.spec->ranged : s0.spec->melee;
                    t.handLostSec += TM::dt; if (w.Has()) t.handLostDmg += TM::dt * WeaponDamage(D->rules, *s0.spec, w, false) * w.speed * static_cast<float>(bow ? D->G("fCombatDPSBowSpeedMult", 0.2) : D->G("fCombatDPSMeleeSpeedMult", 1.0)); }
                if (Up(s0)) { bool onHim = false; for (const Body& o : b) if (o.role == Role::Foe && Up(o) && o.c.targetId == s0.c.id) { onHim = true; break; }   // the decline record: only while a foe is on him
                    if (onHim) { const int k = std::clamp(static_cast<int>(s0.c.hp / std::max(1.0f, s0.c.hpMax) * 10.0f), 0, 9); t.decBandSec[k] += TM::dt; if (s0.recentDmg >= s0.c.hp) t.decLethal[k] += TM::dt; } }
                if (pressed && c0.pressureSince < 0) c0.pressureSince = now; if (!pressed) c0.pressureSince = -1;
                t.mpSum += s0.c.mpMax > 0 ? s0.c.mp / s0.c.mpMax : 1; t.hpSum += s0.c.hp / std::max(1.0f, s0.c.hpMax); t.spSum += s0.c.spMax > 0 ? s0.c.sp / s0.c.spMax : 1; ++t.resN;
                if (c0.gate.spellsOnClass && Up(s0)) { float cheapest = 1e9f; for (const auto& e : c0.book.entries) if (e.source == SpellSource::Spell && e.cost > 0 && e.drop == Prune::Drop::Keep) cheapest = std::min(cheapest, e.cost); if (cheapest < 1e8f && s0.c.mp < cheapest) t.mpDrySec += TM::dt; } }
            now += TM::dt;
            if (!anyFoe && late.empty()) { over = true; t.won = true; }
            else if (!pl.alive || !anyParty) { over = true; t.lost = true; }
            else if (now >= maxSec) over = true;
            if (over) t.dur = now;
        }
    };

}
