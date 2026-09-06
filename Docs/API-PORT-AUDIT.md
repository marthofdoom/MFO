# API PORT AUDIT — is MFO using APMF for everything it already claims?

**Read-only audit, 2026-09-06.** Scope: `origin/main` @ `a486b8e` (verified identical to the
local `main` checkout at audit time). Question answered (marth, verbatim): *"MFO will still
need to use the API for all those things if it isn't already."* Cast is fully ported and
field-proven. This audit classifies every OTHER NPC-touching MFO behaviour the same way.

**Sources read:** `CLAUDE.md` (10 principles), `Docs/CAST-DELIVERY.md` (authoritative, read in
full), `native/APMFBridge.h`/`.cpp` (MFO's ONLY APMF-aware code), `native/APMF_API.h` (byte-shared
ABI, mirrored from the APMF repo), `ai-package-management-framework/Docs/CHANNEL-MAP.md` (the
17-channel intent taxonomy), plus direct reads of `Targeting.cpp`, `CombatStyle.cpp`,
`ComposedCast.cpp`, `Actuation.cpp`, `Actuation_Direct.cpp`, `Logistics.cpp`, `Rapport.cpp`,
`CasterConsent.cpp`, and a full pass of `Packages.cpp` (via subagent, findings verified against
the code cited).

**Classification legend:** PORTED / SHOULD PORT / LEGITIMATE DIRECT / CANNOT PORT YET. Each
finding is marked CONFIRMED (I read the cited lines) or HYPOTHESIS (inferred, not directly
verified — see §5).

---

## 0. The one correction that shapes this whole audit (equip)

marth's requirement, verbatim: **"equipment must be correct at all times and in combat. Only
enforcing equipment sometimes is practically worthless."** Disasm (PASS S, 2026-09-06) proved
only that `CombatInventory::Update` (the weapon rescore pipeline) is **skipped** when the actor
has no live `CombatController` — it did **not** prove nothing else ever contests an out-of-combat
equip pick (the engine has its own non-combat equip behaviour, e.g. torch-at-dusk; whether that or
anything else races MFO's OOC picks is an **open question**, not a proven absence).

So the correct model is **two mechanisms for two regimes, coverage never drops**:
- **In combat:** the AI rescores continuously — a bare one-shot force gets overwritten (the known
  bug class: an equip gambit's dagger displacing a spell mid-cast, followers ending up unarmed).
  The fix is **score-steering / an APMF claim**, not fighting the AI every tick.
- **Out of combat:** the rescore pipeline doesn't run — nothing competes, so a direct force-equip
  sticks cleanly and **must stay** the mechanism. This is not lower priority; it is already correct
  and dropping it would reduce coverage.

Every equip finding below is classified against this frame, not against "combat is the only place
that matters."

---

## 1. Summary table

| # | Facet / behaviour | Status | APMF intent | File:line (primary) | Effort to close gap |
|---|---|---|---|---|---|
| 1 | Cast — hostile Offense, non-self, non-concentration ("owned cast") | **PORTED** | ch.8 `kIntent_SelectSpell` + ch.6 `kIntent_CombatTarget` | `Actuation.cpp:504-538` | — |
| 2 | Cast — Heal, self or ally, any delivery (Composed Forced Cast) | **PORTED** | ch.8b `kIntent_Cast` (`RequestCast`) | `ComposedCast.cpp:99-154`, `APMFBridge.cpp` `ClaimHealCast` | — |
| 3 | Cast — **Buff/Heal aimed at ally or player, fire-and-forget (non-concentration)** | **PORTED** (feat/castone-heal-gate, 2026-09-06) | ch.8b `kIntent_Cast` (`ComposedCast::Try`'s reach extended) | `Actuation.cpp` `CastOn`, immediately after the `ownedCast` block, before the AI-first-grace wait | — |
| 4 | Cast — self/ally Offense or Buff, fire-and-forget, non-Heal | **LEGITIMATE DIRECT** | none (ch.8b is Heal-only by design) | `Actuation_Direct.cpp` `CastSelfDirect`/`CastTargetDirect` non-Heal branches | — |
| 5 | Cast — concentration Offense at a foe | **LEGITIMATE DIRECT** | none (exact-bounding conflict, documented) | `Actuation.cpp:169-266` `ConcentrationCast` → `CastTargetDirect` | — |
| 6 | Cast — legacy AI-first-wait + force-on-miss hybrid (Offense, APMF absent/off) | **LEGITIMATE DIRECT** | — (documented degrade, example (a)) | `Actuation.cpp:595` `ForceCast`, `:785` `CastSpellImmediate` | — |
| 7 | Cast — OOC hostile force (no combat AI exists to arbitrate against) | **LEGITIMATE DIRECT** | none applicable OOC | `Logistics.cpp:1438-1490` | — |
| 8 | Combat-target (who a follower fights) | **PORTED** | ch.6 `kIntent_CombatTarget` (arbitrate) + client executes the write (by design) | claim: `Actuation.cpp:528,1017`; write: `Targeting.cpp:101,118-119` | — |
| 9 | Combat-style swap (the cast-enable lever) | **LEGITIMATE DIRECT** | none — this IS ch.8's documented client-execution mechanism | `CombatStyle.cpp:170,185,214` | — |
| 10 | Combat-style swap, dev probe (default OFF) | **LEGITIMATE DIRECT** | n/a — diagnostic only | `CasterConsent.cpp:442,458,485` | — |
| 11 | Equipment — weapon-order force-hold, **in combat** | **PORTED** | ch.15 `kIntent_Equipment` (gate-only; MFO executes) | claim: `Actuation.cpp:1250,1296`; execute: `:872-906` | — |
| 12 | Equipment — loot pickup / torch upkeep, **out of combat** | **LEGITIMATE DIRECT — must stay** | none applies OOC (nothing to arbitrate) | `Logistics_Loot.cpp:616-617`, `Logistics.cpp:367-391` | — |
| 13 | Equipment — potion consume, broken-gear correction | **CANNOT PORT** (out of taxonomy) | none of the 16 channels fit | `Logistics.cpp:120-124,413-459`, `Logistics_Loot.cpp:311,378` | — |
| 14 | Equipment — spell-hand prep for casting | **LEGITIMATE DIRECT** | none — ch.8's own client-execution mechanism, not a ch.15 contest | `Loadout.cpp:194-195,261,275` | — |
| 15 | Combat-action deny (stop a follower fighting outright) | **BUILT, UNWIRED** | ch.7 `kIntent_CombatAction` | bridge: `APMFBridge.h:199-215`; **zero call sites** | **S** if/when a gambit needs it |
| 16 | Package-offer — loot-travel excursion | **PORTED** | ch.9 `kIntent_OfferPackage` (creation stays MFO's job — example (b)) | offer: `Packages.cpp:1595,1680,1222`; release: `:1417` et al. | — |
| 17 | Package-offer — retreat/flee | **PORTED** | ch.9 `kIntent_OfferPackage` | offer: `Packages.cpp:1904,1231`; release: `:1456` | — |
| 18 | Package — eviction marker / `ReleaseAll` load-revert sweep | **LEGITIMATE DIRECT** | none — bookkeeping, no AI decision to arbitrate | `Packages.cpp:1441,1473,1518,1787,1852,2017,2055` | — |
| 19 | Unused channels — movement block, headtrack, disposition AVs, shout/power select, detection AVs | **N/A** | ch.1, ch.5, ch.11, ch.14, ch.16 | *(no MFO mechanism touches these at all — see §5)* | — |

---

## 2. Per-facet detail

### 2.1 Casting — already ported (rows 1, 2)

**Offense "owned cast."** `Actuation.cpp:504-509` gates on `APMFBridge::Available() &&
Config::g_apmfCast && !Config::g_legacyCastHybrid && target real (≠follower, ≠player) &&
ClassifySpell==Offense`. When true, `:527-528` claims `ClaimCasting`
(`kIntent_SelectSpell`) + `ClaimCombatTarget` (`kIntent_CombatTarget`) every winning tick, then
MFO EXECUTES with its own mechanisms exactly as ch.8/ch.6 of `CHANNEL-MAP.md` prescribe: equips
the spell into the left hand (`Loadout::Prepare`), commands the target (`Targeting::Command`),
grants consent (`CasterConsent::Want`), and biases the combat style so the AI *decides* to cast.
`:1017` gives a pure-melee `kActAttack` directive the same combat-target claim so a non-caster
follower gets ch.6 arbitration too. **No gap here.**

**Heal, any target/delivery.** `ComposedCast::Try` (`ComposedCast.cpp:99-154`) is HEAL-ONLY
(`:38-45 Enabled()`), AE-only, opt-in (`bHealAnimPackage`, default OFF), requires APMF present.
On success it calls `APMFBridge::ClaimHealCast` → APMF's `RequestCast` (`ch.8b`), and APMF's five
engine vfunc seats (`CheckShouldEquip`/`CheckStartCast`/`GetMagicTarget`/`CheckStopCast`/
`SetupAimController`) drive the follower's OWN AI through a real animated cast — MFO fires
**nothing**. Reached from `CastSelfDirect`/`CastTargetDirect` (`Actuation_Direct.cpp:732,946`),
which are themselves called from `ConcentrationCast` (concentration heal) and from `CastOn`'s
**self-target** branch (`Actuation.cpp:394`, fire-and-forget self heal). A refused claim degrades
to the baseline `kInstant` `CastSpellImmediate` apply — a heal always lands (Task 6's diagnostic
watch logs a silent claim instead of masking it, never a fallback timer, per CLAUDE.md principle 7).
**No gap here either** — this is the "AI performs the cast" beta shipped per the recent
`docs(status)` commit.

### 2.2 Casting — legitimate direct, no port needed (rows 4-7)

- **Row 4 (self/ally Offense or Buff, FF, non-Heal):** `ComposedCast::Enabled()` returns false for
  any kind other than `Heal` (`ComposedCast.cpp:40`) — **by design**, per
  `Docs/CAST-DELIVERY.md`'s "HEAL-ONLY-gated... offense and buff casts never enter this module and
  stay on the byte-identical AI-fired / kInstant paths." `CAST-DELIVERY.md`'s decision table (its
  §"THE DELIVERY DECISION TABLE") documents `CastSpellImmediate` as the **universal, permanent**
  delivery for fire-and-forget regardless of APMF — this is not an absence degrade, it is the
  baseline mechanism the whole cast system is built on. Nothing to port; porting it would also
  contradict CAST-DELIVERY.md's explicit "REJECTED APPROACHES"/"SCOPE-CREEP LESSON" sections, which
  forbid touching FF/self delivery beyond the one proven gap (the ConcProxy delivery flip).
- **Row 5 (concentration Offense):** `Actuation.cpp:414-417` — "Concentration is untouched... an
  AI-channeled concentration cannot be exact-bounded (the freeze)" (CAST-DELIVERY.md line 56-58).
  This is a **documented architectural constraint**, not a missed port. Note for a future session
  (§5): Heal concentration IS bounded via `kIntent_Cast`'s `stopPct`/TTL (`ComposedCast.cpp`), which
  is in tension with "cannot be exact-bounded" — flagged as an open question below, not asserted as
  a gap here.
- **Row 6 (legacy hybrid force-on-miss, true APMF-absent/off case):** `ForceCast`
  (`Actuation.cpp:71-165`, called at `:595`) and the silent `CastSpellImmediate` fallback
  (`:785`) are reached **only** when `ownedCast` is false at `:504-509`. For an **Offense** spell
  at a real target, that's exactly the documented degrade (APMF absent, or `bLegacyCastHybrid`
  toggled on) — CLAUDE.md example (a), verbatim: "documented contract, not a bypass." Correct as
  is.
- **Row 7 (OOC hostile force):** `Logistics.cpp:1438-1490` — a follower fighting while out of
  combat has no combat AI deliberation loop to arbitrate against in the first place (mirrors the
  equip OOC reasoning in §0). `Packages::CastAt` is tried first (still the client's own package,
  ch.9-eligible if it ever needed arbitration — see §2.4); the `CastSpellImmediate` fallback fires
  only on a structural decline. Legitimate.

### 2.3 Casting — the one real gap (row 3) — CLOSED (feat/castone-heal-gate, 2026-09-06)

**Ported.** `CastOn`'s FF non-self branch now calls `ComposedCast::Try(a_follower, spell, a_target,
ClassifySpell(spell), /*stopPct=*/0)` immediately after the `ownedCast` (Offense-only) block, before
the AI-first-grace wait — the exact placement and call shape this section originally recommended,
mirroring `CastSelfDirect`/`CastTargetDirect`'s own unconditional call to the same function
(`ComposedCast::Enabled` still gates it HEAL-ONLY internally, so a Buff kind degrades to `false`
immediately, same as those two call sites). Explicitly scoped to `a_target != a_follower` (a
self-target can still reach this code when `bCastSelf`, dev-only default off, never forked it off
earlier in `CastOn` — self-cast stays `CastSelfDirect`'s own gated mechanism). On success: `HoldCastLock`
+ an OPAQUE `{NoOp,"composed cast: AI deciding (animated, mobile)"}`, matching `ownedCast`'s own
success shape exactly. On refusal (Buff kind, AE/APMF/`bHealAnimPackage` absent, or a lost claim):
falls straight through to the unchanged legacy grace/`ForceCast` hybrid — byte-identical degrade, a
heal never silently vanishes. **In-combat only** — `CastOn` runs only from `Actuation::Fire`'s combat
dispatch; the OOC mirror below (`Logistics.cpp:1394-1436`) was deliberately left untouched (§5.1's open
question is still open). See `MAP.md`'s `Actuation.cpp` entry ("Heal/Buff FF non-self claim") for the
file:line trail.

The original finding (kept below for context/history):

**`CastOn`'s non-self branch never distinguishes Heal/Buff from Offense before falling into the
legacy grace+force machinery — even with APMF present and enabled.**

`Actuation.cpp:266-600` (`CastOn`) handles every `kActCastPlayer`/`kActCastTarget` (ally- or
foe-directed) dispatch that is **not** self-target (intercepted earlier at `:387`) and **not**
concentration (intercepted at `:414-417`). Inside that remaining fire-and-forget branch, the
`ownedCast` gate at `:504-509` requires `ClassifySpell(spell) == SpellKind::Offense`. A **Heal or
Buff spell aimed at an ally or the player, fire-and-forget** — e.g. a "cast Fast Healing at nearest
ally" gambit where Fast Healing is *not* concentration — falls straight past `ownedCast` into the
`held < grace` wait (`:560-589`) and then `ForceCast`/the silent `CastSpellImmediate` apply
(`:595-808`), **regardless of `APMFBridge::Available()`**. `ComposedCast::Try` (the `kIntent_Cast`
Heal claim, §2.1) is never called from this branch — it is reached only via `CastSelfDirect`/
`CastTargetDirect`, which `CastOn`'s non-self FF path does not call.

This means: the AI-decided, fully-animated cast that Heal-at-ally already gets when delivered as a
*concentration* spell (via `ConcentrationCast` → `CastTargetDirect` → `ComposedCast`) is **not**
available to the identical gambit when the configured spell happens to be fire-and-forget instead
— it silently downgrades to the older grace-wait-then-force hybrid, with the rooting `UseMagic`
package or an unanimated `CastSpellImmediate`, exactly the cost APMF's owned-cast model exists to
remove.

**What porting would change:** before the `held < grace` wait at `Actuation.cpp:560`, add a call to
`ComposedCast::Try(a_follower, spell, a_target, ClassifySpell(spell), stopPct)` for the
`Heal`/`Buff` kinds (mirroring what `CastSelfDirect` already does for self-target). On success,
return early exactly as the `ownedCast` branch does at `:542` ("AI deciding" — no grace wait
needed, the AI is already driving via APMF's seats); on refusal, fall through to the existing
grace/`ForceCast` path unchanged (byte-identical degrade, matching the Heal path's own contract:
"a refused claim degrades to kInstant... a heal always lands").

**Scope constraint (§0's frame applies here too):** `ComposedCast`'s engine seats live on
`CombatMagicCasterRestore` — a **combat AI** caster class. This branch of `CastOn` only runs from
`Actuation::Fire`, which is itself the in-combat dispatch (the file's own header comment: "the
combat-rule ACTUATION dispatch"), so a live `CombatController` should already be guaranteed there
— **this in-combat leg is confidently SHOULD PORT.**

The identical structural gap exists **out of combat** at `Logistics.cpp:1394-1436` ("FIRE-AND-FORGET
beneficial direct-apply" — self-gate-off / player / ally). Whether porting that leg is safe is an
**open question, not a recommendation** — see §5.1: if `kIntent_Cast`'s engine seats require a live
`CombatController` (unverified either way), the OOC leg cannot use them at all and must stay direct,
mirroring the OOC equip case in §0. Do not port the OOC leg until that is checked.

### 2.4 Package facet (ch.9) — rows 16-18

Fork-verified by direct read of `Packages.cpp:751-2060`. Two families are already fully ported:

- **Loot-travel** (`LootTravelFill`/`Retarget`): writes the alias's own runtime target
  (`ForceRefToNative`, the client's own job) then calls `APMFBridge::OfferPackage`
  (`Packages.cpp:1595,1680`, refreshed every ~500ms at `:1222`), released at `:1417` et al. The
  `ForceRefToNative` legacy-alias branch runs **only** when `!APMFBridge::Available()` — confirmed
  by the code's own comment: "NEVER a decline-fallback... APMF is the COMMITTED route," matching
  CLAUDE.md's "MFO is an APMF showpiece" principle exactly.
- **Retreat/flee** (`RetreatFill`): identical shape — offer at `:1904,1231`, release at `:1456`.

One family is **not yet resolved** and needs a follow-up read outside this audit's file scope:
`MFO_CastPackage`/`MFO_CastPackageSelf` (`Packages.cpp:751-998`, `ForceRefTo` at `:807,952,963`,
`EvaluatePackage` at `:828,1264`) claims by **static quest priority 60** only — it never touches
`APMFBridge::OfferPackage`, and its own comment logs "outranked by a higher-priority package?" —
precisely the race ch.9 exists to fix. **This is `ForceCast`'s package half** (§2.3, `Actuation.cpp
:96-97` `Packages::CastSelf`/`CastAt`) — so it is reached under the exact same conditions as the
`CastSpellImmediate` fallback beside it: APMF-absent/legacy-toggle for Offense spells (legitimate),
**or** any Heal/Buff-at-ally FF cast regardless of APMF state (the §2.3 gap). It is not a
*separate* finding — porting §2.3 removes essentially all of this route's non-degrade traffic. No
new work item beyond §2.3.

The eviction-marker / `ReleaseAll` sweep (`:1441` et al.) is pure session load/revert bookkeeping —
freeing MFO's own serialized alias fills, never an AI decision — correctly LEGITIMATE DIRECT.

### 2.5 Combat-action deny (ch.7) — row 15

`APMFBridge::ClaimCombatActionDeny`/`ReleaseCombatActionDeny` (`APMFBridge.h:199-215`,
implemented in `.cpp`) is fully built and wraps ch.7's `kIntent_CombatAction` — but **grep confirms
zero call sites anywhere in `native/*.cpp`.** The bridge's own doc comment says why: "NOT wired
into the loot-travel dispatch by default... MFO's own PACKAGE-THEFT guard already concedes
loot-travel to a live combat package on purpose... this helper exists for a caller that has a
narrower, considered need." This is not a gap in the audited sense (nothing is fighting the engine
directly here) — it's a **built-but-dormant capability**: a future gambit that needs a follower to
stop fighting outright (a stealth/pickpocket directive, a forced retreat that must not swing on the
way out) has a ready-made claim to call. Flagged so the next session doesn't rebuild it.

### 2.6 Equipment facet (ch.15) — rows 11-14

**In combat, weapon-order force-hold — already ported.** `Actuation.cpp:872-906` force-equips
directly via `ActorEquipManager` (MFO's job to execute, per `APMFBridge.h:131-138`'s own doc:
"MFO still EXECUTES the force-equip itself... this claim only makes APMF's OWN gate enforce...
instead of MFO's own gate doing it natively"), paired with `ClaimEquipment`/`ReleaseEquipment`
(`:1250,1296`) so APMF's T2a `CheckShouldEquip` hook denies a competing spell/staff re-arm for the
duration of the hold — the SAME score-steering-adjacent pattern §0 calls for. `CombatStyle.cpp`'s
own native equip-deny gate (`EquipGateThunk`) stands itself down when
`APMFBridge::IsEquipmentClaimActive` is true (`:318`), so there is exactly one enforcement path
active at a time, never two disagreeing gates. **This is the template any future in-combat
equip-contest case should copy** — it is the concrete existing instance of "score-steer, don't
force-fight" that §0's correction calls for.

**Out of combat — loot pickup, torch upkeep.** `Logistics_Loot.cpp:616-617` (equip a freshly-looted
upgrade) and `Logistics.cpp:367-391` (`EquipTorch`) run where `CombatInventory::Update` is a no-op
(no `CombatController`) — nothing contests these picks, so the direct force-equip **is already
correct and must remain the mechanism.** Per §0, this is not a lower-priority leftover; it is the
correct answer for that regime.

**Not equipment-preference contests at all.** Potion consumption (`Logistics.cpp:120-124`, an
equip-to-consume idiom) and broken/invisible-gear correction (`Logistics_Loot.cpp:311,378`;
`Logistics.cpp:413-459`, `HealExcludedWeapon`) are one-shot corrective or consumable actions, not a
live preference between two valid choices — none of APMF's 16 channels fit, and none should be
invented for them.

**Spell-hand prep.** `Loadout.cpp:194-195,261,275` (`EquipSpell` into the left hand before a cast)
is ch.8's own documented client-execution mechanism verbatim (`CHANNEL-MAP.md` row 8: "equip spell
+ write own selectedSpells...") — a category error to route through ch.15, which arbitrates
weapon-vs-weapon/spell *contests*, not spell selection itself.

### 2.7 Combat-target (ch.6) and combat-style — rows 8-10

**Combat-target is correctly BOTH claimed and directly written — that is ch.6's own documented
shape, not the anti-pattern this audit was told to hunt for.** `CHANNEL-MAP.md` row 6 states ch.6's
mode as "ARBITRATION-ONLY today... CLIENT commands it: compare-and-write of `currentCombatTarget`
(MFO's `Targeting::Command`...) — APMF makes NO combat call." `Actuation.cpp:528,1017` claim the
facet via `ClaimCombatTarget`; `Targeting.cpp:101` (`rt.currentCombatTarget = wanted`) and
`:118-119` (`cc->targetHandle = wanted`) are MFO's own required execution of that claim, from the
`UpdateCombat` hook. Removing the direct write would leave the claim pointing at nothing — there is
no APMF mechanism that ever performs a combat-target write on the client's behalf (ch.6 is
ARBITRATE-only, not DENY). **No further work; this is done correctly.**

**Combat-style swap** (`CombatStyle.cpp:170,185,214`, the CSTY pointer MFO swaps to bias the AI
toward casting/melee) has **no dedicated APMF channel at all** — it is explicitly the client's own
half of ch.8's "how the CLIENT drives it" column ("a Cast-biased combat style makes the follower's
own AI DECIDE to cast"). Correctly direct. The dev-only probe swap (`CasterConsent.cpp:442,458,485`,
gated `Config::g_probeCastStyle`, default OFF) is diagnostic scaffolding, not a shipped facet —
also correctly direct.

`Rapport.cpp:372-375`'s `currentCombatTarget`/`targetHandle` clear on ally-conflict de-aggro is
teardown (drop a target that shouldn't have been acquired), not a facet claim — no APMF interaction
needed there.

---

## 3. Prioritised order (cheapest, highest-value first)

1. ~~**§2.3 — extend `ComposedCast::Try` coverage to FF Heal/Buff-at-ally, in-combat only**~~ —
   **DONE (feat/castone-heal-gate, 2026-09-06).** A field verification pass (same S1 discipline the
   Heal port itself went through) is still outstanding before this is deck-proven.
2. **§2.5 — no code change, just a documented backlog item**: `kIntent_CombatAction` is built and
   ready; wire it the day a gambit needs "stop fighting entirely" semantics (a stealth directive, a
   forced-retreat that must not swing). Effort S when that need arrives; zero effort now beyond
   this note.
3. **§2.4 — resolve the `MFO_CastPackage` static-priority-60 open question** (does it ever
   contend with an outranking custom-AI framework's package the way loot-travel's Cicero case
   did?). Not a code change on its own — porting §2.3 removes most of its live traffic; only worth
   revisiting if a field report shows a package-locked follower still missing forced casts after
   §2.3 ships.
4. **Everything else in the table is already correctly PORTED or correctly LEGITIMATE DIRECT.**
   There is no "wholesale equip retirement" work item — §0's correction rules that out explicitly;
   the only standing equip work is "keep both regimes correct," which they already are.

---

## 4. What changed vs. the brief's rough call-site counts

The brief's counts (33 equip / 14 combat-target / 6 combat-style / 26 package / 60 cast) were
rough greps across comments and file mentions. Real call sites, verified:

- **Equip:** ~21 real `ActorEquipManager`/`EquipObject`/`UnequipObject`/`EquipSpell` calls across
  `Loadout.cpp`, `Logistics.cpp`, `Logistics_Loot.cpp`, `Actuation.cpp` (§2.6 covers all of them).
- **Combat-target:** exactly 2 real writes (`Rapport.cpp:374`, `Targeting.cpp:101` — plus the
  paired `targetHandle` writes at `Rapport.cpp:375`, `Targeting.cpp:119`); the rest of the grep hits
  were reads or bridge-internal plumbing.
- **Combat-style:** exactly 6, matching the brief precisely (`CombatStyle.cpp:170,185,214`,
  `CasterConsent.cpp:442,458,485`).
- **Package:** 16 real `EvaluatePackage(...)` calls + ~19 `ForceRefToNative` alias-fill calls
  (35 total engine-facing calls, not 26 — the brief undercounted; see §2.4's family breakdown).
- **Cast:** 10 real `CastSpellImmediate(...)` calls (the brief's 60 counted comments/mentions of
  the string across headers and docs, not call sites).

---

## 5. What this audit could not determine

1. **HYPOTHESIS, needs RE or a passive probe:** does `kIntent_Cast`'s engine-seat mechanism
   (`CombatMagicCasterRestore`'s five hooked vfuncs) function at all when the actor has **no live
   `CombatController`** (i.e. is not in combat)? This governs whether the §2.3 OOC leg
   (`Logistics.cpp:1394-1436`) can ever be ported, or must permanently stay direct like OOC equip.
   Not verified either way in this pass — do not port the OOC leg on the assumption it mirrors the
   in-combat case.
2. **HYPOTHESIS:** `CAST-DELIVERY.md` states concentration Offense "cannot be exact-bounded"
   (`Actuation.cpp:414-417`'s stated reason for never claiming it), yet Heal concentration IS
   bounded today via `kIntent_Cast`'s `stopPct`/TTL. Whether this is a real architectural
   difference between Offense and Heal concentration, or whether Offense concentration could
   likewise be exact-bounded via the same TTL/stopPct mechanism and simply hasn't been tried, is
   unresolved — flagged for a future session, not asserted as a gap here.
3. **Confirmed absent, not further investigated (out of the brief's five named facets):** MFO has
   **zero** direct writes to ch.1 movement-block, ch.5 headtrack, ch.11 disposition AVs
   (aggression/confidence/morality/assistance), ch.14 shout/power select, or ch.16 detection AVs
   (grepped, confirmed no hits in `native/*.cpp`). These are not gaps in existing behaviour — MFO
   simply has no feature touching these facets yet, so there is nothing to port.
4. **Not independently re-verified in this pass** (taken from `APMFBridge.h`/`.cpp`'s own doc
   comments, which are detailed and internally consistent, but this audit did not re-derive them
   from first principles): the exact round-robin `FacetExpiry()` timing math, and the claim/release
   lifecycle edge cases already covered by `Docs/CAST-DELIVERY.md`'s S1 field-test bug writeups
   (BUG A/B). Nothing here contradicts those; they were not re-audited.
