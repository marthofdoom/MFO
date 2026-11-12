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

    struct Tuning {
        std::array<Curve, kCons> curve{};
        std::array<float, kFamilies> actionWeight{};
        std::array<Sharpness, 5> sharp{};      // rank I..V
        std::array<float, 5> focus{};          // player focus m, rank I..V (ruling Q35)

        float momentum = 1.25f;                // fIausMomentum: the running action decision
        float postureMomentum = 1.5f;          // the running weapon category
        float preemptMargin = 0.25f;           // fIausPreemptMargin: commitment window
        float actFloor = 0.20f;                // fIausActFloor: below it IAUS issues nothing
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
    };

    [[nodiscard]] const Tuning& DefaultTuning();

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
        // School affinity (ruling Q-school-affinity, marth 11-12): a score MULTIPLIER per magic school (SchoolIndex order),
        // a smooth wave over the school circle built from a ClassCurves row (School identity order). 1.0 everywhere = flat = bit-identical. Never a veto.
        std::array<float, kSchools> school{};
        Profile() { family.fill(1.0f); role.fill(1.0f); school.fill(1.0f); }
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
        bool  foeOutOfMeleeReach = false;
        bool  foeUnholy = false;
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
        // class (ruling Q34). onClass: the ability belongs to the follower's current class (tree OFF: its T1
        // class). fromMastered: a class the follower has MASTERED grants it. emergency: EmergencyLevel().
        bool  onClass = true;
        bool  fromMastered = false;
        float emergency = 0.0f;
        // estimates (iaus/Estimate.h). 0 / +inf = not measured: the considerations that read them do not apply.
        float expDamage = 0.0f;           // this cast's expected health damage on this target; < 0 = this spell is not measured
        float bestDamage = 0.0f;          // the best expDamage among his castable offense spells on this target
        float foeHealth = 0.0f;           // the target's health now, absolute
        float expHeal = 0.0f;             // this cast's expected healing
        float recipientMissing = 0.0f;    // health the recipient is missing, absolute
        float selfTtl = kInf;             // seconds he lives at the present incoming damage
        float tauntTtl = kInf;            // ... with this foe on him as well (taunt)
        float playerTtl = kInf;           // the player's
        float recipientTtl = kInf;        // the heal recipient's
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
        bool  foeHelpless = false;        // BoostControlled: the foe is PARALYSED (he cannot answer a blow). Not a fleeing foe:
                                          // foeControlled also covers fleeing (the control-cast gate), and a boost on a runner
                                          // pulled followers after him (review of 303c14d, F2)
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
