#pragma once
// iaus_sim/ModeAccept.h -- --mode-accept: the fights the modes' acceptance is judged on (s5-plan.md 5.3, 5.4). HOST-ONLY. It MEASURES and prints. mode_accept.py judges.
//
// Per (scenario, class, luck): the IAUS arm and the subject-only oracle arm of one fight on the strict road, the same luck, and at every order race of the oracle's the MODE LEADS on
// fresh streams (Oracle.h modeLines). Lines, in job order whatever the thread count:
//   ACCEPTRUN  gen family seed set caps against places | luck refuse k
//   ACCEPTFIGHT <k> <class> <luck> uI uO winI winO ...    the two arms. base = the calibration's check (road off), from --mode-cal
//   ACCEPTKITE  <k> <class> <luck> ...                    the oracle's kite strike windows (World.h Tally), only where a Kite order stood
//   ACCEPTDEC   <k> <class> <luck> t= pass= own=<mode> <other>=leadA/seA/leadB ...
// --mode-refuse <mode> plays the leave-one-out arm: the subject may not be in that mode, whoever orders it. Kite and disengage are refused by the caps as well.
// The difficulty comes from --mode-cal (the MODECAL lines of --mode-calib): a scenario without a row is an error, as is a table of another generator version, family, seed, set or caps.
#include "iaus_sim/ModeGen.h"

namespace Sim {

    using CalTable = std::map<std::pair<int, std::string>, std::pair<float, std::string>>;   // (k, class) -> ratio, the rest of the line
    [[nodiscard]] inline bool GenLoadCal(const GenRun& o, CalTable& T, std::string& err) {
        std::ifstream in(o.cal); if (o.cal.empty() || !in) { err = "--mode-cal <table> (the MODECAL lines of --mode-calib) is needed"; return false; }
        const std::string want = GenHeader(o, "MODECALRUN", o.g.seed); std::string l; bool head = false;
        while (std::getline(in, l)) { if (l.rfind("MODECALRUN ", 0) == 0) { if (l.rfind(want, 0) != 0) { err = "the table is of another run: " + l + " (wanted " + want + ")"; return false; } head = true; }
            if (l.rfind("MODECAL ", 0) != 0) continue; int k = 0, at = 0; char c[64]; float r = 0; if (std::sscanf(l.c_str(), "MODECAL %d %63s ratio=%f%n", &k, c, &r, &at) != 3) { err = "bad line: " + l; return false; }
            T[{ k, c }] = { r, l.substr(static_cast<std::size_t>(at)) }; }
        if (!head) err = "no MODECALRUN line in the table"; return head; }

    [[nodiscard]] inline std::string KiteLine(const Tally& t, double removed) { char b[256]; std::snprintf(b, sizeof b, "kiteSec=%.1f episodes=%ld dead=%ld longest=%.1f strikes=%ld gap=%.1f dmgShare=%.3f", t.kiteSec, t.kiteN, t.kiteDead + (t.kiteOn && t.kiteStrikes == t.kiteStrikes0), t.kiteLongest, t.kiteStrikes, t.kiteGap, removed > 0 ? (t.kiteDmg + (t.kiteOn ? t.dmgDone - t.kiteDmg0 : 0.0)) / removed : 0.0); return b; }
    inline int RunModeAccept(const Data& D, const std::vector<SimClass>& classes, int threads, const GenRun& o, const OracleCfg& oc) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); std::vector<GenPlace> places; if (!GenSetup(D, classes, t1, o, places, "--mode-accept")) return 2;
        CalTable cal; std::string err; if (!GenLoadCal(o, cal, err)) { std::fprintf(stderr, "--mode-accept: %s\n", err.c_str()); return 2; }
        std::uint32_t refuse = o.refuse; if (refuse & (1u << 1)) refuse |= g_moveCaps.disengage ? 0u : 1u << 2;   // a GoTo is carried out as a stand where no disengage is served (ModeCarried): both go
        const StrictCaps keep = g_moveCaps; const std::string head = GenHeader(o, "ACCEPTRUN", o.g.seed);
        std::vector<GenJob> jobs; for (const GenJob& j : GenJobs(o, places)) { const auto it = cal.find({ j.k, kModeClasses[j.ci] }); if (it == cal.end()) { std::fprintf(stderr, "--mode-accept: no difficulty for scenario %d %s in %s\n", j.k, kModeClasses[j.ci], o.cal.c_str()); return 2; } jobs.push_back(j); }
        if (refuse & (1u << 3)) g_moveCaps.kite = false; if (refuse & (1u << 4)) g_moveCaps.disengage = false;
        g_road = Road::Strict; const std::size_t L = static_cast<std::size_t>(o.luck1 - o.luck0); std::vector<std::string> out(jobs.size() * L); std::vector<char> done(out.size(), 0); std::size_t shown = 0; std::mutex mx; /* printed in job order as soon as it can be */ std::atomic<long> rolls{ 0 }, decs{ 0 };
        std::printf("%s luck=%d:%d refuse=%u k=%d drawn=%zu\n", head.c_str(), o.luck0, o.luck1, o.refuse, o.modeK, places.size()); std::fflush(stdout);
        GenParallel(out.size(), threads, [&](std::size_t x) { const GenJob& q = jobs[x / L]; const int luck = o.luck0 + static_cast<int>(x % L); const auto& c = cal.at({ q.k, kModeClasses[q.ci] }); const FightSpec fs = GenSpec(D, classes, kModeClasses[q.ci], q.s, c.first, q.place);
            const auto arm = [&](Policy& P, std::uint32_t ref) { Fight F = GenBuild(D, fs, q.s, t1); GenLuck(F, fs.seed, luck, kAcceptSalt); F.move.s = std::make_unique<MoveState>(); F.move->claim.resize(F.ctl.size()); F.move->refuse = ref; RunFight(F, P); return F; };
            IausPolicy I; I.T = ActiveTuning(); const Fight A = arm(I, 0); std::string& s = out[x]; char b[512]; char id[96]; std::snprintf(id, sizeof id, "%d %s %d", q.k, kModeClasses[q.ci], luck); double uO = 0; int winO = 0; double durO = 0, stO = 0, dO = 0, gapO = 0; std::string kite; std::vector<std::string> ml;
            if (!o.iausOnly) { OraclePolicy O; O.cfg = oc; O.cfg.allFollowers = false; O.iaus.T = ActiveTuning(); O.modeLines = &ml; O.modeK = o.modeK; const Fight B = arm(O, refuse); uO = Utility(B); winO = B.t.won; durO = B.now; stO = B.t.upSec > 0 ? B.t.strictSec / B.t.upSec : 0; double left = 0; for (const Body& z : B.b) if (z.role == Role::Foe && z.alive) left += std::max(0.0f, z.c.hp); const double removed = std::max(0.0, double(B.foeHpTotal) - left); dO = removed > 0 ? B.t.dmgDone / removed : 0; gapO = B.t.strikeGap; if (B.t.kiteN) kite = KiteLine(B.t, removed); rolls += O.modeRolls; decs += static_cast<long>(ml.size()); }
            std::snprintf(b, sizeof b, "ACCEPTFIGHT %s row=%s against=%s%s uI=%.4f uO=%.4f winI=%d winO=%d durI=%.1f durO=%.1f claimI=%.3f claimO=%.3f share=%.3f strikeGap=%.1f\n", id, q.s.row >= 0 ? kModeRows[q.s.row].name : "-", q.s.row >= 0 ? kGenAgainst[kModeRows[q.s.row].against] : "-", c.second.c_str(), Utility(A), o.iausOnly ? Utility(A) : uO, int(A.t.won), winO, A.now, durO, A.t.upSec > 0 ? A.t.strictSec / A.t.upSec : 0.0, stO, dO, gapO); s = b;
            if (!kite.empty()) s += std::string("ACCEPTKITE ") + id + " " + kite + "\n"; for (const std::string& l : ml) s += std::string("ACCEPTDEC ") + id + " " + l + "\n";
            std::scoped_lock lk(mx); done[x] = 1; while (shown < out.size() && done[shown]) { std::fputs(out[shown].c_str(), stdout); out[shown++].clear(); } std::fflush(stdout); });
        std::printf("ACCEPTDONE fights=%zu decisions=%ld modeRolls=%ld\n", out.size(), decs.load(), rolls.load()); g_moveCaps = keep; g_road = Road::Off; return 0; }

    // --mode-unit: planted vectors through the pure pieces (a mutant of any of them fails a row: iaus-sim-v4.md 24).
    inline int RunModeUnit() {
        int n = 0, bad = 0; const auto row = [&](const char* what, bool ok) { ++n; if (!ok) { ++bad; std::printf("  MODE-UNIT FAILED %s\n", what); } }; using MM = MV::MoveMode; using Vd = std::vector<double>;
        const std::vector<int> md{ 0, 1 }; const auto L = [&](const std::vector<Vd>& A, const std::vector<Vd>& B) { return OraclePolicy::ModeLead(md, A, B); };
        row("half A names the owner, half B is the value", L({ { 1, 2, 3 }, { 0, 0, 0 } }, { { 0, 0, 0 }, { 4, 4, 4 } }) == "own=free stand=2.0000/0.5774/-4.0000");
        row("the other mode owns when it leads", L({ { 0, 0 }, { 1, 1 } }, { { 1, 1 }, { 3, 3 } }) == "own=stand free=1.0000/0.0000/2.0000");
        row("equal modes: the lead is zero and so is its error", L({ { 5, 7 }, { 5, 7 } }, { { 1, 1 }, { 1, 1 } }) == "own=free stand=0.0000/0.0000/0.0000");
        row("pairs by stream, not by order of size", L({ { 1, 9 }, { 0, 8 } }, { { 0, 0 }, { 0, 0 } }) == "own=free stand=1.0000/0.0000/0.0000");
        row("a refused Free is a Stand, a refused Stand is Free, another mode stays", RefusedMode(1u, MM::Free) == MM::Stand && RefusedMode(2u, MM::Stand) == MM::Free && RefusedMode(8u, MM::Kite) == MM::Free && RefusedMode(2u, MM::Kite) == MM::Kite && RefusedMode(0u, MM::Stand) == MM::Stand);
        { MoveState S; MoveClaim q; Body x; x.c.x = 5; x.nav.tri = 3; MV::MoveOrder o; o.gen = 7; S.refuse = 1u; MV::MoveOrder r = RefusedOrder(S, q, x, o); row("a refused Free stands where he is, its gen is the pass's", r.mode == MM::Stand && r.p.x == 5 && r.tri == 3 && r.gen == 7 && r.leashR > 0.0f);
          q.order = r; q.order.p.x = 90; q.phase = StrictPhase::Standing; row("... and the stand that stands is kept", RefusedOrder(S, q, x, o).p.x == 90); q.phase = StrictPhase::Ended; row("... an ended one is not", RefusedOrder(S, q, x, o).p.x == 5); S.refuse = 2u; o.mode = MM::Stand; row("a refused Stand is a free order", RefusedOrder(S, q, x, o).mode == MM::Free); }
        row("speed: a third under kiteSpeed, a third under his, the rest to 1.5", GenSpeed(0.0f) == 0.20f && GenSpeed(0.33f) < 0.40f && GenSpeed(0.34f) >= 0.40f && GenSpeed(0.66f) < 1.0f && GenSpeed(0.67f) >= 1.0f && GenSpeed(0.999f) > 1.45f);
        row("count: 1 / 2-3 / 4-6 / 7-10 at 25, 30, 25, 20 percent", GenCount(0.249f) == 1 && GenCount(0.25f) == 2 && GenCount(0.549f) == 3 && GenCount(0.55f) == 4 && GenCount(0.799f) == 6 && GenCount(0.80f) == 7 && GenCount(0.9999f) == 10);
        { std::array<int, kGenBlock> seen{}; for (int k = 0; k < kGenBlock; ++k) ++seen[static_cast<std::size_t>(GenU(0, 1, k, Ax::Light) * kGenBlock)]; row("a block visits every stratum of an axis once", std::count(seen.begin(), seen.end(), 1) == kGenBlock); }
        { bool ok = false; const float r = GenBisect(0.5f, 12, [](float x) { return x < 1.3f ? 0.9f : x < 1.5f ? 0.5f : 0.1f; }, ok); row("the bisection finds the ratio where the share is the target", ok && r >= 1.3f && r < 1.5f); bool no = true; (void)GenBisect(0.5f, 9, [](float) { return 1.0f; }, no); row("a fight that is always won is not calibrated", !no); }
        row("strata: the edges", GenStratum(149, 63, 0) == 0 && GenStratum(150, 64, 1) == 13 && GenStratum(401, 201, 6) == 26 && GenStratum(400, 200, 5) == 13);
        { std::vector<GenPlace> all; std::vector<SimEnv> e(40); for (int i = 0; i < 40; ++i) { e[static_cast<std::size_t>(i)].hash = static_cast<std::uint64_t>(i * 977); GenPlace p; p.env = &e[static_cast<std::size_t>(i)]; p.stratum = i < 30 ? 0 : i < 35 ? 1 : 2; all.push_back(p); } const auto a = GenPlaces(all, GenOpts{}, 9), b = GenPlaces(all, GenOpts{}, 20); std::array<int, 3> ca{}, cb{}; for (const auto& p : a) ++ca[static_cast<std::size_t>(p.stratum)]; for (const auto& p : b) ++cb[static_cast<std::size_t>(p.stratum)]; row("three of a stratum first, then one more of each in turn", GenPlaces(all, GenOpts{}, 2).size() == 9 && ca == std::array<int, 3>{ 3, 3, 3 } && b.size() == 20 && cb[1] == 5 && cb[2] == 5 && cb[0] == 10); }
        { bool ok = true; for (const GenRow& r : kModeRows) ok = ok && r.favours != r.against && (r.kind >= 0) != (r.group >= 0) && (!r.terrain[0] || GenTerrainNo(r.terrain) >= 0) && r.nLo <= r.nHi && r.nHi <= 10 && r.dLo >= kGenT.distLo && r.dHi <= kGenT.distHi; row("the rows are well formed", ok); }
        std::printf("MODE-UNIT %d rows, failed %d\n", n, bad); return bad != 0; }

}
