#include "PCH.h"
#include "Logistics.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Actuation.h"   // cast-in-logistics: reuse the combat cast path (Fire)
#include <algorithm>      // std::sort/std::min/std::erase_if (healing stock cap)
#include <cmath>          // std::sin/cos/sqrt for the view cone
#include <unordered_set>  // keepWeapons: best-of-each-class protection set
#include <array>          // P7: fixed-size per-slot travel-intent table
#include <cctype>         // std::tolower: keyword-name school match (v1.0.31)
#include <utility>        // std::pair: the school-name keyword table (v1.0.31)
#include "Confidence.h"   // the confidence leash (core tenet)
#include "Packages.h"     // Option A: LootTravelFill / LootTravelClear
#include "Forms.h"        // g_travelPackage / g_lootQuest (WALK diagnostic)
#include "Probe.h"        // Probe::CrosshairTarget (the QuickLoot-aware claim signal)
#include "ItemCatalog.h"  // load-order item catalog: potion class + never-loot exclusions
#include "MEOBridge.h"    // MEO gem transfer on gear swap (#17) + WornUid
#include "Papyrus.h"      // route 2b acquire probe: VM-dispatched ObjectReference.Activate
#include "MainThread.h"   // the pump (§0.37): live-vendor reads MUST run on the main thread
#include "Followers.h"    // #62 on-load beast-head sweep iterates g_active (main thread)
#include <functional>     // #62 self-reposting on-load sweep closure
#include <memory>         // std::shared_ptr for that closure
#include "TradeBridge.h"  // #21 econ bridge: MFO_Trade Papyrus round-trip (Phase 0 self-test)
#include <mutex>          // #69: g_stockMx -- g_stockGear is a real cross-thread map (worker + co-save)

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
        // AmmoIsBolt is defined in the PUBLIC namespace (declared in Logistics.h) so
        // the economy buy side shares it; anon-namespace callers resolve it there.

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

        // The "low power" cutoff derived from the load order at kDataLoaded: the
        // floor of the SECOND magnitude tier of restore potions, so the weakest
        // tier reads as low power. 0 until computed / when the list has < 2 tiers.
        // ComputeWeakPotionFloor() fills it; LootPotions uses it when iMinPotionMag
        // is 0 (auto). Written once before any loot tick, then read-only.
        float g_autoPotionFloor = 0.0f;

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

        // ── MAGIC LOADOUT (v1.0.29): the mage's loot preferences ────────────
        // A follower is a MAGIC USER iff he has at least one ENABLED cast
        // gambit (act.cast_self / act.cast_target) in his combat table --
        // GAMBIT-DRIVEN like every other role decision in this file (marth:
        // role is what the table says, never a skill guess; a battlemage with
        // only attack/equip gambits is NOT a magic user however high his
        // Destruction). His TARGET SCHOOL is the most-represented school among
        // those gambits' spells -- each spell's school read from its costliest
        // effect's base MGEF "Magic Skill" field. Pure form-DATA reads (no
        // 3D/physics), so this is safe on the logistics worker. No cast
        // gambits -> a_castGambits stays 0 -> the whole feature is inert for
        // him (unchanged behavior).
        RE::ActorValue TargetMagicSchool(const FollowerState& a_state, int& a_castGambits) {
            constexpr int kNumSchools = 5;
            static constexpr RE::ActorValue kSchools[kNumSchools] = {
                RE::ActorValue::kAlteration, RE::ActorValue::kConjuration,
                RE::ActorValue::kDestruction, RE::ActorValue::kIllusion,
                RE::ActorValue::kRestoration,
            };
            a_castGambits = 0;
            int tally[kNumSchools] = {};
            for (const auto& g : a_state.combat()) {
                if (!g.enabled) continue;   // a toggled-OFF rule doesn't make a mage (same rule as TableHasAction)
                if (g.actionOpcode != Vocab::kActCastSelf &&
                    g.actionOpcode != Vocab::kActCastTarget &&
                    g.actionOpcode != Vocab::kActCastPlayer) continue;
                ++a_castGambits;   // counts even when the spell's school below is unreadable
                auto* spell = g.actionParamForm
                    ? RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm) : nullptr;
                if (!spell) continue;
                const auto* eff  = spell->GetCostliestEffectItem();
                const auto* mgef = eff ? eff->baseEffect : nullptr;
                if (!mgef) continue;
                // data.associatedSkill IS the MGEF record's "Magic Skill" (the
                // school); non-school spells (kNone) simply don't tally.
                const auto school = mgef->data.associatedSkill;
                for (int i = 0; i < kNumSchools; ++i)
                    if (school == kSchools[i]) { ++tally[i]; break; }
            }
            // Most-represented school wins ("weighted by the gambits set" --
            // marth); a tie goes to the first-listed, which is stable across
            // ticks so the preference never flip-flops between two schools.
            int best = -1, bestN = 0;
            for (int i = 0; i < kNumSchools; ++i)
                if (tally[i] > bestN) { bestN = tally[i]; best = i; }
            return best >= 0 ? kSchools[best] : RE::ActorValue::kNone;
        }

        const char* SchoolName(RE::ActorValue a_school) {
            using AV = RE::ActorValue;
            switch (a_school) {
            case AV::kAlteration:  return "Alteration";
            case AV::kConjuration: return "Conjuration";
            case AV::kDestruction: return "Destruction";
            case AV::kIllusion:    return "Illusion";
            case AV::kRestoration: return "Restoration";
            default:               return "none";
            }
        }

        // Which SCHOOL does this actor value boost, if any? Vanilla apparel
        // does NOT fortify the skill AV itself: the "Fortify Destruction"
        // enchantment (spells cost -X%) modifies <School>Modifier, and the
        // "stronger spells" effects use <School>PowerModifier -- so all three
        // AVs map back to the school. Mods use any of the three; matching by
        // AV (never by name) is the §4.8.2 derived-vocabulary principle.
        RE::ActorValue SchoolOfBoostAV(RE::ActorValue a_av) {
            using AV = RE::ActorValue;
            switch (a_av) {
            case AV::kAlteration:  case AV::kAlterationModifier:  case AV::kAlterationPowerModifier:  return AV::kAlteration;
            case AV::kConjuration: case AV::kConjurationModifier: case AV::kConjurationPowerModifier: return AV::kConjuration;
            case AV::kDestruction: case AV::kDestructionModifier: case AV::kDestructionPowerModifier: return AV::kDestruction;
            case AV::kIllusion:    case AV::kIllusionModifier:    case AV::kIllusionPowerModifier:    return AV::kIllusion;
            case AV::kRestoration: case AV::kRestorationModifier: case AV::kRestorationPowerModifier: return AV::kRestoration;
            default:               return AV::kNone;
            }
        }

        // Case-insensitive ASCII substring -- keyword editorIDs are plain
        // ASCII ("MagicSchool_Destruction", "MAG_DestructionRobes", ...). No
        // locale, no allocation.
        bool ContainsNoCase(const char* a_hay, const char* a_needle) {
            if (!a_hay || !a_needle || !*a_needle) return false;
            for (const char* h = a_hay; *h; ++h) {
                const char* a = h;
                const char* b = a_needle;
                while (*a && *b &&
                       std::tolower(static_cast<unsigned char>(*a)) ==
                       std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
                if (!*b) return true;
            }
            return false;
        }

        // LAST-DITCH school read: a keyword whose editorID NAMES a school.
        // Some mods tag their robes/effects ("MagicSchool_Destruction",
        // "DestructionRobes") instead of -- or as well as -- wiring school
        // AVs, and the school names are distinctive enough that a substring
        // match is safe (nothing vanilla or common collides with e.g.
        // "conjuration" outside actual school tagging). Keyword arrays are
        // static form DATA -> worker-safe. This bends §4.8.2's never-by-name
        // rule deliberately and LAST: every AV read gets first refusal, and a
        // robe we'd otherwise misjudge as junk is worse than a name match.
        RE::ActorValue SchoolFromKeywords(const RE::BGSKeywordForm* a_kwf) {
            if (!a_kwf || !a_kwf->keywords) return RE::ActorValue::kNone;
            static constexpr std::pair<const char*, RE::ActorValue> kNames[] = {
                { "alteration",  RE::ActorValue::kAlteration  },
                { "conjuration", RE::ActorValue::kConjuration },
                { "destruction", RE::ActorValue::kDestruction },
                { "illusion",    RE::ActorValue::kIllusion    },
                { "restoration", RE::ActorValue::kRestoration },
            };
            for (std::uint32_t i = 0; i < a_kwf->numKeywords; ++i) {
                const auto* kw = a_kwf->keywords[i];
                const char* ed = kw ? kw->GetFormEditorID() : nullptr;
                if (!ed) continue;
                for (const auto& [name, school] : kNames)
                    if (ContainsNoCase(ed, name)) return school;
            }
            return RE::ActorValue::kNone;
        }

        // Comma-join a form's keyword editorIDs for the diagnostic dump below.
        std::string KeywordCsv(const RE::BGSKeywordForm* a_kwf) {
            std::string out;
            if (!a_kwf || !a_kwf->keywords) return out;
            for (std::uint32_t i = 0; i < a_kwf->numKeywords; ++i) {
                const auto* kw = a_kwf->keywords[i];
                const char* ed = kw ? kw->GetFormEditorID() : nullptr;
                if (!ed || !*ed) continue;
                if (!out.empty()) out += ',';
                out += ed;
            }
            return out;
        }

        // The school ONE beneficial effect boosts, or kNone -- the v1.0.31
        // detection chain. v1.0.29 read ONLY data.primaryAV, which is where
        // VANILLA fortify-school enchantments put the AV -- but marth's
        // modlist (LoreRim) re-authors robe enchantments, and the deck log
        // showed Marcurio's valid Destruction robe scoring 0 (never preferred)
        // while a plain circlet won as "best". The school is now the FIRST of
        // these that resolves (all pure form-DATA reads, worker-safe):
        //   (a) primaryAV                -- the vanilla wiring (unchanged);
        //   (b) associatedSkill          -- the MGEF "Magic Skill" field, the
        //       SAME read TargetMagicSchool already trusts on the spell side;
        //   (c) secondaryAV, only when the archetype really modifies values
        //       (Value/DualValue/PeakValue Modifier -- there the second AV is
        //       a genuine target, elsewhere it is noise);
        //   (d) an MGEF keyword naming the school (SchoolFromKeywords above).
        RE::ActorValue EffectBoostSchool(const RE::EffectSetting* a_mgef) {
            if (!a_mgef) return RE::ActorValue::kNone;
            if (auto s = SchoolOfBoostAV(a_mgef->data.primaryAV); s != RE::ActorValue::kNone) return s;
            if (auto s = SchoolOfBoostAV(a_mgef->data.associatedSkill); s != RE::ActorValue::kNone) return s;
            using Arch = RE::EffectArchetypes::ArchetypeID;
            const auto arch = a_mgef->data.archetype;
            if (arch == Arch::kValueModifier || arch == Arch::kDualValueModifier ||
                arch == Arch::kPeakValueModifier)
                if (auto s = SchoolOfBoostAV(a_mgef->data.secondaryAV); s != RE::ActorValue::kNone) return s;
            return SchoolFromKeywords(a_mgef);
        }

        // How strongly this armor boosts the target school: the count of
        // beneficial fortify-<school> effects on its BASE enchantment (a_outMag
        // sums their magnitudes -- the fanciness input below), with a
        // keyword-tag last resort for gear whose school lives nowhere in its
        // effects. A RUNTIME read of the candidate's own record, deliberately
        // NOT a catalog field, so it works out-of-box on modded gear with no
        // patcher run. Pure form data -> worker-safe. Player-enchanted
        // INSTANCES carry their enchant in extra data, not the base form --
        // out of scope (world/corpse mage gear is base-enchanted).
        int SchoolMatchScore(const RE::TESObjectARMO* a_armo, RE::ActorValue a_school,
                             float* a_outMag = nullptr) {
            if (a_outMag) *a_outMag = 0.0f;
            if (!a_armo || a_school == RE::ActorValue::kNone) return 0;
            int score = 0;
            const auto* ench = a_armo->formEnchanting;
            if (ench) {
                for (const auto* e : ench->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (!mgef) continue;
                    using Flag = RE::EffectSetting::EffectSettingData::Flag;
                    // A curse ("Destruction costs MORE") must never read as a boost.
                    if (mgef->data.flags.any(Flag::kDetrimental) ||
                        mgef->data.flags.any(Flag::kHostile)) continue;
                    if (EffectBoostSchool(mgef) != a_school) continue;
                    ++score;
                    if (a_outMag) *a_outMag += e->GetMagnitude();
                }
                // The effects said nothing but the ENCHANTMENT record itself
                // is school-tagged -- count it once (magnitude unknown, so the
                // fanciness rank falls back to gold value alone).
                if (score == 0 && SchoolFromKeywords(ench) == a_school) score = 1;
            }
            // ...and the ARMO record's own tags, for robes whose school is
            // authored only as an item keyword.
            if (score == 0 && SchoolFromKeywords(a_armo) == a_school) score = 1;
            return score;
        }

        // The slots MAGE gear lives on: body robe, head hood (head+hair bits)
        // or circlet, hands, feet. Deliberately NOT amulet/ring (jewellery is
        // the player's Valuables tier, looted separately) and NOT shield/
        // forearms/calves -- school apparel never occupies those, and scoping
        // the preference here means a magic user's OTHER slots keep the plain
        // rating rules (don't strip gear he legitimately uses).
        using BipedSlot = RE::BGSBipedObjectForm::BipedObjectSlot;
        constexpr BipedSlot kMageSlots[] = {
            BipedSlot::kHead, BipedSlot::kHair, BipedSlot::kCirclet,
            BipedSlot::kBody, BipedSlot::kHands, BipedSlot::kFeet,
        };

        // The mage-apparel ranking key. SCHOOL MATCH IS PRIMARY -- a fancy
        // wrong-school robe must never beat a plain right-school one (marth);
        // FANCINESS (base gold value + total fortify-school magnitude) ranks
        // within a school tier, so with MEO's finer enchanted stock in the
        // world the richer robe wins. Both are form-DATA reads, worker-safe.
        // (MEO socket-capacity scoring was considered and rejected: MEO_API
        // queries are main-thread-only per MEOBridge.h, and this runs on the
        // worker -- value+magnitude already ranks MEO gear up, and the
        // existing gem transfer below carries invested gems onto the upgrade.)
        struct MageKey { int score = 0; float fancy = -1.0f; };
        MageKey MageApparelKey(const RE::TESObjectARMO* a_armo, RE::ActorValue a_school) {
            float mag = 0.0f;
            const int score = SchoolMatchScore(a_armo, a_school, &mag);
            const auto value = static_cast<float>(std::max<std::int32_t>(a_armo->GetGoldValue(), 0));
            return { score, value + mag };
        }
        bool MageKeyBeats(const MageKey& a_cand, const MageKey& a_worn) {
            return a_cand.score > a_worn.score ||
                   (a_cand.score == a_worn.score && a_cand.fancy > a_worn.fancy);
        }

        // ── per-candidate apparel diagnostics (v1.0.31) ─────────────────────
        // The v1.0.29 field failure was INVISIBLE: Marcurio's valid Destruction
        // robe scored 0 (its LoreRim enchantment wired no vanilla school AV)
        // and the log only ever showed the winner, so there was nothing to
        // debug from -- just "circlet -> best" and a wrong guess. Dump the
        // REAL record data for every apparel candidate a magic user evaluates:
        // if the broadened chain STILL misses on some robe, the next fix comes
        // from this log, not another guess. Throttled by a session-scope
        // dedupe (one dump per base form + target school): corpses repeat the
        // same base records endlessly and peek passes re-walk them every tick,
        // so without the dedupe this would spam. Touched only from the
        // logistics worker (sequential, like g_svc) -> no lock needed.
        void LogMageApparelDiag(RE::TESObjectARMO* a_armo, RE::ActorValue a_school) {
            static std::unordered_set<std::uint64_t> s_seen;
            const auto key = static_cast<std::uint64_t>(a_armo->GetFormID()) |
                             (static_cast<std::uint64_t>(a_school) << 32);
            if (s_seen.size() > 512) s_seen.clear();   // bounded; worst case is a re-dump
            if (!s_seen.insert(key).second) return;

            float     mag   = 0.0f;
            const int score = SchoolMatchScore(a_armo, a_school, &mag);
            const auto* ench = a_armo->formEnchanting;
            spdlog::info("[loot] apparel {:08X} '{}' rating={:.0f} value={} ench={:08X} kw=[{}] -> school {} score={} mag={:.0f}",
                         a_armo->GetFormID(),
                         a_armo->GetFullName() ? a_armo->GetFullName() : "?",
                         a_armo->GetArmorRating(), a_armo->GetGoldValue(),
                         ench ? ench->GetFormID() : 0, KeywordCsv(a_armo),
                         SchoolName(a_school), score, mag);
            if (!ench) return;
            int i = 0;
            for (const auto* e : ench->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef) { ++i; continue; }
                using Flag = RE::EffectSetting::EffectSettingData::Flag;
                const bool det = mgef->data.flags.any(Flag::kDetrimental) ||
                                 mgef->data.flags.any(Flag::kHostile);
                spdlog::info("[loot] apparel {:08X} eff#{} mgef={:08X} '{}' primAV={} assocSkill={} secAV={} arch={} mag={:.1f} kw=[{}] -> {}",
                             a_armo->GetFormID(), i, mgef->GetFormID(),
                             mgef->GetFullName() ? mgef->GetFullName() : "?",
                             static_cast<int>(mgef->data.primaryAV),
                             static_cast<int>(mgef->data.associatedSkill),
                             static_cast<int>(mgef->data.secondaryAV),
                             static_cast<int>(mgef->data.archetype),
                             e->GetMagnitude(), KeywordCsv(mgef),
                             det ? "curse" : SchoolName(EffectBoostSchool(mgef)));
                ++i;
            }
        }

        // The school-scored apparel path -- since v1.0.31 the magic user's
        // ONLY apparel path (ArmorIsBetter never runs for him; see the loot
        // loop). Same shape as it: a strict upgrade on at least one MAGE slot
        // it covers, beaten on none. Rules (marth, v1.0.31 -- PURE CASTER):
        //   - RATED armor -- heavy OR light, school-enchanted or not -- is
        //     NEVER mage loot. v1.0.29's first line handed "score 0 but
        //     rated" back to the rating path, which is exactly how Marcurio,
        //     a detected Destruction user, looted a Dwarven Heavy Cuirass. A
        //     magic user takes clothing/robes (rating 0) ONLY. Armor he
        //     ALREADY wears is never force-stripped: it just stops being
        //     replaced by more armor, until a school robe displaces it;
        //   - a school-MATCHING piece may replace anything on a mage slot
        //     (the point of the feature: school beats raw rating there);
        //   - a GENERIC piece (score 0 -- plain clothing) must be a GENUINE
        //     upgrade: it may dress a BARE clothing slot (head/body/hands/
        //     feet) or strictly beat worn rating-0 rags on value. It never
        //     fills an empty kHair/kCirclet bit -- a plain circlet is
        //     jewellery, the player's Valuables tier, and "Copper and
        //     Moonstone Circlet -> best" (deck log, v1.0.30) was junk, not an
        //     upgrade -- and it NEVER replaces real worn armor;
        //   - a tie does NOT swap (stable -- no loot thrash).
        // (No ArmorClassSuits call anymore: everything past the rating gate
        // is clothing, which that check passes unconditionally.)
        bool MageApparelIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo,
                                 RE::ActorValue a_school, const MageKey& a_key) {
            if (a_armo->GetArmorRating() > 0.0f)
                return false;   // PURE CASTER: rated armor is never mage loot
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            bool overlapsAny = false;
            bool genuine     = a_key.score > 0;   // school gear justifies itself
            for (const auto slot : kMageSlots) {
                if (!(mask & static_cast<std::uint32_t>(slot))) continue;
                overlapsAny = true;
                auto* worn = a_follower->GetWornArmor(slot);
                if (!worn) {
                    // Open slot -> nothing to beat. A GENERIC piece only earns
                    // its take here when it actually DRESSES him: a bare
                    // clothing slot, never a bare circlet/hair bit.
                    if (a_key.score <= 0 &&
                        (slot == BipedSlot::kHead  || slot == BipedSlot::kBody ||
                         slot == BipedSlot::kHands || slot == BipedSlot::kFeet))
                        genuine = true;
                    continue;
                }
                if (a_key.score <= 0 && worn->GetArmorRating() > 0.0f)
                    return false;      // a generic piece never strips real armor
                if (!MageKeyBeats(a_key, MageApparelKey(worn, a_school)))
                    return false;      // beaten (or tied) on a slot it would replace
                if (a_key.score <= 0) genuine = true;   // strictly beats worn rags
            }
            return overlapsAny && genuine;
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

        // Retained for reference / possible reuse; loot no longer skill-forces a
        // weapon role (#weapon-switch), so this is not currently called.
        [[maybe_unused]] WepClass BestWeaponClass(RE::Actor* a_f) {
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
                if (g.enabled && g.actionOpcode == a_op) return true;   // a toggled-OFF rule doesn't count (Fable)
            return false;
        }

        // ── #69: the STABLE weapon-role signal ──────────────────────────────
        // LootEquipment (what to loot/keep) and ShedOffRoleWeapon (what to hand
        // back) used to each infer "melee" / "ranged" from whatever weapon was
        // MOMENTARILY WIELDED -- but loot and shed run at DIFFERENT times, so
        // each call caught whatever happened to be drawn at THAT instant. A
        // hybrid 1h/bow follower's role flipped between calls (his 1h got shed
        // while the bow was drawn), and a custom follower's own signature
        // weapon -- drawn only sometimes -- read as "off-role" mid-fight and
        // got handed to the player (Feris's Gauldurbow, marth's field report).
        // The fix: judge the role from what the follower actually MAINTAINS --
        // a gambit first, else whatever they physically CARRY -- never what's
        // in hand right now. WHAT gets force-EQUIPPED over a drawn weapon is a
        // wholly separate decision (LootEquipment's equipIt gate, untouched);
        // this only decides what's looted, kept, or shed.
        struct WeaponRoles {
            WepClass melee        = WepClass::Other;   // Other = no melee role at all
            bool     doRanged     = false;
            bool     wantCrossbow = false;              // meaningful only when doRanged
        };

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

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false) {
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
            const WepClass meleeTargetClass =
                (mageMode && !wantsMelee)  ? WepClass::Other : roles.melee;
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
            bool          hasBackup    = false;   // magic user already carries a qualifying sidearm
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
                    hasBackup = true;
                if (doRanged && w->GetWeaponType() == (wantCrossbow ? WT::kCrossbow : WT::kBow))
                    myRangedDmg = std::max(myRangedDmg, w->GetAttackDamage());
            }

            RE::TESBoundObject* bestArmor     = nullptr;
            float               bestArmorRat  = 0.0f;   // best-first, like bestWeapDmg (marth's rule)
            RE::TESBoundObject* bestWeap      = nullptr;
            std::uint16_t       bestWeapDmg   = baseDmg;
            RE::TESBoundObject* bestRanged    = nullptr;
            std::uint16_t       bestRangedDmg = myRangedDmg;
            RE::TESBoundObject* bestMage      = nullptr;   // school/fanciness apparel (magic user)
            MageKey             bestMageKey{};             // {0, -1} so any real key beats it
            RE::TESBoundObject* bestBackup    = nullptr;   // the mage's one melee sidearm
            std::uint16_t       bestBackupDmg = 0;

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
                    const bool shieldUseless = isShield && meleeTargetClass != WepClass::OneHand;
                    // MAGE APPAREL (v1.0.29; EXCLUSIVE since v1.0.31): a magic
                    // user's apparel is judged by school match then fanciness
                    // (MageApparelIsBetter), never by armor rating. Shields
                    // are skipped outright for him -- a shield is rated armor,
                    // and school gear never lives there.
                    if (mageMode && !isShield) {
                        LogMageApparelDiag(armo, school);   // one dump per form+school (deduped inside)
                        const MageKey mk = MageApparelKey(armo, school);
                        if (MageApparelIsBetter(a_follower, armo, school, mk) &&
                            MageKeyBeats(mk, bestMageKey)) {
                            bestMageKey = mk;
                            bestMage    = obj;
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
                    if (!mageMode &&
                        !shieldUseless && ArmorIsBetter(a_follower, armo) &&
                        armo->GetArmorRating() > bestArmorRat) {
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
                    // MAGE BACKUP (v1.0.29): the sidearm a caster's own AI
                    // draws when his magicka is gone. Daggers only by default
                    // (bMageDaggersOnly); the toggle opens it to the best of
                    // any one-hander. Only fires while he carries NONE -- one
                    // sidearm, never an armory, and no upgrade churn to
                    // re-trigger on every corpse.
                    if (wantBackup && !hasBackup && wc == WepClass::OneHand &&
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
                    // kHair/kCirclet joined in v1.0.29: mage hoods occupy the
                    // hair bit and circlets their own, and a school upgrade on
                    // those slots must carry its gems like any other.
                    static constexpr Slot kSlots[] = {
                        Slot::kHead, Slot::kHair, Slot::kCirclet,
                        Slot::kBody, Slot::kHands, Slot::kForearms,
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

            // PUT IT ON -- BUT HANDS ARE GAMBIT TERRITORY (marth: the equipped
            // weapon must be STABLE; roles switch only on a gambit or a cast).
            // Loot re-equips only IN PLACE: the new weapon must replace the same
            // role currently in the hand (melee over melee, bow over bow,
            // crossbow over crossbow), or the hand must be weaponless. Acquiring
            // the OTHER role -- equip-melee while wielding a bow, equip-ranged
            // while wielding a sword, anything while holding a staff -- STOCKS
            // the pack only; the combat equip gambit (or their own AI) decides
            // when it goes in the hand. The unconditional EquipObject here was
            // the out-of-combat half of the weapon thrash: every corpse
            // re-decided the hand. Armor always equips (slots, not hands); a
            // slot-conflicting piece auto-unequips the worse one. queue=true so
            // the engine applies it on its own next update rather than
            // synchronously re-entering here.
            bool equipIt = true;
            if (auto* nw = best->As<RE::TESObjectWEAP>(); nw && myWeap) {
                const auto     newWt   = nw->GetWeaponType();
                const WepClass newRole = WeaponClassOf(newWt);
                equipIt = WeaponClassOf(myWeap->GetWeaponType()) == newRole &&
                          (newRole != WepClass::Ranged || myWeap->GetWeaponType() == newWt);
            }
            // The mage BACKUP is STOCK-ONLY, even into an empty hand (the
            // myWeap==nullptr case above would otherwise equip it): a caster's
            // hands belong to his spells/staff, and vanilla AI draws the
            // sidearm ITSELF at zero magicka -- equipping here would shove a
            // dagger over his casting hand out of combat, the very thrash the
            // equip-in-place rule exists to stop.
            if (best == bestBackup) equipIt = false;
            if (equipIt) {
                // #62 EQUIP ON THE MAIN THREAD -- the invisible-head fix. This whole
                // loot path runs on the AddTask JOB WORKER (Scheduler::Tick <- the
                // Diagnostics sleeper's AddTask, which drains on BSJobs::JobThread --
                // MainThread.h/§0.37). EquipObject rebuilds the actor's biped 3D, and
                // that rebuild touches the HEAD/NECK partition even for a plain CHEST
                // piece; doing it off the main thread races the render thread and the
                // head node is torn down but not rebuilt -> the "head disappears on
                // armor equip" bug. It reproduces with a perfectly good item like
                // chainmail (marth), which proves it is the EQUIP mechanism, not the
                // item's meshes. 3D mutation is exactly what MainThread::Post exists
                // to marshal. Capture FormIDs (never the worker's Actor*/item, which
                // may be stale next frame) and re-resolve on the frame that runs.
                const RE::FormID folID  = a_follower->GetFormID();
                const RE::FormID itemID = best->GetFormID();
                auto doEquip = [folID, itemID]() {
                    auto* fol  = RE::TESForm::LookupByID<RE::Actor>(folID);
                    auto* form = RE::TESForm::LookupByID(itemID);
                    auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
                    // #62 broken-gear eviction is handled by BeastHeadSink on the
                    // TESEquipEvent this equip fires -- there, not here, so the same
                    // check also covers the player TRADING gear over and the AI
                    // re-dressing, not just MFO's own equip. Keep this to the equip.
                    // (MFO's own loot never reaches here with creature gear: the
                    // IsCreatureArmor filter skips it at selection.)
                    if (auto* eq = RE::ActorEquipManager::GetSingleton(); fol && item && eq)
                        eq->EquipObject(fol, item);
                };
                if (MainThread::IsInstalled()) MainThread::Post(doEquip);
                else                           doEquip();   // VR: pump is a no-op, keep the direct path
            }

            // [equip] DIAGNOSTIC: the weapon swap was INVISIBLE in the log --
            // marth's "Erik switches melee weapons for no reason" could not be
            // seen. Log WHAT we put on, over WHAT, and the class reasoning that
            // chose it, so a soak shows a real upgrade vs a thrash (e.g. a bow-
            // user whose skill-class is melee getting a melee weapon forced on).
            if (auto* nw = best->As<RE::TESObjectWEAP>()) {
                spdlog::info("[equip] {:08X}: LOOT-{} weapon '{}' dmg={} class={} <- held '{}' "
                             "dmg={} class={} | meleeTgt={} wantsMelee={} wantsRanged={} baseDmg={}",
                             a_follower->GetFormID(), equipIt ? "EQUIP" : "STOCK",
                             nw->GetFullName() ? nw->GetFullName() : "?", nw->GetAttackDamage(),
                             static_cast<int>(WeaponClassOf(nw->GetWeaponType())),
                             myWeap && myWeap->GetFullName() ? myWeap->GetFullName() : "(none)",
                             myWeap ? myWeap->GetAttackDamage() : 0,
                             myWeap ? static_cast<int>(WeaponClassOf(myWeap->GetWeaponType())) : -1,
                             static_cast<int>(meleeTargetClass), wantsMelee, wantsRanged, baseDmg);
            } else {
                // #62: armor now equips on the MAIN thread (queued via the pump when
                // live), so the biped/head rebuild never races the render thread.
                spdlog::info("[equip] {:08X}: LOOT armor '{}' -> equip {}", a_follower->GetFormID(),
                             best->As<RE::TESFullName>() && best->As<RE::TESFullName>()->GetFullName()
                                 ? best->As<RE::TESFullName>()->GetFullName() : "?",
                             MainThread::IsInstalled() ? "queued to main thread" : "direct (VR/no-pump)");
            }

            // MAGIC-LOADOUT diagnostics (v1.0.29): say WHY the mage item won,
            // so a field soak shows the school reasoning, not just a transfer.
            // Logged only on a TAKE (never during peeks), so it cannot spam.
            if (best == bestMage || best == bestBackup) {
                spdlog::info("[loot] {:08X} '{}' magic-user: target school {} (from {} cast gambit(s))",
                             a_follower->GetFormID(),
                             a_follower->GetName() ? a_follower->GetName() : "?",
                             SchoolName(school), castGambits);
            }
            if (best == bestMage) {
                auto* ma  = best->As<RE::TESObjectARMO>();
                float mag = 0.0f;
                const int sc = SchoolMatchScore(ma, school, &mag);
                spdlog::info("[loot] robe {:08X} '{}' school {} score={} value={} mag={:.0f} -> best",
                             best->GetFormID(), best->GetName() ? best->GetName() : "?",
                             SchoolName(school), sc, ma->GetGoldValue(), mag);
            }
            if (best == bestBackup) {
                auto* mw = best->As<RE::TESObjectWEAP>();
                spdlog::info("[loot] mage backup {} {:08X} '{}' dmg={} -- stocked; his own AI draws it when the magicka runs out",
                             daggersOnly ? "dagger" : "1h", best->GetFormID(),
                             best->GetName() ? best->GetName() : "?",
                             mw ? mw->GetAttackDamage() : 0);
            }
            if (best == bestMage && fromUid != 0)
                spdlog::info("[loot] MEO gems carried to new robe (from {:08X} uid {})", fromBase, fromUid);

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

            // IN COMBAT drink the STRONGEST (survival first). OUT of combat, drink the
            // WEAKEST potion that still covers the missing amount -- topping up with an
            // Ultimate potion to heal a scratch is the wasteful "burns the best potion"
            // tell (Fable P10). If nothing single-handedly covers the deficit, fall to
            // the strongest to close the most gap.
            const bool  inCombat = a_follower->IsInCombat();
            float deficit = 0.0f;
            if (!inCombat) {
                if (auto* avo = a_follower->AsActorValueOwner())
                    deficit = std::max(0.0f, avo->GetPermanentActorValue(a_which) -
                                             avo->GetActorValue(a_which));
            }

            RE::AlchemyItem* strong = nullptr; float strongMag = -1.0f;
            RE::AlchemyItem* cover  = nullptr; float coverMag  = std::numeric_limits<float>::max();
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc || PotionRestores(alc) != a_which) continue;
                // Magnitude of the costliest effect -- the same effect PotionRestores
                // classified on, so ranking is by the resource the potion is FOR.
                const auto* eff = alc->GetCostliestEffectItem();
                const float mag = eff ? eff->GetMagnitude() : 0.0f;
                if (mag > strongMag) { strongMag = mag; strong = alc; }
                if (!inCombat && mag + 0.5f >= deficit && mag < coverMag) { coverMag = mag; cover = alc; }
            }
            RE::AlchemyItem* best = (!inCombat && cover) ? cover : strong;
            if (!best) {
                // REGRESSION GUARD [potprobe]: a wanted drink found no potion. If the
                // follower carries alchemy anyway, a classifier regression (a mod
                // reworking potion archetypes -> PotionRestores misreads them) is the
                // suspect -- dump the raw archetype/AV so it is diagnosable. Silent
                // when the follower simply has no alchemy. Rate-limited 15s/follower.
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextPotProbe;
                auto& pn = s_nextPotProbe[a_follower->GetFormID()];
                const auto tnow = std::chrono::steady_clock::now();
                if (pn.time_since_epoch().count() == 0 || tnow >= pn) {
                    pn = tnow + std::chrono::seconds(15);
                    std::string dump;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* alc = obj->As<RE::AlchemyItem>();
                        if (!alc || alc->IsFood() || alc->IsPoison()) continue;
                        const auto* eff = alc->GetCostliestEffectItem();
                        auto* mgef = eff ? eff->baseEffect : nullptr;
                        dump += std::format(" [{} x{} arch={} primAV={} -> restores={}]",
                            alc->GetFullName() ? alc->GetFullName() : "?", data.first,
                            mgef ? static_cast<int>(mgef->data.archetype) : -1,
                            mgef ? static_cast<int>(mgef->data.primaryAV) : -1,
                            static_cast<int>(PotionRestores(alc)));
                    }
                    if (!dump.empty())
                        spdlog::info("[potprobe] {:08X} want={} -- no match, but carries alchemy:{}",
                                     a_follower->GetFormID(), static_cast<int>(a_which), dump);
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

        // OPTION A travel state -- up to kMaxLootSlots travellers AT ONCE (one loot
        // quest, four alias pairs; see g_travelSlots below). Worker-tick-only, NOT
        // serialized: this just remembers the intent; the engine-side alias fill is
        // cleared by Packages on load (#55). kArrivalDist ~= arm's reach: once the
        // engine walks the follower this close, the existing inventory transfer runs.
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
        // P7 MULTI-SLOT: up to kMaxLootSlots concurrent loot excursions, one per
        // slot. Slot i maps to the loot quest's alias PAIR (actor 2*i, target
        // 2*i+1). Slot 0 is byte-identical to the shipped single-slot state.
        // Logistics owns this follower->slot map; Packages is passed the slot.
        std::array<TravelIntent, Packages::kMaxLootSlots> g_travelSlots{};

        // The active slot whose intent belongs to a_follower (nullptr if none).
        TravelIntent* SlotOf(RE::FormID a_follower) {
            if (!a_follower) return nullptr;
            for (auto& t : g_travelSlots)
                if (t.active && t.follower == a_follower) return &t;
            return nullptr;
        }
        // Index of a_follower's active slot, or -1.
        int SlotIndexOf(RE::FormID a_follower) {
            if (!a_follower) return -1;
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (g_travelSlots[i].active && g_travelSlots[i].follower == a_follower)
                    return i;
            return -1;
        }
        // First free (inactive) slot, or -1 if all kMaxLootSlots are busy.
        int FreeSlotIndex() {
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (!g_travelSlots[i].active) return i;
            return -1;
        }
        // Is ANY excursion in flight? Gates the global blocklist reassess.
        bool AnyTravelActive() {
            for (auto& t : g_travelSlots)
                if (t.active) return true;
            return false;
        }
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
        // STRAIGHT to sticky, no strike accrual (loose-loot fix, marth field report:
        // Xelzaz cycled 000D1EFx loose gold forever). Two definitive verdicts skip
        // the 2-strike patience: (1) a LOOSE ref that stalls -- a gold/ammo pile
        // never moves, so one no-progress verdict is GEOMETRIC (off-navmesh, on a
        // table/shelf), not a transient block; and (2) a ref the follower REACHED
        // but could not acquire (Activate dispatch failed) -- arrival ERASED its
        // strikes (3551) so it would otherwise re-pick every 25s. Either way the
        // excursion abandons it for the sticky window instead of looping.
        void MarkTravelSticky(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            g_stallStrikes.erase(a_id);
            g_travelFailed[a_id]  = a_now;  EvictOldest(g_travelFailed);
            g_travelUnreach[a_id] = a_now;  EvictOldest(g_travelUnreach);
            spdlog::info("[loot] {:08X} STICKY-unreachable (loose/unacquirable) -- won't re-pick "
                         "for {}m", a_id,
                         std::chrono::duration_cast<std::chrono::minutes>(kTravelStickyCooldown).count());
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
            Clock::time_point farSince{};    // player has been beyond departRadius since this (departure release)
        };
        std::unordered_map<RE::FormID, Claim> g_claim;
        constexpr float kTickSecs = 1.0f;   // ~kLogisticsInterval, the fair-chance accrual step
        constexpr float kDepartRelease = 3.0f;   // player near then gone this long -> release Valuables (P3)

        // ── #69: a follower's OWN gear, snapshotted ONCE at first management ──
        // ShedOffRoleWeapon's exemption list: the Gauldurbow fix. Part C above
        // makes role inference stable; this is the belt -- whatever a follower
        // already owned the moment MFO started managing them (recruit, or an
        // existing save's first post-load service) is off limits to the shed
        // path FOREVER, role logic notwithstanding.
        //
        // THREADING: unlike every other map on this page (g_claim, g_nextTick,
        // etc. -- worker-tick-only, never touched by serialization, per the
        // comments above), this one is written by BOTH the logistics WORKER
        // (EnsureStockSnapshot, called from ServiceFollower) and the co-save's
        // Save/Load/RevertCallback (Serialization.cpp), which run on the real
        // main thread (State.h: "serialization callbacks run on main") -- a
        // DIFFERENT thread from the worker (MainThread.h/§0.37: AddTask drains
        // on BSJobs::JobThread, never main). A save can happen mid-session
        // while the worker is live, so this IS a real cross-thread map and the
        // no-lock discipline above does not apply here. Guarded by g_stockMx,
        // locked in every accessor -- never held across an engine call.
        std::mutex g_stockMx;
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_stockGear;

        // Snapshot a_follower's current weapon/armor base FormIDs as "stock",
        // but ONLY the first time -- an existing entry means this follower was
        // already snapshotted this session, or loaded from the co-save. Runtime
        // (0xFF) bases are skipped (INVARIANTS #9): a dynamic form id is
        // meaningless next session and must never be persisted.
        void EnsureStockSnapshot(RE::Actor* a_follower) {
            const auto id = a_follower->GetFormID();
            { std::scoped_lock lk(g_stockMx); if (g_stockGear.contains(id)) return; }   // fast path
            // Build the set with the lock RELEASED -- GetInventory() is an engine
            // call and g_stockMx must never be held across one (a main-thread
            // SaveCallback could be blocked on it).
            std::unordered_set<RE::FormID> stock;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (!obj->As<RE::TESObjectWEAP>() && !obj->As<RE::TESObjectARMO>()) continue;
                const auto baseID = obj->GetFormID();
                if (!Followers::IsPersistableID(baseID)) continue;   // #9: never a runtime form
                stock.insert(baseID);
            }
            std::scoped_lock lk(g_stockMx);
            if (g_stockGear.contains(id)) return;   // raced another snapshot -- keep the first
            spdlog::info("[stock] {:08X}: snapshotted {} gear item(s) at first management -- never shed",
                         id, stock.size());
            g_stockGear.emplace(id, std::move(stock));
        }

        // Is a_baseID part of a_followerID's snapshotted stock? ShedOffRoleWeapon's
        // one guard against ever giving away a follower's own gear.
        bool IsStockGear(RE::FormID a_followerID, RE::FormID a_baseID) {
            std::scoped_lock lk(g_stockMx);
            const auto it = g_stockGear.find(a_followerID);
            return it != g_stockGear.end() && it->second.count(a_baseID) != 0;
        }

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
                        RE::ActorValue a_potionWant = RE::ActorValue::kNone,
                        LootMode a_mode = LootMode::kNormal) {
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
                        const float pdist = playerPos.GetDistance(srcPos);
                        if (dt > 0.0f && pdist <= Config::g_chanceRadius.load()) {
                            cl.everNear = true;
                            cl.farSince = {};   // back in range -> the departure timer resets
                            if (PlayerIsConsidering(srcId))    cl.nearSecs += dt * 3.0f;
                            else if (PlayerFacing(pc, srcPos)) cl.nearSecs += dt;
                        } else if (pdist > Config::g_departRadius.load() &&
                                   cl.farSince.time_since_epoch().count() == 0) {
                            cl.farSince = a_now;   // player has left the source's vicinity
                        }
                    }

                    const bool rejected = ClaimRejected(srcId, srcPos, playerPos, a_now);
                    bool released;
                    if (a_cat == Category::Equipment) {          // Gear tier
                        released = rejected ||
                                   std::chrono::duration<float>(a_now - cl.seen).count()
                                       >= Config::g_firstDibsDelay.load();   // gear grace
                    } else {                                     // Gold / Jewelry / SoulGems -> Valuables
                        // DEPARTURE release (P3): the player walked near this source
                        // (everNear) but never took from it and has now left its
                        // vicinity for kDepartRelease seconds -- the common "fight
                        // near me, I loot the two I want, walk on" case. Without it,
                        // everNear disabled the abandon backstop and the rest waited
                        // forever. Complements: rejection, the 6s near+facing chance,
                        // and the never-came-near abandon timer.
                        const bool departed = cl.everNear &&
                            cl.farSince.time_since_epoch().count() != 0 &&
                            std::chrono::duration<float>(a_now - cl.farSince).count() >= kDepartRelease;
                        released = rejected || departed ||
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
                    // P7: excursion mode means this follower is already claimed, so
                    // a live slot must exist -- resolve it up front and drive it.
                    const int s = SlotIndexOf(a_follower->GetFormID());
                    if (s < 0) continue;   // no live slot: nothing to drive
                    TravelIntent& tr = g_travelSlots[s];
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
                    if (Packages::LootTravelRetarget(a_follower, ref, s)) {
                        tr.target   = ref->GetHandle();
                        tr.cat      = a_cat;
                        tr.want     = a_potionWant;
                        tr.deadline = TravelDeadline(df, a_now);
                        tr.phase    = TravelPhase::Walking;
                        tr.lastPos    = origin;   // reset no-progress tracker
                        tr.progressAt = a_now;
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
                    // CHURN GUARD (#48, computed above): he's already past the
                    // release margin -- an arm now dies "left leash" next tick.
                    // Skip the candidate entirely: it is far (or loose), so
                    // there is no in-place transfer to fall through to.
                    if (followerBeyondLeash) continue;
                    // Too far to WALK without abandoning the player -- leave it
                    // (following brings them closer later; also fairer to you).
                    if (df > walkLimit) continue;
                    // Skip a target a walk already failed to reach (no churn).
                    if (TravelFailedRecently(rid, a_now)) continue;
                    // CONVERGENCE YIELD: never walk to loot the player is right
                    // next to -- you win the race for the corpse you're heading to.
                    if (playerPos.GetDistance(ref->GetPosition()) <= Config::g_playerBubble.load())
                        continue;
                    // P7: claim the first FREE slot; skip if all are busy.
                    const int s = FreeSlotIndex();
                    if (s < 0) continue;
                    // OFF-NAVMESH GATE (see the excursion path): no mesh near the
                    // ref -> no path -> freeze. Skip + blocklist before dispatch.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelStalled(rid, a_now);   // off-navmesh -> stall strike
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
                if (isLoot) {
                    // Equipment moves ONE piece per call (§4.3's shape) while the
                    // other categories take all they want internally -- but this
                    // visit is the corpse's LAST (it's marked DONE right after we
                    // return). One call stranded everything past the first take:
                    // a body with a better sword AND the bow the equip-ranged
                    // gambit needs (dual-primary) yielded only the sword, and the
                    // bow/cuirass sat blocklisted for 25 s or forever. Drain the
                    // category: each take raises the inventory-derived baseline,
                    // so this terminates; the guard is belt-and-braces (2 weapon
                    // roles + 7 armor slots).
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
            if (SlotOf(fid)) return;
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
                // KEEP THE LOADOUT: a follower who fights with a bow AND a melee weapon
                // only ever has ONE worn at a time, so the sheathed other reads unworn
                // and was SOLD (marth). Protect the best weapon of EACH weapon CLASS --
                // 1H, 2H, bow, crossbow, staff kept SEPARATELY (Fable: merging 1H+2H or
                // bow+crossbow by raw damage let a junk greatsword/crossbow win the keep
                // and the real weapon get sold). Only worse in-class duplicates are junk.
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
                    std::unordered_map<int, std::pair<RE::TESBoundObject*, std::uint16_t>> best;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* w = obj->As<RE::TESObjectWEAP>();
                        if (!w || IsCreatureWeapon(w)) continue;
                        const int b = bucketOf(w->GetWeaponType());
                        if (b < 0) continue;
                        auto& slot = best[b];
                        if (!slot.first || w->GetAttackDamage() >= slot.second)
                            slot = { obj, w->GetAttackDamage() };
                    }
                    for (auto& [b, s] : best) if (s.first) keepWeapons.insert(s.first);
                }

                std::vector<TradeBridge::SellRow> sell;
                int purse = 0;
                // A MEO-socketed instance carries an ExtraUniqueID -- NEVER sell one:
                // the sale discards the follower's socketed gems with the weapon
                // (marth). Worn gear is already barred below; this catches an UNWORN
                // gemmed weapon the loadout keep-set didn't protect. Conservative --
                // a bare uid (socket later emptied) also stays, which is the safe
                // direction (keep a weapon vs lose gems). Same extraList/ExtraUniqueID
                // read MEOBridge::WornUid uses, worker-safe against a loaded follower.
                auto socketed = [](RE::InventoryEntryData* e) {
                    if (!e || !e->extraLists) return false;
                    for (auto* xl : *e->extraLists)
                        if (auto* uid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr;
                            uid && uid->uniqueID != 0) return true;
                    return false;
                };
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (obj->GetFormID() == 0x0000000F) { purse += static_cast<int>(data.first); continue; }
                    auto* weap = obj->As<RE::TESObjectWEAP>();
                    auto* armo = obj->As<RE::TESObjectARMO>();
                    if (!weap && !armo) continue;
                    if (weap && keepWeapons.count(obj)) continue;   // loadout weapon, not junk
                    RE::BGSKeywordForm* kwf = weap
                        ? static_cast<RE::BGSKeywordForm*>(weap)
                        : static_cast<RE::BGSKeywordForm*>(armo);
                    auto* entry = data.second.get();
                    if (entry && entry->IsWorn())                continue;   // never sell worn gear
                    if (socketed(entry))                         continue;   // MEO-gemmed -> never sell (gems ride the item)
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

        // ── #62 BROKEN-GEAR sink ────────────────────────────────────────────
        // A trade / AI equip can put (or re-assert) a non-playable creature item or
        // a foreign non-rendering head piece on a follower -> invisible head. This
        // sink catches such equips at the EQUIP EVENT and posts KeepHeadClear, which
        // DELETES the creature junk and hands back the wrong-race piece (see its
        // definition -- plugin-ownership keeps the follower's own native gear).
        // Scoped to any TEAMMATE, out of combat, NAMED armor/weapon equips only (a
        // cheap trigger gate), debounced ~1s. KeepHeadClear's own unequip fires an
        // equipped=false event, ignored below -- no cascade.
        class BeastHeadSink final : public RE::BSTEventSink<RE::TESEquipEvent> {
        public:
            static BeastHeadSink* GetSingleton() { static BeastHeadSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_ev || !a_ev->equipped || !a_ev->actor)
                    return RE::BSEventNotifyControl::kContinue;
                if (!Config::g_beastHeadFix.load())
                    return RE::BSEventNotifyControl::kContinue;
                // Only WORN gear (armor or weapon) matters here.
                auto* base = a_ev->baseObject ? RE::TESForm::LookupByID(a_ev->baseObject) : nullptr;
                if (!base || (!base->Is(RE::FormType::Armor) && !base->Is(RE::FormType::Weapon)))
                    return RE::BSEventNotifyControl::kContinue;
                // TRIGGER only on NAMED gear equips (a cheap gate to avoid firing on
                // a follower's own hidden native-gear churn). The actual keep/clear
                // separation is done by plugin ownership inside KeepHeadClear.
                auto* named = base->As<RE::TESFullName>();
                if (!named || !named->GetFullName() || !*named->GetFullName())
                    return RE::BSEventNotifyControl::kContinue;
                auto* actor = a_ev->actor->As<RE::Actor>();
                if (!actor || !actor->IsPlayerTeammate() || actor->IsInCombat())
                    return RE::BSEventNotifyControl::kContinue;
                // DEBOUNCE (~1s/follower): coalesces a full-set trade's burst of
                // equip events. Main-thread, so the static map needs no lock.
                static std::unordered_map<RE::FormID, Clock::time_point> s_lastReset;
                const auto       now = Clock::now();
                const RE::FormID id  = actor->GetFormID();
                if (auto it = s_lastReset.find(id);
                    it != s_lastReset.end() && now - it->second < std::chrono::seconds(1))
                    return RE::BSEventNotifyControl::kContinue;
                s_lastReset[id] = now;
                // A trade/AI equip can leave (or re-assert) a non-rendering head item
                // on a follower. Post KeepHeadClear (next frame, after the
                // equip settles): it takes OFF any worn head-slot piece with no mesh
                // for his race -- the invisible-head cause -- and leaves everything
                // else, including his hidden native gear, untouched.
                MainThread::Post([id]() {
                    if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id))
                        KeepHeadClear(a);
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

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

    float PotionMagnitude(RE::AlchemyItem* a_potion) {
        if (!a_potion) return 0.0f;
        const auto want = PotionRestores(a_potion);   // catalog-authoritative type
        if (want == RE::ActorValue::kNone) return 0.0f;
        // Largest magnitude among the potion's restore effects on that resource.
        // Same archetype gate as PotionRestores (value modifiers only, so fortify
        // and cure never count), so the number belongs to the resource the potion
        // is classified under.
        float best = 0.0f;
        for (const auto* e : a_potion->effects) {
            const auto* mgef = e ? e->baseEffect : nullptr;
            if (!mgef) continue;
            const auto arch = mgef->data.archetype;
            if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
                arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) continue;
            if (mgef->data.primaryAV != want) continue;
            const float mag = e->GetMagnitude();
            if (mag > best) best = mag;
        }
        return best;
    }

    bool AmmoIsBolt(RE::TESAmmo* a_ammo) {
        switch (Catalog::AmmoKind(a_ammo->GetFormID())) {
        case Catalog::Ammo::kArrow: return false;
        case Catalog::Ammo::kBolt:  return true;
        default:                    return a_ammo->IsBolt();
        }
    }

    float PotionLootFloor() {
        return Config::g_minPotionMag.load() > 0
                   ? static_cast<float>(Config::g_minPotionMag.load())
                   : g_autoPotionFloor;
    }

    void ComputeWeakPotionFloor() {
        g_autoPotionFloor = 0.0f;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        std::vector<float> mags;
        for (auto* alc : dh->GetFormArray<RE::AlchemyItem>()) {
            if (!alc || alc->IsPoison() || alc->IsFood()) continue;
            if (PotionRestores(alc) == RE::ActorValue::kNone) continue;   // restore potions only
            if (const float m = PotionMagnitude(alc); m > 0.0f) mags.push_back(m);
        }
        if (mags.size() < 2) {
            spdlog::info("[potfloor] {} restore potion(s) in load order -- no low-power floor", mags.size());
            return;
        }
        std::sort(mags.begin(), mags.end());
        // Tier boundary: a magnitude beyond 1.25x the weakest tier's floor starts a
        // new tier; the SECOND tier's floor is the low-power cutoff (the weakest tier
        // sits below it). Relative, so it adapts to the list's own potion scale.
        const float t1 = mags.front();
        for (float m : mags) if (m > t1 * 1.25f) { g_autoPotionFloor = m; break; }
        std::string ladder; float last = -1.0f;   // distinct-ish tiers, for tuning transparency
        for (float m : mags)
            if (last < 0.0f || m > last * 1.25f) { ladder += std::to_string(static_cast<int>(m + 0.5f)); ladder += ' '; last = m; }
        spdlog::info("[potfloor] restore-potion tiers: {}| auto low-power floor = {:.0f} "
                     "(restore potions below this are ignored; iMinPotionMag overrides)",
                     ladder, g_autoPotionFloor);
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

    // Equip a carriable torch the follower holds (moved here from combat, #35 --
    // torch is upkeep; pair with "In an interior"/"At night"). No-op if a light is
    // already in hand or none is carried.
    bool EquipTorch(RE::Actor* a_follower) {
        if (auto* l = a_follower->GetEquippedObject(true); l && l->As<RE::TESObjectLIGH>())
            return false;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* light = obj->As<RE::TESObjectLIGH>();
            if (!light || !light->CanBeCarried()) continue;
            if (auto* mgr = RE::ActorEquipManager::GetSingleton()) mgr->EquipObject(a_follower, light);
            return true;
        }
        return false;
    }

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

    // SHED OFF-ROLE WEAPONS (marth). A follower carrying a weapon of a role they
    // do NOT maintain -- a 2H on a 1H fighter, a crossbow on a bow user, any ranged
    // on a melee-only fighter -- lets their game AI equip the wrong thing "of its
    // own volition", and it is dead weight / a leftover from pre-1.0.12 skill-forced
    // loot. Hand ONE such weapon back to the PLAYER per idle tick (recoverable,
    // never destroyed). The role set is the SAME gambit-driven one LootEquipment
    // uses (keep in sync), so MFO never sheds a class it would loot. NEVER sheds an
    // in-role class, a SOCKETED weapon (gems), a catalog-excluded/quest weapon, a
    // creature weapon, or a staff; never runs for a follower with NO weapon role (a
    // caster); never leaves them with zero in-role weapons. Runs OUT of combat, so
    // unequipping a worn off-role weapon is safe. RemoveItem-to-player is the same
    // worker-safe move LootAmmo already uses.
    bool ShedOffRoleWeapon(RE::Actor* a_follower, const FollowerState& a_state) {
        using WT = RE::WEAPON_TYPE;

        // #69: the SAME stable role signal LootEquipment loots/keeps against
        // (ComputeWeaponRoles, kept in sync by construction) -- never the
        // momentarily-wielded weapon. A follower who carries both a melee and
        // a ranged weapon keeps BOTH roles regardless of which is drawn right
        // now, so this can no longer shed what loot just decided to keep.
        const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);

        // MAGIC LOADOUT (v1.0.29): the mage's one-hand BACKUP is IN-ROLE. A
        // magic user (>= 1 enabled cast gambit -- the SAME gambit-driven test
        // the loot side uses, keep in sync) keeps his sidearm even though no
        // equip-melee gambit authors a melee role for him; without this, a
        // caster who ALSO has a ranged role would shed the dagger the loot
        // pass just fetched, corpse after corpse (loot->shed thrash). A pure
        // caster never reaches the shed at all (the no-role early-return
        // below), so this only matters for the mixed roles.
        int castGambits = 0;
        if (Config::g_magicLoadout.load()) TargetMagicSchool(a_state, castGambits);
        const bool magicUser   = castGambits > 0;
        const bool daggersOnly = Config::g_mageDaggersOnly.load();

        const WepClass meleeRole    = roles.melee;
        const bool     doRanged     = roles.doRanged;
        const bool     wantCrossbow = roles.wantCrossbow;
        if (meleeRole == WepClass::Other && !doRanged) return false;   // no role -> not ours to judge

        auto inRole = [&](const RE::TESObjectWEAP* w) {
            const WepClass wc = WeaponClassOf(w->GetWeaponType());
            // The magic user's sidearm class is always his to keep (v1.0.29).
            if (magicUser && wc == WepClass::OneHand &&
                (!daggersOnly || w->GetWeaponType() == WT::kOneHandDagger)) return true;
            if (wc == WepClass::OneHand || wc == WepClass::TwoHand) return wc == meleeRole;
            if (wc == WepClass::Ranged) {
                if (!doRanged) return false;
                const auto t = w->GetWeaponType();
                return wantCrossbow ? (t == WT::kCrossbow) : (t == WT::kBow);
            }
            return true;   // fists/other -> never a shed target
        };
        auto socketed = [](RE::InventoryEntryData* e) {
            if (!e || !e->extraLists) return false;
            for (auto* xl : *e->extraLists)
                if (auto* uid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr; uid && uid->uniqueID != 0)
                    return true;
            return false;
        };

        RE::TESBoundObject* shed = nullptr; std::int32_t shedCount = 0; int inRoleWeapons = 0;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* w = obj->As<RE::TESObjectWEAP>();
            if (!w || w->IsStaff()) continue;
            if (inRole(w)) { ++inRoleWeapons; continue; }
            if (IsCreatureWeapon(w) || Catalog::IsExcluded(obj->GetFormID())) continue;
            // #69: never shed the follower's OWN gear, snapshotted at first
            // management (the Gauldurbow fix) -- an off-role signature weapon
            // stays in the pack, not handed to the player.
            if (IsStockGear(a_follower->GetFormID(), obj->GetFormID())) continue;
            if (socketed(data.second.get())) continue;
            if (!shed) { shed = obj; shedCount = data.first; }   // first off-role, one per tick
        }
        if (!shed || inRoleWeapons == 0) return false;   // don't disarm

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        const char* nm = shed->GetName() ? shed->GetName() : "?";
        a_follower->RemoveItem(shed, shedCount, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
        // Tell the player where the weight went (P9): a silent hand-off looks like a
        // mystery inventory gain. Reuses the HUD-notification toggle.
        if (Config::g_rapportToasts.load())
            RE::DebugNotification(std::format("{} hands you a {}.",
                a_follower->GetName() ? a_follower->GetName() : "Your follower", nm).c_str());
        spdlog::info("[shed] {:08X}: off-role weapon '{}' x{} (class {}) -> player | meleeRole={} doRanged={} xbow={}",
                     a_follower->GetFormID(), nm, shedCount,
                     static_cast<int>(WeaponClassOf(shed->As<RE::TESObjectWEAP>()->GetWeaponType())),
                     static_cast<int>(meleeRole), doRanged, wantCrossbow);
        return true;
    }

    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower) return;
        g_svc = &a_state;   // loot code reads the gambit table through this (worker-sequential)

        // #69: snapshot the follower's OWN gear the very FIRST time MFO manages
        // them -- before HealExcludedWeapon/ShedOffRoleWeapon/loot touch a
        // single item. First management is recruit for a new follower (nothing
        // looted yet); a no-op for anyone already snapshotted this session or
        // loaded from the co-save.
        EnsureStockSnapshot(a_follower);

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
        // P7: sweep EVERY slot -- each traveller has his own excursion cap, and a
        // subsystem toggle-off must free them all. (Global backstop, keyed to no
        // follower in particular; runs once per serviced follower, idempotent.)
        for (int i = 0; i < Packages::kMaxLootSlots; ++i) {
            auto& tr = g_travelSlots[i];
            if (!tr.active) continue;
            const bool capped = now > tr.startTime + std::chrono::seconds(
                                          static_cast<int>(Config::g_excursionMax.load()));
            if (capped || off) {
                // On a cap hit blacklist the current target so it isn't re-picked
                // immediately. NOT on toggle-off (that corpse never "failed").
                if (!off) {
                    if (auto tp = tr.target.get()) MarkTravelFailed(tp->GetFormID(), now);
                }
                Packages::LootTravelClear(off ? "subsystem off" : "excursion cap", nullptr, i);
                tr = TravelIntent{};
            }
        }

        if (!Config::g_logistics.load()) return;   // whole subsystem off by default (#45)

        // CADENCE GATE (~1 s). Cheap early-out on the frames between logistics
        // ticks -- the Scheduler calls this every time it services the follower
        // out of combat (up to ~7.5 Hz), but logistics only acts at the idle rate.
        auto& due = g_nextTick[id];
        if (due.time_since_epoch().count() != 0 && now < due) return;
        due = now + kLogisticsInterval;

        // Hand back one off-role weapon per idle tick (AI-usable wrong-role gear /
        // pre-1.0.12 leftovers). Cheap when the pack is clean; stops on its own.
        ShedOffRoleWeapon(a_follower, a_state);

        // ── BATCH EXCURSION driver. While THIS follower is on a loot excursion
        // (claimed at priority 60), drive it: walk to the current target, grab it
        // on arrival, then seek the NEXT target and retarget WITHOUT releasing --
        // he hoovers a batch instead of returning to the player after each corpse.
        // Release only on combat, the excursion cap, leaving the leash, or the
        // batch running dry (after a short dibs linger).
        if (const int slot = SlotIndexOf(id); slot >= 0) {
            TravelIntent& tr = g_travelSlots[slot];
            auto* pc = RE::PlayerCharacter::GetSingleton();
            // HARD interrupts -> END the excursion.
            const bool overCap = now > tr.startTime + std::chrono::seconds(
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
                                          a_follower, slot);
                tr = TravelIntent{};
                // fall through to a normal eval this tick (combat table / follow).
            } else if (tr.acquirePending) {
                // ── ACQUIRE READBACK (route 2b probe). One tick after the
                // Activate dispatch, observe what the VM call actually did --
                // dispatch is asynchronous (Papyrus.h), so the previous tick's
                // call has had its frame(s) by now. Ref gone / inventory up =
                // the engine took it natively; ref standing with a flat count =
                // the activation was a no-op.
                tr.acquirePending = false;
                auto aptr = tr.target.get();
                auto* aref = aptr.get();
                const bool refGone = !aref || aref->IsDeleted() || aref->IsDisabled();
                std::int32_t post = 0;
                for (auto& [obj, n] : a_follower->GetInventoryCounts())
                    if (obj && obj->GetFormID() == tr.acquireBase) { post = n; break; }
                const std::int32_t delta = post - tr.acquirePre;
                if (refGone || delta != 0)
                    spdlog::info("[acquire] {:08X}: TOOK {:08X} -- ref {}, inv {:+}",
                                 id, tr.acquireRefID,
                                 refGone ? "gone" : "persists", delta);
                else
                    spdlog::info("[acquire] {:08X}: ACTIVATE NO-OP -- ref persists, inv unchanged",
                                 id);
                // Either way this leg is DONE: blocklist the ref (a persisting
                // no-op must not be re-picked every tick) and continue the batch.
                MarkTravelFailed(tr.acquireRefID, now);
                tr.phase = TravelPhase::Holding;
                tr.lingerUntil = now + std::chrono::seconds(
                                          static_cast<int>(Config::g_batchLinger.load()));
                return;
            } else if (tr.phase == TravelPhase::Walking) {
                auto tptr  = tr.target.get();
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
                                     Forms::IsTravelPackage(cur),
                                     cur ? cur->GetFormID() : 0u,
                                     Forms::g_lootQuest ? static_cast<int>(Forms::g_lootQuest->data.priority) : -1,
                                     dist, pathSpeed, NavmeshReach(a_follower, tref));
                    }
                }
                // Track real movement for the no-progress check below (update on
                // any >kMoveEps world move since the last note).
                const RE::NiPoint3 fpos = a_follower->GetPosition();
                if (tref && fpos.GetDistance(tr.lastPos) > kMoveEps) {
                    tr.lastPos    = fpos;
                    tr.progressAt = now;
                }
                const bool gone = !tref || tref->IsDisabled() || tref->IsMarkedForDeletion();

                // ARRIVAL is checked BEFORE the stall/deadline giveup: a follower
                // standing ON the corpse has ARRIVED, not "stalled walking" -- and
                // the considering/sneak HOLD below legitimately keeps him stationary
                // (he's waiting out YOUR QuickLoot). Letting the no-progress timer
                // fire during that hold would falsely blacklist a corpse he reached
                // and is politely waiting on (audit).
                // #(loose-loot fix): a LOOSE item Activates from a longer reach
                // (g_looseAcquireDist, default 300) -- the engine picks it up
                // regardless of physical distance, so a follower who walked as
                // close as the navmesh allows still grabs a pile the corpse-tight
                // 160u arm's reach would strand (marth: reachable gold, pickup
                // distance too small). A corpse still needs true arm's reach.
                const float arrivalDist = (tref && LooseRef(tref))
                                        ? Config::g_looseAcquireDist.load() : kArrivalDist;
                if (!gone && dist <= arrivalDist) {
                    // REACHED it -> this body is provably reachable: clear any stall
                    // strike so a merely-transient earlier block (boxed in by an actor,
                    // a door) never accumulates toward the sticky verdict. Only bodies
                    // he can NEVER close on keep striking -> those go sticky (above).
                    g_stallStrikes.erase(tref->GetFormID());
                    // MUTATION BAR (#22g / #22g-QL) + sneak courtesy hold.
                    if (PlayerIsConsidering(tref->GetFormID()) || PlayerActivelyStealthing())
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
                        // NATIVE activate on the MAIN THREAD (loose-loot fix, marth:
                        // "it's simple -- check other looting mods"). The VM
                        // ObjectReference.Activate dispatch returned false for EVERY
                        // gold pile (field: Xelzaz reached them, `ACTIVATE dispatch
                        // FAILED` x9, never picked up). TESObjectREFR::ActivateRef is
                        // the exact call the engine runs when the PLAYER presses
                        // activate -- the reliable path loot mods use -- marshalled
                        // to the main thread via MainThread::Post so it stays off the
                        // §0.30 job worker (no PickUpObject/AddTask; the same hop
                        // #62's equip uses). Fire-and-forget: the readback next tick
                        // (inventory delta) decides took vs no-op.
                        const RE::FormID refID = tref->GetFormID();
                        const RE::FormID folID = a_follower->GetFormID();
                        auto doActivate = [refID, folID]() {
                            auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID);
                            auto* f = RE::TESForm::LookupByID<RE::Actor>(folID);
                            if (r && f) r->ActivateRef(f, 0, nullptr, 1, false);   // full processing = the pickup
                        };
                        if (MainThread::IsInstalled()) MainThread::Post(doActivate);
                        else                           doActivate();
                        spdlog::info("[acquire] {:08X}: ACTIVATE (native/main-thread) ref {:08X} ('{}' x{})",
                                     id, tref->GetFormID(), iname ? iname : "?", icount);
                        tr.acquirePending = true;
                        tr.acquireRefID   = tref->GetFormID();
                        tr.acquireBase    = base ? base->GetFormID() : 0;
                        tr.acquirePre     = pre;
                        return;   // the activate IS this tick's action; readback next tick
                    }
                    // Take EVERYTHING his gambits want in this one visit, not just
                    // the category the trip was for -- else gold trips strand the
                    // arrows (marth's 340u/382u bodies). Only THEN is the corpse
                    // genuinely DONE and safe to blocklist.
                    const bool moved = StripCorpse(a_follower, a_state, tref, now);
                    MarkTravelFailed(tref->GetFormID(), now);   // fully stripped -> DONE
                    tr.phase = TravelPhase::Holding;
                    tr.lingerUntil = now + std::chrono::seconds(
                                              static_cast<int>(Config::g_batchLinger.load()));
                    spdlog::info("[loot] {:08X}: arrived -- {} (batch continues)", id,
                                 moved ? "looted" : "nothing to take");
                    return;   // the transfer IS this tick's action; seek next tick
                }

                // NOT arrived: leg fails on vanished target, no-progress (no path),
                // or the leg deadline. Blacklist it and seek another leg THIS tick.
                const bool stalled = tref && now - tr.progressAt > kNoProgress;
                if (gone || stalled || now > tr.deadline) {
                    if (tref) {
                        // A stall (zero progress) is a reachability verdict -> strike
                        // toward sticky; a vanished target or a plain deadline is not.
                        // A LOOSE pile that stalls is beyond even the generous
                        // g_looseAcquireDist Activate reach AND never moves -> a
                        // single verdict is geometric, so sticky it straight off
                        // rather than strike-accrue across a cluster (the churn loop).
                        if (stalled) {
                            if (LooseRef(tref)) MarkTravelSticky(tref->GetFormID(), now);
                            else                MarkTravelStalled(tref->GetFormID(), now);
                        } else {
                            MarkTravelFailed(tref->GetFormID(), now);
                        }
                    }
                    if (stalled && tref)
                        spdlog::info("[loot] {:08X} unreachable {:08X} (no progress, dist={:.0f}) -- skipping",
                                     id, tref->GetFormID(), dist);
                    tr.phase = TravelPhase::Holding;
                    tr.lingerUntil = now + std::chrono::seconds(
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
            if (tr.active && tr.phase == TravelPhase::Holding) {
                // The two loot bars (a ContainerMenu open, or the player sneaking)
                // make LootNearby early-return BEFORE it scans, so the excursion
                // scan would come back empty with g_scanSawWaiting false and be
                // misread as "batch exhausted" -> premature release. Treat them as
                // a HOLD (bounded by the excursion cap), the same courtesy arrival
                // gives -- don't let a menu/crouch gut the batch.
                auto* ui = RE::UI::GetSingleton();
                if ((ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) ||
                    PlayerActivelyStealthing())
                    return;   // hold, retry next tick
                if (RunExcursionScan(a_follower, a_state, now)) {
                    // Retargeted (phase now Walking) or grabbed a cluster corpse
                    // (still Holding) -- productive, so extend the linger.
                    if (tr.phase == TravelPhase::Holding)
                        tr.lingerUntil = now + std::chrono::seconds(
                                                  static_cast<int>(Config::g_batchLinger.load()));
                    return;
                }
                if (g_scanSawWaiting && now <= tr.lingerUntil)
                    return;   // loot still under your dibs -- hold, re-scan next tick
                Packages::LootTravelClear("batch done", a_follower, slot);
                tr = TravelIntent{};
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
            else if (op == Vocab::kActEquipTorch)         acted = EquipTorch(a_follower);   // #35: torch is upkeep
            else if (op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                     op == Vocab::kActCastPlayer) {
                // CAST IN LOGISTICS: out-of-combat gambit casting -- candlelight on
                // SELF or on the PLAYER (light follows you), magelight, self-buffs,
                // out-of-combat heals. Out of combat the follower's AI never casts
                // on its own, so the combat AI-grace path never fired (deck
                // 2026-08-06). SELF and PLAYER go through CastSpellImmediate (the
                // package route bars self-delivery, Decline::SelfRoute; the immediate
                // route applies the effect to any target). A FOE target goes through
                // the animated package (CastAt).
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(choice.actionParam);
                if (!sp || !a_follower->HasSpell(sp)) {
                    start = choice.ruleIndex + 1; continue;   // unknown spell -> next rule
                }
                // Resolve the EFFECT target: self, the player, or the current foe.
                RE::Actor* tgt = a_follower;
                if (op == Vocab::kActCastPlayer) {
                    tgt = RE::PlayerCharacter::GetSingleton();
                } else if (op == Vocab::kActCastTarget) {
                    // Foe selectors never fill the target out of combat -- an empty
                    // handle must SKIP, not fall back to self (a package cast at
                    // self is not barred and would fire a hostile spell at the
                    // follower). cast on self/player is what the immediate route is for.
                    auto p = choice.target.get();
                    if (!p.get()) { start = choice.ruleIndex + 1; continue; }
                    tgt = p.get();
                }
                if (!tgt) { start = choice.ruleIndex + 1; continue; }
                const bool immediate = (op != Vocab::kActCastTarget);   // self/player -> immediate route
                // Skip re-casting a buff still on the TARGET (candlelight up on him
                // or the player), then pace by the spell's DURATION (a 60s light
                // refreshes as it expires; a 3s floor keeps instant spells off the
                // ~1s tick -- heals stay condition-gated).
                if (immediate) {
                    auto* ei   = sp->GetCostliestEffectItem();
                    auto* mgef = ei ? ei->baseEffect : nullptr;
                    if (auto* mt = tgt->AsMagicTarget(); mgef && mt && mt->HasMagicEffect(mgef)) {
                        start = choice.ruleIndex + 1; continue;
                    }
                }
                // Pace per (follower, SPELL) -- keying on the follower alone would
                // let one cast's window starve a sibling cast rule (candlelight
                // blocking an OOC heal). Composite key like g_drinkUntil.
                static std::unordered_map<std::uint64_t, Clock::time_point> s_logiCastUntil;
                const std::uint64_t castKey = (static_cast<std::uint64_t>(id) << 32) | sp->GetFormID();
                if (auto it = s_logiCastUntil.find(castKey); it != s_logiCastUntil.end() && now < it->second) {
                    start = choice.ruleIndex + 1; continue;   // within this spell's window
                }
                if (immediate) {
                    // CastSpellImmediate applies the effect to `tgt` for any delivery.
                    // No charge animation, but the light/buff/heal lands. Spends no
                    // magicka, so gate on affordability and deduct the cost by hand.
                    const float cost = sp->CalculateMagickaCost(a_follower);
                    auto* avo = a_follower->AsActorValueOwner();
                    if (avo && avo->GetActorValue(RE::ActorValue::kMagicka) < cost) {
                        start = choice.ruleIndex + 1; continue;   // can't afford it
                    }
                    if (auto* caster = a_follower->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                        caster->CastSpellImmediate(sp, false, tgt, 1.0f, false, 0.0f, a_follower);
                        if (avo) avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                        RE::ActorValue::kMagicka, -cost);
                        acted = true;
                    }
                } else if (Packages::Available()) {
                    acted = (Packages::CastAt(a_follower, sp, tgt) == Packages::Decline::None);
                }
                if (acted) {
                    auto* ei = sp->GetCostliestEffectItem();
                    const float dur = std::max(3.0f, ei ? static_cast<float>(ei->GetDuration()) * 0.9f : 3.0f);
                    s_logiCastUntil[castKey] = now + std::chrono::duration_cast<Clock::duration>(
                                                        std::chrono::duration<float>(dur));
                    const char* route = op == Vocab::kActCastPlayer ? "player (immediate)"
                                      : immediate                   ? "self (immediate)"
                                                                    : "target (package)";
                    spdlog::info("[logistics] {:08X} OOC cast {:08X} ({}), refresh in {:.0f}s",
                                 id, sp->GetFormID(), route, dur);
                }
            }
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
            if (!AnyTravelActive()) {
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
            // #21 econ scan. GATED behind bEconomy (off by default) AND po3 presence
            // (Fable RC#3): with the toggle off, nothing runs -- no dispatch, no
            // Papyrus, no log -- so a user who hasn't opted in (or lacks po3) never
            // touches the merchant path. Runs on the WORKER, not MainThread::Post
            // (Fable #1/#4): the reads (follower GetInventory / CountPotions /
            // g_travel) must share the thread with the loot/heal/loadout mutations in
            // this same task or they race InventoryChanges (the Actor.cpp:445 CTD
            // class). The merchant read + transaction run in Papyrus (VM), so nothing
            // here needs main; dispatch is worker->VM, like DispatchActivate.
            if (Config::g_economy.load() && Po3Present())
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
        holder->AddEventSink<RE::TESEquipEvent>(BeastHeadSink::GetSingleton());   // #62 beast-head reattach
        spdlog::info("[logistics] player-looted waiver + beast-head equip sinks installed");
    }

    // #62 ON-LOAD broken-gear clean. A follower can come up from a save already
    // wearing non-playable creature gear (the verified Inigo/draugr-helmet case) or
    // a foreign non-rendering head item -> invisible head. So on load, run
    // KeepHeadClear once per loaded teammate. Retries ~2s so a follower that finishes
    // loading a few frames late (or before Followers::Refresh populates g_active) is
    // still caught; a `done` set makes it fire once per follower. The follower's own
    // native gear is left untouched. Called from kPostLoadGame/kNewGame; main thread.
    // Gated on bBeastHeadFix.
    void SweepBeastHeadsOnLoad() {
        if (!Config::g_beastHeadFix.load()) return;
        auto done  = std::make_shared<std::unordered_set<RE::FormID>>();
        auto tries = std::make_shared<int>(120);   // ~2s window for late-loading followers
        auto self  = std::make_shared<std::function<void()>>();
        *self = [done, tries, self]() {
            for (const auto& h : Followers::g_active) {
                auto* a = h.get().get();
                if (!a || !a->IsPlayerTeammate() || !a->Is3DLoaded()) continue;
                const RE::FormID id = a->GetFormID();
                if (done->count(id)) continue;
                KeepHeadClear(a);
                done->insert(id);
            }
            if (--(*tries) > 0) MainThread::Post(*self);
            else                *self = nullptr;   // break the self-capture cycle (running fn is the pump's copy)
        };
        MainThread::Post(*self);
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
        for (int i = 0; i < Packages::kMaxLootSlots; ++i) {
            if (g_travelSlots[i].active) Packages::LootTravelClear("revert", nullptr, i);
            g_travelSlots[i] = TravelIntent{};
        }
        g_travelFailed.clear();
        g_travelUnreach.clear();
        g_stallStrikes.clear();
        g_idleCycles.clear();
        g_lastBlocklistReassess = {};
    }

    void ReleaseTravelOnCombat(RE::Actor* a_follower) {
        if (!a_follower) return;
        const int slot = SlotIndexOf(a_follower->GetFormID());
        if (slot >= 0) {
            // EVICT him from the loot alias and re-evaluate NOW so the combat
            // table / his own AI takes over this tick, not on the engine's slow
            // pass. The corpse is not "failed" -- he can finish it after the
            // fight -- so no LRU mark.
            Packages::LootTravelClear("combat", a_follower, slot);
            g_travelSlots[slot] = TravelIntent{};
        }
    }

    void OnFollowerRemoved(RE::FormID a_id) {
        // A follower dismissed DURING an excursion may still hold alias 0 (Clear
        // hasn't run). With his framework claim gone, MFO's static-60 claim is his
        // sole one -- he'd walk to the stale corpse and re-latch every load.
        // LootTravelEvictIf no-ops unless a_id is the current holder; between
        // excursions the slot holds the player, so this is normally a no-op.
        Packages::LootTravelEvictIf(a_id);
        // Forget the live intent too, if he was the active traveller (his slot).
        if (const int slot = SlotIndexOf(a_id); slot >= 0)
            g_travelSlots[slot] = TravelIntent{};
    }

    // ── #69: co-save companions for g_stockGear ─────────────────────────────
    // Locked accessors so Serialization.cpp never reaches into the anonymous
    // namespace's map directly -- the same hand-off shape every other module's
    // co-save side uses.

    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> CopyStockGear() {
        std::scoped_lock lk(g_stockMx);
        return g_stockGear;   // copied out UNDER the lock; SaveCallback writes the copy lock-free
    }

    void LoadStockRecord(RE::FormID a_followerID, std::unordered_set<RE::FormID> a_set) {
        std::scoped_lock lk(g_stockMx);
        g_stockGear[a_followerID] = std::move(a_set);
    }

    void ClearStockGear() {
        std::scoped_lock lk(g_stockMx);
        g_stockGear.clear();
    }

}
