#pragma once
// iaus/Snapshot.h -- ONE FOLLOWER'S FIGHT AS PLAIN DATA (card I4). The idea of IAUS for MFO is LandingCrew's.
//
// Candidate generation is two layers:
//   1. GATHER (iaus/Candidates.cpp, engine side): the worker's reads plus the SpellBook main-thread mirror fill a
//      Snapshot. Thin: it reads, it never decides.
//   2. GENERATE (iaus/Generate.cpp, PURE): Generate(snapshot, classGate, tuning) -> the candidates.
// Everything in this header is standard types only (a FormID is a std::uint32_t), so the off-line harness and the
// planned combat SIMULATOR (its own card: randomised fight state fed to IAUS tick by tick) build a Snapshot by
// hand and run the very code the game runs. Nothing here may ever include an RE:: header.
//
// CONVENTIONS: pools are ABSOLUTE with their maximum (hp / hpMax ...). A maximum of 0 means the actor could not be
// read (unloaded): the generator skips that actor, it never divides by it. Distances and positions are world
// units. A missing or unknown value keeps its default, which is always the neutral one.
#include "iaus/Core.h"
#include "iaus/PotionBuff.h"
#include "iaus/Prune.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace MFO::Iaus {

    // ── The SpellBook mirror's content (iaus/SpellBook.h measures it on the main thread) ──────────────────
    enum class SpellSource : std::uint8_t { Spell = 0, Scroll, Staff };

    struct SpellEntry {
        std::uint32_t form = 0;                 // spell / scroll FormID; for a staff the WEAPON's FormID
        SpellSource   source = SpellSource::Spell;
        Prune::Nature nature = Prune::Nature::Unknown;
        Prune::Drop   drop = Prune::Drop::Keep; // why it is never a candidate (P1 + the "never generated" list)
        RoleTag       role = RoleTag::None;
        std::int8_t   school = -1;              // magic school of the costliest effect, School identity (iaus/Core.h); -1 = none
        int           resistAv = -1;            // actor value of the costliest hostile effect's resist; -1 none
        float         cost = 0.0f;              // magicka cost for THIS caster (0 for scrolls and staves)
        float         reach = 0.0f;             // aimed reach in units; 0 = no cap of its own
        float         areaUnits = 0.0f;         // max effect area in units; 0 = single target
        bool          selfDelivery = false;
        bool          targetActor = false;      // Delivery::kTargetActor (batch I H4: a lock-on spell; aimed otherwise when not touch / self)
        bool          touch = false;            // reach = the melee reach
        bool          twoHanded = false;
        bool          concentration = false;
        bool          undeadOnly = false;       // a TurnUndead-archetype effect: undead foes only
        bool          meleeBuff = false;        // armor / cloak type: values a recipient in melee
        bool          silent = false;           // G2: the engine's own cast-sound entry point (55) leaves his cast of THIS spell soundless
                                                // (Fills::CastsSilently; false = loud or not measurable, which is today's behaviour)
        // The class-allowed bits AS THE BOOK MEASURED THEM (log / board). Generate reads the live ClassGate.
        bool          onClass = true;           // class_fit (ruling Q34): his current class casts spells
        bool          fromMastered = false;     // class_fit: a class he has MASTERED casts spells
        bool          liveSummon = false;       // Summon: he already commands a live summon from this spell
        std::int32_t  count = 1;                // scrolls / staves carried
        float         charge = 0.0f;            // staff: current charge
        bool          charged = true;           // staff: at least one cast left
        std::vector<std::uint32_t> activeOn;    // Buff / Ward: party members this spell's effect is live on
        // Estimates (iaus/Estimate.h): one cast on an unresisting target, after his perks. 0 = not measured.
        float         damage = 0.0f;            // health damage (a damage-over-time effect counted for its duration)
        float         heal = 0.0f;              // healing
        float         controlSec = 0.0f;        // Control: seconds its effect holds the target (the control effect's duration)
        // MFO-B684: the three estimates above were MEASURED for this entry (the engine fill ran, iaus/Fills.h). false =
        // the zeros mean "not known" and no damage consideration applies to this spell. true = a 0 is a real 0 (a spell
        // that does no health damage scores 0 against his best measured one).
        bool          measured = false;
        std::string   name;
    };

    // Inventory facts the drink and posture candidates read.
    struct Kit {
        std::int32_t potionsHealth = 0;
        std::int32_t potionsStamina = 0;
        std::int32_t potionsMagicka = 0;
        BuffStock    buff;           // every other beneficial potion effect he carries, by kind (iaus/PotionBuff.h; counts, best potion, live)
        std::int32_t arrows = 0;
        std::int32_t bolts = 0;
        bool melee = false;          // carries a melee weapon
        bool silver = false;         // carries a silver MELEE weapon
        bool bow = false;
        bool crossbow = false;
        bool staff = false;
        bool staffCharged = false;   // at least one carried staff has a cast left
        // Card I6 (marth Q64), MEASURED ONLY, NOT SCORED YET: Precision's attack-collision capsule length of his
        // right-hand weapon, in units (0 = Precision absent, refused, or nothing to measure). It is the weapon's
        // LENGTH from the hand, not a centre-to-centre reach, so Generate still reads Snapshot::meleeReach
        // (Docs/research/2026-11/iaus-i6.md "Precision reach").
        float precisionWeaponLength = 0.0f;
    };

    struct Book {
        std::vector<SpellEntry> entries;   // spells first, then scrolls, then staves
        Kit                     kit;
    };

    // ── The follower's class as the generator gates on it (design section 2) ──────────────────────────────
    // Plain flags so the simulator can write any class by hand. SpellBook::GateFor fills it from the EQUIPPED
    // class when the class tree is on (else his T1 class). With the tree OFF nothing is class-gated (marth Q63,
    // card I6): every flag below is open, as the gambit table leaves it today.
    struct ClassGate {
        std::array<float, kSchools> schoolBump{};   // marth 11-12: mastered casting classes add a small bump at their peak school (ClassCurves::ApplyMastered); 0 = none
        bool resolved = false;        // false: no class known yet -- weapons and spells are not gated
        bool treeOn = false;
        bool strict = false;          // strict adherence: the posture layer's weapon gate (G16)
        bool spellsOnClass = true;    // ruling Q34: a casting class (mage line or a hybrid)
        bool spellsFromMastered = false;   // ruling Q34: he has MASTERED a casting class
        bool scrollUser = true;       // ruling Q36
        bool powerAttack = false;     // a ch.18 power share or a power-attack default row
        float powerCooldownSec = 0.0f;   // that row's cooldown (10 s); 0 = none declared
        bool block = false;           // a Block default row (act.block; Q-melee-2: Knight, Great Knight, Ebony Warrior). false with the tree OFF
        float blockCooldownSec = 0.0f;   // that row's cooldown (4 s); 0 = none declared (Tuning::blockCooldownSec then)
        bool taunt = false;           // a taunt default row
        float tauntCooldownSec = 0.0f;   // that row's cooldown; 0 = none declared (Tuning::rowlessTauntCooldownSec then)
        bool disengage = false;       // Flag::kDisengage (Retreat)
        bool backOffPressed = false;  // batch I (Q-T2-3): the Mercenary backs off when two or more foes press him. Knight and Warrior keep no
                                      // way out by design. false with the tree OFF
        bool sneakClass = false;      // Flag::kSneakAlways: a sneak class (card G3's hide run). false with the tree OFF
        bool weaponMelee = true;      // the class weapon set holds a melee weapon (dagger / 1H / 2H)
        bool weaponBow = true;
        bool weaponCrossbow = true;
        bool weaponStaff = true;
        // Per-class curves (ruling Q-class-curves): which iaus/ClassCurves.h row this follower scores with (a Classes::Id ordinal;
        // kNoCurveClass = none resolved = the shared defaults), and the two class-relevant edge constants that row carries.
        // The defaults ARE the shared constants (Core.h kDrinkHealthLine, Reposition.h kTouchSpentSec: ClassCurves.cpp asserts it).
        std::uint8_t curveClass = 0xFF;
        float drinkHealthLine = kDrinkHealthLine;   // Generate.cpp: below it with a health potion in stock the Knight guard line is off
        float touchSpentSec = 3.0f;                 // Candidates.cpp: how long after a touch cast he is "backing out"
    };

    // ── One actor ─────────────────────────────────────────────────────────────────────────────────────────
    struct Combatant {
        std::uint32_t id = 0;
        std::string   name;
        float hp = 0.0f, hpMax = 0.0f;      // hpMax <= 0: unreadable, the generator skips this actor
        float mp = 0.0f, mpMax = 0.0f;
        float sp = 0.0f, spMax = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float dist = 0.0f;                  // from the follower (0 for the follower himself)
        int   level = 1;
        std::uint32_t targetId = 0;         // its current combat target, 0 = none
        bool  targetIsAlly = false;         // ...and that target is a party member other than the follower and the player
        bool  isPlayer = false;
        bool  downed = false;               // bleedout
        bool  paralysed = false;
        bool  fleeing = false;
        bool  blocking = false;
        bool  caster = false;               // a spell in either hand right now
        bool  rangedWeapon = false;         // a bow or crossbow in hand right now (it does not have to close to hurt)
        bool  swinging = false;             // its melee swing is in the air right now (started, not landed)
        bool  casting = false;              // it is charging a spell right now (a bash or a stagger stops the cast)
        bool  healer = false;               // party member: knows a heal it can put on another (the regroup anchor "to the healer")
        bool  undead = false;               // race keyword ActorTypeUndead
        bool  unholy = false;               // what the load order's silver hits
        bool  taunted = false;              // already under a live taunt (any taunter)
        bool  tauntedByMe = false;          // under a live taunt of THIS follower (Taunt::TauntedBy; the Knight line's guard count, Q-S-5)
        bool  outOfMeleeReach = false;      // airborne beyond a swing (CombatSense::OutOfMeleeReach)
        // G2 Layer 1 (iaus/Hide.h): the FOE's combat group's record of the follower. Yes = a live, positive detect level;
        // No = absent / lost / detect level <= 0 (hidden from this foe); Unknown = the foe has no group (and every actor that is not a foe).
        Tri   detectsMe = Tri::Unknown;
        // Estimates (iaus/Estimate.h). 0 = not measured, which every reader treats as unknown.
        float hit = 0.0f;                   // physical damage of ONE weapon hit before the target's armor
        float hitsPerSec = 0.0f;            // the engine's rate model: weapon speed x fCombatDPS(Melee|Bow)SpeedMult
        bool  hitIsMagic = false;           // the hit is a spell in hand (armor does not apply)
        int   incomingAv = -1;              // buff potions: the resist actor value of the element it hits with (the spell in its hand or the one it is
                                            // casting: the costliest hostile effect's resist), -1 = none known (Candidates.cpp FoeElementAv)
        float armor = 0.0f;                 // its physical mitigation, 0..1
        float magicResist = 0.0f;           // kResistMagic, points
        float lossPerSec = 0.0f;            // health it has been losing per second (observed)
        bool  beingHealed = false;          // a heal by ANOTHER party member is in flight on it (a party claim: no second heal)
        Sight sight = Sight::Unknown;       // the follower's OWN ray to it (spells, bows)
        Sight sightEngine = Sight::Unknown; // the engine basis (a melee swing)
        float occludedSec = 0.0f;           // seconds the OWN ray has been Occluded without a break (0 = not; Tuning::occlusionHoldSec)
        float occludedSecEngine = 0.0f;     // the same for the ENGINE ray (sightEngine): what a melee swing's los reads
        std::vector<std::pair<int, float>> resists;   // (resist actor value, its value) for the book's elements
        [[nodiscard]] float Resist(int a_av) const {
            for (const auto& r : resists)
                if (r.first == a_av) return r.second;
            return 0.0f;
        }
    };

    struct SpellTargetNote {   // a (spell, target) pair with a known answer
        std::uint32_t spell = 0;
        std::uint32_t target = 0;   // 0 = the follower himself
        Tri           answer = Tri::Unknown;
    };

    // ── The whole tick ────────────────────────────────────────────────────────────────────────────────────
    struct Snapshot {
        Combatant              self;
        std::vector<Combatant> foes;     // every foe that passed the P3 gates (own combat group, hostile, alive, loaded,
                                    // inside the chase cap). The top-5 cut is the generator's (pure).
        std::vector<Combatant> allies;   // the player (isPlayer) and the active followers other than the follower
        std::uint32_t playerId = 0;
        bool  selfSneaking = false;   // G2 Layer 1: Actor::IsSneaking() on the follower this lap, counted only when MFO owns the sneak
                                      // (bSneakInCombat on, or G1's stance claim live): with the defaults it stays false, as on main
        bool  hideRoad = false;       // G2: Harbinger can hold him still and quiet (ch.1 + ch.7). false: no hold row is generated
        std::uint32_t currentTargetId = 0;   // Targeting::Current, else his engine combat target
        std::uint32_t playerTargetId = 0;    // the player's combat target (design fact 8)

        bool hasBook = false;       // false: no fresh SpellBook yet -- no cast, drink or posture candidates
        Book book;

        PostureKind drawn = PostureKind::None;   // the weapon category in his hands now
        bool  meleeInRightHand = false;   // Taunt::MeleeInRightHand (the act.taunt offer gate)
        bool  meleeOnly = false;          // Actuation::MeleeOnly (the reach gate's subject)
        bool  handBusyLeft = false;       // a heal stands on that hand (design 1.3: never preempted)
        bool  handBusyRight = false;
        bool  scrollRoad = false;         // card S3: a scroll can be READ as a cast (the actuation exists). false (today):
                                          // scrolls stay in the book and no scroll candidate is made
        bool  groundRoad = false;         // Harbinger can step him back from a point (lab only today)
        bool  concealRoad = false;           // card G3: the hide run's road is open ([Debug] bIausHideRoad, Harbinger ABI 39)
        bool  concealPoint = false;          // ...and Concealment knows a fresh point (hidden from every listed foe; a flee: away from them)
        bool  fleeRun = false;               // card G3: a NON-sneak flee run stands (Concealment::FleeRunning): no offense is generated,
                                             // his own heals and health potions are boosted
        // batch I: the H3 runs that are not a hide or a flee. Each road is Concealment::RoadOpen() (the hide road switch, Harbinger ABI 39);
        // each point is a FRESH answer of Concealment's pick for that kind (asked only while the generator could use it).
        bool  backOffRoad = false, backOffPoint = false;
        bool  stepRoad = false, stepPoint = false;
        std::uint32_t equippedSpellOther = 0; // Q-I-2: the spell in the OTHER hand when it holds a different one; 0 = none
        std::uint32_t chosenCast = 0;         // Q-I-2: the form of the cast IAUS ranked first on its LAST decision (Candidates::NoteDecision); 0 = none
        double touchAgo = -1.0;               // seconds since the touch spell fired while it is the running decision; -1 = not applicable (Candidates::Build derives touchSpent from it with the class's touchSpentSec)
        bool  touchSpent = false;             // Q-I-2: the touch spell of his hands fired within Reposition::kTouchSpentSec (Mind::firedAt)
        std::uint32_t equippedSpell = 0;      // batch I H4: the spell in his right hand (else left); 0 = no spell in hand. Looked up in the book
        bool  freeEntry = false;              // batch I item 2: this fight is a guaranteed stealth-entry fight and the entry is not spent (the gather
                                              // already read foes the engine has no verdict on as unaware; logged by Classes::StealthEntry)
        bool  blockRoad = false;          // Harbinger serves the held guard (ch.31 kCombatMove_Block) and his hands can block now (batch I item 4)
        bool  guardHeld = false;          // a block request of his is out or his guard is up (APMFBridge::BlockHoldLive): the held-guard gate
                                          // (marth Q-melee-3) -- no attack, power attack, bash, taunt, cast or weapon change is generated
        bool  bashRoad = false;           // Harbinger can make him bash now (lab only today)
        bool  regroupRoad = false;        // the ch.24 regroup client exists (close on the player / a healer while fighting).
                                          // false (today): no regroup candidate is made
        bool  mortal = false;             // he dies FOR GOOD at 0 health (no bleedout he recovers from: not essential, not
                                          // protected, the list lets followers die). false = he goes down and stands up, or
                                          // it is not known. A mortal follower weighs his own life far more.
        bool  tauntEnabled = false;       // the taunt feature switch
        bool  tauntBusy = false;          // his taunt attempt is in flight or a foe is still pinned to him: act.taunt
                                          // refuses (Taunt::Fire), so no taunt candidate is made (card I6)
        float tauntReach = 0.0f;
        float meleeReach = 200.0f;        // fMeleeReach
        float swingReach = 0.0f;          // ruling Q68: the reach LEARNED for the weapon in his hand (iaus/Reach.h Gate()).
                                          // Read by the swing families ONLY (PowerAttack, Bash). 0 = not learned: they
                                          // read meleeReach. Everything else always reads meleeReach (Q70).
        float chaseCap = 4096.0f;         // Confidence::ChaseRadius
        float combatSec = 0.0f;           // seconds since this fight started for him

        std::vector<SpellTargetNote> willWork;      // G10 answers for the pairs asked last lap (Unknown passes)
        std::vector<SpellTargetNote> liveStreams;   // (spell, recipient) with a live direct stream: answer = Yes
    };

}
