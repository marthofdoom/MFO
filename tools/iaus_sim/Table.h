#pragma once
// iaus_sim/Table.h -- TABLE MODE: the follower run by his class's DEFAULT GAMBIT ROWS, IAUS off. HOST-ONLY.
//
// This is what most players get (IAUS is a per-follower toggle, default off): the gambit table fires its rows and
// between them the engine's own combat AI, shaped by the class package (CSTY, attack mix, leash), is the whole fight.
// The lab runs it to tune the class packages (card C8) and the default rows (card C12) as ONE system.
//
// THE SCAN (the scheduler's shape, MAP section 2): rows top down, the first row whose condition holds AND whose action
// fires wins. A row that is already satisfied (attack: already on that target; equip: already drawn; a spell he cannot
// pay for or does not know) is TRANSPARENT and the scan goes on. After a row fired, the rows BELOW it wait for the
// suppress window (the #26 window, 1.5 s x temperament). Rulings kept: a package never picks a target and never swaps
// a weapon, only rows do.
//
// Conditions and actions are the vocabulary's own opcode strings (native/Vocabulary.h), read from the real class
// table (RealClasses.cpp). A row whose opcode the lab does not model is skipped and COUNTED (Tally::tableSkipped).
#include "iaus_sim/Policy.h"

namespace Sim {

    struct TablePolicy : Policy {
        std::vector<int> lastRow; std::vector<double> until;
        std::vector<std::map<int, double>> rowFired;                // per follower: row -> last fired (row cooldowns)

        // The subject a condition picks: a foe, a friend, or "holds with no subject" (ok, nullptr).
        struct Pick { bool ok = false; const Body* foe = nullptr; const Body* friendly = nullptr; };
        static Pick Cond(const Fight& F, const Ctl& c, const Body& x, const GRow& r) {
            Pick p; const std::string& k = r.cond; const float v = r.param;
            auto foes = [&](auto pred, auto better) { const Body* best = nullptr; for (const Body& o : F.b) { if (o.role != Role::Foe || !Fight::Up(o) || Fight::Dist(x, o) > 4096.0f || !pred(o)) continue; if (!best || better(o, *best)) best = &o; } return best; };
            auto nearer = [&](const Body& a, const Body& b) { return Fight::Dist(x, a) < Fight::Dist(x, b); };
            auto any = [](const Body&) { return true; };
            auto foeIf = [&](auto pred) { p.foe = foes(pred, nearer); p.ok = p.foe != nullptr; return p; };
            const Body& pl = F.b[0];
            if (k == "cond.always") { p.ok = true; p.foe = foes(any, nearer); return p; }
            if (k == "cond.self_hp_pct_below") { p.ok = x.c.hp < v * x.c.hpMax; p.friendly = &x; return p; }
            if (k == "cond.self_mp_pct_below") { p.ok = x.c.mpMax > 0 && x.c.mp < v * x.c.mpMax; p.friendly = &x; return p; }
            if (k == "cond.self_sp_pct_below") { p.ok = x.c.spMax > 0 && x.c.sp < v * x.c.spMax; p.friendly = &x; return p; }
            if (k == "cond.player_hp_pct_below") { p.ok = pl.alive && pl.c.hp < v * pl.c.hpMax; p.friendly = &pl; return p; }
            if (k == "cond.ally_hp_pct_below" || k == "cond.ally_downed") {
                const Body* worst = nullptr; for (const Body& o : F.b) { if (!o.Party() || o.role == Role::Summon || !o.alive || &o == &x) continue;
                    const bool hit = k == "cond.ally_downed" ? o.c.downed : o.c.hp < v * o.c.hpMax; if (hit && (!worst || o.c.hp / o.c.hpMax < worst->c.hp / worst->c.hpMax)) worst = &o; }
                p.ok = worst != nullptr; p.friendly = worst; return p; }
            if (k == "cond.foe_any") return foeIf(any);
            if (k == "cond.foe_lowest_hp") { p.foe = foes(any, [](const Body& a, const Body& b) { return a.c.hp / a.c.hpMax < b.c.hp / b.c.hpMax; }); p.ok = p.foe != nullptr; return p; }
            if (k == "cond.foe_highest_hp") { p.foe = foes(any, [](const Body& a, const Body& b) { return a.c.hp > b.c.hp; }); p.ok = p.foe != nullptr; return p; }
            if (k == "cond.foe_highest_level") { p.foe = foes(any, [](const Body& a, const Body& b) { return a.c.level > b.c.level; }); p.ok = p.foe != nullptr; return p; }
            if (k == "cond.foe_hp_pct_below") { p.foe = foes([&](const Body& o) { return o.c.hp < v * o.c.hpMax; }, [](const Body& a, const Body& b) { return a.c.hp / a.c.hpMax < b.c.hp / b.c.hpMax; }); p.ok = p.foe != nullptr; return p; }
            if (k == "cond.foe_is_caster") return foeIf([](const Body& o) { return o.casterMode || o.tag == "caster" || o.tag == "healer"; });
            if (k == "cond.foe_is_ranged") return foeIf([](const Body& o) { return o.rangedMode || o.tag == "archer"; });
            if (k == "cond.foe_is_undead") return foeIf([](const Body& o) { return o.spec->undead; });
            if (k == "cond.foe_is_unholy") return foeIf([](const Body& o) { return o.spec->undead; });
            if (k == "cond.foe_is_daedra") return foeIf([](const Body& o) { return o.spec->daedra; });
            if (k == "cond.foe_is_dragon") return foeIf([](const Body& o) { return o.spec->dragon; });
            if (k == "cond.foe_fleeing") return foeIf([&](const Body& o) { return F.now < o.fleeUntil; });
            if (k == "cond.foe_blocking") return foeIf([&](const Body& o) { return F.now < o.blockUntil; });
            if (k == "cond.foe_stronger_than_me") return foeIf([&](const Body& o) { return o.c.level > x.c.level; });
            if (k == "cond.foe_weaker_than_me") return foeIf([&](const Body& o) { return o.c.level < x.c.level; });
            if (k == "cond.foe_weak_fire") return foeIf([&](const Body& o) { return F.ResistOf(o, kAvResistFire) < 0; });
            if (k == "cond.foe_weak_frost") return foeIf([&](const Body& o) { return F.ResistOf(o, kAvResistFrost) < 0; });
            if (k == "cond.foe_weak_shock") return foeIf([&](const Body& o) { return F.ResistOf(o, kAvResistShock) < 0; });
            if (k == "cond.foe_attacking_player") return foeIf([&](const Body& o) { return o.c.targetId == pl.c.id; });
            if (k == "cond.foe_attacking_me") return foeIf([&](const Body& o) { return o.c.targetId == x.c.id; });
            if (k == "cond.foe_attacking_me_melee") return foeIf([&](const Body& o) { return o.c.targetId == x.c.id && !o.rangedMode && !o.casterMode; });
            if (k == "cond.foe_attacking_me_ranged") return foeIf([&](const Body& o) { return o.c.targetId == x.c.id && (o.rangedMode || o.casterMode); });
            if (k == "cond.foe_attacking_ally") return foeIf([&](const Body& o) { const Body* tg = F.Find(o.c.targetId); return tg && tg->Party() && tg != &x && tg->role != Role::Player; });
            if (k == "cond.foe_count_at_least") { int n = 0; for (const Body& o : F.b) if (o.role == Role::Foe && Fight::Up(o)) ++n; if (static_cast<float>(n) >= v) return foeIf(any); return p; }
            if (k == "cond.foe_within_range" || k == "cond.foe_beyond_range") {   // TARGET-relative: his current combat target
                const Body* tg = F.Find(c.steer ? c.steer : x.c.targetId); if (!tg || !Fight::Up(*tg)) tg = foes(any, nearer); if (!tg) return p;
                const float d = Fight::Dist(x, *tg); p.ok = k == "cond.foe_within_range" ? d <= v : d > v; p.foe = tg; return p; }
            if (k == "cond.self_out_of_arrows" || k == "cond.self_out_of_bolts") { p.ok = static_cast<float>(x.arrows) < std::max(1.0f, v); p.friendly = &x; return p; }
            p.ok = false; return p;   // not modelled: the caller counts it
        }
        static bool Known(const std::string& cond) {
            static const char* k[] = { "cond.always", "cond.self_hp_pct_below", "cond.self_mp_pct_below", "cond.self_sp_pct_below", "cond.player_hp_pct_below", "cond.ally_hp_pct_below", "cond.ally_downed", "cond.foe_any",
                "cond.foe_lowest_hp", "cond.foe_highest_hp", "cond.foe_highest_level", "cond.foe_hp_pct_below", "cond.foe_is_caster", "cond.foe_is_ranged", "cond.foe_is_undead", "cond.foe_is_unholy", "cond.foe_is_daedra", "cond.foe_is_dragon",
                "cond.foe_fleeing", "cond.foe_blocking", "cond.foe_stronger_than_me", "cond.foe_weaker_than_me", "cond.foe_weak_fire", "cond.foe_weak_frost", "cond.foe_weak_shock", "cond.foe_attacking_player", "cond.foe_attacking_me",
                "cond.foe_attacking_me_melee", "cond.foe_attacking_me_ranged", "cond.foe_attacking_ally", "cond.foe_count_at_least", "cond.foe_within_range", "cond.foe_beyond_range", "cond.self_out_of_arrows", "cond.self_out_of_bolts" };
            for (const char* s : k) if (cond == s) return true;
            return false;
        }
        // His best known spell for a row's role (-1 = none he can pay for).
        static int SpellFor(const Fight& F, const Ctl& c, const Body& x, int role, const Body* foe, bool selfOnly) {
            int best = -1; float bv = -1;
            for (std::size_t i = 0; i < c.book.entries.size(); ++i) {
                const SpellEntry& e = c.book.entries[i]; if (e.source != SpellSource::Spell || e.drop != Prune::Drop::Keep || e.cost > x.c.mp) continue;
                const SimSpell& s = c.build->spells[i]; float v = -1; const float dmg = s.dmg + s.dot * s.dotDur;
                switch (role) {
                case kRoleAttack: case kRoleDestruction: if (e.nature == Prune::Nature::Offense && !(foe && s.e.resistAv >= 0 && F.ResistOf(*foe, s.e.resistAv) >= 100.0f)) v = dmg; break;
                case kRoleFire: if (e.nature == Prune::Nature::Offense && s.element == "fire") v = dmg; break;
                case kRoleFrost: if (e.nature == Prune::Nature::Offense && s.element == "frost") v = dmg; break;
                case kRoleShock: if (e.nature == Prune::Nature::Offense && s.element == "shock") v = dmg; break;
                case kRoleArea: if (e.nature == Prune::Nature::Offense && e.areaUnits > 0) v = dmg; break;
                case kRoleHeal: if (e.nature == Prune::Nature::Heal && (selfOnly ? true : s.healOther)) v = s.heal + s.hot * s.hotDur; break;
                case kRoleSummon: case kRoleReanimate: if (e.nature == Prune::Nature::Summon && c.summonUntil[i] <= F.now) v = 1; break;
                case kRoleWard: if (e.nature == Prune::Nature::Ward) v = 1; break;
                case kRoleArmor: if (s.armor > 0) { bool on = false; for (const auto& pr : c.active[i]) if (pr.second > F.now) on = true; if (!on) v = s.armor; } break;
                case kRoleParalyze: if (s.ctrlPar) v = s.ctrlDur; break;
                case kRoleFear: if (s.ctrlFlee && !s.turnUndead) v = s.ctrlDur; break;
                case kRoleCalm: if (s.ctrlCalm) v = s.ctrlDur; break;
                case kRoleTurnUndead: if (s.turnUndead) v = s.ctrlDur; break;
                default: break;
                }
                if (v > bv) { bv = v; best = static_cast<int>(i); }
            }
            return best;
        }
        void Serve(Fight& F, int ci) override {
            if (lastRow.size() < F.ctl.size()) { lastRow.assign(F.ctl.size(), -1); until.assign(F.ctl.size(), 0.0); rowFired.resize(F.ctl.size()); }
            Ctl& c = F.ctl[static_cast<std::size_t>(ci)]; Body& x = F.b[static_cast<std::size_t>(c.body)]; Tally& t = F.t; const bool subj = ci == 0;
            if (!Fight::Up(x)) return;
            if (subj) ++t.ticks;
            const auto& rows = c.rows;
            for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
                const GRow& r = rows[static_cast<std::size_t>(ri)];
                if (lastRow[static_cast<std::size_t>(ci)] >= 0 && ri > lastRow[static_cast<std::size_t>(ci)] && F.now < until[static_cast<std::size_t>(ci)]) break;   // the #26 window: rows below the one that fired wait
                if (!Known(r.cond)) { if (subj) ++t.tableSkipped; continue; }
                if (r.cooldown > 0) { auto it = rowFired[static_cast<std::size_t>(ci)].find(ri); if (it != rowFired[static_cast<std::size_t>(ci)].end() && F.now - it->second < r.cooldown) continue; }
                const Pick p = Cond(F, c, x, r); if (!p.ok) continue;
                Candidate k; Family fam = Family::kCount; bool fired = false;
                auto fire = [&]() { const Fired fr = Fire(F, ci, k); if (fr == Fired::Yes) { fired = true; if (subj) { ++t.fired; ++t.hist[static_cast<std::size_t>(k.family)];
                            t.actSec[static_cast<std::size_t>(k.family)] += k.family == Family::Attack ? TM::pump * static_cast<double>(F.ctl.size()) : std::max(0.0, x.actEnd - F.now); } } };
                if (r.act == "act.attack") { if (!p.foe || c.steer == p.foe->c.id) continue; k.family = Family::Attack; k.target = p.foe->c.id; fire(); }
                else if (r.act == "act.power_attack") { if (!p.foe) continue; k.family = Family::PowerAttack; k.target = p.foe->c.id; fire(); }
                else if (r.act == "act.taunt") { if (!p.foe || F.now < p.foe->tauntUntil || !(c.drawn == PostureKind::Melee || c.drawn == PostureKind::Silver)) continue;
                    bool busy = x.act == Act::Taunt && F.now < x.actEnd; for (const Body& o : F.b) if (o.role == Role::Foe && o.alive && o.tauntBy == x.c.id && F.now < o.tauntUntil) busy = true; if (busy) continue;
                    k.family = Family::Taunt; k.target = p.foe->c.id; fire(); }
                else if (r.act == "act.flee") { k.family = Family::Retreat; fire(); }
                else if (r.act == "act.drink_health_potion") { if (c.book.kit.potionsHealth <= 0) continue; k.family = Family::DrinkHealth; fire(); }
                else if (r.act == "act.drink_stamina_potion") { if (c.book.kit.potionsStamina <= 0) continue; k.family = Family::DrinkStamina; fire(); }
                else if (r.act == "act.drink_magicka_potion") { if (c.book.kit.potionsMagicka <= 0) continue; k.family = Family::DrinkMagicka; fire(); }
                else if (r.act == "act.equip_melee" || r.act == "act.equip_ranged") {
                    const PostureKind want = r.act == "act.equip_melee" ? PostureKind::Melee : PostureKind::Bow;
                    const bool has = want == PostureKind::Melee ? c.book.kit.melee : (c.book.kit.bow && x.arrows > 0) || (c.book.kit.crossbow && x.arrows > 0);
                    if (!has || c.drawn == want || x.hitAt >= 0 || (x.actIaus && F.now < x.actEnd)) continue;
                    c.drawn = want; x.act = Act::Swap; x.actEnd = F.now + TM::swap; x.hitAt = -1; x.actIaus = true; fired = true; if (subj) ++t.postureSwaps; }
                else if (r.act == "act.cast_self" || r.act == "act.cast_target" || r.act == "act.cast_player") {
                    if (!c.gate.spellsOnClass && !c.gate.spellsFromMastered) continue;
                    const bool self = r.act == "act.cast_self"; const Body* rec = r.act == "act.cast_player" ? &F.b[0] : self ? &x : p.friendly;
                    const int si = SpellFor(F, c, x, r.role, p.foe, self || rec == &x); if (si < 0) continue;
                    const SpellEntry& e = c.book.entries[static_cast<std::size_t>(si)]; fam = Prune::FamilyOf(e.nature); if (fam == Family::kCount) continue;
                    k.family = fam; k.form = e.form;
                    if (fam == Family::CastOffense || fam == Family::CastControl) { if (!p.foe) continue; k.target = p.foe->c.id; }
                    else if (fam == Family::CastHeal || fam == Family::CastBuff) { if (rec && rec != &x) { if (e.selfDelivery) continue; k.target = rec->c.id; } if (fam == Family::CastHeal && (rec ? rec : &x)->c.hp >= 0.999f * (rec ? rec : &x)->c.hpMax) continue; }
                    fire(); }
                else { if (subj) ++t.tableSkipped; continue; }   // an action the lab does not model (loot, steal, sneak, shout ...)
                if (fired) { if (lastRow[static_cast<std::size_t>(ci)] != ri && subj) ++t.switches; lastRow[static_cast<std::size_t>(ci)] = ri; until[static_cast<std::size_t>(ci)] = F.now + c.window; rowFired[static_cast<std::size_t>(ci)][ri] = F.now; return; }
            }
        }
    };

}
