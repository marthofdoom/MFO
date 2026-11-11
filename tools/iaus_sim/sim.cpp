// iaus_sim/sim.cpp -- OFFLINE IAUS combat simulator, v2 (a game-faithful world). HOST-ONLY, never part of the DLL.
//
// Drives the REAL pure layers (iaus/Core.h: Decide / DecidePosture / OnFired; iaus/Generate.h: Generate) inside a
// world built from the installed list's own records and game settings (tools/list_balance: extract_world.py ->
// compile_world.py -> ~/.cache/mfo-list-balance/<list>.sim.json). List data is TEST INPUT only; nothing in native/
// reads it. Models, sources and proof levels: Docs/research/2026-11/iaus-sim-v2.md.
#include "iaus_sim/Setup.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

using namespace Sim;

namespace {

    struct Agg {   // a cell's tallies, summed
        Tally s; long fights = 0, wins = 0, losses = 0, playerDeaths = 0, forced = 0, timeouts = 0;
        double durSum = 0, firstKillSum = 0; long firstKillN = 0;
        void Add(const Tally& t, bool forcedBracket) {
            ++fights; wins += t.won; losses += t.lost; playerDeaths += t.playerDied; forced += forcedBracket; timeouts += !t.won && !t.lost; durSum += t.dur;
            if (t.firstKill >= 0) { firstKillSum += t.firstKill; ++firstKillN; }
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
            S(deathsWithHeal); S(retreatSec); S(kiteDmgTaken); S(wastedCasts);
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
        m["pow/min"] = double(s.powerAttacks) / mins; m["powCtl%"] = pct(double(s.powOnControlled), double(s.powerAttacks));
        return m;
    }

}

int main(int argc, char** argv) {
    const char* listPath = nullptr; const char* cmp = nullptr; const char* outBase = nullptr; std::uint64_t seed = 1; int fights = 8, threads = 8; bool tauntVisible = false, engineCasts = true, g10Sticky = false, legacy = false, mech = false, printClasses = false; std::string only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list") && i + 1 < argc) listPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fights") && i + 1 < argc) fights = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) cmp = argv[++i];
        else if (!std::strcmp(argv[i], "--baseline") && i + 1 < argc) outBase = argv[++i];
        else if (!std::strcmp(argv[i], "--taunt-visible")) tauntVisible = true;
        else if (!std::strcmp(argv[i], "--no-engine-casts")) engineCasts = false;
        else if (!std::strcmp(argv[i], "--g10-sticky")) g10Sticky = true;
        else if (!std::strcmp(argv[i], "--classes") && i + 1 < argc) { only = argv[++i]; if (only == "legacy") { legacy = true; only.clear(); } else if (only == "real") only.clear(); }
        else if (!std::strcmp(argv[i], "--profiles") && i + 1 < argc) mech = !std::strcmp(argv[++i], "mech");
        else if (!std::strcmp(argv[i], "--print-classes")) printClasses = true;
        else { std::fprintf(stderr, "usage: run.sh [--list sim.json] [--seed N] [--fights N] [--threads N] [--compare baseline] [--baseline out] [--taunt-visible] [--no-engine-casts] [--g10-sticky] [--classes legacy|real|slug,slug] [--profiles mech] [--print-classes]\n"); return 2; } }
    const Data D = listPath ? LoadData(listPath) : BuiltinData();
    const std::vector<SimClass> allClasses = legacy ? LegacyClasses() : RealClasses(mech);
    const std::vector<SimClass> t1(allClasses.begin(), allClasses.begin() + 3);
    if (printClasses) { PrintClassTable(allClasses); return 0; }
    std::vector<SimClass> classes;   // --classes a,b,c narrows the cells (slugs)
    for (const SimClass& c : allClasses) if (only.empty() || ("," + only + ",").find("," + c.name + ",") != std::string::npos) classes.push_back(c);
    if (classes.empty()) { std::fprintf(stderr, "no class matches --classes %s\n", only.c_str()); return 2; }
    std::printf("IAUS SIM v2  list=%s  seed=%llu  fights/cell=%d (x5 rapport ranks)  taunted-visible=%d  engine-casts=%d  classes=%s  profiles=%s\n", D.name.c_str(), (unsigned long long)seed, fights, int(tauntVisible), int(engineCasts), legacy ? "legacy" : "real", mech ? "mech" : "default");
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
                F.tauntVisible = tauntVisible; F.engineCasts = engineCasts; F.g10Sticky = g10Sticky;
                IausPolicy P; P.timeIt = true; RunFight(F, P);
                A.Add(F.t, forced); RA[rk - 1].Add(F.t, forced);
            }
            std::scoped_lock lk(mx); cell[static_cast<std::size_t>(job)] = A;
            for (int rk = 0; rk < 5; ++rk) { Agg& d = rankCell[static_cast<std::size_t>(c * 5 + rk)]; d.s.ticks += RA[rk].s.ticks; d.s.idle += RA[rk].s.idle; d.s.hold += RA[rk].s.hold; d.s.switches += RA[rk].s.switches; d.durSum += RA[rk].durSum; }
        }
    };
    { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }

    std::map<std::string, std::map<std::string, double>> all; long totalBad = 0, totalInvalid = 0;
    for (int c = 0; c < nc; ++c) for (int b = 0; b < kNB; ++b) { const Agg& A = cell[static_cast<std::size_t>(c * kNB + b)];
        all[classes[static_cast<std::size_t>(c)].name + "|" + kBrackets[b].name] = Metrics(A); totalBad += A.s.badFacts; totalInvalid += A.s.invalid; }
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
    std::printf("\nINTELLIGENCE (-1 = no sample)\n%-20s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "class|bracket", "ovkil%", "ovhl%", "dblh%", "intr%", "resis%", "immun%", "weak%", "splsh%", "ctlF%", "peel.s", "press%", "prio%", "waste%", "tsw/m", "dwh/f");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.2f %6.1f %6.1f %6.1f %6.1f %6.2f\n", kv.first.c_str(), G(m, "overkill%"), G(m, "overheal%"), G(m, "dblheal%"), G(m, "intr%"), G(m, "resisted%"), G(m, "immune%"), G(m, "weak%"), G(m, "splash%"), G(m, "ctrlFail%"), G(m, "peel.s"), G(m, "press%"), G(m, "prio%"), G(m, "waste%"), G(m, "tsw/min"), G(m, "dwh/f")); }
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
