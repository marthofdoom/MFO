#pragma once
// iaus_sim/ModeSet.h -- THE MODE SCENARIO SET of strict movement (tight-movement-plan.md 11 item 6): fights in which each order mode can be the best answer. HOST-ONLY.
//
// A CELL is one combination of the axes (kModeKinds x speed x count x terrain x party x player). ModeCell(i) enumerates ALL of them; ModeSpec builds the fight of one.
// `--mode-cover` builds every cell and MEASURES what the built fight is (ModeMeasure reads the bodies and the mesh, never the cell): the coverage table and its holes.
// `--mode-test [--mode-n N] [--mode-fights F]` plays a balanced sample of N cells with every class of kModeClasses under FORCED ARMS of the strict road, and counts which
// mode is best: the acceptance of the plan. With --env-set the set's own places are extra terrain rows.
//
// The oracle is not asked for orders under a forced arm (its order race is Oracle.h, S5-C). What is measured here is a TOURNAMENT OF CONSTANT
// RULES (kModeArms) over the same fights with his own IAUS choosing the actions: a lower bound on how often a mode is right, never an optimum.
//
// TO ADD: a foe kind = a kModeKinds row + its case in ModeFoes. A terrain = a fixture (envs/) + a kModeTerrain row. An arm = a kModeArms row + its case in ModeRule.
#include "iaus_sim/Scenarios.h"

#include <thread>

namespace Sim {

    enum class FoeKind : std::uint8_t { Heavy = 0, Pack, Duellist, Shield, Archers, BoltMage, AreaMage, StreamMage, Summoner, Charger, BossAdds, Flyer, kCount };
    inline constexpr const char* kModeKinds[] = { "heavy", "pack", "duellist", "shield", "archers", "bolt-mage", "area-mage", "stream-mage", "summoner", "charger", "boss-adds", "flyer" };
    inline constexpr const char* kModeSpeeds[] = { "slower", "equal", "faster" };
    inline constexpr const char* kModeCounts[] = { "1", "2-3", "6+" };
    struct ModeTerrain { const char* name; const char* fixture; };
    inline constexpr ModeTerrain kModeTerrain[] = { { "open", "mode-open" }, { "corridor", "narrow" }, { "doorway", "doorway-open" }, { "pillars", "pillars" }, { "stairs", "mode-stairs" }, { "bridge", "mode-bridge" }, { "dead-end", "dead-end" }, { "slope", "mode-slope" }, { "water", "mode-water" }, { "door", "doorway" } };
    inline constexpr const char* kModeParty[] = { "alone", "tank", "healer", "two-ranged" };
    inline constexpr const char* kModePlayer[] = { "near", "at-leash", "leaving" };
    inline constexpr int kMK = static_cast<int>(std::size(kModeKinds)), kMS = 3, kMC = 3, kMT = static_cast<int>(std::size(kModeTerrain)), kMP = 4, kML = 3;
    inline constexpr int kModeCells = kMK * kMS * kMC * kMT * kMP * kML;
    inline constexpr const char* kModeClasses[] = { "warrior", "knight", "mercenary", "archer", "thief", "evoker", "healer", "conjurer" };
    struct Cell { int kind = 0, speed = 0, count = 0, terrain = 0, party = 0, player = 0; };
    [[nodiscard]] inline Cell ModeCell(int i) { Cell c; c.player = i % kML; i /= kML; c.party = i % kMP; i /= kMP; c.count = i % kMC; i /= kMC; c.speed = i % kMS; i /= kMS; c.terrain = i % kMT; i /= kMT; c.kind = i % kMK; return c; }
    [[nodiscard]] inline bool ModeCellNA(const Cell& c) { return static_cast<FoeKind>(c.kind) == FoeKind::BossAdds && c.count == 0; }   // a boss with adds is no single foe
    // The k-th cell of a balanced sample: every (kind, speed, terrain) once per 360, the other axes turned so that every pair of axes is met.
    [[nodiscard]] inline Cell ModeSample(int k) { Cell c; const int b = k % (kMK * kMS * kMT), r = k / (kMK * kMS * kMT); c.kind = b % kMK; c.speed = (b / kMK) % kMS; c.terrain = b / (kMK * kMS);
        c.count = (c.kind + c.speed + c.terrain + r) % kMC; c.party = (c.kind / 3 + 2 * c.speed + c.terrain + r) % kMP; c.player = (c.kind / 3 + c.terrain / 3 + 2 * c.speed + c.count + r / 4) % kML; if (ModeCellNA(c)) c.count = 1; return c; }

    [[nodiscard]] inline int ModeSpell(const Data& D, const std::function<bool(const SimSpell&)>& f) { for (std::size_t i = 0; i < D.foeSpells.size(); ++i) if (D.foeSpells[i].e.nature == Prune::Nature::Offense && D.foeSpells[i].school >= 0 && f(D.foeSpells[i])) return static_cast<int>(i); return -1; }
    inline constexpr float kModeRun = 370.0f;   // the subject's own run in the lab
    inline void ModeFoes(const Data& D, const Cell& cell, Rng& R, FightSpec& f) {
        const FoeKind kind = static_cast<FoeKind>(cell.kind); int n = cell.count == 0 ? 1 : cell.count == 1 ? R.I(2, 3) : 6; const float run = kModeRun * (cell.speed == 0 ? (R.P(0.5f) ? 0.8f : 0.38f) /* half of them a slow walker or a slowed foe: slower even than a backpedal (Residue::kiteSpeed), the one foe a kite can hold off */ : cell.speed == 1 ? 1.0f : 1.35f);
        if (kind == FoeKind::BossAdds) n = std::max(2, n);   // a boss alone is the heavy: that cell is not applicable
        const auto base = [&](std::initializer_list<const char*> roles, float hp, float dmg, const char* name) { auto s = std::make_shared<ActorSpec>(*Foe(D, roles, 24, R, hp, dmg, name)); s->run = run; s->conf = 4; return s; };
        const auto mage = [&](int sp, const char* name, float dmg = 1.0f) { auto s = base({ "bandit_mage", "mage" }, 1.2f, dmg, name); if (sp >= 0) s->spells = { sp }; s->st.wMagic = 3.0f; s->mp = std::max(s->mp, 300.0f); s->ranged = Weapon{}; return s; };
        for (int i = 0; i < n; ++i) {
            std::shared_ptr<ActorSpec> s; std::string tag = "grunt";
            switch (kind) {
            case FoeKind::Heavy:      s = base({ "giant", "troll", "draugr" }, 1.6f, 1.6f, "Heavy"); s->creature = true; s->mass = std::max(s->mass, 2.5f); s->unarmedReach = std::max(s->unarmedReach, 180.0f); break;
            case FoeKind::Pack:       s = base({ "wolf" }, 1.0f, 1.0f, "Pack"); break;
            case FoeKind::Duellist:   s = base({ "bandit" }, 1.3f, 1.2f, "Duellist"); s->shield = false; break;
            case FoeKind::Shield:     s = base({ "bandit", "draugr" }, 1.2f, 0.9f, "Shield"); s->shield = true; s->st.def = 2.0f; break;
            case FoeKind::Archers:    s = base({ "bandit_archer", "draugr_archer" }, 1.0f, 1.0f, "Archer"); tag = "archer"; break;
            case FoeKind::BoltMage:   s = mage(ModeSpell(D, [](const SimSpell& x) { return !x.e.concentration && x.e.areaUnits <= 0; }), "Bolt"); tag = "caster"; break;
            case FoeKind::AreaMage:   s = mage(ModeSpell(D, [](const SimSpell& x) { return !x.e.concentration && x.e.areaUnits > 0; }), "Blast"); tag = "caster"; break;
            case FoeKind::StreamMage: s = mage(ModeSpell(D, [](const SimSpell& x) { return x.e.concentration; }), "Stream"); tag = "caster"; break;
            case FoeKind::Summoner:   s = mage(ModeSpell(D, [](const SimSpell& x) { return !x.e.concentration && x.e.areaUnits <= 0; }), "Summoner", 0.6f); tag = "caster"; { auto a = base({ "flame_atronach", "frost_atronach" }, 0.5f, 0.6f, "Summoned"); f.late.push_back({ 4.0 + 2.0 * i, a }); f.lateTags.push_back("grunt"); } break;   // MODEL: what he calls joins at 4, 6, ... s
            case FoeKind::Charger:    s = base({ "wolf", "troll" }, 2.5f, 2.2f, "Charger"); s->animal = s->creature = true; s->mass = std::max(s->mass, 2.0f); break;
            case FoeKind::BossAdds:   if (i == 0) { s = base({ "bandit", "draugr" }, 4.0f, 1.8f, "Boss"); tag = "boss"; } else s = base({ "bandit", "wolf" }, 0.7f, 0.7f, "Add"); break;
            case FoeKind::Flyer:      s = base({ "dragon" }, i ? 0.4f : 0.7f, 0.6f, "Flyer"); tag = "boss"; break;
            default: break; }
            f.customFoes.push_back(s); f.foeTags.push_back(tag);
        }
    }
    [[nodiscard]] inline FightSpec ModeSpec(const Data& D, const std::vector<SimClass>& cls, const char* slug, const Cell& cell, std::uint64_t seed, const SimEnv* real = nullptr) {
        Rng R; R.g.seed(seed); FightSpec f; f.cls = ClassBySlug(cls, slug); f.level = 25; f.rank = 3; f.playerFam = static_cast<Cls>(R.I(0, 2)); f.occluders = 0; f.seed = R.g();
        static constexpr float kRatio[] = { 0.8f, 1.1f, 1.5f }; f.targetRatio = kRatio[R.I(0, 2)];
        if (real) { f.env = real; f.interior = real->interior; f.foeDist = R.U(450.0f, 1300.0f); } else { f.env = g_envPool.ByName(kModeTerrain[cell.terrain].fixture); f.envExact = true; f.foeDist = 1000; }
        ModeFoes(D, cell, R, f);
        if (cell.party == 1) f.others.push_back(ClassBySlug(cls, std::strcmp(slug, "knight") ? "knight" : "warrior"));
        else if (cell.party == 2) { f.others.push_back(ClassBySlug(cls, "healer")); f.ensureOthers = { "healOther", "heal" }; }
        else if (cell.party == 3) f.others.push_back(ClassBySlug(cls, "archer"));
        const int on = R.I(0, 2); if (on == 0) f.onSubject = true; else if (on == 1) f.foesOnPlayer = true;
        if (f.cls->gate.spellsOnClass) f.ensure = { "fire", "heal" };
        f.freeEntry = 0; return f;
    }
    // The cell's player axis, on the built fight.
    inline void ModePlayer(Fight& F, const Cell& cell) {
        Body& p = F.b[0]; const Body& s = F.b[static_cast<std::size_t>(F.ctl[0].body)];
        if (cell.player == 2) p.retreatUntil = 10.0;   // he walks off for ten seconds
        if (cell.player != 1) return;
        const float want = std::min(900.0f, F.ctl[0].leash > 0.0f ? 0.8f * F.ctl[0].leash : 900.0f); const EQ::View v = EnvViewOf(F); MV::Work::Flood f; Env::Walker who; const std::int32_t t0 = s.nav.tri; if (t0 < 0) return;
        MV::Flood(v, { s.c.x, s.c.y, s.c.z }, t0, who, 1800.0f, 4096, f); float best = 1e9f; EQ::Vec3 at; std::int32_t bt = -1;
        for (const auto& [t, d] : f.d) { const EQ::Vec3 q = EQ::Centroid(v, t); float foe = 1e9f; for (const Body& o : F.b) if (!o.Party()) foe = std::min(foe, std::hypot(o.c.x - q.x, o.c.y - q.y)); const float c = std::fabs(d - want) + (foe < 500.0f ? 2.0f * (500.0f - foe) : 0.0f); if (c < best) { best = c; at = q; bt = t; } }
        if (bt >= 0) { p.c.x = at.x; p.c.y = at.y; p.c.z = at.z; p.nav = Nav{}; p.nav.tri = bt; }
    }

    // WHAT A BUILT FIGHT IS, read from the fight alone.
    struct Measured { Cell c; bool ok = true; std::string why; float ratio = 0, dz = 0, width = 0, water = 0, doors = 0, open = 0; };
    [[nodiscard]] inline Measured ModeMeasure(const Fight& F, const std::vector<std::string>& names) {
        Measured m; std::vector<const Body*> foes; for (const Body& o : F.b) if (o.role == Role::Foe) foes.push_back(&o);
        const Body& s = F.b[static_cast<std::size_t>(F.ctl[0].body)]; if (foes.empty()) { m.ok = false; m.why = "no foe"; return m; }
        // kind: by what the bodies are and carry
        const ActorSpec& a = *foes[0]->spec; const auto spell = [&]() -> const SimSpell* { for (int i : a.spells) { const SimSpell& x = F.D->foeSpells[static_cast<std::size_t>(i)]; if (x.e.nature == Prune::Nature::Offense) return &x; } return nullptr; }();
        float hp0 = a.hp, hpMax = 0; for (const Body* o : foes) if (o != foes[0]) hpMax = std::max(hpMax, o->spec->hp);
        FoeKind k = FoeKind::Duellist;
        if (a.dragon) k = FoeKind::Flyer; else if (foes.size() > 1 && hp0 > 3.0f * hpMax && !spell) k = FoeKind::BossAdds;
        else if (spell && !F.late.empty()) k = FoeKind::Summoner; else if (spell) k = spell->e.concentration ? FoeKind::StreamMage : spell->e.areaUnits > 0 ? FoeKind::AreaMage : FoeKind::BoltMage;
        else if (a.ranged.Has()) k = FoeKind::Archers; else if (a.shield) k = FoeKind::Shield; else if (a.animal && a.mass >= 2.0f) k = FoeKind::Charger; else if (a.creature && a.unarmedReach >= 180.0f) k = FoeKind::Heavy; else if (a.animal) k = FoeKind::Pack;
        m.c.kind = static_cast<int>(k);
        std::vector<float> run; for (const Body* o : foes) run.push_back(o->spec->run); std::sort(run.begin(), run.end()); m.ratio = run[run.size() / 2] / std::max(1.0f, s.spec->run); m.c.speed = m.ratio < 0.92f ? 0 : m.ratio > 1.1f ? 2 : 1;
        const std::size_t n = foes.size() + 0; m.c.count = n == 1 ? 0 : n <= 3 ? 1 : 2;
        bool tank = false, heal = false, bow = false; for (std::size_t i = 1; i < F.ctl.size(); ++i) { const Ctl& c = F.ctl[i]; if (c.drawn == PostureKind::Bow) bow = true; else if (c.gate.spellsOnClass) heal = true; else tank = true; }
        m.c.party = F.ctl.size() == 1 ? 0 : tank ? 1 : heal ? 2 : 3; (void)bow;
        const float pd = Fight::Dist(s, F.b[0]); m.c.player = F.now < F.b[0].retreatUntil ? 2 : pd > 500.0f ? 1 : 0;
        // the ground between them, from the mesh
        const EQ::View v = EnvViewOf(F); const EQ::Vec3 from{ s.c.x, s.c.y, s.c.z }, to{ foes[0]->c.x, foes[0]->c.y, foes[0]->c.z }; const std::int32_t t0 = s.nav.tri; EQ::Vec3 on;
        MV::Work::Flood f; MV::Flood(v, from, t0, Env::Walker{}, 4000.0f, 1 << 16, f);
        std::size_t wet = 0; for (const auto& [t, d] : f.d) wet += (v.tris[static_cast<std::size_t>(t)].flags & EQ::kTriWater) != 0; m.water = f.d.empty() ? 0.0f : 100.0f * static_cast<float>(wet) / static_cast<float>(f.d.size());
        m.dz = std::fabs(to.z - from.z); m.doors = static_cast<float>(F.doors.size());
        // narrowest: along the WALKED way, every 48 units, how far one can go left and right of it (the smallest sum)
        float narrow = 1e9f; float blockedAt = 0.0f; EQ::Path path; EQ::PathQuery q; q.from = from; q.fromTri = t0; q.to = to; q.toTri = foes[0]->nav.tri; q.who.opensDoors = true;
        if (t0 >= 0 && foes[0]->nav.tri >= 0 && EQ::FindPath(v, q, tl_envScratch, path)) { EQ::Vec3 at = from;
            for (const EQ::PathPoint& pt : path.pts) { const float l = std::hypot(pt.p.x - at.x, pt.p.y - at.y); if (l > 1.0f) for (float k = 24.0f; k < l; k += 48.0f) { const float ux = (pt.p.x - at.x) / l, uy = (pt.p.y - at.y) / l; const EQ::Vec3 p{ at.x + ux * k, at.y + uy * k, at.z + (pt.p.z - at.z) * k / l }; const std::int32_t pt2 = EQ::NearestTri(v, p, 200.0f, on); if (pt2 < 0) continue;
                    float w = 0; for (const float side : { -1.0f, 1.0f }) { const EQ::Walk e = EQ::WalkLine(v, pt2, on, { on.x - side * uy * 1500.0f, on.y + side * ux * 1500.0f, on.z }, Env::Walker{}); w += std::hypot(e.end.x - on.x, e.end.y - on.y); } narrow = std::min(narrow, w); }
                at = pt.p; } } else blockedAt = 1.0f;
        m.width = narrow; m.open = blockedAt;
        for (int t = 0; t < kMT; ++t) if (names.empty() || F.env->name == kModeTerrain[t].fixture) { m.c.terrain = t; break; }
        return m;
    }
    // The terrain's own signature, measured: what makes it that terrain must be on the mesh between or around the two groups.
    [[nodiscard]] inline bool ModeTerrainIs(int t, const Fight& F, const Measured& m) {
        const std::string n = kModeTerrain[t].name; bool leaf = !F.doors.empty();
        if (n == "open") return m.width > 800.0f && m.dz < 30.0f && m.water < 1.0f && !leaf;
        if (n == "corridor") return m.width < 120.0f && !leaf;
        if (n == "doorway") return m.width < 200.0f && !leaf;
        if (n == "pillars" ) return F.env->walls.size() > 60 && m.width > 60.0f && m.dz < 30.0f;
        if (n == "stairs") return m.dz > 150.0f && F.env->interior;
        if (n == "bridge") return m.width > 150.0f && m.width < 400.0f && !F.env->interior;
        if (n == "dead-end") return m.width > 250.0f && m.width < 800.0f && m.dz < 30.0f && !leaf;
        if (n == "slope") return m.dz > 150.0f && !F.env->interior;
        if (n == "water") return m.water > 20.0f;
        return leaf;   // door: a door leaf between them
    }

    // ── the forced arms ──
    struct ModeArm { const char* name; Force mode; int var; const char* what; };
    inline constexpr ModeArm kModeArms[] = {
        { "free", Force::Free, 0, "no order: the engine's own movement the whole fight" },
        { "stand:own", Force::Stand, 0, "stand where he started" },
        { "stand:pass", Force::Stand, 1, "the pass's own places and holds, every stay held" },
        { "stand:player", Force::Stand, 2, "stand beside the player's starting place" },
        { "stand:gap", Force::Stand, 3, "stand in the narrowest place near him" },
        { "kite:500", Force::Kite, 0, "kite while a foe is inside 500, else free" },
        { "kite:900", Force::Kite, 1, "... inside 900" },
        { "disengage:500", Force::Disengage, 0, "run off while a foe that targets him is inside 500, else free" },
        { "disengage:hurt", Force::Disengage, 1, "... only under half his health" },
        { "disengage:player", Force::Disengage, 2, "... to the player" },
    };
    inline constexpr const char* kModeNames[] = { "Free", "Stand", "Kite", "Disengage" };
    [[nodiscard]] inline bool ModeServed(const ModeArm& a) { return (a.mode != Force::Kite || g_moveCaps.kite) && (a.mode != Force::Disengage || g_moveCaps.disengage); }   // --move-caps: an arm whose mode is not served is not played (F9)
    [[nodiscard]] inline int ModeOf(const ModeArm& a) { return a.mode == Force::Free ? 0 : a.mode == Force::Stand ? 1 : a.mode == Force::Kite ? 2 : 3; }
    namespace ModeDetail {
        inline const EQ::Vec3* Persist(const MoveClaim& q, MV::MoveMode m) { return q.order.mode == m && q.phase != StrictPhase::Ended ? &q.order.p : nullptr; }
        // the place that puts him farthest from the foes: over his own walk out to `reach`, the foes' nearness against his own way there
        inline bool Away(const Fight& F, const Body& x, float reach, bool sight, EQ::Vec3& out, std::int32_t& tri) {
            const EQ::View v = EnvViewOf(F, true); if (x.nav.tri < 0) return false; MV::Work::Flood f; MV::Flood(v, { x.c.x, x.c.y, x.c.z }, x.nav.tri, WalkerOf(x), reach, 2048, f);
            const Ctl& c = F.ctl[static_cast<std::size_t>(x.ctl)]; const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId); float best = -1e9f; bool have = false;
            for (const auto& [t, d] : f.d) { if (d < 100.0f) continue; const EQ::Vec3 p = EQ::Centroid(v, t); if (c.leash > 0.0f && std::hypot(p.x - F.b[0].c.x, p.y - F.b[0].c.y) > c.leash) continue; float foe = 1e9f; for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o)) foe = std::min(foe, std::hypot(o.c.x - p.x, o.c.y - p.y));
                float s = foe - 0.35f * d; if (s <= best) continue; if (sight && tg && Fight::Up(*tg) && !EnvSeesPoint(F, { p.x, p.y, p.z + g_envT.eyeFrac * g_envT.bodyH }, *tg)) { s -= 400.0f; if (s <= best) continue; } best = s; out = p; tri = t; have = true; }
            return have; }
        inline float Nearest(const Fight& F, const Body& x, bool onHim) { float d = 1e9f; for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o) && (!onHim || o.c.targetId == x.c.id)) d = std::min(d, Fight::Dist(x, o)); return d; }
    }
    // What the arm orders this window, for the subject. o comes in as the pass's own order.
    inline void ModeRule(Fight& F, MV::MoveOrder& o) {
        using namespace ModeDetail; MoveState& S = *F.move.s; Body& x = F.b[static_cast<std::size_t>(F.ctl[0].body)]; MoveClaim& q = S.claim[0]; const Tuning& t = *ActiveTuning();
        const auto order = [&](MV::MoveMode m, const EQ::Vec3& p, std::int32_t tri) { MV::MoveOrder r; r.mode = m; r.p = p; r.tri = tri; r.leashR = t.orderLeash; r.arriveR = t.orderArrive; r.gen = o.gen; o = r; };
        const MV::MoveOrder pass = o; o = MV::MoveOrder{}; o.gen = pass.gen;
        const EQ::Vec3 me{ x.c.x, x.c.y, x.c.z };
        switch (S.force) {
        case Force::Stand: {
            if (S.forceVar == 1 && pass.mode == MV::MoveMode::Stand) { o = pass; return; }
            if (const EQ::Vec3* p = Persist(q, MV::MoveMode::Stand)) { order(MV::MoveMode::Stand, *p, q.order.tri); return; }
            if (q.phase == StrictPhase::Ended) { order(MV::MoveMode::Stand, me, x.nav.tri); return; }
            EQ::Vec3 at = me; std::int32_t tri = x.nav.tri; const EQ::View v = EnvViewOf(F, true);
            if (S.forceVar == 2) { EQ::Vec3 on; const std::int32_t k = EQ::NearestTri(v, { F.b[0].c.x + 80.0f, F.b[0].c.y, F.b[0].c.z }, 200.0f, on); if (k >= 0) { at = on; tri = k; } }
            if (S.forceVar == 3) { MV::Work::Flood f; MV::Flood(v, me, x.nav.tri, WalkerOf(x), 900.0f, 1024, f); float best = 1e9f; for (const auto& [k, d] : f.d) { const EQ::Vec3 p = EQ::Centroid(v, k); float w = 1e9f; for (int i = 0; i < 4; ++i) { const float a = 0.7853982f * static_cast<float>(i); float s = 0; for (const float sg : { -1.0f, 1.0f }) { const EQ::Walk e = EQ::WalkLine(v, k, p, { p.x + sg * std::cos(a) * 600.0f, p.y + sg * std::sin(a) * 600.0f, p.z }, WalkerOf(x)); s += std::hypot(e.end.x - p.x, e.end.y - p.y); } w = std::min(w, s); }
                    if (w + 0.1f * d < best) { best = w + 0.1f * d; at = p; tri = k; } } }
            order(MV::MoveMode::Stand, at, tri); return; }
        case Force::Kite: {
            const float trig = S.forceVar == 0 ? 500.0f : 900.0f; const float near = Nearest(F, x, false); if (near > trig + (Persist(q, MV::MoveMode::Kite) ? 150.0f : 0.0f)) return;
            if (const EQ::Vec3* p = Persist(q, MV::MoveMode::Kite); p && q.phase != StrictPhase::Standing) { order(MV::MoveMode::Kite, *p, q.order.tri); return; }
            EQ::Vec3 at; std::int32_t tri = -1; if (Away(F, x, 700.0f, true, at, tri)) order(MV::MoveMode::Kite, at, tri); return; }
        case Force::Disengage: {
            if (S.forceVar == 1 && x.c.hp > 0.5f * x.c.hpMax) return;
            const float near = Nearest(F, x, true); if (near > 500.0f + (Persist(q, MV::MoveMode::Disengage) ? 250.0f : 0.0f)) return;
            if (const EQ::Vec3* p = Persist(q, MV::MoveMode::Disengage); p && q.phase != StrictPhase::Standing) { order(MV::MoveMode::Disengage, *p, q.order.tri); return; }
            EQ::Vec3 at; std::int32_t tri = -1;
            if (S.forceVar == 2) { EQ::Vec3 on; tri = EQ::NearestTri(EnvViewOf(F, true), { F.b[0].c.x, F.b[0].c.y + 100.0f, F.b[0].c.z }, 200.0f, on); at = on; if (tri >= 0 && Fight::Dist(x, F.b[0]) > 300.0f) order(MV::MoveMode::Disengage, at, tri); return; }
            if (Away(F, x, 1500.0f, false, at, tri)) order(MV::MoveMode::Disengage, at, tri); return; }
        default: return;
        }
    }

    // ── the runs ──
    inline int RunModeCover(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, bool real) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); std::map<std::string, long> all; std::map<int, long> wrong[6]; long built = 0, exact = 0, na = 0; std::vector<char> seen(static_cast<std::size_t>(kModeCells), 0);
        std::vector<std::array<double, 5>> ter(static_cast<std::size_t>(kMT)); std::vector<std::array<long, 2>> terOk(static_cast<std::size_t>(kMT)); const std::vector<std::string> names{ "x" };
        for (int i = 0; i < kModeCells; ++i) { const Cell c = ModeCell(i); if (ModeCellNA(c)) { ++na; continue; } FightSpec fs = ModeSpec(D, classes, kModeClasses[static_cast<std::size_t>(i) % std::size(kModeClasses)], c, seed * 7919 + static_cast<std::uint64_t>(i)); if (!fs.env) { std::printf("MODE-COVER: no fixture %s\n", kModeTerrain[c.terrain].fixture); return 2; }
            Fight F = BuildFight(D, fs, t1); F.SetArena(); ModePlayer(F, c); const Measured m = ModeMeasure(F, names); ++built;
            auto& T = ter[static_cast<std::size_t>(c.terrain)]; T[0] += m.width; T[1] += m.dz; T[2] += m.water; T[3] += m.doors; T[4] += 1; auto& ok = terOk[static_cast<std::size_t>(c.terrain)]; const bool tok = ModeTerrainIs(c.terrain, F, m); ok[tok ? 0 : 1]++;
            const int w[6] = { c.kind != m.c.kind, c.speed != m.c.speed, c.count != m.c.count, !tok, c.party != m.c.party, c.player != m.c.player }; bool good = true; for (int k = 0; k < 6; ++k) if (w[k]) { good = false; wrong[k][k == 0 ? c.kind * 100 + m.c.kind : k == 3 ? c.terrain : k == 1 ? c.speed * 100 + m.c.speed : k == 2 ? c.count * 100 + m.c.count : k == 4 ? c.party * 100 + m.c.party : c.player * 1000 + c.terrain]++; }
            exact += good; seen[static_cast<std::size_t>(i)] = good; }
        std::printf("MODE-COVER cells %d (kind %d x speed %d x count %d x terrain %d x party %d x player %d), not applicable %ld, built %ld, measured as asked %ld (%.1f%%)\n", kModeCells, kMK, kMS, kMC, kMT, kMP, kML, na, built, exact, 100.0 * double(exact) / double(built));
        // kind x terrain, counted over the other four axes (of 108), and the three-way speed x count x party
        std::printf("%-12s", "kind\\terrain"); for (const ModeTerrain& t : kModeTerrain) std::printf(" %9s", t.name); std::printf("\n");
        for (int k = 0; k < kMK; ++k) { std::printf("%-12s", kModeKinds[k]); for (int t = 0; t < kMT; ++t) { long n = 0; for (int i = 0; i < kModeCells; ++i) { const Cell c = ModeCell(i); if (c.kind == k && c.terrain == t) n += seen[static_cast<std::size_t>(i)]; } std::printf(" %9ld", n); } std::printf("\n"); }
        std::printf("%-12s", "kind\\sp,cnt"); for (int s = 0; s < kMS; ++s) for (int n = 0; n < kMC; ++n) std::printf(" %10s", (std::string(kModeSpeeds[s]).substr(0, 4) + " " + kModeCounts[n]).c_str()); std::printf("\n");
        for (int k = 0; k < kMK; ++k) { std::printf("%-12s", kModeKinds[k]); for (int s = 0; s < kMS; ++s) for (int q = 0; q < kMC; ++q) { long n = 0; for (int i = 0; i < kModeCells; ++i) { const Cell c = ModeCell(i); if (c.kind == k && c.speed == s && c.count == q) n += seen[static_cast<std::size_t>(i)]; } std::printf(" %10ld", n); } std::printf("\n"); }
        std::printf("%-12s", "party\\player"); for (const char* p : kModePlayer) std::printf(" %9s", p); std::printf("\n"); for (int p = 0; p < kMP; ++p) { std::printf("%-12s", kModeParty[p]); for (int l = 0; l < kML; ++l) { long n = 0; for (int i = 0; i < kModeCells; ++i) { const Cell c = ModeCell(i); if (c.party == p && c.player == l) n += seen[static_cast<std::size_t>(i)]; } std::printf(" %9ld", n); } std::printf("\n"); }
        // the five-way table of the brief (kind x speed x count x terrain x party): a hole = a cell no player placement of which measured as asked
        long five = 0, holes = 0; std::map<std::string, long> why; for (int k = 0; k < kMK; ++k) for (int s = 0; s < kMS; ++s) for (int q = 0; q < kMC; ++q) for (int t = 0; t < kMT; ++t) for (int p = 0; p < kMP; ++p) { if (ModeCellNA({ k, s, q, t, p, 0 })) continue; ++five; bool any = false; for (int l = 0; l < kML; ++l) any |= seen[static_cast<std::size_t>(l + kML * (p + kMP * (q + kMC * (s + kMS * (t + kMT * k)))))] != 0; if (!any) { ++holes; why[std::string(kModeKinds[k]) + " count " + kModeCounts[q]]++; why[std::string("terrain ") + kModeTerrain[t].name]++; } }
        std::printf("FIVE-WAY (kind x speed x count x terrain x party): %ld cells, filled %ld, holes %ld\n", five, five - holes, holes); for (const auto& [n, c] : why) std::printf("  hole %-28s %ld\n", n.c_str(), c);
        for (int t = 0; t < kMT; ++t) { const auto& T = ter[static_cast<std::size_t>(t)]; std::printf("TERRAIN %-9s narrowest %6.0f  height %4.0f  water %4.1f%%  doors %.1f   is what it says in %ld of %ld\n", kModeTerrain[t].name, T[0] / T[4], T[1] / T[4], T[2] / T[4], T[3] / T[4], terOk[static_cast<std::size_t>(t)][0], terOk[static_cast<std::size_t>(t)][0] + terOk[static_cast<std::size_t>(t)][1]); }
        if (real) for (const SimEnv* e : g_envPool.draw) { double T[4] = {}; int n = 0; for (int k = 0; k < 24; ++k) { const Cell c = ModeSample(k * 13); const FightSpec fs = ModeSpec(D, classes, "warrior", c, seed * 31 + static_cast<std::uint64_t>(k), e); Fight F = BuildFight(D, fs, t1); F.SetArena(); ModePlayer(F, c); const Measured m = ModeMeasure(F, names); if (m.open > 0.0f || m.width > 1e8f) continue; T[0] += m.width; T[1] += m.dz; T[2] += m.water; T[3] += m.doors; ++n; }
            if (n) std::printf("PLACE %-70s %s narrowest %6.0f  height %4.0f  water %4.1f%%  doors %4.1f  (%d of 24 layouts with a way)\n", e->name.c_str(), e->interior ? "int" : "ext", T[0] / n, T[1] / n, T[2] / n, T[3] / n, n); }
        static const char* const ax[6] = { "kind", "speed", "count", "terrain", "party", "player" }; for (int k = 0; k < 6; ++k) for (const auto& [n, c] : wrong[k]) std::printf("  measured otherwise %-8s %5d: %ld\n", ax[k], n, c);
        return 0;
    }

    struct ModeOpts { int n = 360, fights = 64; bool real = false; std::string cls; int oracle = 0; bool null = false; };   // null: every arm is FREE with its own luck: what the counting alone makes of nothing
    // One (scenario, class). THE RACE is the oracle's own rule (Oracle.h: paired on the same fights, one-sided Student t at 0.001, 16 fights, then doubling to the ceiling
    // --mode-fights): an arm falls once the best arm leads it by more than luck. RESOLVED = every arm still standing is of one mode: that mode is best there. Else the
    // scenario is open between the modes that stand, and is counted as nobody's. An arm whose orders held him less than kModeActs of the
    // time is Free over again and stands for nothing (disengage:hurt in a fight that never hurts him).
    inline constexpr double kModeActs = 0.10;
    struct ModeResult { std::array<double, std::size(kModeArms)> mean{}; std::array<double, std::size(kModeArms)> used{}; std::array<char, std::size(kModeArms)> up{}; int n = 0; int best = -1 /* argmax mode */, resolved = -1; };
    inline int RunModeTest(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int threads, const ModeOpts& o, const OracleCfg& oc) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); constexpr std::size_t A = std::size(kModeArms), C = std::size(kModeClasses); const std::size_t places = g_envPool.draw.size(); const int N = o.n + (o.real ? static_cast<int>(places) * kMK : 0);
        const auto cellOf = [&](int s) { const bool real = s >= o.n; Cell c = ModeSample(real ? (s - o.n) * 7 + (s - o.n) / kMK : s); if (real) { c.kind = (s - o.n) % kMK; if (ModeCellNA(c)) c.count = 1; } return c; };
        std::vector<ModeResult> res(static_cast<std::size_t>(N) * C); g_road = Road::Strict; std::atomic<int> next{ 0 }; std::atomic<long long> ns{ 0 }, played{ 0 }; const int jobs = N * static_cast<int>(C);
        const auto play = [&](const Cell& cell, const SimEnv* place, std::size_t ci, int s, int f, std::size_t a, double& used) { const FightSpec fs = ModeSpec(D, classes, kModeClasses[ci], cell, seed * 1000003ull + static_cast<std::uint64_t>(s) * 7919ull + static_cast<std::uint64_t>(ci) * 31ull, place);
            const auto t0 = std::chrono::steady_clock::now(); Fight F = BuildFight(D, fs, t1); F.SetArena(); ModePlayer(F, cell); F.move.s = std::make_unique<MoveState>(); F.move->claim.resize(F.ctl.size()); F.move->force = o.null ? Force::Free : kModeArms[a].mode; F.move->forceVar = kModeArms[a].var; F.move->forceRule = &ModeRule;
            // ONE SCENARIO IS ONE FIGHT (builds, foes, places, who is on whom); fight f is the same one with other luck, the same for every arm
            { const std::uint64_t luck = Fight::Mix64(fs.seed ^ (0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(f + 1)) ^ (o.null ? 0xC2B2AE3D27D4EB4Full * (a + 1) : 0ull)); F.R.g.seed(luck); F.V.g.seed(Fight::Mix64(luck + 1)); F.avSeed = Fight::Mix64(luck + 2); }
            IausPolicy P; P.T = ActiveTuning(); OraclePolicy OP; OP.cfg = oc; OP.iaus.T = ActiveTuning(); Policy& pol = o.oracle ? static_cast<Policy&>(OP) : static_cast<Policy&>(P);
            while (!F.over) { if (F.now >= F.nextPump && !F.ctl.empty()) { F.nextPump += TM::pump; pol.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); } pol.Window(F); F.StepWorld(); }
            used = o.null ? 1.0 : F.t.upSec > 0 ? F.t.strictSec / F.t.upSec : 0.0; ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); ++played; return Utility(F); };
        auto work = [&]() { for (;;) { const int j = next.fetch_add(1); if (j >= jobs) return; const int s = j / static_cast<int>(C); const std::size_t ci = static_cast<std::size_t>(j) % C; if (!o.cls.empty() && o.cls != kModeClasses[ci]) continue;
            const Cell cell = cellOf(s); const SimEnv* place = s >= o.n ? g_envPool.draw[static_cast<std::size_t>((s - o.n) / kMK) % places] : nullptr; ModeResult& r = res[static_cast<std::size_t>(j)]; std::array<std::vector<double>, A> u; for (std::size_t a = 0; a < A; ++a) r.up[a] = o.null || ModeServed(kModeArms[a]);
            for (int to = std::min(16, o.fights); ; to = std::min(to * 2, o.fights)) {
                for (std::size_t a = 0; a < A; ++a) if (r.up[a]) for (int f = static_cast<int>(u[a].size()); f < to; ++f) { double used = 0; u[a].push_back(play(cell, place, ci, s, f, a, used)); r.used[a] += used; }
                r.n = to; std::size_t b = 0; bool have = false; for (std::size_t a = 0; a < A; ++a) { if (a > 0 && r.used[a] < kModeActs * static_cast<double>(u[a].size())) r.up[a] = 0; if (!r.up[a]) continue; double m = 0; for (double v : u[a]) m += v / static_cast<double>(u[a].size()); r.mean[a] = m; if (!have || m > r.mean[b]) { b = a; have = true; } }
                const double crit = TCrit(to, 0.001);
                for (std::size_t a = 0; a < A; ++a) if (r.up[a] && a != b) { double m = 0, var = 0; for (int f = 0; f < to; ++f) m += (u[b][static_cast<std::size_t>(f)] - u[a][static_cast<std::size_t>(f)]) / to; for (int f = 0; f < to; ++f) { const double d = u[b][static_cast<std::size_t>(f)] - u[a][static_cast<std::size_t>(f)] - m; var += d * d; } if (m > crit * std::sqrt(var / (to - 1) / to)) r.up[a] = 0; }
                r.best = ModeOf(kModeArms[b]); int only = -1; bool one = true; for (std::size_t a = 0; a < A; ++a) if (r.up[a]) { const int m = ModeOf(kModeArms[a]); if (only < 0) only = m; one = one && m == only; }
                if (one) { r.resolved = only; break; } if (to >= o.fights) break; } } };
        { std::vector<std::thread> th; for (int i = 0; i < std::max(1, threads); ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        struct Cnt { std::array<long, 4> arg{}, res{}, stand{}; long n = 0, resolved = 0; }; std::vector<Cnt> per(C); Cnt all; std::map<std::string, Cnt> by;
        for (int s = 0; s < N; ++s) for (std::size_t ci = 0; ci < C; ++ci) { if (!o.cls.empty() && o.cls != kModeClasses[ci]) continue; const ModeResult& r = res[static_cast<std::size_t>(s) * C + ci]; const Cell cell = cellOf(s); const bool real = s >= o.n;
            std::array<bool, 4> st{}; for (std::size_t a = 0; a < A; ++a) if (r.up[a]) st[static_cast<std::size_t>(ModeOf(kModeArms[a]))] = true;
            const auto add = [&](Cnt& c) { ++c.n; ++c.arg[static_cast<std::size_t>(r.best)]; if (r.resolved >= 0) { ++c.resolved; ++c.res[static_cast<std::size_t>(r.resolved)]; } for (std::size_t k = 0; k < 4; ++k) c.stand[k] += st[k]; };
            add(per[ci]); add(all); for (const std::string& k : { std::string("kind ") + kModeKinds[cell.kind], std::string("speed ") + kModeSpeeds[cell.speed], std::string("count ") + kModeCounts[cell.count], std::string("terrain ") + (real ? "extracted" : kModeTerrain[cell.terrain].name), std::string("party ") + kModeParty[cell.party], std::string("player ") + kModePlayer[cell.player] }) add(by[k]); }
        std::printf("MODE-TEST %d scenarios (%d of them on extracted places) x %zu classes, %zu arms, up to %d fights an arm, seed %llu%s%s\n", N, N - o.n, C, A, o.fights, static_cast<unsigned long long>(seed), o.oracle ? ", actions by the ORACLE" : ", actions by IAUS", o.null ? ", NULL (every arm Free)" : "");
        const auto pct = [](long a, long b) { return b ? 100.0 * double(a) / double(b) : 0.0; };
        const auto line = [&](const char* name, const Cnt& c) { std::printf("%-22s n %5ld resolved %5.1f%% |", name, c.n, pct(c.resolved, c.n)); for (std::size_t k = 0; k < 4; ++k) std::printf(" %s best %5.1f%% resolved %5.1f%% in the running %5.1f%% |", kModeNames[k], pct(c.arg[k], c.n), pct(c.res[k], c.n), pct(c.stand[k], c.n)); std::printf("\n"); };
        for (std::size_t ci = 0; ci < C; ++ci) if (per[ci].n) line(kModeClasses[ci], per[ci]);
        line("ALL", all); for (const auto& [k, c] : by) line(("  " + k).c_str(), c);
        // THE PLAN'S LINES (11 item 6), over the scenarios the race decided. The highest mean is no verdict: it counts arms and luck (s5-plan.md 12 trap 3). A mode the caps do not serve has no line
        bool passRes = true;
        for (std::size_t k = 0; k < 4; ++k) { double r = 0; const char* rc = "no class"; for (std::size_t ci = 0; ci < C; ++ci) if (per[ci].n && per[ci].resolved >= 20 && pct(per[ci].res[k], per[ci].resolved) > r) { r = pct(per[ci].res[k], per[ci].resolved); rc = kModeClasses[ci]; }
            bool served = o.null; for (const ModeArm& a : kModeArms) if (static_cast<std::size_t>(ModeOf(a)) == k && ModeServed(a)) served = true;
            if (!served) { std::printf("ACCEPT %-10s not served by --move-caps: not played\n", kModeNames[k]); continue; }
            const double orr = pct(all.res[k], all.resolved); const bool kr = r >= 10.0 && orr <= 60.0; passRes = passRes && kr;
            std::printf("ACCEPT %-10s of the resolved: %.1f%% for %s, %.1f%% over all %s\n", kModeNames[k], r, rc, orr, kr ? "ok" : "NOT MET"); }
        std::printf("MODE-TEST of the resolved %s   %lld fights, %.1f ms each\n", passRes ? "PASS" : "NOT MET", played.load(), 1e-6 * double(ns) / double(std::max(1ll, played.load())));
        g_road = Road::Off; return 0;
    }
}
