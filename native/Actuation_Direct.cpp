// Actuation_Direct.cpp -- the DIRECT-DELIVERY half of the actuation family
// (split mechanically out of Actuation.cpp, no logic change): the per-follower
// self/target cast streams (CastSelfDirect / CastTargetDirect + their
// reconciles and clears), the AUTO fan-out (CastAuto), and the whole apply
// substrate they share -- the ConcProxy delivery-spell forge, dispel/sustain,
// Apply{Self,Target}Effect / ApplyEffectFromTo, and the beneficial-recast
// pacing. The combat-rule dispatch (Fire), CastOn/ConcentrationCast/ForceCast,
// EquipWeapon and the force-hold co-save stay in Actuation.cpp.
#include "Actuation_internal.h"

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
        using SelfClock = std::chrono::steady_clock;
        struct SelfCastState {
            RE::FormID            spell = 0;
            SelfClock::time_point started{};
            SelfClock::time_point lastFired{};   // last time the rule re-fired (release clock)
            SelfClock::time_point lastApply{};   // last effect/magicka application (apply pacing)
            float                 cap   = 0.0f;  // per-stream randomized time cap (DrawConcCap)
        };
        std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // worker-serial

        // ── ON-TARGET DIRECT FORCE (package-lock-proof; g_selfCast generalized) ──
        // The SAME known-working force (CastSpellImmediate straight onto the actor +
        // magicka deduct) applied to a NON-self target -- player / ally / foe. It
        // touches NO package, so it beats a package-locked custom follower's §4.6
        // alias lock: Lucien (2F00591F) has a prio-80 quest owning the cast alias,
        // so the package route [pkg]-DECLINED every tick and his on-PLAYER heal never
        // landed (deck, build 5f8e873). Direct force lands it. One stream per follower
        // (single channel, like the package). Worker-serial, no lock (#4 discipline);
        // cleared with g_selfCast on revert. `kind` (cached at start) decides the
        // time cap AND when release DISPELS: a ward/buff (Buff) on any release; a
        // momentary heal/damage's SUSTAINED real effect only at end-of-stream
        // (stale/gone/switch -- it genuinely channels and must die with the
        // stream), never on a cap-only re-stream (the re-arm continues it).
        struct TargetCastState {
            RE::FormID               spell  = 0;
            RE::FormID               target = 0;
            CasterConsent::SpellKind kind   = CasterConsent::SpellKind::Buff;
            SelfClock::time_point    started{};
            SelfClock::time_point    lastFired{};
            SelfClock::time_point    lastApply{};
            float                    cap    = 0.0f;  // per-stream randomized time cap (DrawConcCap)
        };
        std::unordered_map<RE::FormID, TargetCastState> g_targetCast;

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

        // ── THE REAL EFFECT, WITH A SYNTHESIZED DURATION (marth's ruling) ────
        // "Shouldn't you be using the ACTUAL spell effect? It seems like you
        // are trying to recreate it instead." -- correct, and it supersedes two
        // earlier attempts: the per-beat CastSpellImmediate re-cast (dc856ea:
        // HUD churn of duration-0 momentaries, no sustained shader) AND the
        // ApplyConcentrationBeat RestoreActorValue recreation (REMOVED: it only
        // covered value-modifier AVs and could never generalize -- a forced
        // waterbreathing/invisibility/ward concentration is not an AV write).
        //
        // THE PREMISE (field, b63beb9 A/B): a one-shot CastSpellImmediate of a
        // concentration spell creates its REAL ActiveEffect but with duration
        // 0 and no sustaining channel, so it dies within a frame -- a
        // per-second Restore Health accumulates ~0, the shader never sustains,
        // and re-casting per beat just stacks short-lived effects. FF spells
        // are untouched by all of this: a duration-0 FF instant applies its
        // per-CAST magnitude in full through the plain call (field-proven).
        //
        // THE FIX: attach the spell's REAL effect(s) ONCE per stream (plain
        // CastSpellImmediate -- correct effect, shader, HUD entry, resists,
        // hostility, every archetype), then SUSTAIN that single ActiveEffect
        // by pinning a real `duration` (the stream's window) and re-arming
        // `elapsedSeconds` on every beat. Given a real duration the engine
        // runs the effect NORMALLY: a per-second value-modifier accumulates
        // its authored magnitude the ordinary way (a duration'd Restore Health
        // heals magnitude-per-second, exactly like a regen potion), a duration
        // archetype (waterbreathing, invisibility, muffle) simply LASTS, a
        // ward wards. No recreation, no manual math, ALL archetypes. The
        // writes are instance-local on the live AE (the same `duration`/
        // `elapsedSeconds` fields the DoT-recast logic already reads) -- NEVER
        // a shared-form (MGEF/SpellItem) mutation.
        //
        // Returns TRUE if a live effect for this spell was found + re-armed
        // (the caller must NOT re-cast); FALSE -> the caller attaches once via
        // CastSpellImmediate and calls this again to pin it. EVIDENCE
        // COLLECTOR: the caller logs "conc effect ATTACHED" on every attach --
        // if the engine honors the pinned duration that line appears ONCE per
        // stream and the HUD shows one continuous effect; if the engine
        // expires the effect regardless (e.g. a no-duration-flagged MGEF, the
        // open premise CI cannot test), the line repeats every beat and the
        // presentation degrades to the previous per-beat re-attach -- loud in
        // the log, and the fix would be an FF-variant runtime spell, NOT a
        // return to RestoreActorValue. MAIN THREAD only (live AE list).
        bool SustainConcentrationEffect(RE::Actor* a_target, RE::SpellItem* a_spell,
                                        float a_window) {
            auto* mt = a_target ? a_target->AsMagicTarget() : nullptr;
            if (!mt || !a_spell) return false;
            auto* list = mt->GetActiveEffectList();
            if (!list) return false;
            bool found = false;
            for (auto* ae : *list) {
                if (!ae || ae->spell != a_spell) continue;
                ae->duration       = a_window;   // real duration -> the engine channels it
                ae->elapsedSeconds = 0.0f;       // rolling re-arm, one beat at a time
                found = true;
            }
            return found;
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
        // TWO transient dynamic (0xFF__) slots, SLOT-FOR-DURATION (marth's hard rule):
        // a CONCENTRATION proxy cast starts a REAL engine channel on the caster (it
        // drains the caster per-second and sustains the effect), so the proxy FORM is
        // load-bearing for the WHOLE life of that channel -- reconfiguring or handing
        // its form to another cast while the channel is live corrupts the in-flight
        // cast (the freeze) and entangles two streams (heal-full stops the 1st but not
        // the 2nd). So each live stream OWNS a slot for its duration: a slot is
        // Configure'd ONLY when FREE, never while its channel lives; released (its
        // channel INTERRUPTED, see TargetCastEndActor) only when the stream ends. Owner
        // = the caster (follower) FormID; one live concentration channel per follower.
        // 2-slot cap: if both slots are owned by OTHER live streams, Acquire returns
        // nullptr and the caller SKIPS (overflow). Never serialized (dynamic forms are
        // not). MAIN THREAD only (form table); returns nullptr off-main (VR).
        namespace ConcProxy {
            struct Slot { RE::SpellItem* form = nullptr; RE::FormID source = 0; RE::FormID owner = 0; };
            Slot g_slot[2];

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

        // Apply the effect + spend magicka for ONE fire (main thread). §5.3:
        // CastSpellImmediate spends nothing (§0.22), so deduct the real cost.
        void ApplySelfEffect(RE::FormID a_id, RE::FormID a_spellID) {
            MainThread::Post([a_id, a_spellID] {
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
                const bool concMomentary =
                    sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                    mgef &&
                    (mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kValueModifier ||
                     mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kDualValueModifier);
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
                const float spend = avo ? std::min(cost, before) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} SELF-CAST {} ({:08X}) -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_id, a->GetName() ? a->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, before, after, cost);
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
        //   a_guard FALSE (heal / damage -- MOMENTARY): every paced beat deducts a
        //                 second's cost and re-arms the sustained effect, so the
        //                 target is topped up steadily while the rule wins.
        void ApplyTargetEffect(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_guard) {
            MainThread::Post([a_casterID, a_targetID, a_spellID, a_guard] {
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
                const float cost  = sp->CalculateMagickaCost(caster);
                const float spend = avo ? std::min(cost, before) : 0.0f;   // #6: never negative
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} FORCE-CAST {} ({:08X}) at {:08X} -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_casterID, caster->GetName() ? caster->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                             before, after, cost);
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

        // AUTO fan-out pacing: last broadcast per follower, so a rule that keeps
        // winning every ~133 ms tick re-fans only once per fCastCooldown instead
        // of every tick (no thrash, no magicka spike). Worker-serial, no lock
        // (the g_followers discipline #4); cleared with g_selfCast on revert.
        std::unordered_map<RE::FormID, SelfClock::time_point> g_autoCast;

        // fix #3/#6: BENEFICIAL DURATION recast suppression. Keyed on
        // (casterFormID<<32 | spellID): the last time this caster fired this
        // beneficial buff, plus the JITTERED window it must wait before re-firing.
        // A light applied hands-free (Magelight/Candlelight) never registers as
        // "already active", so the already-active gate never caught it and MFO
        // respammed it as fast as magicka regenerated (deck 2026-08-18). This map
        // is the second layer: even an undetectable light is held off until near
        // its own authored expiry, and the per-fire jitter keeps the recast beat
        // human (never a fixed cadence). Worker-serial, no lock (same #4 discipline
        // as g_autoCast/g_selfCast); cleared with them on revert. INSTANT
        // beneficial spells (authoredDuration 0) and concentration streams are
        // never entered here -- they must re-fire on demand.
        struct BeneficialRecast {
            SelfClock::time_point lastCast{};
            float                 windowSec = 0.0f;
        };
        std::unordered_map<std::uint64_t, BeneficialRecast> g_beneficialRecast;

        std::uint64_t RecastKey(RE::FormID a_caster, RE::FormID a_spell) {
            return (static_cast<std::uint64_t>(a_caster) << 32) | a_spell;
        }

        // The spell's own authored duration = the longest effectItem.duration over
        // its effects (0 = an instant spell -> exempt from recast suppression).
        float AuthoredDuration(RE::SpellItem* a_spell) {
            float d = 0.0f;
            if (!a_spell) return d;
            for (auto* eff : a_spell->effects) {
                if (!eff) continue;
                const float dur = static_cast<float>(eff->effectItem.duration);
                if (dur > d) d = dur;
            }
            return d;
        }

        // A jittered fraction of the authored duration, recomputed per fire so the
        // recast beat is never a robotic constant (marth: "needs a larger more
        // variable delay to look more human"). Worker-serial context -> a
        // function-local static RNG is safe (single-threaded access). The window is
        //   authoredDuration * fBeneficialRecastFrac * (1 +/- fBeneficialRecastJitter).
        float JitteredRecastWindow(float a_authoredDuration) {
            static std::mt19937 rng{ std::random_device{}() };
            const float frac   = Config::g_beneficialRecastFrac.load();
            const float jitter = std::clamp(Config::g_beneficialRecastJitter.load(), 0.0f, 0.95f);
            std::uniform_real_distribution<float> dist(-jitter, jitter);
            return a_authoredDuration * frac * (1.0f + dist(rng));
        }

        // Apply a_spell's effect FROM a caster TO an arbitrary target, hands-free
        // (CastSpellImmediate, kInstant), and deduct the caster's REAL magicka
        // cost (§56: CastSpellImmediate spends nothing, so we deduct). This is
        // ApplySelfEffect generalised to a non-self target so AUTO can heal an
        // ally / damage a foe with the SAME proven mechanism -- effect + magicka
        // only, NO equip and NO channel (animation deferred, ENGINE_NOTES §0.13).
        // Runs on the main thread. The already-active / DoT-recast decision is
        // made WORKER-SIDE by ShouldApplyTo before the post (F2/F3/F4) -- it owns
        // the "is this target worth a cast?" gate for BOTH allies and enemies, so
        // this must NOT re-apply the old blanket already-active skip here: that
        // would defeat the hostile DoT burst-vs-tail recast the worker approved.
        // a_hostile only tunes the log label. (Sole caller: CastAuto.)
        void ApplyEffectFromTo(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_hostile) {
            MainThread::Post([a_casterID, a_targetID, a_spellID, a_hostile] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_targetID);
                auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!caster || !target || !sp) return;
                auto* avo  = caster->AsActorValueOwner();
                auto* inst = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // FORCED CONCENTRATION under AUTO: ONE sustained REAL effect per
                    // fanned target (the 6s/4s window bridges the fCastCooldown re-fan).
                    // AUTO does NOT proxy a conc-Self spell: the proxy channel needs a
                    // slot for its DURATION but AUTO has no per-target stream/reconcile
                    // to OWN and later FREE a slot, so proxying here would leak the
                    // 2-slot cap. A conc-Self spell fanned to a NON-self target is
                    // skipped for that target (single-target CastTargetDirect delivers
                    // conc-Self heals; FF and natively-aimed conc heals fan normally).
                    if (sp->GetDelivery() == RE::MagicSystem::Delivery::kSelf && target != caster)
                        return;   // AUTO-fanned conc-Self on an ally -> skip (no proxy leak)
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcUtilityHold;
                    if (!SustainConcentrationEffect(target, sp, window)) {
                        inst->CastSpellImmediate(sp, false, target, 1.0f, false, 0.0f, caster);
                        SustainConcentrationEffect(target, sp, window);
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on {:08X} "
                                     "(AUTO, spell {:08X}, window {:.0f}s)",
                                     a_casterID, a_targetID, a_spellID, window);
                    }
                } else {
                    inst->CastSpellImmediate(sp, false, target, 1.0f, false, 0.0f, caster);
                }
                const float cost  = sp->CalculateMagickaCost(caster);
                // #6: clamp to the current pool so a deduct never drives magicka
                // negative (AUTO validates N casts against ONE worker snapshot).
                const float spend = avo ? std::min(cost, before) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} AUTO {} {} ({:08X}) -> {:08X} -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_casterID, caster->GetName() ? caster->GetName() : "?",
                             a_hostile ? "HOSTILE" : "BENEFICIAL",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                             before, after, cost);
            });
        }

        // A detrimental Health effect = damage (an instant hit OR a per-second DoT).
        bool IsDamageEffect(RE::EffectSetting* a_mgef) {
            if (!a_mgef) return false;
            if (a_mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
            return a_mgef->IsDetrimental() || a_mgef->IsHostile();
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

        // Does a_spell restore Health to its target? (Any heal effect at all.)
        bool SpellHealsHealth(RE::SpellItem* a_spell) {
            if (!a_spell) return false;
            for (auto* eff : a_spell->effects)
                if (eff && IsHealEffect(eff->baseEffect)) return true;
            return false;
        }

        // WORKER-SIDE per-target gate for the AUTO fan-out (F2/F3/F4). Decides,
        // BEFORE any main-thread post, whether this cast should actually land on
        // the target -- so an all-covered fan-out returns a transparent NoOp
        // instead of burning the tick + suppression window on posts that every
        // one of them would skip. Mirrors ApplyEffectFromTo's own already-active
        // guard, applied to allies AND enemies (F3): an INSTANT spell leaves no
        // lingering effect, never matches HasMagicEffect, and so ALWAYS fires; a
        // duration buff/DoT/debuff already on the target is skipped.
        //
        // HOSTILE DURATION refinement (F4): do not blanket-skip an active DoT.
        // Decompose the spell -- burst = Sigma magnitude of INSTANT (duration==0)
        // damage effects, dotRate = Sigma magnitude of DURATION damage effects
        // (per second) -- read the target's remaining DoT time, and recast when
        //   burst >= dotRate * timeRemaining * fDotRecastBurstRatio.
        // Pure-burst always fires (dotRate==0 -> RHS 0), pure-DoT waits, a
        // big-burst+small-tail spell re-lands as the tail decays. Reads the
        // target's effect list on the worker, same discipline as the enumeration
        // that calls it (PickFoe/PickAlly context).
        bool ShouldApplyTo(RE::Actor* a_target, RE::SpellItem* a_spell, bool a_hostile) {
            if (!a_target || !a_spell) return false;
            // #5: CONCENTRATION spells (Healing Hands, damage streams) are meant to
            // re-apply continuously while the need holds -- the already-active skip
            // AND the burst-vs-tail DoT math both misfire on a stream, which is the
            // inconsistent healing/concentration behaviour marth reported. Never
            // let this gate block a concentration cast; the caller's own need gate
            // (the heal HP-threshold in CastAuto) still decides WHETHER it is wanted.
            if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
                return true;

            auto* mt   = a_target->AsMagicTarget();
            auto* ei   = a_spell->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            // #6a: ROBUST already-active detection. HasMagicEffect(costliest
            // baseEffect) MISSES light-archetype spells (Magelight/Candlelight
            // applied via CastSpellImmediate never register that effect), so it
            // returned true every cooldown and MFO respammed the light as fast as
            // magicka regenerated (deck 2026-08-18). So ALSO scan the target's
            // active-effect list for an effect cast by THIS spell that still has
            // remaining duration -- a light that never registers via HasMagicEffect
            // is caught here and treated as active, same as HasMagicEffect true.
            bool active = (mgef && mt && mt->HasMagicEffect(mgef));
            if (!active && mt) {
                if (auto* list = mt->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (!ae || ae->spell != a_spell) continue;
                        if ((ae->duration - ae->elapsedSeconds) > 0.0f) { active = true; break; }
                    }
                }
            }
            // Not currently affected (covers EVERY instant spell) -> apply.
            if (!active) return true;
            // Already affected. Beneficial / allies: do not re-stack -> skip.
            // Only a hostile DURATION spell gets the burst-vs-tail recast test.
            if (!a_hostile) return false;

            float burst = 0.0f, dotRate = 0.0f;
            for (auto* eff : a_spell->effects) {
                if (!eff || !IsDamageEffect(eff->baseEffect)) continue;
                const float mag = eff->effectItem.magnitude;
                if (eff->effectItem.duration == 0) burst   += mag;   // instant hit
                else                               dotRate += mag;   // per-second DoT
            }
            // No DoT component -> nothing to wait out, always recast.
            if (dotRate <= 0.0f) return true;

            // DoT time still to be delivered = max (duration - elapsed) across the
            // spell's active effects on this target.
            float timeRemaining = 0.0f;
            if (auto* list = mt->GetActiveEffectList()) {
                for (auto* ae : *list) {
                    if (!ae || ae->spell != a_spell) continue;
                    const float rem = ae->duration - ae->elapsedSeconds;
                    if (rem > timeRemaining) timeRemaining = rem;
                }
            }
            const float ratio = Config::g_dotRecastBurstRatio.load();
            return burst >= dotRate * timeRemaining * ratio;
        }
    }

    // ── SUMMON LIVENESS (v1.1.1) ────────────────────────────────────────────
    // A conjured familiar/atronach (SummonCreatureEffect) or a reanimated corpse
    // (ReanimateEffect) is a COMMANDED ACTOR, not a lingering caster-side magic
    // effect. The OOC cast routes' already-active guards read a magic effect on
    // the cast TARGET (HasMagicEffect / effect-duration scan on `tgt`), which for a
    // self-delivered summon is never the caster and never carries the summon
    // effect -- so the guard misses it entirely and a "cast [summon]" gambit re-
    // summons every cadence (marth, field-confirmed). The authoritative signal is
    // the COMMANDED ACTOR: scan the caster's own active effects for a summon/
    // reanimate effect THIS spell created and check its commandedActor still
    // resolves to a live actor. PER-SPELL (ae->spell == a_spell) so distinct
    // conjures track independently (a Twin Souls pair of different summons is
    // naturally allowed); keyed on the LIVE actor so a killed/expired/despawned
    // summon (handle gone or dead) frees an immediate recast. A non-summon spell
    // (candlelight/flesh/heal) carries no such archetype -> always false, so those
    // paths stay byte-identical. Reads the active-effect list only (no mutation).
    bool CasterHasLiveSummon(RE::Actor* a_caster, RE::SpellItem* a_spell) {
        if (!a_caster || !a_spell) return false;
        auto* mt = a_caster->AsMagicTarget();
        if (!mt) return false;
        auto* list = mt->GetActiveEffectList();
        if (!list) return false;
        for (auto* ae : *list) {
            if (!ae || ae->spell != a_spell) continue;
            RE::ActorHandle h{};
            if (auto* se = skyrim_cast<RE::SummonCreatureEffect*>(ae))      h = se->commandedActor;
            else if (auto* re = skyrim_cast<RE::ReanimateEffect*>(ae))      h = re->commandedActor;
            else continue;   // this spell's effect here is not a summon/reanimate
            if (auto cmd = h.get(); cmd && !cmd->IsDead() && !cmd->IsDeleted())
                return true;   // a live commanded summon from this spell is up
        }
        return false;
    }

    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        // AE-only, mirroring CastOn (the SE crash path #67). Off AE -> transparent.
        if (!REL::Module::IsAE())    return SelfCast::Declined;
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

        const auto now = SelfClock::now();
        auto it = g_selfCast.find(id);

        // A DIFFERENT spell was channeling -> end it (stop its VFX) before this
        // one, so shaders never stack.
        if (it != g_selfCast.end() && it->second.spell != spellID) {
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
            it = g_selfCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. Callers refresh this every service/combat tick
        // while the rule wins. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts one second's cost and re-arms that effect's rolling
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
            it->second.lastApply = now;
            ApplySelfEffect(id, spellID);
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
        const float partySize  = static_cast<float>(Followers::g_active.size() + 1);  // + player
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
    }

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
                              RE::Actor* a_target) {
        if (!REL::Module::IsAE())            return SelfCast::Declined;   // AE-only (#67)
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
            TargetCastEndActor(it->second.target, it->second.spell, id);
            g_targetCast.erase(it);
            it = g_targetCast.end();
        }
        if (it == g_targetCast.end()) {
            auto& tc = g_targetCast[id];
            tc.spell = spellID; tc.target = targetID; tc.kind = kind;
            tc.started = now; tc.lastFired = now; tc.lastApply = {};   // epoch -> apply now
            tc.cap = DrawConcCap(kind);   // per-stream random cap (8-15s heal/util, 2-6s offense)
            it = g_targetCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts one second's cost and re-arms that effect's rolling
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
            it->second.lastApply = now;
            // GUARD only sticky (Buff) buffs; heal/damage re-apply freely (momentary).
            ApplyTargetEffect(id, targetID, spellID, kind == CasterConsent::SpellKind::Buff);
            return SelfCast::Applied;
        }
        return SelfCast::Refreshed;   // winning but paced out this tick (transparent)
    }

    void TargetCastReconcile() {
        if (g_targetCast.empty()) return;
        const auto now = SelfClock::now();
        // Same release window as SelfCastReconcile: out-wait the worst-case
        // round-robin + suppression gap so a still-winning rule is never torn down.
        // (KNOWN SEV-1, shared with SelfCastReconcile: g_active is main-thread/
        // serial-task-only (#4) and this runs on the job worker -- tracked in
        // REVIEW-2026-08-18-comprehensive; the whole cluster is being fixed in
        // one wave, do not half-fix it here.)
        const float suppress   = std::max(0.0f, Config::g_suppressWindow.load());
        const float partySize  = static_cast<float>(Followers::g_active.size() + 1);
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
                TargetCastEndActor(tc.target, tc.spell, id);   // dispel + interrupt + free slot
                done.push_back(id);
            }
        }
        for (const auto id : done) g_targetCast.erase(id);
    }

    // AUTO TARGET INFERENCE for act.cast_target (marth). The board's default
    // target pick ("Auto", Subject::Self on a cast-target row) no longer just
    // falls back to the player -- it INFERS the set from the spell's nature
    // and fans the cast out, one direct effect-application per member, each
    // paying the spell's full magicka cost (N targets = N x cost), reserve-
    // floored and paced by fCastCooldown so it neither thrashes nor spikes.
    // A MANUAL pick (Player / Nearest ally / a specific follower) or a
    // selector-chosen target still takes the single-target CastOn path
    // unchanged -- AUTO only fills the "nobody obvious" default.
    //
    // PUBLIC: called from BOTH Fire (combat) and Logistics::ServiceFollower (out
    // of combat), so an authored "always -> Candlelight (Auto)" lights the
    // follower while exploring, and OOC heals/buffs reach the party. Out of
    // combat the hostile branch finds no combat group and NoOps cleanly.
    //
    // ROUTING (classification via CasterConsent::ClassifySpell; delivery is NOT
    // consulted -- MFO applies effects directly, so no spell is "self-only"):
    //   hostile (Offense) -> every nearby enemy (own combat group, chase radius)
    //   beneficial        -> the WHOLE PARTY within shared radius who NEEDS it --
    //                        every active follower + the player, the CASTER
    //                        INCLUDED as one of N (a self-delivery Candlelight thus
    //                        lights everyone). Heals filtered to HP<full; the
    //                        already-active guard skips anyone already covered.
    //
    // The fan-out uses the DIRECT effect applier (ApplyEffectFromTo), the same
    // proven hands-free mechanism as the self-cast, rather than the foe cast
    // PACKAGE: the package is a single-holder (one MFO_CastPackage on alias 0,
    // MAP §2) and cannot address N targets at once, and it is declined outright
    // on package-locked custom followers. Direct application costs magicka per
    // cast, touches only actors (follower-agnostic), and needs no animation
    // (deferred project-wide). Friendly fire is structurally impossible: the
    // effect is placed on the CHOSEN actor, never launched as a projectile.
    Outcome CastAuto(RE::Actor* a_follower, RE::FormID a_spellID, float a_healThreshold) {
            // AE-only, mirroring CastOn / CastSelfDirect (the SE crash path #67).
            if (!REL::Module::IsAE())
                return { Result::FailedOther,
                         "cast control is AE-only (SE/VR use the follower's own AI casting)", true };
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!spell)
                return { Result::FailedOther,
                         std::format("spell {:08X} not in load order", a_spellID), true };

            const auto id      = a_follower->GetFormID();
            const auto kind    = CasterConsent::ClassifySpell(spell);
            const bool hostile = (kind == CasterConsent::SpellKind::Offense);

            // AUTO ALLY-HEAL, CONCENTRATION -> SEQUENTIAL MOST-HURT (marth). A
            // concentration heal starts an ENGINE channel on the follower's caster,
            // and one caster sustains only ONE channel at a time -- so AUTO cannot fan
            // a concentration heal to N allies. Pick the SINGLE most-hurt hurt member
            // below the threshold (player OR teammate OR self) and serve THAT one via
            // the safe single-target path (CastTargetDirect proxy / CastSelfDirect for
            // self) -- owner-keyed slot, InterruptCast on release, all hardening
            // intact. When that recipient tops off (heal-full RELEASE, slot frees), the
            // next-most-hurt is served next tick, so over a few seconds every hurt ally
            // is topped. This runs EVERY service tick with NO g_autoCast cooldown gate:
            // the stream self-paces at ~1 s and must refresh each tick or it goes
            // stale. (FF/instant heals and non-heal buffs still fan below -- an instant
            // apply has no channel, so N targets at once is fine.) The 99.95% boundary
            // (Vocab::kHealFull) means a topped-off member is not re-selected.
            if (!hostile &&
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                ((kind == CasterConsent::SpellKind::Heal) || SpellHealsHealth(spell))) {
                // HYSTERESIS (anti-oscillation): committing to ONE recipient per beat
                // and re-picking the lowest-HP each tick would THRASH between two
                // similarly-hurt allies -- each ~1 s beat heals one a few HP above the
                // other, flipping the pick and dispel+interrupt+re-casting the channel
                // every second. So STICK with the follower's current heal recipient
                // while it is still below the ceiling, and only SWITCH when it tops off
                // OR another member is more than kHealSwitchMargin (15%) more hurt (a
                // critical drop worth interrupting for). Finish one, then the next.
                constexpr float kHealSwitchMargin = 0.15f;
                const float radius  = Config::g_sharedRadius.load();
                const float ceiling = std::min(a_healThreshold, Vocab::kHealFull);
                const auto  selfPos = a_follower->GetPosition();
                RE::Actor*  neediest = nullptr;
                float       lowest   = ceiling;   // only members strictly under the ceiling
                auto probe = [&](RE::Actor* m) {
                    if (!m || m->IsDead() || m->IsDisabled() || !m->Is3DLoaded()) return;
                    if (selfPos.GetDistance(m->GetPosition()) > radius) return;
                    const float hp = Vocab::HealthPct(m);
                    if (hp < lowest) { lowest = hp; neediest = m; }
                };
                for (const auto& h : Followers::g_active) { auto p = h.get(); probe(p.get()); }
                probe(RE::PlayerCharacter::GetSingleton());
                // Prefer the CURRENT stream's recipient if it is still hurt and no one
                // else is dramatically worse (the hysteresis above).
                RE::Actor* target = neediest;
                if (auto it = g_targetCast.find(id);
                    it != g_targetCast.end() && it->second.spell == a_spellID) {
                    if (auto* cur = RE::TESForm::LookupByID<RE::Actor>(it->second.target);
                        cur && !cur->IsDead() && Vocab::HealthPct(cur) < ceiling &&
                        (!neediest || Vocab::HealthPct(cur) - lowest <= kHealSwitchMargin))
                        target = cur;   // keep serving the current recipient
                }
                if (!target)
                    return { Result::NoOp, "auto conc-heal: nobody below threshold", true };
                const auto r = (target == a_follower)
                                   ? CastSelfDirect(a_follower, spell)
                                   : CastTargetDirect(a_follower, spell, target);
                switch (r) {
                case SelfCast::Applied:   return { Result::Fired, "auto conc-heal (most-hurt served)" };
                case SelfCast::Refreshed: return { Result::NoOp,  "auto conc-heal (paced)", true };
                default:                  return { Result::NoOp,  "auto conc-heal (declined)", true };
                }
            }

            // NOTE (marth, verified in the field): AUTO does NOT collapse a
            // self-delivery spell to the caster. MFO applies effects DIRECTLY to
            // the target actor (ApplyEffectFromTo -> CastSpellImmediate on that
            // actor), bypassing the engine's delivery system entirely -- the same
            // way act.cast_player already lands a self-delivery Candlelight on the
            // PLAYER. So the spell's authored delivery does NOT limit who MFO can
            // place the effect on: a beneficial spell fans to the WHOLE PARTY
            // (every teammate + the player who needs it, the caster included as
            // one of N), each via the direct applier. There is no delivery gate.

            // PACING: one broadcast per fCastCooldown per follower. The rule keeps
            // winning every tick; between cooldowns AUTO is a TRANSPARENT NoOp so
            // lower rules can still run.
            const auto  now      = SelfClock::now();
            const float interval = std::max(1.0f, Config::g_castCooldown.load());
            if (auto it = g_autoCast.find(id); it != g_autoCast.end() &&
                std::chrono::duration<float>(now - it->second).count() < interval)
                return { Result::NoOp, "auto-cast cooling down", true };

            // fix #3/#6: BENEFICIAL DURATION recast suppression -- ADDITIVE to the
            // fCastCooldown pace above, NOT a replacement. A beneficial buff with an
            // authored duration (a light, ward, fortify) must not re-fire until it
            // is near its own real expiry, so even a light that never registers as
            // "already active" (the Magelight spam) is held off, on a jittered/human
            // beat. HOSTILE spells, INSTANT beneficial spells (authoredDuration 0,
            // e.g. instant heals) and CONCENTRATION streams (Healing Hands) are NOT
            // suppressed here -- they re-fire on demand.
            const bool concentration =
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            const float authoredDur  = AuthoredDuration(spell);
            const bool  suppressible = !hostile && !concentration && authoredDur > 0.0f;
            const auto  recastKey    = RecastKey(id, a_spellID);
            if (suppressible) {
                if (auto it = g_beneficialRecast.find(recastKey); it != g_beneficialRecast.end() &&
                    std::chrono::duration<float>(now - it->second.lastCast).count() < it->second.windowSec)
                    return { Result::NoOp, "beneficial recast suppressed (buff still up)", true };
            }

            // Running magicka budget on the WORKER (the real deducts are posted to
            // main and have not run yet): read once, subtract each planned cast,
            // and STOP the moment the next cast would breach the reserve floor or
            // empty the pool. §5.3 -- competence is not permission.
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return { Result::FailedOther, "no magicka pool", true };
            float       budget  = avo->GetActorValue(RE::ActorValue::kMagicka);
            const float cost    = spell->CalculateMagickaCost(a_follower);
            const float reserve = Config::g_magickaReserve.load();
            const float mx      = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                  RE::ActorValue::kMagicka);
            const float floor   = (reserve > 0.0f && mx > 0.0f) ? reserve * mx : 0.0f;
            auto affordable = [&] { return cost <= budget && (budget - cost) >= floor; };

            // ENUMERATE the inferred set (worker-safe reads, same context/precedent
            // as Evaluator::PickFoe / PickAlly which run on this same tick). F1:
            // collect FormIDs, NEVER raw Actor* -- the fan-out below runs AFTER the
            // combat-group read-lock releases, and a stored pointer could dangle if
            // the main thread tears the actor down in that window (UAF). Every
            // post-lock use is FormID-only (LookupByID at apply time).
            std::vector<RE::FormID> targets;
            float radius = 0.0f;
            if (hostile) {
                radius = Confidence::ChaseRadius(a_follower);
                auto& rt = a_follower->GetActorRuntimeData();
                if (auto* cc = rt.combatController; cc && cc->combatGroup) {
                    const auto selfPos = a_follower->GetPosition();
                    RE::BSReadLockGuard lk(cc->combatGroup->lock);
                    for (const auto& t : cc->combatGroup->targets) {
                        auto  ptr = t.targetHandle.get();
                        auto* foe = ptr.get();
                        if (!foe || foe == a_follower) continue;
                        if (foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
                        if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
                        if (!foe->IsHostileToActor(a_follower)) continue;   // brawl gate (#34)
                        if (selfPos.GetDistance(foe->GetPosition()) > radius) continue;
                        // F2/F3/F4: only fan to a foe this cast will actually
                        // affect -- an instant spell always, a duration DoT only
                        // when it is not already covered (burst-vs-tail test).
                        // Filtering here (worker, foe ptr still valid) means an
                        // all-covered fan-out NoOps transparently below.
                        if (ShouldApplyTo(foe, spell, true))
                            targets.push_back(foe->GetFormID());
                    }
                }
                // F7: warm the LoS cache for the foes we will try to hit, so the
                // apply loop's Sightline::Check stops being fail-open Unknown
                // forever (the verdict lands a frame later; walls do not move).
                if (!targets.empty()) Sightline::Want(id, targets);
            } else {
                // WHOLE PARTY: every active follower + the player within range who
                // NEEDS it -- the CASTER INCLUDED (he is one of N, so a self-buff
                // like Candlelight still lights him too). The already-active guard
                // in ApplyEffectFromTo skips anyone who already has the effect; a
                // health-restoring spell is filtered PER TARGET to the firing rule's
                // HP threshold below (#2). No delivery gate.
                radius = Config::g_sharedRadius.load();
                const auto selfPos = a_follower->GetPosition();
                // Field #2: drive the per-target need gate off the SPELL'S EFFECTS,
                // not solely ClassifySpell. A restore-health spell the classifier
                // does not tag kHeal was bypassing the gate and fanning to the whole
                // party -- a full-HP player got healed because a DIFFERENT ally was
                // hurt. Treat the spell as a heal if EITHER classified Heal OR it
                // carries any heal effect, so ANY health-restoring spell heals only
                // those who actually need it (player and followers alike, below).
                const bool heal = (kind == CasterConsent::SpellKind::Heal) ||
                                  SpellHealsHealth(spell);
                auto consider = [&](RE::Actor* ally) {
                    if (!ally) return;
                    if (ally->IsDead() || ally->IsDisabled() || !ally->Is3DLoaded()) return;
                    if (selfPos.GetDistance(ally->GetPosition()) > radius) return;
                    // #2: heal only allies whose status matches the FIRING gambit's
                    // own condition, not everyone below full. a_healThreshold is the
                    // firing rule's health-below fraction (1.0 = the old blanket
                    // "anyone below full" when the rule carries no health gate, e.g.
                    // "Always -> Heal (Auto)" or the Logistics caller's default). A
                    // "Self: Health < 30% -> Heal (Auto)" rule thus heals only allies
                    // under 30%. Non-heal buffs skip this entirely (heal==false), so
                    // Candlelight still fans to the whole party.
                    // HEAL boundary fix: clamp the TOP so a 100% heal threshold means
                    // STRICTLY below full -- a topped-off ally (>= 99.95%) is skipped,
                    // else the fan re-heals a full party forever (see Vocab::kHealFull).
                    if (heal && Vocab::HealthPct(ally) >= std::min(a_healThreshold, Vocab::kHealFull)) return;
                    // F2/F3: skip anyone already carrying a duration buff from
                    // this spell (an instant heal leaves no effect and re-fires),
                    // so an all-covered party fans to nobody -> transparent NoOp.
                    if (!ShouldApplyTo(ally, spell, false)) return;
                    targets.push_back(ally->GetFormID());
                };
                for (const auto& h : Followers::g_active) { auto p = h.get(); consider(p.get()); }
                consider(RE::PlayerCharacter::GetSingleton());
            }

            if (targets.empty())
                return { Result::NoOp,
                         hostile ? "auto-cast: no enemies in range" : "auto-cast: nobody needs it", true };

            int fired = 0, skipped = 0;
            for (const auto tgtID : targets) {
                if (!affordable()) { ++skipped; break; }   // insufficient magicka -> stop the fan-out
                if (hostile && Sightline::Check(id, tgtID) == Sightline::Verdict::Occluded) {
                    ++skipped; continue;                    // no line of sight -- fail-open on Unknown
                }
                ApplyEffectFromTo(id, tgtID, a_spellID, hostile);
                budget -= cost;
                ++fired;
            }

            if (fired == 0)
                return { Result::NoOp, "auto-cast: all targets skipped (magicka/LoS)", true };
            g_autoCast[id] = now;
            // fix #3/#6: a beneficial duration buff just landed -- arm its jittered
            // recast window so it is not re-fired until near real expiry.
            if (suppressible)
                g_beneficialRecast[recastKey] = { now, JitteredRecastWindow(authoredDur) };
            spdlog::info("[cast] {:08X} {} AUTO {} {} ({:08X}) -- fanned to {} target(s), {} skipped "
                         "(radius {:.0f}, cost {:.0f} each)",
                         id, a_follower->GetName() ? a_follower->GetName() : "?",
                         hostile ? "HOSTILE" : "BENEFICIAL",
                         spell->GetName() ? spell->GetName() : "?", a_spellID,
                         fired, skipped, radius, cost);
            return { Result::Fired, "auto-cast fan-out" };
    }
}
