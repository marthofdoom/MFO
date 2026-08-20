# CAST DELIVERY — the single authoritative reference

**Read this before touching ANY cast path.** It describes the FINAL, shipped cast-delivery
model as it stands in the committed code — not the history of how it got here. If you need
the dead-ends, see "REJECTED APPROACHES" at the bottom (they exist so nobody retries them).
Everything lives in `native/Actuation.cpp`; the OOC dispatch is in `native/Logistics.cpp`.

---

## THE MODEL (in one paragraph)

MFO delivers **every** forced cast — self, player, ally, foe, fire-and-forget or
concentration — with a single engine call:

```
follower->GetMagicCaster(kInstant)->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f, follower);
// then hand-deduct the follower's real magicka (CastSpellImmediate spends none, §0.22):
//   spend = min(CalculateMagickaCost(follower), pool);  RestoreActorValue(kDamage, kMagicka, -spend);
// ALWAYS on the main thread (MainThread::Post), re-resolving actors by FormID inside the post.
```

This is package-free (beats a package-locked custom follower's alias lock), animation-free
(deferred), and passes through neither CasterConsent hook. There is **exactly ONE addition**
on top of it: a **CONCENTRATION + Self-delivery spell aimed at a NON-self target** is cast
through a **delivery-flipped proxy** (`ConcProxy`) so it lands on the recipient instead of
collapsing onto the follower. Fire-and-forget, self-casts, and non-Self concentration are the
untouched baseline.

---

## THE DELIVERY DECISION TABLE

| casting type | delivery | target | path | effect lands on |
|---|---|---|---|---|
| fire-and-forget | any (incl. **Self**) | self / player / ally / foe | baseline `CastSpellImmediate(sp, target, follower)` | **the passed target** |
| concentration | Aimed / TargetActor / Touch | player / ally / foe | baseline + `SustainConcentrationEffect` | the target |
| concentration | **Self** | **self** (target == follower) | baseline `CastSelfDirect`/`ApplySelfEffect` | the follower (correct) |
| concentration | **Self** | **player / ally / foe** (≠ follower) | **`ConcProxy` delivery-flipped copy** → concentration-on-others path | **the recipient** |

**THE KEY FACT — why FF works but concentration collapses (and why the proxy exists):**
For **fire-and-forget**, `CastSpellImmediate` applies the one-shot effect to the passed
`target` **regardless of the spell's Self delivery** — recipients do **NOT** self-cast, and no
proxy is needed (field-proven: Candlelight / flesh cast on an individual, the player, self, or
the whole party via AUTO all land correctly). For **concentration**, `CastSpellImmediate`
instead sets up a *channeled* cast whose target is resolved by the spell's **delivery**; a
`kSelf` concentration binds the sustained ActiveEffect to the magic-caster's **owner** (the
follower), so a player/ally concentration heal (e.g. Mysticism Fast Healing `0002F3B8` =
Conc + Self) collapses onto the follower and the recipient gets nothing. **Concentration Self
off-self is the ONLY broken case, and the proxy is the ONLY fix.**

---

## ConcProxy — the delivery-flipped concentration copy

`ConcProxy` / `DeliverySpell` in `Actuation.cpp` (used only by the concentration branches of
`ApplyTargetEffect` and AUTO's `ApplyEffectFromTo`).

- **Gate:** `delivery == kSelf && castingType == kConcentration && target != follower`.
  Anything else (`DeliverySpell`) returns the source spell unchanged.
- **The copy:** `proxy->data = source->data` then `proxy->data.delivery = kTargetActor`
  (**casting style PRESERVED — the copy stays concentration, it is NOT converted to FF**);
  `proxy->effects` is copied from the source **by pointer** (the `Effect*` objects stay owned
  by the real ESP source form). The follower casts the copy through the *unchanged*
  concentration path, so the channel resolves to the recipient; the follower is the caster
  (his rate + magicka), the player never casts / pays. `SustainConcentrationEffect` keys on
  the copy (what was cast), so a stream re-arms its own AE.
- **Two transient `0xFF__` dynamic slots, FILL-CAST-REUSE-FREELY.** The SPEL form is only
  needed *momentarily* to fire the cast — once the ActiveEffect is applied it is
  self-sufficient (it captured its own effect / magnitude / caster), so the slots carry **NO
  lifetime bookkeeping** (no idle timers, no live-AE guards): reuse a slot for the same source
  (so a stream's ~1 s beats re-arm the SAME AE — no stacking), evict round-robin when a third
  source needs one. Forms are minted once via `IFormFactory` and reconfigured in place.
- **Main-thread only.** `ConcProxy::Get` returns the source (not a proxy) when
  `!MainThread::IsInstalled()`, so `IFormFactory::Create` never runs off the main thread (VR).
- **Save-safety.** Dynamic forms are never serialized to the `.ess` or any MFO co-save. They
  do **not** survive a save-load, but their source keys (`g_src`) are stable ESP FormIDs — so
  `ClearSelfCasts()` (called by `ResetAllState` at kPreLoadGame / post-load / revert) calls
  **`ConcProxy::Reset()`**, which nulls `g_form[]` / `g_src[]` / `g_next` (drops the dangling
  pointers so the next cast re-mints fresh forms) and **clears each proxy's borrowed effects
  first** (so the load-time dynamic-form purge cannot double-free the source spell's shared
  `Effect*`). This is the cross-load UAF + double-free guard; it is NOT lifetime bookkeeping.
- **Dispel:** `TargetCastEndActor` dispels both the source and, via `ConcProxy::FormFor`, the
  proxy-keyed AE, so a proxied ward/heal cannot linger past its stream.

---

## STREAM BOUNDING — randomized caps + heal-to-full

A concentration **stream** (one per follower, in `g_targetCast` / `g_selfCast`, re-armed each
~1 s beat while its gambit keeps winning) is bounded so it always ends — even if the gambit
condition check is unreliable ("won't stop when the condition is met").

- **Randomized per-stream time cap** (`DrawConcCap`, a `std::mt19937` + `uniform_real_distribution`,
  worker-serial, **never serialized**), drawn ONCE when the stream starts and stored in the
  stream state (`tc.cap` / `sc.cap`) for loose, human timing:
  - **healing / utility / buff → uniform `[8, 15]` s**
  - **offense / hostile → uniform `[2, 6]` s**
- **Healing also stops early at ~full HP:** the reconciles end a heal stream when the recipient
  (or, for a self-heal, the follower) is at `Vocab::HealthPct >= kHealFullPct` (0.995) — the
  random cap is the backstop. This is the resolved answer to "stop at full": **yes for
  healing, time-capped.**
- **How a cap ends a stream** (`TargetCastReconcile` / `SelfCastReconcile`): a **heal-full**,
  **stale** (gambit stopped winning), or **gone** release is a true END-of-stream and dispels
  the sustained effect. A plain **cap** release on a still-wounded heal is release-only (no
  dispel) and the next winning tick re-streams it with a **fresh** random cap — varied human
  bursts, healing flows while wounded. A sticky **Buff** (ward) dispels on any release and is
  re-served if still wanted. The gambit re-evaluates between bursts, so a satisfied target is
  not re-served.

Note the three window constants (`kConcHealCap` 6 s, `kConcUtilityHold` 4 s,
`kConcSelfUtilityCap` 15 s) are the **per-beat SUSTAIN WINDOW** — the duration
`SustainConcentrationEffect` pins on the live AE each beat so it bridges the ~1 s gap — **not**
the stream cap. Keep each window larger than the beat gap.

## THE BEAT CADENCE

A concentration spell's cost is authored **per second**, so a stream beats about once per
second (`kConcApplyPeriod` = 1 s) while its rule wins: each beat deducts one second's cost
(clamped to the pool) and re-arms the sustained effect's window. Fire-and-forget spells beat at
`fCastCooldown` instead (their magnitude is per CAST; a 1 s beat would multiply it).

## GATES (every apply)

- **LoS + line-of-fire** — never stream a hostile cast into a wall or through a teammate
  (re-checked every apply in `CastTargetDirect`).
- **Already-active guard** — a duration buff / LIGHT (Candlelight/Magelight) must not re-apply
  while already active (light accumulation → ShadowSceneNode CTD). A momentary concentration
  value-modifier BYPASSES the guard (its sustained effect is active by design).
- **Affordability (§5.3)** — real cost + reserve floor gate every apply; the hand deduct is
  clamped `min(cost, pool)` so magicka never goes negative.
- **Threading (#14)** — the engine apply always runs via `MainThread::Post`, re-resolving
  actors by FormID inside the post. Never call `CastSpellImmediate` inline on the job worker.

---

## REJECTED APPROACHES (do not retry — each was built and failed)

- **`MagicTarget::AddTarget` for concentration** — applies the effect once but does **not
  CHANNEL** it, so a per-second concentration heal accumulates ~0 (magicka drains, HP flat).
  AddTarget is fine for an *instant* FF apply, but FF already works via the baseline call, so
  it is not needed anywhere. Field-verified failure.
- **Target self-casts the spell** (make the recipient the magic caster) — the engine then
  charges the **PLAYER's** magicka and the heal **stops when the player runs dry**; a
  follower's spell must never depend on the player's mana. Field-verified (deck a8d641bb).
- **Force-sustain re-attach** (re-`CastSpellImmediate` every beat / re-pin duration on a
  collapsed AE) — the Self concentration AE lives on the follower, so the target's sustain
  search never finds it → re-attach every beat, still no channel on the recipient.
- **Proxying ALL Self spells (FF included)** — a proxy-keyed AE defeats the SOURCE-keyed guards:
  a fanned Candlelight re-casts every cooldown → ShadowSceneNode "Active Lights" CTD, and a long
  FF Self buff on a shared slot is stripped early. FF Self delivery already works via the
  baseline call — it must **not** be proxied.

## SCOPE-CREEP LESSON

The 2026-08-19 rewrite changed FF Self delivery too and layered proxy lifetime machinery — it
broke Candlelight/flesh and caused the light CTD, and was reverted to baseline. **The fix must
touch ONLY the one genuinely-broken case: concentration + Self + off-self.** FF / light /
self-cast were already correct; do not touch them, do not re-broaden the proxy, do not
reintroduce AddTarget.

## KEY SYMBOLS (Actuation.cpp)

`ConcProxy` (namespace: `g_form`/`g_src`/`g_next`, `Configure`, `Get`, `FormFor`, `Reset`) ·
`DeliverySpell` (the gate) · `ApplyTargetEffect` / `ApplyEffectFromTo` (the two proxy wire-ins) ·
`SustainConcentrationEffect` (per-beat AE window) · `DrawConcCap` (randomized stream cap) ·
`TargetCastReconcile` / `SelfCastReconcile` (cap + heal-full release) · `ClearSelfCasts`
(→ `ConcProxy::Reset` on revert/load) · `TargetCastEndActor` (dispels source + proxy AE).
