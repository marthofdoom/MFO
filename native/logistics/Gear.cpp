// logistics/Gear.cpp -- the gear JUDGE: armor preference and scoring, head-slot
// clearing, perk style votes, weapon roles, creature/quest-item guards and the
// safe AcquireEquip step.
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // ROAD 2 (A/B): ch.19 kIntent_Travel loot travel

namespace MFO::Logistics {

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
        // ── ARMOR CLASS BY SKILL + PERKS (marth 2026-09-14) -- THE JUDGE ──────
        // See Logistics_internal.h (kArmorClassBias / kArmorPerkBias / ArmorPref)
        // for the field finding and the arithmetic. This is the ONE place the
        // follower's armor class is decided; every rated-armor compare in loot,
        // owned-equip, keep, sell and buy goes through ArmorScore.
        //
        // CATEGORY = the higher BASE armor skill (GetBaseActorValue, the same read
        // as ProgAllocator's DominantArmorSkill -- never the actual value, which a
        // Fortify enchant on the very piece under judgment would move). On an
        // EXACT tie the follower's owned heavy- vs light-conditioned perk ranks
        // decide (StyleVotesFor -- the g_styleMx MIRROR copy, never TallyStyleVotes
        // off-main); still tied -> light (pure casters, both low, dress light).
        // The winning class earns kArmorClassBias; the class the perk votes lead
        // on earns kArmorPerkBias on top, whichever class that is -- so skill
        // decides the category and perks nudge inside it. No votes (catalog
        // unbuilt, addon absent, mirror not yet landed) -> perkBias 1.0 and the
        // decision is skill alone: no assumption of any perk overhaul.
        ArmorPref ArmorPrefFor(RE::Actor* a_follower) {
            ArmorPref p;
            if (!a_follower) return p;
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return p;
            p.heavySkill = avo->GetBaseActorValue(RE::ActorValue::kHeavyArmor);
            p.lightSkill = avo->GetBaseActorValue(RE::ActorValue::kLightArmor);
            const auto votes = StyleVotesFor(a_follower);
            p.heavyVotes = votes.armor[0];   // ArmorKind bit index 0 Heavy, 1 Light
            p.lightVotes = votes.armor[1];
            if (p.heavySkill != p.lightSkill)      p.heavyClass = p.heavySkill > p.lightSkill;
            else if (p.heavyVotes != p.lightVotes) p.heavyClass = p.heavyVotes > p.lightVotes;
            else                                   p.heavyClass = false;
            (p.heavyClass ? p.heavyBias : p.lightBias) *= kArmorClassBias;
            if (p.heavyVotes > p.lightVotes)      p.heavyBias *= kArmorPerkBias;
            else if (p.lightVotes > p.heavyVotes) p.lightBias *= kArmorPerkBias;
            return p;
        }

        // Single-candidate convenience (recomputes the preference: two base AV
        // reads + the mirror lock). Scans compute ArmorPrefFor ONCE and use the
        // pref-taking inline in Logistics_internal.h.
        float ArmorScore(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            if (!a_follower || !a_armo) return 0.0f;
            return ArmorScore(ArmorPrefFor(a_follower), a_armo);
        }

        // ONE [armor] line per follower per CHANGE of {category, worn-set} --
        // deduped through the style mirror exactly like the [style] line
        // (StyleMirror::loggedArmor). Called from EquipBestOwnedGear's rated
        // branch, the per-tick wear decision, so the next field log shows the
        // class MFO decided, the inputs it decided from, and what the follower
        // is actually wearing on every rated slot with its score. The worn-set
        // half of the key means an engine auto-equip that changes a slot is
        // logged the tick after it lands, even with the category unchanged.
        void LogArmorClassIfChanged(RE::Actor* a_follower, const ArmorPref& a_pref) {
            if (!a_follower) return;
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            using AT   = RE::BGSBipedObjectForm::ArmorType;
            struct SlotRow { Slot slot; const char* name; };
            // The head row reads WornInLogicalSlot(0) (Head|Hair|Circlet), not the
            // kHead bit alone -- a worn vanilla helmet (31+42) printed "head (bare)".
            static constexpr SlotRow kRows[] = {
                { Slot::kBody, "body" }, { Slot::kHead, "head" }, { Slot::kHands, "hands" },
                { Slot::kFeet, "feet" }, { Slot::kShield, "shield" },
            };
            RE::TESObjectARMO* worn[5] = {};
            std::uint64_t key = 1469598103934665603ull;   // FNV-1a offset (never 0)
            for (int i = 0; i < 5; ++i) {
                worn[i] = (kRows[i].slot == Slot::kHead) ? WornInLogicalSlot(a_follower, 0)
                                                         : a_follower->GetWornArmor(kRows[i].slot);
                key = (key ^ (worn[i] ? worn[i]->GetFormID() : 0u)) * 1099511628211ull;
            }
            key = (key ^ (a_pref.heavyClass ? 0x48u : 0x4Cu)) * 1099511628211ull;
            bool logIt = false;
            {
                std::scoped_lock lk(g_styleMx);
                auto& m = g_styleMirror[a_follower->GetFormID()];
                if (m.loggedArmor != key) { m.loggedArmor = key; logIt = true; }
            }
            if (!logIt) return;
            auto typeTag = [](RE::TESObjectARMO* a) -> const char* {
                if (!a) return "";
                switch (a->GetArmorType()) {
                case AT::kHeavyArmor: return "Heavy";
                case AT::kLightArmor: return "Light";
                default:              return "Clothing";
                }
            };
            std::string wornStr;
            for (int i = 0; i < 5; ++i) {
                if (!wornStr.empty()) wornStr += ", ";
                if (!worn[i]) { wornStr += std::format("{} (bare)", kRows[i].name); continue; }
                wornStr += std::format("{} '{}' [{}] rat={:.0f} score={:.1f}", kRows[i].name,
                                       worn[i]->GetFullName() ? worn[i]->GetFullName() : "?", typeTag(worn[i]),
                                       worn[i]->GetArmorRating(), ArmorScore(a_pref, worn[i]));
            }
            spdlog::info("[armor] {:08X} '{}': heavy={:.0f} light={:.0f} -> class {} votes h/l={}/{} bias h/l={:.2f}/{:.2f} | worn {}",
                         a_follower->GetFormID(), a_follower->GetName() ? a_follower->GetName() : "?",
                         a_pref.heavySkill, a_pref.lightSkill, a_pref.heavyClass ? "HEAVY" : "LIGHT",
                         a_pref.heavyVotes, a_pref.lightVotes, a_pref.heavyBias, a_pref.lightBias, wornStr);
        }

        // Class-membership predicate: does this piece belong to the follower's
        // armor class? Clothing (no skill) always passes. NO LONGER GATES
        // ArmorIsBetter (2026-09-14): the class is a BIAS inside ArmorScore, not a
        // filter -- a follower with a bare slot takes off-class armor over
        // nothing, and a real tier-up still wins. Kept as the one named
        // statement of the category rule (ProgAllocator's DominantArmorSkill
        // cites it) and rebased onto the SAME judge (ArmorPrefFor: base skill,
        // perk-vote tiebreak, light default) so it can never disagree with the
        // score. Creature armor is filtered upstream (IsCreatureArmor) at every
        // caller, as before.
        bool ArmorClassSuits(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            using AT = RE::BGSBipedObjectForm::ArmorType;
            if (!a_follower || !a_armo) return true;
            const auto type = a_armo->GetArmorType();
            if (type == AT::kClothing) return true;
            const ArmorPref p = ArmorPrefFor(a_follower);
            return (type == AT::kHeavyArmor) ? p.heavyClass : !p.heavyClass;
        }

        // Is this candidate an upgrade over what the follower WEARS, judged by
        // ArmorScore on every armor slot it covers? A bare slot (nothing worn, or
        // rating-0 clothing) counts as an upgrade; a worn piece is displaced only
        // by a STRICTLY higher score (a tie or worse on any covered slot is a no).
        // Rating <= 0 (clothing / jewelry) is never "better armor" here -- the
        // mage-apparel judge owns those. This is what lets an owned LIGHT
        // cuirass (26 x 2.0 = 52) displace the worn HEAVY one (31 x 1.0 = 31)
        // on a light-skilled follower, and stops a heavy 18 helmet from
        // "upgrading" his bare head over a light 13 (13 x 2.0 = 26 > 18).
        bool ArmorIsBetter(const ArmorPref& a_pref, RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            if (!a_follower || !a_armo) return false;
            if (a_armo->GetArmorRating() <= 0.0f) return false;   // clothing / jewellery -- not armor
            const float cand = ArmorScore(a_pref, a_armo);

            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            // HEAD is judged ONCE as a logical slot (IsHeadSlotMask: Head|Hair|
            // Circlet vs WornInLogicalSlot(0), the same three bits) -- a per-bit
            // kHead test read a worn 31+42 vanilla helmet as a BARE head and let a
            // bit-30 Dwemer Helmet (20) "dress" it over a worn Shrouded Cowl
            // (32.5): a scored downgrade (field 2026-09-14). The remaining slots
            // stay per-bit.
            static constexpr Slot kSlots[] = {
                Slot::kBody, Slot::kHands, Slot::kForearms,
                Slot::kFeet, Slot::kCalves, Slot::kShield,
            };
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            bool overlapsAny = false;
            // BARE slot -- nothing worn, or only rating-0 clothing/rags: this
            // piece DRESSES it. Valid to acquire even with no armor to upgrade
            // (the helmetless-follower case). Not a downgrade -> keep scanning.
            // Real worn armor there: only a strictly higher SCORE may replace
            // it. A tie or worse is beaten -> never downgrade real worn armor.
            auto beatenBy = [&](RE::TESObjectARMO* worn) {
                return worn && worn->GetArmorRating() > 0.0f && ArmorScore(a_pref, worn) >= cand;
            };
            if (IsHeadSlotMask(mask)) {
                overlapsAny = true;
                if (beatenBy(WornInLogicalSlot(a_follower, 0))) return false;
            }
            for (const auto slot : kSlots) {
                if (!(mask & static_cast<std::uint32_t>(slot))) continue;
                overlapsAny = true;
                if (beatenBy(a_follower->GetWornArmor(slot))) return false;
            }
            // Acquirable if it actually covers an armor slot (skip amulets/rings
            // whose bits are not in kSlots) and was beaten on none it would
            // replace -- i.e. it upgrades every filled slot and/or dresses a bare one.
            return overlapsAny;
        }

        bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            if (!a_follower || !a_armo) return false;
            return ArmorIsBetter(ArmorPrefFor(a_follower), a_follower, a_armo);
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
        // SCORED (2026-09-14): "at >= its rating" is now "at >= its ArmorScore",
        // or a worn heavy 31 in the pack would block looting the light 26 (score
        // 52) that ArmorIsBetter just judged an upgrade.
        bool IsCreatureArmor(const RE::TESObjectARMO* a_armo);   // fwd (defined near LootEquipment)
        bool CarriesSlotArmorAtLeast(const ArmorPref& a_pref, RE::Actor* a_follower,
                                     RE::TESObjectARMO* a_armo) {
            const float cand = ArmorScore(a_pref, a_armo);
            const auto  mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            if (mask == 0) return false;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* have = obj->As<RE::TESObjectARMO>();
                if (!have || have == a_armo || IsCreatureArmor(have)) continue;
                if (ArmorScore(a_pref, have) < cand) continue;   // worse -> not a competing baseline
                if (static_cast<std::uint32_t>(have->GetSlotMask()) & mask)
                    return true;   // a carried piece already covers this slot at >= score
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

        // The worker-side read of a follower's perk STYLE VOTES (mirror doc in
        // Logistics_internal.h). Returns the last main-thread tally (empty until
        // the first one lands) and, when the copy is older than kStyleRefresh
        // with no refresh already posted, posts one Progression::TallyStyleVotes
        // to the main thread (FormID captured, actor re-resolved on the frame
        // that runs -- the #62 hop discipline).
        Progression::StyleVotes StyleVotesFor(RE::Actor* a_follower) {
            if (!a_follower) return {};
            const auto id  = a_follower->GetFormID();
            const auto now = Clock::now();
            Progression::StyleVotes out;
            bool post = false;
            {
                std::scoped_lock lk(g_styleMx);
                auto& m = g_styleMirror[id];
                out = m.votes;
                if (!m.inFlight && (m.stamp == Clock::time_point{} || now >= m.stamp + kStyleRefresh)) {
                    m.inFlight = true;
                    post = true;
                }
            }
            if (post) {
                if (!MainThread::IsInstalled()) {
                    // VR: no pump. Leave the mirror empty (default behaviour) and
                    // release the in-flight latch so a later install could refresh.
                    std::scoped_lock lk(g_styleMx);
                    g_styleMirror[id].inFlight = false;
                    return out;
                }
                MainThread::Post([id]() {
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                    const Progression::StyleVotes v = actor ? Progression::TallyStyleVotes(actor)
                                                            : Progression::StyleVotes{};
                    std::scoped_lock lk(g_styleMx);
                    auto& m    = g_styleMirror[id];
                    m.votes    = v;
                    m.stamp    = Clock::now();
                    m.inFlight = false;
                });
            }
            return out;
        }

        void ClearStyleMirror() {
            std::scoped_lock lk(g_styleMx);
            g_styleMirror.clear();
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

            // ── STYLE BY PERKS within the melee class (marth 2026-09-13) ─────
            // "Most perked style wins": among the kinds of the chosen class,
            // the kind(s) with the MOST owned perk ranks conditioned on them
            // become preferred (WeaponScore bias). Zero votes, or every kind
            // level, leaves preferKinds 0 -- today's behaviour exactly. The
            // off-hand vote (shield vs dual wield) is derived the same way and
            // logged; it steers nothing yet (WeaponRoles::offHand doc).
            if (roles.melee != WepClass::Other) {
                const auto votes = StyleVotesFor(a_follower);
                using WK = Progression::WeaponKind;
                const std::uint16_t classMask =
                    (roles.melee == WepClass::TwoHand) ? WK::kWkTwoHandAll : WK::kWkOneHandAll;
                int top = 0, lead = 0;
                for (int b = 0; b < 8; ++b)
                    if (classMask & (1u << b)) top = std::max(top, votes.weapon[b]);
                if (top > 0) {
                    std::uint16_t pick = 0; int kinds = 0;
                    for (int b = 0; b < 8; ++b) {
                        if (!(classMask & (1u << b))) continue;
                        ++kinds;
                        if (votes.weapon[b] == top) { pick |= static_cast<std::uint16_t>(1u << b); ++lead; }
                    }
                    if (lead < kinds) roles.preferKinds = pick;   // all level = no preference
                }
                if (votes.leftHandWeapon != votes.leftHandShield + votes.armor[2])
                    roles.offHand = (votes.leftHandWeapon > votes.leftHandShield + votes.armor[2]) ? 2 : 1;
                // One [style] line per follower per CHANGE of the pick (the
                // roles are recomputed every service tick -- never per call).
                const std::uint32_t key = (static_cast<std::uint32_t>(roles.melee) << 24) |
                                          (static_cast<std::uint32_t>(roles.offHand) << 16) |
                                          roles.preferKinds;
                bool logIt = false;
                {
                    std::scoped_lock lk(g_styleMx);
                    auto& m = g_styleMirror[a_follower->GetFormID()];
                    if (m.loggedPick != key) { m.loggedPick = key; logIt = true; }
                }
                if (logIt)
                    spdlog::info("[style] {:08X}: melee class {} prefers kinds 0x{:02X} (votes 1H s/d/a/m={}/{}/{}/{} "
                                 "2H gs/ba/wh={}/{}/{}) offHand={} (dual={} shield={}+{}) armor h/l/s={}/{}/{} | {} of {} owned catalog rank(s) classifiable",
                                 a_follower->GetFormID(), static_cast<int>(roles.melee), roles.preferKinds,
                                 votes.weapon[0], votes.weapon[1], votes.weapon[2], votes.weapon[3],
                                 votes.weapon[4], votes.weapon[5], votes.weapon[6],
                                 static_cast<int>(roles.offHand), votes.leftHandWeapon, votes.leftHandShield, votes.armor[2],
                                 votes.armor[0], votes.armor[1], votes.armor[2],
                                 votes.classified, votes.owned);
            }

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

        // QUEST-OBJECT INSTANCE GUARD (bLootSpecialItems). Per-INSTANCE, not
        // per-base-record: RE::InventoryEntryData::IsQuestObject() is the
        // engine's own "this exact item instance is currently needed by a live
        // quest" check. Checked UNCONDITIONALLY at every loot-acquisition site
        // this toggle opens up -- quest items stay engine-protected regardless
        // of bLootSpecialItems (marth's decision); a base-record catalog
        // exclusion is a coarser, author-time signal and is what the toggle
        // governs.
        bool IsQuestObjectInstance(RE::InventoryEntryData* a_entry) {
            return a_entry && a_entry->IsQuestObject();
        }

        // Loose-ref analog of IsQuestObjectInstance, for route 2b (a loose
        // world item has no InventoryEntryData to ask -- that overload only
        // exists on a CONTAINED item). VERIFIED against the exact pinned
        // CommonLibSSE commit this project builds against (portfile REF
        // c4ab853d, CharmedBaryon/CommonLibSSE): TESObjectREFR itself
        // declares `bool HasQuestObject() const` (TESObjectREFR.h) which
        // calls straight into the engine (RELOCATION_ID 19201/19627, same
        // relocation-call shape as InventoryEntryData::IsQuestObject) -- the
        // real, supported, ref-level counterpart, not a guessed ExtraDataList
        // member.
        bool IsQuestObjectRef(RE::TESObjectREFR* a_ref) {
            return a_ref && a_ref->HasQuestObject();
        }

        // NEVER-LOOT gate for the route-2b whitelist below: mirrors the
        // two-part guard every dibs-tier container looter runs (quest-instance
        // check + bLootSpecialItems catalog exclusion) -- rebased onto a loose
        // ref via IsQuestObjectRef just above. Used by every loose category
        // that also carries this guard in its container form (Jewelry/
        // SoulGems/Ingredients/Equipment/Valuables); Arrows/Bolts/Potions/
        // Lockpicks/Gold have no such gate in their container form either, so
        // none is added for their loose path.
        bool LooseSpecialItemBlocked(RE::TESObjectREFR* a_ref, RE::FormID a_formId) {
            if (IsQuestObjectRef(a_ref)) return true;
            if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(a_formId)) return true;
            return false;
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
        //   a_forceStock      -> keep it in the pack, never into a hand (mage backup,
        //                        the dual wielder's second one-hander). A STOCKED
        //                        item REPLACES NOTHING, so it captures NO gems
        //                        (Fable SEV-2 on b3ac577: the same-role capture
        //                        below would have queued the primary's gems onto
        //                        the second one-hander, and Actuation's left-hand
        //                        equip at the next combat fired that move).
        // Returns true if it was actually equipped (vs stocked). Worker domain only.
        bool AcquireEquip(RE::Actor* a_follower, RE::TESBoundObject* a_item,
                          RE::TESObjectREFR* a_src, RE::TESObjectWEAP* a_myWeap,
                          bool a_forceStock) {
            if (!a_follower || !a_item) return false;

            // MEO gem transfer (#17): capture the OLD worn item this upgrade REPLACES
            // (base + instance uid) BEFORE the swap. CROSS-ROLE IS THE BUG (marth): a
            // new bow must never pull gems off the melee weapon. Same-role/slot only.
            // RULE: gems are captured ONLY when the new item REPLACES the worn one --
            // a stocked item (a_forceStock) replaces nothing, so nothing is captured
            // and no move is queued.
            RE::FormID    fromBase = 0;
            std::uint16_t fromUid  = 0;
            if (MEOBridge::Available() && !a_forceStock) {
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
            // APMF EQUIP AUTHORITY (feat/mfo-equip-authority): with the standing
            // claim live, ARMOR is never equipped directly from here -- the
            // declaration RefreshEquipDeclaration sends at this service tick's tail
            // names the same piece (ComputeOwnedGearPick is the one judge) and APMF
            // equips it; a direct EquipObject would reach the seat as
            // External(MFO.dll) against a set that does not yet carry the item.
            // The transfer (a_src), the keep/sell verdicts and the MEO gem carry
            // below are untouched. WEAPONS keep the direct equip-in-place hop
            // (listed "to migrate", Docs/STATUS.md): the declaration carries only
            // what the hands HOLD, so a just-looted same-role upgrade is armed by
            // the next combat equip gambit instead.
            const bool declaredArmor = equipIt && a_item->As<RE::TESObjectARMO>() != nullptr &&
                                       EquipAuthorityLive(a_follower->GetFormID());
            if (declaredArmor) {
                spdlog::info("[equip] {:08X}: '{}' declared -- APMF equip authority carries the equip "
                             "(no direct EquipObject)", a_follower->GetFormID(),
                             a_item->GetName() ? a_item->GetName() : "?");
            } else if (equipIt) {
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
            // No-op for a stocked item (nothing captured above), if the old item has
            // no MEO instance uid (fromUid == 0) or MEO is
            // absent. NOTE: a nonzero uid means "MEO has tracked this instance", NOT
            // "it has gems" -- an ungemmed-but-tracked piece still queues a move,
            // and MEO's MoveGems then moves nothing (the EquipSink logs its bool).
            MEOBridge::QueueGemMove(a_follower, fromBase, fromUid, a_item->GetFormID());
            return equipIt;
        }

}
