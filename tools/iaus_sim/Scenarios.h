#pragma once
// iaus_sim/Scenarios.h -- NAMED, reproducible fights. Each one isolates a piece of combat intelligence and names the
// metrics that score it. Foes come from the list's own records by role (Data::roles) and fall back to the fixtures.
// HOST-ONLY.
#include "iaus_sim/Oracle.h"
#include "iaus_sim/Setup.h"

namespace Sim {

    struct Scenario {
        const char* name;
        const char* what;       // what a skilled player does here
        const char* metrics;    // comma list of metric keys to print (sim.cpp Metrics), first = the primary
        bool lowerBetter;       // for the primary metric
        bool stealth = false;   // a scenario of the stealth / disengage lab (G1, G2, G3): part of the headline J and of "--scenario all" only while a stealth road is modelled (--stealth g1|hide|g3); by name or --scenario stealth always
        bool parked = false;    // a PARKED class's scenario (T3 / T4): excluded unless --parked
        bool field = false;     // SIM V3: a FIELD FAILURE reproduction (iaus-sim-v3.md 8): by name or --scenario field only, never part of "all" or of the headline J
        std::function<FightSpec(const Data&, const std::vector<SimClass>&, Rng&)> make;
        Scenario(const char* n, const char* w, const char* m, bool lb, std::function<FightSpec(const Data&, const std::vector<SimClass>&, Rng&)> mk, bool st = false, bool pk = false, bool fd = false) : name(n), what(w), metrics(m), lowerBetter(lb), stealth(st), parked(pk), field(fd), make(std::move(mk)) {}
    };

    inline const SimClass* ClassBySlug(const std::vector<SimClass>& v, const char* slug) {
        for (const SimClass& c : v) if (c.name == slug) return &c;
        return &v[0];
    }
    // A foe of a role near a level (nullptr when the list has none).
    inline std::shared_ptr<const ActorSpec> RoleSpec(const Data& D, const char* role, int level, Rng& R) {
        auto it = D.roles.find(role); if (it == D.roles.end() || it->second.empty()) return nullptr;
        int best = 1 << 30; std::vector<int> pool;
        for (int i : it->second) { const int d = std::abs(D.specs[static_cast<std::size_t>(i)].level - level); if (d < best) { best = d; pool.clear(); } if (d == best) pool.push_back(i); }
        return std::make_shared<ActorSpec>(D.specs[static_cast<std::size_t>(pool[static_cast<std::size_t>(R.I(0, static_cast<int>(pool.size()) - 1))])]);
    }
    inline std::shared_ptr<const ActorSpec> Foe(const Data& D, std::initializer_list<const char*> roles, int level, Rng& R, float hpX = 1, float dmgX = 1, const char* name = nullptr) {
        std::shared_ptr<const ActorSpec> s;
        for (const char* r : roles) { s = RoleSpec(D, r, level, R); if (s) break; }
        if (!s) s = std::make_shared<ActorSpec>(D.specs[0]);
        auto m = std::make_shared<ActorSpec>(*s); m->hp *= hpX; m->atkMult *= dmgX; if (name) m->name = name;
        if (hpX > 2.5f) m->conf = 4;
        return m;
    }

    inline std::vector<Scenario> AllScenarios(bool withStealth = false, bool withField = false, bool withPressure = false) {
        using FS = FightSpec;
        auto base = [](const std::vector<SimClass>& c, const char* slug, int level, Cls player) { FS f; f.cls = ClassBySlug(c, slug); f.level = level; f.rank = 3; f.playerFam = player; f.foeDist = 1300; f.occluders = 0; return f; };
        std::vector<Scenario> v;
        v.push_back({ "tank3", "the player tanks three draugr: kill them one at a time, the weakest first, never leave the player alone with all three",
            "U,win%,pTaken/s,dur.s,tsw/min", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "warrior", 25, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "draugr", "bandit" }, 22, R)); f.foesOnPlayer = true; f.foeDist = 700; f.targetRatio = 0.80f; return f; } });
        v.push_back({ "mage-dragon", "a mage against a dragon: stay at range, use the element it is weak to, do not burn everything while it is in the air out of reach",
            "U,win%,downs/f,dry%,weak%,resisted%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 30, Cls::Melee);
                f.customFoes.push_back(Foe(D, { "dragon" }, 30, R)); f.foeTags = { "boss" }; f.ensure = { "fire", "frost", "shock", "heal" }; f.foeDist = 1800; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "bandit-pack", "a bandit pack with an archer: the archer hurts most for his health, he goes first if he can be reached",
            "U,win%,prio%,dur.s,pTaken/s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "archer", 25, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "bandit" }, 22, R)); f.customFoes.push_back(Foe(D, { "bandit_archer", "draugr_archer" }, 24, R)); f.foeTags = { "grunt", "grunt", "grunt", "archer" }; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "focus-healer", "a healer keeps the others alive: every second spent on anyone else is wasted, kill the healer first",
            "prio%,U,win%,dur.s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "archer", 25, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R, 1.5f)); f.customFoes.push_back(Foe(D, { "healer" }, 24, R, 1.0f)); f.foeTags = { "grunt", "grunt", "healer" }; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "interrupt", "a caster is charging a heavy spell: stagger him (power attack, bash, a stagger spell) before the release",
            "intr%,U,win%,pTaken/s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "warrior", 25, Cls::Melee);
                f.customFoes.push_back(Foe(D, { "mage", "bandit_mage" }, 26, R, 4.0f, 1.0f)); f.foeTags = { "caster" }; f.foeDist = 600; f.targetRatio = 0.70f; return f; } });
        v.push_back({ "weakness", "a troll burns and a frost atronach shrugs off frost: pick the spell by the target's resists, never cast into an immunity",
            "immune%,resisted%,weak%,U,win%,dur.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                f.customFoes.push_back(Foe(D, { "frost_troll", "troll" }, 25, R)); f.customFoes.push_back(Foe(D, { "frost_atronach" }, 25, R)); f.ensure = { "fire", "frost", "shock" }; f.targetRatio = 0.80f; return f; } });
        v.push_back({ "predictive-heal", "a boss hits the player for a third of his health per blow: heal by what is coming, not after it has landed",
            "pdead%,U,pMinHp%,overheal%,heal.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "healer", 28, Cls::Melee);
                f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 30, R, 5.0f, 2.2f, "Boss")); f.foeTags = { "boss" }; f.foesOnPlayer = true; f.playerPotions = 0; f.ensure = { "healOther", "heal" }; f.foeDist = 500; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "peel", "two foes are on a squishy player: whoever is on the player is handled first",
            "press%,peel.s,pTaken/s,U,pdead%", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "mercenary", 25, Cls::Mage);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "wolf", "bandit" }, 22, R, 1.5f)); f.customFoes.push_back(Foe(D, { "bandit_archer", "bandit" }, 22, R)); f.foesOnPlayer = true; f.foeDist = 600; f.playerPotions = 1; f.targetRatio = 0.80f; return f; } });
        v.push_back({ "overkill", "five weak wolves: a big spell on a nearly dead wolf is wasted, small hits and the next target",
            "overkill%,dry%,U,dur.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                for (int i = 0; i < 5; ++i) f.customFoes.push_back(Foe(D, { "wolf", "bandit" }, 8, R)); f.ensure = { "fire", "shock" }; f.targetRatio = 0.40f; return f; } });
        v.push_back({ "double-heal", "two healers and one hurt player: one heals, the other keeps fighting",
            "dblheal%,overheal%,U,dur.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "healer", 25, Cls::Melee);
                f.others.push_back(ClassBySlug(c, "healer")); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R, 2.0f)); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R, 2.0f));
                f.playerHp = 0.45f; f.playerPotions = 0; f.foesOnPlayer = true; f.ensure = { "healOther", "heal" }; f.ensureOthers = { "healOther", "heal" }; f.foeDist = 600; f.targetRatio = 0.80f; return f; } });
        v.push_back({ "mp-budget", "four grunts now and a boss at 25 s: do not arrive at the boss with an empty pool",
            "mpAtLate%,dry%,U,win%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                for (int i = 0; i < 4; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 14, R)); f.late.push_back({ 25.0, Foe(D, { "bandit", "draugr" }, 32, R, 4.0f, 1.6f, "Boss") }); f.lateTags = { "boss" }; f.ensure = { "fire", "heal" }; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "splash", "the player fights in the middle of four foes: no blast that reaches him, single target spells or a different position",
            "splashHits/f,splash%,U,dur.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 30, Cls::Melee);
                for (int i = 0; i < 4; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R)); f.foesOnPlayer = true; f.foeDist = 500; f.foeSpread = 150; f.ensure = { "area", "fire" }; f.targetRatio = 0.80f; return f; } });
        v.push_back({ "control-setup", "an ally paralyses a strong foe: every blow goes into the helpless one, power attacks first",
            "ctlFocus%,powCtl%,U,dur.s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "warrior", 30, Cls::Ranged);
                f.others.push_back(ClassBySlug(c, "evoker")); f.ensureOthers = { "paralyze" }; f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 30, R, 3.0f, 1.3f)); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 26, R, 1.5f)); f.foeDist = 800; f.targetRatio = 0.90f; return f; } });
        v.push_back({ "kite", "a brute he cannot trade with is on him: back off, shoot, never stand and trade",
            "taken/s,downs/f,U,retreat%", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "thief", 22, Cls::Ranged);
                f.customFoes.push_back(Foe(D, { "troll", "giant", "bandit" }, 26, R, 2.0f, 2.0f, "Brute")); f.foeTags = { "boss" }; f.foeDist = 900; f.onSubject = true; f.targetRatio = 1.10f; return f; } });
        v.push_back({ "knight-boss", "a far stronger foe is on the player and nobody can heal the knight: taunt only if he can hold it, or the player is about to die",
            "U,pdead%,downs/f,tntDown%,taunt/min", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "knight", 25, Cls::Ranged);
                f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 34, R, 6.0f, 3.0f, "Boss")); f.foeTags = { "boss" }; f.foesOnPlayer = true; f.foeDist = 600; f.subjectPotionsH = 1; f.targetRatio = 1.20f; return f; } });
        v.push_back({ "knight-boss-healer", "the same boss with a healer in the party: now he can hold it, taunt and keep it",
            "U,pdead%,downs/f,tntDown%,taunt/min", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "knight", 25, Cls::Ranged);
                f.others.push_back(ClassBySlug(c, "healer")); f.ensureOthers = { "healOther", "heal" };
                f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 34, R, 6.0f, 3.0f, "Boss")); f.foeTags = { "boss" }; f.foesOnPlayer = true; f.foeDist = 600; f.targetRatio = 1.20f; return f; } });
        v.push_back({ "hurt-knight", "the knight is at 30 percent and the player at 20 with a foe on him: the taunt is a sacrifice and it is right",
            "pdead%,U,taunt/min,downs/f", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "knight", 25, Cls::Mage);
                f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 25, R, 1.5f, 1.2f)); f.foesOnPlayer = true; f.foeDist = 450; f.subjectHp = 0.30f; f.playerHp = 0.20f; f.playerPotions = 0; f.subjectPotionsH = 0; f.targetRatio = 0.60f; return f; } });
        // ── SIM V3, --caster-pressure (review F6): foes that DIVE the caster. One per casting cell, appended after everything the headline already
        // holds (a block of its own, so no existing scenario changes its index or its fights). The lab's foes rarely choose the caster on
        // their own (World.h PickTarget, unsourced), so without these a caster's tuning never meets blades on him.
        auto pressure = [&]() { if (!withPressure) return;
            static const char* const kSlug[6] = { "mage", "evoker", "conjurer", "healer", "warlock", "mage-off" };
            static const char* const kName[6] = { "dive-mage", "dive-evoker", "dive-conjurer", "dive-healer", "dive-warlock", "dive-mage-off" };
            for (int i = 0; i < 6; ++i) { const char* slug = kSlug[i];
                v.push_back({ kName[i], "three blades start on the caster and a melee player is beside him: stay on spells, keep the distance, live",
                    "U,downs/f,taken/s,drawnMelee%,contact%,minTgt,dl.fired%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, slug, 25, Cls::Melee);
                        for (int k = 0; k < 3; ++k) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R)); f.foeTags = { "grunt", "grunt", "grunt" }; f.ensure = { "fire", "heal" }; f.onSubject = true; f.foeDist = 1100; f.targetRatio = 0.95f; return f; } }); } };
        if (!withStealth) { pressure(); return v; }
        // ── the stealth and disengage lab (iaus-classes.md section 6, iaus-sim-v2.md section 30): meaningful under --stealth g1 | hide | g3; under off they are plain fights ──
        v.push_back({ "thief-cover", "three bandits have found the thief among the pillars of a lit hall: break their line of sight behind cover, crouch until their group loses him, come back with blows from stealth",
            "U,hideOk%,sneakDmg%,hidden%,taken/s,downs/f,hide/min,stealthMiss%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "thief", 25, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 23, R)); f.foeTags = { "grunt", "grunt", "grunt" };
                f.interior = true; f.light = 0.9f; f.occluders = 0; f.cover = { { -120, 430, 230 }, { -120, -430, 230 }, { -760, 0, 200 } }; f.freeEntry = 0; f.onSubject = true; f.foeDist = 800; f.targetRatio = 0.95f; return f; }, true });
        v.push_back({ "thief-open", "the same three on the thief on open ground at noon, nothing to hide behind: no hide run exists, so he keeps his distance and shoots, and never stands still hoping to be lost",
            "U,taken/s,downs/f,sneakDmg%,hidden%,retreat%,still%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "thief", 25, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 23, R)); f.foeTags = { "grunt", "grunt", "grunt" };
                f.interior = false; f.light = 0.9f; f.occluders = 0; f.freeEntry = 0; f.onSubject = true; f.foeDist = 800; f.targetRatio = 0.95f; return f; }, true });
        v.push_back({ "caster-flee", "an evoker at a quarter of his health knows a heal, two blades are closing on him and the player is busy: get away from them, heal on the way, come back and cast",
            "U,downs/f,flee/f,fleeHealed%,runDown%,taken/s,healed%,retreat%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Ranged);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 26, R)); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 26, R)); f.foeTags = { "grunt", "grunt", "grunt" };
                f.ensure = { "heal", "fire" }; f.subjectHp = 0.27f; f.subjectPotionsH = 0; f.onSubject = true; f.foeDist = 550; f.foeSpread = 150; f.targetRatio = 0.85f; return f; }, true });
        v.push_back({ "melee-flee", "a mercenary at a quarter of his health, no healer in the party, one potion, two foes on him: drink, give ground toward the player, do not die where he stands",
            "U,downs/f,taken/s,flee/f,retreat%,healed%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "mercenary", 26, Cls::Ranged);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 26, R, 1.3f)); f.foeTags = { "grunt", "grunt" };
                f.subjectHp = 0.27f; f.subjectPotionsH = 1; f.onSubject = true; f.foeDist = 450; f.foeSpread = 150; f.targetRatio = 0.85f; return f; }, true });
        v.push_back({ "ambush", "the thief's free stealth entry against a camp with a mage: the one sneak attack he gets for nothing goes into the mage, not into a grunt",
            "U,opener%,sneakDmg%,prio%,dur.s,win%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "thief", 25, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 22, R)); f.customFoes.push_back(Foe(D, { "bandit_mage", "mage" }, 24, R)); f.foeTags = { "grunt", "grunt", "caster" };
                f.interior = true; f.light = 0.3f; f.occluders = 1; f.freeEntry = 1; f.foesOnPlayer = true; f.foeDist = 1100; f.targetRatio = 0.90f; return f; }, true });
        v.push_back({ "hit-and-hide", "a brute has seen the master thief and he cannot trade with it: break off behind cover, hide, come back with a sneak attack",
            "U,sneakDmg%,hidden%,hide/min,taken/s,downs/f", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "master-thief", 25, Cls::Melee);
                f.customFoes.push_back(Foe(D, { "troll", "giant", "bandit" }, 26, R, 2.0f, 1.6f, "Brute")); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 22, R)); f.foeTags = { "boss", "grunt" };
                f.interior = true; f.light = 0.35f; f.occluders = 3; f.freeEntry = 0; f.onSubject = true; f.foeDist = 900; f.targetRatio = 1.00f; return f; }, true, true });
        v.push_back({ "shadowcloak", "open ground in daylight, nowhere to hide: the Nightingale's cloak is his only stealth, and every cloak must end in a sneak attack",
            "U,sneakDmg%,sneakAtk%,hidden%,dur.s,win%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "nightingale", 30, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 28, R, 2.0f, 1.2f)); f.light = 0.9f; f.occluders = 0; f.freeEntry = 0; f.foeDist = 900; f.targetRatio = 0.95f; return f; }, true, true });
        v.push_back({ "flank", "a knight holds one of two strong foes: the master thief stabs the held one (the flanking half bonus), never the free one that would turn on him",
            "U,flank%,sneakDmg%,taken/s,dur.s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "master-thief", 28, Cls::Ranged);
                f.others.push_back(ClassBySlug(c, "knight")); for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 28, R, 2.5f, 1.2f));
                f.interior = true; f.light = 0.5f; f.occluders = 0; f.freeEntry = 0; f.foeDist = 700; f.targetRatio = 0.95f; return f; }, true, true });
        v.push_back({ "assassinate", "a healer keeps two brutes alive: the Listener's stealth entry is the one chance to kill the healer before the fight starts",
            "prio%,U,opener%,sneakDmg%,dur.s,win%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "listener", 30, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 28, R, 1.5f)); f.customFoes.push_back(Foe(D, { "healer" }, 26, R)); f.foeTags = { "grunt", "grunt", "healer" };
                f.interior = true; f.light = 0.3f; f.occluders = 2; f.freeEntry = 1; f.foesOnPlayer = true; f.foeDist = 1100; f.targetRatio = 0.95f; return f; }, true, true });
        pressure();
        if (!withField) return v;
        // ── SIM V3: the field failures the lab could not see (ClickUp 1dfcuabd44c; iaus-sim-v3.md 8 and 11). Open ground, no random cover: the geometry is the point ──
        v.push_back({ "jesper-dagger", "an evoker with a dagger on his belt behind a melee player, three bandits: he stays on his spells at spell range; with the blade drawn the engine walks him into the blades",
            "U,downs/f,taken/s,drawnMelee%,contact%,minTgt,tgtDist,dl.fired%,dl.lofHeld%,dps", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 25, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R)); f.foeTags = { "grunt", "grunt", "grunt" }; f.ensure = { "fire", "heal" }; f.onSubject = true; f.foeDist = 1100; f.targetRatio = 0.95f; return f; }, false, false, true });
        v.push_back({ "dragon-circle", "a dragon circles and fights the player: the evoker keeps it as his target for the whole circuit and casts whenever it is in range, he never drops it",
            "cap.drop%,cap.starve%,cap.mean,cap.waived/f,nocand%,idle%,U,win%,dps", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 30, Cls::Ranged);
                f.customFoes.push_back(Foe(D, { "dragon" }, 30, R)); f.foeTags = { "boss" }; f.ensure = { "fire", "frost", "shock" }; f.foesOnPlayer = true; f.foeDist = 1500; f.targetRatio = 0.90f; return f; }, false, false, true });
        v.push_back({ "fire-lane", "the player and a knight stand between the evoker and two bandits: a bolt through a friend's back is held, so step to a clear line first",
            "dl.fired%,dl.landed%,dl.lofHeld%,dl.lofHold.s,dl.lofExpired%,dl.claim.s,strafe/f,U,dps", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 26, Cls::Melee);
                f.others.push_back(ClassBySlug(c, "knight")); for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 25, R, 2.0f)); f.foeTags = { "grunt", "grunt" };
                f.ensure = { "fire", "shock" }; f.foesOnPlayer = true; f.foeDist = 700; f.foeSpread = 120; f.targetRatio = 0.90f; return f; }, false, false, true });
        v.push_back({ "knight-ring", "four bandits come at the knight from four sides, no taunt: a guard only stops what is in front of him, so face the one that swings and keep the wall at his back",
            "guardAll%,arcMiss%,guardDown%,inc/f,guard2%,guard%,U,downs/f,taken/s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "knight", 26, Cls::Ranged);
                for (int i = 0; i < 4; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R)); f.foeTags = { "grunt", "grunt", "grunt", "grunt" }; f.onSubject = true; f.ring = 700; f.targetRatio = 0.95f; return f; }, false, false, true });
        v.push_back({ "bow-channel", "an archer knows a channelled self buff (the field report of 11-12): while it runs her bow is off her back, so the buff is cast before the arrows fly or not at all",
            "handLost%,handLostDmg/f,dl.stream.s,reDraw/f,dl.pick/f,U,dps,dur.s", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "ranged-off", 26, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 25, R, 1.5f)); f.foeTags = { "grunt", "grunt" }; f.ensure = { "stream" }; f.foesOnPlayer = true; f.foeDist = 1400; f.targetRatio = 0.90f; return f; }, false, false, true });
        v.push_back({ "ledge-archer", "an archer shoots from a ledge no blade can reach and one bandit is on the ground: kill what can be reached, then answer the ledge with a bow or a spell, never swing at the air under it",
            "U,win%,timeout%,dur.s,idle%,taken/s", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "warrior", 25, Cls::Ranged);
                f.customFoes.push_back(Foe(D, { "bandit_archer", "draugr_archer", "bandit" }, 24, R)); f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 24, R)); f.foeTags = { "archer", "grunt" }; f.perch = 0; f.foeDist = 1000; f.targetRatio = 0.80f; return f; }, false, false, true });
        v.push_back({ "cloak-area", "the evoker knows a flame cloak and three bandits are on him: the cloak is worth its magicka only while foes stand inside it, and it is not cast again while it burns",
            "area.up%,area.foe.s/f,area.ally.s/f,area.recast%,U,downs/f,dps", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                for (int i = 0; i < 3; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 25, R)); f.foeTags = { "grunt", "grunt", "grunt" }; f.ensure = { "cloak", "fire" }; f.onSubject = true; f.foeDist = 600; f.targetRatio = 0.85f; return f; }, false, false, true });
        v.push_back({ "circle-area", "a healer knows a protective circle and two bandits are on the player: the circle helps whoever stands in it, so it goes where the fight is and he does not walk out of it",
            "area.up%,area.out%,area.recast%,U,pTaken/s,pdead%", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "healer", 28, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 26, R, 1.5f)); f.foeTags = { "grunt", "grunt" }; f.ensure = { "circle", "healOther", "heal" }; f.foesOnPlayer = true; f.foeDist = 600; f.targetRatio = 0.90f; return f; }, false, false, true });
        // ── THE DECK FAILURES OF 11-17 (sim mirror batch; marth: "I would update the sim and run at least a few things through it"). Before / after: --mirror off|on
        // for the engagement cases, the build before / after the fix/field-1117b-choice mirror for the choice cases ──
        v.push_back({ "heal-channel", "bandits come from every side at the hurt player and the evoker beside him: a weak channelled heal roots him while he is cut down. Heal with the stronger cast, from range, and mend himself under a quarter health before any attack",
            "downs/f,heal.conc%,heal.size,heal.self%,selfHealAt%,heal/f,pdead%,U", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                for (int i = 0; i < 5; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 27, R, 1.0f, 1.6f)); f.foeTags = { "grunt", "grunt", "grunt", "grunt", "grunt" };
                f.ensure = { "healStream", "healShot", "heal", "fire" }; f.foesOnPlayer = true; f.playerHp = 0.6f; f.playerPotions = 1; f.subjectPotionsH = 0; f.onSubjectN = 2; f.foeDist = 700; f.ring = 420; f.targetRatio = 1.0f; return f; }, false, false, true });
        v.push_back({ "offense-pick", "two frost-resistant draugr and the evoker knows a frost, a fire and a shock spell, measured: the strongest spell that WORKS on the target, never the cheapest, never frost into the resistance",
            "cast.dmg,cast.cost,resisted%,weak%,immune%,off/f,dps,U", false, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "evoker", 28, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "frost_atronach", "draugr" }, 27, R, 1.4f)); f.foeTags = { "grunt", "grunt" }; f.ensure = { "frost", "fire", "shock" }; f.foesOnPlayer = true; f.foeDist = 1300; f.targetRatio = 0.90f; return f; }, false, false, true });
        v.push_back({ "bow-occluded", "a bow thief's own target stands behind a pillar and another bandit is in the open: she walks until she sees her target (or shoots the one in sight), she does not start a hide run and she does not stand idle",
            "tfh.s,nohit%,hits/f,b.shoot,idle%,run/f,hide/min,U", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "thief", 26, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit", "draugr" }, 25, R, 1.3f)); f.foeTags = { "grunt", "grunt" }; f.onSubject = true; f.foeDist = 1500; f.foeSpread = 450; f.cover = { { 900.0f, 0.0f, 170.0f }, { 1150.0f, 260.0f, 120.0f } }; f.targetRatio = 0.85f; return f; }, false, false, true });
        v.push_back({ "tower-blade", "an archer shoots from a tower top that has a stair: the warrior walks to it and up (the 128 approach), he does not swing at the air under it",
            "tfh.s,nohit%,hits/f,win%,timeout%,dur.s,idle%,U", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "warrior", 25, Cls::Ranged);
                f.customFoes.push_back(Foe(D, { "bandit_archer", "draugr_archer", "bandit" }, 24, R, 1.5f)); f.foeTags = { "archer" }; f.perch = 0; f.perchPath = true; f.foeDist = 1000; f.targetRatio = 0.60f; return f; }, false, false, true });
        v.push_back({ "cap-archer", "two archers shoot a hurt bow follower from past his chase cap: he shoots back from where he stands (or retreats), he never stands idle under fire",
            "idle%,nocand%,tfh.s,nohit%,b.shoot,cap.kept/f,cap.starve%,retreat%,downs/f,U", true, [=](const Data& D, const std::vector<SimClass>& c, Rng& R) { FS f = base(c, "archer", 26, Cls::Melee);
                for (int i = 0; i < 2; ++i) f.customFoes.push_back(Foe(D, { "bandit_archer", "draugr_archer", "bandit" }, 25, R)); f.foeTags = { "archer", "archer" }; f.onSubject = true; f.subjectHp = 0.45f; f.subjectPotionsH = 0; f.foeDist = 2300; f.targetRatio = 0.80f; return f; }, false, false, true });
        return v;
    }

}
