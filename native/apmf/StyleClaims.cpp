// apmf/StyleClaims.cpp -- T2 batch M item 5: MFO's combat-style desire as a Harbinger ch.33 claim (kIntent_CombatStyle,
// ABI v45). MFO no longer writes CombatController::combatStyle itself when Harbinger serves this facet.
// MFO decides POLICY only: CombatStyle.cpp holds, per follower, the style its stance machinery wants (the combat table's
// stance, the class package's CSTY, the OOC substrate cast pair's bit). This file turns that desire into ONE standing claim
// per follower:
//   * the equipped CLASS's own combat style (class tree ON, classes/Package.cpp)  -> ACTOR-WIDE (ival 0): every read of his
//     style answers it. This is the strict class style: movement, attack, power attack, block and bash follow the class.
//   * MFO's stance styles (MFO_MeleeStyle / MFO_MeleeSingleStyle / MFO_RangedStyle / MFO_CastStyle) with no class style, and
//     the substrate cast pair's MFO_CastStyle -> kCombatStyle_EquipChoiceOnly: the controller's cached copy alone, which is
//     exactly the reach the old direct write had (the equipment and spell choice).
// Harbinger answers the style at the engine's own read and has the ENGINE refresh the controller's copy at engage and
// release; nothing is written to the controller by anyone.
// NO DODGE (marth 2026-11-12, Q-S-3): a CLASS style (actor-wide) is swapped for its zero-avoid TWIN (Classes::NoDodgeTwin, the
// same CSTY with Avoid Threat 0, MFO_GenerateESP.py) while the follower WEARS HEAVY ARMOR, whatever his class: the rule is his
// BODY slot (kBody, the cuirass), the one piece that is the armor class, and its vanilla armor type kHeavyArmor (list-agnostic, no
// keyword or form id), read on the MAIN thread by a posted task about every 2 s, never on this worker. A changed style is the
// existing Repoint. MFO styles (EquipChoiceOnly) carry no dodge and are never swapped. (Living Legend's "dodge only on an attack
// his auto-block passive did not stop" is card H8's requirement, class-build-plan.md; nothing here ties dodge to a guard.)
// Docs: Harbinger Docs/FACETS.md "ch.33 Combat style", APMF_API.h "ABI v45: COMBAT STYLE", INVARIANTS #0 (n).
// DEGRADE (a documented contract, never a mask): Harbinger ABSENT, or older than ABI 45 = CombatStyle::ApplyTick keeps
// writing the controller's field itself, as before. Harbinger v45+ present but REFUSING the claim (its seats are not
// installed, [CombatStyle] bCombatStyle=0) = no style is applied, said in the log, and no MFO-side write stands in.
// Threads: the pump's AddTask worker (SweepStyleClaims from APMFBridge::Tick), the dismissal road (ReleaseStyleClaim) and
// ClearStyleClaims in the drained-pump load window; its OWN mutex g_styleMx, never g_mx. Lock order: CombatStyle's own mutex
// is taken and dropped inside SnapshotStyleDesires BEFORE g_styleMx; never the reverse.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "CombatStyle.h"
#include "MainThread.h"
#include "classes/Classes.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kStyleMinAbi = 45;   // the ABI that first serves intent 33
        // Same never-live floor and retry backoff as the sibling tables (~2 s / ~30 s of 133 ms sweeps).
        constexpr std::uint32_t kNeverLiveSweeps = 15;
        constexpr std::uint32_t kRetrySweeps     = 225;

        struct StyleClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;
            RE::FormID       style          = 0;
            std::uint32_t    flags          = 0;      // CombatStyleFlags
            std::uint32_t    unpausedSweeps = 0;
            std::uint32_t    retryIn        = 0;      // > 0: do not re-file yet (sweeps)
            bool             everLive       = false;
        };
        std::mutex                                 g_styleMx;
        std::unordered_map<RE::FormID, StyleClaim> g_styles;   // guarded by g_styleMx

        const APMF_API::APMF_API_v6* StyleApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kStyleMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v6*>(api);
        }

        bool StyleGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        // Heavy body armor, per follower: written by a MainThread::Post'd read (the worker never touches the actor's biped data),
        // read by the sweep. Its OWN mutex g_heavyMx, a leaf (nothing is called under it). known=false until the first read lands:
        // the sweep files no class-style claim for him before that, so a heavy-armored follower never starts out dodging.
        constexpr std::uint32_t kHeavyRefreshSweeps = 15;   // ~2 s of 133 ms sweeps
        struct HeavyState { bool known = false, heavy = false, pending = false; std::uint32_t wait = 0; };
        std::mutex                                  g_heavyMx;
        std::unordered_map<RE::FormID, HeavyState>  g_heavy;   // guarded by g_heavyMx

        // MAIN THREAD only.
        bool WearsHeavyBody(RE::FormID a_fid) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* body  = actor ? actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody) : nullptr;
            return body && body->GetArmorType() == RE::BGSBipedObjectForm::ArmorType::kHeavyArmor;
        }

        // Sweep side: the follower's heavy state (false+known when the main-thread pump is not live: VR, where the style hooks are
        // refused anyway), and the posting of a refresh when due. Returns false while the first answer is outstanding.
        bool HeavyKnown(RE::FormID a_fid, bool& a_heavy) {
            if (!MainThread::IsInstalled()) {
                static std::atomic<bool> s_said{ false };
                if (!s_said.exchange(true))
                    spdlog::warn("[wstyle] the main-thread pump is not installed (VR): the heavy-armor rule cannot read worn armor and "
                                 "answers 'not heavy' for everyone");
                a_heavy = false;
                return true;
            }
            bool post = false, known = false;
            {
                std::scoped_lock lk(g_heavyMx);
                auto& h = g_heavy[a_fid];
                if (!h.pending) {
                    if (h.wait > 0) --h.wait;
                    else { h.pending = true; post = true; }
                }
                a_heavy = h.heavy;
                known   = h.known;
            }
            if (post) {
                MainThread::Post([a_fid]() {
                    const bool heavy = WearsHeavyBody(a_fid);
                    std::scoped_lock lk(g_heavyMx);
                    auto& h = g_heavy[a_fid];
                    if (h.known && heavy != h.heavy)
                        spdlog::info("[wstyle] {:08X}: {} heavy body armor -> ch.33 style {}", a_fid, heavy ? "now wears" : "no longer wears",
                                     heavy ? "NO-DODGE twin" : "class style");
                    h.known = true; h.heavy = heavy; h.pending = false; h.wait = kHeavyRefreshSweeps;
                });
            }
            return known;
        }

        const char* ScopeName(std::uint32_t a_flags) {
            return (a_flags & APMF_API::kCombatStyle_EquipChoiceOnly) ? "equip choice only" : "actor-wide (class style)";
        }

        void ReleaseLocked(const APMF_API::APMF_API_v6* a_api, RE::FormID a_fid, StyleClaim& a_c, const char* a_why) {
            if (a_api && a_c.handle != APMF_API::kInvalidHandle) a_api->Release(a_c.handle);
            spdlog::info("[wstyle] {:08X}: ch.33 combat style claim released ({}, h={}, style {:08X})", a_fid, a_why, a_c.handle, a_c.style);
            a_c.handle         = APMF_API::kInvalidHandle;
            a_c.style          = 0;
            a_c.flags          = 0;
            a_c.everLive       = false;
            a_c.unpausedSweeps = 0;
        }
    }

    bool StyleRoadSupported() { return StyleApi() != nullptr; }

    void ReleaseStyleClaim(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_styleMx);
            const auto it = g_styles.find(a_follower);
            if (it == g_styles.end()) return;
            h = it->second.handle;
            g_styles.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        if (h != APMF_API::kInvalidHandle) spdlog::info("[wstyle] {:08X}: ch.33 combat style claim released ({})", a_follower, a_why);
    }

    void ClearStyleClaims() {
        std::scoped_lock lock(g_styleMx);
        { std::scoped_lock hl(g_heavyMx); g_heavy.clear(); }   // a posted read is dropped by MainThread::Clear on the same boundary
        g_styles.clear();   // Harbinger drops every claim at its own load boundary; a stale Release would be a no-op
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_styleMx itself; g_mx NOT held.
    void SweepStyleClaims() {
        const auto* api = StyleApi();
        if (!api) return;   // absent or older than v45: CombatStyle::ApplyTick writes the field itself (the degrade)

        // What CombatStyle.cpp wants, read BEFORE g_styleMx (its own mutex is taken and dropped inside).
        std::vector<CombatStyle::StyleDesire> desires;
        CombatStyle::SnapshotStyleDesires(desires);
        struct Want { RE::FormID fid; RE::FormID style; std::uint32_t flags; };
        std::vector<Want> wants;
        std::vector<RE::FormID> heavyPending;   // class-style followers whose first heavy-armor read is still outstanding
        wants.reserve(desires.size());
        for (const auto& d : desires) {
            // The two owners never mix (CombatStyle.h): a substrate-only controller takes the cast pair's style, a real
            // fight (or no fight yet) the combat table's.
            if (SubstrateOnly(d.follower)) {
                if (d.substrateStyle != 0) wants.push_back({ d.follower, d.substrateStyle, APMF_API::kCombatStyle_EquipChoiceOnly });
            } else if (d.tableStyle != 0) {
                RE::FormID style = d.tableStyle;
                if (d.tableWide) {   // a class style: the no-dodge switch (see the file header)
                    bool heavy = false;
                    if (!HeavyKnown(d.follower, heavy)) { heavyPending.push_back(d.follower); continue; }   // first read outstanding: no NEW claim; an existing one stands (step 1 skips it)
                    if (heavy) style = Classes::NoDodgeTwin(style);
                }
                wants.push_back({ d.follower, style, d.tableWide ? 0u : APMF_API::kCombatStyle_EquipChoiceOnly });
            }
        }
        const bool paused = StyleGamePaused();

        std::scoped_lock lock(g_styleMx);
        // 1. Claims nobody wants any more (the stance was cleared: combat end, dismissal, the cast pair closed).
        for (auto it = g_styles.begin(); it != g_styles.end();) {
            bool wanted = false;
            for (const Want& w : wants) wanted = wanted || w.fid == it->first;
            for (const RE::FormID f : heavyPending) wanted = wanted || f == it->first;
            if (!wanted) {
                if (it->second.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, it->first, it->second, "no stance owns his style");
                it = g_styles.erase(it);
            } else ++it;
        }
        // 1b. A follower with no want and no read outstanding is out of the fight: forget his heavy-armor state, so his next
        //     fight waits for a fresh read (g_styleMx -> g_heavyMx, g_heavyMx is a leaf).
        {
            std::scoped_lock hl(g_heavyMx);
            for (auto& [fid, h] : g_heavy) {
                bool live = false;
                for (const Want& w : wants) live = live || w.fid == fid;
                for (const RE::FormID f : heavyPending) live = live || f == fid;
                if (!live) { h.known = false; h.wait = 0; }
            }
        }
        // 2. File, or re-point on a handoff (melee <-> ranged, a class style arriving, the scope changing).
        for (const Want& w : wants) {
            auto& c = g_styles[w.fid];
            APMF_API::APMF_Param prm{};
            prm.form = w.style;
            prm.ival = static_cast<std::int32_t>(w.flags);
            if (c.handle != APMF_API::kInvalidHandle) {
                if (c.style == w.style && c.flags == w.flags) continue;
                api->Repoint(c.handle, &prm);
                spdlog::info("[wstyle] {:08X}: ch.33 combat style HANDOFF {:08X} -> {:08X} ({}, h={})", w.fid, c.style, w.style,
                             ScopeName(w.flags), c.handle);
                c.style = w.style;
                c.flags = w.flags;
                continue;
            }
            if (c.retryIn > 0) { if (!paused) --c.retryIn; continue; }
            const APMF_API::Handle h = api->RequestEx(w.fid, APMF_API::kIntent_CombatStyle, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                // Never a mask: no MFO-side write stands in for a refused claim. Said once per follower per ~30 s.
                c.retryIn = kRetrySweeps;
                spdlog::warn("[wstyle] {:08X}: Harbinger REFUSED the ch.33 combat style claim at the call (style {:08X}, {}): its seats "
                             "are not installed or the request is malformed (APMF's '[ch.33] claim refused' line names it). No combat "
                             "style is applied for him; re-tried in ~30 s.", w.fid, w.style, ScopeName(w.flags));
                continue;
            }
            c.handle         = h;
            c.style          = w.style;
            c.flags          = w.flags;
            c.everLive       = false;
            c.unpausedSweeps = 0;
            spdlog::info("[wstyle] {:08X}: ch.33 combat style claim FILED (h={}, style {:08X}, {})", w.fid, h, w.style, ScopeName(w.flags));
        }
        // 3. Liveness: never-live and ended-by-Harbinger claims are released and re-tried after a backoff.
        for (auto& [fid, c] : g_styles) {
            if (c.handle == APMF_API::kInvalidHandle) continue;
            if (api->IsClaimLive(c.handle)) { c.everLive = true; continue; }
            if (!c.everLive) {
                if (!paused) ++c.unpausedSweeps;
                if (c.unpausedSweeps <= kNeverLiveSweeps) continue;
                spdlog::warn("[wstyle] {:08X}: ch.33 combat style claim NEVER WENT LIVE (h={}, {} unpaused sweeps; he is probably not "
                             "loaded) -- released, re-tried in ~30 s. APMF's log says why if it refused.", fid, c.handle, c.unpausedSweeps);
            } else {
                spdlog::info("[wstyle] {:08X}: ch.33 combat style claim ENDED BY HARBINGER (h={}: he unloaded, or an outranking claim "
                             "took him) -- re-tried in ~30 s.", fid, c.handle);
            }
            ReleaseLocked(api, fid, c, "not live");
            c.retryIn = kRetrySweeps;
        }
    }
}
