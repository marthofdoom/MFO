# DIAG 2026-09-06 -- "denials are failing and heals are failing"

Session: deck, 19:51-19:59, Jesper the Guard 0x750012C6 (mage) + Cicero 0x0009BCB0.
DLLs: APMF 84872e0d (main 69c57bde), MFO 843490ac (main aae6df64). Logs: scratchpad `log0906b/`.
Method: every claim below was traced in source by file:line on those two commits. Log
counts are from the pre-gathered EVIDENCE.txt plus a handful of targeted greps named inline.
No source was edited, nothing was built.

Confidence markers: CONFIRMED = code path read end to end and matched to a log line.
PLAUSIBLE = inference from code + log, one link unobserved. UNKNOWN = stated as such.

Install order that matters for reading APMF.log (plugin.cpp:78 `aicastseats::Install`
runs BEFORE plugin.cpp:85 `castseats::Install`): the Group B probe is the INNER hook and
the ch.8b seat is the OUTER one. So every `[aicastseats] CheckStartCast[...] -> YES/NO`
line is the ENGINE's own verdict, which the ch.8b thunk then overrides to YES for the
claimed form. This is why Group B prints NO for Healing Hands while seat 0x06 prints YES.

---

## 1. Ranked root causes

### RC1 -- CONFIRMED -- Two heal rules thrash MFO's single heal-claim slot; each swap releases the live claim before the engine can equip + charge. THIS is "heals are failing".

- MFO `native/APMFBridge.cpp:703-725` `ClaimHealCast` -> `EnsureCastClaimLocked(v5, follower, o.heal, ...)`.
  `o.heal` is ONE `CastClaim` per follower (APMFBridge.cpp:80-81 "heal stays a single instance (always LEFT)").
- MFO `native/APMFBridge.cpp:296-368` `EnsureCastClaimLocked`: if `c.spell != wantSpell` it falls through to
  `:318 api->Release(c.handle)` then `:335 RequestCast(...)`. A different heal spell = release + fresh claim.
- Two heal rules alternate every 1-3 s. APMF.log 19:58:42-19:58:56 (grep `cast-execution CLAIMED|facet released|minted`):
  42.788 CLAIMED 0004D3F2 (Healing Hands, target player) -> 44.397 released + CLAIMED 0002F3B8 (kSelf heal,
  proxy 0xFF001F38 minted) -> 45.317 released + CLAIMED 0004D3F2 -> 47.987 released + 0002F3B8 -> 52.914 0002F3B8
  again -> 55.438 0004D3F2. The engine's own equip-anim + charge for a claimed cast takes ~2.3-2.5 s in this log
  (offense: claim 19:56:56.873 -> first Firebolt fire 19:56:59.155). A swap cadence shorter than that means
  neither heal completes. The only heal that fired all session (proxy 0xFF001F38, castobs Unk1->Casting->
  Concluding 19:59:13.643-14.193, MFO `[cast]` 19:59:14.110) came from the one 0002F3B8 claim that was left alone
  for 2.5 s (minted 19:59:11.158).
- Healing Hands reached `CASTER[L] state=4(Casting)` exactly once (19:58:47.802) and was gone by 48.779; the
  claim at that moment (h=30, CLAIMED 45.317) was still live (expiry 51.317) and seat 0x07 logged zero STOPs all
  session (`grep -c "seat 0x07]"` = 0), so APMF did not stop it. Why that one concentration start ended within a
  second is UNKNOWN from these logs (candidates: engine range/LOS tolerance on a player target, magicka drain,
  the AI's next rescore). It does not change RC1: the heal never got a stable window.
- The MFO-side entry point is NOT ComposedCast::Try called from combat -- it is `Logistics.cpp:1330-1336`
  ("OOC concentration ... (direct force, bounded)") -> `Actuation_Direct.cpp:1020-1046 CastTargetDirect` ->
  `ComposedCast::Try` (:1045) which claims via APMF and returns Applied. The log label "(direct force, bounded)"
  is therefore WRONG when APMF is present -- APMF delivered, nothing was forced. Misleading for diagnosis.
- This is the "spell gambit locks until complete" rule (memory) never having been applied to HEALS: the
  offense path has a per-hand cast lock (`Actuation.cpp:309-340 ResolveCastHand` / `HandFree`), the heal slot has none.

### RC2 -- CONFIRMED -- Every kIntent_Cast claim hard-expires at 6 s and MFO never renews it; a 0.3-1.2 s unclaimed gap recurs every 6 s and foreign spells equip + charge inside it.

- APMF `native/APMF_API.h:303-304` `kCastDefaultTtlMs = 4000`, `kCastMaxTtlMs = 15000`. MFO passes
  `req.ttlMs = kHealCastTtlMs` for EVERY cast claim (APMFBridge.cpp:334; APMFBridge.h:522 = 6000).
- APMF `native/core/ControlMap.cpp:315-318` sets `expiresMs = now + ttl` ONCE at ApplyRequest.
  `ApplyRepoint` (ControlMap.cpp:622-660) updates `self->param` only -- it does NOT touch `expiresMs`.
  There is no Renew in the ABI (grep APMF_API.h). So a claim's life is exactly its ttl, period.
- MFO `APMFBridge.cpp:304-309`: when the claim is unchanged AND `IsClaimLive` -> `return` (no Repoint, no
  refresh). When it has expired -> the `[apmf] ... already auto-expired ... re-requesting` line (:314) and a
  fresh `RequestCast`. APMF.log confirms 6.0 s lives: 19:56:56.873 -> 19:57:02.872, 19:57:24.703 -> 30.716,
  19:57:31.766 -> 37.765, 19:59:17.697 -> 23.740.
- The gap between `expired (h=N)` and the next `CLAIMED` is MFO's tick cadence (~1.1-1.3 s between successive
  "OOC concentration" lines) plus APMF's next `Drain` (Arbiter.cpp:32-33, once per frame): observed 0.26 s,
  0.39 s, 0.61 s. Inside the gap `CheckCast`, `CheckShouldEquip` and all four seats chain to the engine, the AI
  rescored and armed its own pick. Observed: `CASTER[L] Stone Rune` at 19:59:23.850, 110 ms after the 0002F3B8
  claim expired at 23.740; MFO re-requested at 24.341. A spell already charging is never stopped by the
  re-arrived claim (CastGate is pre-charge; seat 0x07 only governs the CLAIMED caster instance).
- Principle 9 verdict: a fixed 6 s expiry against a "hold it as long as the gambit wants it" client is the
  stale-cadence bug class exactly. A bigger TTL (max 15 s) would only stretch the period; the fix is renewal.

### RC3 -- CONFIRMED -- A one-hand claim leaves the OTHER hand completely open, by design, and MFO never claims it. This is what the user sees as "denials are failing".

- APMF `native/core/CastGate.cpp:53-84` `CheckCastThunk`: `AllowedCastForHand(fid, subjectForm, callerHand)`.
  `native/core/Allowance.cpp:103-110`: `if (!TryGetCastClaimForHand(actor, hand, ...)) return true; // no cast
  claim on THIS hand -> ch.8b imposes nothing`. Same scoping in EquipGate.cpp:518-531 (`hasHandSeat`, per-hand).
- MFO picks ONE hand: `Actuation.cpp:309-340 ResolveCastHand`, `HandPick::EitherFree` -> LEFT if free; the heal
  is hard-wired LEFT (ComposedCast.cpp:160, APMFBridge.cpp:80-81). MFO makes no claim of any kind about the
  right hand's MAGIC (its ch.15 kIntent_Equipment claim names the Elven Dagger, which steers the WEAPON score
  only -- and weapon leaves are not hookable at 0x0F, Docs/DENY-COMPLETENESS-AUDIT.md row 15).
- Every "foreign" cast in the log is on the unclaimed hand: Stone Rune `CASTER[R]` x25 while the claim was
  L (all 25 are R; the 3 `CASTER[L]` at 19:59:23.850-25.249 are RC2's gap); Poison Spray `CASTER[R]` x6;
  Raise Zombie `CASTER[R]` x8, and its two `CASTER[L]` (19:57:13.664/14.632) happened while Lightning Bolt was
  on R (`castobs 0002DD29 CASTER[R]` 19:57:13.664-15.776). I found NO case of a foreign spell charging on the
  CLAIMED hand while the claim was live. The per-hand deny holds; the actor is simply half-claimed.
- Note the actor-wide `Allowance::AllowedCast` (Allowance.cpp:81-88) still exists and has ZERO callers -- the
  "deny the whole actor" primitive was written and then superseded by the per-hand pass without an opt-in.

### RC4 -- CONFIRMED -- MFO reads GetCastProxy synchronously after RequestCast, which returns 0 by construction; both MFO layers then cache that 0 forever. Hence "(FF001F38) ... their own spell, not ours".

- APMF `ControlMap.cpp:140-167` `EnqueueCast` only pushes a `PendingOp`; the proxy is minted in
  `ApplyRequest` (`:390-430`, `castproxy::Acquire` at :424) during `Drain` (Arbiter.cpp:33, once per frame).
  `GetCastProxy` (`:1047-1063`) walks the PUBLISHED snapshot (`m_published`), so a handle that has not been
  drained yet returns 0 ("unknown/stale"). `IsClaimLive` (`:1072-1088`) has the same shape.
- MFO `APMFBridge.cpp:361-365` fetches the proxy ONCE, immediately after `RequestCast` (:335), into `c.proxy`.
  `GetHealCastProxy` (:749-755) returns that cached field and never re-queries.
- MFO `ComposedCast.cpp:112-114` `WatchArmed`: `w.proxy` is set only when the watch is CREATED
  (`if (w.spell != a_spell) {...}`); a later call for the same spell with a now-valid proxy is dropped.
- So `ExpectingCast(fid, 0xFF001F38)` (ComposedCast.cpp:216-222) is false, `[cast]` files the proxy cast as
  "not ours" (Diagnostics.cpp:220-226), `NoteObservedCast` never runs, and the `[cfc]` watch for 0002F3B8
  keeps warning after a cast that actually landed (19:59:14.483 "3328 ms with NO observed cast", 370 ms after
  the proxy fired). Also `CastBounds::Arm(fid, spellID, 0, ...)` at ComposedCast.cpp:171 passes proxy 0 literally.
- Correction to the brief: the proxy is NOT stale-by-re-mint. `castproxy::Acquire` reuses a per-owner slot,
  so all six mints are the same 0xFF001F38. The staleness is the opposite: it was read too EARLY, not too late.
- RC4 did not by itself stop a heal (the proxy cast that fired landed on the ally). It blinds MFO to its one
  success and produces false `[cfc]` alarms. Not the heal root; RC1 is.

### RC5 -- CONFIRMED -- Score steer cannot bias a spell under ANY INI setting. It is a code gap, not a config gap.

- APMF `native/core/AiCastSeats.cpp:196-231` `CalculateScoreThunk` (the thunk on the 30 spell/staff
  `CombatInventoryItemMagicT_*` vtables, installed at :1069-1119 under `if (groupA)` = `EnableItemScoreProbe`)
  calls `orig`, LOGS, and returns `score` unchanged. There is no bias in it at all.
- The only `kScoreSteerBias` (:446) applications are `:604` and `:615`, inside `WeaponScoreThunk` (:483),
  installed only on the four raw-RVA weapon leaves (:1161-1224, Melee/Ranged/Shield/Torch). Its "ch.8b cast
  claim" branch (:610-618) tests `driven == itemForm` where `itemForm` is a WEAPON's FormID -- it can never equal
  a spell/proxy FormID. Dead code.
- Net: `EnableScoreSteer=1` steers weapons under a ch.15 equipment claim and nothing else. Log: exactly two
  STEERED lines, both Elven Dagger / "ch.15 equipment claim". Setting `EnableItemScoreProbe=1` would add
  logging only. Fix = code (section 2).
- Data point that matters for the design: the 0x0F YES line prints `category=0` for the Healing Hands
  Restore item (EquipGate.cpp:379-386 reads it through a real vfunc). That contradicts AiCastSeats.cpp's
  `:583-593` inference "heal = category 1, walked before 0, bias a no-op". Heals share category 0 with offense
  spells AND weapons, so the claimed spell IS in a score compare against the dagger on that hand (see P5 below).

### RC6 -- CONFIRMED (as a finding about finding 1) -- Seat 0x06 chains for non-claim spells BY DESIGN; that is not the hole.

- `native/core/CastSeats.cpp:206-244` `CheckStartCastThunk`: `if (!ClaimNamesThisCast(a_this, a_cc, m))
  return orig(a_this, a_cc);`. `ClaimNamesThisCast` (:148-199) matches `this->magicItem` against the claim's
  DRIVEN form via `TryGetCastSeatClaimForForm`; a caster instance holding a different spell chains to the
  engine, it is never answered NO. The seat is a COMPOSE seat (force the claimed cast), not the DENY seat. The
  deny for the cast facet is the pair CastGate 0x0A + EquipGate 0x0F, both per-hand (RC3).
- Seats are installed on Restore + Offensive only (CastSeats.cpp:499-500); Reanimate/Summon/etc. have no
  seat, but CastGate + EquipGate still cover them on the claimed hand (they hook `ActorMagicCaster` and the
  item leaves, not the caster categories). The Group B probe (14 caster vtables, :1126-1156) is observe-only.

### RC7 -- CONFIRMED -- The `[cfc]` watchdog is mis-sized and reports watch age, not claim age; most of its offense warnings are false alarms.

- `ComposedCast.cpp:95` `kSilentWarnAfter = 2000 ms`. Measured claim -> first fire latency here is 2.3-2.5 s
  (equip animation + charge). Every offense `[cfc]` warning in EVIDENCE A is at 2009-2398 ms, i.e. inside the
  engine's normal latency; the `CFC-fired` line follows 0.2-1.5 s later (e.g. warn 19:56:58.995, fire
  19:56:59.155; warn 19:59:02.106, fire 19:59:04.752).
- `WatchArmed` never resets `w.since` for the same spell (:114 only on spell change), so "live 67046 ms" for
  Healing Hands is the age of a watch that spanned ~12 separate 6 s claims and a 35 s no-claim lull
  (19:57:48-19:58:23, "NPCs controlled: 1"). The real signal underneath (Healing Hands never fired) is RC1.

### Latent (not fired this session) -- MFO's own CheckCast exclusivity deny is not stood down by a heal-only claim.

- `CasterConsent.cpp:999` `if (exclusivityDeny && !APMFBridge::IsOwnedCastActive(fid))`; `IsOwnedCastActive`
  (APMFBridge.cpp:461-472) checks the two OFFENSE slots only. `ShouldDeny` (:813-830) does not consult
  `ClientCastClaimed` (:225-229, which does include `IsHealCastActive`; that helper is used by the OTHER deny
  at :270, not this one). With `g_want` latched to an offense spell and cast-control level 4, the heal proxy
  (0xFF001F38 != want) would be HARD-ABORTED by MFO itself. `grep -c "HARD-ABORTED" MFO.log` = 0, so it did
  not happen here, but nothing in code prevents it.

---

## 2. Minimal fixes (described, not written)

F1 (RC1, MFO) -- Lock the heal slot until the claimed heal is observed or its own TTL lapses.
  `native/ComposedCast.cpp` `Try()` (:160-176): before `ClaimHealCast`, if `g_watch[fid].hand[0]` holds a
  DIFFERENT spell, `!observed`, and `APMFBridge::IsHealCastActive(fid)` with the claim younger than
  `kHealCastTtlMs`, return `true` (hold the incumbent; caller treats it as Applied and skips its kInstant
  apply). That is the offense per-hand cast lock (Actuation.cpp:309-340) applied to the heal slot. Optionally
  the same guard at the rule-arbitration level in `Logistics.cpp:1330`. Also fix the `:1362` label so it
  says APMF delivered when `ComposedCast::Try` returned Applied.

F2 (RC2, APMF + MFO) -- Turn the expiry into a renewable floor.
  APMF `ControlMap.cpp:622-660 ApplyRepoint`: when `self` is a kCast claim (`self->expiresMs != 0`), set
  `self->expiresMs = now + <the claim's own ttl>` (store `ttlMs` on `Claim` next to `expiresMs`, :284/:508).
  Document in APMF_API.h next to `kCastDefaultTtlMs` (:299-304) that Repoint renews the window. No ABI bump.
  MFO `APMFBridge.cpp:304-309`: on the "unchanged AND still live" path, if `now - c.refreshed >= ttl/2`
  (3 s), call `Repoint(c.handle, &p)` with the same param as a heartbeat and update `c.refreshed`. The
  6000 ms value then becomes a crash-safety floor (design.md §5a intent) instead of a live-state killer.

F3 (RC3, APMF + MFO) -- Let the client DECLARE actor-wide cast exclusivity; APMF enforces it.
  APMF `APMF_API.h:237-262`: add `kCastFlag_ExclusiveActor = 1u << 4` (bits 4-7 are free; StopPct owns 8-15;
  append-only, no renumbering). `Allowance.cpp:103-110 AllowedCastForHand` and `:121 CastClaimNamesForHand`:
  when the actor's live cast claim (either hand) carries the flag, fall back to the actor-wide test
  (`AllowedCast`, :81-88, already written and currently unused). `EquipGate.cpp:518-531`: same flag makes the
  deny-complete branch use `hasLiveSeat`/`seat` (any hand, :347) instead of `hasHandSeat`. Docs row 34 update.
  MFO `APMFBridge.cpp:330-333`: set the flag when the follower's hand plan has no second spell wanted
  (single-hand `EitherFree`/heal) -- i.e. always, until MFO grows real two-hand plans. Alternative without an
  ABI change: MFO requests a second kIntent_Cast for the same spell on the other hand -- rejected, that
  fabricates intent (principle 4) and makes the engine try to dual-arm.

F4 (RC4, MFO only) -- Read the proxy lazily, and let the watch learn it.
  `APMFBridge.cpp:304-309`: on the unchanged-and-live path, if `c.proxy == 0 && abiVersion >= 6`, re-read
  `GetCastProxy(c.handle)`. (Keep the :361-365 read; it is harmless.)
  `ComposedCast.cpp:114`: on the same-spell path, `if (w.proxy == 0 && a_proxy != 0) w.proxy = a_proxy;`
  before the `observed` early-return. `:171` `CastBounds::Arm` should receive the proxy once known (pass
  `APMFBridge::GetHealCastProxy(fid)`; Arm already handles a 0). Diagnostics.cpp:210-238 needs no change.
  (APMF alternative: make `GetCastProxy` also scan `m_queue` -- rejected; a pending op has no proxy yet.)

F5 (RC5, APMF only, code) -- Put the spell bias where the spell items are scored.
  `AiCastSeats.cpp:196-231 CalculateScoreThunk`: after `score = orig(...)`, add the mirror of `:605-617`
  (`TryGetCastSeatClaimForHand` on the item's hand; `driven == itemForm` -> `score + kScoreSteerBias`),
  gated on `g_scoreSteerEnabled`. The item's hand comes from `itemSlot.equipSlot` the way EquipGate.cpp:198-234
  resolves it (same static_assert'd member; do not invent a second read).
  `:1069` install the item thunk when `groupA || scoreSteer` (keep the SCORE log line gated on `groupA` so
  steer-on does not turn the probe log on). Delete the dead branch `:610-618` in `WeaponScoreThunk`.
  Open question for marth, do not decide inside the fix: "score steer for spells only" read literally means
  the ch.15 weapon branch (`:600-606`) should sit behind its own flag or go; today it is the ONLY thing the
  flag does. Config change alone fixes nothing.

F6 (RC7, MFO + APMF observability) -- `ComposedCast.cpp:95` `kSilentWarnAfter` >= 4000 ms (measured
  latency 2.5 s, with headroom), reset `w.since` when a fresh claim is issued for the same spell (pass a
  "fresh" bool from the RequestCast site, or store APMFBridge's `c.refreshed`), and print claim age not watch
  age. APMF `CastGate.cpp:53-84` has NO per-decision log at all (only the install line :151); add a throttled
  `[t2c] DENY actor/spell/hand` line so the deny can be observed executing (principle 5) -- today the
  completeness of the cast deny is unobservable from APMF.log.

F7 (latent, MFO) -- `CasterConsent.cpp:999`: stand down on `ClientCastClaimed(fid, a_spell)` (or
  `IsHealCastActive || IsOwnedCastActive`), not `IsOwnedCastActive` alone.

Suggested order: F1 + F4 (MFO only, small, fixes the headline heal failure and the false alarms) ->
F2 (APMF 5 lines + MFO heartbeat) -> F3 (design decision, ABI flag) -> F5 -> F6/F7.

---

## 3. Deny-completeness audit -- the cast facet under a standing kIntent_Cast claim

"Standing" = published, unexpired, on hand H of follower A. Each row: how a spell S != claim can reach a
live cast on A, and whether anything stops it.

| # | Path | Verdict | Where / evidence |
|---|------|---------|------------------|
| P1 | AI charges S on the CLAIMED hand H | DENIED (code) / UNOBSERVED (log) | CastGate.cpp:75-84 returns false with kMultipleCast; Allowance.cpp:103-110. No per-decision log exists, so the log cannot show it firing. No counter-example found in this session. |
| P2 | AI re-arms S on the CLAIMED hand H (spell/staff leaves) | DENIED | EquipGate.cpp:518-531 deny-complete; log shows 34-35 `-> NO (deny-complete...)` per foreign spell. |
| P3 | AI charges S on the OTHER hand | NOT DENIED (by design) | Allowance.cpp:105 "no cast claim on THIS hand -> imposes nothing". Observed: Stone Rune R x25, Poison Spray R x6, Raise Zombie R x8 + L x2 (LB was on R). RC3. |
| P4 | AI re-arms S on the OTHER hand | NOT DENIED (by design) | EquipGate.cpp:518-520 `hasHandSeat` is per-hand. RC3. |
| P5 | A WEAPON out-scores the claimed spell on hand H (both category 0) | NOT DENIED | 0x0F is not hookable on weapon leaves (audit row 15); the only weapon lever is the ch.15 steer in WeaponScoreThunk (+1000 for the DAGGER, which pushes the dagger INTO the hand, the opposite direction). Spell steer is dead (RC5). PLAUSIBLE contributor to why Healing Hands got a hand only briefly; not proven from this log. |
| P6 | The 6 s expiry gap (claim dead, MFO not yet re-requested) | NOT DENIED | RC2. Observed Stone Rune L at 19:59:23.850, 110 ms after expiry. Every seat, gate and steer chains during the gap. |
| P7 | RequestCast -> next Drain latency (claim requested, not yet published) | NOT DENIED, <= 1 frame | ControlMap.cpp:140-167 queue, Arbiter.cpp:33 per-frame Drain. Negligible in play; UNKNOWN whether Drain still runs every frame during menus/loads. |
| P8 | S already CHARGING when the claim arrives (or re-arrives after P6) | NOT DENIED | CastGate is pre-charge only; seat 0x07 CheckStopCast governs the CLAIMED caster instance (CastSeats.cpp:381); no interrupt of a foreign charge anywhere. Design gap, becomes rare once P6 is closed. |
| P9 | Caster categories without a ch.8b seat (Reanimate, Summon, Ward, ...) | DENIED on H via P1/P2, NOT DENIED on the other hand (P3) | CastSeats.cpp:499-500 seats only Restore+Offensive; CastGate/EquipGate hook ActorMagicCaster + item leaves, independent of category. Raise Zombie behaved exactly as P3. |
| P10 | Non-hand casting sources on A (kInstant / kOther: abilities, scripts, creature casts) | DENIED (code comment) / UNVERIFIED | CastGate.cpp:69-73 maps other sources to `Hand::kUnknown`; Allowance.h:133 says kUnknown forwards to the actor-wide floor. Not exercised by this session (no such casts on Jesper). |
| P11 | MFO's own direct force (CastSpellImmediate) on A | N/A this session | Heal: ComposedCast::Try returned Applied -> no force (Actuation_Direct.cpp:1045). Offense concentration self/target: claim first, force only on refusal (:790-806, :1060-1072). No "direct-force stream runs instead" lines in MFO.log. |
| P12 | MFO's own CheckCast hook aborting the CLAIMED cast (proxy/heal) under a heal-only claim | LATENT, 0 hits | CasterConsent.cpp:999; 0 HARD-ABORTED lines. Would be a self-inflicted deny of the claim's own cast, the inverse hole. F7. |
| P13 | Another actor's spell cast AT A (e.g. Chaurus Poison 0x001093B2) | OUT OF SCOPE | Not a cast BY the follower; see section 4, finding 2. |
| P14 | The engine arms the claimed spell in the hand MFO did NOT name | DRIVEN ANYWAY (benign) | ClaimNamesThisCast matches by form (CastSeats.cpp:175-186), not by hand; 0x0F forces YES for the claimed form on H only (:366-388), the other hand's instance chains to the vanilla gate. Not a leak. |

Summary: on the claimed hand the deny is complete in code (P1, P2) and unobservable in the log (F6). The
actor is only half-claimed (P3, P4, P5) and unclaimed 5-20% of the time (P6, P8). Those four rows are the
"denials are failing" report; none of them is a bug in a gate, all are scope.

---

## 4. Where the six findings are wrong

Finding 1 -- HALF WRONG. Correct that seat 0x06 returns the engine's own answer (`return orig(...)`,
CastSeats.cpp:214) for a non-claim spell; wrong that this is the deny hole. 0x06 is a compose seat and was
never the deny; CastGate 0x0A + EquipGate 0x0F are, per hand. The Poison Spray / Stone Rune YES lines in
EVIDENCE B are the INNER Group B probe reporting the engine's verdict for the OTHER hand's caster instance
(plugin.cpp:78 before :85). Every such cast is P3 or P6, none is a claimed-hand leak.

Finding 2 -- WRONG. 0x001093B2 "Chaurus Poison" is cast by four Chaurus Reapers (0x00098BF4, 0xFF000F1D,
0x0009917A, 0x0009917B; `castobs` actor column), never by Jesper. It has zero equip-gate reach because those
actors hold no claim and the `[t2a]` engineSays line is `g_logEnabled`-gated (EquipGate.cpp:537). The
"denied at 0x0F yet still cast" spells were denied on hand L and cast on hand R (P2 vs P3). 0x0F is
sufficient for what it covers; the chokepoint that WOULD be complete is CastGate with actor-wide scoping (F3)
plus a renewed claim (F2).

Finding 3 -- RIGHT in effect, wrong in mechanism. It is not that the steer thunk is gated behind
`EnableItemScoreProbe`; the item thunk that IS gated there has no steer in it (AiCastSeats.cpp:196-231). The
only steer lives in the weapon thunk and its cast-claim branch is structurally dead. Config cannot fix it.

Finding 4 -- RIGHT, wrong cause. Not a stale re-minted proxy (same 0xFF001F38 all six times, per-owner slot
reuse); the proxy was read BEFORE APMF's per-frame Drain published the claim, then cached at two layers.

Finding 5 -- RIGHT. TTL = 6000 ms (MFO-chosen, default would be 4000), re-request cadence ~1.2 s, Repoint
does not extend, MFO never repoints an unchanged claim. Mis-sized, and the fix is renewal, not a longer TTL.

Finding 6 -- RESOLVED, both recognizers are right about different things. The 17 `*** THE ANIMATED PATH ***`
lines are real casts and each is paired with a `CFC-fired` line, which proves `ExpectingCast` matched and
`NoteObservedCast` ran for offense claims (Diagnostics.cpp:217-238). The offense `[cfc]` warnings are all at
2.0-2.4 s = false alarms from a 2 s grace window against a 2.5 s engine latency (RC7). The Healing Hands
warnings (7.7 s .. 67 s) are REAL and are RC1. The 0002F3B8 warnings are a mix: four of five claims were
thrashed away (RC1, real), the fifth fired and was unrecognised (RC4, false). How bad it actually is:
offense casting through APMF WORKS (17 animated fires, roughly one per 1.5 s while claimed); heals landed
once in ~15 attempts; the other hand is the AI's; the actor is unclaimed ~5-20% of the time.

---

## 5. Positive, not chased

`[ch.9-redirect] ... won=true` x5 (Cicero 0x0009BCB0 twice, 0x00015D09 twice, Jesper once; H counters
`claimedHits=16 winHits=16`). The 0x49 `CheckForCurrentAliasPackage` redirect fired and won every time it was
claimed. That closes the multi-day "unproven" question in memory `apmf-package-deny-via-vfunc-0x49`.

**QUALIFIER (added 2026-09-07, from the sibling DIAG's accounting).** "Won every time it was claimed" means
*won every time it was NUDGED with a PUBLISHED claim*. It does NOT mean the engine re-asked on its own.
`Docs/DIAG-2026-09-06-loot-travel.md` §2 reconciles all 16 hits, one by one, to explicit MFO/APMF nudges
across ~75 s of cumulative claim time on three followers, and finds no room for a single hit from the
engine's own package-evaluation cadence. The mechanism is proven; the *cadence* is not, and any design that
assumes a published claim holds itself without re-nudging is unsupported by this session.
