# envextract: real Skyrim environments (navmesh included) for the IAUS simulator

Offline tool. Reads plugin files only (read-only), writes one binary file per environment under
`~/.cache/mfo-env/<set>/` (never in git: it is game-derived data). Python 3 standard library only.
The consumer is `tools/iaus_sim`; this README is the contract. STATUS: format v1, see "Open items" at the end.

## Commands

    envextract.py scan     --set Vanilla|LoreRim|Tuxborn        # read plugins -> ~/.cache/mfo-env/_scan/<set>.pkl
    envextract.py library  --set NAME [--count N] [--seed S]    # classify + select + write the .mfoenv files + index.json
    envextract.py sample   --set NAME --class C --seed S --count K   # draw MORE environments of a class (seeded)
    envextract.py svg      FILE.mfoenv [OUT.svg]                # top-down picture to eyeball
    envextract.py validate [--set NAME | FILE ...]              # NVNM self-check over every navmesh / env files
    python3 -m unittest discover -s tools/envextract/tests      # synthetic-record tests, no game data needed

Sets: `Vanilla` = the five base ESMs of the LoreRim Stock Game Data folder. Lists are resolved from the list's own
`ModOrganizer.ini` (`gamePath`), `profiles/<profile>/{plugins,modlist}.txt` (no filesystem-wide search).

## Output layout

    ~/.cache/mfo-env/index.json                       all sets, every environment, class, size, features
    ~/.cache/mfo-env/<set>/<set>.<class>.<id>.mfoenv  one environment each
    ~/.cache/mfo-env/_scan/<set>.pkl                  scan cache (tool-internal)

`index.json`: `{"formatVersion":1,"sets":{"Vanilla":{"plugins":[...],"envs":[{"file","class","id","interior",
"tris","verts","areaUnits2","cells":[...],"features":{...}}]}}}`.

## File format `MFOENV` v1 (little endian, 4-byte aligned, no dependencies)

    u8  magic[8]   = "MFOENV\0\0"
    u32 version    = 1
    u32 nSections
    then nSections x { char tag[4]; u32 offset /*from file start*/; u32 count; u32 recSize; }

Look sections up by tag; unknown tags must be ignored (append-only evolution; a layout change bumps `version`).
A header-only C++ reader with these structs is in `envfile.h` (this directory).

Ids: every FormID in the file is `u32 id = (plugIdx << 24) | local24`. `plugIdx` indexes the PLUG table of
plugin names (the plugin that OWNS the record, i.e. already resolved through masters, light plugins use
`local <= 0xFFF`). This is NOT the in-game FormID (that depends on the load order); to join with
`list_balance/*.world.json` keys use `(lowercase plugin name, local)`. 0 means none.
Units: game units (1 unit = 1/70 m = 1.4 cm). Exterior: absolute worldspace coordinates. Interior: cell space.
Rotation: radians exactly as stored in the REFR (x,y,z Euler, game convention).

| tag | recSize | content |
|-----|---------|---------|
| META | 1 | UTF-8 JSON (count = bytes): name, set, class, interior, source plugins, per-cell info (id, edid, grid, water height, flags), features, tool version |
| STRS | 1 | string pool, NUL terminated strings; other sections refer to a string by byte offset (0 = none, offset 0 holds an empty string) |
| PLUG | 4 | `u32 nameOff` per plugin, lowercase file names |
| MESH | 72 | one per navmesh, see below |
| VERT | 12 | `f32 x,y,z`, all meshes concatenated |
| TRIS | 16 | `u16 v[3]; i16 nb[3]; u16 flags; u16 cover` |
| LINK | 20 | external edge links |
| DOOR | 20 | door triangles |
| REFS | 128 | placed references |

MESH: `u32 id; u32 cellId; u32 worldId; i16 gridX, gridY /*0x7FFF if interior*/; u32 vertBase, vertCount, triBase,
triCount, linkBase, linkCount, doorBase, doorCount; f32 min[3], max[3]`. `cellId` = interior cell, or the
exterior cell's own FormID when it exists in the plugins. Triangle vertex indexes are LOCAL to the mesh (add
`vertBase` to index VERT; `triBase` for TRIS, ...). Tris are 3 vertices wound as stored in the game.

TRIS: `nb[k]` is the neighbour triangle (index local to the mesh) across edge k, or -1. Edge 0 = v0-v1, 1 = v1-v2,
2 = v2-v0. An EXTERNAL edge (the edge leads into another navmesh) has `nb = -1` here and a row in LINK. Edge
neighbours inside one mesh are reciprocal (the validator reports the exceptions). `flags` are the game's raw
bits; the ones relied on and how they were checked (validate.py, over all vanilla navmeshes):
`0x1/0x2/0x4` edge 0/1/2 is external; `0x8` deleted triangle; `0x10` no large creatures; `0x20` overlapping;
`0x40` preferred path; `0x200` water (see "Water" below); `0x400` door triangle (every one has a DOOR row and
every DOOR row has the bit: 1680 of 1680 in vanilla); `0x800` set on 96 percent of triangles, meaning unknown
(called "found" by community docs), do not rely on it. `cover` = raw cover bits (unused by the sim so far).

LINK: `u32 meshIdx /*owner, index into MESH*/; u16 tri; u8 edge; u8 kind /*raw u32 'kind' of the NVNM row; 0 on
cell borders, 1 or 2 on interior mesh-to-mesh links; meaning not pinned down*/; u32 targetMeshId; i32 targetMeshIdx
/*index into this file's MESH or -1 when the other mesh was not included*/; u16 targetTri; u16 pad`.

DOOR: `u32 meshIdx; u16 tri; u16 pad; u32 crc; u32 doorRefId; i32 refIdx /*index into REFS or -1*/`.
The triangle is the one the door opens onto; a path through the door continues on the far side via the
door's teleport destination (REFS.teleportRef), which is another environment's door.

REFS (128 B):

    0 u32 id        4 u32 base      8 u32 cellId
    12 u8 kind      13 u8 baseSig   14 u16 flags
    16 f32 pos[3]   28 f32 rot[3]   40 f32 scale
    44 f32 boundsMin[3] 56 f32 boundsMax[3]   world-space AABB of the base object's OBND, placed (scale + rotation); 0 if the base has none
    68 u32 key (lock key item)  72 u8 lockLevel (raw XLOC level: 0..99 easy..very hard/ 254 requires key / 255 inaccessible)  73 u8 lockFlags
    74 u16 count (XCNT, items)
    76 u32 teleportRef (destination door REFR)  80 f32 tpPos[3]  92 f32 tpRot[3]  104 u32 tpFlags  108 u32 tpCellId (0 unless resolvable)
    112 u32 enableParent   116 u32 baseNameOff (base EDID)  120 u32 modelOff (base model path, the way to fetch real collision later)  124 u32 reserved

`kind`: 0 door, 1 container, 2 activator, 3 furniture, 4 loose item, 5 actor spawn, 6 static (large only), 7 flora, 8 tree (large only).
`baseSig`: 1 DOOR 2 CONT 3 ACTI 4 FURN 5 MISC 6 WEAP 7 ARMO 8 AMMO 9 BOOK 10 ALCH 11 INGR 12 KEYM 13 SCRL 14 SLGM 15 NPC_ 16 LVLN 17 STAT 18 MSTT 19 SCOL 20 FLOR 21 TREE 22 APPA.
`flags`: bit0 persistent, bit1 leveled actor (base is LVLN), bit2 has enable parent (XESP), bit3 locked, bit4 teleport (XTEL),
bit5 enable parent inverted, bit6 reserved.
Actor spawn points are the placed ACHR records. Whether the actor is hostile, a bandit, a draugr is a property of
the base record (see the world json); this file only says which base and whether it is a leveled list.
"Large static" = the larger side of the scaled OBND box is at least `--min-static` units (default 256);
small clutter is not exported (occlusion only). Only refs whose winning record is not deleted and not
initially disabled are exported (counts in META).

## What the extractor resolved (override rules)

Plugins are walked in load order (implicit masters, Creation Club files from Skyrim.ccc, then the profile's `*`
active plugins). For navmeshes, cells and refs the LAST record of a FormID wins whole (records override whole,
not per subrecord). A record with the deleted flag (0x20) removes the thing; with initially disabled (0x800) it is
dropped (a script may enable it in the game; plugins alone cannot say). Compressed records (flag 0x40000) are inflated.
Light plugins (ESL flag or .esl) resolve with 12-bit local ids. A ref moved to another cell by a later plugin follows
its winning record's cell. Persistent exterior refs are placed into the grid cell their position falls in.

## How the library is picked (measured geometry, no names, no list-specific ids)

Every candidate (each interior cell with a navmesh; each exterior 3x3 grid-cell window) gets features from the
navmesh rasterised at 64 units: area, bounding box, vertical span, LAYERING (fraction of raster cells holding two
walkable layers more than 120 units apart: stairs, towers, ledges, bridges), NARROW (share of walkable area with
local width under 200), median width (twice the distance to the nearest non-walkable raster cell), STEEP (area
with surface normal more than 31 degrees off vertical), door count, plus placed-ref counts (actor spawns, doors,
containers, large statics). `classes.py` holds ONE rule table mapping features to a class; the classes are:
`interior_house`, `interior_narrow`, `interior_cave_ledges`, `interior_ruin_halls`, `interior_hall_open`,
`interior_tower_stairs`, `exterior_town`, `exterior_fort`, `exterior_camp`, `exterior_ruin`, `exterior_hills`, `exterior_open`.
Within each class the library takes the members farthest apart in feature space (greedy farthest-point, seeded
start); `sample` draws more with a given seed from the same candidate index. To add a class or retune a
threshold edit the table in `classes.py`; nothing else changes.

### Class rules as shipped (`classes.py`, first match wins; candidates in the vanilla masters)

| class | rule (features, units) | vanilla candidates |
|---|---|---|
| interior_tower_stairs | layering >= 0.15 and area < 3.0e6 (small, several floors) | 106 |
| interior_house | area < 1.5e6, layering < 0.15, < 8 actors | 217 |
| interior_cave_ledges | z span >= 600, steep >= 0.04, area >= 1.5e6, layering < 0.08 | 159 |
| interior_ruin_halls | layering >= 0.05 and area >= 3.0e6 (large, multi level) | 82 |
| interior_hall_open | wide share >= 0.02, area >= 1.5e6 | 38 |
| interior_narrow | narrow share >= 0.9, area >= 1.5e6 | 72 |
| interior_rooms | rest | 71 |
| exterior_town | >= 5 doors with a teleport destination in the window | 282 |
| exterior_fort | layering >= 0.004 and >= 10 large statics | 826 |
| exterior_camp | >= 3 actors, >= 4 containers+furniture, < 2 teleport doors, layering < 0.004 | 2024 |
| exterior_ruin | >= 1 teleport door and >= 40 large statics | 1205 |
| exterior_hills | steep >= 0.22 | 445 |
| exterior_open | steep < 0.22, wide share >= 0.10 | 2250 |
| exterior_rough | rest | 315 |

Exterior candidates overlap (one window per navmesh cell); the library keeps chosen windows at least 3 grid cells
apart. The vanilla masters contain test and developer cells (for example DLC test cells); they classify like any
other cell because nothing here looks at names. Library as built: Vanilla 41, LoreRim (profile Ultra, changed-only:
windows or cells where a mod supplied a navmesh) 14, Tuxborn (profile Tuxborn - Deck, changed-only) 14.

## Water

Triangle bit `0x200` is the navmesh water flag. Measured on the vanilla masters (exterior meshes, cell water height
from the CELL `XCLW`, else the worldspace default): 98.3 percent of flagged triangles lie below the water height and
91 percent of all triangles below water are flagged. The CELL's `waterHeight` (META, per cell; null or about
-2.1e9 means none) is the surface for swimming and wading; the navmesh under water is still walkable ground
(followers wade, they swim only when the player does), so the sim should treat flagged triangles as a movement
cost / speed class, not a wall.

## What plugins alone cannot give

* Real collision meshes (line of sight, projectile blocking, actual wall thickness): they live in the `.nif` files inside
  the BSAs / loose meshes, not in the plugins. REFS carry `modelOff` (and the base bounds) so a later step can fetch them.
  Until then occlusion is the OBND box of each large static.
* Terrain height under non-navmesh ground (LAND records are not exported); the navmesh surface is the only ground.
* Behaviour that scripts decide: which disabled refs get enabled, which doors are really locked after a quest.
* Actor stats: use the world json (list_balance) joined by `(plugin, local)` base ids.

## Validation (command `validate`)

Vanilla masters (Skyrim, Update, Dawnguard, HearthFires, Dragonborn), run on the full NAVM record set:

* 19992 NAVM records, 68 deleted, 19924 decoded, 0 failures: consumed size equals the subrecord size for every
  one, every vertex / neighbour / link / door / cover / grid index in range.
* Door flag `0x400` against door rows: 2211 agree, 3 triangles carry the flag without a door row (left as is).
* Internal neighbours: 7,649,395 internal edges; reciprocal by index 99.995 percent, and the shared edge has the same
  two vertex positions (0.5 unit) on both sides in 99.992 percent. The rest are one-way neighbours the game
  data itself contains (the validator counts them, it does not repair them).
* External links of the winning meshes: 233,852. Target navmesh exists 100 percent; a link back from the target
  exists 99.97 percent (30 rows have none, 36 point at a target triangle number beyond the target mesh: stale
  links left by an override of the neighbour). Geometry: the linked edge is within 4 units of the target
  triangle for 76 percent and within 32 units for 97.5 percent (cell-border meshes are not vertex-snapped; the
  rest are kind 1/2 mesh-to-mesh links inside a cell, which need not be geometrically adjacent).
* No independent NVNM reader is installed on this machine (no xEdit for Skyrim, no pyffi, no Mutagen), so the
  cross-check is the data's own redundancy: size consumed to the byte, door bit versus door rows, link bits versus
  link rows, reciprocal neighbours with matching geometry, water bit versus water height. Layout from the
  community-documented NVNM version 12; the one field not understood is the link `kind` (0 on cell borders, 1 or 2
  on interior links).
* Re-run: `envextract.py validate --set Vanilla` (about a minute); stats are also written to
  `~/.cache/mfo-env/validate-<set>.json`. `validate FILE.mfoenv` checks written environments (ranges, one-way
  neighbours, door ref ids, bounds); with no argument it checks every file in the cache.

## Speed

Vanilla: scan 20 s (two passes, 6 processes), candidates plus features 90 s (Tamriel is rasterised once at 64 units,
windows are slices), writing 40 environments 10 s. A load order of 3500 plugins scans in about 10 s (the
walker skips what it does not need).

## Open items

* Cell-level `kind` of NVNM links is not decoded; `LINK.kind` is the raw value.
* A ref moved by a later plugin to a different cell follows its winning record's cell; refs a script moves or
  enables at runtime cannot be known.
* Classification thresholds (`classes.py`) were set from the vanilla feature distribution; "camp" and "fort" are
  measured proxies (actors + containers/furniture in open ground, multi-level structure with large statics), not
  a statement that the base actors are bandits.
* Actor spawns do not carry encounter zone / level data; join the base id with the world json.
* Exterior environments are a 3x3 grid-cell window (about 12000 units square); towns bigger than that are cut.
