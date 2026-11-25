// tools/iaus_harness/move_rows.cpp -- the movement pass's rows (S1, movement-plan.md "Acceptance" of S1) and its golden vectors.
// Linked into the harness by run.sh. MOVE_STANDALONE builds it alone, which is how another compiler replays the same golden file (the line in run.sh).
#include "iaus/MoveDump.h"
#include "iaus/MoveOrder.h"

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
            p.hold = { 1.4f, 0.6f, 0.0f, 1.7f, 0.8f, 1.2f, 2.0f, 1.0f, 0.5f, 1.3f };
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
        MP::Adopt(pic, r.as); MP::Orders(pic, r.as, t);
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
        {   // MOVE-ONLY (Generated::moveOnly): it does not work from here. Staying is worth nothing for it, and only a heal-reach place pairs with it
            const auto at = [&](bool only, bool other, float score) {
                PartyPicture p; p.party = { Who(0x13, At(1, 8), { Act(Family::CastHeal, 0x14, score, 900.0f, Sight::Occluded, false, 0x200) }), Who(0x14, At(8, 2), {}) }; p.party[1].decides = false;
                if (other) { p.party[0].top[1] = Act(Family::CastOffense, 0x91, 0.5f, 3000.0f, Sight::Visible); p.party[0].nTop = 2; p.foes = { Bad(0x91, At(1, 6), 0x14) }; }
                p.party[0].top[0].moveOnly = only; return p; };
            PartyPicture a = at(true, false, 1.2f); const auto r = Window(mesh, a, t); const auto* h = Of(r, 0x13);
            Row(h->spot >= 0 && a.pool[static_cast<std::size_t>(h->spot)].kinds == KindBit(SpotKind::HealReach) && GeomSees(v, At(8, 2), h->p, t, s) && Dist(h->p, At(8, 2)) <= 900.0f && h->act.moveOnly, "a move-only heal: he walks to where it works");
            PartyPicture b = at(true, true, 9.0f); b.party[0].kinds &= ~KindBit(SpotKind::HealReach); const auto r2 = Window(mesh, b, t);
            Row(Of(r2, 0x13)->act.family == Family::CastOffense, "... with no such place it holds him nowhere, however much it is wanted: he plans for what he can do");
            PartyPicture c = at(false, true, 9.0f); c.party[0].kinds &= ~KindBit(SpotKind::HealReach); const auto r3 = Window(mesh, c, t);
            Row(Of(r3, 0x13)->act.family == Family::CastHeal, "... an action that works from here still may keep him");
            RankedAction m = Act(Family::CastHeal, 0x14, 1.0f, 900.0f, Sight::Occluded); const bool cover = Pairs(SpotKind::Cover, m); m.moveOnly = true;
            Row(cover && !Pairs(SpotKind::Cover, m) && Pairs(SpotKind::HealReach, m), "... and no cover place is paired with it");
            PartyPicture d = at(true, false, std::numeric_limits<float>::quiet_NaN()); Row(Of(Window(mesh, d, t), 0x13)->spot < 0, "... a NaN score moves nobody");
        }
        {   // ... round a corner of a U, with a reach over everything: every near place is in the band and none sees
            const auto u = Build({ "################", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#..............#", "#..............#", "#..............#", "################" });
            PartyPicture p; p.party = { Who(0x13, At(4, 5), { Act(Family::CastHeal, 0x14, 1.2f, 4096.0f, Sight::Occluded, false, 0x200) }), Who(0x14, At(13, 2), {}) }; p.party[1].decides = false; p.party[0].top[0].moveOnly = true;
            const auto r = Window(u, p, t); const auto uv = u.Get(); Row(Of(r, 0x13)->spot >= 0 && GeomSees(uv, At(13, 2), Of(r, 0x13)->p, t, s), "... found although every near place fails the sight test");
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
            TR::Table ch; for (int i = 0; i < 6; ++i) TR::Note(ch, Sign::NoGround, static_cast<FormID>(1 + i % 2), at, ahead, 1.2 * i);
            Row(ch.marks.size() == 6 && TR::Of(ch, here) == Level::Distrusted, "two bodies that take turns under an event's length are more than one event (a mark is measured from the event's first)");
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

    void LightRows() {
        std::printf("\n== movement: light (S4) ==\n");
        namespace L = Light; const Tuning& t = ShippedTuning(); const auto m = Build(kRoom);
        {   // the field
            L::Field f; L::Add(f, { { 100, 100, 0 }, 80.0f, 1.0 }); L::Add(f, { { 110, 100, 0 }, 10.0f, 2.0 }); L::Add(f, { { 110, 90, 0 }, 99.0f, 1.5 });
            Row(f.samples.size() == 1 && f.samples[0].level == 10.0f, "one sample a cell, the freshest");
            Row(L::At(f, { 110, 100, 0 }, 2.0, t).level == 10.0f && L::At(f, { 110, 100, 0 }, 2.0, t).conf > 0.9f, "a place beside a sample has its level");
            Row(!L::At(f, { 110 + t.lightNearU, 100, 0 }, 2.0, t).Known() && !L::At(f, { 110, 100, t.lightStoreyU + 1 }, 2.0, t).Known(), "far off, or on another floor: unknown");
            Row(!L::At(f, { 110, 100, 0 }, 2.0 + t.lightMaxAgeSec, t).Known() && !L::At(f, { 110 + 0.9f * t.lightNearU, 100, 0 }, 2.0, t).Known(), "too old, or worth too little: unknown, never a level");
            L::Add(f, { { 300, 100, 0 }, 70.0f, 2.0 }); Row(L::At(f, { 280, 100, 0 }, 2.0, t).level == 70.0f && L::At(f, { 130, 100, 0 }, 2.0, t).level == 10.0f, "the nearest one answers");
            L::Add(f, { { 1, 1, 1 }, std::numeric_limits<float>::quiet_NaN(), 2.0 }); L::Add(f, { { std::numeric_limits<float>::infinity(), 1, 1 }, 1.0f, 2.0 }); L::Add(f, { { 5000, 1, 1 }, -3.0f, 2.0 }); Row(f.samples.size() == 2, "a reader's bad number is no sample");
            for (int i = 0; i < 600; ++i) L::Add(f, { { 1000.0f + 200.0f * static_cast<float>(i), 0, 0 }, 5.0f, 3.0 + i });
            Row(f.samples.size() == L::kFieldSamples && f.samples.back().t == 602.0, "the memory is bounded, the oldest goes");
            L::Decay(f, 602.0, t); Row(!f.samples.empty() && std::all_of(f.samples.begin(), f.samples.end(), [&](const auto& s) { return 602.0 - s.t < static_cast<double>(t.lightMaxAgeSec); }), "old samples are dropped");
        }
        {   // the engine's formula: the light term and the rest of it
            L::DetectGmst g; L::DetectIn in; in.dist = 300; in.targetIsOsTarget = true; in.sneakingT = true; in.skillO = 30; in.skillT = 30; in.light = 0.0f; const auto dark = L::DetectLevel(g, in); in.light = 100.0f; const auto lit = L::DetectLevel(g, in);
            const float d = 0.88f * 0.88f;
            Row(std::fabs((lit.score - dark.score) - d * (L::DetectLightTerm(100.0f, false, g) - L::DetectLightTerm(0.0f, false, g))) < 1.0e-3f && lit.level > dark.level, "the score's light part is the light term times the distance factor");
            Row(L::DetectLightTerm(1000.0f, false, g) == L::DetectLightTerm(g.lightMax, false, g) && L::DetectLightTerm(100.0f, true, g) > L::DetectLightTerm(100.0f, false, g), "... capped, and the exterior has its own factor");
            in.losGeom = false; Row(L::DetectLevel(g, in).light == 0.0f, "no line of sight, no light part"); in.losGeom = true; in.invisible = true; Row(L::DetectLevel(g, in).light == 0.0f, "... nor for the invisible");
        }
        // the pass
        const auto fight = [&](bool dark, float left, float right) {   // left half / right half of the room at these levels (< 0 = not known)
            PartyPicture p; p.caps.pin = true; p.party = { Who(0x81, At(5, 6), { Act(Family::Disengage, 0, 0.8f, 0.0f, Sight::None) }) }; p.party[0].wantsDark = dark; p.party[0].sneakSkill = 40.0f; p.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Cover);
            p.foes = { Bad(0x91, At(5, 3), 0x81) }; p.foes[0].faceY = 1.0f; p.foes[0].skill = 30.0f;
            for (int y = 1; y < 8; ++y) for (int x = 1; x < 11; ++x) if (const float l = x < 6 ? left : right; l >= 0.0f) Light::Add(p.lightField, { At(static_cast<float>(x), static_cast<float>(y)), l, 0.0 });
            return p; };
        Tuning pin = t; pin.spotPinKinds = 1.0f;
        const auto run = [&](PartyPicture p) { const auto r = Window(m, p, pin); return std::pair{ p, r }; };
        {
            const auto [pd, rd] = run(fight(true, 5.0f, 90.0f)); const auto [pl, rl] = run(fight(true, 90.0f, 5.0f)); const auto [pn, rn] = run(fight(false, 90.0f, 5.0f)); const auto [pu, ru] = run(fight(true, -1.0f, -1.0f)); const auto [p0, r0] = run(fight(false, -1.0f, -1.0f));
            const auto x = [](const Ran& r) { return Of(r, 0x81)->p.x / kCell; };
            Row(Of(rd, 0x81)->spot >= 0 && Of(rl, 0x81)->spot >= 0 && x(rd) < 6.0f && x(rl) > 6.0f, "who wants the dark takes cover on the dark side, whichever it is");
            Row(Same(rn.as, r0.as), "a member who does not want the dark reads no light at all");
            Row(Of(ru, 0x81)->key == Of(r0, 0x81)->key && Of(ru, 0x81)->score == Of(r0, 0x81)->score, "unknown light everywhere: the same place and the same score as without the layer");
            PartyPicture k = fight(true, 90.0f, 90.0f); Env::Scratch s; std::vector<RayAsk> q; std::vector<Assignment> as; MP::Freeze(k, m.Get(), pin); while (!MP::Phase1(k, m.Get(), s, &NoClass, pin, q, 1.0e9f)) {} PartyPicture j = k;
            for (const auto& sp : k.pool) k.light.push_back({ sp.key, 1.0f });
            MP::Phase3(k, &NoClass, pin, as); const float with = as[0].score; MP::Phase3(j, &NoClass, pin, as); Row(with > as[0].score, "a sampler's answer at the spot wins over the field");
        }
        {   // spot_detected (S4 review F8): stays only, the sight of the place not answered
            const auto stay = [&](float foeSkill, float sneak, float level) { PartyPicture p = fight(true, level, level); p.party[0].kinds = KindBit(SpotKind::Stay); p.party[0].sneakSkill = sneak; p.foes[0].skill = foeSkill; const auto r = Window(m, p, pin, 1.0e9f, false); return Of(r, 0x81)->score; };
            Row(stay(100.0f, 5.0f, 90.0f) < stay(0.0f, 100.0f, 90.0f) && stay(0.0f, 100.0f, 90.0f) > 0.0f, "a place somebody may see and would detect him at scores lower, though its sight is not answered");
            Row(stay(100.0f, 5.0f, 90.0f) < stay(100.0f, 5.0f, 5.0f), "... and in the light lower than in the dark");
            Row(stay(-1.0f, 5.0f, 90.0f) == stay(-1.0f, 100.0f, 90.0f) && stay(100.0f, -1.0f, 90.0f) == stay(0.0f, -1.0f, 90.0f), "... a foe or a sneak skill nobody read is not applicable");
            Row(stay(100.0f, 5.0f, -1.0f) == stay(0.0f, 100.0f, -1.0f), "... nor is it where the light is unknown");
        }
        {   // the facts through the score
            const Profile p; SpotFacts f; f.pathSec = 1.0f; const float u = ScoreSpot(f, Family::Step, p, t);
            SpotFacts a = f, b = f, c = f; a.detected = 0.0f; b.detected = 1.0f; c.pathLitSec = 2.0f;
            Row(ScoreSpot(a, Family::Step, p, t) > ScoreSpot(b, Family::Step, p, t) && ScoreSpot(c, Family::Step, p, t) < u, "a place he is detected at, and a walk over lit floor, score lower");
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
            { "light", [] { PartyPicture p = RoomFight(); p.caps.pin = true; p.t = 1.0; p.exterior = true;   // S4: a hunted thief in a half lit room, a sampler's answer, another member who reads none of it
                p.party[1].top[0] = Act(Family::Disengage, 0, 0.8f, 0.0f, Sight::None); p.party[1].nTop = 1; p.party[1].wantsDark = true; p.party[1].sneakSkill = 45.0f; p.foes[0].target = 0x12; p.foes[0].faceY = 1.0f; p.foes[0].skill = 35.0f; p.foes[1].skill = 20.0f; p.foes[1].faceX = 0.6f; p.foes[1].faceY = 0.8f;
                for (int y = 1; y < 8; ++y) for (int x = 1; x < 9; ++x) Light::Add(p.lightField, { At(static_cast<float>(x), static_cast<float>(y)), 7.0f + 9.0f * static_cast<float>(x), 0.1 * y });
                p.light.push_back({ KeyOfPoint(3, 0), 44.0f }); return std::pair{ Build(kRoom), p }; }, { { "t.spotPinKinds", 1.0f } } },
            { "move_only", [] { PartyPicture p; p.party = { Who(0x13, At(4, 5), { Act(Family::CastHeal, 0x14, 1.2f, 4096.0f, Sight::Occluded, false, 0x200), Act(Family::CastOffense, 0x91, 0.6f, 3000.0f, Sight::Visible) }), Who(0x14, At(13, 2), {}) };   // a heal that only works from elsewhere
                p.party[0].top[0].moveOnly = true; p.party[1].decides = false; p.foes = { Bad(0x91, At(2, 2), 0x14) };
                return std::pair{ Build({ "################", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#....######....#", "#..............#", "#..............#", "#..............#", "################" }), p }; } },
            { "strict", [] { PartyPicture p = RoomFight(); p.caps.strict = p.caps.kite = p.caps.disengage = true; p.foes[0].target = 0x12; p.foes[0].p = At(5, 6);   // orders of several modes over three windows
                p.party[1].top[1] = Act(Family::GiveGround, 0, 0.75f, 0.0f, Sight::None); p.party[1].nTop = 2; p.party[2].top[1] = Act(Family::DrinkHealth, 0, 0.8f, 0.0f, Sight::None); p.party[2].nTop = 2; return std::pair{ Build(kRoom), p }; } },
            { "strict_blades", [] { PartyPicture p; p.caps.strict = p.caps.kite = true; p.party = { Who(0x51, At(2, 2), { Act(Family::Attack, 0x91, 0.7f, 200.0f, Sight::None, true) }), Who(0x52, At(1, 1), { Act(Family::Attack, 0x92, 0.7f, 200.0f, Sight::None, true) }) };
                p.foes = { Bad(0x91, At(3, 2), 0x51), Bad(0x92, At(2, 3), 0x51), Bad(0x93, At(7, 2), 0x52) }; for (auto& f : p.foes) f.reach = 160.0f; return std::pair{ Build({ "#########", "#...#...#", "#.......#", "#...#...#", "#########" }), p }; } },
            { "strict_hold", [] { PartyPicture p; p.caps.strict = p.caps.kite = true; p.exterior = false;   // S5-A: holds, with every input of theirs read: a blade in contact, a bow behind an ally, a mage with a shooter on him, a walking player
                p.party = { Who(0x51, At(2, 2), { Act(Family::Attack, 0x91, 0.7f, 200.0f, Sight::None, true) }), Who(0x52, At(1, 3), { Act(Family::Attack, 0x92, 0.7f, 1800.0f, Sight::Visible), Act(Family::CastHeal, 0x51, 0.5f, 900.0f, Sight::Visible, false, 0x200) }), Who(0x53, At(1, 1), { Act(Family::CastOffense, 0x93, 0.8f, 2000.0f, Sight::Visible) }) };
                for (auto& m : p.party) { m.leash = 900.0f; m.anchor = At(1, 2); m.anchorVel = { -60.0f, 10.0f, 0.0f }; m.anchorMoves = true; }
                p.foes = { Bad(0x91, At(3, 2), 0x51), Bad(0x92, At(2, 3), 0x51), Bad(0x93, At(7, 2), 0x53) }; const float sp[] = { 370.0f, 140.0f, 300.0f }; for (std::size_t i = 0; i < 3; ++i) { p.foes[i].reach = 160.0f; p.foes[i].speed = sp[i]; p.foes[i].ranged = i == 2 ? MFO::Iaus::Tri::Yes : MFO::Iaus::Tri::No; }
                return std::pair{ Build({ "#########", "#...#...#", "#.......#", "#...#...#", "#########" }), p }; } },
            { "strict_class", [] { PartyPicture p = RoomFight(); p.caps.strict = true; for (auto& m : p.party) { m.classId = 1; m.top[0].sight = Sight::Visible; m.top[0].reach = (std::max)(m.top[0].reach, 1200.0f); } p.foes[0].target = 0x12; for (auto& f : p.foes) { f.reach = 150.0f; f.speed = 300.0f; f.ranged = MFO::Iaus::Tri::No; } return std::pair{ Build(kRoom), p }; } },
            { "class_room", [] { PartyPicture p = RoomFight(); for (auto& m : p.party) m.classId = 1; p.foes[0].target = 0x12; return std::pair{ Build(kRoom), p }; } },
        };
    }
    void OrderRows() {
        std::printf("\n== movement: orders (MoveOrder.h) ==\n");
        const auto m = Build(kRoom); const Tuning& t = ShippedTuning();
        const auto run = [&](PartyPicture p) { p.caps.strict = p.caps.kite = p.caps.disengage = true; Ran r = Window(m, p, t); return std::pair{ p, r }; };
        const auto mem = [](PartyPicture& p, FormID id) -> Member& { for (Member& x : p.party) if (x.id == id) return x; std::abort(); };
        {   // without the cap nobody is ordered anything, and the pass is what it was
            PartyPicture a = RoomFight(), b = RoomFight(); b.caps.strict = b.caps.kite = b.caps.disengage = true;
            (void)Window(m, a, t); bool none = true; for (const Member& x : a.party) none = none && x.order.mode == MoveMode::Free && x.order.gen == 0; Row(none, "no strict cap: every order is Free");
            const auto [p, r] = run(RoomFight()); bool agree = true, some = false; for (const Assignment& x : r.as) { const Member& w = *std::find_if(p.party.begin(), p.party.end(), [&](const Member& q) { return q.id == x.who; });
                if (x.spot >= 0) { some = true; agree = agree && w.order.mode == OrderRowOf(x.kinds).mode && w.order.p.x == x.p.x && w.order.p.y == x.p.y && w.order.tri == x.tri && w.order.leashR > 0.0f && w.order.arriveR <= w.order.leashR && w.order.gen == 1; } }
            Row(some && agree, "strict: a place becomes its kind's order, at its point, arrival inside the leash"); }
        {   // the caps gate the kinds
            RoadCaps c; const auto has = [&](SpotKind k, const RoadCaps& q) { return (AllowedKinds(q, t) & KindBit(k)) != 0; };
            Row(has(SpotKind::Kite, c) && has(SpotKind::RetreatExit, c) && !has(SpotKind::Cover, c), "the point road: as today");
            c.strict = true; Row(!has(SpotKind::Kite, c) && !has(SpotKind::RetreatExit, c) && has(SpotKind::Cover, c) && has(SpotKind::Chokepoint, c) && has(SpotKind::Flank, c) && has(SpotKind::Firing, c), "strict alone: no kite, no disengage, cover, chokepoint and flank on");
            c.kite = true; Row(has(SpotKind::Kite, c) && !has(SpotKind::RetreatExit, c), "... the kite cap adds the kite alone"); c.kite = false; c.disengage = true; Row(!has(SpotKind::Kite, c) && has(SpotKind::RetreatExit, c), "... the disengage cap the retreat alone"); }
        {   // several kinds: the table's order
            Row(OrderRowOf(KindBit(SpotKind::Firing) | KindBit(SpotKind::Kite)).mode == MoveMode::Kite && OrderRowOf(KindBit(SpotKind::Kite) | KindBit(SpotKind::RetreatExit)).mode == MoveMode::Disengage && OrderRowOf(KindBit(SpotKind::Firing) | KindBit(SpotKind::Cover)).pin && OrderRowOf(0).mode == MoveMode::Free && OrderRowOf(KindBit(SpotKind::Hold)).mode == MoveMode::Stand && OrderRowOf(KindBit(SpotKind::Stay)).mode == MoveMode::Free, "a place of several kinds: the first row of kOrderRows");
            Row(static_cast<int>(MoveMode::Stand) == 1 && static_cast<int>(MoveMode::GoTo) == 2 && static_cast<int>(MoveMode::Kite) == 3 && static_cast<int>(MoveMode::Disengage) == 4 && static_cast<int>(MoveMode::Path) == 5, "the modes are Harbinger's numbers (F10: literals until MFO's APMF_API.h reaches v48, then a static_assert)"); }
        {   // HOLD (S5-A): the gate
            PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(5, 6), {}) }; p.foes = { Bad(0x91, At(6, 6), 0x11) }; Member& w = p.party[0]; const auto on = [&](const RankedAction& a) { return HoldOffered(p, w, a, t); };
            const auto blade = Act(Family::Attack, 0x91, 0.8f, 150.0f, Sight::None, true), bow = Act(Family::Attack, 0x91, 0.8f, 1800.0f, Sight::Visible);
            Row(on(blade) && on(bow), "hold: offered for an action that works from here"); auto x = blade; x.reach = 100.0f; Row(!on(x), "... not out of reach"); x = bow; x.moveOnly = true; Row(!on(x), "... not for a move-only action"); x = bow; x.target = 0; Row(!on(x), "... not without a target");
            Row(!on(Act(Family::Retreat, 0x91, 0.9f, 0.0f, Sight::None)), "... not for a movement"); Row(!on(Act(Family::CastHeal, 0x11, 0.9f, 0.0f, Sight::None)), "... not for what he does to himself"); Row(!on(Act(Family::Attack, 0x99, 0.8f, 1800.0f, Sight::Visible)), "... not for a target that is not there");
            auto sc = bow; sc.family = Family::Scroll; sc.scrollOf = Family::CastOffense; Row(on(sc), "... a scroll as the family it reads"); w.kinds &= ~KindBit(SpotKind::Hold); Row(!on(bow), "... not when his kinds leave it out"); w.kinds = 0xFFFFFFFFu; p.caps.strict = false; Row(!on(bow), "... not without the strict cap"); p.caps.strict = true;
            x = blade; x.reach = 100.0f; w.order.mode = MoveMode::Stand; w.order.p = w.p; w.order.leashR = 64.0f; Row(on(x), "... and while he holds, his leash is the play at the edge of the reach"); x.reach = 60.0f; Row(!on(x), "... no more than that");
            Row((SpotKindsFor(ClassGate{}) & KindBit(SpotKind::Hold)) && !(AllowedKinds(RoadCaps{}, t) & KindBit(SpotKind::Hold)), "every class may hold, the point road never does"); }
        {   // THE PIN: no hold consideration read = exactly the pass without the kind. And at the weights as shipped a hold is made
            static const Profile none = [] { Profile p; p.hold.fill(0.0f); return p; }(); static const MP::ClassProfiles noHold = [](std::uint8_t) -> const Profile& { return none; };
            static const Profile stayed = [] { Profile p; p.hold.fill(0.0f); p.spot[0] = 0.8f; return p; }(); static const MP::ClassProfiles stays = [](std::uint8_t) -> const Profile& { return stayed; };
            bool same = true; int held = 0, windows = 0;
            for (const MP::ClassProfiles prof : { noHold, stays }) for (int scene = 0; scene < 4; ++scene) {
                const auto hall = Build(Hall()); const MD::Mesh& mesh = scene == 3 ? hall : m; PartyPicture a = scene == 3 ? Crowd(6) : RoomFight(); if (scene == 1) for (auto& f : a.foes) f.target = 0x12; if (scene == 2) for (auto& w : a.party) for (auto& x : w.top) x.sight = Sight::Visible, x.reach = 2500.0f;
                a.caps.strict = a.caps.kite = true; for (auto& f : a.foes) { f.reach = 150.0f; f.speed = 300.0f; f.ranged = MFO::Iaus::Tri::No; } PartyPicture b = a, c = a; for (auto& w : b.party) w.kinds &= ~KindBit(SpotKind::Hold);
                for (int w = 0; w < 4; ++w) { const auto ra = Window(mesh, a, t, 1.0e9f, true, prof), rb = Window(mesh, b, t, 1.0e9f, true, prof); Window(mesh, c, t); same = same && Same(ra.as, rb.as) && SameAsks(ra.asks, rb.asks); ++windows; for (const auto& x : ra.as) same = same && x.kinds != KindBit(SpotKind::Hold); for (const auto& x : c.party) held += x.order.mode == MoveMode::Stand && x.order.p.x == x.p.x && x.order.p.y == x.p.y; } }
            Row(same && windows == 32, "hold: every weight 0 = the same asks and assignments as with the kind masked off"); Row(held > 0, "... and with the weights as shipped somebody is held"); }
        {   // the three old scenes of StayMode, by weights that read what it read (foes on him, time to contact, sight)
            static const Profile old = [] { Profile p; p.hold.fill(0.0f); for (const Cons k : { Cons::HoldFoesOn, Cons::HoldContact, Cons::HoldSight }) p.hold[static_cast<std::size_t>(k) - kHoldFirst] = 1.0f; return p; }();
            PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(5, 6), {}) }; p.foes = { Bad(0x91, At(6, 6), 0x11), Bad(0x92, At(4, 6), 0x99) }; for (auto& f : p.foes) { f.reach = 150.0f; f.speed = 370.0f; }
            const auto mode = [&](const RankedAction& a) { PartyPicture q = p; q.party[0].top[0] = a; q.party[0].nTop = 1; q.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Hold); Window(m, q, t, 1.0e9f, true, [](std::uint8_t) -> const Profile& { return old; }); return q.party[0].order.mode; };
            const auto blade = Act(Family::Attack, 0x91, 0.8f, 150.0f, Sight::None, true), bow = Act(Family::Attack, 0x91, 0.8f, 1800.0f, Sight::Visible);
            Row(mode(blade) == MoveMode::Free, "a blade with one foe on him fights freely"); p.foes[1].target = 0x11; Row(mode(blade) == MoveMode::Stand, "... with two he holds"); { PartyPicture q = p; q.party[0].top[0] = blade; q.party[0].nTop = 1; q.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Hold); static const Profile ship; const auto go = [&] { Window(m, q, t, 1.0e9f, true, [](std::uint8_t) -> const Profile& { return ship; }); return q.party[0].order.mode; }; const auto a = go(); q.foes[1].target = 0x99; Row(a == MoveMode::Stand && go() == MoveMode::Free, "... and held by two, with one of them gone he is let go, at the weights as shipped (the momentum does not carry it)"); } p.foes[1].p = At(2, 6); Row(mode(blade) == MoveMode::Free, "... with two, one of which does not reach him, he does not (review F7)"); p.foes[1].p = At(4, 6);
            p.foes[0].p = At(9, 6); Row(mode(blade) == MoveMode::Free, "... nor out of reach of his target"); p.foes[0].p = At(6, 6); Row(mode(bow) == MoveMode::Free, "a shooter with a foe in his face is the engine's");
            p.foes[0].p = At(9, 6); p.foes[1].p = At(2, 2); Row(mode(bow) == MoveMode::Stand, "... with room he stands"); auto hid = bow; hid.sight = Sight::Occluded; Row(mode(hid) == MoveMode::Free, "... not without sight");
            p.foes[0].target = p.foes[1].target = 0; Row(mode(bow) == MoveMode::Stand, "... and with nobody on him"); }
        {   // EVERY CONSIDERATION BY ITSELF, filled by the pass: the one weight read decides between a hold and Free, and what is not known decides nothing
            static Profile one; static const MP::ClassProfiles prof = [](std::uint8_t) -> const Profile& { return one; };
            const auto bowAct = Act(Family::Attack, 0x91, 0.8f, 1800.0f, Sight::Unknown), bladeAct = Act(Family::Attack, 0x91, 0.8f, 150.0f, Sight::None, true);
            const auto held = [&](Cons k, const RankedAction& a, const std::function<void(PartyPicture&)>& set, float kindW = 1.0f, const MD::Mesh* mesh = nullptr, Tuning tt = ShippedTuning()) {
                one = Profile{}; one.hold.fill(0.0f); one.hold[static_cast<std::size_t>(k) - kHoldFirst] = 1.0f; one.spot[static_cast<std::size_t>(SpotKind::Hold)] = kindW;
                PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(5, 6), { a }) }; p.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Hold); p.foes = { Bad(0x91, At(6, 6), 0x11) }; set(p);
                Window(mesh ? *mesh : m, p, tt, 1.0e9f, true, prof); for (const Member& x : p.party) if (x.id == 0x11) return x.order.mode == MoveMode::Stand; return false; };
            const auto foe2 = [](PartyPicture& p) { p.foes.push_back(Bad(0x92, At(4, 6), 0x11)); for (auto& f : p.foes) f.reach = 150.0f; };
            Row(held(Cons::HoldFoesOn, bladeAct, foe2) && !held(Cons::HoldFoesOn, bladeAct, [&](PartyPicture& p) { foe2(p); p.foes[1].target = 0x12; }) && !held(Cons::HoldFoesOn, bladeAct, [&](PartyPicture& p) { foe2(p); p.foes[1].reach = -1.0f; }) && held(Cons::HoldFoesOn, bladeAct, [&](PartyPicture& p) { foe2(p); p.foes[1].reach = -1.0f; }, 1.05f) /* ... and not against it either */ && !held(Cons::HoldFoesOn, bowAct, foe2), "hold_foes_on: two that reach him. One, or one whose reach is not read, or a bow in his hands: no");
            const auto come = [](float d, float speed) { return [d, speed](PartyPicture& p) { p.foes[0].p.x = p.party[0].p.x + d; p.foes[0].reach = 150.0f; p.foes[0].speed = speed; }; };
            Row(held(Cons::HoldContact, bowAct, come(600, 370)) && !held(Cons::HoldContact, bowAct, come(250, 370)) && held(Cons::HoldContact, bowAct, come(250, 60)) && held(Cons::HoldContact, bowAct, come(200, 0)) && !held(Cons::HoldContact, bowAct, come(600, -1)) && !held(Cons::HoldContact, bladeAct, come(140, 0)), "hold_contact_sec: the time left, by his reach and his pace. Not read: nothing. A blade does not wait for it");
            for (const bool nearFirst : { true, false }) Row(!held(Cons::HoldContact, bowAct, [&](PartyPicture& p) { p.foes.push_back(Bad(0x92, At(6, 6), 0x11)); for (std::size_t i = 0; i < 2; ++i) { p.foes[i].p.x = p.party[0].p.x + ((i == 0) == nearFirst ? 250.0f : 1400.0f); p.foes[i].reach = 150.0f; p.foes[i].speed = 370.0f; } }), "... of two on him the one that is there first counts");
            Row(held(Cons::HoldFoeSpeed, bowAct, come(250, 500)) && !held(Cons::HoldFoeSpeed, bowAct, come(250, 150)) && !held(Cons::HoldFoeSpeed, bowAct, [&](PartyPicture& p) { come(250, 500)(p); p.foes[0].target = 0x12; }) && !held(Cons::HoldFoeSpeed, bowAct, [](PartyPicture&) {}), "hold_foe_speed: the fastest of those on him");
            const auto shoots = [](MFO::Iaus::Tri r) { return [r](PartyPicture& p) { p.foes[0].ranged = r; }; };
            Row(held(Cons::HoldFoeRanged, bowAct, shoots(MFO::Iaus::Tri::No), 1.05f) && !held(Cons::HoldFoeRanged, bowAct, shoots(MFO::Iaus::Tri::Yes), 1.05f) && held(Cons::HoldFoeRanged, bowAct, shoots(MFO::Iaus::Tri::Unknown), 1.05f), "hold_foe_ranged: standing in a shooter's line is worth less");
            auto sees = bowAct, hid = bowAct; sees.sight = Sight::Visible; hid.sight = Sight::Occluded; Row(held(Cons::HoldSight, sees, [](PartyPicture&) {}) && !held(Cons::HoldSight, hid, [](PartyPicture&) {}, 1.05f) && held(Cons::HoldSight, bowAct, [](PartyPicture&) {}, 1.05f), "hold_sight: yes, no, unknown is neither");
            const auto fact = [](MFO::Iaus::Tri s) { return [s](PartyPicture& p) { PairFact f; f.sees = s; f.t = p.t; f.actorTri = kAnyTri; f.actorAt = p.foes[0].p; f.spotAt = p.party[0].p; p.facts[{ KeyOfStay(0x11), 0x91 }] = f; }; };
            Row(held(Cons::HoldCover, bowAct, [&](PartyPicture& p) { MP::Freeze(p, m.Get(), t); fact(MFO::Iaus::Tri::No)(p); }) && !held(Cons::HoldCover, bowAct, [&](PartyPicture& p) { MP::Freeze(p, m.Get(), t); fact(MFO::Iaus::Tri::Yes)(p); }) && !held(Cons::HoldCover, bowAct, [](PartyPicture&) {}), "hold_cover: those on him who do not see him there, as answered");
            const auto gap = Build({ "##########", "#...######", "#........#", "#...######", "##########" }, {}, 100.0f), wide = Build({ "##########", "#...#....#", "#........#", "#........#", "##########" }, {}, 100.0f); const auto two = [](PartyPicture& p) { p.party[0].p = { 250.0f, 250.0f, 0.0f }; p.foes[0].p = { 850.0f, 250.0f, 0.0f }; };
            Row(held(Cons::HoldNarrow, bowAct, two, 1.0f, &gap) && !held(Cons::HoldNarrow, bowAct, two, 1.0f, &wide) && !held(Cons::HoldNarrow, bowAct, [&](PartyPicture& p) { two(p); p.foes.push_back(Bad(0x90, { 350.0f, 250.0f, 0.0f }, 0)); }, 1.0f, &gap), "hold_narrow: a doorway between him and the nearest foe. The nearest foe's way is not known: nothing");
            const auto ally = [](float x, float y, FormID on) { return [=](PartyPicture& p) { p.foes[0].p = At(10, 6); p.party.push_back(Who(0x12, At(x, y), {})); p.foes[0].target = on; }; };
            Row(held(Cons::HoldAllyFront, bowAct, ally(8, 6, 0x12)) && !held(Cons::HoldAllyFront, bowAct, ally(8, 6, 0x11)) && !held(Cons::HoldAllyFront, bowAct, ally(2, 6, 0x12)) && !held(Cons::HoldAllyFront, bowAct, ally(8, 2, 0x12)), "hold_ally_front: an ally with a foe on him, between him and it");
            const auto player = [](float d, float vel, bool moves) { return [=](PartyPicture& p) { Member& w = p.party[0]; w.leash = 1000.0f; w.anchor = w.p; w.anchor.x -= d; w.anchorVel = { -vel, 0.0f, 0.0f }; w.anchorMoves = moves; }; };
            const auto playerY = [](float d, float vel) { return [=](PartyPicture& p) { Member& w = p.party[0]; w.leash = 1000.0f; w.anchor = w.p; w.anchor.y -= d; w.anchorVel = { 0.0f, -vel, 0.0f }; w.anchorMoves = true; }; };
            Row(held(Cons::HoldPlayer, bowAct, player(300, 0, true), 1.05f) && !held(Cons::HoldPlayer, bowAct, player(900, 0, false), 1.05f) && !held(Cons::HoldPlayer, bowAct, player(400, 300, true), 1.05f) && held(Cons::HoldPlayer, bowAct, player(400, 300, false), 1.05f) && !held(Cons::HoldPlayer, bowAct, playerY(400, 300), 1.05f) && held(Cons::HoldPlayer, bowAct, playerY(400, 0), 1.05f) && held(Cons::HoldPlayer, bowAct, [](PartyPicture&) {}, 1.05f), "hold_player: near the leash, or where he will be in t.orderLeadSec. No leash: nothing");
            const auto stood = [](float off) { return [off](PartyPicture& p) { MoveOrder& o = p.party[0].order; o.mode = MoveMode::Kite; o.p = p.party[0].p; o.p.x += off; o.tri = 3; o.leashR = 40.0f; o.arriveR = 24.0f; o.gen = 4; }; }; Tuning flat = t; flat.orderHoldMomentum = 1.0f;
            Row(held(Cons::HoldKept, bowAct, stood(70.0f)) && !held(Cons::HoldKept, bowAct, stood(90.0f)) && !held(Cons::HoldKept, bowAct, stood(0.0f), 1.0f, nullptr, flat) && !held(Cons::HoldKept, bowAct, [](PartyPicture&) {}), "hold_kept: the point he holds, by t.orderHoldMomentum");
            {   // F1: on a committed place the hold's momentum is no reason over the place
                one = Profile{}; one.hold.fill(0.0f); one.hold[static_cast<std::size_t>(Cons::HoldKept) - kHoldFirst] = 1.0f; PartyPicture p = RoomFight(); p.caps.strict = true; for (int w = 0; w < 3; ++w) Window(m, p, t, 1.0e9f, true, prof);
                int placed = 0, kept = 0; std::vector<FormID> who; for (auto& w : p.party) if (w.commit.on) { ++placed; who.push_back(w.id); w.p = w.commit.p; w.tri = w.commit.tri; w.marks |= kMarkArrived; for (auto& a : w.top) if (a.sight == Sight::Occluded) a.sight = Sight::Visible; }
                const Ran r = Window(m, p, t, 1.0e9f, true, prof); for (const FormID id : who) { const Assignment* a = Of(r, id); kept += a && a->spot >= 0; }
                Row(placed > 0 && kept == placed, "hold_kept: on his committed place it is not his reason to leave it for a hold"); }
            {   // R2-1: ... nor is any other factor a hold has and the place has not
                one = Profile{}; one.hold.fill(0.0f); one.hold[static_cast<std::size_t>(Cons::HoldSight) - kHoldFirst] = 1.0f; PartyPicture p = RoomFight(); p.caps.strict = true; for (int w = 0; w < 3; ++w) Window(m, p, t, 1.0e9f, true, prof);
                one.hold[static_cast<std::size_t>(Cons::HoldSight) - kHoldFirst] = 40.0f; one.hold[static_cast<std::size_t>(Cons::HoldKept) - kHoldFirst] = 8.0f; std::vector<FormID> who; for (auto& w : p.party) if (w.commit.on) { who.push_back(w.id); w.p = w.commit.p; w.tri = w.commit.tri; w.marks |= kMarkArrived; for (auto& a : w.top) if (a.sight != Sight::None) a.sight = Sight::Visible; }
                const Ran r = Window(m, p, t, 1.0e9f, true, prof); int kept = 0; for (const FormID id : who) { const Assignment* a = Of(r, id); kept += a && a->kinds != KindBit(SpotKind::Hold); }
                Row(!who.empty() && kept == static_cast<int>(who.size()), "hold: no factor of its own takes his committed place over (R2-1)"); }
            SpotFacts f; f.rangeX = 0.5f; Profile p; const float stay = ScoreSpot(f, Family::Attack, p, t); f.hold = true; f.holdFoesOn = 1.5f; f.holdContactSec = 3.0f; std::vector<Term> terms; const float two2 = ScoreSpot(f, Family::Attack, p, t, &terms); int n = 0; for (const auto& x : terms) n += static_cast<std::size_t>(x.cons) >= kHoldFirst; f.holdPlayer = std::numeric_limits<float>::quiet_NaN();
            Row(n == 3 && two2 > stay && ScoreSpot(f, Family::Attack, p, t) == 0.0f, "what is not read is no term, a NaN never scores"); p.hold.fill(0.0f); Row(ScoreSpot(f, Family::Attack, p, t) == stay, "... and at weight 0 nothing is read");
            ClassCurves::Row cr; Row(ClassCurves::Build(cr).hold == Profile{}.hold && ClassCurves::SetHoldWeight(cr, Cons::HoldCover, 0.0f) && ClassCurves::SetHoldWeight(cr, Cons::HoldKept, 2.0f) && ClassCurves::Build(cr).hold[5] == 0.0f && ClassCurves::Build(cr).hold[9] == 2.0f && !ClassCurves::SetHoldWeight(cr, Cons::SpotPath, 1.0f) && !ClassCurves::SetHoldWeight(cr, Cons::kCount, 1.0f) && !ClassCurves::SetHoldWeight(cr, Cons::HoldCover, -1.0f), "a class's hold weight reaches the profile: 0 is not read, below 0 and a stranger are refused"); }
        {   // HYSTERESIS: a foe, then the reach, on the edge for 20 windows. Every change is a claim filed or released in the game
            const auto changes = [&](bool reach, float momentum) { Tuning tt = t; tt.orderHoldMomentum = momentum; PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(3, 6), { Act(Family::Attack, 0x91, 0.8f, reach ? 400.0f : 1800.0f, Sight::Visible) }) }; p.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Hold); p.foes = { Bad(0x91, At(6, 6), 0x11) }; p.foes[0].reach = 150.0f; p.foes[0].speed = reach ? 40.0f : 370.0f; p.foes[0].ranged = MFO::Iaus::Tri::No;
                int n = 0; MoveOrder was; for (int w = 0; w < 20; ++w) { p.foes[0].p.x = p.party[0].p.x + (reach ? (w % 2 ? 410.0f : 390.0f) : (w % 2 ? 320.0f : 390.0f)); Window(m, p, tt); if (w) n += !p.party[0].order.Same(was); was = p.party[0].order; } return n; };
            Row(changes(false, t.orderHoldMomentum) <= 1 && changes(false, 1.0f) > 10, "on the edge of the old flip line the order changes at most once in 20 windows (with no momentum it flips)"); Row(changes(true, t.orderHoldMomentum) <= 1, "... and on the edge of his reach"); }
        {   PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(3, 6), { Act(Family::Attack, 0x91, 0.8f, 1800.0f, Sight::Visible) }) }; p.party[0].kinds = KindBit(SpotKind::Stay) | KindBit(SpotKind::Hold); p.foes = { Bad(0x91, At(9, 6), 0) }; const auto one = Window(m, p, t); p.party[0].p.x += 50.0f; const auto two = Window(m, p, t);
            Row(!Of(one, 0x11)->kept && Of(two, 0x11)->kept && Of(two, 0x11)->spot < 0 && Of(two, 0x11)->p.x == p.party[0].p.x - 50.0f && Of(two, 0x11)->kinds == KindBit(SpotKind::Hold), "the assignment of a hold says its point, and that it is the one that stood"); }
        {   // 24 permutations with the hold in the table
            PartyPicture base = RoomFight(); base.caps.strict = base.caps.kite = true; for (auto& w : base.party) w.top[0].sight = Sight::Visible, w.top[0].reach = (std::max)(w.top[0].reach, 1500.0f); for (auto& f : base.foes) { f.reach = 150.0f; f.speed = 300.0f; f.ranged = MFO::Iaus::Tri::No; }
            PartyPicture ref = base; std::vector<Ran> want; std::vector<std::vector<MoveOrder>> ord; int stood = 0; for (int w = 0; w < 4; ++w) { want.push_back(Window(m, ref, t)); ord.emplace_back(); for (const auto& x : ref.party) ord.back().push_back(x.order); for (const auto& x : want.back().as) stood += x.kinds == KindBit(SpotKind::Hold); }
            std::array<std::size_t, 4> idx{ 0, 1, 2, 3 }; bool ok = true; do { PartyPicture p = base; for (std::size_t i = 0; i < 4; ++i) p.party[i] = base.party[idx[i]]; std::reverse(p.foes.begin(), p.foes.end()); for (std::size_t w = 0; w < 4; ++w) { const auto r = Window(m, p, t); ok = ok && Same(r.as, want[w].as) && SameAsks(r.asks, want[w].asks); for (std::size_t i = 0; i < 4; ++i) ok = ok && p.party[i].order.Same(ord[w][i]) && p.party[i].order.gen == ord[w][i].gen; } } while (std::next_permutation(idx.begin(), idx.end()));
            Row(ok && stood > 0, "24 permutations with holds in the table: the same assignments and orders"); }
        {   // a hold keeps its point, its gen and its leash; another order is another gen
            PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(5, 6), { Act(Family::Attack, 0x91, 0.8f, 1800.0f, Sight::Visible) }) }; p.party[0].tri = 3; p.foes = { Bad(0x91, At(10, 6), 0) };
            Assignment a; a.who = 0x11; a.p = p.party[0].p; a.tri = 3; a.act = p.party[0].top[0]; a.kinds = KindBit(SpotKind::Hold); std::vector<Assignment> as{ a }; MP::Orders(p, as, t); const MoveOrder one = p.party[0].order; Row(one.mode == MoveMode::Stand && one.gen == 1 && one.p.x == a.p.x && one.leashR == t.orderLeash, "he is held where he stands");
            as[0].p.x += 50.0f; p.party[0].p = as[0].p; MP::Orders(p, as, t); Row(p.party[0].order.Same(one) && p.party[0].order.gen == 1, "... pushed inside twice his leash: the same order"); as[0].p.x += 200.0f; p.party[0].p = as[0].p; MP::Orders(p, as, t); Row(p.party[0].order.gen == 2 && p.party[0].order.p.x == as[0].p.x, "... carried off: held where he is now");
            {   PartyPicture k = p; MoveOrder& o = k.party[0].order; o.mode = MoveMode::Kite; o.p = k.party[0].p; o.p.x += 30.0f; o.leashR = 40.0f; o.arriveR = 20.0f; o.gen = 7; MP::Orders(k, as, t); Row(o.mode == MoveMode::Stand && o.p.x == k.party[0].p.x + 30.0f && o.gen == 8, "... from a Kite order: held at the kite's point (F4)"); Row(o.leashR == 40.0f && o.arriveR == 20.0f, "... at the radii it had, a pin stays a pin (F4)"); }
            as[0].kinds = KindBit(SpotKind::Stay); MP::Orders(p, as, t); Row(p.party[0].order.mode == MoveMode::Free && p.party[0].order.gen == 3, "... he stays: Free, whatever stood before"); as[0].kinds = KindBit(SpotKind::Hold); MP::Orders(p, as, t);
            as.clear(); MP::Orders(p, as, t); Row(p.party[0].order.mode == MoveMode::Free, "... nobody decides for him: Free"); mem(p, 0x11).decides = false; as.push_back(a); MP::Orders(p, as, t); Row(p.party[0].order.mode == MoveMode::Free, "... a body the pass plans around gets no order"); }
        {   // a bit whose cap went off since he committed orders nothing (F6)
            PartyPicture p; p.caps.strict = true; p.party = { Who(0x11, At(5, 6), {}) }; Assignment a; a.who = 0x11; a.spot = 0; a.p = At(7, 6); a.kinds = KindBit(SpotKind::RetreatExit) | KindBit(SpotKind::Kite) | KindBit(SpotKind::Firing); const auto mode = [&] { return OrderOf(p, p.party[0], a, t).mode; };
            Row(mode() == MoveMode::Stand, "a kept place whose kite and disengage caps went off: its Stand is left"); p.caps.kite = true; Row(mode() == MoveMode::Kite, "... with the kite cap: Kite"); p.caps.disengage = true; Row(mode() == MoveMode::Disengage, "... with both: Disengage"); p.caps.disengage = p.caps.kite = false; a.kinds = KindBit(SpotKind::RetreatExit); Row(mode() == MoveMode::Free, "... nothing left: Free");
            a.kinds = KindBit(SpotKind::Firing) | KindBit(SpotKind::Cover); p.party[0].kinds = KindBit(SpotKind::Firing); const MoveOrder o = OrderOf(p, p.party[0], a, t); Row(o.mode == MoveMode::Stand && o.leashR == t.orderLeash, "... nor a kind his class does not have"); }
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
                    MD::Write(f, name + "_" + std::to_string(w), mesh, before, r.answers, r.as, pic, w > 0, set);
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
            MP::Answer(p, c->answers, t); MP::Phase3(p, &Profiles, t, as); MP::Adopt(p, as); MP::Orders(p, as, t);
            bool exact = as.size() == c->expect.size(); float worst = 0.0f;
            for (std::size_t i = 0; exact && i < as.size(); ++i) {
                const auto& e = c->expect[i];
                exact = as[i].who == e.who && as[i].key == e.key && static_cast<int>(as[i].action) == e.family && as[i].form == e.form && as[i].target == e.target;
                for (const auto& m : p.party) if (m.id == e.who) exact = exact && m.order.Same(e.order) && m.order.target == e.order.target && m.order.gen == e.order.gen;
                worst = (std::max)(worst, std::fabs(as[i].score - e.score));
            }
            char b[160]; std::snprintf(b, sizeof b, "%s: the same asks, the same assignments, scores within 1e-4 (%g)", c->name.c_str(), static_cast<double>(worst));
            Row(asked && exact && worst <= 1.0e-4f, b);
            ++cases;
        }
        Row(err.empty() && cases == 42, "every golden case was read");
    }
}

int MoveRows(const char* a_golden, bool a_write) {
    g_bad = 0;
    Determinism(); Tunables(); Scoring(); Behaviour(); Budgets(); Review(); TrustRows(); LightRows(); OrderRows(); Golden(a_golden, a_write);
    return g_bad;
}

#ifdef MOVE_STANDALONE
int main(int argc, char** argv) {   // move_rows <golden file> [--write]
    const int bad = MoveRows(argc > 1 ? argv[1] : "golden_move.txt", argc > 2 && std::string(argv[2]) == "--write");
    std::printf("\n%d movement check(s) failed\n", bad);
    return bad;
}
#endif
