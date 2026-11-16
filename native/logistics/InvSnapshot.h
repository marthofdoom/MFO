#pragma once
#include "PCH.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

// logistics/InvSnapshot.h -- THE WORKER'S INVENTORY READ (INVARIANTS #84, ENGINE_NOTES 0.30 amendment 2026-11-16).
//
// MFO's AddTask tick runs on a job worker (0.30). An engine inventory (InventoryChanges::entryList, a BSSimpleList of entries, each
// with a BSSimpleList of ExtraDataLists) has no lock a worker read would respect: the owner's own update, the container menu, other
// plugins and, since #83, MFO's own main-thread transfer drain all mutate it. So the worker never walks one. It reads a SNAPSHOT:
//   * built on the MAIN thread (the player-Update seat, MainThread.h), by the same walk RE::TESObjectREFR::GetInventory makes (the
//     changes list, then the base container, leveled entries ignored), into plain values. No ExtraDataList pointer is kept: a copy's
//     facts are copied out (count, worn, owner, unique id, charge, enchantment, health, display name).
//   * immutable once published (shared_ptr<const Snap>): a reader holds it for as long as it likes.
//   * TICK-STABLE: a worker tick reads the inventory as of its start. Get() never waits on the main thread: a ref it has no
//     readable snapshot of returns null ("not yet"), is asked for, and the next tick has it.
//   * a write MFO itself makes is told to the snapshot: Touch() after a worker-side write (the snapshot stays readable for the rest
//     of THAT tick, then is withheld until one built after the write exists), RebuildOnMain() after a main-side write (replaced in place).
//
// Threads: Get / Touch / BeginTick / EndTickPost from any thread (a leaf mutex, never held across an engine call or a build).
// BuildPass / RebuildOnMain / BuildLive read the engine and are MAIN ONLY, except BuildLive through Get when
// MainThread::LiveReadsAllowed() says this call may read live (the seat, the main thread outside a tick, or no pump at all).
//
// Freshness (principle 9, sized from the real cadence): the worker posts ONE build pass per tick (133 ms); the pass rebuilds the
// party and the player each time and a wanted source (container / corpse) when touched or kSourceRefreshTicks old.
//   * THE PARTY AND THE PLAYER are readable in tick t only from the pass tick t-1 posted (TickClock::needPass). That pass runs
//     behind every main-thread closure tick t-1 posted, so nothing MFO did or asked for in the last tick is missing from the
//     view, and a main thread slower than the tick simply makes the worker skip ticks (Diagnostics.cpp's tick gate).
//   * A SOURCE is readable while it is no older than kSourceMaxAgeTicks and was built since the last pause.
// A ref that fails its rule reads "not yet": the reader does nothing, it never acts on an inventory the main thread has not confirmed.
namespace MFO::InvSnap {

    // One ExtraDataList of an entry, by value. The order is the engine list's order.
    struct Copy {
        std::int32_t         n = 1;                // ExtraCount (1 when absent or <= 0): how many items this list stands for
        bool                 worn = false;         // ExtraWorn
        bool                 wornLeft = false;     // ExtraWornLeft
        bool                 hasOwnership = false; // an ExtraOwnership is present
        RE::TESForm*         owner = nullptr;      // ExtraOwnership::owner (a base NPC or faction form; null = none)
        bool                 hasUid = false;       // an ExtraUniqueID is present
        std::uint16_t        uid = 0;              // ExtraUniqueID::uniqueID
        RE::FormID           uidBase = 0;          // ExtraUniqueID::baseID
        bool                 hasCharge = false;    // an ExtraCharge is present
        float                charge = 0.0f;        // ExtraCharge::charge
        bool                 hasEnch = false;      // an ExtraEnchantment is present (ExtraDataType::kEnchantment)
        RE::EnchantmentItem* ench = nullptr;       // ExtraEnchantment::enchantment
        std::uint16_t        enchCharge = 0;       // ExtraEnchantment::charge (its maximum)
        bool                 hasHealth = false;    // an ExtraHealth is present
        float                health = 1.0f;        // ExtraHealth::health
        bool                 hasText = false;      // an ExtraTextDisplayData is present
        std::string          displayName;          // its displayName
        std::uint16_t        customNameLength = 0; // its customNameLength
        bool                 playerSetName = false;// its IsPlayerSet()
        bool                 cannotWear = false;   // ExtraCannotWear (ExtraDataType::kCannotWear): the prevent-removal lock
        bool                 questAlias = false;   // ExtraDataList::HasQuestObjectAlias() (the engine call) for THIS copy
        std::uintptr_t       token = 0;            // the list's ADDRESS as an identity value only. NEVER dereferenced off the main thread.
    };

    // One inventory entry (RE::InventoryEntryData), by value.
    struct Entry {
        RE::TESBoundObject* object = nullptr;
        bool                hasLists = false;      // the engine entry had an extraLists list (even an empty one)
        std::vector<Copy>   copies;                // one per non-null ExtraDataList
        bool                worn = false;          // InventoryEntryData::IsWorn()
        bool                questObject = false;   // InventoryEntryData::IsQuestObject() (the engine call)
        bool                enchanted = false;     // InventoryEntryData::IsEnchanted()
        std::int32_t        value = 0;             // InventoryEntryData::GetValue() (the engine call)
        [[nodiscard]] bool IsWorn() const noexcept { return worn; }
        [[nodiscard]] bool IsQuestObject() const noexcept { return questObject; }
        [[nodiscard]] bool IsEnchanted() const noexcept { return enchanted; }
        [[nodiscard]] std::int32_t GetValue() const noexcept { return value; }
    };

    // The same shape RE::TESObjectREFR::InventoryItemMap has (ordered by object pointer, `data.first` = the count, `data.second` a
    // never-null unique_ptr to the entry), so a reader's loop reads as it always did; the entry is a value copy, not the engine's.
    using ItemMap = std::map<RE::TESBoundObject*, std::pair<std::int32_t, std::unique_ptr<Entry>>>;

    struct Snap {
        RE::FormID    ref = 0;
        ItemMap       items;
        bool          live = false;                // built in place for this call (LiveReadsAllowed), never cached
        [[nodiscard]] const Entry* Find(const RE::TESBoundObject* a_obj) const;           // null when absent
        [[nodiscard]] std::int32_t Count(const RE::TESBoundObject* a_obj) const;          // 0 when absent
        // RE::Actor::GetWornArmor(slot) / GetWornArmor(formID), answered from the snapshot (same loop, same order).
        [[nodiscard]] RE::TESObjectARMO* WornArmor(RE::BGSBipedObjectForm::BipedObjectSlot a_slot) const;
        [[nodiscard]] RE::TESObjectARMO* WornArmor(RE::FormID a_formID) const;
    };
    using Ptr = std::shared_ptr<const Snap>;

    // The inventory of a_ref for THIS tick, or null = not yet (asked for; never built, withheld after a write, or too old).
    // Where MainThread::LiveReadsAllowed() it is read live, in place, and is never null for a non-null ref.
    // a_noInit = the caller's old `GetInventory(filter, true)` / `GetInventoryChanges(true)`: a ref with no container-changes extra is
    // NOT initialised by the walk (initialising resolves a container's leveled lists): only its base container objects are read.
    // A ref asked both ways is built the initialising way (the way every plain GetInventory() call always read it).
    [[nodiscard]] Ptr Get(RE::TESObjectREFR* a_ref, bool a_noInit = false);
    // THE READER'S HANDLE: Items(ref) is Get(ref) in the shape the old `ref->GetInventory()` map had. Iterating a not-yet view
    // visits nothing, and find() finds nothing, so a reader for which "not yet" may safely read as "nothing seen this tick" needs
    // no branch; one for which it may not tests the view first (`if (!inv) ...`). The view keeps the snapshot alive.
    class View {
    public:
        View() = default;
        explicit View(Ptr a_snap) : _snap(std::move(a_snap)) {}
        [[nodiscard]] explicit operator bool() const noexcept { return _snap != nullptr; }
        // Everything that hands out a pointer or an iterator INTO the snapshot is for a NAMED view only (`const auto inv = ...`):
        // the view owns the snapshot, so `InvSnap::Items(a).Find(x)` would return a pointer into a snapshot that dies with the
        // temporary. Those calls do not compile (the && overloads are deleted). Count / WornArmor return values and forms: fine.
        [[nodiscard]] const ItemMap& map() const& noexcept;
        const ItemMap& map() && = delete;
        [[nodiscard]] ItemMap::const_iterator begin() const& noexcept { return map().begin(); }
        [[nodiscard]] ItemMap::const_iterator end() const& noexcept { return map().end(); }
        [[nodiscard]] ItemMap::const_iterator find(const RE::TESBoundObject* a_obj) const& { return map().find(const_cast<RE::TESBoundObject*>(a_obj)); }
        ItemMap::const_iterator find(const RE::TESBoundObject* a_obj) && = delete;
        [[nodiscard]] bool empty() const noexcept { return !_snap || _snap->items.empty(); }
        [[nodiscard]] const Entry* Find(const RE::TESBoundObject* a_obj) const& { return _snap ? _snap->Find(a_obj) : nullptr; }
        const Entry* Find(const RE::TESBoundObject* a_obj) && = delete;
        [[nodiscard]] std::int32_t Count(const RE::TESBoundObject* a_obj) const { return _snap ? _snap->Count(a_obj) : 0; }
        [[nodiscard]] RE::TESObjectARMO* WornArmor(RE::BGSBipedObjectForm::BipedObjectSlot a_slot) const { return _snap ? _snap->WornArmor(a_slot) : nullptr; }
        [[nodiscard]] RE::TESObjectARMO* WornArmor(RE::FormID a_formID) const { return _snap ? _snap->WornArmor(a_formID) : nullptr; }
    private:
        Ptr _snap;
    };
    [[nodiscard]] View Items(RE::TESObjectREFR* a_ref, bool a_noInit = false);

    // Would Get(a_ref) answer now? Asks for it like Get does. The scheduler's lap gate.
    [[nodiscard]] bool Ready(RE::TESObjectREFR* a_ref);
    // Keep a_ref's snapshot built every pass (the party and the player, each tick). No read.
    void Want(RE::TESObjectREFR* a_ref);

    // MFO wrote a_ref's inventory ON THIS (worker) THREAD just now. Call it AFTER the write. The snapshot stays readable for the
    // rest of this tick and is then withheld until the main thread has built one after the write.
    void Touch(RE::TESObjectREFR* a_ref);
    void Touch(RE::FormID a_ref);
    // MAIN THREAD: MFO wrote a_ref's inventory on the main thread just now; replace its snapshot in place (no "not yet" gap).
    void RebuildOnMain(RE::TESObjectREFR* a_ref);

    // The worker tick's bracket (Diagnostics.cpp). BeginTick advances the clocks the freshness rules count in.
    // a_paused = RE::UI::GameIsPaused() at the tick's start. The player's Update (the main seat) does not run while the game is
    // paused, so no snapshot can be rebuilt then, and nothing the ENGINE owns moves either: the age clock stands still for a paused
    // tick (a snapshot does not age out under a menu), and the first UNPAUSED tick reads only snapshots built since the last
    // paused one (the player may have moved items in that menu). TickClock is what both snapshot kinds stamp and compare.
    // BuildPass is the main-thread half the tick's end posts: it builds what is wanted and due.
    void BeginTick(bool a_paused);
    struct TickClock {
        std::uint64_t tick = 0;         // every worker tick
        std::uint64_t age = 0;          // unpaused worker ticks only
        std::uint64_t lastPaused = 0;   // the last tick that began paused (0 = none)
        bool          paused = false;   // this tick began paused
        // THE PARTY'S RULE: a party member's (and the player's) snapshot is readable in this tick only when it was published by
        // the build pass of tick `needPass` or a later one. Unpaused: needPass = tick - 1, i.e. the pass the PREVIOUS tick posted
        // has run. That pass is queued behind everything that tick posted to the main thread, so "tick t reads what tick t-1 wrote
        // or posted" holds exactly (an equip, a pickup, a cast, a queued transfer). Paused: needPass = the tick before the pause
        // began, and it stands for the whole pause (no pass can run under it).
        std::uint64_t needPass = 0;
    };
    [[nodiscard]] TickClock Now();
    // Readable by the shared rule? a_builtTick / a_builtAge = the TickClock values stamped at the build; a_maxAge in unpaused ticks.
    [[nodiscard]] bool FreshEnough(const TickClock& a_now, std::uint64_t a_builtTick, std::uint64_t a_builtAge, std::uint64_t a_maxAge);
    [[nodiscard]] std::uint64_t CurrentTick();
    void BuildPass(std::uint64_t a_postTick);   // a_postTick = the tick whose end posted this pass (TickClock::tick then)
    // The party's rule (TickClock::needPass), for the effect mirror's party slots: a_builtPass = the a_postTick it was published by.
    [[nodiscard]] inline bool PassReached(const TickClock& a_now, bool a_everByPass, std::uint64_t a_builtPass) {
        return a_everByPass && a_builtPass >= a_now.needPass;
    }
    // Revert / load: drop everything (FormIDs are reused across loads).
    void Clear();

    // MAIN THREAD (or LiveReadsAllowed): the walk itself. Exposed for the main-side code that wants the same value shape.
    [[nodiscard]] Ptr BuildLive(RE::TESObjectREFR* a_ref, bool a_noInit = false);

    // One `[invsnap]` line per 60 s from the sleeper (Diagnostics.cpp): asks, not-yet answers by cause, builds, the worst age read.
    void LogStats();

}
