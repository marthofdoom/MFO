# IAUS simulator v4: real environments, pathing, looting, and an AI that chooses where to stand

marth, 2026-11-23, verbatim: "Do a big sim fix round. Consider t2+ needs. full looting, pulling real environments from game and simulating them, navmesh included, so that no odd pathing can occur once tuned. Then start the tune with exteremely low priority so that it wont interfere with anything".

And the same day, on what the environments make possible: "This means the sentient AI can now power movement, fully aware of the environment. line of sight, etc. Make sure thats included, and tested in the sim." On where the game gets its world: "Instead of extracting it reads the active navmesh". On spells: "I noticed that the spells being chosen were still pretty ineffective, certainly were probably better options available. But I was unsure if a proper spell tuning pass occuired".

Branch `feat/sim-v4-s1`. Revised after the Opus review of 1874f81e and marth's rulings of 2026-11-23 (section 11 records each). This note is the design (sections 1 to 8), the record of slice 1 which is built (section 9), the slice list (10), marth's open decisions with a recommendation each (11) and findings outside this branch's file boundary (12). Read `iaus-sim-v3.md` first. v4 is v3 plus what is below.

Standing rulings this design is held to. The oracle has unlimited tries and only the class's abilities, and tuning follows the oracle. "The goal is that a classes tune will be the actual best possible class behavior". No list-specific data in game logic: lists and game data are sim test input only. The sim derives IAUS constants and a hand value is a labelled placeholder. One named table per set of tunables, one shared reader per kind, and the docs say "to add X, edit Y".

Labels as in v3. **FACT** = read from code at the cited place. **CALIBRATED** = measured in a field log. **MODEL** = our reading of engine behaviour with a source for its shape. **ASSUMPTION** = no source, a labelled guess with a knob. **ESTIMATE** = a cost or size not measured yet. Each slice's acceptance measures it.

Sections 1 to 6 answer the six points of the brief in its order. Each has: what exists today, what is missing, the mechanism, how it is validated against field logs, CPU cost per fight, and a slice boundary another agent can implement from.

How cheap the lab is today, because every cost below is read against it (measured on this branch, one core): an IAUS seed run of one class (24 natural fights, or 24 per scenario for a class with named scenarios) takes 0.03 to 1.0 CPU seconds, so a fight is 1 to 5 ms. An oracle fight is 51 CPU seconds subject only (v3 14.10). So an environment may cost an IAUS fight several times over and nobody notices, while every microsecond inside a rollout is paid about ten thousand times.

## 1. Real environments

### 1.1 Today

`World.h`: a flat plane. Positions are x and y, plus z for the v3 dragon and the perched foe. Cover is a list of circles (`Occluder {x, y, r}`). `--space` gives one round room (`arenaR`). A ledge is a flag with "has a way up" and a climb three times its height (ASSUMPTION, v3 13). Nothing has a door, a corridor, a stair, a wall or a drop. A fight starts with the groups a drawn distance apart on that plane.

### 1.2 What `tools/envextract` gives (branch `feat/envextract`, a Sonnet agent's, not edited here)

Its README is the contract. One binary file per environment, `~/.cache/mfo-env/<set>/<set>.<class>.<id>.mfoenv`, format `MFOENV` v1, with a header-only reader `tools/envextract/envfile.h`. Sections: `MESH` (one per navmesh, with bounds and cell), `VERT`, `TRIS` (three vertices, three neighbours, the game's flags, cover bits), `LINK` (edges into another navmesh), `DOOR` (the door triangles and their door ref), `REFS` (placed doors with lock level and teleport target, containers with item count, activators, furniture, loose items, actor spawn points, large statics, each with a world-space box from the base object's OBND), `META` (JSON), `PLUG` and `STRS`. Twelve environment classes picked by measured geometry (`interior_narrow`, `interior_tower_stairs`, `exterior_camp`, `exterior_fort` and so on), a seeded library per set, `index.json` over all of it. No name and no list-specific id decides a class.

### 1.3 What the sim's consumer needs from that format

Everything below is a request to the extractor's owner, not an edit. The consumer adapts to what ships. Where a request is not met the fallback is named.

| Need | Why | In v1 today | Fallback if it stays out |
|---|---|---|---|
| Triangles with reciprocal neighbours, external links resolved to a mesh index | route search across cells | yes (`TRIS.nb`, `LINK.targetMeshIdx`) | none needed |
| Link kinds decoded (`EDGE_EXTRA_INFO_TYPE` in the fork's `BSNavmesh.h`: portal, ledge up, ledge down, enable/disable portal) | a drop is a one-way edge. Read as a walk edge it gives wall climbing, dropped it loses a real way down. Both are odd pathing | decoded by the Opus review of the extractor (2026-11-23, fixes in progress, re-read its README): kind 0 = two-way walk edge, kind 2 = one-way drop DOWN (median 116 units, p95 600, 82 percent inside one mesh), kind 1 = its reverse and never walkable up, kind 3 = enable/disable portal, closed by default | none needed once the README states it. Until then the consumer hard-codes exactly these four meanings in ONE table (`Env.h kLinkKinds`) |
| Connected components labelled | environments have disconnected islands. A fight spawned across two islands is an unreachable-foe fight by construction | being added (review) | the loader labels components itself with a flood fill |
| Door triangles joined to their REFS row, lock level, key, teleport target cell | closed and locked doors gate a path. A teleport door ends an environment | yes | none needed |
| Ground height outside the navmesh for exteriors (LAND `VHGT`) | line of sight over terrain. The ridge case of 8.5 cannot exist without it | no ("the navmesh surface is the only ground") | exterior LoS only from navmesh triangles treated as terrain patches. A hill with no navmesh on its crest is invisible to the ray. Marked as a known error source |
| Collision for walls and large statics | line of sight and line of fire indoors | no. OBND boxes only, and `modelOff` to fetch the mesh later | the navmesh boundary wall of 2.4, measured in 8.5 before any BSA work |
| Actor spawn points with their base id, joinable to `list_balance`'s world json by (plugin, local) | real foes standing where the game puts them | yes (`REFS.kind` 5, the join rule is in the README) | none needed |
| Containers and loose items with base id and count | the loot layout of section 3 | yes for containers (`count`). Loose items yes (`kind` 4) | container CONTENTS are leveled lists: resolved from the world json, see 3.3 |
| Interior flag, water height, cell ids in META | detection, swimming is out of scope | yes | none |
| A stable environment id and class in `index.json` | seeded draws and results keyed by environment | yes | none |
| Per-triangle preferred flag and cover bits | path cost, cover spots (8.1) | yes (`flags` 0x40, `cover`) | cover from geometry only |

The consumer must also accept, with the same reader, a file written by the game: the dev-build dump of the ACTIVE navmesh (8.6). So one request goes to the format itself: a `LIVE` section (optional, ignored by old readers as the README's unknown-tag rule already allows) holding actor positions, facing, and the engine's own line-of-sight verdicts at the moment of the dump. The extractor never writes it.

### 1.4 Mechanism

A new header `tools/iaus_sim/Env.h`. One immutable `Env` per loaded file, shared by pointer between a fight and all its rollout copies (a `Fight` stays a plain copyable value, v3's rule, and copies only a pointer). It holds the triangles in one array with a uniform grid over them for point location, the portal edges, the door table, the refs, and the wall segments of 2.4.

A fight is placed into an environment by a `FightSpec` field, `env` plus a seed. Followers and the player start at a door or an entry edge. Both sides are placed in ONE connected component of the mesh (walk edges and drops count, the review's islands finding), unless a scenario asks for an unreachable foe on purpose. Foes stand on the environment's own actor spawn points, grouped by distance (MODEL: spawn points within 1500 units and in sight of each other are one encounter). Which actor stands on a point comes from the list world when the base id joins, else from the sim's own foe draw for the environment class. The built-in world keeps working with no game data: `tools/envextract/tests/synth.py` already builds synthetic records, and the sim ships a handful of hand-made synthetic `.mfoenv` fixtures (corridor, doorway, stair tower, ledge with a drop, ridge, dead end) under `tools/iaus_sim/envs/`. Those are not game data, so they can be in git, and the CLI checks and baselines run on them.

Coverage is the goal of the library (marth, 11-23): every terrain type and every kind of design the game has is in it, and an anomalous area (a cell a mod broke, a mesh that fails the validator, an island with no way in) is simply SKIPPED by the sim, with a count. The sim does not study broken places. The GAME has to recognise them when it stands in one: that is the trust reader of 8.9.

Switch: `--v4 env=on|off` in a new `Sim::V4` struct beside `V3`, same rules as v3's switches (off = draw for draw the lab before it, the headline names what is off). `--env <file>`, `--env-set <set> [--env-class <class>]`, `--env-seed`.

### 1.5 Validation against field logs

Nothing in a past log places a fight in a cell with coordinates, so an extracted environment cannot be checked against old evidence. What can be checked is the extraction itself: the extractor's validator (19,924 vanilla navmeshes decode, reciprocity 99.995 percent per its log), and from the first dev-build session on, the parity check of 8.6 (active navmesh of a cell against the extracted cell: triangle count, vertex positions, door states). That check is this section's field validation.

### 1.6 CPU cost per fight (ESTIMATE)

Loading is once per process (`sim --serve` workers keep the library in memory, a file is 0.1 to 5 MB). Point location is a grid lookup, tens of nanoseconds. The cost that matters is in sections 2 and 8.

### 1.7 Slice boundary

Slice S2 "environment loader": `Env.h` (reader over `envfile.h`, grid, wall segments, ledge edges), the synthetic fixtures, `FightSpec::env` and placement, `--env*` flags, `--v4 env`. No movement change yet: with `env=on` and nothing else, bodies still move on the plane and the check is that placement is legal (every body on a triangle) and that `env=off` is the v3 baseline with 0 changes. Needs envextract merged. Does not need game data.

## 2. Navmesh pathing, and "no odd pathing"

### 2.1 Today

`World.h Move` walks a body in a straight line toward a point, clipped to the round room. Blades spread on attack slots around a target (v3). No route, no door, no body ever blocks another, nobody is ever stuck. The approach, hide, flee, back-off, hidden step, leash and loot legs all end in `Move`. Line of sight is a ray against circles (`Sees`). Line of fire is a corridor test against party bodies (`LofClearFrom`).

So the lab cannot produce one of the things the field logs are full of. One Deck session (`_evidence/deck-run-1118-adelinda`, 17 minutes): 23 `Movement Blocked` legs, 58 `GATED` loot records, 16 repaths, 9 stuck verdicts, 3 approaches that ended `ENGINE DROPPED`, 311 `[los]` lines.

### 2.2 Mechanism: route search

A* over triangles of the `Env` (edge midpoints as nodes, straight-line heuristic), then the funnel algorithm for the corner list. Edge costs: length, times a preferred-path discount where the flag is set (MODEL: the engine prefers them, weight is a knob), a closed door adds the open time, a locked door is closed to anyone without the key or a pick road, water triangles are closed for a fight (ASSUMPTION, out of scope). Links by kind (1.3): kind 0 is a walk edge both ways. Kind 2 is a drop, one way, down. Kind 1 is never an edge for a walker (treating it as one gives wall climbing of up to about 1,500 units). Kind 3 is closed unless the environment's state opens it. **Whether followers USE drop links in the game is not observed.** So a drop is an edge only for a body whose traversal abilities say so (8.8), the default for a follower is OFF until the probe of 8.8 has seen one taken, and the lab runs both ways (`--v4 drops=on|off`). `NoLargeCreatures` triangles are closed to a body whose radius is over a threshold.

One function, `FindPath(view, from, to, body) -> {corners, length, reachable, firstBlock}`, the ONE reader of "can he get there and how far is it". **It is written from the start (batch W) as a pure, engine-free file in `native/iaus` (`EnvQuery.h`, `PathSearch.cpp`), compiled into the sim as `Generate.cpp` is, so it cannot fork into a sim copy and a game copy.** That first slice adds the file and nothing calls it in the DLL yet (a pure file with no caller, harness-tested: tier B, Opus review). `Env::Path` in this note is the sim's thin call into it.

### 2.3 Mechanism: path following as the engine does it

The engine does not follow the funnel exactly. What the field shows and the lab must reproduce (each a MODEL with a knob, calibrated in 2.6):

- **Movement Blocked.** The engine raises it when a mover makes no progress against an obstacle the navmesh does not know: a closed door nobody opens, an activator in the lane, another actor standing in a doorway. In the lab: a body whose path crosses a door triangle with the door closed and no open road, or whose next corner is inside another body's radius for longer than `blockSec`, ends his leg BLOCKED at that point. This is what `[travel]` logs (`BLOCKED`, "ends at the same spot") and what native loot turns into GATED and stuck-at-origin records (`LootNeed.h gateRepeatU / gateShortU / gateNearU`).
- **Body collisions.** Bodies are circles with the actor's radius. In a corridor narrower than two radii nobody passes. In a doorway (one portal edge) one body passes at a time, the others queue. The engine's own avoidance is soft, so a pass is allowed after `yieldSec` when the blocker is a friend and never when he is a foe (ASSUMPTION, the first thing 2.6 calibrates).
- **Stuck legs.** A leg that is not blocked and still makes no progress (a corner the funnel cuts too tight, a ledge the route search thought walkable) for `stuckSec` is STUCK. The lab can only produce the ones its geometry produces. The others are found by the parity check and become extractor or cost fixes.
- **Drops.** A kind-2 link is taken only when the route search chose it and his traversal abilities include it (8.8). Falling off an edge with no link does not happen (the engine keeps actors on the mesh). Fall damage is not modelled.
- **Stairs and height.** z comes from the triangle. v3's `height` switch keeps the dragon. Its perch model is replaced by real geometry when `env=on` (a tower top is reachable when a path exists, with its real length, instead of "three times the height").

The legs, as Harbinger's channels execute them (the lab mirrors the request and the end states, not the engine's steering):

| Leg | Native source | Request the lab mirrors | End states |
|---|---|---|---|
| Approach | `apmf/OffenseApproach.cpp`, ch.24 | target ref, radius, hold, standoff by weapon kind | arrived, target lost, ENGINE DROPPED (path failed) with `offenseApproachBackoffSec` and `offenseApproachDropCap` |
| Hide, flee, back-off, hidden step, give ground | `Concealment.cpp` picks, `apmf/Disengage.cpp` and `apmf/GroundApproach.cpp`: a ch.24 approach on a WORLD POINT (`kApproach_Point`, ABI v39, with `kApproach_Hold` for the disengage) | a point, a radius, hold or end on arrival | arrived, seen on the way, ENGINE DROPPED (the engine cannot path to the point), target lost (he left the cell space) |
| Leash | ch.23 | anchor and radius | pulled back, the foe past the radius dropped |
| Entry | ch.21 EngageOnSight | the entry foe | v3 model, unchanged |
| Loot and errand travel | `logistics/LootTravel`, ch.19 | destination ref, arrival radius, gait | arrived, BLOCKED, DONE-WHILE-WALKING, combat |
| Engine combat movement | the combat style (CSTY) | not a channel: the engine's own circling, fallback and advance | `Brain` as today, but every step is a move along the mesh, never through a wall |

### 2.4 Mechanism: line of sight and line of fire by real geometry

Three layers, cheapest first, each one switchable so its error can be measured alone:

1. **Navmesh boundary walls** (no extra data). An edge with no neighbour and no link is a boundary. A ray that crosses non-walkable space at floor level is blocked, unless the space it crosses lies below both ends (a pit or a drop seen across) or the crossed gap is narrower than `gapU` (furniture). Good for corridors, doorways, rooms and tower floors. Blind to windows, railings and low walls: it reads them as solid.
2. **Terrain** for exteriors, from LAND heights when the extractor exports them, else from navmesh triangles as patches.
3. **Collision meshes** from the NIFs (`REFS.modelOff`), read out of the BSAs and loose files. Not built in v4. Whether it is needed is decided by a number, see 8.5.

Rays are 3D, eye to chest, three rays as native `Sightline` casts them (`[los] ... own ray, all 3 rays blocked, end margin 48u`). Line of fire adds the party-body corridor of v3 on top.

### 2.5 The measurable goal: no odd pathing

"Odd" is defined by six numbers. Each is tallied per follower and fight, reported per class and environment class, and compared with a bound that comes from the oracle on the same fights (the oracle moves through the same world and cannot avoid a door either) plus a margin. A tune passes the pathing gate when every metric is inside its bound on every environment class. The field column is what the lab's model of the mechanism is calibrated on, not the bound.

| Metric | Definition | Field evidence it is calibrated on |
|---|---|---|
| `stuck%` | share of his fight time in a leg that makes no progress (moved under 30 units in 2 s while a leg wants him to move) | `[stall-probe]` (moved u in s, with his package), `[travel]` STUCK ends, `[loot]` stuck-at-origin |
| `reblock/f` | BLOCKED endings at a point within `gateRepeatU` of his previous block, per fight | `[travel]` "blocked ends at the same spot", `[loot]` GATED and "stuck evidence" lines (58 and 12 in the 11-18 session) |
| `osc/min` | reversals: a new leg whose first corner lies behind him within 3 s of the last leg's start, per minute | `[offense-approach]` CLAIMED then released pairs, `[loot-road]` DISPATCH and RELEASE pairs under 4 s apart, the "navdist much smaller than dist, legs switching every 2 to 4 s" case in `LootScan.cpp:824` |
| `unreach%` | share of his picks (a foe, a loot source, a spot) that `Env::Path` calls unreachable at the moment of the pick | `[offense-approach]` ENGINE DROPPED, `[lootskip]` navdist against dist |
| `detour` | path length over straight-line distance, mean and p90, for legs he completes | `[lootskip]` `navdist` against `distF` (the only field read of path length today) |
| `holdUnreach s/f` | seconds per fight with act.attack pinned on a foe he cannot reach and cannot shoot | `[target-pin]` lines against `[los]` OCCLUDED runs and `[offense-approach]` drops. This is the Cicero case of 8.5 |

Honest limit: no past log prints a position, so none of these can be calibrated to a number today. The lines above give counts per session and per follower-minute, which is enough to check that the lab's rates are in the field's range (as v3 15 did for damage), not enough to fit a constant. The dev-build dump of 8.6 changes that: a field case then replays in the lab where it happened.

### 2.6 Validation

Step 1, rates: `calib_path.py` (new, like `calib_damage.py`) reads the lines of the table over every `_evidence` run and prints each metric's field rate per follower-minute with its n. The lab prints the same from `sim --path-report`. Step 2, replays: each dumped field case (8.6) is run in the lab from the dumped positions. Pass = the lab reproduces the outcome class (blocked at the same door, the same foe unreachable, sight lost at the same place). Step 3, the parity check of 8.6 bounds how far any extracted cell is from the live one.

### 2.7 CPU cost per fight (ESTIMATE, to be measured by the slice)

An environment has 500 to 20,000 triangles. One A* is 20 to 400 microseconds uncached. A fight asks for a path when a leg starts or its target moves a triangle, about 2 to 10 a second across the bodies, so 0.2 to 4 ms per simulated second, against today's roughly 0.05 ms. That is one to two orders of magnitude on an IAUS fight, which still leaves it at 0.1 to 0.5 CPU seconds. For the oracle it would be ruinous without a cache: a per-`Env` table of triangle-to-triangle path lengths, filled on demand and shared by all rollouts (it is geometry, not fight state, so it is safe to share and deterministic whatever the thread count as long as a miss computes the same value). Door state is the only fight state in a path. Paths are cached per door-state mask. Rays: a grid walk over wall segments, 1 to 5 microseconds. Target: an oracle fight at most 1.5 times today's, an IAUS fight under 0.5 CPU seconds. The slice measures both and reports.

### 2.8 Slice boundaries

- S3 "route search and following": `Env::Path`, the funnel, legs along the mesh for every mover, Movement Blocked, body circles in corridors and doorways, drops, real stairs. Switch `--v4 path`. Synthetic-fixture scenarios with exact expected outcomes (doorway queue, locked door, one-way drop). Baselines for `path=on`.
- S4 "sight by geometry": the wall layer, terrain patches, 3D rays, `--v4 los`. The v3 `lof` corridor on top.
- S5 "pathing metrics": the six tallies, `--path-report`, `calib_path.py`, the oracle-relative bounds measured and written as band rows (the `ORCSURV` pattern of `Playstyles.h`).

## 3. Full looting in the same world as the fights

### 3.1 Today

`Logistics.h` is a separate lab: an episode of 8 abstract stops and a town. At a stop he "finds" bodies and a chest and each item is TAKE or LEAVE. No position, no scan radius, no travel, no leg, no batch, no dibs between followers, no guard, no gate, no fight interrupting a loot and no loot delaying a fight. The IAUS arm scores with the native `ScoreLootKind` and `ScoreConsumable` compiled in. A list world is refused because the list extracts hold no containers. Sixteen loot knobs are searched, 27 are `LOOT_NOT_SEARCHED` rows, 12 of them added in this branch (guard cache timers, batch hold, off-class dwell, the seven gate rows), each with the reason the lab cannot see it.

### 3.2 What the game does that the lab lacks

From `native/logistics` and `iaus/LootNeed.h` (FACT for each name, the behaviour is in MAP.md's logistics entries):

- **Scan**: sources within a walk limit of the follower and the player (`[lootskip] distF, distPlayer, walkLimit 3008, navdist(gate 300)`), bubble and blocklist tests.
- **Batching and commitment**: a batch of sources per trip, `batchHoldMinSec` hold and one rescan before release.
- **Legs**: ch.19 travel with an arrival radius, ended by arrival, BLOCKED, combat or the destination gone.
- **Dibs**: one follower per source, party-wide.
- **Guard checks**: `TheftGate` with its three cache timers, guard-barred corpses, the crime pre-check ("followers never commit detected crimes").
- **Gates**: GATED and GATED-BEHIND records from a blocked leg, the cone (`gateConeDeg`, `gateBeyondU`), the re-admit rows, "a gate is a fact, not a freeze".
- **Party combat stop**: every loot road released when the party enters combat (`RELEASE road=CH19 why=party combat`), resumed after.
- **Carry weight**, the whole-stack check, `roomKnee`.
- **Off-class pieces**: the off-class dwell, the storage chest (STOR), the sell roads.
- **Museum items** (the LOTD free tier and its blocklist), relics the player gave (PGIV).
- **Potions and need**: targets per kind, the weak-potion floor, need rising as stock falls (`needBase`, `needSlope`).

### 3.3 Mechanism

Looting moves into the fight world. An episode is a walk through one environment (or a chain of them through its teleport doors) with its real spawn groups as the fights and its real containers, bodies and loose items as the sources.

- **Sources.** Containers and loose items are `REFS` rows at their real positions. A container's contents are drawn from its base record's leveled lists in the list world (`tools/list_balance` must export CONT and LVLI contents, the same request v2 34.6 already lists). In the built-in world the fixture item table fills them. A body is a source where a foe fell.
- **The scan, batch, dibs, legs, gates and stop** are mirrored from the native files as the v3 mirror batch did for cast delivery: each a small state machine in a new `tools/iaus_sim/Loot.h`, each naming its native file and line, each reading the SAME `LootTuning` table the game ships (`Tune().loot`). The pure scoring stays the native code compiled in.
- **Guards and owners.** An environment's owned containers and its guard spawn points come from the refs (ownership is on the REFR or the cell, a request to the extractor: owner faction or NPC id per ref). The witness check is `Detect.h` plus `StealLab.h`, which exist.
- **Carry, off-class, museum, potions** read the follower's real inventory model from `Logistics.h`, which moves over unchanged.
- **Time is one clock.** A loot leg costs seconds in which the party walks on. A follower 2,000 units behind when the next group is met arrives late to that fight. That is the interaction marth asked for and the reason the two labs must be one.

**The loot oracle.** `Logistics.h` has a rollout oracle over TAKE, LEAVE and SELL RUN. v4 widens its options to the decisions the game makes in a world: go for this source now, skip it, finish the batch, return to the party, and it rolls the SAME fight world forward, so an option that makes him late to the next fight pays for it in that fight's Utility. Its ability set is class-bound like the combat oracle's: only roads the game has (no theft for a class without the steal road, no pick without a lockpick road). Value is one number: the episode's summed fight Utility plus the logistics value of `Logistics.h` (`Value`, `GoldJ`, both MODEL open decisions since v2 34.6, see 11).

**What gets tuned where.** With one world, the loot knobs that were dead become live: `batchHoldMinSec`, the gate rows, the guard timers (only on environments with guards), `takeThreshold` under real travel cost. They are searched by the driver's `loot` stage on episode Utility. The stage's judge already exists (`loot_judge`). Its pooled run changes from `logi:` cells to episode cells.

### 3.4 Validation against field logs

`[iaus-loot]` (2,633 lines in the 11-18 session), `[loot]`, `[lootskip]`, `[loot-road]`, `[theft-gate]`, `[sell]`, `[econ]` give per session: takes by category, skips by reason, legs dispatched, released and blocked, the share of legs ended by party combat, GATED counts, guard answers. `calib_loot.py` (new) prints those rates per follower-minute out of combat. The lab prints the same from an episode run on the same list's environments. marth's method constraint stands (CLAUDE.md field diagnosis 4): arrival counts and travel/arrival ratios prove nothing, because a follower walking past loot during a follow produces arrival-shaped lines. The comparison uses dispatches, releases by reason, blocks and takes, never arrivals.

### 3.5 CPU cost (ESTIMATE)

An episode is 5 to 8 fights plus the walking between them, 3 to 10 simulated minutes, against 0.5 to 2.5 for a fight. Out of combat the world can step at 10 Hz. So an IAUS episode is about 5 to 10 fights' cost. The loot oracle is the expensive part: the combat oracle costs 51 CPU seconds a fight and a loot decision that rolls fights forward pays that per option. Bound it as `Logistics.h` does today: K sampled futures, rolled with the BASE policy in the fights (IAUS, not the combat oracle), which keeps an oracle episode near 20 to 60 CPU seconds.

### 3.6 Slice boundaries

- S8 "loot in the world": `Loot.h` state machines, sources from refs, episodes, the mirrored scan, batch, dibs, legs, party stop, carry. Needs S3. `--episode N`.
- S9 "loot gates and guards": GATED records from real blocked legs, the guard cache, owners, the crime pre-check. Needs S8 and the extractor's ownership fields.
- S10 "loot oracle and the loot stage": the widened oracle, episode Utility, the driver's `loot` stage on episodes, the twelve new knobs moved from `LOOT_NOT_SEARCHED` to `LOOT_KNOBS`, `calib_loot.py`. Needs `list_balance` container contents for list worlds.

## 4. T2 and up: what the sim does not model, or models loosely

### 4.1 Today

The class table is the game's own (`native/classes/ClassTable.cpp`, compiled into the sim): 35 classes and the 3 tree-OFF base cells. The 12 T2-and-below tree-ON rows and the 3 base cells are Beta 1. Their playstyle rows in `Playstyles.h` carry measured bands. The 23 rows of T3, T4 and T5 sit below the PARKED banner: "first draft from the cards, never run, numbers unchecked" (`iaus-classes.md`), active only with `--parked`, which `tune_driver.py --cells all` adds. Round 2 of 11-19 tuned all 38 on those rows.

### 4.2 The walk, per class row

"Loose" = something stands in for it. "No" = nothing reads it. Source for what the game gives a class: its `ClassDef` row, its default gambit rows and its card text in `Playstyles.h`.

| Class | Tier | What the game gives it that matters to a tune | Sim |
|---|---|---|---|
| knight | 2 | held guard (Harbinger block), taunt, bash, short leash, power share cap 0.25 | modelled (batch S). Guard arc is 180 degrees (ASSUMPTION, v3 9) |
| warrior | 2 | power attacks under a 10 s row cooldown, no guard, no retreat | modelled |
| mercenary | 2 | light armor, backs away while fighting (back-off), flank | modelled. "Backs away" is the BackOff family only |
| archer | 2 | bow and crossbow, range hold, the hidden step | modelled. Line of fire by party bodies only |
| thief | 2 | sneak always, opener, dagger from the flank, hide and re-hide | modelled (`Detect.h`). Hide points are picked on a plane with circle cover: LOOSE until section 2 |
| evoker, conjurer, healer, warlock | 2 | spells by role, disengage, spell range (H4), summons, the caster hold | modelled. Books are small random draws (section 7). Summons are one body with a timer: LOOSE (no summon limit by perk, no summon AI differences, no reanimate target choice beyond G7) |
| great-knight | 3 | Shieldwall, taunt at three foes, heavier guard | NO Shieldwall (one mention, no mechanism). Taunt threshold by row: yes |
| champion | 3 | power floor 40 percent, +20 percent power damage | LOOSE: the damage bonus is in the perk chain only if the list's perks carry it. The class passive itself is not applied |
| blademaster | 3 | flurry of light blows, in and out, casters and archers first | LOOSE: target priority through rows. No flurry (one blow at a time, v3's stated limit: no combo framework) |
| ranger | 3 | never swaps to melee, MARKS a closing melee foe for the party, miss budget | `kPsNoMeleeSwap` yes, miss budget yes (H5). The MARK: NO |
| master-thief | 3 | opener one in three, flank half bonus (G4), chases the fleeing | modelled (written, reached only by this line). Never calibrated |
| wizard | 3 | ward only while a hostile spell is charged at him, paralyse then pour damage (+20 percent on the held) | ward timing LOOSE (foe `casting` is filled, the ward is a flat armor value). The +20 percent on a held foe: NO |
| necromancer | 3 | a thrall always up, raise a corpse in reach (G7), shoots what attacks his thralls | reanimate yes (G7 switch). "What attacks my thralls" as a target consideration: NO |
| priest | 3 | healer leash, closes on undead, sun cloak, turn before self heal | cloak as a v3 area: yes. Turn Undead level caps: LOOSE (section 5 and 7) |
| spellsword, sorcerer, templar, nightblade, witchhunter, scout | 3 | the go-in rules (`GoIn`), bound weapon, heal thresholds, control-then-enter | `kPsGoIn` and `World.h GoInOk`: yes for all five rules. Bound weapon: LOOSE (a weapon stat swap, no duration upkeep cost). Never run against a field log: no T3 follower has been fielded |
| arch-mage | 4 | **two different spells at once, one per hand, each hand an independent, mutually aware cast source** (marth 11-17) | NO. The sim's caster has one cast at a time (`Body::act`, one `actSpell`). v3's `hands` switch knows which hand a cast takes, and a stream holds one, but two decision casts never overlap |
| living-legend | 4 | champion mode on one big foe, flurry mode on many, attacks through blows, 40 percent of blows blocked for him | NO for both modes and the passive block |
| nightingale | 4 | Shadowcloak every 30 s (3 s out of sight, re-open with a sneak attack) | modelled (H10, `cloakUntil`) |
| listener | 4 | executes under a quarter health with a heavy blow, Fear at three pressing, Illusion up to his own level | execute as a row condition: yes. Illusion level caps: LOOSE |
| ebony-warrior | 4 | takes a quarter of his neighbours' damage, taunt at two, go-in rule | damage sharing: NO |
| grand-battlemage | 4 | every power attack releases the off-hand spell for free | NO |
| jaqspur | 4 | highest-level foe first, draws 60 percent faster, one miss in ten, marks | draw speed: NO (one mention). Mark: NO |
| saint | 4 | raises the downed first, aura heals allies and burns undead, taunt | revive: NO. Aura: LOOSE (a v3 area, not tied to the passive) |
| dragonborn | 5 | fights as the mastered T4 the player picked, most-perked weapon, switches like Auto, **shouts** (Unrelenting Force at three foes) | NO shouts for a follower (`Data.h` carries a shout recovery and a style weight for FOES only). "As the picked T4": the sim runs his own row, which has no bands |

Across every class, also loose or missing:

- **Hide** and **disengage**: the picks are real native code (`Concealment` rules, `Reposition.h`) on an unreal map. They become meaningful with section 2. Until then every stealth-line and mage-line retreat number is a plane number.
- **Taunt**: modelled with the game's own chance formula. What a taunted foe does next is `PickTarget`, a MODEL (v3 9, item 8).
- **Summons**: as above. A conjurer's tune is tuned against one generic summon.
- **The thief line's theft, lockpicking and pickpocket roads**: `StealLab.h` is a witness pre-check only. No lock, no pick, no crime outcome. Section 3 gives them a world.
- **Foe side**: foes cast and swing from their combat style. Foe shouts for dragons only. No foe uses a potion, flees to heal, or calls for help across spawn groups. Enchantment and poison riders on foe blows are not modelled (9.2 shows they are where the field's bursts come from).

### 4.3 Ranked by tuning impact

Impact = how wrong a tune of the affected classes is while it is missing, times how many cells it touches.

1. **Real geometry for hide, disengage, range and line of fire** (sections 2 and 8). Touches every caster, every archer and the whole thief line: 26 of 38 cells.
2. **Books like the field's and the spell choice pass** (section 7). Every caster and hybrid: 21 cells.
3. **Arch-Mage independent hands.** One cell, but the T4 capstone of the largest line, and the Dragonborn can be one. Without it his tune optimises a mechanic he does not have.
4. **Unseated rows and never-started casts** (section 5). Every cell that holds a paralysis, a turn, a stagger or a disarm spell. Harbinger is seating those rows now (`feat/seat-unseated-casters`), so the lab models both states (`--seats today|all`) and the gap closes on the game's side.
5. **Rider damage and foe spell bursts** (9.2). Every cell: it is the survival side of every tune.
6. **Summons as real bodies with their own AI and limits.** Conjurer, necromancer, sorcerer, arch-mage, warlock.
7. **T4 passives with no mechanism**: Grand Battlemage's free off-hand release, Living Legend's two modes, Ebony Warrior's damage share, Saint's revive, Jaqspur's draw speed, the marks.
8. **Dragonborn**: shouts, and "fights as the picked T4".
9. **Shieldwall, Champion's and Wizard's damage passives.** Small numeric effects on three cells.

The honest consequence: round 2's values for the 23 parked classes rest on rows nobody has checked and, for 9 of them, on a missing signature mechanic. Section 11 asks whether those values should ship at all before items 3 and 7.

### 4.4 Validation, cost, slice

Validation: there is no field log of a T3+ follower. Each mechanic is validated against its native code (a mirror, like 9.1) and its numbers stay MODEL until a T3+ follower is fielded. Cost: each is a few branches in `World.h`, no measurable CPU cost except two-hand casting (two claims in flight, about 1.1 times a caster fight) and real summons (more bodies). Slices: S12 "Arch-Mage hands" (a second cast slot per body, per-hand claims, the oracle offered pairs, mirror of `arch-mage-two-hand-casting-design.md`), S13 "T4 passives" (one switch each, one table `kClassPassives` so "to add a passive, add a row"), S14 "summons", S15 "Dragonborn" (shouts need the game's shout road first: check native before designing).

## 5. Cast starts

### 5.1 Today

v3's `delivery` switch: a follower's cast is a claim that Harbinger builds and the engine fires. Claim-to-fire comes from measured tables (`Data.h Delivery`, 122 windows of one follower, `calib_cast_delay.py`), with a share of claims that never fire (`--deliver-unfired`). The share is one number for every spell.

### 5.2 What is missing

- **Rows Harbinger does not seat.** `cast/Archetype.cpp kUnseatedRows` (FACT): Stagger, Disarm, TargetEffect, Paralyze, and Reanimate without ABI 19. A spell that keys one of those rows never starts through a claim. The field case that found it: Jesper's Turn Lesser Undead keyed TargetEffect, 8 claims in 48 s held the spell-gambit lock (2026-11-19). In the lab that spell fires like any other, so the tuner values control spells the game cannot cast. The lab's `SimSpell` has no engine row.
- **Never-started releases.** `cast/CastOn.cpp OffenseNeverStartedFightLong / OffenseNeverStartedWindowMs` (FACT): an offense claim with no fire and nothing charging is released after a window. A row no seat starts (unseated, Script, NoRow) uses the short window (`kHealClaimNeverObservedCapMs`) and the spell is held off for the fight on every target after ONE failure. A seated row uses the long window (`kHoldLastSeenCapMs`) and needs two failures in a row on the (spell, target) pair. The lab has neither window nor the hold-off, so a never-starting spell costs it one generic "unfired" draw instead of 4 to 6 s of a held hand and then the loss of the spell.
- **The buff road's never-fired release** (`BuffRoad`, Blur stalled about 6.5 s per reset in field 11-19, cause on Harbinger's side and unexplained).
- **The 11-17f cast-layer scalars**, shipped as placeholders "game only until the sim mirror" and still unmirrored after slice 1: `t.occClaimRelease` (a claim on a target his own ray has read occluded for 4 s is released), `t.offApproachBackoff` and `t.offApproachDropCap` (section 2.3), `t.sharedRadiusMargin`, `t.preemptSettle`. The first needs occlusion age per claim (the lab has `occSince`). The approach pair needs section 2. The last two are small.
- **Interrupts.** A cast is lost in the lab when the caster goes down (`castLost`). A stagger, a bash, a paralysis or a ragdoll on a charging caster does not interrupt his cast, and a foe's cast is never interrupted by the party's blow. Whether a plain hit interrupts an NPC's charge is not decoded (`World.h --draw-spoil` says the same for a bow draw).

### 5.3 Mechanism

- `SimSpell::row`, the predicted engine row, computed by the game's own classifier. `ClassifyArchetype` reads engine records, so the list extract must export what it reads (archetype, delivery, casting type, the flags it tests) and a pure function shared by game and sim does the classification. **That is a native change: moving the pure part of `cast/Archetype.cpp` behind an engine-free input struct. Reported as a finding (12), not made.** Until then the sim carries a copy of the row table with a harness check that fails when native's differs, the pattern the vocabulary generator uses.
- A claim on an unseated or Script row never fires. It holds its hand for the short window, is released, and the spell is held off for the fight. A seated row that draws "unfired" holds for the long window and counts toward the two-failure rule. One function, `Sim::NeverStarted`, mirrors `CastOn.cpp`'s two readers by name.
- `--seats today|all`: `all` is the lab as if Harbinger seated every row. The difference per class is the measured value of a Harbinger seating brief, in J, which is the number that brief needs to be ranked.
- Interrupts: a stagger or a control effect on a body with a charge in flight cancels it and refunds nothing (MODEL, source: the engine's stagger ends the cast animation, which is what `SpellFire` hangs off, `observed-vanilla-cast-sequence`). A plain hit does not (ASSUMPTION, a knob, off by default as `--draw-spoil` is).

### 5.4 Validation against field logs

`calib_cast_delay.py` already joins `[iaus] pick=cast_*` to Harbinger's `[ctcensus]` windows. It gains: per predicted row, the share of claims that never fired and the time each held (`[cast] ... NEVER FIRED: claimed N ms ago ... predicted row R` gives all three), and per spell the hold-offs. `_evidence/deck-run-1119-jesper` has the lines. The lab's `dl.fired%` per row must land inside the field's range per row, not only overall.

### 5.5 CPU cost

None measurable. A table lookup per claim.

### 5.6 Slice boundary

S6 "cast starts": `SimSpell::row`, the two release windows and hold-offs, `--seats`, the four mirrorable 11-17f scalars, the interrupt model, the per-row calibration in `calib_cast_delay.py`. Sim only except the reported native refactor. Does not need environments.

## 6. The tune itself

### 6.1 What a round costs (ESTIMATE, from this branch's measurements and v3's)

Measured here: an IAUS seed run costs 0.03 to 1.0 CPU seconds. A smoke classes pass for one class did 2,058 seed runs in 53 s on 8 cores. The thief's knob table is 480 knobs, 69 live.

Search stages (classes, edge, joint, globals, consts, floor, rating, recalib), full configuration (`FULL`: train 64, holdout 64, confirm 96, 2 passes): per class about 6,000 liveness runs, 20,000 to 35,000 raced sweep runs, and per accepted value about 2,000 gate and cross-world runs. At a mean 0.2 CPU seconds that is 2 to 3 CPU hours a class, more for a T1 row (its values are gated on every cell). **38 cells: roughly 90 to 130 CPU hours.** The joint stage on 38 cells: 38 x 3 configs x 128 seeds x 2 tables, about 29,000 runs, 2 CPU hours, plus one such pass per T1 value left out.

Oracle stages. v3 14.10: 51.2 CPU seconds a natural fight, subject only. The gap stage's first pass is cells x 12 seeds x 5 fights x 2 tables: for 38 cells 4,560 fights, **about 65 CPU hours per world, 260 for four worlds**, plus the every-follower fights (114 CPU seconds each on the mage line) and the re-measures after an accepted value. v3's own figure for 15 cells, 3.4 hours on 16 cores per world, scales to the same number. The band measurement (9.6): 160 fights per caster class with every follower planned, about 7 CPU hours a class, and 15 of the 38 cells hold spells: **about 105 CPU hours**.

**Total for 38 cells on four worlds: about 450 to 550 CPU hours.** On this 16-core host at idle priority with the machine otherwise quiet that is 28 to 35 hours of wall time. With the machine in use it is whatever is left over, which is the point of idle priority.

### 6.2 Where it can be cut without weakening the oracle

"Without weakening" means: the oracle that judges is the same search, with the same ceiling, on the same fights. Cuts are in how often and for whom it runs, never in how hard it thinks.

1. **Share the T1 companions' fights.** Every non-T1 cell's fights hold the three T1 companions. Their oracle decisions are recomputed in every cell's every-follower pass. Subject-only already avoids it in the gap stage. The band measurement does not. Measuring the bands subject-only beside IAUS companions (which is also review F7's objection: the subject is better protected beside oracle companions) cuts that stage about three times and fixes the finding. Needs marth's nod because it changes what the bands mean (11).
2. **Early stopping of the gap per cell.** The gap stage grows a cell from 12 to 48 seeds when its gap is noisy. Stop a cell when its gap's standard error is under the stage's decision threshold instead of at a fixed count. A sequential test on the measurement, not on the oracle.
3. **Do not re-measure what did not change.** After an accepted value for class C only C's gap changes, unless C is T1. The driver's cache already keys on the curve table. Keying on the class's own slice of the table (plus the T1 slice) lets the other 34 cells replay.
4. **Skip dead cells.** A class whose classes stage accepted nothing and whose T1 baseline did not move has the shipped gap. One table instead of two.
5. **The ceiling stays at 128.** `--oracle-ceiling 32` costs a third and v3 14.2 could not show a loss, but it is a weaker oracle by definition, and under the standing rule that is marth's call, not a cost lever (11).
6. **The path cache of 2.7** when environments arrive. Without it everything above is swamped.

Items 1 to 4 together: an ESTIMATED 40 to 50 percent off the oracle stages, so a round near 250 to 300 CPU hours.

### 6.3 Which stage makes the oracle gap the main score

Today the classes stage accepts a value on IAUS's own J: train `d > 2.5 se and d >= 0.15 J`, then holdout, gates and cross-world, all on J. The oracle is a fence only: the oracle-relative survival and heal-timing bands in the class card (`downs/f`, `selfHealLow%`, `healLate%`, `dwh/f`) can reject a value, and the `gap` stage runs AFTER the classes stage and searches only the families its (a) clusters name, inside a bound (`gap_families` 3, `gap_knobs` 12). So "tuning follows the oracle" is true of what gets a second look, and not of what gets accepted.

Design: a new stage, `follow`, between `classes` and `joint`, that makes the gap the score.

- **Score**: for class C, `G(C) = mean over its gap fights of (oracle U - IAUS U)`, paired per fight, subject only, natural bracket plus the class's own scenarios (the gap stage's shape).
- **Search**: the candidates are the classes stage's sweep survivors (every value that beat noise on train, not only the winner) plus the gap stage's cluster-named knobs. A candidate is accepted when it LOWERS G by more than 2 se on the gap seeds, and holds on fresh gap seeds, and passes the unchanged J gates (no class worse, the Knight floor, the card). J stays the fence. The gap becomes the objective. That inverts today's roles.
- **Why the oracle's own U does not move**: the oracle is run on the shipped table's fights once per class and seed (its rollouts use IAUS as the base policy, so strictly it does move a little with the table: the stage re-runs the oracle for an accepted value only, and reports the oracle U shift as a number so a "gain" that is the oracle getting worse is visible).
- **Cost**: the IAUS arm of a gap fight is milliseconds. The oracle arm is paid once per class and seed and cached. So a candidate costs IAUS fights only, and `follow` adds about one gap-stage first pass per world, not one per candidate.
- **Divergences a tune cannot close** ((b), (c), (d) of v3 14.6) are subtracted from the target: the stage reports "closable gap" beside the gap, so a class is not searched forever against a missing road.

The spell choice stage of section 7 and the movement stage of section 8 are the same shape: regret against the oracle as the score, J as the fence.

### 6.4 Validation, slice

Validation: the stage is proven on the tuner's own terms, the racing proof's pattern. On a fixed cache it must accept a planted improvement (a curve moved toward the oracle's pick in one cluster) and reject a planted regression. Slice S7 "the follow stage": `stage_follow`, the cuts 1 to 4 of 6.2, the closable-gap report, tests. Driver only. It needs no environment work. It is in batch C (section 10), which runs beside the world batch, so that the one tune that follows this round has the gap as its score.

## 7. The spell choice pass

marth, 2026-11-23, Deck on 2.11.9: "My tests on deck were better btw. I noticed that the spells being chosen were still pretty ineffective, certainly were probably better options available. But I was unsure if a proper spell tuning pass occuired".

None did. The rounds tuned family and consideration curves per class. The Evoker kept one value (`evoker:4.63.a`, the retreat danger curve). Nothing in any round ranks one spell against another. Per-spell worth comes from the native estimate, whose weights are labelled placeholders.

### 7.1 Does the sim run followers with real learned books?

Real records, unreal books. A lab follower's book is drawn from the list world's castable pool (`Data.h spells`, every spell a book teaches, pruned by `Prune`): `Load.h:291-310` takes 2 to 8 spells he can afford at his skill, at random, plus one heal, plus a staff. The built-in world draws from a small fixture pool.

The field follower: Jesper (750012C6, an Evoker, `_evidence/deck-run-1119-jesper`, `[spellbook]` at 16:42:01) holds **74 offense spells, 4 control, 13 heal, 8 buff, 3 ward, 22 not candidates**. So the field decision is "which of 74", and the lab's is "which of 3". A choice problem that is easy with 3 options and hard with 74 has never been in front of the tuner. Nor has the oracle seen it: its options are the generator's candidates, so with a 5-spell book it never had a better spell to prefer.

Needed: (a) books of field size and shape. A `[spellbook]` and `[iaus-fill] spell` dump of a real follower is test input like a list world: `tools/list_balance` gains `book_from_log.py` that turns those lines into a book file keyed by (plugin, local) ids, and the lab loads it with `--book <file>` for a class. (b) Generated books for classes nobody has fielded: every spell of the class's schools the follower's skill allows, the worst case for choice. (c) Both, because a tune on Jesper's book alone is a tune for Jesper.

### 7.2 What decides spell A against spell B today, term by term

From the field lines of 16:42:30 to 16:42:36 (same lap, same follower, the foe a Draugr Deathlord):

```
cast_control Turn Lesser Undead -> Draugr Deathlord = 1.374 | spell_perks 0.81>1.15 | range 0.12>1.00 | los 0.50>0.98 | recast 1.00 | threat 0.27>0.47 w1.5 | foe_count 0.00>0.48 | affordable | mp_reserve w0.5 | control_worth 1.00>1.00 | aw 3.60 cw 1.00 pf 1.50
cast_offense Reflective Light   -> Draugr Deathlord = 0.950 | spell_perks 0.81>1.15 | range | los | recast | threat 0.27>0.66 | on_player | player_target 0.00>0.89 | current_target | exploit 30>0.53 w1.3 | affordable | mp_reserve | variety | damage 1.00>1.00 | overkill 1.00 | aw 1.77 cw 1.00 pf 1.50
cast_offense Sunbeam            -> Draugr Deathlord = 0.878 | ... exploit 0.00>0.75 w1.3 | damage 0.18>0.65 | aw 1.77 ...
```

| Term | What it is | Decides | Can a tune move it? |
|---|---|---|---|
| `aw` (family action weight, `Catalogue.cpp:446`: control 3.6, heal 2.4, offense 1.769) | one number per family | **cross-family.** Control starts at twice offense. This is why a Turn spell outranks every damage spell in the example | yes: `aw.<family>` shared, `family.<family>` per class. The rounds searched it per class, on small books |
| `damage` (`Cons::DamageValue`: this spell's `SpellOn` over his best spell's on this foe) | within offense | the main within-family term. Reflective Light is his best (96 a cast), Sunbeam reads 0.18 of it | its curve, yes. Its INPUT, no: see 7.4 |
| `exploit` (`Cons::Exploit`: the foe's resist to the spell's element) | within offense | weak-to and resisted | curve and weight, yes |
| `overkill` (foe health over expected damage) | within offense | wasting a big spell on a nearly dead foe | yes |
| `variety` | within offense | repetition | yes |
| `spell_perks` (perk-effect reader, placeholder weights) | every cast family | school mastery, cost and magnitude perks | weight yes, shipped as a labelled placeholder |
| `control_worth` (`controlSec / 10`, times a health term) | within control | a long control on a healthy foe | curve yes. Input capped since 2.11.9 |
| `threat`, `foe_count`, `range`, `los`, `recast`, `on_player`, `player_target`, `current_target` | target terms | which foe, not which spell (equal for two spells at one foe, apart from `range`) | yes, and the rounds did |
| `affordable`, `mp_reserve` | cost gates | a spell he cannot pay, or one that empties him | curve yes. Neither reads cost per effect |
| `effectRate` tie-break (`Generate.cpp:588`, 11-17b) | equal scores | effect per second of cast, with `t.spellCastSec` 1.5 s for every fire-and-forget spell | `t.spellCastSec` is in the driver's GLOBALS |
| the top-five foe list, the class gate, hand and cost drops (`[iaus-gen] dropped ... target 20`) | before scoring | what is never a candidate | no (structure) |

Two readings. First, cross-family choice is the action weights, one constant per family, and nothing asks "what does this control spell buy against what that damage spell buys, in the same unit". Second, within offense the ranking is almost entirely `damage`, and `damage` is magnitude per cast with nothing else in it.

### 7.3 A measure of choice quality against the oracle

**Spell regret**, per decision at which IAUS chose a cast or the oracle did. The oracle's side is its normal search (rollouts to the fight's end, class-bound, his book, his magicka, this foe), restricted to the castable spells. For the subject:

- `regretU` = the oracle's paired lead of its best castable spell over IAUS's pick, in U. This is the truth measure and it is what the stage optimises.
- Two diagnostics that explain a regret without a rollout, printed beside it, computed from the lab's world (which knows the real outcome, not the estimate): **effect per cast second** (health removed, or health restored, or foe-seconds controlled times that foe's damage rate, over claim-to-fire plus cast plus recover) and **effect per magicka**. Regret in each = best castable minus chosen, over best.
- Split **within family** (the oracle's best spell is in the same family as IAUS's pick: a ranking error) and **cross family** (control against offense against buff against summon against heal: a weight error). The second needs one unit across families, and the rollout's U is that unit. The diagnostics convert through the foe's damage rate so a control second and a damage point can be read side by side.
- Reported per class and book, by family pair, with the count, the mean regret and the share of decisions with any regret. `SPELLREG <class> <iaus family> <oracle family> n regretU eps_regret epm_regret` lines, the `GAPDEC` pattern.

The field side: `[iaus-why]` prints the top three with every term, so a field book replayed in the lab (7.1) gives the same decision twice, once as the game scored it and once as the lab's oracle would have chosen. The example above is the first test case: with Jesper's book against draugr the stage must say whether Turn Lesser Undead at 1.374 was right. (Section 5 adds: it keyed an unseated row and could not fire at all on that build. marth's ruling is "run every spell": no spell is vetoed for its row. It stays a candidate, bounded by the never-started release, and Harbinger is seating the rows now, `feat/seat-unseated-casters`. So the lab models the release and the hold-off exactly, and a spell that cannot start today costs what it costs in the game: one short window, then it is held off for the fight.)

### 7.4 Missing from the estimate, against only badly weighted

MISSING: no input exists, so no weight can fix it.

| Gap | Evidence |
|---|---|
| **Time to land** per spell. `effectRate` uses one `t.spellCastSec` for every fire-and-forget spell. Charge time, projectile travel, claim-to-fire by row and hand redraw are not in it | `Generate.cpp:588`. The field's claim-to-fire tables differ by family (v3 13.2) |
| **Channel uptime.** A concentration spell is "counted for 1 s" | `[iaus-fill] spell ... concentration=false (counted for 1 s)`. A stream held 4 s and a stream cut after 0.5 s by a line-of-fire hold read the same |
| **Level caps and immunities** of control, fear, calm, turn. `undeadOnly` exists. The capped effect is judged since 2.11.9 for control. Whether a turn or an illusion can affect THIS foe by level is will-work, learned by trying | `Cons::ControlWorth`. `wwMirror` in the lab |
| **Existing effects.** `activeOn` stops a recast. A different spell with the same effect archetype (a second fire DoT, a weaker armor spell) is not seen as redundant | `SpellEntry::activeOn` is per spell form |
| **Kill-confirm.** `overkill` penalises excess. Nothing rewards the cheapest spell that finishes a foe before it acts again | no fact |
| **Magicka per fight length.** `mp_reserve` is a level. Nothing reads expected fight length against magicka per effect, so an expensive spell early is never weighed against being dry later | the oracle drinks magicka potions 3 to 9 percent of its divergence (v3 14.6) |
| **What a failed start costs.** Not a veto (marth: "run every spell"). Missing is the COST of a start that never fires: the seconds a claim holds a hand before the never-started release | section 5. Moot per row as Harbinger seats it |
| **Duration perks, dual cast, the poison-resist swap** | the fill line's own "left out" list |
| **Armor** for spells that deal physical damage, and **summon worth** in damage-per-second terms | `SpellOn` uses element and magic resist only |

ONLY BADLY WEIGHTED: the input exists, the constant is a placeholder or was tuned on books too small to matter.

- The family action weights (control 3.6 against offense 1.769).
- **Area.** The input exists: `Facts::foesInArea` (the foes inside the spell's area at the target, `Generate.cpp:850-860`), `splashSafe` (no ally inside it) and `Cons::AreaValue` (`foesInArea / 3`). What is weak is its use: it is a separate boost, and `expDamage`, so `damage`, `overkill` and the tie-break, stay one foe's worth. An area spell on four foes ranks by what it does to one.
- `damage`'s curve (0.18 of the best reads 0.65, so a spell five times weaker keeps two thirds of its score).
- `exploit` and its weight 1.3. `spell_perks` and its 1.15 ceiling. `variety`. `overkill`.
- `t.spellCastSec`.

### 7.5 The tunables it exposes, and the stage that searches them

One table, native, `iaus/Core.h` `struct SpellWorth` inside `Tuning` (the `WorthTable` and `LootTuning` pattern: one struct, one name per field, `sw.<field>` in the tuning file and the sim, a `SetSpellWorthKnob` reader, a `test_tune_driver` test that the driver's knob list covers the struct). Fields, each replacing a missing input above with a measured or declared one: `landSecBase` and per-row `landSec[row]`, `channelUptime`, `aoeFoeWeight` and `aoeFriendPenalty`, `redundantEffectScale`, `killConfirmBonus`, `magickaHorizonSec`, `summonDpsScale`, `areaFoesIntoDamage` (how far `foesInArea` multiplies the expected effect), `failedStartSec` (what a start that never fires costs, read from the never-started windows of section 5. Never a veto: marth's ruling is "run every spell"). And ONE shared reader, `Estimate::SpellWorthOn(entry, foe, snapshot, table) -> {effect, seconds, magicka}`, which every cast family's worth consideration calls. Two readers of "what is this spell worth" is how the next 11-13 loot bug is born. "To add a term to spell worth, add a field to `SpellWorth` and read it in `Estimate::SpellWorthOn`." That is a native tier-B change with its own brief. Not made here.

Driver stage `spells`, after `follow`: knobs = the `sw.*` table plus the cast families' `aw`, the `damage`, `exploit`, `overkill`, `variety` and `control_worth` curves. Score = mean `regretU` over field-size books (7.1), pooled over the caster cells for shared knobs and per class for class rows. Fence = the J gates and cards as everywhere. It is a shared-table stage like `worth` and `loot` (`shared_search`), so it is mostly wiring.

### 7.6 Validation, cost, slices

Validation: field `[iaus-why]` decisions replayed (7.3), and after a tune the same replay must show the picks changed where the oracle disagreed. Cost: a 74-spell book makes a caster's generate-and-score about ten times larger (still under a millisecond a lap) and the oracle's option list much longer. The race drops most options after 8 streams. ESTIMATE: 2 to 4 times a caster's oracle fight. Mitigation that does not weaken the oracle: options that are the same outcome on every stream already count as one (v3's duplicate rule), and dominated spells (same family, same element, lower damage, higher cost, same range) can be pruned BEFORE the race, exactly, because a dominated spell cannot be the best.

Slices: S11a "field books" (`book_from_log.py`, `--book`, generated worst-case books, the regret report, exact domination pruning). Sim and tools only. S11b "SpellWorth table" (native tier B, Sonnet from a Fable skeleton, Opus review, then its sim mirror, which is free because the sim compiles native). S11c "the spells stage" (driver).

## 8. IAUS chooses where to be

marth: "This means the sentient AI can now power movement, fully aware of the environment. line of sight, etc. Make sure thats included, and tested in the sim."

So the environments are not only a better test world. IAUS itself decides WHERE to stand, and the sim proves that decision before it ships.

### 8.1 The decision

Today IAUS picks WHAT (a family, a target, a spell). Where he is when he does it belongs to the engine's combat style, plus a handful of point picks made outside the scorer (`Concealment` hide and flee points, the hidden step, the back-off point, the ch.24 approach). Each of those is its own rule set with its own constants.

v4: **a position is a candidate.** There is ONE decider of where he stands: the shared movement pass of 8.7, once per 500 ms window for the whole party. The action facet (his own lap, today's scheduling) never picks a position.

**Candidate spots**, generated from the navmesh around him by ONE function, `Env::Spots(self, snapshot, kinds, budget)`, each tagged with its kind. A kind is a generator rule, not a hard-coded behaviour:

| Kind | How the spots are found |
|---|---|
| firing spot | triangles within his next action's range band of the target with a clear ray to it, nearest by path first |
| cover, break line of sight | triangles from which every ray from the foes that threaten him is blocked. The triangle's own cover bits where the game stored them |
| high ground | reachable triangles above the target's by more than a step, with sight |
| chokepoint | portal edges and door triangles between him and the foe group (few neighbours, narrow width) |
| flank | points behind the target relative to who it faces, reachable without crossing its front arc |
| heal reach | the nearest point from which the recipient is in range and in sight |
| peel | a point between a threatened ally and his attacker |
| kite path | the next point along a route that keeps the pursuer's path longer than his own |
| regroup | a point within the leash of the player or the healer |
| loot route | the next source of the batch (section 3) |
| retreat with an exit | a point away from the threats from which at least one further route leads on. A dead end is not offered, or is offered with its cornered cost |
| reach the unreachable | when his target has no path: the nearest point from which it is in weapon or spell range, else none (and the target consideration below says pick another foe) |

**Considerations over a spot**, each one a `Cons::*` with a curve and a weight in the ONE existing catalogue, each fed by one fact in `Facts`, each fact filled by one geometric reader:

| Consideration | Fact | Reader |
|---|---|---|
| path cost to the spot | `spotPathLen` over his speed | `Env::Path` |
| sight to his target from the spot | `spotSeesTarget` | `Env::Ray` |
| sight FROM the threats to the spot | `spotExposure` (count, or summed threat) | `Env::Ray` per threat |
| line of fire through allies from the spot | `spotFireLineClear` | the existing `Estimate::InLineOfFire` on the spot's position |
| range band of the action he will take there | `spotRangeX` | `Reposition.h` range functions, unchanged |
| exposure time along the path | `spotPathExposedSec` | `Env::Path` corners sampled with `Env::Ray` |
| being cornered | `spotExits` (routes out not through a foe) | `Env::Exits` |
| spacing from allies and from active areas | `spotAllyDist`, `spotInHostileArea` | positions, the v3 area list |
| anchor and leash | `spotLeashX` | the class's leash (ch.23 radius, `kNearPlayer`, `kShortLeash`) |
| stairs and ledges on the way | `spotClimb`, `spotDrop` | `Env::Path` edge kinds |
| hide and sneak exposure | `spotDetected` | `Detect.h`'s formula with real rays |
| target reachability | `foeReachable`, `foePathLen` on the TARGET's facts | `Env::Path` |

**Chosen together, by one decider.** Who decides what, and who wins:

- The **action facet** decides WHAT, as today, on his own lap: a family, a target, a spell. It keeps every consideration it has.
- The **movement pass** decides WHERE, once per window. For each follower it scores (action, spot) pairs for the action he holds when the pass runs AND for the next two of his last ranked list (the action facet's own ranking, cached with the picture). So "a spot for a spell he can then cast" is real: the pass may choose a spot that serves his second-ranked action better than his first, when the pair scores higher.
- The pass publishes, per follower, the spot and the action it was chosen for (`spotAction`). The action facet reads two things back as plain facts: where he is going (its range, sight and line-of-fire considerations are evaluated at the committed spot once he is inside its radius, and at his own position before that), and `spotAction`, which gets the existing momentum bonus. It is a bonus, not an order.
- **Position wins inside a window. Action wins across windows.** While a spot is committed the action facet cannot move him. It can only choose among actions, and those that work from the spot score better because their range and sight facts are good there. If it picks another action anyway (a heal the moment an ally drops), he does it from where he is if it can be done, and the next pass, at most one window later, plans for the action he then holds. An action change is one of the events that marks him for the next pass. It never triggers a pass.

The pass pairs an action only with spots that make sense for it: a cast with firing spots for that spell's range band, a heal with heal-reach spots, a melee attack with reach points and flank points, a retreat with exits. "Stay here" is always a spot, with zero path cost. The pair's score is the action's last score times the spot's considerations, so a strong spell from a spot he can reach in one second beats a stronger one from across the room. The existing act.attack veto for a waived foe, the range vetoes and the line-of-fire refusal stay where they are in the action facet. What changes is that the pass offers the spot from which they no longer bite.

**Per class and playstyle.** Spot kinds are gated by the class row exactly as families are (`ClassGate`): a Knight is offered peel and chokepoint and never retreat, a Thief hide and flank, a healer heal reach and regroup. Weights per class live in `ClassCurves` rows like every other class weight, searched by the tune.

**Hysteresis.** He never dithers, by three mechanisms that already exist for actions and are reused inside the pass: momentum (the spot he is walking to gets a `t.postureMomentum`-style bonus until he arrives or it becomes invalid), a commit per leg (a committed follower is not re-scored unless an event of 8.7 marked him), and a minimum gain (a new spot must beat the current by `spotPreempt`, the `t.preempt` pattern). The `osc/min` metric of 2.5 is the gate on all three. The cadence of the decision itself is in 8.7.

**One table.** New tunables are fields of `Tuning` with `t.spot*` names and rows of the consideration catalogue. New kinds are rows of one `kSpotKinds` table (name, generator function, which families may pair with it). "To add a spot kind, add a row to `kSpotKinds` and its generator in `iaus/Spots.cpp`. To add a consideration, add a `Cons` and its `Facts` field. To add a geometric fact, add a method to `Env` and fill it in `Spots.cpp`, nowhere else."

What is removed in time, not at once: `Concealment`'s own pick rules, the back-off point, the hidden step and the approach's "walk toward the pinned foe" become spot kinds scored by the same catalogue. Each moves over only after its spot kind reproduces its field behaviour in the lab.

### 8.2 One code path for game and sim

The scoring, the spot generation, the route search and the ray logic live in `native/iaus` (`Spots.h/.cpp`, `EnvQuery.h`), pure, engine-free, compiled into the sim as `Generate.cpp` is today. Underneath is a small interface:

```
struct EnvView {                       // iaus/EnvQuery.h: what the pure layer may ask. No engine type in it.
    int   TriAt(Vec3 p) const;         // -1 = off the mesh
    Span<Tri> Tris() const;            // vertices, neighbours, flags, cover, edge kind (portal, ledge up, ledge down, door)
    Span<Door> Doors() const;          // triangle, open / closed / locked, key
    Span<Seg> Walls() const;           // sight blockers the view knows (boundary walls, boxes, collision when present)
    uint64 Stamp() const;              // changes when a door or a triangle changes: every cache keys on it
};
Path  FindPath(const EnvView&, Vec3 from, Vec3 to, BodyClass);   // ONE implementation, game and sim
Sight CastRay(const EnvView&, Vec3 from, Vec3 to);               // geometry rays (see below for engine rays)
```

**The sim** implements `EnvView` over an extracted `.mfoenv` (or a dumped one, 8.6).

**The game reads the ACTIVE navmesh.** marth: "Instead of extracting it reads the active navmesh". No extracted and no shipped environment data in the game, ever. The game's `EnvView` is built from the engine's own navmesh objects of the loaded cells, with their live state: closed and locked doors, triangles the engine disabled or cut, and runtime obstacles where the engine exposes them.

What the game side needs, and how sure each piece is:

| Need | Status |
|---|---|
| The navmeshes of a loaded cell | **Already read in shipped code**: `native/logistics/LootReach.cpp:40-165` (the loot walk's connectivity pre-check) takes the cell's `spinLock`, walks `GetRuntimeData().navMeshes`, finds the triangle under a point (`FindTriangle`) and flood-fills neighbours, edge portals and door portals. It runs on the MAIN thread, posted, with its verdict cached, because a cell unload happens on main. It treats closed doors, obstacles and enable/disable portals as passable and answers Island only when certain. That file is the pattern and the proof the members can be read on the shipped runtimes: the snapshot builder is the same walk under the same lock, copying into plain arrays instead of flooding. Declaration: `TESObjectCELL` runtime data `navMeshes` (`NavMeshArray`, `BSTArray<BSTSmartPointer<NavMesh>>`). DECLARED in the fork (`RE/T/TESObjectCELL.h`). The member's offset is VERIFIED on 1.6.1170 and 1.7.104 as the same (`Docs/ENGINE_NOTES.md` layout table, the `navMeshes 0x80` row, already read by `LootTake.cpp` and `Lotd.cpp`). 1.5.97: NOT verified here, own row needed (principle 11) |
| Vertices, triangles, flags, neighbours | `BSNavmesh::vertices / triangles`, `BSNavmeshTriangle {vertices[3], triangles[3], triangleFlags, traversalFlags}` (size 0x10, the same 16 bytes as the NVNM record the extractor decodes). DECLARED in the fork's `RE/B/BSNavmesh.h` and noted in `Linux-Native-Tools/actor-ai-and-packages.md:215` (`triangles` at 0x028). `vertices`, `triangles`, the edge portals and the door portals are exercised by `LootReach.cpp` in the field on 1.6.1170. The members it does not read (`closedDoors`, cover edges, the grid, obstacle data) are NOT verified against the disassembly on any runtime, and none is verified on 1.5.97 here. CommonLib declarations are not ABI-trustworthy (principle 6): each needs a disassembly check per runtime and a `VERIFIED-ADDRESSES`-style row |
| Edge links, ledges, portals | `BSNavmeshEdgeExtraInfo {type: portal, ledge up, ledge down, enable/disable portal}`. DECLARED. Meaning of the types as the pathfinder uses them: needs RE or a probe (this also settles the extractor's `LINK.kind` open item) |
| Door state | `BSNavmeshTriangleDoorPortal`, `BSNavmeshClosedDoorInfo` (a `BSPathingDoor` and a triangle). DECLARED. Whether `closedDoors` is the live list the pathfinder consults: needs a probe (open a door, dump twice) |
| Runtime obstacles | `BSNavmeshObstacleData`, `BSNavmeshObstacleUndoData`. Forward-declared only. Needs RE. Not required for a first version: an obstacle the view lacks shows up as a Movement Blocked leg, which the design already handles as a fact |
| Point location | `BSNavmeshGrid` (DECLARED). Not needed: the snapshot builds its own grid |
| Pathing cost as the ENGINE computes it | not needed and not used. The pure `FindPath` runs on the snapshot so game and sim share it. The engine still walks its own path when a channel moves him. The two can disagree. That disagreement is a metric (the leg's travelled length against `FindPath`'s, logged per leg) and a calibration input for the edge costs of 2.2 |
| Rays | **In the game the engine's rays are the authority for line of sight and line of fire.** marth: "I would lean on the rays, but certainly pull what can be done." The existing rays come first: `Sightline`'s `[los]` verdicts (three rays, own ray and engine ray, a cache) and the line-of-fire guard's. The active navmesh answers reach and path, never sight. Engine rays run on the MAIN thread only, so the movement pass of 8.7 asks for its rays as one batch, the main thread answers them spread over the frames inside the decision window, and the answers land in the shared picture. The snapshot's own geometry (boundary walls) is used for one thing in the game: to choose WHICH rays are worth asking (a spot with a wall between it and the target is not asked about). It never overrules a ray. In the SIM there is no engine, so geometry rays stand in for engine rays, with the error of 8.5 |
| Threading (INVARIANTS #4, #83 to #85, CLAUDE.md "Threading") | the navmesh is engine state that the main thread mutates (cell attach and detach, doors, obstacles). The worker must never walk it. So: a **main-built snapshot**, the perk-effects pattern (#85: built on the main thread, read anywhere). `MainThread::Post` builds an immutable `EnvSnapshot` per loaded cell (plain arrays, no engine pointer), publishes it by atomic shared pointer, and rebuilds on cell attach, cell detach and door events. The worker reads snapshots only. Nothing is pending across a save (#83): a snapshot is derived state, dropped in `ResetAllState` after `StopPump()`, never serialised. No co-save change |
| Budgets | for the party's pass as a whole, 8.7's table (`kMoveBudget`). Path lengths cached per (snapshot stamp, triangle pair). A pass that hits a budget scores fewer spots, never stale ones. The action facets are served by the pump at 133 ms times the party (1.3 s a lap for ten), and the movement pass of 8.7 does not ride that round robin at all: it runs once per `moveDecisionSec` for the whole party (principle 9: sized from its own cadence) |
| Runtimes | 1.6.1170 first (proven there ships there), 1.5.97 and 1.7.104 each with their own verified layout rows. A runtime whose rows fail the self-check gets NO environment view, and IAUS degrades to today's behaviour (spot = "stay here" only, the engine's combat style moves him). That is a documented contract (framework absent means legacy path), not a mask, and it is logged loudly once |

No list-specific data: the game reads what is loaded, whatever the load order.

What needs RE or a probe before any native slice, in order: (1) `BSNavmesh` member offsets on three runtimes, (2) a passive dev-build dump of a loaded cell's navmesh compared with the extracted cell (this is the parity check of 8.6 and it IS the probe: principle 5, observe before building), (3) door-state liveness, (4) edge extra info semantics, (5) obstacles.

### 8.3 Execution: which Harbinger channels carry it

IAUS decides a spot. Harbinger moves the body, so the engine's own path following and animations stay (animations are required for all actions, standing rule).

**A held world point already exists.** ch.24 CombatApproach with `kApproach_Point` (ABI v39, `APMF_API.h:829-855`): the centre is a world point (`param.pos`, target 0), `param.fval` is the radius, the actor fights the whole way, arrival ends the claim unless `kApproach_Hold`, and a point the engine cannot path to ends it as `kApproachState_EngineDropped`. With `kApproach_Hold` he stays within the radius of the point until the client releases it. MFO uses both today: `apmf/GroundApproach.cpp` (give ground: the point, ends on arrival) and `apmf/Disengage.cpp:104` (point and hold: his combat area is the retreat point, the engine paths him there at its run speed). So the first cut of this note was wrong to ask for a new facet for "hold this spot", and to quote v2's "needs a combat area centred on a point" as open: v39 is that. Harbinger does not test the point. Choosing a reachable one is the client's job, which is exactly what `FindPath` on the active navmesh is for.

| Channel | Carries today | Serves which spots |
|---|---|---|
| ch.24 CombatApproach, on a ref (radius, hold, standoff) | walk toward a pinned foe to regain sight (`OffenseApproach.cpp`), close on a heal recipient (`CombatApproach.cpp`) | target-relative firing spots, heal reach, reach the unreachable |
| ch.24 CombatApproach, on a POINT (`kApproach_Point`, with or without `kApproach_Hold`) | give ground, the disengage run | **every spot kind**: firing spot, cover, high ground, chokepoint, flank, peel, kite, retreat. Point plus hold is the spot claim |
| ch.19 travel (to a ref or cell, arrival radius 50 to 512) | loot and errand legs, out of combat | loot route. Ends on the actor entering combat, so not a combat mover |
| ch.23 pursuit leash (anchor, radius) | keeps him near the player | regroup, and the leash as a constraint. Per the ABI note a leash still wins over a ch.24 point |
| ch.21 EngageOnSight (entry foe) | the entry | unchanged |
| ch.25 CastGuard (`kIntent_CastGuard`: the line-of-fire and area guard on his own casts) | refuses or holds a cast through a friend | not a mover. It is the engine-ray authority for line of fire that the pass reads |
| ch.31 CombatMove (`kIntent_CombatMove`, ABI v38: one melee move. The held guard and the power attack ride it) | block, power attack | not a mover |

What the point claim does NOT express, and so what a Harbinger brief (H1) is still for. It is smaller than first written:

- **Stand still.** `kApproach_Hold` keeps him inside R "moving tactically inside it". A firing spot behind a pillar is lost if he circles out of its two metres. Needed: a hold that pins him, or a radius the engine honours down to a body width. Which of the two the engine already does at a small R is NOT known: it is the first thing to observe.
- **Facing.** Nothing says "face this ref" or "face this heading" while he holds. A cover spot is only cover if he looks out of it the right way.
- **Gait.** The point claim runs. A sneak approach, a walk, or a sprint is not a parameter.
- **Strafe**, and any motion defined relative to a foe's facing.
- **Corridor choice.** The engine picks its own path to the point. A route the pass refused (exposure along it) can only be forced as a sequence of point claims on `FindPath`'s corners. Whether the engine then follows corner to corner without stalling is not observed.
- **Field status.** The point road is shipped and field-run for the disengage and give-ground cases only. Its behaviour as a general mover is UNPROVEN: melee classes holding a point while foes are in reach, a point on another floor, a point behind a door, a point claim renewed every 500 ms window. Each is a probe before the design leans on it (principle 5: disassembly and an ABI comment prove the path exists, not that it runs).

marth's ruling (11, item 9): the spot scoring and the Harbinger side are built TOGETHER. So H1 is not "after". It is the same batch as the native movement work: observe the point road as a general mover, then add to Harbinger only what the observation says is missing from the list above. The lab models both (`--position-road point|pinned`: `point` is today's ABI as documented, `pinned` adds stand-still and facing), and the difference per class in J and in the pathing metrics is the measured value of each H1 item.

### 8.4 The oracle plans over positions too

The oracle's options gain the spots: every (action, spot) the generator offers, plus spots of every kind its class may use that the generator did NOT pair (so "IAUS never offered the spot the oracle used" is a (c) divergence, counted like a missing family). Class-bound as always: `ClassRefuses` reads the same `kSpotKinds` gate, and a spot that needs stand-still or facing is refused under `--position-road point` with its own refusal token. The existing `ext:clear_line`, `ext:get_sight`, `ext:step_back`, `ext:sprint_back`, `ext:to_player`, `ext:to_healer` are all special cases of a spot and are retired into it, each only after the spot version reproduces its numbers.

Cost: options per decision grow from about 6 to about 6 times the spots per action. The race drops most after 8 streams. ESTIMATE: 2 to 3 times an oracle fight, on top of 2.7. This is the largest cost in the whole design and the reason 6.2's cuts come first.

### 8.5 Proof in the sim

Each behaviour is a named environment scenario with a pass rule, built on a synthetic fixture (so it runs with no game data and sits in the CLI checks) and replayed on a dumped field environment when one exists.

| Scenario | The field case | Fixture | Pass |
|---|---|---|---|
| `lost-sight-firing-spot` | an archer or a mage whose target steps behind a wall stands and waits (ch.24 walks him only after 2 s occluded, `[offense-approach] ... occluded for 1997 ms`) | L corridor | he moves to a firing spot within `t.spotCommitSec`, `holdUnreach` near 0, his fired share of chosen casts at the level of the open room |
| `ridge-self-occlusion` | Adelinda took a spot from which a ridge hid the foe from herself | exterior ridge with LAND heights | the spot behind the crest is never chosen while a spot with sight is in budget. With `--v4 los` terrain off the lab REPRODUCES the bad pick (proof the scenario tests the mechanism) |
| `tower-stairs` | a foe on a tower top with stairs | stair tower | melee reaches the top by the real path. An archer picks a firing spot instead of climbing when one exists. Nobody stands at the foot looking up |
| `above-through-wall` | Cicero attacking one above him through a wall | two floors, no stair in reach | `foeReachable` false: he takes another foe, or a spot that reaches him, or holds. `holdUnreach` inside its bound. Never a pin on the unreachable one |
| `doorway-block` | melee blocked in a doorway | two rooms, one door | one body in the door at a time, the second takes another route or a spot covering the door. `reblock/f` and `osc/min` inside bounds |
| `healer-round-corner` | a healer out of reach of a recipient round a corner (the heal claim's occluded bound releases it, `HealOccludedBound`) | U corridor | he walks to a heal-reach spot. Allies down while he holds a heal inside its band |
| `dead-end-retreat` | a retreat into a dead end | a room with one exit, foes at the exit | the dead end is not chosen while an exit route exists. When none exists he does not retreat (the cornered consideration), and the retreat family's score says why |

The gates: the six odd-pathing metrics of 2.5, per class and per environment class, as pass or fail against their oracle-relative bounds. A tune that raises J and fails a pathing gate is rejected, like a class-card failure today.

Tune stages: `spots`, one per class after `follow`, searching the `t.spot*` scalars (shared) and the per-class spot weights, scored on the gap as in 6.3, fenced by J, the cards and the pathing gates.

**Where the sim's line of sight comes from, and its error.** From the three layers of 2.4. The extractor has plugin data only: navmesh, placed references and their OBND boxes. It has no collision. Indoors a room is often one or two large statics whose box is the whole room, so boxes are useless as walls there, and the navmesh boundary is the wall. That layer is wrong where sight and walkability differ: windows, railings, balconies, tables, low walls (it blocks sight it should not), and thin pillars or statics on walkable floor (it lets sight through unless the box layer catches them). Outdoors the terrain is the main blocker and needs LAND heights.

Is mesh collision from the BSAs required? **Not decided by argument. Decided by a number.** The error measure: geometry verdict against the engine's own `[los]` verdict at the same positions. No existing `_evidence` log can give it, because a `[los]` line prints the two actors and the verdict and no position (`[los] 0x750012C6 -> 0x00000014: OCCLUDED (own ray, all 3 rays blocked, end margin 48u)`). So today the sim's LoS error against field lines is **unknown, and stated as unknown**. It becomes known with 8.6: every dump carries the engine's verdicts with positions, and replaying them through `CastRay` gives a confusion matrix per environment class. Rule: if the geometry layer disagrees with the engine on more than 5 percent of rays in any environment class (PLACEHOLDER threshold, marth's to set, 11), collision extraction is built for that class. How it would be extracted: `REFS.modelOff` names the NIF. A reader for BSA archives and loose files, then the NIF's collision tree (`bhkCompressedMeshShape`, `bhkMoppBvTreeShape`, convex and box shapes), flattened to triangles in ref space and placed by the ref's transform, written as a new optional `COLL` section. That is a real slice (ESTIMATE 2,500 to 4,000 lines with its tests, Havok packed formats are the risk) and it should not be started on a guess.

### 8.6 The active navmesh: a dump for replay, and a parity check

Two consequences of "it reads the active navmesh".

**(a) A dev-build dump.** In `MFO_DEV_SELFTEST` builds only (personal builds, never a release, standing rule), a passive, rate-limited, config-gated writer (no hotkey: probes are fully passive). On a trigger from the log side (a leg ends BLOCKED or STUCK, an approach ends ENGINE DROPPED, a follower goes down, a spot's geometry verdict disagrees with the engine, or every N minutes in combat) it posts to the main thread and writes the `EnvSnapshot` of the cells around the party to `Data/SKSE/Plugins/MFO_EnvDumps/<time>.mfoenv`, in **the same `MFOENV` format the sim loads**, with a `LIVE` section: every party member and foe in the area (position, facing, base id, health), the legs in flight with their destinations, door states, and the last `Sightline` verdicts with both positions. Size bound per file and per session, counted and logged. A field case then replays in the sim exactly where it happened: `sim --env <dump> --replay-live` places the bodies from `LIVE`.

**(b) A parity check.** `tools/envextract`-side script (or `tools/iaus_sim/env_parity.py`), dump against the extracted cell of the same id: triangles matched by vertex positions, counts, flags, neighbour agreement, door rows and their state, edge kinds, and the share of walkable area present in one and not the other. Output: one line per cell and a summary per environment class, "how far the sim's world is from the live one". What it is expected to find, each a decision input: cells the list's plugins change that the extractor resolved differently, navmesh the engine cuts at runtime (obstacles, disabled triangles), doors whose state differs from the plugin (quests, locks), and initially disabled references a script enabled. The parity number per environment class goes beside every result measured on that class.

Both are small. The dump is the writer half of `envfile.h` plus a main-thread walk, and it is also the probe that verifies 8.2's offsets by showing the same triangles the extractor decoded. It is tier A because it reads engine navmesh structures: Fable authors, Opus reviews, on its own brief, with the layout verification first.

### 8.7 Cadence: a slow, shared decision, and who stands where

marth: "Remember to keep the tick rate slow, no need for any action to be faster than 500 ms for this type of choice". And: "I wouldnt take turns, all followers will share teh available info, so this AI facet doesnt need turns".

**One pass for the whole party, not a turn per follower.** The action facets keep their scheduling (the round robin of the 133 ms pump). The movement facet does not use it. Once per movement decision window the worker builds ONE shared party picture and every follower's spot is decided from it in the same pass.

- **The window.** `Tuning::moveDecisionSec`, a named tunable (`t.moveDecisionSec`), default 0.5, with a hard floor of 0.5 in the reader: a file value under it is clamped and logged. No follower gets a movement decision more often than that.
- **The shared picture** (`iaus/PartyPicture.h`, pure): positions of the party and of the foes any member has in his snapshot, each foe's target, every candidate spot (one pool for the party, a spot is a triangle and a point, tagged with its kinds), who is committed to which spot, and the geometric facts. **A geometric fact is computed once and reused by every follower**: a spot's sight to a foe is the same fact whoever asks, so it is keyed by (spot, foe), not by follower. Path length is keyed by (triangle, triangle). The picture carries the snapshot stamp and its own time, and a fact carries its age.
- **Two phases inside one window, because rays are on the main thread.** Phase 1 (worker, at the window's start): spots from the navmesh snapshot, paths, and the list of rays worth asking. Phase 2 (main thread): the ray batch, spread over the frames of the window under a per-frame cap. Phase 3 (worker, at the window's end): score and assign with the answers. So a sight fact is at most one window old when it is used, and a decision made at time t was asked at t minus 0.5 s. That latency is part of the model and the sim runs with the same one.
- **Commit.** A follower with a spot keeps it until he arrives or a REAL event invalidates it: his target died, his path ended blocked, sight from the spot was lost (a ray answer, not a guess), he was hit hard (the `burst` read of 9.1 over a line), the player gave an order, combat ended. An event marks him for the next pass. It never runs a pass early: the next decision is still at the window's end, never under the floor. A follower who is committed and unmarked is not re-scored. He enters the pass as a fixed assignment the others plan around.
- **Executing the move** stays Harbinger's and the engine's job at their own rate. The pass only files, keeps or releases claims.

**Assignment: they decide together, so who takes which spot is one rule.** Deterministic and independent of any order the followers are stored or iterated in:

1. Committed followers hold their spots.
2. Score every remaining (follower, action, spot) triple with the considerations of 8.1.
3. Take the highest score in the whole table. Ties break by the follower's FormID, then by the spot's triangle index. That follower gets that spot.
4. Mark what the choice costs the others, through the same considerations, not through special cases: spots inside his spacing radius, the SAME doorway or portal edge (one body per portal, the doorway rule of 2.3), spots whose lane to their target crosses his lane or his body (line of fire), and a bonus for a spot that sees the foes threatening him (cover each other). Re-score only the triples those marks touch.
5. Repeat from 3 until everyone has a spot. "Stay here" always exists, so the rule always ends.

A globally highest-first rule has no first mover: swapping two followers' places in memory cannot change the result. Two followers never take one spot because a taken spot leaves the pool. With a party of ten and a pool under 64 spots the table is at most a few thousand triples, so an exact assignment (the Hungarian method on the same scores) is affordable later if the greedy rule's regret against the oracle is measurable. The lab reports that regret.

**Budgets, for the pass as a whole** (ESTIMATES, each measured by its slice, each a named constant in ONE table `kMoveBudget`):

| | Per decision window | Per second (two windows) | Where |
|---|---|---|---|
| Candidate spots in the party pool | 64 | | worker |
| Path searches (after the triangle-pair cache) | 80 for a party of ten, 0.1 to 0.4 ms each | 16 to 64 ms of worker time | worker |
| Ray requests | at most 16 a frame, so at most 16 x the frames the window holds: 192 at 24 frames a second (12 frames), 480 at 60. The pass plans for the FLOOR, 192. A request the window could not serve is carried to the next window and its fact stays marked unanswered. It is never dropped and never guessed | 384 at the floor | main thread, about 5 to 20 microseconds each (ESTIMATE for an engine pick, to be measured by a probe before the budget is trusted): under 0.3 ms in any frame, 2 to 8 ms a second |
| Triples scored | up to 10 followers x 3 actions (his held action and the next two of his ranking, 8.1) x the 64 pooled spots his kinds allow = at most 1,920, typically a quarter of that | | worker, tens of microseconds each: 10 to 40 ms |

The rays are the tight budget. Worst case without sharing would be 64 spots x 5 foes x 3 rays = 960 a window, five times the floor. What makes 192 enough: a fact is keyed by (spot, foe) and shared by the party, a fact answered in the last window is reused while neither end moved a triangle, the snapshot's boundary walls remove the rays whose answer is certain, and spots are asked nearest and best-scored first, so what is left unasked at the window's end is the far tail. A spot whose sight was not answered is scored as UNKNOWN (the catalogue's existing not-applicable input, `In::Off()`), never as clear.

So a full party costs the worker about 30 to 100 ms of every 500 ms window and the main thread well under a third of a millisecond in any frame. When a budget is hit the pass scores fewer spots, nearest first, counts it and logs it once per fight. It never uses a stale fact as fresh.

**How it fits the scheduler work (ClickUp 1dfcuabd439, a 250 to 500 ms decision window).** As the coordinator describes that task, the scheduler gets a decision window of that size. The movement pass is one consumer of it at its slow end: `moveDecisionSec` is a whole multiple of the scheduler's window, the pass is one job in the window (not one per follower), and its commit time is at least two windows. The task itself was not read for this note, so the fit is stated from the description and must be checked when either is built.

**The sim runs the same cadence.** One movement pass per `moveDecisionSec` for the whole party, the same two phases with the same one-window ray latency, the same budgets and the same assignment code (it is `native/iaus`, compiled in). The oracle is asked for a position at the same points and no oftener. A lab that let him re-decide every 133 ms would tune a creature the game does not have.

### 8.8 Traversal abilities: what he can do beyond walking

marth: "I have followers can parkour(or simlar name) installed on both lists. If followers can hadenle this movement, sentient AI would know. We really want to sell the sentience, so our eye for detail in these types of things is important".

IAUS knows, per actor, what he can do beyond walking (drop down, jump, vault, climb, slide, ladder doors), plans with it, and plans without it where it is absent.

**What is known, and how sure.** From the coordinator's read of the Deck install (Follower Parkour Plus 2.8: `FollowerParkour.dll` and its ini, on SkyParkour V. It ships under the GPL: READ ONLY, no code is ever copied). Everything below is from the ini and the DLL's strings, so the BEHAVIOUR IS NOT VERIFIED: a scanner over followers, and over every NPC when `enableForAllNPCs` is true (it is on that list, so foes too). A vault is not limited by the climb slots. A CLIMB happens only when the PLAYER is at least `climbHeightDiff` 100 units above the actor and within `climbMaxDistance` 2000, with at most one friendly and one hostile climber at once. The trigger is a "stuck gate": his speed and velocity while he pushes at a ledge. A slide tackle at 25 percent on humanoid foes. A landing roll. `kNoSim` set during a move, with a 4 s watchdog. Graph variables `SkyParkourLedge`, `SkyParkourOngoing`, `SkyParkourSliding`, `SkyParkourLowerBody`. Graph events `SkyParkour_Start`, `SkyParkour_Stop`, `SkyParkour_Interrupt`. No exported API was found. LoreRim's list has the player's SkyParkour and no follower parkour mod was found there, while marth believes both lists have it. So: **capability is DETECTED at run time, per actor, never assumed from a list.**

**(1) One capability reader.** `iaus/Traversal.h`, pure: `struct Abilities { bool drop, vault, climb, slide, ladderDoor; float maxUp, maxDown, maxGap; float moveSec; Conditions climbWhen; }` and ONE function every consumer calls, `Traversal::Of(actor facts) -> Abilities`. Its inputs are generic sources, one row each in ONE table `kTraversalSources`:

| Source | What it proves | Mod-specific? |
|---|---|---|
| the navmesh's own drop links (kind 2) | a way down exists in the data. Not that followers take it | no |
| a graph variable is PRESENT on his behaviour graph | he runs a graph that knows such moves | the variable names come from an adapter row |
| an event OBSERVED on this actor: a start, then a stop, with his height before and after and the time between | what he really did: this is the only source that sets `maxUp`, `maxDown` and `moveSec`, and it teaches per actor (the reach book's pattern: learned, session-lived, seeded conservatively) | the event names come from an adapter row |
| a failed attempt observed (a start, then an interrupt, or the watchdog) | lowers the learned bound | adapter row |

An adapter row is data: `{ name, graph variables, start / stop / interrupt events, declared conditions }`. One row for SkyParkour-family graphs. No code path names a mod. With no source present, `Abilities` is all false and everything below is exactly today's behaviour. To support another traversal mod, add a row.

**(2) Path costs, spots and foe reach use it.** `FindPath` takes the body's `Abilities`. A drop, a vault or a climb is an edge ONLY while its conditions hold at the moment of the search, and the conditions are data too: for a climb, the declared ones (the player at least 100 above and within 2000, and the single friendly slot free, which the shared picture of 8.7 knows because it knows who else is committed to a climb). When they do not hold, the edge is absent and the route goes by the stairs. An ability edge costs its learned `moveSec` plus an uncertainty price that falls as observations accumulate. Spots: high ground and the retreat kinds gain spots reached only through an ability edge, scored by the same considerations. Foes: with the all-NPC setting a foe has abilities too. `Traversal::Of` is called for foes, so "he is safe on that ledge" is false when the foe in front of him has been seen to climb, and `foeReachable` and the retreat spot's cornered read use the foe's abilities, not the follower's.

**(3) MFO and Harbinger must not fight it.** A follower pressing into a ledge to start a move, or inside one, looks to today's code exactly like the failures it hunts. Each reader gets the one fact `Traversal::InMove(actor)` (the ongoing variable or an observed start with no stop, bounded by the watchdog) and `Traversal::AtLedge(actor)` (the ledge variable, or pressing at a known ability edge), and must hold off while either is true. The audit list, each a native file to check before the native slice is briefed, none changed here:

| Reader that would misread it | What it must not do mid move |
|---|---|
| Movement Blocked handling of ch.19 legs (`logistics/LootTravel`, the BLOCKED and DONE-WHILE-WALKING ends) | end the leg, record a GATED fact, repath |
| stuck-at-origin and the stuck rescue (`LootNeed.h gateRepeatU`, `Service.cpp`) | count the press as stuck evidence |
| `apmf/OffenseApproach.cpp` ENGINE DROPPED and its backoff and drop cap | read the pause as a failed path and block the foe for the fight |
| the chase cap and `foeReachable` | drop or re-pick the foe he is climbing toward |
| cast claims (`cast/CastOn.cpp`, the never-started release of section 5, `StrikeReady`) | claim a hand, or release a claim as never started, during the move. A `kNoSim` window is not a cast failure |
| the act.attack pin and target switches | re-pin or re-target mid move |
| Disengage, hide and back-off runs, the leash | start a run or pull him back mid move |
| the movement pass itself (8.7) | treat "no progress" as an invalidating event. A move in progress is a commitment with the watchdog as its bound |
| `[stall-probe]` and the pathing metrics of 2.5 | count the move as stuck time. The lab's `stuck%` excludes it and reports traversal time on its own line |

**(4) Can he be ordered to do it?** There is no API in what was read. The trigger is the mod's own stuck gate: he pushes at the ledge. So a deliberate traversal is expressed with what exists: the route's corner is the foot of the ledge, squarely facing it, and the leg holds him there (the ch.24 point claim with a small radius, and a facing once H1 adds one), and the mod does the rest. That is a request, not a guarantee, so the move's outcome is observed (source row 3) and a failure falls back to the stairs route with the edge priced up. A proper order would be a Harbinger traversal facet with the mod's cooperation or its own animation road: its own brief, not designed here. And since the mod is GPL, MFO stays MIT by never linking to it and never copying from it. It reads graph variables and events, which any plugin may observe.

**(5) The sim.** `--v4 traversal=off|drops|parkour` and a per-actor ability set with the declared conditions as data (`--traversal-file`, defaults from the adapter row, marked UNVERIFIED until the probe's numbers replace them). Scenarios, each run with and without the ability, the pass rule being that BOTH are sensible: `ledge-shortcut` (a foe above, the player above: with climb he climbs when the slot is free, without it he takes the stairs, and nobody stands at the wall), `ledge-slot` (two followers, one slot: one climbs, the assignment of 8.7 sends the other round), `ledge-player-below` (the climb condition fails: stairs, no pressing at the wall), `foe-climbs` (a mage's safe ledge is not safe from a climbing foe), `drop-retreat` (a drop as the exit of a dead end, and no drop when the ability is off). The oracle is class-bound and actor-bound: it may use an ability only when `Traversal::Of` grants it.

**(6) The passive field probe**, so the numbers are measured and not read off an ini. Config-gated and rate limited, no hotkey, dev and release builds alike since it only observes. Per traversal event on a follower, or on a foe within the party's fight: actor, the event, position and height at the start and at the stop, duration, the player's position and height, the triangle and link kind under him at both ends when the navmesh snapshot exists, who else was in a move, what MFO had in flight for him (leg, claim, pin, run), and whether any of the readers of (3) fired during it. And the same line for a plain drop link taken with no graph event, which is the check the extractor's review asked for: **whether followers use the navmesh's own drop links in the game has not been observed, and nothing in the movement design relies on them until this line has shown one.** `calib_traversal.py` turns those lines into the ability file the lab loads.

### 8.9 Trust: the game has to know when the world is weird

marth: "The in game side will need to know if it actually runs into wierdness."

The sim's library skips anomalous areas (1.4). The game cannot skip the place it is standing in. A list with five hundred mods has navmesh that floats, links that point at nothing and doors that are not there, and a movement facet that believes such a mesh walks followers into walls with full confidence. So the active navmesh and what followers live through on it are both read for trouble, by ONE reader, `iaus/EnvTrust.h` (pure): `Trust::Of(region) -> { level, reason, since, evidence }`. A region is a navmesh, or a connected patch of one around a point (a grid cell of the snapshot, about 512 units), so one bad doorway does not condemn a dungeon.

**Static signs**, checked on the main thread when a cell's snapshot is built, each a row of ONE table `kTrustSigns` with its threshold:

| Sign | Read from | Says |
|---|---|---|
| a mesh far from the ground around it | a few vertical engine rays down from sampled triangle centres (budgeted, at build time, not per window) | floating or buried navmesh |
| a link to a missing or stale triangle | the edge portal's mesh id and triangle index against what is loaded | a plugin conflict left a dangling link |
| one-way neighbours | triangle A names B and B does not name A (the extractor's validator reports the same on vanilla at 0.005 percent, so the threshold is a rate, not a count) | an edited mesh, or a deliberate drop without its link |
| an island under the party | the flood fill `LootReach.cpp` already does | no walkable way off it |
| a triangle the rays say is inside or above geometry | a ray along the triangle's edge at chest height is blocked at once, or the ray down finds nothing | mesh under a static a mod added, or over a hole |
| a door portal without a door | the door portal's ref is null, disabled or deleted | a removed door whose portal stayed |

**Lived signs**, per place, from what a follower or the player actually experienced. They need no new engine read. They are the end states this project already logs:

| Sign | Source |
|---|---|
| the plan said walkable and he made no ground | Movement Blocked with a clear path and no door or actor in the lane, a leg that ends STUCK |
| the engine refused a path we found | `kApproachState_EngineDropped`, a ch.19 leg the engine ends short |
| the engine's path is far from ours | the leg's travelled length against `FindPath`'s (8.2), past a ratio |
| he was moved without walking | a position jump between two samples larger than his speed allows (a teleport, a warp, a script) |
| a traversal we did not plan | a parkour, ladder or drop move (8.8) on a leg whose route had none |
| falling | a height loss with no link under it |

**The result.** A trust level per region (trusted, doubted, distrusted) with its reason and its evidence, **shared by the whole party** (it lives in the party picture of 8.7), learned for the session and held in no save. It is never a silent permanent block. That is the 11-17 GATED lesson, where one false mark froze all loot (`LootNeed.h`: "a gate is a fact, not a freeze"). So every mark decays, and new facts clear it at once: the cell is re-attached, the player walks through the region (the player's own trail is the strongest proof a place is walkable), a follower crosses it, a door event changes the snapshot stamp. A lived sign needs a repeat, or two different signs, before a region is distrusted. One blocked leg is only a blocked leg.

**What low trust does.** Three steps, each weaker than the last and each chosen by the considerations, not by special cases. `spotRegionTrust` and `pathRegionTrust` are facts like any other, with their own curve in the one catalogue:

1. Spots and routes that do not depend on the region score better. A doubted region costs. A distrusted one is priced so that only "stay here" is worse.
2. Where the region cannot be avoided, the pass prefers what has been PROVEN: the engine's own pathing to a foe (the ref form of ch.24, no point of ours) and the player's recent trail (points the player stood on in this cell, which the picture keeps) over spots of its own making.
3. If nothing proven serves, the movement facet hands that follower back to the engine for that region: no spot claim, today's behaviour. That is the documented degrade of 8.2, per region instead of per runtime.

**Never a mask.** The first time a region's level changes, one line: `[env-trust] <cell> region <id> <level>: <sign> (<evidence: the triangle, the leg, the two lengths, the positions>)`, once per region and level, rate limited, and the same record goes into the dev dump (8.6), so the case is diagnosable from a field log and replayable in the lab. The log says what was weird. It never just goes quiet.

**To add a sign, add a row to `kTrustSigns`** (name, static or lived, the threshold, the level it argues for, how it decays). One table, one reader, and every consumer (spots, paths, the loot legs' gating, the dump) calls `Trust::Of`.

**In the sim.** The library is clean by construction, so the defects are INJECTED into clean environments, as scenarios: `trust-missing-link` (a link removed after the plan was made), `trust-buried-patch` (a patch of mesh under a wall the rays see and the mesh does not), `trust-ignored-wall` (a wall the mesh walks through), `trust-one-way` (an edge walkable one way only). The lab's "engine" walks the TRUE geometry while IAUS plans on the defective mesh, which is exactly the game's situation. Gates: recovery time (from the first lived sign to a plan that no longer depends on the region) under a bound, no oscillation (`osc/min` of 2.5 inside its band while the mark stands), no permanent block (after the defect is "repaired" mid-scenario and the player walks it, the region is used again within one decay), and the log line printed once. The same four defects are also the test of the 11-17 lesson: a single blocked leg in a clean environment must NOT distrust a region.

### 8.10 Sequence: one tune, after the environment work

marth's ruling (11, item 1): there is NO tune on the plane. The tune waits for the environment work, because every cached result dies with the sim build it was made by (the driver's cache is keyed by the binary's hash), so a tune started now would be thrown away at the first environment commit. And his rule for the work itself: "We want to avoid just running the sim over and over every time something changes". So the sim slices are batched by area on one agent each, each batch regenerates its baselines once and runs the long checks once at its end, and the tune starts once, on the world all the batches left.

What that one tune needs in the sim before it starts: the world (environments, route search and following, sight by geometry, the pathing metrics as gates, cast starts), the choice batch (the `follow` stage so the gap is the score, field-size books and the spell choice stage), and the pre-tune measurements of 9.6 run ON THAT WORLD, once. What it does not need and can follow as a second, smaller tune of the knobs they add: loot in the world, the T3+ mechanics, and the movement decision (native spots, the snapshot, traversal, trust, the Harbinger additions), which is native tier A work with field probes in front of it. Section 10 is the batch list and its order.

## 9. Slice 1 (built on this branch)

Five commits, each one concern. `native/` is not touched.

### 9.1 The heal-timing mirror (`ec6cf706`)

Native `2de581de..ad4578a8` (`fix/heal-timing`) added `Combatant::grossPerSec` and `maxHit`, `Facts::recipientHitsToDown`, and six `Tuning` scalars. The lab never filled the two snapshot fields, so `recipientHitsToDown` stayed +inf and the knobs were flat. A retune would have re-derived heal thresholds the game already corrects.

Mirror, behind the new v3 switch `burst` (`Data.h`):

- `World.h Body::burst`: his gross damage per sampled lap. One sample every `TM::pump` (the game samples every actor on each follower's lap, at least `kLossMinDtSec` 0.10 s apart, and a lap is served every 133 ms). The lap maths is the game's own `Estimate::LapGross`: a lap with a hit event did at least what the attackers' estimates say, a lap with none is its net drop.
- `NoteHit` is the hit sink: a foe's weapon blow, spell impact or bash on a follower or the player. A hit the party caused is not an event (native review F1). The floor is the attacker's own hit estimate after the target's armor or resistance estimate, as `Fills.cpp:530-539` computes it.
- `grossPerSec` and `maxHit` are read through `Estimate::BurstReads` over the live tuning's windows ONCE PER SAMPLE (`World.h SampleBursts`), as the game reads them on the lap that samples (`Fills.cpp:560-563`), and `Policy.h FillEstimate` copies them into the snapshot. The first cut read them per snapshot, which cost an oracle fight 60 percent more (`BurstReads` is quadratic in the samples and a rollout builds a snapshot per lap). The follow-up commit moved the read: the mirror now costs about 10 percent on an IAUS run and 2 percent on an oracle fight (measured, one core, healer).
- `Tune.h`: the six scalars by the game's own names, so a found value ships. The driver's `GLOBALS` searches `t.dyingAllyHp`, `t.dyingHitsToDown`, `t.urgencyHitsDiv`. The three windows are sized from the game's lap cadence and are not searched: the lab samples every 133 ms whatever the party size, the game every 133 ms times the party, so a window found in the lab would be wrong for a party of ten (principle 9).

No other `native/iaus` Fact was added in `a46d1893..main`. Not mirrored: the five 11-17f cast-layer scalars (5.2) and the twelve loot rows (3.1).

Proof the knobs are live (healer and evoker, seed 1, on the first commit): baseline 75.18, `t.dyingHitsToDown=3` 75.49, `t.urgencyHitsDiv=8` 75.12, `t.grossWindowSec=1` 74.91. With `--v3 burst=off`: 73.62 with and without the knobs, which is the binary before the change. On the branch head (after the hit table changed the world): 78.71, with both knobs moved 78.78, and 78.67 with the switch off whatever the knobs say. A CLI check holds that.

Baseline: 4,099 of 17,269 metrics move. U +0.18 mean over the 90 class and bracket rows (-2.24 to +5.93), +0.09 over the 15 natural rows. Evoker +1.14, mage-off +1.09. Thief and ranged 0.00 (no healer in their fights changes a pick). The v2 baseline: 0 changed. The follow-up (reads at the sample instead of at the snapshot, so a read is up to 133 ms older) moves 1,415 metrics by small amounts: U 0.00 mean over the 90 rows (-1.66 to +1.78).

A modelling limit to know: the lab's attacker estimates are much closer to the real hit than the game's (the game's read 2 to 6 times under, 9.2), so the lab's floor binds more often than the game's. The burst reads agree in shape, not to the point.

### 9.2 F-A: the hit sizes, per event (`7141eeca`, revised in the review round)

The review's finding was right, and the tool now shows it: on `deck-run-1117f` alone `calib_damage.py` prints "FIELD HIT WINDOWS n=16 ... median 4.7% p75 9.7% p90 30.1% max 96.7%", the exact figures of v3 15.5. The sixteen "hits" were sixteen windows.

What a hit line is. "took" is the drop between the worker's two health samples around the event, 0.12 to 0.55 s apart. The engine raises the event twice for most blows, and once more per rider of the blow (an enchantment, a poison, a perk spell: another source form at the same instant). Lines are rate limited: the lines' own "lines skipped" counts add up to 223 dropped lines, and 21 windows report more events on the target than the log holds lines for. An event can land one sample late.

The derivation (`calib_damage.py`, section HITS, reproducible, a CLI check compares its output with the two constants):

- Five sessions carry the lines: 1117c (1117d and 1117e are the same session pulled again, counted once), 1117f, 1117g, 1118-adelinda, 1119-jesper. Not one, as v3 had.
- **139 windows with a loss**, each classified by what it holds (`classify_window`), and each kind kept as its own figure:

| Kind | Windows | Share | Loss, share of the target's health: median, p90, max |
|---|---|---|---|
| plain: one plain weapon blow of one foe | 27 | 19.4 % | 7.5 %, 33.0 %, 100 % |
| rider: a weapon blow with an enchantment, poison or perk spell on it | 35 | 25.2 % | 8.6 %, 32.4 %, 96.7 % |
| power attack | 15 | 10.8 % | 14.8 %, 34.5 %, 39.3 % |
| foe spell | 14 | 10.1 % | 8.6 %, 71.0 %, 95.0 % |
| unarmed (a creature's own attack) | 8 | 5.8 % | 0.3 %, 29.7 %, 29.7 % |
| unsampled attacker (no estimate on its lines) | 37 | 26.6 % | 7.1 %, 46.3 %, 100 % |
| bash, blocked, late (under a quarter of the estimate) | 1 each | | |
| several attackers, or blows at several instants | **0** | 0 % | |

- **No window holds blows of several attackers or instants.** So nothing here is a multi-blow burst, and neither the table nor the lab models one. (The first cut of this note said the field's bursts were partly "several blows in one window". They are not, in these logs.) All 27 plain windows are also free of unlogged events by the lines' own counts.
- The spread is drawn from the **27 plain blows**, each as the health it took over the ATTACKER'S OWN printed estimate after armor (the line's hit x (1 - armor share)). Every plain blow counts whatever its attacker: no per-kind median and no minimum per kind, which is what cut the first version of this table to ten blows. The ratios run 0.77 to 4.62 with a mean of 2.202, which is how far the game's estimate reads under a real plain blow. Divided by that mean: `World.h kFieldHit`, 27 values from 0.35 to 2.10, median 0.91.

**Do the logs support per-event sizes? Yes for a plain blow, on 27 of them.** Part of the spread is the estimate's own error from one attacker to the next, which cannot be separated from the blow's variation at this size. The old table's largest value was 3.91.

What is NOT in the table, and matters more than the table: the plain blow is a fifth of what hits a follower. Riders are a quarter of the windows and the lab does not model them. Foe spells are a tenth and carry the worst tail (p90 71 percent of the target's health). The lab casts foe spells with its own damage model and nothing checks that tail. Power attacks the lab models itself. Each kind is printed by the tool on its own line for the day it is modelled. That gap is item 5 of 4.3.

`kOneHitLine` 0.30 to 0.35: the 90th percentile of the 139 windows is 34.5 percent of the target's health. The plain blows alone give 33.0 percent, so the line does not depend on what a window holds. It is read off the windows because the game's own read, `Estimate::HitsToDown`, is the largest burst inside `Tuning::burstWindowSec`. One assumption inside it: the target's maximum health is the largest health the session's lines show for him.

Baseline (regenerated once in the review round, with the 27-value table): 7,243 of 17,265 metrics move against the ten-value table. U +0.17 mean over the 90 rows (-7.16 to +7.61), +0.53 over the 15 natural rows. Against the old v3 table the world is about 3.5 U kinder over the 90 rows (the old tail had cost -4.56). Every J of the v3 world is a new number again.

### 9.3 F-B, F-C, F-D's uncounted heals (`ee73e70f`)

- F-B: `V3::Any()` counts `hittail`. `--world v2 --v3 hittail=on` is tagged v3.
- F-C, per-subject statistics only: `OraclePolicy::stSubj` holds the subject's decisions and refusals. The per-class `ORCSTOP` and `ORCREFUSED` lines print it. Check: a tree-ON warrior is refused the guard exactly once per decision of his (231 of 231 with every follower planned). The bands measured beside oracle companions (review F7) are unchanged: see 6.2 item 1 and 11.
- F-D: `healAt%`, `healLate%` and `selfHealLow%` are tallied in `World.h StartCast`, where ANY heal of the subject begins: his decision's cast as before, plus a healing scroll (the Scroll family and the oracle's `ext:scroll`) and his own combat AI's heal. The IAUS arm's baseline does not move (0 metrics). The oracle arm's numbers do. F-D's band choice is NOT made (11).

### 9.4 The driver (`037b17f8`)

- `needs_joint_rerun`: `final_curves()` is the one reader of the final table and now leaves out what recalib withheld. The joint stage therefore grades the reduced table, and `ship_state()` clears the flag after a passing joint on it. The withheld list alone used to keep it set. Recalib resets the list when it starts. Review round: when recalib withholds a value after the joint ran, the driver runs the joint stage AGAIN in the same run (`rerun_after`), so the round ends with a verdict on the table it would ship and nobody has to change the command.
- One value per knob (review): the rejudge stage and the classes stage can both accept a value for the same knob (the default rejudge spec `warrior:6.32.b` is a classes knob too). `final_curves()` keeps the classes stage's, which searched the whole range after it.
- The simulator's hash is recorded (`config.bin_sha`) and `carry_over` carries NOTHING from a summary made by another binary. Its cache replays none of it anyway.
- `test_tune_driver`'s `LOOT_KNOBS` failure: the twelve `LootTuning` fields native gained are `LOOT_NOT_SEARCHED` rows with their reasons.
- "15 cells" text: the RULE, the docstrings, the joint failure lines and `summary.md` say "every cell" or the real count.
- `summary.json` config records `cells` and `cell_list`.

### 9.5 `tools/iaus_sim/live/` (`90d75c38`)

The generator, runner and watcher of rounds 1 and 2 lived in `/tmp` and are gone. Rebuilt in the repo, documented in `live/README.md`.

- `run_round.py`: a round runs inside one systemd user scope. Stop is a SIGTERM. Resume is the same command. Proven by `test_live.py` with `SIM_BIN` set: a small round stopped inside the classes stage and resumed gives the same `summary.json` (less cache statistics and the wrapper path) and the same tuning files as an uninterrupted one, with no process left and the cache reused.
- `priority.py`: one level table. marth: "idle priority should be the default. But there should be a drop down to change the current priority". Idle is CPUWeight idle and IOWeight 1. Low 20, normal 100, high 400. Remembered in `<out>/priority`, re-applied on resume, changed live with `systemctl --user set-property --runtime`. Checked on this machine: `cpu.idle` 1 at start, then normal, then idle again on the running unit. No nice, chrt or ionice. To add a level, edit `LEVELS`.
- `gen_class_files.py`: `summary.json` to per-class tuning files. In round 2 the 23 new classes were written empty although the log showed accepted values. The likely cause, unverified because the old tool is gone: a summary seeded from an earlier round whose recalib rows covered 15 classes, read as the value list. Made impossible three ways: values come only from `tune_driver.final_curves()`, a recalib stage that judged another table stops the tool with nothing written, and `progress.log` is checked against the summary. The driver also drops a carried-over joint and recalib result when `--cells` differs (`carry_over`). Each has a test, including the round-2 case itself.
- `watch.py`: read-only status.

Review round, each with a test:

- The generator also refuses a smoke round (`--smoke-ok` is for the tests), a round whose joint stage never ran, and a partial round (a cell of the round's scope never searched), the last two unless `--unshipped`. The log check now compares every knob the log accepted, with its value, against the summary's own row for it. One differing value stops it, not only a class with none.
- `MANIFEST.json` names accepted shared values (worth, loot, globals) that no file carries.
- `run_round.py` holds `<out>/round.lock` (flock) for its whole life. A second runner on the same round exits with code 3 and changes nothing: not the status file, not the unit, not the arguments. That also closes the double run of the no-systemd fallback. `round.json` keeps the ORIGINAL driver arguments and the simulator's hash (`--new-args` and `--new-bin` are recorded under history). A round stopped from outside (`systemctl --user stop <unit>`, a kill of the driver) is recorded as stopped.

### 9.6 Checks, and what was not run

Run on 1874f81e (before the review round): the sim builds, and `SIM_CLITEST=1 run.sh` with `SIM_EVIDENCE` set: CLI-CHECKS PASS, no check failed or skipped. The suite takes about 18 minutes on 8 cores, against under 4 before: three of its checks write a results file, which runs the automatic gap pass with the full oracle on six mage fights, and with the lighter hit table those fights are longer.

Run on the review round's commit, by the coordinator's instruction NOT the full suite again (marth: "We want to avoid just running the sim over and over every time something changes"): one build, the v3 baseline regenerated ONCE and compared with 0 changes, the v2 baseline 0 changed, the baseline byte-identical at 1 and 16 threads, the `calib_damage.py` check against both constants, the heal knobs live and flat with the switch off, `test_tune_driver.py` 36 pass, `live/test_live.py` 25 pass with `SIM_BIN` (the stop and resume proof among them). The only C++ change of the round is the `kFieldHit` table and two comments.

NOT run, each a listed pre-tune step with its command:

1. **The oracle-relative bounds** (`Playstyles.h`: downs, `selfHealLow%`, `healLate%`, `dwh/f`). They were measured on the old hit table, the 30 percent line and no heal mirror, so every one is stale. Cost about 7 CPU hours per class (139 minutes on 16 cores for five classes), past the slice's limit of 3. Command, then rewrite the rows from the `ORCSURV` and `ORCHEAL` lines:
   `sim --seed 1 --fights 1 --rating 0 --class-oracle 160 --gap-scenarios 0 --classes mage,evoker,conjurer,warlock,healer,priest --parked`
2. **The potion-worth table** (`PotionWorth.h`), measured before the hit sizes changed twice: `tools/iaus_sim/potion_worth.py --header`, then rebuild. The driver's prep and recalib stages do this for a round's own tables.
3. **v3 15.6 under pressure** (`--gap-bracket strong2`) on the new world, the record marth's heal-timing ruling needs (11).

## 10. The work, in batches

marth's rulings of 11-23 shape this list. There is one tune, after the environment work. Sim work is batched by area, one agent per batch, with few sim runs: a batch regenerates its baselines ONCE and runs the long CLI suite ONCE, at its end. Spell choice is not "later". The spot scoring and the Harbinger side are built together.

Size = ESTIMATED changed lines including tests and docs. Authors per the model roles: Fable for the sim's structure (the sim is Fable-only) and for tier A, Sonnet for native tier B from a Fable skeleton, Opus reviews every batch. The slice ids are the ones sections 1 to 8 use.

### 10.1 The four sim batches

| Batch | Slices | What | Size | Agent | Needs |
|---|---|---|---|---|---|
| **W, world** | S2, S3, S4, S5, S6 | environment loader and synthetic fixtures. Route search and path following, with `FindPath` written as a PURE file in `native/iaus` from the first commit (no caller in the DLL yet, harness-tested), so it can never fork into a sim copy. Sight and line of fire by geometry. The six pathing metrics, `calib_path.py`, their oracle-relative bounds. Cast starts (rows, never-started releases, `--seats`, interrupts) | 5,000 | one Fable agent. The native `iaus/EnvQuery.h` and `PathSearch.cpp` files are tier B by shape (pure, no engine type, no caller) and get their own Opus pass | `tools/envextract` merged with its rebuilt library (link kinds, components, water and enable parents corrected). LAND heights from the extractor for the ridge case, else exterior sight is navmesh patches and the scenario is marked |
| **C, choice** | S7, S11a, S11b, S11c | the `follow` stage (the gap is the score) and the cost cuts of 6.2. Field-size books (`book_from_log.py`, `--book`, generated worst-case books), the regret report, exact domination pruning. The native `SpellWorth` table and `Estimate::SpellWorthOn`. The `spells` stage | 2,400 | one Fable agent for the driver and the sim. S11b (600 lines, native tier B) is a Sonnet agent from the Fable agent's skeleton, with its Opus review | nothing from W for S7 and S11a. S11c needs S11b. Its sim edits (`Oracle.h`, `Load.h`, `sim.cpp`) rebase onto W before its one baseline regeneration |
| **L, looting** | S8, S9, S10 | loot in the fight world: sources from refs, scan, batch, dibs, legs, the party combat stop, carry. Gates and guards. The loot oracle, episode Utility, the driver's loot stage on episodes with the twelve new knobs, `calib_loot.py` | 3,200 | one Fable agent | W merged (S3). Ownership per ref from the extractor. Container and leveled-list contents from `tools/list_balance` for list worlds |
| **T, T2 and up** | S12, S13, S14, S15 | Arch-Mage independent hands. The T4 passives in one table. Summons as real bodies. Dragonborn | 2,700 | one Fable agent | W merged (S12 needs S6's claims, S14 needs S3's bodies on a mesh). S15 needs a check of the game's shout road first |

### 10.2 The native line, and the sim mirrors that belong to it

| Batch | Slices | What | Size | Agent | Needs |
|---|---|---|---|---|---|
| **N0, probes** | N0 | passive, config-gated, rate-limited field probes, nothing else: traversal events with positions and heights and what MFO had in flight (8.8), a drop link taken with no graph event, the cost of an engine ray, the lived trust signs as log lines (8.9), and the ch.24 POINT road watched as a general mover (8.3: does a small radius pin him, what he does inside it). Plus `calib_traversal.py` | 700 | Sonnet (tier B: observes only, no seat, no engine write) from a Fable spec, Opus review | none. FIRST, because every later native number waits on field sessions |
| **N1, the active navmesh** | N1 | the main-built snapshot of loaded cells (the `LootReach.cpp` walk into plain arrays), the `BSNavmesh` layout rows on three runtimes, the dev-build dump in the sim's format with the `LIVE` section, the parity check, and the STATIC trust signs at snapshot build | 1,600 | Fable (tier A: engine reads, threading), Opus review | `envextract` merged (the format). N0's ray cost |
| **M1, the movement decision** | N2, H1, S16 | native `iaus/PartyPicture`, `Spots`, `EnvTrust`, the shared 500 ms pass and its assignment, spot claims through the ch.24 point road, behind an INI switch that ships OFF. TOGETHER with the Harbinger side (marth's ruling): H1 adds to Harbinger only what N0 showed the point road lacks (stand still, facing, gait, strafe, corridor), as its own brief in its own repo. And the sim's side, S16: `EnvView` over `.mfoenv`, the seven scenarios of 8.5, the four trust scenarios of 8.9, the oracle over spots, the `spots` stage | 2,800 native, 1,800 sim, Harbinger its own | Fable for MFO and the sim (tier A), a Fable Harbinger agent for H1, Opus reviews each. Two build trees, so two agents, never both in one | N1, W merged, N0's field data |
| **M2, traversal** | N3, S17 | native `iaus/Traversal.h`, the adapter table, the hold-offs in the readers of 8.8 item 3. And the sim's side, S17: the traversal switch, the ability file, the five scenarios | 900 native, 600 sim | Fable (tier A: it touches the cast, approach and travel readers), Opus review | M1 (N2), N0's traversal data. S17 needs N3 because the ability reader and its conditions are the shared native code |
| X1, collision from NIFs | X1 | only if N1's parity and ray numbers put the geometry layer's error over the threshold of 11 item 8 | 2,500 to 4,000 | Sonnet (tools) with a Fable format note | N1's numbers |

### 10.3 The order

1. **Now, side by side** (two agents while a Fable one runs): **W** and **N0**. N0 is small and its sessions take calendar time.
2. **Then C**, on W's world. Its driver-only part (S7) can start beside W in its own worktree, since it touches `tune_driver.py` and not the world.
3. **Then the pre-tune measurements of 9.6, ONCE**, on the world W and C left: the oracle-relative bounds, the potion-worth table, the pressure-bracket record. They are not run before, because each belongs to a sim build and dies with it.
4. **THE TUNE**, through `live/run_round.py` at idle priority. Its pathing gates are S5's.
5. **N1** can run beside C or beside the tune (the tune holds no build tree).
6. **L and T**, one after the other, each ending in a small tune of the knobs it adds.
7. **M1, then M2**, each ending in its own stage of a tune (`spots`, then the traversal conditions).

Requests to other owners, not slices of this line: the extractor (LAND heights, ownership per ref, the optional `LIVE` and `COLL` tags reserved), `tools/list_balance` (container and leveled-list contents, the spell fields the archetype classifier reads), `tools/tune_app` (section 12).

## 11. marth's decisions

Ruled on 2026-11-23. Recorded with what follows from each.

1. **Two tunes or one? RULED: no tune now.** The tune waits for the environment work, because cached results die with the sim build. Sim slices are batched by area on one agent with few sim runs. 8.10 and 10.3 are written to that.
2. **F-D: the bands also gate under pressure.** Proceeds on the recommendation: a second band row per caster on the bracket whose downs match the field (strong1 on Tuxborn, strong2 on the built-in world), bound = the oracle's value there plus the same margin. Measured in step 3 of 10.3.
3. **Bands beside IAUS companions instead of oracle companions.** Proceeds: a truer bound for the subject and three times cheaper.
4. **The gap is the score.** Proceeds: S7 is in batch C, before the tune.
5. **The oracle's ceiling stays 128.**
6. **The 23 parked classes' round-2 values do not ship** before S12 and S13 (batch T).
7. **Unseated rows: RULED, "run every spell".** No veto and no candidate filter by row. A spell stays a candidate, bounded by the never-started release, and Harbinger is seating the rows now (`feat/seat-unseated-casters`). The lab models the release and keeps `--seats today|all` as a measurement only.
8. **The line-of-sight error threshold** for collision extraction stays the 5 percent placeholder until N1's first parity and ray numbers exist.
9. **Movement and Harbinger: RULED, together.** Spot scoring and the Harbinger side are one batch (M1). The switch ships off until its tune.
9a. **Drop links and parkour** stay off in any tune until N0 has logged a follower taking one.
10. **The loot value function** keeps the potion-worth exchange rate. Every loot result is reported with and without the gold term.
11. **Field books are test input** like a list world: never in game logic, never committed.
12. **Every field session runs with `bIausFillProbe = 1`.** The hit table is 27 blows because two of the last five sessions' worth of fights were not probed, and the rider and foe-spell tails rest on 35 and 14 windows.

Also ruled the same day and written into the design: environments aim at coverage of every terrain type and design, and anomalous areas are skipped by the sim (1.4) and recognised by the game (8.9). In game the environment is the ACTIVE navmesh, never extracted data (8.2, 8.6). Engine rays are the authority for sight, with a 500 ms floor on the movement decision and no turns between followers (8.7). Traversal abilities are detected per actor (8.8).

## 12. Findings outside this branch's file boundary

Reported, not made.

- **`tools/tune_app/mfo_tune/tuningfiles.py` `T_SCALARS`** (a separate Sonnet change owns the app) does not list `t.dyingSelfHp`, `t.dyingAllyHp`, `t.dyingHitsToDown`, `t.urgencyHitsDiv`, `t.grossWindowSec`, `t.maxHitWindowSec`, `t.burstWindowSec`, `t.waivedRangeX`, `t.bowRange`. Its validator mirrors the in-game loader, and native's `SetTuningScalar` accepts the first seven (`Catalogue.cpp:481`). The driver now searches three of them, so the app would reject a `shared.txt` line the game accepts.
- **The app's priority drop-down** should import `tools/iaus_sim/live/priority.py` (`ORDER`, `DEFAULT`, `get`, `set_level`) and start rounds through `live/run_round.py`. A round started by the app's own launcher has no unit file and cannot be re-prioritised.
- **Native, cast**: the pure part of `cast/Archetype.cpp ClassifyArchetype` behind an engine-free input struct, so the sim predicts the engine row with the game's own code (5.3). `HarbingerSeatsSpell` has no caller and, after marth's ruling ("run every spell"), needs none as a veto: its table (`kUnseatedRows`) shrinks as `feat/seat-unseated-casters` lands.
- **Native, heal timing, the floor over-counts** (found by the Opus review of this branch, checked against the field lines here): the engine raises `TESHitEvent` twice for most blows and once more per rider of the blow (9.2). `Fills.cpp`'s sink pushes one `Taken` per event (`g_taken`, `:216-218`) and the lap adds the attacker's estimate once per `Taken` (`:530-539`), so one plain blow floors at twice the attacker's estimate and an enchanted one at three times or more. The estimate reads about 2.2 times under a real plain blow, so the doubled floor is by accident near the truth for a plain blow and over it for a blocked or glancing one. The lab's `NoteHit` counts a blow once. Fix shape, native: de-duplicate the sink by (attacker, target, instant), or floor with the largest estimate instead of the sum. Not made here.
- **Native, IAUS**: `t.waivedRangeX` and `t.bowRange` are known to the lab and not to the game's tuning file (already recorded as review F3 of 5749026a), so they cannot be searched.
- **Native, heal timing**: the three windows are sized for a party of ten sampling every 1.3 s, while each follower's snapshot samples every ally every lap (0.133 s). The comment in `Core.h` says both. A window per party size is not a tunable the lab can find (9.1).
- **Field probe**: the hit lines would support per-event sizes directly with two more fields that the sink already has in hand: the event's own damage is not available to it, but the health AT the event is (`at-event`, main thread only) and it is NaN on 90 percent of lines. A main-thread read posted for every probed hit would turn windows into events.
- **`_evidence`**: `deck-run-1117c`, `1117d` and `1117e` hold the same session's log. `calib_damage.py` counts it once and says so.

## 13. Batch W as built (branch `feat/sim-v4-world`, 2026-11-23)

Slices S2 to S6 in one batch. The flat world is untouched: `--world v3` (still the default) and `--world v2` compare with 0 metrics changed against their baselines. The new world is `--world v4`, with its own baseline `tools/iaus_sim/baseline_v4_builtin.txt` on the hand-made fixtures.

### 13.1 Where things are

| What | Where | To change it |
|---|---|---|
| The pure geometry the game will share: `View`, `FindPath`, `WalkLine`, `CastRay`, `BuildGrid`, `BuildWalls`, `LabelComponents` | `native/iaus/EnvQuery.h`, `EnvQuery.cpp`, `PathSearch.cpp`. In `native/CMakeLists.txt` so MSVC compiles them. NO caller in the DLL | a link kind: `kLinkKinds`. A path cost: `PathTuning`. A sight rule: `SightTuning`. One table each |
| The loader, the route cache, the pool of places | `tools/iaus_sim/Env.h` | body and following numbers: `EnvTuning`. The v4 switches: `V4` |
| Bodies on the mesh: movement, doors, body circles, leg ends, sight, the six tallies | `tools/iaus_sim/EnvWorld.h` | |
| Placement of a fight in a place | `tools/iaus_sim/Setup.h EnvPlace` | |
| Hand-made fixtures | `tools/iaus_sim/envs/<name>.txt` and the `.mfoenv` each becomes (`make_fixtures.py`, `--check` in the CLI suite) | draw a map, run the script, add the name to `Env.h kFixtureNames` |
| Known-answer checks (81 after the review rounds) | `tools/iaus_sim/EnvTest.h`, `sim --env-test`. The solo walk is `sim --solo-walk N [--env-set all]` | |
| Environment scenarios | `Scenarios.h` `envScen(...)`, `--scenario env` or by name | |
| The pathing gate | `sim.cpp PathReport`, `kPathBounds`, `--path-report N [--path-oracle K] [--path-gate]` | a bound: `kPathBounds` |
| Cast starts | `Data.h SimRow`, `SimRowSeated` (native's own `cast/SeatRows.h RowSeated`), `World.h StartCast`, `WillWorkFor`, `NoteZeroEffect`, `Policy.h` | a row: a value in `SimRow` and its key in `SimRowOf`. A row Harbinger seats: `native/cast/SeatRows.h kRows` (the lab includes that file) |
| Field rates for the gate's numbers | `tools/iaus_sim/calib_path.py <run-dir>` | a log line: one row of `ROWS` |

CLI: `--world v4`, `--v4 env|path|bodies|los|caststart|interrupt|drops|foedrops=on|off`, `--env <file>`, `--env-set <set>|all [--env-class <class>]`, `--env-list`, `--env-test`, `--sight walls=off|terrain=off|gap=<u>`, `--seats today|old`, `--seat-immunity seen|unseen`, `--solo-walk N`, `--path-report`, `--path-oracle`, `--path-gate`. Diagnostics by environment variable: `SIM_PATHTRACE=<job> [SIM_PATHTRACE_ENV=<class>]` traces one fight of a path report, `SIM_ENVBAD=1` prints every off-mesh body.

### 13.2 What differs from the design above, and why

- **A route is asked centroid to centroid.** The cache of 2.7 is keyed by (triangle, triangle, door stamp, walker). A value that depended on the exact start point would depend on which thread asked first. So the cached route runs between the two triangles' centres, the mover swaps the last corner for his own goal, and a straight walk is tried first with the exact points. Runs are the same at 1 and at 16 threads (checked on the baseline).
- **The corridor A* picks need not hold the straight line**, so `FindPath` drops every corner a straight walk makes needless, after the funnel.
- **In-cell doors have no DOOR row.** Measured on the 11-23 library: 165 DOOR rows, all load doors. The 132 plain doors (30 locked) are REFS rows only. The loader joins each to the triangles its leaf crosses (the long side of its box, turned by the ref's rotation). MODEL: a plain door starts closed. This belongs in the extractor.
- **Walls have a height range.** A boundary edge is a wall, the face under a ledge, the foot of a cliff, or nothing (a balcony over more floor), found by probing for the next floor past the edge. A ledge can be seen over. A closed door blocks sight at its leaf.
- **The leash.** The plane's rule (walk back when past the radius, walk out again when inside) steps in and out of the radius three times a second. On the plane nobody could see it. Here it was 14 to 20 reversals a minute for a Knight. In the v4 world he stands within 150 units past the radius (`World.h kLeashStandU`, MODEL: the engine does not pursue past it). v3 is unchanged.
- **No blow through a wall or a closed door** (`EnvMeleeClear`): on the plane reach was distance alone.
- **Water** is a cost (the extractor's README), not closed as 2.2 first said.
- **The environment scenarios measure today's behaviour.** The pass rules of 8.5 need the movement decision (M1). Until then each scenario prints its numbers and three of them are pinned in the CLI suite as what the lab reproduces: the tower is climbed by its ramp, a blade under a floor nothing joins stands pinned the whole fight, and a blade pushes at a wall the mesh does not know.

### 13.3 Not built, or built smaller than the section says

- The gate's bounds are PLACEHOLDERS (`kPathBounds`). `--path-oracle K` measures oracle-relative bounds on the same fights and prints `ORCPATH` lines. It was not run at size: that is the pre-tune measurement of 10.3 item 3.
- `t.sharedRadiusMargin` and `t.preemptSettle` are still not mirrored. `t.occClaimRelease`, `t.offApproachBackoff` and `t.offApproachDropCap` are.
- A plain hit does not interrupt a charge (the assumption of 5.3, off). A stagger, a paralysis and a calm already did. A fear now does, and all are counted (`cs.intr/f`, `cs.foeIntr/f`).
- The engine row of a spell is the lab's reading of `cast/Archetype.cpp kRows` from the fields the extract has (`Data.h SimRowOf`). The shared pure classifier of 5.3 is still a native refactor nobody made.
- `calib_cast_delay.py` did not get its per-row read.
- Trust scenarios: `trust-ignored-wall` and `trust-one-way` only. `trust-buried-patch` and `trust-missing-link` (a link removed mid fight) need a mesh that changes during a fight.
- Terrain is the navmesh's own triangles. LAND heights are not in the file yet, so a hill with no navmesh on its crest does not block.
- Drops: built and tested, OFF (`--v4 drops=on`, `foedrops=on`). Parkour abilities are batch M2.
- The v3 perch (a foe on a ledge by flag) does not exist in a place. A foe is high only where the mesh is.

### 13.4 The review round (Opus review of `37a705e6`, fixed 2026-11-23 and 11-24)

The review's headline: 217 of 11266 solo walks ended over or under their goal with no stuck tally (F1), blasts and areas burned through walls and floors (F2), and the backpedal was 65 percent of the Evoker's stuck time (F3). Every finding is fixed on the branch. The table says what was built and how it is checked.

| Finding | What was built | Check |
|---|---|---|
| F1 the straight-line shortcut had no height test | `EQ::Walk::zMiss` and `At()`. A straight walk counts only when it ends at the goal's height. Arrival and every corner test the height (`EnvTuning::levelU`). `Fight::Move` takes who or what he walks to | `--solo-walk`: fixtures 657 trips, 0 wrong. Library copy of 11-23 (66 places), at the branch head: 10615 walked, 2 wrong (0.019 percent), 0 over or under the goal, 0 off the mesh (3 of 10620 before the re-check round). The stacked fixture in `EnvTest.h MakeStacked` |
| F2 blasts and areas were 2D | `EnvInBlast`: the radius in 3D and a clear ray from the centre | env-test "stacked: a blast on the floor below does not burn through the floor" |
| F3 the backpedal walked into walls | `EnvAwayPick`: nine straight walks over the half circle that leads away, the farthest wins. No gain = CORNERED, counted apart (`away%`, `cornered%`) | the gate table below: stuck% fell for every caster |
| F4 a held-off spell cost nothing | the picker still ranks it and the refusal costs the decision (`cs.heldTurns/f`) | CLI check "a held-off spell costs the turn" |
| F5 seats were the game before ABI 46 | default = every row seated. `--seats old` is the older game. A Script row takes the short window | env-test, CLI check |
| F6 the goal was guessed from who stood near the x, y | `EnvGoal` takes the body, an explicit mesh point, or neither (his own storey) | the stacked checks |
| F7 away legs were not legs | a backpedal and a flight count in the reversals (`osc/min`) | the count then showed the lab's own dither at corners, fixed in 13.9 |
| F8 the stuck tally had holes | see 13.5 | env-test "stuck: ..." (four checks) and "hold: ..." |
| F9 the oracle gate compared different fights | `PathReport` keeps a third tally per cell: IAUS on the fights the oracle also played | CLI check: the `ORCPATH` lines of (N 3, K 1) and (N 1, K 1) are the same 56 lines |
| F10 oracle move points could be off the floor | `EnvStandable` | |
| F11 sight-driven weights could be searched | `tune_driver.py SIGHT_GATED_CONS` and `SIGHT_GATED_SCALARS`, read by `sight_gated()` at the two places a knob enters a search (`all_knobs`, `stage_globals`). `--sight-knobs` lifts it. `summary.json sight_skipped` and a section of `summary.md` say what was kept out | `test_tune_driver.py SightGate` (the names are checked against `iaus/Catalogue.cpp`) |
| F12 the route cache grew without bound | a cap | |
| F13 two libraries compared silently | the pool's hash is in the world tag, so `--compare` refuses | CLI check "a compare across two sets of places is refused" |
| native API items | `EQ::Invalid(view)`, `PathTuning::maxExpand` 20000, `View.walk`, `CastRay` end margin (a look up through a floor read clear), leaf doors gate at the leaf in `WalkLine` and in the search | env-test |

### 13.5 F8: stuck, hold and repeat

Before the round a body was stuck when it moved under `stuckU` in `stuckSec` of clock time, and any gap of three steps in the asking cleared the window. A caster who asked, failed, cast and asked again was never stuck. Four changes, all in `EnvWorld.h EnvMove` and `EnvLegEnd`, all numbers in `EnvTuning`.

- **The window counts wanted seconds.** A gap in the asking is taken out of the window and does not clear it. Only a gap longer than `wantGapSec` (3 s, ASSUMPTION) is standing by choice.
- **A stuck leg ends STUCK again every `stuckSec`.** Each end asks for a route from where he stands. One end per stuck spell had left him on the shared centre-to-centre route for good (a Mage stood 80 s at a door behind a friend on the doorway fixture).
- **Hold time is its own tally.** Waiting out the retry after a BLOCKED or UNREACHABLE end, the offense approach's backoff, and a foe past the drop cap all feed `hold%`. Before, the backoff fed `stuck%` and the same wait past the cap fed nothing. `hold%` is a gate row.
- **Repeats of every kind count.** An UNREACHABLE or STUCK end within `reblockU` of his last end of that kind is `repeat/f`, a gate row. BLOCKED repeats stay `reblock/f`.

The detour sample changed too. It is what he walked over the route he was given, measured from where he stood, plus how far his goal ended from where it began (the net displacement, at most `legGoalU` by the leg's own rule). The cached length ran centre to centre, and a route of 100 units to a foe who then walked 150 read 2.5. A cell with no arrived leg over 200 units (a bow that stands and shoots) prints `no legs`, and the report says in how many cells the detour bound was not tested.

### 13.6 The gate, before and after, by class

`--world v4 --path-report 24` on the library copy of 11-23 (`MFO_ENV_CACHE=~/.cache/mfo-env-batchw`, `--env-set all`, 14 classes of place, 5040 fights). Each number is the mean of the class's 14 cells, `37a705e6` then the branch head after both review rounds. The bounds are still the placeholders of `kPathBounds`.

| Class | stuck% | osc/min | detour p90 | hold% | repeat/f | U | cells failing |
|---|---|---|---|---|---|---|---|
| melee | 0.70 to 0.23 | 1.17 to 0.86 | 1.59 to 1.27 | 0.14 | 0.02 | 79.96 to 80.51 | 2 to 0 |
| ranged | 0.01 to 0.08 | 0.04 to 0.13 | 1.55 to 1.79 | 0.47 | 0.01 | 80.02 to 78.99 | 1 to 2 |
| mage | 1.80 to 0.02 | 2.20 to 1.18 | 2.11 to 1.54 | 0.04 | 0.00 | 70.70 to 70.69 | 4 to 0 |
| knight | 0.05 to 0.06 | 0.61 to 0.61 | 1.50 to 1.27 | 0.10 | 0.00 | 92.62 to 92.76 | 1 to 0 |
| warrior | 0.53 to 0.15 | 0.51 to 0.53 | 1.71 to 1.50 | 0.11 | 0.01 | 80.74 to 81.03 | 1 to 1 |
| mercenary | 0.49 to 0.17 | 1.68 to 1.14 | 1.38 to 1.29 | 0.15 | 0.02 | 88.17 to 89.58 | 0 to 0 |
| archer | 0.02 to 0.01 | 0.01 to 0.23 | 1.29 to 1.25 | 0.02 | 0.00 | 74.88 to 73.32 | 0 to 0 |
| thief | 0.19 to 0.02 | 0.44 to 0.32 | 1.29 to 1.28 | 0.03 | 0.00 | 86.80 to 87.62 | 1 to 0 |
| evoker | 1.67 to 0.16 | 2.71 to 1.27 | 1.62 to 1.46 | 0.73 | 0.03 | 79.44 to 77.51 | 2 to 1 |
| conjurer | 1.01 to 0.09 | 3.90 to 1.47 | 1.62 to 1.43 | 0.14 | 0.00 | 73.22 to 74.00 | 1 to 0 |
| healer | 1.01 to 0.09 | 4.08 to 1.30 | 1.66 to 1.48 | 0.08 | 0.01 | 70.76 to 69.71 | 3 to 0 |
| warlock | 1.16 to 0.02 | 2.99 to 1.57 | 1.77 to 1.41 | 0.05 | 0.00 | 81.87 to 80.01 | 2 to 0 |
| melee-off | 1.00 to 0.21 | 1.22 to 0.77 | 1.68 to 1.27 | 0.10 | 0.03 | 76.55 to 78.19 | 3 to 0 |
| ranged-off | 0.20 to 0.05 | 0.33 to 0.26 | 1.64 to 1.52 | 0.05 | 0.01 | 76.63 to 78.34 | 1 to 1 |
| mage-off | 1.29 to 0.06 | 3.07 to 1.40 | 1.59 to 1.46 | 0.02 | 0.00 | 84.45 to 83.08 | 2 to 0 |

Whole table: 24 failing cells of 210 before (stuck% 15, detour 10, one follower stuck for good), 5 after. No follower is stuck for good, no body left the mesh. On the fixtures (105 cells): 3 failing before, 0 after. The detour mean of a class leaves out its `no legs` cells (28 of 210 on the library, 26 of 105 on the fixtures).

The five that fail, all on two classes of place:
- `exterior_many_doors`, ranged (reblock/f 0.54, unreach% 11.1, holdUnreach 1.23 s, hold% 6.5) and evoker (reblock/f 0.38, holdUnreach 2.38 s, hold% 9.0): a foe behind a door the follower cannot open. It is counted as what it is.
- detour p90 over 3: ranged on `exterior_actors_containers` (5.00), warrior (4.00) and ranged-off (3.25) on `interior_small_multilevel`.

Between the two rounds the table read 34 failing cells, 32 of them `osc/min` at about 12 reversals a minute for every caster. That was the lab's own dither and not a caster's footwork (13.9).

### 13.7 The adopt-seats mirror (MFO main `5b17b58e`, Harbinger ABI 46)

The lab includes native's own pure file `native/cast/SeatRows.h`, as the harness does.

- **Is a row served.** `Data.h SimRowSeated` is `SeatRows::RowSeated` on what Harbinger reports: ABI 46 with the four hostile rows and the equip admission covered (today), or an ABI under 46 (`--seats old`).
- **The keyed will-work** (`cast/WillWork.cpp` (c)). `World.h Fight::WillWorkFor` is what the game's reader answers a follower: the level cap first, then the keyed effect, and a No only when the spell carries no other hostile damage effect. The oracle keeps `WillWork`, the truth.
- **The two-zero-fires hold** (`cast/NoEffect.cpp`). `Fight::NoteZeroEffect` reads every fire of a hostile-row spell through `SeatRows::Judge` and `Tally`. Two zero readings in a row hold the pair for the fight and will-work then answers No. Counted as `cs.zeroFire/f` and `cs.zeroHeld/f`.
- **Paralysis immunity is the open spot.** Native says the engine exposes no bit for it, so will-work says Yes and the hold catches it after two wasted casts. The lab mirrors that by default (`--seat-immunity unseen`). The lab's foes carry the `ImmuneParalysis` keyword (`immunePar`, from `tools/list_balance/compile_world.py`), and whether a list's paralysis effects test that keyword in their conditions is not in the extract. `--seat-immunity seen` is the lab with the conditions present. Measured on the fixtures (`--classes beta1`, 90 class and bracket rows): 4 rows with a zero fire and 1 with a held pair unseen, 2 and 0 seen.
- **Not mirrored**, because the lab has no such effect or record: Disarm's right-hand weapon, Command's summoned target, effect and magic effect conditions as such, the approximate-row exemption. MODEL: every fire is read, at the start of the settle window. In the game a reading needs a refresh lap or a will-work ask inside 2 to 6 s.

### 13.8 Checks and cost of the round

- After the first round: `sim --env-test` 74 checks, CLI-CHECKS PASS (189 s with the library solo walk. The field-log check was skipped, no `_evidence` in the worktree). After the re-check round: 13.9.
- `baseline_v4_builtin.txt` rewritten once in each round. It compares with 0 metrics changed at 1 and at 16 threads. `--world v3` and `--world v2` compare with 0 metrics changed against their baselines.
- Cost, measured on a busy machine (the local MSVC build ran beside it), so read the ratios: `--classes beta1 --seeds 1,2` at 8 threads took 1.1 to 1.5 s in `--world v3`, 2.0 to 2.7 s in v4 on the fixtures (about 1.5 times) and 3.1 to 4.1 s on the library copy (about 2.5 times, the load of 66 places included). The path report's own figure was 2.5 ms of CPU per IAUS fight on the fixtures and 3.5 on the library on a quieter machine. The round did not make a fight dearer: under one load the reviewed build read 5.1 ms and the head 4.7. The oracle's cost per rollout was not measured again.
- Open after the round: `Docs/REVIEW-BACKLOG.md` "feat/sim-v4-world".

### 13.9 The re-check round (Opus re-check of `91946263`, 2026-11-24)

F1 to F13 were confirmed. Two findings were left, and a list of small ones.

**R1 (SEV-2): the casters' 12 reversals a minute were the lab's dither.** The caster and bow lines of `World.h Brain` and `Feet` read sight every step: no sight, walk in. Sight and nearer than 320, back off, which at a corner is back behind the corner. The reviewer counted 71 percent of the reversals inside half a second of the last leg. Native has a dwell on both edges (`apmf/OffenseApproach.cpp kSightDwellMs` 1500) and no step-back of its own. Built:
- `Fight::SightWalk`: the walk-in for sight begins only after the foe has read occluded for the dwell and ends only after he has read visible for it. One opposite sample starts the clock again. Per follower and foe. The one constant is `EnvTuning::sightDwellSec` (1.5 s, native's), read by all four sites (caster and bow, `Brain` and `Feet`).
- `Fight::BackOff`: the step back begins inside its distance with sight, goes on until he is `backOutU` (100, ASSUMPTION) past it, and only a body whose combat style falls back does it, the bow line's own gate.
- `EnvAwayPick` takes the foe he backs off from and prefers a point that still sees him. Any point with sight beats every point without. The pick stands for `awayCommitSec` (2 s) or until he is there. A flight is as before.
- `legMinU` 150 to 200. The held caster's side step is 160 units and is footwork, not a leg.
- On the plane (`--world v3`, v2) all four are the old rule, draw for draw.

Result on the library: `osc/min` of the six caster cells from about 12 to between 1.2 and 1.6, the failing cells from 34 to 5 (13.6).

**R2 (SEV-3): a closed door ended the approach for the fight, for everybody.** Native bounds only the ch.24 approach claim, which a follower walking in for sight holds. It has no ending called "a door", a path it cannot make is a backoff, and a block lapses. Built in `EnvWorld.h`:
- `EnvClaimWalker`: a caster or a bow in his hands. The backoff, the cap and the block apply to him only. A blade's pursuit is never bound: he asks again every `retrySec`, counted as hold time and re-blocks.
- A route that ends at a door he cannot open takes the ENGINE DROPPED road: a backoff, `offenseApproachDropCap` times, then a block.
- `EnvApprBlocked`: a block is scoped to where and when it was made. It lapses when he stands `blockLapseU` (300, native `kBlockMoveUnits`) from that spot or a door has changed since.

**Smaller items.**
- Detour: the goal's net displacement is added to the route, not its whole travel.
- The path report prints `no legs` for a cell with no detour sample and counts those cells.
- `PathSearch.cpp FindPath`: a goal across a shut leaf inside one triangle (start and goal in the leaf's triangle, or the last piece into it) is BLOCKED for a walker who cannot open it, and the path names the door.
- `EnvQuery.cpp Invalid()` refuses a wall or a door leaf that is not finite, and a wall kind out of range.

**Checks.** `sim --env-test`: 81 checks pass (sight dwell, back-off, leaf, snapshot, three door-block checks). `baseline_v4_builtin.txt` rewritten once more. It compares with 0 metrics changed at 1 and at 16 threads. `--world v3` and `--world v2`: 0 metrics changed. Solo walk: fixtures 657 trips and 0 wrong, library copy 10615 walked and 2 wrong, none over or under the goal. `run.sh` with `SIM_CLITEST=1`: CLI-CHECKS PASS (the field-log check skipped again). Local MSVC build of the two changed native files and the link: exit 0.


## 14. Batch L as built (branch `feat/sim-v4-loot`, 2026-11-24)

Slices S8, S9 and S10 in one batch: looting inside the fight world. With looting off nothing moved: `--world v2`, `--world v3` and `--world v4` compare with 0 metrics changed against their baselines, `sim --env-test` passes its 81 checks, and the old 8-stop lab (`--logistics`) passes its own checks. No fight metric of a baseline was rewritten (the review round changed one line of `baseline_v4_builtin.txt`, the pool hash, 14.9). Numbers below are from the runs of 14.8, made once at the end of the review round (14.9 says what that round changed).

### 14.1 Where things are

| What | Where | To change it |
|---|---|---|
| Native's loot rules, restated: the scan, the excursion slot, legs, the player's dibs, the take and its landing hold, the guard answer cache, the corpse bar, gates, stuck at his own origin, the party-combat sweep | `tools/iaus_sim/Loot.h`, `struct World`. Each rule names its native file. The reading it was written from is `_agentlogs/mfo/sim-loot-native-spec.md` (an Opus pass over `native/logistics`, 278 lines, file:line for every rule) | a native constant: one row of `Loot.h Rules` (or `--loot-rule <name>=<v>`). A `LootTuning` knob: the game's own table, `--set loot.<name>=<v>` (`Tune.h kLootKnobsF / kLootKnobsI`) |
| What to take | native's own code compiled in: `iaus/LootNeed.h ScoreLootKind`, `ScoreConsumable`, `ScoreStealNeed`, read through `Tune().loot` | the game's table |
| The episode: a fight, the looting after it, the player's script, a second group arriving, the leg mover, the follow package, the ledger of what was left | `tools/iaus_sim/LootLab.h`, `struct Episode` | a player script: `enum Plan` and `PlayerStep`. A ledger reason: `Finish` |
| The gate | `LootLab.h kLootBounds`, `GateValues` | a bound: one row of `kLootBounds` |
| The loot oracle | `LootLab.h RunOracle` | |
| The field cases as scenarios | `tools/iaus_sim/LootCli.h Scenarios()` | a scenario: one row |
| Loot fixtures | `tools/iaus_sim/envs/loot-*.txt` (`make_fixtures.py`: `loot`, `guard`, `bystander`, `body`, `town` lines) | draw a map, run `make_fixtures.py <name>`, add the name to `Env.h kFixtureNames` |
| A list's items, containers, leveled lists, flora produce and NPC inventories | `tools/list_balance/extract_loot.py` writes `~/.cache/mfo-list-balance/<set>.loot.json` (never committed). The sim reads it with `--loot-list` and joins it to a place by (plugin file, local id), the key `tools/envextract` writes. No name of a list or of a record is read anywhere | a record type: `extract_loot.py`, with a self-check row |
| The loot refs of a place | `Env.h LootRef` (containers, loose items, flora, activators, with owner and lock), filled by `LoadEnv`. `--env-set <set>/heldout` loads a set's held-out places | |
| Field rates | `tools/iaus_sim/calib_loot.py <run-dir>` | a log line: one row of `ROWS` |
| The driver's loot stage | `tune_driver.py stage_loot_episodes`, `LOOT_KNOBS`, `LOOT_EPISODE_KNOBS`, `LOOT_NOT_SEARCHED`, `--loot-places <set>`, `--loot-world episodes|stops` | a searched row: one tuple |

CLI: `--loot-report N [--loot-score]`, `--loot-scenario all|<name> [--loot-trace 0]`, `--loot-test`, `--loot-oracle N`, `--loot-cost N`, `--loot-plan mixed|stands|walks-on|opens-chest|loots-too|faces-away`, `--loot-wave mixed|off|<seconds>`, `--loot-sec S`, `--loot-guards N`, `--loot-list <file>`, `--loot-museum <share>`, `--loot-rule <name>=<v>` (`gate=old1117`), `--env-set <set>/heldout`. Any of them turns `--world v4` on.

### 14.2 What is mirrored, and how

The clocks are native's: one follower is served per pump (133 ms), and a follower's loot tick runs at his first service a second or more after his last. So every arrival, block and release is read 1.06 to 1.9 s late, which is where much of the field's "drifted away" and the waits come from. Movement runs every world step on the navmesh, by the mover batch W built.

- **Scan** (`LootScan.cpp LootNearby`): 3000 units round the follower, the leash from the player by his vitality (512 to 4000), the walk limit, the 150 unit player bubble, the navmesh gate of 300, the cap of 48 candidates applied in ref order before the sort, the sort by (tier, distance) and never by value, two passes (the free tier, then everything), the plan from `ScoreLootKind` with the museum row first and the steal road last.
- **The excursion** (`LootTravel_internal.h TravelIntent`, `Service.cpp`): one of four slots, a chain of legs, the 60 s cap over the whole chain, the release past the leash, the batch hold (`batchHoldMinSec`) and its one rescan, the guard-pending hold.
- **Legs**: the ch.19 claim's radius of 128, MFO's own arrival test (200 for a body, 300 for a loose item, grown by path fails), the deadline `clamp(d / 150 + 5, 6, 20)` s, no progress in 5 s, Movement Blocked held 3 s, an actor in the way (reorder), the far arrival ("ARRIVED but he is N u away"), the 25 s and 60 s blocklists and their strikes, the idle reassess.
- **Dibs**: one walker per source past arm's reach. The player's dibs per source (`LootTake.cpp TierReleased`): equipment after 4 s, coin, jewelry, soul gems and valuables after 6 s of the player near it AND facing it, or 3 s after he left it by 700 units, or 45 s if he was never within 512, or once he took from it and moved on.
- **The take**: the plan in score order at the source, the whole stack or nothing against his carry weight, the landing hold (a second category of the same form type waits 150 ms), ammo by native's keep ladder, one equipment piece per call.
- **Guards and theft** (`TheftGate.cpp`, `Steal.cpp`): the answer cache with its three timers as an async post (the first ask is always pending, the cached answer stands until the new one lands), keep-warm, the town rule, the waiver of a hostile body, the party-wide bar on a body a guard saw, the witness check run again at the write, the Thief's steal-by-need on owned loose items.
- **Gates** (`Service.cpp:1338-1402`, `LootTravel_internal.h`, `StuckAtOrigin.cpp`): the obstacle probe (a locked door or an activator within 256 units on the way), GATED with its cone or TARGET ONLY, no ground closed since his last block (reorder), stuck himself after two blocked ends within `gateRepeatU`, the re-admits (a door or lever event, the recorder gone 500 units, somebody at the item, his own records lifted after four idle ticks). `--loot-rule gate=old1117` is the rule before `9a16a278`.
- **Party combat**: every slot released by whoever is served next, the scan refused, dibs, blocklists, gates and bars kept, the batch lost, looting resumed at the first tick out of combat.
- **The player** (MODEL, a script per episode): he stands, walks on after a few seconds, opens the nearest container (its menu holds every scan), loots the bodies' coin himself, or stands with his back to the bodies.
- **Bags**: `Logistics.h Inv`, unchanged. The fight kit and the bag are one stock: a potion looted is one he can drink in the next fight, and what he drank leaves the bag.

**Sources.** A body lies where its foe fell. A container, a loose item and a flora are the place's own refs, with their owner and lock. In a list world a container's contents are its base record's rows drawn through the list's leveled lists at the player's level, and a loose item is its base record. MODEL: a body's contents are drawn from one of the list's NPC inventory rows (the fight's foes carry no record id), and half the bodies carry coin. In the built-in world the old lab's draws fill them.

**MODEL, with no native source**: the leg mover reports Movement Blocked when a leg makes no ground for 3 s while a route exists. The claim's radius of 128 is about the ref, and a ref off the mesh ends the leg at the mesh point nearest it. After a leg ended ARRIVED the claim holds him there, and in 7 percent of them the follow package takes him back first (`Rules::driftShare`, CALIBRATED per leg and never per arrival: the four field sessions print "ARRIVED but he is N u away" 12 times on 277 legs asked for, 4.3 percent, and the lab prints it on 1.4 to 5.5 percent of its legs on the six library runs). The follow package walks him to the player past 400 units. A guard walks a line (fixtures) or stands at a spawn point of a place that has owned things (library, `--loot-guards`). A second group arrives 6 to 20 s into the looting in half the episodes.

**Not built**: lock picking (a locked container is left and counted), the load-door rescue of a stuck follower, the LOTD deposit trip, an errand during an episode (`Logi::Town` runs at its end, as in the old lab), the teleport-mod clamp, QuickLoot's crosshair. Museum items are a FLAG on an item: the fixture's one made-up piece, and with `--loot-list` a share of the list's records chosen by a hash of their key (`--loot-museum`, default 2 percent). Which items a museum wants is the player's game state and is in no extract.

### 14.3 The numbers, and the gate

One episode is counted into `Loot.h LTally`. `--loot-report` prints it per class (`LOOT`), the events under the field log's own names per follower-minute (`LOOTRATE`, the names `calib_loot.py` prints as `FIELDRATE`), every stack the game's own scorer wanted and left with the reason (`LOOTLEFT`), what was left because no loot road takes it (`LOOTNOROAD`, by item kind with its gold: off-class gear, books, ingredients), and the gate (`LOOTGATE`).

"Wanted" is the SHIPPED table's verdict on the bag each follower BEGAN the episode with (review S3). It is not the table under search and not the bag he ends with: judged that way, a table that takes nothing wants nothing, a full bag wants nothing, and leaving loot would read as a clean ledger. `walkedPast/ep` is judged by the shipped table too, and the two wait rows count a stack left released and reachable as a wait that lasted to the episode's end (a censored wait). So no row gets better because a table wants less. Each wanted stack left gets the first reason that holds: the party lost the fight, carry weight, owned, locked, flora, town container, guard, off-navmesh or unreachable (no route from the player on the true mesh), out of range (the player left), the player's dibs, then the reasons that count as RELEASED AND REACHABLE: GATED, blocklisted, a follower stuck at his origin, the episode ended first, the player's bubble, the island verdict, the candidate cap, beyond every follower's scan radius, and `ignored` when none of them holds.

| Gate row | Bound | Field case it stands for |
|---|---|---|
| `leftRR/ep` | 0.5 | marth: "lots of items being ignored". Scenario `freeze-1117g` |
| `goldLeftRR/ep`, `coinDibsNear/ep` | 0.1 | marth: coin left on bodies. Scenarios `coin-behind-player`, `ruling-near-is-enough` |
| `dibsNear/ep` | 0.1 | ANY stack left under the player's dibs while he stands within 700 units of it at the end (coin, valuables, jewelry, soul gems, equipment) |
| `roadLeft%` | 60 | of the stacks any loot road could take (every category, whatever a score says, the bag as it began, nothing excusing it): the share left. No table can shrink this denominator, so a table that ignores loot fails here |
| `leftCarry/ep`, `leftGuard/ep`, `leftOutOfRange/ep` | 0.5, 1, 5 | wanted stacks left for carry weight, because a guard sees them, because the player walked out of range (review S5: these excuses had no row, so loot could move into them unseen) |
| `museumLeftRR/ep` | 0.1 | marth: museum items left |
| `crimes`, `guardSeenTakes` | 0 | "followers never commit detected crimes". `crimes` = takes of an OWNED thing a witness truly saw. `guardSeenTakes` = takes a guard truly saw of a source the guard rule covers. Both are read on the TRUTH over the whole landing frame of the take (0.5 s), not on the rule's own cached verdict at the write (review S1: read that way the row could not fail). Scenarios `guard-bar-1117`, `thief`, and `thief-no-recheck`, whose variant MUST commit one |
| `legs/src` | 2 | trips per item. CALIBRATED: the field asks for 13 to 15 legs per looted arrival (11-17c, 11-17d, 11-18) |
| `restart%` | 35 | marth 11-17c: "stops and restarts". CALIBRATED: the field reads 18, 17, 20 and 37 |
| `aborted%` | 30 | legs ended by anything but arrival. The field's upper bound is 83 to 90 |
| `reversals/min` | 1 | back and forth |
| `twoOnOne/ep` | 0 | two followers on one thing |
| `combatLoot s/fight`, `combatTakes` | 1.5, 0 | field 11-18: 12 `RELEASE why=party combat`. Scenario `party-combat` |
| `walkedPast/ep` | 0.5 | he came within reach of wanted, released loot and left it |
| `stuckOff s/ep` | 5 | field 11-18: 4 "stuck himself" episodes |
| `wait free p90 s`, `wait dibs p90 s` | 25, 60 | time from release to taken, or to the episode's end for a wanted stack left released and reachable. Where more than a tenth of them are left the row reads the episode's loot time (90 s on the library) |

Every bound is a PLACEHOLDER for marth to set. Where a field log shows the number it is printed beside the row, and the field fails the same rows the lab fails. marth's method constraint is kept: nothing is argued from arrival counts, a leg is what MFO asked for (DISPATCH, RETARGET) and an end is the release reason or the state Harbinger reported.

### 14.4 The field cases as scenarios

`--loot-test` plays fifteen. Each runs under the lab's rules and under a variant, and says what must hold of both. The lab's default rules are native's with marth's two rulings of 11-24 ON (14.6 N1 and N6), which native does not have yet. `--loot-rule dibsNear=0` and `--loot-rule gateEvents=0` give today's native, and the two scenarios that reproduce those faults are pinned to it.

| Scenario | Field case | Variant | Verdict |
|---|---|---|---|
| `room-basic` | a potion, arrows, a museum piece, a chest and coin in one room | the museum road off | pass: everything taken, the piece only with the road on |
| `freeze-1117g` | 11-17g: one blocked follower gated a cone and outdoor loot froze | `gate=old1117` | pass: today's rule leaves nothing released and reachable, the old rule leaves the cone GATED and fails the gate |
| `locked-door` | loot behind a locked door, the player opens it at 30 s | the door stays shut | pass: GATED until the door event, then taken. Nothing through a locked door |
| `guard-bar-1117` | 11-17 L1, Raven Rock: 19 BARRED arrivals | no party-wide bar | pass: two BARRED arrivals with the bar, more without. No take a guard saw in either |
| `pending-1117c` | 11-17c F1: a pending guard answer dropped every candidate | timers shorter than a loot tick, no keep-warm | pass: today's timers lose nothing in a guardless room, the short ones drop the candidates as `guard (pending)` |
| `restart-1117c` | 11-17c: "stops and restarts" | `batchHoldMinSec` 0 | pass: the hold never makes more excursions than no hold |
| `party-combat` | 11-18: `RELEASE why=party combat` | no second group | pass: no take in combat, a slot outlives the fight's start by under 1.5 s |
| `two-followers` | several followers in one room | one follower | pass |
| `thief` | steal by need, unseen only | a Knight | pass: the one unseen potion is stolen, the one a bystander sees is not |
| `thief-no-recheck` | a guard walks in and out of sight of an owned potion a Thief needs, twelve starts of his walk | the witness check not run again at the write (`stealRecheck=0`) | pass: no crime with the re-check, and the variant MUST count one (so the `crimes` row can fail) |
| `ruling-near-is-enough` | marth 11-24: coin dibs "Near is enough" (near 6 s, no facing). The three bodies behind a standing player | today's native (`dibsNear=0`) | pass: all coin taken under the ruling (taken 5 of 8, coin left 0), today's rule leaves all three coins (taken 1) |
| `ruling-gate-events-only` | marth 11-24: a gate lifts on real events only. The locked door, opened at 30 s | today's native (`gateEvents=0`) | pass: no record lifted by its own follower walking off, the door event lifts it, the loot is taken, and not more legs than today's rule (10 against 11) |
| `coin-behind-player` | coin left on bodies, under today's native dibs (`dibsNear=0`) | the player walks on | REPRODUCES a fault of today's rules (14.6 N1) |
| `locked-door-cycle` | the same locked door, never opened, under today's native (`gateEvents=0`) | `gateRepeatU` 0 | REPRODUCES a fault (N3, N6) |
| `carry-full` | a follower at his carry limit | room in the bag | REPRODUCES a fault (N4) |

### 14.5 The loot oracle, the value, and the loot stage

**Value** (`LootLab.h Score`): the fights' Utility with potions not charged there, plus what the bags gained: the potion worth of `PotionWorth.h` (the k-th potion of a kind) and `GoldJ` times the gold-equivalent of `Logistics.h Value`, after the same town errand as the old lab. A potion drunk in a fight and a potion looted are the same stock, so neither is counted twice. Every report prints the value with and without the gold term.

**The oracle** (`RunOracle`). Its plan is every category with a loot road, whatever the scorer says, and every (follower, source, category) take is a decision. Each decision in turn is tried both ways by replaying the same episode from the end of its first fight (a copy: the same draws, the same second group), with the earlier decisions fixed and the later ones "take". The better end value stands, a tie leaves. So a take that fills the bag before the potion, or that keeps him away when the second group arrives, pays for it in the same number. It has only the game's roads: nothing owned without the steal road, nothing locked, nothing a guard bars, nothing under the player's dibs. The walking is the game's (native ranks by distance, a rule and not a knob). The game's own play is one of its options, so a search that ends under it is replaced by it and counted (`floored`).

**The oracle is a LOWER BOUND on the best play, and so is its gap.** It is greedy over the take decisions in the order they come up, one pass, the later ones held at "take". It does not search orders, routes, or which follower goes, and it never beats the game's walking. A gap of 0 says this search found nothing better, not that nothing better exists. The report's header says so.

**The driver's `loot` stage** runs on episodes (`epi:` cells, `sim --loot-report N --loot-score`). It searches the 16 score rows and ONE rule row, the batch hold (`LOOT_EPISODE_KNOBS`), on the mean episode value. A crime or a guard-seen take costs 1000 in that value, so no step can buy value with one. Then the kept table is checked (`tune_driver.py loot_gate_check`), on the gate and, with `--loot-places <set>`, on that set's held-out places:

- ANY gate row worse than the shipped table's is an open decision, whether or not it crosses its bound (review S2: the first version flagged only a row that newly failed).
- More `crimes` or `guardSeenTakes` than the shipped table REJECTS the table: `kept` is emptied, the reason and `kept_before_reject` are in the summary.
- A kept table that loses value on the held-out places is an open decision.

Not searched, each with its reason in `LOOT_NOT_SEARCHED` (26 rows): `guardFreshSec`, `guardUnreadMaxSec`, `guardRepostSec` and `guardBarFloorSec` are a guard rule's timers, a safety rule and not a value trade (a value search would only ever shorten the caution). `gateReadmitSec` is marth's "no timer" ruling. The six gate rows (`gateConeDeg`, `gateBeyondU`, `gateReadmitMoveU`, `gateRepeatU`, `gateShortU`, `gateNearU`) have no signal yet (review S6): the field blocks 2 to 5 times as often as the lab (14.8), so a value found here says nothing about the game. They become searchable once the lab's block rate matches a field log. `offClassDwellSec` is an equip notice no loot rule reads. **Known tie**: `loot.gold` and `loot.valuables` are not independent. Native's Valuables road takes coin too (`LootTake.cpp`), so with valuables in the plan the gold row decides only the order. The stage records it as a note on every run. `--loot-world stops` is the old lab.

**Ignoring loot does not read better** (review S3). `--set loot.takeThreshold=1.0` (take almost nothing) against the shipped table, 480 episodes a set: the value falls on every set (Tuxborn 131.03 to 115.09, fixtures 119.14 to 115.00), `leftRR/ep`, `dibsNear/ep`, `roadLeft%` and `walkedPast/ep` get worse on all seven sets, and the count of rows over their bound is never lower: LoreRim 7 against 7, its held-out 10 against 10, Tuxborn 8 against 6, its held-out 10 against 9, Vanilla 8 against 8, its held-out 8 against 7, fixtures 9 against 6. The ties are sets where the shipped table is already over on the same rows. The rows that DO read lower for a table that walks less are the walking rows (`legs/src`, `restart%`, `reversals/min`, `stuckOff`, `guardSeenTakes`), which is what they measure. That is why the driver compares every row with the shipped table's number and not the count. A CLI check holds the fixtures case.

### 14.6 What the mirror shows about the game (native findings)

Each is today's rule doing what its code says, read in a scenario or in the library runs. None was changed here: native is not this batch's to edit.

- **N1, coin left on bodies** (`LootTake.cpp TierReleased`, read line by line for this). Coin, jewelry, soul gems and valuables are released by the player's dibs only when he has been within 512 units AND facing the source for 6 s, or 3 s after he went 700 units from it, or after 45 s if he was NEVER within 512. A player who stands among the bodies and does not turn to one of them never releases it: near once, so the 45 s clock is off, never facing, never 700 away. Scenario `coin-behind-player`: three bodies round a standing player, all three coins left for the whole episode. On the library `coinDibsNear/ep` is 0.31 to 0.52 (coin left under his dibs with him within 700 units at the end), on every set, held-out or not. This is the largest single reason coin is left. **marth's ruling of 11-24: "Near is enough"** (near 6 s, facing or not). It is the lab's default (`Rules::dibsNearEnough`, `--loot-rule dibsNear=0` for today's native) and NOT in native yet. On Tuxborn's library it takes `coinDibsNear/ep` from 0.36 to 0.03 and `dibsNear/ep` from 0.61 to 0.06, and the value from 129.27 to 131.03 (both rulings on).
- **N2, a thing the mesh reaches and the world does not** (`Service.cpp:1338-1402`, `StuckAtOrigin.cpp`). A static block with no door or lever is never recorded against the item. The first block is a bump, the second "no ground closed" defers the item and is the second stuck hit, he is "stuck himself", every loot of his is off until he stands 300 units away, the follow package takes him there, and the item is his nearest candidate again. Scenario `freeze-1117g` under today's rule: 27 legs in 120 s for one potion he cannot reach, 23 of them restarts, a cycle of about 8.5 s. He does take everything else, which is what the 11-17 fix was for. The field's 11-18 session has the same shape: 4 "stuck himself", 4 "left the stuck point", 7 first-block bumps.
- **N3, a locked door with two things behind it** (the same code). GATED feeds the stuck run too (`feedStuck` stays true after `MarkGated`), so the next item at the same door is "blocked again at the same point" and he is "stuck himself". His own gate records are lifted after four idle ticks with no cooldown, and it begins again. Scenario `locked-door-cycle`: 25 legs in 120 s to a door nobody opens.
- **N4, the carry peek** (`LootTake.cpp HasLoot`, `LootEquipment.cpp:511`). Only the equipment peek weighs. A follower with a little room left and a stack that does not fit walks to it, takes nothing, the body is blocklisted 25 s as "fully stripped", and he walks again. Scenario `carry-full`: 15 legs, nothing taken. The score's room term hides it only once the bag is completely full.
- **N5, the guard answer asked on the way** (`Service.cpp:873`, `TheftGate.cpp GuardCheck`). The peek at 600 units posts a scan. Its answer is unread, so it is usable at arrival for up to `guardUnreadMaxSec` (10 s), and it was taken where he stood then. Where a guard cannot see the item but can see a follower standing at it, the take goes through: 3 to 29 takes a guard truly saw in 480 episodes on the sets with guarded places, 0 on the others. None is a crime (an owned thing is never a candidate): `crimes` is 0 in every run. This one hangs on the lab's reading that a peeked answer counts as unread (uncertain spot 2 of the report).
- **N6, a gate lifted by its own follower's walk** (`LootTravel_internal.h:513-536`, `gateReadmitMoveU` 500, shipped ON). The follower whose block made the record lifts it by standing 500 units from the block point. The follow package takes him there, the item behind the same door is his candidate again, and he walks back. The review found the loop in the 11-18 field log (item 000EA786). On Tuxborn's library today's rule lifts 62 records this way in 480 episodes. **marth's ruling of 11-24: a gate lifts on real events only for now** (a door, a lever, a cell change, somebody else reaching the item). It is the lab's default (`Rules::gateEventsOnly`, `--loot-rule gateEvents=0` for today's native) and NOT in native yet. A route-based lift comes later, with native's active navmesh. Scenario `ruling-gate-events-only`.
- **Not seen**: the 48 candidate cap (spec B6) hid nothing in 3360 episodes. Two followers on one source happen only inside arm's reach, where native has no exclusion.
- **What has no road at all** is printed apart (`LOOTNOROAD`): on Tuxborn's library, per 480 episodes, 1174 stacks of gear no keep rule judges (80121 gold), 1358 one-handed and 352 two-handed weapons that are no upgrade (53958 and 114941 gold), 778 books and scrolls. Off-class pieces have no loot-time road in the game (the storage chest works at sale time), so the oracle has none either.

### 14.7 What should be extracted from native

Shared today, compiled into the lab as they are: `iaus/LootNeed.h` (with `iaus/PotionBuff.h`). Everything else of `native/logistics` uses engine types, so `Loot.h` restates it. Each of these is pure logic inside an engine file. Moved into a header with plain positions and ids, the lab would include it and the restatement would go:

| Piece | Where it is | What it needs |
|---|---|---|
| `TierReleased`, `ClaimRejected`, `PlayerFacing`, the `Claim` record | `LootTake.cpp:783-878`, `Logistics_internal.h:717` | positions and the "considering" flag as inputs |
| The reaction to a static block, `NoteBlockGround`, `BehindBlock`, `GateRecord`, the re-admit sweep | `Service.cpp:1338-1402`, `LootTravel_internal.h:422-536, 613` | a plain vec3 |
| The stuck run (`StuckAtOriginNoteBlock`, `RepeatsBlock`, `ResetRun`) | `StuckAtOrigin.cpp:215-314` | actor reads as inputs |
| `SortLootCandidates`, `TravelDeadline`, `GrabRadiusFor`, the blocklist maps and strikes | `LootTravel_internal.h:242-359, 773-828` | ids and positions |
| The guard cache rule (fresh, unread, one verdict a visit, repost) and the corpse bar's timing | `TheftGate.cpp:198-220, 335` | a clock and a callback |
| The category predicates (free tier, dibs tier, clutter) | `Logistics_internal.h:606-680` | nothing: only the header's includes block it |
| `KindOf` (an effect's buff kind) and the consumable reader's rules | `PotionFx.cpp`, `LootNeed.cpp:126` | archetype and actor value as numbers |
| The ammo keep ladder (`SwapUpAmmoTakeSet` and its helpers) | `SwapUp.cpp:449-549`, `Logistics_internal.h:1180-1203` | an opaque item id |
| The Holding dwell and the far-arrival counter | `Service.cpp:1297-1317, 1621-1662` | inline in the one `ServiceFollower` |

### 14.8 Checks, cost, and what was not run

Made once, at the end of the review round, on the branch head with `origin/main` (`2d617b7d`) merged.

- **The fight baselines.** `origin/main` fails its own three baselines since `fix/spell-worth` merged without rewriting them: `--world v2` 824 metrics changed, `--world v3` 1099, `--world v4` 899. This branch reads the same three numbers, and against baselines written by main's own binary it reads `--world v2` 0 changed, `--world v3` 0 changed, and `--world v4` the same file byte for byte on the old fixtures. So looting moved no fight. The three baseline files were NOT rewritten here (the drift is not this batch's), except the pool hash in the first line of `baseline_v4_builtin.txt` (14.9).
- `sim --env-test`: 81 checks pass. `sim --loot-test`: 15 scenarios, 0 failed, 3 of them reproduce a fault of today's rules. `test_tune_driver.py`: 41 tests pass. A smoke run of the driver's `loot` stage on episodes ends with its summary and the two notes (not checked held-out, the gold tie).
- `run.sh` with `SIM_CLITEST=1`: every loot check passes, and so do the two batch W checks that failed in the first round (the fixtures' `--check`, and the library solo walk now that the duplicate-navmesh place is skipped). Five checks FAIL, all five on `origin/main` exactly as here (main's own binary prints the same numbers): the three baseline compares above and the two `--t2 off` lab-of-section-30 lines (J 79.47 and 80.07 against the 79.31 and 80.10 written in the check), the same spell-worth drift. One loot check was wrong after S1 and was corrected: it asked that no scenario ever count a crime, and `thief-no-recheck`'s variant must. The corrected line was run by hand, not in a second full suite. The same report at 1 thread and at 8 is identical, fights and loot. The field-log check ran against `_evidence`.
- **The gate on the library** (480 episodes a run: 15 cells, 4 seeds, 8 episodes, the player's script and the second group mixed). The lab's default rules, so with marth's two rulings ON. The last two columns are today's native (`--loot-rule dibsNear=0 --loot-rule gateEvents=0`). `*` = over its bound.

| Row | LoreRim | LoreRim held-out | Tuxborn | Tuxborn held-out | Vanilla | Vanilla held-out | fixtures | Tuxborn, today's native | fixtures, today's native |
|---|---|---|---|---|---|---|---|---|---|
| `leftRR/ep` | 1.74* | 2.30* | 1.92* | 3.13* | 1.69* | 1.15* | 0.39 | 1.95* | 0.39 |
| `goldLeftRR/ep` | 0.15* | 0.11* | 0.17* | 0.27* | 0.42* | 0.27* | 0.06 | 0.15* | 0.05 |
| `coinDibsNear/ep` | 0.02 | 0.06 | 0.03 | 0.02 | 0.09 | 0.02 | 0.04 | 0.36* | 0.24* |
| `dibsNear/ep` | 0.05 | 0.13* | 0.06 | 0.04 | 0.19* | 0.04 | 0.09 | 0.61* | 0.39* |
| `roadLeft%` | 49.2 | 69.3* | 41.8 | 51.5 | 72.3* | 67.7* | 48.1 | 46.2 | 52.1 |
| `leftCarry/ep` | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| `leftGuard/ep` | 0.06 | 4.00* | 0.82 | 1.87* | 0.02 | 0.82 | 0.55 | 0.85 | 0.55 |
| `leftOutOfRange/ep` | 7.59* | 3.22 | 7.42* | 7.02* | 3.35 | 1.92 | 0.00 | 7.42* | 0.00 |
| `museumLeftRR/ep` | 0.00 | 0.00 | 0.02 | 0.08 | 0.01 | 0.01 | 0.00 | 0.02 | 0.00 |
| `crimes` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `guardSeenTakes` | 7* | 24* | 0 | 30* | 0 | 4* | 0 | 0 | 0 |
| `legs/src` | 1.38 | 1.86 | 1.42 | 1.70 | 1.71 | 1.31 | 2.49* | 1.50 | 2.75* |
| `restart%` | 29.3 | 41.7* | 28.0 | 36.4* | 36.7* | 25.5 | 53.6* | 30.5 | 56.8* |
| `aborted%` | 15.2 | 28.8 | 19.5 | 21.1 | 38.0* | 17.9 | 52.8* | 22.2 | 56.6* |
| `reversals/min` | 0.30 | 0.37 | 0.28 | 0.53 | 0.27 | 0.29 | 0.44 | 0.28 | 0.43 |
| `twoOnOne/ep` | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| `combatLoot s/fight` | 0.05 | 0.04 | 0.04 | 0.06 | 0.04 | 0.04 | 0.06 | 0.04 | 0.06 |
| `combatTakes` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `walkedPast/ep` | 0.71* | 1.67* | 0.69* | 1.71* | 0.39 | 0.62* | 0.50* | 0.70* | 0.48 |
| `stuckOff s/ep` | 0.01 | 2.44 | 0.09 | 0.00 | 1.78 | 0.17 | 15.0* | 0.22 | 16.7* |
| `wait free p90 s` | 90.0* | 90.0* | 90.0* | 90.0* | 90.0* | 67.4* | 19.3 | 90.0* | 19.3 |
| `wait dibs p90 s` | 90.0* | 90.0* | 87.2* | 90.0* | 90.0* | 90.0* | 60.0* | 89.5* | 60.0* |
| rows over | 7 | 10 | 6 | 9 | 8 | 7 | 6 | 8 | 7 |
| value/ep (without gold) | 143.651 (108.848) | 132.228 (107.490) | 131.025 (111.258) | 130.126 (110.626) | 125.398 (111.215) | 126.619 (111.877) | 119.143 (113.377) | 129.274 (111.093) | 118.579 (113.155) |

The game fails the gate on every set: that is the report's point, not a lab failure. The fixtures are trap-heavy by design (a freeze, a locked door, a guarded street), which is their `stuckOff` and `aborted%`. Against the table of the first round: the two rulings take `coinDibsNear/ep` from 0.26 to 0.52 down to 0.02 to 0.09, `leftRR/ep` is higher because "wanted" is now the shipped table's verdict on the starting bag (a potion he no longer needs at the end still counts), and the wait rows hold the censored waits. Of the wanted stacks left on Tuxborn's library, per episode: owned 23.5, the party lost the fight 10.0, out of range because the player walked on 7.4, unreachable 2.6, locked 1.9, the episode ended first 1.3, guard 0.8, flora 0.7, off the navmesh 0.7, the player's dibs 0.3, ignored 0.20, blocklisted 0.17, the player's bubble 0.16.
- **Against the field**, per follower-minute (lab: Tuxborn library under TODAY's native rules, since that is what the logs ran. Field: `calib_loot.py`, the denominator is an estimate of scanning time): DISPATCH 1.19 against 1.10 (11-18), RETARGET 1.43 against 0.88, `leg ended BLOCKED` 0.19 against 0.20 to 0.68, `RELEASE why=party combat` 0.13 against 0.21, `why=left leash` 0.05 against 0.02 to 0.28, `why=batch done` 0.82 against 0.23 to 1.45, `stuck at his own origin` 0.06 against 0.07. The field still blocks up to 3.5 times as often as the lab (2 to 5 times in the first round's run): the lab's world is the navmesh, and what stands on it in the game (furniture, clutter, collision the mesh does not know) is not there. That is X1 of section 10.2, the largest known gap of this mirror, and the reason the gate rows are not searched (14.5).
- **The oracle** (Tuxborn, 90 episodes each, a lower bound, 14.5): library 137.69 for the game against 145.43 (gap 7.73 with gold, 1.37 without), 12.8 decisions an episode, 0.34 CPU seconds an episode, floored at the game's own play in 9 of the 90. Held-out: 41.98 against 137.02. That gap is not loot value: the game's play there holds guard-seen takes (30 in the 480 of the table above, finding N5) at 1000 each, and the oracle's play leaves those takes.
- **CPU** (first round, not measured again): one thread, the same fights with looting off and on: on the fixtures 2.10 ms an episode against 2.93, on Tuxborn's library 3.96 against 9.63. A 480 episode report takes about a second of wall time at 8 threads after the load.
- **Not run**: the tune itself (the `loot` stage at size, on episodes, with `--loot-places`). The oracle at size. A list world for the FIGHTS of an episode (`--list`) together with `--loot-list`: the runs above use the built-in fight world in the extracted places.

### 14.9 The review round (Opus review of `9689734b`, 2026-11-24)

| Finding | What was done | Where it is held |
|---|---|---|
| S1 the `crimes` row could not fail (it read the rule's own verdict) | crimes and guard-seen takes are read on the truth over the take's landing frame | scenario `thief-no-recheck`: its variant must count a crime |
| S2 the driver flagged only a gate row that newly FAILED | `loot_gate_check`: any row worse than shipped is an open decision, more crimes or guard-seen takes rejects the table. The guard timers, the bar floor and `gateReadmitSec` are not searched | `test_tune_driver.py test_loot_gate_check`, `LOOT_NOT_SEARCHED` |
| S3 the gate could be passed by taking less (`takeThreshold`) | "wanted" is the shipped table's verdict on the starting bag, `roadLeft%` counts what any road could take, `walkedPast` plans by the shipped table, a stack left released and reachable is a censored wait | 14.5 "Ignoring loot does not read better", and a CLI check |
| S4 the actor defer was not cleared without a slot | cleared | |
| S5 excuses with no gate row | rows `dibsNear/ep`, `leftCarry/ep`, `leftGuard/ep`, `leftOutOfRange/ep`, a museum value | 14.3 |
| S6 the gate knobs were searched with no signal | not searched, with the reason | `LOOT_NOT_SEARCHED` |
| Vanilla `x_Skyrim-104217_1_-2` holds duplicate navmeshes | the loader skips such a place and says why | the library solo walk |
| The batch W fixtures were stale against `tools/envextract/fmt.py` | all 17 rewritten, 48 bytes more each (the extractor's three new sections, empty). Proof that nothing else moved: one binary, the old fixtures against the new, writes a v4 baseline that differs in its first line only (`fixtures#85d7c2c8` to `fixtures#64b360f8`). The committed `baseline_v4_builtin.txt` changed in that line only | `make_fixtures.py --check` |
| marth's rulings of 11-24 | `Rules::dibsNearEnough` and `Rules::gateEventsOnly`, default ON as native will become, `--loot-rule dibsNear=0` / `gateEvents=0` for today's native | scenarios `ruling-near-is-enough`, `ruling-gate-events-only` |

The review's native verdicts: N2 and N3 are proven in the field logs of 11-18 and 11-23, N1 is proven in code, N6 is the `gateReadmitMoveU` 500 loop of the 11-18 log (item 000EA786). None is fixed here.

## 15. Batch C as built (branch `feat/sim-v4-choice`, 2026-11-24)

Slices S7, S11a, S11b (the skeleton only) and S11c, with the mirror of `fix/spell-worth` in front of them. The sim and the driver follow the oracle now. Native gained one unwired file pair.

### 15.1 Where things are

| What | Where | To change it |
|---|---|---|
| Which spells a follower can measure | `tools/iaus_sim/Data.h kIndirectArch`, `SimSpell::indirect`, `Policy.h SpellMeasured`, `FillSpellEstimate` | an archetype whose damage lives elsewhere: one value in `kIndirectArch`, after native's switch in `iaus/Fills.cpp MeasureSpell` |
| The rules of the 11-23 field build, for the "before" column | `Data.h g_spellRules`, `Policy.h SimGenerate`, `--spell-rules today\|old\|all` | |
| The ask list (a refused pair stays asked) | `Policy.h` `ServeWith`, native's own `NextAskList` | |
| Field-size books | `Load.h kFieldBook`, `kFieldBookSize`, `kFieldBookHybrid`, `BookHalf`, `--books lab\|field\|heldout` | the mix or the size: that table |
| Held-out places | `Env.h g_envHold`, `kEnvHoldEvery`, `--env-hold all\|train\|heldout` | |
| Spell regret, its causes, the world's own numbers | `Oracle.h RegretLog`, `TrueSpell`, the block in `OraclePolicy::Serve`, `sim.cpp SpellRegretReport`, `--spell-regret` | a cause: the chain in `Serve`. A family's number: `TrueSpell` |
| Alternate tables on one oracle fight | `Oracle.h AltTable`, `--regret-alt "<sets>"` | |
| Exact pruning | `Oracle.h Dominates`, `--oracle-prune off` | a number the world reads for an offense cast: one more term in `Dominates` |
| The IAUS arm of a gap fight alone | `sim.cpp` `--gap-arm iaus` (`GAPIAUS` lines) | |
| The field's failure, counted without an oracle | `World.h Tally spTop`, metrics `sp.unmTop%`, `sp.tie%`, `sp.capTop%` | |
| The follow stage | `tune_driver.py stage_follow`, `follow_class`, `follow_candidates` | sizes: `FULL follow_seeds`, `follow_knobs`, `follow_min_gap` |
| The spells stage | `tune_driver.py stage_spells`, `spell_knobs`, `SPELL_FAMILIES`, `SPELL_CONS`, `SPELL_SCALARS`, `SW_KNOBS` | a shared number to search: a row of one of those lists |
| The fence form of the J rule | `tune_driver.py judge(fence=True)` | |
| The cost cuts | `tune_driver.py table_slice` (3 and 4), the size loop in `follow_class` (2) | |
| The tune's world | `tune_driver.py V4_TRAIN`, `HOLD_FLAGS`, `world_flags`, `--world v4` (the default) | |
| The spell-worth reader, SKELETON | `native/iaus/SpellWorth.h`, `SpellWorth.cpp` | see 15.6 |

### 15.2 The mirror of fix/spell-worth

Most of that fix needed no mirror: the lab compiles `native/iaus/Generate.cpp`, so the per-second comparison (`Facts::expRate` against `bestDamage`), the exploit term reading no resist the damage already carries, the immune pairs dropped and the placeholder for an unmeasured spell were in the lab the moment main had them. What the lab fills itself is the BOOK, and it filled `measured = true` for every spell. So the lab had no unmeasured spell and the placeholder could not be searched.

**How the lab decides a spell is unmeasured.** From the world json's effect records, never from a name. `Data.h Classify` reads each effect's `archetype`. A spell is `indirect` when any effect's archetype is Script (1), Cloak (35), EnhanceWeapon (39) or SpawnHazard (40): native's own four, the damage of which lives in a hazard, a second spell, a script or a weapon enchantment. A spell is unmeasured when it is an offense spell, it is indirect, and its own hostile value-modifier effects on health sum to zero. That last test is `Estimate::DamageUnmeasurable`, native's function, called by the lab. A bolt with a rider keeps its direct damage, a lower bound. Counts in the taught pools: Tuxborn 108 hostile spells are indirect with no direct damage, 271 are riders with direct damage, 128 are plain. LoreRim 169, 287, 50.

Mirrored with it:
- **Units.** A concentration spell's entry holds one second of the stream (`Fills.cpp Total`). The lab's `SimSpell` holds its 2 s stream, so the entry is half of it. Before, a stream read twice as good per second as it is.
- **The level cap.** `SpellEntry::levelCap` is the record's cap for Calm, Demoralize, Frenzy and Turn Undead. The pairs the generator queues (`Generated::capQueued`) join the ask list.
- **The ask list.** `iaus/Candidates.cpp NoteDecision` keeps a refused pair on the list while its No is under 6 s old. The lab asked only the top pairs of the last decision, so a refused pair came back every other lap. The lab now runs native's `NextAskList`.
- **`t.unmeasuredDamage`** is in `Tune.h kTuneScalars` and is a knob of the spells stage (`SPELL_SCALARS`).

Not in the extract, so not mirrored: effect and MGEF conditions (native counts conditional damage effects once, by the largest. The lab sums every effect), the spell type (a poison-type spell reads poison resist first. The lab reads magic resist for all), and the hazard record. That last one matters: the lab's WORLD deals no damage for the hazard a spell spawns, so a Wall of Flames is worth nothing here and the oracle never picks it. A request to `tools/list_balance`: the three fields above, and the hazard's own spell.

Effect on the built-in baseline: the stream unit moves the casters (evoker, seed 1, J 68.19 to 71.75). The three baselines were rewritten once, at the end of the batch.

### 15.3 Field-size books, and the field's failure reproduced

`--books field` gives a caster the mix of Jesper's book (74 offense, 4 control, 13 heal, 8 buff, 3 ward. Summon 6 is an ASSUMPTION, his line holds none) times a size drawn per follower between 60 and 120 percent, from the world's taught spell records of each nature that his skill allows, without replacement, seeded by the fight. A class off the Mage line holds 35 percent of it (ASSUMPTION). A spell he cannot pay for stays in the book, as in the field. Costs, magnitudes and perk multipliers are the list's own, as before. `--books heldout` draws from the other half of the records (a hash of the editor id), so a tuning is checked on spells no search saw. The built-in world has 17 spells and gives what it has: field-size books need a list world.

`book_from_log.py` (a real follower's `[spellbook]` dump as a book) is not built. It needs the dump keyed by (plugin, local id) and the extract keyed the same way. Neither is today.

The field's failure was two things. Checked by the three counters, natural bracket, Tuxborn, seed 1 (a tiny run, and a CLI check pins it):

Share of the subject's spell laps, percent, `--world v3 --books field --seed 1`, natural fights. `unmTop` an unmeasured spell is on top, `tie` the top two spells score the same, `capTop` a level-capped control spell is on top.

| World | Class | unmTop old | tie old | capTop old | unmTop today | tie today | capTop today |
|---|---|---|---|---|---|---|---|
| Tuxborn | evoker | 41.3 | 6.2 | 0 | 1.9 | 0 | 0 |
| Tuxborn | mage | 28.2 | 2.2 | 0.2 | 0 | 0 | 0.2 |
| Tuxborn | healer | 47.7 | 0 | 0.3 | 0 | 0 | 0.3 |
| Tuxborn | conjurer | 24.8 | 5.6 | 0 | 0.2 | 0.2 | 0 |
| Tuxborn | warlock | 14.3 | 0 | 0.2 | 0 | 0 | 0.2 |
| LoreRim | evoker | 63.2 | 4.3 | 0.1 | 19.5 | 7.2 | 0 |
| LoreRim | mage | 59.2 | 14.1 | 0 | 16.3 | 4.4 | 0 |
| LoreRim | healer | 55.5 | 0 | 0.9 | 0 | 0 | 1.2 |
| LoreRim | conjurer | 42.5 | 6.0 | 0 | 6.3 | 3.7 | 0 |
| LoreRim | warlock | 75.9 | 0 | 0.7 | 40.1 | 1.3 | 1.3 |

On Tuxborn today's rules end it. On LoreRim they do not: an unmeasured spell is still on top in 16 to 40 percent of an evoker's, a mage's and a warlock's spell laps, and two spells still tie. LoreRim's pool holds more spells with no direct damage (169 against 108), and with nothing measured to beat it the placeholder `t.unmeasuredDamage` wins. That number is the spells stage's first knob.

- **Unmeasured spells on top, and tied.** Reproduced with `--spell-rules old`, gone today.
- **A level-capped Turn on top.** NOT reproduced by the rules alone: the lab's will-work answers a capped pair No on the lap after it is asked, under both rules, so a capped control spell is on top in about one lap in a hundred either way. In the field the Turn also keyed a row no caster served (`--seats old` in the v4 world is that game). Measured: under `--seats old` in the v4 world a capped control spell is on top in about 1 percent of laps too. So the capped Turn that held the top in the field is not in the lab. What the lab lacks is the game answering Yes, or nothing, for a capped pair. It is on the open list.

### 15.4 The regret report

On the gap pass (`--class-oracle N --spell-regret`), so it costs no oracle fight of its own. Per decision of the subject where IAUS picked a cast or the oracle's best option is one: the oracle's paired lead of its best castable spell over IAUS's pick, in U, counted only when the race dropped IAUS's pick. The causes, in the order they are tested:

- **gate**: IAUS vetoed the oracle's spell or scored it under the floor, or picked something the class's rulings forbid.
- **sight**: that veto was the sight term, or the spell ranks lower while IAUS reads its target occluded.
- **hand**: that veto was the hand term, or the spell ranks lower with a hand-lost share.
- **fact**: ranked lower and an input is wrong or absent. Either spell is unmeasured in his book, or both are offense spells whose estimate orders them the other way round from what the world does per cast second.
- **weight**: ranked lower with none of the above.

Beside it, from the world and not from the estimate: how far his pick is under the best castable spell in effect per cast second (inside his pick's family, and across the families that have a number) and per magicka. The unit across families is health: removed, restored, or a controlled foe's seconds times that foe's damage rate. A buff, a ward and a summon have no number in the lab yet.

The table, before (`--spell-rules old`) and today, field-size books:

Six natural fights per class, seed 1, `--world v3 --books field`, subject only. `regU/f` is the regret in U per fight, `any` the share of cast decisions with a regret, then the cause split in percent of the regret. Six fights is a small table: read the cause columns and the size of the numbers, not a difference of a few U between two rows.

| World | Class | regU/f old | regU/f today | any old | any today | old: fact / weight / gate | today: fact / weight / gate / sight |
|---|---|---|---|---|---|---|---|
| Tuxborn | mage | 0.38 | 0.42 | 13 | 15 | 0 / 27 / 73 | 0 / 25 / 75 / 0 |
| Tuxborn | evoker | 2.02 | 0.81 | 50 | 29 | 6 / 10 / 84 | 0 / 27 / 73 / 0 |
| Tuxborn | conjurer | 16.47 | 16.31 | 60 | 41 | 0 / 4 / 95 | 0 / 3 / 97 / 0 |
| Tuxborn | healer | 31.97 | 58.56 | 54 | 52 | 13 / 9 / 78 | 0 / 13 / 87 / 0 |
| Tuxborn | warlock | 6.54 | 5.90 | 60 | 59 | 76 / 8 / 16 | 0 / 9 / 91 / 0 |
| LoreRim | mage | 6.76 | 6.72 | 24 | 41 | 0 / 58 / 41 | 0 / 57 / 37 / 6 |
| LoreRim | evoker | 2.83 | 3.54 | 27 | 54 | 85 / 14 / 1 | 0 / 26 / 74 / 0 |
| LoreRim | conjurer | 22.95 | 32.24 | 52 | 56 | 1 / 62 / 37 | 0 / 81 / 19 / 0 |
| LoreRim | healer | 24.22 | 14.83 | 55 | 35 | 1 / 27 / 72 | 0 / 73 / 27 / 0 |
| LoreRim | warlock | 16.41 | 8.89 | 40 | 41 | 2 / 56 / 43 | 0 / 24 / 76 / 0 |

What it says:
- **The fact cause is gone.** Under the old rules it was 76 percent of a Tuxborn warlock's regret and 85 percent of a LoreRim evoker's. Today it is 0 in every row. That is the fix/spell-worth mirror doing what it was written for.
- **The regret itself did not fall.** What is left is gate and weight. On Tuxborn it is mostly gate and cross family: the oracle's best cast is a spell of another family that IAUS vetoed or scored under the floor, not a better bolt. On LoreRim weight leads for mage, conjurer and healer. The within-family part is under 2 U per fight for mage, evoker and warlock, and 7 to 22 U for healer and LoreRim's conjurer.
- **The world's own numbers.** Today's pick is 13 to 39 percent under the best castable spell of its family in effect per cast second, and 22 to 79 percent under the best across families. Under the old rules 13 to 47 and 34 to 79. So the spell choice inside a family got a little better and is still not good. That is what the spells stage and the SpellWorth table are for.
- Healer on Tuxborn reads worse today (32 to 59). One fight of the six carries most of it. Not a finding until the tune's four seeds say so.

The hand and sight causes are 0 except one row. Each run costs what an oracle fight costs: 3,263 and 4,357 CPU seconds for the two Tuxborn tables, 10,627 and 8,067 for LoreRim's (30 fights each, the full race).

### 15.5 The follow and spells stages

**follow** (between `edge` and `spells`). Its rule is `stage_follow`'s docstring. In short: the score is the class's gap to its own oracle on its gap fights, on every world of the run. The oracle arm is paid once per class, table and seed. A candidate is the IAUS arm of the same fights with the value set, which is milliseconds. It is accepted when it closes the gap by more than 2 se with no world worse, holds on fresh gap fights, is not worse on the held-out places and books, passes the J rule as a fence (`judge(fence=True)`: the two lines that ask J to rise ask that it does not fall, every gate and the Knight floor unchanged) and the class card, and the gap did not rise when the oracle ran again on the new table. The oracle's own U shift is printed beside every accepted value. The order is the T1 rows, then the cells of the run. The joint gate and the generator read the table through `final_curves()` as before, which now includes this stage's values. Sight-driven knobs stay skipped.

The closable gap is the gap times the (a) share of the divergence cost. It is an ESTIMATE and a report line, since the cluster costs are an ordering and not an additive share.

**spells** (after `follow`). Its rule is `stage_spells`'s docstring. Shared numbers only: `t.unmeasuredDamage`, each cast family's action weight, and for `damage`, `exploit`, `overkill`, `variety` and `control_worth` the shared curve and each cast family's weight on it. A class's own rows on those considerations are the follow stage's. The score is the spell regret pooled over the cells that cast, on the list worlds. One oracle run prices every candidate value as an alternate table: the oracle's option values are read for what the other table would have picked on the same snapshot. That is a screen, because the rollouts ran under the base table, so the winner is then measured as the table on fresh fights, priced on the held-out places and books, and fenced by J over every cell. The `sw.*` table joins the knob list when native reads it (`SW_KNOBS`. Until then a test holds every field in `SW_NOT_SEARCHED`).

**The cuts of 6.2.** Built: 2 (a cell's seed count grows only while its gap is not measured well enough, from 6), 3 and 4 (`table_slice`: a gap fight's command and cache key hold only its own class's values and the T1 rows', so one class's accepted value re-runs one class, and a class that took nothing replays the shipped measurement). Not built: 1 (bands subject-only) changes what the bands mean and waits for marth. 5 is marth's call by the standing rule. 6 was batch W's.

**Validation** (6.4): `test_tune_driver.py ChoiceBatch` plants an improvement and a regression for each stage on a fixed set of results. The stage accepts the first and rejects the second, and rejects a value that fails only on fresh fights, only held out, or only because the gap rose when the oracle ran again.

### 15.6 The SpellWorth skeleton (S11b), and what filling it needs

`native/iaus/SpellWorth.h` and `SpellWorth.cpp`: the named table `struct SpellWorth` (24 fields, each a labelled placeholder), `SetSpellWorthKnob` and `GetSpellWorthKnob` over one name table, and `Estimate::SpellWorthOn(entry, target, snapshot, table, in) -> {known, effect, seconds, magicka}` with one function per term (cast seconds, channel seconds, area, a DoT over the foe's expected life, control, buff, ward and summon in the same unit, magicka against the fight). Every body is a TODO that answers "not known". Nothing calls it. It is in `native/CMakeLists.txt` and in the sim's build. Engine-free: standard headers and `iaus/Snapshot.h`.

One departure from 7.5: the claim-to-fire seconds are per cast FAMILY, not per engine row, because the pure code does not hold the row. `SpellEntry::engineRow` is on the list below.

What the Sonnet brief needs, precisely (the header's own list, items 1 to 5): the table as a member of `Tuning` and its names in `SetTuningScalar`. The names in `tuningfiles.py`, `Tune.h` and `SW_KNOBS`. The inputs the Snapshot lacks, each with its own reader: charge time, the DoT part apart from the direct part, the engine row, the summoned actor's base, what a buff adds and for how long, a ward's power, a foe's expected life, the fight's expected length. The call sites, all in `Generate.cpp Casts`. A harness batch that pins each term.

### 15.7 The tune's world, the commands, the cost

**The world.** `tune_driver.py --world v4` is the default: the library's train places (`--env-set all --env-hold train`), field-size books (`--books field`), and the held-out places and books for every held-out check (`HOLD_FLAGS`).

**What an oracle fight costs there, and why.** With a field-size book a caster has 60 to 100 options at a decision, most of them offense spells on the same foe. Profiled (gprof and callgrind, one evoker fight): a rollout is 3.5 to 7 ms, 55 to 60 percent of it native's own scoring (`Score`, `Input`, `Casts`, `Eval`), 12 to 15 percent the place's sight and path queries, 4 percent allocation. Nothing in `tools/iaus_sim` makes a rollout cheaper without changing what it computes, and `-O3`, `-march=native` and LTO bought nothing (11.3 to 12.8 s on a 12 s fight, noise). The cost is the NUMBER of rollouts. At the first decision of an evoker's fight 58 offense options (29 spells on 2 foes) stood to the ceiling within 1 U of each other, 62 options times 128 streams. Of all options, 27 percent gave the very same utility as an earlier option on each of the first 8 streams.

**The cuts** (`Oracle.h`, the block above `g_oraclePrune`). The ceiling stays 128, a rollout runs to the fight's end, every decision point and every candidate stay. Off in the simulator unless asked for, on in every oracle run of the driver (`tune_driver.py ORACLE_CUTS`, `--oracle-cuts ""` takes them off):

- **A, `--oracle-prune target`.** The dominance test reads what a spell does to THIS foe (damage times the foe's own resist factors), so two bolts of different elements are compared on the foe they are thrown at. A spell with an area keeps the same-element test.
- **B and C, `--oracle-pair 0.10`.** Pairwise indifference. After each look an option is retired when another standing option exists that it is not better than by more than 0.10 U, at the race's own p on their paired difference. An option that gave the same utility as an earlier one on every stream is retired from 16 streams on (a twin. At 8 streams half of the twins still differ later, one of them by 0.5 U). Never retired this way: the best, IAUS's own pick, an alternate table's pick, the action he is on. So a divergence and a regret mean what they meant.
- **D, `--oracle-nmin 4`.** The first look after 4 streams instead of 8.

**The proof.** `--oracle-dump <file>` makes the full race run every option on every stream and writes the table per decision. A stream's seed does not depend on which options stand, so `tools/iaus_sim/oracle_replay.py` replays any rule on the very rollouts the full race saw (`--check`: its copy of the race reproduces the dump's own picks and rollout counts, 74 of 74). 74 decisions of evoker, healer and nightblade fights, Tuxborn, the tune's world, 97 options a decision. Loss is the 128-stream mean of the best option minus that of the pick, in U per decision. The full race's own loss is 0.005.

| Rule | Rollouts a decision | Fewer by | Same pick as the full race | Loss added, U a decision (se) | Largest loss |
|---|---|---|---|---|---|
| the full race | 3,311 | | | | 0.16 |
| A alone | 2,883 | 1.15 | 100 % | 0 | 0.16 |
| B alone (twins) | 2,688 | 1.23 | 100 % | 0 | 0.16 |
| C alone (pair 0.10, twins in it) | 1,717 | 1.93 | 95.9 % | 0.0010 (0.0007) | 0.16 |
| D alone | 3,192 | 1.04 | 100 % | 0 | 0.16 |
| A + C + D, the tune's setting | 1,398 | 2.37 | 97.3 % | 0.0026 (0.0020) | 0.16 |
| the same with pair 0.25 | 1,147 | 2.89 | 93.2 % | 0.0047 (0.0027) | 0.16 |
| for scale: ceiling 64 | 2,064 | 1.60 | 89.2 % | 0.056 (0.029) | 1.89 |
| for scale: ceiling 32 | 1,173 | 2.82 | 78.4 % | 0.230 (0.109) | 5.53 |

A, B and D give the same pick every time. The tune's setting changes the pick in 2 of 74 decisions, 0.19 U worse over the two together. In J: a caster decides 10 to 20 times a fight, so 0.03 to 0.05 U a fight, against gaps of several U and a divergence floor of 1 U. Lowering the ceiling to 32 costs 90 times that for about the same saving, which is the measured reason the ceiling is not the lever.

**End to end**, the same 12 fights played twice (mage, evoker, healer, 4 natural fights each, Tuxborn, the tune's world, seed 1): 6,344 CPU seconds with the full race, 2,653 with the cuts, 2.39 times less. Rollouts a decision: mage 6,540 to 1,476, evoker 3,701 to 1,110, healer 3,120 to 1,619. The oracle's U per fight 95.33 and 95.70, paired difference +0.36 (se 0.20) for the cut race: no loss to see, and 8 of the 12 fights end within 0.1 U of each other. The IAUS arm is the same number in every fight, as it must be. A fight's own decisions differ after the first changed pick, so this is the sanity check and the replay is the proof.

The default pruning (`--oracle-prune on`) against `off`, the identity check: evoker, Tuxborn, `--world v3 --books field`, 2 fights. The same 14 decisions, the same picks, the same gap and the same regret in every line, 116 options not raced, 147 against 150 CPU seconds.

**What is NOT cut, and what it would take.** A blade's oracle is not helped (a knight has 12 options: 287 against 303 CPU seconds for 4 fights). Shared rollouts between near-twins beyond the pair rule, and cached sight or path answers inside a rollout, were looked at and not built: the first is what the pair rule already takes, the second is at most the 12 to 15 percent above and would have to be proven exact.

**CPU for a full round, 38 cells, four worlds. ESTIMATE.** Measured with the cuts, the tune's world, CPU seconds an oracle fight, subject only: mage, evoker and healer 221 (12 fights, Tuxborn), conjurer 24, warlock 3, nightblade under 1 (2 fights each: a fight that ends fast is cheap, so these are too few to trust), LoreRim's evoker and healer 16 (4 fights), knight 72 (4), archer 62 (2), thief 3 (2). Pooled: about 140 a caster fight, about 40 a fight of the others. Fights, counted from the driver's rules at five fights a run: follow's first measure 38 x 4 x 6 seeds, grown where the gap is noisy, about 7,000. Each accepted follow value three more oracle sets (fresh, held out, the oracle again), at 1.5 values a cell about 20,000. Spells, 15 caster cells on 3 list worlds, two rounds, about 6,000. The gap stage about 6,000 not already cached. So about 39,000 oracle fights, 19,000 of them a caster's: **about 1,000 CPU hours for the oracle stages, between 700 and 1,500** given how few fights the per-fight figures stand on. Without the cuts the caster half is about 2.4 times that, so the round would be near 2,200. The search stages (classes, edge, joint, globals and the rest) were 90 to 130 CPU hours on the v3 world (6.1) and a v4 IAUS fight costs about 2.5 times a v3 one (13): 250 to 300. **The round: about 1,250 CPU hours, between 950 and 1,800.** On 16 cores at idle priority with the machine quiet that is 3 to 5 days. The 1,000 hour target is inside the range and is not the middle of it.

The knobs, in the order I would use them, none of which touches how hard the oracle thinks:
1. **Cells per round.** `--cells 15` (Beta 1) first is about 40 percent of the round. The 23 parked classes follow as a second round on the same `--out` pattern.
2. **Worlds.** Every oracle stage is linear in them. `--worlds builtin,LoreRim,Tuxborn` saves about a quarter. The built-in world has no field-size pool, so it is the cheapest to keep and the least like the field.
3. **`follow_knobs`** (12 values a class at most) and **`gap_seeds`** (12): the accepted-value loop is half the fights. 6 and 6 saves about a quarter and searches less.
4. **`--oracle-cuts "--oracle-prune target --oracle-pair 0.25 --oracle-nmin 4"`**: about 1.2 times fewer caster rollouts for 0.002 U a decision more.
5. **Smaller books in the rollouts, the full book at the decision.** Not built. It changes the base policy the oracle plans against, so it needs its own replay proof.
6. **The ceiling** is marth's call by the standing rule. The table above is what it costs.

**Before the tune, once, on this world** (9.6 and 10.3 item 3. `B` is the round's simulator, `<out>/bin/sim` after the first start, or a `SIM_OUT` build. `W` is `--world v4 --env-set all --env-hold train --books field`, `C` is `--oracle-prune target --oracle-pair 0.10 --oracle-nmin 4`):

1. The oracle-relative bounds of `Playstyles.h`, then rewrite the rows from the `ORCSURV` and `ORCHEAL` lines. 960 fights with every follower planned: ESTIMATE 80 to 150 CPU hours.
   `taskset -c 0-7 nice -n 19 $B --no-json --threads 8 $W $C --seed 1 --fights 1 --rating 0 --class-oracle 160 --gap-scenarios 0 --classes mage,evoker,conjurer,warlock,healer,priest --parked > /mnt/gaming/modlists/Projects/_tuning/pre-v4/bounds.txt`
2. The potion-worth tables: the driver's `prep` stage measures each list's own. The built-in header: `python3 tools/iaus_sim/potion_worth.py --header`, then rebuild.
3. Heal timing under pressure (v3 15.6): the command of item 1 with `--class-oracle 24 --gap-bracket strong2 --classes healer,priest,mage`.
4. The pathing gate's bounds (`kPathBounds`): `$B --no-json --threads 8 $W --classes beta1 --seeds 1,2,3 --path-oracle 8`, then the rows from the `ORCPATH` lines.
5. The regret table of 15.4 in the v4 world, as the "before" of the spells stage: `$B --no-json --threads 8 $W $C --seed 1 --fights 1 --rating 0 --gap-scenarios 0 --gap-dump --oracle-subject-only --spell-regret --classes mage,evoker,conjurer,healer,warlock --class-oracle 6 --list ~/.cache/mfo-list-balance/Tuxborn.sim.json --potion-worth legacy` (and LoreRim). The driver's `spells` stage prints the same table on its own base, so this one is optional.

**The tune.** Idle priority is the default, stop and resume are the same command:

`python3 tools/iaus_sim/live/run_round.py --out /mnt/gaming/modlists/Projects/_tuning/r3-v4 --label "sim v4: follow the oracle, spell choice" -- --cells all --worlds builtin,LoreRim,Tuxborn,Vanilla`

`--world v4` and the cuts are the driver's defaults, so they are not on the line. The Beta 1 cells first: `--cells 15` on its own `--out`.

### 15.8 Checks, and what was not run

Run on the merged tree (main at 85854ecc with batch L in it):
- The simulator builds. `SIM_CLITEST=1 run.sh` once: CLI-CHECKS PASS, one check skipped (`calib_damage.py`, no field logs on this path). `--loot-test`: 15 scenarios pass.
- The baselines, rewritten once after the merge: the v2 and v3 files came out byte-identical, so batch L moved nothing there and the three J pins of `run.sh` stand (69.13, 79.62, 80.17). The v4 file is new (batch L's places with this batch's mirror, 1,540 metrics against batch L's own file). Each compares with 0 changes at 8 threads and at 1.
- `test_tune_driver.py` and `live/test_live.py` pass. The first has a case for the cut switches: on every oracle run, on no IAUS arm, off with one switch.
- `oracle_replay.py --check`: 74 of 74 decisions replay to the dump's own pick and rollout count.
- The local MSVC build of `SpellWorth.cpp` was green at 954ccf60. The file did not change since.

Not run: a driver smoke on the merged tree with the cuts (the smoke of 954ccf60 ran follow and spells end to end without them. The cuts change one command line and its cache key). The pre-tune measurements above. The regret table in the v4 world.

### 15.9 Open

- **LoreRim still puts unmeasured spells on top** under today's rules (15.3). `t.unmeasuredDamage` is the knob. If the spells stage cannot close it the placeholder is the wrong shape and the SpellWorth table has to carry it.
- **The capped Turn is not in the lab** (15.3): the lab's will-work answers a capped pair No on the next lap.
- **The lab's world deals no damage for a hazard a spell spawns** (15.2), so walls and runes are worth nothing to the oracle. Needs three fields from `tools/list_balance`.
- **The cost estimate stands on few fights.** An oracle fight costs between 1 and 400 CPU seconds depending on how the fight goes. The first night of the round gives the real figure: `progress.log` holds every run's time.
- **The pair rule's width is a number somebody chose** (0.10 U, under the 0.3 U the oracle itself calls not worth dithering over). The replay is how to move it.
- **The stealthy-caster term of the on-foe dominance is not in the replay** (no stealthy caster in the dump). It only makes the test stricter.
- `book_from_log.py`, the bands subject-only (6.2 item 1), smaller books in the rollouts: not built.
