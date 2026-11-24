#pragma once
// iaus_sim/LootCli.h -- SIM V4 BATCH L: the runs (iaus-sim-v4.md 14.7). HOST-ONLY.
//   --loot-report N     N episodes per class and seed in the run's places: the tally, the events under the field log's names, the gate
//   --loot-scenario X   the field cases as scenarios (all | a name): each under today's rules and under the rule it was fixed from
//   --loot-test         the known-answer checks (the scenarios' verdicts): LOOT-TESTS PASS|FAIL
//   --loot-oracle N     the loot oracle against the game on N episodes per class
//   --loot-score        with --loot-report: SEED lines (mean episode value per seed) for the tune driver's loot stage
//   --loot-cost N       CPU per episode with looting on and off, same fights
#include "iaus_sim/LootLab.h"

#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>

namespace Sim::Loot {

    struct Opts { int report = 0, oracle = 0, cost = 0, trace = -1, plan = -1, guards = 1; bool test = false, score = false; std::string scenario; double wave = -2.0, lootSec = 90.0; };

    // Where the episodes are played: the run's library places, else the loot fixtures.
    inline std::vector<const SimEnv*> Places() {
        std::vector<const SimEnv*> v; for (const SimEnv* e : g_envPool.draw) if (!e->fixture) v.push_back(e);
        if (v.empty()) for (const auto& e : g_envPool.all) if (e->fixture && !e->loot.empty() && e->name != "loot-shop") v.push_back(e.get());
        return v;
    }
    inline std::vector<const SimClass*> Scope(const std::vector<SimClass>& all, const std::string& only) {
        std::vector<const SimClass*> v; for (const SimClass& c : all) if (only.empty() ? v.size() < 3 : ("," + only + ",").find("," + c.name + ",") != std::string::npos) v.push_back(&c); return v;
    }
    inline EpSpec SpecFor(const Opts& o, std::uint64_t h, const SimEnv* env) {
        EpSpec sp; sp.env = env; sp.plan = o.plan < 0 ? Plan::kCount : static_cast<Plan>(o.plan); sp.lootSec = o.lootSec; sp.libGuards = o.guards;
        sp.waveAt = o.wave > -1.5 ? o.wave : ((h >> 9) & 1) ? 6.0 + static_cast<double>((h >> 12) % 15) : -1.0; return sp;
    }

    inline int RunReport(const Data& D, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, const std::string& only, const std::vector<std::uint64_t>& seeds, int threads, const Opts& o) {
        const auto places = Places(); const auto scope = Scope(all, only); if (places.empty() || scope.empty()) { std::fprintf(stderr, "--loot-report: no place with loot (--world v4, or --env-set) or no class\n"); return 2; }
        (void)Logi::PotionFloor();   // computed once, before the threads
        const int n = o.report, jobs = static_cast<int>(scope.size() * seeds.size()) * n; std::vector<LTally> per(scope.size()); std::vector<double> seedSum(seeds.size(), 0.0); std::atomic<int> next{ 0 }; std::mutex mx;
        const auto t0 = std::chrono::steady_clock::now();
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int k = job % n, si = (job / n) % static_cast<int>(seeds.size()), ci = job / n / static_cast<int>(seeds.size());
                const std::uint64_t h = SimEnv::Mix(seeds[Z(si)] * 1000003ULL + static_cast<std::uint64_t>(ci) * 7919 + static_cast<std::uint64_t>(k) * 104729);
                Episode ep = MakeEpisode(D, *scope[Z(ci)], all, t1, h, SpecFor(o, h, places[h % places.size()]));
                if (o.trace == job) ep.W.trace = stdout;
                IausPolicy P; ep.Run(P);
                std::scoped_lock lk(mx); per[Z(ci)].Merge(ep.W.t); seedSum[Z(si)] += ep.Score(true); } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (o.score) { for (std::size_t s = 0; s < seeds.size(); ++s) std::printf("SEED %llu  J=%.4f (scenarios 0.00, natural 0.00)\nSWITCH %llu 0 1.000\n", static_cast<unsigned long long>(seeds[s]), seedSum[s] / (n * static_cast<double>(scope.size())), static_cast<unsigned long long>(seeds[s])); return 0; }
        LTally allT; int bad = 0; for (std::size_t c = 0; c < scope.size(); ++c) { PrintTally(per[c], scope[c]->name.c_str()); bad += PrintGate(per[c], scope[c]->name.c_str()); allT.Merge(per[c]); }
        PrintTally(allT, "ALL"); const int badAll = PrintGate(allT, "ALL");
        std::printf("LOOT-REPORT %d episodes, %zu places, %zu classes: value/ep with gold %.3f, without %.3f (fights %.3f + potions %.3f + gold %.3f + museum %.3f, crimes and guard-seen takes charged apart); gate rows failing: ALL %d, per class %d; wall %.2f s\n", jobs, places.size(), scope.size(),
            (allT.uFights + allT.dPotJ + allT.dGoldJ + allT.dMuseumJ) / std::max(1L, allT.episodes), (allT.uFights + allT.dPotJ) / std::max(1L, allT.episodes), allT.uFights / std::max(1L, allT.episodes), allT.dPotJ / std::max(1L, allT.episodes), allT.dGoldJ / std::max(1L, allT.episodes), allT.dMuseumJ / std::max(1L, allT.episodes), badAll, bad, wall);
        return 0;
    }

    // The loot oracle against the game, on the same episodes (the same first fight, by copy).
    inline int RunOracleReport(const Data& D, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, const std::string& only, const std::vector<std::uint64_t>& seeds, int threads, const Opts& o) {
        const auto places = Places(); const auto scope = Scope(all, only); if (places.empty() || scope.empty()) return 2;
        (void)Logi::PotionFloor();
        struct Row { double game = 0, gameNG = 0, orc = 0, orcNG = 0, cpu = 0; long n = 0, dec = 0, takes = 0, replays = 0, worse = 0; LTally tg, to; }; std::vector<Row> rows(scope.size());
        const int n = o.oracle, jobs = static_cast<int>(scope.size() * seeds.size()) * n; std::atomic<int> next{ 0 }; std::mutex mx;
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int k = job % n, si = (job / n) % static_cast<int>(seeds.size()), ci = job / n / static_cast<int>(seeds.size());
                const std::uint64_t h = SimEnv::Mix(seeds[Z(si)] * 1000003ULL + static_cast<std::uint64_t>(ci) * 7919 + static_cast<std::uint64_t>(k) * 104729);
                Episode ep = MakeEpisode(D, *scope[Z(ci)], all, t1, h, SpecFor(o, h, places[h % places.size()])); IausPolicy P; ep.stopAfterFight = true; ep.Run(P); ep.stopAfterFight = false;
                Episode g = ep; g.Run(P); timespec a{}, b{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a); const OracleOut r = RunOracle(ep, P); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
                std::scoped_lock lk(mx); Row& R = rows[Z(ci)]; ++R.n; R.game += g.Score(true); R.gameNG += g.Score(false); R.orc += r.score; R.orcNG += r.scoreNoGold; R.dec += r.decisions; R.takes += r.takes; R.replays += r.replays; R.worse += r.floored;
                R.cpu += static_cast<double>(b.tv_sec - a.tv_sec) + 1e-9 * static_cast<double>(b.tv_nsec - a.tv_nsec); R.tg.Merge(g.W.t); R.to.Merge(r.t); } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        std::printf("LOOT ORACLE, a LOWER BOUND on the best play (greedy over the take decisions in order; %d episodes a class; value = fights + potion worth + GoldJ x gold; a tie leaves)\n%-12s %9s %9s %8s | %9s %9s %8s | %6s %6s %8s %7s\n", n * static_cast<int>(seeds.size()), "class", "game", "oracle", "gap", "game-ng", "orc-ng", "gap-ng", "dec/ep", "take%", "cpu s/ep", "floored");
        for (std::size_t c = 0; c < scope.size(); ++c) { const Row& R = rows[c]; const double m = std::max<double>(1, static_cast<double>(R.n));
            std::printf("ORCLOOT %-12s %9.3f %9.3f %8.3f | %9.3f %9.3f %8.3f | %6.1f %6.1f %8.2f %7ld\n", scope[c]->name.c_str(), R.game / m, R.orc / m, (R.orc - R.game) / m, R.gameNG / m, R.orcNG / m, (R.orcNG - R.gameNG) / m, R.dec / m, R.dec ? 100.0 * R.takes / R.dec : 0.0, R.cpu / m, R.worse);
            std::printf("ORCLOOT-TAKEN %-12s game %ld of %ld stacks, oracle %ld; game left-RR %ld, oracle %ld; coin taken game %.0f, oracle %.0f\n", scope[c]->name.c_str(), R.tg.taken, R.tg.stacks0, R.to.taken, R.tg.leftRR, R.to.leftRR, R.tg.goldTaken, R.to.goldTaken); }
        return 0;
    }

    // CPU: the same fights with looting off and on (one thread, thread CPU time).
    inline int RunCost(const Data& D, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, const std::string& only, std::uint64_t seed, const Opts& o) {
        const auto places = Places(); const auto scope = Scope(all, only); if (places.empty() || scope.empty()) return 2; double cpu[2] = { 0, 0 }, sim[2] = { 0, 0 }; long nEp = 0;
        for (int k = 0; k < o.cost; ++k) for (const SimClass* sc : scope) { const std::uint64_t h = SimEnv::Mix(seed * 1000003ULL + static_cast<std::uint64_t>(k) * 104729); ++nEp;
            for (int on = 0; on < 2; ++on) { EpSpec sp = SpecFor(o, h, places[h % places.size()]); sp.looting = on; sp.waveAt = -1; timespec a{}, b{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
                Episode ep = MakeEpisode(D, *sc, all, t1, h, sp); IausPolicy P; ep.Run(P); clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b); cpu[on] += static_cast<double>(b.tv_sec - a.tv_sec) + 1e-9 * static_cast<double>(b.tv_nsec - a.tv_nsec); sim[on] += ep.F.now; } }
        std::printf("LOOT-COST %ld episodes: fight only %.2f ms CPU (%.1f simulated s), fight + looting %.2f ms CPU (%.1f simulated s): x%.2f\n", nEp, 1000.0 * cpu[0] / nEp, sim[0] / nEp, 1000.0 * cpu[1] / nEp, sim[1] / nEp, cpu[0] > 0 ? cpu[1] / cpu[0] : 0.0);
        return 0;
    }

    // ── THE FIELD CASES AS SCENARIOS (14.2). Each is played under today's rules and under a variant (the rule it was fixed from, or the same rules with
    // one thing changed), and says what must hold. `finding`: the scenario shows a fault of TODAY's rules (a native finding): it is printed and pinned as what the
    // mirror reproduces, never counted as a lab failure. To add one: a row of Scenarios(). ──
    struct Ctx {
        const Data& D; const std::vector<SimClass>& all; const std::vector<SimClass>& t1; std::FILE* trace = nullptr;
        [[nodiscard]] const SimClass& C(const char* n) const { for (const SimClass& c : all) if (c.name == n) return c; return t1[0]; }
        [[nodiscard]] const SimEnv* Env(const char* n) const { return g_envPool.ByName(n); }
        LTally Bare(const char* env, const char* cls, std::initializer_list<const char*> others, EpSpec sp, const Rules& r, const MFO::Iaus::LootTuning& T, bool traceIt = false) const {
            std::vector<const SimClass*> o; for (const char* n : others) o.push_back(&C(n)); sp.env = Env(env); sp.rules = &r; sp.tuning = &T; Episode ep = MakeBare(D, C(cls), o, all, t1, sp); if (traceIt) ep.W.trace = trace; IausPolicy P; ep.Run(P); return ep.W.t; }
        LTally Fought(const char* env, const char* cls, std::uint64_t seed, EpSpec sp, const Rules& r, const MFO::Iaus::LootTuning& T, bool traceIt = false) const {
            sp.env = Env(env); sp.rules = &r; sp.tuning = &T; Episode ep = MakeEpisode(D, C(cls), all, t1, seed, sp); if (traceIt) ep.W.trace = trace; IausPolicy P; ep.Run(P); return ep.W.t; }
    };
    inline long EvN(const LTally& t, const char* k) { const auto it = t.ev.find(k); return it == t.ev.end() ? 0 : it->second; }
    inline long LeftN(const LTally& t, const char* k) { const auto it = t.left.find(k); return it == t.left.end() ? 0 : it->second; }
    struct Scn { const char* name; const char* field; const char* variant; bool finding; std::function<std::pair<LTally, LTally>(const Ctx&, bool)> run; std::function<std::string(const LTally&, const LTally&)> check; };
    inline std::vector<Scn> Scenarios() {
        using LT = MFO::Iaus::LootTuning; std::vector<Scn> v;
        auto giveCoin = [](Episode& e) { for (Src& s : e.W.src) if (s.kind == SrcKind::Corpse) { bool has = false; for (const Stack& st : s.items) has = has || Items()[Z(st.item)].kind == Kind::Gold; if (!has) { s.items.push_back({ Logi::ItemIdx("Gold001"), 15 }); ++e.W.t.stacks0; } } };
        v.push_back({ "room-basic", "the plain case: a potion, arrows, a museum piece, a chest and coin in one room, the player standing far from them", "the museum road off (bLootLOTD 0, native's default)", false,
            [](const Ctx& c, bool tr) { EpSpec sp; Rules r, off; off.lotd = 0; const LT T; return std::pair{ c.Bare("loot-room", "ranged", { "melee" }, sp, r, T, tr), c.Bare("loot-room", "ranged", { "melee" }, sp, off, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.leftRR != 0) return "a wanted, released, reachable stack was left"; if (t.taken < 5) return "fewer than 5 stacks taken"; if (EvN(t, "take junk") < 1) return "the museum piece was not taken";
                if (EvN(o, "take junk") != 0) return "the museum piece was taken with the museum road off"; if (t.goldLeft != 0) return "coin left"; if (t.crimes || t.guardSeenTakes) return "a crime"; return ""; } });
        v.push_back({ "coin-behind-player", "marth: coin left on bodies. The player stands among three bodies and never turns to them", "the player walks on after 6 s", true,
            [giveCoin](const Ctx& c, bool tr) { EpSpec sp; sp.plan = Plan::FaceAway; sp.setup = giveCoin; EpSpec w = sp; w.plan = Plan::WalkOn; Rules r; const LT T; return std::pair{ c.Bare("loot-backs", "melee", { "ranged" }, sp, r, T, tr), c.Bare("loot-backs", "melee", { "ranged" }, w, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.goldLeft == 0) return "the coin WAS taken behind the standing player (the mirror no longer shows the fault)"; if (LeftN(t, "player dibs") + LeftN(t, "player dibs (never scanned)") == 0) return "the coin was left for another reason than the player's dibs"; (void)o; return ""; } });
        v.push_back({ "ruling-near-is-enough", "marth's ruling of 11-24 for N1: coin dibs \"Near is enough\" (6 s near, no facing). The same three bodies behind a standing player", "today's native (near AND facing): --loot-rule dibsNear=0", false,
            [giveCoin](const Ctx& c, bool tr) { EpSpec sp; sp.plan = Plan::FaceAway; sp.setup = giveCoin; Rules r, today; r.dibsNearEnough = 1; const LT T; return std::pair{ c.Bare("loot-backs", "melee", { "ranged" }, sp, r, T, tr), c.Bare("loot-backs", "melee", { "ranged" }, sp, today, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.goldLeft != 0) return "coin left under the ruling"; if (t.dibsNear != 0) return "a stack left under his dibs with him standing there, under the ruling"; if (o.goldLeft == 0) return "today's rule left no coin (nothing to fix)"; return ""; } });
        v.push_back({ "ruling-gate-events-only", "marth's ruling of 11-24: a gate lifts on real events only for now (gateReadmitMoveU 0). The locked door, the player opens it at 30 s", "today's shipped 500 (the recorder walking off lifts his record)", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.lootSec = 120; sp.setup = [](Episode& e) { e.doorOpenAt = 30.0; }; Rules r; LT T0; T0.gateReadmitMoveU = 0; const LT T; return std::pair{ c.Bare("loot-gate", "melee", {}, sp, r, T0, tr), c.Bare("loot-gate", "melee", {}, sp, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (EvN(t, "GATED re-admitted: its follower left the block") != 0) return "a record was lifted by walking off with gateReadmitMoveU 0"; if (EvN(t, "GATED re-admitted: door / lever event near it") < 1) return "the door event did not lift the gate"; if (t.taken < 3 || t.leftRR) return "the loot behind the opened door was not taken under the ruling";
                if (t.legs > o.legs) return "events-only made MORE legs to the locked door than the shipped 500"; return ""; } });
        v.push_back({ "freeze-1117g", "field 11-17g: a follower blocked at one point (no door, no lever) gated his target and a cone past it, and outdoor loot froze", "the rule before 9a16a278 (--loot-rule gate=old1117)", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.lootSec = 120; Rules r, old; old.gateOld1117 = 1; LT T, To; To.gateReadmitMoveU = 0; To.gateNearU = 0; return std::pair{ c.Bare("loot-freeze", "melee", {}, sp, r, T, tr), c.Bare("loot-freeze", "melee", {}, sp, old, To) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.leftRR != 0) return "today's rule left released, reachable loot"; if (EvN(t, "GATED") != 0) return "today's rule gated a block with no door or lever";
                if (o.leftRR < 2) return "the old rule did not freeze the loot past the block"; if (LeftN(o, "GATED") < 2) return "the old rule's leftovers are not GATED"; if (PrintGate(o, "", false) == 0) return "the old rule passes the gate"; return ""; } });
        v.push_back({ "locked-door", "a locked door with loot behind it: GATED is a fact, the player opening the door lifts it", "the door stays locked", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.lootSec = 120; EpSpec open = sp; open.setup = [](Episode& e) { e.doorOpenAt = 30.0; }; Rules r; const LT T; return std::pair{ c.Bare("loot-gate", "melee", {}, open, r, T, tr), c.Bare("loot-gate", "melee", {}, sp, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (EvN(t, "GATED") + EvN(t, "GATED (target only)") < 1) return "no GATED record at the locked door"; if (t.taken < 3) return "the loot behind the opened door was not taken"; if (t.leftRR != 0) return "released, reachable loot left after the door opened";
                if (o.taken != 0) return "something was taken through a locked door"; return ""; } });
        v.push_back({ "locked-door-cycle", "the same locked door, never opened: what he does for two minutes", "with LootTuning::gateRepeatU 0 (no stuck evidence at a real gate)", true,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.lootSec = 120; Rules r; const LT T; LT T0; T0.gateRepeatU = 0; return std::pair{ c.Bare("loot-gate", "melee", {}, sp, r, T, tr), c.Bare("loot-gate", "melee", {}, sp, r, T0) }; },
            [](const LTally& t, const LTally& o) -> std::string { (void)o; if (t.legs < 4) return "he no longer walks to the locked door again and again (the mirror no longer shows the fault)"; if (t.stuckEpisodes < 1) return "no 'stuck himself' episode at a real gate"; return ""; } });
        v.push_back({ "guard-bar-1117", "field 11-17 L1 (Raven Rock): bodies a guard can see, three followers cycling to them, 19 BARRED arrivals", "no party-wide bar (--loot-rule corpseBar=0)", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.lootSec = 120; sp.setup = [](Episode& e) { e.W.hostileDeathKnown = true; }; Rules r, old; old.corpseBar = 0; const LT T; return std::pair{ c.Bare("loot-town", "melee", { "ranged", "mage" }, sp, r, T, tr), c.Bare("loot-town", "melee", { "ranged", "mage" }, sp, old, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.crimes || t.guardSeenTakes) return "a take a guard saw"; if (o.crimes || o.guardSeenTakes) return "a take a guard saw (no bar)"; const long a = EvN(t, "arrived: guard sees it (BARRED)"), b = EvN(o, "arrived: guard sees it (BARRED)");
                if (a > 6) return "more than two BARRED arrivals a body with the bar"; if (b <= a) return "without the bar the followers do not cycle to the barred bodies"; return ""; } });
        v.push_back({ "pending-1117c", "field 11-17c F1: a guard answer not yet back dropped every candidate where no guard was", "guard timers shorter than a loot tick (guardFreshSec 0.5, guardUnreadMaxSec 0.5), no keep-warm", false,
            [](const Ctx& c, bool tr) { EpSpec sp; Rules r, old; old.guardWarmSec = 0; const LT T; LT To; To.guardFreshSec = 0.5f; To.guardUnreadMaxSec = 0.5f; return std::pair{ c.Bare("loot-room", "ranged", { "melee" }, sp, r, T, tr), c.Bare("loot-room", "ranged", { "melee" }, sp, old, To) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.taken < 5) return "today's timers lose loot in a room with no guard"; if (o.taken >= t.taken) return "the short timers lost nothing"; if (EvN(o, "scan drop: guard (pending)") < 5) return "the short timers did not drop candidates as guard (pending)"; return ""; } });
        v.push_back({ "restart-1117c", "marth 11-17c: 'stops and restarts more than the old system'. The player loots the bodies too, so their coin frees one body at a time", "LootTuning::batchHoldMinSec 0 (release at once)", false,
            [giveCoin](const Ctx& c, bool tr) { EpSpec sp; sp.plan = Plan::WalkOn; sp.standSec = 4; sp.setup = giveCoin; Rules r; const LT T; LT T0; T0.batchHoldMinSec = 0; return std::pair{ c.Bare("loot-backs", "melee", {}, sp, r, T, tr), c.Bare("loot-backs", "melee", {}, sp, r, T0) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.goldLeftRR != 0) return "coin left"; if (EvN(t, "DISPATCH") > EvN(o, "DISPATCH")) return "the batch hold made MORE excursions than no hold"; return ""; } });
        v.push_back({ "party-combat", "field 11-18: 12 'RELEASE why=party combat'. A second group arrives 6 s into the looting", "no second group", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.waveAt = 6; EpSpec q = sp; q.waveAt = -1; Rules r; const LT T; LTally a, b; for (std::uint64_t s = 1; s <= 6; ++s) { a.Merge(c.Fought("loot-room", "melee", s, sp, r, T, tr && s == 1)); b.Merge(c.Fought("loot-room", "melee", s, q, r, T)); } return std::pair{ a, b }; },
            [](const LTally& t, const LTally& o) -> std::string { (void)o; if (t.waves < 3) return "the second group did not arrive"; if (t.combatTakes) return "a take while the party fought"; if (t.combatLootSec / std::max(1L, t.waves) > 1.5) return "a loot slot outlived the start of the fight by over 1.5 s";
                if (t.crimes) return "a crime"; return ""; } });
        v.push_back({ "two-followers", "several followers sharing the room: nobody walks to a source somebody else walks to", "one follower", false,
            [](const Ctx& c, bool tr) { EpSpec sp; Rules r; const LT T; return std::pair{ c.Bare("loot-room", "ranged", { "melee", "mage" }, sp, r, T, tr), c.Bare("loot-room", "ranged", {}, sp, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.twoOnOne) return "two followers walked to one source"; if (t.leftRR) return "loot left with three followers"; if (o.leftRR) return "loot left with one follower"; return ""; } });
        v.push_back({ "thief", "a Thief steals an owned potion he needs, only unseen (followers never commit detected crimes)", "a Knight in the same shop", false,
            [](const Ctx& c, bool tr) { EpSpec sp; sp.setup = [](Episode& e) { for (FState& f : e.W.fs) { f.inv.s.erase(std::remove_if(f.inv.s.begin(), f.inv.s.end(), [](const Stack& s) { return Items()[Z(s.item)].kind == Kind::PotionH; }), f.inv.s.end()); } }; Rules r; const LT T;
                return std::pair{ c.Bare("loot-shop", "thief", {}, sp, r, T, tr), c.Bare("loot-shop", "knight", {}, sp, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.crimes || o.crimes) return "a crime a witness saw"; if (EvN(t, "steal") != 1) return "the Thief did not steal exactly the one unseen potion"; if (EvN(o, "steal") != 0) return "a class with no steal road stole"; return ""; } });
        v.push_back({ "thief-no-recheck", "followers never commit detected crimes: a guard walks in and out of sight of an owned potion a Thief needs. Twelve starts of the guard's walk", "the witness check NOT run again at the write (--loot-rule stealRecheck=0): must commit a crime", false,
            [](const Ctx& c, bool tr) { Rules r, off; off.stealRecheck = 0; const LT T; LTally a, b; for (int k = 0; k < 12; ++k) { EpSpec sp; sp.lootSec = 60; sp.setup = [k](Episode& e) { for (Guard& g : e.W.guards) { g.t = static_cast<float>(k) / 12.0f; g.p = { g.a.x + (g.b.x - g.a.x) * g.t, g.a.y + (g.b.y - g.a.y) * g.t, g.a.z }; }
                        for (FState& f : e.W.fs) f.inv.s.erase(std::remove_if(f.inv.s.begin(), f.inv.s.end(), [](const Stack& s) { return Items()[Z(s.item)].kind == Kind::PotionH; }), f.inv.s.end()); };
                    a.Merge(c.Bare("loot-watch", "thief", {}, sp, r, T, tr && k == 0)); b.Merge(c.Bare("loot-watch", "thief", {}, sp, off, T)); } return std::pair{ a, b }; },
            [](const LTally& t, const LTally& o) -> std::string { if (t.crimes) return "a crime with the write re-check on"; if (EvN(t, "steal") < 1) return "the Thief never stole while the guard looked away"; if (o.crimes < 1) return "with the re-check off no crime was counted: the crimes row cannot fail"; return ""; } });
        v.push_back({ "carry-full", "a follower at his carry limit and a body with potions he wants: only the equipment peek weighs (LootTake.cpp), so he walks, takes nothing, and comes back", "the same with room in the bag", true,
            [](const Ctx& c, bool tr) { EpSpec full; full.lootSec = 100; full.setup = [](Episode& e) { for (Src& s : e.W.src) if (s.kind == SrcKind::Corpse) { s.items.clear(); s.items.push_back({ Logi::ItemIdx("RestoreHealth03"), 2 }); } for (FState& f : e.W.fs) f.inv.s.erase(std::remove_if(f.inv.s.begin(), f.inv.s.end(), [](const Stack& s) { return Items()[Z(s.item)].kind == Kind::PotionH; }), f.inv.s.end()); e.W.R.carryCap = e.W.fs[0].inv.Weight() + 0.4f; };
                EpSpec room = full; room.setup = [](Episode& e) { for (Src& s : e.W.src) if (s.kind == SrcKind::Corpse) { s.items.clear(); s.items.push_back({ Logi::ItemIdx("RestoreHealth03"), 2 }); } }; Rules r; const LT T;
                return std::pair{ c.Bare("loot-backs", "melee", {}, full, r, T, tr), c.Bare("loot-backs", "melee", {}, room, r, T) }; },
            [](const LTally& t, const LTally& o) -> std::string { if (o.taken < 2) return "with room he did not take the potions"; if (t.legs + EvN(t, "arrived -- nothing to take") < 2) return "the full follower no longer walks to loot he cannot carry (the mirror no longer shows the fault)"; return ""; } });
        return v;
    }
    inline int RunScenarios(const Data& D, const std::vector<SimClass>& all, const std::vector<SimClass>& t1, const std::string& which, bool test, bool trace) {
        Ctx c{ D, all, t1, stdout }; int bad = 0, ran = 0, findings = 0; const auto sc = Scenarios();
        for (const Scn& s : sc) { if (which != "all" && which != s.name) continue; ++ran; const auto [today, var] = s.run(c, trace); const std::string why = s.check(today, var);
            if (s.finding) { ++findings; std::printf("LOOT-SCENARIO %-20s %s  (a fault of TODAY's rules, reproduced)\n    case: %s\n    variant: %s\n", s.name, why.empty() ? "REPRODUCED" : "NOT REPRODUCED", s.field, s.variant); if (!why.empty()) { std::printf("    %s\n", why.c_str()); ++bad; } }
            else { std::printf("LOOT-SCENARIO %-20s %s\n    case: %s\n    variant: %s\n", s.name, why.empty() ? "PASS" : "FAIL", s.field, s.variant); if (!why.empty()) { std::printf("    %s\n", why.c_str()); ++bad; } }
            if (!test) { PrintTally(today, "today"); PrintGate(today, "today"); PrintTally(var, "variant"); PrintGate(var, "variant"); }
            else std::printf("    today: taken %ld of %ld, left-RR %ld, coin left %ld, legs %ld (restarted %ld), gate rows failing %d | variant: taken %ld, left-RR %ld, coin left %ld, legs %ld, gate rows failing %d\n", today.taken, today.stacks0, today.leftRR, today.goldLeft, today.legs, today.legsRestarted, PrintGate(today, "", false), var.taken, var.leftRR, var.goldLeft, var.legs, PrintGate(var, "", false)); }
        if (!ran) { std::fprintf(stderr, "unknown loot scenario '%s'. Known:", which.c_str()); for (const Scn& s : sc) std::fprintf(stderr, " %s", s.name); std::fprintf(stderr, "\n"); return 2; }
        if (test) std::printf("LOOT-TESTS %s: %d scenarios, %d failed (%d of them reproduce a fault of today's rules)\n", bad ? "FAIL" : "PASS", ran, bad, findings);
        return bad ? 1 : 0;
    }

}
