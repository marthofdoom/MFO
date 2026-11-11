#pragma once
// iaus_sim/Load.h -- reading <list>.sim.json, the built-in fixture set, and building a follower. HOST-ONLY.
#include "iaus_sim/Data.h"

#include <cstdio>

namespace Sim {

    inline Thr ReadThr(const J& a) { Thr t; for (const J& p : a.a) t.emplace_back(static_cast<float>(p[0].num()), static_cast<float>(p[1].num())); return t; }

    inline Data LoadData(const char* path) {
        Data b;
        std::ifstream f(path); if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
        std::stringstream ss; ss << f.rdbuf(); const std::string txt = ss.str(); JP p{ txt }; const J root = p.val();
        b.name = root["list"].s;
        for (const auto& kv : root["gmst"].o) b.gmst[kv.first] = kv.second.num();
        ApplyRules(b);
        for (const J& s : root["foeSpells"].a) { SimSpell x; Classify(s, x); b.foeSpells.push_back(std::move(x)); }
        for (const J& s : root["specs"].a) {
            ActorSpec a; a.name = s["name"].s; a.race = s["race"].s; a.level = static_cast<int>(s["level"].num(1));
            a.hp = static_cast<float>(s["hp"].num(50)); a.mp = static_cast<float>(s["mp"].num()); a.sp = static_cast<float>(s["sp"].num());
            a.hpR = static_cast<float>(s["hpR"].num()); a.mpR = static_cast<float>(s["mpR"].num()); a.spR = static_cast<float>(s["spR"].num());
            for (std::size_t i = 0; i < 18; ++i) a.skill[i] = static_cast<float>(s["skills"][i].num(15));
            a.melee = ReadWeapon(s["melee"]); a.ranged = ReadWeapon(s["ranged"]); a.staff = s["staff"].yes(); a.shield = s["shield"].yes();
            a.ammoDmg = static_cast<float>(s["ammoDmg"].num()); a.unarmed = static_cast<float>(s["unarmed"].num(4)); a.unarmedReach = static_cast<float>(s["unarmedReach"].num(96));
            a.atkDmgMult = static_cast<float>(s["atkDmgMult"].num(1)); if (a.atkDmgMult <= 0) a.atkDmgMult = 1;
            a.ar = static_cast<float>(s["ar"].num()); a.pieces = static_cast<int>(s["pieces"].num());
            const J& r = s["resist"]; a.rFire = static_cast<float>(r["fire"].num()); a.rFrost = static_cast<float>(r["frost"].num()); a.rShock = static_cast<float>(r["shock"].num());
            a.rMagic = static_cast<float>(r["magic"].num()); a.rPoison = static_cast<float>(r["poison"].num());
            for (const J& si : s["spells"].a) { const int i = static_cast<int>(si.num(-1)); if (i >= 0 && i < static_cast<int>(b.foeSpells.size()) && b.foeSpells[static_cast<std::size_t>(i)].e.drop == Prune::Drop::Keep) a.spells.push_back(i); }
            const J& st = s["style"];
            if (st.has()) { a.hasStyle = true; Style& y = a.st; const J& g = st["general"]; const J& m = st["melee"]; const J& c = st["close"];
                auto fv = [](const J& arr, std::size_t i, float d) { return i < arr.a.size() ? static_cast<float>(arr.a[i].num(d)) : d; };
                y.off = fv(g, 0, 1); y.def = fv(g, 1, 1); y.groupOff = fv(g, 2, 1); y.wMelee = fv(g, 3, 1); y.wMagic = fv(g, 4, 1); y.wRanged = fv(g, 5, 1); y.wShout = fv(g, 6, 1);
                y.wUnarmed = fv(g, 7, 1); y.wStaff = fv(g, 8, 1); y.avoidThreat = fv(g, 9, 0.2f);
                y.atkStaggered = fv(m, 0, 1); y.powStaggered = fv(m, 1, 1); y.powBlocking = fv(m, 2, 1); y.bash = fv(m, 3, 1); y.bashRecoil = fv(m, 4, 1); y.bashAtk = fv(m, 5, 1);
                y.bashPow = fv(m, 6, 1); y.special = fv(m, 7, 1); y.circle = fv(c, 0, 0.2f); y.fallback = fv(c, 1, 0.2f); y.flankDist = fv(c, 2, 0.2f); y.stalk = fv(c, 3, 0.2f);
                y.strafe = fv(st["longRange"], 0, 0.2f); const int fl = static_cast<int>(st["flags"].num()); y.dueling = fl & 1; y.flanking = fl & 2; y.dualWield = fl & 4; }
            a.conf = static_cast<int>(s["conf"].num(2)); a.aggr = static_cast<int>(s["aggr"].num(1)); a.run = static_cast<float>(s["run"].num(370)); if (a.run < 40) a.run = 370;
            a.mass = static_cast<float>(s["mass"].num(1)); if (a.mass <= 0) a.mass = 1;
            a.undead = s["undead"].yes(); a.dragon = s["dragon"].yes(); a.daedra = s["daedra"].yes(); a.animal = s["animal"].yes(); a.creature = s["creature"].yes(); a.immunePar = s["immunePar"].yes();
            a.atkMult = static_cast<float>(s["atkMult"].num(1)); a.powMult = static_cast<float>(s["powMult"].num(1));
            b.specs.push_back(std::move(a));
        }
        for (const J& bk : root["buckets"].a) b.buckets.push_back(static_cast<int>(bk.num()));
        for (const J& e : root["encounters"].a) {
            Encounter en; en.cell = e["cell"].s; en.interior = e["interior"].yes();
            for (const J& row : e["v"].a) { std::vector<EncMember> g; if (!row.a.empty()) for (const J& m : row.a[0].a) g.push_back({ static_cast<int>(m[0].num()), static_cast<float>(m[1].num()), static_cast<float>(m[2].num()) }); en.byBucket.push_back(std::move(g)); }
            b.encounters.push_back(std::move(en));
        }
        const J& fo = root["follower"];
        for (const J& r : fo["races"].a) { FRace x; x.edid = r["edid"].s; x.hp = static_cast<float>(r["hp"].num(100)); x.mp = static_cast<float>(r["mp"].num(100)); x.sp = static_cast<float>(r["sp"].num(100));
            x.hpR = static_cast<float>(r["hpR"].num()); x.mpR = static_cast<float>(r["mpR"].num()); x.spR = static_cast<float>(r["spR"].num()); x.unarmed = static_cast<float>(r["unarmed"].num(4));
            x.run = static_cast<float>(r["run"].num(370)); x.mass = static_cast<float>(r["mass"].num(1)); for (const J& bo : r["boosts"].a) x.boosts.emplace_back(static_cast<int>(bo[0].num()), static_cast<int>(bo[1].num()));
            b.races.push_back(std::move(x)); }
        const char* cn[3] = { "melee", "ranged", "mage" };
        for (int i = 0; i < 3; ++i) { const J& c = fo["classes"][cn[i]]; FClass& k = b.cls[i];
            if (c.has()) { k.edid = c["edid"].s; for (std::size_t j = 0; j < 18; ++j) k.skillW[j] = static_cast<float>(c["skillW"][j].num()); k.hW = static_cast<float>(c["hW"].num(1)); k.mW = static_cast<float>(c["mW"].num(1)); k.sW = static_cast<float>(c["sW"].num(1)); } }
        auto wl = [&](const char* k, std::vector<Weapon>& out) { for (const J& w : fo["weapons"][k].a) out.push_back(ReadWeapon(w)); };
        wl("oneHand", b.wOneHand); wl("dagger", b.wDagger); wl("twoHand", b.wTwoHand); wl("bow", b.wBow); wl("crossbow", b.wCrossbow);
        for (const auto& kv : fo["armor"].o) { auto& v = b.armor[kv.first]; for (const J& x : kv.second.a) v.push_back(static_cast<float>(x.num())); }
        for (const J& x : fo["arrows"].a) b.arrows.push_back(static_cast<float>(x.num()));
        for (const J& x : fo["bolts"].a) b.bolts.push_back(static_cast<float>(x.num()));
        const char* pk[3] = { "24", "25", "26" };
        for (int i = 0; i < 3; ++i) for (const J& x : fo["potions"][pk[i]].a) b.pots[i].emplace_back(static_cast<float>(x[0].num()), static_cast<float>(x[1].num()));
        const J& pt = fo["perks"];
        for (const auto& kv : pt["attack"].o) b.pAttack[kv.first] = ReadThr(kv.second);
        for (const auto& kv : pt["power"].o) b.pPower[kv.first] = ReadThr(kv.second);
        for (const auto& kv : pt["spellMag"].o) b.pSpellMag[kv.first] = ReadThr(kv.second);
        for (const auto& kv : pt["spellCost"].o) b.pSpellCost[kv.first] = ReadThr(kv.second);
        for (const auto& kv : pt["armor"].o) b.pArmor[kv.first] = ReadThr(kv.second);
        bool anyTaught = false; for (const J& s : root["spells"].a) if (s["taught"].yes()) { anyTaught = true; break; }
        for (const J& s : root["spells"].a) { ++b.rawSpells; if (anyTaught && !s["taught"].yes()) continue; ++b.taughtSpells; SimSpell x; Classify(s, x); AddPoolSpell(b, std::move(x)); }
        TagRoles(b);
        return b;
    }

    // ── the built-in fixture set (no list): Skyrim.esm's settings (Rules' defaults) and a hand-written roster ──
    inline SimSpell Fx(const char* n, Prune::Nature na, int school, int resistAv, float cost, float dmg, float heal, float areaFt, float cast = 0.5f, int tier = 0) {
        SimSpell s; s.e.name = n; s.e.nature = na; s.e.resistAv = resistAv; s.e.role = Prune::RoleOf(na, resistAv); s.e.cost = cost; s.cost0 = cost; s.school = school; s.skillReq = tier;
        s.e.selfDelivery = na == Prune::Nature::Buff || na == Prune::Nature::Summon; s.e.reach = s.e.selfDelivery ? 0.0f : 4096.0f; s.dmg = dmg; s.heal = heal; s.castSec = cast;
        s.e.areaUnits = Prune::BlastUnits(areaFt, 0.0f, false); s.element = resistAv == kAvResistFire ? "fire" : resistAv == kAvResistFrost ? "frost" : resistAv == kAvResistShock ? "shock" : "";
        s.summon = na == Prune::Nature::Summon; s.ctrl = na == Prune::Nature::Control; return s;
    }

    inline Data BuiltinData() {
        Data b;   // rules = Skyrim.esm values (the struct's defaults)
        b.buckets = { 5, 12, 20, 28, 38, 50 };
        b.races.push_back({ "FixtureHuman", 100, 100, 100, 0.7f, 3, 5, 4, 370, 1, {} });
        const float w1[18] = { 3, 0, 0, 2, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, w2[18] = { 0, 0, 3, 0, 0, 0, 3, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0 }, w3[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 3, 1, 2, 0 };
        for (int j = 0; j < 18; ++j) { b.cls[0].skillW[static_cast<std::size_t>(j)] = w1[j]; b.cls[1].skillW[static_cast<std::size_t>(j)] = w2[j]; b.cls[2].skillW[static_cast<std::size_t>(j)] = w3[j]; }
        b.cls[0].hW = 3; b.cls[0].mW = 0; b.cls[0].sW = 2; b.cls[1].hW = 2; b.cls[1].mW = 0; b.cls[1].sW = 3; b.cls[2].hW = 1; b.cls[2].mW = 3; b.cls[2].sW = 0;
        auto wp = [](const char* n, float dmg, float speed, int anim, int skill, float stagger, float weight) { Weapon w; w.name = n; w.dmg = dmg; w.speed = speed; w.anim = anim; w.skill = skill; w.stagger = stagger; w.weight = weight; return w; };
        for (int i = 0; i < 8; ++i) {   // iron .. daedric-like steps
            const float t = static_cast<float>(i);
            b.wOneHand.push_back(wp("FixSword", 7 + t, 1.0f, 1, kAvOneHanded, 0.75f, 9 + t)); b.wDagger.push_back(wp("FixDagger", 4 + t, 1.3f, 2, kAvOneHanded, 0, 2 + 0.5f * t));
            b.wTwoHand.push_back(wp("FixGreatsword", 15 + 1.3f * t, 0.7f, 5, kAvTwoHanded, 1.1f, 16 + 1.5f * t)); b.wBow.push_back(wp("FixBow", 6 + 1.6f * t, 0.9f, 7, kAvMarksman, 0, 5 + t));
            b.arrows.push_back(8 + 2 * t);
            b.armor["heavy_body"].push_back(25 + 3.4f * t); b.armor["heavy_head"].push_back(15 + 1.1f * t); b.armor["heavy_hands"].push_back(10 + 1.1f * t); b.armor["heavy_feet"].push_back(10 + 1.1f * t);
            b.armor["heavy_shield"].push_back(20 + 2 * t); b.armor["light_body"].push_back(20 + 2.6f * t); b.armor["light_head"].push_back(10 + 0.9f * t); b.armor["light_hands"].push_back(5 + 0.9f * t);
            b.armor["light_feet"].push_back(5 + 0.9f * t); b.armor["light_shield"].push_back(15 + 1.5f * t);
        }
        for (float m : { 25.0f, 50.0f, 75.0f, 100.0f, 150.0f }) for (auto& pt : b.pots) pt.emplace_back(m, 0.0f);
        b.pAttack["6"] = { {0, 1.2f}, {20, 1.4f}, {40, 1.6f}, {60, 1.8f}, {80, 2.0f} }; b.pAttack["7"] = b.pAttack["6"]; b.pAttack["8"] = b.pAttack["6"];
        for (const char* sc : { "Destruction", "Restoration", "Alteration", "Illusion", "Conjuration" }) b.pSpellCost[sc] = { {0, 0.5f}, {25, 0.5f}, {50, 0.5f}, {75, 0.5f}, {100, 0.5f} };
        using N = Prune::Nature;
        SimSpell flames = Fx("Flames", N::Offense, kAvDestruction, kAvResistFire, 28, 16, 0, 0, 2.0f); flames.e.concentration = true; flames.e.reach = 500;
        SimSpell fb = Fx("Firebolt", N::Offense, kAvDestruction, kAvResistFire, 41, 25, 0, 0, 0.5f, 25); fb.dot = 2.5f; fb.dotDur = 4;
        SimSpell is = Fx("IceSpike", N::Offense, kAvDestruction, kAvResistFrost, 48, 25, 0, 0, 0.5f, 25); is.spDmg = 25;
        SimSpell lb = Fx("LightningBolt", N::Offense, kAvDestruction, kAvResistShock, 51, 25, 0, 0, 0.5f, 25); lb.mpDmg = 12;
        SimSpell fball = Fx("Fireball", N::Offense, kAvDestruction, kAvResistFire, 133, 40, 0, 15, 0.5f, 50); fball.dot = 4; fball.dotDur = 4;
        SimSpell heal = Fx("Healing", N::Heal, kAvRestoration, -1, 24, 0, 20, 0, 2.0f); heal.e.concentration = true; heal.e.selfDelivery = true; heal.e.reach = 0;
        SimSpell fheal = Fx("FastHealing", N::Heal, kAvRestoration, -1, 73, 0, 50, 0, 0.5f, 25); fheal.e.selfDelivery = true; fheal.e.reach = 0;
        SimSpell hh = Fx("HealOther", N::Heal, kAvRestoration, -1, 80, 0, 75, 0, 0.5f, 50); hh.healOther = true;
        SimSpell oak = Fx("Oakflesh", N::Buff, kAvAlteration, -1, 103, 0, 0, 0); oak.armor = 40; oak.buffDur = 60; oak.e.meleeBuff = true;
        SimSpell fear = Fx("Fear", N::Control, kAvIllusion, -1, 156, 0, 0, 0, 0.5f, 25); fear.ctrlFlee = true; fear.ctrlDur = 30; fear.ctrlMaxLevel = 9;
        SimSpell para = Fx("Paralyze", N::Control, kAvAlteration, -1, 450, 0, 0, 0, 0.5f, 75); para.ctrlPar = true; para.ctrlDur = 10;
        SimSpell atro = Fx("ConjureFlameAtronach", N::Summon, kAvConjuration, -1, 150, 0, 0, 0, 0.5f, 25);
        for (SimSpell* s : { &flames, &fb, &is, &lb, &fball, &heal, &fheal, &hh, &oak, &fear, &para, &atro }) { ++b.rawSpells; ++b.taughtSpells; AddPoolSpell(b, SimSpell(*s)); }
        b.foeSpells = { fb, is, lb, hh, flames, fball }; for (auto& s : b.foeSpells) s.e.cost = s.cost0;
        SimSpell breath = Fx("FireBreath", N::Offense, -1, kAvResistFire, 0, 60, 0, 12, 0.8f); breath.recovery = 8; breath.dot = 6; breath.dotDur = 5; b.foeSpells.push_back(breath);
        SimSpell ice = Fx("IceStorm", N::Offense, kAvDestruction, kAvResistFrost, 0, 20, 0, 0, 0.6f); ice.recovery = 4; ice.spDmg = 20; b.foeSpells.push_back(ice);
        // the roster: {name, race, level, hp, mp, sp, weapon dmg/speed/anim, skill, AR, flags}
        auto mk = [&](const char* nm, const char* race, int lvl, float hp, float mp, float sp, Weapon m, Weapon r, float skill, float ar, int pieces) {
            ActorSpec a; a.name = nm; a.race = race; a.level = lvl; a.hp = hp; a.mp = mp; a.sp = sp; a.melee = std::move(m); a.ranged = std::move(r); a.skill.fill(skill); a.ar = ar; a.pieces = pieces;
            a.hpR = 0; a.ammoDmg = a.ranged.Has() ? 8.0f : 0.0f; a.hasStyle = true; a.conf = 3; return a; };
        const Weapon none;
        for (int t = 0; t < 6; ++t) {   // one tier per level bucket
            const int L = b.buckets[static_cast<std::size_t>(t)]; const float l = static_cast<float>(L);
            const float sk = std::min(100.0f, 20 + 1.5f * l);
            ActorSpec bandit = mk("FixBandit", "NordRace", L, 35 + 18 * l, 25, 70 + 5 * l, wp("sword", 7 + 0.2f * l, 1, 1, kAvOneHanded, 0.75f, 10), none, sk, 20 + 4 * l, 3); bandit.name = "Bandit";
            ActorSpec archer = mk("BanditMissile", "NordRace", L, 35 + 14 * l, 25, 70 + 5 * l, wp("dagger", 4 + 0.1f * l, 1.3f, 2, kAvOneHanded, 0, 2), wp("bow", 7 + 0.25f * l, 1, 7, kAvMarksman, 0, 6), sk, 15 + 3 * l, 3); archer.st.wRanged = 2;
            ActorSpec mage = mk("BanditMage", "NordRace", L, 35 + 10 * l, 60 + 12 * l, 50, wp("dagger", 4, 1.3f, 2, kAvOneHanded, 0, 2), none, sk, 0, 0); mage.spells = { 0, 1 }; mage.st.wMagic = 3;
            ActorSpec healer = mk("FixHealer", "NordRace", L, 35 + 10 * l, 80 + 12 * l, 50, wp("dagger", 4, 1.3f, 2, kAvOneHanded, 0, 2), none, sk, 0, 0); healer.spells = { 3, 2 }; healer.st.wMagic = 3;
            ActorSpec draugr = mk("FixDraugr", "DraugrRace", L, 50 + 20 * l, 0, 150, wp("ancient sword", 8 + 0.3f * l, 1, 1, kAvOneHanded, 0.75f, 12), none, sk, 10 + 3 * l, 0); draugr.undead = true; draugr.creature = true; draugr.conf = 4; draugr.rFrost = 50; draugr.rPoison = 100; draugr.run = 345;
            ActorSpec wolf = mk("FixWolf", "WolfRace", L, 12 + 10 * l, 0, 100, none, none, sk, 0, 0); wolf.unarmed = 5 + 0.8f * l; wolf.animal = wolf.creature = true; wolf.run = 556;
            ActorSpec troll = mk("FixTroll", "TrollRace", L, 250 + 10 * l, 0, 250, none, none, sk, 30, 0); troll.unarmed = 20 + 1.5f * l; troll.creature = true; troll.rFire = -50; troll.hpR = 2; troll.mass = 2.5f; troll.run = 400;
            ActorSpec fatro = mk("FixFrostAtronach", "AtronachFrostRace", L, 150 + 10 * l, 100, 200, none, none, sk, 60, 0); fatro.unarmed = 20 + 1.5f * l; fatro.creature = fatro.daedra = true; fatro.rFrost = 100; fatro.rFire = -33; fatro.immunePar = true; fatro.mass = 3; fatro.run = 325;
            ActorSpec dragon = mk("FixDragon", "DragonRace", L, 800 + 60 * l, 150, 200, none, none, sk, 100, 0); dragon.unarmed = 40 + 2 * l; dragon.creature = dragon.dragon = true; dragon.rFire = 50; dragon.rFrost = -25; dragon.immunePar = true; dragon.mass = 8; dragon.spells = { 6 }; dragon.st.wShout = 2; dragon.run = 384;
            ActorSpec boss = mk("BanditChief", "NordRace", L + 8, 150 + 30 * l, 50, 150 + 6 * l, wp("battleaxe", 18 + 0.5f * l, 0.7f, 6, kAvTwoHanded, 1.15f, 22), none, std::min(100.0f, sk + 15), 60 + 6 * l, 4); boss.conf = 4;
            ActorSpec* all[] = { &bandit, &archer, &mage, &healer, &draugr, &wolf, &troll, &fatro, &dragon, &boss };
            for (ActorSpec* a : all) b.specs.push_back(*a);
        }
        // encounters: a few fixture groups per bucket (indices into the tier's 10 specs)
        const std::vector<std::vector<int>> groups = { {0, 0, 1}, {0, 2}, {4, 4, 4}, {5, 5, 5, 5}, {6}, {0, 1, 2, 3}, {4, 4, 0}, {9, 0, 1}, {7}, {0, 0}, {0}, {1, 1, 0}, {5, 5}, {2, 3, 0} };
        for (const auto& g : groups) { Encounter en; en.cell = "fixture"; for (int t = 0; t < 6; ++t) { std::vector<EncMember> row; int k = 0; for (int gi : g) { row.push_back({ t * 10 + gi, static_cast<float>(-150 + 150 * (k % 3)), static_cast<float>(-200 + 200 * (k / 3)) }); ++k; } en.byBucket.push_back(row); } b.encounters.push_back(en); }
        TagRoles(b);
        return b;
    }

    // ── a follower of one class at one level ────────────────────────────────────────────────────────────
    enum class Cls { Melee = 0, Ranged = 1, Mage = 2 };

    struct KitChoice {         // what the class row lets him carry (from the real class table: SimClass)
        bool oneHand = true, twoHand = false, dagger = false, bow = false, crossbow = false, staff = false, shield = false, heavy = true, light = false;
        float mageBlend = 0;   // hybrids: this share of his class weights is the mage row's (MODEL)
    };

    struct FollowerBuild {
        ActorSpec spec;
        Book book;
        std::vector<SimSpell> spells;   // parallel to book.entries
        float potH = 100, potM = 100, potS = 100;
        int   staffIdx = -1;
    };

    template <class T> const T& LadderAt(const std::vector<T>& v, int level, float top = 0.75f) {
        const float q = std::clamp(static_cast<float>(level - 1) / 45.0f, 0.0f, 1.0f) * top;
        return v[static_cast<std::size_t>(std::lround(q * static_cast<float>(v.size() - 1)))];
    }

    // Does a pool spell answer a scenario's "he must know one of these" token?
    inline bool SpellIs(const SimSpell& s, const std::string& tok) {
        const bool off = s.e.nature == Prune::Nature::Offense;
        if (tok == "area") return off && s.e.areaUnits > 0 && !s.e.selfDelivery; if (tok == "paralyze") return s.ctrlPar; if (tok == "fear") return s.ctrlFlee && !s.turnUndead;
        if (tok == "heal") return s.e.nature == Prune::Nature::Heal && s.e.selfDelivery; if (tok == "healOther") return s.healOther;
        if (tok == "fire" || tok == "frost" || tok == "shock") return off && s.element == tok && s.e.areaUnits <= 0 && !s.e.touch && !s.e.selfDelivery;
        if (tok == "armor") return s.armor > 0; if (tok == "summon") return s.summon;
        return false;
    }

    inline FollowerBuild BuildFollower(const Data& D, Cls fam, const KitChoice& kc, const ClassGate& gate, int level, Rng& R, bool withKit = true, const std::vector<std::string>* ensure = nullptr) {
        FollowerBuild fb; ActorSpec& a = fb.spec; const Rules& r = D.rules;
        const FRace& rc = D.races[static_cast<std::size_t>(R.I(0, static_cast<int>(D.races.size()) - 1))]; FClass c = D.cls[static_cast<int>(fam)];
        if (kc.mageBlend > 0) { const FClass& m = D.cls[2]; auto norm = [](const FClass& k) { float s = 0; for (float w : k.skillW) s += w; return std::max(1.0f, s); };
            const float ns = norm(c), nm = norm(m), th = std::max(1.0f, c.hW + c.mW + c.sW), tm = std::max(1.0f, m.hW + m.mW + m.sW), q = kc.mageBlend;
            for (std::size_t i = 0; i < 18; ++i) c.skillW[i] = (1 - q) * c.skillW[i] / ns + q * m.skillW[i] / nm;
            c.hW = (1 - q) * c.hW / th + q * m.hW / tm; c.mW = (1 - q) * c.mW / th + q * m.mW / tm; c.sW = (1 - q) * c.sW / th + q * m.sW / tm; }
        a.name = "Follower"; a.race = rc.edid; a.level = level; const float L1 = static_cast<float>(level - 1), tot = std::max(1.0f, c.hW + c.mW + c.sW);
        a.hp = rc.hp + L1 * (r.hpPerLevel + r.hmsPerLevel * c.hW / tot); a.mp = rc.mp + L1 * r.hmsPerLevel * c.mW / tot; a.sp = rc.sp + L1 * r.hmsPerLevel * c.sW / tot;
        a.hpR = rc.hpR; a.mpR = rc.mpR; a.spR = rc.spR; a.unarmed = rc.unarmed; a.run = rc.run > 40 ? rc.run : 370; a.mass = rc.mass > 0 ? rc.mass : 1;
        float wsum = 0; for (float w : c.skillW) wsum += w; wsum = std::max(1.0f, wsum);
        for (std::size_t i = 0; i < 18; ++i) { float boost = 0; for (const auto& bo : rc.boosts) if (bo.first == static_cast<int>(i) + 6) boost = static_cast<float>(bo.second);
            a.skill[i] = std::min(100.0f, r.skillStart + boost + L1 * r.skillPerLevel * c.skillW[i] / wsum); }
        Kit& k = fb.book.kit;
        if (withKit) {
            if (kc.twoHand && !kc.oneHand && !D.wTwoHand.empty()) { a.melee = LadderAt(D.wTwoHand, level); k.melee = true; }
            else if (kc.oneHand && !D.wOneHand.empty()) { a.melee = LadderAt(D.wOneHand, level); k.melee = true; }
            else if (kc.dagger && !D.wDagger.empty()) { a.melee = LadderAt(D.wDagger, level); k.melee = true; }
            if (kc.bow && !D.wBow.empty()) { a.ranged = LadderAt(D.wBow, level); k.bow = true; k.arrows = R.I(20, 80); if (!D.arrows.empty()) a.ammoDmg = LadderAt(D.arrows, level); }
            else if (kc.crossbow && !D.wCrossbow.empty()) { a.ranged = LadderAt(D.wCrossbow, level); k.crossbow = true; k.bolts = R.I(20, 60); if (!D.bolts.empty()) a.ammoDmg = LadderAt(D.bolts, level); }
            a.shield = kc.shield && !a.melee.TwoH() && a.melee.Has();
            if (kc.heavy || kc.light) {
                const char* t = kc.heavy ? "heavy" : "light"; const int av = kc.heavy ? kAvHeavy : kAvLight; float base = 0; int n = 0;
                for (const char* s : { "_body", "_head", "_hands", "_feet" }) { auto i = D.armor.find(std::string(t) + s); if (i != D.armor.end() && !i->second.empty()) { base += LadderAt(i->second, level); ++n; } }
                if (a.shield) { auto i = D.armor.find(std::string(t) + "_shield"); if (i != D.armor.end() && !i->second.empty()) { base += LadderAt(i->second, level); ++n; } }
                a.ar = base * (r.armorSkillLo + (r.armorSkillHi - r.armorSkillLo) * a.Skill(av) / 100.0f) * PerkBest(D.pArmor, std::to_string(av), a.Skill(av)); a.pieces = n;
            }
        }
        if (a.melee.Has()) { a.atkMult = PerkBest(D.pAttack, std::to_string(a.melee.skill), a.Skill(a.melee.skill)); a.powMult = PerkBest(D.pPower, std::to_string(a.melee.skill), a.Skill(a.melee.skill)); }
        a.hasStyle = true; a.st.wMagic = fam == Cls::Mage ? 3.0f : 0.5f; a.st.wRanged = fam == Cls::Ranged ? 3.0f : 1.0f; a.conf = 3;
        k.potionsHealth = R.I(0, 4); k.potionsStamina = R.I(0, 2); k.potionsMagicka = R.I(0, 3);
        for (int i = 0; i < 3; ++i) if (!D.pots[i].empty()) (i == 0 ? fb.potH : i == 1 ? fb.potM : fb.potS) = LadderAt(D.pots[i], level, 0.9f).first;
        // spells: what he could plausibly have learnt (tier within reach of his skill, affordable), as sim v1 chose them
        auto costOf = [&](const SimSpell& s) { const float sk = s.school >= 0 ? a.Skill(s.school) : 15.0f;
            float m = SkillCostMult(r, sk);
            if (s.school >= 18 && s.school <= 22) { static const char* sn[5] = { "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration" };
                auto i = D.pSpellCost.find(sn[s.school - 18]); if (i != D.pSpellCost.end()) for (const auto& p : i->second) if (p.first <= sk && std::fabs(p.first - static_cast<float>(s.skillReq)) < 1.0f) { m *= p.second; break; } }
            return s.cost0 * m; };
        const bool casts = gate.spellsOnClass;
        std::vector<const SimSpell*> afford;
        for (const SimSpell& s : D.spells) if (static_cast<float>(s.skillReq) <= (s.school >= 0 ? a.Skill(s.school) : 15.0f) + 10 && costOf(s) <= a.mp * 0.7f) afford.push_back(&s);
        if (afford.empty()) for (const SimSpell& s : D.spells) afford.push_back(&s);
        if (fam == Cls::Mage && casts) {
            for (auto nat : { Prune::Nature::Offense, Prune::Nature::Heal, Prune::Nature::Buff, Prune::Nature::Control, Prune::Nature::Ward }) {   // a ward too when the list has one (card I6)
                std::vector<const SimSpell*> cnd; for (auto* s : afford) if (s->e.nature == nat) cnd.push_back(s);
                if (!cnd.empty()) fb.spells.push_back(*cnd[static_cast<std::size_t>(R.I(0, static_cast<int>(cnd.size()) - 1))]);
            }
            for (int i = R.I(2, 8); i > 0 && !afford.empty(); --i) fb.spells.push_back(*afford[static_cast<std::size_t>(R.I(0, static_cast<int>(afford.size()) - 1))]);
        } else if (casts && !afford.empty()) {
            for (int i = R.I(2, 5); i > 0; --i) fb.spells.push_back(*afford[static_cast<std::size_t>(R.I(0, static_cast<int>(afford.size()) - 1))]);
        } else if (!D.spells.empty() && R.I(0, 2) == 0) {   // a non-caster sometimes knows a heal spell the gate must drop
            for (const SimSpell& s : D.spells) if (s.e.nature == Prune::Nature::Heal) { fb.spells.push_back(s); break; }
        }
        if (ensure) for (const std::string& tok : *ensure) {   // a scenario needs him to know one: the dearest he can afford twice, else the cheapest
            bool have = false; for (const SimSpell& s : fb.spells) have |= SpellIs(s, tok); if (have) continue;
            const SimSpell* pick = nullptr; const SimSpell* cheap = nullptr;
            for (const SimSpell& s : D.spells) { if (!SpellIs(s, tok)) continue; const float c0 = costOf(s); if (!cheap || c0 < costOf(*cheap)) cheap = &s; if (c0 <= a.mp * 0.45f && (!pick || c0 > costOf(*pick))) pick = &s; }
            if (!pick) pick = cheap; if (pick) fb.spells.push_back(*pick);
        }
        if (kc.staff && withKit && R.I(0, 1) == 0) {   // a staff: an entry with no magicka cost and charges
            SimSpell st; st.e.nature = Prune::Nature::Offense; st.e.role = RoleTag::Fire; st.e.resistAv = kAvResistFire; st.e.source = SpellSource::Staff; st.e.reach = 4096; st.element = "fire";
            st.dmg = 15.0f + static_cast<float>(level); st.castSec = 0.6f; st.e.name = "Staff"; st.e.charge = static_cast<float>(R.I(4, 14)); st.e.charged = true; st.e.count = 1; st.school = -1;
            fb.staffIdx = static_cast<int>(fb.spells.size()); fb.spells.push_back(st); k.staff = true; k.staffCharged = true; a.staff = true;
        }
        for (std::size_t i = 0; i < fb.spells.size(); ++i) {
            SimSpell& s = fb.spells[i]; s.e.form = 0x1000u + static_cast<std::uint32_t>(i); s.e.onClass = gate.spellsOnClass;
            if (static_cast<int>(i) != fb.staffIdx) {
                s.e.cost = costOf(s);
                static const char* sn[5] = { "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration" };
                if (s.school >= 18 && s.school <= 22) {   // ModSpellMagnitude perks of his school (and element)
                    const float sk = a.Skill(s.school); const std::string sc = sn[s.school - 18];
                    const std::string el = s.element == "fire" ? "MagicDamageFire" : s.element == "frost" ? "MagicDamageFrost" : s.element == "shock" ? "MagicDamageShock" : "";
                    const float m = PerkBest(D.pSpellMag, sc + ":all", sk) * (el.empty() ? 1.0f : PerkBest(D.pSpellMag, sc + ":" + el, sk));
                    s.dmg *= m; s.dot *= m; s.heal *= m; s.hot *= m;
                }
            }
            fb.book.entries.push_back(s.e);
        }
        return fb;
    }

}
