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
            bool          sellGambit;   // the follower had a loot-valuables (to sell) gambit when he took it
            bool          loose = false;        // taken from the world: "returned" = dropped back at pos/rot
            bool          hadGems = true;       // a probe is queued only for an item that was gemmed
            bool          stripQueued = true;   // loose probes queue the strip once the item has arrived
            RE::NiPoint3  pos{}, rot{};
        };
        // Worker-only: loose refs recognised by name this scan pass, and (base,uid) already picked up once.
        std::set<RE::FormID> g_looseRefs;
        std::set<std::pair<RE::FormID, std::uint16_t>> g_looseTaken;
        std::vector<Probe> g_probes;
        std::atomic<int>   g_probeCount{ 0 };

        bool MatchesMeoTitle(std::string a_name, const std::string& a_base);   // below (MEO's title scheme)

        // Is this instance's custom name MEO's socketed-item title? (the only gem tell a container or
        // world item gives before we own it)
        // A LOOSE REF's own extra list (TESObjectREFR::extraList, not an inventory list): LooseNamedGemmed below.
        bool NamedGemmed(RE::ExtraDataList* a_xl, const RE::TESBoundObject* a_base) {
            auto* txt = a_xl ? a_xl->GetByType<RE::ExtraTextDisplayData>() : nullptr;
            if (!txt || !a_base) return false;
            std::string name = txt->displayName.c_str() ? txt->displayName.c_str() : "";
            if (txt->IsPlayerSet() && txt->customNameLength > 0 && txt->customNameLength <= name.size()) name.resize(txt->customNameLength);
            return MatchesMeoTitle(name, a_base->GetName() ? a_base->GetName() : "");
        }
        // A copy in a container / corpse, from the source's snapshot (the same text, copied out on the main thread).
        bool NamedGemmed(const InvSnap::Copy& a_copy, const RE::TESBoundObject* a_base) {
            if (!a_copy.hasText || !a_base) return false;
            std::string name = a_copy.displayName;
            if (a_copy.playerSetName && a_copy.customNameLength > 0 && a_copy.customNameLength <= name.size()) name.resize(a_copy.customNameLength);
            return MatchesMeoTitle(name, a_base->GetName() ? a_base->GetName() : "");
        }

        std::uint16_t UidOf(RE::ExtraDataList* a_xl) {
            auto* u = a_xl ? a_xl->GetByType<RE::ExtraUniqueID>() : nullptr;
            return u ? u->uniqueID : 0;
        }

        // The follower's own xList for (obj,uid), plus the instance value. Null xl if gone.
        // A LIVE read (INVARIANTS #84 exception): it hands a live ExtraDataList to the engine write that follows on the same thread.
        // Callers: Decide and PumpOnMain (the main thread) and TakeAndProbe's direct road (the main thread, or no pump installed:
        // from the worker tick every take is queued, INVARIANTS #83). No decision reads this.
        RE::ExtraDataList* FindOwned(RE::Actor* a_fol, RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                     std::int32_t& a_value) {
            a_value = 0;
            MainThread::NoteWriteRead(__FUNCTION__);   // INVARIANTS #84: the live read of a write made right here (self-counted when off the main thread)
            for (auto& [o, d] : a_fol->GetInventory([&](RE::TESBoundObject& x) { return &x == a_obj; })) {
                auto* e = d.second.get();
                if (!e || !e->extraLists) continue;
                for (auto* xl : *e->extraLists)
                    if (UidOf(xl) == a_uid) { a_value = e->GetValue(); return xl; }
            }
            return nullptr;
        }

        Verdict Check(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, bool a_live, Reason a_reason,
                      std::vector<HoldKey>* a_touched) {
            if (!Active() || !a_actor || !a_obj) return Verdict::Clear;
            const RE::FormID fid = a_actor->GetFormID();
            if (a_live) MEOBridge::RefreshCarriedGems(a_actor);
            std::vector<std::uint16_t> uids;
            const auto inv = InvSnap::Items(a_actor);   // INVARIANTS #84 (read live in place on the main thread: CheckRemovalLive)
            if (!inv) {   // not yet: it does NOT leave on an unread pack (never "Clear" from absence)
                spdlog::debug("[gems] {:08X}: '{}' held ({}): his inventory snapshot is not readable yet", fid, NameOf(a_obj), a_why);
                return Verdict::Held;
            }
            if (const auto* e = inv.Find(a_obj))
                for (const auto& c : e->copies)
                    if (c.hasUid && c.uid != 0) uids.push_back(c.uid);
            if (uids.empty()) return Verdict::Clear;   // no instance has a uid: MEO never socketed it
            // Whatever is about to leave takes no gem back from the reconcile (strip -> re-socket -> sell loop).
            // A hold found live lets the check read the cache; one armed NOW returns Held for this pass.
            bool armed = false;
            for (const auto uid : uids) {
                if (a_touched) a_touched->push_back({ a_obj->GetFormID(), uid });
                if (!MEOBridge::HoldForRemoval(fid, a_obj->GetFormID(), uid, static_cast<std::uint8_t>(a_reason))) armed = true;
            }
            if (armed) {
                MEOBridge::RequestCarriedGemRefresh(a_actor);
                spdlog::debug("[gems] {:08X}: '{}' hold armed ({}), removal on a later pass", fid, NameOf(a_obj), a_why);
                return Verdict::Held;
            }
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
        void Decide(RE::Actor* a_fol, RE::TESBoundObject* a_obj, std::uint16_t a_uid, RE::FormID a_srcID, bool a_sellGambit,
                    bool a_hadGems, const Probe* a_loose = nullptr) {
            std::int32_t value = 0;
            auto* xl = FindOwned(a_fol, a_obj, a_uid, value);
            if (!xl) return;   // already gone from him: nothing to decide
            // marth: the sell floor is the loot-valuables (to sell) gambit's own value/weight rule; with
            // no such gambit only the gems are kept and the gear always goes back.
            // An item that never had gems was only fetched to look: it ALWAYS goes back (the armor has to
            // qualify on its own). Only gear that actually had gems faces the loot-to-sell keep test.
            const bool keep = a_hadGems && a_sellGambit && ValuableRatioClears(static_cast<float>(value), a_obj->GetWeight());
            if (keep) {
                spdlog::info("[gems] {:08X}: kept '{}' (value {} / weight {:.1f} meets the loot-to-sell rule, sells normally)",
                             a_fol->GetFormID(), NameOf(a_obj), value, a_obj->GetWeight());
                return;
            }
            if (a_loose && a_loose->loose) {
                // KEPT ON PURPOSE (marth 2026-10-18: "Keep putting it back"): looted gear not worth keeping goes back where it lay.
                // Back where it lay: same position and angle, same instance (its extra list), via the
                // proven engine DropObject on the main thread (Decide runs there).
                if (DropObjectAt(a_fol, a_obj, xl, a_loose->pos, a_loose->rot))
                    spdlog::info("[gems] returned '{}' to its original position (not worth keeping gemless: {})", NameOf(a_obj),
                                 a_hadGems ? (a_sellGambit ? "below the loot-to-sell value/weight rule" : "no loot-to-sell gambit, gems only") : "no gems on it");
                else
                    spdlog::warn("[gems] {:08X}: could not drop '{}' back where it lay (DropObject refused): kept", a_fol->GetFormID(), NameOf(a_obj));
                return;
            }
            auto* src = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_srcID);
            if (!src || src->IsDisabled()) {
                spdlog::warn("[gems] {:08X}: '{}' not worth keeping but its source {:08X} no longer resolves: kept",
                             a_fol->GetFormID(), NameOf(a_obj), a_srcID);
                return;
            }
            // KEPT ON PURPOSE (marth 2026-10-18: "Keep putting it back"): the looted piece returns to its source container.
            a_fol->RemoveItem(a_obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, xl, src);
            spdlog::info("[gems] returned '{}' to container {:08X} (not worth keeping gemless: {})", NameOf(a_obj), a_srcID,
                         a_hadGems ? (a_sellGambit ? "below the loot-to-sell value/weight rule" : "no loot-to-sell gambit, gems only") : "no gems on it");
        }

        void ProbeOnMain(RE::FormID a_fol, RE::FormID a_base, std::uint16_t a_uid, RE::FormID a_src, bool a_sell) {
            auto* fol = RE::TESForm::LookupByID<RE::Actor>(a_fol);
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_base);
            if (!fol || !obj) return;
            MEOBridge::RefreshCarriedGems(fol);
            if (!MEOBridge::IsCarriedGemmed(a_fol, a_base, a_uid)) { Decide(fol, obj, a_uid, a_src, a_sell, /*hadGems*/ false); return; }
            const int n = MEOBridge::CarriedGemCount(a_fol, a_base, a_uid);
            if (!MEOBridge::CarriedGemUnsocketSupported()) {
                spdlog::warn("[gems] {:08X}: looted '{}' is socketed and MEO cannot strip it (ABI < 3): kept as is", a_fol, NameOf(obj));
                return;
            }
            if (MEOBridge::UnsocketItemGems(fol, a_base, a_uid) > 0)
                spdlog::info("[gems] stripped {} gem(s) from '{}' (looted gear)", n, NameOf(obj));
            g_probes.push_back({ a_fol, a_base, a_src, a_uid, Clock::now(), a_sell });
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
                if (p.loose && !p.stripQueued) {
                    std::int32_t v = 0;
                    if (!FindOwned(fol, obj, p.uid, v)) {   // the pickup has not landed (or failed)
                        if (Clock::now() - p.since > kStripPatience) {
                            spdlog::warn("[gems] {:08X}: loose '{}' never arrived in his pack: probe dropped", a_fol, NameOf(obj));
                            g_probes.erase(g_probes.begin() + i);
                        } else ++i;
                        continue;
                    }
                    if (MEOBridge::IsCarriedGemmed(a_fol, p.base, p.uid)) {
                        const int n = MEOBridge::CarriedGemCount(a_fol, p.base, p.uid);
                        if (MEOBridge::UnsocketItemGems(fol, p.base, p.uid) > 0)
                            spdlog::info("[gems] stripped {} gem(s) from '{}' (looted loose gear)", n, NameOf(obj));
                        g_probes[i].stripQueued = true;
                        g_probes[i].hadGems     = true;
                        g_probes[i].since       = Clock::now();
                        ++i;
                        continue;
                    }
                }
                if (!MEOBridge::IsCarriedGemmed(a_fol, p.base, p.uid)) {
                    g_probes.erase(g_probes.begin() + i);
                    Decide(fol, obj, p.uid, p.src, p.sellGambit, p.hadGems, &p);
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

    void LogStatus() {
        const int abi = MEOBridge::AbiVersion();
        if (Active()) {
            spdlog::info("[gems] rules ACTIVE (MEO ABI {}, strip available: {})", abi,
                         MEOBridge::CarriedGemUnsocketSupported() ? "yes" : "no");
        } else if (!Config::g_meoAwareGems.load()) {
            spdlog::info("[gems] rules inactive (bMeoAwareGems is OFF)");
        } else if (abi == 0) {
            spdlog::info("[gems] rules inactive (MEO not present)");
        } else {
            spdlog::info("[gems] rules inactive (MEO ABI {} < 2, cannot see socketed gems)", abi);
        }
    }

    Verdict CheckRemovalCached(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, Reason a_reason,
                               std::vector<HoldKey>* a_touched) {
        return Check(a_actor, a_obj, a_why, false, a_reason, a_touched);
    }
    Verdict CheckRemovalLive(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, Reason a_reason,
                             std::vector<HoldKey>* a_touched) {
        return Check(a_actor, a_obj, a_why, true, a_reason, a_touched);
    }
    void ReleaseHoldsExcept(RE::FormID a_follower, Reason a_reason, const std::vector<HoldKey>& a_touched) {
        MEOBridge::ReleaseHolds(a_follower, static_cast<std::uint8_t>(a_reason), a_touched);
    }

    GemCandidate FindGemCandidate(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
        if (!Active() || !MEOBridge::CarriedGemUnsocketSupported() || !a_follower || !a_src) return {};
        const auto srcInv = InvSnap::Items(a_src);   // INVARIANTS #84: the main-built snapshot, never the live list
        if (!srcInv) { TheftGate::NoteInventoryNotYet(a_follower, a_src); return {}; }   // not yet: retried next tick, the source is never marked done
        for (auto& [obj, data] : srcInv) {
            if (!obj || data.first <= 0) continue;
            auto* e = data.second.get();
            if (!e || !e->hasLists || IsQuestObjectInstance(e)) continue;
            if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
            if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                if (IsCreatureArmor(armo) || Config::g_dollsMode.load()) continue;
            } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                if (IsCreatureWeapon(weap)) continue;
            } else continue;
            if (!FitsCarryWeight(a_follower, obj->GetWeight())) continue;
            for (const auto& c : e->copies) {
                const std::uint16_t uid = c.hasUid ? c.uid : std::uint16_t{ 0 };
                if (uid == 0 || !NamedGemmed(c, obj)) continue;   // only MEO-titled (socketed) instances are worth a take
                if (TheftGate::TakeBarredOwner(a_follower, a_src, c.owner)) continue;   // theft gate: this very copy may be stolen goods
                if (g_probed.contains({ a_src->GetFormID(), obj->GetFormID(), uid })) continue;
                return { obj, c.owner, uid };
            }
        }
        return {};
    }

    bool TakeAndProbe(RE::Actor* a_follower, RE::TESObjectREFR* a_src, const GemCandidate& a_c) {
        if (!a_follower || !a_src || !a_c.obj) return false;
        const RE::FormID fol = a_follower->GetFormID(), base = a_c.obj->GetFormID(), src = a_src->GetFormID();
        const std::uint16_t uid = a_c.uid;
        if (TheftGate::GuardInterim(a_follower, a_src, TheftGate::Road::Gear) ||
            TheftGate::TakeBarredOwner(a_follower, a_src, a_c.copyOwner)) return false;   // theft gate
        g_probed.insert({ src, base, uid });   // never re-taken, whatever the probe decides
        const bool sellGambit = SellGambitActive(a_follower);
        // ON THE WORKER the take is queued to the main thread like every transfer (TheftGate.h, INVARIANTS #83: a corpse AND a
        // container source since fix/worker-writes-main), the copy named by its uid and re-found there; the probe runs right after
        // it lands. 0 = this call is on the main thread already, or there is no live pump: the direct road below.
        if (TheftGate::QueueCopy(a_follower, a_src, a_c.obj, uid, TheftGate::Road::Gear,
                [fol, base, uid, src, sellGambit](std::int32_t) {
                    spdlog::info("[gems] {:08X}: took '{}' from {:08X} to look for gems (on the main thread)", fol,
                                 NameOf(RE::TESForm::LookupByID<RE::TESBoundObject>(base)), src);
                    ProbeOnMain(fol, base, uid, src, sellGambit);
                }) > 0)
            return true;
        // THE DIRECT ROAD: only on the main thread, or with no pump installed (the legacy path). Its own read is LIVE and stays with
        // the write (NoteWriteRead counts a worker tick that reaches it; it must stay 0). The copy is re-found by its uid in the live
        // source right here: an extra-list pointer from a snapshot would be a tick old. The ownership bar is re-read on that list.
        std::int32_t v = 0;
        RE::ExtraDataList* liveXl = nullptr;
        MainThread::NoteWriteRead(__FUNCTION__);   // INVARIANTS #84: the live read of a write made right here (self-counted when off the main thread)
        for (auto& [o, d] : a_src->GetInventory([&](RE::TESBoundObject& x) { return &x == a_c.obj; })) {
            auto* e = d.second.get();
            if (!e || !e->extraLists) continue;
            for (auto* xl : *e->extraLists)
                if (xl && UidOf(xl) == uid) { liveXl = xl; break; }
        }
        if (!liveXl || TheftGate::TakeBarred(a_follower, a_src, a_c.obj, liveXl, TheftGate::Road::Gear)) return false;   // gone, or its owner changed
        MainThread::NoteEngineWrite("GemRules::TakeAndProbe");   // INVARIANTS #83: counted and said if a worker tick ever reaches this
        a_src->RemoveItem(a_c.obj, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, liveXl, a_follower);
        InvSnap::Touch(a_src); InvSnap::Touch(a_follower);   // both snapshots are behind this write
        if (!FindOwned(a_follower, a_c.obj, uid, v)) return false;
        spdlog::info("[gems] {:08X}: took '{}' from {:08X} to look for gems", fol, NameOf(a_c.obj), src);
        const bool sell = sellGambit;
        if (MainThread::IsInstalled()) MainThread::Post([fol, base, uid, src, sell]() { ProbeOnMain(fol, base, uid, src, sell); });
        else                           ProbeOnMain(fol, base, uid, src, sell);   // VR: no pump, same as the other gem reads
        return true;
    }

    void PumpProbes(RE::Actor* a_follower) {
        if (!a_follower || g_probeCount.load() == 0) return;
        const RE::FormID fol = a_follower->GetFormID();
        if (MainThread::IsInstalled()) MainThread::Post([fol]() { PumpOnMain(fol); });
        else                           PumpOnMain(fol);
    }

    namespace {
        // MEO's exact scheme (MEO plugin.cpp RebuildInstanceEnchant): "{namePart} {BaseName}" where
        // namePart = "{Gem} {I-V}" joined by " + ", optionally prefixed "{Support} {I-V} \xC2\xB7 ".
        bool IsRoman(const std::string& t) { return t == "I" || t == "II" || t == "III" || t == "IV" || t == "V"; }
        bool MatchesMeoTitle(std::string a_name, const std::string& a_base) {
            if (a_base.empty() || a_name.size() <= a_base.size() + 1) return false;
            if (a_name.compare(a_name.size() - a_base.size(), a_base.size(), a_base) != 0) return false;
            if (a_name[a_name.size() - a_base.size() - 1] != ' ') return false;
            std::string p = a_name.substr(0, a_name.size() - a_base.size() - 1);
            const std::string mid = " \xC2\xB7 ";
            if (const auto m = p.find(mid); m != std::string::npos) p.replace(m, mid.size(), " + ");
            std::size_t start = 0;
            while (true) {
                const auto e   = p.find(" + ", start);
                const auto seg = p.substr(start, e == std::string::npos ? std::string::npos : e - start);
                const auto sp  = seg.rfind(' ');
                if (sp == std::string::npos || sp == 0 || !IsRoman(seg.substr(sp + 1))) return false;
                if (e == std::string::npos) break;
                start = e + 3;
            }
            return true;
        }
    }

    bool LooseNamedGemmed(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base) {
        if (!Active() || !MEOBridge::CarriedGemUnsocketSupported() || !a_ref || !a_base) return false;
        if (auto* armo = a_base->As<RE::TESObjectARMO>()) { if (IsCreatureArmor(armo) || Config::g_dollsMode.load()) return false; }
        else if (auto* weap = a_base->As<RE::TESObjectWEAP>()) { if (IsCreatureWeapon(weap)) return false; }
        else return false;
        auto* uid = a_ref->extraList.GetByType<RE::ExtraUniqueID>();
        auto* txt = a_ref->extraList.GetByType<RE::ExtraTextDisplayData>();
        if (!uid || uid->uniqueID == 0 || !txt) return false;
        if (g_looseTaken.contains({ a_base->GetFormID(), uid->uniqueID })) return false;
        if (!NamedGemmed(&a_ref->extraList, a_base)) return false;
        if (g_looseRefs.insert(a_ref->GetFormID()).second)
            spdlog::info("[gems] loose '{}' recognised as gemmed by name", txt->displayName.c_str());
        return true;
    }

    // IAUS looting replaces the act.loot_valuables row (review F6): its valuables verdict TAKE stands in for the row.
    bool SellGambitActive(RE::Actor* a_follower) {
        return g_svc && (TableHasAction(g_svc->logistics(), Vocab::kActLootValuables) || IausLootValuablesTake(a_follower, *g_svc));
    }

    bool NoteLoosePickup(RE::TESObjectREFR* a_ref) {
        if (!a_ref || !g_looseRefs.contains(a_ref->GetFormID())) return false;
        auto* uid = a_ref->extraList.GetByType<RE::ExtraUniqueID>();
        auto* b   = a_ref->GetBaseObject();
        if (!uid || !b) return false;
        g_looseTaken.insert({ b->GetFormID(), uid->uniqueID });
        g_looseRefs.erase(a_ref->GetFormID());
        return true;
    }

    LooseCapture CaptureLoose(RE::TESObjectREFR* a_ref) {
        LooseCapture c;
        if (!a_ref) return c;
        auto* uid = a_ref->extraList.GetByType<RE::ExtraUniqueID>();
        auto* b   = a_ref->GetBaseObject();
        c.base = b ? b->GetFormID() : 0;
        c.uid  = uid ? uid->uniqueID : 0;
        c.pos  = a_ref->GetPosition();
        c.rot  = a_ref->GetAngle();
        return c;
    }

    void BeginLooseProbe(RE::Actor* a_follower, const LooseCapture& a_cap, bool a_sellGambit) {
        if (!a_follower || a_cap.base == 0 || a_cap.uid == 0) return;
        Probe p{ a_follower->GetFormID(), a_cap.base, 0, a_cap.uid, Clock::now(), a_sellGambit };
        p.loose = true; p.stripQueued = false; p.hadGems = false; p.pos = a_cap.pos; p.rot = a_cap.rot;
        g_probes.push_back(p);
        g_probeCount.store(static_cast<int>(g_probes.size()));
    }

    void ClearTransient() {
        g_looseRefs.clear();
        g_looseTaken.clear();
        g_probed.clear();
        g_probes.clear();
        g_probeCount.store(0);
    }
}
