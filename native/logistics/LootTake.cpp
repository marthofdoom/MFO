// logistics/LootTake.cpp -- WHAT a follower takes and FROM WHERE: the per-category
// looters (ammo, potions, gold, lockpicks, jewelry, soul gems, ingredients,
// valuables) and the source policy (mod presence, player home / storage, LOTD
// drop-offs, claim-and-release, tier release, lock pick, navmesh reach).
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
// Logistics_Loot.cpp -- the LOOT side of the logistics family (split
// mechanically out of Logistics.cpp, no logic change): per-category looters
// (ammo/potions/equipment/gold/jewelry/soul gems/lockpicks), the loot judge
// (weapon roles, armor/mage-apparel comparison, creature-gear guards), the
// claim-and-release fair-chance machinery, navmesh reach, corpse strip and
// the excursion scan, and LootNearby itself. Travel STATE (TravelIntent,
// g_travelSlots) is shared with ServiceFollower and lives in
// Logistics_internal.h.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // ROAD 2 (A/B): ch.19 kIntent_Travel loot travel

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
                // NEVER-LOOT: quest amulets/rings stay engine-protected regardless
                // of bLootSpecialItems (per-instance alias check). Unique-ring
                // catalog exclusion below is what the toggle governs.
                if (IsQuestObjectInstance(data.second.get())) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
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
                // NEVER-LOOT: quest gems (Azura's Star et al.) stay engine-protected
                // regardless of bLootSpecialItems (per-instance alias check). The
                // catalog exclusion below is what the toggle governs.
                if (IsQuestObjectInstance(data.second.get())) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
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

        // Is this item an alchemy ingredient? FormType-classified (never by
        // name, §4.8.2 pattern) -- IngredientItem is CommonLib's class for
        // INGR records.
        bool IsIngredientItem(RE::TESBoundObject* a_obj) {
            return a_obj->As<RE::IngredientItem>() != nullptr;
        }

        // Take all the alchemy ingredients on a corpse/container. Free tier
        // like ammo/lockpicks (TierReleased below) -- nobody else competes for
        // a follower's own reagent restock. Collect-then-transfer like LootAmmo
        // (#2), with the same carry-weight gate as Jewelry/SoulGems (ingredients
        // are light but not weightless).
        bool LootIngredients(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (!IsIngredientItem(obj)) continue;
                // NEVER-LOOT: quest ingredients stay engine-protected regardless
                // of bLootSpecialItems (per-instance alias check). The catalog
                // exclusion below is what the toggle governs.
                if (IsQuestObjectInstance(data.second.get())) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
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

        // Is this a "valuable" MISC item worth grabbing to sell -- gold/weight
        // ratio clears Config::g_valuablesRatio (raw gemstones and similar
        // high-density loot). MISC-restricted and explicitly excludes gold and
        // soul gems so "loot valuables" never double-claims what a more
        // specific category already owns. GetGoldValue()/GetWeight() are
        // TESForm-level reads (same API LootJewelry/SoulGems already use via
        // t.obj->GetWeight()), so no per-subclass accessor is needed.
        bool IsValuableMisc(RE::TESBoundObject* a_obj) {
            constexpr RE::FormID kGold001 = 0x0000000F;
            if (!a_obj->As<RE::TESObjectMISC>()) return false;   // MISC only
            if (a_obj->GetFormID() == kGold001 || IsCoinLoot(a_obj)) return false;   // Gold tier
            if (IsSoulGemItem(a_obj)) return false;                                   // SoulGems tier
            const float value = static_cast<float>(std::max<std::int32_t>(a_obj->GetGoldValue(), 0));
            if (value <= 0.0f) return false;
            const float weight = a_obj->GetWeight();
            const float ratio = weight > 0.0f ? value / weight : value;   // weightless & valuable still counts
            return ratio >= Config::g_valuablesRatio.load();
        }

        // Take all "valuable" MISC items on a corpse/container -- grabbed and
        // dibs-protected like gold/jewellery (Valuables tier below); SELLING
        // them is a separate existing/future economy path, this only loots.
        //
        // GOLD FOLDS INTO VALUABLES (marth 2026-09-05, deck evidence): a chest
        // holding only 101 gold was classified "empty" and skipped, because
        // Valuables explicitly excluded coin (IsValuableMisc) and almost nobody
        // separately configures act.loot_gold. "Loot valuables (to sell)"
        // already means "grab what's worth money" -- coin is the purest case --
        // so a Valuables rule now also takes gold. act.loot_gold is UNCHANGED
        // and still works standalone for anyone who wants coin only. The take
        // reuses LootGold verbatim (the proven Gold001/OCF-coin scan over
        // GetInventory) rather than re-deriving a gold count here --
        // Actor::GetGoldAmount() is known to null-deref on some containers,
        // which is exactly why LootGold never calls it.
        bool LootValuables(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
            if (a_peek && LootGold(a_follower, a_src, true)) return true;
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (!IsValuableMisc(obj)) continue;
                // NEVER-LOOT: quest MISC stays engine-protected regardless of
                // bLootSpecialItems (per-instance alias check). The artifact/MISC
                // catalog exclusion below is what the toggle governs.
                if (IsQuestObjectInstance(data.second.get())) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
                if (a_peek) return true;
                takes.push_back({ obj, data.first });
            }
            if (a_peek) return false;
            bool moved = LootGold(a_follower, a_src, false);   // proven take, see banner above
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
                    // DBM_AutoSortDropOff (museum Display Drop-off), DBMMuseumShipmentsCrate-
                    // Incoming / -Outgoing (every town/inn/house shipping crate), DBM_SalesBox
                    // (Sales Income), DBM_Incomebox (Donations), DBM_SellStorage.
                    for (const RE::FormID local :
                         { 0x07EEFDu, 0x1772A6u, 0x1772A7u, 0x166349u, 0x0BE533u, 0x11CC99u }) {
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
                a_cat == Category::Potions || a_cat == Category::Lockpicks ||
                a_cat == Category::Ingredients)
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
            // Gold / Jewelry / SoulGems / Valuables (Category::Valuables, the
            // MISC sell-tier scan) -> Valuables tier. Falls through here by
            // default: anything not in the free-tier list above and not
            // Equipment lands on this shared path.
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
            case Category::Ingredients: return LootIngredients(a_follower, a_ref);
            case Category::Valuables: return LootValuables(a_follower, a_ref);
            case Category::Museum:    return LootMuseum(a_follower, a_ref);   // LOTD, logistics/Lotd.cpp
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
            case Category::Ingredients: return LootIngredients(a_follower, a_ref, true);
            case Category::Valuables: return LootValuables(a_follower, a_ref, true);
            case Category::Museum:    return LootMuseum(a_follower, a_ref, true);
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

}
