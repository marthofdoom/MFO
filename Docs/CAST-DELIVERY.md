# CAST DELIVERY — the one canonical rule (read this BEFORE touching any cast path)

**Status:** authoritative. Supersedes scattered notes in MAP.md / STATUS / code comments.
**Why this doc exists:** the cast-delivery mechanism was solved, then broken twice in one
session by re-deriving it wrong. This is the single source of truth so it does not happen
again. If you are about to change how a follower casts a forced spell, read this first.

---

## THE ONE RULE

**MFO delivers every CONCENTRATION cast and every FORCED EFFECT APPLICATION by DIRECT
APPLICATION — `CastSpellImmediate` straight onto the target actor — NEVER through an AI
package.** (Scope note: two FIRE-AND-FORGET paths still *attempt* a package first for the
animation — see "Where a package is still allowed" — but each one falls back to the direct
force when the package declines, so a package-locked follower never has a dead cell.)

```
inst = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
inst->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f, follower);
// then deduct magicka by hand, CLAMPED to the pool:
//   spend = std::min(cost, currentMagicka); RestoreActorValue(kDamage, kMagicka, -spend)
// and ALWAYS from the MAIN thread (MainThread::Post) — never inline on the job worker.
```

This is the **known-working force**. It is:
- **Universal / package-lock-proof.** It bypasses the engine's AI-package/alias arbitration
  entirely, so it drives **package-locked custom followers** (Lucien 2F00591F and any
  follower whose own quest owns its package alias at a higher priority than MFO's 60)
  exactly like a vanilla one.
- **Proven — but be precise about where.** The public build (v1.0.63) used this call as the
  **FF silent force-half** (`CastOn`'s fallback when the package declined) and as the **OOC
  Logistics direct-apply**, plus `ApplySelfEffect`/`CastSelfDirect` — and every heal that
  ever worked in the field landed through it (incl. ff0cb48's `healStream` concentration
  heals). **v1.0.63 `CastOn` did NOT deliver concentration this way**: from v1.0.58 combat
  concentration went through the package (`ConcentrationCast` → `Packages::CastAt`+hold),
  which was **latently package-declined for locked followers even in the public build** —
  the combat lockout below was already live there, just unreported.
- **Effective — for FIRE-AND-FORGET.** A duration-0 FF instant applies its per-CAST
  magnitude in full through the plain call (field-proven). **For CONCENTRATION the plain
  call applies ~0** — see "THE MAGNITUDE CONTRACT" below; MFO applies the beat's worth
  explicitly. The earlier "no magnitude problem, never add `RestoreActorValue` math"
  ruling was measured on FF spells and AI-*channeled* casts and was a **false premise**
  for a forced concentration cast (deck A/B on b63beb9, 2026-08-18).
- **Consent-coherent.** It passes through NEITHER CasterConsent hook. `CheckStartCast`
  (0x06) advises the *combat AI's* deliberation and `CheckCast` (0x0A) is the pre-charge
  gate of the AI's own casting pipeline (`RequestCastImpl → … → FinishCastImpl`, whose
  precondition is `CheckCast` — ENGINE_NOTES §0.13/§0.14); `CastSpellImmediate` skips that
  state machine entirely (the same reason it never animates). Deck-proven 2026-08-18:
  Lucien's AI cast of Healing (0005AD5C) is `[consent] HARD-ABORTED` while MFO's direct
  `SELF-CAST` applies land on the same actor in the same minute, hooks live. So MFO may
  deny the AI's unbounded channel AND deliver directly — that pairing is the design, and
  it can never deadlock.

Entry points, all the same mechanism:
- **self** → `Actuation::CastSelfDirect` (direct-apply on the caster).
- **player / ally / foe** → `Actuation::CastTargetDirect` (registry `g_targetCast`,
  released by `TargetCastReconcile`) — called by BOTH the OOC Logistics dispatch AND
  combat's `ConcentrationCast`. One channel per follower.

## THE MAGNITUDE CONTRACT (forced concentration ≠ the plain call)

**A concentration effect's magnitude is authored PER SECOND, and the engine only applies
it while a real channel RUNS.** The ActiveEffect a one-shot `CastSpellImmediate` creates
carries duration 0 and no sustaining channel: it attaches (the hit-shader plays — the
"healing glow" residue), then dies on its first update, applying ~one *frame's* worth of a
per-second magnitude — effectively **zero**. Field A/B, deck b63beb9 (2026-08-18): the
SAME spell (Fast Healing 0002F3B8, concentration in this modlist) fired at SELF via
`CastSelfDirect` AND at the PLAYER via `CastTargetDirect`; both drained magicka on every
apply, **neither target's HP moved**. Every heal that "worked with the plain call" in the
public build was either an FF spell or the follower's own AI *channeling* (which
accumulates) — never a forced concentration one-shot.

**So each ~1 s beat applies ONE SECOND'S WORTH of the spell's plain value-modifier
effects explicitly** (`ApplyConcentrationBeat`, Actuation.cpp — main thread, called from
`ApplySelfEffect` / `ApplyTargetEffect` / AUTO's `ApplyEffectFromTo`):
- **beneficial** → `RestoreActorValue(kDamage, av, +magnitude × kConcApplyPeriod)`,
  clamped to the damage actually taken — a beat can never push past max;
- **detrimental** → `RestoreActorValue(kDamage, av, −magnitude × kConcApplyPeriod)` (the
  same call every magicka deduct uses), so a foe stream actually damages;
- archetype gate: `kValueModifier` / `kDualValueModifier` only (primary AV). Wards,
  lights, paralysis and scripted archetypes keep the `CastSpellImmediate` apply — their
  semantics are not per-second AV ticks.
The exchange rate is honest: each beat spends one second's cost and applies one second's
magnitude — a magicka-starved caster fires sparse beats that each still heal a full
second's worth. Log line to verify in the field: `[cast] XXXXXXXX conc beat on YYYYYYYY:
+N/-M (spell ZZZZZZZZ)`.

## THE PRESENTATION CONTRACT (one sustained effect — never a per-beat re-cast)

**Scope correction (dc856ea):** only the caster POSE animation is deferred (post-town
polish). The **target-side effect VFX — the healing shader/glow — is in scope and must
display.** And a forced concentration stream must read as **ONE sustained Active-Effect
HUD entry**, not a rapid-fire stack of momentary effects.

The dc856ea defect: re-invoking `CastSpellImmediate` every beat spawned a fresh
duration-0 ActiveEffect each ~1 s — HUD churn, and the shader died with each short
effect. The fix (`SustainConcentrationEffect`, Actuation.cpp, main thread):
- **Attach once per stream** (stream start, or spell/target change) via
  `CastSpellImmediate`.
- **Sustain that single ActiveEffect**: each beat pins a real `duration` (the stream's
  window — heal 6 s, target offense/utility 4 s, self ward 15 s) and resets
  `elapsedSeconds`. Instance-local writes on the live AE (the same fields the DoT-recast
  logic reads) — **never** a shared-form MGEF/SpellItem mutation. One HUD entry; the
  shader plays continuously for the rolling window.
- **Momentary kinds (heal/damage): the sustained effect's `magnitude` is zeroed** — it is
  purely cosmetic (HUD + shader) while the explicit `RestoreActorValue` beats remain the
  one deterministic source of HP change; double-application is impossible by
  construction. A concentration **ward/buff keeps its authored magnitude** (zeroing would
  zero the ward) and gets only the duration sustain.
- **Release**: a sticky ward dispels on any release (unchanged). A momentary stream's
  cosmetic effect now dispels at **end-of-stream** (rule stale / actor gone / spell-or-
  target switch) so the glow dies with the stream — safe, it heals nothing — but is
  **kept alive across cap-only releases** (the 6 s heal cap re-streams next tick), so
  the HUD entry is continuous for the whole time the rule wins.
- **Fallback**: if the engine expires the effect despite the pinned duration (e.g. a
  no-duration-flagged MGEF), the next beat simply finds nothing and re-attaches —
  degrading to the old per-beat presentation with zero functional loss.

Known residuals: authored magnitude (no skill/perk scaling — matches the flat per-second
cost); dual-value secondary AV skipped (`secondAVWeight` unverified in this CommonLib
rev); foe damage is a plain AV hit (no resist math, no aggro event — the stream runs in
combat anyway).

## THE RE-APPLICATION CADENCE (the contract that makes heals feel right)

**A concentration effect's magnitude AND cost are authored PER SECOND, so the direct
stream must re-apply it about once per second** (`kConcApplyPeriod` = 1 s) while its rule
keeps winning. The last known-working heal path (ff0cb48 `healStream`) re-fired every ~1 s
service tick — that beat IS why it felt right. Pacing a concentration apply by
`fCastCooldown` (default **4 s**) silently cuts heal/damage throughput ~4× ("heals feel
broken") and under-drains magicka by the same factor. Rules:
- **concentration (heal / damage / ward), self or target** → ~1 s beat. A sticky ward's
  beat is de-duplicated by the already-active guard (no stacking).
- **fire-and-forget** → `fCastCooldown` beat (its magnitude is per CAST; a 1 s beat would
  multiply it).
- In combat the gambit suppress window (~1.5 s ±12%) stretches the effective beat slightly;
  that is fine — what is not fine is a 4 s beat.

## THE ROUTE YOU MUST NOT USE (for concentration / forced effects)

**Do NOT deliver a concentration or forced-effect cast through an AI package** —
`Packages::CastAt`(+`CastHold`). Be honest about what the package bought: a **real animated
cast**, at **no hand-deducted magicka**, and it **did work for non-package-locked
followers**. Its flaws are still fatal:
1. **Package-lock (§4.6).** A follower whose own quest owns the package alias at higher
   priority (Lucien: 80 vs MFO's 60) declines the claim **every tick** (deck: `[pkg]
   DECLINED -- alias layer owned by quest … priority 80 … not escalating`). The cell is
   simply dead for them.
2. **Rooting.** The package holds the caster in place mid-fight; the hold must stay short,
   which fights the bounding semantics.
3. **Cadence.** A per-hold beat cannot honor the 1 s re-application contract.
And the animation — the package's one genuine advantage — is **deferred** (see
`cast-animations-deferred-to-post-town-polish`), so with animations off the table it
currently buys nothing worth those flaws. **marth (2026-08-18): "avoid that route for
anything, always use the known working force."**

### The COMBAT lockout (fact, and in scope — fixed by making direct force primary)

From v1.0.58 through v1.0.65, combat concentration was package-delivered
(`ConcentrationCast` → `Packages::CastAt`+hold). For a package-locked follower that
§4.6-declines every tick — **and** the consent layer simultaneously denies the follower's
own AI: `CheckStartCast` refuses the AI's attempt at a wanted concentration spell (an AI
channel cannot be bounded) and `CheckCast` hard-aborts what leaks. The package stream was
designed as "the ONLY open channel" — so when the package was dead, **MFO blocked the AI
AND delivered nothing itself: a total combat cast lockout.** The fix is not to stop
denying the AI (that re-opens the unbounded-stream freeze); it is to make the channel that
replaces the AI one that cannot be declined: the direct force, which the consent hooks
never see (proof above). `ConcentrationCast` now delivers via `CastTargetDirect` —
primary, not a fallback — and the package delivery is removed.

## Where a package is STILL allowed (fire-and-forget only, with direct fallback)

- **Combat FF force-half** (`ForceCast` → `Packages::CastAt`): animated when it works; a
  structural decline already falls through to the direct silent cast.
- **OOC FF-hostile-at-foe** (Logistics): package first for the animation; on a §4.6
  decline (or packages off) it now direct-force falls back (LoS+LoF-gated, main-thread
  posted), so a locked follower still delivers.
A package-locked follower must **always** deliver via direct force with no functional gap
— that is the standing condition on keeping any package path at all.

## TIME-LIMIT BOUNDING (applies to the DIRECT path)

On the direct path, "bounding" is a **re-application window plus a release beat** — NOT a
rooted package hold. The channel re-applies on its cadence while the rule wins;
`Self/TargetCastReconcile` releases the channel when the rule goes stale, the actor
unloads, or the per-kind cap elapses — and the next winning tick **re-streams it**.
Bounding is release + re-stream, never a stop.

| Kind (by target/nature) | Cap | Constant |
|---|---|---|
| foe offense stream | 1–4 s (scaled by Temperament) | via `ConcentrationHold` |
| heal (self / ally / player) | 6 s, or until the target tops off | `kConcHealCap` |
| ally/foe utility | 4 s | `kConcUtilityHold` |
| **self** ward / utility | **15 s** (marth's call — NOT 4 s: `CastSelfDirect` is non-rooting, so a short cap only flickers a self-ward) | `kConcSelfUtilityCap` |

**The cap must NEVER interrupt a heal that is still needed.** A lingering **ward/utility**
buff is dispelled off the target (`TargetCastEndActor`/`SelfCastEndActor`) on ANY release
— gone/stale/capped — so it can never persist as a stuck gameplay buff. A **heal or
damage** stream's HP flow comes from the explicit beats, which run while the rule wins —
nothing about release can interrupt it; its magnitude-0 *cosmetic* sustained effect (THE
PRESENTATION CONTRACT) is dispelled only at **end-of-stream** (stale/gone/switch), never
on a cap-only release, so the visible effect is one continuous entry while the stream
re-arms. Fire-and-forget self-buffs keep their authored duration (no cap).

## GATES (keep on the direct path)

- **LoS** — never stream a hostile cast into a wall. Re-checked on EVERY apply.
- **Line-of-fire** — hold a hostile stream the instant a teammate crosses it (the ffWatch
  analog; also re-checked every apply).
- **Already-active guard** — a duration buff/LIGHT (Candlelight/Magelight) must not
  re-apply while already active (light accumulation → ShadowSceneNode CTD). An instant heal
  leaves no active effect, so it correctly re-fires while the condition holds.
- **Affordability (§5.3)** — real cost + reserve floor gate every apply; the hand deduct is
  clamped `min(cost, pool)` so magicka never goes negative.
- **THREADING (#14)** — the engine apply always runs via `MainThread::Post`, re-resolving
  actors by FormID inside the post. Never call `CastSpellImmediate` inline on the AddTask
  job worker (the pre-fix Logistics inline call is the prime suspect for the queued 1.5.x
  `act.cast_target` AV reports).

## THE MATRIX — every cell delivers, bounded, gated. None barred.

| | self | player / ally | foe |
|---|---|---|---|
| fire-and-forget | `CastSelfDirect`, fCastCooldown beat | direct apply (main-thread posted), fCastCooldown window | package (animated) → **direct fallback on decline**; combat: `ForceCast` package → silent direct fallback |
| concentration | `CastSelfDirect`, **1 s beat**, 6 s heal / 15 s ward-utility cap | `CastTargetDirect`, **1 s beat**, 6 s heal / 4 s utility cap | `CastTargetDirect`, **1 s beat**, 1–4 s offense cap (LoS+LoF every apply) |

- Concentration rows are identical IN COMBAT and OUT OF COMBAT — same function, same
  channel registry, same beat, same caps — and every beat applies the explicit
  per-second magnitude (THE MAGNITUDE CONTRACT above), not just the plain call.
- **Self-concentration is SOLVED, not barred** — INVARIANT #67 was **REVOKED 2026-07-22**;
  `CastSelfDirect` (no package, no QNAM, so no t6 CTD) is the solution.
- **AUTO fan** is the one exception to per-target streaming: one follower holds one channel,
  so a concentration spell under AUTO applies a short per-tick effect to each fanned target
  (a group pulse), not N simultaneous streams.

## REGRESSION HISTORY (so it is not repeated)

- **v1.0.58:** combat concentration moved onto the package stream — latently dead (and
  consent-locked-out) for package-locked followers from that day, even in the public build.
- **2026-08-18, commit `c539257` ("unify concentration routing"):** rerouted OOC
  concentration from the working direct-apply to the **package** (`CastConcentrationAt →
  Packages::CastAt`). Result: package-locked Lucien stopped healing the player/ally
  (declined every tick). **Fix (36231d7 + fable-cast-solve): direct force is the ONLY
  concentration delivery, OOC and combat; ~1 s cadence restored; all applies main-threaded.**
- **2026-08-18, field on `b63beb9`:** delivery fixed (player targeted, zero `[pkg]
  DECLINED`) but **HP flat on self AND player while magicka drained** — the "magnitude
  theory is a dead end" dismissal was itself wrong (it was tested against FF/AI-channeled
  casts, a false premise). A forced concentration one-shot applies ~0 of its per-second
  magnitude. **Fix: `ApplyConcentrationBeat` — explicit per-second magnitude per beat
  (see THE MAGNITUDE CONTRACT). Do not re-remove it on the strength of the old ruling.**

## PROCESS RULE

Before changing any cast/heal delivery: (1) read THIS doc; (2) read `ConcentrationCast` /
`CastOn` / `CastSelfDirect` / `CastTargetDirect` / `Apply{Self,Target}Effect` and
INVARIANTS #67; (3) **diff against the last-good public tag** and **field-test before
building on top**. Never swap a working direct path for a package route, and never slow a
concentration beat past ~1 s. See memory `self-concentration-gambits-barred` (RESOLVED)
and `check-engine-notes-measured-results`.

## RESIDUAL RISK (watch for in the field)

If a future deck log ever shows `[consent] … HARD-ABORTED … (concentration unbounded)`
naming the GAMBIT spell while a direct stream is live, that would mean the runtime is
routing `CastSpellImmediate` through the hooked `CheckCast` after all (contradicting
§0.13 + the 2026-08-18 evidence); the fix would be a `StreamLive`-style exemption keyed on
`g_targetCast`/`g_selfCast`, not a return to the package.
