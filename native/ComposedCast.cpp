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
// This TU is the executor scaffold + the ONE trigger seam (DriveObservedCast),
// now IMPLEMENTED as an OBSERVE-ONLY animated drive (2026-09-04): behind the
// opt-in bHealAnimPackage toggle it replicates the captured NPC cast sequence on
// the hand caster to OBSERVE (via Diagnostics::SpellSink's CFC-fired log) whether
// driving those events produces a real cast, while ALWAYS degrading Try() to the
// caller's proven kInstant apply so a heal still lands. Off/AE-only/APMF-gated ->
// byte-identical to the kInstant heal. Owning the cast (kArmed, suppress kInstant)
// is deferred until a deck cycle proves the driven sequence lands the effect.
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

        // ── THE TRIGGER SEAM (steering 2026-09-04, IMPLEMENTED 2026-09-04) ──────
        // The ONE isolated point that turns an ARMED cast into a REAL animated cast
        // by REPLICATING the engine's own NPC full-animation cast sequence, captured
        // by the APMF passive observer from a deck cycle (a vanilla mage, Arniel):
        //
        //   anim BeginCastRight/Left -> MagicCaster[hand] state 1(ready) -> 2(Charging)
        //     -> 3(Charged) -> 4(Casting) -> anim MRh_SpellFire_Event (+MRh_WinStart)
        //     [MLh_ for left] -> (concentration loops) -> anim InterruptCast, CastStop.
        //
        // This is EXACTLY the drive the shipped `bDriveCaster` probe already runs for
        // the owned/offense path (Actuation.cpp:691-744 -- currentSpell/state/
        // CheckCast/desiredTarget/RequestCastImpl); the probe's DOCUMENTED gap was
        // that the graph RELEASE event never played, so the caster charged and
        // wedged. The deck capture supplies that missing release event, and this seam
        // adds it: RequestCastImpl + BeginCast to charge, then MRh_SpellFire_Event
        // once the caster reaches Charged, then the InterruptCast/CastStop teardown.
        //
        // EXPERIMENTAL + OBSERVE-ONLY (marth 2026-09-04). It is genuinely uncertain
        // whether driving these events makes the engine APPLY the effect or only play
        // the animation. So the drive runs to OBSERVE (the existing Diagnostics
        // SpellSink logs `CFC-fired *** THE ANIMATED PATH ***` for an armed match --
        // no hotkey), and DriveObservedCast returns kObserving, which DEGRADES Try()
        // to the caller's kInstant apply so the heal ALWAYS lands correctly through
        // the proven ConcProxy/self machinery. The animated drive is a SINGLE-SHOT
        // (fire once, tear the channel down) so it never sustains a channel competing
        // with the kInstant heal. kArmed (suppress the kInstant, own the cast) is
        // deliberately NOT returned yet: only after a deck cycle proves the driven
        // sequence both reproduces AND lands the effect should the effect be routed
        // through this path -- and then through the EXISTING correct machinery, not a
        // duplicate. Until then this is fully degrade-safe: a failed/uncertain drive
        // falls to kInstant with no stuck state (every exit runs DriveTeardown).
        enum class DriveResult { kNotImplemented, kArmed, kObserving, kFailed };

        // ── DEDICATED animated-cast proxy pool ──────────────────────────────────
        // A SEPARATE delivery-flipped pool (never the Actuation ConcProxy kInstant
        // pool, never AddSpell'd to the actor -- runtime 0xFF dynamic forms only),
        // lifting ConcProxy's owner-keyed 2-slot design. Off-self heals are Self
        // delivery; the hand caster must aim at the recipient, so the equipped form
        // is a kTargetActor copy. Main-thread-serial (Acquire refuses off-main, VR).
        namespace HealProxy {
            struct Slot { RE::SpellItem* form = nullptr; RE::FormID source = 0; RE::FormID owner = 0; };
            Slot g_slot[2];   // main-thread-serial

            void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src) {
                a_p->data          = a_src->data;                                // castingType/cost/etc.
                a_p->data.delivery = RE::MagicSystem::Delivery::kTargetActor;    // the ONLY change
                a_p->effects.clear();
                for (auto* e : a_src->effects) a_p->effects.push_back(e);        // shared source Effect*
            }
            RE::SpellItem* Acquire(RE::FormID a_owner, RE::SpellItem* a_src) {
                if (!a_src || !a_owner || !MainThread::IsInstalled()) return nullptr;
                const auto sid = a_src->GetFormID();
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) {
                    if (s.source != sid) { Configure(s.form, a_src); s.source = sid; }
                    return s.form;
                }
                for (auto& s : g_slot) if (s.owner == 0) {
                    if (!s.form) {
                        auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                        s.form = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                        if (!s.form) return nullptr;
                    }
                    Configure(s.form, a_src); s.source = sid; s.owner = a_owner;
                    spdlog::info("[cfc] proxy ACQUIRE owner {:08X} src {:08X} form {:08X}",
                                 a_owner, sid, s.form->GetFormID());
                    return s.form;
                }
                spdlog::info("[cfc] proxy OVERFLOW owner {:08X} src {:08X} -- drive original", a_owner, sid);
                return nullptr;
            }
            RE::FormID FormForOwner(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) return s.form->GetFormID();
                return 0;
            }
            void Free(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner) { s.owner = 0; s.source = 0; }
            }
            void Reset() {   // kPreLoadGame / revert -- drop borrowed source Effect* first (UAF guard)
                for (auto& s : g_slot) { if (s.form) s.form->effects.clear(); s = {}; }
            }
        }

        // Left/right hand equip slot, via the same default-object route Loadout uses
        // (Loadout.cpp:59-65 -- kLeftHandEquip). nullptr on failure -> the equip is
        // skipped and the drive degrades.
        const RE::BGSEquipSlot* HandSlot(bool a_left) {
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            if (!dom) return nullptr;
            return dom->GetObject<RE::BGSEquipSlot>(a_left ? RE::DEFAULT_OBJECT::kLeftHandEquip
                                                           : RE::DEFAULT_OBJECT::kRightHandEquip);
        }

        // The numeric charge-state gate below assumes the engine's kNone==0 baseline
        // (the deck capture's states 1 ready / 2 charging / 3 charged / 4 casting).
        static_assert(static_cast<std::uint32_t>(RE::MagicCaster::State::kNone) == 0,
                      "MagicCaster::State baseline moved -- re-check the >=3 (Charged) release gate");

        // Per-drive context threaded (by value) through the main-thread phase chain.
        struct DriveCtx {
            RE::FormID    fid         = 0;   // the follower
            RE::FormID    origSpellID = 0;   // the gambit's spell (teardown key)
            RE::FormID    castFormID  = 0;   // what we DRIVE: proxy off-self, else origSpell
            RE::FormID    targetID    = 0;   // 0 == self cast
            bool          left        = false;
            std::uint32_t ttlMs       = 4000;
        };
        inline constexpr int kSelectTries = 6;    // frames to wait for the equip to select
        inline constexpr int kChargePolls = 180;  // frames (~3 s) to wait for Charged before giving up

        // Every drive exit funnels here: stop the driven channel, play the observed
        // teardown anims, unequip the proxy, and RELEASE every hold Try/the drive
        // armed (bounds + APMF claim + proxy slot + both expect keys). Main-thread.
        void DriveTeardown(const DriveCtx& a_c) {
            const RE::FormID proxyID = HealProxy::FormForOwner(a_c.fid);
            if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_c.fid)) {
                using CS = RE::MagicSystem::CastingSource;
                if (auto* mc = f->GetMagicCaster(a_c.left ? CS::kLeftHand : CS::kRightHand))
                    mc->InterruptCast(false);   // stop the engine channel (no refund; kInstant owns cost)
                f->NotifyAnimationGraph("InterruptCast");   // observed teardown, step 1
                f->NotifyAnimationGraph("CastStop");        // observed teardown, step 2
                if (auto* px = proxyID ? RE::TESForm::LookupByID<RE::SpellItem>(proxyID) : nullptr)
                    f->DeselectSpell(px);       // clear the proxy from the hand (never AddSpell'd)
            }
            {
                std::scoped_lock lk(g_expectMx);
                g_expect.erase(Key(a_c.fid, a_c.origSpellID));
                if (proxyID) g_expect.erase(Key(a_c.fid, proxyID));
            }
            CastBounds::Disarm(a_c.fid);
            APMFBridge::ReleaseCast(a_c.fid);
            HealProxy::Free(a_c.fid);
        }

        // Phase 3: poll the caster until it reaches Charged, then fire the graph
        // RELEASE event (the missing piece the old probe lacked), then tear down.
        void PhaseFire(DriveCtx a_c, int a_pollsLeft) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_c.fid);
            if (!f) { DriveTeardown(a_c); return; }
            using CS = RE::MagicSystem::CastingSource;
            auto* hand = f->GetMagicCaster(a_c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { DriveTeardown(a_c); return; }

            const auto stNum = static_cast<std::uint32_t>(hand->state.get());
            if (stNum >= 3) {   // Charged/Casting -> release
                f->NotifyAnimationGraph(a_c.left ? "MLh_SpellFire_Event" : "MRh_SpellFire_Event");
                f->NotifyAnimationGraph(a_c.left ? "MLh_WinStart"        : "MRh_WinStart");
                spdlog::info("[castobs] {:08X} SpellFire at state {} -- observed sequence REPRODUCED "
                             "(watch for CFC-fired to confirm the effect landed)", a_c.fid, stNum);
                MainThread::Post([a_c] { DriveTeardown(a_c); });   // single-shot: release a couple frames, then stop
                return;
            }
            if (stNum == 0) {   // kNone: never entered charge, or already completed
                spdlog::info("[castobs] {:08X} caster at rest before charge -- no driven cast", a_c.fid);
                DriveTeardown(a_c);
                return;
            }
            if (a_pollsLeft <= 0) {   // charged never reached: the graph wedge the probe predicted
                spdlog::info("[castobs] {:08X} WEDGED at state {} -- graph never charged; degrade", a_c.fid, stNum);
                DriveTeardown(a_c);
                return;
            }
            MainThread::Post([a_c, a_pollsLeft] { PhaseFire(a_c, a_pollsLeft - 1); });
        }

        // Phase 2: wait for the (queued) equip to actually SELECT the spell on the
        // caster (the probe's `currentSpell != spell` guard), then start the request
        // from rest with the BeginCast anim + RequestCastImpl, and hand to PhaseFire.
        void PhaseSelect(DriveCtx a_c, int a_triesLeft) {
            auto* f        = RE::TESForm::LookupByID<RE::Actor>(a_c.fid);
            auto* castForm = RE::TESForm::LookupByID<RE::SpellItem>(a_c.castFormID);
            auto* target   = a_c.targetID ? RE::TESForm::LookupByID<RE::Actor>(a_c.targetID) : nullptr;
            if (!f || !castForm || (a_c.targetID && !target)) { DriveTeardown(a_c); return; }
            using CS = RE::MagicSystem::CastingSource;
            auto* hand = f->GetMagicCaster(a_c.left ? CS::kLeftHand : CS::kRightHand);
            if (!hand) { DriveTeardown(a_c); return; }

            // Re-arm bounds around the ACTUAL driven window on the main thread
            // (actor+spell AND actor+proxy), covering CheckCast's combat-thread hooks
            // regardless of Try's synchronous pre-arm having lapsed. Idempotent.
            CastBounds::Arm(a_c.fid, a_c.origSpellID, HealProxy::FormForOwner(a_c.fid), a_c.ttlMs);

            if (hand->currentSpell != castForm) {   // equip queued -> not selected yet
                if (a_triesLeft <= 0) {
                    spdlog::info("[castobs] {:08X} caster never selected the drive form {:08X} -- degrade",
                                 a_c.fid, a_c.castFormID);
                    DriveTeardown(a_c);
                    return;
                }
                if (auto* mgr = RE::ActorEquipManager::GetSingleton())
                    mgr->EquipSpell(f, castForm, HandSlot(a_c.left));
                f->DrawWeaponMagicHands(true);
                MainThread::Post([a_c, a_triesLeft] { PhaseSelect(a_c, a_triesLeft - 1); });
                return;
            }

            // Selected. Drive ONLY from rest (re-requesting mid-sequence wedges the
            // caster in charge-glow -- the probe's hard-won discipline).
            if (hand->state.get() != RE::MagicCaster::State::kNone)
                hand->InterruptCast(true);
            if (target) hand->desiredTarget = target->CreateRefHandle();

            float                          strength = 1.0f;
            RE::MagicSystem::CannotCastReason reason{};
            const bool ok = hand->CheckCast(castForm, false, &strength, &reason, false);

            // OBSERVED SEQUENCE step 1: the BeginCast anim + the state-machine request.
            f->NotifyAnimationGraph(a_c.left ? "BeginCastLeft" : "BeginCastRight");
            hand->RequestCastImpl();

            spdlog::info("[castobs] {:08X} BeginCast{} + RequestCastImpl -- CheckCast={} reason={} state->{}",
                         a_c.fid, a_c.left ? "Left" : "Right", ok ? "OK" : "REFUSED",
                         static_cast<std::uint32_t>(reason),
                         static_cast<std::uint32_t>(hand->state.get()));

            MainThread::Post([a_c] { PhaseFire(a_c, kChargePolls); });
        }

        // Launch the observe drive (called on the WORKER from Try). All hand/equip/
        // caster mutation is MainThread::Post'd (#62); the proxy is minted on the
        // main thread inside the first phase (HealProxy is main-thread-only). Returns
        // kFailed synchronously (Try degrades cleanly) when there is no main-thread
        // pump (VR); otherwise kObserving (the phase chain OWNS the teardown).
        DriveResult DriveObservedCast(RE::Actor* a_caster, RE::SpellItem* a_castForm,
                                      RE::Actor* a_target, bool a_leftHand, std::uint32_t a_ttlMs) {
            if (!a_caster || !a_castForm)   return DriveResult::kFailed;
            if (!MainThread::IsInstalled()) return DriveResult::kFailed;   // no pump -> kInstant

            const bool     selfCast = (!a_target || a_target == a_caster);
            DriveCtx c;
            c.fid         = a_caster->GetFormID();
            c.origSpellID = a_castForm->GetFormID();
            c.targetID    = selfCast ? 0 : a_target->GetFormID();
            c.left        = a_leftHand;
            c.ttlMs       = a_ttlMs;

            MainThread::Post([c, selfCast]() mutable {
                if (!selfCast) {
                    // Mint the delivery-flipped proxy (main-thread only). No slot ->
                    // drive the original (may self-collapse; logged, harmless -- the
                    // kInstant heal still lands correctly this beat).
                    auto* src = RE::TESForm::LookupByID<RE::SpellItem>(c.origSpellID);
                    auto* px  = src ? HealProxy::Acquire(c.fid, src) : nullptr;
                    c.castFormID = px ? px->GetFormID() : c.origSpellID;
                    if (px) {
                        // The driven cast fires as the PROXY form -- arm the SpellSink
                        // hand-off on that key too so CFC-fired matches it.
                        std::scoped_lock lk(g_expectMx);
                        g_expect.insert(Key(c.fid, c.castFormID));
                    }
                } else {
                    c.castFormID = c.origSpellID;
                }
                PhaseSelect(c, kSelectTries);
            });
            return DriveResult::kObserving;
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
        // leftHand=false -> RIGHT hand, matching the deck capture (Arniel cast from
        // the right hand: BeginCastRight / MRh_SpellFire_Event). The drive equips
        // into whichever hand this names and fires the matching graph events.
        const DriveResult r = DriveObservedCast(
            a_follower, a_spell, selfCast ? a_follower : a_target, /*left*/ false, ttlMs);

        switch (r) {
        case DriveResult::kArmed:
            // The executor OWNS an animated stream: record it so the reconciles route
            // its END through this module, and the caller SKIPS its kInstant apply.
            // (Not returned by today's observe-only drive -- reserved for the post-
            // deck-proof graduation; see DriveObservedCast.)
            g_streams[fid] = StreamRec{ spellID, HealProxy::FormForOwner(fid), targetID };
            return true;

        case DriveResult::kObserving:
            // The animated OBSERVE drive was launched on the main thread; its phase
            // chain OWNS the bounds/claim/proxy/teardown lifecycle. DEGRADE to the
            // caller's kInstant apply (a heal ALWAYS lands, correctly, this beat) but
            // do NOT call degrade() -- that would Disarm the bounds / Release the
            // claim out from under the in-flight drive. Set the backoff (worker-
            // serial) so we observe once per window and never churn the APMF claim.
            g_backoff[fid] = now;
            return false;

        default:   // kNotImplemented / kFailed -> synchronous clean degrade
            return degrade("drive not armed");
        }
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
        HealProxy::Reset();   // free the dedicated proxy pool (drop borrowed Effect* first)
        std::scoped_lock lk(g_expectMx);
        g_expect.clear();
        // APMF claims are dropped by APMFBridge::ClearTransientState on the same
        // kPreLoadGame; CastBounds by CastBounds::Reset.
    }

}
