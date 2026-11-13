#pragma once
// iaus_sim/Setup.h -- who is in a fight: the class cells, the party, the foes from the list's real encounters
// (or the fixture groups), and the relative-strength brackets. HOST-ONLY.
#include "iaus_sim/Table.h"
#include "iaus/ClassCurves.h"

namespace Sim {

    // ── a class cell: the gate the generator reads, the kit the class may carry, its I8 profile ──────────
    struct SimClass {
        std::string name;
        std::string line = "Melee";   // the class table's line: Melee / Ranged / Mage / Hybrid
        Cls   fam = Cls::Melee;
        ClassGate gate;
        KitChoice kit;
        Profile profile;
        int   tier = 1;
        bool  planned = false;
        bool shortLeash = false;   // Flag::kShortLeash (the wall classes)
        std::vector<GRow> rows;  // the class's default gambit rows (table mode); applied = false for T1 rows kept as data
        std::string package = "current", defaults = "current";   // ids of the package / default rows in use (results file)
        std::string playstyle;   // the human expert playstyle the oracle and the personality metric are measured against
        // class traits the world applies (RealClasses.cpp: the table's flags, plus the passives the class cards rule on stealth)
        bool  sneakAlways = false, flank = false, cloak = false, nearPlayer = false, pureMage = false;
        int   entryOdds = 0, goIn = 0, partyPct = 0;   // the free stealth entry is 1 fight in entryOdds (card C13)
        float sneakAtkClass = 1.0f, noiseClass = 1.0f;
        unsigned forbid = 0;                 // Playstyles.h: the class's oracle constraint bits
        std::vector<std::string> ensure;     // the spells its own default rows name (Load.h SpellIs tokens): a Conjurer knows a summon
        // T2 batch S (RealClasses.cpp fills them from the game's class table; section 31)
        float shareMax = -1.0f, shareMin = -1.0f;   // ClassDef::powerShareMax / powerShareMin (ch.18), -1 = not declared
        bool  leashScaled = false;                  // Flag::kShortLeash | kNearPlayer on a tier 2+ class: Classes::PackageLeashScale = fClassLeashMult (0.5)
        float avoidThreat = -1.0f;                  // MFO_GenerateESP.py CLS_AVOID_THREAT (Q-S-3, marth 11-12: Mercenary line 0.35 / 0.50 / 0.55, Thief 0.25, Master Thief 0.30, Knight line 0, else the vanilla 0.20; -1 = a fixture class, untouched): SETS every stance's Avoid Threat, the ranged stance +0.10
        float aversion = 0.0f;                      // apmf/AversionClaims.cpp: fAversionConjurer / fAversionHealer 0.25 for the EQUIPPED class (Q93, Q94; Dragonborn gives none: Q107)
        int   aimBudget = -1;                       // apmf/AimClaims.cpp: the ch.29 miss budget of a ranged class (Archer 5)
        int   csDisengage = -1;                     // the class STYLE's kDisengage offset (class_csty_params): -1 = as gate.disengage (a table class: the flag is both), 0 = none (a tree-OFF base cell: the gate opens Retreat, but there is no class and so no class style)
    };
    // --teach <token>: card "Teach Spells" (Board_Classes.cpp, batch I item 7). MODEL of its effect only: the subject of a casting class
    // also knows one spell of that token, drawn from the pool IAUS would weigh (an unweighable spell is never in the pool, so it cannot be taught).
    inline std::vector<std::string> g_teach;
    inline constexpr float kLeashBase = 2256.0f, kLeashMin = 512.0f, kClassLeashMult = 0.5f;   // Confidence::LeashRadius at mid confidence between fLeashMin 512 and fLeashMax 4000 (MODEL: the lab has no confidence value); Config fClassLeashMult

    inline ClassGate MkGate(bool treeOn, bool spells, bool power, bool taunt, bool dis, bool melee, bool bow, bool xbow, bool staff) {
        ClassGate g; g.resolved = true; g.treeOn = treeOn; g.strict = false; g.spellsOnClass = spells; g.scrollUser = spells; g.powerAttack = power;
        g.powerCooldownSec = 0.0f; g.taunt = taunt; g.disengage = dis; g.weaponMelee = melee; g.weaponBow = bow; g.weaponCrossbow = xbow; g.weaponStaff = staff; return g; }

    // The eight hand-written cells of sim v1 (kept so v1 baselines stay comparable): T1 tree ON, tree OFF (Q63 emulated), Knight, Evoker.
    inline std::vector<SimClass> LegacyClasses() {
        auto kit = [](bool oneH, bool dagger, bool bow, bool staff, bool shield, bool heavy, bool light) { KitChoice k; k.oneHand = oneH; k.dagger = dagger; k.bow = bow; k.staff = staff; k.shield = shield; k.heavy = heavy; k.light = light; return k; };
        std::vector<SimClass> v;
        auto add = [&](const char* n, Cls f, ClassGate g, KitChoice k, int tier) { SimClass c; c.name = n; c.fam = f; c.line = f == Cls::Melee ? "Melee" : f == Cls::Ranged ? "Ranged" : "Mage"; c.gate = g; c.kit = k; c.tier = tier; v.push_back(std::move(c)); };
        const KitChoice km = kit(true, false, false, false, false, true, false), kr = kit(false, true, true, false, false, false, true), kg = kit(false, true, false, true, false, false, false);
        add("melee", Cls::Melee, MkGate(true, false, false, false, false, true, true, true, true), km, 1);
        add("ranged", Cls::Ranged, MkGate(true, false, false, false, false, true, true, true, true), kr, 1);
        add("mage", Cls::Mage, MkGate(true, true, false, false, false, true, true, true, true), kg, 1);
        add("melee-off", Cls::Melee, MkGate(false, true, true, true, true, true, true, true, true), km, 1);
        add("ranged-off", Cls::Ranged, MkGate(false, true, true, true, true, true, true, true, true), kr, 1);
        add("mage-off", Cls::Mage, MkGate(false, true, true, true, true, true, true, true, true), kg, 1);
        add("knight-T2", Cls::Melee, MkGate(true, false, true, true, false, true, false, false, false), kit(true, false, false, false, true, true, false), 2);
        add("evoker-T2", Cls::Mage, MkGate(true, true, false, false, true, true, false, false, true), kg, 2);
        return v;
    }

    // The game's own 35 class rows (RealClasses.cpp, built from native/classes/ClassTable.cpp), then the 3 tree-OFF base cells
    // (melee-off / ranged-off / mage-off, line "Base", tier 1; indexes 35..37). mechProfiles: the I8 mechanical profile.
    std::vector<SimClass> RealClasses(bool mechProfiles);
    // Per-class curves (ruling Q-class-curves): the sim's OWN copy of iaus/ClassCurves.h's table, which RealClasses reads for every class's
    // Profile and edge constants. --curve edits it (ApplyCurveSpecs); untouched it is the shipped table, so scores are unchanged.
    inline MFO::Iaus::ClassCurves::Table g_curveTable = MFO::Iaus::ClassCurves::Defaults();
    inline std::vector<std::size_t> g_curveConsEdited;   // the Cons indexes a --curve spec overrode (sim.cpp refuses --opt c.N on one of them: the override is a COPY of the shared curve, the search would not move it)
    // The curve row of a tree-OFF base cell (melee-off / ranged-off / mage-off = ClassCurves::kBaseOffFirst + root), -1 for any other name.
    inline int BaseOffRow(const std::string& a_name) {
        static const char* const kOff[] = { "melee-off", "ranged-off", "mage-off" };
        for (std::size_t i = 0; i < 3; ++i) if (a_name == kOff[i]) return static_cast<int>(MFO::Iaus::ClassCurves::kBaseOffFirst + i);
        return -1;
    }
    // "<class slug>:<family>.<cons>.<a|b|floor|zeroAbove|zeroBelow>=<v>" or "<class slug>:scalar.<drinkHealthLine|touchSpentSec>=<v>".
    // <family> / <cons> are an index or a Name() (an index when several considerations share a display name). a_shared = the curve an
    // override starts from. False with a_err set on any bad token.
    // The potion-worth table (Oracle.h Utility): class cell -> worth rows. Built once before any fight (sim.cpp: PotionWorth.h for the
    // built-in world, or --potion-worth <file> for a list), read-only afterwards. g_forcePot: --potion-calib sets the subject's stock of one kind.
    inline std::map<std::string, PotWorth> g_potWorth;
    inline int g_forcePot[3] = { -1, -1, -1 };
    [[nodiscard]] inline const PotWorth* PotWorthFor(const std::string& cls) { const auto i = g_potWorth.find(cls); return i == g_potWorth.end() ? nullptr : &i->second; }
    bool ApplyCurveSpecs(const std::vector<std::string>& a_specs, const Tuning& a_shared, std::string& a_err);
    void PrintClassTable(const std::vector<SimClass>& v);

    struct Bracket { const char* name; float lo, hi; bool boss; };
    // natural = the list's encounters as placed, no filter. The others are strength-ratio bands (StrengthRatio below).
    inline constexpr Bracket kBrackets[] = { { "natural", 0.0f, 1e9f, false }, { "balanced", 0.6f, 1.25f, false }, { "weak", 0.0f, 0.6f, false }, { "strong1", 1.25f, 2.0f, false },
                                             { "strong2", 2.0f, 3.5f, false }, { "mixed-boss", 0.7f, 2.6f, true } };
    inline constexpr int kNB = 6;

    // ── strength estimate (for the brackets only): effective health and damage per second of one actor ──
    struct Power { float hpEff = 1, dps = 1; };
    inline Power PowerOf(const Data& D, const ActorSpec& a, const std::vector<SimSpell>* own = nullptr) {
        Power p; const Rules& r = D.rules;
        p.hpEff = a.hp / std::max(0.15f, 1.0f - ArmorMitigation(r, a.ar, a.pieces));
        const float sw = (!a.melee.Has() ? TM::swingCreature : a.melee.TwoH() ? TM::swing2H : TM::swing1H) / std::max(0.3f, a.melee.Has() ? a.melee.speed : 1.0f) + 0.5f;
        float dps = WeaponDamage(r, a, a.melee, false) / sw;
        if (a.ranged.Has()) dps = std::max(dps, WeaponDamage(r, a, a.ranged, false) / 2.6f);
        auto sp = [&](const SimSpell& s, float cost) { if (s.e.nature == Prune::Nature::Offense && cost <= a.mp) dps = std::max(dps, (s.dmg + s.dot * s.dotDur) / (s.castSec + TM::castRecover + 0.5f + s.recovery)); };
        for (int si : a.spells) sp(D.foeSpells[static_cast<std::size_t>(si)], D.foeSpells[static_cast<std::size_t>(si)].e.cost);
        if (own) for (const SimSpell& s : *own) sp(s, s.e.cost);
        p.dps = std::max(0.5f, dps); return p;
    }

    struct FightSpec {           // everything a fight is built from (the scenarios fill it by hand)
        const SimClass* cls = nullptr;
        int   level = 20, rank = 3;
        std::vector<const SimClass*> others;     // further IAUS followers
        Cls   playerFam = Cls::Melee;
        std::vector<std::pair<int, std::pair<float, float>>> foes;   // spec index, offset
        std::vector<std::shared_ptr<const ActorSpec>> customFoes;    // scenario-made specs (appended after foes)
        std::vector<std::string> foeTags;        // optional, parallel to foes then customFoes
        bool  interior = false;
        float foeDist = 1500, worn = 0;          // worn > 0: the party starts hurt (hp x U(1 - worn, 1), magicka likewise)
        bool  foesOnPlayer = false;              // scenario: every foe starts on the player
        std::uint64_t seed = 1;
        int   occluders = -1;                    // -1 = by interior
        std::vector<std::pair<double, std::shared_ptr<const ActorSpec>>> late;   // (seconds, spec): foes that join later
        std::vector<std::string> lateTags;
        float playerHp = 1, subjectHp = 1, subjectMp = 1;   // scenario start fractions
        int   playerPotions = 4; int subjectPotionsH = -1;  // -1 = the random kit
        // The rating battery (Rating.h): everything a fight draws that must NOT depend on the rated class.
        int   subjectPotionsM = -1, subjectPotionsS = -1, subjectArrows = -1;   // -1 = the random kit
        const SimClass* refCls = nullptr;                   // targetRatio is met against a REFERENCE party whose subject is this class (same level, same seed), so the foes' scale never reads the rated class
        std::uint64_t geoSeed = 0;                          // != 0: the second follower's build, the foes' places, the cover and the fight's opening random state come from this seed alone (0 = the fight's own stream, as always)
        float foeSpread = 250;
        std::vector<std::string> ensure, ensureOthers;      // spells the subject / the other followers must know (Load.h SpellIs)
        bool  onSubject = false;                            // every foe starts on the subject
        float targetRatio = 0;                              // > 0: the foes' health and damage are scaled so StrengthRatio lands here (a scenario must be winnable by good play on every list)
        float light = -1;                                   // stealth: the light on the party, 0 dark .. 1 daylight; < 0 = drawn (interior 0.15..0.6, outside day 0.9 / night 0.2)
        int   freeEntry = -1;                               // stealth: 1 = the subject's free stealth entry is this fight, 0 = it is not, -1 = rolled 1 in entryOdds
        // Scenario geometry: solid cover placed by hand, in the fight's own frame (u = units from the player toward the foes' centre,
        // v = units to the left of that line). Added to the random occluders (`occluders`).
        struct Cover { float u, v, r; }; std::vector<Cover> cover;
    };

    // TODAY'S PACKAGE: MFO's own three stance styles (MFO_GenerateESP.py make_csty, CSGD verbatim; CSCR from the vanilla style it
    // mirrors) with the class offsets of class_csty_params (kShortLeash: circle -0.10, fallback -0.15; kDisengage: fallback +0.15,
    // and, with the C8 road off, avoidThreat +0.20 outside the melee stance). No attack mix, no leash (card C8 adds them).
    inline void MfoPackage(const SimClass& sc, Ctl& c) {
        const float csgd[3][10] = { { 1.0f, 0.5f, 1.0f, 10.0f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f, 0.0f }, { 1.0f, 0.5f, 1.0f, 0.1f, 0.1f, 10.0f, 1.0f, 0.1f, 0.1f, 0.5f }, { 1.0f, 0.5f, 1.0f, 0.1f, 10.0f, 0.2f, 1.0f, 0.1f, 1.0f, 0.2f } };
        const float cscr[3][2] = { { 0.73f, 0.42f }, { 0.45f, 0.65f }, { 0.30f, 0.50f } };
        for (int s = 0; s < 3; ++s) { Style& y = c.pkg[s]; y = Style{}; y.off = csgd[s][0]; y.def = csgd[s][1]; y.groupOff = csgd[s][2]; y.wMelee = csgd[s][3]; y.wMagic = csgd[s][4]; y.wRanged = csgd[s][5]; y.wShout = csgd[s][6];
            y.wUnarmed = csgd[s][7]; y.wStaff = csgd[s][8]; y.avoidThreat = csgd[s][9]; y.circle = cscr[s][0]; y.fallback = cscr[s][1]; y.dueling = true;
            if (sc.shortLeash) { y.circle = std::max(0.0f, y.circle - 0.10f); y.fallback = std::max(0.0f, y.fallback - 0.15f); }
            if (sc.csDisengage >= 0 ? sc.csDisengage != 0 : sc.gate.disengage) { y.fallback = std::min(1.0f, y.fallback + 0.15f); if (s != 0 && !(g_t2.c8 && sc.avoidThreat >= 0.0f)) y.avoidThreat = std::min(1.0f, y.avoidThreat + 0.20f); }
            if (g_t2.c8 && sc.avoidThreat >= 0.0f) y.avoidThreat = sc.avoidThreat > 0.0f && s == 1 ? sc.avoidThreat + 0.10f : sc.avoidThreat;   // class_csty_params: SET per class (Q-S-3), no longer a floor
            // HEAVY BODY ARMOR = NO DODGE (apmf/StyleClaims.cpp, merged fcbfec5: while his cuirass is heavy the class style's ZERO-AVOID TWIN is
            // claimed, whatever his class; marth 11-12: for a follower with no class style the list's own dodge rules already stop a heavy
            // dodger, so 0 there too). The kit's `heavy` is his cuirass (MODEL: RealClasses.cpp dresses a class by its card).
            if (g_t2.c8 && sc.avoidThreat >= 0.0f && sc.kit.heavy) y.avoidThreat = 0.0f; }
        c.powerShare = 1.0f; c.leash = 0.0f;
        if (g_t2.c8) {   // card C8: the ch.18 rule and the ch.23 leash (every managed follower is leashed to the player; a wall or near-player class at half the radius, never under fLeashMin)
            c.shareMax = sc.shareMax; c.shareMin = sc.shareMin;
            c.leash = std::max(kLeashMin, kLeashBase * (sc.leashScaled ? kClassLeashMult : 1.0f));
        }
        c.pkgClass = sc.tier >= 2;   // Classes::PackageClass: tier 1 has no package ("t1 behaves vanilla")
        c.aversion = sc.aversion; c.aimBudget = sc.aimBudget;
    }
    inline void (*&PackageHook())(Ctl&) { static void (*h)(Ctl&) = nullptr; return h; }

    // Ruling Q74 (marth 2026-11-11): a follower holds the TOP rows of his class's default set, as many as his rapport rank
    // has combat slots (native/State.h kCombatSlotsByRank: 3 / 5 / 7 / 9 / 12), and the rest join in the same order as slots
    // unlock. g_rowPrefix > 0 forces the prefix length for every follower (the prefix search), g_forceRank the subject's rank.
    inline int g_rowPrefix = 0, g_forceRank = 0;
    [[nodiscard]] inline int CombatSlotsForRank(int rank) { static constexpr int k[6] = { 0, 3, 5, 7, 9, 12 }; return k[std::clamp(rank, 1, 5)]; }
    inline void AddFollower(Fight& F, const Data& D, const SimClass& sc, int level, int rank, float x, float y, Rng& R, float worn, const std::vector<std::string>* ensure = nullptr) {
        std::vector<std::string> taught;   // --teach: the subject only, a casting class only
        if (F.ctl.empty() && !g_teach.empty() && sc.gate.spellsOnClass) { if (ensure) taught = *ensure; for (const std::string& t : g_teach) taught.push_back(t); ensure = &taught; }
        auto fb = std::make_shared<FollowerBuild>(BuildFollower(D, sc.fam, sc.kit, sc.gate, level, R, true, ensure));
        if (F.ctl.empty() && !g_namedFollower.empty()) {   // batch 3 sanity cell: the subject from a named record (the spec compiled nearest his level)
            const auto it = D.named.find(g_namedFollower);
            if (it == D.named.end() || it->second.empty()) { std::fprintf(stderr, "--named-follower %s: not in this world's sim.json (compile_world.py with extract_world.py --named)\n", g_namedFollower.c_str()); std::exit(2); }
            int best = -1, bd = 1 << 30; for (const auto& [lv, si] : it->second) if (si >= 0 && std::abs(lv - level) < bd) { bd = std::abs(lv - level); best = si; }
            const ActorSpec& ns = D.specs[static_cast<std::size_t>(best)]; ActorSpec& a = fb->spec; Kit& k = fb->book.kit;
            a.race = ns.race; a.hp = ns.hp; a.mp = ns.mp; a.sp = ns.sp; a.hpR = ns.hpR; a.mpR = ns.mpR; a.spR = ns.spR; a.skill = ns.skill; a.run = ns.run; a.mass = ns.mass;
            if (ns.melee.Has()) { a.melee = ns.melee; k.melee = true; }
            if (ns.ranged.Has()) { a.ranged = ns.ranged; if (ns.ammoDmg > 0.0f) a.ammoDmg = ns.ammoDmg; k.bow = ns.ranged.anim != 9; k.crossbow = ns.ranged.anim == 9; if (k.arrows + k.bolts <= 0) k.arrows = R.I(20, 80); }
            if (ns.ar > 0.0f) { a.ar = ns.ar; a.pieces = ns.pieces; }
            a.shield = a.shield && a.melee.Has() && !a.melee.TwoH();
            // his perks: the follower perk model (row 11) at HIS skills, per weapon (the record's own PRKR entries are condition-gated: row 10)
            a.atkMult = a.melee.Has() ? PerkBest(D.pAttack, std::to_string(a.melee.skill), a.Skill(a.melee.skill)) : 1.0f;
            a.powMult = a.melee.Has() ? PerkBest(D.pPower, std::to_string(a.melee.skill), a.Skill(a.melee.skill)) : 1.0f;
            a.rangedAtkMult = a.ranged.Has() ? PerkBest(D.pAttack, std::to_string(a.ranged.skill), a.Skill(a.ranged.skill)) : -1.0f;
        }
        auto spec = std::make_shared<ActorSpec>(fb->spec); spec->name = F.ctl.empty() ? "Follower" : "Follower" + std::to_string(F.ctl.size() + 1);
        Body& b = F.Add(Role::Follower, spec, x, y); b.ctl = static_cast<int>(F.ctl.size()); b.arrows = fb->book.kit.arrows + fb->book.kit.bolts;
        if (worn > 0) { b.c.hp *= R.U(1.0f - worn, 1.0f); b.c.mp *= R.U(1.0f - worn, 1.0f); }
        Ctl c; c.body = static_cast<int>(F.b.size()) - 1; c.fam = sc.fam; c.gate = sc.gate; c.profile = sc.profile; if (ProfileHook()) ProfileHook()(c.profile);
        c.forbid = sc.forbid; c.sneakClass = sc.sneakAlways; c.flank = sc.flank; c.cloak = sc.cloak; c.nearPlayer = sc.nearPlayer; c.entryOdds = sc.entryOdds; c.goIn = sc.goIn; c.partyPct = sc.partyPct; c.sneakAtkClass = sc.sneakAtkClass; c.noiseClass = sc.noiseClass;
        if (g_stealth > 0 && sc.sneakAlways) { b.stealthy = true; b.sneaking = true; b.rec.sp = F.dg.spMax; }   // G1: the sneak classes hold the stance for the whole fight
        c.armorWeight = static_cast<float>(fb->spec.pieces) * (sc.kit.heavy ? 9.0f : 3.0f);   // MODEL (Detect.h In::weight)
        { bool anyApplied = false; for (const GRow& r : sc.rows) anyApplied |= r.applied; for (const GRow& r : sc.rows) if (r.applied || !anyApplied) c.rows.push_back(r); }   /* T1: its data-only rows stand in for the kit of today */
        { const std::size_t cap = static_cast<std::size_t>(g_rowPrefix > 0 ? g_rowPrefix : CombatSlotsForRank(rank)); if (c.rows.size() > cap) c.rows.resize(cap); }   /* ruling Q74 */
        if (g_learnReach) {   // ruling Q68: start at the vanilla reach; warm = what this session's earlier swings taught him
            const float truth = F.Reach(b) * 1.2f;
            Rng W; W.g.seed(977u + static_cast<unsigned>(truth * 16.0f));   // its own stream: the fight's draws stay the same in every --reach mode
            for (auto& L : c.reach) { L.Reset(200.0f);
                if (g_learnReach >= 2) for (int i = 0; i < 20; ++i) { const float d = W.U(0.5f, 1.1f) * truth; L.Observe(d, d + 0.04f * truth * (W.U(-1.0f, 1.0f) + W.U(-1.0f, 1.0f)) <= truth); } }
        }
        MfoPackage(sc, c);
        if (PackageHook() && F.ctl.empty()) PackageHook()(c);   // the package under search (the subject only)
        c.build = fb; c.book = fb->book;
        c.active.resize(fb->spells.size()); c.summonUntil.assign(fb->spells.size(), 0.0); c.rank = static_cast<std::uint8_t>(rank); c.seed = R.g();
        c.window = 1.5 * (0.88 + 0.24 * R.U(0.0f, 1.0f));   // g_suppressWindow x (0.88 + 0.24 Temperament), Engine::MakeTick
        c.drawn = sc.fam == Cls::Ranged && fb->spec.ranged.Has() ? PostureKind::Bow : sc.fam == Cls::Mage ? PostureKind::None : fb->spec.melee.Has() ? PostureKind::Melee : PostureKind::None;
        c.potH = fb->potH; c.potM = fb->potM; c.potS = fb->potS; for (int i = 0; i < 3; ++i) c.potDur[i] = fb->potDur[i];
        c.worth = PotWorthFor(sc.name);
        F.ctl.push_back(std::move(c));
    }

    inline void TagFoe(const Data& D, Body& f) {
        bool healer = false, caster = false;
        for (int si : f.spec->spells) { const SimSpell& s = D.foeSpells[static_cast<std::size_t>(si)]; healer |= s.healOther; caster |= s.e.nature == Prune::Nature::Offense; }
        f.tag = healer ? "healer" : caster ? "caster" : (f.spec->ranged.Has() && (!f.spec->melee.Has() || f.spec->st.wRanged > f.spec->st.wMelee)) ? "archer" : "grunt";
    }

    inline float StrengthRatio(const Data& D, const Fight& F);

    inline Fight BuildFight(const Data& D, const FightSpec& fs, const std::vector<SimClass>& t1) {
        Fight F; F.D = &D; F.dg = Detect::Load(D); F.R.g.seed(fs.seed); Rng& R = F.R; F.avSeed = fs.seed * 0xD6E8FEB86659FD93ULL + 0x2545F4914F6CDD1DULL;
        // the player model: a T1 kit of his own, run by the same engine-AI model (no IAUS)
        const SimClass& pc = t1[static_cast<std::size_t>(fs.playerFam)];
        { auto pb = BuildFollower(D, pc.fam, pc.kit, pc.gate, fs.level, R); auto ps = std::make_shared<ActorSpec>(pb.spec); ps->name = "Player"; ps->spells.clear();
          Body& p = F.Add(Role::Player, ps, 0, 0); p.arrows = 80; p.potions = 4; if (fs.worn > 0) p.c.hp *= R.U(1.0f - fs.worn * 0.5f, 1.0f); }
        auto place = [&](Cls f) { return std::pair<float, float>{ f == Cls::Melee ? 220.0f : f == Cls::Ranged ? -300.0f : -250.0f, R.U(-220, 220) }; };
        { auto [x, y] = place(fs.cls->fam); AddFollower(F, D, *fs.cls, fs.level, fs.rank, x, y, R, fs.worn, fs.ensure.empty() ? nullptr : &fs.ensure); }
        Rng Gs; if (fs.geoSeed) Gs.g.seed(fs.geoSeed); Rng& G = fs.geoSeed ? Gs : R;   // the rating battery: a stream the subject's build never draws from
        for (const SimClass* o : fs.others) { auto [x, y] = place(o->fam); AddFollower(F, D, *o, fs.level, 3, x, y, G, fs.worn, fs.ensureOthers.empty() ? nullptr : &fs.ensureOthers); }
        const float ang = G.U(-0.4f, 0.4f), cx = fs.foeDist * std::cos(ang), cy = fs.foeDist * std::sin(ang);
        std::size_t ti = 0;
        auto addFoe = [&](std::shared_ptr<const ActorSpec> sp, float dx, float dy) {
            Body& f = F.Add(Role::Foe, std::move(sp), cx + std::clamp(dx, -700.0f, 700.0f), cy + std::clamp(dy, -700.0f, 700.0f));
            TagFoe(D, f); if (ti < fs.foeTags.size() && !fs.foeTags[ti].empty()) f.tag = fs.foeTags[ti]; ++ti;
            if (fs.foesOnPlayer) { f.c.targetId = 2; f.retargetAt = 6.0; }
            if (fs.onSubject) { Body& s0 = F.b[static_cast<std::size_t>(F.ctl[0].body)]; f.c.targetId = s0.c.id; f.retargetAt = 8.0;
                if (s0.stealthy) { s0.rec.level = 100; s0.rec.sp = 0.0f; s0.rec.known = true; s0.rec.lost = false; s0.rec.lastKnown = 0.0; s0.rec.lx = s0.c.x; s0.rec.ly = s0.c.y; } }   // "they are on him": their group has found him
            Body& nf = F.b.back(); const Body* ft = F.Find(nf.c.targetId); Fight::Face(nf, ft ? ft->c.x : 0.0f, ft ? ft->c.y : 0.0f);   // a foe starts facing its target, else the party
        };
        for (const auto& m : fs.foes) addFoe(std::shared_ptr<const ActorSpec>(std::shared_ptr<const ActorSpec>{}, &D.specs[static_cast<std::size_t>(m.first)]), m.second.first, m.second.second);
        for (const auto& s : fs.customFoes) { const float dy = G.U(-fs.foeSpread, fs.foeSpread), dx = G.U(-fs.foeSpread, fs.foeSpread); addFoe(s, dx, dy); }   // dy FIRST: the order the old call's arguments were drawn in (g++ evaluates them right to left), now fixed in the source so every compiler builds the same fights
        for (std::size_t i = 0; i < fs.late.size(); ++i) { const float lx = G.U(-200, 200), ly = G.U(-200, 200); F.late.push_back({ fs.late[i].first, fs.late[i].second, cx + lx, cy + ly, i < fs.lateTags.size() ? fs.lateTags[i] : std::string() }); }
        if (fs.targetRatio > 0) {   // late foes count toward the strength as if present
            float ratio = 0.0f;
            if (fs.refCls && fs.refCls != fs.cls) { FightSpec rs = fs; rs.cls = fs.refCls; rs.refCls = nullptr; rs.targetRatio = 0; rs.ensure.clear(); Fight T = BuildFight(D, rs, t1); for (const Spawn& s : T.late) T.Add(Role::Foe, s.spec, s.x, s.y); ratio = StrengthRatio(D, T); }   // the reference party's fight: same seed, same foes
            else { Fight T = F; for (const Spawn& s : T.late) T.Add(Role::Foe, s.spec, s.x, s.y); ratio = StrengthRatio(D, T); }
            const float k = std::clamp(fs.targetRatio / std::max(0.01f, ratio), 0.05f, 20.0f);
            for (Body& x : F.b) if (x.role == Role::Foe) { auto sp = std::make_shared<ActorSpec>(*x.spec); sp->hp *= k; x.spec = sp; x.c.hpMax *= k; x.c.hp *= k; x.dmgScale = k; }
            for (Spawn& s : F.late) { auto sp = std::make_shared<ActorSpec>(*s.spec); sp->hp *= k; s.spec = sp; s.dmgScale = k; }
        }
        for (const Body& x : F.b) if (x.role == Role::Foe) { F.foeHpTotal += x.c.hpMax; ++F.t.foesFaced; }
        F.t.foesFaced += static_cast<long>(F.late.size());
        for (const Spawn& s : F.late) F.foeHpTotal += s.spec->hp;
        F.b[0].c.hp *= fs.playerHp; F.b[0].potions = fs.playerPotions;
        { Body& s0 = F.b[static_cast<std::size_t>(F.ctl[0].body)]; s0.c.hp *= fs.subjectHp; s0.c.mp *= fs.subjectMp; if (fs.subjectPotionsH >= 0) F.ctl[0].book.kit.potionsHealth = fs.subjectPotionsH;
          if (fs.subjectPotionsM >= 0) F.ctl[0].book.kit.potionsMagicka = fs.subjectPotionsM; if (fs.subjectPotionsS >= 0) F.ctl[0].book.kit.potionsStamina = fs.subjectPotionsS; if (fs.subjectArrows >= 0 && s0.arrows > 0) s0.arrows = fs.subjectArrows; }
        { Kit& k0 = F.ctl[0].book.kit; if (g_forcePot[0] >= 0) k0.potionsHealth = g_forcePot[0]; if (g_forcePot[1] >= 0) k0.potionsMagicka = g_forcePot[1]; if (g_forcePot[2] >= 0) k0.potionsStamina = g_forcePot[2]; }   // --potion-calib
        for (Ctl& c : F.ctl) { c.potions0 = c.book.kit.potionsHealth + c.book.kit.potionsMagicka + c.book.kit.potionsStamina; c.pot0[0] = c.book.kit.potionsHealth; c.pot0[1] = c.book.kit.potionsMagicka; c.pot0[2] = c.book.kit.potionsStamina; }
        F.interior = fs.interior;
        if (g_stealth > 0) {   // drawn from a stream of their own, so a fight is the same fight in every stealth mode (and mode 0 is main's, draw for draw)
            Rng S; S.g.seed(fs.seed * 0x9E3779B97F4A7C15ULL + 77u);
            F.light = fs.light >= 0 ? fs.light : fs.interior ? S.U(0.15f, 0.6f) : S.P(0.35f) ? 0.2f : 0.9f;
            for (std::size_t i = 0; i < F.ctl.size(); ++i) { const Ctl& c = F.ctl[i]; Body& x = F.b[static_cast<std::size_t>(c.body)]; if (!x.stealthy || c.entryOdds <= 0 || g_stealth < 2) continue;
                const bool free = i == 0 && fs.freeEntry >= 0 ? fs.freeEntry == 1 : S.P(1.0f / static_cast<float>(c.entryOdds));
                // Card C13 AS BUILT (classes/StealthEntry.cpp, Q112): the engine has no lever, so the counted fight changes only how MFO reads a foe
                // with no combat group yet. The lab has no such foe (one group, a record from the first pass), so as built the entry does
                // NOTHING here; the draw above still happens so the stream is the same. --c13 lever = the H10 guarantee, which is not built.
                if (free && !g_t2.c13built) x.cloakUntil = 1e9; }   // the lever: he opens undetected whatever the foes could sense; his first blow ends it
        }
        const int no = fs.occluders >= 0 ? fs.occluders : fs.interior ? G.I(1, 3) : G.I(0, 1);
        for (int i = 0; i < no; ++i) { const float tt = G.U(0.3f, 0.75f); const float ox = G.U(-350, 350), oy = G.U(-350, 350), orad = G.U(60, 140); F.occ.push_back({ cx * tt + ox, cy * tt + oy, orad }); }
        { const float ca = std::cos(ang), sa = std::sin(ang); for (const FightSpec::Cover& k : fs.cover) F.occ.push_back({ k.u * ca - k.v * sa, k.u * sa + k.v * ca, k.r }); }
        if (fs.geoSeed) F.R.g.seed(fs.geoSeed * 0x9E3779B97F4A7C15ULL + 0x5851F42D4C957F2DULL);   // the fight opens on the same random state whatever the subject's build drew
        return F;
    }

    // Strength of the foes relative to the party: sqrt(sum hpEff x sum dps), foes over party.
    inline float StrengthRatio(const Data& D, const Fight& F) {
        float fh = 0, fd = 0, ph = 0, pd = 0;
        for (const Body& b : F.b) { const std::vector<SimSpell>* own = b.ctl >= 0 && F.ctl[static_cast<std::size_t>(b.ctl)].gate.spellsOnClass ? &F.ctl[static_cast<std::size_t>(b.ctl)].build->spells : nullptr;
            const Power p = PowerOf(D, *b.spec, own); if (b.role == Role::Foe) { fh += p.hpEff; fd += p.dps; } else { ph += p.hpEff; pd += p.dps; } }
        return std::sqrt((fh * fd) / std::max(1.0f, ph * pd));
    }

    // A random fight of one bracket: real encounters of the list at the party's level bucket, re-drawn until the
    // strength ratio lands in the bracket (the closest draw is used when none does: counted in `forced`).
    // --class-kit: the subject knows the spells his class's own default rows name. OFF by default: main's fights draw his book at
    // random, and the headline J must stay comparable with main. The class playstyle report runs with it ON.
    inline bool g_classKit = false;
    inline Fight RandomFight(const Data& D, const SimClass& cls, int rank, const Bracket& br, Rng& R, const std::vector<SimClass>& t1, bool* forced = nullptr) {
        FightSpec fs; fs.cls = &cls; fs.rank = rank; fs.level = R.I(12, 45); fs.seed = R.g();
        if (g_classKit) fs.ensure = cls.ensure;
        fs.playerFam = static_cast<Cls>(R.I(0, 2));
        for (int i = R.I(0, 2); i > 0; --i) fs.others.push_back(&t1[static_cast<std::size_t>(R.I(0, 2))]);
        fs.worn = R.I(0, 3) == 0 ? 0.45f : 0.0f; fs.foeDist = R.U(900, 2200);
        int bk = 0; for (std::size_t i = 0; i < D.buckets.size(); ++i) if (std::abs(D.buckets[i] - fs.level) < std::abs(D.buckets[static_cast<std::size_t>(bk)] - fs.level)) bk = static_cast<int>(i);
        Fight best; float bestErr = 1e9f; bool ok = false;
        for (int tries = 0; tries < 80 && !ok; ++tries) {
            const Encounter& e = D.encounters[static_cast<std::size_t>(R.I(0, static_cast<int>(D.encounters.size()) - 1))];
            if (static_cast<std::size_t>(bk) >= e.byBucket.size() || e.byBucket[static_cast<std::size_t>(bk)].empty()) continue;
            const auto& g = e.byBucket[static_cast<std::size_t>(bk)];
            FightSpec f2 = fs; f2.interior = e.interior; f2.foes.clear();
            for (const EncMember& m : g) f2.foes.push_back({ m.spec, { m.dx, m.dy } });
            Fight F = BuildFight(D, f2, t1);
            const float ratio = StrengthRatio(D, F);
            bool boss = false; if (br.boss) { float tot = 0, mx = 0; int n = 0; for (const Body& b : F.b) if (b.role == Role::Foe) { tot += b.c.hpMax; mx = std::max(mx, b.c.hpMax); ++n; } boss = n >= 3 && mx >= 0.5f * tot; }
            const float err = ratio < br.lo ? br.lo - ratio : ratio > br.hi ? ratio - br.hi : 0.0f;
            if (err == 0.0f && (!br.boss || boss)) { best = std::move(F); ok = true; }
            else if (err + (br.boss && !boss ? 0.5f : 0.0f) < bestErr) { bestErr = err + (br.boss && !boss ? 0.5f : 0.0f); best = std::move(F); }
        }
        if (forced) *forced = !ok;
        return best;
    }

}
