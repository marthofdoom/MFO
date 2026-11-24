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
