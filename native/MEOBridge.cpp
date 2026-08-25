#include "MEOBridge.h"
#include "MEO_API.h"
#include "MainThread.h"   // Build A: main-thread refresh of the carried-gem cache

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
                // mints on the now-worn xList). MoveGems self-queues to main.
                g_meo->MoveGems(actor, p.fromBase, p.fromUid, a_ev->baseObject, a_ev->uniqueID);
                spdlog::info("[meo] gem move {:08X}/{} -> {:08X}/{} on {:08X}",
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
        for (const std::uint8_t s : slots)
            g_meo->UnsocketGem(a_actor, a_base, a_uid, s);
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
