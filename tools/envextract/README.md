# envextract: real Skyrim environments (navmesh included) for the IAUS simulator

Offline tool. Reads plugin files only (read-only), writes one binary file per environment under
`~/.cache/mfo-env/<set>/` (never in git: it is game-derived data). The consumer is `tools/iaus_sim`; this README is the
contract. Format version 2 (version 1 never shipped).

Requirements: Python 3 standard library for `scan`, `validate`, `svg` and the tests. `library` and `sample` also need
numpy (installed here, 2.5.3; the raster features use it). Nothing is installed by this tool.

## Commands

    envextract.py scan     --set Vanilla|LoreRim|Tuxborn        # read plugins -> ~/.cache/mfo-env/_scan/<set>.pkl
    envextract.py library  --set NAME [--count N] [--heldout-count M] [--coverage-n N] [--seed S] [--changed-only]
                                                                # reject anomalies, coverage pick, write library + held-out + index.json
    envextract.py sample   --set NAME --class C --seed S --count K   # draw MORE environments of a class (seeded, deterministic)
    envextract.py svg      FILE.mfoenv [OUT.svg]                # top-down picture to eyeball
    envextract.py validate [--set NAME | FILE ...]              # NVNM self-check over every navmesh / env files
    python3 -m unittest discover -s tools/envextract/tests      # synthetic-record tests, no game data needed

Sets: `Vanilla` = the five base ESMs of the LoreRim Stock Game Data folder. Lists are resolved from the list's own
`ModOrganizer.ini` (`gamePath`) and `profiles/<profile>/{plugins,modlist}.txt` (no filesystem-wide search).
`library` rewrites a set's folder and its `heldout/` folder (stale files of an older library are deleted); `--changed-only` limits
candidates to places where a mod supplied a navmesh.

## Output layout

    ~/.cache/mfo-env/index.json                       all sets, every environment, class, size, features
    ~/.cache/mfo-env/<set>/<set>.<class>.<id>.mfoenv  one environment each (id = plugin name + local id + grid, load-order independent)
    ~/.cache/mfo-env/<set>/heldout/                    the held-out places: same coverage, never a place (or an overlapping window) of the library
    ~/.cache/mfo-env/<set>/sample-<class>-<seed>/      what `sample` drew
    ~/.cache/mfo-env/_scan/<set>.pkl                  scan cache (tool-internal)

## File format `MFOENV` v2 (little endian, 4-byte aligned, no dependencies)

Round 4 added sections (DREF, SEAM, LAND) and used the `reserved` field of REFS and two new ref flag bits, all append-only: no struct changed, the
version stays 2 and `envfile.h` still loads older v2 files (the three sections are optional; a file without them reports 0 rows).

    u8  magic[8]   = "MFOENV\0\0"
    u32 version    = 2
    u32 nSections
    then nSections x { char tag[4]; u32 offset /*from file start*/; u32 count; u32 recSize; }

Look sections up by tag; unknown tags must be ignored (append-only evolution; any change to an existing struct bumps
`version`). `envfile.h` (this directory) is a header-only C++17 reader with the structs. Its `load()` validates the
whole file before any accessor works (section table bounds, record sizes, alignment, required sections, string
termination, mesh ranges tiling the tables, every vertex / neighbour / link / door / ref index, plugin indexes in ids).
It is tested by truncating a file at every section boundary and by random byte corruption under ASAN and UBSAN
(`tests/envfile_check.cpp`, run by the unit tests when g++ exists).

Ids: every FormID in the file is `u32 id = (plugIdx << 24) | local24`. `plugIdx` indexes the PLUG table of plugin names
(the plugin that OWNS the record, resolved through masters; light plugins use `local <= 0xFFF`). This is NOT the in-game
FormID (that depends on the load order); to join with `list_balance/*.world.json` keys use `(lowercase plugin name,
local)`. 0 means none. Units: game units (1 unit = 1/70 m). Exterior: absolute worldspace coordinates. Interior: cell
space. Rotation: radians as stored in the REFR.

| tag | recSize | content |
|-----|---------|---------|
| META | 1 | UTF-8 JSON: name, set, class, interior, plugins, per-cell info (id, edid, grid, flags, `waterHeight`, `waterSource`), features, counts, walk component summary |
| STRS | 1 | string pool, NUL terminated; offset 0 is the empty string; other sections refer to a string by byte offset |
| PLUG | 4 | `u32 nameOff` per plugin, lowercase file names |
| MESH | 72 | one per navmesh |
| VERT | 12 | `f32 x,y,z`, all meshes concatenated |
| TRIS | 16 | `u16 v[3]; i16 nb[3]; u16 flags; u16 cover` |
| LINK | 24 | edge links |
| DOOR | 20 | door triangles |
| REFS | 128 | placed references |
| TCMP | 4 | per triangle (same order as TRIS) `u16 walk, u16 weak` component ids |
| DREF | 24 | APPENDED: every door ref joined to its navmesh triangle (optional, see below) |
| SEAM | 32 | APPENDED: seams between linked meshes that do not meet (optional) |
| LAND | 4360 | APPENDED: terrain heights of each exterior grid cell of the window (optional) |

MESH: `u32 id; u32 cellId; u32 worldId; i16 gridX, gridY /*0x7FFF if interior*/; u32 vertBase, vertCount, triBase,
triCount, linkBase, linkCount, doorBase, doorCount; f32 min[3], max[3]`. Meshes tile the tables in order. Triangle vertex
indexes and neighbours are LOCAL to the mesh (add `vertBase` / `triBase`). `cellId` = the interior cell, or the exterior
cell's own FormID when it exists in the plugins.

TRIS: `nb[k]` is the neighbour triangle (mesh-local) across edge k, or -1. Edge 0 = v0-v1, 1 = v1-v2, 2 = v2-v0. An
EXTERNAL edge (flag bit k set: the edge is a border to another navmesh, a portal, a ledge or a drop) has `nb = -1` and
a LINK row. Internal neighbours are reciprocal and share the same two points in opposite directions (the swapped edge);
the validator counts the few one-way ones the game data contains. Flags are the game's raw bits:

* `0x1/0x2/0x4` edge 0/1/2 is external: measured, it is exactly the set of edges that carry a LINK row.
* `0x400` door triangle: measured, every flagged triangle has a DOOR row and nearly every DOOR row has the bit (all five
  vanilla masters: 2211 agree, 3 flag-only, 0 row-only).
* `0x200` water: measured against the cell water height, see "Water".
* `0x40` preferred path: NOT measured; assumed from community docs (in the SVG dumps it runs along roads and corridors).
* `0x10` no large creatures, `0x20` overlapping: not measured, community docs only.
* `0x8` deleted triangle: never set on any triangle of the vanilla masters. `0x800`: set on 96 percent of triangles, meaning
  unknown (community docs: "found"); do not rely on it.
`cover` = raw cover bits (unused so far).

LINK (24): `u32 meshIdx /*owner, index into MESH*/; u16 tri; u8 edge /*0..2, 0xFF = no owner*/; u8 kind;
u32 targetMeshId; i32 targetMeshIdx; u16 targetTri; u16 flags; f32 dz`.

`kind` (decoded from the data, 2 vanilla + LoreRim passes; the semantics come from the direction of the links in the data
and the community enum, NOT from in-game observation):

| kind | meaning | how a consumer treats it |
|---|---|---|
| 0 | two-way portal between meshes: cell borders and mesh seams. dz stays within about +-50 units (5 to 95 percentile); rare outliers up to +491 | an ordinary walk edge |
| 1 | ledge UP: always the reverse of a kind 2 link (Vanilla 2350 of 2351 reciprocate as 1<->2) | never a walk edge: jump up only, pay a jump cost, cap dz |
| 2 | one-way DROP down (dz negative). Vanilla 2351 links, 82 percent target the SAME mesh, median drop 122 units, 5 percent worse than 612 | allowed downward only; pay a jump / fall cost and cap the drop; the sim must not walk it as flat ground |
| 3 | enable/disable portal (LoreRim: 393). dz is meaningless (+-20000) | closed by default; never a geometric step; ignore unless something enables it |

`dz` = z of the target triangle's centroid minus z of the middle of this link's edge (units; 0 with `flags & 4`).
`flags`: bit0 target triangle number is beyond the target mesh (stale: the neighbour was edited and the row was not);
bit1 no triangle edge points at this row (owner-less: tri 0, edge 0xFF); bit2 dz unknown (target not in the scan or stale).
`targetMeshIdx` is the target's index in THIS file's MESH, or -1 when the other mesh is not part of the environment
(a border link: the window ends there; treat the edge as a wall or as the environment boundary).

DOOR (20): `u32 meshIdx; u16 tri; u16 pad; u32 crc; u32 doorRefId; i32 refIdx`. The triangle is the one the door opens onto.
`refIdx` indexes REFS; -1 means that door ref is not in this file (it is outside the window, or deleted, or not an exported
kind). The far side is another environment's door via the door's teleport destination (REFS.teleportRef).

REFS (128 B):

    0 u32 id        4 u32 base      8 u32 cellId
    12 u8 kind      13 u8 baseSig   14 u16 flags
    16 f32 pos[3]   28 f32 rot[3]   40 f32 scale
    44 f32 boundsMin[3] 56 f32 boundsMax[3]   world AABB of the base OBND, placed (scale + rotation; conservative for rotated boxes); 0 if none
    68 u32 key (lock key item)  72 u8 lockLevel (raw XLOC level)  73 u8 lockFlags  74 u16 count (XCNT)
    76 u32 teleportRef (destination door REFR)  80 f32 tpPos[3]  92 f32 tpRot[3]  104 u32 tpFlags  108 u32 tpCellId
    112 u32 enableParent   116 u32 baseNameOff (base EDID)  120 u32 modelOff (base model path)  124 u32 reserved

`tpPos`/`tpRot` are where the door puts you, in the space of the DESTINATION cell (cell space for an interior, world space for
an exterior); `tpCellId` is the cell of the destination door ref when the scan saw it, else 0.
`kind`: 0 door, 1 container, 2 activator, 3 furniture, 4 loose item, 5 actor spawn, 6 static (large only), 7 flora, 8 tree (large only).
`baseSig`: 1 DOOR 2 CONT 3 ACTI 4 FURN 5 MISC 6 WEAP 7 ARMO 8 AMMO 9 BOOK 10 ALCH 11 INGR 12 KEYM 13 SCRL 14 SLGM 15 NPC_ 16 LVLN 17 STAT 18 MSTT 19 SCOL 20 FLOR 21 TREE 22 APPA.
`flags`: bit0 persistent, bit1 leveled actor (base is LVLN), bit2 has enable parent, bit3 locked, bit4 teleport, bit5 enable
parent inverted, bit6 STARTS DISABLED.

Start state (bit6): resolved over ALL refs the scan saw, not just the ones in the file. A ref with an enable parent follows its
parent (transitively, the inversion bit flips it, the player ref 0x14 is always enabled, a cycle or an unseen parent falls back
to the ref's own flag / enabled); other refs start disabled when their record flag 0x800 says so after the override rules. Refs
that start disabled stay in the file (a script may enable them); a consumer that wants only what exists at start drops bit6.
They are not counted in the classification features. Deleted refs are dropped.

Actor spawn points are the placed ACHR records; hostility and stats are properties of the base record (join the world json).
"Large static" = the larger side of the scaled OBND box is at least `--min-static` units (default 256).
Window cells that have no navmesh still contribute their refs.

### Appended sections (round 4)

DREF (24): `u32 refIdx; i32 meshIdx; i32 tri; f32 dist; u8 openDefault; u8 lockLevel; u8 teleport; u8 startsDisabled; u32 key`. One row per
door ref in REFS (plain doors and teleport doors). The triangle is the one of the navmesh DOOR row that names the ref (`dist` 0), else the
triangle whose centroid is nearest within 200 units (`dist` = that distance), else -1 / -1. `openDefault` = the ref's XACT action flags carry
"open by default" (bit 0x8; the layout comes from community docs and the single data point that doors with XACT 0x0D exist, not from
observation in the game). `lockLevel`/`key` are the ref's XLOC. `startsDisabled` = REFS flag bit6 (the resolved start state).

SEAM (32): `u32 linkIdx; f32 gap; f32 from[3]; f32 to[3]`. One row per kind 0 LINK whose owner edge does not touch the target triangle: gap
= nearest approach between the owner edge and the edges of the target triangle (0.25 units or more), `from` a point on the owner edge, `to` the
nearest point of the target triangle's edge, so a consumer can snap the two meshes together. Links that meet get no row.

LAND (4360): `i16 gridX, gridY; f32 h[1089]`. One row per exterior grid cell of the window that has a LAND record: heights of the 33x33
vertices, `h[y*33 + x]`, 128 units apart, y south to north, x west to east, origin `grid * 4096` (world space). Decoded from VHGT (f32 offset
then 33x33 signed bytes, row start = previous row start + first byte, then cumulative, times 8). A crest without navmesh is in these heights.
Checked against the navmesh: the terrain is at or under the navmesh (the navmesh sits on terrain, rocks and buildings), see Validation.

Ownership: `REFS.reserved` = the owner id (0 none): the ref's own XOWN, else the cell's XOWN. Flags bit7 = owner is an NPC_ (else a faction or
other form), bit8 = inherited from the cell. `META.cells[].owner` is the cell's owner id. Loose items (kind 4), containers (kind 1, with base id
and lock), flora (7) and activators (2) are exported as kinds with their owner; race flags (open doors, fly, swim) live in the world json, not here.

### Components (TCMP, META.walkComponents)

`walk`: connected components over internal neighbours plus kind 0 links whose target mesh is in the file. Id 0 is the largest.
`weak`: the same plus kind 1 and 2 links (a drop connects two components one way; the weak id says they are related).
Both ends of a path must be in one `walk` component or the environment must be given a jump / fall model: SPAWN BOTH SIDES IN ONE
`walk` COMPONENT (usually 0). Border links (`targetMeshIdx` -1) end the component at the window edge. META lists the 16 biggest
walk components (tris, area).

## What the extractor resolved (override rules)

Plugins are walked in load order (implicit masters, Creation Club files from Skyrim.ccc, then the profile's `*` active plugins).
For navmeshes, cells and refs the LAST record of a FormID wins whole. A record with the deleted flag (0x20) removes the thing.
Compressed records (0x40000) are inflated. Light plugins resolve with 12-bit local ids. A ref moved to another cell by a later
plugin follows its winning record's cell. Persistent exterior refs go into the grid cell their position falls in.

DISPLACED navmeshes: a plugin "removes" a navmesh by moving it far below the map. Relative rules, no absolute height and no list
value. (1) Override rule: the winning record comes from a plugin that is not the record's owner, it is flat (highest minus lowest
vertex under 16 units) and its height is more than 3000 units from the FIRST record of that mesh (the owner's version, kept by the
scan); no neighbour count is needed and parked meshes cannot shield each other. (2) Median rule, for meshes parked by their own plugin
or without history: highest vertex more than 12000 under the MEDIAN of the lowest vertices of the other, non-flat, not-already-dropped
meshes of the cell (interior, at least 2 others) or of the 3x3 cells (exterior, at least 3 others). They are dropped before classification and export; the count per environment is in
`META.counts.displacedMeshesDropped`.

## How the library is picked: reject, then cover (measured geometry, no names, no list-specific ids)

Goal (marth 11-23): every terrain type and design accounted for; places messed up by mods are skipped, not repaired, and the sim's weightings
then work anywhere.

**1. Rejection.** A candidate (each interior cell with a navmesh that is reachable on foot from an exterior cell through load doors that do not
start disabled and whose CELL carries the interior flag; each exterior 3x3 grid-cell window with 12.3M units^2 of walkable area and under 50
percent water) is REJECTED if it has any of: `displaced_mesh` (a navmesh a plugin parked under the map, rules below), `doubtful_mesh` (flat
within 100 units and more than 3000 under its neighbours' median: looks parked, not certain), `plugin_moved_refs` (a later plugin moved a ref
into or out of one of its cells), `one_way_or_not_swapped_edges` (a neighbour pair that is not reciprocal or whose shared edge is not the swapped
edge), `stale_or_broken_links` (a link to a triangle beyond the target mesh or to a mesh that does not exist), `owner_less_links`, `kind3_portals`
(enable/disable portals), `small_main_component` (the largest walk component holds under `--min-main-share`, default 0.6, of the walkable area).
`library` prints the count per reason, as the first reason hit and as any reason.

**2. Coverage.** Each surviving place gets measured tags (`coverage.py` TAGS, one table; thresholds are the top quartile of the vanilla pool,
measured separately for interiors and exterior windows because they differ by an order of magnitude): `interior`, `exterior`, `slope_flat`
(share of area under 15 degrees >= 0.85), `slope_gentle` (15-30), `slope_steep` (30-45), `slope_cliff` (45+), `stairs_run` / `stairs_long` (rise of
the biggest connected run of triangles steeper than 25 degrees), `drops_low` / `drops_mid` / `drops_high` (kind 2 links by drop height: under 100,
100-300, over 300 units), `narrow`, `doorway_portals` (3+ door triangles), `multi_floor` (layering), `large_open`, `clutter_dense` (small things
to loot or bump into per million units^2), `occluders_dense` (large statics and trees), `cover_marked` (triangles with cover bits), `water_edge`,
`water_large`, `islands` (walk components apart from the main one, 30+ triangles), `corridors` (path-like coarse graph), `dead_ends`, `loops`,
`choke_points` (articulation points) from a coarse graph of 256-unit bins split into floors, `high_ground`, and the distance scale
`scale_short` / `scale_mid` / `scale_long` (longest shortest path of that graph). A pool of up to `--pool` (default 1200) clean places, spread by the
feature-space farthest-point rule, gets the expensive graph tags; the pick then takes, greedily, the place covering the most unmet needs until every
tag and every COMMON tag pair (present together in at least `--pair-share`, default 3 percent, of the pool) is in the library at least
`--coverage-n` (default 2) times or `--count` places are chosen; spare room is filled by farthest-point diversity. The table (pool count, library
count, held-out count, status) and every unmet need are printed and stored in `index.json` (`coverage`, `unmetPairsLibrary`, `unmetTagsLibrary`).

**3. Held-out set.** Same procedure on the pool minus the library and every exterior window within 3 grid cells of a library window, written
to `<set>/heldout/`, so a tuning can be checked on places it never saw.

Class names in the files (`exterior_multilevel_statics`, ...) are the older measured classes (`classes.py`), kept for file names and for the pool
spread. CAVEAT: a class name says what was MEASURED, not what the place is (a "multilevel statics" window is not necessarily a fort, "actors
containers" not necessarily a camp: wildlife counts as actors). Nothing reads a name.

## Water

Triangle bit `0x200` is the navmesh water flag. Measured (exterior meshes, height from the rule below): about 98 percent of flagged
triangles lie below the water height and about 90 percent of triangles below water are flagged. `META.cells[].waterHeight` is
resolved as: the cell's XCLW marker (FLT_MAX and about -2.1e9 mean "not set") -> the worldspace default (WRLD DNAM) for an exterior
cell that has the "has water" bit -> `null` with `waterSource` "none". Cells without the has-water bit have none. The navmesh under
water is walkable (followers wade); treat flagged triangles as a movement cost / speed class, not a wall.

## What plugins alone cannot give

* Real collision meshes (line of sight, projectile blocking, wall thickness): they are `.nif` files in BSAs / loose meshes.
  REFS carry `modelOff` and the base bounds so a later step can fetch them. Until then occlusion is the OBND box.
* Terrain height away from the navmesh (LAND is not exported).
* What scripts decide: which disabled refs get enabled, which doors are locked after a quest.
* Actor stats: use the world json (list_balance) joined by `(plugin, local)` base ids.

## Validation (command `validate`)

Strict decode of every NAVM record (all versions, overridden or not) of a set, then checks over the winning meshes. Numbers from
the last run (`validate-<set>.json` in the cache has the raw counts):

| | Vanilla (5 ESMs) | LoreRim (3501 plugins) | Tuxborn (1484 plugins) |
|---|---|---|---|
| NAVM records / deleted / decoded / failed | 19992 / 68 / 19924 / 0 | 41319 / 59 / 41258 / 0 | 57554 / 78 / 57476 / 0 |
| internal edges, reciprocal by index | 7,649,395, 99.995% | 13,477,532, 99.992% | 16,363,360, 99.995% |
| ... and the swapped edge (same two points, opposite direction) | 99.992% | 99.985% | 99.986% |
| external links of winning meshes | 233,852 | 335,899 | 423,093 |
| ... target mesh present | 100% | 100% | 100% |
| ... a link back from the target | 99.97% | 99.68% | 99.88% |
| ... edge within 4 / within 32 units of the target triangle | 76.5% / 97.5% | 78.4% / 97.6% | 78.6% / 97.5% |
| door flag vs door rows (agree / flag-only) | 2211 / 3 | 6843 / 17 | 8476 / 83 |
| LAND vs navmesh: within 16 units / terrain more than 16 above the navmesh | 51.0% / 1.99% | 51.5% / 2.99% | 53.5% / 2.18% |
| water flag: flagged below water height / below water flagged | 98.3% / 91.2% | 98.5% / 89.8% | 96.5% / 90.3% |

LAND check: the decoded terrain is compared with the navmesh vertices of 300 sampled exterior meshes per set; the navmesh sits on terrain, rocks and
buildings, so the terrain is mostly at or under it; the 2 to 3 percent where it rises more than 16 units above are navmesh meshes edited by mods or on
steep cell corners. The axis and sign conventions of the decode were tested against alternatives (transposed, flipped), which are 4 to 5 times worse.

"Consumed size equals the subrecord size and every vertex / neighbour / link / door / cover / grid index is in range" holds for
100 percent of records. Exceptions, explained: the one-way neighbours and the 30-odd links without a link back are what the game
data contains (the validator counts, it does not repair); 36 vanilla link rows name a target triangle beyond the target mesh
(stale: the neighbour was edited later) and are flagged `LINK.flags` bit0 in the files; link rows no edge points at are flagged
bit1 (0 in all three sets); the links farther than 32 units are kind 1/2 links inside one mesh or cell (a ledge or drop need not be
geometrically adjacent) and a few cell-border seams. The 3 / 17 / 83 flag-only door triangles are left as the game has them.
There is no independent NVNM reader on this machine (no xEdit for Skyrim, no pyffi, no Mutagen), so the cross-check is the data's own
redundancy: bytes consumed exactly, door bit vs door rows, link bits vs link rows, reciprocal neighbours with matching geometry,
water bit vs water height, link kind pairs 1<->2 (2350 of 2351 in vanilla). `validate FILE.mfoenv` checks written environments (ranges,
swapped-edge neighbours, door ref ids, bounds, component table); with no argument it checks every file in the cache. The unit tests
(23) include the C++ reader under ASAN/UBSAN.


## Open items

* DREF `openDefault` reads XACT bit 0x8 (community docs; one data point: 29 vanilla doors carry XACT 0x0D). Not observed in game.
* Reject-everything-anomalous removes about half the candidate places of the big lists (mostly `plugin_moved_refs` and one-way edges): the library
  shows no mod-heavy places that carry those. Retune `--min-main-share` or drop a reason in `coverage.py` if a list needs them.
* Coverage thresholds (`coverage.py` TAGS) are vanilla top quartiles; lists with small `--count` leave some tag pairs unmet (printed and stored).
* The coarse graph (256-unit bins split by floor) is a proxy for dead ends, loops and choke points.

* Link kinds 1/2/3 are decoded from data direction and the community enum only; no in-game observation backs them.
* Triangle bits 0x10, 0x20, 0x40 are not measured.
* Classification thresholds were set from the vanilla feature distribution; the class names are proxies (see the caveat).
* A ref a script moves or enables at runtime cannot be known.
* Reachability uses the door refs the scan kept (cells with a navmesh and their neighbours), so an interior reached only through
  an exterior cell without a navmesh is treated as unreachable.
* Exterior environments are a 3x3 grid-cell window (about 12000 units square); towns bigger than that are cut.
* No independent NVNM reader is installed here (no xEdit for Skyrim, no pyffi, no Mutagen); validation relies on the data's own
  redundancy (see above).
