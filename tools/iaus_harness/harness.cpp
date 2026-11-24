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
#include "iaus/ReachBook.h"  // marth 2026-11-17: the per-weapon / per-sequence reach book and its one reader
#include "iaus/SpellWorth.h"  // S11b: the spell-worth reader
#include "iaus/Generate.h"   // card I4: the pure candidate generator + the Snapshot it reads
#include "iaus/Disengage.h"  // card G3: the pure questions the disengage gate asks
#include "iaus/Reposition.h" // batch I: spell range, back-off, hidden step
#include "iaus/ClassCurves.h" // per-class curve rows (ruling Q-class-curves)
#include "cast/SeatRows.h"   // ABI v46 adoption: the seated-row rule, the need of a target, the zero-effect reading rules
#include "iaus/LootNeed.h"    // IAUS looting: category + consumable scores (ClickUp 1dfcuabd18f)

#include <limits>
#include <optional>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int MoveRows(const char* a_golden, bool a_write);   // move_rows.cpp: the movement pass (S1)

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
                {   // N6 (ClickUp 1dfcuabd8fw): a chase-waived (held) foe past the cap is named by act.attack, at the cap's edge; a non-held one stays vetoed.
                    Candidate held = farC; held.f.foeChaseWaived = true;
                    Candidate heldFar = farC; heldFar.f.foeChaseWaived = true; heldFar.f.dist = 13000.0f;
                    Candidate edge = farC; edge.f.dist = 4096.0f;
                    const float sHeld = ScoreOne(held, Mind{}, Profile{}, t, 1, 0, nullptr), sHeldFar = ScoreOne(heldFar, Mind{}, Profile{}, t, 1, 0, nullptr);
                    Check(sHeld > 0.0f && sHeldFar > 0.0f, "N6: a chase-waived foe past the cap gets an act.attack score (no cap veto)");
                    // A dragon-like foe (twice his level, on the player, the player's target) past the cap must clear the act floor on BOTH
                    // roads: melee (RangeChase) and a drawn bow / staff (RangeSpell, whose own curve is ~0.18 at the cap).
                    for (int rng = 0; rng < 2; ++rng) {
                        Candidate dr = PlainAttack(1, 2.0f, 0.9f);
                        dr.f.foeOnMe = false; dr.f.foeOnPlayer = true; dr.f.playerTarget = true; dr.f.foeChaseWaived = true; dr.f.dist = 6000.0f;
                        if (rng) { dr.f.meleeRange = false; dr.f.chase = 0.0f; dr.f.reach = 4096.0f; } else dr.f.chase = 4096.0f;
                        const float sd = ScoreOne(dr, Mind{}, Profile{}, t, 1, 0, nullptr);
                        std::printf("   N6 waived dragon %s = %.3f (floor %.2f)\n", rng ? "ranged" : "melee", sd, t.actFloor);
                        Check(sd >= t.actFloor, rng ? "N6: a waived dragon past the cap clears the act floor (ranged act.attack)" : "N6: a waived dragon past the cap clears the act floor (melee act.attack)");
                        dr.f.foeChaseWaived = false; Cons bd = Cons::kCount;
                        Check(ScoreOne(dr, Mind{}, Profile{}, t, 1, 0, nullptr, &bd) == 0.0f, "N6: the same dragon not waived is vetoed past the cap");
                    }
                    Check(sHeld == sHeldFar && sHeld == ScoreOne(edge, Mind{}, Profile{}, t, 1, 0, nullptr), "N6: a waived foe scores at the cap's edge, whatever its distance");
                    Check(sHeld < sMid, "N6: a nearer foe still outranks a waived one");
                    Cons by2 = Cons::kCount;
                    Check(ScoreOne(farC, Mind{}, Profile{}, t, 1, 0, nullptr, &by2) == 0.0f && by2 == Cons::RangeChase, "N6: a non-waived foe past the cap is still vetoed");
                    Check(ChaseWaived(7, 7, 0, true) && ChaseWaived(7, 0, 7, true) && !ChaseWaived(7, 7, 0, false) && !ChaseWaived(7, 8, 9, true) && !ChaseWaived(0, 0, 0, true),
                          "N6: the waiver rule (held or entry foe, fighting him or the player)");
                }
                {   // 11-17 A3 + field: the one weapon-range reader, and the reachable-foe reader (Reposition.h)
                    Snapshot bs; bs.drawn = PostureKind::Bow; bs.self.z = 0.0f; bs.meleeReach = 200.0f;
                    Check(Reposition::InWeaponRange(bs, 2500.0f, t.bowWeaponRange) && !Reposition::InWeaponRange(bs, t.bowWeaponRange + 1.0f, t.bowWeaponRange),
                          "A3: a foe inside the bow's range (past the chase cap) is in weapon range, one past it is not");
                    bs.drawn = PostureKind::Melee;
                    Check(!Reposition::InWeaponRange(bs, 500.0f, t.bowWeaponRange), "A3: a blade has no weapon range past the chase cap");
                    Combatant up; up.z = 900.0f; up.sight = Sight::Occluded;
                    Check(Reposition::MeleeUnreachableNow(bs, up), "field: a foe on another level behind a wall is unreachable for a blade");
                    up.z = 350.0f; Check(Reposition::MeleeUnreachableNow(bs, up), "field: one storey (350 u) behind a wall counts (the h + reach line, not StrikeDistance)");
                    up.z = 900.0f;
                    up.sight = Sight::Visible;
                    Check(Reposition::MeleeUnreachableNow(bs, up), "field: a VISIBLE foe above the vertical reach line (a tower top) is unreachable");
                    up.z = 100.0f;
                    Check(!Reposition::MeleeUnreachableNow(bs, up), "field: the same visible foe at reach height is reachable");
                    up.z = 900.0f;
                    bs.drawn = PostureKind::Bow; up.sight = Sight::Occluded;
                    Check(!Reposition::MeleeUnreachableNow(bs, up), "field: the reachability read is for a blade only");
                }
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
        {   // 1dfcuabd309: the PROXIED version of a Self heal (a proxy road serves it at an ally: allyRoad) is a candidate at
            // the hurt player, keyed to the SOURCE form; Lydia is full, so still no candidate on her.
            Snapshot sp = s;
            for (auto& e : sp.book.entries) if (e.form == 0x12FCC) e.allyRoad = true;
            const Generated gp = Generate(sp, mage, t);
            bool atPlayer = false, atLydia = false, dyn = false;
            for (const auto& c : gp.actions) {
                if (c.family != Family::CastHeal) continue;
                if (c.form == 0x12FCC && c.target == 0x14) atPlayer = true;
                if (c.form == 0x12FCC && c.target == 0xA2C94) atLydia = true;
                if ((c.form >> 24) == 0xFFu) dyn = true;
            }
            Check(CountFam(gp, Family::CastHeal) == 4 && atPlayer && !atLydia && !dyn,
                  "a Self heal a proxy road serves (allyRoad) is offered at the hurt player, keyed to the source spell");
        }
        Check(CountFam(g, Family::CastBuff) == 0, "Oakflesh already active on him: no buff candidate");
        {   // 1dfcuabd309 review F2: a Self buff a proxy road serves (allyRoad) is offered at the player and Lydia until the
            // gather marks a live stream there (Candidates.cpp pushes the ally into activeOn): then it is "up" and not offered.
            // (The stream read itself is engine state the harness cannot express; this pins Generate's consumption of it.)
            Snapshot sb = s;
            for (auto& e : sb.book.entries) if (e.form == 0x5AD5C) { e.allyRoad = true; e.activeOn = { 0x14F00 }; }
            const Generated gb = Generate(sb, mage, t);
            for (auto& e : sb.book.entries) if (e.form == 0x5AD5C) e.activeOn.push_back(0x14);
            const Generated gu = Generate(sb, mage, t);
            auto buffAt = [](const Generated& gg, std::uint32_t tgt) {
                for (const auto& c : gg.actions) if (c.family == Family::CastBuff && c.form == 0x5AD5C && c.target == tgt) return true;
                return false;
            };
            Check(buffAt(gb, 0x14) && !buffAt(gu, 0x14) && buffAt(gu, 0xA2C94),
                  "an ally buff with a live proxy stream (activeOn at the player) is not re-offered there; others still are");
        }
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
    // ── marth 2026-11-17: the reach is keyed per (follower, weapon, attack SEQUENCE), learned to the foe's EDGE, and read by ONE reader
    // with two uses (Use::Blow: one blow, no lunge, the height line; Use::Combo: the sequence he would start, the approach) ──
    void ReachSequences() {
        std::printf("\n== 11-17: per-sequence edge reach (iaus/ReachBook.h) ==\n");
        using ReachBook::Use;
        constexpr std::uint32_t F = 0x14, W = 0x1000, W2 = 0x2000;
        constexpr float kSeed = 200.0f - ReachBook::kRefRadius;   // the weapon learner's start: fMeleeReach 200 less a humanoid radius
        {   // fallback chain: nothing learned -> 0 (the readers answer fMeleeReach); weapon only -> the weapon's value
            ReachBook::Book b;
            Check(b.BlowEdge(F, W) == 0.0f && b.ComboEdge(F, W) == 0.0f, "reach book: nothing learned answers 0 (the readers fall back to fMeleeReach)");
            b.Ensure(F, W, kSeed);
            Check(b.BlowEdge(F, W) == kSeed && b.ComboEdge(F, W) == kSeed, "reach book: a weapon with no outcome reads its seed (fMeleeReach less a radius) for both uses");
            Check(ReachBook::CentreReach(kSeed, kSeed, Use::Blow, ReachBook::kRefRadius, 200.0f) == 200.0f &&
                  ReachBook::CentreReach(0.0f, 0.0f, Use::Blow, 80.0f, 200.0f) == 200.0f,
                  "reach book: the seed on a humanoid is exactly the flat fMeleeReach, and nothing learned answers it for any foe");
        }
        {   // a 3-swing combo that carries him forward learns a longer reach than a single swing; Blow stays the single-swing value
            ReachBook::Book b;
            b.Ensure(F, W, kSeed);
            for (int i = 0; i < 40; ++i) {
                b.Observe(F, W, kSeed, 3, 208.0f, true);    // the combo carries him: hits from 208 to the edge
                b.Observe(F, W, kSeed, 1, 150.0f, true);    // the single swing hits from 150 ...
                b.Observe(F, W, kSeed, 1, 190.0f, false);   // ... and whiffs from 190
            }
            const auto* w = b.Find(F, W);
            const float s3 = w->seq.at(ReachBook::SeqKey(3)).Value(), s1 = w->seq.at(ReachBook::SeqKey(1)).Value();
            std::printf("  3-swing combo %.0f, 1-swing %.0f, weapon %.0f, blow %.0f combo %.0f\n", s3, s1, w->weapon.Value(), b.BlowEdge(F, W), b.ComboEdge(F, W));
            Check(w->seq.size() == 2, "reach book: one learner per swing count (no light / power key)");
            Check(s3 > s1, "reach book: a combo that carries him forward learns a longer reach than a single swing");
            Check(b.ComboEdge(F, W) >= s3 && b.BlowEdge(F, W) == w->weapon.Value(), "reach book: Combo is the MAX over the sequences, Blow is the weapon's own value");
            Check(b.BlowEdge(F, W) < s3, "reach book: the plain per-weapon value is not raised by the lunge of a combo");
            Check(b.BlowEdge(F, W2) == 0.0f && b.BlowEdge(0x15, W) == 0.0f, "reach book: another weapon / another follower is a separate entry");
        }
        {   // a sequence is SEEDED from the weapon's value at its first outcome
            ReachBook::Book b;
            for (int i = 0; i < 20; ++i) b.Observe(F, W, kSeed, 1, 235.0f, true);   // the weapon learner climbs
            const float wv = b.Find(F, W)->weapon.Value();
            b.Observe(F, W, kSeed, 2, 150.0f, true);                                // a NEW sequence
            const float sv = b.Find(F, W)->seq.at(ReachBook::SeqKey(2)).start;
            Check(wv > kSeed && std::fabs(sv - wv) < 0.01f * wv + 1.0f, "reach book: a new sequence starts from the weapon's learned value, not from the flat seed");
        }
        {   // an uncountable chain (steps 0) feeds the weapon only; a long chain feeds its sequence only
            ReachBook::Book b;
            b.Observe(F, W, kSeed, 0, 150.0f, true);
            Check(b.Find(F, W)->seq.empty() && b.Find(F, W)->weapon.hits == 1, "reach book: an uncountable chain is the weapon's outcome only");
            b.Observe(F, W, kSeed, 4, 150.0f, true);
            Check(b.Find(F, W)->seq.size() == 1 && b.Find(F, W)->weapon.hits == 1, "reach book: a 4-swing chain feeds its own sequence, not the plain weapon value");
        }
        {   // EDGE REACH: a giant series does not inflate the reach against a human (the reviewer's sim S2, fixed)
            ReachBook::Book b; b.Ensure(F, W, kSeed);
            const float giantR = 150.0f;
            for (int i = 0; i < 40; ++i) {   // a giant whose centre is 300 - 420 away: its EDGE is 150 - 270 away; the blow lands to 200 of edge
                const float centre = 300.0f + 3.0f * static_cast<float>(i), edge = centre - giantR;
                b.Observe(F, W, kSeed, 1 + static_cast<unsigned>(i % 3), edge, edge <= 178.0f);
            }
            const float afterGiants = b.ComboEdge(F, W);
            const float vsHuman = ReachBook::CentreReach(b.BlowEdge(F, W), b.ComboEdge(F, W), Use::Blow, ReachBook::kRefRadius, 200.0f);
            std::printf("  after 40 giant chains: blow edge %.0f combo edge %.0f -> reach vs a human %.0f (flat 200)\n", b.BlowEdge(F, W), afterGiants, vsHuman);
            Check(vsHuman <= 215.0f, "edge reach: a long series of giants (centre 300+ away) does not inflate the reach against a human (<= 215)");
            Check(ReachBook::CentreReach(b.BlowEdge(F, W), b.ComboEdge(F, W), Use::Combo, giantR, 200.0f) > 300.0f,
                  "edge reach: against the giant itself the reach is long (the radius is added back)");
        }
        {   // review N1: a chain split at an instant (the grace ran out unseen, he is already in the next attack)
            // swings 1-3 were the old chain's (t = 100, 400, 700 ms), swing 4 the new attack's (t = 1300 ms); the split is at 1000 ms
            std::int64_t ts[8]{ 100, 400, 700, 1300, 0, 0, 0, 0 };
            const unsigned after = ReachBook::SwingsAfter(ts, 4, 1000);
            Check(after == 1 && 4u - after == 3u, "split: the new attack's swing after the split is counted to the NEW chain, the old chain keeps its 3");
            Check(ReachBook::SwingsAfter(ts, 3, 1000) == 0, "split: no swing after the split -> the old chain keeps all of them");
            std::int64_t ring[8]{ 9000, 9100, 9200, 9300, 9400, 9500, 9600, 9700 };   // 10 swings counted; the ring holds swings 3..10
            Check(ReachBook::SwingsAfter(ring, 10, 9350) == 4, "split: a ring that wrapped still answers from the last 8 swings");
            // hits: the old chain's window ends at the split, the new one's starts there (a hit exactly at noneAt is the old chain's)
            Check(ReachBook::InChainWindow(5.0, 4.0, 6.0) && !ReachBook::InChainWindow(6.5, 4.0, 6.0), "split: a hit after the split is not the old chain's");
            Check(ReachBook::InChainWindow(6.5, 6.0, 0.0) && !ReachBook::InChainWindow(5.5, 6.0, 0.0), "split: the reopened chain's window starts at the split, with no end");
            Check(!ReachBook::InChainWindow(0.0, 0.0, 0.0), "split: an empty latch row (t = 0) is never a hit");
        }
        {   // the published pair: any thread, both 0 erases, CentreReach / SwingLimit are the one fallback
            ReachBook::ClearPublished();
            Check(ReachBook::CentreReach(F, Use::Blow, 22.0f, 200.0f) == 200.0f && ReachBook::SwingLimit(F, 22.0f, 200.0f) == 200.0f, "reach book: nothing published reads fMeleeReach (the executors too)");
            ReachBook::Publish(F, 120.0f, 160.0f);
            Check(ReachBook::CentreReach(F, Use::Blow, 22.0f, 200.0f) == 142.0f && ReachBook::CentreReach(F, Use::Combo, 22.0f, 200.0f) == 182.0f &&
                  ReachBook::CentreReach(0x15, Use::Blow, 22.0f, 200.0f) == 200.0f, "reach book: published edge + the foe's radius; another follower is unaffected");
            const float lim = ReachBook::SwingLimit(F, 22.0f, 200.0f);
            Check(std::fabs(lim - 182.0f * Reach::kGate * 1.15f) < 0.01f, "reach book: the executors' limit is the swing gate's own veto line (review F4)");
            ReachBook::Publish(F, 0.0f, 0.0f);
            Check(ReachBook::CentreReach(F, Use::Combo, 22.0f, 200.0f) == 200.0f, "reach book: a published 0 erases (a bow in hand, the fight ended)");
        }
        {   // every reachability read uses the ONE reader (Reposition::BladeReach / ComboReach)
            Snapshot s; s.drawn = PostureKind::Melee; s.self.z = 0.0f; s.meleeReach = 200.0f;
            Combatant human; human.bodyRadius = 22.0f; human.z = 295.0f; human.sight = Sight::Visible;
            Check(Reposition::BladeReach(s, human) == 200.0f && Reposition::ComboReach(s, human) == 200.0f, "reader: nothing learned -> fMeleeReach for both uses");
            s.weaponEdge = 128.0f; s.comboEdge = 178.0f;
            Check(Reposition::BladeReach(s, human) == 150.0f && Reposition::ComboReach(s, human) == 200.0f, "reader: Blow reads the weapon edge + radius, Combo the sequence edge + radius");
            Check(Reposition::MeleeUnreachableNow(s, human), "reader: the height line uses the one-blow reach (a human 295 up is out of 120 + 150) though the combo would reach 320");
            Combatant dragon; dragon.bodyRadius = 150.0f; dragon.z = 450.0f; dragon.sight = Sight::Visible;
            s.weaponEdge = 178.0f; s.comboEdge = 400.0f;   // a long combo series learned against big foes
            Check(Reposition::MeleeUnreachableNow(s, dragon), "edge reach: a dragon 450 up stays unreachable (the height line is the one-blow value, never the combo max)");
            s.weaponEdge = 0.0f; s.comboEdge = 0.0f;
            Check(!Reposition::MeleeUnreachableNow(s, human), "reader: the same human is reachable under the flat 200 (nothing learned)");
            Combatant nearFoe; nearFoe.dist = 180.0f; nearFoe.detectsMe = Tri::No; nearFoe.bodyRadius = 22.0f;
            s.weaponEdge = 128.0f;
            Check(!Reposition::StrikeReady(s, nearFoe), "reader: StrikeReady uses the one-blow reach (180 away, reach 150)");
            s.weaponEdge = 0.0f;
            Check(Reposition::StrikeReady(s, nearFoe), "reader: StrikeReady falls back to the flat reach (180 away, 200)");
        }
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
    // card G2 Layer 1: the hold row and the blow from stealth (iaus/Generate.cpp Holds, Cons::HoldStill / BoostStrike).
    void HideHold() {
        std::printf("\n== G2 Layer 1: hold while hidden, strike the unaware foe in reach ==\n");
        const Tuning& t = DefaultTuning();
        ClassGate warrior; warrior.resolved = true; warrior.powerAttack = true;
        const auto make = [&](float dist, Tri detects, bool sneaking) {
            Snapshot s;
            s.self = Actor("Thief", 0x14F01, 180, 200, 0, 0);
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, dist, 0) };
            s.foes[0].detectsMe = detects;
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
            s.selfSneaking = sneaking;
            s.hideRoad = true;
            return s;
        };
        const auto count = [](const Generated& g, Family f) {
            int n = 0;
            for (const Candidate& c : g.actions) if (c.family == f) ++n;
            return n;
        };
        const auto score = [&](const Generated& g, Family f) {
            for (const Candidate& c : g.actions)
                if (c.family == f) return ScoreOne(c, Mind{}, Profile{}, t, 1, 0, nullptr);
            return -1.0f;
        };
        {   // default Snapshot (not sneaking, detection unknown): nothing changes, no hold candidate
            const Generated g = Generate(make(150, Tri::Unknown, false), warrior, t);
            Check(count(g, Family::Sneak) == 0 && count(g, Family::Attack) == 1, "G2: not sneaking / detection unknown -> no hold row, the attack stays");
        }
        {   // hidden, foe far: hold stands, the attack on the unaware foe out of reach is not named
            const Generated g = Generate(make(900, Tri::No, true), warrior, t);
            Check(count(g, Family::Sneak) == 1 && count(g, Family::Attack) == 0, "G2: hidden and the foe out of reach -> the hold row, no chase");
            Check(score(g, Family::Sneak) > 0.0f, "G2: the hold row scores above 0 while no blow is ready");
        }
        {   // hidden, foe in reach: the hold row is gated to 0 and the attack carries the strike boost
            const Generated g = Generate(make(150, Tri::No, true), warrior, t);
            const float hold = score(g, Family::Sneak), atk = score(g, Family::Attack);
            const Generated plain = Generate(make(150, Tri::Yes, true), warrior, t);
            Check(hold == 0.0f && atk > 0.0f, "G2: a blow from stealth is ready -> the hold row yields (score 0), the attack stands");
            Check(count(plain, Family::Sneak) == 0 && atk > score(plain, Family::Attack), "G2: the attack on an unaware foe outscores the same attack on a foe that sees him");
        }
        {   // one foe sees him: seen, no hold
            Snapshot s = make(900, Tri::No, true);
            s.foes.push_back(Actor("Archer", 0x3B611, 100, 100, 700, 0));
            s.foes[1].detectsMe = Tri::Yes;
            const Generated g = Generate(s, warrior, t);
            Check(count(g, Family::Sneak) == 0, "G2: one foe detects him -> no hold row");
        }
        {   // Harbinger absent (hideRoad false): no hold row, nothing is held back (legacy = absent only, never a stand-in)
            Snapshot s = make(900, Tri::No, true);
            s.hideRoad = false;
            const Generated g = Generate(s, warrior, t);
            Check(count(g, Family::Sneak) == 0, "G2: no Harbinger hold road -> no hold row");
        }
        {   // hidden, no blow ready: a loud cast and the taunt are not made, a soundless cast is (marth 2026-11-11, hidden casting)
            Snapshot s = make(900, Tri::No, true);
            s.self.mp = 100; s.self.mpMax = 100;
            s.foes[0].targetId = 0x14;
            SpellEntry loud = Spell("Firebolt", 0x12FCD, Prune::Nature::Offense, 40, 2000, Prune::kAvResistFire);
            SpellEntry quiet = Spell("Quiet Bolt", 0x12FCE, Prune::Nature::Offense, 40, 2000, Prune::kAvResistFire);
            quiet.silent = true;
            s.book.entries = { loud, quiet };
            const Generated hid = Generate(s, warrior, t);
            int loudN = 0, quietN = 0;
            for (const Candidate& c : hid.actions) {
                if (c.family != Family::CastOffense) continue;
                if (c.form == 0x12FCD) ++loudN;
                if (c.form == 0x12FCE) ++quietN;
            }
            Check(count(hid, Family::Sneak) == 1 && loudN == 0 && quietN > 0, "G2: hold row up -> the loud cast is dropped, the soundless one stays");
            Snapshot seen = s;
            seen.foes[0].detectsMe = Tri::Yes;
            const Generated open = Generate(seen, warrior, t);
            int loudOpen = 0;
            for (const Candidate& c : open.actions) if (c.family == Family::CastOffense && c.form == 0x12FCD) ++loudOpen;
            Check(loudOpen > 0, "G2: seen (no hold row) -> the loud cast is generated as before");
        }
        {   // a caster posture never holds
            Snapshot s = make(900, Tri::No, true);
            s.drawn = PostureKind::None;
            const Generated g = Generate(s, warrior, t);
            Check(count(g, Family::Sneak) == 0, "G2: no weapon drawn -> no hold row");
        }
    }

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
            Check(boltLiving && std::fabs(boltLiving->f.bestDamage - 25.0f / t.spellCastSec) < 1e-3f,
                  "B684: on a living foe the best is Firebolt (undead-only, out-of-reach, held-hand and unmeasured spells do not count)");
            Check(boltUndead && std::fabs(boltUndead->f.bestDamage - 80.0f / t.spellCastSec) < 1e-3f, "B684: on the undead foe the undead-only spell is the best");
            // fix/spell-worth C2 (supersedes "unmeasured = < 0": that was a free pass): an unmeasured spell is a placeholder share of his best measured option
            // (Tuning::unmeasuredDamage 0.5 x Firebolt's 25), strictly below it; < 0 stays only where NOTHING is measured on the foe (the all-unmeasured block below)
            Check(noise && noise->f.expDamage == 0.0f && mystery && std::fabs(mystery->f.expDamage - 12.5f) < 1e-4f,
                  "B684 / spell-worth C2: a measured 0 is 0, an unmeasured spell is the placeholder share of his best measured option (12.5 of 25)");
            if (noise && mystery && boltLiving) {
                const float sNoise = ScoreOne(*noise, Mind{}, Profile{}, t, 1, 0, nullptr);
                const float sMystery = ScoreOne(*mystery, Mind{}, Profile{}, t, 1, 0, nullptr);
                Check(sMystery > sNoise && sNoise > 0.0f, "B684: the unmeasured spell scores above the no-damage spell (and the no-damage one is not vetoed)");
            }
        }
        {
            const Snapshot s = build(false);   // both hands free: the two-hander counts
            const Generated g = Generate(s, mage, t);
            const Candidate* bolt = find(g, 0x12FCD, 0x3B600);
            Check(bolt && std::fabs(bolt->f.bestDamage - 100.0f / t.spellCastSec) < 1e-3f, "B684: with both hands free the two-handed spell is his best");
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
            s.comboEdge = 150.0f / Reach::kGate - ReachBook::kRefRadius;   // learned: a sequence reaching 150 / kGate centre to centre on a humanoid
            Check(unknownOk && std::fabs(reachOf(Family::PowerAttack) - 150.0f) < 0.01f,
                  "F5 / B687: the power attack reads the learned combo reach x the gate, and fMeleeReach while nothing is learned");
            Check(std::fabs(reachOf(Family::Attack) - 150.0f / Reach::kGate) < 0.01f && Estimate::StrikeDistance(s) == 400.0f,
                  "11-17: the attack's range test reads the learned combo reach (approach), the strike distance keeps fMeleeReach");
        }
    }
}


namespace {
    // S11b: the spell-worth READER (iaus/SpellWorth.h), one pin per term, and the SHIPPED-INERT proof (sw.live = 0 changes no Facts field).
    void SpellWorthReader() {
        std::printf("\n== spell-worth reader (S11b): one pin per term, shipped inert ==\n");
        const SpellWorth d{};   // the shipped table
        const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        Check(d.live == 0.0f, "shipped: sw.live is 0");
        // the names: Set / Get over one table, an unknown name or a non-finite value refused, SetTuningScalar routes sw.*
        { SpellWorth t; float v = -1.0f;
          Check(SetSpellWorthKnob(t, "sw.recoverSec", 0.25f) && GetSpellWorthKnob(t, "sw.recoverSec", v) && near(v, 0.25f) && near(t.recoverSec, 0.25f), "names: sw.recoverSec set and read back");
          Check(!SetSpellWorthKnob(t, "sw.nope", 1.0f) && !GetSpellWorthKnob(t, "sw.nope", v), "names: an unknown knob is refused");
          Check(!SetSpellWorthKnob(t, "sw.recoverSec", std::nanf("")), "names: a non-finite value is refused");
          Tuning tt; std::string why;
          Check(SetTuningScalar(tt, "sw.live", 0.5f, &why) && near(tt.spellWorth.live, 0.5f), "names: SetTuningScalar routes sw.live into Tuning::spellWorth");
          Check(!SetTuningScalar(tt, "sw.live", -1.0f, &why) && !SetTuningScalar(tt, "sw.bogus", 1.0f, &why), "names: SetTuningScalar refuses a negative or unknown sw.*"); }
        // cast seconds: fire-and-forget = land + charge + recover (+ failShare x failedStartSec); a stream = land + channel + recover; no charge read = not known
        { SpellEntry e = Spell("Bolt", 1, Prune::Nature::Offense, 30);
          Estimate::SpellIn in; in.chargeSec = 0.5f;
          Check(near(Estimate::CastSeconds(e, d, in, 11.5f), 0.9f + 0.5f + 0.5f), "cast seconds: land 0.9 + charge 0.5 + recover 0.5");
          in.failShare = 0.25f;
          Check(near(Estimate::CastSeconds(e, d, in, 11.5f), 1.9f + 0.25f * 4.0f), "cast seconds: + failShare x failedStartSec");
          Estimate::SpellIn none;
          Check(Estimate::CastSeconds(e, d, none, 11.5f) < 0.0f, "cast seconds: charge time not read = NOT KNOWN");
          e.concentration = true;
          Check(near(Estimate::ChannelSeconds(d, 11.5f), 11.5f * 0.7f), "channel: concChannelSec x channelUptime");
          Check(near(Estimate::CastSeconds(e, d, none, 11.5f), 0.9f + 8.05f + 0.5f), "cast seconds: a stream = land + channel + recover, no charge needed");
          Check(near(Estimate::ChannelSeconds(d, 0.1f), 0.5f), "channel: never shorter than channelMinSec"); }
        // area: one foe = 1; each extra foe x areaFoesIntoDamage; each party member in it takes areaFriendPenalty off
        Check(near(Estimate::AreaFactor(d, -1.0f, -1.0f), 1.0f) && near(Estimate::AreaFactor(d, 1.0f, 0.0f), 1.0f), "area: no area / one foe = 1");
        Check(near(Estimate::AreaFactor(d, 3.0f, 0.0f), 3.0f), "area: three foes = 3 at areaFoesIntoDamage 1");
        Check(near(Estimate::AreaFactor(d, 3.0f, 1.0f), 2.0f) && near(Estimate::AreaFactor(d, 1.0f, 5.0f), 0.0f), "area: a party member takes his share off, never below 0");
        { SpellWorth t; t.areaFoesIntoDamage = 0.0f; Check(near(Estimate::AreaFactor(t, 4.0f, 0.0f), 1.0f), "area: areaFoesIntoDamage 0 = today (one foe's worth)"); }
        // a DoT over the foe's expected life
        Check(near(Estimate::DotOverLife(d, 10.0f, 8.0f, 3.0f), 30.0f), "dot: 10/s over 8 s against a foe that lives 3 s = 30");
        Check(near(Estimate::DotOverLife(d, 10.0f, 8.0f, 30.0f), 80.0f) && near(Estimate::DotOverLife(d, 10.0f, 8.0f, -1.0f), 80.0f), "dot: a long-lived or unknown-life foe = the whole duration");
        Check(near(Estimate::DotOverLife(d, 10.0f, 8.0f, 0.5f), 20.0f), "dot: the foe's life is floored at foeLifeFloorSec (2 s)");
        Check(Estimate::DotOverLife(d, -1.0f, 8.0f, 3.0f) < 0.0f, "dot: no DoT read = NOT KNOWN");
        // control: held seconds x the foe's damage rate, never past his life
        { Combatant foe = Actor("Foe", 0x3B, 100, 100, 300, 0); foe.hit = 20.0f; foe.hitsPerSec = 1.0f;
          Check(near(Estimate::ControlEffect(d, 5.0f, foe), 100.0f), "control: 5 s x 20 x 1/s = 100");
          Check(near(Estimate::ControlHeldSec(d, 5.0f, 3.0f), 3.0f) && near(Estimate::ControlHeldSec(d, 5.0f, 60.0f), 5.0f) && near(Estimate::ControlHeldSec(d, 5.0f, -1.0f), 5.0f), "control: held seconds stop at the foe's expected life");
          Combatant blind = Actor("Blind", 0x3C, 100, 100, 300, 0);
          Check(Estimate::ControlEffect(d, 5.0f, blind) < 0.0f, "control: a foe whose damage was not measured = NOT KNOWN"); }
        // buff / ward / summon in the same unit
        { Estimate::SpellIn in; in.buffPerSec = 4.0f; in.buffSec = 60.0f; in.fightSec = 10.0f;
          Check(near(Estimate::BuffEffect(d, in), 40.0f), "buff: damage prevented per second x the seconds it is up in THIS fight (capped by the fight)");
          in.fightSec = -1.0f; Check(near(Estimate::BuffEffect(d, in), 240.0f), "buff: fight length unknown = its whole duration");
          Estimate::SpellIn no; Check(Estimate::BuffEffect(d, no) < 0.0f, "buff: nothing read = NOT KNOWN");
          Estimate::SpellIn w; w.wardPerSec = 5.0f; Check(near(Estimate::WardEffect(d, w, 6.0f), 30.0f) && Estimate::WardEffect(d, no, 6.0f) < 0.0f, "ward: absorbed per second x seconds up; nothing read = NOT KNOWN");
          Estimate::SpellIn sm; sm.summonDps = 7.0f;
          Check(near(Estimate::SummonEffect(d, sm, 60.0f), 7.0f * 20.0f) && near(Estimate::SummonEffect(d, sm, 8.0f), 56.0f), "summon: its damage rate x min(its duration, summonLifeSec)");
          Check(Estimate::SummonEffect(d, no, 60.0f) < 0.0f, "summon: rate not read (the game leaves it so) = NOT KNOWN"); }
        // magicka against the fight
        Check(near(Estimate::MagickaShare(d, 0.0f, 2.0f, 0.0f, 0.0f, 30.0f), 1.0f), "magicka: a free spell = whole");
        Check(near(Estimate::MagickaShare(d, 30.0f, 2.0f, 500.0f, 0.0f, 30.0f), 1.0f), "magicka: a pool that lasts the fight = whole");
        Check(near(Estimate::MagickaShare(d, 30.0f, 2.0f, 100.0f, 0.0f, 30.0f), 0.5f), "magicka: a pool that runs dry = 1 - magickaDryPenalty");
        Check(near(Estimate::MagickaShare(d, 30.0f, 2.0f, 100.0f, 20.0f, 30.0f), 1.0f), "magicka: regen counts against the horizon");
        // the derived inputs: foe life and fight length from the party's weapon damage
        { Snapshot s; s.self = Actor("Me", 0x14F00, 100, 100, 0, 0); s.self.hit = 10.0f; s.self.hitsPerSec = 1.0f;
          Combatant ally = Actor("Ally", 0x14F01, 100, 100, 0, 0); ally.hit = 10.0f; ally.hitsPerSec = 1.0f; s.allies = { ally };
          Combatant f1 = Actor("F1", 0x3B1, 100, 100, 300, 0), f2 = Actor("F2", 0x3B2, 40, 40, 300, 0); s.foes = { f1, f2 };
          Check(near(Estimate::FoeLifeSec(s, s.foes[0]), 5.0f) && near(Estimate::FightSec(s), 7.0f), "foe life = hp / the party's damage on it (100 / 20 = 5 s); the fight = the lives added (5 + 2 = 7 s)");
          Combatant blind = Actor("Blind", 0x3B3, 50, 50, 300, 0); blind.armor = 0.0f; s.foes.push_back(blind);
          Check(near(Estimate::FoeLifeSec(s, s.foes[2]), 2.5f), "foe life: the party's damage on a third foe");
          Snapshot none; none.self = Actor("Me", 1, 100, 100, 0, 0); none.foes = { f1 };
          Check(Estimate::FoeLifeSec(none, none.foes[0]) < 0.0f && Estimate::FightSec(none) < 0.0f, "foe life / fight length: no damage measured = NOT KNOWN"); }
        // THE READER, offense: a DoT over a short-lived foe is worth less than today's whole-duration estimate; a stream is its second x the channel; a measured 0 stays unknown
        { Snapshot s; s.self = Actor("Me", 0x14F00, 100, 100, 0, 0); s.self.mp = 500; s.self.mpMax = 500; s.self.hit = 10.0f; s.self.hitsPerSec = 1.0f;
          Combatant foe = Actor("Foe", 0x3B1, 20, 20, 300, 0); s.foes = { foe };   // lives 2 s
          SpellEntry dot = Spell("Burn", 2, Prune::Nature::Offense, 40); dot.measured = true; dot.damage = 90.0f; dot.chargeSec = 0.5f;
          dot.dotPerSec = 10.0f; dot.dotSec = 8.0f; dot.directDamage = 10.0f;
          const Estimate::SpellIn in = Estimate::MakeSpellIn(dot, foe, s, d, -1.0f, -1.0f);
          const Estimate::Worth w = Estimate::SpellWorthOn(dot, foe, s, d, in, 11.5f);
          Check(w.known && near(w.effect, 10.0f + 10.0f * 2.0f) && near(w.seconds, 1.9f) && near(w.magicka, 40.0f), "offense: direct 10 + a DoT counted over the foe's 2 s life = 30 (today 90), 1.9 s, 40 magicka");
          Check(near(w.PerSecond(), 30.0f / 1.9f) && near(w.PerMagicka(), 0.75f), "offense: per second and per magicka");
          SpellEntry st = Spell("Stream", 3, Prune::Nature::Offense, 10); st.measured = true; st.damage = 20.0f; st.concentration = true;
          const Estimate::Worth ws = Estimate::SpellWorthOn(st, foe, s, d, Estimate::MakeSpellIn(st, foe, s, d, -1.0f, -1.0f), 11.5f);
          Check(ws.known && near(ws.effect, 20.0f * 8.05f) && near(ws.magicka, 10.0f * 8.05f), "offense: a stream = its per-second effect x the held seconds, its cost x the same");
          SpellEntry un = Spell("Unmeasured", 4, Prune::Nature::Offense, 10); un.chargeSec = 0.5f;
          Check(!Estimate::SpellWorthOn(un, foe, s, d, Estimate::MakeSpellIn(un, foe, s, d, -1.0f, -1.0f), 11.5f).known, "offense: an unmeasured spell is NOT KNOWN (the caller keeps today's, never a 0)");
          SpellEntry nocharge = Spell("NoCharge", 5, Prune::Nature::Offense, 10); nocharge.measured = true; nocharge.damage = 20.0f;
          Check(!Estimate::SpellWorthOn(nocharge, foe, s, d, Estimate::MakeSpellIn(nocharge, foe, s, d, -1.0f, -1.0f), 11.5f).known, "offense: a spell whose charge time was not read is NOT KNOWN");
          SpellEntry area = dot; area.areaUnits = 300.0f; area.dotPerSec = -1.0f; area.dotSec = -1.0f; area.directDamage = -1.0f;
          Check(near(Estimate::SpellWorthOn(area, foe, s, d, Estimate::MakeSpellIn(area, foe, s, d, 3.0f, 1.0f), 11.5f).effect / Estimate::SpellWorthOn(area, foe, s, d, Estimate::MakeSpellIn(area, foe, s, d, 1.0f, 0.0f), 11.5f).effect, 2.0f), "offense: an area spell on 3 foes with 1 friend in it = 2 x the one-foe effect");
          SpellEntry red = dot; Estimate::SpellIn rin = in; rin.redundant = true;
          Check(near(Estimate::SpellWorthOn(red, foe, s, d, rin, 11.5f).effect, 0.0f), "offense: redundantEffectScale 0 zeroes an effect already live on the target (a table row, not a veto)"); }
        // heal: never more than is missing
        { Snapshot s; s.self = Actor("Me", 0x14F00, 100, 100, 0, 0); s.self.mp = 500; s.self.mpMax = 500;
          Combatant hurt = Actor("Hurt", 0x14F01, 70, 100, 0, 0);
          SpellEntry h = Spell("Heal", 6, Prune::Nature::Heal, 20); h.measured = true; h.heal = 100.0f; h.chargeSec = 0.4f;
          const Estimate::Worth w = Estimate::SpellWorthOn(h, hurt, s, d, Estimate::MakeSpellIn(h, hurt, s, d, -1.0f, -1.0f), 11.5f);
          Check(w.known && near(w.effect, 30.0f), "heal: a 100 heal on a recipient missing 30 is worth 30"); }
        // control and buff through the one reader
        { Snapshot s; s.self = Actor("Me", 0x14F00, 100, 100, 0, 0); s.self.mp = 500; s.self.mpMax = 500; s.self.hit = 10.0f; s.self.hitsPerSec = 1.0f;
          Combatant foe = Actor("Foe", 0x3B1, 100, 100, 300, 0); foe.hit = 20.0f; foe.hitsPerSec = 1.0f; s.foes = { foe };   // lives 10 s
          SpellEntry c = Spell("Calm", 7, Prune::Nature::Control, 20); c.measured = true; c.controlSec = 30.0f; c.chargeSec = 0.5f;
          const Estimate::Worth w = Estimate::SpellWorthOn(c, foe, s, d, Estimate::MakeSpellIn(c, foe, s, d, -1.0f, -1.0f), 11.5f);
          Check(w.known && near(w.effect, 10.0f * 20.0f), "control: 30 s of hold counts the foe's 10 s life x 20 damage/s = 200");
          Combatant me = s.self; Combatant hitter = foe; hitter.targetId = me.id; hitter.hit = 30.0f; hitter.hitsPerSec = 1.0f; hitter.x = 50.0f; hitter.dist = 50.0f; s.foes = { hitter };
          SpellEntry b = Spell("Armor", 8, Prune::Nature::Buff, 20); b.measured = true; b.chargeSec = 0.5f; b.buffArmor = 100.0f; b.buffSec = 60.0f;
          const Estimate::SpellIn bin = Estimate::MakeSpellIn(b, me, s, d, -1.0f, -1.0f);
          Check(bin.buffPerSec >= 0.0f && near(bin.buffPerSec, 30.0f * 0.12f), "buff: 100 armor points x 0.12 percent = 12 percent of the 30/s coming in");
          Check(Estimate::SpellWorthOn(b, me, s, d, bin, 11.5f).known, "buff: known once armor and duration were read"); }
        // INERT: sw.live = 0 (the shipped table) leaves every Facts field of a mixed book exactly as without the reader
        { Tuning off = DefaultTuning(); Tuning on = DefaultTuning(); on.spellWorth.live = 1.0f;
          Snapshot s; s.self = Actor("Jesper", 0x14F00, 180, 200, 0, 0); s.self.mp = 400; s.self.mpMax = 400; s.self.hit = 10.0f; s.self.hitsPerSec = 1.0f;
          s.playerId = 0x14; s.currentTargetId = 0x3B600; s.hasBook = true; s.combatSec = 3.0f;
          s.foes = { Actor("Bandit", 0x3B600, 100, 100, 600, 0) };
          s.allies = { Actor("Ally", 0x14F01, 40, 100, 100, 0) };
          const auto add = [&](const char* n, std::uint32_t id, Prune::Nature nat, float cost, float dmg, float heal, float ctl) {
              SpellEntry e = Spell(n, id, nat, cost); e.measured = true; e.damage = dmg; e.heal = heal; e.controlSec = ctl; e.school = kDestruction; e.chargeSec = 0.5f; s.book.entries.push_back(e); };
          add("Firebolt", 0x12FCD, Prune::Nature::Offense, 40, 25.0f, 0.0f, 0.0f); add("Flare", 0x2FF01, Prune::Nature::Offense, 60, 69.0f, 0.0f, 0.0f);
          add("Heal", 0x12FCC, Prune::Nature::Heal, 30, 0.0f, 80.0f, 0.0f); add("Calm", 0x12FC0, Prune::Nature::Control, 30, 0.0f, 0.0f, 30.0f);
          ClassGate mage; mage.resolved = true; mage.spellsOnClass = true;
          const Generated g0 = Generate(s, mage, off), g1 = Generate(s, mage, DefaultTuning()), gl = Generate(s, mage, on);
          bool same = g0.actions.size() == g1.actions.size();
          for (std::size_t i = 0; same && i < g0.actions.size(); ++i) {
              const Facts& a = g0.actions[i].f; const Facts& b = g1.actions[i].f;
              same = a.expDamage == b.expDamage && a.expRate == b.expRate && a.bestDamage == b.bestDamage && a.effectRate == b.effectRate && a.healRate == b.healRate && a.controlSec == b.controlSec && a.expHeal == b.expHeal && a.bestHealRate == b.bestHealRate;
          }
          Check(same && !g0.actions.empty(), "INERT: sw.live = 0 leaves every Facts field the reader touches bit for bit as it is");
          bool moved = false;
          for (std::size_t i = 0; i < gl.actions.size() && i < g0.actions.size(); ++i) { const Facts& a = g0.actions[i].f; const Facts& b = gl.actions[i].f; moved = moved || a.expRate != b.expRate || a.healRate != b.healRate || a.controlSec != b.controlSec || a.expHeal != b.expHeal; }
          Check(moved, "LIVE: sw.live = 1 moves the Facts of a book the reader knows (the call sites are wired)"); }
    }
}

namespace {
    // fix/spell-worth (field Jesper 11-23: IAUS had no damage number for ~62 of 74 offense spells, so they TIED and the cheapest won).
    void SpellWorthRules() {
        std::printf("\n== spell-worth: rider damage, resist once, unmeasured placeholder, level cap ==\n");
        const Tuning& t = DefaultTuning();
        ClassGate mage; mage.resolved = true; mage.spellsOnClass = true;
        const auto find = [](const Generated& g, std::uint32_t form, std::uint32_t target) -> const Candidate* {
            for (const Candidate& c : g.actions)
                if (c.form == form && c.target == target) return &c;
            return nullptr;
        };
        // C1: which spells are unmeasurable (the pure rule Fills.cpp MeasureSpell calls)
        Check(!Estimate::DamageUnmeasurable(true, true, 30.0f), "C1: a rider bolt (an indirect effect AND direct damage) is MEASURED (its direct damage, a lower bound)");
        Check(Estimate::DamageUnmeasurable(true, true, 0.0f), "C1: Wall of Flames (an indirect effect, no direct damage) stays UNMEASURED (MFO-B684)");
        Check(!Estimate::DamageUnmeasurable(true, false, 0.0f) && !Estimate::DamageUnmeasurable(false, true, 0.0f),
              "C1: a spell with no indirect effect, and a non-offense spell with one, are never marked unmeasurable");

        // The named tie cases: Jesper's Ice Spike (23), Elemental Flare (69), Firebolt (25), one foe. BEFORE = no damage number on any of them (every
        // rider discarded: nothing measured on the foe); AFTER = measured by their direct damage.
        const auto book = [&](bool a_measured) {
            Snapshot s;
            s.self = Actor("Jesper", 0x14F00, 180, 200, 0, 0); s.self.mp = 400; s.self.mpMax = 400;
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B600, 100, 100, 600, 0) };
            s.currentTargetId = 0x3B600; s.hasBook = true; s.combatSec = 3.0f;
            const auto add = [&](const char* n, std::uint32_t id, float cost, float dmg) {
                SpellEntry e = Spell(n, id, Prune::Nature::Offense, cost); e.damage = dmg; e.measured = a_measured; e.school = kDestruction; s.book.entries.push_back(e);
            };
            add("Ice Spike", 0x1CEAD, 30, 23.0f); add("Firebolt", 0x12FCD, 40, 25.0f); add("Elemental Flare", 0x2FF01, 60, 69.0f);
            return s;
        };
        std::string orders[2];
        for (int measured = 0; measured < 2; ++measured) {
            const Snapshot s = book(measured != 0);
            const Generated g = Generate(s, mage, t);
            const Decision d = Decide(Mind{}, g.actions, Profile{}, t, Tick(3));
            bool first = true;
            for (const Scored& sc : d.ranked) {
                const Candidate& c = g.actions[sc.idx];
                if (c.family != Family::CastOffense) continue;
                orders[measured] += std::format("{}{} {:.3f}", first ? "" : " > ", c.formName, sc.score);
                first = false;
            }
            std::printf("  %s: %s\n", measured ? "AFTER  (measured by direct damage)" : "BEFORE (no damage number)         ", orders[measured].c_str());
        }
        Check(orders[0].rfind("Ice Spike", 0) == 0, "spell-worth: BEFORE, the three tie and the cheapest (Ice Spike) wins (the field bug)");
        Check(orders[1].rfind("Elemental Flare", 0) == 0, "spell-worth: AFTER, the strongest measured spell (Elemental Flare 69) ranks first");

        // C3: resist counted once. A foe with 50 magic resist, and 50 poison resist, and 50 fire resist.
        {
            Combatant foe = Actor("Mage", 0x3B700, 100, 100, 600, 0);
            foe.magicResist = 50.0f;
            foe.resists = { { Estimate::kAvResistMagic, 50.0f }, { Estimate::kAvPoisonResist, 50.0f }, { Prune::kAvResistFire, 50.0f } };
            SpellEntry sp = Spell("Bolt", 0x1, Prune::Nature::Offense, 10, 2000.0f, Estimate::kAvResistMagic);
            sp.damage = 100.0f; sp.measured = true;
            Check(std::fabs(Estimate::SpellOn(sp, foe) - 50.0f) < 1e-3f, "C3: a spell keyed on ResistMagic is cut by it ONCE (100 x (1 - 50%) = 50, not 25)");
            sp.resistAv = Prune::kAvResistFire;
            Check(std::fabs(Estimate::SpellOn(sp, foe) - 25.0f) < 1e-3f, "C3: an element spell is cut by magic resist AND its element (50% x 50% = 25)");
            sp.resistAv = Estimate::kAvPoisonResist; sp.poisonType = true; foe.magicResist = 80.0f;
            Check(std::fabs(Estimate::SpellOn(sp, foe) - 50.0f) < 1e-3f, "C3: a poison-type spell reads the poison resist once (50 -> 50) and never the magic resist (80)");
            sp.resistAv = Prune::kAvResistFire;
            Check(std::fabs(Estimate::SpellOn(sp, foe) - 25.0f) < 1e-3f, "C3: a poison-type spell with an element: poison resist x element (50% x 50% = 25)");
        }
        // C3: exploit reads only the resist the damage does not carry
        {
            Snapshot s = book(true);
            s.foes[0].resists = { { Prune::kAvResistFire, -50.0f }, { Prune::kAvResistFrost, 50.0f } };
            s.book.entries[0].resistAv = Prune::kAvResistFrost;   // Ice Spike, measured
            SpellEntry unk = Spell("Unknown Fire", 0x3000, Prune::Nature::Offense, 20, 2000.0f, Prune::kAvResistFire);   // not measured
            SpellEntry ctl = Spell("Frost Calm", 0x3001, Prune::Nature::Control, 20, 2000.0f, Prune::kAvResistFrost); ctl.measured = true;
            s.book.entries.push_back(unk); s.book.entries.push_back(ctl);
            const Generated g = Generate(s, mage, t);
            const Candidate* a = find(g, 0x1CEAD, 0x3B600); const Candidate* b = find(g, 0x3000, 0x3B600); const Candidate* c = find(g, 0x3001, 0x3B600);
            Check(a && a->f.resist == 0.0f, "C3: a measured spell's damage carries the foe's frost resist, so exploit reads none for it");
            Check(b && b->f.resist == 0.0f && c && c->f.resist == 50.0f, "C3 / F4: a control spell keeps the exploit read; an unmeasured spell with a yardstick gets NO weakness bonus (-50 reads 0)");
            s.foes[0].resists = { { Prune::kAvResistFire, 60.0f } };
            const Generated g2 = Generate(s, mage, t);
            const Candidate* b2 = find(g2, 0x3000, 0x3B600);
            Check(b2 && b2->f.resist == 60.0f, "F4: ... and the resist side stays (an unmeasured fire spell on a fire-resistant foe reads 60)");
            Check(a && a->f.expDamage > 0.0f && std::fabs(a->f.expDamage - 23.0f * 0.5f) < 1e-3f, "C3: and the damage itself carries it (Ice Spike 23 x 50% = 11.5)");
        }

        // C2: unmeasured is below the best measured, named and settable; DamageValue / Overkill are not Off
        {
            Snapshot s = book(true);
            SpellEntry unk = Spell("Unknown", 0x3000, Prune::Nature::Offense, 20); unk.measured = false; unk.damage = 900.0f; s.book.entries.push_back(unk);
            Generated g = Generate(s, mage, t);
            const Candidate* u = find(g, 0x3000, 0x3B600);
            Check(u && std::fabs(u->f.bestDamage - 69.0f / t.spellCastSec) < 1e-3f && std::fabs(u->f.expDamage - 34.5f) < 1e-3f && std::fabs(u->f.expRate - 0.5f * u->f.bestDamage) < 1e-3f,
                  "C2: an unmeasured spell is worth t.unmeasuredDamage (0.5) of the best measured (69) = 34.5, below it, whatever its record damage says");
            bool dvOn = false, okOn = false;
            if (u) {
                std::vector<Term> terms;
                (void)ScoreOne(*u, Mind{}, Profile{}, t, 1, 0, &terms);
                for (const Term& tm : terms) {
                    if (tm.cons == Cons::DamageValue && std::fabs(tm.x - 0.5f) < 1e-3f) dvOn = true;
                    if (tm.cons == Cons::Overkill) okOn = true;
                }
            }
            Check(dvOn && okOn, "C2: DamageValue reads 0.5 and Overkill reads a number for it (neither is 'not applicable')");
            Tuning tt = t; std::string why;
            Check(SetTuningScalar(tt, "t.unmeasuredDamage", 0.2f, &why) && tt.unmeasuredDamage == 0.2f, "C2: t.unmeasuredDamage is settable through SetTuningScalar");
            const Candidate* u2 = find(Generate(s, mage, tt), 0x3000, 0x3B600);
            Check(u2 && std::fabs(u2->f.expDamage - 13.8f) < 1e-3f, "C2: and the scalar moves the placeholder (0.2 x 69 = 13.8)");
            SetTuningScalar(tt, "t.unmeasuredDamage", 5.0f, &why);
            const Candidate* u3 = find(Generate(s, mage, tt), 0x3000, 0x3B600);
            Check(u3 && u3->f.expDamage < 69.0f && u3->f.expRate < u3->f.bestDamage, "C2: a scalar of 5 still holds the placeholder strictly below the best measured");
        }

        // C5: a capped Turn on a foe above its cap is no candidate; Turn on a valid foe stays
        {
            Snapshot s;
            s.self = Actor("Jesper", 0x14F00, 180, 200, 0, 0); s.self.mp = 400; s.self.mpMax = 400;
            s.playerId = 0x14;
            Combatant weak = Actor("Skeleton", 0x3B600, 100, 100, 600, 0, 8); weak.undead = true;
            Combatant strong = Actor("Deathlord", 0x3B601, 100, 100, 650, 0, 30); strong.undead = true;
            s.foes = { weak, strong };
            s.currentTargetId = 0x3B600; s.hasBook = true; s.combatSec = 3.0f;
            const std::uint32_t turns[3] = { 0x4D3F9, 0x4D3FA, 0x4D3FB };
            const float caps[3] = { 12.0f, 20.0f, 28.0f };
            for (int i = 0; i < 3; ++i) {
                SpellEntry e = Spell("Turn", turns[i], Prune::Nature::Control, 30); e.undeadOnly = true; e.levelCap = caps[i]; e.measured = true;
                s.book.entries.push_back(e);
            }
            const Generated g = Generate(s, mage, t);
            Check(find(g, turns[0], 0x3B600) && find(g, turns[1], 0x3B600) && find(g, turns[2], 0x3B600), "C5: on a level-8 foe all three Turns are candidates (under every cap)");
            Check(find(g, turns[0], 0x3B601) && find(g, turns[2], 0x3B601) && g.capQueued.size() == 3,
                  "F2: with NO will-work answer yet the book's cap removes nothing (the three pairs on the level-30 foe stay one lap) and they are queued to be asked");
            {
                Snapshot s2 = s;
                s2.willWork = { { turns[0], 0x3B601, Tri::No }, { turns[1], 0x3B601, Tri::No }, { turns[2], 0x3B601, Tri::Yes } };
                const Generated g2 = Generate(s2, mage, t);
                Check(!find(g2, turns[0], 0x3B601) && !find(g2, turns[1], 0x3B601) && find(g2, turns[2], 0x3B601) && g2.capQueued.empty(),
                      "C5 / F2: a will-work No removes the pair on a foe above the cap, a Yes keeps it (the cap is a hint, WillWork decides), and nothing is queued");
                Check(find(g2, turns[0], 0x3B600), "C5: Turn on the valid foe stays");
            }
            Check(Estimate::LevelCapBars(12.0f, 13) && !Estimate::LevelCapBars(12.0f, 12) && !Estimate::LevelCapBars(0.0f, 99), "C5: the bar is WillWork's own (level > cap; cap 0 = none)");
            // and the capped spell does not count as the foe's best option
            SpellEntry big = Spell("Capped Blast", 0x4000, Prune::Nature::Offense, 30); big.damage = 200.0f; big.measured = true; big.levelCap = 20.0f;
            SpellEntry small = Spell("Plain Bolt", 0x4001, Prune::Nature::Offense, 30); small.damage = 20.0f; small.measured = true;
            s.book.entries.push_back(big); s.book.entries.push_back(small);
            s.willWork = { { 0x4000, 0x3B601, Tri::No } };
            const Candidate* pb = find(Generate(s, mage, t), 0x4001, 0x3B601);
            Check(pb && std::fabs(pb->f.bestDamage - 20.0f / t.spellCastSec) < 1e-3f, "F3: a spell WillWork refused on a foe is not that foe's best option (the denominator skips it)");
        }

        // F1: an immune foe loses the pair, it does not score at full
        {
            Snapshot s = book(true);
            s.foes[0].resists = { { Prune::kAvResistFire, 100.0f }, { Prune::kAvResistFrost, 0.0f } };
            s.book.entries[0].resistAv = Prune::kAvResistFrost;   // Ice Spike (frost)
            s.book.entries[1].resistAv = Prune::kAvResistFire;    // Firebolt
            s.book.entries[2].resistAv = Prune::kAvResistFire;    // Elemental Flare
            Generated g = Generate(s, mage, t);
            Check(!find(g, 0x12FCD, 0x3B600) && !find(g, 0x2FF01, 0x3B600) && find(g, 0x1CEAD, 0x3B600),
                  "F1: a flame atronach (100 fire resist) is no target of the fire spells, and the frost spell stays");
            s.book.entries.erase(s.book.entries.begin());   // no frost option left: nothing but fire
            g = Generate(s, mage, t);
            bool anyCast = false;
            for (const Candidate& c : g.actions) if (c.family == Family::CastOffense) anyCast = true;
            Check(!anyCast, "F1: with only fire in the book the immune foe gets no cast candidate (never scored at full)");
        }
        // F4: a score-level check, unmeasured never outranks the measured best (same cost and reach, a foe weak to the unmeasured spell's element)
        {
            Snapshot s = book(true);
            s.foes[0].resists = { { Prune::kAvResistFire, -50.0f } };
            for (auto& e : s.book.entries) e.cost = 40.0f;
            SpellEntry unk = Spell("Unknown Fire", 0x3000, Prune::Nature::Offense, 40, 2000.0f, Prune::kAvResistFire); unk.school = kDestruction;
            s.book.entries.push_back(unk);
            const Generated g = Generate(s, mage, t);
            const Candidate* u = find(g, 0x3000, 0x3B600); const Candidate* best = find(g, 0x2FF01, 0x3B600);
            Check(u && best && ScoreOne(*u, Mind{}, Profile{}, t, 1, 0, nullptr) < ScoreOne(*best, Mind{}, Profile{}, t, 1, 0, nullptr),
                  "F4: on a foe weak to its element an unmeasured spell still scores below the measured best (no weakness bonus)");
        }
        // F5: a stream, a bolt and a long DoT on one unit (effect per cast second)
        {
            Snapshot s;
            s.self = Actor("Jesper", 0x14F00, 180, 200, 0, 0); s.self.mp = 400; s.self.mpMax = 400;
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B600, 100, 100, 600, 0) };
            s.currentTargetId = 0x3B600; s.hasBook = true; s.combatSec = 3.0f;
            SpellEntry bolt = Spell("Bolt", 0x5001, Prune::Nature::Offense, 30); bolt.damage = 30.0f; bolt.measured = true;
            SpellEntry stream = Spell("Stream", 0x5002, Prune::Nature::Offense, 30); stream.damage = 25.0f; stream.measured = true; stream.concentration = true;   // per second
            SpellEntry dot = Spell("Dot", 0x5003, Prune::Nature::Offense, 30); dot.damage = 60.0f; dot.measured = true;
            s.book.entries = { bolt, stream, dot };
            const Generated g = Generate(s, mage, t);
            const Candidate* cb = find(g, 0x5001, 0x3B600); const Candidate* cs = find(g, 0x5002, 0x3B600); const Candidate* cd = find(g, 0x5003, 0x3B600);
            const float bolt1 = 30.0f / t.spellCastSec, dot1 = 60.0f / t.spellCastSec;   // 20 and 40 per cast second; the stream 25 per second
            Check(cb && cs && cd && std::fabs(cb->f.expRate - bolt1) < 1e-3f && std::fabs(cs->f.expRate - 25.0f) < 1e-3f && std::fabs(cd->f.expRate - dot1) < 1e-3f &&
                  std::fabs(cb->f.bestDamage - dot1) < 1e-3f, "F5: bolt, stream and DoT read one unit (effect per cast second); the best is the DoT's 40");
            Check(cb && cs && cd && cb->f.expDamage == 30.0f && cs->f.expDamage == 25.0f && cd->f.expDamage == 60.0f, "F5: the raw cast stays in expDamage (Overkill reads it)");
        }

        // logs: the per-decision veto counts by consideration name
        {
            Snapshot s = book(true);
            Generated g = Generate(s, mage, t);
            for (Candidate& c : g.actions) c.f.willWork = Tri::No;
            const Decision d = Decide(Mind{}, g.actions, Profile{}, t, Tick(3));
            Check(d.vetoByCons[static_cast<std::size_t>(Cons::WillWork)] == 3 && d.vetoed == 3,
                  "log: Decision::vetoByCons counts the vetoes by the consideration that made them (will_work for the 3 casts; the attack is not vetoed)");
            const std::string line = FormatDecision(d, g.actions, t, -1, {}, 0.0);
            Check(line.find("vetoBy[will_work=") != std::string::npos, "log: the [iaus] line names them (vetoBy[will_work=N])");
            const Generated g2 = Generate(s, mage, t);
            const Decision d2 = Decide(Mind{}, g2.actions, Profile{}, t, Tick(3));
            const std::string line2 = FormatDecision(d2, g2.actions, t, 0, {}, 0.0);
            Check(line2.find("bestDmg[cast_offense") != std::string::npos && line2.find("Elemental Flare") != std::string::npos, "log: the [iaus] line names the best offense spell by raw expected damage with its score and rank");
        }
    }
}

namespace {
    // card G3 (rulings 2026-11-11): the Disengage gate (concealRoad / concealPoint / sneakClass / Found), the sneak hide's need, the
    // non-sneak FLEE (tree ON, hurt, a foe on him) and its pacing (no offense generated, his own heals boosted).
    void DisengageRules() {
        std::printf("\n== G3: the disengage gate, the hide need, the non-sneak flee ==\n");
        const Tuning& t = DefaultTuning();
        ClassGate thief; thief.resolved = true; thief.treeOn = true; thief.disengage = true; thief.sneakClass = true;
        ClassGate fighter; fighter.resolved = true; fighter.treeOn = true; fighter.disengage = true;
        const auto make = [&](float hp, Tri detects, bool onMe) {
            Snapshot s;
            s.self = Actor("Follower", 0x14F01, hp, 200, 0, 0);
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, 400, 0) };
            s.foes[0].detectsMe = detects;
            if (onMe) s.foes[0].targetId = 0x14F01;
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
            s.concealRoad = true; s.concealPoint = true;
            return s;
        };
        const auto count = [](const Generated& g, Family f) {
            int n = 0;
            for (const Candidate& c : g.actions) if (c.family == f) ++n;
            return n;
        };
        const auto find = [](const Generated& g, Family f) -> const Candidate* {
            for (const Candidate& c : g.actions) if (c.family == f) return &c;
            return nullptr;
        };
        // ---- the seam: Found reads G2's verdict, nothing else ----
        {
            Snapshot s = make(180, Tri::Yes, false);
            Check(Disengage::Found(s), "G3: a foe whose group record has him (detectsMe Yes) -> Found, though it targets someone else");
            s.foes[0].detectsMe = Tri::No;
            Check(!Disengage::Found(s), "G3: detectsMe No -> not found");
            s.foes[0].detectsMe = Tri::Unknown; s.foes[0].targetId = 0x14F01;
            Check(!Disengage::Found(s), "G3: detectsMe Unknown is not found, even when the foe targets him (the old target reading is gone)");
            s.foes[0].detectsMe = Tri::Yes; s.foes[0].paralysed = true;
            Check(!Disengage::Found(s), "G3: a paralysed foe does not count");
        }
        // ---- the hide run (a sneak class) ----
        {
            const Generated g = Generate(make(180, Tri::Yes, false), thief, t);
            const Candidate* d = find(g, Family::Disengage);
            Check(d && !d->f.disengageFlee && ScoreOne(*d, Mind{}, Profile{}, t, 1, 0, nullptr) > 0.0f, "G3: sneak class + road + point + found -> a hide Disengage that scores above 0");
            Snapshot a = make(180, Tri::Yes, false); a.concealRoad = false;
            Check(count(Generate(a, thief, t), Family::Disengage) == 0, "G3: no road (switch off / Harbinger absent) -> no Disengage");
            Snapshot b = make(180, Tri::Yes, false); b.concealPoint = false;
            Check(count(Generate(b, thief, t), Family::Disengage) == 0, "G3: no fresh point -> no Disengage");
            Check(count(Generate(make(180, Tri::No, false), thief, t), Family::Disengage) == 0, "G3: no foe has found him -> no hide run");
            Check(count(Generate(make(180, Tri::Yes, false), fighter, t), Family::Disengage) == 0, "G3: a non-sneak class at full health is never sent to hide");
            Candidate cd = *d; cd.f.hardCooldownLeft = 10.0f;
            Check(ScoreOne(cd, Mind{}, Profile{}, t, 1, 0, nullptr) == 0.0f, "G3: the hard cooldown (counted from the run's END) vetoes it");
            // need = the share of the counted foes that have found him
            Snapshot two = make(180, Tri::Yes, false);
            two.foes.push_back(Actor("Archer", 0x3B611, 100, 100, 700, 0)); two.foes[1].detectsMe = Tri::No;
            const Candidate* d2 = find(Generate(two, thief, t), Family::Disengage);
            Check(d2 && d2->f.detectShare == 0.5f && ScoreOne(*d2, Mind{}, Profile{}, t, 1, 0, nullptr) < ScoreOne(*d, Mind{}, Profile{}, t, 1, 0, nullptr),
                  "G3: one of two foes has found him -> half the need, a lower score than all of them");
            ClassGate noDis = thief; noDis.disengage = false;
            Check(count(Generate(make(180, Tri::Yes, false), noDis, t), Family::Disengage) == 0, "G3: a sneak class without kDisengage never hides");
        }
        // ---- the flee (a non-sneak class): hurt AND a foe on him, class tree ON only ----
        {
            const Generated g = Generate(make(50, Tri::Unknown, true), fighter, t);   // 25% of 200
            const Candidate* d = find(g, Family::Disengage);
            Check(d && d->f.disengageFlee && ScoreOne(*d, Mind{}, Profile{}, t, 1, 0, nullptr) > 0.0f, "G3: non-sneak, hurt under the retreat line, a foe on him -> a flee Disengage that scores above 0");
            Check(count(Generate(make(120, Tri::Unknown, true), fighter, t), Family::Disengage) == 0, "G3: 60% health -> no flee (it matters only under the retreat's health line)");
            Check(count(Generate(make(50, Tri::Unknown, false), fighter, t), Family::Disengage) == 0, "G3: hurt but no foe on him -> no flee");
            ClassGate treeOff = fighter; treeOff.treeOn = false;
            Check(count(Generate(make(50, Tri::Unknown, true), treeOff, t), Family::Disengage) == 0, "G3 MB3: class tree OFF -> no flee (and no hide run: sneakClass is false then)");
            Check(count(Generate(make(50, Tri::Unknown, true), thief, t), Family::Disengage) == 0, "G3: a sneak class that no foe has found does not flee, however hurt (it hides, or fights)");
            Candidate hiCd = *d; hiCd.f.selfHp = 0.9f;
            Check(ScoreOne(hiCd, Mind{}, Profile{}, t, 1, 0, nullptr) == 0.0f, "G3: the flee's need is RetreatDanger: nothing at 90% health");
        }
        // ---- pacing while the flee runs ----
        {
            Snapshot s = make(50, Tri::Unknown, true);
            s.book.kit.potionsHealth = 2;
            s.self.mp = 100; s.self.mpMax = 100;
            s.book.entries = { Spell("Firebolt", 0x12FCD, Prune::Nature::Offense, 40, 2000, Prune::kAvResistFire) };
            const Generated before = Generate(s, fighter, t);
            Check(count(before, Family::Attack) > 0 && count(before, Family::CastOffense) > 0, "G3: before the flee, offense is generated");
            const float drinkBefore = [&] { const Candidate* c = find(before, Family::DrinkHealth); return c ? ScoreOne(*c, Mind{}, Profile{}, t, 1, 0, nullptr) : -1.0f; }();
            s.fleeRun = true;
            const Generated during = Generate(s, fighter, t);
            Check(count(during, Family::Attack) == 0 && count(during, Family::PowerAttack) == 0 && count(during, Family::CastOffense) == 0 &&
                      count(during, Family::Taunt) == 0 && count(during, Family::Retreat) == 0 && count(during, Family::Disengage) == 0,
                  "G3: while the flee runs no attack, power attack, offense cast, taunt, retreat or second disengage is generated");
            const Candidate* dr = find(during, Family::DrinkHealth);
            const float drinkDuring = dr ? ScoreOne(*dr, Mind{}, Profile{}, t, 1, 0, nullptr) : -1.0f;
            Check(dr && dr->f.fleeHeal && drinkBefore > 0.0f && drinkDuring > drinkBefore, "G3: a health potion is boosted while he flees (heal as soon as possible)");
            ClassGate treeOffGate = fighter; treeOffGate.treeOn = false;
            Check(count(Generate(s, treeOffGate, t), Family::Disengage) == 0, "G3: no second flee while one runs");
        }
        // ---- a heal cast for himself is boosted, a heal for another is not ----
        {
            Snapshot s = make(50, Tri::Unknown, true);
            s.self.mp = 100; s.self.mpMax = 100;
            s.book.entries = { Spell("Healing", 0x12FD0, Prune::Nature::Heal, 40, 0, -1) };
            s.fleeRun = true;
            const Generated g = Generate(s, fighter, t);
            const Candidate* h = find(g, Family::CastHeal);
            Check(h && h->f.fleeHeal, "G3: a heal cast on himself carries the flee boost");
        }
    }
}

namespace {
    // Batch I (T2): block as a candidate, unweighted spells, the spell range, the back-off, the hidden step, the free entry's read.
    void BatchI() {
        std::printf("\n== Batch I: block, unweighted spells, H4 range, back-off, hidden step ==\n");
        const Tuning& t = DefaultTuning();
        const auto count = [](const Generated& g, Family f) {
            int n = 0;
            for (const Candidate& c : g.actions) if (c.family == f) ++n;
            return n;
        };
        const auto find = [](const Generated& g, Family f) -> const Candidate* {
            for (const Candidate& c : g.actions) if (c.family == f) return &c;
            return nullptr;
        };
        const auto score = [&](const Generated& g, Family f) {
            for (const Candidate& c : g.actions)
                if (c.family == f) return ScoreOne(c, Mind{}, Profile{}, t, 1, 0, nullptr);
            return -1.0f;
        };
        // ---- item 6: effects IAUS cannot weight ----
        Check(Prune::EffectWeighable(0) && Prune::EffectWeighable(4) && Prune::EffectWeighable(21) && Prune::EffectWeighable(46),
              "item 6: value modifier, absorb, paralysis, the last known archetype are weighable");
        Check(!Prune::EffectWeighable(1) && !Prune::EffectWeighable(43) && !Prune::EffectWeighable(-1) && !Prune::EffectWeighable(47) && !Prune::EffectWeighable(500),
              "item 6: a Script effect, a scripted spawn, kNone and an archetype past the last known are not");
        Check(Prune::WeighableSpell({ 1, 0 }) && !Prune::WeighableSpell({ 1, 43 }) && !Prune::WeighableSpell({}),
              "item 6: one weighable effect makes the spell weighable; script-only and an empty list do not");
        {
            Prune::StaticIn in; in.nature = Prune::Nature::Offense; in.weighable = false;
            Check(Prune::StaticDrop(in) == Prune::Drop::Unweighted, "item 6: a stated nature with nothing to weigh is dropped as Unweighted");
            in.weighable = true;
            Check(Prune::StaticDrop(in) == Prune::Drop::Keep, "item 6: weighable -> kept");
            in.nature = Prune::Nature::Unknown; in.weighable = false;
            Check(Prune::StaticDrop(in) == Prune::Drop::UnknownNature, "item 6: an unknown nature keeps its own reason");
        }
        {   // MFO-B865: the Wall shape rule. Firebolt / Flames / Fireball carry an impact hazard that does NOT drop to the ground
            Check(!Prune::IsWallShape(false, false, false), "B865: a bolt whose impact hazard does not drop to the ground is not a wall (kept)");
            Check(!Prune::IsWallShape(false, true, false), "B865: a stream with no drop-to-ground hazard is not a wall");
            Check(!Prune::IsWallShape(true, false, false), "B865: a fire-and-forget spell with a drop hazard is not a wall");
            Check(Prune::IsWallShape(true, true, false), "B865: a concentration stream whose impact hazard drops to the ground stays a Wall");
            Check(!Prune::IsWallShape(true, true, true), "B865: a Self concentration spell is not a wall");
            Prune::StaticIn in; in.nature = Prune::Nature::Offense; in.weighable = true;
            in.targetLocation = Prune::IsWallShape(true, true, false);
            Check(Prune::StaticDrop(in) == Prune::Drop::Placement, "B865: a real wall is still dropped as Placement (unchanged)");
            in.targetLocation = Prune::IsWallShape(false, false, false);
            Check(Prune::StaticDrop(in) == Prune::Drop::Keep, "B865: a bolt with an impact hazard is kept");
            in.targetLocation = true;   // a rune: delivery TargetLocation, not read from the impact hazard
            Check(Prune::StaticDrop(in) == Prune::Drop::Placement, "B865: a rune is unchanged");
            Check(Prune::IsSelfBlastPlacement(true, false, true, true), "B865 F1: a Self FF Offense spell with an impact hazard (Fire Storm) is still dropped (interim)");
            Check(!Prune::IsSelfBlastPlacement(true, false, true, false), "B865 F1: a Self blast with no impact hazard is not covered by the interim drop");
            Check(!Prune::IsSelfBlastPlacement(true, true, true, true), "B865 F1: a Self concentration spell is not covered");
            Check(!Prune::IsSelfBlastPlacement(true, false, false, true), "B865 F1: a non-Offense Self spell is not dropped");
            Check(!Prune::IsSelfBlastPlacement(false, false, true, true), "B865 F1: an aimed bolt with an impact hazard is kept");
            in.targetLocation = Prune::IsSelfBlastPlacement(true, false, true, true);
            Check(Prune::StaticDrop(in) == Prune::Drop::Placement, "B865 F1: the Self blast is dropped as Placement");
        }
        // ---- item 4: the block family ----
        ClassGate knight; knight.resolved = true; knight.treeOn = true; knight.block = true;
        ClassGate warrior; warrior.resolved = true; warrior.treeOn = true;
        const auto melee = [&]() {
            Snapshot s;
            s.self = Actor("Knight", 0x14F01, 100, 200, 0, 0);
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, 150, 0) };
            s.foes[0].targetId = 0x14F01; s.foes[0].swinging = true;
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
            s.blockRoad = true;
            return s;
        };
        Check(count(Generate(melee(), knight, t), Family::Block) == 1, "item 4: a class with the Block row + the road + a swing in the air -> a Block candidate");
        Check(count(Generate(melee(), warrior, t), Family::Block) == 0, "item 4: a class with no Block row is never given one (Q-melee-2)");
        { Snapshot s = melee(); s.blockRoad = false; Check(count(Generate(s, knight, t), Family::Block) == 0, "item 4: no road -> no Block"); }
        { Snapshot s = melee(); s.foes[0].swinging = false; Check(count(Generate(s, knight, t), Family::Block) == 0, "item 4: no swing in the air -> no Block"); }
        {   // Q-S-5: the guard line
            Snapshot s = melee(); s.foes[0].swinging = false; s.foes[0].taunted = true;
            s.foes.push_back(Actor("Bandit2", 0x3B611, 100, 100, 160, 0)); s.foes[1].targetId = 0x14F01; s.foes[1].taunted = true; s.foes[0].tauntedByMe = s.foes[1].tauntedByMe = true;
            const Generated g = Generate(s, knight, t);
            const Candidate* b = find(g, Family::Block);
            Check(b && b->f.tauntedOnMe == 2 && !b->f.foeSwinging && score(g, Family::Block) > score(g, Family::Attack),
                  "Q-S-5: two taunted foes on him, one in striking distance, no swing seen -> a Block that outranks the attack");
            s.foes[0].tauntedByMe = s.foes[1].tauntedByMe = true;
            const Generated g2 = Generate(s, knight, t);
            Check(count(g2, Family::Block) == 1, "Q-S-5: counted by his OWN taunts (tauntedByMe)");
            { Snapshot o = s; o.foes[0].tauntedByMe = o.foes[1].tauntedByMe = false;
              Check(count(Generate(o, knight, t), Family::Block) == 0, "Q-S-5: foes pinned by another taunter do not make the guard line"); }
            { Snapshot h = s; h.self.hp = 30; h.self.hpMax = 200; h.book.kit.potionsHealth = 2;
              Check(count(Generate(h, knight, t), Family::Block) == 0, "Q-S-5: hurt (below the drink line) with a health potion -> no guard line, he drinks first");
              h.book.kit.potionsHealth = 0;
              Check(count(Generate(h, knight, t), Family::Block) == 1, "Q-S-5: hurt with NO potion -> the guard line stands"); }
            {   // per-class curves: the Knight's own drink line decides it (hp 0.5, a health potion, two foes taunted by him)
              Snapshot k = s; k.self.hp = 100; k.self.hpMax = 200; k.book.kit.potionsHealth = 1;
              ClassGate kd = knight, kh = knight; kh.drinkHealthLine = 0.80f;
              Check(count(Generate(k, kd, t), Family::Block) == 1, "per-class curves: hp 0.5 above the default 0.34 line -> the guard line stands");
              Check(count(Generate(k, kh, t), Family::Block) == 0, "per-class curves: the same Knight with his line at 0.80 -> the guard line is off, he drinks first"); }
            s.foes[1].taunted = false; s.foes[1].tauntedByMe = false;
            Check(count(Generate(s, knight, t), Family::Block) == 0, "Q-S-5: one taunted foe and no swing in the air -> no Block");
            s.foes[1].taunted = s.foes[1].tauntedByMe = true; s.guardHeld = true;
            const Candidate* r = find(Generate(s, knight, t), Family::Block);
            Check(r && r->f.hardCooldownSec == 0.0f, "Q-S-5: while the guard stands the renewal carries no row cooldown");
        }
        {   // the held-guard gate (marth Q-melee-3)
            Snapshot s = melee(); s.guardHeld = true;
            const Generated g = Generate(s, knight, t);
            Check(count(g, Family::Attack) == 0 && count(g, Family::PowerAttack) == 0 && count(g, Family::Taunt) == 0 && count(g, Family::Bash) == 0,
                  "item 4: while the guard stands no attack, power attack, taunt or bash is generated");
            Check(count(g, Family::Block) == 1, "item 4: the Block row stays generated while the guard stands (the renewal)");
        }
        {   // the row's own cooldown
            ClassGate k = knight; k.blockCooldownSec = 4.0f;
            const Candidate* b = find(Generate(melee(), k, t), Family::Block);
            Check(b && b->f.hardCooldownSec == 4.0f, "item 4: the class row's cooldown is the candidate's hard cooldown");
        }
        // ---- item 1: the spell range ----
        SpellEntry firebolt = Spell("Firebolt", 0x12FCD, Prune::Nature::Offense, 40, 2000, Prune::kAvResistFire);
        SpellEntry flames = Spell("Flames", 0x12FCE, Prune::Nature::Offense, 5, 300, Prune::kAvResistFire); flames.concentration = true;   // reach under StrikeDistance (400)
        SpellEntry frostbite = Spell("Frostbite", 0x12FCF + 0x20, Prune::Nature::Offense, 5, 600, Prune::kAvResistFrost); frostbite.concentration = true;   // reach past it (marth 11-13, aaacee6e)
        SpellEntry touch = Spell("Touch", 0x12FCF, Prune::Nature::Offense, 30, 0, Prune::kAvResistFire); touch.touch = true;
        SpellEntry blast = Spell("Blast", 0x12FD1, Prune::Nature::Offense, 30, 0, Prune::kAvResistFire); blast.selfDelivery = true; blast.areaUnits = 500;
        const Reposition::Range rf = Reposition::SpellRange(firebolt, 200.0f);
        Check(std::fabs(rf.desired - 1800.0f) < 0.01f && std::fabs(rf.minimum - 720.0f) < 0.01f, "H4: aimed = the furthest range x0.9, minimum 0.4 of that");
        const Reposition::Range rc = Reposition::SpellRange(flames, 200.0f);
        Check(std::fabs(rc.desired - 240.0f) < 0.01f && rc.minimum == 0.0f, "H4: a short concentration (cone) spell (reach under the strike distance) = its range x0.8, no minimum");
        const Reposition::Range rfb = Reposition::SpellRange(frostbite, 200.0f);
        Check(std::fabs(rfb.desired - 480.0f) < 0.01f && std::fabs(rfb.minimum - 192.0f) < 0.01f, "H4: a concentration spell that out-ranges melee (Frostbite) = its range x0.8 with the 0.4 minimum");
        const Reposition::Range rt = Reposition::SpellRange(touch, 200.0f);
        Check(std::fabs(rt.desired - 160.0f) < 0.01f && rt.minimum == 0.0f, "H4: touch = the melee reach x0.8, no minimum");
        Check(std::fabs(Reposition::SpellRange(blast, 200.0f).desired - 400.0f) < 0.01f, "H4: a Self-delivered blast = its area x0.8");
        SpellEntry none = Spell("None", 0x12FD2, Prune::Nature::Offense, 30, 0, Prune::kAvResistFire);
        Check(!Reposition::SpellRange(none, 200.0f).Known(), "H4: a spell whose record gives no range keeps none");
        ClassGate mage; mage.resolved = true; mage.treeOn = true; mage.spellsOnClass = true;
        const auto caster = [&](float dist, const SpellEntry& sp) {
            Snapshot s;
            s.self = Actor("Mage", 0x14F01, 150, 200, 0, 0);
            s.self.mp = 200; s.self.mpMax = 200;
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, dist, 0) };
            s.foes[0].targetId = 0x14F01;
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::None; s.meleeReach = 200.0f;
            s.book.entries = { sp };
            s.equippedSpell = sp.form;
            return s;
        };
        {
            Reposition::Close c;
            Check(Reposition::RangeToHold(caster(3000, firebolt), mage, c) && c.far && c.foe == 0x3B610 && std::fabs(c.radius - 1800.0f) < 0.01f,
                  "H4: the foe beyond the range x1.15 -> close, R = the derived range");
            Check(Reposition::RangeToHold(caster(1500, firebolt), mage, c) && !c.far, "H4: inside the range the bound is refreshed but nothing new is filed");
            Snapshot s = caster(3000, firebolt); s.drawn = PostureKind::Melee;
            Check(!Reposition::RangeToHold(s, mage, c), "H4: with a weapon drawn it is not the spell phase");
            Snapshot s2 = caster(3000, firebolt); s2.equippedSpell = 0;
            Check(!Reposition::RangeToHold(s2, mage, c), "H4: no spell in his hand -> no range to keep");
            ClassGate noCast = mage; noCast.spellsOnClass = false;
            Check(!Reposition::RangeToHold(caster(3000, firebolt), noCast, c), "H4: a class that does not cast never closes on a spell range");
            // Q-I-2: the shortest range of one delivery type over BOTH hands; touch + distance = the distance range unless touch is the better cast
            Snapshot two = caster(3000, firebolt); two.book.entries = { firebolt, flames }; two.equippedSpellOther = flames.form;
            Check(Reposition::RangeToHold(two, mage, c) && std::fabs(c.radius - 240.0f) < 0.01f, "Q-I-2: two distance spells (aimed 1800, stream 240) -> the shortest range");
            Snapshot mix = caster(3000, firebolt); mix.book.entries = { firebolt, touch }; mix.equippedSpellOther = touch.form;
            Check(Reposition::RangeToHold(mix, mage, c) && std::fabs(c.radius - 1800.0f) < 0.01f, "Q-I-2: touch + distance, the touch spell not the chosen cast -> the distance spell's range");
            mix.chosenCast = touch.form;
            Check(Reposition::RangeToHold(mix, mage, c) && std::fabs(c.radius - 160.0f) < 0.01f && c.far, "Q-I-2: the touch spell is the cast IAUS chose -> he closes for the touch range");
            mix.touchSpent = true;
            Check(Reposition::RangeToHold(mix, mage, c) && std::fabs(c.radius - 1800.0f) < 0.01f, "Q-I-2: right after the touch cast the range is the distance spell's again");
            mix.foes[0].dist = 100.0f; mix.foes[0].targetId = 0;   // the foe he just touched does not even have him as its target
            Check(Reposition::BackOffNeed(mix, mage) > 0.5f, "Q-I-2: after the touch cast a foe inside the distance spell's minimum is a back-off reason, aimed at him or not");
            mix.touchSpent = false;
            Check(Reposition::BackOffNeed(mix, mage) == 0.0f, "Q-I-2: with the touch cast still wanted, and the foe not on him, there is no back-off");
        }
        // ---- items 1 and 3: the back-off ----
        {
            Snapshot s = caster(300, firebolt); s.backOffRoad = true; s.backOffPoint = true;
            const Generated g = Generate(s, mage, t);
            const Candidate* b = find(g, Family::BackOff);
            Check(b && b->form == Reposition::kBackOffForm && std::fabs(b->f.backOffNeed - (1.0f - 300.0f / 720.0f)) < 0.001f && score(g, Family::BackOff) > 0.0f,
                  "H4: a melee foe on a mage inside 0.4 of the spell's range -> a BackOff candidate whose need is how far inside");
            Snapshot far = caster(900, firebolt); far.backOffRoad = true; far.backOffPoint = true;
            Check(count(Generate(far, mage, t), Family::BackOff) == 0, "H4: outside the minimum -> no back-off");
            Snapshot noRoad = caster(300, firebolt); noRoad.backOffRoad = true; noRoad.backOffPoint = false;
            Check(count(Generate(noRoad, mage, t), Family::BackOff) == 0, "H4: no fresh point -> no back-off");
            Snapshot cone = caster(100, flames); cone.backOffRoad = true; cone.backOffPoint = true;
            Check(count(Generate(cone, mage, t), Family::BackOff) == 0, "H4: a cone / touch spell wants the foe near: no minimum, no back-off");
            Snapshot rangedFoe = caster(300, firebolt); rangedFoe.backOffRoad = true; rangedFoe.backOffPoint = true; rangedFoe.foes[0].rangedWeapon = true;
            Check(count(Generate(rangedFoe, mage, t), Family::BackOff) == 0, "H4: a foe that does not have to close does not push him back");
            Snapshot held = caster(300, firebolt); held.backOffRoad = true; held.backOffPoint = true; held.guardHeld = true;
            Check(count(Generate(held, mage, t), Family::BackOff) == 0, "item 4 x H4: not while a guard stands");
        }
        ClassGate merc; merc.resolved = true; merc.treeOn = true; merc.backOffPressed = true;
        const auto pressed = [&](int foes) {
            Snapshot s;
            s.self = Actor("Mercenary", 0x14F01, 150, 200, 0, 0);
            s.playerId = 0x14;
            for (int i = 0; i < foes; ++i) {
                s.foes.push_back(Actor("Bandit", 0x3B610u + static_cast<std::uint32_t>(i), 100, 100, 120.0f + 20.0f * static_cast<float>(i), 0));
                s.foes.back().targetId = 0x14F01;
            }
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = PostureKind::Melee; s.meleeReach = 200.0f;
            s.backOffRoad = true; s.backOffPoint = true;
            return s;
        };
        {
            Check(count(Generate(pressed(1), merc, t), Family::BackOff) == 0, "Q-T2-3: one foe on the Mercenary is a duel: no back-off");
            const Candidate* two = find(Generate(pressed(2), merc, t), Family::BackOff);
            Check(two && std::fabs(two->f.backOffNeed - 0.6f) < 0.001f, "Q-T2-3: two foes press the Mercenary -> a BackOff candidate, need 0.6");
            const Candidate* four = find(Generate(pressed(4), merc, t), Family::BackOff);
            Check(four && std::fabs(four->f.backOffNeed - 1.0f) < 0.001f, "Q-T2-3: four or more -> need 1.0");
            ClassGate knightLike; knightLike.resolved = true; knightLike.treeOn = true;
            Check(count(Generate(pressed(3), knightLike, t), Family::BackOff) == 0, "Q-T2-3: Knight and Warrior keep no way out by design");
            Snapshot far = pressed(2); far.foes[0].dist = 900; far.foes[1].dist = 950;
            Check(count(Generate(far, merc, t), Family::BackOff) == 0, "Q-T2-3: foes that are not within striking distance do not press him");
        }
        // ---- item 5: the hidden step ----
        ClassGate thief; thief.resolved = true; thief.treeOn = true; thief.sneakClass = true; thief.disengage = true;
        const auto hidden = [&](float dist, PostureKind pk) {
            Snapshot s;
            s.self = Actor("Thief", 0x14F01, 180, 200, 0, 0);
            s.playerId = 0x14;
            s.foes = { Actor("Bandit", 0x3B610, 100, 100, dist, 0) };
            s.foes[0].detectsMe = Tri::No;
            s.currentTargetId = 0x3B610; s.hasBook = true; s.combatSec = 3.0f;
            s.drawn = pk; s.meleeReach = 200.0f;
            s.selfSneaking = true; s.hideRoad = true;
            s.stepRoad = true; s.stepPoint = true;
            return s;
        };
        {
            const Generated g = Generate(hidden(900, PostureKind::Melee), thief, t);
            Check(count(g, Family::Step) == 1 && count(g, Family::Sneak) == 1, "G18: hidden, the blade foe out of reach, a point known -> a Step beside the hold row");
            const Candidate* st = find(g, Family::Step);
            Check(st && st->form == Reposition::kStepForm && st->target == 0x3B610, "G18: the step is for that foe and carries its kind");
            Check(score(g, Family::Step) > score(g, Family::Sneak) && score(g, Family::Sneak) > 0.0f, "G18: the step outranks the hold row it replaces");
            Check(count(Generate(hidden(150, PostureKind::Melee), thief, t), Family::Step) == 0, "G18: a blow is ready (foe in reach) -> the blow, not a step");
            Snapshot noPt = hidden(900, PostureKind::Melee); noPt.stepPoint = false;
            Check(count(Generate(noPt, thief, t), Family::Step) == 0 && count(Generate(noPt, thief, t), Family::Sneak) == 1, "G18: no fresh point -> the hold row stands still, as before");
            Snapshot seen = hidden(900, PostureKind::Melee); seen.foes[0].detectsMe = Tri::Yes;
            Check(count(Generate(seen, thief, t), Family::Step) == 0, "G18: a foe has him -> no step (the hide run is for that)");
            Snapshot unk = hidden(900, PostureKind::Melee); unk.foes[0].detectsMe = Tri::Unknown;
            Check(count(Generate(unk, thief, t), Family::Step) == 0, "G18: no verdict from the engine -> no step (not read as hidden)");
            Check(count(Generate(hidden(900, PostureKind::None), thief, t), Family::Step) == 0, "G18: a caster's hands never step to strike");
            Snapshot bow = hidden(1500, PostureKind::Bow); bow.foes[0].sight = Sight::Occluded;
            Check(count(Generate(bow, thief, t), Family::Step) == 1, "G18: a bow with no line of fire on the unaware foe -> a Step");
            Snapshot bowLine = hidden(1500, PostureKind::Bow); bowLine.foes[0].sight = Sight::Visible;
            Check(count(Generate(bowLine, thief, t), Family::Step) == 0, "G18: a bow with a line of fire is a blow, not a step");
            Snapshot cd = hidden(900, PostureKind::Melee);
            const Generated gc = Generate(cd, thief, t);
            const Candidate* sc = find(gc, Family::Step);
            Check(sc && sc->f.hardCooldownSec == t.stepCooldownSec, "G18: the step carries its hard cooldown");
        }
    }

    // Per-class curves (ruling Q-class-curves): the shipped table overrides nothing (every class scores bit-identically to the
    // uncustomised catalogue), and an override changes ONLY the (family, consideration) it names, ONLY for the Profile that holds it.
    void ClassCurveRows() {
        namespace CC = MFO::Iaus::ClassCurves;
        std::printf("\n=== per-class curves ===\n");
        const Tuning& t = DefaultTuning();
        const Candidate drink = Drink(Family::DrinkHealth, "Health Potion", 0x0003EADD, 3, 0.45f, 1.0f, 1.0f);
        const Candidate atk = Drink(Family::DrinkStamina, "Stamina Potion", 0x0003EADF, 3, 1.0f, 0.45f, 1.0f);   // a second family that reads the SAME Cons (DrinkNeed)
        const Mind m{};
        const Profile none;
        const float d0 = ScoreOne(drink, m, none, t, 3, 0.0f, nullptr), a0 = ScoreOne(atk, m, none, t, 3, 0.0f, nullptr);
        Check(d0 > 0.0f && a0 > 0.0f, "class curves: the probe candidates score above 0");
        // The shipped table after the sim batch (2026-11-12): the edge-number search found no curve override that passed every gate, so
        // every row still has none (the school shape is a Scalars value, checked below) and scores these probes bit-identically.
        // Batch 3 (iaus-sim-v2.md 35) wrote POSTURE values into rows 1 (ranged), 6 (archer), 8 (evoker), 10 (healer): those rows carry only
        // Family::Posture overrides / weights (the drink and attack probes below are untouched by them); every other row still has none.
        bool same = true, empty = true, postureOnly = true;
        for (std::size_t i = 0; i < CC::kRows; ++i) {
            const Profile& p = CC::ProfileFor(static_cast<std::uint8_t>(i));
            if (i == 1 || i == 6 || i == 8 || i == 10) {
                postureOnly = postureOnly && !(p.curves.empty() && p.weights.empty());
                for (const auto& o : p.curves) postureOnly = postureOnly && o.family == Family::Posture;
                for (const auto& o : p.weights) postureOnly = postureOnly && o.family == Family::Posture;
            } else
            empty = empty && p.curves.empty() && p.weights.empty();
            for (const float fw : p.family) empty = empty && fw == 1.0f;
            same = same && ScoreOne(drink, m, p, t, 3, 0.0f, nullptr) == d0 && ScoreOne(atk, m, p, t, 3, 0.0f, nullptr) == a0;
        }
        Check(empty && same, "class curves: every shipped row but the batch 3 posture rows has no curve override, and every row scores the drink probes bit-identically to the default Profile");
        Check(postureOnly, "class curves: the batch 3 rows (ranged, archer, evoker, healer) carry posture overrides only");
        Check(&CC::ProfileFor(0xFF) != nullptr && CC::ProfileFor(0xFF).curves.empty(), "class curves: no resolved class = the empty default Profile");
        Check(&CC::ProfileFor(3) == &CC::ProfileFor(3), "class curves: a class's Profile is cached (the same object every call)");
        ClassGate g; CC::Apply(3, g);
        Check(g.curveClass == 3 && g.drinkHealthLine == kDrinkHealthLine && g.touchSpentSec == Reposition::kTouchSpentSec, "class curves: a shipped row's edge constants are the shared constants");
        // an override equal to the shared curve changes nothing
        CC::Row same1;
        Check(CC::SetCurveParam(same1, Family::DrinkHealth, Cons::DrinkNeed, t.curve[static_cast<std::size_t>(Cons::DrinkNeed)], "a", t.curve[static_cast<std::size_t>(Cons::DrinkNeed)].a), "class curves: SetCurveParam creates an override");
        Check(ScoreOne(drink, m, CC::Build(same1), t, 3, 0.0f, nullptr) == d0, "class curves: an override equal to the shared curve scores bit-identically");
        // a different shape changes that family's score only
        CC::Row knight;
        CC::SetCurveParam(knight, Family::DrinkHealth, Cons::DrinkNeed, t.curve[static_cast<std::size_t>(Cons::DrinkNeed)], "b", 0.60f);   // the midpoint moves up: he drinks sooner
        const Profile pk = CC::Build(knight);
        const float dk = ScoreOne(drink, m, pk, t, 3, 0.0f, nullptr);
        Check(dk > d0, "class curves: a DrinkNeed midpoint override changes the drink's score (up for a later midpoint)");
        Check(ScoreOne(atk, m, pk, t, 3, 0.0f, nullptr) == a0, "class curves: ...and no other family's score (a DrinkStamina candidate reads the same DrinkNeed Cons)");
        Check(ScoreOne(drink, m, none, t, 3, 0.0f, nullptr) == d0 && ScoreOne(drink, m, CC::ProfileFor(3), t, 3, 0.0f, nullptr) == d0, "class curves: ...and no other Profile's (the override lives in that Profile only)");
        Check(!CC::SetCurveParam(knight, Family::Attack, Cons::DrinkNeed, t.curve[0], "nope", 1.0f) && !CC::SetScalar(knight, "nope", 1.0f), "class curves: a bad param name is refused");
        // the edge constant reaches the generator: a Knight below the (raised) line with a potion drops the guard line
        CC::Row line; CC::SetScalar(line, "drinkHealthLine", 0.80f);
        ClassGate gl; CC::Apply(line, 5, gl);
        Check(gl.curveClass == 5 && gl.drinkHealthLine == 0.80f, "class curves: an edited row's edge constant lands on the gate");
        // the tree-OFF base classes have their own rows, seeded from the T1 rows
        Check(CC::kRows == CC::kClassRows + 3 && CC::Defaults()[CC::kBaseOffFirst].curves.empty() && &CC::ProfileFor(static_cast<std::uint8_t>(CC::kBaseOffFirst)) != &CC::ProfileFor(0),
              "class curves: the three tree-OFF base classes have their own rows (35..37), apart from the T1 rows");
        // the guard-line cut-off follows the row's own DrinkNeed midpoint unless a value is set
        CC::Row mid; CC::SetCurveParam(mid, Family::DrinkHealth, Cons::DrinkNeed, t.curve[static_cast<std::size_t>(Cons::DrinkNeed)], "b", 0.55f);
        ClassGate gm; CC::Apply(mid, 4, gm);
        Check(gm.drinkHealthLine == 0.55f, "class curves: with no explicit scalar the edge line follows the row's DrinkNeed midpoint");
        CC::SetScalar(mid, "drinkHealthLine", 0.20f); CC::Apply(mid, 4, gm);
        Check(gm.drinkHealthLine == 0.20f, "class curves: an explicit drinkHealthLine wins over the midpoint");
        // per-class weights (card I8's shape, filled by the tuning): a family multiplier and a consideration exponent, this class only
        CC::Row fam1; Check(CC::SetFamilyWeight(fam1, Family::DrinkHealth, 1.0f) && ScoreOne(drink, m, CC::Build(fam1), t, 3, 0.0f, nullptr) == d0, "class weights: a family weight of 1.0 scores bit-identically");
        CC::Row fam2; CC::SetFamilyWeight(fam2, Family::DrinkHealth, 1.25f);
        Check(ScoreOne(drink, m, CC::Build(fam2), t, 3, 0.0f, nullptr) > d0 && ScoreOne(atk, m, CC::Build(fam2), t, 3, 0.0f, nullptr) == a0, "class weights: a family weight moves that family's score and no other's");
        Check(!CC::SetFamilyWeight(fam2, Family::DrinkHealth, 0.0f) && !CC::SetFamilyWeight(fam2, Family::DrinkHealth, -1.0f) && !CC::SetFamilyWeight(fam2, Family::DrinkHealth, std::numeric_limits<float>::quiet_NaN()) && !CC::SetFamilyWeight(fam2, static_cast<Family>(200), 1.0f),
              "class weights: a family weight is > 0 and finite (a class weight never switches a family off), a bad family is refused");
        CC::Row cw; Check(CC::SetWeight(cw, Family::DrinkHealth, Cons::DrinkNeed, 2.0f), "class weights: SetWeight takes a consideration exponent");
        Check(ScoreOne(drink, m, CC::Build(cw), t, 3, 0.0f, nullptr) < d0 && ScoreOne(atk, m, CC::Build(cw), t, 3, 0.0f, nullptr) == a0, "class weights: a larger exponent lowers that family's term (the raw value is under 1) and no other family's");
        Check(!CC::SetWeight(cw, Family::DrinkHealth, Cons::DrinkNeed, 0.0f) && !CC::SetWeight(cw, Family::DrinkHealth, Cons::kCount, 1.0f), "class weights: a consideration exponent is > 0 and the pair must exist");
        // Piece points: one point's x or y; only on a Piece curve, only a point it has, the x order kept
        const std::array<std::array<float, 2>, 3> pts = { { { 0.0f, 0.2f }, { 0.5f, 0.9f }, { 1.0f, 1.0f } } };
        const Curve piece = Piece(pts);
        CC::Row pr; Check(CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "py1", 0.6f) && pr.curves.size() == 1 && pr.curves[0].curve.py[1] == 0.6f && pr.curves[0].curve.py[0] == 0.2f && Eval(pr.curves[0].curve, 0.5f) == 0.6f,
              "class curves: a Piece point's y is a per-class knob (py<i>)");
        Check(CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "px1", 0.25f) && pr.curves[0].curve.px[1] == 0.25f, "class curves: a Piece point's x moves between its neighbours (px<i>)");
        Check(!CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "px1", 1.5f) && !CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "px1", -0.1f) && !CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "py3", 0.5f)
              && !CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "px1", 0.0f) && !CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "px1", 1.0f) && pr.curves[0].curve.px[1] == 0.25f   /* review F7: an x equal to a neighbour is refused */
              && !CC::SetCurveParam(pr, Family::Attack, Cons::Los, piece, "py0", std::numeric_limits<float>::infinity()), "class curves: a Piece x past or ON a neighbour, a point the curve does not have and a non-finite value are refused");
        CC::Row np; Check(!CC::SetCurveParam(np, Family::DrinkHealth, Cons::DrinkNeed, t.curve[static_cast<std::size_t>(Cons::DrinkNeed)], "py0", 0.5f) && np.curves.empty(), "class curves: a point edit on a curve that is not a Piece is refused and creates no override");
    }

    // School affinity (ruling Q-school-affinity): a smooth wave over the school circle, a multiplier only.
    // ── spell perks (ClickUp 1dfcuabd2pk): Snapshot::PerkRead's spell group -> Facts::spellPerkEdge -> Cons::BoostSpellPerks ──
    void SpellPerkRows() {
        std::printf("\n=== spell perks (perk-effect reader) ===\n");
        const Tuning& t = DefaultTuning();
        // The pure input rule.
        PerkRead p;
        Check(PerkSpellEdge(p, 0) < 0.0f, "spell perks: an unread group is not known (edge < 0)");
        p.spellsRead = true;
        Check(PerkSpellEdge(p, 0) < 0.0f, "spell perks: a school with no spell asked is not known");
        p.spellCount[0] = 2;
        Check(PerkSpellEdge(p, 0) == 0.0f, "spell perks: no perk in the school = edge 0");
        p.spellCost[0] = 0.5f;
        Check(std::fabs(PerkSpellEdge(p, 0) - 0.5f) < 1e-6f, "spell perks: half cost = edge 0.5");
        p.spellMagnitude[0] = 2.0f;
        Check(std::fabs(PerkSpellEdge(p, 0) - 0.75f) < 1e-6f, "spell perks: half cost and double magnitude = edge 0.75");
        p.spellCost[0] = 0.0f;
        Check(PerkSpellEdge(p, 0) < 1.0f && PerkSpellEdge(p, 0) > 0.9f, "spell perks: a free spell is floored, the edge never reaches 1");
        p.spellCost[0] = 2.0f; p.spellMagnitude[0] = 1.0f;
        Check(PerkSpellEdge(p, 0) == 0.0f, "spell perks: perks that make the school worse read 0, never negative");
        p.spellCost[0] = std::numeric_limits<float>::quiet_NaN();
        Check(PerkSpellEdge(p, 0) < 0.0f, "spell perks: a NaN multiplier is not known, never a score");
        Check(PerkSpellEdge(p, -1) < 0.0f && PerkSpellEdge(p, 5) < 0.0f, "spell perks: no school / out of range is not known");

        // The scorer: not applicable = bit-identical; the shipped PLACEHOLDER curve is x1.00 at 0, x1.15 at 0.5 and after.
        const Foe foe{ "Bandit", 0x0003DF30, 600.0f, 1.0f, 1.0f, false, false, false, false, true, false, false, 0.0f, 0.0f, 0.0f };
        Candidate c = Offense("Spell", 0x00012FCD, RoleTag::Fire, 30, 3000, foe, 0.0f, 100.0f, 100.0f);
        const Mind m{};
        const Profile none;
        const float base = ScoreOne(c, m, none, t, 3, 0.0f, nullptr);
        c.f.spellPerkEdge = 0.0f;
        Check(base > 0.0f && ScoreOne(c, m, none, t, 3, 0.0f, nullptr) == base, "spell perks: edge 0 scores exactly as a cast with no perk read");
        c.f.spellPerkEdge = 0.5f;
        const float half = ScoreOne(c, m, none, t, 3, 0.0f, nullptr);
        Check(std::fabs(half / base - 1.15f) < 1e-4f, "spell perks: the shipped placeholder gives x1.15 at edge 0.5");
        c.f.spellPerkEdge = 0.25f;
        const float quarter = ScoreOne(c, m, none, t, 3, 0.0f, nullptr);
        Check(quarter > base && quarter < half, "spell perks: the boost rises with the edge");
        c.f.spellPerkEdge = 1.0f;
        Check(ScoreOne(c, m, none, t, 3, 0.0f, nullptr) == half, "spell perks: flat after 0.5 (a boost, never more than x1.15)");
        Check(RoleOf(Cons::BoostSpellPerks) == Role::Boost && Name(Cons::BoostSpellPerks) == "spell_perks" &&
              static_cast<std::size_t>(Cons::BoostSpellPerks) == static_cast<std::size_t>(Cons::ConsumableWorth) + 1,
              "spell perks: the consideration is APPENDED after consumable_worth, a Boost, named spell_perks");

        // Generate: only a spell he casts himself carries the edge; an unread table leaves every candidate untouched.
        Snapshot s = MageFight();
        for (auto& e : s.book.entries) e.school = static_cast<std::int8_t>(kDestruction);
        const ClassGate g{};
        const auto edgeOf = [&](const Snapshot& a_s, std::uint32_t a_form, bool a_scroll) {
            const auto gen = Generate(a_s, g, t);
            for (const auto& cand : gen.actions)
                if (cand.form == a_form && (cand.family == Family::Scroll) == a_scroll) return cand.f.spellPerkEdge;
            return -2.0f;
        };
        Check(edgeOf(s, 0x12FCD, false) == -1.0f, "spell perks: an unread perk table leaves the cast's edge not known (the simulator today)");
        s.perks.read = true;   // the weapon group alone, as the simulator fills it
        Check(edgeOf(s, 0x12FCD, false) == -1.0f, "spell perks: the weapon group's read flag does not make the spell group known");
        s.perks.spellsRead = true; s.perks.spellCount[static_cast<std::size_t>(kDestruction)] = 3; s.perks.spellCost[static_cast<std::size_t>(kDestruction)] = 0.5f;
        Check(std::fabs(edgeOf(s, 0x12FCD, false) - 0.5f) < 1e-6f, "spell perks: Generate copies his school's edge onto the spell cast");
        s.scrollRoad = true;
        const float scrollEdge = edgeOf(s, 0xA44C5, true);
        Check(scrollEdge == -1.0f || scrollEdge == -2.0f, "spell perks: a scroll never carries the edge");
    }

    // ── hand lost (ClickUp 1dfcuabd22x, field 11-12 Auri): a concentration buff holds the left hand, a bow is put away for the channel ──
    void HandLostRows() {
        std::printf("\n=== hand lost (concentration buff and the weapon hand) ===\n");
        const Tuning& t = DefaultTuning();
        const ClassGate g{};
        const auto build = [&](bool a_conc, float a_leftShare) {
            Snapshot s = MageFight();
            SpellEntry stride = Spell("Transmutate Stride", 0x6600541E, Prune::Nature::Buff, 20, 0);
            stride.selfDelivery = true; stride.concentration = a_conc; s.book.entries.push_back(stride);
            s.self.hit = 30.0f; s.self.hitsPerSec = 1.0f; s.self.weaponLeftShare = a_leftShare;
            return s;
        };
        const auto find = [&](const Snapshot& a_s) -> std::optional<Candidate> {
            const auto gen = Generate(a_s, g, t);
            for (const auto& cand : gen.actions)
                if (cand.form == 0x6600541E) return cand;
            return std::nullopt;
        };
        const auto score = [&](const Candidate& c) { return ScoreOne(c, Mind{}, Profile{}, t, 3, 0.0f, nullptr); };

        const auto bow = find(build(true, 1.0f)), none = find(build(true, 0.0f)), fire = find(build(false, 1.0f));
        Check(bow && none && fire, "hand lost: the concentration buff is a candidate in all three cases");
        if (!(bow && none && fire)) return;
        Check(bow->f.handLost > 0.0f && bow->f.handLost <= 1.0f, "hand lost: a bow follower's channelled buff carries the share of foe health the bow would deal");
        Check(none->f.handLost < 0.0f, "hand lost: a follower whose weapon needs no hand (a mage with no weapon) carries nothing (not applicable)");
        Check(fire->f.handLost < 0.0f, "hand lost: a fire-and-forget buff carries nothing (it releases on fire)");
        Check(score(*bow) < score(*none), "hand lost: Auri's channelled self buff scores LOWER with the bow than the same buff with no bow");
        Check(score(*fire) == score(*none), "hand lost: a fire-and-forget buff scores the same with or without a bow (bit-identical)");
        Check(score(*bow) > 0.0f, "hand lost: it is a cost, never a veto (marth: buff-then-bow stays possible)");
        // a quiet fight loses nothing: a trivial foe's health against a weak hit
        Snapshot weak = build(true, 1.0f);
        weak.self.hit = 0.5f;
        for (auto& f : weak.foes) f.hp = 100000.0f;
        const auto w = find(weak);
        Check(w && w->f.handLost >= 0.0f && w->f.handLost < 0.01f && score(*w) > score(*bow), "hand lost: against a foe the bow barely scratches the buff costs almost nothing");
        // unmeasured damage: not applicable
        Snapshot blind = build(true, 1.0f);
        blind.self.hit = 0.0f;
        const auto b = find(blind);
        Check(b && b->f.handLost < 0.0f && score(*b) == score(*none), "hand lost: damage unmeasured = not applicable, bit-identical");
        // ENGINE-REALISTIC (review SEV-3): a bow's AI rate is weaponData.speed x fCombatDPSBowSpeedMult = 0.2/s (iaus-i8-damage-research.md:106), hit ~20,
        // top foe ~450 hp. The cost is on the TOP foe only, so a crowd behind it does not dilute it.
        {
            const auto real = [&](float a_share) {
                Snapshot s = build(true, a_share);
                s.self.hit = 20.0f; s.self.hitsPerSec = 0.2f;
                for (auto& f : s.foes) f.hp = 450.0f;
                return find(s);
            };
            const auto rb = real(1.0f), rn = real(0.0f);
            Check(rb && rn && rb->f.handLost > 0.04f && rb->f.handLost < 0.15f, "hand lost (realistic): a 20 x 0.2/s bow on a 450 hp top foe costs a few percent of its time-to-kill");
            if (rb && rn) {
                const float sb = score(*rb), sn = score(*rn);
                std::printf("  realistic row: no bow %.4f, bow %.4f (ratio %.3f)\n", static_cast<double>(sn), static_cast<double>(sb), static_cast<double>(sb / sn));
                Check(sb > 0.0f && sb < 0.9f * sn, "hand lost (realistic): the channelled self buff scores at least 10% below the no-bow case and stays above 0");
            }
        }
        // a half share (a second weapon in the left hand) costs less than a bow
        const auto half = [&] { Snapshot hs = build(true, 0.5f); hs.self.hit = 20.0f; hs.self.hitsPerSec = 0.2f; for (auto& f : hs.foes) f.hp = 450.0f; return find(hs); }();
        const auto full = [&] { Snapshot fs = build(true, 1.0f); fs.self.hit = 20.0f; fs.self.hitsPerSec = 0.2f; for (auto& f : fs.foes) f.hp = 450.0f; return find(fs); }();
        Check(half && full && half->f.handLost < full->f.handLost && score(*half) > score(*full), "hand lost: a left-hand second weapon costs less than a bow");
        Check(static_cast<std::size_t>(Cons::HandLost) == static_cast<std::size_t>(Cons::BoostSpellPerks) + 1 && RoleOf(Cons::HandLost) == Role::Shape && Name(Cons::HandLost) == "hand_lost",
              "hand lost: the consideration is APPENDED after spell_perks, a Shape, named hand_lost");
        Tuning tt = t; std::string why;
        Check(SetTuningScalar(tt, "t.concChannelSec", 5.0f, &why) && tt.concChannelSec == 5.0f && SetTuningScalar(tt, "t.handRedrawSec", 1.0f, &why) && tt.handRedrawSec == 1.0f,
              "hand lost: the channel length and the redraw time are tunable by name (t.concChannelSec / t.handRedrawSec)");
    }


    // ── field 11-17b (Jesper's weak spells and death, Adelinda's approach) ──────────────────────────────────────────────────
    void Field1117bRows() {
        std::printf("\n=== field 11-17b: tie-break by effect, heal rate, self dying, line of fire, heal safety, approach keyed on the pinned foe ===\n");
        const Tuning& t = DefaultTuning();
        const ClassGate g{};
        const Foe foe{ "Draugr", 0x3B500, 100.0f, 1.0f, 1.0f, false, false, false, false, true, false, false, 0.0f, 0.0f, 0.0f };
        // 1. The tie-break: higher effect per second first; equal effect: the lower cost PER CAST (a concentration cost is per second, counted over its channel).
        {
            Candidate a = Offense("Frostbite", 0x2B96B, RoleTag::Frost, 20, 2000, foe, 0.0f, 100.0f, 200.0f);
            Candidate b = Offense("Ice Spike", 0x2B96C, RoleTag::Frost, 20, 2000, foe, 0.0f, 100.0f, 200.0f);
            a.f.effectRate = 5.0f;  b.f.effectRate = 20.0f;
            std::vector<Candidate> c{ a, b };
            Decision d = Decide(Mind{}, c, Profile{}, t, Tick(1, 10.0));
            Check(!d.order.empty() && S0(d).idx == 1, "tie-break: equal scores, the spell with the higher effect per second wins (the lower form id would have won before)");
            c[0].f.effectRate = c[1].f.effectRate = 0.0f;
            c[0].f.concentration = true;   // cost 20 PER SECOND over 11.5 s = 230 per cast against a fire-and-forget 20
            d = Decide(Mind{}, c, Profile{}, t, Tick(1, 10.0));
            Check(!d.order.empty() && S0(d).idx == 1, "tie-break: equal effect, a concentration cost is counted over its channel (the stream is no longer the cheapest)");
            c[0].f.concentration = false;
            d = Decide(Mind{}, c, Profile{}, t, Tick(1, 10.0));
            Check(!d.order.empty() && S0(d).idx == 0, "tie-break: nothing differs but the key = the lower key (unchanged)");
        }
        // 2. Heal rate: of two heals that both fit the missing health, the one that keeps up with the blows scores higher.
        {
            Snapshot s = MageFight();
            s.self.hp = 80.0f; s.self.hpMax = 200.0f;
            for (auto& f : s.foes) { f.hit = 20.0f; f.hitsPerSec = 1.0f; f.rangedWeapon = true; }
            s.foes[0].targetId = s.self.id;   // 20 health a second on him
            SpellEntry weak = Spell("Weak Heal", 0x66001001, Prune::Nature::Heal, 20, 0); weak.selfDelivery = true; weak.measured = true; weak.heal = 6.0f;
            SpellEntry strong = Spell("Strong Heal", 0x66001002, Prune::Nature::Heal, 20, 0); strong.selfDelivery = true; strong.measured = true; strong.heal = 30.0f;
            s.book.entries.push_back(weak); s.book.entries.push_back(strong);
            const auto gen = Generate(s, g, t);
            const Candidate *cw = nullptr, *cs = nullptr;
            for (const auto& c : gen.actions) { if (c.form == 0x66001001) cw = &c; if (c.form == 0x66001002) cs = &c; }
            Check(cw && cs, "heal rate: both measured heals are candidates for himself");
            if (cw && cs) {
                Check(cw->f.healRate > 0.0f && cs->f.healRate > cw->f.healRate && cs->f.bestHealRate == cs->f.healRate, "heal rate: Generate fills the heal's rate per cast second and his best castable heal's on this recipient");
                const auto sc = [&](const Candidate& c) { return ScoreOne(c, Mind{}, Profile{}, t, 3, 0.0f, nullptr); };
                Check(sc(*cs) > sc(*cw), "heal rate: the heal that mends more per second scores higher though both fit the missing health (they tied before)");
                Check(sc(*cw) > 0.0f, "heal rate: a weak heal is a shape, never a veto");
                // the tie-break reads the same rate
                Check(cs->f.effectRate > cw->f.effectRate, "heal rate: the heal's effect rate is what the tie-break compares");
                // not applicable while nothing hits him
                Candidate q = *cs; q.f.bestHealRate = 0.0f; Candidate q2 = *cw; q2.f.bestHealRate = 0.0f;
                q.f.expHeal = q2.f.expHeal = 1.0f;   // same HealSize
                Check(sc(q) == sc(q2), "heal rate: no measured best heal = not applicable, two heals that fit alike tie again (the tie-break then decides)");
            }
            Check(static_cast<std::size_t>(Cons::HealRate) == static_cast<std::size_t>(Cons::HandLost) + 1 && RoleOf(Cons::HealRate) == Role::Shape && Name(Cons::HealRate) == "heal_rate",
                  "heal rate: APPENDED after hand_lost, a Shape, named heal_rate");
        }
        // 3. His own dying boost below t.dyingSelfHp even with the time-to-live unknown.
        {
            Snapshot s = MageFight();
            s.self.hp = 30.0f; s.self.hpMax = 200.0f;   // 15%, nothing hits him
            const auto gen = Generate(s, g, t);
            const Candidate* heal = nullptr;
            for (const auto& c : gen.actions) if (c.form == 0x12FCC) heal = &c;
            Check(heal && heal->f.recipientSelf && !std::isfinite(heal->f.recipientTtl), "self dying: his own heal is flagged recipientSelf with the time-to-live unknown");
            if (heal) {
                Candidate other = *heal; other.f.recipientSelf = false;
                Tuning tSelfOnly = t; tSelfOnly.dyingAllyHp = 0.0f;   // 11-17g: the ally fallback would boost the copy too; this row is about the SELF read
                const float with = ScoreOne(*heal, Mind{}, Profile{}, tSelfOnly, 3, 0.0f, nullptr), without = ScoreOne(other, Mind{}, Profile{}, tSelfOnly, 3, 0.0f, nullptr);
                Check(with > without * 1.3f, "self dying: his own heal at 15% health scores x1.4 with the dying boost (ttl unknown)");
                Candidate hi = *heal; hi.f.recipientHp = 0.40f; Candidate hi2 = hi; hi2.f.recipientSelf = false;
                Check(ScoreOne(hi, Mind{}, Profile{}, tSelfOnly, 3, 0.0f, nullptr) == ScoreOne(hi2, Mind{}, Profile{}, tSelfOnly, 3, 0.0f, nullptr), "self dying: at 40% health there is no boost");
            }
        }
        // 3b. 11-17g (field heal-timing): bursts and the gross incoming rate. Heal Other on the player (50 of 100).
        {
            const auto healOnPlayer = [&](const Snapshot& s) -> Candidate {
                const auto gen = Generate(s, g, t);
                for (const auto& c : gen.actions) if (c.form == 0x12FD2 && c.target == 0x14) return c;
                return Candidate{};
            };
            const auto sc = [&](const Candidate& c) { return ScoreOne(c, Mind{}, Profile{}, t, 3, 0.0f, nullptr); };
            Snapshot base = MageFight();
            const Candidate c0 = healOnPlayer(base);
            Check(c0.form == 0x12FD2 && !std::isfinite(c0.f.recipientHitsToDown), "burst: no hit seen = hits-to-down unknown (the pre-11-17g read)");
            // the largest recent hit 60 of his 50 health: the next one downs him; 40: he has 1.25 left
            Snapshot b60 = MageFight(); for (auto& a : b60.allies) if (a.isPlayer) a.maxHit = 60.0f;
            Snapshot b40 = MageFight(); for (auto& a : b40.allies) if (a.isPlayer) a.maxHit = 40.0f;
            const Candidate c60 = healOnPlayer(b60), c40 = healOnPlayer(b40);
            Check(c60.form == 0x12FD2 && c60.f.recipientHitsToDown < 1.0f && c40.f.recipientHitsToDown > 1.0f, "burst: hits-to-down = health / the largest recent hit (50/60, 50/40)");
            Candidate noBoost = c60; noBoost.f.recipientHitsToDown = 1.25f;
            Check(sc(c60) > sc(noBoost) * 1.2f, "burst: an ally with one hit left to down him gets the dying boost with the time-to-live unknown");
            // urgency: x = min(ttl / 10, hitsToDown / 3). 50/60 over 3 = 0.28 is the lower read; infinite ttl alone would be Off
            Candidate lowH = c60; lowH.f.recipientHitsToDown = 1.5f; Candidate highH = c60; highH.f.recipientHitsToDown = 9.0f;
            lowH.f.recipientTtl = highH.f.recipientTtl = kInf;
            Check(sc(lowH) > sc(highH), "burst: the heal urgency is higher the fewer hits he has left (hits-to-down 1.5 vs 9, time-to-live unknown)");
            Candidate ttlOnly = c60; ttlOnly.f.recipientHitsToDown = kInf; ttlOnly.f.recipientTtl = 20.0f;
            Candidate both = ttlOnly; both.f.recipientHitsToDown = 1.5f;
            Check(sc(both) > sc(ttlOnly), "burst: a close hits-to-down raises the heal above the time-to-live read alone (min of the two)");
            // the ally fallback: under t.dyingAllyHp the boost fires with nothing known
            Snapshot low = MageFight(); for (auto& a : low.allies) if (a.isPlayer) a.hp = 20.0f;
            Snapshot mid = MageFight(); for (auto& a : mid.allies) if (a.isPlayer) a.hp = 40.0f;
            const Candidate cl = healOnPlayer(low), cm = healOnPlayer(mid);
            Tuning tNoAlly = t; tNoAlly.dyingAllyHp = 0.0f;
            Check(cl.form == 0x12FD2 && !cl.f.recipientSelf && cm.f.recipientHp > t.dyingAllyHp, "burst: the ally fallback reads recipientHp, not recipientSelf");
            Check(ScoreOne(cl, Mind{}, Profile{}, t, 3, 0.0f, nullptr) > ScoreOne(cl, Mind{}, Profile{}, tNoAlly, 3, 0.0f, nullptr) * 1.3f,
                  "burst: an ally at 20% (under t.dyingAllyHp) scores x1.4 with the dying boost, ttl unknown (off with t.dyingAllyHp = 0)");
            Check(ScoreOne(cm, Mind{}, Profile{}, t, 3, 0.0f, nullptr) == ScoreOne(cm, Mind{}, Profile{}, tNoAlly, 3, 0.0f, nullptr), "burst: an ally at 40% has no fallback boost");
            // gross vs net: a heal stream cancels the observed loss; the gross rate does not
            Snapshot gross = MageFight();
            for (auto& a : gross.allies) if (a.isPlayer) { a.lossPerSec = 0.0f; a.grossPerSec = 25.0f; }
            const Combatant* pcg = nullptr; for (const auto& a : gross.allies) if (a.isPlayer) pcg = &a;
            Check(pcg && Estimate::IncomingOn(gross, *pcg) == 25.0f && Estimate::Ttl(pcg->hp, Estimate::IncomingOn(gross, *pcg)) == 2.0f,
                  "gross rate: net loss 0 under a heal stream, gross 25 a second: the incoming rate is 25 and the time-to-live 2 s");
            Snapshot netOnly = MageFight();
            const Combatant* pcn = nullptr; for (const auto& a : netOnly.allies) if (a.isPlayer) pcn = &a;
            Check(pcn && !std::isfinite(Estimate::Ttl(pcn->hp, Estimate::IncomingOn(netOnly, *pcn))), "gross rate: nothing measured stays unknown");
            const Candidate cg = healOnPlayer(gross);
            Check(cg.form == 0x12FD2 && cg.f.recipientTtl < 5.0f, "gross rate: the recipient's time-to-live comes from the gross rate (dying under 5 s)");
        }
        // 3c. 11-17g review F2: the lap maths (Estimate::LapGross / BurstReads, the very code Fills.cpp runs).
        {
            // Jesper: 166 health in FIVE events from one archer in one lap = ONE burst of 166, not five of 33
            const float volley = Estimate::LapGross(166.0f, 5, 5 * 27.0f);
            Check(volley == 166.0f, "lap gross: a volley of 5 events that took 166 is a lap of 166 (the drop wins over the attackers' estimates 135)");
            Estimate::BurstSample a{ 10.0, volley };
            float gross = 0.0f, mx = 0.0f;
            Estimate::BurstReads({ a }, 10.5, 4.0, 8.0, 0.5, gross, mx);
            Check(mx == 166.0f && gross == 166.0f / 4.0f, "lap gross: the largest recent burst is the whole lap (166), not the per-event split; the rate is over the 4 s window");
            Combatant jes = Actor("Jesper", 5, 172, 172, 0, 0); jes.maxHit = mx;
            Check(Estimate::HitsToDown(172.0f, jes) > 1.0f && Estimate::HitsToDown(100.0f, jes) < 1.0f, "lap gross: 172 health is ~1.04 bursts of 166, 100 health is under one");
            // a heal lands in the same lap as a hit: net drop 0, one event of 27: the floor stands
            Check(Estimate::LapGross(0.0f, 1, 27.0f) == 27.0f, "lap gross: a heal that cancels the net drop cannot cancel a lap with a hit event below the attackers' estimate");
            // no event this lap (it landed in the neighbouring lap): the drop is still recorded as a burst
            Check(Estimate::LapGross(120.0f, 0, 0.0f) == 120.0f, "lap gross: a drop-only lap records the burst (no event count needed)");
            Estimate::BurstReads({ Estimate::BurstSample{ 10.0, Estimate::LapGross(120.0f, 0, 0.0f) } }, 10.2, 4.0, 8.0, 0.5, gross, mx);
            Check(mx == 120.0f, "lap gross: a drop-only lap reaches maxHit");
            // windows: a burst 6 s old is out of the rate (4 s) but in the maximum (8 s); 9 s old is out of both
            Estimate::BurstReads({ Estimate::BurstSample{ 4.0, 100.0f } }, 10.0, 4.0, 8.0, 0.5, gross, mx);
            Check(gross == 0.0f && mx == 100.0f, "lap gross: 6 s old = out of the 4 s rate, still the 8 s maximum");
            Estimate::BurstReads({ Estimate::BurstSample{ 1.0, 100.0f } }, 10.0, 4.0, 8.0, 0.5, gross, mx);
            Check(gross == 0.0f && mx == 0.0f, "lap gross: 9 s old = out of both");
            // 0.3 s volley split over 3 samples = ONE burst; two bursts 2 s apart read as the larger, not the sum
            Estimate::BurstReads({ Estimate::BurstSample{ 10.0, 60.0f }, Estimate::BurstSample{ 10.15, 60.0f }, Estimate::BurstSample{ 10.3, 46.0f } }, 10.4, 4.0, 8.0, 0.5, gross, mx);
            Check(mx == 166.0f, "lap gross: a 0.3 s volley split over 3 samples reads as ONE burst of 166");
            Estimate::BurstReads({ Estimate::BurstSample{ 8.0, 90.0f }, Estimate::BurstSample{ 10.0, 120.0f } }, 10.1, 4.0, 8.0, 0.5, gross, mx);
            Check(mx == 120.0f, "lap gross: two bursts 2 s apart read as the larger (120), not the sum (210)");
            // a friendly heal event adds nothing: the sink drops party-caused events (Fills.cpp), so no events and no floor: net drop only (0 here)
            Check(Estimate::LapGross(0.0f, 0, 0.0f) == 0.0f, "lap gross: a party heal event (dropped by the sink) adds nothing");
        }
        // 4. The line of fire.
        {
            Combatant caster = Actor("Mage", 1, 100, 100, 0, 0), target = Actor("Foe", 2, 100, 100, 600, 0);
            const auto at = [&](float x, float y) { return Actor("Ally", 3, 100, 100, x, y); };
            Check(Estimate::InLineOfFire(caster, target, at(300, 70), 0.0f), "line of fire: an ally 70 units off the middle of the line is in it");
            Check(!Estimate::InLineOfFire(caster, target, at(300, 90), 0.0f), "line of fire: 90 units off the line is clear (corridor 80)");
            Check(!Estimate::InLineOfFire(caster, target, at(-300, 0), 0.0f) && !Estimate::InLineOfFire(caster, target, at(900, 0), 0.0f), "line of fire: behind the caster or past the target is not in the line");
            Check(!Estimate::InLineOfFire(caster, target, at(600, 100), 0.0f) && Estimate::InLineOfFire(caster, target, at(600, 100), 50.0f) && !Estimate::InLineOfFire(caster, target, at(600, 120), 50.0f),
                  "line of fire: a blast reaches a body area + 64 from the target (50 -> 114)");
            Snapshot s = MageFight();   // the player stands at (300, 0): on the line to the foe at (600, 0)
            Generated gen = Generate(s, g, t);
            const Candidate* bolt = nullptr;
            for (const auto& c : gen.actions) if (c.form == 0x12FCD && c.target == 0x3B500) bolt = &c;
            Check(bolt && !bolt->f.splashSafe && ScoreOne(*bolt, Mind{}, Profile{}, t, 3, 0.0f, nullptr) == 0.0f,
                  "line of fire: Firebolt at the foe with the player in the line is vetoed (splash_safe gate, no area needed)");
            bool noted = false;
            for (const auto& pr : gen.fireBlocked) if (pr.first == 0x12FCD && pr.second == 0x3B500) noted = true;
            Check(noted, "line of fire: the refused (spell, foe) pair is reported for the refusal memory");
            s.allies[0].y = 300.0f; s.allies[0].dist = std::sqrt(300.0f * 300.0f + 300.0f * 300.0f);   // the player steps off the line
            s.allies[1].x = 0.0f; s.allies[1].y = 300.0f;
            gen = Generate(s, g, t);
            bolt = nullptr;
            for (const auto& c : gen.actions) if (c.form == 0x12FCD && c.target == 0x3B500) bolt = &c;
            Check(bolt && bolt->f.splashSafe && ScoreOne(*bolt, Mind{}, Profile{}, t, 3, 0.0f, nullptr) > 0.0f, "line of fire: with the party off the line the same cast is a candidate again");
        }
        // 5. Heal safety: no concentration / touch heal for a recipient a foe can hurt who would not survive, while an aimed heal is available.
        {
            Snapshot s = MageFight();
            s.allies[1].hp = 15.0f;   // Lydia (250, 250), 15%
            for (auto& f : s.foes) { f.hit = 50.0f; f.hitsPerSec = 1.0f; }
            s.foes[1].x = 330.0f; s.foes[1].y = 250.0f; s.foes[1].targetId = s.allies[1].id;   // a melee foe on her
            SpellEntry conc = Spell("Channel Heal", 0x66002001, Prune::Nature::Heal, 10, 1000); conc.concentration = true; conc.measured = true; conc.heal = 10.0f;
            s.book.entries.push_back(conc);
            const auto has = [&](const Generated& gn, std::uint32_t form, std::uint32_t tgt) {
                for (const auto& c : gn.actions) if (c.form == form && c.target == tgt) return true;
                return false;
            };
            Generated gn = Generate(s, g, t);
            Check(!has(gn, 0x66002001, s.allies[1].id) && has(gn, 0x12FD2, s.allies[1].id) && gn.healRefused,
                  "heal safety: the concentration heal on Lydia is refused (a foe on her, she would not survive) and the aimed Heal Other stays");
            // no aimed heal to fall back to: the channelled heal stays (the close-spell rule's shape)
            Snapshot only = s;
            for (auto& e : only.book.entries) if (e.form == 0x12FD2 || e.form == 0x12FCC) e.drop = Prune::Drop::Placement;
            Check(has(Generate(only, g, t), 0x66002001, s.allies[1].id), "heal safety: with no fire-and-forget heal in the book the concentration heal is kept");
            // the HEALER exposed (a melee foe on him, he would not survive): the channel on a calm Lydia is refused too; his own self-delivery Healing counts for HIM
            Snapshot hx = s;
            hx.foes[1].targetId = hx.allies[1].id; hx.foes[1].x = 1500.0f;   // off Lydia
            hx.foes[2].x = 60.0f; hx.foes[2].y = 0.0f; hx.foes[2].targetId = hx.self.id; hx.self.hp = 15.0f;
            for (auto& a : hx.allies) a.hp = a.hpMax;
            hx.allies[1].hp = 50.0f;   // hurt, not exposed
            Generated gx = Generate(hx, g, t);
            Check(!has(gx, 0x66002001, s.allies[1].id) && has(gx, 0x12FD2, s.allies[1].id), "heal safety: the healer is in a foe's reach and would not survive: the channel on a calm ally is refused, the aimed heal stays");
            Check(!has(gx, 0x66002001, 0) && has(gx, 0x12FCC, 0), "heal safety: for his own heal the self-delivery fire-and-forget Healing is the fallback (the channel is refused)");
            // she is not threatened: kept
            Snapshot calm = s; calm.foes[1].targetId = 0; calm.foes[1].x = 1500.0f;
            Check(has(Generate(calm, g, t), 0x66002001, s.allies[1].id), "heal safety: no foe can hurt her = the concentration heal is a candidate");
        }
        // 6. The occluded approach is keyed on the foe he is ON.
        {
            Snapshot s = MageFight();
            s.drawn = PostureKind::Bow;
            for (auto& f : s.foes) f.sight = Sight::Visible;
            s.currentTargetId = 0x3B500;
            const float bow = t.bowWeaponRange;
            Check(Reposition::SightHeld(s, bow), "approach: the pinned foe visible = sight held");
            s.foes[0].sight = Sight::Occluded;
            Check(!Reposition::SightHeld(s, bow) && Reposition::PinnedFoe(s, bow) && Reposition::PinnedFoe(s, bow)->id == 0x3B500,
                  "approach: the pinned foe occluded while another foe is visible = sight NOT held (the approach files for it)");
            s.currentTargetId = 0;
            Check(Reposition::SightHeld(s, bow), "approach: no pinned foe, a visible foe inside the bow's reach = sight held");
            for (auto& f : s.foes) f.dist = 5000.0f;
            Check(!Reposition::SightHeld(s, bow), "approach: no pinned foe, every visible foe past the bow's reach = sight NOT held");
            s.drawn = PostureKind::None;
            Check(Reposition::SightHeld(s, bow), "approach: a caster keeps the old reading (any visible foe holds sight)");
        }
    }

    void SchoolAffinityRows() {
        namespace CC = MFO::Iaus::ClassCurves;
        std::printf("\n=== school affinity ===\n");
        const Tuning& t = DefaultTuning();
        const Foe foe{ "Bandit", 0x0003DF30, 600.0f, 1.0f, 1.0f, false, false, false, false, true, false, false, 0.0f, 0.0f, 0.0f };
        auto spell = [&](int a_school) {
            Candidate c = Offense("Spell", 0x00012FCD0u + static_cast<std::uint32_t>(a_school), RoleTag::Fire, 30, 3000, foe, 0.0f, 100.0f, 100.0f);
            c.school = static_cast<std::int8_t>(a_school);
            return c;
        };
        const Mind m{};
        const Profile none;
        float base[5];
        bool flat = true;
        for (int s = 0; s < 5; ++s) base[s] = ScoreOne(spell(s), m, none, t, 3, 0.0f, nullptr);
        // SHIPPED (marth's School Circle save 2026-11-12, a fixed design input): a row with no home school and the Arch-Mage are flat;
        // a home-school row carries floor 0.35, sharpness 1.5, mastered bump 0.15, scores its own school unchanged and every other lower.
        bool shaped = true; int homes = 0;
        for (std::size_t i = 0; i < CC::kRows; ++i) {
            const CC::Scalars& sc = CC::Defaults()[i].scalars; const Profile& pi = CC::ProfileFor(static_cast<std::uint8_t>(i));
            if (sc.schoolPeak < 0 || sc.schoolAny) { for (int s = 0; s < 5; ++s) flat = flat && ScoreOne(spell(s), m, pi, t, 3, 0.0f, nullptr) == base[s]; continue; }
            ++homes; shaped = shaped && sc.schoolFloor == 0.35f && sc.schoolShape == 1.5f && sc.schoolMasteredBump == 0.15f;
            for (int s = 0; s < 5; ++s) { const float v = ScoreOne(spell(s), m, pi, t, 3, 0.0f, nullptr); shaped = shaped && (s == sc.schoolPeak ? v == base[s] : (v < base[s] && v > 0.0f)); }
        }
        Check(flat && base[0] > 0.0f, "school affinity: every shipped row with no home school (and the Arch-Mage) scores every school bit-identically to the default Profile");
        Check(shaped && homes == 14, "school affinity: the 14 home-school rows ship marth's fixed shape (floor 0.35, sharpness 1.5, bump 0.15): home school unchanged, every other school lower, none vetoed");
        Candidate noSchool = spell(0); noSchool.school = -1;
        Check(ScoreOne(noSchool, m, none, t, 3, 0.0f, nullptr) == base[0], "school affinity: no school scores as before");
        CC::Row r;   // a Destruction caster
        CC::SetScalar(r, "schoolPeak", 0.0f); CC::SetScalar(r, "schoolFloor", 0.4f);
        Check(!(CC::SetScalar(r, "schoolPeak", 9.0f) && r.scalars.schoolPeak >= 0), "school affinity: an out-of-range peak means none");
        CC::SetScalar(r, "schoolPeak", 0.0f);
        const Profile pr = CC::Build(r);
        float w[5];
        for (int s = 0; s < 5; ++s) w[s] = pr.school[static_cast<std::size_t>(s)];
        Check(w[0] == 1.0f && w[1] == w[4] && w[2] == w[3] && w[1] > w[2] && w[2] >= 0.4f && w[1] < 1.0f, "school affinity: wave peaks at his school, is symmetric around the circle, falls to the far side, never below the floor");
        Check(ScoreOne(spell(0), m, pr, t, 3, 0.0f, nullptr) == base[0], "school affinity: his own school scores unchanged");
        Check(ScoreOne(spell(2), m, pr, t, 3, 0.0f, nullptr) < base[2] && ScoreOne(spell(2), m, pr, t, 3, 0.0f, nullptr) > 0.0f, "school affinity: a far school is weighted down, never vetoed");
        Check(ScoreOne(spell(1), m, pr, t, 3, 0.0f, nullptr) > ScoreOne(spell(2), m, pr, t, 3, 0.0f, nullptr) * (base[1] / base[2]) , "school affinity: a near school keeps more than a far one");
        Check(ScoreOne(spell(2), m, none, t, 3, 0.0f, nullptr) == base[2] && ScoreOne(spell(2), m, CC::ProfileFor(0), t, 3, 0.0f, nullptr) == base[2], "school affinity: ...and only that class's Profile");
        CC::Row sharp = r; CC::SetScalar(sharp, "schoolShape", 3.0f);
        Check(CC::Build(sharp).school[1] < w[1] && CC::Build(sharp).school[1] >= 0.4f, "school affinity: a higher shape narrows the peak");
        CC::Row flatRow = r; CC::SetScalar(flatRow, "schoolFloor", 1.0f);
        bool allOne = true; for (float x : CC::Build(flatRow).school) allOne = allOne && x == 1.0f;
        Check(allOne, "school affinity: floor 1.0 is exactly flat whatever the peak");
        Check(!CC::SetScalar(r, "schoolNope", 1.0f), "school affinity: a bad scalar name is refused");
        CC::Row ext; CC::SetScalar(ext, "schoolPeak", 0.0f); CC::SetScalar(ext, "schoolFloor", 0.0f); CC::SetScalar(ext, "schoolShape", 500.0f);
        bool pos = true; for (float x : CC::Build(ext).school) pos = pos && x > 0.0f;
        Check(pos && ScoreOne(spell(2), m, CC::Build(ext), t, 3, 0.0f, nullptr) > 0.0f, "school affinity: floor 0 + a huge shape never reaches weight 0 (never a veto)");
        Candidate none1 = spell(0); none1.school = -1;
        Check(ScoreOne(none1, m, pr, t, 3, 0.0f, nullptr) == base[0], "school affinity: a no-school candidate is unaffected by a peaked Profile");
        CC::Row nf; CC::SetScalar(nf, "schoolPeak", std::numeric_limits<float>::quiet_NaN());
        Check(nf.scalars.schoolPeak == -1, "school affinity: a non-finite peak means none");
        // Arch-Mage: any school (a fixed rule), flat even with a floor set on a copy of its row
        CC::Row am = CC::Defaults()[CC::kArchMage];
        CC::SetScalar(am, "schoolFloor", 0.3f);
        bool amFlat = am.scalars.schoolAny; for (float x : CC::Build(am).school) amFlat = amFlat && x == 1.0f;
        Check(amFlat && CC::Defaults()[CC::kArchMage].scalars.schoolAny && !CC::Defaults()[8].scalars.schoolAny, "school affinity: Arch-Mage is flat 1.0 over every school (fixed rule), other rows are not");
        // mastered bump: shipped 0 = no bump; a set bump lands at the mastered class's peak, summed, and the score clamps at the peak weight
        ClassGate g0; CC::ApplyMastered((1ull << 8) | (1ull << 10), g0);
        bool ship = g0.schoolBump[kDestruction] == 0.15f && g0.schoolBump[kRestoration] == 0.15f && g0.schoolBump[kAlteration] == 0.0f && g0.schoolBump[kIllusion] == 0.0f && g0.schoolBump[kConjuration] == 0.0f;
        ClassGate gn; CC::ApplyMastered(1ull | (1ull << 2) | (1ull << CC::kArchMage), gn); for (float x : gn.schoolBump) ship = ship && x == 0.0f;
        Check(ship, "school affinity: a mastered home-school class ships a 0.15 bump at its own school only; a mastered class with no home school gives none");
        CC::Table tab = CC::Defaults();
        CC::SetScalar(tab[8], "schoolMasteredBump", 0.15f);    // Evoker: Destruction
        CC::SetScalar(tab[10], "schoolMasteredBump", 0.10f);   // Healer: Restoration
        CC::SetScalar(tab[0], "schoolMasteredBump", 0.50f);    // Melee: no peak -> nothing
        ClassGate gb; CC::ApplyMastered(tab, (1ull << 8) | (1ull << 10) | 1ull, gb);
        Check(gb.schoolBump[kDestruction] == 0.15f && gb.schoolBump[kRestoration] == 0.10f && gb.schoolBump[kAlteration] == 0.0f, "school affinity: each mastered casting class bumps its own peak school only");
        Candidate far = spell(kRestoration); far.schoolBump = gb.schoolBump[kRestoration];
        const float bumped = ScoreOne(far, m, pr, t, 3, 0.0f, nullptr), unb = ScoreOne(spell(kRestoration), m, pr, t, 3, 0.0f, nullptr);
        Check(bumped > unb && bumped <= base[2], "school affinity: a mastered bump raises the far school but never past the peak weight");
        Candidate huge = spell(kRestoration); huge.schoolBump = 5.0f;
        Check(ScoreOne(huge, m, pr, t, 3, 0.0f, nullptr) == base[2], "school affinity: the bump clamps at the peak weight 1.0");
    }

}

// ── feat/buff-potions (ClickUp 1dfcuabd18f): the DrinkBuff family on a hand-built kit. The VALUES scored are labelled placeholders (not searched); what is
// pinned is the SHAPE: which kind is offered when, and that a follower with no buff potion generates exactly what he did before. ──────────────────────────
namespace {
    Snapshot BuffFight(BuffKind a_kind, float a_mag = 50.0f, float a_dur = 60.0f, int a_av = -1) {
        Snapshot s;
        s.self = Actor("Knight", 0x14F00, 180, 200, 0, 0);
        s.self.sp = 100; s.self.spMax = 100; s.self.mp = 100; s.self.mpMax = 100;
        s.playerId = 0x14;
        Combatant foe = Actor("Mage", 0x3B600, 100, 100, 400, 0);
        foe.targetId = s.self.id; foe.caster = true; foe.incomingAv = Prune::kAvResistFire;
        s.foes.push_back(foe);
        s.drawn = PostureKind::Melee;
        s.hasBook = true;
        PotionBuff& b = s.book.kit.buff[static_cast<std::size_t>(a_kind)];
        b.form = 0x3EAE9; b.count = 1; b.magnitude = a_mag; b.duration = a_dur; b.av = a_av; b.name = "Potion";
        return s;
    }
    // The score of the first DrinkBuff candidate of that kind, -1 when none is generated.
    float BuffScore(const Snapshot& a_s, BuffKind a_kind, ClassGate a_gate = ClassGate{}) {
        const Tuning& t = DefaultTuning();
        const Generated g = Generate(a_s, a_gate, t);
        for (const auto& c : g.actions)
            if (c.family == Family::DrinkBuff && c.f.buffKind == static_cast<std::uint8_t>(a_kind))
                return ScoreOne(c, Mind{}, Profile{}, t, 3, 0.0f, nullptr);
        return -1.0f;
    }
}

static void BuffPotions() {
    std::printf("\n=== 14. Buff potions (feat/buff-potions): the DrinkBuff family, placeholder values ===\n");
    const Tuning& t = DefaultTuning();
    {
        Snapshot s = BuffFight(BuffKind::ResistFire);
        const float on = BuffScore(s, BuffKind::ResistFire);
        std::printf("   resist fire vs a fire mage on him: %.3f (act floor %.2f)\n", on, t.actFloor);
        Check(on > t.actFloor, "a resist potion scores above the act floor against a foe whose spell is that element");
        s.foes[0].incomingAv = Prune::kAvResistFrost;
        Check(BuffScore(s, BuffKind::ResistFire) <= 0.0f, "...and not against a frost mage (buff_threat vetoes)");
        s.foes[0].incomingAv = -1; s.foes[0].caster = false;
        Check(BuffScore(s, BuffKind::ResistFire) <= 0.0f, "...and not against a plain brawler");
        s.foes[0].caster = true; s.foes[0].incomingAv = Prune::kAvResistFrost;
        s.book.kit.buff[static_cast<std::size_t>(BuffKind::ResistMagic)] = s.book.kit.buff[static_cast<std::size_t>(BuffKind::ResistFire)];
        Check(BuffScore(s, BuffKind::ResistMagic) > t.actFloor, "resist magic answers any caster, whatever its element");
        s = BuffFight(BuffKind::ResistFire);
        s.book.kit.buff[static_cast<std::size_t>(BuffKind::ResistFire)].active = true;
        Check(BuffScore(s, BuffKind::ResistFire) <= 0.0f, "a kind already live on him is not offered again (already_active)");
        s = BuffFight(BuffKind::ResistFire);
        s.book.kit.buff[static_cast<std::size_t>(BuffKind::ResistFire)].count = 0;
        Check(BuffScore(s, BuffKind::ResistFire) < 0.0f, "no stock, no candidate (P2)");
    }
    {
        Snapshot s = BuffFight(BuffKind::FortifyHealth);
        s.combatSec = 0.0f;
        const float open = BuffScore(s, BuffKind::FortifyHealth);
        s.combatSec = 12.0f;
        Check(open > t.actFloor, "a fortify is offered at the start of the fight");
        Check(BuffScore(s, BuffKind::FortifyHealth) <= 0.0f, "...and not twelve seconds in (fight_start is an opener only)");
        s = BuffFight(BuffKind::FortifyMagicka);
        Check(BuffScore(s, BuffKind::FortifyMagicka) < 0.0f, "fortify magicka is not offered to a follower who casts nothing");
        s = BuffFight(BuffKind::FortifySkill, 20.0f, 60.0f, 8);
        Check(BuffScore(s, BuffKind::FortifySkill) < 0.0f, "fortify archery is not offered with a melee weapon drawn");
        s.drawn = PostureKind::Bow;
        Check(BuffScore(s, BuffKind::FortifySkill) > t.actFloor, "...and is with a bow drawn");
        Check(BaseSkillAv(149) == 20 && BaseSkillAv(137) == 8 && BaseSkillAv(98) == 8 && BaseSkillAv(11) == 11 && BaseSkillAv(152) == 23 && BaseSkillAv(24) == -1 &&
              BaseSkillAv(153) == -1, "a PowerModifier / *Modifier skill actor value maps to its base skill (Destruction 149 -> 20, Marksman 137 -> 8, 98 -> 8)");
        s = BuffFight(BuffKind::FortifySkill, 20.0f, 60.0f, BaseSkillAv(149));
        s.book.entries.push_back(SpellEntry{});   // a Destruction spell he casts
        s.book.entries.back().drop = Prune::Drop::Keep; s.book.entries.back().school = kDestruction;
        s.drawn = PostureKind::None;
        Check(BuffScore(s, BuffKind::FortifySkill) > t.actFloor, "a fortify-Destruction potion (the PowerModifier value) is offered to a Destruction caster");
        s = BuffFight(BuffKind::FortifyHealth);
        s.foes[0].level = 3;
        Check(BuffScore(s, BuffKind::FortifyHealth) < 0.0f, "no fortify against a foe far below him (mudcrab)");
        s = BuffFight(BuffKind::FortifySkill, 20.0f, 60.0f, 10);
        Check(BuffScore(s, BuffKind::FortifySkill) < 0.0f, "fortify smithing is never a combat candidate");
    }
    {
        Snapshot s = BuffFight(BuffKind::RegenHealth);
        s.self.hp = s.self.hpMax;
        Check(BuffScore(s, BuffKind::RegenHealth) <= 0.0f, "regen health at full health scores nothing (need is vetoed at full)");
        s.self.hp = 30;
        Check(BuffScore(s, BuffKind::RegenHealth) > 0.0f, "...and scores when he is hurt");
        const float low = BuffScore(s, BuffKind::RegenHealth);
        s.self.hp = 80;
        Check(BuffScore(s, BuffKind::RegenHealth) < low, "...more the lower his health");
    }
    {
        Snapshot s = BuffFight(BuffKind::Invisibility);
        Check(BuffScore(s, BuffKind::Invisibility) < 0.0f, "invisibility is not offered without stealth intent");
        s.selfSneaking = true;
        Check(BuffScore(s, BuffKind::Invisibility) > t.actFloor, "...and is while he sneaks");
        s.selfSneaking = false;
        ClassGate g; g.sneakClass = true;
        Check(BuffScore(s, BuffKind::Invisibility, g) < 0.0f, "...but a sneak class alone, not sneaking, does not drink it (never in open melee)");
    }
    {
        Snapshot s = BuffFight(BuffKind::WaterBreathing);
        Check(BuffScore(s, BuffKind::WaterBreathing) < 0.0f, "water breathing is catalogued, never a candidate");
        s = BuffFight(BuffKind::CurePoison);
        Check(BuffScore(s, BuffKind::CurePoison) < 0.0f, "a cure is catalogued, never a candidate");
    }
    {
        // Bit-identical for a follower who carries no buff potion: the family adds no candidate and no term to anything else.
        Snapshot with = BuffFight(BuffKind::ResistFire);
        with.book.kit.buff[static_cast<std::size_t>(BuffKind::ResistFire)] = PotionBuff{};
        const Generated g = Generate(with, ClassGate{}, t);
        Check(CountFam(g, Family::DrinkBuff) == 0, "no buff potion carried: no DrinkBuff candidate at all");
        Check(Name(Family::DrinkBuff) == "drink_buff" && Name(Cons::BuffThreat) == "buff_threat" && Name(Cons::BuffStealth) == "buff_stealth", "the new names");
        Check(static_cast<std::size_t>(Family::DrinkBuff) == static_cast<std::size_t>(Family::Step) + 1 &&
              static_cast<std::size_t>(Cons::BuffThreat) == static_cast<std::size_t>(Cons::BoostGuardLine) + 1, "the new family and considerations are APPENDED");
    }
    {
        // The pure kind rules the catalog and the runtime fallback share.
        BuffKind k = BuffKind::kCount;
        Check(BuffFromName("resist_fire", k) && k == BuffKind::ResistFire && BuffName(BuffKind::FortifySkill) == "fortify_skill", "kind names round-trip");
        Check(!BuffFromName("nope", k), "an unknown kind is refused (a newer patcher)");
        Check(SkillGroupOf(8) == SkillGroup::Ranged && SkillGroupOf(6) == SkillGroup::Melee && SkillGroupOf(20) == SkillGroup::Magic &&
              SkillGroupOf(15) == SkillGroup::Stealth && SkillGroupOf(10) == SkillGroup::Other && SkillSchoolOf(20) == kDestruction &&
              SkillSchoolOf(19) == kConjuration && SkillSchoolOf(8) == -1, "skill groups and schools");
    }
}

// ── feat/consumables (ClickUp 1dfcuabd2t0 / 1dfcuabd334): food and drink as candidates, and the worth-the-gold gate. The values are labelled placeholders
// (WorthTable); what is pinned is the SHAPE: food is a candidate of the matching family, a dear item needs a bigger reason, and a dying follower drinks. ───
namespace {
    // One Knight (hp a_hp / 200, level 10) against one foe of level a_foeLevel, with a_gold on him.
    Snapshot WorthFight(float a_hp, int a_foeLevel, float a_gold) {
        Snapshot s;
        s.self = Actor("Knight", 0x14F00, a_hp, 200, 0, 0);
        s.self.sp = 100; s.self.spMax = 100; s.self.mp = 100; s.self.mpMax = 100;
        s.playerId = 0x14;
        Combatant foe = Actor("Draugr", 0x3B600, 100, 100, 100, 0, a_foeLevel);
        foe.targetId = s.self.id;
        s.foes.push_back(foe);
        s.drawn = PostureKind::Melee;
        s.hasBook = true;
        s.book.kit.gold = a_gold;
        return s;
    }
    const Candidate* FindCand(const Generated& g, Family a_f, std::uint32_t a_form) {
        for (const auto& c : g.actions) if (c.family == a_f && c.form == a_form) return &c;
        return nullptr;
    }
}

static void ConsumablesWorth() {
    std::printf("\n=== 15. Consumables (feat/consumables): food candidates and the worth-the-gold gate ===\n");
    const Tuning& t = DefaultTuning();
    {   // the pure rule
        const WorthTable& w = t.worth;
        Check(Worth(1.0f, 1.0f) == 1.0f && Worth(0.0f, 0.0f) == 0.0f && Worth(0.0f, 1.0f) == 0.0f, "worth: need 1 is worth 1 whatever it costs; need 0 is worth 0");
        Check(Worth(0.5f, 1.0f) < Worth(0.5f, 0.0f), "worth: a dear item needs a bigger reason than a cheap one");
        Check(CostShare(500.0f, -1.0f, w) == 0.0f && CostShare(0.0f, 100.0f, w) == 0.0f, "cost share: unknown wealth or a free item holds nothing against the item");
        Check(CostShare(500.0f, 0.0f, w) == 1.0f && CostShare(10.0f, 100000.0f, w) < 0.01f, "cost share: dear to a broke follower, nothing to a rich one");
        Check(ResourceNeed(0.2f, 0.0f, 0.34f, true, w) == 1.0f && ResourceNeed(0.9f, 0.0f, 0.34f, true, w) == 0.0f, "resource need: a vital resource in real danger is 1 with no fight; healthy and no fight is 0");
        Check(ResourceNeed(0.2f, 0.0f, 0.34f, false, w) == 0.0f, "resource need: a non-vital resource follows the fight's pressure");
        Check(Name(Cons::ConsumableWorth) == "consumable_worth" && static_cast<std::size_t>(Cons::ConsumableWorth) == static_cast<std::size_t>(Cons::BoostPosturePerks) + 1,
              "the new consideration is APPENDED and named");
    }
    {   // FOOD is a candidate of the matching family (marth: potions, food and drink, every effect)
        Snapshot s = WorthFight(40.0f, 10, 1000.0f);
        PotionBuff& f = s.book.kit.restoreFood[0];
        f.form = 0xF00D; f.count = 2; f.magnitude = 40.0f; f.value = 5.0f; f.restoreMask = 1; f.name = "Venison Stew";
        Generated g = Generate(s, ClassGate{}, t);
        const Candidate* c = FindCand(g, Family::DrinkHealth, 0xF00D);
        Check(c && c->f.stock == 2 && c->f.consumableWorth > 0.9f, "food: a restore food is a DrinkHealth candidate (its form set) with its worth, no potion carried");
        Check(c && ScoreOne(*c, Mind{}, Profile{}, t, 3, 0.0f, nullptr) > t.actFloor, "food: and it scores when he is nearly dead");
        s.book.kit.potionsHealth = 1;
        g = Generate(s, ClassGate{}, t);
        Check(FindCand(g, Family::DrinkHealth, 0xF00D) == nullptr && FindCand(g, Family::DrinkHealth, 0) != nullptr, "food: not offered while he carries a restore potion of that resource");
        s.book.kit.potionsHealth = 0;
        f.magnitude = 2.0f;   // an apple against 200 health
        g = Generate(s, ClassGate{}, t);
        Check(FindCand(g, Family::DrinkHealth, 0xF00D) == nullptr, "food: a restore below WorthTable::foodMinGain of the resource's maximum is not a heal");
    }
    {   // a MIXED item counts each effect: a fortify food that also restores health is wanted by a dying follower against a weak foe
        Snapshot s = WorthFight(30.0f, 3, 1000.0f);   // a mudcrab: pressure far below the floor
        PotionBuff& b = s.book.kit.buff[static_cast<std::size_t>(BuffKind::FortifyStamina)];
        b.form = 0xF00E; b.count = 1; b.magnitude = 30.0f; b.duration = 60.0f; b.value = 20.0f; b.name = "Mixed";
        b.kindMask = 1u << static_cast<unsigned>(BuffKind::FortifyStamina);
        Check(BuffScore(s, BuffKind::FortifyStamina) < 0.0f, "worth: a fortify is not offered against a weak foe (the level rule's replacement)");
        b.restoreMask = 1;   // it also restores health
        Check(BuffScore(s, BuffKind::FortifyStamina) > 0.0f, "mixed item: the health restore it carries counts, and the dying follower takes it");
    }
    {   // the gate: a dear item needs a bigger reason, a rich follower pays more readily (marth: "worth the gold cost of the potion")
        Snapshot poor = WorthFight(180.0f, 7, 0.0f);    // pressure 0.35
        PotionBuff& b = poor.book.kit.buff[static_cast<std::size_t>(BuffKind::FortifyHealth)];
        b.form = 0xF00F; b.count = 1; b.magnitude = 50.0f; b.duration = 60.0f; b.value = 1000.0f; b.name = "Elixir";
        b.kindMask = 1u << static_cast<unsigned>(BuffKind::FortifyHealth);
        Snapshot rich = poor; rich.book.kit.gold = 100000.0f;
        Check(BuffScore(poor, BuffKind::FortifyHealth) < 0.0f, "worth gate: a dear fortify is not worth it to a broke follower in a middling fight");
        Check(BuffScore(rich, BuffKind::FortifyHealth) > t.actFloor, "worth gate: the same fortify is worth it to a rich one");
        Snapshot hard = poor; hard.foes[0].level = 20;   // pressure 1: the fight asks for everything
        Check(BuffScore(hard, BuffKind::FortifyHealth) > t.actFloor, "worth gate: and a hard fight justifies it even to the broke one (need dominates cost)");
    }
    {   // LOW HP OVERRIDE: a dying follower drinks the dearest potion whatever his purse
        Snapshot s = WorthFight(30.0f, 10, 0.0f);
        s.book.kit.potionsHealth = 1; s.book.kit.restoreValue[0] = 5000.0f;
        const Generated g = Generate(s, ClassGate{}, t);
        const Candidate* c = FindCand(g, Family::DrinkHealth, 0);
        Check(c && c->f.consumableWorth == 1.0f, "low hp: the need is 1, so the dearest potion's worth is 1 for a follower with no gold");
        Check(c && ScoreOne(*c, Mind{}, Profile{}, t, 3, 0.0f, nullptr) > t.actFloor, "low hp: and he still drinks it (score above the floor)");
        Snapshot ok = WorthFight(190.0f, 10, 0.0f);
        ok.book.kit.potionsHealth = 1; ok.book.kit.restoreValue[0] = 5000.0f;
        const Generated g2 = Generate(ok, ClassGate{}, t);
        const Candidate* c2 = FindCand(g2, Family::DrinkHealth, 0);
        Check(c2 && ScoreOne(*c2, Mind{}, Profile{}, t, 3, 0.0f, nullptr) <= 0.0f, "healthy: the same dear potion is not worth drinking (vetoed by worth)");
    }
    {   // SCARCITY + the BUFF-potion target (marth 11-13: "scarcity should also factor in ... prevent invis and fortify potions from being spammed when they would
        // help, and not when they are needed")
        const WorthTable& w = t.worth;
        Check(CostShare(200.0f, 0.0f, w, 6) < CostShare(200.0f, 0.0f, w, 1) && CostShare(200.0f, 0.0f, w, 1) == CostShare(200.0f, 0.0f, w), "scarcity: a full stack is cheaper to use than the last one");
        Check(CostShare(200.0f, 0.0f, w, 1, true) < CostShare(200.0f, 0.0f, w, 1, false), "scarcity: a restore item's cost is held lightly");
        Snapshot easy = WorthFight(190.0f, 4, 100.0f);   // a foe far below him: comfortably won
        PotionBuff& b = easy.book.kit.buff[static_cast<std::size_t>(BuffKind::FortifyHealth)];
        b.form = 0xF010; b.count = 1; b.magnitude = 50.0f; b.duration = 60.0f; b.value = 100.0f; b.name = "Fortify";
        b.kindMask = 1u << static_cast<unsigned>(BuffKind::FortifyHealth);
        Snapshot hardFight = easy; hardFight.foes[0].level = 20;
        Check(BuffScore(easy, BuffKind::FortifyHealth) < 0.0f, "buff target: a fortify in a fight he wins comfortably is vetoed (would help, not needed)");
        Check(BuffScore(hardFight, BuffKind::FortifyHealth) > t.actFloor, "buff target: the same fortify in a hard fight is drunk");
        Snapshot hurt = WorthFight(100.0f, 4, 100.0f);   // half health, a weak foe: pressure low
        hurt.book.kit.potionsHealth = 6; hurt.book.kit.restoreValue[0] = 300.0f;
        const Generated gh = Generate(hurt, ClassGate{}, t);
        const Candidate* ch = FindCand(gh, Family::DrinkHealth, 0);
        Check(ch && ch->f.consumableWorth >= w.floor, "restore: a hurt follower with a stack of potions is not held back by the gate in a weak fight");
    }
    {   // CANNOT-WIN FLOOR (review F1): a mage with an empty pool and a fight that still needs casts drinks, whatever the pressure or his purse
        Snapshot s = WorthFight(190.0f, 6, 0.0f);   // one 0.6x foe: pressure 0.3
        s.self.mp = 0; s.self.mpMax = 100;
        s.drawn = PostureKind::None;
        SpellEntry sp = Spell("Firebolt", 0x100, Prune::Nature::Offense, 20.0f);
        sp.damage = 40.0f; sp.measured = true;
        s.book.entries.push_back(sp);
        s.book.kit.potionsMagicka = 1; s.book.kit.restoreValue[2] = 300.0f;
        const Generated g = Generate(s, ClassGate{}, t);
        const Candidate* c = FindCand(g, Family::DrinkMagicka, 0);
        Check(c && c->f.consumableWorth == 1.0f, "cannot win: a mage at 0 magicka with casts wanted needs the potion at 1 (0 gold, a 0.6x foe)");
        Check(c && ScoreOne(*c, Mind{}, Profile{}, t, 3, 0.0f, nullptr) > t.actFloor, "cannot win: and he drinks it");
        Snapshot armed = s; armed.book.kit.melee = true;
        const Generated ga = Generate(armed, ClassGate{}, t);
        const Candidate* ca = FindCand(ga, Family::DrinkMagicka, 0);
        Check(ca && ca->f.consumableWorth < 1.0f, "cannot win: a mage who carries a weapon has another offense (the floor is off: worth below 1, the restore use weight alone)");
        Snapshot full = s; full.self.mp = 100;
        const Generated gf = Generate(full, ClassGate{}, t);
        const Candidate* cf = FindCand(gf, Family::DrinkMagicka, 0);
        Check(cf && cf->f.consumableWorth < t.worth.floor, "cannot win: a full pool in a weak fight is still vetoed for a dear potion");
        // stamina: a power-attack class with a melee weapon at ~0 stamina
        Snapshot k = WorthFight(190.0f, 6, 0.0f);
        k.self.sp = 0; k.book.kit.melee = true; k.book.kit.potionsStamina = 1; k.book.kit.restoreValue[1] = 300.0f;
        ClassGate pg; pg.powerAttack = true;
        const Candidate* cs = nullptr;
        const Generated gs = Generate(k, pg, t);
        cs = FindCand(gs, Family::DrinkStamina, 0);
        Check(cs && cs->f.consumableWorth == 1.0f, "cannot win: a power-attack melee class at 0 stamina needs the potion at 1");
        const Generated gs2 = Generate(k, ClassGate{}, t);
        const Candidate* cs2 = FindCand(gs2, Family::DrinkStamina, 0);
        Check(cs2 && cs2->f.consumableWorth < 1.0f, "cannot win: a class whose offense does not need stamina keeps the pressure rule");
        // health: a fast-dying follower above the band
        const WorthTable& w = t.worth;
        Check(DeathWinFloor(2.0f, w) == 1.0f && DeathWinFloor(w.ttlFloorSec + w.ttlSpanSec, w) == 0.0f && DeathWinFloor(1.0e30f, w) == 0.0f &&
              ResourceNeed(0.8f, 0.1f, 0.34f, true, w, DeathWinFloor(2.0f, w)) == 1.0f, "cannot win: a follower who dies in 2 s at 80% health needs a heal at 1; nothing hitting = no floor");
    }
}

namespace {
    // IAUS LOOTING (ClickUp 1dfcuabd18f): the pure scores LootNeed.cpp feeds. Placeholder weights; these check the SHAPE of the rulings.
    void LootNeedScores() {
        std::printf("\n=== IAUS looting scores ===\n");
        LootInv archer;
        archer.ranged = true; archer.usesArrows = true; archer.arrows = 0; archer.arrowTarget = 20; archer.capacity = 300.0f; archer.carried = 50.0f;
        const auto a1 = ScoreLootKind(LootKind::Arrows, archer);
        std::printf("  archer arrows: %.2f %s\n", static_cast<double>(a1.score), a1.why.c_str());
        Check(a1.take && a1.score > 0.9f, "ammo need: uses the kind + carries a launcher, 0/20 -> take near 1");
        LootInv noBow = archer; noBow.usesArrows = false; noBow.arrowTarget = 0; noBow.carried = 290.0f;
        const auto a2 = ScoreLootKind(LootKind::Arrows, noBow);
        Check(!a2.take && a2.why.find("unused") != std::string::npos, "ammo without the launcher: unused, left");
        archer.arrows = 20;
        Check(ScoreLootKind(LootKind::Bolts, archer).why.find("unused") != std::string::npos, "bolts read the bolt fields (no crossbow: unused)");
        Check(ScoreLootKind(LootKind::Arrows, noBow).score == 0.0f && !ScoreLootKind(LootKind::Arrows, noBow).take, "unused ammo: no sell road -> leave (F2)");
        Check(ScoreLootKind(LootKind::Arrows, archer).why.find("need 20/20") != std::string::npos &&
              !ScoreLootKind(LootKind::Arrows, archer).take, "ammo stocked to target: no need");

        LootInv melee; melee.melee = true; melee.capacity = 300.0f; melee.carried = 100.0f;
        Consumable heal; heal.restoreMag[kRestoreHealth] = 50.0f; heal.value = 10.0f; heal.weight = 0.5f;
        const auto h1 = ScoreConsumable(heal, 10, melee);
        std::printf("  heal x10, 0 carried: %.2f n=%d %s\n", static_cast<double>(h1.score), h1.takeCount, h1.why.c_str());
        Check(h1.take && h1.takeCount == DefaultLootTuning().healthTarget, "health potion need: takes only the shortfall");
        LootInv stocked = melee; stocked.restore[kRestoreHealth] = 6;
        Check(!ScoreConsumable(heal, 10, stocked).take, "health stocked + low gold per weight: left");
        Consumable magicka; magicka.restoreMag[kRestoreMagicka] = 50.0f; magicka.value = 10.0f; magicka.weight = 0.5f;
        Check(!ScoreConsumable(magicka, 3, melee).take, "magicka potion for a non-caster: sale only (left at 20 g/w)");

        Consumable bread; bread.food = true; bread.restoreMag[kRestoreStamina] = 10.0f; bread.value = 2.0f; bread.weight = 0.2f;
        const auto f1 = ScoreConsumable(bread, 3, melee);
        Check(!f1.take, "food never creates a restore need (the stock count cannot see it): left (F1)");
        Consumable stamPot; stamPot.restoreMag[kRestoreStamina] = 40.0f; stamPot.value = 20.0f; stamPot.weight = 0.5f;
        LootInv none = melee;
        const auto s1 = ScoreConsumable(stamPot, 10, none);
        Check(s1.take && s1.takeCount == DefaultLootTuning().staminaTarget, "stamina potion need: takes the shortfall");
        LootInv after = none; after.restore[kRestoreStamina] += s1.takeCount;   // what the take does to the stock read
        Check(!ScoreConsumable(stamPot, 10, after).take && ScoreConsumable(stamPot, 10, after).score < s1.score, "a take lowers the need (stock = item read)");
        Consumable foodBuff; foodBuff.food = true; foodBuff.fx.push_back(PotionFx{ BuffKind::ResistFire, 30.0f, 60.0f, -1 }); foodBuff.value = 5.0f; foodBuff.weight = 0.3f;
        Check(ScoreConsumable(foodBuff, 1, melee).take, "a food's buff kinds still count (both sides read them)");

        Consumable resist; resist.fx.push_back(PotionFx{ BuffKind::ResistFrost, 50.0f, 60.0f, -1 }); resist.value = 30.0f; resist.weight = 0.5f;
        Check(ScoreConsumable(resist, 1, melee).take, "a buff kind IAUS drinks (resist): carried");
        Consumable breath; breath.fx.push_back(PotionFx{ BuffKind::WaterBreathing, 1.0f, 60.0f, -1 }); breath.value = 10.0f; breath.weight = 0.5f;
        Check(!ScoreConsumable(breath, 1, melee).take, "water breathing (not drunk): sale only");

        Consumable poison; poison.poison = true; poison.hostile = true; poison.poisonPower = 150.0f; poison.value = 10.0f; poison.weight = 0.5f;
        const auto p1 = ScoreConsumable(poison, 5, melee);
        Check(p1.take && p1.takeCount == DefaultLootTuning().poisonTarget, "poison: weapon user carries up to the target");
        LootInv caster; caster.caster = true; caster.capacity = 300.0f; caster.carried = 100.0f;
        Check(!ScoreConsumable(poison, 5, caster).take, "poison for a pure caster: sale only");
        Consumable drug = heal; drug.hostile = true;
        Check(!ScoreConsumable(drug, 5, melee).take, "a drug (hostile effects, no poison flag): no drink need");

        LootInv full = melee; full.carried = 300.0f;
        Check(!ScoreLootKind(LootKind::Equipment, full).take && ScoreLootKind(LootKind::Gold, full).take,
              "full pack: equipment left, weightless gold still taken");
        Check(ScoreLootKind(LootKind::Lockpicks, melee).take, "lockpicks below target: take");

        // F3: a default follower (everything stocked, no launcher, no weapon to charge) must not take all the categories.
        LootInv stockedAll = melee; stockedAll.restore = { 6, 4, 0 }; stockedAll.lockpicks = 10;
        int taken = 0; for (std::size_t i = 0; i < kLootKinds; ++i) if (ScoreLootKind(static_cast<LootKind>(i), stockedAll).take) ++taken;
        std::printf("  stocked default follower takes %d of %zu categories\n", taken, kLootKinds);
        Check(!ScoreLootKind(LootKind::Arrows, stockedAll).take &&
              !ScoreLootKind(LootKind::Bolts, stockedAll).take && !ScoreLootKind(LootKind::Ingredients, stockedAll).take &&
              !ScoreLootKind(LootKind::SoulGems, stockedAll).take && !ScoreLootKind(LootKind::Lockpicks, stockedAll).take,
              "stocked default follower leaves ammo, ingredients, gems, lockpicks (F2/F3)");
        // Soul gems: a USE value only while an enchanted weapon is below max charge (marth 2026-11-13).
        LootInv charger = stockedAll; charger.weaponChargeFrac = 0.4f;
        Check(ScoreLootKind(LootKind::SoulGems, charger).take && !ScoreLootKind(LootKind::SoulGems, stockedAll).take, "soul gems: taken for a weapon below 50% charge, else left");
        // marth 2026-11-13: cap em, and a weapon at or above 50% charge triggers no looting of gems.
        LootInv half = charger; half.weaponChargeFrac = 0.5f;
        Check(!ScoreLootKind(LootKind::SoulGems, half).take, "soul gems: a weapon at 50% charge does not trigger gem looting");
        LootInv capped = charger; capped.soulGems = LootTuning{}.soulGemTarget - 1;
        Check(ScoreLootKind(LootKind::SoulGems, capped).take, "soul gems: one short of the cap still taken");
        capped.soulGems = LootTuning{}.soulGemTarget;
        Check(!ScoreLootKind(LootKind::SoulGems, capped).take && ScoreLootKind(LootKind::SoulGems, capped).score == 0.0f, "soul gems: need drops to 0 at the cap");
    }
}

// field 11-17f (Jesper, "spell stuttering"): the cast-delivery bounds are rows of the one tuning table. The bounds themselves live in the cast layer
// (CastOn / HealRoad / Hands / OffenseApproach, engine-bound), so what this checks is the table: shipped values, `t.<name>` setters, and the range guard.
void CastStutterRows() {
    std::printf("\n=== field 11-17f: cast-delivery bound rows ===\n");
    Tuning t = ShippedTuning();
    Check(t.occludedClaimReleaseSec == t.occlusionHoldSec, "occluded-claim release: ships at the pick's own occlusion hold, so claim and pick give up together");
    Check(t.offenseApproachBackoffSec > 3.0f && t.sharedRadiusMarginUnits > 0.0f && t.preemptFireSettleSec > 0.0f, "shipped rows are on (placeholders)");
    std::string why;
    Check(SetTuningScalar(t, "t.occClaimRelease", 7.0f, &why) && t.occludedClaimReleaseSec == 7.0f, "t.occClaimRelease sets occludedClaimReleaseSec");
    Check(SetTuningScalar(t, "t.offApproachBackoff", 9.0f, &why) && t.offenseApproachBackoffSec == 9.0f, "t.offApproachBackoff sets offenseApproachBackoffSec");
    Check(SetTuningScalar(t, "t.sharedRadiusMargin", 150.0f, &why) && t.sharedRadiusMarginUnits == 150.0f, "t.sharedRadiusMargin sets sharedRadiusMarginUnits");
    Check(SetTuningScalar(t, "t.preemptSettle", 0.0f, &why) && t.preemptFireSettleSec == 0.0f, "t.preemptSettle 0 = off");
    Check(SetTuningScalar(t, "t.offApproachDropCap", 2.0f, &why) && t.offenseApproachDropCap == 2.0f, "t.offApproachDropCap sets offenseApproachDropCap");
    Check(!SetTuningScalar(t, "t.occClaimRelease", -1.0f, &why), "a negative bound is refused");
}


// ABI v46 adoption (cast/SeatRows.h): the pure parts. A row is seated on its own bit AND TypeEquip, only from ABI 46; the need of a
// target; the zero-effect reading rules.
static void SeatRowRules() {
    using namespace MFO::SeatRows;
    const std::uint32_t all = (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | kTypeEquipBit;
    Check(RowSeated(46, all, Row::Stagger) && RowSeated(46, all, Row::Disarm) && RowSeated(46, all, Row::TargetEffect) && RowSeated(46, all, Row::Paralyze),
          "v46: every hostile row is seated on its bit and TypeEquip");
    Check(!RowSeated(45, all, Row::Disarm), "v46: below ABI 46 no row is seated, whatever the mask says (an older Harbinger)");
    Check(!RowSeated(46, 0, Row::Stagger), "v46: an empty coverage seats nothing");
    Check(!RowSeated(46, all & ~kTypeEquipBit, Row::Paralyze), "v46: the row bit without TypeEquip is not served");
    Check(!RowSeated(46, (1u << 11) | kTypeEquipBit, Row::Stagger) && RowSeated(46, (1u << 11) | kTypeEquipBit, Row::Disarm),
          "v46: a row is seated by its OWN bit only");
    Check(NeedMet(Need::Weapon, true, false) && !NeedMet(Need::Weapon, false, true), "v46: Disarm needs a droppable right-hand weapon");
    Check(NeedMet(Need::Summoned, false, true) && !NeedMet(Need::Summoned, true, false), "v46: Command needs a summoned target");
    Check(NeedMet(Need::None, false, false), "v46: a row with no need always passes");
    {   // the zero-effect reading (review F2 / F3)
        ReadInput w; w.keyedDurationSec = 20.0f; w.ageSec = 3.0f;
        Check(Judge(w) == Reading::Zero, "zero-effect: a lasting keyed effect absent after the settle time reads Zero");
        w.landed = true;
        Check(Judge(w) == Reading::Landed, "zero-effect: the effect on the target reads Landed");
        w.landed = false; w.ageSec = 1.0f;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: read before the settle time is inconclusive");
        w.ageSec = 7.0f;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: read after the upper age is inconclusive");
        w.ageSec = 3.0f; w.keyedDurationSec = 3.5f;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: a duration that may already have ended (age + margin) is inconclusive");
        w.keyedDurationSec = 0.0f;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: an instant keyed effect is never judged");
        w.keyedDurationSec = 20.0f; w.concentration = true;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: a concentration spell is never judged");
        w.concentration = false; w.otherHostileInstant = true;
        Check(Judge(w) == Reading::Inconclusive, "zero-effect: a spell with another hostile instant effect is never judged");
        ReadInput d; d.disarm = true; d.ageSec = 3.0f; d.landed = false;
        Check(Judge(d) == Reading::Zero, "zero-effect: Disarm with the weapon still in the right hand reads Zero (instant is fine for Disarm)");
        d.hadSubject = false;
        Check(Judge(d) == Reading::Inconclusive, "zero-effect: Disarm with no weapon seen at the fire reads nothing");
        int z = 0;
        z = Tally(z, Reading::Zero);
        Check(z < kZerosToHold, "zero-effect: one zero reading holds nothing (a miss or one resist)");
        z = Tally(z, Reading::Inconclusive);
        Check(z == 1, "zero-effect: an inconclusive reading leaves the count");
        z = Tally(z, Reading::Landed);
        Check(z == 0, "zero-effect: a landed reading resets the count");
        z = Tally(Tally(z, Reading::Zero), Reading::Zero);
        Check(z >= kZerosToHold, "zero-effect: two consecutive zero readings hold the pair");
    }
    HoldLedger h;
    Check(!h.Held(1, 2, 3), "zero-effect: nothing held at the start");
    Check(h.Hold(1, 2, 3) && !h.Hold(1, 2, 3), "zero-effect: the first hold is new (logged once), the repeat is not");
    Check(h.Held(1, 2, 3) && !h.Held(1, 2, 4) && !h.Held(1, 9, 3) && !h.Held(7, 2, 3), "zero-effect: the hold is per follower, spell AND target");
    h.Hold(1, 2, 4);
    h.ClearFollower(1);
    Check(!h.Held(1, 2, 3) && h.Size(1) == 0, "zero-effect: the hold ends with the fight (the follower's pairs)");
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
    ReachSequences();
    EngineFillRules();
    SpellWorthRules();
    SpellWorthReader();
    OcclusionHoldBound();
    HideHold();
    DisengageRules();
    BatchI();
    ClassCurveRows();
    SchoolAffinityRows();
    SpellPerkRows();
    HandLostRows();
    Field1117bRows();
    CastStutterRows();
    BuffPotions();
    ConsumablesWorth();
    LootNeedScores();
    SeatRowRules();
    g_failed += MoveRows(MOVE_GOLDEN, false);
    std::printf("\n%d check(s) failed\n", g_failed);
    return g_failed;
}
