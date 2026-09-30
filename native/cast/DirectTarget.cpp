// cast/DirectTarget.cpp -- the ON-TARGET stream of the direct-delivery road:
// CastTargetDirect, its release TargetCastReconcile and TargetStreamLive. Its state
// and apply substrate are in cast/Direct.cpp.
// Cut from cast/Direct.cpp by the Direct.cpp split (2026-09-29, MFO-B152): a pure
// move, proven function by function with tools/splitcheck.
#include "Direct_internal.h"
#include "ComposedCast.h"   // the Composed Forced Cast executor -- replaces the deleted
                            // HealAnimFill package route at the two cast plug-ins below
#include "Runtime.h"        // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"     // feat/cast-gambit-concentration (Task 1): ClaimOffenseCast for a
                            // non-heal (Offense/Buff) CONCENTRATION stream -- ComposedCast::Try
                            // above is HEAL-ONLY by design, so offense/buff concentration needs
                            // its OWN direct claim call here rather than a widened Try() gate.

namespace MFO::Actuation {

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
        // THE HEAL ROAD IS CHOSEN IN ONE PLACE (animheal phase 2, 2026-09-30):
        // ComposedCast::ChooseHealRoad makes exactly the controller test described
        // above, plus the Heal-kind and Harbinger gates Try() itself makes. It
        // reverses fix/mfo-combat-restoration-direct (2026-09-21) for the claim
        // case only: that "freeze" was MFO's own direct cast colliding with MFO's
        // own claim on the instant caster, which Harbinger no longer gates by a
        // hand claim (APMF d41ed43). The combat table sends a claim-road heal
        // through CastOn's composed branch; this ask serves a combat-table road
        // (ConcentrationCast, CastAuto's series) whose lap met a controller only
        // after its own road choice, so one actor never runs both roads for one
        // heal. The out-of-combat table no longer asks (MFO-B173, below).
        // Out of combat (D1) the direct stream below delivers, labelled.
        // COMBAT TABLE ONLY (MFO-B173): the road is asked only inside Fire()
        // (g_firingRule set). The out-of-combat caller never mints a claim (the
        // party-OOC teardown would end it the next service), and while a combat
        // heal claim still stands its heal WAITS instead of streaming beside it.
        if (OocHealWaitsForClaim(a_follower, a_spell)) return SelfCast::Declined;
        if (g_firingRule == kNoRule) ComposedCast::NoteOocDirectHeal(a_follower, a_spell);   // F2: name the instant road
        if (g_firingRule != kNoRule &&
            ComposedCast::ChooseHealRoad(a_follower, a_spell, a_target) ==
                ComposedCast::HealRoad::Claim) {
            switch (ComposedCast::Try(a_follower, a_spell, a_target, kind, a_stopPct)) {
            case ComposedCast::TryResult::Claimed: return SelfCast::Applied;
            case ComposedCast::TryResult::Held:    return SelfCast::Held;
            case ComposedCast::TryResult::ApmfRefused:
                LogApmfRefusal(id, "heal-cast", spellID, targetID, "left");
                return SelfCast::Declined;
            // NO `default:` -- see the twin in CastSelfDirect above (F3-6).
            case ComposedCast::TryResult::NotApplicable:
                // MFO-B181: this lap chose the claim road, so NotApplicable can only
                // be the kill switch (bHealAnimPackage) flipping OFF between
                // ChooseHealRoad and Try. Never the direct stream on a claim lap:
                // nothing is cast now, and the next lap's ChooseHealRoad reads the
                // switch again, releases any standing claim and labels its road.
                spdlog::warn("[heal] {:08X} heal {:08X} -> {:08X}: the animated heal road was switched "
                             "off between the road choice and the claim on this lap -- NOT cast this "
                             "lap (never the direct road on a claim lap); the next lap takes the road "
                             "the switch now names", id, spellID, targetID);
                return SelfCast::Declined;
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

        // NORMAL REACH FOR A HEAL (fix/mfo-can-act, marth 2026-09-29): a heal on
        // another actor reaches only as far as the player's own cast of the spell
        // would, with line of sight (HealReach / HealInReach, Actuation.h). Declined
        // is TRANSPARENT (the rules below run), and a live stream at a target that
        // left reach is no longer refreshed, so TargetCastReconcile ends it (stale).
        // Want() warms the LoS cache for the pair a frame ahead (Unknown passes).
        if (kind == CasterConsent::SpellKind::Heal || SpellHealsHealth(a_spell)) {
            Sightline::Want(id, { targetID });
            if (!HealInReach(a_follower, a_target, a_spell)) {
                // Deduped per (caster, target) at 5 s -- worker-serial, like the
                // other per-follower log throttles on this road.
                static std::unordered_map<std::uint64_t, SelfClock::time_point> s_reachLog;
                const auto rnow = SelfClock::now();
                auto& last = s_reachLog[(static_cast<std::uint64_t>(id) << 32) | targetID];
                if (std::chrono::duration<float>(rnow - last).count() >= 5.0f) {
                    last = rnow;
                    spdlog::info("[cast] {:08X} heal {} ({:08X}) at {:08X} declined -- out of reach "
                                 "(distance {:.0f}, reach {:.0f}, LoS {})",
                                 id, a_spell->GetName() ? a_spell->GetName() : "?", spellID, targetID,
                                 a_follower->GetPosition().GetDistance(a_target->GetPosition()),
                                 HealReach(a_spell),
                                 Sightline::VerdictName(Sightline::Check(id, targetID)));
                }
                return SelfCast::Declined;
            }
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
        HealObsSweep();   // feat/mfo-animheal-p0: the [heal-obs] follow-ups (cheap when none are due)
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
            // CASTER DOWN (fix/mfo-can-act): a caster who cannot act owns no live
            // stream (CannotActReason) -- a true end: dispel, interrupt, free the slot.
            // A knock that is only QUEUED does not end it (review C3).
            const bool casterDown = f && CannotActReason(f, false) != nullptr;
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
            if (gone || stale || capped || healedFull || magickaDry || casterDown) {
                // EVERY release is a TRUE END (marth's burst model): dispel the
                // sustained effect, INTERRUPT the engine channel, and FREE the proxy
                // slot (all in TargetCastEndActor). A plain cap thus ends the burst
                // cleanly and the gambit re-serves a still-wounded target as a FRESH
                // stream next tick (new slot, new channel) -- this both guarantees the
                // channel always stops (no runaway) and avoids orphaning an owned
                // proxy slot whose stream was erased. Breadcrumb names the reason.
                const char* reason = casterDown  ? "caster-down"
                                   : healedFull  ? "heal-full"
                                   : magickaDry  ? "magicka-out"
                                   : gone        ? "gone"
                                   : stale       ? "stale"
                                                 : "cap";
                if (casterDown)
                    spdlog::info("[cast] {:08X} stream RELEASE ({}: {}) tgt {:08X} spell {:08X}",
                                 id, reason, CannotActReason(f, false), tc.target, tc.spell);
                else
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
