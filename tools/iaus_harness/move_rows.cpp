// tools/iaus_harness/move_rows.cpp -- the movement pass's rows (S1, movement-plan.md "Acceptance" of S1) and its golden vectors.
// Linked into the harness by run.sh. MOVE_STANDALONE builds it alone, which is how another compiler replays the same golden file (the line in run.sh).
#include "iaus/MoveDump.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>

using namespace MFO::Iaus;
using namespace MFO::Iaus::Move;
namespace MP = MFO::Iaus::MovePass;
namespace MD = MFO::Iaus::MoveDump;

namespace {
    int g_bad = 0;
    void Row(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_bad; }

    const Profile& NoClass(std::uint8_t) { static const Profile p; return p; }
    // classId 1 = a fixed test class (weights off 1, so the pow branch is in the golden vectors). Not a shipped class: those may be tuned without touching the file.
    const Profile& Profiles(std::uint8_t id) {
        static const Profile one = [] {
            Profile p;
            p.weights = { { Family::CastOffense, Cons::SpotPath, 1.4f }, { Family::CastOffense, Cons::SpotRange, 0.6f }, { Family::Attack, Cons::SpotPath, 0.7f }, { Family::Attack, Cons::SpotExposure, 2.3f }, { Family::CastHeal, Cons::SpotExposure, 1.7f } };
            p.spot[static_cast<std::size_t>(SpotKind::Firing)] = 1.2f; p.spot[static_cast<std::size_t>(SpotKind::HighGround)] = 1.35f; p.spot[static_cast<std::size_t>(SpotKind::Stay)] = 0.9f;
            return p; }();
        return id == 1 ? one : NoClass(id);
    }

    // A map of 128-unit cells ('#' = no floor), two triangles a cell. a_h: the floor's height at a vertex.
    constexpr float kCell = 128.0f;
    MD::Mesh Build(const std::vector<std::string>& a_map, const std::function<float(int, int)>& a_h = {}, float a_cell = kCell) {
        MD::Mesh m;
        const int H = static_cast<int>(a_map.size()), W = static_cast<int>(a_map[0].size());
        for (int y = 0; y <= H; ++y) for (int x = 0; x <= W; ++x) m.verts.push_back({ a_cell * static_cast<float>(x), a_cell * static_cast<float>(y), a_h ? a_h(x, y) : 0.0f });
        const auto vid = [&](int x, int y) { return static_cast<std::uint32_t>(y * (W + 1) + x); };
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::int32_t, int>> edges;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (a_map[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == '#') continue;
            const std::uint32_t a = vid(x, y), b = vid(x + 1, y), c = vid(x + 1, y + 1), d = vid(x, y + 1);
            for (const auto& t : { std::array<std::uint32_t, 3>{ a, b, c }, std::array<std::uint32_t, 3>{ a, c, d } }) {
                Env::Tri T; T.v[0] = t[0]; T.v[1] = t[1]; T.v[2] = t[2];
                const auto id = static_cast<std::int32_t>(m.tris.size());
                m.tris.push_back(T);
                for (int k = 0; k < 3; ++k) {
                    const auto key = std::minmax(t[static_cast<std::size_t>(k)], t[static_cast<std::size_t>((k + 1) % 3)]);
                    const std::pair<std::uint32_t, std::uint32_t> kk{ key.first, key.second };
                    if (const auto it = edges.find(kk); it != edges.end()) {
                        auto& o = m.tris[static_cast<std::size_t>(it->second.first)];
                        o.nb[it->second.second] = id; o.kind[it->second.second] = Env::Edge::Walk;
                        m.tris.back().nb[k] = it->second.first; m.tris.back().kind[k] = Env::Edge::Walk;
                    } else edges[kk] = { id, k };
                }
            }
        }
        m.Build();
        return m;
    }
    Env::Vec3 At(float x, float y, float z = 0.0f) { return { (x + 0.5f) * kCell, (y + 0.5f) * kCell, z }; }

    RankedAction Act(Family f, FormID target, float score, float reach, Sight sight, bool melee = false, std::uint32_t form = 0x100) {
        RankedAction a; a.family = f; a.form = form; a.target = target; a.score = score; a.reach = reach; a.sight = sight; a.melee = melee; return a;
    }
    Member Who(FormID id, Env::Vec3 p, std::initializer_list<RankedAction> acts) {
        Member m; m.id = id; m.p = p; for (const auto& a : acts) if (m.nTop < 3) m.top[m.nTop++] = a; return m;
    }
    Foe Bad(FormID id, Env::Vec3 p, FormID target) { Foe f; f.id = id; f.p = p; f.target = target; return f; }

    std::vector<RayAnswer> Engine(const MD::Mesh& m, const PartyPicture& pic, const std::vector<RayAsk>& asks, const Tuning& t) {   // stands in for the game's rays
        Env::Scratch s; std::vector<RayAnswer> out;
        for (const auto& a : asks) {
            Env::Vec3 p{}; bool have = false;
            for (const auto& f : pic.foes) if (f.id == a.viewer) { p = f.p; have = true; }
            for (const auto& f : pic.party) if (f.id == a.viewer) { p = f.p; have = true; }
            if (have) out.push_back({ a.spot, a.viewer, GeomSees(m.Get(), p, a.p, t, s) ? MFO::Iaus::Tri::Yes : MFO::Iaus::Tri::No, -1.0, {} });
        }
        return out;
    }
    struct Ran { std::vector<RayAsk> asks; std::vector<RayAnswer> answers; std::vector<Assignment> as; int slices = 0; };
    Ran Window(const MD::Mesh& m, PartyPicture& pic, const Tuning& t, float slice = 1.0e9f, bool answer = true, MP::ClassProfiles prof = &NoClass) {
        Ran r; Env::Scratch s; const auto v = m.Get();
        MP::Freeze(pic, v, t);
        for (r.slices = 1; !MP::Phase1(pic, v, s, prof, t, r.asks, slice); ++r.slices) {}
        if (answer) { r.answers = Engine(m, pic, r.asks, t); MP::Answer(pic, r.answers, t); }
        MP::Phase3(pic, prof, t, r.as);
        MP::Adopt(pic, r.as);
        pic.t += static_cast<double>(MoveDecisionSec(t));
        return r;
    }
    bool Same(const std::vector<Assignment>& a, const std::vector<Assignment>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].who != b[i].who || a[i].key != b[i].key || a[i].action != b[i].action || a[i].form != b[i].form || a[i].target != b[i].target || a[i].score != b[i].score) return false;
        return true;
    }
    bool SameAsks(const std::vector<RayAsk>& a, const std::vector<RayAsk>& b) { return a == b; }
    const Assignment* Of(const Ran& r, FormID id) { for (const auto& a : r.as) if (a.who == id) return &a; return nullptr; }

    // A wall across the room with a gap on the right, foes above it, the party below.
    const std::vector<std::string> kRoom = {
        "############", "#..........#", "#..........#", "#..........#", "######.....#", "#..........#", "#..........#", "#..........#", "############" };
    PartyPicture RoomFight() {
        PartyPicture p;
        p.party = { Who(0x11, At(2, 6), { Act(Family::CastOffense, 0x91, 0.8f, 1500.0f, Sight::Occluded), Act(Family::CastBuff, 0x11, 0.3f, 0.0f, Sight::None) }),
                    Who(0x12, At(3, 6), { Act(Family::Attack, 0x91, 0.7f, 1800.0f, Sight::Occluded) }),
                    Who(0x13, At(4, 6), { Act(Family::CastHeal, 0x14, 0.9f, 900.0f, Sight::Occluded) }),
                    Who(0x14, At(8, 2), { Act(Family::Attack, 0x92, 0.6f, 150.0f, Sight::Visible, true), Act(Family::Retreat, 0, 0.5f, 0.0f, Sight::None) }) };
        p.foes = { Bad(0x91, At(2, 1), 0x14), Bad(0x92, At(9, 1), 0x14) };
        return p;
    }

    void Determinism() {
        std::printf("\n== movement: order and slicing ==\n");
        const Tuning& t = ShippedTuning();
        const auto mesh = Build(kRoom);
        PartyPicture ref = RoomFight(); std::vector<Ran> want;
        for (int w = 0; w < 3; ++w) want.push_back(Window(mesh, ref, t));
        std::array<int, 4> idx{ 0, 1, 2, 3 }; bool ok = true; int n = 0;
        do {
            const PartyPicture base = RoomFight(); PartyPicture p = base;
            for (std::size_t i = 0; i < 4; ++i) p.party[i] = base.party[static_cast<std::size_t>(idx[i])];
            std::reverse(p.foes.begin(), p.foes.end());
            for (int w = 0; w < 3; ++w) { const auto r = Window(mesh, p, t); ok = ok && Same(r.as, want[static_cast<std::size_t>(w)].as) && SameAsks(r.asks, want[static_cast<std::size_t>(w)].asks); }
            ++n;
        } while (std::next_permutation(idx.begin(), idx.end()));
        Row(ok && n == 24, "24 permutations of four followers: the same asks and assignments, three windows");
        for (const float slice : { 0.0f, 1.0f, 50.0f }) {
            PartyPicture p = RoomFight(); bool same = true; int slices = 0;
            for (int w = 0; w < 3; ++w) { const auto r = Window(mesh, p, t, slice); same = same && Same(r.as, want[static_cast<std::size_t>(w)].as) && SameAsks(r.asks, want[static_cast<std::size_t>(w)].asks); slices += r.slices; }
            char b[120]; std::snprintf(b, sizeof b, "phase 1 in slices of %.0f ms (%d calls): the same result", static_cast<double>(slice), slices);
            Row(same && (slice > 0.0f || slices > 20), b);
        }
        // two followers who are alike in everything: the tie goes to the lower FormID, whichever is stored first
        SpotKey won = 0;
        for (const bool swap : { false, true }) {
            PartyPicture p;
            p.party = { Who(0x21, At(3, 6), { Act(Family::Attack, 0x91, 0.7f, 1800.0f, Sight::Occluded) }), Who(0x22, At(3, 6), { Act(Family::Attack, 0x91, 0.7f, 1800.0f, Sight::Occluded) }) };
            if (swap) std::swap(p.party[0], p.party[1]);
            p.foes = { Bad(0x91, At(2, 1), 0x21) };
            p.foes[0].target = 0;
            const auto r = Window(mesh, p, t);
            const auto* a = Of(r, 0x21); const auto* b = Of(r, 0x22);
            if (!swap) won = a->key;
            Row(a->spot >= 0 && b->spot >= 0 && a->key != b->key && a->key == won && a->score >= b->score, swap ? "twins (stored swapped): the lower FormID has the same, better spot" : "twins: two spots, the lower FormID has the better one");
        }
    }

    void Tunables() {
        std::printf("\n== movement: tunables ==\n");
        Tuning t = ShippedTuning();
        t.moveDecisionSec = 0.1f; Row(MoveDecisionSec(t) == 0.5f, "moveDecisionSec 0.1 reads as the 0.5 floor");
        t.moveDecisionSec = std::numeric_limits<float>::quiet_NaN(); Row(MoveDecisionSec(t) == 0.5f, "a NaN reads as the floor");
        t.moveDecisionSec = 0.75f; Row(MoveDecisionSec(t) == 0.75f, "0.75 stays");
        Row(SetTuningScalar(t, "t.spotPinKinds", 1.0f) && SetTuningScalar(t, "t.moveDecisionSec", 0.2f) && MoveDecisionSec(t) == 0.5f, "t.spot* / t.moveDecisionSec are settable, the floor still holds");
        ClassCurves::Row row; Row(ClassCurves::Build(row).spot == Profile{}.spot, "a fresh class row has every spot weight 1");
        Row(ClassCurves::SetSpotWeight(row, SpotKind::Firing, 1.5f) && ClassCurves::Build(row).spot[1] == 1.5f && !ClassCurves::SetSpotWeight(row, SpotKind::Firing, 0.0f), "a class spot weight reaches the profile, 0 is refused");
        RoadCaps caps; Tuning s = ShippedTuning();
        const std::uint32_t pin = KindBit(SpotKind::Cover) | KindBit(SpotKind::Chokepoint) | KindBit(SpotKind::Flank);
        Row((AllowedKinds(caps, s) & pin) == 0, "pin kinds are off as shipped");
        caps.pin = true; Row((AllowedKinds(caps, s) & pin) == 0, "a pin road alone does not open them");
        caps.pin = false; s.spotPinKinds = 1.0f; Row((AllowedKinds(caps, s) & pin) == 0, "the tunable alone does not open them (the need is unmet)");
        caps.pin = true; Row((AllowedKinds(caps, s) & pin) == pin, "both open them");
        caps.point = false; Row(AllowedKinds(caps, s) == KindBit(SpotKind::Stay), "no point road: stay only");
        ClassGate k; k.resolved = true; k.taunt = true; k.casterLine = false; k.weaponBow = k.weaponCrossbow = k.weaponStaff = false; k.spellsOnClass = false;
        const auto knight = SpotKindsFor(k);
        Row((knight & KindBit(SpotKind::Peel)) && (knight & KindBit(SpotKind::Chokepoint)) && !(knight & KindBit(SpotKind::RetreatExit)) && !(knight & KindBit(SpotKind::Firing)), "a Knight: peel and chokepoint, never retreat");
        ClassGate th; th.resolved = true; th.sneakClass = true; th.spellsOnClass = false;
        Row((SpotKindsFor(th) & KindBit(SpotKind::Flank)) && (SpotKindsFor(th) & KindBit(SpotKind::RetreatExit)), "a Thief: flank and retreat");
        Row(kMoveBudget.factsPerWindow == 32 && kMoveBudget.answeredSpots == 24, "the ray budget: 32 facts a window over at most 24 spots");
    }

    void Scoring() {
        std::printf("\n== movement: scoring ==\n");
        const Tuning& t = ShippedTuning(); const Profile p;
        SpotFacts f; f.pathSec = 1.0f; f.rangeX = 0.95f;
        SpotFacts yes = f, no = f; yes.sees = MFO::Iaus::Tri::Yes; no.sees = MFO::Iaus::Tri::No;
        std::vector<Term> terms; const float u = ScoreSpot(f, Family::CastOffense, p, t, &terms);
        bool in = false; for (const auto& x : terms) if (x.cons == Cons::SpotSight) in = true;
        Row(!in && ScoreSpot(yes, Family::CastOffense, p, t) > u && u > ScoreSpot(no, Family::CastOffense, p, t), "unknown sight is NOT APPLICABLE: no term, under clear, over blocked");
        SpotFacts nan = f; nan.exits = std::numeric_limits<float>::quiet_NaN(); Row(ScoreSpot(nan, Family::CastOffense, p, t) == 0.0f, "a NaN fact never scores");
        SpotFacts far = f; far.leashX = 1.1f; Row(ScoreSpot(far, Family::CastOffense, p, t) == 0.0f, "past the leash: veto");
        SpotFacts dark = f; dark.light = 100.0f; Row(ScoreSpot(dark, Family::Step, p, t) < u, "light is an input hook: a lit spot scores lower, an unknown one is untouched");
        Candidate a, b, c; a.family = Family::CastOffense; b.family = Family::Attack; c.family = Family::CastHeal; a.target = 1; b.target = 2; c.target = 3;
        const std::vector<Candidate> cands{ a, b, c, a };
        Decision d; d.ranked = { { 0, 0.9f, 0.9f, 1, false }, { 2, 0.8f, 0.8f, 1, false }, { 1, 0.5f, 0.4f, 1, true }, { 3, 0.2f, 0.2f, 1, false } };
        std::array<RankedAction, 3> top; const auto n = TopRanked(d, cands, top);
        Row(n == 3 && top[0].family == Family::Attack && top[1].family == Family::CastOffense && top[2].family == Family::CastHeal && top[1].score == 0.9f, "TopRanked: the held action, then the next two of the ranking");
    }

    void Behaviour() {
        std::printf("\n== movement: what it picks ==\n");
        const Tuning& t = ShippedTuning();
        const auto mesh = Build(kRoom); const auto v = mesh.Get(); Env::Scratch s;
        {
            PartyPicture p = RoomFight(); const auto r = Window(mesh, p, t);
            const auto* mage = Of(r, 0x11); const auto* bow = Of(r, 0x12); const auto* heal = Of(r, 0x13);
            Row(mage->spot >= 0 && GeomSees(v, At(2, 1), mage->p, t, s), "a mage behind a wall walks to a spot that sees his target");
            Row(bow->spot >= 0 && bow->key != mage->key && GeomSees(v, At(2, 1), bow->p, t, s), "an archer too, to another spot");
            Row(heal->spot >= 0 && GeomSees(v, At(8, 2), heal->p, t, s) && Dist(heal->p, At(8, 2)) <= 900.0f, "a healer round the corner walks to where he reaches the hurt one");
            const auto r2 = Window(mesh, p, t);
            Row(Of(r2, 0x11)->kept && Of(r2, 0x11)->key == mage->key, "the next window keeps a committed spot");
            // committed and unmarked: not re-scored, whatever else is on offer
            for (auto& m : p.party) if (m.id == 0x12) m.top[0].score = 5.0f;
            const auto r3 = Window(mesh, p, t);
            Row(Of(r3, 0x11)->kept && Of(r3, 0x11)->score == 0.0f && Of(r3, 0x12)->kept, "committed and unmarked: a fixed assignment, not scored");
            for (auto& m : p.party) if (m.id == 0x11) m.marks = kMarkHitHard;
            const auto r4 = Window(mesh, p, t);
            Row(Of(r4, 0x11)->score > 0.0f, "an event marks him: he is scored in the next pass");
            Row(Of(r4, 0x11)->key == mage->key, "... and with nothing better he goes on to the same spot (no dither)");
        }
        {
            PartyPicture p = RoomFight(); Window(mesh, p, t);
            const auto key = p.party[0].commit.key; p.facts[{ key, 0x91 }].sees = MFO::Iaus::Tri::No; p.facts[{ key, 0x91 }].t = p.t;
            MP::Freeze(p, v, t);
            Row((p.party[0].seen & kMarkSightLost) && !p.party[0].fixed, "a ray that says the kept spot lost its target marks him");
            PartyPicture q = RoomFight(); Window(mesh, q, t); q.foes.erase(q.foes.begin()); MP::Freeze(q, v, t);
            Row((q.party[0].seen & kMarkTargetGone) != 0, "a target that is gone marks him");
            PartyPicture z = RoomFight(); Window(mesh, z, t); const auto seen = [&] { int n = 0; for (const auto& [k, x] : z.facts) if (k.second >= 0x91 && k.second <= 0x92) ++n; return n; }; const int had = seen(); for (auto& f : z.foes) f.p.x += 200.0f; MP::Freeze(z, v, t);
            Row(had > 0 && seen() == 0, "a fact is gone when its viewer moved");
        }
        {
            PartyPicture p; p.party = RoomFight().party; p.foes = RoomFight().foes;
            MD::Mesh none; const auto r = Window(none, p, t);
            bool stay = r.as.size() == 4; for (const auto& a : r.as) stay = stay && a.spot < 0;
            Row(stay && r.asks.empty(), "no mesh: everyone stays, nothing is asked");
            PartyPicture q = RoomFight(); for (auto& m : q.party) m.nTop = 0;
            const auto r2 = Window(mesh, q, t); stay = r2.as.size() == 4; for (const auto& a : r2.as) stay = stay && a.spot < 0 && a.action == Family::kCount;
            Row(stay, "no ranked action: stay, with no action");
            PartyPicture u = RoomFight(); std::vector<RayAsk> asks; std::vector<Assignment> as; Env::Scratch sc;
            MP::Freeze(u, v, t); MP::Phase1(u, v, sc, &NoClass, t, asks, 0.0f); MP::Phase3(u, &NoClass, t, as);
            stay = true; for (const auto& a : as) stay = stay && a.spot < 0;
            Row(stay && u.counts.unfinished == 1, "a window whose phase 1 did not finish moves nobody, and counts it");
            PartyPicture b = RoomFight(); b.party[1].p.x = std::numeric_limits<float>::quiet_NaN(); b.party[2].top[0].score = std::numeric_limits<float>::infinity();
            const auto r3 = Window(mesh, b, t); Row(r3.as.size() == 4 && Of(r3, 0x12)->spot < 0 && Of(r3, 0x13)->spot < 0, "a NaN place or score: he stays, the rest decide");
        }
        {   // sharing: two casters on one foe ask one sight fact per spot
            PartyPicture p = RoomFight(); p.party.resize(2);
            const auto r = Window(mesh, p, t, 1.0e9f, false);
            bool twice = false; for (std::size_t i = 0; i < r.asks.size(); ++i) for (std::size_t j = i + 1; j < r.asks.size(); ++j) if (r.asks[i] == r.asks[j]) twice = true;
            Row(!r.asks.empty() && !twice, "a (spot, foe) question is asked once for the party");
            const auto r2 = Window(mesh, p, t, 1.0e9f, false);
            Row(Of(r2, 0x11) != nullptr && p.facts.empty(), "unanswered stays unanswered (no guess)");
        }
        {   // a retreat: the dead end on the left is nearer than the way out on the right
            const auto m2 = Build({ "##########", "#...######", "###.######", "#........#", "#........#", "######.###", "######.###", "#........#", "#........#", "##########" });
            PartyPicture p; p.party = { Who(0x31, At(4, 3), { Act(Family::Retreat, 0, 0.9f, 0.0f, Sight::None) }) };
            p.foes = { Bad(0x91, At(1, 3), 0x31) };
            const auto r = Window(m2, p, t); const auto* a = Of(r, 0x31);
            Row(a->spot >= 0 && p.pool[static_cast<std::size_t>(a->spot)].exits > 0 && Dist(a->p, At(1, 3)) > Dist(At(4, 3), At(1, 3)), "a retreat goes away from the foe, to a place with a way out");
        }
        {   // a step up: high ground
            const auto m3 = Build({ "##########", "#........#", "#........#", "#........#", "##########" }, [](int x, int) { return x >= 6 ? 160.0f : x == 5 ? 80.0f : 0.0f; });
            PartyPicture p; p.party = { Who(0x41, At(4, 2), { Act(Family::Attack, 0x91, 0.7f, 2000.0f, Sight::Visible) }) };
            p.foes = { Bad(0x91, At(1, 2), 0) };
            Window(m3, p, t); bool any = false; for (const auto& s2 : p.pool) if (s2.kinds & KindBit(SpotKind::HighGround)) any = true;
            Row(any, "high ground is found over the target");
        }
        {   // doorway: one body
            Tuning pin = t; pin.spotPinKinds = 1.0f;
            const auto m4 = Build({ "#########", "#...#...#", "#.......#", "#...#...#", "#########" });
            PartyPicture p; p.caps.pin = true;
            for (int i = 0; i < 3; ++i) p.party.push_back(Who(0x51 + static_cast<FormID>(i), At(1.0f + static_cast<float>(i) * 0.6f, 2), { Act(Family::Attack, 0x91, 0.7f, 150.0f, Sight::Visible, true) }));
            p.foes = { Bad(0x91, At(7, 2), 0) };
            for (int w = 0; w < 3; ++w) {
                const auto r = Window(m4, p, pin);
                int in = 0, doorways = 0; for (const auto& a : r.as) if (a.spot >= 0 && p.pool[static_cast<std::size_t>(a.spot)].portals[0]) ++in;
                for (const auto& x : p.pool) if (x.portals[0]) ++doorways;
                std::vector<std::uint64_t> held; bool clash = false;
                for (const auto& a : r.as) if (a.spot >= 0) for (const auto id : p.pool[static_cast<std::size_t>(a.spot)].portals) if (id) { if (std::find(held.begin(), held.end(), id) != held.end()) clash = true; held.push_back(id); }
                Row(!clash && doorways > 0 && in >= 1, "one body per doorway, and a doorway is held");
            }
            PartyPicture q = p; q.Clear(); Row(q.party.empty() && q.facts.empty(), "Clear forgets everything");
        }
    }

    PartyPicture Crowd(int n) {
        PartyPicture p;
        for (int i = 0; i < n; ++i) {
            const auto id = 0x60u + static_cast<FormID>(i);
            p.party.push_back(Who(id, At(2.0f + static_cast<float>(i % 5) * 4.0f, 14.0f + static_cast<float>(i / 5) * 3.0f), { Act(Family::CastOffense, 0x90u + static_cast<FormID>(i % 5), 0.8f, 2200.0f, i % 2 ? Sight::Visible : Sight::Occluded),
                Act(Family::CastHeal, 0x60u + static_cast<FormID>((i + 1) % n), 0.5f, 1200.0f, Sight::Visible, false, 0x200), Act(Family::BackOff, 0, 0.4f, 0.0f, Sight::None) }));
            p.party.back().leash = 3000.0f; p.party.back().anchor = At(12, 14);
        }
        for (int i = 0; i < 6; ++i) p.foes.push_back(Bad(0x90u + static_cast<FormID>(i), At(3.0f + static_cast<float>(i) * 3.0f, 4.0f + static_cast<float>(i % 2)), 0x60u + static_cast<FormID>((i * 2) % n)));
        return p;
    }
    std::vector<std::string> Hall() {
        std::vector<std::string> m(24, std::string(26, '.'));
        for (std::size_t y = 0; y < m.size(); ++y) for (std::size_t x = 0; x < m[y].size(); ++x) if (y == 0 || x == 0 || y + 1 == m.size() || x + 1 == m[y].size() || (y % 6 == 5 && x % 5 == 2) || (y == 9 && x > 5 && x < 16)) m[y][x] = '#';
        return m;
    }
    void Budgets() {
        std::printf("\n== movement: budgets ==\n");
        const Tuning& t = ShippedTuning();
        const auto mesh = Build(Hall());
        PartyPicture p = Crowd(10); bool asks = true, spots = true, pool = true, carried = true, nearest = true; std::uint32_t hits = 0; double ms = 0, p3 = 0, worst = 0;
        std::vector<RayAsk> tail;
        for (int w = 0; w < 6; ++w) {
            Env::Scratch s; const auto v = mesh.Get(); Ran r;
            for (auto& m : p.party) { m.p.x += 20.0f; m.marks = kMarkOrder; }
            const auto t0 = std::chrono::steady_clock::now();
            MP::Freeze(p, v, t);
            const auto was = p.carried;
            while (!MP::Phase1(p, v, s, &NoClass, t, r.asks, 3.0f)) {}
            const auto t1 = std::chrono::steady_clock::now();
            MP::Answer(p, Engine(mesh, p, r.asks, t), t);
            const auto t2 = std::chrono::steady_clock::now();
            MP::Phase3(p, &NoClass, t, r.as);
            const auto t3 = std::chrono::steady_clock::now();
            MP::Adopt(p, r.as); p.t += 0.5;
            ms += std::chrono::duration<double, std::milli>(t1 - t0).count(); p3 += std::chrono::duration<double, std::milli>(t3 - t2).count(); worst = (std::max)(worst, std::chrono::duration<double, std::milli>(t3 - t2).count());
            asks = asks && r.asks.size() <= static_cast<std::size_t>(kMoveBudget.factsPerWindow);
            std::set<SpotKey> keys; for (const auto& a : r.asks) if (!(a.spot & kStayBit)) keys.insert(a.spot);
            spots = spots && keys.size() <= static_cast<std::size_t>(kMoveBudget.answeredSpots);
            pool = pool && p.pool.size() <= static_cast<std::size_t>(kMoveBudget.spots) && p.counts.paths <= static_cast<std::uint32_t>(kMoveBudget.paths) && p.counts.triples <= static_cast<std::uint32_t>(kMoveBudget.triples);
            hits += p.counts.spotsDropped + p.counts.pathsSkipped + p.counts.asksShort;
            for (std::size_t i = 0; i < p.counts.asksCarried && i < r.asks.size(); ++i) carried = carried && std::find(was.begin(), was.end(), r.asks[i]) != was.end();
            tail = p.carried;
            std::map<std::uint32_t, std::int32_t> last;   // what a path budget drops is the far end of each member's own list
            for (const auto& u : p.work.pathUnits) { auto [it, fresh] = last.try_emplace(u.member, u.near); nearest = nearest && (fresh || it->second <= u.near); it->second = u.near; }
            std::printf("    window %d: pool %zu of %u, paths %u, asks %zu (%u carried), facts %zu, short %u, dropped %u, skipped %u, triples %u\n", w, p.pool.size(), p.counts.generated, p.counts.paths, r.asks.size(), p.counts.asksCarried, p.facts.size(), p.counts.asksShort, p.counts.spotsDropped, p.counts.pathsSkipped, p.counts.triples);
        }
        std::printf("    a party of ten: phase 1 %.2f ms, phase 3 %.2f ms a window (worst %.2f)\n", ms / 6.0, p3 / 6.0, worst);
        Row(asks, "never more ray asks than factsPerWindow");
        Row(spots, "never more asked spots than answeredSpots");
        Row(pool, "pool, paths and triples stay inside their budgets");
        Row(hits > 0 && nearest, "a budget that is hit is counted, and it serves each member's nearest first");
        {   // the pool is made one spot a unit: the dear halves (exits flood, offers) never run in one call
            PartyPicture q = Crowd(10); Env::Scratch s; const auto v = mesh.Get(); std::vector<RayAsk> a; std::size_t place = 0, offer = 0; double top = 0;
            MP::Freeze(q, v, t);
            for (;;) {
                const auto was = q.work.stage; const auto t0 = std::chrono::steady_clock::now();
                const bool end = MP::Phase1(q, v, s, &NoClass, t, a, 0.0f);
                top = (std::max)(top, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                if (was == Work::Stage::Place) ++place;
                if (was == Work::Stage::Offer) ++offer;
                if (end) break;
            }
            std::printf("    the dearest unit %.2f ms (the slice is %.0f)\n", top, static_cast<double>(kMoveBudget.sliceMs));
            Row(place == q.counts.generated + 1 && offer == q.pool.size() + 1, "the pool is built one spot a unit");
        }
        Row(carried, "what a window could not ask goes first in the next");
        Row(worst < 250.0, "phase 3 is one bounded slice (host time, a sanity bound)");
    }

    // ── the Opus review's findings (review-move-s1.md), one block each ──
    void Review() {
        std::printf("\n== movement: the review's findings ==\n");
        const Tuning& t = ShippedTuning();
        using Tri3 = MFO::Iaus::Tri;
        {   // F1: two retreaters back to back, each with his own foe on the other side
            // a hall with room to the sides: between two foes in a corridor three squares wide there is no way on, and none is offered (CheckExit)
            const auto corridor = Build({ "##########################", "#........................#", "#........................#", "#........................#", "##########################" });
            std::vector<std::string> wide(25, "#........................#"); wide.front() = wide.back() = "##########################";
            const auto m = Build(wide);
            int picks = 0, bad = 0, toward = 0, shut = 0;
            for (const float gap : { 1.0f, 2.0f, 3.0f, 4.0f }) for (const float off : { 1.5f, 2.0f, 3.0f }) {
                const float x = 12.0f;
                const auto Pair = [&](float y) { PartyPicture q; q.party = { Who(0x31, At(x, y), { Act(Family::Retreat, 0, 0.9f, 0.0f, Sight::None) }), Who(0x32, At(x + gap, y), { Act(Family::Retreat, 0, 0.9f, 0.0f, Sight::None) }) };
                    q.foes = { Bad(0x91, At(x + gap + off, y), 0x32), Bad(0x92, At(x - off, y), 0x31) }; return q; };
                { PartyPicture q = Pair(2); for (const auto& a : Window(corridor, q, t).as) if (a.kinds & KindBit(SpotKind::RetreatExit)) ++shut; }
                PartyPicture p = Pair(12);
                for (int w = 0; w < 3; ++w) {
                    const auto r = Window(m, p, t);
                    for (const auto& a : r.as) if (a.spot >= 0) {
                        const auto mine = a.who == 0x31 ? p.foes[1].p : p.foes[0].p; const auto at = a.who == 0x31 ? At(x, 12) : At(x + gap, 12);   // Freeze sorted the foes: 0x91, 0x92
                        ++picks; if (!(Dist(mine, a.p) > Dist(mine, at) + 200.0f)) ++bad;
                        if (Dist(mine, a.p) < Dist(mine, at)) ++toward;
                    }
                }
            }
            char b[160]; std::snprintf(b, sizeof b, "F1 back to back retreaters: every pick is 200 u farther from his OWN foe (%d picks, %d break it, %d toward)", picks, bad, toward);
            Row(picks >= 24 && bad == 0, b);
            Row(shut == 0, "F1 between two foes in a corridor no retreat place is offered (no way on)");
            // a kept retreat that a new attacker stands in the way of is not kept
            PartyPicture k; k.party = { Who(0x31, At(8, 2), { Act(Family::Retreat, 0, 0.9f, 0.0f, Sight::None) }) }; k.foes = { Bad(0x92, At(6, 2), 0x31) };
            const auto r1 = Window(m, k, t); const auto was = Of(r1, 0x31)->p;
            k.foes = { Bad(0x93, { was.x, was.y, 0.0f }, 0x31) }; k.party[0].marks = kMarkHitHard;
            const auto r2 = Window(m, k, t);
            Row(Of(r1, 0x31)->spot >= 0 && Dist(Of(r2, 0x31)->p, k.foes[0].p) >= Dist(k.party[0].p, k.foes[0].p), "F1 a kept retreat whose way out is now the attacker's place is given up");
            // a melee attack never takes an archer's firing spot
            const auto room = Build(kRoom);
            PartyPicture q; q.party = { Who(0x41, At(2, 6), { Act(Family::Attack, 0x91, 0.7f, 1800.0f, Sight::Occluded) }), Who(0x42, At(3, 6), { Act(Family::Attack, 0x91, 0.9f, 150.0f, Sight::Occluded, true) }) };
            q.foes = { Bad(0x91, At(2, 1), 0) };
            const auto r3 = Window(room, q, t); const auto* me = Of(r3, 0x42);
            Row(Of(r3, 0x41)->spot >= 0 && !(me->kinds & KindBit(SpotKind::Firing)) && me->spot < 0, "F1 a firing spot of an archer is not offered to a melee attack");
        }
        {   // S2 review F4: the peel rules. The row's own generator, then the pass
            const std::vector<std::string> hall{ "##############", "#............#", "#............#", "#............#", "#............#", "##############" };
            const auto m = Build(hall);
            const auto fight = [&](RankedAction a, float foeX) { PartyPicture p; p.party = { Who(0x51, At(2, 2), { a }), Who(0x52, At(8, 2), {}) }; p.party[1].decides = false; p.foes = { Bad(0x91, At(foeX, 2), 0x52) }; return p; };
            const auto peels = [&](PartyPicture p) { Window(m, p, t); int n = 0; for (const auto& o : p.work.offers) if (o.kinds & KindBit(SpotKind::Peel)) ++n; return n; };
            const RankedAction blade = Act(Family::Attack, 0x91, 0.7f, 150.0f, Sight::Visible, true), bow = Act(Family::Attack, 0x91, 0.7f, 1800.0f, Sight::Visible);
            Row(peels(fight(blade, 9.5f)) > 0, "F4 a blade is offered a peel place between an attacker and the ally he stands on");
            Row(peels(fight(bow, 9.5f)) == 0, "F4 a bow never is");
            Row(peels(fight(Act(Family::CastControl, 0x91, 0.7f, 1800.0f, Sight::Visible), 9.5f)) > 0 && peels(fight(Act(Family::Taunt, 0x91, 0.7f, 1800.0f, Sight::Visible), 9.5f)) > 0, "F4 a control and a taunt are");
            Row(Dist(At(12, 2), At(8, 2)) > 2.0f * t.spotPeelU && peels(fight(blade, 12.0f)) == 0, "F4 nor anyone for an attacker farther from the ally than twice spotPeelU");
            // a kept peel: the attacker leaves the ally, the place is given up
            PartyPicture k = fight(blade, 9.5f); Profile pr; pr.spot[static_cast<std::size_t>(SpotKind::Peel)] = 4.0f;
            static Profile held; held = pr; const MP::ClassProfiles prof = [](std::uint8_t) -> const Profile& { return held; };
            const auto r1 = Window(m, k, t, 1.0e9f, true, prof); const bool got = k.party[0].commit.on && (k.party[0].commit.kinds & KindBit(SpotKind::Peel));
            PartyPicture k2 = k;
            k.party[0].marks = kMarkHitHard; const auto r2 = Window(m, k, t, 1.0e9f, true, prof);
            k2.foes[0].p = At(1, 4); k2.party[0].marks = kMarkHitHard; const auto r3 = Window(m, k2, t, 1.0e9f, true, prof);
            Row(got && Of(r1, 0x51)->spot >= 0 && k.party[0].commit.on && Of(r2, 0x51)->key == Of(r1, 0x51)->key, "F4 a kept peel stands while the attacker stands on the ally");
            Row(got && !(k2.party[0].commit.on && (k2.party[0].commit.kinds & KindBit(SpotKind::Peel))), "F4 a kept peel is given up when the attacker left the ally");
        }
        {   // F2: the target runs 185 u a window behind a wall while the shooter is on his way
            const auto m = Build({ "##############################", "#............................#", "#............................#", "######....##########.........#", "#............................#", "#............................#", "#............................#", "##############################" });
            Env::Scratch s; const auto v = m.Get();
            for (const bool prompt : { true, false }) {   // prompt: the freed window's own questions are answered too
            PartyPicture p; p.party = { Who(0x11, At(15, 6), { Act(Family::CastOffense, 0x91, 0.8f, 2600.0f, Sight::Occluded) }) };
            p.foes = { Bad(0x91, At(22, 1), 0) };
            const auto r = Window(m, p, t); const SpotKey key = Of(r, 0x11)->key; const auto spot = Of(r, 0x11)->p;
            int firstNo = -1, marked = -1; SpotKey then = key;
            for (int w = 1; w <= 12 && marked < 0; ++w) {
                p.foes[0].p.x -= 185.0f;
                if (firstNo < 0 && !GeomSees(v, p.foes[0].p, spot, t, s)) firstNo = w;
                const auto x = Window(m, p, t, 1.0e9f, prompt || firstNo < 0 || w <= firstNo);
                if (!Of(x, 0x11)->kept || Of(x, 0x11)->score > 0.0f) { marked = w; then = Of(x, 0x11)->key; }
            }
            char b[160]; std::snprintf(b, sizeof b, "F2 a foe that runs out of the kept spot's sight marks him on time (blind in window %d, freed in %d)", firstNo, marked);
            Row(Of(r, 0x11)->spot >= 0 && firstNo > 0 && marked > 0 && marked <= firstNo + 2, b);
            Row(then != key, prompt ? "F2 ... and the blind spot is not taken again" : "F2 ... nor when that window's rays are not back (the No that freed him still counts)");
            }
            // a still foe, and a door that closes: the kept spot's answer is asked again before it is five seconds old
            PartyPicture q; q.party = { Who(0x11, At(15, 6), { Act(Family::CastOffense, 0x91, 0.8f, 2600.0f, Sight::Occluded) }) }; q.foes = { Bad(0x91, At(22, 1), 0) };
            const auto r1 = Window(m, q, t); const SpotKey k1 = Of(r1, 0x11)->key; int freed = -1;
            for (int w = 1; w <= 12 && freed < 0; ++w) {
                std::vector<RayAsk> asks; std::vector<Assignment> as; MP::Freeze(q, v, t);
                if (q.party[0].seen & kMarkSightLost) freed = w;
                while (!MP::Phase1(q, v, s, &NoClass, t, asks, 1.0e9f)) {}
                std::vector<RayAnswer> no; for (const auto& a : asks) no.push_back({ a.spot, a.viewer, a.spot == k1 ? Tri3::No : Tri3::Yes, -1.0, {} });
                MP::Answer(q, no, t); MP::Phase3(q, &NoClass, t, as); MP::Adopt(q, as); q.t += 0.5;
            }
            Row(freed > 0 && freed <= 3, "F2 a still foe behind a closed door: freed as soon as the kept answer is spotKeptAgeSec old");
        }
        {   // F3: what the ray budget did not ask may not beat what was answered and seen
            const auto m = Build(kRoom); bool ok = true; float worst = 0.0f; int n = 0;
            for (const Family f : { Family::CastOffense, Family::Attack, Family::CastControl }) for (const int threats : { 1, 2 }) {
                const auto run = [&](bool answered) {
                    PartyPicture p; p.party = { Who(0x11, At(2, 6), { Act(f, 0x91, 0.8f, 1500.0f, Sight::Occluded) }) };
                    p.foes = { Bad(0x91, At(2, 1), 0x11) }; if (threats == 2) p.foes.push_back(Bad(0x92, At(3, 1), 0x11));
                    Env::Scratch s; std::vector<RayAsk> asks; std::vector<Assignment> as; const auto v = m.Get();
                    MP::Freeze(p, v, t); while (!MP::Phase1(p, v, s, &NoClass, t, asks, 1.0e9f)) {}
                    std::vector<RayAnswer> a; if (answered) for (const auto& k : asks) a.push_back({ k.spot, k.viewer, Tri3::Yes, -1.0, {} });   // sees, and is seen by all
                    MP::Answer(p, a, t); MP::Phase3(p, &NoClass, t, as);
                    return as[0].spot >= 0 ? as[0].score : -1.0f;
                };
                const float off = run(false), on = run(true); ++n;
                if (!(off > 0.0f && on > 0.0f)) { ok = false; continue; }
                worst = (std::max)(worst, off / on - 1.0f);
            }
            char b[160]; std::snprintf(b, sizeof b, "F3 an unanswered spot outranks a seen and exposed one by at most spotPreempt (%.1f%% of %.0f%%)", static_cast<double>(worst) * 100.0, static_cast<double>(t.spotPreempt) * 100.0);
            Row(ok && n == 6 && worst <= t.spotPreempt, b);
            // not as good as a hidden one either
            const Profile none; SpotFacts f; f.pathSec = 1.0f; f.sees = Tri3::Yes; SpotFacts hid = f, unk = f, exp = f; hid.exposure = 0.0f; unk.exposure = t.spotUnknownExposure; exp.exposure = 1.0f;
            const float a = ScoreSpot(hid, Family::CastOffense, none, t), c = ScoreSpot(unk, Family::CastOffense, none, t), d = ScoreSpot(exp, Family::CastOffense, none, t);
            Row(a > c && c > d, "F3 unanswered exposure lies between hidden and seen");
            PartyPicture p; p.party = { Who(0x11, At(2, 6), { Act(Family::CastOffense, 0x91, 0.8f, 1500.0f, Sight::Occluded) }) }; p.foes = { Bad(0x91, At(2, 1), 0x11) };
            const auto r = Window(m, p, t); bool stay = false; for (const auto& k : r.asks) if (k.spot == KeyOfStay(0x11) && k.viewer == 0x91) stay = true;
            Row(stay, "F3 every threatened member's own place is asked, whatever kinds he is offered");
        }
        {   // F4: an event between Freeze and Adopt
            const auto m = Build(kRoom); PartyPicture p = RoomFight(); Window(m, p, t);
            const auto mid = [&](std::uint32_t before, std::uint32_t during) {
                Env::Scratch s; std::vector<RayAsk> asks; std::vector<Assignment> as; const auto v = m.Get();
                p.party[0].marks |= before; MP::Freeze(p, v, t); while (!MP::Phase1(p, v, s, &NoClass, t, asks, 1.0e9f)) {}
                p.party[0].marks |= during;
                MP::Answer(p, Engine(m, p, asks, t), t); MP::Phase3(p, &NoClass, t, as); MP::Adopt(p, as); p.t += 0.5;
                return as[0].score > 0.0f;
            };
            const bool kept = !mid(0, kMarkHitHard), scored = mid(0, 0), quiet = !mid(0, 0);
            Row(kept && scored && quiet, "F4 an event set mid-window frees him in the next one, once");
            const bool one = mid(kMarkHitHard, kMarkHitHard), two = mid(0, 0);
            Row(one && two, "F4 ... also when the same event already freed him in this one");
            p.party[0].marks = kMarkOrder; MP::Freeze(p, m.Get(), t); MP::Freeze(p, m.Get(), t);
            Row((p.party[0].seen & kMarkOrder) != 0, "F4 a window that is never adopted keeps its events");
        }
        {   // F5: a caster 1600 u out of a 900 u spell, on flat floors of three cell sizes
            for (const float cell : { 128.0f, 77.0f, 51.0f }) {
                const auto n = static_cast<std::size_t>(3600.0f / cell);
                std::vector<std::string> map(n, std::string(n, '.'));
                const auto m = Build(map, {}, cell);
                PartyPicture p; p.party = { Who(0x11, { 1000.0f, 1800.0f, 0.0f }, { Act(Family::CastOffense, 0x91, 0.8f, 900.0f, Sight::Visible) }) };
                p.foes = { Bad(0x91, { 2600.0f, 1800.0f, 0.0f }, 0) };
                const auto r = Window(m, p, t); const auto* a = Of(r, 0x11);
                char b[160]; std::snprintf(b, sizeof b, "F5 cells of %.0f u: he finds a place in reach (%u spots), floods cut: %u", static_cast<double>(cell), p.counts.generated, p.counts.floodsShort);
                Row(a->spot >= 0 && Dist(a->p, p.foes[0].p) <= 900.0f && (cell > 100.0f ? p.counts.floodsShort == 0 : cell < 60.0f ? p.counts.floodsShort > 0 : true), b);
            }
        }
        {   // F6: the mesh is swapped for a smaller one between two slices
            const auto big = Build(Hall()); auto small = Build(kRoom); small.stamp = 2; int safe = 0, tried = 0; bool counted = true;
            for (int at = 1; at < 200; at += 7) {
                PartyPicture p = Crowd(6); Env::Scratch s; std::vector<RayAsk> asks; std::vector<Assignment> as;
                MP::Freeze(p, big.Get(), t); bool done = false;
                for (int k = 0; k < at && !done; ++k) done = MP::Phase1(p, big.Get(), s, &NoClass, t, asks, 0.0f);
                if (done) break;
                ++tried; done = MP::Phase1(p, small.Get(), s, &NoClass, t, asks, 1.0e9f);
                MP::Phase3(p, &NoClass, t, as); MP::Adopt(p, as);
                bool stay = done && asks.empty() && as.size() == 6; for (const auto& a : as) stay = stay && a.spot < 0;
                counted = counted && p.counts.viewChanged == 1; if (stay) ++safe;
                Window(small, p, t);   // the next window starts clean on the new mesh
                counted = counted && p.envStamp == 2 && p.counts.viewChanged == 0;
            }
            char b[120]; std::snprintf(b, sizeof b, "F6 a view swapped mid-window: the work is dropped, everyone stays, counted (%d of %d)", safe, tried);
            Row(tried > 10 && safe == tried && counted, b);
        }
        {   // F7: answers that come after their window
            const auto m = Build(kRoom); const auto v = m.Get();
            const auto open = [&](PartyPicture& p, std::vector<RayAsk>& asks) { Env::Scratch s; MP::Freeze(p, v, t); while (!MP::Phase1(p, v, s, &NoClass, t, asks, 1.0e9f)) {} };
            const auto close = [&](PartyPicture& p) { std::vector<Assignment> as; MP::Phase3(p, &NoClass, t, as); MP::Adopt(p, as); p.t += 0.5; };
            PartyPicture p = RoomFight(); std::vector<RayAsk> first, next;
            open(p, first); const auto late = Engine(m, p, first, t); close(p);
            open(p, next); MP::Answer(p, late, t);
            Row(!late.empty() && p.facts.size() == late.size(), "F7 answers one window late all land (nobody moved)");
            close(p);
            PartyPicture q = RoomFight(); open(q, first); auto said = Engine(m, q, first, t); close(q);
            for (auto& f : q.foes) f.p.x += 200.0f;
            open(q, next); MP::Answer(q, said, t);
            int foe = 0; for (const auto& [k, f] : q.facts) if (k.second >= 0x91 && k.second <= 0x92) ++foe;
            Row(foe == 0, "F7 a late answer about where a runner WAS is not used as fresh");
            PartyPicture z = RoomFight(); open(z, first); close(z); for (auto& f : z.foes) f.p.x += 200.0f;
            open(z, next); said.clear();
            for (const auto& a : first) for (const auto& f : z.foes) if (f.id == a.viewer) said.push_back({ a.spot, a.viewer, Tri3::Yes, z.t + 0.1, f.p });
            MP::Answer(z, said, t); foe = 0; for (const auto& [k, f] : z.facts) if (k.second >= 0x91 && f.t > 0.5) ++foe;
            const auto had = foe; close(z); open(z, next); foe = 0; for (const auto& [k, f] : z.facts) if (k.second >= 0x91) ++foe;
            Row(had > 0 && had == static_cast<int>(said.size()) && foe == had, "F7 a late answer that says when and where it was cast is as good as a prompt one");
            // a road two windows slow never starves
            PartyPicture w = RoomFight(); std::vector<std::vector<RayAsk>> q2; std::size_t most = 0;
            for (int k = 0; k < 8; ++k) { std::vector<RayAsk> a; open(w, a); q2.push_back(a); if (k >= 2) MP::Answer(w, Engine(m, w, q2[static_cast<std::size_t>(k) - 2], t), t); most = (std::max)(most, w.facts.size()); close(w); }
            Row(most >= 8 && w.late.size() <= static_cast<std::size_t>(kMoveBudget.lateAsks), "F7 a road two windows slow is not starved");
            MP::Answer(p, std::vector<RayAnswer>{ { 12345, 0x91, Tri3::Yes, -1.0, {} } }, t);
            Row(p.facts.count({ 12345, 0x91 }) == 0, "F7 an answer nobody asked for is ignored");
        }
    }

    // Two rooms, a gap in the middle (column 6, row 2) and a way round at the bottom (row 6).
    const std::vector<std::string> kTwoWays{ "#############", "#.....#.....#", "#...........#", "#.....#.....#", "#.....#.....#", "#.....#.....#", "#...........#", "#############" };
    PartyPicture TwoWays(const MD::Mesh& m) {
        PartyPicture p; p.party = { Who(0x61, At(2, 2), { Act(Family::CastOffense, 0x91, 0.8f, 700.0f, Sight::Occluded) }), Who(0x62, At(2, 2.4f), { Act(Family::CastOffense, 0x91, 0.7f, 700.0f, Sight::Occluded) }) };
        p.foes = { Bad(0x91, At(10, 4), 0) };
        p.envStamp = m.stamp; WallVerts(m.Get(), p.wallVert);   // a window that is not the mesh's first: a mark in the table stands
        return p;
    }
    void TrustRows() {
        std::printf("\n== movement: trust (S3) ==\n");
        namespace TR = Trust; using TR::Sign; using TR::Level;
        const Tuning& t = ShippedTuning(); const TR::Rules& R = TR::kTrustRules;
        const Env::Vec3 at{ 1000.0f, 800.0f, 0.0f }, ahead{ 1400.0f, 800.0f, 0.0f };
        const auto here = TR::RegionOf({ at.x + R.aheadU, at.y, at.z });
        {   // the thresholds
            TR::Table a; const bool n1 = TR::Note(a, Sign::NoGround, 1, at, ahead, 0.0); const auto l1 = TR::Of(a, here);
            const bool n2 = TR::Note(a, Sign::NoGround, 1, at, ahead, 0.9); const auto l2 = TR::Of(a, here);
            const bool n3 = TR::Note(a, Sign::NoGround, 1, at, ahead, 2.0); const auto l3 = TR::Of(a, here);
            Row(n1 && l1 == Level::Doubted, "one lived sign doubts and never distrusts");
            Row(!n2 && l2 == Level::Doubted, "the same event again (inside repeatSec) is not a repeat");
            Row(n3 && l3 == Level::Distrusted, "one sign repeated distrusts");
            TR::Table b; TR::Note(b, Sign::NoGround, 1, at, ahead, 0.0); TR::Note(b, Sign::EngineRefused, 1, at, ahead, 2.0);
            Row(TR::Of(b, here) == Level::Distrusted, "two different signs distrust");
            TR::Table c; TR::Note(c, Sign::NoGround, 1, at, ahead, 0.0); TR::Note(c, Sign::NoGround, 2, at, ahead, 2.0);
            Row(TR::Of(c, here) == Level::Distrusted, "the same sign lived by two bodies is a repeat");
            // S3 review R4: one event is one sign
            TR::Table o; TR::Note(o, Sign::NoGround, 1, at, ahead, 0.0); TR::Note(o, Sign::NoGround, 2, at, ahead, 0.2); TR::Note(o, Sign::EngineRefused, 1, at, ahead, 0.1);
            const auto oneMoment = TR::Of(o, here); TR::Note(o, Sign::NoGround, 2, at, ahead, 3.0);
            Row(o.marks.size() == 4 && oneMoment == Level::Doubted && TR::Of(o, here) == Level::Distrusted, "two bodies stuck and a refusal at one moment are one event, a later one is the second");
            TR::Table g; TR::Note(g, Sign::LengthFar, 1, at, ahead, 0.0, 0, 0, 5); TR::Note(g, Sign::NoGround, 1, at, ahead, 8.0, 0, 0, 5); const auto oneLeg = TR::Of(g, here);
            TR::Note(g, Sign::NoGround, 1, at, ahead, 12.0, 0, 0, 6);
            Row(g.marks.size() == 3 && oneLeg == Level::Doubted && TR::Of(g, here) == Level::Distrusted, "one leg that was far and then stuck is one event, the next leg's is the second");
            TR::Table st; TR::Note(st, Sign::DanglingLink, 1, at, ahead, 0.0); TR::Note(st, Sign::OneWayLinks, 1, at, ahead, 0.0);
            Row(TR::Of(st, here) == Level::Distrusted, "... static signs are not lived: two kinds read at one moment are two");
            bool lived = true, stat = true;
            for (const auto& row : TR::kTrustSigns) { TR::Table d; TR::Note(d, row.sign, 1, at, ahead, 0.0); (row.lived ? lived : stat) &= TR::Of(d, here) == row.alone && (!row.lived || row.alone == Level::Doubted) && row.decaySec > 0.0f && TR::Seen(row.sign, row.threshold) && !TR::Seen(row.sign, row.threshold - 0.01f); }
            Row(lived && stat, "every row of kTrustSigns: alone it argues its own level, no lived row distrusts alone, every row decays, its threshold is >=");
        }
        {   // what takes a mark away
            TR::Table a; TR::Note(a, Sign::NoGround, 1, at, ahead, 0.0); TR::Note(a, Sign::NoGround, 1, at, ahead, 5.0);
            TR::Table b = a; TR::Decay(b, 29.0); const auto d1 = TR::Of(b, here); TR::Decay(b, 31.0); const auto d2 = TR::Of(b, here); TR::Decay(b, 36.0);
            Row(d1 == Level::Distrusted && d2 == Level::Doubted && b.Clean(), "marks decay one by one (30 s): distrusted, doubted, trusted");
            const auto walk = [&](Env::Vec3 p, Env::Vec3 q) { TR::Table w = a; TR::Walked(w, true, p, q, 6.0); return w.Clean(); };
            Row(walk({ 1000, 800, 0 }, { 1080, 800, 0 }), "a walk across the place, the way it was going, clears it");
            {   // S3 review R2 and R3
                const auto slide = [&](float deg, float u) { TR::Table w = a; const float c = std::cos(deg * 0.0174533f), n = std::sin(deg * 0.0174533f); for (float s = 0.0f; s < 160.0f; s += u) TR::Walked(w, false, { 1000.0f + c * s, 800.0f + n * s, 0 }, { 1000.0f + c * (s + u), 800.0f + n * (s + u), 0 }, 6.0); return w.marks.size(); };
                Row(slide(45, 30) == 2 && slide(45, 7) == 2 && slide(-45, 30) == 2 && slide(60, 30) == 2 && slide(30, 30) == 2, "sliding along a wall at 30, 45 or 60 degrees to the way he was going clears nothing (R2)");
                TR::Table w = a; for (const auto& [p, q] : { std::pair{ Env::Vec3{ 1000, 800, 0 }, Env::Vec3{ 1000, 850, 0 } }, std::pair{ Env::Vec3{ 1000, 850, 0 }, Env::Vec3{ 1100, 850, 0 } }, std::pair{ Env::Vec3{ 1100, 850, 0 }, Env::Vec3{ 1100, 800, 0 } } }) TR::Walked(w, false, p, q, 6.0);
                Row(w.marks.size() == 2, "... nor does walking round the end of a thin one (R2)");
                Row(!walk({ 1000, 800, 0 }, { 1050, 800, 0 }), "... nor a push a body's width on");
                TR::Table far = a; TR::Note(far, Sign::LengthFar, 1, { 1480, 800, 0 }, { 1900, 800, 0 }, 5.5); TR::Walked(far, false, { 1500, 800, 0 }, { 1560, 800, 0 }, 6.0);
                Row(far.marks.size() == 2 && TR::Of(far, here) == Level::Distrusted, "a walk across one mark leaves the region's others (R3)");
            }
            Row(!walk({ 1080, 800, 0 }, { 1000, 800, 0 }), "... the other way does not (a one-way edge)");
            Row(!walk({ 1080, 800, 0 }, { 1140, 800, 0 }) && !walk({ 1000, 900, 0 }, { 1080, 900, 0 }), "... nor a walk beside it or on the far side of it");
            Row(!walk({ 1000, 800, 300 }, { 1080, 800, 300 }), "... nor one on the floor above");
            TR::Table f = a; TR::Walked(f, false, { 1000, 800, 0 }, { 1080, 800, 0 }, 6.0);
            Row(f.Clean() && f.trail.empty(), "a follower's crossing clears too, and leaves no trail");
            TR::Table g = a; TR::Walked(g, true, { 1200, 700, 0 }, { 1210, 700, 0 }, 6.0);
            Row(TR::Of(g, Env::Vec3{ 1250, 720, 0 }) == Level::Trusted && TR::Of(g, Env::Vec3{ 1100, 950, 0 }) == Level::Distrusted, "a place the player stood on is proven, the rest of the region is not");
            TR::Decay(g, 80.0); Row(g.trail.empty(), "the trail gets old");
            TR::Table h = a; TR::Forget(h); Row(h.Clean() && h.gen != a.gen, "a stamp change forgets");
        }
        {   // a route
            TR::Table a; TR::Note(a, Sign::NoGround, 1, at, ahead, 0.0); TR::Note(a, Sign::NoGround, 1, at, ahead, 5.0);   // region 512..1536 is x 1024..1536
            const auto lvl = [&](Env::Vec3 from, std::vector<Env::Vec3> via) { return TR::RouteLevel(a, from, via); };
            Row(lvl({ 200, 800, 0 }, { { 328, 800, 0 }, { 800, 800, 0 }, { 1100, 800, 0 }, { 1800, 800, 0 } }) == Level::Distrusted, "a route into the region depends on it");
            Row(lvl({ 200, 800, 0 }, { { 328, 400, 0 }, { 800, 300, 0 }, { 1100, 300, 0 }, { 1800, 300, 0 } }) == Level::Trusted, "a route round it does not");
            Row(lvl({ 1100, 900, 0 }, { { 1100, 600, 0 }, { 1100, 400, 0 } }) == Level::Trusted, "leaving it is not depending on it");
            Row(lvl({ 1000, 800, 0 }, { { 1010, 800, 0 }, { 1030, 800, 0 } }) == Level::Distrusted, "a short route across the place itself is");
        }
        {   // the legs' ends
            TR::Table a; TR::LegEvent e; e.who = 7; e.spot = 42; e.end = TR::End::Stuck; e.at = at; e.toward = ahead; e.laneClear = false;
            Row(TR::Leg(a, e) == 0 && a.Clean(), "a stuck leg with a body or a door in the lane is no sign");
            e.laneClear = true; Row(TR::Leg(a, e) == 1 && !TR::Refused(a, 7, 42), "a stuck leg with a clear lane is one, and its spot may be tried again");
            TR::Table b; e.end = TR::End::Blocked; e.laneClear = false; TR::Leg(b, e);
            Row(b.Clean() && TR::Refused(b, 7, 42) && !TR::Refused(b, 8, 42) && !TR::Refused(b, 7, 43), "a blocked leg remembers its spot, for him alone, and is no sign");
            TR::Decay(b, 9.0); const bool still = TR::Refused(b, 7, 42); TR::Decay(b, 10.5); Row(still && !TR::Refused(b, 7, 42), "... for ten seconds");
            TR::Table c; e.end = TR::End::Refused; e.laneClear = true; Row(TR::Leg(c, e) == 1 && TR::Refused(c, 7, 42), "a route the engine refuses is a sign and remembers its spot");
            const std::vector<Env::Vec3> way{ { 600, 100, 0 }, { 1200, 100, 0 }, { 1800, 100, 0 } };
            e.end = TR::End::Arrived; e.route = way; e.passed = 1u; e.planned = 1000.0f;
            TR::Table d; e.walked = 1590.0f; const int under = TR::Leg(d, e); e.walked = 1610.0f; const int over = TR::Leg(d, e);
            Row(under == 0 && over == 2 && TR::Of(d, way[0]) == Level::Trusted && TR::Of(d, way[2]) == Level::Doubted, "1.6 times the planned length marks the parts of the plan he did not come by");
            TR::Table s; e.planned = 100.0f; e.walked = 300.0f; Row(TR::Leg(s, e) == 0, "... not on a short leg, whatever the ratio");
        }
        {   // the log
            TR::Table a; for (int i = 0; i < 6; ++i) TR::Note(a, Sign::NoGround, 1, at, ahead, 2.0 * i);
            TR::Decay(a, 100.0); for (int i = 0; i < 6; ++i) TR::Note(a, Sign::NoGround, 1, at, ahead, 101.0 + 2.0 * i);
            Row(a.log.size() == 2 && a.log[0].level == Level::Doubted && a.log[1].level == Level::Distrusted && TR::Text(a.log[1], "Cell").find("[env-trust] Cell region 2,1,0 distrusted: no-ground") == 0, "one line a region and level, also after it decayed and came back");
        }
        {   // the route search's price
            const auto m = Build(kTwoWays); Env::Scratch s; Env::Path p, q, z; const auto v = m.Get();
            const auto tri = [&](Env::Vec3 x) { return Env::TriAt(v, x); };
            std::vector<float> dear(v.tris.size(), 1.0f), plain(v.tris.size(), 1.0f);
            for (std::size_t i = 0; i < dear.size(); ++i) if (const auto c = Env::Centroid(v, static_cast<std::int32_t>(i)); c.x > 5.5f * kCell && c.x < 8.0f * kCell && c.y < 4.0f * kCell) dear[i] = 6.0f;
            Env::PathQuery k{ At(2, 2), At(10, 2), tri(At(2, 2)), tri(At(10, 2)), {} }; Env::FindPath(v, k, s, p);
            k.avoid = dear; Env::FindPath(v, k, s, q); k.avoid = plain; Env::FindPath(v, k, s, z);
            float low = 0.0f; for (const auto& c : q.pts) low = (std::max)(low, c.p.y);
            Row(p.reachable && q.reachable && q.length > p.length + 300.0f && low > 6.0f * kCell, "an avoided gap is walked round, and the length is the real walk's");
            const auto room = Build({ "############", "#..........#", "#..........#", "#..........#", "#..........#", "#..........#", "############" }); const auto w = room.Get();
            std::vector<float> mid(w.tris.size(), 1.0f); for (std::size_t i = 0; i < mid.size(); ++i) if (const auto c = Env::Centroid(w, static_cast<std::int32_t>(i)); c.x > 5.0f * kCell && c.x < 7.0f * kCell && c.y < 5.0f * kCell) mid[i] = 12.0f;
            Env::Path o; Env::PathQuery j{ At(2, 2), At(9, 2), Env::TriAt(w, At(2, 2)), Env::TriAt(w, At(9, 2)), {}, mid }; Env::FindPath(w, j, s, o);
            Row(o.reachable && o.length > Dist(At(2, 2), At(9, 2)) + 100.0f, "the straight line over an avoided patch is not put back");
            Row(z.length == p.length && z.pts.size() == p.pts.size(), "a price of 1 everywhere is the plain search");
        }
        {   // the pass
            const auto m = Build(kTwoWays);
            const auto run = [&](const std::function<void(PartyPicture&)>& f, Ran& r) { PartyPicture p = TwoWays(m); if (f) f(p); r = Window(m, p, t); return p; };
            const auto gapMark = [&](PartyPicture& p, int n) { for (int i = 0; i < n; ++i) TR::Note(p.trust, Sign::NoGround, 0x61, At(5.4f, 2), At(9, 2), -4.0 + 2.0 * i); };
            Ran r0, r1, r2, r3; run({}, r0);
            const auto p1 = run([&](PartyPicture& p) { gapMark(p, 1); }, r1);
            const auto p2 = run([&](PartyPicture& p) { gapMark(p, 2); }, r2);
            const auto through = [&](const PartyPicture& p, const Ran& r, FormID who) { for (const auto& a : r.as) if (a.who == who && a.spot >= 0) for (const auto& w : p.work.pairs) if (w.spot == a.spot && p.party[w.member].id == who) return TR::RouteLevel(p.trustNow, p.party[w.member].p, { w.path.via.data(), w.path.nVia }); return Level::Trusted; };
            Row(Of(r0, 0x61)->spot >= 0 && Of(r1, 0x61)->spot >= 0 && through(p1, r1, 0x61) <= Level::Doubted, "one mark in the gap: he still goes");
            Row(through(p2, r2, 0x61) != Level::Distrusted && through(p2, r2, 0x62) != Level::Distrusted, "two: no place is given whose way depends on the gap");
            // refused for one
            const auto key = Of(r0, 0x61)->key;
            const auto p3 = run([&](PartyPicture& p) { p.trust.refused.push_back({ 0x61, key, 0.0 }); }, r3);
            bool mine = false, his = false; for (const auto& o : p3.work.offers) if (p3.pool[static_cast<std::size_t>(o.spot)].key == key) (p3.party[o.member].id == 0x61 ? mine : his) = true;
            Row(Of(r0, 0x61)->spot >= 0 && Of(r3, 0x61)->key != key && !mine, "a refused spot is not offered to him again");
            Row(his, "... and still is to the other");
            PartyPicture one = TwoWays(m); one.party.resize(1); PartyPicture two = one; Window(m, one, t);
            for (const auto& o : one.work.offers) two.trust.refused.push_back({ 0x61, one.pool[static_cast<std::size_t>(o.spot)].key, 0.0 });
            Window(m, two, t); bool same = false; for (const auto& o : two.work.offers) for (const auto& x : one.work.offers) same = same || two.pool[static_cast<std::size_t>(o.spot)].key == one.pool[static_cast<std::size_t>(x.spot)].key;
            Row(!one.work.offers.empty() && !two.work.offers.empty() && !same, "... and the next best are looked for in its stead");
            {   // S3 review R1: many refused places. What is looked at next is the next best, never the unsorted rest
                const auto room = Build(std::vector<std::string>{ "##############", "#............#", "#............#", "#............#", "#............#", "#............#", "#............#", "#............#", "#............#", "#............#", "#............#", "##############" }); const auto v = room.Get();
                std::set<SpotKey> gone; float worst = 0.0f; bool order = true; int rounds = 0;
                for (; rounds < 60; ++rounds) {
                    PartyPicture p; p.party = { Who(0x71, At(2, 2), { Act(Family::Regroup, 0, 0.8f, 0.0f, Sight::None) }) }; p.party[0].anchor = At(11, 9); p.party[0].leash = 4000.0f; p.envStamp = v.stamp; WallVerts(v, p.wallVert);
                    for (const auto k : gone) p.trust.refused.push_back({ 0x71, k, 0.0 });
                    Window(room, p, t); if (p.pool.empty()) break;
                    Work::Flood fl; Flood(v, p.party[0].p, p.party[0].tri, p.party[0].who, t.spotRadius, kMoveBudget.gatherTris, fl);
                    float lo = kInf, hi = 0.0f; for (const auto& s : p.pool) { const float w = s.tri == p.party[0].tri ? Dist(p.party[0].p, s.p) : fl.At(s.tri) + Dist(Env::Centroid(v, s.tri), s.p); lo = (std::min)(lo, w); hi = (std::max)(hi, w); if (!gone.insert(s.key).second) order = false; }
                    if (lo < worst - 1.5f) order = false;
                    worst = (std::max)(worst, hi);
                }
                Row(rounds >= 40 && gone.size() > 100 && order, "refused places take no room: the next best are found in order, however many were refused (R1)");
            }
            PartyPicture late = TwoWays(m); gapMark(late, 2); late.t = 60.0; Window(m, late, t);
            Row(late.trust.Clean() && late.avoid.empty(), "the pass lets marks decay");
            PartyPicture k = TwoWays(m); Window(m, k, t); const bool was = k.party[0].commit.on; k.trust.refused.push_back({ 0x61, k.party[0].commit.key, k.t }); const auto old = k.party[0].commit.key; const auto rk = Window(m, k, t);
            Row(was && Of(rk, 0x61)->key != old, "a committed spot that was refused is given up");
            // an event inside a window
            PartyPicture a = TwoWays(m), b = TwoWays(m); Env::Scratch s; std::vector<RayAsk> qa, qb; std::vector<Assignment> xa, xb; const auto v = m.Get();
            MP::Freeze(a, v, t); MP::Freeze(b, v, t); gapMark(b, 2);
            while (!MP::Phase1(a, v, s, &NoClass, t, qa, 1.0e9f)) {} while (!MP::Phase1(b, v, s, &NoClass, t, qb, 1.0e9f)) {}
            MP::Phase3(a, &NoClass, t, xa); MP::Phase3(b, &NoClass, t, xb); Ran ra, rb; ra.as = xa; rb.as = xb;
            Row(SameAsks(qa, qb) && Same(xa, xb) && !b.trust.Clean(), "a mark made inside a window changes nothing in it");
            PartyPicture d = TwoWays(m); gapMark(d, 2); d.envStamp = 999; Window(m, d, t);
            Row(d.trust.Clean(), "a new stamp forgets the marks");
        }
    }

    // ── golden vectors ──
    struct Scene { std::string name; std::function<std::pair<MD::Mesh, PartyPicture>()> make; std::vector<std::pair<std::string, float>> tuning = {}; };
    std::vector<Scene> Scenes() {
        return {
            { "room", [] { return std::pair{ Build(kRoom), RoomFight() }; } },
            { "hall4", [] { return std::pair{ Build(Hall()), Crowd(4) }; } },
            { "hall10", [] { return std::pair{ Build(Hall()), Crowd(10) }; } },
            { "door_pin", [] { PartyPicture p; p.caps.pin = true;   // the tuning ships pin kinds off: the case proves they stay away
                for (int i = 0; i < 3; ++i) p.party.push_back(Who(0x51 + static_cast<FormID>(i), At(1.0f + static_cast<float>(i) * 0.6f, 2), { Act(Family::Attack, 0x91, 0.7f, 150.0f, Sight::Visible, true) }));
                p.foes = { Bad(0x91, At(7, 2), 0x52) }; return std::pair{ Build({ "#########", "#...#...#", "#.......#", "#...#...#", "#########" }), p }; } },
            { "pins_on", [] { PartyPicture p = RoomFight(); p.caps.pin = true; p.foes[0].faceX = 1.0f; p.foes[1].faceY = 1.0f;   // cover, flank, chokepoint
                p.foes[0].target = 0x13; p.party[2].top[1] = Act(Family::DrinkHealth, 0, 0.8f, 0.0f, Sight::None); p.party[2].nTop = 2;
                p.party[1].top[1] = Act(Family::PowerAttack, 0x92, 0.75f, 150.0f, Sight::Visible, true); p.party[1].nTop = 2; return std::pair{ Build(kRoom), p }; }, { { "t.spotPinKinds", 1.0f } } },
            { "door_pins_on", [] { PartyPicture p; p.caps.pin = true;
                for (int i = 0; i < 3; ++i) p.party.push_back(Who(0x51 + static_cast<FormID>(i), At(1.0f + static_cast<float>(i) * 0.6f, 2), { Act(Family::Attack, 0x91, 0.7f, 150.0f, Sight::Visible, true) }));
                p.foes = { Bad(0x91, At(7, 2), 0x52) }; return std::pair{ Build({ "#########", "#...#...#", "#.......#", "#...#...#", "#########" }), p }; }, { { "t.spotPinKinds", 1.0f } } },
            { "trust", [] { auto m = Build(kTwoWays); PartyPicture p = TwoWays(m);   // two marks in the gap, a doubted corner, a refusal, a trail
                for (int i = 0; i < 2; ++i) Trust::Note(p.trust, Trust::Sign::NoGround, 0x61, At(5.4f, 2), At(9, 2), -4.0 + 2.0 * i);
                Trust::Note(p.trust, Trust::Sign::LengthFar, 0x62, At(9, 6), At(10, 6), -1.0, 900.0f, 1700.0f); p.trust.refused.push_back({ 0x62, KeyOfPoint(20, 0), -1.0 });
                p.trust.trail.push_back({ At(3, 5), -1.0 }); return std::pair{ m, p }; } },
            { "class_room", [] { PartyPicture p = RoomFight(); for (auto& m : p.party) m.classId = 1; p.foes[0].target = 0x12; return std::pair{ Build(kRoom), p }; } },
        };
    }
    void Golden(const std::string& path, bool write) {
        std::printf("\n== movement: golden vectors (%s) ==\n", path.c_str());
        const auto tune = [](const std::vector<std::pair<std::string, float>>& set) { Tuning t = ShippedTuning(); for (const auto& [n, x] : set) if (!SetTuningScalar(t, n, x)) Row(false, "a golden case's tuning name"); return t; };
        if (write) {
            std::ofstream f(path, std::ios::binary);
            for (const auto& [name, make, set] : Scenes()) {
                const Tuning t = tune(set);
                auto [mesh, pic] = make();
                for (int w = 0; w < 3; ++w) {
                    pic.paths.clear();
                    for (auto& m : pic.party) if (w == 2) m.marks = kMarkHitHard;
                    // what was filed and never answered is the next Freeze's carried list: the dump states it
                    PartyPicture before = pic; MP::File(before);
                    const auto r = Window(mesh, pic, t, 1.0e9f, true, &Profiles);
                    MD::Write(f, name + "_" + std::to_string(w), mesh, before, r.answers, r.as, w > 0, set);
                }
            }
        }
        std::ifstream f(path, std::ios::binary); std::string err; int cases = 0;
        MD::Mesh before;
        while (auto c = MD::Read(f, err, &before)) {
            before = c->mesh;
            const Tuning t = tune(c->tuning);
            PartyPicture& p = c->pic; Env::Scratch s; std::vector<RayAsk> asks; std::vector<Assignment> as; const auto v = c->mesh.Get();
            MP::Freeze(p, v, t);
            while (!MP::Phase1(p, v, s, &Profiles, t, asks, 1.0f)) {}
            bool asked = asks.size() == c->answers.size();
            for (std::size_t i = 0; asked && i < asks.size(); ++i) asked = asks[i].spot == c->answers[i].spot && asks[i].viewer == c->answers[i].viewer;
            MP::Answer(p, c->answers, t); MP::Phase3(p, &Profiles, t, as);
            bool exact = as.size() == c->expect.size(); float worst = 0.0f;
            for (std::size_t i = 0; exact && i < as.size(); ++i) {
                const auto& e = c->expect[i];
                exact = as[i].who == e.who && as[i].key == e.key && static_cast<int>(as[i].action) == e.family && as[i].form == e.form && as[i].target == e.target;
                worst = (std::max)(worst, std::fabs(as[i].score - e.score));
            }
            char b[160]; std::snprintf(b, sizeof b, "%s: the same asks, the same assignments, scores within 1e-4 (%g)", c->name.c_str(), static_cast<double>(worst));
            Row(asked && exact && worst <= 1.0e-4f, b);
            ++cases;
        }
        Row(err.empty() && cases == 24, "every golden case was read");
    }
}

int MoveRows(const char* a_golden, bool a_write) {
    g_bad = 0;
    Determinism(); Tunables(); Scoring(); Behaviour(); Budgets(); Review(); TrustRows(); Golden(a_golden, a_write);
    return g_bad;
}

#ifdef MOVE_STANDALONE
int main(int argc, char** argv) {   // move_rows <golden file> [--write]
    const int bad = MoveRows(argc > 1 ? argv[1] : "golden_move.txt", argc > 2 && std::string(argv[2]) == "--write");
    std::printf("\n%d movement check(s) failed\n", bad);
    return bad;
}
#endif
