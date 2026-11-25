#pragma once
// iaus_sim/SpotTest.h -- S2 / S3: the scenarios of iaus-sim-v4.md 8.5 and of trust (8.9) as ASSERTED OUTCOMES over the movement pass (`sim --spot-test`). HOST-ONLY.
//
// Each row of kSpotCases is played on its hand-made fixture with --position-road point (no pin kinds, except the doorway, which says so), and must hold. Then
// the same fights are played with ONE mechanism of the pass broken (kMoveFaults) and the rule must NOT hold: a rule that passes broken tests nothing.
// The bodies a rule does not judge are statues (run speed 0, a deep health pool), so what moves and what it reaches is the followers' own doing.
// SCRIPTED rows replace his own ranking by a fixed one after each lap: the rule is then about the PLACE, for an action he does not pick by himself in
// that fight (marked in the row; a heal past its reach is no candidate of Generate.cpp at all, iaus-sim-v4.md 17.6).
#include "iaus_sim/Oracle.h"
#include "iaus_sim/Scenarios.h"

#include <functional>
#include <map>
#include <thread>

namespace Sim {

    // Fights an arm. 12 put the rules on the edge of chance (S2 review F1: 3 of 6 fresh seeds failed with nothing wrong). At 200 fights lost-sight's
    // true rate is 94 to 97 percent (what is left is the cast claim that closes unfired, not the place), so at 48 its 80 percent rule is over four standard errors away.
    inline int g_spotFights = 48;
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
            const int n = static_cast<int>(r.size()), hit = Count(r, [&](const SpotProbe& p) { return Get(p, "hit", 1e9) <= within; }), filed = Count(r, [](const SpotProbe& p) { return Get(p, "filed", 0) > 0; }); const double rn = Sum(r, "reach.n"), bl = Sum(r, "blind") / n;
            o = Fmt("first hit within %.0f s in %d of %d, a place filed in %d, places reached %.0f, held without sight %.2f s a fight, hold-unreachable %.2f s", within, hit, n, filed, rn, bl, Sum(r, "holdUn") / n);
            return hit * 10 >= n * 8 && filed * 2 >= n &&   /* not vacuous: the FIGHTS in which the pass filed a place (places reached counted one fight's many) */ bl <= 1.5 && Sum(r, "holdUn") / n <= 1.0; };
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
          // GEOMETRY, not PortalsAt (the function under test): the one square of floor between the two rooms (map column 10, row 4 of 9) and 100 units round it, beyond which a body leaves the way free
          c.watch = [](Fight& F, SpotProbe& p) { const auto in = [](const EQ::Vec3& q) { return std::hypot(q.x - std::clamp(q.x, 1280.0f, 1408.0f), q.y - std::clamp(q.y, 512.0f, 640.0f)) < 100.0f; }; int n = 0, stand = 0;
              for (std::size_t i = 0; i < 2; ++i) { const MoveClaim& q = F.move->claim[i]; if (q.on && in(q.p)) ++n; const Body& x = F.b[static_cast<std::size_t>(F.ctl[i].body)]; stand += in({ x.c.x, x.c.y, x.c.z }); }
              if (n) p["one"] = 1; if (n > 1) p["both"] = 1; if (stand) p["stand"] = 1; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()); o = Fmt("a place in the gap in %.0f of %d (stood in it in %.0f), two places in it in %.0f", Sum(r, "one"), n, Sum(r, "stand"), Sum(r, "both")); return Sum(r, "one") * 10 >= n * 9 && Sum(r, "both") == 0; }; v.push_back(c); }
        { SpotCase c{ "peel-bow-in-hand", "env-tower-stairs", "melee-hands" }; c.scripted = true; c.sec = 12.0; c.what = "a warrior with his bow in hand is never sent between an attacker and the player he stands on: a body blocks, a bow shoots (scripted: he ranks the shot at an attacker he sees)";
          c.setup = [](Fight& F) { AllStatues(F); Put(F, F.b[0], At(F, 10, 8, 11)); Put(F, *Foes(F)[0], At(F, 11, 8, 11)); Put(F, Sub(F), At(F, 4, 8, 11)); F.ctl[0].leash = 0.0f; F.ctl[0].profile.spot[static_cast<std::size_t>(MFO::Iaus::SpotKind::Peel)] = 4.0f; /* wanted in both arms: only the rule keeps him off it */ };
          c.script = [](Fight& F) { Script(F, 0, { Want(Family::Attack, Foes(F)[0]->c.id, 0.8f, 1800.0f, Sight::Visible, false) }); };
          c.watch = [](Fight& F, SpotProbe& p) { const MoveClaim& q = F.move->claim[0]; p["win"] = F.move->pic.window; const Body& f = *Foes(F)[0]; const EQ::Vec3 mid{ 0.5f * (F.b[0].c.x + f.c.x), 0.5f * (F.b[0].c.y + f.c.y), F.b[0].c.z };
              if (q.on && (q.kinds & MV::KindBit(MFO::Iaus::SpotKind::Peel))) p["peel"] = 1; if (q.on && MV::Dist(q.p, mid) < 250.0f) p["between"] = 1; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()), won = Count(r, [](const SpotProbe& p) { return Get(p, "win", 0) >= 10; }); o = Fmt("windows decided in %d of %d, a peel place in %.0f, a place between them in %.0f", won, n, Sum(r, "peel"), Sum(r, "between")); return won == n && Sum(r, "peel") == 0 && Sum(r, "between") == 0; }; v.push_back(c); }
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
        // ── TRUST (S3, 8.9): the truth differs from the mesh the pass plans on. One scripted bow, one target he has to go through the bad place for. Measured on the
        // CLAIMS: does a claim's planned way cross the bad place (`dep`). Repaired at kRepairAt, then the player walks it. No mechanism of the pass is told any of this.
        static constexpr double kRepairAt = 25.0, kRepairLate = 34.0, kDecaySec = 30.0;
        struct Bad { float x, y0, y1; };                                            // the line the bad place lies on
        const auto crosses = [](const MoveClaim& q, const Bad& b, int way /* +1 east only, -1 west only, 0 both */) { if (!q.on || q.via.empty()) return false;
            for (std::size_t i = 0; i < q.via.size(); ++i) { const auto &p = i ? q.via[i - 1] : q.from, &r = q.via[i]; if ((p.x < b.x) == (r.x < b.x) || (way > 0 && r.x < p.x) || (way < 0 && r.x > p.x)) continue; const float k = (b.x - p.x) / (r.x - p.x), y = p.y + (r.y - p.y) * k; if (y >= b.y0 - 70.0f && y <= b.y1 + 70.0f) return true; }
            return false; };
        const auto walkPlayer = [](Fight& F, float x0, float x1, float y, double t0) {  // the player walks the place, 300 u/s
            const double s = F.now - t0; if (s < 0 || s > std::fabs(x1 - x0) / 300.0) return; Body& p = F.b[0]; p.c.x = x0 + (x1 - x0) * static_cast<float>(s * 300.0 / std::fabs(x1 - x0)); p.c.y = y; p.c.z = 0.0f; EQ::Vec3 on; p.nav.tri = EQ::NearestTri(EnvViewOf(F), { p.c.x, p.c.y, 0.0f }, 100.0f, on); };
        const auto watchTrust = [crosses](Fight& F, SpotProbe& p, const Bad& b, int way, double repairAt = 25.0) { p["repair"] = repairAt; const MoveClaim& q = F.move->claim[0]; const bool dep = crosses(q, b, way); const double key = static_cast<double>(q.key % 1000003u) + q.since * 1e-3;
            if (F.t.trustMarks > 0 && !p.v.count("sign")) p["sign"] = F.now; if (dep) { p["depAny"] = 1; if (p["_dep"] != key) { p["_dep"] = key; p["deps"] += 1; if (F.now >= repairAt + 2.0 && !p.v.count("again")) p["again"] = F.now; }
                if (F.now < repairAt && p.v.count("sign")) p["lastDep"] = F.now; }
            if (F.now < repairAt) p["osc"] = static_cast<double>(F.t.pathOsc) * 60.0 / std::max(1.0, F.now);
            int dis = 0, dup = 0; const auto& L = F.move->trustLog; for (std::size_t i = 0; i < L.size(); ++i) { dis += L[i].level == TR::Level::Distrusted; for (std::size_t j = 0; j < i; ++j) dup += L[i].region == L[j].region && L[i].level == L[j].level; } p["dis"] = dis; p["dup"] = dup; p["lines"] = static_cast<double>(L.size()); };
        // recovery: from the first sign to the last moment a claim still depended on the place (before the repair). Band: kPathBounds osc/min 8.
        const auto ruleTrust = [](const std::vector<SpotProbe>& r, double bound, std::string& o) { const int n = static_cast<int>(r.size()), sign = Count(r, [](const SpotProbe& p) { return p.v.count("sign") > 0 && p.v.at("sign") < p.v.at("repair"); }); double worst = 0, sum = 0, osc = 0; for (const auto& p : r) { const double k = std::max(0.0, Get(p, "lastDep", 0) - Get(p, "sign", 1e9)); worst = std::max(worst, k); sum += k; osc = std::max(osc, Get(p, "osc", 0)); }
            const int in = Count(r, [bound](const SpotProbe& p) { return Get(p, "lastDep", 0) - Get(p, "sign", 1e9) <= bound; }), again = Count(r, [](const SpotProbe& p) { return Get(p, "again", 1e9) <= p.v.at("repair") + 2.0 + kDecaySec; });
            o = Fmt("a sign in %d of %d, recovered within %.0f s in %d (mean %.1f s, worst %.1f s), worst osc/min %.1f, distrusted lines a fight %.1f, said twice %.0f, used again within a decay of the walk in %d", sign, n, bound, in, sum / n, worst, osc, Sum(r, "dis") / n, Sum(r, "dup"), again);
            return sign * 10 >= n * 9 && in * 10 >= n * 9 && osc <= 8.0 && Sum(r, "dup") == 0 && Sum(r, "dis") >= n * 0.9 && again * 10 >= n * 9; };
        const auto bow = [](Fight& F) { Body& f = *Foes(F)[0]; Script(F, 0, { Want(Family::Attack, f.c.id, 0.8f, 800.0f, EnvSees(F, Sub(F), f) ? Sight::Visible : Sight::Occluded, false) }); };
        const auto stage = [](Fight& F, EQ::Vec3 me, EQ::Vec3 foe, EQ::Vec3 pc) { AllStatues(F); for (Body* f : Foes(F)) Put(F, *f, foe); for (std::size_t i = 1; i < Foes(F).size(); ++i) Foes(F)[i]->alive = false; Put(F, Sub(F), me); Put(F, F.b[0], pc); F.ctl[0].leash = 0.0f; for (std::size_t i = 1; i < F.ctl.size(); ++i) F.b[static_cast<std::size_t>(F.ctl[i].body)].alive = false;
            F.ctl[0].drawn = PostureKind::Bow; Sub(F).rangedMode = true; F.ctl[0].gate.weaponBow = true; };
        { SpotCase c{ "trust-ignored-wall", "trust-ignored-wall", "no-trust" }; c.scripted = true; c.sec = 58.0; c.what = "the mesh walks through a wall that is really there, the engine reads the same mesh: he stops being sent into it";
          const Bad b{ 1290.0f, 640.0f, 768.0f }; c.setup = [=](Fight& F) { stage(F, At(F, 7, 3, 8), At(F, 18, 2, 8), At(F, 2, 5, 8)); };
          c.script = [=](Fight& F) { bow(F); if (F.now >= kRepairAt && !F.meshEdit) EnvRepair(F); walkPlayer(F, 1100.0f, 1460.0f, 704.0f, kRepairAt + 0.5); };
          c.watch = [=](Fight& F, SpotProbe& p) { watchTrust(F, p, b, 0); }; c.rule = [=](const auto& r, std::string& o) { return ruleTrust(r, 12.0, o); }; v.push_back(c); }
        { SpotCase c{ "trust-buried-patch", "env-doorway-block", "no-trust" }; c.scripted = true; c.sec = 58.0; c.what = "a patch of mesh under a wall the rays see and the mesh does not (the one gap between two rooms)";
          const Bad b{ 1344.0f, 512.0f, 640.0f }; c.setup = [=](Fight& F) { stage(F, At(F, 7, 5, 9), At(F, 18, 2, 9), At(F, 2, 6, 9));
              EnvEdit(F, [](const EQ::View& w, std::int32_t i, std::int32_t j) { const auto in = [&](std::int32_t k) { const auto q = EQ::Centroid(w, k); return q.x > 1280.0f && q.x < 1408.0f; }; return in(i) != in(j); }, { { 1344.0f, 512.0f, 0.0f, 1344.0f, 640.0f, 0.0f, -24.0f, 300.0f, EQ::WallKind::Face } }); };
          c.script = [=](Fight& F) { bow(F); if (F.now >= kRepairAt) EnvRepair(F); walkPlayer(F, 1100.0f, 1560.0f, 576.0f, kRepairAt + 0.5); };
          c.watch = [=](Fight& F, SpotProbe& p) { watchTrust(F, p, b, 0); }; c.rule = [=](const auto& r, std::string& o) { return ruleTrust(r, 12.0, o); }; v.push_back(c); }
        { SpotCase c{ "trust-missing-link", "env-doorway-block", "no-trust" }; c.scripted = true; c.sec = 58.0; c.what = "a link goes after the plan was made and the engine knows: it refuses the paths we find";
          const Bad b{ 1344.0f, 512.0f, 640.0f }; c.setup = [=](Fight& F) { stage(F, At(F, 7, 5, 9), At(F, 18, 2, 9), At(F, 2, 6, 9)); F.engineTrue = true; };
          c.script = [=](Fight& F) { bow(F); if (F.now >= 1.6 && F.now < kRepairAt && !F.meshEdit) EnvEdit(F, [](const EQ::View& w, std::int32_t i, std::int32_t j) { return (EQ::Centroid(w, i).x < 1280.0f) != (EQ::Centroid(w, j).x < 1280.0f); });
              if (F.now >= kRepairAt) EnvRepair(F); walkPlayer(F, 1100.0f, 1560.0f, 576.0f, kRepairAt + 0.5); };
          c.watch = [=](Fight& F, SpotProbe& p) { watchTrust(F, p, b, 0); }; c.rule = [=](const auto& r, std::string& o) { return ruleTrust(r, 12.0, o); }; v.push_back(c); }
        { SpotCase c{ "trust-one-way", "trust-one-way", "no-trust" }; c.scripted = true; c.sec = 68.0; c.what = "an edge walkable one way only and the engine knows: it takes him round, twice, and the plan follows it round (the target crosses every 4 s, so he has to)";
          const Bad b{ 1290.0f, 640.0f, 768.0f }; c.setup = [=](Fight& F) { stage(F, At(F, 15, 2, 8), At(F, 3, 2, 8), At(F, 20, 5, 8)); F.engineTrue = true; };
          c.script = [=](Fight& F) { Body& f = *Foes(F)[0]; const bool west = std::fmod(F.now, 8.0) < 4.0; const bool isWest = f.c.x < 1280.0f; if (west != isWest) Put(F, f, west ? At(F, 3, 2, 8) : At(F, 18, 2, 8));
              Script(F, 0, { Want(Family::Attack, f.c.id, 0.8f, 700.0f, EnvSees(F, Sub(F), f) ? Sight::Visible : Sight::Occluded, false) });
              if (F.now >= kRepairLate && !F.meshEdit) EnvRepair(F); walkPlayer(F, 1460.0f, 1100.0f, 704.0f, kRepairLate + 0.5); };
          c.watch = [=](Fight& F, SpotProbe& p) { watchTrust(F, p, b, -1, kRepairLate); }; c.rule = [=](const auto& r, std::string& o) { return ruleTrust(r, 20.0, o); }; v.push_back(c); }
        // one blocked leg in a clean place: nothing is distrusted and he goes on (the 11-17 GATED lesson)
        { SpotCase c{ "trust-one-blocked-leg", "env-doorway-block", "hair-trust" }; c.scripted = true; c.sec = 20.0; c.what = "a clean place, something stands in the gap for a moment and one leg ends STUCK: the region is not distrusted and he goes through it";
          const Bad b{ 1344.0f, 512.0f, 640.0f }; c.setup = [=](Fight& F) { stage(F, At(F, 7, 5, 9), At(F, 18, 2, 9), At(F, 2, 6, 9)); EnvEdit(F, [](const EQ::View& w, std::int32_t i, std::int32_t j) { return (EQ::Centroid(w, i).x < 1280.0f) != (EQ::Centroid(w, j).x < 1280.0f); }); };
          c.script = [=](Fight& F) { bow(F); if (!F.move) return; const auto& mk = F.move->pic.trust.marks; if (F.meshEdit && !mk.empty() && F.now >= mk[0].t + 1.0) EnvRepair(F); };   // gone again a second after his first STUCK end, before a second one
          c.watch = [=](Fight& F, SpotProbe& p) { watchTrust(F, p, b, 0); if (Sub(F).c.x > 1344.0f && F.move->claim[0].on && !p.v.count("over")) p["over"] = F.now; };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()), over = Count(r, [](const SpotProbe& p) { return Get(p, "over", 1e9) <= 15.0; }), s = Count(r, [](const SpotProbe& p) { return p.v.count("sign") > 0; }); o = Fmt("one leg ended STUCK in %d of %d, his claim takes him through the gap within 15 s in %d, distrusted lines %.0f, lines %.0f", s, n, over, Sum(r, "dis"), Sum(r, "lines")); return s * 10 >= n * 9 && over * 10 >= n * 9 && Sum(r, "dis") == 0; }; v.push_back(c); }
        // S2 review F6
        { SpotCase c{ "blocked-spot", "env-doorway-block", "no-memory" }; c.scripted = true; c.sec = 12.0; c.what = "a leg that ended BLOCKED: the same spot is not filed again for him in the next seconds";
          c.setup = [=](Fight& F) { stage(F, At(F, 7, 5, 9), At(F, 18, 2, 9), At(F, 2, 6, 9)); };
          c.script = [=](Fight& F) { bow(F); Body& x = Sub(F); if (!F.move) return; MoveClaim& q = F.move->claim[0]; if (q.on && !q.arrived && F.now > 2.0 && F.move->pic.trust.refused.empty() && !F.t.spotBlocked) { q.walks = true; EnvLegEnd(F, x, LegEnd::Blocked); q.walks = false; } };
          c.watch = [](Fight& F, SpotProbe& p) { const MoveClaim& q = F.move->claim[0]; if (!q.on && F.t.spotBlocked && !p.v.count("key")) { p["key"] = static_cast<double>(q.key); p["at"] = F.now; } else if (q.on && p.v.count("key") && F.now < p["at"] + 5.0) { p["next"] = 1; if (static_cast<double>(q.key) == p["key"]) p["same"] = 1; } };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()); const int k = Count(r, [](const SpotProbe& p) { return p.v.count("key") > 0; }); o = Fmt("blocked in %d of %d, another claim within 5 s in %.0f, on the same spot in %.0f", k, n, Sum(r, "next"), Sum(r, "same")); return k * 10 >= n * 9 && Sum(r, "next") * 10 >= n * 9 && Sum(r, "same") == 0; }; v.push_back(c); }
        { SpotCase c{ "waiting-claim", "env-doorway-block", "any-leg" }; c.scripted = true; c.sec = 6.0; c.what = "a goal-less leg of another mover ends while a claim waits: the claim stays and trust hears nothing";
          c.setup = [=](Fight& F) { stage(F, At(F, 7, 5, 9), At(F, 18, 2, 9), At(F, 2, 6, 9)); };
          c.script = [=](Fight& F) { bow(F); Body& x = Sub(F); if (!F.move) return; MoveClaim& q = F.move->claim[0]; if (q.on && !q.arrived && F.now > 2.0 && x.perch == false && q.since > 0.0 && F.t.spotArrived >= 0 && F.t.spotBlocked == 0 && F.t.apprDrops == 0) { F.t.apprDrops = 1; x.nav.goalBody = 0; EnvLegEnd(F, x, LegEnd::Unreachable); F.t.apprBlocked = q.on ? 2 : 1; } };
          c.watch = [](Fight& F, SpotProbe& p) { if (F.t.apprBlocked && !p.v.count("did")) { p["did"] = 1; p["kept"] = F.t.apprBlocked == 2; p["marks"] = F.t.trustMarks; } };
          c.rule = [](const std::vector<SpotProbe>& r, std::string& o) { const int n = static_cast<int>(r.size()); o = Fmt("the other leg ended in %.0f of %d, the claim stayed in %.0f, marks %.0f", Sum(r, "did"), n, Sum(r, "kept"), Sum(r, "marks")); return Sum(r, "did") * 10 >= n * 9 && Sum(r, "kept") == Sum(r, "did") && Sum(r, "marks") == 0; }; v.push_back(c); }
        return v;
    }

    inline int RunSpotTests(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int threads) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); const std::vector<Scenario> all = AllScenarios(true, true, true); const int N = g_spotFights; int fails = 0, checks = 0;
        const Tuning* base = ActiveTuning(); const char* traced = std::getenv("SIM_SPOTTRACE");   // <case name>:<arm 0|1>:<fight>: one event trace (--threads 1)
        const std::vector<SpotCase> cases = SpotCases();
        const char* only = std::getenv("SIM_SPOTCASE");   // a scenario name's beginning: that one alone (diagnostics)
        for (const SpotCase& c : cases) { if (only && std::strncmp(c.name, only, std::strlen(only))) continue;
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
