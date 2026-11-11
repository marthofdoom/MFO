#pragma once
// iaus_sim/Tune.h -- runtime overrides of the IAUS tuning, so a weight or a curve can be searched WITHOUT a rebuild.
// HOST-ONLY. The values found here are then written into native/iaus/Catalogue.cpp by hand, with their evidence.
//
//   --set <param>=<value>[,<param>=<value>...]
//       w.<family>.<cons>     that family's weight on a consideration (Profile::WeightOverride)
//       aw.<family>           the family's action weight
//       c.<cons>.a | .b | .floor     the consideration's curve parameters (Core.h Curve)
//       t.momentum | t.preempt | t.actFloor | t.softCd | t.tauntCd | t.powerCd
//     <family> / <cons> are INDEXES (--list-params prints them: several considerations share a display name).
//   --opt <param>[,<param>...] [--opt-passes N] [--opt-fights N]
//       coordinate search: each parameter in turn is tried at x0.5, x0.75, x1.33, x2 of its value (and +-0.15 for a
//       value near 0), the best mean utility is kept. Objective = mean U over the scenarios and the natural bracket.
#include "iaus_sim/Setup.h"

#include <cstring>

namespace Sim {

    struct TuneState {
        Tuning t = DefaultTuning();
        std::vector<Profile::WeightOverride> weights;
        std::map<std::string, float> pkg;   // st.<stance>.<field> / pk.power / pk.leash: the class package under search
    };
    inline TuneState& Tune() { static TuneState s; return s; }

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
        if (name == "t.momentum") s.t.momentum = f; else if (name == "t.preempt") s.t.preemptMargin = f; else if (name == "t.actFloor") s.t.actFloor = f; else if (name == "t.softCd") s.t.softCooldownSec = f;
        else if (name == "t.occHold") s.t.occlusionHoldSec = f; else if (name == "t.kite") s.t.kiteDistance = f; else if (name == "t.blockBase") s.t.blockBase = f; else if (name == "t.tauntCd") s.t.rowlessTauntCooldownSec = f; else if (name == "t.powerCd") s.t.rowlessPowerCooldownSec = f; else return false;
        return true;
    }
    inline double GetParam(const std::string& name, double unset = 1.0) {
        const TuneState& s = Tune();
        auto idx = [&](std::size_t from) { return static_cast<std::size_t>(std::atoi(name.c_str() + from)); };
        if (name.rfind("w.", 0) == 0) { const auto dot = name.find('.', 2); const std::size_t fi = idx(2), ci = dot == std::string::npos ? 0 : idx(dot + 1);
            for (const auto& o : s.weights) if (o.family == static_cast<Family>(fi) && o.cons == static_cast<Cons>(ci)) return o.w; return unset; }
        if (name.rfind("aw.", 0) == 0) return s.t.actionWeight[idx(3) % kFamilies];
        if (name.rfind("c.", 0) == 0) { const auto dot = name.find('.', 2); const Curve& c = s.t.curve[idx(2) % kCons]; const std::string fld = dot == std::string::npos ? "" : name.substr(dot + 1); return fld == "a" ? c.a : fld == "b" ? c.b : c.floor; }
        if (name.rfind("st.", 0) == 0 || name.rfind("pk.", 0) == 0 || name.rfind("row.", 0) == 0) { auto i = s.pkg.find(name); return i == s.pkg.end() ? unset : i->second; }
        if (name == "t.momentum") return s.t.momentum; if (name == "t.preempt") return s.t.preemptMargin; if (name == "t.actFloor") return s.t.actFloor; if (name == "t.softCd") return s.t.softCooldownSec;
        if (name == "t.occHold") return s.t.occlusionHoldSec; if (name == "t.kite") return s.t.kiteDistance; if (name == "t.blockBase") return s.t.blockBase; if (name == "t.tauntCd") return s.t.rowlessTauntCooldownSec; if (name == "t.powerCd") return s.t.rowlessPowerCooldownSec;
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
        std::printf("\nparams: w.<family>.<cons>  aw.<family>  c.<cons>.a|b|floor  t.momentum t.preempt t.actFloor t.softCd t.tauntCd t.powerCd t.occHold t.kite\n");
    }

}
