// logistics/Storage.cpp -- OFF-CLASS STORAGE (ClickUp 86e3t212u). Header: Storage.h (the rulings, threading, contracts).
// Design + seats: Docs/research/2026-11/offclass-storage-design.md. Co-save layout: Serialization.h ('STOR' v1).
//
// RULES KEPT (CLAUDE.md):
//   * co-save: 'STOR' is a NEW independent record. Readers for every shipped version forever (#12), every count bounded (#11),
//     every FormID ResolveFormID'd with an unresolvable one DROPPED (#8), no 0xFF id written (#9). Snapshot under the mutex,
//     write from the copy (never a lock across WriteRecordData).
//   * threading: the worker owns the live path; the table has its own LEAF mutex; no native inventory mutation of an unloaded
//     container (the VM RemoveItem road, Papyrus::DispatchMoveItem).
//   * no list-specific data: the chest is MFO's own record (MFO.esp 0x919).
//   * followers never commit detected crimes: the chest is an unowned ref in an unowned MFO cell, moved by script RemoveItem,
//     which reaches no steal alarm (Linux-Native-Tools crime-and-theft-alarms.md section 1, the MFO_Trade precedent).
//   * never mask a failure: every move is confirmed from the follower's own count; a move that did not land is a WARN and the
//     entry is dropped or retried by rule, never assumed.
//
// THE CHEST IS SHARED by every follower. Two followers may each store a copy of one base: each tracks his own entry and each
// move is confirmed by HIS count, never by the chest's. A retrieval takes "one copy of the base" out of the chest, which is any
// copy of that base in it: with identical base forms that is the same item, but instance data (a temper level, a gem) can
// differ between two followers' copies of one base. That is the documented limit (design doc 0a).
#include "PCH.h"
#include "Storage.h"

#include "Logistics_internal.h"   // ClassWeaponBit / ClassSet, ComputeWeaponRoles, WeaponScore, ArmorScore, the keep set + its gates
#include "Pricing.h"
#include "Papyrus.h"
#include "Config.h"
#include "Forms.h"                // g_storageChest (MFO.esp 0x919)
#include "Followers.h"            // IsPersistableID (#9)
#include "Serialization.h"        // kRecStorage / kStorageVersion, CoSaveBuffer
#include "apmf/APMFBridge.h"      // InCombat (a Harbinger substrate is not combat)
#include "classes/Classes.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Storage {

    namespace {
        // #11 bounds of the record (writer and reader use the same ones).
        constexpr std::uint32_t kMaxSavedFollowers   = 4096;   // the FLWR / PGIV bound
        constexpr std::uint32_t kMaxItemsPerFollower = 16;     // one per Bucket (15 buckets)
        static_assert(static_cast<std::uint32_t>(Bucket::kCount) - 1 <= kMaxItemsPerFollower);

        // Cadences. A move is a VM dispatch: its confirm window is a FLOOR far above VM latency (a busy VM queues for a few
        // seconds; 30 s is what the smith stop's ExtraHealth read-back waits for its own dispatch).
        constexpr auto kTickEvery        = std::chrono::seconds(2);
        constexpr auto kRetrieveEvery    = std::chrono::seconds(5);
        constexpr auto kConfirmWindow    = std::chrono::seconds(30);
        constexpr auto kMenuHold         = std::chrono::seconds(10);   // a confirm window never runs out while a menu is open
        constexpr auto kRetrieveBackoff  = std::chrono::minutes(5);
        constexpr int  kMaxRetrieveMisses = 3;
        constexpr auto kDecisionLogEvery = std::chrono::seconds(60);

        // An in-flight move (session-only; saved as an entry flag when a save lands in its window, and re-created from that flag
        // after the load with a FRESH window, review F4).
        enum class OpKind : std::uint8_t {
            Store,           // him -> chest: a new entry when his count falls below `before`
            Retrieve,        // chest -> him: the entry goes when his count rises above `before`
            SettleStore,     // a load restored a store in flight (kUnverified): confirmed when he carries none, else dropped at the deadline
            SettleRetrieve   // a load restored a retrieval in flight (kRetrieving): retrieved when he carries one, else stays stored
        };
        struct Op {
            OpKind            kind = OpKind::Store;
            RE::FormID        base = 0;
            std::int32_t      before = 0;       // his count of the base at dispatch
            Clock::time_point deadline{};       // 0 = not started (a settle op waits for his first loaded tick)
        };
        struct Miss { int misses = 0; Clock::time_point retryAt{}; };

        // LEAF mutex: never held across an engine, Papyrus or WriteRecordData call.
        std::mutex g_mx;
        Table      g_table;   // SAVED
        // Session-only (ClearTransientState).
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>        g_wants;    // follower -> bases held for storing
        std::unordered_map<RE::FormID, std::vector<Op>>                       g_ops;
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>        g_evict;     // follower -> superseded bases to bring back (in-memory only)
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>        g_blocked;  // a store that did not land / dispatch
        std::unordered_map<std::uint64_t, Miss>                                g_miss;     // (follower, base) retrieval misses
        std::unordered_map<RE::FormID, Clock::time_point>                      g_nextTick, g_nextRetrieve;
        std::unordered_map<RE::FormID, std::uint8_t>                           g_lastClass;
        std::unordered_set<RE::FormID>                                         g_settled;  // followers whose load flags became ops
        std::unordered_map<std::uint64_t, Clock::time_point>                   g_logNext;  // (follower, base) decision lines
        bool g_marginOffLogged = false;
        bool g_chestMissingLogged = false;

        std::uint64_t Key(RE::FormID a_a, RE::FormID a_b) { return (static_cast<std::uint64_t>(a_a) << 32) | a_b; }

        bool Robes() { return Config::g_mageWearRobes.load() && !Config::g_dollsMode.load(); }

        const char* NameOf(const RE::TESForm* a_f) {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        RE::TESObjectREFR* Chest() { return Forms::g_storageChest; }

        // The player handles his inventory (container / gift / barter menu): a count change may be the player's, so no confirm
        // reads it and no new move starts (review F5). UI::IsMenuOpen is the read the logistics worker already uses (LootScan.cpp).
        bool InventoryMenuOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME) ||
                          ui->IsMenuOpen(RE::BarterMenu::MENU_NAME));
        }

        // ── piece classification ──────────────────────────────────────────────────────────────────────────────────
        struct Piece {
            Bucket        bucket = Bucket::None;
            std::uint16_t classBit = 0;   // weapon / shield
            std::uint8_t  kind = 0;       // armor
            bool          armor = false;  // ARMO that is not a shield
        };
        Bucket WeaponBucket(std::uint16_t a_bit) {
            switch (a_bit) {
                case Classes::Weapon::kDagger:   return Bucket::Dagger;
                case Classes::Weapon::kOneHand:  return Bucket::OneHand;
                case Classes::Weapon::kTwoHand:  return Bucket::TwoHand;
                case Classes::Weapon::kBow:      return Bucket::Bow;
                case Classes::Weapon::kCrossbow: return Bucket::Crossbow;
                case Classes::Weapon::kStaff:    return Bucket::Staff;
                default:                         return Bucket::None;
            }
        }
        // Logical armor slot, the keep set's order (SwapUp.cpp armorLogicalSlot): body, head, hands, feet; -1 = none.
        int ArmorSlot(RE::TESObjectARMO* a_ar) {
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            const auto mask = static_cast<std::uint32_t>(a_ar->GetSlotMask());
            if (mask & static_cast<std::uint32_t>(Slot::kBody))  return 1;
            if (IsHeadSlotMask(mask))                            return 0;
            if (mask & static_cast<std::uint32_t>(Slot::kHands)) return 2;
            if (mask & static_cast<std::uint32_t>(Slot::kFeet))  return 3;
            return -1;
        }
        Piece Classify(RE::TESBoundObject* a_obj) {
            Piece p;
            if (!a_obj) return p;
            if (auto* w = a_obj->As<RE::TESObjectWEAP>()) {
                if (IsCreatureWeapon(w) || w->IsBound()) return p;
                p.classBit = ClassWeaponBit(w);
                p.bucket   = WeaponBucket(p.classBit);
                return p;
            }
            if (auto* ar = a_obj->As<RE::TESObjectARMO>()) {
                if (ar->IsShield()) { p.classBit = Classes::Weapon::kShield; p.bucket = Bucket::Shield; return p; }
                if (IsJewelryPiece(ar)) return p;
                const int s = ArmorSlot(ar);
                if (s < 0) return p;
                const bool rated = ar->GetArmorRating() > 0.0f;   // the keep set's split (SwapUp.cpp)
                p.armor = true;
                p.kind  = rated ? Kind::kRated : Kind::kClothing;
                static constexpr Bucket kRatedB[] = { Bucket::HeadRated, Bucket::BodyRated, Bucket::HandsRated, Bucket::FeetRated };
                static constexpr Bucket kClothB[] = { Bucket::HeadCloth, Bucket::BodyCloth, Bucket::HandsCloth, Bucket::FeetCloth };
                p.bucket = rated ? kRatedB[s] : kClothB[s];
            }
            return p;
        }

        // The STORE-side rank inside a storage bucket (only off-class pieces meet here): the keep set's weapon ranks (SwapUp.cpp
        // ComputeKeepSet), ArmorScore for a shield or rated armor (the skill-led judge), gold value for clothing.
        float StoreRank(const WeaponRoles& a_roles, const ArmorPref& a_pref, RE::TESBoundObject* a_obj, const Piece& a_p) {
            if (auto* w = a_obj->As<RE::TESObjectWEAP>()) {
                switch (a_p.bucket) {
                    case Bucket::Staff:    return static_cast<float>(std::clamp<std::int32_t>(w->GetGoldValue(), 0, 0xFFFF));
                    case Bucket::Bow:
                    case Bucket::Crossbow: return static_cast<float>(w->GetAttackDamage());
                    default:               return WeaponScore(a_roles, w);
                }
            }
            if (auto* ar = a_obj->As<RE::TESObjectARMO>()) {
                if (a_p.bucket == Bucket::Shield || a_p.kind == Kind::kRated) return ArmorScore(a_pref, ar);
                return static_cast<float>(std::max<std::int32_t>(ar->GetGoldValue(), 0));
            }
            return 0.0f;
        }

        std::int32_t CountOf(RE::Actor* a_actor, RE::TESBoundObject* a_obj) {
            if (!a_actor || !a_obj) return 0;
            const auto counts = a_actor->GetInventoryCounts([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
            const auto it = counts.find(a_obj);
            return it == counts.end() ? 0 : it->second;
        }
        bool IsWornNow(RE::Actor* a_actor, RE::TESBoundObject* a_obj) {
            for (auto& [obj, data] : a_actor->GetInventory([a_obj](RE::TESBoundObject& o) { return &o == a_obj; }))
                if (obj == a_obj && data.second && data.second->IsWorn()) return true;
            return false;
        }

        bool DecisionLogDue(RE::FormID a_fid, RE::FormID a_base, Clock::time_point a_now) {
            std::scoped_lock lk(g_mx);
            auto& n = g_logNext[Key(a_fid, a_base)];
            if (n.time_since_epoch().count() != 0 && a_now < n) return false;
            n = a_now + kDecisionLogEvery;
            return true;
        }

        std::uint8_t CurrentKind(RE::Actor* a_follower, const FollowerState& a_state, std::uint8_t a_eq) {
            if (a_eq < Classes::kClassCount) return KindOf(static_cast<Classes::Id>(a_eq), Robes());
            return (UsesMageClothing(a_state, a_follower) && Robes()) ? Kind::kClothing : Kind::kRated;
        }

        // Mark every OTHER entry of a_keepBase's bucket as SUPERSEDED (g_evict): Tick brings it back to the follower through the
        // ordinary retrieval dispatch + confirm, and the sale path then judges it (a piece left in the chest with no entry would
        // be lost: the chest is unreachable by the player). The entry stays in the table until that retrieval confirms.
        // Called under g_mx: it does form LOOKUPS only (TESForm::LookupByID, the form map's own reader lock; nothing takes g_mx
        // under that lock).
        void ReleaseSameBucket(std::vector<Item>& a_items, RE::FormID a_keepBase, RE::FormID a_fid) {
            const Piece kp = Classify(RE::TESForm::LookupByID<RE::TESBoundObject>(a_keepBase));
            if (kp.bucket == Bucket::None) return;
            for (const auto& i : a_items) {
                if (i.base == a_keepBase || i.flags) continue;
                auto* o = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
                if (!o || Classify(o).bucket != kp.bucket) continue;
                if (!g_evict[a_fid].insert(i.base).second) continue;
                spdlog::info("[storage] {:08X}: '{}' superseded (a better {} is stored) -- it comes back to him from the MFO chest", a_fid,
                             NameOf(o), BucketName(kp.bucket));
            }
        }

        void DropEntry(RE::FormID a_fid, RE::FormID a_base) {   // under g_mx
            if (auto e = g_evict.find(a_fid); e != g_evict.end()) { e->second.erase(a_base); if (e->second.empty()) g_evict.erase(e); }
            if (auto it = g_table.stored.find(a_fid); it != g_table.stored.end()) {
                std::erase_if(it->second, [&](const Item& e) { return e.base == a_base; });
                if (it->second.empty()) g_table.stored.erase(it);
            }
        }

        // ── codec (one template, two carriers: the SKSE interface and CoSaveBuffer) ────────────────────────────────────
        template <class W>
        void WriteBodyT(W& a_w, const Table& a_t, LoadStats& a_st) {
            std::vector<std::pair<RE::FormID, std::vector<Item>>> fol;
            for (const auto& [fid, items] : a_t.stored) {
                if (fol.size() >= kMaxSavedFollowers) break;
                if (!Followers::IsPersistableID(fid)) { ++a_st.skippedRuntime; continue; }   // #9
                std::vector<Item> v;
                for (const auto& i : items) {
                    if (v.size() >= kMaxItemsPerFollower) break;
                    if (!Followers::IsPersistableID(i.base)) { ++a_st.skippedRuntime; continue; }
                    v.push_back(i);
                }
                if (!v.empty()) fol.emplace_back(fid, std::move(v));
            }
            a_w.WriteRecordData(static_cast<std::uint32_t>(fol.size()));
            for (const auto& [fid, v] : fol) {
                a_w.WriteRecordData(fid);
                a_w.WriteRecordData(static_cast<std::uint32_t>(v.size()));
                for (const auto& i : v) {
                    a_w.WriteRecordData(i.base);
                    a_w.WriteRecordData(i.count);
                    a_w.WriteRecordData(static_cast<std::uint8_t>(i.flags & Item::kAllFlags));
                }
            }
        }

        template <class R, class T>
        bool Rd(R& a_r, T& a_v) { return a_r.ReadRecordData(a_v) == sizeof(T); }

        template <class R>
        bool ReadBodyT(R& a_r, std::uint32_t /*a_version: v1 is the only one*/, Table& a_out, LoadStats& a_st) {
            std::uint32_t followers = 0;
            if (!Rd(a_r, followers)) { spdlog::error("[cosave] short read on STOR follower count -- ABORTING storage load"); return false; }
            if (followers > kMaxSavedFollowers) { spdlog::error("[cosave] implausible STOR follower count {} -- ABORTING storage load", followers); return false; }
            for (std::uint32_t f = 0; f < followers; ++f) {
                RE::FormID rawFid = 0; std::uint32_t n = 0;
                if (!Rd(a_r, rawFid) || !Rd(a_r, n)) { spdlog::error("[cosave] short read in STOR -- ABORTING (kept what was parsed)"); return false; }
                if (n > kMaxItemsPerFollower) { spdlog::error("[cosave] implausible STOR item count {} -- ABORTING (kept what was parsed)", n); return false; }
                RE::FormID   fid   = 0;
                const bool   fidOk = a_r.ResolveFormID(rawFid, fid);   // #8: unresolvable -> his items are read past and DROPPED
                if (!fidOk) ++a_st.droppedFollowers;
                for (std::uint32_t k = 0; k < n; ++k) {
                    RE::FormID rb = 0; std::uint16_t count = 0; std::uint8_t flags = 0;
                    if (!Rd(a_r, rb) || !Rd(a_r, count) || !Rd(a_r, flags)) {
                        spdlog::error("[cosave] short read in STOR items -- ABORTING (kept what was parsed)");
                        return false;
                    }
                    if (!fidOk) { ++a_st.droppedItems; continue; }
                    RE::FormID   base   = 0;
                    const bool   baseOk = a_r.ResolveFormID(rb, base);
                    RE::TESForm* bo     = baseOk ? RE::TESForm::LookupByID(base) : nullptr;
                    if (!bo || !(bo->As<RE::TESObjectWEAP>() || bo->As<RE::TESObjectARMO>()) || count == 0) { ++a_st.droppedItems; continue; }
                    if ((flags & ~Item::kAllFlags) != 0) { ++a_st.clampedFlags; flags &= Item::kAllFlags; }   // #11
                    if (count > 1) { ++a_st.clampedFlags; count = 1; }                                        // #11: one piece per entry
                    auto& v = a_out.stored[fid];
                    if (std::any_of(v.begin(), v.end(), [&](const Item& e) { return e.base == base; })) { ++a_st.droppedItems; continue; }
                    v.push_back(Item{ base, count, flags });
                }
            }
            return true;
        }

#ifdef MFO_DEV_SELFTEST
        struct DevRT {
            RE::FormID        base = 0;
            int               phase = 0;   // 0 wait for the fixture copy, 1 store hop, 2 retrieval hop, 3 done
            std::int32_t      before = 0;
            Clock::time_point deadline{};
            DevRoundTrip      out;
        };
        std::unordered_map<RE::FormID, DevRT> g_devRT;   // under g_mx

        // One step of the armed round trip (WORKER, from Tick). The moves are the live road's (Papyrus::DispatchMoveItem, each hop
        // confirmed from his count), so a PASS proves both directions through the MFO chest, receiver binding included.
        void DevStepRoundTrip(RE::Actor* a_follower, Clock::time_point a_now) {
            const RE::FormID fid = a_follower->GetFormID();
            DevRT rt;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_devRT.find(fid);
                if (it == g_devRT.end() || it->second.out.done) return;
                rt = it->second;
            }
            auto* obj   = RE::TESForm::LookupByID<RE::TESBoundObject>(rt.base);
            auto* chest = Chest();
            const std::int32_t c = CountOf(a_follower, obj);
            auto finish = [&](bool a_ok, std::string a_ev) { rt.phase = 3; rt.out.done = true; rt.out.ok = a_ok; rt.out.evidence = std::move(a_ev); };
            if (!obj || !chest) finish(false, !chest ? "the MFO storage chest (MFO.esp 0x919) did not resolve" : "the fixture base does not resolve");
            else if (rt.phase == 0) {
                if (rt.deadline.time_since_epoch().count() == 0) rt.deadline = a_now + std::chrono::seconds(20);
                if (c >= 1) {
                    if (Papyrus::DispatchMoveItem(a_follower, obj, 1, chest)) { rt.phase = 1; rt.before = c; rt.deadline = a_now + kConfirmWindow; }
                    else finish(false, "the store hop (him -> MFO chest) could not be dispatched");
                } else if (a_now > rt.deadline) finish(false, "the fixture copy never reached his pack");
            } else if (rt.phase == 1) {
                if (c < rt.before) {
                    if (Papyrus::DispatchMoveItem(chest, obj, 1, a_follower)) { rt.phase = 2; rt.before = c; rt.deadline = a_now + kConfirmWindow; }
                    else finish(false, std::format("stored (his count {} -> {}), but the retrieval hop FROM the MFO chest could not be "
                                                   "dispatched (the receiver binding, review F1)", rt.before, c));
                } else if (a_now > rt.deadline) finish(false, std::format("the store hop did not land in 30 s (his count stays {})", c));
            } else if (rt.phase == 2) {
                if (c > rt.before) finish(true, std::format("round trip through the MFO chest {:08X}: stored (his count fell), retrieved "
                                                            "(his count {} -> {}), both by Papyrus RemoveItem", chest->GetFormID(), rt.before, c));
                else if (a_now > rt.deadline)
                    finish(false, std::format("the retrieval hop from the MFO chest did not land in 30 s (his count stays {})", c));
            }
            if (rt.out.done)
                spdlog::info("[storage] DEV round trip {:08X}: {} -- {}", fid, rt.out.ok ? "PASS" : "FAIL", rt.out.evidence);
            std::scoped_lock lk(g_mx);
            if (auto it = g_devRT.find(fid); it != g_devRT.end()) it->second = rt;
        }
#endif
    }

    // ── the pure decision ───────────────────────────────────────────────────────────────────────────────────────────
    const char* BucketName(Bucket a_b) {
        static constexpr const char* kNames[] = { "none", "dagger", "one-hander", "two-hander", "bow", "crossbow", "staff", "shield",
                                                  "rated helmet", "rated body", "rated gauntlets", "rated boots",
                                                  "clothing head", "clothing body", "clothing hands", "clothing feet" };
        const auto i = static_cast<std::size_t>(a_b);
        return i < std::size(kNames) ? kNames[i] : "?";
    }

    std::uint8_t KindOf(Classes::Id a_class, bool a_robes) {
        return (a_robes && Classes::Get(a_class).line == Classes::Line::Mage) ? Kind::kClothing : Kind::kRated;
    }

    Plan PlannedFor(std::uint8_t a_equipped, Classes::Mask a_mastered, bool a_robes) {
        Plan p;
        if (a_equipped >= Classes::kClassCount) return p;
        const auto eqId = static_cast<Classes::Id>(a_equipped);
        const auto root = Classes::Get(eqId).rootT1;
        // "future in line": every class reachable from the equipped one through parentMask (a child names its parent in its unlock
        // rule), staying on the same T1 root (his own tree line).
        Classes::Mask ahead = 0, frontier = Classes::Bit(eqId);
        for (int guard = 0; frontier && guard < 8; ++guard) {
            Classes::Mask next = 0;
            for (std::size_t c = 0; c < Classes::kClassCount; ++c) {
                const auto id = static_cast<Classes::Id>(c);
                const auto& d = Classes::Get(id);
                const auto bit = Classes::Bit(id);
                if ((ahead | Classes::Bit(eqId)) & bit) continue;
                if (root == 0 || d.rootT1 != root || !(d.parentMask & frontier)) continue;
                next |= bit;
            }
            ahead |= next;
            frontier = next;
        }
        p.classes = (a_mastered | ahead) & ~Classes::Bit(eqId);
        for (std::size_t c = 0; c < Classes::kClassCount; ++c) {
            const auto id = static_cast<Classes::Id>(c);
            if (!(p.classes & Classes::Bit(id))) continue;
            const auto& d = Classes::Get(id);
            if (!(d.weapons & Classes::Weapon::kUnrestricted)) p.weapons |= d.weapons;
            if (d.rootT1 != 0) p.kinds |= KindOf(id, a_robes);
        }
        return p;
    }

    bool PlannedOffClass(const StoreInput& a, const char** a_whyOut) {
        const char* why = nullptr;
        const bool weaponish = a.armorKind == 0;
        if (!a.classesOn)                          why = "classes off (no storing)";
        else if (a.bucket == Bucket::None)         why = "not a stored kind";
        else if (weaponish && a.classBit == 0)     why = "not a stored kind";
        else if (weaponish ? (a.currentSet == 0 || (a.classBit & a.currentSet)) : (a.armorKind == a.currentKind))
                                                   why = "in his class now";
        else if (weaponish ? !(a.classBit & a.plan.weapons) : !(a.armorKind & a.plan.kinds))
                                                   why = "no mastered or future class of his uses it";
        if (a_whyOut) *a_whyOut = why ? why : "";
        return why == nullptr;
    }

    StoreOutput DecideStore(const StoreInput& a) {
        StoreOutput o;
        o.useValue  = Pricing::Apply(a.value, a.buyMult);
        o.saleValue = Pricing::Apply(a.value, a.sellMult);
        if (!PlannedOffClass(a, &o.why)) return o;
        if (!a.storageReady) { o.why = "no MFO storage chest (MFO.esp out of date?)"; return o; }
        if (!(a.rank > a.bestOtherRank || (a.rank == a.bestOtherRank && !a.tieLoses))) {
            o.why = "a better one of its kind is carried or stored"; return o;
        }
        if (!a.singleCopy) { o.why = "he carries more than one copy (a store could not be told from a sale)"; return o; }
        if (!(o.useValue > o.saleValue && o.useValue - o.saleValue >= a.minMargin)) {
            o.why = "selling now beats buying it back later"; return o;
        }
        o.verdict = Verdict::Store;
        o.why = "planned, top of its kind, worth keeping";
        return o;
    }

    bool DecideRetrieve(const RetrieveInput& a, const char** a_whyOut) {
        const char* why = "";
        bool ok = false;
        const bool weaponish = a.armorKind == 0;
        if (a.bucket == Bucket::None)                                          why = "not a stored kind";
        else if (weaponish ? !(a.currentSet == 0 || (a.classBit & a.currentSet)) : (a.armorKind != a.currentKind))
                                                                               why = "not his class now";
        else if (!a.saleKeeps)                                                 why = "the sale path would not keep it (not better than what he carries)";
        else { ok = true; why = a.classChanged ? "class change" : "better than what he carries"; }
        if (a_whyOut) *a_whyOut = why;
        return ok;
    }

    // ── live seats ───────────────────────────────────────────────────────────────────────────────────────────────
    bool Enabled() {
        if (!Config::g_offClassStorage.load()) return false;
        if (!Chest()) {
            if (!g_chestMissingLogged) {   // worker-serial
                g_chestMissingLogged = true;
                spdlog::error("[storage] the MFO storage chest (MFO.esp 0x919 MFO_StorageChestRef) did not resolve -- off-class storage is OFF "
                              "(install the MFO.esp that ships with this DLL)");
            }
            return false;
        }
        return true;
    }

    std::int32_t HoldFromSale(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_obj,
                              RE::InventoryEntryData* a_entry, std::int32_t a_count, std::int32_t a_value) {
        // CHEAP CHECKS FIRST (review F7): no inventory walk, no roles, no price until the piece is a planned off-class one.
        if (!a_follower || !a_obj || a_count <= 0 || !Enabled()) return 0;
        if (a_entry && a_entry->IsWorn()) return 0;   // a worn force-sell row: storage never takes anything off him
        const RE::FormID fid  = a_follower->GetFormID();
        const RE::FormID base = a_obj->GetFormID();
        if (!Followers::IsPersistableID(base)) return 0;   // a runtime (0xFF) form could never be saved (#9)
        const Piece pc = Classify(a_obj);
        if (pc.bucket == Bucket::None) return 0;
        if (pc.armor && Config::g_dollsMode.load()) return 0;   // the player dresses him (dolls mode): his apparel is not ours
        std::vector<Item> mine;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_ops.find(fid); it != g_ops.end())
                for (const auto& op : it->second)
                    if (op.base == base) return 1;   // a move of it is in flight: never sell it from under the move
            if (const auto it = g_blocked.find(fid); it != g_blocked.end() && it->second.count(base)) return 0;
            if (const auto it = g_table.stored.find(fid); it != g_table.stored.end()) mine = it->second;
        }
        const auto now = Clock::now();
        // Review F8: a copy of this base is stored already -- never a second entry of one base (and never replace it: the stored
        // copy would be orphaned in the chest). This copy sells as today.
        if (std::any_of(mine.begin(), mine.end(), [&](const Item& i) { return i.base == base; })) {
            if (DecisionLogDue(fid, base, now))
                spdlog::info("[storage] {:08X} '{}': '{}' sells -- a copy of it is already stored", fid, NameOf(a_follower), NameOf(a_obj));
            return 0;
        }

        StoreInput in;
        in.bucket = pc.bucket; in.classBit = pc.armor ? 0 : pc.classBit; in.armorKind = pc.armor ? pc.kind : 0;
        const std::uint8_t eq = Classes::Active() ? Classes::GetEquipped(fid) : Classes::kNoClass;
        in.classesOn = eq < Classes::kClassCount;
        if (in.classesOn) {
            in.currentSet  = ClassSet(fid);
            in.currentKind = KindOf(static_cast<Classes::Id>(eq), Robes());
            in.plan        = PlannedFor(eq, Classes::GetMastered(fid), Robes());
        }
        if (!PlannedOffClass(in)) return 0;   // the common case: in his class, or no planned class uses it -- sells, nothing read

        in.storageReady = Chest() != nullptr;
        in.singleCopy = a_count == 1;
        in.value = a_value;
        in.minMargin = Config::g_errandMinSellGold.load();

        const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);
        const ArmorPref   pref  = ArmorPrefFor(a_follower);
        in.rank = StoreRank(roles, pref, a_obj, pc);
        std::int32_t bestOtherValue = -1; RE::FormID bestOtherId = 0;
        auto consider = [&](RE::TESBoundObject* o) {
            if (!o || o == a_obj) return;
            const Piece op = Classify(o);
            if (op.bucket != pc.bucket) return;
            const float r = StoreRank(roles, pref, o, op);
            const std::int32_t v = std::max<std::int32_t>(o->GetGoldValue(), 0);
            if (r > in.bestOtherRank || (r == in.bestOtherRank && (v > bestOtherValue || (v == bestOtherValue && o->GetFormID() < bestOtherId)))) {
                in.bestOtherRank = r; bestOtherValue = v; bestOtherId = o->GetFormID();
            }
        };
        for (auto& [o, data] : a_follower->GetInventory([](RE::TESBoundObject& b) { return b.IsWeapon() || b.IsArmor(); }))
            if (data.first > 0) consider(o);
        for (const auto& i : mine) consider(RE::TESForm::LookupByID<RE::TESBoundObject>(i.base));
        if (bestOtherId && in.rank == in.bestOtherRank) {
            const std::int32_t myV = std::max<std::int32_t>(a_value, 0);
            in.tieLoses = bestOtherValue > myV || (bestOtherValue == myV && bestOtherId < base);
        }
        const auto quote = Pricing::Resolve(a_follower->GetHandle(), RE::ActorHandle{}, 0);
        in.buyMult = quote.buy; in.sellMult = quote.sell;
        if (!quote.modelOn && !g_marginOffLogged) {
            g_marginOffLogged = true;   // worker-serial
            spdlog::info("[storage] the barter model is off (bSpeechPricing 0 or a GMST unreadable): buying back costs what selling "
                         "pays, so nothing is worth storing and every off-class piece sells");
        }
        const StoreOutput out = DecideStore(in);
        const bool logIt = DecisionLogDue(fid, base, now);
        if (out.verdict == Verdict::Store) {
            {
                std::scoped_lock lk(g_mx);
                g_wants[fid].insert(base);
            }
            if (logIt)
                spdlog::info("[storage] {:08X} '{}': HOLD '{}' ({}) for the MFO chest -- planned for classes {:X}, top of its kind (rank {:.1f} vs "
                             "{:.1f}), use {}g vs sale {}g (margin {}g >= {}g, a placeholder)",
                             fid, NameOf(a_follower), NameOf(a_obj), BucketName(pc.bucket), in.plan.classes, in.rank, in.bestOtherRank,
                             out.useValue, out.saleValue, out.useValue - out.saleValue, in.minMargin);
            return 1;
        }
        {
            std::scoped_lock lk(g_mx);
            if (auto it = g_wants.find(fid); it != g_wants.end()) it->second.erase(base);
        }
        if (logIt)
            spdlog::info("[storage] {:08X} '{}': '{}' ({}) sells -- {} (use {}g vs sale {}g)", fid, NameOf(a_follower), NameOf(a_obj),
                         BucketName(pc.bucket), out.why, out.useValue, out.saleValue);
        return 0;
    }

    void Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now) {
        if (!a_follower || !a_follower->Is3DLoaded()) return;
        const RE::FormID fid = a_follower->GetFormID();
        {
            std::scoped_lock lk(g_mx);
            auto& n = g_nextTick[fid];
            if (n.time_since_epoch().count() != 0 && a_now < n) return;
            n = a_now + kTickEvery;
        }
        if (APMFBridge::InCombat(a_follower)) return;
#ifdef MFO_DEV_SELFTEST
        DevStepRoundTrip(a_follower, a_now);
#endif
        if (!Enabled()) return;
        auto* chest = Chest();

        const std::uint8_t eq = Classes::Active() ? Classes::GetEquipped(fid) : Classes::kNoClass;
        bool classChanged = false;
        {
            std::scoped_lock lk(g_mx);
            auto [it, fresh] = g_lastClass.try_emplace(fid, eq);
            if (!fresh && it->second != eq) { classChanged = true; it->second = eq; }
            // 0. A LOAD'S FLAGS BECOME OPS (review F4): an entry saved mid-move gets a settle op with a FRESH window, started on
            //    this, his first loaded tick (never judged on the first pass). Once per follower per session.
            if (g_settled.insert(fid).second)
                if (const auto st = g_table.stored.find(fid); st != g_table.stored.end())
                    for (const auto& i : st->second)
                        if (i.flags) g_ops[fid].push_back(Op{ (i.flags & Item::kRetrieving) ? OpKind::SettleRetrieve : OpKind::SettleStore,
                                                              i.base, 0, a_now + kConfirmWindow });
        }

        const bool menu = InventoryMenuOpen();
        std::vector<Op> ops;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_ops.find(fid); it != g_ops.end()) ops = it->second;
        }
        std::vector<Op> keepOps;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(kConfirmWindow).count();

        // 1. CONFIRM in-flight moves from HIS count (the only count read: never the shared chest's). While the player has an
        //    inventory menu open nothing is confirmed (his count may be the player's doing) and no window runs out (review F5).
        for (auto op : ops) {
            if (menu) { op.deadline = std::max(op.deadline, a_now + kMenuHold); keepOps.push_back(op); continue; }
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(op.base);
            const std::int32_t c = CountOf(a_follower, obj);
            const bool timedOut = a_now > op.deadline;
            std::scoped_lock lk(g_mx);
            switch (op.kind) {
                case OpKind::Store:
                    if (c < op.before) {
                        auto& v = g_table.stored[fid];
                        v.push_back(Item{ op.base, 1, 0 });
                        ReleaseSameBucket(v, op.base, fid);
                        if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(op.base);
                        spdlog::info("[storage] {:08X} '{}': STORED '{}' in the MFO chest (verified: it left his pack)", fid, NameOf(a_follower), NameOf(obj));
                    } else if (timedOut) {
                        g_blocked[fid].insert(op.base);
                        if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(op.base);
                        spdlog::warn("[storage] {:08X}: '{}' did NOT leave his pack within {} s -- not recorded, no more storing of it this "
                                     "session (it sells as before)", fid, NameOf(obj), secs);
                    } else keepOps.push_back(op);
                    break;
                case OpKind::Retrieve:
                    if (c > op.before) {
                        g_miss.erase(Key(fid, op.base));
                        DropEntry(fid, op.base);
                        spdlog::info("[storage] {:08X} '{}': RETRIEVED '{}' from the MFO chest", fid, NameOf(a_follower), NameOf(obj));
                    } else if (timedOut) {
                        auto& m = g_miss[Key(fid, op.base)];
                        ++m.misses;
                        if (m.misses >= kMaxRetrieveMisses) {
                            DropEntry(fid, op.base);
                            g_miss.erase(Key(fid, op.base));
                            spdlog::warn("[storage] {:08X}: '{}' did not arrive from the MFO chest ({} tries) -- it is no longer in the chest; "
                                         "entry dropped", fid, NameOf(obj), kMaxRetrieveMisses);
                        } else {
                            m.retryAt = a_now + kRetrieveBackoff;
                            spdlog::warn("[storage] {:08X}: '{}' did not arrive from the MFO chest within {} s (try {}/{}) -- retry in 5 min",
                                         fid, NameOf(obj), secs, m.misses, kMaxRetrieveMisses);
                        }
                    } else keepOps.push_back(op);
                    break;
                case OpKind::SettleStore: {
                    auto st = g_table.stored.find(fid);
                    if (st == g_table.stored.end()) break;   // gone meanwhile
                    auto it = std::find_if(st->second.begin(), st->second.end(), [&](const Item& e) { return e.base == op.base; });
                    if (it == st->second.end()) break;
                    if (c == 0) {
                        it->flags &= static_cast<std::uint8_t>(~Item::kUnverified);
                        spdlog::info("[storage] {:08X}: store of '{}' (in flight at the save) CONFIRMED -- he no longer carries it", fid, NameOf(obj));
                        ReleaseSameBucket(st->second, op.base, fid);
                    } else if (timedOut) {
                        spdlog::warn("[storage] {:08X}: store of '{}' (in flight at the save) NOT confirmed within {} s -- he still carries {}; entry "
                                     "dropped", fid, NameOf(obj), secs, c);
                        DropEntry(fid, op.base);
                    } else keepOps.push_back(op);
                    break;
                }
                case OpKind::SettleRetrieve: {
                    auto st = g_table.stored.find(fid);
                    if (st == g_table.stored.end()) break;
                    auto it = std::find_if(st->second.begin(), st->second.end(), [&](const Item& e) { return e.base == op.base; });
                    if (it == st->second.end()) break;
                    if (c >= 1) {
                        spdlog::info("[storage] {:08X}: RETRIEVED '{}' (the move was in flight at the save; he carries it now)", fid, NameOf(obj));
                        DropEntry(fid, op.base);
                    } else if (timedOut) {
                        it->flags &= static_cast<std::uint8_t>(~Item::kRetrieving);   // still in the chest: an ordinary entry again
                        spdlog::info("[storage] {:08X}: the retrieval of '{}' in flight at the save did not land -- it stays stored", fid, NameOf(obj));
                    } else keepOps.push_back(op);
                    break;
                }
            }
        }

        // 2. DISPATCH wanted stores (the sale path held them). Never while a menu is open.
        std::vector<RE::FormID> wants;
        if (!menu) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_wants.find(fid); it != g_wants.end())
                for (const RE::FormID b : it->second)
                    if (std::none_of(keepOps.begin(), keepOps.end(), [&](const Op& o) { return o.base == b; })) wants.push_back(b);
        }
        for (const RE::FormID b : wants) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(b);
            const std::int32_t c = CountOf(a_follower, obj);
            const char* drop = nullptr;
            if (c != 1)                          drop = c == 0 ? "he no longer carries it" : "he carries more than one copy now";
            else if (IsWornNow(a_follower, obj)) drop = "he wears it now";
            if (drop) {
                std::scoped_lock lk(g_mx);
                if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(b);
                spdlog::info("[storage] {:08X}: '{}' not stored -- {}", fid, NameOf(obj), drop);
                continue;
            }
            if (Papyrus::DispatchMoveItem(a_follower, obj, 1, chest)) {
                keepOps.push_back(Op{ OpKind::Store, b, c, a_now + kConfirmWindow });
            } else {
                std::scoped_lock lk(g_mx);
                if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(b);
                g_blocked[fid].insert(b);
                spdlog::warn("[storage] {:08X}: the move of '{}' to the MFO chest could not be dispatched (VM / handle / binding) -- not stored, "
                             "no more tries this session (it sells as before)", fid, NameOf(obj));
            }
        }

        // 3. RETRIEVE one piece per pass (own cadence), never while a menu is open or another move of his is in flight.
        bool due = false;
        if (!menu && keepOps.empty()) {
            std::scoped_lock lk(g_mx);
            auto& n = g_nextRetrieve[fid];
            if (n.time_since_epoch().count() == 0 || a_now >= n) { due = true; n = a_now + kRetrieveEvery; }
        }
        if (due) {
            std::vector<Item> items;
            {
                std::scoped_lock lk(g_mx);
                if (const auto it = g_table.stored.find(fid); it != g_table.stored.end()) items = it->second;
            }
            const std::uint16_t curSet  = ClassSet(fid);
            const std::uint8_t  curKind = items.empty() ? 0 : CurrentKind(a_follower, a_state, eq);
            std::unordered_set<RE::FormID> evict;
            {
                std::scoped_lock lk(g_mx);
                if (const auto e = g_evict.find(fid); e != g_evict.end()) evict = e->second;
            }
            for (const auto& i : items) {
                if (i.flags) continue;
                {
                    std::scoped_lock lk(g_mx);
                    if (const auto m = g_miss.find(Key(fid, i.base)); m != g_miss.end() && a_now < m->second.retryAt) continue;
                }
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
                if (!obj) {
                    std::scoped_lock lk(g_mx);
                    DropEntry(fid, i.base);
                    spdlog::warn("[storage] {:08X}: entry {:08X} dropped -- its form no longer resolves", fid, i.base);
                    continue;
                }
                if (CountOf(a_follower, obj) != 0) continue;   // a copy in his pack would make the confirm ambiguous
                const Piece pc = Classify(obj);
                const bool superseded = evict.count(i.base) != 0;   // a better piece of its bucket is stored: back to him, the sale path judges
                RetrieveInput r;
                r.bucket = pc.bucket; r.classBit = pc.armor ? 0 : pc.classBit; r.armorKind = pc.armor ? pc.kind : 0;
                r.currentSet = curSet; r.currentKind = curKind; r.classChanged = classChanged;
                // The cheap class half first; the keep set only for a piece that fits his class now.
                const bool fits = r.armorKind == 0 ? (curSet == 0 || (r.classBit & curSet)) : (r.armorKind == curKind);
                if (!superseded && (!fits || r.bucket == Bucket::None)) continue;
                // Review F2: the SALE PATH's own verdict, as if he carried it: the keep set with this piece added plus the sell loop's
                // gates (Economy.cpp SaleKeepsUnworn). A piece it would not keep is never brought back to be sold.
                const char* why = "";
                if (superseded) why = "superseded by a better stored piece";
                else {
                    const KeepSet       ks    = ComputeKeepSet(a_follower, a_state, obj);
                    const SaleKeepGates gates = BuildSaleKeepGates(a_follower, a_state);
                    r.saleKeeps = SaleKeepsUnworn(ks, gates, obj);
                    if (!DecideRetrieve(r, &why)) continue;
                }
                if (Papyrus::DispatchMoveItem(chest, obj, 1, a_follower)) {
                    keepOps.push_back(Op{ OpKind::Retrieve, i.base, 0, a_now + kConfirmWindow });
                    spdlog::info("[storage] {:08X} '{}': RETRIEVE '{}' from the MFO chest -- {}", fid, NameOf(a_follower), NameOf(obj), why);
                } else {
                    // Review F1: a dispatch failure is a miss with the same backoff (never a WARN every 5 s), counted toward nothing:
                    // the piece is still in the chest, so the entry stays.
                    std::scoped_lock lk(g_mx);
                    g_miss[Key(fid, i.base)].retryAt = a_now + kRetrieveBackoff;
                    spdlog::warn("[storage] {:08X}: the retrieval of '{}' from the MFO chest could not be dispatched (VM / handle / binding) -- "
                                 "retry in 5 min", fid, NameOf(obj));
                }
                break;
            }
        }

        std::scoped_lock lk(g_mx);
        if (keepOps.empty()) g_ops.erase(fid);
        else                 g_ops[fid] = std::move(keepOps);
    }

    bool IsStored(RE::FormID a_follower, RE::FormID a_base) {
        std::scoped_lock lk(g_mx);
        const auto it = g_table.stored.find(a_follower);
        if (it == g_table.stored.end()) return false;
        return std::any_of(it->second.begin(), it->second.end(), [&](const Item& i) { return i.base == a_base; });
    }

    std::vector<Item> StoredOf(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_table.stored.find(a_follower);
        return it != g_table.stored.end() ? it->second : std::vector<Item>{};
    }

    void Forget(RE::FormID a_follower) {
        std::vector<Item> items;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_table.stored.find(a_follower); it != g_table.stored.end()) { items = std::move(it->second); g_table.stored.erase(it); }
            g_wants.erase(a_follower);
            g_ops.erase(a_follower);
            g_evict.erase(a_follower);
            g_blocked.erase(a_follower);
            g_lastClass.erase(a_follower);
        }
        if (items.empty()) return;
        // His pieces must not stay in the chest untracked (the player cannot reach it): send each back to him if he still resolves,
        // else to the player. No confirm tracking after this (the entry is gone); the dispatch result is logged.
        auto* chest = Chest();
        auto* who   = RE::TESForm::LookupByID<RE::Actor>(a_follower);
        RE::TESObjectREFR* dest = who ? static_cast<RE::TESObjectREFR*>(who) : static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton());
        for (const auto& i : items) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
            if (!chest || !dest || !obj) {
                spdlog::warn("[storage] {:08X}: roster removal -- piece {:08X} NOT returned ({}); it stays in the MFO chest untracked", a_follower,
                             i.base, !chest ? "no chest" : !dest ? "no destination" : "its form no longer resolves");
                continue;
            }
            const bool ok = Papyrus::DispatchMoveItem(chest, obj, 1, dest);
            spdlog::info("[storage] {:08X}: roster removal -- '{}' from the MFO chest to {} ({})", a_follower, NameOf(obj), who ? "him" : "the player",
                         ok ? "dispatched" : "dispatch FAILED (VM / handle / binding); it stays in the chest untracked");
        }
    }

    // ── the 'STOR' record ────────────────────────────────────────────────────────────────────────────────────────────
    void WriteBody(CoSaveBuffer& a_out, const Table& a_table, LoadStats* a_stats) {
        LoadStats st;
        WriteBodyT(a_out, a_table, a_stats ? *a_stats : st);
    }
    bool ReadBody(CoSaveBuffer& a_in, std::uint32_t a_version, Table& a_out, LoadStats& a_stats) {
        return ReadBodyT(a_in, a_version, a_out, a_stats);
    }

    void CoSave(SKSE::SerializationInterface* a_intfc) {
        // SNAPSHOT under the lock, write from the copy. A move in flight is written as its entry with a flag, so the load settles it
        // in a fresh window from his count: a store -> a new entry kUnverified; a retrieval -> kRetrieving. A settle op still running
        // keeps its flag.
        Table snap;
        {
            std::scoped_lock lk(g_mx);
            snap = g_table;
            for (const auto& [fid, ops] : g_ops)
                for (const auto& op : ops) {
                    auto& v = snap.stored[fid];
                    auto it = std::find_if(v.begin(), v.end(), [&](const Item& e) { return e.base == op.base; });
                    switch (op.kind) {
                        case OpKind::Store:
                            if (it == v.end()) v.push_back(Item{ op.base, 1, Item::kUnverified });
                            break;
                        case OpKind::Retrieve:
                            if (it != v.end()) it->flags |= Item::kRetrieving;
                            break;
                        case OpKind::SettleStore:
                        case OpKind::SettleRetrieve:
                            break;   // the entry still carries its own flag
                    }
                }
            std::erase_if(snap.stored, [](const auto& kv) { return kv.second.empty(); });
        }
        if (snap.stored.empty()) return;   // written ONLY when there is something to keep
        if (!a_intfc->OpenRecord(kRecStorage, kStorageVersion)) {
            spdlog::error("[cosave] OpenRecord('STOR') failed -- off-class storage NOT saved (the pieces stay in the MFO chest untracked)");
            return;
        }
        LoadStats st;
        WriteBodyT(*a_intfc, snap, st);
        std::size_t pieces = 0;
        for (const auto& [f, v] : snap.stored) pieces += v.size();
        spdlog::info("[cosave] saved {} stored piece(s) for {} follower(s), schema v{}{}", pieces, snap.stored.size(), kStorageVersion,
                     st.skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) id(s)", st.skippedRuntime) : std::string{});
    }

    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        Table t;
        LoadStats st;
        const bool complete = ReadBodyT(*a_intfc, a_version, t, st);
        std::size_t pieces = 0, flagged = 0;
        for (const auto& [f, v] : t.stored)
            for (const auto& i : v) { ++pieces; if (i.flags) ++flagged; }
        const std::size_t followers = t.stored.size();
        {
            std::scoped_lock lk(g_mx);
            g_table = std::move(t);
        }
        spdlog::info("[cosave] loaded {} stored piece(s) for {} follower(s){}, {} in flight at the save (settled on each follower's first "
                     "loaded tick); dropped {} follower(s), {} piece(s), {} clamped field(s)",
                     pieces, followers, complete ? "" : " (record ABORTED, kept what was parsed)", flagged, st.droppedFollowers,
                     st.droppedItems, st.clampedFlags);
    }

    void ClearRecord() {
        std::scoped_lock lk(g_mx);
        g_table = Table{};
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_wants.clear();
        g_ops.clear();
        g_evict.clear();
        g_blocked.clear();
        g_miss.clear();
        g_nextTick.clear();
        g_nextRetrieve.clear();
        g_lastClass.clear();
        g_settled.clear();
        g_logNext.clear();
        g_marginOffLogged = false;
#ifdef MFO_DEV_SELFTEST
        g_devRT.clear();
#endif
    }

#ifdef MFO_DEV_SELFTEST
    void DevArmRoundTrip(RE::FormID a_follower, RE::FormID a_base) {
        std::scoped_lock lk(g_mx);
        DevRT rt;
        rt.base = a_base;
        rt.out.armed = true;
        g_devRT[a_follower] = rt;
        spdlog::info("[storage] DEV round trip ARMED for {:08X}: base {:08X} through the MFO chest", a_follower, a_base);
    }
    void DevDisarmRoundTrip(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        g_devRT.erase(a_follower);
    }
    DevRoundTrip DevReadRoundTrip(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_devRT.find(a_follower);
        return it != g_devRT.end() ? it->second.out : DevRoundTrip{};
    }
#endif
}
