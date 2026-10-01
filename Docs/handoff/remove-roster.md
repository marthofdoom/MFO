# Remove from roster: handoff for the local agent (stowed 2026-10-01)

marth 2026-10-01: "Stow the roster for the local agent next week."

State when stowed:
- Branch `feat/mfo-remove-roster`, head 032a03b0. It is based on main 4f574cd0.
- CI run 36902674220 on 032a03b0 was still running when stowed. Check it first.
- NOT reviewed. This change is tier A: co-save state plus actor mutation.
- Next step: a full Opus tier-3 review. Check the undo list below against every place MFO writes to an actor.
- The author's MAP entry, CHANGELOG line and docs are in c802ed6e.

Open points for marth / the reviewer:
- Spells taught from a book stay learned after removal. They are not ledgered anywhere.
- About 90 runtime throttle clocks and once-latches are not erased one by one. They hold no MFO data and never reach the co-save. They drop at the next load or revert. Erasing each one would touch about 20 files outside the brief.
- Dev-only probe writes (ProgProbe) are out of scope.

The author's disk log follows verbatim. It holds the undo list, the decisions and the files.

---

# remove-roster (ClickUp 86e3eewaf) agent log
- 2026-10-01 start. worktree $SP/wt-remove, branch feat/mfo-remove-roster off origin/main 4f574cd0. CommonLib fork /tmp/claude-0/cl at fde0f3a (mit-3.7).

## UNDO LIST (built before code; 2026-10-01, origin/main 4f574cd0)
Legend: U = undone by removal, R = released by the existing release road (Followers::ReleaseHeldState, Followers.cpp:346), E = record erased, S = deliberately STAYS.
1. Perks MFO granted (base TESNPC AddPerk + ApplyPerksFromBase): progression/PerkGate.cpp:452 GrantRank, :518 ReapplyFollower (re-add per session). U: for every PerkAlloc remove ranks[rank-1] from base if present (Respec's exact loop, Verbs.cpp:297-309), ApplyPerksFromBase once. (Base perk edits do not survive a load anyway, P3.)
2. Native perks progression STRIPPED (base RemovePerk): PerkGate.cpp:292 StripNativePerks, record ProgState::strippedPerks. U: public ProgAllocator::RestoreNativePerks (Verbs.cpp:345 -> RestoreNativePerksImpl PerkGate.cpp:312), AFTER step 1 (a stripped native rank1 + MFO rank2 on one node ends with the native back).
3. Skill base AVs (SetBaseActorValue): SkillScale.cpp:98 ReconcileSkill (the single skill write site; RecomputeSkills/manual points/Respec all funnel here). U: ReconcileSkill with newPoints 0 per SkillAlloc = writes MFO's own 'natural' (REVERT: enrollment baseline; ADOPT: cur-points) - the exact inverse of the write model.
4. HMS base H/M/S (SetBaseActorValue via Followers::SetFollowerHMS): progression/Hms.cpp:490 RecomputeHMS hold (incl. fixed-stat grant, parity withhold, retro). U if hmsCaptured: write hmsBaseline[p] (the captured pre-MFO base) per pool; health guard like Hms.cpp:500-512 (heal min(drop, damage) so current health does not fall). Leveling NPC: engine re-slams absolute autocalc next level; fixed-stat NPC: baseline is exact.
5. Health/Magicka damage restores (RestoreActorValue kDamage): Hms.cpp:508 guard, cast/Auto.cpp:155, CastOn.cpp:1646, Direct.cpp:326/435/632, Summon.cpp:306, Service.cpp:1865/1939. S: transient vitals (heals/costs), nothing to give back.
6. Spells: Board.cpp:1382 TeachSpell (AddSpell, player's spellbook consumed); logistics/Cast.cpp:147 learn-from-book (follower's own book consumed). NOT ledgered anywhere. S: player/follower paid a book; the spell is learned, like an item owned. Flag for reviewer.
   Loadout spell-in-hand (EquipSpell Loadout.cpp:357/443/485): R (Loadout::Restore deselects g_mfoSpell).
7. Equips/unequips: Loadout.cpp:109 debt swaps -> R (Loadout::Restore). cast/Equip.cpp force-hold (FWPN) -> R+E (Actuation::ReleaseForcedWeapon erases g_forcedWeapon). Upkeep/Gear/Service/LootEquipment/Lotd/SwapUp equips of gear he owns: S (his items; equip authority released so engine owns his worn set).
8. Items moved: LootTake (loot into follower), SwapUp, Lotd relic deposit to museum, Economy sales/gold, Lockpick consumption, Gear.cpp:341-344 + Upkeep.cpp:270-285 evictions (non-playable deleted / foreign handed to player), MEO gem socket/unsocket in his own gear. S: items/gold now owned by someone; deletions unrecorded+irreversible; not MFO data.
9. Combat style on CombatController (CombatStyle.cpp:171/186/215, CasterConsent.cpp:473/489/516 style swap): R (CombatStyle::Clear, CasterConsent::Clear). Runtime only, not saved.
10. Alias fills (persist in .ess): Packages cast alias, loot-travel alias, retreat alias -> R (Packages::Release, Logistics::OnFollowerRemoved->LootTravelEvictIf, Packages::RetreatEvictIf). EvaluatePackage: not persistent.
11. APMF claims/declarations: equip authority + declaration (R: ReleaseEquipAuthority, ForgetEquipDeclaration), target pin (Targeting::Clear), spell allow-list (CasterConsent::Clear), combat entry (EngageOnSight::Forget), retreat re-entry deny, pursuit leash, heal cast, offense cast, lockpick hold (Lockpick::Abort), deposit (Lotd::EndDeposit), loot travel (LootTravelEvictIf). All R.
12. Factions / outfit / teammate flag / essential / AI flags / AV modifiers: MFO writes NONE (grep: no SetOutfit/AddToFaction/SetPlayerTeammate/SetRelationshipRank/ModActorValue). Nothing to undo.
13. Papyrus DoCombatSpellApply / casts: transient magic effects. S.
14. Gait: writes MFO's own package forms, not the actor. N/A.
15. Dev-only probes: ProgProbe.cpp:145/168 (SetBaseActorValue kProbeAV), :408/:470 (AddPerk) + harness hotkey: unledgered dev harness, not shipped behaviour. S (named, out of scope).
16. Field Orders power (Forms.cpp:142): on the PLAYER. N/A.
RECORDS ERASED (E): g_followers (FLWR: gambits, rank, rapport, class override, mfoEnabled), g_prog (PRGN: alloc, skills, baseline, HMS, strippedPerks after restore), g_hmsFiredMask, g_forcedWeapon (FWPN, via release), g_stockGear (MSTK, new EraseStockGear), PlayerGiven g_rec (PGIV, new Forget), Followers file-locals (g_lastCombat, g_missStreak, g_stateSample) + mirrors republished (g_tracked, g_mfoOff, g_resolvedClass).
RUNTIME CACHES NOT erased individually: ~90 log-throttle clocks / once-latches across Scheduler, cast/, logistics/, apmf/, MEOBridge (inventory grepped). Claims/latches among them are released by the release road; the rest hold no MFO data, never reach the co-save, and are dropped at the next load/revert. Erasing each would touch ~20 files outside the brief boundary -> reported, not done.
- WIP commit pushed (code, before docs). Files: roster/Roster.h, roster/Remove.cpp, progression/Remove.cpp, SkillScale UnwindSkills, Followers::EraseRecord, Logistics::EraseStockGear, PlayerGiven::ForgetFollower, Board EditKind+ApplyEdits+FieldKit UI, i18n keys + template.
- docs commit pushed (MAP roster entry, CHANGELOG, Remove.cpp ordering comment). Waiting CI.
- commit 3: FollowerRow::partyMember greys button for out-of-area teammates (IsEligibleFollower). pushed.
