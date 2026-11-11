#pragma once
// iaus_sim/World.h -- the fight as a plain, COPYABLE value (sim v2). HOST-ONLY.
//
// A Fight holds every actor, the followers' IAUS memory and the tallies. It is copied freely: the expert oracle
// (Oracle.h) clones it to roll candidate actions forward. Nothing in it points into itself.
//
// TIME: the world steps at 30 Hz. The followers' decision layer is served round-robin, one follower every 133 ms
// (the worker pump, Diagnostics kPumpMs), so each follower decides every 133 ms x followers (Candidates.cpp).
//
// WHO SWINGS: act.attack only names the target (Vocabulary.h kActAttack "target = the chosen foe"): the ENGINE's
// combat AI closes, swings, blocks and shoots. Brain() below models that AI for every actor (foes, the player
// model, and a follower whenever his decision layer is not holding his body), from the combat style record.
#include "iaus/Generate.h"
#include "iaus_sim/Load.h"

namespace Sim {

    // ── timing (seconds). MODEL values unless a source is named; every row is in the doc's model table ──
    struct TM {
        static constexpr float dt = 1.0f / 30.0f;
        static constexpr float pump = 0.133f;          // PROVEN (code): Diagnostics kPumpMs
        static constexpr float swing1H = 1.00f;        // one-handed swing, start to recovered, at weapon speed 1.0
        static constexpr float swing2H = 1.40f;        // two-handed (the engine plays it x fWeaponTwoHandedAnimationSpeedMult)
        static constexpr float swingCreature = 1.30f;
        static constexpr float hitFrac = 0.45f;        // the hit frame, as a fraction of the swing
        static constexpr float powerMult = 1.55f, powerHitFrac = 0.55f;
        static constexpr float bash = 0.9f, bashHitFrac = 0.35f;
        static constexpr float bowNock = 0.55f, bowHoldMin = 0.25f, bowHoldMax = 1.5f, bowRecover = 0.35f;   // + fBowDrawTime / speed (GMST); hold = fCombatRangedAttackHoldTime*
        static constexpr float crossbowReload = 1.8f;
        static constexpr float castRecover = 0.45f;    // after the release
        static constexpr float drink = 1.2f, drinkAt = 0.6f;
        static constexpr float swap = 1.3f;            // sheathe + draw the other category
        static constexpr float taunt = 1.2f;           // the taunt idle
        static constexpr float staggerSec = 1.1f, bashStagger = 0.8f, recoil = 0.6f;
        static constexpr float thinkMin = 0.05f, thinkMax = 0.45f;   // the engine AI pause between attacks, / offensive mult. CALIBRATED: field hit cadence p50 1.21 s (calib_logs.py)
        static constexpr float arrowSpeed = 3600.0f, spellSpeed = 2400.0f;
        static constexpr float downSec = 9.5f;         // bleedout before a follower stands again. CALIBRATED: field p50 9.5 s (n=50)
        static constexpr float tauntChance = 0.7f, tauntDur = 15.0f;   // duration: Config fTauntDuration default 15 s (PROVEN, code); the chance is MODEL
        static constexpr float retreatSec = 2.0f;
        static constexpr float aimMiss = 0.12f;        // an aimed projectile at a moving target
    };

    enum class Act : std::uint8_t { None = 0, Swing, Power, Bash, Shoot, Cast, Drink, Swap, Taunt, Block };
    enum class Role : std::uint8_t { Player = 0, Follower, Summon, Foe };

    struct Effect { int kind = 0; float v = 0; int av = -1; double until = 0; std::uint32_t src = 0; };   // 0 dot, 1 hot, 2 armor, 3 resist
    struct Shot { double at = 0; std::uint32_t src = 0, tgt = 0; float dmg = 0; int spell = -1; bool power = false, fromIaus = false; float x = 0, y = 0; float mag = 1; };

    struct Body {
        Combatant c;
        std::shared_ptr<const ActorSpec> spec;
        Role  role = Role::Foe;
        int   ctl = -1;                 // follower: index into Fight::ctl
        bool  alive = true;
        // activity
        Act   act = Act::None;
        double actEnd = 0, hitAt = -1;  // free again at actEnd; the pending impact / release at hitAt (< 0 none)
        std::uint32_t actTarget = 0; int actSpell = -1; bool actIaus = false; float actCost = 0;
        double think = 0, retargetAt = 0;
        double blockUntil = 0, staggerUntil = 0, ctrlUntil = 0, tauntUntil = 0, fleeUntil = 0, downUntil = 0, frenzyUntil = 0, retreatUntil = 0;
        std::uint32_t tauntBy = 0, lastAttacker = 0;
        double regenAt[3] = { 0, 0, 0 };
        std::vector<Effect> fx;
        std::vector<double> spellReady;  // per spec spell (foes) / per book entry (follower engine casts)
        float recentDmg = 0;             // decaying incoming damage per second (2 s window)
        float dmgDealt = 0;
        int   arrows = 60, potions = 0;
        bool  rangedMode = false, casterMode = false;
        double airUntil = 0, groundUntil = 0;   // dragons
        float armorBuff = 0;
        float dmgScale = 1;              // scenario strength scaling of a foe's damage (Setup.h targetRatio)
        std::string tag;                 // scenario role ("healer", "archer", "boss" ...)
        [[nodiscard]] bool Party() const { return role != Role::Foe; }
    };

    struct Occluder { float x, y, r; };
    struct Spawn { double at = 0; std::shared_ptr<const ActorSpec> spec; float x = 0, y = 0; std::string tag; float dmgScale = 1; };   // a foe that joins later (waves)

    // Per-fight counters. `subject` = follower 0 unless stated.
    struct Tally {
        long ticks = 0, idle = 0, hold = 0, act = 0, fired = 0, switches = 0, badFacts = 0, invalid = 0, postureSwaps = 0;
        long hist[kFamilies] = {};
        double actSec[kFamilies] = {};          // seconds his decisions spent per family (attack: one serve interval per fire)
        double bodySec[10] = {}, bodyIausSec[10] = {}, upSec = 0;   // what his BODY did, by Act (the behaviour signature); Iaus = started by a decision
        long lowEpisodes = 0, lowCandEpisodes = 0, lowHealed = 0, lowCandTicks = 0, lowHoldTicks = 0; double lowHealSecSum = 0;
        long noCandTicks = 0, candTicks = 0, idleCand = 0, reachTicks = 0, idleReach = 0, taunts = 0, tauntRedundant = 0, tauntFail = 0;
        double dSum = 0, dMax = 0; long dN = 0;
        // absolute outcomes
        bool   won = false, lost = false, playerDied = false; double dur = 0, firstKill = -1; long foesFaced = 0;
        long   downs = 0, partyDowns = 0;
        double dmgDone = 0, dmgTaken = 0, partyDmgTaken = 0, playerDmgTaken = 0, healDone = 0;
        double mpSum = 0, hpSum = 0, spSum = 0; long resN = 0; double mpDrySec = 0;
        // intelligence counters
        double overkill = 0, iausDmg = 0;                 // damage beyond the target's remaining health, of IAUS-chosen hits
        double overheal = 0, healTotal = 0; long healCasts = 0, doubleHeals = 0;
        long   foeCasts = 0, foeCastsInterrupted = 0, interruptsByFollowers = 0;
        long   elemCasts = 0, resistedCasts = 0, weakCasts = 0, weakOpp = 0, immuneCasts = 0;
        long   splashAllyHits = 0, areaCasts = 0;
        long   ctrlCasts = 0, ctrlFails = 0, ctrlOnControlled = 0, hitsOnControlled = 0, powOnControlled = 0, powerAttacks = 0;
        double playerPressureSec = 0, peelSum = 0; long peelN = 0, peelMiss = 0;
        long   tgtSwitches = 0; double engagedSec = 0;
        long   tauntsOutmatched = 0, tauntThenDown = 0, tauntSaves = 0;
        long   deathsWithHeal = 0;                        // a party member went down while the subject had an affordable heal for him
        long   killOrderGood = 0, killOrderBad = 0;       // priority foes (healer / caster / archer) killed before / after grunts
        double retreatSec = 0, kiteDmgTaken = 0;
        long   scrollsUsed = 0, extActions = 0; double moveOrderSec = 0;   // the extended action set (oracle / phase 2)
        long   wastedCasts = 0, hitsWhileCtl = 0, escaped = 0, wwDeclined = 0;   // wwDeclined: a walk try spent on a cast Fire refuses (will not work)
        double playerMinHp = 1, mpAtLate = -1;     // the player's lowest health fraction; the subject's magicka fraction when a late foe arrived                           // released at a dead / out-of-sight target
        std::vector<std::string> killOrder;
    };

    struct Ctl {   // one IAUS-driven follower
        int   body = -1;
        Cls   fam = Cls::Melee;
        ClassGate gate;
        Profile profile;
        std::shared_ptr<const FollowerBuild> build;
        Book  book;                        // live copy (costs, staff charge, activeOn, liveSummon)
        std::vector<std::vector<std::pair<std::uint32_t, double>>> active;   // per entry: (recipient, until)
        std::vector<double> summonUntil;
        Mind  mind;
        PostureKind drawn = PostureKind::None;
        std::map<std::uint64_t, double> firedAt;   // the Cooldowns store (Kind::IausAction): CooldownId -> last fired
        std::vector<SpellTarget> wants;            // G10: the pairs the last decision ranked on top
        std::vector<SpellTarget> asked;            // every pair ever asked (--g10-sticky: the answers are remembered)
        std::vector<SpellTarget> wwMirror;         // cast/WillWork's mirror: pairs with a measured verdict (the first ask or Fire is the prefetch)
        std::uint32_t steer = 0;                   // act.attack's target for the engine AI
        std::uint8_t rank = 3; std::uint64_t seed = 0; double window = 1.5;
        float potH = 100, potM = 100, potS = 100;
        double lowSince = -1; bool lowHealed = false, lowHadCand = false;
        double pressureSince = -1;                 // the player came under attack at (peel latency)
        double commitUntil = 0;                    // oracle rollouts: he keeps the option he was given until then
        float  goalX = 0, goalY = 0; double moveUntil = 0; bool sprint = false;   // a deliberate reposition order (the extended action set)
        int    potions0 = 0;                       // potions at the start (Utility: resources left)
        Key    lastKey{}; bool hasLastKey = false; // oracle arm: the action he is on (switch counting)
        std::uint32_t lastTarget = 0;
    };

    struct Policy;   // decides for one follower when he is served (Policy.h)

    struct Fight {
        const Data* D = nullptr;
        Rng R;
        std::vector<Body> b;
        std::vector<Ctl> ctl;
        std::vector<Occluder> occ;
        std::vector<Shot> shots;
        std::vector<Spawn> late;
        float foeHpTotal = 0;                      // every foe's full health, late ones included (Utility)
        double now = 0, nextPump = 0, maxSec = 150;
        int rr = 0;
        bool tauntVisible = true, engineCasts = true, g10Sticky = false, over = false;   // card I6: the game fills Combatant::taunted
        Tally t;
        std::uint32_t nextId = 1;
        std::FILE* trace = nullptr;   // --trace: one line per event (never set in a rollout copy)
        double lastTauntBy0 = -1e9;   // the subject's last landed taunt (taunt-then-down tally)

        // ── lookup ──
        [[nodiscard]] Body* Find(std::uint32_t id) { for (auto& x : b) if (x.c.id == id) return &x; return nullptr; }
        [[nodiscard]] const Body* Find(std::uint32_t id) const { for (auto& x : b) if (x.c.id == id) return &x; return nullptr; }
        [[nodiscard]] static bool Up(const Body& x) { return x.alive && !x.c.downed; }
        [[nodiscard]] Body& Player() { return b[0]; }
        [[nodiscard]] static float Dist(const Body& a, const Body& o) { return std::hypot(a.c.x - o.c.x, a.c.y - o.c.y); }
        [[nodiscard]] bool Sees(const Body& a, const Body& o) const {
            for (const Occluder& oc : occ) {   // segment / circle
                const float dx = o.c.x - a.c.x, dy = o.c.y - a.c.y, l2 = dx * dx + dy * dy; if (l2 < 1) continue;
                const float tt = std::clamp(((oc.x - a.c.x) * dx + (oc.y - a.c.y) * dy) / l2, 0.0f, 1.0f);
                if (std::hypot(a.c.x + tt * dx - oc.x, a.c.y + tt * dy - oc.y) < oc.r) return false;
            }
            return true;
        }
        [[nodiscard]] const Weapon& MeleeOf(const Body& x) const { return x.spec->melee; }
        [[nodiscard]] float Reach(const Body& x) const {   // hit reach in units: fCombatDistance x weapon reach, plus both bodies' radii (MODEL 70)
            const float r = x.spec->melee.Has() ? D->rules.combatDistance * x.spec->melee.reach : std::max(96.0f, x.spec->unarmedReach);
            return r + 70.0f + (x.spec->mass > 2.0f ? 60.0f : 0.0f);
        }
        [[nodiscard]] bool Busy(const Body& x) const { return now < x.actEnd || now < x.staggerUntil || now < x.ctrlUntil || now < x.downUntil; }
        [[nodiscard]] bool Controlled(const Body& x) const { return now < x.ctrlUntil || now < x.fleeUntil; }
        [[nodiscard]] float ArmorOf(const Body& x) const { return x.spec->ar + x.armorBuff; }
        [[nodiscard]] float ResistOf(const Body& x, int av) const {
            float r = x.spec->Resist(av);
            for (const Effect& e : x.fx) if (e.kind == 3 && e.av == av && e.until > now) r += e.v;
            return std::clamp(r, -100.0f, D->rules.maxResist + (x.spec->Resist(av) >= 100.0f ? 15.0f : 0.0f));   // a 100% innate resist is an immunity
        }

        // ── building ──
        Body& Add(Role role, std::shared_ptr<const ActorSpec> sp, float x, float y, float hpPct = 1.0f) {
            Body n; n.role = role; n.spec = std::move(sp); n.c.id = role == Role::Player ? 2u : role == Role::Foe ? 100u + nextId++ : 10u + nextId++;
            n.c.name = n.spec->name; n.c.level = n.spec->level; n.c.hpMax = n.spec->hp; n.c.mpMax = n.spec->mp; n.c.spMax = n.spec->sp;
            n.c.hp = n.c.hpMax * hpPct; n.c.mp = n.c.mpMax; n.c.sp = n.c.spMax; n.c.x = x; n.c.y = y; n.c.isPlayer = role == Role::Player;
            n.c.undead = n.spec->undead; n.c.unholy = n.spec->undead; n.spellReady.assign(n.spec->spells.size(), 0.0);
            n.think = R.U(0.0f, 0.8f); n.arrows = n.spec->ranged.Has() ? 60 : 0;
            b.push_back(std::move(n)); return b.back();
        }

        // ── damage and healing ──
        void Down(Body& x, const Body* by) {
            if (trace) std::fprintf(trace, "%7.2f  DOWN %s (%u) by %s\n", now, x.c.name.c_str(), x.c.id, by ? by->c.name.c_str() : "-");
            if (x.role == Role::Foe || x.role == Role::Summon) {
                x.alive = false; x.c.hp = 0;
                if (x.role == Role::Foe) { if (t.firstKill < 0) t.firstKill = now; t.killOrder.push_back(x.tag.empty() ? "grunt" : x.tag); }
            } else if (x.role == Role::Player) { x.alive = false; t.playerDied = true; }
            else {
                x.c.downed = true; x.downUntil = now + TM::downSec; x.c.hp = 0; x.act = Act::None; x.hitAt = -1; x.actEnd = now; ++t.partyDowns;
                if (x.ctl == 0) { ++t.downs; if (now - lastTauntBy0 < 8.0) ++t.tauntThenDown; }
            }
            (void)by;
        }
        // returns the health actually removed
        float Hurt(Body& x, float dmg, Body* by, bool fromIaus = false) {
            if (!x.alive || x.c.downed || dmg <= 0) return 0;
            const float before = x.c.hp; x.c.hp -= dmg; x.regenAt[0] = now + D->rules.delayHp; x.recentDmg += dmg;
            const float dealt = std::min(before, dmg);
            if (by) { by->dmgDealt += dealt; x.lastAttacker = by->c.id; if (by->ctl == 0) t.dmgDone += dealt; }
            if (fromIaus && by && by->ctl == 0) { t.iausDmg += dmg; t.overkill += dmg - dealt; }
            if (x.role == Role::Player) t.playerDmgTaken += dealt;
            if (x.Party()) t.partyDmgTaken += dealt;
            if (x.ctl == 0) { t.dmgTaken += dealt; if (now < x.retreatUntil) t.kiteDmgTaken += dealt; }
            if (x.c.hp <= 0) {
                if (x.Party() && x.role != Role::Summon && !ctl.empty()) {   // could the subject have healed him?
                    const Ctl& c0 = ctl[0]; const Body& s0 = b[static_cast<std::size_t>(c0.body)];
                    if (Up(s0)) for (std::size_t i = 0; i < c0.book.entries.size(); ++i) { const SpellEntry& e = c0.book.entries[i];
                        if (e.nature == Prune::Nature::Heal && c0.gate.spellsOnClass && e.cost <= s0.c.mp && (&x == &s0 || !e.selfDelivery)) { ++t.deathsWithHeal; break; } }
                }
                Down(x, by);
            }
            return dealt;
        }
        void Heal(Body& x, float amt, const Body* by, bool fromIaus) {
            if (!x.alive || amt <= 0) return;
            const float room = x.c.hpMax - x.c.hp, used = std::min(room, amt); x.c.hp += used;
            if (x.c.downed && x.c.hp > 0.1f * x.c.hpMax) { x.c.downed = false; x.downUntil = 0; }
            if (fromIaus && by && by->ctl == 0) { t.healTotal += amt; t.overheal += amt - used; t.healDone += used; }
        }
        void Stagger(Body& x, float sec, Body* by) {
            if (!Up(x) || x.spec->mass >= 6.0f) return;   // dragons and the like do not stagger (MODEL)
            if (x.act == Act::Cast && x.hitAt >= 0) { if (x.role == Role::Foe) { ++t.foeCastsInterrupted; if (by && by->role == Role::Follower) ++t.interruptsByFollowers; } }
            x.act = Act::None; x.hitAt = -1; x.staggerUntil = now + sec; x.actEnd = now + sec; x.blockUntil = 0;
        }
        // one physical hit that has connected in time and space
        float Physical(Body& a, Body& d, float dmg, bool power, bool ranged, bool fromIaus) {
            const Rules& r = D->rules; bool blocked = false;
            if (now < d.blockUntil && !ranged) {   // MODEL block: shield 0.45 + 0.30 x skill, weapon 0.30 + 0.20 x skill; a power attack halves it (fBlockPowerAttackMult)
                const float sk = d.spec->Skill(kAvBlock) / 100.0f; float bf = d.spec->shield ? 0.45f + 0.30f * sk : 0.30f + 0.20f * sk;
                if (power) bf *= r.blockPowMult;
                dmg *= 1.0f - std::min(0.85f, bf); blocked = true;
                if (!power && d.spec->shield && R.P(0.25f)) Stagger(a, TM::recoil, &d);
            }
            dmg *= a.dmgScale * (1.0f - ArmorMitigation(r, ArmorOf(d), d.spec->pieces));
            if (d.role == Role::Player) dmg *= r.diffToPC;
            if (a.role == Role::Player) dmg *= r.diffByPC;
            if (a.ctl == 0) { if (Controlled(d)) { ++t.hitsOnControlled; if (power) ++t.powOnControlled; }
                for (const Body& o : b) if (o.role == Role::Foe && Up(o) && now < o.ctrlUntil) { ++t.hitsWhileCtl; break; } }
            const float dealt = Hurt(d, dmg, &a, fromIaus);
            if (trace) std::fprintf(trace, "%7.2f  hit  %s -> %s (%u) %.0f%s%s%s  hp %.0f/%.0f\n", now, a.c.name.c_str(), d.c.name.c_str(), d.c.id, dealt, power ? " POWER" : "", blocked ? " blocked" : "", ranged ? " arrow" : "", d.c.hp, d.c.hpMax);
            if (power && !blocked && Up(d)) Stagger(d, TM::staggerSec * std::clamp(a.spec->mass / d.spec->mass, 0.6f, 1.4f), &a);
            return dealt;
        }
        // a spell's effect on one target (the magnitude already carries the caster's perks)
        void SpellHit(Body& a, const SimSpell& s, Body& d, bool fromIaus, float mag = 1.0f) {
            if (!Up(d)) return;
            const Rules& r = D->rules;
            const float el = std::clamp(ResistOf(d, s.e.resistAv) / 100.0f, -1.0f, 1.0f), mr = std::clamp(ResistOf(d, kAvResistMagic) / 100.0f, -1.0f, 0.85f);
            float k = mag * a.dmgScale * (1.0f - el) * (1.0f - mr);
            if (d.role == Role::Player) k *= r.diffToPC;
            if (a.role == Role::Player) k *= r.diffByPC;
            if (s.dmg > 0) Hurt(d, s.dmg * k, &a, fromIaus);
            if (trace) std::fprintf(trace, "%7.2f  spell %s: %s -> %s (%u) dmg %.0f (x%.2f)%s  hp %.0f/%.0f\n", now, s.e.name.c_str(), a.c.name.c_str(), d.c.name.c_str(), d.c.id, s.dmg * k, k, s.ctrl ? " control" : "", d.c.hp, d.c.hpMax);
            if (s.dot > 0 && Up(d)) d.fx.push_back({ 0, s.dot * k, -1, now + s.dotDur, a.c.id });
            if (s.mpDmg > 0) d.c.mp = std::max(0.0f, d.c.mp - s.mpDmg * k);
            if (s.spDmg > 0) d.c.sp = std::max(0.0f, d.c.sp - s.spDmg * k);
            if (s.stagger && Up(d)) Stagger(d, TM::bashStagger, &a);
            if (s.ctrl && Up(d)) {
                const bool works = CtrlWorks(s, d);
                if (a.ctl == 0) { ++t.ctrlCasts; if (!works) ++t.ctrlFails; if (Controlled(d)) ++t.ctrlOnControlled; }
                if (works) { const double until = now + std::clamp<double>(s.ctrlDur, 1.0, 60.0);
                    if (s.ctrlPar) { d.ctrlUntil = until; d.c.paralysed = true; d.act = Act::None; d.hitAt = -1; }
                    else if (s.ctrlCalm) { d.ctrlUntil = until; d.act = Act::None; d.hitAt = -1; }
                    else if (s.ctrlFrenzy) d.frenzyUntil = until;
                    else { d.fleeUntil = until; d.c.fleeing = true; } }
            }
        }
        [[nodiscard]] bool CtrlWorks(const SimSpell& s, const Body& d) const {
            if (s.ctrlPar) return !d.spec->immunePar;
            if (s.turnUndead && !d.spec->undead) return false;
            if (d.spec->dragon || (d.spec->daedra && !s.turnUndead && (s.ctrlCalm || s.ctrlFlee || s.ctrlFrenzy) && d.spec->creature && d.spec->immunePar)) return false;
            return static_cast<float>(d.c.level) <= s.ctrlMaxLevel;
        }
        [[nodiscard]] Tri WillWork(const SimSpell& s, const Body& d) const {   // what Actuation::WillWork would answer (G10)
            if (s.ctrl) return CtrlWorks(s, d) ? Tri::Yes : Tri::No;
            if (s.e.nature == Prune::Nature::Offense && s.e.resistAv >= 0 && d.spec->Resist(s.e.resistAv) >= 100.0f) return Tri::No;
            return Tri::Yes;
        }

        // ── the engine's combat AI (MODEL from the combat style) ──
        void Move(Body& x, float tx, float ty, float speed) {
            const float dx = tx - x.c.x, dy = ty - x.c.y, d = std::hypot(dx, dy); if (d < 1e-3f) return;
            const float k = std::min(speed * TM::dt, d) / d; x.c.x += dx * k; x.c.y += dy * k;
        }
        [[nodiscard]] Body* PickTarget(Body& x) {
            Body* best = nullptr; float bs = -1;
            const bool frenzy = now < x.frenzyUntil;
            for (auto& o : b) {
                if (&o == &x || !Up(o)) continue;
                if (!frenzy && o.Party() == x.Party()) continue;
                float s = 1000.0f / (Dist(x, o) + 300.0f);
                if (o.c.id == x.lastAttacker) s *= 1.8f;
                if (x.role == Role::Foe && o.role == Role::Player) s *= 1.25f;
                if (x.role == Role::Player) s *= 1.0f + (1.0f - o.c.hp / std::max(1.0f, o.c.hpMax));   // the player model finishes what it started
                if (o.c.outOfMeleeReach && !x.spec->ranged.Has() && x.spec->spells.empty()) s *= 0.1f;
                s *= R.U(0.85f, 1.15f);
                if (s > bs) { bs = s; best = &o; }
            }
            return best;
        }
        void StartSwing(Body& x, Body& tg, Act kind, bool fromIaus) {
            const Weapon& w = x.spec->melee; const bool power = kind == Act::Power;
            float base = !w.Has() ? TM::swingCreature : w.TwoH() ? TM::swing2H : TM::swing1H;
            float dur = base / std::max(0.3f, w.Has() ? w.speed : 1.0f), hf = TM::hitFrac;
            if (power) { dur *= TM::powerMult; hf = TM::powerHitFrac; x.c.sp = std::max(0.0f, x.c.sp - PowerStamina(D->rules, w)); x.regenAt[2] = now + D->rules.delaySp + dur; if (x.ctl == 0) ++t.powerAttacks; }
            if (kind == Act::Bash) { dur = TM::bash; hf = TM::bashHitFrac; x.c.sp = std::max(0.0f, x.c.sp - 20.0f); }
            x.act = kind; x.actTarget = tg.c.id; x.hitAt = now + dur * hf; x.actEnd = now + dur; x.actIaus = fromIaus;
            // the defender may read the swing and block it (MODEL: 0.20 x defensive mult, + 0.25 with a shield)
            if (kind != Act::Bash && Up(tg) && !Busy(tg) && tg.spec->hasStyle && (tg.spec->shield || tg.spec->melee.Has()) && !tg.rangedMode && !tg.casterMode && tg.c.targetId == x.c.id) {
                const float p = std::clamp(0.20f * tg.spec->st.def + (tg.spec->shield ? 0.25f : 0.0f), 0.0f, 0.75f);
                if (R.P(p)) { tg.act = Act::Block; tg.blockUntil = x.hitAt + 0.15; tg.actEnd = tg.blockUntil; tg.c.blocking = true; }
            }
        }
        // Releases the pending action of x at its hit frame.
        void Resolve(Body& x) {
            const Act kind = x.act; x.hitAt = -1;
            Body* tg = Find(x.actTarget);
            if (kind == Act::Swing || kind == Act::Power || kind == Act::Bash) {
                if (!tg || !Up(*tg) || Dist(x, *tg) > Reach(x) * 1.2f || (tg->c.outOfMeleeReach)) return;   // whiff
                if (kind == Act::Bash) { Hurt(*tg, 3.0f, &x); Stagger(*tg, TM::bashStagger, &x); return; }
                Physical(x, *tg, WeaponDamage(D->rules, *x.spec, x.spec->melee, kind == Act::Power), kind == Act::Power, false, x.actIaus);
            } else if (kind == Act::Shoot) {
                if (!tg) return;
                --x.arrows;
                shots.push_back({ now + Dist(x, *tg) / TM::arrowSpeed, x.c.id, tg->c.id, WeaponDamage(D->rules, *x.spec, x.spec->ranged, false), -1, false, x.actIaus, tg->c.x, tg->c.y, 1 });
            } else if (kind == Act::Cast) {
                ReleaseCast(x, tg);
            } else if (kind == Act::Drink && x.ctl >= 0) {
                const Ctl& c = ctl[static_cast<std::size_t>(x.ctl)];
                if (x.actSpell == 0) Heal(x, c.potH, nullptr, false); else if (x.actSpell == 1) x.c.mp = std::min(x.c.mpMax, x.c.mp + c.potM); else x.c.sp = std::min(x.c.spMax, x.c.sp + c.potS);
            } else if (kind == Act::Taunt) {
                if (!tg || !Up(*tg)) return;
                if (R.P(TM::tauntChance)) { tg->tauntUntil = now + TM::tauntDur; tg->tauntBy = x.c.id; tg->c.taunted = true; tg->c.targetId = x.c.id; x.think = now; lastTauntBy0 = x.ctl == 0 ? now : lastTauntBy0; }
                else if (x.ctl == 0) ++t.tauntFail;
            }
        }
        [[nodiscard]] const SimSpell* SpellOf(const Body& x, int idx) const {
            if (idx < 0) return nullptr;
            if (x.ctl >= 0) { const auto& sp = ctl[static_cast<std::size_t>(x.ctl)].build->spells; return static_cast<std::size_t>(idx) < sp.size() ? &sp[static_cast<std::size_t>(idx)] : nullptr; }
            return static_cast<std::size_t>(idx) < x.spec->spells.size() ? &D->foeSpells[static_cast<std::size_t>(x.spec->spells[static_cast<std::size_t>(idx)])] : nullptr;
        }
        void ReleaseCast(Body& x, Body* tg) {
            const SimSpell* s = SpellOf(x, x.actSpell); if (!s) return;
            // pay at the release (an interrupted charge costs nothing)
            if (x.ctl >= 0) { Ctl& c = ctl[static_cast<std::size_t>(x.ctl)]; SpellEntry& e = c.book.entries[static_cast<std::size_t>(x.actSpell)];
                if (e.source == SpellSource::Staff) { e.charge -= 1.0f; e.charged = e.charge > 0.5f; c.book.kit.staffCharged = e.charged; }
                else if (e.source == SpellSource::Scroll) { e.count = std::max(0, e.count - 1); if (x.ctl == 0) ++t.scrollsUsed; }
                else { x.c.mp = std::max(0.0f, x.c.mp - x.actCost); x.regenAt[1] = now + D->rules.delayMp; } }
            else { x.c.mp = std::max(0.0f, x.c.mp - x.actCost); x.regenAt[1] = now + D->rules.delayMp; }
            const bool iaus = x.actIaus; const Prune::Nature na = s->e.nature;
            if (na == Prune::Nature::Heal) {
                Body* r = (tg && !s->e.selfDelivery) ? tg : &x; if (!r->alive) r = &x;
                if (iaus && x.ctl == 0) { ++t.healCasts; }
                Heal(*r, s->heal, &x, iaus);
                if (s->hot > 0) r->fx.push_back({ 1, s->hot, -1, now + s->hotDur, x.c.id });
                return;
            }
            if (na == Prune::Nature::Buff || na == Prune::Nature::Ward) {
                Body* r = (tg && tg->Party() == x.Party()) ? tg : &x;
                if (s->armor > 0) r->fx.push_back({ 2, s->armor, -1, now + std::max(30.0f, s->buffDur), x.c.id });
                if (s->resistBuffAv >= 0) r->fx.push_back({ 3, s->resistBuff, s->resistBuffAv, now + std::max(30.0f, s->buffDur), x.c.id });
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].active[static_cast<std::size_t>(x.actSpell)].push_back({ r->c.id, now + std::max(30.0, static_cast<double>(s->buffDur)) });
                return;
            }
            if (na == Prune::Nature::Summon) {
                if (x.ctl >= 0) ctl[static_cast<std::size_t>(x.ctl)].summonUntil[static_cast<std::size_t>(x.actSpell)] = now + s->summonDur;
                auto sp = std::make_shared<ActorSpec>(); sp->name = "Summon"; sp->race = "AtronachFlameRace"; sp->level = x.c.level; sp->creature = sp->daedra = true;
                sp->hp = 60.0f + 12.0f * static_cast<float>(x.c.level); sp->unarmed = 8.0f + 0.8f * static_cast<float>(x.c.level); sp->hpR = 0; sp->mp = 0; sp->run = 400;   // MODEL: a level-scaled atronach
                const float ox = x.c.x + 150, oy = x.c.y + 60; const double until = now + s->summonDur; const bool party = x.Party();
                Body& n = Add(party ? Role::Summon : Role::Foe, sp, ox, oy); n.downUntil = 0; n.airUntil = -until;   // airUntil < 0 = expiry time of a summon
                return;
            }
            if (!tg) { if (x.ctl == 0 && iaus) ++t.wastedCasts; return; }
            if (x.ctl == 0 && iaus && (!Up(*tg) || !Sees(x, *tg))) ++t.wastedCasts;
            shots.push_back({ now + (s->e.concentration || s->e.touch ? 0.0 : Dist(x, *tg) / TM::spellSpeed), x.c.id, tg->c.id, 0, x.actSpell, false, iaus, tg->c.x, tg->c.y, 1 });
        }
        void Land(const Shot& sh) {
            Body* a = Find(sh.src); Body* d = Find(sh.tgt); if (!a) return;
            if (sh.spell < 0) {   // an arrow
                if (!d || !Up(*d)) return;
                const float moved = std::hypot(d->c.x - sh.x, d->c.y - sh.y);
                if (!Sees(*a, *d) || (moved > 40.0f && R.P(TM::aimMiss))) return;
                Physical(*a, *d, sh.dmg, false, true, sh.fromIaus); return;
            }
            const SimSpell* s = SpellOf(*a, sh.spell); if (!s) return;
            if (!d) return;
            if (!s->e.concentration && !s->e.touch && (!Sees(*a, *d))) return;   // the bolt hit the pillar
            const bool aimedMiss = !s->e.concentration && s->e.areaUnits <= 0 && Up(*d) && std::hypot(d->c.x - sh.x, d->c.y - sh.y) > 40.0f && R.P(TM::aimMiss);
            if (s->e.areaUnits > 0) {   // everyone inside the blast, friend or foe (marth: never splash the party)
                if (a->ctl == 0 && sh.fromIaus) ++t.areaCasts;
                const float cx = Up(*d) ? d->c.x : sh.x, cy = Up(*d) ? d->c.y : sh.y;
                for (auto& o : b) { if (!Up(o) || &o == a) continue; if (std::hypot(o.c.x - cx, o.c.y - cy) > s->e.areaUnits) continue;
                    if (o.Party() == a->Party()) { if (a->ctl == 0 && sh.fromIaus) ++t.splashAllyHits; SpellHit(*a, *s, o, false, 0.5f); }   // friendly fire at half (MODEL)
                    else SpellHit(*a, *s, o, sh.fromIaus); }
            } else if (!aimedMiss) SpellHit(*a, *s, *d, sh.fromIaus);
        }
        // Begin a cast (engine AI or IAUS). cost = what the release will take.
        void StartCast(Body& x, int idx, const SimSpell& s, std::uint32_t target, float cost, bool fromIaus) {
            x.act = Act::Cast; x.actSpell = idx; x.actTarget = target; x.actCost = cost; x.actIaus = fromIaus;
            x.hitAt = now + s.castSec; x.actEnd = x.hitAt + TM::castRecover; x.c.caster = true;
            if (trace) std::fprintf(trace, "%7.2f  cast %s begins %s -> %u (%s, %.1fs, cost %.0f, mp %.0f)\n", now, x.c.name.c_str(), s.e.name.c_str(), target, fromIaus ? "decision" : "own AI", s.castSec, cost, x.c.mp);
            if (x.role == Role::Foe && (s.e.nature == Prune::Nature::Offense || s.e.nature == Prune::Nature::Heal || s.ctrl)) ++t.foeCasts;
            if (static_cast<std::size_t>(idx) < x.spellReady.size()) x.spellReady[static_cast<std::size_t>(idx)] = x.actEnd + std::max(0.0f, s.recovery);
        }
        // The engine AI's own spell choice: a heal when someone on its side is hurt, else a damage spell it can pay for.
        [[nodiscard]] bool BrainCast(Body& x, Body* tg) {
            const std::size_t n = x.ctl >= 0 ? ctl[static_cast<std::size_t>(x.ctl)].build->spells.size() : x.spec->spells.size();
            if (x.spellReady.size() < n) x.spellReady.resize(n, 0.0);
            int best = -1; float bw = 0; std::uint32_t btgt = 0; float bcost = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const SimSpell* s = SpellOf(x, static_cast<int>(i)); if (!s || now < x.spellReady[i]) continue;
                float cost = s->e.cost;
                if (x.ctl >= 0) { const SpellEntry& e = ctl[static_cast<std::size_t>(x.ctl)].book.entries[i]; if (e.source != SpellSource::Spell) continue; if (!ctl[static_cast<std::size_t>(x.ctl)].gate.spellsOnClass) continue; cost = e.cost; }
                if (cost > x.c.mp) continue;
                float w = 0; std::uint32_t tid = 0;
                if (s->e.nature == Prune::Nature::Heal && x.role == Role::Foe) {   // foes heal themselves and each other; a follower's heals are IAUS's to cast
                    Body* r = nullptr; float worst = 0.5f;
                    for (auto& o : b) if (Up(o) && o.Party() == x.Party() && (&o == &x || s->healOther) && o.c.hp / o.c.hpMax < worst && Dist(x, o) < 2000) { worst = o.c.hp / o.c.hpMax; r = &o; }
                    if (r) { w = 60.0f * (1.0f - worst) * 4.0f; tid = r->c.id; }
                } else if (s->e.nature == Prune::Nature::Offense && tg && s->dmg + s->dot * s->dotDur > 0) {
                    const float d = Dist(x, *tg); if (!s->e.touch && d > s->e.reach && s->e.reach > 0) continue; if (s->e.touch && d > Reach(x)) continue;
                    if (!Sees(x, *tg)) continue;
                    if (tg->c.outOfMeleeReach && s->e.touch) continue;
                    w = s->dmg + s->dot * s->dotDur; tid = tg->c.id;
                }
                w *= R.U(0.6f, 1.4f);
                if (w > bw) { bw = w; best = static_cast<int>(i); btgt = tid; bcost = cost; }
            }
            if (best < 0) return false;
            StartCast(x, best, *SpellOf(x, best), btgt, bcost, false); return true;
        }
        [[nodiscard]] float FleeBelow(const Body& x) const {   // AIDT confidence: 0 cowardly .. 4 foolhardy (MODEL thresholds)
            if (x.Party()) return 0.0f;
            switch (x.spec->conf) { case 0: return 0.75f; case 1: return 0.40f; case 2: return 0.20f; case 3: return 0.08f; default: return 0.0f; }
        }
        void Brain(Body& x) {
            const Rules& r = D->rules; const Style& st = x.spec->st;
            if (x.spec->dragon) {   // MODEL: 12 s on the wing, 15 s on the ground
                if (x.airUntil == 0 && x.groundUntil == 0) x.groundUntil = now + R.U(6, 14);
                if (x.c.outOfMeleeReach && now >= x.airUntil) { x.c.outOfMeleeReach = false; x.groundUntil = now + 15; }
                else if (!x.c.outOfMeleeReach && now >= x.groundUntil) { x.c.outOfMeleeReach = true; x.airUntil = now + 12; }
            }
            Ctl* c = x.ctl >= 0 ? &ctl[static_cast<std::size_t>(x.ctl)] : nullptr;
            if (x.role == Role::Player && x.potions > 0 && x.c.hp < 0.35f * x.c.hpMax) {   // the player model drinks (MODEL: 40% of his health, 1 s)
                --x.potions; Heal(x, 0.4f * x.c.hpMax, nullptr, false); x.act = Act::Drink; x.actEnd = now + 1.0; x.hitAt = -1; return; }
            // flee (a fear effect, or low health for the unconfident)
            Body* threat = nullptr; { float bd = 1e9f; for (auto& o : b) if (Up(o) && o.Party() != x.Party()) { const float d = Dist(x, o); if (d < bd) { bd = d; threat = &o; } } }
            if (!threat) return;
            if (now < x.fleeUntil || (x.c.hp < FleeBelow(x) * x.c.hpMax && now >= x.think) || now < x.retreatUntil) {
                if (now >= x.fleeUntil && now >= x.retreatUntil) { x.fleeUntil = now + R.U(4, 7); x.c.fleeing = true; }
                Move(x, x.c.x + (x.c.x - threat->c.x), x.c.y + (x.c.y - threat->c.y), x.spec->run);
                if (now < x.retreatUntil && x.ctl == 0) t.retreatSec += TM::dt;
                return;
            }
            x.c.fleeing = false;
            if (c && now < c->moveUntil) {   // a reposition order: he goes there first (sprint = x1.5, stamina 8 / s: MODEL)
                const float gd = std::hypot(c->goalX - x.c.x, c->goalY - x.c.y);
                if (gd > 40.0f) { const bool sp = c->sprint && x.c.sp > 5.0f; if (sp) { x.c.sp -= 8.0f * TM::dt; x.regenAt[2] = now + D->rules.delaySp; }
                    Move(x, c->goalX, c->goalY, x.spec->run * (sp ? 1.5f : 1.0f)); if (x.ctl == 0) t.moveOrderSec += TM::dt; return; }
                c->moveUntil = 0;
            }
            // target
            Body* tg = Find(x.c.targetId);
            const bool taunted = now < x.tauntUntil;
            if (taunted) { Body* tb = Find(x.tauntBy); if (tb && Up(*tb)) tg = tb; else x.tauntUntil = 0; }
            else if (c && c->steer) { Body* s = Find(c->steer); if (s && Up(*s) && !s->Party()) tg = s; else c->steer = 0; }
            if (!tg || !Up(*tg) || (tg->Party() == x.Party() && now >= x.frenzyUntil) || (!taunted && !(c && c->steer) && now >= x.retargetAt)) {
                tg = PickTarget(x); x.retargetAt = now + R.U(3, 7);
            }
            if (!tg) return;
            if (x.c.targetId != tg->c.id) { x.c.targetId = tg->c.id; if (x.ctl == 0) ++t.tgtSwitches; }
            const float d = Dist(x, *tg); const bool los = Sees(x, *tg);
            // mode: what the combat style and the kit favour at this distance
            const bool canCast = (x.ctl < 0 || engineCasts) && (x.ctl >= 0 ? !ctl[static_cast<std::size_t>(x.ctl)].build->spells.empty() && c->drawn != PostureKind::Melee && c->drawn != PostureKind::Bow && c->drawn != PostureKind::Silver : !x.spec->spells.empty());
            const bool hasRanged = x.spec->ranged.Has() && x.arrows > 0 && (!c || c->drawn == PostureKind::Bow);
            const bool hasMelee = !c || c->drawn == PostureKind::Melee || c->drawn == PostureKind::Silver || c->drawn == PostureKind::None;
            x.casterMode = canCast && st.wMagic >= 0.9f;
            x.rangedMode = !x.casterMode && hasRanged && (c ? true : (st.wRanged > st.wMelee || d > 700.0f || tg->c.outOfMeleeReach || !x.spec->melee.Has()));
            x.c.caster = x.casterMode;
            if (c && c->drawn == PostureKind::Staff && now >= x.think) {   // a drawn staff is fired by the engine AI while it has a charge
                for (std::size_t i = 0; i < c->book.entries.size(); ++i) { const SpellEntry& e = c->book.entries[i];
                    if (e.source != SpellSource::Staff || !e.charged) continue;
                    if (!los || d > 2000.0f) { Move(x, tg->c.x, tg->c.y, x.spec->run); return; }
                    StartCast(x, static_cast<int>(i), c->build->spells[i], tg->c.id, 0.0f, false); x.think = x.actEnd + R.U(TM::thinkMin, TM::thinkMax); return; }
            }
            if (x.casterMode) {
                if (now >= x.think && BrainCast(x, tg)) { x.think = x.actEnd + R.U(TM::thinkMin, TM::thinkMax) / std::max(0.25f, st.off); return; }
                // nothing castable now: keep the standoff, or fall back to the blade when out of magicka (a dagger)
                bool anyAfford = false; { const std::size_t n = x.ctl >= 0 ? c->build->spells.size() : x.spec->spells.size();
                    for (std::size_t i = 0; i < n; ++i) { const SimSpell* s = SpellOf(x, static_cast<int>(i)); const float cost = x.ctl >= 0 ? c->book.entries[i].cost : (s ? s->e.cost : 1e9f);
                        if (s && s->e.nature == Prune::Nature::Offense && cost <= x.c.mp && !(x.ctl >= 0 && c->book.entries[i].source != SpellSource::Spell)) anyAfford = true; } }
                if (anyAfford || (x.Party() && x.c.mp > 0.15f * x.c.mpMax)) {
                    const float want = 750.0f;
                    if (!los || d > want * 1.4f) Move(x, tg->c.x, tg->c.y, x.spec->run);
                    else if (d < 320.0f) Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f);
                    return;
                }
                x.casterMode = false;
            }
            if (x.rangedMode) {
                if (!los || d > r.rangedDist * 1.6f) { Move(x, tg->c.x, tg->c.y, x.spec->run); return; }
                if (d < r.rangedMin && st.fallback > 0.05f) { Move(x, x.c.x + (x.c.x - tg->c.x), x.c.y + (x.c.y - tg->c.y), x.spec->run * 0.55f); }
                if (now >= x.think) {
                    const Weapon& w = x.spec->ranged; const float draw = w.anim == 9 ? TM::crossbowReload : TM::bowNock + r.bowDraw / std::max(0.3f, w.speed) * 0.6f + R.U(TM::bowHoldMin, TM::bowHoldMax) * 0.5f;
                    x.act = Act::Shoot; x.actTarget = tg->c.id; x.hitAt = now + draw; x.actEnd = x.hitAt + TM::bowRecover; x.actIaus = false;
                    x.think = x.actEnd + R.U(TM::thinkMin, TM::thinkMax) * 0.5f / std::max(0.25f, st.off);
                }
                return;
            }
            if (!hasMelee) return;
            // melee
            const float reach = Reach(x);
            if (d > reach * 0.9f || tg->c.outOfMeleeReach) { if (!tg->c.outOfMeleeReach) Move(x, tg->c.x, tg->c.y, x.spec->run); else if (d > 400) Move(x, tg->c.x, tg->c.y, x.spec->run * 0.5f); return; }
            if (now < x.think) return;
            const bool tgBlocking = now < tg->blockUntil, tgStag = now < tg->staggerUntil, tgCasting = tg->act == Act::Cast && tg->hitAt >= 0;
            const Weapon& w = x.spec->melee;
            Act kind = Act::Swing;
            const bool engPower = !c || c->gate.powerAttack || !c->gate.resolved;   // a class without a power share does not power attack on its own (the ch.18 share denies it)
            if (x.spec->shield && (tgCasting || tgBlocking) && R.P(std::clamp(0.18f * st.bash, 0.0f, 0.6f)) && x.c.sp >= 20) kind = Act::Bash;
            else if (engPower && x.c.sp >= PowerStamina(r, w) && R.P(std::clamp(0.12f * st.off * (tgBlocking ? 1.5f * st.powBlocking : 1.0f) * (tgStag ? 1.3f * st.powStaggered : 1.0f), 0.0f, 0.6f))) kind = Act::Power;
            StartSwing(x, *tg, kind, false);
            x.think = x.actEnd + R.U(TM::thinkMin, TM::thinkMax) / std::max(0.25f, st.off);
        }

        // ── one 30 Hz step of the world (the followers' decision layer is served by the caller's policy) ──
        void Regen(Body& x) {
            const Rules& r = D->rules;
            if (now >= x.regenAt[0]) x.c.hp = std::min(x.c.hpMax, x.c.hp + x.c.hpMax * x.spec->hpR / 100.0f * r.combatHp * TM::dt);
            if (now >= x.regenAt[1]) x.c.mp = std::min(x.c.mpMax, x.c.mp + x.c.mpMax * x.spec->mpR / 100.0f * r.combatMp * TM::dt);
            if (now >= x.regenAt[2]) x.c.sp = std::min(x.c.spMax, x.c.sp + x.c.spMax * x.spec->spR / 100.0f * r.combatSp * TM::dt);
            x.recentDmg *= 1.0f - TM::dt / 2.0f; x.armorBuff = 0;
            for (std::size_t i = 0; i < x.fx.size();) {
                Effect& e = x.fx[i];
                if (e.until <= now) { x.fx[i] = x.fx.back(); x.fx.pop_back(); continue; }
                if (e.kind == 0) Hurt(x, e.v * TM::dt, Find(e.src)); else if (e.kind == 1) Heal(x, e.v * TM::dt, nullptr, false); else if (e.kind == 2) x.armorBuff = std::max(x.armorBuff, e.v);
                ++i;
            }
        }
        void StepWorld() {
            for (std::size_t i = 0; i < b.size(); ++i) {
                Body& x = b[i]; if (!x.alive) continue;
                if (x.c.downed) { if (now >= x.downUntil) { x.c.downed = false; x.c.hp = std::max(x.c.hp, D->rules.bleedoutDefault * x.c.hpMax); } continue; }
                if (x.role == Role::Summon && x.airUntil < 0 && now >= -x.airUntil) { x.alive = false; continue; }
                Regen(x); if (!Up(x)) continue;
                if (x.role == Role::Foe && now < x.fleeUntil && Dist(x, b[0]) > 4096.0f) { x.alive = false; ++t.escaped; if (trace) std::fprintf(trace, "%7.2f  ESCAPED %s (%u)\n", now, x.c.name.c_str(), x.c.id); continue; }   // a routed foe past the chase cap has left the fight
                if (now >= x.ctrlUntil) x.c.paralysed = false;
                if (now >= x.fleeUntil) x.c.fleeing = now < x.retreatUntil;
                if (now >= x.blockUntil) x.c.blocking = false;
                if (now >= x.tauntUntil) x.c.taunted = false;
                if (x.hitAt >= 0 && now >= x.hitAt && now >= x.staggerUntil && now >= x.ctrlUntil) Resolve(b[i]);
            }
            for (std::size_t i = 0; i < late.size();) { if (late[i].at <= now) { const Spawn sp = late[i]; late[i] = late.back(); late.pop_back(); Body& n = Add(Role::Foe, sp.spec, sp.x, sp.y); n.tag = sp.tag; n.dmgScale = sp.dmgScale;
                    if (!ctl.empty() && t.mpAtLate < 0) { const Body& s0 = b[static_cast<std::size_t>(ctl[0].body)]; t.mpAtLate = s0.c.mpMax > 0 ? s0.c.mp / s0.c.mpMax : 1.0; } } else ++i; }
            for (std::size_t i = 0; i < shots.size();) { if (shots[i].at <= now) { const Shot s = shots[i]; shots[i] = shots.back(); shots.pop_back(); Land(s); } else ++i; }
            for (std::size_t i = 0; i < b.size(); ++i) { Body& x = b[i]; if (!Up(x) || Busy(x)) continue; if (x.act != Act::None) { x.act = Act::None; x.c.blocking = false; } Brain(b[i]); }
            // tallies that integrate over time
            Body& pl = Player(); bool pressed = false; bool anyFoe = false, anyParty = false;
            for (auto& x : b) { if (!Up(x)) continue; if (x.role == Role::Foe) { anyFoe = true; if (x.c.targetId == pl.c.id && Dist(x, pl) < 1500) pressed = true; } else if (x.role == Role::Player || x.role == Role::Follower) anyParty = true; }
            if (pressed) t.playerPressureSec += TM::dt;
            if (pl.alive) t.playerMinHp = std::min(t.playerMinHp, double(pl.c.hp / std::max(1.0f, pl.c.hpMax))); else t.playerMinHp = 0;
            if (!ctl.empty()) { const Body& s0 = b[static_cast<std::size_t>(ctl[0].body)]; if (Up(s0)) { t.upSec += TM::dt; const std::size_t ai = now < s0.actEnd ? static_cast<std::size_t>(s0.act) : 0; t.bodySec[ai] += TM::dt; if (ai && s0.actIaus) t.bodyIausSec[ai] += TM::dt; } }
            if (!ctl.empty()) { Ctl& c0 = ctl[0]; const Body& s0 = b[static_cast<std::size_t>(c0.body)];
                if (pressed && c0.pressureSince < 0) c0.pressureSince = now; if (!pressed) c0.pressureSince = -1;
                t.mpSum += s0.c.mpMax > 0 ? s0.c.mp / s0.c.mpMax : 1; t.hpSum += s0.c.hp / std::max(1.0f, s0.c.hpMax); t.spSum += s0.c.spMax > 0 ? s0.c.sp / s0.c.spMax : 1; ++t.resN;
                if (c0.gate.spellsOnClass && Up(s0)) { float cheapest = 1e9f; for (const auto& e : c0.book.entries) if (e.source == SpellSource::Spell && e.cost > 0 && e.drop == Prune::Drop::Keep) cheapest = std::min(cheapest, e.cost); if (cheapest < 1e8f && s0.c.mp < cheapest) t.mpDrySec += TM::dt; } }
            now += TM::dt;
            if (!anyFoe && late.empty()) { over = true; t.won = true; }
            else if (!pl.alive || !anyParty) { over = true; t.lost = true; }
            else if (now >= maxSec) over = true;
            if (over) t.dur = now;
        }
    };

}
