# SPEC — Forced self-cast (self-concentration & self fire-and-forget)

Status: QUEUED (alongside the 1.5.97 cast-target crash). Intended to be built by a
**worktree-isolated agent** briefed with this file, to keep the heavy reads
(Packages.cpp 3278, ESP gen, probe/measure logs) out of the main context.
Written 2026-08-17 after v1.0.64.

## Problem

A follower's `act.cast_self` gambit for a **concentration** spell (a self-heal
like *Healing*, or a **self-ward**, or any self-channel a user wants to author)
is **declined every tick**:

```
[eval] Lucien ... rules 0(concentration self-cast unreachable (package self route barred)) skipped -> ...
```

`ConcentrationCast` bars `self` (Actuation.cpp ~240), and `Packages::Begin`
returns `Decline::SelfRoute` for a null target (Packages.cpp ~689-707). The
**self-heal marth sees working in-game is the follower's AUTONOMOUS vanilla AI**,
not an MFO gambit — so authored self-concentration gambits silently do nothing.

### Why the route is barred (do not naively unbar it)

MFO channels a bounded cast via the AI package `MFO_CastPackage`, which aims at a
**target alias** and therefore carries an authored **QNAM** (names the command
quest; authored Target is t4 → alias 1). A self-cast makes the engine write
**targType 6** into that same record → the **QNAM + t6** combination, which is the
**rev-4 CTD's surviving zero-precedent suspect** (ENGINE_NOTES §0.22). Probe-6
proved a clean t6 self-cast works **only on a record with NO QNAM**. So unbarring
the *existing* package for self = arming the suspected-CTD shape. This is a
deliberate safety bar, NOT a design choice against self-casts.

### Why "AI-first" (the target-cast hybrid) is INSUFFICIENT here

The target casts use AI-first ("held off: waiting for their AI to cast it", force
on miss). For self-concentration that fails the actual use case:
- **It can't force the CHOSEN spell.** The AI casts *its* pick, not the gambit's
  (the `cast-gambit-spell-choice-not-enforced` problem). A ward the AI never
  volunteers never goes up. Only works for spells the AI already casts — defeats
  the point of authoring the gambit.
- **It can't guarantee timing.** Detect-miss-then-force is too slow for a reactive
  window (marth: "enemy is power-attacking, do we respond in time?").

## Goal

FORCE the **exact chosen** self spell (concentration or fire-and-forget),
**promptly**, and **bound** it for the allowed duration — independent of what the
follower's AI would choose. Covers self-heal, self-ward, and arbitrary
user-authored self-spells.

## Design — dedicated no-QNAM self package (path "B")

1. **New package record `MFO_CastPackageSelf`** authored in `MFO_GenerateESP.py`
   (ESP is generated from source — never CK/xEdit; see
   `esp-without-xedit.md` / ARCHITECTURE §8): targType 6 (self), **NO QNAM** (the
   proven-clean probe-6 shape). New FormID in `Forms.h` (frozen-contract check +
   `MFO_GenerateESP.py` mirror). No co-save/alias-fill change.
2. **`Packages::Begin` self route**: instead of `Decline::SelfRoute`, fill/run the
   self package for `a_target == nullptr`. Reuse the existing `CastHold` bounding
   (holdSeconds, cooldown, mid-stream watches) — a self-cast needs NO line-of-fire
   gate (nothing to friendly-fire), so the ffWatch is skipped; heal/utility holds
   still apply.
3. **`ConcentrationCast` self branch** (Actuation.cpp ~240): route to the self
   package instead of returning FailedOther. Same latch/cooldown/exact-bounding
   discipline as the target stream.
4. **Fire-and-forget self** already works; verify the concentration change does
   not regress it.

## Reactivity (marth's power-attack question) — an ACCEPTANCE TEST, not an assumption

- Detection is fine: the combat scan runs every **133ms** (~7.5Hz); a power-attack
  windup is ~0.7–1.2s.
- The bottleneck is the spell's own cast-start (~0.3–0.5s to raise a ward). Against
  a slow 2H windup we win; against a fast attacker it's marginal.
- **Robust pattern:** author slightly *proactive* — "ward **while** a foe is in
  melee range / attacking" — and hold it as a bounded stream (hold-while-relevant).
  The ward is already up through the window instead of racing each windup. This is
  native to the concentration-hold model.

## Acceptance tests (deck-verified; measurement is explicit)

1. Authored `cast_self` **ward** gambit ("ward while foe in melee") RAISES and
   HOLDS the ward for the bounded duration; releases when the condition goes false.
   Log shows `[cast] ... CONCENTRATION <ward> ... stream, hold X.Xs` — NOT the
   `self route barred` decline.
2. Authored `cast_self` **self-heal** (concentration *Healing*) channels and heals;
   releases on heal cap / condition false.
3. **LATENCY MEASUREMENT (hard gate):** instrument force-dispatch → spell-up ms;
   measure reactive ward vs a real power attack on the deck. Report which windows
   we win (by weapon class). Document the number — do not ship a vague "usually".
4. **No CTD:** probe the no-QNAM t6 self package IN ISOLATION on the deck FIRST
   (the whole reason it was barred). Only wire `Begin`/`ConcentrationCast` after the
   probe is clean. This is the #1 risk.
5. Exact cast-control invariant preserved (no unbounded self-channel leaks; the
   `exact-bounding-covers-all-spells` rule still holds — self bounds via holdSeconds
   + cooldown + latch DENY).

## Files

- `MFO_GenerateESP.py` — new `MFO_CastPackageSelf` record + FormID; regen ESP.
- `native/Forms.h` (+ `.cpp`) — new package form lookup.
- `native/Packages.cpp` — `Begin` self route (~684-707), `CastHold` for self.
- `native/Actuation.cpp` — `ConcentrationCast` self branch (~224-244).
- `native/CasterConsent.cpp` — confirm the latch DENY covers the self stream.

## Risks

- **The CTD cell (P0).** Probe no-QNAM t6 self in isolation before wiring. If it
  still CTDs, fall back to a fundamentally different mechanism (direct
  `ActorMagicCaster` channel + timed interrupt, no package) — but that path also
  needs its own bounding proof.
- ESP change → frozen-contract review (`Forms.h` ↔ `MFO_GenerateESP.py`).
- One agent per build; worktree-isolate (never two agents on one tree).
