#pragma once
// iaus_sim/Policy.h -- the followers' decision layer over a Fight (sim v2). HOST-ONLY.
//
// MakeSnapshot mirrors what native/iaus/Candidates.cpp Gather fills (the fields the game cannot fill yet stay
// hidden: Combatant::taunted unless --taunt-visible). Fire mirrors what an action DOES in the game:
//   attack        -> names the target; the engine AI does the swinging (Vocabulary kActAttack)
//   power attack  -> a forced power attack once in reach
//   taunt         -> one animated attempt (Taunt.h), a chance to pin the foe on him
//   retreat       -> he backs off
//   drink / cast / posture -> the body is busy for the animation; the effect lands at its frame
// A Policy decides for ONE follower when the pump serves him. IausPolicy is the game's own Generate + Decide.
#include "iaus_sim/World.h"

#include <chrono>
#include <span>

namespace Sim {

    struct Served {            // the snapshot must outlive the candidates (string_views into it)
        Snapshot  sn;
        Generated g;
        Decision  d;
    };

    inline void MakeSnapshot(const Fight& F, int ci, Snapshot& sn) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& self = F.b[static_cast<std::size_t>(c.body)];
        sn.self = self.c; sn.self.dist = 0; sn.self.targetId = c.steer ? c.steer : self.c.targetId;
        std::vector<int> elems;
        for (const auto& e : c.book.entries) if (e.resistAv >= 0 && std::find(elems.begin(), elems.end(), e.resistAv) == elems.end()) elems.push_back(e.resistAv);
        const Body& pl = F.b[0];
        for (const Body& o : F.b) {
            if (&o == &self || !o.alive) continue;
            if (o.role == Role::Foe) {
                if (o.c.downed) continue;
                Combatant k = o.c; k.dist = Fight::Dist(self, o); if (k.dist > sn.chaseCap) continue;
                if (!F.tauntVisible) k.taunted = false;
                const Body* tg = F.Find(o.c.targetId);
                k.targetIsAlly = tg && tg->Party() && tg->c.id != self.c.id && tg->role != Role::Player;
                k.paralysed = F.now < o.ctrlUntil; k.fleeing = F.now < o.fleeUntil; k.blocking = F.now < o.blockUntil;
                k.sight = F.Sees(self, o) ? Sight::Visible : Sight::Occluded; k.sightEngine = k.sight;
                k.resists.clear(); for (int av : elems) k.resists.push_back({ av, F.ResistOf(o, av) });
                sn.foes.push_back(std::move(k));
            } else if (o.role == Role::Player || o.role == Role::Follower) {
                Combatant k = o.c; k.dist = Fight::Dist(self, o); k.sight = F.Sees(self, o) ? Sight::Visible : Sight::Occluded; k.sightEngine = k.sight;
                sn.allies.push_back(std::move(k));
            }
        }
        sn.playerId = pl.c.id; sn.currentTargetId = sn.self.targetId; sn.playerTargetId = pl.alive ? pl.c.targetId : 0;
        sn.hasBook = true; sn.book = c.book;
        for (std::size_t i = 0; i < sn.book.entries.size(); ++i) {
            SpellEntry& e = sn.book.entries[i]; e.activeOn.clear();
            for (const auto& pr : c.active[i]) if (pr.second > F.now) e.activeOn.push_back(pr.first);
            e.liveSummon = c.summonUntil[i] > F.now;
        }
        sn.book.kit.arrows = self.spec->ranged.anim == 7 ? self.arrows : 0; sn.book.kit.bolts = self.spec->ranged.anim == 9 ? self.arrows : 0;
        sn.drawn = c.drawn; sn.meleeInRightHand = c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver;
        sn.meleeOnly = c.fam == Cls::Melee && sn.meleeInRightHand && !c.gate.spellsOnClass;
        sn.tauntEnabled = c.gate.taunt; sn.tauntReach = 600; sn.combatSec = static_cast<float>(F.now);
        // a heal of his own in flight: the hand it stands on is held, and the stream is live on its recipient
        if (self.act == Act::Cast && self.hitAt >= 0) { const SimSpell* s = F.SpellOf(self, self.actSpell);
            if (s && s->e.nature == Prune::Nature::Heal) { sn.handBusyLeft = sn.handBusyRight = true; sn.liveStreams.push_back({ s->e.form, self.actTarget == self.c.id ? 0u : self.actTarget, Tri::Yes }); } }
        for (const SpellTarget& w : (F.g10Sticky ? c.asked : c.wants)) {   // G10: only the pairs the last decision ranked on top have an answer (main); sticky = experiment
            const Body* d = F.Find(w.target); if (!d) continue;
            for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == w.spell) sn.willWork.push_back({ w.spell, w.target, F.WillWork(c.build->spells[i], *d) });
        }
    }

    // Engine::FillCooldowns over the sim's own store (Cooldowns::Remaining(T) = T - since while recent, else 0).
    inline void FillCooldowns(const Ctl& c, std::span<Candidate> cands, double now, const Tuning& T) {
        for (auto& k : cands) {
            const auto it = c.firedAt.find(CooldownId(KeyOf(k)));
            const float since = it == c.firedAt.end() ? kInf : static_cast<float>(now - it->second);
            k.f.hardCooldownLeft = k.f.hardCooldownSec > 0.0f && since < k.f.hardCooldownSec ? k.f.hardCooldownSec - since : 0.0f;
            k.f.sinceFired = since < T.softCooldownSec ? since : kInf;
        }
    }

    enum class Fired : std::uint8_t { Yes, Declined, Invalid };

    // What firing a candidate does to the world. Declined = transparent (the walk goes on); Invalid = IAUS offered
    // something the world cannot do at all (a defect, counted).
    inline Fired Fire(Fight& F, int ci, const Candidate& k) {
        Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; Tally& t = F.t; const bool subj = ci == 0;
        const Family fam = k.family; Body* tg = k.target ? F.Find(k.target) : nullptr;
        const bool hostile = fam == Family::Attack || fam == Family::PowerAttack || fam == Family::Taunt || fam == Family::CastOffense || fam == Family::CastControl;
        if (hostile && (!tg || !Fight::Up(*tg) || tg->Party())) return Fired::Invalid;
        const bool bodyHeld = x.hitAt >= 0 || F.now < x.staggerUntil || F.now < x.ctrlUntil || (x.actIaus && F.now < x.actEnd);
        auto peel = [&]() { if (subj && tg && c.pressureSince >= 0 && tg->c.targetId == F.b[0].c.id) { t.peelSum += F.now - c.pressureSince; ++t.peelN; c.pressureSince = -2; } };
        auto take = [&](Act a, double dur, double hit) { x.act = a; x.actEnd = F.now + dur; x.hitAt = hit >= 0 ? F.now + hit : -1; x.actIaus = true; x.actTarget = k.target; x.blockUntil = 0; };
        switch (fam) {
        case Family::Attack:
            if (subj && c.steer != tg->c.id && c.lastTarget && c.lastTarget != tg->c.id) {}
            c.steer = tg->c.id; c.lastTarget = tg->c.id; peel(); return Fired::Yes;
        case Family::PowerAttack: {
            if (bodyHeld) return Fired::Declined;
            if (x.c.sp < PowerStamina(F.D->rules, x.spec->melee)) return Fired::Declined;
            c.steer = tg->c.id; peel();
            if (Fight::Dist(x, *tg) > F.Reach(x) * 0.95f || tg->c.outOfMeleeReach) return Fired::Declined;   // closes first (the steer stands)
            F.StartSwing(x, *tg, Act::Power, true); return Fired::Yes; }
        case Family::Taunt: {
            if (bodyHeld) return Fired::Declined;
            if (Fight::Dist(x, *tg) > 600.0f) return Fired::Declined;
            take(Act::Taunt, TM::taunt, TM::taunt * 0.7); peel();
            if (subj) { ++t.taunts; if (F.now < tg->tauntUntil) ++t.tauntRedundant;
                if (static_cast<float>(tg->c.level) > 1.3f * static_cast<float>(x.c.level)) ++t.tauntsOutmatched;
                const Body& pl = F.b[0]; if (tg->c.targetId == pl.c.id && pl.c.hp < 0.4f * pl.c.hpMax) ++t.tauntSaves; }
            return Fired::Yes; }
        case Family::Retreat: x.retreatUntil = F.now + TM::retreatSec; return Fired::Yes;
        case Family::DrinkHealth: case Family::DrinkStamina: case Family::DrinkMagicka: {
            Kit& kit = c.book.kit; std::int32_t& n = fam == Family::DrinkHealth ? kit.potionsHealth : fam == Family::DrinkStamina ? kit.potionsStamina : kit.potionsMagicka;
            if (n <= 0) return Fired::Invalid;
            if (bodyHeld) return Fired::Declined;
            --n; take(Act::Drink, TM::drink, TM::drinkAt); x.actSpell = fam == Family::DrinkHealth ? 0 : fam == Family::DrinkMagicka ? 1 : 2; return Fired::Yes; }
        case Family::CastOffense: case Family::CastControl: case Family::CastHeal: case Family::CastBuff: case Family::CastSummon: case Family::CastWard: {
            int si = -1; for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == k.form) si = static_cast<int>(i);
            if (si < 0) return Fired::Invalid;
            const SpellEntry& e = c.book.entries[static_cast<std::size_t>(si)]; const SimSpell& s = c.build->spells[static_cast<std::size_t>(si)];
            if (e.cost > x.c.mp + 0.5f || !(c.gate.spellsOnClass || c.gate.spellsFromMastered)) return Fired::Invalid;
            if (bodyHeld) return Fired::Declined;
            const std::uint32_t target = hostile ? tg->c.id : (k.target ? k.target : x.c.id);
            if (subj) {
                if (fam == Family::CastOffense && e.resistAv >= 0) { ++t.elemCasts; const float r = F.ResistOf(*tg, e.resistAv); if (r >= 50.0f) ++t.resistedCasts; if (r < 0.0f) ++t.weakCasts; if (r >= 100.0f) ++t.immuneCasts; }
                if (fam == Family::CastHeal) { const Body* r = F.Find(target);
                    for (const Body& o : F.b) if (&o != &x && o.role == Role::Follower && o.act == Act::Cast && o.hitAt >= 0 && o.actTarget == target) { const SimSpell* os = F.SpellOf(o, o.actSpell); if (os && os->e.nature == Prune::Nature::Heal) { ++t.doubleHeals; break; } }
                    (void)r; }
            }
            F.StartCast(x, si, s, target, e.source == SpellSource::Staff ? 0.0f : e.cost, true); if (hostile) { c.lastTarget = tg->c.id; peel(); }
            return Fired::Yes; }
        default: return Fired::Invalid;
        }
    }

    struct Policy {
        virtual ~Policy() = default;
        virtual void Serve(Fight& F, int ci) = 0;
    };

    struct IausPolicy : Policy {
        const Tuning* T = &DefaultTuning();
        bool timeIt = false;
        // Decide for the served follower and act on it. `forced` (oracle): fire this candidate instead of the decision's.
        void Serve(Fight& F, int ci) override { Served sv; ServeWith(F, ci, sv); }
        void ServeWith(Fight& F, int ci, Served& sv) {
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; Tally& t = F.t; const bool subj = ci == 0;
            if (!Fight::Up(x)) return;
            MakeSnapshot(F, ci, sv.sn);
            const auto t0 = timeIt ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            sv.g = Generate(sv.sn, c.gate, *T);
            FillCooldowns(c, sv.g.actions, F.now, *T); FillCooldowns(c, sv.g.postures, F.now, *T);
            TickIn in; in.now = F.now; in.rapportRank = c.rank; in.seed = c.seed; in.commitWindowSec = c.window;
            sv.d = Decide(c.mind, sv.g.actions, c.profile, *T, in);
            if (timeIt && subj) { const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); t.dSum += us; t.dMax = std::max(t.dMax, us); ++t.dN; }
            const Decision& d = sv.d; const auto& acts = sv.g.actions;
            c.wants = TopSpellPairs(d, acts);
            if (F.g10Sticky) for (const SpellTarget& w : c.wants) if (std::find(c.asked.begin(), c.asked.end(), w) == c.asked.end()) c.asked.push_back(w);
            if (subj) Account(F, c, x, sv);
            // posture layer
            if (!sv.g.postures.empty() && x.hitAt < 0 && !(x.actIaus && F.now < x.actEnd) && F.now >= x.staggerUntil) {
                const int pi = DecidePosture(c.mind, sv.g.postures, c.profile, *T, c.rank);
                if (pi >= 0 && static_cast<std::size_t>(pi) < sv.g.postures.size()) { const Candidate& pc = sv.g.postures[static_cast<std::size_t>(pi)];
                    if (pc.f.posture != c.drawn && pc.f.posture != PostureKind::None) {
                        c.drawn = pc.f.posture; x.act = Act::Swap; x.actEnd = F.now + TM::swap; x.hitAt = -1; x.actIaus = true; OnPostureFired(c.mind, pc); c.firedAt[CooldownId(KeyOf(pc))] = F.now; if (subj) ++t.postureSwaps; } }
            }
            bool fired = false;
            if (d.verdict != Verdict::Idle) {
                const std::size_t tries = d.verdict == Verdict::Hold ? std::min<std::size_t>(1, d.order.size()) : d.order.size();
                for (std::size_t s = 0; s < tries && !fired; ++s) {
                    const Step& st = d.order[s]; if (st.idx >= acts.size()) { if (subj) ++t.invalid; continue; }
                    const Candidate& k = acts[st.idx];
                    const Fired r = Fire(F, ci, k);
                    if (r == Fired::Invalid) { if (subj) ++t.invalid; continue; }
                    if (r == Fired::Declined) continue;
                    fired = true; OnFired(c.mind, k, st, F.now); c.firedAt[CooldownId(KeyOf(k))] = F.now;
                    if (F.trace && (k.family != Family::Attack || d.newDecision)) std::fprintf(F.trace, "%7.2f  IAUS %s: %s%s%s -> %u  score %.3f  (%s, try %zu)\n", F.now, x.c.name.c_str(), std::string(Name(k.family)).c_str(), k.formName.empty() ? "" : " ", std::string(k.formName).c_str(), k.target, s < d.ranked.size() ? d.ranked[0].score : 0.0f, d.verdict == Verdict::Hold ? "hold" : "act", s);
                    if (subj) { ++t.fired; ++t.hist[static_cast<std::size_t>(k.family)]; if (d.newDecision && s == 0) ++t.switches;
                        t.actSec[static_cast<std::size_t>(k.family)] += k.family == Family::Attack ? TM::pump * static_cast<double>(F.ctl.size()) : k.family == Family::Retreat ? TM::retreatSec : std::max(0.0, x.actEnd - F.now);
                        if ((k.family == Family::DrinkHealth || k.family == Family::CastHeal) && c.lowSince >= 0 && !c.lowHealed && c.lowHadCand) { c.lowHealed = true; ++t.lowHealed; t.lowHealSecSum += F.now - c.lowSince; } }
                }
            }
            if (d.verdict == Verdict::Idle) c.steer = 0;   // the follower's own AI picks
        }
        static void Account(Fight& F, Ctl& c, Body& x, const Served& sv) {
            Tally& t = F.t; const Decision& d = sv.d; const auto& acts = sv.g.actions;
            ++t.ticks; t.badFacts += static_cast<long>(d.badFacts);
            const bool low = x.c.hp < 0.35f * x.c.hpMax;
            if (low && c.lowSince < 0) { c.lowSince = F.now; c.lowHealed = false; c.lowHadCand = false; ++t.lowEpisodes; }
            if (!low && c.lowSince >= 0) c.lowSince = -1;
            const bool hasCand = !acts.empty();
            if (!hasCand) ++t.noCandTicks;
            if (d.verdict == Verdict::Idle) ++t.idle; else if (d.verdict == Verdict::Hold) ++t.hold; else ++t.act;
            if (hasCand) { ++t.candTicks; if (d.verdict == Verdict::Idle) ++t.idleCand;
                bool inReach = false; for (const auto& f : sv.sn.foes) if (f.dist <= (c.fam == Cls::Melee ? sv.sn.meleeReach : 1500.0f)) inReach = true;
                if (inReach) { ++t.reachTicks; if (d.verdict == Verdict::Idle) ++t.idleReach; } }
            if (c.lowSince >= 0) { bool healCand = false; for (const auto& k : acts) if (k.family == Family::CastHeal || k.family == Family::DrinkHealth) healCand = true;
                if (healCand) { if (!c.lowHadCand) { c.lowHadCand = true; ++t.lowCandEpisodes; } ++t.lowCandTicks; if (d.verdict == Verdict::Hold) ++t.lowHoldTicks; } }
            bool weak = false; for (const auto& k : acts) if (k.family == Family::CastOffense && k.f.resist < 0.0f) weak = true;
            if (weak) ++t.weakOpp;
        }
    };

    // Run a fight to its end: the world at 30 Hz, one follower served per pump tick, round-robin.
    inline void RunFight(Fight& F, Policy& P) {
        while (!F.over) {
            if (F.now >= F.nextPump && !F.ctl.empty()) { F.nextPump += TM::pump; P.Serve(F, F.rr); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size()); }
            F.StepWorld();
        }
    }

}
