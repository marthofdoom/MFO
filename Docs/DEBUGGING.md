# MFO — Debugging Cookbook

Symptom → cause → fix.

**Honesty marker:** the sibling `DEBUGGING.md` files open with *"every entry
below was hit for real in this project."* MFO has hit none of them yet.
Entries here are tagged:

- **[SIBLING]** — hit for real in MRO / MEO / MAO. These will happen to MFO
  too; the cause and fix are known.
- **[PREDICTED]** — derived from MFO's architecture, not yet observed. Treat
  the *cause* column as a hypothesis to test, not a diagnosis.

**Replace [PREDICTED] with the version and the real symptom the first time
each one bites.** A predicted entry that turns out wrong should be deleted,
not quietly edited.

**The universal method when nothing below matches:** find something that
already works and diff against it. For records, dump a vanilla twin
subrecord by subrecord. For engine behavior, read the SKSE64 source for the
equivalent Papyrus native. For a framework call with no visible effect, read
its source.

**Two rules of numeric diagnosis** (violated in MRO's 1H-stall hunt, costing
two release cycles):
1. **When a computed number is wrong, log EVERY term of the formula.** MRO's
   diagnostic logged every term except the guilty one.
2. **Before shipping a fix, check the hypothesis reproduces the observed
   number.** A theory that cannot explain a number already in your log is
   wrong regardless of how plausible it sounds.

---

## 0. Environment facts (verified 2026-07-21 on this machine)

**Two instances are in play and they are not interchangeable.** Confusing them
cost a test session.

| Fact | Value |
|---|---|
| **TEST instance** (where MFO is installed and run) | `/mnt/gaming/modlists/custom-modlist`, profile **`Requiem`** |
| **SURVEY instance** (where the prior-art mod survey was done — *not* a test target) | `/mnt/gaming/modlists/LoreRim`, profile `Default` |
| Game path, test instance | `/mnt/gaming/modlists/custom-modlist/Stock Game` |
| Vanilla masters | `<instance>/Stock Game/Data/` |
| **MFO.log and most plugin logs** | `/mnt/gaming/modlists/custom-modlist/overwrite/SKSE/Plugins/MFO.log` |
| **`skse64.log` / `skse64_loader.log`** | `/home/marth/Games/umu/489830/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/` |
| Wine prefix | **umu**, `/home/marth/Games/umu/489830/` — *not* Steam compatdata |
| Saves + game INIs | under the umu prefix's `My Games/Skyrim Special Edition/` |
| `gh` CLI | `/home/marth/.local/bin/gh` (use the absolute path; auth in keyring) |
| capstone | installed, 5.0.7 |

### There are TWO log destinations, and which one a plugin uses is a choice

This tripped up an earlier version of this document, so it is worth being
exact:

1. **Game-root-relative writes** (`Data/SKSE/Plugins/X.log`) are redirected by
   MO2's USVFS into `<instance>/overwrite/SKSE/Plugins/`. This is where
   ActorLimitFix, BugFixesSSE, ScrambledBugs, BarterLimitFix,
   MainMenuRandomizer and NPCWaterAIFix all land — i.e. **most plugins in
   this setup**.
2. **`SKSE::log::log_directory()`** resolves into the *wine prefix's*
   `My Games\Skyrim Special Edition\SKSE\`, on a different filesystem. On this
   machine that is the umu prefix above, and it contains only skse64's own
   two logs.

**MFO deliberately uses (1)**, so its log sits alongside every other plugin's
where you are already looking. MEO uses (2), which is why `MEO.log` is not in
the Overwrite folder next to the others.

**Corrections to MRO's `PROJECT_PLAYBOOK.md` — verify before copying paths:**

- It cites Proton appid **3375297225**; no such directory exists here.
- It implies Steam compatdata. The live prefix is a **umu** prefix at
  `/home/marth/Games/umu/489830/`. Steam's `compatdata/489830` exists but has
  **no SKSE directory at all**, so a path built from it will silently find
  nothing.

A worked example of the doctrine: the documented environment had drifted from
the real one, and a search that "found nothing" was evidence about the docs,
not about the machine.

---

## 1. Build, deploy, and the stale-binary trap

| Symptom | Tag | Cause | Fix |
|---|---|---|---|
| Changed code, behavior identical | [SIBLING] | **Stale DLL.** The running game loaded a different binary than you built | **Check the log's version header before believing ANY in-game result.** MEO was bitten twice. `sha256sum` the artifact against the deployed file |
| DLL not in `skse64.log` at all | [SIBLING] | Not deployed, or MO2 mod unchecked | `skse64.log` prints `plugin MFO.dll ... loaded correctly`. **MO2 has TWO checkboxes** — left-pane mod AND right-pane plugin |
| ESP present but records missing in game | [SIBLING] | `plugins.txt` entry lacks the leading `*` | Starless = not loaded. The only symptom is your own "form not found" line |
| Plugin log missing entirely | [SIBLING] | Log path resolved somewhere unexpected | `find /mnt/gaming/modlists/custom-modlist -iname 'MFO.log'` before assuming the DLL didn't run |
| CI build fails on a `fmt`/CommonLib error after months of green | [SIBLING] | A floated vcpkg baseline picked up a breaking change | Baselines are **pinned deliberately, never floated** |
| API compiles locally in your head, fails in CI | [SIBLING] | Wrong CommonLibSSE-NG fork assumed | Verify signatures against **CharmedBaryon/CommonLibSSE-NG** raw headers. The forks diverge; no headers exist locally |

---

## 2. Startup and forms

| Symptom | Tag | Cause | Fix |
|---|---|---|---|
| A feature silently absent, no error | [SIBLING] | `TESDataHandler::LookupForm<T>` with an abstract intermediate — gates on `Is(FORMTYPE)`, returns nullptr **100%** | Use `TESForm::LookupByID<T>` or non-template lookup + `->As<T>()`. **Log every form resolution by name** |
| Global reads the wrong value in game | [SIBLING] | **GlobalVariable values are save-persisted** — a `kDataLoaded` write is overwritten when the save loads | Re-assert on `kPostLoadGame` AND `kNewGame` |
| One-time grant repeats every load | [SIBLING] | Latch set before the grant succeeded, or FormIDs changed between installs | Latch only on confirmed success; never change FormIDs post-release |
| Record absent from `help <edid> 4` but parses offline | [SIBLING] | Loader rejected the layout — silently | Dump vanilla twin, diff every subrecord |
| Field Orders power not granted | [PREDICTED] | Grant ran before the co-save loaded, or SPEL type wrong | Grant on `kPostLoadGame`/`kNewGame`, not `kDataLoaded`. Lesser power = SPIT type **3** |

---

## 2b. Rapport and detection — REAL, from field logs

| Symptom | Tag | Cause | Fix |
|---|---|---|---|
| Rapport visibly increments in game but the log says nothing | **[MFO v0.2.0]** | `Award()` only logged on a rank change or under `bProfileRapport`. The overlay landed and the log went silent for the same data in the same release | Log **every** award and every credited kill at info. An observation surface that replaces another must not *reduce* coverage |
| A kill credits nobody, seemingly at random | **[MFO v0.2.0]** | Follower briefly absent from `highActorHandles`, dropped instantly, and the death sink `Refresh()`es before awarding — so the award loop iterated a set the follower was not in. Signature in the log is `- id` then `+ id` **~100 ms apart**, far tighter than the pump interval | Hysteresis: hold a follower for `kMissesBeforeDrop` consecutive missed sweeps. **One sweep is not evidence of absence** |
| A "boss" kill awards the standard multiplier | **[MFO v0.2.0]** | `IsUnique()` alone. That means *named one-off actor*, which is not what a player means by boss — generic dungeon bosses (bandit chief with a boss bar) are leveled and not unique | Unique **OR** level ≥ player + `iBossLevelDelta`. Relative, so a chief is a boss at level 8 and not at 50 |
| Autosave writes a co-save record while "not saving" | **[MFO v0.2.0]** | Autosave and quicksave fire regardless of intent — `[cosave] saved 0 follower record(s)` appeared in a session where no manual save was made | Not a bug, but "I am not saving" is **not** under the player's control. Anything that must not reach a save has to be gated at the source, not by intention |

## 3. The evaluator — MFO's own failure surface

All **[PREDICTED]**. This is the subsystem with no sibling precedent, so
these are hypotheses with the diagnostic attached.

| Symptom | Likely cause | First diagnostic |
|---|---|---|
| **No follower ever acts** | Pump not running; or detection returns empty; or every tick skipped | Log tick count per minute and detected-follower count **including the zero case** — "ran and found nothing" must be distinguishable from "never ran" (`INVARIANTS.md` #46) |
| **One follower acts, others never do** | Round-robin cursor not advancing, or K stuck at 1 with a broken aging weight | Log the serviced actor id per tick for 60 s; the distribution should be even |
| **A rule never fires though its condition looks true** | Predicate reads the wrong subject (the player, not the follower) | **Suspect `INVARIANTS.md` #14 first.** Log the subject actor id inside the predicate. This is MEO's worst-blocker shape and MFO is built out of it |
| **A rule fires but nothing happens** | Action issued and rejected by the engine — insufficient magicka, unknown spell, no valid target | This is *by design* (`DESIGN.md` §5.3). The board must show the reason. If it shows nothing, the outcome recording is broken, not the action |
| **Follower acts twice on one match** | Suppression window not opened, or opened without recording the rule position | Log `(ruleIndex, suppressUntil)` on every actuation |
| **A high-priority rule waits on a low one** | Suppression treated as absolute rather than positional | `INVARIANTS.md` #26. Log the blocking rule's position alongside the blocked one |
| **Followers act in a visible rotation** | Jitter or shuffle not applied | Log service order over 20 cycles; a repeating sequence is the bug |
| **Burst of actions right after a load screen** | Catch-up loop (`last += interval`) | `INVARIANTS.md` #24. Log tick timestamps across a load; gaps are correct, bursts are not |
| **FPS drop that vanishes when MFO is disabled** | Evaluator over budget, or the world snapshot built every tick instead of lazily | `bProfileEvaluator`; split the timing by phase (snapshot / scan / actuate). **Do not guess which phase** |
| **Cost scales with party size** | Round-robin broken — servicing everyone per tick | Per-tick cost must be flat 1→12 followers (`ENGINE_NOTES.md` §9 item 6) |
| **Follower reacts instantly and robotically** | Jitter clamped away, or urgency override applied to everything | Log the drawn interval per service; it should vary |

**The zero-case rule matters more here than anywhere else in the mod.** An
evaluator that silently does nothing looks identical to an evaluator that
correctly found no matching rule. Every tick logs, at debug level, what it
did *including "nothing."*

---

## 4. The board

| Symptom | Tag | Cause | Fix |
|---|---|---|---|
| **One click performs the action twice** | [SIBLING] | `Selectable`-return OR'd with `IsItemClicked` — press frame + release frame | **`IsItemActivated()` only.** `INVARIANTS.md` #30. A generation check cannot catch it — the echo is stale in intent, not data. In MFO this deletes two rules |
| Selection jumps to the wrong rule after a reorder | [PREDICTED] | Selection keyed on row index | Key on rule id (`INVARIANTS.md` #31) |
| Rows draw outside the pane on long lists | [SIBLING] | Draw list fetched at window level | Fetch inside each `BeginChild` (`INVARIANTS.md` #32) |
| Menu draws off-center | [SIBLING] | `io.DisplaySize` from `GetClientRect` disagrees with the backbuffer under Proton | Overwrite from cached `sd.BufferDesc` every frame, between the two `NewFrame` calls |
| First click of a session misses | [SIBLING] | ImGui never learned the cursor position | Seed it on open |
| Menu opens then instantly closes | [SIBLING] | Opener key's RELEASE leaked to the game and re-cast the power | Close on release, swallow both edges, require a press seen while open |
| Board unavailable, game otherwise fine | [SIBLING] | D3D init failed | Each failure path logs a distinct reason and disables the menu. **The evaluator must still work** (`INVARIANTS.md` #25) — if followers also stopped, that coupling is the bug |
| Gamepad nav dead in one pane | [SIBLING] | Missing `NavFlattened`, or pane jump left to spatial nav | Deterministic pane jump via `SetKeyboardFocusHere()` |
| Stick scrolls continuously | [SIBLING] | Thumbstick passed as an axis instead of edge-triggered | ±0.5 edge trigger into d-pad keys |
| Focus ring invisible on a skin | [SIBLING] | `ImGuiCol_NavHighlight` not set from the skin accent | Set it per skin |

---

## 5. Config and MCM

| Symptom | Tag | Cause | Fix |
|---|---|---|---|
| A tuning value behaves ~100× off | [SIBLING] | **A key's SEMANTICS changed under the same name.** MCM Helper persists per key name into MO2 overwrite and survives updates | **Rename the key.** `INVARIANTS.md` #37. Print the LIVE value before theorizing |
| A new MCM control reads as OFF | [SIBLING] | Key absent from an existing Settings ini; MCM Helper does **not** fall back to `config.json` defaults | Backfill on deploy (`INVARIANTS.md` #39) |
| Removing a key doesn't restore the default | [SIBLING] | Parser doesn't reset before parsing | Reset-then-parse every pass |
| A feature silently disabled | [SIBLING] | Unparseable value became `0.0` | Skip and warn; never apply a failed parse |
| First key of the file ignored | [SIBLING] | UTF-8 BOM | Strip it |
| MCM changes need a restart | [SIBLING] | Config only read at startup | Re-read on JournalMenu close **and** board open |

---

## 6. Crashes

- **Read the crash log's top frames for `MFO.dll+offset`.** If MFO is nowhere
  on the stack, that is *not* exoneration — MEO's use-after-free crashed at
  an engine offset with MEO absent from the stack, because the corruption was
  handed across an API boundary earlier.
- **Suspect lifetime first**: a raw `Actor*` held across a tick, a snapshot
  read after the underlying actor unloaded, an `ExtraDataList` whose
  ownership was inferred rather than proven.
- **One hook or mechanism per release** exists precisely so a CTD bisects to
  one change (`INVARIANTS.md` #45).
- Disassemble a live dump with capstone (5.0.7, installed) when hunting a
  relocated site. Validate the Address Library `.bin` against crash-log
  ground truth **first** — the wrong `versionlib-*.bin` parses cleanly and
  yields plausible-but-shifted addresses.
- **The on-disk exe is Steam-DRM encrypted.** Static byte reads of code
  sections return garbage and will "prove" a mismatch that isn't real.
  Verify against `/proc/<pid>/mem` of the running game.

---

## 7. Console verification

```
help MFO_ 0                 list MFO's forms
player.getav Health         sanity-check AV reads
<click follower> getav Magicka     the follower's actual magicka
                            (the §5.3 "insufficient magicka" case)
<click follower> getcombatstate    0 none / 1 combat / 2 searching
sqv <questid>               quest state if the MCM quest misbehaves
setstage ski_configmanagerinstance 1    force SkyUI to rescan MCMs
tfc / tm                    clean screenshots of board layout issues
```

**Zero-code controls beat code diagnostics.** Before instrumenting, ask
whether a hand-authored rule list, a console-summoned test actor, or a
deliberately impossible condition would answer the question faster. MEO's
worst UI mystery fell to a renamed control item, not to more logging.

---

## 8. Test discipline

- **Keep one read-only baseline save that has never seen the plugin.** Reload
  it for every test of a build whose records changed. When in doubt, clean
  reload — **persisted state lies.**
- **Do not validate an editorID lookup on this machine alone.** po3 Tweaks
  caches editorIDs for all forms and LoreRim ships it, so lookups that pass
  here fail for Nexus users.
- **A behavior test needs the MFO-absent baseline for comparison** —
  `DESIGN.md` §4.4's "does nothing when no rule matches" guarantee is only
  meaningful as a diff.
- **Test on a Lorerim-class order, not an empty cell.** Every performance
  number in `BALANCE.md` is meaningless otherwise, and the outfit-manager
  re-entrancy trap (§1.1 of `ENGINE_NOTES.md`) only exists in a real order.
