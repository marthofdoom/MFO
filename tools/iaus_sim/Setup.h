#pragma once
// iaus_sim/Setup.h -- who is in a fight: the class cells, the party, the foes from the list's real encounters
// (or the fixture groups), and the relative-strength brackets. HOST-ONLY.
#include "iaus_sim/Table.h"

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
        std::vector<GRow> rows;  // the class's default gambit rows (table mode); applied = false for T1 rows kept as data
        std::string package = "current", defaults = "current";   // ids of the package / default rows in use (results file)
        std::string playstyle;   // the human expert playstyle the oracle and the personality metric are measured against
    };

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

    // The game's own 35 class rows (RealClasses.cpp, built from native/classes/ClassTable.cpp). mechProfiles: the I8 mechanical profile.
    std::vector<SimClass> RealClasses(bool mechProfiles);
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
        float foeSpread = 250;
        std::vector<std::string> ensure, ensureOthers;      // spells the subject / the other followers must know (Load.h SpellIs)
        bool  onSubject = false;                            // every foe starts on the subject
        float targetRatio = 0;                              // > 0: the foes' health and damage are scaled so StrengthRatio lands here (a scenario must be winnable by good play on every list)
    };

    inline void AddFollower(Fight& F, const Data& D, const SimClass& sc, int level, int rank, float x, float y, Rng& R, float worn, const std::vector<std::string>* ensure = nullptr) {
        auto fb = std::make_shared<FollowerBuild>(BuildFollower(D, sc.fam, sc.kit, sc.gate, level, R, true, ensure));
        auto spec = std::make_shared<ActorSpec>(fb->spec); spec->name = F.ctl.empty() ? "Follower" : "Follower" + std::to_string(F.ctl.size() + 1);
        Body& b = F.Add(Role::Follower, spec, x, y); b.ctl = static_cast<int>(F.ctl.size()); b.arrows = fb->book.kit.arrows + fb->book.kit.bolts;
        if (worn > 0) { b.c.hp *= R.U(1.0f - worn, 1.0f); b.c.mp *= R.U(1.0f - worn, 1.0f); }
        Ctl c; c.body = static_cast<int>(F.b.size()) - 1; c.fam = sc.fam; c.gate = sc.gate; c.profile = sc.profile; if (ProfileHook()) ProfileHook()(c.profile);
        { bool anyApplied = false; for (const GRow& r : sc.rows) anyApplied |= r.applied; for (const GRow& r : sc.rows) if (r.applied || !anyApplied) c.rows.push_back(r); }   /* T1: its data-only rows stand in for the kit of today */
        c.build = fb; c.book = fb->book;
        c.active.resize(fb->spells.size()); c.summonUntil.assign(fb->spells.size(), 0.0); c.rank = static_cast<std::uint8_t>(rank); c.seed = R.g();
        c.window = 1.5 * (0.88 + 0.24 * R.U(0.0f, 1.0f));   // g_suppressWindow x (0.88 + 0.24 Temperament), Engine::MakeTick
        c.drawn = sc.fam == Cls::Ranged && fb->spec.ranged.Has() ? PostureKind::Bow : sc.fam == Cls::Mage ? PostureKind::None : fb->spec.melee.Has() ? PostureKind::Melee : PostureKind::None;
        c.potH = fb->potH; c.potM = fb->potM; c.potS = fb->potS;
        F.ctl.push_back(std::move(c));
    }

    inline void TagFoe(const Data& D, Body& f) {
        bool healer = false, caster = false;
        for (int si : f.spec->spells) { const SimSpell& s = D.foeSpells[static_cast<std::size_t>(si)]; healer |= s.healOther; caster |= s.e.nature == Prune::Nature::Offense; }
        f.tag = healer ? "healer" : caster ? "caster" : (f.spec->ranged.Has() && (!f.spec->melee.Has() || f.spec->st.wRanged > f.spec->st.wMelee)) ? "archer" : "grunt";
    }

    inline float StrengthRatio(const Data& D, const Fight& F);

    inline Fight BuildFight(const Data& D, const FightSpec& fs, const std::vector<SimClass>& t1) {
        Fight F; F.D = &D; F.R.g.seed(fs.seed); Rng& R = F.R;
        // the player model: a T1 kit of his own, run by the same engine-AI model (no IAUS)
        const SimClass& pc = t1[static_cast<std::size_t>(fs.playerFam)];
        { auto pb = BuildFollower(D, pc.fam, pc.kit, pc.gate, fs.level, R); auto ps = std::make_shared<ActorSpec>(pb.spec); ps->name = "Player"; ps->spells.clear();
          Body& p = F.Add(Role::Player, ps, 0, 0); p.arrows = 80; p.potions = 4; if (fs.worn > 0) p.c.hp *= R.U(1.0f - fs.worn * 0.5f, 1.0f); }
        auto place = [&](Cls f) { return std::pair<float, float>{ f == Cls::Melee ? 220.0f : f == Cls::Ranged ? -300.0f : -250.0f, R.U(-220, 220) }; };
        { auto [x, y] = place(fs.cls->fam); AddFollower(F, D, *fs.cls, fs.level, fs.rank, x, y, R, fs.worn, fs.ensure.empty() ? nullptr : &fs.ensure); }
        for (const SimClass* o : fs.others) { auto [x, y] = place(o->fam); AddFollower(F, D, *o, fs.level, 3, x, y, R, fs.worn, fs.ensureOthers.empty() ? nullptr : &fs.ensureOthers); }
        const float ang = R.U(-0.4f, 0.4f), cx = fs.foeDist * std::cos(ang), cy = fs.foeDist * std::sin(ang);
        std::size_t ti = 0;
        auto addFoe = [&](std::shared_ptr<const ActorSpec> sp, float dx, float dy) {
            Body& f = F.Add(Role::Foe, std::move(sp), cx + std::clamp(dx, -700.0f, 700.0f), cy + std::clamp(dy, -700.0f, 700.0f));
            TagFoe(D, f); if (ti < fs.foeTags.size() && !fs.foeTags[ti].empty()) f.tag = fs.foeTags[ti]; ++ti;
            if (fs.foesOnPlayer) { f.c.targetId = 2; f.retargetAt = 6.0; }
            if (fs.onSubject) { f.c.targetId = F.b[static_cast<std::size_t>(F.ctl[0].body)].c.id; f.retargetAt = 8.0; }
        };
        for (const auto& m : fs.foes) addFoe(std::shared_ptr<const ActorSpec>(std::shared_ptr<const ActorSpec>{}, &D.specs[static_cast<std::size_t>(m.first)]), m.second.first, m.second.second);
        for (const auto& s : fs.customFoes) addFoe(s, R.U(-fs.foeSpread, fs.foeSpread), R.U(-fs.foeSpread, fs.foeSpread));
        for (std::size_t i = 0; i < fs.late.size(); ++i) F.late.push_back({ fs.late[i].first, fs.late[i].second, cx + R.U(-200, 200), cy + R.U(-200, 200), i < fs.lateTags.size() ? fs.lateTags[i] : std::string() });
        if (fs.targetRatio > 0) {   // late foes count toward the strength as if present
            Fight T = F; for (const Spawn& s : T.late) T.Add(Role::Foe, s.spec, s.x, s.y);
            const float k = std::clamp(fs.targetRatio / std::max(0.01f, StrengthRatio(D, T)), 0.05f, 20.0f);
            for (Body& x : F.b) if (x.role == Role::Foe) { auto sp = std::make_shared<ActorSpec>(*x.spec); sp->hp *= k; x.spec = sp; x.c.hpMax *= k; x.c.hp *= k; x.dmgScale = k; }
            for (Spawn& s : F.late) { auto sp = std::make_shared<ActorSpec>(*s.spec); sp->hp *= k; s.spec = sp; s.dmgScale = k; }
        }
        for (const Body& x : F.b) if (x.role == Role::Foe) { F.foeHpTotal += x.c.hpMax; ++F.t.foesFaced; }
        F.t.foesFaced += static_cast<long>(F.late.size());
        for (const Spawn& s : F.late) F.foeHpTotal += s.spec->hp;
        F.b[0].c.hp *= fs.playerHp; F.b[0].potions = fs.playerPotions;
        { Body& s0 = F.b[static_cast<std::size_t>(F.ctl[0].body)]; s0.c.hp *= fs.subjectHp; s0.c.mp *= fs.subjectMp; if (fs.subjectPotionsH >= 0) F.ctl[0].book.kit.potionsHealth = fs.subjectPotionsH; }
        for (Ctl& c : F.ctl) c.potions0 = c.book.kit.potionsHealth + c.book.kit.potionsMagicka + c.book.kit.potionsStamina;
        const int no = fs.occluders >= 0 ? fs.occluders : fs.interior ? R.I(1, 3) : R.I(0, 1);
        for (int i = 0; i < no; ++i) { const float tt = R.U(0.3f, 0.75f); F.occ.push_back({ cx * tt + R.U(-350, 350), cy * tt + R.U(-350, 350), R.U(60, 140) }); }
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
    inline Fight RandomFight(const Data& D, const SimClass& cls, int rank, const Bracket& br, Rng& R, const std::vector<SimClass>& t1, bool* forced = nullptr) {
        FightSpec fs; fs.cls = &cls; fs.rank = rank; fs.level = R.I(12, 45); fs.seed = R.g();
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
