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
#include "Actuation.h"   // cast-in-logistics: reuse the combat cast path (Fire)
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
#include "MEOBridge.h"    // MEO gem transfer on gear swap (#17) + WornUid
#include "Papyrus.h"      // route 2b acquire probe: VM-dispatched ObjectReference.Activate
#include "MainThread.h"   // the pump (§0.37): live-vendor reads MUST run on the main thread
#include "Sightline.h"    // LoS + line-of-fire gate on the OOC hostile-FF direct fallback
#include "Followers.h"    // #62 on-load beast-head sweep iterates g_active (main thread)
#include <functional>     // #62 self-reposting on-load sweep closure
#include <memory>         // std::shared_ptr for that closure
#include "TradeBridge.h"  // #21 econ bridge: MFO_Trade Papyrus round-trip (Phase 0 self-test)
#include <mutex>          // #69: g_stockMx -- g_stockGear is a real cross-thread map (worker + co-save)

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
        // OBSERVED in combat, stamped by NoteInCombat (below) from the Scheduler's
        // in-combat branch -- the ONLY place combat=true is visible, because this
        // whole service path is entered only when the follower reads out of combat
        // (Scheduler.cpp). ShedOffRoleWeapon requires kShedPostBattleDwell to have
        // elapsed since that stamp before it drops anything, so a mid-fight
        // IsInCombat() FLAP (a brief LoS loss / disengage that the Scheduler
        // services the follower straight through) can never mature the dwell: the
        // next real combat frame re-stamps `now` and resets the clock. Worker-only,
        // no lock -- both the stamp and the read run on the same BSJobs worker tick,
        // sequential across followers (#4). Save-scoped: cleared on revert.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_lastCombatSeen;

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
        // type, never by name (§4.8.2). (Armor's light/heavy steer is still
        // scoped: ArmorIsBetter compares raw rating on the slot -- the heavy/light
        // steer wants a CommonLib armor-type call not verified on this offline box.)
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

        // OPTION A travel state -- up to kMaxLootSlots travellers AT ONCE (one loot
        // quest, four alias pairs; see g_travelSlots below). Worker-tick-only, NOT
        // serialized: this just remembers the intent; the engine-side alias fill is
        // cleared by Packages on load (#55). kArrivalDist ~= arm's reach: once the
        // engine walks the follower this close, the existing inventory transfer runs.
        constexpr float kArrivalDist = 200.0f;   // was 160 -- perf pass: fewer "stalled a hair short" strandings
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
            // PACKAGE-THEFT episode start (deck 12:25:18: onTravelPkg flipped
            // false mid-walk, curPkg=FF001780 -- a runtime scene/framework
            // package took the follower while the loot alias was STILL filled).
            // While an external package holds him, the leg's stall/deadline
            // clocks must not run -- the REF is not to blame -- and the claim is
            // re-asserted (EvaluatePackage) each tick for a bounded grace.
            // Zero = currently on the travel package.
            Clock::time_point   stolenSince{};
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
        inline std::array<TravelIntent, Packages::kMaxLootSlots> g_travelSlots{};

        // The active slot whose intent belongs to a_follower (nullptr if none).
        inline TravelIntent* SlotOf(RE::FormID a_follower) {
            if (!a_follower) return nullptr;
            for (auto& t : g_travelSlots)
                if (t.active && t.follower == a_follower) return &t;
            return nullptr;
        }
        // Index of a_follower's active slot, or -1.
        inline int SlotIndexOf(RE::FormID a_follower) {
            if (!a_follower) return -1;
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (g_travelSlots[i].active && g_travelSlots[i].follower == a_follower)
                    return i;
            return -1;
        }
        // First free (inactive) slot, or -1 if all kMaxLootSlots are busy.
        inline int FreeSlotIndex() {
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (!g_travelSlots[i].active) return i;
            return -1;
        }
        // Is ANY excursion in flight? Gates the global blocklist reassess.
        inline bool AnyTravelActive() {
            for (auto& t : g_travelSlots)
                if (t.active) return true;
            return false;
        }
        constexpr float kMoveEps    = 50.0f;                  // real-movement threshold (units)
        // 5s (was 7, was 4). 4s false-positived on momentary repositioning; 7s was
        // the fix -- but most of what 7s actually absorbed turned out to be the
        // PACKAGE-THEFT case (a scene/framework package holding the follower, deck
        // 12:25:18), which the excursion driver now detects and exempts from the
        // stall clock entirely (stolenSince). With theft out of the stall path, a
        // genuine on-package zero-movement stall is a much cleaner geometric
        // verdict, so it can fail FASTER (marth RC#4: ~9s wasted per dead leg).
        constexpr auto  kNoProgress = std::chrono::seconds(5);
        // How long a stolen leg re-asserts the alias claim before giving the leg
        // up (transient blocklist ONLY, never sticky -- the ref was reachable for
        // all we know). Long enough to outlast a follower one-liner/idle scene,
        // short enough that a standing external hold (a long scripted scene)
        // releases the batch instead of parking the excursion.
        constexpr auto  kStealGrace = std::chrono::seconds(10);
        // PACKAGE-THEFT BACK-OFF. A single steal that reclaims within grace is
        // normal, but a claim that keeps getting stolen is fighting a package
        // that will not release (a persistent scene, or a combat gambit) -- so
        // re-asserting forever just churns (deck: "travel pkg stolen ...
        // re-asserting claim" logged every few seconds for both followers, the
        // leg never completing). Count displacements of the SAME follower+target
        // claim; after kStealStrikeMax, ABANDON the leg to the transient
        // blocklist (MarkTravelFailed, never sticky -- the ref was reachable)
        // and move on. Keyed (follower<<32 | target). Reset on arrival/loot
        // (provably reachable) or a target change (a new key starts fresh).
        // Worker-tick-only, erased on resolution like g_stallStrikes, never
        // serialized.
        inline std::unordered_map<std::uint64_t, int> g_stealStrikes;
        constexpr int kStealStrikeMax = 4;
        inline std::uint64_t StealKey(RE::FormID a_fol, RE::FormID a_tgt) {
            return (static_cast<std::uint64_t>(a_fol) << 32) | a_tgt;
        }

        // Set by an excursion-mode scan when it found loot it could NOT act on
        // because the player's dibs have not released yet (dNotYet > 0). The Hold
        // logic reads it: something still worth waiting for -> linger; else the
        // batch is exhausted -> return to the player. Worker-tick-only.
        inline bool g_scanSawWaiting = false;

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
        inline std::unordered_map<RE::FormID, Clock::time_point> g_travelFailed;
        constexpr auto kTravelFailCooldown = std::chrono::seconds(25);

        // GROWN GRAB (stall cure, marth): the best cure for a path-failed BODY
        // is to take its contents FROM RANGE, not to retry the walk -- the
        // transfer is RemoveItem, which never needed physical reach; arm's
        // reach is a courtesy, not an engine limit. Every path-fail against a
        // specific ref (the off-navmesh pre-gate, or a walked no-progress
        // stall) widens that ref's in-place grab radius by kGrabGrowStep, up
        // to kGrabRadiusMax -- so a body on rubble/stairs the follower stands
        // near but can never path the last gap to gets hoovered a tick later
        // instead of blocklisted (the "stands idle among lootable corpses"
        // stall). NON-LOOSE only: a loose pile is a physical Activate at the
        // item and cannot act from range. The player-bubble and leash gates
        // still apply to a grown grab, and dibs (TierReleased) always applies.
        // Counter clears on a successful grab and on revert; crude size bound
        // (these are transient per-cell verdicts, not worth a real LRU).
        inline std::unordered_map<RE::FormID, int> g_grabGrow;
        constexpr float kGrabGrowStep  = 100.0f;
        constexpr float kGrabRadiusMax = 600.0f;
        inline float GrabRadiusFor(RE::FormID a_id) {
            auto it = g_grabGrow.find(a_id);
            if (it == g_grabGrow.end()) return kArrivalDist;
            return std::min(kArrivalDist + kGrabGrowStep * static_cast<float>(it->second),
                            kGrabRadiusMax);
        }
        inline void NotePathFail(RE::FormID a_id) {
            if (!a_id) return;
            if (g_grabGrow.size() >= 256 && !g_grabGrow.contains(a_id)) g_grabGrow.clear();
            ++g_grabGrow[a_id];
        }

        // STICKY unreachable set. The transient block above is WIPED by the idle
        // reassess (so a body that becomes reachable once the follower moves gets
        // re-tried) -- but a GEOMETRICALLY unreachable target never will: an
        // off-navmesh item gives a short navmesh path that ENDS far from the item,
        // so the follower walks "there", can't close the last gap, and the wipe
        // makes him re-pick it forever (marth's frozen-Erik loop, v0.8.29: arrow
        // 00020169 navdist=18 vs dist=630, ping-ponged with 0002016A). So the
        // SECOND stall on a ref promotes it here: a cooldown the reassess does
        // NOT clear. One stall is still just transient (could be a momentary block);
        // two is a verdict. ONLY a ref the follower ACTUALLY WALKED to and could
        // not close on may strike (the Walking-phase no-progress verdict, plus the
        // loose/unacquirable direct-sticky below) -- the off-navmesh PRE-gate is a
        // heuristic that never attempted a walk, so it stays transient-only
        // (stairs/rubble false-positives were 5-min-poisoning reachable loot: the
        // "follower stands idle among lootable corpses" stall). 60 s (was 5 min):
        // still far above the ~2-4 s churn cycle this set exists to break, but a
        // stale geometric verdict now recovers within a minute. The PRIMARY stall
        // cure is the GROWN GRAB above -- a path-failed body is usually taken
        // from range before it can ever reach this set; the sticky window is only
        // the fallback for bodies beyond even kGrabRadiusMax and loose piles.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_travelUnreach;
        inline std::unordered_map<RE::FormID, int>               g_stallStrikes;
        constexpr auto kTravelStickyCooldown = std::chrono::seconds(60);

        inline bool TravelFailedRecently(RE::FormID a_id, Clock::time_point a_now) {
            auto su = g_travelUnreach.find(a_id);
            if (su != g_travelUnreach.end() && a_now < su->second + kTravelStickyCooldown)
                return true;
            auto it = g_travelFailed.find(a_id);
            return it != g_travelFailed.end() && a_now < it->second + kTravelFailCooldown;
        }
        inline void MarkTravelFailed(RE::FormID a_id, Clock::time_point a_now) {
            if (a_id) { g_travelFailed[a_id] = a_now; EvictOldest(g_travelFailed); }
        }
        // A STALL (the follower WALKED at the ref and made ZERO progress -- a real
        // attempted-and-failed leg, never a pre-dispatch heuristic) -- transient-
        // block it like any fail, but on the 2nd strike promote it to the sticky
        // set so the idle reassess can't resurrect it into a re-attempt loop.
        inline void MarkTravelStalled(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            MarkTravelFailed(a_id, a_now);
            if (++g_stallStrikes[a_id] >= 2) {
                g_stallStrikes.erase(a_id);
                g_travelUnreach[a_id] = a_now;
                EvictOldest(g_travelUnreach);
                spdlog::info("[loot] {:08X} STICKY-unreachable (2nd stall) -- won't re-pick "
                             "for {}s (survives idle reassess)", a_id,
                             std::chrono::duration_cast<std::chrono::seconds>(kTravelStickyCooldown).count());
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
        inline void MarkTravelSticky(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            g_stallStrikes.erase(a_id);
            g_travelFailed[a_id]  = a_now;  EvictOldest(g_travelFailed);
            g_travelUnreach[a_id] = a_now;  EvictOldest(g_travelUnreach);
            spdlog::info("[loot] {:08X} STICKY-unreachable (loose/unacquirable) -- won't re-pick "
                         "for {}s", a_id,
                         std::chrono::duration_cast<std::chrono::seconds>(kTravelStickyCooldown).count());
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
        inline std::unordered_map<RE::FormID, int> g_idleCycles;
        inline Clock::time_point            g_lastBlocklistReassess{};
        constexpr int  kIdleReassessCycles   = 4;                       // ~4 s idle
        constexpr auto kReassessCooldown      = std::chrono::seconds(15);

        // fBatchLinger as a duration. The default is sub-second-grained (1.5 s),
        // so it must NOT truncate through whole seconds -- convert via ms.
        inline std::chrono::milliseconds BatchLingerDur() {
            return std::chrono::milliseconds(
                static_cast<int>(Config::g_batchLinger.load() * 1000.0f));
        }

        // Travel deadline scaled to the distance: enough time to actually walk
        // there at a jog, never so long the follower is stuck if the path is
        // blocked. ~150 u/s effective + 5 s slack, clamped to [6, 20] s.
        inline Clock::time_point TravelDeadline(float a_dist, Clock::time_point a_now) {
            const float secs = std::clamp(a_dist / 150.0f + 5.0f, 6.0f, 20.0f);
            return a_now + std::chrono::seconds(static_cast<int>(secs));
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
        inline std::unordered_map<RE::FormID, Claim> g_claim;
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
    bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo);
    void KeepHeadClear(RE::Actor* a_actor);
    WeaponRoles ComputeWeaponRoles(RE::Actor* a_follower, const FollowerState& a_state);
    bool IsCreatureWeapon(const RE::TESObjectWEAP* a_w);
    bool IsCreatureArmor(const RE::TESObjectARMO* a_armo);
    RE::TESObjectARMO* WornInLogicalSlot(RE::Actor* a_follower, int a_logicalSlot);
    bool AcquireEquip(RE::Actor* a_follower, RE::TESBoundObject* a_item,
                      RE::TESObjectREFR* a_src, RE::TESObjectWEAP* a_myWeap,
                      bool a_forceStock);
    bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek = false);
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
}
