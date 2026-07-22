# MFO — Test Guide

This is the document you read **while the game is running**, so the thing you
need next is at the top and the history is at the bottom.

---

## Before you start — 30 seconds, skip it and the results are worthless

**1. Check the binary is the one you think it is.**
First line of `MFO.log`:

```
=== MFO 0.5.1 loading — game 1-6-1170-0 ===
```

If that version is not the build you just installed, **stop**. Everything below
is meaningless. This has bitten the sibling projects twice.

**2. Know where the log is.**

| | |
|---|---|
| MFO's log | `custom-modlist/overwrite/SKSE/Plugins/MFO.log` |
| SKSE's log | `~/Games/umu/489830/drive_c/.../My Games/Skyrim Special Edition/SKSE/skse64.log` |

Those are on **different filesystems** — SKSE's lives inside the wine prefix.
If `MFO.log` is missing, search for it before concluding the DLL never ran.

**3. Use a throwaway save.** Not your real playthrough. Keep one save that has
never seen MFO and reload it whenever records change — persisted state lies.

---

## ⚠ One standing caution

Anything that mutates a follower's **equipment** (the equip policy) can strand
gear if it misbehaves. Nothing is destroyed — the failure mode is "my follower
stopped using her shield". If you see that, say so; it is a real bug, not a
quirk.

---

## THE NEXT SESSION — targeting, and animation for free

This one session answers the two questions the whole mod is waiting on:
**can we tell a follower who to fight**, and **does a follower ever cast with an
animation**.

### Set this in `MFO.ini`

```ini
bCommandTarget      = 1     ; installs the targeting hook
bSeedEvaluatorRules = 1     ; gives the follower a heal gambit
bEquipToCast        = 1     ; puts that spell in their hand
fAiCastGrace        = 3.0   ; seconds their AI gets before MFO casts for them
```

**`bCommandTarget` needs a game restart to turn ON** (the hook installs once at
load). Turning it off works live.

**Change these one at a time across sessions.** If two new mechanisms are on and
something misbehaves, you can't tell which one did it.

### You need a fight with MORE THAN ONE enemy

With a single enemy this test proves nothing — the follower would attack it
anyway, and we already wasted a session learning that. Bandit camps are ideal.

---

### Part 1 — can we command a target?

**Do this:**

1. Get into a multi-enemy fight.
2. Open the Field Kit, go to the **Probe** tab, pick your follower.
3. **Look directly at an enemy they are NOT currently fighting.** Your crosshair
   is the picker.
4. Hit **`Command target (crosshair)`**.

**What success looks like:** the follower breaks off and goes for the enemy you
were looking at. The log says who:

```
[probe] Command target (crosshair) -> OK (latched onto Bandit Archer (000ABCDE))
```

**What failure looks like:**

- *"nothing under the crosshair"* — you weren't actually looking at an enemy.
- *"BUT HOOK IS NOT INSTALLED"* — `bCommandTarget` is still 0.
- Log says latched, follower ignores it — **the mechanism is wrong.** This is the
  important failure; tell me and stop here.

**Then hit `Clear commanded target`** and check they go back to choosing for
themselves.

---

### Part 2 — two things worth breaking on purpose

**Kill the enemy you commanded.** The follower should just move on normally. If
they freeze or keep swinging at a corpse, the latch is outliving its target.

**Let an enemy flee or break line of sight.** The follower should **not** chase
something they can't see. If they do, a safety guard isn't working — that's the
failure that makes a mod feel haunted rather than broken, and it's worth
reporting even if everything else works.

---

### Part 3 — the animation question (free, same fight)

Just watch the log during that same fight. Two very different lines:

| Line | Who acted | Can it animate? |
|---|---|---|
| `[eval] ... fired rule 0 (act.cast_self)` | **MFO** cast it | No — proven, three times |
| `[cast] ... CAST Healing -- AI-fired` | **The follower** cast it | **Yes — this is the one that matters** |

A `[cast]` line means the follower's own AI fired a spell MFO put in their hand.
**Go look at them when it happens.** If that cast has an animation, the animation
problem is solved and the mod works.

If you get through a long fight with no `[cast]` line at all, the AI won't fire
what we equip on a useful timescale, and we go to the fallback plan.

---

### What I need back from you

- Did the follower switch targets? (Part 1)
- Did they chase anything they couldn't see? (Part 2)
- Any `[cast]` line — and if so, **did it animate**? (Part 3)
- The log.

Everything else is detail.

---

### Also useful, if you have the patience

Run the same fight once with `bCommandTarget = 0`. The state report prints
`targeting: ... N assert(s), N drift(s)` — without a baseline those numbers
don't mean anything.

---

## Reference — where a real gambit's target will come from

Not the crosshair, and not a world scan. The engine keeps a list of who is
actually in the fight (`combatGroup->targets`), and that is what "lowest HP foe"
or "foe weak to fire" will sort over once the vocabulary has an attack action.
Cheaper than sweeping every loaded actor, and it can't pick something the
follower isn't even engaged with.

---

## Previous session — the evaluator (still worth re-checking)

**Set:** `bSeedEvaluatorRules = 1`. Each follower gets `Self HP < 40% → Cast
Healing`, then `Always → Wait`.

**The evaluator only runs IN COMBAT.** A hurt follower standing in town doing
nothing is behaving correctly, not broken.

The three that matter:

1. **Follower drops below 40% in a fight** → they heal. Log: `[eval] ... fired
   rule 0`.
2. **Follower at full health** → nothing happens. If they heal anyway, conditions
   aren't being read.
3. **`bSeedEvaluatorRules = 0`** → *zero* `[eval]` action lines. MFO must be
   invisible when it has no rules to run. If anything fires here, the mod is
   acting without being told to, which breaks its compatibility promise.

Worth a glance: a follower with no magicka should log `insufficient magicka` and
**not** cast. And `bProfileEvaluator = 1` prints per-tick timing if you want to
see the cost.

---

# Archive — completed sessions

Kept for the gotchas, not the steps.

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

