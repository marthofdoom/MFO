# HMS rate + magicka cost research (ClickUp 86e3d6dnv, 86e3d6dp0)

Opus 5.5 research agent, 2026-11-09. READ-ONLY. Code = origin/main `ed70d36` (v2.8.2). Field = `_evidence/*/MFO.log*`
(LoreRim 2026-10-27..11-01, Deck/Tuxborn 2026-10-27e, 11-02). Records/GMSTs read from the installed lists
(LoreRim profile `Ultra`, Tuxborn profile `Tuxborn - Deck`) by walking plugins.txt in order. Engine formula
disassembled from `binaries/1.6.1170/SkyrimSE.unpacked.exe` with `versionlib-1-6-1170-0.bin`
(sanity: 38048 -> 0x6A35F0 per binaries/README). Working log: `scratchpad/agentlogs/hms-magicka-research.md`.
NOTE: both task texts ask for the working notes as ClickUp comments; this brief forbade ClickUp writes, so
they are here. The coordinator should paste them.

Both tasks were filed 2026-09-23. Since then main has shipped PRGN v8 player-rate parity (09-23),
the v2.0.12 "charge for time" magicka billing (which cites 86e3d6dp0), and on 11-02 the total cap
(4aa8754, ClickUp 1dfcuabc4dv, in v2.8.0+). The answers below are against current main.

---------------------------------------------------------------------------------------------------
## TASK 1 — 86e3d6dnv: follower HMS gain vs the player's rate

### Q1. Player per-level gain vs MFO's per-level grant (like for like)

Both sides are measured the same way: the BASE AV sum H+M+S (`Followers::GetFollowerHMS` =
`GetBaseActorValue`, `Followers.cpp:851`). The player's gain is sampled on each player level-up
(`progression/Poll.cpp:108-131`).

| list | player per level (GMST `iAVDhmsLevelUp`) | NPC engine per level (`iAVDhmsLevelUp + fNPCHealthLevelBonus`) | field |
|---|---|---|---|
| LoreRim | **5** (Requiem.esp) | 5 + **2** (Big Tweaks.esp) = **7** | `player HMS gain 5.0` x3 (11-01 L37->38, 38->39 twice); Auri `engine award 7.0` x3 |
| Tuxborn | **10** (vanilla) | 10 + **5** (vanilla) = **15** | 11-02 Deck: `02002B74 engine award 15.0 | player-rate credit 10.0 | applied 10.0 withheld 5.0` |

What MFO grants per level:
* **Leveling follower** (`Hms.cpp:432-458`): `applied = min(engineAward, hmsParityCredit)`. The rest goes to
  `hmsWithheld`. Every player level adds `playerGain` to the credit of EVERY record (`Poll.cpp:130`). So over
  the follower's life the applied total is at most the player's gain since capture: lifetime parity, not
  per-level parity. Credit banked earlier (for example, levels while he was benched) lets single levels exceed
  the player's rate. Field: Auri's credit went 16 -> 9 -> (+5) 14 -> 7. She got 7 a level while the player got
  5, using banked credit that runs out after 2-3 more levels.
* **Fixed-stat follower** (`Poll.cpp:245-283`): grant = `max(0, playerTotal - npcBaselineTotal - granted)`,
  so his total converges to the player's total and then tracks the player's gain. Field: Jesper `00013491`
  `playerGain 5.0 -> backfill budget 5.0` (equal).

### Q2. Anything granting more than once per level?
* **Base feature vs add-on: NO double grant.** The only HMS writers are `Hms.cpp:657` (the hold) and
  `:841/:895` (UndoHms on roster removal). The add-on's code path reaches HMS only through `RecomputeHMS`.
  It is called from `Poll.cpp:185` (add-on level gain), `Poll.cpp:283` (the drift watch) and
  `PerkGate.cpp:623` (reapply, log off). A second call in the same poll measures drift against the target the
  first call just held, which is 0.
* **Re-grant on load: NO, field-proven idempotent.** The 11-01 log `MFO.log.1` replays the same 38->39
  level-up twice after a reload (14:02:47 and 14:08:46). Both times: credit 14.0, award 7.0, applied 7.0,
  result base H 541 / M 127 / S 408.
* **Re-grant on class change / respec:** class only changes the split (profile). The budget is the measured
  award or the grant. Nothing found that re-grants.
* **HOLE (new finding, low severity, bounded):** `Poll.cpp:130` credits fixed-stat records too, and the
  grant path never spends credit. If a record flips from fixed-stat back to leveling (`Poll.cpp` `gotAward ->
  fixedStat=false`, for example an NPC whose level multiplier makes him skip two player levels, or a raised
  level cap), the credit banked during the grant levels lets the next engine awards through ON TOP of the
  grants already given. Bound: the grant only fills up to the player's total, and the cap
  (`bHmsCapToPlayerTotal`, default ON) trims anything above it. So this can make a follower REACH the
  player's total early. It cannot push him past it. Fix shape (tier A, co-save-adjacent semantics, no layout
  change): zero or skip the credit for a record while `fixedStat` (or when the grant path spends a budget).
  Suggest SEV-4 for the backlog. Rule 9 carve-out (a) does not apply, because no layout changes.

### Q3. Is the BASELINE the same? YES, it is the root cause of "totals significantly higher".
An uncaptured record ADOPTs the follower's live base with no take-back (`Hms.cpp:375-395`). A follower who
joins at level N brings his NPC record's base (race start plus N levels at the NPC rate of 7 or 15) into
MFO. Field (11-01, MFO 2.7.5, before the cap): **Auri base H534 + M127 + S408 = 1069 vs the player's 545**
at player level 38. ClickUp 1dfcuabc4dv's 11-01 diagnosis had already traced her 1076 to her adopted NPC
record. So the RATE was right after v8, and the TOTALS were high by construction.
marth ruled (D) on 11-01: cap the total at the player's. Shipped in 4aa8754 (v2.8.0+): `Hms.cpp:585-640`,
INI `bHmsCapToPlayerTotal` default ON (`Config.cpp:458`). The cap uses the level-up-sampled player total,
trims the pools above their class share, and keeps a 50-point per-pool floor. Field confirmation (11-02
Deck): `adopted total capped: Cicero 750 -> 730 (player 730)`.
**Intent as it now stands in code: a follower's base H+M+S total never exceeds the player's, and its growth
never outruns the player's gain over its lifetime.**
Caveat for marth's eyes: the cap is on BASE AVs. The values a player sees in game are maxima (base plus
race, ability, perk and enchantment modifiers). Auri's live magicka reached 867 while her held base Magicka
was 127, so roughly 740 comes from modifiers MFO does not touch. A follower can still LOOK far above the
player after the cap.

### Q4. PRGN co-save double-counting across sessions?
None found. The v<8 retro (`HmsRetroParity`, `Hms.cpp:729-783`) runs once (the next save is v8).
The never-processed-block adopt (`Hms.cpp:321-337`) is armed only at load and is once-only. Credit and
withheld persist (`Allocator.cpp:837`), and the reload replay above is byte-for-byte consistent. The
fix-forward sweep for old over-cap saves is the cap itself: it runs on the first `RecomputeHMS` after load.
Side observation (not inflation): Jesper `00013491` on LoreRim logs `outside change: live base H209 M132
S152 | MFO last held H240 M108 S197`. MFO's hold does not stick on him. Something re-slams his base, so his
fixed-stat grant is not landing, and he is LOWER than MFO thinks (MFO-B70, observe-only).

### VERDICT — Task 1
**Root cause found, and ALREADY FIXED on main (v2.8.0+, 4aa8754). Pending a field confirm.**
The totals were high because of the adopted baseline (Q3), not the rate. The rate has been at player parity
(lifetime) since PRGN v8. One residual low-severity hole remains (Q2: fixed-stat credit banking, bounded by
the cap).
Field confirm needed (no new code): one session on v2.8.x. Expect these lines:
* `[hms] adopted total capped: <name> X -> Y (player P)` with Y about equal to P, for every over-parity
  follower (Auri above all);
* at each level-up, `[hms-parity] player HMS gain G` and each follower's `applied <= credit`;
* `[hms] ... award trimmed to the player's total` once a follower sits at the cap.
Optional capture to settle the "looks higher in the UI" question: add `GetPermanentActorValue` H/M/S for the
follower AND the player to the `[hms] adopted total capped` / `[hms-parity] player HMS gain` lines (tier C,
log only).

---------------------------------------------------------------------------------------------------
## TASK 2 — 86e3d6dp0: is follower magicka use calculated correctly?

### The formula MFO uses
Direct roads: `spend = clamp(sp->CalculateMagickaCost(caster) * a_chargeSec, 0, pool)`, applied as
`RestoreActorValue(kDamage, kMagicka, -spend)`. Sites: `cast/Direct.cpp:626-633` (target), `:327` (settle),
`:436` (self), `cast/Auto.cpp:196`, `cast/CastOn.cpp:1735`, `cast/Summon.cpp:375`, `logistics/Service.cpp:1957/2031`.
`CalculateMagickaCost(Actor*)` is the fork's `MagicItem::CalculateCost` = engine `RELOCATION_ID(11213, 11321)`
(`mit-3.7-fork/src/RE/M/MagicItem.cpp:5-48`, `include/RE/Offsets.h:378`).

### The engine's effective cost (DISASSEMBLED here, AE 1.6.1170, not assumed)
* `CalculateCost` (id 11321 @0x14C1F0): for a spell with SPIT flag 0x1 (manual cost), cost = the record's
  cost, then `AdjustCost(cost, caster)` (vtable slot 0x63). For an autocalc spell, the per-effect costs are
  summed (0x1401434D0). Then, if there is a caster, `HandleEntryPoint(0x26 = kModSpellCost, caster, spell,
  &cost)` runs (the perk entry point: perks and Fortify-School). Result is `max(cost, 0)`.
* `SpellItem::AdjustCost` (vtable 186619 slot 0x63 -> 0x14FF30): reads `skill = caster->GetActorValue(spell's
  school)`, then calls id 26505 @0x41AA40.
* **id 26505 is the skill curve, and it has SEPARATE PLAYER AND NPC BRANCHES** (branch =
  `ActorValueOwner::GetIsPlayerOwner`, vfunc 8):
  * player: `cost * fMagicCasterPCSkillCostMult * (1 - powf(fMagicCasterPCSkillCostBase * skill, fMagicPCSkillCostScale))`
  * NPC:    `cost * fMagicCasterSkillCostMult   * (1 - powf(fMagicCasterSkillCostBase   * skill, fMagicSkillCostScale))`
  (`K = 1.0`, `.rdata`. `powf` confirmed through the IAT.) Exe defaults: Base 0.005, Scale 0.5, Mult 1.0 on both.

| GMST | vanilla | LoreRim (winner) | Tuxborn (winner) |
|---|---|---|---|
| fMagicCasterPCSkillCostBase | 0.0025 | **0.0** (Requiem - Magic Redone) | **0.0034** (Adamant) |
| fMagicPCSkillCostScale | 0.65 | 1.0 (RMR) | 0.65 |
| fMagicCasterPCSkillCostMult | 1.0 (exe) | 1.0 | 1.0 |
| fMagicCasterSkillCostBase (NPC) | 0.005 (exe) | 0.005 | 0.005 |
| fMagicSkillCostScale (NPC) | 0.5 | 0.5 | 0.5 |
| fMagicCasterSkillCostMult (NPC) | **0.5** | **1.0** (Requiem) | **0.5** |

Curve factor (before perks). Vanilla at skill 0/50/100: player 1.0 / 0.741 / 0.594, NPC 0.5 / 0.25 / 0.146.
**NPC/player ratio at skill 0/50/100: LoreRim 1.0 / 0.50 / 0.29, Tuxborn 0.50 / 0.37 / 0.29.**
On LoreRim the player has NO engine skill curve at all (factor 1.0), presumably because Requiem moves skill
reduction into perks. NEEDS a check of which kModSpellCost perks the player carries.

### Q1. Where MFO gets the cost, and is it the effective cost? Field numbers.
It is the engine's EFFECTIVE cost for that caster: skill curve (the NPC branch, because the caster is an NPC),
then the perk entry point. Fast Healing is the specific case:
* **Tuxborn, Jesper `750012C6`, Fast Healing `0002F3B8`** (2026-10-27e Deck, 2 lines: `cost 3 x 1.00s = 3.5`).
  The Tuxborn winner (Adamant over Mysticism) is a **Concentration/Self spell, manual cost 38/s** (vanilla:
  FF, autocalc 73). Restoration 80 (`[prog] ledger 750012C6`): NPC factor `0.5*(1-sqrt(0.4))` = 0.1838, giving
  6.98/s. Field 3.5 means the perk factor is **0.501** (0.494..0.508 from rounding): exactly one x0.5
  kModSpellCost perk. The numbers close, so this is the correct effective cost.
  The player at the same skill with the same perk pays `38 * 0.571 * 0.5` = **10.85/s**. The follower pays
  **32%** of what the player pays.
* **LoreRim, Auri `483139C8`, Healing `00012FCC`** (118 lines, `cost 7`, once 8). The winner (Requiem - Magic
  Redone) is **Concentration/Self, manual 40/s** (vanilla 12). Restoration 60-63: NPC factor 0.45-0.44, giving
  about 17.9/s. Field 7 means the perk factor is about **0.39** (0.36..0.43). That fits a real perk/enchant
  stack, but her perk list is not in the logs: UNDETERMINED for the perk part. The player's curve on LoreRim
  is 1.0, so pre-perk the follower pays **about 45%** of the player.
* The task's 09-23 example (Jesper Healing Hands `0004D3F2` cost 4) is not on disk. On Tuxborn that spell is
  also a 38/s Concentration override, and cost 4 fits Restoration about 70 with one x0.5 perk. Consistent, but
  not provable without that log.
**So the number MFO charges is right for an NPC. It is low because the ENGINE gives NPCs a much cheaper cost
curve than the player** (and on LoreRim the player has no curve at all).

### Q2. Is magicka charged on the AI-fired (animated) road?
Yes, by the ENGINE. MFO adds no deduct there (MAP.md "The AI-fired road is charged by the engine and must
never gain an MFO deduct". The heal channel road has "No MFO magicka deduction on this road").
The direct roads are charged by MFO, because `CastSpellImmediate` is free (ENGINE_NOTES §0.22, measured).
Both roads use the same function: CAST-DELIVERY.md records that `MagicCaster::GetCurrentSpellCost` calls
`CalculateCost` (disassembled earlier, not re-traced by me). The caster is the same NPC, so both take the NPC
branch. **The two roads cost the same and neither is free.**
Open caveat, already flagged in code (`Direct.cpp` "CONCENTRATION DOUBLE-CHARGE: OPEN"): whether a direct
concentration stream ALSO starts an engine channel that drains by itself was never A/B measured. The field
pool deltas say no visible double drain. Example: `866->859` on a 7.0 charge, and the next beat's `before`
reflects regen, not a second drain. Not proven.

### Q3. Charged once per cast, or per tick for concentration?
Per SECOND of channel, since v2.0.12 (the `paidThrough` clock, `BeatChargeSec` `Direct.cpp:280`; settle on
release). FF is charged once per cast (`x 1.00s`). Field stream (11-01 `MFO.log.2`): ATTACHED 12:59:03.088 ->
SETTLE (heal-full) 12:59:07.481 = **4.39 s channelled, billed 1.00+0.07+1.20+1.07+1.07 = 4.41 s x 7.0 = 30.6
spent (100%)**. Expected total per stream = effective cost/s x stream seconds. Correct.

### Q4. Regen or HMS making spending look cheap?
* HMS is not inflating the pool: Auri's held base Magicka is 127 while her live pool is 826-867. The pool is
  about 85% non-HMS modifiers. Also, HMS has given her 0 Magicka in every logged award (`converge award H 7.0
  M 0.0 S 0.0`).
* MFO does not touch magicka regen (no write to `kMagickaRate`/`kMagickaRateMult`; the only references are
  `EquipAuthority.cpp:637-639` gear scoring). Whether the engine's NPC regen differs from the player's (any
  NPC-only regen GMST) is **NEEDS-RE**. In the field the pool recovers about 3/s between beats.

### VERDICT — Task 2
**Working as the ENGINE designed. Not an MFO bug.** MFO charges exactly the engine's effective NPC cost (the
Jesper case closes to 0.5%), on the right cadence (per second, about 100% of channel time), on the same
function the engine uses for the AI road.
The "looks low" is real and has a NAMED cause: the engine's separate NPC skill-cost curve
(`fMagicCasterSkillCost*`). It makes a follower pay about 29-50% of what the player pays at the same skill and
perks, and on LoreRim the player has no engine skill curve at all. HMS inflation is NOT the cause.
**Decision for marth (AskUserQuestion):** should followers pay the PLAYER's cost curve (parity, like HMS)?
If yes, fix shape:
* (a) **Proper, tier A (engine seat, Harbinger per fix-in-Harbinger):** hook the curve (id 26505, or
  `SpellItem::AdjustCost` slot 0x63) so that an MFO-managed follower takes the PLAYER branch. Then both
  roads, the engine's own AI charge, the competence and affordability gates and dual-cast all agree. Needs
  a VerifiedAddresses row and a `SeatVerified()` guard, plus a 1.5.97 path (SE id 11213 / its curve).
* (b) Not recommended: rescale only on MFO's direct deduct by `PCfactor(s)/NPCfactor(s)` from the live
  GMSTs. The AI-fired road would still charge the NPC curve, so the two roads would cost different amounts.
  That is the exact divergence Q2 warns about.
On LoreRim, parity also needs a check of the player's own kModSpellCost perks (Requiem's skill-based
reduction), which `progression-owns-player-perk-effects` may already copy to followers.

**Capture plan (tier C, log-only) to close the remaining unknowns:** on the first FORCE-CAST / SELF-CAST of a
spell per caster per session, log `skill=<GetActorValue(school)> curve=<id26505 factor> perkFactor=<cost /
(record cost x curve)> playerCurve=<PC-branch factor at the same skill>`, plus the six GMSTs once at load.
That proves Auri's 0.39 perk factor and gives marth the follower vs player numbers directly.
