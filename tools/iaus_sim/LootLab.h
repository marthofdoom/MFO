#pragma once
// iaus_sim/LootLab.h -- SIM V4 BATCH L: the EPISODE around Loot.h's rules (iaus-sim-v4.md 14). HOST-ONLY.
//
// An episode is one place and one clock: a fight (the place's foes), the looting after it with the player doing something meanwhile, maybe a
// second group of foes arriving in the middle of the looting, and the looting after that. The fight is the fight world's own (World.h, the
// policy of the run). Out of combat this file steps the world: the player's script, the leg mover (the lab's stand-in for Harbinger ch.19),
// the follow package, the guards' patrol, and one loot service per pump (Loot.h World::Service).
//
// VALUE (14.4, the exchange of Logistics.h kept): the fights' Utility with potions NOT charged there, plus what the bags gained: the potion
// worth (PotionWorth.h, the k-th potion of a kind) and GoldJ x gold-equivalent. Reported with and without the gold term.
#include "iaus_sim/Loot.h"
#include "iaus_sim/Oracle.h"

#include <cstdarg>
#include <cstring>
#include <functional>

namespace Sim::Loot {

    enum class Plan : int { Stand = 0, WalkOn, OpenChest, LootsToo, FaceAway, kCount };
    inline const char* PlanName(Plan p) { static const char* n[] = { "stands", "walks-on", "opens-chest", "loots-too", "faces-away" }; return n[static_cast<int>(p)]; }
    struct EpSpec {
        const SimEnv* env = nullptr; std::uint64_t seed = 1; Plan plan = Plan::Stand; double standSec = 6.0;
        double waveAt = -1.0;          // seconds into the looting at which a second group arrives (< 0: none)
        double lootSec = 90.0;         // the looting ends here at the latest, or once nobody has had a slot for 12 s and 60 s have passed (the abandon clock of the player's dibs is 45 s)
        bool looting = true, wide = false; int libGuards = 0;
        const Rules* rules = nullptr; const MFO::Iaus::LootTuning* tuning = nullptr;   // a scenario's own (default: the run's g_rules and Tune().loot)
        std::function<void(struct Episode&)> setup;   // a scenario's hand-made start (after the sources are made)
    };

    struct Episode {
        Fight F; World W; EpSpec sp; Rng R;
        int pstage = 0; double pAt = 0, lootStart = 0, phaseStart = 0, quietSince = -1, combatStart = -1, tWave = -1; EnvPoint pGoal; int pSrc = -1; bool waved = false;
        std::vector<std::uint32_t> corpsed; double downs0 = 0; int fights = 0; bool stopAfterFight = false, lostFight = false; double doorOpenAt = -1;   // doorOpenAt: the player unlocks and opens every locked door then (a scenario)
        std::vector<signed char> forced; int decisions = 0;   // the loot oracle's forced options, by decision number (beyond the list: the game decides)
        std::vector<std::pair<int, int>> skipSrc;             // (follower, source) the oracle told him not to walk to

        Episode() = default;
        Episode(const Episode& o) { *this = o; }
        Episode& operator=(const Episode& o) { F = o.F; W = o.W; sp = o.sp; R = o.R; pstage = o.pstage; pAt = o.pAt; lootStart = o.lootStart; phaseStart = o.phaseStart; quietSince = o.quietSince; combatStart = o.combatStart; tWave = o.tWave; pGoal = o.pGoal; pSrc = o.pSrc; waved = o.waved;
            corpsed = o.corpsed; downs0 = o.downs0; fights = o.fights; stopAfterFight = o.stopAfterFight; lostFight = o.lostFight; doorOpenAt = o.doorOpenAt; forced = o.forced; decisions = o.decisions; skipSrc = o.skipSrc; Fix(); return *this; }
        void Fix() { W.Fp = &F; W.force = nullptr; if (sp.wide) W.force = [this](int fi, int si, Cat c) { (void)fi; (void)si; (void)c; const int n = decisions++; return n < static_cast<int>(forced.size()) ? static_cast<int>(forced[Z(n)]) : -1; }; }

        [[nodiscard]] const SimEnv& E() const { return *F.env; }
        [[nodiscard]] EQ::View View() const { return F.env->View(F.doors, F.doorStamp); }

        // ── the sources of the place ──
        void AddSrc(Src s) { s.id = static_cast<std::uint32_t>(W.src.size() + 1); s.born = F.now; for (const Stack& st : s.items) if (st.count > 0) ++W.t.stacks0; W.src.push_back(std::move(s)); }
        void BodyLoot(std::vector<Stack>& out) {
            if (g_list.on && !g_list.bodies.empty()) { const auto& row = g_list.bodies[Z(R.I(0, static_cast<int>(g_list.bodies.size()) - 1))]; for (const auto& [k, n] : row) Resolve(k, n, F.b[0].c.level, R, out); if (R.P(0.5f)) Resolve("skyrim.esm:f", R.I(3, 40), 1, R, out); }   // MODEL: half the bodies carry coin (a list's NPC gold is a leveled list on its death item or none at all)
            else { const auto st = Logi::MakeStop(R); out = st[0].items; }
        }
        void MakeSources() {
            const SimEnv& e = E();
            for (const LootRef& r : e.loot) {
                if (r.kind == mfoenv::kActivator) { W.activators.push_back(r.p); continue; }
                Src s; s.p = r.p; s.on = r.on; s.tri = r.tri; s.navDist = r.navDist; s.owned = r.owned; s.locked = r.locked;
                if (r.kind == mfoenv::kContainer) { s.kind = SrcKind::Container;
                    if (g_list.on && !e.fixture) { if (const auto it = g_list.cont.find(r.key); it != g_list.cont.end()) for (const auto& [k, n] : it->second) Resolve(k, n, F.b[0].c.level, R, s.items); }
                    else { const auto st = Logi::MakeStop(R); s.items = st.back().items; } }
                else if (r.kind == mfoenv::kItem) { s.kind = SrcKind::Loose; const int n = std::max<int>(1, r.count);
                    if (e.fixture) { const int it = Logi::ItemIdx(r.name.c_str()); if (it >= 0) s.items.push_back({ it, n }); }
                    else if (g_list.on) { if (const auto it = g_list.item.find(r.key); it != g_list.item.end()) s.items.push_back({ it->second, n }); } }
                else { s.kind = SrcKind::Flora;
                    if (e.fixture) { s.coin = true; s.items.push_back({ Logi::ItemIdx("Gold001"), R.I(5, 25) }); }
                    else if (g_list.on) { if (const auto it = g_list.flora.find(r.key); it != g_list.flora.end()) { Resolve(it->second, 1, F.b[0].c.level, R, s.items); s.coin = !s.items.empty() && Items()[Z(s.items[0].item)].kind == Kind::Gold; } } }
                if (s.items.empty()) continue;
                AddSrc(std::move(s)); }
            W.town = e.town;
            for (const LootSpot& g : e.guards) W.guards.push_back({ g.a, g.b, g.a, 0.0f, 1.0f, 90.0f, true });
            for (const LootSpot& g : e.bystanders) W.guards.push_back({ g.a, g.a, g.a, 0.0f, 1.0f, 0.0f, false });
            for (const LootSpot& g : e.bodies) { Src s; s.kind = SrcKind::Corpse; s.p = g.a; s.tri = EQ::NearestTri(View(), g.a, 300.0f, s.on); s.hostileDeath = g.flag; BodyLoot(s.items); if (s.items.empty()) s.items.push_back({ Logi::ItemIdx("Gold001"), 12 }); AddSrc(std::move(s)); }
            // A library place with owned things is a civilised one (MODEL: which of its actors are guards is not in the file): sp.libGuards of its spawn points hold a guard.
            if (!e.fixture && sp.libGuards > 0) { bool owned = false; for (const Src& s : W.src) owned = owned || s.owned;
                if (owned) for (int g = 0; g < sp.libGuards && !e.spawns.empty(); ++g) { const EnvPoint& a = e.spawns[Z(R.I(0, static_cast<int>(e.spawns.size()) - 1))]; W.guards.push_back({ a.p, a.p, a.p, 0.0f, 1.0f, 0.0f, true }); } }
        }
        void AddCorpses() {
            for (const Body& x : F.b) { if (x.role != Role::Foe || x.alive || std::find(corpsed.begin(), corpsed.end(), x.c.id) != corpsed.end()) continue; corpsed.push_back(x.c.id);
                Src s; s.kind = SrcKind::Corpse; s.p = { x.c.x, x.c.y, x.c.z }; s.tri = x.nav.tri; s.on = s.p; if (s.tri < 0) s.tri = EQ::NearestTri(View(), s.p, 300.0f, s.on); s.body = x.c.id; BodyLoot(s.items); W.hostileDeathKnown = true;
                if (!s.items.empty()) AddSrc(std::move(s)); }
        }

        // ── the start ──
        void Begin(const std::vector<SimClass>& all) {
            Fix(); W.R = sp.rules ? *sp.rules : g_rules; W.T = sp.tuning ? sp.tuning : &Tune().loot; W.oracleWide = sp.wide; R.g.seed(sp.seed * 0x9E3779B97F4A7C15ULL + 0xB5297A4D3F84D5B5ULL);
            for (std::size_t ci = 0; ci < F.ctl.size(); ++ci) { FState f; f.ci = static_cast<int>(ci); const Ctl& c = F.ctl[ci]; const Body& x = F.b[Z(c.body)];
                const SimClass* sc = nullptr; for (const SimClass& s : all) if (s.fam == c.fam && (!sc || s.name == x.c.name)) sc = &s;
                f.who.fam = c.fam; f.who.cls = sc ? sc->name : "?"; f.who.worth = c.worth; f.thief = x.stealthy;
                f.inv = Logi::StartKit(f.who); f.inv.s.erase(std::remove_if(f.inv.s.begin(), f.inv.s.end(), [](const Stack& s) { const Kind k = Items()[Z(s.item)].kind; return k == Kind::PotionH || k == Kind::PotionM || k == Kind::PotionS; }), f.inv.s.end());
                if (c.book.kit.potionsHealth > 0) f.inv.Add(Logi::ItemIdx("RestoreHealth02"), c.book.kit.potionsHealth); if (c.book.kit.potionsMagicka > 0) f.inv.Add(Logi::ItemIdx("RestoreMagicka02"), c.book.kit.potionsMagicka); if (c.book.kit.potionsStamina > 0) f.inv.Add(Logi::ItemIdx("RestoreStamina02"), c.book.kit.potionsStamina);
                f.inv0 = f.inv; f.nextLogi = 0; W.fs.push_back(std::move(f)); }
            if (sp.looting) MakeSources();
            if (sp.setup) sp.setup(*this);
        }
        // What he drank in the fight leaves the bag (the kit is the fight's count of the same stock).
        void SyncFromKit() { for (FState& f : W.fs) { const auto& kit = F.ctl[Z(f.ci)].book.kit; const std::pair<Kind, int> k[3] = { { Kind::PotionH, kit.potionsHealth }, { Kind::PotionM, kit.potionsMagicka }, { Kind::PotionS, kit.potionsStamina } };
                for (const auto& [kind, have] : k) { int over = f.inv.Count(kind) - have; for (Stack& s : f.inv.s) { if (over <= 0) break; if (Items()[Z(s.item)].kind != kind) continue; const int n = std::min(over, s.count); s.count -= n; over -= n; } } } }

        // ── a fight inside the episode ──
        [[nodiscard]] bool AnyFoe() const { for (const Body& x : F.b) if (x.role == Role::Foe && Fight::Up(x)) return true; return !F.late.empty(); }
        void EndFight(double t0) {
            const double now = F.now; const double downs = F.t.partyDowns; F.now = now - t0; F.t.partyDowns -= static_cast<decltype(F.t.partyDowns)>(downs0);
            W.t.uFights += Utility(F, false); F.now = now; F.t.partyDowns = static_cast<decltype(F.t.partyDowns)>(downs); downs0 = downs; ++fights;
            SyncFromKit(); if (sp.looting) AddCorpses(); W.lastCombatEnd = F.now; W.combat = false; F.over = false;
        }
        void Wave() {   // MODEL: the second group is up to three of the first fight's foes again, at a spawn point (else a triangle) 1200 to 2600 path units from the player
            waved = true; const SimEnv& e = E(); const Body& pl = F.b[0]; if (pl.nav.tri < 0) return; const auto field = e.Field(pl.nav.tri); EnvPoint at; float best = -1;
            auto score = [&](std::int32_t tri) { const float d = (*field)[Z(tri)]; return d > 1e29f ? -1.0f : d >= 1200.0f && d <= 2600.0f ? 1e6f - std::fabs(d - 1800.0f) : d; };
            for (const EnvPoint& p : e.spawns) { const float s = score(p.tri); if (s > best) { best = s; at = p; } }
            if (best < 1e5f) for (std::size_t i = 0; i < e.tris.size(); i += 1 + e.tris.size() / 400) { const float s = score(static_cast<std::int32_t>(i)); if (s > best) { best = s; at.tri = static_cast<std::int32_t>(i); at.p = EQ::Centroid(View(), at.tri); } }
            if (at.tri < 0) return; std::vector<std::shared_ptr<const ActorSpec>> specs; for (const Body& x : F.b) if (x.role == Role::Foe && !x.alive && specs.size() < 3) specs.push_back(x.spec);
            if (specs.empty()) return; float hp = 0;
            for (std::size_t i = 0; i < specs.size(); ++i) { Body& n = F.Add(Role::Foe, specs[i], at.p.x + 60.0f * static_cast<float>(i), at.p.y); n.c.z = at.p.z; n.nav = Nav{}; n.nav.tri = at.tri; EnvAdopt(F, n); n.c.targetId = F.b[0].c.id; hp += n.c.hpMax; }
            F.foeHpTotal = hp; F.maxSec = F.now + 150.0; tWave = F.now; ++W.t.waves;
            for (std::size_t f = 0; f < W.fs.size(); ++f) { W.t.lateU += World::D3(World::P(W.BodyOf(static_cast<int>(f))), World::P(F.b[0])); ++W.t.lateN; }
            W.Log("[wave] %zu foes arrive", specs.size());
        }

        // ── the player's script (MODEL: what marth's complaints name. He stands, walks on, opens a container, loots too, or stands with his back to it) ──
        void PlayerStep() {
            Body& pl = F.b[0]; const double t = F.now - lootStart; const float walk = 0.45f * pl.spec->run;
            if (doorOpenAt >= 0 && t >= doorOpenAt) { doorOpenAt = -1; const EQ::View v = View(); for (std::size_t i = 0; i < F.doors.size(); ++i) if (F.doors[i].state == EQ::DoorState::Locked) { const EQ::Vec3 at = EQ::Centroid(v, F.doors[i].tri); F.OpenDoor(static_cast<std::int32_t>(i)); W.DoorEvent(at); W.Ev("player opened a locked door"); } }
            auto go = [&](const EnvPoint& g, float speed) { Fight::Face(pl, g.p.x, g.p.y); F.Move(pl, g.p.x, g.p.y, speed, false, nullptr, &g); return std::hypot(pl.c.x - g.p.x, pl.c.y - g.p.y) < 60.0f; };
            auto nearest = [&](SrcKind k, bool withGold) { int b = -1; float bd = 1e30f; for (std::size_t i = 0; i < W.src.size(); ++i) { const Src& s = W.src[i]; if (s.kind != k || s.tri < 0 || s.owned || s.locked || s.claim.playerTook >= 0) continue; bool has = false; for (const Stack& st : s.items) has = has || (st.count > 0 && (!withGold || Items()[Z(st.item)].kind == Kind::Gold)); if (!has) continue;
                    const float d = World::D3(World::P(pl), s.p); if (d < bd) { bd = d; b = static_cast<int>(i); } } return b; };
            auto takeGold = [&](int si) { Src& s = W.src[Z(si)]; for (Stack& st : s.items) if (st.count > 0 && Items()[Z(st.item)].kind == Kind::Gold) { st.count = 0; W.Ev("player took the coin"); } for (Src& o : W.src) if (&o != &s && o.claim.playerTook >= 0) o.claim.rejected = true; s.claim.playerTook = F.now; };   // Sinks.cpp R1: taking from another source rejects the one before
            switch (sp.plan) {
            case Plan::Stand: break;
            case Plan::FaceAway: if (pstage == 0) { pstage = 1; float cx = 0, cy = 0; int n = 0; for (const Src& s : W.src) if (s.kind == SrcKind::Corpse) { cx += s.p.x; cy += s.p.y; ++n; } if (n) Fight::Face(pl, 2 * pl.c.x - cx / n, 2 * pl.c.y - cy / n); } break;
            case Plan::WalkOn:
                if (pstage == 0 && t >= sp.standSec) { pstage = 1; const SimEnv& e = E(); if (pl.nav.tri >= 0) { const auto field = e.Field(pl.nav.tri); float best = -1; for (std::size_t i = 0; i < e.tris.size(); ++i) { const float d = (*field)[i]; if (d < 1e29f && d > best) { best = d; pGoal.tri = static_cast<std::int32_t>(i); } } if (pGoal.tri >= 0) pGoal.p = EQ::Centroid(View(), pGoal.tri); } }
                if (pstage == 1 && pGoal.tri >= 0 && go(pGoal, walk)) pstage = 2;
                break;
            case Plan::OpenChest:
                if (pstage == 0) { pSrc = nearest(SrcKind::Container, false); pstage = pSrc >= 0 ? 1 : 9; }
                if (pstage == 1) { const Src& s = W.src[Z(pSrc)]; const EnvPoint g{ s.on, s.tri }; if (go(g, pl.spec->run) || World::D3(World::P(pl), s.p) < 150.0f) { pstage = 2; pAt = F.now; W.playerMenu = true; W.playerOpenSrc = pSrc; W.Ev("player opened a container"); } }
                if (pstage == 2 && F.now - pAt >= 6.0) { takeGold(pSrc); W.playerMenu = false; W.playerOpenSrc = -1; pstage = 9; }
                break;
            case Plan::LootsToo:
                if (pstage == 0) { pSrc = nearest(SrcKind::Corpse, true); pstage = pSrc >= 0 ? 1 : 9; }
                if (pstage == 1) { const Src& s = W.src[Z(pSrc)]; const EnvPoint g{ s.on, s.tri }; if (go(g, pl.spec->run) || World::D3(World::P(pl), s.p) < 150.0f) { pstage = 2; pAt = F.now; } }
                if (pstage == 2 && F.now - pAt >= 2.0) { takeGold(pSrc); pstage = 0; }
                break;
            default: break;
            }
        }

        // ── the leg mover: the lab's Harbinger ch.19 (a travel claim to a ref, radius Rules::arriveRadius; BLOCKED = Movement Blocked held blockedGateSec) ──
        void MoveFollower(int fi) {
            FState& f = W.fs[Z(fi)]; Slot& sl = f.slot; Body& x = W.BodyOf(fi); if (!Fight::Up(x)) return; const Body& pl = F.b[0];
            if (sl.active && sl.phase == Phase::Walking && sl.leg == LegState::Walking) {
                const Src& s = W.src[Z(sl.target)]; if (s.tri < 0) return; const EnvPoint g{ s.on, s.tri };
                F.Move(x, s.on.x, s.on.y, x.spec->run, false, nullptr, &g);
                if (World::D3(World::P(x), s.p) <= W.R.arriveRadius || World::D3(World::P(x), s.on) <= 24.0f) { /* the claim's radius is about the REF. A ref off the mesh: the leg ends where the mesh ends, at the point nearest it */ sl.leg = LegState::Arrived; sl.legAt = F.now; sl.drift = static_cast<float>(SimEnv::Mix(sp.seed * 31 + static_cast<std::uint64_t>(W.t.legs) * 977 + static_cast<std::uint64_t>(fi)) >> 40) * (1.0f / 16777216.0f) < W.R.driftShare; return; }
                if (F.now - sl.sampleAt >= 0.5) { const float m = std::hypot(x.c.x - sl.sx, x.c.y - sl.sy);
                    if (m < 15.0f) { if (sl.mbSince < 0) sl.mbSince = sl.sampleAt; } else { sl.mbSince = -1; sl.everMoved = true; }
                    sl.sampleAt = F.now; sl.sx = x.c.x; sl.sy = x.c.y; }
                // Movement Blocked: a route exists (or existed, or ends at a door he cannot open) and he makes no ground. No route at all is not a block:
                // the engine never moved him, and MFO's own no-progress rule (5 s) is what ends that leg.
                if (sl.mbSince >= 0 && F.now - sl.mbSince >= W.R.blockedGateSec && (sl.everMoved || x.nav.reach || x.nav.blocked)) {
                    sl.leg = LegState::Blocked; sl.legAt = F.now; sl.bx = x.c.x; sl.by = x.c.y; sl.bz = x.c.z; sl.blocker = 0;
                    const float tx = s.p.x - x.c.x, ty = s.p.y - x.c.y, tl = std::max(1.0f, std::hypot(tx, ty));   // LootTravel_internal.h FindActorBlocker: a living actor within 200, 128 of his height, within 45 degrees of the way to the target
                    for (const Body& o : F.b) { if (&o == &x || !Fight::Up(o)) continue; const float ox = o.c.x - x.c.x, oy = o.c.y - x.c.y, ol = std::hypot(ox, oy); if (ol > W.R.actorBlockU || ol < 1.0f || std::fabs(o.c.z - x.c.z) > W.R.actorBlockDz) continue; if ((ox * tx + oy * ty) / (ol * tl) >= 0.7071f) { sl.blocker = o.c.id; break; } } }
                return; }
            if (sl.active && (sl.leg == LegState::Blocked || (sl.leg == LegState::Arrived && !sl.drift))) return;   // he stands at the block, or at the ref (the claim holds him), until MFO's tick reads it
            if (World::D3(World::P(x), World::P(pl)) > W.R.followU) F.Move(x, pl.c.x, pl.c.y, x.spec->run, false, &pl);   // the follow package (also after a leg ended ARRIVED: this is the drift)
        }

        // ── one world step out of combat ──
        void QuietStep() {
            for (Body& x : F.b) { if (!x.alive) continue; if (x.c.downed) { if (F.now >= x.downUntil) { x.c.downed = false; x.c.hp = std::max(x.c.hp, F.D->rules.bleedoutDefault * x.c.hpMax); } continue; } F.Regen(x); x.act = Act::None; }
            PlayerStep();
            for (std::size_t f = 0; f < W.fs.size(); ++f) MoveFollower(static_cast<int>(f));
            for (Guard& g : W.guards) { if (g.speed <= 0.0f) continue; const float len = std::max(1.0f, World::D3(g.a, g.b)); g.t += g.dir * g.speed * TM::dt / len; if (g.t >= 1.0f) { g.t = 1.0f; g.dir = -1.0f; } else if (g.t <= 0.0f) { g.t = 0.0f; g.dir = 1.0f; } g.p = { g.a.x + (g.b.x - g.a.x) * g.t, g.a.y + (g.b.y - g.a.y) * g.t, g.a.z + (g.b.z - g.a.z) * g.t }; }
            EnvStep(F); F.now += TM::dt;
        }
        void Tallies() {
            LTally& t = W.t; const Body& pl = F.b[0]; int walking = 0;
            if (!W.combat) { t.lootSec += TM::dt; for (const FState& f : W.fs) if (Fight::Up(W.BodyOf(f.ci))) t.followerSec += TM::dt; }
            for (std::size_t f = 0; f < W.fs.size(); ++f) { const Slot& sl = W.fs[f].slot; if (!sl.active) { if (W.fs[f].stuck) t.stuckOffSec += TM::dt; continue; } const float d = World::D3(World::P(W.BodyOf(static_cast<int>(f))), World::P(pl));
                if (d > W.R.departRadius) t.awaySec += TM::dt; t.awayMax = std::max(t.awayMax, d);
                if (W.combat) t.combatLootSec += TM::dt;
                if (sl.phase == Phase::Walking) { ++walking; for (std::size_t o = f + 1; o < W.fs.size(); ++o) if (W.fs[o].slot.active && W.fs[o].slot.phase == Phase::Walking && W.fs[o].slot.target == sl.target) t.twoOnOneSec += TM::dt; } }
            (void)walking;
        }
        // "Walking past wanted loot": once a loot tick, per follower on a leg or following: a source within reach that holds what his own plan would take
        // now (released, not barred, not blocklisted) is noted, and counted when he has left it behind with that still true.
        void PastCheck(int fi) {
            FState& f = W.fs[Z(fi)]; if (f.near.size() < W.src.size()) f.near.resize(W.src.size(), 0); const EQ::Vec3 fp = World::P(W.BodyOf(fi)); if (f.stuck || W.combat) return; std::vector<Cat> plan;
            for (std::size_t i = 0; i < W.src.size(); ++i) { const Src& s = W.src[i]; const float d = World::D3(fp, s.p); if (!f.near[i] && d > W.R.grab) continue; if (f.near[i] && d <= W.R.grab + 150.0f && d > W.R.grab) continue;
                bool want = false; if (!s.owned && !s.locked && !s.barred && s.kind != SrcKind::Flora && !(W.town && s.kind == SrcKind::Container) && !W.FailedPeek(s, static_cast<int>(i))) { if (plan.empty()) plan = W.Plan(f, 1);
                    for (Cat c : plan) if (c != Cat::Steal && W.TierReleasedPeek(c, s) && !W.Picks(f, s.items, c, true).empty()) { want = true; break; } }
                if (d <= W.R.grab) f.near[i] = want ? 1 : 0; else { if (f.near[i] && want && f.slot.target != static_cast<int>(i)) { ++W.t.walkedPast; W.Log("[past] F%d left %u behind", fi, s.id); } f.near[i] = 0; } }
        }
        // The loop of one phase: fight while foes stand, else loot. Ends when the looting is over.
        void Run(Policy& P) {
            double fightT0 = F.now; bool inFight = AnyFoe(); if (inFight) { W.combat = true; combatStart = F.now; }
            for (;;) {
                if (inFight) {
                    if (F.now >= F.nextPump && !F.ctl.empty()) { F.nextPump += TM::pump; P.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); }
                    F.StepWorld();
                    if (F.over || !AnyFoe()) { const bool lost = F.t.lost || !F.b[0].alive || AnyFoe(); /* lost, or timed out with foes standing: no looting follows */ EndFight(fightT0); if (lost) lostFight = true; inFight = false; if (fights == 1) lootStart = F.now; phaseStart = F.now; if (stopAfterFight) return; if (lost || !sp.looting) break; if (fights == 1) lootStart = F.now; phaseStart = F.now; quietSince = -1; W.nextPump = F.now; }
                } else {
                    if (!waved && sp.waveAt >= 0 && F.now - lootStart >= sp.waveAt) { Wave(); if (AnyFoe()) { inFight = true; fightT0 = F.now; W.combat = true; F.over = false; F.nextPump = F.now; continue; } }
                    QuietStep();
                    if (F.now - phaseStart >= sp.lootSec) break;
                    if (W.ActiveSlots() > 0 || pstage == 1 || pstage == 2) quietSince = -1; else if (quietSince < 0) quietSince = F.now;
                    if (quietSince >= 0 && F.now - quietSince >= 12.0 && F.now - phaseStart >= 60.0 && (waved || sp.waveAt < 0)) break;
                }
                if (sp.looting && !W.fs.empty() && F.now >= W.nextPump) { W.nextPump += static_cast<double>(W.R.pumpSec); const double was = W.fs[Z(W.rr)].nextLogi; W.Service(W.rr); if (W.fs[Z(W.rr)].nextLogi != was) PastCheck(W.rr); W.rr = (W.rr + 1) % static_cast<int>(W.fs.size()); }
                Tallies();
            }
            Finish();
        }

        // ── the ledger at the end: every stack still lying there that the game's own scorer wants, and why it was left ──
        void Finish() {
            LTally& t = W.t; ++t.episodes; if (lostFight) ++t.lostFights; const Body& pl = F.b[0]; if (!sp.looting) { Values(); return; }
            std::vector<std::vector<Cat>> plans; for (const FState& f : W.fs) plans.push_back(W.Plan(f, 1));
            for (std::size_t i = 0; i < W.src.size(); ++i) { const Src& s = W.src[i]; const int si = static_cast<int>(i);
                for (const Stack& st : s.items) { if (st.count <= 0) continue; const ItemDef& d = Items()[Z(st.item)]; const std::vector<Stack> one{ st }; Cat via = Cat::kCount; bool carry = false;
                    for (std::size_t f = 0; f < W.fs.size() && via == Cat::kCount; ++f) for (Cat c : plans[f]) { if (c == Cat::Steal) continue; if (W.Picks(W.fs[f], one, c, true).empty()) { if (!W.Picks(W.fs[f], one, c, false).empty()) carry = true; continue; } via = c; break; }
                    if (via == Cat::kCount && !carry) { if (d.value > 0 && !s.owned) { auto& nr = t.noRoad[Logi::Name(d.kind)]; ++nr.first; nr.second += static_cast<double>(d.value) * st.count; } continue; }   // nobody's plan takes it: not wanted loot (ingredients, junk under the ratio, gear that is no upgrade). Counted apart, by kind, with its gold
                    ++t.wanted; const bool gold = d.kind == Kind::Gold, museum = d.museum; const char* why; bool rr = false;
                    bool reach = s.tri >= 0 && s.navDist <= W.R.navGate; if (reach && pl.nav.tri >= 0) { EQ::Walker w; const auto r = F.env->FindRoute(F.doors, F.doorStamp, pl.nav.tri, s.tri, w, false); reach = r.any && r.reach; }
                    const bool inRange = World::D3(World::P(pl), s.p) <= W.R.leashMax; bool anyStuck = false; for (const FState& f : W.fs) anyStuck = anyStuck || f.stuck;
                    if (lostFight) why = "the party lost the fight (no looting)";
                    else if (via == Cat::kCount) why = "carry weight";
                    else if (s.owned) why = "owned";
                    else if (s.locked) why = "locked";
                    else if (s.kind == SrcKind::Flora && !s.coin) why = "flora (no road)";
                    else if (W.town && s.kind == SrcKind::Container && via != Cat::Museum) why = "town container";
                    else if (s.barred || (!W.CorpseWaived(s) && W.GuardSeesTruth(0, si))) why = "guard";
                    else if (!reach) why = s.tri < 0 || s.navDist > W.R.navGate ? "off-navmesh" : "unreachable";
                    else if (!inRange) why = "out of range (the player left)";
                    else if (!W.TierReleasedPeek(via, s)) why = s.claim.seen < 0 ? "player dibs (never scanned)" : "player dibs";
                    else if (W.GatedBy(si) >= 0) { why = "GATED"; rr = true; }
                    else if (F.now < s.stickyUntil || F.now < s.transientUntil) { why = "blocklisted"; rr = true; }
                    else if (anyStuck) { why = "follower stuck at his origin"; rr = true; }
                    else if (W.ActiveSlots() > 0 || F.now - phaseStart >= sp.lootSec) { why = "the episode ended first"; rr = true; }
                    else if (!Clutter(via) && World::D3(World::P(pl), s.p) <= W.R.bubble) { why = "player bubble (he stands on it)"; rr = true; }
                    else if (s.island >= 2) { why = "island verdict"; rr = true; }
                    else if (F.now - s.capAt < 5.0) { why = "candidate cap (48, in ref order before the sort)"; rr = true; }
                    else { bool inScan = false; for (std::size_t f = 0; f < W.fs.size(); ++f) inScan = inScan || World::D3(World::P(W.BodyOf(static_cast<int>(f))), s.p) <= W.R.lootRadius; why = inScan ? "ignored" : "beyond every follower's scan radius"; rr = true; }
                    ++t.left[why]; if (rr) ++t.leftRR; if (gold) { ++t.goldLeft; if (rr) ++t.goldLeftRR; if (!rr && !std::strncmp(why, "player dibs", 11) && reach && World::D3(World::P(pl), s.p) <= W.R.departRadius) ++t.goldDibsNear; } if (museum) { ++t.museumLeft; if (rr) ++t.museumLeftRR; }
                    W.Log("[left] %s x%d in %s %u: %s (via %s; follower 0 at %.0f u, player at %.0f u, navDist %.0f, dispatches %d, arrivals %d)", d.edid, st.count, SrcName(s.kind), s.id, why, via == Cat::kCount ? "-" : CatName(via), World::D3(World::P(W.BodyOf(0)), s.p), World::D3(World::P(pl), s.p), s.navDist, s.dispatches, s.arrivals); } }
            Values();
        }
        void Values() {
            for (FState& f : W.fs) { if (!f.who.worth) continue; Inv a = f.inv0, b = f.inv; Logi::Town(a, f.who, false); Logi::Town(b, f.who, false);
                const double pa = Logi::PotionJ(a, f.who), pb = Logi::PotionJ(b, f.who); W.t.dPotJ += pb - pa; W.t.dGoldJ += (Logi::Value(b, f.who) - pb) - (Logi::Value(a, f.who) - pa); }
        }
        [[nodiscard]] double Score(bool withGold = true) const { return W.t.uFights + W.t.dPotJ + (withGold ? W.t.dGoldJ : 0.0); }
    };

    // ── building an episode ──
    // An ordinary one: the class's random fight (its place drawn from the pool this thread was given), then looting.
    inline Episode MakeEpisode(const Data& D, const SimClass& cls, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, std::uint64_t seed, const EpSpec& base) {
        Episode ep; ep.sp = base; ep.sp.seed = seed; Rng R; R.g.seed(seed * 2654435761ULL + 977);
        std::vector<const SimEnv*> one; if (base.env) { one.push_back(base.env); tl_envDraw = &one; }
        ep.F = RandomFight(D, cls, 1 + static_cast<int>(seed % 5), kBrackets[0], R, t1); if (base.env) tl_envDraw = nullptr;
        if (base.plan == Plan::kCount) ep.sp.plan = static_cast<Plan>(SimEnv::Mix(seed ^ 0x51ED27) % static_cast<std::uint64_t>(Plan::kCount));
        ep.Begin(all); return ep;
    }
    // A scenario's: no fight, the party at the place's entry, whatever its setup adds.
    inline Episode MakeBare(const Data& D, const SimClass& cls, const std::vector<const SimClass*>& others, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, const EpSpec& base) {
        Episode ep; ep.sp = base; FightSpec fs; fs.cls = &cls; fs.others = others; fs.level = 20; fs.rank = 3; fs.seed = base.seed; fs.env = base.env; fs.envExact = true; fs.interior = base.env ? base.env->interior : true;
        ep.F = BuildFight(D, fs, t1); ep.F.over = false; ep.Begin(all); ep.lootStart = ep.phaseStart = ep.F.now; return ep;
    }

    // ── THE LOOT ORACLE (14.5). Unlimited tries, only the roads the game has. In its world the plan is every category with a road (no score decides),
    // and every (follower, source, category) take is a decision: TAKE or LEAVE. Each decision in turn is tried both ways by replaying the SAME episode
    // from the end of its first fight (a copy: the same draws, the same second fight) with the earlier decisions fixed and the later ones TAKE, and
    // the better end value stands. So an option that makes him late to the next fight, or fills the bag before the potion, pays for it. A tie leaves.
    // The walking itself (which source next) stays the game's: native ranks by distance, never by value, and that is a rule, not a knob. ──
    struct OracleOut { double score = 0, scoreNoGold = 0; int decisions = 0, takes = 0, replays = 0; bool floored = false; LTally t; };
    inline OracleOut RunOracle(const Episode& afterFight, Policy& P, int maxDecisions = 400) {
        OracleOut o; std::vector<signed char> dec;
        auto play = [&](const std::vector<signed char>& f, int* nDec, LTally* tt, double* noGold) { Episode e = afterFight; e.sp.wide = true; e.forced = f; e.decisions = 0; e.Fix(); e.W.oracleWide = true; e.Run(P); ++o.replays; if (nDec) *nDec = e.decisions; if (tt) *tt = e.W.t; if (noGold) *noGold = e.Score(false); return e.Score(true); };
        for (int k = 0; k < maxDecisions; ++k) {
            std::vector<signed char> a = dec, b = dec; a.push_back(1); b.push_back(0); int na = 0; const double va = play(a, &na, nullptr, nullptr);
            if (na <= k) break;   // the episode never reached decision k: every decision is made
            const double vb = play(b, nullptr, nullptr, nullptr); dec.push_back(va > vb + 1e-9 ? 1 : 0); }
        o.decisions = static_cast<int>(dec.size()); for (signed char d : dec) o.takes += d; o.score = play(dec, nullptr, &o.t, &o.scoreNoGold);
        // THE FLOOR: the game's own play is one of the oracle's options (its plan is a subset of the wide one). A search that ended under it failed, and the game's play stands.
        { Episode g = afterFight; g.Run(P); if (g.Score(true) > o.score) { o.score = g.Score(true); o.scoreNoGold = g.Score(false); o.t = g.W.t; o.floored = true; } }
        return o;
    }

    // ── the gate (14.3). Each row: a per-episode or per-minute number of the tally and the most it may be. PLACEHOLDERS unless the row says CALIBRATED;
    // calib_loot.py prints the field's value of the rows a log can show. To change a bound: this table. ──
    struct Bound { const char* name; double max; const char* what; };
    inline constexpr Bound kLootBounds[] = {
        { "leftRR/ep", 0.50, "wanted stacks left though released, reachable and in range, per episode (marth: 'lots of items being ignored')" },
        { "goldLeftRR/ep", 0.10, "coin left though released, reachable and in range" },
        { "coinDibsNear/ep", 0.10, "coin left under the player's dibs while he stands within 700 units of it at the end (marth: coin on bodies left; scenario coin-behind-player)" },
        { "museumLeftRR/ep", 0.10, "museum pieces left though released, reachable and in range" },
        { "crimes", 0.0, "takes of an owned thing a witness truly saw (followers never commit detected crimes)" },
        { "guardSeenTakes", 0.0, "takes a guard truly saw of a source the guard rule covers" },
        { "legs/src", 2.0, "loot legs per source something was taken from (trips per item; CALIBRATED: the field asks for 13 to 15 legs per looted arrival, 11-17c, 11-17d, 11-18)" },
        { "restart%", 35.0, "share of legs that are a second or later leg to the same source ('stops and restarts'; CALIBRATED: field 11-17c 18, 11-17d 17, 11-17g 20, 11-18 37)" },
        { "aborted%", 30.0, "share of legs ended by anything but arrival (CALIBRATED: the field's upper bound is 83 to 90)" },
        { "reversals/min", 1.0, "a leg heading back against his last one within 10 s (back and forth), per follower-minute" },
        { "twoOnOne/ep", 0.0, "two followers walking to one source" },
        { "combatLoot s/fight", 1.5, "seconds a loot slot outlived the start of party combat (the sweep is one pump per follower away)" },
        { "combatTakes", 0.0, "takes while the party fights" },
        { "walkedPast/ep", 0.5, "he came within reach of wanted, released loot and left it" },
        { "stuckOff s/ep", 5.0, "seconds a follower's looting was off because he was 'stuck himself'" },
        { "wait free p90 s", 25.0, "release to taken, 90th percentile, what nobody has dibs on (potions, ammo, picks, museum pieces)" },
        { "wait dibs p90 s", 60.0, "release to taken, 90th percentile, what the player has first dibs on (the abandon clock alone is 45 s)" },
    };
    inline std::map<std::string, double> GateValues(const LTally& t) {
        std::map<std::string, double> v; const double ep = std::max<double>(1, static_cast<double>(t.episodes)), min = std::max(1e-9, t.followerSec / 60.0);
        v["leftRR/ep"] = t.leftRR / ep; v["goldLeftRR/ep"] = t.goldLeftRR / ep; v["coinDibsNear/ep"] = t.goldDibsNear / ep; v["museumLeftRR/ep"] = t.museumLeftRR / ep; v["crimes"] = static_cast<double>(t.crimes); v["guardSeenTakes"] = static_cast<double>(t.guardSeenTakes);
        v["legs/src"] = t.srcTaken ? double(t.legs) / t.srcTaken : 0.0; v["restart%"] = t.legs ? 100.0 * t.legsRestarted / t.legs : 0.0; v["aborted%"] = t.legs ? 100.0 * t.legsAborted / t.legs : 0.0;
        v["reversals/min"] = t.reversals / min; v["twoOnOne/ep"] = t.twoOnOne / ep; v["combatLoot s/fight"] = t.combatLootSec / std::max<double>(1, static_cast<double>(t.waves)); v["combatTakes"] = static_cast<double>(t.combatTakes);
        v["walkedPast/ep"] = t.walkedPast / ep; v["stuckOff s/ep"] = t.stuckOffSec / ep;
        auto p90 = [](std::vector<float> w) { if (w.empty()) return 0.0; std::sort(w.begin(), w.end()); return static_cast<double>(w[std::min(w.size() - 1, static_cast<std::size_t>(0.9 * static_cast<double>(w.size())))]); };
        v["wait free p90 s"] = p90(t.waits); v["wait dibs p90 s"] = p90(t.waitsDibs);
        return v;
    }
    inline int PrintGate(const LTally& t, const char* tag, bool lines = true) {
        const auto v = GateValues(t); int bad = 0;
        for (const Bound& b : kLootBounds) { const double x = v.at(b.name); const bool ok = x <= b.max + 1e-12; bad += !ok; if (lines) std::printf("LOOTGATE %-22s %-20s %9.3f  <= %-7.2f %s\n", tag, b.name, x, b.max, ok ? "ok" : "FAIL"); }
        return bad;
    }
    inline void PrintTally(const LTally& t, const char* tag) {
        const double ep = std::max<double>(1, static_cast<double>(t.episodes)), min = std::max(1e-9, t.followerSec / 60.0);
        std::printf("LOOT %-22s episodes %ld (lost fights %ld)  loot %.0f s  follower-min %.1f  stacks %ld taken %ld (%.0f%%)  wanted-left %ld (RR %ld)  gold left %ld  museum left %ld  legs %ld arrived %ld aborted %ld restarted %ld  U %.2f  dPotJ %.3f  dGoldJ %.3f\n",
            tag, t.episodes, t.lostFights, t.lootSec, min, t.stacks0, t.taken, t.stacks0 ? 100.0 * t.taken / t.stacks0 : 0.0, t.wanted, t.leftRR, t.goldLeft, t.museumLeft, t.legs, t.legsArrived, t.legsAborted, t.legsRestarted, t.uFights / ep, t.dPotJ / ep, t.dGoldJ / ep);
        for (const auto& kv : t.noRoad) std::printf("LOOTNOROAD %-22s %-12s %6ld stacks  %9.0f gold  (unowned, left, in nobody's plan)\n", tag, kv.first.c_str(), kv.second.first, kv.second.second);
        for (const auto& kv : t.left) std::printf("LOOTLEFT %-22s %-32s %6ld  %.2f/ep\n", tag, kv.first.c_str(), kv.second, kv.second / ep);
        for (const auto& kv : t.ev) std::printf("LOOTRATE %-22s %-58s %7ld  %8.3f /follower-min\n", tag, kv.first.c_str(), kv.second, kv.second / min);
    }

}
