#pragma once
// iaus_sim/ModeGen.h -- THE SCENARIO GENERATOR of strict movement, v2 (s5-plan.md 5.1 to 5.3, package S5-D). HOST-ONLY.
//
// A scenario is DRAWN, not enumerated: (family, seed, k) gives one by a Latin hypercube over kModeAxes in blocks of kGenBlock, so every seed fights NEW cells and scenario k is the
// same whatever number is asked for. ModeSet.h's 12,960 cells stay what they are (a build check and the old tournament).
//   --mode-gen      draws and BUILDS the scenarios and prints what each asked for and what its bodies and its mesh are (GENSCEN lines, no fight)
//   --mode-calib    plays them in IAUS-only arms, road OFF, and finds each (scenario, class)'s difficulty by bisection (MODECAL lines, the table --mode-cal reads;
//                   MODECELL lines: medians a cell). What was FOUGHT is read back from the fight that was played (GENFOUGHT lines)
//   --mode-accept   ModeAccept.h: the oracle's fights and the mode leads. tools/iaus_sim/mode_accept.py judges
// A FAMILY (design | train | heldout) salts every draw, so a seed of one can never be a seed of another. kModeGenVersion is in every header line: a held-out family drawn under
// another version is refused (mode_accept.py).
// Caps: a row or an arm built for a mode --move-caps does not serve is not drawn, and no mode that is not served has a line.
//
// TO ADD: an axis = a kModeAxes row + its use in GenDraw. A mixed group = a kModeGroups row. A fight built for or against a mode = a kModeRows row. A terrain = a kGenTerrain row
// with its fixtures (envs/make_fixtures.py GEN). ANY of these changes kModeGenVersion.
#include "iaus_sim/ModeSet.h"

namespace Sim {

    inline constexpr const char* kModeGenVersion = "g2.0";
    enum class Ax : std::uint8_t { Speed = 0, Speed2, Count, Kind, Mix, Reach, Level, Target, Terrain, Fixture, Dist, Open, Chain, ChainAt, Party, Corner, Player, Hurt, Style, Light, On, Row, RowPick, kCount };
    inline constexpr const char* kModeAxes[] = { "speed", "speed2", "count", "kind", "mix", "reach", "level", "target", "terrain", "fixture", "dist", "open", "chain", "chainAt", "party", "corner", "player", "hurt", "style", "light", "on", "row", "rowPick" };
    static_assert(std::size(kModeAxes) == static_cast<std::size_t>(Ax::kCount));
    inline constexpr int kGenBlock = 120;
    inline constexpr const char* kGenFamilies[] = { "design", "train", "heldout" };
    // THE DRAWS (every share of the generator, one table)
    struct GenTuning {
        float mixShare = 0.40f, openShare = 0.25f /* of one-foe scenarios */, openU = 1500.0f, chainShare = 0.15f, cornerShare = 0.5f, rowShare = 0.25f, againstShare = 0.5f /* --mode-against <mode> */;
        float distLo = 300.0f, distHi = 2500.0f, hurtLo = 0.15f, hurtHi = 0.40f; std::array<int, 3> level{ 10, 25, 45 }; std::array<float, 3> target{ 0.40f, 0.50f, 0.60f };
        std::array<float, 4> light{ 0.10f, 0.35f, 0.65f, 0.95f };
    };
    inline constexpr GenTuning kGenT;
    struct GenTerrain { const char* name; std::array<const char*, 3> fixture; };
    inline constexpr GenTerrain kGenTerrain[] = {
        { "open", { "mode-open", "gen-open-a", "gen-open-b" } },          { "corridor", { "narrow", "gen-corridor-a", "gen-corridor-b" } }, { "doorway", { "doorway-open", "gen-doorway-a", "gen-doorway-b" } },
        { "pillars", { "pillars", "gen-pillars-a", "gen-pillars-b" } },   { "stairs", { "mode-stairs", "gen-stairs-a", "gen-stairs-b" } },  { "bridge", { "mode-bridge", "gen-bridge-a", "gen-bridge-b" } },
        { "dead-end", { "dead-end", "gen-dead-end-a", "gen-dead-end-b" } }, { "slope", { "mode-slope", "gen-slope-a", "gen-slope-b" } },     { "water", { "mode-water", "gen-water-a", "gen-water-b" } },
        { "door", { "doorway", "gen-door-a", "gen-door-b" } },            { "corner", { "gen-corner-a", "gen-corner-b", "gen-corner-c" } } };
    [[nodiscard]] constexpr int GenTerrainNo(std::string_view n) { for (std::size_t i = 0; i < std::size(kGenTerrain); ++i) if (n == kGenTerrain[i].name) return static_cast<int>(i); return -1; }
    // A mixed group: kinds and their shares of the count (each has at least one, so a group has a least size).
    struct GenPart { FoeKind kind = FoeKind::kCount; float share = 0.0f; };
    struct GenGroup { const char* name; std::array<GenPart, 3> part; };
    inline constexpr GenGroup kModeGroups[] = {
        { "shield-line", { GenPart{ FoeKind::Shield, 0.5f }, { FoeKind::Archers, 0.3f }, { FoeKind::BoltMage, 0.2f } } },
        { "boss-adds", { GenPart{ FoeKind::BossAdds, 1.0f } } },
        { "pack-archer", { GenPart{ FoeKind::Pack, 0.75f }, { FoeKind::Archers, 0.25f } } },
        { "summoner-melee", { GenPart{ FoeKind::Summoner, 0.3f }, { FoeKind::Duellist, 0.7f } } },
        { "melee-only", { GenPart{ FoeKind::Duellist, 0.5f }, { FoeKind::Shield, 0.25f }, { FoeKind::Heavy, 0.25f } } },
        { "stream-chargers", { GenPart{ FoeKind::StreamMage, 0.3f }, { FoeKind::Charger, 0.7f } } } };
    // A FIGHT BUILT FOR A MODE (favours) OR AGAINST ONE (against): what it fixes of the draw. -1 / "" / 0 = the draw's own. Modes as kModeNames (0 free, 1 stand, 2 kite, 3 disengage).
    // A hypothesis about a fight, not a label on it: what wins is measured.
    struct GenRow { const char* name; int favours, against; int kind, group; int nLo, nHi; float spLo, spHi; const char* terrain; float dLo, dHi; int party; bool onHim; float target; };
    inline constexpr GenRow kModeRows[] = {
        // a hold, a choke or cover beats free
        { "doorway-vs-many",   1, 0, int(FoeKind::Pack),       -1, 5, 9,  0.8f, 1.3f, "doorway",  450, 1100, -1, false, 0 },
        { "funnel-vs-blades",  1, 0, -1, 4 /* melee-only */,       4, 8,  0.7f, 1.1f, "corridor", 500, 1200,  1, false, 0 },
        { "bridge-vs-many",    1, 0, int(FoeKind::Duellist),   -1, 4, 7,  0.8f, 1.2f, "bridge",   600, 1400, -1, false, 0 },
        { "archers-behind",    1, 0, int(FoeKind::Archers),    -1, 3, 6,  0.6f, 1.0f, "pillars",  700, 1600, -1, false, 0 },
        { "bolts-at-a-corner", 1, 0, int(FoeKind::BoltMage),   -1, 2, 4,  0.6f, 1.0f, "corner",   700, 1500, -1, false, 0 },
        { "healer-line",       1, 0, -1, 0 /* shield-line */,      4, 6,  0.6f, 1.0f, "corner",   600, 1400,  2, false, 0 },
        { "dead-end-back",     1, 0, int(FoeKind::Pack),       -1, 4, 7,  1.0f, 1.4f, "dead-end", 400,  900, -1, false, 0 },
        // free beats a hold
        { "one-weak-blade",    0, 1, int(FoeKind::Duellist),   -1, 1, 1,  0.4f, 0.9f, "open",     600, 1500,  0, false, 0.65f },
        { "caster-line",       0, 1, int(FoeKind::BoltMage),   -1, 2, 3,  0.8f, 1.2f, "open",     900, 1800, -1, false, 0 },
        { "fast-pack-open",    0, 1, int(FoeKind::Pack),       -1, 4, 7,  1.2f, 1.5f, "open",     600, 1500, -1, true,  0 },
        { "charger-600",       0, 1, int(FoeKind::Charger),    -1, 1, 2,  1.1f, 1.5f, "open",     550,  650, -1, true,  0 },
        { "contact",           0, 1, int(FoeKind::Duellist),   -1, 2, 4,  0.8f, 1.2f, "",         300,  340, -1, true,  0 },
        { "dead-end-shooter",  0, 1, int(FoeKind::Duellist),   -1, 2, 3,  0.9f, 1.2f, "dead-end", 350,  800, -1, true,  0 },
        { "stream-on-him",     0, 1, int(FoeKind::StreamMage), -1, 1, 2,  0.7f, 1.0f, "open",     500,  900, -1, true,  0 },
        // a kite beats both (drawn only where it is served: the later delta stage)
        { "slow-heavy-open",   2, 1, int(FoeKind::Heavy),      -1, 1, 1, 0.20f, 0.38f, "open",   1500, 2200,  0, true,  0 },
        { "slow-blades-open",  2, 0, int(FoeKind::Duellist),   -1, 2, 3, 0.20f, 0.38f, "open",   1200, 2200, -1, true,  0 },
        // leaving beats all three (likewise)
        { "boss-contact",      3, 1, int(FoeKind::BossAdds),   -1, 4, 6,  0.8f, 1.1f, "",         300,  500,  1, true,  0 },
    };
    inline constexpr const char* kGenAgainst[] = { "free", "stand", "kite", "disengage" };   // as Oracle.h kModeName says them
    [[nodiscard]] inline bool GenServed(int mode) { return (mode != 2 || g_moveCaps.kite) && (mode != 3 || g_moveCaps.disengage); }

    struct GenScen {
        int k = 0, kind = 0, group = -1, count = 1, level = 25, terrain = 0, fixture = 0, chain = 0 /* 1 after chainAt s, 2 at half dead */, party = 0, player = 0, style = 0, on = 0, row = -1; bool corner = false;
        float speed = 1, speed2 = 1, reach = 1, target = 0.5f, dist = 1000, chainAt = 15, hurt = 1, light = 0.5f; std::uint64_t seed = 0;
    };
    inline constexpr const char* kGenParty[] = { "alone", "tank", "healer", "two-ranged", "mate", "four" };
    inline constexpr const char* kGenPlayer[] = { "near", "at-leash", "leaving", "hurt" };
    inline constexpr const char* kGenStyle[] = { "melee", "range", "mage", "passive" };
    inline constexpr const char* kGenOn[] = { "him", "player", "nobody" };
    [[nodiscard]] inline std::uint64_t GenSalt(int family, std::uint64_t seed, std::uint64_t x) { return Fight::Mix64(Fight::Mix64(seed * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(family + 1) * 0xD1B54A32D192ED03ULL) ^ Fight::Mix64(x)); }
    // The hypercube: in a block every axis visits its kGenBlock strata once each.
    [[nodiscard]] inline float GenU(int family, std::uint64_t seed, int k, Ax a) {
        const int blk = k / kGenBlock; std::mt19937_64 g(GenSalt(family, seed, 0xA0000000ULL + static_cast<std::uint64_t>(blk) * 64 + static_cast<std::uint64_t>(a)));
        std::array<int, kGenBlock> p{}; for (int i = 0; i < kGenBlock; ++i) p[static_cast<std::size_t>(i)] = i;
        for (int i = kGenBlock - 1; i > 0; --i) std::swap(p[static_cast<std::size_t>(i)], p[static_cast<std::size_t>(g() % static_cast<std::uint64_t>(i + 1))]);
        const double in = static_cast<double>(GenSalt(family, seed, 0xB0000000ULL + static_cast<std::uint64_t>(k) * 64 + static_cast<std::uint64_t>(a)) >> 11) * (1.0 / 9007199254740992.0);
        return static_cast<float>(std::min(0.999999, (p[static_cast<std::size_t>(k % kGenBlock)] + in) / kGenBlock)); }
    // Foe speed over his run: a third slow enough for a kite (0.40 is kiteSpeed over 370), a third below him, a third equal and faster.
    [[nodiscard]] inline float GenSpeed(float u) { const float t = u * 3.0f; return t < 1.0f ? 0.20f + 0.20f * t : t < 2.0f ? 0.40f + 0.60f * (t - 1.0f) : 1.00f + 0.50f * (t - 2.0f); }
    [[nodiscard]] inline int GenCount(float u) { const auto in = [](float v, int lo, int hi) { return lo + std::min(hi - lo, static_cast<int>(v * static_cast<float>(hi - lo + 1))); };
        return u < 0.25f ? 1 : u < 0.55f ? in((u - 0.25f) / 0.30f, 2, 3) : u < 0.80f ? in((u - 0.55f) / 0.25f, 4, 6) : in((u - 0.80f) / 0.20f, 7, 10); }
    [[nodiscard]] inline int GenParts(int group) { int n = 0; for (const GenPart& p : kModeGroups[group].part) n += p.share > 0.0f; return n; }
    struct GenOpts { int family = 0; std::uint64_t seed = 1; int against = -1; };   // against: a kModeNames index, more rows built against it (s5-plan.md 5.4)
    [[nodiscard]] inline GenScen GenDraw(const GenOpts& o, int k) {
        const auto u = [&](Ax a) { return GenU(o.family, o.seed, k, a); }; const auto pick = [&](Ax a, int n) { return std::min(n - 1, static_cast<int>(u(a) * static_cast<float>(n))); }; const auto lerp = [](float t, float a, float b) { return a + t * (b - a); };
        GenScen s; s.k = k; s.seed = GenSalt(o.family, o.seed, 0xC0000000ULL + static_cast<std::uint64_t>(k));
        s.speed = GenSpeed(u(Ax::Speed)); s.speed2 = GenSpeed(u(Ax::Speed2)); s.count = GenCount(u(Ax::Count)); s.kind = pick(Ax::Kind, kMK);
        { const float r = u(Ax::Reach); s.reach = r < 1.0f / 3 ? lerp(3 * r, 0.75f, 1.0f) : r < 2.0f / 3 ? 1.0f : lerp(3 * r - 2, 1.0f, 1.8f); }
        s.level = kGenT.level[static_cast<std::size_t>(pick(Ax::Level, 3))]; s.target = kGenT.target[static_cast<std::size_t>(pick(Ax::Target, 3))];
        s.terrain = pick(Ax::Terrain, static_cast<int>(std::size(kGenTerrain))); s.fixture = pick(Ax::Fixture, 3); s.dist = lerp(u(Ax::Dist), kGenT.distLo, kGenT.distHi);
        if (u(Ax::Mix) < kGenT.mixShare) s.group = pick(Ax::Kind, static_cast<int>(std::size(kModeGroups)));
        s.party = pick(Ax::Party, static_cast<int>(std::size(kGenParty))); s.corner = u(Ax::Corner) < kGenT.cornerShare; s.player = pick(Ax::Player, 4); s.hurt = s.player == 3 ? lerp(u(Ax::Hurt), kGenT.hurtLo, kGenT.hurtHi) : 1.0f;
        s.style = pick(Ax::Style, 4); s.light = kGenT.light[static_cast<std::size_t>(pick(Ax::Light, 4))]; s.on = pick(Ax::On, 3);
        if (const float c = u(Ax::Chain) / kGenT.chainShare; c < 1.0f) { s.chain = c < 0.5f ? 1 : 2; s.chainAt = lerp(u(Ax::ChainAt), 8.0f, 25.0f); }
        if (s.count == 1 && s.group < 0 && u(Ax::Open) < kGenT.openShare) { s.terrain = GenTerrainNo("open"); s.fixture = 1 + pick(Ax::Fixture, 2); s.dist = std::max(s.dist, kGenT.openU); s.corner = false; }
        std::vector<int> rows; for (std::size_t i = 0; i < std::size(kModeRows); ++i) if (GenServed(kModeRows[i].favours) && (o.against < 0 || kModeRows[i].against == o.against)) rows.push_back(static_cast<int>(i));
        if (!rows.empty() && u(Ax::Row) < (o.against >= 0 ? kGenT.againstShare : kGenT.rowShare)) { s.row = rows[static_cast<std::size_t>(pick(Ax::RowPick, static_cast<int>(rows.size())))]; const GenRow& r = kModeRows[s.row];
            s.group = r.group; if (r.kind >= 0) s.kind = r.kind; s.count = r.nLo + std::min(r.nHi - r.nLo, static_cast<int>(u(Ax::Count) * static_cast<float>(r.nHi - r.nLo + 1))); s.speed = lerp(u(Ax::Speed), r.spLo, r.spHi); s.speed2 = lerp(u(Ax::Speed2), r.spLo, r.spHi); s.dist = lerp(u(Ax::Dist), r.dLo, r.dHi);
            if (r.terrain[0]) s.terrain = GenTerrainNo(r.terrain); if (r.party >= 0) s.party = r.party; if (r.onHim) s.on = 0; if (r.target > 0) s.target = r.target; s.corner = s.corner && !r.terrain[0]; s.chain = 0; if (r.terrain[0] && s.dist > 1300.0f && !std::strcmp(r.terrain, "open")) s.fixture = 1 + pick(Ax::Fixture, 2); }
        if (s.group >= 0) s.count = std::max(s.count, std::max(2, GenParts(s.group))); else if (static_cast<FoeKind>(s.kind) == FoeKind::BossAdds) s.count = std::max(2, s.count);
        if (s.group < 0 && static_cast<FoeKind>(s.kind) == FoeKind::Flyer) s.count = std::min(s.count, 3);   // MODEL: no flight of ten
        return s; }

    // The foes of a draw, n of them, into f.customFoes. Every KIND of a mixed group has a speed of its own (the first the scenario's, the second speed2, the third between).
    inline void GenFoes(const Data& D, const GenScen& s, int n, Rng& R, FightSpec& f) {
        const std::size_t first = f.customFoes.size(); const float sp[3] = { s.speed, s.speed2, 0.5f * (s.speed + s.speed2) };
        if (s.group < 0) for (int i = 0; i < n; ++i) ModeFoe(D, static_cast<FoeKind>(s.kind), i, kModeRun * s.speed, s.level, R, f);
        else { const auto& part = kModeGroups[s.group].part; const int parts = GenParts(s.group); int left = n; for (int p = 0; p < parts; ++p) { const int m = p == parts - 1 ? left : std::clamp(static_cast<int>(std::lround(part[static_cast<std::size_t>(p)].share * static_cast<float>(n))), 1, left - (parts - 1 - p)); left -= m; for (int i = 0; i < m; ++i) ModeFoe(D, part[static_cast<std::size_t>(p)].kind, i, kModeRun * sp[p], s.level, R, f); } }
        for (std::size_t i = first; i < f.customFoes.size(); ++i) { auto c = std::make_shared<ActorSpec>(*f.customFoes[i]); if (!c->ranged.Has() && c->spells.empty()) { c->melee.reach *= s.reach; c->unarmedReach *= s.reach; } f.customFoes[i] = c; } }
    [[nodiscard]] inline FightSpec GenSpec(const Data& D, const std::vector<SimClass>& cls, const char* slug, const GenScen& s, float ratio, const SimEnv* real) {
        Rng R; std::uint64_t h = 1469598103934665603ULL; for (const char* q = slug; *q; ++q) h = (h ^ static_cast<unsigned char>(*q)) * 1099511628211ULL; R.g.seed(Fight::Mix64(s.seed ^ Fight::Mix64(h))); FightSpec f; f.cls = ClassBySlug(cls, slug); f.level = s.level; f.rank = 3; f.playerFam = static_cast<Cls>(s.style == 3 ? 0 : s.style); f.occluders = 0; f.seed = R.g(); f.targetRatio = ratio; f.foeDist = s.dist;
        const bool healer = s.party == 2 || s.party == 5 || !std::strcmp(slug, "healer"); int terrain = s.terrain; if (healer && s.corner) terrain = GenTerrainNo("corner");
        f.env = real ? real : g_envPool.ByName(kGenTerrain[terrain].fixture[static_cast<std::size_t>(s.fixture)]); f.interior = f.env ? f.env->interior : false;
        GenFoes(D, s, s.count, R, f);
        if (s.chain) { FightSpec t; GenFoes(D, s, std::max(1, s.count / 2), R, t); for (std::size_t i = 0; i < t.customFoes.size(); ++i) { f.late.push_back({ s.chain == 1 ? static_cast<double>(s.chainAt) : 90.0, t.customFoes[i] }); f.lateTags.push_back(t.foeTags[i]); } }   // an arrival at 90 s the latest (GenBuild: or at half dead)
        const auto mate = [&](const char* m) { f.others.push_back(ClassBySlug(cls, m)); }; const char* tank = std::strcmp(slug, "knight") ? "knight" : "warrior";
        switch (s.party) { case 1: mate(tank); break; case 2: mate("healer"); break; case 3: mate("archer"); mate("evoker"); break; case 4: mate(kModeClasses[static_cast<std::size_t>(R.I(0, static_cast<int>(std::size(kModeClasses)) - 1))]); break; case 5: mate(tank); mate("healer"); mate("archer"); break; default: break; }
        if (s.party == 2 || s.party == 5) f.ensureOthers = { "healOther", "heal" };
        f.onSubject = s.on == 0; f.foesOnPlayer = s.on == 1; f.playerHp = s.hurt; f.light = s.light; f.freeEntry = 0; if (f.cls->gate.spellsOnClass) f.ensure = { "fire", "heal" };
        return f; }
    [[nodiscard]] inline Fight GenBuild(const Data& D, const FightSpec& f, const GenScen& s, const std::vector<SimClass>& t1) {
        Fight F = BuildFight(D, f, t1); F.SetArena(); Cell c; c.player = s.player == 3 ? 0 : s.player; ModePlayer(F, c);
        if (s.style == 3) { Body& p = F.b[0]; auto q = std::make_shared<ActorSpec>(*p.spec); q->atkMult *= 0.02f; if (q->rangedAtkMult > 0) q->rangedAtkMult *= 0.02f; p.spec = q; }   // MODEL: a passive player's blows count for nothing
        if (s.chain == 2) { int n = 0; for (const Body& x : F.b) n += x.role == Role::Foe; for (Spawn& l : F.late) l.halfOf = n; }
        return F; }
    // ONE SCENARIO IS ONE FIGHT: luck f is the same fight with other luck, the same in every arm. salt: the calibration's luck is never the acceptance's.
    inline void GenLuck(Fight& F, std::uint64_t seed, int f, std::uint64_t salt) { const std::uint64_t l = Fight::Mix64(seed ^ Fight::Mix64(salt) ^ (0x9E3779B97F4A7C15ULL * static_cast<std::uint64_t>(f + 1))); F.R.g.seed(l); F.V.g.seed(Fight::Mix64(l + 1)); F.avSeed = Fight::Mix64(l + 2); F.raceSeed = Fight::Mix64(l + 3); }

    // ── the extracted set: stratified by what is measured, no place by name ──
    struct GenPlace { const SimEnv* env = nullptr; float width = 0, dz = 0, doors = 0; int stratum = 0; };
    inline constexpr int kGenStrata = 27;
    [[nodiscard]] inline int GenStratum(float w, float dz, float doors) { return (w < 150.0f ? 0 : w <= 400.0f ? 1 : 2) * 9 + (dz < 64.0f ? 0 : dz <= 200.0f ? 1 : 2) * 3 + (doors < 0.5f ? 0 : doors <= 5.0f ? 1 : 2); }
    [[nodiscard]] inline std::string GenStratumName(int s) { static const char* const w[] = { "under 150", "150-400", "over 400" }; static const char* const z[] = { "flat", "64-200", "over 200" }; static const char* const d[] = { "no door", "1-5", "6+" }; return std::string(w[s / 9]) + ", " + z[s / 3 % 3] + ", " + d[s % 3]; }
    [[nodiscard]] inline std::vector<GenPlace> GenMeasurePlaces(const Data& D, const std::vector<SimClass>& cls, const std::vector<SimClass>& t1) {
        std::vector<GenPlace> all; const std::vector<std::string> names{ "x" }; for (const SimEnv* e : g_envPool.draw) { std::vector<float> w, z, d; for (int k = 0; k < 16; ++k) { const FightSpec fs = ModeSpec(D, cls, "warrior", ModeSample(k * 13), 31ull * static_cast<std::uint64_t>(k) + 17, e); Fight F = BuildFight(D, fs, t1); F.SetArena(); const Measured m = ModeMeasure(F, names); if (m.open > 0.0f || m.width > 1e8f) continue; w.push_back(m.width); z.push_back(m.dz); d.push_back(m.doors); }
            if (w.size() < 8) continue; const auto med = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }; GenPlace p; p.env = e; p.width = med(w); p.dz = med(z); p.doors = med(d); p.stratum = GenStratum(p.width, p.dz, p.doors); all.push_back(p); }
        return all; }
    // Up to 3 of a stratum, then more of the fuller ones one at a time until `want` (a stratum with none is printed, never filled by another).
    [[nodiscard]] inline std::vector<GenPlace> GenPlaces(std::vector<GenPlace> all, const GenOpts& o, std::size_t want) {
        std::sort(all.begin(), all.end(), [&](const GenPlace& a, const GenPlace& b) { const auto h = [&](const GenPlace& p) { return GenSalt(o.family, o.seed, p.env->hash); }; const auto x = h(a), y = h(b); return x != y ? x < y : a.env->name < b.env->name; });
        std::vector<std::vector<GenPlace>> st(kGenStrata); for (const GenPlace& p : all) st[static_cast<std::size_t>(p.stratum)].push_back(p); std::vector<GenPlace> out;
        for (std::size_t depth = 0; depth < all.size(); ++depth) { if (depth >= 3 && out.size() >= want) break; for (const auto& v : st) if (depth < v.size() && (depth < 3 || out.size() < want)) out.push_back(v[depth]); }
        return out; }

    // ── what a fight IS, from its bodies and its mesh ──
    [[nodiscard]] inline std::string GenRead(const Fight& F) {
        const Body& s = F.b[static_cast<std::size_t>(F.ctl[0].body)]; std::vector<float> run; float reach = 0, hisReach = F.Reach(s), near = 1e9f, lvl = 0; int n = 0, shoot = 0, cast = 0, blade = 0, beast = 0, boss = 0, shield = 0;
        for (const Body& o : F.b) if (o.role == Role::Foe) { ++n; run.push_back(o.spec->run / std::max(1.0f, s.spec->run)); lvl += static_cast<float>(o.spec->level); near = std::min(near, Fight::Dist(o, s)); const bool sp = [&] { for (int i : o.spec->spells) if (F.D->foeSpells[static_cast<std::size_t>(i)].e.nature == Prune::Nature::Offense) return true; return false; }();
            if (o.tag == "boss") ++boss; if (sp) ++cast; else if (o.spec->ranged.Has()) ++shoot; else { (o.spec->animal || o.spec->creature ? beast : blade)++; reach = std::max(reach, F.Reach(o)); } if (o.spec->shield) ++shield; }
        std::sort(run.begin(), run.end()); const std::vector<std::string> names{ "x" }; const Measured m = ModeMeasure(F, names); int heal = 0; for (std::size_t i = 1; i < F.ctl.size(); ++i) { bool h = false; for (const SimSpell& q : F.ctl[i].build->spells) h = h || q.healOther; heal += h; }
        char b[640]; std::snprintf(b, sizeof b, "n=%d late=%zu shoot=%d cast=%d blade=%d beast=%d boss=%d shield=%d runLo=%.2f runHi=%.2f reach=%.2f foeLevel=%.0f level=%d start=%.0f way=%d width=%.0f dz=%.0f water=%.0f doors=%.0f int=%d party=%zu heal=%d playerHp=%.2f playerDist=%.0f leaving=%d light=%.2f", n, F.late.size(), shoot, cast, blade, beast, boss, shield, run.empty() ? 0.0f : run.front(), run.empty() ? 0.0f : run.back(), reach / std::max(1.0f, hisReach), n ? lvl / static_cast<float>(n) : 0.0f,
            s.spec->level, near, m.open > 0 ? 0 : 1, m.width > 1e8f ? -1.0f : m.width, m.dz, m.water, m.doors, int(F.env && F.env->interior), F.ctl.size(), heal, F.b[0].c.hp / std::max(1.0f, F.b[0].c.hpMax), Fight::Dist(s, F.b[0]), int(F.now < F.b[0].retreatUntil), F.light);
        return b; }
    [[nodiscard]] inline std::string GenAsked(const GenScen& s, const char* terrain) { char b[320]; std::snprintf(b, sizeof b, "row=%s kind=%s group=%s count=%d speed=%.2f speed2=%.2f reach=%.2f level=%d target=%.2f terrain=%s dist=%.0f chain=%d party=%s player=%s style=%s light=%.2f on=%s", s.row >= 0 ? kModeRows[s.row].name : "-", s.group < 0 ? kModeKinds[s.kind] : "-", s.group >= 0 ? kModeGroups[s.group].name : "-", s.count, s.speed, s.speed2, s.reach, s.level, s.target, terrain, s.dist, s.chain, kGenParty[s.party], kGenPlayer[s.player], kGenStyle[s.style], s.light, kGenOn[s.on]); return b; }

    // ── the calibration's cells (s5-plan.md 5.2) ──
    [[nodiscard]] inline const char* GenKindGroup(const GenScen& s) { if (s.group >= 0) return "mixed"; switch (static_cast<FoeKind>(s.kind)) { case FoeKind::Archers: case FoeKind::BoltMage: case FoeKind::AreaMage: case FoeKind::StreamMage: case FoeKind::Summoner: return "ranged"; case FoeKind::BossAdds: case FoeKind::Flyer: return "boss"; case FoeKind::Pack: case FoeKind::Charger: return "beast"; default: return "melee"; } }
    [[nodiscard]] inline int GenBand(int n) { return n == 1 ? 0 : n <= 3 ? 1 : n <= 6 ? 2 : 3; }
    inline constexpr const char* kGenBands[] = { "1", "2-3", "4-6", "7-10" };

    struct GenRun {   // one run of a mode command
        GenOpts g; int n = 120, fights = 16, check = 24, steps = 7; bool real = false; std::size_t places = 60; std::string cls, cal; std::vector<int> scen; int luck0 = 0, luck1 = 8; std::uint32_t refuse = 0; int modeK = 16; bool iausOnly = false;
    };
    struct GenJob { int k = 0; std::size_t ci = 0; GenScen s; const SimEnv* place = nullptr; };
    [[nodiscard]] inline std::vector<GenJob> GenJobs(const GenRun& o, const std::vector<GenPlace>& places) {
        std::vector<GenJob> j; std::vector<int> ks = o.scen; if (ks.empty()) for (int k = 0; k < o.n; ++k) ks.push_back(k);
        for (const int k : ks) for (std::size_t c = 0; c < std::size(kModeClasses); ++c) { if (!o.cls.empty() && ("," + o.cls + ",").find(std::string(",") + kModeClasses[c] + ",") == std::string::npos) continue; GenJob q; q.k = k; q.ci = c; q.s = GenDraw(o.g, k); if (o.real) q.place = places[static_cast<std::size_t>(k) % places.size()].env; j.push_back(q); }
        return j; }
    [[nodiscard]] inline std::string GenHeader(const GenRun& o, const char* what, std::uint64_t seed) { char b[320]; std::snprintf(b, sizeof b, "%s gen=%s family=%s seed=%llu set=%s caps=strict%s%s against=%s places=%08llx", what, kModeGenVersion, kGenFamilies[o.g.family], static_cast<unsigned long long>(seed), o.real ? "real" : "gen", g_moveCaps.kite ? ",kite" : "", g_moveCaps.disengage ? ",disengage" : "", o.g.against < 0 ? "-" : kModeNames[o.g.against], static_cast<unsigned long long>((o.real ? g_envPool.Hash() : 0) & 0xFFFFFFFFull)); return b; }
    template <class Fn> inline void GenParallel(std::size_t n, int threads, const Fn& fn) { std::atomic<std::size_t> next{ 0 }; std::vector<std::thread> th; for (int i = 0; i < std::max(1, threads); ++i) th.emplace_back([&] { for (;;) { const std::size_t j = next.fetch_add(1); if (j >= n) return; fn(j); } }); for (auto& t : th) t.join(); }
    [[nodiscard]] inline bool GenSetup(const Data& D, const std::vector<SimClass>& classes, const std::vector<SimClass>& t1, const GenRun& o, std::vector<GenPlace>& places, const char* cmd) {
        if (!LoadGenFixtures(g_envPool)) { std::fprintf(stderr, "%s: cannot load the generator's fixtures (make_fixtures.py)\n", cmd); return false; }
        for (const GenTerrain& t : kGenTerrain) for (const char* f : t.fixture) if (!g_envPool.ByName(f)) { std::fprintf(stderr, "%s: no fixture %s\n", cmd, f); return false; }
        if (!o.real) return true;
        const std::vector<GenPlace> all = GenMeasurePlaces(D, classes, t1); places = GenPlaces(all, o.g, o.places); if (places.empty()) { std::fprintf(stderr, "%s: no extracted place with a way\n", cmd); return false; }
        std::array<int, kGenStrata> have{}, took{}; for (const GenPlace& p : all) ++have[static_cast<std::size_t>(p.stratum)]; for (const GenPlace& p : places) ++took[static_cast<std::size_t>(p.stratum)];
        int empty = 0; for (int s = 0; s < kGenStrata; ++s) { if (!have[static_cast<std::size_t>(s)]) ++empty; std::printf("STRATUM %-28s places %2d drawn %2d\n", GenStratumName(s).c_str(), have[static_cast<std::size_t>(s)], took[static_cast<std::size_t>(s)]); }
        std::printf("STRATA %d, empty %d, places %zu, drawn %zu\n", kGenStrata, empty, all.size(), places.size()); return true; }

    inline int RunModeGen(const Data& D, const std::vector<SimClass>& classes, int threads, const GenRun& o) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); std::vector<GenPlace> places; if (!GenSetup(D, classes, t1, o, places, "--mode-gen")) return 2;
        const std::vector<GenJob> jobs = GenJobs(o, places); std::vector<std::string> out(jobs.size()); std::printf("%s\n", GenHeader(o, "MODEGEN", o.g.seed).c_str());
        GenParallel(jobs.size(), threads, [&](std::size_t j) { const GenJob& q = jobs[j]; const FightSpec f = GenSpec(D, classes, kModeClasses[q.ci], q.s, 1.0f, q.place); const Fight F = GenBuild(D, f, q.s, t1); char b[32]; std::snprintf(b, sizeof b, "GENSCEN %d %s ", q.k, kModeClasses[q.ci]); out[j] = b + GenAsked(q.s, q.place ? "extracted" : f.env->name.c_str()) + " | " + GenRead(F); });
        for (const auto& l : out) std::printf("%s\n", l.c_str());
        return 0; }

    inline constexpr std::uint64_t kCalSalt = 0xCA11B8A7EULL, kCheckSalt = 0xC43C4ULL, kAcceptSalt = 0xACCE97ULL;
    // Bisection on the log of the strength ratio: the share of wins falls as it grows. The arm is IAUS with the road OFF (it depends on no value the round tunes).
    inline constexpr float kRatioLo = 0.08f, kRatioHi = 8.0f;
    template <class Win> [[nodiscard]] inline float GenBisect(float target, int steps, const Win& win, bool& ok) {   // win(ratio) -> share
        float lo = std::log(kRatioLo), hi = std::log(kRatioHi), best = 0, bestErr = 1e9f; for (int i = 0; i < steps; ++i) { const float mid = 0.5f * (lo + hi); const float w = win(std::exp(mid)), e = std::fabs(w - target); if (e < bestErr) { bestErr = e; best = mid; } if (e <= 0.04f) break; if (w > target) lo = mid; else hi = mid; }
        ok = bestErr <= 0.15f; return std::exp(best); }
    inline int RunModeCalib(const Data& D, const std::vector<SimClass>& classes, int threads, const GenRun& o) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3); std::vector<GenPlace> places; if (!GenSetup(D, classes, t1, o, places, "--mode-calib")) return 2;
        g_road = Road::Off; const std::vector<GenJob> jobs = GenJobs(o, places); struct Res { float ratio = 1, win = 0; bool ok = false, way = true; int played = 0; double dur = 0, strikes = 0; std::string read, terrain; }; std::vector<Res> res(jobs.size());
        GenParallel(jobs.size(), threads, [&](std::size_t j) { const GenJob& q = jobs[j]; Res& r = res[j]; const auto play = [&](float ratio, int f, std::uint64_t salt, bool read) { const FightSpec fs = GenSpec(D, classes, kModeClasses[q.ci], q.s, ratio, q.place); Fight F = GenBuild(D, fs, q.s, t1); if (read) { r.read = GenRead(F); r.terrain = q.place ? "extracted" : F.env->name; r.way = r.read.find(" way=1") != std::string::npos; } GenLuck(F, fs.seed, f, salt); IausPolicy P; P.T = ActiveTuning(); RunFight(F, P); ++r.played; r.dur += F.now; r.strikes += double(F.t.strikes); return F.t.won; };
            r.ratio = GenBisect(q.s.target, o.steps, [&](float ratio) { int w = 0; for (int f = 0; f < o.fights; ++f) w += play(ratio, f, kCalSalt, false); return static_cast<float>(w) / static_cast<float>(o.fights); }, r.ok);
            int w = 0; for (int f = 0; f < o.check; ++f) w += play(r.ratio, f, kCheckSalt, f == 0); r.win = static_cast<float>(w) / static_cast<float>(std::max(1, o.check)); });
        std::printf("%s fights=%d check=%d steps=%d\n", GenHeader(o, "MODECALRUN", o.g.seed).c_str(), o.fights, o.check, o.steps); std::map<std::string, std::vector<float>> cell; std::map<std::string, std::array<long, 2>> in;
        for (std::size_t j = 0; j < jobs.size(); ++j) { const GenJob& q = jobs[j]; const Res& r = res[j]; std::printf("MODECAL %d %s ratio=%.4f target=%.2f win=%.3f n=%d ok=%d way=%d\n", q.k, kModeClasses[q.ci], r.ratio, q.s.target, r.win, o.check, int(r.ok), int(r.way));
            std::printf("GENFOUGHT %d %s %s | %s | fights=%d dur=%.1f strikes=%.1f win=%.3f\n", q.k, kModeClasses[q.ci], GenAsked(q.s, r.terrain.c_str()).c_str(), r.read.c_str(), r.played, r.dur / std::max(1, r.played), r.strikes / std::max(1, r.played), r.win);
            if (r.ok) cell[std::string(kModeClasses[q.ci]) + " " + GenKindGroup(q.s) + " " + kGenBands[GenBand(q.s.count)] + " " + std::to_string(q.s.level)].push_back(r.ratio); auto& a = in[kModeClasses[q.ci]]; ++a[1]; a[0] += r.way && r.ok && r.win >= 0.30f && r.win <= 0.70f; }
        for (auto& [k, v] : cell) { std::sort(v.begin(), v.end()); std::printf("MODECELL %s n=%zu ratio=%.3f lo=%.3f hi=%.3f\n", k.c_str(), v.size(), v[v.size() / 2], v.front(), v.back()); }
        for (const auto& [k, a] : in) std::printf("MODECALCLASS %s inside 30-70%% %ld of %ld (%.1f%%)\n", k.c_str(), a[0], a[1], 100.0 * double(a[0]) / double(std::max(1l, a[1])));
        return 0; }

}
