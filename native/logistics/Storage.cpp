// logistics/Storage.cpp -- OFF-CLASS STORAGE (ClickUp 86e3t212u). Header: Storage.h (the rulings, threading, contracts).
// Design + seats: Docs/research/2026-11/offclass-storage-design.md. Co-save layout: Serialization.h ('STOR' v1).
//
// RULES KEPT (CLAUDE.md):
//   * co-save: 'STOR' is a NEW independent record. Readers for every shipped version forever (#12), every count bounded (#11),
//     every FormID ResolveFormID'd with an unresolvable one DROPPED (#8), no 0xFF id written (#9). Snapshot under the mutex,
//     write from the copy (never a lock across WriteRecordData).
//   * threading: the worker owns the live path; the tables have their own LEAF mutex; no native inventory mutation of an
//     unloaded container (the VM RemoveItem road, Papyrus::DispatchMoveItem).
//   * no list-specific data: chests are found at runtime from record flags and ownership.
//   * followers never commit detected crimes: only a container the player owns (or one in a cell he owns), moved by script
//     RemoveItem, which reaches no steal alarm (Linux-Native-Tools crime-and-theft-alarms.md section 1, MFO_Trade precedent).
//   * never mask a failure: every move is confirmed from the follower's own count; a move that did not land is a WARN and the
//     entry is dropped or retried by rule, never assumed.
#include "PCH.h"
#include "Storage.h"

#include "Logistics_internal.h"   // ClassWeaponBit / ClassSet, ComputeWeaponRoles, WeaponScore, ArmorScore, the keep-set helpers
#include "Pricing.h"
#include "Papyrus.h"
#include "Config.h"
#include "Followers.h"            // IsPersistableID (#9)
#include "LocationTypes.h"        // TownAnchor (informational)
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
        constexpr std::uint32_t kMaxHomes            = 16;
        constexpr std::uint32_t kMaxSavedFollowers   = 4096;   // the FLWR / PGIV bound
        constexpr std::uint32_t kMaxItemsPerFollower = 16;     // one per Bucket (15 buckets)
        static_assert(static_cast<std::uint32_t>(Bucket::kCount) - 1 <= kMaxItemsPerFollower);

        // Cadences. A move is a VM dispatch: its confirm window is a FLOOR far above VM latency (a busy VM queues for a few
        // seconds; 30 s is what the smith stop's ExtraHealth read-back waits for its own dispatch).
        constexpr auto kTickEvery      = std::chrono::seconds(2);
        constexpr auto kRetrieveEvery  = std::chrono::seconds(5);
        constexpr auto kConfirmWindow  = std::chrono::seconds(30);
        constexpr auto kRetrieveBackoff = std::chrono::minutes(5);
        constexpr int  kMaxRetrieveMisses = 3;
        constexpr auto kDecisionLogEvery = std::chrono::seconds(60);
        // A cell is walked again at most once a minute while someone stands in it: a Hearthfire chest is DISABLED until the player
        // builds it, and a house bought while he stands inside becomes his on the next walk.
        constexpr auto kCellRecheck = std::chrono::seconds(60);

        // An in-flight move (session-only; saved as an entry flag when a save lands in its window).
        enum class OpKind : std::uint8_t {
            Store,        // him -> chest (a new entry when his count falls)
            Retrieve,     // chest -> him (the entry goes when his count rises)
            MigrateOut,   // interim chest -> him, the first hop of interim -> home
            MigrateIn     // him -> the home chest, the second hop
        };
        struct Op {
            OpKind            kind = OpKind::Store;
            RE::FormID        base = 0;
            RE::FormID        chest = 0;        // Store / MigrateIn: the destination; Retrieve / MigrateOut: the source
            RE::FormID        fromChest = 0;    // MigrateIn: the interim chest the entry still names
            std::int32_t      before = 0;       // his count of the base at dispatch
            Clock::time_point at{};
        };
        struct Miss { int misses = 0; Clock::time_point retryAt{}; };

        // LEAF mutex: never held across an engine, Papyrus or WriteRecordData call.
        std::mutex g_mx;
        Table      g_table;   // SAVED
        // Session-only (ClearTransientState).
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, Clock::time_point>> g_wants;   // follower -> base -> held at
        std::unordered_map<RE::FormID, std::vector<Op>>                                g_ops;
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>                 g_blocked;  // a store / migration that did not land
        std::unordered_map<std::uint64_t, Miss>                                         g_miss;     // (follower, base) retrieval misses
        std::unordered_map<RE::FormID, Clock::time_point>                               g_nextTick, g_nextRetrieve;
        std::unordered_map<RE::FormID, std::uint8_t>                                    g_lastClass;
        std::unordered_map<RE::FormID, Clock::time_point>                               g_cellCheckedAt;   // cell -> last walk
        std::unordered_map<std::uint64_t, Clock::time_point>                            g_logNext;  // (follower, base) decision lines
        bool g_marginOffLogged = false;

        std::uint64_t Key(RE::FormID a_a, RE::FormID a_b) { return (static_cast<std::uint64_t>(a_a) << 32) | a_b; }

        bool Robes() { return Config::g_mageWearRobes.load() && !Config::g_dollsMode.load(); }

        const char* NameOf(const RE::TESForm* a_f) {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return n && *n ? n : "?";
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

        // The STORE-side rank inside a storage bucket: the keep set's weapon ranks (SwapUp.cpp ComputeKeepSet), ArmorScore for a
        // shield or rated armor (the skill-led judge), gold value for clothing.
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

        // The KEEP SET's own key and rank for a piece (a mirror of SwapUp.cpp ComputeKeepSet, so a retrieved piece is exactly
        // the one the keep set then holds; a stored piece that would not be kept is never brought back to be sold).
        struct KeepCtx {
            WeaponRoles  roles;
            ArmorPref    pref;
            bool         mage = false;         // useMageApparel
            std::uint8_t top2 = 0;
            bool         strict = false, villain = false;
        };
        struct KeepRank { bool ok = false; int key = -1; float primary = 0.0f, secondary = 0.0f; };
        KeepRank KeepRankOf(RE::Actor* a_actor, const KeepCtx& a_k, RE::TESBoundObject* a_obj) {
            KeepRank r;
            if (auto* w = a_obj->As<RE::TESObjectWEAP>()) {
                if (IsCreatureWeapon(w)) return r;
                using WT = RE::WEAPON_TYPE;
                switch (w->GetWeaponType()) {
                    case WT::kBow:            r.key = 3; r.primary = static_cast<float>(w->GetAttackDamage()); break;
                    case WT::kCrossbow:       r.key = 4; r.primary = static_cast<float>(w->GetAttackDamage()); break;
                    case WT::kStaff:          r.key = 5; r.primary = static_cast<float>(std::clamp<std::int32_t>(w->GetGoldValue(), 0, 0xFFFF)); break;
                    case WT::kTwoHandSword:
                    case WT::kTwoHandAxe:     r.key = 2; r.primary = WeaponScore(a_k.roles, w); break;
                    case WT::kHandToHandMelee: return r;
                    default:                  r.key = 1; r.primary = WeaponScore(a_k.roles, w); break;
                }
                r.ok = true;
                return r;
            }
            auto* ar = a_obj->As<RE::TESObjectARMO>();
            if (!ar || ArmorWontRender(a_actor, ar)) return r;
            if (a_k.mage) {
                const int cs = MageClothingSlot(ar);
                if (cs < 0) return r;
                int t = 0; std::int32_t m = 0;
                if (ar->GetArmorRating() > 0.0f) { t = 0; m = std::max<std::int32_t>(ar->GetGoldValue(), 0); }
                else if (!MageApparelBuyKey(ar, a_k.top2, a_k.strict, a_k.villain, t, m)) return r;
                r.ok = true; r.key = 100 + cs; r.primary = static_cast<float>(t); r.secondary = static_cast<float>(m);
                return r;
            }
            if (ar->GetArmorRating() <= 0.0f) return r;   // a non-mage's clothing: nothing wears it
            int ls = ArmorSlot(ar);
            if (ls < 0 && ar->IsShield()) ls = 4;
            if (ls < 0) return r;
            r.ok = true; r.key = 10 + ls;
            r.primary   = ArmorScore(a_k.pref, ar);
            r.secondary = static_cast<float>(std::max<std::int32_t>(ar->GetGoldValue(), 0));
            return r;
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

        // ── chests ────────────────────────────────────────────────────────────────────────────────────────────────
        bool IsPlayerOwnerId(RE::FormID a_id) { return a_id == 0x00000007 || a_id == 0x00000DB1; }   // Player, PlayerFaction (Skyrim.esm)

        // The player's home: a cell owned by the Player / PlayerFaction, or an UNOWNED cell whose location chain carries
        // LocTypePlayerHouse (000FC1A3) -- RefInPlayerStorage's two halves (LootTake.cpp), with an owner who is someone else
        // ruling the keyword out. Base-game forms only.
        bool IsPlayerHomeCell(RE::TESObjectCELL* a_cell, RE::Actor* a_in) {
            if (auto* owner = a_cell->GetOwner()) return IsPlayerOwnerId(owner->GetFormID());
            static RE::BGSKeyword* kwPlayerHouse = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000FC1A3);
            if (!kwPlayerHouse) return false;
            int guard = 0;
            for (auto* loc = a_in->GetCurrentLocation(); loc && guard < 8; loc = loc->parentLoc, ++guard)
                if (loc->HasKeyword(kwPlayerHouse)) return true;
            return false;
        }

        // A chest still usable (the live re-check at every use; the pick's own tests are in LearnFrom).
        RE::TESObjectREFR* UsableChest(RE::FormID a_chest) {
            auto* ref = a_chest ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_chest) : nullptr;
            if (!ref || !ref->IsPersistent() || ref->IsDisabled() || ref->IsDeleted() || ref->IsMarkedForDeletion()) return nullptr;
            auto* base = ref->GetBaseObject();
            return base && base->As<RE::TESObjectCONT>() ? ref : nullptr;
        }

        // Where a NEW piece goes: the first usable MAIN home chest (the first home learned is the main one), else the first
        // usable INTERIM chest. 0 = no safe storage at all (no storing).
        Home TargetHome() {
            std::vector<Home> homes;
            { std::scoped_lock lk(g_mx); homes = g_table.homes; }
            for (const auto kind : { Home::kMain, Home::kInterim })
                for (const auto& h : homes)
                    if (h.kind == kind && UsableChest(h.chest)) return h;
            return {};
        }
        std::uint8_t HomeKindOf(RE::FormID a_chest) {
            std::scoped_lock lk(g_mx);
            for (const auto& h : g_table.homes)
                if (h.chest == a_chest) return h.kind;
            return 0;
        }

        // Learn a chest from the cell the follower stands in (WORKER; a cell at most once per kCellRecheck). The follower's OWN attached
        // interior cell is walked (the Economy / LootScan worker walk; collect-then-act, #2: ids inside the walk, reads after).
        void LearnFrom(RE::Actor* a_follower, Clock::time_point a_now) {
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached() || !cell->IsInteriorCell()) return;
            const RE::FormID cid = cell->GetFormID();
            const bool home = IsPlayerHomeCell(cell, a_follower);
            bool haveMain = false;
            std::vector<Home> known;
            {
                std::scoped_lock lk(g_mx);
                auto [it, fresh] = g_cellCheckedAt.try_emplace(cid, a_now);
                if (!fresh) {
                    if (a_now - it->second < kCellRecheck) return;
                    it->second = a_now;
                }
                known = g_table.homes;
            }
            for (const auto& h : known) {
                if (h.cell == cid && (!home || h.kind == Home::kMain) && UsableChest(h.chest)) return;   // this cell already serves
                if (h.kind == Home::kMain) haveMain = true;
            }
            if (known.size() >= kMaxHomes) {
                spdlog::warn("[storage] {} storage chests known already (the record's bound): cell {:08X} is not learned", kMaxHomes, cid);
                return;
            }
            if (!home && haveMain) return;   // the main home is known: interim storage is no longer needed

            std::vector<RE::FormID> ids;
            cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
                if (base && base->As<RE::TESObjectCONT>()) ids.push_back(a_ref->GetFormID());
                return RE::BSContainer::ForEachResult::kContinue;
            });

            struct Why { int notPersistent = 0, respawns = 0, locked = 0, disabled = 0, linked = 0, service = 0, ownedOther = 0, notPlayers = 0; } why;
            RE::FormID pick = 0;
            for (const RE::FormID id : ids) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                auto* cont = ref && ref->GetBaseObject() ? ref->GetBaseObject()->As<RE::TESObjectCONT>() : nullptr;
                if (!cont) continue;
                if (ref->IsDisabled() || ref->IsDeleted() || ref->IsMarkedForDeletion()) { ++why.disabled; continue; }
                // From anywhere = the chest must stay in memory while its cell is unloaded: a PERSISTENT ref. (Evidence, 2026-11-13,
                // Skyrim.esm: the vanilla city houses' PlayerHouseChest refs are TEMPORARY, so those homes have no usable chest.)
                if (!ref->IsPersistent()) { ++why.notPersistent; continue; }
                if (cont->data.flags.all(RE::CONT_DATA::Flag::kRespawn)) { ++why.respawns; continue; }   // it would empty itself
                if (ref->IsLocked()) { ++why.locked; continue; }
                // A ref with linked refs is part of a scripted setup (the vanilla PlayerBookShelfContainer: 19 XLKR + a script that
                // hands every non-book back to the player). Never a storage chest.
                if (ref->extraList.HasType(RE::ExtraDataType::kLinkedRef)) { ++why.linked; continue; }
                if (IsLOTDDropOff(ref) || IsFactionServiceContainer(ref)) { ++why.service; continue; }
                auto*            owner = ref->GetOwner();
                const RE::FormID oid   = owner ? owner->GetFormID() : 0;
                if (oid && !IsPlayerOwnerId(oid)) { ++why.ownedOther; continue; }
                // Interim (not a home): only a container the player OWNS is safe storage (an unowned public container can be
                // emptied by a quest script or claimed by its cell's owner later).
                if (!home && !IsPlayerOwnerId(oid)) { ++why.notPlayers; continue; }
                if (!pick || id < pick) pick = id;   // deterministic: the lowest FormID (no chest inventory is read)
            }
            const auto why_s = std::format("{} container(s): {} not persistent, {} respawn, {} locked, {} disabled, {} scripted (linked refs), "
                                           "{} service / drop-off, {} owned by another{}",
                                           ids.size(), why.notPersistent, why.respawns, why.locked, why.disabled, why.linked, why.service,
                                           why.ownedOther, home ? std::string{} : std::format(", {} not the player's", why.notPlayers));
            if (!pick) {
                if (home)
                    spdlog::warn("[storage] no usable chest in the player's home cell {:08X} ('{}'): {} -- a temporary container vanishes "
                                 "when its cell unloads, so storing from anywhere needs a persistent one",
                                 cid, NameOf(cell), why_s);
                return;
            }
            const auto* town = LocationTypes::TownAnchor(a_follower->GetCurrentLocation());
            Home h{ pick, cid, town ? town->GetFormID() : 0, home ? Home::kMain : Home::kInterim };
            {
                std::scoped_lock lk(g_mx);
                for (auto& e : g_table.homes)
                    if (e.chest == pick) {
                        if (home && e.kind != Home::kMain) {   // an interim chest whose cell became his home: promote it
                            e.kind = Home::kMain;
                            spdlog::info("[storage] interim chest {:08X} is in the player's home now: promoted to the main home", pick);
                        }
                        return;
                    }
                if (home && !haveMain) {
                    // the first main home goes FIRST: it is the preferred target from now on
                    g_table.homes.insert(g_table.homes.begin(), h);
                } else {
                    g_table.homes.push_back(h);
                }
            }
            auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(pick);
            spdlog::info("[storage] {} chest CHOSEN: {:08X} '{}' in cell {:08X} '{}' (town {:08X}) -- {}{}",
                         home ? "HOME" : "INTERIM (safe, player-owned)", pick, chest ? NameOf(chest->GetBaseObject()) : "?", cid, NameOf(cell),
                         h.town, why_s, home && !haveMain ? " -- the main home: every interim piece moves here" : "");
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

        // Drop every OTHER entry of a_base's bucket (the superseded piece is released to the player, never sold). Called under g_mx:
        // it does form LOOKUPS only (TESForm::LookupByID, the form map's own reader lock; nothing takes g_mx under that lock).
        void ReleaseSameBucket(std::vector<Item>& a_items, RE::FormID a_keepBase, RE::FormID a_fid) {
            const Piece kp = Classify(RE::TESForm::LookupByID<RE::TESBoundObject>(a_keepBase));
            if (kp.bucket == Bucket::None) return;
            std::erase_if(a_items, [&](const Item& i) {
                if (i.base == a_keepBase) return false;
                auto* o = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
                if (!o || Classify(o).bucket != kp.bucket) return false;
                spdlog::info("[storage] {:08X}: '{}' released to the player in chest {:08X} (a better {} is stored)", a_fid, NameOf(o),
                             i.chest, BucketName(kp.bucket));
                return true;
            });
        }

        // ── codec (one template, two carriers: the SKSE interface and CoSaveBuffer) ────────────────────────────────────
        template <class W>
        void WriteBodyT(W& a_w, const Table& a_t, LoadStats& a_st) {
            std::vector<Home> homes;
            for (const auto& h : a_t.homes) {
                if (homes.size() >= kMaxHomes) break;
                if (!Followers::IsPersistableID(h.chest)) { ++a_st.skippedRuntime; continue; }   // #9
                Home c = h;
                if (!Followers::IsPersistableID(c.cell)) c.cell = 0;
                if (!Followers::IsPersistableID(c.town)) c.town = 0;
                homes.push_back(c);
            }
            std::vector<std::pair<RE::FormID, std::vector<Item>>> fol;
            for (const auto& [fid, items] : a_t.stored) {
                if (fol.size() >= kMaxSavedFollowers) break;
                if (!Followers::IsPersistableID(fid)) { ++a_st.skippedRuntime; continue; }
                std::vector<Item> v;
                for (const auto& i : items) {
                    if (v.size() >= kMaxItemsPerFollower) break;
                    if (!Followers::IsPersistableID(i.base) || !Followers::IsPersistableID(i.chest)) { ++a_st.skippedRuntime; continue; }
                    v.push_back(i);
                }
                if (!v.empty()) fol.emplace_back(fid, std::move(v));
            }
            a_w.WriteRecordData(static_cast<std::uint32_t>(homes.size()));
            for (const auto& h : homes) {
                a_w.WriteRecordData(h.chest);
                a_w.WriteRecordData(h.cell);
                a_w.WriteRecordData(h.town);
                a_w.WriteRecordData(h.kind);
            }
            a_w.WriteRecordData(static_cast<std::uint32_t>(fol.size()));
            for (const auto& [fid, v] : fol) {
                a_w.WriteRecordData(fid);
                a_w.WriteRecordData(static_cast<std::uint32_t>(v.size()));
                for (const auto& i : v) {
                    a_w.WriteRecordData(i.chest);
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
            std::uint32_t homeCount = 0;
            if (!Rd(a_r, homeCount)) { spdlog::error("[cosave] short read on STOR home count -- ABORTING storage load"); return false; }
            if (homeCount > kMaxHomes) { spdlog::error("[cosave] implausible STOR home count {} -- ABORTING storage load", homeCount); return false; }
            for (std::uint32_t i = 0; i < homeCount; ++i) {
                Home h; RE::FormID rc = 0, rcell = 0, rtown = 0; std::uint8_t kind = 0;
                if (!Rd(a_r, rc) || !Rd(a_r, rcell) || !Rd(a_r, rtown) || !Rd(a_r, kind)) {
                    spdlog::error("[cosave] short read in STOR homes -- ABORTING (kept {} chest(s))", a_out.homes.size());
                    return false;
                }
                RE::FormID chest = 0;
                const bool       chestResolved = a_r.ResolveFormID(rc, chest);
                RE::TESObjectREFR* ref  = chestResolved ? RE::TESForm::LookupByID<RE::TESObjectREFR>(chest) : nullptr;
                RE::TESBoundObject* base = ref ? ref->GetBaseObject() : nullptr;
                if (!base || !base->As<RE::TESObjectCONT>() || (kind != Home::kMain && kind != Home::kInterim)) { ++a_st.droppedHomes; continue; }
                if (std::any_of(a_out.homes.begin(), a_out.homes.end(), [&](const Home& e) { return e.chest == chest; })) { ++a_st.droppedHomes; continue; }
                h.chest = chest;
                if (!rcell || !a_r.ResolveFormID(rcell, h.cell)) h.cell = 0;
                if (!rtown || !a_r.ResolveFormID(rtown, h.town)) h.town = 0;
                h.kind = kind;
                a_out.homes.push_back(h);
            }
            std::uint32_t followers = 0;
            if (!Rd(a_r, followers)) { spdlog::error("[cosave] short read on STOR follower count -- ABORTING (kept the chests)"); return false; }
            if (followers > kMaxSavedFollowers) { spdlog::error("[cosave] implausible STOR follower count {} -- ABORTING (kept the chests)", followers); return false; }
            for (std::uint32_t f = 0; f < followers; ++f) {
                RE::FormID rawFid = 0; std::uint32_t n = 0;
                if (!Rd(a_r, rawFid) || !Rd(a_r, n)) { spdlog::error("[cosave] short read in STOR -- ABORTING (kept what was parsed)"); return false; }
                if (n > kMaxItemsPerFollower) { spdlog::error("[cosave] implausible STOR item count {} -- ABORTING (kept what was parsed)", n); return false; }
                RE::FormID fid = 0;
                const bool fidOk = a_r.ResolveFormID(rawFid, fid);   // #8: unresolvable -> his items are read past and DROPPED
                if (!fidOk) ++a_st.droppedFollowers;
                for (std::uint32_t k = 0; k < n; ++k) {
                    RE::FormID rc = 0, rb = 0; std::uint16_t count = 0; std::uint8_t flags = 0;
                    if (!Rd(a_r, rc) || !Rd(a_r, rb) || !Rd(a_r, count) || !Rd(a_r, flags)) {
                        spdlog::error("[cosave] short read in STOR items -- ABORTING (kept what was parsed)");
                        return false;
                    }
                    if (!fidOk) { ++a_st.droppedItems; continue; }
                    RE::FormID chest = 0, base = 0;
                    const bool chestOk = a_r.ResolveFormID(rc, chest) &&
                        std::any_of(a_out.homes.begin(), a_out.homes.end(), [&](const Home& e) { return e.chest == chest; });
                    const bool   baseResolved = a_r.ResolveFormID(rb, base);
                    RE::TESForm* bo = baseResolved ? RE::TESForm::LookupByID(base) : nullptr;
                    if (!chestOk || !bo || !(bo->As<RE::TESObjectWEAP>() || bo->As<RE::TESObjectARMO>()) || count == 0) { ++a_st.droppedItems; continue; }
                    if ((flags & ~Item::kAllFlags) != 0) { ++a_st.clampedFlags; flags &= Item::kAllFlags; }   // #11
                    if (count > 1) { ++a_st.clampedFlags; count = 1; }                                        // #11: one piece per entry
                    auto& v = a_out.stored[fid];
                    if (std::any_of(v.begin(), v.end(), [&](const Item& e) { return e.base == base; })) { ++a_st.droppedItems; continue; }
                    v.push_back(Item{ chest, base, count, flags });
                }
            }
            return true;
        }
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

    StoreOutput DecideStore(const StoreInput& a) {
        StoreOutput o;
        o.useValue  = Pricing::Apply(a.value, a.buyMult);
        o.saleValue = Pricing::Apply(a.value, a.sellMult);
        if (!a.classesOn)                { o.why = "classes off (no storing)"; return o; }
        if (a.bucket == Bucket::None)    { o.why = "not a stored kind"; return o; }
        if (!a.storageReady)             { o.why = "no safe storage known (no home, no player-owned chest)"; return o; }
        const bool weaponish = a.armorKind == 0;
        if (weaponish ? (a.classBit == 0) : false) { o.why = "not a stored kind"; return o; }
        if (weaponish ? (a.currentSet == 0 || (a.classBit & a.currentSet)) : (a.armorKind == a.currentKind)) {
            o.why = "in his class now"; return o;
        }
        if (weaponish ? !(a.classBit & a.plan.weapons) : !(a.armorKind & a.plan.kinds)) {
            o.why = "no mastered or future class of his uses it"; return o;
        }
        if (!(a.rank > a.bestOtherRank || (a.rank == a.bestOtherRank && !a.tieLoses))) {
            o.why = "a better one of its kind is carried or stored"; return o;
        }
        if (!a.singleCopy)               { o.why = "he carries more than one copy (a store could not be told from a sale)"; return o; }
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
        else if (!a.roleKeeps)                                                 why = "his roles do not use it";
        else if (!(a.rank > a.bestCarriedRank || (a.rank == a.bestCarriedRank && a.tieWins)))
                                                                               why = "what he carries is as good";
        else { ok = true; why = a.classChanged ? "class change" : "better than what he carries"; }
        if (a_whyOut) *a_whyOut = why;
        return ok;
    }

    // ── live seats ───────────────────────────────────────────────────────────────────────────────────────────────
    bool Enabled() { return Config::g_offClassStorage.load(); }

    std::int32_t HoldFromSale(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_obj,
                              RE::InventoryEntryData* a_entry, std::int32_t a_count, std::int32_t a_value) {
        if (!Enabled() || !a_follower || !a_obj || a_count <= 0) return 0;
        if (a_entry && a_entry->IsWorn()) return 0;   // a worn force-sell row: storage never takes anything off him
        const RE::FormID fid = a_follower->GetFormID();
        const RE::FormID base = a_obj->GetFormID();
        if (!Followers::IsPersistableID(base)) return 0;   // a runtime (0xFF) form could never be saved (#9)
        const Piece pc = Classify(a_obj);
        if (pc.bucket == Bucket::None) return 0;
        const auto now = Clock::now();
        std::vector<Item> mine;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_ops.find(fid); it != g_ops.end())
                for (const auto& op : it->second)
                    if (op.base == base) return 1;   // a move of it is in flight: never sell it from under the move
            if (const auto it = g_blocked.find(fid); it != g_blocked.end() && it->second.count(base)) return 0;
            if (const auto it = g_table.stored.find(fid); it != g_table.stored.end()) mine = it->second;
        }
        if (pc.armor && Config::g_dollsMode.load()) return 0;   // the player dresses him (dolls mode): his apparel is not ours

        StoreInput in;
        in.bucket = pc.bucket; in.classBit = pc.armor ? 0 : pc.classBit; in.armorKind = pc.armor ? pc.kind : 0;
        const std::uint8_t eq = Classes::Active() ? Classes::GetEquipped(fid) : Classes::kNoClass;
        in.classesOn = eq < Classes::kClassCount;
        if (in.classesOn) {
            in.currentSet  = ClassSet(fid);
            in.currentKind = KindOf(static_cast<Classes::Id>(eq), Robes());
            in.plan        = PlannedFor(eq, Classes::GetMastered(fid), Robes());
        }
        const Home target = TargetHome();
        in.storageReady = target.chest != 0;
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
        if (in.classesOn && in.storageReady) {
            const auto quote = Pricing::Resolve(a_follower->GetHandle(), RE::ActorHandle{}, 0);
            in.buyMult = quote.buy; in.sellMult = quote.sell;
            if (!quote.modelOn && !g_marginOffLogged) {
                g_marginOffLogged = true;   // worker-serial
                spdlog::info("[storage] the barter model is off (bSpeechPricing 0 or a GMST unreadable): buying back costs what selling "
                             "pays, so nothing is worth storing and every off-class piece sells");
            }
        }
        const StoreOutput out = DecideStore(in);
        const bool logIt = DecisionLogDue(fid, base, now);
        if (out.verdict == Verdict::Store) {
            {
                std::scoped_lock lk(g_mx);
                g_wants[fid].try_emplace(base, now);
            }
            if (logIt)
                spdlog::info("[storage] {:08X} '{}': HOLD '{}' ({}) for chest {:08X} ({}) -- planned for {:X}, top of its kind (rank {:.1f} vs {:.1f}), "
                             "use {}g vs sale {}g (margin {}g >= {}g, a placeholder)",
                             fid, NameOf(a_follower), NameOf(a_obj), BucketName(pc.bucket), target.chest,
                             target.kind == Home::kMain ? "home" : "interim", in.plan.classes, in.rank, in.bestOtherRank,
                             out.useValue, out.saleValue, out.useValue - out.saleValue, in.minMargin);
            return 1;
        }
        {
            std::scoped_lock lk(g_mx);
            if (auto it = g_wants.find(fid); it != g_wants.end()) it->second.erase(base);
        }
        if (logIt && in.classesOn)
            spdlog::info("[storage] {:08X} '{}': '{}' ({}) sells -- {} (use {}g vs sale {}g)", fid, NameOf(a_follower), NameOf(a_obj),
                         BucketName(pc.bucket), out.why, out.useValue, out.saleValue);
        return 0;
    }

    void Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now) {
        if (!Enabled() || !a_follower || !a_follower->Is3DLoaded()) return;
        const RE::FormID fid = a_follower->GetFormID();
        {
            std::scoped_lock lk(g_mx);
            auto& n = g_nextTick[fid];
            if (n.time_since_epoch().count() != 0 && a_now < n) return;
            n = a_now + kTickEvery;
        }
        if (APMFBridge::InCombat(a_follower)) return;

        LearnFrom(a_follower, a_now);

        const std::uint8_t eq = Classes::Active() ? Classes::GetEquipped(fid) : Classes::kNoClass;
        bool classChanged = false;
        {
            std::scoped_lock lk(g_mx);
            auto [it, fresh] = g_lastClass.try_emplace(fid, eq);
            if (!fresh && it->second != eq) { classChanged = true; it->second = eq; }
        }

        // 1. SETTLE what a load restored mid-move (flags are only ever set by a save that landed in a move's window).
        std::vector<Item> items;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_table.stored.find(fid); it != g_table.stored.end()) items = it->second;
        }
        for (const auto& i : items) {
            if (!i.flags) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
            const std::int32_t c = CountOf(a_follower, obj);
            std::scoped_lock lk(g_mx);
            auto& v = g_table.stored[fid];
            const auto it = std::find_if(v.begin(), v.end(), [&](const Item& e) { return e.base == i.base; });
            if (it == v.end()) continue;
            if (i.flags & Item::kRetrieving) {
                if (c >= 1) {
                    spdlog::info("[storage] {:08X}: RETRIEVED '{}' (the move was in flight at the save; he carries it now)", fid, NameOf(obj));
                    v.erase(it);
                    continue;
                }
                it->flags &= static_cast<std::uint8_t>(~Item::kRetrieving);
            }
            if (it->flags & Item::kUnverified) {
                if (c == 0) {
                    it->flags &= static_cast<std::uint8_t>(~Item::kUnverified);
                    spdlog::info("[storage] {:08X}: store of '{}' in {:08X} CONFIRMED after the load (he no longer carries it)", fid, NameOf(obj), it->chest);
                    const RE::FormID keep = it->base;
                    ReleaseSameBucket(v, keep, fid);
                } else {
                    spdlog::warn("[storage] {:08X}: store of '{}' NOT confirmed after the load (he still carries {}) -- entry dropped", fid, NameOf(obj), c);
                    v.erase(it);
                }
            }
        }

        // 2. CONFIRM in-flight moves from HIS count (the only count read: never a chest inventory).
        std::vector<Op> ops;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_ops.find(fid); it != g_ops.end()) ops = it->second;
        }
        std::vector<Op> keepOps;
        for (const auto& op : ops) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(op.base);
            const std::int32_t c = CountOf(a_follower, obj);
            const bool timedOut = a_now - op.at > kConfirmWindow;
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(kConfirmWindow).count();
            switch (op.kind) {
                case OpKind::Store:
                case OpKind::MigrateIn:
                    if (c < op.before) {
                        std::scoped_lock lk(g_mx);
                        auto& v = g_table.stored[fid];
                        if (op.kind == OpKind::MigrateIn) {
                            for (auto& e : v) if (e.base == op.base) e.chest = op.chest;
                            if (std::none_of(v.begin(), v.end(), [&](const Item& e) { return e.base == op.base; })) v.push_back(Item{ op.chest, op.base, 1, 0 });
                            spdlog::info("[storage] {:08X}: MOVED '{}' from interim chest {:08X} to the home chest {:08X} (verified: it left his pack)",
                                         fid, NameOf(obj), op.fromChest, op.chest);
                        } else {
                            std::erase_if(v, [&](const Item& e) { return e.base == op.base; });
                            v.push_back(Item{ op.chest, op.base, 1, 0 });
                            spdlog::info("[storage] {:08X} '{}': STORED '{}' in chest {:08X} (verified: it left his pack)", fid, NameOf(a_follower),
                                         NameOf(obj), op.chest);
                        }
                        ReleaseSameBucket(v, op.base, fid);
                        if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(op.base);
                    } else if (timedOut) {
                        std::scoped_lock lk(g_mx);
                        g_blocked[fid].insert(op.base);
                        if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(op.base);
                        if (op.kind == OpKind::MigrateIn) {
                            auto& v = g_table.stored[fid];
                            std::erase_if(v, [&](const Item& e) { return e.base == op.base; });
                            spdlog::warn("[storage] {:08X}: '{}' did NOT reach the home chest {:08X} within {} s -- he still carries it; entry dropped, "
                                         "no more storing of it this session (it goes back to the sale path)", fid, NameOf(obj), op.chest, secs);
                        } else {
                            spdlog::warn("[storage] {:08X}: '{}' did NOT leave his pack within {} s (chest {:08X}) -- not recorded, no more storing "
                                         "of it this session (it sells as before)", fid, NameOf(obj), secs, op.chest);
                        }
                    } else {
                        keepOps.push_back(op);
                    }
                    break;
                case OpKind::Retrieve:
                case OpKind::MigrateOut:
                    if (c > op.before) {
                        std::scoped_lock lk(g_mx);
                        g_miss.erase(Key(fid, op.base));
                        if (op.kind == OpKind::Retrieve) {
                            std::erase_if(g_table.stored[fid], [&](const Item& e) { return e.base == op.base; });
                            spdlog::info("[storage] {:08X} '{}': RETRIEVED '{}' from chest {:08X}", fid, NameOf(a_follower), NameOf(obj), op.chest);
                        } else {
                            keepOps.push_back(op);   // the second hop is dispatched below, outside the lock
                        }
                    } else if (timedOut) {
                        std::scoped_lock lk(g_mx);
                        auto& m = g_miss[Key(fid, op.base)];
                        ++m.misses;
                        if (m.misses >= kMaxRetrieveMisses) {
                            auto& v = g_table.stored[fid];
                            std::erase_if(v, [&](const Item& e) { return e.base == op.base; });
                            g_miss.erase(Key(fid, op.base));
                            spdlog::warn("[storage] {:08X}: '{}' did not arrive from chest {:08X} ({} tries) -- it is no longer in the chest (the "
                                         "player took it?); entry dropped", fid, NameOf(obj), op.chest, kMaxRetrieveMisses);
                        } else {
                            m.retryAt = a_now + kRetrieveBackoff;
                            spdlog::warn("[storage] {:08X}: '{}' did not arrive from chest {:08X} within {} s (try {}/{}) -- retry in 5 min",
                                         fid, NameOf(obj), op.chest, secs, m.misses, kMaxRetrieveMisses);
                        }
                    } else {
                        keepOps.push_back(op);
                    }
                    break;
            }
        }
        // The migration's second hop: the piece is in his pack now (verified); send it to the home chest.
        for (auto& op : keepOps) {
            if (op.kind != OpKind::MigrateOut) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(op.base);
            const std::int32_t c = CountOf(a_follower, obj);
            if (c <= op.before) continue;   // still waiting for the first hop
            const Home home = TargetHome();
            auto* chest = home.kind == Home::kMain ? UsableChest(home.chest) : nullptr;
            if (chest && Papyrus::DispatchMoveItem(a_follower, obj, 1, chest)) {
                op = Op{ OpKind::MigrateIn, op.base, home.chest, op.chest, c, a_now };
            } else {
                std::scoped_lock lk(g_mx);
                auto& v = g_table.stored[fid];
                std::erase_if(v, [&](const Item& e) { return e.base == op.base; });
                spdlog::warn("[storage] {:08X}: '{}' came out of interim chest {:08X} but the move to the home chest could not be dispatched -- "
                             "he carries it; entry dropped (the sale path judges it again)", fid, NameOf(obj), op.chest);
                op.base = 0;   // drop this op
            }
        }
        std::erase_if(keepOps, [](const Op& o) { return o.base == 0; });

        // 3. DISPATCH wanted stores (the sale path held them).
        std::vector<RE::FormID> wants;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_wants.find(fid); it != g_wants.end())
                for (const auto& [b, at] : it->second)
                    if (std::none_of(keepOps.begin(), keepOps.end(), [&](const Op& o) { return o.base == b; })) wants.push_back(b);
        }
        for (const RE::FormID b : wants) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(b);
            const std::int32_t c = CountOf(a_follower, obj);
            const Home target = TargetHome();
            auto* chest = UsableChest(target.chest);
            const char* drop = nullptr;
            if (c != 1)                         drop = c == 0 ? "he no longer carries it" : "he carries more than one copy now";
            else if (IsWornNow(a_follower, obj)) drop = "he wears it now";
            else if (!chest)                     drop = "no usable chest any more";
            if (drop) {
                std::scoped_lock lk(g_mx);
                if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(b);
                spdlog::info("[storage] {:08X}: '{}' not stored -- {}", fid, NameOf(obj), drop);
                continue;
            }
            if (Papyrus::DispatchMoveItem(a_follower, obj, 1, chest)) {
                keepOps.push_back(Op{ OpKind::Store, b, target.chest, 0, c, a_now });
            } else {
                std::scoped_lock lk(g_mx);
                if (auto w = g_wants.find(fid); w != g_wants.end()) w->second.erase(b);
                g_blocked[fid].insert(b);
                spdlog::warn("[storage] {:08X}: the move of '{}' to chest {:08X} could not be dispatched (VM / handle) -- not stored, it sells as before",
                             fid, NameOf(obj), target.chest);
            }
        }

        // 4. RETRIEVE (one per pass, its own cadence) or MIGRATE an interim piece to the main home.
        bool due = false;
        {
            std::scoped_lock lk(g_mx);
            auto& n = g_nextRetrieve[fid];
            if (n.time_since_epoch().count() == 0 || a_now >= n) { due = true; n = a_now + kRetrieveEvery; }
        }
        if (due && keepOps.empty()) {
            {
                std::scoped_lock lk(g_mx);
                items.clear();
                if (const auto it = g_table.stored.find(fid); it != g_table.stored.end()) items = it->second;
            }
            if (!items.empty()) {
                KeepCtx k;
                k.roles   = ComputeWeaponRoles(a_follower, a_state);
                k.pref    = ArmorPrefFor(a_follower);
                k.mage    = UsesMageClothing(a_state, a_follower) && Robes();
                k.top2    = k.mage ? TopTwoSchoolMask(a_follower) : 0;
                k.strict  = Config::g_mageApparelStrictSchool.load();
                k.villain = k.mage && IsNecromancerFollower(a_state);
                const std::uint16_t curSet  = ClassSet(fid);
                const std::uint8_t  curKind = CurrentKind(a_follower, a_state, eq);
                const Home main = TargetHome();
                // his carried keep-set ranks, per keep key
                std::unordered_map<int, std::pair<float, float>> carried;
                for (auto& [o, data] : a_follower->GetInventory([](RE::TESBoundObject& b) { return b.IsWeapon() || b.IsArmor(); })) {
                    if (!o || data.first <= 0) continue;
                    const KeepRank kr = KeepRankOf(a_follower, k, o);
                    if (!kr.ok) continue;
                    auto [it, fresh] = carried.try_emplace(kr.key, kr.primary, kr.secondary);
                    if (!fresh && (kr.primary > it->second.first || (kr.primary == it->second.first && kr.secondary > it->second.second)))
                        it->second = { kr.primary, kr.secondary };
                }
                for (const auto& i : items) {
                    if (i.flags) continue;
                    {
                        std::scoped_lock lk(g_mx);
                        if (const auto m = g_miss.find(Key(fid, i.base)); m != g_miss.end() && a_now < m->second.retryAt) continue;
                    }
                    auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(i.base);
                    auto* chest = UsableChest(i.chest);
                    if (!obj || !chest) {
                        std::scoped_lock lk(g_mx);
                        auto& v = g_table.stored[fid];
                        std::erase_if(v, [&](const Item& e) { return e.base == i.base; });
                        spdlog::warn("[storage] {:08X}: entry {:08X} dropped -- {} (the piece, if any, stays there as the player's)", fid, i.base,
                                     !obj ? "its form no longer resolves" : "its chest is no longer a usable persistent container");
                        continue;
                    }
                    if (CountOf(a_follower, obj) != 0) continue;   // a copy in his pack would make the confirm ambiguous
                    const Piece pc = Classify(obj);
                    RetrieveInput r;
                    r.bucket = pc.bucket; r.classBit = pc.armor ? 0 : pc.classBit; r.armorKind = pc.armor ? pc.kind : 0;
                    r.currentSet = curSet; r.currentKind = curKind; r.classChanged = classChanged;
                    switch (pc.bucket) {
                        case Bucket::Dagger: case Bucket::OneHand: r.roleKeeps = k.roles.melee == WepClass::OneHand; break;
                        case Bucket::TwoHand:                      r.roleKeeps = k.roles.melee == WepClass::TwoHand; break;
                        case Bucket::Bow: case Bucket::Crossbow:
                            r.roleKeeps = k.roles.doRanged && RangedInFamily(obj->As<RE::TESObjectWEAP>(), k.roles.wantCrossbow); break;
                        case Bucket::Staff:                        r.roleKeeps = true; break;
                        case Bucket::Shield:                       r.roleKeeps = k.roles.melee == WepClass::OneHand && k.roles.offHand != 2; break;
                        default:                                   r.roleKeeps = true; break;
                    }
                    const KeepRank kr = KeepRankOf(a_follower, k, obj);
                    if (!kr.ok) r.roleKeeps = false;   // the keep set would not hold it: never brought back to be sold
                    r.rank = kr.primary;
                    if (const auto c = carried.find(kr.key); kr.ok && c != carried.end()) {
                        r.bestCarriedRank = c->second.first;
                        r.tieWins = kr.secondary > c->second.second;
                    }
                    const char* why = "";
                    if (DecideRetrieve(r, &why)) {
                        if (Papyrus::DispatchMoveItem(chest, obj, 1, a_follower)) {
                            keepOps.push_back(Op{ OpKind::Retrieve, i.base, i.chest, 0, 0, a_now });
                            spdlog::info("[storage] {:08X} '{}': RETRIEVE '{}' from chest {:08X} -- {} (rank {:.1f} vs {:.1f})", fid, NameOf(a_follower),
                                         NameOf(obj), i.chest, why, r.rank, r.bestCarriedRank);
                        } else {
                            spdlog::warn("[storage] {:08X}: the retrieval of '{}' from chest {:08X} could not be dispatched (VM / handle)", fid, NameOf(obj), i.chest);
                        }
                        break;
                    }
                    if (main.kind == Home::kMain && main.chest != i.chest && HomeKindOf(i.chest) == Home::kInterim) {
                        if (Papyrus::DispatchMoveItem(chest, obj, 1, a_follower)) {
                            keepOps.push_back(Op{ OpKind::MigrateOut, i.base, i.chest, 0, 0, a_now });
                            spdlog::info("[storage] {:08X}: MOVING '{}' from interim chest {:08X} to the home chest {:08X} (through his pack, each hop verified)",
                                         fid, NameOf(obj), i.chest, main.chest);
                        } else {
                            spdlog::warn("[storage] {:08X}: the move of '{}' out of interim chest {:08X} could not be dispatched (VM / handle)", fid, NameOf(obj), i.chest);
                        }
                        break;
                    }
                }
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

    std::vector<Home> Homes() {
        std::scoped_lock lk(g_mx);
        return g_table.homes;
    }

    void Forget(RE::FormID a_follower) {
        std::size_t n = 0;
        {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_table.stored.find(a_follower); it != g_table.stored.end()) { n = it->second.size(); g_table.stored.erase(it); }
            g_wants.erase(a_follower);
            g_ops.erase(a_follower);
            g_blocked.erase(a_follower);
            g_lastClass.erase(a_follower);
        }
        if (n) spdlog::info("[storage] {:08X}: {} stored piece(s) released to the player (roster removal)", a_follower, n);
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
        // SNAPSHOT under the lock, write from the copy. A move in flight is written as its entry with a flag, so a load can settle
        // it from his count: a store -> the new entry kUnverified; a retrieval / the first migration hop -> kRetrieving; the second
        // migration hop -> the entry at the home chest, kUnverified.
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
                            if (it == v.end()) v.push_back(Item{ op.chest, op.base, 1, Item::kUnverified });
                            else { it->chest = op.chest; it->flags |= Item::kUnverified; }
                            break;
                        case OpKind::MigrateIn:
                            if (it != v.end()) { it->chest = op.chest; it->flags |= Item::kUnverified; }
                            break;
                        case OpKind::Retrieve:
                        case OpKind::MigrateOut:
                            if (it != v.end()) it->flags |= Item::kRetrieving;
                            break;
                    }
                }
            std::erase_if(snap.stored, [](const auto& kv) { return kv.second.empty(); });
        }
        if (snap.homes.empty() && snap.stored.empty()) return;   // written ONLY when there is something to keep
        if (!a_intfc->OpenRecord(kRecStorage, kStorageVersion)) {
            spdlog::error("[cosave] OpenRecord('STOR') failed -- off-class storage NOT saved (the pieces stay in the chests as the player's)");
            return;
        }
        LoadStats st;
        WriteBodyT(*a_intfc, snap, st);
        std::size_t pieces = 0;
        for (const auto& [f, v] : snap.stored) pieces += v.size();
        spdlog::info("[cosave] saved {} storage chest(s), {} stored piece(s) for {} follower(s), schema v{}{}", snap.homes.size(), pieces,
                     snap.stored.size(), kStorageVersion,
                     st.skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) id(s)", st.skippedRuntime) : std::string{});
    }

    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        Table t;
        LoadStats st;
        const bool complete = ReadBodyT(*a_intfc, a_version, t, st);
        std::size_t pieces = 0;
        for (const auto& [f, v] : t.stored) pieces += v.size();
        const std::size_t chests = t.homes.size();
        {
            std::scoped_lock lk(g_mx);
            g_table = std::move(t);
        }
        spdlog::info("[cosave] loaded {} storage chest(s), {} stored piece(s){}; dropped {} chest(s), {} follower(s), {} piece(s), {} clamped field(s)",
                     chests, pieces, complete ? "" : " (record ABORTED, kept what was parsed)", st.droppedHomes, st.droppedFollowers,
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
        g_blocked.clear();
        g_miss.clear();
        g_nextTick.clear();
        g_nextRetrieve.clear();
        g_lastClass.clear();
        g_cellCheckedAt.clear();
        g_logNext.clear();
        g_marginOffLogged = false;
    }
}
