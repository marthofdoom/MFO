#pragma once
// iaus_sim/Oracle.h -- the EXPERT ORACLE: the "skilled human player" upper bound IAUS is measured against. HOST-ONLY.
//
// A rollout search with full information and no time budget over the same world. At each decision of a free
// follower every option is tried in a copy of the fight, held for a commitment (as a person holds a choice), rolled
// forward with the base policy for everyone over random streams shared by all options, and scored by Utility().
// The best option is taken. A written ruling acts as a constraint: an option a ruling forbids is never tried.
//
// THE STANDING TUNING RULE (marth 2026-11-17, verbatim): "as a standing tuning rule the Oracle gets as many tries as it
// wants for the best class based decisions. The tuning then follows the Oracle as its guide. The Oracle only has the
// classes abilities at its disposal". And the goal: "The goal is that a classes tune will be the actual best possible
// class behavior". What that means here (iaus-sim-v3.md section 14):
//   NO QUALITY CAP   every candidate the generator offers is an option (no option limit); he decides at every decision
//                    point the game has (each time the pump serves him and his body is free: OracleCfg::every = 0);
//                    a rollout runs to the fight's end (H = Fight::maxSec); the streams per option are not a fixed K:
//                    they run until the best option is STATISTICALLY SEPARATED from every other (the race in OraclePolicy::Serve) or the
//                    safety ceiling is hit (OracleCfg::ceiling, reported with how often it binds).
//   CLASS-BOUND      every option, the extended set included, must be an ability of HIS CLASS (ClassRefuses: the ONE
//                    reader of that rule) and may not break a written ruling of the class (RulingRefuses, Playstyles.h).
//                    Every refusal is counted by reason (OracleStats::refused, the ORCREFUSED lines).
//
// OPTIONS = EVERY candidate of the catalogue (Generate) + every posture + "let his own AI run" + THE EXTENDED ACTION
// SET (Ext below): what a follower of that class can physically do that the catalogue has no family for yet (scrolls,
// deliberate repositioning, block, bash). An oracle limited to the catalogue would hide exactly those gaps.
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
    // --consumables (tune 2.11.1): a buff potion or a food used up is charged its gold value at J PER GOLD where gold buys the most J: the
    // first restore potion's worth over the buy price of the cheapest restore potion of that kind (RestoreHealth01 17 g, RestoreMagicka01 /
    // RestoreStamina01 20 g at Speech 15: x2.25, Logistics.h QuoteFor). The SAME number as Logistics.h GoldJ (a LOGI-CHECK pins them equal).
    inline double GoldJOf(const PotWorth* w) {
        if (!w) return 0.0; const double buy[3] = { double(std::lround(17 * 2.25f)), double(std::lround(20 * 2.25f)), double(std::lround(20 * 2.25f)) }; double best = 0;
        for (int t = 0; t < 3; ++t) best = std::max(best, double(w->m[static_cast<std::size_t>(t)][0]) / buy[t]);
        return best;
    }
    // Can he spend magicka at all: a class spell in his book that his gate lets him cast (Generate.cpp: on-class or from a mastered class;
    // an unresolved gate gates nothing). A scroll and a staff cost none. Utility's "magicka left" term is 1 (full) for anyone else: before
    // review F3 of 11-17 it paid a Knight for a full pool he cannot use, and his magicka potion (worth 0 in PotionWorth.h) was free utility.
    [[nodiscard]] inline bool Casts(const Ctl& c) {
        if (c.gate.resolved && !(c.gate.spellsOnClass || c.gate.spellsFromMastered)) return false;
        for (const SpellEntry& e : c.book.entries) if (e.source == SpellSource::Spell && e.cost > 0.0f) return true;
        return false;
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
            const double mpf = x.c.mpMax > 0 && Casts(c) ? x.c.mp / x.c.mpMax : 1.0; mp += mpf; /* review F3 (11-17): magicka left is a resource only for a follower who can spend it */ if (chargePotions) { cost += PotionCost(c); if (g_consumables) cost += GoldJOf(c.worth) * c.consumedGold; }
            res += 0.5 * (x.c.mpMax > 0 ? x.c.mp / x.c.mpMax : 1.0) + 0.5 * (c.potions0 > 0 ? double(pots) / c.potions0 : 1.0); }   // the legacy rate keeps its own (raw) magicka term: it exists for comparisons with old numbers only
        const double resTerm = !g_utilResources ? 5.0 : g_potLegacy ? 5.0 * res / nf : 2.5 * mp / nf + 2.5 - cost;
        return 50.0 * removed + (win ? 15.0 * (1.0 - std::min(1.0, F.now / F.maxSec)) : 0.0) + 15.0 * pl.c.hp / std::max(1.0f, pl.c.hpMax)
             + 10.0 * (1.0 - std::min(1.0, double(F.t.partyDowns) / nf)) + 5.0 * partyHp / nf + resTerm;
    }

    // ── the extended action set ─────────────────────────────────────────────────────────────────────────
    enum class Ext : std::uint8_t { None = 0, StepBack, SprintBack, ToPlayer, ToHealer, GetSight, Block, Bash, Scroll, Hide, Flee, ClearLine, kCount };
    inline const char* Name(Ext e) {
        static const char* n[] = { "", "ext:step_back", "ext:sprint_back", "ext:to_player", "ext:to_healer", "ext:get_sight", "ext:block", "ext:bash", "ext:scroll", "ext:hide", "ext:flee", "ext:clear_line" };
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

    // A family's own cooldown slot (Core.h CooldownId: family, posture, form; never the target), and whether it has run out. The extended
    // block and bash write and read the SAME slot as the catalogue's Block and Bash candidates (marth 11-17: "Bash/block cooldowns bind").
    [[nodiscard]] inline Key FamilyKey(Family f) { Candidate k; k.family = f; return KeyOf(k); }
    [[nodiscard]] inline bool Cooled(const Fight& F, const Ctl& c, Family f, float sec) {
        const auto it = c.firedAt.find(CooldownId(FamilyKey(f))); return it == c.firedAt.end() || F.now - it->second >= static_cast<double>(sec); }
    [[nodiscard]] inline float BlockCooldown(const Ctl& c) { return c.gate.blockCooldownSec > 0.0f ? c.gate.blockCooldownSec : std::max(0.0f, ActiveTuning()->blockCooldownSec); }   // Generate.cpp: the row's, else the tuning's

    // What the extended set offers this follower right now. EVERY target an action can take is offered (review F5 of 11-17: no
    // "nearest foe only", no "strongest foe only", no "first healer only"). What remains here is physics, never a choice made for him:
    //   a step or a sprint away needs a foe within 900 units to step away from; a move to the player or a healer needs him to be away from
    //   them (350 / 300 units); a guard or a bash needs the blade in hand, a bash a foe inside his reach and 20 stamina (Policy.h Fire);
    //   a scroll needs a target it can work on that he sees, a heal scroll someone who is hurt; get_sight / clear_line act on the foe he
    //   is fighting (they move HIM to a line on HIS target; another target is the Attack candidate's job).
    // THE GAME'S OWN RUNS ARE NOT HERE (marth 11-17: "Hide/flee gates bind"): card G3's hide run and flee are the catalogue's Disengage
    // candidates, generated behind the game's own gates (a sneak class a foe has found; 30% health and threatened). Until 11-17 the oracle
    // had its own ungated copies (Ext::Hide, Ext::Flee: kept in the enum and in Apply for old logs, offered by nothing).
    // DISENGAGING IS HIS TO CHOOSE (marth 11-17: "It may disengage whenever it wants. That's not flee for its purposes"): the sprint away
    // (Ext::SprintBack) has no situation gate. It is still an ability of a class with Flag::kDisengage only (ClassRefuses).
    inline void ExtOptions(const Fight& F, int ci, std::vector<Opt>& out) {
        const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& x = F.b[static_cast<std::size_t>(c.body)];
        bool anyFoe = false; for (const Body& o : F.b) anyFoe |= o.role == Role::Foe && Fight::Up(o);
        if (!anyFoe) return;
        for (const Body& o : F.b) { if (o.role != Role::Foe || !Fight::Up(o) || Fight::Dist(x, o) >= 900.0f) continue;
            out.push_back({ nullptr, Ext::StepBack, -1, o.c.id }); if (x.c.sp > 20.0f) out.push_back({ nullptr, Ext::SprintBack, -1, o.c.id }); }
        if (Fight::Dist(x, F.b[0]) > 350.0f && F.b[0].alive) out.push_back({ nullptr, Ext::ToPlayer, -1, 0 });
        for (const Ctl& o : F.ctl) { if (&o == &c) continue; const Body& ob = F.b[static_cast<std::size_t>(o.body)]; if (!Fight::Up(ob) || Fight::Dist(x, ob) < 300.0f) continue;
            bool heals = false; for (const SimSpell& s : o.build->spells) heals |= s.healOther; if (heals && o.gate.spellsOnClass) out.push_back({ nullptr, Ext::ToHealer, -1, ob.c.id }); }
        const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId);
        if (tg && Fight::Up(*tg) && !F.Sees(x, *tg) && (c.drawn == PostureKind::Bow || c.drawn == PostureKind::Staff || c.gate.spellsOnClass)) out.push_back({ nullptr, Ext::GetSight, -1, tg->c.id });
        // SIM V3 (lof): a friend stands in his line to the target and his hands are on spells: a short side step to a clear line (the catalogue has no family for it: design note N4)
        if (g_v3.lof && tg && Fight::Up(*tg) && F.Sees(x, *tg) && c.gate.spellsOnClass && (c.drawn == PostureKind::None || c.drawn == PostureKind::Staff) && F.LofBlocker(x, *tg, 0.0f)) out.push_back({ nullptr, Ext::ClearLine, -1, tg->c.id });
        const bool blade = c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver;
        if (blade && F.now >= c.blockRestUntil && Cooled(F, c, Family::Block, BlockCooldown(c))) out.push_back({ nullptr, Ext::Block, -1, 0 });   // the guard has no target
        if (blade && x.c.sp >= 20.0f && Cooled(F, c, Family::Bash, std::max(0.0f, ActiveTuning()->bashCooldownSec)))
            for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o) && Fight::Dist(x, o) <= F.Reach(x) * 1.1f) out.push_back({ nullptr, Ext::Bash, -1, o.c.id });
        if (c.gate.scrollUser) for (std::size_t i = 0; i < c.book.entries.size(); ++i) {   // a scroll: on every foe it can work on, every hurt friend, or himself
            const SpellEntry& e = c.book.entries[i]; if (e.source != SpellSource::Scroll || e.count <= 0) continue;
            const SimSpell& s = c.build->spells[i];
            if (s.e.nature == Prune::Nature::Offense || s.ctrl) {
                for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o) && F.Sees(x, o) && F.WillWork(s, o) != Tri::No) out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), o.c.id });
            } else if (s.e.nature == Prune::Nature::Heal) {
                for (const Body& o : F.b) if (o.Party() && o.role != Role::Summon && o.alive && (s.healOther || &o == &x) && o.c.hp < o.c.hpMax) out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), o.c.id });
            } else out.push_back({ nullptr, Ext::Scroll, static_cast<int>(i), x.c.id });
        }
    }

    // ── THE CLASS-BOUND ACTION SET (marth 2026-11-17: "The Oracle only has the classes abilities at its disposal") ─────────
    // THE ONE READER of "is this option an ability of his class". nullptr = yes; else the reason it is refused (a fixed token: the key of
    // OracleStats::refused). The class is the game's own: Ctl::gate is the ClassGate RealClasses.cpp builds from native/classes/
    // ClassTable.cpp (flags, weapon set, default gambit rows), his book is the spell book of that class (Load.h BuildFollower), his kit the
    // class's weapon set (KitChoice). An unresolved gate (no class known) gates nothing, as in the game (Snapshot.h ClassGate::resolved).
    //   catalogue candidate   the generator already holds every family to the gate (Generate.cpp); each line below RE-CHECKS the same bit, so
    //                         a refusal here is a generator leak and is counted, never silently dropped ("cat:" reasons, expected 0)
    //   Ext::Block            only a class with a Block default row (ClassGate::block: Generate.cpp raises a guard for no one else; review F2).
    //                         It IS that row's guard (Policy.h Fire, Family::Block: the Harbinger hold and its rest) under that row's cooldown
    //   Ext::Bash             only with the shield or a melee weapon of the class's weapon set in hand, under Tuning::bashCooldownSec
    //   Ext::SprintBack       only a class with Flag::kDisengage (marth 11-17: he may disengage whenever he wants, as a class that can)
    //   Ext::Scroll           only a scroll user (ruling Q36: ClassGate::scrollUser) and only a scroll he carries
    //   Ext::Hide / Flee      not offered since 11-17 (the catalogue's Disengage candidates are the game's runs behind the game's gates). The
    //                         lines below state who they belong to all the same: hide a sneak class with Flag::kDisengage; flee a class with
    //                         the flag that is no sneak class, tree ON (Generate.cpp `hide` / `flee`; review F4)
    //   potions               no class gate exists in the game (ClassGate has none: every follower drinks what he carries), so none here
    //   REPOSITIONING         Ext::StepBack / ToPlayer / ToHealer / GetSight / ClearLine, and the catalogue's Regroup / GiveGround /
    //                         BackOff / Step: ALWAYS a class ability. Movement is not an ability (coordinator brief 11-17). The class's written
    //                         rulings may still forbid a move (RulingRefuses: kPsNoRetreat, kPsBackOnly, kPsNearPlayer).
    // To add an ability rule: one line here. To add an Ext action: its enum, its ExtOptions offer, its Apply case and its line here.
    [[nodiscard]] inline const char* ClassRefuses(const Fight& F, const Ctl& c, const Opt& o) {
        const ClassGate& g = c.gate; if (!g.resolved) return nullptr;
        const Body& x = F.b[static_cast<std::size_t>(c.body)];
        const bool blade = x.spec->shield || (g.weaponMelee && x.spec->melee.Has());   // the shield, or a melee weapon of the class set
        switch (o.ext) {
        case Ext::None: break;
        case Ext::Block: return !g.block ? "ext:block:class has no Block row" : blade ? nullptr : "ext:block:no shield or class melee weapon";
        case Ext::SprintBack: return g.disengage ? nullptr : "ext:sprint_back:class has no disengage";
        case Ext::Bash:  return blade ? nullptr : "ext:bash:no shield or class melee weapon";
        case Ext::Scroll: return !g.scrollUser ? "ext:scroll:not a scroll user" : (o.idx < 0 || static_cast<std::size_t>(o.idx) >= c.book.entries.size() || c.book.entries[static_cast<std::size_t>(o.idx)].source != SpellSource::Scroll) ? "ext:scroll:not a scroll he carries" : nullptr;
        case Ext::Hide: return !g.disengage ? "ext:hide:class has no disengage" : !(c.sneakClass || g.sneakClass) ? "ext:hide:not a sneak class" : nullptr;
        case Ext::Flee: return !g.disengage ? "ext:flee:class has no disengage" : (!g.treeOn || c.sneakClass || g.sneakClass) ? "ext:flee:the game's flee is a tree-ON class that is no sneak class" : nullptr;
        default: return nullptr;   // repositioning: movement is not an ability
        }
        const Candidate* k = o.k; if (!k) return nullptr;   // idle: his own AI
        switch (k->family) {
        case Family::PowerAttack: return g.powerAttack ? nullptr : "cat:power_attack:not a class row";
        case Family::Taunt:       return g.taunt ? nullptr : "cat:taunt:not a class row";
        case Family::Block:       return g.block ? nullptr : "cat:block:not a class row";
        case Family::Bash:        return blade ? nullptr : "cat:bash:no shield or class melee weapon";
        case Family::Retreat: case Family::Disengage: return g.disengage ? nullptr : "cat:retreat:class has no disengage";
        case Family::Posture: { const PostureKind p = k->f.posture;
            const bool ok = p == PostureKind::Melee || p == PostureKind::Silver ? g.weaponMelee : p == PostureKind::Bow ? (g.weaponBow || g.weaponCrossbow) : p == PostureKind::Staff ? g.weaponStaff : true;
            return ok ? nullptr : "cat:posture:weapon outside the class set"; }   // whatever ClassGate::strict says (G16 is whether IAUS enforces the set; the oracle plays the class): his kit is built from the class set (RealClasses.cpp KitChoice), so expected 0
        case Family::Scroll: case Family::CastOffense: case Family::CastControl: case Family::CastHeal: case Family::CastBuff: case Family::CastSummon: case Family::CastWard: case Family::Exploit: {
            const SpellEntry* e = nullptr; for (const SpellEntry& b : c.book.entries) if (b.form == k->form) e = &b;
            if (!e) return "cat:cast:not in his book";
            if (e->source == SpellSource::Scroll) return g.scrollUser ? nullptr : "cat:scroll:not a scroll user";
            if (e->source == SpellSource::Staff) return nullptr;   // a staff he carries: the kit is the class's weapon set (KitChoice::staff)
            return g.spellsOnClass || g.spellsFromMastered ? nullptr : "cat:cast:not a casting class"; }
        default: return nullptr;   // Attack, Sneak, the drinks, the repositioning families
        }
    }

    // THE CLASS RULINGS AS CONSTRAINTS (Playstyles.h): an option the follower's class forbids is never tried, so the oracle is an
    // expert who plays THAT class. ON for every oracle since the standing rule of 11-17 (it was off unless --class-oracle before);
    // --no-class-rulings is a DIAGNOSTIC only (the results file's oracle kind then says so). nullptr = allowed; else the ruling's token.
    inline bool g_classRulings = true;
    [[nodiscard]] inline const char* RulingRefuses(const Fight& F, const Ctl& c, const Opt& o) {
        const Body& x = F.b[static_cast<std::size_t>(c.body)]; const Candidate* k = o.k; const unsigned fb = c.forbid;
        if (k && k->f.hasArea && !k->f.splashSafe) return "ruling:never splash the party";                                  // every class
        if (k && k->family == Family::Posture && !k->f.classAllowsWeapon) return "ruling:posture the game forbids";   // Generate.cpp's own fact: the strict weapon gate (G16), or "spells stay on spells" (marth 11-13, Mage line only: no melee posture while he can pay for an offense spell)
        const bool toMelee = k && k->family == Family::Posture && (k->f.posture == PostureKind::Melee || k->f.posture == PostureKind::Silver);
        const bool meleeAct = (k && (k->family == Family::PowerAttack || k->family == Family::Bash)) || o.ext == Ext::Bash;
        const bool flee = (k && k->family == Family::Retreat) || o.ext == Ext::Flee || (k && k->family == Family::Disengage && k->f.disengageFlee), back = o.ext == Ext::StepBack || (k && k->family == Family::GiveGround), run = o.ext == Ext::SprintBack,
                   hide = o.ext == Ext::Hide || (k && k->family == Family::Disengage && !k->f.disengageFlee);
        if ((fb & kPsNoMelee) && (toMelee || meleeAct)) {   // the blade only when he has nothing else left
            bool other = c.book.kit.potionsMagicka > 0;
            for (const SpellEntry& e : c.book.entries) other |= e.nature == Prune::Nature::Offense && (e.source == SpellSource::Staff ? e.charged : e.source == SpellSource::Scroll ? e.count > 0 : e.cost <= x.c.mp);
            if (other) return "ruling:kPsNoMelee"; }
        if ((fb & kPsNoMeleeSwap) && toMelee && x.arrows > 0) return "ruling:kPsNoMeleeSwap";
        if ((fb & kPsGoIn) && (toMelee || meleeAct) && !F.GoInOk(c, x)) return "ruling:kPsGoIn";
        if ((fb & kPsNoRetreat) && (flee || back || run || hide)) return "ruling:kPsNoRetreat";
        if ((fb & kPsBackOnly) && (flee || run)) return "ruling:kPsBackOnly";
        if ((fb & kPsNearPlayer) && (flee || back || run) && Fight::Dist(x, F.b[0]) > 900.0f) return "ruling:kPsNearPlayer";
        return nullptr;
    }
    [[nodiscard]] inline bool ClassAllows(const Fight& F, const Ctl& c, const Opt& o) { return RulingRefuses(F, c, o) == nullptr; }

    struct Disagreement {      // one cluster: IAUS picked a, the oracle picked b
        long n = 0; double dU = 0;
        double iFoeHp = 0, oFoeHp = 0, iOnPlayer = 0, oOnPlayer = 0, iCaster = 0, oCaster = 0, iResist = 0, oResist = 0, iRecHp = 0, oRecHp = 0, selfHp = 0, selfMp = 0, foes = 0, sameTarget = 0, iDist = 0, oDist = 0, playerHp = 0, nearDist = 0;
    };

    // THE GAP STAGE'S DUMP (tune_driver.py stage gap, iaus-sim-v2.md 34). g_oracleSubjectOnly (--oracle-subject-only): the oracle plans for
    // the SUBJECT only and his companions play IAUS in both arms, so a class's gap is the gap of his own decisions (the default oracle
    // drives every follower, and a class's gap then carries its T1 companions' gap too). SitRec: one divergence cluster of the subject,
    // keyed "iaus pick|oracle pick|kind|situation" (kind: retarget = the same family on another target or form; ranked = the oracle took a candidate IAUS ranked lower; unranked = a candidate
    // IAUS generated and scored under the floor or vetoed; posture; ext = an action the catalogue has no candidate for; idle).
    inline bool g_oracleSubjectOnly = false;
    struct SitRec { long n = 0; double dU = 0; };
    [[nodiscard]] inline std::string Situation(const Body& x, const Snapshot& sn) {
        float nd = 1e9f; for (const auto& f : sn.foes) nd = std::min(nd, f.dist);
        const float hp = x.c.hp / std::max(1.0f, x.c.hpMax); const std::size_t n = sn.foes.size();
        return std::string("hp:") + (hp < 0.3f ? "lo" : hp < 0.6f ? "mid" : "hi") + ",near:" + (n == 0 ? "none" : nd < 250.0f ? "melee" : nd < 900.0f ? "mid" : "far")
             + ",foes:" + (n <= 1 ? std::to_string(n) : n <= 3 ? "2-3" : "4+");
    }

    // THE SEARCH SETTING. Nothing here is a budget that changes the choice (standing rule above):
    //   every    0 = he decides at EVERY decision point the game has: each time the pump serves him (Diagnostics kPumpMs 133 ms x followers,
    //            World.h TM::pump) with his body free. (It was a 0.4 s throttle: with three followers a lap is 0.399 s, so the throttle skipped
    //            every second lap; with one or two it skipped more.) A value > 0 is a DIAGNOSTIC throttle.
    //   H        how long a rollout runs past the decision. 150 = Fight::maxSec: every fight is over by then, so a rollout always runs to the
    //            fight's END (RunFor reads H >= maxSec as "to the end" whatever maxSec is). --oracle-h below maxSec is a DIAGNOSTIC cut.
    //   nMin, ceiling, pOne   the stopping rule of the race in OraclePolicy::Serve: every option gets nMin shared streams, then the streams double (8, 16, 32, ...) and
    //            after each round an option is dropped once the best option's PAIRED lead over it (the same streams, so the fight's luck
    //            cancels) is above the one-sided Student-t bound at pOne (0.001) for that many streams; he stops when one option is left, or
    //            at the safety ceiling (128 streams an option: iaus-sim-v3.md 14.2 has the measurement behind the number). At the ceiling the
    //            options still standing are within noise of each other over that many paired fights (OracleStats::unresolved is the best's
    //            mean lead there: what a wrong pick could cost); the best mean is taken. MEASURED: the ceiling ends most races, because most
    //            options of a decision differ by less than a fight's luck (the shared seed stops pairing two rollouts once they diverge).
    //   K        0 = the stopping rule. K > 0 (--oracle-k) is a DIAGNOSTIC fixed ceiling of K streams (the CLI checks' one-stream smoke runs,
    //            and the rule before 11-17 was K = 8): never a tuning setting; the results file records it.
    //   commit   how long a rollout HOLDS the option before the base policy takes over again (an attack order, a retreat, his own AI). It is
    //            part of the rollout MODEL, not a budget: the oracle itself is asked again at his next decision point.
    struct OracleCfg { int K = 0, nMin = 8, ceiling = 128; double pOne = 0.001; double H = 150.0, every = 0.0, commit = 2.5; double minDU = 1.0; bool allFollowers = true, ext = true, split = true; };

    // One-sided Student-t critical value at p for n paired samples (df = n - 1): the Cornish-Fisher expansion about the normal quantile
    // (4.76 at n = 8 against the table's 4.785, 3.73 at 16, 3.37 at 32, 3.12 at 256 for p = 0.001). n < 2 has no variance: +infinity.
    [[nodiscard]] inline double TCrit(int n, double p) {
        if (n < 2) return 1e300;
        // the normal quantile of 1 - p
        const double q = std::sqrt(-2.0 * std::log(std::clamp(p, 1e-12, 0.5)));
        const double z = q - (2.515517 + 0.802853 * q + 0.010328 * q * q) / (1.0 + 1.432788 * q + 0.189269 * q * q + 0.001308 * q * q * q);   // Abramowitz-Stegun 26.2.23, |error| < 4.5e-4
        const double d = n - 1, z3 = z * z * z, z5 = z3 * z * z, z7 = z5 * z * z;
        return z + (z3 + z) / (4 * d) + (5 * z5 + 16 * z3 + 3 * z) / (96 * d * d) + (3 * z7 + 19 * z5 + 17 * z3 - 15 * z) / (384 * d * d * d);
    }

    // What the search did (summed over an oracle's decisions; sim.cpp prints it per class: the ORCSTOP and ORCREFUSED lines).
    struct OracleStats {
        long decisions = 0, options = 0, rollouts = 0, streams = 0;   // options offered after the class filter; rollouts run; streams at the stop (summed)
        long bound = 0, standing = 0; double unresolved = 0, unresolvedMax = 0;   // decisions the ceiling ended with more than one option standing; how many stood; the best's lead over the best other standing (summed, and the largest)
        std::map<std::string, long> refused;   // option offers refused, by reason (ClassRefuses / RulingRefuses token)
        void Merge(const OracleStats& o) { decisions += o.decisions; options += o.options; rollouts += o.rollouts; streams += o.streams; bound += o.bound; standing += o.standing; unresolved += o.unresolved; unresolvedMax = std::max(unresolvedMax, o.unresolvedMax);
            for (const auto& kv : o.refused) refused[kv.first] += kv.second; }
    };

    // The rollout policy inside the search: IAUS, except that a follower holds an option he was given until commitUntil.
    struct RolloutPolicy : Policy {
        IausPolicy iaus;
        void Serve(Fight& F, int ci) override { if (ServiceRun(F, ci)) return; if (F.now < F.ctl[static_cast<std::size_t>(ci)].commitUntil) return; iaus.Serve(F, ci); }   // card G3's run is serviced every lap, committed or not
    };

    struct OraclePolicy : Policy {
        OracleCfg cfg;
        IausPolicy iaus;                       // followers the oracle does not drive, and IAUS's own pick for the log
        std::function<bool(const Fight&, const Ctl&, const Opt&)> allowed;   // extra constraints of the caller (nullptr = none); the class rulings are ClassAllows
        OracleStats st;
        // The class filter (the abilities, then the written rulings), counted by reason; then the caller's own constraint.
        [[nodiscard]] bool Ok(const Fight& F, const Ctl& c, const Opt& o) {
            const char* why = ClassRefuses(F, c, o); if (!why && g_classRulings) why = RulingRefuses(F, c, o);
            if (why) { ++st.refused[why]; if (F.trace) std::fprintf(F.trace, "%7.2f  ORACLE REFUSED %s: %s (%s)\n", F.now, F.b[static_cast<std::size_t>(c.body)].c.name.c_str(), o.Label().c_str(), why); return false; }
            return !allowed || allowed(F, c, o); }
        std::map<std::string, Disagreement>* log = nullptr;
        std::vector<double> last;
        long decisions = 0, disagreed = 0; double dUSum = 0;
        std::map<std::string, SitRec>* sit = nullptr;          // --gap-dump: the subject's divergences (SitRec above)
        std::array<long, kFamilies>* gen = nullptr;            // --gap-dump: candidates his generator offered per family, summed over the oracle's decisions for him
        long subjDec = 0;                                      // --gap-dump: the oracle's decisions for the subject

        static void RunFor(Fight& G, Policy& P, double until) {   // until < 0: to the fight's end
            while (!G.over && (until < 0 || G.now < until)) {
                if (G.now >= G.nextPump && !G.ctl.empty()) { G.nextPump += TM::pump; P.Serve(G, G.rr); G.rr = (G.rr + 1) % static_cast<int>(G.ctl.size()); }
                G.StepWorld();
            }
        }
        // Apply an option for follower ci in G. false = it cannot be done now.
        static bool Apply(Fight& G, int ci, const Opt& o, double commit, double every = 0.0) {
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
            case Ext::ClearLine: { const Body* tg = G.Find(o.target); if (!tg) return false;   // the nearest point that sees him with no friend in the corridor
                for (float r : { 120.0f, 200.0f, 320.0f }) for (int a = 0; a < 12; ++a) { const float ang = 0.5236f * static_cast<float>(a), gx = x.c.x + r * std::cos(ang), gy = x.c.y + r * std::sin(ang);
                    if (G.Standable(gx, gy) && SeesFrom(G, gx, gy, *tg) && G.LofClearFrom(x, gx, gy, *tg)) return move(gx, gy, 1.2, false); }
                return false; }
            case Ext::Hide: case Ext::Flee: { const bool fl = o.ext == Ext::Flee; float gx = 0, gy = 0; if (c.runPhase != 0 || !G.ConcealPick(x, c, fl, gx, gy)) return false;
                c.pick.valid = true; c.pick.flee = fl; c.pick.kind = fl ? 1 : 0; c.pick.x = gx; c.pick.y = gy; if (!BeginRun(G, ci, fl, true)) return false; c.commitUntil = G.now + (fl ? 2.0 : 0.3); return true; }
            case Ext::Block: {   // the class's own guard, raised when HE wants it (the generator offers it only against a coming swing or on the guard line). Fire is the game's hold: its length, its rest (Policy.h Family::Block)
                if (held || !Cooled(G, c, Family::Block, BlockCooldown(c))) return false;
                Candidate bk; bk.family = Family::Block; if (Fire(G, ci, bk) != Fired::Yes) return false;
                c.firedAt[CooldownId(KeyOf(bk))] = G.now; c.commitUntil = x.actEnd; return true; }
            case Ext::Bash: { Body* tg = G.Find(o.target); if (held || !tg || !Fight::Up(*tg) || x.c.sp < 20.0f || Fight::Dist(x, *tg) > G.Reach(x) * 1.1f || !Cooled(G, c, Family::Bash, std::max(0.0f, ActiveTuning()->bashCooldownSec))) return false;
                G.StartSwing(x, *tg, Act::Bash, true); c.firedAt[CooldownId(FamilyKey(Family::Bash))] = G.now; c.commitUntil = x.actEnd; return true; }
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
            if (k->family == Family::Sneak && every > 0.0) x.holdUntil = std::max(x.holdUntil, G.now + every + 0.05);   // a throttled oracle (DIAGNOSTIC): its hold lasts until its next decision. Unthrottled, Fire's own hold (one lap) is exactly that
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
            // THE OPTIONS: EVERY candidate the generator offers (IAUS's ranked list first, so a tie keeps IAUS's order), every posture, the
            // extended set, idle. No option limit (marth 11-17). Each passes the class filter (Ok) or is counted as refused.
            const auto& acts = sv.g.actions; std::vector<Opt> opt;
            std::vector<const Candidate*> seen;   // offered once each (the ranked list and the full list overlap), so a refusal is counted once
            auto add = [&](const Candidate* k) { Opt o; o.k = k;
                if (std::find(seen.begin(), seen.end(), k) != seen.end()) return; seen.push_back(k);
                if (k->f.hardCooldownSec > 0.0f && k->f.hardCooldownLeft > 0.0f) return;   /* rulings Q7 / Q69 / Q73: a hard cooldown (the power attack's, the taunt's 30 s) binds a person too */
                for (const Opt& e : opt) if (e.Same(o)) return; if (!Ok(F, c, o)) return; opt.push_back(o); };
            const Candidate* iausPick = nullptr;
            if (sv.d.verdict != Verdict::Idle && !sv.d.order.empty() && sv.d.order[0].idx < acts.size()) iausPick = &acts[sv.d.order[0].idx];
            for (const Scored& r : sv.d.ranked) if (r.idx < acts.size()) add(&acts[r.idx]);
            for (const Candidate& k : acts) add(&k);
            for (const Candidate& k : sv.g.postures) if (k.f.posture != c.drawn) { Opt o; o.k = &k; if (Ok(F, c, o)) opt.push_back(o); }
            if (cfg.ext) { std::vector<Opt> ex; ExtOptions(F, ci, ex); for (const Opt& o : ex) if (Ok(F, c, o)) opt.push_back(o); }
            opt.push_back(Opt{});   // idle: his own AI
            // EVALUATE: the race (OracleCfg). Streams are shared by all options (stream k has one seed for every option), so two options are
            // compared on the same luck and the test is on their PAIRED difference.
            const std::size_t N = opt.size(); std::vector<std::vector<double>> U(N); std::vector<char> alive(N, 1); std::vector<double> u(N, -1e9);
            const std::uint64_t s0 = F.R.g(); const int ceil = std::max(1, cfg.K > 0 ? cfg.K : cfg.ceiling), nmin = std::clamp(cfg.nMin, 1, ceil);
            const double until = cfg.H >= F.maxSec ? -1.0 : F.now + cfg.H;
            auto lead = [&](std::size_t a, std::size_t b, double* sd) {   // the mean and the sample deviation of U[a] - U[b] over the streams both ran
                const std::size_t m = std::min(U[a].size(), U[b].size()); if (!m) { if (sd) *sd = 0; return 0.0; }
                double sum = 0, sq = 0; for (std::size_t k = 0; k < m; ++k) { const double d = U[a][k] - U[b][k]; sum += d; sq += d * d; }
                const double mean = sum / double(m); if (sd) *sd = m > 1 ? std::sqrt(std::max(0.0, (sq - sum * mean) / double(m - 1))) : 0.0; return mean; };
            std::size_t best = N - 1, live = N; int n = 0;
            for (int next = nmin;;) {
                for (int k = n; k < next; ++k) for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue;
                    Fight G = F; G.trace = nullptr; G.R.g.seed(s0 + 7919ULL * static_cast<std::uint64_t>(k)); if (cfg.split) { G.split = true; G.splitSeed = s0 + 7919ULL * static_cast<std::uint64_t>(k); G.xs.clear(); }
                    if (!Apply(G, ci, opt[i], cfg.commit, cfg.every)) { alive[i] = 0; continue; }   // it cannot be done now
                    RolloutPolicy rp; RunFor(G, rp, until); U[i].push_back(Utility(G)); ++st.rollouts; }
                n = next; bool any = false;
                for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue; double sum = 0; for (double v : U[i]) sum += v; u[i] = sum / double(U[i].size()); if (!any || u[i] > u[best]) { best = i; any = true; } }   // the first of equal means stands (IAUS's order)
                const double tc = TCrit(n, cfg.pOne), rn = std::sqrt(double(n)); live = 0;
                for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue; if (i == best) { ++live; continue; }
                    double sd = 0; const double d = lead(best, i, &sd);
                    if (n >= 2 && (sd <= 0.0 ? d >= 0.0 : d > tc * sd / rn)) alive[i] = 0; else ++live; }   // sd 0: the same outcome on every stream (a duplicate of the best, or beaten on every one)
                if (live <= 1 || n >= ceil) break;
                next = std::min(ceil, n * 2);
            }
            st.options += static_cast<long>(N); st.streams += n; ++st.decisions;
            if (live > 1) { ++st.bound; st.standing += static_cast<long>(live); double other = -1e9; for (std::size_t i = 0; i < N; ++i) if (alive[i] && i != best) other = std::max(other, u[i]); st.unresolved += u[best] - other; st.unresolvedMax = std::max(st.unresolvedMax, u[best] - other); }
            // pick: the best; the action he is already on wins when it still stands (not separated from the best) and is within 0.3 U of it (a person does not dither)
            if (c.hasLastKey) for (std::size_t i = 0; i < N; ++i) if (alive[i] && opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == c.lastKey && u[i] >= u[best] - 0.3) { best = i; break; }
            // what IAUS's own pick costs: the oracle's PAIRED lead over it, on the streams both ran (an option dropped early ran fewer)
            auto cost = [&](std::size_t i) { return U[i].empty() ? 0.0 : lead(best, i, nullptr); };
            ++decisions;
            // the disagreement with IAUS's own pick
            std::size_t ii = opt.size() - 1; if (iausPick) for (std::size_t i = 0; i < opt.size(); ++i) if (opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == KeyOf(*iausPick)) { ii = i; break; }
            if (subj && (sit || gen)) {
                ++subjDec;
                if (gen) for (const Candidate& k : acts) ++(*gen)[static_cast<std::size_t>(k.family)];
                const double dU = cost(ii);
                if (sit && ii != best && !alive[ii] && dU >= cfg.minDU) {   // review F1: only a pick the race DROPPED is a divergence; one still standing was not told apart from the oracle's
                    const bool iFound = !iausPick || ii != opt.size() - 1;   // IAUS's pick is among the options (idle is last and has no candidate)
                    const Opt& ob = opt[best]; bool ranked = false;
                    if (ob.k) for (const Scored& r : sv.d.ranked) if (r.idx < acts.size() && &acts[r.idx] == ob.k) { ranked = true; break; }
                    const char* kind = ob.ext != Ext::None ? "ext" : !ob.k ? "idle" : ob.k->family == Family::Posture ? "posture" : iausPick && iausPick->family == ob.k->family ? "retarget" : ranked ? "ranked" : "unranked";
                    const std::string il = iausPick ? std::string(MFO::Iaus::Name(iausPick->family)) + (iFound ? "" : "!") : std::string("idle");
                    SitRec& r = (*sit)[il + "|" + ob.Label() + "|" + kind + "|" + Situation(x, sv.sn)]; ++r.n; r.dU += dU;
                }
            }
            if (log) {
                const double dU = cost(ii);
                if (ii != best && !alive[ii] && dU >= cfg.minDU) {   // review F1, as above
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
            if (F.trace && (!c.hasLastKey || !(c.lastKey == key))) std::fprintf(F.trace, "%7.2f  ORACLE %s: %s%s%s -> %u  U %.2f (IAUS would: %s, U %.2f)  [%zu options, %d streams, %zu standing]\n", F.now, x.c.name.c_str(), o.Label().c_str(), k && !k->formName.empty() ? " " : "", k ? std::string(k->formName).c_str() : "", k ? k->target : o.target, u[best], opt[ii].Label().c_str(), u[ii], N, n, live);
            if (subj) {
                if (o.ext != Ext::None) ++t.extActions;
                if (k) { ++t.fired; if (k->family != Family::Posture) { ++t.hist[static_cast<std::size_t>(k->family)];
                        t.actSec[static_cast<std::size_t>(k->family)] += k->family == Family::Attack ? std::max(cfg.every, double(TM::pump) * double(F.ctl.size())) : k->family == Family::Retreat ? TM::retreatSec : std::max(0.0, x.actEnd - F.now); } else ++t.postureSwaps;
                    if ((k->family == Family::DrinkHealth || k->family == Family::CastHeal) && c.lowSince >= 0 && !c.lowHealed && c.lowHadCand) { c.lowHealed = true; ++t.lowHealed; t.lowHealSecSum += F.now - c.lowSince; } }
                if (!c.hasLastKey || !(c.lastKey == key)) ++t.switches;
            }
            c.lastKey = key; c.hasLastKey = true;
        }
    };

}
