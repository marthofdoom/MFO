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
        g_road = Road::Strict; const std::size_t L = static_cast<std::size_t>(o.luck1 - o.luck0); std::vector<std::string> out(jobs.size() * L); std::atomic<long> rolls{ 0 }, decs{ 0 };
        std::printf("%s luck=%d:%d refuse=%u k=%d drawn=%zu\n", head.c_str(), o.luck0, o.luck1, o.refuse, o.modeK, places.size());
        GenParallel(out.size(), threads, [&](std::size_t x) { const GenJob& q = jobs[x / L]; const int luck = o.luck0 + static_cast<int>(x % L); const auto& c = cal.at({ q.k, kModeClasses[q.ci] }); const FightSpec fs = GenSpec(D, classes, kModeClasses[q.ci], q.s, c.first, q.place);
            const auto arm = [&](Policy& P, std::uint32_t ref) { Fight F = GenBuild(D, fs, q.s, t1); GenLuck(F, fs.seed, luck, kAcceptSalt); F.move.s = std::make_unique<MoveState>(); F.move->claim.resize(F.ctl.size()); F.move->refuse = ref; RunFight(F, P); return F; };
            IausPolicy I; I.T = ActiveTuning(); const Fight A = arm(I, 0); std::string& s = out[x]; char b[512]; char id[96]; std::snprintf(id, sizeof id, "%d %s %d", q.k, kModeClasses[q.ci], luck); double uO = 0; int winO = 0; double durO = 0, stO = 0, dO = 0, gapO = 0; std::string kite; std::vector<std::string> ml;
            if (!o.iausOnly) { OraclePolicy O; O.cfg = oc; O.cfg.allFollowers = false; O.iaus.T = ActiveTuning(); O.modeLines = &ml; O.modeK = o.modeK; const Fight B = arm(O, refuse); uO = Utility(B); winO = B.t.won; durO = B.now; stO = B.t.upSec > 0 ? B.t.strictSec / B.t.upSec : 0; double left = 0; for (const Body& z : B.b) if (z.role == Role::Foe && z.alive) left += std::max(0.0f, z.c.hp); const double removed = std::max(0.0, double(B.foeHpTotal) - left); dO = removed > 0 ? B.t.dmgDone / removed : 0; gapO = B.t.strikeGap; if (B.t.kiteN) kite = KiteLine(B.t, removed); rolls += O.modeRolls; decs += static_cast<long>(ml.size()); }
            std::snprintf(b, sizeof b, "ACCEPTFIGHT %s row=%s against=%s%s uI=%.4f uO=%.4f winI=%d winO=%d durI=%.1f durO=%.1f claimI=%.3f claimO=%.3f share=%.3f strikeGap=%.1f\n", id, q.s.row >= 0 ? kModeRows[q.s.row].name : "-", q.s.row >= 0 ? kGenAgainst[kModeRows[q.s.row].against] : "-", c.second.c_str(), Utility(A), o.iausOnly ? Utility(A) : uO, int(A.t.won), winO, A.now, durO, A.t.upSec > 0 ? A.t.strictSec / A.t.upSec : 0.0, stO, dO, gapO); s = b;
            if (!kite.empty()) s += std::string("ACCEPTKITE ") + id + " " + kite + "\n"; for (const std::string& l : ml) s += std::string("ACCEPTDEC ") + id + " " + l + "\n"; });
        for (const auto& s : out) std::fputs(s.c_str(), stdout);
        std::printf("ACCEPTDONE fights=%zu decisions=%ld modeRolls=%ld\n", out.size(), decs.load(), rolls.load()); g_moveCaps = keep; g_road = Road::Off; return 0; }

}
