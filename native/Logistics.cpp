#include "PCH.h"
#include "Logistics.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Confidence.h"   // the confidence leash (core tenet)
#include "Packages.h"     // Option A: LootTravelFill / LootTravelClear

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

        // First time a lootable ref was OBSERVED in a follower's radius. The
        // first-dibs clock counts from here (#22h). Bounded LRU.
        std::unordered_map<RE::FormID, Clock::time_point> g_seen;

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

        // ── loot eligibility (#22h) ─────────────────────────────────────────
        // A ref is eligible once it has sat in radius fFirstDibsDelay seconds --
        // UNLESS the player has taken from it, which collapses the wait to
        // fQuickLootWaiver (never to zero). Reads only our own timers.
        bool LootEligible(RE::FormID a_refID, Clock::time_point a_now) {
            if (const auto w = g_playerLooted.find(a_refID); w != g_playerLooted.end()) {
                return std::chrono::duration<float>(a_now - w->second).count() >=
                       Config::g_quickLootWaiver.load();
            }
            const auto s = g_seen.find(a_refID);
            if (s == g_seen.end()) return false;   // never seen -> never eligible
            return std::chrono::duration<float>(a_now - s->second).count() >=
                   Config::g_firstDibsDelay.load();
        }

        // ── the follower's equipped ranged weapon, for ammo matching ────────
        // Returns the equipped bow/crossbow, or nullptr. Reads the NAMED
        // follower only (#14).
        // Ammo of one class (bolts vs arrows) the actor carries. NO bow gate:
        // the ACTION is dumb -- whether a follower should gather ammo is the
        // GAMBIT's condition to decide, not this function's (marth). Arrows and
        // bolts are counted/looted separately (they are different gambits).
        int AmmoCount(RE::Actor* a_actor, bool a_wantBolt) {
            if (!a_actor) return 0;
            int n = 0;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* ammo = obj->As<RE::TESAmmo>(); ammo && ammo->IsBolt() == a_wantBolt)
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
        bool LootAmmo(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_wantBolt) {
            // Collect matching ammo tuples first (object, count), THEN transfer
            // -- RemoveItem dispatches TESContainerChangedEvent synchronously, so
            // mutating the source inventory mid-walk is the #2 landmine.
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* ammo = obj->As<RE::TESAmmo>(); ammo && ammo->IsBolt() == a_wantBolt)
                    takes.push_back({ obj, data.first });
            }
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
        bool LootPotions(RE::Actor* a_follower, RE::TESObjectREFR* a_src, RE::ActorValue a_want) {
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
                takes.push_back({ obj, data.first });
            }
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
        bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            const float cand = a_armo->GetArmorRating();
            if (cand <= 0.0f) return false;   // clothing / jewellery -- not armor

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

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
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

            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;

                if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                    if (!bestArmor && ArmorIsBetter(a_follower, armo)) bestArmor = obj;
                } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    // In the follower's best class, and strictly harder-hitting
                    // than what they effectively wield. A bow never beats a sword
                    // for a swordsman; a greatsword beats a dagger for a 2h user.
                    if (wieldsRealWeapon &&
                        WeaponClassOf(weap->GetWeaponType()) == myClass &&
                        weap->GetAttackDamage() > bestWeapDmg) {
                        bestWeapDmg = weap->GetAttackDamage();
                        bestWeap    = obj;
                    }
                }
            }

            // Prefer the weapon upgrade -- it is what makes the follower hit
            // harder; else the better armor piece. One item this tick (§4.3).
            RE::TESBoundObject* best = bestWeap ? bestWeap : bestArmor;
            if (!best) return false;
            if (!FitsCarryWeight(a_follower, best->GetWeight())) return false;

            a_src->RemoveItem(best, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                              nullptr, a_follower);

            // PUT IT ON. EquipObject on a slot-conflicting armor auto-unequips
            // the worse piece; a weapon takes the right hand. queue=true so the
            // engine applies it on its own next update rather than synchronously
            // re-entering here.
            if (auto* eq = RE::ActorEquipManager::GetSingleton())
                eq->EquipObject(a_follower, best);
            return true;
        }

        // Take all the gold on a corpse/container. Gold001 is the one hardcoded
        // FormID in the game (0x0000000F, Skyrim.esm) -- there is no "gold type"
        // to derive, so this is the sole by-FormID check in the loot code, and
        // it is stable (a base-game record every load order carries). Weightless,
        // so no carry-weight gate; nothing to equip. Held for the player, who
        // gets it back by trading -- which is why gold WAITS out first dibs
        // (below), same as gear: you want first pick of the coin.
        bool LootGold(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
            constexpr RE::FormID kGold001 = 0x0000000F;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() != kGold001) continue;
                a_src->RemoveItem(obj, data.first, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                return true;
            }
            return false;
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
            if (!best) return false;

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
        enum class Category { Arrows, Bolts, Potions, Equipment, Gold };

        // OPTION A travel state -- a SINGLE traveller at a time (one loot quest,
        // one alias pair). Worker-tick-only, NOT serialized: this just remembers
        // the intent; the engine-side alias fill is cleared by Packages on load
        // (#55). kArrivalDist ~= arm's reach: once the engine walks the follower
        // this close, the existing inventory transfer runs.
        constexpr float kArrivalDist = 160.0f;
        struct TravelIntent {
            RE::FormID          follower = 0;
            RE::ObjectRefHandle target;
            Category            cat  = Category::Arrows;
            RE::ActorValue      want = RE::ActorValue::kNone;
            Clock::time_point   deadline{};
            bool                active = false;
        };
        TravelIntent g_travel;

        // Walk nearby refs, gate them, and perform ONE transfer. Returns true if
        // something was looted. Collect-then-act (#2): the world walk only reads
        // and records timers; all mutation happens afterwards on re-resolved
        // handles.
        bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now,
                        RE::ActorValue a_potionWant = RE::ActorValue::kNone) {
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

            // Eligible loot sources, collected inside the walk and acted on after
            // it. Bounded so a room full of corpses cannot make the tick unbounded.
            std::vector<RE::ObjectRefHandle> candidates;
            candidates.reserve(16);

            // DIAGNOSTIC counters (marth: "nothing looted" -- find WHICH stage
            // drops the corpse). Logged rate-limited below.
            int dRefs = 0, dLootable = 0, dOwned = 0, dOffLimits = 0, dLocked = 0, dNotYet = 0, dLeash = 0;

            cell->ForEachReferenceInRange(origin, kLootRadius,
                [&](RE::TESObjectREFR& a_ref) {
                    if (candidates.size() >= 16) return RE::BSContainer::ForEachResult::kStop;
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
                    // our side at all. Until then, loose items are simply not looted.
                    bool lootable = false;
                    if (auto* actor = ref->As<RE::Actor>()) {
                        lootable = actor->IsDead();
                    } else if (auto* base = ref->GetBaseObject()) {
                        lootable = base->Is(RE::FormType::Container);
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
                    // Locked is locked -- RemoveItem ignores the lock, so the
                    // filter must not.
                    if (ref->IsLocked())    { ++dLocked;    return RE::BSContainer::ForEachResult::kContinue; }
                    // Beyond the confidence leash from the player -- too far for
                    // this follower's nerve right now. This is the invisible
                    // string: the same corpse is in-reach when they feel safe and
                    // out-of-reach when they do not.
                    if (playerPos.GetDistance(ref->GetPosition()) > leash) {
                        ++dLeash; return RE::BSContainer::ForEachResult::kContinue;
                    }

                    // FIRST DIBS is the PLAYER's pick of VALUABLES (#22h) -- it
                    // does NOT apply to a follower restocking its own consumables
                    // (marth): you don't compete for the arrows/potions on a
                    // corpse, and followers rarely linger the full delay anyway.
                    // So arrows/potions are eligible the moment they're SAFE
                    // (unowned/unlocked, already checked above); EQUIPMENT and
                    // GOLD wait out the delay so you get first pick of the loot
                    // that is actually worth competing for.
                    if (a_cat != Category::Equipment && a_cat != Category::Gold) {
                        candidates.push_back(ref->GetHandle());
                    } else {
                        const auto id = ref->GetFormID();
                        if (g_seen.emplace(id, a_now).second) EvictOldest(g_seen);
                        if (LootEligible(id, a_now)) candidates.push_back(ref->GetHandle());
                        else ++dNotYet;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            // One diagnostic line per follower per ~10 s (any category), so the
            // walk composition is visible without flooding the ~1 s tick.
            {
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextWalkLog;
                auto& nxt = s_nextWalkLog[a_follower->GetFormID()];
                if (nxt.time_since_epoch().count() == 0 || a_now >= nxt) {
                    nxt = a_now + std::chrono::seconds(10);
                    // Include the follower's OWN state so a "3 eligible but
                    // nothing looted" is unambiguous -- an eligible corpse yields
                    // nothing if it simply does not hold the thing the winning
                    // rule wants (arrows, or the specific potion).
                    spdlog::info("[loot] {:08X} r={:.0f} leash={:.0f}: {} refs, {} lootable, {} eligible "
                                 "(dropped owned={} locked={} waiting={} farleash={}) | self arrows={} bolts={} "
                                 "potH={} potS={} potM={}",
                                 a_follower->GetFormID(), kLootRadius, leash, dRefs, dLootable,
                                 (int)candidates.size(), dOwned, dLocked, dNotYet, dLeash,
                                 ArrowCount(a_follower), BoltCount(a_follower),
                                 CountPotions(a_follower, RE::ActorValue::kHealth),
                                 CountPotions(a_follower, RE::ActorValue::kStamina),
                                 CountPotions(a_follower, RE::ActorValue::kMagicka));
                }
            }

            // Act after the walk. Re-resolve each handle at act time (#2); stop at
            // the first successful transfer -- one loot action per tick (§4.8.3).
            for (auto& h : candidates) {
                auto ptr = h.get();
                auto* ref = ptr.get();
                if (!ref) continue;

                // OPTION A: if the loot is beyond arm's reach and walk-to-loot is
                // ON, send the follower to WALK there -- one traveller at a time;
                // the transfer runs on ARRIVAL (see ServiceFollower). If travel
                // is busy or unavailable we do NOT teleport it (that would defeat
                // the point) -- we skip and try again next tick. With travel OFF
                // (default) the follower teleport-grabs, as before.
                if (origin.GetDistance(ref->GetPosition()) > kArrivalDist &&
                    Config::g_lootTravel.load()) {
                    if (!g_travel.active && Packages::LootTravelFill(a_follower, ref)) {
                        g_travel.active   = true;
                        g_travel.follower = a_follower->GetFormID();
                        g_travel.target   = ref->GetHandle();
                        g_travel.cat      = a_cat;
                        g_travel.want     = a_potionWant;
                        g_travel.deadline = a_now + std::chrono::seconds(12);
                        return true;   // committed to the walk; transfer on arrival
                    }
                    continue;   // travel busy/unavailable -- wait, don't teleport
                }

                bool moved = false;
                // Corpse/container: transfer the wanted category out of its
                // inventory. (Loose world items are excluded at the walk gate --
                // see the note there; they belong to the package-acquisition
                // feature, not this worker-thread transfer path.)
                switch (a_cat) {
                case Category::Arrows:    moved = LootAmmo(a_follower, ref, false); break;
                case Category::Bolts:     moved = LootAmmo(a_follower, ref, true);  break;
                case Category::Potions:   moved = LootPotions(a_follower, ref, a_potionWant); break;
                case Category::Equipment: moved = LootEquipment(a_follower, ref); break;
                case Category::Gold:      moved = LootGold(a_follower, ref); break;
                }
                if (moved) return true;
            }
            return false;
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

    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!Config::g_logistics.load()) return;   // whole subsystem off by default (#45)
        if (!a_follower) return;

        const auto id  = a_follower->GetFormID();
        const auto now = Clock::now();

        // GLOBAL stale-intent expiry, keyed to NO follower in particular. The
        // per-follower arrival branch below only runs when the TRAVELLER is
        // serviced out of combat -- but a traveller who died, was dismissed,
        // dropped off the roster, or got pulled down the combat branch is never
        // serviced there, so its intent (and the single loot alias) would stick
        // all session. The deadline is the backstop, checked for every follower
        // so it fires even when the traveller itself is gone. (#55: the alias
        // also self-heals on the next load via ReleaseAll.)
        if (g_travel.active && now > g_travel.deadline) {
            Packages::LootTravelClear("stale");
            g_travel.active = false;
        }

        // CADENCE GATE (~1 s). Cheap early-out on the frames between logistics
        // ticks -- the Scheduler calls this every time it services the follower
        // out of combat (up to ~7.5 Hz), but logistics only acts at the idle rate.
        auto& due = g_nextTick[id];
        if (due.time_since_epoch().count() != 0 && now < due) return;
        due = now + kLogisticsInterval;

        // OPTION A: if THIS follower is mid-travel to loot, drive that instead of
        // a fresh eval. Arrive-by-distance -> run the transfer; interrupt on
        // combat / timeout / vanished target; ALWAYS release the alias (it is
        // save-serialized, #55). A different follower travelling just means the
        // single loot alias is busy -- handled in LootNearby.
        if (g_travel.active && g_travel.follower == id) {
            auto tptr  = g_travel.target.get();
            auto* tref = tptr.get();
            // Re-validate the target: it was gated up to 12 s ago and may since
            // have been disabled/deleted (the player looted and the engine
            // cleaned it up).
            const bool gone = !tref || tref->IsDisabled() || tref->IsMarkedForDeletion();
            if (gone || a_follower->IsInCombat() || now > g_travel.deadline) {
                Packages::LootTravelClear(a_follower->IsInCombat() ? "combat"
                                          : (gone ? "target gone" : "timeout"));
                g_travel.active = false;
                // fall through to a normal eval this tick.
            } else if (a_follower->GetPosition().GetDistance(tref->GetPosition()) <= kArrivalDist) {
                // §22g ABSOLUTE BAR (the same one LootNearby honours): never
                // mutate a container while the player has a ContainerMenu open --
                // arrival lands on the SAME fresh corpse the player may be
                // looting (arrows/potions/gold skip first-dibs). Wait; the
                // deadline bounds the intent. Also hold off if the player just
                // crouched (the sneak courtesy).
                auto* ui = RE::UI::GetSingleton();
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if ((ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) ||
                    (pc && pc->IsSneaking())) {
                    return;   // still "arrived", just holding -- retry next tick
                }
                bool moved = false;
                switch (g_travel.cat) {
                case Category::Arrows:    moved = LootAmmo(a_follower, tref, false); break;
                case Category::Bolts:     moved = LootAmmo(a_follower, tref, true);  break;
                case Category::Potions:   moved = LootPotions(a_follower, tref, g_travel.want); break;
                case Category::Equipment: moved = LootEquipment(a_follower, tref); break;
                case Category::Gold:      moved = LootGold(a_follower, tref); break;
                }
                Packages::LootTravelClear("arrived");
                g_travel.active = false;
                spdlog::info("[loot] {:08X}: arrived -- {}", id, moved ? "looted" : "nothing to take");
                return;   // the arrival transfer IS this tick's action
            } else {
                return;   // still walking -- do not start a fresh loot eval
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
            spdlog::info("[logistics] {:08X} rule {} fired: {}", id, fired, label);
        } else {
            // Heartbeat so "serviced, nothing to do" is distinguishable from
            // "never ran" (#53) -- promoted from debug (never written at info
            // level) to a RATE-LIMITED info line, once per follower per ~30 s.
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextIdleLog;
            auto& nxt = s_nextIdleLog[id];
            if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                nxt = now + std::chrono::seconds(30);
                spdlog::info("[logistics] {:08X} serviced -- nothing to loot/drink right now", id);
            }
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
        g_seen.clear();
        g_drinkUntil.clear();
        g_playerLooted.clear();
        // Drop any in-flight travel intent and release the engine alias so a
        // revert/load never leaves a follower latched (#55).
        if (g_travel.active) Packages::LootTravelClear("revert");
        g_travel = TravelIntent{};
    }

}
