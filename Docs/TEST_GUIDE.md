# MFO — Test Guide

In-game verification. **Before claiming anything works, work the matrix.**

**Rule zero, inherited and non-negotiable: a stale binary voids every test.**
The first line of `MFO.log` prints the version and the game version. Check it
before believing any result. MEO was bitten by this twice.

**Rule one:** keep a read-only baseline save that has never seen this plugin.
Reload it for every test of a build whose *records* changed. When in doubt,
clean reload — persisted state lies.

---

## ⚠ DEFERRED: everything that requires SAVING (marth, 2026-07-21)

**No saving with MFO active until further along.** That is the right call this
early, and it is cheap because the co-save callbacks only fire on save/load —
with `bSeedTestData = 0` and no saving, **MFO writes nothing anywhere.** It
reads state and draws it.

**What it costs, stated plainly so it does not get forgotten:**

- **The co-save stays UNPROVEN.** It is the highest-blast-radius subsystem in
  the mod — a schema bug there is the "mod ate my save" class — and it has
  been reviewed but never executed. Every session below that says "save, quit,
  reload" is deferred, not passed.
- Specifically deferred: session 1 step 9, tests A (load-order remap) and B
  (downgrade guard), and 2A steps 4/5 in their persistent form (dismissal
  *retention* is still observable live in the Field Kit, just not across a
  reload).

**Un-defer it with a dedicated throwaway save**, not the real playthrough —
one save file created solely to be destroyed. Do it before M5, because the
evaluator will start writing per-follower state that matters and a schema
defect gets more expensive every milestone.

---

## Session 1 — M1 + M2: the DLL loads, forms resolve, the co-save round-trips

The first real evidence the mod exists. Nothing here is gameplay.

### Package

```
Data/
  MFO.esp
  SEQ/MFO.seq
  SKSE/Plugins/MFO.dll
```

Build it with:

```bash
./tools/package_test.sh            # writes MFO-test.zip, installable in MO2
```

Install as a normal mod. **MO2 has TWO checkboxes** — the left-pane mod *and*
the right-pane plugin. A starless entry in `profiles/<P>/plugins.txt` is not
loaded, and the only symptom is MFO's own "form not found" line.

### Where the logs are (verified 2026-07-21 on this machine)

| Log | Path |
|---|---|
| MFO's own | `LoreRim/overwrite/SKSE/Plugins/MFO.log` |
| SKSE's | `~/Games/umu/489830/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/skse64.log` |

MFO writes game-root-relative on purpose so USVFS lands its log beside every
other plugin's. `skse64.log` is in the wine prefix instead — a **different
filesystem**; see `DEBUGGING.md` §0.

If `MFO.log` is missing:
`find /mnt/gaming/modlists/custom-modlist -iname 'MFO.log'` **before** concluding the
DLL never ran. (MRO's playbook cites a Proton appid and a `My Games/.../SKSE/`
path that do not exist on this machine — see `DEBUGGING.md` §0.)

### The matrix

| # | Step | Expect | Fails ⇒ |
|---|---|---|---|
| 1 | Launch, reach main menu | `skse64.log`: `plugin MFO.dll ... loaded correctly` | DLL not deployed, or MO2 checkbox |
| 2 | `MFO.log` first line | `=== MFO 0.0.1 loading — game 1.6.xxxx ===` | **stale binary — stop, redeploy** |
| 3 | Same log | `[forms] resolved MFO_FieldOrdersPower -> XX000801` | ESP missing/disabled, or FormID drift — run `tools/audit_esp.py` |
| 4 | Same log | `[forms] resolved MFO_GrantedSpell -> XX000802` | as above |
| 5 | Load any save | `[startup] kPostLoadGame — N follower record(s) live` | messaging listener not firing |
| 6 | Same | `[setup] granted Field Orders power` (first time) | form unresolved; check 3 |
| 7 | Console: `player.showinventory` / magic menu | **Field Orders** present as a Power | SPIT type wrong — audit checks this, so suspect deployment |
| 8 | Same log | `[p0] seeded 2 combat + 1 logistics gambit(s)` | seed guard tripped — already had data |
| 9 | **Save. Quit to desktop. Relaunch. Load that save.** | `[cosave] saved 1 follower record(s), schema v1` on save, then `[cosave] loaded 1 follower(s); dropped 0 ... disabled 0 ...` on load | **the P0 gate — this is the whole session** |
| 10 | Same log | `[setup] Field Orders power already present` | the grant is not idempotent |

### The two tests that actually matter

**A. Load-order change (proves `ResolveFormID`).**
Save with MFO active. Exit. **Enable or disable an unrelated ESP** so plugin
indices shift. Reload the save.

- Expect: the follower record still loads, and the gambit whose param is
  `0x00012FCD` (Healing) still resolves — its index moved and was remapped.
- A `disabled 1 rule(s) with missing targets` line here means resolution is
  broken, **not** that the mod is being cautious.
- Removing a plugin the rule depended on *should* disable that one rule and
  keep the rest. That is correct behavior, not a bug.

**B. Downgrade guard (proves the schema warning).**
Temporarily bump `kSchemaVersion` to 2, build, save with it, then revert to
the v1 DLL and load.

- Expect a **loud** `[cosave] SAVE IS NEWER (v2) THAN THIS DLL (v1)` error and
  **no records loaded**.
- If it silently loads anything, the guard is broken and users will lose data
  on the next save. This is the single most destructive failure mode in the
  co-save and it is worth the ten minutes to prove.

### Not in scope for session 1

No follower detection, no Rapport, no evaluator, no board. Using the Field
Orders power does nothing yet — it is granted so that step 7 can prove the
SPEL record is well-formed. **The board opener arrives at M7.**

---

## Session 2 — M3: follower detection + Rapport

**Written before the code, deliberately.** A gate defined afterwards is a
gate defined to fit whatever got built.

### Why the test list is the right one

LoreRim ships **Inigo**, **Auri**, **Lucien**, and **Simple Follower
Framework** — so the hardest detection case is already installed.

Inigo is the one `DESIGN.md` §3.1 got wrong twice, and the precise statement
matters because it decides what the test proves: **he sets `IsPlayerTeammate`
while following, but does NOT clear it when dismissed** — he signals that with
`GetActorValue("WaitingForPlayer") == -1` instead. So the teammate flag alone
reports him as an active follower *forever after you dismiss him*.

That makes **step 7, not step 6, the load-bearing one.** Detecting Inigo is
easy; *un*-detecting him is the bug.

There is **no debug UI at M3**. Everything below is read out of `MFO.log`,
so the logging has to be good enough to test with — which is itself part of
what this session proves.

### 2A. Detection

| # | Step | Expect | Fails ⇒ |
|---|---|---|---|
| 1 | Recruit a vanilla follower (Lydia / Faendal) | `[follower] +XXXXXXXX <name> (teammate)` | the base case is broken; stop here |
| 2 | Walk through a load door | still tracked, **no re-add spam** | handle re-resolution churning |
| 3 | Fast-travel far, then back | dropped from the active set on unload, re-added on load, **Rapport unchanged** | handle held as a raw pointer |
| 4 | Dismiss them | leaves the ACTIVE set; **record and Rapport retained** | dismissal is destroying state — the emotional core of §5 |
| 5 | Re-recruit | resumes with the **same Rapport**, no reset | keyed on something non-persistent |
| 6 | **Recruit Inigo** | detected as a normal teammate | the base gate is broken |
| 7 | **Dismiss Inigo** | **leaves the active set** — the quirk revokes despite the teammate flag still being set | **THE test of this session.** Still-active here means the quirk table never fired, and he would keep earning Rapport after dismissal |
| 8 | Recruit Auri and Lucien together | all three tracked simultaneously, distinct records | |
| 9 | Follower dies | leaves the active set cleanly, **no crash**, record retained | |
| 10 | Conjure a familiar | **NOT** tracked (`bAllowSummons` off) | summons would get a session-only board and pollute Rapport |
| 11 | `bAllowSummons = 1`, re-conjure | tracked, **and its record is NOT written to the co-save** | persisting a 0xFF FormID — INVARIANTS #9 |

### 2B. Rapport — the award rules

The awarding conditions are the part most likely to be subtly wrong, because
every one of them is a state scan against a non-player actor (#14).

| # | Scenario | Expect | Proves |
|---|---|---|---|
| 12 | Player melees a bandit, follower fighting beside them | **+1, exactly once** | `TESDeathEvent` **fires twice** — a +2 here is the classic |
| 13 | Follower kills a bandit themselves | +1 | follower-as-killer path |
| 14 | **Archery case:** player snipes from far away, follower is in combat | **+1** | the combat-state test, not distance |
| 15 | **Stealth case:** player one-shots from hiding, follower beside them and never aggroed | **+1** | the `fSharedRadius` fallback |
| 16 | Player kills something with the follower left behind, far away, not in combat | **+0** | the radius/combat gate actually gates |
| 17 | Dismissed follower, player kills something | **+0** | dismissed earns nothing |
| 18 | Kill a boss / named enemy | **+5** | multiplier |
| 19 | Kill a dragon | **+10** | multiplier |
| 20 | Fight ends | survival award fires **once**, on exit | not per-tick |
| 21 | **Long fight with lulls** | survival does **NOT** fire mid-fight | `aeCombatState == 0` is untrustworthy alone — §4.5a rule 7 |
| 22 | Two followers present, player kills one enemy | **both** get +1, independently | Rapport is per-follower, never pooled |
| 23 | Cross a rank threshold | rank increments, slot counts change (log both tables) | `BALANCE.md` ladder |
| 24 | Save, quit to desktop, relaunch, load | Rapport and rank **exactly** as before | |

### 2C. The performance question this milestone must answer

`TESCombatEvent` is a **global** event source — it fires for every actor in
the load order entering or leaving combat, not just followers. MRO's most
expensive lesson was that a global actor event taxed the whole VM and **the
cost was the dispatch, not the handler body, so filtering inside the handler
did not help.** A native sink is far cheaper than a Papyrus dispatch, but the
volume question is real and unmeasured.

| # | Step | Expect |
|---|---|---|
| 25 | Log every `TESCombatEvent` with actor + state for 60 s in a **large battle** (a dragon attack on a hold capital, or a Civil War fight) | a *count*, so we know the order of magnitude |
| 26 | Same, with MFO's handler early-outing on non-followers | no measurable frame cost |

**If dispatch volume is high enough to matter**, the mitigation is to gate on
`IsPlayerTeammate` **before** any other work in the sink, and to consider
whether combat-exit survival can be driven off the follower's own polled
state instead of a global sink. Decide on the number, not the vibe.

### 2D. The measurement `BALANCE.md` is waiting on

| # | Step | Records |
|---|---|---|
| 27 | Play normally for **3 sessions of ~1 h** with one steady follower, `bProfileRapport` on | kills/hour, boss kills/hour, encounters/hour, **Rapport/hour** |

`BALANCE.md` §1.1 assumes ~45 Rapport/hour on vanilla-ish play and ~30 on a
Requiem-class list. **The entire rank ladder rests on that estimate and it
has never been measured.** If the real number is half, Rank V is 220 hours
and the ladder is wrong. Feed the result back into `BALANCE.md` §1.2 before
M5 — the numbers are cheap to change now and expensive to change after
players have saves.

### Definition of done

2A and 2B fully green, 2C measured with a number written into
`ENGINE_NOTES.md` §9 item 3, and 2D's kills/hour recorded in `BALANCE.md` §7.
**Detection working on Inigo is the single most informative result in this
session** — it is the case that was designed wrong first.

---

## Session 3+ — added as milestones land

Per `ROADMAP.md`: M4 the stick-poking harness, M5 the evaluator, M6
logistics, M7 the board.

**Write each session's matrix before writing the code it tests.**

---

## Session 5 — M5: the evaluator (first playable)

**Written before the code.** This is the milestone that makes gambits execute.
The slice is deliberately narrow: cheap conditions, cast/wait actions, no
standing orders (§4.7 is unproven), no drink/equip (own build).

**Setup:** `bSeedEvaluatorRules = 1` in `MFO.ini` seeds a default combat rule
onto each detected follower — `Self HP < 40% → Cast Healing`, plus
`Always → Wait`. No board needed. Turn it off for anything but this test.

> **The evaluator only runs IN COMBAT.** The combat table is combat-only
> (§4.8); a wounded follower standing in town is *supposed* to do nothing.
> Testing out of combat will read as "it's broken" when it is behaving.

| # | Step | Expect | Failure means |
|---|---|---|---|
| 1 | Follower takes damage **in combat**, HP drops below 40% | **Follower casts Healing on itself** — `[eval] <id> fired rule 0 (act.cast_self)` in the log | The evaluator does not fire, or the condition does not read follower HP — **the entire thesis is unproven** |
| 2 | Follower at full HP, in combat | **Does NOT cast** (rule 0 false; `Always→Wait` consumes the tick) | Conditions read as always-true, or the evaluator acts unconditionally |
| 3 | `bSeedEvaluatorRules = 0` (empty tables) | **Behaves exactly as vanilla — zero `[eval]` action lines** | MFO is acting without a matching rule; the §4.4 do-nothing guarantee is broken |
| 4 | Follower wounded **out of combat** (town, after a fight) | **Nothing. No cast, no `[eval]` line** | The combat gate is missing — the combat table is running the logistics table's shift |
| 5 | Follower whose magicka is below the spell cost | **Does NOT cast; log shows `insufficient magicka`** and the tick falls through | Competence-is-not-permission (§5.3) not enforced |
| 6 | Same heal fires, then 1.5 s passes | **One cast, then quiet for the window** — not a cast every tick | Suppression not applied; follower spam-heals |
| 7 | `bProfileEvaluator = 1`, watch the log in a fight | **Per-tick wall time logged, well under a frame** | Perf problem — the hot path is too heavy |
| 8 | Two followers, both seeded | **Both heal when hurt; log shows round-robin, one serviced per tick** | O(N) tick — party size scales cost |

### 5b — the two open engine questions (Probe tab, ~2 minutes)

These are measurements, not pass/fail. Both change the design.

| # | Step | What to record |
|---|---|---|
| A | Fire **Cast Healing — kInstant**, then **kRightHand**, **kLeftHand**, **kOther**, watching the follower each time | **Which one plays a cast animation?** `kInstant` is known not to — it is the control. Whichever animates becomes `iCastSource` in the INI, no rebuild needed (ENGINE_NOTES §0.10) |
| B | ~~Does a cast spend magicka?~~ | **ANSWERED 2026-07-21: yes, it deducts** (ENGINE_NOTES §0.9). §5.3's gate is load-bearing; MFO must not deduct manually. Nothing to retest |
| C | Drain a follower to *below* a spell's cost, then let the heal rule win | **Log shows `insufficient magicka (X < Y)` and no cast.** MFO's own gate should refuse before the engine is ever asked — this tests OUR check, not the engine's |

**Definition of done:** steps 1–3 green (fire / don't-fire-when-false /
don't-fire-when-absent — the three that prove the evaluator is a program and
not a spammer), 4 green (the combat gate), 5 green (the competence gate), and
7 showing a sane number. 6 and 8 are refinements; note them if they misbehave
but they do not block. **A and B should be answered even if everything else
fails** — they are cheap and they unblock the next build.

**Read the log regardless of what the HUD shows.** Every fired rule and every
fall-through-with-reason logs at info.

---

## Session 6 — the attack verb (ENGINE_NOTES §0.14)

**Written before the code.** This is the decisive probe: one session answers
whether the redirect takes, whether it sticks, and how badly it fights the one
mod known to contend with it.

**Setup:** `bCommandTarget = 1` in `MFO.ini`. **Requires a MULTI-ENEMY fight** —
with one hostile present the test is inconclusive by construction, which is
exactly the mistake that wasted the §0.6 session.

### How you actually command a target

The gambit vocabulary has no attack action yet (that needs foe enumeration —
see below), so the hook is driven manually from the Field Kit:

1. Open the Field Kit, **Probe** tab, select the follower.
2. **Look at the enemy you want them on** — the crosshair is the picker.
3. Fire **`Command target (crosshair)`**.
4. **`Clear commanded target`** releases the latch and hands them back to the
   engine's own choice.

The log names who you latched: `latched onto Bandit (000ABCDE)`. If the hook is
not installed it says so on the same line rather than silently doing nothing.

| # | Step | Expect | Failure means |
|---|---|---|---|
| 1 | Load, check the log | `[target] UpdateCombat vfunc hook installed (Character vtbl idx 0xE4)` | The vtable index is wrong for this runtime — stop, nothing below is valid |
| 2 | Same line, if on LoreRim | `SmartNPCTargetSelector.dll IS LOADED` warning | Conflict detection failed; results are unattributable |
| 3 | Multi-enemy fight, latch a follower onto a foe they are **not** already fighting | Follower switches to that foe | **The redirect does not take** — the whole mechanism is wrong |
| 4 | Watch `assert / drift / pass` counts in the state report | `drift` counts the engine re-picking away from our choice | This is §4.7's real measurement — record the number either way |
| 5 | Kill the commanded foe | Follower moves on normally, no stuck state | The latch outlives its target |
| 6 | Let the foe flee / lose detection | Follower does **not** chase something it cannot perceive | The "engine already has a target" guard (#59) is not working |
| 7 | Run once with `bCommandTarget = 0` in the same fight | Baseline drift for comparison | Without this, step 4's number means nothing |

**Definition of done:** step 1 (hook installs), step 3 (redirect takes), and
steps 4+7 together (a drift number WITH a baseline). 5 and 6 are safety checks —
if either fails, the mechanism is not ready regardless of how well 3 worked.

### 6b — the animation question, answered by the SAME session (free)

Session 6 tests targeting, but it also answers animation at no extra cost,
because the two preconditions are both in this build.

**Setup:** `bSeedEvaluatorRules = 1` (so the follower has a heal gambit and
`bEquipToCast` puts it in their hand) **and** latch them onto a foe.

| # | Watch for | Meaning |
|---|---|---|
| A | `[cast] <id> <name> CAST Healing -- AI-fired` in the log | **The follower's own AI fired an MFO-equipped spell.** This is the animation answer (§0.15) — go look at whether it animated |
| B | No `[cast]` line at all across a long fight | The AI will not fire what MFO equips on a useful timescale — fall back to driving the `MagicCaster` state machine (§0.13 option 2) |
| C | `[cast]` line but still no visible animation | The premise is wrong at a deeper level than expected; report it, it changes the design |

**Do not confuse an AI-fired cast with an MFO-issued one.** `[eval] ... fired
rule` is MFO acting; `[cast] ... AI-fired` is the follower acting. Only the
second one can animate.

### Where a real gambit's target will come from

Not from a world scan. The reference implementation enumerates candidates from
`combatGroup->targets` under a read lock — the actors already in the fight,
which is both cheaper than sweeping `highActorHandles` and more correct, since
it cannot pick something the follower is not actually engaged with. That list is
what "lowest HP foe" / "foe weak to fire" will select over, and the hook is
already positioned to read it.

**The number that matters:** drift with the hook on should be *corrected* every
combat update. If drift climbs and the follower still visibly fights the wrong
foe, the write is landing somewhere the engine does not read.
