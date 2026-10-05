// cast/DirectSelf.cpp -- the SELF stream of the direct-delivery road: CastSelfDirect
// and its release SelfCastReconcile (the design note sits above SelfCastState in
// cast/Direct_internal.h). Its state and apply substrate are in cast/Direct.cpp.
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

    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell, std::uint32_t a_stopPct) {
        // RUNTIME GATE: Runtime::CastPathsVerified() = EXACTLY 1.6.1170 (G1, 2026-10-04;
        // was the AE bucket) or EXACTLY 1.5.97 (feat/mfo-1.5.97-pass, 2026-09-15; was IsAE()-only as
        // the T#67 SE crash gate) or EXACTLY 1.7.104 (F2b, 2026-10-05). The T#67 fault was Loadout::LeftHandSlot()'s
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
        // THE HEAL ROAD IS CHOSEN IN ONE PLACE (animheal phase 2, 2026-09-30):
        // ComposedCast::ChooseHealRoad. With Harbinger present and a combat
        // controller a self heal is a ch.8b claim, cast by the follower's own AI
        // (this reverses fix/mfo-combat-restoration-direct, 2026-09-21, for that
        // case only). The combat table sends a claim-road heal through CastOn's
        // composed branch and never gets here with one; this ask serves a combat-
        // table road (the self fork, ConcentrationCast, CastAuto's series) whose
        // lap met a controller only after its own road choice, so one actor never
        // runs a claim and a direct cast of the same heal side by side. The out-
        // of-combat table no longer asks (MFO-B173, below). No controller (D1), no seat for the shape, or Harbinger absent:
        // the direct stream below, labelled by ChooseHealRoad.
        // COMBAT TABLE ONLY (MFO-B173): the road is asked only inside Fire()
        // (g_firingRule set). The out-of-combat caller never mints a claim (the
        // party-OOC teardown would end it the next service), and while a combat
        // heal claim still stands its heal WAITS instead of streaming beside it.
        const auto selfKind = CasterConsent::ClassifySpell(a_spell);
        if (OocHealWaitsForClaim(a_follower, a_spell)) return SelfCast::Declined;
        if (g_firingRule == kNoRule) ComposedCast::NoteOocDirectHeal(a_follower, a_spell);   // F2: name the instant road
        if (g_firingRule != kNoRule &&
            ComposedCast::ChooseHealRoad(a_follower, a_spell, a_follower) ==
                ComposedCast::HealRoad::Claim) {
            NoteArchetypeRoad(a_follower, a_spell, ArchRoad::HealClaim);   // [archetype] probe (passive)
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
                // MFO-B181: this lap chose the claim road, so NotApplicable can only
                // be the kill switch (bHealAnimPackage) flipping OFF between
                // ChooseHealRoad and Try. Never the direct stream on a claim lap:
                // nothing is cast now, and the next lap's ChooseHealRoad reads the
                // switch again, releases any standing claim and labels its road.
                spdlog::warn("[heal] {:08X} self heal {:08X}: the animated heal road was switched off "
                             "between the road choice and the claim on this lap -- NOT cast this lap "
                             "(never the direct road on a claim lap); the next lap takes the road the "
                             "switch now names", id, spellID);
                return SelfCast::Declined;
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
            NoteArchetypeRoad(a_follower, a_spell, ArchRoad::ConcClaim);   // [archetype] probe (passive)
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
            // CASTER DOWN (fix/mfo-can-act): a caster who cannot act (bleedout, down,
            // knocked down, paralysed, kill move -- CannotActReason) owns no live
            // stream. A true END: dispel + interrupt, like heal-full. A knock that is
            // only QUEUED does not end it (review C3: a_countPendingKnock=false).
            const bool casterDown = !gone && CannotActReason(a, false) != nullptr;
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
            if (gone || stale || concCapped || healedFull || magickaDry || casterDown) {
                // RELEASE. DISPEL (+ interrupt the channel, in SelfCastEndActor) a
                // lingering STICKY buff on any release, AND a momentary stream's
                // SUSTAINED effect when it truly ENDED (stale / heal-full / magicka-out /
                // caster-down).
                // A plain CAP-only release re-streams next tick (fresh random cap),
                // keeping a wanted self buff/heal continuous across bursts (self has no
                // proxy slot to orphan). There is NO equip to undo.
                const char* reason = casterDown ? "caster-down"
                                   : healedFull ? "heal-full"
                                   : magickaDry ? "magicka-out"
                                   : (a && !stale && !concCapped) ? "gone"
                                   : concCapped ? "cap" : "stale";
                if (casterDown)
                    spdlog::info("[cast] {:08X} self-stream RELEASE ({}: {}) spell {:08X}",
                                 id, reason, CannotActReason(a, false), sc.spell);
                else
                    spdlog::info("[cast] {:08X} self-stream RELEASE ({}) spell {:08X}", id, reason, sc.spell);
                // CHARGE FOR TIME: settle the seconds the effect ran past the last
                // paid beat (skipped when the caster is gone: nothing to bill).
                if (a)
                    PostSettle(id, sc.spell,
                               SettleSec(sc.timed, sc.paidThrough, sc.lastApply, sc.window, now),
                               "self-stream", reason);
                if (a && (kind == CasterConsent::SpellKind::Buff || stale || healedFull || magickaDry ||
                          casterDown))
                    SelfCastEndActor(id, sc.spell);
                done.push_back(id);
            }
        }
        for (const auto id : done) g_selfCast.erase(id);
    }

}
