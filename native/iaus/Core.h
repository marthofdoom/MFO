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

    // ── Identity ─────────────────────────────────────────────────────────────────────────────────────────
    // Action families (design section 2). APPEND ONLY: the value is in log lines I9 parses and in the
    // Cooldowns id (CooldownId below), so a renumber would make old logs lie.
    enum class Family : std::uint8_t {
        Attack = 0,
        PowerAttack,
        Taunt,
        Sneak,        // after G1 (no act.sneak yet): scored, never generated today
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
        float noHealRetreat = 0.30f;           // Retreat vetoed while a heal candidate scores above this
        float noHealDrink = 0.50f;             // Health drink vetoed while a heal candidate scores above this
        // Card I6: the hard cooldown of a ROW-GATED action whose class row declares none (ClassGate::*CooldownSec
        // == 0). The power-attack rows all declare 10 s; no taunt row declares one, and with the class tree OFF
        // (marth Q63: nothing is class-gated) there is no row at all. Without a floor the sim's Knight taunts
        // 12-20 times a minute. One taunt ATTEMPT is one idle play plus a roll, and a hit already blocks the next
        // taunt for fTauntDuration (15 s) in the executor, so the floor only spaces MISSES. Tunable (I9).
        float rowlessPowerCooldownSec = 10.0f;
        float rowlessTauntCooldownSec = 10.0f;
    };

    [[nodiscard]] const Tuning& DefaultTuning();

    // ── Class profile: the weight-input shape card I8 fills (section 4.9) ─────────────────────────────────
    // classWeight(candidate) = family[f] x role[tag]. A consideration weight override REPLACES that family's
    // default w (design: "that consideration's w becomes 1.3 for that family"). Default: all 1, no overrides,
    // which is exactly the uncustomised catalogue.
    struct Profile {
        struct WeightOverride { Family family; Cons cons; float w; };
        std::array<float, kFamilies> family{};
        std::array<float, kRoleTags> role{};
        std::vector<WeightOverride> weights;
        Profile() { family.fill(1.0f); role.fill(1.0f); }
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
        Tri   unseen = Tri::Unknown;      // no foe has me in sight (H-DETECT, M2)
        bool  disengageClass = false;     // Flag::kDisengage
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
        float expDamage = 0.0f;           // this cast's expected health damage on this target
        float bestDamage = 0.0f;          // the best expDamage among his castable offense spells on this target
        float foeHealth = 0.0f;           // the target's health now, absolute
        float expHeal = 0.0f;             // this cast's expected healing
        float recipientMissing = 0.0f;    // health the recipient is missing, absolute
        float selfTtl = kInf;             // seconds he lives at the present incoming damage
        float tauntTtl = kInf;            // ... with this foe on him as well (taunt)
        float playerTtl = kInf;           // the player's
        float recipientTtl = kInf;        // the heal recipient's
    };

    struct Candidate {
        Family      family = Family::Attack;
        Family      scrollOf = Family::CastOffense;  // Scroll only: whose consideration set the spell uses
        RoleTag     role = RoleTag::None;
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
        float         bestHeal = 0.0f;   // best CastHeal / heal-scroll score (Retreat and the health drink read it)
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
        struct Row { std::string label; float score = 0.0f; };
        std::array<Row, 3> top{};
        std::uint8_t topN = 0;
        std::string  pick;
        double       runningSec = 0.0;
        std::uint8_t rapportRank = 1;
        float        actFloor = 0.0f;
        Verdict      verdict = Verdict::Idle;
    };
    [[nodiscard]] Readout MakeReadout(const Decision& a_d, std::span<const Candidate> a_cands, const Mind& a_m,
                                      const Tuning& a_t, double a_now);

}
