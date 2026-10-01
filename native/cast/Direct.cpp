// cast/Direct.cpp -- the DIRECT-DELIVERY road's substrate: its state (the self and
// target stream registries, the APMF-refusal log), the apply substrate (ConcProxy,
// dispel, Apply{Self,Target}Effect, charge-for-time), SpellHealsHealth /
// IsRestorationSpell, and the revert reset ClearSelfCasts. The streams themselves
// live in cast/DirectSelf.cpp (CastSelfDirect) and cast/DirectTarget.cpp
// (CastTargetDirect), the can-act gate in cast/CanAct.cpp, [heal-obs] in
// cast/HealObs.cpp. CastAuto lives in cast/Auto.cpp, summons in cast/Summon.cpp.
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24), then cut again by the Direct.cpp split (2026-09-29, MFO-B152): pure
// moves, proven function by function with tools/splitcheck.
#include "Direct_internal.h"
#include "ComposedCast.h"   // the Composed Forced Cast executor -- replaces the deleted
                            // HealAnimFill package route at the two cast plug-ins below
#include "CastBounds.h"     // Reset the MFO-executed-cast bound beside ConcProxy::Reset()

namespace MFO::Actuation {

        std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // worker-serial

    namespace {
        // OocHealWaitsForClaim's line dedup (MFO-B173): one per follower per 5 s.
        // Worker-serial, no lock (#4); cleared on revert in ClearSelfCasts.
        std::unordered_map<RE::FormID, SelfClock::time_point> g_oocHealWaitLog;
    }

    namespace {
        // [heal-obs] road label of the direct stream (fix/mfo-lifestate, log text only): a
        // caster out of combat (the controller just vanished, ChooseHealRoad's DirectNoCombat)
        // reads "OOC stream", not "direct stream", so it is never mistaken for an in-combat
        // fallback. HealObs.cpp's sustainsItself accepts both prefixes.
        const char* StreamRoadLabel(const RE::Actor* a_caster) {
            return (a_caster && a_caster->IsInCombat()) ? "direct stream" : "OOC stream (no combat controller)";
        }
    }

    namespace {
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
            // The minted forms' FormIDs, readable from ANY thread (feat/mfo-animheal-p0,
            // 1m(c)): the job worker publishes them on the follower's ch.8 allow-list
            // (APMFBridge::PublishSpellAllowList) while only the main thread touches
            // g_slot. Written when a slot's form is created, cleared by Reset.
            std::atomic<RE::FormID> g_slotFormId[kSlotCount]{};

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
                        g_slotFormId[&s - g_slot].store(s.form->GetFormID(), std::memory_order_release);
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
                for (auto& id : g_slotFormId) id.store(0, std::memory_order_release);
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
                // CAN HE ACT (fix/mfo-can-act)? Re-checked HERE, right before the
                // apply: the worker decided a frame or more ago and he may have gone
                // down since. Refused = no cast, no magicka, logged once.
                if (const char* why = CannotActReason(a)) {
                    NoteRefusedApply(a_id, "self-cast", why, a_spellID, a_id);
                    return;
                }
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
                NoteArchetypeRoad(a, sp, ArchRoad::Direct);   // [archetype] probe (passive)
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                const bool  heal     = SpellHealsHealth(sp);
                const float hpBefore = Vocab::HealthPct(a);   // [heal-obs]: read BEFORE the cast
                // READ-BACK (feat/mfo-animheal-p0, 1m(a), principle 7): the apply is
                // reported as landed only when an effect of it is on the recipient after
                // the cast. See ApplyTargetEffect for the full reasoning.
                HealAttach attach = HealAttach::Instant;
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
                    attach = HealAttach::Present;   // the live effect was found and re-armed
                    if (!SustainConcentrationEffect(a, sp, window)) {
                        CastBreadcrumb("direct-self", a, sp, a_id);   // [cast-call], flushed
                        inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);   // attach ONCE
                        if (SustainConcentrationEffect(a, sp, window)) {               // then pin it
                            // Evidence line: ONCE per stream if the engine honors the
                            // pinned duration; repeating every beat = sustain refused.
                            spdlog::info("[cast] {:08X} conc effect ATTACHED on self "
                                         "(spell {:08X}, window {:.0f}s)", a_id, a_spellID, window);
                        } else {
                            // Only a delivery that applies inside the call is decidable
                            // now (DeliveryAppliesInCall); any other keeps its charge.
                            attach = DeliveryAppliesInCall(sp) ? HealAttach::Absent : HealAttach::Instant;
                        }
                    }
                } else {
                    CastBreadcrumb("direct-self", a, sp, a_id);   // [cast-call], flushed
                    inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);
                    if (HasDurationEffect(sp) && DeliveryAppliesInCall(sp))
                        attach = SpellEffectPresentOn(a, sp) ? HealAttach::Present : HealAttach::Absent;
                }
                if (attach == HealAttach::Absent &&
                    CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Offense) {
                    if (std::uint32_t held = 0; ReadbackWarnDue(a_id, a_spellID, ReadbackWarn::NotLanded, held))
                        spdlog::warn("[cast] {:08X} {} SELF-CAST {} ({:08X}) -- NOT LANDED: no effect of it on "
                                     "him after the cast; no magicka spent (+{} held in 5s)", a_id,
                                     a->GetName() ? a->GetName() : "?",
                                     sp->GetName() ? sp->GetName() : "?", a_spellID, held);
                    if (heal) HealObsNote(a, a, sp, sp, "direct self", hpBefore, attach);
                    return;
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
                if (heal) {
                    NoteHealLanded(a_id, a_id, a_spellID);   // [bleed] attribution
                    HealObsNote(a, a, sp, sp, "direct self", hpBefore, attach);
                }
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
                // fix/mfo-can-act: re-check the CASTER (he may have gone down since the
                // worker decided) and, for a heal, its normal reach + a synchronous LoS
                // measure. Either refusal = no cast, no magicka.
                if (const char* why = CannotActReason(caster)) {
                    NoteRefusedApply(a_casterID, "force-cast", why, a_spellID, a_targetID);
                    return;
                }
                if (RefuseHealApplyOnMain(caster, tgt, sp, "force-cast")) return;
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
                NoteArchetypeRoad(caster, sp, ArchRoad::Direct);   // [archetype] probe (passive)
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                const bool  heal     = SpellHealsHealth(sp);
                const float hpBefore = Vocab::HealthPct(tgt);   // [heal-obs]: read BEFORE the cast
                // READ-BACK (feat/mfo-animheal-p0, 1m(a), principle 7). "effect applied" /
                // "conc effect ATTACHED" used to print unconditionally, while Harbinger
                // refused 48 of MFO's own ConcProxy casts on the instant caster (field
                // 2026-09-28, `hand=?`). The apply now reports landed only when an effect
                // OF THE FORM CAST (the spell or its proxy) is on the recipient after the
                // cast. Decided ON THIS TICK only when the form cast has a kSelf or
                // kTargetActor delivery (DeliveryAppliesInCall): an Aimed/TargetLocation
                // spell launches a projectile whose effect arrives frames later, so it is
                // undecidable now, keeps its charge and [heal-obs] answers it. Within
                // those deliveries: a spell with a duration effect is read back directly.
                // The CONCENTRATION attach relies on the second SustainConcentrationEffect
                // finding the new effect on this same tick -- that premise is UNPROVEN
                // (review of 22d735d, SEV-3): the 09-28 "ATTACHED once per stream" lines
                // printed unconditionally then, and a channel CastSpellImmediate leaves
                // running on the instant caster keeps its effect alive either way. Its
                // FALSIFIER is in Docs/STATUS.md's field plan: `conc effect NOT ATTACHED`
                // followed ~1 s later by [heal-obs] `effect at apply=ABSENT now=present`
                // with `channel=running` means the effect arrives after the call, not
                // inside it. NOT decidable for an instant fire-and-forget
                // heal (duration 0: applied and gone inside the call), which keeps its
                // charge and is answered by the [heal-obs] HP read ~1 s later. A beneficial
                // apply that did not land spends NO magicka and logs NOT LANDED; an
                // offensive one keeps its old charge (a resisted hit still costs the
                // caster) but no longer prints ATTACHED.
                HealAttach attach = HealAttach::Instant;
                RE::SpellItem* castForm = sp;
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
                    castForm = castSp;
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcUtilityHold;
                    attach = HealAttach::Present;   // the live effect was found and re-armed
                    if (!SustainConcentrationEffect(tgt, castSp, window)) {
                        // The KNOWN-WORKING FORCE -- caster casts (proxy of) sp AT tgt.
                        CastBreadcrumb("direct-target", caster, castSp, a_targetID);   // [cast-call], flushed
                        inst->CastSpellImmediate(castSp, false, tgt, 1.0f, false, 0.0f, caster);
                        if (SustainConcentrationEffect(tgt, castSp, window)) {
                            // Evidence line: ONCE per stream if the engine honors the
                            // pinned duration; repeating every beat = sustain refused.
                            spdlog::info("[cast] {:08X} conc effect ATTACHED on {:08X} "
                                         "(spell {:08X}{}, window {:.0f}s)",
                                         a_casterID, a_targetID, a_spellID,
                                         castSp != sp ? " self->target proxy" : "", window);
                        } else if (!DeliveryAppliesInCall(castSp)) {
                            // A projectile delivery lands frames later: undecidable
                            // now, keep the charge, [heal-obs] answers it.
                            attach = HealAttach::Instant;
                        } else {
                            attach = HealAttach::Absent;
                            if (std::uint32_t held = 0; ReadbackWarnDue(a_casterID, castSp->GetFormID(),
                                                                    ReadbackWarn::NotAttached, held))
                                spdlog::warn("[cast] {:08X} conc effect NOT ATTACHED on {:08X} (spell {:08X}{}) -- "
                                             "the cast left no effect of {:08X} on the recipient (+{} held in 5s)",
                                             a_casterID, a_targetID, a_spellID,
                                             castSp != sp ? " self->target proxy" : "", castSp->GetFormID(),
                                             held);
                        }
                    }
                } else {
                    // The KNOWN-WORKING FORCE -- caster casts sp AT tgt, package-free.
                    // (Baseline: an FF Self spell force-cast here lands on tgt.)
                    CastBreadcrumb("direct-target", caster, sp, a_targetID);   // [cast-call], flushed
                    inst->CastSpellImmediate(sp, false, tgt, 1.0f, false, 0.0f, caster);
                    if (HasDurationEffect(sp) && DeliveryAppliesInCall(sp))
                        attach = SpellEffectPresentOn(tgt, sp) ? HealAttach::Present : HealAttach::Absent;
                }
                if (attach == HealAttach::Absent &&
                    CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Offense) {
                    if (std::uint32_t held = 0; ReadbackWarnDue(a_casterID, castForm->GetFormID(),
                                                                ReadbackWarn::NotLanded, held))
                        spdlog::warn("[cast] {:08X} {} FORCE-CAST {} ({:08X}) at {:08X} -- NOT LANDED: no effect of "
                                     "{:08X} on the recipient after the cast; no magicka spent (+{} held in 5s)",
                                     a_casterID, caster->GetName() ? caster->GetName() : "?",
                                     sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                                     castForm->GetFormID(), held);
                    if (heal) HealObsNote(caster, tgt, sp, castForm, StreamRoadLabel(caster), hpBefore, attach);
                    return;
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
                if (heal) {
                    NoteHealLanded(a_targetID, a_casterID, a_spellID);   // [bleed]
                    HealObsNote(caster, tgt, sp, castForm, StreamRoadLabel(caster), hpBefore, attach);
                }
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
    namespace {
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
        ResetArchetypeLog();          // [archetype] once-per-session ledgers (cast/Archetype.cpp)
        g_summonPosted.clear();       // summon post throttle (worker state, session-scoped)
        {
            std::lock_guard lk(g_summonMx);   // main-written verdicts / last casts
            g_summon.clear();
        }
        ResetCanActState();           // fix/mfo-can-act: the [bleed] ledgers
        ResetHealObs();               // feat/mfo-animheal-p0: [heal-obs] is session-scoped
        g_oocHealWaitLog.clear();     // MFO-B173: OocHealWaitsForClaim's log dedup
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

    std::vector<RE::FormID> ConcProxyForms() {
        std::vector<RE::FormID> out;
        for (const auto& id : ConcProxy::g_slotFormId)
            if (const auto f = id.load(std::memory_order_acquire); f != 0) out.push_back(f);
        return out;
    }

    // See Actuation.h (MFO-B176). The SAME end the streams' own "switch" branch makes
    // (CastSelfDirect / CastTargetDirect): settle the unpaid seconds, then the
    // EndActor post (dispel, interrupt the kInstant channel, and for a target stream
    // free the ConcProxy slot), then drop the registry entry. Both posts carry
    // FormIDs only and re-resolve on the main thread. Only a HEAL stream: a ward or
    // other non-heal stream is not on the heal road and keeps running.
    void EndDirectHealStreams(RE::FormID a_follower, const char* a_why) {
        if (a_follower == 0) return;
        const auto now = SelfClock::now();
        if (auto it = g_selfCast.find(a_follower); it != g_selfCast.end()) {
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(it->second.spell);
            if (sp && CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal) {
                spdlog::info("[heal] {:08X} self-stream RELEASE (road switch: {}) spell {:08X}",
                             a_follower, a_why, it->second.spell);
                PostSettle(a_follower, it->second.spell,
                           SettleSec(it->second.timed, it->second.paidThrough, it->second.lastApply,
                                     it->second.window, now),
                           "self-stream", "road-switch");
                SelfCastEndActor(a_follower, it->second.spell);
                g_selfCast.erase(it);
            }
        }
        if (auto it = g_targetCast.find(a_follower);
            it != g_targetCast.end() && it->second.kind == CasterConsent::SpellKind::Heal) {
            spdlog::info("[heal] {:08X} stream RELEASE (road switch: {}) tgt {:08X} spell {:08X}",
                         a_follower, a_why, it->second.target, it->second.spell);
            PostSettle(a_follower, it->second.spell,
                       SettleSec(it->second.timed, it->second.paidThrough, it->second.lastApply,
                                 it->second.window, now),
                       "stream", "road-switch");
            TargetCastEndActor(it->second.target, it->second.spell, a_follower);
            g_targetCast.erase(it);
        }
    }

    // See Direct_internal.h (MFO-B173).
    bool OocHealWaitsForClaim(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        if (!a_follower || !a_spell || g_firingRule != kNoRule) return false;
        if (CasterConsent::ClassifySpell(a_spell) != CasterConsent::SpellKind::Heal) return false;
        const RE::FormID fid      = a_follower->GetFormID();
        const RE::FormID standing = APMFBridge::GetHealCastSpell(fid);
        if (standing == 0) return false;
        const auto now  = SelfClock::now();
        auto&      last = g_oocHealWaitLog[fid];
        if (now - last >= std::chrono::seconds(5)) {
            last = now;
            spdlog::info("[heal] {:08X} out-of-combat heal {:08X} WAITS: a combat heal claim (spell {:08X}) "
                         "still stands and one actor never runs both heal roads; the party-OOC teardown "
                         "ends that claim, then this heal takes the direct road",
                         fid, a_spell->GetFormID(), standing);
        }
        return true;
    }

}
