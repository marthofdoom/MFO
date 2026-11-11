// iaus_sim/sim.cpp -- OFFLINE IAUS combat simulator, v2 (a game-faithful world). HOST-ONLY, never part of the DLL.
//
// Drives the REAL pure layers (iaus/Core.h: Decide / DecidePosture / OnFired; iaus/Generate.h: Generate) inside a
// world built from the installed list's own records and game settings (tools/list_balance: extract_world.py ->
// compile_world.py -> ~/.cache/mfo-list-balance/<list>.sim.json). List data is TEST INPUT only; nothing in native/
// reads it. Models, sources and proof levels: Docs/research/2026-11/iaus-sim-v2.md.
#include "iaus_sim/Results.h"
#include "iaus_sim/Scenarios.h"
#include "iaus_sim/Tune.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

using namespace Sim;

namespace {

    bool g_objEstimates = true, g_objScrollRoad = false, g_tableMode = false;
    bool g_oracleMode = false; OracleCfg g_oc;   // --headline-oracle: the objective is played by the rollout oracle over the current IAUS
    void RunMode(Fight& F, bool timeIt = false) { if (g_oracleMode) { OraclePolicy O; O.cfg = g_oc; RunFight(F, O); } else if (g_tableMode) { TablePolicy P; RunFight(F, P); } else { IausPolicy P; P.timeIt = timeIt; RunFight(F, P); } }

    struct Agg {   // a cell's tallies, summed
        Tally s; long fights = 0, wins = 0, losses = 0, playerDeaths = 0, forced = 0, timeouts = 0;
        double durSum = 0, firstKillSum = 0; long firstKillN = 0; double uSum = 0, pMinSum = 0, mpLateSum = 0; long mpLateN = 0, fightsWithDown = 0, foes = 0;
        void Add(const Tally& t, bool forcedBracket) {
            ++fights; wins += t.won; losses += t.lost; playerDeaths += t.playerDied; forced += forcedBracket; timeouts += !t.won && !t.lost; durSum += t.dur;
            if (t.firstKill >= 0) { firstKillSum += t.firstKill; ++firstKillN; }
            fightsWithDown += t.downs > 0; foes += t.foesFaced;
            pMinSum += t.playerMinHp; if (t.mpAtLate >= 0) { mpLateSum += t.mpAtLate; ++mpLateN; }
#define S(f) s.f += t.f
            S(ticks); S(idle); S(hold); S(act); S(fired); S(switches); S(badFacts); S(invalid); S(postureSwaps);
            for (std::size_t i = 0; i < kFamilies; ++i) { s.hist[i] += t.hist[i]; s.actSec[i] += t.actSec[i]; }
            for (std::size_t i = 0; i < 10; ++i) { s.bodySec[i] += t.bodySec[i]; s.bodyIausSec[i] += t.bodyIausSec[i]; } S(upSec);
            S(lowEpisodes); S(lowCandEpisodes); S(lowHealed); S(lowCandTicks); S(lowHoldTicks); S(lowHealSecSum);
            S(noCandTicks); S(candTicks); S(idleCand); S(reachTicks); S(idleReach); S(taunts); S(tauntRedundant); S(tauntFail);
            S(dSum); S(dN); s.dMax = std::max(s.dMax, t.dMax);
            S(downs); S(partyDowns); S(dmgDone); S(dmgTaken); S(partyDmgTaken); S(playerDmgTaken); S(healDone);
            S(mpSum); S(hpSum); S(spSum); S(resN); S(mpDrySec);
            S(overkill); S(iausDmg); S(overheal); S(healTotal); S(healCasts); S(doubleHeals);
            S(foeCasts); S(foeCastsInterrupted); S(interruptsByFollowers);
            S(elemCasts); S(resistedCasts); S(weakCasts); S(weakOpp); S(immuneCasts); S(splashAllyHits); S(areaCasts);
            S(ctrlCasts); S(ctrlFails); S(ctrlOnControlled); S(hitsOnControlled); S(powOnControlled); S(powerAttacks);
            S(playerPressureSec); S(peelSum); S(peelN); S(peelMiss); S(tgtSwitches); S(tauntsOutmatched); S(tauntThenDown); S(tauntSaves);
            S(deathsWithHeal); S(retreatSec); S(kiteDmgTaken); S(wastedCasts); S(hitsWhileCtl); S(wwDeclined); S(scrollsUsed); S(extActions); S(moveOrderSec);
#undef S
            // kill order: a priority foe (healer / caster / archer) that dies before every grunt is "good"
            bool gruntDead = false; for (const std::string& k : t.killOrder) { if (k == "grunt" || k == "boss") gruntDead = true; else (gruntDead ? s.killOrderBad : s.killOrderGood)++; }
        }
    };

    std::map<std::string, double> Metrics(const Agg& a) {
        const Tally& s = a.s; std::map<std::string, double> m;
        const double t = std::max<double>(1, double(s.ticks)), f = std::max<double>(1, double(s.fired)), secs = std::max(1e-9, a.durSum), mins = secs / 60.0, n = std::max<double>(1, double(a.fights));
        auto pct = [](double x, double y) { return y > 0 ? 100.0 * x / y : -1.0; };
        m["idle%"] = 100.0 * double(s.idle) / t; m["hold%"] = 100.0 * double(s.hold) / t; m["sw/min"] = double(s.switches) / mins;
        m["heal.s"] = s.lowHealed ? s.lowHealSecSum / double(s.lowHealed) : -1; m["healed%"] = pct(double(s.lowHealed), double(s.lowCandEpisodes));
        m["noHealCand%"] = pct(double(s.lowEpisodes - s.lowCandEpisodes), double(s.lowEpisodes)); m["holdLow%"] = pct(double(s.lowHoldTicks), double(s.lowCandTicks));
        m["bad"] = double(s.badFacts); m["invalid"] = double(s.invalid); m["dec.us"] = s.dSum / std::max<double>(1, double(s.dN)); m["dec.max"] = s.dMax;
        m["win%"] = 100.0 * double(a.wins) / n; m["loss%"] = 100.0 * double(a.losses) / n; m["pdead%"] = 100.0 * double(a.playerDeaths) / n; m["timeout%"] = 100.0 * double(a.timeouts) / n;
        m["downs/f"] = double(s.downs) / n; m["nocand%"] = 100.0 * double(s.noCandTicks) / t;
        m["iC%"] = pct(double(s.idleCand), double(s.candTicks)); m["iR%"] = pct(double(s.idleReach), double(s.reachTicks));
        m["taunt/min"] = double(s.taunts) / mins; m["tR%"] = s.taunts ? 100.0 * double(s.tauntRedundant) / double(s.taunts) : 0.0;
        { double as = 0; for (std::size_t i = 0; i < kFamilies; ++i) as += s.actSec[i]; as = std::max(1e-9, as);
          for (std::size_t i = 0; i < kFamilies; ++i) if (s.hist[i]) m[std::string("a.") + std::string(Name(Family(i)))] = 100.0 * s.actSec[i] / as; }
        { const double up = std::max(1e-9, s.upSec); auto bs = [&](Act a) { return 100.0 * s.bodySec[static_cast<std::size_t>(a)] / up; }; auto bi = [&](Act a) { return 100.0 * s.bodyIausSec[static_cast<std::size_t>(a)] / up; };
          m["b.melee"] = bs(Act::Swing) + bs(Act::Power) + bs(Act::Bash); m["b.power"] = bs(Act::Power); m["b.shoot"] = bs(Act::Shoot); m["b.cast"] = bs(Act::Cast); m["b.castIaus"] = bi(Act::Cast);
          m["b.block"] = bs(Act::Block); m["b.other"] = bs(Act::Drink) + bs(Act::Swap) + bs(Act::Taunt); m["b.free"] = bs(Act::None); (void)f; }
        m["posture/min"] = double(s.postureSwaps) / mins;
        // absolute outcomes
        m["dur.s"] = a.durSum / n; m["ttfk.s"] = a.firstKillN ? a.firstKillSum / double(a.firstKillN) : -1;
        m["dps"] = s.dmgDone / secs; m["taken/s"] = s.dmgTaken / secs; m["pTaken/s"] = s.playerDmgTaken / secs;
        m["mp.avg%"] = s.resN ? 100.0 * s.mpSum / double(s.resN) : -1; m["hp.avg%"] = s.resN ? 100.0 * s.hpSum / double(s.resN) : -1; m["dry%"] = 100.0 * s.mpDrySec / secs;
        m["forced%"] = 100.0 * double(a.forced) / n;
        // intelligence
        m["overkill%"] = pct(s.overkill, s.iausDmg); m["overheal%"] = pct(s.overheal, s.healTotal); m["dblheal%"] = pct(double(s.doubleHeals), double(s.healCasts));
        m["intr%"] = pct(double(s.interruptsByFollowers), double(s.foeCasts)); m["resisted%"] = pct(double(s.resistedCasts), double(s.elemCasts));
        m["immune%"] = pct(double(s.immuneCasts), double(s.elemCasts)); m["weak%"] = pct(double(s.weakCasts), double(s.elemCasts));
        m["splash%"] = pct(double(s.splashAllyHits), double(s.areaCasts)); m["ctrlFail%"] = pct(double(s.ctrlFails), double(s.ctrlCasts)); m["ctrlDup%"] = pct(double(s.ctrlOnControlled), double(s.ctrlCasts));
        m["peel.s"] = s.peelN ? s.peelSum / double(s.peelN) : -1; m["press%"] = 100.0 * s.playerPressureSec / secs;
        m["prio%"] = pct(double(s.killOrderGood), double(s.killOrderGood + s.killOrderBad)); m["waste%"] = pct(double(s.wastedCasts), double(s.fired));
        m["tsw/min"] = double(s.tgtSwitches) / mins; m["dwh/f"] = double(s.deathsWithHeal) / n; m["tntDown%"] = pct(double(s.tauntThenDown), double(s.taunts));
        m["down%"] = 100.0 * double(a.fightsWithDown) / n; m["takenPerFoe"] = a.foes ? s.dmgTaken / double(a.foes) : -1;
        m["U"] = a.uSum / n; m["pMinHp%"] = 100.0 * a.pMinSum / n; m["mpAtLate%"] = a.mpLateN ? 100.0 * a.mpLateSum / double(a.mpLateN) : -1; m["splashHits/f"] = double(s.splashAllyHits) / n;
        m["ctlFocus%"] = pct(double(s.hitsOnControlled), double(s.hitsWhileCtl)); m["retreat%"] = 100.0 * s.retreatSec / secs;
        m["wwDecl/min"] = double(s.wwDeclined) / mins; m["ext/f"] = double(s.extActions) / n; m["scroll/f"] = double(s.scrollsUsed) / n; m["move%"] = 100.0 * s.moveOrderSec / secs;
        m["pow/min"] = double(s.powerAttacks) / mins; m["powCtl%"] = pct(double(s.powOnControlled), double(s.powerAttacks));
        return m;
    }

    // ── the named scenarios: IAUS, and (with --oracle) the expert oracle on the SAME fights ──
    int RunScenarios(const Data& D, const std::vector<SimClass>& classes, const std::string& which, std::uint64_t seed, int fights, int threads, bool oracle, const OracleCfg& oc, bool g10Sticky, int trace, Results& out, bool estimates, bool scrollRoad) {
        const std::vector<Scenario> all = AllScenarios(); std::vector<const Scenario*> pick;
        for (const Scenario& s : all) if (which == "all" || ("," + which + ",").find(std::string(",") + s.name + ",") != std::string::npos) pick.push_back(&s);
        if (pick.empty()) { std::fprintf(stderr, "unknown scenario '%s'. Known:", which.c_str()); for (const Scenario& s : all) std::fprintf(stderr, " %s", s.name); std::fprintf(stderr, "\n"); return 2; }
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        struct Res { Agg iaus, orc; std::map<std::string, Disagreement> dis; long dec = 0, disagreed = 0; double dU = 0; std::string cls; };
        std::vector<Res> res(pick.size()); std::atomic<int> next{ 0 }; std::mutex mx; const int jobs = static_cast<int>(pick.size()) * fights;
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int si = job / fights, f = job % fights;
            Rng R; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919);
            FightSpec fs = pick[static_cast<std::size_t>(si)]->make(D, classes, R); fs.seed = R.g(); fs.rank = 1 + f % 5;
            { std::scoped_lock lk(mx); res[static_cast<std::size_t>(si)].cls = fs.cls->name; }
            Fight F = BuildFight(D, fs, t1); F.g10Sticky = g10Sticky; F.estimates = estimates; F.scrollRoad = scrollRoad; Fight G = F;
            if (trace == f) { std::scoped_lock lk(mx); std::printf("\nTRACE %s fight %d (%s)\n", pick[static_cast<std::size_t>(si)]->name, f, oracle ? "oracle arm" : "IAUS arm");
                for (const Body& x : F.b) std::printf("   %-10s id %-4u L%-2d hp %.0f mp %.0f sp %.0f  AR %.0f  melee %s %.0f  ranged %.0f  spells %zu  run %.0f\n", x.c.name.c_str(), x.c.id, x.c.level, x.c.hp, x.c.mp, x.c.sp, x.spec->ar, x.spec->melee.name.c_str(), WeaponDamage(D.rules, *x.spec, x.spec->melee, false), x.spec->ranged.Has() ? WeaponDamage(D.rules, *x.spec, x.spec->ranged, false) : 0.0f, x.ctl >= 0 ? F.ctl[static_cast<std::size_t>(x.ctl)].build->spells.size() : x.spec->spells.size(), x.spec->run);
                (oracle ? G : F).trace = stdout; }
            RunMode(F, true); const double uI = Utility(F);
            double uO = 0; OraclePolicy O; std::map<std::string, Disagreement> dl;
            if (oracle) { O.cfg = oc; O.log = &dl; RunFight(G, O); uO = Utility(G); }
            std::scoped_lock lk(mx); Res& r = res[static_cast<std::size_t>(si)]; r.iaus.uSum += uI; r.iaus.Add(F.t, false);
            if (oracle) { r.orc.uSum += uO; r.orc.Add(G.t, false); r.dec += O.decisions; r.disagreed += O.disagreed; r.dU += O.dUSum;
                for (auto& kv : dl) { Disagreement& g = r.dis[kv.first]; const Disagreement& a = kv.second; g.n += a.n; g.dU += a.dU; g.iFoeHp += a.iFoeHp; g.oFoeHp += a.oFoeHp; g.iOnPlayer += a.iOnPlayer; g.oOnPlayer += a.oOnPlayer; g.iCaster += a.iCaster; g.oCaster += a.oCaster;
                    g.iResist += a.iResist; g.oResist += a.oResist; g.iRecHp += a.iRecHp; g.oRecHp += a.oRecHp; g.selfHp += a.selfHp; g.selfMp += a.selfMp; g.foes += a.foes; g.sameTarget += a.sameTarget; g.iDist += a.iDist; g.oDist += a.oDist; g.playerHp += a.playerHp; g.nearDist += a.nearDist; } } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        long bad = 0, inv = 0; double gapSum = 0;
        std::printf("\nSCENARIOS (%d fights each, ranks I..V in turn)%s\n", fights, oracle ? "  IAUS | ORACLE (rollout search)" : "");
        for (std::size_t i = 0; i < pick.size(); ++i) {
            const Scenario& sc = *pick[i]; const auto mi = Metrics(res[i].iaus); const auto mo = Metrics(res[i].orc); bad += res[i].iaus.s.badFacts; inv += res[i].iaus.s.invalid;
            { ScenarioRow sr; sr.name = sc.name; sr.cls = res[i].cls; sr.iausU = mi.at("U"); sr.metrics = mi; if (oracle) { sr.hasOracle = true; sr.oracleU = mo.at("U"); sr.gap = sr.oracleU - sr.iausU; sr.oracleMetrics = mo; } out.scen.push_back(std::move(sr)); }
            std::printf("\n%-18s %s\n", sc.name, sc.what);
            std::string keys = sc.metrics; std::size_t p = 0;
            std::printf("  %-8s", "");
            while (p <= keys.size()) { const std::size_t q = keys.find(',', p); const std::string k = keys.substr(p, q == std::string::npos ? std::string::npos : q - p); std::printf(" %12s", k.c_str()); if (q == std::string::npos) break; p = q + 1; }
            auto row = [&](const char* nm, const std::map<std::string, double>& m) { std::printf("\n  %-8s", nm); std::size_t p2 = 0;
                while (p2 <= keys.size()) { const std::size_t q = keys.find(',', p2); const std::string k = keys.substr(p2, q == std::string::npos ? std::string::npos : q - p2); auto it = m.find(k); std::printf(" %12.2f", it == m.end() ? -1.0 : it->second); if (q == std::string::npos) break; p2 = q + 1; } };
            row("IAUS", mi); if (oracle) { row("ORACLE", mo); const double gap = mo.at("U") - mi.at("U"); gapSum += gap;
                std::printf("\n  U gap (oracle - IAUS) = %+.2f   oracle decisions %ld, IAUS disagreed on %ld (%.0f%%), mean cost %.2f U", gap, res[i].dec, res[i].disagreed, res[i].dec ? 100.0 * double(res[i].disagreed) / double(res[i].dec) : 0.0, res[i].disagreed ? res[i].dU / double(res[i].disagreed) : 0.0);
                std::vector<std::pair<double, std::string>> top; for (auto& kv : res[i].dis) top.push_back({ kv.second.dU, kv.first }); std::sort(top.rbegin(), top.rend());
                for (std::size_t k = 0; k < top.size() && k < 7; ++k) { const Disagreement& g = res[i].dis[top[k].second]; const double n = double(g.n);
                    std::printf("\n    %-34s n=%-5ld sum dU=%7.1f  foeHp %.2f>%.2f onPlayer %.2f>%.2f caster %.2f>%.2f resist %.0f>%.0f recipHp %.2f>%.2f dist %.0f>%.0f  selfHp %.2f mp %.2f playerHp %.2f nearFoe %.0f foes %.1f sameTgt %.2f", top[k].second.c_str(), g.n, g.dU,
                                g.iFoeHp / n, g.oFoeHp / n, g.iOnPlayer / n, g.oOnPlayer / n, g.iCaster / n, g.oCaster / n, g.iResist / n, g.oResist / n, g.iRecHp / n, g.oRecHp / n, g.iDist / n, g.oDist / n, g.selfHp / n, g.selfMp / n, g.playerHp / n, g.nearDist / n, g.foes / n, g.sameTarget / n); }
                std::printf("\n    oracle used: %.2f extended actions per fight, %.2f scrolls per fight, %.1f%% of the time on a move order", mo.at("ext/f"), mo.at("scroll/f"), mo.at("move%")); }
            std::printf("\n");
        }
        if (oracle) std::printf("\nMEAN U GAP over %zu scenario(s): %+.2f\n", pick.size(), gapSum / double(pick.size()));
        { std::map<std::string, std::pair<double, int>> byClass; std::map<std::string, Agg> agg;
          for (std::size_t i = 0; i < pick.size(); ++i) { auto& b = byClass[res[i].cls]; if (oracle) { b.first += out.scen[i].gap; ++b.second; } Agg& a = agg[res[i].cls]; a.uSum += res[i].iaus.uSum; a.s = res[i].iaus.s; a.fights += res[i].iaus.fights; a.durSum += res[i].iaus.durSum; }
          for (auto& kv : byClass) { const SimClass* sc = ClassBySlug(classes, kv.first.c_str()); ResultRow rr; rr.cls = kv.first; rr.line = sc->line; rr.tier = sc->tier; rr.bracket = "scenarios"; rr.fights = agg[kv.first].fights; rr.mode = g_tableMode ? "table" : "iaus"; rr.package = sc->package; rr.defaults = sc->defaults;
              rr.metrics["U"] = agg[kv.first].fights ? agg[kv.first].uSum / double(agg[kv.first].fights) : 0; if (kv.second.second) { rr.hasGap = true; rr.mainGap = kv.second.first / kv.second.second; } out.rows.push_back(std::move(rr)); } }
        std::printf("\nCHECKS: BAD-FACTS ticks=%ld (must be 0)  invalid picks=%ld (must be 0)  => %s\n", bad, inv, bad == 0 && inv == 0 ? "PASS" : "FAIL");
        return bad == 0 && inv == 0 ? 0 : 1;
    }

    // Mean utility of IAUS (no oracle) over the scenarios and the natural bracket of every class: the search objective.
    std::string g_optClass;   // --opt-class <slug>: the objective is that class only (its natural / balanced / strong1 brackets and its scenarios)
    double ObjectiveClass(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, double* scenU, double* natU) {
        const std::vector<Scenario> all = AllScenarios(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        int ci = -1; for (std::size_t i = 0; i < classes.size(); ++i) if (classes[i].name == g_optClass) ci = static_cast<int>(i); if (ci < 0) return 0;
        std::vector<int> mine; { Rng R; R.g.seed(1); for (std::size_t i = 0; i < all.size(); ++i) { FightSpec fs = all[i].make(D, classes, R); if (fs.cls->name == g_optClass) mine.push_back(static_cast<int>(i)); } }
        const int br[3] = { 0, 1, 3 }; const int nb = 3 * fights, ns = static_cast<int>(mine.size()) * fights; std::atomic<int> next{ 0 }; std::vector<double> u(static_cast<std::size_t>(ns + nb), 0.0);
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= ns + nb) return; Rng R;
            if (job < ns) { const int si = mine[static_cast<std::size_t>(job / fights)], f = job % fights; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919);
                FightSpec fs = all[static_cast<std::size_t>(si)].make(D, classes, R); fs.seed = R.g(); fs.rank = 1 + f % 5; Fight F = BuildFight(D, fs, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); u[static_cast<std::size_t>(job)] = Utility(F); }
            else { const int k = job - ns, b = br[k / fights], f = k % fights; R.g.seed(seed * 1000003ULL + std::uint64_t(ci) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(f) * 32452843ULL);
                Fight F = RandomFight(D, classes[static_cast<std::size_t>(ci)], 1 + f % 5, kBrackets[b], R, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); u[static_cast<std::size_t>(job)] = Utility(F); } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        double a = 0, b = 0; for (int i = 0; i < ns; ++i) a += u[static_cast<std::size_t>(i)]; for (int i = ns; i < ns + nb; ++i) b += u[static_cast<std::size_t>(i)];
        a = ns ? a / ns : 0; b /= std::max(1, nb); if (scenU) *scenU = a; if (natU) *natU = b; return ns ? 0.35 * a + 0.65 * b : b;
    }

    double Objective(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, double* scenU = nullptr, double* natU = nullptr) {
        if (!g_optClass.empty()) return ObjectiveClass(D, classes, seed, fights, threads, scenU, natU);
        const std::vector<Scenario> all = AllScenarios(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        const int ns = static_cast<int>(all.size()) * fights, nb = static_cast<int>(classes.size()) * std::max(1, fights / 2); std::atomic<int> next{ 0 };
        std::vector<double> u(static_cast<std::size_t>(ns + nb), 0.0);
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= ns + nb) return; Rng R;
            if (job < ns) { const int si = job / fights, f = job % fights; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919);
                FightSpec fs = all[static_cast<std::size_t>(si)].make(D, classes, R); fs.seed = R.g(); fs.rank = 1 + f % 5; Fight F = BuildFight(D, fs, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); u[static_cast<std::size_t>(job)] = Utility(F); }
            else { const int k = job - ns, c = k / std::max(1, fights / 2), f = k % std::max(1, fights / 2); R.g.seed(seed * 1000003ULL + std::uint64_t(c) * 7919 + std::uint64_t(f) * 32452843ULL);
                Fight F = RandomFight(D, classes[static_cast<std::size_t>(c)], 1 + f % 5, kBrackets[0], R, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); u[static_cast<std::size_t>(job)] = Utility(F); } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        double a = 0, b = 0; for (int i = 0; i < ns; ++i) a += u[static_cast<std::size_t>(i)]; for (int i = ns; i < ns + nb; ++i) b += u[static_cast<std::size_t>(i)];
        a /= std::max(1, ns); b /= std::max(1, nb); if (scenU) *scenU = a; if (natU) *natU = b; return 0.5 * (a + b);
    }

    int Optimise(const Data& D, const std::vector<SimClass>& classes, const std::string& params, std::uint64_t seed, int fights, int threads, int passes) {
        std::vector<std::string> ps; { std::size_t p = 0; while (p <= params.size()) { const auto q = params.find(',', p); ps.push_back(params.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; } }
        for (const std::string& p : ps) if (!SetParam(p, GetParam(p))) { std::fprintf(stderr, "unknown parameter '%s' (see --list-params)\n", p.c_str()); return 2; }
        double sU = 0, nU = 0; double best = Objective(D, classes, seed, fights, threads, &sU, &nU);
        std::printf("OPTIMISE list=%s  %zu parameter(s), %d pass(es), %d fights per scenario\nstart: J=%.3f (scenarios %.2f, natural %.2f)\n", D.name.c_str(), ps.size(), passes, fights, best, sU, nU);
        for (int pass = 0; pass < passes; ++pass) for (const std::string& p : ps) {
            const double v0 = GetParam(p); double bv = v0;
            std::vector<double> tries = { v0 * 0.5, v0 * 0.75, v0 * 1.33, v0 * 2.0 }; if (std::fabs(v0) < 0.3) { tries.push_back(v0 + 0.15); tries.push_back(std::max(0.0, v0 - 0.15)); }
            if (p == "pk.leash") tries = { 0.0, 600.0, 1000.0, 1500.0, 2500.0 };
            for (double v : tries) { SetParam(p, v); const double j = Objective(D, classes, seed, fights, threads); if (j > best + 0.05) { best = j; bv = v; } }
            SetParam(p, bv); std::printf("  pass %d  %-14s %8.3f -> %8.3f   J=%.3f\n", pass + 1, p.c_str(), v0, bv, best); std::fflush(stdout);
        }
        Objective(D, classes, seed, fights, threads, &sU, &nU);
        std::printf("end:   J=%.3f (scenarios %.2f, natural %.2f)\n--set ", best, sU, nU);
        for (std::size_t i = 0; i < ps.size(); ++i) std::printf("%s%s=%.4g", i ? "," : "", ps[i].c_str(), GetParam(ps[i]));
        std::printf("\n"); return 0;
    }

    // ── default gambit rows (card C12): text form, the row library a class may use, and a hill-climb over row sets ──
    std::string RowsText(const std::vector<GRow>& rows) {
        std::string o; char b[160];
        for (const GRow& r : rows) { std::snprintf(b, sizeof b, "%s%s|%s|%g|%d|%g", o.empty() ? "" : ";", r.cond.c_str(), r.act.c_str(), r.param, r.role, r.cooldown); o += b; }
        return o;
    }
    bool ParseRows(const std::string& text, std::vector<GRow>& out) {
        out.clear(); std::size_t p = 0;
        while (p < text.size()) { const auto q = text.find(';', p); const std::string one = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
            std::vector<std::string> f; std::size_t a = 0; for (;;) { const auto bq = one.find('|', a); f.push_back(one.substr(a, bq == std::string::npos ? std::string::npos : bq - a)); if (bq == std::string::npos) break; a = bq + 1; }
            if (f.size() < 2) return false; GRow r; r.cond = f[0]; r.act = f[1]; r.param = f.size() > 2 ? static_cast<float>(std::atof(f[2].c_str())) : 0; r.role = f.size() > 3 ? std::atoi(f[3].c_str()) : 0; r.cooldown = f.size() > 4 ? static_cast<float>(std::atof(f[4].c_str())) : 0; out.push_back(r);
            if (q == std::string::npos) break; p = q + 1; }
        return true;
    }
    // Every row the board lets THIS class pick (ruling Q59: a pick that does not match the class is impossible): only vocabulary
    // opcodes, gated by the class's own gate and weapon set.
    std::vector<GRow> RowLibrary(const SimClass& c) {
        std::vector<GRow> lib; auto add = [&](const char* cond, const char* act, float p = 0, int role = 0, float cd = 0) { lib.push_back({ cond, act, p, role, cd, true }); };
        for (const char* k : { "cond.foe_any", "cond.foe_lowest_hp", "cond.foe_highest_hp", "cond.foe_is_caster", "cond.foe_is_ranged", "cond.foe_attacking_player", "cond.foe_attacking_ally", "cond.foe_attacking_me",
                               "cond.foe_fleeing", "cond.foe_stronger_than_me", "cond.foe_weaker_than_me", "cond.foe_highest_level" }) add(k, "act.attack");
        add("cond.foe_hp_pct_below", "act.attack", 0.3f);
        if (c.gate.powerAttack) for (const char* k : { "cond.foe_blocking", "cond.foe_stronger_than_me", "cond.foe_attacking_player", "cond.foe_is_caster", "cond.foe_any" }) add(k, "act.power_attack", 0, 0, 10.0f);
        if (c.gate.taunt) { add("cond.foe_attacking_ally", "act.taunt"); add("cond.foe_attacking_player", "act.taunt"); }
        add("cond.self_hp_pct_below", "act.drink_health_potion", 0.3f); add("cond.self_hp_pct_below", "act.drink_health_potion", 0.5f);
        add("cond.self_sp_pct_below", "act.drink_stamina_potion", 0.25f);
        if (c.gate.spellsOnClass) {
            add("cond.self_mp_pct_below", "act.drink_magicka_potion", 0.2f); add("cond.self_mp_pct_below", "act.drink_magicka_potion", 0.4f);
            for (const char* k : { "cond.foe_any", "cond.foe_is_caster", "cond.foe_lowest_hp", "cond.foe_attacking_player", "cond.foe_highest_hp" }) add(k, "act.cast_target", 0, kRoleAttack);
            add("cond.foe_weak_fire", "act.cast_target", 0, kRoleFire); add("cond.foe_weak_frost", "act.cast_target", 0, kRoleFrost); add("cond.foe_weak_shock", "act.cast_target", 0, kRoleShock);
            add("cond.foe_count_at_least", "act.cast_target", 3, kRoleArea);
            add("cond.self_hp_pct_below", "act.cast_self", 0.4f, kRoleHeal); add("cond.self_hp_pct_below", "act.cast_self", 0.6f, kRoleHeal);
            add("cond.ally_hp_pct_below", "act.cast_target", 0.5f, kRoleHeal); add("cond.player_hp_pct_below", "act.cast_player", 0.5f, kRoleHeal); add("cond.ally_downed", "act.cast_target", 0, kRoleHeal);
            add("cond.foe_any", "act.cast_self", 0, kRoleArmor); add("cond.foe_any", "act.cast_self", 0, kRoleSummon); add("cond.foe_count_at_least", "act.cast_self", 2, kRoleSummon);
            add("cond.foe_stronger_than_me", "act.cast_target", 0, kRoleParalyze); add("cond.foe_attacking_player", "act.cast_target", 0, kRoleParalyze); add("cond.foe_count_at_least", "act.cast_target", 3, kRoleFear);
            add("cond.foe_is_undead", "act.cast_target", 0, kRoleTurnUndead);
        }
        if (c.kit.oneHand || c.kit.twoHand || c.kit.dagger) { add("cond.foe_any", "act.equip_melee"); add("cond.foe_within_range", "act.equip_melee", 200.0f); add("cond.self_out_of_arrows", "act.equip_melee", 1.0f); }
        if (c.kit.bow || c.kit.crossbow) { add("cond.foe_any", "act.equip_ranged"); add("cond.foe_beyond_range", "act.equip_ranged", 400.0f); }
        if (c.gate.disengage) { add("cond.self_hp_pct_below", "act.flee", 0.25f); add("cond.foe_attacking_me_melee", "act.flee"); }
        return lib;
    }
    int OptimiseRows(const Data& D, std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, int cap, int maxEvals) {
        int ci = -1; for (std::size_t i = 0; i < classes.size(); ++i) if (classes[i].name == g_optClass) ci = static_cast<int>(i);
        if (ci < 0) { std::fprintf(stderr, "--opt-rows needs --opt-class <slug>\n"); return 2; }
        SimClass& c = classes[static_cast<std::size_t>(ci)]; const std::vector<GRow> lib = RowLibrary(c);
        std::vector<GRow> cur; { bool any = false; for (const GRow& r : c.rows) any |= r.applied; for (const GRow& r : c.rows) if (r.applied || !any) cur.push_back(r); }
        auto eval = [&](const std::vector<GRow>& rows) { c.rows = rows; for (GRow& r : c.rows) r.applied = true; return Objective(D, classes, seed, fights, threads); };
        const std::vector<GRow> start = cur;
        double best = eval(cur); int evals = 1; std::printf("OPT-ROWS class=%s list=%s cap=%d library=%zu rows\nstart: J=%.3f  %s\n", c.name.c_str(), D.name.c_str(), cap, lib.size(), best, RowsText(cur).c_str());
        Rng R; R.g.seed(seed * 77 + 5);
        for (bool improved = true; improved && evals < maxEvals;) {
            improved = false; std::vector<std::vector<GRow>> nb;
            for (std::size_t i = 0; i + 1 < cur.size(); ++i) { auto n = cur; std::swap(n[i], n[i + 1]); nb.push_back(n); }                          // reorder
            for (std::size_t i = 0; i < cur.size(); ++i) { if (cur.size() > 1) { auto n = cur; n.erase(n.begin() + static_cast<long>(i)); nb.push_back(n); }   // drop
                if (cur[i].param > 0 && cur[i].param <= 1.0f) for (float k : { 0.75f, 1.33f }) { auto n = cur; n[i].param = std::clamp(cur[i].param * k, 0.05f, 0.95f); nb.push_back(n); }
                else if (cur[i].param > 1.0f) for (float k : { 0.66f, 1.5f }) { auto n = cur; n[i].param = cur[i].param * k; nb.push_back(n); } }
            for (const GRow& l : lib) { bool have = false; for (const GRow& r : cur) have |= r.cond == l.cond && r.act == l.act && r.role == l.role; if (have) continue;
                if (static_cast<int>(cur.size()) < cap) for (std::size_t pos : { std::size_t{ 0 }, cur.size() / 2, cur.size() }) { auto n = cur; n.insert(n.begin() + static_cast<long>(pos), l); nb.push_back(n); }   // add
                for (std::size_t i = 0; i < cur.size(); ++i) { auto n = cur; n[i] = l; nb.push_back(n); } }                                             // replace
            std::shuffle(nb.begin(), nb.end(), R.g);
            // THE CLASS STAYS ITSELF: every kind of action today's defaults hold (act + spell role) must still be there, and the
            // power-attack rows may not multiply (each row has its own 10 s cooldown, so stacking them is a way round ruling Q7).
            auto legal = [&](const std::vector<GRow>& n) {
                for (const GRow& k : start) { bool have = false; for (const GRow& r : n) have |= r.act == k.act && r.role == k.role; if (!have) return false; }
                int pw = 0, pw0 = 0; for (const GRow& r : n) pw += r.act == "act.power_attack"; for (const GRow& r : start) pw0 += r.act == "act.power_attack";
                return pw <= std::max(1, pw0);
            };
            for (const auto& n : nb) { if (evals >= maxEvals) break; if (!legal(n)) continue; const double j = eval(n); ++evals; if (j > best + 0.3) { best = j; cur = n; improved = true; std::printf("  %4d evals  J=%.3f  %s\n", evals, best, RowsText(cur).c_str()); std::fflush(stdout); break; } }
        }
        c.rows = cur; std::printf("end:   J=%.3f after %d evals\n--rows %s\n", best, evals, RowsText(cur).c_str()); return 0;
    }

}

int main(int argc, char** argv) {
    const char* listPath = nullptr; const char* cmp = nullptr; const char* outBase = nullptr; std::uint64_t seed = 1; int fights = 8, threads = 8; bool tauntVisible = true, engineCasts = true, estimates = true, scrollRoad = false, g10Sticky = false, legacy = false, mech = false, printClasses = false, oracle = false; std::string only, scenario; OracleCfg oc; int trace = -1; std::string jsonPath, label, sets, opt, rowsText, printRows; bool noJson = false, listParams = false, optRows = false; int optPasses = 2, rowCap = 5, rowEvals = 500, headlineOracle = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list") && i + 1 < argc) listPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fights") && i + 1 < argc) fights = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) cmp = argv[++i];
        else if (!std::strcmp(argv[i], "--baseline") && i + 1 < argc) outBase = argv[++i];
        else if (!std::strcmp(argv[i], "--taunt-visible")) tauntVisible = true;
        else if (!std::strcmp(argv[i], "--taunt-hidden")) tauntVisible = false;
        else if (!std::strcmp(argv[i], "--no-engine-casts")) engineCasts = false;
        else if (!std::strcmp(argv[i], "--g10-sticky")) g10Sticky = true;
        else if (!std::strcmp(argv[i], "--no-estimates")) estimates = false;
        else if (!std::strcmp(argv[i], "--scroll-road")) scrollRoad = true;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) g_tableMode = !std::strcmp(argv[++i], "table");
        else if (!std::strcmp(argv[i], "--classes") && i + 1 < argc) { only = argv[++i]; if (only == "legacy") { legacy = true; only.clear(); } else if (only == "real") only.clear(); }
        else if (!std::strcmp(argv[i], "--profiles") && i + 1 < argc) mech = !std::strcmp(argv[++i], "mech");
        else if (!std::strcmp(argv[i], "--print-classes")) printClasses = true;
        else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--oracle")) oracle = true;
        else if (!std::strcmp(argv[i], "--no-ext")) oc.ext = false;
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
        else if (!std::strcmp(argv[i], "--no-json")) noJson = true;
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) { if (!sets.empty()) sets += ","; sets += argv[++i]; }
        else if (!std::strcmp(argv[i], "--opt") && i + 1 < argc) opt = argv[++i];
        else if (!std::strcmp(argv[i], "--opt-passes") && i + 1 < argc) optPasses = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--opt-class") && i + 1 < argc) g_optClass = argv[++i];
        else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) rowsText = argv[++i];          // <class>=<cond>|<act>|<param>|<role>|<cooldown>;...
        else if (!std::strcmp(argv[i], "--print-rows") && i + 1 < argc) printRows = argv[++i];
        else if (!std::strcmp(argv[i], "--opt-rows")) optRows = true;
        else if (!std::strcmp(argv[i], "--headline-oracle") && i + 1 < argc) headlineOracle = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--row-cap") && i + 1 < argc) rowCap = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--row-evals") && i + 1 < argc) rowEvals = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list-params")) listParams = true;
        else if (!std::strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--oracle-k") && i + 1 < argc) oc.K = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--oracle-h") && i + 1 < argc) oc.H = std::atof(argv[++i]);
        else { std::fprintf(stderr, "usage: run.sh [--list sim.json] [--seed N] [--fights N] [--threads N] [--compare baseline] [--baseline out] [--taunt-visible] [--no-engine-casts] [--g10-sticky] [--classes legacy|real|slug,slug] [--profiles mech] [--print-classes] [--scenario all|name,name [--oracle [--oracle-k N] [--oracle-h S] [--no-ext]] [--trace N]] [--json path|--no-json] [--label text]\n"); return 2; } }
    if (listParams) { ListParams(); return 0; }
    ActiveTuning() = &Tune().t; ProfileHook() = &ApplyWeights; PackageHook() = &ApplyPackage;
    if (!sets.empty() && !ParseSets(sets.c_str())) return 2;
    const Data D = listPath ? LoadData(listPath) : BuiltinData();
    std::vector<SimClass> allClasses = legacy ? LegacyClasses() : RealClasses(mech);
    if (!rowsText.empty()) { const auto eq = rowsText.find('='); bool ok = eq != std::string::npos; std::vector<GRow> rows;
        if (ok) ok = ParseRows(rowsText.substr(eq + 1), rows); bool found = false;
        if (ok) for (SimClass& c : allClasses) if (c.name == rowsText.substr(0, eq)) { c.rows = rows; c.defaults = "lab"; found = true; }
        if (!ok || !found) { std::fprintf(stderr, "bad --rows (use <class>=<cond>|<act>|<param>|<role>|<cooldown>;...)\n"); return 2; } }
    if (!printRows.empty()) { for (const SimClass& c : allClasses) if (c.name == printRows) { std::vector<GRow> cur; bool any = false; for (const GRow& r : c.rows) any |= r.applied; for (const GRow& r : c.rows) if (r.applied || !any) cur.push_back(r); std::printf("%s\n", RowsText(cur).c_str()); } return 0; }
    const std::vector<SimClass> t1(allClasses.begin(), allClasses.begin() + 3);
    if (printClasses) { PrintClassTable(allClasses); return 0; }
    g_objEstimates = estimates; g_objScrollRoad = scrollRoad;
    if (optRows) return OptimiseRows(D, allClasses, seed, fights, threads, rowCap, rowEvals);
    if (!opt.empty()) return Optimise(D, allClasses, opt, seed, fights, threads, optPasses);
    Results R; R.list = D.name; R.seed = seed; R.fights = fights; R.mode = g_tableMode ? "table" : "iaus";
    g_objEstimates = estimates; g_objScrollRoad = scrollRoad;
    if (!noJson && opt.empty() && headlineOracle > 0) {   // the oracle on the first fights of the same seeds
        g_oracleMode = true; g_oc = oc; R.oracleJ = Objective(D, allClasses, seed, headlineOracle, threads, &R.oracleJScen, &R.oracleJNat); g_oracleMode = false; R.hasOracleJ = true; R.oracleFights = headlineOracle;
        R.oracleOver = std::string("rollout oracle (K=") + std::to_string(oc.K) + ", " + std::to_string(int(oc.H)) + " s, " + (oc.ext ? "extended" : "catalogue") + " actions) planning over " + (g_tableMode ? "IAUS (table mode is not planned over)" : estimates ? "IAUS with estimates" : "IAUS as on main") + (sets.empty() ? "" : " + --set");
        std::printf("HEADLINE  ORACLE J=%.2f (scenarios %.2f, natural %.2f)  %d fights per scenario, %d per class natural  [%s]\n", R.oracleJ, R.oracleJScen, R.oracleJNat, headlineOracle, std::max(1, headlineOracle / 2), R.oracleOver.c_str()); }
    if (!noJson && opt.empty()) { R.jVal = Objective(D, allClasses, seed, 24, threads, &R.jScen, &R.jNat); R.hasJ = true; std::printf("HEADLINE  J=%.2f (scenarios %.2f, natural %.2f)  mode=%s\n", R.jVal, R.jScen, R.jNat, R.mode.c_str()); } R.label = sets.empty() ? label : label + " [--set " + sets + "]";
    if (!scenario.empty()) {
        std::printf("IAUS SIM v2  list=%s  seed=%llu  scenario=%s  oracle=%d (K=%d H=%.0fs)\n", D.name.c_str(), (unsigned long long)seed, scenario.c_str(), int(oracle), oc.K, oc.H);
        if (oracle) { R.oracleKind = oc.ext ? "rollout over IAUS, extended action set" : "rollout over IAUS, catalogue actions"; R.oracleK = oc.K; R.oracleH = oc.H; }
        const int rc = RunScenarios(D, allClasses, scenario, seed, fights, trace >= 0 ? 1 : threads, oracle, oc, g10Sticky, trace, R, estimates, scrollRoad); if (!noJson) R.Write(jsonPath); return rc; }
    std::vector<SimClass> classes;   // --classes a,b,c narrows the cells (slugs)
    for (const SimClass& c : allClasses) if (only.empty() || ("," + only + ",").find("," + c.name + ",") != std::string::npos) classes.push_back(c);
    if (classes.empty()) { std::fprintf(stderr, "no class matches --classes %s\n", only.c_str()); return 2; }
    std::printf("IAUS SIM v2  list=%s  seed=%llu  fights/cell=%d (x5 rapport ranks)  taunted-visible=%d  engine-casts=%d  classes=%s  profiles=%s  mode=%s\n", D.name.c_str(), (unsigned long long)seed, fights, int(tauntVisible), int(engineCasts), legacy ? "legacy" : "real", mech ? "mech" : "default", g_tableMode ? "table" : "iaus");
    std::printf("world: specs=%zu encounters=%zu foeSpells=%zu  spell pool: raw=%d taught/kept=%d  natures: offense=%d control=%d heal=%d buff=%d summon=%d ward=%d  dropped: placement=%d notCastable=%d reanimate=%d unknown(script)=%d\n",
                D.specs.size(), D.encounters.size(), D.foeSpells.size(), D.rawSpells, D.taughtSpells, D.natures[1], D.natures[2], D.natures[3], D.natures[4], D.natures[5], D.natures[6], D.dropPlace, D.dropNotCast, D.dropReanim, D.dropUnknown);
    const Rules& r = D.rules;
    std::printf("rules: dmgSkill %.2f..%.2f  armor %.3f/pt max %.0f%% +%.1f%%/piece  armorSkill %.2f..%.2f  power x%.2f  regen(combat) mp %.2f sp %.2f  cost(base %.4f mult %.2f scale %.2f)  hp/level %.1f  toPC x%.2f byPC x%.2f\n",
                r.dmgSkillMin, r.dmgSkillMax, r.armorScale, r.armorMax, r.armorBasePiece * 100, r.armorSkillLo, r.armorSkillHi, 1 + r.powBonus, r.combatMp, r.combatSp, r.costBase, r.costMult, r.costScale, r.hpPerLevel, r.diffToPC, r.diffByPC);

    const int nc = static_cast<int>(classes.size());
    std::vector<Agg> cell(static_cast<std::size_t>(nc * kNB)); std::vector<Agg> rankCell(static_cast<std::size_t>(nc * 5));
    std::atomic<int> next{ 0 }; std::mutex mx;
    auto work = [&]() {
        for (;;) {
            const int job = next.fetch_add(1); if (job >= nc * kNB) return;
            const int c = job / kNB, b = job % kNB; Agg A; Agg RA[5];
            for (int rk = 1; rk <= 5; ++rk) for (int f = 0; f < fights; ++f) {
                Rng R; R.g.seed(seed * 1000003ULL + std::uint64_t(c) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(rk) * 15485863ULL + std::uint64_t(f) * 32452843ULL);
                bool forced = false; Fight F = RandomFight(D, classes[static_cast<std::size_t>(c)], rk, kBrackets[b], R, t1, &forced);
                F.tauntVisible = tauntVisible; F.engineCasts = engineCasts; F.g10Sticky = g10Sticky; F.estimates = estimates; F.scrollRoad = scrollRoad;
                RunMode(F, true);
                A.uSum += Utility(F); A.Add(F.t, forced); RA[rk - 1].Add(F.t, forced);
            }
            std::scoped_lock lk(mx); cell[static_cast<std::size_t>(job)] = A;
            for (int rk = 0; rk < 5; ++rk) { Agg& d = rankCell[static_cast<std::size_t>(c * 5 + rk)]; d.s.ticks += RA[rk].s.ticks; d.s.idle += RA[rk].s.idle; d.s.hold += RA[rk].s.hold; d.s.switches += RA[rk].s.switches; d.durSum += RA[rk].durSum; }
        }
    };
    { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }

    std::map<std::string, std::map<std::string, double>> all; long totalBad = 0, totalInvalid = 0;
    for (int c = 0; c < nc; ++c) for (int b = 0; b < kNB; ++b) { const Agg& A = cell[static_cast<std::size_t>(c * kNB + b)];
        all[classes[static_cast<std::size_t>(c)].name + "|" + kBrackets[b].name] = Metrics(A); totalBad += A.s.badFacts; totalInvalid += A.s.invalid;
        { const SimClass& sc = classes[static_cast<std::size_t>(c)]; ResultRow rr; rr.cls = sc.name; rr.line = sc.line; rr.tier = sc.tier; rr.planned = sc.planned; rr.bracket = kBrackets[b].name; rr.fights = A.fights; rr.metrics = Metrics(A); rr.mode = g_tableMode ? "table" : "iaus"; rr.package = sc.package; rr.defaults = sc.defaults; R.rows.push_back(std::move(rr)); } }
    if (!noJson) R.Write(jsonPath);
    auto G = [](const std::map<std::string, double>& m, const char* k) { auto i = m.find(k); return i == m.end() ? 0.0 : i->second; };
    std::printf("\nDECISIONS\n%-20s %5s %5s %5s %6s %6s %5s %5s %5s %4s %4s %5s %5s %5s  decisions (%% of deciding time)\n", "class|bracket", "idle", "hold", "sw/m", "heal.s", "heal%", "noHC%", "holdL", "iC%", "iR%", "inv", "dec.us", "tnt/m", "tR%");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %5.1f %5.1f %5.1f %6.2f %6.1f %5.0f %5.1f %5.1f %4.1f %4.0f %5.1f %5.1f %5.0f  ", kv.first.c_str(), G(m, "idle%"), G(m, "hold%"), G(m, "sw/min"), G(m, "heal.s"), G(m, "healed%"), G(m, "noHealCand%"), G(m, "holdLow%"), G(m, "iC%"), G(m, "iR%"), G(m, "invalid"), G(m, "dec.us"), G(m, "taunt/min"), G(m, "tR%"));
        for (auto& a : m) if (a.first.rfind("a.", 0) == 0 && a.second >= 1.0) std::printf("%s=%.0f ", a.first.c_str() + 2, a.second);
        std::printf("\n"); }
    std::printf("\nOUTCOMES (absolute)\n%-20s %5s %5s %5s %5s %6s %6s %6s %6s %6s %6s %5s %5s %5s\n", "class|bracket", "win%", "loss%", "pdead", "dn/f", "dur.s", "ttfk", "dps", "tkn/s", "pTkn/s", "mp%", "dry%", "tmo%", "frc%");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %5.0f %5.0f %5.0f %5.2f %6.1f %6.1f %6.1f %6.1f %6.1f %6.0f %5.1f %5.0f %5.0f\n", kv.first.c_str(), G(m, "win%"), G(m, "loss%"), G(m, "pdead%"), G(m, "downs/f"), G(m, "dur.s"), G(m, "ttfk.s"), G(m, "dps"), G(m, "taken/s"), G(m, "pTaken/s"), G(m, "mp.avg%"), G(m, "dry%"), G(m, "timeout%"), G(m, "forced%")); }
    std::printf("\nBODY (what he does, %% of the time he is up): the behaviour signature\n%-20s %6s %6s %6s %6s %6s %6s %6s %6s\n", "class|bracket", "melee", "power", "shoot", "cast", "castI", "block", "other", "free");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f\n", kv.first.c_str(), G(m, "b.melee"), G(m, "b.power"), G(m, "b.shoot"), G(m, "b.cast"), G(m, "b.castIaus"), G(m, "b.block"), G(m, "b.other"), G(m, "b.free")); }
    std::printf("\nINTELLIGENCE (-1 = no sample)\n%-20s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "class|bracket", "ovkil%", "ovhl%", "dblh%", "intr%", "resis%", "immun%", "weak%", "splsh%", "ctlF%", "peel.s", "press%", "prio%", "waste%", "tsw/m", "dwh/f", "wwD/m");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.2f %6.1f %6.1f %6.1f %6.1f %6.2f %6.2f\n", kv.first.c_str(), G(m, "overkill%"), G(m, "overheal%"), G(m, "dblheal%"), G(m, "intr%"), G(m, "resisted%"), G(m, "immune%"), G(m, "weak%"), G(m, "splash%"), G(m, "ctrlFail%"), G(m, "peel.s"), G(m, "press%"), G(m, "prio%"), G(m, "waste%"), G(m, "tsw/min"), G(m, "dwh/f"), G(m, "wwDecl/min")); }
    std::printf("\nIdle%% by rapport rank (all brackets):\n");
    for (int c = 0; c < nc; ++c) for (int rk = 0; rk < 5; ++rk) { const Agg& A = rankCell[static_cast<std::size_t>(c * 5 + rk)]; const double t = std::max<double>(1, double(A.s.ticks));
        std::printf("  %-12s rank%d  idle %.1f%%  hold %.1f%%  sw/min %.1f\n", classes[static_cast<std::size_t>(c)].name.c_str(), rk + 1, 100.0 * double(A.s.idle) / t, 100.0 * double(A.s.hold) / t, double(A.s.switches) / std::max(1e-9, A.durSum / 60.0)); }
    std::printf("\nCHECKS: BAD-FACTS ticks=%ld (must be 0)  invalid picks=%ld (must be 0)  => %s\n", totalBad, totalInvalid, totalBad == 0 && totalInvalid == 0 ? "PASS" : "FAIL");
    if (outBase) { std::ofstream o(outBase); for (auto& kv : all) for (auto& m : kv.second) if (m.first.find("dec.") == std::string::npos) o << kv.first << "|" << m.first << "=" << m.second << "\n"; std::printf("baseline written: %s\n", outBase); }
    if (cmp) {
        std::ifstream f(cmp); std::string line; int diffs = 0; std::printf("\nCOMPARE vs %s (|delta| > max(0.5, 5%%)):\n", cmp);
        while (std::getline(f, line)) { const auto eq = line.rfind('='); if (eq == std::string::npos) continue; const std::string k = line.substr(0, eq); const double old = std::atof(line.c_str() + eq + 1);
            const auto p = k.rfind('|'); const auto ci = all.find(k.substr(0, p)); double nv = 0; if (ci != all.end()) { auto mi = ci->second.find(k.substr(p + 1)); if (mi != ci->second.end()) nv = mi->second; }
            if (k.find("dec.") != std::string::npos) continue;
            if (std::fabs(nv - old) > std::max(0.5, 0.05 * std::fabs(old))) { std::printf("  %-34s %9.2f -> %9.2f\n", k.c_str(), old, nv); ++diffs; } }
        std::printf("%d metric(s) changed\n", diffs); }
    return totalBad == 0 && totalInvalid == 0 ? 0 : 1;
}
