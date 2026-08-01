#include "PCH.h"
#include "Logistics.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include <cmath>          // std::sin/cos/sqrt for the view cone
#include "Confidence.h"   // the confidence leash (core tenet)
#include "Packages.h"     // Option A: LootTravelFill / LootTravelClear
#include "Forms.h"        // g_travelPackage / g_lootQuest (WALK diagnostic)
#include "Probe.h"        // Probe::CrosshairTarget (the QuickLoot-aware claim signal)
#include "ItemCatalog.h"  // load-order item catalog: potion class + never-loot exclusions
#include "MEOBridge.h"    // MEO gem transfer on gear swap (#17) + WornUid
#include "Papyrus.h"      // route 2b acquire probe: VM-dispatched ObjectReference.Activate
#include "MainThread.h"   // the pump (§0.37): live-vendor reads MUST run on the main thread
#include "TradeBridge.h"  // #21 econ bridge: MFO_Trade Papyrus round-trip (Phase 0 self-test)

// <windows.h> is BANNED outside Board.cpp (it #defines GetObject and hijacks
// BGSDefaultObjectManager::GetObject<T>) -- so declare the one Win32 call we
// need by hand, exactly as Targeting.cpp does. Used only for QuickLoot presence.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);

// The logistics table. DESIGN.md §4.8. Out-of-combat upkeep: drink H/S/M
// potions, loot arrows / potions / better gear -- one action per idle tick,
// behind bLogistics, and NEVER interleaved with the combat table (§4.8).

namespace MFO::Logistics {

    namespace {

        // ── tuning that is NOT surfaced as config ───────────────────────────
        // The three MCM keys are fFirstDibsDelay / fQuickLootWaiver / bLogistics
        // (the M6 contract). These two are structural, not preferences, so they
        // stay compile-time.

        // §4.8: the idle tick is ~1 s (DESIGN §4.1's out-of-combat rate), NOT the
        // 133 ms combat deadline -- nothing here is reflex-timed, and an
        // inventory walk per tick is only affordable at this cadence.
        constexpr auto kLogisticsInterval = std::chrono::milliseconds(1000);

        // The "consideration radius" (§4.8.3) is now Config::g_lootRadius, tunable
        // by editing the synced INI (fLootRadius) with no rebuild. Until active
        // pathing lands (deferred "Option A"), this is a TELEPORT-GRAB radius --
        // the follower takes from any eligible corpse/container within it without
        // walking there. Arm's-reach (200u) proved too small on the deck: a corpse
        // is rarely that close once a fight ends, so looting looked dead (marth,
        // 2026-07-29). The parent-cell walk (§0.30) still iterates only the
        // follower's own cell, so a radius past one cell (~4096u) gains nothing.

        // The loot LRUs are bounded and deliberately NOT serialized (#22h):
        // worst case after a load is one more first-dibs wait on an
        // already-picked corpse, which is not worth a growing co-save record.
        constexpr size_t kLruCap = 256;

        using Clock = std::chrono::steady_clock;

        // ── transient, main-thread-only, cleared on revert ──────────────────
        // Same discipline as Loadout's g_debt: no lock, no cross-thread reader,
        // reconstructed from live state rather than persisted.

        // When each follower may next take a logistics action. The ~1 s cadence
        // gate; also the natural one-action-per-tick rate limit (§4.8.3).
        std::unordered_map<RE::FormID, Clock::time_point> g_nextTick;

        // The last source the PLAYER took from (Claim-and-Release R1). When their
        // next take is from a DIFFERENT source, the previous one's claim RELEASES
        // -- they took what they wanted there and left the rest. Set by the sink.
        RE::FormID g_lastLootSource = 0;

        // Per-follower per-AV drink cooldown (M5): a duration restore potion
        // must not be chain-drunk while its effect is still active. Key is
        // (follower FormID << 8 | av-index).
        std::unordered_map<std::uint64_t, Clock::time_point> g_drinkUntil;

        // Refs the PLAYER has taken from -- the waiver (#22h). Presence collapses
        // the delay to fQuickLootWaiver, and the timestamp RESETS on every take
        // so the follower moves in that many seconds after the player's LAST
        // take, not their first (the QuickLoot-IE case). Bounded LRU.
        std::unordered_map<RE::FormID, Clock::time_point> g_playerLooted;

        // Evict the oldest entry when a bounded map is over cap. n <= kLruCap and
        // inserts are rare, so the O(n) scan is cheaper than carrying a deque.
        void EvictOldest(std::unordered_map<RE::FormID, Clock::time_point>& a_map) {
            if (a_map.size() <= kLruCap) return;
            auto oldest = a_map.begin();
            for (auto it = a_map.begin(); it != a_map.end(); ++it) {
                if (it->second < oldest->second) oldest = it;
            }
            a_map.erase(oldest);
        }

        RE::FormID PlayerID() {
            auto* p = RE::PlayerCharacter::GetSingleton();
            return p ? p->GetFormID() : 0x14;   // 0x14 is the fixed player FormID
        }

        // (Loot eligibility is now Claim-and-Release -- see g_claim / ClaimRejected
        // / the tier gate in LootNearby. The old flat-delay LootEligible is gone.)

        // ── the follower's equipped ranged weapon, for ammo matching ────────
        // Returns the equipped bow/crossbow, or nullptr. Reads the NAMED
        // follower only (#14).
        // Is this ammo a bolt? The CATALOG decides (read from the real record by
        // the patcher) -- runtime TESAmmo::IsBolt() proved unreliable: vanilla
        // Iron/Steel/Ancient Nord arrows report IsBolt()==true here, so the arrow
        // gambit rejected every arrow on a corpse (deck arrowprobe, 000C5684).
        // Uncatalogued ammo falls back to IsBolt() (mod still runs with no patcher).
        bool AmmoIsBolt(RE::TESAmmo* a_ammo) {
            switch (Catalog::AmmoKind(a_ammo->GetFormID())) {
            case Catalog::Ammo::kArrow: return false;
            case Catalog::Ammo::kBolt:  return true;
            default:                    return a_ammo->IsBolt();
            }
        }

        // Ammo of one class (bolts vs arrows) the actor carries. NO bow gate:
        // the ACTION is dumb -- whether a follower should gather ammo is the
        // GAMBIT's condition to decide, not this function's (marth). Arrows and
        // bolts are counted/looted separately (they are different gambits).
        int AmmoCount(RE::Actor* a_actor, bool a_wantBolt) {
            if (!a_actor) return 0;
            int n = 0;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* ammo = obj->As<RE::TESAmmo>(); ammo && AmmoIsBolt(ammo) == a_wantBolt)
                    n += data.first;
            }
            return n;
        }

        // ── carry-weight guard (§4.8.3) ─────────────────────────────────────
        // Never loot a follower into being overencumbered. Reads the named
        // follower's live weight and carry cap.
        bool FitsCarryWeight(RE::Actor* a_follower, float a_addWeight) {
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return false;
            const float cap  = avo->GetActorValue(RE::ActorValue::kCarryWeight);
            const float have = a_follower->GetWeightInContainer();
            return (have + a_addWeight) <= cap;
        }

        // ── the three loot actions ──────────────────────────────────────────
        // Each transfers from a source ref into the follower and returns true if
        // anything moved. COLLECT-THEN-ACT is the caller's job (#2): these are
        // called AFTER the ForEachReferenceInRange walk has finished, on a
        // handle re-resolved at act time, so a container mutation never happens
        // mid-iteration of the world's ref list.

        // Take all ammo of one class (arrows OR bolts) from the source. No bow
        // gate -- the gambit's condition decides whether to gather at all.
        // a_peek: read-only "does this source hold anything I'd take?" -- return
        // true on the first match WITHOUT transferring. Used to skip walking to a
        // body that hasn't got what the gambit wants (marth).
        bool LootAmmo(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_wantBolt,
                      bool a_peek = false) {
            // Collect matching ammo tuples first (object, count), THEN transfer
            // -- RemoveItem dispatches TESContainerChangedEvent synchronously, so
            // mutating the source inventory mid-walk is the #2 landmine.
            struct Take { RE::TESBoundObject* obj; std::int32_t count; float dmg; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* ammo = obj->As<RE::TESAmmo>(); ammo && AmmoIsBolt(ammo) == a_wantBolt) {
                    if (a_peek) return true;
                    takes.push_back({ obj, data.first,
                                      ammo->GetRuntimeData().data.damage });
                }
            }
            if (a_peek) return false;
            // QUALITY FIRST (#27): transfer the HIGHEST-damage ammo before the
            // rest, so when the carry-weight gate cuts the haul short it is the
            // iron arrows left behind, never the ebony. Damage is the record's
            // own DATA field (AMMO_DATA::damage) -- the same number the game
            // shows -- so "better" is measured, not name-guessed.
            std::sort(takes.begin(), takes.end(),
                      [](const Take& a, const Take& b) { return a.dmg > b.dmg; });
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // Is this alchemy item a POTION the follower would drink? Any potion --
        // health, stamina, magicka, fortify, resist, cure -- but NOT a poison
        // (that is a weapon coating) or food.
        bool IsDrinkablePotion(RE::AlchemyItem* a_alc) {
            return a_alc && !a_alc->IsPoison() && !a_alc->IsFood();
        }

        // a_want names WHICH restorative to take; kNone is the catch-all (any
        // drinkable). The gambit's condition still decides WHEN -- this decides
        // WHAT: kNone -> any potion (IsDrinkablePotion, incl. fortify/cure);
        // kHealth/kStamina/kMagicka -> only that restorative (PotionRestores'
        // MGEF archetype). Type is chosen by the ACTION, never inferred from the
        // condition (marth: the any-potion action never replaced per-type loot).
        bool LootPotions(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::ActorValue a_want,
                         bool a_peek = false) {
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (a_want == RE::ActorValue::kNone) {
                    if (!IsDrinkablePotion(alc)) continue;         // catch-all: any drinkable
                } else {
                    if (!alc || PotionRestores(alc) != a_want) continue;   // only this resource
                }
                if (a_peek) return true;
                takes.push_back({ obj, data.first });
            }
            if (a_peek) return false;
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // Is this armor a strict upgrade on at least one slot it covers, and
        // beaten on none? "Better" is the item's OWN rating vs what the follower
        // wears in the same slots -- the §4.8.2 derived-vocabulary principle, so
        // modded gear works with no patch. Reads the named follower.
        //
        // SCOPED (see the module report): "weighted by their armor skill" and
        // the light/heavy-skill steer are a refinement not implemented here --
        // this compares raw armor rating. It never upgrades a follower's whole
        // outfit in one tick, only the single better piece it transfers.
        // A follower should only pick up armour of the CLASS they can actually use:
        // Auri (Heavy Armor skill 5) kept looting heavy plate on raw rating alone
        // (marth). Take heavy only when the follower is STRICTLY more heavy- than
        // light-skilled; take light whenever they are at least as light-skilled.
        // Ties and pure casters (both low) fall to light, never heavy. Clothing has
        // no skill and is rating 0 (rejected below) so it rides through.
        bool ArmorClassSuits(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            using AT = RE::BGSBipedObjectForm::ArmorType;
            const auto type = a_armo->GetArmorType();
            if (type == AT::kClothing) return true;
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return true;
            const float heavy = avo->GetActorValue(RE::ActorValue::kHeavyArmor);
            const float light = avo->GetActorValue(RE::ActorValue::kLightArmor);
            return (type == AT::kHeavyArmor) ? (heavy > light) : (light >= heavy);
        }

        bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            const float cand = a_armo->GetArmorRating();
            if (cand <= 0.0f) return false;   // clothing / jewellery -- not armor
            if (!ArmorClassSuits(a_follower, a_armo)) return false;   // wrong class for this follower

            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            static constexpr Slot kSlots[] = {
                Slot::kHead, Slot::kBody, Slot::kHands, Slot::kForearms,
                Slot::kFeet, Slot::kCalves, Slot::kShield,
            };
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            bool overlapsAny = false;
            for (const auto slot : kSlots) {
                if (!(mask & static_cast<std::uint32_t>(slot))) continue;
                overlapsAny = true;
                auto* worn = a_follower->GetWornArmor(slot);
                if (worn && worn->GetArmorRating() >= cand) {
                    return false;   // beaten on a slot it would replace
                }
            }
            // Better only if it actually covers an armor slot (skip amulets/rings
            // whose bits are not in kSlots) and nothing it replaces is >= it.
            return overlapsAny;
        }

        // ── skill-aware weapon selection ────────────────────────────────────
        // marth: "loot equipment based on the follower's combat skills." A weapon
        // upgrade is judged WITHIN the follower's DOMINANT weapon-skill class, and
        // a better in-class weapon beats an out-of-class one they merely happen to
        // hold -- so a two-hander specialist stuck with a dagger will take a
        // greatsword. Classified by the follower's OWN skills and the weapon's
        // type, never by name (§4.8.2). (Armor's light/heavy steer is still
        // scoped: ArmorIsBetter compares raw rating on the slot -- the heavy/light
        // steer wants a CommonLib armor-type call not verified on this offline box.)
        enum class WepClass : std::uint8_t { OneHand, TwoHand, Ranged, Other };

        WepClass WeaponClassOf(RE::WEAPON_TYPE a_t) {
            switch (a_t) {
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:      return WepClass::TwoHand;
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:        return WepClass::Ranged;
            case RE::WEAPON_TYPE::kStaff:
            case RE::WEAPON_TYPE::kHandToHandMelee: return WepClass::Other;
            default:                                return WepClass::OneHand;  // 1h sword/dagger/axe/mace
            }
        }

        WepClass BestWeaponClass(RE::Actor* a_f) {
            auto* avo = a_f->AsActorValueOwner();
            if (!avo) return WepClass::OneHand;
            const float one = avo->GetActorValue(RE::ActorValue::kOneHanded);
            const float two = avo->GetActorValue(RE::ActorValue::kTwoHanded);
            const float arc = avo->GetActorValue(RE::ActorValue::kArchery);
            if (two >= one && two >= arc) return WepClass::TwoHand;
            if (arc >= one && arc >= two) return WepClass::Ranged;
            return WepClass::OneHand;
        }

        // The follower currently being serviced this tick, set at ServiceFollower
        // entry. Loot code deep in the call tree (LootEquipment) reads the gambit
        // table through it without threading a_state through every signature --
        // safe because the worker services followers SEQUENTIALLY (one
        // ServiceFollower at a time), so it points at the right state for the
        // whole of that follower's loot pass. Never dereferenced outside it.
        const FollowerState* g_svc = nullptr;

        // Does this table author the given action anywhere? (e.g. an equip-ranged
        // gambit => the follower is meant to use a bow/crossbow, so loot one.)
        bool TableHasAction(const std::vector<Gambit>& a_tab, const char* a_op) {
            for (const auto& g : a_tab)
                if (g.actionOpcode == a_op) return true;
            return false;
        }

        // A NON-PLAYABLE weapon (record-header flag bit 2 == Mutagen
        // Weapon.MajorFlag.NonPlayable, confirmed set on the Dwarven Sphere Crossbow)
        // is creature/automaton gear with no humanoid mesh -- invisible if a follower
        // equips it, though it still fires. Direct check so the loot filter + heal
        // work off the DLL alone, independent of the catalog's nonplayable exclusion.
        bool IsCreatureWeapon(const RE::TESObjectWEAP* a_w) {
            return a_w && (a_w->GetFormFlags() & (1u << 2)) != 0;
        }

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            // Generalized by CATEGORY, never by item (§4.8.2). One better piece
            // per tick (one action per tick, §4.3). We TRANSFER, then EQUIP -- a
            // real person who finds a better cuirass puts it on, they do not just
            // carry it (marth: the follower is a thinking person). Safe here
            // because logistics runs OUT of combat, where Loadout is not holding
            // a hand for a cast, and MFO has no equip-event sink to loop on;
            // armor slots are independent of the (left-hand) spell hand.
            const WepClass myClass = BestWeaponClass(a_follower);
            auto* equippedWeap = a_follower->GetEquippedObject(false);
            auto* myWeap       = equippedWeap ? equippedWeap->As<RE::TESObjectWEAP>() : nullptr;
            // ONLY loot weapons for an actual weapon-fighter: someone who already
            // wields a real melee/ranged weapon (not a staff, not empty-handed).
            // A staff-wielding healer or a weaponless caster must NOT hoover up
            // the first sword on a corpse -- that is the "arbitrary-weapon vacuum"
            // (marth). Their weapon-skill numbers are incidental, so BestWeaponClass
            // alone is not a licence to arm them.
            const bool wieldsRealWeapon =
                myWeap && WeaponClassOf(myWeap->GetWeaponType()) != WepClass::Other;
            // Baseline to beat: our current weapon's damage ONLY if it is already
            // in our best class; otherwise 0, so an in-class weapon upgrades over
            // an off-class one we happen to hold (the two-hander-with-a-dagger case).
            std::uint16_t baseDmg = (myWeap && WeaponClassOf(myWeap->GetWeaponType()) == myClass)
                                    ? myWeap->GetAttackDamage() : 0;

            RE::TESBoundObject* bestArmor   = nullptr;
            RE::TESBoundObject* bestWeap    = nullptr;
            std::uint16_t       bestWeapDmg = baseDmg;

            // RANGED ACQUISITION (marth): a follower whose gambits include "equip
            // ranged" is MEANT to shoot -- so loot a ranged weapon even when they
            // don't currently wield a real weapon and their best melee class isn't
            // ranged (the wieldsRealWeapon/myClass gates would otherwise never let
            // a swordsman or an empty-handed archer pick one up).
            //
            // BOW vs CROSSBOW must not be conflated: they feed different ammo, so
            // a crossbow-user handed a bow (or vice versa) ends up with a weapon
            // it has no ammo for. Pick the kind the follower can actually feed --
            // whichever ranged weapon they already carry (better of the two), else
            // by the ammo they hold (arrows -> bow, bolts -> crossbow), else bow
            // by default. Loot only THAT kind; baseline is their current of it.
            using WT = RE::WEAPON_TYPE;
            const bool wantsRanged = g_svc && TableHasAction(g_svc->combat(), Vocab::kActEquipRanged);
            bool          wantCrossbow = false;
            std::uint16_t myRangedDmg  = 0;
            if (wantsRanged) {
                std::uint16_t bowDmg = 0, xbowDmg = 0;
                int arrows = 0, bolts = 0;
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                        if (w->GetWeaponType() == WT::kBow)           bowDmg  = std::max(bowDmg,  w->GetAttackDamage());
                        else if (w->GetWeaponType() == WT::kCrossbow) xbowDmg = std::max(xbowDmg, w->GetAttackDamage());
                    } else if (auto* am = obj->As<RE::TESAmmo>()) {
                        (AmmoIsBolt(am) ? bolts : arrows) += data.first;
                    }
                }
                if (bowDmg > 0 || xbowDmg > 0)   wantCrossbow = xbowDmg > bowDmg;   // upgrade what they wield
                else if (arrows > 0 || bolts > 0) wantCrossbow = bolts > arrows;    // else match their ammo
                else                              wantCrossbow = false;             // else default to a bow
                myRangedDmg = wantCrossbow ? xbowDmg : bowDmg;
            }
            RE::TESBoundObject* bestRanged    = nullptr;
            std::uint16_t       bestRangedDmg = myRangedDmg;

            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                // NEVER-LOOT: the catalog marks quest items, artifacts/unique
                // enchantments, and scripted/no-drop gear as off-limits -- leave
                // them for the player (marth). Fail-open with no patcher run.
                if (Catalog::IsExcluded(obj->GetFormID())) continue;

                if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                    // A SHIELD needs a free off-hand; a two-hander or bow user has
                    // none, so it's dead weight -- never loot one for them (marth:
                    // Farkas, a two-hander, picked up a shield).
                    const bool isShield = (static_cast<std::uint32_t>(armo->GetSlotMask())
                        & static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kShield)) != 0;
                    const bool shieldUseless = isShield &&
                        (myClass == WepClass::TwoHand || myClass == WepClass::Ranged);
                    if (!bestArmor && !shieldUseless && ArmorIsBetter(a_follower, armo)) bestArmor = obj;
                } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    if (IsCreatureWeapon(weap)) continue;   // never equip automaton/creature gear
                    const WepClass wc = WeaponClassOf(weap->GetWeaponType());
                    // In the follower's best class, and strictly harder-hitting
                    // than what they effectively wield. A bow never beats a sword
                    // for a swordsman; a greatsword beats a dagger for a 2h user.
                    if (wieldsRealWeapon && wc == myClass &&
                        weap->GetAttackDamage() > bestWeapDmg) {
                        bestWeapDmg = weap->GetAttackDamage();
                        bestWeap    = obj;
                    }
                    // Ranged pickup -- ONLY the follower's kind (bow XOR crossbow),
                    // independent of wieldsRealWeapon/myClass.
                    if (wantsRanged) {
                        const auto wt = weap->GetWeaponType();
                        const bool kindMatch = wantCrossbow ? (wt == WT::kCrossbow) : (wt == WT::kBow);
                        if (kindMatch && weap->GetAttackDamage() > bestRangedDmg) {
                            bestRangedDmg = weap->GetAttackDamage();
                            bestRanged    = obj;
                        }
                    }
                }
            }

            // Prefer the in-class weapon upgrade; then a ranged weapon they need
            // for their equip-ranged gambit; then the better armor. One item this
            // tick (§4.3).
            RE::TESBoundObject* best = bestWeap ? bestWeap
                                     : bestRanged ? bestRanged
                                                  : bestArmor;
            if (a_peek) return best != nullptr;   // just checking for an upgrade
            if (!best) return false;
            if (!FitsCarryWeight(a_follower, best->GetWeight())) return false;

            // MEO gem transfer (#17): capture the OLD worn item this upgrade
            // REPLACES (base + instance uid) BEFORE the swap, so MEO carries its
            // socketed gems onto the new piece once it's worn (fired from the
            // equip event). CROSS-ROLE IS THE BUG (marth): a new BOW must never
            // pull the gems off the follower's MELEE weapon -- it doesn't replace
            // it. So the old item must share the new one's ROLE: melee<->melee,
            // bow<->bow, crossbow<->crossbow, and for armor the same biped slot.
            // Only a WORN old item with gems (uid != 0) qualifies; no match -> no
            // transfer. All no-ops when MEO is absent or nothing has gems.
            RE::FormID    fromBase = 0;
            std::uint16_t fromUid  = 0;
            if (MEOBridge::Available()) {
                RE::TESBoundObject* oldItem = nullptr;
                if (auto* newWeap = best->As<RE::TESObjectWEAP>()) {
                    const auto     newWt   = newWeap->GetWeaponType();
                    const WepClass newRole = WeaponClassOf(newWt);
                    // The follower's currently-WORN weapon in the same role (bow vs
                    // crossbow distinguished within Ranged). Never a melee->ranged
                    // or ranged->melee steal.
                    if (auto* eqW = myWeap) {
                        const auto eqWt = eqW->GetWeaponType();
                        const bool sameRole = (WeaponClassOf(eqWt) == newRole) &&
                            (newRole != WepClass::Ranged || eqWt == newWt);
                        if (sameRole) oldItem = eqW;
                    }
                } else if (auto* newArmo = best->As<RE::TESObjectARMO>()) {
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    static constexpr Slot kSlots[] = {
                        Slot::kHead, Slot::kBody, Slot::kHands, Slot::kForearms,
                        Slot::kFeet, Slot::kCalves, Slot::kShield,
                    };
                    const auto mask = static_cast<std::uint32_t>(newArmo->GetSlotMask());
                    for (const auto s : kSlots) {
                        if (!(mask & static_cast<std::uint32_t>(s))) continue;
                        auto* worn = a_follower->GetWornArmor(s);
                        if (auto uid = worn ? MEOBridge::WornUid(a_follower, worn) : 0; uid != 0) {
                            oldItem = worn; fromUid = uid; break;
                        }
                    }
                }
                if (oldItem && fromUid == 0) fromUid = MEOBridge::WornUid(a_follower, oldItem);
                if (oldItem) fromBase = oldItem->GetFormID();
            }

            a_src->RemoveItem(best, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                              nullptr, a_follower);

            // PUT IT ON. EquipObject on a slot-conflicting armor auto-unequips
            // the worse piece; a weapon takes the right hand. queue=true so the
            // engine applies it on its own next update rather than synchronously
            // re-entering here.
            if (auto* eq = RE::ActorEquipManager::GetSingleton())
                eq->EquipObject(a_follower, best);

            // Move the old piece's gems onto the new one when it becomes worn.
            // No-op if the old item had no gems (fromUid == 0) or MEO is absent.
            MEOBridge::QueueGemMove(a_follower, fromBase, fromUid, best->GetFormID());
            return true;
        }

        // Take all the gold on a corpse/container. Gold001 is the one hardcoded
        // FormID in the game (0x0000000F, Skyrim.esm) -- there is no "gold type"
        // to derive, so this is the sole by-FormID check in the loot code, and
        // it is stable (a base-game record every load order carries). Weightless,
        // so no carry-weight gate; nothing to equip. Held for the player, who
        // gets it back by trading -- which is why gold WAITS out first dibs
        // (below), same as gear: you want first pick of the coin.
        bool LootGold(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            constexpr RE::FormID kGold001 = 0x0000000F;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() != kGold001) continue;
                if (a_peek) return true;
                a_src->RemoveItem(obj, data.first, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                return true;
            }
            return false;
        }

        // Take all the lockpicks on a corpse/container. Same shape as LootGold:
        // the Lockpick is Skyrim.esm's one fixed MISC record (0x0000000A), so a
        // FormID match is the simplest, stable test -- every load order carries
        // it, and mods add lockpick QUANTITY, not new lockpick forms. Weightless
        // consumable, Free tier like ammo (the follower restocks his own picks;
        // nobody competes for them) -- so no carry-weight gate and no dibs wait.
        bool LootLockpicks(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            constexpr RE::FormID kLockpick = 0x0000000A;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() != kLockpick) continue;
                if (a_peek) return true;
                a_src->RemoveItem(obj, data.first, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                return true;
            }
            return false;
        }

        // Is this ARMO a piece of jewellery (amulet/ring)? The patcher's catalog
        // classifies it from the real record; the record heuristic -- worn on
        // the Amulet or Ring biped slot AND zero armor rating (an enchanted
        // circlet-of-armor would still protect; jewellery does not, cf.
        // ArmorIsBetter's clothing test) -- SUPPLEMENTS it (#20): a loaded
        // catalog must never turn the fallback OFF, or a mod-added ring the
        // patcher run predates is invisible until the next patcher run.
        // IsJewelry on an unloaded catalog is just an empty-set miss, so this
        // one OR covers both worlds.
        bool IsJewelryPiece(RE::TESObjectARMO* a_armo) {
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            const bool jewelSlot =
                (mask & static_cast<std::uint32_t>(Slot::kAmulet)) != 0 ||
                (mask & static_cast<std::uint32_t>(Slot::kRing))   != 0;
            return Catalog::IsJewelry(a_armo->GetFormID()) ||
                   (jewelSlot && a_armo->GetArmorRating() <= 0.0f);
        }

        // Take all the jewellery (amulets/rings) on a corpse/container. Held for
        // the player like gold -- Valuables tier, so it WAITS out first dibs
        // (below); nothing to equip. Collect-then-transfer like LootAmmo (#2).
        bool LootJewelry(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!armo || !IsJewelryPiece(armo)) continue;
                // NEVER-LOOT: quest amulets / unique rings stay for the player.
                if (Catalog::IsExcluded(obj->GetFormID())) continue;
                if (a_peek) return true;
                takes.push_back({ obj, data.first });
            }
            if (a_peek) return false;
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // Is this item a soul gem? Catalog-first (the patcher read the real
        // record), with the engine type as the fallback -- TESSoulGem is its own
        // form class (a MISC subclass), so As<TESSoulGem>() is reliable even
        // with no patcher run. Either signal suffices (the jewellery #20
        // lesson: a loaded catalog must not turn the runtime test off).
        bool IsSoulGemItem(RE::TESBoundObject* a_obj) {
            if (Catalog::IsSoulGem(a_obj->GetFormID())) return true;
            return a_obj->As<RE::TESSoulGem>() != nullptr;
        }

        // Take all the soul gems on a corpse/container. Held for the player
        // like gold/jewellery -- Valuables tier, so it WAITS out first dibs
        // (below); nothing to equip. Collect-then-transfer like LootAmmo (#2).
        bool LootSoulGems(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (!IsSoulGemItem(obj)) continue;
                // NEVER-LOOT: quest gems (Azura's Star et al.) stay for the player.
                if (Catalog::IsExcluded(obj->GetFormID())) continue;
                if (a_peek) return true;
                takes.push_back({ obj, data.first });
            }
            if (a_peek) return false;
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // ── DRINK ───────────────────────────────────────────────────────────
        // Consume the BEST (highest-magnitude) restore potion of a_which the
        // follower already carries. The AlchemyItem equip path IS the vanilla
        // "drink" for an actor (DESIGN §4.5 Tier A). Reads/mutates the named
        // follower only.
        std::uint64_t drinkKey(RE::FormID a_fid, RE::ActorValue a_av) {
            return (static_cast<std::uint64_t>(a_fid) << 8) | static_cast<std::uint8_t>(a_av);
        }

        bool DrinkBest(RE::Actor* a_follower, RE::ActorValue a_which) {
            // COOLDOWN: a restore potion works over its duration; do not chain-
            // drink the stack while the first is still active (M5).
            const auto key = drinkKey(a_follower->GetFormID(), a_which);
            if (auto it = g_drinkUntil.find(key);
                it != g_drinkUntil.end() && std::chrono::steady_clock::now() < it->second) {
                return false;
            }

            RE::AlchemyItem* best = nullptr;
            float bestMag = -1.0f;

            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc || PotionRestores(alc) != a_which) continue;

                // Magnitude of the costliest effect -- the same effect
                // PotionRestores classified on, so "best" is by the resource the
                // potion is actually FOR.
                const auto* eff = alc->GetCostliestEffectItem();
                const float mag = eff ? eff->GetMagnitude() : 0.0f;
                if (mag > bestMag) { bestMag = mag; best = alc; }
            }
            if (!best) {
                // POTION PROBE (temp, v0.8.17). marth: "he has 5 health potions
                // but won't drink." PotionRestores classifies by MGEF archetype
                // (kValueModifier/kDualValueModifier) -- a REQUIREM rework may use
                // a different archetype, so his health potions read as kNone and
                // are invisible. Dump every alchemy item he carries with its raw
                // archetype id + primaryAV + our verdict, so the classifier can be
                // fixed to the EXACT archetype Requiem uses. Rate-limited per
                // follower per 15s, only when a wanted drink found nothing.
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextPotProbe;
                auto& pn = s_nextPotProbe[a_follower->GetFormID()];
                const auto tnow = std::chrono::steady_clock::now();
                if (pn.time_since_epoch().count() == 0 || tnow >= pn) {
                    pn = tnow + std::chrono::seconds(15);
                    std::string dump;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* alc = obj->As<RE::AlchemyItem>();
                        if (!alc) continue;
                        const auto* eff = alc->GetCostliestEffectItem();
                        auto* mgef = eff ? eff->baseEffect : nullptr;
                        dump += std::format(
                            " [{} x{} arch={} primAV={} food={} poison={} -> restores={}]",
                            alc->GetFullName() ? alc->GetFullName() : "?", data.first,
                            mgef ? static_cast<int>(mgef->data.archetype) : -1,
                            mgef ? static_cast<int>(mgef->data.primaryAV) : -1,
                            alc->IsFood() ? 1 : 0, alc->IsPoison() ? 1 : 0,
                            static_cast<int>(PotionRestores(alc)));
                    }
                    spdlog::info("[potprobe] {:08X} want={} -- no match. carried alchemy:{}",
                                 a_follower->GetFormID(), static_cast<int>(a_which),
                                 dump.empty() ? " (none)" : dump);
                }
                return false;
            }

            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return false;
            // Equipping a potion on an actor consumes it -- the documented Tier A
            // path. One unit; the engine removes it from the pack.
            mgr->EquipObject(a_follower, best, nullptr, 1);
            // Gate this AV for the potion's own duration (min one logistics
            // interval), so the next tick does not drink the rest of the stack.
            float dur = 1.0f;
            if (const auto* eff = best->GetCostliestEffectItem())
                dur = std::max<float>(1.0f, static_cast<float>(eff->GetDuration()));
            g_drinkUntil[key] = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(static_cast<int>(dur * 1000.0f));
            return true;
        }

        // ── the looting dispatcher ──────────────────────────────────────────
        enum class Category { Arrows, Bolts, Potions, Equipment, Gold, Jewelry, SoulGems, Lockpicks };

        // Category label for the [loot] diagnostic. Naming the scanned category is
        // the ONLY way to read the composition line: "empty=36" is meaningless
        // without knowing whether the follower was hunting arrows (rare on bodies)
        // or gold (common) that tick (marth: "he never loots arrows").
        const char* CatName(Category a_cat) {
            switch (a_cat) {
            case Category::Arrows:    return "arrows";
            case Category::Bolts:     return "bolts";
            case Category::Potions:   return "potions";
            case Category::Equipment: return "equipment";
            case Category::Gold:      return "gold";
            case Category::Jewelry:   return "jewelry";
            case Category::SoulGems:  return "soulgems";
            case Category::Lockpicks: return "lockpicks";
            default:                  return "?";
            }
        }

        // OPTION A travel state -- a SINGLE traveller at a time (one loot quest,
        // one alias pair). Worker-tick-only, NOT serialized: this just remembers
        // the intent; the engine-side alias fill is cleared by Packages on load
        // (#55). kArrivalDist ~= arm's reach: once the engine walks the follower
        // this close, the existing inventory transfer runs.
        constexpr float kArrivalDist = 160.0f;
        // A BATCH EXCURSION, not a single trip. The follower stays claimed
        // (priority 60) across corpses: Walking = en route to `target`; Holding =
        // arrived/leg-failed, seeking the next leg or waiting out a dibs timer.
        enum class TravelPhase { Walking, Holding };
        struct TravelIntent {
            RE::FormID          follower = 0;
            RE::ObjectRefHandle target;
            Category            cat  = Category::Arrows;
            RE::ActorValue      want = RE::ActorValue::kNone;
            Clock::time_point   deadline{};       // per-LEG walk deadline
            bool                active = false;
            TravelPhase         phase = TravelPhase::Walking;
            Clock::time_point   startTime{};      // excursion start -> fExcursionMax cap
            Clock::time_point   lingerUntil{};    // Hold bound -> fBatchLinger
            // NO-PROGRESS detection: an UNREACHABLE target (no navmesh path) sits
            // at a flat distance -- the follower is on the travel package but
            // FROZEN in place (deck: dist=1449 unchanged for 13 s). We track his
            // own WORLD position (not distance-to-target, which plateaus on a
            // detour around a wall): if he has not MOVED for kNoProgress seconds,
            // the target is unreachable -- give up NOW, well before the leg
            // deadline, so he stops cycling unreachable bodies and the excursion
            // ends -> he follows.
            RE::NiPoint3        lastPos{};          // his position at progressAt
            Clock::time_point   progressAt{};       // last time he actually moved
            // ACQUIRE PROBE (route 2b) readback: after an Activate dispatch at a
            // LOOSE ref, the NEXT tick observes what the engine actually did
            // (dispatch is asynchronous -- Papyrus.h -- so same-tick reads lie).
            bool                acquirePending = false;
            RE::FormID          acquireRefID = 0;   // the loose ref, for the log (its handle may die)
            RE::FormID          acquireBase  = 0;   // its base object -- the inventory-delta key
            std::int32_t        acquirePre   = 0;   // follower's count of base BEFORE dispatch
        };
        TravelIntent g_travel;
        constexpr float kMoveEps    = 50.0f;                  // real-movement threshold (units)
        // 7s, not 4: the deck showed reachable bodies transiently flagged
        // "unreachable" at 4s (a momentary stall while repositioning / the player
        // moving) then reached on the next dispatch. More grace kills the false
        // positive; the navmesh gate already catches genuinely off-mesh bodies, so
        // this only needs to catch a follower truly wedged with no path.
        constexpr auto  kNoProgress = std::chrono::seconds(7);

        // Set by an excursion-mode scan when it found loot it could NOT act on
        // because the player's dibs have not released yet (dNotYet > 0). The Hold
        // logic reads it: something still worth waiting for -> linger; else the
        // batch is exhausted -> return to the player. Worker-tick-only.
        bool g_scanSawWaiting = false;

        // kNormal: not (yet) on an excursion -- arm's-reach transfer OR START one
        // by walking to a far corpse (LootTravelFill, claim at 60). kExcursion:
        // already claimed and driving a batch -- the closest eligible candidate
        // drives the tick, arm's-reach -> grab, farther-but-walkable -> RETARGET
        // without releasing (LootTravelRetarget). One action per tick either way.
        enum class LootMode { kNormal, kExcursion };

        // Targets a walk FAILED to reach (navmesh-blocked, or the follower could
        // not close the distance before the deadline). Skipped for a cooldown so
        // closest-first does not re-pick the same unreachable corpse every tick
        // and churn the follower in place (the v0.8.1 loop). Bounded LRU, not
        // serialized. Keyed by the target FormID.
        std::unordered_map<RE::FormID, Clock::time_point> g_travelFailed;
        constexpr auto kTravelFailCooldown = std::chrono::seconds(25);

        // STICKY unreachable set. The transient block above is WIPED by the idle
        // reassess (so a body that becomes reachable once the follower moves gets
        // re-tried) -- but a GEOMETRICALLY unreachable target never will: an
        // off-navmesh item gives a short navmesh path that ENDS far from the item,
        // so the follower walks "there", can't close the last gap, and the wipe
        // makes him re-pick it forever (marth's frozen-Erik loop, v0.8.29: arrow
        // 00020169 navdist=18 vs dist=630, ping-ponged with 0002016A). So the
        // SECOND stall on a ref promotes it here: a long cooldown the reassess does
        // NOT clear. One stall is still just transient (could be a momentary block);
        // two is a verdict.
        std::unordered_map<RE::FormID, Clock::time_point> g_travelUnreach;
        std::unordered_map<RE::FormID, int>               g_stallStrikes;
        constexpr auto kTravelStickyCooldown = std::chrono::minutes(5);

        bool TravelFailedRecently(RE::FormID a_id, Clock::time_point a_now) {
            auto su = g_travelUnreach.find(a_id);
            if (su != g_travelUnreach.end() && a_now < su->second + kTravelStickyCooldown)
                return true;
            auto it = g_travelFailed.find(a_id);
            return it != g_travelFailed.end() && a_now < it->second + kTravelFailCooldown;
        }
        void MarkTravelFailed(RE::FormID a_id, Clock::time_point a_now) {
            if (a_id) { g_travelFailed[a_id] = a_now; EvictOldest(g_travelFailed); }
        }
        // A STALL (no navmesh path at dispatch, or walked with ZERO progress) --
        // transient-block it like any fail, but on the 2nd strike promote it to the
        // sticky set so the idle reassess can't resurrect it into a re-attempt loop.
        void MarkTravelStalled(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            MarkTravelFailed(a_id, a_now);
            if (++g_stallStrikes[a_id] >= 2) {
                g_stallStrikes.erase(a_id);
                g_travelUnreach[a_id] = a_now;
                EvictOldest(g_travelUnreach);
                spdlog::info("[loot] {:08X} STICKY-unreachable (2nd stall) -- won't re-pick "
                             "for {}m (survives idle reassess)", a_id,
                             std::chrono::duration_cast<std::chrono::minutes>(kTravelStickyCooldown).count());
            }
        }

        // IDLE REASSESS (marth: "to be fair, reassess all nearby bodies if nothing
        // else matches for a few cycles"). The blocklist is a churn-guard, not a
        // verdict: a body skipped as unreachable may be reachable once the follower
        // has moved, and a body marked DONE is (post-StripCorpse) genuinely empty
        // so re-scanning it is a cheap HasLoot=false, never a wasted trip. When a
        // follower services NOTHING for a few consecutive idle ticks and no
        // excursion is running, wipe the blocklist so the next scan looks at every
        // body fresh. Bounded to once per window so a truly-unreachable body can't
        // make him re-attempt it every few seconds.
        std::unordered_map<RE::FormID, int> g_idleCycles;
        Clock::time_point                   g_lastBlocklistReassess{};
        constexpr int  kIdleReassessCycles   = 4;                       // ~4 s idle
        constexpr auto kReassessCooldown      = std::chrono::seconds(15);

        // Travel deadline scaled to the distance: enough time to actually walk
        // there at a jog, never so long the follower is stuck if the path is
        // blocked. ~150 u/s effective + 5 s slack, clamped to [6, 20] s.
        Clock::time_point TravelDeadline(float a_dist, Clock::time_point a_now) {
            const float secs = std::clamp(a_dist / 150.0f + 5.0f, 6.0f, 20.0f);
            return a_now + std::chrono::seconds(static_cast<int>(secs));
        }

        // ── CLAIM-AND-RELEASE: is the player CONSIDERING this source right now? ──
        // The one live-claim signal, true for BOTH loot UIs (INVARIANTS #22g and
        // the new #22g-QL). Detected once whether QuickLoot is in the load order:
        // on a QuickLoot list the crosshair over a corpse means its HUD is up (the
        // player is deciding); on a vanilla-menu list the crosshair is mere
        // looking, so it must NOT count there or the follower would yield on every
        // glance. Cheap module-handle presence check; the precise QuickLoot IE
        // event API is a later upgrade. All O(1): a UI flag + two atomic reads.
        bool QuickLootPresent() {
            static const bool present = [] {
                for (const char* dll : { "QuickLootIE.dll", "QuickLootRE.dll",
                                         "QuickLootEE.dll", "QuickLoot.dll" })
                    if (::GetModuleHandleA(dll)) return true;
                return false;
            }();
            return present;
        }

        // PLAYER-HOME gate. A follower ransacking your own house reads as theft,
        // not tidying, so looting is suppressed wherever the player's current
        // LOCATION carries vanilla LocTypePlayerHouse (0x01CB85 in Skyrim.esm) --
        // bought houses, Hearthfire builds, and the home mods that set it. The
        // keyword is resolved once by its FormID (LookupByEditorID is unreliable
        // unless a tweak kept EDIDs). Gated by bLootInPlayerHomes (default OFF);
        // the caller checks the toggle so this stays a pure "are we in a home?".
        bool InPlayerHome() {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) return false;
            static RE::BGSKeyword* kHouse = []() -> RE::BGSKeyword* {
                auto* dh = RE::TESDataHandler::GetSingleton();
                return dh ? dh->LookupForm<RE::BGSKeyword>(0x01CB85, "Skyrim.esm") : nullptr;
            }();
            if (!kHouse) return false;
            auto* loc = pc->GetCurrentLocation();
            return loc && loc->HasKeyword(kHouse);
        }
        bool PlayerIsConsidering(RE::FormID a_sourceID) {
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) return true;   // vanilla menu
            if (QuickLootPresent() && a_sourceID != 0 &&
                Probe::CrosshairTarget() == a_sourceID) return true;               // QuickLoot HUD
            return false;
        }

        // Per-source CLAIM state (Claim-and-Release). The player holds an implicit
        // claim on a source until evidence RELEASES it; this tracks that evidence.
        // Worker-tick-only, bounded LRU, not serialized (#22h: worst case after a
        // load is one more fair-chance wait). Replaces the old bare g_seen map.
        struct Claim {
            Clock::time_point seen{};        // when the FOLLOWER first saw it (gear grace, abandon)
            Clock::time_point lastAccrue{};  // last tick fair-chance was accrued (dedupe, see below)
            float             nearSecs = 0;  // accrued player near+visible time (fair chance)
            bool              everNear = false;   // player ever within chanceRadius (abandon backstop)
            bool              rejected = false;   // player engaged then moved on (R1: to another source)
        };
        std::unordered_map<RE::FormID, Claim> g_claim;
        constexpr float kTickSecs = 1.0f;   // ~kLogisticsInterval, the fair-chance accrual step

        // EvictOldest for the Claim map -- oldest by first-seen. The FormID/time
        // overload above cannot serve it (Claim has no operator<), so this is its
        // own bounded-LRU trim, same shape.
        void EvictOldest(std::unordered_map<RE::FormID, Claim>& a_map) {
            if (a_map.size() <= kLruCap) return;
            auto oldest = a_map.begin();
            for (auto it = a_map.begin(); it != a_map.end(); ++it)
                if (it->second.seen < oldest->second.seen) oldest = it;
            a_map.erase(oldest);
        }

        // Is the player facing a_srcPos (a forward view-cone, ~±60deg, no raycast)?
        // "Had a chance to SEE it" for fair-chance -- proximity alone is not enough
        // (a player facing away has not been shown it). Skyrim heading: 0 = +Y,
        // clockwise, so forward = (sin h, cos h). Cheap: one angle read + a dot.
        bool PlayerFacing(RE::Actor* a_player, const RE::NiPoint3& a_srcPos) {
            if (!a_player) return false;
            const auto p = a_player->GetPosition();
            float dx = a_srcPos.x - p.x, dy = a_srcPos.y - p.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) return true;                 // standing on it
            dx /= len; dy /= len;
            const float h = a_player->GetAngleZ();       // heading, radians
            return (std::sin(h) * dx + std::cos(h) * dy) > 0.5f;   // within ~60deg
        }

        // The player TOOK from this source and has since moved to a DIFFERENT one
        // (R1) or walked away (R2) or gone quiet past the linger (R3) -> the claim
        // is released. Reads the waiver map (g_playerLooted, stamped by the sink)
        // + the per-source rejected flag (R1, set by the sink) + player distance.
        bool ClaimRejected(RE::FormID a_id, const RE::NiPoint3& a_srcPos,
                           const RE::NiPoint3& a_playerPos, Clock::time_point a_now) {
            if (auto it = g_claim.find(a_id); it != g_claim.end() && it->second.rejected)
                return true;   // R1: player looted this, then looted elsewhere
            auto lt = g_playerLooted.find(a_id);
            if (lt == g_playerLooted.end()) return false;   // player never took from it
            if (a_now - lt->second >= std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<float>(Config::g_quickLootWaiver.load())))
                return true;   // R3: quiet for the linger since the last take
            if (a_srcPos.GetDistance(a_playerPos) > Config::g_departRadius.load())
                return true;   // R2: took from it, then walked away
            return false;
        }

        // Can this follower open a_ref's lock with their own Lockpicking skill?
        // marth: a follower does not loot through a lock they could not actually
        // pick. The CommonLib enum is { kUnlocked=-1, kVeryEasy, kEasy, kAverage,
        // kHard, kVeryHard, kRequiresKey } and maps kVeryEasy=Novice, kEasy=
        // Apprentice, kAverage=Adept, kHard=Expert, kVeryHard=MASTER. Vanilla
        // skill thresholds (0/25/50/75); MASTER (kVeryHard), key-required, and
        // inaccessible fall to default and are NEVER pickable -- deliberately out
        // of reach even for a maxed follower. Owned locks never reach here (the
        // ownership gate bars them first), so this only opens UNOWNED containers.
        bool LockPickable(RE::Actor* a_follower, RE::TESObjectREFR* a_ref) {
            float need;
            switch (a_ref->GetLockLevel()) {
            case RE::LOCK_LEVEL::kVeryEasy: need = 0.0f;   break;   // Novice
            case RE::LOCK_LEVEL::kEasy:     need = 25.0f;  break;   // Apprentice
            case RE::LOCK_LEVEL::kAverage:  need = 50.0f;  break;   // Adept
            case RE::LOCK_LEVEL::kHard:     need = 75.0f;  break;   // Expert
            default:                        return false;          // Master / requires key / inaccessible
            }
            auto* avo = a_follower->AsActorValueOwner();
            const float skill = avo ? avo->GetActorValue(RE::ActorValue::kLockpicking) : 0.0f;
            return skill >= need;
        }

        // Walk nearby refs, gate them, and perform ONE transfer. Returns true if
        // something was looted. Collect-then-act (#2): the world walk only reads
        // and records timers; all mutation happens afterwards on re-resolved
        // handles.
        // Transfer the wanted category out of one corpse/container. Shared by the
        // scan act-loop, the excursion arrival, and the arm's-reach path.
        bool LootHere(RE::Actor* a_follower, RE::TESObjectREFR* a_ref,
                      Category a_cat, RE::ActorValue a_want) {
            switch (a_cat) {
            case Category::Arrows:    return LootAmmo(a_follower, a_ref, false);
            case Category::Bolts:     return LootAmmo(a_follower, a_ref, true);
            case Category::Potions:   return LootPotions(a_follower, a_ref, a_want);
            case Category::Equipment: return LootEquipment(a_follower, a_ref);
            case Category::Gold:      return LootGold(a_follower, a_ref);
            case Category::Jewelry:   return LootJewelry(a_follower, a_ref);
            case Category::SoulGems:  return LootSoulGems(a_follower, a_ref);
            case Category::Lockpicks: return LootLockpicks(a_follower, a_ref);
            }
            return false;
        }

        // Read-only: does a_ref hold anything the a_cat gambit would take? The
        // peek path of each LootX, no transfer. Lets the scan skip a body that
        // hasn't got what the follower is after -- he never walks to an empty one.
        bool HasLoot(RE::Actor* a_follower, RE::TESObjectREFR* a_ref,
                     Category a_cat, RE::ActorValue a_want) {
            switch (a_cat) {
            case Category::Arrows:    return LootAmmo(a_follower, a_ref, false, true);
            case Category::Bolts:     return LootAmmo(a_follower, a_ref, true, true);
            case Category::Potions:   return LootPotions(a_follower, a_ref, a_want, true);
            case Category::Equipment: return LootEquipment(a_follower, a_ref, true);
            case Category::Gold:      return LootGold(a_follower, a_ref, true);
            case Category::Jewelry:   return LootJewelry(a_follower, a_ref, true);
            case Category::SoulGems:  return LootSoulGems(a_follower, a_ref, true);
            case Category::Lockpicks: return LootLockpicks(a_follower, a_ref, true);
            }
            return false;
        }

        // ── NEAREST-NAVMESH GATE (the freeze pre-filter) ─────────────────────
        // Distance from a_pos to the nearest navmesh VERTEX in a_cell, or
        // kNoNavmesh if none. The Travel procedure must map its goal to a navmesh
        // TRIANGLE before it can plan; a corpse physics-settled OFF the navmesh
        // (clipped into geometry / on furniture / a disconnected island) yields no
        // goal triangle, so the planner never starts and the follower stands with
        // distance flat (the 1449u/13s freeze). Vertex distance is a cheap upper
        // bound on distance-to-mesh -- good enough to answer "is there mesh near
        // this ref to path to?". Read-only over the DECODED BSNavmesh data on the
        // worker: attached-cell gate + the cell's own spinLock (the guard
        // CommonLib's ForEachReference takes) + smart-pointer mesh elements. po3's
        // PapyrusExtender ships this exact read (MoveToNearestNavmeshLocation) from
        // VM worker threads, live in this load order. NG keeps navMeshes in the
        // RUNTIME_DATA accessor, NOT a flat cell->navMeshes (that is po3's fork).
        constexpr float kNoNavmesh = 1e9f;

        float NearestNavmeshDist(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_pos) {
            if (!a_cell || !a_cell->IsAttached()) return kNoNavmesh;
            auto& rd = a_cell->GetRuntimeData();
            RE::BSSpinLockGuard lock(rd.spinLock);
            auto* arr = rd.navMeshes;
            if (!arr) return kNoNavmesh;

            constexpr std::size_t kVertexBudget = 32768;             // ~0.1 ms of float math
            constexpr float       kGoodEnoughSq = 64.0f * 64.0f;     // clearly on-mesh: stop
            std::size_t visited = 0;
            float bestSq = kNoNavmesh;
            for (const auto& meshPtr : arr->navMeshes) {
                auto* mesh = meshPtr.get();
                if (!mesh) continue;
                for (const auto& v : mesh->vertices) {
                    const float dSq = a_pos.GetSquaredDistance(v.location);
                    if (dSq < bestSq) {
                        bestSq = dSq;
                        if (bestSq <= kGoodEnoughSq) return std::sqrt(bestSq);
                    }
                    if (++visited >= kVertexBudget)
                        return bestSq < kNoNavmesh ? std::sqrt(bestSq) : kNoNavmesh;
                }
            }
            return bestSq < kNoNavmesh ? std::sqrt(bestSq) : kNoNavmesh;
        }

        // Reachability heuristic for a loot ref: nearest navmesh in the ref's OWN
        // cell, and -- if that reads off-mesh AND the follower is in a different
        // cell (exterior grid border: the nearest vertex can be one cell over) --
        // the follower's cell too. Returns the smaller. Big = "no mesh near it".
        float NavmeshReach(RE::Actor* a_follower, RE::TESObjectREFR* a_ref) {
            if (!a_ref) return kNoNavmesh;
            const RE::NiPoint3 p = a_ref->GetPosition();
            float d = NearestNavmeshDist(a_ref->GetParentCell(), p);
            if (d >= kNoNavmesh && a_follower &&
                a_follower->GetParentCell() != a_ref->GetParentCell())
                d = std::min(d, NearestNavmeshDist(a_follower->GetParentCell(), p));
            return d;
        }

        // ACQUIRE PROBE (route 2b). A LOOSE ref: neither a dead actor nor a
        // container base -- the exact INVERSE of LootNearby's lootable test.
        // Only whitelisted loose refs (ammo/gold, below) ever become candidates,
        // so at act/arrival time this only ever distinguishes those from the
        // corpses/containers the transfer path owns.
        bool LooseRef(RE::TESObjectREFR* a_ref) {
            if (!a_ref) return false;
            if (a_ref->As<RE::Actor>()) return false;   // actors are never loose items
            auto* base = a_ref->GetBaseObject();
            return base && !base->Is(RE::FormType::Container);
        }

        bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now,
                        RE::ActorValue a_potionWant = RE::ActorValue::kNone,
                        LootMode a_mode = LootMode::kNormal) {
            // §22g ABSOLUTE BAR, ahead of every delay and waiver: never mutate a
            // container while the player has ANY container menu open -- it breaks
            // the vanilla menu building its list from that container (MEO m19e).
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return false;
            }
            // BEHAVIOUR LAYER: don't loot while the PLAYER is sneaking -- a
            // follower breaking off to grab loot blows your stealth (marth; the
            // deferred FCL gate, §0.32). An invisible courtesy: crouch and they
            // hold off.
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsSneaking())
                return false;
            // PLAYER HOME: don't loot your own house unless opted in (default OFF).
            if (!Config::g_lootInPlayerHomes.load() && InPlayerHome())
                return false;
            // WALK THE FOLLOWER'S OWN CELL, NOT TES::ForEachReferenceInRange.
            // crash4 (2026-07-22, exterior Wilderness): TES::ForEachReferenceInRange
            // ends its exterior branch with `worldSpace->GetSkyCell()`, and
            // TES::worldSpace was a TORN pointer (0x450FE000_45242000 -- two
            // mismatched 32-bit halves) because the engine was mid worldspace/cell
            // stream. It chases three engine-owned pointers (gridCells, worldSpace,
            // skycell) that churn during a transition. The follower's parent cell,
            // when ATTACHED, iterates only its own reference list (no worldspace
            // deref at all -- see TESObjectCELL::ForEachReferenceInRange), and the
            // loot radius is clamped to one 4096u cell, so nothing real is lost.
            // The IsAttached gate also skips the walk outright during a transition,
            // which is exactly when those pointers are unstable.
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached()) return false;
            const auto origin = a_follower->GetPosition();
            const float kLootRadius = Config::g_lootRadius.load();   // tunable, one snapshot per walk

            // THE CONFIDENCE LEASH (core tenet, DESIGN behaviour layer). A
            // candidate must be within this follower's confidence-scaled distance
            // FROM THE PLAYER -- bold when safe (leash -> max, ranges out),
            // cautious when hurt/fighting (leash -> min, stays close). Measured to
            // the PLAYER, while the scan radius above is measured to the FOLLOWER.
            const float leash = Confidence::LeashRadius(a_follower);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const RE::NiPoint3 playerPos = pc ? pc->GetPosition() : origin;

            // HOW FAR HE WILL WALK to a body (follower-relative). marth's tenet:
            // the confidence LEASH is his range, not a fixed room. So the walk
            // limit IS the leash -- bold when safe (ranges across rooms), cautious
            // when hurt. fTravelRadius is only a hard ceiling now (default raised
            // high), for anyone who wants to clamp it below the leash. Previously
            // the fixed 768 hid most in-leash bodies (deck: 6 eligible, only the 2
            // inside 768 ever attempted).
            const float walkLimit = std::min(leash, Config::g_travelRadius.load());

            // Eligible loot sources, collected inside the walk and acted on after
            // it. Bounded so a room full of corpses cannot make the tick unbounded
            // -- but the cap is applied in ARBITRARY cell-list order, BEFORE the
            // closest-first sort, so keep it generous: a low cap could drop a NEAR
            // source in favour of far ones iterated earlier ("ignores near, walks
            // far", audit). The HasLoot gate already trims to the few bodies that
            // hold the wanted category, so hitting even this is uncommon.
            constexpr size_t kMaxCandidates = 48;
            std::vector<RE::ObjectRefHandle> candidates;
            candidates.reserve(kMaxCandidates);

            // DIAGNOSTIC counters (marth: "nothing looted" -- find WHICH stage
            // drops the corpse). Logged rate-limited below.
            int dRefs = 0, dLootable = 0, dOwned = 0, dOffLimits = 0, dLocked = 0, dNotYet = 0, dLeash = 0, dEmpty = 0;

            cell->ForEachReferenceInRange(origin, kLootRadius,
                [&](RE::TESObjectREFR& a_ref) {
                    if (candidates.size() >= kMaxCandidates) return RE::BSContainer::ForEachResult::kStop;
                    ++dRefs;
                    RE::TESObjectREFR* ref = &a_ref;
                    if (ref == a_follower) return RE::BSContainer::ForEachResult::kContinue;
                    if (ref->IsDisabled() || ref->IsMarkedForDeletion())
                        return RE::BSContainer::ForEachResult::kContinue;

                    // A lootable ref is a CORPSE (dead actor) or a CONTAINER --
                    // things we TRANSFER an inventory out of. Living actors are
                    // never touched (that is pickpocketing).
                    //
                    // LOOSE world items are DELIBERATELY excluded here. Picking a
                    // loose ref up is PickUpObject, which tears down the ref's 3D
                    // and mutates the cell -- and this whole tick runs on a BSJobs
                    // JOB WORKER (§0.30), overlapping the streaming threads, the
                    // crash4 class. Re-queuing via SKSE AddTask does NOT escape it:
                    // the task queue itself is drained inside Job_Post_process on a
                    // worker (§0.30, INVARIANTS #72), so there is no main-thread hop
                    // to be had from here. Loose-item pickup is the package-
                    // acquisition feature (ROADMAP "Option A"): the ENGINE walks the
                    // follower to the item and grabs it natively, no PickUpObject on
                    // our side at all. Until then, loose items are simply not looted
                    // -- EXCEPT the route-2b ACQUIRE PROBE whitelist below.
                    bool lootable = false;
                    bool loose    = false;   // route 2b: a loose WORLD item, not an inventory
                    if (auto* actor = ref->As<RE::Actor>()) {
                        lootable = actor->IsDead();
                    } else if (auto* base = ref->GetBaseObject()) {
                        lootable = base->Is(RE::FormType::Container);
                        // ACQUIRE PROBE (route 2b) WHITELIST: for the Arrows /
                        // Bolts / Gold scans ONLY, a loose world ref of exactly
                        // that thing is a candidate too. It rides the SAME
                        // excursion machinery (walk to it; every gate below
                        // still applies) and the acquire happens at ARRIVAL via
                        // a VM-dispatched ObjectReference.Activate in the
                        // excursion driver -- NEVER an in-place PickUpObject
                        // (the worker trap the comment above describes).
                        if (!lootable) {
                            if (a_cat == Category::Arrows || a_cat == Category::Bolts) {
                                if (auto* ammo = base->As<RE::TESAmmo>();
                                    ammo && AmmoIsBolt(ammo) == (a_cat == Category::Bolts))
                                    lootable = loose = true;
                            } else if (a_cat == Category::Gold) {
                                constexpr RE::FormID kGold001 = 0x0000000F;   // see TakeGold
                                if (base->GetFormID() == kGold001)
                                    lootable = loose = true;
                            }
                        }
                    }
                    if (!lootable) return RE::BSContainer::ForEachResult::kContinue;
                    ++dLootable;

                    // OWNERSHIP IS ABSOLUTE (#22e). IsOffLimits() is ONLY a crime
                    // check (IsCrimeToActivate) -- and taking from a PLAYER-owned
                    // chest is not a crime, so it would sail through and let a
                    // follower drain the player's own storage. Check GetOwner()
                    // first (any explicit owner, including the player), then keep
                    // IsOffLimits() as the second bar for cell-owned/no-owner
                    // crime cases. No delay or waiver overrides this.
                    if (ref->GetOwner())    { ++dOwned;     return RE::BSContainer::ForEachResult::kContinue; }
                    if (ref->IsOffLimits()) { ++dOffLimits; return RE::BSContainer::ForEachResult::kContinue; }
                    // Locked -- UNLESS the follower's Lockpicking skill can open
                    // it. RemoveItem ignores locks, so without this a follower
                    // would loot through any lock; with it, only locks their
                    // skill covers (marth). Owned locks are already barred above.
                    if (ref->IsLocked() && !LockPickable(a_follower, ref)) {
                        ++dLocked; return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // Beyond the confidence leash from the player -- too far for
                    // this follower's nerve right now. This is the invisible
                    // string: the same corpse is in-reach when they feel safe and
                    // out-of-reach when they do not.
                    if (playerPos.GetDistance(ref->GetPosition()) > leash) {
                        ++dLeash; return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // MUST HOLD WHAT WE WANT. A read-only peek: never make him walk
                    // to a body that hasn't got the category this gambit is looting
                    // (marth). This is the biggest cut -- a fight leaves many
                    // corpses, few with the one thing (e.g. arrows) he's after --
                    // and it kills the "walk to an empty barrel and stall" trips.
                    // A LOOSE ref (route 2b) IS the loot -- it has no inventory
                    // for HasLoot to peek, so the gate does not apply.
                    if (!loose && !HasLoot(a_follower, ref, a_cat, a_potionWant)) {
                        // ARROW PROBE (temp, v0.8.17). marth: "multiple bodies with
                        // arrows available" yet the arrow scan calls every corpse
                        // empty. When an ARROW scan judges a lootable body empty,
                        // dump the ammo it ACTUALLY holds -- if it shows "Iron Arrow
                        // xN isBolt=0", HasLoot(arrows) is wrong on a body that has
                        // them (vs the body genuinely lacking arrows / being an
                        // adjacent-cell corpse the scan never sees). Per-body 15s.
                        if (a_cat == Category::Arrows) {
                            static std::unordered_map<RE::FormID, Clock::time_point> s_nextAP;
                            auto& an = s_nextAP[ref->GetFormID()];
                            if (an.time_since_epoch().count() == 0 || a_now >= an) {
                                an = a_now + std::chrono::seconds(15);
                                std::string ad; int tot = 0;
                                for (auto& [obj, data] : ref->GetInventory()) {
                                    ++tot;
                                    if (auto* am = obj ? obj->As<RE::TESAmmo>() : nullptr)
                                        ad += std::format(" [{} x{} isBolt={}]",
                                            am->GetFullName() ? am->GetFullName() : "?",
                                            data.first, am->IsBolt() ? 1 : 0);
                                }
                                if (!ad.empty())
                                    spdlog::info("[arrowprobe] {:08X} EMPTY-verdict body {:08X} "
                                                 "invItems={} but HAS ammo:{}",
                                                 a_follower->GetFormID(), ref->GetFormID(), tot, ad);
                            }
                        }
                        ++dEmpty; return RE::BSContainer::ForEachResult::kContinue;
                    }

                    // CLAIM-AND-RELEASE (dibs redesign). Eligibility is per
                    // value-tier, and the player's claim releases on EVIDENCE
                    // ABOUT THE PLAYER, not a wall clock.
                    //   Free (arrows/bolts/potions/lockpicks): released at once --
                    //     nobody competes for the follower's own restock.
                    //   Gear (equipment): a short anti-snatch grace, or rejection.
                    //   Valuables (gold, jewellery, soul gems): rejection,
                    //     fair-chance (player was near AND could see it and left
                    //     it), or the abandonment backstop (player never came
                    //     near).
                    if (a_cat == Category::Arrows || a_cat == Category::Bolts ||
                        a_cat == Category::Potions || a_cat == Category::Lockpicks) {
                        candidates.push_back(ref->GetHandle());
                        return RE::BSContainer::ForEachResult::kContinue;
                    }

                    const RE::FormID    srcId  = ref->GetFormID();
                    const RE::NiPoint3  srcPos = ref->GetPosition();
                    auto& cl = g_claim[srcId];
                    if (cl.seen.time_since_epoch().count() == 0) { cl.seen = a_now; EvictOldest(g_claim); }

                    // Accrue the player's "chance" as REAL elapsed time since this
                    // source last accrued, capped -- so N followers servicing in
                    // one ~1 s window cannot multiply the player's clock (a party
                    // of four must not release valuables 4x faster; L1). The
                    // within-tick dedupe still holds: a follower's per-category
                    // re-walks share a_now, so the second sees dt=0. Near AND
                    // could-see-it: QuickLoot HUD on it (considering) counts x3
                    // (the UI literally shows them the contents); else facing x1.
                    // (Vanilla-menu considering never reaches here -- the walk
                    // returns at the top while a ContainerMenu is open.)
                    {
                        const float dt = cl.lastAccrue.time_since_epoch().count() == 0
                            ? kTickSecs
                            : std::min(std::chrono::duration<float>(a_now - cl.lastAccrue).count(), 2.0f);
                        cl.lastAccrue = a_now;
                        if (dt > 0.0f &&
                            playerPos.GetDistance(srcPos) <= Config::g_chanceRadius.load()) {
                            cl.everNear = true;
                            if (PlayerIsConsidering(srcId))    cl.nearSecs += dt * 3.0f;
                            else if (PlayerFacing(pc, srcPos)) cl.nearSecs += dt;
                        }
                    }

                    const bool rejected = ClaimRejected(srcId, srcPos, playerPos, a_now);
                    bool released;
                    if (a_cat == Category::Equipment) {          // Gear tier
                        released = rejected ||
                                   std::chrono::duration<float>(a_now - cl.seen).count()
                                       >= Config::g_firstDibsDelay.load();   // gear grace
                    } else {                                     // Gold / Jewelry / SoulGems -> Valuables
                        released = rejected ||
                                   cl.nearSecs >= Config::g_fairChance.load() ||
                                   (!cl.everNear &&
                                    std::chrono::duration<float>(a_now - cl.seen).count()
                                        >= Config::g_abandonDelay.load());
                    }
                    if (released) candidates.push_back(ref->GetHandle());
                    else ++dNotYet;
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            // One diagnostic line per follower PER CATEGORY per ~10 s, so the walk
            // composition is visible without flooding the ~1 s tick. Keyed by
            // (follower, category) -- a shared key would hide every category but
            // the first scanned in the window, which is exactly what made "he
            // never loots arrows" un-diagnosable (the arrow scan's empty=N never
            // reached the log). cat= names which scan this line is.
            {
                static std::unordered_map<std::uint64_t, Clock::time_point> s_nextWalkLog;
                const auto key = (static_cast<std::uint64_t>(a_follower->GetFormID()) << 8)
                               | static_cast<std::uint64_t>(a_cat);
                auto& nxt = s_nextWalkLog[key];
                if (nxt.time_since_epoch().count() == 0 || a_now >= nxt) {
                    nxt = a_now + std::chrono::seconds(10);
                    // Include the follower's OWN state so a "3 eligible but
                    // nothing looted" is unambiguous -- an eligible corpse yields
                    // nothing if it simply does not hold the thing the winning
                    // rule wants (arrows, or the specific potion).
                    spdlog::info("[loot] {:08X} cat={} r={:.0f} leash={:.0f}: {} refs, {} lootable, {} eligible "
                                 "(dropped owned={} locked={} waiting={} farleash={} empty={}) | self arrows={} bolts={} "
                                 "potH={} potS={} potM={}",
                                 a_follower->GetFormID(), CatName(a_cat), kLootRadius, leash, dRefs, dLootable,
                                 (int)candidates.size(), dOwned, dLocked, dNotYet, dLeash, dEmpty,
                                 ArrowCount(a_follower), BoltCount(a_follower),
                                 CountPotions(a_follower, RE::ActorValue::kHealth),
                                 CountPotions(a_follower, RE::ActorValue::kStamina),
                                 CountPotions(a_follower, RE::ActorValue::kMagicka));
                }
            }

            // CLOSEST FIRST (marth): loot the nearest eligible source before the
            // farther ones, so a follower grabs what it is standing next to
            // rather than an arbitrary cell-walk order. Sort the collected
            // handles by distance to the follower; an unresolvable handle sorts
            // last. (Bounded at 16, so this is a tiny sort.)
            std::sort(candidates.begin(), candidates.end(),
                [&origin](const RE::ObjectRefHandle& a, const RE::ObjectRefHandle& b) {
                    auto pa = a.get(); auto pb = b.get();
                    const float da = pa ? origin.GetDistance(pa->GetPosition()) : 1e30f;
                    const float db = pb ? origin.GetDistance(pb->GetPosition()) : 1e30f;
                    return da < db;
                });

            // Act after the walk. Re-resolve each handle at act time (#2); stop at
            // the first successful action -- one loot action per tick (§4.8.3).
            for (auto& h : candidates) {
                auto ptr = h.get();
                auto* ref = ptr.get();
                if (!ref) continue;
                const RE::FormID rid = ref->GetFormID();

                // CLAIM MUTATION BAR (#22g / #22g-QL): never take from a source
                // the player is CONSIDERING right now -- a vanilla ContainerMenu
                // OR (QuickLoot list) the crosshair on it. Mutating it would yank
                // an item out from under the menu/HUD the player is reading.
                if (PlayerIsConsidering(rid)) continue;

                const float df = origin.GetDistance(ref->GetPosition());

                // ── EXCURSION MODE: the follower is already claimed (priority 60)
                // and driving a batch. The closest eligible candidate decides the
                // tick: within arm's reach -> grab it (a mutation); farther but
                // walkable -> RETARGET the excursion to it (movement, no release,
                // no turn-around). Do NOT start a new fill and do NOT release.
                if (a_mode == LootMode::kExcursion) {
                    if (df <= kArrivalDist && !LooseRef(ref)) {
                        if (LootHere(a_follower, ref, a_cat, a_potionWant)) return true;
                        continue;   // arm's-reach corpse was empty -- try the next
                    }
                    // COMMIT TO THE CURRENT LEG. RETARGET (below) resets the
                    // no-progress tracker (progressAt), so re-picking the closest
                    // ref EVERY tick meant an unreachable leg never accumulated the
                    // kNoProgress stall that sticky-blocklists it -- both followers
                    // churned unreachable corpses forever (v0.8.31: pathSpeed=0,
                    // navdist<<dist, legs switching every 2-4 s < the 7 s timer).
                    // While already walking to a still-valid, not-yet-stalled
                    // target, do NOT retarget: let the Walking-phase arrival/stall
                    // logic finish this leg, THEN the scan picks the next. Arm's-
                    // reach grabs (above) still fire; only the churn is stopped.
                    if (g_travel.phase == TravelPhase::Walking) {
                        auto  tptr = g_travel.target.get();
                        auto* cur  = tptr.get();
                        if (cur && !cur->IsDisabled() && !cur->IsMarkedForDeletion() &&
                            !TravelFailedRecently(cur->GetFormID(), a_now) &&
                            a_now - g_travel.progressAt <= kNoProgress)
                            return false;   // stay the course
                    }
                    // A LOOSE ref (route 2b) falls through to RETARGET even at
                    // arm's reach: the acquire runs at the driver's ARRIVAL
                    // (Activate dispatch), never as an in-place transfer here.
                    if (!Config::g_lootTravel.load())                              continue;
                    if (df > walkLimit)                                           continue;
                    if (TravelFailedRecently(rid, a_now))                          continue;
                    if (playerPos.GetDistance(ref->GetPosition())
                            <= Config::g_playerBubble.load())                      continue;
                    // OFF-NAVMESH GATE: if no navmesh is near the ref, the Travel
                    // package can't build a path and he'd freeze -- skip + blocklist
                    // (25s LRU, so the scan isn't re-run) BEFORE dispatch.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelStalled(rid, a_now);   // off-navmesh -> stall strike
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    if (Packages::LootTravelRetarget(a_follower, ref)) {
                        g_travel.target   = ref->GetHandle();
                        g_travel.cat      = a_cat;
                        g_travel.want     = a_potionWant;
                        g_travel.deadline = TravelDeadline(df, a_now);
                        g_travel.phase    = TravelPhase::Walking;
                        g_travel.lastPos    = origin;   // reset no-progress tracker
                        g_travel.progressAt = a_now;
                        return true;   // new leg -- the excursion continues at 60
                    }
                    continue;
                }

                // ── NORMAL MODE: START an excursion by walking to a far corpse,
                // or transfer one within arm's reach. A LOOSE ref (route 2b)
                // takes the excursion path REGARDLESS of distance -- there is no
                // in-place transfer for it, the acquire is the driver's arrival
                // Activate -- so it must claim/walk even from arm's reach.
                if ((df > kArrivalDist || LooseRef(ref)) && Config::g_lootTravel.load()) {
                    // Too far to WALK without abandoning the player -- leave it
                    // (following brings them closer later; also fairer to you).
                    if (df > walkLimit) continue;
                    // Skip a target a walk already failed to reach (no churn).
                    if (TravelFailedRecently(rid, a_now)) continue;
                    // CONVERGENCE YIELD: never walk to loot the player is right
                    // next to -- you win the race for the corpse you're heading to.
                    if (playerPos.GetDistance(ref->GetPosition()) <= Config::g_playerBubble.load())
                        continue;
                    // One traveller at a time (single loot alias).
                    if (g_travel.active) continue;
                    // OFF-NAVMESH GATE (see the excursion path): no mesh near the
                    // ref -> no path -> freeze. Skip + blocklist before dispatch.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelStalled(rid, a_now);   // off-navmesh -> stall strike
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    if (Packages::LootTravelFill(a_follower, ref)) {
                        g_travel.active    = true;
                        g_travel.follower  = a_follower->GetFormID();
                        g_travel.target    = ref->GetHandle();
                        g_travel.cat       = a_cat;
                        g_travel.want      = a_potionWant;
                        g_travel.deadline  = TravelDeadline(df, a_now);
                        g_travel.phase     = TravelPhase::Walking;
                        g_travel.startTime = a_now;   // excursion begins now
                        g_travel.lastPos    = origin;  // reset no-progress tracker
                        g_travel.progressAt = a_now;
                        return true;   // committed to the walk; transfer on arrival
                    }
                    // Travel UNAVAILABLE (off AE, records unresolved, quest not
                    // running) -- fall through to the arm's-reach transfer below
                    // rather than never looting this candidate (the SE fallback).
                }

                // Corpse/container within reach (or travel unavailable): transfer.
                if (LootHere(a_follower, ref, a_cat, a_potionWant)) return true;
            }
            // SKIP-REASON DIAGNOSTIC (v0.8.36). Nothing acted this tick even though
            // the scan collected candidates -- recompute WHY the CLOSEST eligible
            // body was passed over, so "loot efficiency is bad" is a measured fact,
            // not a guess (marth: Eric found the potion in front of him only after
            // several runs). Rate-limited per (follower,cat) 10 s, mirroring the scan
            // log. The committed-to-leg early-out (return false while Walking) never
            // reaches here, so this fires only on a genuine no-op tick.
            if (!candidates.empty()) {
                static std::unordered_map<std::uint64_t, Clock::time_point> s_nextSkipLog;
                const auto key = (static_cast<std::uint64_t>(a_follower->GetFormID()) << 8)
                               | static_cast<std::uint64_t>(a_cat);
                auto& nxt = s_nextSkipLog[key];
                if (nxt.time_since_epoch().count() == 0 || a_now >= nxt) {
                    nxt = a_now + std::chrono::seconds(10);
                    auto p0 = candidates.front().get();
                    if (auto* r0 = p0.get()) {
                        const auto  r0id = r0->GetFormID();
                        const float dF   = origin.GetDistance(r0->GetPosition());
                        const float dP   = playerPos.GetDistance(r0->GetPosition());
                        spdlog::info("[lootskip] {:08X} cat={} closest={:08X} distF={:.0f} distPlayer={:.0f} "
                                     "walkLimit={:.0f} | considering={} bubble={}(<{:.0f}) blocklist={} "
                                     "aliasBusy={} navdist={:.0f}(gate {:.0f})",
                                     a_follower->GetFormID(), CatName(a_cat), r0id, dF, dP, walkLimit,
                                     PlayerIsConsidering(r0id),
                                     dP <= Config::g_playerBubble.load(), Config::g_playerBubble.load(),
                                     TravelFailedRecently(r0id, a_now),
                                     (g_travel.active && g_travel.follower != a_follower->GetFormID()),
                                     NavmeshReach(a_follower, r0), Config::g_navmeshGate.load());
                    }
                }
            }
            // Excursion Hold decision needs to know if loot is still WAITING on
            // the player's dibs (vs genuinely nothing left).
            if (a_mode == LootMode::kExcursion && dNotYet > 0) g_scanSawWaiting = true;
            return false;
        }

        // ARM'S-REACH FULL STRIP (marth: "he takes the body's gold but leaves its
        // Iron Arrow (9)"). On arrival the follower is standing ON the corpse, so
        // take EVERYTHING his gambits currently want in this ONE visit -- not just
        // the category the trip was for. Without this, a gold trip loots the gold,
        // marks the corpse DONE (blocklisted ~25s), then walks back to the player,
        // stranding the arrows/potions in a body now past arm's reach AND on the
        // blocklist -- exactly the 340u/382u arrow bodies in the deck test.
        //
        // Transfers straight from the KNOWN corpse (no world scan, no retarget).
        // The Evaluate walk means a category is taken only if its gambit CONDITION
        // is true right now (out of arrows, low on potions; gold's Always) -- #28,
        // never loot a thing the player's rules don't ask for. Wait ends the sweep
        // (the deliberate STOP gambit). Returns true if anything moved.
        bool StripCorpse(RE::Actor* a_follower, const FollowerState& a_state,
                         RE::TESObjectREFR* a_corpse, Clock::time_point /*a_now*/) {
            bool moved = false;
            for (int start = 0; ; ) {
                const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
                if (choice.ruleIndex < 0) break;
                const auto& op = choice.actionOpcode;
                Category cat = Category::Gold; RE::ActorValue want = RE::ActorValue::kNone;
                bool isLoot = true;
                if      (op == Vocab::kActLootArrows)         cat = Category::Arrows;
                else if (op == Vocab::kActLootBolts)          cat = Category::Bolts;
                else if (op == Vocab::kActLootPotions)      { cat = Category::Potions; }
                else if (op == Vocab::kActLootHealthPotion) { cat = Category::Potions; want = RE::ActorValue::kHealth;  }
                else if (op == Vocab::kActLootStaminaPotion){ cat = Category::Potions; want = RE::ActorValue::kStamina; }
                else if (op == Vocab::kActLootMagickaPotion){ cat = Category::Potions; want = RE::ActorValue::kMagicka; }
                else if (op == Vocab::kActLootEquipment)      cat = Category::Equipment;
                else if (op == Vocab::kActLootGold)           cat = Category::Gold;
                else if (op == Vocab::kActLootJewelry)        cat = Category::Jewelry;
                else if (op == Vocab::kActLootSoulGems)       cat = Category::SoulGems;
                else if (op == Vocab::kActLootLockpicks)      cat = Category::Lockpicks;
                else if (op == Vocab::kActWait) break;   // user's STOP gambit ends the sweep
                else isLoot = false;
                if (isLoot && LootHere(a_follower, a_corpse, cat, want)) moved = true;
                start = choice.ruleIndex + 1;
            }
            return moved;
        }

        // A LEG BOUNDARY on an active excursion: run ONLY the follower's LOOT
        // gambits, in excursion mode, to grab an arm's-reach corpse or retarget
        // to the next walkable one. LOOT-ONLY (no drink/wait) so a leg boundary
        // never fires a second real action in the tick (the arrival transfer was
        // this tick's mutation; a retarget is movement). Returns true if it acted
        // (grabbed or retargeted). Sets g_scanSawWaiting via LootNearby.
        bool RunExcursionScan(RE::Actor* a_follower, const FollowerState& a_state,
                              Clock::time_point a_now) {
            g_scanSawWaiting = false;
            for (int start = 0; ; ) {
                const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
                if (choice.ruleIndex < 0) break;
                const auto& op = choice.actionOpcode;
                bool acted = false, isLoot = true;
                if      (op == Vocab::kActLootArrows)         acted = LootNearby(a_follower, Category::Arrows,  a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootBolts)          acted = LootNearby(a_follower, Category::Bolts,   a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootPotions)        acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootHealthPotion)   acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kHealth,  LootMode::kExcursion);
                else if (op == Vocab::kActLootStaminaPotion)  acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kStamina, LootMode::kExcursion);
                else if (op == Vocab::kActLootMagickaPotion)  acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kMagicka, LootMode::kExcursion);
                else if (op == Vocab::kActLootEquipment)      acted = LootNearby(a_follower, Category::Equipment, a_now, RE::ActorValue::kNone,  LootMode::kExcursion);
                else if (op == Vocab::kActLootGold)           acted = LootNearby(a_follower, Category::Gold,    a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootJewelry)        acted = LootNearby(a_follower, Category::Jewelry, a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootSoulGems)       acted = LootNearby(a_follower, Category::SoulGems, a_now, RE::ActorValue::kNone,   LootMode::kExcursion);
                else if (op == Vocab::kActLootLockpicks)      acted = LootNearby(a_follower, Category::Lockpicks, a_now, RE::ActorValue::kNone,  LootMode::kExcursion);
                else if (op == Vocab::kActWait) break;   // Wait is the user's deliberate STOP gambit
                                                         // (#3.3): end the batch -- returning false with
                                                         // nothing "waiting" makes Holding release.
                else isLoot = false;
                if (isLoot && acted) return true;
                start = choice.ruleIndex + 1;   // skip non-loot / no-op, try next
            }
            return false;
        }

        // ── ECONOMY PROBE (#21, temp) ───────────────────────────────────────
        // LOG-ONLY dry run of follower<->vendor trading: ZERO transactions, ZERO
        // mutations -- every call below is a read. Validates, BEFORE any gold
        // moves: (a) vendor-faction resolution off a living actor, (b) the
        // merchant-chest read (incl. an UNLOADED chest -- inventory lives in
        // extra data, not 3D), (c) where the vendor's gold actually is (chest vs
        // pocket), and (d) the VEND keyword filter on the follower's sellables.
        //
        // Faction resolution is Actor::VisitFactions, NOT GetVendorFaction --
        // that one has an NG bug and MUTATES engine state (caches the resolved
        // faction back onto the process), which breaks the zero-mutation
        // contract of a probe. First faction with IsVendor() && OffersServices()
        // wins, matching the engine's own barter pick closely enough to verify.
        //
        // MAIN THREAD ONLY (§0.37). This reads a LIVE merchant's inventory,
        // which the main thread mutates as it manages the vendor -- read it
        // from the job worker and a form cast comes back null and is
        // dereferenced (CTD on Ulfberth/Warmaidens, crash-2026-07-31-13-12-39).
        // The caller Posts here via MainThread; the statics below are therefore
        // main-thread-only and unlocked, same discipline as the rest of MFO.
        // Takes the logistics GAMBITS, not the FollowerState: the caller hands
        // over a by-value copy because a_state on the worker is a reference
        // into g_followers, which the main thread itself edits via the board.

        // Does the vendor's VEND list trade this item? The list holds KEYWORDS
        // (VendorItemWeapon et al.); notBuySell inverts it into an EXCLUSION
        // list (the engine's own semantics for that flag).
        bool VendorTrades(RE::BGSListForm* a_vend, bool a_notBuySell,
                          RE::BGSKeywordForm* a_kwf) {
            if (!a_vend || !a_kwf) return false;
            bool match = false;
            for (auto* f : a_vend->forms) {
                auto* kw = f ? f->As<RE::BGSKeyword>() : nullptr;
                if (kw && a_kwf->HasKeyword(kw)) { match = true; break; }
            }
            return a_notBuySell ? !match : match;
        }

        // Econ scan cadence clocks. Namespace scope (not function-local statics) so
        // ClearTransientState wipes them on revert (Fable audit #7): FF-dynamic
        // follower IDs get reused, so a stale 15/20/60 s cooldown must not carry
        // into the next save, and the pair map must not grow unbounded across one.
        std::unordered_map<RE::FormID,   Clock::time_point> g_econScan;    // per-follower 15 s
        std::unordered_map<RE::FormID,   Clock::time_point> g_econTrade;   // per-follower 20 s
        std::unordered_map<std::uint64_t, Clock::time_point> g_econPair;   // per-(follower,vendor) 60 s

        void EconomyProbe(RE::Actor* a_follower, const std::vector<Gambit>& a_logistics,
                          Clock::time_point a_now) {
            const auto fid = a_follower->GetFormID();

            // Per-follower SCAN cooldown (15 s) -- the cell walk itself is the
            // cost being limited here, same pattern as the other diagnostics.
            auto& sn = g_econScan[fid];
            if (sn.time_since_epoch().count() != 0 && a_now < sn) return;
            sn = a_now + std::chrono::seconds(15);

            // Same crash4-safe walk as LootNearby: the follower's OWN attached
            // cell, never TES::ForEachReferenceInRange (§0.30).
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached()) return;
            const auto  origin      = a_follower->GetPosition();
            const float kLootRadius = Config::g_lootRadius.load();

            // COLLECT-THEN-ACT (#2): handles only inside the walk; the inventory
            // reads happen after it. LIVING actors only -- corpses are loot,
            // not merchants.
            constexpr size_t kMaxVendorCands = 16;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            std::vector<RE::ActorHandle> living;
            living.reserve(kMaxVendorCands);
            cell->ForEachReferenceInRange(origin, kLootRadius,
                [&](RE::TESObjectREFR& a_ref) {
                    if (living.size() >= kMaxVendorCands) return RE::BSContainer::ForEachResult::kStop;
                    auto* actor = a_ref.As<RE::Actor>();
                    if (!actor || actor == a_follower || actor == pc || actor->IsDead() ||
                        actor->IsDisabled() || actor->IsMarkedForDeletion())
                        return RE::BSContainer::ForEachResult::kContinue;
                    // A fellow FOLLOWER is never a merchant. Auri carries a stray
                    // vendor faction (IsVendor && OffersServices both pass), but her
                    // "merchant container" is malformed -- reading it null-derefs a
                    // form cast and CTDs (crash 2026-07-31, thread on the job worker).
                    // Teammates trade through the player, not each other.
                    if (actor->IsPlayerTeammate())
                        return RE::BSContainer::ForEachResult::kContinue;
                    living.push_back(actor->GetHandle());
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            // PHASE 4 anti-thrash + don't-trade-mid-loot.
            // (a) A follower already walking to loot must not break off to trade --
            //     loot and trade never overlap; finish the excursion first.
            if (g_travel.active && g_travel.follower == fid) return;
            // (b) Per-follower TRADE cooldown: at most one trade dispatch per window,
            //     so a purchase SETTLES (the arrow/potion count updates) before the
            //     next scan re-evaluates the need. Without it a follower near two
            //     vendors traded with BOTH in the same scan and over-bought (field:
            //     Erik +17 @ Ysolda AND +22 @ Adrianne, same second).
            if (auto& tn = g_econTrade[fid]; tn.time_since_epoch().count() != 0 && a_now < tn) return;

            for (auto& h : living) {
                auto  ptr    = h.get();
                auto* vendor = ptr.get();
                if (!vendor) continue;

                // First vendor faction that actually offers services. rank < 0
                // is a runtime REMOVAL (ExtraFactionChanges) -- skip it.
                RE::TESFaction* fac = nullptr;
                vendor->VisitFactions([&](RE::TESFaction* a_fac, std::int8_t a_rank) {
                    if (!a_fac || a_rank < 0) return false;
                    if (a_fac->IsVendor() && a_fac->OffersServices()) { fac = a_fac; return true; }
                    return false;
                });
                if (!fac) continue;

                // Per-(follower,vendor) log cooldown (60 s).
                const auto pairKey = (static_cast<std::uint64_t>(fid) << 32) | vendor->GetFormID();
                auto& pn = g_econPair[pairKey];
                if (pn.time_since_epoch().count() != 0 && a_now < pn) continue;
                pn = a_now + std::chrono::seconds(60);

                const auto& vv   = fac->vendorData.vendorValues;
                auto*       vend = fac->vendorData.vendorSellBuyList;

                // Only a chest that is a real container REFR (has a base
                // TESContainer) is even a candidate -- a malformed merchant link
                // (fake vendor faction) has none. But we NO LONGER read its
                // inventory natively: GetInventory on an unpopulated merchant chest
                // CTDs on any thread (§0.37). The chest read moves to Papyrus.
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;
                // SELL list: the follower's OWN unworn weapons/armour (jewellery is
                // ARMO, so IsJewelryPiece rides along) that clear the never-loot
                // catalog and the vendor's VEND filter -- all native-SAFE reads. Also
                // count the PURSE in the same pass: Actor::GetGoldAmount() CTDs here
                // (Actor.cpp:445, null-deref on the InventoryChanges the worker tick
                // may be mutating -- crash 2026-08-01), but the GetInventory snapshot
                // is safe, so sum Gold001 (0x0000000F) straight from it.
                std::vector<TradeBridge::SellRow> sell;
                int purse = 0;
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (obj->GetFormID() == 0x0000000F) { purse += static_cast<int>(data.first); continue; }
                    auto* weap = obj->As<RE::TESObjectWEAP>();
                    auto* armo = obj->As<RE::TESObjectARMO>();
                    if (!weap && !armo) continue;
                    RE::BGSKeywordForm* kwf = weap
                        ? static_cast<RE::BGSKeywordForm*>(weap)
                        : static_cast<RE::BGSKeywordForm*>(armo);
                    auto* entry = data.second.get();
                    if (entry && entry->IsWorn())                continue;   // never sell worn gear
                    if (Catalog::IsExcluded(obj->GetFormID()))   continue;
                    if (!VendorTrades(vend, vv.notBuySell, kwf)) continue;
                    sell.push_back(TradeBridge::SellRow{
                        obj, static_cast<std::int32_t>(data.first),
                        entry ? entry->GetValue() : 0,
                        armo && IsJewelryPiece(armo) });
                }
                // Highest-value first: the vendor's barter gold is limited (field log:
                // sale total often > vendor gold), so sell the most valuable junk first
                // to convert the most worth per visit. Papyrus caps at the chest's gold.
                std::sort(sell.begin(), sell.end(),
                          [](const auto& a, const auto& b) { return a.value > b.value; });

                // BUY NEEDS: each supply gambit BELOW its threshold -> the category
                // + how many MORE to acquire. Papyrus enumerates the vendor's ACTUAL
                // stock and TradeBridge::PlanBuy matches it to these (best affordable
                // first, capped at quota + purse). Native can't pre-name the stock --
                // the field log proved guessing gives stock=0 -- so it names the
                // NEED and lets the enumeration find the match.
                std::vector<TradeBridge::NeedCat> needs;
                auto addNeed = [&](TradeBridge::NeedCat::Kind a_kind, int have, int want) {
                    if (have < want) needs.push_back({ static_cast<std::int32_t>(a_kind),
                                                       static_cast<std::int32_t>(want - have) });
                };
                for (const auto& g : a_logistics) {
                    if (!g.enabled) continue;
                    const auto& c    = g.conditionOpcode;
                    const int   want = static_cast<int>(g.conditionParam);
                    if      (c == Vocab::kCondSelfLowHealthPotion)  addNeed(TradeBridge::NeedCat::kPotHealth,  CountPotions(a_follower, RE::ActorValue::kHealth),  want);
                    else if (c == Vocab::kCondSelfLowStaminaPotion) addNeed(TradeBridge::NeedCat::kPotStamina, CountPotions(a_follower, RE::ActorValue::kStamina), want);
                    else if (c == Vocab::kCondSelfLowMagickaPotion) addNeed(TradeBridge::NeedCat::kPotMagicka, CountPotions(a_follower, RE::ActorValue::kMagicka), want);
                    else if (c == Vocab::kCondSelfOutOfArrows)      addNeed(TradeBridge::NeedCat::kArrows,     ArrowCount(a_follower),                             want);
                    else if (c == Vocab::kCondSelfOutOfBolts)       addNeed(TradeBridge::NeedCat::kBolts,      BoltCount(a_follower),                              want);
                }

                // Only burn the cooldown + stop scanning if a trade ACTUALLY
                // dispatched (Fable audit #8): a chest already busy with another
                // follower's order, or the bridge being down, must not cost this
                // follower its 20 s window -- try the next vendor / next scan.
                if (TradeBridge::VendorTrade(a_follower, vendor, chest,
                                             std::move(sell), std::move(needs), purse)) {
                    g_econTrade[fid] = a_now + std::chrono::seconds(20);
                    break;
                }
            }   // for (living vendors)
        }

        // ── the player-looted waiver sink (#22h) ────────────────────────────
        class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
        public:
            static ContainerSink* GetSingleton() { static ContainerSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                                  RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                // Logistics off -> the waiver map is never read, so do no work.
                if (!Config::g_logistics.load()) return RE::BSEventNotifyControl::kContinue;

                // DIRECTION FILTER IS MANDATORY (#22h): only items ENTERING the
                // player count as a "take". Without it the sink re-triggers on
                // its own removal, which is MAO's infinite-credit loop. This also
                // encodes "the waiver keys on TAKING, never on looking" -- a
                // QuickLoot glance opens no menu and moves no item, so it fires
                // nothing here.
                if (a_event->newContainer != PlayerID()) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID srcID = a_event->oldContainer;
                if (srcID == 0) return RE::BSEventNotifyControl::kContinue;   // spawned into player, no source

                // Sinks QUEUE; they never touch a main-thread map inline (#1/#4).
                // The timer RESETS on every take, so multiple QuickLoot takes push
                // the follower's window out to the LAST one.
                SKSE::GetTaskInterface()->AddTask([srcID]() {
                    g_playerLooted[srcID] = Clock::now();
                    EvictOldest(g_playerLooted);
                    // R1: the player's take is from a DIFFERENT source than their
                    // last -> release the previous one's claim (they finished
                    // there). Runs on the same worker queue as the loot walk, so
                    // touching g_claim here is serial with it (no race, §0.30).
                    if (g_lastLootSource != 0 && g_lastLootSource != srcID) {
                        if (auto it = g_claim.find(g_lastLootSource); it != g_claim.end())
                            it->second.rejected = true;
                    }
                    g_lastLootSource = srcID;
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }

    // ── public: pure reads used by the evaluator ────────────────────────────

    RE::ActorValue PotionRestores(RE::AlchemyItem* a_potion) {
        if (!a_potion || a_potion->IsPoison()) return RE::ActorValue::kNone;
        // FOOD is an AlchemyItem with a Restore-Health/Stamina effect, so the
        // archetype gate below would pass it. §4.8.2 is potions ONLY -- exclude
        // food or the follower vacuums city food barrels and counts apples as
        // health potions.
        if (a_potion->IsFood()) return RE::ActorValue::kNone;

        // CATALOG FIRST. The MFO.Synthesis patcher already classified this potion
        // from its real effect record (beneficial, non-recover, on a resource) --
        // Requiem-proof, where the archetype heuristic below misreads a reworked
        // potion as kNone (marth: "5 health potions, won't drink"). A catalog hit
        // is authoritative; a miss falls through to the heuristic so MFO still
        // works with no patcher run.
        if (auto av = Catalog::PotionRestores(a_potion->GetFormID()); av != RE::ActorValue::kNone)
            return av;

        // The COSTLIEST effect is the potion's dominant purpose. Classify by
        // ARCHETYPE, not by name and not by the effect's AV alone: only
        // kValueModifier / kDualValueModifier are real resource restores
        // (fortify is kPeakValueModifier, cure is its own archetype), which is
        // the portable classifier from MRO's Requiem note (DESIGN §3.3).
        const auto* eff  = a_potion->GetCostliestEffectItem();
        auto*       mgef = eff ? eff->baseEffect : nullptr;
        if (!mgef) return RE::ActorValue::kNone;

        const auto arch = mgef->data.archetype;
        if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
            arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) {
            return RE::ActorValue::kNone;
        }
        return mgef->data.primaryAV;
    }

    int CountPotions(RE::Actor* a_follower, RE::ActorValue a_which) {
        if (!a_follower) return 0;
        int n = 0;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* alc = obj->As<RE::AlchemyItem>();
            if (alc && PotionRestores(alc) == a_which) n += data.first;
        }
        return n;
    }

    // ARROWS the follower carries -- no bow gate; the "arrows below N" gambit
    // decides whether that matters. Bolts are a separate count/gambit.
    int ArrowCount(RE::Actor* a_follower) { return AmmoCount(a_follower, false); }
    int BoltCount(RE::Actor* a_follower)  { return AmmoCount(a_follower, true);  }

    // ── public: actuation ───────────────────────────────────────────────────

    // Public forwarder so Actuation (the combat dispatcher) drinks through the
    // exact same cooldown-gated path as the logistics table. NOT gated on
    // bLogistics: that flag governs OUT-OF-COMBAT looting/drinking; an in-combat
    // drink is a combat gambit, and the gate is that the player assigned it.
    bool DrinkPotion(RE::Actor* a_follower, RE::ActorValue a_which) {
        if (!a_follower) return false;
        return DrinkBest(a_follower, a_which);
    }

    // HEAL: a follower wielding an EXCLUDED weapon renders it INVISIBLE -- MFO
    // wrongly looted a NON-PLAYABLE creature weapon (field: a Dwarven Sphere
    // Crossbow off an automaton corpse) before the catalog excluded that class. It
    // still FIRES, so combat looks fine, but there's no humanoid mesh. Swap it for
    // the follower's best carried PLAYABLE weapon (or just take it off if they carry
    // none). Reuses Catalog::IsExcluded, which now flags nonplayable weapons; a
    // no-op once healed, so it's safe to poll. Requires the regenerated catalog.
    void HealExcludedWeapon(RE::Actor* a_follower) {
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!em) return;
        for (int hand = 0; hand < 2; ++hand) {
            auto* eq  = a_follower->GetEquippedObject(hand == 1);   // false=right, true=left
            auto* bad = eq ? eq->As<RE::TESObjectWEAP>() : nullptr;
            if (!bad || (!Catalog::IsExcluded(bad->GetFormID()) && !IsCreatureWeapon(bad))) continue;
            RE::TESBoundObject* best    = nullptr;
            std::uint16_t       bestDmg = 0;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || Catalog::IsExcluded(obj->GetFormID()) || IsCreatureWeapon(w)) continue;
                if (WeaponClassOf(w->GetWeaponType()) == WepClass::Other) continue;  // no staff/creature
                if (w->GetAttackDamage() > bestDmg) { bestDmg = w->GetAttackDamage(); best = obj; }
            }
            if (best) {
                em->EquipObject(a_follower, best);   // auto-unequips the excluded one
            } else {
                em->UnequipObject(a_follower, bad);  // nothing real carried -- just take it off
            }
            // CRITICAL: un-equipping alone leaves the creature weapon in the pack, and
            // the engine re-wields it as "best weapon" within seconds -- the poll then
            // just churns forever (field-caught: same 'Dwarven Sphere Crossbow' re-healed
            // 3 min apart). Evict it to the player so nothing can re-select it. Count the
            // copies so a stack leaves entirely.
            std::int32_t haveBad = 0;
            for (auto& [obj, data] : a_follower->GetInventory())
                if (obj == bad) { haveBad = data.first; break; }
            if (haveBad > 0) {
                if (auto* player = RE::PlayerCharacter::GetSingleton())
                    a_follower->RemoveItem(bad, haveBad, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                           nullptr, player);
            }
            spdlog::info("[heal] {:08X} excluded '{}' -> {}, evicted {}x to player",
                         a_follower->GetFormID(),
                         bad->GetName() ? bad->GetName() : "?",
                         best ? (best->GetName() ? best->GetName() : "?") : "(bare hands)",
                         haveBad);
            return;   // one hand per call
        }
    }

    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower) return;
        g_svc = &a_state;   // loot code reads the gambit table through this (worker-sequential)


        const auto id  = a_follower->GetFormID();
        const auto now = Clock::now();

        // Undo a wrongly-looted non-playable weapon (invisible on a follower) before
        // anything else this tick -- cheap + rate-limited (5 s), a no-op once healed.
        {
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextHeal;
            auto& hn = s_nextHeal[id];
            if (hn.time_since_epoch().count() == 0 || now >= hn) {
                hn = now + std::chrono::seconds(5);
                HealExcludedWeapon(a_follower);
            }
        }

        // GLOBAL travel-intent BACKSTOP, keyed to NO follower in particular, and
        // run BEFORE the logistics early-return on purpose. The per-follower
        // arrival branch below only runs when the TRAVELLER is serviced out of
        // combat -- but a traveller who died, was dismissed, dropped off the
        // roster, or got pulled down the combat branch is never serviced there,
        // so its intent (and the loot quest's raised priority) would stick all
        // session. This fires for EVERY follower, and also when the subsystem is
        // toggled OFF mid-travel -- otherwise turning bLogistics/bLootTravel off
        // would strand a traveller at priority 60. (An in-session load does NOT
        // revert the raised priority -- form data keeps the runtime value -- so
        // ReleaseAll's kPreLoadGame write is the real reset, not this; this is
        // the LIVE release. See ReleaseAll.)
        // Keyed to the whole-EXCURSION cap, not the per-leg deadline: during a
        // Hold the leg deadline is stale and would wrongly fire this. The cap
        // catches an excursion whose traveller is no longer being serviced.
        // (Combat yield is NOT here -- ServiceFollower is skipped for in-combat
        // followers, so this backstop never runs for the one who matters. The
        // yield lives in the Scheduler's combat branch, ReleaseTravelOnCombat.)
        const bool off = !Config::g_logistics.load() || !Config::g_lootTravel.load();
        const bool capped = g_travel.active &&
            now > g_travel.startTime + std::chrono::seconds(
                      static_cast<int>(Config::g_excursionMax.load()));
        if (g_travel.active && (capped || off)) {
            // On a cap hit blacklist the current target so it isn't re-picked
            // immediately. NOT on toggle-off (that corpse never "failed").
            if (!off) {
                if (auto tp = g_travel.target.get()) MarkTravelFailed(tp->GetFormID(), now);
            }
            Packages::LootTravelClear(off ? "subsystem off" : "excursion cap");
            g_travel = TravelIntent{};
        }

        if (!Config::g_logistics.load()) return;   // whole subsystem off by default (#45)

        // CADENCE GATE (~1 s). Cheap early-out on the frames between logistics
        // ticks -- the Scheduler calls this every time it services the follower
        // out of combat (up to ~7.5 Hz), but logistics only acts at the idle rate.
        auto& due = g_nextTick[id];
        if (due.time_since_epoch().count() != 0 && now < due) return;
        due = now + kLogisticsInterval;

        // ── BATCH EXCURSION driver. While THIS follower is on a loot excursion
        // (claimed at priority 60), drive it: walk to the current target, grab it
        // on arrival, then seek the NEXT target and retarget WITHOUT releasing --
        // he hoovers a batch instead of returning to the player after each corpse.
        // Release only on combat, the excursion cap, leaving the leash, or the
        // batch running dry (after a short dibs linger).
        if (g_travel.active && g_travel.follower == id) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            // HARD interrupts -> END the excursion.
            const bool overCap = now > g_travel.startTime + std::chrono::seconds(
                                          static_cast<int>(Config::g_excursionMax.load()));
            // HYSTERESIS on the leash (×1.15): a corpse near the leash edge puts
            // the follower in a thin band (>leash from player, still walking to an
            // in-leash target) where a bare check would release then instantly
            // re-fill to the same corpse and oscillate. The margin ends the batch
            // only once he is clearly past the leash.
            const bool outOfLeash = pc &&
                a_follower->GetPosition().GetDistance(pc->GetPosition())
                    > Confidence::LeashRadius(a_follower) * 1.15f;
            const bool inHome = !Config::g_lootInPlayerHomes.load() && InPlayerHome();
            if (a_follower->IsInCombat() || overCap || outOfLeash || inHome) {
                Packages::LootTravelClear(a_follower->IsInCombat() ? "combat"
                                          : (overCap ? "excursion cap"
                                          : (inHome ? "player home" : "left leash")),
                                          a_follower);
                g_travel = TravelIntent{};
                // fall through to a normal eval this tick (combat table / follow).
            } else if (g_travel.acquirePending) {
                // ── ACQUIRE READBACK (route 2b probe). One tick after the
                // Activate dispatch, observe what the VM call actually did --
                // dispatch is asynchronous (Papyrus.h), so the previous tick's
                // call has had its frame(s) by now. Ref gone / inventory up =
                // the engine took it natively; ref standing with a flat count =
                // the activation was a no-op.
                g_travel.acquirePending = false;
                auto aptr = g_travel.target.get();
                auto* aref = aptr.get();
                const bool refGone = !aref || aref->IsDeleted() || aref->IsDisabled();
                std::int32_t post = 0;
                for (auto& [obj, n] : a_follower->GetInventoryCounts())
                    if (obj && obj->GetFormID() == g_travel.acquireBase) { post = n; break; }
                const std::int32_t delta = post - g_travel.acquirePre;
                if (refGone || delta != 0)
                    spdlog::info("[acquire] {:08X}: TOOK {:08X} -- ref {}, inv {:+}",
                                 id, g_travel.acquireRefID,
                                 refGone ? "gone" : "persists", delta);
                else
                    spdlog::info("[acquire] {:08X}: ACTIVATE NO-OP -- ref persists, inv unchanged",
                                 id);
                // Either way this leg is DONE: blocklist the ref (a persisting
                // no-op must not be re-picked every tick) and continue the batch.
                MarkTravelFailed(g_travel.acquireRefID, now);
                g_travel.phase = TravelPhase::Holding;
                g_travel.lingerUntil = now + std::chrono::seconds(
                                          static_cast<int>(Config::g_batchLinger.load()));
                return;
            } else if (g_travel.phase == TravelPhase::Walking) {
                auto tptr  = g_travel.target.get();
                auto* tref = tptr.get();
                const float dist = tref ?
                    a_follower->GetPosition().GetDistance(tref->GetPosition()) : 1e9f;
                // DIAGNOSTIC (v0.8.6): is MFO's travel package driving him, and is
                // he closing on the target? onTravelPkg=true + shrinking dist =
                // working; onTravelPkg=true + flat dist = UNREACHABLE (no path).
                // WALK diagnostic -- RATE-LIMITED to ~once per 4 s per follower
                // (was every ~1 s tick: ~60 lines a session). The claim + gate are
                // proven now; this is kept only as a debugging surface for a future
                // freeze, so it need not be dense. dist shrinking is the real
                // signal; pathSpeed is noisy (reads 0 mid-walk) and kept only as a
                // hint. navdist calibrates the gate.
                if (tref) {
                    static std::unordered_map<RE::FormID, Clock::time_point> s_nextWalkDiag;
                    auto& nxt = s_nextWalkDiag[id];
                    if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                        nxt = now + std::chrono::seconds(4);
                        auto* cur = a_follower->GetCurrentPackage();
                        float pathSpeed = -1.0f;
                        if (auto* proc = a_follower->GetActorRuntimeData().currentProcess)
                            if (auto* high = proc->high)
                                pathSpeed = high->pathingCurrentMovementSpeed.Length();
                        spdlog::info("[loot] {:08X} WALK->{:08X}: onTravelPkg={} curPkg={:08X} prio={} "
                                     "dist={:.0f} pathSpeed={:.1f} navdist={:.0f}",
                                     id, tref->GetFormID(),
                                     cur == Forms::g_travelPackage,
                                     cur ? cur->GetFormID() : 0u,
                                     Forms::g_lootQuest ? static_cast<int>(Forms::g_lootQuest->data.priority) : -1,
                                     dist, pathSpeed, NavmeshReach(a_follower, tref));
                    }
                }
                // Track real movement for the no-progress check below (update on
                // any >kMoveEps world move since the last note).
                const RE::NiPoint3 fpos = a_follower->GetPosition();
                if (tref && fpos.GetDistance(g_travel.lastPos) > kMoveEps) {
                    g_travel.lastPos    = fpos;
                    g_travel.progressAt = now;
                }
                const bool gone = !tref || tref->IsDisabled() || tref->IsMarkedForDeletion();

                // ARRIVAL is checked BEFORE the stall/deadline giveup: a follower
                // standing ON the corpse has ARRIVED, not "stalled walking" -- and
                // the considering/sneak HOLD below legitimately keeps him stationary
                // (he's waiting out YOUR QuickLoot). Letting the no-progress timer
                // fire during that hold would falsely blacklist a corpse he reached
                // and is politely waiting on (audit).
                if (!gone && dist <= kArrivalDist) {
                    // REACHED it -> this body is provably reachable: clear any stall
                    // strike so a merely-transient earlier block (boxed in by an actor,
                    // a door) never accumulates toward the sticky verdict. Only bodies
                    // he can NEVER close on keep striking -> those go sticky (above).
                    g_stallStrikes.erase(tref->GetFormID());
                    // MUTATION BAR (#22g / #22g-QL) + sneak courtesy hold.
                    if (PlayerIsConsidering(tref->GetFormID()) || (pc && pc->IsSneaking()))
                        return;   // arrived, holding under the bar -- retry next tick
                    // ── ACQUIRE PROBE (route 2b): a LOOSE ref has no inventory
                    // to transfer -- StripCorpse/LootHere would no-op on it.
                    // Dispatch ObjectReference.Activate(follower) through the VM
                    // instead: the ENGINE runs the pickup on its own scheduling.
                    // NO PickUpObject and NO SKSE AddTask -- both land on the
                    // §0.30 worker, the crash4 class. Readback next tick.
                    if (LooseRef(tref)) {
                        auto* base = tref->GetBaseObject();
                        const char* iname = tref->GetDisplayFullName();
                        const auto icount = tref->extraList.GetCount();
                        std::int32_t pre = 0;
                        for (auto& [obj, n] : a_follower->GetInventoryCounts())
                            if (obj && base && obj->GetFormID() == base->GetFormID()) { pre = n; break; }
                        if (Papyrus::DispatchActivate(tref, a_follower)) {
                            spdlog::info("[acquire] {:08X}: ACTIVATE dispatched for ref {:08X} ('{}' x{})",
                                         id, tref->GetFormID(), iname ? iname : "?", icount);
                            g_travel.acquirePending = true;
                            g_travel.acquireRefID   = tref->GetFormID();
                            g_travel.acquireBase    = base ? base->GetFormID() : 0;
                            g_travel.acquirePre     = pre;
                            return;   // the dispatch IS this tick's action; readback next tick
                        }
                        // VM unreachable / no handle -- end the leg, batch continues.
                        spdlog::info("[acquire] {:08X}: ACTIVATE dispatch FAILED for ref {:08X}",
                                     id, tref->GetFormID());
                        MarkTravelFailed(tref->GetFormID(), now);
                        g_travel.phase = TravelPhase::Holding;
                        g_travel.lingerUntil = now + std::chrono::seconds(
                                                  static_cast<int>(Config::g_batchLinger.load()));
                        return;
                    }
                    // Take EVERYTHING his gambits want in this one visit, not just
                    // the category the trip was for -- else gold trips strand the
                    // arrows (marth's 340u/382u bodies). Only THEN is the corpse
                    // genuinely DONE and safe to blocklist.
                    const bool moved = StripCorpse(a_follower, a_state, tref, now);
                    MarkTravelFailed(tref->GetFormID(), now);   // fully stripped -> DONE
                    g_travel.phase = TravelPhase::Holding;
                    g_travel.lingerUntil = now + std::chrono::seconds(
                                              static_cast<int>(Config::g_batchLinger.load()));
                    spdlog::info("[loot] {:08X}: arrived -- {} (batch continues)", id,
                                 moved ? "looted" : "nothing to take");
                    return;   // the transfer IS this tick's action; seek next tick
                }

                // NOT arrived: leg fails on vanished target, no-progress (no path),
                // or the leg deadline. Blacklist it and seek another leg THIS tick.
                const bool stalled = tref && now - g_travel.progressAt > kNoProgress;
                if (gone || stalled || now > g_travel.deadline) {
                    if (tref) {
                        // A stall (zero progress) is a reachability verdict -> strike
                        // toward sticky; a vanished target or a plain deadline is not.
                        if (stalled) MarkTravelStalled(tref->GetFormID(), now);
                        else         MarkTravelFailed(tref->GetFormID(), now);
                    }
                    if (stalled && tref)
                        spdlog::info("[loot] {:08X} unreachable {:08X} (no progress, dist={:.0f}) -- skipping",
                                     id, tref->GetFormID(), dist);
                    g_travel.phase = TravelPhase::Holding;
                    g_travel.lingerUntil = now + std::chrono::seconds(
                                              static_cast<int>(Config::g_batchLinger.load()));
                    // no return -- fall into Holding
                } else {
                    return;   // still walking to the current target
                }
            }

            // ── HOLDING: seek the next leg (retarget to a walkable corpse) or
            // grab one within arm's reach. If nothing is actionable but loot is
            // still under the player's dibs, linger and re-scan; else the batch is
            // exhausted -> release and return to the player.
            if (g_travel.active && g_travel.phase == TravelPhase::Holding) {
                // The two loot bars (a ContainerMenu open, or the player sneaking)
                // make LootNearby early-return BEFORE it scans, so the excursion
                // scan would come back empty with g_scanSawWaiting false and be
                // misread as "batch exhausted" -> premature release. Treat them as
                // a HOLD (bounded by the excursion cap), the same courtesy arrival
                // gives -- don't let a menu/crouch gut the batch.
                auto* ui = RE::UI::GetSingleton();
                if ((ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) ||
                    (pc && pc->IsSneaking()))
                    return;   // hold, retry next tick
                if (RunExcursionScan(a_follower, a_state, now)) {
                    // Retargeted (phase now Walking) or grabbed a cluster corpse
                    // (still Holding) -- productive, so extend the linger.
                    if (g_travel.phase == TravelPhase::Holding)
                        g_travel.lingerUntil = now + std::chrono::seconds(
                                                  static_cast<int>(Config::g_batchLinger.load()));
                    return;
                }
                if (g_scanSawWaiting && now <= g_travel.lingerUntil)
                    return;   // loot still under your dibs -- hold, re-scan next tick
                Packages::LootTravelClear("batch done", a_follower);
                g_travel = TravelIntent{};
                return;
            }
        }

        if (a_state.logistics().empty()) return;   // no rules -> nothing to run

        // FALL-THROUGH scan. Try matching rules in order until one ACTUALLY
        // acts. A rule that matches but whose action finds nothing -- "loot
        // potions" with no potions nearby, "loot better gear" with no upgrade,
        // a drink on cooldown -- must NOT shadow the useful rules below it. That
        // shadow was why looting looked dead: the near-always-true low-potions
        // rule (a Requiem follower rarely carries >=2) won every tick and found
        // nothing, so "loot equipment" was never reached. Still at most ONE real
        // action per tick (§4.8.3); the loop only skips PAST non-acting matches.
        bool  acted = false;
        int   fired = -1;
        std::string label;   // a COPY -- `choice` is loop-scoped; c_str() would dangle
        for (int start = 0; ; ) {
            const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
            if (choice.ruleIndex < 0) break;          // nothing (more) matched
            const auto& op = choice.actionOpcode;
            fired = choice.ruleIndex;
            label = op;

            if      (op == Vocab::kActDrinkHealthPotion)  acted = DrinkBest(a_follower, RE::ActorValue::kHealth);
            else if (op == Vocab::kActDrinkStaminaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kStamina);
            else if (op == Vocab::kActDrinkMagickaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kMagicka);
            else if (op == Vocab::kActLootArrows)         acted = LootNearby(a_follower, Category::Arrows, now);
            else if (op == Vocab::kActLootBolts)          acted = LootNearby(a_follower, Category::Bolts,  now);
            else if (op == Vocab::kActLootPotions)        acted = LootNearby(a_follower, Category::Potions, now);   // any drinkable
            else if (op == Vocab::kActLootHealthPotion)   acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kHealth);
            else if (op == Vocab::kActLootStaminaPotion)  acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kStamina);
            else if (op == Vocab::kActLootMagickaPotion)  acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kMagicka);
            else if (op == Vocab::kActLootEquipment)      acted = LootNearby(a_follower, Category::Equipment, now);
            else if (op == Vocab::kActLootGold)           acted = LootNearby(a_follower, Category::Gold, now);
            else if (op == Vocab::kActLootJewelry)        acted = LootNearby(a_follower, Category::Jewelry, now);
            else if (op == Vocab::kActLootSoulGems)       acted = LootNearby(a_follower, Category::SoulGems, now);
            else if (op == Vocab::kActLootLockpicks)      acted = LootNearby(a_follower, Category::Lockpicks, now);
            else if (op == Vocab::kActWait) {
                return;   // Wait consumes the tick and suppresses below (#3.3) -- stops the scan.
            }
            else {
                // Unknown / non-logistics opcode: skip PAST it so a stray rule
                // cannot shadow the rest (does not fall through to a real action).
                spdlog::info("[logistics] {:08X} rule {} has non-logistics action '{}' -- skipped",
                             id, choice.ruleIndex, op);
                start = choice.ruleIndex + 1;
                continue;
            }

            if (acted) break;                  // did something real -> done this tick
            start = choice.ruleIndex + 1;      // matched but no-op -> try the next rule
        }

        if (acted) {
            g_idleCycles.erase(id);   // productive -> not idle
            spdlog::info("[logistics] {:08X} rule {} fired: {}", id, fired, label);
        } else {
            // IDLE this tick. After a few idle ticks with no excursion running,
            // wipe the travel blocklist so previously-skipped bodies (the 340u/382u
            // arrow corpses, or ones that were unreachable before he moved) get a
            // fresh assessment -- bounded to once per kReassessCooldown so a
            // genuinely-unreachable body can't drive a re-attempt loop.
            if (!g_travel.active) {
                const int ic = ++g_idleCycles[id];
                if (ic >= kIdleReassessCycles && !g_travelFailed.empty() &&
                    (g_lastBlocklistReassess.time_since_epoch().count() == 0 ||
                     now - g_lastBlocklistReassess >= kReassessCooldown)) {
                    const size_t n = g_travelFailed.size();
                    g_travelFailed.clear();
                    // NOTE: g_stallStrikes deliberately SURVIVES the reassess. Wiping
                    // it here defeated the 2-strike sticky entirely: a geometrically
                    // unreachable body (navmesh path ends short -- e.g. deck 0002CFBF
                    // navdist=148 < gate 300, dist frozen every walk) stalls once per
                    // excursion, but the reassess reset its strike to 0 before the 2nd
                    // could promote it, so it was re-picked forever and burned whole
                    // excursions. Strikes now accumulate across excursions; a body that
                    // proves reachable has its strike cleared on ARRIVAL (below), so a
                    // merely-transient block never falsely reaches sticky.
                    g_lastBlocklistReassess = now;
                    g_idleCycles[id] = 0;
                    spdlog::info("[loot] {:08X} idle {} ticks -- cleared {} blocklisted "
                                 "bodies to reassess", id, ic, n);
                }
            }
            // Heartbeat so "serviced, nothing to do" is distinguishable from
            // "never ran" (#53) -- promoted from debug (never written at info
            // level) to a RATE-LIMITED info line, once per follower per ~30 s.
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextIdleLog;
            auto& nxt = s_nextIdleLog[id];
            if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                nxt = now + std::chrono::seconds(30);
                spdlog::info("[logistics] {:08X} serviced -- nothing to loot/drink right now", id);
            }
            // ECONOMY PROBE (#21) -- DISABLED again 2026-07-31 after the MAIN-THREAD
            // run STILL crashed (crash-2026-07-31-15-49-45, vendor Ma'dran). That
            // is the decisive result: the vendor CTD is NOT a thread race. The pump
            // is proven live ("[mainthread] first drain ..."), the probe ran ON the
            // main thread, and it still null-derefed inside chest->GetInventory()
            // on the merchant's chest (RSI=Chest, a form cast -> null -> deref). The
            // chest is a persistent merchant container that no barter menu has
            // populated, so its InventoryChanges is not set up and native
            // GetInventory faults on it -- on ANY thread. GetContainer() passes (it
            // IS a real container); the fault is deeper.
            //
            // So the real barter cannot read merchant stock via native
            // TESObjectREFR::GetInventory on an unpopulated merchant chest. It needs
            // a safe merchant-read (Papyrus, which is what the game's own barter menu
            // uses; or force-initialising the chest's InventoryChanges first). The
            // main-thread pump (§0.37) is still correct + needed for the TRANSACTION
            // (mutations must run on main), just not sufficient for the READ.
            // See [[economy-vendor-detection-excludes-teammates]] + ENGINE_NOTES.
            // Phase 1 (#21): re-enabled as a DIAGNOSTIC build (v0.8.42) -- PDB now
            // emitted + per-step [bc] breadcrumbs in EconomyProbe, so the v0.8.40
            // CTD pins to an exact step/line instead of a guess.
            // Runs HERE, on the WORKER, NOT via MainThread::Post (Fable audit #1/#4):
            // the reads -- follower GetInventory / CountPotions / g_travel -- must
            // stay on the SAME thread as the loot/heal/loadout mutations that share
            // this task, or they race the follower's InventoryChanges (the Actor.cpp
            // :445 CTD class). The old main-thread rationale ("mutations run on main")
            // is obsolete: the merchant read AND the transaction now run in Papyrus
            // (VM thread) via the bridge, so nothing here needs main -- and the
            // dispatch is worker->VM, exactly like DispatchActivate already does.
            static constexpr bool kEconProbeEnabled = true;
            if (kEconProbeEnabled)
                EconomyProbe(a_follower, a_state.logistics(), now);
        }
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[logistics] no event source holder -- waiver sink NOT installed");
            return;
        }
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        spdlog::info("[logistics] player-looted waiver sink installed");
    }

    void ClearTransientState() {
        g_nextTick.clear();
        g_claim.clear();
        g_lastLootSource = 0;
        g_drinkUntil.clear();
        g_playerLooted.clear();
        g_econScan.clear();   // #21 econ cadence clocks -- save-scoped (Fable audit #7)
        g_econTrade.clear();
        g_econPair.clear();
        // Drop any in-flight travel intent and release the engine alias so a
        // revert/load never leaves a follower latched (#55).
        if (g_travel.active) Packages::LootTravelClear("revert");
        g_travel = TravelIntent{};
        g_travelFailed.clear();
        g_travelUnreach.clear();
        g_stallStrikes.clear();
        g_idleCycles.clear();
        g_lastBlocklistReassess = {};
    }

    void ReleaseTravelOnCombat(RE::Actor* a_follower) {
        if (!a_follower) return;
        if (g_travel.active && g_travel.follower == a_follower->GetFormID()) {
            // EVICT him from the loot alias and re-evaluate NOW so the combat
            // table / his own AI takes over this tick, not on the engine's slow
            // pass. The corpse is not "failed" -- he can finish it after the
            // fight -- so no LRU mark.
            Packages::LootTravelClear("combat", a_follower);
            g_travel = TravelIntent{};
        }
    }

    void OnFollowerRemoved(RE::FormID a_id) {
        // A follower dismissed DURING an excursion may still hold alias 0 (Clear
        // hasn't run). With his framework claim gone, MFO's static-60 claim is his
        // sole one -- he'd walk to the stale corpse and re-latch every load.
        // LootTravelEvictIf no-ops unless a_id is the current holder; between
        // excursions the slot holds the player, so this is normally a no-op.
        Packages::LootTravelEvictIf(a_id);
        // Forget the live intent too, if he was the active traveller.
        if (g_travel.active && g_travel.follower == a_id) g_travel = TravelIntent{};
    }

}
