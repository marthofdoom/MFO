// logistics/GemRules.cpp -- MEO-aware gem rules. Module doc in GemRules.h.
#include "PCH.h"
#include "GemRules.h"
#include "Logistics_internal.h"   // FitsCarryWeight, IsQuestObjectInstance, IsCreature*
#include "ItemCatalog.h"
#include "Config.h"
#include "MEOBridge.h"
#include "MainThread.h"

#include <atomic>
#include <chrono>
#include <set>
#include <tuple>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::GemRules {

    namespace {
        using Clock = std::chrono::steady_clock;

        // "Worth selling once gemless": the item's own instance value (tempering/enchant included)
        // at or above this. No such floor existed in the sell path (it sells anything unkept), so
        // this is the one new threshold -- marth's "dont even bother keeping gear that isnt worth
        // selling gemless".
        constexpr std::int32_t kMinSellValue = 50;
        // A strip that has not landed after this long is given up on (the item stays, gems on it).
        constexpr std::chrono::seconds kStripPatience{ 15 };

        const char* NameOf(const RE::TESBoundObject* a_o) {
            const char* n = a_o ? a_o->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        // Worker-only: probed (source, base, uid) keys, so a gemless / returned item left in its
        // container is never taken again. Written and read only in LootEquipment (one follower at
        // a time on the logistics worker) -- no lock. Cleared at revert.
        std::set<std::tuple<RE::FormID, RE::FormID, std::uint16_t>> g_probed;

        // MAIN-THREAD ONLY: looted gear whose gems are being stripped, awaiting the decision.
        struct Probe {
            RE::FormID    fol, base, src;
            std::uint16_t uid;
            Clock::time_point since;
        };
        std::vector<Probe> g_probes;
        std::atomic<int>   g_probeCount{ 0 };

        std::uint16_t UidOf(RE::ExtraDataList* a_xl) {
            auto* u = a_xl ? a_xl->GetByType<RE::ExtraUniqueID>() : nullptr;
            return u ? u->uniqueID : 0;
        }

        // The follower's own xList for (obj,uid), plus the instance value. Null xl if gone.
        RE::ExtraDataList* FindOwned(RE::Actor* a_fol, RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                     std::int32_t& a_value) {
            a_value = 0;
            for (auto& [o, d] : a_fol->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; })) {
                auto* e = d.second.get();
                if (!e || !e->extraLists) continue;
                for (auto* xl : *e->extraLists)
                    if (UidOf(xl) == a_uid) { a_value = e->GetValue(); return xl; }
            }
            return nullptr;
        }

        Verdict Check(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, bool a_live) {
            if (!Active() || !a_actor || !a_obj) return Verdict::Clear;
            const RE::FormID fid = a_actor->GetFormID();
            if (a_live) MEOBridge::RefreshCarriedGems(a_actor);
            std::vector<std::uint16_t> uids;
            for (auto& [o, d] : a_actor->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; })) {
                auto* e = d.second.get();
                if (!e || !e->extraLists) continue;
                for (auto* xl : *e->extraLists)
                    if (const auto uid = UidOf(xl)) uids.push_back(uid);
            }
            if (uids.empty()) return Verdict::Clear;   // no instance has a uid: MEO never socketed it
            if (!MEOBridge::CacheWarmed(fid)) {
                MEOBridge::RequestCarriedGemRefresh(a_actor);
                spdlog::debug("[gems] {:08X}: '{}' held ({}): carried-gem cache not warm yet", fid, NameOf(a_obj), a_why);
                return Verdict::Held;
            }
            bool held = false;
            for (const auto uid : uids) {
                if (!MEOBridge::IsCarriedGemmed(fid, a_obj->GetFormID(), uid)) continue;
                held = true;
                if (!MEOBridge::CarriedGemUnsocketSupported()) {
                    static thread_local std::set<std::uint64_t> warned;   // once per item per thread, not per scan
                    if (warned.insert((static_cast<std::uint64_t>(a_obj->GetFormID()) << 16) | uid).second)
                        spdlog::warn("[gems] {:08X}: '{}' is SOCKETED and MEO cannot strip it (ABI < 3): NOT leaving ({})",
                                     fid, NameOf(a_obj), a_why);
                    continue;
                }
                const int n = MEOBridge::CarriedGemCount(fid, a_obj->GetFormID(), uid);
                if (MEOBridge::UnsocketItemGems(a_actor, a_obj->GetFormID(), uid) > 0)
                    spdlog::info("[gems] stripped {} gem(s) from '{}' ({})", n, NameOf(a_obj), a_why);
            }
            return held ? Verdict::Held : Verdict::Clear;
        }

        // MAIN THREAD. Gems are out (or the item had none): keep it if it is worth selling gemless,
        // else put it back where it was found.
        void Decide(RE::Actor* a_fol, RE::TESBoundObject* a_obj, std::uint16_t a_uid, RE::FormID a_srcID) {
            std::int32_t value = 0;
            auto* xl = FindOwned(a_fol, a_obj, a_uid, value);
            if (!xl) return;   // already gone from him: nothing to decide
            if (value >= kMinSellValue) {
                spdlog::info("[gems] {:08X}: kept '{}' (worth {} gemless, sells normally)", a_fol->GetFormID(), NameOf(a_obj), value);
                return;
            }
            auto* src = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_srcID);
            if (!src || src->IsDisabled()) {
                spdlog::warn("[gems] {:08X}: '{}' not worth keeping but its source {:08X} no longer resolves: kept",
                             a_fol->GetFormID(), NameOf(a_obj), a_srcID);
                return;
            }
            a_fol->RemoveItem(a_obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, xl, src);
            spdlog::info("[gems] returned '{}' to container {:08X} (not worth keeping gemless)", NameOf(a_obj), a_srcID);
        }

        void ProbeOnMain(RE::FormID a_fol, RE::FormID a_base, std::uint16_t a_uid, RE::FormID a_src) {
            auto* fol = RE::TESForm::LookupByID<RE::Actor>(a_fol);
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
            if (!fol || !obj) return;
            MEOBridge::RefreshCarriedGems(fol);
            if (!MEOBridge::IsCarriedGemmed(a_fol, a_base, a_uid)) { Decide(fol, obj, a_uid, a_src); return; }
            const int n = MEOBridge::CarriedGemCount(a_fol, a_base, a_uid);
            if (!MEOBridge::CarriedGemUnsocketSupported()) {
                spdlog::warn("[gems] {:08X}: looted '{}' is socketed and MEO cannot strip it (ABI < 3): kept as is", a_fol, NameOf(obj));
                return;
            }
            if (MEOBridge::UnsocketItemGems(fol, a_base, a_uid) > 0)
                spdlog::info("[gems] stripped {} gem(s) from '{}' (looted gear)", n, NameOf(obj));
            g_probes.push_back({ a_fol, a_base, a_src, a_uid, Clock::now() });
            g_probeCount.store(static_cast<int>(g_probes.size()));
        }

        void PumpOnMain(RE::FormID a_fol) {
            auto* fol = RE::TESForm::LookupByID<RE::Actor>(a_fol);
            if (!fol) return;
            bool refreshed = false;
            for (std::size_t i = 0; i < g_probes.size();) {
                const Probe p = g_probes[i];
                if (p.fol != a_fol) { ++i; continue; }
                if (!refreshed) { MEOBridge::RefreshCarriedGems(fol); refreshed = true; }
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(p.base);
                if (!obj) { g_probes.erase(g_probes.begin() + i); continue; }
                if (!MEOBridge::IsCarriedGemmed(a_fol, p.base, p.uid)) {
                    g_probes.erase(g_probes.begin() + i);
                    Decide(fol, obj, p.uid, p.src);
                    continue;
                }
                if (Clock::now() - p.since > kStripPatience) {
                    spdlog::warn("[gems] {:08X}: strip of '{}' not confirmed after {} s: keeping it with its gems (never lost)",
                                 a_fol, NameOf(obj), kStripPatience.count());
                    g_probes.erase(g_probes.begin() + i);
                    continue;
                }
                ++i;
            }
            g_probeCount.store(static_cast<int>(g_probes.size()));
        }
    }

    bool Active() {
        return Config::g_meoAwareGems.load() && MEOBridge::CarriedGemsSupported();
    }

    Verdict CheckRemovalCached(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why) {
        return Check(a_actor, a_obj, a_why, false);
    }
    Verdict CheckRemovalLive(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why) {
        return Check(a_actor, a_obj, a_why, true);
    }

    GemCandidate FindGemCandidate(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
        if (!Active() || !MEOBridge::CarriedGemUnsocketSupported() || !a_follower || !a_src) return {};
        for (auto& [obj, data] : a_src->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* e = data.second.get();
            if (!e || !e->extraLists || IsQuestObjectInstance(e)) continue;
            if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
            if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                if (IsCreatureArmor(armo) || Config::g_dollsMode.load()) continue;
            } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                if (IsCreatureWeapon(weap)) continue;
            } else continue;
            if (!FitsCarryWeight(a_follower, obj->GetWeight())) continue;
            for (auto* xl : *e->extraLists) {
                const auto uid = UidOf(xl);
                if (uid == 0) continue;
                if (g_probed.contains({ a_src->GetFormID(), obj->GetFormID(), uid })) continue;
                return { obj, xl, uid };
            }
        }
        return {};
    }

    bool TakeAndProbe(RE::Actor* a_follower, RE::TESObjectREFR* a_src, const GemCandidate& a_c) {
        if (!a_follower || !a_src || !a_c.obj) return false;
        const RE::FormID fol = a_follower->GetFormID(), base = a_c.obj->GetFormID(), src = a_src->GetFormID();
        const std::uint16_t uid = a_c.uid;
        g_probed.insert({ src, base, uid });   // never re-taken, whatever the probe decides
        std::int32_t v = 0;
        a_src->RemoveItem(a_c.obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, a_c.xl, a_follower);
        if (!FindOwned(a_follower, a_c.obj, uid, v)) return false;
        spdlog::info("[gems] {:08X}: took '{}' from {:08X} to look for gems", fol, NameOf(a_c.obj), src);
        if (MainThread::IsInstalled()) MainThread::Post([fol, base, uid, src]() { ProbeOnMain(fol, base, uid, src); });
        else                           ProbeOnMain(fol, base, uid, src);   // VR: no pump, same as the other gem reads
        return true;
    }

    void PumpProbes(RE::Actor* a_follower) {
        if (!a_follower || g_probeCount.load() == 0) return;
        const RE::FormID fol = a_follower->GetFormID();
        if (MainThread::IsInstalled()) MainThread::Post([fol]() { PumpOnMain(fol); });
        else                           PumpOnMain(fol);
    }

    void ClearTransient() {
        g_probed.clear();
        g_probes.clear();
        g_probeCount.store(0);
    }
}
