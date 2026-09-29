// logistics/Lotd.cpp -- LOTD AWARENESS (feat/mfo-lotd, ClickUp 86e3edghj, rounds
// L1-L3). The module doc is in logistics/Lotd.h; the research and design are in
// _research/lotd-design-2026-09-24.md. marth's rulings (2026-09-24): "It will be a
// loot gambit. 'Loot museum items' will appear as an option when LoTD is enabled.
// This automatically enables the depositing behaviour as well (museum items cannot
// be sold if needed)." "Trigger the message when LOTD awareness is triggered on.
// Pick a box and initialize through it." The deposit is automatic.
//
// EVERY LOTD FORM BELOW IS VERIFIED against the installed LegacyoftheDragonborn.esm
// 6.9.0 (Tuxborn) and 6.10.0 (Authoria): identical local IDs, types and editor IDs
// in both (agent log mfo-lotd.md). IdleGive is Skyrim.esm 0x0B5E20 (IDLE, no CTDA).
//
// THREADING.
//   * MAIN thread: Detect, RegisterSinks (the sink itself only posts), OnPostLoad,
//     the snapshot rebuild (the Papyrus VM read), the shipping intro, the ActivateRef
//     and the deposit transfer (RemoveItem into the crate). All reached through
//     plugin.cpp's messaging handler or MainThread::Post.
//   * WORKER (the AddTask job worker, serial): RunGambit, DepositTick, EndDeposit,
//     HoldFromSale, LootMuseum, the needs cache. Serial, so the needs cache and the
//     trip need no lock among themselves; the trip still takes g_tripMx because
//     EndDeposit can also arrive from a dismissal edge.
//   * Shared: the snapshot (g_snapMx, an immutable shared_ptr swap), the in-transit
//     ledger (g_ledgerMx: written by the main-thread transfer, read by the worker),
//     and plain atomics for the flags the render thread reads (GambitOffered).
// NOTHING IS SAVED (no co-save record, no new serialized field).
#include "PCH.h"
#include "Logistics.h"
#include "Logistics_internal.h"   // LootNearby, Category, SlotIndexOf, FitsCarryWeight, IsStockGear, ...
#include "Lotd.h"
#include "Config.h"
#include "Forms.h"
#include "Followers.h"          // ActiveSnapshot: the follower supply (safe from any thread)
#include "ItemCatalog.h"        // Catalog::IsExcluded: bLootSpecialItems parity with the other looters
#include "MainThread.h"
#include "TeleportCompat.h"     // the confidence leash (the crate must be inside it, like a loot target)
#include "apmf/APMFBridge.h"    // the deposit trip's Harbinger claims (apmf/Deposit.cpp)
#include "cast/Actuation.h"      // ForcedHoldFor: a worn kept relic under an MFO hold is not shipped (MFO-B126)
#include "Loadout.h"             // LeftHandSlot: a worn LEFT-hand relic is unequipped from that slot before the transfer
#include "PlayerGiven.h"         // a player-given item is never shipped (batch L review round)
#include "Scheduler.h"           // ServiceClock(): the UNPAUSED clock the give idle's confirmation window runs on

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Lotd {

    namespace {
        constexpr std::string_view kLotdPlugin = "LegacyoftheDragonborn.esm";

        // ── LOTD local FormIDs (verified, see the file banner) ──────────────────
        constexpr RE::FormID kQuestMuseumApi   = 0x138793;   // QUST DBM_MuseumUtility (script DBM_MuseumAPI)
        constexpr RE::FormID kContOutgoing     = 0x1772A7;   // CONT DBMMuseumShipmentsCrateOutgoing
        constexpr RE::FormID kRefDropoffCrate  = 0x1772AA;   // REFR DropoffCrate (persistent, Hall of Heroes)
        constexpr RE::FormID kRefAutoSort      = 0x07EF00;   // REFR display drop-off (persistent, base 0x07EEFD)
        constexpr RE::FormID kGlobMajor        = 0x0B3A1F;   // GLOB DBM_VersionMajor
        constexpr RE::FormID kGlobMinor        = 0x06B884;   // GLOB DBM_VersionMinor
        constexpr RE::FormID kGlobPatch        = 0x06B885;   // GLOB DBM_VersionPatch
        constexpr RE::FormID kGlobShipFirst    = 0x1772A8;   // GLOB DBMMuseumShipmentsFirst
        constexpr RE::FormID kFlstExclude      = 0x2940ED;   // FLST DBM_ExcludeList
        constexpr RE::FormID kFlstProtected    = 0x161D0B;   // FLST DBM_ProtectedItems
        constexpr RE::FormID kCellHolding      = 0x1252E1;   // CELL DBMQA (placeable crates wait here)
        constexpr RE::FormID kRefPlaceable1    = 0x1772AB;   // REFR ShipmentCrate001 (persistent, in DBMQA)
        constexpr RE::FormID kIdleGive         = 0x0B5E20;   // Skyrim.esm IDLE IdleGive (no conditions)

        constexpr const char* kApiScript = "DBM_MuseumAPI";

        // ── tunables (design §5) ──────────────────────────────────────────────
        // NEAR (museum priority, marth 2026-09-28): a crate counts when it is inside the
        // follower's PERMITTED LEASH from the player (TeleportCompat::View(f).leash, the
        // leash every loot trip uses) -- no fixed range of its own any more.
        constexpr float kCrateRadius    = 100.0f;    // ch.19 arrival radius (APMF clamps to [50,512])
        constexpr float kArriveSlack    = 75.0f;     // distance backstop when no leg state is read
        constexpr auto  kTripMax        = std::chrono::seconds(90);
        constexpr auto  kTripStall      = std::chrono::seconds(10);   // owner not ticked this long -> orphaned
        // NO ANIMATION, NO TRANSFER (Harbinger idle-confirm review, SEV-3.4): Harbinger's
        // ch.12 ENDS an idle claim it has not CONFIRMED playing about 3.0-3.3 s after the
        // play (its kConfirmWaitMs 3000, measured in world-tick time; the ~0.3 s spread is
        // its own drain). The transfer therefore waits until the give idle has stayed live
        // for kGiveConfirmSec on the UNPAUSED service clock (Scheduler::ServiceClock, so a
        // menu never ages it, as world ticks do not) from MFO's first live read, which is at
        // or after the play: 3.0 s + 0.3 s (the measured spread) + 0.2 s for the end to be
        // published by Harbinger's per-frame drain and read by DepositIdleStatus, which the
        // Giving step reads BEFORE it transfers. Was 1 s of wall time (kGiveAfter), which
        // shipped relics under an IdleGive that never played. Harbinger's ABI has no
        // "confirmed" query; outlasting the window is the proof.
        constexpr double kGiveConfirmSec = 3.5;
        constexpr auto  kIdleSettle     = std::chrono::milliseconds(2500);   // after the transfer: the rest of IdleGive's clip
        constexpr auto  kNeedsTtl       = std::chrono::seconds(2);
        constexpr auto  kRebuildMinGap  = std::chrono::seconds(5);
        constexpr auto  kCooldownOk     = std::chrono::seconds(10);
        constexpr auto  kCooldownFail   = std::chrono::seconds(30);
        constexpr auto  kCrateCooldown  = std::chrono::seconds(120);   // per (follower, crate), never crate-wide
        // A BLOCKED walk that ends OUTSIDE interaction reach is retried by that
        // follower after kBlockedRetry (a door swung shut, an NPC or a knocked-over
        // cart in the lane clears in seconds); only kBlockedStreakMax BLOCKED ends in a
        // row at the same crate earn the full kCrateCooldown. Each end is logged WARN.
        constexpr auto  kBlockedRetry     = std::chrono::seconds(10);
        constexpr int   kBlockedStreakMax = 3;
        constexpr auto  kYieldCooldown    = std::chrono::seconds(3);   // a trip ended to let a heal fire

        // ── detection (main thread writes once at kDataLoaded; atomics for readers) ──
        std::atomic<bool>       g_detected{ false };
        std::atomic<RE::FormID> g_questId{ 0 };
        std::atomic<RE::FormID> g_outgoingBase{ 0 };
        std::atomic<RE::FormID> g_dropoffRef{ 0 };
        std::atomic<RE::FormID> g_autosortRef{ 0 };
        std::atomic<RE::FormID> g_shipFirstGlob{ 0 };
        std::atomic<RE::FormID> g_excludeFlst{ 0 };
        std::atomic<RE::FormID> g_protectedFlst{ 0 };
        std::atomic<RE::FormID> g_holdingCell{ 0 };
        std::atomic<RE::FormID> g_placeable1{ 0 };
        std::atomic<RE::FormID> g_idleGive{ 0 };
        std::atomic<bool>       g_lastEnabled{ false };   // the toggle-on edge (OnConfigRead)

        // ── the SNAPSHOT (built on main, read on the worker) ────────────────────
        // One Slot per display position: a single display ref, or a GROUP (a FLST of
        // alternative display refs; LOTD fills at most one of them). Open = every
        // display in it is still disabled (LOTD enables a display when it fills it).
        struct Slot {
            std::vector<RE::FormID> displays;
            std::vector<RE::FormID> accepted;   // inventory bases that fill it (any one)
        };
        struct Snapshot {
            std::uint32_t                  seq = 0;
            std::vector<Slot>              slots;
            std::unordered_set<RE::FormID> bases;   // union of every slot's accepted bases
            // EVERY display ref the slot table names (a slot with no inventory item
            // included): the loose-loot bar (IsDisplayRef, 86e3f9pkg) -- a displayed
            // relic is an ENABLED item ref, lootable-shaped.
            std::unordered_set<RE::FormID> displays;
        };
        std::mutex                        g_snapMx;
        std::shared_ptr<const Snapshot>   g_snap;          // guarded by g_snapMx
        std::atomic<std::uint32_t>        g_snapSeq{ 0 };
        std::atomic<bool>                 g_rebuildQueued{ false };
        std::atomic<std::int64_t>         g_lastRebuildMs{ 0 };   // steady ms, for kRebuildMinGap

        std::shared_ptr<const Snapshot> CurrentSnapshot() {
            std::scoped_lock lock(g_snapMx);
            return g_snap;
        }

        using CountMap = std::unordered_map<RE::FormID, std::int32_t>;

        // ── the IN-TRANSIT LEDGER (MAIN THREAD ONLY) ─────────────────────────────
        // Items MFO put into an outgoing crate. LOTD moves them to DropoffCrate some
        // game hours later (5 h after the crate's LAST OnItemAdded re-armed it); until
        // then an UNLOADED town crate cannot be read, so this is the only record they
        // exist. A FLOOR, never an expiry (principle 9): an entry stays until its item
        // is SEEN arriving in DropoffCrate (its count there reaches the baseline read
        // at the deposit + the count) or its base is no longer needed by any open slot
        // (displayed). Session-only: a reload before arrival can cost one duplicate
        // relic, which LOTD leaves harmlessly in DropoffCrate (a known limit, design
        // §2 B -- not masked).
        struct InTransit { RE::FormID base; std::int32_t count; std::int32_t dropBaseline; RE::FormID crate; };
        std::mutex             g_ledgerMx;
        std::vector<InTransit> g_ledger;   // guarded by g_ledgerMx; written and pruned on MAIN only

        // ── the MAIN-THREAD SUPPLY (built on main, published like g_snap) ───────
        // THREADING (review round 1, carve-out): the PLAYER's inventory and the
        // museum's own containers are read on the MAIN thread only, never on the
        // worker. RefreshMainSupplyOnMain takes those counts (and prunes the ledger)
        // and publishes an immutable copy the worker's needs read.
        struct MainSupply {
            std::uint32_t     snapSeq = 0;
            Clock::time_point at{};
            CountMap          fixed;   // player + DropoffCrate + display drop-off + in transit
            std::int32_t      sPlayer = 0, sDrop = 0, sAuto = 0, sTransit = 0;
        };
        std::mutex                          g_mainMx;
        std::shared_ptr<const MainSupply>   g_main;   // guarded by g_mainMx
        std::atomic<bool>                   g_mainQueued{ false };

        std::shared_ptr<const MainSupply> CurrentMainSupply() {
            std::scoped_lock lock(g_mainMx);
            return g_main;
        }

        // ── the NEEDS cache (worker only) ────────────────────────────────────────
        struct Needs {
            std::uint32_t                   snapSeq = 0;
            Clock::time_point               at{};
            std::shared_ptr<const Snapshot> snap;
            std::vector<std::uint32_t>      open;          // open slot indices
            std::shared_ptr<const MainSupply> mainSupply;  // the main-thread counts this was built from
            std::unordered_map<RE::FormID, std::int32_t> fixedSupply;   // player, dropoff, autosort, in transit
            // Follower supply, sorted by follower FormID: a follower's own relic
            // "covers" a slot only after every LOWER-id follower's copy, so two
            // followers holding one needed relic never both keep (or both ship) it.
            std::vector<std::pair<RE::FormID, std::unordered_map<RE::FormID, std::int32_t>>> followerSupply;
            std::unordered_map<RE::FormID, std::int32_t> uncoveredAll;   // after ALL supply (the loot want)
            std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, std::int32_t>> uncoveredExcl;
        };
        Needs g_needs;   // worker only
        // Coverage roster (field 0928c A2): per follower, his relic counts as last read
        // while he was in the active snapshot. Worker only, cleared on revert.
        std::unordered_map<RE::FormID, CountMap> g_retainedSupply;
        std::size_t                              g_retainedLogged = 0;
        // MFO-B126: per follower, the off-role relics ShedOffRoleWeapon kept for the
        // deposit on its last pass and the in-role weapons he carried then
        // (NoteKeptForDeposit). Worker only, like g_needs.
        struct KeptRecord {
            std::unordered_set<RE::FormID> kept;
            std::vector<RE::FormID>        inRole;
        };
        std::unordered_map<RE::FormID, KeptRecord> g_keptForDeposit;
        // Round 3 (positive proof): per follower, the kept relics MFO has SEEN UNWORN in
        // his pack. Sticky across the shed's early returns; a full walk prunes it to the
        // relics still kept. Worker only.
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_keptSeenUnworn;
        std::uint32_t g_needsLoggedSeq = 0;

        // ── the TRIPS (one per follower, several at once; worker road + dismissal edge) ──
        // marth 2026-09-28: "Several followers may deposit at the same time." The
        // Harbinger claims were always keyed by follower (apmf/Deposit.cpp s_claims);
        // the single global trip was MFO's own limit and is gone.
        enum class Phase { Walking, Giving, Settling };
        struct Trip {
            RE::FormID        follower = 0;
            RE::FormID        crate    = 0;
            Phase             phase    = Phase::Walking;
            Clock::time_point start{};
            Clock::time_point lastTick{};   // the owner's last DepositTick (SweepTrip's stall test)
            Clock::time_point idleAt{};     // the give idle was claimed
            double            liveClk = -1.0;   // the give idle was first SEEN live, unpaused service clock (-1 = not yet)
            Clock::time_point settleFrom{}; // the transfer was posted
            // THIS trip's posted transfer outcome (was one global atomic when one trip
            // ran at a time): 0 = not run yet, 1 = moved something, 2 = skipped / moved
            // nothing (-> a FAIL end with the crate cooldown, never a silent DONE loop).
            // Shared with the main-thread TransferOnMain it is posted to.
            std::shared_ptr<std::atomic<int>> transfer = std::make_shared<std::atomic<int>>(0);
        };
        // (follower, crate): failure cooldowns and the BLOCKED streak are per pair, so
        // one follower's failed walk never locks the crate for everyone else.
        using PairKey = std::uint64_t;
        PairKey Pair(RE::FormID a_follower, RE::FormID a_crate) {
            return (static_cast<std::uint64_t>(a_follower) << 32) | a_crate;
        }
        std::mutex g_tripMx;
        std::unordered_map<RE::FormID, Trip> g_trips;   // guarded by g_tripMx; key = follower
        std::unordered_map<RE::FormID, Clock::time_point> g_followerCooldown;   // guarded by g_tripMx
        std::unordered_map<PairKey, Clock::time_point>    g_crateCooldown;      // guarded by g_tripMx; per (follower, crate)
        std::unordered_map<PairKey, int>                  g_blockedStreak;      // guarded by g_tripMx; BLOCKED ends in a row
        // Throttle for the priority gate's "why not now" lines (worker only): the last
        // reason logged per follower and when, so a held-off deposit says so once, not
        // every tick.
        std::unordered_map<RE::FormID, std::pair<std::string, Clock::time_point>> g_gateLog;
        // Interaction reach for a BLOCKED leg that stopped short of the crate:
        // the engine's activation pick length, INI 'fActivatePickLength:Interface'
        // (engine default 180; the setting name is in the 1.6.1170 executable's
        // setting table). Read on the MAIN thread at Detect / OnPostLoad (the
        // Lockpick.cpp:215 INISettingCollection road); 0 = unreadable, and then a
        // short BLOCKED stop is never counted as arrived (logged once, loud).
        std::atomic<float> g_activateReach{ 0.0f };
        // A SYNCHRONOUS ch.12 v2 refusal ([Idle] bIdleV2=0, runtime, self-check) does not
        // change within a process: latch the deposit OFF instead of a walk-refuse loop.
        std::atomic<bool> g_depositLatched{ false };
        // The shipping intro is in flight (steady ms until which no second activation is
        // posted), so it can never show twice.
        std::atomic<std::int64_t> g_introPendingUntilMs{ 0 };
        // (The posted transfer's outcome is per trip now: Trip::transfer.)

        std::int64_t SteadyMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now().time_since_epoch()).count();
        }

        template <class T>
        T* ById(const std::atomic<RE::FormID>& a_id) {
            const RE::FormID id = a_id.load(std::memory_order_relaxed);
            return id ? RE::TESForm::LookupByID<T>(id) : nullptr;
        }

        const char* NameOf(const RE::TESForm* a_form) {
            const char* n = a_form ? a_form->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        // The item types a museum slot can be filled from a follower's pack with
        // (design §4: the ACTI / MSTT "displays" are quest/exploration, not items).
        bool IsMuseumItemType(const RE::TESForm* a_form) {
            if (!a_form) return false;
            switch (a_form->GetFormType()) {
            case RE::FormType::Weapon:
            case RE::FormType::Armor:
            case RE::FormType::Book:
            case RE::FormType::Misc:
            case RE::FormType::AlchemyItem:
            case RE::FormType::KeyMaster:
            case RE::FormType::Ammo:
            case RE::FormType::SoulGem:
            case RE::FormType::Ingredient:
                return true;
            default:
                return false;
            }
        }

        // A FormList's entries IN INDEX ORDER, nulls KEPT (positions pair a display
        // list with its item list; CommonLib's ForEachForm skips nulls and would shift
        // the pairing). Editor forms first, then the script-added ones -- the same
        // order for BOTH lists of a pair, so a lockstep MergeLists append stays aligned.
        std::vector<RE::TESForm*> Entries(const RE::BGSListForm* a_list, std::uint32_t* a_scriptAdded = nullptr) {
            std::vector<RE::TESForm*> out;
            if (!a_list) return out;
            for (auto* f : a_list->forms) out.push_back(f);
            if (a_list->scriptAddedTempForms) {
                for (const auto id : *a_list->scriptAddedTempForms) {
                    out.push_back(RE::TESForm::LookupByID(id));
                    if (a_scriptAdded) ++*a_scriptAdded;
                }
            }
            return out;
        }

        void AddAccepted(RE::TESForm* a_item, std::vector<RE::FormID>& a_out,
                         const RE::BGSListForm* a_exclude, const RE::BGSListForm* a_protected) {
            if (!a_item) return;
            auto add = [&](RE::TESForm* f) {
                if (!f || !IsMuseumItemType(f)) return;
                if (a_exclude && a_exclude->HasForm(f)) return;       // LOTD's own "never display"
                if (a_protected && a_protected->HasForm(f)) return;   // LOTD's quest-item protection
                if (std::find(a_out.begin(), a_out.end(), f->GetFormID()) == a_out.end())
                    a_out.push_back(f->GetFormID());
            };
            if (auto* fl = a_item->As<RE::BGSListForm>()) {   // "any one of these"
                for (auto* f : Entries(fl)) add(f);
            } else {
                add(a_item);
            }
        }

        // ── L1: THE VM READ (MAIN THREAD) ────────────────────────────────────────
        // One read of the DBM_MuseumAPI script object's four array sets. Principle 5:
        // this read must be OBSERVED on the deck (the [lotd] snapshot line) before any
        // behaviour is trusted to it.
        void RebuildSnapshot(const char* a_reason) {
            g_rebuildQueued.store(false);
            if (!g_detected.load()) return;
            const auto t0 = Clock::now();
            g_lastRebuildMs.store(SteadyMs());

            auto* quest = ById<RE::TESQuest>(g_questId);
            auto* vm    = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            if (!quest || !vm || !policy) {
                spdlog::warn("[lotd] snapshot ({}): no VM / policy / museum quest -- not built", a_reason);
                return;
            }
            const RE::VMHandle h = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(quest->GetFormType()), quest);
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (h == policy->EmptyHandle() || !vm->FindBoundObject(h, kApiScript, obj) || !obj) {
                // A new game can arrive here before the quest's script is bound; LOTD's
                // own MCMRefresh event (all patches registered) triggers the next read.
                // Logged at most once a minute (the worker re-asks every kRebuildMinGap).
                static std::int64_t s_nextLog = 0;
                if (SteadyMs() >= s_nextLog) {
                    s_nextLog = SteadyMs() + 60000;
                    spdlog::info("[lotd] snapshot ({}): {} is not bound yet -- waiting for LOTD's MCMRefresh / "
                                 "DBM events", a_reason, kApiScript);
                }
                return;
            }

            auto* exclude   = ById<RE::BGSListForm>(g_excludeFlst);
            auto* protectd  = ById<RE::BGSListForm>(g_protectedFlst);
            auto snap = std::make_shared<Snapshot>();
            std::uint32_t sections = 0, groupSlots = 0, displayRefs = 0, skipped = 0, scriptAdded = 0;
            std::uint32_t perSet[4]{};

            auto listArray = [&](const char* a_prop) -> RE::BSTSmartPointer<RE::BSScript::Array> {
                const auto* v = obj->GetProperty(a_prop);
                if (!v || !(v->IsArray() || v->IsNoneArray())) return nullptr;
                return v->GetArray();
            };
            auto listAt = [](const RE::BSTSmartPointer<RE::BSScript::Array>& a_arr,
                             std::uint32_t a_i) -> RE::BGSListForm* {
                if (!a_arr || a_i >= a_arr->size()) return nullptr;
                const auto& v = (*a_arr)[a_i];
                if (!(v.IsObject() || v.IsNoneObject())) return nullptr;
                return v.Unpack<RE::BGSListForm*>();
            };

            static constexpr const char* kDisplayProps[4] = { "SectionDisplayLists", "SectionDisplayLists2",
                                                             "SectionDisplayLists3", "SectionDisplayLists4" };
            static constexpr const char* kItemProps[4]    = { "SectionDisplayItems", "SectionDisplayItems2",
                                                             "SectionDisplayItems3", "SectionDisplayItems4" };
            for (int set = 0; set < 4; ++set) {
                const auto dispArr = listArray(kDisplayProps[set]);
                const auto itemArr = listArray(kItemProps[set]);
                if (!dispArr || !itemArr) continue;
                const std::uint32_t n = static_cast<std::uint32_t>(dispArr->size());
                for (std::uint32_t s = 0; s < n; ++s) {
                    auto* dispList = listAt(dispArr, s);
                    auto* itemList = listAt(itemArr, s);
                    if (!dispList || !itemList) continue;
                    ++sections;
                    ++perSet[set];
                    const auto disps = Entries(dispList, &scriptAdded);
                    const auto items = Entries(itemList, &scriptAdded);
                    for (std::size_t i = 0; i < disps.size(); ++i) {
                        RE::TESForm* d    = disps[i];
                        RE::TESForm* item = i < items.size() ? items[i] : nullptr;
                        Slot slot;
                        if (auto* group = d ? d->As<RE::BGSListForm>() : nullptr) {
                            // A group: sub-display k pairs with entry k of the item FLST
                            // (or the one item for all of them) -- DBMDisplayAutoSorter.
                            const auto sub = Entries(group);
                            auto* itemGroup = item ? item->As<RE::BGSListForm>() : nullptr;
                            const auto subItems = itemGroup ? Entries(itemGroup) : std::vector<RE::TESForm*>{};
                            for (std::size_t k = 0; k < sub.size(); ++k) {
                                auto* ref = sub[k] ? sub[k]->As<RE::TESObjectREFR>() : nullptr;
                                if (!ref) continue;
                                snap->displays.insert(ref->GetFormID());
                                slot.displays.push_back(ref->GetFormID());
                                AddAccepted(itemGroup ? (k < subItems.size() ? subItems[k] : nullptr) : item,
                                            slot.accepted, exclude, protectd);
                            }
                            ++groupSlots;
                        } else if (auto* ref = d ? d->As<RE::TESObjectREFR>() : nullptr) {
                            snap->displays.insert(ref->GetFormID());
                            slot.displays.push_back(ref->GetFormID());
                            AddAccepted(item, slot.accepted, exclude, protectd);
                        }
                        if (slot.displays.empty() || slot.accepted.empty()) { ++skipped; continue; }
                        displayRefs += static_cast<std::uint32_t>(slot.displays.size());
                        for (auto b : slot.accepted) snap->bases.insert(b);
                        snap->slots.push_back(std::move(slot));
                    }
                }
            }

            // Open slots + samples, for the passive log (the worker recomputes these
            // itself, this is only what L1's field check reads).
            std::uint32_t open = 0;
            std::string sample;
            int samples = 0;
            for (const auto& sl : snap->slots) {
                bool allOff = true;
                for (auto id : sl.displays) {
                    auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                    if (!r || !r->IsDisabled()) { allOff = false; break; }
                }
                if (!allOff) continue;
                ++open;
                if (samples < 10) {
                    auto* b = RE::TESForm::LookupByID(sl.accepted.front());
                    sample += std::format("{}'{}' ({:08X})", samples ? ", " : "", NameOf(b), sl.accepted.front());
                    ++samples;
                }
            }

            snap->seq = g_snapSeq.fetch_add(1) + 1;
            const auto slotsN = snap->slots.size();
            const auto basesN = snap->bases.size();
            {
                std::scoped_lock lock(g_snapMx);
                g_snap = std::move(snap);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            spdlog::info("[lotd] snapshot #{} ({}): {} sections (sets {}/{}/{}/{}), {} slots ({} groups), {} display "
                         "refs, {} distinct item bases, {} skipped (no display or no inventory item), {} "
                         "script-added list entries; {} OPEN (display still disabled), {} filled; built in {} ms",
                         g_snapSeq.load(), a_reason, sections, perSet[0], perSet[1], perSet[2], perSet[3], slotsN,
                         groupSlots, displayRefs, basesN, skipped, scriptAdded, open, slotsN - open, ms);
            if (samples) spdlog::info("[lotd] snapshot #{} sample open slots: {}", g_snapSeq.load(), sample);
        }

        void RequestRebuild(const char* a_reason, bool a_force = false) {
            if (!g_detected.load()) return;
            if (!a_force && SteadyMs() - g_lastRebuildMs.load() <
                                std::chrono::duration_cast<std::chrono::milliseconds>(kRebuildMinGap).count())
                return;
            if (g_rebuildQueued.exchange(true)) return;   // one queued rebuild at a time
            if (!MainThread::IsInstalled()) {             // VR: no pump -> no VM read off the main thread
                g_rebuildQueued.store(false);
                return;
            }
            std::string why = a_reason;
            MainThread::Post([why]() { RebuildSnapshot(why.c_str()); });
        }

        // ── LOTD's SKSE ModEvents (the snapshot's rebuild triggers) ──────────────
        class ModEventSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
        public:
            static ModEventSink* GetSingleton() { static ModEventSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_ev,
                                                  RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
                if (!a_ev || !g_detected.load()) return RE::BSEventNotifyControl::kContinue;
                const std::string_view name = a_ev->eventName.c_str() ? a_ev->eventName.c_str() : "";
                // MCMRefresh: LOTD's "every patch has registered" (a new game / an update).
                // DBM DisplayListUpdate / DBM DisplaySortComplete: displays were added.
                // The sink only QUEUES: the read runs on the main thread, rate-limited.
                if (name == "MCMRefresh" || name == "DBM DisplayListUpdate" || name == "DBM DisplaySortComplete")
                    RequestRebuild(a_ev->eventName.c_str(), name == "MCMRefresh");
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ── the needs ─────────────────────────────────────────────────────────────
        void AddCounts(RE::TESObjectREFR* a_ref, const Snapshot& a_snap, CountMap& a_out, bool a_noInit) {
            if (!a_ref) return;
            const auto counts = a_ref->GetInventoryCounts(
                [&a_snap](RE::TESBoundObject& o) { return a_snap.bases.count(o.GetFormID()) != 0; }, a_noInit);
            for (const auto& [obj, n] : counts)
                if (obj && n > 0) a_out[obj->GetFormID()] += n;
        }

        std::int32_t Sum(const CountMap& a_m) {
            std::int32_t t = 0;
            for (auto& [k, v] : a_m) t += v;
            return t;
        }

        // Open = every display of the slot is still disabled.
        bool SlotOpen(const Slot& a_slot) {
            for (auto id : a_slot.displays) {
                auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                if (!r || !r->IsDisabled()) return false;
            }
            return true;
        }

        // MAIN THREAD. The player's and the museum containers' counts + the ledger,
        // published for the worker. Also where the ledger floor is released.
        void RefreshMainSupplyOnMain() {
            g_mainQueued.store(false);
            auto snap = CurrentSnapshot();
            if (!snap) return;
            auto m = std::make_shared<MainSupply>();
            m->snapSeq = snap->seq;
            m->at      = Clock::now();
            CountMap player, drop, autos;
            AddCounts(RE::PlayerCharacter::GetSingleton(), *snap, player, false);
            // Persistent museum-side containers, read WITHOUT initialising an unloaded
            // one's inventory (a_noInit): delivered, waiting to be sorted.
            AddCounts(ById<RE::TESObjectREFR>(g_dropoffRef), *snap, drop, true);
            AddCounts(ById<RE::TESObjectREFR>(g_autosortRef), *snap, autos, true);
            m->sPlayer = Sum(player);
            m->sDrop   = Sum(drop);
            m->sAuto   = Sum(autos);
            for (auto* src : { &player, &drop, &autos })
                for (auto& [k, v] : *src) m->fixed[k] += v;
            {
                std::scoped_lock lock(g_ledgerMx);
                if (!g_ledger.empty()) {
                    std::unordered_set<RE::FormID> openBases;
                    for (const auto& sl : snap->slots)
                        if (SlotOpen(sl))
                            for (auto b : sl.accepted) openBases.insert(b);
                    std::erase_if(g_ledger, [&](const InTransit& t) {
                        auto it = drop.find(t.base);
                        const std::int32_t there = it != drop.end() ? it->second : 0;
                        if (there >= t.dropBaseline + t.count) {
                            spdlog::info("[lotd] in transit: {:08X} x{} ARRIVED in DropoffCrate -- ledger entry released",
                                         t.base, t.count);
                            return true;
                        }
                        if (!openBases.count(t.base)) {
                            spdlog::info("[lotd] in transit: {:08X} x{} no longer needed by any open display -- ledger "
                                         "entry released", t.base, t.count);
                            return true;
                        }
                        return false;
                    });
                }
                for (const auto& t : g_ledger) { m->fixed[t.base] += t.count; m->sTransit += t.count; }
            }
            std::scoped_lock lock(g_mainMx);
            g_main = std::move(m);
        }

        void RequestMainSupply() {
            if (!MainThread::IsInstalled() || g_mainQueued.exchange(true)) return;
            MainThread::Post([]() { RefreshMainSupplyOnMain(); });
        }

        // Greedy cover: each open slot consumes one unit of the first accepted base
        // with supply left; the uncovered slots' accepted bases are what is needed.
        CountMap Uncovered(const Needs& a_n, CountMap a_supply) {
            CountMap out;
            for (auto idx : a_n.open) {
                const auto& sl = a_n.snap->slots[idx];
                bool covered = false;
                for (auto b : sl.accepted) {
                    auto it = a_supply.find(b);
                    if (it != a_supply.end() && it->second > 0) { --it->second; covered = true; break; }
                }
                if (!covered)
                    for (auto b : sl.accepted) ++out[b];
            }
            return out;
        }

        // WORKER. Reads ONLY the follower inventories itself; the player's and the museum
        // containers' counts come from the main-thread copy (RefreshMainSupplyOnMain).
        // Without a main-thread copy for this snapshot there is no needs answer yet.
        Needs& FreshNeeds(Clock::time_point a_now) {
            auto snap = CurrentSnapshot();
            if (!snap) {
                g_needs = Needs{};
                RequestRebuild("needs asked, no snapshot");
                return g_needs;
            }
            auto ms = CurrentMainSupply();
            if (!ms || ms->snapSeq != snap->seq || a_now - ms->at >= kNeedsTtl) RequestMainSupply();
            if (!ms || ms->snapSeq != snap->seq) {
                g_needs = Needs{};   // not answerable yet: nothing is looted, shipped or held
                return g_needs;
            }
            if (g_needs.snap == snap && g_needs.mainSupply == ms && a_now - g_needs.at < kNeedsTtl) return g_needs;

            Needs n;
            n.snap    = snap;
            n.mainSupply = ms;
            n.snapSeq = snap->seq;
            n.at      = a_now;
            for (std::uint32_t i = 0; i < snap->slots.size(); ++i)
                if (SlotOpen(snap->slots[i])) n.open.push_back(i);
            n.fixedSupply = ms->fixed;
            std::int32_t sFollowers = 0;
            // COVERAGE FOLLOWS THE PARTY, NOT THE PROCESS LIST (field 0928c, A2). The
            // active snapshot is Followers::Refresh's rebuild from the HIGH process list:
            // a follower left behind in another cell drops out of it after three missed
            // sweeps and comes back when he catches up (Serana, 4 times in one session).
            // Rebuilding coverage from that snapshot made her relics vanish from supply
            // for those seconds, so a HIGHER-id follower (Jesper, via UncoveredExcluding)
            // became the coverer of her Novice Robes' slot and had his own robes stripped,
            // then restored -- and Cicero's Dawnguard Helmet the same way through Adelinda.
            // So: an active follower's relic counts are read now and REMEMBERED
            // (g_retainedSupply); a follower who is out of the snapshot but still a
            // follower (Followers::IsEligibleFollower: teammate, not dead / disabled / a
            // dismissed custom follower) keeps counting with his last-read counts. His
            // inventory is never read while he is outside the high process list (the
            // unloaded-inventory read is exactly what the snapshot avoided), and it cannot
            // change there without him. A follower who is no longer eligible is dropped.
            // Not a timer: membership is the engine's own teammate state.
            std::unordered_set<RE::FormID> activeNow;
            if (auto ids = Followers::ActiveSnapshot()) {
                for (auto fid : *ids) {
                    CountMap m;
                    AddCounts(RE::TESForm::LookupByID<RE::Actor>(fid), *snap, m, false);
                    g_retainedSupply[fid] = std::move(m);
                    activeNow.insert(fid);
                }
            }
            std::vector<RE::FormID> sorted;
            for (auto it = g_retainedSupply.begin(); it != g_retainedSupply.end();) {
                if (!activeNow.count(it->first)) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
                    if (!a || !Followers::IsEligibleFollower(a)) { it = g_retainedSupply.erase(it); continue; }
                }
                sorted.push_back(it->first);
                ++it;
            }
            std::sort(sorted.begin(), sorted.end());
            for (auto fid : sorted) {
                const CountMap& m = g_retainedSupply[fid];
                sFollowers += Sum(m);
                n.followerSupply.emplace_back(fid, m);
            }
            if (sorted.size() > activeNow.size() && g_retainedLogged != sorted.size() - activeNow.size()) {
                g_retainedLogged = sorted.size() - activeNow.size();
                spdlog::info("[lotd] needs: {} follower(s) out of the loaded roster still count toward museum "
                             "coverage (last-read relic counts, still teammates)", g_retainedLogged);
            } else if (sorted.size() == activeNow.size()) {
                g_retainedLogged = 0;
            }
            CountMap all = n.fixedSupply;
            for (auto& [fid, m] : n.followerSupply)
                for (auto& [k, v] : m) all[k] += v;
            n.uncoveredAll = Uncovered(n, std::move(all));
            g_needs = std::move(n);

            if (g_needsLoggedSeq != g_needs.snapSeq) {
                g_needsLoggedSeq = g_needs.snapSeq;
                std::uint32_t needBases = 0;
                for (auto& [k, v] : g_needs.uncoveredAll) if (v > 0) ++needBases;
                spdlog::info("[lotd] needs (snapshot #{}): {} open slot(s), {} item base(s) still wanted after "
                             "supply -- player {}, followers {}, DropoffCrate {}, display drop-off {}, in transit {}",
                             g_needs.snapSeq, g_needs.open.size(), needBases, ms->sPlayer, sFollowers, ms->sDrop,
                             ms->sAuto, ms->sTransit);
            }
            return g_needs;
        }

        // What is still uncovered when THIS follower's own copies are left out (and
        // only lower-id followers count before him): the relics HE is covering.
        const CountMap& UncoveredExcluding(Needs& a_n, RE::FormID a_follower) {
            auto it = a_n.uncoveredExcl.find(a_follower);
            if (it != a_n.uncoveredExcl.end()) return it->second;
            CountMap supply = a_n.fixedSupply;
            for (auto& [fid, m] : a_n.followerSupply) {
                if (fid >= a_follower) break;   // sorted: only the lower ids cover before him
                for (auto& [k, v] : m) supply[k] += v;
            }
            return a_n.uncoveredExcl.emplace(a_follower, Uncovered(a_n, std::move(supply))).first->second;
        }

        // worn: the planned instance is WORN and ships anyway (an off-role relic the shed
        // kept, MFO-B126); the main-thread transfer honours it instead of skipping.
        struct ShipItem { RE::FormID base; std::int32_t count; bool worn = false; };

        // How many copies of one inventory entry are WORN: one per extra list carrying
        // ExtraWorn (right / body) or ExtraWornLeft (left hand) -- the same reads
        // TransferOnMain unequips by (review R2-4: a stack is shipped copy by copy).
        std::int32_t WornCopies(RE::InventoryEntryData* a_e) {
            std::int32_t n = 0;
            if (!a_e || !a_e->extraLists) return 0;
            for (auto* xl : *a_e->extraLists)
                if (xl && (xl->HasType(RE::ExtraDataType::kWorn) || xl->HasType(RE::ExtraDataType::kWornLeft))) ++n;
            return n;
        }

        // MFO-B126: a WORN relic ships only when the shed kept it (off-role) on its last
        // pass, there is POSITIVE PROOF his own AI put it on (PlayerGiven::IsAiEquipped:
        // an equip with no trade / gift menu open of a relic MFO had seen unworn in his
        // pack as kept -- review round 3; that mark is not saved, so after a load this
        // fails closed and nothing worn ships), it is NOT player-given (a relic the
        // player gave is SAVED, 'PGIV', 86e3faccn: never shipped after a reload), no MFO hold
        // names it (a hold is a gambit's choice: the relic was his only weapon of that
        // category), and he STILL carries one of the in-role weapons that pass saw (the
        // never-disarm guard, re-checked at ship time against a_inv). Worker.
        bool KeptOffRoleWorn(RE::FormID a_follower, RE::FormID a_base, const RE::TESObjectREFR::InventoryItemMap& a_inv) {
            if (!PlayerGiven::Installed() || PlayerGiven::IsPlayerGiven(a_follower, a_base)) return false;
            if (!PlayerGiven::IsAiEquipped(a_follower, a_base)) return false;
            const auto it = g_keptForDeposit.find(a_follower);
            if (it == g_keptForDeposit.end() || !it->second.kept.count(a_base)) return false;
            const auto [holdR, holdL] = Actuation::ForcedHoldFor(a_follower);
            if (holdR == a_base || holdL == a_base) return false;
            for (const RE::FormID r : it->second.inRole) {
                if (r == a_base) continue;
                for (const auto& [obj, data] : a_inv)
                    if (obj && obj->GetFormID() == r && data.first > 0) return true;
            }
            return false;
        }

        // POSITIVE PROOF for the worn-relic ship (review F4 on f2c8e2b): the needed relics
        // MFO has SEEN UNWORN in this follower's pack this session, per follower (worker
        // only; cleared on revert; pruned to what he still carries). A relic seen unworn
        // and worn later, that the PLAYER did not give him or put on him in the trade /
        // gift menu (PlayerGiven records every such menu equip of a relic, SAVED 'PGIV'),
        // was put on OUTSIDE the menu -- his AI, the engine's OutfitApply, SPID -- which
        // is exactly what ships. A relic never seen unworn (worn since a load, or the
        // player's dressing from before PlayerGiven existed) does NOT ship: a missing
        // PlayerGiven record is not proof. Refreshed at most every kUnwornScan per
        // follower (the needs cache's own 2 s TTL).
        // PER COPY (review R2-4 on 56c032b): only an UNWORN copy is proof. The record is
        // base -> the most copies seen while NONE of them was worn; a worn copy has proof
        // only when every copy he carries now was seen that way (count <= the record),
        // so a second copy that arrived later, or a stack seen only with one copy on,
        // proves nothing about the worn one.
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, std::int32_t>> g_relicSeenUnworn;
        std::unordered_map<RE::FormID, Clock::time_point>              g_relicScanAt;
        constexpr auto kUnwornScan = std::chrono::seconds(2);

        void NoteUnwornRelics(RE::Actor* a_follower, Needs& a_n, Clock::time_point a_now) {
            if (!a_follower || !a_n.snap) return;
            const RE::FormID fid = a_follower->GetFormID();
            auto& at = g_relicScanAt[fid];
            if (at.time_since_epoch().count() != 0 && a_now - at < kUnwornScan) return;
            at = a_now;
            auto& seen = g_relicSeenUnworn[fid];
            std::unordered_set<RE::FormID> carried;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0 || !a_n.snap->bases.count(obj->GetFormID())) continue;
                carried.insert(obj->GetFormID());
                auto* e = data.second.get();
                if (!e || !e->IsWorn()) {   // every copy unworn right now
                    auto& n = seen[obj->GetFormID()];
                    n = std::max<std::int32_t>(n, static_cast<std::int32_t>(data.first));
                }
            }
            std::erase_if(seen, [&carried](const auto& kv) { return !carried.count(kv.first); });
        }

        // The "kind" a weapon fights as, for the never-disarm test (review F4): a bow, a
        // crossbow, or melee. A relic launcher needs another launcher of ITS kind (so the
        // off-kind ammo sale can never then sell his arrows); a relic melee weapon needs
        // another melee weapon. Staves and non-playable creature gear are no kind.
        int WeaponKindOf(const RE::TESObjectWEAP* a_w) {
            if (!a_w || a_w->IsStaff() || (a_w->GetFormFlags() & (1u << 2)) != 0) return -1;
            if (a_w->IsBow()) return 1;
            if (a_w->IsCrossbow()) return 2;
            return 0;
        }

        // MUSEUM-NEEDED ITEMS TAKE PRIORITY OVER EQUIPMENT (marth 2026-09-28: "needed
        // museum items take priority over equipment"; "an item the museum still needs
        // must not stay worn or held"). A WORN relic ships -- the transfer unequips it
        // at the crate (TransferOnMain) -- when there is POSITIVE PROOF it is not the
        // player's dressing, EXCEPT:
        //   * the PLAYER: given or put on in the trade / gift menu (PlayerGiven, SAVED
        //     'PGIV'); without PlayerGiven installed that cannot be told, so nothing
        //     worn ships (fail closed);
        //   * no positive proof: the off-role kept relic his AI equipped
        //     (KeptOffRoleWorn, PlayerGiven::IsAiEquipped), or a relic MFO saw UNWORN
        //     in his pack this session (g_relicSeenUnworn above);
        //   * an MFO hold names it: the combat pick chose it because he carries no
        //     other weapon of its category (cast/Equip.cpp IsMuseumRelic), so it is
        //     his weapon until an alternative exists;
        //   * a WEAPON with no other non-relic weapon OF ITS KIND carried (never disarm
        //     him; never leave him without the launcher his ammo is for).
        // IsPlayerPick / stock gear / quest are filtered by Shippable itself. Worker.
        bool WornRelicShippable(RE::Actor* a_follower, RE::TESBoundObject* a_base,
                                const RE::TESObjectREFR::InventoryItemMap& a_inv) {
            const RE::FormID fid = a_follower->GetFormID();
            const RE::FormID bid = a_base->GetFormID();
            if (KeptOffRoleWorn(fid, bid, a_inv)) return true;
            if (!PlayerGiven::Installed() || PlayerGiven::IsPlayerGiven(fid, bid)) return false;
            std::int32_t carried = 0;
            for (const auto& [obj, data] : a_inv)
                if (obj == a_base) carried = static_cast<std::int32_t>(data.first);
            const bool proof = PlayerGiven::IsAiEquipped(fid, bid) ||
                               [&] { const auto it = g_relicSeenUnworn.find(fid);
                                     if (it == g_relicSeenUnworn.end()) return false;
                                     const auto b = it->second.find(bid);
                                     return b != it->second.end() && carried > 0 && carried <= b->second; }();
            if (!proof) return false;
            const auto [holdR, holdL] = Actuation::ForcedHoldFor(fid);
            if (holdR == bid || holdL == bid) return false;
            auto* rw = a_base->As<RE::TESObjectWEAP>();
            if (!rw) return true;   // armor / shield / other: ships
            const int kind = WeaponKindOf(rw);
            if (kind < 0) return true;   // a relic staff is no weapon kind he fights with
            for (const auto& [obj, data] : a_inv) {
                if (!obj || data.first <= 0 || obj == a_base) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || WeaponKindOf(w) != kind) continue;
                if (HoldFromSale(fid, obj)) continue;   // another relic is no alternative
                return true;
            }
            return false;
        }

        // The relics this follower carries that the museum needs FROM HIM: a WORN one
        // only through WornRelicShippable above (never the player's choice, never his
        // only weapon, never an MFO hold), never an item the PLAYER gave or put on
        // him (IsPlayerPick, Economy's own check), never quest-flagged (LOTD's crate
        // ships quest items too -- trap (b)), never his own stock gear.
        std::vector<ShipItem> Shippable(RE::Actor* a_follower, Needs& a_n) {
            std::vector<ShipItem> out;
            if (!a_follower || !a_n.snap) return out;
            const auto fid = a_follower->GetFormID();
            const auto& excl = UncoveredExcluding(a_n, fid);
            if (excl.empty()) return out;
            const auto inv = a_follower->GetInventory();
            for (auto& [obj, data] : inv) {
                if (!obj || data.first <= 0) continue;
                auto it = excl.find(obj->GetFormID());
                if (it == excl.end() || it->second <= 0) continue;
                auto* e = data.second.get();
                if (e && e->IsQuestObject()) continue;
                if (Logistics::IsStockGear(fid, obj->GetFormID())) continue;
                if (Logistics::IsPlayerPick(fid, obj->GetFormID())) continue;
                if (PlayerGiven::IsPlayerGiven(fid, obj->GetFormID())) continue;   // marth: never ship what you gave him
                // COPY BY COPY (review R2-4): the UNWORN copies ship first, as not worn;
                // a worn copy only for the need left over, and only with proof for it.
                const std::int32_t need   = it->second;
                const std::int32_t wornN  = std::min<std::int32_t>(WornCopies(e), static_cast<std::int32_t>(data.first));
                const std::int32_t unworn = static_cast<std::int32_t>(data.first) - wornN;
                const std::int32_t first  = std::min(unworn, need);
                if (first > 0) out.push_back({ obj->GetFormID(), first, false });
                const std::int32_t rest = std::min(need - first, wornN);
                if (rest > 0 && WornRelicShippable(a_follower, obj, inv)) out.push_back({ obj->GetFormID(), rest, true });
            }
            return out;
        }

        bool DepositCanRun() {
            return !g_depositLatched.load() && APMFBridge::DepositSupported() && g_idleGive.load() &&
                   MainThread::IsInstalled();
        }

        bool HarbingerReady() {
            // MainThread: the ActivateRef and the transfer are main-thread work (no pump on VR).
            if (APMFBridge::DepositSupported() && g_idleGive.load() && MainThread::IsInstalled()) return true;
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true))
                spdlog::warn("[lotd] the Loot museum items gambit is INERT: it needs Harbinger (APMF) ABI v17 "
                             "for the deposit's give animation (this APMF: {}{}). No museum looting and no "
                             "deposits until Harbinger is updated. (Logged once.)",
                             APMFBridge::DepositApiVersion() ? std::format("ABI v{}", APMFBridge::DepositApiVersion())
                                                             : std::string("absent"),
                             g_idleGive.load() ? "" : "; IdleGive did not resolve");
            return false;
        }

        // The nearest (to the follower) enabled, loaded outgoing crate INSIDE HIS LEASH:
        // the crate is within TeleportCompat::View(f).leash of the PLAYER (museum
        // priority, marth 2026-09-28: "near" = the permitted leash, the same leash every
        // loot trip uses -- no fixed range). Never a placeable crate still parked in the
        // DBMQA holding cell, never one on cooldown FOR THIS FOLLOWER (per pair). WORKER:
        // walks only ATTACHED cells anchored on the follower and the player
        // (LootNearby's crash4-safe shape). a_leash receives the leash used (for the log).
        RE::TESObjectREFR* NearestCrate(RE::Actor* a_follower, Clock::time_point a_now, float* a_dist,
                                        float* a_leash = nullptr) {
            const RE::FormID base = g_outgoingBase.load();
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!base || !a_follower || !pc) return nullptr;
            const RE::FormID fid = a_follower->GetFormID();
            const RE::FormID holding = g_holdingCell.load();
            const auto fpos = a_follower->GetPosition();
            const auto ppos = pc->GetPosition();
            const float leash = Logistics::TeleportCompat::View(a_follower).leash;
            if (a_leash) *a_leash = leash;
            RE::TESObjectREFR* best = nullptr;
            float bestD = 0.0f;
            std::unordered_set<RE::TESObjectCELL*> seen;
            for (auto* anchor : { static_cast<RE::TESObjectREFR*>(a_follower), static_cast<RE::TESObjectREFR*>(pc) }) {
                auto* cell = anchor ? anchor->GetParentCell() : nullptr;
                if (!cell || !cell->IsAttached() || !seen.insert(cell).second) continue;
                cell->ForEachReferenceInRange(ppos, leash, [&](RE::TESObjectREFR& r) {
                    auto* b = r.GetBaseObject();
                    if (!b || b->GetFormID() != base) return RE::BSContainer::ForEachResult::kContinue;
                    if (r.IsDisabled() || !r.Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
                    if (auto* pcell = r.GetParentCell(); pcell && pcell->GetFormID() == holding)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (auto it = g_crateCooldown.find(Pair(fid, r.GetFormID()));
                        it != g_crateCooldown.end() && a_now < it->second)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (ppos.GetDistance(r.GetPosition()) > leash)
                        return RE::BSContainer::ForEachResult::kContinue;
                    const float d = fpos.GetDistance(r.GetPosition());
                    if (!best || d < bestD) { bestD = d; best = &r; }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (a_dist) *a_dist = bestD;
            return best;
        }

        // INTERACTION REACH for a BLOCKED leg (item 2, field 2026-09-28): the Riften
        // "Museum Shipments" crate 11099DB7 wears LOTD's OWN collision box --
        // LegacyoftheDragonborn.esm REFR 0x084197, base Skyrim.esm STAT 0x21
        // 'CollisionMarker' (base flags Obstacle | IsMarker), a ~70 u box enable-
        // parented to the crate and 29 u from its centre -- whose Obstacle flag cuts the
        // navmesh around it, so the walk ends BLOCKED at 186-187 u every time (two of
        // three attempts on the same triangle 0x000F20C5#695), 11 u past the old 175 u
        // test. A BLOCKED stop counts as ARRIVED only when the crate is within the
        // engine's ACTIVATION reach of him: 'fActivatePickLength:Interface' (the pick
        // ray's length, g_activateReach) measured to the crate's NEAR FACE, i.e. his
        // distance to the crate's origin minus the crate base's SMALLER horizontal OBND
        // half-extent (conservative: the near face is at least that close; the outgoing
        // crate's OBND is 83 x 138 u, so 41 u). The ref's scale is not applied (the
        // placed crates carry none). No line-of-sight test: MFO's Sightline cache is
        // actor-to-actor only (MeasureNow resolves both ends as actors), and the crate's
        // own collision box would stand between him and it anyway. Worker (form reads).
        bool WithinReach(RE::Actor* a_follower, RE::TESObjectREFR* a_crate, float* a_dist, float* a_reach) {
            const float pick = g_activateReach.load();
            const float d = a_follower->GetPosition().GetDistance(a_crate->GetPosition());
            float half = 0.0f;
            if (auto* b = a_crate->GetBaseObject()) {
                const auto& bd = b->boundData;
                const float hx = 0.5f * static_cast<float>(bd.boundMax.x - bd.boundMin.x);
                const float hy = 0.5f * static_cast<float>(bd.boundMax.y - bd.boundMin.y);
                half = std::max(0.0f, std::min(hx, hy));
            }
            if (a_dist) *a_dist = d;
            if (a_reach) *a_reach = pick + half;
            return pick > 0.0f && d <= pick + half;
        }

        // SAME FLOOR (review F6 on f2c8e2b): a reach / grown-radius arrival never counts
        // through a floor or a ceiling. His feet must be within the crate's OWN HEIGHT of
        // its base (the base record's OBND z-extent -- 109 u for LOTD's outgoing crate,
        // 0x1772A7 OBND z 0..109): a follower on the crate's floor stands at its base
        // (the field stops were 1 u and 0 u off it), while anything a full crate-height
        // above or below it is on a stair landing, a balcony or another storey, where
        // the pick ray would have to pass through the floor. 64 u when the record
        // carries no height. The loot road's grown grab is NOT changed (reported).
        bool SameFloor(RE::Actor* a_follower, RE::TESObjectREFR* a_crate, float* a_dz, float* a_limit) {
            float h = 0.0f;
            if (auto* b = a_crate->GetBaseObject())
                h = static_cast<float>(b->boundData.boundMax.z - b->boundData.boundMin.z);
            const float limit = h > 0.0f ? h : 64.0f;
            const float dz = a_follower->GetPosition().z - a_crate->GetPosition().z;
            if (a_dz) *a_dz = dz;
            if (a_limit) *a_limit = limit;
            return std::abs(dz) <= limit;
        }

        // MAIN THREAD (Detect / OnPostLoad): the activation pick length, read by name from
        // the INI setting collection (Lockpick.cpp's road). Missing -> 0 and one loud line:
        // a BLOCKED stop is then never counted as arrived (never a guessed constant).
        void ReadActivateReach() {
            auto* ini = RE::INISettingCollection::GetSingleton();
            auto* st  = ini ? ini->GetSetting("fActivatePickLength:Interface") : nullptr;
            const float v = st ? st->GetFloat() : 0.0f;
            const float was = g_activateReach.exchange(v);
            if (v <= 0.0f)
                spdlog::warn("[lotd] 'fActivatePickLength:Interface' is unreadable -- a deposit walk that ends "
                             "BLOCKED short of the crate is NOT counted as arrived (it fails as before)");
            else if (was != v)
                spdlog::info("[lotd] interaction reach for a BLOCKED deposit walk: fActivatePickLength = {:.0f} u "
                             "(+ the crate's near-face half-extent)", v);
        }

        bool ShippingInitialised() {
            auto* g = ById<RE::TESGlobal>(g_shipFirstGlob);
            return !g || g->value != 0.0f;   // unreadable -> do not block deposits on it
        }

        bool IntroPending() { return SteadyMs() < g_introPendingUntilMs.load(); }

        // MAIN THREAD. The crate script's (DBM_MuseumShipmentsScript) current Papyrus
        // STATE, lower-cased: "firstactivation" (never activated: IGNORES OnItemAdded,
        // trap (a)), "ready", "waitingtoship" (a shipment is pending; OnActivate here
        // re-arms nothing but resets to Ready, and a Ready OnItemAdded re-registers the
        // 5 h update -- so it is NOT re-activated). Empty = no bound script object.
        std::string CrateState(RE::TESObjectREFR* a_crate) {
            auto* vm     = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            if (!a_crate || !policy) return {};
            const RE::VMHandle h = policy->GetHandleForObject(
                static_cast<RE::VMTypeID>(a_crate->GetFormType()), a_crate);
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (h == policy->EmptyHandle() || !vm->FindBoundObject(h, "DBM_MuseumShipmentsScript", obj) || !obj)
                return {};
            std::string st = obj->currentState.c_str() ? obj->currentState.c_str() : "";
            std::transform(st.begin(), st.end(), st.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return st.empty() ? std::string("(empty state)") : st;
        }

        // "Pick a box and initialize through it" (marth). MAIN THREAD. Activating an
        // outgoing crate runs its FirstActivation state, which shows LOTD's own Museum
        // Shipments message (only while DBMMuseumShipmentsFirst == 0), sets that global
        // and moves THAT crate to Ready. The activator is a loaded follower, never the
        // player (a player activation opens the crate, and a sneaking player's picks a
        // placeable crate up).
        void InitShippingOnMain(RE::FormID a_activator, RE::FormID a_crate, const char* a_why) {
            auto* g = ById<RE::TESGlobal>(g_shipFirstGlob);
            if (!g) return;
            if (g->value != 0.0f) {
                spdlog::info("[lotd] shipping intro ({}): already seen (DBMMuseumShipmentsFirst = 1) -- nothing to show",
                             a_why);
                return;
            }
            RE::Actor* act = a_activator ? RE::TESForm::LookupByID<RE::Actor>(a_activator) : nullptr;
            if (!act) {
                if (auto ids = Followers::ActiveSnapshot())
                    for (auto fid : *ids)
                        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(fid); a && a->Is3DLoaded() && !a->IsDead()) {
                            act = a;
                            break;
                        }
            }
            if (!act) {
                spdlog::info("[lotd] shipping intro ({}): no follower loaded to initialise a crate through -- the "
                             "first deposit trip will show it", a_why);
                return;
            }
            RE::TESObjectREFR* crate = a_crate ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_crate) : nullptr;
            if (!crate) {
                // No crate at hand: the persistent placeable crate LOTD parks in DBMQA.
                // Every outgoing crate ships to the same DropoffCrate, so any one will do.
                crate = ById<RE::TESObjectREFR>(g_placeable1);
            }
            if (!crate) {
                spdlog::warn("[lotd] shipping intro ({}): no outgoing crate resolved -- not shown", a_why);
                return;
            }
            if (IntroPending()) {
                spdlog::info("[lotd] shipping intro ({}): already in flight -- not activated twice", a_why);
                return;
            }
            g_introPendingUntilMs.store(SteadyMs() + 600000);   // a floor: ShippingInitialised() flipping is what ends it
            crate->ActivateRef(act, 0, nullptr, 1, false);   // full processing: the script's OnActivate runs
            spdlog::info("[lotd] shipping intro ({}): activated crate {:08X} through {:08X} -- LOTD shows its Museum "
                         "Shipments message and sets DBMMuseumShipmentsFirst", a_why, crate->GetFormID(),
                         act->GetFormID());
        }

        // ── the trips (worker; g_tripMx held by the callers that touch g_trips) ──
        // How an unsuccessful end treats the (follower, crate) pair: nothing, the short
        // BLOCKED retry, or the full kCrateCooldown. The follower cooldown is separate.
        enum class CrateCd { None, Retry, Full };

        // End a_follower's trip (if any): release the three claims, set the cooldowns,
        // log, forget it. a_followerCd overrides the follower cooldown (zero = the
        // default kCooldownOk / kCooldownFail).
        void EndTripLocked(RE::FormID a_follower, Clock::time_point a_now, const char* a_why, bool a_ok,
                           CrateCd a_crateCd, Clock::duration a_followerCd = Clock::duration::zero()) {
            const auto it = g_trips.find(a_follower);
            if (it == g_trips.end()) return;
            const RE::FormID fid   = a_follower;
            const RE::FormID crate = it->second.crate;
            const float      secs  = std::chrono::duration<float>(a_now - it->second.start).count();
            APMFBridge::ReleaseDeposit(fid);
            Clock::duration fcd = a_followerCd;
            if (fcd == Clock::duration::zero()) {
                // A BLOCKED retry is the follower's own retry too (review SEV-5: the log
                // said 10 s while kCooldownFail held him 30 s).
                fcd = a_ok ? Clock::duration(kCooldownOk)
                           : (a_crateCd == CrateCd::Retry ? Clock::duration(kBlockedRetry)
                                                          : Clock::duration(kCooldownFail));
            }
            g_followerCooldown[fid] = a_now + fcd;
            if (a_crateCd == CrateCd::Full)  g_crateCooldown[Pair(fid, crate)] = a_now + kCrateCooldown;
            if (a_crateCd == CrateCd::Retry) g_crateCooldown[Pair(fid, crate)] = a_now + kBlockedRetry;
            const char* cdNote = "";
            if (a_crateCd == CrateCd::Full)  cdNote = " -- this follower skips that crate for 120 s";
            if (a_crateCd == CrateCd::Retry) cdNote = " -- this follower retries that crate in 10 s";
            if (a_ok) {
                g_blockedStreak.erase(Pair(fid, crate));
                spdlog::info("[lotd] {:08X}: deposit trip DONE at crate {:08X} ({:.1f} s) -- released walk, hold, idle",
                             fid, crate, secs);
            } else {
                spdlog::warn("[lotd] {:08X}: deposit trip ENDED at crate {:08X} after {:.1f} s: {}{}", fid, crate, secs,
                             a_why, cdNote);
            }
            g_trips.erase(it);
        }

        const char* LegName(std::uint32_t a_s) {
            switch (a_s) {
            case APMF_API::kLeg_None:            return "none";
            case APMF_API::kLeg_Pending:         return "pending";
            case APMF_API::kLeg_Walking:         return "walking";
            case APMF_API::kLeg_Arrived:         return "arrived";
            case APMF_API::kLeg_Blocked:         return "BLOCKED";
            case APMF_API::kLeg_CombatCancelled: return "combat";
            case APMF_API::kLeg_DestGone:        return "crate gone";
            case APMF_API::kLeg_StuckTimeout:    return "stuck (2 min)";
            case APMF_API::kLeg_ActorGone:       return "follower gone";
            case APMF_API::kLeg_Failed:          return "FAILED";
            case APMF_API::kLeg_Released:        return "released";
            default:                             return "?";
            }
        }

        // A player-facing inventory menu is open: no transfer under it (the vanilla
        // menus build their lists from the containers MFO would be mutating).
        bool InventoryMenuOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::BarterMenu::MENU_NAME));
        }

        // MAIN THREAD, at arrival. TRAP (a): a crate nobody ever activated is still in
        // FirstActivation and IGNORES OnItemAdded, so it is activated (the follower is
        // the activator). A crate already in Ready / WaitingtoShip is NOT re-activated:
        // re-activating a WaitingtoShip crate resets it to Ready and the next add re-arms
        // LOTD's 5 h clock. An unreadable state is activated (the only way to init it).
        void ArriveOnMain(RE::FormID a_follower, RE::FormID a_crate) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            auto* c = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_crate);
            if (!f || !c) return;
            const std::string st = CrateState(c);
            if (st == "ready" || st == "waitingtoship") {
                spdlog::info("[lotd] {:08X}: crate {:08X} is '{}' -- not re-activated", a_follower, a_crate, st);
                return;
            }
            c->ActivateRef(f, 0, nullptr, 1, false);   // full processing: the script's OnActivate runs
            spdlog::info("[lotd] {:08X}: crate {:08X} was '{}' -- activated so it takes deliveries", a_follower,
                         a_crate, st.empty() ? "unreadable" : st);
        }

        // The transfer, on the MAIN thread. Re-reads everything (the worker's list is
        // a plan, not a promise): a worn / quest instance is skipped here too (the
        // player-given filter ran on the worker, in Shippable). The crate must be CONFIRMED Ready (or WaitingtoShip, whose pending update
        // ships everything in it) first -- a crate still in FirstActivation would keep
        // the items forever. What moved is read back from the crate and entered in the
        // ledger with the DropoffCrate baseline its arrival is measured against.
        void TransferOnMain(RE::FormID a_follower, RE::FormID a_crate, std::vector<ShipItem> a_items,
                            std::shared_ptr<std::atomic<int>> a_result) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            auto* c = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_crate);
            if (!f || !c || c->IsDisabled()) {
                spdlog::warn("[lotd] {:08X}: transfer skipped -- follower or crate {:08X} no longer resolves",
                             a_follower, a_crate);
                a_result->store(2);
                return;
            }
            if (InventoryMenuOpen()) {
                spdlog::warn("[lotd] {:08X}: transfer skipped -- a container / barter menu is open", a_follower);
                a_result->store(2);
                return;
            }
            const std::string st = CrateState(c);
            if (st != "ready" && st != "waitingtoship") {
                spdlog::warn("[lotd] {:08X}: transfer skipped -- crate {:08X} is '{}', not Ready (its activation has "
                             "not run)", a_follower, a_crate, st.empty() ? "unreadable" : st);
                a_result->store(2);
                return;
            }
            auto* drop = ById<RE::TESObjectREFR>(g_dropoffRef);
            auto countIn = [](RE::TESObjectREFR* a_ref, RE::FormID a_base, bool a_noInit) {
                std::int32_t n = 0;
                if (!a_ref) return n;
                for (auto& [o, cnt] : a_ref->GetInventoryCounts(
                         [a_base](RE::TESBoundObject& x) { return x.GetFormID() == a_base; }, a_noInit))
                    n += cnt;
                return n;
            };
            std::int32_t moved = 0;
            std::string names;
            for (const auto& it : a_items) {
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(it.base);
                if (!obj) continue;
                // The player-given record is mutex-guarded (PlayerGiven.cpp): re-checked here.
                if (PlayerGiven::IsPlayerGiven(a_follower, it.base)) continue;
                std::int32_t have = 0;
                bool hold = false;
                // The HANDS a planned WORN ship (MFO-B126) is worn in: unequipped below,
                // before the RemoveItem, per hand with a nullptr extraList and that hand's
                // slot -- the same shape as every other UnequipObject in the tree (review
                // round 3: never hand a live ExtraDataList to a QUEUED unequip that the
                // RemoveItem right after may free).
                bool wornRight = false, wornLeft = false;
                std::int32_t wornCopies = 0;   // review R2-4: an unworn item moves only unworn copies
                for (auto& [o, d] : f->GetInventory([&](RE::TESBoundObject& x) { return x.GetFormID() == it.base; })) {
                    have += d.first;
                    auto* e = d.second.get();
                    if (!e) continue;
                    if (e->IsQuestObject()) hold = true;
                    wornCopies += WornCopies(e);
                    if (it.worn && e->extraLists)
                        for (auto* xl : *e->extraLists) {
                            if (!xl) continue;
                            if (xl->HasType(RE::ExtraDataType::kWornLeft)) wornLeft = true;
                            if (xl->HasType(RE::ExtraDataType::kWorn)) wornRight = true;
                        }
                }
                // IsPlayerPick is NOT re-checked here: g_playerPicks is an unlocked WORKER-only
                // map (EquipAuthority.cpp); Shippable filtered it on the worker.
                if (hold || have <= 0) continue;
                // A NOT-worn item moves only the unworn copies (no unequip); plain copies
                // carry no extra list, so RemoveItem with a null list takes them first.
                const std::int32_t movable = it.worn ? have : have - wornCopies;
                if (movable <= 0) continue;
                if (wornRight || wornLeft) {
                    if (auto* eq = RE::ActorEquipManager::GetSingleton()) {
                        if (wornRight) eq->UnequipObject(f, obj, nullptr, 1, nullptr);
                        if (wornLeft)  eq->UnequipObject(f, obj, nullptr, 1, Loadout::LeftHandSlot());
                    }
                }
                const std::int32_t before = countIn(c, it.base, false);
                f->RemoveItem(obj, std::min(it.count, movable), RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, c);
                const std::int32_t delta = countIn(c, it.base, false) - before;
                if (delta <= 0) continue;
                moved += delta;
                names += std::format("{}'{}' x{}", names.empty() ? "" : ", ", NameOf(obj), delta);
                std::scoped_lock lock(g_ledgerMx);
                g_ledger.push_back({ it.base, delta, countIn(drop, it.base, true), a_crate });
            }
            if (moved > 0)
                spdlog::info("[lotd] {:08X}: DEPOSITED {} item(s) -> crate {:08X} ('{}'): {} (LOTD ships them to "
                             "DropoffCrate some game hours later; the ledger holds them until they arrive)",
                             a_follower, moved, a_crate, st, names);
            else
                spdlog::warn("[lotd] {:08X}: transfer into crate {:08X} moved NOTHING ({} planned item(s))",
                             a_follower, a_crate, a_items.size());
            a_result->store(moved > 0 ? 1 : 2);
            RefreshMainSupplyOnMain();   // the ledger and the player's counts changed: republish now
        }
    }

    // ═════════════════════════ lifecycle ═════════════════════════════════════

    void Detect() {
        g_detected.store(false);
        auto* dh = RE::TESDataHandler::GetSingleton();
        const bool present = dh && dh->LookupLoadedModByName(kLotdPlugin) != nullptr;
        if (present) {
            auto* quest  = dh->LookupForm<RE::TESQuest>(kQuestMuseumApi, kLotdPlugin);
            auto* out    = dh->LookupForm<RE::TESObjectCONT>(kContOutgoing, kLotdPlugin);
            auto* major  = dh->LookupForm<RE::TESGlobal>(kGlobMajor, kLotdPlugin);
            auto* minor  = dh->LookupForm<RE::TESGlobal>(kGlobMinor, kLotdPlugin);
            auto* patch  = dh->LookupForm<RE::TESGlobal>(kGlobPatch, kLotdPlugin);
            const int maj = major ? static_cast<int>(major->value) : -1;
            if (!quest || !out || !major || maj < 6) {
                // A documented degrade, said LOUDLY (not a mask): the feature stays off.
                spdlog::warn("[lotd] LegacyoftheDragonborn.esm is loaded but its layout is UNSUPPORTED (museum quest "
                             "{}, outgoing crate {}, version {}) -- LOTD awareness stays OFF (needs LOTD 6.x)",
                             quest ? "ok" : "MISSING", out ? "ok" : "MISSING",
                             major ? std::format("{}.{}.{}", maj, minor ? static_cast<int>(minor->value) : -1,
                                                 patch ? static_cast<int>(patch->value) : -1)
                                   : std::string("unknown"));
            } else {
                auto id = [&](RE::TESForm* f) { return f ? f->GetFormID() : 0u; };
                g_questId.store(quest->GetFormID());
                g_outgoingBase.store(out->GetFormID());
                g_dropoffRef.store(id(dh->LookupForm(kRefDropoffCrate, kLotdPlugin)));
                g_autosortRef.store(id(dh->LookupForm(kRefAutoSort, kLotdPlugin)));
                g_shipFirstGlob.store(id(dh->LookupForm<RE::TESGlobal>(kGlobShipFirst, kLotdPlugin)));
                g_excludeFlst.store(id(dh->LookupForm<RE::BGSListForm>(kFlstExclude, kLotdPlugin)));
                g_protectedFlst.store(id(dh->LookupForm<RE::BGSListForm>(kFlstProtected, kLotdPlugin)));
                g_holdingCell.store(id(dh->LookupForm(kCellHolding, kLotdPlugin)));
                g_placeable1.store(id(dh->LookupForm(kRefPlaceable1, kLotdPlugin)));
                g_idleGive.store(id(dh->LookupForm<RE::TESIdleForm>(kIdleGive, "Skyrim.esm")));
                g_detected.store(true);
                spdlog::info("[lotd] detected Legacy of the Dragonborn v{}.{}.{} -- museum quest {:08X}, outgoing "
                             "crate base {:08X}, DropoffCrate {:08X}, IdleGive {:08X}; Harbinger {} (the Loot museum "
                             "items gambit needs ABI v17)", maj, minor ? static_cast<int>(minor->value) : -1,
                             patch ? static_cast<int>(patch->value) : -1, quest->GetFormID(), out->GetFormID(),
                             g_dropoffRef.load(), g_idleGive.load(),
                             APMFBridge::DepositApiVersion() ? std::format("ABI v{}", APMFBridge::DepositApiVersion())
                                                             : std::string("absent"));
            }
        } else {
            spdlog::info("[lotd] Legacy of the Dragonborn absent -- LOTD awareness off (the MCM control is hidden)");
        }
        if (dh)
            if (auto* g = dh->LookupForm<RE::TESGlobal>(Forms::kLotdDetectedGlob, Forms::kPlugin))
                g->value = g_detected.load() ? 1.0f : 0.0f;
        if (g_detected.load()) ReadActivateReach();   // main thread (kDataLoaded)
    }

    void RegisterSinks() {
        if (!g_detected.load()) return;
        if (auto* src = SKSE::GetModCallbackEventSource()) {
            src->AddEventSink(ModEventSink::GetSingleton());
            spdlog::info("[lotd] listening for LOTD's MCMRefresh / DBM DisplayListUpdate / DBM DisplaySortComplete");
        }
    }

    void OnPostLoad(bool a_newGame) {
        // A GLOB value is SAVE-PERSISTED: re-assert it on every load (the plugin.cpp rule).
        if (auto* dh = RE::TESDataHandler::GetSingleton())
            if (auto* g = dh->LookupForm<RE::TESGlobal>(Forms::kLotdDetectedGlob, Forms::kPlugin))
                g->value = g_detected.load() ? 1.0f : 0.0f;
        // The toggle-on edge is a change DURING play: a save loaded with it already on
        // is not an edge.
        g_lastEnabled.store(Enabled());
        if (!g_detected.load()) return;
        ReadActivateReach();   // main thread; an INI edit / setini between loads is picked up
        // Main thread already: read now. On a new game the API script may not be bound
        // yet; LOTD's MCMRefresh (all patches registered) triggers the real read.
        RebuildSnapshot(a_newGame ? "new game" : "post-load");
        RefreshMainSupplyOnMain();   // main thread already: the player's / museum counts for the worker
    }

    void OnConfigRead() {
        const bool on = Enabled();
        const bool was = g_lastEnabled.exchange(on);
        if (on && !was) {
            spdlog::info("[lotd] LOTD awareness turned ON");
            if (MainThread::IsInstalled())
                MainThread::Post([]() { InitShippingOnMain(0, 0, "awareness turned on"); });
            RequestRebuild("awareness turned on", true);
        }
    }

    void ClearTransientState() {
        {
            std::scoped_lock lock(g_tripMx);
            g_trips.clear();
            g_followerCooldown.clear();
            g_crateCooldown.clear();
            g_blockedStreak.clear();
        }
        APMFBridge::ClearDepositClaims();
        {
            std::scoped_lock lock(g_snapMx);
            g_snap.reset();
        }
        {
            std::scoped_lock lock(g_ledgerMx);
            g_ledger.clear();
        }
        {
            std::scoped_lock lock(g_mainMx);
            g_main.reset();
        }
        g_needs = Needs{};
        g_retainedSupply.clear();
        g_retainedLogged = 0;
        g_keptForDeposit.clear();
        g_keptSeenUnworn.clear();
        g_relicSeenUnworn.clear();
        g_relicScanAt.clear();
        g_needsLoggedSeq = 0;
        g_rebuildQueued.store(false);
        g_mainQueued.store(false);
        g_introPendingUntilMs.store(0);
        g_gateLog.clear();
        // g_depositLatched is NOT reset: a synchronous ch.12 v2 refusal is process-stable.
    }

    bool Detected() { return g_detected.load(std::memory_order_relaxed); }
    bool Enabled() { return Detected() && Config::g_lootLOTD.load(std::memory_order_relaxed); }
    bool GambitOffered() { return Enabled(); }

    // ═════════════════════════ worker road ═══════════════════════════════════

    bool RunGambit(RE::Actor* a_follower, Clock::time_point a_now) {
        // Museum priority (marth 2026-09-28): the DEPOSIT no longer starts here. It is
        // PriorityDeposit's, checked every service tick right after DepositTick, ahead of
        // the excursion driver and this rule loop, so board order and the pass-0 dibs
        // deferral no longer decide it (and a heal rule the loop has not reached yet can
        // still hold it off). This gambit's own work is the museum LOOT.
        if (!a_follower || !Enabled() || !HarbingerReady()) return false;
        auto& needs = FreshNeeds(a_now);
        if (!needs.snap) return false;
        return Logistics::LootNearby(a_follower, Logistics::Category::Museum, a_now);
    }

    namespace {
        // The priority gate's "why not now" line, once per reason change or every 30 s
        // (worker only). Never on the silent reasons (no rule, nothing to ship, no crate
        // in leash, cooldowns, combat): those are the ordinary idle state.
        void GateLog(RE::FormID a_follower, Clock::time_point a_now, const std::string& a_why) {
            auto& [last, at] = g_gateLog[a_follower];
            if (last == a_why && a_now - at < std::chrono::seconds(30)) return;
            last = a_why;
            at   = a_now;
            spdlog::info("[lotd] {:08X}: museum deposit HELD -- {}", a_follower, a_why);
        }
    }

    bool PriorityDeposit(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now,
                         const DepositGate& a_gate) {
        if (!a_follower || !Enabled() || !HarbingerReady() || !DepositCanRun()) return false;
        const auto fid = a_follower->GetFormID();
        const bool hasGambit = std::any_of(a_state.logistics().begin(), a_state.logistics().end(),
                                           [](const Gambit& g) { return g.enabled && g.actionOpcode == Vocab::kActLootMuseum; });
        if (!hasGambit) return false;
        {
            std::scoped_lock lock(g_tripMx);
            if (g_trips.count(fid)) return false;   // DepositTick owns him
            if (auto it = g_followerCooldown.find(fid); it != g_followerCooldown.end() && a_now < it->second)
                return false;
        }
        // It YIELDS ONLY to combat (his or the player's) and to heals. Sneaking and open
        // menus no longer hold it: a game-pausing menu stops this tick anyway, and the
        // transfer itself still refuses under a container / barter menu (TransferOnMain).
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || a_follower->IsInCombat() || pc->IsInCombat()) return false;
        auto& needs = FreshNeeds(a_now);
        if (!needs.snap) return false;
        // Positive proof for the worn-relic ship (review F4): the needed relics seen
        // UNWORN in his pack, recorded whether or not a crate is near.
        NoteUnwornRelics(a_follower, needs, a_now);
        // LEASH IS BOSS at the start too (review SEV-4): never start a trip while he
        // himself is already outside it (the crate test alone let that through).
        const float leashNow = Logistics::TeleportCompat::View(a_follower).leash;
        if (a_follower->GetPosition().GetDistance(pc->GetPosition()) > leashNow) return false;

        // The cheap crate lookup first (review SEV-5), the inventory walk after it.
        float dist = 0.0f, leash = 0.0f;
        RE::TESObjectREFR* crate = nullptr;
        {
            std::scoped_lock lock(g_tripMx);
            crate = NearestCrate(a_follower, a_now, &dist, &leash);
        }
        if (!crate) return false;   // none inside his leash: behaviour unchanged
        const auto items = Shippable(a_follower, needs);
        if (items.empty()) return false;
        if (!ShippingInitialised()) {
            // The intro was never shown (no follower was loaded when the toggle went
            // on, or it was on before this build): show it now through THIS crate --
            // ONCE (IntroPending) -- and ship on a later trip: a crate whose
            // OnActivate is still showing its message box would not take the items yet.
            if (!IntroPending()) {
                const auto cid = crate->GetFormID();
                MainThread::Post([fid, cid]() { InitShippingOnMain(fid, cid, "first deposit"); });
            }
            std::scoped_lock lock(g_tripMx);
            g_followerCooldown[fid] = a_now + std::chrono::seconds(5);
            return false;
        }
        const RE::FormID crateId = crate->GetFormID();
        // The callbacks run OUTSIDE g_tripMx: ending the excursion goes through the loot
        // road (Packages::LootTravelClear), which must never nest under this lock.
        if (a_gate.healWants && a_gate.healWants()) {
            GateLog(fid, a_now, std::format("a heal is in flight or just fired (crate {:08X} {:.0f} u away)",
                                            crateId, dist));
            return false;
        }
        // A loot excursion fetching a MUSEUM item: the deposit waits for it to land.
        if (a_gate.excursionBlocks && a_gate.excursionBlocks()) {
            GateLog(fid, a_now, std::format("his loot leg is fetching a museum item; the deposit waits for it to "
                                            "land (crate {:08X} {:.0f} u away)", crateId, dist));
            return false;
        }
        std::size_t running = 0;
        {
            std::scoped_lock lock(g_tripMx);
            if (g_trips.count(fid)) return false;
            // CLAIM FIRST (review SEV-4): his loot excursion is ended only once the
            // deposit walk is ours, so a refused claim never costs him the excursion.
            if (!APMFBridge::ClaimDepositTravel(fid, crateId, kCrateRadius)) {
                g_followerCooldown[fid] = a_now + kCooldownFail;   // refused: logged in the bridge
                return false;
            }
            Trip t;
            t.follower = fid;
            t.crate    = crateId;
            t.phase    = Phase::Walking;
            t.start    = a_now;
            t.lastTick = a_now;
            g_trips[fid] = std::move(t);
            running = g_trips.size();
        }
        g_gateLog.erase(fid);
        if (a_gate.endExcursion) a_gate.endExcursion();   // no-op without a running excursion
        std::string what;
        for (const auto& i : items)
            what += std::format("{}{}'{}' x{}", what.empty() ? "" : ", ", i.worn ? "WORN " : "",
                                NameOf(RE::TESForm::LookupByID(i.base)), i.count);
        spdlog::info("[lotd] {:08X}: DEPOSIT trip (PRIORITY) -> crate {:08X} ({:.0f} u; crate {:.0f} u from the "
                     "player, leash {:.0f}; {} trip(s) running) carrying {}", fid, crateId, dist,
                     pc->GetPosition().GetDistance(crate->GetPosition()), leash, running, what);
        return true;
    }

    bool DepositTick(RE::Actor* a_follower, Clock::time_point a_now, const std::function<bool()>& a_healWants) {
        if (!a_follower) return false;
        const auto fid = a_follower->GetFormID();
        std::scoped_lock lock(g_tripMx);
        const auto tit = g_trips.find(fid);
        if (tit == g_trips.end()) return false;
        Trip& trip = tit->second;
        trip.lastTick = a_now;

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!Enabled())                                   { EndTripLocked(fid, a_now, "LOTD awareness off", false, CrateCd::None); return false; }
        if (a_follower->IsInCombat() || (pc && pc->IsInCombat())) { EndTripLocked(fid, a_now, "combat", false, CrateCd::None); return false; }
        if (a_follower->IsDead() || !a_follower->Is3DLoaded()) { EndTripLocked(fid, a_now, "follower gone", false, CrateCd::None); return false; }
        auto* crate = RE::TESForm::LookupByID<RE::TESObjectREFR>(trip.crate);
        if (!crate || crate->IsDisabled() || !crate->Is3DLoaded()) {
            EndTripLocked(fid, a_now, "crate unloaded or disabled", false, CrateCd::Full);
            return false;
        }
        if (a_now - trip.start > kTripMax) { EndTripLocked(fid, a_now, "timed out (90 s)", false, CrateCd::Full); return false; }
        // LEASH IS BOSS (marth 2026-09-28): released as soon as the player is beyond the
        // leash -- from HIM or from the CRATE he is walking to -- at the leash itself, not
        // the loot excursion's x1.15 release. That hysteresis exists for the excursion's
        // re-fill loop (release, then instantly re-pick the same corpse); here a release
        // sets the follower cooldown and the start refuses a crate outside the leash, so
        // the band cannot oscillate faster than kCooldownFail.
        if (pc) {
            const float leash = Logistics::TeleportCompat::View(a_follower).leash;
            const float toF = a_follower->GetPosition().GetDistance(pc->GetPosition());
            const float toC = crate->GetPosition().GetDistance(pc->GetPosition());
            if (toF > leash || toC > leash) {
                EndTripLocked(fid, a_now, std::format("the player left his leash ({:.0f} u from him, {:.0f} u from "
                                                      "the crate; leash {:.0f})", toF, toC, leash).c_str(),
                              false, CrateCd::None);
                return false;
            }
        }

        const RE::FormID cid = trip.crate;
        switch (trip.phase) {
        case Phase::Walking: {
            // It yields to HEALS (marth): while he is still walking, a heal rule that
            // wants to fire ends the trip (no crate cooldown, a short follower one) and
            // the rule loop runs the heal this tick; PriorityDeposit re-starts it once no
            // heal wants the tick. Never mid-give: the idle and the transfer are seconds.
            if (a_healWants && a_healWants()) {
                EndTripLocked(fid, a_now, "yields to a heal rule (restarts once no heal wants the tick)", false,
                              CrateCd::None, kYieldCooldown);
                return false;
            }
            bool arrived = false;
            APMFBridge::LootLegState leg;
            if (APMFBridge::ReadDepositLeg(fid, leg) && leg.ours) {
                if (leg.state == APMF_API::kLeg_Arrived) {
                    arrived = true;
                } else if (leg.state == APMF_API::kLeg_Blocked) {
                    // A BLOCKED walk that stopped within INTERACTION REACH of the crate
                    // (WithinReach: the activation pick length to the crate's near face)
                    // or inside the crate's GROWN arrival radius (the loot road's grown-grab
                    // treatment, below) is an arrival. One that stopped short of both
                    // FAILS, loudly, and widens that radius for the next walk
                    // (NotePathFail): a short retry for THIS follower (a transient block --
                    // an NPC, a door, knocked-over clutter -- clears in seconds), the full
                    // per-pair cooldown only after kBlockedStreakMax BLOCKED ends in a row.
                    float d = 0.0f, reach = 0.0f;
                    const float grown = Logistics::GrabRadiusFor(cid);
                    float dz = 0.0f, dzLimit = 0.0f;
                    const bool sameFloor = SameFloor(a_follower, crate, &dz, &dzLimit);
                    if (!sameFloor) {
                        d = a_follower->GetPosition().GetDistance(crate->GetPosition());
                        spdlog::warn("[lotd] {:08X}: walk to crate {:08X} ended BLOCKED at {:.0f} u on ANOTHER FLOOR "
                                     "(dz {:+.0f} u, limit {:.0f}) -- never counted as arrived", fid, cid, d, dz,
                                     dzLimit);
                    }
                    if (sameFloor && WithinReach(a_follower, crate, &d, &reach)) {
                        arrived = true;
                        spdlog::info("[lotd] {:08X}: walk to crate {:08X} ended BLOCKED at {:.0f} u -- within reach "
                                     "({:.0f} u = fActivatePickLength {:.0f} + the crate's near-face half-extent): "
                                     "counted as ARRIVED", fid, cid, d, reach, g_activateReach.load());
                    } else if (sameFloor && d <= grown) {
                        arrived = true;
                        spdlog::info("[lotd] {:08X}: walk to crate {:08X} ended BLOCKED at {:.0f} u -- inside its "
                                     "grown arrival radius ({:.0f} u): counted as ARRIVED", fid, cid, d, grown);
                    } else {
                        Logistics::NotePathFail(cid);   // the loot road's grown grab: the next walk arrives from further out
                        const int streak = ++g_blockedStreak[Pair(fid, cid)];
                        const bool full = streak >= kBlockedStreakMax;
                        if (full) g_blockedStreak.erase(Pair(fid, cid));
                        EndTripLocked(fid, a_now,
                                      std::format("the walk ended: BLOCKED at {:.0f} u, outside reach ({:.0f} u){} and "
                                                  "the grown arrival radius ({:.0f} u, now {:.0f}) (BLOCKED {} in a row "
                                                  "at this crate)", d, reach,
                                                  g_activateReach.load() > 0.0f ? "" : " [reach unreadable]", grown,
                                                  Logistics::GrabRadiusFor(cid), streak)
                                          .c_str(),
                                      false, full ? CrateCd::Full : CrateCd::Retry);
                        return false;
                    }
                } else if (leg.state > APMF_API::kLeg_Blocked && leg.state <= APMF_API::kLeg_Released) {
                    if (leg.state == APMF_API::kLeg_StuckTimeout) Logistics::NotePathFail(cid);   // a walked stall: widen too
                    EndTripLocked(fid, a_now, std::format("the walk ended: {}", LegName(leg.state)).c_str(), false,
                                  CrateCd::Full);
                    return false;
                }
            }
            // Distance backstop (a leg read that is not ours yet, or never arrives
            // inside the radius on uneven ground), at the crate's GROWN ARRIVAL RADIUS --
            // the loot road's grown-grab treatment (marth 2026-09-28: "I'd give drop-offs
            // the same extending range treatment we used for looting before"), REUSED, not
            // copied: Logistics::GrabRadiusFor / NotePathFail (LootTravel_internal.h), keyed
            // by the crate ref like a corpse, kArrivalDist (200 u) + 100 u per failed walk
            // to it, capped at kGrabRadiusMax (600 u), cleared by a delivered trip. Never
            // the old 175 u below it. A follower who already stands inside it when the trip
            // starts arrives on this first tick, with no walk. The crate itself is always
            // inside the leash (NearestCrate + the release above): the leash stays boss.
            // The part past the old 175 u counts only on the crate's floor (SameFloor,
            // review F6); the old 175 u backstop is unchanged.
            if (!arrived) {
                const float dNow = a_follower->GetPosition().GetDistance(crate->GetPosition());
                if (dNow <= kCrateRadius + kArriveSlack ||
                    (dNow <= Logistics::GrabRadiusFor(cid) && SameFloor(a_follower, crate, nullptr, nullptr)))
                    arrived = true;
            }
            if (!arrived) return true;

            // TRAP (a): activate the crate only if it has not been (ArriveOnMain reads
            // its script state), at least one tick before the transfer, which then
            // CONFIRMS Ready itself.
            MainThread::Post([fid, cid]() { ArriveOnMain(fid, cid); });
            if (!APMFBridge::ClaimDepositIdle(fid, g_idleGive.load(), cid)) {
                // A SYNCHRONOUS ch.12 v2 refusal does not change within a process: latch
                // the deposit off rather than walk-and-refuse forever. Proper animations
                // are required: no give idle, no transfer.
                g_depositLatched.store(true);
                spdlog::warn("[lotd] the museum DEPOSIT is OFF for this session: Harbinger refused the ch.12 v2 give "
                             "idle synchronously (APMF.log names why: [Idle] bIdleV2=0, runtime, self-check). Museum "
                             "looting continues; nothing is shipped.");
                EndTripLocked(fid, a_now, "the give idle was refused (see APMF.log)", false, CrateCd::None);
                return false;
            }
            trip.phase  = Phase::Giving;
            trip.idleAt = a_now;
            spdlog::info("[lotd] {:08X}: at crate {:08X} ({:.0f} u) -- IdleGive requested", fid, cid,
                         a_follower->GetPosition().GetDistance(crate->GetPosition()));
            return true;
        }
        case Phase::Giving: {
            // NO ANIMATION, NO TRANSFER: the give idle must be SEEN live, and still be
            // live at a LATER tick at least kGiveConfirmSec on, before anything moves. A claim
            // Harbinger ends (the engine refused the idle, the NPC is in no state to
            // play it) ends the trip; the never-live grace never counts as live.
            const int st = APMFBridge::DepositIdleStatus(fid);
            if (st == 2) {
                EndTripLocked(fid, a_now, "Harbinger ended the give idle before the hand-over (see APMF.log)", false,
                              CrateCd::Full);
                return false;
            }
            if (st != 1) return true;   // not published yet (the bridge's grace turns into 2 on its own)
            const double clk = Scheduler::ServiceClock();
            if (trip.liveClk < 0.0) { trip.liveClk = clk; return true; }
            if (clk - trip.liveClk < kGiveConfirmSec) return true;   // Harbinger's confirmation window
            if (InventoryMenuOpen()) return true;   // wait it out (kTripMax still bounds the trip)
            auto& needs = FreshNeeds(a_now);
            auto items = Shippable(a_follower, needs);
            if (items.empty()) {
                EndTripLocked(fid, a_now, "nothing left to ship", true, CrateCd::None);
                return false;
            }
            trip.transfer->store(0);
            MainThread::Post([fid, cid, items = std::move(items), result = trip.transfer]() mutable {
                TransferOnMain(fid, cid, std::move(items), std::move(result));
            });
            g_needs.at = {};   // the transfer changes the supply: re-read next time
            trip.phase      = Phase::Settling;
            trip.settleFrom = a_now;
            return true;
        }
        case Phase::Settling: {
            const int r = trip.transfer->load();
            if (r == 2) {   // skipped or moved nothing: FAIL + crate cooldown (never a silent DONE loop)
                EndTripLocked(fid, a_now, "the transfer was skipped or moved nothing (see the transfer line)", false,
                              CrateCd::Full);
                return false;
            }
            if (r == 0 || a_now - trip.settleFrom < kIdleSettle) return true;   // not run yet / idle still playing
            Logistics::g_grabGrow.erase(cid);   // delivered -> stale grow verdict (the loot road's rule)
            EndTripLocked(fid, a_now, "", true, CrateCd::None);
            return false;
        }
        }
        return true;
    }

    void EndDeposit(RE::FormID a_follower, const char* a_why) {
        std::scoped_lock lock(g_tripMx);
        EndTripLocked(a_follower, Clock::now(), a_why, false, CrateCd::None);
    }

    void SweepTrip() {
        std::scoped_lock lock(g_tripMx);
        if (g_trips.empty()) return;
        const auto now = Clock::now();
        const auto ids = Followers::ActiveSnapshot();
        std::vector<std::pair<RE::FormID, const char*>> ends;
        for (const auto& [fid, trip] : g_trips) {
            const char* why = nullptr;
            auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!Enabled())                                        why = "LOTD awareness off";
            else if (!Config::g_logistics.load())                  why = "logistics off";
            else if (!f || f->IsDead() || !f->Is3DLoaded())        why = "follower dead or unloaded";
            else if (now - trip.start > kTripMax)                  why = "timed out (90 s)";
            else if (now - trip.lastTick > kTripStall)             why = "the follower is no longer serviced";
            else if (ids && std::find(ids->begin(), ids->end(), fid) == ids->end())
                                                                   why = "no longer an active follower";
            if (why) ends.emplace_back(fid, why);
        }
        for (const auto& [fid, why] : ends) EndTripLocked(fid, now, why, false, CrateCd::None);
    }

    bool IsDisplayRef(RE::FormID a_ref) {
        if (!a_ref || !g_detected.load()) return false;
        auto snap = CurrentSnapshot();
        return snap && snap->displays.count(a_ref) != 0;
    }

    bool MayBeRelic(RE::FormID a_base) {
        if (!a_base || !g_detected.load()) return false;
        auto snap = CurrentSnapshot();
        return !snap || snap->bases.count(a_base) != 0;
    }

    bool HoldFromSale(RE::FormID a_follower, RE::TESBoundObject* a_base) {
        if (!a_base || !Enabled()) return false;
        auto& needs = FreshNeeds(Clock::now());
        if (!needs.snap || !needs.snap->bases.count(a_base->GetFormID())) return false;
        const auto& excl = UncoveredExcluding(needs, a_follower);
        auto it = excl.find(a_base->GetFormID());
        return it != excl.end() && it->second > 0;
    }

    void NoteKeptForDeposit(RE::FormID a_follower, std::vector<RE::FormID> a_kept, std::vector<RE::FormID> a_inRole,
                            std::vector<RE::FormID> a_keptUnworn) {
        if (a_kept.empty()) { g_keptForDeposit.erase(a_follower); return; }
        auto& rec  = g_keptForDeposit[a_follower];
        rec.kept   = std::unordered_set<RE::FormID>(a_kept.begin(), a_kept.end());
        rec.inRole = std::move(a_inRole);
        // The sticky seen-unworn set: prune to what this full walk still keeps, then add.
        auto& seen = g_keptSeenUnworn[a_follower];
        std::erase_if(seen, [&rec](RE::FormID b) { return !rec.kept.count(b); });
        seen.insert(a_keptUnworn.begin(), a_keptUnworn.end());
        if (seen.empty()) g_keptSeenUnworn.erase(a_follower);
    }

    bool SeenUnwornKept(RE::FormID a_follower, RE::FormID a_base) {
        const auto it = g_keptSeenUnworn.find(a_follower);
        return it != g_keptSeenUnworn.end() && it->second.count(a_base) != 0;
    }

    bool KeepForDeposit(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_base) {
        if (!a_follower || !a_base || !Enabled() || !DepositCanRun()) return false;
        const bool hasGambit = std::any_of(a_state.logistics().begin(), a_state.logistics().end(),
                                           [](const Gambit& g) { return g.enabled && g.actionOpcode == Vocab::kActLootMuseum; });
        return hasGambit && HoldFromSale(a_follower->GetFormID(), a_base);
    }
}

namespace MFO::Logistics {

    // The Category::Museum looter (LootHere / HasLoot). Takes, from one corpse or
    // container, each item the museum still needs after ALL supply (the player, every
    // follower, the museum's own drop-off containers, MFO's in-transit ledger) --
    // never more than is still wanted, never a quest instance, and an artifact only
    // under bLootSpecialItems like every other looter. The source policy (ownership,
    // locks, LOTD crates, player storage -- UNCONDITIONAL for this category) is the
    // scan's, LootScan.cpp.
    bool LootMuseum(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek) {
        if (!a_follower || !a_src || !Lotd::Enabled() || !APMFBridge::DepositSupported()) return false;
        if (IsLOTDDropOff(a_src)) return false;   // belt: never take from a LOTD crate
        auto& needs = Lotd::FreshNeeds(Clock::now());
        if (!needs.snap) return false;
        struct Take { RE::TESBoundObject* obj; std::int32_t count; };
        std::vector<Take> takes;
        for (auto& [obj, data] : a_src->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto it = needs.uncoveredAll.find(obj->GetFormID());
            if (it == needs.uncoveredAll.end() || it->second <= 0) continue;
            if (IsQuestObjectInstance(data.second.get())) continue;
            if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
            if (a_peek) return true;
            takes.push_back({ obj, std::min<std::int32_t>(data.first, it->second) });
        }
        if (a_peek) return false;
        bool moved = false;
        for (const auto& t : takes) {
            if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
            a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_follower);
            needs.uncoveredAll[t.obj->GetFormID()] -= t.count;   // this cache window must not take it twice
            moved = true;
            spdlog::info("[lotd] {:08X}: looted museum item '{}' x{} from {:08X}", a_follower->GetFormID(),
                         Lotd::NameOf(t.obj), t.count, a_src->GetFormID());
        }
        if (moved) needs.uncoveredExcl.clear();   // his holdings changed
        return moved;
    }

    // The route-2b twin of LootMuseum (86e3f9pkg): a LOOSE world item qualifies iff a
    // container holding it would have been looted by LootMuseum -- the base is still
    // needed after ALL supply, it is not a quest item, and an artifact only under
    // bLootSpecialItems. The source bar (displays, the museum halls, player storage,
    // ownership, civilised places) is LooseRefBarred's, applied by the scan to every
    // loose category alike. The pickup is the arrival ActivateRef on MAIN.
    bool LooseMuseumQualifies(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base) {
        if (!a_ref || !a_base || !Lotd::Enabled() || !APMFBridge::DepositSupported()) return false;
        auto& needs = Lotd::FreshNeeds(Clock::now());
        if (!needs.snap) return false;
        auto it = needs.uncoveredAll.find(a_base->GetFormID());
        if (it == needs.uncoveredAll.end() || it->second <= 0) return false;
        return !LooseSpecialItemBlocked(a_ref, a_base->GetFormID());
    }
}
