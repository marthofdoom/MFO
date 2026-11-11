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
#include <cstdlib>
#include "iaus_sim/World.h"

#include <chrono>
#include <span>

namespace Sim {

    struct Served {            // the snapshot must outlive the candidates (string_views into it)
        Snapshot  sn;
        Generated g;
        Decision  d;
    };

    // The estimate inputs of one actor, from what the GAME can read of him: his drawn weapon (or the spell in his hand) through the
    // engine's own damage and rate model (I8 research 2.1 / 2.4), his armor, his magic resistance, the damage he has been taking.
    inline void FillEstimate(const Fight& F, const Body& o, Combatant& k) {
        const Rules& r = F.D->rules; const ActorSpec& sp = *o.spec;
        k.hit = 0; k.hitsPerSec = 0; k.hitIsMagic = false;
        const Ctl* c = o.ctl >= 0 ? &F.ctl[static_cast<std::size_t>(o.ctl)] : nullptr;
        if (o.casterMode) {
            float best = 0, cast = 1.0f; const std::size_t n = c ? c->build->spells.size() : sp.spells.size();
            for (std::size_t i = 0; i < n; ++i) { const SimSpell* s = F.SpellOf(o, static_cast<int>(i)); if (!s || s->e.nature != Prune::Nature::Offense) continue;
                const float cost = c ? c->book.entries[i].cost : s->e.cost; if (cost > o.c.mp) continue; const float dm = s->dmg + s->dot * s->dotDur; if (dm > best) { best = dm; cast = s->castSec; } }
            if (best > 0) { k.hit = best * o.dmgScale; k.hitsPerSec = 1.0f / (cast + TM::castRecover + 0.25f); k.hitIsMagic = true; }
        }
        if (!(k.hit > 0)) {
            const bool ranged = o.rangedMode && sp.ranged.Has(); const Weapon& w = ranged ? sp.ranged : sp.melee;
            k.hit = WeaponDamage(r, sp, w, false) * o.dmgScale;
            k.hitsPerSec = (w.Has() ? w.speed : 1.0f) * static_cast<float>(ranged ? F.D->G("fCombatDPSBowSpeedMult", 0.2) : F.D->G("fCombatDPSMeleeSpeedMult", 1.0));
        }
        k.armor = ArmorMitigation(r, F.ArmorOf(o), sp.pieces); k.magicResist = F.ResistOf(o, kAvResistMagic); k.lossPerSec = o.recentDmg / 2.0f;
    }

    inline void MakeSnapshot(const Fight& F, int ci, Snapshot& sn) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& self = F.b[static_cast<std::size_t>(c.body)];
        sn.self = self.c; sn.self.dist = 0; sn.self.targetId = c.steer ? c.steer : self.c.targetId;
        if (F.estimates) FillEstimate(F, self, sn.self);
        std::vector<int> elems;
        for (const auto& e : c.book.entries) if (e.resistAv >= 0 && std::find(elems.begin(), elems.end(), e.resistAv) == elems.end()) elems.push_back(e.resistAv);
        const Body& pl = F.b[0];
        for (const Body& o : F.b) {
            if (&o == &self || !o.alive) continue;
            if (o.role == Role::Foe) {
                if (o.c.downed) continue;
                Combatant k = o.c; k.dist = Fight::Dist(self, o); if (k.dist > sn.chaseCap) continue; k.rangedWeapon = o.rangedMode; k.swinging = (o.act == Act::Swing || o.act == Act::Power) && o.hitAt >= 0; k.casting = o.act == Act::Cast && o.hitAt >= 0;
                if (!F.tauntVisible) k.taunted = false;
                const Body* tg = F.Find(o.c.targetId);
                k.targetIsAlly = tg && tg->Party() && tg->c.id != self.c.id && tg->role != Role::Player;
                k.paralysed = F.now < o.ctrlUntil; k.fleeing = F.now < o.fleeUntil; k.blocking = F.now < o.blockUntil;
                k.sight = F.Sees(self, o) ? Sight::Visible : Sight::Occluded; k.sightEngine = k.sight;
                // One ray model in the sim (sightEngine = sight), so one age serves both bases. The 1 ms floor is the
                // native NoteOcclusion's: 0 means untracked, which takes no hold.
                if (k.sight == Sight::Occluded) k.occludedSec = (std::max)(1.0e-3f, static_cast<float>(F.now - c.occSince.try_emplace(k.id, F.now).first->second));
                else c.occSince.erase(k.id);
                k.occludedSecEngine = k.occludedSec;
                k.resists.clear(); for (int av : elems) k.resists.push_back({ av, F.ResistOf(o, av) });
                if (F.estimates) FillEstimate(F, o, k);
                sn.foes.push_back(std::move(k));
            } else if (o.role == Role::Player || o.role == Role::Follower) {
                Combatant k = o.c; k.dist = Fight::Dist(self, o); k.sight = F.Sees(self, o) ? Sight::Visible : Sight::Occluded; k.sightEngine = k.sight;
                if (F.estimates) FillEstimate(F, o, k);
                if (F.estimates) for (const Body& h : F.b) if (&h != &self && h.role == Role::Follower && h.act == Act::Cast && h.hitAt >= 0 && h.actTarget == o.c.id) { const SimSpell* hs = F.SpellOf(h, h.actSpell); if (hs && hs->e.nature == Prune::Nature::Heal) k.beingHealed = true; }
                if (o.ctl >= 0) { const Ctl& oc = F.ctl[static_cast<std::size_t>(o.ctl)]; if (oc.gate.spellsOnClass) for (const SimSpell& hs : oc.build->spells) k.healer |= hs.healOther; }
                sn.allies.push_back(std::move(k));
            }
        }
        sn.playerId = pl.c.id; sn.currentTargetId = sn.self.targetId; sn.playerTargetId = pl.alive ? pl.c.targetId : 0;
        sn.hasBook = true; sn.book = c.book;
        for (std::size_t i = 0; i < sn.book.entries.size(); ++i) {
            SpellEntry& e = sn.book.entries[i]; e.activeOn.clear();
            for (const auto& pr : c.active[i]) if (pr.second > F.now) e.activeOn.push_back(pr.first);
            e.liveSummon = c.summonUntil[i] > F.now;
            if (F.estimates) { const SimSpell& s = c.build->spells[i]; e.damage = s.dmg + s.dot * s.dotDur; e.heal = s.heal + s.hot * s.hotDur; e.controlSec = s.ctrl ? s.ctrlDur : 0.0f; e.measured = true; }
        }
        sn.book.kit.arrows = self.spec->ranged.anim == 7 ? self.arrows : 0; sn.book.kit.bolts = self.spec->ranged.anim == 9 ? self.arrows : 0;
        sn.drawn = c.drawn; sn.meleeInRightHand = c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver;
        sn.meleeOnly = c.fam == Cls::Melee && sn.meleeInRightHand && !c.gate.spellsOnClass;
        if (g_learnReach && c.reach[0].Known()) sn.swingReach = (c.reach[1].hits + c.reach[1].misses >= 3 ? c.reach[1] : c.reach[0]).Gate();   // ruling Q68: the power attack's own outcomes once it has three
        sn.mortal = g_followerDeath && g_mortalKnown; sn.regroupRoad = g_regroupRoad; sn.groundRoad = g_groundRoad; sn.blockRoad = g_blockRoad; sn.bashRoad = g_bashRoad;
        sn.tauntEnabled = c.gate.taunt; sn.tauntReach = 600; sn.combatSec = static_cast<float>(F.now); sn.scrollRoad = F.scrollRoad;
        // card I6: no taunt candidate while his attempt is in flight or a foe is still pinned to him (Taunt::Busy)
        if (F.tauntVisible) { sn.tauntBusy = self.act == Act::Taunt && F.now < self.actEnd; for (const Body& o : F.b) if (o.role == Role::Foe && o.alive && o.tauntBy == self.c.id && F.now < o.tauntUntil) sn.tauntBusy = true; }
        // a heal of his own in flight: the hand it stands on is held, and the stream is live on its recipient
        if (self.act == Act::Cast && self.hitAt >= 0) { const SimSpell* s = F.SpellOf(self, self.actSpell);
            if (s && s->e.nature == Prune::Nature::Heal) { sn.handBusyLeft = sn.handBusyRight = true; sn.liveStreams.push_back({ s->e.form, self.actTarget == self.c.id ? 0u : self.actTarget, Tri::Yes }); } }
        for (const SpellTarget& w : (F.g10Sticky ? c.asked : c.wants)) {   // G10: only the pairs the last decision ranked on top have an answer (main); sticky = experiment
            const Body* d = F.Find(w.target); if (!d) continue;
            for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == w.spell) sn.willWork.push_back({ w.spell, w.target, F.WillWork(c.build->spells[i], *d) });
            if (std::find(c.wwMirror.begin(), c.wwMirror.end(), w) == c.wwMirror.end()) const_cast<Ctl&>(c).wwMirror.push_back(w);   // the ask is the prefetch
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

    // G3 in the lab: break off at a RUN (H3) to a point no foe sees, crouch there (G1), and G2 hides him. The hide has its own 12 s pacing.
    [[nodiscard]] inline bool CanHide(const Fight& F, int ci, float* gx = nullptr, float* gy = nullptr) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& x = F.b[static_cast<std::size_t>(c.body)];
        if (g_stealth < 2 || !x.stealthy || !c.gate.disengage || F.now - x.hideAt < 12.0 || F.now < x.cloakUntil || F.Unseen(x)) return false;
        float px = 0, py = 0; if (!F.HidePoint(x, px, py)) return false; if (gx) *gx = px; if (gy) *gy = py; return true;
    }
    inline bool DoHide(Fight& F, int ci) {
        Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; float gx = 0, gy = 0;
        if (x.hitAt >= 0 || F.now < x.staggerUntil || F.now < x.ctrlUntil || !CanHide(F, ci, &gx, &gy)) return false;
        const double sec = std::min(5.0, static_cast<double>(std::hypot(gx - x.c.x, gy - x.c.y) / std::max(100.0f, x.spec->run)) + 0.4);
        c.goalX = gx; c.goalY = gy; c.moveUntil = F.now + sec; c.sprint = false; c.moveFight = false; x.runUntil = c.moveUntil; x.sneaking = false; x.hideAt = F.now; c.steer = 0;
        if (ci == 0) ++F.t.hides; return true;
    }
    // THE DETECTION INPUT IN THE LAB (g_stealth 3). native/iaus is not edited in this phase, so the two things the input would feed
    // ride considerations that exist: (1) a blow on a foe that does not detect him is a sneak attack, and is boosted the way a blow on
    // a helpless foe is (BoostControlled, Facts::foeHelpless); (2) "go and hide" is a Family::Sneak candidate whose worth, (m - 1) /
    // (m + 1) of his sneak multiplier m, rides the FightStart curve (Inv(1): combatSec = 6 x (1 / worth - 1)). Both are STAND-INS for
    // the considerations of iaus-classes.md "The Sneak family"; the numbers they give are the worth of the input, not of the wiring.
    inline void StealthInject(const Fight& F, int ci, Served& sv) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& x = F.b[static_cast<std::size_t>(c.body)];
        if (!x.stealthy) return;
        for (Candidate& k : sv.g.actions) { if (k.family != Family::Attack && k.family != Family::PowerAttack) continue;   // a blow that is multiplied: a sneak attack, or the flanking half bonus (G4)
            const Body* tg = F.Find(k.target); if (!tg || tg->role != Role::Foe) continue;
            if (x.sneaking && !F.Detects(*tg, x)) k.f.foeHelpless = true;
            else if (c.flank) { const Body* tt = F.Find(tg->c.targetId); if (tt && tt != &x && tt->Party()) k.f.foeHelpless = true; } }
        if (CanHide(F, ci)) { const Body& pl = F.b[0]; bool playerDying = pl.alive && pl.c.hp < 0.35f * pl.c.hpMax;
            const Weapon& w = (c.drawn == PostureKind::Bow && x.spec->ranged.Has()) ? x.spec->ranged : x.spec->melee;
            const float m = SneakAttackMult(*F.D, *x.spec, w) * c.sneakAtkClass, worth = std::clamp((m - 1.0f) / (m + 1.0f), 0.05f, 1.0f) * (playerDying ? 0.3f : 1.0f);
            Candidate k; k.family = Family::Sneak; k.formName = "hide"; k.f.unseen = Tri::Yes; k.f.combatSec = 6.0f * (1.0f / worth - 1.0f); k.f.selfHp = x.c.hp / std::max(1.0f, x.c.hpMax); k.f.onClass = true;
            sv.g.actions.push_back(k); }
    }

    // What firing a candidate does to the world. Declined = transparent (the walk goes on); Invalid = IAUS offered
    // something the world cannot do at all (a defect, counted).
    inline Fired Fire(Fight& F, int ci, const Candidate& k) {
        Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; Tally& t = F.t; const bool subj = ci == 0;
        const Family fam = k.family; Body* tg = k.target ? F.Find(k.target) : nullptr;
        const bool hostile = fam == Family::Attack || fam == Family::PowerAttack || fam == Family::Taunt || fam == Family::CastOffense || fam == Family::CastControl || (fam == Family::Scroll && k.scrollOf == Family::CastOffense);
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
        case Family::Sneak: return DoHide(F, ci) ? Fired::Yes : Fired::Declined;
        case Family::GiveGround: { if (!tg || !Fight::Up(*tg)) return Fired::Invalid; if (bodyHeld) return Fired::Declined;
            const float dx = x.c.x - tg->c.x, dy = x.c.y - tg->c.y, l = std::max(1.0f, std::hypot(dx, dy));
            c.goalX = x.c.x + dx / l * 500.0f; c.goalY = x.c.y + dy / l * 500.0f; c.moveUntil = F.now + 2.0; c.sprint = false; c.moveFight = false; if (subj) ++t.grounds; return Fired::Yes; }
        case Family::Block: { if (bodyHeld) return Fired::Declined; x.act = Act::Block; x.blockUntil = F.now + 1.5; x.actEnd = x.blockUntil; x.hitAt = -1; x.actIaus = true; x.c.blocking = true; if (subj) ++t.blocks; return Fired::Yes; }
        case Family::Bash: { if (!tg || !Fight::Up(*tg) || tg->Party()) return Fired::Invalid; if (bodyHeld || x.c.sp < 20.0f || Fight::Dist(x, *tg) > F.Reach(x) * 1.1f) return Fired::Declined;
            F.StartSwing(x, *tg, Act::Bash, true); if (subj) ++t.bashes; return Fired::Yes; }
        case Family::Regroup: { if (!tg || !Fight::Up(*tg) || !tg->Party()) return Fired::Invalid; if (bodyHeld) return Fired::Declined;
            c.goalX = tg->c.x - 110.0f; c.goalY = tg->c.y + (x.c.y > tg->c.y ? 100.0f : -100.0f); c.moveUntil = F.now + 4.0; c.sprint = false; c.moveFight = true; if (subj) ++t.regroups; return Fired::Yes; }
        case Family::DrinkHealth: case Family::DrinkStamina: case Family::DrinkMagicka: {
            Kit& kit = c.book.kit; std::int32_t& n = fam == Family::DrinkHealth ? kit.potionsHealth : fam == Family::DrinkStamina ? kit.potionsStamina : kit.potionsMagicka;
            if (n <= 0) return Fired::Invalid;
            if (bodyHeld) return Fired::Declined;
            --n; take(Act::Drink, TM::drink, TM::drinkAt); x.actSpell = fam == Family::DrinkHealth ? 0 : fam == Family::DrinkMagicka ? 1 : 2; return Fired::Yes; }
        case Family::Scroll:
        case Family::CastOffense: case Family::CastControl: case Family::CastHeal: case Family::CastBuff: case Family::CastSummon: case Family::CastWard: {
            int si = -1; for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == k.form) si = static_cast<int>(i);
            if (si < 0) return Fired::Invalid;
            const SpellEntry& e = c.book.entries[static_cast<std::size_t>(si)]; const SimSpell& s = c.build->spells[static_cast<std::size_t>(si)];
            if (fam == Family::Scroll) { if (e.source != SpellSource::Scroll || e.count <= 0 || !c.gate.scrollUser) return Fired::Invalid; }
            else if (e.cost > x.c.mp + 0.5f || !(c.gate.spellsOnClass || c.gate.spellsFromMastered)) return Fired::Invalid;
            if (bodyHeld) return Fired::Declined;
            const std::uint32_t target = hostile ? tg->c.id : (k.target ? k.target : x.c.id);
            if (hostile) {   // cast/Fire.cpp:430: a spell that can never affect this target is passed over, transparently, once the mirror has a verdict
                const SpellTarget pr{ k.form, tg->c.id }; const bool known = std::find(c.wwMirror.begin(), c.wwMirror.end(), pr) != c.wwMirror.end();
                if (!known) c.wwMirror.push_back(pr);
                else if (F.WillWork(s, *tg) == Tri::No) { if (subj) ++t.wwDeclined; return Fired::Declined; }
            }
            if (subj) {
                if ((fam == Family::CastOffense || (fam == Family::Scroll && hostile)) && e.resistAv >= 0) { ++t.elemCasts; const float r = F.ResistOf(*tg, e.resistAv); if (r >= 50.0f) ++t.resistedCasts; if (r < 0.0f) ++t.weakCasts; if (r >= 100.0f) ++t.immuneCasts; }
                if (fam == Family::CastHeal) { const Body* r = F.Find(target); if (r && r != &x) { t.healAtSum += r->c.hp / std::max(1.0f, r->c.hpMax); ++t.healAtN; }
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

    // The tuning every IausPolicy starts from (Tune.h points it at a searched copy; the default is the game's own).
    inline const Tuning*& ActiveTuning() { static const Tuning* t = &DefaultTuning(); return t; }
    inline void (*&ProfileHook())(Profile&) { static void (*h)(Profile&) = nullptr; return h; }   // weight overrides for every follower (Tune.h)

    struct IausPolicy : Policy {
        const Tuning* T = ActiveTuning();
        bool timeIt = false;
        // Decide for the served follower and act on it. `forced` (oracle): fire this candidate instead of the decision's.
        void Serve(Fight& F, int ci) override { Served sv; ServeWith(F, ci, sv); }
        void ServeWith(Fight& F, int ci, Served& sv) {
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; Tally& t = F.t; const bool subj = ci == 0;
            if (!Fight::Up(x)) return;
            MakeSnapshot(F, ci, sv.sn);
            const auto t0 = timeIt ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            sv.g = Generate(sv.sn, c.gate, *T);
            if (g_stealth >= 3) StealthInject(F, ci, sv);
            FillCooldowns(c, sv.g.actions, F.now, *T); FillCooldowns(c, sv.g.postures, F.now, *T);
            TickIn in; in.now = F.now; in.rapportRank = c.rank; in.seed = c.seed; in.commitWindowSec = c.window;
            sv.d = Decide(c.mind, sv.g.actions, c.profile, *T, in);
            if (F.trace && std::getenv("SIM_WHY")) {   // debug: SIM_WHY=1 with --trace prints why the top candidates and every heal scored as they did
                std::fprintf(F.trace, "%7.2f  WHY %s: %zu candidates, %zu ranked, verdict %d, best heal %.3f\n", F.now, x.c.name.c_str(), sv.g.actions.size(), sv.d.ranked.size(), static_cast<int>(sv.d.verdict), sv.d.bestHeal);
                for (std::size_t i = 0; i < sv.d.ranked.size() && i < 3; ++i) std::fprintf(F.trace, "           #%zu %s\n", i + 1, FormatWhy(sv.g.actions[sv.d.ranked[i].idx], c.mind, c.profile, *T, c.rank, sv.d.bestHeal).c_str());
                if (F.now < 0.3) for (const SpellEntry& e : sv.sn.book.entries) std::fprintf(F.trace, "           book %-40s nature %d drop %d cost %.0f reach %.0f self %d conc %d heal %.0f dmg %.0f src %d n %d\n", e.name.c_str(), static_cast<int>(e.nature), static_cast<int>(e.drop), e.cost, e.reach, e.selfDelivery, e.concentration, e.heal, e.damage, static_cast<int>(e.source), e.count);
                for (const Candidate& k : sv.g.actions) if (k.family == Family::CastHeal || k.family == Family::Taunt || k.family == Family::PowerAttack || k.family == Family::Retreat) std::fprintf(F.trace, "           also %s\n", FormatWhy(k, c.mind, c.profile, *T, c.rank, sv.d.bestHeal).c_str());
            }
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
                        t.actSec[static_cast<std::size_t>(k.family)] += k.family == Family::Attack ? TM::pump * static_cast<double>(F.ctl.size()) : k.family == Family::Retreat ? TM::retreatSec : k.family == Family::Sneak ? std::max(0.0, c.moveUntil - F.now) : std::max(0.0, x.actEnd - F.now);
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
