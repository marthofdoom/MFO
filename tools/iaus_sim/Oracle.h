#pragma once
// iaus_sim/Oracle.h -- the EXPERT ORACLE: the "skilled human player" upper bound IAUS is measured against. HOST-ONLY.
//
// A rollout search with full information and no time budget over the SAME action set (Generate's candidates plus
// "let his own AI run") and the SAME world. At each decision of a free follower every option is tried in a copy of
// the fight, held for a commitment (as a person holds a choice), rolled H seconds forward with the base policy for
// everyone, over K random streams shared by all options, and scored by Utility(). The best option is taken. A
// written ruling acts as a constraint: an option a ruling forbids is never tried (Constraint).
//
// Every decision where IAUS's own pick differs from the oracle's is logged with the utility it cost (Disagreement):
// the gap ledger's evidence.
#include "iaus_sim/Policy.h"

#include <functional>

namespace Sim {

    // How good a fight state is for the party, 0..100. One objective for every policy: win, fast, the player and
    // the party healthy, nobody down, resources left.
    inline double Utility(const Fight& F) {
        double foeHp = 0; for (const Body& x : F.b) if (x.role == Role::Foe && x.alive && x.airUntil >= 0) foeHp += std::max(0.0f, x.c.hp);
        for (const Spawn& s : F.late) foeHp += s.spec->hp;
        const double removed = F.foeHpTotal > 0 ? std::clamp(1.0 - foeHp / F.foeHpTotal, 0.0, 1.0) : 1.0;
        const Body& pl = F.b[0];
        if (!pl.alive) return 10.0 * removed;
        const bool win = foeHp <= 0.0;
        double partyHp = 0, res = 0; const double nf = std::max<double>(1, double(F.ctl.size()));
        for (const Ctl& c : F.ctl) { const Body& x = F.b[static_cast<std::size_t>(c.body)];
            partyHp += Fight::Up(x) ? x.c.hp / std::max(1.0f, x.c.hpMax) : 0.0;
            const int pots = c.book.kit.potionsHealth + c.book.kit.potionsMagicka + c.book.kit.potionsStamina;
            res += 0.5 * (x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 1.0) + 0.5 * (c.potions0 > 0 ? double(pots) / c.potions0 : 1.0); }
        return 50.0 * removed + (win ? 15.0 * (1.0 - std::min(1.0, F.now / F.maxSec)) : 0.0) + 15.0 * pl.c.hp / std::max(1.0f, pl.c.hpMax)
             + 10.0 * (1.0 - std::min(1.0, double(F.t.partyDowns) / nf)) + 5.0 * partyHp / nf + 5.0 * res / nf;
    }

    struct Disagreement {      // one cluster: IAUS picked a, the oracle picked b
        long n = 0; double dU = 0;
        // means over the cluster, IAUS's pick then the oracle's (i / o)
        double iFoeHp = 0, oFoeHp = 0, iOnPlayer = 0, oOnPlayer = 0, iCaster = 0, oCaster = 0, iResist = 0, oResist = 0, iRecHp = 0, oRecHp = 0, selfHp = 0, selfMp = 0, foes = 0, sameTarget = 0, iDist = 0, oDist = 0;
    };

    struct OracleCfg { int K = 2; double H = 8.0, every = 0.4, commit = 2.5; std::size_t maxOptions = 14; double minDU = 1.0; bool allFollowers = true; };

    // The rollout policy inside the search: IAUS, except that a follower holds an option he was given until commitUntil.
    struct RolloutPolicy : Policy {
        IausPolicy iaus;
        void Serve(Fight& F, int ci) override { if (F.now < F.ctl[static_cast<std::size_t>(ci)].commitUntil) return; iaus.Serve(F, ci); }
    };

    struct OraclePolicy : Policy {
        OracleCfg cfg;
        IausPolicy iaus;                       // followers the oracle does not drive, and IAUS's own pick for the log
        std::function<bool(const Ctl&, const Candidate&)> allowed;   // rulings as constraints (nullptr = everything)
        std::map<std::string, Disagreement>* log = nullptr;
        std::vector<double> last;
        long decisions = 0, disagreed = 0; double dUSum = 0;

        static void RunFor(Fight& G, Policy& P, double until) {
            while (!G.over && G.now < until) {
                if (G.now >= G.nextPump && !G.ctl.empty()) { G.nextPump += TM::pump; P.Serve(G, G.rr); G.rr = (G.rr + 1) % static_cast<int>(G.ctl.size()); }
                G.StepWorld();
            }
        }
        // Apply option `k` (nullptr = let his own AI run) for follower ci in G. false = it cannot be done now.
        static bool Apply(Fight& G, int ci, const Candidate* k, double commit) {
            Ctl& c = G.ctl[static_cast<std::size_t>(ci)]; Body& x = G.b[static_cast<std::size_t>(c.body)];
            if (!k) { c.steer = 0; c.commitUntil = G.now + commit; return true; }
            if (k->family == Family::Posture) {
                if (k->f.posture == c.drawn || k->f.posture == PostureKind::None || x.hitAt >= 0) return false;
                c.drawn = k->f.posture; x.act = Act::Swap; x.actEnd = G.now + TM::swap; x.hitAt = -1; x.actIaus = true; c.commitUntil = x.actEnd; return true;
            }
            if (Fire(G, ci, *k) != Fired::Yes) return false;
            c.firedAt[CooldownId(KeyOf(*k))] = G.now;
            c.commitUntil = (k->family == Family::Attack || k->family == Family::Retreat) ? G.now + commit : std::max(G.now + 0.3, x.actEnd);
            return true;
        }
        void Serve(Fight& F, int ci) override {
            if (!cfg.allFollowers && ci != 0) { iaus.Serve(F, ci); return; }
            if (last.size() < F.ctl.size()) last.assign(F.ctl.size(), -1e9);
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; const bool subj = ci == 0;
            if (!Fight::Up(x)) return;
            const bool held = x.hitAt >= 0 || F.now < x.staggerUntil || F.now < x.ctrlUntil || (x.actIaus && F.now < x.actEnd);
            Served sv; MakeSnapshot(F, ci, sv.sn); sv.g = Generate(sv.sn, c.gate, *iaus.T);
            FillCooldowns(c, sv.g.actions, F.now, *iaus.T); FillCooldowns(c, sv.g.postures, F.now, *iaus.T);
            TickIn in; in.now = F.now; in.rapportRank = 5; in.seed = c.seed; in.commitWindowSec = c.window;
            sv.d = Decide(c.mind, sv.g.actions, c.profile, *iaus.T, in);
            if (subj) IausPolicy::Account(F, c, x, sv);
            if (held || F.now - last[static_cast<std::size_t>(ci)] < cfg.every) return;
            last[static_cast<std::size_t>(ci)] = F.now;
            // the options: IAUS's ranked list first, then the best-looking candidate of every family it did not rank, then postures, then idle
            const auto& acts = sv.g.actions; std::vector<const Candidate*> opt; std::vector<Key> seen;
            auto add = [&](const Candidate* k) { if (opt.size() >= cfg.maxOptions) return; if (allowed && !allowed(c, *k)) return; const Key key = KeyOf(*k); if (std::find(seen.begin(), seen.end(), key) != seen.end()) return; seen.push_back(key); opt.push_back(k); };
            const Candidate* iausPick = nullptr;
            if (sv.d.verdict != Verdict::Idle && !sv.d.order.empty() && sv.d.order[0].idx < acts.size()) iausPick = &acts[sv.d.order[0].idx];
            for (std::size_t i = 0; i < sv.d.ranked.size() && opt.size() < cfg.maxOptions - 4; ++i) if (sv.d.ranked[i].idx < acts.size()) add(&acts[sv.d.ranked[i].idx]);
            for (std::size_t f = 0; f < kFamilies; ++f) {   // one unranked option per family and target class: the weakest foe, the foe on the player, a caster
                const Candidate* lo = nullptr; const Candidate* onP = nullptr; const Candidate* cast = nullptr; const Candidate* rec = nullptr;
                for (const Candidate& k : acts) { if (static_cast<std::size_t>(k.family) != f) continue;
                    if (!lo || k.f.foeHpPct < lo->f.foeHpPct) lo = &k; if (k.f.foeOnPlayer && !onP) onP = &k; if (k.f.foeCaster && !cast) cast = &k; if (!rec || k.f.recipientHp < rec->f.recipientHp) rec = &k; }
                for (const Candidate* k : { lo, onP, cast, rec }) if (k) add(k);
            }
            for (const Candidate& k : sv.g.postures) if (k.f.posture != c.drawn) { if (opt.size() < cfg.maxOptions + 2) { opt.push_back(&k); } }
            opt.push_back(nullptr);   // idle: his own AI
            // evaluate
            std::vector<double> u(opt.size(), -1e9); const std::uint64_t s0 = F.R.g();
            for (std::size_t i = 0; i < opt.size(); ++i) {
                double sum = 0; int n = 0;
                for (int k = 0; k < cfg.K; ++k) {
                    Fight G = F; G.R.g.seed(s0 + 7919ULL * static_cast<std::uint64_t>(k));
                    if (!Apply(G, ci, opt[i], cfg.commit)) break;
                    RolloutPolicy rp; RunFor(G, rp, F.now + cfg.H); sum += Utility(G); ++n;
                }
                if (n) u[i] = sum / n;
            }
            // pick: the best; the action he is already on wins a near tie (a person does not dither)
            std::size_t best = opt.size() - 1; for (std::size_t i = 0; i < opt.size(); ++i) if (u[i] > u[best]) best = i;
            if (c.hasLastKey) for (std::size_t i = 0; i < opt.size(); ++i) if (opt[i] && KeyOf(*opt[i]) == c.lastKey && u[i] > -1e8 && u[i] >= u[best] - 0.3) { best = i; break; }
            ++decisions;
            // the disagreement with IAUS's own pick
            if (log) {
                std::size_t ii = opt.size() - 1; if (iausPick) for (std::size_t i = 0; i < opt.size(); ++i) if (opt[i] && KeyOf(*opt[i]) == KeyOf(*iausPick)) ii = i;
                const double dU = u[ii] > -1e8 ? u[best] - u[ii] : 0.0;
                if (ii != best && dU >= cfg.minDU) {
                    ++disagreed; dUSum += dU;
                    auto nm = [](const Candidate* k) { return k ? std::string(Name(k->family)) : std::string("idle"); };
                    const Candidate* a = opt[ii]; const Candidate* b = opt[best];
                    Disagreement& g = (*log)[nm(a) + " -> " + nm(b)]; ++g.n; g.dU += dU;
                    auto fill = [](const Candidate* k, double& hp, double& onP, double& cas, double& res, double& rec, double& dist) { if (!k) return; hp += k->f.foeHpPct; onP += k->f.foeOnPlayer; cas += k->f.foeCaster; res += k->f.resist; rec += k->f.recipientHp; dist += k->f.dist; };
                    fill(a, g.iFoeHp, g.iOnPlayer, g.iCaster, g.iResist, g.iRecHp, g.iDist); fill(b, g.oFoeHp, g.oOnPlayer, g.oCaster, g.oResist, g.oRecHp, g.oDist);
                    g.selfHp += x.c.hp / std::max(1.0f, x.c.hpMax); g.selfMp += x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 1; g.foes += double(sv.sn.foes.size()); g.sameTarget += a && b && a->target == b->target;
                }
            }
            // act
            const Candidate* k = opt[best]; Tally& t = F.t;
            const Key key = k ? KeyOf(*k) : Key{ Family::kCount, PostureKind::None, 0, 0 };
            if (!Apply(F, ci, k, 0.0)) return;
            if (F.trace && (!c.hasLastKey || !(c.lastKey == key))) std::fprintf(F.trace, "%7.2f  ORACLE %s: %s%s%s -> %u  U %.2f (IAUS would: %s, U %.2f)\n", F.now, x.c.name.c_str(), k ? std::string(Name(k->family)).c_str() : "idle", k && !k->formName.empty() ? " " : "", k ? std::string(k->formName).c_str() : "", k ? k->target : 0u, u[best], iausPick ? std::string(Name(iausPick->family)).c_str() : "idle", 0.0);
            c.commitUntil = 0;
            if (k && subj) { ++t.fired; if (k->family != Family::Posture) { ++t.hist[static_cast<std::size_t>(k->family)];
                    t.actSec[static_cast<std::size_t>(k->family)] += k->family == Family::Attack ? cfg.every : k->family == Family::Retreat ? TM::retreatSec : std::max(0.0, x.actEnd - F.now); } else ++t.postureSwaps;
                if (!c.hasLastKey || !(c.lastKey == key)) ++t.switches;
                if ((k->family == Family::DrinkHealth || k->family == Family::CastHeal) && c.lowSince >= 0 && !c.lowHealed && c.lowHadCand) { c.lowHealed = true; ++t.lowHealed; t.lowHealSecSum += F.now - c.lowSince; } }
            c.lastKey = key; c.hasLastKey = true;
        }
    };

}
