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

namespace MFO { struct FollowerState; }   // State.h; card C12 takes the follower's live tables by reference

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
    // spell is the follower's own known spell; card C12 resolves the role to a form, classes/Defaults.cpp).
    // Not serialized.
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
        float        cooldownSec;    // 0 = none. Applied by card C12 through CooldownTenthsFromSec (card G9's FLWR v6 row field)
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
        // nullopt = none stored (the class was never switched away from, or it is the equipped one: the switch
        // back CONSUMES the stored table); an engaged EMPTY vector is a real, stored empty table. The equipped
        // class's live table is FLWR's, not this.
        std::array<std::optional<std::vector<Gambit>>, kClassCount> tables{};
        // ── MCLS v2 (card C12; all default-empty, so a v1 record loads with none of it) ──────────────────
        // marth Q60: Dragonborn's player-picked T4 COMBAT BEHAVIOUR (an Id ordinal of a tier-4 class), or
        // kNoClass = none picked. Stored only; cards C8 / C10 read it.
        std::uint8_t pickedT4 = kNoClass;
        // marth Q29: the LOGISTICS table saved under each class, same rules as `tables` above.
        std::array<std::optional<std::vector<Gambit>>, kClassCount> logiTables{};
        // The tables he had with classes OFF (equipped == kNoClass), saved when his FIRST class is equipped and
        // handed back when his class is taken off again, so equipping a class never destroys the setup the
        // player built without classes. nullopt = none stored.
        std::optional<std::vector<Gambit>> offCombat;
        std::optional<std::vector<Gambit>> offLogistics;
    };

    // Which of a class's two stored tables (the values equal MFO::Table's, State.h).
    enum class TableKind : std::uint8_t { Combat = 0, Logistics = 1 };

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
    [[nodiscard]] bool HasStoredTable(RE::FormID a_follower, Id a_class, TableKind a_kind = TableKind::Combat);
    [[nodiscard]] std::optional<std::vector<Gambit>> GetStoredTable(RE::FormID a_follower, Id a_class,
                                                                    TableKind a_kind = TableKind::Combat);
    [[nodiscard]] std::uint8_t GetPickedT4(RE::FormID a_follower);   // marth Q60; kNoClass if none

    // Writes (false = refused: not Writable(), a 0xFF follower id, an Id out of range, or a table over
    // kMaxStoredRows). Masks drop bits beyond the roster.
    bool SetEquipped(RE::FormID a_follower, Id a_class);
    bool ClearEquipped(RE::FormID a_follower);
    bool SetProgress(RE::FormID a_follower, Id a_class, ClassProgress a_progress);
    bool SetMastered(RE::FormID a_follower, Mask a_mastered);
    bool SetUnlocked(RE::FormID a_follower, Mask a_unlocked);
    bool SetStealthFights(RE::FormID a_follower, std::uint16_t a_thief, std::uint16_t a_masterThief);
    bool StoreTable(RE::FormID a_follower, Id a_class, std::vector<Gambit> a_rows,
                    TableKind a_kind = TableKind::Combat);   // runtime-only fields (uid, last*) are zeroed
    bool ClearStoredTable(RE::FormID a_follower, Id a_class, TableKind a_kind = TableKind::Combat);
    // marth Q60: Dragonborn's picked T4. Refused unless a_t4 is a TIER-4 class the follower has MASTERED.
    bool SetPickedT4(RE::FormID a_follower, Id a_t4);
    bool ClearPickedT4(RE::FormID a_follower);

    // ── mastery = card C5 (classes/Mastery.cpp) ───────────────────────────────────────────────────────
    // Class level 1..cap per class; reaching the cap sets the mastered bit. CAPS BY TIER (marth 2026-11-10): T1 10, T2 15,
    // T3 20, T4 25. Dragonborn (hidden T5) has NO cap and NO level (LevelCap = 0). NO new persisted field: level lives in
    // ClassProgress::level, mastered / unlocked in the MCLS bitsets (layout untouched, MCLS stays v1).
    //
    // THREADING: the PURE functions (LevelCap, Scale01, LevelForXp, ComputeUnlocked) touch no state and are
    // callable from any thread. SetLevelFromXp (C4, job worker) and SwitchClass (C10, board thread) are each ONE compound
    // writer in State.cpp (ApplyLevel / ActivateClass): the whole read-decide-write happens under a single hold of the State.cpp
    // mutex, so a co-save snapshot never sees a half-applied level-up and two writers cannot lose each other's update. None of these
    // holds a lock across an engine call.
    //
    // RESET NOTE (future "reset class progress"): IsUnlocked takes the stored unlocked bits OR the rule-computed ones, and unlocks
    // are never revoked, so a reset must clear the stored unlocked (and mastered) bits explicitly, not just level / xp.
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
    // one), and refreshes the unlocked set from the mastered set, all in ONE State.cpp lock hold (ApplyLevel). Returns true if
    // anything changed. Dragonborn and a locked class are refused (false), and so is a table whose size is not LevelCap(class)
    // (logged as an error once per class per session: a short table could never reach mastery).
    bool SetLevelFromXp(RE::FormID a_follower, Id a_class, std::span<const std::uint32_t> a_cumulative);

    // Free, instant switch between UNLOCKED classes (C10's accessor). A class never used before is started at level 1 (Dragonborn
    // stays level 0). false if not Writable(), the class is not unlocked, or it is already equipped. It does NOT store or
    // restore the per-class gambit tables (Q8): the board calls EquipClass (card C12, below), which does both in one write.
    bool SwitchClass(RE::FormID a_follower, Id a_class);

    // The compound writers behind SetLevelFromXp / SwitchClass (State.cpp, one g_mx hold each; false = refused or nothing changed).
    // ApplyLevel: raise a_class to min(a_level, cap) (never lowers), set mastered at the cap, OR in the rule-computed unlocks.
    // ActivateClass: if a_class is unlocked and not already equipped, start it at level 1 when never used (Dragonborn stays 0)
    // and equip it.
    bool ApplyLevel(RE::FormID a_follower, Id a_class, std::uint8_t a_level);
    bool ActivateClass(RE::FormID a_follower, Id a_class);

    // ── class XP = card C4 (classes/Xp.cpp) + the aura values = card C11 (classes/Auras.cpp) ─────────────────────
    // A FIGHT WON = the party's combat ends with a hostile killed by the party (the follower, his commanded summon, the
    // player, or a party member he shared the kill with) and the party not wiped. XP is credited to a follower's EQUIPPED
    // class only, in whole points: participation + kills + assists + observed heals + summon uptime, times Config::g_classXpRate.
    // THE CURVE (marth 2026-11-10 pace "~3 h / 6 h / 10 h", the FE research's Master tier "12 to 14 h" -> 13 h): hours to master
    // by tier T1 3, T2 6, T3 10, T4 13, at ~10 fights an hour (BALANCE.md) and ~100 points a typical fight; XpToMaster(class)
    // = hours x 10 x 100, the cumulative threshold of level L is XpToMaster x ((L-1)/(cap-1))^1.3 (C5's LevelForXp contract).
    [[nodiscard]] std::uint32_t XpToMaster(Id a_class);                   // PURE; 0 for Dragonborn
    [[nodiscard]] std::span<const std::uint32_t> XpCurve(Id a_class);     // PURE, static storage; size == LevelCap, empty for Dragonborn

    // The compound XP writer (State.cpp, ONE g_mx hold): only while a_class is the follower's EQUIPPED class and not yet at its
    // cap, add a_points, recompute the level through a_cumulative, set mastered at the cap, OR in the rule-computed unlocks.
    // Any thread (the C4 award runs on the job worker). applied == false: refused (not Writable, not equipped, mastered, bad table).
    struct XpResult {
        bool          applied = false;
        std::uint32_t xp = 0;               // the stored total after the add
        std::uint8_t  levelBefore = 0, levelAfter = 0;
        bool          mastered = false;     // this award reached the cap
    };
    XpResult AwardXp(RE::FormID a_follower, Id a_class, std::uint32_t a_points, std::span<const std::uint32_t> a_cumulative);

    // The four credit feeds. Each is one line at its call site and does NOTHING unless Active() (and, for a follower, only
    // when he has a class equipped). THREADS: XpOnDeath and XpOnPartyFightEnd = the job worker (Rapport's gated death task,
    // CombatScan's party-out-of-combat teardown); XpNoteHeal = the main thread (HealObs verdict); all state is under Xp.cpp's
    // leaf mutex. The award itself (AwardXp) runs on the thread that ends the fight (the worker).
    void XpOnDeath(RE::Actor* a_victim, RE::Actor* a_killer);
    void XpNoteHeal(RE::FormID a_caster, RE::FormID a_target);
    void XpOnPartyFightEnd(RE::FormID a_follower);   // idempotent: every party-out-of-combat service calls it, it acts once per fight

    // Card C11 (classes/Auras.cpp): re-apply the MCM /s values to the aura ability effects after an MCM change. Any thread (it
    // posts the write to the main thread). OnDataLoaded applies them once at load itself.
    void OnConfigRead();

    // ── default gambit sets + per-class memory = card C12 (classes/Defaults.cpp) ─────────────────────────
    // marth Q8 / Q29 / Q59: equipping a class resets the follower's COMBAT table to that class's defaults the
    // FIRST time, and from then on each class remembers BOTH his tables (combat and logistics): switching away
    // stores the live tables under the class he leaves, switching back hands them back. The store, the equip and
    // the hand-back are ONE compound write in State.cpp (one g_mx hold); the live tables are then replaced here.
    //   - DEFAULTS: tier 2 = the class's applied DefaultGambit rows (ClassTable.cpp; a "todo.*" placeholder row
    //     and a pendingRuling row are never applied); tier 1 = today's base kit (Followers::ApplyDefaultKit's
    //     combat rows, marth 2026-11-09 "T1 keeps today's kit"); tier 3+ = NONE SHIPPED YET (parked by marth): the
    //     live combat table is carried over unchanged. There is no class default for the LOGISTICS table: a first
    //     equip and a reset leave it alone (the per-class memory still stores and restores it).
    //   - RANK (marth Q74): the follower gets the TOP rows that fit his rank's combat slots; OnRankUp appends the
    //     next ones when a rank-up opens slots, only while his table is still the untouched default prefix.
    //   - SPELLS: a default cast row names a SpellRole. The row is added DISABLED with no spell, the follower's own
    //     known spell for the role is picked on the MAIN thread (MainThread::Post) and written back on the worker,
    //     which enables the row. No spell for the role: the row stays disabled, logged, for the player to fill.
    //   - CLASSES OFF: his first class equip saves the tables he had without classes; UnequipClass hands them back.
    // THREADING: all four are WORKER-ONLY (they read and write a_st, which lives in g_followers). They refuse and
    // change NOTHING unless Writable() (so with the ESL absent or bClassTree = 0 they are inert).
    // EquipClass / UnequipClass call Followers::RepublishActiveMirror() themselves (the C10 HOOK below).
    enum class EquipResult : std::uint8_t {
        Refused,    // not Writable(), a 0xFF id, locked, already equipped, or a live table the store cannot hold
        Restored,   // the class had stored tables: handed back
        Defaults,   // first equip: the class's default combat rows
        Kept        // first equip of a class with no shipped default set (tier 3+): the live tables carried over
    };
    EquipResult EquipClass(RE::FormID a_follower, Id a_class, FollowerState& a_st);
    // Classes OFF for this follower (equipped -> kNoClass). true = done; the pre-class tables come back when stored.
    bool UnequipClass(RE::FormID a_follower, FollowerState& a_st);
    // The reset action (C10's button): the equipped class's default combat rows again. false = refused (not
    // Writable(), no class equipped, or a tier 3+ class with no shipped default set).
    bool ResetClassTables(RE::FormID a_follower, FollowerState& a_st);
    // Rapport.cpp, on a rank INCREASE (a_before = the old rank; a_st.rank is the new one). marth Q74.
    void OnRankUp(RE::FormID a_follower, FollowerState& a_st, std::uint8_t a_before);

    // NO class Auto (marth 2026-11-10: "Auto is disabled per follower when classes are enabled on that follower"). The C5 stat-based
    // picker (AutoClassFor / PickAutoClass / ReadStats) was removed in card C5b: no class is ever auto-assigned. A follower with
    // classes ON (equipped != kNoClass) skips the Gambit-tab Auto vote in Logistics::ResolveBaseClass and takes his equipped
    // class's rootT1 instead.
    // C2b: EVERY reader of the class pick (not just the vote) goes through Logistics::EffectiveClassPick (logistics/ClassResolve.cpp):
    // classes ON -> his equipped class's root T1 (Dragonborn: Q60), else the raw combatClassOverride. The raw value is never written on
    // class equip. C10 HOOK: after a SwitchClass / ActivateClass / SetEquipped / ClearEquipped and after a bClassTree toggle (NOT after
    // EquipClass / UnequipClass, which call it themselves), call
    // Followers::RepublishActiveMirror() on the WORKER (the ResolvedClassFast mirror the main-thread HMS poll reads is rebuilt by it;
    // it is NOT called from State.cpp because it reads g_followers, worker-only). Until then the mirror catches up at the next
    // Followers::Refresh (its tail republishes). Direct readers on the worker see an equip change at once (no cache).

    // ── passive carriers = card C6 (classes/Carriers.cpp) ─────────────────────────────────────────────
    // The ESL's passive PERKS are gated on each follower's rank in the class faction MFOC_P_<Name> (the C7 RANK SPACE): 0 / absent
    // OFF, 1..cap ACTIVE at class level N, kRankMasteredUnequipped = mastered and unequipped (1/4 of the FULL value). CarrierRanks
    // turns a follower's class state into those ranks; the reconcile (main thread) writes them, grants the perks ONCE (TESNPC::AddPerk,
    // a runtime base mutation, so re-granted every session) and adds the Mercenary / Scout ability spells.
    //   - equipped == kNoClass = classes OFF for that follower (marth 2026-11-10): every rank 0, nothing applies, mastered quarters included;
    //   - the equipped class: rank = its level (clamped to 1..cap; a never-levelled equipped class counts as level 1, MFO-B627);
    //     a MASTERED equipped class is at the cap (full);
    //   - every other mastered class: kRankMasteredUnequipped;
    //   - a T4 equipped: its T3 parents (mastered members of its unlock rule, first `need` in Id order for an N-of pool) at the cap = full;
    //   - Dragonborn equipped (no passive, no level): EVERY mastered class at its cap = full (marth: all mastered passives at full).
    // The same passive is one faction, so it can never double (max rank wins by construction).
    inline constexpr std::uint8_t kRankMasteredUnequipped = 100;
    [[nodiscard]] std::array<std::uint8_t, kClassCount> CarrierRanks(const FollowerClasses& a_fc);   // PURE

    // Queue a carrier re-apply for one follower / every stored follower. ANY thread (they only queue; the engine work runs on the
    // MAIN thread through MainThread::Post, retried until the actor is loaded). No-ops unless the class ESL is Present(); with the
    // ESL present but bClassTree = 0 the pass REMOVES the carrier abilities and zeroes the ranks instead (teardown).
    void RefreshCarriers(RE::FormID a_follower);
    void RefreshAllCarriers();
    // Drop the pending set / granted-perk memory and retire the pump (a new generation). ResetAllState / LoadCallback, via ClearAll.
    void ResetCarriers();

#ifdef MFO_DEV_SELFTEST
    // DEV BUILDS ONLY (native/selftest/TestsDrive.cpp class.* tests): a read-back of what the C6 carrier reconcile actually put on
    // the actor, to compare with CarrierRanks(). MAIN THREAD. resolved = false when the ESL forms are not resolved / the actor does not
    // resolve. ranks[] are the MFOC_P_<Name> faction ranks as read (absent / negative = 0).
    struct DevCarrierView {
        bool          resolved = false;
        bool          processLive = false;                            // a middle-high process (live perk entries are only counted with one)
        std::array<std::int32_t, kClassCount> ranks{};
        std::array<bool, kClassCount>         hasFaction{};                  // a MFOC_P_<Name> faction exists for the class (a class without one reads 0)
        bool          mercFull = false, mercQuarter = false, scoutQuarter = false;
        std::int32_t  scoutLevel = 0;                                  // 1..N = the Scout speed ability present (0 none)
        std::int32_t  perksOnBase = 0, perksExpected = 0;              // carrier perks on the base NPC / the number of carrier perks
        std::int32_t  entriesExpected = 0, entriesLive = 0;            // rank-1 entry-point entries the perks declare / found live on the actor
    };
    DevCarrierView DevReadCarriers(RE::FormID a_follower);
#endif
}
