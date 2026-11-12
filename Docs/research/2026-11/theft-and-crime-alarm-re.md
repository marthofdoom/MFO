# Engine theft and crime-alarm road (RE, 2026-11-11)

Source: Opus read-only diagnosis of ClickUp 1dfcuabczbg ("followers' looting in towns gives the player theft bounties",
user report on v2.8.1, 5 g bounties in Whiterun, guards attack, stops with looting off). Disassembly of the unpacked
1.6.1170 binary, spot checks on 1.5.97 and 1.7.104. Verify any id below against `VerifiedAddresses` before binding it.

## 1. Verdict

- MFO's take roads cannot raise a steal alarm with the v2.8.1 / main gates:
  - **Loose ref (Road L)**: `LooseRefBarred` (LootScan.cpp:438, re-checked at Service.cpp:913) bars ANY owner, which is
    strictly broader than the engine's pickup crime test. Both use the same owner function. Taken with
    `ActivateRef(follower)` (Service.cpp:956).
  - **Container / corpse (Roads C, K)**: `RemoveItem` (LootTake.cpp, LootEquipment, SwapUp.cpp:598, Gear.cpp:854,
    StripCorpse LootScan.cpp:992 / Service.cpp:1030). The engine never alarms on this road (traced 7 calls deep).
    No per-item `ExtraOwnership` check exists on it, so already-stolen copies can enter his pack (no crime, but it breaks
    the reporter's "never marked stolen" ask).
- 5 g = vanilla `iCrimeGoldTrespass` (Tuxborn vanilla 5, LoreRim 500). Ranking: (1) teammate trespass charged to the
  player through a door / container / furniture road, about 45%; (2) a road outside the loot code that runs only while
  looting is on (errand legs, an item OnActivate script), about 25%; (3) an item that gains an owner mid-leg, about 10%.
  marth 11-11: "It may be a mod that alters the crime system then."
- Vendor trade itself cannot be theft: `MFO_Trade.psc` uses Papyrus RemoveItem/AddItem only (l.69-70, l.107-109). The
  walk to the shop (a locked owned door) is the trade-adjacent risk.
- marth's hypotheses: (1) actor mismatch is real (pickup judged for the follower, flora and IsOffLimits for the player,
  the crime always charged to the player) but not the cause, since MFO bars any owner. (2) "the check misses a case" is
  refuted for ref-level checks; the one gap is per-item ownership inside containers and corpses.

## 2. Engine rules

- `IsCrimeToActivate` (what `IsOffLimits()` calls) is hardcoded to the PlayerCharacter (403521). Default branch:
  crime = `!IsAnOwner(ref, player, useFaction=1, requiresOwner=0)`. Locks count only on the DOOR branch; FURN and NPC_
  have their own branches. A locked unowned container is not a crime.
- `GetOwner` and `IsAnOwner` share one owner chain: ref `ExtraOwnership`, then the original-ref handle (11688) and its
  ownership, then 20202 (skipped for FURN/DOOR/ACTI), then the parent cell owner (18982). No owner gives
  `!requiresOwner` (no crime). With an owner: true when the owner is the actor's base or a faction he is in at the
  required rank (11695 vs 37676); the player gets an extra test (40670). CommonLib `TESObjectREFR::GetOwner` is this
  engine function.
- Pickup: `ActivateRef` (19796) calls `base->Activate` (vtable slot 0x37). ALCH, MISC, AMMO, ARMO, SLGM, KEY, INGR go to
  17670 (WEAP via 18101 to 17670), which calls `activator->vfunc 0xCC` = `Character::PickUpObject` (37521). At 0x678809:
  `!IsAnOwner(itemRef, PICKER, 1, 0)` calls `StealAlarm(...)` and stamps ownership (11609).
- Coin purse / flora: `TESFlora::Activate` 17369 to 14864, loop at 0x1E916D tests `IsAnOwner(flora, PLAYER)` and calls
  `StealAlarm(PLAYER, ...)` per produce item.
- `StealAlarm` (37422): if the thief has `boolBits & kPlayerTeammate` (bit 26, +0xE8 AE / +0xE0 SE), the thief is
  replaced by the player and allowWarning forced to 1. Non-player non-teammate thieves return at once. Gold = item value
  (14978) x count, min 1. Witness detection after the swap runs FOR THE PLAYER.
- `RemoveItem` 19689, `Character::RemoveItem` 37797, `Character::AddObjectToContainer` 37525 reach neither StealAlarm nor
  PickUpObject. Items keep their per-item ExtraOwnership.

## 3. Ids and RVAs

| Function | 1.6.1170 id / RVA | 1.5.97 id / RVA | 1.7.104 RVA |
|---|---|---|---|
| IsCrimeToActivate | 19827 / 0x2EE480 | 19400 / 0x29A330 | 0x2F4A50 |
| IsAnOwner(ref, actor, useFaction, requiresOwner) | 20210 / 0x2FA6F0 | 19805 / 0x2A6EE0 | 0x300CF0 |
| GetOwner | 20194 / 0x2F9E40 | 19789 / 0x2A6670 | 0x300440 |
| GetLock | 20223 / 0x2FAD60 | 19818 / 0x2A74C0 | - |
| ActivateRef | 19796 / 0x2EAC20 | 19369 / 0x296C00 | 0x2F11B0 |
| Character::PickUpObject (vtable 207886 slot 0xCC) | 37521 / 0x678600 | - | 0x68B0E0 |
| StealAlarm | 37422 / 0x66F320 | 36427 / 0x5DD770 | 0x681DE0 |
| TrespassAlarm(this, ref, owner, crime=-1) | 37427 / 0x6711B0 | 36432 / 0x5DF630 | 0x683C70 |

- StealAlarm signature (disassembled): rcx thief, rdx ref, r8 object, r9d count, stack +0x28 total, +0x30 owner,
  +0x38 allowWarning.
- PickUpObject: CommonLib says AE slot 0xCE, but Character vtable slot 0xCC holds 37521 (the pickup body); 0xCE holds
  38541. Verify before binding.
- Crime constructor 42502 / 0x7A2350 (type in edx, `PackageNS::CRIME_TYPE`). Alarm family, all call 42502 then the
  registration: 37422 Steal (0), 37423 (type passed in, probably Pickpocket), 37425 Attack (3), 37426 Murder (4),
  37427 Trespass (2), 37428 (type r15+6, unmapped). Crime registration **41349** (also called by 37681 and 22206).

## 4. Producers

- StealAlarm: Character::PickUpObject 37521 (actor-evaluated); PlayerCharacter::PickUpObject 40533 (vtable 208040 slot
  0xCC); flora harvest 14864 (player-evaluated); TESObjectCONT::Activate 17887 (vtable 189633 slot 0x37); script command
  entry 22681 (probably SendStealAlarm); player-only 40359 (via 40351) and 51125 (via 51172, menu); 56221 (no direct
  xref, probably Papyrus).
- TrespassAlarm: TESObjectACTI::Activate 17702; TESObjectDOOR::Activate 17922 / 0x27DA50 (a non-player without the key
  runs an NPC lock attempt 37528; on success with an owned door it calls TrespassAlarm and unlocks); teammate
  activate-target handler 37496 / 0x6754A0 (from high-process update 39436: on a locked CONT/DOOR it unlocks then calls
  TrespassAlarm(PLAYER) unless follower or player is an owner; an owned NPC target also raises trespass and can raise
  Attack 37425); 38626 (from Actor update 36991, also 38586, 34335; furniture/bed-shaped, ends in 44673, not fully
  traced); player lockpick menu 51981; 40076, 40814, 22434, 54740 unmapped.
- Teammate to player charge: StealAlarm (0x66F39B) and TrespassAlarm (0x6711EB) test bit 26 kPlayerTeammate at
  +0xE8 AE / +0xE0 SE and swap in PlayerCharacter (403521) INSIDE the alarm.
- Witness / report stage (next RE target): shared callees after registration 37679, 37680, 37682, 38094, 38145, 38305,
  39361, 39557, 42506. Trespass gold comes from the r9d=-1 path. Guard response trigger not traced.

## 5. Crime alarm road, for a future Harbinger facet (marth 11-11: "We'll need harbinger to block crime alarms later")

- Cleanest seat: the ENTRY of each `Actor::*Alarm` (six functions, ids known on AE and SE, the main two mapped on
  1.7.104). It is the only point where the original thief (the follower) is still visible; after the redirect
  everything says "player", and 41349 has lost who did it.
- A deny there (thief is a managed teammate and the facet is claimed) covers theft, trespass, assault, murder and
  pickpocket from any road, including engine AI and other mods' scripts acting on a teammate. It does NOT cover crimes
  the player commits, script `ModCrimeGold`, or non-crime witness reactions (aggro, dialogue).
- 41349 alone would be one seat for everything, but it would deny player crimes too and the follower identity is gone.
- G13 steal no-witness pre-check: reuse the engine's decision. `IsAnOwner(item, follower)` decides whether a crime
  exists; the witness loop inside StealAlarm (the shared callees above) decides whether it is seen. Witnesses are judged
  FOR THE PLAYER after the redirect, so the engine's question is "does anybody see the player", not the follower.

## 6. Fix shape (marth 11-11: bar all stolen; both halves in one round; no looting in front of guards for now)

- One gate `Logistics::TakeBarred(follower, src, obj, xList, road)` at every take road: Road L at scan and arrival; each
  LootX before RemoveItem, LootEquipment, SwapUp:598, Gear:854; the LOTD crate activation.
  - Loose ref: keep "any owner barred", add `!IsAnOwner(ref, follower,1,0)` and `!IsAnOwner(ref, player,1,0)`.
  - Inventory copy: owner = the copy's xList ExtraOwnership, else the container's GetOwner(). Take only copies owned by
    null / player / PlayerFaction / the follower; pass the specific xList so a stolen copy never moves with a clean one.
  - Already-stolen copies are never taken, including what the player dropped (marth 11-11, overrides the 09-27
    "what you drop is fair game" ruling for stolen items).
- Door / furniture: refuse any loot, errand or deposit leg whose route needs a locked door he has no key for, or an
  owned door / furniture (extends `TownWalkBarredByLock` from "all doors locked" to "this door").
- Guards (marth 11-11, interim until the culprit is found): no loot take of any kind while a guard can see the follower
  or the item, for every follower.
- Proof log line, rate-limited per (follower, src, verdict) every 30 s:
  `[theft-gate] F=<fol> road=<...> src=<ref>(<base>) item=<base>x<n> owner=<id|0> via=<...> isOwner(fol)=<0|1>
  isOwner(pc)=<0|1> offLimits(pc)=<0|1> locked=<0|1> verdict=<take|BARRED:<reason>>`, plus a per-follower ring of the
  last 3 MFO world actions printed on every `[crime-probe] BOUNTY UP` (bCrimeGoldProbe ON in the next field run).
