// iaus_sim/sim.cpp -- OFFLINE IAUS combat simulator (card I2s). HOST-ONLY, never part of the DLL.
// Drives the REAL pure layers (iaus/Core.h: Decide / DecidePosture / OnFired; iaus/Generate.h: Generate) with
// randomised fights built as plain Snapshots. The world model (damage, regen, potions, deaths, movement) is a
// CRUDE but plausible stand-in whose rates come from the list JSON's GMSTs / races / spells / potions
// (tools/list_balance/extract.py). List data is TEST INPUT only; nothing in native/ reads it.
#include "iaus/Core.h"
#include "iaus/Generate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace MFO::Iaus;

// ── tiny JSON reader (objects, arrays, numbers, strings, true/false/null) ───────────────────────────────
struct J {
    enum T { Nul, Num, Str, Arr, Obj, Bool } t = Nul;
    double n = 0; std::string s; std::vector<J> a; std::map<std::string, J> o;
    const J& operator[](const char* k) const { static const J nul; auto i = o.find(k); return i == o.end() ? nul : i->second; }
    double num(double d = 0) const { return t == Num || t == Bool ? n : d; }
};
struct JP {
    const std::string& s; size_t p = 0;
    void ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    J val() {
        ws(); J j;
        if (s[p] == '{') { j.t = J::Obj; ++p; ws(); if (s[p] == '}') { ++p; return j; }
            for (;;) { ws(); std::string k = str(); ws(); ++p; j.o[k] = val(); ws(); if (s[p++] == '}') break; } }
        else if (s[p] == '[') { j.t = J::Arr; ++p; ws(); if (s[p] == ']') { ++p; return j; }
            for (;;) { j.a.push_back(val()); ws(); if (s[p++] == ']') break; } }
        else if (s[p] == '"') { j.t = J::Str; j.s = str(); }
        else if (!s.compare(p, 4, "true")) { j.t = J::Bool; j.n = 1; p += 4; }
        else if (!s.compare(p, 5, "false")) { j.t = J::Bool; p += 5; }
        else if (!s.compare(p, 4, "null")) { p += 4; }
        else { j.t = J::Num; char* e; j.n = std::strtod(s.c_str() + p, &e); p = size_t(e - s.c_str()); }
        return j;
    }
    std::string str() { std::string r; ++p; while (s[p] != '"') { if (s[p] == '\\') ++p; r += s[p++]; } ++p; return r; }
};

// ── the list balance (from JSON, or built-in vanilla-like defaults) ─────────────────────────────────────
struct SimSpell {
    SpellEntry e; float dmg = 0, heal = 0, dur = 0, castSec = 1.0f, cost0 = 0; bool ctrl = false, ctrlPar = false; float ctrlDur = 0; int skillReq = 0;
};
struct Balance {
    std::string name = "builtin";
    std::map<std::string, double> gmst;
    double G(const char* k, double d) const { auto i = gmst.find(k); return i == gmst.end() ? d : i->second; }
    struct Race { double hp, mp, sp, hpR, mpR, spR; };
    std::vector<Race> playable, hostile;
    std::vector<SimSpell> spells;            // castable pool (taught by a BOOK when the JSON has the flag)
    float potHealth = 100, potMagicka = 100, potStamina = 100;
    int natures[7] = {};                     // classified spells per Prune::Nature (index 0 = Unknown, dropped)
    int rawSpells = 0, taughtSpells = 0;
};

static bool Hostile(const J& e) { return e["hostile"].num() != 0; }
static constexpr int kAvHealth = 24, kAvWardPower = 63;

// Mirrors Prune::ClassifyNature's precedence (reanimate, summon, ward, control, self cloak, hostile = Offense, heal,
// script-costliest / undeclared = Unknown, else Buff). ward = a beneficial ValueMod on kWardPower (APPROXIMATION of
// Actuation::SpellShape::Ward); Offense = any hostile effect (CasterConsent kind Offense, APPROXIMATION).
static void Classify(const J& sp, SimSpell& out) {
    const auto& fx = sp["effects"].a; SpellEntry& e = out.e;
    bool summon = false, reanimate = false, ward = false, control = false, cloak = false, hostileAny = false, heal = false;
    double dmg = 0, healSum = 0, dur = 0, autoCost = 0, topCost = -1, topHostCost = -1; int topArch = -1, resist = -1, skillReq = 0;
    const bool self = sp["delivery"].s == "Self";
    for (const J& x : fx) {
        const int av = int(x["av"].num(-1)), arch = int(x["archetype"].num()); const double mag = x["mag"].num(), d = x["dur"].num();
        const double tot = mag * (d > 0 ? std::min(d, 30.0) : 1.0);
        const double ec = x["baseCost"].num() * std::pow(std::max(mag, 1.0) * std::max(d, 1.0), 1.1);
        autoCost += ec;
        if (ec > topCost) { topCost = ec; topArch = arch; skillReq = int(x["minSkill"].num()); }
        const bool host = Hostile(x), detr = x["detrimental"].num() != 0;
        if (arch == 18) summon = true;
        if (arch == 22) reanimate = true;
        if (arch == 6 || arch == 7 || arch == 8 || arch == 21 || arch == 24) {
            control = true;
            if (arch == 21) out.ctrlPar = true;
            out.ctrlDur = std::max(out.ctrlDur, float(d));
        }
        if (arch == 35 && self) cloak = true;
        if (host || detr) { if (host) hostileAny = true; if (ec > topHostCost) { topHostCost = ec; resist = int(x["resist"].num(-1)); } }
        if (host && (arch == 0 || arch == 34 || arch == 4) && av == kAvHealth) dmg += tot;
        if (!host && !detr && av == kAvWardPower && (arch == 0 || arch == 34)) ward = true;
        if (!host && !detr && av == kAvHealth && (arch == 0 || arch == 34)) { heal = true; healSum += tot; }
        e.areaUnits = std::max(e.areaUnits, Prune::BlastUnits(float(x["area"].num()), 0.0f));
        dur = std::max(dur, d);
    }
    if (reanimate) e.drop = Prune::Drop::UnknownNature;
    else if (summon) e.nature = Prune::Nature::Summon;
    else if (ward) e.nature = Prune::Nature::Ward;
    else if (control) e.nature = Prune::Nature::Control;
    else if (cloak) e.nature = Prune::Nature::Buff;
    else if (hostileAny) e.nature = Prune::Nature::Offense;
    else if (heal) e.nature = Prune::Nature::Heal;
    else if (topArch == 1 || fx.empty()) e.drop = Prune::Drop::UnknownNature;
    else e.nature = Prune::Nature::Buff;
    out.ctrl = e.nature == Prune::Nature::Control;
    e.resistAv = (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control) ? resist : -1;
    e.role = Prune::RoleOf(e.nature, e.resistAv);
    const std::string& dl = sp["delivery"].s;
    e.selfDelivery = dl == "Self"; e.touch = dl == "Touch"; e.concentration = sp["castType"].s == "Concentration";
    e.reach = (e.selfDelivery || e.touch) ? 0.0f : float(sp["range"].num()) > 0 ? float(sp["range"].num()) : 4096.0f;
    if (dl == "TargetLocation") e.drop = Prune::Drop::Placement;
    if (sp["castType"].s == "ConstantEffect") e.drop = Prune::Drop::NotCastable;
    // cost: the record's, except an auto-calculate spell (SPIT flag 0x1) whose stored cost is not authoritative: effect costs summed.
    out.cost0 = sp["autoCalc"].num() != 0 ? float(autoCost) : float(sp["cost"].num());
    e.cost = out.cost0; out.skillReq = skillReq;
    out.dmg = float(dmg); out.heal = float(healSum); out.dur = float(dur);
    out.castSec = e.concentration ? 2.0f : std::max(0.6f, float(sp["chargeTime"].num(0.0)));
    e.name = sp["edid"].s;
}

static Balance LoadBalance(const char* path) {
    Balance b;
    if (!path) {   // built-in vanilla-like
        b.playable = { {100, 100, 100, 0.7, 3, 5} }; b.hostile = { {50, 0, 100, 0.0, 3, 5}, {150, 50, 100, 0.0, 3, 5}, {300, 0, 150, 0, 0, 5} };
        struct D { const char* n; Prune::Nature na; RoleTag r; int rs; float c, d, h, area; };
        const D d[] = { {"Firebolt", Prune::Nature::Offense, RoleTag::Fire, 41, 25, 25, 0, 0}, {"IceSpike", Prune::Nature::Offense, RoleTag::Frost, 43, 32, 40, 0, 0},
                        {"Sparks", Prune::Nature::Offense, RoleTag::Shock, 42, 8, 8, 0, 0}, {"Fireball", Prune::Nature::Offense, RoleTag::Fire, 41, 150, 90, 0, 15},
                        {"Healing", Prune::Nature::Heal, RoleTag::Heal, -1, 40, 0, 80, 0}, {"FastHealing", Prune::Nature::Heal, RoleTag::Heal, -1, 100, 0, 200, 0},
                        {"OakFlesh", Prune::Nature::Buff, RoleTag::Buff, -1, 100, 0, 0, 0}, {"Fear", Prune::Nature::Control, RoleTag::Control, -1, 70, 0, 0, 0},
                        {"Paralyze", Prune::Nature::Control, RoleTag::Control, -1, 120, 0, 0, 0},
                        {"ConjureFlameAtronach", Prune::Nature::Summon, RoleTag::Summon, -1, 150, 0, 0, 0} };
        for (const D& x : d) { SimSpell s; s.e.nature = x.na; s.e.role = x.r; s.e.resistAv = x.rs; s.e.cost = x.c; s.cost0 = x.c; s.e.name = x.n;
            s.e.selfDelivery = x.na == Prune::Nature::Buff || x.na == Prune::Nature::Summon; s.e.reach = s.e.selfDelivery ? 0 : 4096; s.dmg = x.d; s.heal = x.h; s.dur = 60;
            s.e.areaUnits = Prune::BlastUnits(x.area, 0.0f); s.ctrl = x.na == Prune::Nature::Control; s.ctrlDur = 10; s.ctrlPar = std::string(x.n) == "Paralyze";
            ++b.natures[int(x.na)]; b.spells.push_back(s); }
        b.rawSpells = b.taughtSpells = int(b.spells.size());
        return b;
    }
    std::ifstream f(path); if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    std::stringstream ss; ss << f.rdbuf(); const std::string txt = ss.str(); JP p{ txt }; const J root = p.val();
    b.name = root["list"].s;
    for (auto& kv : root["gmst"].o) b.gmst[kv.first] = kv.second.num();
    for (const J& r : root["races"].a) {
        Balance::Race x{ r["hp"].num(), r["mp"].num(), r["sp"].num(), r["hpRegen"].num(), r["mpRegen"].num(), r["spRegen"].num() };
        (r["playable"].num() ? b.playable : b.hostile).push_back(x);
    }
    if (b.playable.empty()) b.playable.push_back({ 100, 100, 100, 0.7, 3, 5 });
    if (b.hostile.empty()) b.hostile = b.playable;
    bool anyTaught = false; for (const J& s : root["spells"].a) if (s["taught"].num() != 0) { anyTaught = true; break; }
    for (const J& s : root["spells"].a) {
        ++b.rawSpells; if (anyTaught && s["taught"].num() == 0) continue; ++b.taughtSpells;
        SimSpell ss2; Classify(s, ss2);
        if (ss2.e.drop == Prune::Drop::Keep) { ++b.natures[int(ss2.e.nature)]; b.spells.push_back(std::move(ss2)); } else ++b.natures[0]; }
    double best[3] = { 0, 0, 0 };   // largest SINGLE-effect restore with no side effect
    for (const J& p2 : root["potions"].a) {
        const auto& fx = p2["effects"].a; if (fx.size() != 1) continue;
        const int av = int(fx[0]["av"].num(-1)); const double m = fx[0]["mag"].num(); if (av < 24 || av > 26 || Hostile(fx[0]) || fx[0]["detrimental"].num() != 0) continue;
        const int ix = av - 24; if (m > best[ix] && m <= 1000) { best[ix] = m; }
    }
    if (best[0] > 0) b.potHealth = float(best[0]);
    if (best[1] > 0) b.potMagicka = float(best[1]);
    if (best[2] > 0) b.potStamina = float(best[2]);
    return b;
}

// ── scenario ────────────────────────────────────────────────────────────────────────────────────────────
enum class Cls { Melee, Ranged, Mage };
struct ClsDef { const char* name; Cls fam; ClassGate gate; };
// Gates come from the real T1/T2 rows (ClassTable.cpp via SpellBook.cpp GateFor), written out by hand:
//  T1 (tree ON): weapons kUnrestricted, no taunt / power attack / disengage; Mage line casts. Knight: kMelee1H2H|kShield, taunt row,
//  powerShareMax set. Evoker: kMageSet = dagger+staff, kDisengage, casts.  Tree OFF = Q63 (EMULATED: GateFor's tree-OFF output still
//  derives the T1 row, so a melee T1 gets spellsOnClass=false and no power attack / taunt; the fix belongs to I6): nothing restricted.
static ClassGate MkGate(bool treeOn, bool spells, bool power, bool taunt, bool dis, bool melee, bool bow, bool xbow, bool staff) {
    ClassGate g; g.resolved = true; g.treeOn = treeOn; g.strict = false; g.spellsOnClass = spells; g.scrollUser = spells; g.powerAttack = power;
    g.powerCooldownSec = 0.0f; g.taunt = taunt; g.disengage = dis; g.weaponMelee = melee; g.weaponBow = bow; g.weaponCrossbow = xbow; g.weaponStaff = staff; return g; }
static const ClsDef kClasses[] = {
    { "melee",     Cls::Melee,  MkGate(true,  false, false, false, false, true, true, true, true) },
    { "ranged",    Cls::Ranged, MkGate(true,  false, false, false, false, true, true, true, true) },
    { "mage",      Cls::Mage,   MkGate(true,  true,  false, false, false, true, true, true, true) },
    { "melee-off", Cls::Melee,  MkGate(false, true,  true,  true,  true,  true, true, true, true) },
    { "ranged-off",Cls::Ranged, MkGate(false, true,  true,  true,  true,  true, true, true, true) },
    { "mage-off",  Cls::Mage,   MkGate(false, true,  true,  true,  true,  true, true, true, true) },
    { "knight-T2", Cls::Melee,  MkGate(true,  false, true,  true,  false, true, false, false, false) },
    { "evoker-T2", Cls::Mage,   MkGate(true,  true,  false, false, true,  true, false, false, true) } };
static constexpr int kNC = int(sizeof(kClasses) / sizeof(kClasses[0]));

struct Bracket { const char* name; float ratio; bool boss; int minFoes, maxFoes; };
static const Bracket kBrackets[] = {
    { "balanced", 1.0f, false, 1, 5 }, { "weak", 0.45f, false, 2, 8 }, { "strong1", 1.6f, false, 1, 4 },
    { "strong2", 2.4f, false, 1, 3 }, { "mixed-boss", 0.8f, true, 3, 6 } };
static constexpr int kNB = 5;

struct Actor {
    Combatant c; double hpR = 0, mpR = 0, spR = 0; double nextAtk = 0, stunUntil = 0, tauntUntil = 0, retarget = 0; bool alive = true; bool melee = true; float dmgBase = 10;
};

struct Stats {
    long ticks = 0, idle = 0, hold = 0, act = 0, fired = 0, switches = 0, badFacts = 0, invalid = 0, postureSwaps = 0, fights = 0, deaths = 0, wins = 0;
    double simSec = 0; long hist[kFamilies] = {};
    long lowEpisodes = 0, lowCandEpisodes = 0, lowHealed = 0; double lowHealSecSum = 0;
    long lowCandTicks = 0, lowHoldTicks = 0;
    double dSum = 0, dMax = 0; long dN = 0;
    long noCandTicks = 0, candTicks = 0, idleCand = 0, reachTicks = 0, idleReach = 0, taunts = 0, tauntRedundant = 0, tauntFail = 0;
};

struct Rng {
    std::mt19937_64 g;
    float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); }
    int I(int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); }
};

static constexpr double kDt = 0.133;
static bool g_tauntVisible = false;   // --taunt-visible: show Combatant::taunted to the generator (the game does NOT: R1)

static void Place(Combatant& c, float x, float y) { c.x = x; c.y = y; }
static float Dist(const Combatant& a, const Combatant& b) { return std::hypot(a.x - b.x, a.y - b.y); }
static void StepToward(Combatant& c, float tx, float ty, float amt) {
    const float dx = tx - c.x, dy = ty - c.y, d = std::hypot(dx, dy); if (d < 1e-3f) return; const float k = std::min(amt, d) / d; c.x += dx * k; c.y += dy * k; }

static void RunFight(const Balance& B, const ClsDef& cd, int rank, const Bracket& br, Rng& R, std::uint64_t seed, Stats& S) {
    const Cls cls = cd.fam;
    const int level = R.I(12, 45);
    const double skill = std::min(100.0, 20.0 + 1.6 * level);   // the follower's magic skill: scales spell cost and gates spell tiers
    auto pickRace = [&](const std::vector<Balance::Race>& v) { return v[size_t(R.I(0, int(v.size()) - 1))]; };
    auto mk = [&](std::uint32_t id, const char* nm, int lvl, const Balance::Race& rc, bool foe) {
        Actor a; a.c.id = id; a.c.name = nm; a.c.level = lvl;
        const double hp = rc.hp + 8.0 * lvl, mp = std::max(rc.mp, 0.0) + (foe ? 0 : 4.0 * lvl), sp = std::min(rc.sp, 400.0) + 2.0 * lvl;
        a.c.hpMax = float(hp); a.c.mpMax = float(mp); a.c.spMax = float(sp);
        a.c.hp = a.c.hpMax * R.U(0.4f, 1.0f); a.c.mp = a.c.mpMax * R.U(0.3f, 1.0f); a.c.sp = a.c.spMax * R.U(0.4f, 1.0f);
        a.hpR = rc.hpR; a.mpR = rc.mpR; a.spR = rc.spR; a.dmgBase = float(6 + 1.0 * lvl);
        return a;
    };
    const double combatHp = B.G("fCombatHealthRegenRateMult", 0.7), combatMp = B.G("fCombatMagickaRegenRateMult", 0.5), combatSp = B.G("fCombatStaminaRegenRateMult", 0.7);
    const double diffToPC = B.G("fDiffMultHPToPCN", 1.0), staCost = 10.0 + 40.0 * B.G("fStaminaAttackWeaponMult", 0.3);

    Actor self = mk(1, "Follower", level, pickRace(B.playable), false); self.c.hp = self.c.hpMax * R.U(0.5f, 1.0f);
    Actor player = mk(2, "Player", level, pickRace(B.playable), false); player.c.isPlayer = true; player.c.hp = player.c.hpMax;
    // geometry: the player at the origin, foes ahead (+x); the follower stands by his role; allies cluster near the player
    Place(player.c, 0, 0);
    Place(self.c, cls == Cls::Melee ? 250.0f : cls == Cls::Ranged ? -300.0f : -250.0f, R.U(-150, 150));
    std::vector<Actor> allies;
    for (int i = R.I(0, 3); i > 0; --i) { Actor a = mk(10u + std::uint32_t(allies.size()), "Ally", level, pickRace(B.playable), false); Place(a.c, R.U(-400, 100), R.U(-300, 300)); allies.push_back(std::move(a)); }
    std::vector<Actor> foes; const int nf = R.I(br.minFoes, br.maxFoes);
    const float baseAng = R.U(-0.5f, 0.5f);
    for (int i = 0; i < nf; ++i) {
        const float ratio = br.boss ? (i == 0 ? 2.2f : 0.7f) : br.ratio * R.U(0.85f, 1.15f);
        const int fl = std::max(1, int(level * ratio));
        Actor f = mk(100u + std::uint32_t(i), "Foe", fl, pickRace(B.hostile), true); f.c.hp = f.c.hpMax;
        const float d0 = R.U(300, 1800), ang = baseAng + R.U(-0.5f, 0.5f);
        Place(f.c, self.c.x + d0 * std::cos(ang), self.c.y + d0 * std::sin(ang));
        f.c.caster = R.I(0, 3) == 0; f.melee = !f.c.caster && R.I(0, 4) != 0; f.c.undead = R.I(0, 7) == 0;
        f.c.sight = R.I(0, 9) == 0 ? Sight::Occluded : Sight::Visible; f.c.sightEngine = Sight::Visible;
        for (int av : { 41, 42, 43 }) f.c.resists.push_back({ av, R.I(0, 6) == 0 ? R.U(40, 100) : R.U(0, 30) });
        f.nextAtk = R.U(0, 2); foes.push_back(std::move(f));
    }
    // class gate / kit / book (the gate is the class row's, see kClasses)
    const ClassGate gate = cd.gate;
    Book book; Kit& k = book.kit;
    k.melee = cls != Cls::Ranged; k.bow = cls == Cls::Ranged; k.arrows = cls == Cls::Ranged ? R.I(10, 80) : 0;
    k.potionsHealth = R.I(0, 4); k.potionsStamina = R.I(0, 2); k.potionsMagicka = R.I(0, 3);
    std::vector<SimSpell> mySp;
    const bool casts = gate.spellsOnClass;
    {
        std::vector<const SimSpell*> afford;
        for (const SimSpell& s : B.spells) if (s.skillReq <= skill + 10 && s.cost0 * (1.0 - 0.5 * skill / 100.0) <= self.c.mpMax * 0.7) afford.push_back(&s);
        if (afford.empty()) for (const SimSpell& s : B.spells) afford.push_back(&s);
        if (cls == Cls::Mage) {
            for (auto nat : { Prune::Nature::Offense, Prune::Nature::Heal, Prune::Nature::Buff, Prune::Nature::Control }) {
                std::vector<const SimSpell*> c; for (auto* s : afford) if (s->e.nature == nat) c.push_back(s);
                if (!c.empty()) mySp.push_back(*c[size_t(R.I(0, int(c.size()) - 1))]);
            }
            for (int i = R.I(2, 8); i > 0 && !afford.empty(); --i) mySp.push_back(*afford[size_t(R.I(0, int(afford.size()) - 1))]);
        } else if (casts && !afford.empty()) {   // tree OFF (Q63): a non-mage knows any spell
            for (int i = R.I(2, 5); i > 0; --i) mySp.push_back(*afford[size_t(R.I(0, int(afford.size()) - 1))]);
        } else if (!B.spells.empty() && R.I(0, 2) == 0) {   // tree ON non-caster sometimes knows a heal spell the gate must drop
            for (const SimSpell& s : B.spells) if (s.e.nature == Prune::Nature::Heal) { mySp.push_back(s); break; }
        }
    }
    int staffIdx = -1;
    if (cls == Cls::Mage && R.I(0, 1) == 0) {   // staff kit option: a staff entry (source Staff, no magicka cost, charges)
        SimSpell st; st.e.nature = Prune::Nature::Offense; st.e.role = RoleTag::Fire; st.e.resistAv = 41; st.e.source = SpellSource::Staff; st.e.cost = 0; st.e.reach = 4096;
        st.dmg = float(15 + level); st.castSec = 0.8f; st.e.name = "Staff"; st.e.charge = float(R.I(4, 14)); st.e.charged = true; st.e.count = 1;
        staffIdx = int(mySp.size()); mySp.push_back(st); k.staff = true; k.staffCharged = true;
    }
    for (size_t i = 0; i < mySp.size(); ++i) {
        mySp[i].e.form = 0x1000u + std::uint32_t(i); mySp[i].e.onClass = gate.spellsOnClass;
        if (int(i) != staffIdx) mySp[i].e.cost = float(mySp[i].cost0 * (1.0 - 0.5 * skill / 100.0));   // cost scaled by skill (cost 0 stays 0)
        book.entries.push_back(mySp[i].e);
    }
    std::vector<std::vector<std::pair<std::uint32_t, double>>> active(mySp.size());
    std::vector<double> summonUntil(mySp.size(), 0.0);

    Mind mind; PostureKind drawn = cls == Cls::Ranged ? PostureKind::Bow : cls == Cls::Mage ? PostureKind::None : PostureKind::Melee;
    double now = 0, busyUntil = 0, lowSince = -1; bool lowHealedFlag = false, lowHadCand = false; std::uint32_t curTarget = 0;
    const Tuning& T = DefaultTuning(); const Profile P;
    auto alive = [](const Actor& a) { return a.alive && a.c.hp > 0; };
    auto find = [&](std::uint32_t id) -> Actor* {
        if (id == 1) return &self;
        if (id == 2) return &player;
        for (auto& a : foes) if (a.c.id == id) return &a;
        for (auto& a : allies) if (a.c.id == id) return &a;
        return nullptr; };
    auto dmgTo = [&](const Actor& att, const Actor& def) {
        const double ratio = std::clamp(double(att.c.level) / std::max(1, def.c.level), 0.3, 3.0);
        double d = att.dmgBase * std::pow(ratio, 0.8); if (def.c.id == 1 || def.c.id == 2 || (def.c.id >= 10 && def.c.id < 100)) d *= diffToPC; return d; };
    auto refreshDist = [&]() { for (auto& f : foes) f.c.dist = Dist(f.c, self.c); for (auto& a : allies) a.c.dist = Dist(a.c, self.c); player.c.dist = Dist(player.c, self.c); };
    ++S.fights;
    const double kMax = 120.0;
    while (now < kMax) {
        auto regen = [&](Actor& a) {
            a.c.hp = std::min(a.c.hpMax, a.c.hp + float(a.c.hpMax * a.hpR / 100.0 * combatHp * kDt));
            a.c.mp = std::min(a.c.mpMax, a.c.mp + float(a.c.mpMax * a.mpR / 100.0 * combatMp * kDt));
            a.c.sp = std::min(a.c.spMax, a.c.sp + float(a.c.spMax * a.spR / 100.0 * combatSp * kDt)); };
        regen(self); regen(player); for (auto& a : allies) regen(a);
        std::vector<Actor*> liveFoes; for (auto& f : foes) if (alive(f)) liveFoes.push_back(&f);
        if (liveFoes.empty()) { ++S.wins; break; }
        if (!alive(self)) { ++S.deaths; break; }
        auto nearest = [&]() -> Actor* { Actor* b = nullptr; for (auto* f : liveFoes) if (alive(*f) && (!b || f->c.dist < b->c.dist)) b = f; return b; };
        refreshDist();
        auto partyAttack = [&](Actor& a) {
            if (now < a.nextAtk) return;
            a.nextAtk = now + 1.2; Actor* t = nearest(); if (!t) return;
            t->c.hp -= float(dmgTo(a, *t)); if (t->c.hp <= 0) t->alive = false; };
        partyAttack(player); for (auto& a : allies) if (alive(a)) partyAttack(a);
        player.c.hp = std::max(player.c.hp, player.c.hpMax * 0.1f);   // the player is not allowed to die in the sim
        for (auto* f : liveFoes) {
            if (!alive(*f)) continue;
            if (now >= f->stunUntil) { f->c.paralysed = false; f->c.fleeing = false; }
            if (now >= f->tauntUntil) f->c.taunted = false;
            if (now < f->stunUntil) { if (f->c.fleeing) StepToward(f->c, f->c.x + (f->c.x - self.c.x), f->c.y + (f->c.y - self.c.y), float(350 * kDt)); continue; }
            if (now >= f->retarget) {
                const int r = R.I(0, 3 + int(allies.size())); f->retarget = now + R.U(3, 8);
                f->c.targetId = r <= 1 ? 1u : r == 2 ? 2u : allies.empty() ? 1u : allies[size_t(R.I(0, int(allies.size()) - 1))].c.id; }
            if (now < f->tauntUntil) f->c.targetId = 1;
            f->c.targetIsAlly = f->c.targetId >= 10 && f->c.targetId < 100;
            Actor* tg = find(f->c.targetId); if (!tg || !alive(*tg)) { f->retarget = 0; continue; }
            const float dt = Dist(f->c, tg->c); const bool close = f->melee ? dt < 160.0f : dt < 1500.0f;
            if (!close) { StepToward(f->c, tg->c.x, tg->c.y, float(350 * kDt)); continue; }
            if (now >= f->nextAtk) { f->nextAtk = now + (f->melee ? 1.5 : 2.4); tg->c.hp -= float(dmgTo(*f, *tg));
                if (tg->c.hp <= 0 && tg != &player) tg->alive = false; }
        }
        liveFoes.clear(); for (auto& f : foes) if (alive(f)) liveFoes.push_back(&f);   // deaths this tick: the snapshot must not offer a corpse
        refreshDist();
        const bool low = self.c.hp < 0.35f * self.c.hpMax;
        if (low && lowSince < 0) { lowSince = now; lowHealedFlag = false; lowHadCand = false; ++S.lowEpisodes; }
        if (!low && lowSince >= 0) lowSince = -1;
        // ── snapshot ──
        Snapshot sn; sn.self = self.c; sn.self.dist = 0; sn.self.targetId = curTarget;
        for (auto* f : liveFoes) { Combatant c = f->c; if (!g_tauntVisible) c.taunted = false; sn.foes.push_back(c); }
        sn.allies.push_back(player.c); for (auto& a : allies) if (alive(a)) sn.allies.push_back(a.c);
        sn.playerId = 2; sn.currentTargetId = curTarget; sn.hasBook = true; sn.book = book; sn.book.kit = k;
        for (size_t i = 0; i < mySp.size(); ++i) {
            auto& act = active[i]; act.erase(std::remove_if(act.begin(), act.end(), [&](auto& pr) { return pr.second <= now; }), act.end());
            sn.book.entries[i].activeOn.clear(); for (auto& pr : act) sn.book.entries[i].activeOn.push_back(pr.first);
            sn.book.entries[i].liveSummon = summonUntil[i] > now; }
        sn.drawn = drawn; sn.meleeInRightHand = cls == Cls::Melee; sn.meleeOnly = cls == Cls::Melee; sn.tauntEnabled = gate.taunt; sn.tauntReach = 600; sn.combatSec = float(now);
        if (Actor* n = nearest()) sn.playerTargetId = n->c.id;
        // ── IAUS ──
        const auto t0 = std::chrono::steady_clock::now();
        Generated g = Generate(sn, gate, T);
        TickIn in; in.now = now; in.rapportRank = std::uint8_t(rank); in.seed = seed; in.commitWindowSec = 1.5;
        Decision d = Decide(mind, g.actions, P, T, in);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        S.dSum += us; S.dMax = std::max(S.dMax, us); ++S.dN;
        ++S.ticks; S.badFacts += long(d.badFacts);
        const bool hasCand = !g.actions.empty();
        if (!hasCand) ++S.noCandTicks;
        if (d.verdict == Verdict::Idle) ++S.idle; else if (d.verdict == Verdict::Hold) ++S.hold; else ++S.act;
        if (hasCand) { ++S.candTicks; if (d.verdict == Verdict::Idle) ++S.idleCand;
            bool inReach = false; for (auto* f : liveFoes) if (f->c.dist <= (cls == Cls::Melee ? sn.meleeReach : 1500.0f)) inReach = true;
            if (inReach) { ++S.reachTicks; if (d.verdict == Verdict::Idle) ++S.idleReach; } }
        if (lowSince >= 0) {   // heal-candidate bookkeeping for the low-HP episode
            bool healCand = false; for (const auto& c : g.actions) if (c.family == Family::CastHeal || c.family == Family::DrinkHealth) healCand = true;
            if (healCand) { if (!lowHadCand) { lowHadCand = true; ++S.lowCandEpisodes; } ++S.lowCandTicks; if (d.verdict == Verdict::Hold) ++S.lowHoldTicks; } }
        if (!g.postures.empty() && now >= busyUntil) {
            const int pi = DecidePosture(mind, g.postures, P, T, std::uint8_t(rank));
            if (pi >= 0 && size_t(pi) < g.postures.size() && g.postures[size_t(pi)].f.posture != drawn && g.postures[size_t(pi)].f.posture != PostureKind::None) {
                drawn = g.postures[size_t(pi)].f.posture; ++S.postureSwaps; busyUntil = now + 1.0; } }
        bool engineAI = d.verdict == Verdict::Idle || d.order.empty();   // the follower's own AI runs: it closes on the nearest foe
        // Hold: order[0] is the running decision (Core.h: "only the running decision may go"), fired as a continuation
        if (d.verdict != Verdict::Idle && !d.order.empty() && now >= busyUntil) {
            const Step& st = d.order[0]; bool bad = st.idx >= g.actions.size(); const Candidate* c = bad ? nullptr : &g.actions[st.idx];
            if (c) {
                Actor* tg = c->target ? find(c->target) : nullptr; const Family fam = c->family;
                const bool hostileT = fam == Family::Attack || fam == Family::PowerAttack || fam == Family::Taunt || fam == Family::CastOffense || fam == Family::CastControl;
                if (hostileT && (!tg || !alive(*tg) || tg->c.id < 100)) bad = true;
                if (fam == Family::DrinkHealth && k.potionsHealth <= 0) bad = true;
                if (fam == Family::DrinkStamina && k.potionsStamina <= 0) bad = true;
                if (fam == Family::DrinkMagicka && k.potionsMagicka <= 0) bad = true;
                const SimSpell* sp = nullptr; size_t si = 0;
                if (fam >= Family::CastOffense && fam <= Family::CastWard) {
                    bool found = false; for (size_t i = 0; i < mySp.size(); ++i) if (book.entries[i].form == c->form) { sp = &mySp[i]; si = i; found = true; }
                    if (!found || book.entries[si].cost > self.c.mp + 0.5f || !gate.spellsOnClass) bad = true;
                    if (found && book.entries[si].source == SpellSource::Staff && !book.entries[si].charged) bad = true; }
                if (bad) { ++S.invalid; if (std::getenv("SIM_DEBUG")) std::fprintf(stderr, "invalid: fam=%s target=%u form=%u mp=%.0f cls=%s\n", std::string(Name(c->family)).c_str(), c->target, c->form, self.c.mp, cd.name); }
                else {
                    OnFired(mind, *c, st, now); ++S.fired; ++S.hist[size_t(fam)]; if (d.newDecision) ++S.switches;
                    double dur = 1.0;
                    const bool healAct = fam == Family::DrinkHealth || fam == Family::CastHeal;
                    if (healAct && lowSince >= 0 && !lowHealedFlag && lowHadCand) { lowHealedFlag = true; ++S.lowHealed; S.lowHealSecSum += now - lowSince; }
                    switch (fam) {
                    case Family::Attack: case Family::PowerAttack: {
                        curTarget = tg->c.id; const bool rangedAtk = drawn == PostureKind::Bow;
                        if (!rangedAtk && tg->c.dist > 180.0f) { StepToward(self.c, tg->c.x, tg->c.y, float(350 * 0.6)); dur = 0.6; break; }   // closes first
                        float m = fam == Family::PowerAttack ? 1.8f : 1.0f; const float sc = float(staCost) * (fam == Family::PowerAttack ? 2.5f : 1.0f);
                        if (self.c.sp < sc) m *= 0.5f;
                        self.c.sp = std::max(0.0f, self.c.sp - sc);
                        if (rangedAtk) { if (k.arrows <= 0) m *= 0.3f; else --k.arrows; }
                        tg->c.hp -= float(dmgTo(self, *tg) * m); if (tg->c.hp <= 0) tg->alive = false; dur = fam == Family::PowerAttack ? 1.4 : 0.9; break; }
                    case Family::Taunt: {   // S7: a success chance and a duration, not instant and permanent
                        ++S.taunts; if (now < tg->tauntUntil) ++S.tauntRedundant;
                        if (R.U(0, 1) < 0.7f) { tg->tauntUntil = now + 10.0; tg->c.taunted = true; tg->c.targetId = 1; } else ++S.tauntFail;
                        dur = 0.8; break; }
                    case Family::Retreat: if (Actor* n = nearest()) { const float dx = self.c.x - n->c.x, dy = self.c.y - n->c.y, l = std::max(1.0f, std::hypot(dx, dy)); self.c.x += dx / l * 150; self.c.y += dy / l * 150; } dur = 0.8; break;
                    case Family::DrinkHealth: --k.potionsHealth; self.c.hp = std::min(self.c.hpMax, self.c.hp + B.potHealth); break;
                    case Family::DrinkStamina: --k.potionsStamina; self.c.sp = std::min(self.c.spMax, self.c.sp + B.potStamina); break;
                    case Family::DrinkMagicka: --k.potionsMagicka; self.c.mp = std::min(self.c.mpMax, self.c.mp + B.potMagicka); break;
                    case Family::CastOffense: case Family::CastControl: {
                        const bool staff = book.entries[si].source == SpellSource::Staff;
                        if (staff) { book.entries[si].charge -= 1.0f; book.entries[si].charged = book.entries[si].charge > 0.5f; k.staffCharged = book.entries[si].charged; }
                        else self.c.mp -= book.entries[si].cost;
                        dur = sp->castSec; curTarget = tg->c.id;
                        const double res = std::clamp(tg->c.Resist(sp->e.resistAv) / 100.0, 0.0, 1.0); const double scale = 1.0 + 0.02 * level;
                        if (sp->ctrl) {   // S3: the foe state the FoeControlled gate reads, for the effect's own duration
                            tg->stunUntil = now + std::clamp<double>(sp->ctrlDur, 1.0, 60.0); tg->c.paralysed = sp->ctrlPar; tg->c.fleeing = !sp->ctrlPar; }
                        else { const double n = sp->e.areaUnits > 0 ? double(liveFoes.size()) * 0.5 + 0.5 : 1.0; tg->c.hp -= float(sp->dmg * scale * (1.0 - res) * n); if (tg->c.hp <= 0) tg->alive = false; }
                        break; }
                    case Family::CastHeal: {
                        self.c.mp -= book.entries[si].cost; dur = sp->castSec; Actor* r = c->target ? find(c->target) : &self; if (!r) r = &self;
                        r->c.hp = std::min(r->c.hpMax, r->c.hp + sp->heal * float(1.0 + 0.02 * level)); break; }
                    case Family::CastBuff: case Family::CastWard: self.c.mp -= book.entries[si].cost; dur = sp->castSec; active[si].push_back({ c->target ? c->target : 1u, now + std::max(30.0, double(sp->dur)) }); break;
                    case Family::CastSummon: self.c.mp -= book.entries[si].cost; dur = sp->castSec; summonUntil[si] = now + 60; for (auto* f : liveFoes) f->nextAtk += 1.0; break;
                    default: break; }
                    busyUntil = now + dur; }
            }
        }
        if (engineAI && now >= busyUntil && cls == Cls::Melee) { if (Actor* n = nearest()) StepToward(self.c, n->c.x, n->c.y, float(350 * kDt)); }   // engine AI closes in
        now += kDt; S.simSec += kDt;
    }
}

// ── reporting ───────────────────────────────────────────────────────────────────────────────────────────
static void Merge(Stats& a, const Stats& b) {
    a.ticks += b.ticks; a.idle += b.idle; a.hold += b.hold; a.act += b.act; a.fired += b.fired; a.switches += b.switches; a.badFacts += b.badFacts; a.invalid += b.invalid;
    a.postureSwaps += b.postureSwaps; a.fights += b.fights; a.deaths += b.deaths; a.wins += b.wins; a.simSec += b.simSec; for (size_t i = 0; i < kFamilies; ++i) a.hist[i] += b.hist[i];
    a.lowEpisodes += b.lowEpisodes; a.lowCandEpisodes += b.lowCandEpisodes; a.lowHealed += b.lowHealed; a.lowHealSecSum += b.lowHealSecSum; a.lowCandTicks += b.lowCandTicks; a.lowHoldTicks += b.lowHoldTicks;
    a.dSum += b.dSum; a.dMax = std::max(a.dMax, b.dMax); a.dN += b.dN; a.noCandTicks += b.noCandTicks; a.candTicks += b.candTicks; a.idleCand += b.idleCand;
    a.reachTicks += b.reachTicks; a.idleReach += b.idleReach; a.taunts += b.taunts; a.tauntRedundant += b.tauntRedundant; a.tauntFail += b.tauntFail; }

static std::map<std::string, double> Metrics(const Stats& s) {
    std::map<std::string, double> m; const double t = std::max<double>(1, double(s.ticks)), f = std::max<double>(1, double(s.fired)), mins = std::max(1e-9, s.simSec / 60.0);
    m["idle%"] = 100.0 * double(s.idle) / t; m["hold%"] = 100.0 * double(s.hold) / t; m["sw/min"] = double(s.switches) / mins;
    m["heal.s"] = s.lowHealed ? s.lowHealSecSum / double(s.lowHealed) : -1;
    m["healed%"] = s.lowCandEpisodes ? 100.0 * double(s.lowHealed) / double(s.lowCandEpisodes) : -1;   // only episodes that had a heal candidate
    m["noHealCand%"] = s.lowEpisodes ? 100.0 * double(s.lowEpisodes - s.lowCandEpisodes) / double(s.lowEpisodes) : -1;
    m["holdLow%"] = s.lowCandTicks ? 100.0 * double(s.lowHoldTicks) / double(s.lowCandTicks) : -1;   // R2: Hold while hurt with a heal candidate
    m["bad"] = double(s.badFacts); m["invalid"] = double(s.invalid); m["dec.us"] = s.dSum / std::max<double>(1, double(s.dN)); m["dec.max"] = s.dMax;
    m["win%"] = 100.0 * double(s.wins) / std::max<double>(1, double(s.fights)); m["death%"] = 100.0 * double(s.deaths) / std::max<double>(1, double(s.fights));
    m["nocand%"] = 100.0 * double(s.noCandTicks) / t;
    m["iC%"] = 100.0 * double(s.idleCand) / std::max<double>(1, double(s.candTicks));      // Idle among ticks with >= 1 candidate
    m["iR%"] = 100.0 * double(s.idleReach) / std::max<double>(1, double(s.reachTicks));   // ...and a foe inside the class's reach
    m["taunt/min"] = double(s.taunts) / mins; m["tR%"] = s.taunts ? 100.0 * double(s.tauntRedundant) / double(s.taunts) : 0.0;
    for (size_t i = 0; i < kFamilies; ++i) if (s.hist[i]) m[std::string("a.") + std::string(Name(Family(i)))] = 100.0 * double(s.hist[i]) / f;
    m["posture/min"] = double(s.postureSwaps) / mins;
    return m; }

int main(int argc, char** argv) {
    const char* listPath = nullptr; const char* cmp = nullptr; const char* outBase = nullptr; std::uint64_t seed = 1; int fights = 8;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list") && i + 1 < argc) listPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fights") && i + 1 < argc) fights = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) cmp = argv[++i];
        else if (!std::strcmp(argv[i], "--baseline") && i + 1 < argc) outBase = argv[++i];
        else if (!std::strcmp(argv[i], "--taunt-visible")) g_tauntVisible = true;
        else { std::fprintf(stderr, "usage: run.sh [--list json] [--seed N] [--fights N] [--compare baseline] [--baseline out] [--taunt-visible]\n"); return 2; } }
    const Balance B = LoadBalance(listPath);
    std::printf("IAUS SIM  list=%s  seed=%llu  fights/cell=%d (x5 rapport ranks)  taunted-visible=%d\n", B.name.c_str(), (unsigned long long)seed, fights, int(g_tauntVisible));
    std::printf("spell pool: raw=%d taught/kept=%d  natures: offense=%d control=%d heal=%d buff=%d summon=%d ward=%d  dropped-unknown=%d  potions(h/m/s)=%.0f/%.0f/%.0f\n",
                B.rawSpells, B.taughtSpells, B.natures[1], B.natures[2], B.natures[3], B.natures[4], B.natures[5], B.natures[6], B.natures[0], B.potHealth, B.potMagicka, B.potStamina);
    std::map<std::string, std::map<std::string, double>> all; std::map<std::string, Stats> rankCell; long totalBad = 0, totalInvalid = 0;
    for (int c = 0; c < kNC; ++c) for (int b = 0; b < kNB; ++b) {
        Stats S;
        for (int r = 1; r <= 5; ++r) { Stats RS; for (int f = 0; f < fights; ++f) {
            Rng R; R.g.seed(seed * 1000003ULL + std::uint64_t(c) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(r) * 15485863ULL + std::uint64_t(f) * 32452843ULL);
            RunFight(B, kClasses[c], r, kBrackets[b], R, R.g(), RS); }
            Merge(S, RS); Merge(rankCell[std::string(kClasses[c].name) + "|rank" + char('0' + r)], RS); }
        const std::string key = std::string(kClasses[c].name) + "|" + kBrackets[b].name; all[key] = Metrics(S); totalBad += S.badFacts; totalInvalid += S.invalid; }
    std::printf("\n%-20s %5s %5s %5s %6s %6s %5s %5s %5s %4s %4s %5s %5s %5s %5s %5s  actions (%% of fired)\n", "class|bracket", "idle", "hold", "sw/m", "heal.s", "heal%", "noHC%", "holdL", "iC%", "iR%", "inv", "dec.us", "win%", "death", "tnt/m", "tR%");
    for (auto& kv : all) { auto& m = kv.second; auto g = [&](const char* k) { auto i = m.find(k); return i == m.end() ? 0.0 : i->second; };
        std::printf("%-20s %5.1f %5.1f %5.1f %6.2f %6.1f %5.0f %5.1f %5.1f %4.1f %4.0f %5.1f %5.0f %5.0f %5.1f %5.0f  ", kv.first.c_str(), g("idle%"), g("hold%"), g("sw/min"), g("heal.s"), g("healed%"), g("noHealCand%"), g("holdLow%"), g("iC%"), g("iR%"), g("invalid"), g("dec.us"), g("win%"), g("death%"), g("taunt/min"), g("tR%"));
        for (auto& a : m) if (a.first.rfind("a.", 0) == 0 && a.second >= 1.0) std::printf("%s=%.0f ", a.first.c_str() + 2, a.second);
        std::printf("\n"); }
    std::printf("\nIdle%% by rapport rank (all brackets):\n");
    for (auto& kv : rankCell) std::printf("  %-18s idle %.1f%%  hold %.1f%%  sw/min %.1f\n", kv.first.c_str(), 100.0 * double(kv.second.idle) / std::max<double>(1, double(kv.second.ticks)),
                                         100.0 * double(kv.second.hold) / std::max<double>(1, double(kv.second.ticks)), double(kv.second.switches) / std::max(1e-9, kv.second.simSec / 60.0));
    std::printf("\nCHECKS: BAD-FACTS ticks=%ld (must be 0)  invalid picks=%ld (must be 0)  => %s\n", totalBad, totalInvalid, totalBad == 0 && totalInvalid == 0 ? "PASS" : "FAIL");
    if (outBase) { std::ofstream o(outBase); for (auto& kv : all) for (auto& m : kv.second) o << kv.first << "|" << m.first << "=" << m.second << "\n"; std::printf("baseline written: %s\n", outBase); }
    if (cmp) {
        std::ifstream f(cmp); std::string line; int diffs = 0; std::printf("\nCOMPARE vs %s (|delta| > max(0.5, 5%%)):\n", cmp);
        while (std::getline(f, line)) { const auto eq = line.rfind('='); if (eq == std::string::npos) continue; const std::string k = line.substr(0, eq); const double old = std::atof(line.c_str() + eq + 1);
            const auto p = k.rfind('|'); const auto ci = all.find(k.substr(0, p)); double nv = 0; if (ci != all.end()) { auto mi = ci->second.find(k.substr(p + 1)); if (mi != ci->second.end()) nv = mi->second; }
            if (k.find("dec.") != std::string::npos) continue;
            if (std::fabs(nv - old) > std::max(0.5, 0.05 * std::fabs(old))) { std::printf("  %-34s %9.2f -> %9.2f\n", k.c_str(), old, nv); ++diffs; } }
        std::printf("%d metric(s) changed\n", diffs); }
    return totalBad == 0 && totalInvalid == 0 ? 0 : 1;
}
