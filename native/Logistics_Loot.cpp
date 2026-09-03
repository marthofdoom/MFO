// Logistics_Loot.cpp -- the LOOT side of the logistics family (split
// mechanically out of Logistics.cpp, no logic change): per-category looters
// (ammo/potions/equipment/gold/jewelry/soul gems/lockpicks), the loot judge
// (weapon roles, armor/mage-apparel comparison, creature-gear guards), the
// claim-and-release fair-chance machinery, navmesh reach, corpse strip and
// the excursion scan, and LootNearby itself. Travel STATE (TravelIntent,
// g_travelSlots) is shared with ServiceFollower and lives in
// Logistics_internal.h.
#include "Logistics_internal.h"

namespace MFO::Logistics {

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
                      bool a_peek) {
            // Collect first, mutate after -- RemoveItem dispatches
            // TESContainerChangedEvent synchronously, so touching an inventory
            // mid-walk is the #2 landmine.
            struct Ammo { RE::TESBoundObject* obj; std::int32_t count; float dmg; };
            std::vector<Ammo> body;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* am = obj->As<RE::TESAmmo>(); am && AmmoIsBolt(am) == a_wantBolt)
                    body.push_back({ obj, static_cast<std::int32_t>(data.first),
                                     am->GetRuntimeData().data.damage });
            }
            if (body.empty()) return false;

            // The follower's OWN matching ammo, and its WORST damage. With none held
            // this is a pure restock (worstHeld = -1 -> every body arrow is "better",
            // nothing to shed). Held junk turns it into a TRADE.
            std::vector<Ammo> held;
            float worstHeld = std::numeric_limits<float>::max();
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* am = obj->As<RE::TESAmmo>(); am && AmmoIsBolt(am) == a_wantBolt) {
                    const float d = am->GetRuntimeData().data.damage;
                    held.push_back({ obj, static_cast<std::int32_t>(data.first), d });
                    worstHeld = std::min(worstHeld, d);
                }
            }
            if (held.empty()) worstHeld = -1.0f;

            // PEEK: the eligibility check must agree with the TAKE below, or the
            // follower walks back to a corpse holding only the junk it just shed and
            // takes nothing, forever (Fable). RESTOCK, not upgrade-only: this action
            // is gated by the gambit's own count condition ("arrows < N"), so it only
            // fires WHILE the follower is short -- it must accept same-tier ammo (>=
            // worst held), or a low archer stands over corpses full of the very arrows
            // he's firing and never restocks. The condition self-limits the quantity
            // (it stops firing once the follower is back above N), exactly like the
            // potion path. Only STRICTLY worse ammo is passed over.
            if (a_peek) {
                for (auto& b : body) if (b.dmg >= worstHeld) return true;
                return false;
            }

            // TAKE the body arrows at-or-above the follower's worst, best-first (so
            // the carry-weight gate drops iron, never ebony). Track the weakest we
            // actually took, so the shed below never gives back something as good.
            std::sort(body.begin(), body.end(), [](const Ammo& a, const Ammo& b) { return a.dmg > b.dmg; });
            std::int32_t taken = 0;
            float minTaken = std::numeric_limits<float>::max();
            for (auto& b : body) {
                if (b.dmg < worstHeld) break;   // strictly worse -> never downgrade
                if (!FitsCarryWeight(a_follower, b.obj->GetWeight() * b.count)) continue;
                a_src->RemoveItem(b.obj, b.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_follower);
                taken   += b.count;
                minTaken = std::min(minTaken, b.dmg);
            }
            if (taken == 0) return false;

            // TRADE (marth #35): give the follower's WORST arrows back to the body,
            // one per better arrow taken -- capped at `taken` (<= better available)
            // and only ever shedding arrows strictly worse than the weakest we took.
            // Empty-handed followers shed nothing (held is empty), so it stays a
            // clean restock; a junk stack gets upgraded count-neutral.
            std::sort(held.begin(), held.end(), [](const Ammo& a, const Ammo& b) { return a.dmg < b.dmg; });
            std::int32_t toShed = taken;
            for (auto& h : held) {
                if (toShed <= 0) break;
                if (h.dmg >= minTaken) break;   // worst-first: nothing worse remains
                const std::int32_t n = std::min(toShed, h.count);
                a_follower->RemoveItem(h.obj, n, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, a_src);
                toShed -= n;
            }
            return true;
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
            struct Take { RE::TESBoundObject* obj; std::int32_t count; float mag; };
            std::vector<Take> takes;
            // LOW-POWER FLOOR (marth: "ignore low power potions entirely"). A restore
            // potion whose magnitude is below this is never looted. iMinPotionMag > 0
            // is an explicit magnitude floor; 0 means use the auto floor derived from
            // the load order's weakest tier (g_autoPotionFloor). Fortify/cure carry
            // magnitude 0 here and are never filtered.
            const float floor = PotionLootFloor();
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (a_want == RE::ActorValue::kNone) {
                    if (!IsDrinkablePotion(alc)) continue;         // catch-all: any drinkable
                } else {
                    if (!alc || PotionRestores(alc) != a_want) continue;   // only this resource
                }
                const float mag = alc ? PotionMagnitude(alc) : 0.0f;
                if (floor > 0.0f && mag > 0.0f && mag < floor) continue;   // low power -> ignore
                takes.push_back({ obj, data.first, mag });
            }

            // BEST FIRST (marth's standing rule for threshold-gated supply): take the
            // strongest potions first, so a carry-weight cutoff keeps the best, not
            // whatever the inventory happened to enumerate first.
            std::sort(takes.begin(), takes.end(),
                      [](const Take& a, const Take& b) { return a.mag > b.mag; });

            if (a_peek) return !takes.empty();
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // Is this armor worth acquiring? Two ways to qualify on a slot it covers:
        // it STRICTLY beats real worn armor there (an upgrade), OR it DRESSES a
        // BARE slot -- one with nothing worn, or only rating-0 clothing/rags. It
        // must be beaten on NO filled slot (never a downgrade). A slot the
        // follower has EMPTY is acquirable on its own (marth field: a helmetless
        // follower must pick up a helmet with no helmet already on him to
        // "upgrade") -- this mirrors the mage clothing path's bare-slot dress.
        // "Better" is the item's OWN rating vs what the follower wears in the same
        // slots -- the §4.8.2 derived-vocabulary principle, so modded gear works
        // with no patch. Reads the named follower.
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
                // BARE slot -- nothing worn, or only rating-0 clothing/rags: this
                // piece DRESSES it. Valid to acquire even with no armor to upgrade
                // (the helmetless-follower case). Not a downgrade -> keep scanning.
                if (!worn || worn->GetArmorRating() <= 0.0f) continue;
                // Real worn armor here: only a strictly higher rating may replace
                // it. A tie or worse is beaten -> never downgrade real worn armor.
                if (worn->GetArmorRating() >= cand) return false;
            }
            // Acquirable if it actually covers an armor slot (skip amulets/rings
            // whose bits are not in kSlots) and was beaten on none it would
            // replace -- i.e. it upgrades every filled slot and/or dresses a bare one.
            return overlapsAny;
        }

        // #3 STRIP DOUBLE-TAKE: does the follower ALREADY CARRY a playable armor
        // covering any slot a_armo would fill, at >= its rating? The arm's-reach
        // strip drains Equipment in ONE worker tick, and each take's EQUIP is
        // DEFERRED to the main thread (doEquip) -- so GetWornArmor still shows the
        // OLD worn piece for EVERY take in the drain. Two same-slot upgrades both
        // pass ArmorIsBetter (both beat the stale worn baseline) and both queue an
        // equip, and the WORSE one -- queued last -- wins. Baselining against what's
        // already IN THE PACK (the better piece taken earlier this strip, transferred
        // synchronously by RemoveItem) stops the second, worse take from being
        // looted/equipped at all. Outside a strip this is a no-op / a harmless
        // anti-hoard: no reason to grab a spare worse than one already carried.
        bool IsCreatureArmor(const RE::TESObjectARMO* a_armo);   // fwd (defined near LootEquipment)
        bool CarriesSlotArmorAtLeast(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            const float cand = a_armo->GetArmorRating();
            const auto  mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            if (mask == 0) return false;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* have = obj->As<RE::TESObjectARMO>();
                if (!have || have == a_armo || IsCreatureArmor(have)) continue;
                if (have->GetArmorRating() < cand) continue;   // worse -> not a competing baseline
                if (static_cast<std::uint32_t>(have->GetSlotMask()) & mask)
                    return true;   // a carried piece already covers this slot at >= rating
            }
            return false;
        }

        // ── #62 BROKEN-GEAR EVICTION (root-caused 2026-08-10) ────────────────
        // The invisible-head bug is MFO having equipped an item that renders on
        // NO playable body. VERIFIED root cause (headless save + load-order plugin
        // parse, Linux-Native-Tools): Inigo -- who is STOCK KhajiitRace 0x13745,
        // NOT a custom race -- was wearing DraugrHelmet01 (0001FD77), a
        // NON-PLAYABLE draugr helmet whose single ArmorAddon is race-locked to
        // DraugrRace with no coverage for ANY playable race (not even Nords). A
        // pre-v1.0.41 MFO looted it off a draugr corpse and equipped it -- the
        // ARMOR twin of the old creature-WEAPON bug (IsCreatureWeapon, v0.8.34).
        // Such a piece hides the head partition but draws nothing. Vanilla never
        // does this: only MFO's looting put creature gear on a follower, which is
        // exactly why the bug never happens without MFO. So it is NOT beast-
        // specific -- a non-playable helmet blanks any race's head.
        //
        // FIX (marth): do away with items that shouldn't be equippable; leave
        // proper-race selection to the engine. There is nothing to "swap to the
        // Khajiit version" for a draugr helmet -- no playable variant exists -- so
        // it is DELETED. Standard armor is a single ARMO that auto-selects the
        // right ArmorAddon per race (the ARMA system IS the swap), so MFO looting
        // only playable gear (the IsCreatureArmor loot filter, below) already
        // guarantees proper-race rendering going forward. This routine is the
        // CLEANUP for gear a pre-fix MFO stuck on, plus a trade that hands a bad
        // piece over. NEVER a 3D reset (the v1.0.39-43 dead end: forcing a
        // rebuild is what actually dropped the head).
        //
        // One pass over the follower's WORN armor (all slots). A piece that draws
        // on his race (ArmorAddon match up the armorParentRace chain) is fine and
        // kept. What renders NOTHING is triaged:
        //   - his OWN-plugin non-rendering gear -> KEEP (intentional invisible
        //     native gear, e.g. a custom follower's design; #64).
        //   - a foreign NON-PLAYABLE piece -> DELETE. Creature/draugr gear, no
        //     valid playable variant, worthless to the player. Fixes Inigo.
        //   - a foreign PLAYABLE wrong-race piece IN A HEAD SLOT -> HAND BACK to
        //     the player. May still be a real item he wants (we can't fabricate a
        //     race-correct variant), so it goes to his pack, not the void. Scoped
        //     to head slots (the invisible-HEAD symptom) so a custom-race
        //     follower's rings/amulets/body aren't stripped on a null-resolve.
        // Main-thread only (equip + inventory mutation). PluginKey handles ESL/FE
        // light plugins (top byte 0xFE -> id>>12, else id>>24).
        void KeepHeadClear(RE::Actor* a_actor) {
            auto* race   = a_actor->GetRace();
            auto* eq     = RE::ActorEquipManager::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!race || !eq || !player) return;
            auto pluginKey = [](RE::FormID a_id) -> std::uint32_t {
                return ((a_id >> 24) == 0xFEu) ? (a_id >> 12) : (a_id >> 24);
            };
            const std::uint32_t actorKey = pluginKey(a_actor->GetFormID());
            // The NonPlayable record flag (bit 2) -- same read as IsCreatureArmor
            // (defined below with the loot filter); inlined here to avoid a forward
            // reference across the anonymous namespace.
            auto isNonPlayable = [](const RE::TESObjectARMO* a) {
                return a && (a->GetFormFlags() & (1u << 2)) != 0;
            };
            // JEWELRY slots (amulet/ring). The hand-back branch below covers every
            // VISIBLE armor slot -- head, body, hands, feet, shield, ... (marth: the
            // invisible-CHEST-piece case, not just the head) -- but SKIPS jewelry:
            // rings/amulets key their ArmorAddon off DefaultRace and can resolve
            // null on a custom race even when they render, and an invisible ring is
            // negligible, so the false-positive risk there isn't worth it. (A
            // NON-PLAYABLE creature ring is still DELETED in any slot above.)
            auto isJewelrySlot = [](RE::TESObjectARMO* a) {
                using S = RE::BGSBipedObjectForm::BipedObjectSlot;
                const auto m = static_cast<std::uint32_t>(a->GetSlotMask());
                return (m & (static_cast<std::uint32_t>(S::kAmulet) |
                             static_cast<std::uint32_t>(S::kRing))) != 0;
            };
            // Renders on the race IFF some ArmorAddon matches the race OR a race up
            // its armorParentRace (RNAM) chain -- the SAME resolution the engine
            // uses to build the biped. Walking the chain means a genuinely custom-
            // race follower whose gear renders via its parent is NOT a false hit;
            // the draugr helmet still resolves null (its one ARMA is DraugrRace,
            // reachable from no playable race).
            auto rendersOn = [](RE::TESObjectARMO* a_armo, RE::TESRace* a_race) {
                int guard = 0;
                for (RE::TESRace* r = a_race; r && guard < 8; r = r->armorParentRace, ++guard)
                    if (a_armo->GetArmorAddon(r)) return true;
                return false;
            };

            // ONE pass over the WORN inventory (all slots, deduped). A piece that
            // renders on his race is fine; his OWN-plugin non-rendering gear is
            // INTENTIONAL invisible native gear (#64) and kept. What's left renders
            // nothing and isn't his: NON-PLAYABLE -> DELETE (creature/draugr junk,
            // no valid variant, worthless to the player); otherwise a foreign
            // playable wrong-race piece -> HAND BACK to the player (may be a real
            // item; we can't fabricate a race-correct variant). Either way it leaves
            // his ownership so the AI can't re-equip it. Snapshot first -- RemoveItem
            // mutates the inventory map.
            struct Hit { RE::TESObjectARMO* armo; std::int32_t count; bool del; const char* why; };
            std::vector<Hit> hits;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!data.second || !data.second->IsWorn()) continue;
                auto* armo = obj ? obj->As<RE::TESObjectARMO>() : nullptr;
                if (!armo) continue;
                if (rendersOn(armo, race)) continue;                        // draws on his race -> keep
                if (pluginKey(armo->GetFormID()) == actorKey) continue;     // his OWN plugin -> intentional (#64)
                if (Catalog::IsExcluded(armo->GetFormID())) continue;       // #3 artifact/quest -- NEVER hand off (a follower may wear it; keep it on him even if it doesn't render)
                if (isNonPlayable(armo))
                    hits.push_back({ armo, data.first, true,  "NON-PLAYABLE creature armor" });   // DELETE, any slot
                else if (!isJewelrySlot(armo))
                    hits.push_back({ armo, data.first, false, "FOREIGN non-rendering armor" });    // HAND BACK, any visible slot
                // else: a foreign PLAYABLE non-rendering JEWELRY piece (amulet/ring)
                // is LEFT ALONE -- see isJewelrySlot above for why. Non-playable
                // creature junk is still deleted in ANY slot (jewelry included) above.
            }
            for (auto& h : hits) {
                const char* nm = h.armo->GetFullName();
                eq->UnequipObject(a_actor, h.armo);
                if (h.count > 0) {
                    if (h.del) a_actor->RemoveItem(h.armo, h.count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    else       a_actor->RemoveItem(h.armo, h.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
                }
                spdlog::info("[evict] {:08X}: {} {} '{}' ({:08X}) count={}",
                             a_actor->GetFormID(), h.del ? "DELETED" : "returned-to-player",
                             h.why, nm ? nm : "?", h.armo->GetFormID(), h.count);
            }
        }


        WeaponRoles ComputeWeaponRoles(RE::Actor* a_follower, const FollowerState& a_state) {
            using WT = RE::WEAPON_TYPE;
            const bool wantsMelee  = TableHasAction(a_state.combat(), Vocab::kActEquipMelee);
            const bool wantsRanged = TableHasAction(a_state.combat(), Vocab::kActEquipRanged);

            // Best MELEE skill decides 1H vs 2H (archery is irrelevant here) --
            // shared by the gambit-driven case AND the carries-but-no-gambit
            // fallback below; neither is a wielded-weapon guess.
            auto*       avo = a_follower->AsActorValueOwner();
            const float one = avo ? avo->GetActorValue(RE::ActorValue::kOneHanded) : 0.0f;
            const float two = avo ? avo->GetActorValue(RE::ActorValue::kTwoHanded) : 0.0f;
            const WepClass bestMeleeSkill = (two > one) ? WepClass::TwoHand : WepClass::OneHand;

            // ONE stable read of the pack: any melee weapon at all, any bow/
            // crossbow, and (for the bow-vs-crossbow call) the ammo/damage
            // census -- moved here verbatim from ShedOffRoleWeapon so both
            // callers derive wantCrossbow from the exact same scan.
            bool carriesMelee = false, carriesRanged = false;
            std::uint16_t bowDmg = 0, xbowDmg = 0;
            int arrows = 0, bolts = 0;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                    switch (WeaponClassOf(w->GetWeaponType())) {
                    case WepClass::OneHand:
                    case WepClass::TwoHand:
                        carriesMelee = true;
                        break;
                    case WepClass::Ranged:
                        carriesRanged = true;
                        if (w->GetWeaponType() == WT::kBow) bowDmg  = std::max(bowDmg,  w->GetAttackDamage());
                        else                                xbowDmg = std::max(xbowDmg, w->GetAttackDamage());
                        break;
                    default: break;   // staff/hand-to-hand -- not a role signal
                    }
                } else if (auto* am = obj->As<RE::TESAmmo>()) {
                    (AmmoIsBolt(am) ? bolts : arrows) += data.first;
                }
            }

            WeaponRoles roles;
            if (wantsMelee || carriesMelee) roles.melee = bestMeleeSkill;
            roles.doRanged = wantsRanged || carriesRanged;

            // BOW vs CROSSBOW (verbatim from ShedOffRoleWeapon): carrying both,
            // the ammo they hold decides (damage breaks a tie); carrying ONE
            // kind, that kind; carrying neither, their ammo decides.
            if (roles.doRanged) {
                if (bowDmg > 0 && xbowDmg > 0)      roles.wantCrossbow = (bolts != arrows) ? (bolts > arrows) : (xbowDmg > bowDmg);
                else if (bowDmg > 0 || xbowDmg > 0) roles.wantCrossbow = xbowDmg > bowDmg;
                else                                 roles.wantCrossbow = bolts > arrows;
            }
            return roles;
        }

        // KNOWN creature weapons Bethesda left UN-flagged (record flags 0, so the
        // NonPlayable gate below never fires): the giant clubs are kTwoHandSword /
        // EitherHand with normal weapon keywords -- byte-indistinguishable from a
        // real greatsword -- and their huge base damage makes them a "top upgrade"
        // (field-caught: a follower looted a Giant's Club). Curated against the
        // vanilla masters at the record level; every row verified flags==0 and
        // wielded only by creature races. Resolved through TESDataHandler ONCE so
        // the DLC rows survive any load order (Skyrim.esm is always index 00, but
        // one mechanism for all masters beats two).
        bool IsKnownCreatureWeapon(RE::FormID a_fid) {
            // Magic static: built on the first call, which is always a logistics/
            // equip-gate tick -- long after kDataLoaded, so the handler exists.
            static const std::unordered_set<RE::FormID> s_known = [] {
                std::unordered_set<RE::FormID> s;
                auto* dh = RE::TESDataHandler::GetSingleton();
                if (!dh) return s;
                constexpr struct { RE::FormID id; const char* plugin; } kRows[] = {
                    { 0x0461DA, "Skyrim.esm" },      // CrGiantClub (the reported loot)
                    { 0x0C334F, "Skyrim.esm" },      // DA06GiantClub
                    { 0x0CDEC9, "Skyrim.esm" },      // C00GiantClub
                    { 0x07F6DF, "Skyrim.esm" },      // crDwarvenSphereCrossbow (dummy mesh)
                    { 0x10EC8A, "Skyrim.esm" },      // crDwarvenSphereCrossbow02
                    { 0x012D14, "Dawnguard.esm" },   // DLC1FrostGiantClub
                    { 0x01E112, "Dragonborn.esm" },  // DLC2CrBenthicLurkerWeapon
                };
                for (const auto& r : kRows)
                    if (auto* f = dh->LookupForm(r.id, r.plugin)) s.insert(f->GetFormID());
                return s;
            }();
            return s_known.contains(a_fid);
        }

        // A NON-PLAYABLE weapon (record-header flag bit 2 == Mutagen
        // Weapon.MajorFlag.NonPlayable) is creature/automaton gear with no humanoid
        // mesh -- invisible if a follower equips it, though it still fires. Direct
        // check so the loot filter + heal work off the DLL alone, independent of
        // the catalog's nonplayable exclusion. NOTE: vanilla sets this flag on NO
        // weapon at all (measured: zero flagged WEAPs across the five masters);
        // it catches modded/overridden records, while the curated set above kills
        // the vanilla un-flagged ones (giant clubs, sphere crossbows, lurker fist).
        bool IsCreatureWeapon(const RE::TESObjectWEAP* a_w) {
            return a_w && ((a_w->GetFormFlags() & (1u << 2)) != 0 ||
                           IsKnownCreatureWeapon(a_w->GetFormID()));
        }

        // Same NonPlayable record flag (bit 2), for ARMOR. Creature/critter body
        // "skins" (e.g. More Nasty Critters' "BearBrownSoft") are ARMO records with
        // an armor RATING and a biped slot -- hidden OR regular -- but marked
        // Non-Playable because they're the creature's invisible body, not gear a
        // humanoid can wear. They passed the rating gate (marth: BearBrownSoft got
        // looted) since ArmorIsBetter only saw rating>0. This is the ARMO analog of
        // IsCreatureWeapon: a direct flag read, so the loot filter works off the DLL
        // alone, independent of the catalog's nonplayable exclusion.
        bool IsCreatureArmor(const RE::TESObjectARMO* a_armo) {
            return a_armo && (a_armo->GetFormFlags() & (1u << 2)) != 0;
        }

        // The ARMO currently WORN in a logical mage-apparel slot (MageClothingSlot
        // order: 0 head[head/hair/circlet], 1 body, 2 hands, 3 feet, 4 ring, 5
        // amulet), or nullptr if that slot is bare. Worker-safe read of a loaded
        // follower's worn gear (same GetWornArmor the loot judge already uses).
        RE::TESObjectARMO* WornInLogicalSlot(RE::Actor* a_follower, int a_logicalSlot) {
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            switch (a_logicalSlot) {
            case 0:
                if (auto* w = a_follower->GetWornArmor(Slot::kHead))    return w;
                if (auto* w = a_follower->GetWornArmor(Slot::kHair))    return w;
                if (auto* w = a_follower->GetWornArmor(Slot::kCirclet)) return w;
                return nullptr;
            case 1: return a_follower->GetWornArmor(Slot::kBody);
            case 2: return a_follower->GetWornArmor(Slot::kHands);
            case 3: return a_follower->GetWornArmor(Slot::kFeet);
            case 4: return a_follower->GetWornArmor(Slot::kRing);
            case 5: return a_follower->GetWornArmor(Slot::kAmulet);
            default: return nullptr;
            }
        }

        // ── v1.0.38 SAFE acquire+equip step (shared by loot AND the buy / owned-
        // upgrade pass) ─────────────────────────────────────────────────────────
        // Carries the same-role/slot worn item's MEO gems, optionally transfers the
        // item from a_src (loot), and equips IN PLACE via MainThread::Post +
        // ActorEquipManager::EquipObject -- NEVER DoReset3D (#62 beast-head fix); the
        // BeastHeadSink on the resulting TESEquipEvent handles any reattach. Then
        // queues the gem carry (fires when the piece becomes worn). Rules preserved
        // verbatim from the loot path:
        //   a_src == nullptr  -> the follower ALREADY OWNS the item (buy / owned
        //                        upgrade); no transfer.
        //   a_myWeap          -> currently-equipped weapon (the equip-IN-PLACE rule +
        //                        gem role match); weapons only go into a hand that
        //                        already holds the same role, else STOCK.
        //   a_forceStock      -> keep it in the pack, never into a hand (mage backup).
        // Returns true if it was actually equipped (vs stocked). Worker domain only.
        bool AcquireEquip(RE::Actor* a_follower, RE::TESBoundObject* a_item,
                          RE::TESObjectREFR* a_src, RE::TESObjectWEAP* a_myWeap,
                          bool a_forceStock) {
            if (!a_follower || !a_item) return false;

            // MEO gem transfer (#17): capture the OLD worn item this upgrade REPLACES
            // (base + instance uid) BEFORE the swap. CROSS-ROLE IS THE BUG (marth): a
            // new bow must never pull gems off the melee weapon. Same-role/slot only.
            RE::FormID    fromBase = 0;
            std::uint16_t fromUid  = 0;
            if (MEOBridge::Available()) {
                RE::TESBoundObject* oldItem = nullptr;
                if (auto* newWeap = a_item->As<RE::TESObjectWEAP>()) {
                    const auto     newWt   = newWeap->GetWeaponType();
                    const WepClass newRole = WeaponClassOf(newWt);
                    if (auto* eqW = a_myWeap) {
                        const auto eqWt = eqW->GetWeaponType();
                        const bool sameRole = (WeaponClassOf(eqWt) == newRole) &&
                            (newRole != WepClass::Ranged || eqWt == newWt);
                        if (sameRole) oldItem = eqW;
                    }
                } else if (auto* newArmo = a_item->As<RE::TESObjectARMO>()) {
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    static constexpr Slot kSlots[] = {
                        Slot::kHead, Slot::kHair, Slot::kCirclet,
                        Slot::kBody, Slot::kHands, Slot::kForearms,
                        Slot::kFeet, Slot::kCalves, Slot::kShield,
                        Slot::kRing, Slot::kAmulet,
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

            if (a_src)
                a_src->RemoveItem(a_item, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);

            // Equip-IN-PLACE: a weapon only enters a hand already holding the same
            // role (else stock for the combat gambit); armor always equips its slot.
            bool equipIt = !a_forceStock;
            if (!a_forceStock) {
                if (auto* nw = a_item->As<RE::TESObjectWEAP>(); nw && a_myWeap) {
                    const auto     newWt   = nw->GetWeaponType();
                    const WepClass newRole = WeaponClassOf(newWt);
                    equipIt = WeaponClassOf(a_myWeap->GetWeaponType()) == newRole &&
                              (newRole != WepClass::Ranged || a_myWeap->GetWeaponType() == newWt);
                }
            }
            if (equipIt) {
                // #62 EQUIP ON THE MAIN THREAD. Capture FormIDs (never the worker's
                // Actor*/item) and re-resolve on the frame that runs.
                const RE::FormID folID  = a_follower->GetFormID();
                const RE::FormID itemID = a_item->GetFormID();
                auto doEquip = [folID, itemID]() {
                    auto* fol  = RE::TESForm::LookupByID<RE::Actor>(folID);
                    auto* form = RE::TESForm::LookupByID(itemID);
                    auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
                    if (auto* eq = RE::ActorEquipManager::GetSingleton(); fol && item && eq)
                        eq->EquipObject(fol, item);
                };
                if (MainThread::IsInstalled()) MainThread::Post(doEquip);
                else                           doEquip();   // VR: pump is a no-op, keep the direct path
            }

            // Move the old piece's gems onto the new one when it becomes worn.
            // No-op if the old item had no gems (fromUid == 0) or MEO is absent.
            MEOBridge::QueueGemMove(a_follower, fromBase, fromUid, a_item->GetFormID());
            return equipIt;
        }

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek) {
            // Generalized by CATEGORY, never by item (§4.8.2). One better piece
            // per CALL (one action per tick, §4.3; StripCorpse drains by calling
            // again -- each take raises the inventory baseline). We TRANSFER,
            // then EQUIP IN PLACE (see the equipIt gate below) -- a real person
            // who finds a better cuirass puts it on, they do not just
            // carry it (marth: the follower is a thinking person). Safe here
            // because logistics runs OUT of combat, where Loadout is not holding
            // a hand for a cast, and MFO has no equip-event sink to loop on;
            // armor slots are independent of the (left-hand) spell hand.
            auto* equippedWeap = a_follower->GetEquippedObject(false);
            auto* myWeap       = equippedWeap ? equippedWeap->As<RE::TESObjectWEAP>() : nullptr;
            using WT = RE::WEAPON_TYPE;

            // THE ROLE WE LOOT/KEEP is a STABLE signal, never the momentarily
            // WIELDED weapon (#69, ComputeWeaponRoles above): a gambit-driven
            // role first, else whatever the follower actually CARRIES. Loot and
            // ShedOffRoleWeapon judge the SAME roles from the SAME helper, so
            // they can no longer disagree about what's "off-role" -- that
            // disagreement was the Gauldurbow bug (a custom follower's own
            // weapon, drawn only sometimes, read as off-role and got handed to
            // the player) and the hybrid-1h-gets-shed bug. WHAT gets force-
            // EQUIPPED over a drawn weapon is still decided below by the
            // equipIt gate, UNCHANGED -- roles only decide what's looted/kept.
            const bool wantsRanged = g_svc && TableHasAction(g_svc->combat(), Vocab::kActEquipRanged);
            const bool wantsMelee  = g_svc && TableHasAction(g_svc->combat(), Vocab::kActEquipMelee);
            const WeaponRoles roles = g_svc ? ComputeWeaponRoles(a_follower, *g_svc) : WeaponRoles{};

            // ── MAGIC LOADOUT (v1.0.29) ─────────────────────────────────────
            // Gambit-driven magic-user detection. A magic user gets two loot
            // paths: (1) SCHOOL-SCORED APPAREL on the mage slots (bypasses
            // ArmorIsBetter's rating>0 gate, which can never judge a robe); and
            // (2) ONE one-handed melee BACKUP (daggers by default) his AI draws
            // at zero magicka. Detected BEFORE the role below, because a pure
            // caster must be kept OUT of the general weapon-upgrade role.
            int castGambits = 0;
            RE::ActorValue school = RE::ActorValue::kNone;
            if (Config::g_magicLoadout.load() && g_svc)
                school = TargetMagicSchool(*g_svc, castGambits);
            const bool mageMode    = castGambits > 0;   // magic user AND master toggle on
            const bool daggersOnly = Config::g_mageDaggersOnly.load();
            // #21 bMageWearRobes (default ON): the mage school-clothing dress-up gate.
            // When OFF, a magic user is treated like any other class for APPAREL and
            // falls through to the plain rating armor judge below (marth). It gates
            // ONLY apparel selection -- the mage still keeps the backup-weapon /
            // no-melee-role contract (mageMode) and still buys/learns tomes.
            const bool useMageApparel = mageMode && Config::g_mageWearRobes.load();
            // #21 UNIFIED mage-apparel ranking (loot side; shared with the buy side).
            // Same MEO-aware model: value-primary when MEO carries gems, else school-
            // enchant primary; villain blacklist with a necromancer exception; all
            // clothing + jewelry slots (MageClothingSlot). See MageApparelBuyKey.
            const std::uint8_t mageTop2      = useMageApparel ? TopTwoSchoolMask(a_follower) : 0;
            const bool mageSchoolPrimary     = !MEOBridge::Available() || Config::g_mageApparelStrictSchool.load();
            const bool mageAllowVillain      = useMageApparel && g_svc && IsNecromancerFollower(*g_svc);

            // The MELEE class we loot/upgrade, or Other = "no melee role at all".
            // #69: ComputeWeaponRoles hands a role to ANY carried melee/ranged
            // weapon, but a non-battlemage magic user must NOT take the general
            // weapon-upgrade path -- his melee is the ONE-sidearm backup contract
            // (#52/#54: daggers-only, never an armory), not a role, and he loots
            // no bow either. Only an explicit equip-melee/equip-ranged gambit (a
            // battlemage/spellbow) earns him the real role. Pre-#69 this fell out
            // for free -- the role was WIELD-based and a caster wields spells/
            // staff, never his carried sidearm -- so the stable carries-based
            // signal has to say it explicitly. (Shed still KEEPS his carried gear;
            // this only bars LOOTING new weapon upgrades for a pure caster.)
            // marth: the BASE CLASS decides the melee-loot contract, NOT the mere
            // presence of an equip-melee gambit. A base MAGE (Cast class, #65
            // combatClassOverride==3) keeps the daggers-only sidearm even when he
            // melees via a gambit -- "a base mage who melees", not a spellsword.
            // A base WARRIOR/ARCHER who casts (Melee/Ranged class == a spellsword)
            // earns the full weapon-upgrade role. Auto (0, no explicit class)
            // keeps the old gambit heuristic (mageMode && !wantsMelee). Ordinals
            // match CombatStyle::Stance by construction (State.h:82).
            const std::uint8_t baseClass   = g_svc ? g_svc->combatClassOverride : 0;
            const bool baseCaster          = baseClass == 3;
            const bool baseWeaponUser      = baseClass == 1 || baseClass == 2;
            const WepClass meleeTargetClass =
                (mageMode && !baseWeaponUser && (baseCaster || !wantsMelee))
                    ? WepClass::Other : roles.melee;
            // Ranged is a primary if a gambit wants it OR they carry one.
            const bool doRanged =
                (mageMode && !wantsRanged) ? false           : roles.doRanged;
            const bool wantBackup  = mageMode && meleeTargetClass == WepClass::Other;

            // Baseline the MELEE/RANGED upgrade must beat: the best in-role
            // weapon anywhere in the follower's OWN INVENTORY (the equipped one
            // is part of it). Equipped-only was the residual thrash hole: a
            // follower HOLDING HIS BOW with an equip-melee gambit baselined
            // melee at 0, so every corpse's iron dagger "beat" the good sword
            // already in his pack -- looted a duplicate and force-equipped it
            // over the bow, corpse after corpse (the in-AND-out-of-combat half
            // of "Erik switches weapons for no reason": the combat gambit put
            // the bow back, the next corpse knocked it out again). BOW vs
            // CROSSBOW is decided by ComputeWeaponRoles now (#69, shared with
            // the shed side); this pass only baselines the follower's best
            // ALREADY-CARRIED weapon of that kind. Creature/excluded weapons
            // are unusable gear -- they never set a baseline.
            const bool wantCrossbow = roles.wantCrossbow;
            std::uint16_t baseDmg      = 0;
            std::uint16_t myRangedDmg  = 0;
            std::uint16_t myBackupDmg  = 0;       // best qualifying sidearm the mage already OWNS (not the wielded hand)
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || IsCreatureWeapon(w) || Catalog::IsExcluded(obj->GetFormID())) continue;
                if (meleeTargetClass != WepClass::Other &&
                    WeaponClassOf(w->GetWeaponType()) == meleeTargetClass)
                    baseDmg = std::max(baseDmg, w->GetAttackDamage());
                // ONE sidearm is the mage-backup contract: he restocks only
                // when he carries NONE, never accumulates an armory (creature/
                // excluded weapons already skipped above -- an unusable weapon
                // is not a backup).
                if (wantBackup && WeaponClassOf(w->GetWeaponType()) == WepClass::OneHand &&
                    (!daggersOnly || w->GetWeaponType() == WT::kOneHandDagger))
                    myBackupDmg = std::max(myBackupDmg, w->GetAttackDamage());
                if (doRanged && w->GetWeaponType() == (wantCrossbow ? WT::kCrossbow : WT::kBow))
                    myRangedDmg = std::max(myRangedDmg, w->GetAttackDamage());
            }

            RE::TESBoundObject* bestArmor     = nullptr;
            float               bestArmorRat  = 0.0f;   // best-first, like bestWeapDmg (marth's rule)
            RE::TESBoundObject* bestWeap      = nullptr;
            std::uint16_t       bestWeapDmg   = baseDmg;
            RE::TESBoundObject* bestRanged    = nullptr;
            std::uint16_t       bestRangedDmg = myRangedDmg;
            RE::TESBoundObject* bestMage      = nullptr;   // clothing/jewelry apparel (magic user) -- unified MEO-aware judge
            int                 bestMageTier  = 0;         // 2 top-2 school, 1 plain, 0 off-school (see MageApparelBuyKey)
            std::int32_t        bestMageMetric= 0;         // value (+fortify mag for a school piece); higher = fancier
            RE::TESBoundObject* bestBackup    = nullptr;   // the mage's melee sidearm (upgrade past best owned)
            std::uint16_t       bestBackupDmg = myBackupDmg;   // beat his best-OWNED sidearm, not the wielded hand

            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                // NEVER-LOOT: the catalog marks quest items, artifacts/unique
                // enchantments, and scripted/no-drop gear as off-limits -- leave
                // them for the player (marth). Fail-open with no patcher run.
                if (Catalog::IsExcluded(obj->GetFormID())) continue;

                if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                    // Never loot a NON-PLAYABLE creature "skin" armor (MNC's
                    // BearBrownSoft et al.): armor-rated but the creature's own
                    // invisible body, useless/broken on a follower. Same flag as
                    // IsCreatureWeapon -- this is what BearBrownSoft slipped past.
                    if (IsCreatureArmor(armo)) continue;
                    // #61 FASHIONRIM: armor-only dolls mode -- skip EVERY armor
                    // candidate (plain-rated AND mage school-robe) so MFO never
                    // loots or swaps a follower's armor; the player dresses them
                    // by hand. Weapons fall to the else-branch below, untouched.
                    if (Config::g_dollsMode.load()) continue;
                    // A SHIELD needs a free off-hand: only a ONE-HAND melee role can
                    // use one. A 2H, ranged, or no-melee-role follower has no hand
                    // for it -- dead weight (marth: Farkas, a two-hander, picked up
                    // a shield).
                    const bool isShield = (static_cast<std::uint32_t>(armo->GetSlotMask())
                        & static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kShield)) != 0;
                    const bool shieldUseless = isShield && !(meleeTargetClass == WepClass::OneHand && !doRanged);
                    // MAGE APPAREL + JEWELRY (#21 unified with the buy side): a magic
                    // user's dress-up is judged by the shared MEO-aware ranking
                    // (MageApparelBuyKey: value-primary with MEO, else school-enchant
                    // primary), across ALL clothing slots AND jewelry (ring/amulet),
                    // never by armor rating. A candidate must BEAT what he currently
                    // WEARS in that logical slot, then beat the running best pick.
                    // Shields are skipped -- rated armor, never dress-up.
                    if (useMageApparel && !isShield) {
                        LogMageApparelDiag(armo, school);   // one dump per form+school (deduped inside)
                        int cTier = 0; std::int32_t cMetric = 0;
                        const int slot = MageClothingSlot(armo);
                        // CLOTHING slots only here (0 head .. 3 feet). JEWELRY (ring/
                        // amulet, slots 4/5) is NOT acquired via the Equipment loot
                        // category -- it stays on the Valuables tier (LootJewelry, its
                        // stricter dibs preserved). EquipBestOwnedGear still WEARS the
                        // best owned ring/amulet (looted-as-valuable, bought, handed).
                        if (slot >= 0 && slot <= 3 &&
                            MageApparelBuyKey(armo, mageTop2, mageSchoolPrimary, mageAllowVillain, cTier, cMetric)) {
                            // Beats what he wears in this slot?
                            int wTier = 0; std::int32_t wMetric = 0;
                            if (auto* worn = WornInLogicalSlot(a_follower, slot))
                                MageApparelBuyKey(worn, mageTop2, mageSchoolPrimary, /*allowVillain*/true, wTier, wMetric);
                            const bool beatsWorn = cTier > wTier || (cTier == wTier && cMetric > wMetric);
                            const bool beatsBest = cTier > bestMageTier ||
                                                   (cTier == bestMageTier && cMetric > bestMageMetric);
                            if (beatsWorn && beatsBest) {
                                bestMageTier   = cTier;
                                bestMageMetric = cMetric;
                                bestMage       = obj;
                            }
                        }
                    }
                    // The PLAIN rating path is for NON-magic users ONLY
                    // (marth, v1.0.31: PURE CASTER). v1.0.29 ran it for mages
                    // too and let their "no school match but rated" pieces
                    // fall through to it -- which is exactly how Marcurio, a
                    // detected Destruction user, looted a Dwarven Heavy
                    // Cuirass and Chitin Heavy Boots by raw rating. Skipping
                    // the whole path means a magic user can never LOOT armor
                    // at all; armor he already wears stays on (nothing here
                    // strips gear) until a school robe displaces it on equip.
                    // This also retires the WouldStripSchoolGear guard: with
                    // no rating path for mages there is nothing left to
                    // thrash against their robes.
                    // Best-first: among the armour upgrades this body offers, keep the
                    // HIGHEST-rated (not the first enumerated), so a carry-weight cutoff
                    // can't strand the actually-best piece.
                    if (!useMageApparel &&
                        !shieldUseless && ArmorIsBetter(a_follower, armo) &&
                        armo->GetArmorRating() > bestArmorRat &&
                        !CarriesSlotArmorAtLeast(a_follower, armo)) {   // #3: don't re-take/equip a worse same-slot piece already in the pack (strip double-take)
                        bestArmorRat = armo->GetArmorRating();
                        bestArmor    = obj;
                    }
                } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    if (IsCreatureWeapon(weap)) continue;   // never equip automaton/creature gear
                    const WepClass wc = WeaponClassOf(weap->GetWeaponType());
                    // MELEE upgrade: ONLY the target melee class -- the equip-melee
                    // gambit's best-skill class, or the class they already wield. Never
                    // cross-class, and never skill-forced onto a ranged user (that was
                    // the thrash bug). meleeTargetClass == Other means no melee role.
                    if (meleeTargetClass != WepClass::Other && wc == meleeTargetClass &&
                        weap->GetAttackDamage() > bestWeapDmg) {
                        bestWeapDmg = weap->GetAttackDamage();
                        bestWeap    = obj;
                    }
                    // Ranged pickup -- ONLY the follower's kind (bow XOR crossbow).
                    if (doRanged) {
                        const auto wt = weap->GetWeaponType();
                        const bool kindMatch = wantCrossbow ? (wt == WT::kCrossbow) : (wt == WT::kBow);
                        if (kindMatch && weap->GetAttackDamage() > bestRangedDmg) {
                            bestRangedDmg = weap->GetAttackDamage();
                            bestRanged    = obj;
                        }
                    }
                    // MAGE BACKUP (v1.0.29): the sidearm a caster's own AI draws
                    // when his magicka is gone. Daggers only by default
                    // (bMageDaggersOnly); the toggle opens it to the best of any
                    // one-hander. Baselined on his BEST-OWNED sidearm (bestBackupDmg
                    // = myBackupDmg), not the momentarily wielded hand: a caster
                    // dual-wielding SPELLS reads an empty weapon hand, so the old
                    // carries-none gate skipped every better dagger while he cast
                    // (marth field: "ignores better daggers while casting"). Now he
                    // restocks when he carries none (myBackupDmg==0) AND upgrades to
                    // a STRICTLY better sidearm -- one at a time (§4.3), and NEVER
                    // force-equipped (equipIt=false below), so it stays a stocked
                    // backup he switches to, not an armory or a per-corpse equip thrash.
                    if (wantBackup && wc == WepClass::OneHand &&
                        (!daggersOnly || weap->GetWeaponType() == WT::kOneHandDagger) &&
                        weap->GetAttackDamage() > bestBackupDmg) {
                        bestBackupDmg = weap->GetAttackDamage();
                        bestBackup    = obj;
                    }
                }
            }

            // Prefer the in-class weapon upgrade; then a ranged weapon they need
            // for their equip-ranged gambit; then the mage's missing sidearm
            // (safety before wardrobe); then school apparel over plain armor
            // (the point of the magic loadout). One item this tick (§4.3).
            RE::TESBoundObject* best = bestWeap   ? bestWeap
                                     : bestRanged ? bestRanged
                                     : bestBackup ? bestBackup
                                     : bestMage   ? bestMage
                                                  : bestArmor;
            // Peek: an upgrade exists AND the follower can actually carry it. Without
            // the weight gate an overencumbered follower walks a whole excursion leg,
            // takes nothing at arrival (the real take IS weight-gated below), and the
            // corpse gets marked DONE -- a wasted trip.
            if (a_peek) return best != nullptr && FitsCarryWeight(a_follower, best->GetWeight());
            if (!best) return false;
            if (!FitsCarryWeight(a_follower, best->GetWeight())) return false;

            // ACQUIRE + EQUIP through the shared v1.0.38 safe step: transfers from
            // a_src, captures + carries MEO gems, equips IN PLACE on the main thread
            // (MainThread::Post EquipObject, never DoReset3D -- #62), queues the gem
            // move. The mage BACKUP stays STOCK-ONLY (a caster's hand belongs to his
            // spells; his own AI draws the sidearm at zero magicka). The buy / owned-
            // upgrade pass calls the SAME AcquireEquip with a_src=nullptr.
            const bool equipped = AcquireEquip(a_follower, best, a_src, myWeap, best == bestBackup);

            // [equip] DIAGNOSTIC: log WHAT we put on, over WHAT, and the reasoning.
            if (auto* nw = best->As<RE::TESObjectWEAP>()) {
                spdlog::info("[equip] {:08X}: LOOT-{} weapon '{}' dmg={} class={} <- held '{}' "
                             "dmg={} class={} | meleeTgt={} wantsMelee={} wantsRanged={} baseDmg={}",
                             a_follower->GetFormID(), equipped ? "EQUIP" : "STOCK",
                             nw->GetFullName() ? nw->GetFullName() : "?", nw->GetAttackDamage(),
                             static_cast<int>(WeaponClassOf(nw->GetWeaponType())),
                             myWeap && myWeap->GetFullName() ? myWeap->GetFullName() : "(none)",
                             myWeap ? myWeap->GetAttackDamage() : 0,
                             myWeap ? static_cast<int>(WeaponClassOf(myWeap->GetWeaponType())) : -1,
                             static_cast<int>(meleeTargetClass), wantsMelee, wantsRanged, baseDmg);
            } else {
                spdlog::info("[equip] {:08X}: LOOT armor/apparel '{}' -> equip {}", a_follower->GetFormID(),
                             best->As<RE::TESFullName>() && best->As<RE::TESFullName>()->GetFullName()
                                 ? best->As<RE::TESFullName>()->GetFullName() : "?",
                             MainThread::IsInstalled() ? "queued to main thread" : "direct (VR/no-pump)");
            }

            // MAGIC-LOADOUT diagnostics: WHY the mage item won (logged on a TAKE only).
            if (best == bestMage || best == bestBackup) {
                spdlog::info("[loot] {:08X} '{}' magic-user: target school {} (from {} cast gambit(s))",
                             a_follower->GetFormID(),
                             a_follower->GetName() ? a_follower->GetName() : "?",
                             SchoolName(school), castGambits);
            }
            if (best == bestMage) {
                spdlog::info("[loot] apparel {:08X} '{}' tier={} metric={} (schoolPrimary={}) -> best",
                             best->GetFormID(), best->GetName() ? best->GetName() : "?",
                             bestMageTier, bestMageMetric, mageSchoolPrimary);
            }
            if (best == bestBackup) {
                auto* mw = best->As<RE::TESObjectWEAP>();
                spdlog::info("[loot] mage backup {} {:08X} '{}' dmg={} -- stocked; his own AI draws it when the magicka runs out",
                             daggersOnly ? "dagger" : "1h", best->GetFormID(),
                             best->GetName() ? best->GetName() : "?",
                             mw ? mw->GetAttackDamage() : 0);
            }
            return true;
        }

        // COIN-PURSE / LOOSE-COIN detection (marth field: coin purses count as
        // gold). LOAD-ORDER-AGNOSTIC via OCF (Object Categorization Framework,
        // this list's curated item classifier): OCF_MiscTreasure_Coinpurse marks
        // bags that yield gold (vanilla TGCoinpurse*, modded purses),
        // OCF_MiscTreasure_Coin marks loose septims/coins. KID mints these keywords
        // at runtime with no stable FormID AND no editorID->form reverse entry, so
        // the item is matched by scanning ITS OWN keywords' editorID strings (the
        // forward read), never a reverse LookupByEditorID<BGSKeyword> (that returned
        // null here and missed every coin purse -- deck "coinpurse-kw=null"). A coin
        // purse is a MISC object whose gold is granted by a PICKUP SCRIPT (e.g.
        // TGCoinpurseScript) only once the PHYSICAL object reaches the player -- so
        // MFO loots the object itself (held for the player like any valuable,
        // delivered on trade) and NEVER value-credits it. Requiem's
        // REQ_GoldWeightDisplayPurse carries the coinpurse keyword but is a
        // weightless gold-weight DISPLAY proxy, not loot -- excluded by FormID
        // (resolved once by editorID). FAIL-CLOSED: an item with no OCF coin keyword
        // is not coin loot, so only hardcoded Gold001 is taken, exactly as before.
        bool IsCoinLoot(RE::TESBoundObject* a_obj) {
            // #coinfix: match the ITEM'S OWN keyword EDITORIDS, never a reverse
            // LookupByEditorID<BGSKeyword> on the OCF keyword. KID mints
            // OCF_MiscTreasure_Coinpurse/_Coin at runtime with no stable FormID and
            // NO editorID→form reverse entry in this modlist, so the old lookup
            // returned null and HasKeyword(null) never matched -- coin purses were
            // missed (deck: "coinpurse-kw=null"). The FORWARD read works: it is the
            // SAME kw->GetFormEditorID() the loot logging (KeywordCsv) already prints.
            static const RE::FormID s_proxy = [] {
                auto* px = RE::TESForm::LookupByEditorID("REQ_GoldWeightDisplayPurse");
                const RE::FormID id = px ? px->GetFormID() : 0;
                spdlog::info("[loot] coin detection: editorID-scan active (item keywords "
                             "matched against OCF_MiscTreasure_Coinpurse / _Coin); "
                             "Requiem display proxy {}", id ? "excluded" : "absent");
                return id;
            }();
            if (!a_obj) return false;
            if (s_proxy && a_obj->GetFormID() == s_proxy) return false;   // Requiem display proxy, not loot
            auto* kwf = a_obj->As<RE::BGSKeywordForm>();
            if (!kwf || !kwf->keywords) return false;
            for (std::uint32_t i = 0; i < kwf->numKeywords; ++i) {
                const auto* kw = kwf->keywords[i];
                const char* ed = kw ? kw->GetFormEditorID() : nullptr;   // same forward reader as KeywordCsv
                if (!ed) continue;
                if (std::string_view(ed) == "OCF_MiscTreasure_Coinpurse" ||   // gold bag
                    std::string_view(ed) == "OCF_MiscTreasure_Coin")          // loose coins
                    return true;
            }
            return false;
        }

        // Take all the gold on a corpse/container. Gold001 is the one hardcoded
        // FormID in the game (0x0000000F, Skyrim.esm); coin PURSES and loose
        // coins are matched by OCF keyword (IsCoinLoot) so the take is not blind
        // to modded currency. Weightless / near-weightless, so no carry-weight
        // gate; nothing to equip. Held for the player, who gets it back by trading
        // -- which is why gold WAITS out first dibs (below), same as gear: you
        // want first pick of the coin. Collect-then-transfer (RemoveItem mutates
        // the inventory map, so never transfer mid-iteration) now that more than
        // one stack can match.
        bool LootGold(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            constexpr RE::FormID kGold001 = 0x0000000F;
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() != kGold001 && !IsCoinLoot(obj)) continue;
                if (a_peek) return true;
                takes.push_back({ obj, data.first });
            }
            if (a_peek || takes.empty()) return false;
            for (const auto& t : takes) {
                // Log a matched coin PURSE/coin once per form (never plain Gold001,
                // which would flood): surfaces exactly what the OCF rule caught.
                if (t.obj->GetFormID() != kGold001) {
                    static std::unordered_set<RE::FormID> s_seen;
                    if (s_seen.insert(t.obj->GetFormID()).second)
                        spdlog::info("[loot] coin item {:08X} '{}' x{} -> follower (OCF-classified; "
                                     "converts to gold when it reaches the player)",
                                     t.obj->GetFormID(),
                                     t.obj->GetName() ? t.obj->GetName() : "?", t.count);
                }
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
            }
            return true;
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

        // po3's Papyrus Extender -- the economy's merchant enumeration
        // (AddAllItemsToArray) lives there. Absent -> the buy phase can't work, so we
        // don't dispatch the econ scan at all (no silent Papyrus failures). Logged once.
        bool Po3Present() {
            static const bool present = [] {
                const bool p = ::GetModuleHandleA("po3_papyrusextender.dll") != nullptr;
                if (!p) spdlog::info("[econ] po3 Papyrus Extender not found -- follower economy disabled "
                                     "(install powerofthree's Papyrus Extender to use it)");
                return p;
            }();
            return present;
        }

        // PLAYER-HOME gate. A follower ransacking your own house reads as theft,
        // not tidying, so looting is suppressed wherever the player's current
        // LOCATION carries vanilla LocTypeHouse (0x01CB85 in Skyrim.esm) --
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

        // #66: is a_ref inside the player's OWN space -- a location tagged
        // LocTypePlayerHouse, or a cell owned by the player / PlayerFaction? This
        // is the signal that catches LOTD museum drop-off / storage crates. Those
        // crates are UNOWNED at the REF level (so the GetOwner() bar below misses
        // them) and carry NO container keyword/faction -- a headless plugin parse
        // of Legacy of the Dragonborn confirmed the only stable signal lives on
        // the museum CELL/LOCATION: every DBM museum/storeroom/safehouse/airship/
        // Deepholme/field-station cell carries LocTypePlayerHouse and is
        // PlayerFaction-owned. Checking that here ALSO covers every vanilla/
        // Hearthfire/mod player home -- semantically the same "don't ransack my
        // storage" case -- with no LOTD dependency and no hardcoded container
        // FormID. Pure Skyrim.esm forms (stable index 00): LocTypePlayerHouse
        // 000FC1A3, PlayerFaction 000DB1, Player 00000007. (NB: this is the
        // CORRECT player-home keyword; InPlayerHome above uses the broader
        // LocTypeHouse 01CB85 for its coarse player-location gate -- left as-is.)
        bool RefInPlayerStorage(RE::TESObjectREFR* a_ref) {
            if (!a_ref) return false;
            static RE::BGSKeyword* kwPlayerHouse =
                RE::TESForm::LookupByID<RE::BGSKeyword>(0x000FC1A3);   // LocTypePlayerHouse
            if (kwPlayerHouse) {
                int guard = 0;
                for (auto* loc = a_ref->GetCurrentLocation(); loc && guard < 8;
                     loc = loc->parentLoc, ++guard)
                    if (loc->HasKeyword(kwPlayerHouse)) return true;
            }
            // Cell-level ownership: a player-owned cell with no location tag (LOTD's
            // haunted safehouse copy; some player-home mods). Player 0x7 / PlayerFaction.
            if (auto* cell = a_ref->GetParentCell()) {
                if (auto* owner = cell->GetOwner()) {
                    const auto oid = owner->GetFormID();
                    if (oid == 0x00000007 || oid == 0x000DB1) return true;
                }
            }
            return false;
        }

        // #66 (the real bug -- marth: the drop-off boxes in TOWNS and INNS, not the
        // museum). Legacy of the Dragonborn scatters "drop-off" / income / sell /
        // shipment containers across public cities and inns so you can deposit
        // museum items on the go. They are ref-UNOWNED and carry NO keyword/faction
        // (headless plugin parse), so neither the GetOwner() bar nor the
        // LocTypePlayerHouse check above catches them -- and they sit in ordinary
        // town/inn cells, so they must be skipped UNCONDITIONALLY (not behind the
        // player-home toggle). The only stable signal is the container BASE form.
        // These are a handful of container TYPES (not placed instances), resolved
        // once by local FormID within LegacyoftheDragonborn.esm; if LOTD is not in
        // the load order every lookup returns null and the set is empty -> inert.
        bool IsLOTDDropOff(RE::TESObjectREFR* a_ref) {
            static const std::unordered_set<RE::FormID> s_bases = [] {
                std::unordered_set<RE::FormID> s;
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    // DBM_AutoSortDropOff (Display Drop-off, the town/inn box),
                    // DBMMuseumShipmentsCrateIncoming, DBM_SalesBox (Sales Income),
                    // DBM_Incomebox (Donations), DBM_SellStorage.
                    for (const RE::FormID local :
                         { 0x07EEFDu, 0x1772A6u, 0x166349u, 0x0BE533u, 0x11CC99u }) {
                        if (auto* f = dh->LookupForm<RE::TESObjectCONT>(local, "LegacyoftheDragonborn.esm"))
                            s.insert(f->GetFormID());
                    }
                }
                if (!s.empty())
                    spdlog::info("[loot] LOTD drop-off/storage guard active ({} container bases)", s.size());
                return s;
            }();
            if (s_bases.empty()) return false;
            auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
            return base && s_bases.count(base->GetFormID()) != 0;
        }
        bool PlayerIsConsidering(RE::FormID a_sourceID) {
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) return true;   // vanilla menu
            if (QuickLootPresent() && a_sourceID != 0 &&
                Probe::CrosshairTarget() == a_sourceID) return true;               // QuickLoot HUD
            return false;
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

        // CLAIM-AND-RELEASE, factored out of LootNearby's scan so BOTH the scan and
        // the arm's-reach StripCorpse honour the SAME dibs (a free-tier arrival strip
        // must not snatch a fresh kill's enchanted sword + gold ahead of the player's
        // grace -- the #2 bug). Accrues the player's "chance" (near/facing/departed)
        // on g_claim exactly as the inline scan did, then applies the per-tier rule:
        //   Free (arrows/bolts/potions/lockpicks): released at once -- the follower's
        //     own restock, nobody competes.
        //   Gear (equipment): a short anti-snatch grace, or rejection.
        //   Valuables (gold/jewellery/soul gems): rejection, fair-chance, departure,
        //     or the never-came-near abandon backstop.
        bool TierReleased(Category a_cat, RE::TESObjectREFR* a_src,
                          const RE::NiPoint3& a_playerPos, Clock::time_point a_now) {
            if (a_cat == Category::Arrows || a_cat == Category::Bolts ||
                a_cat == Category::Potions || a_cat == Category::Lockpicks)
                return true;

            const RE::FormID   srcId  = a_src->GetFormID();
            const RE::NiPoint3 srcPos = a_src->GetPosition();
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto& cl = g_claim[srcId];
            if (cl.seen.time_since_epoch().count() == 0) { cl.seen = a_now; EvictOldest(g_claim); }

            // Accrue the player's REAL elapsed "chance" since this source last accrued
            // (capped, so N followers in one ~1 s window can't multiply the clock).
            {
                const float dt = cl.lastAccrue.time_since_epoch().count() == 0
                    ? kTickSecs
                    : std::min(std::chrono::duration<float>(a_now - cl.lastAccrue).count(), 2.0f);
                cl.lastAccrue = a_now;
                const float pdist = a_playerPos.GetDistance(srcPos);
                if (dt > 0.0f && pdist <= Config::g_chanceRadius.load()) {
                    cl.everNear = true;
                    cl.farSince = {};
                    if (PlayerIsConsidering(srcId))    cl.nearSecs += dt * 3.0f;
                    else if (PlayerFacing(pc, srcPos)) cl.nearSecs += dt;
                } else if (pdist > Config::g_departRadius.load() &&
                           cl.farSince.time_since_epoch().count() == 0) {
                    cl.farSince = a_now;
                }
            }

            const bool rejected = ClaimRejected(srcId, srcPos, a_playerPos, a_now);
            if (a_cat == Category::Equipment) {          // Gear tier
                return rejected ||
                       std::chrono::duration<float>(a_now - cl.seen).count()
                           >= Config::g_firstDibsDelay.load();
            }
            // Gold / Jewelry / SoulGems -> Valuables tier.
            const bool departed = cl.everNear &&
                cl.farSince.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(a_now - cl.farSince).count() >= kDepartRelease;
            return rejected || departed ||
                   cl.nearSecs >= Config::g_fairChance.load() ||
                   (!cl.everNear &&
                    std::chrono::duration<float>(a_now - cl.seen).count()
                        >= Config::g_abandonDelay.load());
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

        // Hold looting only when the player is ACTIVELY stealthing -- sneaking AND
        // (weapon drawn or in combat) -- not merely crouch-walking, else a stealth
        // build that sneaks the whole dungeon never loots at all (Fable, P4).
        bool PlayerActivelyStealthing() {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc || !pc->IsSneaking()) return false;
            if (pc->IsInCombat()) return true;
            auto* as = pc->AsActorState();
            return as && as->GetWeaponState() >= RE::WEAPON_STATE::kDrawing;   // weapon out/coming out
        }

        bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now,
                        RE::ActorValue a_potionWant,
                        LootMode a_mode) {
            // §22g ABSOLUTE BAR, ahead of every delay and waiver: never mutate a
            // container while the player has ANY container menu open -- it breaks
            // the vanilla menu building its list from that container (MEO m19e).
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return false;
            }
            // BEHAVIOUR LAYER: hold looting while the player is ACTIVELY stealthing,
            // not merely crouched. A stealth build spends most of the dungeon in
            // sneak; a blanket block there means logistics looting effectively never
            // happens (Fable). Hold only when sneaking coincides with a drawn weapon
            // or combat -- the moments a follower breaking off would actually blow it.
            if (PlayerActivelyStealthing()) {
                static Clock::time_point s_nextSneakLog{};
                if (a_now >= s_nextSneakLog) {
                    s_nextSneakLog = a_now + std::chrono::seconds(10);
                    spdlog::info("[loot] {:08X} holding -- player stealthing (sneak + armed/combat)",
                                 a_follower->GetFormID());
                }
                return false;
            }
            // PLAYER HOME: don't loot your own house unless opted in (default OFF).
            if (!Config::g_lootInPlayerHomes.load() && InPlayerHome())
                return false;
            // WALK ACTOR-ANCHORED CELLS, NOT TES::ForEachReferenceInRange.
            // crash4 (2026-07-22, exterior Wilderness): TES::ForEachReferenceInRange
            // ends its exterior branch with `worldSpace->GetSkyCell()`, and
            // TES::worldSpace was a TORN pointer (0x450FE000_45242000 -- two
            // mismatched 32-bit halves) because the engine was mid worldspace/cell
            // stream. It chases three engine-owned pointers (gridCells, worldSpace,
            // skycell) that churn during a transition. A ref's parent cell, when
            // ATTACHED, iterates only its own reference list (no worldspace deref
            // at all -- see TESObjectCELL::ForEachReferenceInRange). The IsAttached
            // gate also skips the walk outright during a transition, which is
            // exactly when those pointers are unstable.
            //
            // RC#4 (marth: "skipped a second gold pile on the SAME table"): a
            // single-cell walk goes BLIND across exterior cell borders -- the deck
            // scan's ref count swung 1394 -> 132 -> 84 as the follower crossed
            // boundaries, and the table's other pile/potions sat unseen in the
            // neighbour cell. So scan up to three ACTOR-ANCHORED attached cells --
            // the follower's, the player's, and the live travel TARGET's -- each
            // reached through a ref we already hold (same safety class as before;
            // still zero TES/worldspace derefs). The target's cell is the one that
            // makes an excursion loot a table OUT: once he walks at pile 1, pile 2
            // beside it becomes visible even from another cell.
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

            // The scanned cell set (see the crash4/RC#4 note above): follower's
            // cell always; player's and live travel-target's when attached and
            // distinct. All anchored to refs in hand -- never TES globals.
            RE::TESObjectCELL* cells[3] = { cell, nullptr, nullptr };
            int nCells = 1;
            auto addCell = [&](RE::TESObjectREFR* a_anchor) {
                if (!a_anchor) return;
                auto* c = a_anchor->GetParentCell();
                if (!c || !c->IsAttached()) return;
                for (int i = 0; i < nCells; ++i)
                    if (cells[i] == c) return;   // dedupe (interiors: all one cell)
                if (nCells < 3) cells[nCells++] = c;
            };
            addCell(pc);
            if (auto* sl = SlotOf(a_follower->GetFormID())) {
                auto tp = sl->target.get();
                addCell(tp.get());
            }

            auto scanOne =
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
                            } else if (a_cat == Category::Potions) {
                                // Loose potion on a surface (marth field: Stenvar
                                // walked past a health potion on a table). Same
                                // route-2b acquire as loose gold -- walk to it,
                                // ActivateRef at arrival, never an in-place
                                // PickUpObject. The ownership gate below
                                // (ref->GetOwner) still skips inn/shop/home stock.
                                // Match LootPotions' item test (want + low-power
                                // floor) so a loose potion qualifies iff the loot
                                // would take it.
                                if (auto* alc = base->As<RE::AlchemyItem>()) {
                                    const bool wantOk =
                                        a_potionWant == RE::ActorValue::kNone
                                            ? IsDrinkablePotion(alc)
                                            : (PotionRestores(alc) == a_potionWant);
                                    const float fl  = PotionLootFloor();
                                    const float mag = PotionMagnitude(alc);
                                    if (wantOk && !(fl > 0.0f && mag > 0.0f && mag < fl))
                                        lootable = loose = true;
                                }
                            }
                        }
                    }
                    if (!lootable) return RE::BSContainer::ForEachResult::kContinue;
                    ++dLootable;

                    // #66 (the reported bug): LOTD drop-off / income / sell boxes
                    // in TOWNS and INNS -- ALWAYS skipped, regardless of the
                    // player-home toggle (they sit in ordinary public cells, so
                    // they'd otherwise be fair game). Matched by container base
                    // (IsLOTDDropOff); inert if LOTD is not installed.
                    if (!loose && IsLOTDDropOff(ref)) {
                        ++dOffLimits;
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // #66 (home storage): also skip containers in the player's OWN
                    // space -- LOTD museum halls + every vanilla/Hearthfire/mod
                    // player home (RefInPlayerStorage: LocTypePlayerHouse location
                    // or player/PlayerFaction-owned cell). CONTAINER-only: a corpse
                    // in a home is still fair loot, and the ref-level GetOwner() bar
                    // below never fires on these (ref-unowned). This half IS gated
                    // by bLootInPlayerHomes, so a player who wants followers tidying
                    // their own chests still can (the town/inn boxes above are not).
                    if (!loose && !Config::g_lootInPlayerHomes.load()) {
                        auto* cbase = ref->GetBaseObject();
                        if (cbase && cbase->Is(RE::FormType::Container) && RefInPlayerStorage(ref)) {
                            ++dOffLimits;
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                    }

                    // OWNERSHIP IS ABSOLUTE (#22e). IsOffLimits() is ONLY a crime
                    // check (IsCrimeToActivate) -- and taking from a PLAYER-owned
                    // chest is not a crime, so it would sail through and let a
                    // follower drain the player's own storage. Check GetOwner()
                    // first (any explicit owner, including the player), then keep
                    // IsOffLimits() as the second bar for cell-owned/no-owner
                    // crime cases. No delay or waiver overrides this.
                    if (auto* owner = ref->GetOwner()) {
                        ++dOwned;
                        // REGRESSION GUARD [ownprobe]: the soak proved every owned drop
                        // was a shop/faction container -- but a KILLED corpse must never
                        // read as owned (that would silently eat combat loot). Log ONLY
                        // that anomaly (dead actor), rate-limited -- silent normally.
                        if (auto* act = ref->As<RE::Actor>(); act && act->IsDead()) {
                            static std::unordered_map<RE::FormID, Clock::time_point> s_nextOP;
                            auto& op = s_nextOP[ref->GetFormID()];
                            if (op.time_since_epoch().count() == 0 || a_now >= op) {
                                op = a_now + std::chrono::seconds(20);
                                spdlog::info("[ownprobe] {:08X} CORPSE {:08X} dropped as OWNED (owner {:08X}) "
                                             "-- ownership gate may be eating combat loot",
                                             a_follower->GetFormID(), ref->GetFormID(), owner->GetFormID());
                            }
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
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
                    //
                    // EXCEPT what is already at the follower's OWN feet (#30): the
                    // leash bounds how far he TRAVELS from you, not what he grabs
                    // where he already stands. A follower out looting a corpse that
                    // WAS in leash would otherwise ignore a potion beside it that
                    // sits a hair past the player-bubble -- the "found the potion
                    // right in front of him only after several runs" report. If
                    // he's within arrival distance of the ref, no excursion is
                    // needed, so the leash does not apply.
                    if (playerPos.GetDistance(ref->GetPosition()) > leash &&
                        a_follower->GetPosition().GetDistance(ref->GetPosition()) > kArrivalDist) {
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

                    // CLAIM-AND-RELEASE (dibs redesign) -- factored into TierReleased
                    // so the arm's-reach StripCorpse honours the SAME per-tier grace
                    // (#2). Eligibility is per value-tier and releases on EVIDENCE
                    // ABOUT THE PLAYER (near/facing/departed/abandon), not a wall
                    // clock; Free tiers (ammo/potions/lockpicks) release at once. The
                    // accrual it runs is idempotent-per-tick (a_now dedupe), so this
                    // scan drives it exactly as the inline version did.
                    if (TierReleased(a_cat, ref, playerPos, a_now))
                        candidates.push_back(ref->GetHandle());
                    else ++dNotYet;
                    return RE::BSContainer::ForEachResult::kContinue;
                };
            for (int ci = 0; ci < nCells; ++ci) {
                cells[ci]->ForEachReferenceInRange(origin, kLootRadius, scanOne);
                if (candidates.size() >= kMaxCandidates) break;   // cap hit mid-set
            }

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

            // REACHABLE FIRST, THEN CLOSEST (unified loot-failure model, marth):
            // a target that recently FAILED to path is DEPRIORITIZED, not
            // removed -- it sorts to the BACK (body or loose item alike), and
            // within each group closest-first, so the follower clears every
            // easy target before circling back to a path-troubled one (by which
            // time its grown grab radius usually lets it be taken from range).
            // An unresolvable handle sorts last. (Bounded at kMaxCandidates=48,
            // so this is a small sort.)
            std::sort(candidates.begin(), candidates.end(),
                [&origin, a_now](const RE::ObjectRefHandle& a, const RE::ObjectRefHandle& b) {
                    auto pa = a.get(); auto pb = b.get();
                    const bool fa = pa ? TravelFailedRecently(pa->GetFormID(), a_now) : true;
                    const bool fb = pb ? TravelFailedRecently(pb->GetFormID(), a_now) : true;
                    if (fa != fb) return !fa;   // un-failed targets first
                    const float da = pa ? origin.GetDistance(pa->GetPosition()) : 1e30f;
                    const float db = pb ? origin.GetDistance(pb->GetPosition()) : 1e30f;
                    return da < db;
                });

            // CHURN GUARD (#48): never ARM a new excursion the release gate
            // (the excursion driver's x1.15 "left leash" margin) would kill on
            // the very next tick. The arm gate below measures the CORPSE to the
            // player, the release gate measures the FOLLOWER -- so a follower
            // parked past the leash would arm toward an in-leash corpse, be
            // judged "left leash", release (evicting into the actor alias),
            // then re-arm ~1/sec (deck: 5x in 8s, each eviction ejecting the
            // player from furniture pre-marker). Arm's-reach grabs are
            // unaffected -- they need no travel, so they are outside this guard.
            const bool followerBeyondLeash =
                origin.GetDistance(playerPos) > leash * 1.15f;

            // Act after the walk. Re-resolve each handle at act time (#2). Perf
            // pass: an IN-REACH source is drained in place (StripCorpse -- every
            // wanted category, dibs-gated) and the loop KEEPS GOING, so all loot
            // already within arm's reach clears in this one tick (bounded by
            // kMaxCandidates=48 + physical reach). Movement-class actions (arming
            // or retargeting a walk) still end the tick -- one DISPATCH per tick
            // (§4.8.3); only the in-place mutations batch.
            int drained = 0;   // in-reach sources drained this tick (normal mode)
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

                // GROWN GRAB radius for this ref (stall cure): widens past
                // kArrivalDist with each path-fail so a body he can stand near
                // but never path the last gap to is taken from range. Loose
                // refs keep flat arm's reach (their acquire is a physical
                // Activate). A grab BEYOND plain arm's reach also honours the
                // player bubble -- never hoover a body you are standing over.
                const float grabR = LooseRef(ref) ? kArrivalDist : GrabRadiusFor(rid);
                // Clutter (arrows/bolts/lockpicks) is exempt from the bubble --
                // grabbed right under the player's feet, never deferred.
                const bool  grabOk = df <= kArrivalDist ||
                    (df <= grabR &&
                     (IsClutterCat(a_cat) ||
                      playerPos.GetDistance(ref->GetPosition()) > Config::g_playerBubble.load()));

                // ── EXCURSION MODE: the follower is already claimed (priority 60)
                // and driving a batch. The closest eligible candidate decides the
                // tick: within arm's reach -> grab it (a mutation); farther but
                // walkable -> RETARGET the excursion to it (movement, no release,
                // no turn-around). Do NOT start a new fill and do NOT release.
                if (a_mode == LootMode::kExcursion) {
                    // P7: excursion mode means this follower is already claimed, so
                    // a live slot must exist -- resolve it up front and drive it.
                    const int s = SlotIndexOf(a_follower->GetFormID());
                    if (s < 0) continue;   // no live slot: nothing to drive
                    TravelIntent& tr = g_travelSlots[s];
                    if (grabOk && !LooseRef(ref)) {
                        // DRAIN IN PLACE (perf pass): he is on it (or within its
                        // grown grab radius), so take EVERYTHING his gambits want
                        // in this visit -- the same full strip the arrival path
                        // runs (every tier dibs-gated through TierReleased inside
                        // StripCorpse), not one category per tick.
                        bool leftWaiting = false;
                        const bool moved = g_svc
                            ? StripCorpse(a_follower, *g_svc, ref, a_now, &leftWaiting)
                            : LootHere(a_follower, ref, a_cat, a_potionWant);
                        if (leftWaiting) g_scanSawWaiting = true;
                        if (moved) {
                            g_grabGrow.erase(rid);      // grabbed -> stale grow verdict
                            g_travelFailed.erase(rid);  // back to normal sort priority
                            return true;
                        }
                        continue;   // nothing takeable here yet -- try the next
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
                    if (tr.phase == TravelPhase::Walking) {
                        auto  tptr = tr.target.get();
                        auto* cur  = tptr.get();
                        if (cur && !cur->IsDisabled() && !cur->IsMarkedForDeletion() &&
                            !TravelFailedRecently(cur->GetFormID(), a_now) &&
                            a_now - tr.progressAt <= kNoProgress)
                            return false;   // stay the course
                    }
                    // A LOOSE ref (route 2b) falls through to RETARGET even at
                    // arm's reach: the acquire runs at the driver's ARRIVAL
                    // (Activate dispatch), never as an in-place transfer here.
                    if (!Config::g_lootTravel.load())                              continue;
                    if (df > walkLimit)                                           continue;
                    // WALK-only anti-churn commit: never re-WALK a target inside
                    // its fail cooldown -- Retarget/Fill reset the no-progress
                    // clock, so an instant re-walk would defeat the stall verdict
                    // (the frozen-Erik loop). The unified failure model still
                    // keeps the ref IN THE RUNNING: it sorts to the back, and the
                    // in-range grown-grab path above never consults this list.
                    if (TravelFailedRecently(rid, a_now))                          continue;
                    // Clutter is exempt from the bubble (IsClutterCat, above).
                    if (!IsClutterCat(a_cat) &&
                        playerPos.GetDistance(ref->GetPosition())
                            <= Config::g_playerBubble.load())                      continue;
                    // OFF-NAVMESH GATE: if no navmesh is near the ref, the Travel
                    // package can't build a path and he'd freeze -- skip + blocklist
                    // (25s LRU, so the scan isn't re-run) BEFORE dispatch.
                    // TRANSIENT ONLY (stall-bug fix): this is a pre-dispatch
                    // HEURISTIC -- nearest-VERTEX misjudges stairs/rubble/body-
                    // piles -- and it used to accrue a stall strike, so two ticks
                    // of false verdict 5-min-stickied reachable loot (the
                    // "follower stands idle among corpses" stall). Never sticky
                    // a ref no walk was ever attempted at; the idle reassess
                    // wipes this transient block, so it re-tries once he moves.
                    // Each verdict also GROWS the ref's from-range grab radius
                    // (NotePathFail), so the usual cure is a later-tick hoover,
                    // not a retry of the walk.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelFailed(rid, a_now);   // off-navmesh -> transient only
                        NotePathFail(rid);              // -> widen its grab radius
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    if (Packages::LootTravelRetarget(a_follower, ref, s)) {
                        tr.target   = ref->GetHandle();
                        tr.cat      = a_cat;
                        tr.want     = a_potionWant;
                        tr.deadline = TravelDeadline(df, a_now);
                        tr.phase    = TravelPhase::Walking;
                        tr.lastPos    = origin;   // reset no-progress tracker
                        tr.progressAt = a_now;
                        tr.stolenSince = {};      // fresh leg -> fresh theft episode
                        return true;   // new leg -- the excursion continues at 60
                    }
                    continue;
                }

                // ── NORMAL MODE: START an excursion by walking to a far corpse,
                // or transfer one within arm's reach. A LOOSE ref (route 2b)
                // takes the excursion path REGARDLESS of distance -- there is no
                // in-place transfer for it, the acquire is the driver's arrival
                // Activate -- so it must claim/walk even from arm's reach.
                if ((!grabOk || LooseRef(ref)) && Config::g_lootTravel.load()) {
                    // Already drained in-reach loot this tick: that WAS the
                    // action. Candidates are closest-first, so everything in
                    // reach came before this far/loose one; arming a walk on
                    // top would stack a movement dispatch onto the mutations.
                    // The next tick's scan arms the excursion.
                    if (drained > 0) return true;
                    // CHURN GUARD (#48, computed above): he's already past the
                    // release margin -- an arm now dies "left leash" next tick.
                    // Skip the candidate entirely: it is far (or loose), so
                    // there is no in-place transfer to fall through to.
                    if (followerBeyondLeash) continue;
                    // Too far to WALK without abandoning the player -- leave it
                    // (following brings them closer later; also fairer to you).
                    if (df > walkLimit) continue;
                    // WALK-only anti-churn commit (see the excursion path note):
                    // no re-walk inside the fail cooldown; the ref still sorts
                    // last (not skipped) and the grown grab never checks this.
                    if (TravelFailedRecently(rid, a_now)) continue;
                    // CONVERGENCE YIELD: never walk to loot the player is right
                    // next to -- you win the race for the corpse you're heading to.
                    // Clutter is exempt from the bubble (IsClutterCat, above).
                    if (!IsClutterCat(a_cat) &&
                        playerPos.GetDistance(ref->GetPosition()) <= Config::g_playerBubble.load())
                        continue;
                    // P7: claim the first FREE slot; skip if all are busy.
                    const int s = FreeSlotIndex();
                    if (s < 0) continue;
                    // OFF-NAVMESH GATE (see the excursion path): no mesh near the
                    // ref -> no path -> freeze. Skip + blocklist before dispatch.
                    // TRANSIENT ONLY -- a pre-dispatch heuristic never earns a
                    // sticky strike (see the excursion-path note); it GROWS the
                    // ref's from-range grab radius instead.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelFailed(rid, a_now);   // off-navmesh -> transient only
                        NotePathFail(rid);              // -> widen its grab radius
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    if (Packages::LootTravelFill(a_follower, ref, s)) {
                        g_travelSlots[s].active    = true;
                        g_travelSlots[s].follower  = a_follower->GetFormID();
                        g_travelSlots[s].target    = ref->GetHandle();
                        g_travelSlots[s].cat       = a_cat;
                        g_travelSlots[s].want      = a_potionWant;
                        g_travelSlots[s].deadline  = TravelDeadline(df, a_now);
                        g_travelSlots[s].phase     = TravelPhase::Walking;
                        g_travelSlots[s].startTime = a_now;   // excursion begins now
                        g_travelSlots[s].lastPos    = origin;  // reset no-progress tracker
                        g_travelSlots[s].progressAt = a_now;
                        g_travelSlots[s].stolenSince = {};     // slots are reused -- clear stale episode
                        return true;   // committed to the walk; transfer on arrival
                    }
                    // Travel UNAVAILABLE (off AE, records unresolved, quest not
                    // running) -- fall through to the arm's-reach transfer below
                    // rather than never looting this candidate (the SE fallback).
                }

                // Corpse/container within (grown) reach: DRAIN it -- every
                // category his gambits want (StripCorpse; each tier still
                // dibs-gated through TierReleased), then keep going to the next
                // in-reach source. A far/loose ref that fell through here
                // (travel unavailable -- the SE fallback) keeps the old
                // one-transfer-per-tick shape.
                if (grabOk && !LooseRef(ref) && g_svc) {
                    if (StripCorpse(a_follower, *g_svc, ref, a_now, nullptr)) {
                        ++drained;
                        g_grabGrow.erase(rid);      // grabbed -> stale grow verdict
                        g_travelFailed.erase(rid);  // back to normal sort priority
                    }
                    continue;
                }
                if (LootHere(a_follower, ref, a_cat, a_potionWant)) return true;
            }
            if (drained > 0) {
                if (drained > 1)
                    spdlog::info("[loot] {:08X}: drained {} in-reach sources in one tick",
                                 a_follower->GetFormID(), drained);
                return true;
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
                                     // Clutter is bubble-exempt (IsClutterCat) --
                                     // report false/exempt so the log stays truthful.
                                     !IsClutterCat(a_cat) && dP <= Config::g_playerBubble.load(),
                                     Config::g_playerBubble.load(),
                                     TravelFailedRecently(r0id, a_now),
                                     (FreeSlotIndex() < 0 && SlotIndexOf(a_follower->GetFormID()) < 0),
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
                         RE::TESObjectREFR* a_corpse, Clock::time_point a_now,
                         bool* a_leftWaiting) {
            bool moved = false;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const RE::NiPoint3 playerPos = pc ? pc->GetPosition() : a_follower->GetPosition();
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
                if (isLoot) {
                    // CLAIM-AND-RELEASE parity with the scan (#2): a free-tier
                    // arm's-reach strip must not snatch a fresh kill's Gear/Valuables
                    // ahead of the player's dibs. Gate the non-free tiers by the SAME
                    // predicate LootNearby uses; if it's still the player's, leave it
                    // and flag WAITING so the caller does NOT mark the body DONE --
                    // the excursion linger revisits it once the claim releases.
                    if (!TierReleased(cat, a_corpse, playerPos, a_now)) {
                        if (a_leftWaiting) *a_leftWaiting = true;
                        start = choice.ruleIndex + 1;
                        continue;
                    }
                    // Equipment moves ONE piece per call (§4.3's shape) while the
                    // other categories take all they want internally -- but this
                    // visit is the corpse's LAST (it's marked DONE right after we
                    // return). One call stranded everything past the first take:
                    // a body with a better sword AND the bow the equip-ranged
                    // gambit needs (dual-primary) yielded only the sword, and the
                    // bow/cuirass sat blocklisted for 25 s or forever. Drain the
                    // category: each take raises the inventory-derived baseline
                    // (armor now baselines against carried pieces too, #3), so this
                    // terminates; the guard is belt-and-braces (2 weapon roles + 7
                    // armor slots).
                    int guard = (cat == Category::Equipment) ? 10 : 1;
                    while (guard-- > 0 && LootHere(a_follower, a_corpse, cat, want)) moved = true;
                }
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
        // WORKER THREAD (§0.37, ServiceFollower's task -- NOT MainThread::Post).
        // The follower reads (GetInventory / CountPotions / g_travel) must share
        // the thread with the loot/heal/loadout mutations in that same task or they
        // race InventoryChanges. This routine is LOG-ONLY: the merchant read + any
        // transaction run in Papyrus (VM, dispatched worker->VM), so nothing here
        // touches a live merchant's inventory off the main thread -- the CTD class
        // the old MainThread::Post existed for. The cadence statics below are the
        // namespace-scope g_econ* maps, wiped on revert by ClearTransientState.
        // Takes the logistics GAMBITS by const-reference (a_state.logistics(), a
        // reference into g_followers) -- shared serially with the same worker task.

        // Does the vendor's VEND list trade this item? The list holds KEYWORDS
        // (VendorItemWeapon et al.); notBuySell inverts it into an EXCLUSION
        // list (the engine's own semantics for that flag).
}
