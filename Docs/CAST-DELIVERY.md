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

**APMF OWNED CAST MODEL — the REAL, AI-DECIDED animated cast (default when APMF present; MODERATOR
model, marth 2026-09-02).** This is the important exception to the "everything is CastSpellImmediate"
model above, and it is DELIBERATELY the animated primary — a REAL cast the follower's OWN combat AI
DECIDES to make, NOT a forced one. `CastSpellImmediate` fires a spell but **cannot animate** (the
caster is driven by the animation graph — ENGINE_NOTES §0.13); the only animated cast is the vanilla
one the AI runs. MFO could already produce that real animated cast (§0.15a/§0.27/§0.28) — the ONLY
problem was that its deterministic route did it via the rooting **UseMagic package** (`ForceCast` →
`Packages::CastAt`), which stopped locomotion and took the package slot. The owned model keeps the
real cast and DROPS that cost.

- **APMF ARBITRATES; MFO EXECUTES (the moderator split).** `Actuation::CastOn`'s FF-non-self hostile
  branch CLAIMS two facets via APMF (`APMFBridge::ClaimCasting` + `ClaimCombatTarget` — APMF records
  the owner + suppresses competitors, it executes NOTHING), then MFO makes the AI decide to cast with
  its OWN mechanisms: writes the follower's own `selectedSpells` (`SelectCasterSpell`), commands the
  target via `Targeting::Command` → `currentCombatTarget`, grants `CasterConsent::Want` (permit our
  spell + deny competing), and the **Cast-biased combat style** (`Scheduler` applies `MFO_CastStyle`
  when a cast is wanted — raises the magic score so the AI CHOOSES to cast; the inverse of a deny).
  The AI then casts our spell at our target, **full animation, still MOBILE**. Resolves the
  long-deferred cast-animation gap ([[cast-animations-deferred-to-post-town-polish]]).
- **GRANULAR — movement is NOT touched.** The owned path claims ONLY the cast + combat-target facets;
  it does NOT claim/block the movement facet (no `SetDontMove`, no package). The follower keeps
  kiting/repositioning under its own control WHILE its AI casts. That granular non-interruption is
  the whole reason the cast routes through APMF.
- **NO force on this path.** `CastSpellImmediate` NEVER runs in the owned model — no rooting package,
  no unanimated force. If a follower's combat style still won't DECIDE to cast, that is a magic-score
  bias question (raise it, the inverse of a deny), NOT a reason to force. Force + the rooting UseMagic
  package survive ONLY in the LEGACY hybrid.
- **Concentration is untouched.** A concentration spell never enters this branch — its bounded
  direct-force fork (`ConcentrationCast` → `CastTargetDirect`/`CastSelfDirect`) returns earlier,
  because an AI-channeled concentration cannot be exact-bounded (the freeze). Exact-bounding holds.
- **Claim lifecycles + scope + degrade.** casting-claim = per-cast (released crisply on `!castSeen`);
  combat-target-claim = per-combat (re-pointed via APMF `Repoint` on a foe change, kept alive each
  in-combat tick, released at combat end; APMF's `CombatTarget::Release` relinquishes). Owned model is
  HOSTILE-only (`CasterConsent::ClassifySpell == Offense`), foe-only. Active iff
  `APMFBridge::Available() && bApmfCast && !bLegacyCastHybrid`. Turn on the MCM **bLegacyCastHybrid**,
  or run without APMF, and CastOn uses the ORIGINAL AI-first-wait + force-on-miss package hybrid
  (byte-identical to pre-APMF — the only place the rooting package + `CastSpellImmediate` force live).
  No save/co-save state; claims auto-expire via `APMFBridge::Tick` and drop at kPreLoadGame. See
  MAP.md `APMFBridge`.

**STANDING PRINCIPLE (marth, 2026-09-03): MFO is an APMF showpiece.** With APMF present, MFO
ROUTES THROUGH APMF and COMMITS to it — a legacy/pre-APMF path is the APMF-ABSENT degrade ONLY,
never a decline-fallback. A failure on an APMF-committed path is logged loudly and fails closed
(no dispatch this tick/action); it never silently reverts to the old mechanism, which would mask
an APMF-path bug behind a false "it worked". This governs every APMF-client path in MFO, not just
casting — see `Packages.cpp`'s loot-travel routing (ch.9 0x49 `OfferPackage`, `LootTravelFill`/
`Retarget`, MAP.md's `Packages.cpp — APMF LOOT-TRAVEL` entry) for the second worked example, and
apply it to any future channel MFO consumes from APMF.

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
- **A proxy cast starts a REAL ENGINE CHANNEL.** Casting the kTargetActor concentration proxy
  via `CastSpellImmediate` does not just apply a one-shot effect — the engine sustains a real
  concentration channel on the FOLLOWER that drains his magicka **per-second, independent of
  MFO's per-beat `ApplyTargetEffect`** (which is the only thing that logs `FORCE-CAST` and
  hand-deducts). So a runaway shows as **magicka draining with no `FORCE-CAST` log line** — that
  is the engine channel, not MFO. Two consequences drive the design below: the channel must be
  **explicitly interrupted** to stop (dispelling the target AE does not stop the caster-side
  channel), and the proxy FORM is **load-bearing for the channel's whole duration**.
- **Two transient `0xFF__` dynamic slots, SLOT-FOR-DURATION (marth's hard rule).** Reconfiguring
  or handing a slot's form to another cast while its channel is live corrupts the in-flight cast
  (**the freeze**) and entangles two streams (**heal-full stops the 1st heal but not the 2nd**,
  because re-casting the same form re-enters the 1st's residual channel). So each live stream
  **OWNS** a slot for its duration (owner = follower FormID): `ConcProxy::Acquire(owner, src)`
  reuses the owner's slot, else `Configure`s a **FREE** slot, else (both owned by other live
  streams) returns nullptr and the caller **SKIPS** (2-slot overflow). A slot is `Configure`d
  ONLY when free — never while its channel lives. `ConcProxy::Free(owner)` clears the owner
  markers (form kept for reuse) after the channel is interrupted.
- **Release = dispel + INTERRUPT + free, on EVERY release** (`TargetCastEndActor(target, spell,
  owner)`): dispels the source and the owner's proxy AE off the target, **interrupts the
  follower's `kInstant` magic caster** (`InterruptCast(false)` — stops the engine channel so the
  drain ends and the next stream starts clean), then `ConcProxy::Free(owner)`. The reconcile
  makes every release a true END (heal-full / magicka-out / cap / stale / gone); a still-wounded
  heal's cap ends the burst and the gambit re-serves a FRESH stream (new slot, new channel) next
  tick — so the channel always stops (no runaway) and an owned slot is never orphaned. The
  self path (`SelfCastEndActor`) likewise interrupts the self channel.
- **AUTO ally-heal, CONCENTRATION = SEQUENTIAL MOST-HURT (one channel per caster).** A
  concentration heal starts an engine channel and a caster sustains only ONE at a time, so AUTO
  cannot fan a concentration heal to N allies. `CastAuto` intercepts a concentration heal
  (`kind==Heal || SpellHealsHealth`) BEFORE the fan / the `g_autoCast` cooldown gate and picks
  the SINGLE most-hurt member below the threshold (player OR teammate OR self, `HealthPct` under
  `min(threshold, kHealFull)`), serving it via the safe single-target path — `CastTargetDirect`
  (owner-keyed proxy slot, InterruptCast on release) for another actor, `CastSelfDirect` for
  self. It runs EVERY tick (no cooldown gate — the stream self-paces at ~1 s). **Hysteresis
  (anti-oscillation):** it STICKS with the current recipient while they are still below the
  ceiling and only SWITCHES when they top off OR another member is > 15% (`kHealSwitchMargin`)
  more hurt — otherwise re-picking the lowest-HP each beat would thrash between two similarly-
  hurt allies (each beat heals one a few HP above the other, flipping the pick + dispel/
  interrupt/re-cast every second). So it finishes one, then serves the next; a critically-hurt
  member still interrupts. When a recipient tops off (heal-full RELEASE, slot frees) the
  next-most-hurt is served — over a few seconds every hurt ally is topped. This uses
  ONE slot at a time (respects the 2-slot cap, never a live-slot collision). FF/instant heals
  and non-heal buffs still FAN below (`ApplyEffectFromTo`) — an instant apply has no channel, so
  N-at-once is fine; a conc-Self **non-heal** buff fanned via AUTO is skipped (no stream to own a
  slot; rare).
- **Main-thread only.** `ConcProxy::Acquire` returns nullptr when `!MainThread::IsInstalled()`,
  so `IFormFactory::Create` never runs off the main thread (VR); the caller skips.
- **Save-safety.** Dynamic forms are never serialized; they do not survive a save-load, but the
  slots key on stable ESP source FormIDs, so `ClearSelfCasts()` (kPreLoadGame / post-load /
  revert) calls **`ConcProxy::Reset()`** — nulls each slot and clears each form's borrowed
  effects first (cross-load UAF + double-free guard).
- **Breadcrumbs** (terse `[cast]` log): `proxy slot ACQUIRE/RECONFIG/FREE/OVERFLOW owner …` and
  `stream RELEASE (heal-full | magicka-out | cap | stale | gone) …` — so a field test is legible.

---

## STREAM BOUNDING — long randomized caps, MAGICKA-GATED, heal-to-full at 99.95%

A concentration **stream** (one per follower, in `g_targetCast` / `g_selfCast`, re-armed each
~1 s beat while its gambit keeps winning) is bounded three ways so it always ends.

- **Randomized per-stream time cap** (`DrawConcCap`, a `std::mt19937` + `uniform_real_distribution`,
  worker-serial, **never serialized**), drawn ONCE when the stream starts and stored in the
  stream state (`tc.cap` / `sc.cap`) for loose, human timing:
  - **healing / utility / buff → uniform `[8, 15]` s**
  - **offense / hostile → uniform `[2, 6]` s**
- **Magicka-out stop** (`TargetCastReconcile` / `SelfCastReconcile`): the moment the CASTER can't
  afford the next beat's cost (`have < CalculateMagickaCost(follower)`), the stream ENDS (dispel)
  instead of re-applying at 0. This is what makes the LONG caps safe — a channel stops on
  magicka-out first, so a long cap never over-drains the follower (which had starved the
  Candlelight AUTO fan mid-party). A dried follower ends the channel; magicka regens; the gambit
  re-serves a fresh burst.
- **Heal-to-full at 99.95%** (`kHealFullPct` = `Vocab::kHealFull` = 0.9995): a heal stream ends
  early when the recipient (or the follower, self-heal) is at `HealthPct >= 0.9995`; the random
  cap is the backstop. This is the resolved answer to "stop at full": **yes for healing.**

**THE "AT-OR-BELOW-100 NEVER STOPS" BOUNDARY BUG (marth's root-cause).** A heal gambit condition
is "target HP below X%". At **X = 100** the effective test never fails — `HealthPct` asymptotes
to but rarely equals exactly 1.0, so a topped-off target keeps satisfying "HP below 100%" and the
heal **re-dispatches forever** (independent of the stream cap; the stream cap alone just chunks
the endless cast into bursts). FIX: a heal threshold's TOP is clamped to `Vocab::kHealFull`
(99.95%) at EVERY heal re-dispatch / target-selection site, so a target at `>= 99.95%` no longer
satisfies a 100% threshold and stops triggering — and the stream heal-full stop uses the SAME
value so re-dispatch and stream stop agree. Sites: `Evaluator::ConditionTrue`
(`kCondSelfHpBelow` / `kCondPlayerHpBelow`, `< min(p, kHealFull)`), `Evaluator::PickAlly`
(`lowest = min(param, kHealFull)`), `CastAuto` heal fan (`>= min(threshold, kHealFull)`). Only the
top boundary is clamped (`min`); thresholds under 100% are unchanged.

- **How a cap/stop ends a stream:** a **heal-full**, **magicka-out**, **stale** (gambit stopped
  winning), or **gone** release is a true END-of-stream and dispels the sustained effect. A plain
  **cap** release on a still-wounded heal is release-only (no dispel) and the next winning tick
  re-streams it with a **fresh** random cap — varied human bursts, healing flows while wounded. A
  sticky **Buff** (ward) dispels on any release and is re-served if still wanted.

**FF affordability** is unchanged: `CastAuto`'s `affordable()` gate `break`s the fan the moment
the running budget can't afford the next cast (all fan casts share one cost, so once one is
unaffordable the rest are too — a clean end, never an attempt-and-fail loop). The mid-party fan
break was the HELD heal draining the caster; the magicka-out stop above prevents that.

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

## COMPOSED FORCED CAST (CFC) — bHealAnimPackage repurposed, DEFAULT OFF

This replaces the old OPT-IN ANIMATED HEAL section (2026-09-04 rework,
`Docs/SPEC-FORCED-CAST.md`). The M9 forced-casting PACKAGE route is gone.
`Packages::HealAnimFill`, `g_healAnimMap`, and the two UseMagic PACKs
(`MFO_APMFHealSelfPackage` 0x83B, `MFO_APMFHealPlayerPackage` 0x83C) are deleted. A
heal is a cast facet now, not a package facet. The FormIDs stay retired forever
(INVARIANTS #41), never recycled.

`ComposedCast::Try(follower, spell, target, kind)` (`native/ComposedCast.cpp`) sits
where `HealAnimFill` used to be called, in `CastSelfDirect`/`CastTargetDirect`
(`Actuation_Direct.cpp`), after the competence gate. It composes five things before
it ever touches the hand.

1. An APMF cast-execution claim (`kIntent_Cast`, ABI v5, `APMFBridge::ClaimCast`).
   APMF denies the follower's own casting and re-arm for a bounded TTL. APMF fires
   nothing. Movement is never claimed.
2. A `CastBounds::Arm` registration for (actor, spell) and (actor, proxy) if any.
   This is the fix for the CasterConsent HARD-ABORT described below.
3. The trigger. One isolated seam, `DriveObservedCast`, turns an armed cast into a
   real animated one.
4. Observe. The trigger arms an "expected cast" hand-off that
   `Diagnostics::SpellSink` consults. A match logs the animated path.
5. Restore. `CastBounds::Disarm` and `APMFBridge::ReleaseCast` run on every exit,
   including every degrade.

Any failure at any step degrades to the caller's existing `kInstant` apply. A heal
always lands, exactly as it does today.

### The trigger is not built yet

`SPEC-FORCED-CAST.md`'s original guess for the trigger was a hand-built
`RE::TESActionData::Process` call with `ActionRightAttack`/`ActionRightRelease` and
four `BGSAction` FormIDs. That guess was dropped (steering decision, 2026-09-04,
marth-approved). It was unproven and version-fragile, and it was never built.

The trigger is OBSERVE-AND-REPLICATE instead. A parallel APMF passive observer at
the 0xAD seat will capture the engine's real full-animation NPC cast sequence from a
deck cycle: the `MagicCaster` state machine, the animation-graph cast events, and
the charge/fire boundary. MFO will replicate that captured sequence in
`DriveObservedCast` (`native/ComposedCast.cpp`).

Until that sequence lands, `DriveObservedCast` returns `kNotImplemented` and `Try`
always returns false. The whole executor is runtime-inert today, byte-identical to
the existing kInstant heal, even with `bHealAnimPackage` switched ON. This is a
deliberate, honest state, not a bug. The build is complete. Only the one seam is
still open.

### CastBounds — the HARD-ABORT fix (§2)

`CasterConsent::ConcUnboundedDeny` hard-aborts a tracked follower's own
concentration cast at Exact unless MFO can prove the stream is one MFO itself is
metering. Before this fix the only proof was the legacy alias-package stream
(`Packages::StreamLive`, written only by `Packages::Begin`). Every other
MFO-arranged cast, the `ConcProxy` direct force and now `ComposedCast`, passed
through the same `CheckCast` (0x0A) thunk but was never registered as MFO's own.
Exact-bounding vetoed those as an unbounded AI stream. That was the deck
HARD-ABORT of `0002F3B8` / `FF001BA4`.

`CastBounds` (`native/CastBounds.h/.cpp`) generalizes the legacy one-slot
`g_liveStream` contract to a lock-free 8-slot registry. `Arm(actor, spell, proxy,
ttlMs)` registers an MFO-executed bounded cast. `Live(actor, spell)` is the
combat-thread reader, lock-free and fail-safe. A torn, cleared, or expired slot
reads as no-match, never as a false permit. `Disarm(actor)` clears every slot an
actor owns. `Reset()` runs beside `ConcProxy::Reset()` on `kPreLoadGame`.

Three call sites in `CasterConsent.cpp` now check `CastBounds::Live` before
falling through to a deny: `ConcUnboundedDeny`, the `CheckStartCast` thunk, and
`CheckCastThunk` (0x0A). A registered cast early-passes every one of them, the
same way the legacy package stream always did.

**Why `ComposedCast::Try` is the only caller today, and that is correct, not a
gap.** `CastSpellImmediate` skips the `MagicCaster` state machine entirely
(`RequestCastImpl → StartChargeImpl → StartReadyImpl → StartCastImpl →
FinishCastImpl`, ENGINE_NOTES §0.13) — it is the "apply this now, no actor
deliberation required" trap/script path. The hooked `CheckStartCast`/`CheckCast`
thunks fire only when the ENGINE itself deliberates a cast through that state
machine, which `ConcProxy`'s plain `kInstant` `CastSpellImmediate` never enters.
So `ConcProxy` never reaches `ConcUnboundedDeny` in the first place and needs no
`CastBounds` registration — proven by it shipping and healing correctly at
`iCastControl` Exact today, with no HARD-ABORT. The deck HARD-ABORT
(`0002F3B8`/`FF001BA4`) was specific to the AI-DELIBERATED heal-anim PACKAGE
build (the since-shelved `feat/heal-anim-proxy`), never the plain kInstant path.
`ComposedCast`'s future hand cast is the one MFO-driven cast that WILL deliberate
through the real state machine (that is the whole point of the trigger — a real,
animated cast), so it is the only path that needs the bound. `ComposedCast::Try`
being the sole `CastBounds::Arm` caller is by design.

### Path comparison

| path | trigger | animated | target reach | status |
|---|---|---|---|---|
| kInstant force-apply | `CastSpellImmediate` | no | any actor | baseline, always on |
| APMF owned cast | the follower's own AI decides | yes | hostile foe only | default when APMF is present |
| Composed Forced Cast (CFC) | `ComposedCast::Try` | yes, once the trigger seam lands | any actor, via a delivery-flipped proxy | opt-in (`bHealAnimPackage`). Inert today, degrades to kInstant every time |

See `native/ComposedCast.cpp` `TtlMsFor` for the per-kind bound (heal 6 s, offense
4 s, buff/utility 4 s) and `Config::g_cfcBackoffMs` (default 10 s) for the
per-follower degrade backoff that stops a stub trigger from churning an APMF claim
every beat.

## KEY SYMBOLS (Actuation.cpp)

`ConcProxy` (owner-keyed `Slot g_slot[2]{form,source,owner}`, `Configure`, `Acquire`,
`FormForOwner`, `Free`, `Reset`) · `DeliverySpell` (gate; nullptr → caller skips) ·
`ApplyTargetEffect` (the proxy wire-in; AUTO `ApplyEffectFromTo` skips conc-Self) ·
`SustainConcentrationEffect` (per-beat AE window) · `DrawConcCap` (randomized stream cap) ·
`TargetCastReconcile` / `SelfCastReconcile` (cap + heal-full + magicka-out release) ·
`ClearSelfCasts` (→ `ConcProxy::Reset` on revert/load) · `TargetCastEndActor(target,spell,owner)`
(dispel source + owner proxy AE, `InterruptCast` the channel, `ConcProxy::Free`) ·
`SelfCastEndActor` (dispel + `InterruptCast` the self channel).
