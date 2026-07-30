# Banked design — navmesh-snap router + stall-buster (NOT built)

Status: **shelved, ready to implement** if a real walk-to-loot freeze is ever
proven in a deck log. As of v0.8.12 the pragmatic router (travel package +
navmesh GATE that skips off-mesh bodies + HasLoot pre-filter + no-progress
giveup) works well (deck 2026-07-30: 4/4 dispatches arrived and looted). Do not
build this speculatively — pull it off the shelf only against evidence.

Header facts below are byte-verified against the CI CommonLibSSE-NG clone.

## Piece A — snap routing (reach off-mesh bodies, don't skip)
- **A1 nearest-navmesh POINT** (not just distance): project the ref position onto
  each `BSNavmesh::triangles` face (Ericson closest-point-on-triangle), skip
  `TriangleFlag::kDeleted`, guard vertex indices. Types: `BSNavmesh::triangles`
  (BSTArray<BSNavmeshTriangle> @0x028), `BSNavmeshTriangle{vertices[3], triangles[3],
  triangleFlags}` sizeof 0x10, `BSNavmeshVertex::location` NiPoint3. Same cell
  access + spinLock as the shipped `NearestNavmeshDist`. Budget 16k triangles,
  early-out at 32u. (Nearest-VERTEX is NOT enough once we route — a mid-triangle
  corpse parks him outside kArrivalDist.)
- **A2 destination = one persistent XMarker moved to the snap point**, filled into
  alias 1 (PLDT has no raw-XYZ form — 12-byte type/value/radius only). ESP: add an
  interior `MFO_HoldingCell` + a persistent (flag 0x400) `MFO_LootGoal` REFR with
  NAME=XMarker 0x3B (exemplar: po3 FollowersCanLoot's holding cell/chests). Move
  it via **VM-dispatched `ObjectReference.MoveTo(lootRef, dx,dy,dz, false)`** —
  re-parents + offsets in one engine-scheduled call; the VM queue is MFO's
  main-thread-equivalent lane for engine mutations under §0.32. New
  `TravelPhase::Placing` (compute snap → dispatch MoveTo → functor sets an atomic →
  next tick fills alias 1 with the marker). Arrival relaxes to
  `dist(follower,corpse)<=kArrivalDist || dist(follower,snapPoint)<=~96`; LootHere
  is a native transfer that works at the standoff. Sanity cap `g_snapMax` (~512).
- **A3 lifecycle**: one shared marker (single-excursion by construction); moved
  position + alias fill in a save are inert (ReleaseAll neutralizes on load); no
  cleanup (invisible collisionless marker).

## Piece B — stall-buster (the 2011 corridor-empty stall)
- The corridor internals are UNDECODED (`MovementControllerNPC` all Unk_*; Keep
  Up's hook is closed-source). A vfunc hook = original RE, research-only.
- **Use the TICK-DRIVEN KICK instead (no offsets):** escalate in the Walking
  phase before the giveup — (1) soft-stall ~1.5s no-progress → `EvaluatePackage`
  (forces a fresh path request = Keep-Up "clear+rebuild" from above); (2) +1.5s →
  nudge the marker ±32u toward the follower + EvaluatePackage (a *changed* goal
  beats same-goal dedup); (3) kNoProgress/deadline → existing blocklist.

## Build order if resumed
1. A1 (pure math, log snapDist to calibrate). 2. B kicks (~30 lines, free, feeds
A2's deck cycle). 3. A2 (ESP regen, Placing phase, VM MoveTo). Probes P1–P4 each
one deck cycle with a readback (marker lands; moved-marker re-plans; kick restores
pathSpeed; loot-from-standoff UX). Everything uses public decoded API; the only
private temptation (`MoveTo_Impl`) is avoided by the VM MoveTo path.
