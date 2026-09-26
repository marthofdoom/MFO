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

#include <algorithm>
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
        constexpr float kCrateRange     = 3000.0f;   // a crate this close to the follower counts
        constexpr float kCrateRadius    = 100.0f;    // ch.19 arrival radius (APMF clamps to [50,512])
        constexpr float kArriveSlack    = 75.0f;     // distance backstop when no leg state is read
        constexpr auto  kTripMax        = std::chrono::seconds(90);
        constexpr auto  kGiveAfter      = std::chrono::milliseconds(1000);   // the reach, then the transfer
        constexpr auto  kIdleSettle     = std::chrono::milliseconds(3500);   // IdleGive's clip, then release
        constexpr auto  kNeedsTtl       = std::chrono::seconds(2);
        constexpr auto  kRebuildMinGap  = std::chrono::seconds(5);
        constexpr auto  kCooldownOk     = std::chrono::seconds(10);
        constexpr auto  kCooldownFail   = std::chrono::seconds(30);
        constexpr auto  kCrateCooldown  = std::chrono::seconds(120);
        // LOTD ships 5 game hours after the first item lands; + a margin. In game DAYS.
        constexpr float kInTransitDays  = 5.5f / 24.0f;

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

        // ── the IN-TRANSIT LEDGER (main writes, worker reads) ────────────────────
        // Items MFO put into an outgoing crate. LOTD moves them to DropoffCrate 5 game
        // hours later; until then an UNLOADED town crate cannot be read, so this is the
        // only record they exist. Session-only: a reload inside the window can cost one
        // duplicate relic, which LOTD leaves harmlessly in DropoffCrate (a known limit,
        // design §2 B -- not masked).
        struct InTransit { RE::FormID base; std::int32_t count; float untilDays; };
        std::mutex             g_ledgerMx;
        std::vector<InTransit> g_ledger;   // guarded by g_ledgerMx

        float GameDays() {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? cal->GetCurrentGameTime() : 0.0f;
        }

        // ── the NEEDS cache (worker only) ────────────────────────────────────────
        struct Needs {
            std::uint32_t                   snapSeq = 0;
            Clock::time_point               at{};
            std::shared_ptr<const Snapshot> snap;
            std::vector<std::uint32_t>      open;          // open slot indices
            std::unordered_map<RE::FormID, std::int32_t> fixedSupply;   // player, dropoff, autosort, in transit
            // Follower supply, sorted by follower FormID: a follower's own relic
            // "covers" a slot only after every LOWER-id follower's copy, so two
            // followers holding one needed relic never both keep (or both ship) it.
            std::vector<std::pair<RE::FormID, std::unordered_map<RE::FormID, std::int32_t>>> followerSupply;
            std::unordered_map<RE::FormID, std::int32_t> uncoveredAll;   // after ALL supply (the loot want)
            std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, std::int32_t>> uncoveredExcl;
        };
        Needs g_needs;   // worker only
        std::uint32_t g_needsLoggedSeq = 0;

        // ── the TRIP (one deposit at a time, worker road + dismissal edge) ──────
        enum class Phase { Walking, Giving, Settling };
        struct Trip {
            bool              active   = false;
            RE::FormID        follower = 0;
            RE::FormID        crate    = 0;
            Phase             phase    = Phase::Walking;
            Clock::time_point start{};
            Clock::time_point idleAt{};
            bool              legEndLogged = false;
        };
        std::mutex g_tripMx;
        Trip       g_trip;   // guarded by g_tripMx
        std::unordered_map<RE::FormID, Clock::time_point> g_followerCooldown;   // guarded by g_tripMx
        std::unordered_map<RE::FormID, Clock::time_point> g_crateCooldown;      // guarded by g_tripMx

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
                                slot.displays.push_back(ref->GetFormID());
                                AddAccepted(itemGroup ? (k < subItems.size() ? subItems[k] : nullptr) : item,
                                            slot.accepted, exclude, protectd);
                            }
                            ++groupSlots;
                        } else if (auto* ref = d ? d->As<RE::TESObjectREFR>() : nullptr) {
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

        // ── the needs (WORKER) ───────────────────────────────────────────────────
        using CountMap = std::unordered_map<RE::FormID, std::int32_t>;

        void AddCounts(RE::TESObjectREFR* a_ref, const Snapshot& a_snap, CountMap& a_out, bool a_noInit) {
            if (!a_ref) return;
            const auto counts = a_ref->GetInventoryCounts(
                [&a_snap](RE::TESBoundObject& o) { return a_snap.bases.count(o.GetFormID()) != 0; }, a_noInit);
            for (const auto& [obj, n] : counts)
                if (obj && n > 0) a_out[obj->GetFormID()] += n;
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

        Needs& FreshNeeds(Clock::time_point a_now) {
            auto snap = CurrentSnapshot();
            if (!snap) {
                g_needs = Needs{};
                RequestRebuild("needs asked, no snapshot");
                return g_needs;
            }
            if (g_needs.snap == snap && a_now - g_needs.at < kNeedsTtl) return g_needs;

            Needs n;
            n.snap    = snap;
            n.snapSeq = snap->seq;
            n.at      = a_now;
            for (std::uint32_t i = 0; i < snap->slots.size(); ++i) {
                bool allOff = true;
                for (auto id : snap->slots[i].displays) {
                    auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                    if (!r || !r->IsDisabled()) { allOff = false; break; }
                }
                if (allOff) n.open.push_back(i);
            }
            std::int32_t sPlayer = 0, sDrop = 0, sAuto = 0, sTransit = 0, sFollowers = 0;
            auto sum = [](const CountMap& m) { std::int32_t t = 0; for (auto& [k, v] : m) t += v; return t; };
            {
                CountMap m;
                AddCounts(RE::PlayerCharacter::GetSingleton(), *snap, m, false);
                sPlayer = sum(m);
                for (auto& [k, v] : m) n.fixedSupply[k] += v;
            }
            {
                // Persistent museum-side containers, read WITHOUT initialising an
                // unloaded one's inventory (a_noInit): delivered, waiting to be sorted.
                CountMap m;
                AddCounts(ById<RE::TESObjectREFR>(g_dropoffRef), *snap, m, true);
                sDrop = sum(m);
                for (auto& [k, v] : m) n.fixedSupply[k] += v;
                CountMap a;
                AddCounts(ById<RE::TESObjectREFR>(g_autosortRef), *snap, a, true);
                sAuto = sum(a);
                for (auto& [k, v] : a) n.fixedSupply[k] += v;
            }
            {
                const float today = GameDays();
                std::scoped_lock lock(g_ledgerMx);
                std::erase_if(g_ledger, [today](const InTransit& t) { return t.untilDays < today; });
                for (const auto& t : g_ledger) { n.fixedSupply[t.base] += t.count; sTransit += t.count; }
            }
            if (auto ids = Followers::ActiveSnapshot()) {
                std::vector<RE::FormID> sorted(ids->begin(), ids->end());
                std::sort(sorted.begin(), sorted.end());
                for (auto fid : sorted) {
                    CountMap m;
                    AddCounts(RE::TESForm::LookupByID<RE::Actor>(fid), *snap, m, false);
                    sFollowers += sum(m);
                    n.followerSupply.emplace_back(fid, std::move(m));
                }
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
                             g_needs.snapSeq, g_needs.open.size(), needBases, sPlayer, sFollowers, sDrop, sAuto,
                             sTransit);
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

        struct ShipItem { RE::FormID base; std::int32_t count; };

        // The relics this follower carries that the museum needs FROM HIM: never worn,
        // favourited (the player's manual override), quest-flagged (LOTD's crate ships
        // quest items too -- trap (b)) or his own stock gear.
        std::vector<ShipItem> Shippable(RE::Actor* a_follower, Needs& a_n) {
            std::vector<ShipItem> out;
            if (!a_follower || !a_n.snap) return out;
            const auto fid = a_follower->GetFormID();
            const auto& excl = UncoveredExcluding(a_n, fid);
            if (excl.empty()) return out;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto it = excl.find(obj->GetFormID());
                if (it == excl.end() || it->second <= 0) continue;
                auto* e = data.second.get();
                if (e && (e->IsWorn() || e->IsFavorited() || e->IsQuestObject())) continue;
                if (Logistics::IsStockGear(fid, obj->GetFormID())) continue;
                out.push_back({ obj->GetFormID(), std::min<std::int32_t>(data.first, it->second) });
            }
            return out;
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

        // The nearest enabled, loaded outgoing crate within kCrateRange of the
        // follower (and inside his leash from the player), never a placeable crate
        // still parked in the DBMQA holding cell, never one on cooldown. WORKER: walks
        // only ATTACHED cells anchored on the follower and the player (LootNearby's
        // crash4-safe shape).
        RE::TESObjectREFR* NearestCrate(RE::Actor* a_follower, Clock::time_point a_now, float* a_dist) {
            const RE::FormID base = g_outgoingBase.load();
            if (!base || !a_follower) return nullptr;
            const RE::FormID holding = g_holdingCell.load();
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const auto fpos = a_follower->GetPosition();
            const auto ppos = pc ? pc->GetPosition() : fpos;
            const float leash = Logistics::TeleportCompat::View(a_follower).leash;
            RE::TESObjectREFR* best = nullptr;
            float bestD = kCrateRange;
            std::unordered_set<RE::TESObjectCELL*> seen;
            for (auto* anchor : { static_cast<RE::TESObjectREFR*>(a_follower), static_cast<RE::TESObjectREFR*>(pc) }) {
                auto* cell = anchor ? anchor->GetParentCell() : nullptr;
                if (!cell || !cell->IsAttached() || !seen.insert(cell).second) continue;
                cell->ForEachReferenceInRange(fpos, kCrateRange, [&](RE::TESObjectREFR& r) {
                    auto* b = r.GetBaseObject();
                    if (!b || b->GetFormID() != base) return RE::BSContainer::ForEachResult::kContinue;
                    if (r.IsDisabled() || !r.Is3DLoaded()) return RE::BSContainer::ForEachResult::kContinue;
                    if (auto* pcell = r.GetParentCell(); pcell && pcell->GetFormID() == holding)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (auto it = g_crateCooldown.find(r.GetFormID()); it != g_crateCooldown.end() && a_now < it->second)
                        return RE::BSContainer::ForEachResult::kContinue;
                    if (pc && ppos.GetDistance(r.GetPosition()) > leash)
                        return RE::BSContainer::ForEachResult::kContinue;
                    const float d = fpos.GetDistance(r.GetPosition());
                    if (d < bestD) { bestD = d; best = &r; }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (a_dist) *a_dist = bestD;
            return best;
        }

        bool ShippingInitialised() {
            auto* g = ById<RE::TESGlobal>(g_shipFirstGlob);
            return !g || g->value != 0.0f;   // unreadable -> do not block deposits on it
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
            crate->ActivateRef(act, 0, nullptr, 1, false);   // full processing: the script's OnActivate runs
            spdlog::info("[lotd] shipping intro ({}): activated crate {:08X} through {:08X} -- LOTD shows its Museum "
                         "Shipments message and sets DBMMuseumShipmentsFirst", a_why, crate->GetFormID(),
                         act->GetFormID());
        }

        // ── the trip (worker; g_tripMx held by the callers that touch g_trip) ────
        void EndTripLocked(Clock::time_point a_now, const char* a_why, bool a_ok, bool a_crateCooldown) {
            if (!g_trip.active) return;
            const auto fid = g_trip.follower;
            const auto crate = g_trip.crate;
            APMFBridge::ReleaseDeposit(fid);
            g_followerCooldown[fid] = a_now + (a_ok ? kCooldownOk : kCooldownFail);
            if (a_crateCooldown) g_crateCooldown[crate] = a_now + kCrateCooldown;
            const auto secs = std::chrono::duration<float>(a_now - g_trip.start).count();
            if (a_ok)
                spdlog::info("[lotd] {:08X}: deposit trip DONE at crate {:08X} ({:.1f} s) -- released walk, hold, idle",
                             fid, crate, secs);
            else
                spdlog::warn("[lotd] {:08X}: deposit trip ENDED at crate {:08X} after {:.1f} s: {}", fid, crate, secs,
                             a_why);
            g_trip = Trip{};
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

        // The transfer, on the MAIN thread. Re-reads everything (the worker's list is
        // a plan, not a promise): a worn / favourited / quest instance is skipped here
        // too, and what moved is read back from the crate, then entered in the ledger.
        void TransferOnMain(RE::FormID a_follower, RE::FormID a_crate, std::vector<ShipItem> a_items) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            auto* c = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_crate);
            if (!f || !c || c->IsDisabled()) {
                spdlog::warn("[lotd] {:08X}: transfer skipped -- follower or crate {:08X} no longer resolves",
                             a_follower, a_crate);
                return;
            }
            std::int32_t moved = 0;
            std::string names;
            const float until = GameDays() + kInTransitDays;
            for (const auto& it : a_items) {
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(it.base);
                if (!obj) continue;
                std::int32_t have = 0;
                bool hold = false;
                for (auto& [o, d] : f->GetInventory([&](RE::TESBoundObject& x) { return x.GetFormID() == it.base; })) {
                    have += d.first;
                    if (auto* e = d.second.get(); e && (e->IsWorn() || e->IsFavorited() || e->IsQuestObject()))
                        hold = true;
                }
                if (hold || have <= 0) continue;
                auto crateCount = [&]() {
                    std::int32_t n = 0;
                    for (auto& [o, cnt] : c->GetInventoryCounts([&](RE::TESBoundObject& x) {
                             return x.GetFormID() == it.base; }))
                        n += cnt;
                    return n;
                };
                const std::int32_t before = crateCount();
                f->RemoveItem(obj, std::min(it.count, have), RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, c);
                const std::int32_t delta = crateCount() - before;
                if (delta <= 0) continue;
                moved += delta;
                names += std::format("{}'{}' x{}", names.empty() ? "" : ", ", NameOf(obj), delta);
                std::scoped_lock lock(g_ledgerMx);
                g_ledger.push_back({ it.base, delta, until });
            }
            if (moved > 0)
                spdlog::info("[lotd] {:08X}: DEPOSITED {} item(s) -> crate {:08X}: {} (LOTD ships them to the museum "
                             "in 5 game hours)", a_follower, moved, a_crate, names);
            else
                spdlog::warn("[lotd] {:08X}: transfer into crate {:08X} moved NOTHING ({} planned item(s))",
                             a_follower, a_crate, a_items.size());
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
        // Main thread already: read now. On a new game the API script may not be bound
        // yet; LOTD's MCMRefresh (all patches registered) triggers the real read.
        RebuildSnapshot(a_newGame ? "new game" : "post-load");
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
            g_trip = Trip{};
            g_followerCooldown.clear();
            g_crateCooldown.clear();
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
        g_needs = Needs{};
        g_needsLoggedSeq = 0;
        g_rebuildQueued.store(false);
    }

    bool Detected() { return g_detected.load(std::memory_order_relaxed); }
    bool Enabled() { return Detected() && Config::g_lootLOTD.load(std::memory_order_relaxed); }
    bool GambitOffered() { return Enabled(); }

    // ═════════════════════════ worker road ═══════════════════════════════════

    bool RunGambit(RE::Actor* a_follower, Clock::time_point a_now) {
        if (!a_follower || !Enabled() || !HarbingerReady()) return false;
        const auto fid = a_follower->GetFormID();
        auto& needs = FreshNeeds(a_now);
        if (!needs.snap) return false;

        // ── start a deposit trip? ──
        bool tryDeposit = true;
        {
            std::scoped_lock lock(g_tripMx);
            if (g_trip.active) tryDeposit = false;   // one trip at a time
            if (auto it = g_followerCooldown.find(fid); it != g_followerCooldown.end() && a_now < it->second)
                tryDeposit = false;
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (tryDeposit && (Logistics::SlotIndexOf(fid) >= 0 || a_follower->IsInCombat() ||
                           (pc && pc->IsInCombat()) || Logistics::PlayerActivelyStealthing()))
            tryDeposit = false;
        if (tryDeposit)
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME))
                tryDeposit = false;
        if (tryDeposit) {
            const auto items = Shippable(a_follower, needs);
            if (!items.empty()) {
                std::scoped_lock lock(g_tripMx);
                float dist = 0.0f;
                if (auto* crate = NearestCrate(a_follower, a_now, &dist)) {
                    if (!ShippingInitialised()) {
                        // The intro was never shown (no follower was loaded when the
                        // toggle went on, or it was on before this build): show it now
                        // through THIS crate, and ship on the next trip -- a crate
                        // whose OnActivate is still showing its message box would not
                        // take the items yet.
                        const auto cid = crate->GetFormID();
                        MainThread::Post([fid, cid]() { InitShippingOnMain(fid, cid, "first deposit"); });
                        g_followerCooldown[fid] = a_now + std::chrono::seconds(5);
                        return true;
                    }
                    if (APMFBridge::ClaimDepositTravel(fid, crate->GetFormID(), kCrateRadius)) {
                        g_trip.active   = true;
                        g_trip.follower = fid;
                        g_trip.crate    = crate->GetFormID();
                        g_trip.phase    = Phase::Walking;
                        g_trip.start    = a_now;
                        std::string what;
                        for (const auto& i : items)
                            what += std::format("{}'{}' x{}", what.empty() ? "" : ", ",
                                                NameOf(RE::TESForm::LookupByID(i.base)), i.count);
                        spdlog::info("[lotd] {:08X}: DEPOSIT trip -> crate {:08X} ({:.0f} u) carrying {}", fid,
                                     crate->GetFormID(), dist, what);
                        return true;
                    }
                    g_followerCooldown[fid] = a_now + kCooldownFail;   // refused: logged in the bridge
                }
            }
        }

        // ── else loot museum items (Category::Museum through the loot machinery) ──
        return Logistics::LootNearby(a_follower, Logistics::Category::Museum, a_now);
    }

    bool DepositTick(RE::Actor* a_follower, Clock::time_point a_now) {
        if (!a_follower) return false;
        const auto fid = a_follower->GetFormID();
        std::scoped_lock lock(g_tripMx);
        if (!g_trip.active || g_trip.follower != fid) return false;

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!Enabled())                                   { EndTripLocked(a_now, "LOTD awareness off", false, false); return false; }
        if (a_follower->IsInCombat() || (pc && pc->IsInCombat())) { EndTripLocked(a_now, "combat", false, false); return false; }
        if (a_follower->IsDead() || !a_follower->Is3DLoaded()) { EndTripLocked(a_now, "follower gone", false, false); return false; }
        auto* crate = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_trip.crate);
        if (!crate || crate->IsDisabled() || !crate->Is3DLoaded()) {
            EndTripLocked(a_now, "crate unloaded or disabled", false, true);
            return false;
        }
        if (a_now - g_trip.start > kTripMax) { EndTripLocked(a_now, "timed out (90 s)", false, true); return false; }
        // The loot driver's leash rule: a trip never outlasts the player walking off.
        if (pc && a_follower->GetPosition().GetDistance(pc->GetPosition()) > Logistics::TeleportCompat::View(a_follower).release) {
            EndTripLocked(a_now, "the player left his leash", false, false);
            return false;
        }

        const RE::FormID cid = g_trip.crate;
        switch (g_trip.phase) {
        case Phase::Walking: {
            bool arrived = false;
            APMFBridge::LootLegState leg;
            if (APMFBridge::ReadDepositLeg(fid, leg) && leg.ours) {
                if (leg.state == APMF_API::kLeg_Arrived) {
                    arrived = true;
                } else if (leg.state >= APMF_API::kLeg_Blocked && leg.state <= APMF_API::kLeg_Released) {
                    EndTripLocked(a_now, std::format("the walk ended: {}", LegName(leg.state)).c_str(), false, true);
                    return false;
                }
            }
            // Distance backstop (a leg read that is not ours yet, or never arrives
            // inside the radius on uneven ground).
            if (!arrived && a_follower->GetPosition().GetDistance(crate->GetPosition()) <= kCrateRadius + kArriveSlack)
                arrived = true;
            if (!arrived) return true;

            // TRAP (a): a crate nobody ever activated is still in its FirstActivation
            // state and IGNORES OnItemAdded. Activate it now (the follower is the
            // activator) so its OnActivate runs, one tick before the transfer.
            MainThread::Post([fid, cid]() {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                auto* c = RE::TESForm::LookupByID<RE::TESObjectREFR>(cid);
                if (f && c) c->ActivateRef(f, 0, nullptr, 1, false);
            });
            if (!APMFBridge::ClaimDepositIdle(fid, g_idleGive.load(), cid)) {
                // Proper animations are required: no give idle, no transfer.
                EndTripLocked(a_now, "the give idle was refused (see APMF.log)", false, true);
                return false;
            }
            g_trip.phase  = Phase::Giving;
            g_trip.idleAt = a_now;
            spdlog::info("[lotd] {:08X}: at crate {:08X} ({:.0f} u) -- crate activated, IdleGive requested", fid, cid,
                         a_follower->GetPosition().GetDistance(crate->GetPosition()));
            return true;
        }
        case Phase::Giving: {
            if (APMFBridge::DepositIdleStatus(fid) == 2) {
                EndTripLocked(a_now, "Harbinger ended the give idle before the hand-over (see APMF.log)", false, true);
                return false;
            }
            if (a_now - g_trip.idleAt < kGiveAfter) return true;
            auto& needs = FreshNeeds(a_now);
            auto items = Shippable(a_follower, needs);
            if (items.empty()) {
                EndTripLocked(a_now, "nothing left to ship", true, false);
                return false;
            }
            MainThread::Post([fid, cid, items = std::move(items)]() mutable {
                TransferOnMain(fid, cid, std::move(items));
            });
            g_needs.at = {};   // the transfer changes the supply: re-read next time
            g_trip.phase = Phase::Settling;
            return true;
        }
        case Phase::Settling:
            if (a_now - g_trip.idleAt < kIdleSettle) return true;
            EndTripLocked(a_now, "", true, false);
            return false;
        }
        return true;
    }

    void EndDeposit(RE::FormID a_follower, const char* a_why) {
        std::scoped_lock lock(g_tripMx);
        if (g_trip.active && g_trip.follower == a_follower)
            EndTripLocked(Clock::now(), a_why, false, false);
    }

    bool HoldFromSale(RE::FormID a_follower, RE::TESBoundObject* a_base) {
        if (!a_base || !Enabled()) return false;
        auto& needs = FreshNeeds(Clock::now());
        if (!needs.snap || !needs.snap->bases.count(a_base->GetFormID())) return false;
        const auto& excl = UncoveredExcluding(needs, a_follower);
        auto it = excl.find(a_base->GetFormID());
        return it != excl.end() && it->second > 0;
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
}
