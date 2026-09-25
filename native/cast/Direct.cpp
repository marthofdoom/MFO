// cast/Direct.cpp -- the DIRECT-DELIVERY road: the apply substrate (ConcProxy,
// dispel, Apply{Self,Target}Effect, charge-for-time) and the per-follower
// self/target streams (CastSelfDirect / CastTargetDirect, their reconciles and
// ClearSelfCasts). CastAuto lives in cast/Auto.cpp, summons in cast/Summon.cpp.
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "ComposedCast.h"   // the Composed Forced Cast executor -- replaces the deleted
                            // HealAnimFill package route at the two cast plug-ins below
#include "CastBounds.h"     // Reset the MFO-executed-cast bound beside ConcProxy::Reset()
#include "Runtime.h"        // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"     // feat/cast-gambit-concentration (Task 1): ClaimOffenseCast for a
                            // non-heal (Offense/Buff) CONCENTRATION stream -- ComposedCast::Try
                            // above is HEAL-ONLY by design, so offense/buff concentration needs
                            // its OWN direct claim call here rather than a widened Try() gate.

namespace MFO::Actuation {

    // ── FORCED SELF-CAST: the UNIVERSAL direct trigger (SPEC-self-cast-forced) ──
    // The MFO_CastPackageSelf alias route EQUIPS the spell but never TRIGGERS the
    // cast, and is declined outright on package-locked custom followers (Lucien).
    // So self-cast bypasses packages and applies the effect DIRECTLY -- follower-
    // agnostic, effect + magicka only, NO equip and NO channel:
    //
    //   * NEVER EQUIP THE SPELL. CastSpellImmediate (kInstant) applies the effect
    //     without the spell in hand. Leaving a light spell (Candlelight/Magelight)
    //     equipped let the follower's OWN AI spam-cast it -- 55+ non-MFO lights
    //     piled up to a ShadowSceneNode light-limit CTD (deck 2026-08-19). The
    //     cast animation is DEFERRED (polish pass), so the equip/HoldStow/caster-
    //     drive scaffolding is gone entirely.
    //   * FIRE (CastSelfDirect, every service/combat tick the rule wins): register
    //     the entry, refresh lastFired, and -- once per beat (kConcApplyPeriod ~1 s
    //     for a CONCENTRATION spell, per-second authored magnitude/cost; once per
    //     fCastCooldown for FF) -- apply the
    //     effect + spend magicka (§5.3) via ApplySelfEffect. The ALREADY-ACTIVE
    //     guard there (the foe cast's own HasMagicEffect check) blocks re-applying
    //     a duration self-buff/light while it is still up, so exactly ONE light
    //     per effect-duration cycle. An instant heal re-fires when HP drops again.
    //   * RELEASE (SelfCastReconcile, each tick): when the rule stops re-firing
    //     (goes stale, or the follower unloads) DISPEL a lingering ward/effect so
    //     it cannot persist as a stuck gameplay buff. NO time cap -- a long buff
    //     lives its authored duration while the rule wins. Covers rule-disabled /
    //     condition-false (the entry simply goes stale). Nothing to unequip.
    namespace {
        struct SelfCastState {
            RE::FormID            spell = 0;
            SelfClock::time_point started{};
            SelfClock::time_point lastFired{};   // last time the rule re-fired (release clock)
            SelfClock::time_point lastApply{};   // last effect/magicka application (apply pacing)
            float                 cap   = 0.0f;  // per-stream randomized time cap (DrawConcCap)
            // CHARGE FOR TIME (v2.0.12): a TIMED stream (momentary concentration --
            // see StreamCharge) is billed for the seconds its effect actually ran.
            // paidThrough = the instant the caster has paid up to (epoch = nothing
            // paid yet); window = the sustain window the beats pin, i.e. how long
            // the effect keeps running past the last beat. Worker-serial.
            SelfClock::time_point paidThrough{};
            float                 window = 0.0f;
            bool                  timed  = false;
        };
        std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // worker-serial

        // ── APMF REFUSAL: the ONE trace a cast that did NOT happen leaves ─────────
        // The exact twin of Actuation.cpp's own LogApmfRefusal (same wording, same
        // ~5 s (follower, spell, target) dedup, same spdlog::error level) -- a
        // separate copy only because the two files are separate TUs and each keeps
        // its own worker-serial anon-namespace state. marth 2026-09-07: "With APMF
        // theres no fallback for it. APMF shoudl work" -- a claim APMF is CAPABLE
        // of granting and refuses anyway is a BUG IN APMF, so the cast fails closed
        // and this line is the whole diagnosis (CLAUDE.md principle 7). Rate-limited
        // (principle 8) because these call sites run at the ~133 ms service cadence.
        //
        // ONE ENTRY PER KEY, NOT ONE SLOT PER FOLLOWER (F2-2, deploy-gate review
        // 2026-09-07) -- see the twin's fuller note in Actuation.cpp. A single
        // last-value slot thrashed between two refused rules on the same follower
        // every tick (failing closed is transparent, so the scan reaches both), so
        // the 5 s window never matched and the throttle emitted two error lines per
        // 133 ms tick. Expired entries are swept on every touch and the vector is
        // capped, reusing the OLDEST slot at the cap -- degrading toward MORE
        // logging, never a silent diagnostic.
        constexpr float       kApmfRefusalEverySec = 5.0f;
        constexpr std::size_t kApmfRefusalMaxKeys  = 8;   // >> the realistic cast-rule count

        struct ApmfRefusalLog {
            RE::FormID            spell  = 0;
            RE::FormID            target = 0;
            SelfClock::time_point when{};
        };
        std::unordered_map<RE::FormID, std::vector<ApmfRefusalLog>> g_lastApmfRefusal;   // worker-serial

        // True = this exact (follower, spell, target) was already logged inside the
        // window, so the caller stays quiet. Otherwise stamps it and returns false.
        bool ApmfRefusalThrottled(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                                  SelfClock::time_point a_now) {
            auto& keys = g_lastApmfRefusal[a_follower];
            std::erase_if(keys, [&](const ApmfRefusalLog& e) {
                return std::chrono::duration<float>(a_now - e.when).count() >= kApmfRefusalEverySec;
            });
            for (auto& e : keys) {
                if (e.spell == a_spell && e.target == a_target) return true;
            }
            if (keys.size() >= kApmfRefusalMaxKeys) {
                auto oldest = std::min_element(keys.begin(), keys.end(),
                                               [](const ApmfRefusalLog& l, const ApmfRefusalLog& r) {
                                                   return l.when < r.when;
                                               });
                *oldest = ApmfRefusalLog{ a_spell, a_target, a_now };
            } else {
                keys.push_back(ApmfRefusalLog{ a_spell, a_target, a_now });
            }
            return false;
        }

        // DISMISSAL DROP (F3-7, deploy-gate review 2026-09-07): the per-follower
        // erase lives at namespace scope below (ClearApmfRefusalLog) because
        // Actuation.cpp's ClearCastLock has to reach it across the TU boundary.
        void LogApmfRefusal(RE::FormID a_follower, const char* a_what, RE::FormID a_spell,
                            RE::FormID a_target, const char* a_hand) {
            if (ApmfRefusalThrottled(a_follower, a_spell, a_target, SelfClock::now())) return;
            spdlog::error("[apmf] {:08X} APMF REFUSED the {} claim -- spell {:08X}, target {:08X}, "
                          "{} hand. APMF is the COMMITTED route: NOT falling back to the direct-"
                          "force/kInstant path, this cast does NOT happen this tick. Fix the "
                          "refusal in APMF.",
                          a_follower, a_what, a_spell, a_target, a_hand);
        }

    }
        std::unordered_map<RE::FormID, TargetCastState> g_targetCast;
    namespace {

        // Stop a spell's lingering effect VFX -- the concentration hit-shader
        // that never terminates when the spell is applied one-shot (deck
        // 2026-08-17: the healing glow ran on after the pose ended). Main thread.
        void DispelSpellEffectsOn(RE::Actor* a_actor, RE::FormID a_spellID) {
            auto* mt = a_actor ? a_actor->AsMagicTarget() : nullptr;
            if (!mt) return;
            auto* list = mt->GetActiveEffectList();
            if (!list) return;
            // Collect FIRST, then dispel -- Dispel can mutate the active-effect
            // list, so calling it mid-iteration is unsafe (F5 hardening).
            std::vector<RE::ActiveEffect*> hits;
            for (auto* ae : *list)
                if (ae && ae->spell && ae->spell->GetFormID() == a_spellID)
                    hits.push_back(ae);
            for (auto* ae : hits) ae->Dispel(true);
        }

        // ── CONCENTRATION + SELF-DELIVERY PROXY (the ONLY delivery fix on top of the
        // baseline). A fire-and-forget Self spell force-cast at another actor lands on
        // that actor (baseline, field-proven -- Candlelight/flesh work). A
        // CONCENTRATION Self spell does NOT: CastSpellImmediate sets up a channeled
        // cast whose target is resolved by the spell's DELIVERY, and kSelf binds the
        // sustained AE to the caster's OWNER (the follower), so a player/ally
        // concentration heal collapses onto the follower and silently fails. FIX:
        // cast a transient COPY of the source with its casting style PRESERVED and ONLY
        // data.delivery flipped kSelf -> kTargetActor, so the channel resolves to the
        // passed target. The follower is still the caster (his rate + magicka); the
        // player never casts / pays. Used ONLY for concentration+Self+off-self -- FF /
        // self-cast / non-Self are untouched baseline.
        //
        // A POOL of transient dynamic (0xFF__) slots, SLOT-FOR-DURATION (marth's hard
        // rule): a CONCENTRATION proxy cast starts a REAL engine channel on the caster
        // (it drains the caster per-second and sustains the effect), so the proxy FORM
        // is load-bearing for the WHOLE life of that channel -- reconfiguring or handing
        // its form to another cast while the channel is live corrupts the in-flight
        // cast (the freeze) and entangles two streams (heal-full stops the 1st but not
        // the 2nd). So each live stream OWNS a slot for its duration: a slot is
        // Configure'd ONLY when FREE, never while its channel lives; released (its
        // channel INTERRUPTED, see TargetCastEndActor) only when the stream ends. Owner
        // = the caster (follower) FormID; one live concentration channel per follower.
        // POOL CAP: one slot per concurrent self-delivery concentration stream, i.e.
        // per healer/warder in the party. The old cap was 2, so a 3rd concurrent
        // self-delivery stream got nullptr + a silent skip and that follower never
        // healed (a 3-healer party's 3rd healer was dead weight, SEV-3). Sized to 6
        // (a party-realistic healer count) so Acquire only overflows past a genuinely
        // pathological caster count; past it Acquire returns nullptr and the caller
        // SKIPS. Never serialized (dynamic forms are not). MAIN THREAD only (form
        // table); returns nullptr off-main (VR).
        //   MERGE NOTE (human): a SEPARATE branch (feat/heal-anim-proxy) adds its own
        //   `g_healSlot` pool near here. This wave is NOT on that branch and only grows
        //   the existing g_slot pool -- reconcile the two pools when that branch merges.
        namespace ConcProxy {
            struct Slot { RE::SpellItem* form = nullptr; RE::FormID source = 0; RE::FormID owner = 0; };
            constexpr std::size_t kSlotCount = 6;   // concurrent self-delivery conc streams (party healers)
            Slot g_slot[kSlotCount];

            void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src) {
                a_p->data          = a_src->data;                                 // castingType/cost/etc.
                a_p->data.delivery = RE::MagicSystem::Delivery::kTargetActor;     // the ONLY change
                a_p->effects.clear();
                for (auto* e : a_src->effects) a_p->effects.push_back(e);         // shared effect ptrs
            }
            // Acquire the owner's slot (its channel keeps its form for its whole life).
            // Reuse the owner's existing slot; else claim a FREE slot and Configure it;
            // else (both owned by other live streams) nullptr -> caller skips.
            RE::SpellItem* Acquire(RE::FormID a_owner, RE::SpellItem* a_src) {
                if (!a_src || !a_owner || !MainThread::IsInstalled()) return nullptr;   // no off-main create (VR)
                const auto sid = a_src->GetFormID();
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) {   // the owner's own slot
                    if (s.source != sid) {   // owner switched channel spell (its old channel already released)
                        Configure(s.form, a_src); s.source = sid;
                        spdlog::info("[cast] proxy slot RECONFIG owner {:08X} src {:08X}", a_owner, sid);
                    }
                    return s.form;
                }
                for (auto& s : g_slot) if (s.owner == 0) {                   // a FREE slot
                    if (!s.form) {
                        auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                        s.form = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                        if (!s.form) return nullptr;
                    }
                    Configure(s.form, a_src); s.source = sid; s.owner = a_owner;
                    spdlog::info("[cast] proxy slot ACQUIRE owner {:08X} src {:08X} form {:08X}",
                                 a_owner, sid, s.form->GetFormID());
                    return s.form;
                }
                spdlog::info("[cast] proxy slot OVERFLOW owner {:08X} src {:08X} -- skipped", a_owner, sid);
                return nullptr;   // both slots owned by other live streams -> skip
            }
            // The proxy FormID the owner currently holds (or 0) -- so a stream's release
            // can dispel the proxy-keyed AE off the target.
            RE::FormID FormForOwner(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner && s.form) return s.form->GetFormID();
                return 0;
            }
            // Release the owner's slot (channel ended). The form is KEPT for reuse; only
            // the owner/source markers clear, so a future Acquire may Configure it fresh.
            void Free(RE::FormID a_owner) {
                for (auto& s : g_slot) if (s.owner == a_owner) {
                    spdlog::info("[cast] proxy slot FREE owner {:08X}", a_owner);
                    s.owner = 0; s.source = 0;
                }
            }
            // Revert/load reset (ClearSelfCasts, kPreLoadGame BEFORE any post-load cast
            // + BEFORE the old game's forms are torn down). The dynamic 0xFF forms do
            // NOT survive a load; null the slots so the next cast re-mints, and clear
            // each form's BORROWED source Effect* first (Configure copied them by
            // pointer) so the load-time purge frees an EMPTY array -- never double-free
            // a live source spell's effects. Main thread (StopPump drained).
            void Reset() {
                for (auto& s : g_slot) {
                    if (s.form) s.form->effects.clear();   // drop borrowed source Effect*
                    s = {};
                }
            }
        }

        // The spell to CAST for a_follower delivering a_src at a_tgt. A CONCENTRATION +
        // Self spell aimed off-self returns its delivery-flipped PROXY, acquired for the
        // follower's OWNED slot (slot-for-duration) -- or nullptr if both slots are held
        // by other live streams / off-main (VR), in which case the caller MUST SKIP (it
        // must NOT cast the Self source, which would collapse the channel onto the
        // follower). Every other spell (FF, self-cast, non-Self) returns a_src unchanged.
        // `out_isProxy` distinguishes "skip (nullptr proxy needed)" from a normal cast.
        // MAIN THREAD.
        RE::SpellItem* DeliverySpell(RE::SpellItem* a_src, RE::Actor* a_follower, RE::Actor* a_tgt,
                                     bool& out_needsProxy) {
            out_needsProxy = a_src && a_follower && a_tgt && a_tgt != a_follower &&
                             a_src->GetDelivery()    == RE::MagicSystem::Delivery::kSelf &&
                             a_src->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            if (out_needsProxy)
                return ConcProxy::Acquire(a_follower->GetFormID(), a_src);   // proxy or nullptr(skip)
            return a_src;
        }

        // A MOMENTARY concentration spell (value-modifier: heal/drain) -- the kind
        // whose sustained effect genuinely channels between beats and whose beat
        // is never skipped by the already-active guard. ONE predicate for both the
        // guard bypass in ApplySelfEffect (main thread) and the worker's decision
        // that a self stream is TIMED (charged for time). Reads form data only.
        bool ConcMomentary(RE::SpellItem* a_sp) {
            if (!a_sp || a_sp->GetCastingType() != RE::MagicSystem::CastingType::kConcentration)
                return false;
            auto* ei   = a_sp->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            return mgef &&
                   (mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kValueModifier ||
                    mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kDualValueModifier);
        }

        // ── CHARGE FOR TIME (v2.0.12, ClickUp 86e3d6dp0) ─────────────────────────
        // CalculateMagickaCost on a concentration spell is the caster's effective
        // cost PER SECOND. The direct road used to charge ONE second per beat, but
        // beats land ~1.2-2.0 s apart and a released stream's effect ran on up to
        // its sustain window past the last beat: followers paid for ~48% of the
        // healing seconds they delivered (44 field streams). A TIMED stream now
        // pays for the seconds its effect actually ran. All clocks are WORKER-side
        // (the stream maps); only the resulting seconds cross to the main-thread
        // deduct, BY VALUE, exactly like the spell/target ids already do.
        //
        // Seconds to bill on a beat at a_now. First beat of a stream (paidThrough
        // at epoch): one second, as before (it pays the channel's first second in
        // advance). Later beats: the time since paidThrough, but only up to where
        // the PREVIOUS beat's sustain window ran out (a_prevApply + a_window) -- if
        // the beats were sparser than the window the effect lapsed and the gap was
        // not channeled. Advances paidThrough. An untimed stream bills 1 (FF, or a
        // guarded sticky ward: per application, unchanged).
        float BeatChargeSec(bool a_timed, SelfClock::time_point& a_paidThrough,
                            SelfClock::time_point a_prevApply, float a_window,
                            SelfClock::time_point a_now) {
            if (!a_timed) return 1.0f;
            if (a_paidThrough == SelfClock::time_point{}) {
                a_paidThrough = a_now + std::chrono::duration_cast<SelfClock::duration>(
                                            std::chrono::duration<float>(kConcApplyPeriod));
                return kConcApplyPeriod;
            }
            const auto ranTo = std::min(a_now, a_prevApply + std::chrono::duration_cast<SelfClock::duration>(
                                                                 std::chrono::duration<float>(a_window)));
            const float sec  = std::max(0.0f, std::chrono::duration<float>(ranTo - a_paidThrough).count());
            a_paidThrough    = std::max(a_paidThrough, a_now);
            return sec;
        }

        // Seconds still owed when a timed stream is RELEASED at a_now: from
        // paidThrough to where the effect stops -- the release itself (it is
        // dispelled, or re-streamed at once on a cap-only self release) or the
        // last beat's sustain window running out, whichever is first. 0 when
        // untimed, never applied, or already paid past that point.
        float SettleSec(bool a_timed, SelfClock::time_point a_paidThrough,
                        SelfClock::time_point a_lastApply, float a_window, SelfClock::time_point a_now) {
            if (!a_timed || a_paidThrough == SelfClock::time_point{}) return 0.0f;
            const auto ranTo = std::min(a_now, a_lastApply + std::chrono::duration_cast<SelfClock::duration>(
                                                                 std::chrono::duration<float>(a_window)));
            return std::max(0.0f, std::chrono::duration<float>(ranTo - a_paidThrough).count());
        }

        // Post the release-time SETTLE deduct (main thread, like every other
        // deduct on this road): a_sec x the caster's per-second cost, clamped to
        // the live pool (#6). a_reason / a_road are string literals (static
        // storage), safe to carry across the post.
        void PostSettle(RE::FormID a_casterID, RE::FormID a_spellID, float a_sec,
                        const char* a_road, const char* a_reason) {
            if (a_sec <= 0.0f) return;
            MainThread::Post([a_casterID, a_spellID, a_sec, a_road, a_reason] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
                auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!caster || !sp) return;
                auto* avo = caster->AsActorValueOwner();
                if (!avo) return;
                const float before = avo->GetActorValue(RE::ActorValue::kMagicka);
                const float cost   = sp->CalculateMagickaCost(caster);
                const float spend  = std::clamp(cost * a_sec, 0.0f, std::max(0.0f, before));
                if (spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo->GetActorValue(RE::ActorValue::kMagicka);
                spdlog::info("[cast] {:08X} {} SETTLE ({}) spell {:08X} -- {:.2f}s x {:.1f}/s, "
                             "magicka {:.0f}->{:.0f} (spent {:.1f})",
                             a_casterID, a_road, a_reason, a_spellID, a_sec, cost, before, after, spend);
            });
        }

        // Apply the effect + spend magicka for ONE fire (main thread). §5.3:
        // CastSpellImmediate spends nothing (§0.22), so deduct the real cost:
        // a_chargeSec x CalculateMagickaCost (the seconds BeatChargeSec billed
        // for a timed concentration stream; 1 = one cast's / one second's cost).
        void ApplySelfEffect(RE::FormID a_id, RE::FormID a_spellID, float a_chargeSec) {
            MainThread::Post([a_id, a_spellID, a_chargeSec] {
                auto* a  = RE::TESForm::LookupByID<RE::Actor>(a_id);
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!a || !sp) return;
                // ALREADY-ACTIVE GUARD -- byte-identical to the TARGET one-shot
                // cast's own guard (Logistics OOC cast, "skip re-casting a buff
                // still on the TARGET"): do NOT re-apply a spell whose effect is
                // already active on the caster. A duration self-buff/LIGHT
                // (Candlelight) otherwise spawns a fresh light every re-cast and
                // they accumulate to a ShadowSceneNode null-call CTD (deck
                // 2026-08-18, "Active Lights 57"). Self and foe now behave
                // identically; an instant self-heal leaves no active effect, so it
                // still re-fires when the condition recurs; a ward re-fires after
                // it drops.
                auto* ei   = sp->GetCostliestEffectItem();
                auto* mgef = ei ? ei->baseEffect : nullptr;
                // A MOMENTARY concentration effect (value-modifier: heal/drain)
                // BYPASSES the guard: its SUSTAINED real effect (the synthesized-
                // duration channel) is by design ACTIVE on every subsequent beat,
                // and the guard must not block the beat that re-arms it -- block
                // it and the channel expires at the window instead of rolling.
                // The guard's CTD case (lights/duration buffs) is not value-
                // modifier and stays guarded.
                const bool concMomentary = ConcMomentary(sp);
                if (auto* mt = a->AsMagicTarget();
                    !concMomentary && mgef && mt && mt->HasMagicEffect(mgef)) {
                    spdlog::info("[cast] {:08X} cast_self skipped -- {} ({:08X}) already active",
                                 a_id, sp->GetName() ? sp->GetName() : "?", a_spellID);
                    return;
                }
                auto* avo  = a->AsActorValueOwner();
                auto* inst = a->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // THE REAL EFFECT with a synthesized duration (marth's
                    // ruling -- see SustainConcentrationEffect): attach once,
                    // then re-arm the same ActiveEffect each beat; the ENGINE
                    // channels the authored magnitude itself. Window = the self
                    // stream's cap (heal 6 s / ward-utility 15 s) so the single
                    // entry bridges even a magicka-starved caster's sparse beats.
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcSelfUtilityCap;
                    if (!SustainConcentrationEffect(a, sp, window)) {
                        inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);   // attach ONCE
                        SustainConcentrationEffect(a, sp, window);                      // then pin it
                        // Evidence line: ONCE per stream if the engine honors the
                        // pinned duration; repeating every beat = sustain refused.
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on self "
                                     "(spell {:08X}, window {:.0f}s)", a_id, a_spellID, window);
                    }
                } else {
                    inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);
                }
                const float cost  = sp->CalculateMagickaCost(a);
                // #6: clamp to the current pool so a deduct never drives magicka
                // negative (AUTO/self validate cost against ONE worker snapshot).
                // CHARGE FOR TIME: a_chargeSec seconds of the per-second cost.
                const float spend = avo ? std::clamp(cost * a_chargeSec, 0.0f, std::max(0.0f, before)) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} SELF-CAST {} ({:08X}) -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f} x {:.2f}s = {:.1f})",
                             a_id, a->GetName() ? a->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, before, after, cost,
                             a_chargeSec, spend);
            });
        }

        // RELEASE a self-cast (main thread): dispel the applied ward/effect so it
        // does not persist as a stuck gameplay buff after the rule goes false.
        // The self-cast NEVER equips or channels, so there is nothing to unequip,
        // interrupt, or sheathe -- and doing any of those would touch the
        // follower's OWN combat draw/equip state, which is not ours to change.
        void SelfCastEndActor(RE::FormID a_id, RE::FormID a_spellID) {
            MainThread::Post([a_id, a_spellID] {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id)) {
                    DispelSpellEffectsOn(a, a_spellID);
                    // A SELF concentration channel also drains the caster per-second
                    // via the engine -- interrupt it so a self-heal/ward channel truly
                    // ends (no runaway self-drain), same as the target path.
                    if (auto* mc = a->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant))
                        mc->InterruptCast(false);
                }
            });
        }

        // ApplySelfEffect generalized to a NON-self target (main thread). SAME
        // direct call the public build's CastOn force-half used -- the known-
        // working force, no package. Deducts the CASTER's real magicka (§5.3).
        // MAGNITUDE: an FF spell's effect applies in full through the plain call;
        // a CONCENTRATION spell's effect is attached ONCE and SUSTAINED with a
        // synthesized duration (SustainConcentrationEffect -- marth's real-effect
        // ruling) so the ENGINE channels the authored magnitude itself; a bare
        // one-shot applies ~0 (b63beb9 field A/B, the revoked "plain call heals
        // fine" ruling).
        //   a_guard TRUE  (sticky ward/buff): skip if the buff is already active on
        //                 the target, so a duration buff is not re-stacked.
        //   a_guard FALSE (heal / damage -- MOMENTARY): every paced beat deducts
        //                 the seconds billed since the last charge (a_chargeSec,
        //                 CHARGE FOR TIME) and re-arms the sustained effect, so the
        //                 target is topped up steadily while the rule wins.
        void ApplyTargetEffect(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_guard, float a_chargeSec) {
            MainThread::Post([a_casterID, a_targetID, a_spellID, a_guard, a_chargeSec] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
                auto* tgt    = RE::TESForm::LookupByID<RE::Actor>(a_targetID);
                auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!caster || !tgt || !sp) return;
                if (a_guard) {
                    auto* ei   = sp->GetCostliestEffectItem();
                    auto* mgef = ei ? ei->baseEffect : nullptr;
                    if (auto* mt = tgt->AsMagicTarget(); mgef && mt && mt->HasMagicEffect(mgef)) {
                        spdlog::info("[cast] {:08X} force-cast skipped -- {} ({:08X}) already "
                                     "active on {:08X}", a_casterID,
                                     sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID);
                        return;
                    }
                }
                auto* avo  = caster->AsActorValueOwner();
                auto* inst = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // FORCED CONCENTRATION = THE REAL EFFECT with a synthesized
                    // duration (marth's ruling -- see SustainConcentrationEffect):
                    // attach ONCE, then re-arm the one ActiveEffect each beat; the
                    // ENGINE channels the authored magnitude itself (resists,
                    // hostility, every archetype). Window by kind: heal 6 s
                    // bridges sparse beats; offense/utility 4 s covers their caps.
                    // A CONCENTRATION + SELF spell aimed off-self collapses onto the
                    // FOLLOWER (delivery binds the channel to the caster's owner), so
                    // cast the delivery-flipped PROXY (kTargetActor) instead -- the
                    // channel then resolves to tgt. Sustain keys on the SAME spell we
                    // cast, so a proxy stream re-arms its own AE cleanly.
                    bool needsProxy = false;
                    RE::SpellItem* castSp = DeliverySpell(sp, caster, tgt, needsProxy);
                    if (needsProxy && !castSp) return;   // proxy slots full / VR -> SKIP
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcUtilityHold;
                    if (!SustainConcentrationEffect(tgt, castSp, window)) {
                        // The KNOWN-WORKING FORCE -- caster casts (proxy of) sp AT tgt.
                        inst->CastSpellImmediate(castSp, false, tgt, 1.0f, false, 0.0f, caster);
                        SustainConcentrationEffect(tgt, castSp, window);
                        // Evidence line: ONCE per stream if the engine honors the
                        // pinned duration; repeating every beat = sustain refused.
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on {:08X} "
                                     "(spell {:08X}{}, window {:.0f}s)",
                                     a_casterID, a_targetID, a_spellID,
                                     castSp != sp ? " self->target proxy" : "", window);
                    }
                } else {
                    // The KNOWN-WORKING FORCE -- caster casts sp AT tgt, package-free.
                    // (Baseline: an FF Self spell force-cast here lands on tgt.)
                    inst->CastSpellImmediate(sp, false, tgt, 1.0f, false, 0.0f, caster);
                }
                // ── CONCENTRATION DOUBLE-CHARGE: OPEN, deliberately UNCHANGED ─────
                // Concurrency-wave verdict (representative of all three Apply* paths):
                // a possible double-charge exists on CONCENTRATION streams -- the manual
                // per-beat deduct below AND, per TargetCastEndActor's field note, a REAL
                // engine channel this cast starts that "drains him per-second INDEPENDENT
                // of MFO's per-beat apply" (the runaway that InterruptCast exists to stop).
                // But the code+docs evidence is CONTRADICTORY and cannot settle it:
                //   * ENGINE_NOTES 0.22/0.23(b) MEASURED these casts as FREE (Thunderbolt
                //     343, pool 1000 -> 1000 after) -- which is the whole REASON MFO
                //     deducts manually; if that holds for concentration too there is NO
                //     double-charge and this deduct is the SOLE charge (correct).
                //   * ENGINE_NOTES 0.9 MEASURED CastSpellImmediate as DEDUCTING and warns
                //     #16 double-spend -- the opposite.
                //   * The self-drain claim is a field observation of a runaway AFTER a
                //     stream ends, not an isolated measurement of a per-beat double-deduct
                //     DURING one; concentration magicka was never A/B measured either way.
                // Deciding needs runtime instrumentation (deduct disabled -> does a live
                // concentration stream still drain?), which code review cannot do. Per the
                // brief's rule -- never guess a magicka path; breaking the economy is worse
                // than the flag -- this is left EXACTLY as-is and flagged for a field A/B.
                // CHARGE FOR TIME: a_chargeSec seconds of the per-second cost
                // (BeatChargeSec, worker-side); #6: clamped, never negative.
                const float cost  = sp->CalculateMagickaCost(caster);
                const float spend = avo ? std::clamp(cost * a_chargeSec, 0.0f, std::max(0.0f, before)) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} FORCE-CAST {} ({:08X}) at {:08X} -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f} x {:.2f}s = {:.1f})",
                             a_casterID, caster->GetName() ? caster->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                             before, after, cost, a_chargeSec, spend);
            });
        }

        // RELEASE a target stream (main thread): dispel our lingering ward/buff --
        // and the SUSTAINED real effect a momentary stream leaves -- off the
        // TARGET. Since the real-effect sustain, this dispel is LOAD-BEARING for
        // momentary kinds: the sustained effect genuinely channels (heals/damages)
        // until its pinned window elapses, so the stream's end must cut it rather
        // than let it run unpaid. Callers gate WHEN (Buff: any release; momentary:
        // end-of-stream / switch only, never a cap-only re-stream -- the re-stream
        // re-arms the same effect seamlessly). Mirrors SelfCastEndActor.
        void TargetCastEndActor(RE::FormID a_targetID, RE::FormID a_spellID, RE::FormID a_ownerID) {
            MainThread::Post([a_targetID, a_spellID, a_ownerID] {
                if (auto* t = RE::TESForm::LookupByID<RE::Actor>(a_targetID)) {
                    DispelSpellEffectsOn(t, a_spellID);
                    // A concentration+Self stream channels through a delivery-flipped
                    // PROXY the OWNER holds, so the live AE carries the PROXY's spellID
                    // -- dispel it too so a heal/ward cannot linger past the stream.
                    if (auto proxyID = ConcProxy::FormForOwner(a_ownerID))
                        DispelSpellEffectsOn(t, proxyID);
                }
                // STOP THE ENGINE CHANNEL. A concentration proxy cast starts a real
                // channel on the follower's kInstant caster that drains him per-second
                // INDEPENDENT of MFO's per-beat apply (the runaway drain with no
                // FORCE-CAST log). Dispelling the TARGET's AE does not stop the
                // CASTER-side channel; interrupt it so the drain ends and the next
                // stream starts clean (fixes "1st heal stops, 2nd doesn't").
                if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_ownerID))
                    if (auto* mc = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant))
                        mc->InterruptCast(false);
                // Free the owner's proxy slot (form kept for reuse) AFTER the channel is
                // interrupted -- the slot is never reconfigured while its channel lives.
                ConcProxy::Free(a_ownerID);
            });
        }
        // A BENEFICIAL Health effect = a heal (restore/fortify Health, instant or
        // over time) -- the mirror of IsDamageEffect: primaryAV Health but NOT
        // detrimental/hostile. Field #2: the per-target need gate must key off the
        // spell's EFFECTS, not solely CasterConsent::ClassifySpell -- a restore-
        // health spell the classifier does NOT tag kHeal was falling through to the
        // generic whole-party beneficial fan, so a full-HP player got healed merely
        // because a DIFFERENT ally was hurt. Any spell carrying a heal effect is now
        // gated by each target's own HP.
        bool IsHealEffect(RE::EffectSetting* a_mgef) {
            if (!a_mgef) return false;
            if (a_mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
            return !a_mgef->IsDetrimental() && !a_mgef->IsHostile();
        }

    }

        // Does a_spell restore Health to its target? (Any heal effect at all.)
        bool SpellHealsHealth(RE::SpellItem* a_spell) {
            if (!a_spell) return false;
            for (auto* eff : a_spell->effects)
                if (eff && IsHealEffect(eff->baseEffect)) return true;
            return false;
        }

    // See Actuation.h. Restoration = not Offense AND (restores Health OR any
    // effect of the Restoration school). The school read is the EffectSetting's
    // own data field (pinned CommonLibSSE-NG 3.7.0 include/RE/E/EffectSetting.h:72
    // `associatedSkill`, a plain member at +0x10 of Data -- NOT the
    // SpellItem::GetAssociatedSkill vfunc, which this worker-side predicate must
    // not dispatch through the game's vtable). Offense is decided FIRST by the
    // same ClassifySpell every other road uses, so a hostile Restoration-school
    // spell (Sun Fire, Turn Undead) stays on the AI-fired offense road.
    bool IsRestorationSpell(RE::SpellItem* a_spell) {
        if (!a_spell) return false;
        if (CasterConsent::ClassifySpell(a_spell) == CasterConsent::SpellKind::Offense) return false;
        if (SpellHealsHealth(a_spell)) return true;
        for (auto* eff : a_spell->effects) {
            auto* base = eff ? eff->baseEffect : nullptr;
            if (base && base->data.associatedSkill == RE::ActorValue::kRestoration) return true;
        }
        return false;
    }

    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell, std::uint32_t a_stopPct) {
        // RUNTIME GATE: Runtime::CastPathsVerified() = the AE bucket (pre-existing)
        // or EXACTLY 1.5.97 (feat/mfo-1.5.97-pass, 2026-09-15; was IsAE()-only as
        // the T#67 SE crash gate). The T#67 fault was Loadout::LeftHandSlot()'s
        // GetObject read of BGSDefaultObjectManager (+0xB80 poison on SE) -- now a
        // FormID lookup (Docs/ADDRESS-TABLE-2026-09-15.md rows "held branch
        // Loadout.cpp:63,81 LookupByID<BGSEquipSlot>(0x00013F43)" + "BGSDefault
        // ObjectManager::objects[]/objectInit[] count": 364 SE / 366 AE).
        // Everything else this path touches on 1.5.97 is CONFIRMED there too: the
        // forced-cast package route (row "Packages.cpp:302-305 TESQuest::
        // ForceRefTo" 24523/25052), the CombatController reads (row "attackerHandle
        // @0x28, targetHandle @0x2C, combatStyle @0x38 (all < 0x68)"; the
        // `combatGroup` member at +0x00 is not a table row -- it is CommonLib's own
        // SE-origin declaration, right on SE by construction), and the seats a
        // claimed cast rides on (rows "CasterConsent.cpp:1087-1095 14 seat
        // vtables", "CombatStyle.cpp:384-417 CombatInventoryItemMagicT" 30/30,
        // "GetMagicTarget sret" same ABI). VR and every other 1.5.x stay refused:
        // the 1.5 values were confirmed on 1.5.97 only (Runtime.h). Mirrors
        // CastOn / CastTargetDirect / CastAuto / ComposedCast::Enabled.
        if (!Runtime::CastPathsVerified())   return SelfCast::Declined;
        if (!a_follower || !a_spell) return SelfCast::Declined;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();

        // §5.3 COMPETENCE: real cost gates the cast; the reserve floor keeps a
        // self-heal from emptying the pool. Unaffordable -> transparent decline.
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return SelfCast::Declined;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return SelfCast::Declined;
            }
        }

        // COMPOSED FORCED CAST (OPT-IN bHealAnimPackage, default OFF): try to
        // claim this SELF cast as an APMF kIntent_Cast claim (APMF's engine seats
        // drive the AI to equip+animate+fire it natively, movement kept) instead
        // of the kInstant force-apply below. Placed AFTER the competence gate so
        // an unaffordable cast declines exactly as today. ComposedCast::Try is
        // fully self-gating (HEAL-ONLY; AE + APMF + toggle). FOUR outcomes:
        //   Claimed       -> APMF owns the cast, return Applied with no engine call.
        //   NotApplicable -> offense/buff kind, unverified runtime, toggle off, APMF ABSENT, or
        //                    an APMF too old to carry the facet: the kInstant path
        //                    below runs, BYTE-IDENTICAL to today. That is the
        //                    declared degrade contract (marth 2026-09-07: "without
        //                    APMF, it needs to just work, whatever unpolished way
        //                    works"). Was called `Refused` before
        //                    fix/mfo-no-decline-fallback split that value in two.
        //   ApmfRefused   -> APMF is PRESENT and CAPABLE and said NO. FAIL CLOSED:
        //                    Declined, logged loudly, NO kInstant force-apply. MFO
        //                    never routes a cast around a live APMF (principle 7 --
        //                    masking this would hide the APMF bug behind a heal
        //                    that silently keeps landing unanimated).
        //   Held          -> see below.
        // a_stopPct forwards through unchanged.
        //
        // HELD (Fable SEV-2, 2026-09-06): a different spell's live claim owns this
        // follower's single heal slot, so nothing was claimed AND nothing was
        // applied. It used to arrive folded into Try()'s `true` and therefore came
        // back from here as Applied -- a spell that was never cast, logged as fired,
        // buying the suppression window and re-stamping the Task-2 hand lock.
        // Forward it as its own SelfCast value so every caller decides about it
        // explicitly. NOT a fall-through to the kInstant apply below: the whole
        // point of the hold is that the incumbent claim keeps the slot for its
        // charge window, and silently landing this spell's effect anyway would
        // erase the very condition the incumbent is being given time to satisfy.
        // ORTHOGONAL to ApmfRefused: Held is MFO-internal slot ownership between
        // two heals (APMF was never asked), ApmfRefused is APMF's own answer.
        //
        // RESTORATION TAKES THE DIRECT ROAD, NOT THIS CLAIM (fix/mfo-combat-
        // restoration-direct, 2026-09-21 -- see IsRestorationSpell in Actuation.h
        // for the field shape). A self-heal is a restoration cast by definition,
        // so the ask below is skipped for it and the kInstant direct force further
        // down delivers -- the same bounded delivery the OOC logistics heal has
        // always used. Try() is HEAL-ONLY and every Heal-kind spell IS
        // restoration, so this ask is now reachable only if the two classifiers
        // ever diverge; it is kept (not deleted) so the F1 hold / ApmfRefused
        // contract stays compiled and its removal is its own brief.
        const auto selfKind = CasterConsent::ClassifySpell(a_spell);
        if (!IsRestorationSpell(a_spell)) {
            switch (ComposedCast::Try(a_follower, a_spell, a_follower, selfKind, a_stopPct)) {
            case ComposedCast::TryResult::Claimed: return SelfCast::Applied;
            case ComposedCast::TryResult::Held:    return SelfCast::Held;
            case ComposedCast::TryResult::ApmfRefused:
                // Heals are LEFT always (ClaimHealCast's own hard rule); target 0 = self.
                LogApmfRefusal(id, "heal-cast (self)", spellID, /*target=*/0, "left");
                return SelfCast::Declined;
            // NO `default:` (F3-6, deploy-gate review 2026-09-07): all four enumerators
            // are listed, so adding a fifth TryResult must BREAK THE BUILD here rather
            // than silently take the kInstant path below -- exactly the masked fallback
            // fix/mfo-no-decline-fallback removed.
            case ComposedCast::TryResult::NotApplicable:
                break;
            }
        }

        // TASK 1 (feat/cast-gambit-concentration): a non-heal (Offense/Buff)
        // CONCENTRATION self-cast never reached the engine-seat path above --
        // ComposedCast::Try is HEAL-ONLY by design (ComposedCast.h; the
        // offense-cast-seats port kept that gate rather than widen it), so a
        // channelled self-buff/damage stream always fell straight to the
        // kInstant direct-force beat below, never AI-fired/animated. Claim the
        // SAME kIntent_Cast facet directly instead: APMFBridge::ClaimOffenseCast
        // already carries an a_concentration parameter WIRED for exactly this
        // ("a future AI-driven concentration offense call site without
        // inventing one here" -- APMFBridge.h) and left unused until now.
        // target=0 (self, matching ClaimHealCast's own convention), hand=LEFT,
        // stopPct=0 (a heal-only concept -- no threshold in scope for offense/
        // buff). Reuses the SAME [cfc] silent-claim diagnostic offense's owned-
        // cast branch already arms (ComposedCast::WatchClaim, NOT a Try() gate
        // widening). A false is SPLIT since fix/mfo-no-decline-fallback (marth
        // 2026-09-07): APMF present + CAPABLE (OffenseCastClaimSupported) means it
        // REFUSED -> FAIL CLOSED (Declined, logged loudly, no direct-force stream);
        // otherwise the facet was never there (ABI < 5 / toggle raced off) and the
        // direct-force stream below runs, byte-identical to today.
        // `!IsRestorationSpell` (fix/mfo-combat-restoration-direct, 2026-09-21):
        // a Restoration-school concentration self-cast (a ward) is a restoration
        // cast and takes the direct-force stream below, like every other
        // restoration cast in combat; only a non-restoration Offense/Buff stream
        // still claims the AI-fired road here.
        if (selfKind != CasterConsent::SpellKind::Heal && !IsRestorationSpell(a_spell) &&
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
            APMFBridge::Available() && Config::g_apmfCast.load() &&
            !Config::g_legacyCastHybrid.load()) {
            if (APMFBridge::ClaimOffenseCast(id, spellID, /*target=*/0,
                                             APMFBridge::kApmfHandLeft,
                                             /*concentration=*/true, /*stopPct=*/0)) {
                // Fetch the SAME claim's minted delivery-flip proxy (ABI v6; 0
                // on ABI < 6 or no proxy minted) so the watch recognises a cast
                // of the proxy as this claim firing (cast-claim observability,
                // 2026-09-06).
                ComposedCast::WatchClaim(id, spellID, APMFBridge::kApmfHandLeft,
                                         APMFBridge::GetOffenseCastProxy(id, APMFBridge::kApmfHandLeft));
                return SelfCast::Applied;
            }
            if (APMFBridge::OffenseCastClaimSupported()) {
                LogApmfRefusal(id, "concentration offense-cast (self)", spellID,
                               /*target=*/0, "left");
                return SelfCast::Declined;
            }
            // APMF absent for this facet (ABI < 5 / bApmfCast off) -- APMFBridge
            // already warns ONCE per session about a too-old ABI, so nothing is
            // logged here. Fall through to the direct-force stream (degrade).
        }

        const auto now = SelfClock::now();
        auto it = g_selfCast.find(id);

        // A DIFFERENT spell was channeling -> end it (stop its VFX) before this
        // one, so shaders never stack.
        if (it != g_selfCast.end() && it->second.spell != spellID) {
            // CHARGE FOR TIME: settle the old stream's unpaid seconds first.
            PostSettle(id, it->second.spell,
                       SettleSec(it->second.timed, it->second.paidThrough, it->second.lastApply,
                                 it->second.window, now),
                       "self-stream", "switch");
            SelfCastEndActor(id, it->second.spell);
            g_selfCast.erase(it);
            it = g_selfCast.end();
        }

        if (it == g_selfCast.end()) {
            // FIRST fire. DELIBERATELY DO NOT EQUIP THE SPELL. CastSpellImmediate
            // (kInstant, in ApplySelfEffect) applies the effect without the spell
            // in hand, so the follower is NEVER left holding it. Leaving a light
            // spell (Candlelight/Magelight) equipped let the follower's OWN AI
            // spam-cast it -- 55 non-MFO lights piled up to a ShadowSceneNode
            // light-limit CTD (deck 2026-08-19). The animation is deferred anyway,
            // so the equip/HoldStow/caster-drive scaffolding is dropped: MFO casts
            // the chosen self-spell ONCE, it applies, and the already-active guard
            // (ApplySelfEffect) blocks re-casting until the effect expires.
            auto& sc = g_selfCast[id];
            sc.spell = spellID; sc.started = now;
            sc.lastFired = now; sc.lastApply = {};   // epoch -> apply the effect immediately
            sc.cap = DrawConcCap(CasterConsent::ClassifySpell(a_spell));   // per-stream random cap
            // CHARGE FOR TIME: a momentary concentration self stream is billed for
            // the seconds it ran; window = the sustain window ApplySelfEffect pins.
            sc.timed  = ConcMomentary(a_spell);
            sc.window = CasterConsent::ClassifySpell(a_spell) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcSelfUtilityCap;
            sc.paidThrough = {};
            it = g_selfCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. Callers refresh this every service/combat tick
        // while the rule wins. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts the seconds run since the last charge (BeatChargeSec --
        // CHARGE FOR TIME, v2.0.12) and re-arms that effect's rolling
        // window (fCastCooldown pacing would under-charge 4x and let the
        // channel lapse between re-arms -- "heals feel broken"). A
        // FIRE-AND-FORGET spell keeps the configured fCastCooldown beat (its
        // magnitude is per CAST). A duration buff is additionally capped to
        // once per effect-duration by the already-active guard in
        // ApplySelfEffect, so the 1 s beat can never stack a still-up ward.
        const float interval =
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                ? kConcApplyPeriod
                : std::max(1.0f, Config::g_castCooldown.load());
        if (std::chrono::duration<float>(now - it->second.lastApply).count() >= interval) {
            const float chargeSec = BeatChargeSec(it->second.timed, it->second.paidThrough,
                                                  it->second.lastApply, it->second.window, now);
            it->second.lastApply = now;
            ApplySelfEffect(id, spellID, chargeSec);
            return SelfCast::Applied;   // effect + magicka landed THIS tick
        }
        // Rule winning, channel kept alive, but paced out this tick -- a
        // refresh, NOT an action (F3: the caller must not let it suppress the
        // rules below it, or an "always -> cast_self" starves loot/drink).
        return SelfCast::Refreshed;
    }

    void SelfCastReconcile() {
        if (g_selfCast.empty()) return;
        const auto  now = SelfClock::now();
        // Release when the rule stops re-firing. The callers refresh lastFired
        // every service/combat tick while the rule wins, so this only needs to
        // out-wait the round-robin gap (one follower serviced per ~133 ms tick),
        // not the cast cooldown. 2 s covers a large party and still releases
        // promptly when the rule goes false / is disabled.
        //
        // DURATION FIX (field-confirmed): there is NO time cap here. An earlier
        // `capped = started > 30 s` force-released the channel -- and thus
        // DISPELLED the buff -- 30 s after the FIRST cast even while the rule was
        // still winning. That tore a 300 s Candlelight down at 30 s, so the
        // already-active guard saw it expire and re-cast on a rock-steady ~30 s
        // beat (deck 2026: 22:44:49 -> :45:19 -> :45:46 ...). CastSpellImmediate
        // already applies the spell's FULL authored duration/magnitude
        // (magnitudeOverride 0 = use the effect's own); the cap was the only
        // thing shortening it. Release now hinges solely on the rule going stale
        // (or the follower unloading), so a long buff lives its authored life and
        // re-casts at its real expiry.
        // #5 DISPEL-BEAT: a fixed 2 s window is too short in COMBAT. There the
        // rule only re-fires when the follower is BOTH serviced (round-robin,
        // ~133 ms x party) AND past his suppression window (fSuppressWindow x
        // temperament, up to ~1.12x). For party >= 3 that gap exceeds 2 s, so the
        // channel goes "stale", we DISPEL the live self-buff, and the next fire
        // re-applies it -- the exact re-cast beat the 300 s-cap fix removed, back
        // at window cadence. Size the release to out-wait the worst-case gap:
        //   fSuppressWindow*1.12 (max temperament) + 0.133*partySize + margin,
        // floored at the old 2 s so it never releases SLOWER-to-react than before
        // when the party is small / the window short.
        const float suppress   = std::max(0.0f, Config::g_suppressWindow.load());
        // SEV-1: size the party from the lock-guarded snapshot, never a raw
        // g_active.size() -- this runs on the job worker and a concurrent
        // Followers::Refresh (death/loot sink) can be rebuilding g_active (#4).
        const auto  snap       = Followers::ActiveSnapshot();
        const float partySize  = static_cast<float>((snap ? snap->size() : 0) + 1);  // + player
        const float releaseSec = std::max(2.0f,
                                          suppress * 1.12f + 0.133f * partySize + 0.5f);
        std::vector<RE::FormID> done;
        for (auto& [id, sc] : g_selfCast) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            const bool gone   = !a || !a->Is3DLoaded();
            const bool stale  = std::chrono::duration<float>(now - sc.lastFired).count() > releaseSec;
            // SAME-TIME-LIMIT (marth) + HEAL-MUST-FLOW (coordinator): classify the
            // channel once, for BOTH the concentration cap AND the sticky-dispel gate.
            //   * The CAP applies to CONCENTRATION only (an FF buff lives its authored
            //     duration -- the no-time-cap fix): HEAL -> kConcHealCap (6s), WARD/
            //     UTILITY(Buff) -> kConcSelfUtilityCap (15s, marth -- not the 4s
            //     package hold, which would only FLICKER a non-rooting self ward).
            //   * The cap RELEASES + re-streams; whether release DISPELS is the
            //     sticky gate below, NOT the cap. A momentary HEAL/DAMAGE is capped
            //     but NEVER dispelled, so the entry just re-creates and re-applies
            //     next tick -- the heal FLOWS UNINTERRUPTED (this is why marth's
            //     "not healing himself either" cannot be this cap: dispel is skipped
            //     for Heal, and an instant restore-Health has no active effect to rip).
            CasterConsent::SpellKind kind = CasterConsent::SpellKind::Buff;   // default sticky
            bool concCapped = false;
            bool healedFull = false;
            bool magickaDry = false;
            if (!gone) {
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sc.spell)) {
                    kind = CasterConsent::ClassifySpell(sp);
                    if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                        // RANDOMIZED per-stream cap (sc.cap: heal/utility 8-15s) --
                        // GUARANTEES the self stream ends; the gambit re-serves a
                        // still-wanted buff as a fresh burst.
                        if (std::chrono::duration<float>(now - sc.started).count() >= sc.cap)
                            concCapped = true;
                        // Self-HEAL stops early at ~full own HP (marth: heal to 100%).
                        if (kind == CasterConsent::SpellKind::Heal && a &&
                            Vocab::HealthPct(a) >= kHealFullPct)
                            healedFull = true;
                        // MAGICKA-OUT STOP: end the channel the moment the caster can't
                        // afford the next beat, instead of re-applying for free at 0.
                        if (auto* avo = a->AsActorValueOwner();
                            avo && avo->GetActorValue(RE::ActorValue::kMagicka) <
                                       sp->CalculateMagickaCost(a))
                            magickaDry = true;
                    }
                }
            }
            if (gone || stale || concCapped || healedFull || magickaDry) {
                // RELEASE. DISPEL (+ interrupt the channel, in SelfCastEndActor) a
                // lingering STICKY buff on any release, AND a momentary stream's
                // SUSTAINED effect when it truly ENDED (stale / heal-full / magicka-out).
                // A plain CAP-only release re-streams next tick (fresh random cap),
                // keeping a wanted self buff/heal continuous across bursts (self has no
                // proxy slot to orphan). There is NO equip to undo.
                const char* reason = healedFull ? "heal-full" : magickaDry ? "magicka-out"
                                   : (a && !stale && !concCapped) ? "gone"
                                   : concCapped ? "cap" : "stale";
                spdlog::info("[cast] {:08X} self-stream RELEASE ({}) spell {:08X}", id, reason, sc.spell);
                // CHARGE FOR TIME: settle the seconds the effect ran past the last
                // paid beat (skipped when the caster is gone: nothing to bill).
                if (a)
                    PostSettle(id, sc.spell,
                               SettleSec(sc.timed, sc.paidThrough, sc.lastApply, sc.window, now),
                               "self-stream", reason);
                if (a && (kind == CasterConsent::SpellKind::Buff || stale || healedFull || magickaDry))
                    SelfCastEndActor(id, sc.spell);
                done.push_back(id);
            }
        }
        for (const auto id : done) g_selfCast.erase(id);
    }

    void ClearSelfCasts() {
        // Revert/load: drop the channels. No engine call -- the world is being
        // replaced, and the self-cast holds no equip/debt to undo.
        g_selfCast.clear();
        g_targetCast.clear();         // on-target direct-force streams, likewise
        g_autoCast.clear();           // AUTO fan-out pacing is session-scoped too
        g_beneficialRecast.clear();   // fix #3/#6: per-buff recast windows likewise
        ConcProxy::Reset();           // null dangling 0xFF proxy forms + drop borrowed
                                      // source Effect* (cross-load UAF / double-free)
        CastBounds::Reset();          // drop every MFO-executed-cast bound (§2 registry)
        ComposedCast::Reset();        // drop executor streams/backoff/expected-cast set
        ClearCastLocks();             // Task 2: drop every firing-spell gambit lock
        g_lastApmfRefusal.clear();    // this file's APMF-refusal log dedup, session-scoped
        g_summonPosted.clear();       // summon post throttle (worker state, session-scoped)
        {
            std::lock_guard lk(g_summonMx);   // main-written verdicts / last casts
            g_summon.clear();
        }
    }

    // F3-7 (deploy-gate review 2026-09-07). Drop ONE follower's APMF-refusal log
    // dedup entries. Actuation.cpp's twin map is erased per follower from
    // ClearCastLock (dismissal / combat end) as well as wholesale on revert; this
    // copy was only ever `.clear()`ed on revert (ClearSelfCasts above), so the two
    // maps documented as identical twins were not identical. The effect was
    // harmless -- a stale entry can only DELAY one error line, by at most the 5 s
    // window, and never suppress a different (spell, target) -- but a twin that
    // silently differs from its documented twin is how the next reader is misled.
    // Called from ClearCastLock (Actuation.cpp) so both maps share one release
    // point. Idempotent; worker-serial, no lock (#4), same as the map itself.
    void ClearApmfRefusalLog(RE::FormID a_follower) { g_lastApmfRefusal.erase(a_follower); }

    // ON-TARGET DIRECT FORCE = CastSelfDirect generalized to a NON-self target.
    // The known-working, package-lock-proof delivery for a concentration cast at a
    // player / ally / foe: no package -> no §4.6 alias-lock decline, so a package-
    // locked custom follower (Lucien) actually heals the player and damages the foe.
    // Registers a single per-follower stream, paces the apply at kConcApplyPeriod
    // (~1 s -- a concentration magnitude is authored per second; FF spells keep
    // fCastCooldown), and is time-bounded + released by TargetCastReconcile.
    // Returns Applied/Refreshed/Declined with the SAME semantics as CastSelfDirect
    // (a paced REFRESH is NOT an action). LoS + line-of-fire GATE hostile offense
    // (never direct-apply damage into a wall or through a teammate). Callers:
    // Logistics OOC cast dispatch AND combat's ConcentrationCast -- BOTH primary,
    // no package anywhere on the concentration delivery. Worker-serial state;
    // the engine apply itself is posted to the MAIN thread (ApplyTargetEffect).
    SelfCast CastTargetDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                              RE::Actor* a_target, std::uint32_t a_stopPct) {
        // RUNTIME GATE (Runtime::CastPathsVerified(): AE bucket or exactly 1.5.97)
        // -- the same lift, the same Docs/ADDRESS-TABLE-2026-09-15.md rows and the
        // same reasoning as CastSelfDirect's gate above (T#67's fault was the
        // LeftHandSlot GetObject read, now a FormID lookup; ForceRefTo 24523/25052;
        // CombatController < 0x68; the seats).
        if (!Runtime::CastPathsVerified())   return SelfCast::Declined;
        if (!a_follower || !a_spell || !a_target) return SelfCast::Declined;
        if (a_target == a_follower)          return SelfCast::Declined;   // self -> CastSelfDirect
        const auto id       = a_follower->GetFormID();
        const auto spellID  = a_spell->GetFormID();
        const auto targetID = a_target->GetFormID();
        const auto kind     = CasterConsent::ClassifySpell(a_spell);

        // §5.3 COMPETENCE: real cost gates the cast; the reserve floor keeps it from
        // emptying the pool. Unaffordable -> transparent decline (mirrors CastSelfDirect).
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return SelfCast::Declined;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return SelfCast::Declined;
            }
        }

        // COMPOSED FORCED CAST (OPT-IN bHealAnimPackage, default OFF): try to
        // claim this ON-TARGET cast as an APMF kIntent_Cast claim (a heal at the
        // player/an ally -- APMF's engine seats drive the AI to equip+animate+
        // fire it natively, movement kept, explicit target rides the claim so a
        // follower fighting one foe can still heal a DIFFERENT ally) instead of
        // the kInstant force-apply below. Placed AFTER the competence gate
        // (parity with CastSelfDirect) and before the offense sightline gate --
        // heals are never offense, so order there is immaterial. Self-gating
        // (HEAL-ONLY; AE + APMF + toggle) -- FOUR outcomes, see the identical
        // switch in CastSelfDirect above for the full reasoning. NotApplicable
        // degrades to the kInstant path byte-identically (the APMF-ABSENT
        // contract; this value was named `Refused` before
        // fix/mfo-no-decline-fallback split it), ApmfRefused FAILS CLOSED rather
        // than routing the heal around a live APMF, and Held (Fable SEV-2,
        // 2026-09-06) is forwarded as its own SelfCast value rather than
        // masquerading as Applied -- and is likewise not a fall-through to the
        // kInstant apply. `kind` is computed above; a_stopPct forwards unchanged.
        //
        // ── NO COMBAT AI -> NO SEATS -> NO CLAIM (2026-09-09) ────────────────────
        // marth's standing ruling, recorded at Logistics.cpp's OOC concentration
        // branch: that delivery "must always use the known working force." The
        // claim path silently overrode it the moment bHealAnimPackage went ON, and
        // on the deck it IS on. This restores the ruling for the case where the
        // claim CANNOT work at all.
        //
        // WHY IT CANNOT WORK. Every one of APMF's cast seats is a vfunc on the
        // engine's COMBAT AI objects -- CheckStartCast / CheckStopCast /
        // GetMagicTarget are `bool(CombatMagicCaster*, CombatController*)` on the
        // Restore and Offensive caster vtables, the classify seat rides the same
        // controller, and the equip gate reads CombatController::inventory (APMF
        // core/CastSeats.cpp, core/EquipGate.cpp). With no CombatController there
        // is no CombatMagicCaster to seat, so NOTHING can drive the cast. Only
        // CheckCast (t2c) still runs out of combat, and that is a DENY gate: it can
        // stop a cast, never start one.
        //
        // THE FIELD SHAPE (deck log 2026-09-09, Jesper 0x750012C6). Combat ended at
        // 14:19:14. From then until 14:21:19 -- over two minutes -- the heal claim
        // stood, MFO heartbeat-Repointed it every ~3.6 s, `[logistics] ... (APMF
        // claimed)` printed on every lap, and APMF logged ZERO seat lines for that
        // actor. The claim reported success on every tick and the player was never
        // healed once, while the direct force that would have healed him sat behind
        // this switch.
        //
        // THIS IS NOT "ROUTING AROUND A LIVE APMF" (the rule at :865-869 and
        // [[mfo-is-apmf-showpiece-legacy-is-absent-only]]). That rule forbids
        // bypassing APMF's ARBITRATION where APMF can deliver -- and an
        // ApmfRefused below still FAILS CLOSED, unchanged. Out of combat APMF has
        // no delivery mechanism to arbitrate for: this is the APMF-ABSENT degrade
        // contract, evaluated per SITUATION instead of per session.
        //
        // THE TEST IS THE OBJECT THE SEATS HANG OFF, not a mood flag. `IsInCombat()`
        // is a bool on the actor and answers a different question -- it can read
        // true through a teardown, and it says nothing about whether the controller
        // the seats need exists. `combatController` IS that controller: null means
        // the engine built no combat AI for this actor, so no seat can be reached.
        // MFO already treats it as the canonical "is this follower fighting" read
        // (CombatSense.h's FoeCount, Evaluator.cpp, Targeting.cpp), and it is a
        // plain member load through GetActorRuntimeData()'s SE/AE-shifted accessor
        // -- no vfunc, no allocation, safe from this job worker
        // (ACTOR_RUNTIME_DATA::combatController, pinned CommonLibSSE-NG 3.7.0
        // RE/A/Actor.h:657, offset 0x158; this TU already makes the identical read
        // further down). A racy read is fine and is the
        // race every other reader here already takes: worst case one lap takes the
        // other road, and both roads heal.
        //
        // IN COMBAT NOTHING CHANGES -- the claim path runs exactly as it did, and
        // that is the path that produced this session's confirmed animated heals.
        //
        // RESTORATION TAKES THE DIRECT ROAD ON THE COMBAT TABLE TOO (fix/mfo-
        // combat-restoration-direct, 2026-09-21 -- IsRestorationSpell, Actuation.h).
        // The controller test above chose the direct road OUT of combat only; in
        // combat the claim road produced the frozen follower (a left-hand claim the
        // engine never fired + the idle-hand floor on the right). So the ask is
        // made only for a spell that is NOT restoration -- and since Try() is
        // HEAL-ONLY and every Heal-kind spell is restoration, the claim road below
        // is unreachable unless the two classifiers diverge (kept compiled, same
        // reasoning as the self twin above).
        if (a_follower->GetActorRuntimeData().combatController && !IsRestorationSpell(a_spell)) {
            switch (ComposedCast::Try(a_follower, a_spell, a_target, kind, a_stopPct)) {
            case ComposedCast::TryResult::Claimed: return SelfCast::Applied;
            case ComposedCast::TryResult::Held:    return SelfCast::Held;
            case ComposedCast::TryResult::ApmfRefused:
                LogApmfRefusal(id, "heal-cast", spellID, targetID, "left");
                return SelfCast::Declined;
            // NO `default:` -- see the twin in CastSelfDirect above (F3-6).
            case ComposedCast::TryResult::NotApplicable:
                break;
            }
        }

        // TASK 1 (feat/cast-gambit-concentration): a non-heal (Offense/Buff)
        // CONCENTRATION cast at a player/ally/foe never reached the engine-seat
        // path above -- ComposedCast::Try is HEAL-ONLY by design (ComposedCast.h),
        // so a channelled damage/buff stream aimed at a target always fell
        // straight to the kInstant direct-force beat below. Claim the SAME
        // kIntent_Cast facet directly, mirroring CastOn's owned-cast branch
        // exactly (hand=LEFT, stopPct=0 -- heal-only concept) and the self twin
        // just above: APMFBridge::ClaimOffenseCast's a_concentration parameter
        // was wired for precisely this call site and left unused until now.
        // Placed BEFORE the LoS/line-of-fire gate below on purpose -- once
        // APMF's engine seats own the cast, the AI's own combat sense re-checks
        // sightline itself every charge/aim tick (the same reason the owned-cast
        // branch in CastOn never re-derives LoS either); that gate exists for
        // the kInstant fallback path only. A false is SPLIT since
        // fix/mfo-no-decline-fallback, exactly as in the self twin above: APMF
        // present + CAPABLE means it REFUSED -> FAIL CLOSED (Declined, logged);
        // otherwise the facet was never there and the gate + direct-force stream
        // below runs, byte-identical to today.
        // `!IsRestorationSpell` (fix/mfo-combat-restoration-direct, 2026-09-21):
        // a Restoration-school concentration cast at an ally is a restoration
        // cast and takes the direct-force stream below; only a non-restoration
        // Offense/Buff stream still claims the AI-fired road here.
        if (kind != CasterConsent::SpellKind::Heal && !IsRestorationSpell(a_spell) &&
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
            APMFBridge::Available() && Config::g_apmfCast.load() &&
            !Config::g_legacyCastHybrid.load()) {
            if (APMFBridge::ClaimOffenseCast(id, spellID, targetID, APMFBridge::kApmfHandLeft,
                                             /*concentration=*/true, /*stopPct=*/0)) {
                // Fetch the SAME claim's minted delivery-flip proxy (ABI v6; 0
                // on ABI < 6 or no proxy minted) so the watch recognises a cast
                // of the proxy as this claim firing (cast-claim observability,
                // 2026-09-06).
                ComposedCast::WatchClaim(id, spellID, APMFBridge::kApmfHandLeft,
                                         APMFBridge::GetOffenseCastProxy(id, APMFBridge::kApmfHandLeft));
                return SelfCast::Applied;
            }
            if (APMFBridge::OffenseCastClaimSupported()) {
                LogApmfRefusal(id, "concentration offense-cast", spellID, targetID, "left");
                return SelfCast::Declined;
            }
            // APMF absent for this facet (ABI < 5 / bApmfCast off) -- APMFBridge
            // already warns ONCE per session about a too-old ABI. Fall through to
            // the gate + direct-force stream below (degrade).
        }

        // HOSTILE offense: LoS + line-of-fire gates on the direct path too (the
        // package's ffWatch has no analog here, so re-check every tick). Held ->
        // Declined (transparent): don't apply this tick, and the un-refreshed entry
        // goes stale so TargetCastReconcile CUTS the beam, exactly like ffWatch.
        if (kind == CasterConsent::SpellKind::Offense) {
            if (Sightline::Check(id, targetID) == Sightline::Verdict::Occluded)
                return SelfCast::Declined;
            if (Sightline::TeammateInFireLine(id, targetID))
                return SelfCast::Declined;
        }

        const auto now = SelfClock::now();
        auto it = g_targetCast.find(id);
        // Spell OR target switched -> end the old stream first, ALWAYS: a ward
        // must not linger, and the SUSTAINED real effect on the OLD target
        // genuinely channels until its window elapses -- it must die with its
        // stream, not keep healing/damaging the old target unpaid. Shaders and
        // buffs never stack or stray.
        if (it != g_targetCast.end() &&
            (it->second.spell != spellID || it->second.target != targetID)) {
            spdlog::info("[cast] {:08X} stream RELEASE (switch)", id);
            // CHARGE FOR TIME: settle the old stream's unpaid seconds first.
            PostSettle(id, it->second.spell,
                       SettleSec(it->second.timed, it->second.paidThrough, it->second.lastApply,
                                 it->second.window, now),
                       "stream", "switch");
            TargetCastEndActor(it->second.target, it->second.spell, id);
            g_targetCast.erase(it);
            it = g_targetCast.end();
        }
        if (it == g_targetCast.end()) {
            auto& tc = g_targetCast[id];
            tc.spell = spellID; tc.target = targetID; tc.kind = kind;
            tc.started = now; tc.lastFired = now; tc.lastApply = {};   // epoch -> apply now
            tc.cap = DrawConcCap(kind);   // per-stream random cap (8-15s heal/util, 2-6s offense)
            // CHARGE FOR TIME: a MOMENTARY concentration stream (heal/offense --
            // ApplyTargetEffect's guard is off, so no beat is ever skipped as
            // already-active) is billed for the seconds it ran. A sticky Buff ward
            // keeps the per-application charge. window = ApplyTargetEffect's pin.
            tc.timed  = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                        kind != CasterConsent::SpellKind::Buff;
            tc.window = kind == CasterConsent::SpellKind::Heal ? kConcHealCap : kConcUtilityHold;
            tc.paidThrough = {};
            it = g_targetCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts the seconds run since the last charge (BeatChargeSec --
        // CHARGE FOR TIME, v2.0.12) and re-arms that effect's rolling
        // window (fCastCooldown pacing would under-charge 4x and let the
        // channel lapse between re-arms -- "heals feel broken"). An FF spell
        // (this function can be handed one) keeps the fCastCooldown beat; a
        // sticky concentration ward's 1 s beat is de-duplicated by the
        // already-active guard in ApplyTargetEffect.
        const float interval =
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                ? kConcApplyPeriod
                : std::max(1.0f, Config::g_castCooldown.load());
        if (std::chrono::duration<float>(now - it->second.lastApply).count() >= interval) {
            const float chargeSec = BeatChargeSec(it->second.timed, it->second.paidThrough,
                                                  it->second.lastApply, it->second.window, now);
            it->second.lastApply = now;
            // GUARD only sticky (Buff) buffs; heal/damage re-apply freely (momentary).
            ApplyTargetEffect(id, targetID, spellID, kind == CasterConsent::SpellKind::Buff, chargeSec);
            return SelfCast::Applied;
        }
        return SelfCast::Refreshed;   // winning but paced out this tick (transparent)
    }

    void TargetCastReconcile() {
        if (g_targetCast.empty()) return;
        const auto now = SelfClock::now();
        // Same release window as SelfCastReconcile: out-wait the worst-case
        // round-robin + suppression gap so a still-winning rule is never torn down.
        // (Was the KNOWN SEV-1 shared with SelfCastReconcile: g_active is main-
        // thread/serial-task-only (#4) and this runs on the job worker. FIXED in
        // the concurrency wave -- the party size now reads the lock-guarded
        // snapshot, immune to a concurrent Followers::Refresh reallocating g_active.)
        const float suppress   = std::max(0.0f, Config::g_suppressWindow.load());
        const auto  snap       = Followers::ActiveSnapshot();
        const float partySize  = static_cast<float>((snap ? snap->size() : 0) + 1);
        const float releaseSec = std::max(2.0f, suppress * 1.12f + 0.133f * partySize + 0.5f);
        std::vector<RE::FormID> done;
        for (auto& [id, tc] : g_targetCast) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(id);
            auto* t = RE::TESForm::LookupByID<RE::Actor>(tc.target);
            const bool gone  = !f || !f->Is3DLoaded() || !t || !t->Is3DLoaded() || t->IsDead();
            const bool stale = std::chrono::duration<float>(now - tc.lastFired).count() > releaseSec;
            // RANDOMIZED PER-STREAM TIME CAP (tc.cap, drawn at stream start:
            // heal/utility 8-15s, offense 2-6s). GUARANTEES the stream ends even if
            // the gambit condition is unreliable; the gambit re-evaluates between
            // bursts (a satisfied target is not re-served). A plain cap on a still-
            // needed HEAL is release-only -- the next winning tick re-streams it with
            // a FRESH random cap (varied human bursts), so it FLOWS while wounded.
            const bool capped = std::chrono::duration<float>(now - tc.started).count() >= tc.cap;
            // HEAL also ends EARLY at ~full recipient HP (marth: "heal always to
            // 100%") -- a true END-of-stream, dispel + stop.
            const bool healedFull = tc.kind == CasterConsent::SpellKind::Heal && t &&
                                    Vocab::HealthPct(t) >= kHealFullPct;
            // MAGICKA-OUT STOP (marth: "a held cast should stop when magicka runs").
            // The moment the CASTER can't afford the next beat's cost, END the stream
            // (a true end-of-stream, dispel) instead of re-applying for free at 0 --
            // this is what makes the long caps safe (no over-drain), and it stops the
            // never-ending re-apply that would otherwise churn every beat.
            bool magickaDry = false;
            if (!gone) {
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(tc.spell)) {
                    if (auto* avo = f->AsActorValueOwner();
                        avo && avo->GetActorValue(RE::ActorValue::kMagicka) <
                                   sp->CalculateMagickaCost(f))
                        magickaDry = true;
                }
            }
            if (gone || stale || capped || healedFull || magickaDry) {
                // EVERY release is a TRUE END (marth's burst model): dispel the
                // sustained effect, INTERRUPT the engine channel, and FREE the proxy
                // slot (all in TargetCastEndActor). A plain cap thus ends the burst
                // cleanly and the gambit re-serves a still-wounded target as a FRESH
                // stream next tick (new slot, new channel) -- this both guarantees the
                // channel always stops (no runaway) and avoids orphaning an owned
                // proxy slot whose stream was erased. Breadcrumb names the reason.
                const char* reason = healedFull  ? "heal-full"
                                   : magickaDry  ? "magicka-out"
                                   : gone        ? "gone"
                                   : stale       ? "stale"
                                                 : "cap";
                spdlog::info("[cast] {:08X} stream RELEASE ({}) tgt {:08X} spell {:08X}",
                             id, reason, tc.target, tc.spell);
                // CHARGE FOR TIME: settle the seconds the effect ran past the last
                // paid beat -- posted before the dispel, same main-thread queue.
                // Skipped when the CASTER is gone (nothing to bill); a gone TARGET
                // still settles, the effect ran on it until now.
                if (f)
                    PostSettle(id, tc.spell,
                               SettleSec(tc.timed, tc.paidThrough, tc.lastApply, tc.window, now),
                               "stream", reason);
                TargetCastEndActor(tc.target, tc.spell, id);   // dispel + interrupt + free slot
                done.push_back(id);
            }
        }
        for (const auto id : done) g_targetCast.erase(id);
    }

    // See Actuation.h. The registry entry IS the direct road's footprint.
    bool TargetStreamLive(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target) {
        const auto it = g_targetCast.find(a_follower);
        return it != g_targetCast.end() && it->second.spell == a_spell && it->second.target == a_target;
    }

}
