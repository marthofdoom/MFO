// logistics/ErrandRest.cpp -- TOWN ERRANDS phase 2, round R7: the INN and TEMPLE stops (ClickUp 86e3t1mx7; design
// _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md 1.g, marth's rulings 2026-10-19). This file owns the DECISIONS of those two
// stop kinds; every animation goes through Harbinger (MFO declares, Harbinger acts: ch.9 work packages via apmf/ErrandWork.cpp,
// ch.12 idles + ch.1 hold via apmf/ErrandClaims.cpp, the ch.19 walk the trip already holds).
//
//   THE TRIP (marth: ONE complete trip per town visit, ending at the inn). Augment() appends, after the R5 shop stops:
//     [ R8 SMITH STOP: INSERTION POINT, below ]  ->  TEMPLE (when owed)  ->  INN (LAST of every trip).
//   Once a trip ends no new errand is planned for that follower until the player leaves the town (TripDone / MarkTripDone).
//   WORLD STATE IS PRIMARY (marth 2026-10-19, no co-save): what a follower still needs is derived from the world on every plan --
//     inventory (arrows / potions / food / sellables), his active effects (a blessing = an effect whose MGEF carries MagicBlessing,
//     a disease), so a fully stocked, blessed follower plans no SHOP or TEMPLE stop after a reload, only the INN-ONLY trip
//     (marth 2026-10-19: he walks to the inn and stays; this overturns "the inn is never a trip alone"). The session-only TripDone mark
//     ends that trip's re-plan for the visit; it is a SECONDARY guard for needs the visit could not satisfy (a vendor with no gold left).
//
//   INN. Found through the town's load doors (the interior CELL whose location IsInn, LocationTypes) and the vendor FACTION whose
//     place sits in that location (the innkeeper, a normal shop stop: talk / give / trade / take beats, the order a small FOOD
//     stock built here). Then the stay: Eat (the meal), then Sandbox (Sleep at night when a bed he may use is free) through the
//     ch.9 work packages. HE STAYS until the player leaves the town (the phase-1 path: Errand::Sweep ends the errand and his
//     follow package walks him back, or Sim::Sweep recalls him) or combat starts (phase 1's Tick / Sweep). He does NOT rejoin when
//     the player enters the inn (marth 2026-10-19). Off screen (REST_SIM): no return leg until the player leaves town.
//   TEMPLE. Found the same way (LocTypeTemple). At the shrine ref (the ACTI bases of the nine vanilla shrines, or any activator
//     named "Shrine ..."): kneel idle (ch.12 + ch.1), then the ch.9 Activate package targeting the shrine, so the shrine's own
//     script runs with HIM as the activator (R0-6: Requiem's DispelAllSpells, then the TempleBlessing cast, which cures disease).
//     Pray BEFORE buffing: the temple stop precedes the inn and MFO buffs after the errand. Off screen (TEMPLE_SIM): the visit takes
//     iTempleGameMin and the blessing is OWED, paid only by a physical visit (a shrine ref is not persistent).
//
// THREADING. Planner / Begin / Step / End: the AddTask job worker (the service tick, the pump's sweep), the follower loaded. Engine
// reads that are not worker-safe (the active-effect list) run in a closure Posted to the true main thread and land in a leaf-mutex
// mirror (MagicView). g_mx is a LEAF: never held across an engine, Harbinger, Papyrus or TradeBridge call, never nested with the
// Errand or Sim g_mx. NOTHING HERE IS SAVED (the ERND record carries the stop kinds; the ledgers are session-only).
#include "PCH.h"
#include "Errand_internal.h"
#include "Config.h"
#include "Forms.h"
#include "LocationTypes.h"
#include "MainThread.h"
#include "Pricing.h"
#include "Runtime.h"
#include "TradeBridge.h"
#include "apmf/APMFBridge.h"
#include "APMF_API.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics {

    // The worth of a FOOD item: judged like a potion (R0-10) from its real MGEF effects -- the same archetype split as
    // PotionRestores / PotionMagnitude. Any detrimental / hostile effect = 0 (not stocked). A value-modifier on a resource (Health,
    // Magicka, Stamina) is a restore: magnitude x max(duration, 1) seconds, capped at an hour so a one-hour regen does not drown a heal; any
    // other BENEFICIAL value / peak-value modifier counts magnitude x sqrt(max(duration, 1)). Survival-mode hunger effects are player-only
    // and carry no beneficial value-modifier on the follower, so they score nothing. Pure form reads: safe on the Papyrus VM thread (PlanBuy).
    float FoodWorth(const RE::AlchemyItem* a_food) {
        if (!a_food || !a_food->IsFood() || a_food->IsPoison()) return 0.0f;
        float worth = 0.0f;
        for (const auto* e : a_food->effects) {
            const auto* mgef = e ? e->baseEffect : nullptr;
            if (!mgef) continue;
            if (mgef->IsDetrimental() || mgef->IsHostile()) return 0.0f;
            const auto arch = mgef->data.archetype;
            if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier && arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier &&
                arch != RE::EffectArchetypes::ArchetypeID::kPeakValueModifier) continue;
            const float mag = std::max(0.0f, e->GetMagnitude());
            const float dur = static_cast<float>(std::max<std::uint32_t>(e->GetDuration(), 1u));
            const auto av = mgef->data.primaryAV;
            const bool resource = av == RE::ActorValue::kHealth || av == RE::ActorValue::kMagicka || av == RE::ActorValue::kStamina;
            if (resource && arch != RE::EffectArchetypes::ArchetypeID::kPeakValueModifier && !mgef->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kRecover))
                worth += mag * std::min(dur, 3600.0f);
            else
                worth += mag * std::sqrt(dur);
        }
        return worth;
    }
}

namespace MFO::Logistics::Errand::Rest {

    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr RE::FormID kIdleKneelEnter = 0x000F11EE;   // IdleBlessingKneelEnter (verified in Skyrim.esm with tools/esp_inspect.py, 2026-10-19)
        constexpr RE::FormID kIdleKneelExit  = 0x000F11EF;   // IdleBlessingKneelExit
        // The vanilla temple blessing's MGEFs (FortifyHealthFFSelf 000FB98D, FortifyMagickaRateFFSelf 000FB989, FortifySpeechcraftFFSelf
        // 000FB993, FortifySneakFFSelf 0010E8AF, ...) all carry KYWD MagicBlessing; the LoreRim winner (Wintersun - Reqtificated.esp)
        // overrides 000FB98D and keeps the keyword. Verified in Skyrim.esm and Wintersun with tools/esp_inspect.py, 2026-10-19.
        constexpr RE::FormID kBlessingKeyword = 0x000FB98C;   // MagicBlessing
        constexpr int kMagicMaxDefers = 5;   // MagicReadPending: ~5 plan cadences (6 s) = ~30 s, then WARN and plan without the temple decision
        constexpr float  kScan          = 4096.0f;   // the door / shrine / bed scan around the follower (one cell)
        constexpr float  kShrineRadius  = 110.0f;    // ch.19 arrival radius at the shrine (APMF clamps [50,512])
        constexpr float  kInnSandboxRad = 700.0f;    // the stay: around the anchor (the inn's entry door), i.e. the main room
        constexpr float  kInnEatRad     = 500.0f;
        constexpr float  kInnSleepRad   = 500.0f;
        constexpr float  kShrineNear    = 180.0f;    // distance backstop for the shrine walk
        constexpr auto   kMealPatience  = std::chrono::seconds(45);
        constexpr auto   kSleepPatience = std::chrono::seconds(30);
        constexpr auto   kKneelPatience = std::chrono::seconds(8);
        constexpr auto   kKneelHold     = std::chrono::milliseconds(2500);
        constexpr auto   kShrineWalk    = std::chrono::seconds(25);
        constexpr auto   kShrineSeek    = std::chrono::seconds(8);
        constexpr auto   kActivateWait  = std::chrono::seconds(15);
        constexpr auto   kExitHold      = std::chrono::seconds(2);
        constexpr auto   kProbeEvery    = std::chrono::seconds(3);
        constexpr int    kMaxReoffers   = 6;
        constexpr float  kNightStart = 22.0f, kNightEnd = 6.0f;

        // The nine vanilla shrine ACTI bases (design 1.g), Skyrim.esm local ids. Other mods' shrines are accepted by name ("Shrine of ...").
        constexpr std::array<RE::FormID, 9> kShrineBases = {
            0x00071854, 0x000D9883, 0x000D9881, 0x000D987F, 0x000D9885, 0x000D9887, 0x000D987D, 0x000D987B, 0x00100780 };

        // The active-effect mirror (filled on the TRUE main thread, read on the worker): disease and the effect count -- the temple's
        // reason, and the P-TEMPLE-1 before / after lines.
        struct MagicView {
            bool valid = false;
            bool diseased = false;
            bool blessed = false;       // an active effect whose base MGEF carries the MagicBlessing keyword (kBlessingKeyword)
            int  effects = 0;
            std::string diseases;       // the disease spells' names, for the log
            std::string blessings;      // the blessing effects' names, for the log
            Clock::time_point stamp{};
        };

        enum class Stage { Meal, Stay, Seek, Walk, Kneel, Activate, Exit };
        struct Work {
            StopKind     kind = StopKind::Inn;
            Stage        stage = Stage::Meal;
            std::string  name, followerName;
            RE::FormID   town = 0;
            RE::FormID   vendor = 0;       // the innkeeper (a fallback anchor)
            RE::FormID   anchor = 0;       // inn: the static ref the work packages are placed around
            RE::FormID   shrine = 0;       // temple
            RE::FormID   bed = 0;
            Clock::time_point began{}, stageAt{}, lastProbe{}, confirmedAt{};
            int          foodAtMeal = 0, mealsEaten = 0, reoffers = 0;
            bool         eatFailedLogged = false, bedChecked = false, sleepFailedTonight = false, kneelPlayed = false;
            bool         offeredValid = false;
            APMFBridge::ErrandWorkKind offered = APMFBridge::ErrandWorkKind::Sandbox;
            MagicView    before;
            bool         changed = false;
            int          lastState = -1;
        };

        std::mutex                                       g_mx;    // a LEAF (see the file header)
        std::unordered_map<RE::FormID, Work>             g_work;
        std::unordered_map<RE::FormID, MagicView>        g_magic;
        std::unordered_map<RE::FormID, int>              g_templeTry;  // follower -> game day of the last temple visit (any outcome): a short-term ANTI-THRASH only, NOT the blessing's truth (that is MagicView::blessed)
        std::unordered_set<RE::FormID>                   g_tripDone;   // followers whose trip ended this town visit
        std::unordered_set<RE::FormID>                   g_tripLogged; // ... for whom the re-plan block was already logged
        std::unordered_map<RE::FormID, int>              g_magicDefers;  // consecutive MagicReadPending defers per follower (cap kMagicMaxDefers)
        std::unordered_set<RE::FormID>                   g_lockLogged;  // load doors whose LOCK was already logged (one skip line per door per town visit)
        std::unordered_set<RE::FormID>                   g_magicWaitLogged; // ... for whom "waiting for the blessing/disease read" was logged this visit
        RE::FormID                                       g_marksTown = 0;
        struct InnFac { RE::FormID faction = 0, chest = 0, locId = 0, cell = 0; };   // cell: the interior cell of the faction's PLVD place (0 = unknown)
        std::unordered_map<RE::FormID, std::vector<InnFac>> g_innFacs;  // town -> the innkeeper factions (immutable once built)

        int GameDay() {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? static_cast<int>(std::floor(cal->GetDaysPassed())) : 0;
        }
        bool IsNight() {
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal) return false;
            const float h = cal->GetHour();
            return h >= kNightStart || h < kNightEnd;
        }
        const char* StateName(RE::SIT_SLEEP_STATE s) {
            switch (s) {
                case RE::SIT_SLEEP_STATE::kNormal: return "Normal";
                case RE::SIT_SLEEP_STATE::kWantToSit: return "WantToSit";
                case RE::SIT_SLEEP_STATE::kWaitingForSitAnim: return "WaitingForSitAnim";
                case RE::SIT_SLEEP_STATE::kIsSitting: return "Sitting";
                case RE::SIT_SLEEP_STATE::kWantToStand: return "WantToStand";
                case RE::SIT_SLEEP_STATE::kWantToSleep: return "WantToSleep";
                case RE::SIT_SLEEP_STATE::kWaitingForSleepAnim: return "WaitingForSleepAnim";
                case RE::SIT_SLEEP_STATE::kIsSleeping: return "Sleeping";
                case RE::SIT_SLEEP_STATE::kWantToWake: return "WantToWake";
            }
            return "?";
        }
        bool InSleepStates(RE::SIT_SLEEP_STATE s) {
            return s == RE::SIT_SLEEP_STATE::kWantToSleep || s == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim || s == RE::SIT_SLEEP_STATE::kIsSleeping;
        }

        // ── engine reads ─────────────────────────────────────────────────────────────────────────────────
        // Main-thread closure: the follower's active effects -> the mirror. Never reads on the worker (the list mutates under the
        // magic caster on the main thread; cast/Summon.cpp's rule).
        void PostMagicProbe(RE::FormID a_fid) {
            auto fn = [a_fid]() {
                MagicView v;
                v.stamp = Clock::now();
                // valid only for a LOADED follower: an unloaded one has no readable list, which must not read as "no blessing"
                if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid); f && f->Is3DLoaded()) {
                    v.valid = true;
                    if (auto* mt = f->AsMagicTarget()) {
                        if (auto* list = mt->GetActiveEffectList()) {
                            for (auto* ae : *list) {
                                if (!ae) continue;
                                ++v.effects;
                                if (!ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
                                    if (const auto* mgef = ae->GetBaseObject(); mgef && mgef->HasKeywordID(kBlessingKeyword)) {
                                        v.blessed = true;
                                        if (const char* n = mgef->GetName(); n && *n) {
                                            if (!v.blessings.empty()) v.blessings += ", ";
                                            v.blessings += n;
                                        }
                                    }
                                }
                                if (ae->spell && ae->spell->GetSpellType() == RE::MagicSystem::SpellType::kDisease) {
                                    v.diseased = true;
                                    if (const char* n = ae->spell->GetName(); n && *n) {
                                        if (!v.diseases.empty()) v.diseases += ", ";
                                        v.diseases += n;
                                    }
                                }
                            }
                        }
                    }
                }
                std::scoped_lock lk(g_mx);
                g_magic[a_fid] = std::move(v);
            };
            if (MainThread::IsInstalled()) MainThread::Post(fn);
            else                           fn();   // VR: the pump is a no-op, keep the direct path
        }
        MagicView MagicOf(RE::FormID a_fid) {
            std::scoped_lock lk(g_mx);
            const auto it = g_magic.find(a_fid);
            return it != g_magic.end() ? it->second : MagicView{};
        }

        int GoldOf(RE::Actor* a_actor) {
            std::int64_t n = 0;
            for (auto& [obj, data] : a_actor->GetInventory())
                if (obj && data.first > 0 && obj->GetFormID() == 0x0000000F) n += data.first;
            return static_cast<int>(std::min<std::int64_t>(n, 2000000000));
        }
        // a_worthy: only foods the judge would STOCK (the inn's stock target); else every food (the meal detection).
        int CountFood(RE::Actor* a_actor, bool a_worthy) {
            int n = 0;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (alc && alc->IsFood() && (!a_worthy || FoodWorth(alc) > 0.0f)) n += data.first;
            }
            return n;
        }

        // ── places: the inn / temple INTERIOR CELL, found through the town's load doors ─────────────────────
        struct Place {
            bool ok = false;
            RE::FormID cell = 0, locId = 0;
            RE::NiPoint3 door{};
            std::string name;
            bool lockedSkip = false;   // a LOCKED load door to this kind of place was skipped (the data fallback must not resolve around the lock)
        };
        const char* LockWord(RE::LOCK_LEVEL a_l) {
            switch (a_l) {
                case RE::LOCK_LEVEL::kUnlocked:    return "unlocked";
                case RE::LOCK_LEVEL::kVeryEasy:    return "very easy";
                case RE::LOCK_LEVEL::kEasy:        return "easy";
                case RE::LOCK_LEVEL::kAverage:     return "average";
                case RE::LOCK_LEVEL::kHard:        return "hard";
                case RE::LOCK_LEVEL::kVeryHard:    return "very hard";
                case RE::LOCK_LEVEL::kRequiresKey: return "requires a key";
                default:                           return "?";
            }
        }
        // A load door is LOCKED when either side of the pair says so (REFR_LOCK via TESObjectREFR::GetLockLevel, the same read Lockpick.cpp
        // uses). Fills the locked door's id and level. Generic: no per-town or per-door knowledge (a temple closed by a quest event reads
        // locked the same way).
        bool DoorLocked(RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked, RE::FormID& a_id, RE::LOCK_LEVEL& a_lvl) {
            for (auto* d : { a_door, a_linked }) {
                if (!d) continue;
                const auto l = d->GetLockLevel();
                if (l != RE::LOCK_LEVEL::kUnlocked) { a_id = d->GetFormID(); a_lvl = l; return true; }
            }
            return false;
        }
        void FindPlaces(RE::Actor* a_follower, const RE::BGSLocation* a_town, Place& a_inn, Place& a_temple) {
            auto* cell = a_follower ? a_follower->GetParentCell() : nullptr;
            if (!cell || !cell->IsAttached() || !a_town) return;
            std::vector<RE::ObjectRefHandle> doors;
            cell->ForEachReferenceInRange(a_follower->GetPosition(), kScan, [&](RE::TESObjectREFR* a_ref) {
                if (doors.size() < 96 && a_ref->extraList.GetByType<RE::ExtraTeleport>()) doors.push_back(a_ref->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            float innD = 1.0e9f, templeD = 1.0e9f;
            for (auto& h : doors) {
                auto dp = h.get();
                auto* d = dp.get();
                auto* tele = d ? d->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
                if (!tele || !tele->teleportData) continue;
                auto lp = tele->teleportData->linkedDoor.get();
                auto* lc = lp.get() ? lp.get()->GetParentCell() : nullptr;
                if (!lc || !lc->IsInteriorCell()) continue;
                const RE::BGSLocation* loc = lc->GetLocation();
                if (!loc || LocationTypes::TownAnchor(loc) != a_town) continue;
                const float dist = d->GetPosition().GetDistance(a_follower->GetPosition());
                auto take = [&](Place& a_p, float& a_best, const char* a_fallback) {
                    if (dist >= a_best) return;
                    RE::FormID lockedId = 0;
                    RE::LOCK_LEVEL lvl = RE::LOCK_LEVEL::kUnlocked;
                    if (DoorLocked(d, lp.get(), lockedId, lvl)) {
                        a_p.lockedSkip = true;
                        bool first = false;
                        { std::scoped_lock lk(g_mx); first = g_lockLogged.insert(lockedId).second; }
                        if (first) {
                            const char* n = loc->GetName();
                            spdlog::info("[errand] {:08X}: {} '{}' SKIPPED at planning -- its load door {:08X} is LOCKED (lock level {}: {}); a closed place is never walked to",
                                         a_follower->GetFormID(), a_fallback, n && *n ? n : "?", lockedId, static_cast<int>(lvl), LockWord(lvl));
                        }
                        return;
                    }
                    a_best = dist;
                    a_p.ok = true; a_p.cell = lc->GetFormID(); a_p.locId = loc->GetFormID(); a_p.door = d->GetPosition();
                    const char* n = loc->GetName();
                    a_p.name = n && *n ? n : a_fallback;
                };
                if (LocationTypes::IsInn(loc))         take(a_inn, innD, "the inn");
                else if (LocationTypes::IsTemple(loc)) take(a_temple, templeD, "the temple");
            }
        }

        // The innkeeper vendor factions of a town: the faction's PLVD place sits in an inn location of the town. Read the way 1.6.1170's
        // loader leaves vendorLocation (Errand.cpp BuildTownShops; disassembly-verified, that build ONLY -- per-runtime rule): elsewhere the
        // list is empty and the inn stop is rest-only. Cached per town (immutable).
        std::vector<InnFac> InnFactionsOf(const RE::BGSLocation* a_town) {
            if (!a_town || !Runtime::IsVerified1_6_1170()) return {};
            {
                std::scoped_lock lk(g_mx);
                if (const auto it = g_innFacs.find(a_town->GetFormID()); it != g_innFacs.end()) return it->second;
            }
            std::vector<InnFac> out;
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
                    if (!fac || !fac->IsVendor() || !fac->OffersServices()) continue;
                    auto* chest = fac->vendorData.merchantContainer;
                    if (!chest || !chest->GetContainer()) continue;
                    if (fac->vendorData.vendorConditions && fac->vendorData.vendorConditions->head) continue;
                    const auto& vv = fac->vendorData.vendorValues;
                    if (vv.buysStolen && !vv.buysNonStolen) continue;
                    auto* pl = fac->vendorData.vendorLocation;
                    if (!pl) continue;
                    const RE::BGSLocation* placeLoc = nullptr;
                    RE::FormID placeCell = 0;
                    if (pl->locType.get() == RE::PackageLocation::Type::kInCell) {
                        auto* c = pl->data.object ? pl->data.object->As<RE::TESObjectCELL>() : nullptr;
                        placeLoc = c ? c->GetLocation() : nullptr;
                        if (c && c->IsInteriorCell()) placeCell = c->GetFormID();
                    } else if (pl->locType.get() == RE::PackageLocation::Type::kNearReference) {
                        auto ref = pl->data.refHandle.get();
                        if (ref) {
                            placeLoc = ref->GetEditorLocation(); if (!placeLoc) placeLoc = ref->GetCurrentLocation();
                            if (auto* rc = ref->GetParentCell(); rc && rc->IsInteriorCell()) placeCell = rc->GetFormID();
                        }
                    }
                    if (!placeLoc || !LocationTypes::IsInn(placeLoc) || LocationTypes::TownAnchor(placeLoc) != a_town) continue;
                    // the innermost inn location of the place (the one the interior cell reports)
                    const RE::BGSLocation* inner = placeLoc;
                    InnFac f;
                    f.faction = fac->GetFormID(); f.chest = chest->GetFormID(); f.locId = inner->GetFormID(); f.cell = placeCell;
                    out.push_back(f);
                }
            }
            spdlog::info("[errand] inn catalog for '{}': {} innkeeper vendor faction(s)", a_town->GetName() ? a_town->GetName() : "?", out.size());
            std::scoped_lock lk(g_mx);
            g_innFacs[a_town->GetFormID()] = out;
            return out;
        }

        // R7 review SEV-3: the inn / temple are NOT only reachable through a load door in the follower's own cell (in an exterior town the
        // door is often in another cell). Resolve the place from the DATA, the way BuildTownShops does and on the same gate (1.6.1170 only):
        //   (a) the innkeeper vendor faction's PLVD place -> its interior cell (InnFactionsOf, fills InnFac::cell);
        //   (b) the location's own special refs (LCSR): UnloadedRefData::parentSpaceID is the cell the ref sits in; the first INTERIOR cell whose
        //       location IsInn / IsTemple and sits in this town.
        // The cell is walked to by ch.19 exactly like a shop CELL; no door position is known (Stop::havePos false -> the planner's 2000 u street
        // estimate). UNVERIFIED IN THE FIELD: whether a given location lists a special ref inside its interior.
        bool ResolveViaData(const RE::BGSLocation* a_town, bool a_inn, Place& a_out, const char*& a_how) {
            if (a_out.ok || !a_town || !Runtime::IsVerified1_6_1170()) return a_out.ok;
            if (a_inn) {
                for (const auto& f : InnFactionsOf(a_town)) {
                    auto* c = f.cell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(f.cell) : nullptr;
                    const RE::BGSLocation* loc = c ? c->GetLocation() : nullptr;
                    if (!loc) continue;
                    a_out.ok = true; a_out.cell = c->GetFormID(); a_out.locId = loc->GetFormID();
                    const char* n = loc->GetName();
                    a_out.name = n && *n ? n : "the inn";
                    a_how = "the innkeeper faction's place";
                    return true;
                }
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return false;
            for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
                if (!loc || !(a_inn ? LocationTypes::IsInn(loc) : LocationTypes::IsTemple(loc)) || LocationTypes::TownAnchor(loc) != a_town) continue;
                for (const auto& sr : loc->specialRefs) {
                    auto* c = RE::TESForm::LookupByID<RE::TESObjectCELL>(sr.refData.parentSpaceID);
                    if (!c || !c->IsInteriorCell()) continue;
                    const RE::BGSLocation* cl = c->GetLocation();
                    if (!cl || !(a_inn ? LocationTypes::IsInn(cl) : LocationTypes::IsTemple(cl)) || LocationTypes::TownAnchor(cl) != a_town) continue;
                    a_out.ok = true; a_out.cell = c->GetFormID(); a_out.locId = cl->GetFormID();
                    const char* n = cl->GetName();
                    a_out.name = n && *n ? n : (a_inn ? "the inn" : "the temple");
                    a_how = "the location's special refs";
                    return true;
                }
            }
            return false;
        }

        // ── the temple's reason ──────────────────────────────────────────────────────────────────────────
        // Q4: cure when diseased, bless when he has no active blessing. The blessing is read from the WORLD STATE (marth 2026-10-19, no
        // co-save): an active effect whose MGEF carries MagicBlessing (MagicView::blessed, filled on the main thread). An unread mirror
        // (not posted yet, or he is not loaded) wants NOTHING: the next plan (6 s) reads it. g_templeTry stays only as a short-term
        // anti-thrash (at most one temple visit per game day, session-only): a blessing that does not take (a shrine that ignores him)
        // must not loop him back to the temple every plan.
        bool TempleWanted(RE::FormID a_fid, std::string& a_why) {
            const int today = GameDay();
            const auto m = MagicOf(a_fid);
            std::scoped_lock lk(g_mx);
            if (const auto it = g_templeTry.find(a_fid); it != g_templeTry.end() && it->second == today) return false;   // visited today
            if (!m.valid) return false;
            if (m.diseased) { a_why = "diseased (" + m.diseases + ")"; return true; }
            if (!m.blessed) { a_why = "no active blessing"; return true; }
            return false;
        }

        // ── the inn's food order ─────────────────────────────────────────────────────────────────────────
        std::shared_ptr<const ErrandPlanned> BuildFoodOrder(RE::Actor* a_follower, const InnFac& a_fac, int a_need, int a_purse) {
            auto pl = std::make_shared<ErrandPlanned>();
            pl->vendorKey = a_fac.faction; pl->faction = a_fac.faction; pl->chest = a_fac.chest;
            pl->needs.push_back({ static_cast<std::int32_t>(TradeBridge::NeedCat::kFood), static_cast<std::int32_t>(a_need) });
            pl->needKinds = 1u << TradeBridge::NeedCat::kFood;
            pl->buy.wantFood = true;
            pl->purse = a_purse;
            pl->chestGold = TradeBridge::LastSeenChestGold(a_fac.chest);
            pl->sig = TradeBridge::TradeSignature(a_purse, pl->needs, pl->sell);
            if (auto* cal = RE::Calendar::GetSingleton()) pl->daysNow = cal->GetDaysPassed();
            pl->quote = Pricing::Resolve(a_follower->GetHandle(), RE::ActorHandle{}, a_fac.faction);
            return pl;
        }

        // ── work helpers ─────────────────────────────────────────────────────────────────────────────────
        bool IsShrine(RE::TESObjectREFR* a_ref) {
            auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
            if (!base || base->GetFormType() != RE::FormType::Activator) return false;
            const RE::FormID local = base->GetFormID() & 0x00FFFFFFu;
            if (std::find(kShrineBases.begin(), kShrineBases.end(), local) != kShrineBases.end() && (base->GetFormID() >> 24) == 0) return true;
            const char* n = base->GetName();
            if (!n || !*n) return false;
            std::string low(n);
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return low.rfind("shrine of ", 0) == 0 || low.rfind("shrine to ", 0) == 0;
        }
        RE::FormID FindShrine(RE::Actor* a_follower, RE::FormID& a_base) {
            a_base = 0;
            auto* cell = a_follower ? a_follower->GetParentCell() : nullptr;
            if (!cell || !cell->IsAttached()) return 0;
            std::vector<RE::ObjectRefHandle> cands;
            cell->ForEachReferenceInRange(a_follower->GetPosition(), kScan, [&](RE::TESObjectREFR* a_ref) {
                if (cands.size() < 32 && !a_ref->IsDisabled() && !a_ref->IsMarkedForDeletion() && IsShrine(a_ref)) cands.push_back(a_ref->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            RE::FormID best = 0;
            float bestD = 1.0e9f;
            for (auto& h : cands) {
                auto rp = h.get();
                auto* r = rp.get();
                if (!r) continue;
                const float d = r->GetPosition().GetDistance(a_follower->GetPosition());
                if (d < bestD) { bestD = d; best = r->GetFormID(); a_base = r->GetBaseObject() ? r->GetBaseObject()->GetFormID() : 0u; }
            }
            return best;
        }
        // The inn's static anchor: the nearest load door in his (interior) cell, else the innkeeper, else himself.
        RE::FormID FindAnchor(RE::Actor* a_follower, RE::FormID a_vendor, const char*& a_what) {
            auto* cell = a_follower->GetParentCell();
            if (cell && cell->IsAttached()) {
                std::vector<RE::ObjectRefHandle> doors;
                cell->ForEachReferenceInRange(a_follower->GetPosition(), kScan, [&](RE::TESObjectREFR* a_ref) {
                    if (doors.size() < 24 && a_ref->extraList.GetByType<RE::ExtraTeleport>()) doors.push_back(a_ref->GetHandle());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                RE::FormID best = 0;
                float bestD = 1.0e9f;
                for (auto& h : doors) {
                    auto dp = h.get();
                    auto* d = dp.get();
                    if (!d) continue;
                    const float dist = d->GetPosition().GetDistance(a_follower->GetPosition());
                    if (dist < bestD) { bestD = dist; best = d->GetFormID(); }
                }
                if (best) { a_what = "the entry door"; return best; }
            }
            if (a_vendor) { a_what = "the innkeeper"; return a_vendor; }
            a_what = "himself";
            return a_follower->GetFormID();
        }
        // A bed he may use: a furniture ref that can be slept in, unowned or the player's (P-INN-2: teammates use player-owned furniture).
        RE::FormID FindBed(RE::Actor* a_follower) {
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached()) return 0;
            std::vector<RE::ObjectRefHandle> cands;
            cell->ForEachReferenceInRange(a_follower->GetPosition(), kScan, [&](RE::TESObjectREFR* a_ref) {
                auto* base = a_ref->GetBaseObject();
                auto* fur = base ? base->As<RE::TESFurniture>() : nullptr;
                if (cands.size() < 24 && fur && fur->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep) && !a_ref->IsDisabled())
                    cands.push_back(a_ref->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            RE::FormID best = 0;
            float bestD = 1.0e9f;
            for (auto& h : cands) {
                auto rp = h.get();
                auto* r = rp.get();
                if (!r) continue;
                auto* owner = r->GetOwner();
                if (owner && owner->GetFormID() != 0x00000007u && owner->GetFormID() != 0x00000014u) continue;   // someone else's bed
                const float d = r->GetPosition().GetDistance(a_follower->GetPosition());
                if (d < bestD) { bestD = d; best = r->GetFormID(); }
            }
            return best;
        }
        RE::TESObjectREFR* RefOf(RE::FormID a_id) { return a_id ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id) : nullptr; }

        bool Offer(RE::Actor* a_follower, Work& w, APMFBridge::ErrandWorkKind a_kind, RE::TESObjectREFR* a_ref, float a_radius, Clock::time_point a_now,
                   std::string* a_whyOut = nullptr) {
            std::string why;
            if (!APMFBridge::OfferErrandWork(a_follower->GetFormID(), a_kind, a_ref, a_radius, why)) {
                spdlog::warn("[errand] {:08X} '{}': the {} work could not be offered at '{}': {}", a_follower->GetFormID(), w.followerName,
                             APMFBridge::ErrandWorkName(a_kind), w.name, why);
                if (a_whyOut) *a_whyOut = why;
                return false;
            }
            w.offered = a_kind; w.offeredValid = true; w.stageAt = a_now;
            return true;
        }

        void Put(RE::FormID a_fid, const Work& w) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_work.find(a_fid); it != g_work.end()) it->second = w;
        }

        // ── the INN's stay ───────────────────────────────────────────────────────────────────────────────
        StepOut StepInn(RE::Actor* f, Work& w, Clock::time_point a_now) {
            StepOut out;
            const auto fid = f->GetFormID();
            auto* anchor = RefOf(w.anchor);
            if (!anchor || anchor->IsDisabled()) anchor = f;   // the anchor left memory: around himself (logged on change below)
            const auto state = f->AsActorState()->GetSitSleepState();
            if (static_cast<int>(state) != w.lastState) {
                spdlog::info("[errand] {:08X} '{}': sit/sleep state {} -> {} (stay at '{}', work {})", fid, w.followerName,
                             w.lastState < 0 ? "?" : StateName(static_cast<RE::SIT_SLEEP_STATE>(w.lastState)), StateName(state), w.name,
                             w.offeredValid ? APMFBridge::ErrandWorkName(w.offered) : "none");
                w.lastState = static_cast<int>(state);
            }
            switch (w.stage) {
            case Stage::Meal: {
                if (!w.offeredValid || w.offered != APMFBridge::ErrandWorkKind::Eat) {
                    w.foodAtMeal = CountFood(f, false);
                    if (!Offer(f, w, APMFBridge::ErrandWorkKind::Eat, anchor, kInnEatRad, a_now)) { w.stage = Stage::Stay; w.offeredValid = false; break; }
                    spdlog::info("[errand] {:08X} '{}': MEAL at '{}' -- the Eat package is offered (food in inventory {})", fid, w.followerName, w.name, w.foodAtMeal);
                    break;
                }
                const int food = CountFood(f, false);
                if (food < w.foodAtMeal) {
                    ++w.mealsEaten;
                    spdlog::info("[errand] {:08X} '{}': ATE at '{}' (food {} -> {}, meal {}/{}; sit/sleep state {})", fid, w.followerName, w.name,
                                 w.foodAtMeal, food, w.mealsEaten, Config::g_innMeals.load(), StateName(state));
                    w.foodAtMeal = food;
                    w.stageAt = a_now;
                    if (w.mealsEaten >= Config::g_innMeals.load() || food <= 0) { w.stage = Stage::Stay; w.offeredValid = false; }
                } else if (a_now - w.stageAt > kMealPatience) {
                    spdlog::warn("[errand] {:08X} '{}': the Eat package did not eat -- food still in inventory ({}) after {} s; the food is KEPT "
                                 "(sit/sleep state {})", fid, w.followerName, food, std::chrono::duration_cast<std::chrono::seconds>(a_now - w.stageAt).count(),
                                 StateName(state));
                    w.stage = Stage::Stay;
                    w.offeredValid = false;
                }
                break;
            }
            case Stage::Stay: {
                const bool night = IsNight();
                if (!night) w.sleepFailedTonight = false;
                bool sleepWanted = false;
                if (night && !w.sleepFailedTonight) {
                    if (!w.bedChecked) {
                        w.bedChecked = true;
                        w.bed = FindBed(f);
                        if (!w.bed) {
                            w.sleepFailedTonight = true;
                            spdlog::info("[errand] {:08X} '{}': night at '{}' -- no bed he may use is free here (unowned / the player's); Sandbox only", fid, w.followerName, w.name);
                        }
                    }
                    sleepWanted = w.bed != 0 && !w.sleepFailedTonight;
                } else if (!night) {
                    w.bedChecked = false;
                    w.bed = 0;
                }
                const auto want = sleepWanted ? APMFBridge::ErrandWorkKind::Sleep : APMFBridge::ErrandWorkKind::Sandbox;
                const bool held = APMFBridge::ErrandWorkHeld(fid);
                if (!w.offeredValid || w.offered != want || !held) {
                    if (w.offeredValid && w.offered == want && !held) {   // the claim was lost: re-offered, bounded, never silent
                        if (++w.reoffers > kMaxReoffers) { out.res = Result::Failed; out.why = "the stay's work claim keeps being lost (Harbinger / another mod)"; return out; }
                        spdlog::warn("[errand] {:08X} '{}': the stay's ch.9 claim is no longer held -- offering {} again ({}/{})", fid, w.followerName,
                                     APMFBridge::ErrandWorkName(want), w.reoffers, kMaxReoffers);
                    }
                    RE::TESObjectREFR* ref = want == APMFBridge::ErrandWorkKind::Sleep ? RefOf(w.bed) : anchor;
                    std::string why;
                    if (!ref || !Offer(f, w, want, ref, want == APMFBridge::ErrandWorkKind::Sleep ? kInnSleepRad : kInnSandboxRad, a_now, &why)) {
                        if (want == APMFBridge::ErrandWorkKind::Sleep) { w.sleepFailedTonight = true; w.offeredValid = false; break; }
                        out.res = Result::Failed; out.why = "the stay's Sandbox work could not be offered" + (why.empty() ? std::string{} : " (" + why + ")");
                        return out;
                    }
                    spdlog::info("[errand] {:08X} '{}': STAY at '{}' -- {} offered (ch.9); he stays until the player leaves town or combat starts", fid, w.followerName,
                                 w.name, APMFBridge::ErrandWorkName(want));
                } else if (w.offered == APMFBridge::ErrandWorkKind::Sleep && !InSleepStates(state) && a_now - w.stageAt > kSleepPatience) {
                    w.sleepFailedTonight = true;   // the bed did not take him (occupied / owned / unreachable): Sandbox for the rest of the night
                    w.offeredValid = false;
                    spdlog::info("[errand] {:08X} '{}': the Sleep work did not put him to bed in {} s (sit/sleep state {}) -- Sandbox for the rest of the night",
                                 fid, w.followerName, kSleepPatience.count(), StateName(state));
                }
                break;
            }
            default: break;
            }
            return out;
        }

        // ── the TEMPLE's prayer ──────────────────────────────────────────────────────────────────────────
        StepOut StepTemple(RE::Actor* f, Work& w, Clock::time_point a_now) {
            StepOut out;
            const auto fid = f->GetFormID();
            if (a_now - w.lastProbe >= kProbeEvery && (w.stage == Stage::Seek || w.stage == Stage::Walk || w.stage == Stage::Kneel || w.stage == Stage::Activate)) {
                w.lastProbe = a_now;
                PostMagicProbe(fid);
            }
            auto* shrine = RefOf(w.shrine);
            auto toActivate = [&]() -> bool {
                APMFBridge::ReleaseErrandIdle(fid);   // the kneel's claims go first: a hold would block the Activate package's move
                w.before = MagicOf(fid);
                if (!shrine) { out.res = Result::Failed; out.why = "the shrine left memory"; return false; }
                std::string why;
                if (!Offer(f, w, APMFBridge::ErrandWorkKind::Activate, shrine, 0.0f, a_now, &why)) {
                    out.res = Result::Failed; out.why = "the Activate work could not be offered" + (why.empty() ? std::string{} : " (" + why + ")");
                    return false;
                }
                spdlog::info("[errand-temple] {:08X} '{}': ACTIVATE offered on shrine {:08X} -- before: {} active effect(s), diseased={}{}", fid, w.followerName,
                             w.shrine, w.before.effects, w.before.diseased, w.before.diseases.empty() ? std::string{} : " [" + w.before.diseases + "]");
                w.stage = Stage::Activate;
                w.stageAt = a_now;
                return true;
            };
            switch (w.stage) {
            case Stage::Seek: {
                RE::FormID base = 0;
                const RE::FormID id = FindShrine(f, base);
                if (!id) {
                    if (a_now - w.stageAt > kShrineSeek) { out.res = Result::Failed; out.why = "no shrine in the temple's cell"; }
                    break;
                }
                w.shrine = id;
                spdlog::info("[errand-temple] {:08X} '{}': SHRINE {:08X} (base {:08X}) found in '{}' -- walking to it", fid, w.followerName, id, base, w.name);
                const bool walk = APMFBridge::RepointErrandTravel(fid, id, kShrineRadius) || APMFBridge::ClaimErrandTravel(fid, id, kShrineRadius);
                if (!walk) { out.res = Result::Failed; out.why = "the walk to the shrine could not be declared"; break; }
                w.stage = Stage::Walk;
                w.stageAt = a_now;
                break;
            }
            case Stage::Walk: {
                if (!shrine) { out.res = Result::Failed; out.why = "the shrine left memory"; break; }
                const float dist = f->GetPosition().GetDistance(shrine->GetPosition());
                bool arrived = dist <= kShrineNear;
                APMFBridge::LootLegState leg;
                if (!arrived && APMFBridge::ReadErrandLeg(fid, leg) && leg.ours) {
                    if (leg.state == APMF_API::kLeg_Arrived) arrived = true;
                    else if (leg.state > APMF_API::kLeg_Walking && leg.state <= APMF_API::kLeg_Released) {   // blocked / combat / gone ...: close enough, else fail
                        if (dist <= 300.0f) arrived = true;
                        else { out.res = Result::Failed; out.why = std::format("the walk to the shrine ended before it ({} u away, leg state {})", static_cast<int>(dist), leg.state); break; }
                    }
                }
                if (!arrived && a_now - w.stageAt > kShrineWalk) {
                    if (dist <= 300.0f) arrived = true;
                    else { out.res = Result::Failed; out.why = std::format("the walk to the shrine timed out ({} u away)", static_cast<int>(dist)); break; }
                }
                if (!arrived) break;
                spdlog::info("[errand-temple] {:08X} '{}': at the shrine ({:.0f} u) -- kneel idle (ch.12 + ch.1)", fid, w.followerName, dist);
                if (APMFBridge::PlayErrandIdle(fid, kIdleKneelEnter, w.shrine)) { w.stage = Stage::Kneel; w.stageAt = a_now; w.confirmedAt = {}; w.kneelPlayed = true; }
                else { spdlog::warn("[errand-temple] {:08X}: the kneel idle was refused at the call (presentation only) -- praying standing", fid); toActivate(); }
                break;
            }
            case Stage::Kneel: {
                const auto rd = APMFBridge::ReadErrandIdle(fid);
                using V = APMFBridge::ErrandIdleVerdict;
                bool go = false;
                if (rd.verdict == V::Played) {
                    if (w.confirmedAt.time_since_epoch().count() == 0) { w.confirmedAt = a_now; spdlog::info("[errand-temple] {:08X}: kneel idle PLAYED ({})", fid, rd.detail); }
                    go = a_now - w.confirmedAt >= kKneelHold;
                } else if (rd.verdict == V::Failed || (rd.verdict == V::Waiting && a_now - w.stageAt > kKneelPatience)) {
                    spdlog::warn("[errand-temple] {:08X}: the kneel idle did not play ({}) -- the animation is presentation, praying anyway", fid, rd.detail);
                    go = true;
                }
                if (go) toActivate();
                break;
            }
            case Stage::Activate: {
                const auto m = MagicOf(fid);
                const bool fresh = m.valid && m.stamp > w.stageAt + std::chrono::seconds(1);
                const bool changed = fresh && (m.effects != w.before.effects || (w.before.diseased && !m.diseased) || (!w.before.blessed && m.blessed));
                const bool timeout = a_now - w.stageAt > kActivateWait;
                if (!changed && !timeout) { if (a_now - w.lastProbe >= std::chrono::seconds(1)) { w.lastProbe = a_now; PostMagicProbe(fid); } break; }
                spdlog::info("[errand-temple] {:08X} '{}': P-TEMPLE-1 {} -- before: {} effect(s), diseased={}; after: {} effect(s), diseased={}{} (shrine {:08X})", fid,
                             w.followerName, changed ? "the shrine's activation CHANGED his active effects" : "NO observable change after 15 s (did OnActivate fire with him?)",
                             w.before.effects, w.before.diseased, m.valid ? m.effects : -1, m.valid && m.diseased,
                             m.diseases.empty() ? std::string{} : " [" + m.diseases + "]", w.shrine);
                w.changed = changed;
                APMFBridge::ReleaseErrandWork(fid);
                if (w.kneelPlayed) APMFBridge::PlayErrandIdle(fid, kIdleKneelExit, w.shrine);
                w.stage = Stage::Exit;
                w.stageAt = a_now;
                break;
            }
            case Stage::Exit: {
                if (a_now - w.stageAt < kExitHold) break;
                APMFBridge::ReleaseErrandIdle(fid);
                out.res = Result::Done;
                out.why = w.changed ? "prayed (the blessing took)" : "prayed (no observable change)";
                break;
            }
            default: break;
            }
            return out;
        }
    }

    // ── public: the planner ─────────────────────────────────────────────────────────────────────────────
    bool Augment(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now, const RE::BGSLocation* a_town, bool a_haveStops,
                 std::vector<Stop>& a_stops, std::string& a_line) {
        (void)a_state; (void)a_now;
        if (!Config::g_errandInn.load() || !a_town || !a_follower) return a_haveStops;
        const auto fid = a_follower->GetFormID();
        PostMagicProbe(fid);   // the NEXT plan (6 s) reads it: disease is a main-thread read
        Place inn, temple;
        FindPlaces(a_follower, a_town, inn, temple);
        {   // not reachable by a door in his own cell: resolve through the data (see ResolveViaData)
            const char* how = "";
            if (!inn.ok && !inn.lockedSkip && ResolveViaData(a_town, true, inn, how))
                spdlog::info("[errand] {:08X}: inn '{}' (cell {:08X}) resolved through {} (no load door to it in his cell)", fid, inn.name, inn.cell, how);
            if (!temple.ok && !temple.lockedSkip && ResolveViaData(a_town, false, temple, how))
                spdlog::info("[errand] {:08X}: temple '{}' (cell {:08X}) resolved through {} (no load door to it in his cell)", fid, temple.name, temple.cell, how);
        }
        std::string templeWhy;
        const bool wantTemple = temple.ok && TempleWanted(fid, templeWhy);
        // marth 2026-10-19: with no shop work and no temple reason the trip is the INN ALONE (walk to the inn, then stay: sandbox / eat /
        // sleep until the player leaves town or combat starts). A fully stocked follower in town goes to the inn; TripDone ends it.
        const bool wasEmpty = a_stops.empty();
        std::string add;

        if (wantTemple && a_stops.size() + 2 <= kMaxTripStops) {
            Stop t;
            t.kind = StopKind::Temple;
            t.vendorName = temple.name;
            t.destKind = DestKind::Cell; t.destForm = temple.cell; t.destRad = 0.0f;
            t.pos = temple.door; t.havePos = temple.door.x != 0.0f || temple.door.y != 0.0f || temple.door.z != 0.0f;
            t.state = StopState::Pending;
            a_stops.push_back(std::move(t));
            add += std::format(" -> temple '{}' ({})", temple.name, templeWhy);
        }

        // ── R8 SMITH STOP: INSERTION POINT ─────────────────────────────────────────────────────────────────
        // Round R8 (smithing: sharpen / temper / forge) inserts its Smith stop(s) HERE -- after the shops and the temple, BEFORE the
        // inn -- so one trip covers weapons and armor worked on as well as bought. Reserve the slot: 5 shops + temple + smith + inn = the
        // 8 stops of the ERND wire (kMaxTripStops).

        // ── the INN: the LAST stop of every trip ───────────────────────────────────────────────────────────
        if (inn.ok && a_stops.size() < kMaxTripStops) {
            Stop s;
            s.kind = StopKind::Inn;
            s.vendorName = inn.name;
            s.destKind = DestKind::Cell; s.destForm = inn.cell; s.destRad = 0.0f;
            s.pos = inn.door; s.havePos = inn.door.x != 0.0f || inn.door.y != 0.0f || inn.door.z != 0.0f;
            s.state = StopState::Pending;
            // The innkeeper and a small food stock (R0-10: judged from the food's real effects; the stock is iInnFoodStock worthy foods).
            std::string food = "rest only";
            const int want = Config::g_innFoodStock.load();
            const int have = CountFood(a_follower, true);
            const int purse = GoldOf(a_follower);
            if (want > have && purse > 0) {
                const auto facs = InnFactionsOf(a_town);
                for (const auto& fac : facs) {
                    // The faction whose place sits in THIS inn's location; a town with a single innkeeper faction and a single inn matches
                    // even when the place resolves to a sub-location.
                    if (fac.locId != inn.locId && facs.size() != 1) continue;
                    s.faction = fac.faction; s.chest = fac.chest;
                    s.order = BuildFoodOrder(a_follower, fac, want - have, purse);
                    food = std::format("food {}/{} worthy in stock, buys up to {}, purse {}", have, want, want - have, purse);
                    break;
                }
                if (!s.order) food = std::format("no innkeeper faction for it ({} food of {}); rest only", have, want);
            } else {
                food = std::format("food stock {}/{}{}", have, want, purse > 0 ? "" : ", no gold");
            }
            a_stops.push_back(std::move(s));
            add += std::format(" -> INN '{}' ({}; LAST stop, he stays until the player leaves town)", inn.name, food);
        } else if (!inn.ok) {
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextLog;
            if (auto& nx = s_nextLog[fid]; nx.time_since_epoch().count() == 0 || a_now >= nx) {
                nx = a_now + std::chrono::seconds(120);
                spdlog::info("[errand] {:08X}: no inn stop -- no load door in his cell leads to an inn interior of '{}' (or the inn is not in this part of town)",
                             fid, a_town->GetName() ? a_town->GetName() : "?");
            }
        }
        if (wasEmpty && !a_stops.empty()) a_stops.front().state = StopState::Active;
        if (!add.empty()) a_line += (a_line.empty() ? std::string{} : std::string(" | ")) + "R7" + add;
        return !a_stops.empty();
    }

    // ── public: one trip per town visit ─────────────────────────────────────────────────────────────────
    void NoteTown(RE::FormID a_townId) {
        std::scoped_lock lk(g_mx);
        if (a_townId == g_marksTown) return;
        if (!g_tripDone.empty())
            spdlog::info("[errand] the player's town changed ({:08X} -> {:08X}): {} follower(s)' trip-done mark(s) cleared", g_marksTown, a_townId, g_tripDone.size());
        g_marksTown = a_townId;
        g_tripDone.clear();
        g_tripLogged.clear();
        g_lockLogged.clear();
        g_magicWaitLogged.clear();
        g_magicDefers.clear();
    }
    bool MagicReadPending(RE::Actor* a_follower) {
        if (!a_follower || !Config::g_errandInn.load()) return false;
        const auto fid = a_follower->GetFormID();
        const bool loaded = a_follower->Is3DLoaded();
        const auto m = MagicOf(fid);
        const bool fresh = m.valid && Clock::now() - m.stamp <= std::chrono::seconds(30);
        if (fresh) { std::scoped_lock lk(g_mx); g_magicDefers.erase(fid); return false; }
        bool first = false;
        int defers = 0;
        { std::scoped_lock lk(g_mx); first = g_magicWaitLogged.insert(fid).second; defers = ++g_magicDefers[fid]; }
        if (loaded && defers > kMagicMaxDefers) {   // never stall silently: the main-thread read did not land in ~30 s
            spdlog::warn("[errand] {:08X}: the blessing/disease read did not land after {} defers (~{} s) -- planning WITHOUT the temple decision (is the main-thread Post running?)",
                         fid, defers - 1, (defers - 1) * 6);
            { std::scoped_lock lk(g_mx); g_magicDefers[fid] = 0; }
            return false;
        }
        if (!loaded) {   // an unloaded follower's list is not readable: today's behaviour (TempleWanted wants nothing without a read)
            if (first) spdlog::info("[errand] {:08X}: not loaded -- no blessing/disease read possible, planning without the temple", fid);
            return false;
        }
        if (first) spdlog::info("[errand] {:08X}: waiting for the blessing/disease read (main-thread active-effect mirror) before planning", fid);
        PostMagicProbe(fid);
        return true;
    }
    bool TripDone(RE::FormID a_follower, RE::FormID a_townId) {
        std::scoped_lock lk(g_mx);
        if (!g_tripDone.contains(a_follower)) return false;
        if (g_tripLogged.insert(a_follower).second)
            spdlog::info("[errand] {:08X}: re-plan BLOCKED -- his trip for this visit to town {:08X} is done (one complete trip per town visit); no new errand until the player leaves town",
                         a_follower, a_townId);
        return true;
    }
    void MarkTripDone(RE::FormID a_follower, RE::FormID a_townId, const char* a_why) {
        {
            std::scoped_lock lk(g_mx);
            g_tripDone.insert(a_follower);
            g_tripLogged.erase(a_follower);
        }
        spdlog::info("[errand] {:08X}: TRIP DONE for this visit to town {:08X} ({}) -- no new errand until the player leaves town", a_follower, a_townId, a_why);
    }

    // ── public: the food order's dispatch ───────────────────────────────────────────────────────────────
    Dispatch DispatchOrder(RE::Actor* a_follower, RE::Actor* a_vendor, const ErrandPlanned& a_order, int a_purseCap) {
        auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_order.chest);
        const int purse = a_purseCap >= 0 ? std::min(a_order.purse, a_purseCap) : a_order.purse;
        if (!chest || purse <= 0) return Dispatch::Nothing;
        const bool ok = TradeBridge::VendorTrade(a_follower, a_vendor, chest, a_order.sell, a_order.needs, purse, a_order.buy, a_order.sig, a_order.daysNow,
                                                 a_order.vendorKey, &a_order.quote);
        if (ok)
            spdlog::info("[errand] {:08X}: the inn's FOOD order dispatched at chest {:08X}: up to {} item(s), purse {}", a_follower->GetFormID(), a_order.chest,
                         a_order.needs.empty() ? 0 : a_order.needs.front().quota, purse);
        return ok ? Dispatch::Sent : Dispatch::Busy;
    }

    // ── public: the physical work ───────────────────────────────────────────────────────────────────────
    bool Begin(RE::Actor* a_follower, const Stop& a_stop, RE::FormID a_vendor, const std::string& a_followerName, RE::FormID a_town, Clock::time_point a_now) {
        if (!a_follower) return false;
        const auto fid = a_follower->GetFormID();
        const bool inn = a_stop.kind == StopKind::Inn;
        if (!Forms::ErrandWorkPackage(inn ? Forms::ErrandWorkKind::Sandbox : Forms::ErrandWorkKind::Activate, 0)) {
            spdlog::warn("[errand] {:08X} '{}': the {} work package did not resolve (an MFO.esp older than R7?) -- the stop is skipped", fid, a_followerName,
                         inn ? "inn (Sandbox)" : "temple (Activate)");
            return false;
        }
        Work w;
        w.kind = a_stop.kind;
        w.name = a_stop.vendorName.empty() || a_stop.vendorName == "?" ? (inn ? "the inn" : "the temple") : a_stop.vendorName;
        w.followerName = a_followerName;
        w.vendor = a_vendor;
        w.town = a_town;
        w.began = w.stageAt = a_now;
        w.lastProbe = a_now - kProbeEvery;
        w.lastState = static_cast<int>(a_follower->AsActorState()->GetSitSleepState());
        if (inn) {
            const char* what = "";
            w.anchor = FindAnchor(a_follower, a_vendor, what);
            w.stage = (Config::g_innMeals.load() > 0 && CountFood(a_follower, false) > 0) ? Stage::Meal : Stage::Stay;
            detail::ToastResting(a_followerName, w.name);
            spdlog::info("[errand] {:08X} '{}': INN STAY BEGINS at '{}' -- anchor {:08X} ({}); {}; he stays (rest / sit / sleep through the ch.9 work package) until "
                         "the player leaves town or combat starts", fid, a_followerName, w.name, w.anchor, what,
                         w.stage == Stage::Meal ? "he eats first" : "no meal (none carried or iInnMeals 0)");
        } else {
            w.stage = Stage::Seek;
            { const int today = GameDay(); std::scoped_lock lk(g_mx); g_templeTry[fid] = today; }
            PostMagicProbe(fid);
            detail::ToastPraying(a_followerName, w.name);
            spdlog::info("[errand] {:08X} '{}': TEMPLE '{}' reached -- seeking the shrine in his cell", fid, a_followerName, w.name);
        }
        std::scoped_lock lk(g_mx);
        g_work[fid] = w;
        return true;
    }

    StepOut Step(RE::Actor* a_follower, Clock::time_point a_now) {
        StepOut out;
        if (!a_follower) { out.res = Result::Failed; out.why = "no follower"; return out; }
        const auto fid = a_follower->GetFormID();
        Work w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_work.find(fid);
            if (it == g_work.end()) { out.res = Result::Failed; out.why = "the stop's work state is gone"; return out; }
            w = it->second;
        }
        out = w.kind == StopKind::Inn ? StepInn(a_follower, w, a_now) : StepTemple(a_follower, w, a_now);
        Put(fid, w);
        return out;
    }

    void End(RE::FormID a_follower, const char* a_why, bool a_tripOver) {
        Work w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_work.find(a_follower);
            if (it == g_work.end()) return;
            w = it->second;
            g_work.erase(it);
        }
        // a_tripOver false = the HAND-OFF to the simulation: the ch.9 work claim STANDS (it holds the unloaded follower in the inn / temple;
        // marth 2026-10-20 "mfo claims need to hold for the duration of the mfo event"). The sim's Drop / OnLoadedTick or Finish releases it.
        if (a_tripOver) APMFBridge::ReleaseErrandWork(a_follower);
        if (w.kind == StopKind::Temple && a_tripOver) APMFBridge::ReleaseErrandIdle(a_follower);   // hand-off: the kneel's ch.12 / ch.1 claims stand too (released by the sim: MoveHold / Drop / PlaceReturn)
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - w.began).count();
        if (w.kind == StopKind::Inn)
            spdlog::info("[errand] {:08X} '{}': INN STAY ENDS at '{}' after {} s -- released: {} ({}; meals eaten {})", a_follower, w.followerName, w.name, secs, a_why,
                         a_tripOver ? "the trip is over" : "handed to the simulation", w.mealsEaten);
        else
            spdlog::info("[errand] {:08X} '{}': TEMPLE VISIT ENDS at '{}' after {} s -- {}", a_follower, w.followerName, w.name, secs, a_why);
        if (a_tripOver && w.kind == StopKind::Inn) MarkTripDone(a_follower, w.town, "the inn stay ended");
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_work.clear();
        g_magic.clear();
        g_templeTry.clear();
        g_tripDone.clear();
        g_tripLogged.clear();
        g_lockLogged.clear();
        g_magicWaitLogged.clear();
        g_magicDefers.clear();
        g_innFacs.clear();
        g_marksTown = 0;
    }

    bool Working(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        return g_work.contains(a_follower);
    }

    std::string Describe(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_work.find(a_follower);
        if (it == g_work.end()) return {};
        return (it->second.kind == StopKind::Inn ? "Resting at " : "Praying at ") + it->second.name;
    }

    bool InStopCell(RE::Actor* a_follower, const Stop& a_stop) {
        auto* c = a_follower ? a_follower->GetParentCell() : nullptr;
        if (!c) return false;
        if (a_stop.destKind == DestKind::Cell && a_stop.destForm && c->GetFormID() == a_stop.destForm) return true;
        const RE::BGSLocation* loc = c->GetLocation();
        return loc && (a_stop.kind == StopKind::Inn ? LocationTypes::IsInn(loc) : a_stop.kind == StopKind::Temple && LocationTypes::IsTemple(loc));
    }

    void LogBlockedDoors(RE::Actor* a_follower, RE::FormID a_destCell, const std::string& a_label) {
        if (!a_follower || !a_destCell) return;
        auto* cell = a_follower->GetParentCell();
        if (!cell || !cell->IsAttached()) {
            spdlog::info("[errand] {:08X}: BLOCKED walk to '{}' (cell {:08X}) -- no loaded cell around him to list its load doors", a_follower->GetFormID(), a_label, a_destCell);
            return;
        }
        std::vector<RE::ObjectRefHandle> doors;
        cell->ForEachReferenceInRange(a_follower->GetPosition(), kScan, [&](RE::TESObjectREFR* a_ref) {
            if (doors.size() < 96 && a_ref->extraList.GetByType<RE::ExtraTeleport>()) doors.push_back(a_ref->GetHandle());
            return RE::BSContainer::ForEachResult::kContinue;
        });
        int found = 0;
        for (auto& h : doors) {
            auto dp = h.get();
            auto* d = dp.get();
            auto* tele = d ? d->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
            if (!tele || !tele->teleportData) continue;
            auto lp = tele->teleportData->linkedDoor.get();
            auto* lc = lp.get() ? lp.get()->GetParentCell() : nullptr;
            if (!lc || lc->GetFormID() != a_destCell) continue;
            ++found;
            const auto dl = d->GetLockLevel();
            const auto ll = lp.get()->GetLockLevel();
            spdlog::info("[errand] {:08X}: BLOCKED walk to '{}' (cell {:08X}) -- load door {:08X} {:.0f} u from him: lock level {} ({}), its linked door {:08X}: lock level {} ({})",
                         a_follower->GetFormID(), a_label, a_destCell, d->GetFormID(), d->GetPosition().GetDistance(a_follower->GetPosition()),
                         static_cast<int>(dl), LockWord(dl), lp.get()->GetFormID(), static_cast<int>(ll), LockWord(ll));
        }
        if (!found)
            spdlog::info("[errand] {:08X}: BLOCKED walk to '{}' (cell {:08X}) -- no load door to it within {:.0f} u of him in cell {:08X} (the door is in another cell)",
                         a_follower->GetFormID(), a_label, a_destCell, kScan, cell->GetFormID());
    }

    void NoteOwed(RE::FormID a_follower, const std::string& a_followerName, const std::string& a_temple) {
        const int today = GameDay();
        { std::scoped_lock lk(g_mx); g_templeTry[a_follower] = today; }
        spdlog::info("[errand] {:08X} '{}': TEMPLE_SIM -- the visit to '{}' ran off screen; the blessing / cure is OWED (the shrine ref is not persistent, so only a "
                     "physical visit pays it)", a_follower, a_followerName, a_temple);
    }
}
