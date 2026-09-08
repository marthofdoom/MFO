# CAST DELIVERY — the single authoritative reference

**Read this before touching ANY cast path.** It describes the shipped cast-delivery
model as it stands in the committed code — not the history of how it got here. If you need
the dead-ends, see "REJECTED APPROACHES" at the bottom (they exist so nobody retries them).

**WHICH PATH IS PRIMARY (corrected 2026-09-07 — read this before the section below).**
With **APMF present** — the normal case, and what MFO ships as its showpiece — the primary
cast path is **`ComposedCast::Try` → `APMFBridge::ClaimHealCast` / `ClaimOffenseCast` →
`RequestCast`** (`native/ComposedCast.cpp:198-460`, `native/APMFBridge.cpp:661`/`:875`). The
follower's OWN AI performs a real animated cast, driven by APMF's four `CombatMagicCaster`
seats plus `CheckShouldEquip`; neither mod calls an engine cast verb. The `CastSpellImmediate`
model described in the next section is the **APMF-ABSENT DEGRADE** plus the legacy hybrid —
it is not "the model", and the STANDING PRINCIPLE further down says exactly that. This
document grew by appending a dated section per branch, so its oldest framing sits at the top;
read the top as history and lines 272 onward as the current mechanism.

**WHERE THE CODE LIVES (corrected 2026-09-07).** Not "everything in `native/Actuation.cpp`".
`Actuation.cpp` holds the dispatch and the owned-cast branch; `native/Actuation_Direct.cpp`
holds `ConcProxy`, `SustainConcentrationEffect`, `DrawConcCap`, `CastSelfDirect`,
`CastTargetDirect` and the reconcilers; `native/ComposedCast.cpp` holds the composed/claimed
path; `native/APMFBridge.cpp` holds the claim lifecycle; the OOC dispatch is in
`native/Logistics.cpp` and `native/Logistics_Cast.cpp`.

---

## THE APMF-ABSENT / LEGACY MODEL (in one paragraph)

*Historical framing, still exactly correct for the degrade path. See "WHICH PATH IS PRIMARY"
above for what runs when APMF is present.*

On the legacy/degrade path MFO delivers a forced cast — self, player, ally, foe,
fire-and-forget or concentration — with a single engine call:

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

- **APMF DRIVES the exact spell; MFO EXECUTES the equip/target/consent half (PORTED
  feat/offense-cast-seats, 2026-09-05, off the earlier ch.8 `kIntent_SelectSpell` gate-only claim —
  see the OFFENSE CAST PORT section below).** `Actuation::CastOn`'s FF-non-self hostile branch CLAIMS
  the `kIntent_Cast` facet (`APMFBridge::ClaimOffenseCast` — the SAME ch.8b facet `ClaimHealCast`
  uses) + `ClaimCombatTarget` (ch.6, unchanged), then MFO makes the equip/target/consent half true
  with its OWN mechanisms: `Loadout::Prepare`'s `EquipSpell` puts the spell in the LEFT hand, commands
  the target via `Targeting::Command` → `currentCombatTarget`, and grants `CasterConsent::Want` (a
  veto-removal, not an invented consent). While the `kIntent_Cast` claim stands, APMF's FIVE engine
  vfunc seats (the SAME seats `ClaimHealCast` drives) select/equip/charge/aim/fire/channel EXACTLY the
  claimed spell at EXACTLY the claimed target — so (unlike the retired gate-only claim, which only
  ARBITRATEd + DENIED a competitor's own selection and let the AI pick its own spell) **the gambit's
  named spell is now the one that actually fires.** The AI then casts our spell at our target, **full
  animation, still MOBILE**. Resolves the long-deferred cast-animation gap
  ([[cast-animations-deferred-to-post-town-polish]]) AND the long-standing "target right, spell wrong"
  defect ([[cast-gambit-spell-choice-not-enforced]]).
- **GRANULAR — movement is NOT touched.** The owned path claims ONLY the cast + combat-target facets;
  it does NOT claim/block the movement facet (no `SetDontMove`, no package). The follower keeps
  kiting/repositioning under its own control WHILE its AI casts. That granular non-interruption is
  the whole reason the cast routes through APMF.
- **NO force on this path.** `CastSpellImmediate` NEVER runs in the owned model — no rooting package,
  no unanimated force. If a follower's combat style still won't DECIDE to cast, that is a magic-score
  bias question (raise it, the inverse of a deny), NOT a reason to force. Force + the rooting UseMagic
  package survive ONLY in the LEGACY hybrid.
- **Concentration does not enter THIS branch** (the owned OFFENSE branch) — its bounded fork
  (`ConcentrationCast` → `CastTargetDirect`/`CastSelfDirect`) returns earlier, because an
  AI-channeled concentration cannot be exact-bounded (the freeze). Exact-bounding holds.
  **CORRECTION (2026-09-07): "concentration is untouched" full stop is WRONG, and this doc
  said both things.** Since the 2026-09-05 pass documented at "a non-heal CONCENTRATION
  stream now reaches the engine-seat path too" (search that phrase), `CastSelfDirect` and
  `CastTargetDirect` call `APMFBridge::ClaimOffenseCast` DIRECTLY with `concentration=true`,
  so an OOC concentration heal IS an APMF claim path. *(The log label that made this
  invisible is FIXED on `main`: `Logistics.cpp:1583` now prints `"APMF claimed"` vs
  `"direct force, bounded"` conditionally instead of always claiming direct force —
  `Docs/DIAG-2026-09-06-deny-heal-failures.md` RC1 is closed on that point.)*
- **Claim lifecycles + scope + degrade.** offense-cast-claim (`kIntent_Cast`, TTL-bounded) = per-cast
  (released crisply on `!castSeen`); combat-target-claim = per-combat (re-pointed via APMF `Repoint`
  on a foe change, kept alive each in-combat tick, released at combat end; APMF's
  `CombatTarget::Release` relinquishes). Owned model is HOSTILE-only (`CasterConsent::ClassifySpell ==
  Offense`), foe-only. Active iff `APMFBridge::Available() && bApmfCast && !bLegacyCastHybrid`.
  **A REFUSED offense claim FAILS CLOSED** (corrected 2026-09-07 — this bullet used to say it
  "falls through to the SAME legacy hybrid"; `fix/mfo-no-decline-fallback` removed that):
  when APMF is present AND capable (`APMFBridge::OffenseCastClaimSupported()`) and it still says
  no, `Actuation.cpp:1039-1052` (the PRE-FLIGHT, before any hand is touched) returns
  `{FailedSkill, "APMF refused the cast claim", transparent}`
  and logs `[apmf] … APMF REFUSED …`. Nothing routes around APMF. The cast does not happen this
  tick, the rules BELOW it still run (transparent, not paralysis), and a refusal is a bug to fix in
  APMF rather than a condition to degrade through. Only the ABSENCE of the channel degrades:
  turn on the MCM **bLegacyCastHybrid**, or run without
  APMF, and CastOn uses the ORIGINAL AI-first-wait + force-on-miss package hybrid (byte-identical to
  pre-APMF — the only place the rooting package + `CastSpellImmediate` force live). No save/co-save
  state; claims auto-expire via `APMFBridge::Tick` and drop at kPreLoadGame. See MAP.md `APMFBridge`.

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
off-self is the ONLY broken case, and a delivery-flipped proxy is the fix.**

**WHICH proxy, though — the table above is the APMF-ABSENT column (clarified 2026-09-07).**
On the legacy/degrade path MFO builds `ConcProxy` itself, as described below. **With APMF
present, APMF mints its own delivery-flip proxy** for a `kSelf`-delivery spell aimed at a
non-self target (`req.proxy = 0` in `EnsureCastClaimLocked`) — MFO never inspects delivery
and never builds one on that path. Read every row of this table as "APMF absent"; the
APMF-present equivalent is the CAST-CLAIM OBSERVABILITY section near the bottom.

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

**S1 rework (2026-09-05, "prove cast+heal on APMF"): the MFO-side hand-drive is
RETIRED.** The first cut of CFC (`kIntent_Cast`/`RequestCast`, ABI v5) had MFO
itself force-equip the follower's hand caster and replay the observed cast
sequence (`DriveObservedCast`/`PhaseSelect`/`PhaseFire`/`HealProxy`) — it stayed
OBSERVE-ONLY (always degraded to kInstant) because it raced APMF/the AI for the
SAME hand, and that race caused a cross-thread use-after-free CTD in the field.
APMF's `feat/cast-act` graduated the EXISTING `kIntent_SelectSpell` channel into a
declarative contract instead: name the spell/hand/target and APMF itself equips,
drives the animated sequence, and guarantees delivery. `native/ComposedCast.cpp`'s
`DriveObservedCast` family and `APMFBridge`'s old `ClaimCast`/`ReleaseCast`/
`IsCastClaimActive`/`CastReq` were retired from MFO entirely at this point.

**feat/mfo-cast-port (2026-09-05): PORTED BACK to `kIntent_Cast`/`RequestCast` —
this time for real, RE'd and shipped.** APMF's `feat/ai-cast-seats-impl` in turn
RETIRED the `kIntent_SelectSpell` +ACT drive above: `ival`/`target`/`pos` on that
channel are now accepted-and-ignored (`APMF_API.h`), and the actual native cast is
driven by FIVE engine vfunc seats APMF answers while a `kIntent_Cast` claim
stands — `CheckShouldEquip` (0x0F), `CheckStartCast` (0x06), `GetMagicTarget`
(0x0A), `CheckStopCast` (0x07), `SetupAimController` (0x0D) — so the FOLLOWER'S
OWN AI selects/equips/charges/aims/fires/channels the spell, with ZERO
engine-cast call from either mod (this is exactly the design the now-superseded
"FORWARD LOOK" note further below used to flag as research-only; it is shipped
now). `native/APMF_API.h` was re-synced byte-identical to APMF's copy (still
append-only, `APMF_CastRequest`'s layout unchanged) and `APMFBridge::
ClaimHealCast` rebuilt to call `RequestCast` directly. `ComposedCast.cpp` stays a
thin shim over it — the shape below is CURRENT, not history.

`ComposedCast::Try(follower, spell, target, kind, stopPct)`
(`native/ComposedCast.cpp`) sits where `HealAnimFill` used to be called, in
`CastSelfDirect`/`CastTargetDirect` (`Actuation_Direct.cpp`), after the
competence gate — call sites forward an extra `a_stopPct` now (see STOP-PERCENT
below), otherwise unchanged. It is **HEAL-ONLY-gated** (`kind != SpellKind::Heal`
→ immediate false): offense and buff casts never enter this module and stay on
the byte-identical AI-fired / kInstant paths. It composes two things, no hand
touch at all:

**Third call site (feat/castone-heal-gate, 2026-09-06, API-PORT-AUDIT.md #1):**
`Actuation.cpp`'s `CastOn` — the FF (non-concentration) non-self dispatch —
now also calls `Try` directly, immediately after its `ownedCast` (Offense-only)
block and before the AI-first-grace wait, with `stopPct=0` (no per-gambit
threshold in scope there, same as every site but `CastAuto`'s). This closes the
one gap the ownedCast port left: a fire-and-forget Heal/Buff spell aimed at an
ally or the player never reached `ComposedCast::Try` at all, so it fell into the
legacy grace+`ForceCast` hybrid even with APMF present. `a_target != a_follower`
is required explicitly (a self-target can still reach this far when `bCastSelf`,
dev-only default off, never forked it off earlier — self-cast stays
`CastSelfDirect`'s own gated mechanism). On success: the same `HoldCastLock` +
OPAQUE-hold return shape `ownedCast`'s own success path uses.

**CORRECTED 2026-09-07 — this paragraph used to lump every "refusal" together and say
they all "fall straight through to the same legacy hybrid, byte-identical". A LOST CLAIM
DOES NOT.** The four-state `TryResult` splits them: `NotApplicable` (Buff kind,
AE/APMF/`bHealAnimPackage` absent — the channel was never there) falls through to the
legacy hybrid, byte-identical; **`ApmfRefused` (APMF present, capable, and it said no)
FAILS CLOSED** at `Actuation.cpp:1082-1089` with no hybrid and no `kInstant`; `Held`
returns a transparent NoOp. **In-combat only** — `CastOn`
runs only from `Actuation::Fire`'s combat dispatch. The identical OOC gap
(`Logistics.cpp:1606-1690`, the FIRE-AND-FORGET beneficial direct-apply leg — `:1606`
"FIRE-AND-FORGET from here down", `CastSelfDirect` at `:1671`, API-PORT-AUDIT §2.3; **NOT** the
OOC concentration block at `:1465-1580`, which this same doc says above already IS an APMF claim
path) is deliberately untouched: it is not established that
`kIntent_Cast`'s engine seats function without a live `CombatController` (see
API-PORT-AUDIT.md §5.1) — do not port that leg on the assumption it mirrors this
one.

1. `APMFBridge::ClaimHealCast(fid, spellID, targetID, APMFBridge::kApmfHandLeft,
   isConcentration, stopPct)` — the `kIntent_Cast` claim:

   ```cpp
   APMF_API::APMF_CastRequest req{};
   req.spell  = spellFormID;
   req.proxy  = 0;               // APMF mints its own delivery-flip proxy (core/CastProxy.h)
   req.target = castTargetFid;   // 0 => self. LOAD-BEARING now (seat 0x0A/0x0D read it directly)
   req.flags  = APMF_API::kCastFlag_LeftHand            // ALWAYS left -- see the HAND FIX below
              | (isConcentration ? APMF_API::kCastFlag_Concentration : 0u)
              | APMF_API::MakeStopPct(stopPct);          // 0 = stop at full restoration
   req.ttlMs  = APMFBridge::kHealCastTtlMs;              // 6s, the SAME window CastBounds::Arm uses
   Handle h = api->RequestCast(actorFid, basis, &req);
   ```

   Unlike the retired +ACT channel's `Repoint`, `RequestCast`'s rich payload has
   NO in-place re-point: a repeat call with the SAME (spell, target, hand,
   concentration, stopPct) is a cheap no-op refresh; a CHANGE in any of them
   releases the old claim and requests a fresh one (a new bounded window, never
   two live claims on one follower's heal slot at once).

   **HAND FIX (2026-09-05, deck: heal driven right hand, then displaced by the
   equip gambit's own re-equip ~500ms later, cast never left "rest").**
   `ClaimHealCast` used to pass `hand=0` (auto); APMF's auto resolution prefers
   the RIGHT hand when it reads free, and while a follower was transiently
   unarmed (a facet-expiry gap, see below) auto grabbed the right hand — then
   the equip gambit's own periodic re-equip put the melee weapon right back
   into that hand, displacing the spell. THE RULE: whenever an equip gambit is
   actively force-holding a weapon (`APMFBridge::IsEquipmentClaimActive`), that
   weapon owns the RIGHT hand, so a spell must claim LEFT rather than contest
   it; LEFT is also the correct fallback with no weapon held (heals are
   left-hand almost always regardless). Still the ONLY hand policy a heal
   passes — there is no more auto/right/dual hand-mode plumbing on THIS path
   at all (the retired +ACT drive's `ival` hand bits are gone with it), and a
   heal deliberately bypasses the general policy below.

   **THE GENERAL INTELLIGENT HAND POLICY (`Loadout::PlanCastHand`/
   `CanDualCast`, `native/Loadout.h`/`.cpp`, 2026-09-06) NOW EXISTS, for
   offense's future use.** A pure decision, no engine writes, no APMF
   dependency: a weapon owning the right hand (`APMFBridge::
   WeaponHandActive` — a live grip read OR'd with `IsEquipmentClaimActive`,
   closing the SAME race the HAND FIX above does) still forces LEFT; with
   both hands genuinely free and one spell wanted, `CanDualCast` checks the
   follower's REAL dual-casting perk for that spell's own school (HasPerk +
   the base template's perk list, the vanilla Skyrim.esm perks
   `0x000153CD`..`0x000153D1`) and whether current magicka affords the
   doubled cost (`CalculateMagickaCost x 2.8`, the vanilla
   `fMagicDualCastingCostMult`) — eligible plans `DualCast`, otherwise
   `EitherFree` (the caller may assign either hand, including giving a
   second, different wanted spell the other hand — the "juggle" case).
   **`DualCast` IS NOW EXPRESSIBLE (2026-09-06, `APMF_API::kCastFlag_DualCast`,
   bit 3, append-only mirror of APMF's own header) — the claim SHAPE gap is
   closed.** `APMFBridge::HandFor(Loadout::HandPick)` translates
   `PlanCastHand`'s decision into the bridge's own `a_hand` encoding
   (`kApmfHandLeft` / the new `kApmfHandDualCast` / 0 for either-hand), and
   `EnsureHealClaimLocked`'s `req.flags` build sets `kCastFlag_DualCast`
   INSTEAD OF `kCastFlag_LeftHand` for `kApmfHandDualCast` — never both (APMF's
   header: "do NOT set `kCastFlag_LeftHand` alongside it"). It is still only a
   HINT: the engine seats decide whether both hands actually arm, and a
   follower who can't afford it (or the engine otherwise won't) simply casts
   single-hand — no retry, no re-claim, no fallback that would make that
   degrade look like a real dual-cast. A `[cfc]` log line distinguishes
   "asked for dual, claim granted" from "asked for dual, claim REFUSED
   outright" (silence = never asked), so the field can tell a refused second
   hand from a policy that never requested one.
   **NOW WIRED (integration/2026-09-06, then feat/per-hand-cast-slots,
   2026-09-06)** — supersedes the "still NOT wired" state this paragraph used
   to describe. `Actuation.cpp`'s `CastOn` (`ownedCast` branch, ch.8b
   `ClaimOffenseCast`) consults `Loadout::PlanCastHand` (via the per-hand
   cast-gambit lock's `ResolveCastHand` — see the "PER-HAND CAST SLOTS"
   section below) and passes the resolved hand plan through directly, rather
   than `HandFor`'s bare `HandPick` mapping (which cannot express an
   already-juggled concrete Right). `HandFor` still documents the raw
   `HandPick` → `a_hand` mapping used for the Left/DualCast cases.

   **STOP-PERCENT (feat/mfo-cast-port, Task 3).** Bits 8-15 of `flags`
   (`APMF_API::MakeStopPct`) tell seat 0x07 `CheckStopCast` to end a
   CONCENTRATION channel once the target's HEALTH reaches a whole percent
   (1..100) of its PERMANENT actor value, instead of the engine default (full
   restoration). `Actuation_Direct.cpp`'s `CastAuto` is the one call site in the
   tree where a real per-gambit heal threshold is in scope at a
   `CastSelfDirect`/`CastTargetDirect` call (a "Health < N% → Heal" rule's own
   `conditionParam`, already used there to pick the neediest target) — it
   converts that fraction to a whole percent and forwards it through
   `CastSelfDirect`/`CastTargetDirect` → `ComposedCast::Try` →
   `ClaimHealCast`. Every OTHER call site (`Actuation.cpp`'s `ConcentrationCast`
   self-fork and target-fork, `Logistics.cpp`'s OOC concentration dispatch) has
   no numeric threshold in scope today and passes the default 0 (stop at full),
   byte-identical to the pre-port behaviour. Harmless (ignored) on an instant
   heal — there is no channel to stop early.

   APMF's engine seats equip the resolved hand, drive the AI's own native
   animated cast, and channel/stop it per the flags above. Because
   `RequestCast` has no in-place re-point, a spell/target/hand/concentration/
   stopPct CHANGE is a release-and-refresh (a brand-new bounded claim), not a
   silent repoint of the old one; the SAME (fully unchanged) values are still a
   cheap no-op refresh, so the steady-state heal-over-time cadence pays no
   release/re-request churn.
2. A `CastBounds::Arm` registration for (actor, spell) (proxy = 0 — APMF owns any
   delivery-flip proxy on its own side now). This is still the fix for the
   CasterConsent HARD-ABORT described below: MFO's globally-installed consent hook
   also intercepts APMF's seat-answered `CheckStartCast`/`CheckCast` on the SAME
   Restore caster vtable, so it still needs to be told this (actor, spell) pair
   is authorized.

**A refused claim FAILS CLOSED — corrected 2026-09-07.** This paragraph used to
say a refused claim "degrades to the caller's existing `kInstant` apply. A heal
always lands, exactly as it does today." That is FALSE since
`fix/mfo-no-decline-fallback`, and it is the heal-side twin of the offense
decline-fallback the showpiece principle forbids. `ComposedCast::Try` SPLITS a
`false` from `ClaimHealCast` (`ComposedCast.cpp:441`):
`APMFBridge::HealCastClaimSupported()` true → **`TryResult::ApmfRefused`**, and
every caller fails closed with no `kInstant` apply (`Actuation.cpp:1082-1089`,
`Actuation_Direct.cpp:889` and `:1186`); the code says so itself at
`ComposedCast.cpp:429` — *"the caller FAILS CLOSED, no kInstant apply"*.
**Only `TryResult::NotApplicable` — the channel was never there (ABI < 5, toggle
off, APMF absent) — degrades to `kInstant`.** So a heal is NOT guaranteed to
land: it can be lost to APMF arbitration, on purpose and loudly.
`CastBounds::Disarm` still runs on the refusal path. Release
(`ComposedCast::End` → `APMFBridge::ReleaseHealCast` + `CastBounds::Disarm`) has no
dedicated per-tick reconcile call site — `Try` short-circuits `CastSelfDirect`/
`CastTargetDirect` before their own stream bookkeeping runs, so an abandoned claim
relies on the SAME "stop refreshing → expire" backstop the offense
combat-target/cast-select claims already use. **This is deliberate, not a gap**
(marth: "hold the claim across ticks and Repoint rather than release/re-claim") —
the caller re-`Try`s (which Repoints the SAME handle) every tick the gambit still
wants the heal; the instant the gambit stops evaluating that rule (target healed,
lost, out of range, spell switched to a different one), `Try` simply stops being
called and the claim ages out on its own, with NO explicit release call needed on
the common path. `APMFBridge::Tick()`'s expiry sweep no longer uses the flat 500ms
`kExpiry` for this facet: see `FacetExpiry()` below.

**`FacetExpiry()` — round-robin-aware expiry (2026-09-05, `feat/facet-expiry`).**
The heal claim (and the offense cast-select/combat-target/equipment claims) are
refreshed from inside the SAME per-follower `Scheduler::Tick` ROUND-ROBIN lap —
one follower serviced per ~133ms — so a given follower's own gambit only re-fires
every ~0.133s × party size. For anything but a 1-2-follower party that gap already
exceeds a flat 500ms, so the OLD flat backstop released a live, still-wanted claim
every round-robin lap (deck-proven: `feat/heal-claim-hold`'s "claim/release every
~530ms, caster stuck at rest forever", then re-proven the SAME day on the
equipment claim, Cicero deck capture). `FacetExpiry()` (`APMFBridge.cpp`, anon ns)
sizes the window the SAME way `TargetCastReconcile`/`SelfCastReconcile` already
size their own round-robin-aware release windows (`suppress*1.12 + 0.133*partySize
+ 0.5`, floored at the old 500ms) — `Tick()`'s heal/cast/target/equipment handle
checks all compare against `FacetExpiry()` now, not the flat `kExpiry` (which
package-offer and combat-action-deny still use — see `APMFBridge.cpp`'s own
entry in `MAP.md` for why those two are unaffected).

### S1 deck field-test fixes (2026-09-05) — two real MFO-side bugs

**BUG A — HARD-ABORT despite an armed claim.** Deck log: `HARD-ABORTED cast of
0004D3F2 (concentration unbounded)`. `ComposedCast::Try` armed `CastBounds` for
the EXACT gambited spell FormID, but MFO no longer touches the hand itself for
this path (APMF's drive does) — so there is no guarantee the FormID that
actually reaches `CasterConsent`'s hooked thunks (APMF's animated drive, or its
own guaranteed-delivery `CastSpellImmediate` fallback for a CONCENTRATION heal)
is the SAME one `CastBounds` was armed for. Fix: all THREE hard-abort sites in
`CasterConsent.cpp` (`ConcUnboundedDeny`, the `CheckStartCast` thunk's
early-pass, `CheckCastThunk`'s early-pass) now ALSO stand down on
`APMFBridge::IsHealCastActive(fid)` alone — a broad PER-ACTOR trust, exactly
mirroring `IsOwnedCastActive`'s existing per-actor (not per-spell) standdown for
the offense exclusivity deny. A live heal-cast claim is proof MFO already
vetted this cast; the consent hook no longer needs an exact spell-identity
match to stand down for it.

**GENERALIZED (`feat/mfo-claim-only-heal`, 2026-09-05, Task 2 audit).** The three
duplicated OR-expressions BUG A's fix added (`CastBounds::Live(...) ||
APMFBridge::IsHealCastActive(fid)`, hand-copied at all three sites) are now one
helper, `CasterConsent.cpp`'s `ClientCastClaimed(fid, magicItem)` — same two
checks, same behavior, called first at all three sites. Every `DENIED`/
`HARD-ABORT`/veto path in `CasterConsent.cpp` was walked this pass to confirm it
sits downstream of one of the two early-passes that call it (`ConcUnboundedDeny`,
`CtrlUnlatchedDeny`, `ShouldDeny`/the exclusivity deny, the concentration
force-block, the pacing deny, the force-YES path, and `CheckCastThunk`'s
friendly-fire hold) — a live client cast claim silences ALL of them for its
follower, not just the concentration bound. See `MAP.md`'s `CasterConsent.cpp`
entry for the full site-by-site trail. Deliberately does NOT fold in
`IsOwnedCastActive` (offense's own, separate standdown) — heal and offense stay
two distinct claims.

**BUG B — investigated, NOT a code bug (marth, 2026-09-05).** Field observation:
MFO requested +ACT for two different heal spells on the same follower, one
Self-delivery (needs a proxy to heal an ally) and one already target-delivery,
and marth recalled only one being gambited. Audited every dispatch path from
the gambit's own `actionParamForm` (`Board.cpp`) through `CastOn`/
`ConcentrationCast`/`CastAuto`/`CastSelfDirect`/`CastTargetDirect` down to
`ComposedCast::Try` — **none of them ever substitutes a spell; every one
forwards the gambit's own configured FormID unchanged.** The only fallthrough
found is a DECLINE-and-fall-to-the-next-rule when a configured spell is unknown
to the follower (`Logistics.cpp:1188` et al.) — never a substitution, just
evaluation moving to a DIFFERENT rule with its OWN configured spell. The deck
APMF.log independently showed multiple `act.cast_target` rules ("rule 2",
"rule 4") on the SAME follower, so two spells firing means two RULES name them
— a gambit-configuration question for marth to check on the Board, not an MFO
dispatch bug.

A REAL (but separate) gap was found and FIXED then REVERTED the same day: an
initial pass had `ComposedCast::Try` decline the +ACT drive outright for a
Self-delivery-spell-at-non-self-target mismatch, reasoning MFO would need to
build a delivery-flip proxy from its own WORKER context (unsafe -- proxy
creation is main-thread-only in this codebase's discipline) and that declining
was safer than that risk. This was WRONG and a REGRESSION: **no such proxy is
MFO's to build.** APMF's feat/cast-act drive already mints its OWN delivery-
flip proxy synchronously on ITS confirmed-main Engage/Repoint path
(`core/CastExecutor.cpp`'s `proxy::Acquire`, called from `StartHandDrive`),
field-proven in the deck APMF.log (`driving left hand -- spell 0002F3B8
cast-as FF001A7D target 0009BCB0` -- Fast Healing/Self correctly proxied onto
Cicero). Declining silently killed animated heal-other for every Self-delivery
gambit spell, the primary capability this whole workstream exists to deliver.
Reverted: `ComposedCast::Try` forwards a_spell + a_target to `APMFBridge::
ClaimHealCast` UNCHANGED in every case -- MFO does not inspect delivery, does
not proxy, does not substitute; APMF resolves delivery entirely on its side
(mismatch → its proxy, match → the raw spell).

**Flagged then, RESOLVED by feat/mfo-cast-port, then FURTHER PORTED by
feat/offense-cast-seats (2026-09-05):** offense's own "owned cast" gambit
(`Actuation.cpp`'s `ownedCast` branch, `APMFBridge::ClaimCasting` at the time)
used to ride the SAME `kIntent_SelectSpell` channel the heal claim rode under
the (since-retired) +ACT drive, raising a real cross-talk risk if a live
feat/cast-act APMF build started driving both. APMF's `feat/ai-cast-seats-impl`
mooted the concern from both directions: the heal claim moved OFF
`kIntent_SelectSpell` entirely, onto its own `kIntent_Cast` (ch.8b) facet, and
the +ACT drive that would have made `kIntent_SelectSpell` do anything beyond
gate-only is retired. At that point offense's `ClaimCasting` stayed on the
now-inert `kIntent_SelectSpell` gate-only channel — safe, but still exposed the
long-standing "target right, spell wrong" defect (offense's own AI still
picked its own spell; the channel only arbitrates/denies). **feat/offense-
cast-seats ported offense fully onto `kIntent_Cast` too** (`ClaimOffenseCast`,
its own DISTINCT claim slot — see the OFFENSE CAST PORT section below), closing
that defect the same way `ClaimHealCast` already did for heals. Heal and
offense are two structurally separate facet CLAIMS sharing one underlying
`kIntent_Cast`/`RequestCast` mechanism — never merged, never cross-talking
(`IsHealCastActive` vs `IsOwnedCastActive`, distinct `Owned` struct fields).

### Task 1/3 audit (`feat/mfo-claim-only-heal`, 2026-09-05) — already claim-only

A later pass was briefed to "make the heal path claim-only": MFO issues an APMF
claim naming the gambit's spell/target and releases it when the gambit no longer
wants the heal, calling no engine cast function, equipping nothing, driving no
caster, minting no proxy. Line-by-line audit of `ComposedCast::Try`/`End` (this
section, above) found that description ALREADY matches this file's PASS E shape
exactly — `Try` calls only `ClaimHealCast` + `CastBounds::Arm`, `End` calls only
`ReleaseHealCast` + `CastBounds::Disarm`, and release is the existing
"stop refreshing → expire" idiom described above. **No functional change was
needed or made** for Task 1; the hand policy (`kApmfHandLeft`) and the
`bHealAnimPackage` opt-in gate were likewise already exactly as specified.

### Native-AI seat-answering — SHIPPED (feat/mfo-cast-port, 2026-09-05)

This section used to be a "FORWARD LOOK" flagging a DISASSEMBLY-BACKED research
conclusion as not yet built anywhere in the APMF tree. **It is built now — this
is the CURRENT design**, described in full in the CFC section above; this entry
just closes the loop on the research note.

APMF's `feat/ai-cast-seats-impl` answers five vfunc seats on the follower's own
combat caster object while a `kIntent_Cast` claim stands — `CheckShouldEquip`
(0x0F), `CheckStartCast` (0x06), `CombatMagicCaster::GetMagicTarget` (0x0A),
`CheckStopCast` (0x07),

> **VFUNC-INDEX WARNING (2026-09-07): `0x0A` names TWO DIFFERENT vfuncs on TWO
> DIFFERENT vtables in this document, and both usages are correct.**
> `CombatMagicCaster::GetMagicTarget` is slot `0x0A` on the combat-caster
> vtables (APMF's seats). `MagicCaster::CheckCast` is slot `0x0A` on
> `ActorMagicCaster` (MFO's `CasterConsent` `CheckCastThunk`). Always
> class-qualify a vfunc index; an unqualified `0x0A` sends the next reader to
> the wrong vtable.
`SetupAimController` (0x0D) — so the FOLLOWER'S OWN AI equips, aims, charges, and
fires the heal at the claimed target, with ZERO engine-cast call from either mod.
`APMFBridge::ClaimHealCast` (`native/APMFBridge.cpp`) now calls `RequestCast`
directly (see the CFC section's code block above for the exact payload); the
Intent's own doc comment in `APMF_API.h` was updated on APMF's side to match (no
longer "APMF fires NO cast: the CLIENT executes its own animated cast" — that was
the PASS D contract this design replaces) and MFO's copy was re-synced
byte-identical. `ComposedCast`'s overall shape (a thin claim-plus-CastBounds shim)
needed no change beyond the `ClaimHealCast` payload itself and threading the new
`stopPct` parameter through — exactly as this note predicted it would.

### feat/mfo-cast-port audit (Tasks 4-6, 2026-09-05)

**Task 4 — `IsHealCastActive` semantics preserved.** The port changes WHAT
`APMFBridge::ClaimHealCast` sends APMF (a `kIntent_Cast` `RequestCast` payload
instead of a `kIntent_SelectSpell` `RequestEx`/`Repoint` param) but not the
claim-lifecycle shape `IsHealCastActive` reports on: it is still "does
`g_owned[fid].healHandle` hold a live handle," true from a successful
`ClaimHealCast` until `ReleaseHealCast`/`Tick()`'s expiry sweep. `CasterConsent.
cpp`'s `ClientCastClaimed` (still ORing `CastBounds::Live` with
`IsHealCastActive`) and every hard-abort/deny/veto path downstream of it needed
no changes.

**Task 5 — 0x06 install-order, verified safe.** APMF's `feat/ai-cast-seats-impl`
now ALSO answers 0x06 `CheckStartCast` on `VTABLE_CombatMagicCasterRestore`, the
same vtable slot MFO hooks globally (`CasterConsent::InstallHook`, 14 caster
vtables). SKSE's alphabetical plugin load order ("APMF.dll" before "MFO.dll")
means APMF installs first, so MFO's thunk ends up OUTER (last write to the
vtable slot) — the dangerous-sounding order, since an outer thunk could in
principle veto the inner seat's forced YES. Read both hooked thunks
(`CasterConsent.cpp`'s 0x06 `thunk` and the `ActorMagicCaster::CheckCast` 0x0A
`CheckCastThunk`) line by line:
both call their captured `original()` FIRST (so the local `aiSaysYes`/`aiOK`
already reflects whatever APMF's inner chain decided), THEN hit the
`ClientCastClaimed` early-pass BEFORE any deny/veto logic (the latch check,
`ConcUnboundedDeny`, `CtrlUnlatchedDeny`, the exclusivity deny, the
concentration-never-force-yes deny, the pacing deny, and 0x0A's friendly-fire
deny all sit AFTER it), and return the untouched value when a claim is live.
Neither thunk can act on a deny verdict it never computes — so the ordering is
safe regardless of which way SKSE actually resolves it at runtime. See the
`ClientCastClaimed` doc comment in `CasterConsent.cpp` for the full write-up
(kept there, not just here, since that is the file a future edit to either
thunk needs to re-read first).

**Task 6 — no delivery watchdog; a diagnostic instead.** APMF deliberately
removed its own `CastSpellImmediate` guaranteed-delivery fallback with this
port (APMF's INVARIANTS.md #0: never manufacture the effect) — so if a claim stands
but the AI's seats never actually fire it, the heal must visibly NOT happen;
building a fallback timer here would be exactly the band-aid marth ruled out
for the S1 test profile. Instead, `ComposedCast.cpp` revives the `ExpectingCast`/
`NoteObservedCast` seam (previously a permanent no-op after APMF's own +ACT
drive took over observation) as a real, worker-serial, LOG-ONLY watch: every
`Try()` success arms/refreshes a per-follower (spell, since, observed) record;
`Diagnostics.cpp`'s `TESSpellCastEvent` sink (which already calls
`ExpectingCast`/`NoteObservedCast` for every tracked follower's own-AI cast)
marks a claim observed the moment the exact claimed spell actually fires. A
claim that stays un-observed for 2s+ logs a `[cfc]` warning (rate-limited to
once per 5s) naming the follower/spell so the deck log shows "claim stands, no
cast landed" as a legible signal — it never re-claims, re-fires, or falls back.

### OFFENSE CAST PORT (feat/offense-cast-seats, 2026-09-05) — ch.8 → ch.8b

Ports offense's own "owned cast" gambit (`Actuation::CastOn`'s `ownedCast` branch)
off the retired ch.8 `kIntent_SelectSpell` gate-only claim onto the SAME `kIntent_
Cast`/`RequestCast` mechanism `ClaimHealCast` uses — closing the long-standing
"target right, spell wrong" defect ([[cast-gambit-spell-choice-not-enforced]]):
`kIntent_SelectSpell` only ever ARBITRATEd + DENIED a competing framework's own
spell selection, so the follower's own AI still picked whichever spell IT wanted.
`kIntent_Cast`'s five engine seats instead DRIVE the AI's OWN cast decision
directly (the same seats already proven for heals), so the gambit's named spell
is now the one that actually fires.

**New/changed symbols (`native/APMFBridge.h`/`.cpp`):**
- `ClaimOffenseCast(follower, spell, target, hand, concentration, stopPct)` —
  replaces `ClaimCasting` (retired, along with `ReleaseCasting`). Same
  create-or-refresh/RequestCast shape as `ClaimHealCast`, sharing its
  implementation via the renamed `EnsureCastClaimLocked` helper (was
  `EnsureHealClaimLocked` — the function was already fully generic over its
  handle/out-params, only the name was heal-specific).
- `ReleaseOffenseCast(follower)` — replaces `ReleaseCasting`.
- `IsOwnedCastActive(follower)` — SAME name, REPOINTED: now backed by
  `Owned::offenseHandle` (a `kIntent_Cast` handle) instead of the retired
  `Owned::spellHandle` (a `kIntent_SelectSpell` handle). Every existing call
  site (`CasterConsent.cpp`'s exclusivity/hard-abort standdowns, `CombatStyle.
  cpp`'s equip-gate standdown) is unchanged and remains correct, since a live
  `kIntent_Cast` claim ALSO answers the 0x0F `CheckShouldEquip` seat — covering
  exactly what those standdowns relied on ch.8 for.
- `Owned::offenseHandle`/`offenseSpell`/`offenseTarget`/`offenseHand`/
  `offenseConc`/`offenseStopPct`/`offenseRefreshed` — a DISTINCT claim slot
  from `Owned::healHandle`'s heal-cast fields (Task 3's "heal and offense stay
  two distinct claims" invariant, preserved). Uses the SAME round-robin-aware
  `FacetExpiry()` backstop the heal/combat-target/equipment claims already use.

**HAND POLICY — SUPERSEDED (integration/2026-09-06, then feat/per-hand-
cast-slots, 2026-09-06).** `ClaimOffenseCast` no longer always passes LEFT: it
now consults `Loadout::PlanCastHand` (via `Actuation.cpp`'s per-hand
cast-gambit lock, `ResolveCastHand` — see the "PER-HAND CAST SLOTS" section
below for the full mechanism), which can resolve Left, Right, or DualCast
depending on the follower's real weapon-hand state and dual-casting
eligibility. `Loadout::Prepare`'s `EquipSpell` call itself is UNCHANGED by
either pass and still always targets `LeftHandSlot()` unconditionally — see
the "PER-HAND CAST SLOTS" section's own flagged open item for why that is not
yet verified safe for a Right/DualCast claim. This paragraph's original
reasoning (kept below for history) was correct for its own time, when
`ClaimOffenseCast` genuinely never requested anything but LEFT:

*Original text: "this is not a new conditional rule, it matches PHYSICAL
REALITY: `Loadout::Prepare`'s `EquipSpell` has ALWAYS targeted `LeftHandSlot()`
unconditionally, whether or not a weapon is held in the right hand. Since the
claimed hand and the physically-equipped hand must never disagree (the
deck-proven heal-path failure this exact rule already fixed once), LEFT-always
is simply the correct value here too; there was no scenario where 'no weapon
held → free/auto' would differ from LEFT in practice, so no new weapon-state
branching was added."*

**CONCENTRATION / STOP-PERCENT.** `ClaimOffenseCast`'s sole call site
(`CastOn`'s `ownedCast` branch) never reaches it for a concentration spell —
`CastOn` forks concentration off EARLIER, to `ConcentrationCast` →
`CastTargetDirect`/`CastSelfDirect`'s direct-force stream (a deliberately
DIFFERENT, pre-existing delivery model, untouched by this pass — see "DIRECT
FORCE is THE delivery" above). So this call site always passes
`concentration=false`. The parameter and its `kCastFlag_Concentration` wiring
exist end-to-end (shared with `ClaimHealCast` via `EnsureCastClaimLocked`) for
a FUTURE AI-driven concentration-offense call site, not invented here.
`stopPct` is always `0` — it is a HEAL-ONLY concept (a client restore
threshold read by seat 0x07 `CheckStopCast`); no offense gambit has an
analogous threshold in scope.

**UPDATE (feat/cast-gambit-concentration, 2026-09-06): that future call site now
exists — see the next section.** `ClaimOffenseCast`'s `concentration`/`stopPct`
parameters are no longer dead wiring for offense; `CastTargetDirect`/
`CastSelfDirect` call it directly for a non-heal concentration stream. `CastOn`'s
`ownedCast` branch itself is UNCHANGED (a concentration spell still forks off
before reaching it, so it still always passes `concentration=false`).

**DEGRADE PATH — REWRITTEN 2026-09-07 (`fix/mfo-no-decline-fallback`).** This
block used to say a refused claim "falls through to the SAME AI-first-grace +
force-on-miss legacy hybrid … byte-identical to the pre-port behaviour". It does
not, and it must not: that was the decline-fallback the showpiece principle
forbids. `ClaimOffenseCast` returning `false` now splits into two halves:

- **DEGRADE-WHEN-ABSENT** — the channel is not there at all
  (`APMFBridge::OffenseCastClaimSupported()` false: APMF absent, `bApmfCast` off,
  ABI < 5), or `bLegacyCastHybrid` is ON → the ORIGINAL AI-first-grace +
  force-on-miss hybrid, byte-identical to pre-APMF. This is the ONLY surviving
  legacy route and it is a documented contract, not a mask.
- **FAIL CLOSED** — APMF is present AND capable AND it REFUSED →
  `Actuation.cpp:1039-1052` returns
  `{FailedSkill, "APMF refused the cast claim", transparent}` with a rate-limited
  `[apmf] … APMF REFUSED …` line (`LogApmfRefusal`). The cast does not happen and
  nothing routes around APMF. `transparent` means the rules below still run — the
  follower is not frozen out of its whole table, it visibly does not cast.

So a refusal no longer "never drops the cast": it drops the cast DELIBERATELY and
loudly, which is the point (CLAUDE.md principle 7).

**`CasterConsent.cpp`'s `ClientCastClaimed` standdown, extended.** The
generalized early-pass (`ConcUnboundedDeny`, the `CheckStartCast` thunk's
early-pass, `CheckCastThunk`'s early-pass all call it first) now ORs in
`APMFBridge::IsOwnedCastActive(fid)` alongside `CastBounds::Live` and
`IsHealCastActive` — the SAME HARD-ABORT protection heal's port needed
(2026-09-05 S1 field HARD-ABORT) now covers offense's kIntent_Cast claim too:
once APMF's seats drive the AI's own cast decision for an offense claim, MFO's
globally-installed consent hook (which also intercepts APMF's seat-answered
CheckStartCast/CheckCast on the same Restore caster vtable) must stand down for
it exactly as it does for heal, or it could hard-abort a cast MFO itself
claimed. The two now-redundant standalone `IsOwnedCastActive` checks
(`CasterConsent.cpp`'s exclusivity-deny standdown and `CheckCastThunk`'s
exclusivityDeny guard) are UNREACHABLE in practice once the early-pass already
returns — left in place as defense-in-depth, not removed, per their own updated
comments.

**Reused, not duplicated:** the `[cfc]`-style silent-claim diagnostic
(`ComposedCast.cpp`'s `g_watch`/`WatchArmed`, previously private to `Try()`) is
now exposed as `ComposedCast::WatchClaim`/`ClearWatch` — generic over WHICH
claim armed it (heal via `Try()`, or offense via `Actuation::CastOn` calling
`WatchClaim` directly, since offense does not go through `Try()` — `Try()`
stays HEAL-ONLY-gated, unchanged). `Actuation::CastOn` arms it on a successful
`ClaimOffenseCast`; `Scheduler.cpp`'s `!castSeen` release and
`Followers::OnFollowerRemoved`'s dismissal teardown both clear it alongside
`ReleaseOffenseCast`. No watchdog, no re-fire, no fallback — a claim that
stands with no observed cast only logs (principle #7).

### CONCENTRATION CLAIM PORT + THE FIRING-SPELL GAMBIT LOCK (feat/cast-gambit-concentration, 2026-09-06)

Two independent fixes, same branch.

**1 — a non-heal CONCENTRATION stream now reaches the engine-seat path too.**
Before this pass, `CastOn` forked EVERY concentration spell (self or target,
any `SpellKind`) to `ConcentrationCast` → `CastTargetDirect`/`CastSelfDirect`,
which already tried `ComposedCast::Try` for the engine-seat claim — but `Try()`
is HEAL-ONLY gated (its whole point, per the offense-cast-seats port above: not
widened). So a HEAL concentration stream (Healing Hands, a channelled heal
gambit) already got the real AI-driven, animated channel; an OFFENSE or BUFF
concentration stream (a channelled damage/drain, a channelled ward) always fell
straight to the kInstant direct-force stream — never AI-fired, never animated,
exactly the gap the "CONCENTRATION / STOP-PERCENT" note above called out as
"a FUTURE call site, not invented here."

That call site is now `CastSelfDirect`/`CastTargetDirect` themselves
(`Actuation_Direct.cpp`), immediately after their existing `ComposedCast::Try`
call: when `Try()` declines (which it always does for `kind != Heal`) AND the
spell is `kConcentration` AND `kind != Heal`, they call `APMFBridge::
ClaimOffenseCast` DIRECTLY with `concentration=true, stopPct=0` — the SAME
`kIntent_Cast` facet the offense-cast port already wired concentration/stopPct
parameters into but never called with `concentration=true`. `ComposedCast.*`
itself is UNTOUCHED (owned by a parallel change; its `Try()` gate stays
HEAL-ONLY by design) — this is a sibling call, not a widened one, mirroring
`ComposedCast::Try`'s own sequence (claim, then `ComposedCast::WatchClaim` for
the shared `[cfc]` silent-claim diagnostic, exposed for exactly this reuse) but
without touching that module. `target=0` for self (matches `ClaimHealCast`'s
convention); `hand=kApmfHandLeft` (matches every other claim on this path).

**CORRECTED 2026-09-07 — the "Degrade, preserved exactly" block below used to include "or a
refused claim (lost arbitration)" among the conditions that fall through, "byte-identical". A LOST
ARBITRATION DOES NOT FALL THROUGH.** It was the eighth site of that error and it contradicted
`MAP.md`, which already had it right. The concentration twin splits three ways, like the
fire-and-forget one:

- **FAIL CLOSED** — APMF present AND capable (`APMFBridge::OffenseCastClaimSupported()`) AND it
  REFUSED → `LogApmfRefusal(..., "concentration offense-cast (self)"` / `"concentration
  offense-cast", ...)` then **`return SelfCast::Declined`** (`Actuation_Direct.cpp:935-938` and
  `:1226-1229`). The stream does not start and nothing routes around APMF.
- **DEGRADE-WHEN-ABSENT** — the conditions in the original block below, minus the refusal
  (`Actuation_Direct.cpp:940-942` and `:1230-1232`).

**A concentration cast never SILENTLY vanishes** — on a refusal "silently" is the whole load-bearing
word: it DOES vanish, deliberately, with an `[apmf] … APMF REFUSED …` line naming
actor/spell/target/hand. Absence degrades; refusal is loud.

**Degrade-when-absent, preserved exactly.** APMF absent, ABI < 5 (no `RequestCast` slot),
`bApmfCast` off, or `bLegacyCastHybrid` on — i.e.
`APMFBridge::OffenseCastClaimSupported()` is false — make `ClaimOffenseCast`
return `false` and both call sites fall straight through to the SAME
direct-force stream code that already ran before this pass, byte-identical
(`Actuation_Direct.cpp:940-942` and `:1230-1232`). **A REFUSED claim is NOT in
that list** — a lost arbitration fails closed instead (`:935-938` and
`:1226-1229`; see the correction above). **A concentration cast never SILENTLY
vanishes:** on the degrade path it still runs, and on a refusal it vanishes
LOUDLY — an `[apmf] … APMF REFUSED …` line names actor/spell/target/hand. The
existing `[cfc] claim live N ms with NO observed cast` diagnostic
(`ComposedCast.cpp`) keeps working unchanged for this path either way (it is
claim-generic, not heal-specific).

> **⚠ THE `[cfc]` WATCHDOG IS MIS-SIZED — DO NOT READ ITS OFFENSE WARNINGS AS
> FAILURES (RC7, 2026-09-06).** `kSilentWarnAfter` is **2000 ms**
> (`native/ComposedCast.cpp:95`) against a measured engine equip+charge latency
> of **~2.5 s**, so on the offense path it warns about casts that are simply
> still charging: every offense `[cfc]` warning in the 2026-09-06 session was at
> 2.0-2.4 s and every one was a FALSE ALARM. It also reports **watch age**, not
> claim age — a watch spans many separate 6 s claims and any no-claim lull
> between them, so a 7.7 s / 67 s figure is not "one claim held that long".
> The Healing Hands warnings in that session (7.7-67 s) were nonetheless REAL:
> they are RC1 (heal-slot thrash). Re-size the window before trusting it, and
> until then read `[cfc]` as "look here", never as "this failed".

**2 — a firing spell gambit now LOCKS until it completes.** *(SCOPE, added
2026-09-07: this lock covers the OFFENSE path only — `CastOn`/`ConcentrationCast`
via `Actuation.cpp`'s `g_castLock`. **HEALS WERE NOT COVERED**, and RC1 of
`Docs/DIAG-2026-09-06-deny-heal-failures.md` is exactly that hole: two heal rules
thrash MFO's single heal slot every 1-3 s, each swap releasing the claim before
the engine's ~2.5 s equip+charge finishes — 1 heal landed in ~15 attempts.
**The heal-side lock (F1) has since LANDED on `main`** (`fix/mfo-heal-slot-and-proxy`,
merged 2026-09-07): `ComposedCast::Try` now reports an incumbent heal as
`TryResult::Held` instead of silently thrashing it, and `HeldOffBy`
(`ComposedCast.cpp:499`) names the holder in the log. The hole described here is
CLOSED in code and awaits its field cycle.)*
marth: "while a
spell gambit is actively firing, another spell gambit must not preempt or
re-point it, even if it would otherwise win the rule evaluation." Before this
pass, every APMF claim + concentration stream here uses a "call every tick the
gambit wins" idiom with NO memory of what was previously firing — so a
round-robin condition flicker (a foe's HP crossing a threshold, an ally
becoming the new "most hurt") could hand `CastOn`/`ConcentrationCast` a
DIFFERENT `(spell,target)` mid-charge/mid-channel, which the existing
create-or-refresh claim logic treats as "a CHANGE" and tears down + re-requests
— wasting an in-flight multi-second charge or channel.

`Actuation.cpp` now keeps one `CastLock{spell,target,lastSeen}` slot per
follower (worker-serial, `g_castLock`/`g_lastLockLog`, anon-namespace,
file-local — no cross-TU exposure needed). `CastOn` checks it (`CheckCastLock`)
right after the range/competence gates and before the self-cast fork,
concentration fork, or owned-cast claim: a request for the SAME `(spell,target)`
already locked proceeds normally (and refreshes the lock itself, `HoldCastLock`,
at each of CastOn's self-cast-fork / owned-cast-claim / `ConcentrationCast`'s
own Applied/Refreshed return points); a request for a DIFFERENT `(spell,target)`
is held off — a transparent (GAMBIT_FLOWS §2) `NoOp` — while the lock is still
LIVE, logged once per (follower,spell) per ~2s at `[eval]`.

**Liveness, not a flat timer (marth's "must never become an unbounded hold").**
`CastLockLive` checks `APMFBridge::IsOwnedCastActive`/`IsHealCastActive` FIRST —
a live engine-seat claim (offense or heal) is authoritative proof the follower
is still actively driven, so the lock tracks a claim's own bounded TTL/release
exactly, never outliving it. Only when NEITHER claim is live (APMF absent/off,
or the plain un-claimed direct-force concentration stream, whose registry is
private to `Actuation_Direct.cpp`'s own TU and unqueryable from `Actuation.cpp`)
does it fall back to a staleness window — `APMFBridge::FacetExpiry()`, REUSED
verbatim (not a new invented budget, #9) since a still-winning gambit re-Holds
the lock every round-robin lap, well inside that same window.

**Release, every exit path:**
- **Completion / claim release / TTL expiry** — `CastLockLive` returns false
  the instant neither claim is live and the staleness window has elapsed;
  `CheckCastLock` erases the stale entry on the very next differing request.
- **The gambit stops winning entirely** — `Scheduler.cpp`'s `!castSeen` release
  (no cast rule's condition held this tick at all) now also calls
  `Actuation::ClearCastLock(id)`, alongside `ReleaseOffenseCast`/
  `ComposedCast::ClearWatch`.
- **Combat ends** — `Scheduler.cpp`'s out-of-combat teardown block now also
  calls `Actuation::ClearCastLock(id)`, alongside `CasterConsent::Clear`.
- **Follower dismissed** — `Followers::ReleaseHeldState` now also calls
  `Actuation::ClearCastLock(id)`, alongside `ReleaseHealCast`/
  `ReleaseOffenseCast`.
- **Revert/load** — `Actuation::ClearCastLocks()` (bulk, all followers) is
  called from `Actuation::ClearSelfCasts()`, the existing `Serialization.cpp`
  revert call site — no new call site added there.

**Scope, deliberate.** The legacy AI-first-grace + force-on-miss hybrid is NOT
covered (it already self-protects via its own grace window + the `aiCastOther`
miss detector, and is the degrade-when-absent path, not the primary one).
`CastAuto`'s sequential-most-hurt heal fan is NOT covered (its own hysteresis
is a deliberate target-cycling design, not the re-pointing bug this lock
exists for — see [[cast-fanning-known-good-behavior]]). The out-of-combat
Logistics dispatch never calls `CastOn`/`ConcentrationCast` at all (single-pass,
no suppression-window rule scan to re-point from), so it needs no gate.

### PER-HAND CAST SLOTS (feat/per-hand-cast-slots, 2026-09-06)

**THE FIELD PROBLEM.** Deck-confirmed: casts fired less often than they should,
and hands were used unwisely when two spells were valid within a few ticks.
36 cast rules fired, only 22 claims made — ~14 wanted casts suppressed. Log:

```
[eval] 750012C6 cast gambit HELD OFF -- spell 0002F3B8 wants the follower, spell 0002DD29 is still firing
[eval] 750012C6 cast gambit HELD OFF -- spell 0002DD29 wants the follower, spell 0002F3B8 is still firing
```

**Two causes, both closed by this pass.** (1) The firing-gambit lock above
(Task 2) was PER-FOLLOWER: a spell firing in one hand held off a DIFFERENT
spell that could have used the OTHER hand. Mechanically, this alternation
happens WITHIN one Scheduler tick too — the evaluator scan falls through a
transparent HELD-OFF outcome (GAMBIT_FLOWS §2) to the next rule, so a heal
rule and an offense rule can both be evaluated (and one of them held off) in
the SAME tick, not just across ticks. (2) `APMFBridge` held ONE offense
kIntent_Cast claim slot and one heal slot per follower — even with a per-hand
lock, there was nowhere to put a second concurrent offense claim.
`Loadout::PlanCastHand`'s `EitherFree` "juggle" plan (two different spells,
one per hand) was unexpressible on the claim side.

**THE COORDINATED CHANGE.** A parallel APMF change makes `kIntent_Cast`
arbitrate PER (actor, hand): two concurrent claims — LEFT (`kCastFlag_LeftHand`
set) and RIGHT (unset) — can now both stand on one actor, and
`kCastFlag_DualCast` claims both and is mutually exclusive with either single-
hand claim. **No API change** — MFO just calls `RequestCast` twice and holds
two handles. This section covers MFO's side of that contract.

**1 — the firing-gambit lock is now PER-HAND.** `Actuation.cpp`'s `g_castLock`
is `unordered_map<FormID, FollowerCastLocks{CastLock hand[2]}>` (index 0=left,
1=right) instead of one `CastLock` per follower. `CheckCastLock` is retired,
replaced by `ResolveCastHand(follower, Loadout::HandPick, spell, target,
HandPlan&)` — the SAME gate (checked at the SAME point, right after range/
competence, before self-cast/concentration/owned-cast/composed-cast/legacy-
hybrid all run), now hand-aware:
- **Self-target, or ANY concentration spell** (self or on-target —
  `ConcentrationCast`/`CastSelfDirect`/`CastTargetDirect` all claim LEFT
  unconditionally, heal or offense) — forced `HandPick::Left`.
- **A Heal/Buff spell** reaching the composed-cast branch (or falling through
  to the legacy hybrid) — forced `HandPick::Left` (the heal-claim hard rule).
- **An Offense spell at a real (non-self) target** — the only real decision:
  `Loadout::PlanCastHand`'s result is passed straight through.

`ResolveCastHand` implements THE JUGGLE per `HandPick`, checking `HandFree`
(unlocked, already locked to this EXACT (spell,target) — a refresh — or gone
live-false/stale, dropped in place):
- `Left` — checks hand 0 only; busy → held off naming the left hand.
- `EitherFree` — tries hand 0, then hand 1; busy on both → held off naming
  both hands' occupants.
- `DualCast` — requires BOTH hands free-or-refreshing-the-same-spell at once;
  ANY contention holds the WHOLE request off (never a half-landed dual-cast —
  CLAUDE.md principle 7, no fallback that makes a degrade look like the real
  thing).

`CastOn` resolves `handPlan` ONCE right after the gate and reuses it via a
`lockHands` lambda at every `HoldCastLock` call site in the function (self-cast
fork, owned-cast claim, composed-cast claim), so the lock and the actual APMF
claim never disagree about which hand(s) they occupy. `ConcentrationCast` (a
separate function, unchanged call sites) hardcodes hand 0 directly, matching
its own always-LEFT delivery. `CastLockLive(follower, hand, lock)` now checks
`APMFBridge::IsOwnedCastActiveOnHand(follower, hand)` (NEW) for that ONE
slot — ORed with `IsHealCastActive` for hand 0 only (heals are always LEFT) —
falling back to `APMFBridge::FacetExpiry()` exactly as before otherwise. Every
release path (`!castSeen`, combat-end, dismissal, revert) still calls
`ClearCastLock`/`ClearCastLocks`, which drop BOTH hands for that follower (all
four mean "nothing wanted on this follower anymore" — a genuinely mid-flight
single-hand release still relies on the create-or-refresh/expiry idiom, same
as before this pass).

**2 — `APMFBridge` gained a per-hand offense claim slot.** `Owned::offense` is
now `CastClaim offense[2]` (was one flat `offenseHandle`/`offenseSpell`/…
field set) — `CastClaim` bundles a claim's handle, spell, target, hand,
concentration flag, stop-percent, and its own refresh timestamp, one instance
per hand. `Owned::heal` is the SAME `CastClaim` type but stays a single
instance (heals are LEFT always, never per-hand). `ClaimOffenseCast(a_hand)`
routes to `offense[0]` for `kApmfHandLeft`, `offense[1]` for anything else
(the NEW `kApmfHandRight` constant, or bare 0 — matching APMF's own "no
LeftHand flag = right hint" default), or claims BOTH slots for
`kApmfHandDualCast` — ONE underlying `RequestCast` call (never two for a
single dual ask), its handle/metadata mirrored into both slots so a
hand-availability query on either index sees it occupied. A single-hand ask
that lands on a slot currently mirroring a stale dual claim un-mirrors the
OTHER slot FIRST, so the shared handle is never double-released.
`ReleaseOffenseCast(follower)` (unchanged signature — its two callers,
`Scheduler`'s `!castSeen` and `Followers`' dismissal, both mean "nothing
wanted on either hand anymore") releases both slots, dedupe-aware for a
mirrored dual claim; `Tick()`'s expiry sweep is likewise dedupe-aware per
slot. `IsOwnedCastActive(follower)` keeps its existing "is ANY hand's claim
live" per-actor contract UNCHANGED (its two consumers, `CasterConsent`'s
exclusivity/hard-abort standdowns and `CombatStyle`'s equip-gate standdown,
never cared which hand); the NEW `IsOwnedCastActiveOnHand(follower, hand)`
answers ONE slot, consumed by the per-hand cast-gambit lock above.

**3 — the `[cfc]` silent-claim diagnostic is per-hand too.** `ComposedCast`'s
`g_watch` is now `unordered_map<FormID, FollowerWatch{Watch hand[2]}>` (was
one `Watch` per follower) — a heal claim (hand 0) and a concurrent offense
claim (hand 0 or 1) no longer overwrite each other's watch. `WatchClaim`
gained an `a_hand` parameter (default `kApmfHandLeft`, so every existing
heal-adjacent call site — the two concentration-offense claims in
`Actuation_Direct.cpp`, both hardcoded LEFT — needed no edit); `CastOn`'s
owned-cast branch calls it once per hand `handPlan` actually granted (both,
for a DualCast plan). `ExpectingCast`/`NoteObservedCast` keep their existing
`(follower, spell)` signature (`Diagnostics.cpp`'s sink has no hand to report)
but now check/mark BOTH hand slots internally. `ComposedCast::End()` (and a
refused heal claim inside `Try()`) clear ONLY hand 0's watch slot, so a
concurrent right-hand offense watch survives a heal ending; `ClearWatch`
(no hand param) still clears both, reserved for the same two "nothing wanted
at all" callers as `ReleaseOffenseCast`.

**4 — THE HARD RULE IS UNCHANGED AND STILL ABSOLUTE.** `Loadout::PlanCastHand`
checks `a_weaponHandActive` FIRST, unconditionally, before Dual/EitherFree are
even considered — a weapon owning the right hand always forces `HandPick::
Left`, no matter what. `ResolveCastHand` therefore only ever sees `Right` or
`DualCast` as CANDIDATES when `PlanCastHand` has ALREADY established the
follower's right hand is genuinely free; there is no path in this pass's code
that requests `kApmfHandRight`/`kApmfHandDualCast` from `APMFBridge` while a
weapon is active, because `PlanCastHand`'s own gate runs first and produces
`Left` in that case. The per-hand claim/lock machinery is a pure ROUTING
layer on top of that decision — it does not re-derive or second-guess it.

**Open item, flagged not fixed by this pass.** `Loadout::Prepare`'s own
`EquipSpell` call (`Actuation.cpp`, in `CastOn`'s equip step, unchanged by this
pass) still ALWAYS equips into `LeftHandSlot()` unconditionally, regardless of
`handPlan`. Before this pass that was provably correct (the offense claim was
ALSO always LEFT, so the claimed hand and the physically-equipped hand could
never disagree — see the OFFENSE CAST PORT section's HAND POLICY above). Now
that a claim can resolve to Right or DualCast, this pass does NOT verify (nor
change) what physically happens on the follower's right hand when APMF's
seats try to arm it — whether the AI's own equip-scoring independently arms a
spell there once the seat hint permits it (per [[ai-scoring-controls-equip-
too]]), or whether a Right/DualCast claim currently has nothing to actually
cast from. This needs a field observation (CLAUDE.md principle 5:
disassembly/design proves a path exists, not that it runs) before it is
trusted; flagged here rather than silently assumed correct.

### CastBounds — the HARD-ABORT fix (§2)

`CasterConsent::ConcUnboundedDeny` hard-aborts a tracked follower's own
concentration cast at Exact unless MFO can prove the stream is one MFO itself is
metering. Before this fix the only proof was the legacy alias-package stream
(`Packages::StreamLive`, written only by `Packages::Begin`). Every other
MFO-arranged cast, the `ConcProxy` direct force and now `ComposedCast`, passed
through the same `ActorMagicCaster::CheckCast` (0x0A) thunk but was never registered as MFO's own.
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
`CheckCastThunk` (`ActorMagicCaster::CheckCast` 0x0A). A registered cast early-passes every one of them, the
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
The cast APMF drives declaratively on `ComposedCast`'s behalf (via `core/
CastExecutor.cpp`) is the one MFO-arranged cast that DOES deliberate through the
real state machine (that is the whole point — a real, animated cast), so it is the
only path that needs the bound, regardless of which DLL's code is actually turning
the crank on the hand caster. `ComposedCast::Try` being the sole `CastBounds::Arm`
caller is by design.

**Task 3 verdict (`feat/mfo-claim-only-heal`, 2026-09-05): KEEP, redundancy
noted, nothing deleted.** `CasterConsent`'s `ClientCastClaimed` helper ORs
`CastBounds::Live` with `APMFBridge::IsHealCastActive`, and for the heal path
specifically the latter is a strict superset of the former today (`ComposedCast::
Try` only ever `Arm`s `CastBounds` in the same call a heal claim succeeds, and
only ever `Disarm`s it alongside releasing that claim) — so `CastBounds::Live`
currently never fires independently of `IsHealCastActive` for this consumer.
Kept anyway: it is the general, cheap, already-proven "MFO-executed-cast bound"
primitive documented above as outliving any one caller, and it is the fix for a
real field HARD-ABORT crash — pruning a crash-class safety net to remove
~190 lines of code that costs nothing to keep is the wrong trade. No code
changed here this pass.

### Path comparison

| path | trigger | animated | target reach | status |
|---|---|---|---|---|
| kInstant force-apply | `CastSpellImmediate` | no | any actor | baseline, always on |
| APMF owned cast | `APMFBridge::ClaimOffenseCast` (`kIntent_Cast`/`RequestCast`, ch.8b — ported feat/offense-cast-seats off ch.8) drives the follower's OWN AI to cast the EXACT gambit spell | yes | hostile foe only | default when APMF is present. A refused claim **FAILS CLOSED** (`{FailedSkill, "APMF refused the cast claim", transparent}`, `Actuation.cpp:1039-1052`, in the PRE-FLIGHT before any hand is touched) — only the channel being ABSENT/`bLegacyCastHybrid` degrades to the AI-first-grace + force-on-miss hybrid |
| Composed Forced Cast (CFC) | `ComposedCast::Try` → `APMFBridge::ClaimHealCast` (`kIntent_Cast`/`RequestCast`, ch.8b) | the follower's OWN AI, via APMF's five engine seats — real native animated cast, ZERO engine-cast call from either mod | any actor (explicit target rides the claim, LOAD-BEARING at seats 0x0A/0x0D) | opt-in (`bHealAnimPackage`), HEAL-ONLY. A refused claim **FAILS CLOSED** — `TryResult::ApmfRefused` (`ComposedCast.cpp:441`) and every caller drops the heal with no kInstant apply (`Actuation.cpp:1082-1089`, `Actuation_Direct.cpp:889`/`:1186`); **only `NotApplicable` (channel absent) degrades to kInstant**, so a heal CAN be lost to arbitration. A claim that stands with no observed cast logs a rate-limited diagnostic (Task 6) instead of falling back — no delivery watchdog |

`native/APMFBridge.h`'s `kHealCastTtlMs` (6 s) sizes BOTH the `RequestCast`
payload's `req.ttlMs` and (via `native/ComposedCast.cpp`'s `kHealBoundsTtlMs`,
which aliases it) the `CastBounds::Arm` ceiling.

> **CORRECTED 2026-09-07 — this paragraph used to say the TTL is "re-armed every
> tick `Try()` succeeds, so it is only the guardrail on a crashed/forgotten
> claim, never a real cap on a continuous heal". That is FALSE on `main`.**
> The `CastBounds::Arm` ceiling IS re-armed each successful `Try()`
> (`native/ComposedCast.cpp:453`). **The APMF CLAIM's `req.ttlMs` is NOT.**
> `EnsureCastClaimLocked` (`native/APMFBridge.cpp:339-534`) returns early when
> the claim is unchanged AND `IsClaimLive` says it still holds — and that early
> return performs no renew, no `Repoint`, nothing. So the claim runs out its
> 6 s on APMF's own clock and MFO only re-requests on the tick AFTER expiry.
> **RC2 of `Docs/DIAG-2026-09-06-deny-heal-failures.md` measured exactly that:
> hard 6.0 s claim lives and a recurring 0.3-1.2 s UNCLAIMED gap every 6 s**,
> inside which foreign spells equip and charge on the "claimed" hand. It IS a
> real cap on a continuous heal (CLAUDE.md principle 9, and see ENGINE_NOTES
> §0.45).
> **PENDING, NOT SHIPPED — corrected 2026-09-07 after checking BOTH repos.** The
> APMF half of F2 (`Repoint` renews the TTL) IS merged to APMF `main`
> (`fix/apmf-claim-renew-denyhand-spellsteer` = `ed637fe`, 2026-09-07 10:08 PDT),
> but it is **in no tagged APMF release** — the newest is `v0.9.2` (2026-09-06),
> which predates it, and MFO ships against a release. **The MFO half — the
> heartbeat — does not exist at all:** `APMFBridge.h:670` says "MFO never Repoints (⚠ the REST of that comment, `:672-673`, is STALE — it still says APMF's `ApplyRepoint` does not renew and that the renewal is unmerged; both are false on APMF `main` today, and that header needs its own comment-only fix)
> a `kIntent_Cast` claim" and `APMFBridge.cpp:346-397` performs no renew. A
> renewing APMF that is never asked to renew changes nothing, so the 6 s gap is
> still open. Do not restore the old wording until a newer APMF release ships AND
> MFO heartbeats, and both have been field-verified. `Config::g_cfcBackoffMs` is now VESTIGIAL (the retired
MFO-side drive's degrade backoff; the claim is a cheap create-or-refresh every
tick with no backoff of its own — a CHANGE releases and re-requests rather than
repointing, since `RequestCast` has no in-place re-point) — left declared/parsed
since its INI key name is a frozen MCM-Helper identity.

### CAST-CLAIM OBSERVABILITY (ABI v6, feat/consume-cast-observability, 2026-09-06)

Field-diagnosed the same day as the per-hand cast-slots pass: the engine layer
above WORKS (a claimed heal genuinely cast — `CASTER[L] state=4(Casting)
spell=0xFF001F2F` then `state=6`, concentration channel open) but MFO carried
two blind spots that made it LOOK like a total failure. APMF's `APMF_API_v6`
(append-only over v5, `native/APMF_API.h`, mirrored byte-identically — never
edited on MFO's side) adds two read-only queries that close both:

```
RE::FormID (*GetCastProxy)(Handle h);  // the delivery-flip proxy APMF minted, or 0
bool       (*IsClaimLive)(Handle h);   // true while APMF still holds that claim
```

**1. Recognise the proxy as our own cast.** APMF mints its own delivery-flip
proxy for a `kSelf`-delivery spell aimed at a non-self target (`req.proxy = 0`
in `EnsureCastClaimLocked`, `native/APMFBridge.cpp` — MFO never inspects
delivery, never builds one itself). The cast that actually lands is of that
PROXY FormID (e.g. `0xFF001F2F`), never the gambit's original spell (e.g.
`0x0002F3B8`) — but `Diagnostics.cpp`'s `SpellSink` and `ComposedCast::
ExpectingCast` only ever matched the original spell, so the real cast was
filed as `"their own spell, not ours"` and the `[cfc] ... claim live N ms with
NO observed cast` diagnostic fired a false alarm on a claim that had, in
fact, fired.

> **⚠ AS FIRST WRITTEN, THE FIX BELOW DID NOT CLOSE THE BLIND SPOT
> (found 2026-09-06, RC4; CLOSED on `main` 2026-09-07 — see the end of this box).**
> `GetCastProxy` walks APMF's **PUBLISHED** snapshot,
> so a handle that has not been DRAINED yet returns **0** — and MFO fetched the
> proxy exactly once, synchronously, immediately after `RequestCast`, i.e.
> always before the drain. The 0 was then cached at two layers:
> `native/ComposedCast.cpp:114` (pre-fix numbering) set `w.proxy` **only** on the
> `w.spell != a_spell` branch (a same-spell re-request never updates it), and
> `:183` passed a hard-coded `0` as `CastBounds::Arm`'s proxy argument. The
> proxy is also NOT stable across re-mints (APMF's `CastProxy.cpp` 4-slot pool),
> so a cached value can go stale as well as start wrong.
> **FIXED ON `main` 2026-09-07** (`fix/mfo-heal-slot-and-proxy`, merged): F4 landed.
> `EnsureCastClaimLocked` now re-reads `GetCastProxy` on the LIVE path
> (`APMFBridge.cpp:395-396`, not only once at request time — `:528-529` is the
> request-time read it supplements), `ComposedCast.cpp:140`
> guards the cache with `if (a_proxy != 0) w.proxy = a_proxy;` so a same-spell
> re-request can still fill in a proxy minted later and a re-mint cannot blank it,
> and `CastBounds::Arm` is passed the real proxy
> (`ComposedCast.cpp:453 … APMFBridge::GetHealCastProxy(fid) …`) instead of a
> hard-coded `0`. **The line numbers quoted in this warning are the PRE-FIX ones**
> and are kept only to show what the defect looked like. Still true, and still the
> reason the fix is a workaround rather than a cure: `GetCastProxy` walks the
> PUBLISHED snapshot, so the proxy is 0 until APMF's next Drain — a call-and-response
> API should return the proxy rather than make the client poll for it.

Fix (as designed, and as it now behaves on `main`):
`EnsureCastClaimLocked` fetches `GetCastProxy(c.handle)` right after a
successful `RequestCast` (ABI < 6 → `0`, same as a claim that minted none) and
stores it on the `CastClaim` (`native/APMFBridge.cpp`'s `CastClaim::proxy`).
Two new bridge accessors expose it read-only: `APMFBridge::GetHealCastProxy`
and `APMFBridge::GetOffenseCastProxy` (`native/APMFBridge.h`). Every caller
that arms `ComposedCast`'s silent-claim watch now fetches this alongside the
spell and forwards it: `ComposedCast::Try` (heal, always LEFT) and every
`ComposedCast::WatchClaim` call site (`Actuation.cpp`'s owned-cast branch,
`Actuation_Direct.cpp`'s two self/target concentration-offense claims).
`ComposedCast`'s per-hand `Watch` struct now carries a `proxy` field alongside
`spell`; `ExpectingCast`/`NoteObservedCast` match on **either** — a claim that
lands as the proxy now counts as this claim's cast landing, same as one that
lands as the original spell. `Diagnostics.cpp`'s `SpellSink` "ours" check
(the `*** MFO GAMBIT SPELL ***` vs `(their own spell, not ours)` log tag,
previously `g.actionParamForm == spellID` only) now also counts a hit via
`ComposedCast::ExpectingCast(casterID, spellID)`, so it stops mislabeling a
proxied claim's own animated cast as somebody else's. **The diagnostic is not
weakened** — a claim that produces neither the original spell's nor the
proxy's cast still warns exactly as before (principle 7: never mask a real
silent failure).

**2. Stop trusting a dead handle.** APMF auto-expires a claim at its own TTL
(`kHealCastTtlMs`, 6 s) with no notice to the client. `EnsureCastClaimLocked`'s
unchanged-claim fast path used to trust a stored handle unconditionally and
return early — so once APMF silently dropped a claim, MFO kept believing it
was still live and never re-requested. Field-observed: the gambit re-fired
every ~1.5 s for 23 s while this early-return swallowed every re-fire and
`RequestCast` never reached APMF again ("claim live 26650 ms" was MFO's own
belief, not APMF's state).

Fix: on the unchanged-claim fast path, ABI ≥ 6 now calls `IsClaimLive(c.handle)`
before returning; a `false` result resets the claim to empty and falls through
to the SAME fresh-`RequestCast` code the "values changed" branch already used
(no new re-claim loop, no watchdog — the gambit's own cadence keeps re-firing
into `EnsureCastClaimLocked` every tick regardless; this only stops MFO from
silently eating those re-fires). ABI < 6 skips the check and returns exactly
as before — byte-identical degrade, matching every other `>= N` ABI guard in
this file.

Both changes are entirely inside `native/APMFBridge.cpp` (`CastClaim`,
`EnsureCastClaimLocked`, the two new `GetXCastProxy` accessors),
`native/ComposedCast.h`/`.cpp` (`Watch::proxy`, `WatchArmed`/`WatchClaim`'s new
`a_proxy` parameter, the spell-or-proxy match in `ExpectingCast`/
`NoteObservedCast`), and their call sites in `Actuation.cpp`/
`Actuation_Direct.cpp`/`Diagnostics.cpp` — no ABI header change, no widened
deny, no fabricated cast.

## KEY SYMBOLS (`Actuation_Direct.cpp` — NOT `Actuation.cpp`; corrected 2026-09-07)

*Every symbol in this list lives in `native/Actuation_Direct.cpp` on `main` (zero hits in
`Actuation.cpp`); they moved there in the 2026-08-31 split.*

`ConcProxy` (owner-keyed `Slot g_slot[2]{form,source,owner}`, `Configure`, `Acquire`,
`FormForOwner`, `Free`, `Reset`) · `DeliverySpell` (gate; nullptr → caller skips) ·
`ApplyTargetEffect` (the proxy wire-in; AUTO `ApplyEffectFromTo` skips conc-Self) ·
`SustainConcentrationEffect` (per-beat AE window) · `DrawConcCap` (randomized stream cap) ·
`TargetCastReconcile` / `SelfCastReconcile` (cap + heal-full + magicka-out release) ·
`ClearSelfCasts` (→ `ConcProxy::Reset` on revert/load) · `TargetCastEndActor(target,spell,owner)`
(dispel source + owner proxy AE, `InterruptCast` the channel, `ConcProxy::Free`) ·
`SelfCastEndActor` (dispel + `InterruptCast` the self channel).
