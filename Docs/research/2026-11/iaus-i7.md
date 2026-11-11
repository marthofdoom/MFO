# IAUS card I7: board toggle + Thinking panel (2026-11-10)

Branch `feat/iaus-i7-board`. Tier B. No co-save change (FLWR bit 3 already exists from I5).

## What shipped
- **Toggle.** Gambits tab header, a button after Helmets: "IAUS combat: On/Off" (`###iausbtn`, `Board_FieldKit.cpp`). It is a `Click::SmallButton`, so d-pad focus and [A] work like the Class and Helmets buttons. It queues `EditKind::SetIausCombat`; `ApplyEdits` writes `FollowerState::iausCombat` on the worker that owns `g_followers`. Default OFF (the `FollowerState` default). Greyed on the Logistics page. The toggle edge is detected by `Scheduler::IausToggleEdge`; the board adds no second handler.
- **Thinking panel.** `DrawIausThinking` (`Board_Iaus.cpp`), shown on the Combat page while the switch is on: the verdict (doing / keeping / idle), rapport rank and time on the action, the top 3 candidates with scores, and each one's consideration breakdown (the `FormatWhy` body). Plain text, no focusable widget.
- **Published view.** None was new: `Iaus::Engine::Publish / ReadReadout` (leaf mutex, copy by value) already existed from I2 and the scan already publishes each lap. Two additions: `Readout::Row::why` (the breakdown) and `ReadReadout`'s optional `a_ageSec` (the panel greys a readout older than 3 s, since a finished fight leaves the last one). `MakeReadout` takes an optional `const Profile*` (null = the uncustomised catalogue the scan uses today).
- **`bIausExplain`.** `[Debug]`, INI-only (like `bAttackObserve` and the censuses, no MCM face), default 0, `Config::g_iausExplain`. Gates the `[iaus-why]` lines; the one wiring line is in `scheduler/IausSource.cpp`'s dtor (was a hard-wired `false`).

## In-game checks
1. Toggle on for one follower, save, reload: the button still says On (bit 3 round trip). Other followers stay Off.
2. Gamepad: d-pad to the button, [A] flips it; no mouse needed.
3. Fight with the switch on: the panel shows a decision, scores move as the fight changes; after the fight it reads "Last decision, N s ago".
4. Switch off mid-fight: the panel disappears and the follower goes back to his gambits (I6 edge).
5. `bIausExplain = 1`: `[iaus-why]` lines follow each `[iaus]` line. `0`: none.
