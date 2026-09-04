# MFO — Current Status & Handoff

> **LIVING DOC — KEEP THIS CURRENT.** This is the first thing to read to continue
> MFO from a clean slate. Update it *in the same change* whenever you: ship a
> release, open/resolve an issue, get a field-test result back from marth, or
> change the workflow. A stale status doc is worse than none — if you touch the
> project and don't touch this, you've left the next session a trap.
>
> **Last updated:** 2026-09-04 (doc-only touch; see the branch note directly below —
> the rest of this header below it is UNTOUCHED history and reads OLDER than the
> shipped v2.0.0 / Harbinger work in CHANGELOG.md; treat the CHANGELOG top entry as
> the more current source for what actually shipped since this header was last
> rewritten).
>
> **▶ IN FLIGHT (2026-09-04, branch `feat/forced-cast`, NOT merged, NOT pushed —
> CI status UNVERIFIED, built locally only): the Composed Forced Cast (CFC)
> rework.** New executor `ComposedCast::Try` (`native/ComposedCast.{h,cpp}`)
> replaces the deleted `Packages::HealAnimFill` package-substitution heal route at
> both `Actuation_Direct.cpp` call sites (`CastSelfDirect`/`CastTargetDirect`). It
> composes an APMF cast-execution claim (`kIntent_Cast`, APMF ABI bumped 4→5,
> `APMFBridge::ClaimCast`/`ReleaseCast`/`IsCastClaimActive`), a new `CastBounds`
> registration, the one trigger seam `DriveObservedCast`, and a restore/degrade
> path. **THE HARD-ABORT FIX (§2, ENGINE_NOTES §0.40, INVARIANTS #76):**
> `CasterConsent::ConcUnboundedDeny` was hard-aborting an MFO-executed
> concentration cast at `iCastControl` Exact whenever it did not come through the
> one legacy alias-package stream (`Packages::StreamLive`) — the deck HARD-ABORT
> of `0002F3B8`/`FF001BA4`. New file `native/CastBounds.{h,cpp}` (lock-free 8-slot
> registry, `Arm`/`Disarm`/`Live`/`Reset`) generalizes that one-slot contract; three
> `CasterConsent.cpp` sites now early-pass a `CastBounds`-registered cast
> (`ConcUnboundedDeny`, the `CheckStartCast` thunk, `CheckCastThunk`/0x0A). **THE
> TRIGGER IS NOT BUILT.** `DriveObservedCast` (`ComposedCast.cpp`) is a stub that
> always returns `kNotImplemented`, so `ComposedCast::Try` always degrades to the
> existing `kInstant` heal — the whole executor is runtime-inert today, even with
> the (repurposed) `bHealAnimPackage` toggle ON. The spec's original guessed
> trigger (`RE::TESActionData::Process` + `ActionRightAttack`/`Release` + four
> `BGSAction` FormIDs) was explicitly NOT built (steering call, marth-approved) —
> the trigger is OBSERVE-AND-REPLICATE instead: a parallel APMF passive observer at
> the 0xAD seat will capture the engine's real NPC full-animation cast sequence
> from a deck cycle, and `DriveObservedCast` will replicate it. **DELETIONS DONE:**
> `Packages::HealAnimFill`/`HealAnimHolds`/`HealAnimEvictIf`/`g_healAnimMap` +
> the two UseMagic PACKs (`Forms.h` `0x83B`/`0x83C`, now `_RETIRED`, reserved
> forever per #41) + `make_apmf_heal_packages()` in `MFO_GenerateESP.py`.
> **`feat/heal-anim-proxy` is SHELVED** (tag `archive/heal-anim-proxy-2026-09-04`),
> not merged. **`CastBounds::Arm` has exactly ONE caller today, `ComposedCast::Try`
> — this is BY DESIGN, not a gap.** `ConcProxy`'s plain `kInstant`
> `CastSpellImmediate` direct force skips the `MagicCaster` state machine entirely
> (ENGINE_NOTES §0.13) and so never reaches the hooked `CheckStartCast`/`CheckCast`
> thunks at all — it has SHIPPED and heals correctly at `iCastControl` Exact with
> no HARD-ABORT. The deck HARD-ABORT (`0002F3B8`/`FF001BA4`) was specific to the
> AI-DELIBERATED heal-anim PACKAGE build (the shelved `feat/heal-anim-proxy`),
> never the plain kInstant path. `ComposedCast`'s future hand cast is the one
> MFO-driven cast that WILL deliberate through the real state machine, so it is
> correctly the only path that needs the bound. **NEXT: field-test at Exact to
> confirm the HARD-ABORT is gone for the ComposedCast/legacy-package paths, then
> resume the CFC trigger once the APMF observer's captured sequence lands.** See
> `Docs/CAST-DELIVERY.md` "COMPOSED FORCED CAST (CFC)", `Docs/ENGINE_NOTES.md`
> §0.40, `Docs/INVARIANTS.md` #76, `MAP.md`'s `CastBounds.cpp`/`ComposedCast.cpp`
> entries.
>
> **✅ v1.1.4 SHIPPED (GitHub):** https://github.com/marthofdoom/MFO/releases/tag/v1.1.4 — `MFO-v1.1.4.zip` (main file only; progression add-on UNCHANGED from v1.1.3, no ESP/ESL touched). Commit `4d7ec03`, DLL `286e2452`, ESP `a3a003a7` (unchanged since v1.1.2), CI `33574059247`. Contents = the loot perf + stall pass ([[loot-multiminute-stall-navmesh-sticky]]): off-navmesh false-positives no longer 5-min-sticky-poison reachable loot; a path-failed BODY grows its in-place grab radius to 600u and is hoovered from range; failed targets sort LAST (not abandoned); sticky 5min→60s; drain-all-in-reach per tick (StripCorpse); kArrivalDist 160→200; batchLinger 4→1.5s; econ cooldowns 6/20/20s→2/8/12s. **FIELD-TESTED on Tuxborn before the cut** (native tree of DLL `3e060894`, identical to the release): off-navmesh refs resolved in <1min, ZERO `STICKY-unreachable` all session, loots succeeding, no new errors. **DEFERRED to next release:** excursion-slot bump (ESP-alias-bound, kept 4) + the range-grab-on-external-hold one-liner (Cicero package-lock loot; untested; `Logistics.cpp:957` add `NotePathFail`). **▶ NEXUS = marth's manual step** (v1.1.4 zip + CHANGELOG v1.1.4 block). **NEW WORK (both in design, see memories):** APMF — "AI Package Management Framework", a SEPARATE repo (`Projects/ai-package-management-framework/`, deep-hook AI-control broker via `Actor::Update` 0xAD; its `design.md` is authoritative; marth wants the GH repo public + README next) + the loot-selection expansion (potion/misc submenus + "valuables" value/weight loot-to-sell).
>
> **v1.1.3 SHIPPED (GitHub):** https://github.com/marthofdoom/MFO/releases/tag/v1.1.3 — tag pushed, `MFO-v1.1.3.zip` + `MFO-Progression-Addon-v1.1.3.zip` (updated 12597b ESL w/ the verdicts sub-FLST) attached. Commit `fc05931`, DLL `3bf46209`, CI `33471410623`. Contents (the post-v1.1.2 batch): (1) per-follower MFO enable/disable toggle (Followers-tab first-column checkbox, co-save FLWR v4→v5, releases held state on OFF); (2) residual #5 — perk-effectiveness verdicts became add-on ESL DATA (`g_verdicts` from manifest, -1 default, DLL holds zero verdicts; `WalkPerkEntries` = the general primitive); (3) #7 loot travel-pkg back-off; (4) LoS — inert OOC cast wall-gate seeded (`Want()`) + MFO's own raycast for cloth tents + height/stairs sampling. **FIELD-TESTED CLEAN on Tuxborn (`Tuxbornrc1`, the CURRENT test list — [[current-test-list-is-tuxborn-on-deck]]):** ~2hr session on the batch DLL, no CTD/error, all four features observed working (#7 strike back-off, `[los]` gate + fail-open fallback, `[hms-diag]` progression classify off the new ESL). **▶ NEXUS = marth's manual step:** upload the two v1.1.3 zips + paste the CHANGELOG v1.1.3 block; description bbcode still pending live-page text ([[bbcode-from-current-nexus-text]]). **3 CommonLibSSE-NG PRs OPEN** (#107 StartCombat, #108 ExtraDataList ctor, #109 SendInventoryUpdateMessage) against CharmedBaryon/CommonLibSSE-NG ([[commonlib-upstream-prs-coverage]]) — stay on CharmedBaryon, po3/dev is a NO-GO (per-runtime build-time model, breaks our single-binary NG design). **🔴 DEFERRED (marth "we defer this work"): 1.7.104 multi-version crash support** ([[multiversion-crash-root-causes]]) — the ecosystem is mid-churn (Steam label "1.7.99" = exe 1.7.104; Address Library/CommonLib catching up). Root causes found: overlay call-site trampolines (hardcoded offsets, 1.6.1170-only — the live 1.5.x/1.7.x crash) + `GetObject<T>` AE-offset poison (`Loadout.cpp:64`, gated off SE). Overlay VTABLE-rewrite (offset-free, the durable fix) is FULLY IMPLEMENTED but shelved on branch `worktree-agent-aae40b089815622ee` (not merged). Both game binaries saved under `binaries/` (gitignored) for a future diff. Resume plan in the memory. **Next threads (marth's pick): cut a version from the unreleased batch after field test / town update / assassin / resume 1.7.104 once upstream settles.**
>
> **Latest public:** v1.0.67 (https://github.com/marthofdoom/MFO/releases/tag/v1.0.67). **v1.0.68 = DEV BUILD ONLY, NOT RELEASED** — cut to `releases/v1.0.68/` (DLL `050b1c0c`, from green CI `32918264688`, commit ~`816521a` on main), local tag `v1.0.68` **NOT pushed, NOT published** (marth: "not a release"). Deployed to Tuxborn + custom-modlist (`050b1c0c`) for continued ECONOMY field-testing. Its economy is solid + tested (buy/sell autonomy, mage wardrobe by base value, role-aware shields on loot/buy/keep, gem conservation reconcile + "MEO Aware Followers" toggle, faster shopping, SOURCE-based signature-gear detection, Circlet-of-Light apparel blacklist, sell-worn-redundant convergence, `[sell]` diagnostic kept).
>
> **🚧 RELEASE BLOCKER = HMS.** HMS only REDISTRIBUTES the engine's per-level award, so it works for LEVELING/autocalc followers (Auri Ranged → all into Stamina, CONFIRMED) but does NOTHING for FIXED-STAT NPCs (Jesper) and reads the class from the PROGRESSION add-on, not the base Gambit tab. marth: **no public release until HMS is fixed.** See [[hms-v2-base-feature-and-fixed-stat-grant]].
>
> **▶ CURRENT DIRECTION (marth 2026-08-26): do v1.1 (the add-on-architecture blocker) FIRST**, HMS folded in. Full plan: `Docs/V1.1-ADDON-ARCHITECTURE-PLAN.md` (10 chunks, each CI-green + separately reviewable + low-token, one at a time, documented as we go; progression stays functional throughout). Three required outcomes: full removal of progression from the DLL, Vortex fix (self-contained manifest, NO MFO.esp master), progression add-on as the reference example. See [[addon-architecture-general-api-self-declarative]]. **PHASE 0 DONE (2026-08-26) — `Docs/ADDON-API-DESIGN.md` written, awaiting marth's approval.** Audit headline: the strangler is ALREADY ~70% done — discovery + class defs + the whole economy are already manifest-declared by editor-id suffix (no fixed IDs). Residual C++-baked design = 4 values (HMS ratios, `kEntryPoints[92]`, skill-name list, board tab + view contract). **Both open calls resolved with evidence in the doc:** manifest = ESL records (already is); Vortex fix = swap discovery off the MFO.esp `g_addonSentinel 0x803` reference to an editor-id suffix match (one place) → no MFO.esp master. Order = thin-API-slice (`GetBaseClass`/`Get/SetHMS`/`MeasureEngineVitalAward`) → **HMS base-feature + class-read fix (unblocks the blocker)** → fixed-stat grant → then bulk generalization. HMS crux confirmed: it reads add-on `clsId` (0/wrong when ESL didn't resolve), must re-root on base `combatClassOverride` (State.h:82). **Phase 0 APPROVED (no-host-default governing rule + host-provided board-tab creation). Phase 1a MERGED** (run 32932287929, sha f8df9494): seeded the general follower API in `MFO::Followers::` (`Get/SetBaseClass`, `Get/SetFollowerHMS`, `MeasureEngineVitalAward`), all sites byte-identical. **Phase 2 (unblock B) MERGED (2026-08-26, CI-green run 32938446204, sha fdec7008): ROOT CAUSE = GLOB editor-ids are discarded at runtime, so `ClassDef::stance` (matched by `_Stance` editor-id suffix) always parses 0 → HMS skipped + `SetClass` wrote `combatClassOverride=0` (Auto) → melee. Fix: HMS stance now from `Followers::GetBaseClass` (base Gambit class); `SetClass` stops clobbering the base class (Gambit tab = stance authority); + `[hms-diag]` line. Same bug kills economy GLOB knobs (why INI seeds were needed) — deferred (A). NOT deployed.** ⛔ **NO field testing until the ENTIRE v1.1 pass is done (marth 2026-08-26) — one test pass at the end, not per-phase.** **Phase 3 (fixed-stat HMS grant, RELEASE BLOCKER #2) MERGED (2026-08-26, CI-green run 32990827231, sha da3ffed8, main `91e871f`):** fixed-stat NPCs (0 engine award) get a per-level HMS grant once the player's total HMS catches up, reshaped by base-class ratio via the converging allocation, fractional-remainder carry, robust 2-level detection. Co-save PRGN **v5→v6**: dropped the derived `hmsTarget` (recompute on load), added `g_playerHmsTotalLast` global + `fixedStat` flags-bit 0x20 + per-follower `hmsZeroAwardStreak`/`hmsGrantRemainder`/`hmsAwardAccum`; v5 reader kept (v1.0.68 dev saves load). **Adversarial save-safety review: v6 format SAFE; 3 findings fixed** (SEV-2 serialize hmsAwardAccum else save/load double-dips leveling followers; SEV-3 range-clamp remainder; SEV-3 clear fixedStat on poisoned payload). **BOTH RELEASE BLOCKERS (Phase 2 + 3) now resolved in code.** **Phase 4 MERGED (CI-green run 32995756042, main after `71b84ca`): generic manifest model + reader + VORTEX FIX complete** — discovery self-declares via the add-on's OWN keyword (`MFOP_MFOAddonManifest`, edid-suffix; keyword edids survive runtime, GLOB/FLST don't), progression ESL masters ONLY Skyrim.esm (MFO.esp master DROPPED, own-prefix 0x02→0x01), 0x803 sentinel retired; hmsWeights lifted as add-on data. **EFFICIENT WORKFLOW (marth, low weekly budget until Sun): ONE persistent builder agent carries context across Phases 4-9; light diff checks; heavy save-safety review only at Phase 8.** **Phase 3 BACKFILL fix MERGED (2026-08-26, CI-green 32998471194, main `0a3a191`): retroactive = backfill** — fixed-stat grant now targets the shortfall to `max(0, playerTotal−npcTotal)`, so an existing fixed-stat follower jumps to match the player's total in one shot at activation then tracks per-level (no co-save change). **DEPLOYED to Tuxborn (marth "push to tux", 2026-08-26): FULL v1.1 main — DLL `d39ae1d1` + regen MFO.esp `864a9534` + MFO_Progression.esl `55ec90e6` + SEQ `e0b8cc57`, ALL deck shas verified.** First in-game v1.1 run — WATCH: progression still detected (`[prog] addon manifest … registered`, the discovery-swap risk), existing class/perks survive the ESL FormID shift, `[hms-diag]` baseClass=3 for a Gambit-Mage, fixed-stat `backfill budget` jump. custom-modlist still on v1.0.68 dev `050b1c0c`. **NEXT (Sunday reset / marth's word): Phase 5 (host MCM/Config from manifest — light) → 6 → 7 → 8 (heavy co-save review) → 9 (acceptance test + cut). Efficient shared-context-builder workflow.** END-OF-PASS test for Jesper still pending (set Mage on GAMBIT tab → `[hms-diag]` baseClass=3 + fixed-stat grant once player catches up). **v1.1 PHASES 0-9 COMPLETE (2026-08-30). Phase 9 acceptance-test audit DONE (`Docs/V1.1-ACCEPTANCE-AUDIT.md`): VERDICT = PASS-with-noted-residuals.** DLL is a general add-on host; delete the manifest → zero LIVE progression behavior (discovery off + one log line, catalog skipped, every alloc verb refuses "addon absent", zero hosted board tabs, co-save writes header+count=0, no crash). 4 residual verdicts: (1) `AddonVerb`→backend dispatch = KEEP (general host plumbing); (2) board tab body/view payload = KEEP (general widgets fed by data, zero progression content; soft residual = widget composition still hardcoded not manifest-declared); (3) `kEntryPoints[92]` perk-verdict table = the ONE compiled-in progression JUDGMENT, recommend SPLIT (keep the walk as engine fact, lift verdicts to manifest `entryPointVerdicts[]`) — FIX post-v1.1 LOW, INERT when add-on absent (gated on `g_detected`), NOT moved now (big generator change held for marth); (4) sweep = clean except the `MFO_Progression.esl/.esp` save-compat literals (KEEP, #12 frozen reader) + cosmetic chrome captions. Docs-only, no native change → no CI. **✅ QUICK TEST PASSED + v1.1.0 SHIPPED on GitHub (2026-08-31):** https://github.com/marthofdoom/MFO/releases/tag/v1.1.0 — tag pushed, DLL `8f45ac6c`, `MFO-v1.1.0.zip` + self-contained `MFO-Progression-Addon-v1.1.0.zip` (attach the addon so the new keyword-discovery DLL finds it). Field-validated on Tuxborn (progression detected via `MFOP_MFOAddonManifest` keyword, baseClass correct, fixed-stat detected, no crash, 505 clean log lines). A 4-CHUNK pre-cut review (co-save / HMS / discovery-manifest / board-API-config) all returned SHIP, zero blockers. v1.1.0 folds the v1.0.68 Town Update Alpha economy. **NEXUS deferred to v1.1.1 = the SUMMON FIX** (that's the public Nexus release; finalize the Nexus bbcode v1.0.67→v1.1.1 then). **▶ IN FLIGHT: the summon duration-tracking fix** ([[summon-duration-tracking-bug]], → v1.1.1) — per-spell active tracking keyed on the summon's LIVE commanded actor (summons spam because a summon isn't a caster-side effect; candlelight is fine). THEN the Fable split pass ([[modularize-giant-files-split-pass]]) → town → assassin. Post-v1.1 LOW residual: `kEntryPoints[92]` perk-verdict table → split (walk = exposed general API, verdicts → ESL data). (Candlelight recast-gap on Jesper 2026-08-31 was a FALSE alarm — 270s recast is by design, it was dark, it re-fired correctly.) **✅ FABLE SPLIT PASS COMPLETE (2026-08-31, main `d26995e`): the top-four giants modularized, ≤2500-line HARD CAP now holds codebase-wide** ([[modularize-giant-files-split-pass]] + CLAUDE.md rule). Logistics 5557→core 1611+Loot 2142+Economy 1034+Cast 268; Board 3478→2190+Progression 1234; Actuation 2605→core 1244+Direct 1309; ProgAllocator 3268→core 2333 (co-save stays whole, VERIFIED byte-identical)+Hms 384+Manifest 485. All pure moves, CI-green, shared state in `*_internal.h` as `inline` (the LNK2005 ODR gotcha). Packages 1657/CasterConsent 1066 deferred (already under cap). **▶ IN FLIGHT: the summon duration-tracking fix** ([[summon-duration-tracking-bug]], → v1.1.1 = Nexus release) on the now-lean Logistics — per-spell active tracking keyed on the summon's live commanded actor. THEN → town update → assassin. **✅ v1.1.1 CUT + GH RELEASE LIVE (2026-08-31): https://github.com/marthofdoom/MFO/releases/tag/v1.1.1** — DLL `045cf0db` (field-tested source `18aaf103`, same tree `58b73db`; sha differs only as non-reproducible-build noise), both zips attached (main + self-contained progression addon). Summon spam guard covers OOC (Logistics ServiceFollower) AND combat (Actuation::Fire transparent NoOp), `Actuation::CasterHasLiveSummon` per-spell + live-actor-keyed, non-summon casts byte-identical. Field-confirmed OOC (marth: "looks good"); combat untested in-game but same helper. **v1.1.1 = THE NEXUS RELEASE** — folds v1.0.68 alpha + v1.1.0 (HMS blockers, Vortex fix, add-on architecture) + this summon fix. **▶ NEXUS UPLOAD = marth's manual step:** upload `MFO-v1.1.1.zip` as main file + `MFO-Progression-Addon-v1.1.1.zip` as optional; paste the CHANGELOG v1.1.1 block into the Changelog tab. DESCRIPTION bbcode: rebuild from the LIVE Nexus page text ([[bbcode-from-current-nexus-text]]) — a LOT changed since the last Nexus version (economy, HMS/class growth, add-on architecture, Vortex fix, summon fix), so the description warrants an update; needs marth to paste the current live text. **NEXT after Nexus: town update → assassin.** **✅ v1.1.2 CUT + GH RELEASE (2026-08-31): https://github.com/marthofdoom/MFO/releases/tag/v1.1.2** — DLL `5d5d87dd`, fixed ESP `a3a003a7` + ESL `1308bbaa`, both zips attached. FIX: the ESP/ESL generator emitted QUST/MESG subrecords OUT OF ORDER (game-lenient, so it played fine, but xEdit/Vortex/Synthesis reject it — a user hit an xEdit crash on v1.1.1). Corrected to Skyrim.esm's real order (QUST: `EDID VMAD FULL`; MESG: `EDID DESC FULL INAM DNAM`) in both plugins; recorded as [[modularize-giant-files-split-pass]]-adjacent **INVARIANT #75** (Docs/INVARIANTS.md + TOOLING.md). Engine-invisible change → gameplay byte-identical to field-tested v1.1.1; VERSION-stamp-only DLL diff. Deployed to Tuxborn (all shas verified) for a belt-and-braces load-confirm. **v1.1.2 SUPERSEDES v1.1.1 as THE Nexus release** — marth uploads v1.1.2 zips + the v1.1.2 changelog block; description bbcode still pending live-page text.
>
> **▶ IN FLIGHT (2026-09-02, branch `feat/apmf-cast`, NOT merged — field-test first): APMF Phase 3, the OWNED CAST MODEL — MFO+APMF own the whole hostile cast gambit (target+spell+trigger), and this is how MFO FINALLY gets the FULL CAST ANIMATION.** First field test (Jesper 0x750012C6) proved cast-SELECTION works: the AI fired the gambit's Lightning Bolt **animated** ("THE ANIMATED PATH") — but it CONTENDED with MFO's AI-first-wait + force-on-miss package hybrid (log thrash "held off waiting" / "FORCED grace elapsed" / "cast package busy" = inaction), and cast-select owned the SPELL not the TARGET. **marth's decision:** own the gambit fully; the animated AI path is PRIMARY, unanimated `CastSpellImmediate` force is a rare last-resort; keep the old hybrid behind a legacy MCM toggle. **APMF side (commits `ba958e2` ABI v2 + `03032b3` combat-target HOLD, both CI-green on `main`):** ABI **v2** = `RequestEx(actor,intent,basis,const APMF_Param{form,fval,ival}*)` (prefix-extension `APMF_API_v2`, `kABIVersion=2`); cast-select uses `param.form` (spell); **combat-target now HOLDs** the client's `param.form` target — `Tick` re-asserts `StartCombat` only on `currentCombatTarget` drift (known-incomplete #2 PIN-gap fill, near-zero when holding). **MFO side:** `native/APMF_API.h` byte-identical (frozen contract, CLAUDE.md #5); `APMFBridge` (`native/APMFBridge.{h,cpp}`, atomic iface ptr, Win32 hand-declared) queries APMF at kDataLoaded, **null-degrades** to the legacy hybrid. `Actuation::CastOn` FF-non-self **hostile** branch (`ClassifySpell==Offense`, foe target) now runs the **owned model**: `APMFBridge::OwnHostileCast(follower,spell,target)` engages cast-select + combat-target HOLD, consent lets the AI fire it **ANIMATED** (primary, opaque "owned cast: AI casting"), and ONLY if the AI misses the grace window does it fire ONE bounded `CastTargetDirect` (unanimated, logged `owned FALLBACK`) — **no package/grace churn, no inaction.** **This RESOLVES the long-deferred cast-animation gap ([[cast-animations-deferred-to-post-town-polish]])** — casts animate because the AI fires them. Concentration stays MFO-bounded-direct (exact-bounding invariant — an AI channel can't be bounded). Lifecycle = refresh-per-tick + per-pump expiry (`APMFBridge::Tick`, 500ms) + `ClearTransientState` at kPreLoadGame. **NEW MCM key `bLegacyCastHybrid`** (default 0 = owned model; ON or APMF-absent = original hybrid; frozen-key #37; wired Config.h/cpp + EnsureMcmDefaults + out/MCM config.json + Settings ini). Also gated by `bApmfCast` (INI, default ON). **NO save/co-save change; frozen contracts + exact-bounding intact.** CI: MFO branch green (run below). **This establishes the "own the gambit" pattern (target+spell+trigger via APMF) — extends to other gambits (attack/flee/town-nav) later.** **FIELD-TEST on Tuxborn BEFORE merge** (deck needs APMF.dll = APMF `main` `03032b3`): a "cast X at nearest enemy" gambit → follower plays the FULL cast animation, casts X at the nearest enemy, no inaction, `owned FALLBACK` rare/never; `[ch.6] … HOLD` + `[ch.8] … selection` in APMF log. Flip MCM `bLegacyCastHybrid` ON → reverts to the old (unanimated) hybrid. Do NOT merge/cut until field-proven. **▶ LIFECYCLE REWORK (2026-09-02, marth's correction + review fixes — APMF `3353313` ABI v3 + `c33464e`, MFO branch updated):** releasing the combat-target mid-battle was the wrong operation — a rule/directive transition (cast→melee, new nearest foe) is a TARGET CHANGE, not a release. Now the two claims are DECOUPLED: **cast-SELECT = per-cast** (crisp release on `!castSeen` via `ReleaseCastSpell`), **combat-TARGET = per-COMBAT** (created by the cast directive, RE-POINTED in place via new APMF **`Repoint`** ABI v3 — same handle, no release/re-request — when the foe changes; the ATTACK directive re-points it too (create=false, never creates for pure-melee); kept alive every in-combat tick by `RefreshCombatTarget` ← Scheduler; released ONLY at combat end via expiry). APMF `CombatTarget::Release` now RELINQUISHES (no `StopCombat` — INVARIANT #5a: a steer channel relinquishes, does not undo a live engine decision). Net: mid-battle = re-point; release ≈ combat-end only. **Field-verify:** a cast→melee transition mid-fight emits a combat-target RE-POINT (`[ctl] … REPOINT`) or no change, NOT a Release. Also fixed: negative-basis owner-seed in ApplyRequest. Deck needs APMF `main` `3353313` (ABI v3). **▶ MODERATOR REDESIGN — the CTD reframe + the definitive model (2026-09-02; APMF `main` `8fa8a69`, MFO branch updated; CI-green both).** A field CTD (AV inside engine `StartCombat`, from APMF ch.6's executor) exposed a CATEGORY ERROR: APMF was EXECUTING behavior (StartCombat, cast injection) when it must only MODERATE. marth's binding model: **MFO makes proper behavior; APMF makes it WIN / reach the actor; APMF NEVER generates behavior.** APMF now DENY/ARBITRATE-only — **ch.6 combat-target + ch.8 casting are arbitration-only** (record the owner; zero engine calls — no StartCombat, no `selectedSpells`, no `currentCombatTarget`; the StartCombat crash class is now structurally impossible). MFO EXECUTES the REAL animated cast ITSELF with its OWN proven mechanisms — the cast MFO could always do (equip + `CasterConsent::Want` + a Cast-biased combat style → the follower's own AI DECIDES to cast, full animation, ENGINE_NOTES §0.15a/§0.27/§0.28) — now MINUS its old movement cost: the animation never needed the rooting UseMagic package (that was only the legacy force route), so the owned path DROPS the package and the follower keeps kiting while it casts. `Actuation::CastOn` owned branch: `APMFBridge::ClaimCasting` + `ClaimCombatTarget` (arbitration) → `SelectCasterSpell` (MFO's own `selectedSpells` write) + `Targeting::Command` (MFO's own `currentCombatTarget`) + consent + Cast-style → `{NoOp,"owned cast: AI deciding (animated, mobile)"}`. **NO `CastSpellImmediate` on the primary path** (force + the rooting package survive ONLY behind `bLegacyCastHybrid`). **GRANULAR: the owned path claims ONLY cast + combat-target — never movement**, so kiting continues. **DOCS made authoritative to deny-only in the same change:** APMF design.md §1a (binding contract), INVARIANTS #0 (no behavior-generating calls, cites the CTD), CHANNEL-MAP (PROMOTE→"client drives it"), ARCHITECTURE/MAP/README/STATUS; MFO MAP/CAST-DELIVERY/STATUS. **AI-DECIDES confidence:** the decision driver is the Cast combat-style (raises magic score) + `CasterConsent::Want` (permit wanted, deny competing) — MFO's existing, field-observed animated-cast machinery; if a melee-biased style still won't decide, the clean fix is more magic-score bias (the inverse of a deny), never force — surfaced, not papered over. **FIELD-TEST:** mage follower casts the RIGHT spell at the RIGHT foe, FULL animation, AI-decided, WHILE STILL MOVING (no freeze); no StartCombat CTD; APMF log shows `[ch.6]/[ch.8] … CLAIMED` (arbitration), no APMF combat/cast call. Deck needs APMF `main` `8fa8a69`.
>
> **▶ IN FLIGHT (2026-09-03, branch `feat/apmf-cast`, NOT merged — marth reviews before deploy, then field-test): APMF PASS B — loot-travel routes through APMF ch.9/ch.7 (the Cicero fix).** APMF's Pass A (branch `feat/allowance-channels-t1-t3`, CI-green) graduated two probe-proven channels into real API channels: `kIntent_OfferPackage` (ch.9, T3 0x49 — CLAIM the package-offer facet, `param.form` = the TESPackage FormID to hand the actor DIRECTLY via `Actor::CheckForCurrentAliasPackage`) and `kIntent_CombatAction` (ch.7, T1 — DENY combat behavior-tree leaf CATEGORIES, `param.ival` = a `CombatActionCategory` bitmask). MFO mirrors `APMF_API.h` byte-identical (append-only: the two new intents + the enum) and adds `APMFBridge::OfferPackage`/`ReleaseOfferPackage` + `ClaimCombatActionDeny`/`ReleaseCombatActionDeny` on the SAME claim-map machinery the owned cast uses (`EnsureClaimLocked`, now also an ival twin `EnsureIvalClaimLocked`; both claim functions return `bool` = is the claim now LIVE). **THE HEADLINE: `Packages::LootTravelFill/Retarget/Clear/EvictIf` now ROUTE THROUGH `OfferPackage`** — 4 new `MFO_APMFLootTravelPackage0-3` records (`Forms.h` `0x836-0x839`, `MFO_GenerateESP.py make_apmf_loot_travel_package()`, `out/MFO.esp` regenerated + `audit_esp.py` PASS) authored PLDT type 0 ("Near Reference") instead of an alias, because APMF's 0x49 hook delivers with NO alias fill — `Packages::SetAPMFLootTravelTarget` writes a runtime `ObjectRefHandle` into `PackageLocation::data.refHandle` (the Location twin of the existing `ReadTarget`/`SetInputs` PTDA mechanism, generalised via the SAME `kPointerOffFromIPackageData` offset model — **UNVERIFIED IN THE FIELD for a Location input specifically**, guarded identically: any layout mismatch declines loudly, never a blind write). This is the fix for a follower package-locked by an outranking custom AI framework (Cicero) that MFO's own `MFO_LootQuest` alias/static-priority-60 race could lose (the exact case the PACKAGE-THEFT guard, `Logistics.cpp:917`, reactively limps around today). **APMF SHOWPIECE PRINCIPLE (marth's binding correction, 2026-09-03): with APMF present, MFO is COMMITTED to the APMF path — never a decline-fallback to a legacy/pre-APMF route.** A failure on an APMF-committed path (unresolved package record, a layout-guard decline, or APMF itself refusing the claim — e.g. a lost arbitration to a higher-basis client) is logged LOUDLY and FAILS CLOSED (no travel dispatched this tick; the caller's existing arm's-reach fallback is the only degrade) rather than silently reverting to the alias route and masking an APMF-path bug behind a false "it worked". The legacy alias route runs ONLY when APMF is entirely ABSENT (or the `bApmfLootTravel` INI kill switch, default ON, is off) — the sole reason it still exists. Documented as a standing principle in `Docs/CAST-DELIVERY.md` (governs every APMF-client path in MFO, not just casting) and `MAP.md`. `Packages::Pump()` now refreshes any live APMF loot-travel claim UNCONDITIONALLY, first, every tick (~133ms) — well under APMFBridge's 500ms expiry backstop, since Fill/Retarget only touch the claim at excursion-start/leg-boundary, not every tick mid-walk (the same starvation lesson the owned-cast dedupe-latch revert taught, applied up front here). `ClaimCombatActionDeny` is built but NOT wired into the loot-travel dispatch (marth: "keep it scoped") — MFO's PACKAGE-THEFT guard already concedes loot-travel to a live combat package on purpose, so a blanket offense-deny would fight that existing design. **Both repos CI-green.** **▶ FIRST CICERO FIELD TEST (2026-09-03, deck Tuxborn) — BUG FOUND + ROOT-CAUSED + FIXED from the raw MFO.log (pulled directly via SSH, not paraphrased):** every dispatch logged `[warning] [loot] <id>: APMF loot-travel package write failed (slot N) -- falling back to the alias route` (still the pre-refinement wording; this run predates the commit-don't-fallback push), which fell back to the alias route and LOST to Cicero's framework package exactly as designed to avoid (`curPkg=0009BE51 onTravelPkg=false`, distance frozen ~2800 — never walked). **ROOT CAUSE, proven by `FindInput`'s own one-shot both-maps-miss dump right above the warning:** `kInputLocation` was authored as `"Location"` — the ANAM TYPE-STRING, not the template's BNAM-declared HUMAN PARAMETER NAME `FindInput` actually searches for. The dump read the vanilla Travel template's (`00016FAA`) real 3 parameter names verbatim: **`"Place to Travel"`** (uid 0, the Location input), `"Ride Horse if possible?"` (uid 2), `"Prefer Preferred Path?"` (uid 4) — an exhaustive 3-of-3 miss on `"Location"`, so the runtime write NEVER RAN (declined loudly before ever touching memory, per the guard discipline — not a crash, not silent). **FIXED:** `kInputLocation = "Place to Travel"sv` (`Packages.cpp`, one string constant; no ESP/generator change — the ANAM authoring was already correct). The `PackageLocation::data.refHandle` write itself (the offset-model risk) is THEREFORE STILL UNVERIFIED — this only clears the parameter-name miss standing in front of it. **Per marth's explicit ordering: the routing-commit refinement is now ALSO live (the earlier push), so the NEXT Cicero test exercises BOTH the fixed write AND the no-fallback commit in one pass** — watch for the runtime write to succeed (no more "package write failed") and Cicero to actually walk to and loot an item via the APMF-delivered package. **Both repos CI-green; MFO commit pending marth's review, then redeploy for the retest.**
>
> **▶ IN FLIGHT (2026-09-01, branch `perf/loot-stall-econ-pass`, NOT merged): the loot perf/stall pass** — marth's "followers do nothing for minutes" stall root-caused (off-navmesh pre-gate stall-strikes 5-min-stickied reachable loot). Fix = UNIFIED LOOT-FAILURE MODEL (see MAP §4 bullet): failed-targets-sort-LAST (never removed), per-ref GROWN grab radius 200+100/fail cap 600u (bodies only, dibs/bubble/leash still gate), off-navmesh pre-gate transient-only, sticky 5min→60s. Plus Balanced cadence: in-reach DRAIN-ALL per tick (StripCorpse from LootNearby, multi-source), kArrivalDist 160→200, fBatchLinger 4→1.5s (ms-precision linger), econ cooldowns scan 6→2s / trade 20→8s / pair 20→12s. kMaxLootSlots stays 4 (6 needs ESP alias pairs + travel PACKs — deferred). Awaits marth's Tuxborn field test before merge.

> - **⚠️ CAST TESTING IN PROGRESS (2026-08-19 eve) — DEPLOYED-FOR-TEST, NOT MERGED.** The long-cap build `b531305`/`344044b7` field-FAILED (candlelight fan starved mid-party via magicka over-drain from long caps; heal wouldn't stop; a HARD FREEZE mid-loot-scan). Fable review OVERTURNED the first diagnosis (it is NOT the g_active concurrency race — pump is serial). Root causes found: (1) heal gambit "HP AT OR BELOW X%" at X=100 is ALWAYS true -> never stops (marth's find); (2) long caps over-drain magicka -> starve the FF fan. THREE FIXES on branch `worktree-agent-a622d046b08b6aefc` HEAD `2302ead` (GREEN CI `32324225391`): (a) `Vocab::kHealFull`=0.9995 clamps the heal threshold top so 100% means <100% (Evaluator PickAlly/ConditionTrue + CastAuto + the stream heal-full mark, unified); (b) magicka-out held-stop in both reconciles (channel ends when caster can't afford the next beat -> makes long caps safe, un-starves the fan); (c) long randomized caps KEPT. **DLL `0504d276`** deployed to local mod for test (NOT merged; field test gates merge) — RE-VERIFY deck sha==`0504d276`. FREEZE: audit found NO unbounded per-beat accumulation -> likely SEPARATE; watch for recurrence. FIELD-WATCH: heal stops at ~full & no re-fire; held cast ends when dry; candlelight fan lights WHOLE party; freeze gone-or-not.
> - **⚠️ CAST TEST v2 (2026-08-19 night) — DEPLOYED-FOR-TEST `45b0ed1e` (from `6578035`, GREEN CI `32326353689`), NOT merged.** ROOT CAUSE FINALLY FOUND (marth's slot insight): the `kTargetActor` proxy cast creates a REAL ENGINE-SUSTAINED concentration channel on the follower that drains per-second on its own; MFO only dispelled the TARGET AE, never stopped the CASTER channel -> 2nd heal re-entered the 1st's still-live channel (runaway, un-stoppable by AE-dispel; 'works once fails twice'); the per-second drain had no FORCE-CAST line (it was the engine channel); and `Configure` overwriting a form whose channel was still live = the FREEZE. FIX (`6578035`): ConcProxy owner-keyed slots (Slot{form,source,owner}, Configure ONLY when free, Acquire reuse-owner/claim-free/else-nullptr-skip); `InterruptCast(false)` the follower's kInstant caster on EVERY stream release (TargetCastEndActor+SelfCastEndActor); every release a true END (dispel+interrupt+free); AUTO does not proxy conc-Self (no slot leak); + RELEASE/slot breadcrumbs. Kept long caps, 99.95% boundary, magicka-out. RE-VERIFY deck sha==`45b0ed1e`. FIELD-WATCH: `stream RELEASE (heal-full|magicka-out|cap|stale|gone)` every end; balanced `proxy slot ACQUIRE/FREE` (never 2 ACQUIRE w/o FREE); magicka STOPS bleeding after RELEASE; candlelight whole party; no freeze. RESIDUAL (field-only): if magicka still bleeds after RELEASE, InterruptCast didn't stop the engine channel -> next lever = caster-dispel / null-spell cast (scoped).
> - **✅✅ CAST-DELIVERY SAGA RESOLVED + MERGED + FIELD-CONFIRMED (2026-08-19 night, main `9733722`, DLL `45b0ed1e`).** Deck log confirmed: 3 sequential heals each `ACQUIRE -> 2 beats -> RELEASE (heal-full) -> FREE` (stops EVERY time, not just the 1st); magicka regens between heals (no runaway drain -> InterruptCast genuinely stops the engine channel); Candlelight fans 3/3 targets 0 skipped (whole party, no starvation); NO freeze. Final model = baseline `CastSpellImmediate` delivery + ConcProxy (concentration-Self-off-self ONLY, owner-keyed slot-for-duration, InterruptCast on every release) + long randomized caps + 99.95% heal boundary + magicka-out. FF/light/self-cast = baseline. Docs: CAST-DELIVERY.md authoritative. **REMAINING TO CUT v1.0.65:** (1) bundle the clean power-attack fix (`0d487e8`) + deck-test the power-attack behavior (never field-tested); (2) rewrite the stale CHANGELOG v1.0.65 entry + Nexus bbcode (1.0.62->65); (3) `./release.sh` Phase 2 (package + tag) — HELD for marth's explicit go.
> - **▶ CAST + POWER-ATTACK + ALLY-HEAL BUNDLED — main `2299569`, DLL `6e222dda` (GREEN CI `32328512145`) deployed-for-test.** Cast core FIELD-CONFIRMED already; this adds: (1) power-attack gambit (close-to-melee + swing only at the blocking foe, `0d487e8`); (2) AUTO ally-heal restored — serves ALL hurt allies sequentially (most-hurt first, one channel per caster, hysteresis `kHealSwitchMargin` 15% anti-oscillation) so Stenvar/teammates get healed, not just the player (`c3dfe1b`). RE-VERIFY deck sha==`6e222dda`. FIELD-WATCH: ally-heal streams cycle across BOTH player+Stenvar (`ACQUIRE->RELEASE(heal-full) tgt <each>`, no rapid flip); power-attack closes to melee then swings at the blocker; core cast still clean (every heal stops, no drain, candlelight whole-party, no freeze). If all pass -> CUT: changelog v1.0.65 rewrite + Nexus bbcode + `./release.sh` Phase 2 (HELD for marth's go).
> - **🎉 v1.0.65 CUT (2026-08-20) — packaged + local-tagged, NOT yet pushed/published.** `releases/v1.0.65/MFO-v1.0.65.zip` built from commit `ab0d045` / green CI `32383641531` (DLL `795e7fc3…`). Local tag `v1.0.65` created; **`git push origin v1.0.65` + Nexus upload = marth's manual step.** Contents: the concentration-heal overhaul (reliable party/ally/self heals, stop-at-full, magicka-out, no runaway/freeze — field-confirmed over a clean 1hr deck session), AUTO ally-heal (sequential most-hurt + hysteresis), power-attack gambit (close-to-melee, no longer experimental), self-cast, the 6 RC fixes, loot/economy fixes, the comprehensive review pass. NO save-format change. Progression ESL (MFO_Progression.esl) is GENERATED + audited but the progression addon is NEXT UPDATE (not the shipped zip's active feature). REMAINING: (1) rebuild Nexus BBCODE from the LIVE page (standing rule) — draft in the changelog agent's report; (2) marth pushes tag + uploads; (3) NEXT: the progression ESL addon (incl. the revert-engine-awards redesign + compat toggle, see [[next-update-follower-progression-esl]]) + its own BBCODE explaining the addon + addon API.
> - **▶ PROGRESSION ADDON — REVERT-ENGINE-AWARDS skill model BUILT (2026-08-20, worktree branch `worktree-agent-a13682140499e12fa`, NOT merged/no VERSION bump).** `ReconcileSkill` (ProgAllocator.cpp:298) now has TWO skill models behind the live MCM toggle `g_econ.cancelEngineAwards` (default ON, addon INI `bCancelEngineAwards` via `ApplyEconomyOverride` — mirrors the economy sliders, NO GLOB/NO PRGN/NO save-format change). **ON (new default):** `natural = enrollmentBaseline` — MFO reverts the engine's per-level/autocalc skill growth and applies ONLY its own award; the ~2s drift-watch clobbers engine gains each cycle (pure MFO, no inflation, base is permanent if MFO removed). **OFF:** byte-identical to the shipped adopt path (engine leveling + MFO stack). 7th control on the "MFO — Follower Progression" MCM tab (toggle, ModSettingBool); `tools/audit_mcm.py` PASSES (7 controls). Docs: FOLLOWER-PROGRESSION-ESL-DESIGN.md §4.2 + §18.6, MAP.md §5. FIELD-WATCH with toggle ON: `[prog] … skill X: base 28.0 -> 25.0 (natural 20.0 + alloc 5 …)` — base dropping BELOW `cur` proves the engine level-up gain was cancelled.
> - **🚀 v1.0.66 SHIPPED (2026-08-20) — GitHub Release live, both artifacts.** https://github.com/marthofdoom/MFO/releases/tag/v1.0.66 (tag pushed, `MFO-v1.0.66.zip` + `MFO-Progression-Addon-v1.0.66.zip` attached). v1.0.66 = the full follower overhaul (cast/heal rebuild + AUTO ally-heal + power-attack + revert-engine-awards + all v1.0.65 content) PLUS **follower economy default ON** and the 'count summon kills' toggle hidden. First public release (v1.0.65 was cut but never published). The **progression ESL add-on** ships as the separate `MFO-Progression-Addon` zip (MFO_Progression.esl + SEQ + MCM tab incl. cancelEngineAwards toggle). NEXUS: marth uploads the mod zip as the main file, the addon zip as an optional file, pastes `assets/nexus-description.bbcode` (newcomer intro, in-voice) as the description and the CHANGELOG v1.0.66 block into the Changelog tab. De-inflation is a one-time thing on marth's own save only (new users count clean from install). NEXT: (1) draft the 2 CommonLibSSE-NG PRs (CombatController AE-layout header fix [doc-ready, removes MFO's biggest fragility class + fixes an upstream bug] + Actor::StartCombat binding); (2) sandbox the new Skyrim exe (dropped 2026-08-20) for offset probing without breaking the install; (3) support the new runtime + 1170 (verify the 4 CombatController offsets + re-measure the 3 Board call-site VariantOffsets once Address Library publishes). See [[next-support-new-skyrim-version]].
> - **📦 PROGRESSION ADD-ON v1.0.1 PACKAGED (2026-08-20) — add-on-only, DLL unchanged (stays v1.0.66).** The add-on now carries its OWN version line (was shipped inside v1.0.66; this is the first independent bump). Change: default skill points per level **2 -> 5** for BOTH pools (`iSkillPointsPerLevel` + `iManualSkillPointsPerLevel`; GLOBs `MFOP_SkillPointsPerLevel` 0x803 + `MFOP_ManualSkillPointsPerLevel` 0x809 both 5.0). Safe now that **Cancel engine skill leveling** (default ON, revert-engine-awards) stops the full award from inflating. Also FIXED a source-of-truth gap: the `bCancelEngineAwards` toggle had been hand-added to `out/` bypassing the generator (working tree had even lost it) — it's now emitted from `MFO_GenerateESP.py` `PROG_MCM_TOGGLES`, so regen reproduces the exact 7-control config.json. `audit_mcm.py` + `audit_esp.py` PASS. Artifact: `releases/progression-addon-v1.0.1/MFO-Progression-Addon-v1.0.1.zip` (+ MANIFEST). NOT committed/pushed/uploaded (marth's manual step); NOT yet deck-deployed. NOTE: mod `VERSION` file stays `1.0.66`.
> - **🏗️ ECONOMY BUY + MAGE DRESS-UP — BUILT on branch `worktree-agent-a2f981e4c0ca69637` (2026-08-21), NOT merged, CI green (part-1 run `32499156185`; full build `32501980176` in flight).** The "two easiest parts of the town update" (marth). Extends the sell-only economy with BUYING on the existing TradeBridge/RunTrade path (NO new natives / NO .psc·.pex / NO ESP / NO save-format change): weapon+armor buy (best affordable vs owned baseline), spell-tome buy for casters (top-2 schools, unknown, ≤50% purse) + worker-side `LearnCarriedTomes` auto-learn, and a per-slot mage CLOTHING+JEWELRY dress-up (loot AND buy) via the unified MEO-aware `MageApparelBuyKey` (value-primary when MEO present + strict-off, else school-enchant primary; villain-robe blacklist w/ `IsNecromancerFollower` kReanimate exception). Bought/owned upgrades get worn by `EquipBestOwnedGear` (idle branch, worker, dolls-gated, idempotent) through the reused #62-safe `AcquireEquip` (MainThread::Post→EquipObject, NO DoReset3D) → buy-path `QueueGemMove` fires. Jewelry ACQUISITION stays on the stricter Valuables/dibs tier (dibs preserved); mages still WEAR best owned ring/amulet. **4 new MCM toggles** (Economy page, `audit_mcm` PASS): `bEconomyBuyGear`/`bEconomyBuyTomes`/`bMageWearRobes` (default ON), `bMageApparelStrictSchool` (default OFF). `combatClassOverride` unchanged (weapon selection only). Reviewed the shipped loot/equip diff = safety-clean. **PLUS College-of-Winterhold tome unlock** (`UnlockCollegeTomes`, 3rd commit `5cdb39e`): a 30s-rate-limited worker roster pass sets the vanilla `PC{School}{tier}` gate globals (`000F2584`..`2592`) to 0 when max party BASE skill meets Adept 50 / Expert 75 / Master 100 — one-way, batched via MainThread::Post, gated `bEconomy && bEconomyBuyTomes`, natural restock (no chest mutation). See [[college-tome-gate-globals]]. **DEPLOYED to `custom-modlist/mods/MFO` for Deck field test: DLL sha `2d9cb639…` (2003968 bytes) + config.json (4 toggles); INIs untouched (configs preserved).** Branch `worktree-agent-a2f981e4c0ca69637` has 3 commits (`b93cf46` buy layer / `60e28f2` equip+loot dress-up / `5cdb39e` College), full CI green (`32506112718`), NOT merged — pending marth's field verdict, then merge to main. Open follow-ups if field-approved: jewelry-looting-for-mages nuance (currently WEAR-owned only, acquisition stays on Valuables/dibs tier); toggles landed on MCM **General** page (cosmetic).
> - **🏗️ ECONOMY BATCH 2 DEPLOYED (2026-08-21) — DLL sha `92852b6a` (2014208 bytes) + INI SEEDS.** Branch `worktree-agent-a2f981e4c0ca69637` commit `2797576` (full CI `32512569343` green), NOT merged. Adds: **keepArmor logical-slot fix** (was bucketing by raw GetSlotMask so a mage kept every varied-mask robe and sold none — now keeps worn+best-per-LOGICAL-slot, sells extras); **coin-purse gold** (scan the item's own keyword editorIDs OCF_MiscTreasure_Coinpurse/_Coin instead of the reverse LookupByEditorID that returns null when KID mints the kw — TGCoinpurse*/modded bags loot now); **Merchant/Salesman perk bypass** (`g_merchantPerkID` default 0x00058F7A, OwnsExactPerk dual check base->GetPerkIndex||HasPerk → follower sells ANY item to ANY vendor; base VendorTrades whitelist was CORRECT); **Speech sell pricing** (vanilla curve fBarterMax 3.3→fBarterMin 2.0, receipt 30%@Speech0→50%@100; BUY untouched); **board spell-picker tooltip** (per-follower CalculateMagickaCost + synth effect desc) + **Followers-tab H/M/S** (snapshot-precomputed). INI-only config `bSpeechPricing`/`bMerchantPerkBypass`/`xMerchantPerkID`. **DEPLOYED the INI seeds** (MCM/Settings/MFO.ini + SKSE/Plugins/MFO.ini + config.json + MCM/Config/MFO/settings.ini) — marth confirmed "the ini seeds are needed" to LINK the 4 earlier toggles (bEconomyBuyGear/Tomes/bMageWearRobes/bMageApparelStrictSchool) to their active state. **DEFERRED to the NEXT build (marth: bundle it):** the Haggling/Investor **kModSellPrices perk PRICE boost** — read each owned perk's sell multiplier via the progression addon's CI-PROVEN perk-entry-data idiom (`Progression.cpp:218-231`: `entry->data.functionData.function.get()` / `.params[0]`; entry-point table enumerates ModSellPrices at `:126`), NO Synthesis. The agent deferred it only because IT couldn't verify EPFD layout offline — the proven read already exists in-repo.
> - **▶ RESUMED (2026-08-24) — batch 3 DEPLOYED, DLL sha `ba245ae0`.** Branch `worktree-agent-a2f981e4c0ca69637` HEAD `a9074b8`, full CI `32685574812` GREEN. This build adds: (1) the `[sell]` per-item sell-exclusion DIAGNOSTIC (rate-limited; logs the reason each weap/armo is dropped + whether the vendor VEND has VendorItemClothing) to finally pin why Lucien's extra clothing shows `sell n=0`; (2) ARTIFACT keep-but-equip (Catalog::IsExcluded — never looted/sold/shed/handed-off, but auto-equip made permissive); (3) `LookupAddonForm` `.esl`↔`.esp` sibling resolution (read-side, no PRGN format change). **NEXT: marth restarts Skyrim + trades Lucien near a clothing-buying vendor → I pull the `[sell]` log to pin the block, then fix.** VORTEX `.esp` addon PACKAGED + verified drop-in: `releases/progression-addon-v1.0.1/MFO-Progression-Addon-v1.0.1-vortex-esp.zip` (sha `8789c529`) — regular ESP (flags 0x0, masters Skyrim.esm+MFO.esp, no load-order cycle), works with current v1.0.66 for FRESH installs; generator now emits both `.esl`+`.esp` (uncommitted on main, ready). **STILL QUEUED:**
>   1. **Lucien clothing STILL not selling** (deck 92852b6a, `sell n=0`) — keepArmor fix is correct, so the block is stock/worn/socketed/excluded/vendor-VEND and the `[econ]` line doesn't itemize. NEXT: add rate-limited `[sell]` per-item exclusion diagnostics (header + per-dropped-item reason: stock/keepWeap/keepArmor/worn/socketed/excluded/vendor-filter incl. whether the vendor's VEND has VendorItemClothing 0x0008F95B), deploy, read the log to pin it. NOTE: Lucien was seen only at College vendors (Urag=books, Phinis/Drevis/Colette=spells); Drevis/Colette's VendorItemsSpells FLST DOES include VendorItemClothing (investigation-confirmed) so clothing SHOULD sell there — verify with the diagnostic which vendor + which reason.
>   2. **Haggling/Investor kModSellPrices perk PRICE boost** (deferred; marth: bundle next) — read each owned perk's sell multiplier via the CI-PROVEN progression idiom `Progression.cpp:218-231` (`entry->data.functionData.function.get()` / `.params`), entry-point table enumerates ModSellPrices at `:126`. NO Synthesis.
>   3. **NEW (marth 2026-08-23) — Taunt gambit:** a combat gambit that DETECTS JaySerpa's Taunt mod (provoke-enemies power/shout) when present and USES it; no-op when the mod is absent (detect the taunt form by editorID/keyword like the other optional-mod detections).
>   4. **NEW — Artifact handling:** artifacts must NOT be looted (already done), but a follower CAN equip them, and they must never be dropped or handed off to the player (stock-like protection that still allows equip). Identify artifacts by keyword (VendorItemDaedricArtifact / artifact keywords).
>   5. **NEW — Vortex + ESL reciprocal-dependency bug:** MFO_Progression.esl masters Skyrim.esm + MFO.esp; in Vortex a reciprocal dependency prevents actual usage. Investigate the ESL master declaration in `MFO_GenerateESP.py` (does the addon strictly NEED MFO.esp as a master? the AddonManifest FLST references an MFO.esp sentinel keyword — see if that can be self-contained / the master dropped) and/or provide Vortex load-rule guidance.
> - **📋 FIELD-SESSION QUEUE (2026-08-24, Jesper on Tuxborn) — authoritative open list on branch `worktree-agent-a2f981e4c0ca69637`:**
>   1. **IN FLIGHT — socketed-sell fix (DONE, uncommitted) + double rapport growth (agent building).** `[sell]` diagnostic proved the sell-block was a bare-`ExtraUniqueID` over-fire (Tuxborn tags nearly every item; items keep the uid after MEO moves gems out) → follower sold nothing. REMOVED the bare-uid block (worn/best-per-slot gems still protected by worn+keepWeapons/keepArmor). Rapport growth default DOUBLED. → commit both → CI → deploy to Tuxborn → re-test Jesper selling AT A CLOTHING-BUYING VENDOR (the log showed Ahlam/Carlotta have `buysClothing=false` — not all vendors buy clothing; clothes need a clothier/general vendor or the Salesman perk).
>   2. **HMS on level-up (marth: use the ENGINE'S existing framework, don't hand-roll).** Progression allocates ONLY skills+perks, never the H/M/S pools — so a "Mage"-set follower gains NO Magicka (his Magicka growth follows his VANILLA class weights; Jesper=Guard=no Magicka). FIX: map each progression class to a vanilla TESClass (Mage→a mage class) and set the follower's `npcClass` on enroll so the engine's class-weighted HMS distribution gives Magicka. Verify: SetClass save-safety + it triggers the HMS recompute + it doesn't wreck combat AI.
>   3. **GEM PROTECTION + MANAGEMENT (marth adding MEO API — `HasGems` etc. in CI; will update Tuxborn MEO on green). Three MFO layers, need the updated MEO_API.h to build (2)/(3):**
>      - **(GATES v1.0.67) accurate socketed-skip:** re-add the sell-block using MEO `HasGems(actor, itemBase, uid)` — block ONLY genuinely-gemmed items (not bare uids). Closes the Iron Dagger gem-loss the bare-uid removal opened (log 2026-08-24: Jesper's gemmed Iron Dagger sold at Ysolda, only the ring's gem moved, dagger's gem lost). DECIDED: equip-check-before-sale REJECTED as dangerous (temp-equip fires TESEquipEvents → triggers MFO's own QueueGemMove + #62 reattach; main-thread async per item; visible flicker). v1.0.67 waits for this so it doesn't ship the regression.
>      - **DEFAULT (on): fill empty sockets with class-appropriate gems** (mage→magicka/school gems, etc.) from the follower's loose gems.
>      - **MCM "MEO Aware Followers" (default OFF, seeded off): full gem-loadout management** — swap up to better/more-effective gems found, gem-XP/level aware. Needs MEO API: empty-socket query, socket-specific-gem, enumerate loose/available gems, gem effectiveness + XP/level exposure.
>      - (Feature A "extract-gem-then-sell + re-socket own slots" folds in as a later nicety once the management APIs exist.)
>   4. **Feature B — invisible dibs for enchanted/artifact/socketed loot.** These (currently loot-excluded) CAN be looted by followers (gambit-gated by loot gambits) but only after a STRONGER dibs — very clear the player was going to leave them.
>   5. **Feature C — loot batching.** All loot TYPES batched together, nearest-first, ONE pickup animation covers all nearby items tagged for looting (vs today's per-category passes).
>   6. **Taunt gambit** — detect + use JaySerpa's Taunt (provoke power) when present; no-op if absent.
>   7. **Haggling/Investor perk PRICE boost** — read each owned perk's kModSellPrices multiplier from `BGSEntryPointPerkEntry::entryData.functionData` (NOT the condition-FUNCTION_DATA idiom — that was the wrong type; needs a CI-verified `BGSEntryPointFunctionData` read). NO Synthesis.
> - **✅ v1.0.67 SHIPPED (2026-08-25) — GitHub Release live.** https://github.com/marthofdoom/MFO/releases/tag/v1.0.67 · tag pushed, `MFO-v1.0.67.zip` attached · commit `4e7acc71` · DLL `b5a210bc` · CI `32863275708` green · zip `releases/v1.0.67/MFO-v1.0.67.zip`. Nexus upload = marth's manual step (changelog block + zip). **Field-confirmed on Tuxborn (Jesper) before cut.** This ships the whole economy round: follower BUY (weapons/armor/gear + spell tomes, matched to skill, auto-learn), mage dress-up (school-matched robes/clothing/jewelry, `bMageWearRobes` on), College tome unlock by skill, board spell-picker magicka-cost tooltip + Followers-tab H/M/S, coin-purse gold, rapport doubled, merchant-perk bypass, speech sell pricing, artifact keep-but-equip, keepArmor logical-slot fix, and **ungem-then-sell** gem handling (MEO ABI v3 — extract gem to follower inventory, sell ungemmed next scan; v2 protects, cold-cache protects, <v2 none). **CLOSES field-session-queue item 1** (socketed-sell over-block was a bare-`ExtraUniqueID` over-fire; removed + rapport doubled) **and item 3's first bullet** (accurate socketed-skip → became ungem-then-sell v3). Merge `cf89e8e` (branch `worktree-agent-a2f981e4c0ca69637` retired).
> - **▶ NEXT BUILD = GEM AUTONOMY (field-session-queue item 3, remainder) + mage-clothing-oscillation fix BUNDLED (marth 2026-08-25).** Two tiers; the MCM toggle gates ONLY effect-awareness, not conservation. **(a) OFF / default — gem CONSERVATION, always on:** a follower who unsockets a gem (the shipped ungem-then-sell) must not leave it loose — if a piece he keeps/wears has an empty socket, fill it, reusing the loot-time socket-fill mechanism. TIMING CAVEAT (marth): buy/sell is looser than loot (v3 UnsocketGem queues to main, item sells a scan+ later after cache refresh, gem is loose for a window), so the re-socket must be a DECOUPLED RECONCILE PASS (each scan: match any loose previously-socketed gem against any worn/kept piece with an empty socket, fill it, independent of when the unsocket fired) — reuse loot's socket-fill CALL but drive it from the reconcile step, not the unsocket event. **(b) ON — "MEO Aware Followers" (default OFF):** effect-aware — knows gem effects, picks the best gem per slot, optimizes the loadout (effectiveness/XP/level aware), swaps up. Needs MEO v3 GetEmptySocketCount/GetLooseGems/GetGemDetails/SocketGem. **BUNDLED mage-clothing-oscillation fix:** Jesper (mage-set, robes on) won't sell a LOWER-value clothing item (Nord Tribal Cloth White) though his Khajiit outfit is worth far more — the engine periodically re-equips the Nord (default-outfit/auto-best-gear), and the sell loop never sells WORN gear (Logistics.cpp:3744) + keepArmor pins worn per-slot (:3646), so whatever is worn at scan is protected → never sells. FIX: make mage dress-up AUTHORITATIVE — force the single best per logical slot and force-UNEQUIP + sell the losers, so an engine-re-equipped lesser piece is corrected not protected (#62-adjacent equip surface — careful pass). Build worktree-isolated off shipped v1.0.67 main.
> - **▶ THEN HMS class-redistribution (v1.0.68) — DESIGN PASS DONE (2026-08-25), 4 decisions need marth before build.** Core: HMS is a SIBLING of the existing SKILL revert subsystem, not new machinery — build `RecomputeHMS`/`ReconcileHMS` beside `RecomputeSkills`/`ReconcileSkill` (ProgAllocator.cpp:429/:298), driven by the SAME main-thread poll + drift-watch (`PollWork` :1021/:1070/:1082) and the SAME enrollment-baseline pattern (`Enroll` :1719). **Fork resolved by sidestep:** don't measure the engine's HMS award — REVERT engine HMS to the enrollment baseline (like the skill path) then GRANT MFO's own class-% budget on top; autocalc + fixed-stat followers then behave identically. Class from `ClassDef::stance` (3=Mage 15/80/5, 2=Ranged 40/5/55, 1=Melee 60/5/35; no enum renumber). **Co-save: bump PRGN v4→v5** (Serialization.h:46), APPEND a 3-pool block (baseline/lastWritten/skew, fixed H,M,S order) at the END of each per-follower record, `if(version>=5)` guard, all v1–v4 readers untouched, byte-identical for old saves (single highest-risk edit). Skew = one-directional pull FROM class-primary TOWARD the off-class pool the gambit exercises; "a mage can't skew mage more" falls out because a mage gambit's pool == primary → shift=0. **DECISIONS LOCKED (marth 2026-08-25):** (1)+(4) budget is NOT fixed — **COUNT the total HMS points the engine actually awards that level (= what a vanilla follower gets) and redistribute THAT total by class %**; revert-and-grant APPROVED but must measure/count the engine's per-level award and use it as the budget (net: follower's TOTAL HMS gain == vanilla's, reshaped to 15/80/5 etc.). **Budget is a PER-MODLIST LIVE CHECK — NEVER a hardcoded constant (no "10/level"):** different modlists/leveling mods award different HMS-per-level, so the counted actual award is the only correct source; read it live every level. CONSEQUENCE: only produces HMS when the engine awards some (autocalc / player-level-scaled followers); a truly fixed-stat follower's counted total is 0 → 0 redistribution (== vanilla). Jesper gains Magicka because he's a leveling follower (engine awards H/S by guard weights, reshaped to give M). (2) skew scales with usage volume up to **20%** of the budget toward the off-class pool, with a FLOOR: any real usage grants **≥1 point** (a few spells → ≥1 Magicka). (3) usage metric = **% of battles SINCE LAST LEVEL-UP in which the off-class gambit fired** (a real per-follower counter reset each level-up — NOT a recency stamp; needs battle-enter counting + off-class-gambit-fired-this-battle flag, both SERIALIZED in the v5 block so they survive saves between level-ups). **DESIGN AMENDMENT vs the agent plan:** the HMS drift-watch does NOT blindly clobber like skills — it MEASURES positive engine drift (the award) at the level-up/drift edge, redistributes it, then holds target; capture the engine delta BEFORE re-asserting or it's erased. Between level-ups base==target (autocalc HMS only moves on level-up). MAP.md §1/§5 co-save table needs the v5 update (now also the battle counters) as part of the build. **REFINEMENTS (marth 2026-08-25, post-review) — fold into a follow-up RecomputeHMS pass AFTER the F2/F3/F4 review fixes land, re-review before CI (touches core math):** (i) **class % is a LONG-TERM cumulative target, not a per-level split** — each level allocate the budget to CONVERGE the follower's running-total HMS toward the class ratio (correcting past deviation incl. the pre-enrollment vanilla baseline that's off-ratio), so levels DIFFER and small corrections accumulate over ~10 levels; can't push a pool below its baseline, so it nudges/settles rather than snapping. (ii) **skew is SEMI-PERMANENT, gated on the off-class gambit being EQUIPPED + ENABLED** — persists while the gambit stays slotted and not disabled (magnitude from usage volume, capped 20%); unequip/disable it and the skew is no longer maintained and the long-term correction (i) pulls the ratio back toward pure class %. So the F3 gambit-fired mirror feeds skew MAGNITUDE; add the equipped+enabled state as the persistence gate.
> - **OPEN ISSUE (post-v1.0.66, non-blocking) — loot travel-pkg re-assert churn.** Deck log (release build 5b5c6943, 2026-08-20): `[loot] <id> travel pkg stolen mid-walk (curPkg=FF00XXXX) -- re-asserting claim, grace 10s` repeats constantly for BOTH followers (Lucien 3000591F + Stenvar 000B998C), every few seconds. INFO-level, NO crash / NO save impact / 0 errors, so NOT release-blocking. Mechanism: MFO's walk-to-loot AI package (static prio 60, [[check-engine-notes-measured-results]]) gets displaced mid-walk by another package (curPkg = a dynamic FF pkg) and MFO re-asserts its claim, looping instead of completing the walk. LIKELY FIX (claim-lifecycle, NOT a runtime priority flip per the standing rule): after N repeated displacements of the same claim, BACK OFF (abandon/defer that loot-travel) rather than re-assert forever; and/or suppress re-assert while the follower is in combat. DIAGNOSE FIRST: identify what `curPkg=FF00…` is (combat package / sandbox / another mod) — that picks the lever. Queued AFTER the 2 CommonLibSSE-NG PRs + the new-runtime support work.
> - **🚧 v1.1 BLOCKER (2026-08-20) — TRUE ADD-ON ARCHITECTURE, deferred TBD.** marth's ruling: the DLL must expose a WIDE GENERAL follower API and act as a general add-on HOST that reads an add-on's SELF-DECLARATION ("I am a progression tab for MFO, here are my features and design"; "I am a roster tab", etc.) — the add-on is not recognized by name/FormID, it declares itself. NO new Papyrus, and NO add-on-specific C++ in the DLL: delete the progression add-on and MFO.dll must lose ZERO lines. Today the progression LOGIC (ProgAllocator.cpp / Progression.cpp / progression tab in Board.cpp / progression MCM+Config globals) lives in the DLL, which violates this — v1.0.66's progression "add-on" is a config shell over DLL-resident logic, not an independent add-on. Because this was not understood/implemented for v1.0.66, marth made it a **v1.1 blocker, deferred until TBD.** Crux to design: is progression's logic (class weights, per-level allocation, scarcity pool, revert-engine-awards, perk-tree walking) expressible as DATA + event-rules over the general API? Where not, widen the API with a GENERAL primitive any add-on could call — never smuggle progression math back in. See [[addon-architecture-general-api-self-declarative]]. Re-run the classified DLL audit (GENERAL-API / ADDON-HOST / PROGRESSION-SPECIFIC) when 1.1 work begins.
>
> **▶▶ RESUME HERE (2026-08-19 late) — v1.0.65 STAGED (`f1110f2`, NO TAG); minimal cast rebuild MERGED + DEPLOYED for field test. Cut HELD.**
> - **✅ CAST REBUILD WORKS — FIELD-CONFIRMED, then stream-bounding added. main `a800b60`, DLL `344044b7` deployed.**
>   The concentration proxy CHANNELS the heal on the recipient (deck log 2026-08-19: `conc effect ATTACHED on
>   <recipient> … self->target proxy`, follower pays magicka, HP climbs) — the core unknown is SOLVED. Follow-up
>   (this build): concentration streams now bound by RANDOMIZED human-timed caps — **heal/utility 8–15s, offense
>   2–6s** (`DrawConcCap`, worker-serial mt19937, per-stream, not serialized) + healing stops at ~full recipient
>   HP (`kHealFullPct` 0.995). This RESOLVES the kHealFullPct decision (marth: yes stop-at-full, time-capped) and
>   the "won't stop when condition met" issue. **`Docs/CAST-DELIVERY.md` fully REWRITTEN** as the single
>   authoritative reference (model + delivery table + rejected approaches + scope-creep lesson); MAP.md cast
>   entries updated. `Actuation.h`/`Logistics.cpp` STILL byte-identical to baseline. DLL `344044b7` (green CI
>   `32320451089`, merged native tree == b531305) in local mod — **RE-VERIFY deck sha == `344044b7`**. FIELD-WATCH:
>   streams end within their band (varying durations), heal stops at full, no endless casting; Candlelight/flesh
>   all modes unchanged. Power-attack fix (`0d487e8`) still NOT bundled — deploy next round.
> - **(history) main previously shipped BROKEN cast delivery** (`2ad0c414`, wave-3 AddTarget — doesn't channel
>   concentration, Lucien healed nobody). REPLACED by the rebuild above.
> - **THE SAGA, RESOLVED TO A MINIMAL FIX (2026-08-19).** A ~1-day cast rework (AddTarget → target-self-cast →
>   force-sustain → 2-slot SPEL proxy) OVERREACHED: to fix ONE bug it rewrote the delivery of already-working
>   spells and chased its own regressions (player-mana drain, no-stop-at-full, FF-fan-lands-on-caster, SEV-1
>   light-respam CTD). A **read-only AUDIT** (agent, 2026-08-19) pinned it:
>   - **BASELINE = `00c6fce`** (last shipped-working build, `c875048`; before any rewrite). Delivers via plain
>     `CastSpellImmediate(sp, target, follower)`; has AUTO light-crash suppression; NO rewrite machinery.
>   - **The ONE genuinely-broken thing:** a **CONCENTRATION + Self-delivery** spell (Mysticism Fast Healing =
>     Conc+Self) aimed at a NON-SELF target lands its AE on the FOLLOWER (Self-delivery collapse) → player/ally
>     concentration heals silently fail. NOT a package-lock issue (fixed pre-baseline by `36231d7`); hits normal
>     AND locked followers equally. Everything else (Candlelight/flesh fanning, self-casts, normal heals) WORKED
>     at baseline — marth confirms Candlelight worked in all modes (individual/player/self/AUTO-all-in-dark).
> - **✅ DEFINITIVE FIX BUILT + REVIEW-CLEAN — branch `worktree-agent-a622d046b08b6aefc` HEAD `665be2c`, GREEN CI `32318675820`.**
>   Diff vs baseline is ONE FILE: `native/Actuation.cpp` +120/−12 (the `ConcProxy`/`DeliverySpell` proxy + 2
>   concentration-branch wire-ins + `TargetCastEndActor` proxy dispel + `ConcProxy::Reset()`). **`Actuation.h` +
>   `Logistics.cpp` byte-identical to baseline.** Not merged, VERSION untouched. Independent Fable review (read
>   the WORKTREE) found ONE merge-blocker — a cross-load dangling-pointer/double-free (ConcProxy's 0xFF form
>   ptrs survived a save-load reset while the dynamic forms didn't) — **FIXED** via `ConcProxy::Reset()` in
>   `ClearSelfCasts()` at kPreLoadGame (nulls g_form/g_src/g_next + clears borrowed effects before teardown).
>   KEY FINDING: baseline delivers FF Self via plain `CastSpellImmediate(sp,target,follower)` and for
>   FIRE-AND-FORGET the effect lands on the TARGET regardless of Self delivery — only CONCENTRATION Self
>   collapses onto the caster. So the "Self always lands on caster" premise that drove the whole rewrite was
>   FALSE for FF; the FF scope creep fixed a non-bug.
>   - **REMAINING GATES:** (1) DECK FIELD TEST — does the flipped concentration copy actually CHANNEL the heal
>     on the recipient? (the one real unknown; never deployed). (2) marth's GO to merge+deploy. Bundle with the
>     clean power-attack fix (`0d487e8`) in ONE deploy; it REPLACES main's broken `2ad0c414`.
>   - **DECISION PENDING (marth):** re-add the explicit `kHealFullPct` stop-at-full terminator (~6 lines), or
>     rely on baseline stop (gambit "HP below X" + 6s cap)? Review did NOT flag baseline stop as broken →
>     genuine preference. (marth earlier wanted stop-at-full, but that was on the broken build where the heal
>     hit the wrong actor.)
> - **(design, now BUILT) DEFINITIVE FIX spec:**
>   rebuild cast delivery = **baseline `00c6fce` + ONE mechanism**: a delivery-flipped copy (copy data+effects,
>   set `data.delivery=kTargetActor`, KEEP castingType — targeting-only, NOT an FF conversion) gated strictly to
>   `kSelf && kConcentration && target!=follower`, wired into `ApplyTargetEffect`'s concentration branch (covers
>   OOC + combat). **DELETE** all `NeedsCasterAttributedDelivery`/`ApplyEffectsFromCaster`/`AddTarget`. FF
>   (Candlelight/flesh/self-cast) = baseline byte-for-byte, untouched. **marth's SPEL insight (simplifies it):**
>   the fabricated SPEL form is only needed MOMENTARILY to fire the cast — once the AE is applied it's
>   self-sufficient → 2 slots, fill→cast→REUSE-FREELY, NO idle timers / lifetime guards (that machinery solved a
>   non-problem; the SEV-3 lifetime findings are moot). Diff vs baseline should be ~just the proxy.
> - **STILL-OPEN QUESTIONS (field-test after the rebuild builds + CI-green + Fable-review):** (1) does the
>   delivery-flipped concentration copy actually CHANNEL the heal on the recipient? (the real unknown — never
>   deployed). (2) confirm the baseline FF-Self-on-others mechanism (marth disputes recipients self-cast; agent
>   to report how baseline delivers Candlelight-on-others so we preserve it). Memories: [[self-delivery-fallback-two-reserved-spels]],
>   [[cast-fanning-known-good-behavior]].
> - **POWER-ATTACK GAMBIT FIX — CLEAN, PARKED, ready to merge.** Branch `worktree-agent-ade5a6ffc963fa615`
>   (`0d487e8`, GREEN CI `32300153193`). Fixes "foe blocking → power attack" swinging at any range: now resolves
>   the specific blocking foe, latches it so the engine CLOSES to melee (`fMeleeReach` 200u, INI-tunable), and
>   swings only in reach. Passed Fable review (0 findings). Bundle with the cast rebuild in ONE deploy.
> - **PROCESS GATE (marth):** Fable review BOTH the cast rebuild AND power-attack BEFORE pushing/merging. When
>   reviewing a worktree branch, agents MUST read the branch's WORKTREE (`cd <worktree>`), NOT the main repo (a
>   first review read `main` by mistake and falsely refuted everything). Don't merge the churn branches
>   (`e32cf14` etc.) — the rebuild supersedes them.
> - **LOCAL FABLE REVIEW: DONE + CLEAN (2026-08-19).** 6-dimension multi-agent review of the 67-commit diff
>   (`v1.0.64`..main), each finding adversarially verified. **Zero SEV-1/SEV-2.** Two SEV-3s, both NON-blocking
>   follow-ups: (a) `Logistics.cpp:4438` OOC hostile cast's LoS wall-gate is inert (Check without a Want seed →
>   Unknown passes); (b) `ProgAllocator.cpp:171` NodeIndex first-wins makes FindNode order-dependent on a
>   duplicate FormID (only bites a malformed/overlapping catalog — PLAUSIBLE, pathological). Cloud ultrareview
>   stays ABANDONED (no access). **Phase 2 (`./release.sh` package + `git tag`) + tag-push stay HELD** until the
>   field test passes AND marth says go. Then: rewrite the STALE CHANGELOG v1.0.65 entry (predates the cast
>   overhaul + RC fixes — 69 commits) + Nexus bbcode (1.0.62→65). Full cast saga: `Docs/CAST-DELIVERY.md`.
> - **(prior) v1.0.65 RC field-fix batch MERGED + CI-GREEN (`b63beb9`); cast-delivery rewrite field-pending.**
> - **CAST DELIVERY = UNIVERSAL DIRECT FORCE (`b63beb9`, sha `b0c65aae`, deployed). READ `Docs/CAST-DELIVERY.md` (canonical) BEFORE touching ANY cast path.** The concentration-unification `5f8e873` (below) REGRESSED heals: it routed OOC concentration through the AI PACKAGE (`ConcentrationCast`→`Packages::CastAt`), which §4.6-declines every tick for PACKAGE-LOCKED custom followers (Lucien 2F00591F, his quest owns the alias at prio 80 > MFO 60) — so his player/ally heals stopped. marth: heals worked in the public build, "avoid that [package] route for anything, always use the known working force." Fable OWNED the fix end-to-end (`fable-cast-solve`) after ~10 high-effort turns of my incremental mis-steps + a Fable review. FINAL: every concentration cast + forced effect delivers via DIRECT `CastSpellImmediate` onto the target (package-lock-proof), self+target, combat+OOC unified via `CastTargetDirect`/`CastSelfDirect`. Package removed from concentration delivery; 2 FF paths keep it WITH direct fallback (no dead cell for locked followers). Gaps closed: **1s heal re-apply cadence** (`kConcApplyPeriod`; was silently 4s=¼ throughput), **all `CastSpellImmediate` MainThread::Post'd** (fixed 2 off-worker calls incl. `CastOn` combat FF = the 1.5.x `act.cast_target` AV frame), **combat consent-coherence proven** (AI stays denied, direct channel always delivers, can't deadlock), ward-release + magicka-clamp. Bounds unchanged (self heal 6s/ward 15s; foe 1-4s; heal-until-topped). **FIELD-WATCH:** `FORCE-CAST … magicka X→Y` ~1/s while wounded + HP climbing, `concentration (direct force)` in combat, ZERO concentration `[pkg] DECLINED`. Residual risk (Fable-flagged): if a log shows `[consent] HARD-ABORTED … (concentration unbounded)` naming the GAMBIT spell mid-stream, add a `StreamLive`-style registry-keyed CheckCast exemption (per doc), NOT a package return.
> - **(superseded) CONCENTRATION FULLY UNIFIED (`5f8e873`, sha `5118941`, deployed).** marth pushed hard here — "don't
>   miss previous solutions, make all cases work." MFO ALREADY had the channel (`ConcentrationCast`,
>   bounded package stream, v1.0.58) AND the self solution (`CastSelfDirect`, #67 REVOKED — self-conc is
>   NOT barred). The only gap was the OOC logistics path never ROUTING to them. Fixed: new public wrapper
>   `Actuation::CastConcentrationAt` over `ConcentrationCast`; OOC concentration (player/ally/foe) routes
>   through it; self-conc → `CastSelfDirect`. BOUNDS (single source `ConcentrationHold` + caps): foe
>   offense 1-4s (LoF-gated), heal 6s/until-topped, utility 4s; **self heal 6s cap, self ward/utility 15s
>   cap** (`kConcSelfUtilityCap`, marth's call — NOT 4s: CastSelfDirect is non-rooting so a shorter cap
>   only flickers the ward). FF self-buffs keep authored duration. The ff0cb48 instant-apply heal-stream
>   stopgap is GONE; a latent OOC foe-offense UNBOUNDED-freeze risk was found+fixed. Full spell×target×
>   context matrix in the feat/unify-concentration-routing agent report — every cell WORKS, none barred.
>   LESSON (see [[self-concentration-gambits-barred]] memory, now flipped to RESOLVED): read
>   `ConcentrationCast`/`CastOn`/`CastSelfDirect` + concentration docs BEFORE designing cast behavior.
> - **⛔ STILL HOLDING the cut.** The 6 RC field issues + 3 follow-up field reports (logistics ally/player
>   heal, off-role shed-drop, concentration unification) are ALL FIXED, merged, CI-GREEN, and DEPLOYED to
>   the deck (`5f8e873`, sha `5118941`). Re-cut ONLY after marth
>   field-re-probes AND says go. **Two follow-ups on top of the 6 (`1fad7e5`):**
>   (A) **`8a369d5` — logistics "Ally HP below" now heals the PLAYER.** The ally SELECTOR (`PickAlly`)
>   already included the player, but the OOC cast dispatch routed a beneficial `cast_target` onto a
>   resolved ally through `Packages::CastAt` (the FOE-aimed alias-0 package), so it never landed. Fix:
>   `immediate = !selfPkg && !castAtFoe` where `castAtFoe = cast_target && hostile spell && foe target`;
>   beneficial/ally casts now DIRECT-apply (CastSpellImmediate) like combat `CastOn`/`CastAuto`. Combat
>   had no hole. (`Logistics.cpp`.)
>   (B) **`6831959` — off-role weapons: no combat shedding, DROPPED on the floor after battle (no player
>   handoff).** `ShedOffRoleWeapon` was firing during `IsInCombat` flaps mid-fight and HANDING the player
>   the weapon. Now: a 3s post-battle dwell (`g_lastCombatSeen`, stamped by `Logistics::NoteInCombat` from
>   Scheduler's in-combat branch — resets on any real combat frame so a lull can't mature it) gates it;
>   ALL off-role weapons `DropObject` on the floor via `MainThread::Post` (FormID-capture, VR skips+logs);
>   the RemoveItem-to-player handoff + "hands you a X" toast are GONE; no INI knob. Guards kept (never
>   disarm, skip stock/creature/socketed/excluded). marth: MFO never looted the mace (weapon loot is
>   upgrade-only in the dominant class) — vanilla follower AI grabbed it; MFO now just disposes of it. **The 6 fixes** (3 worktree agents, disjoint files):
>   (1) **prog prereq `==`→`>=`** — `OwnsPerkForm` walks `nextPerk` forward so owning a HIGHER perk rank
>   satisfies a lower-rank prereq (`ProgAllocator.cpp`); (2) **AUTO heal fanned to full-HP members** —
>   effect-driven `SpellHealsHealth`/`IsHealEffect` gate so ANY health-restoring spell is filtered by
>   per-target need (player runs the same `consider` gate); `Choice` now carries the firing rule's HP
>   threshold (`Actuation.cpp`/`Evaluator.*`); (3) **robotic recast** — `g_beneficialRecast` per-(caster,
>   spell) suppression = authored duration × `fBeneficialRecastFrac`(0.85) × ±`fBeneficialRecastJitter`(0.20)
>   jitter (new INI knobs); (5) **concentration caught by DoT filter** — concentration spells bypass the
>   already-active/DoT gate entirely; (6) **Magelight "when dark" spam** — robust active detection
>   (HasMagicEffect OR active-effect-list scan by spell) + the #3 suppression; (4) **looting broken**
>   (Fable) — scene-package THEFT mid-walk no longer stickies loose piles (theft guard pauses stall clocks,
>   re-asserts claim, transient-blocklist only), sibling-follower fail-map churn age-gated (≥10s), single-cell
>   scan → 3 actor-anchored attached cells (crash4-safe) so a table loots OUT (`Logistics.cpp`).
>   `kNoProgress` 7s→5s. See scratchpad `RC-issues.md` for the deck evidence.
> - **VENDOR FEATURE REVERTED OFF MAIN (marth's call).** The town-update vendor spell-tome feature had been
>   auto-merged to main+origin (errant 12:30 cron, gated dark) and re-bumped VERSION to 1.0.65. Reverted:
>   main force-reset to the clean pre-vendor base + the 3 RC fixes, VERSION back to `1.0.64`. Vendor work
>   PRESERVED on branch `feature/vendor-spell-tomes` (origin) for the TOWN UPDATE — do NOT re-merge to a
>   mage-patch release. Design-calls still open: #75 equip-gate policy, §16 manual-points undo.
> - **#2 caveat (for marth):** the per-target heal gate covers the AUTO *fan* path (`Self/Always/world-gate
>   → Cast (Auto)`). An `Ally: Health < X% → Heal (Auto)` still routes to SINGLE-target (heals the one
>   wounded ally), so it never over-fanned. If marth wants "Ally < X%" to fan as an AoE heal, that's a small
>   `autoPick`-gate routing change — NOT yet done. **#3 caveat:** a non-concentration duration HoT heal is
>   now under the recast suppression like any buff (instant + concentration heals exempt).
> - **What v1.0.65 is:** the UNIVERSAL forced self-cast (`Actuation::CastSelfDirect`, direct effect+magicka,
>   NO equip/channel — see MAP §2 & `SPEC-self-cast-forced.md`) + **AUTO cast-target inference**
>   (`Actuation::CastAuto`: hostile → nearby foes, beneficial → whole party who needs it) + the full
>   **comprehensive review-fix batch** (`Docs/REVIEW-2026-08-18-comprehensive.md`, 6 Fable reviewers).
> - **All 6 waves MERGED to main** (`ec0e04f` = Wave 2 head; Waves 3/5 in `cb0d9ab`/`69fe287`; Wave 6 this):
>   - **Wave 1** — threading accessors: `Followers::IsTrackedFast(FormID)` + `ActiveSnapshot()` (any-thread
>     mirror, closes the combat-thread `IsTracked` UAF); `Diagnostics::PausePump/ResumePump` +
>     `PumpTickGate` (resumable worker-quiesce for `SaveCallback`, Dekker TOCTOU close).
>   - **Waves 2–5** — combat F3 tri-state (Refreshed→transparent NoOp), combat dispel-beat window, ally-
>     selector friendly-fire refusal, progression snapshot reads, economy stock-gear/StripCorpse dibs,
>     worker-equip race, magicka clamp, DoT recast (`fDotRecastBurstRatio`), + more.
>   - **Wave 6 (this)** — (1) logistics `cast_target` MANUAL pick now fires via the shared public
>     `Actuation::ResolveCastTarget` (was silently dropped); (2) `Board::PublishSnapshot` roster reads
>     moved to `ActiveSnapshot`/`IsTrackedFast`; (3) item-2b `Followers::BoardEditScope` tripwire —
>     documents+asserts that a main-thread board Prog edit never INSERTS into g_followers (rehash);
>     (4) new **INVARIANT #74** (BSJobs-vs-main distinct threads, mutual exclusion UNPROVEN → off-worker
>     reads use the Wave-1 mirror); (5) docs refresh (CLAUDE/MAP co-save 4 records/PRGN v4/FWPN v1,
>     Wave-1 API, cast semantics, Refresh-on-worker, Packages line drift).
> - **STILL PENDING before the v1.0.65 cut:** (a) **deck field re-probe** of `1fad7e5` — confirm the 6 RC
>   fixes hold (no Magelight spam, humanlike light recast, heal only the wounded, concentration heals steady,
>   prereq ranks unlock, looting completes a room fast); (b) **cut + tag** v1.0.65 + Nexus bbcode (still owed
>   1.0.62→65) + changelog. Vendor feature is NO LONGER pending here — reverted to `feature/vendor-spell-tomes`
>   for the town update. Saves are VERIFIED SAFE — no co-save regression in the batch.
> - **DESIGN DECISIONS AWAITING MARTH (do not decide unilaterally):**
>   - **#75 equip-gate policy** — how aggressively the CheckShouldEquip gate should clamp the AI's own
>     re-arm vs. letting it drink/scroll/shout; current 30-vtable list excludes weapon/potion/scroll/shout.
>   - **§16 manual skill points** — retroactive repricing on level-up + NO undo of an allocated point
>     (a spent manual point can't be refunded). Flagged as a deliberate design choice to confirm, not a bug.
>
> **▶ (superseded) RESUME (2026-08-17 PM) — v1.0.64 SHIPPED; crash queue next.**
> - **v1.0.64 RELEASED & TAGGED — DLL `3832128e`, deployed custom-modlist (Tuxborn PENDING deck wake).**
>   Field-verified good by marth (Lucien: dagger-only, clean cast↔melee handoffs). Tag `v1.0.64` pushed;
>   `releases/v1.0.64/` archived. Main files only (release.sh excludes the addon). Last PUBLIC Nexus =
>   v1.0.61; **Nexus bbcode still owed, spans 1.0.62→1.0.64** (exact-cast, equip-override, this batch).
>   This release folds in, all Fable-reviewed (two passes, SHIP):
>   - **addon Fable fixes** (`6a1405e`): PRGN v4 plugin-qualified class identity, OR-tail prereq,
>     PerkKnownToCatalog, board economy strings from snapshot.
>   - **equip-override = FFXII TRUE-OVERRIDE** (`1560bea`/`66716da`): a #65 CAST class override no
>     longer defeats an active equip gambit's stance clamp (Lucien dagger↔spell thrash). Arms the
>     CheckShouldEquip gate; higher cast gambit still left-hand casts (gate exempts WantedSpell);
>     reverts on POSITIONAL known-false (equip rule above the scan stop w/ equipHeld==0).
>   - **base-class mage sidearm** (`c97035f`): melee-loot contract keys on combatClassOverride, not the
>     gambit — base Mage (Cast==3) = daggers-only for BOTH loot AND the combat gambit; spellsword
>     (Melee/Ranged class) = full role. Gated bMageDaggersOnly.
>   - **loose health potions** (`cb214a8`): join the route-2b acquire whitelist (walk + ActivateRef);
>     ownership gate still skips owned inn/shop/home stock.
>   - **hysteresis** (`fa90004`+`7a5956f`): melee-clamp release gated on a per-follower Temperament
>     dwell (3.0s±1.0s) of SUSTAINED known-false — reads human; Fable SEV-3 fixes (dwell refreshes on
>     unknown-held ticks too; erase debounced 2-tick; stale comment).
> - **NEXT (two tracks, ALONGSIDE):** (A) **CRASH QUEUE** ([[crash-reports-cast-target-1.5.x]]): 3
>   pastebins — MFO AV in `act.cast_target` (poisoned ptr +0x13) on SE 1.5.97 + a tbbmalloc/EngineFixes
>   heap crash. Diagnose via [bc] + CI PDB. (B) **FORCED SELF-CAST** — spec at
>   `Docs/SPEC-self-cast-forced.md`. **BUILT, GATED, awaiting deck probe** (branch
>   `worktree-agent-a9afe7fd5fe8c8e26`): dedicated **no-QNAM t6 self package** `MFO_CastPackageSelf`
>   (Forms `0x835`) on command-quest **alias 2**; `Packages::Begin`/`ConcentrationCast` self routes wired,
>   bounded via CastHold; ReleaseAll sweeps alias 2. Whole route is behind **`bCastSelf` (INI, default
>   OFF)** — it MUST NOT ship active until the isolation probe passes. §0.22 already field-proved probe
>   6's t6+no-QNAM ward casts clean (REVOKED #67) — the record shape is not the open risk; the untested
>   part is the PRODUCTION path (arbitrary self spell, in combat, alias fill/evict). **DECK PROBE (do
>   before trusting):** (1) load a save with the new ESP + `bCastSelf=0` → game loads = record shape is
>   load-safe. (2) `bCastSelf=1`, author ONE `cast_self` ward/self-heal gambit on ONE follower, trigger
>   in combat. CLEAN = `[pkg] … requested cast … on self (alias 2)` → `[cast] … CONCENTRATION … on self …
>   stream` and the ward raises, NO CTD. CTD at the cast = the production path still hits the cell → fall
>   back to a direct `ActorMagicCaster` channel + timed interrupt (spec §Risks). Still owed after a clean
>   probe: the deck LATENCY number (force-dispatch→ward-up ms by weapon class). Robust authoring pattern:
>   proactive "ward WHILE threatened" held as a stream, not reactive race.
> - Also owed: Nexus bbcode 1.0.62→64; Progression Add-On release + kit; MAP.md full refresh.
> - **CONCENTRATION (FINAL — DIRECT FORCE everywhere; see `Docs/CAST-DELIVERY.md`, canonical):** delivery is
>   the **known-working DIRECT FORCE** (`Actuation::CastTargetDirect` = `CastSpellImmediate` onto the target
>   + magicka deduct, NO package) in **BOTH contexts — OOC AND combat** (`ConcentrationCast`'s v1.0.58-65
>   package stream is REMOVED, marth's ruling), so a **package-locked custom follower (Lucien 2F00591F)
>   heals the player/ally and damages the foe everywhere** — the package `§4.6`-DECLINED every tick (OOC:
>   the c539257 regression, deck build 5f8e873; COMBAT: same decline + the consent hooks denying his own AI
>   = total lockout). The direct apply bypasses `CheckStartCast`/`CheckCast` (they sit on the AI pipeline
>   `CastSpellImmediate` skips, ENGINE_NOTES §0.13 — deck-proven: SELF-CAST applies land while the same
>   actor's AI casts are HARD-ABORTED), so deny-the-AI + deliver-directly is coherent. **CADENCE:
>   concentration re-applies every ~1s (`kConcApplyPeriod` — per-second authored magnitude/cost; the 4s
>   fCastCooldown pacing quartered heal throughput, "heals feel broken"); FF keeps fCastCooldown.**
>   **REAL-EFFECT CONTRACT (marth's ruling, FINAL — supersedes the interim RestoreActorValue recreation
>   AND the magnitude-zeroed sustain): a bare one-shot CastSpellImmediate applies ~0 of a per-second
>   concentration magnitude (b63beb9 A/B: magicka drained, HP flat on self AND player — rate × ~one
>   frame), so `SustainConcentrationEffect` attaches the REAL effect once per stream and sustains it:
>   each ~1s beat pins a real duration (stream window) + re-arms elapsedSeconds on that ONE ActiveEffect,
>   never touching the ENGINE-COMPUTED per-caster magnitude — the engine channels the number itself
>   (skill/perks/resists correct, EVERY archetype: waterbreathing/invisibility/ward just last). MFO owns
>   delivery + per-second cost deduct + bounds only; zero manual magnitude math remains. ONE sustained
>   HUD entry + continuous shader (effect VFX in scope; only caster POSE deferred); momentary sustained
>   effect dispels at end-of-stream only (it genuinely channels — cut it, never let it run unpaid).
>   Wired into self, target, AND AUTO applies. Field check: "conc effect ATTACHED" ONCE per stream (if it
>   repeats per beat the engine refused the sustain → fallback = engine-computed ae->magnitude per
>   second, never base values), ONE continuous HUD entry + visible shader, HP climbing at the caster's
>   own rate.**
>   Bounded/released by `TargetCastReconcile` (registry `g_targetCast`): hostile 1-4s LoS+LoF-gated on
>   every apply / heal 6s cap but re-applies while wounded / utility 4s; **dispel-on-release is
>   STICKY(ward)-ONLY so a heal flows uninterrupted** — release + re-stream, never a stop. Threading: every
>   `CastSpellImmediate` is `MainThread::Post`ed (the Logistics OOC FF inline call was the last off-main
>   one — fixed; prime suspect for the queued 1.5.x cast_target AVs). OOC FF-hostile-at-foe keeps the
>   animated package but now direct-force falls back on a `§4.6` decline (no dead cell for locked
>   followers). The old OOC `healStream` stopgap (ff0cb48) and the c539257 `CastConcentrationAt` package
>   wrapper are both REMOVED. **Self-concentration NOT barred** (#67 REVOKED): self → `CastSelfDirect`
>   (same 1s concentration beat), capped in `SelfCastReconcile` — HEAL `kConcHealCap` 6s / WARD
>   `kConcSelfUtilityCap` 15s (marth), dispel STICKY-only, FF self buffs keep authored duration. AUTO
>   (`CastAuto`) keeps its instant-apply broadcast for concentration (one package stream can't fan to N —
>   see report). Old "not working" report predates 1.0.53.
> - **THEN roadmap:** town update (#31), then the Roster addon (2nd ESL).
> - Deferred/known: MAP.md now TRACKED but STALE (PRGN v2 era) — full refresh owed (PRGN v4, #76
>   equip-override/hysteresis, base-class sidearm); PRGN v3-save wipe fully closed only for v4+ saves.
>
> **▶ §18 ADDON-API REFACTOR — DONE (Stages 1-4 shipped; addon fixes pending FF above).**
> Rebuilt fresh on current main (NOT the stale `esl-api-wip` branch, which predates the
> 2026-08-15/16 perk-gate fixes) using that branch only as reference. Contract = §18.6.
> - **STAGE 1 DONE (registration seam)** — DLL `1e3b…`→ **`804bd21`** (CI 31987673446): MFO.esp
>   ships `MFO_AddonManifest` sentinel KYWD (0x803); an addon = ONE FLST manifest whose
>   entry[0] is that sentinel; `Progression::Init` enumerates manifests (N addons) instead
>   of `LookupLoadedLightModByName`. MFO_Progression.esl now masters MFO.esp (own prefix
>   0x02 = OWN_PROG) + carries the manifest FLST. audit_esp.py derives own-prefix from master
>   count. NO save-format change. Deployed local custom-modlist; **deck field-verify PENDING
>   deck wake** (asleep during deploy) — confirm log `[prog] addon manifest … registered` +
>   board still opens before building Stage 2 on it.
> - **STAGE 2 DONE (save-critical, deployed) — DLL `38357016` (CI 31989899988), ESL `b1b3ef3f`.**
>   N-class model: `ClassDef{id,name,stance,skills,perkPriority}`, `Classes()`/`FindClassDef()`,
>   `SetClass(FormID)`; `ProgState.cls`→`clsId`. Built at Init by parsing each manifest's
>   classes-list FLST (entry[1]) → class-def FLSTs (MESG name + AVIF skills + PERK + `_Stance`
>   GLOB). Board prompt dynamic-N. **PRGN v2→v3**: class = 4-byte FormID (ResolveFormID) at the
>   old ordinal's field slot; v<3 load reads the 1-byte ordinal and maps 1/2/3→`g_classes[ord-1].id`
>   (reviewed vs INVARIANT #12 — field order preserved, v1/v2 readers intact). Generator emits
>   MESG 0x830-2 / `_Stance` GLOB 0x840-2 / class-def FLST 0x850-2 / classes-list FLST 0x85F.
>   Today's gate fixes (base-aware ownership, OR-group eval, condPrereq edges) preserved.
>   **FIELD-VERIFY:** existing followers keep their class across load (migration), class pick +
>   auto-scale still work, board opens, log `[prog] … class … declared`.
> - **STAGE 3 DONE (economy addon-declared, deployed) — DLL `ce8ac5a1` (CI 31992158112).** Every
>   economy value now read from the manifest's GLOB entries matched by editor-id SUFFIX
>   (`AssignEconomyGlob`/`EdidEndsWith`): `_LevelsPerPerkPoint` default 2, `_SkillPointsPerLevel` **2**,
>   `_ManualSkillPointsPerLevel` **2**, `_SharedGrowthDivisor` 2, `_RespecRapportCost` 500, `_SkillCap` 100,
>   `_DevCmd` live. DLL supplies defaults when absent. Manifest FLST now 9 entries (sentinel + classes-list
>   + 7 econ GLOBs). **ALL transitional coupling removed** — `kAddonPlugin`/`kAddonVersionGlob`/`AddonVersion`
>   /fixed `kGlob*` gone (grep-clean). Class model + gate fixes + PRGN v3 untouched. Econ record shape frozen
>   in DESIGN §18.6 for the API doc. **FIELD-VERIFY:** log `[prog] economy: _SkillPointsPerLevel = 2 (from …)`,
>   perks 1/2 levels, followers get ~2 skill pts/level.
> - **STAGE 3 REMAINDER — BOTH DONE.** (a) **Skill-menu position memory** shipped, DLL `c9742c4d`
>   (Board.cpp, keyboard-focus restore on apply-popup close). (b) **Addon MCM tab, entirely in the ESL**
>   — DLL `d30b7a22` (CI 31995641610): the ESL ships `MFOP_MCMQuest` (VMAD→`MFOP_MCM.pex`), its own
>   `Config/MCM/Config/MFO_Progression/config.json` (6 GlobalValue sliders bound to the economy globals) +
>   `SEQ/MFO_Progression.seq`; the DLL re-reads the manifest economy on menu close (`ReloadEconomy`/
>   `OnMenuClose`, MainThread-posted) — GENERICALLY, never naming the addon. **MFO.esp byte-unchanged;
>   zero `MFO_Progression` code refs.** Deployed local custom-modlist (`d30b7a22` + `MFOP_MCM.pex` + MCM
>   config + SEQ); **deck field-verify PENDING deck wake + Tuxborn scp retry**. FIELD-TEST (in-game only, no
>   CI): the "MFO — Follower Progression" MCM tab shows 6 sliders; moving one + closing the menu changes the
>   economy (log `[prog] economy reloaded on menu close`). Papyrus compile pipeline proven (`tools/compile.sh`).
> - **BOARD-EXTENSION API — SCOPED (not built):** `Docs/BOARD-EXTENSION-API-DESIGN.md` — two tiers
>   (Tier 1 declarative ESL/JSON panels via the manifest + GlobalValue/Papyrus; Tier 2 native companion-DLL
>   tabs via a `MEO_API.h`-style versioned interface + stable C draw shim). Recommends Tier 1 first. Frozen
>   into ADDON-API.md when built. marth wants it documented + available for third parties.
> - **Tooling now documented:** `Docs/TOOLING.md` (Papyrus compile, MCM-Helper pattern, ESP/ESL generator,
>   audits, two-deck deploy) — the pipeline the next session no longer reconstructs from MEO.
> - **STAGE 4 DONE — `Docs/ADDON-API.md` written (the frozen third-party contract).** Sentinel KYWD +
>   manifest FLST layout (entry[0]=sentinel, one classes-list FLST of class-def FLSTs {MESG name + AVIF
>   skills + PERK priority + `_Stance` GLOB}, economy GLOBs by suffix + defaults, master reqs, PRGN
>   discipline) + MFO_Progression.esl as the worked example; linked in Docs/INDEX.md. **The §18 addon-API
>   detachment is COMPLETE — the runtime "reproducible by a third party with only an ESL" bar is met.**
>   Remaining before the addon's Nexus release: the MCM-tab injection + skill-menu memory (Stage 3
>   remainder) and packaging/kit.
> - Also pending (independent files, [[field-notes-queue-2026-08-17]]): slot-empty looting (#1), mage sidearm
>   loot (#2), coin-purse gold (#3), ally-gambit-includes-player (#4).
> - **STAGE 4:** `Docs/ADDON-API.md` (frozen third-party contract) + MFO_Progression.esl as
>   the worked example. This is the Nexus-release gate for the addon.
> - **Known-good fallback:** tag `esl-fieldverified-2026-08-13`. After the addon: **Roster
>   addon** (separate ESL), then town update (#31).

> **#74 COMPONENT 2 BUILT (2026-08-11) — the ALLOCATOR backend + MFO_Progression.esl +
> dev harness — awaiting marth review + CI.** New `native/ProgAllocator.cpp/.h` (design
> §4/§5/§6/§8/§15; consumes component 1's frozen catalog, ProgProbe untouched). The
> LOCKED model: AUTO skills / MANUAL perks; class gate (no skills until a concrete
> Melee/Ranged/Mage pick — mirrors into #65 combatClassOverride, same ordinals); perk
> earn = 1/player-level × scarcity (catalog effectiveRanks÷totalRanks); Shared Growth
> ON = benched at ESL-divisor half rate (active earns at the player's RATE, never an
> instant catch-up — bench-lag is the price); first class-set = level-match + one-shot
> veteran grant (level-matched, scarcity-scaled, no skill bonus); respec = perks
> refunded + −500 rapport via new `Rapport::Spend` (not rate-scaled); v1 unique-base
> only + never builds on natively-owned perks. §5 gate enforced in the backend
> (prereq via parentPerkIDs any-parent-owned + rank order + `perkConditions.IsTrue
> (follower,follower)` at apply; named `[prog] allocate REJECTED` lines). Engine writes
> are the three probe-proven paths only (P1 TESNPC::AddPerk+ApplyPerksFromBase with
> rank-K-replaces-K−1; P2 §4.2 reconcile with adopt-natural + never-below-natural
> clamp; P3 GetPerkIndex-guarded reapply, lazy via the poll until the actor resolves).
> Level poll = MainThread::Post self-chain (~2s), generation-guarded, started at
> kPostLoadGame. Co-save: NEW independent record `'PRGN'` v1 (header lastPlayerLevel;
> per follower flags/class/level/remainder/unspentPerk f32/perkAlloc/skillAlloc/
> baseline; ResolveFormID-or-drop, off-catalog perk allocs drop+REFUND; saved even
> when the ESL is absent so disabling the addon never eats the data). The ESL is now
> EMITTED by MFO_GenerateESP.py (out/MFO_Progression.esl, ESL-flagged, Skyrim.esm
> master, no SEQ): GLOBs 0x800 MFOP_Version=1 / 0x802-0x807 economy knobs (perk/lvl,
> skill/lvl, shared divisor, respec rapport 500, veteran mult, skill cap) / 0x808
> MFOP_DevCmd; FLSTs 0x810-0x812 MFOP_ClassSkills_* (ordered AVIF priority, dumped
> vanilla ids incl. Illusion=AVMysticism 0x45B; DLL maps via live ActorValueList,
> triangular weights + dominant-sibling pruning → Melee lands 40/30/20/10) and
> 0x818-0x81A MFOP_ClassPerks_* shipped EMPTY (xEdit extension point; DLL falls back
> name-agnostic); KYWD 0x820 MFOP_Enrolled reserved (not stamped in v1). DLL reads
> GLOB record DEFAULTS at kDataLoaded (§10). audit_esp.py now has per-plugin profiles
> and audits BOTH files (no args); package_test.sh ships the ESL in the test zip.
> DEV HARNESS (`bProgHarness=0` default, INI-only + `iProgHarnessKey=40` apostrophe):
> one hotkey on the first unique teammate, verb from console `set MFOP_DevCmd to N` —
> 0=status 1=enroll 2=cycle-class 3=skills 4=alloc-next-eligible-perk 5=respec
> 6=economy dump; all `[prog]`-logged. New RE:: symbol for CI: `RE::BGSListForm`
> (everything else already in-tree). Shared-growth player toggle = `bSharedGrowth`
> (Config, MCM wiring later). NO version bump, NOT pushed.
> **Adversarial review round 1 FIXED (same day):** SEV-1 — ReconcileSkill stored the
> REQUESTED points, so a cap-saturated write under-recovered natural and a later shrink
> (class change / drift-watch dominance re-pick) wrote the base BELOW true natural into
> the save. Fixed both ways: `points` now stores the APPLIED delta (desired−natural,
> exact recovery), AND the serialized enrollment baseline is a hard floor on natural
> and on every write — SetBaseActorValue has exactly ONE call site (ReconcileSkill), so
> no path (class change, drift-watch, respec, reload) can leave a base below the
> captured natural; a pre-fix-corrupted save self-heals on first post-load reconcile.
> L1: reapply sibling-clear could RemovePerk a natively-appeared rank — now DEFERS
> (touches nothing, named line) and GateNextRank freezes upgrades whose own rank form
> is no longer on the base. L2: co-save `av` ordinals validated against the 18-skill
> set on load (droppedAv counter), not just counts. L3: PerkPointsPerLevel /
> SkillPointsPerLevel / SharedGrowthDivisor / VeteranCatchupMult GLOBs now FNAM 'f'
> (float-typed, fractional-authorable in xEdit; DLL unchanged) + audit lockstep check.

> **#74 ROUND 5b (2026-08-12) — vertical-axis DATA-TRACE fix: constellation position
> = GRID + FLOAT OFFSET.** marth (One-Handed field test): Penetrating Strikes drew
> BELOW Weapon Mastery — inverted vs the game. Offline trace of the installed
> winning One-Handed AVIF (Requiem.esp) settled the convention with data: NEITHER
> float sign works alone (vpos-up: PS −0.10 < WM 0.00 → PS below, the exact bug;
> vpos-down: capstone Stunning Charge +2.2 lands at the bottom). The engine's real
> position is the COMPOSITE of the integer grid slot and the float fine-offset —
> x = gridX + hpos, y_up = gridY + vpos — which reproduces the game exactly: WM
> (2,0)+0.187/0.0 → y 0.0 bottom; PS (2,1)+0.187/−0.1 → y 0.9 STRAIGHT ABOVE WM
> (same x column); Stunning Charge (3,4)+2.2 → y 6.2 top. Cross-checked on
> Destruction: Novice 0.0 → Apprentice 0.9 → Adept 2.1 → Expert 3.0 → Master 4.6,
> every chain monotone upward. Board.cpp maps compX/compY with one screen-y flip
> (root at canvas bottom); median-NN normalization, fit clamp, Béziers, nav,
> culling, zoom, filter all unchanged. BONUS guard the trace exposed: real records
> carry UNINITIALIZED grid fields on the root section (~4.5e8) — perk-node
> outliers are now excluded from the bbox and clamped to its edge (rendered,
> selectable, never layout-fatal). NOT field-tested (round 5b).

> **#74 ROUND 5 (2026-08-12) — AUTHORED DOME LAYOUT replaces the tiers.** marth:
> combat trees tiered fine, MAGIC trees were terrible under round-4 Kahn tiering —
> and layout must be universal (Vokriinator-scale merges he can't test). The tree
> now renders from the catalog's authored hpos/vpos (the game's own constellation
> coordinates, shipped by every perk mod by construction). LAYOUT only — §5 gating
> untouched. Normalization: kept-set bbox → origin, vpos flipped (root at bottom),
> scaled by the tree's MEDIAN nearest-neighbour distance (one unit ≈ one typical
> node gap at any authored scale; a lone close pair can't inflate it; coincident
> nodes get a tiny deterministic nudge, never amplified). Per-frame fit-to-canvas
> clamped to a 110–300px typical gap ×zoom: dense trees scroll at readable density,
> tiny trees don't stretch. Presentation: vertically-eased cubic edges (soft wide
> underlay + thin core = tapered constellation look; lateral edges ease straight),
> soft glow on available nodes. Explicit nav (geometry-agnostic — scoring runs on
> the normalized units), culling, zoom, filter, 90%-window, economies, save-safety
> all unchanged. Kahn/condPrereq layout edges removed from the board (IsTrue still
> gates; catalog field stays for the census). NOT field-tested (round 5).

> **#74 ROUND 4b (2026-08-12) — Pyromancer acceptance TRACE + manual-skills =
> REPLACE.** (1) **The falsifiable trace (design doc §17 has the full data):**
> offline parse of marth's INSTALLED winning records (custom-modlist/Requiem
> profile; winning Destruction AVIF = Requiem.esp, 18 nodes) with the round-4
> classify/bridge/gate pipeline ported over the real data. Verified: Novice
> Destruction (root child) classifies EFFECTIVE/kept — the entry gate is intact;
> Pyromancy's bridged prereq set = exactly {Novice Destruction} (nothing bridged
> past, rootLine false); its perkConditions independently carry HasPerk(Novice)==1
> + Destruction>=25; three-state gate: nothing owned → unavailable, everything-but-
> Novice → unavailable, Novice owned → available. Gate keys on OWNERSHIP of the
> nearest kept parent, never structural reachability. **ONE real gap exposed +
> fixed:** Requiem's entry-less script-driven perks (Cremation/Deep Freeze/
> Electrostatic Discharge/Impact) were classified DEAD → their children's
> HasPerk conditions (Fire/Frost/Lightning Mastery) could never pass — the whole
> Mastery tier was permanently locked. Entry-less perks are now kMARGINAL:
> takeable for a point (as the player pays), dimmed-passthrough-visible when an
> effective descendant needs them — the player-identical chain restored. Field
> sighting likely explained by "Requiem - SPID Apprentice and Novice Perks for
> All Followers" granting Novice natively (legitimate availability, subtle native
> rendering); re-check on the deck. Trace tool: session scratchpad pyro_trace.py.
> (2) **Manual skill points now REPLACE auto growth (marth reverses "additive" —
> stacking was overpowered):** while ON, auto per-level growth is FROZEN at the
> stint baseline (RecomputeSkills effAutoLvl); OFF→ON re-latches the baseline
> (fresh stint, pool from 0); ON→OFF banks the stint's levels into NEW PRGN-v2
> field `manualExcludedLevels` so resumed auto never back-fills manual levels —
> no double-dip in either toggle direction, all fields change only on toggle
> transitions (replay-safe). Class enrollment baseline stays. Tooltip/hints now
> say REPLACES unambiguously. v2 layout extended (still never shipped — no
> migration).

> **#74 ROUND 4 POLISH (2026-08-12) — marth: round 3 "much better"; this pass fixes
> prereq ordering + menu feel.** **Prereq root cause (the real one): catalog pass 2
> recorded FILTERED direct parents (Requiem/Ordinator entry-less marker perks,
> classified dead) as the prereq truth — unallocatable through MFO, so whole
> subtrees were permanently locked AND their children drew as root-row orphans
> (tier 0, no edge): "order of unlocked perks doesn't respect prereqs" exactly.
> FIX: pass 2 now BRIDGES each prereq line through filtered nodes to the nearest
> KEPT ancestors (drawable edge + §5 truth, vanilla ANY-line rule; a line reaching
> the root through only-filtered nodes = root-reachable). perkConditions.IsTrue on
> the follower stays the final authority on top (an overhaul's HasPerk conditions
> still bind).** Also: rank-1 `HasPerk X==1` conditions extracted into the catalog
> (`condPrereqPerkIDs`) and added as TIER edges so visual order matches the
> conditions authority; tiering itself is now Kahn longest-path (strict topological,
> cycle-defensive) replacing the bounded relaxation. **Feel sweep (each a real
> state-machine bug, reasoned frame-by-frame):** (1) the A press that OPENED the
> tree window instantly opened the seeded root's take popup (same-frame edge) —
> release-guard added (r1Ready pattern); (2) scroll-home and follow-scroll fought
> in the same frame (SetScroll lands next frame; the clamp read stale scroll) —
> home mutes the clamp that frame; (3) continuous follow-scroll made the mouse
> wheel unusable (yanked back every frame) — follow now runs on INTENT only (pad
> move/zoom/filter reflow); (4) a parked deck cursor over the canvas stole the
> pad's selection every frame — hover steers only while the mouse MOVES; (5) ImGui
> key-repeat (20/s) overshot nodes — moves throttled to ~8 hops/s; (6) zoom
> shifted the layout under the selection — LB/RB now recentre ON the selection;
> (7) the marginal toggle reseeded to root, losing your place — selection restored
> by node id + pulled into view; (8) an all-filtered tree drew a blank canvas AND
> its gated input block ate [Y]/[View]/zoom (stuck) — message drawn, window
> controls moved outside the empty gate. Census dump updated (bridged parents +
> condPrereq counts; the old off-board delta underflowed). Design doc §17 round-4
> addendum. Economies untouched (perk floor(level/3)−native−spent; manual flat
> 5/level; ReconcileSkill single-site + floors intact). NOT field-tested (round 4).

> **#74 DECK ROUND 3 (2026-08-12) — field test 2 rebuild: explicit nav, tiered tree,
> filtered perks, derived perk economy.** Commit on top of 6080d1c (which IS CI-green,
> run 31614543597). **P1 root cause of two dead rounds: ImGui spatial auto-nav can
> only move to SUBMITTED items — virtualization culls them and nothing seeded focus;
> deck has no mouse. ABANDONED.** The tree window now runs EXPLICIT selection nav:
> everything in it is NoNav (PushItemFlag); a selected-node index seeds to the root
> row's middle node on open, d-pad/arrows (+LStick keys; the hook folds the stick
> into d-pad anyway) move it by deterministic nearest-in-direction over the LAYOUT
> (forward projection + 2.5x lateral penalty) — pure geometry, so culled nodes are
> reachable and the canvas follow-scrolls to the selection every frame; A/Enter/E
> opens the detail popup (its Take Selectable keeps ImGui nav — listPopup pattern);
> mouse hover/click steer the SAME selection. Pad controls: LB/RB zoom, [Y] next
> tree, [View] show-marginal, [B] close (popup-native). **P2 (skills invisible):
> branch order bug — `!eligible` was checked BEFORE `enrolled`, and eligibility
> (teammate/dismissal quirks) can flap at runtime, blanking an enrolled follower's
> whole UI. Now enrolled-first; eligibility only gates enrollment. Belt+braces: the
> skill list renders from the allocator's 18-line snapshot (compile-time names,
> engine-read levels — can never be blank), catalog joined per-row by AV, failed
> join degrades to a visible "no tree".** **P3:** NPC-dead perks never reach the
> catalog (comp 1); MARGINAL now hidden by default, kept as dimmed passthroughs when
> an effective descendant needs the prereq (needed-fixpoint keeps every parent of a
> kept node → edges never dangle); "Show marginal" checkbox + [View]. **P4 (marth
> SIMPLIFIED mid-round, supersedes rate×scarcity): perk points are DERIVED, never
> stored — max(0, floor(level/3) − nativeTreePerksAtEnroll − ranksAllocated).**
> Native tree ranks counted ONCE at enroll (serialized, PRGN v2); refunds automatic
> (respec/dropped allocs remove the debit); curve L10→3, L25→8, L50→16 minus
> pre-trained. 0x802/0x806 GLOBs now unread (kept in the ESL for id stability); v1
> co-save's stored pool read-and-discarded. **Manual skill points (also
> simplified): flat 5/level, hardcoded** — pool = (level − baseline)×5 − applied;
> 0x803 stays auto-scale-only. **P5:** dome coords replaced by a TIERED layout —
> tier = prereq depth, roots bottom row, siblings hpos-ordered (stable L-R only),
> centered rows; cached per (skill, marginal-toggle); culling + zoom kept; explicit
> nav works on any layout. Design doc §17 written (+§16 rate corrected). NOT yet
> field-tested (round 3).

> **#74 §16 MANUAL SKILL POINTS (2026-08-12) — per-follower toggle, built on the new
> design lens.** New design-doc §16 engrains the lens (marth): every auto-behavior is
> a DEFAULT, never a cage — manual override always exists (mage + multiclass builds
> are the motivating cases). The toggle ("Manual skill points", OFF by default, a
> nav-focusable Checkbox under the perk-point headline) banks a VISIBLE pool per
> progression level at the existing `MFOP_SkillPointsPerLevel` GLOB rate (0x803
> reused — no new record, no generator change), ADDITIVE on top of class
> auto-scaling. Spend: selecting a skill row with manual ON opens an action popup —
> "Apply 1 skill point (N -> N+1)" (DontClosePopups, pump-able; backend re-validates
> the pool per press so a stale double-A refuses, never over-applies) or "Open perk
> tree"; manual OFF keeps the old row-opens-tree behavior; tree-less rows become
> selectable under manual so points can go anywhere. **Save-safety (SEV-1 lesson
> baked in): NO incremental accumulator** — pool = floor((level − manualBaselineLevel)
> × rate) − manualPointsApplied, a pure function of two serialized baselines;
> baseline latches ONCE at first enable (off/on cycling can't farm or forfeit);
> apply routes through the ONE SetBaseActorValue site (RecomputeSkills →
> ReconcileSkill, target = auto share + per-skill manualPoints, baseline floor,
> applied-delta recovery); refuses at skillCap instead of absorbing into the clamp;
> manual-carrying skill entries survive class changes (override outranks default).
> Co-save PRGN **v2**: flags bit 0x10, manualBaselineLevel/manualPointsApplied u16,
> per-skill manualPoints f32 — version-guarded, value-validated, ON-without-baseline
> loads as OFF. Board seam: BoardFollowerView.manualSkills/manualAvail,
> BoardSkillLine.manual, snap skillPtsPerLevel/skillCap; EditKinds ProgSetManual /
> ProgApplySkillPoint (param carries the AV ordinal, the SetClassOverride shape).
> Respec still perk-only (does not touch manual skill points). NOT yet field-tested.

> **#74 COMPONENT 3 DECK FIELD FIXES (2026-08-12) — three field-test failures fixed,
> tab flow redesigned.** (1) *Perks un-selectable on deck* — ROOT CAUSE: the tree lived
> in a plain BeginChild and ImGui directional nav does NOT cross into a non-flattened
> child window (ScrollY tables work because their inner window IS nav-flattened — why
> Gambits never hit this); on the deck there is no mouse fallback, so the pad could
> never focus a node and A had nothing to press. Not the edit wiring, not the §5 gate.
> The canvas child now carries `ImGuiChildFlags_NavFlattened`, and the perk-point count
> is a HEADLINE (head font, always visible on the tab, in the tree window header, and
> inside the take dialog) with a self-explaining zero line (earn rate x scarcity, live
> numbers; enrolling at player level 1 legitimately starts at 0 — SetClass's veteran
> catch-up is (playerLevel−1)×rate×scarcity). Every locked node states WHY (tooltip +
> detail popup — whyNot from the §5 gate); no silent no-ops. Bonus deck-visible bug:
> several UI strings used a Unicode em-dash the baked fonts' default (Basic Latin)
> glyph ranges can't draw — all UI literals are ASCII `--` now (allocator whyNot/
> blocker strings included). (2) *Tree far too small* — the tree now opens in a
> DEDICATED full-screen popup (the listPopup pattern scaled up: B/Esc-close, focus and
> the board cascade all native), sized every frame to 90% of the LIVE
> `io.DisplaySize` (rewritten per frame from the real backbuffer — deck 1280x800 and
> docked 1080p+ both land; nothing hardcoded), canvas + culling + zoom inside.
> (3) *Skill selection unclear* — the Y-only cycler is replaced by an explicit SKILL
> PICKER table (Gambits' proven ScrollY pattern): one row per catalog skill — name,
> level (+alloc accent; auto-scaled by class, never manually assigned), perks owned/
> total, and an accent "N to spend" indicator; A/click on a row opens that tree's
> window; Y still cycles trees from INSIDE the window. Flow now: tab → L1/R1 follower
> → [class prompt] → skill list w/ visible points → skill → full-screen tree → A to
> allocate → B back. NOT yet re-field-tested on the deck.

> **#74 COMPONENT 3 BUILT (2026-08-11) — the Field Orders "Progression" TAB.**
> Third board tab, emitted ONLY when MFO_Progression.esl is
> detected (`kTabCount` is runtime now; the View-cycle follows). Flow per §15: selecting
> an eligible UNENROLLED follower auto-pops the class prompt once per selection (Melee/
> Ranged/Mage via `ProgAllocator::ClassName`, nothing hardcoded, nothing assigned until
> the pick) → one `ProgSetClass` edit enrolls + sets class + auto-scales skills. Then:
> 18-skill strip (base + accent `(+alloc)`), per-skill tree canvas, node detail popup
> (desc/ranks/skill-reqs; available → "Take rank N (1 point)", locked → the unmet
> requirement, never a confirm — §5's UI leg), respec button + danger confirm popup
> (−500 rapport, default focus on Cancel). **Threading (the load-bearing bit):** the tab
> reads TWO immutable sources on the render thread — the frozen catalog (its designed
> lock-free contract) and a NEW value-only `BoardProgSnap` published by the allocator on
> the MAIN thread (g_prog's thread) behind a small mutex as `shared_ptr<const>`; the
> snapshot/render copies only bump a refcount. Publish cadence: PollTick per-frame check
> — open-edge, focus-change (L1/R1), or ~500ms; seeded at OnPostLoad so the tab exists on
> first open; cleared in ClearAll (stale-save views can't cross a load). Mutations queue
> new EditKinds (`ProgSetClass`/`ProgAllocPerk`/`ProgRespec`) that ApplyEdits re-posts to
> `MainThread::Post` (the harness-hotkey shape — NEVER the AddTask drain), where the
> allocator's §5 backend gate re-validates; each verb republishes for an immediate echo.
> **Tree render (Vokriinator-scale):** dome-coord layout (hpos/vpos normalized, root at
> bottom, land-on-root scroll), zoom 0.5–2.0, VIRTUALIZED — nodes outside the child rect
> +1.5 cells submit nothing (edges AABB-culled), so cost tracks the viewport, not tree
> size. Each visible node is an InvisibleButton → ImGui's OWN spatial gamepad nav walks
> the tree (d-pad), A activates, auto-scroll follows focus, and the B-cascade sees a
> child window — zero new input paths; L1/R1 party-switch + r1Ready guard copied from
> Gambits; Y = next tree. DELIBERATE deviations: §7's "X = respec/auto-spend" NOT bound
> (FaceLeft is ImGui's nav-windowing key — a real collision; respec is a nav-reachable
> button instead) and auto-spend has no UI yet (the allocator's `autoSpend` flag stays
> reserved; its poll doesn't consume it either — a comp-3.5 follow-up with the MCM work).
> Allocator API added (the comp-2/3 seam): `BoardNodeView/BoardSkillLine/
> BoardFollowerView/BoardProgSnap`, `SetBoardFocus` (render-side atomic),
> `PublishBoardViews` (main thread; full §5 gate walk only for the FOCUSED follower —
> one tree per publish), `CopyBoardViews`, + read-only `EnrollBlocker` mirror of
> Enroll's refusals for the "why not eligible" line. `<memory>` added to the PCH
> (shared_ptr — the v1.0.8 missing-header class). No ESP/ESL changes (classes/economy
> already in the ESL). NO version bump; pushed for CI.

> **#74 COMPONENT 1 BUILT (2026-08-11) — the PRODUCTION catalog reader — awaiting marth
> review + CI.** New `native/Progression.cpp/.h` (design §1/§2/§3; component 1 of 4:
> catalog → allocator → board tab → ESL). One read-only pass at kDataLoaded (main thread)
> over the fully-merged AVIF trees of ALL 18 skills (18, not 12: the §15 scarcity ratio
> needs the full player pool as denominator) → an IMMUTABLE frozen catalog of value-only
> `PerkNodeView`s (FormID/name/desc/grid/dome coords/parent edges/rank chain with display
> skill-req strings extracted from perkConditions kGetBaseActorValue-on-kSelf->=). The §3
> dead-perk filter is in: kQuest dead; kAbility effective unless every effect is behind a
> narrow player-pin (GetIsID/GetIsReference == player, AND, ==1); kEntryPoint via a
> 92-row static_assert-complete verdict table (combat/defense effective; lockpick/craft/
> commerce/UI dead; marginal set flagged, unknowns FLAG-never-kill). Zero-effective-rank
> perks are excluded but recorded with a per-perk reason; `parentPerkIDs` keeps filtered
> prereqs in the graph truth. Detection helper per §1 (`LookupLoadedLightModByName
> ("MFO_Progression.esl")` + MFOP_Version GLOB 0x800 record-default read) — absent = one
> named line, off. Since the ESL doesn't exist yet: `bProgCatalogDump = 1` (INI-only,
> default OFF, bProgProbe precedent) forces the build + a `[prog]` census dump (per-skill
> kept/marginal/filtered + rank pools, every filtered perk + reason, 3 spot-check nodes
> per tree, global scarcity ratio) for on-deck verification. Mutates nothing. Wired:
> plugin.cpp kDataLoaded (after Catalog::Load), Config, CMakeLists. NO version bump,
> NOT pushed. ProgProbe untouched (stays gated off; removed later).

> **#59 CONTINUOUS CAST-CONTROL FILTER (2026-08-12) — BUILT, awaiting marth redeploy +
> field test.** Deck evidence (Serana, EXACT): forced Sunfire fires and concentration is
> bounded, but BETWEEN forced casts her AI slips Ice Spike (000C969C) / conjuration —
> `[cast] … (their own spell, not ours)`. ROOT CAUSE: the whole slider filter (deny +
> CastExempt kind-filter) lived on the transient `g_want` latch — armed only while a
> cast rule wins the scan, dropped by `Clear` on any IsInCombat flap — and BOTH deny
> hooks fast-out un-latched (`ShouldDeny` line ~582 "not latched → never our deny").
> FIX (CasterConsent.cpp/.h + Scheduler.cpp): persistent per-follower `g_ctrl` map
> (follower → configured combat cast-gambit spells; own `g_ctrlCount` fast-out mirror,
> same `g_mx` leaf lock), repopulated EVERY combat service tick by the Scheduler (before
> the ready-beat/suppression early-outs; empty = control off / log mode / no cast
> gambits → erase), swept by Clear/ClearAll (combat end, dismissal, revert/load). New
> `CtrlUnlatchedDeny` runs the SAME slider policy latch-free in both hooks
> (CheckStartCast advisory + CheckCast pre-charge hard gate, the latter behind an
> explicit `actor->IsInCombat()`): gambit spells always pass (forced/animated path
> untouched); **exact (≥4) denies every other spell — fire-and-forget, summons, all
> schools**; **partial (1–3) applies the CastExempt kind-filter continuously (marth:
> "the partial filter will also need to be active constant")** — self-heal/heal/buff
> exemptions per level preserved. Normal kSpell only (potions/scrolls/shouts/powers
> untouched); concentration exact-bound, pacing, FFHold decay, miss detector all
> unchanged. Out of combat: completely hands-off (population is combat-branch-only,
> hard gate checks IsInCombat, world casters never enter the map). FIELD SIGNATURE:
> an exact-mode follower in combat logs NO `(their own spell, not ours)` — leaks now
> log `[consent] … DENIED own spell … continuous cast control (unlatched; gambit …)`
> or `HARD-ABORTED … (continuous cast control)`. No version bump, NOT deployed.

> **v1.0.62 — #75 WEAPON-EQUIP THRASH FIX + progression probe.** #75 (Fable-built,
> adversarially reviewed CLEAN + CI green): (1) both-hand idempotency in EquipWeapon
> (right-hand-only guard was the thrash trigger — a caster's off-hand weapon was
> invisible); (2) equip-order ownership — `CombatStyle::Want` gains an equip-order
> flag set ONLY by the Scheduler wantStance branch (explicit equip gambit), and a
> `write_vfunc` gate on `CombatInventoryItem::CheckShouldEquip` (0x0F) across the 30
> concrete spell/staff item vtables denies the AI re-arming a spell/staff while an
> equip order owns the stance (latched gambit spell exempt; fast-out atomic; leaf
> locks; VR-guarded; off-switch bWeaponStyleControl). **marth's SCOPING CALL: gate
> covers the equip GAMBIT ONLY — NOT the magicka-dry fallback or #65 class override.
> A mage backing off to spells when low on magicka is correct base behavior; don't
> override it. The bug was thrash, not commitment.** Bundled: the dev-only progression
> PROBE (bProgProbe=0 default, CI green) — the P1/P2/P3 sinker validation for #74; run
> it on Tuxborn (throwaway save) as the on-ramp to the real progression build. Field-
> test both: Serana keeps her ordered blade (`[wstyle] ... DENIED`); no thrash.

> **#74 PROBE BUILD WRITTEN (2026-08-11) — awaiting marth review + CI + field run.** New
> `native/ProgProbe.cpp/.h` (LOG-ONLY, throwaway): the three §13 sinker probes behind
> `bProgProbe = 0` (INI-only, Data/SKSE/Plugins/MFO.ini) + trigger key `iProgProbeKey = 39`
> (semicolon). Press once → picks first unique-base teammate, dumps the 12 combat-skill
> AVIF trees ([prog] census lines), P1 adds the first unowned ABILITY-entry perk via
> TESNPC::AddPerk+ApplyPerksFromBase (observable: HasPerk + the ability's MGEF in active
> effects, immediate + ~2s settle re-check), P2 writes OneHanded base +10 with §4.2
> reconcile logging. Later presses = status re-read (press after a level-up → P2 verdict).
> Save+reload → P3 GetPerkIndex-guarded reapply at kPostLoadGame logs baseCount/effectCount
> doubling verdict. Wired: Board.cpp hotkey (MainThread::Post, never AddTask), plugin.cpp
> kPostLoadGame, CMakeLists, Config, seed INI. NO version bump, NOT pushed. Watch `[prog]`
> in MFO.log; use a throwaway save (perk + skill writes are real).

> **#74 DESIGN PASS DONE (2026-08-11) — feasibility GREEN.** Full doc: `Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md`. AVIF perk-tree graph + PERK introspection verified in CommonLibSSE-NG → render + gate the real merged trees player-identically, NO Synthesis. Engine surprise: `Actor::AddPerk` is a NPC no-op → `TESNPC::AddPerk`+`ApplyPerksFromBase()` (SPID pattern, already live in LoreRim). ESL = 2 GLOB records, co-save `PRGN`, board 3rd tab, no storage. First build = a PROBE dev build (P1 entry-point-on-NPC / P2 base-AV-vs-autocalc / P3 reapply-idempotency — log-only) before UI. AWAITING marth's calls on: XP rate, veteran catch-up, Shared-Growth default, respec free/paid, MCM-detect mechanism, v1 unique-base-only, + 5 proposed additions.

> **v1.0.61 — creature weapons DELETED, not handed to player** (marth: a creature
> weapon is non-playable, useless clutter on the player too). `HealExcludedWeapon`
> (Logistics.cpp ~3304) now splits eviction like the v1.0.46 ARMOR fix: `IsCreatureWeapon`
> → `RemoveItem(kRemove)` DELETE; `Catalog::IsExcluded`-only (playable quest/unique)
> → `kStoreInContainer` hand-back as before. Also fixed the now-stale "Requires the
> regenerated catalog" comment (IsCreatureWeapon works off the DLL alone). CI-compiled.

> **v1.0.60 — #73 CREATURE-WEAPON LOOT FIX** (Fable-built, dotnet-selftested). User
> report: a follower looted a Giant's Club. ROOT: CrGiantClub (skyrim.esm 0x0461DA)
> + variants are creature weapons Bethesda left UN-flagged (record flags 0), 60 dmg
> vanilla / 200 under Requiem, byte-indistinguishable from a greatsword (kTwoHandSword/
> EitherHand/normal kwds) — MFO's flag-based IsCreatureWeapon + patcher both missed it.
> THREE parts: (1) DLL `IsKnownCreatureWeapon` — magic-static curated set (7 vanilla
> formkeys: 3 giant clubs, 2 sphere crossbows, DLC1FrostGiantClub, DLC2BenthicLurker)
> via LookupForm, OR'd into IsCreatureWeapon. (2) Patcher (Catalog.cs) general rule:
> creature-only-wielded AND creature-convention (Cr[UPPER] EDID or actors\ mesh), hard
> guard refusing Draugr*/AncientNord*, assertion, containers=humanoid evidence, Traits-
> shells skipped — marth's draugr-loot concern PROVABLY handled (dotnet-built + selftest
> vs 123-plugin Requiem: 0 false positives). (3) Fallback mfo_items.json (7 excludes)
> shipped at out/SKSE/Plugins/MFO/ — catalog SUPPLEMENTS heuristics (Loaded() unused).
> Corrections: vanilla flags NO weapon NonPlayable (old DLL comment was wrong, fixed);
> under Requiem the club EDID→REQ_Creature_Giant_Club so the curated list is load-bearing.
> DLL is CI-compiled (green pending); patcher local-built OK. Field-test/public pending.

> **v1.0.59 — CONTROLLER board back/close fix** (Fable-built). Field bug (Deck):
> the Field Orders board broke on controller after a keyboard↔gamepad input-mode
> flip — B stopped backing out/closing, nav went erratic, only Esc closed. ROOT:
> B's ONLY source was ImGui's Win32 backend XInput poll (`ImGui_ImplWin32_NewFrame`),
> which goes deaf on a Steam Input mode flip AND rewrites `HasGamepad` every frame
> (so a deaf poll killed ALL hook-fed gamepad nav, not just B) — confirmed against
> imgui 1.92.8 source; route (b) "disable the poll from outside" is impossible there.
> FIX (route a): VENDOR `imgui_impl_win32.cpp/.h` (byte-exact upstream v1.92.8) into
> native/, drop vcpkg's `win32-binding` feature, compile the vendored TU with
> `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD` (XInput poll compiled OUT). B now forwarded from
> the input hook as `GamepadFaceRight` (Skyrim ButtonEvent stream, survives flips) —
> single gamepad source, original double-B race structurally gone. Plus a stuck-key
> sweep (`io.ClearInputKeys()` on board open) so a close-press's swallowed release
> can't eat the next session's first B. **CI proves compile/LINK (watch for
> duplicate ImGui_ImplWin32_* symbols = stale imgui restored); controller behavior
> is FIELD-TEST ONLY.** Deck checklist handed to marth. See [[imgui-backend-polls-xinput-itself]].

> **v1.0.58 — CONCENTRATION FREEZE FIX + bounded concentration casting** (jumped
> 53→58 per marth). ROOT (deck-diagnosed live: all threads parked, log dead, no
> crash): Lucien force-cast concentration Flames (00012FCD) as a permanent held
> stream → sprayed teammate Xelzaz → friendly-fire ally combat → the #63 quash
> called StopCombat re-entrantly from the TESCombatEvent dispatch AND from the
> job-worker → lock-inversion deadlock. TWO fixes (Fable-built, adversarially
> reviewed, all findings repaired): (A) `Rapport::QuashAllyPair` — both quash
> sites now route StopCombat through the main-thread pump (SKSE AddTask on VR) with
> a per-pair 2s cooldown + best-effort combat-target drop; never inline in the
> dispatch, never worker-side. (B) concentration spells no longer force-streamed:
> `Actuation::ConcentrationCast` package-drives a BOUNDED hold (hostile 1-4s via
> Temperament / heal until ≥95% HP cap 6s / utility 4s cap) + explicit release
> (marker-evict + InterruptCast all sources), gated by `Sightline::TeammateInFireLine`
> (no ally in the beam, pre-cast AND mid-stream). `CasterConsent::ConcUnboundedDeny`
> closes the AI channel so nothing streams unbounded — **EXACT-ONLY (marth's call):
> fires only at cast-control 4; levels 1-3 leave a follower's own AI concentration
> alone (vanilla-safe/self-bounded).** Principle recorded: [[exact-bounding-covers-all-spells]].
> kRunTimeout(12s) backstops every exit; no new co-save fields. VR re-entrancy also
> closed (M1). Field-test pending marth.
> **FOLLOW-UP #72 DEFERRED (marth, only-if-needed):** framework followers (Lucien/
> Xelzaz) are §4.6-declined for the package-driven bound (quest owns them at 80/99 vs
> MFO 60, #64), so v1.0.58's cap + FF gate don't reach them — they cast concentration
> via their own AI, unmanaged-but-safe (deadlock fix + AI self-bounds). Fable scoped a
> monitor-and-interrupt fix via the hook layer (bound the AI's own stream, never touch
> their package; ~150-250 lines + a Deck interrupt-cleanliness probe; also memoize the
> §4.6 verdict to kill [pkg] DECLINED log spam). Build ONLY if a framework mage's
> unmanaged concentration causes an observable field problem.

> **v1.0.53 — DEPLOYED to Tuxborn** (DLL `75bb3c00`, ESP `a05c1b1d`, both deck-verified; tag `v1.0.53` pushed). Field-test PENDING marth: melee/ranged followers should hold ground / keep bowman spacing instead of back-pedalling like casters. CSTY combat-pathfinding fix. MFO's three combat-style records
> (MFO_MeleeStyle/RangedStyle/CastStyle) were copying their close-range positioning
> block (CSCR: circleMult/fallbackMult/flankDistance/stalkTime) byte-verbatim from a
> vanilla MAGE style (csHumanMagic 0x3BE1C — circle 0.30 / fallback 0.50), so a
> follower forced to melee or ranged *moved* like a kiting caster (low circle, high
> fallback) even though its scoring (CSGD) was right — that's why battle pathing felt
> worse than vanilla. Fix: each MFO style now carries its MATCHING vanilla CSCR —
> Melee = csHumanMeleeLvl1 0x3BE1B (circle 0.73 / fallback 0.42), Ranged =
> csHumanMissile 0x3BE1D (circle 0.45 / fallback 0.65), Cast unchanged. ESP-only
> change (generator: `_csty_record` takes a per-style `cscr`); DLL rebuild is
> functionally identical (version stamp only). Regenerated + audit PASS. Deploying.

> **v1.0.50:** #65 = per-follower Combat Class dropdown on the board (Auto/Melee/
> Ranged/Mage), Auto=no-override (custom-follower-safe #64), forces the CSTY stance
> in Scheduler, co-save FLWR v3→v4. `cond.dark` = interior-OR-night condition (for
> "when dark → Magelight"). LOOSE-LOOT FIX: field log showed a follower cycling
> REACHABLE loose gold forever, stalling ~221u short of the 160u corpse arm's-reach
> and never triggering the Activate. Now loose items acquire from `fLooseAcquireDist`
> (default 300, INI-tunable) since Activate is distance-independent; unreachable/
> unacquirable piles go STICKY instead of looping. All three Fable-PASS. v1.0.49
> Tuxborn field-test (Feris bow / hybrid / Magelight) + v1.0.50 all pending marth.
> Public GitHub release of v1.0.49/50 HELD until field-confirmed. Next: TOWN UPDATE (#31).

> **v1.0.49 (#68 + #69), marth's combined drop.** #69 = stock-loadout snapshot
> (co-save `MSTK` record, snapshot at first management, never shed/displace stock
> gear; shared stable `ComputeWeaponRoles` for loot+shed; mage kept out of the
> weapon-upgrade path). Fixes Feris's Gauldurbow give-away + the 1h/bow thrash.
> #68 = cast-target ladder (selector → specific-follower → subject → PLAYER
> fallback; Subject::Self(0) = AUTO on cast-at-target rows), player-by-name +
> nearby-follower Target picker on the board, FLWR schema v2→v3 (subjectActorForm),
> spell-range transparent skip. Both agent-built + reviewed (mage-loot regression
> caught & fixed; Fable PASS; `<limits>` + lock-across-GetInventory advisories
> applied). FIELD TEST on Tuxborn: Feris keeps her bow; a hybrid keeps 1h+bow; a
> "when dark → Magelight" gambit lights up around the player. **#70(A)** (config/
> mod-authored gambit tables) SHELVED for a later update. After this: back to the
> **town update (#31)**.

> **PRIORITY LIST STATUS (#62-67):** #62 ✅ shipped+confirmed (v1.0.46). #63 ✅
> coded (v1.0.42+, in v1.0.46), field-test pending. #64 ✅ audit-clean — Packages
> is additive-by-design, CombatStyle overrides only the per-combat controller (base
> style untouched); nothing to fix. #65 = per-follower combat-type DROPDOWN, the one
> remaining real feature build. #66 ✅ v1.0.48 — REAL bug was LOTD drop-off boxes
> in TOWNS/INNS (public cells, ref-unowned, no keyword): matched by container BASE
> form (IsLOTDDropOff, LegacyoftheDragonborn.esm locals 07EEFD/1772A6/166349/
> 0BE533/11CC99), skipped unconditionally. Plus museum halls + player homes via
> LocTypePlayerHouse 000FC1A3 / player-owned cell (gated by bLootInPlayerHomes).
> #67 ✅ v1.0.48 (cast-control gated AE-only in Actuation::CastOn; SE 1.5.97 crash
> was Scheduler->Fire->CastOn on the job worker, pinned via CI PDB + llvm-symbolizer).
> **#68** (cast targeting: default-to-player as the LAST rung, player-by-name +
> nearby-follower picker, spell-range skip — marth-confirmed) and **#69** (hybrid
> 1h/bow loot: loot whitelist vs wielded-selection reconcile) queued after v1.0.48.

> **#62 invisible head — ROOT-CAUSED & FIXED (v1.0.46), record-verified. The whole
> v1.0.39-45 line was WRONG** ([[off-main-equip-invisible-head]]). It is NOT a 3D
> rebuild / beast-race problem. **MFO looted a NON-PLAYABLE creature item (a draugr
> helmet, DraugrHelmet01 0x1FD77) off a corpse and equipped it** — it renders on no
> playable race → invisible head. The ARMOR twin of the creature-WEAPON bug
> (IsCreatureWeapon). Proven headless: Inigo is STOCK KhajiitRace (not custom), has
> no outfit; the "Ancient Nord Helmet" was a worn draugr helmet (count 2). Vanilla
> never loots creature gear onto a follower → that's why it never happens without
> MFO. **Fix:** `KeepHeadClear` rewritten (Logistics.cpp ~380) — one pass over worn
> armor: renders-on-race → keep; own-plugin non-rendering → keep (#64); foreign
> NON-PLAYABLE → DELETE; foreign playable non-rendering HEAD item → hand back to
> player. Runs on load (all teammates) + equip-event (catches trades). Prevention
> (IsCreatureArmor loot skip) already shipped v1.0.41. `IsBeastRace`/DoReset3D/
> Update3DModel all removed. Fable-reviewed PASS. Watch `[evict]` in MFO.log.
>
> **v1.0.46 DEPLOYED to Tuxborn** (DLL `5bd89e17`, deck-verified). **FIELD TEST:**
> load a broken Inigo (6E008AE9) → the on-load sweep should DELETE the worn draugr
> helmet and his head returns; then TRADE gear (the case that broke) → must hold.
> Watch `[evict] … DELETED NON-PLAYABLE creature armor` in MFO.log (deck path per
> [[deploy-workflow]]). Deploy overwrote MFO.ini so `bBeastHeadFix` is back to 1.
> This is race-agnostic — any Khajiit/Argonian/human follower with stuck creature
> gear is fixed the same way. **#63 quash** (inter-follower hostility, `[peace]`,
> `bQuashAllyCombat`) rides in v1.0.42+ — still field-untested.

---

## Continue in one screen

- **v1.0.41 — DEPLOYED TO TUXBORN + TAG PUSHED (`v1.0.41`), field-test PENDING; NO
  GitHub/Nexus release yet.** DLL `746b491a`, CI run 31401481609 green,
  `releases/v1.0.41/`, deck-verified on Tuxborn. **⚠️ marth field-tests in
  `Tuxbornrc1`, NOT custom-modlist, and Tuxborn is NOT syncthing-linked — deploy
  DIRECTLY over SSH** ([[deploy-workflow]]); Tuxborn log
  `/home/deck/Games/Tuxbornrc1/overwrite/SKSE/Plugins/MFO.log`. **#62 fix is now in
  FOUR layers** ([[off-main-equip-invisible-head]]): v1.0.38 main-thread equip →
  v1.0.39 beast `DoReset3D` (loot CONFIRMED) → v1.0.40 equip-event sink
  (covers TRADE/AI, field-test pending) → v1.0.41 ON-LOAD sweep (heads fix
  themselves on load, no trigger needed) **+ creature-skin armor filter**
  (`IsCreatureArmor` = NonPlayable flag; stops looting MNC "BearBrownSoft").
  **Field test (Tuxborn):** load a save with a headless beast follower → head
  fixed on load (`[beasthead] … 3D reset on load`); trade a weapon to a beast
  follower → holds (`… on equip event`); confirm no `BearBrownSoft`-type creature
  armor looted. If clean, publishable (`gh release create v1.0.41` — main agent).
  Known gap: IN-COMBAT equips not reset (flicker-avoidance). Off-main siblings
  still un-reset: EquipBack (Loadout.cpp:91), HealExcludedWeapon, torch.
- **NOW IN PROGRESS — #63 follower-vs-follower hostility.** First finding: `PickFoe`
  (Evaluator.cpp:223) already skips any non-`IsHostileToActor(self)` target, so MFO
  never SELECTS a friendly teammate as a foe → the bug is upstream: a follower
  genuinely BECOMES hostile to another, prime suspect FRIENDLY FIRE from forced
  casts/attacks hitting a teammate (ties to the parked friendly-fire-hold, gated
  off because the cast hook runs off-main). Investigate the forced-cast/attack
  target + AoE path next.
- **#62 history (superseded by v1.0.40 above; full detail in
  [[off-main-equip-invisible-head]] + CHANGELOG):** v1.0.38 = loot equip moved to
  the main thread (`MainThread::Post`); v1.0.39 = beast `DoReset3D` after MFO's
  equip (field-confirmed for loot, but trade still broke → v1.0.40's equip-event
  sink). `bBeastHeadFix` INI kill-switch. **Load-time caveat still open:** if a
  beast head is broken the instant a save loads (before MFO acts), add a load-time
  beast-head sweep (DoReset3D on beast teammates at load, furniture-sweep shape).
  Same off-main class NOT yet given the reset (do NOT DoReset3D mid-combat —
  flicker): EquipBack shield restore (Loadout.cpp:91), HealExcludedWeapon + torch,
  combat equip gambit (Actuation ~500).
- **Latest shipped & deployed: v1.0.37** ("mage follow-up") — deck-verified (DLL
  b0602fa2…), GitHub Release = Latest. Bundles the field-fixes over the v1.0.34
  mage update: (1) cast control that STICKS — deny the wrong spell PRE-charge via
  the MagicCaster::CheckCast hook (0x0A, ActorMagicCaster vtable[0]; the v1.0.35
  SpellCast/0x09 release-abort was reverted — it animated + wedged the caster) +
  Loadout::Prepare keeps the GAMBIT spell in hand (spell→spell swap); (2) effect-
  based spell classification (any hostile effect → Offense, fixes Absorb Health);
  (3) cast-in-logistics FIXED — self-casts via CastSpellImmediate (package bars
  self-delivery, Decline::SelfRoute), foe-casts via CastAt; per-(follower,spell)
  pace; (4) NEW `act.cast_player` (logistics-only). Fable-reviewed (3 MAJORs fixed:
  per-spell pace key, skip empty cast_target, cast_player-not-in-combat). Field:
  candlelight OOC confirmed working; the CheckCast hook fires NON-main (so the
  friendly-fire hold is inert -> the v1.0.37-below LoS review). **v1.0.35/1.0.36
  were TEST-DEPLOYS only (never public) — superseded by v1.0.37.**
- **Latest shipped & deployed:** **v1.0.34 — "the mage update"** — deck-verified
  (DLL 6f8f64ef…), GitHub Release = Latest (CI run 31130000975; auto-trigger kept
  flaking so runs were `gh workflow run`-dispatched, and GitHub concurrency
  cancelled several — the release poll now re-dispatches on a cancel). SEVEN
  pieces in one cut:
  1. **Cast-control slider** (`iCastControl`, MCM, default 2 "ignore heals"):
     off → ignore buffs+heals → ignore heals → ignore self-heals → exact. The
     consent hook classifies the AI's own spell by the deliberating caster's
     vtable (Offense/Buff/Heal, heal split self/other by Delivery) and denies
     only the non-exempt categories. `CasterConsent::CastExempt`.
  2. **`MFO_CastStyle` cast stance** (reuses CSTY 0x832): a cast-latched follower
     is swapped to pure-mage; magicka-dry → pure MELEE, back on regen. Driven by
     the scheduler on the CombatStyle/UpdateCombat rails (v1.0.33's).
  3. **Cast in logistics** — `act.cast_*` runs in the logistics table too
     (out-of-combat self-buffs/candlelight/heals) via `Actuation::Fire`; a still-
     active self-buff is skipped (HasMagicEffect).
  4. **Teach spells from spellbooks** — the picker lists player-carried teachable
     spells "Name (spellbook)"; a 2nd click (DESTROYS-book warning) AddSpells +
     consumes the book (`EditKind::TeachSpell`, main-task ApplyEdits).
  5. **#56** combat-HUD X/Y margin sliders (clamped on-screen).
  6. **#61** fashionrim `bDollsMode` (armor acquisition+fitting off; sell stays).
  7. **#55 MCM fix** — ships `MCM/Config/MFO/settings.ini` (the defaults file MCM
     Helper registers from) + `audit_mcm.py` 5-touchpoint release gate.
  Fable-reviewed pre-release (caught a compile blocker + 4 picker MAJORs, all
  fixed). A4 (act.attack casts the set magic attack) SKIPPED per marth. **Field
  test pending** — watch items below.
- Field-test watch (v1.0.34): the cast slider at each stop (does a mage keep
  healing at "ignore heals" but cast the gambit for offense?); `[wstyle] … cast`
  + the dry→melee swap; a cast gambit in the LOGISTICS table firing out of
  combat; teaching a spell from a book (book consumed, spell set); fashionrim
  stops armor loot/robes but weapons still work; HUD sliders move the overlay.
- **v1.0.33** ("weapon-stance ownership") — Auri melee fix, deck-verified + FIELD-
  VALIDATED (combatStyle swap holds, 0 re-derives). **v1.0.32** ("mage fixes") —
  casting "drastically improved" per marth.
- **Compile is CI-only.** No local MSVC. The DLL only ever comes from a GREEN
  GitHub Actions `native` run whose `native/` tree matches HEAD. Never trust
  "it built" without `gh run list --workflow=native --status=success` + a
  headSha/tree match. (INVARIANTS #44; verify-ci-green memory.)
- **ESP is generated** by `python3 MFO_GenerateESP.py out` (Linux, no Creation
  Kit). ESP changes = edit the generator + regen. (ARCHITECTURE §8.)
- **Release** = two-phase `release.sh` (see its header). Phase 1 stamps+pushes→CI;
  phase 2 (after green) regens ESP, runs `audit_esp.py`, pulls the green DLL,
  packages `releases/vX/`.
- **Deploy** = unzip the package over `/mnt/gaming/modlists/custom-modlist/mods/MFO/`
  only (keep `meta.ini`), force a syncthing rescan, then poll the deck until its
  DLL sha256 matches the packaged one. (deploy-workflow memory has the exact
  commands + the syncthing key handling.)
- **Publish hygiene:** subagents do code→CI→deploy→tag; the **main agent** (in
  direct conversation with marth) runs `gh release create`. Keeps the public
  publish anchored to marth's authorization. (release-scope memory.)
- **Repo hygiene:** only `main` exists locally and on origin (feature/worktree
  branches were cleaned 2026-08-05). GitHub Releases exist for every tag
  v0.7.0→v1.0.31 (the v0.8.24–v0.8.48 gap and all v1.0.x were backfilled).

## Shipped this cycle (all deployed to deck + GitHub Release)

| Ver | What | Field-test status |
|---|---|---|
| v1.0.25/26 | Furniture-ejection fix: evict follower to an XMarker (never the player) + load-sweep un-latches old saves | marth reported furniture works; treat resolved |
| v1.0.27 | Hybrid forced-cast (force the gambit spell on AI miss) + line-of-sight gating (worker posts, raycast on main thread, cached verdict) | **pending** — watch `[los]`; does `HasLineOfSight` discriminate NPC→NPC on this runtime? (fail-open by design) |
| v1.0.28 | Exclusive cast control — while a cast gambit owns him, DENY every spell but the gambit's (was casting his own too) | **pending** — watch `[consent] … DENIED own spell` |
| v1.0.29 | Magic-user loadout — school robes + backup dagger + 2 MCM toggles (bMagicLoadout, bMageDaggersOnly) | superseded in part by v1.0.31; MCM toggles BROKEN (see #55) |
| v1.0.30 | Cast latch persistence — suppression holds through the cast cooldown, not just the fire (closes the between-casts leak) | **pending** — watch `[consent] … holding exclusive control through the cast cooldown`; no `(their own spell, not ours)` while a cast rule wins |
| v1.0.31 | Pure casters: mages loot NO rated armor (heavy or light), robes/clothing only; broadened school-robe detection; per-candidate apparel diagnostics; no junk picks | **pending** — watch `[loot] apparel …` diag lines; is Marcurio switching to his valid robe / out of heavy armor? |
| v1.0.33 | **Weapon-stance ownership** — equip gambits own the follower's live per-combat `combatStyle` (melee/ranged CSTY 0x833/0x834), applied on the combat thread from the UpdateCombat hook; reverts at battle end, hands off melee↔ranged. Stance follows the winning gambit, not the class (mage-melee / mage-ranged work). Default ON, `bWeaponStyleControl` INI kill-switch. | **pending** — watch `[wstyle] … OWNED/HANDOFF`; does Auri now hold the mace and stop plinking? Does she stop the bow↔mace flicker? |
| v1.0.32 | **Mage fixes** — forced casts actually fire (FindInput template-map fallback + static uids; CastAt only, cast_self stays barred: QNAM+t6 zero-precedent cell); cooldown-consulted permit (`permitAfter` on the latch — no more 4-casts-in-2.2s bursts); potion-exempt deny (only formType Spell is ever denied); flicker-proof latch (deny holds for the combat's duration; releases only on combat end/dismissal/revert); + INI-gated P1 combat-style probe (`bProbeCastStyle`, default OFF, new CSTY 0x832 `MFO_CastStyle`) | **pending** — watch `[cast] … FORCED … at …` (real animated force, no more `template input 'Spell'` errors), `[consent] … pacing` once per window, no suppressed combat drinking, `DENIED own spell` continuity across flickers; probe (only if armed): `[probe cstyle]` |

## Open field threads (awaiting marth's deck — pull the log, then diagnose)

The deck runs the game; MFO.log is at
`deck@marthdeck:~/Games/custom-modlist/overwrite/SKSE/Plugins/MFO.log`.
**CAVEAT: MFO.log is TRUNCATED every game launch** (one file, no backup) — the
session with a bug is gone after a relaunch. To catch something, marth must
reproduce it in the CURRENT session, then pull the log before the next launch.
(Offered but undecided: add an `MFO.log.prev` backup-on-load so sessions aren't
lost — small, safe; do it if marth confirms.) Deck may be ASLEEP → SSH timeout,
just retry ([[deck-sleeps-ssh-timeout]]).

1. **Auri melee switch — FIXED + FIELD-VALIDATED (v1.0.33, 2026-08-06).** Deck
   log: clean `OWNED -> ranged`, `HANDOFF -> melee`, `HANDOFF -> ranged` — she
   holds the stance, no more 6×/fight bow↔mace thrash. **`RE-DERIVED: 0`** — the
   live `combatStyle` swap HOLDS on 1.6.1170, engine never stomps it. That
   field-proves the CSTY-swap rails for Stage 2 (caster style swap) and makes the
   P1 probe fully unnecessary. History below:
   The deck log showed it was NOT the STATUS hypotheses: she carries a Glass Mace
   (dmg 84), `rule 1 act.equip_melee` fires, and she DID equip melee 6× — but
   the equips were *real* re-equips (a bow↔mace tug-of-war with her own ranged-
   weighted AI), so she flickered and never attacked. Fixed by weapon-stance
   ownership (v1.0.33): the melee gambit now swaps her live combat style so the
   AI stops re-drawing the bow. Confirm on deck: `[wstyle]` shows OWNED melee,
   she holds the mace, no flicker.
2. **P1 combat-style probe — largely OBVIATED by v1.0.33.** The probe's core
   MECHANICAL question ("does writing `combatStyle` on the live per-combat
   controller HOLD, or does the engine re-derive it?") is now answered for free
   by the v1.0.33 weapon-stance feature, which does exactly that swap on the
   RELIABLE UpdateCombat hook (all combatants) and logs re-derives explicitly
   (`[wstyle] … engine RE-DERIVED …`). So `bProbeCastStyle` need not be run
   separately for that. The probe's OTHER question (does a caster-forward style
   make him cast MORE / mobile) is cast-specific and folds into Stage 2 below.
3. **v1.0.31/1.0.32 field confirms** — see the shipped table's watch items
   (Marcurio switching to robes; forced casts firing animated, paced, potions
   flowing, no mid-flicker leak).

## Active priority list (marth 2026-08-09) — tasks #62–#67

1. **#62 P1 HIGHEST — follower HEAD DISAPPEARS on armor equip. DIAGNOSED + FIXED
   (2026-08-10, in CI as of run 31350989371; field test pending).** ROOT CAUSE is
   NOT form data — it's the EQUIP MECHANISM running OFF the main thread. The loot
   tick runs on the SKSE AddTask worker (Scheduler::Tick <- Diagnostics sleeper
   AddTask, BSJobs::JobThread), and it called `EquipObject` directly there;
   EquipObject rebuilds the biped 3D (head/neck partition, even for a plain CHEST
   piece), so off-main it races the render thread and the head node is torn down
   but not rebuilt. Proven by marth: reproduces with a GOOD item (chainmail), so
   it's the equip, not the meshes. A Fable pass empirically killed the first
   (form-data) approach: it would over-block ~760 legit vanilla armors, and a
   blank female mesh is NOT invisible (engine falls back to male model).
   FIX (Logistics.cpp ~1068): capture FormIDs, re-resolve on the frame, EquipObject
   via `MainThread::Post`; VR falls back to direct via new `MainThread::IsInstalled()`.
   MEO gem transfer unaffected (QueueGemMove already deferred). See
   [[off-main-equip-invisible-head]]. FIELD TEST: watch `[equip] … LOOT armor …
   -> equip queued to main thread`; hand a follower chainmail / let them loot a
   chest piece — head must stay. FOLLOW-UPS (same off-main class, likely also part
   of the crash hunt): EquipBack shield restore (Loadout.cpp:91), HealExcludedWeapon
   + torch (Logistics 2822/2863), combat equip gambit (Actuation ~500).
2. **#63 — followers turning hostile toward OTHER followers.** Check PickFoe /
   Targeting never latches a teammate/another follower as a foe.
3. **#64 — don't break custom follower packages (Lucien/Inigo/Kaidan).** "Good so
   far" — regression-watch that alias claims/evictions/cast+style changes don't
   clobber their bespoke packages. Ties to #58.
4. **#65 — per-follower combat-type override: a DROPDOWN on the board** right of
   the combat/logistics selector. Promote the P1 combat-style swap (#59, CSTY @
   combatController 0x38) to a user-facing, per-follower, persisted setting.
5. **#66 — LOTD: followers loot the museum drop-off crates.** Exclude LOTD museum
   containers from the loot scan (by keyword/quest/faction, not hardcoded FormID).
6. **#67 P6 LOWEST — spell casting CTD on SE 1.5.97.** Gate the cast path's
   AE-only/vtable bits so SE degrades gracefully (like the VR guard).

## Open issues (ranked)

0. **✅ SHIPPED in v1.0.34 (the mage update) — casting overhaul + full cast
   control.** Stage 2 landed as the graduated `iCastControl` SLIDER (not the
   single `bFullCastControl` toggle first sketched) + the `MFO_CastStyle` stance
   with magicka-dry→melee. Field test pending (watch items in "Continue"). Only
   the act.attack-casts-set-magic half was cut (marth). Historical detail below.
   HEADLINE — casting overhaul (next Nexus = "mage fixes"). Research (task #59)
   found the v1.0.27 forced cast had **never fired on deck** — it errored
   `template input 'Spell' is not declared on FE090820` and silently fell to the
   invisible `CastSpellImmediate`. **v1.0.32 SHIPPED (task #60), deck-verified:** fixed the
   dead forced cast (Packages.cpp `FindInput` — template nameMap on a MISS not
   just null; static uid fallback Spell=3/Target=4; scoped to CastAt — CastSelf
   stays barred via `Decline::SelfRoute`, the CTD-class t6+QNAM cell), cooldown-
   consulted permit (`permitAfter` on the latch), potion-exempt deny (only
   formType==Spell), flicker-proof latch (combat-duration hold), + the INI-gated
   **P1 combat-style probe** (`bProbeCastStyle`, default OFF; CSTY 0x832) to
   de-risk Stage 2. Field test pending (watch items in the table above).
   **Stage 2 (#59, the headline toggle `bFullCastControl`, default OFF) — NEXT,
   IN PROGRESS.** marth green-lit it 2026-08-06 ("just in case people want very
   tight control of casters"), to build right after v1.0.33 deploys. Own every
   decision the AI's cast machinery consults (deny-all + spell-scoring 0x0C +
   CalcCastMagicChance 0x08 + swap combatStyle 0x38 to an MFO caster CSTY) →
   100% vanilla-animated+mobile, MFO owns what/when/whom. **The combatStyle-swap
   leg now rides the PROVEN v1.0.33 rails:** extend the unified CombatStyle
   ownership with an `MFO_CastStyle` stance driven by CAST gambits on the
   UpdateCombat hook, instead of the fragile CheckStartCast-driven probe. Still
   gated on 5 open design questions for marth (force the exact gambit spell vs
   bias-to-cast; caster-CSTY aggressiveness; override target selection too; etc.
   — see task #59).
   **Also folded into this mage update (marth, 2026-08-06):** **#61 fashionrim**
   (armor-only dolls toggle — disables armor looting, the mage school-robe
   loadout, armor auto-equip, MEO gem transfer, armor buy/sell; weapons
   untouched). #61 is specified as an MCM toggle and **depends on #55** (broken
   MCM registration) or it's a dead checkbox — so the mage cut must either fix
   #55 first or ship #61 INI-gated like `bWeaponStyleControl`/`bFullCastControl`.

1. **✅ FIXED in v1.0.34 — #55 MCM new toggles = empty checkboxes on existing
   saves.** Root cause: MFO never shipped `MCM/Config/MFO/settings.ini` (the
   author defaults file MCM Helper REGISTERS from); it shipped only the mutable
   user store, which MO2 shadowed with a stale copy. Now ships the defaults file
   + `audit_mcm.py` release gate (every control wired in all 5 places or the
   build fails). Verified against SkyUI/Precision/TDM/TrueHUD (which ship it).
   Historical detail below.
   #55 — MCM new toggles = empty, unresponsive checkboxes on existing saves.**
   RECURRING ("as per usual"). All file touch points verified correct (atomic,
   parse, ResetToDefaults, kMcmDefaults heal table, config.json control, Settings
   ini, seed ini); live deck store has the keys = 1 under `[General]`; no MCM
   parse error. So it's MCM Helper NOT REGISTERING the new ModSettings for an
   existing save (control draws, nothing bound). `EnsureMcmDefaults` (append to
   store ini at kInputLoaded) is insufficient. FIX: diagnose empirically (diff a
   working installed mod that adds toggles across updates — the "read the data"
   rule) + add a `tools/audit_mcm.py` **release gate** checking every toggle's
   touch-points are consistent, so broken MCM can't ship. marth: "please never
   release broken mcm options."
2. **#48 — retreat didn't relocate; StopCombat fix (v1.0.24) needs validation.**
   Watch `[retreat] moved=` in the next outmatched fight; escalate to a Flee
   package if `moved≈0`.
3. **#24 — retreat threshold tuning** (fire sooner, chase-leash) — pending values
   with marth.
4. **#31 — autonomous town errands** (walk-to-merchant / door nav) — the headline
   NEXT FEATURE; generalises the loot-travel `Packages.*`/`Logistics.*` machinery.
5. **#29 — MEO v1.0.8** finalize (strip `[wdiag]`, bump, release) — *different mod*
   (marth's Equipment Overhaul), same dev flow.

## v1.0.35 — BUILT + DECK-DEPLOYED FOR FIELD TEST, PUBLIC RELEASE HELD

**Deployed to deck 2026-08-06 (DLL d09e1a72, CI run 31137713645). NOT tag-pushed
/ NOT gh-release'd** — this is a high-risk cast-abort hook; the GitHub release
waits on marth's freeze/CTD field-test. To publish once he confirms:
`git push origin v1.0.35` + `gh release create v1.0.35 …` (main agent). Fable-
reviewed twice (2 blockers + majors fixed). **Field-test watch:** no follower
freeze/CTD; `[consent] SpellCast fires on the MAIN/NON-main thread` (one-shot —
tells if friendly-fire is active); `HARD-ABORTED … (exclusive)` in "exact";
`HARD-ABORTED … (friendly fire)` instead of Fireballs-in-the-back; Absorb Health
now obeys "ignore buffs & heals". Design detail below.

1. **HARD cast suppression ("the hard way", marth).** CheckStartCast (0x06) is
   advisory — a denied non-gambit spell still fires (deck: Marcurio cast Icy
   Shard/Turn Undead/Mage Armor while latched in "exact"). FIX: also hook
   `MagicCaster::SpellCast` (VTABLE_ActorMagicCaster[0..2] idx 0x09) — the real
   cast-execution — and ABORT (don't call original) when a latched follower's
   spell is denied. Shared decision with CheckStartCast.
2. **Effect-based spell classification** (replaces the caster-vtable category).
   Classify by the spell's effects: any hostile/detrimental effect → Offense
   (fixes marth's Absorb Health complaint — it was tagged Heal by the Restore
   caster); else beneficial Health effect → Heal (self via Delivery); else Buff.
   `EffectSetting::IsHostile()/IsDetrimental()`, `data.primaryAV==kHealth`.
3. **Friendly-fire hold.** The engine's HasLineOfSight passes THROUGH allies, so
   MFO fired Fireballs into Auri's back in a hallway. FIX: in the SpellCast hook,
   for an OFFENSIVE spell, abort if a teammate (`IsPlayerTeammate`) is within the
   spell's AoE (`GetLargestArea()`) of the target OR on the caster→target line
   (point-near-segment). INI kill-switch `bFriendlyFireHold` default ON.
   (Drop the earlier wrong idea of "ignore allies in the LoS raycast".)

## ⚠️ TOP PRIORITY NEXT — CRASH INVESTIGATION (marth: strong suspicion MFO causes the memory/graphics CTDs, 2026-08-06)

Crashes trump features. Start here. MFO's plausible crash surfaces, ranked:
1. **The ImGui/Field-Kit overlay (Board.cpp) — #1 GRAPHICS suspect.** Three RENDER-
   thread hooks (D3DInit, DXGIPresent, InputDispatch) + an every-frame overlay +
   baked fonts. (The Win32 backend's XInput poll ([[imgui-backend-polls-xinput-
   itself]]) is GONE as of the v1.0.59 fix: imgui_impl_win32.cpp is vendored and
   compiled with IMGUI_IMPL_WIN32_DISABLE_GAMEPAD; the input hook is the single
   gamepad source, B included.) Graphics/device-lost/present crashes usually
   live here.
2. **Off-main engine MUTATIONS added v1.0.35-37 (memory-corruption/UAF suspects).**
   The CheckCast hook (ActorMagicCaster vtable[0] idx 0x0A) fires on a NON-main
   thread (confirmed in the deck log) and calls GetCasterAsActor + ShouldDeny on
   EVERY cast; `CombatStyle::ApplyTick` writes cc->combatStyle on the UpdateCombat
   thread; cast-in-logistics calls CastSpellImmediate + RestoreActorValue + a
   PLAYER `HasMagicEffect` walk from the job-WORKER tick (§0.30). Any off-main
   engine write/list-walk racing the main thread = corruption.
3. **The vtable hooks written at load** — UpdateCombat 0xE4, CheckStartCast 0x06,
   CheckCast 0x0A. A layout/index mismatch would corrupt (Fable already caught the
   VTABLE_ActorMagicCaster[1]/[2] clobber; verify none like it remain).

METHOD (don't guess — [[getgoldamount-ctds-count-gold-from-getinventory]] has the
CI-PDB + `[bc]` breadcrumb crash-pinning recipe): (a) have marth install/keep a
crash logger (Crash Logger SSE / .NET SF) and grab the crash .txt; symbolicate the
top frames against the CI-shipped MFO.pdb. (b) BISECT via INI kill-switches to
localise the subsystem before reading code: `bShowHud=0` (overlay off), `bCasterHook
=0` (CheckStartCast+CheckCast off), `bWeaponStyleControl=0` + `iCastControl=0`
(combatStyle swaps off), `bLogistics=0` (OOC casts/loot off). If a kill-switch
combo stops the CTD, that subsystem is it. (c) THEN read the pinned frames' code.

## v1.0.38 — LINE-OF-SIGHT review (after the crash hunt) (marth flagged 2026-08-06)

Two separate LoS problems, NOT fixed by v1.0.36 (which only fixes which spell is
cast, not aim/LoS):
1. **Shooting walls/floors.** Sightline uses the engine's `Actor::HasLineOfSight`
   (Sightline.cpp:65), which fails-open on "unknown" and only holds on a confirmed
   "occluded" (Actuation.cpp:56-63). An unreliable/unknown verdict lets a forced
   cast fire into terrain. FIX: a real geometry raycast (caster eyes -> target),
   not the engine boolean — treat a solid hit before the target as occluded.
2. **Hitting teammates on occasion.** The friendly-fire hold (v1.0.35, in the
   cast hook) is gated OFF because CheckCast/SpellCast fire on a NON-main thread
   ("[consent] ... fires on a NON-main thread"), so the highActorHandles walk is
   skipped (UAF-safe fail-open). FIX: snapshot teammate positions on the main
   thread (the MainThread pump / evaluator tick) into a lock-free structure the
   off-main hook reads; do the AoE/line check against the snapshot.
Both want a focused review + Fable pass (touches the raycast + threading).

- **~~BUG: cast-in-logistics OOC~~ — DIAGNOSED + FIXED (2026-08-06, pending field-test).**
  Root cause: the package route DECLINES cast_self with reason=8 (Decline::SelfRoute
  -- the QNAM+t6 CTD cell it was barred from), so Candlelight was refused every
  tick (deck diag: `OOC cast DECLINED by package reason=8, self`). FIX: route
  cast_self through CastSpellImmediate (Actuation's proven self-delivery path) --
  effect applies, no charge animation; affordability-gated + cost deducted by hand.
  cast_target keeps the package (CastAt) route. Deployed as a test DLL.
- **ENH: add cond.is_dark (ambient light level), better than is_night** (marth):
  a dark dungeon by day needs light; a lit town at night does not. A light-level
  read (interior ambient / GetLightingRun-style) beats the clock. Pairs with the
  cast-in-logistics fix above (the motivating use: auto candlelight/magelight).

## Backlog (captured, not scheduled)

- **TOWN UPDATE (next after the mage cut) — headline #31 autonomous town errands
  + economy gear BUY.** marth 2026-08-06: the mage update is the current cut; the
  next is the Town Update (walk-to-merchant/door nav, #31) and gear-buying at
  vendors folds in naturally (buying IS a town errand).
  - **Economy armor/weapon BUY:** the economy currently BUYS only consumables
    (Logistics `addNeed`: kPotHealth/Stamina/Magicka, kArrows, kBolts); selling
    already covers weapons+armor. Buying is NATIVE-ONLY (Papyrus hands all stock
    to `PlanBuy`) but must buy UPGRADES only, "same restrictions/features as
    looting" (marth) — so factor the loot-upgrade predicates out of the
    monolithic `LootNearby` loop (weapon-class match; `ArmorIsBetter`; the mage
    school-robe path via `MageApparelIsBetter`+school), add NeedCat kWeapon/kArmor
    + `ClassifyBuy` cases, gate armor buy behind #61 `bDollsMode`, and generate
    gear buy-needs from the loot_equipment gambit. Own Fable review + CI (touches
    shared loot code).

- **~~Economy selling socketed items~~ — NON-ISSUE (resolved 2026-08-06).** It was
  marth using a `resurrect` console command, not the economy selling. No bug; the
  log correctly showed zero `[econ]`/sell events. (Kept as a note so it isn't
  re-investigated.)
- **#56 — Combat overlay X/Y position adjuster** (now folded into the mage cut) —
  MCM sliders (or live drag) into the overlay's ImGui window pos (Board.cpp);
  controller nudge per the family rule.
- **#57 — Matching armor sets (MCM toggle, default OFF).** NOT per-piece: a
  set-COLLECTION state machine — the follower collects a better set's pieces
  (holds them, doesn't wear piecemeal, and the economy must NOT sell in-progress
  set pieces) until the set is COMPLETE, then equips it all at once; with a
  piecemeal OVERRIDE when a single piece is a big enough upgrade to wear now.
  Stateful + save-persisted + economy sell-exemption → focused later build.
- **#61 — "fashionrim" / my-followers-are-dolls debug toggle** (rolls into the mage
  update). Default-OFF Debug MCM toggle. **ARMOR ONLY for now:** disables armor
  looting, the mage school-robe loadout, armor auto-equip, MEO gem transfer on
  armor swaps, and armor buy/sell (partial). WEAPONS unaffected — weapon
  switching, weapon looting, ammo, weapon trade all still work. Warnings cover the
  armor breakage. Depends on #55 (MCM registration fix) or it's a dead checkbox.
- **#58 — Recognize framework followers (Kaidan, Inigo, Lucien).** Custom-framework
  voiced followers with bespoke quest/AI systems, not the vanilla follower faction
  path MFO detects. Reverse-engineer each one's active-follower signal from its
  installed ESP + Papyrus scripts (read the source — they expose no API), assess
  gambit/AI interop, document the mechanism in ENGINE_NOTES. Verify which are in
  marth's modlist first (primary sources).

## Where knowledge goes

See INDEX.md's routing table. Durable facts about *this session's* mechanisms are
already in the memory store (furniture latch, cast spell-choice, LoS worker-safe
pattern, MCM schema, release scope/publish hygiene). Engine findings → ENGINE_NOTES
§0; incident-born rules → INVARIANTS; symptom→cause→fix → DEBUGGING.
