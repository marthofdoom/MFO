#pragma once
// iaus_sim/Oracle.h -- the EXPERT ORACLE: the "skilled human player" upper bound IAUS is measured against. HOST-ONLY.
//
// A rollout search with full information and no time budget over the same world. At each decision of a free
// follower every option is tried in a copy of the fight, held for a commitment (as a person holds a choice), rolled
// forward with the base policy for everyone over K random streams shared by all options, and scored by Utility().
// The best option is taken. A written ruling acts as a constraint: an option a ruling forbids is never tried.
//
// OPTIONS = the catalogue's candidates (Generate) + "let his own AI run" + THE EXTENDED ACTION SET (Ext below):
// what a follower can physically do that the catalogue has no family for yet (scrolls, deliberate repositioning,
// block, bash). An oracle limited to the catalogue would hide exactly those gaps.
//
// Every decision where IAUS's own pick differs from the oracle's is logged with the utility it cost (Disagreement):
// the gap ledger's evidence.
#include "iaus_sim/Policy.h"

#include <functional>

namespace Sim {

    // How good a fight state is for the party, 0..100. One objective for every policy: win, fast, the player and
    // the party healthy, nobody down, resources left.
    inline bool g_utilResources = true;   // --utility-no-resources (DIAGNOSTIC): the "resources left" term is a constant 5, so a value's gain can be split into "fights better" and "kept a potion"
    // WHAT A POTION IS WORTH (marth 11-12, Q-potion-value: "Sim calibrates it": price a potion by its worth in later fights over a chain
    // of fights, not by a fixed constant). g_potWorth holds, per class cell and per potion kind, the worth in J of the k-th potion still
    // in the bag when a fight ends (PotionWorth.h: measured by --potion-calib and the chain recursion of tools/iaus_sim/potion_worth.py).
    // A fight's Utility pays 2.5 for the bag and charges each potion drunk in it that worth. g_potLegacy (--potion-worth legacy) is the
    // older fixed rate, 2.5 / (potions carried) / (followers) per potion, kept for comparisons only.
    inline bool g_potLegacy = false;
    inline double PotionCost(const Ctl& c) {
        if (!c.worth) return 0.0;
        const int left[3] = { c.book.kit.potionsHealth, c.book.kit.potionsMagicka, c.book.kit.potionsStamina }; double cost = 0;
        for (int t = 0; t < 3; ++t) for (int k = left[t] + 1; k <= c.pot0[t]; ++k) cost += c.worth->m[static_cast<std::size_t>(t)][static_cast<std::size_t>(std::min(k, kPotWorthMax) - 1)];
        return cost;
    }
    inline double Utility(const Fight& F, bool chargePotions = true) {
        double foeHp = 0; for (const Body& x : F.b) if (x.role == Role::Foe && x.alive && x.airUntil >= 0) foeHp += std::max(0.0f, x.c.hp);
        for (const Spawn& s : F.late) foeHp += s.spec->hp;
        const double removed = F.foeHpTotal > 0 ? std::clamp(1.0 - foeHp / F.foeHpTotal, 0.0, 1.0) : 1.0;
        const Body& pl = F.b[0];
        if (!pl.alive) return 10.0 * removed;
        const bool win = foeHp <= 0.0;
        double partyHp = 0, res = 0, mp = 0, cost = 0; const double nf = std::max<double>(1, double(F.ctl.size()));
        for (const Ctl& c : F.ctl) { const Body& x = F.b[static_cast<std::size_t>(c.body)];
            partyHp += Fight::Up(x) ? x.c.hp / std::max(1.0f, x.c.hpMax) : 0.0;
            const int pots = c.book.kit.potionsHealth + c.book.kit.potionsMagicka + c.book.kit.potionsStamina;
            const double mpf = x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 1.0; mp += mpf; if (chargePotions) cost += PotionCost(c);
            res += 0.5 * mpf + 0.5 * (c.potions0 > 0 ? double(pots) / c.potions0 : 1.0); }
        const double resTerm = !g_utilResources ? 5.0 : g_potLegacy ? 5.0 * res / nf : 2.5 * mp / nf + 2.5 - cost;
        return 50.0 * removed + (win ? 15.0 * (1.0 - std::min(1.0, F.now / F.maxSec)) : 0.0) + 15.0 * pl.c.hp / std::max(1.0f, pl.c.hpMax)
             + 10.0 * (1.0 - std::min(1.0, double(F.t.partyDowns) / nf)) + 5.0 * partyHp / nf + resTerm;
    }

    // ── the extended action set ─────────────────────────────────────────────────────────────────────────
    enum class Ext : std::uint8_t { None = 0, StepBack, SprintBack, ToPlayer, ToHealer, GetSight, Block, Bash, Scroll, Hide, Flee, kCount };
    inline const char* Name(Ext e) {
        static const char* n[] = { "", "ext:step_back", "ext:sprint_back", "ext:to_player", "ext:to_healer", "ext:get_sight", "ext:block", "ext:bash", "ext:scroll", "ext:hide", "ext:flee" };
        return n[static_cast<std::size_t>(e)];
    }
    struct Opt {                       // one thing he could do now
        const Candidate* k = nullptr;  // a catalogue candidate (nullptr with ext None = his own AI)
        Ext   ext = Ext::None;
        int   idx = -1;                // Scroll: the book entry
        std::uint32_t target = 0;
        [[nodiscard]] std::string Label() const { return ext != Ext::None ? std::string(Name(ext)) : k ? std::string(MFO::Iaus::Name(k->family)) : std::string("idle"); }
        [[nodiscard]] bool Same(const Opt& o) const { return ext == o.ext && idx == o.idx && target == o.target && ((!k && !o.k) || (k && o.k && KeyOf(*k) == KeyOf(*o.k))); }
    };

    [[nodiscard]] inline bool SeesFrom(const Fight& F, float ax, float ay, const Body& o) {
        for (const Occluder& oc : F.occ) {
            const float dx = o.c.x - ax, dy = o.c.y - ay, l2 = dx * dx + dy * dy; if (l2 < 1) continue;
            const float tt = std::clamp(((oc.x - ax) * dx + (oc.y - ay) * dy) / l2, 0.0f, 1.0f);
            if (std::hypot(ax + tt * dx - oc.x, ay + tt * dy - oc.y) < oc.r) return false;
        }
        return true;
    }

    // What the extended set offers this follower right now.
    inline void ExtOptions(const Fight& F, int ci, std::vector<Opt>& out) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& x = F.b[static_cast<std::size_t>(c.body)];
        const Body* near = nullptr; float nd = 1e9f; const Body* onMe = nullptr; const Body* casting = nullptr;
        for (const Body& o : F.b) { if (o.role != Role::Foe || !Fight::Up(o)) continue; const float d = Fight::Dist(x, o); if (d < nd) { nd = d; near = &o; }
            if (o.c.targetId == x.c.id && d < F.Reach(o) * 1.6f) onMe = &o; if (o.act == Act::Cast && o.hitAt >= 0 && d < F.Reach(x)) casting = &o; }
        if (!near) return;
        // Card G3's two runs as a PERSON would use them, free of the generator's gates (class flag, "a foe has found him", the 30% health
        // line): a hide run for anyone who sneaks and is detected, a flee for anyone with a foe on him and under 60% health. The road
        // and the hard cooldown still bind (a cooldown binds a person too). What the oracle gains with them is the gates' cost.
        if (g_stealth >= 3 && c.runPhase == 0) {
            const auto it = c.firedAt.find(CooldownId(DisengageKey())); const bool cool = it == c.firedAt.end() || F.now - it->second >= static_cast<double>(ActiveTuning()->disengageCooldownSec);
            float gx = 0, gy = 0;
            if (cool && x.stealthy && !c.stanceHeld && F.GroupDetects(x) && F.ConcealPick(x, c, false, gx, gy)) out.push_back({ nullptr, Ext::Hide, -1, 0 });
            if (cool && !x.stealthy && onMe && x.c.hp < 0.6f * x.c.hpMax && F.ConcealPick(x, c, true, gx, gy)) out.push_back({ nullptr, Ext::Flee, -1, 0 });
        }
        if (nd < 900.0f) { out.push_back({ nullptr, Ext::StepBack, -1, near->c.id }); if (x.c.sp > 20.0f) out.push_back({ nullptr, Ext::SprintBack, -1, near->c.id }); }
        if (Fight::Dist(x, F.b[0]) > 350.0f && F.b[0].alive) out.push_back({ nullptr, Ext::ToPlayer, -1, 0 });
        for (const Ctl& o : F.ctl) { if (&o == &c) continue; const Body& ob = F.b[static_cast<std::size_t>(o.body)]; if (!Fight::Up(ob) || Fight::Dist(x, ob) < 300.0f) continue;
            bool heals = false; for (const SimSpell& s : o.build->spells) heals |= s.healOther; if (heals && o.gate.spellsOnClass) { out.push_back({ nullptr, Ext::ToHealer, -1, ob.c.id }); break; } }
        const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId);
        if (tg && Fight::Up(*tg) && !F.Sees(x, *tg) && (c.drawn == PostureKind::Bow || c.drawn == PostureKind::Staff || c.gate.spellsOnClass)) out.push_back({ nullptr, Ext::GetSight, -1, tg->c.id });
        const bool blade = c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver;
        if (onMe && blade) out.push_back({ nullptr, Ext::Block, -1, onMe->c.id });
        if (blade && x.c.sp >= 20.0f) { if (casting) out.push_back({ nullptr, Ext::Bash, -1, casting->c.id }); else if (onMe && nd < F.Reach(x)) out.push_back({ nullptr, Ext::Bash, -1, onMe->c.id }); }
        if (c.gate.scrollUser) for (std::size_t i = 0; i < c.book.entries.size(); ++i) {   // a scroll: on the strongest foe, the foe on the player, or the most hurt friend
            const SpellEntry& e = c.book.entries[i]; if (e.source != SpellSource::Scroll || e.count <= 0) continue;
            const SimSpell& s = c.build->spells[i];
            if (s.e.nature == Prune::Nature::Offense || s.ctrl) {
                const Body* big = nullptr; const Body* onP = nullptr;
                for (const Body& o : F.b) { if (o.role != Role::Foe || !Fight::Up(o) || !F.Sees(x, o) || F.WillWork(s, o) == Tri::No) continue; if (!big || o.c.hp > big->c.hp) big = &o; if (o.c.targetId == F.b[0].c.id) onP = &o; }
                if (big) out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), big->c.id });
                if (onP && onP != big) out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), onP->c.id });
            } else if (s.e.nature == Prune::Nature::Heal) {
                const Body* hurt = nullptr; for (const Body& o : F.b) if (o.Party() && o.role != Role::Summon && o.alive && (s.healOther || &o == &x) && (!hurt || o.c.hp / o.c.hpMax < hurt->c.hp / hurt->c.hpMax)) hurt = &o;
                if (hurt && hurt->c.hp < 0.7f * hurt->c.hpMax) out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), hurt->c.id });
            } else out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), x.c.id });
        }
    }

    // THE CLASS RULINGS AS CONSTRAINTS (Playstyles.h): an option the follower's class forbids is never tried, so the oracle is an
    // expert who plays THAT class. They are OFF unless --class-rulings (or --class-oracle) is given: the default oracle is main's unconstrained one (doc sections 9 to 27).
    inline bool g_classRulings = false;   // OFF by default (main's oracle); --class-rulings, or --class-oracle, turns it on
    [[nodiscard]] inline bool ClassAllows(const Fight& F, const Ctl& c, const Opt& o) {
        const Body& x = F.b[static_cast<std::size_t>(c.body)]; const Candidate* k = o.k; const unsigned fb = c.forbid;
        if (k && k->f.hasArea && !k->f.splashSafe) return false;                                  // never splash the party (every class)
        const bool toMelee = k && k->family == Family::Posture && (k->f.posture == PostureKind::Melee || k->f.posture == PostureKind::Silver);
        const bool meleeAct = (k && (k->family == Family::PowerAttack || k->family == Family::Bash)) || o.ext == Ext::Bash;
        const bool flee = (k && k->family == Family::Retreat) || o.ext == Ext::Flee || (k && k->family == Family::Disengage && k->f.disengageFlee), back = o.ext == Ext::StepBack || (k && k->family == Family::GiveGround), run = o.ext == Ext::SprintBack,
                   hide = o.ext == Ext::Hide || (k && k->family == Family::Disengage && !k->f.disengageFlee);
        if ((fb & kPsNoMelee) && (toMelee || meleeAct)) {   // the blade only when he has nothing else left
            bool other = c.book.kit.potionsMagicka > 0;
            for (const SpellEntry& e : c.book.entries) other |= e.nature == Prune::Nature::Offense && (e.source == SpellSource::Staff ? e.charged : e.source == SpellSource::Scroll ? e.count > 0 : e.cost <= x.c.mp);
            if (other) return false; }
        if ((fb & kPsNoMeleeSwap) && toMelee && x.arrows > 0) return false;
        if ((fb & kPsGoIn) && (toMelee || meleeAct) && !F.GoInOk(c, x)) return false;
        if ((fb & kPsNoRetreat) && (flee || back || run || hide)) return false;
        if ((fb & kPsBackOnly) && (flee || run)) return false;
        if ((fb & kPsNearPlayer) && (flee || back || run) && Fight::Dist(x, F.b[0]) > 900.0f) return false;
        return true;
    }

    struct Disagreement {      // one cluster: IAUS picked a, the oracle picked b
        long n = 0; double dU = 0;
        double iFoeHp = 0, oFoeHp = 0, iOnPlayer = 0, oOnPlayer = 0, iCaster = 0, oCaster = 0, iResist = 0, oResist = 0, iRecHp = 0, oRecHp = 0, selfHp = 0, selfMp = 0, foes = 0, sameTarget = 0, iDist = 0, oDist = 0, playerHp = 0, nearDist = 0;
    };

    struct OracleCfg { int K = 8; double H = 150.0, every = 0.4, commit = 2.5; std::size_t maxOptions = 14; double minDU = 1.0; bool allFollowers = true, ext = true; };

    // The rollout policy inside the search: IAUS, except that a follower holds an option he was given until commitUntil.
    struct RolloutPolicy : Policy {
        IausPolicy iaus;
        void Serve(Fight& F, int ci) override { if (ServiceRun(F, ci)) return; if (F.now < F.ctl[static_cast<std::size_t>(ci)].commitUntil) return; iaus.Serve(F, ci); }   // card G3's run is serviced every lap, committed or not
    };

    struct OraclePolicy : Policy {
        OracleCfg cfg;
        IausPolicy iaus;                       // followers the oracle does not drive, and IAUS's own pick for the log
        std::function<bool(const Fight&, const Ctl&, const Opt&)> allowed;   // extra constraints of the caller (nullptr = none); the class rulings are ClassAllows
        [[nodiscard]] bool Ok(const Fight& F, const Ctl& c, const Opt& o) const { return (!g_classRulings || ClassAllows(F, c, o)) && (!allowed || allowed(F, c, o)); }
        std::map<std::string, Disagreement>* log = nullptr;
        std::vector<double> last;
        long decisions = 0, disagreed = 0; double dUSum = 0;

        static void RunFor(Fight& G, Policy& P, double until) {
            while (!G.over && G.now < until) {
                if (G.now >= G.nextPump && !G.ctl.empty()) { G.nextPump += TM::pump; P.Serve(G, G.rr); G.rr = (G.rr + 1) % static_cast<int>(G.ctl.size()); }
                G.StepWorld();
            }
        }
        // Apply an option for follower ci in G. false = it cannot be done now.
        static bool Apply(Fight& G, int ci, const Opt& o, double commit) {
            Ctl& c = G.ctl[static_cast<std::size_t>(ci)]; Body& x = G.b[static_cast<std::size_t>(c.body)];
            const bool held = x.hitAt >= 0 || G.now < x.staggerUntil || G.now < x.ctrlUntil || (x.actIaus && G.now < x.actEnd);
            auto move = [&](float gx, float gy, double sec, bool sprint) { c.goalX = gx; c.goalY = gy; c.moveUntil = G.now + sec; c.sprint = sprint; c.moveFight = false; c.commitUntil = c.moveUntil; return true; };
            switch (o.ext) {
            case Ext::None: break;
            case Ext::StepBack: case Ext::SprintBack: { const Body* n = G.Find(o.target); if (!n) return false; const float dx = x.c.x - n->c.x, dy = x.c.y - n->c.y, l = std::max(1.0f, std::hypot(dx, dy));
                return move(x.c.x + dx / l * 500.0f, x.c.y + dy / l * 500.0f, 2.0, o.ext == Ext::SprintBack); }
            case Ext::ToPlayer: { const Body& p = G.b[0]; return move(p.c.x - 120.0f, p.c.y + (x.c.y > p.c.y ? 100.0f : -100.0f), 3.0, false); }
            case Ext::ToHealer: { const Body* h = G.Find(o.target); if (!h) return false; return move(h->c.x - 100.0f, h->c.y, 3.0, false); }
            case Ext::GetSight: { const Body* tg = G.Find(o.target); if (!tg) return false;
                for (float r : { 250.0f, 450.0f, 700.0f }) for (int a = 0; a < 12; ++a) { const float ang = 0.5236f * static_cast<float>(a), gx = x.c.x + r * std::cos(ang), gy = x.c.y + r * std::sin(ang);
                    if (SeesFrom(G, gx, gy, *tg)) return move(gx, gy, 2.5, false); }
                return false; }
            case Ext::Hide: case Ext::Flee: { const bool fl = o.ext == Ext::Flee; float gx = 0, gy = 0; if (c.runPhase != 0 || !G.ConcealPick(x, c, fl, gx, gy)) return false;
                c.pick.valid = true; c.pick.flee = fl; c.pick.kind = fl ? 1 : 0; c.pick.x = gx; c.pick.y = gy; if (!BeginRun(G, ci, fl, true)) return false; c.commitUntil = G.now + (fl ? 2.0 : 0.3); return true; }
            case Ext::Block: if (held) return false; x.act = Act::Block; x.blockUntil = G.now + 1.5; x.actEnd = x.blockUntil; x.hitAt = -1; x.actIaus = true; x.c.blocking = true; c.commitUntil = x.actEnd; return true;
            case Ext::Bash: { Body* tg = G.Find(o.target); if (held || !tg || !Fight::Up(*tg) || Fight::Dist(x, *tg) > G.Reach(x) * 1.1f) return false; G.StartSwing(x, *tg, Act::Bash, true); c.commitUntil = x.actEnd; return true; }
            case Ext::Scroll: { if (held || o.idx < 0 || c.book.entries[static_cast<std::size_t>(o.idx)].count <= 0) return false;
                G.StartCast(x, o.idx, c.build->spells[static_cast<std::size_t>(o.idx)], o.target, 0.0f, true); c.commitUntil = x.actEnd; return true; }
            default: return false;
            }
            const Candidate* k = o.k;
            if (!k) { c.steer = 0; c.commitUntil = G.now + commit; return true; }
            if (k->family == Family::Posture) {
                if (k->f.posture == c.drawn || k->f.posture == PostureKind::None || x.hitAt >= 0) return false;
                c.drawn = k->f.posture; x.act = Act::Swap; x.actEnd = G.now + TM::swap; x.hitAt = -1; x.actIaus = true; c.commitUntil = x.actEnd; return true;
            }
            if (Fire(G, ci, *k) != Fired::Yes) return false;
            if (k->family == Family::Sneak) x.holdUntil = std::max(x.holdUntil, G.now + 0.45);   // the oracle decides every 0.4 s: its hold lasts until its next decision
            c.firedAt[CooldownId(KeyOf(*k))] = G.now;
            c.commitUntil = (k->family == Family::Attack || k->family == Family::Retreat) ? G.now + commit : std::max(G.now + 0.3, x.actEnd);
            return true;
        }
        void Serve(Fight& F, int ci) override {
            if (!cfg.allFollowers && ci != 0) { iaus.Serve(F, ci); return; }
            if (last.size() < F.ctl.size()) last.assign(F.ctl.size(), -1e9);
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; const bool subj = ci == 0;
            if (ServiceRun(F, ci)) return;   // card G3: a hide run or its hold stands
            if (!Fight::Up(x)) return;
            const bool held = x.hitAt >= 0 || F.now < x.staggerUntil || F.now < x.ctrlUntil || (x.actIaus && F.now < x.actEnd) || F.now < c.moveUntil;
            Served sv; MakeSnapshot(F, ci, sv.sn); sv.g = Generate(sv.sn, c.gate, *iaus.T);
            FillCooldowns(c, sv.g.actions, F.now, *iaus.T); FillCooldowns(c, sv.g.postures, F.now, *iaus.T);
            TickIn in; in.now = F.now; in.rapportRank = 5; in.seed = c.seed; in.commitWindowSec = c.window;
            sv.d = Decide(c.mind, sv.g.actions, c.profile, *iaus.T, in);
            if (subj) IausPolicy::Account(F, c, x, sv);
            if (held || F.now - last[static_cast<std::size_t>(ci)] < cfg.every) return;
            last[static_cast<std::size_t>(ci)] = F.now;
            // the options: IAUS's ranked list first, then the best-looking candidate of every family it did not rank, postures, the extended set, idle
            const auto& acts = sv.g.actions; std::vector<Opt> opt;
            auto add = [&](const Candidate* k) { if (opt.size() >= cfg.maxOptions) return; Opt o; o.k = k;
                if (k->f.hardCooldownSec > 0.0f && k->f.hardCooldownLeft > 0.0f) return;   /* rulings Q7 / Q69 / Q73: a hard cooldown (the power attack's, the taunt's 30 s) binds a person too */
                if (!Ok(F, c, o)) return; for (const Opt& e : opt) if (e.Same(o)) return; opt.push_back(o); };
            const Candidate* iausPick = nullptr;
            if (sv.d.verdict != Verdict::Idle && !sv.d.order.empty() && sv.d.order[0].idx < acts.size()) iausPick = &acts[sv.d.order[0].idx];
            for (std::size_t i = 0; i < sv.d.ranked.size() && opt.size() + 4 < cfg.maxOptions; ++i) if (sv.d.ranked[i].idx < acts.size()) add(&acts[sv.d.ranked[i].idx]);
            for (std::size_t f = 0; f < kFamilies; ++f) {
                const Candidate* lo = nullptr; const Candidate* onP = nullptr; const Candidate* cast = nullptr; const Candidate* rec = nullptr;
                for (const Candidate& k : acts) { if (static_cast<std::size_t>(k.family) != f) continue;
                    if (!lo || k.f.foeHpPct < lo->f.foeHpPct) lo = &k; if (k.f.foeOnPlayer && !onP) onP = &k; if (k.f.foeCaster && !cast) cast = &k; if (!rec || k.f.recipientHp < rec->f.recipientHp) rec = &k; }
                for (const Candidate* k : { lo, onP, cast, rec }) if (k) add(k);
            }
            for (const Candidate& k : sv.g.postures) if (k.f.posture != c.drawn) { Opt o; o.k = &k; if (Ok(F, c, o)) opt.push_back(o); }
            if (cfg.ext) { std::vector<Opt> ex; ExtOptions(F, ci, ex); for (const Opt& o : ex) if (Ok(F, c, o)) opt.push_back(o); }
            opt.push_back(Opt{});   // idle: his own AI
            // evaluate
            std::vector<double> u(opt.size(), -1e9); const std::uint64_t s0 = F.R.g();
            for (std::size_t i = 0; i < opt.size(); ++i) {
                double sum = 0; int n = 0;
                for (int k = 0; k < cfg.K; ++k) {
                    Fight G = F; G.trace = nullptr; G.R.g.seed(s0 + 7919ULL * static_cast<std::uint64_t>(k));
                    if (!Apply(G, ci, opt[i], cfg.commit)) break;
                    RolloutPolicy rp; RunFor(G, rp, F.now + cfg.H); sum += Utility(G); ++n;
                }
                if (n) u[i] = sum / n;
            }
            // pick: the best; the action he is already on wins a near tie (a person does not dither)
            std::size_t best = opt.size() - 1; for (std::size_t i = 0; i < opt.size(); ++i) if (u[i] > u[best]) best = i;
            if (c.hasLastKey) for (std::size_t i = 0; i < opt.size(); ++i) if (opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == c.lastKey && u[i] > -1e8 && u[i] >= u[best] - 0.3) { best = i; break; }
            ++decisions;
            // the disagreement with IAUS's own pick
            std::size_t ii = opt.size() - 1; if (iausPick) for (std::size_t i = 0; i < opt.size(); ++i) if (opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == KeyOf(*iausPick)) { ii = i; break; }
            if (log) {
                const double dU = u[ii] > -1e8 ? u[best] - u[ii] : 0.0;
                if (ii != best && dU >= cfg.minDU) {
                    ++disagreed; dUSum += dU;
                    const Candidate* a = opt[ii].k; const Candidate* b = opt[best].k;
                    Disagreement& g = (*log)[opt[ii].Label() + " -> " + opt[best].Label()]; ++g.n; g.dU += dU;
                    auto fill = [](const Candidate* k, double& hp, double& onP, double& cas, double& res, double& rec, double& dist) { if (!k) return; hp += k->f.foeHpPct; onP += k->f.foeOnPlayer; cas += k->f.foeCaster; res += k->f.resist; rec += k->f.recipientHp; dist += k->f.dist; };
                    fill(a, g.iFoeHp, g.iOnPlayer, g.iCaster, g.iResist, g.iRecHp, g.iDist); fill(b, g.oFoeHp, g.oOnPlayer, g.oCaster, g.oResist, g.oRecHp, g.oDist);
                    float nd = 1e9f; for (const auto& f : sv.sn.foes) nd = std::min(nd, f.dist);
                    g.selfHp += x.c.hp / std::max(1.0f, x.c.hpMax); g.selfMp += x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 1; g.foes += double(sv.sn.foes.size()); g.sameTarget += a && b && a->target == b->target;
                    g.playerHp += F.b[0].c.hp / std::max(1.0f, F.b[0].c.hpMax); g.nearDist += nd < 1e8f ? nd : 0;
                }
            }
            // act
            const Opt& o = opt[best]; const Candidate* k = o.k; Tally& t = F.t;
            const Key key = k ? KeyOf(*k) : Key{ Family::kCount, PostureKind::None, static_cast<std::uint32_t>(o.ext) * 1000u + static_cast<std::uint32_t>(o.idx + 1), o.target };
            if (!Apply(F, ci, o, 0.0)) return;
            c.commitUntil = 0;
            if (F.trace && (!c.hasLastKey || !(c.lastKey == key))) std::fprintf(F.trace, "%7.2f  ORACLE %s: %s%s%s -> %u  U %.2f (IAUS would: %s, U %.2f)\n", F.now, x.c.name.c_str(), o.Label().c_str(), k && !k->formName.empty() ? " " : "", k ? std::string(k->formName).c_str() : "", k ? k->target : o.target, u[best], opt[ii].Label().c_str(), u[ii]);
            if (subj) {
                if (o.ext != Ext::None) ++t.extActions;
                if (k) { ++t.fired; if (k->family != Family::Posture) { ++t.hist[static_cast<std::size_t>(k->family)];
                        t.actSec[static_cast<std::size_t>(k->family)] += k->family == Family::Attack ? cfg.every : k->family == Family::Retreat ? TM::retreatSec : std::max(0.0, x.actEnd - F.now); } else ++t.postureSwaps;
                    if ((k->family == Family::DrinkHealth || k->family == Family::CastHeal) && c.lowSince >= 0 && !c.lowHealed && c.lowHadCand) { c.lowHealed = true; ++t.lowHealed; t.lowHealSecSum += F.now - c.lowSince; } }
                if (!c.hasLastKey || !(c.lastKey == key)) ++t.switches;
            }
            c.lastKey = key; c.hasLastKey = true;
        }
    };

}
