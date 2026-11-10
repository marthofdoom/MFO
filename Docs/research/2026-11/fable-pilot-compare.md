# Fable 5.2 end-to-end pilot vs Opus card-by-card (marth 2026-11-10)
Pilot: H5 ranged miss detection + correction (Harbinger projectile launch seat + MFO budgets: Ranger <=3/10, Jaqspur <=1/10 or never).
Metrics to record: subagent tokens (author + every review round), number of author/review rounds, wall time, CI failures, SEV>3 findings per round, field outcome (first Deck session: works / partial / fails).
Opus tier-A baselines (from this project): one branch 6 author/review rounds ~1.8M subagent tokens (2026-09-08); equip authority ~12 rounds / ~3M tokens across two repos (2026-09-15); cost parity 11-10: author 328k + review 184k, 2 rounds (tier B/A mix, smaller scope).
## Pilot log
- 2026-11-10T04:38:24Z interim: Fable author at 515.7k subagent tokens, 219 tool uses, 58 min; waiting on its own CI (not reported yet)
- AUTHOR DONE (2026-11-10): one author pass, 1 CI failure (compile, fixed in 1 commit), 0 approaches abandoned, ~3 h 40 m wall. Tokens: Fable context ~530k (last interim 522k) + 2 Explore subagents (98k + 92k) = ~720k. Scope: RE on 3 runtimes (1.6/1.5 verified, 1.7 refuses by name), new Harbinger ch.29 seat + ABI v36 (21 files, +1292), MFO adoption (12 files, +395).
- Comparison so far: Opus tier-A baselines 1.8M (6 rounds) / 3M (12 rounds) were for comparable seat+ABI work. Pending: Opus tier-3 review rounds count + first Deck field result.
- OPUS TIER-3 REVIEW (11-10, 200k tokens): 0 findings above SEV-3 (0 SEV-1, 0 SEV-2), 2 SEV-3 + 3 SEV-4 + 4 SEV-5. RE independently confirmed on 1.6.1170 + 1.5.97. Fix round 2 sent to the Fable author (2 SEV-3 + 2 SEV-4 field-relevant).
- ROUND 2 DONE (11-10): Fable fix round 1 pass, 0 CI failures, no subagents; Fable total author tokens ~579k (+190k Explore) = ~770k. Re-check by the same Opus reviewer pending.
- REVIEW ROUND 2 RE-CHECK: 0 >SEV-3 across both rounds (final). 1 SEV-4 (unverified frame-delta address) -> round 3 small fix.
- FINAL (pre-field): 3 author rounds (1 CI failure total), Fable ~611k + Explore ~190k = ~800k author tokens; Opus review ~200k + 2 re-checks (~16k + ~?); 0 findings above SEV-3 in any round. Merged APMF 62dc4e7 / MFO 7e8dc21. Field result pending (first Deck run with bProbe=1/bAimProbe=1).
