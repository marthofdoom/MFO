# GAMBIT_FLAIR — low-cost texture: making gambit responses feel human and FFXII

*Suggestion list (design lead pass). No code changed by this doc. Reviewed against
v1.0.11 code: `Scheduler.cpp` (133 ms tick, round-robin, positional suppression),
`Actuation.cpp` (attack latch, cast grace, equip, flee), `Vocabulary.h`,
`Confidence.h`, `Gait.h`, `Rapport.h`, `Board.cpp`, and Docs/GAMBIT_FLOWS.md.*

Every item below obeys the house rules:

- **Byte-identical-when-idle** (DESIGN §4.4): flair only rides on moments MFO
  already acts — a rule firing, combat starting/ending, the player editing the
  board. No new engine calls on an idle follower, ever.
- **No new subsystems, no engine research, no threading hazards.** Everything is
  a handful of lines in existing functions on the main thread, or render-only
  board cosmetics.
- **No thrash, no spam.** Nothing re-fires per tick; log/HUD additions are
  transition-only or off by default; no animation event is sent repeatedly.
- **Legible.** Where flair changes a *timing*, the base knob stays in MCM and the
  variation is small, deterministic, and explainable in one sentence.

Rough sizes are honest estimates against the current code, not promises.

---

## A. The temperament seed (the foundation — read this first)

### 1. Per-follower temperament seed
- **Feel:** the prerequisite for everything human. Right now every follower runs
  the same clock: same 1.5 s suppression window, same 3 s cast grace, same
  everything. One stable scalar per follower makes each one *consistently*
  themselves — Lydia is always the deliberate one, Erik always the eager one —
  which reads as personality without any personality system.
- **Mechanism:** one inline function, e.g. in `Confidence.h` or a 10-line
  `Temperament.h`: hash the FormID to a stable float in [0,1]
  (`(a_id * 2654435761u >> 16 & 0x3FF) / 1023.0f` — any cheap mix). No state, no
  serialization, no RNG: the same follower gets the same value every session.
  Consumers below scale their timings by it.
- **Size / difficulty:** ~10 lines. **Trivial.**
- **Tenet risk:** none by itself — it is a pure function. It only matters through
  its consumers (#2, #3, #10, #11), each bounded below.

### 2. Suppression-window tempo (kills the metronome)
- **Feel:** today a winning rule re-fires on an exact 1.5 s metronome, and when a
  shared trigger hits the whole party (player HP drops), everyone's window opens
  and closes in lockstep — the single most robotic thing MFO does. Scaling the
  window per follower (±10–15% by temperament) makes each follower act on their
  own tempo and desynchronizes the party permanently, for free.
- **Mechanism:** one expression change at the window assignment,
  `Scheduler.cpp` (`recent.until = now + ms(g_suppressWindow * 1000)` in the
  `Fired` case): multiply by `0.88f + 0.24f * Temperament(id)`. The MCM knob
  stays the *center*; the deviation is deterministic and small.
- **Size / difficulty:** ~2 lines. **Trivial.**
- **Tenet risk:** none. Positional suppression semantics untouched (higher rules
  still preempt); fires-only, so idle is untouched. Legibility: the [eval] log
  already timestamps fires, so the actual cadence is still readable.

### 3. Combat-entry "ready beat"
- **Feel:** a follower who fires a gambit on the very first serviced tick of a
  fight acts with inhuman instantaneity — no human squares up, reads the field,
  *then* acts in 0 ms. A one-beat hold (≈200–350 ms, temperament-jittered) on
  entering combat reads as the follower registering the fight. Because the
  jitter differs per follower, the party also stops opening in unison.
- **Mechanism:** the combat branch of `Scheduler::Tick()` already detects the
  combat↔idle transition shape (the `g_retreatNotes` erase on combat end is the
  precedent). Keep a per-follower `combatEnteredAt` timestamp in the existing
  `Recent` record (or a sibling map cleared where retreat notes are cleared);
  before evaluating the combat table, return if
  `now < enteredAt + ms(200 + 150 * Temperament(id))`. Wall-clock, not ticks, so
  large parties (already serviced every N×133 ms) never stack extra delay.
- **Size / difficulty:** ~10 lines. **Easy.**
- **Tenet risk:** delays the *opening* action of a fight by at most ~350 ms —
  within the §4.1 spirit (133 ms is a floor, not a promise of zero-latency
  openings). Never delays mid-fight reactions (the hold applies once per combat
  entry). If marth is nervous, gate behind a default-on `bReadyBeat`.

---

## B. Visible commitment (feedback that reads as intent)

### 4. Weapon-ready flourish on equip
- **Feel:** when `act.equip_melee/ranged` swaps the weapon, the follower should
  visibly *commit* — steel out, squared up — rather than the new weapon sitting
  wherever the previous draw state left it. FFXII party members visibly ready
  the new tool when a gambit changes their job.
- **Mechanism:** in `Actuation.cpp::EquipWeapon`, after the successful equip
  (the `Fired, "equipped melee/ranged"` return path, ~line 355), call
  `a_follower->DrawWeaponMagicHands(true)` — the exact call `Loadout.cpp:240`
  already ships for the cast path, so this is a proven, precedented verb.
  Combat-table only, so it never fires on an idle follower.
- **Size / difficulty:** 1–2 lines. **Trivial.**
- **Tenet risk:** none meaningful. It fires only on a real equip `Fired` (which
  already owns a suppression window, so no repeat), and drawing while already
  drawn is a no-op. The idempotent "already holding that category" NoOp path is
  untouched — no thrash.

### 5. Retarget hesitation (mid-fight target switch)
- **Feel:** today, the instant a different foe satisfies the winning attack
  rule, the latch snaps to them — frame-perfect target swapping no human does.
  Requiring the *same* new choice to win one more service before re-latching
  reads as "finishing the thought, then turning" — deliberate, not robotic.
- **Mechanism:** in the `kActAttack` branch of `Actuation::Fire` (or just before
  `Targeting::Command` is consulted in the scheduler), keep the last *proposed*
  target per follower; if this fire would switch an **existing** latch to a new
  foe, store the proposal and return `NoOp, "sizing up <name>"` once; commit on
  the next service if the choice repeats. Initial latch of a fight is exempt
  (covered by #3) — only switches hesitate.
- **Size / difficulty:** ~15 lines (one small map keyed like `g_recent`,
  cleared on combat end alongside retreat notes). **Easy.**
- **Tenet risk:** adds one service interval (~133 ms × party size) to target
  *switches* only — never to the first engagement, never to heals/casts. The
  NoOp reason goes through the existing transition-only logger, so no spam.
  Worth flagging: with 3+ followers this is ~0.5 s; keep the exemption for
  `foe_attacking_me` selectors if that feels slow in the field (self-defense
  should stay snappy).

### 6. Cast-grace temperament offset
- **Feel:** all casters currently give their own AI exactly 3.0 s
  (`fAiCastGrace`) before the silent fallback — so two mages who both fail to
  animate fall back on the same beat, twin silent heals landing in sync. A
  ±0.4 s temperament offset staggers them; each caster has their own patience.
- **Mechanism:** in `Actuation.cpp::CastOn` where `held < g_aiCastGrace` is
  tested (~line 139), compare against
  `g_aiCastGrace * (0.87f + 0.26f * Temperament(id))`.
- **Size / difficulty:** ~2 lines. **Trivial.**
- **Tenet risk:** none — the reason string stays stable (dedup key unchanged),
  the MCM knob stays the center, fallback still guaranteed.

---

## C. Confidence and the aftermath (cosmetic expressiveness)

### 7. Confidence-expressive loot gait
- **Feel:** the leash tenet says the player *feels* confidence through behavior.
  Cheapest possible read: a confident follower strolls/jogs to loot like they
  own the field; a rattled one hustles — grab it and get back. Suddenly the
  invisible variable is visible in the walk.
- **Mechanism:** `Gait::Apply` already writes `preferredSpeed` (one byte on
  MFO's *own* `MFO_TravelPackage`, the lowest-risk write MFO makes, per
  `Gait.h`). At loot-travel fill time in `Logistics`, pick the byte from
  `Confidence::Of(f)`: ≥0.6 → Jog, <0.35 → Run, else the configured default.
  The MCM gait setting becomes the mid-band value.
- **Size / difficulty:** ~8 lines (a `Gait::ApplyFor(confidence)` variant).
  **Easy.**
- **Tenet risk:** near zero — session-local write on our own package, only at
  the moment a loot trip fires (never idle). One caution: the package byte is
  global to the package, so with two followers travelling simultaneously the
  later fill wins; acceptable for a cosmetic (both are "MFO followers on
  errands"), but say so in the commit comment.

### 8. Post-combat breather
- **Feel:** the frame combat ends, logistics wakes and the follower beelines to
  corpses — a looting robot. Humans exhale first. A 2–4 s (temperament-jittered)
  hold on *loot-travel* after combat ends reads as catching breath, scanning the
  field, then getting to work. Also naturally staggers two followers so they
  don't sprint off in lockstep.
- **Mechanism:** the non-combat branch of `Scheduler::Tick()` already runs
  teardown on the combat→idle transition (RetreatClear + `g_retreatNotes.erase`).
  Stamp `combatEndedAt` there; `Logistics::ServiceFollower` (or just its
  loot-travel gate) skips loot actions until
  `now > endedAt + s(2 + 2 * Temperament(id))`. Drink/torch upkeep can stay
  immediate — drinking after a fight IS the human move.
- **Size / difficulty:** ~10 lines. **Easy.**
- **Tenet risk:** none — it only *delays* MFO's own optional actions; idle
  followers without loot gambits see zero change. First-dibs interplay is
  actually improved (the player gets a naturally longer head start).

---

## D. The board and the log (FFXII texture, render-only)

### 9. Fired-rule pulse on the board
- **Feel:** the FFXII gambit "firing" feel — on the board you *see* the line
  light up as it executes. MFO already tracks `rule.lastFired` for display;
  making the row flash and fade (~1 s) when it fires turns the board from a
  static editor into a live readout of the follower thinking.
- **Mechanism:** `Scheduler.cpp` already writes `rule.lastFired` per fire
  (~line 300); add a `lastFiredAt` timestamp beside it (display-only — the
  evaluator never reads these back, per the existing #22 comment). In
  `Board.cpp`'s Gambits tab, lerp the row background from an accent color to
  normal over the second after `lastFiredAt`. Render-thread reads a timestamp;
  no new locking beyond what the existing lastFired display already does.
- **Size / difficulty:** ~15 lines. **Easy.**
- **Tenet risk:** none — pure ImGui cosmetics, only drawn while the board is
  open. Follow whatever snapshot discipline the current `lastFired` display
  uses.

### 10. FFXII slot styling: condition–arrow–action coloring
- **Feel:** FFXII's board is instantly readable because the *shape* of a gambit
  is typographic: target/condition in one hue, action in another, the arrow
  between. Coloring MFO's rule rows the same way (condition text in the accent,
  `→`, action in a second tone; `act.wait` dimmed as the "OFF/hold" idiom) makes
  the list read as a gambit page, not a config table.
- **Mechanism:** `Board.cpp` Gambits tab: split the row text into two
  `TextColored` calls with a literal arrow between. Pick the two tones from the
  existing MEO-parity palette.
- **Size / difficulty:** ~10 lines, render-only. **Easy.**
- **Tenet risk:** none. Keep contrast accessible; no per-frame allocation
  beyond what the row already does.

### 11. Follower names in [eval] fired lines
- **Feel:** small but constant humanity in the primary legibility surface:
  `[eval] Lydia (000A2C8E) fired rule 2 (act.cast_self)` reads as a person
  acting; a bare FormID reads as a process. Every field session gets nicer.
- **Mechanism:** the three `spdlog::info("[eval] {:08X} ...")` sites in
  `Scheduler.cpp` (fired / did NOT fire / held off): prepend
  `f->GetName() ? f->GetName() : "?"`. `f` is live at all three sites.
- **Size / difficulty:** ~3 lines. **Trivial.**
- **Tenet risk:** none — same lines, same transition-dedup keys, just richer.
  (Keep the FormID: it is the grep key.)

### 12. Order acknowledgment on board close
- **Feel:** in FFXII, changing a gambit page is acknowledged by the game state
  instantly and tangibly. When the player closes Field Orders after actually
  editing a follower's list, a single HUD line — `Lydia takes her new orders.`
  — closes the loop: the order was *received by a person*, not written to a
  config. (True voiced barks are the not-easy version; see §F.)
- **Mechanism:** `Board.cpp` already knows when a list mutates (it writes the
  tables); keep a per-open dirty flag + the last-edited follower; on board
  close, if dirty, one `RE::DebugNotification` naming the follower. One line
  per board session maximum.
- **Size / difficulty:** ~10 lines. **Trivial.**
- **Tenet risk:** none — player-initiated moment, once per edit session, no
  engine calls on the follower. Keep the copy short and dry (LoreRim tone,
  not quippy).

### 13. Rank-up moment for Rapport
- **Feel:** rapport currently rises silently (rank changes go to the log only,
  per `Rapport::Award`). Surfacing the *rank transition* — one HUD line,
  `Lydia fights beside you with new resolve.` — gives the fighting-together
  system its FFXII "the party grows" beat. Transitions are rare by design
  (BALANCE thresholds), so this can't spam.
- **Mechanism:** `Rapport.cpp::Award` already detects and logs rank changes;
  add `RE::DebugNotification` beside that log line, gated behind a
  default-on `bRapportToasts` if desired.
- **Size / difficulty:** ~5 lines. **Trivial.**
- **Tenet risk:** none — transition-only by construction.

### 14. FFXII job flavor on the seeded role templates
- **Feel:** GAMBIT_FLOWS H5 will ship seeded role sets (Vanguard / Archer /
  Hybrid / Elementalist / Spellsword). Giving each a one-line FFXII-flavored
  epigraph on the board picker — e.g. Spellsword: *"steel when the Mist runs
  dry"* — is the cheap, tasteful homage: naming texture, zero mechanics. The
  set names themselves stay marth's (they are clearer than Uhlan/Machinist).
- **Mechanism:** string constants beside the H5 seed tables (`Board.cpp` /
  GAMBIT_LIBRARY.md); shown as the template's subtitle in the picker.
- **Size / difficulty:** strings only. **Trivial** (rides on H5's Phase 2).
- **Tenet risk:** none. Keep epigraphs to one short clause each; the board is
  an instrument, not a novel.

### 15. Gambit callouts (FFXII action banner), **off by default**
- **Feel:** FFXII floats the action name over the actor as a gambit fires.
  MFO's nearest cheap equivalent: the first time each rule fires in a given
  combat, one HUD line — `Lydia: Foe HP < 50% → Firebolt`. For players who
  want the full FFXII "watch the program run" fantasy; off for everyone else.
- **Mechanism:** in the `Fired` case in `Scheduler.cpp`, alongside the [eval]
  log: a per-follower bitmask/set of "rules called out this combat" (cleared
  where retreat notes are cleared on combat end); if unseen and
  `bGambitCallouts`, `RE::DebugNotification` with the rule's display strings
  (the board already renders condition/action names, so the formatter exists).
- **Size / difficulty:** ~20 lines + MCM toggle. **Easy** (largest item here).
- **Tenet risk:** HUD spam is the risk; bounded by first-fire-per-rule-per-
  combat and default-off. MCM toggle must ride an existing settings page (no
  new schema risk, per the MCM Helper memory note).

---

## E. Explicitly cheap because of what they reuse

Worth stating: #4 reuses `DrawWeaponMagicHands` (proven at `Loadout.cpp:240`),
#7 reuses the Gait one-byte package write (proven since v0.8.3), #9/#10 reuse
the existing `lastFired` display plumbing, #12/#13/#15 reuse `DebugNotification`
(ubiquitous, main-thread-safe from where we'd call it), and #2/#3/#5/#6/#8 are
arithmetic on timers that already exist. Nothing above requires a hook, an
alias, a form, or a serialization change. No frozen opcode strings are touched
(#10 vocabulary contract unaffected).

---

## F. Considered, but not easy (the boundary)

Listed so the line is visible; none of these belong in a flair pass.

- **Idle glances / turning to face the player when spoken to.** Any engine call
  on an idle follower breaks byte-identical-when-idle. Hard no, whatever it
  costs in charm.
- **Voiced barks / Say() acknowledgments.** Needs dialogue topics in the ESP,
  voice-type coverage across every follower mod, and subtitle plumbing — a
  content system, not a tweak.
- **Victory poses / sheathe flourish after combat.** Sheathing is the vanilla
  AI's; sending sheath/pose animation events against it is a tug-of-war with
  the engine (the D7 shape from GAMBIT_FLOWS). Rejected.
- **VFX on the silent-cast fallback** (making the unanimated heal visible).
  Effect-shader application needs a ledgered reversal (§8.5) and touches the
  §0.6 confound. Real work, not flair.
- **Squaring up on latch** (taunt/brace animation when acquiring a target).
  Same animation-graph contention class as poses; the latch is deliberately
  invisible to the engine. #5's hesitation delivers the feel without the risk.
- **Per-action-class suppression windows** (cast ≈ cast time, equip ≈ 0).
  Genuinely good — and already GAMBIT_FLOWS Phase 3. Scheduler-semantics work,
  not texture; do it there, not here.
- **Confidence-posture while fighting** (bolder stance when confident). No
  cheap verb: combat locomotion/stance belongs to the combat controller.
  Confidence expressiveness ships through #7 and the existing leash/chase radii.

---

## G. Suggested batch order (if several are picked)

1. **Batch 1 (pure arithmetic + strings, one commit):** #1 seed, #2 window
   tempo, #6 grace offset, #11 log names — invisible-risk, immediately field-
   testable via [eval] timestamps.
2. **Batch 2 (moments):** #4 equip flourish, #8 breather, #12 board ack,
   #13 rank toast.
3. **Batch 3 (board render):** #9 pulse, #10 slot styling, #14 epigraphs
   (with H5).
4. **Batch 4 (behavioral, field-verify):** #3 ready beat, #5 retarget
   hesitation, #7 confidence gait, #15 callouts.
