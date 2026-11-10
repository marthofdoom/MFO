#pragma once
#include "PCH.h"
#include "State.h"   // Gambit: the stored per-class tables (card C3)
#include <array>
#include <optional>
#include <span>

// CLASS ADD-ON (native/classes/): the public face of the class tree. Cards C1 (skeleton, presence,
// INI toggles) and C2 (the compiled class data model) of the class update. ClickUp 86e3dhbd3.
//
// WHAT THIS IS: the add-on's DLL half. The class ESL (MFO_Classes.esl, card C7, NOT shipped yet) is
// detected by a manifest keyword; everything here is INERT unless it is present AND bClassTree = 1,
// and then today's three classes (Melee / Ranged / Mage, State.h combatClassOverride) behave exactly
// as before. This header is the ONLY thing another subsystem may include.
//
// WHAT IT IS NOT (yet): no XP (C4), no passives (C6), no GUI (C10); the co-save record (C3) holds state
// but nothing writes it yet. The table below
// is DATA: nothing reads it at runtime except the load-time self-check and the accessors.
//
// PRESENCE CONTRACT (for card C7, the ESL generator): the ESL ships ONE BGSListForm (FLST) whose FIRST
// entry is a KEYWORD whose editor-id ENDS WITH "_MFOClassesManifest". It is deliberately NOT the
// progression add-on suffix "_MFOAddonManifest": Progression::Init enumerates every FLST with that
// suffix and would register the class ESL as a progression add-on. Keyword editor-ids persist at
// runtime (FLST/GLOB ones do not), which is why the keyword carries the match, same as the precedent
// in Progression.cpp. No MFO.esp form is referenced, so the ESL needs no MFO.esp master.
//
// THREADING: OnDataLoaded runs once on the main thread at kDataLoaded. Every accessor after that is a
// read of immutable constexpr data or of two atomics, safe from any thread.

namespace MFO::Classes {

    // 35 classes. The ORDER is the bit index into every mastered/unlocked bitset (C3 will persist those),
    // so it is a future co-save ABI: append only, never reorder or insert. T1 first: the Id ordinals 0..2 are
    // Melee/Ranged/Mage and T1ForOverride maps State.h combatClassOverride 1..3 (0 = Auto) onto them.
    enum class Id : std::uint8_t {
        // tier 1
        Melee, Ranged, Mage,
        // tier 2
        Knight, Warrior, Mercenary, Archer, Thief, Evoker, Conjurer, Healer, Warlock,
        // tier 3, pure
        GreatKnight, Champion, Blademaster, Ranger, MasterThief, Wizard, Necromancer, Priest,
        // tier 3, hybrids
        Spellsword, Sorcerer, Templar, Nightblade, Witchhunter, Scout,
        // tier 4
        ArchMage, LivingLegend, Nightingale, Listener, EbonyWarrior, GrandBattlemage, Jaqspur, Saint,
        // tier 5 (hidden)
        Dragonborn,
        kCount
    };
    inline constexpr std::size_t kClassCount = static_cast<std::size_t>(Id::kCount);
    static_assert(kClassCount == 35, "the roster is 35 classes (marth 2026-11-09)");

    using Mask = std::uint64_t;   // one bit per Id
    constexpr Mask Bit(Id a_id) { return Mask{ 1 } << static_cast<unsigned>(a_id); }

    enum class Line : std::uint8_t { Melee, Ranged, Mage, Hybrid };

    // The T1 root a class stands on, as the State.h combatClassOverride ordinal (FROZEN: 0 Auto, 1 Melee,
    // 2 Ranged, 3 Mage; equal to CombatStyle::Stance, asserted in Classes.cpp). The equipped T2+ class's
    // root drives the existing stance and Auto logic. 0 = no fixed root (Dragonborn copies its strongest
    // mastered T4 at runtime).
    using RootT1 = std::uint8_t;

    // Allowed weapon set (a bit set). Melee 1H/2H are merged except the dagger (marth). A shield is a
    // weapon-set bit so the defensive-only rule is data. kUnrestricted = no limits (T1, Dragonborn).
    namespace Weapon {
        inline constexpr std::uint16_t kDagger   = 1 << 0;
        inline constexpr std::uint16_t kOneHand  = 1 << 1;   // 1H melee that is not a dagger
        inline constexpr std::uint16_t kTwoHand  = 1 << 2;
        inline constexpr std::uint16_t kBow      = 1 << 3;
        inline constexpr std::uint16_t kCrossbow = 1 << 4;
        inline constexpr std::uint16_t kStaff    = 1 << 5;
        inline constexpr std::uint16_t kShield   = 1 << 6;
        inline constexpr std::uint16_t kUnrestricted = 1 << 15;
    }

    // Package flags (the class's Harbinger / CSTY shape; consumed by card C8, nothing reads them yet).
    namespace Flag {
        inline constexpr std::uint16_t kShortLeash   = 1 << 0;   // ch.23 pursuit leash short (the wall classes)
        inline constexpr std::uint16_t kNearPlayer   = 1 << 1;   // leashed near the player (healers)
        inline constexpr std::uint16_t kDisengage    = 1 << 2;   // may disengage with regular movement (H3: Thief and mage lines)
        inline constexpr std::uint16_t kSpellRange   = 1 << 3;   // range follows the equipped spell (H4)
        inline constexpr std::uint16_t kSneakAlways  = 1 << 4;   // sneak regardless of player action (G1)
    }

    // Hybrid / T4 go-in rule id (card C9). None = no special rule (the class's normal package decides).
    enum class GoIn : std::uint8_t {
        None,
        Never,                    // ranged hybrids that never go in (Witchhunter)
        BiasHealthMagickaRange,   // Spellsword: a health / magicka / range bias picks IN or RANGE each tick
        FullSummonsHighHealth,    // Sorcerer: only when high health and full summons
        PartyHealthyOrNoMagicka,  // Templar (and Ebony Warrior): only when all the party is healthy, or no magicka
        ControlledFoesOnly        // Nightblade: only on controlled foes, or when none can be controlled
    };

    // What a cast gambit row wants for its spell. The class defaults name a ROLE, never a spell form (a
    // spell is the follower's own known spell; card C12 resolves the role to a form). Not serialized.
    enum class SpellRole : std::uint8_t {
        None, Attack, Destruction, Fire, Frost, Shock, Area, Heal, Summon, BoundWeapon, Ward, Armor,
        Paralyze, Fear, Calm, Reanimate, TurnUndead
    };

    // One default gambit row. cond/act are EXISTING base opcode strings (Vocabulary.h) when planCard is
    // nullptr. A row that needs an opcode that does not exist yet carries a "todo.*" placeholder name and a
    // planCard naming the build-plan card that adds it; such a row is NOT applied (IsApplied() is false).
    struct DefaultGambit {
        const char*  cond;
        const char*  act;
        float        condParam;      // pct params are fractions [0,1], distances are units, like Gambit::conditionParam
        std::uint8_t subject;        // Vocab::Subject ordinal (0 Self/Auto ... 4 Caster)
        SpellRole    role;           // cast rows only
        float        cooldownSec;    // 0 = none. NOT applied until card G9 (per-rule cooldown, FLWR v6) exists
        const char*  planCard;       // nullptr = applied; otherwise the card that adds the missing opcode
        bool         pendingRuling;  // a T1 row (marth 2026-11-09: T1 keeps today's kit, no class default set): C12 must not apply it. (Enemy-control rows were also flagged until AUTO-hostile landed, 1dfcuabcr0d.)
        [[nodiscard]] constexpr bool IsApplied() const { return planCard == nullptr; }
    };

    // Unlock rule: ALL of allOf must be mastered, AND at least needA of poolA, AND at least needB of poolB (each constraint is
    // counted independently, pools may overlap allOf and each other). T2 = its root T1 only; pure T3 = own T2 + one same-line T2;
    // hybrids = one weapon class + one school class; T4 = fixed pair or N of a set; T5 = 3 distinct T4.
    struct UnlockRule {
        Mask         allOf;
        Mask         poolA;
        std::uint8_t needA;
        Mask         poolB;
        std::uint8_t needB;
    };

    inline constexpr float kShareUnset = -1.0f;   // ch.18 powerShare not declared for this class

    struct ClassDef {
        Id                 id;
        std::string_view   slug;        // lowercase-hyphen, the review page's db doc ids ("great-knight")
        std::string_view   name;        // display name
        std::uint8_t       tier;        // 1..5
        Line               line;
        RootT1             rootT1;
        UnlockRule         unlock;
        Mask               parentMask;  // every class named by `unlock` (candidates, for "two of X/Y/Z")
        std::uint16_t      weapons;     // Weapon:: bits
        const DefaultGambit* gambits;
        std::uint8_t       gambitCount;
        std::string_view   passive;     // passive id (card C6 turns it into an ESL perk); one passive per class
        float              powerShareMax;   // ch.18 cap (kShareUnset = none)
        float              powerShareMin;   // ch.18 floor (kShareUnset = none)
        std::uint16_t      flags;       // Flag:: bits
        GoIn               goIn;
        std::uint8_t       partyHealthyPct;   // the "party healthy" threshold in percent (0 = none; 70 for Templar, Scout, Ebony Warrior, Saint)
    };

    // ── state ────────────────────────────────────────────────────────────────────────────────────────
    // Call once at kDataLoaded (after Forms::Resolve and Progression::Init). Detects the class ESL, logs
    // presence once, and runs the table self-check (logs counts per tier). Never throws.
    void OnDataLoaded();

    // The class ESL's manifest was found this session.
    [[nodiscard]] bool Present();
    // The tree is live: ESL present AND bClassTree = 1. Everything the add-on does gates on this.
    [[nodiscard]] bool Active();
    // Strict adherence: Active() AND bClassStrict = 1.
    [[nodiscard]] bool Strict();

    // ── data ─────────────────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] const ClassDef& Get(Id a_id);
    [[nodiscard]] const ClassDef* FindBySlug(std::string_view a_slug);   // nullptr if unknown
    // Mastered-set check for the unlock rule (pure; C5 supplies the mastered bitset).
    [[nodiscard]] bool UnlockSatisfied(const ClassDef& a_def, Mask a_mastered);
    // The T1 class (Melee/Ranged/Mage) for a combatClassOverride ordinal 1..3; nullptr for 0 / out of range.
    [[nodiscard]] const ClassDef* T1ForOverride(std::uint8_t a_combatClassOverride);

    // ── per-follower class state = the 'MCLS' co-save record (card C3, classes/State.cpp) ─────────────
    // Byte layout, version rules and the ESL-absent behaviour: the header of classes/State.cpp. Keyed on the
    // follower's PERSISTENT FormID (like g_followers / PGIV); a 0xFF runtime id is refused (#9).
    //
    // THREADING: one mutex inside State.cpp guards all of it, so every function below is callable from any
    // thread (the C4 XP credit runs on the job worker, the board on its own thread). The lock is never held
    // across an engine call or a WriteRecordData. Getters return COPIES.
    //
    // WRITABILITY: every Set*/Store*/Clear* returns false and changes NOTHING unless Writable(): the tree is
    // Active() AND the loaded record was parsed (not kept verbatim as newer-than-this-DLL). So with the ESL
    // absent or bClassTree = 0, the loaded progress is held unchanged and written back unchanged (marth Q9).

    inline constexpr std::uint8_t kNoClass = 0xFF;          // FollowerClasses::equipped: nothing equipped yet
    inline constexpr std::size_t  kMaxStoredRows = 64;      // rows per stored class table (#11; combat Rank V is 12)

    struct ClassProgress {
        std::uint32_t xp = 0;      // class XP in whole POINTS (card C4 picks the point scale; integer so a
                                   // save/load never drifts and the curve compares exactly)
        std::uint8_t  level = 0;   // 0 = never levelled; card C5 owns 1..cap
        bool operator==(const ClassProgress&) const = default;
    };

    struct FollowerClasses {
        std::uint8_t equipped = kNoClass;                     // an Id ordinal, or kNoClass
        std::array<ClassProgress, kClassCount> progress{};   // indexed by Id
        Mask          mastered = 0;
        Mask          unlocked = 0;
        std::uint16_t thiefFights = 0;         // card C13: fights since the Thief's last guaranteed stealth entry
        std::uint16_t masterThiefFights = 0;   // card C13: same, Master Thief
        // marth Q8: the COMBAT gambit table saved under each class when he switched away from it (card C12).
        // nullopt = none stored (the class was never switched away from); an engaged EMPTY vector is a real,
        // stored empty table. The equipped class's live table is FLWR's, not this.
        std::array<std::optional<std::vector<Gambit>>, kClassCount> tables{};
    };

    // Co-save (Serialization.cpp calls these and nothing else does).
    void CoSave(SKSE::SerializationInterface* a_intfc);
    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
    void BeginLoad();   // LoadCallback start: clear (belt and braces) + the bClassCoSaveSelfTest round trip
    void ClearAll();    // ResetAllState, after StopPump

    [[nodiscard]] bool Writable();
    // True ONCE after a load that kept a newer MCLS record verbatim (class progress read-only this session);
    // plugin.cpp shows the on-screen notice at kPostLoadGame (review F-1).
    [[nodiscard]] bool ConsumeReadOnlyNotice();

    // Reads (copies).
    [[nodiscard]] std::optional<FollowerClasses> GetFollower(RE::FormID a_follower);   // nullopt = no record
    [[nodiscard]] std::uint8_t  GetEquipped(RE::FormID a_follower);                     // kNoClass if none
    [[nodiscard]] ClassProgress GetProgress(RE::FormID a_follower, Id a_class);         // {} if none
    [[nodiscard]] Mask GetMastered(RE::FormID a_follower);
    [[nodiscard]] Mask GetUnlocked(RE::FormID a_follower);
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> GetStealthFights(RE::FormID a_follower);   // {thief, masterThief}
    [[nodiscard]] bool HasStoredTable(RE::FormID a_follower, Id a_class);
    [[nodiscard]] std::optional<std::vector<Gambit>> GetStoredTable(RE::FormID a_follower, Id a_class);

    // Writes (false = refused: not Writable(), a 0xFF follower id, an Id out of range, or a table over
    // kMaxStoredRows). Masks drop bits beyond the roster.
    bool SetEquipped(RE::FormID a_follower, Id a_class);
    bool ClearEquipped(RE::FormID a_follower);
    bool SetProgress(RE::FormID a_follower, Id a_class, ClassProgress a_progress);
    bool SetMastered(RE::FormID a_follower, Mask a_mastered);
    bool SetUnlocked(RE::FormID a_follower, Mask a_unlocked);
    bool SetStealthFights(RE::FormID a_follower, std::uint16_t a_thief, std::uint16_t a_masterThief);
    bool StoreTable(RE::FormID a_follower, Id a_class, std::vector<Gambit> a_rows);   // C12: on switching away; runtime-only fields (uid, last*) are zeroed
    bool ClearStoredTable(RE::FormID a_follower, Id a_class);                          // C12: the reset button

    // ── mastery = card C5 (classes/Mastery.cpp) ───────────────────────────────────────────────────────
    // Class level 1..cap per class; reaching the cap sets the mastered bit. CAPS BY TIER (marth 2026-11-10): T1 10, T2 15,
    // T3 20, T4 25. Dragonborn (hidden T5) has NO cap and NO level (LevelCap = 0). NO new persisted field: level lives in
    // ClassProgress::level, mastered / unlocked in the MCLS bitsets (layout untouched, MCLS stays v1).
    //
    // THREADING: the PURE functions (LevelCap, Scale01, LevelForXp, ComputeUnlocked, PickAutoClass) touch no state and are
    // callable from any thread. The follower functions (SetLevelFromXp, SwitchClass, IsUnlocked, AutoClassFor) go through the
    // State.cpp accessors, so each single read/write is under its lock, but a function that does read-then-write is NOT atomic
    // as a whole: the XP writer (C4, job worker) is the ONLY caller of SetLevelFromXp, and the GUI (C10, board thread) only
    // calls SwitchClass (one write: SetEquipped). AutoClassFor reads actor values with the same calls ClassResolve.cpp makes
    // (call it where ResolveBaseClass is called). None of these holds a lock across an engine call.
    //
    // WRITES refuse (return false, change nothing) unless Writable(), which already implies Active(); with the ESL absent
    // they are inert, and the reads return the empty / no-class answers.

    // The level cap of a class by its tier; 0 for Dragonborn (no cap, no level).
    [[nodiscard]] std::uint8_t LevelCap(Id a_class);

    // Strong-effect scaling for card C6 (T3 STRONG effect and T4 UNIQUE mechanic; T1/T2 and the T3 smaller effect stay flat):
    // 0.25 + 0.75 * (level - 1) / (cap - 1), so 0.25 at level 1 and 1.0 at the cap. level <= 0 (never levelled) returns 0;
    // level > cap is clamped to 1.0; cap <= 1 returns 1.0 for level >= 1. PURE, writes no carrier (C6 owns the carriers).
    // The mastered-but-unequipped quarter is applied by C6 on top of the FULL value (not of this scale).
    [[nodiscard]] float Scale01(std::uint32_t a_level, std::uint32_t a_cap);

    // Level from XP, given per-level thresholds. C4 CONTRACT: a_cumulative[i] is the TOTAL class XP (in the same whole points as
    // ClassProgress::xp) needed to BE at level i+1, ascending (non-decreasing); a_cumulative[0] is normally 0 (level 1 is the
    // starting level once a class is in use); a_cumulative.size() is the level cap for that class (LevelCap(class) entries,
    // C4 sizes the curve per tier from the 3 h / 6 h / 10 h targets). Returns the highest level whose threshold a_xp has
    // reached (1..size); 0 for an empty span. PURE.
    [[nodiscard]] std::uint8_t LevelForXp(std::uint32_t a_xp, std::span<const std::uint32_t> a_cumulative);

    // The unlocked set that follows from a mastered set: every class whose UnlockRule is satisfied (T1 always, the table's
    // rules for the rest, Dragonborn included). PURE; an unlock never needs the old unlocked bits.
    [[nodiscard]] Mask ComputeUnlocked(Mask a_mastered);

    // Is a_class usable by the follower now: its bit is in the stored unlocked set OR the rule holds for the stored mastered
    // set (so a follower with no record still has the three T1s). false unless Active().
    [[nodiscard]] bool IsUnlocked(RE::FormID a_follower, Id a_class);

    // C4 -> C5 hook: after C4 has stored new XP with SetProgress, call this with the class's cumulative thresholds. It
    // recomputes the level (never lowers it), clamps to the cap, sets the mastered bit when the cap is reached (never clears
    // one), and refreshes the unlocked set from the mastered set. Returns true if anything changed. Dragonborn and a locked
    // class are refused (false).
    bool SetLevelFromXp(RE::FormID a_follower, Id a_class, std::span<const std::uint32_t> a_cumulative);

    // Free, instant switch between UNLOCKED classes (C10's accessor). A class never used before is started at level 1 (Dragonborn
    // stays level 0). false if not Writable(), the class is not unlocked, or it is already equipped. Storing / restoring the
    // per-class gambit table (Q8) is card C12's, not done here.
    bool SwitchClass(RE::FormID a_follower, Id a_class);

    // The stat inputs the Auto resolver votes on: BASE actor values (a fortify potion cannot flip it), like ClassResolve.cpp.
    struct StatProfile {
        float oneHanded = 0, twoHanded = 0, archery = 0;
        float alteration = 0, conjuration = 0, destruction = 0, illusion = 0, restoration = 0;
    };
    [[nodiscard]] StatProfile ReadStats(RE::Actor* a_actor);   // zeros for nullptr

    // Pick the best-fitting class among a_unlocked from a_stats. A class's fit is the mean of its skill groups (Melee = better of
    // 1H / 2H, Ranged = Archery, Mage = best of five schools): a line class uses its own group, a hybrid uses its weapon-side
    // root plus Mage. Highest fit wins; equal fits go to the HIGHER tier, then the lower Id. Dragonborn is never auto-picked.
    // nullopt when a_unlocked holds no pickable class. PURE.
    [[nodiscard]] std::optional<Id> PickAutoClass(const StatProfile& a_stats, Mask a_unlocked);

    // Strict adherence: with Strict() (ESL present, bClassTree = 1, bClassStrict = 1) resolve an Auto follower's class from its
    // stats among its unlocked classes. nullopt (no opinion, today's three-class behaviour) when not Strict(). Writes nothing:
    // the caller (when combatClassOverride == 0) decides whether to SwitchClass to it.
    [[nodiscard]] std::optional<Id> AutoClassFor(RE::FormID a_follower, RE::Actor* a_actor);
}
