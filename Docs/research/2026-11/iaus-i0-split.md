# IAUS card I0: the Scheduler combat-scan split (2026-11-10)

Branch `refactor/iaus-i0-combatscan`. Base `2b0b91a` (main), code commit `894f40b`.
Two findings a later split or the I1 author should not have to rediscover.

## 1. The combat scan is not a function

`iaus-design.md` section 9 asks to move "the scan and its reconciliation (`Scheduler.cpp:1583-2230`)". On main
`2b0b91a` that span is INSIDE `Scheduler::Tick` (`Scheduler.cpp:850-2369`): the scan loop
`for (int start = 0; ; )` at `:1705`, the reconciliation at `:1923-2210`, the outcome bookkeeping at `:2211-2360`.
It shares about fifteen locals with the pre-scan part of the lap (`f`, `id`, `it`, `now`, `cannotAct`,
`ownCombat`, `serviceOwnOoc`, `t0`, ...).

A TU split moves whole functions. A span inside a function can only leave its file through an extraction (a new
function, a parameter list, a call), and neither proof tool accepts that: `linecheck` fails on new code lines in a
`.cpp`, and `splitcheck` fails because `Tick` itself differs in the no-optimizer build.

So I0 moved the whole `Tick` to `native/scheduler/CombatScan.cpp`, with the two reconciliation helpers only it uses
(`MeleeClampDwell`, `EquipRangeUndecidable`). Giving the scan its own function is card I1's work: the
`ChoiceSource` seam restructures that loop anyway, and it is a behaviour-level change that gets a behaviour-level
review.

Sizes after the split: `Scheduler.cpp` 495 lines, `scheduler/CombatScan.cpp` 1622, `scheduler/Scheduler_internal.h`
331. `CombatScan.cpp` is still past the ~1500 plan mark because `Tick` alone is 1520 lines.

## 2. Linkage changes codegen at /O2, even with identical inline sites

Any variable two TUs share must have external linkage. Before the split the Scheduler state sat in an anonymous
namespace (internal linkage). MSVC treats an internal, never address-taken global as unaliased, so it may move a
load of it across stores made through a pointer. Once the variable is external it can no longer assume that.

Measured on this split: `ServiceRetreat` is the same 3475 bytes and the same 714 instructions in both builds, but
four adjacent instructions are in a different order. Source (`Scheduler.cpp:256-258` on the branch):

```
note.phase       = RetreatPhase::Stay;
note.stayAt      = now;
note.stayClockAt = g_serviceClock;
```

They are instructions 226-229 of 714. main does the `stayClockAt` pair first (the load of `g_serviceClock` and its
store to `note+0x30`), then the `phase` store (`+0x18`) and the `stayAt` store (`+0x28`). The branch keeps source
order: `phase`, `stayAt`, then the load and the `+0x30` store. These are three stores to separate fields of one
heap `unordered_map` node, which cannot alias the global, and all of it runs on the worker, so the order is not
observable. The no-optimizer proof build is identical for this function.

`splitcheck` reports this as FAIL: its rule is that a function whose inline sites are identical but whose bytes
differ always fails (that rule is what catches a changed constant). It has no class for "same instructions,
reordered because a referenced global changed linkage".

What follows for later splits:

- This is not specific to `inline` variables. `extern` plus a definition in one TU is external linkage too.
- It cannot be avoided by a different pure-move shape unless every reader and writer of the variable stays in
  one TU.
- Expect it whenever file-local mutable state becomes shared state. The earlier waves moved state that was
  mostly already shared, which is probably why it did not show up before.
- It needs either a `--allow` line with the disassembly evidence, or a new explained class in the tool
  (same instruction multiset, the differing run touches a global whose linkage changed, proven at /Od).

Decision for I0 (coordinator, 2026-11-10): accepted with the tool's own `--allow`, one entry scoped to
`ServiceRetreat`, in `tools/splitcheck/allow/iaus-i0.txt` (the evidence is in that file's comment). No reshape, no
tool change.

One object, checked: every shared Scheduler variable has exactly one address in the branch's /O2 and /Od builds
(23 shared ones plus the file-local `g_deadTgt`), and `g_serviceClock` is referenced at that one address from
both TUs (`Tick` in `CombatScan.cpp`; `ServiceRetreat`, `ClearTransientState`, `ServiceClock` in `Scheduler.cpp`).
The Opus tier-3 review of `894f40b` checked the same thing independently in the PDB: 23 shared `g_` variables
with one DATA32 record each, `g_serviceClock` = `S_GDATA32 0003:126960`. Nothing above SEV-3.

## Follow-up

- **Plan a split of `scheduler/CombatScan.cpp`** (1622 lines, past the ~1500 plan mark). It cannot be a pure move
  while `Tick` is one 1520-line function: the first step is giving the pre-scan service, the scan, the
  reconciliation and the bookkeeping their own functions, which is behaviour-level work (card I1 starts it with
  `ChoiceSource`). Not done in I0.

## Results

See the branch's review record for the full tool output. Summary of the /O2 compare of `2b0b91a` vs `894f40b`:
21927 matched pairs, 21895 identical, 7 inline-drift, 20 funclet-drift, 12 outlined, 1 renumbered, 1 FAIL
(`ServiceRetreat`, above; allow-listed as decided). Named data 1819 compared, 0 differing, 0 per-TU copies. No-optimizer proof: PASS,
73420 identical, 0 fail. No co-save function is in any list.
