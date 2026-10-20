// logistics/SwapUp.cpp -- THE SWAP-UP RULE (ClickUp 86e3ebfu3, batch L, 2026-09-25).
//
// marth 2026-09-24: "swap up needs to work in looting and shopping. An archer
// should be willing to sell iron for better arrows, and even be willing to drop
// lower arrows for better ones." And: "lower arrows are worthless when better ones
// are available, lowest removed first, or slated for sale when obsolete."
//
// ONE rule, shared by the loot side and the economy (shop) side. When a follower
// can get a better item of a kind it keeps (weapon, armor, AMMO) it takes the
// better one and gets rid of the worse: SOLD at a vendor (EconomyProbe's sell
// list), never dropped (marth 2026-10-18). Two halves:
//
//   * THE KEEP SET (weapons + armor) -- ComputeKeepSet. The best weapon of each
//     class bucket (1H [top-2 for a dual wielder], 2H, bow, crossbow, staff) and
//     the worn + best-scored piece per logical armor slot (ArmorScore, the
//     skill-led judge). Moved here VERBATIM out of EconomyProbe so the sell side
//     and the loot side read the SAME set: everything outside it is superseded.
//     The economy sells it. The loot side never drops it to make room: an upgrade
//     that does not fit the carry weight is left in the body and the superseded
//     piece waits for the vendor (the old PlanRoomForSwapUp / CommitSwapUpDrops
//     were removed 2026-10-18).
//
//   * AMMO -- ranked by DAMAGE, VALUE breaking a tie, within one kind (arrows vs
//     bolts), and judged ONLY for the kind the follower actually uses
//     (UsesAmmoKind: his ranged role; a gambit for the kind is not use -- the
//     seeded "arrows below 10" rule sits on every follower). The follower keeps a
//     TARGET count (AmmoKeepTarget: his "arrows/bolts below N" gambit's N, never
//     under kAmmoKeepFloor). SPECIAL rounds (an explosion on the projectile, or an
//     enchanted instance) are outside the ladder: never counted, never shed or
//     sold. Walking
//     his stacks best-first, the stack where the running count reaches the target
//     is the CUTOFF tier; every stack STRICTLY below the cutoff tier is OBSOLETE.
//     Obsolete ammo leaves lowest-damage first: slated for sale at a vendor
//     (EconomyProbe), dropped back into the body when looting better
//     (SwapUpAmmoFrom), and the shop buys the upgrade that makes it obsolete
//     (AmmoUpgradeBar -> TradeBridge::PlanBuy's AMMO SWAP-UP pass).
//
// NEVER SUPERSEDED, whatever the ranking says: worn items, the follower's own
// signature gear (IsStockGear, T#69), quest instances, catalog-excluded items
// (artifacts / uniques), player-put-on pieces and ammo (IsPlayerPick), special
// ammo, non-playable ammo (bound arrows), and -- on the loot drop only --
// enchanted gear and any instance carrying an MEO
// unique id (it may hold socketed gems; the economy un-gems before it sells, a
// drop cannot).
//
// MFO-B36 SHAPE (review backlog): one candidate must never evict for more than
// itself. Every consumer here plans ONCE over one pool and acts on that one plan:
// the loot shed recomputes from the post-take inventory in one pass, the weight
// relief frees exactly the weight one candidate needs (all or nothing), and
// PlanBuy's ammo pass never plans more of a stock line than the line still has.
//
// THREADING: everything here runs on the logistics worker (ServiceFollower /
// EconomyProbe), like every caller. Inventory moves are container -> container
// RemoveItem into the SOURCE container (the LootAmmo trade that has shipped for
// months) -- no world drop, no 3D, so no MainThread::Post hop is needed.
#include "Logistics_internal.h"
#include "GemRules.h"
#include "Lotd.h"          // LOTD: HoldFromSale -- a needed relic is never sold or dropped
#include <cmath>          // std::ceil: the gambit's N is a float param
#include <limits>         // std::numeric_limits: the "short of the target" cutoff

namespace MFO::Logistics {

        // ── THE KEEP SET (weapons + armor) ──────────────────────────────────
        // Moved verbatim from EconomyProbe (logistics/Economy.cpp) so the loot
        // side reads the same set. The only edit: the armor half's gate is read
        // ONCE into out.armorJudged (the loot drop must know whether armor was
        // judged at all -- with the gate off keepArmor is empty and nothing may be
        // read as superseded from it).
        KeepSet ComputeKeepSet(RE::Actor* a_follower, const FollowerState& a_state) {
            KeepSet out;
            out.armorJudged = Config::g_economyBuyGear.load();

            // KEEP THE LOADOUT: a follower who fights with a bow AND a melee weapon
            // only ever has ONE worn at a time, so the sheathed other reads unworn
            // and was SOLD (marth). Protect the best weapon of EACH weapon CLASS --
            // 1H, 2H, bow, crossbow, staff kept SEPARATELY (Fable: merging 1H+2H or
            // bow+crossbow by raw damage let a junk greatsword/crossbow win the keep
            // and the real weapon get sold). Only worse in-class duplicates are junk.
            // STYLE BY PERKS (marth 2026-09-13): the melee buckets (1H, 2H) rank
            // by the loot judge's WeaponScore -- damage x the perk-style bias --
            // so the greatsword the loot side just preferred over a stronger
            // warhammer is the one KEPT, not the one sold. Ranged/staff buckets
            // are unchanged; with no preferred kind the score IS the damage.
            // SECOND ONE-HANDER (dual wield by perks, 2026-09-14): a dual wielder
            // fights with the TOP-2 owned one-handers (Actuation's PickOffHandWeapon
            // pairs from the pack, and ReleaseForcedWeapon un-wears the left at
            // combat end, so the off-hand weapon reads UNWORN here). Bucket 1
            // therefore keeps its runner-up too -- unless the best form is a
            // stack of >= 2, which already covers both hands (PickOffHandWeapon
            // takes the second copy), so the runner-up is junk and sells.
            const WeaponRoles keepRoles = ComputeWeaponRoles(a_follower, a_state);
            // MFO-B24: gated on the loot judge's meleeTargetClass, with its exact mageMode
            // (LootEquipment.cpp BuildEquipmentContext): cast gambit AND (class Mage, or
            // class Auto with NO melee/ranged gambit). Keep is "what we hold", so it must
            // never sell what loot fetches. castGambits > 0 there is HasCastGambit and
            // g_magicLoadout is IsCasterFollower. Buy (Economy.cpp) uses the broader
            // IsCasterFollower test, which is safe: whenever buy's class is OneHand, loot's
            // and this one are too, so buy never purchases what keep sells. No shared
            // helper exists, so the expression is repeated.
            const bool keepWantsMelee   = TableHasAction(a_state.combat(), Vocab::kActEquipMelee);
            const bool keepWantsRanged  = TableHasAction(a_state.combat(), Vocab::kActEquipRanged);
            const std::uint8_t keepBaseClass = a_state.combatClassOverride;
            const bool keepBaseCaster     = keepBaseClass == 3;
            const bool keepBaseWeaponUser = keepBaseClass == 1 || keepBaseClass == 2;
            const bool keepMageMode = IsCasterFollower(a_state) &&
                (keepBaseCaster || (keepBaseClass == 0 && !keepWantsMelee && !keepWantsRanged));
            // An equip-melee gambit is an OVERRIDE for any class EXCEPT that a base MAGE (class 3) keeps the
            // sidearm contract (marth 2026-10-18 "Daggers only still applies"): no melee TARGET class, a
            // one-hander BACKUP -- a dagger under bMageDaggersOnly (the default), any one-hander without it --
            // exactly cast/Equip.cpp's daggerMelee (class 3 && bMageDaggersOnly). Loot (LootEquipment.cpp
            // meleeTargetClass/wantBackup) and buy (Economy.cpp) use this same expression.
            const WepClass keepMeleeTargetClass =
                (keepMageMode && !keepBaseWeaponUser && (keepBaseCaster || !keepWantsMelee))
                    ? WepClass::Other : keepRoles.melee;
            const bool rolesFinal = StyleVotesReady(a_follower) || !MainThread::IsInstalled();   // not the carried fallback
            const bool keepSecond1H = keepRoles.offHand == 2 && keepMeleeTargetClass == WepClass::OneHand;
            std::unordered_set<RE::TESBoundObject*> keepWeapons;
            {
                // bucket: 1=1H 2=2H 3=bow 4=crossbow 5=staff; -1 = don't protect.
                auto bucketOf = [](RE::WEAPON_TYPE wt) -> int {
                    using WT = RE::WEAPON_TYPE;
                    switch (wt) {
                        case WT::kBow:          return 3;
                        case WT::kCrossbow:     return 4;
                        case WT::kStaff:        return 5;
                        case WT::kTwoHandSword:
                        case WT::kTwoHandAxe:   return 2;
                        case WT::kHandToHandMelee: return -1;   // never protect fists
                        default:                return 1;      // 1h sword/dagger/axe/mace
                    }
                };
                std::unordered_map<int, std::pair<RE::TESBoundObject*, float>> best;
                std::unordered_map<int, std::pair<RE::TESBoundObject*, float>> gatedBest;   // off-role melee, floor only
                bool anyInRole = false;
                // doRanged exactly as Economy's offRoleLauncher and the loot judge take it.
                const bool keepDoRanged = (keepMageMode && !keepWantsRanged) ? false : keepRoles.doRanged;
                struct OneHander { RE::TESBoundObject* obj; float rank; std::int32_t count; };
                std::vector<OneHander> oneHanders;   // bucket 1, for the dual-wield runner-up
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* w = obj->As<RE::TESObjectWEAP>();
                    if (!w || IsCreatureWeapon(w)) continue;
                    const int b = bucketOf(w->GetWeaponType());
                    if (b < 0) continue;
                    // OFF-ROLE MELEE IS NOT KEPT (marth 2026-10-18: "off-role = sold"): roles come from the
                    // class and the equip-melee/ranged gambit override (ComputeWeaponRoles), never from what
                    // is carried, and ShedOffRoleWeapon no longer moves anything -- so a melee bucket outside
                    // the melee role (the SAME keepMeleeTargetClass the loot judge fetches by) falls to the
                    // sale path. Only once the roles are final: until the perk mirror is stamped they are the
                    // carried fallback, which must never sell. The mage backup, the silver slot and the
                    // dual-wield runner-up are inserted separately below/above and are unaffected; stock
                    // gear, relics, quest/excluded items and gemmed weapons are protected in the sell loop.
                    const bool offRoleMelee = rolesFinal && ((b == 1 && keepMeleeTargetClass != WepClass::OneHand) ||
                                                             (b == 2 && keepMeleeTargetClass != WepClass::TwoHand));
                    if (!offRoleMelee && (b == 1 || b == 2)) anyInRole = true;
                    // a launcher counts only if Economy's offRoleLauncher would NOT sell it (role + ammo family)
                    if ((b == 3 || b == 4) && keepDoRanged && RangedInFamily(w, keepRoles.wantCrossbow)) anyInRole = true;
                    // Staves are ranked by their ENCHANTED WORTH (GetGoldValue), not
                    // GetAttackDamage() -- a staff's melee swing stat is irrelevant to
                    // its value, so ranking that bucket by damage could keep the worse
                    // (cheaper) of 2+ unworn staves and sell the better one (SEV2 fix).
                    // Melee buckets rank by WeaponScore (perk-style biased damage);
                    // bow/crossbow by attack damage.
                    const float rank = (b == 5)
                        ? static_cast<float>(std::clamp<std::int32_t>(w->GetGoldValue(), 0, 0xFFFF))
                        : (b == 1 || b == 2) ? WeaponScore(keepRoles, w)
                                             : static_cast<float>(w->GetAttackDamage());
                    // An off-role melee bucket is ranked on the side (gatedBest): it joins the keep set only
                    // under the NEVER-DISARM FLOOR below (no in-role weapon held at all).
                    auto& slot = offRoleMelee ? gatedBest[b] : best[b];
                    if (!slot.first || rank >= slot.second)
                        slot = { obj, rank };
                    if (!offRoleMelee && keepSecond1H && b == 1) oneHanders.push_back({ obj, rank, data.first });
                }
                for (auto& [b, s] : best) if (s.first) keepWeapons.insert(s.first);
                // NEVER-DISARM FLOOR: with no in-role weapon carried at all, the best carried melee weapon
                // of each class is kept as before (the follower is never left with nothing to wield).
                // Only when he HAS a weapon role (a follower with none has nothing to be disarmed of), and
                // then ONE weapon: the best carried melee weapon across the off-role buckets.
                if (!anyInRole && (keepMeleeTargetClass != WepClass::Other || keepDoRanged)) {
                    RE::TESBoundObject* floorW = nullptr; float floorRank = -1e9f;
                    for (auto& [b, s] : gatedBest)
                        if (s.first && s.second > floorRank) { floorW = s.first; floorRank = s.second; }
                    if (floorW) keepWeapons.insert(floorW);
                }
                // Dual wield: keep the runner-up one-hander as well (top-2 by
                // WeaponScore), unless the best is a stack of >= 2 of one form.
                if (keepSecond1H && oneHanders.size() >= 2) {
                    auto bi = best.find(1);
                    RE::TESBoundObject* top = (bi != best.end()) ? bi->second.first : nullptr;
                    bool topStacked = false;
                    const OneHander* runnerUp = nullptr;
                    for (const auto& oh : oneHanders) {
                        if (oh.obj == top) { topStacked = oh.count >= 2; continue; }
                        if (!runnerUp || oh.rank > runnerUp->rank) runnerUp = &oh;
                    }
                    if (top && !topStacked && runnerUp) keepWeapons.insert(runnerUp->obj);
                }
            }

            // #21 KEEP-ARMOR: protect what the follower WEARS + its single best
            // next-upgrade PER LOGICAL SLOT from being sold, so a just-BOUGHT
            // upgrade is not re-sold as junk (keepWeapons does this for weapons) --
            // but EVERYTHING ELSE (extra/old clothing, spare armor) stays sellable.
            // BUG FIXED: the old version bucketed by the RAW GetSlotMask() bitmask,
            // so two same-logical-slot robes with different modded slot-bit combos
            // (plain robe = body; "Blue Mage Robes" = head+body) each survived as
            // "best in its own bucket" -> a mage kept ALL his clothing and sold
            // none. Now: bucket by LOGICAL slot (MageClothingSlot for clothing/
            // jewelry, primary biped slot for rated armor), keep ONE best per slot,
            // ranked to MATCH what EquipBestOwnedGear would actually wear.
            std::unordered_set<RE::TESBoundObject*> keepArmor;
            std::unordered_map<int, RE::TESBoundObject*> bestBySlot;   // logical-slot key -> best obj (worn-inferior convergence)
            if (out.armorJudged) {
                using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                const bool useMageApparel = UsesMageClothing(a_state, a_follower) && Config::g_mageWearRobes.load() &&
                                            !Config::g_dollsMode.load();
                const std::uint8_t top2   = useMageApparel ? TopTwoSchoolMask(a_follower) : 0;
                const bool schoolPrimary  = Config::g_mageApparelStrictSchool.load();
                const bool allowVillain   = useMageApparel && IsNecromancerFollower(a_state);
                // A shield is in-role ONLY with a one-hand MELEE role (class Melee, or an equip-melee gambit
                // override, or Auto by perks): "Melee classes allow casting", so a cast gambit no longer
                // takes it away (a spellsword keeps his shield). A follower with no melee role, or a
                // dual wielder (roles.offHand == 2: his left hand holds a weapon), never equips one, so it
                // is dead weight that sells. Armor, so the weapon keep set above never covered it.
                const WeaponRoles& roles = keepRoles;   // computed once above (keepWeapons)
                const bool usesShield   = keepMeleeTargetClass == WepClass::OneHand && roles.offHand != 2;
                // ARMOR CLASS (2026-09-14): the rated buckets rank by ArmorScore --
                // the SAME judge EquipBestOwnedGear wears by -- so the piece kept
                // per slot is the one the follower will actually wear, and the
                // off-class piece he is still wearing becomes the redundant
                // inferior the force-sell below removes (the trade's RemoveItem is
                // the un-wear). Computed once per scan.
                const ArmorPref keepPref = ArmorPrefFor(a_follower);

                // (a) Always keep what is WORN, regardless of the bucket math (the
                //     sell loop's IsWorn gate already bars worn gear; this is belt-
                //     and-suspenders and also pins the jewelry logical slots).
                //     YIELDS to the redundant-inferior force-sell below: a worn
                //     piece that is not its slot's best BY SCORE bypasses this set
                //     (forceSell) -- that is how a worn off-class cuirass leaves.
                for (int ls = 0; ls < 6; ++ls)
                    if (auto* w = WornInLogicalSlot(a_follower, ls)) keepArmor.insert(w);
                for (auto sl : { Slot::kForearms, Slot::kCalves })
                    if (auto* w = a_follower->GetWornArmor(sl)) keepArmor.insert(w);
                if (usesShield)
                    if (auto* w = a_follower->GetWornArmor(Slot::kShield)) keepArmor.insert(w);

                // (b) The single best NEXT-UPGRADE per LOGICAL slot.
                auto armorLogicalSlot = [](std::uint32_t mask) -> int {
                    if (mask & static_cast<std::uint32_t>(Slot::kBody))   return 1;
                    if (IsHeadSlotMask(mask))                             return 0;   // Head|Hair|Circlet (vanilla helmets have no bit 30)
                    if (mask & static_cast<std::uint32_t>(Slot::kHands))  return 2;
                    if (mask & static_cast<std::uint32_t>(Slot::kFeet))   return 3;
                    if (mask & static_cast<std::uint32_t>(Slot::kShield)) return 4;
                    return -1;
                };
                struct Best { RE::TESBoundObject* obj = nullptr; float primary = -1e9f; float secondary = -1e9f; };
                std::unordered_map<int, Best> best;   // key: clothing 0..5, rated armor 10..14
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || ArmorWontRender(a_follower, ar)) continue;   // wont-render armor is never kept: it sells
                    int key = -1; float primary = 0.0f, secondary = 0.0f;
                    if (useMageApparel) {
                        // A mage's body/clothing slot holds ONE item -- a robe (rating 0)
                        // OR a rated-armor outfit both compete for the SAME biped slot, so
                        // bucket BOTH by MageClothingSlot (one bucket per slot) instead of
                        // splitting rated-vs-clothing (which let two body pieces both survive).
                        // Rated armor competes at plain tier by BASE value (no gem), so a
                        // pricier outfit beats a cheaper robe and only one survives
                        // (marth: Khajiit ~800 must beat Nord ~100, not bucket apart).
                        const int cs = MageClothingSlot(ar);
                        if (cs < 0) continue;   // shields/other -> a mage doesn't wear them, sells
                        int t = 0; std::int32_t m = 0;
                        if (ar->GetArmorRating() > 0.0f) {
                            t = 0; m = std::max<std::int32_t>(ar->GetGoldValue(), 0);
                        } else if (!MageApparelBuyKey(ar, top2, schoolPrimary, allowVillain, t, m)) {
                            continue;
                        }
                        key = cs; primary = static_cast<float>(t); secondary = static_cast<float>(m);
                    } else if (ar->GetArmorRating() > 0.0f) {                // non-mage: rated armor by biped slot
                        const int ls = armorLogicalSlot(static_cast<std::uint32_t>(ar->GetSlotMask()));
                        if (ls < 0) continue;
                        if (ls == 4 && !usesShield) continue;   // don't keep a shield for a non-shield-user -> it sells
                        key = 10 + ls;
                        primary   = ArmorScore(keepPref, ar);   // class x perk biased rating, never the raw rating
                        secondary = static_cast<float>(std::max<std::int32_t>(ar->GetGoldValue(), 0));
                    } else {
                        continue;   // a non-mage's clothing/jewelry is sellable junk (nothing wears it)
                    }
                    auto& b = best[key];
                    if (!b.obj || primary > b.primary ||
                        (primary == b.primary && secondary > b.secondary))
                        b = { obj, primary, secondary };
                }
                for (auto& [k, b] : best) if (b.obj) { keepArmor.insert(b.obj); bestBySlot[k] = b.obj; }
                // The ring / amulet the declaration CHOSE (EquipAuthority.cpp ChooseJewelry): never sold from under it,
                // for a non-mage too (his jewelry used to fall through to "sellable junk" above).
                if (!Config::g_dollsMode.load()) {
                    RE::TESObjectARMO *chosenRing = nullptr, *chosenAmulet = nullptr;
                    DeclaredJewelry(a_follower, a_state, chosenRing, chosenAmulet);
                    if (chosenRing)   keepArmor.insert(chosenRing);
                    if (chosenAmulet) keepArmor.insert(chosenAmulet);
                }
            }

            // 86e3phy41 "Wear helmets" OFF for him: every helmet he CARRIES stays in the bag (marth
            // 2026-10-12), whether or not armor is judged (bEconomyBuyGear). No new one is looted or bought
            // while OFF, so this only holds what he had. Economy's sell loop is the only keep.armor reader.
            if (!WearsHelmets(&a_state)) {
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (auto* ar = obj->As<RE::TESObjectARMO>(); ar && IsHelmetPiece(ar)) keepArmor.insert(obj);
                }
            }

            // THE MAGE BACKUP (review round 1 on 4f23c30, the caster backup-dagger
            // churn): a magic user's loot judge upgrades ONE sidearm by attack damage
            // (daggers only under bMageDaggersOnly, BuildEquipmentContext's
            // wantBackup contract), while bucket 1 above ranks one-handers by the
            // perk-biased WeaponScore -- so the dagger the loot side just took could
            // lose bucket 1 to a sword, sell, and be looted again. Keep the loot
            // judge's own backup pick too, by the loot judge's own rule and inputs.
            {
                const EquipmentContext ctx = BuildEquipmentContext(a_follower, &a_state);
                if (ctx.wantBackup) {
                    RE::TESBoundObject* backup = nullptr;
                    std::uint16_t       backupDmg = 0;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* w = obj->As<RE::TESObjectWEAP>();
                        if (!w || IsCreatureWeapon(w) ||
                            (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID()))) continue;
                        if (WeaponClassOf(w->GetWeaponType()) != WepClass::OneHand) continue;
                        if (ctx.daggersOnly && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                        if (!backup || w->GetAttackDamage() > backupDmg) { backup = obj; backupDmg = w->GetAttackDamage(); }
                    }
                    if (backup) keepWeapons.insert(backup);
                }
            }

            // THE SILVER SLOT (ClickUp 86e3p2gp9, logistics/Silver.cpp): tracked independently of
            // the main weapon -- the best owned silver melee weapon and the best one-handed one are
            // never sold or dropped, whatever the class buckets above decided (a follower with an
            // act.equip_silver rule only).
            {
                const auto [silverBest, silverOneHand] = TrackedSilver(a_follower, a_state);
                if (silverBest)    keepWeapons.insert(silverBest);
                if (silverOneHand) keepWeapons.insert(silverOneHand);
            }

            out.roles      = keepRoles;
            out.weapons    = std::move(keepWeapons);
            out.armor      = std::move(keepArmor);
            out.bestBySlot = std::move(bestBySlot);
            return out;
        }

        // ── AMMO ────────────────────────────────────────────────────────────
        bool AmmoSwapEligible(const RE::TESAmmo* a_ammo) {
            // A non-playable ammo (a Bound Bow's conjured arrows) is the engine's,
            // not the follower's: never ranked, never superseding, never removed.
            return a_ammo && !a_ammo->GetRuntimeData().data.flags.all(RE::AMMO_DATA::Flag::kNonPlayable);
        }

        bool AmmoIsSpecialBase(const RE::TESAmmo* a_ammo) {
            // An ammo whose projectile carries an EXPLOSION (Dawnguard's fire / frost
            // / shock arrows, exploding bolts, modded elemental ammo) delivers its
            // magic effect through it: it is a player's special round, not a tier
            // of the ordinary damage ladder.
            if (!a_ammo) return false;
            const auto* proj = a_ammo->GetRuntimeData().data.projectile;
            // SILVER AMMO (86e3p2gp9, marth 2026-10-12) is the silver slot's: outside the ordinary ladder too.
            return (proj && proj->data.explosionType) || IsSilverAmmo(a_ammo);
        }

        bool AmmoIsSpecial(const RE::TESAmmo* a_ammo, RE::InventoryEntryData* a_entry) {
            return AmmoIsSpecialBase(a_ammo) || (a_entry && a_entry->IsEnchanted());
        }

        float AmmoDamage(const RE::TESAmmo* a_ammo) {
            return a_ammo ? a_ammo->GetRuntimeData().data.damage : 0.0f;
        }

        bool UsesAmmoKind(const FollowerState* a_state, const WeaponRoles& a_roles, bool a_wantBolt) {
            // "Actually uses the kind" (review round 1 on 4f23c30): his stable
            // ranged role is this kind -- a carried bow (arrows) or crossbow
            // (bolts), ComputeWeaponRoles' signal -- and, for a magic user, only
            // with an equip-ranged gambit (the loot/buy judges' caster exception,
            // BuildBuyThresholds' doRanged). A gambit for the kind is NOT use: the
            // seeded "arrows below 10" rule sits on every follower.
            if (!a_roles.doRanged || a_roles.wantCrossbow != a_wantBolt) return false;
            if (a_state && IsCasterFollower(*a_state) &&
                !TableHasAction(a_state->combat(), Vocab::kActEquipRanged))
                return false;
            return true;
        }

        int AmmoKeepTarget(const FollowerState* a_state, bool a_wantBolt, bool a_usesKind) {
            if (!a_usesKind) return 0;   // not his kind: never judged or shed (with NO weapon of the kind, EconomyProbe sells it: the off-kind pass)
            const char* cond = a_wantBolt ? Vocab::kCondSelfOutOfBolts : Vocab::kCondSelfOutOfArrows;
            float want = 0.0f;
            if (a_state) {
                for (const auto& g : a_state->logistics())
                    if (g.enabled && g.conditionOpcode == cond) want = std::max(want, g.conditionParam);
            }
            return std::max(kAmmoKeepFloor, static_cast<int>(std::ceil(want)));
        }

        std::vector<AmmoStack> HeldAmmo(RE::Actor* a_follower, bool a_wantBolt) {
            std::vector<AmmoStack> out;
            if (!a_follower) return out;
            const RE::FormID fid = a_follower->GetFormID();
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* am = obj->As<RE::TESAmmo>();
                if (!am || AmmoIsBolt(am) != a_wantBolt || !AmmoSwapEligible(am)) continue;
                auto* entry = data.second.get();
                AmmoStack s;
                s.obj     = obj;
                s.count   = static_cast<std::int32_t>(data.first);
                s.dmg     = AmmoDamage(am);
                s.value   = entry ? entry->GetValue() : 0;
                s.held    = true;
                s.worn    = entry && entry->IsWorn();
                s.special = AmmoIsSpecial(am, entry);
                s.pinned  = s.worn || s.special || IsStockGear(fid, obj->GetFormID()) ||
                            IsPlayerPick(fid, obj->GetFormID()) ||
                            Lotd::HoldFromSale(fid, obj) ||   // LOTD: a needed relic is never sold or dropped
                            IsQuestObjectInstance(entry) || Catalog::IsExcluded(obj->GetFormID());
                out.push_back(s);
            }
            return out;
        }

        AmmoRank AmmoCutoff(std::vector<AmmoStack>& a_pool, int a_target) {
            std::stable_sort(a_pool.begin(), a_pool.end(), [](const AmmoStack& a, const AmmoStack& b) {
                return AmmoRankAbove(a.dmg, a.value, b.dmg, b.value);
            });
            if (a_target <= 0) return {};
            std::int64_t cum = 0;
            for (const auto& s : a_pool) {
                if (s.special) continue;   // special rounds never count toward retiring the ladder
                cum += s.count;
                if (cum >= a_target) return AmmoRank{ s.dmg, s.value, true };
            }
            return {};   // short of the target: nothing is obsolete
        }

        std::vector<AmmoStack> ObsoleteHeldAmmo(RE::Actor* a_follower, bool a_wantBolt, int a_target) {
            std::vector<AmmoStack> pool = HeldAmmo(a_follower, a_wantBolt);
            const AmmoRank cutoff = AmmoCutoff(pool, a_target);
            std::vector<AmmoStack> out;
            for (const auto& s : pool)
                if (AmmoObsolete(s, cutoff)) out.push_back(s);
            // LOWEST FIRST (marth): the weakest arrows are the first to go.
            std::stable_sort(out.begin(), out.end(), [](const AmmoStack& a, const AmmoStack& b) {
                return AmmoRankAbove(b.dmg, b.value, a.dmg, a.value);
            });
            return out;
        }

        bool AmmoUpgradeBar(RE::Actor* a_follower, bool a_wantBolt, int a_target,
                            AmmoRank& a_outBar, std::int32_t& a_outQty) {
            a_outBar = {}; a_outQty = 0;
            if (a_target <= 0) return false;
            std::vector<AmmoStack> pool = HeldAmmo(a_follower, a_wantBolt);
            std::erase_if(pool, [](const AmmoStack& s) { return s.special; });
            if (pool.empty()) return false;   // nothing held: that is a RESTOCK (the need quota), not a swap-up
            AmmoCutoff(pool, a_target);       // sorts best-first
            // The BAR is the weakest tier he still RELIES on: the cutoff tier when
            // he holds the target, else his weakest stack (every stack is relied on).
            std::int64_t cum = 0;
            AmmoRank bar{ pool.back().dmg, pool.back().value, true };
            for (const auto& s : pool) {
                cum += s.count;
                if (cum >= a_target) { bar = AmmoRank{ s.dmg, s.value, true }; break; }
            }
            // How many rounds ABOVE the bar would carry the whole target on their own
            // (then everything at/under the bar becomes obsolete and sells).
            std::int64_t above = 0;
            for (const auto& s : pool) if (AmmoRankAbove(s.dmg, s.value, bar.dmg, bar.value)) above += s.count;
            a_outBar = bar;
            a_outQty = static_cast<std::int32_t>(std::max<std::int64_t>(0, a_target - above));
            return a_outQty > 0;
        }

        // THE RULE's TAKE SIDE (moved verbatim out of SwapUpAmmoFrom, 86e3f9pkg, so a LOOSE
        // ammo ref is judged by the same rule as a stack in a body -- LooseAmmoQualifies):
        // which of a_body's stacks the rule keeps. Empty = take nothing.
        std::vector<AmmoStack> SwapUpAmmoTakeSet(RE::Actor* a_follower, bool a_wantBolt, int a_target,
                                                 bool a_upgradeOnly, const std::vector<AmmoStack>& body,
                                                 const std::vector<AmmoStack>* a_held) {
            // a_held: the caller's HeldAmmo(a_follower, a_wantBolt), when it already has one
            // (the loose scan reads it once per scan, not once per loose ref).
            std::vector<AmmoStack> held = a_held ? *a_held : HeldAmmo(a_follower, a_wantBolt);
            // ELIGIBILITY (what this call may take):
            //  * RESTOCK (the arrows/bolts gambit, a_upgradeOnly=false): at-or-above
            //    his WORST held ordinary ammo -- a low archer restocks the very arrows
            //    he fires; only STRICTLY worse ammo is passed over (the shipped rule).
            //  * UPGRADE (the equipment gambit, a_upgradeOnly=true): STRICTLY above
            //    the weakest tier he relies on (AmmoUpgradeBar) -- he is not short,
            //    so only better ammo is worth the walk. Nothing held: not an upgrade.
            AmmoRank floorRank;   // invalid = no floor
            bool     strict = false;
            if (a_upgradeOnly) {
                std::int32_t qty = 0;
                if (!AmmoUpgradeBar(a_follower, a_wantBolt, a_target, floorRank, qty)) return {};
                strict = true;
            } else {
                for (const auto& h : held) {
                    if (h.special) continue;
                    if (!floorRank.valid || AmmoRankAbove(floorRank.dmg, floorRank.value, h.dmg, h.value))
                        floorRank = AmmoRank{ h.dmg, h.value, true };
                }
            }
            // THE RULE over the COMBINED pool (what he holds + what the body offers):
            // a body stack the rule would call obsolete the moment it landed is never
            // taken -- so a take is never shed straight back (the walk-back-to-the-
            // junk-it-just-shed loop the old LootAmmo peek comment warned about).
            std::vector<AmmoStack> pool = held;
            pool.insert(pool.end(), body.begin(), body.end());
            const AmmoRank cutoff = AmmoCutoff(pool, a_target);
            std::vector<AmmoStack> take;
            for (const auto& b : body) {
                bool ok = true;
                if (floorRank.valid) {
                    // The damage floor applies to a special round too (the shipped
                    // restock's rule); once held it is pinned, never shed.
                    ok = strict ? AmmoRankAbove(b.dmg, b.value, floorRank.dmg, floorRank.value)
                                : !AmmoRankAbove(floorRank.dmg, floorRank.value, b.dmg, b.value);
                }
                if (ok && !AmmoObsolete(b, cutoff)) take.push_back(b);
            }
            return take;
        }

        // The route-2b twin of the RESTOCK take (86e3f9pkg): a LOOSE ammo ref (a_ammo its
        // base, already of the wanted class) qualifies iff SwapUpAmmoTakeSet would take it
        // out of a body -- at-or-above his worst held ordinary ammo and not obsolete the
        // moment it landed. a_target is the caller's AmmoKeepTarget (computed once per scan,
        // exactly as LootAmmo computes it). A loose ref has no InventoryEntryData: its value
        // is the base's (GetGoldValue, the same InventoryEntryData read of a plain item) and
        // "special" is the base's explosion projectile; an enchanted loose round (ExtraEnchantment
        // on the ref) is not read -- see MAP. The SHED half does not run for a loose pickup
        // (there is no body to drop into); obsolete held ammo goes at the next vendor.
        bool LooseAmmoQualifies(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, RE::TESAmmo* a_ammo,
                                bool a_wantBolt, int a_target, const std::vector<AmmoStack>& a_held) {
            if (!a_follower || !a_ref || !a_ammo || !AmmoSwapEligible(a_ammo)) return false;
            AmmoStack s;
            s.obj     = a_ammo;
            s.count   = std::max<std::int32_t>(1, static_cast<std::int32_t>(a_ref->extraList.GetCount()));
            s.dmg     = AmmoDamage(a_ammo);
            s.value   = a_ammo->GetGoldValue();
            s.special = AmmoIsSpecialBase(a_ammo);
            s.pinned  = s.special;
            return !SwapUpAmmoTakeSet(a_follower, a_wantBolt, a_target, false, { s }, &a_held).empty();
        }

        bool SwapUpAmmoFrom(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_wantBolt,
                            int a_target, bool a_upgradeOnly, bool a_peek) {
            if (!a_follower || !a_src) return false;
            // Collect first, mutate after -- RemoveItem dispatches
            // TESContainerChangedEvent synchronously, so touching an inventory
            // mid-walk is the #2 landmine.
            std::vector<AmmoStack> body;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* am = obj->As<RE::TESAmmo>();
                if (!am || AmmoIsBolt(am) != a_wantBolt || !AmmoSwapEligible(am)) continue;
                auto* entry = data.second.get();
                if (IsQuestObjectInstance(entry)) continue;   // quest ammo stays where it is
                AmmoStack s;
                s.obj = obj; s.count = static_cast<std::int32_t>(data.first); s.dmg = AmmoDamage(am);
                s.value   = entry ? entry->GetValue() : 0;
                s.special = AmmoIsSpecial(am, entry);
                s.pinned  = s.special;   // a special round is never obsolete, here or once held
                // The UPGRADE road buys into the damage ladder only; a special round
                // is the restock road's (or the player's) business.
                if (a_upgradeOnly && s.special) continue;
                body.push_back(s);
            }
            if (body.empty()) return false;

            std::vector<AmmoStack> take = SwapUpAmmoTakeSet(a_follower, a_wantBolt, a_target, a_upgradeOnly, body);
            if (a_peek) return !take.empty();
            if (take.empty()) return false;

            // TAKE best-first, so the carry-weight gate drops iron, never ebony.
            std::stable_sort(take.begin(), take.end(), [](const AmmoStack& a, const AmmoStack& b) {
                return AmmoRankAbove(a.dmg, a.value, b.dmg, b.value);
            });
            std::int32_t taken = 0;
            for (const auto& b : take) {
                if (!FitsCarryWeight(a_follower, b.obj->GetWeight() * b.count)) continue;
                a_src->RemoveItem(b.obj, b.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_follower);
                taken += b.count;
            }
            if (taken == 0) return false;

            // NOTHING IS DROPPED BACK (marth 2026-10-18: "We dont do handing or dropping anymore. It's
            // selling"): the stacks this take made obsolete stay in his pack for the vendor's sell list
            // (Economy.cpp, ObsoleteHeldAmmo at the same target). A target of 0 is a plain restock.
            spdlog::info("[swapup] {:08X}: {} {} took {} from {:08X}, target={}",
                         a_follower->GetFormID(), a_upgradeOnly ? "UPGRADE" : "RESTOCK",
                         a_wantBolt ? "bolts" : "arrows", taken, a_src->GetFormID(), a_target);
            return true;
        }

}
