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
#include "iaus/MoveOrder.h"

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
    // S5-C: an ORDER the oracle may give him (mode x place), before the class filter. kinds: what the place is good for (0 = his own place), away: why the place itself is out.
    struct OrderAsk { MV::MoveMode mode = MV::MoveMode::Free; std::uint32_t kinds = 0; bool leash = false, taken = false, refused = false, noWay = false; };
    inline constexpr const char* kModeName[] = { "free", "stand", "goto", "kite", "disengage", "path" };
    // --move-caps: the modes the mover serves. Two modes the executor carries out alike (StrictWorld.h reads Disengage, and GoTo's free aim under the disengage cap,
    // and nothing else of a mode) are ONE option at a place: raced twice they would be twins on every stream.
    [[nodiscard]] inline bool ModeServedBy(const MV::RoadCaps& c, MV::MoveMode m) { return m == MV::MoveMode::Kite ? c.kite : m == MV::MoveMode::Disengage ? c.disengage : m != MV::MoveMode::Path && m != MV::MoveMode::kCount; }
    [[nodiscard]] inline int ModeCarried(const MV::RoadCaps& c, MV::MoveMode m) { return m == MV::MoveMode::Free ? 0 : m == MV::MoveMode::Disengage ? 1 : m == MV::MoveMode::GoTo && c.disengage ? 2 : 3; }
    struct Opt {                       // one thing he could do now
        const Candidate* k = nullptr;  // a catalogue candidate (nullptr with ext None = his own AI)
        Ext   ext = Ext::None;
        int   idx = -1;                // Scroll: the book entry
        std::uint32_t target = 0;
        const OrderAsk* order = nullptr;   // S5-C: an order (ClassRefuses / RulingRefuses only: the order race keeps its own list)
        [[nodiscard]] std::string Label() const { if (order) return std::string("order:") + kModeName[static_cast<std::size_t>(order->mode)]; return ext != Ext::None ? std::string(Name(ext)) : k ? std::string(MFO::Iaus::Name(k->family)) : std::string("idle"); }
        [[nodiscard]] bool Same(const Opt& o) const { return ext == o.ext && idx == o.idx && target == o.target && ((!k && !o.k) || (k && o.k && KeyOf(*k) == KeyOf(*o.k))); }
    };

    [[nodiscard]] inline bool SeesFrom(const Fight& F, float ax, float ay, const Body& o, const Body* who = nullptr) {
        if (F.env) return who && EnvSeesFrom(F, *who, ax, ay, o);   // SIM V4
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
    //   an ORDER (S5-C)       a mode the caps do not serve, a place of no kind his class (SpotKindsFor) and the mover (AllowedKinds) both allow, a place past his
    //                         leash, under another body, one trust has refused him, or with no way to it. His own place is Free's and Hold's, with no gate but the caps
    [[nodiscard]] inline const char* OrderRefuses(const Fight& F, const Ctl& c, const OrderAsk& a) {
        static const auto tok = [] { std::array<std::array<std::string, 2>, MFO::Iaus::kSpotKindCount> t; for (std::size_t i = 0; i < t.size(); ++i) { const std::string n = MV::kSpotKinds[i].name; t[i] = { "order:" + n + ":cap off", "order:" + n + ":class has no " + n + " kind" }; } return t; }();
        static const auto mode = [] { std::array<std::string, 6> t; for (std::size_t i = 0; i < t.size(); ++i) t[i] = std::string("order:") + kModeName[i] + ":cap off"; return t; }();
        const MV::RoadCaps& caps = F.move->pic.caps; const std::uint32_t allowed = MV::AllowedKinds(caps, *ActiveTuning()), his = MV::SpotKindsFor(c.gate);
        if (!ModeServedBy(caps, a.mode)) return mode[static_cast<std::size_t>(a.mode)].c_str();
        if (a.kinds && !(a.kinds & allowed & his)) for (std::size_t i = 0; i < tok.size(); ++i) if (a.kinds & (1u << i)) return tok[i][allowed & (1u << i) ? 1 : 0].c_str();
        return a.leash ? "order:place:past his leash" : a.refused ? "order:place:trust has refused it" : a.taken ? "order:place:another body has it" : a.noWay ? "order:place:no way there" : nullptr;
    }
    [[nodiscard]] inline const char* ClassRefuses(const Fight& F, const Ctl& c, const Opt& o) {
        if (o.order) return OrderRefuses(F, c, *o.order);
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
                   rear = o.order && o.order->mode == MV::MoveMode::Kite, off = o.order && o.order->mode == MV::MoveMode::Disengage, hide = o.ext == Ext::Hide || (k && k->family == Family::Disengage && !k->f.disengageFlee);
        if ((fb & kPsNoMelee) && (toMelee || meleeAct)) {   // the blade only when he has nothing else left
            bool other = c.book.kit.potionsMagicka > 0;
            for (const SpellEntry& e : c.book.entries) other |= e.nature == Prune::Nature::Offense && (e.source == SpellSource::Staff ? e.charged : e.source == SpellSource::Scroll ? e.count > 0 : e.cost <= x.c.mp);
            if (other) return "ruling:kPsNoMelee"; }
        if ((fb & kPsNoMeleeSwap) && toMelee && x.arrows > 0) return "ruling:kPsNoMeleeSwap";
        if ((fb & kPsGoIn) && (toMelee || meleeAct) && !F.GoInOk(c, x)) return "ruling:kPsGoIn";
        if ((fb & kPsNoRetreat) && (flee || back || run || hide || rear || off)) return "ruling:kPsNoRetreat";
        if ((fb & kPsBackOnly) && (flee || run || off)) return "ruling:kPsBackOnly";
        if ((fb & kPsNearPlayer) && (flee || back || run || rear || off) && Fight::Dist(x, F.b[0]) > 900.0f) return "ruling:kPsNearPlayer";
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
    struct OracleCfg { int K = 0, nMin = 8, ceiling = 128; double pOne = 0.001; double pair = -1.0;   /* pair: THE COST CUTS below (g_oraclePrune's comment); < 0 = off */ double H = 150.0, every = 0.0, commit = 2.5; double orderSec = 5.0; /* S5-C: an order to a place ends after this, or by a mark (OrderPick) */ double minDU = 1.0; int cuts = 15; /* the order race's cost cuts, OraclePolicy::kCut* */ bool leadRule = true; /* strict road: the default is left on a lead (Choose). false = DIAGNOSTIC, the 0.3 rule of the other roads */ bool allFollowers = true, ext = true, split = true; };

    // One-sided Student-t critical value at p for n paired samples (df = n - 1): the Cornish-Fisher expansion about the normal quantile
    // (4.76 at n = 8 against the table's 4.785, 3.73 at 16, 3.37 at 32, 3.12 at 256 for p = 0.001). n < 2 has no variance: +infinity.
    [[nodiscard]] inline double TCrit(int n, double p) {
        if (n < 2) return 1e300;
        // the normal quantile of 1 - p
        const double q = std::sqrt(-2.0 * std::log(std::clamp(p, 1e-12, 0.5)));
        const double z = q - (2.515517 + 0.802853 * q + 0.010328 * q * q) / (1.0 + 1.432788 * q + 0.189269 * q * q + 0.001308 * q * q * q);   // Abramowitz-Stegun 26.2.23, |error| < 4.5e-4
        const double d = n - 1, z3 = z * z * z, z5 = z3 * z * z, z7 = z5 * z * z;
        double t = z + (z3 + z) / (4 * d) + (5 * z5 + 16 * z3 + 3 * z) / (96 * d * d) + (3 * z7 + 19 * z5 + 17 * z3 - 15 * z) / (384 * d * d * d);
        // UNDER 8 STREAMS (`--oracle-nmin 4`, choice review SEV-5) the four terms fall short (9.3 at 3 degrees of freedom against the table's 10.21 for p 0.001):
        // the fifth term of the same expansion brings it to 9.9. From 8 streams on the value is the one every earlier result was made with, bit for bit.
        if (n < 8) t += (79 * z7 * z * z + 776 * z7 + 1482 * z5 - 1920 * z3 - 945 * z) / (92160 * d * d * d * d);
        return t;
    }

    // What the search did (summed over an oracle's decisions; sim.cpp prints it per class: the ORCSTOP and ORCREFUSED lines).
    struct OracleStats {
        long decisions = 0, options = 0, rollouts = 0, streams = 0;   // options offered after the class filter; rollouts run; streams at the stop (summed)
        long twins = 0;   // options the pair rule retired (OracleCfg::pair): told apart from no standing option by more than its width, or the same outcome as an earlier one on every stream
        long bound = 0, standing = 0; double unresolved = 0, unresolvedMax = 0;   // decisions the ceiling ended with more than one option standing; how many stood; the best's lead over the best other standing (summed, and the largest)
        // S5-C, the order race: windows he was asked at, options, rollouts, wins by mode, wins of a place no action of his was paired with, orders that stood, races the
        // ceiling ended, and per party size the winner's paired lead over the pass's own order (the winner of a race: it reads high)
        long orders = 0, orderOpts = 0, orderRolls = 0, unpaired = 0, orderKept = 0, orderBound = 0, orderSkip = 0, orderClust = 0; std::array<long, 6> modeWin{}, modeIaus{};
        struct Lead { long n = 0, same = 0; double sum = 0; }; std::map<int, Lead> party;
        std::map<std::string, long> refused;   // option offers refused, by reason (ClassRefuses / RulingRefuses token)
        void Merge(const OracleStats& o) { decisions += o.decisions; options += o.options; rollouts += o.rollouts; streams += o.streams; twins += o.twins; bound += o.bound; standing += o.standing; unresolved += o.unresolved; unresolvedMax = std::max(unresolvedMax, o.unresolvedMax);
            orders += o.orders; orderOpts += o.orderOpts; orderRolls += o.orderRolls; unpaired += o.unpaired; orderKept += o.orderKept; orderBound += o.orderBound; orderSkip += o.orderSkip; orderClust += o.orderClust; for (std::size_t i = 0; i < 6; ++i) { modeWin[i] += o.modeWin[i]; modeIaus[i] += o.modeIaus[i]; }
            for (const auto& kv : o.party) { Lead& l = party[kv.first]; l.n += kv.second.n; l.same += kv.second.same; l.sum += kv.second.sum; }
            for (const auto& kv : o.refused) refused[kv.first] += kv.second; }
    };
    // His body is not free for another action now.
    [[nodiscard]] inline bool OracleHeld(const Fight& F, const Ctl& c, const Body& x) { return x.hitAt >= 0 || F.now < x.staggerUntil || F.now < x.ctrlUntil || (x.actIaus && F.now < x.actEnd) || F.now < c.moveUntil; }

    // The rollout policy inside the search: IAUS, except that a follower holds an option he was given until commitUntil.
    // His ranking reaches the movement pass each lap as in the game (IausPolicy::ServeWith calls MoveNoteRanked), but not while he is held to the option: what he is held to
    // was put first when it was applied. An order option has an action too (`first`): it is begun at his first free lap, within a second.
    struct RolloutPolicy : Policy {
        IausPolicy iaus; std::function<bool(Fight&)> first; int firstCi = -1; double firstUntil = 0.0;
        void Serve(Fight& F, int ci) override { if (ServiceRun(F, ci)) return; const Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; if (F.now < c.commitUntil) return;
            if (ci == firstCi && first) { if (F.now > firstUntil) first = nullptr; else if (!OracleHeld(F, c, F.b[static_cast<std::size_t>(c.body)])) { const auto f = std::move(first); first = nullptr; if (Fight::Up(F.b[static_cast<std::size_t>(c.body)]) && f(F)) return; } }
            iaus.Serve(F, ci); }   // card G3's run is serviced every lap, committed or not
    };

    // ── SPELL REGRET, ALTERNATE TABLES, EXACT PRUNING (sim v4 batch C, iaus-sim-v4.md 7.3 and 15) ──────────────────────────────────────────
    // Spell regret: at every decision of the subject where IAUS picked a cast or the oracle's best option is one, the oracle's PAIRED lead of
    // its best castable spell over IAUS's pick, in U, on the streams both ran. As for a gap divergence (review F1) only a pick the race DROPPED
    // has a regret: one still standing was not told apart from the best. Summed per fight (`SPELLFIGHT`), and clustered by IAUS's family, the
    // oracle's family and a CAUSE (`SPELLREG`):
    //   gate    IAUS vetoed the oracle's spell or scored it under the act floor (a veto, a gate, a missing consideration), or IAUS picked
    //           something the class's rulings forbid
    //   sight   ... and the veto was the sight term, or the spell is ranked lower while IAUS reads its target occluded
    //   hand    ... and the veto was the hand term, or the spell is ranked lower with a hand_lost share
    //   fact    ranked lower, and an INPUT is wrong or absent: either spell is unmeasured in his book, or both are offense spells whose
    //           estimate (Facts::expRate) orders them the other way round from what the lab's world does per cast second (TrueSpell)
    //   weight  ranked lower with nothing above: the curves and weights rank it lower (cross family: the family weights)
    // Two diagnostics from the WORLD (never the estimate), for a pick that is a cast: effect per cast second and per magicka, as the share the
    // best castable spell has over the pick (within his pick's family, and across the families that have a number: health removed, health
    // restored, a controlled foe's seconds times that foe's damage rate).
    // Alternate tables (`--regret-alt "<sets>"`, tune_driver.py stage spells): the SAME oracle decision values are read for what another table
    // would have picked on the same snapshot (its own generate and decide, on a copy of the mind as it stood before the base table decided). One
    // oracle fight then prices any number of candidate tables. It is a screen: the rollouts ran under the base table, so an accepted value
    // is measured again as the base table.
    // Exact pruning (`--oracle-prune off` is the diagnostic; `target` is the first cost cut below): an offense spell that another offense spell on the same foe beats or equals on
    // every number the world reads (Dominates) cannot be the best, and is not raced. IAUS's own pick and the alternates' picks always race.
    //
    // THE COST CUTS OF THE RACE (sim v4 batch C, iaus-sim-v4.md 15.7; each is measured there on the same streams as the race it replaces).
    // With a field-size book a caster has 60 to 100 options, most of them offense spells on the same foe that do nearly the same thing, and
    // the race could not tell them apart at the ceiling: it ran every one of them 128 times. Nothing below lowers the ceiling, shortens a
    // rollout, skips a decision point or drops a candidate unseen.
    //   g_oraclePrune 2 (`--oracle-prune target`)   the dominance test reads what the spell does to THIS foe: its damage times the foe's own
    //                    resist factors (World.h SpellHit), so two bolts of different elements are compared on the foe they are thrown at.
    //                    A spell with an area keeps the same-element test (it lands on other foes too). 1 (`on`, the default) is the
    //                    record's numbers with the same element, as before. 0 = off.
    //   OracleCfg::pair (`--oracle-pair <U>`, < 0 = off)   PAIRWISE INDIFFERENCE. After each look an option I is retired when another standing
    //                    option J exists that it is not better than by more than `pair` U, at the race's own p on their paired difference
    //                    (mean(J - I) > t * sd / sqrt(n) - pair), or, at any width, when J comes earlier (IAUS ranks it higher) and
    //                    gave the SAME utility on every stream so far, from kTwinMin streams on (a twin: the cast did the same thing in
    //                    every future tried. Measured: of the options that equal an earlier one on 8 streams, half differ on a later
    //                    one, by under 0.1 U in the mean with one case of 0.5; from 16 streams on no such case was left). The
    //                    race then spends its streams on options that differ. Never retired this way: the best, IAUS's own pick, an
    //                    alternate table's pick, the action he is on, so the gap ledger and the spell regret keep their meaning. The loss
    //                    per decision is at most `pair` per link of a chain; measured, it is a few thousandths of a U (15.7).
    //   OracleCfg::nMin 4 (`--oracle-nmin 4`)   the first look after 4 streams instead of 8. The test at 4 streams is the same test with its
    //                    own (much wider) t; what it buys is that twins and near-twins leave after 4 rollouts instead of 8.
    inline int g_oraclePrune = 1;
    inline constexpr int kTwinMin = 16;
    inline constexpr int kScreenK = 16;           // the order screen's ceiling (kCutScreen)
    inline constexpr float kClusterU = 128.0f;    // two places of one triangle this near are one option (kCutCluster)
    // `--oracle-dump <file>` (DIAGNOSTIC, the proof of the cuts): after each race of the SUBJECT every option that could be applied is run on
    // every stream up to the ceiling (about three times the fight's cost) and the decision is appended as one JSON line: the options (label,
    // target, form, IAUS's pick, the spell's numbers and its factor on the foe) and their utility per stream. A stream's seed does not depend
    // on which options stand, so tools/iaus_sim/oracle_replay.py can replay ANY race rule on the very rollouts the full race saw.
    inline std::FILE* g_oracleDump = nullptr;
    struct AltTable { std::string sets; Tuning t; std::vector<Profile::WeightOverride> w; };
    inline std::vector<AltTable>& Alts() { static std::vector<AltTable> a; return a; }
    [[nodiscard]] inline bool CastFamily(Family f) { return f == Family::CastOffense || f == Family::CastControl || f == Family::CastHeal || f == Family::CastBuff || f == Family::CastSummon || f == Family::CastWard; }
    struct RegRec { long n = 0, nReg = 0, nEps = 0; double regU = 0, epsW = 0, epsX = 0, epm = 0; };
    struct RegretLog {
        std::map<std::string, RegRec> rows;   // "iaus family|oracle family|cause"
        long decisions = 0, castDecisions = 0, pruned = 0; double regU = 0;
        std::vector<double> alt; std::vector<long> altMiss;   // per alternate table: its regret sum, and decisions its pick was not among the options
        void Merge(const RegretLog& o) { decisions += o.decisions; castDecisions += o.castDecisions; pruned += o.pruned; regU += o.regU;
            for (const auto& kv : o.rows) { RegRec& r = rows[kv.first]; r.n += kv.second.n; r.nReg += kv.second.nReg; r.nEps += kv.second.nEps; r.regU += kv.second.regU; r.epsW += kv.second.epsW; r.epsX += kv.second.epsX; r.epm += kv.second.epm; }
            if (alt.size() < o.alt.size()) { alt.resize(o.alt.size(), 0.0); altMiss.resize(o.alt.size(), 0); }
            for (std::size_t i = 0; i < o.alt.size(); ++i) { alt[i] += o.alt[i]; altMiss[i] += o.altMiss[i]; } }
    };
    [[nodiscard]] inline int BookIndex(const Ctl& c, std::uint32_t form) { if (!form) return -1; for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == form) return static_cast<int>(i); return -1; }
    // What one cast does in the LAB'S WORLD, in health points: effect < 0 = this family has no single number here (a buff, a ward, a summon).
    struct SpellTruth { double effect = -1.0, sec = 1.0, mp = 0.0; };
    [[nodiscard]] inline SpellTruth TrueSpell(const Fight& F, const Ctl& c, const Body& x, const Candidate& k) {
        SpellTruth o; const int si = BookIndex(c, k.form); if (si < 0 || static_cast<std::size_t>(si) >= c.build->spells.size()) return o;
        const SimSpell& s = c.build->spells[static_cast<std::size_t>(si)]; const SpellEntry& e = c.book.entries[static_cast<std::size_t>(si)];
        o.sec = static_cast<double>(s.castSec) + TM::castRecover; o.mp = e.cost; const Body* tg = F.Find(k.target);
        auto hit = [&](const Body& d) { const float el = std::clamp(F.ResistOf(d, s.e.resistAv) / 100.0f, -1.0f, 1.0f), mr = std::clamp(F.ResistOf(d, kAvResistMagic) / 100.0f, -1.0f, 1.0f);   // World.h SpellHit
            return static_cast<double>(std::min(std::max(0.0f, d.c.hp), (s.dmg + s.dot * s.dotDur) * x.dmgScale * (1.0f - el) * (1.0f - mr))); };
        if (k.family == Family::CastOffense && tg) { o.effect = hit(*tg);
            if (e.areaUnits > 0.0f) for (const Body& d : F.b) if (&d != tg && d.role == Role::Foe && Fight::Up(d) && Fight::Dist(*tg, d) <= e.areaUnits) o.effect += hit(d); }
        else if (k.family == Family::CastHeal) { const Body& r = tg ? *tg : x; o.effect = std::min(static_cast<double>(s.heal + s.hot * s.hotDur), static_cast<double>(std::max(0.0f, r.c.hpMax - r.c.hp))); }
        else if (k.family == Family::CastControl && tg) { o.effect = 0.0; if (F.WillWork(s, *tg) == Tri::Yes) { Combatant kk = tg->c; FillEstimate(F, *tg, kk); o.effect = static_cast<double>(s.ctrlDur) * kk.hit * kk.hitsPerSec; } }
        return o;
    }
    // A beats or equals B on every number the world reads for an offense cast at one foe (same road, same element, same area, same row).
    // onFoe (g_oraclePrune 2): ka / kb are each spell's factor on the foe both are thrown at (World.h SpellHit: (1 - element resist)(1 - magic
    // resist)), so the element may differ unless the spell has an area. A stealthy caster's cover breaks on a spell that deals damage, so for
    // him the two must agree on dealing any (stealthy).
    [[nodiscard]] inline bool Dominates(const SimSpell& A, const SpellEntry& a, const SimSpell& B, const SpellEntry& b, bool onFoe = false, float ka = 1.0f, float kb = 1.0f, bool stealthy = false) {
        const bool area = a.areaUnits > 0.0f || A.areaKind != 0;
        if (!onFoe || area) { if (a.resistAv != b.resistAv) return false; ka = kb = 1.0f; }
        if (onFoe && stealthy && (A.dmg > 0.0f) != (B.dmg > 0.0f)) return false;
        return a.source == SpellSource::Spell && b.source == SpellSource::Spell && !a.concentration && !b.concentration && a.touch == b.touch && a.selfDelivery == b.selfDelivery
            && A.row == B.row && a.areaUnits == b.areaUnits && A.areaKind == B.areaKind && !A.ctrl && !B.ctrl && a.undeadOnly == b.undeadOnly
            && a.cost <= b.cost && A.castSec <= B.castSec && a.reach >= b.reach && A.dmg * ka >= B.dmg * kb && A.dot * ka >= B.dot * kb && A.dotDur >= B.dotDur && A.mpDmg * ka >= B.mpDmg * kb && A.spDmg * ka >= B.spDmg * kb
            && (A.stagger || !B.stagger) && A.heal >= B.heal && A.armor >= B.armor;
    }

    struct OraclePolicy : Policy {
        OracleCfg cfg;
        IausPolicy iaus;                       // followers the oracle does not drive, and IAUS's own pick for the log
        std::function<bool(const Fight&, const Ctl&, const Opt&)> allowed;   // extra constraints of the caller (nullptr = none); the class rulings are ClassAllows
        OracleStats st;                        // every follower the oracle drives
        OracleStats stSubj;                    // the SUBJECT only (ctl 0): what the per-class ORCSTOP / ORCREFUSED lines print (review F-C of 610e2952: a companion's
                                               // decisions and refusals were merged under the subject's class label)
        // The class filter (the abilities, then the written rulings), counted by reason; then the caller's own constraint.
        [[nodiscard]] bool Ok(const Fight& F, const Ctl& c, const Opt& o) {
            const char* why = ClassRefuses(F, c, o); if (!why && g_classRulings) why = RulingRefuses(F, c, o);
            if (!why && g_v4.summons && o.k && o.k->family == Family::CastSummon) for (std::size_t i = 0; i < c.book.entries.size(); ++i) if (c.book.entries[i].form == o.k->form) { if (F.SummonGate(F.b[static_cast<std::size_t>(c.body)], static_cast<int>(i)) != Fight::kSummonClear) why = "engine:the summon limit"; break; }   // bundle A: he cannot cast what the game refuses
            if (why) { ++st.refused[why]; if (&c == &F.ctl[0]) ++stSubj.refused[why]; if (F.trace) std::fprintf(F.trace, "%7.2f  ORACLE REFUSED %s: %s (%s)\n", F.now, F.b[static_cast<std::size_t>(c.body)].c.name.c_str(), o.Label().c_str(), why); return false; }
            return !allowed || allowed(F, c, o); }
        std::map<std::string, Disagreement>* log = nullptr;
        std::vector<double> last;
        long decisions = 0, disagreed = 0; double dUSum = 0;
        std::map<std::string, SitRec>* sit = nullptr;          // --gap-dump: the subject's divergences (SitRec above)
        std::array<long, kFamilies>* gen = nullptr;            // --gap-dump: candidates his generator offered per family, summed over the oracle's decisions for him
        long subjDec = 0;                                      // --gap-dump: the oracle's decisions for the subject
        RegretLog* reg = nullptr;                              // --spell-regret: the subject's spell regret (RegretLog above)

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
            auto move = [&](float gx, float gy, double sec, bool sprint) { if (G.env && !G.Standable(gx, gy, &x)) return false;   // SIM V4 (review F10): a point that is not floor of his own storey is not an order
                c.goalX = gx; c.goalY = gy; c.moveUntil = G.now + sec; c.sprint = sprint; c.moveFight = false; c.commitUntil = c.moveUntil; return true; };
            switch (o.ext) {
            case Ext::None: break;
            case Ext::StepBack: case Ext::SprintBack: { const Body* n = G.Find(o.target); if (!n) return false; const float dx = x.c.x - n->c.x, dy = x.c.y - n->c.y, l = std::max(1.0f, std::hypot(dx, dy));
                return move(x.c.x + dx / l * 500.0f, x.c.y + dy / l * 500.0f, 2.0, o.ext == Ext::SprintBack); }
            case Ext::ToPlayer: { const Body& p = G.b[0]; return move(p.c.x - 120.0f, p.c.y + (x.c.y > p.c.y ? 100.0f : -100.0f), 3.0, false); }
            case Ext::ToHealer: { const Body* h = G.Find(o.target); if (!h) return false; return move(h->c.x - 100.0f, h->c.y, 3.0, false); }
            case Ext::GetSight: { const Body* tg = G.Find(o.target); if (!tg) return false;
                for (float r : { 250.0f, 450.0f, 700.0f }) for (int a = 0; a < 12; ++a) { const float ang = 0.5236f * static_cast<float>(a), gx = x.c.x + r * std::cos(ang), gy = x.c.y + r * std::sin(ang);
                    if (SeesFrom(G, gx, gy, *tg, &x)) return move(gx, gy, 2.5, false); }
                return false; }
            case Ext::ClearLine: { const Body* tg = G.Find(o.target); if (!tg) return false;   // the nearest point that sees him with no friend in the corridor
                for (float r : { 120.0f, 200.0f, 320.0f }) for (int a = 0; a < 12; ++a) { const float ang = 0.5236f * static_cast<float>(a), gx = x.c.x + r * std::cos(ang), gy = x.c.y + r * std::sin(ang);
                    if (G.Standable(gx, gy, &x) && SeesFrom(G, gx, gy, *tg, &x) && G.LofClearFrom(x, gx, gy, *tg)) return move(gx, gy, 1.2, false); }
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
        // THE RACE, the same for actions and for orders: N options, roll(G, i) applies option i to a stream's copy and plays it out (false = it cannot be done now).
        // Streams are shared by all options (stream k has one seed for every option), so two options are compared on the same luck and the test is on their PAIRED
        // difference.
        // ON THE STRICT ROAD the race's luck is a hash of the fight, the time and the size of the race, never a draw of the world's: the worlds of on / rank / off / IAUS
        // share their luck until they differ, and every stream has block, carry and aversion draws of its own (review findings 1 and 4). Elsewhere as before, to the bit.
        enum : int { kCutPair = 1, kCutScreen = 2, kCutSkip = 4, kCutCluster = 8 };
        static constexpr double kTieU = 0.3;   // a lead under this is a tie. On the strict road also the pair rule's width
        [[nodiscard]] bool cut(int c) const { return g_road == Road::Strict && (cfg.cuts & c); }
        [[nodiscard]] double pairW() const { return cfg.pair >= 0.0 && cut(kCutPair) ? std::max(cfg.pair, kTieU) : cfg.pair; }
        [[nodiscard]] bool leads() const { return g_road == Road::Strict && cfg.leadRule; }
        [[nodiscard]] static std::uint64_t RaceSeed(Fight& F, std::size_t N, int ci, bool order) { if (g_road != Road::Strict) return F.R.g(); return Fight::Mix64(F.raceSeed ^ Fight::Mix64(static_cast<std::uint64_t>(F.now * 1000.0 + 0.5) * 31 + N * 7) ^ Fight::Mix64(static_cast<std::uint64_t>(ci * 2 + (order ? 1 : 0)) + 0xC2B2AE3D27D4EB4FULL)); }
        struct Race { std::vector<std::vector<double>> U; std::vector<char> alive; std::vector<double> u; std::size_t best = 0, live = 0; int n = 0, ceil = 0; long retired = 0; std::uint64_t s0 = 0; };
        [[nodiscard]] double until(const Fight& F) const { return cfg.H >= F.maxSec ? -1.0 : F.now + cfg.H; }
        [[nodiscard]] Fight Stream(const Fight& F, std::uint64_t s0, int k) const { Fight G = F; G.trace = nullptr; G.R.g.seed(s0 + 7919ULL * static_cast<std::uint64_t>(k)); if (g_road == Road::Strict && !g_moveFault->oracleAv) { G.avSeed = Fight::Mix64(s0 + 13ULL * static_cast<std::uint64_t>(k)); G.V.g.seed(Fight::Mix64(G.avSeed + 1)); StrictRedraw(G); } if (cfg.split) { G.split = true; G.splitSeed = s0 + 7919ULL * static_cast<std::uint64_t>(k); G.xs.clear(); } return G; }
        static double Lead(const std::vector<std::vector<double>>& U, std::size_t a, std::size_t b, double* sd) {   // the mean and the sample deviation of U[a] - U[b] over the streams both ran
            const std::size_t m = std::min(U[a].size(), U[b].size()); if (!m) { if (sd) *sd = 0; return 0.0; }
            double sum = 0, sq = 0; for (std::size_t k = 0; k < m; ++k) { const double d = U[a][k] - U[b][k]; sum += d; sq += d * d; }
            const double mean = sum / double(m); if (sd) *sd = m > 1 ? std::sqrt(std::max(0.0, (sq - sum * mean) / double(m - 1))) : 0.0; return mean; }
        // in: the options raced (nullptr = all). memo: utilities an earlier race of the same seed has paid for. cap: a ceiling of its own.
        template <class Roll> Race RunRace(Fight& F, std::size_t N, std::uint64_t seed, const std::vector<char>& guard, const Roll& roll, long& rollouts, const std::vector<char>* in = nullptr, std::vector<std::vector<double>>* memo = nullptr, int cap = 0) {
            Race r; r.U.resize(N); r.alive.assign(N, 1); r.u.assign(N, -1e9); auto& U = r.U; auto& alive = r.alive; auto& u = r.u; long& retired = r.retired;
            const std::uint64_t s0 = r.s0 = seed; int ceil = std::max(1, cfg.K > 0 ? cfg.K : cfg.ceiling); if (cap > 0) ceil = std::min(ceil, cap); r.ceil = ceil; const double pair = pairW(); const int nmin = std::clamp(cfg.nMin, 1, ceil);
            const auto lead = [&](std::size_t a, std::size_t b, double* sd) { return Lead(U, a, b, sd); };
            std::size_t best = N - 1, live = N; int n = 0;
            if (in) for (std::size_t i = 0; i < N; ++i) if (!(*in)[i]) { alive[i] = 0; if (memo && !(*memo)[i].empty()) { U[i] = (*memo)[i]; double sum = 0; for (double v : U[i]) sum += v; u[i] = sum / double(U[i].size()); } }
            for (int next = nmin;;) {
                for (int k = n; k < next; ++k) for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue;
                    if (memo && (*memo)[i].size() > static_cast<std::size_t>(k)) { U[i].push_back((*memo)[i][static_cast<std::size_t>(k)]); continue; }
                    Fight G = Stream(F, s0, k);
                    if (!roll(G, i)) { alive[i] = 0; continue; }
                    U[i].push_back(Utility(G)); ++rollouts; }
                n = next; bool any = false;
                for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue; double sum = 0; for (double v : U[i]) sum += v; u[i] = sum / double(U[i].size()); if (!any || u[i] > u[best]) { best = i; any = true; } }   // the first of equal means stands (IAUS's order)
                const double tc = TCrit(n, cfg.pOne), rn = std::sqrt(double(n)); live = 0;
                for (std::size_t i = 0; i < N; ++i) { if (!alive[i]) continue; if (i == best) { ++live; continue; }
                    double sd = 0; const double d = lead(best, i, &sd);
                    if (n >= 2 && (sd <= 0.0 ? (d > 0.0 || (d >= 0.0 && (pair < 0.0 || n >= kTwinMin))) : d > tc * sd / rn)) alive[i] = 0; else ++live; }   // with the cuts on, a duplicate of the best is a twin like any other: not retired under kTwinMin streams (choice review SEV-4: at 4 or 8 streams half of the twins still differ later)   // sd 0: the same outcome on every stream (a duplicate of the best, or beaten on every one)
                // PAIRWISE INDIFFERENCE (OracleCfg::pair, the block comment above g_oraclePrune): from the last option to the first, so of two twins the earlier stands
                if (pair >= 0.0 && n >= 2 && live > 1) for (std::size_t i = N; i-- > 0;) { if (!alive[i] || i == best || guard[i]) continue;
                    for (std::size_t j = 0; j < N; ++j) { if (j == i || !alive[j]) continue;
                        double sd = 0; const double d = lead(j, i, &sd);
                        if (sd <= 0.0 ? (d >= 0.0 && j < i && n >= kTwinMin) : (pair > 0.0 && d > tc * sd / rn - pair && d > -0.5 * pair)) { alive[i] = 0; --live; ++retired; break; } } }
                if (live <= 1 || n >= ceil) break;
                next = std::min(ceil, n * 2);
            }
            r.best = best; r.live = live; r.n = n; return r;
        }
        // THE PICK ON THE STRICT ROAD (review finding 2). A race's winner is the highest of N noisy means: taken as it is, the oracle plays the noise. Its default is what
        // IAUS would do, then what he is on IF he was put on it by a confirmed lead (else the first Free of a fight, which nobody chose, would be a default nothing leaves). Each is left only on a PAIRED lead of the winner over 2 standard errors and over a tie's width, and only when the same lead
        // stands on streams the race has not seen (fresh(i, k), the same luck for both). The fresh lead over IAUS's own answer is `won` (it was not selected: unbiased).
        struct Pick { std::size_t best = 0; bool left = false; double won = 0.0; };   // left: IAUS's answer was left, on a confirmed lead
        [[nodiscard]] static bool Beats(double d, double sd, std::size_t n) { return n >= 2 && d > kTieU && d > 2.0 * sd / std::sqrt(double(n)); }
        template <class Fresh> static Pick Choose(const Race& R, int iausIdx, int lastIdx, const Fresh& fresh) {
            Pick p; p.best = R.best; std::vector<std::vector<double>> FU(R.U.size());
            for (const int di : { iausIdx, lastIdx }) { if (di < 0 || R.U[static_cast<std::size_t>(di)].empty() || static_cast<std::size_t>(di) == p.best) continue; const auto d = static_cast<std::size_t>(di);
                double sd = 0; double m = Lead(R.U, p.best, d, &sd); bool go = Beats(m, sd, std::min(R.U[p.best].size(), R.U[d].size()));
                if (go) { const int K = std::max(8, R.n); for (const std::size_t i : { p.best, d }) while (static_cast<int>(FU[i].size()) < K) { const std::optional<double> v = fresh(i, static_cast<int>(FU[i].size())); if (!v) break; FU[i].push_back(*v); }
                    m = Lead(FU, p.best, d, &sd); go = Beats(m, sd, std::min(FU[p.best].size(), FU[d].size())); }
                if (!go) p.best = d; else if (di == iausIdx) { p.left = true; p.won = m; } }
            if (static_cast<int>(p.best) == iausIdx) { p.left = false; p.won = 0.0; }
            return p; }
        static constexpr int kFreshK = 1 << 20;   // the first stream the races never reach
        template <class Roll> [[nodiscard]] auto FreshOf(const Fight& F, std::uint64_t s0, const Roll& roll, long& rollouts) const { return [this, &F, s0, &roll, &rollouts](std::size_t i, int k) -> std::optional<double> { Fight G = Stream(F, s0, kFreshK + k); if (!roll(G, i)) return std::nullopt; ++rollouts; return Utility(G); }; }
        // A candidate as the movement pass reads an action: its own row of the ranking, else the top row's score (the oracle holds it: it is no worse than that).
        [[nodiscard]] static MFO::Iaus::RankedAction RankedOf(const Candidate& k, const MFO::Iaus::RankedAction& top) { return { k.family, k.scrollOf, k.form, k.target, top.score > 0.0f ? top.score : 1.0f, k.f.reach, k.f.meleeRange, k.f.sight }; }
        [[nodiscard]] static MFO::Iaus::RankedAction RankedOf(const Candidate& k, const Decision& d, const Generated& g) {
            MFO::Iaus::RankedAction a = RankedOf(k, MFO::Iaus::RankedAction{}); bool found = false; float top = 0.0f;
            for (const Scored& s : d.ranked) if (s.idx < g.actions.size()) { top = std::max(top, s.score); if (KeyOf(g.actions[s.idx]) == KeyOf(k)) { a.score = s.score; found = true; } }
            if (!found && top > 0.0f) a.score = top;
            return a; }

        // ── S5-C: THE ORDER RACE (s5-plan.md 4.2). At the end of a movement window at which the pass itself would decide for him (not committed, or marked), and never
        // between windows: orders x his ranked actions, raced like actions. The pass's own assignment for that window is IAUS's order. ──
        std::map<std::string, SitRec>* orderSit = nullptr;   // divergences of the order: "iaus order|oracle order|paired or not|situation"
        std::vector<std::string>* orderLines = nullptr;      // the ORCMODE lines
        [[nodiscard]] static bool Asked(const Fight& F, int ci) {
            if (g_road != Road::Strict || g_oracleOrders != OracleOrders::On || !F.move) return false;
            const MoveState& S = *F.move.s; if (!S.open || F.now < S.winEnd - 1e-9 || S.force != Force::None || static_cast<std::size_t>(ci) >= S.claim.size()) return false;   // MoveStep closes it in this very step
            if (S.pic.work.stage != MV::Work::Stage::Done) return false;
            const Body& x = F.b[static_cast<std::size_t>(F.ctl[static_cast<std::size_t>(ci)].body)];
            for (const MV::Member& m : S.pic.party) if (m.id == x.c.id) { const OrderPick& k = S.claim[static_cast<std::size_t>(ci)].pick; if (k.on && !k.fresh && !k.spot && !m.seen && F.now < k.askAt - 1e-9) return false;   /* his own place is held as long as it was raced (finding 9) */ return m.decides && !m.fixed && m.nTop > 0 && m.tri >= 0 && !MoveBodyTaken(F, x); }
            return false; }
        struct OrdOpt { OrderPick pick; const Candidate* k = nullptr; OrderAsk ask; bool paired = true, iaus = false; std::uint8_t a = 0; };
        std::function<void(const std::vector<OrdOpt>&, const Race&, std::size_t)> peek;   // the spot tests read an order race
        std::vector<OrderPick> wonOrder; std::vector<std::pair<bool, Key>> wonAct;   // the order / action that won over IAUS's own, per follower, while it stands (Choose)
        std::vector<std::uint64_t> keptSig;   // a Free or Hold that was kept, per follower: what it was decided on (kCutSkip)
        [[nodiscard]] static std::string KindName(const OrderPick& p) { return !p.spot ? (p.kinds & MV::KindBit(MFO::Iaus::SpotKind::Hold) ? "hold" : p.end ? "own_end" : "own") : MV::kSpotKinds[static_cast<std::size_t>(MV::OrderRowOf(p.kinds).kind)].name; }
        void Window(Fight& F) override { for (int ci = 0; ci < static_cast<int>(F.ctl.size()); ++ci) if ((cfg.allFollowers || ci == 0) && Asked(F, ci)) Order(F, ci); }
        void Order(Fight& F, int ci) {
            using MFO::Iaus::SpotKind; MoveState& S = *F.move.s; MoveClaim& q = S.claim[static_cast<std::size_t>(ci)]; Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; const Body& x = F.b[static_cast<std::size_t>(c.body)]; const bool subj = ci == 0;
            const Tuning& T = *iaus.T; std::size_t mi = 0; while (S.pic.party[mi].id != x.c.id) ++mi; const MV::Member& m = S.pic.party[mi];
            // CUT: his own place, unmarked, and nothing the race was about has changed (the pool, what each action was offered, his three actions): it stands again
            std::uint64_t sig = 0; if (cut(kCutSkip)) { const auto mix = [&](std::uint64_t z) { sig = Fight::Mix64(sig ^ z); }; for (const MV::Spot& sp : S.pic.pool) { mix(sp.key); mix(sp.kinds); } for (std::uint8_t a = 0; a < m.nTop; ++a) { mix(static_cast<std::uint64_t>(m.top[a].family)); mix(m.top[a].form); mix(m.top[a].target); } mix(m.seen);
                if (keptSig.size() < F.ctl.size()) keptSig.resize(F.ctl.size(), 0); auto& was = keptSig[static_cast<std::size_t>(ci)];
                if (q.pick.on && !q.pick.fresh && !q.pick.spot && !m.seen && was == sig) { q.pick.askAt = F.now + cfg.commit; if (!q.pick.end) q.pick.until = q.pick.askAt; OracleStats one; one.orderSkip = 1; st.Merge(one); if (subj) stSubj.Merge(one); return; } was = 0; }
            // the pass's own answer, on a copy
            MV::PartyPicture probe = S.pic; std::vector<MV::Assignment> as; tl_moveFight = &F; MP::Phase3(probe, &MoveProfile, T, as); MP::Adopt(probe, as); MP::Orders(probe, as, T);
            MV::Assignment mine; for (const MV::Assignment& a : as) if (a.who == m.id) mine = a;
            const MV::MoveMode iausMode = probe.party[mi].order.mode;
            // his actions: the three the pass pairs. One that is refused, cooling or only a wish from afar (moveOnly) has no candidate: the order then goes with whatever IAUS does
            Served sv; MakeSnapshot(F, ci, sv.sn); sv.g = SimGenerate(sv.sn, c.gate, T); FillCooldowns(c, sv.g.actions, F.now, T);
            std::array<const Candidate*, 3> act{}; std::array<char, 3> use{}; int nUse = 0;
            for (std::uint8_t a = 0; a < m.nTop; ++a) { const auto& r = m.top[a]; bool bad = false;
                if (!r.moveOnly) for (const Candidate& k : sv.g.actions) if (k.family == r.family && k.form == r.form && k.target == r.target) { if (k.f.hardCooldownSec > 0.0f && k.f.hardCooldownLeft > 0.0f) bad = true; else { Opt o; o.k = &k; if (Ok(F, c, o)) act[a] = &k; else bad = true; } break; }
                use[a] = !bad; nUse += !bad; }
            if (!nUse) use[0] = 1;
            // the orders
            long clustered = 0; const std::uint32_t allowed = MV::AllowedKinds(S.pic.caps, T) & m.kinds; const EQ::View v = EnvViewOf(F, true); std::vector<OrdOpt> opt;
            const auto ok = [&](const OrderAsk& a) { Opt o; o.order = &a; return Ok(F, c, o); };
            const auto add = [&](OrderPick p, const OrderAsk& ask, std::uint8_t a, bool paired) { p.on = p.fresh = true; p.act = use[a] ? m.top[a] : MFO::Iaus::RankedAction{}; OrdOpt o; o.pick = p; o.k = act[a]; o.ask = ask; o.paired = paired; o.a = a; opt.push_back(o); };
            for (const MV::MoveMode md : { MV::MoveMode::Kite, MV::MoveMode::Disengage }) if (!ModeServedBy(S.pic.caps, md)) { OrderAsk a; a.mode = md; (void)ok(a); }   // a mode that is not served is refused once a window
            const double soon = F.now + cfg.commit;   // his own place is held as long as an action option is, unless a mark ends it. Free is offered to the end too: what `off` plays (finding 3)
            { OrderAsk fr, ho; ho.mode = MV::MoveMode::Stand; ho.kinds = MV::KindBit(SpotKind::Hold); const bool okF = ok(fr), okH = ok(ho) && (allowed & ho.kinds);
              for (std::uint8_t a = 0; a < m.nTop; ++a) if (use[a] || a == 0) {
                OrderPick p; p.until = p.askAt = soon; p.kinds = MV::KindBit(SpotKind::Stay); if (okF) { add(p, fr, a, true); if (!g_moveFault->oracleNoEnd) { OrderPick z = p; z.end = true; z.until = 1e9; add(z, fr, a, true); } }
                p.mode = MV::MoveMode::Stand; p.kinds = ho.kinds; if (okH) add(p, ho, a, true); } }   // no target asked for: he may hold while he drinks
            // CUT: places that are one place to him (the same kinds and offers, one triangle or one arrive radius apart) are one option. His own and the pass's own stay
            std::vector<std::size_t> order; std::vector<std::uint32_t> offs; for (std::size_t si = 0; si < S.pic.pool.size(); ++si) { std::uint32_t any = 0; for (std::uint8_t a = 0; a < m.nTop; ++a) any |= S.pic.work.Offered(static_cast<std::int32_t>(si), static_cast<std::uint32_t>(mi), a); offs.push_back(any); order.push_back(si); }
            const auto first0 = [&](std::size_t si) { const MV::SpotKey k = S.pic.pool[si].key; return (m.commit.on && m.commit.key == k) || (mine.spot >= 0 && mine.key == k); };
            if (cut(kCutCluster)) std::stable_sort(order.begin(), order.end(), [&](std::size_t l, std::size_t r) { const int a = first0(l) ? 2 : offs[l] != 0, b = first0(r) ? 2 : offs[r] != 0; return a > b; });
            std::vector<std::size_t> heads;
            const auto twin = [&](std::size_t si) { const MV::Spot& s = S.pic.pool[si]; for (const std::size_t h : heads) { const MV::Spot& z = S.pic.pool[h]; if (z.kinds == s.kinds && offs[h] == offs[si] && z.portals == s.portals && (MV::Dist(z.p, s.p) <= T.orderArrive || (z.tri == s.tri && MV::Dist(z.p, s.p) <= kClusterU))) return true; } return false; };
            for (const std::size_t si : order) { const MV::Spot& sp = S.pic.pool[si];
                if (cut(kCutCluster) && !first0(si) && twin(si)) { ++clustered; continue; }
                std::uint32_t any = 0; for (std::uint8_t a = 0; a < m.nTop; ++a) any |= S.pic.work.Offered(static_cast<std::int32_t>(si), static_cast<std::uint32_t>(mi), a);
                const bool stands = m.commit.on && m.commit.key == sp.key; OrderAsk ask; ask.kinds = sp.kinds;
                ask.leash = c.leash > 0.0f && std::hypot(sp.p.x - F.b[0].c.x, sp.p.y - F.b[0].c.y) > c.leash + kLeashStandU;
                ask.refused = !g_moveFault->oracleDistrusted && TR::Refused(S.pic.trustNow, m.id, sp.key);
                for (std::size_t o = 0; o < S.pic.party.size(); ++o) { const MV::Member& w = S.pic.party[o]; if (o == mi || (!w.fixed && w.decides && w.nTop)) continue;   // MovePass Taken, for those who are planned before anybody
                    const std::array<std::uint64_t, 2>* po = &w.portals; if (w.fixed) { ask.taken = ask.taken || w.commit.key == sp.key; for (const MV::Spot& z : S.pic.pool) if (z.key == w.commit.key) po = &z.portals; }
                    for (const auto e : sp.portals) for (const auto f : *po) ask.taken = ask.taken || (e && e == f); }
                bool way = stands; for (const auto& p : S.pic.work.pairs) way = way || (p.member == mi && p.spot == static_cast<std::int32_t>(si));
                if (!way && !ask.leash && !ask.refused && !ask.taken) { EQ::Path p; ask.noWay = !(EQ::FindPath(v, { m.p, sp.p, m.tri, sp.tri, m.who, S.pic.avoid }, tl_envScratch, p) && p.reachable); }
                std::array<char, 6> said{};   // a refusal is counted once a place and mode, whatever it is paired with
                for (std::uint8_t a = 0; a < m.nTop; ++a) { if (!use[a] && a) continue;
                    std::uint32_t mask = S.pic.work.Offered(static_cast<std::int32_t>(si), static_cast<std::uint32_t>(mi), a); const bool paired = mask != 0; if (!mask && !g_moveFault->oraclePaired) mask = sp.kinds & allowed;
                    if (stands && !paired) mask = m.commit.kinds & allowed;
                    OrderAsk aa = ask; aa.mode = mask ? MV::OrderRowOf(mask).mode : MV::MoveMode::Stand; if (!mask) aa.kinds = sp.kinds & ~allowed; std::array<char, 4> carried{};
                    for (const MV::MoveMode md : { aa.mode, MV::MoveMode::Stand, MV::MoveMode::GoTo, MV::MoveMode::Kite, MV::MoveMode::Disengage }) {
                        if (md == MV::MoveMode::Free) continue;
                        if (md != aa.mode && !ModeServedBy(S.pic.caps, md)) continue;   // said above
                        char& seen = carried[static_cast<std::size_t>(ModeCarried(S.pic.caps, md))]; if (seen) continue; seen = 1;
                        OrderAsk am = aa; am.mode = md; if (said[static_cast<std::size_t>(md)]) continue;
                        if (!ok(am) || !mask) { said[static_cast<std::size_t>(md)] = 1; continue; }
                        OrderPick p; p.spot = true; p.key = sp.key; p.kinds = mask; p.mode = md; p.until = F.now + cfg.orderSec; add(p, am, a, any != 0); if (heads.empty() || heads.back() != si) heads.push_back(si); } } }
            const std::size_t N = opt.size(); if (N < 2) return;
            // what stands now (the incumbent: it wins a tie, as the action he is on does), and IAUS's own answer
            int last = -1, ii = -1; const auto first = [&](const OrderPick& w, const MFO::Iaus::RankedAction& a, bool modeToo) { int alt = -1; for (std::size_t i = 0; i < N; ++i) { const OrderPick& p = opt[i].pick; if (p.spot != w.spot || p.end != w.end || (p.spot ? p.key != w.key : p.kinds != w.kinds) || (modeToo && p.spot && p.mode != w.mode)) continue; if (alt < 0) alt = static_cast<int>(i); if (p.act.family == a.family && p.act.form == a.form && p.act.target == a.target) return static_cast<int>(i); } return alt; };
            { OrderPick w; w.end = q.pick.on && q.pick.end; w.spot = m.commit.on; w.key = m.commit.key; w.mode = m.order.mode; w.kinds = MV::KindBit(m.order.mode == MV::MoveMode::Free ? SpotKind::Stay : SpotKind::Hold); last = first(w, m.top[0], true); if (last < 0) last = first(w, m.top[0], false); }
            { OrderPick w; w.spot = mine.spot >= 0; w.key = mine.key; w.kinds = mine.kinds; ii = first(w, mine.act, false); if (ii >= 0) opt[static_cast<std::size_t>(ii)].iaus = true; }
            std::vector<char> guard; if (cfg.pair >= 0.0) { guard.assign(N, 0); if (last >= 0) guard[static_cast<std::size_t>(last)] = 1; if (ii >= 0) guard[static_cast<std::size_t>(ii)] = 1; }
            const auto roll = [&](Fight& G, std::size_t i) {
                G.move->claim[static_cast<std::size_t>(ci)].pick = opt[i].pick; RolloutPolicy rp;
                if (opt[i].k) { rp.firstCi = ci; rp.firstUntil = F.now + 1.0; rp.first = [&, i](Fight& H) { Opt o; o.k = opt[i].k; if (!Apply(H, ci, o, cfg.commit, cfg.every)) return false; MoveNoteFront(H.move->claim[static_cast<std::size_t>(ci)], opt[i].pick.act); return true; }; }
                RunFor(G, rp, until(F)); return true; };
            long rolls = 0; const std::uint64_t seed = RaceSeed(F, N, ci, true); Race R; std::vector<char> in(N, 1);
            if (cut(kCutScreen) && nUse > 1) {   // CUT: the orders are screened at one action, what still stands is crossed with the others (the screen's utilities are kept: the same streams)
                std::uint8_t a0 = 0; while (!use[a0]) ++a0;
                std::vector<char> s(N, 0); for (std::size_t i = 0; i < N; ++i) s[i] = opt[i].a == a0 || static_cast<int>(i) == last || static_cast<int>(i) == ii;
                const Race R1 = RunRace(F, N, seed, guard, roll, rolls, &s, nullptr, kScreenK); std::vector<std::vector<double>> memo = R1.U;
                for (std::size_t i = 0; i < N; ++i) { if (s[i]) { in[i] = R1.alive[i] || static_cast<int>(i) == last || static_cast<int>(i) == ii; continue; }
                    in[i] = 0; for (std::size_t j = 0; j < N && !in[i]; ++j) in[i] = s[j] && R1.alive[j] && opt[j].pick.SameOrder(opt[i].pick) && opt[j].pick.end == opt[i].pick.end; }
                R = RunRace(F, N, seed, guard, roll, rolls, &in, &memo);
            } else R = RunRace(F, N, seed, guard, roll, rolls);
            auto& U = R.U; std::size_t best = R.best;
            const auto label = [&](const OrdOpt& o) { return std::string("order:") + kModeName[static_cast<std::size_t>(o.pick.mode)] + ":" + KindName(o.pick); };   // free:own_end = to the end
            if (g_oracleDump && subj) {   // what the screen dropped is not in it: the dump is the race that decided
                const auto at = [&](int i) { int k = 0; for (int j = 0; j < i; ++j) k += in[static_cast<std::size_t>(j)]; return k; }; bool comma = false;
                std::string o = "{\"t\":" + std::to_string(F.now) + ",\"order\":1,\"n\":" + std::to_string(R.n) + ",\"best\":" + std::to_string(at(static_cast<int>(best))) + ",\"last\":" + std::to_string(last < 0 ? -1 : at(last)) + ",\"opts\":[";
                for (std::size_t i = 0; i < N; ++i) { if (!in[i]) continue; std::vector<double> full = U[i]; for (int k = static_cast<int>(full.size()); k < R.ceil; ++k) { Fight G = Stream(F, R.s0, k); if (!roll(G, i)) break; full.push_back(Utility(G)); }
                    o += std::string(comma ? "," : "") + "{\"l\":\"" + label(opt[i]) + "\",\"tg\":" + std::to_string(opt[i].pick.act.target) + ",\"form\":" + std::to_string(opt[i].pick.act.form) + ",\"key\":" + std::to_string(opt[i].pick.spot ? opt[i].pick.key : 0) + ",\"paired\":" + std::to_string(int(opt[i].paired)) + ",\"n0\":" + std::to_string(U[i].size()) + ",\"iaus\":" + std::to_string(int(opt[i].iaus)) + ",\"u\":[";
                    for (std::size_t z = 0; z < full.size(); ++z) { char nb[32]; std::snprintf(nb, sizeof nb, "%s%.4f", z ? "," : "", full[z]); o += nb; } o += "]}"; comma = true; }
                o += "]}\n"; std::fputs(o.c_str(), g_oracleDump); std::fflush(g_oracleDump); }
            Pick pk; pk.best = best;
            if (leads()) { if (wonOrder.size() < F.ctl.size()) wonOrder.resize(F.ctl.size()); OrderPick& won = wonOrder[static_cast<std::size_t>(ci)]; const auto isWon = [&](int i) { return i >= 0 && won.on && won.SameOrder(opt[static_cast<std::size_t>(i)].pick) && won.end == opt[static_cast<std::size_t>(i)].pick.end; };
                // an order is left for another order, not for the action it was paired with: what stands for it is its best row (the action race decides the action anyway)
                const auto row = [&](int i) { if (i < 0) return i; int b = i; for (std::size_t j = 0; j < N; ++j) if (opt[j].pick.SameOrder(opt[static_cast<std::size_t>(i)].pick) && opt[j].pick.end == opt[static_cast<std::size_t>(i)].pick.end && (U[j].size() > U[static_cast<std::size_t>(b)].size() || (U[j].size() == U[static_cast<std::size_t>(b)].size() && R.u[j] > R.u[static_cast<std::size_t>(b)]))) b = static_cast<int>(j); return b; };
                if (!g_moveFault->oracleBest) pk = Choose(R, row(ii), isWon(last) || ii < 0 ? row(last) : -1, FreshOf(F, seed, roll, rolls)); best = pk.best;
                if (pk.left) won = opt[best].pick; else if (!isWon(static_cast<int>(best))) won.on = false; }
            else if (last >= 0 && R.alive[static_cast<std::size_t>(last)] && R.u[static_cast<std::size_t>(last)] >= R.u[best] - 0.3) best = static_cast<std::size_t>(last);   // a person does not dither
            if (peek) peek(opt, R, best);
            const OrdOpt& w = opt[best];
            { OracleStats one; one.orders = 1; one.orderOpts = static_cast<long>(N); one.orderRolls = rolls; one.unpaired = !w.paired; one.orderKept = static_cast<int>(best) == last; one.orderBound = R.live > 1; one.orderClust = clustered; ++one.modeWin[static_cast<std::size_t>(w.pick.mode)]; ++one.modeIaus[static_cast<std::size_t>(iausMode)];
              if (ii >= 0) { auto& l = one.party[static_cast<int>(F.ctl.size())]; l.n = 1; l.same = w.pick.SameOrder(opt[static_cast<std::size_t>(ii)].pick); l.sum = l.same ? 0.0 : pk.left ? pk.won : Lead(U, best, static_cast<std::size_t>(ii), nullptr); }
              st.Merge(one); if (subj) stSubj.Merge(one); }
            if (subj && orderLines) {   // per mode: its best option's mean, the streams it ran, the winner's paired lead over it and the standard error
                char b[320]; std::snprintf(b, sizeof b, "t=%.2f commit=%d marks=%x age=%.2f opts=%zu streams=%d win=%s:%s act=%s paired=%d iaus=%s:%s same=%d", F.now, int(m.commit.on), m.seen, F.now - q.topAt, N, R.n, kModeName[static_cast<std::size_t>(w.pick.mode)], KindName(w.pick).c_str(), std::string(MFO::Iaus::Name(w.pick.act.family == Family::kCount ? Family::Attack : w.pick.act.family)).c_str(), int(w.paired), kModeName[static_cast<std::size_t>(iausMode)], ii >= 0 ? KindName(opt[static_cast<std::size_t>(ii)].pick).c_str() : "refused", int(ii >= 0 && w.pick.SameOrder(opt[static_cast<std::size_t>(ii)].pick)));
                std::string o = b; if (w.pick.act.family == Family::kCount) { const auto at = o.find("act="); o.replace(at, o.find(' ', at) - at, "act=-"); }
                for (std::size_t md = 0; md < 5; ++md) { if (!ModeServedBy(S.pic.caps, static_cast<MV::MoveMode>(md))) continue;
                    std::size_t mb = N; for (std::size_t i = 0; i < N; ++i) if (static_cast<std::size_t>(opt[i].pick.mode) == md && !U[i].empty() && (mb == N || U[i].size() > U[mb].size() || (U[i].size() == U[mb].size() && R.u[i] > R.u[mb]))) mb = i;   // of those that stood longest
                    if (mb == N) { o += std::string("  ") + kModeName[md] + " -"; continue; }
                    double sd = 0; const double d = Lead(U, best, mb, &sd); const std::size_t k = std::min(U[best].size(), U[mb].size()); std::snprintf(b, sizeof b, "  %s u=%.3f n=%zu lead=%.3f se=%.3f", kModeName[md], R.u[mb], U[mb].size(), d, sd / std::sqrt(double(std::max<std::size_t>(1, k)))); o += b; }
                orderLines->push_back(o); }
            if (subj && orderSit && static_cast<int>(best) != ii && (ii < 0 || ((leads() ? pk.left : !R.alive[static_cast<std::size_t>(ii)]) && !w.pick.SameOrder(opt[static_cast<std::size_t>(ii)].pick)))) {
                const double dU = ii < 0 ? 0.0 : pk.left ? pk.won : Lead(U, best, static_cast<std::size_t>(ii), nullptr);
                if (ii < 0 || dU >= cfg.minDU) { SitRec& r = (*orderSit)[(ii >= 0 ? label(opt[static_cast<std::size_t>(ii)]) : std::string("order:") + kModeName[static_cast<std::size_t>(iausMode)] + ":refused") + "|" + label(w) + "|" + (w.paired ? "ranked" : "unpaired") + "|" + Situation(x, sv.sn)]; ++r.n; r.dU += dU; } }
            q.pick = w.pick; if (sig && !w.pick.spot && static_cast<int>(best) == last) keptSig[static_cast<std::size_t>(ci)] = sig;
            if (F.trace) std::fprintf(F.trace, "%7.2f  ORACLE ORDERS %s: %s with %s -> %u  U %.2f (the pass would: %s, U %.2f)  [%zu options, %d streams, %zu standing]\n", F.now, x.c.name.c_str(), label(w).c_str(), w.pick.act.family == Family::kCount ? "-" : std::string(MFO::Iaus::Name(w.pick.act.family)).c_str(), w.pick.act.target, R.u[best], ii >= 0 ? label(opt[static_cast<std::size_t>(ii)]).c_str() : "refused", ii >= 0 ? R.u[static_cast<std::size_t>(ii)] : 0.0, N, R.n, R.live);
        }

        void Serve(Fight& F, int ci) override {
            if (!cfg.allFollowers && ci != 0) { iaus.Serve(F, ci); return; }
            if (last.size() < F.ctl.size()) last.assign(F.ctl.size(), -1e9);
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; const bool subj = ci == 0;
            if (ServiceRun(F, ci)) return;   // card G3: a hide run or its hold stands
            if (!Fight::Up(x)) return;
            const bool held = OracleHeld(F, c, x);
            Served sv; MakeSnapshot(F, ci, sv.sn); sv.g = SimGenerate(sv.sn, c.gate, *iaus.T);
            FillCooldowns(c, sv.g.actions, F.now, *iaus.T); FillCooldowns(c, sv.g.postures, F.now, *iaus.T);
            TickIn in; in.now = F.now; in.rapportRank = 5; in.seed = c.seed; in.commitWindowSec = c.window;
            const bool wantAlt = subj && reg && !Alts().empty(); Mind mind0; if (wantAlt) mind0 = c.mind;   // the mind as it stood before the base table decided
            const bool claims = F.move && g_oracleOrders != OracleOrders::Off;   // S5-C: he holds a claim like anybody
            if (claims && F.move->claim[static_cast<std::size_t>(ci)].on) { const auto& a = F.move->claim[static_cast<std::size_t>(ci)].act; in.hasSpotAction = true; in.spotAction = Key{ a.family, PostureKind::None, a.form, a.target }; }
            sv.d = Decide(c.mind, sv.g.actions, c.profile, *iaus.T, in);
            if (claims) { MoveClaim& q = F.move->claim[static_cast<std::size_t>(ci)]; FillCooldowns(c, sv.g.moveOnly, F.now, *iaus.T); MoveNoteRanked(F, ci, sv.d, sv.g, *iaus.T);
                if (c.hasLastKey) for (const Candidate& k : sv.g.actions) if (KeyOf(k) == c.lastKey) { MoveNoteFront(q, RankedOf(k, sv.d, sv.g)); break; } }   // the action he is on
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
            // (batch C) what each alternate table would pick on this snapshot; then the exact pruning, which never drops IAUS's pick or an alternate's
            std::vector<Key> altKey; std::vector<char> altHas, altCast;
            if (wantAlt) for (const AltTable& a : Alts()) {
                Generated ga = SimGenerate(sv.sn, c.gate, a.t); FillCooldowns(c, ga.actions, F.now, a.t);
                Profile pf = c.profile; for (const auto& w : a.w) SetWeight(pf, w.family, w.cons, w.w);
                Mind m = mind0; const Decision da = Decide(m, ga.actions, pf, a.t, in);
                const bool has = da.verdict != Verdict::Idle && !da.order.empty() && da.order[0].idx < ga.actions.size();
                altKey.push_back(has ? KeyOf(ga.actions[da.order[0].idx]) : Key{}); altHas.push_back(has ? 1 : 0); altCast.push_back(has && CastFamily(ga.actions[da.order[0].idx].family) ? 1 : 0); }
            std::vector<char> guard;   // options the pair rule never retires: IAUS's pick, an alternate's pick, the action he is on (filled below; the best is the race's own)
            if (g_oraclePrune) { std::vector<int> bi(opt.size(), -1); std::vector<char> dead(opt.size(), 0), keep(opt.size(), 0); std::vector<float> kf(opt.size(), 1.0f); long np = 0; const bool onFoe = g_oraclePrune == 2;
                for (std::size_t i = 0; i < opt.size(); ++i) { const Candidate* k = opt[i].k; if (!k || opt[i].ext != Ext::None || k->family != Family::CastOffense) continue;
                    bi[i] = BookIndex(c, k->form); const Key ky = KeyOf(*k);
                    if (onFoe && bi[i] >= 0) { const Body* tg = F.Find(k->target); if (!tg) { bi[i] = -1; continue; } const int av = c.build->spells[static_cast<std::size_t>(bi[i])].e.resistAv;
                        kf[i] = (1.0f - std::clamp(F.ResistOf(*tg, av) / 100.0f, -1.0f, 1.0f)) * (1.0f - std::clamp(F.ResistOf(*tg, kAvResistMagic) / 100.0f, -1.0f, 1.0f)); } if (iausPick && ky == KeyOf(*iausPick)) keep[i] = 1; for (std::size_t a = 0; a < altKey.size(); ++a) if (altHas[a] && ky == altKey[a]) keep[i] = 1; }
                for (std::size_t i = 0; i < opt.size(); ++i) { if (bi[i] < 0 || dead[i]) continue;
                    for (std::size_t j = i + 1; j < opt.size(); ++j) { if (bi[j] < 0 || dead[j] || opt[i].k->target != opt[j].k->target) continue;
                        const auto si = static_cast<std::size_t>(bi[i]), sj = static_cast<std::size_t>(bi[j]);
                        if (!keep[j] && Dominates(c.build->spells[si], c.book.entries[si], c.build->spells[sj], c.book.entries[sj], onFoe, kf[i], kf[j], x.stealthy)) { dead[j] = 1; ++np; }   // equal on every number: the earlier (IAUS's higher ranked) stands
                        else if (!keep[i] && Dominates(c.build->spells[sj], c.book.entries[sj], c.build->spells[si], c.book.entries[si], onFoe, kf[j], kf[i], x.stealthy)) { dead[i] = 1; ++np; break; } } }
                if (np) { std::vector<Opt> kept; kept.reserve(opt.size()); for (std::size_t i = 0; i < opt.size(); ++i) if (!dead[i]) kept.push_back(opt[i]); opt.swap(kept); if (subj && reg) reg->pruned += np; } }
            opt.push_back(Opt{});   // idle: his own AI
            if (cfg.pair >= 0.0) { guard.assign(opt.size(), 0);
                for (std::size_t i = 0; i < opt.size(); ++i) { const Candidate* k = opt[i].k; if (!k || opt[i].ext != Ext::None) continue; const Key ky = KeyOf(*k);
                    if ((iausPick && ky == KeyOf(*iausPick)) || (c.hasLastKey && ky == c.lastKey)) guard[i] = 1; for (std::size_t a = 0; a < altKey.size(); ++a) if (altHas[a] && ky == altKey[a]) guard[i] = 1; } }
            const long rollouts0 = st.rollouts;
            // EVALUATE: the race (OracleCfg).
            const std::size_t N = opt.size();
            const MFO::Iaus::RankedAction none; const bool note = F.move && g_oracleOrders != OracleOrders::Off;
            const auto roll = [&](Fight& G, std::size_t i) {
                if (!Apply(G, ci, opt[i], cfg.commit, cfg.every)) return false;   // it cannot be done now
                if (note && opt[i].k && opt[i].ext == Ext::None && opt[i].k->family != Family::Posture) MoveNoteFront(G.move->claim[static_cast<std::size_t>(ci)], RankedOf(*opt[i].k, none));
                RolloutPolicy rp; RunFor(G, rp, until(F)); return true; };
            Race R = RunRace(F, N, RaceSeed(F, N, ci, false), guard, roll, st.rollouts); auto& U = R.U; auto& alive = R.alive; auto& u = R.u; std::size_t best = R.best; const std::size_t live = R.live; const int n = R.n, ceil = R.ceil; const std::uint64_t s0 = R.s0; const long retired = R.retired;
            const auto lead = [&](std::size_t a, std::size_t b, double* sd) { return Lead(U, a, b, sd); };
            if (g_oracleDump && subj) {
                std::string o = "{\"t\":" + std::to_string(F.now) + ",\"n\":" + std::to_string(n) + ",\"best\":" + std::to_string(best) + ",\"last\":" + std::to_string([&] { if (c.hasLastKey) for (std::size_t i = 0; i < N; ++i) if (opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == c.lastKey) return static_cast<int>(i); return -1; }()) + ",\"opts\":[";
                for (std::size_t i = 0; i < N; ++i) { std::vector<double> full = U[i];
                    if (!full.empty()) for (int k = static_cast<int>(full.size()); k < ceil; ++k) { Fight G = Stream(F, s0, k); if (!roll(G, i)) break; full.push_back(Utility(G)); }
                    const Candidate* k = opt[i].k; const int bi = k ? BookIndex(c, k->form) : -1; char buf[512]; buf[0] = 0;
                    if (bi >= 0 && opt[i].ext == Ext::None) { const SimSpell& sp = c.build->spells[static_cast<std::size_t>(bi)]; const SpellEntry& e = c.book.entries[static_cast<std::size_t>(bi)]; const Body* tg = F.Find(k->target);
                        float el = 0, mr = 0; if (tg) { el = std::clamp(F.ResistOf(*tg, sp.e.resistAv) / 100.0f, -1.0f, 1.0f); mr = std::clamp(F.ResistOf(*tg, kAvResistMagic) / 100.0f, -1.0f, 1.0f); }
                        std::snprintf(buf, sizeof buf, ",\"sp\":{\"cost\":%.2f,\"sec\":%.3f,\"reach\":%.0f,\"dmg\":%.2f,\"dot\":%.2f,\"dotDur\":%.1f,\"mp\":%.1f,\"spd\":%.1f,\"stg\":%d,\"area\":%.0f,\"ak\":%d,\"row\":%d,\"av\":%d,\"conc\":%d,\"touch\":%d,\"self\":%d,\"src\":%d,\"ctrl\":%d,\"und\":%d,\"heal\":%.1f,\"armor\":%.1f,\"k\":%.4f}",
                            e.cost, sp.castSec, e.reach, sp.dmg, sp.dot, sp.dotDur, sp.mpDmg, sp.spDmg, int(sp.stagger), e.areaUnits, sp.areaKind, sp.row, e.resistAv, int(e.concentration), int(e.touch), int(e.selfDelivery), int(e.source), int(sp.ctrl), int(e.undeadOnly), sp.heal, sp.armor, (1.0f - el) * (1.0f - mr)); }
                    o += std::string(i ? "," : "") + "{\"l\":\"" + opt[i].Label() + "\",\"tg\":" + std::to_string(k ? k->target : opt[i].target) + ",\"form\":" + std::to_string(k ? k->form : 0u) + ",\"n0\":" + std::to_string(U[i].size())
                        + ",\"iaus\":" + std::to_string(int(iausPick && k && opt[i].ext == Ext::None && KeyOf(*k) == KeyOf(*iausPick))) + buf + ",\"u\":[";
                    for (std::size_t q = 0; q < full.size(); ++q) { char nb[32]; std::snprintf(nb, sizeof nb, "%s%.4f", q ? "," : "", full[q]); o += nb; }
                    o += "]}"; }
                o += "]}\n"; std::fputs(o.c_str(), g_oracleDump); std::fflush(g_oracleDump); }   // one fputs per decision: whole lines from every worker thread
            { OracleStats one; one.options = static_cast<long>(N); one.streams = n; one.decisions = 1; one.rollouts = st.rollouts - rollouts0; one.twins = retired;
              if (live > 1) { one.bound = 1; one.standing = static_cast<long>(live); double other = -1e9; for (std::size_t i = 0; i < N; ++i) if (alive[i] && i != best) other = std::max(other, u[i]); one.unresolved = one.unresolvedMax = u[best] - other; }
              const long r = one.rollouts; one.rollouts = 0; st.Merge(one); one.rollouts = r; if (ci == 0) stSubj.Merge(one); }
            // pick: the best; the action he is already on wins when it still stands (not separated from the best) and is within 0.3 U of it (a person does not dither)
            // On the strict road: IAUS's own pick, then the action he is on, each left on a confirmed lead (Choose). A pick that is not among the options has no row to stand on.
            if (leads()) { int di = -1, dl = -1; for (std::size_t i = 0; i < N; ++i) { if (opt[i].ext != Ext::None) continue; if (!opt[i].k) { if (!iausPick) di = static_cast<int>(i); continue; } if (iausPick && di < 0 && KeyOf(*opt[i].k) == KeyOf(*iausPick)) di = static_cast<int>(i); if (c.hasLastKey && dl < 0 && KeyOf(*opt[i].k) == c.lastKey) dl = static_cast<int>(i); }
                if (wonAct.size() < F.ctl.size()) wonAct.resize(F.ctl.size(), { false, Key{} }); auto& won = wonAct[static_cast<std::size_t>(ci)]; const bool held1 = dl >= 0 && won.first && won.second == c.lastKey;
                const Pick pk = Choose(R, di, held1 || di < 0 ? dl : -1, FreshOf(F, s0, roll, st.rollouts)); best = pk.best; const bool act1 = opt[best].k && opt[best].ext == Ext::None;
                if (pk.left && act1) won = { true, KeyOf(*opt[best].k) }; else if (!(act1 && won.first && KeyOf(*opt[best].k) == won.second)) won.first = false; }
            else if (c.hasLastKey) for (std::size_t i = 0; i < N; ++i) if (alive[i] && opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == c.lastKey && u[i] >= u[best] - 0.3) { best = i; break; }
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
            if (subj && reg) {   // SPELL REGRET (the block comment above RegretLog)
                auto isCast = [&](std::size_t i) { return opt[i].k && opt[i].ext == Ext::None && CastFamily(opt[i].k->family); };
                std::size_t bc = N; for (std::size_t i = 0; i < N; ++i) if (isCast(i) && !U[i].empty() && (bc == N || u[i] > u[bc])) bc = i;
                if (isCast(best)) bc = best;
                ++reg->decisions;
                auto regretOf = [&](std::size_t i) { return (bc != N && i != bc && !alive[i] && !U[i].empty()) ? std::max(0.0, lead(bc, i, nullptr)) : 0.0; };
                const bool iCast = iausPick && CastFamily(iausPick->family);
                if (reg->alt.size() < altKey.size()) { reg->alt.resize(altKey.size(), 0.0); reg->altMiss.resize(altKey.size(), 0); }
                for (std::size_t a = 0; a < altKey.size(); ++a) {   // the same rule for an alternate table's pick
                    std::size_t ia = N - 1; bool found = !altHas[a]; if (altHas[a]) for (std::size_t i = 0; i < N; ++i) if (opt[i].k && opt[i].ext == Ext::None && KeyOf(*opt[i].k) == altKey[a]) { ia = i; found = true; break; }
                    if (!found) ++reg->altMiss[a];   // a pick that is not among the options (the class filter, a hard cooldown) does not happen: he idles, and is charged idle's loss exactly as the base table's missing pick is (ia stays N - 1; choice review R4. It was charged nothing, so a table that ranked refused casts higher looked better)
                    if (bc != N && ((found ? isCast(ia) : altCast[a] != 0) || isCast(best))) reg->alt[a] += regretOf(ia); }
                if (bc != N && (iCast || isCast(best))) {
                    ++reg->castDecisions; const double r = regretOf(ii); reg->regU += r;
                    const Candidate* ob = opt[bc].k; const bool iFound = !iausPick || ii != N - 1;
                    SpellTruth tI, tO = TrueSpell(F, c, x, *ob); if (iCast) tI = TrueSpell(F, c, x, *iausPick);
                    auto eps = [](const SpellTruth& t) { return t.effect / std::max(0.05, t.sec); };
                    const char* cause = "none";
                    if (r > 0.0) {
                        bool ranked = false; for (const Scored& q : sv.d.ranked) if (q.idx < acts.size() && &acts[q.idx] == ob) { ranked = true; break; }
                        auto unmeasured = [&](const Candidate* k) { const int bi = k ? BookIndex(c, k->form) : -1; return bi >= 0 && sv.sn.book.entries.size() > static_cast<std::size_t>(bi) && sv.sn.book.entries[static_cast<std::size_t>(bi)].source == SpellSource::Spell && !sv.sn.book.entries[static_cast<std::size_t>(bi)].measured; };
                        if (!iFound) cause = "gate";
                        else if (!ranked) { Cons vb = Cons::kCount; (void)ScoreOne(*ob, c.mind, c.profile, *iaus.T, 5, sv.d.bestHeal, nullptr, &vb); cause = vb == Cons::Los ? "sight" : vb == Cons::HandLost ? "hand" : "gate"; }
                        else if (unmeasured(iausPick) || unmeasured(ob)) cause = "fact";
                        else if (iCast && iausPick->family == Family::CastOffense && ob->family == Family::CastOffense && tI.effect >= 0.0 && tO.effect >= 0.0 && iausPick->f.expRate >= ob->f.expRate && eps(tO) > eps(tI)) cause = "fact";
                        else if (ob->f.sight == Sight::Occluded || ob->f.occludedSec > 0.0f) cause = "sight";
                        else if (ob->f.handLost > 0.0f) cause = "hand";
                        else cause = "weight";
                    }
                    const std::string il = iausPick ? std::string(MFO::Iaus::Name(iausPick->family)) + (iFound ? "" : "!") : std::string("idle");
                    RegRec& row = reg->rows[il + "|" + std::string(MFO::Iaus::Name(ob->family)) + "|" + cause]; ++row.n; if (r > 0.0) { ++row.nReg; row.regU += r; }
                    if (iCast && tI.effect >= 0.0) {   // the world's own numbers for his pick against the best castable spell
                        double bw = 0, bx = 0, bm = 0; for (std::size_t i = 0; i < N; ++i) { if (!isCast(i)) continue; const SpellTruth t = TrueSpell(F, c, x, *opt[i].k); if (t.effect < 0.0) continue;
                            bx = std::max(bx, eps(t)); if (opt[i].k->family == iausPick->family) { bw = std::max(bw, eps(t)); bm = std::max(bm, t.effect / std::max(1.0, t.mp)); } }
                        ++row.nEps; if (bw > 0) row.epsW += std::max(0.0, (bw - eps(tI)) / bw); if (bx > 0) row.epsX += std::max(0.0, (bx - eps(tI)) / bx); if (bm > 0) row.epm += std::max(0.0, (bm - tI.effect / std::max(1.0, tI.mp)) / bm); }
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
            if (claims && k && o.ext == Ext::None && k->family != Family::Posture) MoveNoteFront(F.move->claim[static_cast<std::size_t>(ci)], RankedOf(*k, sv.d, sv.g));
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
