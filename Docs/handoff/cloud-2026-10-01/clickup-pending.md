# ClickUp updates still to apply after the daily MCP limit resets (session 2026-10-01)

The ClickUp MCP daily limit (100 calls) was used up at about 06:20 UTC on 2026-10-01. Apply this list once it resets.
The `cloud` tag may not exist yet in the "Modding / Coding Projects" space. Create it first in the ClickUp UI
(clickup_add_tag_to_task only adds tags that already exist).

## 1. Tag `cloud`: tasks a cloud session can do from code alone

No field logs, no game files and no disassembly needed. Tier is per CLAUDE.md's cost table.

| Task | What the cloud can do | Tier |
|---|---|---|
| 86e394119 MFO-B3 (remainder) | Read-only `Followers` accessor for the gambit table, so an Auto (class 0) hybrid with style control off counts as a weapon user | B |
| 86e39411z MFO-B19 (remainder) | The seam the batch-3 review named: a POD {preferKinds, offHand} + non-inline `CombatWeaponRolesFor` in Gear.cpp, reuse `WeaponBuyScore`, move the 4 equip-authority declarations to Logistics.h | B |
| 86e39411t MFO-B15 | Render-side "respec queued" latch so the board footer stops showing "free (one time)" for ~300 ms | C/B |
| 86e3d6dp0 magicka research | Fix M2 (AUTO road bills concentration ~25%) and M3 (concentration wards billed once per 15 s pin). M1 regen fix is tier B (bill lost regen) or A (regen delay, needs disasm) | B |
| 86e3d6dnv HMS research | R2 fix: judge fixed-stat only after a pass that got past the class check, drain MFO-B69. R1 (Auto skips the cap) needs marth's decision first | B |
| 86e3d6ay6 MFO-B63 remainder | Best-per-slot declaration for NON-mage followers (judge output restructure) | B/A |
| 86e3gmxmg translatable ImGui | Move UI strings to a translation table / file loader | B (large) |
| 86e3eewaf remove follower from roster | Board action deleting all MFO data for a follower and undoing MFO's changes (touches co-save records) | A |
| 86e3e89w7 per-gambit summon count | Gambit-row control for the summon count within the list's limit | B |
| 86e3g4vdr dragon flying/landed conditions | New gambit conditions IF an existing engine read gives the flight state (verify in the fork, else STOP) | B |
| 86e39412e MFO-B28 | Bought weapons get gem capture via AcquireEquip. Check the gem-choice F5 decision (pouch overflow) first | B |

Not cloud (keep untagged): play sessions, anything needing field logs (86e3haxr2 Jesper, 86e3haxr4 Fire tiers,
86e3eb078 dead target after the diagnostic ships), Skyrim.esm values (86e3haxqw lockpick SNDRs, Monday checklist on
`feat/mfo-lockpick-sndr`), disassembly or new engine hooks (86e3haxqx pickup sounds, APMF 86e3hbh80 sound facet,
B16 engine equip-best), marth decisions (B25, B33, B34 tier 1, B35, 86e3940yn gem redo, 86e3940zh, 86e3940yv), and
the cosave_roundtrip harness (86e3hbcmn, needs local saves).

## 2. Comments to post

- **86e3d6dp0 (magicka research):** paste the full text of `scratchpad/clickup-86e3d6dp0-comment.md`.
- **86e394100 (review debt):** the reviewer's summary is already posted. Add: "Fixed on main via `fix/mfo-input-review-debt`
  (0d9939f): input trampoline refuses a non-E8 call site (49c9d1b), ToggleControls storeState back to true (6d3e9c5).
  Opus review MERGE. Remaining SEV-4/5 (AE rdx proof [DISASM], held-input loss, blanket re-enable without snapshot,
  thread claim, VR offset placeholder, shout-close grace, AllocTrampoline-before-check, menu names in comment) go to
  REVIEW-BACKLOG." (Recording those backlog entries in Docs/REVIEW-BACKLOG.md is also still to do.)
- **86e39411g (B8), 86e39411p (B12), 86e394126 (B23):** "Fixed on `fix/mfo-backlog-batch3`" plus the merge SHA once
  merged, then set status complete.
- **86e39411z (B19):** "STOPPED in batch 3 (needs moving inline code). Cheapest seam recorded in the backlog: see table above."
- **86e3haxr2 (Jesper):** "Batch 3 adds a read-only `[prog] ledger` dump per enrolled follower after load (progression/Poll.cpp
  DumpLedgerOnce). Grep `[prog] ledger <JesperID>` in the next field log: auto/manual/baseline per skill settles the armor question."
- **86e3eb078 (dead target):** "Diagnostic on `diag/mfo-dead-target` (24269b8): passive `[deadtgt]` worker line. Grep it in the
  next field run." Add the merge SHA if merged.
- **86e3haxqw (lockpick sounds):** "Plumbing on `feat/mfo-lockpick-sndr` (ff15da4), DO NOT MERGE until the Skyrim.esm values are in.
  Monday checklist is in the commit body. UX branch `feat/mfo-ux-0930` held with it for the local release."
- **86e3haxqu / 86e3haxr1 (hotkeys, cell):** "Review fixes da371ab (one-time 0 to -1 migration). Held with the UX branch for the
  local release."

## 3. Added later in the session
- **86e3eewaf (remove follower from roster):** comment "marth 2026-10-01: removing a follower should also be the repair path for HMS state. A full remove (all MFO records incl. the PRGN/HMS block) followed by a fresh add re-captures the HMS block with a new retro flag. That restores the player-rate cap for a follower whose flag the never-processed adopt used up (HMS core caveat: enrolled while the HMS switch was off). Dispatch after feat/mfo-hms-core merges (shared files: Followers.cpp, Board.cpp, progression/)."
- **86e3eewaf (remove from roster), add:** "Built on `feat/mfo-remove-roster` (032a03b0 code, plus a handoff doc at Docs/handoff/remove-roster.md). STOWED for the local agent next week (marth). Unreviewed, tier A. Next step: Opus tier-3 review of the undo list, then a Deck test."
- **APMF design rule (marth 2026-10-01), post on the APMF equip-deny / equip-authority task (86e3940xu):** "If the engine has a weighting system in an area Harbinger touches, Harbinger controls decisions through that weighting. Deny plus substitute is only the fallback where no weighting exists. First application: steer the follower auto-equip scorer so other mods can still equip what they need (MFO-B16 b)."
