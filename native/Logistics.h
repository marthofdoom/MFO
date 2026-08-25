#pragma once
#include "PCH.h"
#include "State.h"
#include <unordered_set>   // #69: stock-gear sets (CopyStockGear/LoadStockRecord)

// The logistics table's actuation. DESIGN.md §4.8.
//
// A follower's SECOND gambit table (State::logistics() == tables[1]) governs
// upkeep -- drinking potions, looting arrows/potions/gear -- and runs on the
// OUT-OF-COMBAT idle tick. §4.8: the combat and logistics tables NEVER
// interleave, so the Scheduler runs this exactly when the follower is not in
// combat, one action per tick.
//
// TWO KINDS of thing live here:
//   * PURE READS used by the evaluator to answer the supply conditions
//     (CountPotions, ArrowCount, PotionRestores). No mutation.
//   * ACTUATION (ServiceFollower and the sink) that mutates the world, main
//     thread only, behind bLogistics.
//
// TRANSIENT-STATE DISCIPLINE (like Loadout): every map here describes live
// engine or session state, is main-thread-only, takes no lock, and is cleared
// on revert (ClearTransientState). None of it is serialized -- the loot LRU is
// deliberately not persisted (#22h: worst case after a load is one more delay
// on an already-picked corpse, not worth a growing save record).

namespace MFO::Logistics {

    // ── pure reads (the evaluator's supply conditions) ──────────────────────

    // Which resource a potion RESTORES, or kNone if it is not a plain H/S/M
    // restorative. Classified by MGEF ARCHETYPE (kValueModifier /
    // kDualValueModifier) on the costliest effect, never by name or by the
    // effect's target actor-value alone -- the portable classifier from MRO's
    // Requiem note (DESIGN §3.3/§4.8.2). Poisons and fortify/cure potions
    // return kNone by construction. Reads only the POTION, no actor.
    RE::ActorValue PotionRestores(RE::AlchemyItem* a_potion);

    // The dominant restore MAGNITUDE of a_potion, in the same resource
    // PotionRestores classifies it under (the largest-magnitude value-modifier
    // effect on that AV). 0 for non-restore potions. Used to rank health potions
    // strongest-first for the loot stock cap -- a relative measure, so a list
    // that restores 50 flat and one that restores 5/sec both order correctly.
    float PotionMagnitude(RE::AlchemyItem* a_potion);

    // Derive the "low power" potion cutoff from the load order (the weakest restore
    // tier). Call once at kDataLoaded, after the data handler is ready. Feeds the
    // auto floor LootPotions uses when iMinPotionMag is 0. Logs the tier ladder.
    void ComputeWeakPotionFloor();

    // The effective low-power potion floor a restore potion must meet to be looted
    // OR bought (iMinPotionMag if set, else the auto floor). Shared so the economy
    // buy side applies the same "ignore low power" rule as looting.
    float PotionLootFloor();

    // Arrow vs bolt, catalog-first with an IsBolt() fallback (runtime IsBolt() alone
    // is unreliable). Shared so the buy side classifies uncatalogued ammo too.
    bool AmmoIsBolt(RE::TESAmmo* a_ammo);

    // ── #21 economy GEAR/TOME buy helpers (reuse the loot judge on the VM side) ──
    // The economy buy plan (TradeBridge::PlanBuy) runs on the Papyrus/VM thread
    // and must classify the vendor's ACTUAL stock. These wrap the loot judge's
    // weapon-class / mage-apparel / spell-school logic so PlanBuy reuses it rather
    // than re-deriving it (and so the buy and loot sides can never disagree). All
    // are pure form-DATA reads -- no actor, no 3D -- safe on any thread.

    // WepClass of a weapon type as an int, matching the buy thresholds passed in
    // TradeBridge::BuyThresholds::meleeClass: 0=OneHand 1=TwoHand 2=Ranged 3=Other.
    int WeaponBuyClass(RE::WEAPON_TYPE a_type);

    // The magic-school BIT INDEX of a spell (its costliest effect's Magic Skill),
    // 0..4 in the fixed order Alteration/Conjuration/Destruction/Illusion/
    // Restoration, or -1 for a non-school (or unreadable) spell. The economy tome
    // buy tests this bit against BuyThresholds::eligibleSchools.
    int SpellSchoolBit(RE::SpellItem* a_spell);

    // The LOGICAL mage dress-up slot a candidate occupies, or -1 if it is not a
    // dress-up slot: 0 head (hat/hood/circlet), 1 body (robe), 2 hands (gloves),
    // 3 feet (shoes/boots), 4 ring, 5 amulet. For mages, jewelry counts as apparel
    // (marth). Shared by the buy planner and the worker-side owned baseline.
    int MageClothingSlot(RE::TESObjectARMO* a_armo);

    // The LOGICAL rated-armor slot for the per-slot armor buy baseline (and the
    // sell keepArmor set): 0 head, 1 body, 2 hands, 3 feet, 4 shield, or -1 if it
    // covers none of those. Shared by BuildBuyThresholds + PlanBuy so a warrior can
    // buy a helmet/boots for bare slots even with a good chestpiece.
    int ArmorBuySlot(RE::TESObjectARMO* a_armo);

    // Rank a candidate vendor ARMO as mage apparel/jewelry. Returns false unless it
    // is buyable (rating-0 clothing/jewelry, not villain-coded unless a_allowVillain,
    // i.e. a necromancer follower). On true, fills a comparable (out_tier, out_metric)
    // the caller sorts per slot (higher tier first, then higher metric):
    //   a_schoolPrimary == false (MEO present + not strict): VALUE-driven -- tier is
    //     always 0, metric = gold value. Gems transfer on swap, so the most expensive
    //     piece is safe.
    //   a_schoolPrimary == true  (MEO absent OR bMageApparelStrictSchool): SCHOOL-
    //     enchant primary -- tier 2 fortifies one of the follower's top-2 schools
    //     (a_top2Mask), tier 1 is plain (no school fortify -- still fills a slot),
    //     tier 0 fortifies an off-school (ranked lowest). metric = value (+ matching
    //     fortify magnitude for tier 2), so a cheap school robe beats a pricey
    //     wrong-school one.
    bool MageApparelBuyKey(RE::TESObjectARMO* a_armo, std::uint8_t a_top2Mask,
                           bool a_schoolPrimary, bool a_allowVillain,
                           int& out_tier, std::int32_t& out_metric);

    // Count of H/S/M restore potions of a_which resource in a_follower's OWN
    // inventory (#14 -- the named follower, never the player). Walks the
    // inventory, so it is a ~1 s logistics-tick read, not a combat one.
    int CountPotions(RE::Actor* a_follower, RE::ActorValue a_which);

    // How many arrows/bolts a_follower carries matching their EQUIPPED bow or
    // crossbow. Returns -1 when no bow/crossbow is equipped, so the "out of
    // arrows" rule reads as N/A rather than true (a melee follower is never
    // "out of arrows"). Reads the named follower only.
    int ArrowCount(RE::Actor* a_follower);
    int BoltCount(RE::Actor* a_follower);

    // ── actuation ───────────────────────────────────────────────────────────

    // Run one out-of-combat logistics tick for a_follower: cadence-gated
    // internally (~1 s, §4.8's idle rate -- NOT the 133 ms combat deadline),
    // then evaluate the logistics table and fire at most one action. No-op and
    // cheap when bLogistics is off or the follower is not yet due. Main thread
    // only; a_state is the follower's live record.
    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state);

    // Drink the best restore potion of a_which the follower carries, if any and
    // not on cooldown (~the potion's own duration, per resource). Returns true
    // if a potion was consumed. Exposed so the COMBAT dispatcher (Actuation) can
    // run drink gambits in combat, through the exact same cooldown-gated path as
    // the out-of-combat logistics table. Main thread.
    bool DrinkPotion(RE::Actor* a_follower, RE::ActorValue a_which);

    // The player-looted WAIVER sink (#22h). A TESContainerChangedEvent filtered
    // to items entering the player collapses the first-dibs delay on the ref
    // they came from. Registered once at kDataLoaded, after form resolution.
    void RegisterSinks();

    // #62 ON-LOAD beast-head sweep: rebuild each beast-race teammate's 3D once on
    // load so a save comes up with heads already reattached (no trade/loot needed).
    // Main-thread, self-retrying, gated on bBeastHeadFix. Call at kPostLoadGame.
    void SweepBeastHeadsOnLoad();

    // Save-scoped: the cadence clocks and the two loot LRUs. RevertCallback
    // clears them -- they describe a live session, and a stale entry keyed by
    // FormID would apply the previous save's timers to this one.
    void ClearTransientState();

    // Called from the Scheduler's COMBAT branch, per in-combat follower. If this
    // follower is the active loot traveller, END the excursion immediately so he
    // fights instead of looting -- ServiceFollower (which holds the other combat
    // check) is skipped for in-combat followers, so a long batch would otherwise
    // keep him claimed at priority 60 straight through the fight. No-op otherwise.
    void ReleaseTravelOnCombat(RE::Actor* a_follower);

    // Called from the Scheduler's COMBAT branch, once per in-combat follower per
    // tick. Stamps the follower's "last seen in combat" time -- the post-battle
    // gate ShedOffRoleWeapon reads so it drops off-role weapons only AFTER a fight
    // ends, never during a mid-fight IsInCombat() lull (the field mace-hand-off).
    // Worker-only, no lock: same BSJobs tick as the shed that reads it (#4).
    void NoteInCombat(RE::FormID a_id);

    // Called from the roster-removal (dismissal) path. If a_id was mid travel-
    // to-loot, EVICT them from the loot alias: a dismissed follower can't be
    // freed by dropping quest priority (nothing reclaims him) and the alias fill
    // is save-serialized, so it would re-latch on every future load. Safe to
    // call for any id -- a no-op unless a_id is the active traveller.
    void OnFollowerRemoved(RE::FormID a_id);

    // ── #69: co-save companions for g_stockGear ──────────────────────────────
    // A follower's OWN weapon/armor gear, snapshotted the first time MFO
    // manages them (ServiceFollower's EnsureStockSnapshot) and never touched
    // by ShedOffRoleWeapon again (the Gauldurbow fix). This map is a REAL
    // cross-thread structure -- written by the logistics worker, read/written
    // by the co-save callbacks on the main thread -- so every accessor locks
    // internally; none of these may be called while already holding a lock
    // this module owns.

    // A lock-taken COPY of the whole map, for SaveCallback to write the
    // kRecStock record from without holding the lock across file/engine calls.
    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> CopyStockGear();

    // Repopulate one follower's stock set on LoadCallback (mirrors how the
    // FLWR loader hands resolved data to other modules). Overwrites any
    // existing entry for a_followerID.
    void LoadStockRecord(RE::FormID a_followerID, std::unordered_set<RE::FormID> a_set);

    // RevertCallback: drop every snapshot. The next LoadCallback (if any)
    // repopulates from that save's kRecStock record; a main-menu revert with
    // no load leaves the map empty, same as every other save-scoped map.
    void ClearStockGear();

}
