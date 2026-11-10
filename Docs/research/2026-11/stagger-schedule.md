# Staggered schedule: backlog + class prereqs finish together (marth 2026-11-09)
Rule: 3 agent slots. Slot A = class track, Slot B = backlog track, Slot C = whichever queue has MORE remaining effort.
Re-balance at every completion: compare remaining effort (sum of effort points) of both queues.

## Class queue (M1, in dependency order; effort)
C1+C2 (5, RUNNING) -> C3 MCLS [A] (3) -> C7 ESL gen (4) -> C6 carriers (4) -> C5 mastery (2) -> C4 XP (3) -> C8 packages + MFO ch.18 header mirror (4)
G9 cooldown [A, FLWR v6] (3) | G10 level skip (2) | G5+G6 summon conds (2) | G1 act.sneak (2) | H4 spell range (3) | G15 class gate (1) | G16 off-class rule (2)
C10 GUI tab (4) -> C11 MCM (2) -> C12 defaults (2)
Total ~ 53

## Backlog queue (priority order; effort est.)
MFO-B1 (2, IN REVIEW FIX) | bound bows after B1 (2) | potions (DONE, merging) | MFO-B2 dry-run query [A] (4) | 86e3dmtr1 complex spells (4) | 86e39412x cast facet gaps 10/12/13 [A] (4) | 86e39411b MFO-B5 retargeting [A] (4) | 86e3dwn9d animated point casting [A] (4) | 86e3thffy migration remainder (5) | 86e3t1gfm split Equip facet (3) | 86e3t1gfp NFF source in menu (2) | 86e3t1gfv gem preference (3) | 86e3jw3b4 HMS above parity (2) | 86e3eevgz hidden perk effects (3) | 1dfcuabb1j6 CommonLib AE members (3) | 86e3t212u store off-class items (4, design defaults) | 86e3t2tm3 granular sliders (2) | 1dfcuabb1q4 faith (5) | review-backlog drain batches
Total ~ 63  (86e3940z6 epic + 86e3940y5 umbrella: tracked by their children)

## Prerelease counter (marth 11-09: prerelease every 10 merged additions, Deck-tested)
1. Harbinger ch.18 attack selection (c0a720e)
2. MFO potion drug allow-list (merge on main)
3. MFO-B1 Prepare target hand
4. Classes C1+C2 skeleton + table
5. Classes C3 MCLS co-save
6. Dev self-test gambit
7. Harbinger CanServe dry run (ABI v35)
8. G10 spell-will-work
9. AUTO control spells hostile
10. G9 per-rule cooldown (FLWR v6, one-way)  <-- PRERELEASE DUE
11. MFO adopts CanServe (ABI v35 header)
(next: potions drug allow-list, MFO-B1, bound bows, C1+C2, B2 dry-run ...)

## Backlog queue TOP (marth 11-09): DEV TEST-RUNNER GAMBIT (design first, Opus; then build) - dev builds only

## Backlog relevance ruling (marth 2026-11-10)
KEEP: bugs in play (perk-dup hotfix, cost parity, Tailor, B1a, retreat hold); mage casting (86e3dmtr1 complex spells, 86e39412x cast gaps, 86e39411b B5); progression correctness (86e3jw3b4, 86e3eevgz); point casting (86e3dwn9d). Also kept (not deferred): Harbinger menu/polish items, small/low-impact items, engine-gated items.
DEFERRED post-classes (tag deferred-post-classes, due 2027-01-11): 86e3thffy migration remainder, 86e3940z6 EPIC, 86e3t212u store off-class items, 1dfcuabb1q4 faith.
## Batch 2 counter
1. Progression perk-dup fix c3018f1 (hotfix candidate)
2. Spell cost parity 2bb9c59 (merged 11-10)
3. Classes C7 MFO_Classes.esl generator 59ec8c4 (merged 11-10)
4. MFO-B1a hand planner sees right weapon 92b6e84 (merged 11-10)
5. Retreat holds through engine Flee 912e545 (merged 11-10)
6. HMS above-parity gate e3d15c4 (merged 11-10)
7. Classes C5 mastery 2f584aa (merged 11-10)
8. Spell-row alias + shape census bbf90fd (merged 11-10)
9. Classes C6 passive carriers 84678a5 (merged 11-10)
10. Hidden-perk census (log only) 92604e0 (merged 11-10)
11. H5 projectile aim pair: Harbinger 62dc4e7 (ch.29 ABI v36) + MFO 7e8dc21 (merged 11-10) -> PRERELEASE 2.10.0 / 0.19.0 DUE
(Note: gold popout part 2 etc. on main after v2.9.0 were already counted? check before the cut.)
- POST-CLASS FIRST: 86e3t212u off-class items in player home (marth 11-10)

## Batch 3 counter
1. C5b remove class Auto picker b8f287a (merged 11-10)
2. C5c dev-only class equip af93cea (merged 11-10)
