#pragma once
// iaus/MoveDump.h -- THE GOLDEN VECTOR FORMAT of the movement pass (S1): one window's inputs, the ray answers it got and the assignments it made, as text.
// PURE, header only. Writers: the harness now, `sim --pass-dump` later (S2 / G3). Readers: the harness on any compiler. Floats are hexadecimal (exact).
// The tuning is ShippedTuning() plus the case's own `u` lines (name value, SetTuningScalar). The profiles are NOT in it: the replayer maps Member::classId
// (the harness: 0xFF no class, 1 its fixed test profile). A change of a t.spot* / spot curve rewrites the cases.
// The PATH CACHE is not in it either: a writer clears PartyPicture::paths before the window it dumps (a cached path is up to t.spotFactMoveU old).
// PartyPicture::sight is not in it: a dumped window ran on the shipped rules.
// Walls and the grid are rebuilt from the mesh. TO ADD AN INPUT: a field at the END of its line's writer and reader, and raise kVersion.
#include "iaus/MovePass.h"

#include <cstdio>
#include <istream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>

namespace MFO::Iaus::MoveDump {

    inline constexpr int kVersion = 3;

    struct Mesh {   // a View that owns its arrays
        std::vector<Env::Vec3> verts; std::vector<Env::Tri> tris; std::vector<Env::Door> doors; std::vector<Env::Wall> walls; std::vector<std::uint32_t> walk, weak; Env::Grid grid;
        bool interior = true; bool portalsOpen = false; std::uint64_t stamp = 1;
        void Build() {
            Env::BuildGrid(grid, verts, tris); Env::LabelComponents(tris, walk, weak);
            Env::View v = Get(); Env::BuildWalls(walls, v); Env::IndexWalls(grid, walls);
        }
        [[nodiscard]] Env::View Get() const { return { verts, tris, doors, walls, weak, walk, &grid, interior, portalsOpen, stamp }; }
    };
    struct Expect { Move::FormID who = 0; Move::SpotKey key = 0; int family = 0; std::uint32_t form = 0, target = 0; float score = 0.0f; };
    struct Case { std::string name; Mesh mesh; Move::PartyPicture pic; std::vector<Move::RayAnswer> answers; std::vector<Expect> expect; std::vector<std::pair<std::string, float>> tuning; };

    namespace detail {
        struct Out {
            std::ostream& o;
            Out& operator<<(float v) { char b[48]; std::snprintf(b, sizeof b, " %a", static_cast<double>(v)); o << b; return *this; }
            Out& operator<<(double v) { char b[48]; std::snprintf(b, sizeof b, " %a", v); o << b; return *this; }
            Out& operator<<(const Env::Vec3& p) { return *this << p.x << p.y << p.z; }
            template <class I> requires(std::is_integral_v<I> || std::is_enum_v<I>) Out& operator<<(I v) { o << ' ' << static_cast<long long>(v); return *this; }
        };
        struct In {
            std::istringstream i; bool ok = true;
            std::string Tok() { std::string s; if (!(i >> s)) ok = false; return s; }
            In& operator>>(float& v) { v = std::strtof(Tok().c_str(), nullptr); return *this; }
            In& operator>>(double& v) { v = std::strtod(Tok().c_str(), nullptr); return *this; }
            In& operator>>(Env::Vec3& p) { return *this >> p.x >> p.y >> p.z; }
            template <class I> requires(std::is_integral_v<I> || std::is_enum_v<I>) In& operator>>(I& v) { v = static_cast<I>(std::strtoll(Tok().c_str(), nullptr, 10)); return *this; }
        };
        inline void Act(Out& o, const RankedAction& a) { o << a.family << a.scrollOf << a.form << a.target << a.score << a.reach << a.melee << a.sight; }
        inline void Act(In& i, RankedAction& a) { i >> a.family >> a.scrollOf >> a.form >> a.target >> a.score >> a.reach >> a.melee >> a.sight; }
    }

    // a_pic as it stood BEFORE Freeze. a_answers: what Answer was given. a_as: what Phase3 returned.
    inline void Write(std::ostream& f, const std::string& a_name, const Mesh& a_m, const Move::PartyPicture& a_pic, const std::vector<Move::RayAnswer>& a_answers, const std::vector<Move::Assignment>& a_as, bool a_sameMesh = false,
        const std::vector<std::pair<std::string, float>>& a_tuning = {}) {
        detail::Out o{ f };
        const auto line = [&](const char* k) { f << '\n' << k; };
        f << "case " << a_name << ' ' << kVersion;
        if (a_sameMesh) line("same");   // the mesh of the case before
        else {
        line("view"); o << a_m.interior << a_m.portalsOpen << a_m.stamp;
        for (const auto& v : a_m.verts) { line("v"); o << v; }
        for (const auto& t : a_m.tris) { line("t"); for (auto x : t.v) o << x; for (auto x : t.nb) o << x; for (auto x : t.kind) o << x; o << t.flags << t.door; }
        for (const auto& d : a_m.doors) { line("d"); o << d.tri << d.state << d.key << d.id << d.leaf << d.ax << d.ay << d.bx << d.by << d.z0 << d.z1; }
        }
        for (const auto& [n, x] : a_tuning) { line("u"); f << ' ' << n; o << x; }
        line("pic"); o << a_pic.envStamp << a_pic.t << a_pic.caps.point << a_pic.caps.hold << a_pic.caps.pin << a_pic.caps.facing << a_pic.caps.gait << a_pic.exterior;
        for (const auto& m : a_pic.party) {
            line("m"); o << m.id << m.p << m.ageSec << m.decides << m.classId << m.kinds << m.speed << m.anchor << m.leash << m.wantsDark << (m.marks | m.seen)
                         << m.who.radius << m.who.opensDoors << m.who.unlocks << m.who.large << m.who.drops << m.who.maxDrop << m.sneakSkill << m.invisible;
            for (std::size_t k = 0; k < m.nTop; ++k) { line("a"); detail::Act(o, m.top[k]); }
            if (m.commit.on) { line("c"); o << m.commit.key << m.commit.p << m.commit.tri << m.commit.kinds << m.commit.at; detail::Act(o, m.commit.act); detail::Act(o, m.commit.held); }
        }
        for (const auto& e : a_pic.foes) { line("f"); o << e.id << e.p << e.ageSec << e.target << e.faceX << e.faceY << e.skill; }
        for (const auto& r : a_pic.areas) { line("r"); o << r.p << r.r; }
        for (const auto& l : a_pic.light) { line("l"); o << l.key << l.level; }
        for (const auto& s : a_pic.lightField.samples) { line("i"); o << s.p << s.level << s.t << s.source; }   // S4
        if (!a_pic.lightField.samples.empty()) { line("g"); Light::EachFormulaNumber(a_pic.detect, [&](const float& v) { o << v; }); }
        for (const auto& [k, x] : a_pic.facts) { line("k"); o << k.first << k.second << x.sees << x.t << x.actorTri << x.actorAt << x.spotAt; }
        for (const auto& q : a_pic.carried) { line("q"); o << q.spot << q.p << q.viewer; }
        for (const auto& q : a_pic.late) { line("b"); o << q.spot << q.p << q.viewer << q.t << q.viewerAt << q.viewerTri; }
        for (const auto& m : a_pic.trust.marks) { line("w"); o << m.sign << m.who << m.region << m.t << m.at << m.dx << m.dy << m.a << m.b << m.leg; }   // S3
        for (const auto& r : a_pic.trust.refused) { line("z"); o << r.who << r.spot << r.t; }
        for (const auto& q : a_pic.trust.trail) { line("y"); o << q.p << q.t; }
        for (const auto& a : a_answers) { line("n"); o << a.spot << a.viewer << a.sees << a.t << a.viewerAt; }
        for (const auto& a : a_as) { line("x"); o << a.who << a.key << a.action << a.form << a.target << a.score; }
        f << "\nend\n";
    }

    // The next case, or null at the end of the stream / on a line it does not know (a_err says which).
    inline std::unique_ptr<Case> Read(std::istream& f, std::string& a_err, const Mesh* a_before = nullptr) {
        std::unique_ptr<Case> c; std::string row; bool same = false;
        while (std::getline(f, row)) {
            detail::In i{ std::istringstream(row) };
            std::string k; if (!(i.i >> k)) continue;
            if (k == "case") { c = std::make_unique<Case>(); c->name = i.Tok(); int ver = 0; i >> ver; if (ver != kVersion) { a_err = "version"; return nullptr; } continue; }
            if (!c) { a_err = "no case: " + row; return nullptr; }
            if (k == "end") { if (!same) c->mesh.Build(); Move::WallVerts(c->mesh.Get(), c->pic.wallVert); return c; }
            auto& P = c->pic;
            if (k == "same") { if (!a_before) { a_err = "same: no case before"; return nullptr; } c->mesh = *a_before; same = true; }
            else if (k == "view") i >> c->mesh.interior >> c->mesh.portalsOpen >> c->mesh.stamp;
            else if (k == "v") { Env::Vec3 p; i >> p; c->mesh.verts.push_back(p); }
            else if (k == "t") { Env::Tri t; for (auto& x : t.v) i >> x; for (auto& x : t.nb) i >> x; for (auto& x : t.kind) i >> x; i >> t.flags >> t.door; c->mesh.tris.push_back(t); }
            else if (k == "d") { Env::Door d; i >> d.tri >> d.state >> d.key >> d.id >> d.leaf >> d.ax >> d.ay >> d.bx >> d.by >> d.z0 >> d.z1; c->mesh.doors.push_back(d); }
            else if (k == "pic") i >> P.envStamp >> P.t >> P.caps.point >> P.caps.hold >> P.caps.pin >> P.caps.facing >> P.caps.gait >> P.exterior;
            else if (k == "m") { Move::Member m; i >> m.id >> m.p >> m.ageSec >> m.decides >> m.classId >> m.kinds >> m.speed >> m.anchor >> m.leash >> m.wantsDark >> m.marks
                                                   >> m.who.radius >> m.who.opensDoors >> m.who.unlocks >> m.who.large >> m.who.drops >> m.who.maxDrop >> m.sneakSkill >> m.invisible; P.party.push_back(m); }
            else if (k == "a" && !P.party.empty() && P.party.back().nTop < 3) { auto& m = P.party.back(); detail::Act(i, m.top[m.nTop++]); }
            else if (k == "c" && !P.party.empty()) { auto& x = P.party.back().commit; x.on = true; i >> x.key >> x.p >> x.tri >> x.kinds >> x.at; detail::Act(i, x.act); detail::Act(i, x.held); }
            else if (k == "f") { Move::Foe e; i >> e.id >> e.p >> e.ageSec >> e.target >> e.faceX >> e.faceY >> e.skill; P.foes.push_back(e); }
            else if (k == "r") { Move::Area r; i >> r.p >> r.r; P.areas.push_back(r); }
            else if (k == "l") { Move::LightAt l; i >> l.key >> l.level; P.light.push_back(l); }
            else if (k == "i") { Light::Sample s; i >> s.p >> s.level >> s.t >> s.source; P.lightField.samples.push_back(s); }
            else if (k == "g") Light::EachFormulaNumber(P.detect, [&](float& v) { i >> v; });
            else if (k == "k") { Move::PairKey key; Move::PairFact x; i >> key.first >> key.second >> x.sees >> x.t >> x.actorTri >> x.actorAt >> x.spotAt; P.facts[key] = x; }
            else if (k == "q") { Move::RayAsk q; i >> q.spot >> q.p >> q.viewer; P.carried.push_back(q); }
            else if (k == "b") { Move::RayAsk q; i >> q.spot >> q.p >> q.viewer >> q.t >> q.viewerAt >> q.viewerTri; P.late.push_back(q); }
            else if (k == "w") { Trust::Mark m; i >> m.sign >> m.who >> m.region >> m.t >> m.at >> m.dx >> m.dy >> m.a >> m.b >> m.leg; if (m.sign >= Trust::Sign::kCount) { a_err = "sign: " + row; return nullptr; } P.trust.marks.push_back(m); P.trust.gen = 1; }
            else if (k == "z") { Trust::Refusal r; i >> r.who >> r.spot >> r.t; P.trust.refused.push_back(r); }
            else if (k == "y") { Trust::Trail q; i >> q.p >> q.t; P.trust.trail.push_back(q); }
            else if (k == "u") { const std::string n = i.Tok(); float x = 0.0f; i >> x; c->tuning.push_back({ n, x }); }
            else if (k == "n") { Move::RayAnswer a; i >> a.spot >> a.viewer >> a.sees >> a.t >> a.viewerAt; c->answers.push_back(a); }
            else if (k == "x") { Expect e; i >> e.who >> e.key >> e.family >> e.form >> e.target >> e.score; c->expect.push_back(e); }
            else { a_err = "unknown line: " + row; return nullptr; }
            if (!i.ok) { a_err = "short line: " + row; return nullptr; }
        }
        return nullptr;
    }
}
