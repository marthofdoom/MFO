# MFO — Balance

The tuning math behind `DESIGN.md`: the Rapport ladder, its content budget,
the reaction-spread curve, and the numbers §4 and §5 defer to this file.

**STATUS: derived, not validated.** Every number here comes from a stated
model with stated assumptions. None has been measured in play. The models are
the durable part; the numbers are the first hypothesis and are expected to
move. Where a number is a pure guess it says so.

**Doctrine (inherited from MEO):** thresholds are derived from **content
budgets, not vibes**. A number nobody can explain is a number nobody can
retune when a load order changes it.

---

## 1. The content budget

### 1.1 Rapport income model

The unit of progression is **an hour adventuring with a specific follower**.
Everything below derives from one estimate:

| Source | Rate (per hour, one follower present) | Rapport each | Per hour |
|---|---|---|---|
| Standard shared kills | ~25 | 1 | 25 |
| Boss / named kills | ~2 | 5 | 10 |
| Dragons | ~0.2 | 10 | 2 |
| Combat encounters survived together | ~10 | 1 | 10 |
| | | **Total** | **≈ 45 / hour** |

**Assumptions, all needing playtest validation:**
- ~25 kills/hour is *average* play — travel, dialogue, and crafting included,
  not a dungeon-clearing hour (which runs 40–60).
- **A Requiem/LoreRim-class list runs slower and deadlier: 15–20 kills/hour
  is realistic**, so a heavily modded install earns ~30/hour, not 45. This is
  the single biggest source of variance between installs and is why
  `fRapportRate` exists (§5).
- Encounter count assumes ~10 distinct fights/hour, each awarding once on
  combat exit (not per tick).

### 1.2 Ladder thresholds

Design intent from `DESIGN.md` §5.2: Rank III near the end of a mid-length
questline with one steady companion; Rank V a long-haul commitment to **one**
follower; serial follower-swapping deliberately slower than loyalty.

| Rank | Cumulative Rapport | Hours together @45/hr | @30/hr (Requiem-class) | Reads as |
|---|---|---|---|---|
| **I** | 0 | — | — | recruited |
| **II** | **250** | ~6 | ~8 | a few dungeons in |
| **III** | **1,000** | ~22 | ~33 | one questline together |
| **IV** | **2,500** | ~55 | ~83 | a companion, not a hireling |
| **V** | **5,000** | ~110 | ~165 | the long haul |

**Curve shape:** roughly ×2 per rank after II. Early ranks arrive fast enough
to teach the system (a player must reach Rank II before they believe the
board matters); late ranks are a commitment.

**Rank V is the number most likely to be wrong.** 110 hours with one follower
is deliberately steep — it is the "you took Lydia everywhere" reward — but it
risks being unreachable for players who rotate companions or run shorter
playthroughs. First tuning lever if it proves discouraging: drop to 4,000
(~89 hr). Do **not** solve it by raising income, which compresses the early
ranks too.

**No decay.** Rapport is never lost, never decays with time apart, and
survives dismissal (`DESIGN.md` §3.1). A follower left in Breezehome for 200
hours is exactly as they were. Decay would punish exactly the attachment the
system is trying to reward.

### 1.3 What does NOT scale Rapport income

Deliberate exclusions, each closing an exploit or an incoherence:

- **Not player level, not follower level.** Time together is time together; a
  level-50 pair does not out-earn a level-10 pair. Otherwise Rapport becomes
  a function of when you recruited rather than how long you fought.
- **Not enemy difficulty beyond the boss tier.** Boss/dragon flags are a
  coarse 5×/10×; no per-enemy scaling. Fine-grained difficulty scaling in a
  modded list is unknowable and would make the ladder unpredictable per
  install.
- **Not gambit firing.** `DESIGN.md` §5.1 — rewarding rules for firing
  incentivizes spammy rules. Rapport measures time fought together, not
  automation quality.
- **Not kills the follower makes while the player is absent.** Requires
  shared presence (§2).

---

## 2. Shared-kill credit — the radius question (RESOLVED)

`DESIGN.md` §11 left this open. It decides whether archery and stealth builds
earn Rapport at all, so a pure distance test is the wrong instrument.

**Rule: a kill is shared if the follower is IN COMBAT at the moment of the
kill, OR is within `fSharedRadius` of the player.**

| Test | Catches |
|---|---|
| Follower `GetCombatState() != 0` | The archer case — player snipes from 200 m while the follower fights. They are participating; distance is irrelevant. |
| Within `fSharedRadius` (default **3,000 units**, ~42 m) | The stealth case — player one-shots from hiding, follower is beside them and never aggroed. |

3,000 units is roughly a large room or a courtyard: generous enough that a
follower a few paces behind always counts, tight enough that one left at the
dungeon entrance while you clear the far end does not. Combat state carries
most of the weight; the radius is the fallback for fights that end before the
follower engages.

**Follower's own kills always count**, regardless of where the player is —
they were unambiguously fighting.

---

## 3. Reaction-spread curve (owed by `DESIGN.md` §4.1b / §5.2)

The mechanism is in §4.1b: jitter the **observation time**, never queue the
action. This section supplies the parameters.

**Model.** The scheduler heartbeat is `T = max(4 frames, 133 ms)`. Each
follower is serviced every **M ticks**, where M derives from Rapport rank.
Mean response to a condition is ~`M·T / 2` (uniform phase), with a tail from
the miss chance.

| Rank | M (ticks between service) | Interval @30 fps | Mean response | Miss-a-beat | Worst realistic |
|---|---|---|---|---|---|
| **I** | 9 | 1.20 s | ~600 ms | 12% | ~2.4 s |
| **II** | 8 | 1.06 s | ~530 ms | 9% | ~2.1 s |
| **III** | 6 | 0.80 s | ~400 ms | 6% | ~1.6 s |
| **IV** | 5 | 0.67 s | ~330 ms | 4% | ~1.3 s |
| **V** | 4 | 0.53 s | ~265 ms | 2% | ~1.1 s |

**Why these land where they do.** `DESIGN.md` §4.1 sets the honest target as
the human *choice*-reaction band, 300–600 ms. Rank I sits at the slow end —
visibly deliberate, occasionally caught napping. Rank V sits at the fast end
— an expert who has seen this fight before. **No rank is superhuman**: even
Rank V averages 265 ms, comfortably above the 133 ms reflex floor.

**Jitter:** ±25% on M, redrawn each cycle, clamped to `M >= 3`. So a Rank V
follower's interval wanders 400–665 ms and a Rank I's 900 ms–1.5 s. The clamp
keeps anyone from sampling faster than 3 ticks (~400 ms), preserving headroom
under the floor rule.

**Miss-a-beat** is a whole skipped service turn, not a shortened one — the
follower simply did not notice this cycle. It is the single most human
artifact the system produces and it is why the "worst realistic" column is
roughly double the interval rather than equal to it.

**Urgency override (§4.1b).** Critical-tier conditions (self/ally at low HP,
paralyzed) **halve M and suppress the miss chance** for that evaluation, so a
dying follower is not left waiting on a bad draw. Routine-tier conditions use
the table unmodified. This is what makes the deviation read as attention
rather than lag.

**Urgency affects NOTICING SPEED ONLY — never priority.** A critical
condition is sampled more often and more reliably; it does not jump the
queue. Which rule wins is decided by the player's ordering alone (§6). The
two must never be conflated: the moment MFO decides that its notion of
"critical" outranks the player's rule 1, the list stops being a program.

**Performance note, and it points the right way:** a Rank I follower is
serviced *less* often than a Rank V one, so a party of new recruits is
cheaper than a party of veterans. Cost rises with investment, is bounded by
§4.1a's O(K) tick, and never exceeds the K-per-tick cap regardless of rank.

---

## 4. Slot ladder cross-check

From `DESIGN.md` §5.2, restated with the hours attached:

| Rank | Slots | Hours (@45/hr) | Slots per hour invested |
|---|---|---|---|
| I | 2 | 0 | — |
| II | 4 | 6 | 0.33 |
| III | 6 | 22 | 0.09 |
| IV | 8 | 55 | 0.04 |
| V | 12 | 110 | 0.02 |

Sharply diminishing, deliberately. The first two slots arrive fast because a
2-slot board cannot express anything interesting; the last four are the
long-tail reward. **Rank V grants 4 slots at once** (8 → 12) rather than 2, so
the final rank feels like an arrival rather than another increment — the same
shape as MAO's bulk capacity milestones.

---

## 5. Tuning surface

All keys live in `SKSE/Plugins/MFO.ini`, overridden by MCM Helper's
`MCM/Settings/MFO.ini` (`DESIGN.md` §7).

| Key | Default | Effect |
|---|---|---|
| `fRapportRate` | 1.0 | Global multiplier on all Rapport income. **The primary lever** — a slow, deadly list should raise it (1.5 for Requiem-class); a fast vanilla list may lower it |
| `iRapportRank2` | 250 | Rank II threshold |
| `iRapportRank3` | 1000 | Rank III threshold |
| `iRapportRank4` | 2500 | Rank IV threshold |
| `iRapportRank5` | 5000 | Rank V threshold |
| `fRapportKill` | 1.0 | Per standard shared kill |
| `fRapportBossMult` | 5.0 | Boss/named multiplier |
| `fRapportDragonMult` | 10.0 | Dragon multiplier |
| `fRapportSurvival` | 1.0 | Per encounter survived together |
| `fSharedRadius` | 3000 | Shared-kill fallback radius, units (§2) |
| `fReactionMult` | 1.0 | Scales M across all ranks — >1 slower/more deliberate, <1 sharper. **Cannot push effective sampling below 3 ticks** |
| `fSuppressWindow` | 1.5 | Fallback post-action suppression in seconds, for actions with no knowable duration; jittered ±30%. Positional — never blocks a higher-ranked rule (§6) |
| `bProfileEvaluator` | 0 | P2 timing instrumentation (§4.2) |

**Rename discipline (inherited, load-bearing):** an INI/MCM key that changes
**semantics** must be **renamed**, not redefined. MCM Helper persists values
per key name into MO2's overwrite and they survive mod updates — MEO's
absolute-to-multiplier change on an unchanged key silently cut an XP stream
~100× for every upgrading user. If `fRapportKill` ever becomes a multiplier
rather than an absolute, it gets a new name.

---

## 6. One action per tick — the FFXII contract (confirmed)

Stated here because it is a balance property as much as a design one.

**Exactly one rule fires per follower per service tick.** Evaluation runs
top-down **from rule 1 every tick** — no program counter, no resumption — the
first true condition executes its action, and every rule below it is not
evaluated at all this cycle (`DESIGN.md` §4.3). The list is a priority table
re-read continuously, not a script that advances. This is FFXII's contract
verbatim, and three consequences follow that matter for tuning:

1. **Rule order is priority, absolutely.** There is no scoring, no
   best-match, no tie-breaking. Rule 1 beating rule 2 is the entire
   mechanism.
2. **A follower cannot heal *and* attack in one cycle.** Multi-action turns
   would make slot count a raw power multiplier and break §4's budget. The
   cost of a heal is the attack you didn't make — that tension is the game.
3. **Evaluation cost is bounded by match position, not list length**
   (`DESIGN.md` §4.1b), which is why a full board carries no penalty.

**The subtlety worth naming: can a rule interrupt an action already in
flight?** Yes — and the answer follows from the contract rather than needing
a special case.

An earlier draft of this document ruled that "critical-urgency conditions
break the suppression window." **That ruling is RETRACTED.** It was wrong on
FFXII fidelity and wrong on principle: it used MFO's own urgency tiers to
decide that some rules outrank others, which is a second priority system
competing with the player's ordering — exactly what `DESIGN.md` §4.3a
forbids.

**Correct ruling: suppression is positional.** A rule **above** the one that
fired may preempt it at any tick; a rule at or below its position may not.
This is what "re-scan from the top every tick" *means* — priority is
re-asserted continuously, so a first-ranked rule never waits on a
sixth-ranked one. Consequences:

- **The dying-follower case solves itself**, correctly and without a carve-
  out. A player who puts "Self: HP < 25% → Heal" at the top gets instant
  preemption of anything below it. A player who buries it at slot 9 does
  not — and that is *their* ordering decision, made legible (§5.3), not
  overridden.
- **Thrashing is structurally impossible** between a rule and anything below
  it. Two rules alternating at positions 3 and 4 cannot fight, because 4 can
  never preempt 3. No damping heuristic needed.
- **Urgency tiers stay in their lane.** They set *reaction spread* (§3 above,
  how fast a follower notices) and nothing else. They no longer touch
  priority, which is the player's alone.

Suppression duration is per-action rather than a flat 1.5 s: a cast
suppresses for its cast time, a potion for its animation, an equip swap for
almost nothing. `fSuppressWindow` is the fallback for actions with no
knowable duration.

---

## 7. Open, deferred to playtest

- **Rank V at 5,000** (§1.2) — the most likely number to move. Fallback 4,000.
- **Kills/hour on a Requiem-class list** (§1.1) — the whole ladder rests on
  it. Measure it before trusting any hour figure in this document.
- **Whether the miss-a-beat chance reads as charm or as jank** (§3). It is
  the highest-variance feel decision here. If it reads as jank, cut the
  chance rather than the jitter — irregular timing is the goal, missed
  turns were the flourish.
- **Whether positional preemption (§6) makes followers twitchy** when a top
  rule flickers in and out of true. Expected fix if so is a brief re-fire
  guard on the *same* rule, never a priority override.
