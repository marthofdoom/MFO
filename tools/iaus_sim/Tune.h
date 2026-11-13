#pragma once
// iaus_sim/Tune.h -- runtime overrides of the IAUS tuning, so a weight or a curve can be searched WITHOUT a rebuild.
// HOST-ONLY. The values found here are then written into native/iaus/Catalogue.cpp by hand, with their evidence.
//
//   --set <param>=<value>[,<param>=<value>...]
//       w.<family>.<cons>     that family's weight on a consideration (Profile::WeightOverride)
//       aw.<family>           the family's action weight
//       c.<cons>.a | .b | .floor     the consideration's curve parameters (Core.h Curve)
//       t.<name>              a Tuning scalar (kTuneScalars below: momentum, preempt, actFloor, the cooldowns and holds, the heal lines; --list-params prints them)
//     <family> / <cons> are INDEXES (--list-params prints them: several considerations share a display name).
//   --opt <param>[,<param>...] [--opt-passes N] [--opt-fights N]
//       coordinate search: each parameter in turn is tried at x0.5, x0.75, x1.33, x2 of its value (and +-0.15 for a
//       value near 0), the best mean utility is kept. Objective = mean U over the scenarios and the natural bracket.
#include "iaus_sim/Setup.h"
#include "iaus/LootNeed.h"

#include <cstring>

namespace Sim {

    struct TuneState {
        Tuning t = DefaultTuning();
        std::vector<Profile::WeightOverride> weights;
        std::map<std::string, float> pkg;   // st.<stance>.<field> / pk.power / pk.leash: the class package under search
        MFO::Iaus::LootTuning loot{};       // loot.<field>: the IAUS loot scorer's table (iaus/LootNeed.h), read by the logistics world's IAUS arm
    };
    inline TuneState& Tune() { static TuneState s; return s; }
    // Every scalar of iaus/Core.h Tuning a search may move, by name (sim batch: the edge-number search reaches the cooldowns, holds and
    // heal lines too, not only the ten the first tuning rounds used). One table, so --set, --opt and --list-params cannot disagree.
    struct TuneScalar { const char* name; float Tuning::* m; };
    inline constexpr TuneScalar kTuneScalars[] = {
        { "t.momentum", &Tuning::momentum }, { "t.postureMomentum", &Tuning::postureMomentum }, { "t.preempt", &Tuning::preemptMargin }, { "t.actFloor", &Tuning::actFloor },
        { "t.softCd", &Tuning::softCooldownSec }, { "t.occHold", &Tuning::occlusionHoldSec }, { "t.kite", &Tuning::kiteDistance }, { "t.blockBase", &Tuning::blockBase },
        { "t.tauntCd", &Tuning::rowlessTauntCooldownSec }, { "t.powerCd", &Tuning::rowlessPowerCooldownSec }, { "t.disengageCd", &Tuning::disengageCooldownSec },
        { "t.noHealDrink", &Tuning::noHealDrink }, { "t.noHealRetreat", &Tuning::noHealRetreat }, { "t.emergencyHpHigh", &Tuning::emergencyHpHigh }, { "t.emergencyHpLow", &Tuning::emergencyHpLow },
        { "t.offClassBase", &Tuning::offClassBase }, { "t.groundCd", &Tuning::groundCooldownSec }, { "t.blockCd", &Tuning::blockCooldownSec }, { "t.bashCd", &Tuning::bashCooldownSec },
        { "t.backOffCd", &Tuning::backOffCooldownSec }, { "t.stepCd", &Tuning::stepCooldownSec }, { "t.regroupCd", &Tuning::regroupCooldownSec },
    };

    // tune 2.11.1: the worth-the-gold table (iaus/ConsumableWorth.h WorthTable, carried in Tuning::worth) and the loot table
    // (iaus/LootNeed.h LootTuning), each field by its native name: worth.<field>, loot.<field>. One name = one native member, nothing else.
    struct WorthKnob { const char* name; float MFO::Iaus::WorthTable::* m; };
    inline constexpr WorthKnob kWorthKnobs[] = {
        { "worth.ratioCap", &MFO::Iaus::WorthTable::ratioCap }, { "worth.pressureFull", &MFO::Iaus::WorthTable::pressureFull }, { "worth.dangerSpan", &MFO::Iaus::WorthTable::dangerSpan },
        { "worth.hurtWeight", &MFO::Iaus::WorthTable::hurtWeight }, { "worth.magickaWinLine", &MFO::Iaus::WorthTable::magickaWinLine }, { "worth.staminaWinLine", &MFO::Iaus::WorthTable::staminaWinLine },
        { "worth.winNeed", &MFO::Iaus::WorthTable::winNeed }, { "worth.ttlFloorSec", &MFO::Iaus::WorthTable::ttlFloorSec }, { "worth.ttlSpanSec", &MFO::Iaus::WorthTable::ttlSpanSec },
        { "worth.wealthFrac", &MFO::Iaus::WorthTable::wealthFrac }, { "worth.wealthFloor", &MFO::Iaus::WorthTable::wealthFloor }, { "worth.costWeight", &MFO::Iaus::WorthTable::costWeight },
        { "worth.scarcityRate", &MFO::Iaus::WorthTable::scarcityRate }, { "worth.restoreCostScale", &MFO::Iaus::WorthTable::restoreCostScale }, { "worth.restoreUseWeight", &MFO::Iaus::WorthTable::restoreUseWeight },
        { "worth.floor", &MFO::Iaus::WorthTable::floor }, { "worth.foodMinGain", &MFO::Iaus::WorthTable::foodMinGain },
    };
    struct LootKnobF { const char* name; float MFO::Iaus::LootTuning::* m; };
    struct LootKnobI { const char* name; int MFO::Iaus::LootTuning::* m; };
    using LT = MFO::Iaus::LootTuning;
    inline constexpr LootKnobF kLootKnobsF[] = {
        { "loot.takeThreshold", &LT::takeThreshold }, { "loot.needBase", &LT::needBase }, { "loot.needSlope", &LT::needSlope }, { "loot.roomKnee", &LT::roomKnee },
        { "loot.saleGoldPerWeight", &LT::saleGoldPerWeight }, { "loot.saleWeight", &LT::saleWeight }, { "loot.gold", &LT::gold }, { "loot.equipment", &LT::equipment },
        { "loot.valuables", &LT::valuables }, { "loot.jewelry", &LT::jewelry }, { "loot.gemChargeTrigger", &LT::gemChargeTrigger }, { "loot.buffScale", &LT::buffScale },
        { "loot.foodScale", &LT::foodScale }, { "loot.poisonPowerRef", &LT::poisonPowerRef }, { "loot.poisonWeakScale", &LT::poisonWeakScale },
    };
    inline constexpr LootKnobI kLootKnobsI[] = {
        { "loot.soulGemTarget", &LT::soulGemTarget }, { "loot.healthTarget", &LT::healthTarget }, { "loot.staminaTarget", &LT::staminaTarget },
        { "loot.staminaTargetNoWeapon", &LT::staminaTargetNoWeapon }, { "loot.magickaTarget", &LT::magickaTarget }, { "loot.resistTarget", &LT::resistTarget },
        { "loot.fortifyTarget", &LT::fortifyTarget }, { "loot.regenTarget", &LT::regenTarget }, { "loot.invisTarget", &LT::invisTarget },
        { "loot.lockpickTarget", &LT::lockpickTarget }, { "loot.poisonTarget", &LT::poisonTarget },
    };
    inline void ApplyWeights(Profile& p) { for (const auto& o : Tune().weights) SetWeight(p, o.family, o.cons, o.w); }

    // The default weight of (family, cons) is not reachable from outside Catalogue.cpp; the search starts a weight at 1 unless told.
    inline bool SetParam(const std::string& name, double v) {
        TuneState& s = Tune(); const float f = static_cast<float>(v);
        auto idx = [&](std::size_t from) { return static_cast<std::size_t>(std::atoi(name.c_str() + from)); };
        if (name.rfind("w.", 0) == 0) { const auto dot = name.find('.', 2); if (dot == std::string::npos) return false; const std::size_t fi = idx(2), ci = idx(dot + 1); if (fi >= kFamilies || ci >= kCons) return false;
            for (auto& o : s.weights) if (o.family == static_cast<Family>(fi) && o.cons == static_cast<Cons>(ci)) { o.w = f; return true; }
            s.weights.push_back({ static_cast<Family>(fi), static_cast<Cons>(ci), f }); return true; }
        if (name.rfind("aw.", 0) == 0) { const std::size_t fi = idx(3); if (fi >= kFamilies) return false; s.t.actionWeight[fi] = f; return true; }
        if (name.rfind("c.", 0) == 0) { const auto dot = name.find('.', 2); if (dot == std::string::npos) return false; const std::size_t ci = idx(2); if (ci >= kCons) return false; Curve& c = s.t.curve[ci]; const std::string fld = name.substr(dot + 1);
            if (fld == "a") c.a = f; else if (fld == "b") c.b = f; else if (fld == "floor") c.floor = f; else return false; return true; }
        if (name.rfind("st.", 0) == 0 || name.rfind("pk.", 0) == 0 || name.rfind("row.", 0) == 0) { s.pkg[name] = f; return true; }
        for (const TuneScalar& k : kTuneScalars) if (name == k.name) { s.t.*(k.m) = f; return true; }
        for (const WorthKnob& k : kWorthKnobs) if (name == k.name) { s.t.worth.*(k.m) = f;
            if (k.m == &MFO::Iaus::WorthTable::floor) s.t.curve[static_cast<std::size_t>(Cons::ConsumableWorth)].zeroBelow = f;   // Catalogue.cpp builds the veto from the same member
            return true; }
        for (const LootKnobF& k : kLootKnobsF) if (name == k.name) { s.loot.*(k.m) = f; return true; }
        for (const LootKnobI& k : kLootKnobsI) if (name == k.name) { s.loot.*(k.m) = static_cast<int>(std::lround(v)); return true; }
        return false;
    }
    inline double GetParam(const std::string& name, double unset = 1.0) {
        const TuneState& s = Tune();
        auto idx = [&](std::size_t from) { return static_cast<std::size_t>(std::atoi(name.c_str() + from)); };
        if (name.rfind("w.", 0) == 0) { const auto dot = name.find('.', 2); const std::size_t fi = idx(2), ci = dot == std::string::npos ? 0 : idx(dot + 1);
            for (const auto& o : s.weights) if (o.family == static_cast<Family>(fi) && o.cons == static_cast<Cons>(ci)) return o.w; return unset; }
        if (name.rfind("aw.", 0) == 0) return s.t.actionWeight[idx(3) % kFamilies];
        if (name.rfind("c.", 0) == 0) { const auto dot = name.find('.', 2); const Curve& c = s.t.curve[idx(2) % kCons]; const std::string fld = dot == std::string::npos ? "" : name.substr(dot + 1); return fld == "a" ? c.a : fld == "b" ? c.b : c.floor; }
        if (name.rfind("st.", 0) == 0 || name.rfind("pk.", 0) == 0 || name.rfind("row.", 0) == 0) { auto i = s.pkg.find(name); return i == s.pkg.end() ? unset : i->second; }
        for (const TuneScalar& k : kTuneScalars) if (name == k.name) return s.t.*(k.m);
        for (const WorthKnob& k : kWorthKnobs) if (name == k.name) return s.t.worth.*(k.m);
        for (const LootKnobF& k : kLootKnobsF) if (name == k.name) return s.loot.*(k.m);
        for (const LootKnobI& k : kLootKnobsI) if (name == k.name) return s.loot.*(k.m);
        return unset;
    }
    // The package overrides onto a follower (st.<0 melee|1 ranged|2 cast>.<off|def|avoid|fallback|circle|powBlock|powStag|bash>, pk.power, pk.leash).
    inline void ApplyPackage(Ctl& c) {
        for (const auto& kv : Tune().pkg) { const std::string& n = kv.first; const float v = kv.second;
            if (n == "pk.power") { c.powerShare = v; continue; } if (n == "pk.leash") { c.leash = v; continue; }
            if (n.rfind("row.", 0) == 0) { const std::size_t ri = static_cast<std::size_t>(std::atoi(n.c_str() + 4)); if (ri < c.rows.size()) c.rows[ri].param = v; continue; }   // a default row's threshold
            const int s = n.size() > 3 ? n[3] - '0' : -1; if (s < 0 || s > 2 || n.size() < 6) continue; Style& y = c.pkg[s]; const std::string f = n.substr(5);
            if (f == "off") y.off = v; else if (f == "def") y.def = v; else if (f == "avoid") y.avoidThreat = v; else if (f == "fallback") y.fallback = v; else if (f == "circle") y.circle = v;
            else if (f == "powBlock") y.powBlocking = v; else if (f == "powStag") y.powStaggered = v; else if (f == "bash") y.bash = v; }
    }

    inline bool ParseSets(const char* spec) {   // "a=1,b=2" (a weight may give its start as  w.9.60=1.5)
        std::string s = spec; std::size_t p = 0;
        while (p < s.size()) { const auto q = s.find(',', p); const std::string kv = s.substr(p, q == std::string::npos ? std::string::npos : q - p); const auto eq = kv.find('=');
            if (eq == std::string::npos || !SetParam(kv.substr(0, eq), std::atof(kv.c_str() + eq + 1))) { std::fprintf(stderr, "bad --set entry '%s'\n", kv.c_str()); return false; }
            if (q == std::string::npos) break; p = q + 1; }
        return true;
    }
    inline void ListParams() {
        std::printf("families:"); for (std::size_t i = 0; i < kFamilies; ++i) std::printf(" %zu=%s", i, std::string(Name(static_cast<Family>(i))).c_str());
        std::printf("\nconsiderations:"); for (std::size_t i = 0; i < kCons; ++i) std::printf(" %zu=%s", i, std::string(Name(static_cast<Cons>(i))).c_str());
        std::printf("\nparams: w.<family>.<cons>  aw.<family>  c.<cons>.a|b|floor "); for (const TuneScalar& k : kTuneScalars) std::printf(" %s", k.name);
        for (const WorthKnob& k : kWorthKnobs) std::printf(" %s", k.name); for (const LootKnobF& k : kLootKnobsF) std::printf(" %s", k.name); for (const LootKnobI& k : kLootKnobsI) std::printf(" %s", k.name); std::printf("\n");
    }

}
