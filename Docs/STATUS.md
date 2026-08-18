# MFO — Current Status & Handoff

> **LIVING DOC — KEEP THIS CURRENT.** This is the first thing to read to continue
> MFO from a clean slate. Update it *in the same change* whenever you: ship a
> release, open/resolve an issue, get a field-test result back from marth, or
> change the workflow. A stale status doc is worse than none — if you touch the
> project and don't touch this, you've left the next session a trap.
>
> **Last updated:** 2026-08-18 (v1.0.65 IN PROGRESS — self-cast + AUTO + full 6-wave review-fix batch merged to main, not yet cut) · **Latest public:** v1.0.63 (casters cast only the spell you chose; #59 continuous cast-takeover exact+partial) · prior public: v1.0.61 (creature weapons), v1.0.60 (#73), v1.0.59 (controller). v1.0.62 (weapon-thrash, was HELD) folds in — users jump v1.0.61→v1.0.63. The PROGRESSION ADDON (#74) is NOT in this release — it ships separately, gated on the §18 ESL/Addon-API rework; components 1–3 + manual skills + authored-constellation trees are BUILT and field-verified but DORMANT without MFO_Progression.esl. Next: §18 Addon-API/ESL work (FIRST tweak: perk points floor(level/2), was /3), then Roster addon (Mon 2026-08-18), then town update (#31).

> **▶▶ RESUME HERE (2026-08-18) — v1.0.65 IN PROGRESS: self-cast + AUTO + review-fix batch merged; NOT yet cut.**
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
> - **STILL PENDING before the v1.0.65 cut:** (a) **vendor spell-tome feature** — FIRST PIECE BUILT on a
>   town-update worktree branch (gated dark behind new INI `bFollowerBuySpells`, default OFF; NOT tagged,
>   NOT in the v1.0.65 cut): follower auto-BUYS tomes suited to its own skill/magicka/gold via the econ
>   bridge (`FollowerCanUseTome` gate in `TradeBridge::PlanBuy`, `NeedCat::kSpellTome`), the board
>   teach-path now also reads/consumes tomes from the FOLLOWER's own inventory, and the buy gate is
>   evaluated against the FOLLOWER's magic skill (Part-1 "unlock on follower skill" — see the report's
>   scope note: the merchant's SHOWN stock is engine-side/player-gated, MFO buys from what's physically in
>   the chest). Needs review + deck field test with `bEconomy`+`bFollowerBuySpells` ON. Design knobs to
>   confirm with marth: tomes-per-trade cap (2), Apprentice-25 caster gate, budget = pre-sell purse.
>   (b) **deck field test** of the whole
>   batch (self-cast + AUTO live, `bCastSelf`/AUTO on); (c) **cut + tag** v1.0.65 + Nexus bbcode
>   (still owed 1.0.62→65) + changelog. Saves are VERIFIED SAFE — no co-save regression in the batch.
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
> - **CONCENTRATION (answered marth):** works v1.0.53+ (ConcentrationCast, Actuation.cpp:224) IFF
>   TARGETED (not self) AND bForceCastOnMiss+bUsePackages ON — bounded package stream (hostile 1-4s LoF-
>   gated / heal-until-healed / utility). Self-cast concentration BARRED (package self route = unprobed
>   CTD cell) — a real gap if someone wants self-channel-heal. Old "not working" report predates 1.0.53.
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
