# Board-Extension API — scoping (third-party tabs & panels in MFO's UI)

**Status: DESIGN / SCOPING (not built). 2026-08-17.**

Goal (marth): let third parties add their **own tabs and features into MFO's
in-game board**, with their own logic, and have it documented so people can do
what they want. This document scopes what that actually takes, the options and
trade-offs, and a recommended phasing. It is a design, not a contract — the
frozen contract is authored per tier when that tier is built.

---

## 1. The hard constraint (why this is different from the ESL/MCM APIs)

The existing addon surfaces are **data**:
- **Progression data** (classes, economy) — declared in an ESL, read by MFO
  (`ADDON-API.md`).
- **MCM tabs** — a `config.json` + a registration quest, rendered by MCM-Helper.
  These are third-party-ownable **from an ESL today**.

The board is **not data**. MFO's board is **ImGui, drawn on MFO's render thread
inside `MFO.dll`** (D3D11 Present hook → `NewFrame` → `BeginTabBar("##tabs")` →
per-tab draw → nav arbitration → Present; see `Board.cpp`). ImGui panels are
**live C++ calls against a live ImGui context** — they cannot be expressed as
records and cannot ship inside an ESL. So a board extension is fundamentally a
**code plugin** (a companion SKSE DLL) OR a **declarative spec MFO interprets**.
There is no third path where an ESL alone paints new ImGui.

This gives two tiers, for two audiences.

---

## 2. Tier 1 — Declarative panels (ESL / JSON, no C++)

For authors who want a **settings / info / simple-action panel** without writing
code. The addon **declares** a panel; MFO's board **interprets** it and draws
generic ImGui. This is "MCM-Helper, but inside the board."

**Registration:** via the existing manifest sentinel (`ADDON-API.md`) — the
addon adds ONE board-panel record (a small JSON blob in a record, or a
`Config/Board/<modName>/panel.json` file MFO reads on detection, mirroring the
MCM-Helper file convention).

**Widget vocabulary (the whole point — and the whole limit):** MFO implements a
fixed set; the addon can only use what MFO ships.
- `text` / `heading` — static or bound to a global / Papyrus property (live read)
- `slider` / `toggle` / `stepper` — **bound to a GlobalValue** (read/write), same
  binding we built for the MCM economy sliders
- `button` — on press, `SendModEvent`s a named event the addon's Papyrus listens
  for (that is how "their own logic" runs — a Papyrus handler, not C++)
- `list` / `keyvalue` — rows sourced from a FormList or a Papyrus-filled array
- `separator` / `spacer` / `group`

**Data & logic binding:**
- Reads/writes: GlobalValue (immediate) and Papyrus properties (polled).
- Actions: button → mod event → the addon's Papyrus quest script does the work
  (grant an item, toggle a mechanic, open its MCM, etc.).

**Pros:** ESL-only (no DLL, no ABI), zero crash surface into MFO (MFO owns every
draw call), reuses the manifest + GlobalValue + Papyrus-event machinery already
shipped. **Cons:** bounded to MFO's widget set; no custom rendering, no
per-frame logic, no novel layouts.

**This covers ~80% of what most addons want** (a tab of settings + a few
actions) and is the natural extension of the existing ESL addon API.

---

## 3. Tier 2 — Native panels (companion SKSE DLL, full power)

For authors who want **arbitrary ImGui** — custom rendering, live per-frame
logic, novel layouts. The addon ships its **own SKSE DLL** that registers a tab
with MFO and draws it every frame. This is the "do anything" tier, and it is
**C++, not ESL** — unavoidably, because arbitrary UI is code.

**Registration — the `MEO_API.h` pattern (precedent already in this codebase):**
a versioned, SKSE-messaging, append-only C++ interface.
1. On board init, MFO dispatches `kMessage_BoardReady` carrying an
   `IMFOBoard*` (ABI-versioned).
2. The addon (in its own SKSE messaging handler) calls
   `board->RegisterTab({ name, abiVersion, renderFn, userdata })`.
3. MFO draws a `BeginTabItem(name)` and, inside it, calls `renderFn(userdata)`
   on the render thread.

**The ImGui-across-DLL problem (the core technical risk).** Two forms exist and
must be a deliberate choice:
- **(a) Shared raw context — max power, ABI-fragile.** MFO hands the addon its
  `ImGuiContext*` + allocator funcs; the addon calls ImGui directly. Requires
  the addon to link the **exact same ImGui version** as MFO — a hard lock-step
  that breaks silently on any bump. Viable only if MFO pins ImGui and documents
  the version as part of the frozen contract.
- **(b) Stable C draw shim — ABI-safe, bounded (RECOMMENDED default).** MFO
  exposes drawing as a **versioned C interface** (`Text`, `Slider`, `Checkbox`,
  `Button`, `BeginChild/EndChild`, `SameLine`, `Image`, …) that the addon calls;
  MFO translates to ImGui internally. Append-only, frozen — exactly the
  `MEO_API.h` discipline. Trades vocabulary for immunity to ImGui version drift.
  A power-user opt-in `GetRawImGui()` can still hand out (a) with a giant
  "unstable, you pinned our ImGui version" warning.

**Render contract (non-negotiable, from the board's real threading):**
- `renderFn` runs on the **render thread**, between `NewFrame` and `Present`.
- **No actor / form / gameplay access from `renderFn`** — that is main-thread
  only (the standing rule). The addon reads game state on the main thread (its
  own SKSE task) into a snapshot, and `renderFn` draws the snapshot. MFO can
  expose a `PostToMainThread(fn)` convenience.
- `renderFn` must be fast and **exception-safe**; MFO wraps every addon draw in
  a guard so a throwing/crashing addon **disables its own tab and logs**, never
  takes down MFO's board (failure isolation is a hard requirement).
- **Input/nav stays MFO's.** MFO owns board open/close, the B-to-close, and
  tab switching + gamepad focus arbitration (the XInput-double-path lesson —
  input is delicate). The addon draws within its tab and may query nav state via
  the interface; it does not own the input hook.

**Lifecycle & state:** the addon owns its own persistence (its own co-save /
Papyrus). MFO stores nothing for it. Tab visibility can gate on the addon's own
detection. `kBoardABIVersion` is checked at register time; a mismatch refuses
the tab with a log line — never a crash. Every method is **forever** once
shipped (append-only), like `MEO_API.h` and `Forms.h`.

**Pros:** unlimited. **Cons:** C++/DLL barrier; the ABI-maintenance burden is
permanent; UI can't be CI-tested (in-game only); a whole new frozen contract to
keep.

---

## 4. Recommended phasing

1. **Phase 1 — Tier 1 declarative.** Highest value per effort: it lets ESL
   authors add a settings/action tab with **no code**, reuses the manifest +
   GlobalValue + Papyrus-event plumbing already shipped for the MCM tab, carries
   **zero ABI risk**, and covers most real requests. Frozen as a section in
   `ADDON-API.md`.
2. **Phase 2 — Tier 2 native DLL API**, built on the `MEO_API.h` messaging
   pattern with the **stable C draw shim** as the default surface (raw-context
   as an explicit unstable opt-in). Ship when a concrete power-user need exists;
   it commits MFO to a second forever-contract, so it should be demand-driven.

Both tiers register through the **same addon family** — Tier 1 via the manifest
sentinel, Tier 2 via a parallel `kMessage_BoardReady` handshake — so it reads as
one coherent ecosystem, documented together in `ADDON-API.md`.

---

## 5. Open questions to settle before building either tier

- **Widget set (Tier 1):** exactly which controls ship v1 — the vocabulary is a
  frozen contract, so start minimal and append.
- **ImGui pinning (Tier 2):** does MFO commit to the stable-C shim only, or also
  publish its ImGui version for raw-context power users? (Recommend shim-only
  first.)
- **Main-thread bridge:** the shape of `PostToMainThread` / the snapshot
  convention MFO offers addons so they never touch actors on the render thread.
- **Crash-isolation guarantees:** how hard MFO sandboxes an addon draw (try/catch
  is not a full sandbox against memory corruption — document the honest limit).
- **Discovery UX:** how the board surfaces "this tab came from addon X" and what
  happens when an addon is removed mid-save.
- **Testing:** UI is in-game-only; define a manual acceptance checklist since CI
  can't cover it.

---

*Nothing here is a contract yet. When a tier is built, its exact records /
interface are frozen and moved into `ADDON-API.md` with the append-only
discipline `MEO_API.h` and `Forms.h` already follow.*
