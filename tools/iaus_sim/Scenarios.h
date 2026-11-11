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
        bool stealth = false;   // a class playstyle scenario of the stealth lab: never part of the headline J, run by name or by --scenario stealth
        bool parked = false;    // a PARKED class's scenario (T3 / T4): excluded unless --parked
        std::function<FightSpec(const Data&, const std::vector<SimClass>&, Rng&)> make;
        Scenario(const char* n, const char* w, const char* m, bool lb, std::function<FightSpec(const Data&, const std::vector<SimClass>&, Rng&)> mk, bool st = false, bool pk = false) : name(n), what(w), metrics(m), lowerBetter(lb), stealth(st), parked(pk), make(std::move(mk)) {}
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

    inline std::vector<Scenario> AllScenarios(bool withStealth = false) {
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
        if (!withStealth) return v;
        // ── the stealth lab (iaus-classes.md): meaningful under --stealth g1 | hide | aware; under off they are plain fights ──
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
        return v;
    }

}
