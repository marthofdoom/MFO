# Runbook — sessions 1 & 2 (do this, in this order)

Companion to `TEST_GUIDE.md`, which says *what* is being proved. This says
*what to press*.

**Use a throwaway save, not your real playthrough.** The M3 build seeds test
gambits onto a record keyed to the player, and `bAllowSummons` gets toggled.

---

## 0. Deploy (2 min, before launching)

```bash
cd /mnt/gaming/modlists/Projects/marth-follower-overhaul
./tools/package_test.sh          # rebuilds from the latest green CI run
rm -f /mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log   # start clean
```

Install `MFO-test-v0.0.1.zip` in MO2. **Tick BOTH boxes** — the mod on the
left, `MFO.esp` on the right.

---

## 1. Boot checks (5 min)

1. **Launch to the main menu. Stop there.** Check SKSE loaded the plugin:
   ```bash
   grep -i mfo "/home/marth/Games/umu/489830/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/skse64.log"
   ```
   Want: `plugin MFO.dll ... loaded correctly`.
   **Nothing? Stop — it isn't deployed.** Everything below is meaningless.

2. **Load any save.**

3. **Read the log:**
   ```bash
   cat /mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log
   ```
   Four things must be there:
   - `=== MFO 0.0.1 loading — game 1.6.…` ← if the version is wrong, **stop and redeploy**
   - `[forms] resolved MFO_FieldOrdersPower -> …0801`
   - `[forms] resolved MFO_GrantedSpell -> …0802`
   - `[p0] seeded 2 combat + 1 logistics gambit(s)`

4. **Find the power.** Magic menu → **Powers** → **Field Orders**. Equip it.
   *(Missing? The SPEL record is malformed — tell me, don't continue.)*

5. **Press the shout key.** This is your observation tool for everything
   below. It dumps a `MFO STATE REPORT` block to the log. Press it any time
   you want a snapshot; it does nothing else.

6. **The co-save test.** Save → **quit to desktop** (not main menu) → relaunch
   → load that save. Then:
   ```bash
   grep -E "cosave|seeded" /mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log
   ```
   Want `[cosave] saved 1 follower record(s)` and then
   `[cosave] loaded 1 follower(s); dropped 0 …`.
   **`loaded 0` = the whole thing is broken.** Stop and send me the log.

---

## 2. Detection (10 min)

The fast way — no questlines needed. Open console, **click an NPC**, then:

| Do | Console | Then |
|---|---|---|
| 7. Make anyone a follower | `setplayerteammate 1` | close console, wait ~3s, **press the power** |
| | | Want: `[follower] + XXXXXXXX <name> (teammate)` and them listed under ACTIVE |
| 8. Walk through any door | — | press the power. Still listed, and **no repeated `+` spam** in the log |
| 9. Dismiss | click them, `setplayerteammate 0` | wait 3s, press the power |
| | | Want: `[follower] - XXXXXXXX (record and Rapport retained)`, and under STORED they appear as `(inactive - retained)` — **still there, not deleted** |
| 10. Re-recruit | `setplayerteammate 1` | press the power. Same rapport as before, **not reset to 0** |

**11. The Inigo test — the one that matters.** He fakes the exact failure:
click any actor you've made a teammate, then

```
setav waitingforplayer -1
```

Wait 3s, press the power. **They must leave ACTIVE.** If they're still listed,
the quirk table never fired — that's the bug this milestone exists to catch.
Undo with `setav waitingforplayer 0`.

*(If you have Inigo handy, doing it through his real dialogue is the true
test. The console version proves the same code path.)*

**12. Summons.** Cast any conjuration. Press the power. The summon must **not**
appear under ACTIVE.

---

## 3. Rapport (10 min) — needs actual fighting

Go find a bandit camp with your follower.

13. **Kill one yourself, follower fighting beside you.** Press the power.
    Rapport **+1 exactly** — `+2` means the double-fire guard failed.
14. **Let the follower get a kill.** +1.
15. **Archery case:** back off, snipe something while they fight. +1.
16. **Negative case:** tell them to wait, walk far away, kill something.
    **+0.** Anything else means the gate isn't gating.
17. **A boss or dragon** if you happen across one: +5 / +10.

---

## 4. Send me this

```bash
cp /mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log /tmp/mfo-session.log
```

The whole file. The `MFO STATE REPORT` blocks carry the two measurements I
actually need:

- **`combat events (teammate-filtered): N in M min`** — tells me whether
  `TESCombatEvent`'s global dispatch is a performance problem before it
  becomes one.
- **`session: N kill(s) … => X kills/hr`** — `BALANCE.md` assumes ~45
  rapport/hr and **that number has never been measured.** If it comes back
  near half, Rank V is ~220 hours and the whole ladder needs redoing while
  it's still free to change.

Note anything that felt wrong even if it "passed" — a hitch, a pause, a
follower behaving oddly. Those are worth more than the pass/fail boxes.
