# Task 4 status audit: APMF equip deny, MFO as equip authority (ClickUp 86e3940xu)

Read-only audit, 2026-10-01. Sources: MFO `origin/main` (fetched), APMF `origin/main` (fetched, depth 200, tip `b0decf9` = v0.9.11).
I did not edit anything. ClickUp was not reachable (the daily MCP limit was hit), so the task text comes from `item4-brief.md` section 4.

## 1. What has shipped since 2026-09-16

### APMF (marthofdoom/APMF)
| Commit | Date | What |
|---|---|---|
| `8c4250e` / `039027d` / `3d5cab8` / `f49aabb`, merged `9226f77` | 09-16 | **ABI v9 `SetEquipScope`.** `APMF_EquipScope{owned, denied, reserved[2]}` (16 bytes), category masks Armor/Shield/Right/Left/Ammo/Light (All 0x3F). nullptr = `{All,0}` = v8 behaviour. A single `equipsink::Categorize`. Seat verdict: `competes & denied` -> deny, `competes & owned` -> the v8 in-set test, else allow. The enforce pass skips unowned or denied entries. Review fix: a mixed ARMO competes Shield+Left+Armor, plus `kEquipCat_None`. |
| code | | `native/core/ClientAPI.cpp:136` `APMF_SetEquipScope`, `native/core/ControlMap.cpp:460` `EnqueueSetEquipScope`, `:1509` `ApplySetEquipScope`, `native/channels/EquipAuthority.cpp:60` (scope in the enforce pass), `native/core/EquipSink.cpp:665` (INI read, code default observe=1) |
| `a2c7d65` | 09-22 | v0.9.5 release. It is the first release that carries v7 + v8 + v9 (the CHANGELOG says "Not field-run" and "ships in observe mode"). |
| `827f320` / `38f2dd3`, merged `8d64c5c` | 09-29 | **Passive ranged-select probe (phase 1)**, prompted by the 09-28 Cicero dragon fight (see section 3). Observe only. `[Probe] bRangedSelect` was reset to 0 for release in `37b72e1` (v0.9.10). |
| ABI v10 to v17 | 09-22..09-25 | Unrelated channels (travel, entry, pin, leash, idle). The ch.17 layout did not change. |

### MFO
| Commit | Date | What (file:line on current main) |
|---|---|---|
| `d3bd6ab3` | 09-16 | **v9 wiring, "F6 resolved".** The scope is computed from the ForcedHold ledger: `owned = Armor` always, `+Right`/`+Left` per hold, `+Right+Left+Ammo` for a held bow or crossbow, `+Right+Left` for a held 2H. Light and Shield are never owned. The hands are declared from the ledger ONLY: the "weapon currently in the hand" fallback and the torch read are deleted. Bridge floor is ABI >= 9. A v9 MFO on a v8 APMF gets no authority, never a blanket lock. |
| `78574467` | 09-16 | Re-mirrored the final v9 header from APMF `9226f77`. |
| `42a354a2` | 09-20 | Opus/Fable F1: the OOC road declares from the ledger (`Actuation::ForcedHoldFor`), not 0/0, so a combat-flag flap no longer demotes a standing hold to `owned=Armor`. Backlog B44-B46. |
| `b5569023` | 09-22 | **Declaration hygiene (MFO-B63 parts 2+3).** (a) A declared ARMO that is not worn is re-sent, under enforcement only, throttled by `kDeclDriftHold` 3 s (`native/logistics/EquipAuthority.cpp:323`, `:961-986`). (b) Nothing is declared while `!Is3DLoaded()`, and the caches are dropped so the first loaded tick re-sends (`:508`). |
| `e056be5a`, `55f15dd9`, `83271e1c` | 09-28 | **No decline-fallback.** A refused claim means MFO stays out of equipment entirely (marth: "MFO's fallback is deprecated"). **The Shield category is denied for every non-shield user**: dual wield, no one-hand melee role, or a pure caster (`EquipAuthority.cpp:589` `denied`, `pureCaster`/`noShieldUser` just above). |
| `cc4e8951` | 09-28 | **Mage set.** A mage-apparel follower declares the whole per-slot best set (`MageBestPerSlot`, `EquipAuthority.cpp:66`, used at `:123`/`:767`). The set depends on what the follower owns, not on what is worn (fixes Jesper's set 6/7 flip). |
| scope assembly | | `EquipAuthority.cpp:557-561` (owned), `:589` (denied), `:993` `DeclareEquipScope` then the set. Bridge: `native/apmf/Equip.cpp:84-95` (v9 floor), `:194-216` `DeclareEquipScope`, `native/apmf/APMFBridge.h:858-871`. |
| `d30c2b7e` / `a643086e` | 09-28 | Backlog MFO-B136..B139, cross-referenced from MAP.md §7 (`MAP.md:5054` entry). |

## 2. Is the deny on for users today? Is F6 closed?

- **The deny is OFF by default.** APMF ships `[EquipAuthority] bEquipAuthority=1`, `bEquipObserveOnly=1` (`Data/SKSE/Plugins/APMF.ini:64,69`; code default 1 at `native/core/EquipSink.cpp:665`). APMF STATUS (around line 711) says: "the equip-authority observe flip (`bEquipObserveOnly`) is the only one left".
- MFO ships `bApmfEquipAuthority = 1` (`out/SKSE/Plugins/MFO.ini`, `Config.cpp:322,430`, `Config.h:341`). So MFO claims and declares for users, but APMF only logs would-deny. MFO never sets `kEquipAuth_ObserveOnly`, and APMF's INI is the single enforcement switch.
- Side effect in observe mode: since 09-28, a user with the authority supported has MFO's direct equips suppressed (MFO "stays out"), while APMF only observes. So under the shipped defaults no one enforces the declared set. The engine dresses the follower, apart from what APMF's own non-forced pass equips. Worth stating in the next brief.
- Stale text (tier C): the MFO.ini comment for `bApmfEquipAuthority` still says "Inert without Harbinger at ABI v7 or newer". The real floor is v9 (`apmf/Equip.cpp:84`).
- **F6 in its original form is closed in code** by v9 scoping. A follower with no hold owns only Armor, so the engine's own melee/ranged switch is allowed (`d3bd6ab3`, STATUS "F6 RESOLVED"). This has not been confirmed by an APMF-side field run: APMF STATUS says "v9's scope work ... NOT field-run".
- **Contradicting evidence: the Deck appears to run ENFORCED locally.** MFO STATUS cites `agentlogs/fable-v9-enforced-run.md` (Deck 2026-09-21). The 09-22 B63 diagnosis says "APMF correctly denied the engine's outfit piece". The 09-28 Cicero log shows `verdict=deny` refusals. NEEDS LOCAL: read the Deck's APMF.ini.
- **An F6-shaped residue is OPEN** (APMF STATUS, the 09-28 diag-dragon-melee entry). The setup: a melee follower UNDER a hold (owned Armor+Right+Left, two swords) fighting a flying dragon. The engine kept SELECTING the Glass Bow. There were 114 ch.17 refusals at `CombatNode` `ret=877F1B`, and 18 Riften Guard's Shield refusals have the same shape. The bow is refused at EQUIP but still admitted at SELECTION, so the AI keeps a ranged plan and walked him off a cliff. This is "the hold wins over range" working as designed, but the deny is not complete at selection. marth approved fix "A": deny at selection (Ranged vtable slot 0x0F, phase 2 of the ranged-select probe). The phase-1 probe is merged, but its field checks are "NOT YET OBSERVED".

## 3. What is still OPEN from the DECIDED scope

| Item | State |
|---|---|
| Passive probe first | DONE for v8 (observe run 6/7, criterion 3 named by APMF `8b03000`). NOT done as a formal v9 observe run against INTEGRATION criteria 8-15 (APMF STATUS). |
| Enforcement flipped for users | OPEN. The shipped default is observe-only on APMF. |
| **Deny complete across cell load** | PARTLY. The OutfitApply re-apply is denied (09-16 enforced run: guard outfit denied, shield denied 15x). MFO re-sends after a 3D load and on armor drift (`b5569023`). **Open:** (a) after a DENIED `OutfitApply`, the shield and hood helmet VANISHED from `GetInventory()`. Disassembly shows no RemoveItem after the denied call at `0x3BD690`, so the cause is NOT DETERMINABLE yet; the next step is a `TESContainerChangedEvent` probe (ENGINE_NOTES §0.46 addendum, MFO-B63). (b) MFO-B63 part 1: best-per-slot for NON-mage followers, so an outfit piece is not adopted because it happens to be worn. This is ClickUp task 5, `86e3d6ay6`. (c) MFO-B64: `CoLoadForcedWeapons` leaves a loaded follower empty-handed. (d) MFO-B136: a never-wearable mage set piece is re-sent every 3 s. |
| **Deny complete across dismiss** | CODE DONE, NOT FIELD-PROVEN. `Followers::ReleaseHeldState` releases the authority FIRST and defers `Loadout::Restore` past APMF's Drain with two main-thread hops (`native/Followers.cpp:326-365`). `Logistics::OnFollowerRemoved` releases again and drops the change detector (`native/logistics/Upkeep.cpp:647-654`). No enforced-mode dismiss observation is recorded. |
| Deny complete at selection (bow/shield) | OPEN (the residue described in section 2). Ranged-select phase 2, plus the same question for the Shield vtable 0x0F. |
| **MFO-B16 closed** | OPEN. `Docs/REVIEW-BACKLOG.md:143` has no closure note. Its "fix shape (b)" is exactly this authority: an engine re-wear of an off-class piece. Under enforcement plus Armor owned it should be refused, but closing it needs an enforced-run observation (`[armor-obs] EQUIP '<heavy>'` with no preceding MFO `[equip]`, or the matching APMF deny line). In observe mode (users' default) B16's flip still happens, and with "MFO stays out" MFO does not even counter-swap. |
| **Clothing-vs-heavy-helmet question answered** | NOT RECORDED AS ANSWERED anywhere in either repo. I could not read the exact question (ClickUp rate-limited). What exists: clothing scores 0 in `ArmorScore`, so a rated helmet beats a hood for non-mages (MAP "THE WEAR DECISION"). Mages declare the per-slot clothing set (`cc4e8951`). The head slot is three biped bits (`IsHeadSlotMask`, 09-14). Under enforcement with Armor owned, an engine heavy-helmet equip over a declared hood is refused by construction. That is a code-level answer only, never written down or field-confirmed. |
| Open backlog on this mechanism | MFO-B44 (owning Ammo pins one stack, marth's call), B45, B46, B133, B134 (SEV-4: Gear.cpp weapon hop and EquipTorch still direct under a refused claim), B135 (SEV-3, NOT FIXED by marth's call), B136, B139 (SEV-5: a mage on class Auto is not denied a shield). APMF-B11..B18 (deferred ch.17 SEV-4/5), APMF-B36 (F2 is a phase-2 prerequisite: refuse to install unless slot 0x0F holds the base function). |

## 4. Per open item: where, tier, next step

| # | Item | Cloud or local | Tier | Next step |
|---|---|---|---|---|
| A | Confirm the Deck's actual APMF `bEquipObserveOnly` and pull the 09-21 / 09-28 enforced-run findings into STATUS | NEEDS LOCAL (Deck INI + agentlog `fable-v9-enforced-run.md`) | C | The coordinator reads the Deck APMF.ini and the agentlog, then records "v9 enforced run done/not done" in APMF STATUS. The two repos contradict each other today. |
| B | v9 observe/enforced run against INTEGRATION criteria 8-15, including a dismiss under enforcement and a cell transition | NEEDS LOCAL (Deck) | field | One session: a no-hold archer (must switch to the bow and shoot, `owned=Armor`), a dual-wield hold (`owned=Armor+Right+Left denied=Shield`), dismiss a follower (leaves in pre-MFO gear), cross cells (outfit denied, no bare body). |
| C | Ranged-select deny phase 2 (bow, and maybe shield, refused at SELECTION for a ch.17 claimant whose sink verdict is would-deny) | Phase-1 field data NEEDS LOCAL (`[Probe] bRangedSelect=1`, the dragon test in APMF STATUS "FIELD CHECKS"). Phase 2 code is cloud-buildable once the data exists. Seat verification uses the unpacked exe (local) | A (engine seat, APMF) | Run the probe on the Deck first (principle 5). Then an APMF brief: a Ranged vtable 0x0F deny keyed on ch.17, plus the APMF-B36 F2 base-function guard. |
| D | Inventory vanish after a denied OutfitApply | Probe code is cloud-buildable. The result NEEDS LOCAL | B (passive probe, MFO side, a new sink rides the existing logistics ContainerSink) | A passive `TESContainerChangedEvent` log on managed followers around OutfitApply, then a Deck run. |
| E | MFO-B63 part 1, best-per-slot for non-mages (= task 5, 86e3d6ay6) | Cloud-buildable (code). Field verdict local | B | Its own branch per the ClickUp working notes. Pick the logical-slot vs biped-bit notion explicitly. |
| F | MFO-B16 closure | NEEDS LOCAL (enforced field observation) | C after evidence | After B, grep `[armor-obs]` for engine re-wears, then close B16 in the backlog with the evidence. |
| G | Clothing-vs-heavy-helmet answer | Writing it down is cloud-doable. Confirming it NEEDS LOCAL (field) and marth's exact question | C (docs) | Recover the question from ClickUp, write the code-level answer (section 3) into STATUS/MAP, and confirm it in run B. |
| H | Users' default: flip `bEquipObserveOnly=0` in a release | Code/INI is cloud. The decision is marth's, after B and C | C (INI) but release-gating | Only after B passes and C lands. Note the observe-mode gap: since 09-28 MFO stays out and nothing enforces. |
| I | MFO-B134 / B139 / stale MFO.ini "ABI v7" comment | Cloud-buildable | B (B134), C (comment), B139 small B | Batch into one backlog drain. B135 stays out by marth's call. |
| J | MFO-B64 CoLoad empty hands | Cloud-buildable code, but it sits in a co-save load path | A (co-save area) | Its own brief with a 3D gate and a dirty-tick re-arm. |

## 5. APMF_API.h sync

- **In sync, byte-identical.** MFO `origin/main:native/APMF_API.h` and APMF `origin/main:native/APMF_API.h` have the same md5, `708b5ba379b4f109f46759f0225d113b`, 1987 lines, empty diff.
- Both copies are at **`kABIVersion = 17`** (line 160). APMF's last header change is `73504ad` (v17, idle v2). MFO last mirrored it in the 09-25 series (`94ce807a` v14, then up to v17).
- MFO **requests** ABI 10 (`native/apmf/Bridge.cpp:842`, `kRequestAbi = 10`) and gates each feature by `abiVersion` (equip authority at >= 9).

## Summary

**What is left.** v9 scoping (`SetEquipScope`, APMF `9226f77`; MFO `d3bd6ab3` + `42a354a2`), the hands-only-under-a-hold rule, shield-deny-for-non-users, the mage per-slot set and declaration hygiene (B63 parts 2+3) have all shipped. The headers are in sync at ABI 17. But:
- Users run observe-only (APMF `bEquipObserveOnly=1`), and since 09-28 MFO stays out, so nothing enforces the declared set by default.
- The deny is not complete at SELECTION: the engine still selects the bow or shield and is refused only at equip (the 09-28 dragon case).
- The inventory vanish after a denied OutfitApply is unexplained.
- Non-mage best-per-slot (task 5) is open.
- MFO-B16 is not closed.
- The clothing-vs-heavy-helmet question has no recorded answer.
- The two repos disagree on whether a v9 enforced run happened.

**What the cloud can do now.**
- (E) MFO-B63 part 1 best-per-slot. This is task 5's branch.
- (I) A small MFO drain: B134, B139, the stale "ABI v7" INI comment.
- (D) The passive `TESContainerChangedEvent` probe code.
- (G) Write down the code-level helmet answer.

Everything that closes task 4 itself needs the Deck: the v9 enforced/observe run, the ranged-select phase-1 probe data before the phase-2 seat (tier A, APMF), the dismiss and cell-load proof, and B16 evidence.

**Recommended next branch.** For task 4 itself, no cloud code branch should come first. The gating step is local: confirm the Deck INI and run the ranged-select probe plus the v9 criteria session. If the cloud must start something now, take task 5's best-per-slot branch (tier B, MFO), or the small B134/B139 drain. The phase-2 selection deny (APMF, tier A) should wait for the probe data.
