#include "PCH.h"
#include "ComposedCast.h"
#include "Config.h"
#include "APMFBridge.h"
#include "CastBounds.h"
#include "MainThread.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

// See ComposedCast.h for the full design + the OBSERVE-AND-REPLICATE steering.
// This TU is the executor scaffold; the ONE trigger seam (DriveObservedCast) is
// isolated and currently DEGRADES, so the whole module is runtime-inert until the
// observed NPC-cast sequence is captured and that seam is filled in.
namespace MFO::ComposedCast {

    namespace {
        using Clock = std::chrono::steady_clock;

        // ── Bounds windows by kind (SPEC-FORCED-CAST.md §1.4) ───────────────────
        // What CastBounds::Arm registers AND what the APMF claim's ttlMs carries;
        // both auto-expire so a crashed phase never leaves a standing hold.
        std::uint32_t TtlMsFor(CasterConsent::SpellKind a_kind) {
            switch (a_kind) {
            case CasterConsent::SpellKind::Heal:    return 6000;   // heal: 6 s / until topped
            case CasterConsent::SpellKind::Offense: return 4000;   // hostile stream: 1-4 s cap
            default:                                return 4000;   // buff / utility hold
            }
        }

        // Executor-held streams (worker-serial, like Actuation's g_selfCast, #4). An
        // entry exists only while the executor OWNS an animated cast on that
        // follower -- empty today (the trigger seam never arms yet).
        struct StreamRec { RE::FormID spell = 0; RE::FormID proxy = 0; RE::FormID target = 0; };
        std::unordered_map<RE::FormID, StreamRec> g_streams;   // worker-serial

        // Per-follower DEGRADE backoff (§1.6): after the executor degrades to
        // kInstant for a follower, do not re-attempt it for kCfcBackoffMs. This is
        // both the spec's "don't hammer" rule AND -- while the trigger seam is a
        // stub that always degrades -- what keeps an opt-in-ON experimental profile
        // from churning an APMF claim every beat. Worker-serial (Try runs on the
        // worker); cleared on Reset.
        std::unordered_map<RE::FormID, Clock::time_point> g_backoff;   // worker-serial

        // "Expected cast" hand-off to Diagnostics::SpellSink. The drive arms an
        // (actor, spell) just before it fires; the sink reports a match as the
        // ANIMATED path and clears it. Guarded by a leaf mutex (sink + drive both
        // run on the game thread, but the guard makes it trivially safe regardless).
        std::mutex g_expectMx;
        std::unordered_set<std::uint64_t> g_expect;

        inline std::uint64_t Key(RE::FormID a_actor, RE::FormID a_spell) {
            return (static_cast<std::uint64_t>(a_actor) << 32) | a_spell;
        }

        // Master gate: AE-only (mirrors CastSelfDirect #67); needs APMF present (the
        // deny keeps the AI/other frameworks off the hand -- without it "legacy =
        // APMF-absent-only" means the silent kInstant heal, never a half-composed
        // one); opt-in behind the (repurposed) bHealAnimPackage toggle, default OFF.
        bool Enabled(RE::Actor* a_follower, RE::SpellItem* a_spell) {
            if (!a_follower || !a_spell)           return false;
            if (!REL::Module::IsAE())              return false;   // SE/VR -> kInstant
            if (!Config::g_healAnimPackage.load()) return false;   // opt-in, default OFF
            if (!APMFBridge::Available())          return false;   // APMF absent -> kInstant
            return true;
        }

        // ── THE TRIGGER SEAM (steering 2026-09-04) ──────────────────────────────
        // The ONE isolated point that turns an ARMED cast into a REAL animated cast.
        // It will REPLICATE the engine's own NPC full-animation cast sequence -- the
        // MagicCaster state machine, the animation-graph cast events, and the
        // charge/fire boundary -- exactly as captured by the parallel APMF passive
        // observer at the 0xAD seat from a deck cycle. It runs on the MAIN thread
        // (MainThread::Post from here): mint/reuse a DEDICATED delivery-flipped proxy
        // (lift feat/heal-anim-proxy's AcquireHeal/FreeHeal pool -- NEVER shared with
        // the kInstant stream pool, NEVER AddSpell'd to the actor), equip it into
        // hand H, set desiredTarget, CastBounds::Arm the proxy key too, then drive
        // the observed input sequence; on the fire event hand off via NoteObservedCast.
        //
        // The spec's originally-guessed trigger (a hand-built TESActionData::Process
        // with ActionRightAttack/Release) is DELIBERATELY NOT USED -- it was unproven
        // and version-fragile; observe-and-replicate removes the guess.
        //
        // TODO(observed-cast): implement per the captured sequence, then return
        // kArmed once the cast is committed (state left kNone). Any non-kArmed return
        // makes Try() degrade to kInstant (a heal always lands).
        enum class DriveResult { kNotImplemented, kArmed, kFailed };

        DriveResult DriveObservedCast(RE::Actor* /*a_caster*/, RE::SpellItem* /*a_castForm*/,
                                      RE::Actor* /*a_target*/, bool /*a_leftHand*/) {
            // UNIMPLEMENTED: awaiting the observer's captured sequence. Reads the
            // reserved trigger selector (a runtime atomic, never < 0) so the return
            // is not a compile-time constant -- that keeps Try()'s "armed" success
            // branch reachable for the compiler (no /WX unreachable-code warning)
            // until this seam is filled in.
            if (Config::g_forcedCastTrigger.load() < 0) return DriveResult::kFailed;   // never today
            return DriveResult::kNotImplemented;
        }
    }

    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind) {
        if (!Enabled(a_follower, a_spell)) return false;   // -> caller's kInstant apply

        const RE::FormID fid = a_follower->GetFormID();
        const auto       now = Clock::now();

        // §1.6 degrade backoff: recently degraded for this follower -> stay on
        // kInstant (don't hammer the executor / churn the APMF claim).
        if (auto it = g_backoff.find(fid); it != g_backoff.end()) {
            if (now - it->second < std::chrono::milliseconds(Config::g_cfcBackoffMs.load()))
                return false;
            g_backoff.erase(it);
        }

        // ── COMPOSE ─────────────────────────────────────────────────────────────
        const RE::FormID spellID  = a_spell->GetFormID();
        const bool       selfCast = (a_target == nullptr) || (a_target == a_follower);
        const RE::FormID targetID = selfCast ? 0 : a_target->GetFormID();
        const std::uint32_t ttlMs = TtlMsFor(a_kind);

        auto degrade = [&](const char* a_why) -> bool {
            {
                std::scoped_lock lk(g_expectMx);
                g_expect.erase(Key(fid, spellID));
            }
            CastBounds::Disarm(fid);
            APMFBridge::ReleaseCast(fid);
            g_backoff[fid] = now;
            spdlog::info("[cfc] {:08X} degraded ({}) -- kInstant apply", fid, a_why);
            return false;
        };

        // 2. CLAIM the APMF cast-execution facet: deny the follower's own casting/
        //    re-arm + the cast leaves for the window (keep the AI off the hand);
        //    MOVEMENT is never claimed. Refused / APMF too old (v4) -> degrade. (The
        //    proxy FormID is minted main-thread inside the drive, so the claim
        //    carries proxy=0 here; the drive re-arms bounds with the proxy key.)
        APMFBridge::CastReq req;
        req.spell  = spellID;
        req.proxy  = 0;
        req.target = targetID;
        req.flags  = (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
                         ? APMFBridge::CastReqFlag_Concentration : 0u;
        req.ttlMs  = ttlMs;
        if (!APMFBridge::ClaimCast(fid, req)) return degrade("claim refused");

        // 3. BOUND (before the hand is touched): register (actor, spell) as an
        //    MFO-executed bounded cast so CasterConsent early-passes it (§2 -- the
        //    HARD-ABORT fix). The drive adds the proxy key when it mints one.
        CastBounds::Arm(fid, spellID, 0, ttlMs);

        // 4-5. ARM + TRIGGER. Arm the expected-cast hand-off, then drive.
        {
            std::scoped_lock lk(g_expectMx);
            g_expect.insert(Key(fid, spellID));
        }
        const DriveResult r =
            DriveObservedCast(a_follower, a_spell, selfCast ? a_follower : a_target, /*left*/ false);
        if (r != DriveResult::kArmed) return degrade("drive not armed");

        // Armed: record the stream so the reconciles route its END through this module.
        g_streams[fid] = StreamRec{ spellID, /*proxy*/ 0, targetID };
        return true;
    }

    bool StreamLive(RE::FormID a_follower) {
        return g_streams.find(a_follower) != g_streams.end();
    }

    void End(RE::FormID a_follower) {
        auto it = g_streams.find(a_follower);
        if (it == g_streams.end()) return;
        {
            std::scoped_lock lk(g_expectMx);
            g_expect.erase(Key(a_follower, it->second.spell));
            if (it->second.proxy) g_expect.erase(Key(a_follower, it->second.proxy));
        }
        g_streams.erase(it);
        CastBounds::Disarm(a_follower);
        APMFBridge::ReleaseCast(a_follower);
        // The hand restore itself lives with the trigger (main-thread), added when
        // DriveObservedCast is implemented.
    }

    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell) {
        std::scoped_lock lk(g_expectMx);
        return g_expect.find(Key(a_follower, a_spell)) != g_expect.end();
    }

    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell) {
        std::scoped_lock lk(g_expectMx);
        g_expect.erase(Key(a_follower, a_spell));
    }

    void Reset() {
        g_streams.clear();
        g_backoff.clear();
        std::scoped_lock lk(g_expectMx);
        g_expect.clear();
        // APMF claims are dropped by APMFBridge::ClearTransientState on the same
        // kPreLoadGame; CastBounds by CastBounds::Reset.
    }

}
