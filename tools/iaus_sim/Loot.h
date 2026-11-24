#pragma once
// iaus_sim/Loot.h -- SIM V4 BATCH L: LOOTING INSIDE THE FIGHT WORLD (iaus-sim-v4.md section 3 and 14). HOST-ONLY.
//
// The old lab (Logistics.h) is 8 abstract stops. Here a follower loots in the place the fight was fought: bodies lie where the foes fell,
// containers, loose items and flora stand where the place has them, he walks to them on the navmesh, and the clock is the fight's own.
// EVERY RULE BELOW IS A MIRROR of native/logistics, written from the code (the reading is _agentlogs/mfo/sim-loot-native-spec.md, each rule
// names its native file). The decision of WHAT to take is native's own code compiled in (iaus/LootNeed.h ScoreLootKind / ScoreConsumable, the
// LootTuning table under search = Tune().loot). Everything else in native/logistics uses engine types, so it is re-stated here: iaus-sim-v4.md
// 14.6 lists what should be extracted so this file can include it instead.
//
// THE CLOCKS (Diagnostics.cpp kPumpMs 133, Logistics_internal.h kLogisticsInterval 1000): one follower is served per pump, round robin, and a
// follower's loot tick runs at his first service >= 1 s after his last. So with N followers a loot decision is 1.06 to 1.9 s apart, and
// every leg end, arrival and release is seen that late. Movement runs every world step (TM::dt).
//
// WHAT IS A MODEL HERE (no native source): how the leg mover reports Movement Blocked (no ground made for Rules::blockedGateSec while a leg
// wants him to move), the follow package (he walks back to the player past Rules::followU), a guard's patrol, a wave of foes arriving.
// What is NOT built: lock picking (a locked container is left, counted), the load-door stuck rescue, the LOTD deposit trip, the town errand
// during an episode (Logi::Town runs at its end as in the old lab), the teleport-mod clamp, QuickLoot's crosshair.
#include "iaus_sim/Logistics.h"
#include "iaus_sim/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <deque>
#include <functional>
#include <tuple>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Sim::Loot {

    using Logi::Inv; using Logi::ItemDef; using Logi::Items; using Logi::Kind; using Logi::Stack; using Logi::Who;
    inline std::size_t Z(int i) { return static_cast<std::size_t>(i); }

    // ── native's constants, ONE table. Each row names where native keeps it. To change a rule's number: here (or --loot-rule name=v). ──
    struct Rules {
        // clocks
        float pumpSec = 0.133f;        // Diagnostics.cpp kPumpMs
        float logiSec = 1.0f;          // Logistics_internal.h kLogisticsInterval
        float landingSec = 0.15f;      // Logistics_internal.h RearmForLanding
        // Config.cpp defaults (INI)
        float lootRadius = 3000.0f, travelRadius = 4096.0f, leashMin = 512.0f, leashMax = 4000.0f, looseAcquire = 300.0f, navGate = 300.0f, bubble = 150.0f;
        float excursionMax = 60.0f, firstDibs = 4.0f, quickLootWaiver = 4.0f, chanceRadius = 512.0f, fairChance = 6.0f, abandonDelay = 45.0f, departRadius = 700.0f;
        float lotd = 1.0f;             // bLootLOTD. Native's default is 0. The lab's is 1: the field lists run it on and marth's complaint names museum items
        // internals
        float releaseMul = 1.15f;      // TeleportCompat.cpp: the follower-to-player release = leash x 1.15
        float departRelease = 3.0f;    // Logistics_internal.h kDepartRelease
        float grab = 200.0f, grabGrow = 100.0f, grabMax = 600.0f;   // LootTravel_internal.h GrabRadiusFor
        float arriveRadius = 128.0f;   // LootScan.cpp:25, the ch.19 claim's radius
        float moveEps = 50.0f, noProgressSec = 5.0f;                // LootTravel_internal.h kMoveEps, kNoProgress
        float blockedGateSec = 3.0f;   // kBlockedGate, and Harbinger's own Movement Blocked hold
        float transientSec = 25.0f, stickySec = 60.0f;              // MarkTravelFailed / MarkTravelSticky
        float farCloseU = 25.0f;       // Service.cpp far arrival: a second one that did not close this much is a stall strike
        float deadlineSpeed = 150.0f, deadlineBase = 5.0f, deadlineMin = 6.0f, deadlineMax = 20.0f;   // LootTravel_internal.h TravelDeadline
        float actorBlockU = 200.0f, actorBlockDz = 128.0f;          // FindActorBlocker
        float obstacleU = 256.0f, obstacleLane = 128.0f, obstacleBack = 64.0f;   // FindGateObstacle
        float gateMinU = 64.0f, gateFloorU = 256.0f, readmitEventU = 2048.0f;    // BehindBlock, the door / lever events
        float stuckWindowSec = 90.0f;  // StuckAtOrigin.cpp: a hit older than this leaves the run
        float guardRangeU = 3500.0f, guardReaskSec = 3.0f, guardWarmSec = 15.0f;   // TheftGate.cpp ScanGuards, CorpseBarred, KeepWarm
        float idleReassessSec = 15.0f, idleAgeSec = 10.0f;          // Service.cpp idle reassess
        float witnessU = 1536.0f;      // Steal.cpp: max(iCrimeAlarmRecDistance, 1536)
        float maxSlots = 4.0f, candCap = 48.0f, stuckHits = 2.0f, idleTicks = 4.0f, stallStrikes = 2.0f;
        // MODEL (header)
        float followU = 400.0f;        // the follow package: past this from the player he walks back
        float driftShare = 0.07f;      // CALIBRATED per LEG, never per arrival (marth's method constraint): the four field sessions print 12 'ARRIVED but he is N u away' on 277 legs asked for (4.3 percent). After a leg ended ARRIVED the follow package takes him back this often before MFO's tick reads the arrival, otherwise the claim holds him at the ref. At 0.07 the lab prints the line on about 4 percent of its legs (14.8)
        float carryCap = Logi::kCarryCap;
        // THE RULES OF OLDER BUILDS, for the scenarios that must fail on them:
        float museumGold = 250.0f;     // ASSUMPTION (review S5): what a museum piece taken is worth, in gold, whatever it sells for (a display has no price; the errand never sells it). One knob, reported apart (dMuseumJ)
        float stealRecheck = 1.0f;     // Steal.cpp: the witness check is run AGAIN at the write (Service.cpp:1019). 0 = the cached verdict alone (scenario thief-no-recheck: must commit a crime)
        // MARTH'S RULINGS OF 11-24, IN NATIVE SINCE efd097dc. The default is native; 0 = the builds before it (--loot-rule dibsNear=0 / gateEvents=0):
        float dibsNearEnough = 1.0f;   // coin dibs "Near is enough": the fair chance accrues while the player is within chanceRadius, facing or not (6 s near releases; LootTake.cpp TierReleased). 0 = the builds before efd097dc (near AND facing, finding N1)
        float gateEventsOnly = 1.0f;   // a gate lifts on REAL EVENTS only (door, lever, cell, somebody else at the item, the player moving through). Native says it with gateReadmitMoveU 0; 0 here together with LootTuning::gateReadmitMoveU 500 = the builds before efd097dc (the recorder's own walk off his block lifts his record, finding N6)
        float native1124 = 1.0f;       // the looting fixes of efd097dc (N2, N3, N4, N6). 0 = the build before it: the idle lift, the sweep only inside a scan, no pass lift, a gate feeds the stuck run, no live-gate rule, no same-item stall strike, an equipment-only carry peek. ONE switch, for the scenarios that must reproduce the old faults
        float gateIdleLift = 0.0f;     // 1 = four idle ticks lift the follower's own records (builds before efd097dc; native1124=0 has it too)
        // MIRRORS of constants inside a native function body (LootTravel_internal.h GateReadmitSweep kPassMaxGapSec / kPassSlackU). test_tune_driver.py compares them, and gateFloorU with kGateFloorBand
        float passMaxGapSec = 3.0f, passSlackU = 64.0f;
        float corpseBar = 1.0f;        // 0 = before the party-wide guard bar on a corpse (field 11-17 L1)
        float gateOld1117 = 0.0f;      // 1 = before 9a16a278 (11-17): every static block gates the target and its cone, no repeat / short test, no move / near / idle re-admit, stuck needs 3 distinct targets
    };
    inline bool SetRule(Rules& r, const std::string& name, float v) {
        struct F { const char* n; float Rules::* m; };
        static const F k[] = { { "pumpSec", &Rules::pumpSec }, { "logiSec", &Rules::logiSec }, { "landingSec", &Rules::landingSec }, { "lootRadius", &Rules::lootRadius }, { "travelRadius", &Rules::travelRadius },
            { "leashMin", &Rules::leashMin }, { "leashMax", &Rules::leashMax }, { "looseAcquire", &Rules::looseAcquire }, { "navGate", &Rules::navGate }, { "bubble", &Rules::bubble }, { "excursionMax", &Rules::excursionMax },
            { "firstDibs", &Rules::firstDibs }, { "quickLootWaiver", &Rules::quickLootWaiver }, { "chanceRadius", &Rules::chanceRadius }, { "fairChance", &Rules::fairChance }, { "abandonDelay", &Rules::abandonDelay },
            { "departRadius", &Rules::departRadius }, { "lotd", &Rules::lotd }, { "releaseMul", &Rules::releaseMul }, { "departRelease", &Rules::departRelease }, { "grab", &Rules::grab }, { "arriveRadius", &Rules::arriveRadius },
            { "noProgressSec", &Rules::noProgressSec }, { "blockedGateSec", &Rules::blockedGateSec }, { "transientSec", &Rules::transientSec }, { "stickySec", &Rules::stickySec }, { "deadlineMax", &Rules::deadlineMax },
            { "deadlineSpeed", &Rules::deadlineSpeed }, { "stuckWindowSec", &Rules::stuckWindowSec }, { "stuckHits", &Rules::stuckHits }, { "guardRangeU", &Rules::guardRangeU }, { "maxSlots", &Rules::maxSlots },
            { "candCap", &Rules::candCap }, { "followU", &Rules::followU }, { "carryCap", &Rules::carryCap }, { "idleTicks", &Rules::idleTicks }, { "gate", &Rules::gateOld1117 }, { "corpseBar", &Rules::corpseBar }, { "dibsNear", &Rules::dibsNearEnough }, { "gateEvents", &Rules::gateEventsOnly }, { "native1124", &Rules::native1124 }, { "gateIdle", &Rules::gateIdleLift }, { "passMaxGapSec", &Rules::passMaxGapSec }, { "passSlackU", &Rules::passSlackU }, { "stealRecheck", &Rules::stealRecheck }, { "museumGold", &Rules::museumGold }, { "driftShare", &Rules::driftShare }, { "guardWarmSec", &Rules::guardWarmSec } };
        for (const F& f : k) if (name == f.n) { r.*(f.m) = v; return true; }
        return false;
    }
    inline Rules g_rules;   // --loot-rule name=v (and `gate=old1117`)

    // ── categories (logistics Category; the first ten are iaus/LootNeed.h LootKind in its order) ──
    enum class Cat : std::uint8_t { Arrows = 0, Bolts, Potions, Equipment, Gold, Jewelry, SoulGems, Lockpicks, Ingredients, Valuables, Museum, Steal, kCount };
    inline constexpr std::size_t kCats = static_cast<std::size_t>(Cat::kCount);
    inline const char* CatName(Cat c) { static const char* n[] = { "arrows", "bolts", "potions", "equipment", "gold", "jewelry", "soulgems", "lockpicks", "ingredients", "valuables", "museum", "steal" }; return n[static_cast<std::size_t>(c)]; }
    [[nodiscard]] inline bool FreeTier(Cat c) { return c == Cat::Arrows || c == Cat::Bolts || c == Cat::Potions || c == Cat::Lockpicks || c == Cat::Ingredients || c == Cat::Museum || c == Cat::Steal; }   // LootTake.cpp TierReleased
    [[nodiscard]] inline bool Clutter(Cat c) { return c == Cat::Lockpicks || c == Cat::Arrows || c == Cat::Bolts; }   // Logistics_internal.h IsClutterCat
    // The engine form type of an item: a take of one form type is refused while another of the same type is still landing (TheftGate.cpp landing hold).
    [[nodiscard]] inline int FormType(Kind k) {
        switch (k) {
        case Kind::PotionH: case Kind::PotionM: case Kind::PotionS: case Kind::Buff: case Kind::Food: case Kind::Poison: return 1;   // ALCH
        case Kind::Ammo: case Kind::Bolt: return 2;                                                                                   // AMMO
        case Kind::Weapon1H: case Kind::Weapon2H: case Kind::Bow: return 3;                                                           // WEAP
        case Kind::ArmorBody: case Kind::ArmorHead: case Kind::Jewelry: return 4;                                                     // ARMO
        case Kind::SoulGem: return 5; case Kind::Ingredient: return 6; case Kind::Book: return 7;
        case Kind::OffGear: return 8;   // a shield / staff / clothing: never taken by a road of the lab
        default: return 0;                                                                                                            // MISC: gold, lockpicks, gems, junk
        }
    }
    [[nodiscard]] inline bool ValuableMisc(const ItemDef& d) { return (d.kind == Kind::Gem || d.kind == Kind::Junk) && (d.weight > 0 ? d.value / d.weight >= Logi::kValuablesRatio : d.value > 0); }   // LootTake.cpp IsValuableMisc

    // ── a list's loot tables (tools/list_balance/extract_loot.py). TEST INPUT: joined by (plugin file, local id), never by a name. ──
    struct LootList {
        bool on = false; std::string name;
        std::unordered_map<std::string, int> item;                               // key -> index into Logi::Items()
        std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> cont;   // container base -> rows
        struct Lvl { int flags = 0, none = 0; std::vector<std::tuple<int, std::string, int>> e; };
        std::unordered_map<std::string, Lvl> lvli;
        std::unordered_map<std::string, std::string> flora;
        std::vector<std::vector<std::pair<std::string, int>>> bodies;
        std::deque<std::string> names;
    };
    inline LootList g_list;
    inline float g_museumShare = 0.02f;   // --loot-museum <share>: MODEL. This share of a list's MISC, weapon, armor and jewelry records is flagged museum-needed, by a hash of the record's key (which items a museum wants is the player's game state, in no extract)

    // native logistics/PotionFx.cpp KindOf, restated with the engine's numbers (RE/E/EffectArchetypes.h, RE/A/ActorValues.h of the mit-3.7 fork).
    inline bool BuffKindOf(int arch, int av, bool recover, MFO::Iaus::BuffKind& kind, int& skillAv) {
        using MFO::Iaus::BuffKind; skillAv = -1;
        if (arch == 29) { kind = BuffKind::CurePoison; return true; } if (arch == 3) { kind = BuffKind::CureDisease; return true; } if (arch == 11) { kind = BuffKind::Invisibility; return true; }
        if (arch != 0 && arch != 5 && arch != 34) return false;
        const bool peak = arch == 34;
        switch (av) {
        case 41: kind = BuffKind::ResistFire; return true; case 43: kind = BuffKind::ResistFrost; return true; case 42: kind = BuffKind::ResistShock; return true;
        case 40: kind = BuffKind::ResistPoison; return true; case 44: kind = BuffKind::ResistMagic; return true; case 57: kind = BuffKind::WaterBreathing; return true;
        case 27: case 155: kind = BuffKind::RegenHealth; return true; case 29: case 157: kind = BuffKind::RegenStamina; return true; case 28: case 156: kind = BuffKind::RegenMagicka; return true;
        case 154: case 34: case 35: kind = BuffKind::FortifyAttack; return true;
        case 24: if (!(recover || peak)) return false; kind = BuffKind::FortifyHealth; return true;
        case 26: if (!(recover || peak)) return false; kind = BuffKind::FortifyStamina; return true;
        case 25: if (!(recover || peak)) return false; kind = BuffKind::FortifyMagicka; return true;
        default: break;
        }
        if (const int n = MFO::Iaus::BaseSkillAv(av); n >= 0) { kind = BuffKind::FortifySkill; skillAv = n; return true; }
        return false;
    }
    inline bool LoadLootList(const std::string& path, std::string* err) {
        std::FILE* f = std::fopen(path.c_str(), "rb"); if (!f) { if (err) *err = "cannot open " + path; return false; }
        std::string s; char buf[1 << 16]; std::size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n); std::fclose(f);
        JP p{ s }; const J root = p.val(); if (root.t != J::Obj || !root["items"].has()) { if (err) *err = path + ": not a loot extract (tools/list_balance/extract_loot.py)"; return false; }
        LootList& L = g_list; L = LootList{}; L.on = true; L.name = root["list"].s; std::vector<ItemDef>& I = Logi::ItemsStore(); auto& CR = Logi::ConsRows();
        for (const auto& [key, row] : root["items"].o) {
            const std::string& k = row[0].s; ItemDef d{}; L.names.push_back(key); d.edid = L.names.back().c_str(); d.value = static_cast<int>(row[1].num()); d.weight = static_cast<float>(row[2].num()); d.power = static_cast<float>(row[3].num()); d.weightModel = false;
            const J& ex = row[4];
            if (k == "gold") d.kind = Kind::Gold; else if (k == "lockpick") d.kind = Kind::Lockpick; else if (k == "gem") d.kind = Kind::Gem; else if (k == "ingredient") d.kind = Kind::Ingredient;
            else if (k == "soulgem") d.kind = Kind::SoulGem; else if (k == "key" || k == "book" || k == "scroll") d.kind = Kind::Book; else if (k == "ammo") d.kind = Kind::Ammo; else if (k == "bolt") d.kind = Kind::Bolt;
            else if (k == "weapon1h") d.kind = Kind::Weapon1H; else if (k == "weapon2h") d.kind = Kind::Weapon2H; else if (k == "bow") d.kind = Kind::Bow; else if (k == "armorbody") d.kind = Kind::ArmorBody;
            else if (k == "armorhead") d.kind = Kind::ArmorHead; else if (k == "jewelry") d.kind = Kind::Jewelry;
            else if (k == "crossbow" || k == "staff" || k == "shield" || k == "armorhands" || k == "armorfeet" || k == "clothing") d.kind = Kind::OffGear;
            else if (k == "potion" || k == "food" || k == "poison") {   // native logistics/LootNeed.cpp ReadConsumableImpl, from the record's effect rows
                using namespace MFO::Iaus; Consumable c; c.value = float(d.value); c.weight = d.weight; c.food = k == "food"; c.poison = k == "poison"; bool anyHostile = false; int fam = -1; float mag = 0;
                for (const J& e : ex.a) { const int arch = static_cast<int>(e[0].num()), av = static_cast<int>(e[1].num()), fl = static_cast<int>(e[4].num()); const float m = static_cast<float>(e[2].num()), du = static_cast<float>(e[3].num());
                    const bool hostile = (fl & 1) || (fl & 2), recover = fl & 4; anyHostile = anyHostile || hostile; c.maxDuration = std::max(c.maxDuration, du);
                    if (c.poison) { if (hostile) c.poisonPower += m * std::max(1.0f, du); continue; }
                    if (hostile) continue;
                    const int fa = av == 24 ? kRestoreHealth : av == 26 ? kRestoreStamina : av == 25 ? kRestoreMagicka : -1;
                    if (fa >= 0 && (arch == 0 || arch == 5) && !recover) { if (c.food) { c.foodRestoreMag[Z(fa)] = std::max(c.foodRestoreMag[Z(fa)], m); c.foodRestoreDur[Z(fa)] = du; } else if (fam < 0) { fam = fa; mag = m; } }
                    BuffKind bk; int sk; if (BuffKindOf(arch, av, recover, bk, sk)) { PotionFx fx; fx.kind = bk; fx.magnitude = m; fx.duration = du; fx.av = sk; c.fx.push_back(fx); } }
                c.hostile = c.poison || (anyHostile && !c.food);   // MODEL of PotionIsDrug: a drink with a hostile effect
                if (c.hostile && !c.poison) { c.fx.clear(); fam = -1; }
                if (fam >= 0) c.restoreMag[Z(fam)] = mag > 0.0f ? mag : 1.0f;
                d.kind = c.poison ? Kind::Poison : c.food ? Kind::Food : fam == kRestoreHealth ? Kind::PotionH : fam == kRestoreMagicka ? Kind::PotionM : fam == kRestoreStamina ? Kind::PotionS : Kind::Buff;
                d.power = c.poison ? c.poisonPower : fam >= 0 ? mag : c.fx.empty() ? 0.0f : c.fx[0].magnitude;
                if (d.kind == Kind::Buff && !c.fx.empty()) { d.bk = static_cast<int>(c.fx[0].kind); d.av = c.fx[0].av; d.dur = c.fx[0].duration; }
                if (d.kind == Kind::Food) { d.rfam = 0; for (int q = 0; q < 3; ++q) if (c.foodRestoreMag[Z(q)] > 0) { d.rfam = q; break; } }
                d.cons = static_cast<int>(CR.size()); CR.push_back(std::move(c)); }
            else d.kind = Kind::Junk;   // misc, ingot, ore, hide
            if (g_museumShare > 0.0f && (d.kind == Kind::Junk || d.kind == Kind::Gem || d.kind == Kind::Weapon1H || d.kind == Kind::Weapon2H || d.kind == Kind::Bow || d.kind == Kind::ArmorBody || d.kind == Kind::ArmorHead || d.kind == Kind::Jewelry || d.kind == Kind::OffGear)) {
                std::uint64_t h = 1469598103934665603ULL; for (char ch : key) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL; d.museum = static_cast<float>(SimEnv::Mix(h) >> 40) * (1.0f / 16777216.0f) < g_museumShare; }
            L.item[key] = static_cast<int>(I.size()); I.push_back(d); }
        for (const auto& [key, rows] : root["cont"].o) { auto& v = L.cont[key]; for (const J& r : rows.a) v.push_back({ r[0].s, static_cast<int>(r[1].num()) }); }
        for (const auto& [key, lv] : root["lvli"].o) { LootList::Lvl& x = L.lvli[key]; x.flags = static_cast<int>(lv["f"].num()); x.none = static_cast<int>(lv["none"].num()); for (const J& e : lv["e"].a) x.e.push_back({ static_cast<int>(e[0].num()), e[1].s, static_cast<int>(e[2].num()) }); }
        for (const auto& [key, pr] : root["flora"].o) L.flora[key] = pr.s;
        for (const J& b : root["bodies"].a) { std::vector<std::pair<std::string, int>> v; for (const J& r : b.a) v.push_back({ r[0].s, static_cast<int>(r[1].num()) }); L.bodies.push_back(std::move(v)); }
        Logi::g_potionFloor = -1.0f; return true;
    }
    // A row of a container or an inventory: an item, or a leveled list drawn as the engine draws it (LVLF 1 = every level up to the player's, else the
    // highest level not above his; 4 = every entry; LVLD = chance of nothing). Depth-capped: a list can name itself.
    inline void Resolve(const std::string& key, int count, int level, Rng& R, std::vector<Stack>& out, int depth = 0) {
        const LootList& L = g_list; if (count <= 0 || depth > 8) return;
        if (const auto it = L.item.find(key); it != L.item.end()) { for (Stack& s : out) if (s.item == it->second) { s.count += count; return; } out.push_back({ it->second, count }); return; }
        const auto lv = L.lvli.find(key); if (lv == L.lvli.end()) return; const LootList::Lvl& x = lv->second;
        for (int c = 0; c < std::min(count, 40); ++c) {   // (flag 2, "each": every one of count draws again; the lab draws again in both cases, MODEL)
            if (x.none > 0 && R.I(1, 100) <= x.none) continue;
            if (x.flags & 4) { for (const auto& [lvN, k, n] : x.e) { (void)lvN; Resolve(k, n, level, R, out, depth + 1); } continue; }
            int top = -1; for (const auto& e : x.e) if (std::get<0>(e) <= level) top = std::max(top, std::get<0>(e));
            std::vector<const std::tuple<int, std::string, int>*> pool; for (const auto& e : x.e) if (std::get<0>(e) <= level && ((x.flags & 1) || std::get<0>(e) == top)) pool.push_back(&e);
            if (pool.empty()) continue; const auto& e = *pool[Z(R.I(0, static_cast<int>(pool.size()) - 1))]; Resolve(std::get<1>(e), std::get<2>(e), level, R, out, depth + 1); }
    }

    // ── sources ──
    enum class SrcKind : std::uint8_t { Corpse, Container, Loose, Flora };
    inline const char* SrcName(SrcKind k) { static const char* n[] = { "corpse", "container", "loose", "flora" }; return n[static_cast<std::size_t>(k)]; }
    struct Claim { double seen = -1, lastAccrue = -1, farSince = -1, playerTook = -1; bool everNear = false, rejected = false; float nearSecs = 0; };   // Logistics_internal.h Claim (player dibs, per source, party-wide)
    struct Src {
        std::uint32_t id = 0; SrcKind kind = SrcKind::Corpse; EQ::Vec3 p, on; std::int32_t tri = -1; float navDist = 0;   // p: the ref. on / tri: its place on the mesh
        std::vector<Stack> items; bool owned = false, locked = false, hostileDeath = true, coin = false; std::uint32_t body = 0;
        double born = 0;                                    // when it began to exist as loot (a body: its fight's end)
        // party-wide records native keeps per ref
        Claim claim; double transientUntil = -1, stickyUntil = -1; int strikes = 0, pathFails = 0; const char* failWhy = "";
        double barSince = -1, barAsk = -1; bool barred = false;   // TheftGate.cpp BarCorpse
        // the lab's own record (metrics)
        int dispatches = 0, arrivals = 0, takes = 0; bool playerOpen = false; std::uint8_t scanned = 0, island = 0; double capAt = -1e9;
    };

    // ── a follower's loot state ──
    enum class Phase : std::uint8_t { Walking, Holding };
    enum class LegState : std::uint8_t { None, Walking, Arrived, Blocked };   // Harbinger ch.19 (APMF_API.h): the states MFO reacts to
    struct Slot {            // LootTravel_internal.h TravelIntent
        bool active = false; Phase phase = Phase::Holding; Cat cat = Cat::Gold; int target = -1; double start = 0, deadline = 0, emptySince = -1, guardHoldSince = -1, progressAt = 0; float lx = 0, ly = 0;
        bool drift = false; LegState leg = LegState::None; double legAt = 0, mbSince = -1, sampleAt = 0; float sx = 0, sy = 0, bx = 0, by = 0, bz = 0, moved = 0; std::uint32_t blocker = 0; bool everMoved = false;
    };
    struct GuardAns { int verdict = 0; double at = -1e9, readAt = -1, postAt = -1e9, usedAt = -1e9; bool has = false, pend = false; int pv = 0; double pat = 0; };   // 0 clear, 1 seen. pend: a posted scan whose answer lands at pat (the cached one stands until then)
    struct Gate { int target = -1, fi = -1; EQ::Vec3 block, tpos; double since = 0; bool targetOnly = false; std::vector<int> told; };   // LootTravel_internal.h GateRecord
    struct Hit { EQ::Vec3 p; double at = 0; int target = -1; };
    struct FState {
        int ci = -1; Inv inv, inv0; Who who; Slot slot; double nextLogi = 0; bool thief = false;
        std::map<int, GuardAns> guard;                      // key: source index, -1 = the follower-level answer
        int deferItem = -1; std::uint32_t deferBlocker = 0; EQ::Vec3 deferPos; bool hasDefer = false; double deferAt = -1e9;   // g_actorDefer
        std::vector<Hit> run; bool stuck = false; EQ::Vec3 stuckAt;   // StuckAtOrigin.cpp: the run of blocked ends, the open episode
        bool hasLast = false; EQ::Vec3 lastBlock; int lastTarget = -1; float lastShort = 0; double lastBlockAt = -1e9;   // NoteBlockGround
        std::map<int, std::pair<int, float>> farArr;        // target -> (count, last distance)
        int idle = 0, museumTaken = 0; double lastReassess = -1e9;
        std::map<std::pair<int, int>, int> left;            // the loot oracle: (source, category) he was told to leave: not offered again
        // the lab's record
        float ldx = 0, ldy = 0; double lastLegAt = -1e9; int lastDest = -1; std::vector<int> visited; std::vector<char> near;
    };
    struct Guard { EQ::Vec3 a, b, p; float t = 0, dir = 1, speed = 90.0f; bool guard = true; };   // guard = false: a bystander (a witness, not a guard)

    // ── what an episode counts (iaus-sim-v4.md 14.3). Sums: Merge adds. ──
    struct LTally {
        long episodes = 0, lostFights = 0; double lootSec = 0, followerSec = 0;                   // seconds out of party combat, and that times the followers
        std::map<std::string, long> ev;                                           // events under the field log's own names (calib_loot.py prints the same keys)
        long stacks0 = 0, taken = 0, wanted = 0;                                  // stacks that existed, stacks taken, stacks the game's own scorer wanted at the end
        std::map<std::string, long> left;                                         // wanted and left, by reason
        std::map<std::string, std::pair<long, double>> noRoad;                    // left, unowned, worth gold, and in NOBODY's plan: by item kind (stacks, gold). Off-class gear, books, ingredients: no loot road takes them
        long leftRR = 0;                                                          // ... of those: RELEASED and REACHABLE and in range (the failure marth names)
        long goldLeft = 0, goldLeftRR = 0, goldDibsNear = 0, museumLeft = 0, museumLeftRR = 0;      // coin and museum pieces left (all, and released + reachable)
        double waitSum = 0; long waitN = 0; std::vector<float> waits, waitsDibs;   // release (the party left combat with it lying there) to taken: the free tier, and the tier the player has first dibs on
        long legs = 0, legsArrived = 0, legsAborted = 0, legsRestarted = 0, reversals = 0, revisits = 0;
        long maxSrcLegs = 0;                                                      // the most legs any one source got
        long srcTaken = 0;                                                        // sources something was taken from (trips per item = legs / srcTaken)
        double twoOnOneSec = 0; long twoOnOne = 0;                                // two followers walking to one source
        double combatLootSec = 0; long combatTakes = 0;                           // a loot leg alive, or a take, while the party fights
        long guardSeenTakes = 0, crimes = 0;                                      // a take a guard truly saw; a take of an owned thing a witness truly saw
        double awaySec = 0; float awayMax = 0;                                    // seconds a follower was past the leash from the player on a loot leg
        double lateU = 0; long lateN = 0;                                         // how far from the player a follower stood when a fight began
        long walkedPast = 0;                                                      // he came within reach of wanted, released loot and left it
        long waves = 0, stuckEpisodes = 0; double stuckOffSec = 0;                           // stuck-at-origin episodes, and the seconds his looting was off for one
        double uFights = 0, dPotJ = 0, dGoldJ = 0, dMuseumJ = 0; double goldTaken = 0;
        long roadTaken = 0, roadLeft = 0;                                         // stacks a loot road COULD take (any category, the bag as it began): taken, and left with nothing excusing it
        long dibsNear = 0;                                                        // any stack left under the player's dibs with him within 700 units of it at the end          // the episode's value (14.4)
        void Merge(const LTally& o) {
            episodes += o.episodes; lostFights += o.lostFights; lootSec += o.lootSec; followerSec += o.followerSec; for (const auto& kv : o.ev) ev[kv.first] += kv.second; stacks0 += o.stacks0; taken += o.taken; wanted += o.wanted;
            for (const auto& kv : o.left) left[kv.first] += kv.second; for (const auto& kv : o.noRoad) { noRoad[kv.first].first += kv.second.first; noRoad[kv.first].second += kv.second.second; } leftRR += o.leftRR; goldLeft += o.goldLeft; goldLeftRR += o.goldLeftRR; goldDibsNear += o.goldDibsNear; museumLeft += o.museumLeft; museumLeftRR += o.museumLeftRR;
            waitSum += o.waitSum; waitN += o.waitN; waits.insert(waits.end(), o.waits.begin(), o.waits.end()); waitsDibs.insert(waitsDibs.end(), o.waitsDibs.begin(), o.waitsDibs.end()); legs += o.legs; legsArrived += o.legsArrived; legsAborted += o.legsAborted; legsRestarted += o.legsRestarted;
            reversals += o.reversals; revisits += o.revisits; srcTaken += o.srcTaken; maxSrcLegs = std::max(maxSrcLegs, o.maxSrcLegs); twoOnOneSec += o.twoOnOneSec; twoOnOne += o.twoOnOne; combatLootSec += o.combatLootSec; combatTakes += o.combatTakes;
            guardSeenTakes += o.guardSeenTakes; crimes += o.crimes; awaySec += o.awaySec; awayMax = std::max(awayMax, o.awayMax); lateU += o.lateU; lateN += o.lateN; walkedPast += o.walkedPast;
            waves += o.waves; stuckEpisodes += o.stuckEpisodes; stuckOffSec += o.stuckOffSec; uFights += o.uFights; dPotJ += o.dPotJ; dGoldJ += o.dGoldJ; dMuseumJ += o.dMuseumJ; goldTaken += o.goldTaken; roadTaken += o.roadTaken; roadLeft += o.roadLeft; dibsNear += o.dibsNear;
        }
    };

    // ── the world of one episode ──
    struct World {
        Fight* Fp = nullptr; Rules R; const MFO::Iaus::LootTuning* T = nullptr;
        std::vector<Src> src; std::vector<FState> fs; std::vector<Guard> guards; std::vector<Gate> gates; std::vector<EQ::Vec3> activators;
        // GateReadmitSweep's last sample of the player (native g_passSample). One per world: an episode is one space. A lab world of several spaces would key it as native does (SpaceKey).
        struct PassSample { bool valid = false; EQ::Vec3 pos; double at = 0; } pass;
        std::map<int, int> museumNeed;                      // item -> how many the museum still wants (Lotd.cpp uncoveredAll)
        bool town = false, hostileDeathKnown = false, playerMenu = false; int playerOpenSrc = -1;
        bool oracleWide = false;                            // the loot oracle's world (LootLab.h): the plan is every category with a road, a consumable is offered whatever its score
        double nextPump = 0, lastCombatEnd = 0, lastGlobalReassess = -1e9; int rr = 0; bool combat = false;
        LTally t; std::FILE* trace = nullptr;
        // A take is QUEUED and lands on the next frame (TheftGate.cpp Take). Whether anybody saw it is read over that frame, with the guards where they walk to
        // meanwhile, and not only at the instant the rule itself looked (review S1: read at that instant alone the count can never differ from the rule).
        struct Landing { int fi, si; bool owned, waived; double at; bool seenG, seenW; }; std::vector<Landing> landing;
        void LandStep() { for (std::size_t i = 0; i < landing.size();) { Landing& L = landing[i]; if (!L.waived && !L.seenG && GuardSeesTruth(L.fi, L.si)) L.seenG = true; if (L.owned && !L.seenW && GuardSeesTruth(L.fi, L.si, false)) L.seenW = true;
                if (Now() >= L.at) { t.guardSeenTakes += L.seenG; t.crimes += L.seenW; if (L.seenW) Log("[crime] F%d: a witness saw the take from %u", L.fi, src[Z(L.si)].id); landing.erase(landing.begin() + static_cast<std::ptrdiff_t>(i)); } else ++i; } }
        // an option the loot oracle forces (LootLab.h): per (follower, source, category) -1 = the game decides, 0 = leave, 1 = take
        std::function<int(int fi, int si, Cat c)> force;

        Fight& F() { return *Fp; }
        const Fight& F() const { return *Fp; }
        [[nodiscard]] double Now() const { return Fp->now; }
        Body& BodyOf(int fi) { return Fp->b[Z(Fp->ctl[Z(fs[Z(fi)].ci)].body)]; }
        const Body& BodyOf(int fi) const { return Fp->b[Z(Fp->ctl[Z(fs[Z(fi)].ci)].body)]; }
        [[nodiscard]] static float D3(const EQ::Vec3& a, const EQ::Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
        [[nodiscard]] static EQ::Vec3 P(const Body& x) { return { x.c.x, x.c.y, x.c.z }; }
        [[nodiscard]] EQ::View View() const { return Fp->env->View(Fp->doors, Fp->doorStamp); }
        void Ev(const char* k) { ++t.ev[k]; }
        void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3))) { if (!trace) return; std::fprintf(trace, "%8.2f  ", Now()); va_list ap; va_start(ap, fmt); std::vfprintf(trace, fmt, ap); va_end(ap); std::fputc('\n', trace); }

        // ── sight (a guard, a witness): the place's own geometry, eye to torso (EnvQuery CastRay) ──
        [[nodiscard]] bool Sees(const EQ::Vec3& from, const EQ::Vec3& to) const {
            static thread_local EQ::Scratch sc; const EnvTuning& E = g_envT;
            return EQ::CastRay(View(), { from.x, from.y, from.z + E.eyeFrac * E.bodyH }, { to.x, to.y, to.z + E.torsoFrac * E.bodyH }, sc, g_sightT, E.targetMargin).clear;
        }
        [[nodiscard]] bool GuardSeesTruth(int fi, int si, bool guardsOnly = true) const {
            const EQ::Vec3 fp = P(BodyOf(fi));
            for (const Guard& g : guards) { if (guardsOnly && !g.guard) continue;
                if (D3(g.p, fp) <= R.guardRangeU && Sees(g.p, fp)) return true;
                if (si >= 0 && D3(g.p, src[Z(si)].p) <= R.guardRangeU && Sees(g.p, { src[Z(si)].p.x, src[Z(si)].p.y, src[Z(si)].p.z + 40.0f - g_envT.torsoFrac * g_envT.bodyH })) return true; }
            return false;
        }
        // TheftGate.cpp GuardCheck: a cached verdict is trusted guardFreshSec, an answer nobody has read is usable once up to guardUnreadMaxSec, one verdict
        // per visit (a verdict first used under guardFreshSec ago stands), a (follower, source) is asked again no faster than guardRepostSec. The answer
        // of a post lands on the next frame, so the FIRST ask is always pending. Returns 0 clear, 1 seen, 2 pending.
        // Steal.cpp Check's witness verdict cache: fresh 1.5 s, an unread answer usable up to 10 s, asked again no faster than 0.4 s (Steal.cpp:105-241). 0 unseen, 1 seen, 2 pending.
        int StealCheck(int fi, int si) {
            GuardAns& e = fs[Z(fi)].guard[100000 + si]; const double now = Now();
            if (e.pend && now >= e.pat) { e.verdict = e.pv; e.at = e.pat; e.readAt = -1; e.pend = false; e.has = true; }
            if (e.has && (now - e.at < 1.5 || (e.readAt < 0 && now - e.at < 10.0) || (e.readAt >= 0 && now - e.readAt < 1.5))) { if (e.readAt < 0) e.readAt = now; return e.verdict; }
            if (!e.pend && now - e.postAt >= 0.4) { e.pv = GuardSeesTruth(fi, si, false) ? 1 : 0; e.pat = now + static_cast<double>(TM::dt); e.pend = true; e.postAt = now; }
            return 2;
        }
        void GuardPost(GuardAns& e, int fi, int si) { e.pv = GuardSeesTruth(fi, si) ? 1 : 0; e.pat = Now() + static_cast<double>(TM::dt); e.pend = true; e.postAt = Now(); Ev("guard.post"); }
        int GuardCheck(int fi, int si, bool peek = false) {
            GuardAns& e = fs[Z(fi)].guard[si]; const double now = Now();
            if (e.pend && now >= e.pat) { e.verdict = e.pv; e.at = e.pat; e.readAt = -1; e.pend = false; e.has = true; }
            if (e.has && !peek && (now - e.at < T->guardFreshSec || (e.readAt < 0 && now - e.at < T->guardUnreadMaxSec) || (e.readAt >= 0 && now - e.readAt < T->guardFreshSec))) { if (e.readAt < 0) e.readAt = now; e.usedAt = now; return e.verdict; }
            if (e.has && peek && now - e.at < T->guardFreshSec) return e.verdict;
            if (!e.pend && now - e.postAt >= T->guardRepostSec) GuardPost(e, fi, si);
            return 2;
        }
        // TheftGate.cpp KeepWarm: the follower-level answer, used within 15 s and older than half its freshness, is asked again (the cached one stands meanwhile).
        void GuardKeepWarm(int fi) { GuardAns& e = fs[Z(fi)].guard[-1]; const double now = Now(); if (e.has && !e.pend && now - e.usedAt < R.guardWarmSec && now - e.at > 0.5 * T->guardFreshSec) GuardPost(e, fi, -1); }
        [[nodiscard]] bool CorpseWaived(const Src& s) const { return s.kind == SrcKind::Corpse && (!town || s.hostileDeath) && !s.owned; }   // TheftGate.cpp CorpseGuardWaived
        // TheftGate.cpp CorpseBarred: a corpse a guard barred is skipped by everybody for guardBarFloorSec, then while a guard still sees it (asked every 3 s).
        bool CorpseBarred(int fi, int si) {
            Src& s = src[Z(si)]; if (!s.barred) return false; const double now = Now();
            if (now - s.barSince < T->guardBarFloorSec) return true;
            if (now - s.barAsk >= R.guardReaskSec) { const int g = GuardCheck(fi, si); /* the CACHED answer, as every other ask (review): a pending one keeps the bar */ if (g != 2) s.barAsk = now; if (g == 0) { s.barred = false; Ev("corpse bar LIFTED"); Log("[theft-gate] corpse %u bar LIFTED", s.id); return false; } }
            return true;
        }

        // ── the player's dibs (LootTake.cpp TierReleased), per source, party-wide ──
        [[nodiscard]] bool PlayerFacing(const EQ::Vec3& sp) const { const Body& pl = Fp->b[0]; float dx = sp.x - pl.c.x, dy = sp.y - pl.c.y; const float l = std::hypot(dx, dy); if (l < 1.0f) return true; return (pl.hx * dx + pl.hy * dy) / l > 0.5f; }
        bool TierReleased(Cat c, int si) {
            if (FreeTier(c)) return true;
            Src& s = src[Z(si)]; Claim& cl = s.claim; const double now = Now(); const float pd = D3(P(Fp->b[0]), s.p);
            if (cl.seen < 0) cl.seen = now;
            { const float dt = cl.lastAccrue < 0 ? 1.0f : std::min(static_cast<float>(now - cl.lastAccrue), 2.0f); cl.lastAccrue = now;
              if (dt > 0.0f && pd <= R.chanceRadius) { cl.everNear = true; cl.farSince = -1; if (R.dibsNearEnough > 0.0f || PlayerFacing(s.p)) cl.nearSecs += dt; }   // (PlayerIsConsidering's x3 needs QuickLoot's crosshair, which the lab does not model: an open container menu is not it)
              else if (pd > R.departRadius && cl.farSince < 0) cl.farSince = now; }
            const bool rejected = cl.rejected || (cl.playerTook >= 0 && (now - cl.playerTook >= R.quickLootWaiver || pd > R.departRadius));
            if (c == Cat::Equipment) return rejected || now - cl.seen >= R.firstDibs;
            const bool departed = cl.everNear && cl.farSince >= 0 && now - cl.farSince >= R.departRelease;
            return rejected || departed || cl.nearSecs >= R.fairChance || (!cl.everNear && now - cl.seen >= R.abandonDelay);
        }
        // The same rule read WITHOUT touching the claim (the end-of-episode ledger must not move what it measures).
        [[nodiscard]] bool TierReleasedPeek(Cat c, const Src& s) const {
            if (FreeTier(c)) return true; const Claim& cl = s.claim; const double now = Now(); const float pd = D3(P(Fp->b[0]), s.p);
            const bool rejected = cl.rejected || (cl.playerTook >= 0 && (now - cl.playerTook >= R.quickLootWaiver || pd > R.departRadius));
            if (cl.seen < 0) return false;
            if (c == Cat::Equipment) return rejected || now - cl.seen >= R.firstDibs;
            return rejected || (cl.everNear && cl.farSince >= 0 && now - cl.farSince >= R.departRelease) || cl.nearSecs >= R.fairChance || (!cl.everNear && now - cl.seen >= R.abandonDelay);
        }

        // ── the blocklists (LootTravel_internal.h), party-wide per ref ──
        void MarkFailed(int si, const char* why) { Src& s = src[Z(si)]; s.transientUntil = Now() + static_cast<double>(R.transientSec); s.failWhy = why; ++t.ev[std::string("blocklisted: ") + why]; Log("[loot] %u blocklisted ~25s (transient skip) -- %s", s.id, why); }
        void MarkSticky(int si, const char* why) { Src& s = src[Z(si)]; s.stickyUntil = Now() + static_cast<double>(R.stickySec); s.failWhy = why; ++t.ev[std::string("blocklisted: ") + why]; Log("[loot] %u STICKY-unreachable -- %s", s.id, why); }
        void MarkStalled(int si) { Src& s = src[Z(si)]; s.pathFails = std::min(s.pathFails + 1, 8); if (++s.strikes >= static_cast<int>(R.stallStrikes)) MarkSticky(si, "STICKY (2nd stall)"); else MarkFailed(si, "unreachable (no progress)"); }
        [[nodiscard]] float GrabRadius(const Src& s) const { return std::min(R.grab + R.grabGrow * static_cast<float>(s.pathFails), R.grabMax); }
        // LootTravel_internal.h BehindBlock + GatedNow: the target itself, or (no TARGET ONLY) an item in the cone past the block, on the target's floor.
        [[nodiscard]] int GatedBy(int si) const {
            const Src& s = src[Z(si)];
            for (std::size_t g = 0; g < gates.size(); ++g) { const Gate& G = gates[g]; if (G.target == si) return static_cast<int>(g); if (G.targetOnly) continue;
                const float ix = s.p.x - G.block.x, iy = s.p.y - G.block.y, tx = G.tpos.x - G.block.x, ty = G.tpos.y - G.block.y; const float li = std::hypot(ix, iy), lt = std::hypot(tx, ty);
                if (li < R.gateMinU || lt < 1.0f || li > lt + T->gateBeyondU || std::fabs(s.p.z - G.tpos.z) > R.gateFloorU) continue;
                if ((ix * tx + iy * ty) / (li * lt) >= std::cos(T->gateConeDeg * 3.14159265f / 180.0f)) return static_cast<int>(g); }
            return -1;
        }
        [[nodiscard]] bool FailedRecently(int si) { const Src& s = src[Z(si)]; if (Now() < s.stickyUntil || Now() < s.transientUntil) return true; const int g = GatedBy(si); if (g < 0) return false;
            Gate& G = gates[Z(g)]; if (G.target != si && std::find(G.told.begin(), G.told.end(), si) == G.told.end()) { G.told.push_back(si); Ev("GATED-BEHIND"); Log("[loot] %u GATED-BEHIND the block on the way to %u -- skipped", s.id, src[Z(G.target)].id); } return true; }
        [[nodiscard]] bool FailedPeek(const Src& s, int si) const { return Now() < s.stickyUntil || Now() < s.transientUntil || GatedBy(si) >= 0; }
        void Readmit(std::size_t g, const char* why) { ++t.ev[std::string("GATED re-admitted: ") + why]; Log("[loot] GATED %u re-admitted -- %s", src[Z(gates[g].target)].id, why); gates.erase(gates.begin() + static_cast<std::ptrdiff_t>(g)); }
        // LootTravel_internal.h GateReadmitSweep, rules in native's order: the timer, somebody else near the gated item, the player moving through the block
        // point (native's own pure geometry, iaus/LootNeed.h), the recorder far from his block (off). Once per service lap and per party-combat lap: Service()
        // is served in a fight too, so it calls this first.
        void GateSweep() {
            if (R.gateOld1117 > 0.0f) return;
            const bool now1124 = R.native1124 > 0.0f;
            if (gates.empty()) { pass.valid = false; return; }
            bool havePass = false; EQ::Vec3 pa, pb;
            if (now1124 && T->gatePassU > 0.0f) {
                const EQ::Vec3 pos = P(Fp->b[0]);
                if (pass.valid) { const float dt = static_cast<float>(Now() - pass.at);
                    if (dt <= R.passMaxGapSec && MFO::Iaus::GatePassStepPlausible(D3(pos, pass.pos), dt, T->gatePassRunU, R.passSlackU)) { havePass = true; pa = pass.pos; pb = pos; } }
                pass = { true, pos, Now() }; }
            for (std::size_t g = 0; g < gates.size();) { const Gate& G = gates[g]; bool lift = false; const char* why = "";
                if (T->gateReadmitSec > 0.0f && Now() - G.since >= T->gateReadmitSec) { lift = true; why = "timer"; }
                if (!lift && T->gateNearU > 0.0f) { if (D3(P(Fp->b[0]), G.tpos) <= T->gateNearU) { lift = true; why = "the player stands at it"; }
                    for (std::size_t f = 0; f < fs.size() && !lift; ++f) if (static_cast<int>(f) != G.fi && (now1124 || fs[f].slot.active) && D3(P(BodyOf(static_cast<int>(f))), G.tpos) <= T->gateNearU) { lift = true; why = "a follower stands at it"; } }   // native: any other active party member. (The slot test was the lab's own, kept for the old arm's numbers)
                if (!lift && havePass && MFO::Iaus::GatePassCrossed({ G.block.x, G.block.y, G.block.z }, { G.tpos.x, G.tpos.y, G.tpos.z }, { pa.x, pa.y, pa.z }, { pb.x, pb.y, pb.z }, T->gatePassU, R.gateFloorU)) { lift = true; why = "the player walked through the block point"; }   // native's log words, so calib_loot.py's key is the same
                if (!lift && R.gateEventsOnly <= 0.0f && T->gateReadmitMoveU > 0.0f && D3(P(BodyOf(G.fi)), G.block) > T->gateReadmitMoveU) { lift = true; why = "its follower left the block"; }
                if (lift) Readmit(g, why); else ++g; }
        }
        // A door was unlocked or opened, or a lever pulled, by the player (Sinks.cpp): every record with its block or target within 2048 is lifted. Both rule sets.
        void DoorEvent(const EQ::Vec3& at) { for (std::size_t g = 0; g < gates.size();) if (D3(gates[g].block, at) <= R.readmitEventU || D3(gates[g].tpos, at) <= R.readmitEventU) Readmit(g, "door / lever event near it"); else ++g; }

        // ── what a category would take from a source now (the peek, LootTake.cpp HasLoot), and the take (LootHere) ──
        [[nodiscard]] MFO::Iaus::LootInv InvOf(const FState& f) const { MFO::Iaus::LootInv v = Logi::LootInvOf(f.inv, f.who); v.capacity = R.carryCap; v.lockpicks = f.inv.Count(Kind::Lockpick); v.sneaks = f.thief; return v; }
        [[nodiscard]] bool Fits(const FState& f, const ItemDef& d, int n) const { return d.kind == Kind::Gold || d.kind == Kind::Lockpick || f.inv.Weight() + d.weight * float(n) <= R.carryCap; }   // LootTake.cpp FitsCarryWeight: the whole stack. Coin and picks are not weighed
        // The stacks category c would move from these items, best first: (index, count). weigh: apply the carry check (peek and take both weigh the planned stack: N4, WeighPeek below).
        std::vector<std::pair<int, int>> Picks(const FState& f, const std::vector<Stack>& items, Cat c, bool weigh) const {
            std::vector<std::pair<int, int>> out; const auto& I = Items();
            if (!f.left.empty() && !src.empty() && &items >= &src.front().items && &items <= &src.back().items) { const int si = static_cast<int>((reinterpret_cast<const char*>(&items) - reinterpret_cast<const char*>(&src.front().items)) / static_cast<std::ptrdiff_t>(sizeof(Src))); if (f.left.count({ si, static_cast<int>(c) })) return out; }
            auto all = [&](auto pred) { for (std::size_t i = 0; i < items.size(); ++i) if (items[i].count > 0 && pred(I[Z(items[i].item)])) out.push_back({ static_cast<int>(i), items[i].count }); };
            auto ammo = [&](bool upgradeOnly) { std::vector<Stack> body; std::vector<int> at; for (std::size_t i = 0; i < items.size(); ++i) if (items[i].count > 0 && I[Z(items[i].item)].kind == Kind::Ammo) { body.push_back(items[i]); at.push_back(static_cast<int>(i)); }
                if (body.empty()) return; std::vector<char> used(body.size(), 0);
                for (const Stack& st : Logi::AmmoTakeSet(f.inv, f.who, body, upgradeOnly)) for (std::size_t k = 0; k < body.size(); ++k) if (!used[k] && body[k].item == st.item && body[k].count == st.count) { used[k] = 1; out.push_back({ at[k], st.count }); break; } };
            switch (c) {
            case Cat::Arrows: ammo(false); break;
            case Cat::Bolts: break;   // no crossbow in the lab's kits (ScoreLootKind leaves bolts)
            case Cat::Gold: all([](const ItemDef& d) { return d.kind == Kind::Gold; }); break;
            case Cat::Jewelry: all([](const ItemDef& d) { return d.kind == Kind::Jewelry; }); break;
            case Cat::SoulGems: all([](const ItemDef& d) { return d.kind == Kind::SoulGem; }); break;
            case Cat::Lockpicks: all([](const ItemDef& d) { return d.kind == Kind::Lockpick; }); break;
            case Cat::Ingredients: all([](const ItemDef& d) { return d.kind == Kind::Ingredient; }); break;
            case Cat::Valuables: all([](const ItemDef& d) { return d.kind == Kind::Gold || ValuableMisc(d); }); break;
            case Cat::Museum: if (R.lotd > 0.0f) for (std::size_t i = 0; i < items.size(); ++i) { if (items[i].count <= 0 || !I[Z(items[i].item)].museum) continue; const auto it = museumNeed.find(items[i].item); const int need = it == museumNeed.end() ? 1 : it->second; if (need > 0) out.push_back({ static_cast<int>(i), std::min(items[i].count, need) }); } break;
            case Cat::Equipment: {   // LootEquipment.cpp: ONE piece a call, weapon, then ranged, then armor; with none, the ammo upgrade road
                auto best = [&](std::initializer_list<Kind> ks) { int b = -1; for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = I[Z(items[i].item)]; bool in = false; for (Kind k : ks) in = in || d.kind == k;
                        if (items[i].count > 0 && in && Logi::IsUpgrade(f.inv, f.who, d) && (b < 0 || d.power > I[Z(items[Z(b)].item)].power)) b = static_cast<int>(i); } return b; };
                int b = best({ Kind::Weapon1H, Kind::Weapon2H }); if (b < 0) b = best({ Kind::Bow }); if (b < 0) b = best({ Kind::ArmorBody, Kind::ArmorHead });
                if (b >= 0) { if (!weigh || Fits(f, I[Z(items[Z(b)].item)], 1)) out.push_back({ b, 1 }); }   // the one best does not fit: skipped, no second best (LootEquipment.cpp; spec B11)
                else ammo(true);
                break; }
            case Cat::Potions: {   // LootNeed.cpp LootConsumablesNeed: every ALCH scored against what he carries, best first
                using namespace MFO::Iaus; const LootInv inv = InvOf(f); std::vector<std::pair<float, std::pair<int, int>>> cand;
                for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = I[Z(items[i].item)]; if (items[i].count <= 0 || !Logi::IsAlch(d.kind)) continue;
                    const LootVerdict r = ScoreConsumable(Logi::ConsOf(d), items[i].count, inv, *T); if (oracleWide) cand.push_back({ r.score, { static_cast<int>(i), items[i].count } }); else if (r.take && r.takeCount > 0) cand.push_back({ r.score, { static_cast<int>(i), r.takeCount } }); }
                std::stable_sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); for (const auto& x : cand) out.push_back(x.second);
                break; }
            default: break;
            }
            if (weigh && c != Cat::Equipment) { std::vector<std::pair<int, int>> w; for (const auto& x : out) if (Fits(f, I[Z(items[Z(x.first)].item)], x.second)) w.push_back(x); return w; }
            return out;
        }
        // The loot plan of a follower (LootNeed.cpp IausLootPlan): the kinds ScoreLootKind says TAKE, best score first, the museum row first of all
        // (a table row, free tier), the steal road last. pass 0 drops the dibs tier.
        std::vector<Cat> Plan(const FState& f, int pass) const {
            using namespace MFO::Iaus; std::vector<std::pair<float, Cat>> p; const LootInv inv = InvOf(f);
            if (oracleWide) { std::vector<Cat> all; if (R.lotd > 0.0f) all.push_back(Cat::Museum); for (std::size_t i = 0; i < kLootKinds; ++i) if (pass != 0 || FreeTier(static_cast<Cat>(i))) all.push_back(static_cast<Cat>(i)); if (f.thief) all.push_back(Cat::Steal); return all; }
            for (std::size_t i = 0; i < kLootKinds; ++i) { const LootVerdict r = ScoreLootKind(static_cast<LootKind>(i), inv, *T); if (r.take) p.push_back({ r.score, static_cast<Cat>(i) }); }
            std::stable_sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<Cat> out; if (R.lotd > 0.0f) out.push_back(Cat::Museum); for (const auto& x : p) if (pass != 0 || FreeTier(x.second)) out.push_back(x.second);
            if (f.thief) out.push_back(Cat::Steal);
            return out;
        }
        // LootNeed.cpp IausNeedStealWants: an owned loose potion, lockpick or soul gem whose need clears stealNeedMin after the risk term.
        [[nodiscard]] bool StealWants(const FState& f, const Src& s) const {
            using namespace MFO::Iaus; if (!f.thief || s.kind != SrcKind::Loose || !s.owned || s.items.empty() || s.items[0].count <= 0) return false; const ItemDef& d = Items()[Z(s.items[0].item)]; const LootInv inv = InvOf(f); LootVerdict need;
            if (Logi::IsAlch(d.kind)) need = ScoreConsumable(Logi::ConsOf(d), s.items[0].count, inv, *T); else if (d.kind == Kind::Lockpick) need = ScoreLootKind(LootKind::Lockpicks, inv, *T); else if (d.kind == Kind::SoulGem) need = ScoreLootKind(LootKind::SoulGems, inv, *T); else return false;
            return ScoreStealNeed(need, static_cast<float>(d.value), *T).take;
        }
        // N4: every category's peek weighs the planned stack like the take (one reader: Picks, weigh). A loose ref's predicate does not (its road has no carry check, spec B15). Before efd097dc: equipment alone.
        [[nodiscard]] bool WeighPeek(const Src& s, Cat c) const { return c == Cat::Equipment || (R.native1124 > 0.0f && s.kind != SrcKind::Loose && s.kind != SrcKind::Flora); }
        [[nodiscard]] bool HasLoot(const FState& f, const Src& s, Cat c) const { if (c == Cat::Steal) return StealWants(f, s); return !Picks(f, s.items, c, WeighPeek(s, c)).empty(); }

        void NoteTake(int fi, int si, const ItemDef& d, int n, bool owned, bool freeTier) {
            Src& s = src[Z(si)]; ++t.taken; ++s.takes; if (s.takes == 1) ++t.srcTaken; Ev("take"); ++t.ev[std::string("take ") + Logi::Name(d.kind)];
            const double w = Now() - std::max(s.born, lastCombatEnd); t.waitSum += w; ++t.waitN; (freeTier ? t.waits : t.waitsDibs).push_back(static_cast<float>(w));
            if (d.kind == Kind::Gold) t.goldTaken += n;
            if (combat) ++t.combatTakes;
            landing.push_back({ fi, si, owned, CorpseWaived(s), Now() + static_cast<double>(TM::dt), false, false }); { Landing& L = landing.back(); L.seenG = !L.waived && GuardSeesTruth(fi, si); L.seenW = owned && GuardSeesTruth(fi, si, false); }
            if (d.museum) ++fs[Z(fi)].museumTaken;
            Log("[take] F%d %s x%d from %s %u", fi, d.edid, n, SrcName(s.kind), s.id);
        }
        // LootTake.cpp LootHere for one category. formBusy: the form types a take of this visit already queued (the landing hold). Returns moved; landing = a take was refused by the hold.
        bool TakeCat(int fi, int si, Cat c, std::uint32_t& formBusy, bool& landing) {
            FState& f = fs[Z(fi)]; Src& s = src[Z(si)]; const auto& I = Items(); bool moved = false;
            if (force && !Picks(f, s.items, c, true).empty()) { const int o = force(fi, si, c); if (o == 0) { ++f.left[{ si, static_cast<int>(c) }]; return false; } }
            for (int guard = c == Cat::Equipment ? 10 : 1; guard > 0; --guard) {
                const auto picks = Picks(f, s.items, c, true); if (picks.empty()) break; bool any = false;
                for (const auto& [i, n] : picks) { const ItemDef& d = I[Z(s.items[Z(i)].item)]; const std::uint32_t bit = 1u << FormType(d.kind);
                    if (c == Cat::Potions && !oracleWide) { const auto r = MFO::Iaus::ScoreConsumable(Logi::ConsOf(d), s.items[Z(i)].count, InvOf(f), *T); if (!r.take || r.takeCount <= 0) continue; }   // re-judged against what he carries now
                    if ((formBusy & bit) && !(c == Cat::Potions) && !any) { landing = true; return moved; }   // another category's take of this form type is still landing
                    const int k = c == Cat::Potions ? std::min(n, s.items[Z(i)].count) : n; if (!Fits(f, d, k)) { Ev("carry: stack does not fit"); continue; }
                    const bool up = Logi::IsUpgrade(f.inv, f.who, d); f.inv.Add(s.items[Z(i)].item, k); if (up) Logi::Equip(f.inv, s.items[Z(i)].item); s.items[Z(i)].count -= k;
                    if (c == Cat::Museum) { auto [it, fresh] = museumNeed.try_emplace(s.items[Z(i)].item, 1); (void)fresh; it->second -= k; }
                    SyncKit(fi, d, k); NoteTake(fi, si, d, k, false, FreeTier(c)); moved = any = true; }
                formBusy |= [&] { std::uint32_t m = 0; for (const auto& [i, n] : picks) { (void)n; m |= 1u << FormType(I[Z(s.items[Z(i)].item)].kind); } return m; }();
                if (!any || c != Cat::Equipment) break; formBusy = 0;   // Equipment: one piece a call, the next call is the next 150 ms revisit in native; the lab lets the ten calls run (MODEL: no time cost)
            }
            return moved;
        }
        // The fight kit and the bag are one stock: a restore potion he loots is one he can drink in the next fight.
        void SyncKit(int fi, const ItemDef& d, int n) { auto& kit = Fp->ctl[Z(fs[Z(fi)].ci)].book.kit; if (d.kind == Kind::PotionH) kit.potionsHealth += n; else if (d.kind == Kind::PotionM) kit.potionsMagicka += n; else if (d.kind == Kind::PotionS) kit.potionsStamina += n; }

        // LootScan.cpp StripCorpse: the outer guard gate, then every category of his plan in score order (all tiers, each tier's dibs asked again).
        // Returns 0 nothing moved, 1 moved, 2 guard seen, 3 pending (guard or landing).
        int Strip(int fi, int si, bool& dibsLeft) {
            FState& f = fs[Z(fi)]; Src& s = src[Z(si)]; dibsLeft = false;
            if (!CorpseWaived(s)) { const int g = GuardCheck(fi, si); if (g == 1) return 2; if (g == 2) return 3; }
            bool moved = false, landing = false; std::uint32_t busy = 0;
            for (Cat c : Plan(f, 1)) { if (c == Cat::Steal) continue; if (Picks(f, s.items, c, R.native1124 > 0.0f).empty()) continue; if (!TierReleased(c, si)) { dibsLeft = true; continue; }
                moved = TakeCat(fi, si, c, busy, landing) || moved; if (landing) break; }
            f.run.clear();   // Service.cpp:1129: EVERY strip attempt resets the stuck run, whatever it took (spec B1)
            if (landing) { f.nextLogi = Now() + static_cast<double>(R.landingSec); Ev("landing hold"); return 3; }
            return moved ? 1 : 0;
        }

        // ── legs ──
        [[nodiscard]] bool TargetedByOther(int si, int self) const { for (std::size_t f = 0; f < fs.size(); ++f) if (static_cast<int>(f) != self && fs[f].slot.active && fs[f].slot.phase == Phase::Walking && fs[f].slot.target == si) return true; return false; }   // LootTravel_internal.h TargetedByOtherSlot
        [[nodiscard]] int ActiveSlots() const { int n = 0; for (const FState& f : fs) n += f.slot.active; return n; }
        [[nodiscard]] float Leash(int fi) const { const Body& x = BodyOf(fi); const float vit = 0.75f * x.c.hp / std::max(1.0f, x.c.hpMax) + 0.25f * (x.c.spMax > 0 ? x.c.sp / x.c.spMax : 1.0f); return R.leashMin + (R.leashMax - R.leashMin) * std::clamp(vit, 0.0f, 1.0f); }   // Confidence.h, out of combat: vitality x trend (trend 1: he is not losing health)
        void StartLeg(int fi, int si, Cat c, bool fresh) {
            FState& f = fs[Z(fi)]; Slot& sl = f.slot; Src& s = src[Z(si)]; const Body& x = BodyOf(fi); const double now = Now(); const float df = D3(P(x), s.p);
            const bool same = sl.active && sl.target == si;
            if (fresh) { sl = Slot{}; sl.active = true; sl.start = now; Ev("DISPATCH"); Log("[loot-road] F%d: DISPATCH road=CH19 dest=%u cat=%s", fi, s.id, CatName(c)); }
            else { Ev("RETARGET"); Log("[loot-road] F%d: RETARGET road=CH19 dest=%u%s", fi, s.id, same ? " (same ref, its last leg had ended)" : ""); }
            sl.phase = Phase::Walking; sl.cat = c; sl.target = si; sl.deadline = now + std::floor(std::clamp(df / R.deadlineSpeed + R.deadlineBase, R.deadlineMin, R.deadlineMax)); sl.progressAt = now; sl.lx = x.c.x; sl.ly = x.c.y;
            sl.emptySince = sl.guardHoldSince = -1; sl.leg = LegState::Walking; sl.legAt = now; sl.mbSince = -1; sl.sampleAt = now; sl.sx = x.c.x; sl.sy = x.c.y; sl.moved = 0; sl.everMoved = false; sl.blocker = 0;
            if (f.hasDefer && f.deferItem == si) f.hasDefer = false;
            ++t.legs; ++s.dispatches; if (s.dispatches > 1) ++t.legsRestarted;
            if (std::find(f.visited.begin(), f.visited.end(), si) != f.visited.end()) ++t.revisits;
            { const float dx = s.p.x - x.c.x, dy = s.p.y - x.c.y, l = std::hypot(dx, dy);   // back and forth: this leg heads back against his last one, both real legs, within 10 s
              if (l > 200.0f && f.lastDest >= 0 && now - f.lastLegAt < 10.0 && (dx * f.ldx + dy * f.ldy) / l < -0.5f) ++t.reversals;
              if (l > 200.0f) { f.ldx = dx / l; f.ldy = dy / l; f.lastLegAt = now; f.lastDest = si; } }
            if (df > R.grab && TargetedByOther(si, fi)) ++t.twoOnOne;   // past arm's reach, where native excludes a second walker. Inside it native has no exclusion (LootTravel_internal.h:140), so that is not counted against it
        }
        void Release(int fi, const char* why) { FState& f = fs[Z(fi)]; if (!f.slot.active) return; ++t.ev[std::string("RELEASE why=") + why]; Log("[loot-road] RELEASE road=CH19 F%d why=%s", fi, why); if (f.slot.phase == Phase::Walking && f.slot.leg == LegState::Walking) ++t.legsAborted; f.slot = Slot{}; }

        // LootTravel_internal.h FindGateObstacle: a LOCKED door or any activator within 256 of the block, on the way (not more than 64 behind, lane 128).
        [[nodiscard]] bool GateObstacle(const EQ::Vec3& S, const EQ::Vec3& Tg) const {
            const float tx = Tg.x - S.x, ty = Tg.y - S.y, tl = std::max(1.0f, std::hypot(tx, ty)); const EQ::View v = View();
            auto on = [&](const EQ::Vec3& q) { const float qx = q.x - S.x, qy = q.y - S.y; if (std::hypot(qx, qy, q.z - S.z) > R.obstacleU) return false; const float along = (qx * tx + qy * ty) / tl, perp = std::fabs(qx * ty - qy * tx) / tl; return along >= -R.obstacleBack && perp <= R.obstacleLane; };
            for (const EQ::Door& d : Fp->doors) { if (d.state != EQ::DoorState::Locked) continue; const EQ::Vec3 q = d.leaf ? EQ::Vec3{ 0.5f * (d.ax + d.bx), 0.5f * (d.ay + d.by), d.z0 } : EQ::Centroid(v, d.tri); if (on(q)) return true; }
            for (const EQ::Vec3& a : activators) if (on(a)) return true;
            return false;
        }
        // LootTravel_internal.h MarkGated
        void MarkGated(int fi, int si, const EQ::Vec3& bp, bool cone) {
            Src& s = src[Z(si)]; if (!cone && R.native1124 > 0.0f) for (const Gate& g : gates) if (g.target == si && !g.targetOnly) return;   // a repeat never downgrades a cone record (native R4)
            for (std::size_t g = 0; g < gates.size(); ++g) if (gates[g].target == si) { gates.erase(gates.begin() + static_cast<std::ptrdiff_t>(g)); break; }
            if (gates.size() >= 64) gates.erase(gates.begin());
            Gate G; G.target = si; G.fi = fi; G.block = bp; G.tpos = s.p; G.since = Now(); G.targetOnly = !cone; gates.push_back(G);
            Ev(cone ? "GATED" : "GATED (target only)"); Log("[loot] F%d target %u GATED -- Movement Blocked at (%.0f,%.0f,%.0f), %.0f u short%s", fi, s.id, bp.x, bp.y, bp.z, D3(bp, s.p), cone ? "; skipping it and the items behind the same block" : " TARGET ONLY");
        }
        // LootTravel_internal.h LiveGateAtPoint (N3): 0 no live gate's block point within gateRepeatU of bp, 1 Same (the new target is on that record's far side), 2 Jammed (it is not). Same wins.
        [[nodiscard]] int LiveGateAt(int si, const EQ::Vec3& bp) const {
            int res = 0; const EQ::Vec3& tp = src[Z(si)].p;
            for (const Gate& G : gates) { if (D3(G.block, bp) > T->gateRepeatU) continue;
                if ((G.tpos.x - G.block.x) * (tp.x - G.block.x) + (G.tpos.y - G.block.y) * (tp.y - G.block.y) > 0.0f) return 1; res = 2; }
            return res;
        }
        // StuckAtOrigin.cpp StuckAtOriginNoteBlock: true = he is stuck himself (the run reached Rules::stuckHits; the old build wanted 3 DISTINCT targets).
        bool StuckNote(int fi, int si, const EQ::Vec3& bp) {
            FState& f = fs[Z(fi)]; const double now = Now(); const bool old = R.gateOld1117 > 0.0f;
            f.run.erase(std::remove_if(f.run.begin(), f.run.end(), [&](const Hit& h) { return now - h.at > R.stuckWindowSec || D3(h.p, bp) > T->gateRepeatU; }), f.run.end());
            if (old) { for (const Hit& h : f.run) if (h.target == si) return false; }
            f.run.push_back({ bp, now, si });
            if (static_cast<int>(f.run.size()) < (old ? 3 : static_cast<int>(R.stuckHits))) return false;
            f.run.clear(); f.stuck = true; f.stuckAt = P(BodyOf(fi)); ++t.stuckEpisodes; Ev("stuck himself"); Log("[loot] F%d is stuck himself at (%.0f,%.0f) -- consecutive blocked ends", fi, bp.x, bp.y);
            return true;
        }
        [[nodiscard]] bool StuckRepeats(int fi, const EQ::Vec3& bp) const { const FState& f = fs[Z(fi)]; for (const Hit& h : f.run) if (Now() - h.at <= R.stuckWindowSec && D3(h.p, bp) <= T->gateRepeatU) return true; return false; }
        // Service.cpp:1338-1402, the reaction to a static Movement Blocked. Returns true when the excursion was ended (stuck at his own origin).
        bool StaticBlock(int fi, int si, const EQ::Vec3& bp) {
            FState& f = fs[Z(fi)]; const Src& s = src[Z(si)]; const double now = Now(); bool feed = true;
            if (R.gateOld1117 > 0.0f) { MarkGated(fi, si, bp, true); }
            else {
                const bool real = GateObstacle(bp, s.p); const float short_ = D3(bp, s.p); int ground = 0;   // NoteBlockGround: 0 first, 1 closed, 2 no ground
                if (f.hasLast && now - f.lastBlockAt <= R.stuckWindowSec) ground = f.lastTarget == si ? (short_ < f.lastShort - 25.0f ? 1 : 2) : (D3(bp, f.lastBlock) > T->gateRepeatU ? 1 : 2);
                const bool n24 = R.native1124 > 0.0f, sameItem = f.hasLast && f.lastTarget == si;
                f.hasLast = true; f.lastBlock = bp; f.lastTarget = si; f.lastShort = short_; f.lastBlockAt = now;
                if (real && n24) {
                    const int live = LiveGateAt(si, bp);
                    if (live == 1) { MarkGated(fi, si, bp, false); feed = false; }   // the same locked door with another item behind it: a gate fact about this target only
                    else if (live == 2 || StuckRepeats(fi, bp)) { Ev("blocked again at the same point (stuck evidence, not gated)"); }
                    else { MarkGated(fi, si, bp, short_ <= T->gateShortU); feed = false; } }   // a real gate is a fact about the door, not stuck evidence
                else if (real) { if (StuckRepeats(fi, bp)) { Ev("blocked again at the same point (stuck evidence, not gated)"); } else MarkGated(fi, si, bp, short_ <= T->gateShortU); }
                else if (ground == 1) { Ev("bump (ground closed)"); feed = false; f.run.clear(); }
                else if (ground == 2 && n24 && sameItem) { MarkStalled(si); f.hasLast = false; Ev("stall strike (no ground closed on the same item)"); Log("[loot] F%d target %u Movement Blocked, no door/activator, NO ground closed on the SAME item: stall strike", fi, s.id); }   // N2: the strike is the verdict (a retry from another side starts fresh), and stuck evidence still
                else if (ground == 2) { f.hasDefer = true; f.deferItem = si; f.deferBlocker = 0; f.deferPos = bp; f.deferAt = now; Ev("REORDER (no ground closed, stuck evidence)"); Log("[loot] F%d target %u Movement Blocked, no door/activator, NO ground closed: REORDER", fi, s.id); }
                else { Ev("bump (first block, stuck evidence only)"); Log("[loot] F%d target %u Movement Blocked, no door/activator on the way: a bump / repath, not gated", fi, s.id); }
            }
            if (feed && StuckNote(fi, si, bp)) { Release(fi, "stuck at his own origin"); return true; }
            return false;
        }

        // ── the scan (LootScan.cpp LootNearby) for one category. excursion: called from a slot in Holding (RunExcursionScan). Returns true = it acted. ──
        bool scanGuardPending = false;
        bool Scan(int fi, Cat c, bool excursion) {
            FState& f = fs[Z(fi)]; const Body& x = BodyOf(fi); const Body& pl = Fp->b[0]; const double now = Now(); const EQ::Vec3 fp = P(x), pp = P(pl);
            if (playerMenu) return false;                                                   // the player's container menu
            if (f.stuck) return false;                                                      // StuckAtOriginCooldown: ALL his looting, arm's reach included
            const int ga = GuardCheck(fi, -1); const bool guardSeen = ga != 0;
            if (guardSeen && town && !hostileDeathKnown) return false;
            if (ga == 2 && excursion) scanGuardPending = true;
            if (combat) return false;
            if (R.native1124 <= 0.0f) GateSweep();   // before efd097dc: only inside a scan
            const float leash = Leash(fi), walkLimit = std::min(leash, R.travelRadius); const bool beyond = D3(fp, pp) > leash * R.releaseMul;
            struct Cand { int si; int tier; float d; }; std::vector<Cand> cs; int seen = 0;
            for (std::size_t i = 0; i < src.size(); ++i) { Src& s = src[i]; const int si = static_cast<int>(i); const float df = D3(fp, s.p);
                if (df > R.lootRadius) continue;
                if (seen >= static_cast<int>(R.candCap)) { s.capAt = now; continue; }   // LootScan.cpp:203: the cap of 48 candidates is applied in ref order, BEFORE the sort (spec B6). What it hides is noted for the ledger
                const bool loose = s.kind == SrcKind::Loose || s.kind == SrcKind::Flora;
                if (s.kind == SrcKind::Flora && !(s.coin && (c == Cat::Gold || c == Cat::Valuables))) continue;   // flora other than a coin purse is never a source
                if (c == Cat::Steal ? !(loose && s.owned) : false) continue;
                if (s.kind == SrcKind::Container && town && c != Cat::Museum) { Ev("scan drop: town container"); continue; }
                if (loose && c != Cat::Steal && !HasLoot(f, s, c)) continue;              // the loose predicate of the category
                if (c != Cat::Steal && s.owned) { ++t.ev["scan drop: owned"]; continue; }
                if (s.locked) { ++t.ev["scan drop: locked"]; continue; }
                if (s.kind == SrcKind::Corpse && CorpseBarred(fi, si)) continue;
                if (D3(pp, s.p) > leash && df > R.grab) { ++t.ev["scan drop: farleash"]; continue; }
                if (!loose && !HasLoot(f, s, c)) continue;
                if (c == Cat::Steal && !StealWants(f, s)) continue;
                s.scanned = 1;
                if (!TierReleased(c, si)) { ++t.ev["scan drop: waiting"]; continue; }
                if (guardSeen && !CorpseWaived(s)) { ++t.ev[ga == 2 ? "scan drop: guard (pending)" : "scan drop: guard"]; continue; }
                ++seen;
                int tier = FailedRecently(si) ? 3 : (f.hasDefer && f.deferItem == si) ? 2 : 0;   // LootTravel_internal.h SortLootCandidates
                if (tier == 0 && f.hasDefer) { const float bx = f.deferPos.x - fp.x, by = f.deferPos.y - fp.y, ix = s.p.x - fp.x, iy = s.p.y - fp.y, lb = std::hypot(bx, by), li = std::hypot(ix, iy); if (lb > 1.0f && li > 1.0f && (bx * ix + by * iy) / (lb * li) >= 0.7071f) tier = 1; }
                cs.push_back({ si, tier, df }); }
            std::stable_sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.tier != b.tier ? a.tier < b.tier : a.d < b.d; });
            int drained = 0;
            for (const Cand& cd : cs) { Src& s = src[Z(cd.si)]; const int si = cd.si; const bool loose = s.kind == SrcKind::Loose || s.kind == SrcKind::Flora;
                if (playerOpenSrc == si) continue;                                          // PlayerIsConsidering
                const float df = cd.d, pd = D3(pp, s.p); const bool grabOk = df <= R.grab || (df <= (loose ? R.grab : GrabRadius(s)) && (Clutter(c) || pd > R.bubble));
                auto walkable = [&]() {
                    if (df > walkLimit) { ++t.ev["skip: past the walk limit"]; return false; }
                    if (FailedRecently(si)) return false;
                    if (R.native1124 > 0.0f && f.hasDefer && f.deferItem == si && f.deferAt == now) { ++t.ev["skip: deferred this tick"]; return false; }   // N2, LootScan.cpp: an item deferred THIS tick is not re-dispatched this tick
                    if (df > R.grab && TargetedByOther(si, fi)) { ++t.ev["skip: another follower walks to it"]; return false; }
                    if (!Clutter(c) && pd <= R.bubble && df > R.grab) { ++t.ev["skip: player bubble"]; return false; }
                    if (s.tri < 0 || s.navDist > R.navGate) { MarkFailed(si, "off-navmesh (no mesh near it)"); s.pathFails = std::min(s.pathFails + 1, 8); return false; }
                    if (s.island >= 2) { ++t.ev["skip: UNREACHABLE (navmesh island)"]; return false; }   // LootReach.cpp: the verdict is asked async, the FIRST dispatch goes before it is back
                    return true; };
                if (excursion) {
                    if (grabOk && !loose) { bool dl = false; const int r = Strip(fi, si, dl); if (r == 1) { s.pathFails = 0; s.transientUntil = -1; return true; } if (r == 3) { f.nextLogi = std::min(f.nextLogi, now + static_cast<double>(R.landingSec)); } continue; }
                    if (!walkable()) continue;
                    StartLeg(fi, si, c, false); NoteIsland(fi, si); return true;
                }
                if (!grabOk || loose) {
                    if (drained > 0) return true;
                    if (beyond) { ++t.ev["skip: he is beyond the leash"]; continue; }
                    if (!walkable()) continue;
                    if (ActiveSlots() >= static_cast<int>(R.maxSlots)) { ++t.ev["skip: no free slot"]; continue; }
                    StartLeg(fi, si, c, true); NoteIsland(fi, si); return true;
                }
                bool dl = false; if (Strip(fi, si, dl) == 1) ++drained;
            }
            return drained > 0;
        }
        // LootReach.cpp NavConnected: an island verdict comes back after the first dispatch. Doors count as passable.
        void NoteIsland(int fi, int si) { Src& s = src[Z(si)]; if (s.island || s.tri < 0) return; const Body& x = BodyOf(fi); EQ::Walker w = WalkerOf(x); w.unlocks = true;
            std::vector<EQ::Door> open = Fp->doors; for (EQ::Door& d : open) d.state = EQ::DoorState::Open;
            const auto r = Fp->env->FindRoute(open, 0x15A1D, x.nav.tri, s.tri, w, true); s.island = (r.any && r.reach) ? 1 : 2; }
        // LootScan.cpp RunExcursionScan / the rule walk of Service.cpp: pass 0 the free tier, pass 1 everything, the first category that acts ends the tick.
        bool ScanAll(int fi, bool excursion) { scanGuardPending = false; for (int pass = 0; pass < 2; ++pass) for (Cat c : Plan(fs[Z(fi)], pass)) if (Scan(fi, c, excursion)) return true; return false; }

        // ── a follower's loot tick (Service.cpp ServiceFollower, the loot part) ──
        void Service(int fi) {
            FState& f = fs[Z(fi)]; Slot& sl = f.slot; const double now = Now(); const Body& x = BodyOf(fi);
            if (R.native1124 > 0.0f) GateSweep();   // Service.cpp:217 (every lap, ahead of every return) and Upkeep.cpp NoteInCombat (party-combat laps)
            if (!Fight::Up(x)) { if (sl.active) Release(fi, "combat"); return; }
            // every service: the global slot sweep (Service.cpp:258-307), by whoever is served
            for (std::size_t o = 0; o < fs.size(); ++o) { Slot& so = fs[o].slot; if (!so.active) continue;
                if (combat) Release(static_cast<int>(o), "party combat");
                else if (now > so.start + std::floor(R.excursionMax)) { if (so.target >= 0 && so.phase == Phase::Walking) MarkFailed(so.target, "excursion cap: target blacklisted"); Release(static_cast<int>(o), "excursion cap"); } }
            if (f.stuck) { const EQ::Vec3 p = P(x); if (D3(p, f.stuckAt) > T->gateRepeatU) { f.stuck = false; Ev("left the stuck point"); Log("[loot] F%d left the stuck point -- loot excursions allowed again", fi); } }
            if (now < f.nextLogi) return;
            f.nextLogi = now + static_cast<double>(R.logiSec);
            if (combat) return;
            GuardKeepWarm(fi);
            if (sl.active) { Drive(fi); return; }
            f.hasDefer = false;   // Service.cpp:467,479: g_actorDefer is erased on every tick he has no slot (review S4)
            if (ScanAll(fi, false)) { f.idle = 0; return; }
            // idle (Service.cpp:2399-2430): nothing acted and no slot is active anywhere. It lifts no gate (marth 11-24)
            if (ActiveSlots() == 0 && ++f.idle >= static_cast<int>(R.idleTicks)) {
                if (R.gateOld1117 <= 0.0f && (R.native1124 <= 0.0f || R.gateIdleLift > 0.0f)) for (std::size_t g = 0; g < gates.size();) if (gates[g].fi == fi) Readmit(g, "idle (his own record lifted)"); else ++g;
                if (now - lastGlobalReassess >= R.idleReassessSec) { int n = 0; for (Src& s : src) if (s.transientUntil > now && now - (s.transientUntil - static_cast<double>(R.transientSec)) >= R.idleAgeSec) { s.transientUntil = -1; ++n; }
                    if (n) { lastGlobalReassess = now; f.idle = 0; Ev("idle: cleared blocklisted bodies"); Log("[loot] F%d idle -- cleared %d blocklisted bodies to reassess", fi, n); } } }
        }
        // The excursion driver (Service.cpp:480-1664). The tick is his: nothing falls through to the rule walk.
        void Drive(int fi) {
            FState& f = fs[Z(fi)]; Slot& sl = f.slot; const double now = Now(); const Body& x = BodyOf(fi); const Body& pl = Fp->b[0]; const EQ::Vec3 fp = P(x);
            if (D3(fp, P(pl)) > Leash(fi) * R.releaseMul) { Release(fi, "left leash"); return; }
            if (sl.phase == Phase::Walking) {
                Src& s = src[Z(sl.target)]; const int si = sl.target; const bool loose = s.kind == SrcKind::Loose || s.kind == SrcKind::Flora; const float d = D3(fp, s.p); const char* skip = nullptr;
                if (sl.cat != Cat::Steal && !HasLoot(f, s, sl.cat)) { skip = "EMPTIED before arrival"; if (sl.leg == LegState::Walking) ++t.legsAborted; }
                else {
                    if (std::hypot(x.c.x - sl.lx, x.c.y - sl.ly) > R.moveEps) { sl.lx = x.c.x; sl.ly = x.c.y; sl.progressAt = now; }
                    if (d <= 600.0f) GuardCheck(fi, si, true);   // the prefetch peek
                    const float arriveD = loose ? R.looseAcquire : std::max(R.grab, (Clutter(sl.cat) || D3(P(pl), s.p) > R.bubble) ? GrabRadius(s) : R.grab);
                    if (d <= arriveD) {   // ARRIVAL by MFO's own distance test
                        if (sl.leg == LegState::Walking) { sl.leg = LegState::Arrived; }
                        s.strikes = 0; f.farArr.erase(si); if (std::find(f.visited.begin(), f.visited.end(), si) == f.visited.end()) f.visited.push_back(si);
                        if (playerOpenSrc == si) return;   // the player is in it: hold
                        ++s.arrivals; ++t.legsArrived; Ev("leg ended ARRIVED");
                        if (loose) { ArriveLoose(fi, si); return; }
                        bool dl = false; const int r = Strip(fi, si, dl);
                        if (r == 2) { s.barred = R.corpseBar > 0.0f; if (s.barSince < 0 || now - s.barSince > T->guardBarFloorSec) s.barSince = now; s.barAsk = now; Ev("arrived: guard sees it (BARRED)"); Log("[loot] F%d: arrived at %u -- guard sees it, kept, batch continues", fi, s.id); sl.phase = Phase::Holding; }
                        else if (r == 3) { --s.arrivals; --t.legsArrived; --t.ev["leg ended ARRIVED"]; Ev("arrived: pending (guard or landing)"); return; }
                        else { if (!dl) MarkFailed(si, "visited and fully stripped (done)"); Ev(r == 1 ? (dl ? "arrived -- looted (dibs left)" : "arrived -- looted") : (dl ? "arrived -- nothing to take (dibs left)" : "arrived -- nothing to take")); Log("[loot] F%d: arrived -- %s%s (batch continues)", fi, r == 1 ? "looted" : "nothing to take", dl ? " (loot still under player's dibs -- corpse kept)" : ""); sl.phase = Phase::Holding; }
                    }
                    else if (sl.leg == LegState::Arrived) {   // FAR ARRIVAL: Harbinger's leg ended inside its radius and he has drifted off since
                        auto& fa = f.farArr[si]; if (fa.first > 0 && d > fa.second - R.farCloseU) ++fa.first; else fa.first = 1; fa.second = d; Ev("ARRIVED but drifted away (leg restarted)");
                        Log("[loot] F%d leg->%u: Harbinger ARRIVED but he is %.0f u away now (> %.0f) -- the leg ended, re-dispatching", fi, s.id, d, arriveD);
                        if (fa.first >= 2) MarkStalled(si); skip = "ARRIVED per Harbinger but drifted away"; f.run.clear();
                    }
                    else if (sl.leg == LegState::Blocked) {
                        const EQ::Vec3 bp{ sl.bx, sl.by, sl.bz }; Ev("leg ended BLOCKED"); ++t.legsAborted;
                        if (sl.blocker) { f.hasDefer = true; f.deferItem = si; f.deferBlocker = sl.blocker; f.deferPos = bp; f.deferAt = now; f.run.clear(); Ev("ACTOR-BLOCKED (REORDER)"); Log("[loot] F%d target %u ACTOR-BLOCKED: REORDER", fi, s.id); skip = "ACTOR-BLOCKED"; }
                        else { if (StaticBlock(fi, si, bp)) return; skip = "BLOCKED (static)"; }
                    }
                    else if (now - sl.progressAt > R.noProgressSec) { ++t.legsAborted; Ev("unreachable (no progress)"); if (loose) MarkSticky(si, "STICKY (loose/unacquirable)"); else MarkStalled(si); skip = "stalled"; if (s.island == 0) NoteIsland(fi, si); }
                    else if (now > sl.deadline) { ++t.legsAborted; Ev("DEADLINE EXPIRED"); MarkFailed(si, "leg deadline expired or target gone"); skip = "deadline"; }
                    else return;   // still walking
                }
                (void)skip; sl.phase = Phase::Holding; sl.leg = LegState::None;
            }
            // HOLDING (Service.cpp:1608-1663)
            if (playerMenu) return;
            if (sl.emptySince >= 0 && now - sl.emptySince < T->batchHoldMinSec) return;      // the dwell: no scan
            if (ScanAll(fi, true)) { sl.emptySince = sl.guardHoldSince = -1; return; }
            if (f.stuck || !sl.active) return;
            if (scanGuardPending) { if (sl.guardHoldSince < 0) sl.guardHoldSince = now; if (now - sl.guardHoldSince < T->guardUnreadMaxSec) return; Ev("guard answer still PENDING: batch released"); }
            else sl.guardHoldSince = -1;
            if (sl.emptySince < 0 && T->batchHoldMinSec > 0.0f) { sl.emptySince = now; return; }
            Release(fi, "batch done"); return;
        }
        // Service.cpp:937-1166, a loose ref at arrival: the bars again, then the pickup (ActivateRef). No carry check on this road (spec B15).
        bool ArriveLoose(int fi, int si) {
            FState& f = fs[Z(fi)]; Slot& sl = f.slot; Src& s = src[Z(si)]; const auto& I = Items(); f.run.clear();
            if (s.owned) {   // the steal road (Steal.cpp Check): sneaking, guard clear, no witness; run AGAIN at the write
                if (!f.thief || GuardCheck(fi, si) != 0) { if (GuardCheck(fi, si, true) == 2) return false; MarkFailed(si, "loose ref skipped at ARRIVAL -- owned"); sl.phase = Phase::Holding; return false; }
                { const int w = StealCheck(fi, si); if (w == 2) { Ev("arrived: pending (guard or landing)"); return false; }   // Steal.cpp Check: the cached witness verdict first
                  if (w == 1 || (R.stealRecheck > 0.0f && GuardSeesTruth(fi, si, false))) { Ev("steal refused: witness"); MarkFailed(si, "steal refused -- a witness"); sl.phase = Phase::Holding; return false; } }
            } else if (!CorpseWaived(s)) { const int g = GuardCheck(fi, si); if (g == 2) { Ev("arrived: pending (guard or landing)"); return false; } if (g == 1) { MarkFailed(si, "loose ref skipped at ARRIVAL -- guard"); sl.phase = Phase::Holding; return false; } }
            if (!s.items.empty() && s.items[0].count > 0) { const ItemDef& d = I[Z(s.items[0].item)]; int n = s.items[0].count;
                if (sl.cat == Cat::Museum) { auto [it, fresh] = museumNeed.try_emplace(s.items[0].item, 1); (void)fresh; n = std::min(n, std::max(1, it->second)); it->second -= n; }
                const bool up = Logi::IsUpgrade(f.inv, f.who, d); f.inv.Add(s.items[0].item, n); if (up) Logi::Equip(f.inv, s.items[0].item); s.items[0].count -= n; SyncKit(fi, d, n); NoteTake(fi, si, d, n, s.owned, FreeTier(sl.cat)); if (s.owned) Ev("steal"); }
            MarkFailed(si, "acquire readback: leg done"); sl.phase = Phase::Holding; return false;
        }
    };

}
