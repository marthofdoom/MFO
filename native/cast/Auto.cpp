// cast/Auto.cpp -- the AUTO fan-out (CastAuto): its pacing state, the
// beneficial-recast window, the per-target gate (ShouldApplyTo) and the
// hands-free applier (ApplyEffectFromTo), plus IsSummonSpell (inlined here).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "ComposedCast.h"   // the Composed Forced Cast executor -- replaces the deleted
                            // HealAnimFill package route at the two cast plug-ins below
#include "CastBounds.h"     // Reset the MFO-executed-cast bound beside ConcProxy::Reset()
#include "Runtime.h"        // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "APMFBridge.h"     // feat/cast-gambit-concentration (Task 1): ClaimOffenseCast for a
                            // non-heal (Offense/Buff) CONCENTRATION stream -- ComposedCast::Try
                            // above is HEAL-ONLY by design, so offense/buff concentration needs
                            // its OWN direct claim call here rather than a widened Try() gate.

namespace MFO::Actuation {

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
        std::unordered_map<std::uint64_t, BeneficialRecast> g_beneficialRecast;

    namespace {
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
        // a_hostile only tunes the log label. (Sole caller: CastAuto's fan-out. A summon never comes here: CastSummonOnce casts it itself on the main thread.)
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

    // ── SUMMON = ONE-SHOT CONJURE (fix/mfo-summon-oneshot) ──────────────────
    // See Actuation.h. A summon used to ride whatever road its target setting
    // picked: SELF went through CastSelfDirect, which registered it as a
    // self-CHANNEL in g_selfCast and took a left-hand cast lock. The summon
    // guard then (correctly) skipped the live summon, so the channel's
    // lastFired never refreshed, SelfCastReconcile called it stale ~3 s later
    // and SelfCastEndActor DISPELLED the summon -- recast, dispel, recast
    // (deck 2026-09-24, Jesper: 23 Matrons in one fight). A summon is not a
    // channel. It is cast once, by the caster, and MFO never ends it: it is
    // never registered in g_selfCast / g_targetCast (so no reconcile, combat-
    // end or cleanup path can dispel it) and takes no hand lock.
    bool IsSummonSpell(RE::SpellItem* a_spell) {
        if (!a_spell) return false;
        for (auto* eff : a_spell->effects) {
            if (eff && eff->baseEffect &&
                eff->baseEffect->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kSummonCreature)
                return true;
        }
        return false;
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
            // RUNTIME GATE (Runtime::CastPathsVerified(): AE bucket or exactly 1.5.97)
            // -- mirrors CastOn / CastSelfDirect (the former T#67 SE crash gate; see
            // CastSelfDirect's comment for the Docs/ADDRESS-TABLE-2026-09-15.md rows
            // every 1.5.97 value on this path comes from).
            if (!Runtime::CastPathsVerified())
                return { Result::FailedOther,
                         "cast control is verified on 1.6 and 1.5.97 only (this runtime uses the follower's own AI casting)", true };
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
                // SEV-1: iterate the lock-guarded FormID snapshot, not the live
                // g_active handle vector -- a concurrent Followers::Refresh can
                // reallocate it under this worker read (#4). See Sightline/CasterConsent.
                if (auto snap = Followers::ActiveSnapshot())
                    for (const RE::FormID fid : *snap)
                        probe(RE::TESForm::LookupByID<RE::Actor>(fid));
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
                // Task 3 (feat/mfo-cast-port): forward THIS firing rule's own
                // heal-below ceiling to the APMF-claimed path as a whole-percent
                // stop threshold (APMF_API::MakeStopPct) -- the ONE call site in
                // the tree where a real per-gambit heal threshold is in scope at
                // a CastSelfDirect/CastTargetDirect call (every other caller has
                // no numeric threshold and passes the default 0, i.e. "stop at
                // full"). `ceiling` is already clamped to Vocab::kHealFull above,
                // so a rule with no health-below condition (ceiling == 1.0, the
                // "Always -> Heal (Auto)" default) rounds to 100 -- same
                // full-restoration behaviour as passing 0.
                const std::uint32_t stopPct =
                    static_cast<std::uint32_t>(std::clamp(ceiling, 0.0f, 1.0f) * 100.0f + 0.5f);
                const auto r = (target == a_follower)
                                   ? CastSelfDirect(a_follower, spell, stopPct)
                                   : CastTargetDirect(a_follower, spell, target, stopPct);
                switch (r) {
                case SelfCast::Applied:   return { Result::Fired, "auto conc-heal (most-hurt served)" };
                case SelfCast::Refreshed: return { Result::NoOp,  "auto conc-heal (paced)", true };
                // Fable SEV-2 (2026-09-06): the SHIPPED DEFAULT rule ("Always ->
                // Heal (Auto)") reached this switch with a Held folded into
                // Applied and reported `[eval] fired: auto conc-heal (most-hurt
                // served)` for a spell that was never delivered -- and bought the
                // Scheduler's suppression window on that no-op. CastAuto has no
                // ResolveCastHand/HandFree/HoldCastLock gate of its own, so the
                // hold is fully reachable here; this is the site the field
                // configuration under diagnosis (AUTO heal + a kSelf heal) hits.
                // Transparent NoOp with its OWN label: nothing fired, the rules
                // below this one get their tick, and no lock is stamped.
                case SelfCast::Held:      return { Result::NoOp,
                                                   "auto conc-heal (held off -- another heal owns the claim)",
                                                   true };
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

            // AUTO-fan budget honesty (SEV-3): a CONCENTRATION + Self spell fanned
            // to a NON-caster is SILENTLY SKIPPED by ApplyEffectFromTo (it would leak
            // a ConcProxy slot AUTO cannot later FREE -- see its comment), so the
            // ONLY target such a spell can actually land on under AUTO is the caster
            // himself. Pre-FILTER those dead targets out during enumeration (below),
            // alongside the SpellHealsHealth heal-gate, so fired/budget count REAL
            // applications and never charge for a target the apply drops on the floor.
            const bool concSelf =
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                spell->GetDelivery()    == RE::MagicSystem::Delivery::kSelf;

            // SUMMONS ARE NEVER FANNED OUT (fix/mfo-summon-no-fanout). A summon
            // effect only ever conjures for its own caster, so the fan-out below
            // (one CastSpellImmediate per ally + the player, all in one tick)
            // conjured N creatures on the caster at once -- the lead for the
            // Serana freeze. A spell with a Summon Creature effect is cast ONCE,
            // by the caster, on the same direct road (ApplyEffectFromTo). Bound
            // weapons (kBoundWeapon) and Reanimate (kReanimate) are other
            // archetypes and never match. (fix/mfo-summon-oneshot: both gambit
            // dispatchers now send every summon to CastSummonOnce BEFORE they
            // reach CastAuto; this is the same single path, kept so no future
            // CastAuto caller can fan a summon out.)
            if (IsSummonSpell(spell))
                return CastSummonOnce(a_follower, spell, -1, "auto", "auto");

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
                        if (concSelf) continue;   // SEV-3: a conc-Self spell lands only on the caster under AUTO -- no foe is a real target
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
                    // SEV-3: a conc-Self spell only lands on the caster under AUTO
                    // (ApplyEffectFromTo drops it for anyone else), so never enumerate
                    // another ally for it -- that keeps fired/budget honest below.
                    if (concSelf && ally != a_follower) return;
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
                // SEV-1: read the lock-guarded FormID SNAPSHOT, never the live
                // g_active handle vector -- a concurrent Followers::Refresh (death/
                // loot sink on another job worker) can reallocate it under us (#4).
                // Matches Sightline/CasterConsent's off-domain read pattern.
                if (auto snap = Followers::ActiveSnapshot())
                    for (const RE::FormID fid : *snap)
                        consider(RE::TESForm::LookupByID<RE::Actor>(fid));
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
