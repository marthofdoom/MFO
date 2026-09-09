# DIAG 2026-09-08 -- "possible unrelated crash, incomplete denies, self heal delayed"

Session: deck, 11:33:35-11:36:50, Jesper the Guard `0x750012C6` (Mage, gambits 7c/6l) + Adelinda `0x00015D09`
(Ranged) + Cicero `0x0009BCB0` (Melee) vs Falmer + four Chaurus Reapers. Crash 11:36:49.
DLLs (sha-verified on the deck): MFO `572f798e` (main `3c492a8`), APMF `74da869b` (main `05563f3`).
Logs: scratchpad `field-0908/` (`MFO.log` 570 lines, `APMF.log` 1200, `crash.log`, `EVIDENCE.txt`).
Method: every claim below is traced to source by `file:line` on those two commits, or to a log line quoted
with its timestamp. Symbols verified against `_commonlib/pinned-3.7.0-c4ab853d/` only. No source edited,
nothing built, no git state touched.

Confidence markers: CONFIRMED = code path read end to end and matched to log lines. PLAUSIBLE = code +
log, one link unobserved. UNKNOWN = stated as such.

Reading aid for `APMF.log` `[ctl]` lines on the cast channel: `+ ... additional claim (h=N)` is a NEW
`RequestCast`; `- ... claim dropped (h=N); 1 claim(s) remain` is a `Release` of one handle while another
stands (the `[ch.8b] ... RE-POINTED (spell X)` line beside it is the channel re-publishing the survivor,
NOT an in-place repoint); `~ ... REPOINT (h=N, ... TTL renewed +6000 ms)` is the real in-place repoint
(the F5-1 heartbeat). Reconstructed handle map for the follower's cast channel (spell from the survivor
line, hand from `SCORE-STEER ... hand=`):

| handle | requested | released | spell | hand |
|---|---|---|---|---|
| h=1 | 11:35:48.502 | 11:35:52.765 | Firebolt `00012FD0` | DualCast (both) |
| h=4 | 11:35:49.441 | 11:35:55.426 | Firebolt (new target `000530C6`) | DualCast |
| h=6 | 11:35:53.043 | 11:35:57.034 | Firebolt | DualCast |
| h=7 | 11:35:55.568 | 11:35:58.755 | Lightning Bolt `0002DD29` | RIGHT |
| h=8 | 11:35:57.166 | 11:36:04.891 | Fast Healing `0002F3B8` -> proxy `FF001F32`, target Adelinda | LEFT |
| h=9 | 11:36:03.700 | 11:36:07.415 | Lightning Bolt | RIGHT |
| h=10 | 11:36:05.022 | 11:36:49.889 | Fast Healing, target SELF (no proxy) | LEFT |

From 11:36:07.415 to the end (42 s) the ONLY cast claim on the follower was h=10, a LEFT-hand self-heal.

---

## 0. Corrections to the evidence brief

These matter for everything below, so they come first.

1. **"Three un-gambited spells reached a live cast on the RIGHT hand while claims stood" is true but
   misleading.** Every one of those casts (Poison Spray `848C886D` at 11:36:00.556 and 11:36:12.476, Stone
   Rune `FE209C83` x5 fires 11:36:18-40, Raise Zombie `0007E8E1` x3 fires 11:36:33-37) happened in a window
   where **no RIGHT-hand claim stood** -- only h=8 or h=10 (both LEFT). The one right-hand claim in that
   period, h=9, stood 11:36:03.700-11:36:07.415 and during it the right hand cast exactly the claimed
   Lightning Bolt (fires 11:36:06.439, 11:36:08.827). The three `[t2c] CheckCast DENIED` lines all fell
   where a claim DID occupy the hand: Poison Spray R at 11:35:51.822 (dual-cast Firebolt occupied both
   hands), Firebolt L at 11:35:59.314 and Poison Spray L at 11:36:05.467 (LEFT heal claims). **The deny
   went 3 for 3 where a claim stood and was never asked where none stood.** That is RC3 of
   `DIAG-2026-09-06-deny-heal-failures.md` reproduced exactly; the APMF header itself predicts it
   (`APMF_API.h:277-285`: "25 Stone Runes, 6 Poison Sprays and 8 Raise Zombies all charged on the
   UNCLAIMED hand").
2. **"A heal landed" undercounts.** Two MFO heal deliveries fired: the delivery-flip proxy `FF001F32`
   (ally heal at Adelinda, claim h=8) fired `MLh_SpellFire_Event` at **11:36:03.258**, 4.5 s after the h=8
   repoint, and MFO filed it correctly (`[cast] ... CAST (unnamed) (FF001F32) ... *** MFO GAMBIT SPELL ***`
   11:36:03.270 -- RC4/F4 of 09-06 is fixed). The raw Fast Healing at 11:36:21.727 was the SELF-heal claim
   h=10, a different gambit (rule 1 `act.cast_self`), not the same claim delayed 22 s. See section 3.
3. **The hand bookkeeping the brief calls contradictory is not.** `[cfc]` says "spell 00012FD0, right
   hand" while `[eval]` says "00012FD0 is still firing there (left hand)" because Firebolt was claimed
   **DualCast** (`Loadout.cpp:237 PlanCastHand` -> `HandPick::DualCast`; `APMFBridge.cpp:695-702` sets
   `kCastFlag_DualCast`; `Actuation.cpp:805-806` locks BOTH hands). `ComposedCast.cpp:86-87 WatchSlot` maps
   anything not `kApmfHandLeft` to slot 1 = "right" in the warning text. APMF confirms both-hand
   occupancy: `SCORE-STEER Firebolt hand=L` (11:35:48.966) and `t2c DENIED Poison Spray hand=R`
   (11:35:51.822) under the same claim. Cosmetic, but the `[cfc]` hand label is wrong for a dual claim.
4. **`0x0002F3B8`'s proxy was `FF001F32` for h=8 only.** h=10 (self-target) minted no proxy -- correctly,
   a kSelf spell cast on self needs no delivery flip -- and APMF's steer switched to the raw spell:
   `SCORE-STEER item=0x0002F3B8 'Fast Healing' hand=L engineScore=945 STEERED->1945` from 11:36:05.027.
5. **The three `[cfc]` warnings are all MFO-side artefacts, not seat failures** (RC5 below): 2394 ms
   (Firebolt fired 24 ms later, on the LEFT hand of a dual claim); 3204 ms (h=8 proxy heal fired at
   +4.5 s -- the 2 s grace is shorter than a heal's equip cycle; F6 of 09-06 was not applied); 8135 ms
   (Lightning Bolt: charged at 11:35:58.664 under h=7 and thrown away by MFO's own release at 58.755, then
   re-claimed as h=9 -- the watch was never reset because the spell was the same).
6. **MFO's own input sink IS registered on this build** (`[overlay-probe] input sink registered on
   BSInputDeviceManager` 11:34:55.199; `Board.cpp:1332-1334`). The brief noted only that APMF's is not.
   It ends up not mattering (section 1.2), but it had to be checked.
7. **All-claims release at 11:36:49.889 is MFO, not APMF, and it is the Journal pause.** RC6.
8. Facts 2 and 5 of the brief are correct as stated. The heartbeat proof exists only for the heal claims
   (h=8/h=10 renewed in place, ages to 31 s); no offense claim lived long enough to need one.

---

## 1. Verdicts

### 1.1 "Incomplete denies" -- CONFIRMED, and it is the designed gap, unchanged since 09-06

The deny is complete per hand and MFO never claimed the right hand after 11:35:58.755 (except h=9 for
3.7 s). The right hand was AI-governed for ~48 of the session's 62 combat seconds because (a) APMF scopes
the deny to the claim's hand by design (`core/EquipGate.cpp:520-548`, `core/CastGate.cpp:148-162`,
`core/Allowance.cpp:105`), (b) the flag that closes it, `kCastFlag_DenyHandOnly` (`APMF_API.h:273`), ships
with no client setting it (grep `kCastFlag_DenyHandOnly` in MFO `native/` returns only the comment at
`APMFBridge.cpp:163-173`: "MFO's own deny-only hand claim is queued work"), and (c) MFO's evaluator
stopped issuing offense claims at all after 11:36:05 (RC2). Nothing "leaked through" a deny; the hand was
open.

### 1.2 The crash -- NOT attributable to MFO or APMF; near-certainly a pre-existing engine signature

An identical crash -- same `SkyrimSE.exe+0CD509D`, same `mov r13, [rdi+rax*8+0x60]`, same six frames
`68542+0x12D / 68617+0x58 / 69378+0xD8 / 69380+0x361 / 69344+0x8A / 68445+0x3D` -- is public in a Feb 2025
log (https://pastebin.com/CmgENZh5) on 1.6.1170 with **no MFO.dll, APMF.dll, MFO.esp** in that load order
(MCMHelper.dll 1.5 present). Details in RC7. The residual MFO exposure (its `InputSink` runs inside this
same dispatch; `Board.cpp:589`) cannot be excluded to zero from a stack with no MFO frame, but an identical
signature without us makes it improbable. "Possibly unrelated" is the right call; "unrelated to the
session end" is wrong -- it is the session end, on a gamepad button event in the frame the Journal/MCM
closed.

### 1.3 "Definitely better but disappointing" -- the follower threw away three of its own charged casts, then spent 42 s parked on a self-heal the engine executed once

What moved (evidence in section 4): heartbeat works; proxy reads correctly; the ally heal landed in
4.5 s; the score steer fires on spells; the equip-deny is complete on the claimed hand; nothing crashed
or misbehaved in the Board split. What did not move: **MFO's claim churn** (RC1) and **the evaluator's
first-match lock on the self-heal rule** (RC2) -- together they are the whole of "disappointing". The
follower delivered 2 Firebolt-class casts, 2 Lightning Bolts and 2 heals in 62 s while charging and
aborting 3 more of MFO's own spells, and the AI's own right-hand offense filled the rest.

### 1.4 "Self heal delayed, if it occurred at all" -- it occurred once, 16.7 s after MFO asked; the delay is the engine's own heal trigger, not the seats

Section 3.

---

## 2. Ranked root causes

### RC1 -- CONFIRMED -- MFO re-claims (Release + RequestCast) on every spell/target/hand change at rule-lap cadence, and each change interrupts the engine's in-flight cast. Three CHARGED MFO casts were discarded by MFO's next claim.

- `native/APMFBridge.cpp:683-701` `EnsureCastClaimLocked`: anything but an identical
  (spell,target,hand,conc,stopPct) tuple does `api->Release(c.handle)` then `api->RequestCast(...)`.
  `ClaimOffenseCast` (`APMFBridge.cpp:902-946`) is called from every winning offense lap. Seven cast
  handles in 19 s on one actor (table above); 11:35:48-11:36:07 shows a change every 0.9-2.4 s.
- Each change makes the AI rebuild its equipment set (`core/EquipGate.cpp:348 inventory->dirty = true`,
  and the deny-set itself changes), which unequips the hand: `castobs` shows `InterruptCast` +
  `CastStop` + `Magic_Equip_Out` within 150 ms of **5 of 7** ch.8 claim changes (script over
  `APMF.log`: 52.765 -> +0.042 s, 55.426 -> +0.146 s, 55.568 -> +0.004 s, 03.700 -> +0.003 s,
  04.891 -> +0.042 s). MFO itself never calls `InterruptCast` on the owned path (`Actuation.cpp:1567` and
  `Actuation_Direct.cpp:427/561` are the legacy/direct routes; `Packages.cpp:844` the package route) --
  this is the engine reacting.
- Casts lost: Firebolt R **Charged** 11:35:54.040 -> h=4 dropped 55.426 -> `InterruptCast` 55.572, never
  fired; Firebolt L **Charged** 56.995 -> the hand was re-claimed for the heal (h=8) -> `t2c DENIED Firebolt
  hand=L` 59.314, never fired; Lightning Bolt R **Charged** 58.664 -> h=7 dropped 58.755 (heal rule won the
  lap) -> `InterruptCast` 59.371, never fired. h=9's Lightning Bolt survived only because the second charge
  (06.779-07.323) had already fired once and the drop at 07.415 came 0.09 s after "Charged".
- Why h=9 dropped at 07.415: rule 1 (`act.cast_self`) fired first every lap from 05.020 (RC2), so rule 5
  was never reached, `o.offense[1].refreshed` stopped moving, and `Tick()`'s sweep released it at
  `FacetExpiry()` (`APMFBridge.cpp:1289-1315`, ~2.4 s: 05.02 + 2.4 = 07.4). Matches to the tenth.
- This is RC1 of 09-06 (heal-slot thrash) generalized: F1 locked the HEAL slot, but the offense slots
  still thrash against each other and against the heal, and DualCast vs single-hand flips (h=6 -> h=7)
  are also a "change".

### RC2 -- CONFIRMED -- The self-heal rule monopolizes the evaluator: once it fires it fires every lap for as long as its condition holds, because the hand lock treats "same spell, same target" as free. No offense claim was raised for 44 s.

- `native/Actuation.cpp:381` `HandFree`: `if (lock.spell == a_spell && lock.target == a_target) return
  true;` -- the lock never holds a rule off from re-asserting ITSELF. `Scheduler.cpp:987` logs `fired rule
  1` and the lap ends there (first match). `MFO.log` 11:36:05.020 -> 11:36:49.910: 22 consecutive `fired
  rule 1 (act.cast_self)` laps, zero rule 2/4/5 lines (the offense and ally-heal rules), zero
  `ClaimOffenseCast` traffic in `APMF.log` (no `+ additional claim` after h=10).
- The lock is also self-sustaining: `Actuation.cpp:359-365 CastLockLive` returns true while
  `IsHealCastActive` / `IsOwnedCastActiveOnHand`, and the F5-1 heartbeat keeps the claim alive
  (`APMFBridge.cpp:528-610`). So rule 1 -> claim -> lock -> rule 1. That is "spell gambit locks until
  complete" (memory) working exactly as written; the unrequested consequence is that a self-heal whose
  condition stays true (his health did not clear the threshold after one Fast Healing -- rule 1 kept
  firing after 11:36:21.7) starves every other cast gambit indefinitely. `Actuation.cpp:827` protects only
  attack/drink from a persistently-true `cast_self`, not other cast rules.
- What the owner saw: a mage who, after the first 15 s, cast only what its own AI picked for the right
  hand (Stone Rune, Raise Zombie, Poison Spray) and healed himself once.

### RC3 -- CONFIRMED -- The unclaimed hand is open by design; F3 (`kCastFlag_DenyHandOnly`) is dormant.

- `core/Allowance.cpp:105` (no cast claim on THIS hand -> imposes nothing), `core/EquipGate.cpp:520-548`
  (`hasHandSeat || handDenyOnly` gates the deny-complete branch, per hand), `core/CastGate.cpp:148-162`
  (`AllowedCastForHand` per hand). Log: 154 `-> NO` decisions, all on the CLAIMED hand's slot set; the
  right hand's `CheckShouldEquip` traffic is invisible because there was nothing to deny.
- MFO never sets the bit (`APMFBridge.cpp:163-173` comment; `apmf-f3-denyhand-followups` memory lists 4
  APMF fixes needed first, `ApplyRepoint` param overwrite mandatory).
- Same finding, same numbers' shape, as 09-06 RC3 / audit rows P3-P4. Nothing shipped since addresses it.

### RC4 -- CONFIRMED -- The self-heal claim cannot make the engine deliberate a Restore cast; seat 0x06 answers only when asked, and the engine asked once, on its own health trigger.

- `core/CastSeats.cpp` seat 0x06 (`CheckStartCastThunk`, body shown around the `[ch.8b seat 0x06]` log
  site): it is a hook on `CombatMagicCasterRestore::CheckStartCast`; it returns YES when the claim names
  this cast, and otherwise chains. It runs only when the AI's cast leaf evaluates the Restore caster.
- `APMF.log` `[aicastseats]` for the follower, 11:36:05-11:36:20: `CheckStartCast[Offensive]` at 05.467,
  08.846, 12.422, 17.699, 18.167 (the AI deliberating its right-hand offense), **zero
  `CheckStartCast[Restore]`** calls until **11:36:20.183** (`Fast Healing -> YES`, `GetMagicTarget ->
  SELF/ATTACKER`), then `CASTER[L] Casting 0002F3B8` 21.253, `MLh_SpellFire` 21.727. Seat 0x06 printed
  nothing at 20.183 because the engine had already said YES (nothing to override; the seat's log fires on
  the override path). Between 05.0 and 20.2 the left hand held the freed h=8 proxy (`CASTER[L]
  state=4(Casting) FF001F32` 04.786, no fire) and then nothing.
- `SCORE-STEER Fast Healing hand=L 945->1945` every ~2 s from 05.027 proves the item was in the AI's set
  and steered; the claim's YES was armed the whole time. What was missing is the engine choosing to run a
  Restore deliberation -- vanilla does that off its own health fraction / restore-restriction timer
  (`CastSeats.cpp` seat 0x06 comment: "ally% < lerp(0, 0.5, defensiveMult)" and the 15 s
  `MagicRestoreRestrictionTimer`). The 16.7 s is the gap between MFO's heal threshold and the engine's.
- This is the "DISASSEMBLY PROVES A PATH EXISTS, NOT THAT IT RUNS" principle in a new coat: the seat is
  correct and observed working (11:35:59.353 for the ally heal, where the engine's ally-heal trigger
  asked within 0.6 s of the claim); for a SELF heal the engine's trigger is later than MFO's.

### RC5 -- CONFIRMED -- The `[cfc]` watch is keyed on spell only, so it is blind to a re-claim of the same spell and false-alarms on MFO's own churn.

- `native/ComposedCast.cpp:122-151 WatchArmed`: `if (w.spell != a_spell) reset` and `if (w.observed)
  return`. h=10 (self heal) inherited h=8's watch (same spell, `observed` already true from the proxy at
  03.258) -> 16.7 s of silence produced no warning. The 8135 ms Lightning Bolt warning measured from h=7's
  arming across h=7's release and h=9's request. `kSilentWarnAfter = 2000` (`ComposedCast.cpp:95`) is
  still below the measured 4.5 s heal equip+charge (09-06 F6 asked for >= 4000).

### RC6 -- CONFIRMED mechanism -- A wall-clock expiry sweep crossed the Journal pause and released every MFO claim on all three followers in one tick (11:36:49.889). Benign this session, wrong in principle.

- The pump did not evaluate during the Journal (last `[eval]` 11:36:39.533, Journal open by ~40.7, next
  `[eval]` 49.910); `MenuSink` (`Diagnostics.cpp:117-135`) queued the config re-read for the close.
  `Diagnostics.cpp:346` runs `APMFBridge::Tick()` on the pump; `APMFBridge.cpp:1289-1327` compares
  `steady_clock::now() - refreshed` against `FacetExpiry()` (~2.4 s for a 4-body party). Ten seconds of
  menu pause aged every stamp past it -> `ReleaseClaimLocked` on h=10 and all three ch.6 target handles,
  3 ms after the config re-read task ran. "A FLOOR IS SAFE; AN EXPIRY IS NOT" (CLAUDE.md principle 9)
  with a pause instead of a cadence. Cost here: none visible (the crash came first). Cost in general: any
  menu ends every claim; the AI gets one free lap of its own choices after every MCM/Journal/inventory.

### RC7 -- CONFIRMED signature, engine-side -- `SkyrimSE.exe+0CD509D` is the input thread's device poll dereferencing `devices[event->device]` (or an equivalent 8-byte table at +0x60) with a garbage 32-bit index.

- Address library (`versionlib-1-6-1170-0.bin`, parsed locally): frame [1] `68617` = 0xCD8F40 =
  `BSInputDeviceManager::PollInputDevices` (pinned `src/RE/B/BSInputDeviceManager.cpp:152`
  `RELOCATION_ID(67315, 68617)`; its comment: "Calls Process() on each device / ControlMap ... / Emits the
  last InputEvent"). Frame [0] `68542` = 0xCD4F70 is a callee of it at +0x58 and is not bound in
  CommonLib 3.7.0 (no hit in the pinned tree). Two engine objects have an 8-byte table at +0x60:
  `BSInputDeviceManager::devices[4]` (pinned `include/RE/B/BSInputDeviceManager.h:80`) and
  `ControlMap::controlMap[InputContextID::kTotal]` (pinned `include/RE/C/ControlMap.h:102`). `rdi` =
  `SkyrimSE.exe+31878D0` is address-library ID 403592, a static object in .bss; both singletons above are
  reached through `**` pointer slots (`ControlMap.cpp:7-10`, `BSInputDeviceManager.cpp:13-16`), so the
  exact object is UNKNOWN without disassembly. `rax = 0xFFFFFFFF8339BEB0` is a sign-extended 32-bit read
  whose value is the low dword of a heap pointer in the same `0x833A....` region as RBX/R13: the code read
  an int field from memory that held a pointer -- a stale or mis-typed object. `R12 = SkyrimSE.exe+31CCBF0`
  is ID 410244, a STATIC `ButtonEvent` (crash logger identified its vtable; `BSInputEventQueue` is heap,
  `BSInputEventQueue.cpp:5-9`, so this is not the queue pool). Frames [2]-[5] (`69378/69380/69344/68445`)
  sit directly on `BaseThreadInitThunk`: this is a dedicated thread whose entry is `68445`, i.e. 1.6.1170
  polls input off the main thread (consistent with `Board.cpp:588` "Runs on the input thread").
- The public Feb 2025 log at https://pastebin.com/CmgENZh5 has the same offset, instruction and all six
  frame IDs with identical intra-function offsets, no MFO/APMF, MCMHelper 1.5 present. Our session:
  Journal/MCM closed by gamepad in the same frame (`[config] re-read after Journal/MCM close` 49.886;
  `RSP+20 = BSWin32GamepadDevice*`; `R8 -> "n Menu"`, the tail of a user-event name such as "Tween Menu",
  the gamepad B mapping). Same shape as ours: gamepad + menu transition.
- MFO's exposure, for completeness: `Board.cpp:589-806` `InputSink` READS every event on this thread and
  never writes an event; `Board.cpp:443-460` `SyncControlBlock` calls `ControlMap::ToggleControls` via
  `MainThread::Post` (main thread, `enabledControls` only, never the context stack); the WndProc swap
  (`overlay-probe` 11:34:55.213) forwards; the vendored ImGui backend's XInput poll is compiled out
  (`Board.cpp:746-750` comment). The Board had been closed for 25 s (`[bcancel] ... CLOSE BOARD`
  11:36:24.115). APMF registered no input sink (`[input] test surface NOT armed` 11:34:47.807) and has no
  code on this thread. Nothing here writes what frame [0] reads.

---

## 3. The self-heal timeline, answered

The brief's 32.8 / 24.6 / 22.4 s intervals splice two different claims. Correct sequence:

```
11:35:48.502  h=1 Firebolt DUAL-CAST claimed (rule 4 act.cast_target) -> both hands locked by MFO
11:35:48.966  [t2a] 0x0002F3B8 -> NO ... x6 to 11:35:56.521   <- correct: no heal claim yet, Firebolt owns both hands
11:35:56.365  [eval] HELD OFF -- 0002F3B8 wants LEFT, 00012FD0 still firing there   <- MFO's own dual-cast lock
11:35:57.163  rule 2 (act.cast_target: Fast Healing -> Adelinda) fires; h=8 requested; proxy FF001F32 minted 57.166
11:35:57.171  SCORE-STEER FF001F32 hand=L 1890->2890; [t2a] FF001F32 -> YES
11:35:59.353  seat 0x06 YES / seat 0x0A -> Adelinda (the engine's ALLY-heal trigger asked 0.6 s after the claim)
11:36:00.778  CASTER[L] Unk1 FF001F32; 02.892 Casting; 03.258 MLh_SpellFire  <- ALLY HEAL FIRED, +4.5 s from repoint
11:36:04.786  CASTER[L] Casting FF001F32 again (no fire); 04.891 h=8 released (rule 1 outranks rule 2 now)
11:36:05.022  h=10 Fast Healing on SELF (rule 1 act.cast_self); no proxy (correct); steer moves to the raw spell
11:36:05-20   engine deliberates Offensive (right) 5x; Restore (left) ZERO times; left hand idle
11:36:20.183  CheckStartCast[Restore] Fast Healing -> YES (engine's own self-heal trigger)
11:36:21.727  MLh_SpellFire 0002F3B8  <- SELF HEAL FIRED, +16.7 s from claim, +1.5 s from the engine asking
11:36:22-49   rule 1 keeps firing every lap; claim heartbeat-renewed 9 more times; no second Restore deliberation
```

**Q1 -- what consumed the ~22 s?** Nothing consumed 22 s of one claim. The ally heal (h=8) took 4.5 s
claim-to-fire, in line with the 09-06 measurement (2.95 s) plus one extra equip cycle (the hand was
mid-unequip from the Firebolt churn, `Magic_Equip_Out` 11:35:53.135). The self heal (h=10) waited 15.2 s
for the engine to evaluate its Restore caster at all (RC4), then fired in 1.5 s. The engine's cast timing
is not the floor here; the engine's DECISION timing is.

**Q2 -- the first ~8 s is MFO blocking its own heal.** Correct, and it is the designed arbitration
working: rule 4 (offense, dual-cast) fired first at 11:35:48.5 and `Actuation.cpp:805-806` locked both
hands; the heal rules were then `HELD OFF` by `HandFree` (`Actuation.cpp:374-388`) until the Firebolt lock
died. Whether a heal should pre-empt is a gambit-order question the owner authored (rule 1 = self heal,
rule 2 = ally heal, rule 4/5 = offense); the ORDER in his table already says heal-first, and the log shows
rule 4 firing first only because at 48.5 the heal conditions were not yet true. The defect is not
ordering; it is that a dual-cast offense claim occupies BOTH hands (`kCastFlag_DualCast`), so a heal that
becomes wanted 1 s later has no free hand until the offense claim is released -- and RC1 then released
it by churn rather than by completion. If a self-heal must pre-empt: `ResolveCastHand`
(`Actuation.cpp:399-437`) would need a priority rule that evicts a lower-ranked lock on the hand the heal
needs (and releases its claim) instead of holding the heal off. Cost: an evicted offense cast is
interrupted (RC1's mechanism, but deliberate and once).

**Q3 -- score steer.** CONFIRMED firing, on spells, per hand: `SCORE-STEER item=0xFF001F32 hand=L
engineScore=1890 STEERED->2890` (11:35:57.171) and `0x0002F3B8 hand=L 945->1945` (from 11:36:05.027),
`Firebolt hand=L 550->1550`, `Lightning Bolt hand=R 400.2->1400.2`. Source `core/AiCastSeats.cpp:495-548`
(`kScoreSteerBias = 1000.0f`, `SteerScoreForCastClaim`). Was it decisive? UNKNOWN from this log:
`EnableItemScoreProbe=0`, so competitors' `SCORE` lines were not written. What can be said: under
deny-complete every competing spell/staff on the claimed hand is already `-> NO` at seat 0x0F, so on the
claimed hand the steer competes only against weapons/fists (09-06 audit row P5, unhookable) -- the +1000
is insurance against a weapon out-scoring the claimed spell, not the reason the spell won. The engine's
own bases (945 for a self heal, 1890 for the heal-other proxy, 400-550 for offense) are the first field
measurement of the ceiling the bias was sized against ("~200" in `AiCastSeats.cpp:495-497` -- the
comment is now wrong by 5-10x; +1000 still clears 1890 but not by the margin the comment claims).

---

## 4. Grade of what shipped

| Item | Verdict | Evidence |
|---|---|---|
| F5-1 heartbeat (repoint-renew) | WORKS | `[ctl] ~ REPOINT (h=10 ... TTL renewed +6000 ms)` x11 at ~3.3 s cadence; claim ages to 31 193 ms; zero `already auto-expired` lines. Only exercised on heal claims (offense claims never lived > 6 s, RC1). |
| Proxy read / `[cast]` attribution (F4) | WORKS | `FF001F32` filed as `MFO GAMBIT SPELL` at 11:36:03.270; `[castproxy] minted` at 57.166 and `GetCastProxy` learned it before the fire. |
| Heal path (ally heal via proxy) | WORKS, 4.5 s | Section 3. One-for-one with the claim this time (09-06 was 1-in-15). |
| Heal path (self heal) | LANDED, 16.7 s, engine-timed | RC4. The claim is correct and inert until the engine's own Restore trigger. |
| Four-state fail-closed (F3-1 `everLive`) | NOT EXERCISED | Zero `NEVER published as live` warnings; every handle went live. No data on the refusal path. |
| Equip deny-complete (0x0F) | WORKS on the claimed hand | 154 `-> NO` on the claim's slot set, 6 `-> YES` for the driven proxy only; `[t2a] 0x0002F3B8 -> NO (N4)` never needed. Cannot act on an unclaimed hand (RC3). |
| Cast gate (t2c) | WORKS on the claimed hand | 3/3 denies where a claim occupied the hand. |
| Score steer (F5) | FIRES, on spells, per hand | Section 3 Q3. Decisiveness unmeasured. |
| Seat 0 classify | WORKS | `FF001F32 selfFlag 0->1` at 57.170; 343 `CLASSIFY` lines, all "DIFFERENT spell; not forced" except the driven form, as designed. |
| Board split (shell + Progression tab) | NO SIGN OF TROUBLE | `[board] opened` 11:36:18.083, `[bcancel] ... CLOSE BOARD` 24.115, `SUMMARY present=3102 ... pad=17 consumed-while-open=yes`, 4200 frames rendered, no `[overlay-probe]` error. Not implicated in the crash (RC7). |
| Loot travel (0x49 redirect) | NOT EXERCISED | `claimedActorsNow=0` all three heartbeats; `[loot]` scans ran (25 lines: potions/valuables/arrows/...) and found `0 eligible` each time. No data. |
| APMF input test surface gating | CORRECT | `[input] test surface NOT armed -- no keyboard sink registered`. |
| `[cfc]` watchdog (F6) | STILL MIS-SIZED | RC5; `kSilentWarnAfter` unchanged at 2000. |

Net: the framework side did what it was asked on every hand it was asked about. The disappointment is
MFO's client behaviour (RC1, RC2) plus the one gap both sides already knew about and deferred (RC3).

---

## 5. Minimal fixes (described, not written)

F8 (RC1, MFO) -- **Stop re-claiming on a target change; Repoint instead.** `EnsureCastClaimLocked`
(`APMFBridge.cpp:683-701`): when only `target` differs and the hand/spell match, call `Repoint` with the
new target (the ABI already renews TTL on it; `ApplyRepoint` overwrites `param` -- the F3 follow-up memo
says this overwrite is what a deny-only claim must survive, so a driving claim is fine). When the SPELL
differs on the same hand, do NOT release until the incumbent's cast is observed or its own TTL lapses --
extend the F1 heal-slot lock to the offense slots (`Actuation.cpp:374-388 HandFree` already refuses a
different spell on a busy hand; the hole is `ClaimOffenseCast` being reachable with a new spell for the
same hand from a different rule in the same lap, and the DualCast <-> single flip counting as a change).
Cost: a rule that wins the lap with a new spell waits for the previous cast to fire (~2.5 s). That is
the "spell gambit locks until complete" memory applied to offense.

F9 (RC2, MFO) -- **A rule whose action is already in flight must not end the lap.** In `Scheduler.cpp`
around `:987`, when `ResolveCastHand` reports "same spell, same target, claim live" (the
`HandFree` early-return at `Actuation.cpp:381`), treat the rule as SATISFIED-IN-FLIGHT (refresh the
claim, keep `castSeen`) and continue to the next rule instead of logging `fired rule 1` and stopping.
Then rule 5 (offense) gets its lap while the self-heal claim stands on the left hand, the right-hand
claim comes back, and RC3's open hand closes for free whenever an offense rule is wanted. Cost: the
heal's `refreshed` stamp must still be bumped on the in-flight path or `Tick()` sweeps it (RC1's h=9
mechanism).

F10 (RC3, APMF 4 fixes + MFO 1 flag) -- **Adopt `kCastFlag_DenyHandOnly` when a single-hand claim stands
and no offense rule wants the other hand.** Prerequisites are the four items in
`apmf-f3-denyhand-followups`. Then `ClaimOffenseCast`/`ClaimHealCast` raise a second, deny-only claim on
the free hand while the driving claim lives. Cost: the follower's other hand does nothing for the claim's
duration -- a Mage that heals with the left hand stops attacking with the right. This is a design choice
the owner has to make explicitly; F9 gives most of the benefit without it when he HAS an offense rule.

F11 (RC4, design) -- **Decide what a self-heal claim means when the engine does not want to heal.** Two
honest options: (a) accept engine timing and make MFO's `act.cast_self` threshold match the engine's
(then the claim only steers WHICH heal), or (b) drive the deliberation: a seat on the cast leaf's caster
SELECTION (the step that picks Offensive vs Restore before `CheckStartCast`) -- new RE, new seat, same
"observe it executing first" discipline (`Docs/ENGINE_NOTES.md` §0.41). Do not add a force-cast fallback
(memory: legacy = APMF-absent only).

F12 (RC5, MFO) -- `WatchArmed` keys on (spell, target, handle) not spell; reset `observed` on a new
handle; `kSilentWarnAfter >= 5000` (measured 4.5 s heal). `ComposedCast.cpp:95, 122-151`.

F13 (RC6, MFO) -- Make the `Tick()` sweep pause-aware: either compare against game time
(`Calendar`-derived or a frame counter advanced by the pump) or clamp `now - refreshed` by the pump's own
last-tick gap so a paused interval counts as zero. `APMFBridge.cpp:1289-1327`. Also the dual `[cfc]`
hand label (`ComposedCast.cpp:87`) should print "both" for `kApmfHandDualCast`.

F14 (RC7, none) -- No code change. See section 6 for what would attribute it.

Suggested order: F8 + F9 (MFO only; they are the whole "disappointing") -> F12/F13 (small) -> F10/F11
(owner's design calls) .

---

## 6. What this log cannot show, and the probe that settles each

1. **Whether the +1000 steer is decisive** -- needs `EnableItemScoreProbe=1` for one session so the
   competing `SCORE` lines exist beside `SCORE-STEER`. Zero code.
2. **Why the engine did not deliberate the Restore caster for 15 s** -- a passive probe on the cast
   leaf's caster-selection step (which `CombatMagicCaster` it picks each deliberation and the health
   fraction it read), or at minimum log Jesper's health % beside every `CheckStartCast[Restore]` and every
   MFO `act.cast_self` fire. Until then RC4's "threshold gap" is the mechanism observed, not the number.
3. **Whether the ally heal at 11:36:03.258 actually HIT Adelinda** -- the proxy fired; a
   `TESHitEvent`/`MagicEffectApply` observation on the target would close it. The 09-06 log's proxy heal
   "landed on the ally"; this one is unobserved past the fire.
4. **The crash** -- attribution needs any ONE of: (a) a repro with MFO.dll removed (the sink has no kill
   switch; `Board.cpp:1332` registers unconditionally) on the same gesture: gamepad B out of the MCM
   during combat; (b) a symbolised frame [0] (disassemble `0xCD4F70` in 1.6.1170 to name the +0x60
   table: `BSInputDeviceManager::devices` vs `ControlMap::controlMap` vs the static ID 403592); (c) the
   Feb 2025 reporter's load order intersected with ours (MCMHelper is the visible common factor). If (b)
   shows `devices[event->device]`, the bug is a stale static `ButtonEvent` (ID 410244) being re-dispatched
   with a freed object's bytes -- engine-side, and the fix is upstream of us.
5. **Loot travel** -- no eligible loot spawned. Nothing to conclude; re-test with a known-lootable corpse.
6. **The four-state fail-closed** -- no refusal happened. Needs a deliberate hand-collision test (two
   clients on one hand) to exercise `everLive == false`.

---

## 7. Fact check of the brief's five facts

| # | Brief | Status |
|---|---|---|
| 1 | Crash not unrelated to session end; engine frames only; APMF sink not registered | Correct. Add: MFO's sink IS registered and runs on that thread; identical public signature without us (RC7). |
| 2 | Heartbeat works, ages to 31 s | Correct, heal claims only; offense claims never lived to test it. |
| 3 | A heal landed; three `[cfc]` warnings | Two MFO heal deliveries landed (03.258 proxy, 21.727 self); all three warnings are MFO-side artefacts (RC5). |
| 4 | Three un-driven spells reached the RIGHT hand while claims stood | Counts correct; framing wrong: no RIGHT-hand claim stood during any of them (section 0.1). "154 NO / 0 YES" should read 154 NO / 6 YES (the driven proxy). |
| 5 | Loot travel never exercised | Correct. |
