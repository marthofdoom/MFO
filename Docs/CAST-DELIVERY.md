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
  magnitude in full through the plain call (field-proven). **For CONCENTRATION a bare
  one-shot applies ~0** — see "THE REAL-EFFECT CONTRACT" below: the real effect is
  attached once and SUSTAINED with a synthesized duration so the ENGINE channels its own
  computed per-caster magnitude. The earlier "no magnitude problem" ruling was measured
  on FF spells and AI-*channeled* casts — a false premise for a forced one-shot (deck A/B
  on b63beb9, 2026-08-18) — and the interim `RestoreActorValue` recreation that followed
  was equally wrong (base value, VM-only; superseded by marth's real-effect ruling).
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

## THE REAL-EFFECT CONTRACT (concentration = the ACTUAL effect, sustained — marth's ruling)

**"Shouldn't you be using the ACTUAL spell effect?"** — yes. Two earlier attempts are
superseded and must not return: the per-beat `CastSpellImmediate` re-cast (dc856ea: HUD
churn of duration-0 momentaries, no sustained shader) and the `ApplyConcentrationBeat`
`RestoreActorValue` recreation (REMOVED: it re-derived HP from the BASE authored
magnitude — ignoring the caster's skill/perks/effectiveness — and only ever covered
value-modifier AVs; a waterbreathing/invisibility/ward gambit could never work that way).

**The premise (field, b63beb9 A/B):** a one-shot `CastSpellImmediate` of a concentration
spell creates its REAL ActiveEffect — the engine computes the per-caster effective
magnitude into it — but with duration 0 and no sustaining channel, so it dies within a
frame: a per-second Restore Health accumulates ~0 (magicka drained, HP flat on self AND
player; the same spell). Every heal that "worked with the plain call" was FF or
AI-channeled. Crucially, the ~0 result is itself evidence the effect applies as a **rate
over its active lifetime** (an instant-apply model would have landed the full magnitude —
the field ruled that out), so lifetime is the only missing factor.

**The contract (`SustainConcentrationEffect`, Actuation.cpp, main thread — called from
`ApplySelfEffect` / `ApplyTargetEffect` / AUTO's `ApplyEffectFromTo`):**
- **Attach once per stream** (stream start, or spell/target change) via plain
  `CastSpellImmediate` — the real effect, so the ENGINE computes the per-caster
  magnitude, plays the real shader, creates the real HUD entry, applies resists and
  hostility, for EVERY archetype.
- **Sustain that single ActiveEffect**: each ~1 s beat pins a real `duration` (the
  stream's window — heal 6 s, target offense/utility 4 s, self ward 15 s) and re-arms
  `elapsedSeconds`. Instance-local writes on the live AE (the same fields the DoT-recast
  logic reads) — **never** a shared-form MGEF/SpellItem mutation, **never** touching the
  engine-computed `magnitude`. Given real lifetime, the engine channels the effect
  normally: a per-second value-modifier accumulates its own computed rate, a duration
  archetype (waterbreathing, invisibility, muffle) simply lasts, a ward wards.
- **MFO owns delivery, cost, and bounds; the ENGINE owns the magnitude.** No manual AV
  math, no base-authored values, no constants in the heal/damage numbers — the amount is
  exactly what that NPC casting that spell normally does. The ~1 s beat deducts the
  per-second cost (clamped) and re-arms the window; that is all it does.
- **Release**: a sticky ward dispels on any release (unchanged). A momentary stream's
  sustained effect genuinely channels, so it is dispelled at **end-of-stream** (rule
  stale / actor gone / spell-or-target switch) — the stream's end must cut it rather
  than let it run unpaid — but is **kept alive across cap-only releases** (the 6 s heal
  cap re-streams next tick), so the HUD entry is continuous while the rule wins. An
  orphan can outlive its stream by at most its remaining window (bounded by design).
- **Presentation scope:** only the caster POSE animation is deferred (post-town). The
  target-side effect VFX — shader/glow + ONE sustained HUD entry — is in scope and is
  exactly what the sustained real effect provides.
- **Evidence collector / fallback:** the attach logs
  `[cast] XXXXXXXX conc effect ATTACHED on YYYYYYYY (spell ZZZZZZZZ, window Ws)`. If the
  engine honors the pinned duration this appears ONCE per stream and HP climbs at the
  engine's own rate; if a concentration-specific check kills the AE regardless (the one
  premise CI cannot test), the line repeats every beat and presentation degrades to
  per-beat re-attach. Should the field prove non-accumulation, the sanctioned fallback is
  applying the ENGINE-COMPUTED `ae->magnitude` (captured off the created AE — never
  `Effect::GetMagnitude()` base) per second, or an FF-variant runtime spell — NOT a
  return to base-value `RestoreActorValue` recreation.

## THE BEAT CADENCE (the stream's heartbeat)

**A concentration spell's cost is authored PER SECOND, and the engine channels its
magnitude continuously through the sustained real effect — so the stream beats about once
per second** (`kConcApplyPeriod` = 1 s) while its rule keeps winning: each beat **deducts
one second's cost** (clamped) and **re-arms the sustained effect's rolling window**.
Pacing the beat by `fCastCooldown` (default **4 s**) would under-charge the channel ~4×
and let the effect lapse between re-arms (the original "heals feel broken" shape). Rules:
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
damage** stream's sustained REAL effect genuinely channels, so it is dispelled only at
**end-of-stream** (stale/gone/switch — the stream's end must cut it rather than let it
run unpaid), never on a cap-only release: the cap re-streams next tick and the next beat
re-arms the SAME effect, so the heal flows and the visible entry stays continuous while
the rule wins. Fire-and-forget self-buffs keep their authored duration (no cap).

## GATES (keep on the direct path)

- **LoS** — never stream a hostile cast into a wall. Re-checked on EVERY apply.
- **Line-of-fire** — hold a hostile stream the instant a teammate crosses it (the ffWatch
  analog; also re-checked every apply).
- **Already-active guard** — a duration buff/LIGHT (Candlelight/Magelight) must not
  re-apply while already active (light accumulation → ShadowSceneNode CTD). A momentary
  concentration effect (value-modifier) BYPASSES the guard: its sustained real effect is
  active by design, and the guard must not block the beat that re-arms it.
- **Affordability (§5.3)** — real cost + reserve floor gate every apply; the hand deduct is
  clamped `min(cost, pool)` so magicka never goes negative.
- **THREADING (#14)** — the engine apply always runs via `MainThread::Post`, re-resolving
  actors by FormID inside the post. Never call `CastSpellImmediate` inline on the AddTask
  job worker (the pre-fix Logistics inline call is the prime suspect for the queued 1.5.x
  `act.cast_target` AV reports).

## SELF-DELIVERY: the effect lands on the CASTER'S OWNER — read the SPEL's Delivery

**`CastSpellImmediate` does NOT apply a spell to the `target` ref for a SELF-delivery
spell.** A Self-delivery magic effect always lands on the *magic-caster's owner*,
whatever `TESObjectREFR*` you pass. So `follower->GetMagicCaster()->CastSpellImmediate(sp,
…, player, …)` on a **Self** spell heals the FOLLOWER, not the player — the old comment
"CastSpellImmediate applies the effect to `tgt` for any delivery" was simply false.

**Deck field, 2026-08-19 (build c875048 = 47fd0de):** Lucien set to heal the player with
"Fast Healing" (`0002F3B8`). In the loaded modlist (Mysticism) that record is **Concentration
+ Self delivery** (mag 20 / dur 1 / cost 38) — *not* the vanilla FireForget+Self instant.
GetCastingType was therefore correct (it IS concentration); the real blocker was **Self
delivery**: every beat the effect attached on *Lucien*, so the player never healed, Lucien's
magicka drained, and — because the sustain searched the *player's* effect list and never
found the effect (it was on Lucien) — `conc effect ATTACHED` re-fired every beat instead of
FOUND-and-re-armed once. Two symptoms, one cause.

**The rule is DELIVERY-TYPE + CASTING-TYPE driven, ARCHETYPE-AGNOSTIC — no heal/"is a
heal"/effect-archetype special-casing anywhere.** It holds for EVERY Self-delivery spell
and EVERY concentration spell alike: heal, ward, flesh/armor, waterbreathing, invisibility,
muffle, fear/frenzy, damage/DoT stream, or any buff.

1. **WHERE it lands (`Actuation::EffectCasterFor`, reads `GetDelivery()`):** to place a
   Self-delivery effect on a NON-self target, that TARGET must be the magic caster (it
   self-casts). Non-Self deliveries (Aimed / TargetActor / Touch) cast from the follower
   onto the target as before. Casting-type- and archetype-agnostic; a genuine
   `act.cast_self` (target == follower) is unchanged — which is why self-Candlelight always
   worked: its target already IS the caster.
2. **WHOSE rate it uses (`Actuation::ReattributeEffectCaster`):** the target self-casting
   means the engine created the ActiveEffect(s) with the TARGET as caster — so it would
   channel the TARGET's effective magnitude/rate (the target's skill/perks). **The follower
   is conceptually the caster, so every fresh AE of the spell is re-pointed
   (`ae->caster = follower`) back to the follower.** The engine then applies the FOLLOWER's
   perks/effectiveness at each application — a master-healer heals at HIS rate on a
   low-Restoration player; a follower with Mage Armor 3/3 lands the full flesh buff on the
   player; a follower's Augmented-element DoT burns at his rate. Pure caster attribution —
   **no magnitude math, no per-effect `magnitudeOverride`** (a single override scalar would
   collapse a multi-effect spell): every effect of a multi-effect spell carries the
   follower's rate, and because the sustain re-arms the SAME re-attributed AE each beat, the
   follower's rate persists for the whole concentration stream, not just the first beat.

The follower stays the blame actor and always pays the real magicka. All five direct-apply
sites route through both helpers: `ApplyTargetEffect`, `ApplyEffectFromTo` (AUTO), `CastOn`'s
combat force-half, and the two Logistics FF direct casts. The
`FORCE-CAST … at TTTTTTTT (self-delivery: target self-casts)` tag marks a re-route in the log.

**Instant vs concentration is still decided by the real casting type**
(`GetCastingType() == kConcentration`) — a genuine FireForget spell is delivered as a SINGLE
`CastSpellImmediate` (full instant magnitude once, paced by `fCastCooldown`), never sustained;
only real concentration spells enter `SustainConcentrationEffect`. Do not re-derive
concentration from effect archetype, "is a heal", or effect duration — read the SPEL.

## THE MATRIX — every cell delivers, bounded, gated. None barred.

| | self | player / ally | foe |
|---|---|---|---|
| fire-and-forget | `CastSelfDirect`, fCastCooldown beat | direct apply (main-thread posted), fCastCooldown window | package (animated) → **direct fallback on decline**; combat: `ForceCast` package → silent direct fallback |
| concentration | `CastSelfDirect`, **1 s beat**, 6 s heal / 15 s ward-utility cap | `CastTargetDirect`, **1 s beat**, 6 s heal / 4 s utility cap | `CastTargetDirect`, **1 s beat**, 1–4 s offense cap (LoS+LoF every apply) |

- Concentration rows are identical IN COMBAT and OUT OF COMBAT — same function, same
  channel registry, same beat, same caps — and every cell delivers the REAL sustained
  effect whose magnitude the ENGINE computes and channels (THE REAL-EFFECT CONTRACT).
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
  DECLINED`) but **HP flat on self AND player while magicka drained** — a forced
  concentration one-shot applies ~0 of its per-second magnitude (rate × ~one frame).
- **2026-08-18, interim `37d9982`/`ffdfbd7` (superseded):** first fixed HP with an
  explicit `RestoreActorValue` recreation of the BASE authored magnitude, then zeroed the
  sustained effect and kept the manual beats. marth's ruling ended both: the base value
  ignores the caster's skill/perks, the AV writes cover only value-modifiers, and the
  engine already computes the correct per-caster number into the ActiveEffect. **Final:
  THE REAL-EFFECT CONTRACT — attach the real effect once, sustain its duration, and let
  the ENGINE own the magnitude. Do not reintroduce manual magnitude math.**

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
