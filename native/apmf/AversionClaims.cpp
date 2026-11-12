// apmf/AversionClaims.cpp -- the ch.32 AVERSION claim table (Harbinger ABI v41, kIntent_Aversion): MFO's client side of class
// card H2, "less likely to be targeted" (marth, class walkthrough: Conjurer "a low chance of being targeted by foes", Healer "same
// shape as Conjurer"). A new claim family, so its own file in apmf/; the shape mirrors apmf/AimClaims.cpp.
//
// WHAT MFO DECLARES (policy only; the mechanism is Harbinger's). For every managed follower whose class state carries the Conjurer
// or the Healer PASSIVE, ONE standing claim whose param.fval is his aversion weight in (0, 1). Harbinger then, when a foe's own
// target selection lands on him, has the engine pick its next best valid target instead in that share of time (6 s windows); with
// no other valid target he is picked as always. MFO never touches a foe, a target or a combat group: it only says WHO and HOW MUCH.
// The weight follows the class passive rules (classes/Carriers.cpp CarrierRanks, the same ranks the passive perks are gated on):
//   the class EQUIPPED (any level; the weight does not grow with level)   the class weight   fAversionConjurer / fAversionHealer (0.25)
//   the class MASTERED and not equipped                                    nothing            (marth 11-12, Q94: equipped class only)
//   the class MASTERED while Dragonborn is equipped                        the class weight   (CarrierRanks: Dragonborn holds every mastered passive at full; Q107)
//   both passives present                                            the larger of the two, never a sum
// The stronger tiers (Necromancer, Priest, Arch-Mage, Saint) get their own weights with their own cards; until then they carry
// what the rules above give them through an equipped Conjurer / Healer.
//
// LIFETIME: filed by the pump's sweep when the follower is managed, alive, the class add-on is active (Classes::Active: the ESL
// present and bClassTree on), bClassAversion is on and his weight is above zero; re-filed when the weight changes (class equip,
// mastery, INI); released when the weight drops to zero (class unequipped, classes OFF for him), when he dies, on dismissal
// (Followers::ReleaseHeldState), when the add-on goes inactive or bClassAversion goes off, and by ClearTransientState (load /
// revert). A claim Harbinger ENDS (he unloaded) or that never goes live is released and re-tried after a backoff, never in a loop.
// Harbinger absent / older than v41 / the seats refused = no claim, logged ONCE: foes select targets as the engine always did.
// NOTHING IS SAVED. Engine effects: none on MFO's side.
// Threads: the pump's AddTask worker for everything but ClearAversionClaims (the drained-pump load window); its OWN mutex, never
// g_mx (same lock-order note as apmf/AimClaims.cpp). The class State reads (GetEquipped / GetMastered) happen BEFORE g_avMx is taken.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"           // ActiveSnapshot(): the managed followers, any thread
#include "classes/Classes.h"    // Classes::Active(), GetEquipped(), GetMastered(), CarrierRanks(), Id

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kAversionMinAbi = 41;   // the ABI that first serves intent 32
        constexpr RE::FormID    kPlayerId       = 0x14;
        // Harbinger takes a weight strictly inside (0, 1): the class weight is kept inside these.
        constexpr float kMinWeight = 0.05f;
        constexpr float kMaxWeight = 0.95f;

        // Same never-live floor and retry backoff as the sibling tables (~30 s of 133 ms sweeps).
        constexpr std::uint32_t kNeverLiveSweeps = 15;
        constexpr std::uint32_t kRetrySweeps     = 225;

        struct AvClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;
            float            weight         = 0.0f;
            std::uint32_t    unpausedSweeps = 0;
            std::uint32_t    retryIn        = 0;      // > 0: do not re-file yet (sweeps)
            bool             everLive       = false;
        };
        std::mutex                              g_avMx;
        std::unordered_map<RE::FormID, AvClaim> g_avs;   // guarded by g_avMx

        std::atomic<bool> g_avSeatRefused{ false };   // session-stable, like the sibling tables
        std::atomic<bool> g_avAbsentLogged{ false };

        const APMF_API::APMF_API_v6* AversionApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kAversionMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v6*>(api);
        }

        bool AvGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        // One class passive's share of its INI weight (the equipped class only: a mastered, unequipped one gives none), from the passive rank (classes/Carriers.cpp rank space).
        float Share(std::uint8_t a_rank, float a_classWeight) {
            if (a_rank == 0 || a_rank == Classes::kRankMasteredUnequipped || !(a_classWeight > 0.0f)) return 0.0f;   // equipped class only (Q94)
            return a_classWeight;
        }

        // The follower's aversion weight by the class passive rules. 0 = none. Two cheap State reads (each takes the class State
        // mutex; never the whole record with its stored tables, this runs every sweep) fed to the PURE rank rule. The two reads
        // are not one snapshot: a class switch between them is corrected on the next sweep.
        float WeightFor(RE::FormID a_fid) {
            Classes::FollowerClasses fc;
            fc.equipped = Classes::GetEquipped(a_fid);
            if (fc.equipped == Classes::kNoClass) return 0.0f;   // classes OFF for him: nothing applies
            fc.mastered = Classes::GetMastered(a_fid);
            const auto ranks = Classes::CarrierRanks(fc);        // reads equipped, mastered and the equipped class's level (any level = active)
            const float w = std::max(
                Share(ranks[static_cast<std::size_t>(Classes::Id::Conjurer)], Config::g_aversionConjurer.load(std::memory_order_relaxed)),
                Share(ranks[static_cast<std::size_t>(Classes::Id::Healer)], Config::g_aversionHealer.load(std::memory_order_relaxed)));
            if (!(w > 0.0f)) return 0.0f;
            return std::clamp(w, kMinWeight, kMaxWeight);
        }

        void ReleaseLocked(const APMF_API::APMF_API_v6* a_api, RE::FormID a_fid, AvClaim& a_c, const char* a_why) {
            if (a_api && a_c.handle != APMF_API::kInvalidHandle) a_api->Release(a_c.handle);
            spdlog::info("[aversion] {:08X}: ch.32 aversion claim released ({}, h={}, weight {:.2f})", a_fid, a_why, a_c.handle, a_c.weight);
            a_c.handle         = APMF_API::kInvalidHandle;
            a_c.weight         = 0.0f;
            a_c.everLive       = false;
            a_c.unpausedSweeps = 0;
        }

        bool FileLocked(const APMF_API::APMF_API_v6* a_api, RE::FormID a_fid, AvClaim& a_c, float a_weight) {
            APMF_API::APMF_Param prm{};
            prm.fval                 = a_weight;
            const APMF_API::Handle h = a_api->RequestEx(a_fid, APMF_API::kIntent_Aversion, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_avSeatRefused.exchange(true))
                    spdlog::warn("[aversion] Harbinger REFUSED the ch.32 aversion claim for {:08X} at the call: its seats are not "
                                 "installed (VR, a runtime other than 1.6.1170 / 1.5.97 / 1.7.104, [Aversion] bAversion=0 in APMF.ini, "
                                 "or its address self-check; APMF's '[ch.32] claim refused' line names it). No class aversion this "
                                 "session: foes select targets as the engine always did.", a_fid);
                return false;
            }
            a_c.handle         = h;
            a_c.weight         = a_weight;
            a_c.everLive       = false;
            a_c.unpausedSweeps = 0;
            a_c.retryIn        = 0;
            spdlog::info("[aversion] {:08X}: ch.32 aversion claim FILED (h={}, weight {:.2f}: a foe whose selection lands on him picks "
                         "its next best valid target instead in that share of time)", a_fid, h, a_weight);
            return true;
        }
    }

    void ReleaseAversionClaim(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_avMx);
            const auto it = g_avs.find(a_follower);
            if (it == g_avs.end()) return;
            h = it->second.handle;
            g_avs.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        if (h != APMF_API::kInvalidHandle) spdlog::info("[aversion] {:08X}: ch.32 aversion claim released ({})", a_follower, a_why);
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_avMx itself; g_mx NOT held.
    void SweepAversionClaims() {
        const bool wanted = Config::g_classAversion.load(std::memory_order_relaxed) && Classes::Active();
        const auto* api   = AversionApi();
        const auto  snap  = Followers::ActiveSnapshot();

        // Read every follower's weight and life BEFORE g_avMx (the class State mutex and a form lookup stay outside it).
        struct Want { RE::FormID fid; float weight; bool dead; };
        std::vector<Want> wants;
        if (wanted) {
            wants.reserve(snap->size());
            for (const RE::FormID fid : *snap) {
                if (fid == 0 || fid == kPlayerId) continue;
                const float w = WeightFor(fid);
                bool dead = false;
                if (w > 0.0f) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
                    dead    = a && a->IsDead();
                }
                wants.push_back({ fid, w, dead });
            }
        }
        if (!api) {
            // Harbinger absent or older than v41: nothing was ever filed. Say so once, and only when a follower would claim.
            const bool any = std::any_of(wants.begin(), wants.end(), [](const Want& a_w) { return a_w.weight > 0.0f; });
            if (any && !g_avAbsentLogged.exchange(true)) {
                const auto* base = g_apmf.load(std::memory_order_relaxed);
                if (base)
                    spdlog::warn("[aversion] Harbinger ABI {} is older than {}: the Conjurer / Healer 'less likely to be targeted' "
                                 "passive does nothing this session (no MFO-side stand-in; update Harbinger).", base->abiVersion,
                                 kAversionMinAbi);
                else
                    spdlog::warn("[aversion] Harbinger is absent: the Conjurer / Healer 'less likely to be targeted' passive does "
                                 "nothing this session (no MFO-side stand-in).");
            }
            return;
        }
        const bool on     = wanted && !g_avSeatRefused.load(std::memory_order_relaxed);
        const bool paused = AvGamePaused();
        std::scoped_lock lock(g_avMx);
        if (!on) {
            for (auto& [fid, c] : g_avs)
                if (c.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, fid, c, "class aversion off / class add-on inactive");
            return;
        }
        // 1. The managed followers: file, re-file on a weight change, release at weight zero or death.
        for (const Want& w : wants) {
            auto& c = g_avs[w.fid];
            if (!(w.weight > 0.0f) || w.dead) {
                if (c.handle != APMF_API::kInvalidHandle)
                    ReleaseLocked(api, w.fid, c, w.dead ? "he is dead" : "no Conjurer / Healer passive (class unequipped or classes off)");
                continue;
            }
            if (c.handle != APMF_API::kInvalidHandle) {
                if (std::fabs(c.weight - w.weight) < 0.005f) continue;
                ReleaseLocked(api, w.fid, c, "weight changed (class, mastery or INI)");
            }
            if (c.retryIn > 0) { if (!paused) --c.retryIn; continue; }
            if (!FileLocked(api, w.fid, c, w.weight)) return;   // refused at the call: session-stable, stop here
        }
        // 2. Claims for followers no longer managed.
        for (auto it = g_avs.begin(); it != g_avs.end();) {
            const bool managed = std::find(snap->begin(), snap->end(), it->first) != snap->end();
            if (!managed) {
                if (it->second.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, it->first, it->second, "not managed");
                it = g_avs.erase(it);
            } else ++it;
        }
        // 3. Liveness: never-live and ended-by-Harbinger claims are released and re-tried after a backoff.
        for (auto& [fid, c] : g_avs) {
            if (c.handle == APMF_API::kInvalidHandle) continue;
            if (api->IsClaimLive(c.handle)) { c.everLive = true; continue; }
            if (!c.everLive) {
                if (!paused) ++c.unpausedSweeps;
                if (c.unpausedSweeps <= kNeverLiveSweeps) continue;
                spdlog::warn("[aversion] {:08X}: ch.32 aversion claim NEVER WENT LIVE (h={}, {} unpaused sweeps; he is probably not "
                             "loaded) -- released, re-tried in ~30 s. APMF's log says why if it refused.",
                             fid, c.handle, c.unpausedSweeps);
            } else {
                spdlog::info("[aversion] {:08X}: ch.32 aversion claim ENDED BY HARBINGER (h={}: he unloaded, or an outranking claim "
                             "took him) -- re-tried in ~30 s.", fid, c.handle);
            }
            ReleaseLocked(api, fid, c, "not live");
            c.retryIn = kRetrySweeps;
        }
    }

    void ClearAversionClaims() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_avMx);
        for (auto& [fid, c] : g_avs)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_avs.clear();
    }

#ifdef MFO_DEV_SELFTEST
    // DEV BUILDS ONLY (selftest/TestsT2.cpp): read-only windows, see APMFBridge.h. Take g_avMx only for the claim read; no engine call under it.
    std::uint32_t DevAbi() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        return api ? api->abiVersion : 0u;
    }
    DevAversionInfo DevReadAversion(RE::FormID a_follower) {
        DevAversionInfo out;
        const auto* api = AversionApi();
        out.seatRefused = g_avSeatRefused.load(std::memory_order_relaxed);
        if (!api) return out;
        out.api = true;
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_avMx);
            const auto it = g_avs.find(a_follower);
            if (it != g_avs.end() && it->second.handle != APMF_API::kInvalidHandle) { h = it->second.handle; out.weight = it->second.weight; }
        }
        out.live = h != APMF_API::kInvalidHandle && api->IsClaimLive(h);
        return out;
    }
    DevServeInfo DevCanServeIntent(RE::FormID a_actor, std::uint32_t a_intent, float a_fval, RE::FormID a_form) {
        DevServeInfo out;
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < 35) return out;
        out.api = true;
        APMF_API::APMF_Param p{};
        p.fval = a_fval;
        p.form = a_form;
        APMF_API::APMF_ServeQuery q{};
        q.size   = sizeof(q);
        q.call   = APMF_API::kServeCall_RequestEx;
        q.actor  = a_actor;
        q.intent = a_intent;
        q.basis  = kOwnBasis;
        q.param  = &p;
        APMF_API::APMF_ServeInfo info{};
        info.size = sizeof(info);
        out.verdict = reinterpret_cast<const APMF_API::APMF_API_v35*>(api)->CanServe(&q, &info);
        out.refusal = info.refusal;
        info.why[sizeof(info.why) - 1] = '\0';
        out.why = info.why;
        return out;
    }
#endif

}
