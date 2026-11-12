# T2 remaining batches: launch-ready briefs (2026-11-11 night)

Companion to `t2-manifest.md`. Each brief is complete: the coordinator pastes the COMMON block plus one batch block
into the agent prompt the moment a slot frees. Nothing is drip-fed later.

## Launch order, bases and boundaries (why this order)

| # | Batch | Model | Base | Starts when | File boundary (nobody else touches these) |
|---|---|---|---|---|---|
| L | G13 steal + G14 locks + G16 off-class | Sonnet (tier B) | MFO `origin/main` | first free slot | `native/logistics/*` (new `Steal.cpp`), `logistics/EquipAuthority.cpp`, `Vocabulary.h` APPEND only (`act.steal`, `act.pick_locks`), ESL generator for `MFOC_OffClassWeapon` perk |
| M | Harbinger melee: ch.31 Block + PowerAttack, ch.18 Normal-share-max, dodge facet, combat-style facet + MFO adoption | Fable (tier A) | Harbinger: the G7 branch if G7 touched Harbinger, else `feat/aversion` after its fix round; MFO: the T2 stack tip | the G3/H2 fix round is re-reviewed (its Fable slot frees) | Harbinger `native/channels/CombatMove*`, `AttackSelect*`, new `Dodge*`, new `CombatStyle*`; MFO `cast/Fire.cpp` power attack, `classes/Package.cpp`, `apmf/AttackMix.cpp`, new `apmf/BlockClaims.cpp` / `DodgeClaims.cpp` / `StyleClaims.cpp`, `Vocabulary.h` APPEND (`act.block`) |
| I | H4 spell range + C13 stealth entry + Mercenary back-off | Sonnet (tier B) | MFO T2 stack tip AFTER G3 merges (it edits the IAUS / Concealment files G3 owns) | G3 is on main | `native/iaus/*`, `scheduler/IausSource.cpp`, `Concealment.cpp`, `classes/` fight counter (no MCLS layout change: STOP if one is needed) |
| S | Sim: everything in the manifest's Sim column | Fable | `feat/iaus-sim-stealth` merged to main first | L, M, I and G7 have pushed their final heads | `tools/iaus_sim/*`, `tools/iaus_dash/*`, docs only |

ABI allocation (Harbinger `APMF_API.h`, append-only, byte-identical in MFO): v41 = H2 aversion (in review). v42 =
reserved for G7 if it needs a dead-target extension. Batch M takes the next free numbers after whatever G7 used,
one version per facet, and states them in its report. Two batches never bump the ABI at the same time.

Merge order: G3 (MFO) -> H2 (Harbinger + MFO) -> G7 -> L -> M -> I -> sim-stealth -> S. Each gets its Opus review at
its tier first. Field session after S: one per tier, DEV DLL, `sDevClassEquip` lines from the test list.

## COMMON block (every brief)

- Title the agent "[REPO] Model · Task". Disk log at
  `/tmp/claude-1000/-mnt-gaming-modlists-Projects-marth-follower-overhaul/a935cdde-4a9b-4a50-983f-339ce7b41062/scratchpad/agentlogs/<task>.md`,
  appended after every meaningful step; read it first if resumed. Commit + push WIP at each checkpoint (/tmp can be
  wiped by a reboot).
- Repos: MFO `/mnt/gaming/modlists/Projects/marth-follower-overhaul` (marthofdoom/MFO), Harbinger
  `/mnt/gaming/modlists/Projects/ai-package-management-framework` (marthofdoom/APMF). Fresh detached worktrees under
  `/tmp/claude-1000/worktrees/<task>`; always `cd X || exit 1` before git; never the main checkouts' working trees or
  another agent's worktree. Push your own branch only: no merge, tag, release, force-push. Never `find /`.
- Build: `taskset -c 0-7 nice -n 10 /mnt/gaming/modlists/Projects/_toolchains/mfo-localbuild.sh <worktree> /mnt/gaming/modlists/Projects/_toolchains/build-<task>`.
  Report the `LOCALBUILD: exit` line on your final SHA. MFO: `tools/iaus_harness/run.sh` (0 failed),
  `tools/iaus_sim/run.sh` (CHECKS PASS).
- Read each repo's CLAUDE.md, MAP.md "What breaks" for every subsystem you touch, and its open REVIEW-BACKLOG entries.
  Update MAP.md, STATUS, INI-REFERENCE in the same change. New mechanism = new file in its subsystem.
- NEVER guess an API or symbol: `/mnt/gaming/modlists/Projects/_commonlib/mit-3.7-fork/` is authoritative; anything
  ABI-shaped is verified against the disassembly of the unpacked binaries (Harbinger `Docs/VERIFIED-ADDRESSES.md`).
  New seats: VerifiedAddresses rows from the generator, `SeatVerified()` guards, per-runtime paths (1.6.1170, 1.5.97,
  1.7.104), VR refused, a passive rate-limited or config-gated observation line (a disassembled path is not a running one).
- Engine effects go through Harbinger (new facets as needed); MFO gates its decisions and logs. Gate the source, never
  undo the output. No list-specific data in logic (list files are test input in ~/.cache only). No GPL code copied
  (TK Dodge, UND and other list mods are read for integration only).
- Threading: `g_followers` is main-thread / serial-task only; sinks run on the main thread; the tick on the AddTask
  job worker. Co-save layout: do not change it; if unavoidable STOP and report.
- Stay in scope. Anything else goes to `Docs/REVIEW-BACKLOG.md`. No questions to the user: take a sensible default,
  append it to MFO `Docs/research/2026-11/open-questions-for-marth.md` with a `Q-<task>-N` id, move on.
- Deliverables include the in-game steps appended to the T2 field session in MFO
  `Docs/research/2026-11/in-game-test-list.md`, each with its exact `[Debug] sDevClassEquip` line.
- Final report: heads, `git diff --name-status` vs base per repo, build/harness/sim lines, what each item does with
  file:line, ABI versions taken and md5 of `APMF_API.h` in both repos (if touched), and your 1-5 least-sure spots for
  the Opus review.

## Batch L: G13 steal + G14 pick locks + G16 off-class weapon (Sonnet, tier B, MFO only)

Read `class-build-plan.md` cards G13, G14, G16 and the crime ruling memory text quoted there.
- G13 `act.steal` (APPEND to Vocabulary): a logistics action, the loot selection as a superset that also takes OWNED
  loose items, only when the pre-check proves no witness: every loaded, live, non-teammate NPC within hearing range
  passes `SenseActor` with all senses 0, the owner is not in line of sight, the follower is sneaking (G1). Any hit =
  the take does not happen (never a refund after the fact). Pickpocket is cut. Write the pre-check as one shared
  function (`logistics/Steal.cpp`) that G14 calls. `CrimeProbe` (`bCrimeGoldProbe`) is the evidence tool. Thief line
  class default (`cond.dark -> act.steal`); works with the tree OFF.
- G14 `act.pick_locks`: the existing chest lockpicking (`logistics/Lockpick.cpp`, LP-M1) seeks nearby locked chests on
  its own; `Admit()` already has the ownership and `IsOffLimits` gates; owned locks go through G13's pre-check; town
  locked doors stay blanket-blacklisted. ClickUp 86e3t1gg0.
- G16: with the tree ON, an off-class weapon (Archer: one-handed only) is never equipped unless the follower would
  otherwise be unarmed; then a player notification, and damage halved via a class perk `ModAttackDamage x0.5`
  conditioned on faction rank `MFOC_OffClassWeapon` that MFO sets while the fallback is held. Enforcement through the
  equip authority (`logistics/EquipAuthority.cpp`, Harbinger ch.17 per-item deny, ABI 33). The perk and faction come
  from the ESL generator (ESP is generated from source; regenerate, never hand-edit). ClickUp 1dfcuabcn02.
Tier B: one Opus pass, at most 2 rounds.

## Batch M: Harbinger melee batch (Fable, tier A, Harbinger + MFO adoption)

Read `t2-manifest.md` section 2, `class-perks-packages.md` 0.5 and the Knight / Warrior / Mercenary blocks,
`iaus-classes.md` gap rows C-BLOCK / C-POWER / C-C8, Harbinger FACETS ch.18 and ch.31 and the "ABI v38: COMBAT MOVE"
block at the end of `APMF_API.h`, and `dodge-research.md` (LoreRim ships Ultimate NPC Dodging, Tuxborn Deck has no
dodge mod, TuxBFCO has Simple Npc Dodge + TK Dodge). Items, in this order, each committed separately:
1. ch.31 `kCombatMove_Block` (Q83, Q-T2-1): a HELD state, a window plus the deny of his own offense and of the
   engine's release. MFO `act.block` (APPEND) for the Knight default; the class package's "blocks a lot".
2. ch.31 `kCombatMove_PowerAttack` (Q82, Q-T2-2): the engine's real power-attack road (power damage and stamina). Port
   MFO's `act.power_attack` (`cast/Fire.cpp:471`, its own graph drive) onto it, keeping the old road selectable
   until the field probe (iaus-sim-v2 29.1) decides; log which road fired.
3. ch.18 Normal-share-max (gap G-H2): deny Normal while the sliding window is over the cap, so the Warrior gets a power
   FLOOR; deny-only, attacks are never removed (additive rule). Class packages (C8 `apmf/AttackMix.cpp`) declare it.
4. Dodge facet (gap G-H3): RE first. List-agnostic: when a dodge framework is present use its own road (resolved at
   runtime by what is loaded, never by list name), else the vanilla side step. Mercenary package files it.
5. Combat-style facet: Harbinger owns writing `CombatController::combatStyle` (0x38, the field read under the AE +8
   layout rule: verify the offset in the disassembly), MFO's C8 (`classes/Package.cpp` -> `CombatStyle::Want /
   ApplyTick`) files a claim instead of writing it. Strict adherence with the tree ON (marth 10-27).
Pair rule: each Harbinger facet ships with its MFO adoption in this run. Deny-completeness: each facet gets its
DENY-COMPLETENESS-AUDIT row with open paths listed (never YES by narrowing). Tier 3 review per facet; rounds until
nothing above SEV-3.

## Batch I: H4 spell range + C13 stealth entry + Mercenary back-off (Sonnet, tier B, MFO only)

Read cards H4, C13 in `class-build-plan.md`, `t2-manifest.md` Q-T2-3, the Mercenary block in `class-perks-packages.md`.
- H4: desired distance from the equipped spell: touch or cone/concentration = the spell's range x0.8; aimed or
  target-actor = near the furthest working range x0.9; target-location = its range. Closing uses ch.24 (X = target,
  R = derived range); keeping back uses H3 when inside the minimum. Every mage class and hybrids in their spell phase.
- C13: a per-follower fight counter (classes add-on state; if that needs an MCLS layout change, STOP and report). On a
  guaranteed fight (Thief 1 in 5, Master Thief 1 in 3) the Thief enters the fight hidden through G1 + G2; on other
  fights his sneak rolls against the foes' detection (the engine's own; no fabricated result).
- Mercenary back-off (Q-T2-3, gap G21 / C-NOFLEE): when two or more foes press him, back off through H3 then re-engage
  ("backs off when two foes press, then darts in again"); Knight and Warrior keep no way out by design.
Tier B: one Opus pass, at most 2 rounds.

## Batch S: the sim (Fable, after L, M, I and G7 push)

Model every OPEN row of the manifest's Sim column in `tools/iaus_sim`: C8 packages (CSTY values, ch.18 shares,
ch.23 leash), H2 aversion (weighted selector with the H2-1 rule), H5 miss budget, G7 corpses and reanimate (stop
dropping reanimate spells), the G13/G14 witness pre-check against `Detect.h`, Block, PowerAttack + Normal-share-max,
dodge, H4 ranges, C13 stealth entry, Mercenary back-off. Each model states what is engine fact (with its source) and
what is MODEL. Re-baseline all T1+T2 classes over 6 seeds; `--<road> off` must reproduce the previous baseline exactly.
No curve tuning (that is the Haiku round after).
