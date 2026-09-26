#pragma once
// Logistics_internal.h -- the Logistics family's SHARED substrate. One TU
// (Logistics.cpp) used to hold all of this in a single anonymous namespace;
// the mechanical module split (Logistics.cpp / _Cast / _Economy / _Loot)
// moved the cross-module state, types, and small helpers here as `inline`
// (ONE shared instance across the four TUs -- never per-TU copies), and
// declares the big cross-module helpers next to the module that defines
// them. Single-module helpers stay file-local in their module. NOT a public
// API: only the four Logistics*.cpp TUs may include this.

#include "PCH.h"
#include "Logistics.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include "cast/Actuation.h"   // cast-in-logistics: reuse the combat cast path (Fire)
#include "CasterConsent.h"  // ClassifySpell: beneficial-vs-hostile OOC cast routing
#include <algorithm>      // std::sort/std::min/std::erase_if (healing stock cap)
#include <cmath>          // std::sin/cos/sqrt for the view cone
#include <unordered_set>  // keepWeapons: best-of-each-class protection set
#include <array>          // P7: fixed-size per-slot travel-intent table
#include <cctype>         // std::tolower: keyword-name school match (v1.0.31)
#include <utility>        // std::pair: the school-name keyword table (v1.0.31)
#include <string_view>    // #coinfix: editorID string match on an item's own keywords
#include "Confidence.h"   // the confidence leash (core tenet)
#include "Packages.h"     // Option A: LootTravelFill / LootTravelClear
#include "Forms.h"        // g_travelPackage / g_lootQuest (WALK diagnostic)
#include "Probe.h"        // Probe::CrosshairTarget (the QuickLoot-aware claim signal)
#include "ItemCatalog.h"  // load-order item catalog: potion class + never-loot exclusions
#include "Progression.h"  // perk STYLE votes (TallyStyleVotes) -- weapon/armor style by perks
#include "MEOBridge.h"    // MEO gem transfer on gear swap (#17) + WornUid
#include "Papyrus.h"      // route 2b acquire probe: VM-dispatched ObjectReference.Activate
#include "MainThread.h"   // the pump (§0.37): live-vendor reads MUST run on the main thread
#include "Sightline.h"    // LoS + line-of-fire gate on the OOC hostile-FF direct fallback
#include "Followers.h"    // #62 on-load beast-head sweep iterates g_active (main thread)
#include <functional>     // #62 self-reposting on-load sweep closure
#include <memory>         // std::shared_ptr for that closure
#include "TradeBridge.h"  // #21 econ bridge: MFO_Trade Papyrus round-trip (Phase 0 self-test)
#include <mutex>          // T#69: g_stockMx -- g_stockGear is a real cross-thread map (worker + co-save)

// <windows.h> is BANNED outside Board.cpp (it #defines GetObject and hijacks
// BGSDefaultObjectManager::GetObject<T>) -- so declare the one Win32 call we
// need by hand, exactly as Targeting.cpp does. Used only for QuickLoot presence.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);

namespace MFO::Logistics {

        // Apparel MFO must NEVER wear or keep, even when it ranks well -- marth's annoyance
        // list (e.g. a light-emitting circlet). Blacklisted pieces are force-sold like a
        // redundant inferior. Case-sensitive name-contains; extend kApparelBlacklist as needed.
        inline bool IsBlacklistedApparel(RE::TESObjectARMO* a_armo) {
            if (!a_armo) return false;
            const char* nm = a_armo->GetName();
            if (!nm || !*nm) return false;
            static constexpr const char* kApparelBlacklist[] = { "Circlet of Light" };
            const std::string_view name = nm;
            for (const char* b : kApparelBlacklist)
                if (name.find(b) != std::string_view::npos) return true;
            return false;
        }

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
        inline std::unordered_map<RE::FormID, Clock::time_point> g_nextTick;

        // POST-BATTLE SHED GATE. The last steady_clock instant each follower was
        // OBSERVED in a fight, stamped by NoteInCombat (below) from the Scheduler's
        // PARTY-combat branch on every party-combat service, own IsInCombat() or
        // not (fix/mfo-party-combat-gate, 2026-09-21). This service path is
        // entered out of party combat, and ALSO for an own-OOC follower inside a
        // party fight when the combat table took no action (Scheduler.cpp's
        // serviceOwnOoc). ShedOffRoleWeapon requires kShedPostBattleDwell to have
        // elapsed since that stamp before it drops anything, so a mid-fight
        // IsInCombat() FLAP (a brief LoS loss / disengage that the Scheduler
        // services the follower straight through) can never mature the dwell, and
        // neither can an own-OOC stretch inside a party fight: the next
        // party-combat service re-stamps `now` and resets the clock. Worker-only,
        // no lock -- both the stamp and the read run on the same BSJobs worker tick,
        // sequential across followers (#4). Save-scoped: cleared on revert.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_lastCombatSeen;

        // The shed's last-logged FISTS verdict per follower (marth's rule: fists
        // are never a valid fighting style unless the Progression add-on is
        // installed AND unarmed perks were allocated through it). Packed
        // (valid | progression<<1 | unarmedVotes<<2) so the `[shed] ... fists`
        // line is written once per CHANGE, not once per idle tick -- a follower
        // carrying one off-role weapon is evaluated every tick forever.
        // Worker-only, no lock (#4); save-scoped, cleared with the dwell map.
        inline std::unordered_map<RE::FormID, std::uint32_t> g_shedFistsLogged;

        // How long a follower must be CONTINUOUSLY out of combat before the shed
        // will drop an off-role weapon -- long enough that a combat LULL (LoS loss,
        // a target dying, a disengage) can't be mistaken for "the battle is over".
        // A vanilla combat lull is sub-second to ~2 s; 3 s clears it with margin.
        constexpr auto kShedPostBattleDwell = std::chrono::seconds(3);

        // The last source the PLAYER took from (Claim-and-Release R1). When their
        // next take is from a DIFFERENT source, the previous one's claim RELEASES
        // -- they took what they wanted there and left the rest. Set by the sink.
        inline RE::FormID g_lastLootSource = 0;

        // Per-follower per-AV drink cooldown (M5): a duration restore potion
        // must not be chain-drunk while its effect is still active. Key is
        // (follower FormID << 8 | av-index).
        inline std::unordered_map<std::uint64_t, Clock::time_point> g_drinkUntil;

        // Per-(follower,SPELL) OOC cast pacing window. Namespace scope (not a
        // function-local static) so ClearTransientState wipes it on revert -- an
        // FF-dynamic follower ID gets reused, and a stale window would MUTE an OOC
        // cast rule in the next save; unbounded, it would also grow across saves.
        // Key composed like g_drinkUntil ((follower << 32) | spell).
        inline std::unordered_map<std::uint64_t, Clock::time_point> g_logiCastUntil;

        // (The OOC heal-effect predicate that classified a streamable concentration
        // heal was removed with the `healStream` stopgap -- concentration now
        // delivers through Actuation::CastTargetDirect, which classifies via
        // CasterConsent::ClassifySpell. See the cast dispatch below.)

        // Refs the PLAYER has taken from -- the waiver (#22h). Presence collapses
        // the delay to fQuickLootWaiver, and the timestamp RESETS on every take
        // so the follower moves in that many seconds after the player's LAST
        // take, not their first (the QuickLoot-IE case). Bounded LRU.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_playerLooted;

        // Evict the oldest entry when a bounded map is over cap. n <= kLruCap and
        // inserts are rare, so the O(n) scan is cheaper than carrying a deque.
        inline void EvictOldest(std::unordered_map<RE::FormID, Clock::time_point>& a_map) {
            if (a_map.size() <= kLruCap) return;
            auto oldest = a_map.begin();
            for (auto it = a_map.begin(); it != a_map.end(); ++it) {
                if (it->second < oldest->second) oldest = it;
            }
            a_map.erase(oldest);
        }

        inline RE::FormID PlayerID() {
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

        // The "low power" cutoff derived from the load order at kDataLoaded: the
        // floor of the SECOND magnitude tier of restore potions, so the weakest
        // tier reads as low power. 0 until computed / when the list has < 2 tiers.
        // ComputeWeakPotionFloor() fills it; LootPotions uses it when iMinPotionMag
        // is 0 (auto). Written once before any loot tick, then read-only.
        inline float g_autoPotionFloor = 0.0f;

        // ── skill-aware weapon selection ────────────────────────────────────
        // marth: "loot equipment based on the follower's combat skills." A weapon
        // upgrade is judged WITHIN the follower's DOMINANT weapon-skill class, and
        // a better in-class weapon beats an out-of-class one they merely happen to
        // hold -- so a two-hander specialist stuck with a dagger will take a
        // greatsword. Classified by the follower's OWN skills and the weapon's
        // type, never by name (§4.8.2). (Armor has its own judge, ArmorScore
        // below: rating x the heavy/light class bias x the perk bias.)
        enum class WepClass : std::uint8_t { OneHand, TwoHand, Ranged, Other };

        inline WepClass WeaponClassOf(RE::WEAPON_TYPE a_t) {
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
        [[maybe_unused]] inline WepClass BestWeaponClass(RE::Actor* a_f) {
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
        inline const FollowerState* g_svc = nullptr;

        // Does this table author the given action anywhere? (e.g. an equip-ranged
        // gambit => the follower is meant to use a bow/crossbow, so loot one.)
        inline bool TableHasAction(const std::vector<Gambit>& a_tab, const char* a_op) {
            for (const auto& g : a_tab)
                if (g.enabled && g.actionOpcode == a_op) return true;   // a toggled-OFF rule doesn't count (Fable)
            return false;
        }

        // ── T#69: the STABLE weapon-role signal ──────────────────────────────
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
            // ── STYLE BY PERKS (marth 2026-09-13) ─────────────────────────
            // Within the melee class above, the weapon KIND(s) the follower's
            // owned perks are most conditioned on (Progression::WeaponKind
            // bits: sword/dagger/war axe/mace inside OneHand, greatsword/
            // battleaxe/warhammer inside TwoHand). 0 = no preference: no
            // votes, or every kind tied — the default case, byte-identical
            // to the pre-style behaviour. A tie BETWEEN leading kinds keeps
            // them all preferred. Consumed by WeaponScore below.
            std::uint16_t preferKinds = 0;
            // Off-hand style the perks vote for: 0 none/tie, 1 shield
            // (shield-conditioned ranks lead), 2 dual wield (left-hand-
            // weapon-conditioned ranks lead). DETECTED AND LOGGED ONLY today:
            // the engine dual-wields an NPC only under a CombatStyle that
            // allows it (MFO_MeleeStyle does not) and the combat equip lives
            // in Actuation -- both outside this change (see MAP.md).
            std::uint8_t  offHand = 0;
        };

        // "Strongly prefers": the multiplier a perk-preferred weapon kind
        // earns in every damage comparison the loot / keep / buy judges run
        // (a bias, never a filter -- a follower with greatsword perks and only
        // a warhammer still fights with the warhammer). 1.5 = a steel
        // greatsword (17) out-scores an ebony warhammer (25); an iron
        // greatsword (15 -> 22.5) does not. Big enough that same-tier loot
        // always follows the perks, small enough that a tier-up still wins.
        constexpr float kStyleBias = 1.5f;

        // Which Progression::WeaponKind a weapon IS -- by the engine's own
        // WeapType keyword on the record (the same vocabulary the perk
        // conditions name), with the WEAPON_TYPE enum as the fallback for a
        // record that lost its keyword. Battleaxe vs warhammer are the same
        // WEAPON_TYPE (kTwoHandAxe) and are told apart ONLY by keyword.
        inline std::uint16_t WeaponKindOf(const RE::TESObjectWEAP* a_w) {
            if (!a_w) return 0;
            using WK = Progression::WeaponKind;
            if (a_w->HasKeywordString("WeapTypeSword"))      return WK::kWkSword;
            if (a_w->HasKeywordString("WeapTypeDagger"))     return WK::kWkDagger;
            if (a_w->HasKeywordString("WeapTypeWarAxe"))     return WK::kWkWarAxe;
            if (a_w->HasKeywordString("WeapTypeMace"))       return WK::kWkMace;
            if (a_w->HasKeywordString("WeapTypeGreatsword")) return WK::kWkGreatsword;
            if (a_w->HasKeywordString("WeapTypeBattleaxe"))  return WK::kWkBattleaxe;
            if (a_w->HasKeywordString("WeapTypeWarhammer"))  return WK::kWkWarhammer;
            switch (a_w->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword:  return WK::kWkSword;
            case RE::WEAPON_TYPE::kOneHandDagger: return WK::kWkDagger;
            case RE::WEAPON_TYPE::kOneHandAxe:    return WK::kWkWarAxe;
            case RE::WEAPON_TYPE::kOneHandMace:   return WK::kWkMace;
            case RE::WEAPON_TYPE::kTwoHandSword:  return WK::kWkGreatsword;
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:      return WK::kWkBow;
            default:                              return 0;   // 2H axe w/o keyword, staff, fists
            }
        }

        // THE in-role melee weapon score every judge compares: base attack
        // damage, times kStyleBias when the weapon is a perk-preferred kind.
        // With no preference (roles.preferKinds == 0) this is exactly the
        // integer attack damage widened to float -- every comparison that
        // used to be `dmgA > dmgB` on uint16 is unchanged in that case.
        inline float WeaponScore(const WeaponRoles& a_roles, const RE::TESObjectWEAP* a_w) {
            if (!a_w) return 0.0f;
            float s = static_cast<float>(a_w->GetAttackDamage());
            if (a_roles.preferKinds != 0 && (WeaponKindOf(a_w) & a_roles.preferKinds) != 0)
                s *= kStyleBias;
            return s;
        }

        // ── ARMOR CLASS BY SKILL + PERKS (marth 2026-09-14) ─────────────────
        // "Highest skill wins the category, most-perked style within a
        // category wins; strong bias, not exclusion; no assumption of Adamant
        // or any overhaul." FIELD (Deck log on 69c5b3c, Fable): after a respec
        // Adelinda (Heavy 15 / Light 69) and Cicero (Heavy 20 / Light 77) kept
        // wearing heavy armor and Cicero BOUGHT more (Falkreath Helmet, rating
        // 18, over light 13s), because every rated-armor compare MFO ran was
        // raw rating vs raw rating with NO class term, and the ENGINE's auto-
        // equip (highest rating per slot, skill-blind) was the actual wearer --
        // MFO never un-wore anything. ArmorClassSuits was the only skill read
        // and a candidate-side FILTER; the votes were consumed only on a tie.
        // THE judge is now ArmorScore (Logistics_Loot.cpp, beside
        // ArmorClassSuits) = rating x classBias x perkBias -- the armor twin of
        // WeaponScore / kStyleBias above -- and EVERY rated-armor compare runs
        // on it: ArmorIsBetter, CarriesSlotArmorAtLeast, LootEquipment's best-
        // first pick, EquipBestOwnedGear's owned-upgrade pick (which is what
        // displaces a worn heavy cuirass with an owned light one), the economy
        // keep buckets, the redundant-inferior force-sell (the trade's
        // RemoveItem is the proven un-wear), BuildBuyThresholds' per-slot
        // baseline and TradeBridge::PlanBuy (TradeBridge::ArmorScoreOf, the
        // same arithmetic). They share ONE score by design: bias one site alone
        // and loot and keep disagree, so a looted piece sells or a bought one
        // is never worn.
        //
        // kArmorClassBias -- the multiplier a piece of the follower's OWN class
        // (the higher BASE armor skill) earns. 2.0, so a same-tier in-class
        // piece always beats the off-class one (vanilla cuirasses):
        //   light specialist: hide 20x2=40 > iron 25; leather 26x2=52 > steel 31;
        //                     elven 29x2=58 > dwarven 34; glass 38x2=76 > ebony 43
        //   heavy specialist: iron 25x2=50 > leather 26; steel 31x2=62 > glass 38
        //                     (every heavy cuirass x2 beats every light one)
        // It is a BIAS, never a filter: a light specialist in hide (20x2=40)
        // still takes a found ebony plate (43) -- a real tier-up wins; a bare
        // slot takes any off-class piece over nothing; clothing (rating 0)
        // scores 0 and rides through untouched, as does creature armor (the
        // IsCreatureArmor filters are upstream of every score).
        constexpr float kArmorClassBias = 2.0f;
        // kArmorPerkBias -- the extra step for the class the follower's OWNED
        // perk ranks are conditioned on (StyleVotes::armor[0] heavy vs [1]
        // light, the leading vote; a tie is no vote). The kStyleBias shape,
        // smaller: it only re-orders pieces when skills and perks disagree, and
        // 1.25 < 2.0 so the skill still wins the category (a heavy-skilled
        // follower with light perks scores heavy x2.0 vs light x1.25).
        constexpr float kArmorPerkBias = 1.25f;

        // A follower's armor preference: the two multipliers ArmorScoreOf
        // applies, plus the inputs (diagnostics for the [armor] line). Computed
        // by ArmorPrefFor (Logistics_Loot.cpp) from BASE skills
        // (GetBaseActorValue -- the same read as ProgAllocator's
        // DominantArmorSkill, so the allocator and the wardrobe agree) and the
        // perk-vote MIRROR (StyleVotesFor, never TallyStyleVotes off-main).
        // Compute it ONCE per scan and hand it to the pref-taking overloads
        // below; the Actor* overloads recompute it per call (AV reads + the
        // mirror lock) and are for single-candidate sites.
        struct ArmorPref {
            float heavyBias  = 1.0f;
            float lightBias  = 1.0f;
            bool  heavyClass = false;   // the winning category (false = light, the tie default)
            float heavySkill = 0.0f;    // base Heavy Armor
            float lightSkill = 0.0f;    // base Light Armor
            int   heavyVotes = 0;       // StyleVotes::armor[0]
            int   lightVotes = 0;       // StyleVotes::armor[1]
        };

        // THE rated-armor score: rating x the follower's multiplier for the
        // piece's ArmorType (heavy / light; anything else -- clothing, a modded
        // rated "clothing" -- is the raw rating). Rating <= 0 scores 0 exactly
        // (clothing / jewelry never compete here; the callers' rating>0 gates
        // stay). The arithmetic is TradeBridge::ArmorScoreOf so the buy planner
        // (which has no Actor on the VM thread, only the worker-built
        // thresholds) ranks a light-vs-heavy piece exactly as loot/keep do.
        inline float ArmorScore(const ArmorPref& a_pref, RE::TESObjectARMO* a_armo) {
            if (!a_armo) return 0.0f;
            return TradeBridge::ArmorScoreOf(a_armo->GetArmorRating(), a_armo->GetArmorType(),
                                             a_pref.heavyBias, a_pref.lightBias);
        }

        // ── THE HEAD SLOT is THREE biped bits, not one (field 2026-09-14) ───
        // Vanilla helmets (Imperial Light Helmet 00013EDB, Elven Helmet
        // 0001391D, Leather/Iron Helmet, Shrouded Cowl 000D2842) carry BOD2 =
        // 31 Hair + 42 Circlet with NO bit 30 Head; IA/Dwemer helmets carry
        // 30+31+42+43. A kHead-only test therefore saw every vanilla helmet
        // as "not a head piece": never bought (ArmorBuySlot -1), never a head
        // baseline, never kept, sold as junk, never looted -- and the inverse,
        // a bit-30 helmet judged a worn 31+42 helmet as a BARE head and MFO
        // scored a DOWNGRADE ("OWNED armor 'Dwemer Helmet' [Heavy] 20 <- worn
        // 'Shrouded Cowl' [Light] 32.5"). ONE predicate for "covers the head",
        // the SAME three bits MageClothingSlot / WornInLogicalSlot(0) already
        // use: ArmorBuySlot, the keep buckets, ArmorIsBetter and the [armor]
        // log all route through it. Rating-0 circlets are still excluded by
        // every consumer's own rating<=0 gate; this is slot identity only.
        inline bool IsHeadSlotMask(std::uint32_t a_mask) {
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            constexpr std::uint32_t kHeadBits =
                static_cast<std::uint32_t>(Slot::kHead) |
                static_cast<std::uint32_t>(Slot::kHair) |
                static_cast<std::uint32_t>(Slot::kCirclet);
            return (a_mask & kHeadBits) != 0;
        }

        // ── PERK-STYLE MIRROR (worker reads, main thread writes) ────────────
        // Progression::TallyStyleVotes reads the base's live perk array, which
        // the allocator mutates on the MAIN thread (AddPerk/RemovePerk realloc
        // it) -- so the tally runs on the main thread (MainThread::Post) and
        // the worker reads a per-follower COPY under g_styleMx, the g_stockMx /
        // g_hmsFiredMask mirror pattern. Refresh cadence sized to the change
        // rate: perks change on a board action or a level-up, never per tick,
        // so a 10 s re-tally is prompt and negligible (one perk-array walk per
        // follower per 10 s). First read of a follower returns NO votes (the
        // default, no-bias case) until the posted tally lands next frame.
        // VR (no pump): Post is a no-op -> votes stay empty -> default behaviour.
        // CLEARED ON LOAD: derived from live perks and never persisted, but a
        // stale copy across a load is NOT harmless -- ShedOffRoleWeapon acts on
        // `votes.unarmed` (a DROP), and StyleVotesFor hands back the previous
        // copy while only posting a refresh. So Serialization::ResetAllState
        // calls Logistics::ClearStyleMirror() (Logistics_Loot.cpp) after
        // StopPump() + MainThread::Clear(): that wipes both the stale copy and
        // the `inFlight` latch (a Post discarded by MainThread::Clear() would
        // otherwise leave the latch set for the process lifetime). Fable F1/F4.
        struct StyleMirror {
            Progression::StyleVotes votes;
            Clock::time_point       stamp{};
            bool                    inFlight = false;
            std::uint32_t           loggedPick = 0xFFFFFFFFu;   // last [style] line's pick (log on change)
            std::uint64_t           loggedArmor = 0;             // last [armor] line's {class, worn-set} key (0 = never; log on change)
        };
        inline std::mutex g_styleMx;
        inline std::unordered_map<RE::FormID, StyleMirror> g_styleMirror;
        constexpr auto kStyleRefresh = std::chrono::seconds(10);

        // Equipment-loot judging context: the follower's combat role/mage-mode
        // gate plus the baselines any weapon/armor candidate must beat, all
        // computed ONCE from the follower's OWN gear (BuildEquipmentContext,
        // Logistics_Loot_Equipment.cpp). Shared type: LootEquipment's
        // container scan AND LooseEquipmentQualifies (route 2b, LootNearby in
        // Logistics_Loot.cpp) build/consume it, so a bare sword on the ground
        // is judged by the EXACT same rule as one in a corpse's pack -- ONE
        // decision path, two call sites, two translation units.
        struct EquipmentContext {
            WepClass      meleeTargetClass  = WepClass::Other;
            bool          doRanged          = false;
            bool          wantCrossbow      = false;
            bool          wantBackup        = false;
            bool          daggersOnly       = false;
            bool          useMageApparel    = false;
            std::uint8_t  mageTop2          = 0;
            bool          mageSchoolPrimary = true;
            bool          mageAllowVillain  = false;
            // Best in-role MELEE weapon already carried, as a WeaponScore (attack
            // damage x the perk-style bias; == the integer damage when the
            // follower has no preferred kind). Every in-role candidate is
            // compared through the SAME WeaponScore(roles, weapon).
            float         baseScore         = 0.0f;
            // SECOND ONE-HANDER (dual wield by perks, 2026-09-14): wantOffHand =
            // roles.offHand == 2 AND the melee target class is OneHand -- the
            // follower fights with two one-handers, so a second in-class one-
            // hander is loot too. offHandBaseScore = the SECOND-best owned in-
            // class one-hander's WeaponScore (a count >= 2 stack counts twice;
            // 0 when he owns fewer than two): the bar the second must beat,
            // as baseScore is the bar for the first. Mirrored in
            // TradeBridge::BuyThresholds for the buy side and in the economy
            // keep buckets (top-2 kept) so loot, keep and buy agree.
            bool          wantOffHand       = false;
            float         offHandBaseScore  = 0.0f;
            WeaponRoles   roles;                   // the roles the score was built from
            ArmorPref     armorPref;               // the armor class/perk bias every rated-armor compare in this scan uses
            std::uint16_t myRangedDmg       = 0;   // best ranged weapon already carried
            std::uint16_t myBackupDmg       = 0;   // best mage sidearm already carried
            bool          wantsMelee        = false;   // diagnostics only
            bool          wantsRanged       = false;   // diagnostics only
            RE::ActorValue school           = RE::ActorValue::kNone;   // diagnostics only
            int           castGambits       = 0;        // diagnostics only
        };

        // ── the looting dispatcher ──────────────────────────────────────────
        // APPEND-ONLY (marth CLAUDE.md hard rule): existing ordinals are
        // frozen, new categories go at the end.
        enum class Category { Arrows, Bolts, Potions, Equipment, Gold, Jewelry, SoulGems, Lockpicks,
                               Ingredients, Valuables };

        // Category label for the [loot] diagnostic. Naming the scanned category is
        // the ONLY way to read the composition line: "empty=36" is meaningless
        // without knowing whether the follower was hunting arrows (rare on bodies)
        // or gold (common) that tick (marth: "he never loots arrows").
        inline const char* CatName(Category a_cat) {
            switch (a_cat) {
            case Category::Arrows:    return "arrows";
            case Category::Bolts:     return "bolts";
            case Category::Potions:   return "potions";
            case Category::Equipment: return "equipment";
            case Category::Gold:      return "gold";
            case Category::Jewelry:   return "jewelry";
            case Category::SoulGems:  return "soulgems";
            case Category::Lockpicks: return "lockpicks";
            case Category::Ingredients: return "ingredients";
            case Category::Valuables: return "valuables";
            default:                  return "?";
            }
        }

        // CLUTTER categories are exempt from Config::g_playerBubble (the
        // convergence-yield gate): lockpicks/arrows/bolts are Free-tier
        // consumables nobody else competes for, so a follower should still
        // grab them right under the player's feet instead of deferring like
        // real Valuables/Gear does. Checked at every bubble-gate site in
        // Logistics_Loot.cpp + the arrival site in Logistics.cpp.
        inline bool IsClutterCat(Category a_cat) {
            return a_cat == Category::Lockpicks || a_cat == Category::Arrows ||
                   a_cat == Category::Bolts;
        }

        // LOOT RUN ORDERING (marth, "no-dibs + closest only first, then the dibs
        // items later in the runs, no pauses"). Mirrors TierReleased's own
        // free-tier branch exactly: Arrows/Bolts/Potions/Lockpicks/Ingredients
        // release at once (no dibs, nobody competes). Equipment (short anti-
        // snatch grace) and Gold/Jewelry/SoulGems/Valuables (full dibs chain)
        // are NOT free-tier for this ordering -- they're deferred to the
        // second pass so a follower always sweeps what's immediately available
        // before ever waiting on a dibs-gated category. Ordering only -- does
        // not touch TierReleased's timing.
        inline bool IsFreeTierCat(Category a_cat) {
            return a_cat == Category::Arrows || a_cat == Category::Bolts ||
                   a_cat == Category::Potions || a_cat == Category::Lockpicks ||
                   a_cat == Category::Ingredients;
        }

        // Same ordering test at the OPCODE level, for the two gambit-table
        // dispatch loops (Logistics.cpp's per-tick loop + RunExcursionScan)
        // that switch on the raw Vocab op string before ever resolving a
        // Category. Kept in lockstep with IsFreeTierCat/TierReleased.
        inline bool IsDibsTierLootOp(const std::string& a_op) {
            return a_op == Vocab::kActLootEquipment || a_op == Vocab::kActLootGold  ||
                   a_op == Vocab::kActLootJewelry   || a_op == Vocab::kActLootSoulGems ||
                   a_op == Vocab::kActLootValuables;
        }

        // Every opcode that names a LOOT action (as opposed to drink/torch/cast,
        // which share the same per-tick fall-through in Logistics.cpp). Used to
        // gate the [L] glyph's real-acquisition stamp (g_justLooted) onto loot
        // opcodes only -- a successful drink/torch/cast must not light "Looting".
        inline bool IsLootOp(const std::string& a_op) {
            return a_op == Vocab::kActLootArrows        || a_op == Vocab::kActLootBolts    ||
                   a_op == Vocab::kActLootPotions        || a_op == Vocab::kActLootHealthPotion ||
                   a_op == Vocab::kActLootStaminaPotion  || a_op == Vocab::kActLootMagickaPotion ||
                   a_op == Vocab::kActLootEquipment      || a_op == Vocab::kActLootGold     ||
                   a_op == Vocab::kActLootJewelry        || a_op == Vocab::kActLootSoulGems ||
                   a_op == Vocab::kActLootLockpicks      || a_op == Vocab::kActLootIngredients ||
                   a_op == Vocab::kActLootValuables;
        }

}

// The loot TRAVEL substrate (travel intents and slots, stall / sticky / gate /
// actor-block state, scan timing) lives in its own header, included HERE so
// every declaration keeps its original position.
#include "LootTravel_internal.h"

namespace MFO::Logistics {

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
        inline std::unordered_map<RE::FormID, Claim> g_claim;
        constexpr float kTickSecs = 1.0f;   // ~kLogisticsInterval, the fair-chance accrual step
        constexpr float kDepartRelease = 3.0f;   // player near then gone this long -> release Valuables (P3)

        // ── T#69: a follower's OWN gear, snapshotted ONCE at first management ──
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
        inline std::mutex g_stockMx;
        inline std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_stockGear;

        // Snapshot a_follower's current weapon/armor base FormIDs as "stock",
        // but ONLY the first time -- an existing entry means this follower was
        // already snapshotted this session, or loaded from the co-save. Runtime
        // (0xFF) bases are skipped (INVARIANTS #9): a dynamic form id is
        // meaningless next session and must never be persisted.
        inline void EnsureStockSnapshot(RE::Actor* a_follower) {
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
        // A shared vanilla master -- common items live here, a follower's unique gear does not.
        inline bool IsVanillaMaster(RE::TESFile* a_file) {
            if (!a_file) return true;
            const std::string_view n = a_file->GetFilename();
            return n == "Skyrim.esm" || n == "Update.esm" || n == "Dawnguard.esm" ||
                   n == "HearthFires.esm" || n == "Dragonborn.esm";
        }

        inline bool IsStockGear(RE::FormID a_followerID, RE::FormID a_baseID) {
            {
                std::scoped_lock lk(g_stockMx);
                const auto it = g_stockGear.find(a_followerID);
                if (it == g_stockGear.end() || it->second.count(a_baseID) == 0) return false;
            }
            // In the snapshot -- but PROTECT it only if it is a SIGNATURE/unique piece, not
            // a standard common item (marth: Auri's plain Iron Daggers should sell; her Bow /
            // Jesper's Armor stay). Signature = artifact/quest, OR enchanted, OR the item
            // comes from the follower's OWN plugin (marth: a follower's unique gear lives in
            // the follower's ESP; common items come from Skyrim.esm / the DLC masters).
            auto* form = RE::TESForm::LookupByID(a_baseID);
            if (!form) return true;                          // unresolved -> conservative keep
            if (Catalog::IsExcluded(a_baseID)) return true;  // artifact / quest
            if (auto* w = form->As<RE::TESObjectWEAP>(); w && w->formEnchanting) return true;
            if (auto* a = form->As<RE::TESObjectARMO>(); a && a->formEnchanting) return true;
            // SOURCE MATCH: item's originating plugin == the follower base-NPC's plugin, and
            // that plugin is not a shared vanilla master (so a vanilla follower's common gear
            // still sells, while a modded follower's dedicated-ESP gear is kept).
            auto* fol      = RE::TESForm::LookupByID<RE::Actor>(a_followerID);
            auto* npc      = fol ? fol->GetActorBase() : nullptr;
            auto* itemFile = form->GetFile(0);
            auto* npcFile  = npc ? npc->GetFile(0) : nullptr;
            if (itemFile && npcFile && itemFile == npcFile && !IsVanillaMaster(itemFile))
                return true;   // from the follower's OWN mod -> signature
            return false;      // standard common item -> sellable / sheddable
        }

        // EvictOldest for the Claim map -- oldest by first-seen. The FormID/time
        // overload above cannot serve it (Claim has no operator<), so this is its
        // own bounded-LRU trim, same shape.
        inline void EvictOldest(std::unordered_map<RE::FormID, Claim>& a_map) {
            if (a_map.size() <= kLruCap) return;
            auto oldest = a_map.begin();
            for (auto it = a_map.begin(); it != a_map.end(); ++it)
                if (it->second.seen < oldest->second.seen) oldest = it;
            a_map.erase(oldest);
        }

        // Econ scan cadence clocks. Namespace scope (not function-local statics) so
        // ClearTransientState wipes them on revert (Fable audit #7): FF-dynamic
        // follower IDs get reused, so a stale 2/8/12 s cooldown must not carry
        // into the next save, and the pair map must not grow unbounded across one.
        inline std::unordered_map<RE::FormID,   Clock::time_point> g_econScan;    // per-follower 2 s
        inline std::unordered_map<RE::FormID,   Clock::time_point> g_econTrade;   // per-follower 8 s
        inline std::unordered_map<std::uint64_t, Clock::time_point> g_econPair;   // per-(follower,vendor) 12 s

        // [L] glyph reliability fix (marth 2026-09-06): stamped ONLY at a confirmed
        // acquisition (LootNearby's own true return, the loose-item Activate
        // readback's inventory delta, StripCorpse's `moved`) -- never at the
        // travel-slot claim, which is neither necessary nor sufficient for a real
        // pickup. Holds an "expires at" time_point exactly like g_econTrade above;
        // Logistics::JustLooted (Logistics.cpp) sizes the window from the real
        // round-robin cadence (partySize * kPumpMs), not a guessed constant.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_justLooted;

    // ── cross-module helper declarations ────────────────────────────────
    // Defined at namespace scope in the named module; every other module
    // calls through these. Default arguments live HERE (the definitions
    // carry none -- C++ forbids repeating them).

    // defined in Logistics_Loot.cpp
    int  AmmoCount(RE::Actor* a_actor, bool a_wantBolt);
    bool FitsCarryWeight(RE::Actor* a_follower, float a_addWeight);
    bool LootAmmo(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_wantBolt,
                  bool a_peek = false);
    bool ArmorClassSuits(RE::Actor* a_follower, RE::TESObjectARMO* a_armo);
    ArmorPref ArmorPrefFor(RE::Actor* a_follower);
    float ArmorScore(RE::Actor* a_follower, RE::TESObjectARMO* a_armo);
    void LogArmorClassIfChanged(RE::Actor* a_follower, const ArmorPref& a_pref);
    bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo);
    bool ArmorIsBetter(const ArmorPref& a_pref, RE::Actor* a_follower, RE::TESObjectARMO* a_armo);
    bool CarriesSlotArmorAtLeast(const ArmorPref& a_pref, RE::Actor* a_follower,
                                 RE::TESObjectARMO* a_armo);
    void KeepHeadClear(RE::Actor* a_actor);
    WeaponRoles ComputeWeaponRoles(RE::Actor* a_follower, const FollowerState& a_state);
    Progression::StyleVotes StyleVotesFor(RE::Actor* a_follower);
    bool IsCreatureWeapon(const RE::TESObjectWEAP* a_w);
    bool IsCreatureArmor(const RE::TESObjectARMO* a_armo);
    RE::TESObjectARMO* WornInLogicalSlot(RE::Actor* a_follower, int a_logicalSlot);
    bool AcquireEquip(RE::Actor* a_follower, RE::TESBoundObject* a_item,
                      RE::TESObjectREFR* a_src, RE::TESObjectWEAP* a_myWeap,
                      bool a_forceStock);
    bool IsJewelryPiece(RE::TESObjectARMO* a_armo);
    bool Po3Present();
    bool InPlayerHome();
    bool PlayerIsConsidering(RE::FormID a_sourceID);
    bool ClaimRejected(RE::FormID a_id, const RE::NiPoint3& a_srcPos,
                       const RE::NiPoint3& a_playerPos, Clock::time_point a_now);
    bool LootHere(RE::Actor* a_follower, RE::TESObjectREFR* a_ref,
                  Category a_cat, RE::ActorValue a_want);
    float NavmeshReach(RE::Actor* a_follower, RE::TESObjectREFR* a_ref);
    bool LooseRef(RE::TESObjectREFR* a_ref);
    bool HasLoot(RE::Actor* a_follower, RE::TESObjectREFR* a_ref,
                 Category a_cat, RE::ActorValue a_want);   // Logistics_Loot.cpp (the scan's peek)
    bool PlayerActivelyStealthing();
    bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now,
                    RE::ActorValue a_potionWant = RE::ActorValue::kNone,
                    LootMode a_mode = LootMode::kNormal);
    bool StripCorpse(RE::Actor* a_follower, const FollowerState& a_state,
                     RE::TESObjectREFR* a_corpse, Clock::time_point a_now,
                     bool* a_leftWaiting = nullptr);
    bool RunExcursionScan(RE::Actor* a_follower, const FollowerState& a_state,
                          Clock::time_point a_now);
    bool IsValuableMisc(RE::TESBoundObject* a_obj);
    bool IsQuestObjectInstance(RE::InventoryEntryData* a_entry);
    bool IsQuestObjectRef(RE::TESObjectREFR* a_ref);
    bool LooseSpecialItemBlocked(RE::TESObjectREFR* a_ref, RE::FormID a_formId);

    // defined in Logistics_Loot_Equipment.cpp (split out of Logistics_Loot.cpp,
    // 2500-line hard rule) -- the EquipmentContext type itself lives above,
    // WeaponRoles-adjacent, since both TUs construct/consume it by value.
    bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false);
    EquipmentContext BuildEquipmentContext(RE::Actor* a_follower);
    bool LooseEquipmentQualifies(RE::Actor* a_follower, RE::TESBoundObject* a_obj,
                                 const EquipmentContext& ctx);

    // defined in Logistics_Cast.cpp
    RE::ActorValue TargetMagicSchool(const FollowerState& a_state, int& a_castGambits);
    bool HasCastGambit(const FollowerState& a_state);
    bool IsNecromancerFollower(const FollowerState& a_state);
    bool IsCasterFollower(const FollowerState& a_state);
    std::uint8_t TopTwoSchoolMask(RE::Actor* a_follower);
    void LearnCarriedTomes(RE::Actor* a_follower);
    const char* SchoolName(RE::ActorValue a_school);
    bool ContainsNoCase(const char* a_hay, const char* a_needle);
    RE::ActorValue SchoolFromKeywords(const RE::BGSKeywordForm* a_kwf);
    std::string KeywordCsv(const RE::BGSKeywordForm* a_kwf);
    RE::ActorValue EffectBoostSchool(const RE::EffectSetting* a_mgef);

    // defined in Logistics_Economy.cpp
    void LogMageApparelDiag(RE::TESObjectARMO* a_armo, RE::ActorValue a_school);
    void UnlockCollegeTomes();
    void EquipBestOwnedGear(RE::Actor* a_follower, const FollowerState& a_state);
    void EconomyProbe(RE::Actor* a_follower, const FollowerState& a_state,
                      Clock::time_point a_now);
    // ── APMF EQUIP AUTHORITY (feat/mfo-equip-authority, 2026-09-15), defined in
    // Logistics_Economy.cpp beside EquipBestOwnedGear (full doc there). Declared
    // in THIS header, not Logistics.h, because the only non-Logistics caller is
    // Actuation.cpp (the combat weapon events), which already includes it.
    // The judged armor pick, factored out of EquipBestOwnedGear verbatim so the
    // declaration names exactly what that pass would wear.
    RE::TESBoundObject* ComputeOwnedGearPick(RE::Actor* a_follower, const FollowerState& a_state,
                                             ArmorPref& a_outPref, bool& a_outMageMode);
    // Supported AND claimed: the per-follower switch the direct equip sites read.
    bool EquipAuthorityLive(RE::FormID a_follower);
    // Rebuild the declared worn set and SEND it iff it changed (never a forced
    // re-issue, F3). a_holdRight/a_holdLeft: Actuation's ForcedHold ledger (ABI
    // v9: 0 = no hold, the hand is NOT owned and carries no entry -- never "read
    // the hand"; every road passes the ledger, Actuation::ForcedHoldFor OOC);
    // a_leftReserved: a cast holds/claims the left, so nothing left (not owned,
    // not declared); a_judgeArmor: include the owned-armor upgrade pick
    // (the OOC service road) -- the combat road passes false and declares worn
    // armor as is (F9). Worker only (#4). Claims on first use (re-validating a
    // kept handle, F2); releases when the gate is off.
    void RefreshEquipDeclaration(RE::Actor* a_follower, const FollowerState& a_state,
                                 RE::FormID a_holdRight, RE::FormID a_holdLeft,
                                 bool a_leftReserved, bool a_judgeArmor, const char* a_why);
    // The bound WEAP forms whose BoundItemEffect is live on a_follower right now
    // (sorted, deduped; worker, read-only). The declaration carries them while
    // live; Actuation's in-combat lap compares this to trigger a re-declaration.
    std::vector<RE::FormID> LiveBoundWeapons(RE::Actor* a_follower);
    void ForgetEquipDeclaration(RE::FormID a_follower);   // one follower's change detector (dismiss)
    void ResendEquipDeclaration(RE::FormID a_follower);   // drop ONLY the change detector so the next rebuild is sent (MFO-B41); player picks kept
    void ClearEquipDeclarations();                        // all (revert / load)

    // ── added by the wave-2 folder split (2026-09-25): helpers that were
    // file-local BY POSITION (defined above their only caller in the same TU)
    // and now cross a file boundary. Declarations only; no default arguments.

    // defined in logistics/Upkeep.cpp, called by ServiceFollower
    bool EquipTorch(RE::Actor* a_follower);
    void HealExcludedWeapon(RE::Actor* a_follower);
    bool ShedOffRoleWeapon(RE::Actor* a_follower, const FollowerState& a_state);
    void MarkJustLooted(RE::FormID a_id, Clock::time_point a_now);

    // defined in logistics/LootTake.cpp, called by logistics/LootScan.cpp
    bool IsDrinkablePotion(RE::AlchemyItem* a_alc);
    bool IsCoinLoot(RE::TESBoundObject* a_obj);
    bool IsSoulGemItem(RE::TESBoundObject* a_obj);
    bool IsIngredientItem(RE::TESBoundObject* a_obj);
    bool RefInPlayerStorage(RE::TESObjectREFR* a_ref);
    bool IsLOTDDropOff(RE::TESObjectREFR* a_ref);

    // defined in logistics/Lockpick.cpp (LP-M1, follower lockpicking of chests; replaces the
    // old LockPickable). Worker-only; see that file's banner.
    namespace Lockpick {
        // The scan's lock gate: may this follower attempt this locked ref? Logs each refusal
        // reason once per (follower, lock). Read-only.
        bool Admit(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Clock::time_point a_now);
        enum class StepResult {
            kProceed,   // the lock is open (picked, keyed, or already unlocked): transfer now
            kHold,      // the attempt is in progress: do nothing else this tick
            kRefused,   // refused or failed: skip this ref (the caller blocklists it transiently)
        };
        // The excursion's ARRIVAL at a locked ref. a_excursionEnd = the excursion cap's end.
        StepResult Step(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, Clock::time_point a_now,
                        Clock::time_point a_excursionEnd);
        // Abandon (release the hold, consume nothing) every job its excursion no longer drives.
        void SweepStale(Clock::time_point a_now);
        void Abort(RE::FormID a_follower, const char* a_why);
        void Clear();   // revert / load
    }
    bool TierReleased(Category a_cat, RE::TESObjectREFR* a_src,
                      const RE::NiPoint3& a_playerPos, Clock::time_point a_now);

    // defined in logistics/EquipAuthority.cpp, called by EconomyProbe
    bool IsPlayerPick(RE::FormID a_follower, RE::FormID a_form);
}
