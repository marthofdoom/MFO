#pragma once
// iaus/Core.h -- the IAUS combat scorer's PURE core (card I2, Docs/research/2026-11/iaus-design.md sections 3-6).
//
// CREDIT: the IAUS idea for MFO (utility scoring in place of a priority table, "Auto mode") is LandingCrew's.
// The combination math is Dave Mark's Infinite Axis Utility System (GDC 2013/2015). Credit both in the release
// notes, README and Nexus page (card I10, memory credits-landingcrew-iaus).
//
// WHAT THIS IS: a library. Given a list of CANDIDATES (action, form, target) whose FACTS the caller has already
// read, it scores every candidate through its family's considerations, ranks them, and picks one with the
// follower's rapport sharpness, momentum and the commitment window. Nobody calls it yet (I6 wires the scan, I4
// generates the candidates and reads the facts, I8 fills the Profile). tools/iaus_harness drives it off line.
//
// PURE: standard headers only, no RE:: type, no engine call, no global state, no clock, no RNG state. Every input
// is in the arguments (a FormID is a plain std::uint32_t here). That is what lets the harness build it with g++ on
// Linux and lets I9 replay field logs through it. The engine-facing pieces (Cooldowns, Temperament, spdlog, the
// board snapshot, ResetAllState) are in iaus/Engine.h, and only there.
//
// TUNABLE AS DATA: every curve parameter, action weight and rapport/focus row lives in a Tuning value
// (DefaultTuning(), Catalogue.cpp, each with its reason). I9 tunes by editing that table, never the code.
//
// THREADING: none of its own. Every function is reentrant over its arguments. A Mind is per follower and is owned
// by whoever holds it (Engine.cpp keeps them in the g_followers worker domain, #4).
#include "iaus/ConsumableWorth.h"
#include "iaus/SpellWorthTable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MFO::Iaus {

    inline constexpr float kInf = std::numeric_limits<float>::infinity();
    inline constexpr float kDrinkHealthLine = 0.34f;     // Cons::DrinkNeed's midpoint (Catalogue.cpp Logi(-11, 0.34)): below it a health potion is a real option
    inline constexpr std::uint8_t kGuardLineFoes = 2;   // Q-S-5: taunted foes on the Knight line from which the guard outranks his blows

    // ── Identity ─────────────────────────────────────────────────────────────────────────────────────────
    // Action families (design section 2). APPEND ONLY: the value is in log lines I9 parses and in the
    // Cooldowns id (CooldownId below), so a renumber would make old logs lie.
    enum class Family : std::uint8_t {
        Attack = 0,
        PowerAttack,
        Taunt,
        Sneak,        // act.sneak exists (card G1, IausSource maps it): scored, never generated today
        Retreat,
        DrinkHealth,
        DrinkStamina,
        DrinkMagicka,
        Posture,      // the weapon-category layer (design 1.2), scored by DecidePosture, never by Decide
        CastOffense,
        CastControl,
        CastHeal,
        CastBuff,
        CastSummon,
        CastWard,
        Scroll,       // uses the set of Candidate::scrollOf (Offense / Heal / Buff) with the scroll terms
        Exploit,      // the act.exploit gambit row (I3): the offense set with exploit at w 2.0
        Regroup,      // close on the player or a healer WHILE FIGHTING (Harbinger ch.24 combat approach). Generated only
                      // once that road has its MFO client (Snapshot::regroupRoad), never today
        GiveGround,   // step back from a foe he cannot trade with, still facing it (needs the Harbinger point-centred combat
                      // area: Snapshot::groundRoad). Lab only until that road exists
        Block,        // raise the guard against a swing that is coming (needs the Harbinger leaf promote: Snapshot::blockRoad)
        Bash,         // bash a foe in reach, first of all one that is charging a spell (same road: Snapshot::bashRoad)
        Disengage,    // card G3: a sneak class a foe has found runs to a point no foe can see and crouches there (the H3
                      // disengage drive, Concealment.cpp). Generated only behind Snapshot::concealRoad with a point known
        BackOff,      // batch I (H4 + Mercenary): a short H3 run to a point farther from the foes, then he fights on. A mage inside the
                      // minimum range of his equipped spell, or a Mercenary two or more foes press. Behind Snapshot::backOffRoad + a point
        Step,         // batch I item 5 (gap G18): hidden with no blow ready, a short quiet H3 run toward a foe that does not detect him,
                      // to gain a strike (blade) or a line of fire (bow) and stay hidden. Behind Snapshot::stepRoad + a point
        DrinkBuff,    // feat/buff-potions (ClickUp 1dfcuabd18f): a carried potion whose effect is not a restore (resist, fortify, regen,
                      // invisibility). One candidate per kind he carries (iaus/PotionBuff.h); Candidate::form = the potion, Facts::buffKind = the kind
        Decast,       // S4 (iaus/Light.h): end a light of ours on somebody who wants the dark. form = the light spell, target = who wears it. Generated only behind
                      // Snapshot::decastRoad (never today)
        kCount
    };
    inline constexpr std::size_t kFamilies = static_cast<std::size_t>(Family::kCount);

    // The posture layer's weapon categories. Bow covers bow and crossbow; Staff counts as ranged for mages
    // (kStaff) but has its own charge veto.
    enum class PostureKind : std::uint8_t { None = 0, Melee, Bow, Staff, Silver };

    // What a class profile's row bump (section 4.9) narrows to: the row's SpellRole for spells, the weapon
    // category for postures. APPEND ONLY (I8 data indexes it).
    enum class RoleTag : std::uint8_t {
        None = 0, Fire, Frost, Shock, OtherDamage, Heal, Buff, Ward, Summon, Control,
        Melee, Ranged, Staff, Silver,
        kCount
    };
    inline constexpr std::size_t kRoleTags = static_cast<std::size_t>(RoleTag::kCount);

    enum class Sight : std::uint8_t { None = 0, Visible, Unknown, Occluded };   // None = not a sighted action
    enum class Tri : std::uint8_t { Unknown = 0, Yes, No };                     // Unknown always PASSES

    // ── Considerations (design section 3) ────────────────────────────────────────────────────────────────
    // APPEND ONLY (Tuning::curve is indexed by it; [iaus-why] prints its name).
    enum class Cons : std::uint8_t {
        // shared (3.1)
        RangeMelee = 0, RangeSpell, Los, FoeThreat, FoeOnMe, FoeOnPlayer, FoeOnAlly, PlayerTarget, CurrentTarget,
        FoeFinish, Affordable, MpReserve, CooldownHard, CooldownSoft, Variety, WillWork, TargetCond, AlreadyActive,
        HandFree, SplashSafe, Exploit, ClassFit,
        // per action (3.2)
        FoeBlocking, SelfStamina, FoeStronger, FoeTaunted, TauntSelfHp, Unseen, FightStart, RetreatHp,
        NoHealRetreat, DisengageClass, DrinkNeed, Stock, NoHealDrink, PostureRanged, PostureMelee, Ammo,
        StaffCharge, MeleeReach, Unholy, MundaneVsUnholy, ClassWeapon, AreaValue, FoeControlled, FoeCountControl, HealNeed,
        HealReach, BuffEarly, BuffMelee, SummonLive, SummonFoes, WardCaster, WardHp, ScrollStock, ScrollMpLow, FoeEngaged,
        // multipliers (> 1 when the fact holds; outside the compensated product)
        BoostCaster, BoostDowned, BoostOutnumbered,
        // estimates (iaus/Estimate.h, the damage-estimate part of card I8). Each one is NOT APPLICABLE while its
        // inputs are unmeasured, so a follower without estimates scores exactly as before.
        DamageValue,        // this spell's expected damage on this target against his best option on it
        Overkill,           // the target's remaining health against that expected damage
        TauntSurvive,       // seconds he would live with this foe on him as well
        RetreatDanger,      // low health, or seconds to live at the present incoming damage
        HealSize,           // how much of the heal the recipient can use
        BoostPlayerDying,   // multiplier: the player has seconds to live
        BoostRecipientDying,   // multiplier: the heal's recipient has seconds to live
        RangeChase,         // ruling Q70: a melee ATTACK names a target anywhere in chase range (distance shades, the cap vetoes)
        BoostControlled,    // multiplier: a blow on a foe that cannot answer (PARALYSED only: Facts::foeHelpless)
        TauntChance,        // ruling Q69: the stat-based chance this taunt works on this foe
        HealClaimed,        // another follower's heal is already in flight on this recipient
        HealUrgency,        // seconds the recipient has left at the damage he is taking
        ControlWorth,       // how long this control holds the foe, and how much of the foe is left to hold
        MagickaBuys,        // magicka potion: the casts the fight still asks of him
        RangeTaunt,         // the taunt's reach: inside it every distance is the same (a shout, not an aimed cast)
        MortalTaunt,        // multiplier (<= 1): a follower who dies for good does not pull a foe he cannot outlast
        BoostMortalDanger,  // multiplier: a follower who dies for good backs off / drinks sooner
        PostureTrade,       // the melee category: seconds he lives at the damage coming in (do not draw steel to trade with a brute)
        RegroupNeed,        // regroup: the player is being killed, or he is and the anchor is away from it, or he is hurt and it heals
        RegroupFar,         // regroup: how far the anchor is (already beside it = nothing to do)
        GroundNeed,         // give ground: he is being beaten by something he cannot trade with, or it is closing on a ranged fighter
        BlockNeed,          // block: a foe's swing at him is in the air, and how much it matters
        BashNeed,           // bash: the foe is charging a spell (an interrupt), else it is on him
        BoostStrike,        // multiplier: a weapon blow on a foe that does not detect him while he sneaks (G2 Layer 1: the engine's own sneak attack)
        HoldStill,          // gate: the hold row (G2 Layer 1) stands only while no blow from stealth is ready
        DisengageNeed,      // disengage (card G3): a sneak class = the share of the foes that have found him; any other class = the
                            // retreat's own RetreatDanger (low health under threat)
        BoostFleeHeal,      // multiplier: a heal or a health potion for HIMSELF while a non-sneak flee runs (heal as soon as possible)
        BackOffNeed,        // back off (batch I): a mage = how far inside the minimum range of his equipped spell a foe on him stands;
                            // a Mercenary = how many foes press him (two or more)
        BoostGuardLine,     // multiplier x2.0 (APPENDED, Q-S-5): the Knight line holds two or more taunted foes and a swing at him is in the air:
                            // the guard outranks his own blows (marth: the line blocks at least 60% of the attacks aimed at it then)
        BuffThreat,         // buff potion (resist kinds): the share of the foes whose incoming element is the one this potion resists
        BuffStealth,        // buff potion (invisibility): gate, he sneaks or is a sneak class (stealth intent)
        BoostBuffThreat,    // multiplier (APPENDED, batch 3 "sim decides the edge"): a buff cast, by the foes in the fight (x = min(foes, 4) / 4).
                            // Ships FLAT 1.0 at every x (bit-identical); a class row's curve override is the searched knob (iaus-sim-v2.md 35)
        BoostPosturePerks,  // multiplier (APPENDED, batch 3 part C): a weapon posture by its side's share of his weapon-damage perk multipliers (Facts::perkShare).
                            // Ships FLAT 1.0 (bit-identical); NOT APPLICABLE while Snapshot::perks is unread (the game today)
        ConsumableWorth,    // (APPENDED, feat/consumables, marth 2026-11-13): is this potion / food worth its gold, needed enough here to win (iaus/ConsumableWorth.h).
                            // NOT APPLICABLE (flat-safe) while Facts::consumableWorth < 0; a veto below WorthTable::floor
        BoostSpellPerks,    // multiplier (APPENDED, ClickUp 1dfcuabd2pk, marth 11-12 "what spells to use ... it needs to read perks like potions"): a spell
                            // cast by what his perks do for that spell's SCHOOL (Facts::spellPerkEdge = 1 - cost multiplier / magnitude multiplier,
                            // iaus/Snapshot.h PerkSpellEdge). NOT APPLICABLE while Snapshot::perks.spellsRead is false (the sim today) or the cast is a scroll / staff
        HandLost,           // (APPENDED, ClickUp 1dfcuabd22x, field 11-12 Auri): a CONCENTRATION buff / ward holds his left hand for the whole channel, so a weapon that
                            // needs that hand (bow, crossbow, two-hander) is put away. x = the share of the top foe's current health his weapon would have dealt in
                            // that time (Facts::handLost, iaus/Generate.cpp HandLostShare). NOT APPLICABLE (bit-identical) for any other candidate, a fire-and-forget
                            // spell, a follower whose weapon does not need that hand, or while his weapon damage is unmeasured
        HealRate,           // (APPENDED, field 11-17b Jesper "when he does choose to heal it's a weak spell"): a heal by what it MENDS PER SECOND of cast against his BEST castable
                            // heal on that recipient (Facts::healRate / Facts::bestHealRate, the bestDamage pattern): heals are ranked among themselves and healing as a family is never
                            // penalised. HealSize stays the overheal brake (a heal bigger than the missing health). NOT APPLICABLE while the heal is unmeasured
        // MOVEMENT (S1, movement-plan.md): considerations over a (action, SPOT) pair, scored by Spots.cpp ScoreSpot from SpotFacts, in no
        // action family's own set. Every curve is linear / piecewise / boolean (no exp, no pow): gcc and MSVC must rank alike.
        SpotPath,           // seconds of walking to the spot
        SpotSight,          // the target sees the spot (an engine ray's answer). Unknown = NOT APPLICABLE, never clear
        SpotExposure,       // the share of his threats (those answered) that see the spot
        SpotFireLine,       // no ally's body in the lane from the spot to the target
        SpotRange,          // the action's range band at the spot
        SpotPathExposed,    // seconds of the walk a threat can see
        SpotExits,          // routes out that do not lead through a foe
        SpotAllyDist,       // distance to the nearest ally's chosen place
        SpotHostileArea,    // the spot lies in an active hostile area
        SpotLeash,          // distance to the anchor / the leash. Past 1: veto
        SpotClimb, SpotDrop,   // height gained / lost on the way
        SpotDetected,       // how detected he would be there (S4 fills it: not applicable until then)
        SpotLight,          // how lit the spot is, for a member who wants the dark (input hook: PartyPicture::light)
        SpotFoeReach,       // melee: can he walk to the target at all
        SpotLane,           // his body or lane crosses another member's chosen lane
        SpotPortalBusy,     // the walk goes through a doorway another member's chosen place stands in
        SpotServes,         // a movement action (retreat, regroup ...) served by a spot (1) or by standing still (0)
        BoostSpotCover,     // multiplier: from the spot he sees a foe that threatens an ally who has his place (cover each other)
        SpotRegionTrust,    // (APPENDED, S3) how far the mesh round the spot is trusted (iaus/EnvTrust.h): 1 doubted, 2 distrusted. NOT APPLICABLE while trusted
        SpotPathTrust,      // (APPENDED, S3) ... and the walk to it
        // LIGHT (APPENDED, S4, iaus/Light.h). NOT APPLICABLE while its fact is unread (the game until G6).
        LightVeto,          // gate: a light buff on somebody who wants the dark
        LightNeed,          // a light buff: how dark it is where the recipient stands (the level). Past the curve's zero: he is lit already
        LightRank,          // a light buff: whom (Light::RecipientRank). The player first, then members in his view
        SelfLight,          // the hidden hold: how lit he stands
        LightOnStealth,     // decast: one of our lights is on somebody who wants the dark
        SpotPathLight,      // seconds of the walk over lit ground, for a member who wants the dark
        kCount
    };
    inline constexpr std::size_t kCons = static_cast<std::size_t>(Cons::kCount);

    // How a consideration takes part in the combination (section 4, and why in Catalogue.cpp):
    //   Shape  -- a real [0,1] utility, compensated and multiplied, counted in n.
    //   Gate   -- 0 or 1 only. 0 vetoes; 1 is NOT counted in n (a passing gate carries no information, and
    //             counting it would raise every other term's compensation).
    //   Boost  -- a multiplier >= 1 applied outside the compensated product (x1.15 caster, x1.4 downed ...).
    enum class Role : std::uint8_t { Shape = 0, Gate, Boost };

    [[nodiscard]] std::string_view Name(Family a_f);
    [[nodiscard]] std::string_view Name(Cons a_c);
    [[nodiscard]] std::string_view Name(PostureKind a_p);
    [[nodiscard]] Role RoleOf(Cons a_c);

    // ── Curve library (section 3) ────────────────────────────────────────────────────────────────────────
    // Lin(m,b)   = m x + b
    // Pow(k)     = x^k                      (x clamped to [0,1])
    // Logi(k,x0) = 1 / (1 + e^(-k (x - x0)))   (k < 0 = falling)
    // Inv(k)     = 1 - x^k                  (x clamped to [0,1])
    // Bin(lo,hi) = x >= 0.5 ? hi : lo       (booleans arrive as 0 / 1)
    // Piece      = linear through up to 6 (x, y) points, flat outside them
    // then: x > zeroAbove or x < zeroBelow -> 0 (the Step veto), else max(y, floor), and a Shape/Gate result is
    // clamped to [0,1]. Bin(lo, hi) with hi > 1 is how a Boost is written.
    enum class CurveKind : std::uint8_t { Lin = 0, Pow, Logi, Inv, Bin, Piece };

    struct Curve {
        CurveKind kind = CurveKind::Lin;
        float a = 1.0f;                 // Lin m | Pow k | Logi k | Inv k | Bin lo
        float b = 0.0f;                 // Lin b | Logi x0 | Bin hi
        float floor = 0.0f;             // minimum output when not vetoed (shaping, never a veto)
        float zeroAbove = kInf;         // Step veto above
        float zeroBelow = -kInf;        // Step veto below
        std::array<float, 6> px{};      // Piece points, ascending x
        std::array<float, 6> py{};
        std::uint8_t np = 0;
    };

    [[nodiscard]] float Eval(const Curve& a_c, float a_x);   // raw curve value (Boost may exceed 1)

    [[nodiscard]] Curve Lin(float m, float b, float floor = 0.0f);
    [[nodiscard]] Curve Pow(float k, float floor = 0.0f);
    [[nodiscard]] Curve Logi(float k, float x0, float floor = 0.0f);
    [[nodiscard]] Curve Inv(float k, float floor = 0.0f);
    [[nodiscard]] Curve Bin(float lo, float hi);
    [[nodiscard]] Curve Piece(std::span<const std::array<float, 2>> pts);
    [[nodiscard]] Curve VetoAbove(Curve c, float t);
    [[nodiscard]] Curve VetoBelow(Curve c, float t);

    // ── Tuning (all data; DefaultTuning() in Catalogue.cpp documents each value) ───────────────────────────
    struct Sharpness {        // section 5.1, one row per rapport rank
        int   k = 1;          // pool size
        float floor = 1.0f;   // pool admits S >= floor x S_best
        float tau = 0.0f;     // softmax temperature; 0 = always the best
    };

    // Per-taunter act.taunt cooldown, seconds, from the ATTEMPT (marth Q69: "a 30s cool down"). The ONE constant: Taunt::Fire
    // and Tuning::rowlessTauntCooldownSec both use it. (Declared here, before Tuning, which needs it; SharedCooldownId is below.)
    inline constexpr float kTauntCooldownSec = 30.0f;

    // MOVEMENT (S1): what kind of place a spot is. The rows (name, generator, families, needs) are iaus/Spots.h kSpotKinds. APPEND ONLY:
    // per-class weights, the class masks and iIausSpotKinds index it.
    enum class SpotKind : std::uint8_t { Stay = 0, Firing, Cover, HighGround, Chokepoint, Flank, HealReach, Peel, Kite, Regroup, LootRoute, RetreatExit, ReachUnreach, kCount };
    inline constexpr std::size_t kSpotKindCount = static_cast<std::size_t>(SpotKind::kCount);
    inline constexpr float kMoveDecisionFloorSec = 0.5f;   // marth: no movement decision faster than 500 ms. The reader (MoveDecisionSec) clamps

    struct Tuning {
        std::array<Curve, kCons> curve{};
        std::array<float, kFamilies> actionWeight{};
        std::array<Sharpness, 5> sharp{};      // rank I..V
        std::array<float, 5> focus{};          // player focus m, rank I..V (ruling Q35)

        float momentum = 1.25f;                // fIausMomentum: the running action decision
        float postureMomentum = 1.5f;          // the running weapon category
        float preemptMargin = 0.25f;           // fIausPreemptMargin: commitment window
        float actFloor = 0.20f;                // fIausActFloor: below it IAUS issues nothing
        // PLACEHOLDER (sim N6 to search it): a chase-waived foe's RangeSpell x is clamped to this for a ranged act.attack. 0.85 is the
        // Logi(-10, 0.85) midpoint (0.5 on the curve), the parity of the melee waived foe's 0.5 on RangeChase at its cap edge.
        float waivedRangeX = 0.85f;
        // PLACEHOLDER (sim to search it; 11-17 A3): the distance a bow / crossbow in hand shoots at. A foe inside it stays in the foe list past the chase cap
        // (marked Combatant::noApproach): the cap limits where he MOVES, not what he shoots. Read only through Reposition::InWeaponRange.
        float bowWeaponRange = 3000.0f;
        float tieEps = 0.005f;                 // |dS| below this is a tie (section 4.8)
        int   maxTries = 6;                    // the walk cap per tick (design 1.1)
        int   maxCandidates = 384;             // iIausMaxCandidates
        float budgetMs = 0.5f;                 // fIausBudgetMs (Engine warns, never cuts)
        float softCooldownSec = 4.0f;          // CooldownSoft recovery T for the same spell
        float logRepeatSec = 10.0f;            // [iaus] line: on change, plus every N s while running
        // ruling Q34 (class_fit): an off-class ability granted by a MASTERED class scores offClassBase, and an
        // emergency lifts it by offClassEmergencyLift x emergency, capped at offClassCap.
        float offClassBase = 0.30f;
        float offClassEmergencyLift = 0.45f;
        float offClassCap = 0.80f;
        // Q34's literal last sentence, read strictly ("only abilities from mastered classes are valid in
        // emergencies", on-class ones too). OFF by default: see Catalogue.cpp and the I2 report.
        bool  emergencyOnlyMastered = false;
        float emergencyStrictAt = 0.5f;        // emergency level at which the strict reading bites
        float emergencyHpHigh = 0.35f;         // EmergencyLevel: 0 at or above this party-worst HP pct
        float emergencyHpLow = 0.10f;          //                1 at or below this
        // Both compare against Decision::bestHeal = the best heal's score DIVIDED BY its family's action weight. The values
        // are the doc's 0.30 / 0.50 over the heal weight they were written against (1.2), so they mean what they meant.
        float noHealRetreat = 0.25f;           // Retreat vetoed while a heal candidate scores above this
        float noHealDrink = 0.4167f;           // Health drink vetoed while a heal candidate scores above this
        // Card I6: the hard cooldown of a ROW-GATED action whose class row declares none (ClassGate::*CooldownSec
        // == 0). The power-attack rows all declare 10 s; no taunt row declares one (the floor is the 30 s of Q69), and with the class tree OFF
        // (marth Q63: nothing is class-gated) there is no row at all. Without a floor the sim's Knight taunts
        // 12-20 times a minute. One taunt ATTEMPT is one idle play plus a roll, and a hit already blocks the next
        // taunt for fTauntDuration (15 s) in the executor, so the floor only spaces MISSES. Tunable (I9).
        float rowlessPowerCooldownSec = 10.0f;
        // KEEP THE CURRENT ATTACK UNDER OCCLUSION (marth Q93): the attack / power attack on the foe he is ALREADY on
        // keeps its los term (Visible) while the foe has been occluded for less than this. A new pick or another foe
        // takes the full occluded penalty. Sized from the real cadence: a service lap is one round-robin turn
        // (party size x 133 ms, about 0.3-1 s) and the sight line is re-measured per lap, so 4 s is about 4-12 laps,
        // long enough to ride out a pillar or a doorframe and short enough that a real hide-and-seek re-picks.
        // 0 = off. Tunable in the sim.
        float occlusionHoldSec = 4.0f;
        float rowlessTauntCooldownSec = kTauntCooldownSec;   // marth Q69: 30 s, the executor's own cooldown
        // HAND LOST (ClickUp 1dfcuabd22x): how long a concentration buff / ward holds his hand, and what putting the weapon away costs. The stream's own cap is a
        // random draw per stream (cast/Actuation_internal.h DrawConcCap: 8-15 s for a heal / utility / self buff), so the mean is 11.5 s. The redraw is the sim's
        // ASSUMPTION (0.65 s, half of the v2 swap time, iaus-sim-v3.md 4.3). PLACEHOLDERS: the sim decides them. `t.concChannelSec` / `t.handRedrawSec`.
        float concChannelSec = 11.5f;
        float handRedrawSec = 0.65f;
        // 11-17b (field Jesper): the rate a spell is compared at is its effect per second OF CAST. A concentration stream's effect is already per second (Fills.cpp
        // counts it for 1 s); a fire-and-forget cast's total is divided by this. PLACEHOLDER (engine cast times differ per spell and are not read yet): `t.spellCastSec`.
        float spellCastSec = 1.5f;
        // 11-17b: his OWN heal gets the dying boost (Cons::BoostRecipientDying) below this health fraction even while the time-to-live is unknown (nothing seen
        // hitting him: a burst takes the rest in one blow). PLACEHOLDER, not searched: `t.dyingSelfHp`.
        float dyingSelfHp = 0.25f;
        // 11-17g (field heal-timing): the same fallback for an ALLY (the unknown time-to-live was a hole for a recipient who is not him: Cicero died at 8 %
        // with the flag off). PLACEHOLDER, not searched: `t.dyingAllyHp`.
        float dyingAllyHp = 0.25f;
        // 11-17g: bursts (single hits of 30 to 97 % of the maximum) defeat a rate-based time-to-live. The dying boost also fires when the recipient has
        // this many of his largest recent burst left (1 = the next such burst downs him; 1.2 so a burst that nearly downs him at full health counts, Jesper 172 vs 166 =
        // 1.036), and the heal urgency is the lower of ttl / 10 and hitsToDown / the divisor.
        // PLACEHOLDERS, not searched: `t.dyingHitsToDown`, `t.urgencyHitsDiv`.
        float dyingHitsToDown = 1.2f;
        // 11-17g: the windows of the gross reads (Fills.cpp, Estimate::BurstReads). Sized from the cadence (principle 9): a lap is 133 ms x party (about 1.3 s for
        // ten), so 4 s holds three laps of a party of ten and the largest burst is held 8 s (still counted when the next heal lands). PLACEHOLDERS:
        // `t.grossWindowSec`, `t.maxHitWindowSec`. Read from LiveTuning() on the same (worker) thread that writes it. A "lap" for an ally is one SAMPLE interval
        // (Fills kLossMinDtSec 0.10 s: every follower's snapshot samples every ally), so a volley spread over 0.2-0.4 s lands in 2-3 samples: maxHit is the largest
        // SUM of samples inside `burstWindowSec` (PLACEHOLDER, `t.burstWindowSec`).
        float grossWindowSec = 4.0f;
        float maxHitWindowSec = 8.0f;
        float burstWindowSec = 0.5f;
        float urgencyHitsDiv = 3.0f;
        // 11-17b: how long a (spell, foe) pair whose line of fire had a party member in it stays refused after the line cleared (a friendly stepping across the
        // line would otherwise flip the pick every lap). PLACEHOLDER: `t.fireLineKeepSec`.
        float fireLineKeepSec = 1.0f;
        // 11-17f (field Jesper, "spell stuttering"): how long a standing claim may aim at a target his OWN ray has read Occluded without a break before it is
        // RELEASED (an offense claim on a foe: cast/CastOn.cpp; a heal on a recipient: cast/HealRoad.cpp HealOccludedBound). The same 4 s as occlusionHoldSec
        // (the pick's keep-current-attack hold), so the claim and the pick give up on an unseen target on the same clock. 0 = off. FAILS SAFE, principle 9: Sightline's cache is
        // fresh for 1 s and a service lap with 8+ followers is longer, so a verdict can go stale between laps and the bound then simply does not fire. PLACEHOLDER: `t.occClaimRelease`.
        float occludedClaimReleaseSec = 4.0f;
        // 11-17f: how long a foe whose ch.24 approach ended ENGINE DROPPED (its own path to him failed) waits before the approach is filed again (apmf/OffenseApproach.cpp;
        // it used to be blocked for the whole fight). Sized past the 3 s tightening gap so a path that failed from this spot is retried from another. PLACEHOLDER: `t.offApproachBackoff`.
        float offenseApproachBackoffSec = 6.0f;
        // 11-17f review F2: how many ENGINE DROPPED endings of the approach to one foe get the backoff before the foe is blocked for the fight again. PLACEHOLDER: `t.offApproachDropCap`.
        float offenseApproachDropCap = 3.0f;
        // 11-17f: how far past fSharedRadius the recipient of a STANDING heal / buff claim is kept before it is released (cast/Actuation_internal.h WithinSharedRadius):
        // the picker names a NEW recipient only inside the radius, so a claim is not released and re-named one lap apart at the edge. World units, 10% of the default
        // 3000. PLACEHOLDER: `t.sharedRadiusMargin`.
        float sharedRadiusMarginUnits = 300.0f;
        // 11-17f: a hand is not taken from its incumbent within this long of an observed fire on it (cast/Hands.cpp CanPreemptHand): the take in the same frame as the
        // fire was read as the taker's own cast and threw away the next charge. About two service laps. 0 = off. PLACEHOLDER: `t.preemptSettle`.
        float preemptFireSettleSec = 0.25f;
        // KITE (RetreatDanger): a follower who fights at range backs off from a foe that has to close once it is inside this
        // distance, by how little he could afford the trade. 0 = off (the default until the lab shows a gain on every list).
        float kiteDistance = 0.0f;
        float groundCooldownSec = 3.0f;          // give ground: one step, then look again
        float disengageCooldownSec = 25.0f;      // card G3: one hide run, then fight from where he is for a while (the run and
                                                 // its 12 s hold are over well inside it; not from the search)
        float blockCooldownSec = 2.0f;
        float blockBase = 0.0f;                  // BlockNeed for a swing in the air when he is in no danger. 0 = he blocks only what is
                                                 // killing him (the lab: 0.5 lost on LoreRim, 0 holds on every list)
        float bashCooldownSec = 4.0f;
        float backOffCooldownSec = 6.0f;         // batch I: one back-off run, then he fights from where he is for a while (so a Mercenary darts in
                                                 // again and a mage is not run in circles; chosen, tuned in the sim)
        float stepCooldownSec = 3.0f;            // batch I item 5: one quiet step, then the hold row looks again (chosen)
        float regroupCooldownSec = 6.0f;         // a regroup is a walk of seconds: no second order inside this (no yo-yo)
        // feat/consumables: THE ONE TABLE of the worth-the-gold rule (iaus/ConsumableWorth.h). PLACEHOLDERS, not searched; the sim sets them later.
        // 1dfcuabd2pk THE PERK-EFFECT WEIGHTS OF THE GEAR PICKS (iaus/PerkEffects.h). PLACEHOLDERS, not searched (the combat sim has no gear pick; marth
        // tunes them in MFO_Tuning/shared.txt as `t.perkArmor` / `t.perkWeapon`). The factor a piece of gear earns is 1 + weight x (multiplier - 1), where the
        // multiplier is what the ENGINE says his perks do to that armor type's rating / that weapon kind's damage. 0 = the perks are not weighed (the old pick).
        float perkArmorWeight = 0.0f;            // SHIPPED INERT (marth 11-16 "Ship 0 plan tune soon"): armor pick (logistics/Gear.cpp ArmorPrefFor): light / heavy by the engine's ModArmorRating answer
        float perkWeaponWeight = 0.0f;           // SHIPPED INERT (marth 11-16): weapon pick (Logistics_internal.h WeaponScore): the weapon kind by the engine's ModAttackDamage answer
        // fix/spell-worth C2 (PLACEHOLDER, not searched; the sim mirror is open): what an offense spell whose damage could NOT be measured is worth, as a
        // share of his best MEASURED castable option on that foe (Estimate::UnmeasuredWorth: held strictly below it). `t.unmeasuredDamage` in shared.txt.
        float unmeasuredDamage = 0.5f;
        // MOVEMENT (S1). Every value is a PLACEHOLDER the `spots` stage searches (S5) or a probe replaces. Read only by Spots.cpp / MovePass.cpp.
        float moveDecisionSec = 0.5f;            // read ONLY through MoveDecisionSec(): the 0.5 floor
        float spotPinKinds = 0.0f;               // >= 1: kinds that need a pin (cover, chokepoint hold, flank) may be offered, and then only when RoadCaps::pin holds
        float spotMomentum = 1.25f;              // the spot he is walking to
        float spotPreempt = 0.15f;               // a new spot must beat where he is (or is going) by this share
        float spotRadius = 1500.0f;              // how far (walked) from a member spots are looked for
        float spotSplitArea = 160000.0f;         // a triangle larger than this offers three more points than its centre
        float spotEyeU = 110.0f;                 // the eye over the floor, geometry rays only
        float spotHighU = 96.0f;                 // high ground stands this far over the target
        float spotPortalU = 180.0f;              // a walk edge between two walls narrower than this is a doorway: one body
        float spotPortalStandU = 100.0f;         // a point this near to it stands in it. A gap one square (128) deep is ONE doorway, both its edges (at 60 two bodies stood in it)
        float spotExitU = 500.0f;                // a route out must lead this far
        float spotPeelU = 250.0f;                // a peel spot lies this near to the middle between attacker and ally
        // ORDERS (MoveOrder.h, strict movement). PLACEHOLDERS, read only by MovePass.cpp OrderOf / StayMode
        float orderLeash = 64.0f;                // a Stand / Kite / GoTo order's leashR: pushed past it he is walked back
        float orderLeashPin = 40.0f;             // ... on a kind that needs a pin (cover, chokepoint, flank)
        float orderArrive = 24.0f;               // arriveR: he walks on until this near (Harbinger's kDefaultArriveR)
        float orderHoldStay = 1.0f;              // >= 1: "stay" may be a Stand on his own place (StayMode). 0 = always Free
        float orderHoldFoes = 2.0f;              // a blade holds where he is with this many foes on him in orderNearU, and fights freely with fewer
        float orderNearU = 400.0f;
        float orderStandMinU = 300.0f;           // a shooter stands while no foe that targets him is nearer than this. Nearer, the engine's own back-off is worth more than standing
        float spotArriveU = 90.0f;               // inside this of a committed spot he has arrived
        float spotFactMoveU = 128.0f;            // a sight fact or a path is reused until an end moved this far (or left its triangle)
        float spotFactMaxAgeSec = 5.0f;          // ... or got this old
        float spotKeptAgeSec = 1.0f;             // a committed spot's own sight is asked again once its answer is this old (kMarkSightLost)
        float spotUnknownExposure = 0.75f;       // the share an UNANSWERED threat counts as in spot_exposure: neither hidden nor surely seen. It keeps the ray budget from deciding the place
        // LIGHT (S4, iaus/Light.h). PLACEHOLDERS: the level's scale is the engine's cached actor light level (probe-light reports it)
        float lightNearU = 256.0f;               // a sample says something about places this near
        float lightStoreyU = 96.0f;              // ... and this near in height
        float lightMaxAgeSec = 20.0f;            // ... for this long
        float lightMinConf = 0.25f;              // under this confidence the light of a place is unknown
        float lightLitLevel = 45.0f;             // ground at or above this level is lit (the walk of a member who wants the dark)
        float lightGenU = 900.0f;                // the spot generators: a place known to be lit (at t.lightLitLevel or above, less below it) ranks as if this much farther, for who wants the dark
        float moveHealYield = 0.0f;              // a heal he would have to walk for ranks under every ranked heal of somebody he reaches NOW that scores at least this share of it (0 = under every one). PLACEHOLDER
        float spotActionMomentum = 1.0f;         // the momentum of the action his place was chosen for (8.1 spotAction, TickIn). 1 = none. PLACEHOLDER, not searched
        float lightDarkHoldSec = 6.0f;           // who wanted the dark this recently is not lit (the one hysteresis of the cast and the decast)
        WorthTable worth{};
        // S11b (iaus/SpellWorth.h): what one cast of a spell is worth in one unit for every family. SHIPPED INERT (`sw.live` = 0): `sw.<field>` in shared.txt.
        SpellWorth spellWorth{};
    };
    [[nodiscard]] inline constexpr float MoveDecisionSec(const Tuning& a_t) { return a_t.moveDecisionSec >= kMoveDecisionFloorSec ? a_t.moveDecisionSec : kMoveDecisionFloorSec; }   // a NaN reads as the floor

    // DefaultTuning() is the LIVE tuning: the compiled values until iaus/TuningFile.cpp (MFO_Tuning/shared.txt, potions.txt) swaps edited ones in on the
    // worker between ticks. ShippedTuning() is the compiled table, never edited; a reload always starts from it. LiveTuning() is the editable live object
    // (TuningFile only). SetTuningScalar: one Tuning scalar by its `t.<name>` (the sim's kTuneScalars, tools/iaus_sim/Tune.h; the two lists are duplicated).
    [[nodiscard]] const Tuning& DefaultTuning();
    [[nodiscard]] const Tuning& ShippedTuning();
    [[nodiscard]] Tuning& LiveTuning();
    bool SetTuningScalar(Tuning& a_t, std::string_view a_name, float a_v, std::string* a_why = nullptr);   // a_why: the range / reason on false

    // ── Class profile: the weight-input shape card I8 fills (section 4.9) ─────────────────────────────────
    // classWeight(candidate) = family[f] x role[tag]. A consideration weight override REPLACES that family's
    // default w (design: "that consideration's w becomes 1.3 for that family"). Default: all 1, no overrides,
    // which is exactly the uncustomised catalogue.
    // The five schools, by IDENTITY (the index into Profile::school, Candidate::school, ClassCurves schoolPeak and the sim's
    // `schoolPeak` scalar). The identity numbers never change; the ORDER ON THE CIRCLE is kSchoolCircle below.
    enum School : std::int8_t { kDestruction = 0, kAlteration = 1, kRestoration = 2, kIllusion = 3, kConjuration = 4 };
    inline constexpr std::size_t kSchools = 5;
    // THE SCHOOL CIRCLE (marth is still laying the order out): the ONE place that fixes which school sits next to which. The wave of
    // ClassCurves::SchoolWeight measures circular distance in steps along THIS array. To change the order, reorder these five.
    inline constexpr std::array<School, kSchools> kSchoolCircle = { kDestruction, kAlteration, kRestoration, kIllusion, kConjuration };
    static_assert([] {   // a permutation of the five schools: each identity exactly once
        std::array<int, kSchools> seen{};
        for (const School s : kSchoolCircle) { if (s < 0 || static_cast<std::size_t>(s) >= kSchools) return false; ++seen[static_cast<std::size_t>(s)]; }
        for (const int n : seen) if (n != 1) return false;
        return true;
    }(), "kSchoolCircle must list each of the five schools exactly once");
    struct Profile {
        struct WeightOverride { Family family; Cons cons; float w; };
        // Per-class CURVE override (ruling Q-class-curves, marth 11-12): a (family, consideration) pair scored with this curve
        // instead of Tuning::curve[cons]. Empty = every pair uses the shared catalogue curve, which is bit-identical to today.
        // Filled from iaus/ClassCurves.h (one table row per class). Score.cpp reads it (detail::CurveFor).
        struct CurveOverride { Family family; Cons cons; Curve curve; };
        std::array<float, kFamilies> family{};
        std::array<float, kRoleTags> role{};
        std::vector<WeightOverride> weights;
        std::vector<CurveOverride> curves;
        // School affinity (ruling Q-school-affinity, marth 11-12): a score MULTIPLIER per magic school (School identity order),
        // a smooth wave over the school circle built from a ClassCurves row (School identity order). 1.0 everywhere = flat = bit-identical. Never a veto.
        std::array<float, kSchools> school{};
        std::array<float, kSpotKindCount> spot{};   // MOVEMENT (S1): a multiplier per spot kind (ClassCurves Row::spot)
        Profile() { family.fill(1.0f); role.fill(1.0f); school.fill(1.0f); spot.fill(1.0f); }
    };

    // I8's mechanical derivation, one call per applied DefaultGambit[] row: adds 0.5 x (rows - index) / rows to
    // the family (a_tag == None) or to the role tag (a SpellRole or weapon row narrows the bump to it).
    void BumpRow(Profile& a_p, Family a_f, RoleTag a_tag, int a_index, int a_rows);
    void SetWeight(Profile& a_p, Family a_f, Cons a_c, float a_w);

    // ── Candidate facts: everything the scorer reads, already read by the caller (I4) ─────────────────────
    // Defaults are the neutral value, so a family only fills what its considerations read. A fact a
    // consideration does not apply to makes it NOT APPLICABLE (dropped, not counted in n): see Catalogue.cpp.
    struct Facts {
        // range and sight
        float dist = 0.0f;
        float reach = 0.0f;               // <= 0: range not applicable (self actions)
        bool  meleeRange = false;         // true: melee range curve, false: spell / ranged curve
        Sight sight = Sight::None;
        float occludedSec = 0.0f;         // how long the ray this candidate reads (sight) has been Occluded (0 = not, or not tracked)
        // the foe
        float foeLevelRatio = 1.0f;       // foe level / self level
        float foeHpPct = 1.0f;
        bool  foeCaster = false;
        bool  foeOnMe = false;
        bool  foeOnPlayer = false;
        bool  foeOnAlly = false;
        bool  playerTarget = false;       // the foe is the player's combat target
        bool  currentTarget = false;      // the foe is Targeting::Current
        bool  foeBlocking = false;
        bool  foeStronger = false;
        bool  foeControlled = false;
        bool  foeTaunted = false;
        bool  foeChaseWaived = false;     // the foe is chase-waived (Combatant::chaseWaived): act.attack's range input clamps to the cap's edge, no veto
        bool  foeOutOfMeleeReach = false;
        bool  foeUnholy = false;
        bool  damageClass = false;        // a destruction caster (Classes::Flag::kDamage): Decide caps a buff's score at the best viable offense
        // resources (magicka absolute; cost <= 0 = no magicka cost, Affordable not applicable)
        float cost = 0.0f;
        float magicka = 0.0f;
        float magickaMax = 0.0f;
        int   stock = -1;                 // potions / scrolls carried; -1 = not a stocked item
        // pacing (Engine::FillCooldowns writes the two read-backs)
        float hardCooldownSec = 0.0f;     // class row cooldown (power attack 10 s); 0 = none
        float hardCooldownLeft = 0.0f;
        float sinceFired = kInf;          // seconds since this (family, form) last fired; inf = not recently
        // gates
        Tri   willWork = Tri::Unknown;    // G10 + CTDA mirror
        Tri   targetCond = Tri::Unknown;  // the spell's own target conditions
        bool  alreadyActive = false;
        bool  handFree = true;            // HealStandsOnHand / CastInFlightOnHand on the needed hand
        bool  splashSafe = true;          // no party member inside area of the target
        bool  areaGuardOk = true;         // M2 (AreaCensus.h): no bystander in a hostile area and the picture is readable (Control: no party member either). Read by splash_safe
        // element and area
        float resist = 0.0f;              // target's resist AV for the spell's element; 0 = none / non-elemental
        bool  hasArea = false;
        float foesInArea = 0.0f;
        // self
        float selfHp = 1.0f;
        float selfStamina = 1.0f;
        float selfMagicka = 1.0f;         // pct, now
        // recipient (heal / buff)
        float recipientHp = 1.0f;
        bool  recipientIsPlayer = false;
        bool  recipientDowned = false;
        bool  recipientInMelee = false;
        bool  meleeBuff = false;          // armor / cloak type: values a recipient in melee
        Tri   inHealReach = Tri::Unknown;
        // the fight
        float combatSec = 0.0f;
        int   foeCount = 1;
        int   partyCount = 1;
        bool  liveSummon = false;
        bool  casterOnMe = false;         // a hostile caster targets me (G11 refines to "charging at me")
        Tri   unseen = Tri::Unknown;      // no foe has me in sight (G2 Layer 1: he sneaks and every listed foe's group record says he is not detected)
        bool  selfSneaking = false;       // Snapshot::selfSneaking
        Tri   foeUnaware = Tri::Unknown;  // THIS candidate's foe does not detect him (Combatant::detectsMe == No -> Yes)
        std::uint8_t tauntedOnMe = 0;     // Q-S-5: foes under a live taunt that have him as their target (Generate counts, Block candidates only)
        float backOffNeed = 0.0f;         // batch I: Cons::BackOffNeed's input, 0..1 (Generate fills it; iaus/Reposition.h)
        bool  strikeReady = false;        // the hold row: a blow from stealth is ready now (an unaware foe in reach / in sight)
        bool  disengageClass = false;     // Flag::kDisengage
        bool  disengageFlee = false;      // disengage: a non-sneak flee (need = RetreatDanger) rather than a hide run (need = detectShare)
        float detectShare = 0.0f;         // disengage (hide run): the share of the counted foes whose group record says they have him
        bool  fleeHeal = false;           // a non-sneak flee runs and this heal / health potion is for himself (BoostFleeHeal)
        // posture
        PostureKind posture = PostureKind::None;
        bool  postureCurrent = false;     // this category is what is drawn now ("keep")
        bool  outOfAmmo = false;
        bool  staffCharged = true;
        bool  classAllowsWeapon = true;   // false only under strict adherence (G16)
        float perkShare = -1.0f;          // batch 3 part C: this posture's share of his weapon-damage perk multipliers, mine / (mine + the other side's); < 0 = not read
        float handLost = -1.0f;           // 1dfcuabd22x: the share of the fight's foe health his weapon would deal while a concentration stream holds his hand, 0..1; < 0 = not applicable
        float spellPerkEdge = -1.0f;      // 1dfcuabd2pk: the share of this spell's worth his perks pay for in its school, 0..1 (Snapshot.h PerkSpellEdge); < 0 = not read
        // class (ruling Q34). onClass: the ability belongs to the follower's current class (tree OFF: its T1
        // class). fromMastered: a class the follower has MASTERED grants it. emergency: EmergencyLevel().
        bool  onClass = true;
        bool  fromMastered = false;
        float emergency = 0.0f;
        // estimates (iaus/Estimate.h). 0 / +inf = not measured: the considerations that read them do not apply.
        float expDamage = 0.0f;           // this cast's expected health damage on this target; < 0 = this spell is not measured
        float bestDamage = 0.0f;          // the best expected damage PER CAST SECOND (Generate.cpp EffectRate's unit) among his castable offense spells on this target (fix/spell-worth F5)
        float expRate = -1.0f;            // this spell's expDamage in that same unit, an unmeasured spell's placeholder included (DamageValue's numerator); < 0 = not known
        float foeHealth = 0.0f;           // the target's health now, absolute
        float expHeal = 0.0f;             // this cast's expected healing
        float healRate = 0.0f;            // this heal's expected healing per second of cast (Generate.cpp EffectRate); 0 = not measured
        float bestHealRate = 0.0f;        // his best healing per second of cast among the heals he can cast at THIS recipient now (Generate.cpp BestHealRateOn); 0 = none measured
        float effectRate = 0.0f;          // 11-17b: this spell's expected damage / healing per second of cast (the tie-break's first read; 0 = not measured)
        bool  concentration = false;      // 11-17b: a concentration stream (its cost is per second: the tie-break counts it over the channel, Tuning::concChannelSec)
        bool  recipientSelf = false;      // a heal for the caster himself (Cons::BoostRecipientDying's low-health read)
        float recipientMissing = 0.0f;    // health the recipient is missing, absolute
        float selfTtl = kInf;             // seconds he lives at the present incoming damage
        float tauntTtl = kInf;            // ... with this foe on him as well (taunt)
        float playerTtl = kInf;           // the player's
        float recipientTtl = kInf;        // the heal recipient's
        float recipientHitsToDown = kInf; // 11-17g: his health / the largest burst (one lap) he took lately (Estimate::HitsToDown); +inf = no hit seen
        float chase = 0.0f;               // melee attack: how far he may chase (Snapshot::chaseCap); 0 = not a chase
        float tauntChance = -1.0f;        // taunt: the chance it works on this foe (Estimate::TauntChance); < 0 = unknown
        bool  recipientBeingHealed = false;   // another party member's heal is in flight on the recipient
        float controlSec = 0.0f;          // control cast: seconds the effect holds the foe (SpellEntry::controlSec); 0 = not measured
        float castsWanted = -1.0f;        // magicka potion: casts of his best spell the fight still asks for; < 0 = unknown
        bool  mortal = false;             // Snapshot::mortal
        float meleeThreatDist = kInf;     // retreat: distance of the nearest foe that targets him and has to close to hurt; +inf = none
        bool  rangedDrawn = false;        // retreat: a bow, crossbow or staff is in his hands (he fights at range)
        float threatTtl = kInf;           // retreat: seconds he would live with every foe that targets him on him (Estimate::ThreatOn)
        bool  anchorHealer = false;       // regroup: the anchor can heal others
        bool  anchorSafer = false;        // regroup: the anchor stands clearly farther from what is hitting him than he does
        bool  foeSwinging = false;        // block / bash: this foe's swing at him is in the air
        bool  foeCasting = false;         // bash: this foe is charging a spell right now
        // buff potions (Family::DrinkBuff; iaus/PotionBuff.h). Each is "not applicable" at its sentinel, so the consideration that reads it drops out.
        std::uint8_t buffKind = 0xFF;     // the BuffKind this candidate drinks for; 0xFF = not a buff potion
        float buffThreat = -1.0f;         // resist kinds: 0..1 share of the kept foes whose incoming element the potion resists; < 0 = n/a
        float buffRegenPct = -1.0f;       // regen kinds: the resource's pct now (feeds DrinkNeed); < 0 = n/a
        bool  buffOpener = false;         // fortify kinds: FightStart applies (a fortify is an opener, drunk before the fight is old)
        Tri   buffStealth = Tri::Unknown; // invisibility: Yes = stealth intent stands, No = it does not (a gate); Unknown = n/a
        float consumableWorth = -1.0f;    // Cons::ConsumableWorth: iaus/ConsumableWorth.h Worth(need, cost share) for the item this drink uses; < 0 = n/a
        // S4 (iaus/Light.h): each not applicable at its sentinel
        bool  lightBuff = false;          // a buff cast / scroll that lights its recipient: the three below are read, and Decide keeps it under a heal or a ward
        bool  recipientWantsDark = false;
        float recipientLight = -1.0f;     // the level where he stands; < 0 = unknown
        float lightRank = -1.0f;          // Light::RecipientRank
        float selfLight = -1.0f;          // the hold row: the level where he himself stands; < 0 = unknown
        bool  lightOnStealth = false;     // decast
        bool  foeHelpless = false;        // BoostControlled: the foe is PARALYSED (he cannot answer a blow). Not a fleeing foe:
                                          // foeControlled also covers fleeing (the control-cast gate), and a boost on a runner
                                          // pulled followers after him (review of 303c14d, F2)
    };

    // MOVEMENT (S1): the facts of one (member, action, spot) triple, filled in ONE place (MovePass.cpp) and read by Spots.cpp ScoreSpot.
    // A negative float, a Tri::Unknown = NOT APPLICABLE.
    struct SpotFacts {
        float pathSec = -1.0f, pathExposedSec = -1.0f, climb = -1.0f, drop = -1.0f;
        Tri   sees = Tri::Unknown, fireLineClear = Tri::Unknown, inHostileArea = Tri::Unknown, foeReachable = Tri::Unknown;
        Tri   laneCrossed = Tri::Unknown, portalBusy = Tri::Unknown, serves = Tri::Unknown;
        float exposure = -1.0f, rangeX = -1.0f, exits = -1.0f, allyDist = -1.0f, leashX = -1.0f;
        float detected = -1.0f, light = -1.0f, pathLitSec = -1.0f;   // S4: for a member who wants the dark, where the light is known
        float spotRegionTrust = -1.0f, pathRegionTrust = -1.0f;   // Trust::Level as a number, -1 while trusted (S3)
        bool  coversAlly = false;
        bool operator==(const SpotFacts&) const = default;
    };
    // MOVEMENT (S1): one of a member's ranked actions, exported from a Decision (TopRanked) for the movement pass to pair with spots.
    struct RankedAction {
        Family family = Family::kCount; Family scrollOf = Family::CastOffense;
        std::uint32_t form = 0, target = 0;
        float score = 0.0f, reach = 0.0f;   // reach <= 0: range not applicable
        bool  melee = false;
        Sight sight = Sight::None;          // from where he stands, as the action facet read it. None = not a sighted action
        bool  moveOnly = false;             // Generated::moveOnly: it does not work from where he stands. Staying is worth nothing for it, and only kServesMoveOnly places pair with it
        Family Kind() const { return family == Family::Scroll ? scrollOf : family; }
    };

    struct Candidate {
        Family      family = Family::Attack;
        Family      scrollOf = Family::CastOffense;  // Scroll only: whose consideration set the spell uses
        RoleTag     role = RoleTag::None;
        std::int8_t school = -1;                     // a spell / scroll's magic school (School identity 0..kSchools-1); -1 = none / not a spell
        float       schoolBump = 0.0f;               // cross-class bump at this spell's school from the MASTERED casting classes (ClassGate::schoolBump); 0 = none
        std::uint32_t form = 0;                      // spell / scroll / potion FormID; 0 for attacks
        std::uint32_t target = 0;                    // explicit target FormID; 0 = self / none
        std::string_view formName;                   // display only; owned by the caller for the tick
        std::string_view targetName;
        Facts       f;
    };

    // The identity momentum, the commitment window, the variety ring and Cooldowns key on.
    struct Key {
        Family        family = Family::Attack;
        PostureKind   posture = PostureKind::None;
        std::uint32_t form = 0;
        std::uint32_t target = 0;
        bool operator==(const Key&) const = default;
    };
    [[nodiscard]] Key KeyOf(const Candidate& a_c);
    // Cooldowns id (Engine): (family, posture, form), NOT the target -- a spell cools down whoever it hit.
    [[nodiscard]] std::uint64_t CooldownId(const Key& a_k);
    // marth Q69 (taunt) / Q73 (power attack): these two families keep ONE cooldown per follower, shared with the gambit
    // rows, not one per (family, posture, form). Returns the Cooldowns::Kind::Follower slot (Cooldowns.h
    // kFollowerPowerAttack = 1, kFollowerTaunt = 2), or 0 = the family keeps its own CooldownId. Engine.cpp maps it.
    [[nodiscard]] inline constexpr std::uint64_t SharedCooldownId(Family a_f) {
        return a_f == Family::PowerAttack ? 1u : a_f == Family::Taunt ? 2u : 0u;
    }

    // Every float fact is finite (sinceFired may be +inf, "not recently"). A candidate that fails is VETOED by the
    // scorer and counted in Decision::badFacts: a NaN would otherwise read as a passing or dropped term (review
    // round 1, finding 2) and a broken reader would look like a working one.
    [[nodiscard]] bool FactsFinite(const Facts& a_f);

    // 0 at or above emergencyHpHigh of the worst party HP pct (self, player, allies), 1 at or below emergencyHpLow.
    [[nodiscard]] float EmergencyLevel(float a_worstPartyHpPct, const Tuning& a_t = DefaultTuning());

    // ── Per-follower runtime memory (momentum, decision epoch, variety ring, log pacing) ──────────────────
    // RUNTIME ONLY, never saved (design 7). Cleared by Engine::Clear from ResetAllState after StopPump.
    struct Mind {
        bool          hasRunning = false;
        Key           running{};
        std::uint32_t epoch = 0;              // decision epoch; rank = kRankBase - epoch
        double        firedAt = -1.0e300;     // FIRST Fired of the running decision (window anchor; a repeat Fired of the same decision does not move it, marth Q67)
        double        decidedAt = -1.0e300;   // when the running decision started (readout "how long")
        std::array<std::uint32_t, 4> recent{};   // last 4 Fired forms (variety)
        std::uint8_t  recentN = 0;
        std::uint8_t  recentHead = 0;
        bool          hasPosture = false;
        Key           posture{};
        bool          hasLogged = false;
        Key           logged{};
        std::uint8_t  loggedVerdict = 0;      // the logged line's verdict and fired flag are part of the dedupe
        bool          loggedFired = false;
        double        loggedAt = -1.0e300;
        void Clear() { *this = Mind{}; }
    };

    // Choice.rank for I1 (design 1.3): far above any table index (64 rows max); a newer decision has a LOWER
    // rank, so it outranks an older hold.
    inline constexpr int kRankBase = 1'000'000'000;
    [[nodiscard]] int RankOf(std::uint32_t a_epoch);

    // ── One tick's decision ───────────────────────────────────────────────────────────────────────────────
    struct TickIn {
        double        now = 0.0;              // seconds, one monotonic clock for the whole session
        std::uint8_t  rapportRank = 1;        // 1..5 (Rapport::RankFor)
        double        commitWindowSec = 1.5;  // g_suppressWindow x (0.88 + 0.24 Temperament), Engine::MakeTick
        std::uint64_t seed = 0;               // per follower (Temperament bits), Engine::MakeTick
        bool          hasSpotAction = false;  // MOVEMENT (8.1): the movement pass holds a place for him, chosen for spotAction. A bonus (Tuning::spotActionMomentum), never an order
        Key           spotAction{};
    };

    struct Term { Cons cons; float x; float raw; float w; float s; float comp; };

    struct Scored {
        std::uint16_t idx = 0;           // into the candidate span
        float         score = 0.0f;      // final, momentum included
        float         base = 0.0f;       // before momentum
        float         focus = 1.0f;      // the player-focus multiplier that applied
        bool          running = false;
    };

    enum class Verdict : std::uint8_t {
        Act = 0,    // walk `order`
        Hold,       // commitment window: only the running decision may go; if it is transparent the scan stops
                    // "suppressed" (the #26 analogue)
        Idle,       // nothing above the floor: issue nothing (the follower's own AI runs)
    };

    struct Step {
        std::uint16_t idx = 0;
        int           rank = 0;          // Choice.rank for this try (RankOf(epoch))
        std::uint32_t epoch = 0;         // the decision epoch this try belongs to (OnFired adopts it)
        bool          running = false;   // continuing the running decision
    };

    struct Decision {
        Verdict             verdict = Verdict::Idle;
        std::vector<Scored> ranked;      // every candidate that cleared the floor, best first, ties broken
        std::vector<Step>   order;       // the walk (<= maxTries)
        bool          newDecision = false;   // order[0] is not the running decision
        bool          rolled = false;
        float         roll = -1.0f;
        std::uint32_t proposedEpoch = 0;
        std::size_t   generated = 0;     // candidates offered
        std::size_t   evaluated = 0;     // fully scored (not vetoed, not pruned)
        std::size_t   vetoed = 0;
        std::size_t   pruned = 0;        // P5 early-out
        bool          overCap = false;   // more than maxCandidates offered (only the first maxCandidates scored)
        std::size_t   buffCapped = 0;    // damage-class buffs held at the best viable offense score (Decide, marth 2026-11-13)
        std::array<std::uint16_t, kCons> vetoByCons{};   // fix/spell-worth: candidates vetoed, by the consideration that vetoed them (Cons::kCount's own bucket excluded: a zero weight / a bad fact)
        std::size_t   badFacts = 0;      // candidates VETOED because a fact was NaN / inf (a reader bug: never masked)
        int           firstBadFact = -1; // the first such candidate's index, for the adapter's WARN
        float         bestHeal = 0.0f;   // best CastHeal / heal-scroll score over its action weight (Retreat and the health drink read it)
        std::uint8_t  rapportRank = 1;
    };

    // Score one candidate fully (explain path, harness). a_terms (optional) receives every consideration.
    // Returns the score before momentum, 0 when vetoed (a_vetoBy set when non-null; Cons::kCount = a zero weight
    // or a NaN / inf fact, see FactsFinite).
    [[nodiscard]] float ScoreOne(const Candidate& a_c, const Mind& a_m, const Profile& a_p, const Tuning& a_t,
                                 std::uint8_t a_rapportRank, float a_bestHeal, std::vector<Term>* a_terms,
                                 Cons* a_vetoBy = nullptr, float* a_focus = nullptr);

    // The action layer (every family except Posture). Pure: a_m is read, never written.
    [[nodiscard]] Decision Decide(const Mind& a_m, std::span<const Candidate> a_cands, const Profile& a_p,
                                  const Tuning& a_t, const TickIn& a_in);

    // MOVEMENT (S1): the action he holds (the running decision, when it is ranked) and the best of the rest, at most three. Returns the count.
    std::size_t TopRanked(const Decision& a_d, std::span<const Candidate> a_cands, std::array<RankedAction, 3>& a_out);

    // MOVE-ONLY (Generated::moveOnly): actions he can not do from where he stands but could from another place. Never scored by Decide, never in its order, never cast:
    // the movement pass alone hears of them, here. Scored as if in reach. One for each target (the best), under every ranked heal of somebody he reaches now
    // (Tuning::moveHealYield; his own heal holds him nowhere), over any light (a light in the export ranks under the best of them), never the held action.
    struct MoveAsk {
        std::span<const Candidate> cands;      // Generated::moveOnly
        std::span<const Sight>     sight;      // Generated::moveOnlySight: what his ray reads now, for RankedAction::sight
        const Mind&    mind;
        const Profile& profile;
        const Tuning&  tuning;
    };
    std::size_t TopRanked(const Decision& a_d, std::span<const Candidate> a_cands, std::array<RankedAction, 3>& a_out, const MoveAsk& a_ask);

    // The posture layer (Posture candidates only): deterministic best with postureMomentum on the drawn
    // category. No rapport roll (a weapon swap is animated and expensive: dithering is never wanted). Returns
    // the index into a_cands, or -1 when nothing clears the floor (no equip order wanted).
    [[nodiscard]] int DecidePosture(const Mind& a_m, std::span<const Candidate> a_cands, const Profile& a_p,
                                    const Tuning& a_t, std::uint8_t a_rapportRank, float* a_score = nullptr);

    // Bookkeeping after Actuation::Fire returned FIRED for a_step (design 1.1 "Record"). A NEW decision adopts the
    // epoch and anchors the commitment window; a repeat Fired of the running decision does NOT re-anchor it (marth
    // Q67, card I6). Every Fired feeds the variety ring.
    void OnFired(Mind& a_m, const Candidate& a_c, const Step& a_step, double a_now);
    void OnPostureFired(Mind& a_m, const Candidate& a_c);
    // The running decision ended without a successor (combat end, the toggle edge, a dead target).
    void EndDecision(Mind& a_m);

    // The section 5.1 pick on its own (Decide uses it; the harness prints its distribution).
    // Returns the index into a_sorted (best first). a_u in [0,1).
    [[nodiscard]] std::size_t SharpPick(std::span<const float> a_sortedScores, const Sharpness& a_s, float a_u);
    [[nodiscard]] float Unit(std::uint64_t a_seed, std::uint32_t a_epoch);   // deterministic [0,1)

    // ── Explain (section 6) ───────────────────────────────────────────────────────────────────────────────
    struct WalkNote {                 // a transparent fall-past, in walk order (I6 fills it from Outcome.reason)
        std::uint16_t idx = 0;
        std::string   reason;
    };
    // "[iaus] ..." without the tag: "pick=... | 2 ... | 3 ... | n=90/202 r=III roll=0.37 mom=+25% walk=1 0.06ms"
    [[nodiscard]] std::string FormatDecision(const Decision& a_d, std::span<const Candidate> a_cands,
                                             const Tuning& a_t, int a_pickedStep,
                                             std::span<const WalkNote> a_walk, double a_ms);
    // "[iaus-why]" body for one candidate: "<label> = 0.712 | exploit r=-50 1.00 w1.3 | range 0.92 | ..."
    [[nodiscard]] std::string FormatWhy(const Candidate& a_c, const Mind& a_m, const Profile& a_p,
                                        const Tuning& a_t, std::uint8_t a_rapportRank, float a_bestHeal);
    [[nodiscard]] std::string Label(const Candidate& a_c);   // "cast_offense Firebolt(0001C789) -> Draugr(0003B547)"
    // On change of the picked key, plus every logRepeatSec while it runs. Writes the pacing into a_m.
    [[nodiscard]] bool ShouldLog(Mind& a_m, const Key& a_pick, Verdict a_verdict, bool a_fired, double a_now,
                                 const Tuning& a_t);

    // The board's "Thinking" readout (I7 renders it). Strings are COPIED so it can cross threads.
    struct Readout {
        struct Row { std::string label; float score = 0.0f; std::string why; };   // why = the FormatWhy body, no tag
        std::array<Row, 3> top{};
        std::uint8_t topN = 0;
        std::string  pick;
        double       runningSec = 0.0;
        std::uint8_t rapportRank = 1;
        float        actFloor = 0.0f;
        Verdict      verdict = Verdict::Idle;
    };
    [[nodiscard]] Readout MakeReadout(const Decision& a_d, std::span<const Candidate> a_cands, const Mind& a_m,
                                      const Tuning& a_t, double a_now, const Profile* a_p = nullptr);

}
