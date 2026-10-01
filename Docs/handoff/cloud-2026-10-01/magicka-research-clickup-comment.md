FOLLOW-UP RESEARCH (Opus, 2026-10-01, from the code only, on origin/main b6b7b905). No field logs were available for this round, so anything that needs a log is marked "FIELD".

**Status of the 2026-09-23 diagnosis**
That round found that the direct road charged ONE second of cost per beat, about 0.48 of the time actually channeled. Fixed by "charge for time" (commit 8cad88aa, **first shipped in v2.0.12**). The example line in this task, `magicka 323->319 (cost 4)`, is the OLD pre-v2.0.12 format. On v2.0.12 and later, direct-road lines read `(cost C x S s = X)` and add `SETTLE` lines.

**1. Where the cost comes from**
Every charge site uses `sp->CalculateMagickaCost(caster)`, the engine's own `MagicItem::CalculateCost`. It already includes the skill curve and Mod Spell Cost perks, and the engine's own cast charge uses the same function (see the 09-23 disassembly comment). On a concentration spell that is the cost PER SECOND. Charge sites:
- direct target `cast/Direct.cpp:623`
- direct self `:426`
- settle `:323`
- AUTO `cast/Auto.cpp:147`
- summon `cast/Summon.cpp:293`
- logistics OOC `logistics/Service.cpp:1860,1932`, `cast/CastOn.cpp:1639`
The source of the cost is correct. On Tuxborn, Adamant makes Fast Healing and Healing Hands concentration spells with a manual cost of 38/s, so "cost 4" means 4 per second after Jesper's skill and perks.

**2. The two roads**
- AI-fired, animated (offense via kIntent_Cast, and the opt-in bHealAnimPackage heal claim): the engine charges natively. MFO deducts nothing there. That is correct.
- Direct force (CastSpellImmediate, which charges nothing): MFO deducts.
- Neither road is free. **They still do not cost the same, though, because of regen (below).**

**3. Once per cast or per second**
- Direct target and self concentration streams that are "momentary" (heal or damage): billed for time. `BeatChargeSec` (`Direct.cpp:280`) bills the seconds since `paidThrough`, but only up to the previous beat's sustain window. `SettleSec`/`PostSettle` (`:301`/`:313`) bill the tail at release. The first beat pre-pays 1 s. Expected total per stream = cost x (seconds from the first beat to min(release, last beat + window)), with window = 6 s for a heal.
- **RESIDUAL M2: the AUTO road is still per cast.** `Auto.cpp:147` charges `cost` once per fan. Fans are paced by `max(1, fCastCooldown)`, 4 s by default (`:538`), and the concentration effect is pinned for 6 s (heal) or 4 s (`:119`), with no settle. A concentration heal on AUTO therefore pays about 1 s per 4 s of channel, plus an unpaid tail of up to 6 s. That is about 25%, the exact 4x under-charge the comment at `DirectTarget.cpp:294` warns about. On v2.0.12+, an AUTO line still prints the old `(cost N)` format.
- **RESIDUAL M3: concentration WARDS are billed per application.** A Buff-kind (sticky) concentration stream stays untimed: target `tc.timed` excludes Buff (`DirectTarget.cpp:281`), and self `sc.timed = ConcMomentary` covers value-modifiers only (`DirectSelf.cpp:218`). The already-active guard skips the re-apply while the ward is up, so a self ward pinned for 15 s (`kConcSelfUtilityCap`) pays 1 s for the whole stream (cap 8-15 s), and a target ward pays 1 s per 4 s.

**4. Regen (the most likely remaining cause of "looks low")**
- **M1: regen keeps running during a direct stream.** The engine does not consider the follower to be casting, so the NPC magicka regen that the AI road pauses during a cast keeps going (the 09-23 field read: the "before" value is full at nearly every beat). Rough numbers for Jesper, assuming the vanilla race magicka regen of 3%/s (FIELD/xEdit to confirm): Tuxborn's fCombatMagickaRegenRateMult is 0.5 (BladeAndBlunt.esp). With a 338 max that is about 5.1/s in combat and about 10/s out of combat, **which is more than the 4/s cost**. Even billed exactly per second, his pool cannot drain. The bill is right and the NET cost is about zero.
- MFO changes no regen anywhere (no MagickaRate / regen writes in native/).
- HMS inflation is a minor factor here. Jesper's 338 Magicka was below an all-Magicka player's 360. After the parity retro (v2.0.12) his max is lower, so regen per second is lower too. See [are follower HMS gains matching the PLAYER's rate?](https://app.clickup.com/t/86e3d6dnv).
- M4 (open, would make it HIGHER, not lower): the possible engine channel that drains during a direct concentration stream (comment at `Direct.cpp` ~596) has still never been A/B measured.

**Ranked**
1. M1, regen not paused on the direct road. High confidence that it is the main reason the pool never moves.
2. M2, AUTO concentration billed per fan. Medium: it applies only where an AUTO rule fires a concentration spell.
3. M3, wards billed once. Medium-low: it applies only to ward streams.
4. Cost source: correct (no change).

**Fix shapes**
- M1, two options. (a) Tier B: on timed direct streams bill `sec x (cost + regenPerSec)`, where regenPerSec is the live MagickaRate x MagickaRateMult x max x the combat mult, read from the engine. (b) Tier A, an engine field write: pause regen the way the engine does after a cast. CommonLib declares `HighProcessData::magickaRegenDelay` at +0x24, but per principle 6 it must be verified against the disassembly and the VerifiedAddresses self-check first. FIELD-confirm first that the AI road really pauses NPC regen.
- M2, tier B: give AUTO concentration fans a per-(caster, target, spell) `paidThrough` and reuse `BeatChargeSec`, or bill `min(interval, window)` seconds per fan plus a tail on rule loss.
- M3, tier B: bill a held ward per second while its stream is held, even on beats where the guard skips the re-apply (timed, but without re-applying).

**FIELD lines to read on Monday (DLL >= v2.0.12)**
- `[cast] <id> <name> FORCE-CAST <spell> (<fid>) at <tgt> -- effect applied, magicka A->B (cost C x S s = X)` and `... SELF-CAST ...`: sum S per stream and compare it with the stream's wall time.
- `[cast] <id> <road> SETTLE (<reason>) spell <fid> -- S s x C/s, magicka A->B (spent X)`: the tail at release.
- **Regen:** for consecutive beats, `(A_n - B_{n-1}) / dt` = regen per second. Compare it with C. If regen >= C, that is M1.
- `[cast] <id> <name> AUTO BENEFICIAL <spell> ... magicka A->B (cost C)` on a CONCENTRATION spell: M2. Count the seconds between lines (expect about 4).
- `[cast] <id> conc effect ATTACHED on <tgt> (... window Ns)` and `cast_self skipped -- <ward> already active`: ward cadence (M3).
- For the AI road, take any kIntent_Cast concentration offense cast and read the follower's magicka over the cast (a probe would be needed: no [cast] deduct line exists there by design).
