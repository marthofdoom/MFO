#pragma once
// iaus_sim/SpotTest.h -- S2: the seven scenarios of iaus-sim-v4.md 8.5 as ASSERTED OUTCOMES over the movement pass (`sim --spot-test`). HOST-ONLY.
//
// Each row of kSpotCases is played on its hand-made fixture with --position-road point (no pin kinds, except the doorway, which says so), and must hold. Then
// the same fights are played with ONE mechanism of the pass broken (kMoveFaults) and the rule must NOT hold: a rule that passes broken tests nothing.
// The bodies a rule does not judge are statues (run speed 0, a deep health pool), so what moves and what it reaches is the followers' own doing.
// SCRIPTED rows replace his own ranking by a fixed one after each lap: the rule is then about the PLACE, for an action he does not pick by himself in
// that fight (marked in the row; a heal past its reach is no candidate of Generate.cpp at all, movement-plan-s2 note).
#include "iaus_sim/Oracle.h"
#include "iaus_sim/Scenarios.h"

#include <functional>
#include <map>
#include <thread>

namespace Sim {

    struct SpotProbe { std::map<std::string, double> v; double& operator[](const char* k) { return v[k]; } };
    struct SpotCase {
        const char* name; const char* scenario; const char* fault; Road road = Road::Point; float pin = 0.0f; double sec = 30.0; bool scripted = false;
        std::function<void(Fight&)> setup = nullptr;                        // once, before the first step
        std::function<void(Fight&)> script = nullptr;                       // before every step (scripted rankings)
        std::function<void(Fight&, SpotProbe&)> watch = nullptr;            // after every step
        std::function<bool(const std::vector<SpotProbe>&, std::string&)> rule = nullptr;   // over all fights; writes its numbers
        const char* what = "";
    };
    namespace SpotDetail {
        inline EQ::Vec3 At(const Fight& F, float sx, float sy, int rows, float cell = 128.0f) { const EQ::View v = EnvViewOf(F); const EQ::Vec3 p{ (sx + 0.5f) * cell, (static_cast<float>(rows) - sy - 0.5f) * cell, 0.0f }; EQ::Vec3 best = p; float bd = 1e30f;
            for (std::size_t i = 0; i < v.tris.size(); ++i) { const EQ::Vec3 c = EQ::Centroid(v, static_cast<std::int32_t>(i)); const float d = std::hypot(c.x - p.x, c.y - p.y); if (d < bd) { bd = d; best = c; } } return best; }
        inline void Put(Fight& F, Body& x, const EQ::Vec3& p) { x.c.x = p.x; x.c.y = p.y; x.c.z = p.z; EQ::Vec3 on; x.nav = Nav{}; x.nav.tri = EQ::NearestTri(EnvViewOf(F), p, 200.0f, on); x.c.x = on.x; x.c.y = on.y; x.c.z = on.z; }
        inline void Statue(Body& x, bool harmless) { auto sp = std::make_shared<ActorSpec>(*x.spec); sp->run = 0.0f; if (harmless) { sp->ranged = {}; sp->spells.clear(); } x.spec = sp; x.c.hp = x.c.hpMax = 50000.0f; }
        inline Body& Sub(Fight& F) { return F.b[static_cast<std::size_t>(F.ctl[0].body)]; }
        inline std::vector<Body*> Foes(Fight& F) { std::vector<Body*> o; for (Body& x : F.b) if (!x.Party()) o.push_back(&x); return o; }
        // every foe and the player stand; foes shoot nothing and keep their target
        inline void AllStatues(Fight& F) { for (Body& x : F.b) if (x.ctl < 0) { Statue(x, true); x.retargetAt = 1e9; } for (Body* f : Foes(F)) f->c.targetId = F.b[0].c.id; }
        inline double Sum(const std::vector<SpotProbe>& v, const char* k) { double s = 0; for (const auto& p : v) { const auto it = p.v.find(k); if (it != p.v.end()) s += it->second; } return s; }
        inline int Count(const std::vector<SpotProbe>& v, const std::function<bool(const SpotProbe&)>& f) { int n = 0; for (const auto& p : v) n += f(p); return n; }
        inline double Get(const SpotProbe& p, const char* k, double def) { const auto it = p.v.find(k); return it == p.v.end() ? def : it->second; }
        inline std::string Fmt(const char* f, auto... a) { char b[300]; std::snprintf(b, sizeof b, f, a...); return b; }
        // He stands on a place chosen for a sighted action: does he see its target there? (reach.n places reached, reach.see of them with sight)
        inline void WatchReached(Fight& F, SpotProbe& p) {
            const MoveClaim& q = F.move->claim[0]; const bool now = q.on && q.arrived, was = p["_arr"] > 0 && p["_key"] == static_cast<double>(q.key % 1000003u);
            if (now && !was && q.act.target && !q.act.melee) { const Body* t = F.Find(q.act.target); if (t && Fight::Up(*t)) { p["reach.n"] += 1; if (EnvSees(F, Sub(F), *t)) p["reach.see"] += 1; } }
            if (now && q.act.target && !q.act.melee) { const Body* t = F.Find(q.act.target); if (t && Fight::Up(*t) && !EnvSees(F, Sub(F), *t)) p["blind"] += TM::dt; }
            p["_arr"] = now; p["_key"] = static_cast<double>(q.key % 1000003u);
            p["filed"] = static_cast<double>(F.t.spotFiled);
        }
        inline void WatchHit(Fight& F, SpotProbe& p) { if (F.t.firstHitN > 0 && !p.v.count("hit")) p["hit"] = F.t.firstHitSum; p["holdUn"] = F.t.pathHoldUnreachSec; p["stuck"] = F.t.pathStuckSec; }
        inline MFO::Iaus::RankedAction Want(Family f, std::uint32_t target, float score, float reach, Sight s, bool melee) { MFO::Iaus::RankedAction a; a.family = f; a.target = target; a.score = score; a.reach = reach; a.sight = s; a.melee = melee; a.form = 1; return a; }
        inline void Script(Fight& F, std::size_t ci, std::initializer_list<MFO::Iaus::RankedAction> as) { Ctl& c = F.ctl[ci]; Body& x = F.b[static_cast<std::size_t>(c.body)]; c.runPhase = 0; c.moveUntil = 0.0; x.holdUntil = x.retreatUntil = 0.0;   // a scripted row is about the claim: no other mover
            if (!F.move || F.move->claim.size() <= ci) return; MoveClaim& q = F.move->claim[ci]; q.nTop = 0; for (const auto& a : as) if (q.nTop < 3) q.top[q.nTop++] = a; q.topAt = F.now; }
    }

    inline std::vector<SpotCase> SpotCases() {
        using namespace SpotDetail; std::vector<SpotCase> v;
        const auto seesOnPlaces = [](const std::vector<SpotProbe>& r, double within, std::string& o) {
            const int n = static_cast<int>(r.size()), hit = Count(r, [&](const SpotProbe& p) { return Get(p, "hit", 1e9) <= within; }); const double rn = Sum(r, "reach.n"), bl = Sum(r, "blind") / n;
            o = Fmt("first hit within %.0f s in %d of %d, places reached %.0f, held without sight %.2f s a fight, hold-unreachable %.2f s", within, hit, n, rn, bl, Sum(r, "holdUn") / n);
            return hit * 10 >= n * 8 && rn >= n * 0.5 && bl <= 1.5 && Sum(r, "holdUn") / n <= 1.0; };
        { SpotCase c{ "lost-sight-firing-spot", "env-lost-sight", "blind" }; c.what = "a caster whose foes stand round a corner takes a place that sees them and fires; he holds no place that does not";
          c.setup = [](Fight& F) { AllStatues(F); for (Body* f : Foes(F)) Put(F, *f, At(F, 21, 9, 16)); Put(F, *Foes(F)[1], At(F, 21, 11, 16)); Put(F, Sub(F), At(F, 14, 4, 16)); Put(F, F.b[0], At(F, 12, 4, 16)); };
          c.script = [](Fight& F) { for (Body& x : F.b) if (x.role == Role::Summon) x.alive = false; };   // his atronach stands in his own line of fire: not what this row is about
          c.watch = [](Fight& F, SpotProbe& p) { WatchReached(F, p); WatchHit(F, p); }; c.rule = [=](const auto& r, std::string& o) { return seesOnPlaces(r, 15.0, o); }; v.push_back(c); }
        { SpotCase c{ "ridge-self-occlusion", "env-ridge", "terrain" }; c.what = "an archer never holds a place from which the ridge hides his target (Adelinda, 11-18)";
          c.setup = [](Fight& F) { AllStatues(F); int k = 0; for (Body* f : Foes(F)) Put(F, *f, At(F, 18, 3.0f + 2.0f * static_cast<float>(k++), 9, 160.0f)); Put(F, Sub(F), At(F, 6, 4, 9, 160.0f)); Put(F, F.b[0], At(F, 3, 4, 9, 160.0f)); };
          c.watch = [](Fight& F, SpotProbe& p) { WatchReached(F, p); WatchHit(F, p); }; c.rule = [=](const auto& r, std::string& o) { return seesOnPlaces(r, 15.0, o); }; v.push_back(c); }
        { SpotCase c{ "tower-stairs (blade)", "env-tower-stairs", "stay-holds" }; c.what = "the warrior reaches the tower top by its ramp and does not stand at the foot looking up";
          c.setup = [](Fight& F) { AllStatues(F); }; c.watch = [](Fight& F, SpotProbe& p) { WatchHit(F, p); const Body& s = Sub(F); const Body* f = Foes(F)[0]; if (std::fabs(s.c.z - f->c.z) < 100.0f && Fight::Dist(s, *f) < 220.0f && !p.v.count("top")) p["top"] = F.now; if (F.now - s.movedAt > 1.0 && f->c.z - s.c.z > 100.0f) p["foot"] += TM::dt; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()), top = Count(r, [](const SpotProbe& p) { return Get(p, "top", 1e9) <= 20.0; }); o = Fmt("on the top beside him within 20 s in %d of %d, stood below him %.2f s a fight", top, n, Sum(r, "foot") / n); return top * 10 >= n * 9 && Sum(r, "foot") / n <= 1.0; }; v.push_back(c); }
        { SpotCase c{ "tower-stairs (archer)", "env-tower-archer", "blind" }; c.what = "a bow shoots the tower top from below, from a place that sees it, and does not climb into the blade";
          c.setup = [](Fight& F) { AllStatues(F); Put(F, *Foes(F)[0], At(F, 15, 5, 11)); Put(F, Sub(F), At(F, 8, 7, 11)); }; c.watch = [](Fight& F, SpotProbe& p) { WatchReached(F, p); WatchHit(F, p); p["z"] = std::max(p["z"], static_cast<double>(Sub(F).c.z)); };
          c.rule = [=](const std::vector<SpotProbe>& r, std::string& o) { const bool k = seesOnPlaces(r, 15.0, o); const int up = Count(r, [](const SpotProbe& p) { return Get(p, "z", 0) > 330.0; }); o += Fmt(", on the top in %d", up); return k && up == 0; }; v.push_back(c); }
        { SpotCase c{ "above-through-wall", "env-above-wall", "reach" }; c.what = "a blade is never given the foe no road leads to while another can be reached (scripted: he ranks the unreachable one 1.15 times higher)"; c.scripted = true;
          c.setup = [](Fight& F) { AllStatues(F); F.ctl[0].drawn = PostureKind::Melee; auto spec = std::make_shared<ActorSpec>(*Foes(F)[0]->spec); Body& n = F.Add(Role::Foe, spec, F.b[0].c.x + 100.0f, F.b[0].c.y - 100.0f); Statue(n, true); n.c.targetId = F.b[0].c.id; n.retargetAt = 1e9; Put(F, n, At(F, 2, 6, 8)); Statue(Sub(F), true); Put(F, Sub(F), At(F, 10, 4, 8)); Put(F, F.b[0], At(F, 4, 2, 8)); };
          c.script = [](Fight& F) { const auto f = Foes(F); Script(F, 0, { Want(Family::Attack, f[0]->c.id, 0.80f, 150.0f, Sight::Visible, true), Want(Family::PowerAttack, f[1]->c.id, 0.70f, 150.0f, Sight::Visible, true) }); };
          c.watch = [](Fight& F, SpotProbe& p) { if (F.move->pic.window == static_cast<std::uint32_t>(p["_w"])) return; p["_w"] = F.move->pic.window; for (const auto& a : F.move->last) if (a.who == Sub(F).c.id && a.act.family != Family::kCount) { p["n"] += 1; if (a.target == Foes(F)[0]->c.id) p["un"] += 1; } };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { o = Fmt("windows decided %.0f, on the unreachable foe %.0f", Sum(r, "n"), Sum(r, "un")); return Sum(r, "n") > 0 && Sum(r, "un") == 0; }; v.push_back(c); }
        { SpotCase c{ "doorway-block", "env-doorway-block", "portals" }; c.road = Road::Pinned; c.pin = 1.0f; c.scripted = true; c.what = "two blades who both want the chokepoint: one place in the doorway at a time (pinned road, pin kinds on)";
          c.setup = [](Fight& F) { AllStatues(F); int k = 0; for (Body* f : Foes(F)) Put(F, *f, At(F, 13.0f + static_cast<float>(k++), 4, 10)); Put(F, Sub(F), At(F, 2, 3, 10)); Put(F, F.b[static_cast<std::size_t>(F.ctl[1].body)], At(F, 2, 5, 10)); Put(F, F.b[0], At(F, 1, 1, 10)); for (Ctl& c : F.ctl) { c.leash = 0.0f; MFO::Iaus::SetWeight(c.profile, Family::Attack, Cons::SpotAllyDist, 0.0f); }   /* the spacing is off in BOTH arms: only the doorway rule keeps them apart */ };
          c.script = [](Fight& F) { const auto f = Foes(F); for (std::size_t i = 0; i < 2; ++i) Script(F, i, { Want(Family::Attack, f[0]->c.id, 0.80f, 150.0f, Sight::Visible, true) }); };
          c.watch = [](Fight& F, SpotProbe& p) { const EQ::View w = EnvViewOf(F); auto door = [&](const MoveClaim& q) { return q.on ? MV::PortalsAt(w, F.move->pic.wallVert, q.tri, q.p, DefaultTuning()) : std::array<std::uint64_t, 2>{}; }; const auto a = door(F.move->claim[0]), b = door(F.move->claim[1]); bool both = false; for (auto x : a) for (auto y : b) both |= x && x == y; if (a[0] || b[0]) p["one"] = 1; if (both) p["both"] = 1; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()); o = Fmt("the doorway was held in %.0f of %d, two in it in %.0f", Sum(r, "one"), n, Sum(r, "both")); return Sum(r, "one") * 10 >= n * 9 && Sum(r, "both") == 0; }; v.push_back(c); }
        { SpotCase c{ "healer-round-corner", "env-heal-corner", "no-heal" }; c.scripted = true; c.what = "a healer whose patient fights round the corner walks to where he reaches and sees him (scripted: Generate offers no heal past its reach)";
          c.setup = [](Fight& F) { AllStatues(F); Put(F, F.b[0], At(F, 14, 3, 11)); F.b[0].c.hp = 0.4f * F.b[0].c.hpMax; Put(F, Sub(F), At(F, 4, 2, 11)); Put(F, F.b[static_cast<std::size_t>(F.ctl[1].body)], At(F, 2, 3, 11)); F.ctl[0].leash = 0.0f; int k = 0; for (Body* f : Foes(F)) Put(F, *f, At(F, 15.0f, 2.0f + static_cast<float>(k++), 11)); };
          c.script = [](Fight& F) { const Body& s = Sub(F); const bool see = EnvSees(F, F.b[0], s); Script(F, 0, { Want(Family::CastHeal, F.b[0].c.id, 1.2f, 1000.0f, see ? Sight::Visible : Sight::Occluded, false) }); };
          c.watch = [](Fight& F, SpotProbe& p) { const Body& s = Sub(F); if (F.move->claim[0].on && F.move->claim[0].arrived && Fight::Dist(s, F.b[0]) <= 1000.0f && EnvSees(F, s, F.b[0]) && !p.v.count("ok")) p["ok"] = F.now; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()), k = Count(r, [](const SpotProbe& p) { return Get(p, "ok", 1e9) <= 12.0; }); o = Fmt("stands where he reaches and sees the patient within 12 s in %d of %d", k, n); return k * 10 >= n * 9; }; v.push_back(c); }
        // the pocket's squares: rows 1 to 3 of the map
        const auto pocket = [](const EQ::Vec3& p) { return p.y > (9.0f - 4.0f) * 128.0f; };
        { SpotCase c{ "dead-end-retreat (a way on)", "env-dead-end", "exits" }; c.scripted = true; c.sec = 12.0; c.what = "a hurt caster with two blades on him in the hall: no retreat is ever sent into the pocket room (scripted: he ranks a retreat)";
          c.setup = [](Fight& F) { AllStatues(F); Put(F, Sub(F), At(F, 9, 5, 9)); Put(F, F.b[0], At(F, 24, 6, 9)); Put(F, *Foes(F)[0], At(F, 7, 6, 9)); Put(F, *Foes(F)[1], At(F, 8, 7, 9)); F.ctl[0].leash = 0.0f; for (Body* f : Foes(F)) f->c.targetId = Sub(F).c.id; };
          c.script = [](Fight& F) { Script(F, 0, { Want(Family::Retreat, 0, 0.9f, 0.0f, Sight::None, false) }); };
          c.watch = [pocket](Fight& F, SpotProbe& p) { const MoveClaim& q = F.move->claim[0]; if (q.on) { p["filed"] = 1; if (pocket(q.p)) p["pocket"] = 1; } };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()); o = Fmt("a retreat place in %.0f of %d, in the pocket in %.0f", Sum(r, "filed"), n, Sum(r, "pocket")); return Sum(r, "filed") * 10 >= n * 9 && Sum(r, "pocket") == 0; }; v.push_back(c); }
        { SpotCase c{ "dead-end-retreat (none)", "env-dead-end", "exits" }; c.scripted = true; c.sec = 8.0; c.what = "in the pocket with the foes at its mouth: no exit route exists, so no retreat place is offered";
          c.setup = [](Fight& F) { AllStatues(F); Put(F, Sub(F), At(F, 8, 3, 9)); Put(F, F.b[0], At(F, 2, 6, 9)); Put(F, *Foes(F)[0], At(F, 9, 4, 9)); Put(F, *Foes(F)[1], At(F, 10, 4, 9)); F.ctl[0].leash = 0.0f; for (Body* f : Foes(F)) f->c.targetId = Sub(F).c.id; };
          c.script = [](Fight& F) { Script(F, 0, { Want(Family::Retreat, 0, 0.9f, 0.0f, Sight::None, false) }); };
          c.watch = [](Fight& F, SpotProbe& p) { const MoveClaim& q = F.move->claim[0]; if (q.on && (q.kinds & MV::KindBit(MFO::Iaus::SpotKind::RetreatExit))) p["filed"] = 1; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { o = Fmt("a retreat place in %.0f of %zu", Sum(r, "filed"), r.size()); return Sum(r, "filed") == 0; }; v.push_back(c); }
        return v;
    }

    inline int RunSpotTests(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int threads) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); const std::vector<Scenario> all = AllScenarios(true, true, true); constexpr int N = 12; int fails = 0, checks = 0;
        const Tuning* base = ActiveTuning(); const char* traced = std::getenv("SIM_SPOTTRACE");   // <case name>:<arm 0|1>:<fight>: one event trace (--threads 1)
        const std::vector<SpotCase> cases = SpotCases();
        for (const SpotCase& c : cases) {
            const Scenario* sc = nullptr; for (const Scenario& s : all) if (!std::strcmp(s.name, c.scenario)) sc = &s; if (!sc) { std::printf("SPOT-TEST %s: no scenario %s\n", c.name, c.scenario); return 2; }
            Tuning t = *base; t.spotPinKinds = c.pin; ActiveTuning() = &t; g_road = c.road;
            for (int arm = 0; arm < 2; ++arm) {
                g_moveFault = arm ? MoveFaultByName(c.fault) : &kMoveFaults[0]; std::vector<SpotProbe> res(N); std::atomic<int> next{ 0 };
                auto work = [&]() { for (;;) { const int f = next.fetch_add(1); if (f >= N) return; Rng R; R.g.seed(seed * 2654435761ULL + std::uint64_t(f) * 7919 + 17); FightSpec fs = sc->make(D, classes, R); fs.seed = R.g(); fs.rank = 3;
                    Fight F = BuildFight(D, fs, t1); F.SetArena(); if (c.setup) c.setup(F); if (traced && std::string(traced) == std::string(c.name) + ":" + char('0' + arm) + ":" + std::to_string(f)) F.trace = stdout;
                    IausPolicy P; P.T = &t; SpotProbe& p = res[static_cast<std::size_t>(f)];
                    while (!F.over && F.now < c.sec) {
                        if (F.now >= F.nextPump && !F.ctl.empty()) { F.nextPump += TM::pump; P.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); }
                        if (c.script) c.script(F);
                        F.StepWorld(); if (F.move && c.watch) c.watch(F, p); } } };
                { std::vector<std::thread> th; for (int i = 0; i < std::max(1, threads); ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
                std::string o; const bool held = c.rule(res, o), ok = arm ? !held : held; ++checks; fails += !ok;
                std::printf("SPOT-TEST %-28s %-18s %s  %s%s\n", c.name, arm ? (std::string("fault ") + c.fault).c_str() : "the pass", ok ? (arm ? "FAILS AS IT MUST" : "HOLDS") : (arm ? "*** STILL HOLDS" : "*** DOES NOT HOLD"), o.c_str(), c.scripted && !arm ? "  [scripted ranking]" : "");
            }
            ActiveTuning() = base; }
        g_road = Road::Off; g_moveFault = &kMoveFaults[0];
        std::printf("SPOT-TESTS %s: %zu scenarios, %d checks, %d failed\n", fails ? "FAIL" : "PASS", cases.size(), checks, fails);
        return fails ? 1 : 0;
    }

}
