// tools/iaus_harness -- off-line driver for the IAUS pure core (native/iaus/, card I2).
// RUN: tools/iaus_harness/run.sh   (host g++ or CXX=clang++; builds only the pure core + this file, exit code =
// number of failed checks). Not part of the CMake/CI build: the DLL never links it.
//
// Builds ONLY the pure files (Curves, Catalogue, Score, Decide, Explain, Generate) with a host compiler: no CommonLib,
// no engine, no PCH. Each scenario hand-builds the facts I4 will read in game, runs Decide, and prints the ranked
// list with every consideration ([iaus-why] format), so a curve change can be judged before a field cycle. The
// CHECKs at the end assert the properties the design promises; the exit code is the number that failed.
#include "iaus/Core.h"
#include "iaus/Estimate.h"
#include "iaus/Reach.h"      // ruling Q68: the learned swing reach
#include "iaus/Generate.h"   // card I4: the pure candidate generator + the Snapshot it reads

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace MFO::Iaus;

namespace {

    int g_failed = 0;
    void Check(bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++g_failed;
    }

    const char* Verdicts[] = { "Act", "Hold", "Idle" };

    // The first try of a decision, or a sentinel that matches no candidate (so a Check fails, never crashes).
    const Step& S0(const Decision& d) {
        static const Step kNone{ 0xFFFF, 0, 0, false };
        return d.order.empty() ? kNone : d.order[0];
    }

    Decision Show(const char* title, const std::vector<Candidate>& c, const Mind& m, const Profile& p, TickIn in,
                  bool why = true) {
        const Tuning& t = DefaultTuning();
        const Decision d = Decide(m, c, p, t, in);
        std::printf("\n=== %s ===\n", title);
        std::printf("[iaus] %s\n", FormatDecision(d, c, t, d.order.empty() ? -1 : 0, {}, 0.0).c_str());
        std::printf("  verdict=%s new=%d order:", Verdicts[static_cast<int>(d.verdict)], d.newDecision ? 1 : 0);
        for (const auto& s : d.order) std::printf(" #%u", static_cast<unsigned>(s.idx));
        std::printf("\n  ranked:\n");
        int i = 0;
        for (const auto& r : d.ranked)
            std::printf("   %2d. %.3f%s  %s\n", ++i, r.score, r.running ? " (running)" : "",
                        Label(c[r.idx]).c_str());
        if (why) {
            std::printf("  [iaus-why] every candidate:\n");
            for (const auto& cand : c)
                std::printf("   %s\n", FormatWhy(cand, m, p, t, in.rapportRank, d.bestHeal).c_str());
        }
        return d;
    }

    // ── fact builders ────────────────────────────────────────────────────────────────────────────────────
    struct Foe {
        const char* name; std::uint32_t id; float dist; float levelRatio; float hp;
        bool onMe, onPlayer, onAlly, playerTarget, current, caster, unholy;
        float resistFire, resistFrost, resistShock;
    };

    Candidate Offense(const char* spell, std::uint32_t form, RoleTag role, float cost, float reach, const Foe& foe,
                      float resist, float mp, float mpMax) {
        Candidate c;
        c.family = Family::CastOffense; c.role = role; c.form = form; c.target = foe.id;
        c.formName = spell; c.targetName = foe.name;
        auto& f = c.f;
        f.dist = foe.dist; f.reach = reach; f.sight = Sight::Visible;
        f.foeLevelRatio = foe.levelRatio; f.foeHpPct = foe.hp; f.foeCaster = foe.caster;
        f.foeOnMe = foe.onMe; f.foeOnPlayer = foe.onPlayer; f.foeOnAlly = foe.onAlly;
        f.playerTarget = foe.playerTarget; f.currentTarget = foe.current;
        f.cost = cost; f.magicka = mp; f.magickaMax = mpMax; f.selfMagicka = mp / mpMax;
        f.resist = resist;
        return c;
    }

    Candidate Attack(Family fam, const Foe& foe, float reach, bool melee) {
        Candidate c;
        c.family = fam; c.target = foe.id; c.targetName = foe.name;
        auto& f = c.f;
        f.dist = foe.dist; f.reach = reach; f.meleeRange = melee; f.sight = Sight::Visible;
        f.foeLevelRatio = foe.levelRatio; f.foeHpPct = foe.hp; f.foeCaster = foe.caster;
        f.foeOnMe = foe.onMe; f.foeOnPlayer = foe.onPlayer; f.foeOnAlly = foe.onAlly;
        f.playerTarget = foe.playerTarget; f.currentTarget = foe.current;
        f.foeStronger = foe.levelRatio > 1.0f;
        return c;
    }

    Candidate Drink(Family fam, const char* name, std::uint32_t form, int stock, float hp, float st, float mp) {
        Candidate c;
        c.family = fam; c.form = form; c.formName = name;
        c.f.stock = stock; c.f.selfHp = hp; c.f.selfStamina = st; c.f.selfMagicka = mp;
        return c;
    }

    Candidate Heal(const char* spell, std::uint32_t form, float cost, float mp, float mpMax, std::uint32_t who,
                   const char* whoName, float hp, bool isPlayer) {
        Candidate c;
        c.family = Family::CastHeal; c.role = RoleTag::Heal; c.form = form; c.formName = spell;
        c.target = who; c.targetName = whoName;
        auto& f = c.f;
        f.cost = cost; f.magicka = mp; f.magickaMax = mpMax; f.selfMagicka = mp / mpMax;
        f.recipientHp = hp; f.recipientIsPlayer = isPlayer; f.inHealReach = Tri::Yes;
        f.sight = who ? Sight::Visible : Sight::None;
        return c;
    }

    TickIn Tick(std::uint8_t rank, double now = 100.0) {
        TickIn in;
        in.now = now; in.rapportRank = rank; in.commitWindowSec = 1.5; in.seed = 0x5EED0000A2C94ull;
        return in;
    }

    // ── scenarios ────────────────────────────────────────────────────────────────────────────────────────
    void MageLowMagickaFireWeak() {
        // A T1 Mage (tree OFF), rank III, 35/200 magicka, two foes: an Ice Wraith (fire -50, frost immune) on
        // her, and a Bandit on the player. Firebolt costs 30, Ice Spike 48 (cannot afford), Flames/Frostbite 14
        // (650 reach), a Fireball scroll, two magicka potions.
        const float mp = 35.0f, mpMax = 200.0f;
        const Foe wraith{ "Ice Wraith", 0x00023ABE, 900.0f, 1.0f, 0.9f, true, false, false, false, true, false, false,
                          -50.0f, 100.0f, 0.0f };
        const Foe bandit{ "Bandit", 0x0003DF21, 600.0f, 0.8f, 0.6f, false, true, false, true, false, false, false,
                          0.0f, 0.0f, 0.0f };
        std::vector<Candidate> c;
        c.push_back(Offense("Firebolt", 0x0012FCD0, RoleTag::Fire, 30, 3000, wraith, wraith.resistFire, mp, mpMax));
        c.push_back(Offense("Firebolt", 0x0012FCD0, RoleTag::Fire, 30, 3000, bandit, bandit.resistFire, mp, mpMax));
        c.push_back(Offense("Ice Spike", 0x0002B96C, RoleTag::Frost, 48, 3000, bandit, 0.0f, mp, mpMax));
        c.push_back(Offense("Frostbite", 0x0002B96B, RoleTag::Frost, 14, 650, wraith, wraith.resistFrost, mp, mpMax));
        c.push_back(Offense("Frostbite", 0x0002B96B, RoleTag::Frost, 14, 650, bandit, 0.0f, mp, mpMax));
        c.push_back(Offense("Flames", 0x00012FCD, RoleTag::Fire, 14, 650, wraith, wraith.resistFire, mp, mpMax));
        c.push_back(Offense("Flames", 0x00012FCD, RoleTag::Fire, 14, 650, bandit, 0.0f, mp, mpMax));
        {
            Candidate s = Offense("Scroll of Fireball", 0x000A44A3, RoleTag::Fire, 0, 3000, wraith, wraith.resistFire,
                                  mp, mpMax);
            s.family = Family::Scroll; s.scrollOf = Family::CastOffense;
            s.f.cost = 0; s.f.stock = 1; s.f.hasArea = true; s.f.foesInArea = 1; s.f.splashSafe = true;
            c.push_back(s);
        }
        c.push_back(Drink(Family::DrinkMagicka, "Potion of Magicka", 0x0003EADF, 2, 0.8f, 0.9f, mp / mpMax));
        c.push_back(Attack(Family::Attack, bandit, 150.0f, true));   // dagger: out of melee reach
        Show("1. Mage low on magicka (35/200) vs a fire-weak foe, rank III", c, Mind{}, Profile{}, Tick(3));

        // The same fight with full magicka: no potion, Ice Spike affordable.
        for (auto& x : c) { x.f.magicka = 200.0f; x.f.selfMagicka = 1.0f; }
        const Decision d1 = Show("1b. Same fight at full magicka (200/200), rank I (no player focus)", c, Mind{},
                                 Profile{}, Tick(1), false);
        Check(!d1.ranked.empty() && c[d1.ranked[0].idx].form == 0x0012FCD0 && c[d1.ranked[0].idx].target == wraith.id,
              "full magicka, rank I: Firebolt on the fire-weak Ice Wraith ranks first");
        const Decision d3 = Show("1c. Same at rank III: player focus x1.2 on the Bandit attacking the player", c,
                                 Mind{}, Profile{}, Tick(3), false);
        Check(!d3.ranked.empty() && c[d3.ranked[0].idx].target == bandit.id,
              "rank III: protecting the player outweighs the weakness (focus x1.2 + on_player + player_target)");
    }

    void MeleeLowHpHeal() {
        // Lydia, T1 Warrior (tree OFF), rank IV, 22% HP, 50% stamina, 50/50 magicka; a Draugr on her.
        const Foe draugr{ "Draugr Wight", 0x0003B547, 120.0f, 1.2f, 0.7f, true, false, false, true, true, false, true,
                          0.0f, 0.0f, 0.0f };
        const float hp = 0.22f;
        const float emergency = EmergencyLevel(hp);
        auto build = [&](bool healOnClass, bool healMastered) {
            std::vector<Candidate> c;
            Candidate a = Attack(Family::Attack, draugr, 160.0f, true);
            a.f.selfHp = hp;
            c.push_back(a);
            Candidate pa = Attack(Family::PowerAttack, draugr, 160.0f, true);
            pa.f.selfHp = hp; pa.f.selfStamina = 0.5f; pa.f.hardCooldownSec = 10.0f;
            c.push_back(pa);
            c.push_back(Drink(Family::DrinkHealth, "Potion of Health", 0x0003EADE, 1, hp, 0.5f, 1.0f));
            Candidate h = Heal("Healing", 0x00012FCC, 35, 50, 50, 0, "", hp, false);
            h.f.onClass = healOnClass; h.f.fromMastered = healMastered; h.f.emergency = emergency;
            c.push_back(h);
            Candidate fl; fl.family = Family::Retreat; fl.f.selfHp = hp; fl.f.disengageClass = false;
            c.push_back(fl);
            return c;
        };
        std::printf("\n(emergency level at %.0f%% HP = %.2f)\n", hp * 100.0f, emergency);
        const auto cA = build(false, false);
        const Decision dA = Show("2a. Melee at 22% HP, Healing is OFF-class and no mage class mastered (Q34), rank IV",
                                 cA, Mind{}, Profile{}, Tick(4));
        Check(!dA.ranked.empty() && cA[dA.ranked[0].idx].family == Family::DrinkHealth,
              "unmastered off-class heal is invalid: the health potion ranks first");
        const auto cB = build(false, true);
        const Decision dB = Show("2b. Same, a mage class is MASTERED: the heal is valid, scores low, emergency lifts it",
                                 cB, Mind{}, Profile{}, Tick(4));
        Check(!dB.ranked.empty() && cB[dB.ranked[0].idx].family == Family::CastHeal,
              "mastered off-class heal in an emergency outranks the potion (and the potion is vetoed by it)");
        const auto cC = build(true, false);
        const Decision dC = Show("2c. Same, Healing ON-class (a Spellsword-like profile)", cC, Mind{}, Profile{},
                                 Tick(4), false);
        float sB = 0, sC = 0;
        for (const auto& r : dB.ranked) if (cB[r.idx].family == Family::CastHeal) sB = r.score;
        for (const auto& r : dC.ranked) if (cC[r.idx].family == Family::CastHeal) sC = r.score;
        Check(sC > sB, "on-class heal scores above the mastered off-class heal");
    }

    void NearEqualAtEachRank() {
        std::printf("\n=== 3. Near-equal candidates at each rapport rank ===\n");
        // (a) the design's own section 5.1 example, scores 0.80 / 0.72 / 0.65 / 0.55, through SharpPick over a
        // uniform grid of u.
        const float s[] = { 0.80f, 0.72f, 0.65f, 0.55f };
        const Tuning& t = DefaultTuning();
        std::printf("  5.1 table check (design: I 52/27/15/6, II 66/24/10/0, III 80/16/4, IV 98/2, V 100):\n");
        double pI0 = 0, pIV0 = 0;
        for (int r = 0; r < 5; ++r) {
            int hits[4] = {};
            const int N = 100000;
            for (int i = 0; i < N; ++i) ++hits[SharpPick(s, t.sharp[r], (static_cast<float>(i) + 0.5f) / N)];
            std::printf("   rank %-3s %5.1f%% %5.1f%% %5.1f%% %5.1f%%\n", r == 0 ? "I" : r == 1 ? "II" : r == 2 ? "III" : r == 3 ? "IV" : "V",
                        100.0 * hits[0] / N, 100.0 * hits[1] / N, 100.0 * hits[2] / N, 100.0 * hits[3] / N);
            if (r == 0) pI0 = 100.0 * hits[0] / N;
            if (r == 3) pIV0 = 100.0 * hits[0] / N;
        }
        Check(std::abs(pI0 - 52.0) < 1.0 && std::abs(pIV0 - 98.0) < 1.0, "SharpPick reproduces the design's table");

        // (b) a real near-equal fight: two Bandits a step apart, both on the player; Firebolt for either.
        const Foe b1{ "Bandit", 0x0003DF21, 700.0f, 1.0f, 0.8f, false, true, false, false, true, false, false, 0, 0, 0 };
        const Foe b2{ "Bandit Outlaw", 0x0003DF22, 760.0f, 1.0f, 0.75f, false, true, false, true, false, false, false, 0, 0, 0 };
        std::vector<Candidate> c;
        c.push_back(Offense("Firebolt", 0x0012FCD0, RoleTag::Fire, 30, 3000, b1, 0, 150, 200));
        c.push_back(Offense("Firebolt", 0x0012FCD0, RoleTag::Fire, 30, 3000, b2, 0, 150, 200));
        c.push_back(Offense("Sparks", 0x0002DD2A, RoleTag::Shock, 14, 650, b1, 0, 150, 200));
        c.push_back(Offense("Frostbite", 0x0002B96B, RoleTag::Frost, 14, 650, b2, 0, 150, 200));
        Show("3b. Near-equal fight, rank III (ranked list)", c, Mind{}, Profile{}, Tick(3), false);
        std::printf("  pick distribution over 2000 fresh decisions (epoch 1..2000):\n");
        for (std::uint8_t r = 1; r <= 5; ++r) {
            std::vector<int> hits(c.size(), 0);
            for (std::uint32_t e = 0; e < 2000; ++e) {
                Mind m; m.epoch = e;
                const Decision d = Decide(m, c, Profile{}, t, Tick(r));
                if (!d.order.empty()) ++hits[d.order[0].idx];
            }
            std::printf("   rank %u:", r);
            for (std::size_t i = 0; i < c.size(); ++i) std::printf("  %s %4.1f%%", Label(c[i]).c_str(), hits[i] / 20.0);
            std::printf("\n");
            if (r == 5) Check(*std::max_element(hits.begin(), hits.end()) == 2000, "rank V always picks the best");
        }
    }

    void PlayerFocus() {
        std::printf("\n=== 4. Player focus (Q35: 1.0/1.1/1.2/1.35/1.5, boost only) ===\n");
        std::vector<Candidate> c;
        c.push_back(Heal("Healing Hands", 0x0002F3B8, 30, 150, 200, 0x00000014, "Player", 0.5f, true));
        c.push_back(Heal("Healing Hands", 0x0002F3B8, 30, 150, 200, 0x000A2C94, "Lydia", 0.5f, false));
        const Tuning& t = DefaultTuning();
        for (std::uint8_t r = 1; r <= 5; ++r) {
            const float sp = ScoreOne(c[0], Mind{}, Profile{}, t, r, 0, nullptr);
            const float sa = ScoreOne(c[1], Mind{}, Profile{}, t, r, 0, nullptr);
            std::printf("   rank %u: heal player %.3f  heal Lydia %.3f  ratio %.2f\n", r, sp, sa, sp / sa);
            if (r == 1) Check(std::abs(sp - sa) < 1e-6f, "rank I: no player focus");
            if (r == 5) Check(std::abs(sp / sa - 1.5f) < 1e-3f, "rank V: x1.5 on the player, ally unchanged");
        }
    }

    void MomentumAndWindow() {
        std::printf("\n=== 5. Momentum and the commitment window (rank V, window 1.5 s) ===\n");
        const Tuning& t = DefaultTuning();
        Tuning noMom = t; noMom.momentum = 1.0f;
        const Foe fa{ "Bandit", 0x0003DF21, 120.0f, 1.0f, 0.8f, true, false, false, false, false, false, false, 0, 0, 0 };
        Foe fb = fa; fb.name = "Bandit Chief"; fb.id = 0x0003DF30;
        std::vector<Candidate> c{ Attack(Family::Attack, fa, 160, true), Attack(Family::Attack, fb, 160, true) };
        auto line = [&](const char* what, const Decision& d) {
            std::printf("   %-46s verdict=%s first=%-30s new=%d |", what, Verdicts[static_cast<int>(d.verdict)],
                        d.order.empty() ? "-" : Label(c[d.order[0].idx]).c_str(), d.newDecision ? 1 : 0);
            for (const auto& r : d.ranked) std::printf(" %s %.3f%s", c[r.idx].targetName.data(), r.score, r.running ? "(running)" : "");
            std::printf("\n");
        };
        Mind m;
        Decision d = Decide(m, c, Profile{}, t, Tick(5, 10.0));
        line("t=10.0 identical foes", d);
        if (d.order.empty()) { Check(false, "identical attacks clear the act floor"); return; }
        Check(S0(d).idx == 0, "an exact tie goes to the lower (family, form, target) key");
        OnFired(m, c[0], S0(d), 10.0);
        const int runRank = S0(d).rank;

        c[1].f.foeHpPct = 0.3f;   // the Chief is a little more worth finishing (~+5%)
        d = Decide(m, c, Profile{}, t, Tick(5, 10.5));
        line("t=10.5 Chief +5%, momentum x1.25", d);
        Check(d.verdict == Verdict::Act && !d.newDecision && S0(d).idx == 0, "momentum keeps the running attack");
        d = Decide(m, c, Profile{}, noMom, Tick(5, 10.5));
        line("t=10.5 Chief +5%, momentum OFF, in window", d);
        Check(d.verdict == Verdict::Hold && d.order.size() == 1 && S0(d).idx == 0,
              "in the window a gain under the +25% margin holds (suppressed)");
        d = Decide(m, c, Profile{}, noMom, Tick(5, 12.0));
        line("t=12.0 same, momentum OFF, window over", d);
        Check(d.verdict == Verdict::Act && d.newDecision && S0(d).idx == 1, "after the window the better one is picked");

        c[1].f.foeHpPct = 0.02f; c[1].f.foeOnPlayer = true; c[1].f.playerTarget = true;   // a big, urgent gain
        d = Decide(m, c, Profile{}, t, Tick(5, 10.6));
        line("t=10.6 Chief nearly dead and on the player", d);
        Check(d.newDecision && S0(d).idx == 1, "in the window a gain above momentum x margin preempts");
        Check(S0(d).rank < runRank, "the new decision outranks the running one (lower Choice.rank)");
        OnFired(m, c[1], S0(d), 10.6);
        Check(m.epoch == 2 && RankOf(m.epoch) == S0(d).rank, "OnFired adopts the new epoch");

        // Card I6, marth Q67 (closes MFO-B651): a repeat Fired of the SAME decision does not re-anchor the window.
        {
            Mind q;
            std::vector<Candidate> a{ Attack(Family::Attack, fa, 160, true), Attack(Family::Attack, fb, 160, true) };
            Decision d0 = Decide(q, a, Profile{}, noMom, Tick(5, 20.0));
            OnFired(q, a[S0(d0).idx], S0(d0), 20.0);
            const double anchor = q.firedAt;
            const std::uint32_t epoch = q.epoch;
            const std::size_t run = S0(d0).idx, other = 1 - run;
            for (double tt = 20.2; tt < 23.1; tt += 0.4) {   // the same attack keeps firing for 3 s
                const Decision dk = Decide(q, a, Profile{}, noMom, Tick(5, tt));
                if (!dk.order.empty() && dk.order[0].running) OnFired(q, a[dk.order[0].idx], dk.order[0], tt);
            }
            Check(q.firedAt == anchor && q.epoch == epoch, "Q67: repeat fires of the same decision leave the window anchor and the epoch alone");
            a[other].f.foeHpPct = 0.3f;   // a small gain (~+5%), under the preempt margin
            const Decision late = Decide(q, a, Profile{}, noMom, Tick(5, 23.2));
            Check(late.verdict == Verdict::Act && late.newDecision && S0(late).idx == other,
                  "Q67: 3 s into a repeating decision the window is over, so a small gain is picked (no Hold)");
            OnFired(q, a[S0(late).idx], S0(late), 23.2);
            Check(q.firedAt == 23.2 && q.epoch == epoch + 1, "Q67: a NEW decision firing re-anchors the window");
        }
    }

    void TiesAndPosture() {
        std::printf("\n=== 6. Ties and the posture layer ===\n");
        const Tuning& t = DefaultTuning();
        const Foe a{ "Skeever", 0x00023AB7, 300.0f, 0.5f, 1.0f, true, false, false, false, false, false, false, 0, 0, 0 };
        Foe b = a; b.name = "Skeever"; b.id = 0x00023AB8; b.current = true;   // otherwise identical
        std::vector<Candidate> c{ Attack(Family::Attack, a, 400, false), Attack(Family::Attack, b, 400, false) };
        const Decision d = Decide(Mind{}, c, Profile{}, t, Tick(5));
        std::printf("   tie: %s %.4f / %s %.4f -> first %08X\n", Label(c[0]).c_str(),
                    ScoreOne(c[0], Mind{}, Profile{}, t, 5, 0, nullptr), Label(c[1]).c_str(),
                    ScoreOne(c[1], Mind{}, Profile{}, t, 5, 0, nullptr), (S0(d).idx < c.size() ? c[S0(d).idx].target : 0u));
        Check(S0(d).idx < c.size() && c[S0(d).idx].target == b.id, "the current target outranks an otherwise identical foe");

        auto posture = [](PostureKind k, bool current, float dist, bool unholy) {
            Candidate p; p.family = Family::Posture; p.f.posture = k; p.f.postureCurrent = current;
            p.f.dist = dist; p.f.foeUnholy = unholy;
            p.role = k == PostureKind::Silver ? RoleTag::Silver : k == PostureKind::Bow ? RoleTag::Ranged : RoleTag::Melee;
            return p;
        };
        for (const bool unholy : { false, true }) {
            for (const float dist : { 150.0f, 1500.0f }) {
                std::vector<Candidate> pc{ posture(PostureKind::Melee, true, dist, unholy),
                                           posture(PostureKind::Silver, false, dist, unholy),
                                           posture(PostureKind::Bow, false, dist, unholy) };
                float sc = 0;
                const int w = DecidePosture(Mind{}, pc, Profile{}, t, 3, &sc);
                std::printf("   posture vs %s foe at %4.0f: %s (%.3f)\n", unholy ? "Unholy" : "plain ", dist,
                            w >= 0 ? Label(pc[static_cast<std::size_t>(w)]).c_str() : "none", sc);
                if (unholy && dist < 200) Check(w == 1, "close Unholy foe: silver beats the drawn mace");
                if (!unholy && dist < 200) Check(w == 0, "close plain foe: keep the drawn mace");
                if (dist > 1000) Check(w == 2, "far foe: the bow");
            }
        }
    }

    // ── Review round 1 (a0745ee6) findings, each reproduced as a check ─────────────────────────────────────
    Candidate PlainAttack(std::uint32_t id, float ratio, float distOverReach) {
        Candidate c; c.family = Family::Attack; c.target = id; c.f.reach = 150.0f; c.f.meleeRange = true;
        c.f.dist = distOverReach * 150.0f; c.f.sight = Sight::Visible; c.f.foeOnMe = true; c.f.foeLevelRatio = ratio;
        return c;
    }

    void ReviewRound1() {
        std::printf("\n=== 8. Review round 1 fixes ===\n");
        const Tuning& t = DefaultTuning();
        // F1 (SEV-2): the running candidate must never be pruned; the verdict must not depend on order.
        {
            std::vector<Candidate> v;
            for (int i = 0; i < 7; ++i)
                v.push_back(PlainAttack(100u + static_cast<std::uint32_t>(i), i == 6 ? 0.3f : 1.4f, 0.33f));
            Mind m; m.hasRunning = true; m.running = KeyOf(v[6]); m.epoch = 3; m.firedAt = 10.0;
            const TickIn in = Tick(5, 10.5);
            const Decision a = Decide(m, v, Profile{}, t, in);
            std::vector<Candidate> w(v.rbegin(), v.rend());
            const Decision b = Decide(m, w, Profile{}, t, in);
            const std::uint32_t fa = S0(a).idx < v.size() ? v[S0(a).idx].target : 0u;
            const std::uint32_t fb = S0(b).idx < w.size() ? w[S0(b).idx].target : 0u;
            std::printf("   F1 running-last: verdict=%s first=%u pruned=%zu | running-first: verdict=%s first=%u pruned=%zu\n",
                        Verdicts[static_cast<int>(a.verdict)], fa, a.pruned, Verdicts[static_cast<int>(b.verdict)], fb,
                        b.pruned);
            Check(a.verdict == b.verdict && a.verdict == Verdict::Hold && fa == 106u && fb == 106u,
                  "F1: running attack inside the window is never pruned; Hold in both candidate orders");
        }
        // F1b (finding 9): a candidate under the k-th best by less than tieEps survives the prune and can win the
        // tie on current target.
        {
            const float A = ScoreOne(PlainAttack(200, 1.0f, 0.5f), Mind{}, Profile{}, t, 5, 0, nullptr);
            float lo = 0.0f, hi = 1.0f, found = -1.0f, ratioFound = 0.0f;
            for (int it = 0; it < 60; ++it) {   // bisect a current-target foe's level ratio into (A - eps/2, A)
                const float mid = 0.5f * (lo + hi);
                Candidate c = PlainAttack(299, mid, 0.5f); c.f.currentTarget = true;
                const float sc = ScoreOne(c, Mind{}, Profile{}, t, 5, 0, nullptr);
                if (sc < A && sc > A - t.tieEps * 0.5f) { found = sc; ratioFound = mid; break; }
                if (sc >= A) hi = mid; else lo = mid;
            }
            std::vector<Candidate> v;
            for (int i = 0; i < 6; ++i) v.push_back(PlainAttack(200u + static_cast<std::uint32_t>(i), 1.0f, 0.5f));
            Candidate last = PlainAttack(299, ratioFound, 0.5f); last.f.currentTarget = true;
            v.push_back(last);
            const Decision d = Decide(Mind{}, v, Profile{}, t, Tick(5));
            std::printf("   F1b k-th %.4f, current-target foe %.4f (ratio %.3f) scored last -> first %u, pruned %zu\n", A,
                        found, ratioFound, S0(d).idx < v.size() ? v[S0(d).idx].target : 0u, d.pruned);
            Check(found > 0.0f && S0(d).idx == 6, "F1b: the tieEps prune slack keeps a tie-cluster winner scored last");
        }
        // F2 (SEV-3): a NaN / inf fact VETOES (and is counted), never drops or passes a term.
        {
            Candidate h = Heal("Healing", 9, 10, 100, 100, 8, "Ally", 0.2f, false);
            h.f.recipientHp = NAN;
            Candidate dk = Drink(Family::DrinkHealth, "Potion", 5, 2, NAN, 1, 1);
            const Foe foe{ "Foe", 1, 500, 1, 1, true, false, false, false, true, false, false, 0, 0, 0 };
            Candidate mi = Offense("Firebolt", 7, RoleTag::Fire, 10, 3000, foe, 0, 100, 100);
            mi.f.magicka = INFINITY;
            Candidate dn = PlainAttack(3, 1.0f, 0.5f);
            dn.f.dist = NAN;
            Candidate mx = Heal("Healing", 9, 1, 100, 100, 8, "Ally", 0.2f, false);
            mx.f.magicka = INFINITY; mx.f.magickaMax = INFINITY;
            const float s1 = ScoreOne(h, Mind{}, Profile{}, t, 1, 0, nullptr);
            const float s2 = ScoreOne(dk, Mind{}, Profile{}, t, 1, 0, nullptr);
            const float s3 = ScoreOne(mi, Mind{}, Profile{}, t, 1, 0, nullptr);
            const float s4 = ScoreOne(dn, Mind{}, Profile{}, t, 1, 0, nullptr);
            const float s5 = ScoreOne(mx, Mind{}, Profile{}, t, 1, 0, nullptr);
            const std::vector<Candidate> all{ h, dk, mi, dn, mx };
            const Decision d = Decide(Mind{}, all, Profile{}, t, Tick(1));
            std::printf("   F2 heal hp=NaN %.3f | drink hp=NaN %.3f | magicka=inf %.3f | dist=NaN %.3f | mp,max=inf %.3f | badFacts=%zu\n",
                        s1, s2, s3, s4, s5, d.badFacts);
            std::printf("   %s\n", FormatWhy(h, Mind{}, Profile{}, t, 1, 0).c_str());
            std::printf("   [iaus] %s\n", FormatDecision(d, all, t, -1, {}, 0.0).c_str());
            Check(s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 0 && d.badFacts == 5 && d.verdict == Verdict::Idle,
                  "F2: every NaN/inf fact vetoes its candidate and is counted (BAD-FACTS)");
        }
        // F3 (SEV-3): outside the window, low rapport is NOT the most stubborn: a normal 5.1 roll over the pool.
        {
            const Foe weak{ "Ice Wraith", 0x23ABE, 900, 1.0f, 0.9f, true, false, false, false, true, false, false, -50, 0, 0 };
            const Foe plain{ "Bandit", 0x3DF21, 500, 0.6f, 1.0f, false, false, false, false, false, false, false, 0, 0, 0 };
            const std::vector<Candidate> c{ Offense("Flames", 0x12FCD, RoleTag::Fire, 14, 650, plain, 0, 150, 200),
                                            Offense("Firebolt", 0x12FCD0, RoleTag::Fire, 30, 3000, weak, -50, 150, 200) };
            const float sf = ScoreOne(c[0], Mind{}, Profile{}, t, 1, 0, nullptr);
            const float sb = ScoreOne(c[1], Mind{}, Profile{}, t, 1, 0, nullptr);
            for (const std::uint8_t rk : { std::uint8_t{ 1 }, std::uint8_t{ 3 }, std::uint8_t{ 5 } }) {
                const int n = 2000;
                int fbw = 0;
                for (int e = 0; e < n; ++e) {
                    Mind m; m.hasRunning = true; m.running = KeyOf(c[0]); m.epoch = static_cast<std::uint32_t>(e);
                    m.firedAt = 0.0;   // long ago: outside the window
                    const Decision d = Decide(m, c, Profile{}, t, Tick(rk, 100.0));
                    if (S0(d).idx == 1) ++fbw;
                }
                const Sharpness& sh = t.sharp[rk - 1];
                const float sfm = sf * t.momentum;
                const bool inPool = sh.k >= 2 && sfm >= sh.floor * sb - 1e-6f;
                const float expected = inPool ? 100.0f / (1.0f + std::exp((sfm - sb) / sh.tau)) : 100.0f;
                const float got = 100.0f * static_cast<float>(fbw) / static_cast<float>(n);
                std::printf("   F3 rank %u: running Flames %.3f (x1.25 = %.3f) vs Firebolt on fire-weak %.3f -> Firebolt %.1f%% (5.1 predicts %.1f%%)\n",
                            rk, sf, sfm, sb, got, expected);
                Check(std::abs(got - expected) < 3.0f && got > 50.0f,
                      "F3: outside the window the pick follows the 5.1 roll (momentum in S), not a pool hold");
            }
        }
        // F4 (SEV-3): a plain melee attack on the current target clears the act floor with margin, 0.3-1.15x
        // reach, level ratio 0.3-1.0.
        {
            const float reach[] = { 0.3f, 0.5f, 0.7f, 0.9f, 1.0f, 1.15f };
            std::printf("   F4 plain attack on me, current target [not current target], by reach x level ratio:\n      reach ");
            for (const float r : reach) std::printf("     %4.2f      ", r);
            std::printf("\n");
            float worst = 9.0f;
            for (const float ratio : { 0.3f, 0.5f, 0.7f, 1.0f }) {
                std::printf("   ratio %.1f ", ratio);
                for (const float r : reach) {
                    Candidate c = PlainAttack(1, ratio, r);
                    c.f.currentTarget = true;
                    const float sc = ScoreOne(c, Mind{}, Profile{}, t, 1, 0, nullptr);
                    c.f.currentTarget = false;
                    const float sn = ScoreOne(c, Mind{}, Profile{}, t, 1, 0, nullptr);
                    worst = (std::min)(worst, sc);
                    std::printf("  %.3f [%.3f]", sc, sn);
                }
                std::printf("\n");
            }
            Check(worst >= 1.2f * t.actFloor, "F4: worst plain current-target attack >= 1.2 x act floor across the grid");
            {   // RULING Q70 (marth 2026-11-11, clarifying Q64: "I meant when it should swing the weapon, not when it
                // should choose a target"). The 1.15x reach gate is the SWING gate: it vetoes a power attack. A melee
                // ATTACK only names the target, so it is shaded by distance and vetoed only past the chase cap.
                Candidate pw = PlainAttack(1, 1.0f, 1.16f); pw.family = Family::PowerAttack; Cons by = Cons::kCount;
                Check(ScoreOne(pw, Mind{}, Profile{}, t, 1, 0, nullptr, &by) == 0.0f && by == Cons::RangeMelee,
                      "F4 (Q70): the 1.15x reach gate still vetoes a swing (power attack)");
                Check(ScoreOne(PlainAttack(1, 1.0f, 1.16f), Mind{}, Profile{}, t, 1, 0, nullptr) > 0.0f,
                      "Q70: a melee attack names a foe just out of reach (target choice is not gated by reach)");
                Candidate nearC = PlainAttack(1, 1.0f, 0.9f), midC = PlainAttack(1, 1.0f, 4.0f), farC = PlainAttack(1, 1.0f, 0.9f);
                nearC.f.chase = 4096.0f; midC.f.chase = 4096.0f; farC.f.chase = 4096.0f; farC.f.dist = 4200.0f;
                const float sNear = ScoreOne(nearC, Mind{}, Profile{}, t, 1, 0, nullptr), sMid = ScoreOne(midC, Mind{}, Profile{}, t, 1, 0, nullptr);
                Cons byFar = Cons::kCount; const float sFar = ScoreOne(farC, Mind{}, Profile{}, t, 1, 0, nullptr, &byFar);
                Check(sNear > sMid && sMid > 0.0f, "Q70: a nearer foe is the better attack, a farther one still an attack");
                Check(sFar == 0.0f && byFar == Cons::RangeChase, "Q70: a foe beyond the chase cap is vetoed");
                Check(Estimate::TauntChance(20, 20) == 0.5f && Estimate::TauntChance(30, 20) == 0.9f && Estimate::TauntChance(10, 30) == 0.1f,
                      "Q69: the taunt chance is the executor's rule (0.5 + 0.05 per level, kept in 0.1 .. 0.9)");
            }
        }
        // F5 (SEV-4): the log dedupe includes the verdict and the fired flag.
        {
            Mind m;
            const Key k{ Family::Attack, PostureKind::None, 0, 0 };
            const bool l0 = ShouldLog(m, k, Verdict::Act, true, 1.0, t);
            const bool l1 = ShouldLog(m, k, Verdict::Act, true, 2.0, t);
            const bool l2 = ShouldLog(m, k, Verdict::Hold, true, 2.1, t);
            const bool l3 = ShouldLog(m, k, Verdict::Hold, false, 2.2, t);
            const bool l4 = ShouldLog(m, Key{}, Verdict::Idle, false, 2.3, t);
            std::printf("   F5 first %d, repeat %d, Act->Hold %d, fired->none %d, ->Idle(Key{}) %d\n", l0, l1, l2, l3, l4);
            Check(l0 && !l1 && l2 && l3 && l4, "F5: a verdict or fired change logs at once on the same key");
        }
        // F6 (SEV-4): a heal SCROLL counts as a heal for Retreat / the health drink.
        {
            Candidate sc = Heal("Scroll of Healing", 0xA44A0, 0, 0, 0, 0, "", 0.2f, false);
            sc.family = Family::Scroll; sc.scrollOf = Family::CastHeal; sc.f.cost = 0; sc.f.stock = 2;
            sc.f.magickaMax = 0; sc.f.selfMagicka = 0.5f; sc.f.sight = Sight::None;
            const Candidate dk = Drink(Family::DrinkHealth, "Potion of Health", 0x3EADE, 1, 0.2f, 1, 1);
            Candidate fl; fl.family = Family::Retreat; fl.f.selfHp = 0.2f; fl.f.disengageClass = true;
            const std::vector<Candidate> v{ sc, dk, fl };
            const Decision d = Decide(Mind{}, v, Profile{}, t, Tick(5));
            std::printf("   F6 bestHeal %.3f, ranked:", d.bestHeal);
            for (const auto& r : d.ranked) std::printf(" %s %.3f", Name(v[r.idx].family).data(), r.score);
            std::printf("\n");
            Check(d.bestHeal > t.noHealDrink && d.ranked.size() == 1 && v[d.ranked[0].idx].family == Family::Scroll,
                  "F6: a heal scroll above 0.5 vetoes the health drink and the retreat");
        }
    }

    void Budget() {
        // Design 1.6's realistic mage: ~90 candidates after pruning. Time Decide over them.
        std::vector<Candidate> c;
        for (int foe = 0; foe < 5; ++foe) {
            const float ff = static_cast<float>(foe);
            const Foe f{ "Foe", 0x00100000u + static_cast<std::uint32_t>(foe), 400.0f + 150.0f * ff, 1.0f, 0.9f,
                         foe == 0, foe == 1, foe == 2, foe == 1, foe == 0, foe == 3, false,
                         -25.0f * ff, 10.0f * ff, 0.0f };
            for (int s = 0; s < 12; ++s)
                c.push_back(Offense("Spell", 0x00200000u + static_cast<std::uint32_t>(s), RoleTag::Fire,
                                    10.0f + 8.0f * static_cast<float>(s), 3000, f, f.resistFire, 150, 200));
            c.push_back(Attack(Family::Attack, f, 150, true));
            c.push_back(Attack(Family::PowerAttack, f, 150, true));
        }
        for (int h = 0; h < 10; ++h) c.push_back(Heal("Heal", 0x00300000u + h, 30, 150, 200, 0, "", 0.7f, false));
        c.push_back(Drink(Family::DrinkHealth, "Potion", 0x3EADE, 2, 0.7f, 1, 1));
        const auto t0 = std::chrono::steady_clock::now();
        const int N = 2000;
        std::size_t pr = 0, ev = 0;
        for (int i = 0; i < N; ++i) {
            const Decision d = Decide(Mind{}, c, Profile{}, DefaultTuning(), Tick(3));
            pr = d.pruned; ev = d.evaluated;
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("\n=== 7. Budget: %zu candidates, %zu evaluated, %zu pruned by P5: %.1f us per Decide (host, -O2) ===\n",
                    c.size(), ev, pr, us);
        Check(us < 500.0, "under fIausBudgetMs = 0.5 ms on the host");
    }

    // ── 8. Candidate generation (card I4): the PURE layer, from a hand-built Snapshot ───────────────────────
    // iaus/Generate.cpp is the code the game runs; the engine side only fills the Snapshot. This is also the
    // proof that the pure layer links on the host (the combat simulator card builds on exactly this).
    SpellEntry Spell(const char* name, std::uint32_t id, Prune::Nature n, float cost, float reach = 2000.0f,
                     int resistAv = -1) {
        SpellEntry e;
        e.name = name; e.form = id; e.nature = n; e.cost = cost; e.reach = reach; e.resistAv = resistAv;
        e.role = Prune::RoleOf(n, resistAv);
        return e;
    }
    Combatant Actor(const char* name, std::uint32_t id, float hp, float hpMax, float x, float y, int level = 10) {
        Combatant u;
        u.name = name; u.id = id; u.hp = hp; u.hpMax = hpMax; u.mp = 50; u.mpMax = 50; u.sp = 80; u.spMax = 100;
        u.x = x; u.y = y; u.dist = std::sqrt(x * x + y * y); u.level = level; u.sight = Sight::Visible;
        u.sightEngine = Sight::Visible;
        return u;
    }
    Snapshot MageFight() {
        Snapshot s;
        s.self = Actor("Mage", 0x14F00, 180, 200, 0, 0);
        s.self.mp = 100; s.self.mpMax = 200;
        s.playerId = 0x14;
        Combatant pc = Actor("Player", 0x14, 50, 100, 300, 0); pc.isPlayer = true;
        Combatant lydia = Actor("Lydia", 0xA2C94, 100, 100, 250, 250);
        s.allies = { pc, lydia };
        for (int i = 0; i < 7; ++i) {
            Combatant f = Actor("Draugr", 0x3B500u + static_cast<std::uint32_t>(i), 80, 100, 600.0f + 150.0f * static_cast<float>(i), 0);
            f.undead = i < 2;
            f.resists = { { Prune::kAvResistFire, i == 0 ? -50.0f : 0.0f } };
            if (i == 3) f.targetId = 0x14;
            s.foes.push_back(f);
        }
        s.foes.push_back(Actor("Unloaded", 0x3B5FF, 0, 0, 500, 0));   // maxima read 0: must be skipped
        s.currentTargetId = 0x3B500;
        s.hasBook = true;
        auto& b = s.book.entries;
        b.push_back(Spell("Firebolt", 0x12FCD, Prune::Nature::Offense, 40, 2000, Prune::kAvResistFire));
        SpellEntry ball = Spell("Fireball", 0x1C789, Prune::Nature::Offense, 150, 2000, Prune::kAvResistFire);
        ball.areaUnits = Prune::AreaUnits(15);
        b.push_back(ball);                                                         // 150 > 100 magicka: P2
        SpellEntry rune = Spell("Fire Rune", 0x5DB90, Prune::Nature::Offense, 30);
        rune.drop = Prune::Drop::Placement; b.push_back(rune);                     // never generated
        SpellEntry odd = Spell("Strange Script", 0xFE000801, Prune::Nature::Unknown, 10);
        odd.drop = Prune::Drop::UnknownNature; b.push_back(odd);                   // nature unknown
        SpellEntry turn = Spell("Turn Undead", 0x4DEE8, Prune::Nature::Control, 30);
        turn.undeadOnly = true; b.push_back(turn);
        SpellEntry heal = Spell("Healing", 0x12FCC, Prune::Nature::Heal, 20, 0);
        heal.selfDelivery = true; b.push_back(heal);
        b.push_back(Spell("Heal Other", 0x12FD2, Prune::Nature::Heal, 60, 1000));
        SpellEntry oak = Spell("Oakflesh", 0x5AD5C, Prune::Nature::Buff, 30, 0);
        oak.selfDelivery = true; oak.meleeBuff = true; oak.activeOn = { 0x14F00 }; b.push_back(oak);   // already on him
        b.push_back(Spell("Flame Atronach", 0x204C3, Prune::Nature::Summon, 80, 0));
        b.push_back(Spell("Lesser Ward", 0x13018, Prune::Nature::Ward, 10, 0));
        SpellEntry scroll = Spell("Scroll of Fireball", 0xA44C5, Prune::Nature::Offense, 0);
        scroll.source = SpellSource::Scroll; scroll.count = 2; b.push_back(scroll);   // S3: in the book, no candidate
        s.book.kit.potionsHealth = 2; s.book.kit.potionsMagicka = 1;
        s.book.kit.melee = true; s.book.kit.staff = true; s.book.kit.staffCharged = true;
        s.book.kit.bow = true;   // no arrows
        s.drawn = PostureKind::None;
        s.combatSec = 3.0f;
        return s;
    }
    std::size_t CountFam(const Generated& g, Family f) { return g.perFamily[static_cast<std::size_t>(f)]; }

    void Generation() {
        std::printf("\n=== 8. Candidate generation from a hand-built snapshot (card I4, pure layer) ===\n");
        const Tuning& t = DefaultTuning();
        ClassGate mage; mage.resolved = true; mage.spellsOnClass = true;

        const Snapshot s = MageFight();
        const Generated g = Generate(s, mage, t);
        std::printf("   foes %zu/%zu, actions %zu, postures %zu, emergency %.2f (worst party HP %.0f%%)\n", g.foesKept,
                    g.foesSeen, g.actions.size(), g.postures.size(), g.emergency, g.worstPartyHp * 100.0f);
        for (std::size_t f = 0; f < kFamilies; ++f)
            if (g.perFamily[f]) std::printf("   %-14s %u\n", std::string(Name(static_cast<Family>(f))).c_str(), g.perFamily[f]);

        Check(g.foesSeen == 7 && g.foesKept == 5, "P3: the unreadable foe is skipped and 7 foes are cut to the top 5");
        Check(CountFam(g, Family::CastOffense) == 5, "Firebolt x 5 foes; Fireball (cost 150 > 100 magicka) dropped by P2");
        Check(g.droppedCost == 1, "exactly one spell dropped as unaffordable");
        bool finite = true, hostileOnParty = false, dropped = false, healOnFoe = false, turnOk = true, scrollUsed = false;
        for (const auto& c : g.actions) {
            if (!FactsFinite(c.f)) finite = false;
            const bool isFoe = c.target >= 0x3B500 && c.target <= 0x3B5FF;
            if ((c.family == Family::CastOffense || c.family == Family::CastControl || c.family == Family::Attack) && !isFoe)
                hostileOnParty = true;
            if ((c.family == Family::CastHeal || c.family == Family::CastBuff) && isFoe) healOnFoe = true;
            if (c.form == 0x5DB90 || c.form == 0xFE000801 || c.target == 0x3B5FF) dropped = true;
            if (c.form == 0xA44C5 || c.family == Family::Scroll) scrollUsed = true;
            if (c.form == 0x4DEE8 && !(c.target == 0x3B500 || c.target == 0x3B501)) turnOk = false;
            if (c.form == 0x4DEE8 && c.f.targetCond != Tri::Yes) turnOk = false;
        }
        for (const auto& c : g.postures) if (!FactsFinite(c.f)) finite = false;
        Check(finite, "every generated candidate passes FactsFinite");
        Check(!hostileOnParty && !healOnFoe, "hostile actions only name foes; heals and buffs never name a foe");
        Check(!dropped, "never generated: the rune, the unknown-nature spell, the unreadable foe");
        Check(!scrollUsed, "a carried scroll is in the book but is no candidate (card S3)");
        Check(CountFam(g, Family::CastControl) == 2 && turnOk, "Turn Undead only on the two undead foes, target_cond = Yes");
        // Healing (self only): he is at 90%. Heal Other: himself and the player (50%); Lydia is full.
        Check(CountFam(g, Family::CastHeal) == 3, "heals: hurt recipients only, a self-delivery heal on himself only");
        Check(CountFam(g, Family::CastBuff) == 0, "Oakflesh already active on him: no buff candidate");
        Check(CountFam(g, Family::CastSummon) == 1 && CountFam(g, Family::CastWard) == 1, "one summon, one ward");
        Check(CountFam(g, Family::DrinkHealth) == 1 && CountFam(g, Family::DrinkMagicka) == 1 &&
              CountFam(g, Family::DrinkStamina) == 0, "drinks: only the stocked ones (P2)");
        Check(CountFam(g, Family::PowerAttack) == 0 && CountFam(g, Family::Taunt) == 0 &&
              CountFam(g, Family::Retreat) == 0, "no power attack / taunt / retreat without the class row");
        Check(CountFam(g, Family::Sneak) == 0 && CountFam(g, Family::Exploit) == 0, "sneak and exploit are never generated");
        bool bowNoAmmo = false, staffSeen = false, keyZero = true;
        for (const auto& c : g.postures) {
            if (c.f.posture == PostureKind::Bow && c.f.outOfAmmo) bowNoAmmo = true;
            if (c.f.posture == PostureKind::Staff && c.f.staffCharged) staffSeen = true;
            if (c.target != 0 || c.form != 0) keyZero = false;
        }
        Check(g.postures.size() == 3 && bowNoAmmo && staffSeen && keyZero,
              "postures: melee, bow (out of ammo), staff (charged); the posture key's target stays 0");

        // The scorer takes them as they are.
        TickIn in; in.now = 100.0; in.rapportRank = 5;
        const Decision d = Decide(Mind{}, g.actions, Profile{}, t, in);
        const int post = DecidePosture(Mind{}, g.postures, Profile{}, t, 5);
        std::printf("   Decide: %s, %zu ranked, bad facts %zu, first = %s | posture pick = %s\n",
                    Verdicts[static_cast<int>(d.verdict)], d.ranked.size(), d.badFacts,
                    d.order.empty() ? "-" : Label(g.actions[d.order[0].idx]).c_str(),
                    post >= 0 ? std::string(Name(g.postures[static_cast<std::size_t>(post)].f.posture)).c_str() : "-");
        Check(d.badFacts == 0 && !d.ranked.empty(), "Decide scores the generated set with no bad facts");
        Check(post < 0 || g.postures[static_cast<std::size_t>(post)].f.posture != PostureKind::Bow,
              "the posture layer never picks the bow with no ammunition");
        const auto pairs = TopSpellPairs(d, g.actions);
        bool pairsOk = pairs.size() <= 6;
        for (const auto& p : pairs) if (p.spell != 0x12FCD && p.spell != 0x4DEE8) pairsOk = false;
        Check(pairsOk && !pairs.empty(), "G10: at most 6 (spell, target) pairs, offense / control only");
        {   // D1: a refused pair stays on the ask list although the next decision does not rank it
            const SpellTarget A{ 0x100, 0x200 }, B{ 0x101, 0x200 }, C{ 0x102, 0x201 };
            std::vector<AskedPair> asked = NextAskList({}, std::vector<SpellTarget>{ A, B }, 10.0, 6.0);
            Check(asked.size() == 2 && !asked[0].refused && !asked[1].refused, "G10 ask list: a new pair starts unanswered");
            asked[0].refused = true; asked[0].lastNo = 10.5;   // Gather: A answered No, B answered Yes
            asked = NextAskList(asked, std::vector<SpellTarget>{ B }, 10.6, 6.0);   // the decision ranks only B (A is vetoed)
            Check(asked.size() == 2 && asked[0].pair == B && asked[1].pair == A && asked[1].refused,
                  "G10 ask list: a pair answered No stays on the list when the decision stops ranking it (D1)");
            asked[1].lastNo = 12.0;   // still No on the next lap: re-stamped
            asked = NextAskList(asked, std::vector<SpellTarget>{ B, C }, 12.1, 6.0);
            Check(asked.size() == 3 && asked[2].pair == A && asked[2].lastNo == 12.0, "G10 ask list: a standing refusal is carried with its stamp");
            asked = NextAskList(asked, std::vector<SpellTarget>{ A, B }, 13.0, 6.0);   // ranked again (an Unknown lap): the stamp survives
            Check(asked.size() == 2 && asked[0].pair == A && asked[0].refused && asked[0].lastNo == 12.0, "G10 ask list: a refused pair that is ranked again keeps its refusal");
            asked[0].refused = false;   // Gather: A now answers Yes
            asked = NextAskList(asked, std::vector<SpellTarget>{ B }, 13.1, 6.0);
            Check(asked.size() == 1 && asked[0].pair == B, "G10 ask list: a Yes clears the refusal");
            std::vector<AskedPair> old{ { A, true, 1.0 } };
            Check(NextAskList(old, std::vector<SpellTarget>{}, 7.5, 6.0).empty() && NextAskList(old, std::vector<SpellTarget>{}, 6.9, 6.0).size() == 1,
                  "G10 ask list: a refusal lapses once its last No is older than the keep time, not before");
            std::vector<AskedPair> many; for (std::uint32_t i = 0; i < 40; ++i) many.push_back({ { 0x300 + i, 0x200 }, true, 20.0 + 0.01 * i });
            const auto capped = NextAskList(many, std::vector<SpellTarget>{ A, B }, 21.0, 6.0, 8);
            Check(capped.size() == 8 && capped[0].pair == A && capped[1].pair == B && capped[2].pair.spell == 0x300 + 39,
                  "G10 ask list: capped, the top pairs first, then the newest refusals");
            const AskedPair nan{ A, true, NAN };
            Check(NextAskList(std::vector<AskedPair>{ nan }, std::vector<SpellTarget>{}, 5.0, 6.0).empty(), "G10 ask list: a NaN stamp is not a live refusal");
        }

        // A broken read must not become a broken fact, and an unreadable follower generates nothing.
        Snapshot bad = MageFight();
        bad.self.mp = NAN; bad.foes[0].dist = NAN; bad.foes[1].level = 0; bad.self.level = 0; bad.combatSec = NAN;
        bad.foes[2].resists = { { Prune::kAvResistFire, NAN } }; bad.allies[0].x = NAN;
        const Generated gb = Generate(bad, mage, t);
        bool finiteB = true;
        for (const auto& c : gb.actions) if (!FactsFinite(c.f)) finiteB = false;
        for (const auto& c : gb.postures) if (!FactsFinite(c.f)) finiteB = false;
        Check(finiteB && !gb.actions.empty(), "NaN pools, distances, resists and a 0 level still give finite facts");
        Check(CountFam(gb, Family::CastOffense) == 0 && CountFam(gb, Family::CastSummon) == 0,
              "an unreadable magicka pool drops every priced spell (never a guess)");
        Snapshot gone = MageFight(); gone.self.hpMax = 0;
        const Generated gg = Generate(gone, mage, t);
        Check(gg.selfUnreadable && gg.actions.empty() && gg.postures.empty(), "follower maxima of 0 (unloaded): nothing generated");
        Snapshot noBook = MageFight(); noBook.hasBook = false;
        const Generated gn = Generate(noBook, mage, t);
        Check(gn.actions.size() == 5 && gn.postures.empty(), "no fresh SpellBook: attacks only (no cast, drink or posture)");

        // Class gates (rulings Q34, G16).
        ClassGate warrior; warrior.resolved = true; warrior.spellsOnClass = false; warrior.powerAttack = true;
        warrior.powerCooldownSec = 10.0f; warrior.taunt = true; warrior.disengage = true;
        Snapshot ws = MageFight(); ws.drawn = PostureKind::Melee; ws.meleeInRightHand = true; ws.tauntEnabled = true;
        ws.tauntReach = 1024.0f;
        const Generated gw = Generate(ws, warrior, t);
        Check(CountFam(gw, Family::CastOffense) + CountFam(gw, Family::CastHeal) + CountFam(gw, Family::CastSummon) == 0 &&
              gw.droppedClass > 0, "Q34: an off-class spell with no mastered casting class is no candidate");
        Check(CountFam(gw, Family::PowerAttack) == 5 && CountFam(gw, Family::Taunt) == 1 && CountFam(gw, Family::Retreat) == 1,
              "class rows open power attack (melee drawn), taunt (the foe on the player) and retreat");
        bool cd = true;
        for (const auto& c : gw.actions) if (c.family == Family::PowerAttack && c.f.hardCooldownSec != 10.0f) cd = false;
        Check(cd, "the power attack carries the class row's 10 s hard cooldown");
        // Card I6 (sim R1): the taunt's hard cooldown, the taunted gate's input and the busy rule.
        {
            float tauntCd = -1.0f;
            for (const auto& c : gw.actions) if (c.family == Family::Taunt) tauntCd = c.f.hardCooldownSec;
            Check(tauntCd == t.rowlessTauntCooldownSec && tauntCd > 0.0f, "I6: a taunt row with no cooldown gets the tuning floor as its hard cooldown");
            ClassGate rowCd = warrior; rowCd.tauntCooldownSec = 25.0f;
            for (const auto& c : Generate(ws, rowCd, t).actions) if (c.family == Family::Taunt) tauntCd = c.f.hardCooldownSec;
            Check(tauntCd == 25.0f, "I6: a taunt row's own cooldown wins over the floor");
            ClassGate open = warrior; open.powerCooldownSec = 0.0f;   // the tree OFF (Q63): open, no row
            bool floorCd = true; int n = 0;
            for (const auto& c : Generate(ws, open, t).actions)
                if (c.family == Family::PowerAttack) { ++n; if (c.f.hardCooldownSec != t.rowlessPowerCooldownSec) floorCd = false; }
            Check(n > 0 && floorCd, "I6 / Q63: a power attack with no class row gets the tuning floor as its hard cooldown");
            Snapshot busy = ws; busy.tauntBusy = true;
            Check(CountFam(Generate(busy, warrior, t), Family::Taunt) == 0, "I6: no taunt candidate while his taunt is in flight or still holds a foe");
            Snapshot tk = ws;
            for (auto& f : tk.foes) f.taunted = true;
            const Generated gt = Generate(tk, warrior, t);
            bool vetoed = CountFam(gt, Family::Taunt) > 0;
            Mind tm;
            for (const auto& c : gt.actions)
                if (c.family == Family::Taunt) {
                    Cons by = Cons::kCount;
                    const float sc = ScoreOne(c, tm, Profile{}, t, 5, 0.0f, nullptr, &by);
                    if (!(sc == 0.0f && by == Cons::FoeTaunted)) vetoed = false;
                }
            Check(vetoed, "I6: a foe already under his taunt vetoes the taunt candidate (FoeTaunted has its input now)");
            // The hard cooldown gate: inside it the taunt is vetoed, after it it scores.
            for (auto c : gw.actions)
                if (c.family == Family::Taunt) {
                    Cons by = Cons::kCount;
                    c.f.hardCooldownLeft = 4.0f;
                    const float in = ScoreOne(c, tm, Profile{}, t, 5, 0.0f, nullptr, &by);
                    c.f.hardCooldownLeft = 0.0f;
                    const float out = ScoreOne(c, tm, Profile{}, t, 5, 0.0f, nullptr);
                    Check(in == 0.0f && by == Cons::CooldownHard && out > 0.0f, "I6: the taunt is vetoed inside its hard cooldown and scores after it");
                }
        }
        warrior.spellsFromMastered = true;
        const Generated gm = Generate(ws, warrior, t);
        bool flagged = CountFam(gm, Family::CastHeal) > 0;
        for (const auto& c : gm.actions)
            if (c.form != 0 && (c.f.onClass || !c.f.fromMastered)) flagged = false;
        Check(flagged, "Q34: with a mastered casting class the spells are candidates, marked off-class / from-mastered");
        ClassGate strict = mage; strict.strict = true; strict.weaponBow = false; strict.weaponCrossbow = false;
        bool bowBarred = false, meleeOk = false;
        for (const auto& c : Generate(s, strict, t).postures) {
            if (c.f.posture == PostureKind::Bow && !c.f.classAllowsWeapon) bowBarred = true;
            if (c.f.posture == PostureKind::Melee && c.f.classAllowsWeapon) meleeOk = true;
        }
        Check(bowBarred && meleeOk, "strict adherence: a category outside the class weapon set is marked not allowed");
        Snapshot held = MageFight(); held.handBusyLeft = true; held.book.entries[0].twoHanded = true;
        const Generated gh = Generate(held, mage, t);
        Check(CountFam(gh, Family::CastOffense) == 0 && gh.droppedHand == 1 && CountFam(gh, Family::CastHeal) == 3,
              "a heal standing on one hand drops a two-hander (P1); one-handed spells keep the other hand");
        Snapshot splash = MageFight(); splash.self.mp = 200; splash.allies[0].x = 650; splash.allies[0].y = 0;
        bool sawUnsafe = false, sawArea = false;
        for (const auto& c : Generate(splash, mage, t).actions)
            if (c.form == 0x1C789) { sawArea = c.f.hasArea && c.f.foesInArea >= 1.0f; if (!c.f.splashSafe) sawUnsafe = true; }
        Check(sawArea && sawUnsafe, "an area spell counts the foes in its area and is splash-unsafe near the player");

        // I4 review F2: the blast radius is the larger of the EFIT area and the explosion radius, and it is what
        // splash_safe and foes-in-area measure with.
        Check(Prune::BlastUnits(15.0f, 600.0f, false) == 600.0f && Prune::BlastUnits(15.0f, 0.0f, false) == Prune::AreaUnits(15.0f) &&
              Prune::BlastUnits(15.0f, NAN, false) == Prune::AreaUnits(15.0f) && Prune::BlastUnits(0.0f, 0.0f, false) == 0.0f,
              "F2: blast = max(EFIT area in units, explosion radius); a broken explosion read falls back to the area");
        // MFO-B661 (card I6): a cosmetic impact explosion gives a bolt NO area; a payload explosion does.
        Check(Prune::BlastUnits(0.0f, 300.0f, false) == 0.0f && Prune::BlastUnits(0.0f, 300.0f, true) == 300.0f &&
              Prune::BlastUnits(0.0f, NAN, true) == 0.0f && Prune::BlastUnits(15.0f, 600.0f, true) == 600.0f,
              "B661: EFIT area 0 + a cosmetic explosion = no area; a payload explosion (damage or enchantment) counts");
        {
            Snapshot b = MageFight(); b.self.mp = 200;
            b.allies[0].x = 600.0f + 100.0f; b.allies[0].y = 0;   // the player stands 100 units from the nearest foe (a tank at his target)
            for (auto& e : b.book.entries) if (e.form == 0x1C789) e.areaUnits = Prune::BlastUnits(0.0f, 300.0f, false);
            bool any = false, vetoShape = false;
            for (const auto& c : Generate(b, mage, t).actions)
                if (c.form == 0x1C789 && c.target == 0x3B500) { any = true; vetoShape = c.f.hasArea || !c.f.splashSafe; }
            Check(any && !vetoShape, "B661: a bolt with a cosmetic explosion stays castable at a foe a party member stands beside");
        }
        auto blast = [&](float units, bool* unsafe, float* inArea) {
            Snapshot b = MageFight(); b.self.mp = 200;
            b.allies[0].x = 600.0f + 400.0f; b.allies[0].y = 0;      // the player, 400 units behind the nearest foe
            for (auto& e : b.book.entries) if (e.form == 0x1C789) e.areaUnits = units;
            *unsafe = false; *inArea = 0.0f;
            for (const auto& c : Generate(b, mage, t).actions)
                if (c.form == 0x1C789 && c.target == 0x3B500) { *unsafe = !c.f.splashSafe; *inArea = c.f.foesInArea; }
        };
        bool unsafeSmall = true, unsafeBig = false; float inSmall = 0, inBig = 0;
        blast(Prune::BlastUnits(5.0f, 0.0f, false), &unsafeSmall, &inSmall);      // EFIT 5 ft alone: about 107 units
        blast(Prune::BlastUnits(5.0f, 500.0f, false), &unsafeBig, &inBig);       // with a 500-unit explosion (it may enlarge a stated area)
        std::printf("   F2 5 ft EFIT alone: splash-unsafe %d, foes in area %.0f | with a 500-unit explosion: %d, %.0f\n",
                    unsafeSmall ? 1 : 0, inSmall, unsafeBig ? 1 : 0, inBig);
        Check(!unsafeSmall && unsafeBig, "F2: an ally 400 units from the target is safe from 107 units, NOT from a 500-unit explosion");
        Check(inSmall == 1.0f && inBig == 4.0f, "F2: foes in area follows the blast radius (1 foe at 107 units, 4 at 500)");

        // I4 review F1: a heal already running on ANY road (self stream, target stream, a Harbinger claim) arrives
        // as a liveStreams note; target 0 = himself.
        Snapshot run = MageFight();
        run.liveStreams = { { 0x12FCC, 0u, Tri::Yes }, { 0x12FD2, 0x14u, Tri::Yes } };
        bool selfActive = false, playerActive = false, otherIdle = true;
        for (const auto& c : Generate(run, mage, t).actions) {
            if (c.family != Family::CastHeal) continue;
            if (c.form == 0x12FCC && c.target == 0) selfActive = c.f.alreadyActive;
            else if (c.form == 0x12FD2 && c.target == 0x14) playerActive = c.f.alreadyActive;
            else if (c.f.alreadyActive) otherIdle = false;
        }
        Check(selfActive && playerActive && otherIdle,
              "F1: a running self heal (target 0) and a running ally heal read already-active; other pairs do not");

        // The pure rules on their own.
        Prune::NatureIn n;
        n.kind = Prune::Kind::Buff; n.scriptCostliest = true;
        Check(Prune::ClassifyNature(n) == Prune::Nature::Unknown, "a Buff-kind spell whose costliest effect is Script: nature unknown");
        n.kind = Prune::Kind::Offense;
        Check(Prune::ClassifyNature(n) == Prune::Nature::Offense, "a record-hostile spell stays offense beside a Script effect");
        n = {}; n.kind = Prune::Kind::Offense; n.control = true;
        Check(Prune::ClassifyNature(n) == Prune::Nature::Control, "a control archetype is control even when flagged hostile");
        n = {}; n.kind = Prune::Kind::Offense; n.selfCloak = true;
        Check(Prune::ClassifyNature(n) == Prune::Nature::Buff, "a self cloak is a buff on himself");
        Check(Prune::RoleOf(Prune::Nature::Offense, Prune::kAvResistFrost) == RoleTag::Frost &&
              Prune::RoleOf(Prune::Nature::Offense, -1) == RoleTag::OtherDamage, "element -> role tag");
        Check(Prune::SafeRatio(1.0f, 0.0f, 0.5f) == 0.5f && Prune::SafeRatio(NAN, 2.0f, 0.5f) == 0.5f, "SafeRatio guards 0 and NaN");
        std::vector<Prune::FoeIn> fs(3);
        fs[0].id = 3; fs[0].dist = 100; fs[1].id = 2; fs[1].dist = 3000; fs[1].onPlayer = true; fs[2].id = 1; fs[2].dist = 100;
        const auto top = Prune::TopFoes(fs, 2);
        Check(top.size() == 2 && top[0] == 1 && top[1] == 2, "relevance: a foe on the player outranks a near one; ties go to the lower id");
    }

    // ── Ruling Q68: the swing reach is LEARNED per weapon from hits and misses (iaus/Reach.h) ───────────────────────
    // A toy world with a TRUE reach R the learner never sees: a swing from distance d hits when d + noise <= R, and a
    // share of swings miss for reasons that are not distance (the target stepped aside). Seven in ten swings are the
    // engine's own (anywhere from 0.5 R to 1.1 R), three in ten are gated by the learned value (up to 1.15 x Gate(), which is 1.05 x it).
    struct ReachRun { float endErr = 0, spread = 0, whiff = 0, cover = 0, minR = 0; };
    ReachRun RunReach(float R, float prior, float sigma, float stray, std::uint32_t seed, int warm, int steady) {
        Reach::Learner L; L.Reset(prior);
        std::uint32_t st = seed * 2654435761u + 12345u;
        auto u = [&]() { st = st * 1664525u + 1013904223u; return static_cast<float>(st >> 8) / 16777216.0f; };
        auto gauss = [&]() { float a = 0; for (int i = 0; i < 6; ++i) a += u(); return (a - 3.0f) * 1.41421f; };   // ~N(0,1)
        ReachRun o; o.minR = L.Value(); float lo = 1e9f, hi = 0; int own = 0, ownMiss = 0, could = 0, allowed = 0;
        for (int i = 0; i < warm + steady; ++i) {
            const bool mine = u() < 0.3f;
            const float d = mine ? (0.6f + 0.55f * u()) * L.Gate() : (0.5f + 0.6f * u()) * R;
            const bool byDist = d + sigma * R * gauss() <= R;
            const bool hit = byDist && !(u() < stray);
            if (i >= warm) {
                lo = (std::min)(lo, L.Value()); hi = (std::max)(hi, L.Value());
                if (mine) { ++own; if (!byDist) ++ownMiss; }
                const float probe = (0.5f + 0.6f * u()) * R;          // a swing chance at a random distance: would the gate let it?
                if (probe <= R * (1.0f - sigma)) { ++could; if (probe <= 1.15f * L.Gate()) ++allowed; }
            }
            L.Observe(d, hit);
            o.minR = (std::min)(o.minR, L.Value());
        }
        o.endErr = std::fabs(L.Value() - R) / R; o.spread = (hi - lo) / R;
        o.whiff = own ? static_cast<float>(ownMiss) / static_cast<float>(own) : 0.0f;
        o.cover = could ? static_cast<float>(allowed) / static_cast<float>(could) : 1.0f;
        return o;
    }
    void ReachLearning() {
        std::printf("\n== Q68: the learned swing reach ==\n");
        float worstErr = 0, worstSpread = 0, worstWhiff = 0, worstCover = 1, worstFloor = 1e9f, meanErr = 0, meanWhiff = 0; int runs = 0;
        for (const float R : { 150.0f, 240.0f, 330.0f })
            for (const float pf : { 0.6f, 1.0f, 1.6f })
                for (std::uint32_t seed = 1; seed <= 8; ++seed) {
                    const ReachRun o = RunReach(R, R * pf, 0.05f, 0.05f, seed, 80, 400);
                    worstErr = (std::max)(worstErr, o.endErr); worstSpread = (std::max)(worstSpread, o.spread);
                    worstWhiff = (std::max)(worstWhiff, o.whiff); worstCover = (std::min)(worstCover, o.cover);
                    worstFloor = (std::min)(worstFloor, o.minR / (R * pf)); meanErr += o.endErr; meanWhiff += o.whiff; ++runs;
                }
        std::printf("  %d runs (3 true reaches x start 0.6 / 1.0 / 1.6 x 8 seeds, noise 5%%, stray misses 5%%): end error mean %.1f%% worst %.1f%%, worst spread %.1f%%, whiff share mean %.1f%% worst %.1f%%, worst coverage %.1f%%, lowest value %.2f x start\n",
                    runs, meanErr / runs * 100, worstErr * 100, worstSpread * 100, meanWhiff / runs * 100, worstWhiff * 100, worstCover * 100, worstFloor);
        const ReachRun clean = RunReach(240.0f, 240.0f * 1.5f, 0.0f, 0.0f, 3, 80, 400);
        std::printf("  no noise, start 1.5 x: end error %.1f%%, spread %.1f%%, whiff %.1f%%, coverage %.1f%%\n", clean.endErr * 100, clean.spread * 100, clean.whiff * 100, clean.cover * 100);
        const ReachRun noisy = RunReach(240.0f, 240.0f, 0.10f, 0.15f, 5, 80, 400);
        std::printf("  heavy noise (10%%, stray 15%%): end error %.1f%%, spread %.1f%%, whiff %.1f%%, coverage %.1f%% (reported, not a check)\n", noisy.endErr * 100, noisy.spread * 100, noisy.whiff * 100, noisy.cover * 100);
        Check(meanErr / runs <= 0.08f && worstErr <= 0.15f, "Q68 reach: from a start 0.6x to 1.6x off it ends within 15% of the true reach in every run (mean within 8%)");
        Check(meanWhiff / runs <= 0.10f && worstWhiff <= 0.15f, "Q68 reach: gated swings that miss by distance stay under 15% in every run (mean under 10%)");
        Check(worstCover >= 0.95f, "Q68 reach: the gate still lets through at least 95% of the swings that would have hit");
        Check(worstFloor >= Reach::kFloor - 1e-4f, "Q68 reach: the value never goes under its floor");
        Check(clean.endErr <= 0.02f && clean.spread <= 0.05f, "Q68 reach: without noise it settles on the true reach and stays (no oscillation)");
        {
            Reach::Learner L;
            L.Observe(100.0f, true);
            Check(!L.Known() && L.Value() == 0.0f, "Q68 reach: a learner that was never given a start value answers unknown and ignores outcomes");
            L.Reset(200.0f);
            L.Observe(NAN, false); L.Observe(-5.0f, false);
            Check(L.Value() == 200.0f && L.misses == 0, "Q68 reach: a non-finite or negative distance moves nothing");
            for (const float d : { 150.0f, 170.0f, 160.0f, 180.0f }) L.Observe(d, true);
            Check(L.Value() == 200.0f, "Q68 reach: hits inside the reach leave it where it is");
            L.Observe(60.0f, false);
            Check(L.Value() == 200.0f, "Q68 reach: one miss far inside (the target stepped aside) is outvoted by the hits beyond it");
            L.Observe(190.0f, false);
            Check(L.Value() < 200.0f && L.Value() >= 180.0f, "Q68 reach: a miss just inside the reach lowers it toward that distance, by one step at most");
            const float low = L.Value();
            L.Observe(260.0f, false);
            Check(L.Value() == low, "Q68 reach: a miss beyond the reach changes nothing (it was not expected to hit)");
            L.Observe(low * 1.04f, true);
            Check(L.Value() > low, "Q68 reach: a hit beyond the reach raises it");
            Check(std::fabs(L.Gate() - L.Value() * Reach::kGate) < 1e-4f && L.Gate() * 1.15f <= L.Value() * 1.05f + 1e-3f, "Q68 reach: the gate's reach lets him try 5% past the learned value, not 15%");
        }
    }
}

// marth Q69 / Q72 / Q73: the one taunt formula, the 30 s taunt floor, and the follower-wide cooldown identity of
// power attack and taunt (the Cooldowns store itself is engine-side; the host sees the identity the Engine maps).
void SharedTauntPowerRules() {
    Check(Estimate::TauntChance(10, 10) == 0.5f, "Q72: equal levels taunt at 0.50");
    Check(std::fabs(Estimate::TauntChance(20, 10) - 0.9f) < 1e-6f && Estimate::TauntChance(50, 1) == 0.9f, "Q72: a higher taunter is capped at 0.90");
    Check(std::fabs(Estimate::TauntChance(10, 14) - 0.3f) < 1e-6f && Estimate::TauntChance(1, 60) == 0.1f, "Q72: a stronger foe lowers it, floored at 0.10");
    Check(DefaultTuning().rowlessTauntCooldownSec == 30.0f, "Q69: the rowless taunt floor is 30 s");
    Check(DefaultTuning().rowlessPowerCooldownSec == 10.0f, "Q73: the power attack floor keeps its 10 s");
    // Shared identity: every power-attack candidate (any posture, any foe) and every taunt candidate map to ONE slot each,
    // distinct from each other; no other family is remapped (a cooldown per row stacks power attacks, this blocks it).
    Check(SharedCooldownId(Family::PowerAttack) != 0 && SharedCooldownId(Family::Taunt) != 0 &&
              SharedCooldownId(Family::PowerAttack) != SharedCooldownId(Family::Taunt),
          "Q69/Q73: power attack and taunt each own one follower-wide cooldown slot");
    Check(SharedCooldownId(Family::Attack) == 0 && SharedCooldownId(Family::Retreat) == 0, "Q73: other families keep their own cooldown id");
    Candidate a, b; a.family = b.family = Family::PowerAttack; a.f.posture = PostureKind::Melee; b.f.posture = PostureKind::Silver; b.form = 7;
    Check(CooldownId(KeyOf(a)) != CooldownId(KeyOf(b)) && SharedCooldownId(a.family) == SharedCooldownId(b.family),
          "Q73: two power-attack candidates that differ per (posture, form) share the one follower slot");
}

namespace {
    // marth Q93 keep-current-attack hold, review round 2: a melee swing reads the ENGINE ray, so its hold is bounded by the
    // engine ray's occlusion age, never by the own ray's. Diverges the two rays (own Visible, engine Occluded).
    void OcclusionHoldBound() {
        std::printf("\n== Q93: the keep-current-attack hold is bounded on the ray the swing reads ==\n");
        const Tuning& t = DefaultTuning();
        ClassGate warrior; warrior.resolved = true; warrior.powerAttack = true;
        Snapshot s;
        s.self = Actor("Warrior", 0x14F01, 180, 200, 0, 0);
        s.playerId = 0x14;
        s.foes = { Actor("Bandit", 0x3B610, 100, 100, 150, 0) };
        s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
        s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
        s.foes[0].sight = Sight::Visible; s.foes[0].occludedSec = 0.0f;   // the own ray is clear: its age is 0
        s.foes[0].sightEngine = Sight::Occluded;
        const auto scoreOf = [&](Family fam, float engineAge, float* age = nullptr) {
            s.foes[0].occludedSecEngine = engineAge;
            const Generated g = Generate(s, warrior, t);
            for (const Candidate& c : g.actions)
                if (c.family == fam && c.target == 0x3B610) {
                    if (age) *age = c.f.occludedSec;
                    return ScoreOne(c, Mind{}, Profile{}, t, 1, 0, nullptr);
                }
            return -1.0f;
        };
        for (const Family fam : { Family::Attack, Family::PowerAttack }) {
            float ageRead = -1.0f;
            const float held    = scoreOf(fam, 1.0f, &ageRead);
            const float expired = scoreOf(fam, t.occlusionHoldSec + 1.0f);
            const float untrack = scoreOf(fam, 0.0f);
            std::printf("  %s: held %.4f expired %.4f untracked %.4f (age read %.2f)\n", std::string(Name(fam)).c_str(),
                        held, expired, untrack, ageRead);
            Check(ageRead == 1.0f, fam == Family::Attack ? "Q93: the melee attack reads the engine ray's occlusion age"
                                                         : "Q93: the power attack reads the engine ray's occlusion age");
            Check(held > expired && expired >= 0.0f, fam == Family::Attack ? "Q93: the attack's hold ends once the engine ray is occluded past the hold"
                                                                           : "Q93: the power attack's hold ends once the engine ray is occluded past the hold");
            Check(untrack == expired, fam == Family::Attack ? "Q93: an untracked age (0) takes no hold for the attack"
                                                            : "Q93: an untracked age (0) takes no hold for the power attack");
        }
    }

    // MFO-B684 (the engine-fill card): the best-damage denominator applies the candidate's own filters, and an
    // UNMEASURED spell is "not known", never "does nothing".
    void EngineFillRules() {
        std::printf("\n== B684: measured flags and the best-damage denominator ==\n");
        const Tuning& t = DefaultTuning();
        ClassGate mage; mage.resolved = true; mage.spellsOnClass = true;
        const auto measured = [](SpellEntry e, float dmg) { e.damage = dmg; e.measured = true; return e; };
        const auto build = [&](bool a_handBusy) {
            Snapshot s;
            s.self = Actor("Mage", 0x14F00, 180, 200, 0, 0); s.self.mp = 400; s.self.mpMax = 400;
            s.playerId = 0x14;
            Combatant bandit = Actor("Bandit", 0x3B600, 100, 100, 600, 0);
            Combatant draugr = Actor("Draugr", 0x3B601, 100, 100, 0, 600); draugr.undead = true;
            s.foes = { bandit, draugr };
            s.currentTargetId = bandit.id; s.hasBook = true; s.combatSec = 3.0f; s.handBusyLeft = a_handBusy;
            auto& b = s.book.entries;
            b.push_back(measured(Spell("Firebolt", 0x12FCD, Prune::Nature::Offense, 40), 25.0f));
            SpellEntry sun = measured(Spell("Sun Fire", 0x2003B, Prune::Nature::Offense, 40), 80.0f); sun.undeadOnly = true; b.push_back(sun);
            b.push_back(measured(Spell("Noise", 0x2003C, Prune::Nature::Offense, 10), 0.0f));
            SpellEntry mystery = Spell("Mystery", 0x2003D, Prune::Nature::Offense, 10); mystery.damage = 500.0f; b.push_back(mystery);   // NOT measured
            b.push_back(measured(Spell("Short Bolt", 0x2003E, Prune::Nature::Offense, 40, 300.0f), 60.0f));   // foes at 600 > 1.15 x 300
            SpellEntry big = measured(Spell("Two-Hand Blast", 0x2003F, Prune::Nature::Offense, 40), 100.0f); big.twoHanded = true; b.push_back(big);
            return s;
        };
        const auto find = [](const Generated& g, std::uint32_t form, std::uint32_t target) -> const Candidate* {
            for (const Candidate& c : g.actions)
                if (c.form == form && c.target == target) return &c;
            return nullptr;
        };
        {
            const Snapshot s = build(true);   // a heal stands on one hand: the two-hander is no candidate
            const Generated g = Generate(s, mage, t);
            const Candidate* boltLiving = find(g, 0x12FCD, 0x3B600);
            const Candidate* boltUndead = find(g, 0x12FCD, 0x3B601);
            const Candidate* noise = find(g, 0x2003C, 0x3B600);
            const Candidate* mystery = find(g, 0x2003D, 0x3B600);
            Check(boltLiving && boltLiving->f.bestDamage == 25.0f,
                  "B684: on a living foe the best is Firebolt (undead-only, out-of-reach, held-hand and unmeasured spells do not count)");
            Check(boltUndead && boltUndead->f.bestDamage == 80.0f, "B684: on the undead foe the undead-only spell is the best");
            Check(noise && noise->f.expDamage == 0.0f && mystery && mystery->f.expDamage < 0.0f,
                  "B684: a measured 0 is 0, an unmeasured spell is 'not known' (< 0)");
            if (noise && mystery && boltLiving) {
                const float sNoise = ScoreOne(*noise, Mind{}, Profile{}, t, 1, 0, nullptr);
                const float sMystery = ScoreOne(*mystery, Mind{}, Profile{}, t, 1, 0, nullptr);
                Check(sMystery > sNoise && sNoise > 0.0f, "B684: the unmeasured spell is not scored down, the no-damage spell is (and is not vetoed)");
            }
        }
        {
            const Snapshot s = build(false);   // both hands free: the two-hander counts
            const Generated g = Generate(s, mage, t);
            const Candidate* bolt = find(g, 0x12FCD, 0x3B600);
            Check(bolt && bolt->f.bestDamage == 100.0f, "B684: with both hands free the two-handed spell is his best");
        }
        {
            Snapshot s = build(false);
            for (auto& e : s.book.entries) e.measured = false;   // the fill has not run (today's game)
            const Generated g = Generate(s, mage, t);
            bool allOff = true;
            for (const Candidate& c : g.actions)
                if (c.family == Family::CastOffense && (c.f.bestDamage != 0.0f || c.f.expDamage >= 0.0f)) allOff = false;
            Check(allOff, "B684: with nothing measured no damage fact is set (the considerations read 'off')");
        }
        {
            // Review F5 (rulings Q68 / Q70): the learned swing reach gates the swings he times, nothing else.
            ClassGate warrior; warrior.resolved = true; warrior.powerAttack = true;
            Snapshot s;
            s.self = Actor("Warrior", 0x14F01, 180, 200, 0, 0);
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, 190, 0) };
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
            const auto reachOf = [&](Family f) {
                const Generated g = Generate(s, warrior, t);
                for (const Candidate& c : g.actions)
                    if (c.family == f) return c.f.reach;
                return -1.0f;
            };
            const bool unknownOk = reachOf(Family::PowerAttack) == 200.0f;
            s.swingReach = 150.0f;
            Check(unknownOk && reachOf(Family::PowerAttack) == 150.0f,
                  "F5 / B687: the power attack reads the learned swing reach, and fMeleeReach while nothing is learned");
            Check(reachOf(Family::Attack) == 200.0f && Estimate::StrikeDistance(s) == 400.0f,
                  "F5: the attack and the strike distance keep fMeleeReach whatever was learned");
        }
    }
}

int main() {
    std::printf("IAUS core harness (card I2). Curves and weights = DefaultTuning() (native/iaus/Catalogue.cpp).\n");
    MageLowMagickaFireWeak();
    MeleeLowHpHeal();
    NearEqualAtEachRank();
    PlayerFocus();
    MomentumAndWindow();
    TiesAndPosture();
    ReviewRound1();
    Budget();
    Generation();
    SharedTauntPowerRules();
    ReachLearning();
    EngineFillRules();
    OcclusionHoldBound();
    std::printf("\n%d check(s) failed\n", g_failed);
    return g_failed;
}
