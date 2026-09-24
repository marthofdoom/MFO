#include "MEOBridge.h"
#include "MEO_API.h"
#include "MainThread.h"   // Build A: main-thread refresh of the carried-gem cache

#include <algorithm>
#include <cctype>   // reconcile: case-insensitive gem-name preference match
#include <chrono>   // reconcile stall detector: the 60 s backoff floor
#include <cstring>  // reconcile: MEO's tool-name blocklist (strstr), mirrored
#include <format>   // reconcile: the pass-count suffix on the socket line
#include <limits>   // reconcile: best/weakest gem sentinels
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>   // Build B: per-item gem slots copied out of the lock for UnsocketGem

#include <spdlog/spdlog.h>

namespace MFO::MEOBridge {

    namespace {
        MEO_API::IMEO* g_meo = nullptr;

        struct Pending { RE::FormID fromBase; std::uint16_t fromUid; };
        std::mutex                                   g_mx;
        std::unordered_map<std::uint64_t, Pending>   g_pending;   // (follower<<32 | toBase) -> old item

        std::uint64_t Key(RE::FormID a_follower, RE::FormID a_toBase) {
            return (static_cast<std::uint64_t>(a_follower) << 32) | a_toBase;
        }

        // Per-follower carried-gemmed item cache. followerID -> ((base<<16|uid) ->
        // set of filled gem SLOTS). Slots are needed to queue UnsocketGem per slot
        // (ungem-then-sell, MEO v3). Written by RefreshCarriedGems (MAIN thread),
        // read by IsCarriedGemmed / UnsocketItemGems (worker); all under g_mx.
        std::unordered_map<RE::FormID,
            std::unordered_map<std::uint64_t, std::unordered_set<std::uint8_t>>> g_carriedGems;

        // De-dup for the async unsocket: (fid -> set of (base<<16|uid)) we've already
        // queued an extract for, so a pending unsocket isn't re-queued every scan.
        // Pruned in RefreshCarriedGems once the item leaves the gemmed cache.
        std::unordered_map<RE::FormID, std::unordered_set<std::uint64_t>> g_extractRequested;

        std::uint64_t GemKey(RE::FormID a_base, std::uint16_t a_uid) {
            return (static_cast<std::uint64_t>(a_base) << 16) | a_uid;
        }

        bool IsWornXList(RE::ExtraDataList* a_xl) {
            return a_xl && (a_xl->HasType(RE::ExtraDataType::kWorn) ||
                            a_xl->HasType(RE::ExtraDataType::kWornLeft));
        }

        // Flush a pending gem move once the destination item is actually WORN --
        // MEO mints the dest uid on the worn xList, so the move must wait for the
        // equip to land (the loot tick only queued the equip). Main thread.
        class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent> {
        public:
            static EquipSink* GetSingleton() { static EquipSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_ev || !a_ev->equipped || !a_ev->actor || !g_meo)
                    return RE::BSEventNotifyControl::kContinue;

                const RE::FormID actorID = a_ev->actor->GetFormID();
                Pending p{};
                {
                    std::scoped_lock lk(g_mx);
                    auto it = g_pending.find(Key(actorID, a_ev->baseObject));
                    if (it == g_pending.end()) return RE::BSEventNotifyControl::kContinue;
                    p = it->second;
                    g_pending.erase(it);
                }
                auto* actor = a_ev->actor->As<RE::Actor>();
                if (!actor) return RE::BSEventNotifyControl::kContinue;

                // Event carries the equipped instance's uid; pass it (0 -> MEO
                // mints on the now-worn xList). MoveGems self-queues to main. The
                // bool means ACCEPTED FOR QUEUING only (MEO_API.h) -- a false is
                // MEO refusing the request outright, never a silent no-op.
                const bool queued = g_meo->MoveGems(actor, p.fromBase, p.fromUid, a_ev->baseObject, a_ev->uniqueID);
                if (queued)
                    spdlog::info("[meo] gem move {:08X}/{} -> {:08X}/{} on {:08X} (queued)",
                                 p.fromBase, p.fromUid, a_ev->baseObject, a_ev->uniqueID, actorID);
                else
                    spdlog::warn("[meo] gem move {:08X}/{} -> {:08X}/{} on {:08X} REFUSED by MEO (MoveGems returned false) -- see MEO.log",
                                 p.fromBase, p.fromUid, a_ev->baseObject, a_ev->uniqueID, actorID);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Acquire() {
        MEO_API::InterfaceRequest req{ MEO_API::kABIVersion, nullptr };
        SKSE::GetMessagingInterface()->Dispatch(MEO_API::kMessage_RequestInterface,
                                                &req, sizeof(req), MEO_API::kPluginName);
        g_meo = (req.out && req.out->Version() >= 1) ? req.out : nullptr;
        spdlog::info("[meo] interface {}",
                     g_meo ? "acquired -- follower gem transfer enabled"
                           : "absent -- gem transfer off (MEO not in load order)");
    }

    bool Available() { return g_meo != nullptr; }

    void RegisterSink() {
        if (auto* src = RE::ScriptEventSourceHolder::GetSingleton())
            src->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
    }

    std::uint16_t WornUid(RE::Actor* a_actor, RE::TESBoundObject* a_base) {
        if (!a_actor || !a_base) return 0;
        auto* ch = a_actor->GetInventoryChanges();
        if (!ch || !ch->entryList) return 0;
        for (auto* e : *ch->entryList) {
            if (!e || e->object != a_base || !e->extraLists) continue;
            for (auto* xl : *e->extraLists) {
                if (!IsWornXList(xl)) continue;
                if (auto* uid = xl->GetByType<RE::ExtraUniqueID>()) return uid->uniqueID;
            }
        }
        return 0;
    }

    // ── Build A: accurate carried-gem sell-skip (ABI v2) ─────────────────────
    std::uint32_t CarriedGems(RE::Actor* a_actor, MEO_API::GemInfo* a_out, std::uint32_t a_max) {
        if (!g_meo || g_meo->Version() < 2 || !a_actor || !a_out || a_max == 0) return 0;
        return g_meo->GetActorGemsCarried(a_actor, a_out, a_max);   // MAIN-THREAD ONLY
    }

    void RefreshCarriedGems(RE::Actor* a_actor) {
        if (!a_actor) return;
        const RE::FormID fid = a_actor->GetFormID();
        std::unordered_map<std::uint64_t, std::unordered_set<std::uint8_t>> gems;   // GemKey -> filled slots
        constexpr std::uint32_t kMax = 64;
        MEO_API::GemInfo buf[kMax];
        const std::uint32_t n = std::min(CarriedGems(a_actor, buf, kMax), kMax);   // 0 if MEO < v2
        for (std::uint32_t i = 0; i < n; ++i)
            if (buf[i].itemBase != 0 && buf[i].itemUid != 0)
                gems[GemKey(buf[i].itemBase, buf[i].itemUid)].insert(buf[i].slot);
        std::scoped_lock lk(g_mx);
        // Drop extract-requests for items that are no longer gemmed (extract landed
        // -> a future re-gem may re-request); prune before replacing the cache.
        if (auto rq = g_extractRequested.find(fid); rq != g_extractRequested.end()) {
            std::erase_if(rq->second, [&](std::uint64_t k) { return !gems.contains(k); });
            if (rq->second.empty()) g_extractRequested.erase(rq);
        }
        g_carriedGems[fid] = std::move(gems);   // replace (empty -> nothing gemmed)
    }

    void RequestCarriedGemRefresh(RE::Actor* a_follower) {
        if (!a_follower || !g_meo || g_meo->Version() < 2) return;   // no v2 -> leave cache empty (correct degrade)
        if (!MainThread::IsInstalled()) { RefreshCarriedGems(a_follower); return; }   // VR: no pump, run direct
        const RE::FormID fid = a_follower->GetFormID();
        MainThread::Post([fid]() {
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(fid)) RefreshCarriedGems(a);
        });
    }

    bool IsCarriedGemmed(RE::FormID a_followerID, RE::FormID a_base, std::uint16_t a_uid) {
        if (a_uid == 0 || a_base == 0) return false;
        std::scoped_lock lk(g_mx);
        auto it = g_carriedGems.find(a_followerID);
        return it != g_carriedGems.end() && it->second.contains(GemKey(a_base, a_uid));
    }

    bool CacheWarmed(RE::FormID a_followerID) {
        std::scoped_lock lk(g_mx);
        return g_carriedGems.contains(a_followerID);
    }

    bool CarriedGemsSupported() { return g_meo && g_meo->Version() >= 2; }

    bool CarriedGemUnsocketSupported() { return g_meo && g_meo->Version() >= 3; }

    void UnsocketItemGems(RE::Actor* a_actor, RE::FormID a_base, std::uint16_t a_uid) {
        if (!g_meo || g_meo->Version() < 3 || !a_actor || a_base == 0 || a_uid == 0) return;
        const RE::FormID     fid = a_actor->GetFormID();
        const std::uint64_t  key = GemKey(a_base, a_uid);
        std::vector<std::uint8_t> slots;
        {
            std::scoped_lock lk(g_mx);
            auto& req = g_extractRequested[fid];
            if (req.contains(key)) return;               // already queued this item's extract
            auto it = g_carriedGems.find(fid);
            if (it == g_carriedGems.end()) return;
            auto gi = it->second.find(key);
            if (gi == it->second.end() || gi->second.empty()) return;
            slots.assign(gi->second.begin(), gi->second.end());   // copy out of the lock
            req.insert(key);                             // mark requested (pruned when it leaves the cache)
        }
        // UnsocketGem queues to the main thread -- safe from any thread. One per gem.
        // The bool means accepted for queuing (MEO_API.h); a false is a refusal.
        for (const std::uint8_t s : slots)
            if (!g_meo->UnsocketGem(a_actor, a_base, a_uid, s))
                spdlog::warn("[meo] ungem-then-sell {:08X}/{} slot {} on {:08X} REFUSED by MEO (UnsocketGem returned false) -- see MEO.log",
                             a_base, a_uid, s, fid);
    }

    // ── GEM RECONCILE (ABI v3) — MAIN-THREAD re-socket of the follower's loose gems ──
    namespace {
        // ── RECONCILE STALL DETECTOR (field 2026-09-14) ──────────────────────
        // SocketGem's bool only means "accepted for queuing" (MEO_API.h); the
        // real outcome lands asynchronously and a failure is warned in MEO's OWN
        // log, invisible here. The deck log showed one identical socket request
        // re-issued 192 times at the ~1.2 s reconcile cadence: MFO's "no dedup
        // state -- GetEmptySocketCount reflects landed sockets" assumption
        // held for a request MEO accepts and then FAILS. Per-actor, per
        // request (op, item base, item uid, slot, gem base -- op 0 = SocketGem,
        // op 1 = the tier-2 swap-out UnsocketGem, whose gem base MEO's GemDetail
        // does not carry, so 0): count consecutive passes that re-issue it while
        // GetEmptySocketCount did not MOVE (a socket drops it, an unsocket raises
        // it). On the kStuckPasses-th such pass -- i.e. after kStuckPasses-1
        // accepted issues -- log STUCK once (loud, names the item so MEO.log's
        // [api] SocketGem/UnsocketGem line can be matched) and BACK OFF that
        // key -- until the follower's inventory changes (the loose-gem set or
        // the worn set differs from the pass that stalled) or a 60 s floor
        // (principle 9: sized from the ~1.2 s cadence, ~50 passes, never
        // silently forever). NOT a mask: the request is retried after the
        // backoff and the failure stays in the log. A backed-off socket does
        // NOT reserve its gem (Fable SEV-4 on b3ac577): a later worn item in
        // the same pass may take it.
        struct StuckKey {
            std::uint8_t op; RE::FormID base; std::uint16_t uid; std::uint8_t slot; RE::FormID gemBase;
            bool operator==(const StuckKey&) const = default;
        };
        struct StuckKeyHash {
            std::size_t operator()(const StuckKey& k) const noexcept {
                std::uint64_t h = (static_cast<std::uint64_t>(k.base) << 32) ^ k.gemBase;
                h ^= (static_cast<std::uint64_t>(k.uid) << 8) ^ k.slot ^ (static_cast<std::uint64_t>(k.op) << 24);
                return std::hash<std::uint64_t>{}(h);
            }
        };
        struct StuckState {
            int   emptyAtIssue = -1;   // GetEmptySocketCount when the request was last issued
            int   passes       = 0;    // consecutive passes that saw it with the count unchanged (issues + the STUCK pass)
            bool  reported     = false;
            std::chrono::steady_clock::time_point backoffUntil{};   // zero = not backed off
            std::uint64_t inventoryKey = 0;   // the follower's loose+worn fingerprint when the stall was declared
        };
        constexpr auto kStuckBackoff = std::chrono::seconds(60);
        constexpr int  kStuckPasses  = 3;   // STUCK is declared on the 3rd consecutive pass, BEFORE issuing: 2 accepted
                                            // issues (~2.4 s at the ~1.2 s cadence) with the empty count unchanged
        // MAIN THREAD ONLY (ReconcileLooseGems runs there; VR runs it inline from the
        // worker, where nothing else touches this map) -- no lock, like the pass itself.
        std::unordered_map<RE::FormID, std::unordered_map<StuckKey, StuckState, StuckKeyHash>> g_stuck;

        // Case-insensitive substring test over a small POD char buffer (gid/name <=64).
        bool ContainsCI(const char* a_hay, const char* a_needle) {
            if (!a_hay || !a_needle || !*a_needle) return false;
            for (const char* h = a_hay; *h; ++h) {
                const char* a = h; const char* b = a_needle;
                while (*a && *b) {
                    const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
                    const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
                    if (ca != cb) break;
                    ++a; ++b;
                }
                if (!*b) return true;
            }
            return false;
        }

        const char* SchoolWord(std::uint32_t a_av) {
            switch (static_cast<RE::ActorValue>(a_av)) {
            case RE::ActorValue::kAlteration:  return "alteration";
            case RE::ActorValue::kConjuration: return "conjuration";
            case RE::ActorValue::kDestruction: return "destruction";
            case RE::ActorValue::kIllusion:    return "illusion";
            case RE::ActorValue::kRestoration: return "restoration";
            default:                           return "";
            }
        }

        // Class-preference bonus for a gem (effect-aware tier only). HEURISTIC on the
        // gem's gid/name text: MEO's gem catalog is not shared with MFO, so this is a
        // soft ranking and MAGNITUDE is the tie-break. Bigger = more wanted. (Flagged
        // for tuning against MEO's real gid set -- see the build report.)
        int GemBonus(const char* a_gid, const char* a_name, bool a_isArmor, bool a_isSupport,
                     const GemReconcilePrefs& a_p) {
            int b = 0;
            if (a_isSupport) b += 1;   // Focus/Conduit/Echo glue is always useful
            if (a_p.caster) {
                const char* sw = SchoolWord(a_p.school);
                if (*sw && (ContainsCI(a_gid, sw) || ContainsCI(a_name, sw)))            b += 4;
                if (ContainsCI(a_gid, "magick") || ContainsCI(a_name, "magick") ||
                    ContainsCI(a_gid, "spell")  || ContainsCI(a_name, "spell"))          b += 2;
                if (a_isArmor && (ContainsCI(a_gid, "magic") || ContainsCI(a_name, "magic"))) b += 1;
            } else {
                if (ContainsCI(a_gid, "damage") || ContainsCI(a_gid, "health") ||
                    ContainsCI(a_gid, "stamina"))                                        b += 2;
            }
            return b;
        }

        // ── THE WORN SET: MEO's OWN eligibility, mirrored (marth 2026-09-14) ─────
        // "There should never be unequipped gems when there are free spaces on
        // equipped items." That invariant is only meaningful over the items MEO
        // will actually socket, and MEO's API does NOT gate on its eligibility:
        // SocketCapacity (MEO plugin.cpp:1628) returns 1 for ANY armor or weapon,
        // so GetEmptySocketCount reports an empty socket on a base-enchanted ring
        // or a bound sword, and SocketGem then either fails in MEO.log (a STUCK
        // loop here) or stamps a slot MEO's own menu cannot see (the gem becomes
        // unrecoverable -- MEO's shield story, plugin.cpp:2315-2319). So the worn
        // set is filtered by the SAME predicates MEO's menu applies:
        //   IsSocketableArmorBase  (MEO plugin.cpp:2300-2326): not base-enchanted,
        //     playable, no MagicDisallowEnchanting, and worn in one of kHead,
        //     kHair, kBody, kHands, kAmulet, kRing, kCirclet, kFeet, kShield.
        //     (kForearms / kCalves are NOT in MEO's list and are no longer here.)
        //   IsSocketableWeaponBase (MEO plugin.cpp:2270-2298): not base-enchanted,
        //     playable, not unarmed, not bound, no MagicDisallowEnchanting, named,
        //     and not a tool (MEO's own name blocklist -- "no socketed pickaxes").
        // A worn instance carrying a FOREIGN enchant (a player-table enchant on
        // its xList with no MEO socket record) is refused by ApiSocketGem
        // (plugin.cpp:9258 "item already carries a foreign enchant") and, at uid 0,
        // would make MEO's mint path fall through to a DIFFERENT copy; that check
        // needs the item's gem count, so it lives in the item loop below.
        using BipedSlot = RE::BGSBipedObjectForm::BipedObjectSlot;
        constexpr BipedSlot kMeoArmorSlots[] = {
            BipedSlot::kHead, BipedSlot::kHair, BipedSlot::kBody, BipedSlot::kHands, BipedSlot::kAmulet,
            BipedSlot::kRing, BipedSlot::kCirclet, BipedSlot::kFeet, BipedSlot::kShield,
        };
        bool MeoSocketableArmor(const RE::TESObjectARMO* a_armo) {
            if (!a_armo || a_armo->formEnchanting || !a_armo->GetPlayable() ||
                a_armo->HasKeywordString("MagicDisallowEnchanting")) return false;
            for (const auto s : kMeoArmorSlots)          // HasPartOf is .all(): per slot, OR'd (MEO's NB)
                if (a_armo->HasPartOf(s)) return true;
            return false;
        }
        bool MeoSocketableWeapon(const RE::TESObjectWEAP* a_weap) {
            if (!a_weap || a_weap->formEnchanting || !a_weap->GetPlayable() ||
                a_weap->IsHandToHandMelee() || a_weap->IsBound() ||
                a_weap->HasKeywordString("MagicDisallowEnchanting")) return false;
            const char* n = a_weap->GetName();
            if (!n || !*n) return false;                 // nameless pseudo-weapons (trap/effect bases)
            static constexpr const char* kToolWords[] = { "Pickaxe", "Pick Axe", "Woodcutter", "Wood Axe", "Woodsman" };
            for (const char* w : kToolWords)
                if (std::strstr(n, w)) return false;
            return true;
        }
        // The WORN xList of a_base on a_actor (the one MEO mints/stamps), or null.
        // Same walk as WornUid, but the pass also needs the list itself (its enchant
        // flag, and its identity: an identical dual-wield pair is TWO worn instances
        // of one base -- Fable SEV-4 on 1efa3e3 -- so a weapon is looked up by its
        // HAND: a_hand 0 = right (kWorn), 1 = left (kWornLeft), -1 = either (armor).
        RE::ExtraDataList* WornXList(RE::Actor* a_actor, const RE::TESBoundObject* a_base, int a_hand) {
            auto* ch = a_actor ? a_actor->GetInventoryChanges() : nullptr;
            if (!ch || !ch->entryList) return nullptr;
            for (auto* e : *ch->entryList) {
                if (!e || e->object != a_base || !e->extraLists) continue;
                for (auto* xl : *e->extraLists) {
                    if (!xl) continue;
                    const bool worn = a_hand == 0 ? xl->HasType(RE::ExtraDataType::kWorn)
                                    : a_hand == 1 ? xl->HasType(RE::ExtraDataType::kWornLeft)
                                                  : IsWornXList(xl);
                    if (worn) return xl;
                }
            }
            return nullptr;
        }

        // ── THE DOMAIN RULE (MEO_API.h:112 + ApiSocketGem, MEO plugin.cpp:9235-9280) ──
        // weapon gems -> weapons, armor gems -> armor, EXCEPT an item holding a
        // socketed Conduit accepts an off-domain gem (:9277 ItemHasConduit, :9279).
        // Support gems (Echo/Conduit/Focus) fit ANY item -- but only a DUAL-socket
        // one (:9241 "support gem needs a dual-socket item") that does not already
        // hold a support gem (:9272 "item already holds a support gem"). Before
        // this the pass treated support as fitting everything, so a Focus picked
        // for a single-socket ring was accepted, failed in MEO.log, and went STUCK.
        struct ItemFit { int capacity; bool isArmor; bool hasSupport; bool hasConduit; };
        bool GemFits(const MEO_API::LooseGemInfo& a_g, const ItemFit& a_f) {
            if (a_g.isSupport) return a_f.capacity >= 2 && !a_f.hasSupport;
            return a_g.isArmor == a_f.isArmor || a_f.hasConduit;
        }

        // ── THE LEFTOVER LINE ──────────────────────────────────────────────────
        // After a pass, a loose gem still in stock while ANY worn item still has
        // an empty socket is a broken invariant, and principle 7 says it is never
        // silent: one `[meo] reconcile LEFTOVER` line per (actor, gem, why), then
        // again after the same 60 s floor as STUCK or when the follower's
        // loose/worn fingerprint changes (principle 9). Keys not seen in a pass are
        // forgotten at its end. MAIN THREAD ONLY, like g_stuck.
        enum class LeftoverWhy : std::uint8_t {
            kOffDomain,       // no worn item of its domain has an empty socket
            kStuck,           // every compatible empty socket is backed off for it
            kCapacity,        // compatible sockets existed but other gems filled them this pass
            kRefused,         // MEO refused the SocketGem outright this pass (returned false)
            kDuplicateCopy,   // the only compatible empty sockets are on a uid-0 item with a duplicate copy carried (deferred)
            kMinting,         // the only compatible empty sockets are on a uid-0 item that took its first gem this pass (the rest fill next pass)
            kSupportLimit,    // a support gem: a dual-socket item is open but already holds (or was just given) its one support gem
            kUnclassified,    // a compatible, un-deferred empty socket exists and the gem was neither issued nor stuck -- a bug, never dropped
        };
        const char* LeftoverWord(LeftoverWhy a_w) {
            switch (a_w) {
            case LeftoverWhy::kOffDomain:     return "off-domain (no worn item of its domain has an empty socket)";
            case LeftoverWhy::kStuck:         return "stuck (every compatible empty socket is backed off for it -- see the STUCK line)";
            case LeftoverWhy::kCapacity:      return "capacity (compatible sockets existed but other gems filled them this pass)";
            case LeftoverWhy::kRefused:       return "refused (MEO refused the socket request outright this pass -- see MEO.log)";
            case LeftoverWhy::kDuplicateCopy: return "duplicate-copy (its only compatible empty sockets are on an un-minted item with a second copy carried; deferred until the copy leaves)";
            case LeftoverWhy::kMinting:       return "minting (its only compatible empty sockets are on an un-minted item that took its first gem this pass, or whose twin worn instance took the base's one uid-0 request; the rest fill next pass)";
            case LeftoverWhy::kSupportLimit:  return "support-limit (a dual-socket item is open but already holds its one support gem)";
            default:                          return "unclassified";
            }
        }
        struct LeftoverState { std::chrono::steady_clock::time_point at{}; std::uint64_t inventoryKey = 0; bool seen = false; };
        // actor -> ((gemBase<<8 | why) -> state)
        std::unordered_map<RE::FormID, std::unordered_map<std::uint64_t, LeftoverState>> g_leftover;

        void ReconcileLooseGems(RE::Actor* a_actor, bool a_effectAware, GemReconcilePrefs a_prefs) {
            if (!g_meo || g_meo->Version() < 3 || !a_actor) return;

            // 1) LOOSE gems in the actor's OWN inventory.
            constexpr std::uint32_t kMaxLoose = 64;
            MEO_API::LooseGemInfo loose[kMaxLoose];
            const std::uint32_t nLoose = std::min(g_meo->GetLooseGems(a_actor, loose, kMaxLoose), kMaxLoose);
            if (nLoose == 0) {         // nothing to place -> nothing to reconcile
                g_stuck.erase(a_actor->GetFormID());   // no request recurs with no gems: drop any stall keys
                g_leftover.erase(a_actor->GetFormID());
                return;
            }

            // Remaining stock per loose entry: the async SocketGem won't decrement the
            // real stack until it lands, so track availability locally within the pass.
            std::uint32_t avail[kMaxLoose];
            for (std::uint32_t i = 0; i < nLoose; ++i)
                avail[i] = loose[i].count ? loose[i].count : 1u;

            // 2) The follower's WORN socketable gear (weapons + armor), filtered by
            //    MEO's own eligibility (see the worn-set note above). Worn is the
            //    unambiguous "keeps/wears" set -- never re-socket into to-be-sold junk.
            //    An item is a worn INSTANCE = (base, worn xList): an identical dual-wield
            //    pair is two instances of one base, each with its own sockets (Fable
            //    SEV-4 on 1efa3e3: deduping by base hid the second dagger). A weapon
            //    with no worn xList for its hand is skipped -- MEO could not mint it in
            //    place either (its uid-0 path wants a worn xList, MEO plugin.cpp:9143).
            struct WornItem { RE::FormID base; std::uint16_t uid; bool isArmor; bool xlEnchanted; const RE::ExtraDataList* xl; };
            std::vector<WornItem> items;
            auto addItem = [&](RE::TESBoundObject* a_obj, bool a_isArmor, int a_hand) {
                if (!a_obj) return;
                const RE::FormID base = a_obj->GetFormID();
                auto* xl = WornXList(a_actor, a_obj, a_hand);
                if (!xl) return;
                for (const auto& it : items) if (it.base == base && it.xl == xl) return;   // dedupe (armor covers many biped slots)
                std::uint16_t uid = 0;
                if (auto* x = xl->GetByType<RE::ExtraUniqueID>()) uid = x->uniqueID;
                items.push_back({ base, uid, a_isArmor, xl->HasType(RE::ExtraDataType::kEnchantment), xl });
            };
            for (int hand = 0; hand < 2; ++hand)
                if (auto* eq = a_actor->GetEquippedObject(hand == 1))
                    if (auto* w = eq->As<RE::TESObjectWEAP>(); w && MeoSocketableWeapon(w)) addItem(w, false, hand);
            for (const auto s : kMeoArmorSlots)
                if (auto* w = a_actor->GetWornArmor(s); w && MeoSocketableArmor(w)) addItem(w, true, -1);
            auto wornCopies = [&](RE::FormID b) { std::int32_t n = 0; for (const auto& it : items) if (it.base == b) ++n; return n; };

            // Total inventory count of a base (worn copies included) -- used to defer
            // ambiguous uid-0 SocketGem targeting when a duplicate copy is carried.
            auto invBaseCount = [&](RE::FormID b) -> std::int32_t {
                for (auto& [obj, data] : a_actor->GetInventory())
                    if (obj && obj->GetFormID() == b) return data.first;
                return 0;
            };

            // Stall detector inputs: a fingerprint of this pass's loose-gem set and
            // worn set -- "the follower's inventory changed" lifts a backoff early.
            std::uint64_t inventoryKey = 1469598103934665603ull;   // FNV-1a
            auto fnv = [&](std::uint64_t v) { inventoryKey = (inventoryKey ^ v) * 1099511628211ull; };
            for (std::uint32_t i = 0; i < nLoose; ++i) { fnv(loose[i].gemBase); fnv(loose[i].gemUid); fnv(loose[i].count); }
            for (const auto& it : items) { fnv(it.base); fnv(it.uid); }
            auto& stuckMap = g_stuck[a_actor->GetFormID()];
            const auto now = std::chrono::steady_clock::now();
            // Every key not re-issued this pass is forgotten at the end (a request
            // that stopped recurring is not stuck) -- collect the ones we touched.
            std::vector<StuckKey> touched;
            // THE GATE, shared by SocketGem and the swap-out UnsocketGem. true = issue
            // the request now (st.passes then says which issue this is); false = the
            // key is stuck and backed off, skip it this pass.
            auto stallGate = [&](const StuckKey& a_key, int a_emptyNow, const char* a_api,
                                 const char* a_gemName) -> bool {
                auto& st = stuckMap[a_key];
                touched.push_back(a_key);
                if (st.backoffUntil != std::chrono::steady_clock::time_point{}) {
                    const bool inventoryChanged = st.inventoryKey != inventoryKey;
                    if (now < st.backoffUntil && !inventoryChanged) return false;
                    // Backoff over (60 s floor) or the inventory changed: retry from a
                    // clean count, and report again if it stalls again.
                    st = StuckState{};
                }
                if (st.passes > 0 && st.emptyAtIssue == a_emptyNow) {
                    ++st.passes;
                    if (!st.reported && st.passes >= kStuckPasses) {
                        st.reported = true;
                        spdlog::warn("[meo] reconcile STUCK {:08X} '{}' {:08X}/{} slot {} gem {:08X} '{}' -- "
                                     "{} accepted {} time(s) and the empty count never moved ({}); "
                                     "backing off {} s (or until his inventory changes) -- see MEO.log [api] {}",
                                     a_actor->GetFormID(), a_actor->GetName() ? a_actor->GetName() : "?",
                                     a_key.base, a_key.uid, a_key.slot, a_key.gemBase, a_gemName,
                                     a_api, st.passes - 1, a_emptyNow,
                                     std::chrono::duration_cast<std::chrono::seconds>(kStuckBackoff).count(), a_api);
                        st.backoffUntil = now + kStuckBackoff;
                        st.inventoryKey = inventoryKey;
                        return false;
                    }
                } else {
                    st.passes       = 1;   // first issue, or the count moved (progress): restart
                    st.emptyAtIssue = a_emptyNow;
                }
                return true;
            };
            auto passOf = [&](const StuckKey& a_key) { auto it = stuckMap.find(a_key); return it == stuckMap.end() ? 0 : it->second.passes; };
            // Is a SOCKET of this gem base into this item (any slot) currently backed
            // off? The swap-up must never unsocket a worn gem to make room for a gem
            // MEO keeps refusing on this item (Fable round-2 SEV-2 on 5f814d5: that
            // was a self-driven unsocket/re-socket loop, since our own unsocket
            // changes the inventory fingerprint and lifts the back-off).
            auto socketBackedOff = [&](RE::FormID a_base, std::uint16_t a_uid, RE::FormID a_gemBase) {
                for (const auto& [k, st] : stuckMap)
                    if (k.op == 0 && k.base == a_base && k.uid == a_uid && k.gemBase == a_gemBase &&
                        st.backoffUntil != std::chrono::steady_clock::time_point{} && now < st.backoffUntil)
                        return true;
                return false;
            };

            // Per-item record of what this pass did, for the LEFTOVER classification.
            struct ItemPass {
                ItemFit fitAtStart{};   // capacity/domain/support/conduit as found (support = socketed gems)
                ItemFit fitNow{};       // ... plus any support gem ISSUED to it this pass
                int  emptyAtStart = 0;  // GetEmptySocketCount at the top of the pass
                int  issued       = 0;  // SocketGem requests queued to it this pass
                bool considered   = false;   // reached the slot loop (eligible, socketable, not foreign-enchanted, not dup-deferred)
                bool dupDeferred  = false;   // uid 0 + an UNWORN duplicate copy carried: no request this pass
                bool mintIssued   = false;   // uid 0: took its ONE socket this pass, the rest wait for the uid
                bool mintWait     = false;   // uid 0: another worn instance of the same base took the uid-0 request this pass
                bool refused      = false;   // a SocketGem returned false on it this pass (see the tripwire note at the issue site)
                std::unordered_set<std::uint32_t> excluded;   // loose entries stuck/backed off on THIS item
            };
            std::vector<ItemPass> recs(items.size());
            std::unordered_set<RE::FormID> mintedBases;    // bases that took a uid-0 SocketGem this pass (one per base per pass)

            for (std::size_t idx = 0; idx < items.size(); ++idx) {
                auto& item = items[idx];
                auto& rec  = recs[idx];
                const int emptyCount = g_meo->GetEmptySocketCount(a_actor, item.base, item.uid);
                rec.emptyAtStart = std::max(emptyCount, 0);
                // Tier 1 (conservation) only FILLS empty sockets -- if none, skip. Tier 2
                // (effect-aware) must still reach the swap-up below on a fully-socketed
                // item, so don't skip it here when a_effectAware.
                if (emptyCount <= 0 && !a_effectAware) continue;

                // Filled slots (+ their gems, for the effect-aware swap). GetGemDetails
                // returns the TRUE count; capacity = filled + empty.
                constexpr std::uint32_t kMaxDet = 8;
                MEO_API::GemDetail det[kMaxDet];
                const std::uint32_t trueDet = g_meo->GetGemDetails(a_actor, item.base, item.uid, det, kMaxDet);
                const std::uint32_t nDet    = std::min(trueDet, kMaxDet);
                const int capacity          = static_cast<int>(trueDet) + emptyCount;
                if (capacity <= 0) continue;   // socketless item -- nothing to fill or swap

                // FOREIGN enchant: an enchant on the worn xList that is not one of
                // MEO's socket records. ApiSocketGem refuses it (MEO plugin.cpp:9258)
                // and at uid 0 MEO's mint path skips this xList and falls through to
                // "any fresh instance of this base" -- never target it.
                if (item.xlEnchanted && (item.uid == 0 || trueDet == 0)) continue;

                std::unordered_set<std::uint8_t> filled;
                bool hasSupport = false, hasConduit = false;
                for (std::uint32_t i = 0; i < nDet; ++i) {
                    filled.insert(det[i].slot);
                    if (det[i].isSupport) {
                        hasSupport = true;
                        if (ContainsCI(det[i].gid, "conduit")) hasConduit = true;   // MEO GemCatalog.h gid "conduit"
                    }
                }
                rec.fitAtStart = ItemFit{ capacity, item.isArmor, hasSupport, hasConduit };
                rec.fitNow     = rec.fitAtStart;

                // A WORN item never gemmed (uid 0): MEO mints the uid on the FIRST
                // socket, so fill only ONE slot this pass; the rest fill next pass once
                // WornUid returns the minted uid (avoids ambiguous uid-0 targeting).
                const bool mintGuard = (item.uid == 0);
                // If an UNWORN duplicate copy of this base is carried (e.g. a junk copy
                // awaiting sale), a uid-0 SocketGem could mint on the wrong instance and
                // park the gem on to-be-sold junk (churn with economy on, permanent loss
                // with it off). Defer until the copy is gone or the worn copy has a real
                // uid. A second WORN copy (the dual-wield pair) is not junk: MEO's uid-0
                // path mints on a worn xList (plugin.cpp:9143), so it lands on one of the
                // pair; only ONE uid-0 request per base per pass, the other instance waits.
                if (mintGuard && invBaseCount(item.base) > wornCopies(item.base)) { rec.dupDeferred = true; continue; }
                rec.considered = true;   // classifier order: dupDeferred is tested first, then mintWait/mintIssued/refused, then excluded
                if (mintGuard && mintedBases.contains(item.base)) { rec.mintWait = true; continue; }

                // Loose entries EXCLUDED for THIS item this pass: a gem whose socket
                // into this item is stuck/backed off. It neither reserves (a later
                // item may still take it) nor SHADOWS (the slot re-picks past it, and
                // the swap-up ignores it) -- Fable round-2 SEV-2 on 5f814d5.
                auto& excluded = rec.excluded;
                auto pickGem = [&]() -> int {
                    int pick = -1;
                    if (!a_effectAware) {
                        // tier 1 conservation: first fitting loose gem in stock.
                        for (std::uint32_t i = 0; i < nLoose; ++i)
                            if (avail[i] > 0 && !excluded.contains(i) && GemFits(loose[i], rec.fitNow)) { pick = static_cast<int>(i); break; }
                    } else {
                        // tier 2 effect-aware: best by (class bonus, base magnitude).
                        int bestB = std::numeric_limits<int>::min(); float bestM = -1.0f;
                        for (std::uint32_t i = 0; i < nLoose; ++i) {
                            if (avail[i] == 0 || excluded.contains(i) || !GemFits(loose[i], rec.fitNow)) continue;
                            const int bo = GemBonus(loose[i].gid, loose[i].name,
                                                    loose[i].isArmor, loose[i].isSupport, a_prefs);
                            if (bo > bestB || (bo == bestB && loose[i].magnitude > bestM)) {
                                bestB = bo; bestM = loose[i].magnitude; pick = static_cast<int>(i);
                            }
                        }
                    }
                    return pick;
                };

                for (int slot = 0; slot < capacity; ++slot) {
                    if (filled.contains(static_cast<std::uint8_t>(slot))) continue;

                    // Pick, gate, and on a stuck pick EXCLUDE it and RE-PICK for this
                    // same slot until a gem passes the gate or none is left.
                    int pick = -1;
                    StuckKey key{};
                    for (;;) {
                        pick = pickGem();
                        if (pick < 0) break;   // no (non-stuck) gem fits this item
                        // STALL DETECTOR: same request as last pass with the empty count
                        // unchanged = MEO accepted it and it did not land.
                        key = StuckKey{ 0, item.base, item.uid, static_cast<std::uint8_t>(slot), loose[pick].gemBase };
                        if (stallGate(key, emptyCount, "SocketGem", loose[pick].name)) break;
                        excluded.insert(static_cast<std::uint32_t>(pick));   // stuck here: neither reserved nor shadowing
                    }
                    if (pick < 0) break;   // nothing left for this item -> next item

                    const bool queued = g_meo->SocketGem(a_actor, item.base, item.uid, static_cast<std::uint8_t>(slot),
                                                         loose[pick].gemBase, loose[pick].gemUid);
                    if (queued) {
                        // The gem is spoken for ONLY once MEO took the request: a refusal
                        // leaves it in stock for the next item (and the LEFTOVER line).
                        --avail[pick];
                        ++rec.issued;
                        if (mintGuard) mintedBases.insert(item.base);
                        if (loose[pick].isSupport) {
                            rec.fitNow.hasSupport = true;   // one support per item (MEO :9272)
                            if (ContainsCI(loose[pick].gid, "conduit")) rec.fitNow.hasConduit = true;   // MEO runs the queue in order: it lands before a later off-domain request
                        }
                        spdlog::info("[meo] reconcile socket '{}' -> {:08X}/{} slot {} on {:08X} (queued{})",
                                     loose[pick].name, item.base, item.uid, slot, a_actor->GetFormID(),
                                     passOf(key) > 1 ? std::format(", pass {}", passOf(key)) : std::string{});
                    } else {
                        spdlog::warn("[meo] reconcile socket '{}' -> {:08X}/{} slot {} on {:08X} REFUSED by MEO (SocketGem returned false) -- see MEO.log",
                                     loose[pick].name, item.base, item.uid, slot, a_actor->GetFormID());
                        // TRIPWIRE, not a live path: today's MEO returns false only for a
                        // null actor (plugin.cpp:9416-9427), which this pass excludes; every
                        // real refusal happens inside its queued task and is visible here
                        // only through the STUCK detector. Kept because the ABI allows false.
                        rec.refused = true;
                        break;
                    }
                    if (mintGuard) { rec.mintIssued = true; break; }   // one socket per pass on a fresh (uid 0) item
                }

                // tier 2 SWAP-UP: if a still-available loose gem STRICTLY beats the
                // weakest socketed gem (same domain), unsocket the socketed one. It
                // returns to loose inventory and the freed slot re-fills next pass with
                // the better gem. STRICT-only, so the demoted gem (now weaker than what
                // replaces it) never re-triggers -> no ping-pong. Async latency << the
                // ~1 s cadence, so no double-unsocket.
                if (a_effectAware && !mintGuard && nDet > 0) {
                    std::uint32_t weakIdx = 0;
                    int   weakB = std::numeric_limits<int>::max();
                    float weakM = std::numeric_limits<float>::max();
                    for (std::uint32_t i = 0; i < nDet; ++i) {
                        const int bo = GemBonus(det[i].gid, det[i].name, det[i].isArmor, det[i].isSupport, a_prefs);
                        if (bo < weakB || (bo == weakB && det[i].effectiveMagnitude < weakM)) {
                            weakB = bo; weakM = det[i].effectiveMagnitude; weakIdx = i;
                        }
                    }
                    // The candidate must fit the item WITHOUT the evictee (Fable SEV-2 on
                    // 1efa3e3): evicting the Conduit un-admits every off-domain gem, so an
                    // off-domain candidate admitted THROUGH that Conduit would be refused
                    // next pass, the Conduit re-socketed, and the pair would cycle every
                    // ~2.4 s with the empty count moving each time (invisible to the stall
                    // detector). Likewise a support evictee frees the item's one support seat.
                    ItemFit sansEvictee = rec.fitNow;
                    // A merely QUEUED Conduit must not license an eviction (Fable SEV-4 on
                    // 9dacc0e): if MEO refuses it inside its task, the evictee was unsocketed
                    // for nothing and our own fingerprint change lifts the back-off -- churn.
                    // The slot path keeps the queued shortcut (a refused Conduit there costs
                    // nothing that is not already cleared); the swap-up judges LANDED state.
                    sansEvictee.hasConduit = rec.fitAtStart.hasConduit;
                    if (det[weakIdx].isSupport) {
                        sansEvictee.hasSupport = false;
                        if (ContainsCI(det[weakIdx].gid, "conduit")) sansEvictee.hasConduit = false;
                    }
                    int   loot = -1;
                    int   lootB = std::numeric_limits<int>::min();
                    float lootM = -1.0f;
                    for (std::uint32_t i = 0; i < nLoose; ++i) {
                        if (avail[i] == 0 || !GemFits(loose[i], sansEvictee)) continue;
                        // Never make room for a gem MEO keeps refusing on this item
                        // (excluded this pass, or its socket key is still backed off from
                        // an earlier pass) -- the self-driven unsocket/re-socket loop.
                        if (excluded.contains(i) || socketBackedOff(item.base, item.uid, loose[i].gemBase)) continue;
                        const int bo = GemBonus(loose[i].gid, loose[i].name,
                                                loose[i].isArmor, loose[i].isSupport, a_prefs);
                        if (bo > lootB || (bo == lootB && loose[i].magnitude > lootM)) {
                            lootB = bo; lootM = loose[i].magnitude; loot = static_cast<int>(i);
                        }
                    }
                    if (loot >= 0 && (lootB > weakB || (lootB == weakB && lootM > weakM))) {
                        // Same stall gate as the socket (op 1; GemDetail carries no gem
                        // base, so 0): an accepted-then-failed unsocket would otherwise
                        // re-issue every pass with a "(queued)" line -- the 192x class.
                        // Progress = the empty count RISING once the unsocket lands.
                        const StuckKey ukey{ 1, item.base, item.uid, det[weakIdx].slot, 0 };
                        if (!stallGate(ukey, emptyCount, "UnsocketGem", det[weakIdx].name)) continue;
                        if (g_meo->UnsocketGem(a_actor, item.base, item.uid, det[weakIdx].slot)) {
                            // MFO-B36: RESERVE the copy this swap-out is for, so a later item
                            // cannot evict its own weakest for the same single loose gem. It is
                            // out of avail[] now, so the LEFTOVER line below no longer counts it
                            // (its socket opens next pass) -- no separate swap-pending exemption.
                            --avail[static_cast<std::uint32_t>(loot)];
                            spdlog::info("[meo] reconcile swap-out '{}' (slot {}) on {:08X} -- '{}' will re-fill (queued{})",
                                         det[weakIdx].name, det[weakIdx].slot, a_actor->GetFormID(), loose[loot].name,
                                         passOf(ukey) > 1 ? std::format(", pass {}", passOf(ukey)) : std::string{});
                        } else
                            spdlog::warn("[meo] reconcile swap-out '{}' (slot {}) on {:08X} REFUSED by MEO (UnsocketGem returned false) -- see MEO.log",
                                         det[weakIdx].name, det[weakIdx].slot, a_actor->GetFormID());
                    }
                }
            }

            // ── THE LEFTOVER LINE: the invariant, checked at the end of the pass ──
            // For every loose gem still in stock: is there a worn socket (as MEO
            // counts them) that is still empty after this pass's issues? If no item
            // has one, the invariant holds and nothing is logged. Otherwise say why
            // the gem is not in it (one line per (actor, gem, why), rate-limited).
            auto& leftMap = g_leftover[a_actor->GetFormID()];
            for (auto& kv : leftMap) kv.second.seen = false;
            bool anyEmpty = false;
            for (const auto& rec : recs)
                if (rec.considered || rec.dupDeferred) if (rec.emptyAtStart - rec.issued > 0) { anyEmpty = true; break; }
            if (anyEmpty) {
                for (std::uint32_t i = 0; i < nLoose; ++i) {
                    // Copies a swap-out was issued FOR this pass are already out of
                    // avail[] (MFO-B36 reservation): the REST of a stack still count.
                    std::uint32_t leftN = avail[i];
                    if (leftN == 0) continue;
                    bool compatNow = false, stuck = false, refused = false, dup = false, minting = false, supportLimit = false, hole = false;
                    for (std::size_t idx = 0; idx < items.size(); ++idx) {
                        const auto& rec = recs[idx];
                        if (!(rec.considered || rec.dupDeferred) || rec.emptyAtStart - rec.issued <= 0) continue;
                        if (!GemFits(loose[i], rec.fitNow)) {
                            // A support gem kept out only by the item's one support seat.
                            if (loose[i].isSupport && rec.fitNow.capacity >= 2) supportLimit = true;
                            continue;
                        }
                        compatNow = true;
                        // ORDER MATTERS: the deferrals are tested before `excluded`, because a
                        // deferred item never ran its slot loop and so never excluded anything.
                        if (rec.dupDeferred)       { dup = true;     continue; }
                        if (rec.mintWait || rec.mintIssued) { minting = true; continue; }
                        if (rec.refused)           { refused = true; continue; }
                        if (rec.excluded.contains(i) || socketBackedOff(items[idx].base, items[idx].uid, loose[i].gemBase)) { stuck = true; continue; }
                        hole = true;   // an open, compatible, un-deferred socket the pass walked past
                    }
                    LeftoverWhy why;
                    if (!compatNow) {
                        // Nothing of its kind is open now: kept out by the support seat, or
                        // was it ever open this pass (then other gems took it), or never?
                        bool wasOpen = false;
                        for (const auto& rec : recs)
                            if ((rec.considered || rec.dupDeferred) && rec.emptyAtStart > 0 && GemFits(loose[i], rec.fitAtStart)) { wasOpen = true; break; }
                        why = supportLimit ? LeftoverWhy::kSupportLimit
                            : wasOpen      ? LeftoverWhy::kCapacity : LeftoverWhy::kOffDomain;
                    } else if (hole)    why = LeftoverWhy::kUnclassified;
                    else if (stuck)     why = LeftoverWhy::kStuck;
                    else if (refused)   why = LeftoverWhy::kRefused;
                    else if (dup)       why = LeftoverWhy::kDuplicateCopy;
                    else if (minting)   why = LeftoverWhy::kMinting;
                    else                why = LeftoverWhy::kUnclassified;

                    const std::uint64_t lk = (static_cast<std::uint64_t>(loose[i].gemBase) << 8) | static_cast<std::uint8_t>(why);
                    auto& ls = leftMap[lk];
                    const bool fresh = ls.at == std::chrono::steady_clock::time_point{};
                    if (fresh || ls.inventoryKey != inventoryKey || now >= ls.at + kStuckBackoff) {
                        ls.at = now;
                        ls.inventoryKey = inventoryKey;
                        // A one-pass mint deferral is by design (info); every other class is a
                        // broken invariant the user needs to see (warn).
                        const char* actorName = a_actor->GetName() ? a_actor->GetName() : "?";
                        if (why == LeftoverWhy::kMinting)
                            spdlog::info("[meo] reconcile LEFTOVER {:08X} '{}' {:08X} '{}' x{} -- {}",
                                         a_actor->GetFormID(), actorName, loose[i].gemBase, loose[i].name, leftN, LeftoverWord(why));
                        else
                            spdlog::warn("[meo] reconcile LEFTOVER {:08X} '{}' {:08X} '{}' x{} -- {}",
                                         a_actor->GetFormID(), actorName, loose[i].gemBase, loose[i].name, leftN, LeftoverWord(why));
                    }
                    ls.seen = true;
                }
            }
            std::erase_if(leftMap, [](const auto& kv) { return !kv.second.seen; });
            if (leftMap.empty()) g_leftover.erase(a_actor->GetFormID());

            // Forget every stall key this pass did not re-issue: the request stopped
            // recurring (it landed, the gem left, the item was sold), so it is not
            // stuck and a later identical request starts from a clean count. A key
            // still in BACK-OFF is kept even if untouched (the swap-up's
            // socketBackedOff read needs it when the item has no empty slot to gate
            // on); it expires at its own backoffUntil.
            std::erase_if(stuckMap, [&](const auto& kv) {
                const bool backedOff = kv.second.backoffUntil != std::chrono::steady_clock::time_point{} &&
                                       now < kv.second.backoffUntil;
                return !backedOff && std::find(touched.begin(), touched.end(), kv.first) == touched.end();
            });
            if (stuckMap.empty()) g_stuck.erase(a_actor->GetFormID());
        }
    }

    bool GemReconcileSupported() { return g_meo && g_meo->Version() >= 3; }

    void RequestGemReconcile(RE::Actor* a_follower, bool a_effectAware, GemReconcilePrefs a_prefs) {
        if (!GemReconcileSupported() || !a_follower) return;
        if (!MainThread::IsInstalled()) { ReconcileLooseGems(a_follower, a_effectAware, a_prefs); return; }  // VR: run direct
        const RE::FormID fid = a_follower->GetFormID();
        MainThread::Post([fid, a_effectAware, a_prefs]() {
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(fid))
                ReconcileLooseGems(a, a_effectAware, a_prefs);
        });
    }

    void QueueGemMove(RE::Actor* a_follower, RE::FormID a_fromBase, std::uint16_t a_fromUid,
                      RE::FormID a_toBase) {
        // No source gems (uid 0), no MEO, or malformed -> nothing to carry over.
        if (!g_meo || !a_follower || a_fromUid == 0 || a_fromBase == 0 || a_toBase == 0) return;
        std::scoped_lock lk(g_mx);
        g_pending[Key(a_follower->GetFormID(), a_toBase)] = { a_fromBase, a_fromUid };
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_pending.clear();
        g_carriedGems.clear();      // per-follower carried-gem cache is session-scoped
        g_extractRequested.clear();
        g_stuck.clear();            // reconcile stall detector: keyed by FormID, session-scoped (main thread, like its writer)
        g_leftover.clear();         // reconcile LEFTOVER rate-limit: same domain as g_stuck
    }

    GemPreview PreviewWithGems(RE::Actor* a_actor, RE::TESBoundObject* a_candidateBase) {
        GemPreview out{ 0, 0, 0, 0.0f };
        if (!g_meo || !a_actor || !a_candidateBase) return out;
        out.capacity = g_meo->GetSocketCapacity(a_candidateBase);

        constexpr std::uint32_t kMax = 32;
        MEO_API::GemInfo gems[kMax];
        const std::uint32_t n = g_meo->GetActorGems(a_actor, gems, kMax);
        out.gemsHeld = static_cast<int>(n);

        // Preview approximation: fill the candidate's sockets with the actor's
        // gems up to capacity (MEO does the real domain/support filtering on the
        // actual move). Enough to SHOW "this item, with my gems."
        const std::uint32_t have = std::min<std::uint32_t>(n, kMax);
        for (std::uint32_t i = 0; i < have && out.gemsThatFit < out.capacity; ++i) {
            out.magnitudeFit += gems[i].magnitude;
            ++out.gemsThatFit;
        }
        return out;
    }
}
