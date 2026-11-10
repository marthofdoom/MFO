// tools/iaus_harness -- off-line driver for the IAUS pure core (native/iaus/, card I2).
// RUN: tools/iaus_harness/run.sh   (host g++ or CXX=clang++; builds only the pure core + this file, exit code =
// number of failed checks). Not part of the CMake/CI build: the DLL never links it.
//
// Builds ONLY the pure core files (Curves, Catalogue, Score, Decide, Explain) with a host compiler: no CommonLib,
// no engine, no PCH. Each scenario hand-builds the facts I4 will read in game, runs Decide, and prints the ranked
// list with every consideration ([iaus-why] format), so a curve change can be judged before a field cycle. The
// CHECKs at the end assert the properties the design promises; the exit code is the number that failed.
#include "iaus/Core.h"

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
}

int main() {
    std::printf("IAUS core harness (card I2). Curves and weights = DefaultTuning() (native/iaus/Catalogue.cpp).\n");
    MageLowMagickaFireWeak();
    MeleeLowHpHeal();
    NearEqualAtEachRank();
    PlayerFocus();
    MomentumAndWindow();
    TiesAndPosture();
    Budget();
    std::printf("\n%d check(s) failed\n", g_failed);
    return g_failed;
}
