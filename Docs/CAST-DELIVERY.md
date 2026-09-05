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

**S1 rework (2026-09-05, "prove cast+heal on APMF"): the MFO-side hand-drive is
RETIRED.** The first cut of CFC (`kIntent_Cast`/`RequestCast`, ABI v5) had MFO
itself force-equip the follower's hand caster and replay the observed cast
sequence (`DriveObservedCast`/`PhaseSelect`/`PhaseFire`/`HealProxy`) — it stayed
OBSERVE-ONLY (always degraded to kInstant) because it raced APMF/the AI for the
SAME hand, and that race caused a cross-thread use-after-free CTD in the field.
APMF's `feat/cast-act` graduated the EXISTING `kIntent_SelectSpell` channel into a
declarative contract instead: name the spell/hand/target and APMF itself equips,
drives the animated sequence, and guarantees delivery. `ComposedCast.cpp` is now a
thin shim; `native/ComposedCast.cpp`'s `DriveObservedCast` family and
`APMFBridge`'s old `ClaimCast`/`ReleaseCast`/`IsCastClaimActive`/`CastReq` are
gone from MFO entirely (the old `kIntent_Cast`/`RequestCast` facet still exists in
`APMF_API.h` — APMF's byte-shared header, mirrored verbatim — but MFO no longer
calls it).

`ComposedCast::Try(follower, spell, target, kind)` (`native/ComposedCast.cpp`) sits
where `HealAnimFill` used to be called, in `CastSelfDirect`/`CastTargetDirect`
(`Actuation_Direct.cpp`), after the competence gate — call sites unchanged, only
`Try`'s internals changed. It is **HEAL-ONLY-gated** (`kind != SpellKind::Heal` →
immediate false): offense and buff casts never enter this module and stay on the
byte-identical AI-fired / kInstant paths. It composes two things, no hand touch at
all:

1. `APMFBridge::ClaimHealCast(fid, spellID, targetID, APMFBridge::kApmfHandLeft)`
   — the declarative `kIntent_SelectSpell` +ACT claim:

   ```cpp
   APMF_API::APMF_Param p{};
   p.form   = spellFormID;      // the SpellItem
   p.ival   = kApmfHandLeft;    // ALWAYS left (2) -- never auto (0). See the HAND FIX below.
   p.target = castTargetFid;    // 0 => self (or a winning combat-target claim first)
   Handle h = api->RequestEx(actorFid, kIntent_SelectSpell, basis, &p);
   ```

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
   left-hand almost always regardless). An intelligent per-perk/loadout-aware
   hand pass (dual-cast when both hands are free, etc.) is tracked separately
   as future work, not built here.

   APMF equips the resolved hand(s), drives the observed animated sequence
   (`core/CastExecutor.cpp`, ported/graduated from this file's old drive — same
   phase shape, same proxy trick, now on APMF's side under its own per-hand
   deny), and GUARANTEES delivery (falls back to its own `CastSpellImmediate` if
   the drive can't animate, VR included). Repointing the SAME handle with the
   same values while mid-cast is a no-op; a changed spell/hand/target switches in
   place; a repeat call once parked fires again (the heal-over-time cadence).
2. A `CastBounds::Arm` registration for (actor, spell) (proxy = 0 — APMF owns any
   delivery-flip proxy on its own side now). This is still the fix for the
   CasterConsent HARD-ABORT described below: MFO's globally-installed consent hook
   also intercepts APMF's driven `CheckCast`/`RequestCastImpl` on the SAME hand
   caster, so it still needs to be told this (actor, spell) pair is authorized.

A refused claim `CastBounds::Disarm`s and degrades to the caller's existing
`kInstant` apply. A heal always lands, exactly as it does today. Release
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

**Flagged, not fixed here (out of this pass's scope):** offense's own "owned cast"
gambit (`Actuation.cpp:522`, `APMFBridge::ClaimCasting`) rides the SAME
`kIntent_SelectSpell` channel this heal claim uses. APMF's `feat/cast-act` drives
EVERY winning claim on that channel via `core/CastExecutor.cpp`, not just heal-cast
ones — so once a feat/cast-act APMF build is actually loaded, the offense claim may
also start being equipped/driven by APMF, independent of anything in MFO. That is
an APMF-side architectural change outside this repo's control; MFO's own offense
dispatch logic is untouched by this pass, and CI never links a live APMF.dll to
exercise the interaction.

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

### FORWARD LOOK — native-AI seat-answering (RE'd, NOT built, 2026-09-05)

A separate research effort (outside this repo) worked out a DIFFERENT, more
native mechanism than the PASS E shape documented above: instead of APMF's
`core/CastExecutor.cpp` equipping+driving the follower's hand caster on MFO's
behalf, APMF would answer five vfunc seats on the follower's own combat caster
object — `CheckShouldEquip` (0x0F), `CheckStartCast` (0x06), `GetMagicTarget`
(0x0A), `CheckStopCast` (0x07), `SetupAimController` (0x0D) — so the FOLLOWER'S
OWN AI equips, aims, charges, and fires the heal at the claimed ally, with ZERO
engine-cast call from either mod. This is a real, disassembly-backed research
conclusion with a concrete implementation plan (the RE notebook's own "IMPLEMENTATION
PLAN — research conclusion, no code written"), but as of this writing **it is not
implemented anywhere in the APMF tree** — every branch touching this idea is
OBSERVE-ONLY (passive seat probes, no seat answered). The natural channel for it
is the already-declared `kIntent_Cast`/`RequestCast`/`APMF_CastRequest` (ABI v5,
`APMF_API.h`) — currently UNUSED by MFO — but that Intent's own doc comment still
describes the RETIRED PASS D contract ("APMF fires NO cast: the CLIENT executes
its own animated cast"), i.e. the exact MFO-drives-the-hand design this file's
"S1 rework" section above already retired for racing. **Switching
`ComposedCast::Try` onto `RequestCast` today, ahead of APMF actually
implementing the 5-seat answers, would silently break heal delivery** (nothing
would equip or animate the spell). Do not make that switch until an APMF build
exists that answers those seats and `APMF_API.h`'s own comments are updated to
match it — at that point this file's `ComposedCast` section is expected to need
only its Intent/function-pointer names updated, not its overall shape.

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
| APMF owned cast | the follower's own AI decides | yes | hostile foe only | default when APMF is present |
| Composed Forced Cast (CFC) | `ComposedCast::Try` → `APMFBridge::ClaimHealCast` (declarative `kIntent_SelectSpell` +ACT) | APMF equips + drives the animated sequence + guarantees delivery | any actor (explicit target rides the claim) | opt-in (`bHealAnimPackage`), HEAL-ONLY. A refused claim degrades to kInstant every time — heal always lands |

`native/ComposedCast.cpp`'s `kHealBoundsTtlMs` (6 s) sizes the `CastBounds::Arm`
ceiling — re-armed every tick `Try()` succeeds, so it is only the guardrail on a
crashed/forgotten claim, never a real cap on a continuous heal. `Config::
g_cfcBackoffMs` is now VESTIGIAL (the retired MFO-side drive's degrade backoff; the
declarative claim is a cheap create-or-repoint every tick with no backoff of its
own) — left declared/parsed since its INI key name is a frozen MCM-Helper identity.

## KEY SYMBOLS (Actuation.cpp)

`ConcProxy` (owner-keyed `Slot g_slot[2]{form,source,owner}`, `Configure`, `Acquire`,
`FormForOwner`, `Free`, `Reset`) · `DeliverySpell` (gate; nullptr → caller skips) ·
`ApplyTargetEffect` (the proxy wire-in; AUTO `ApplyEffectFromTo` skips conc-Self) ·
`SustainConcentrationEffect` (per-beat AE window) · `DrawConcCap` (randomized stream cap) ·
`TargetCastReconcile` / `SelfCastReconcile` (cap + heal-full + magicka-out release) ·
`ClearSelfCasts` (→ `ConcProxy::Reset` on revert/load) · `TargetCastEndActor(target,spell,owner)`
(dispel source + owner proxy AE, `InterruptCast` the channel, `ConcProxy::Free`) ·
`SelfCastEndActor` (dispel + `InterruptCast` the self channel).
