// iaus/Generate.cpp -- the PURE candidate generator (contract in iaus/Generate.h). Card I4.
// Standard headers only: tools/iaus_harness/run.sh builds this file with the host compiler.
#include "iaus/Generate.h"

#include "iaus/Estimate.h"

#include <algorithm>
#include <cmath>

namespace MFO::Iaus {

    namespace {

        using Prune::Finite;
        using Prune::SafeRatio;

        float Pct(float a_cur, float a_max) { return std::clamp(SafeRatio(a_cur, a_max, 1.0f), 0.0f, 1.0f); }
        bool  Readable(const Combatant& a_u) { return std::isfinite(a_u.hpMax) && a_u.hpMax > 0.0f; }

        float Between(const Combatant& a_a, const Combatant& a_b) {
            const float dx = a_a.x - a_b.x, dy = a_a.y - a_b.y, dz = a_a.z - a_b.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            return std::isfinite(d) ? d : 1.0e9f;   // an unreadable position is "far", never "inside the area"
        }

        Tri Note(const std::vector<SpellTargetNote>& a_notes, std::uint32_t a_spell, std::uint32_t a_target) {
            for (const auto& n : a_notes)
                if (n.spell == a_spell && n.target == a_target) return n.answer;
            return Tri::Unknown;
        }

        struct Ctx {
            const Snapshot&  s;
            const ClassGate& g;
            const Tuning&    t;
            Generated&       out;
            float selfHp = 1.0f, selfSp = 1.0f, selfMp = 1.0f;
            int   foeCount = 0;
            int   partyCount = 1;
            std::vector<const Combatant*> top;   // the P3 cut, most relevant first
            // estimates (iaus/Estimate.h); unknown = +inf / 0
            float selfIncoming = 0.0f;
            float selfTtl = kInf;
            float playerTtl = kInf;

            void Push(Candidate&& a_c) {
                ++out.perFamily[static_cast<std::size_t>(a_c.family)];
                (a_c.family == Family::Posture ? out.postures : out.actions).push_back(std::move(a_c));
            }
            // What every family may read about the follower and the fight.
            void Common(Facts& f) const {
                f.selfHp = selfHp;
                f.selfStamina = selfSp;
                f.selfMagicka = selfMp;
                f.combatSec = (std::max)(0.0f, Finite(s.combatSec, 0.0f));
                f.foeCount = foeCount;
                f.partyCount = partyCount;
                f.emergency = out.emergency;
                f.selfTtl = selfTtl;
                f.playerTtl = playerTtl;
            }
            void Foe(Facts& f, const Combatant& a_foe, bool a_ownRay) const {
                f.dist = (std::max)(0.0f, Finite(a_foe.dist, 0.0f));
                f.sight = a_ownRay ? a_foe.sight : a_foe.sightEngine;
                f.foeLevelRatio = SafeRatio(static_cast<float>(a_foe.level), static_cast<float>(s.self.level), 1.0f);
                f.foeHpPct = Pct(a_foe.hp, a_foe.hpMax);
                f.foeCaster = a_foe.caster;
                f.foeOnMe = a_foe.targetId != 0 && a_foe.targetId == s.self.id;
                f.foeOnPlayer = a_foe.targetId != 0 && a_foe.targetId == s.playerId;
                f.foeOnAlly = a_foe.targetIsAlly;
                f.playerTarget = s.playerTargetId != 0 && a_foe.id == s.playerTargetId;
                f.currentTarget = s.currentTargetId != 0 && a_foe.id == s.currentTargetId;
                f.foeBlocking = a_foe.blocking;
                f.foeStronger = a_foe.level > s.self.level;
                f.foeControlled = a_foe.paralysed || a_foe.fleeing;
                f.foeTaunted = a_foe.taunted;
                f.foeOutOfMeleeReach = a_foe.outOfMeleeReach;
                f.foeUnholy = a_foe.unholy;
            }
            Candidate Make(Family a_f, const Combatant* a_target) const {
                Candidate c;
                c.family = a_f;
                if (a_target) { c.target = a_target->id; c.targetName = a_target->name; }
                Common(c.f);
                return c;
            }
        };

        // ── attack / power attack / taunt / retreat ──────────────────────────────────────────────────────
        void Swings(Ctx& x) {
            const Snapshot& s = x.s;
            const bool ranged = s.drawn == PostureKind::Bow || s.drawn == PostureKind::Staff;
            const bool melee  = s.drawn == PostureKind::Melee || s.drawn == PostureKind::Silver;
            for (const Combatant* foe : x.top) {
                // Today's reach gate: a melee-only follower never names a foe airborne out of his swing.
                if (!(s.meleeOnly && foe->outOfMeleeReach)) {
                    Candidate c = x.Make(Family::Attack, foe);
                    x.Foe(c.f, *foe, ranged);
                    c.role = ranged ? RoleTag::Ranged : RoleTag::Melee;
                    c.f.meleeRange = !ranged;
                    if (!ranged) c.f.chase = (std::max)(0.0f, Finite(s.chaseCap, 0.0f));   // act.attack names the target; the engine closes
                    c.f.reach = (std::max)(0.0f, Finite(ranged ? s.chaseCap : s.meleeReach, 0.0f));
                    x.Push(std::move(c));
                }
                if (x.g.powerAttack && melee && !foe->outOfMeleeReach) {
                    Candidate c = x.Make(Family::PowerAttack, foe);
                    x.Foe(c.f, *foe, false);
                    c.role = RoleTag::Melee;
                    c.f.meleeRange = true;
                    c.f.reach = (std::max)(0.0f, Finite(s.meleeReach, 0.0f));
                    // The class row's cooldown; a class with no declared one (and the tree OFF, marth Q63) gets the
                    // tuning floor, so "any follower may power attack" is never "every lap".
                    const float rowCd = (std::max)(0.0f, Finite(x.g.powerCooldownSec, 0.0f));
                    c.f.hardCooldownSec = rowCd > 0.0f ? rowCd : (std::max)(0.0f, Finite(x.t.rowlessPowerCooldownSec, 0.0f));
                    x.Push(std::move(c));
                }
                const bool onParty = foe->targetIsAlly || (foe->targetId != 0 && foe->targetId == s.playerId);
                // Card I6 (sim finding R1): no candidate while his own taunt is in flight or still holds a foe (the
                // executor refuses then), the FoeTaunted gate reads Combatant::taunted (filled from the Taunt
                // module's own pins), and the taunt carries a HARD cooldown: the class row's, else the tuning floor.
                if (s.tauntEnabled && !s.tauntBusy && x.g.taunt && s.meleeInRightHand && onParty) {
                    Candidate c = x.Make(Family::Taunt, foe);
                    x.Foe(c.f, *foe, false);
                    c.f.meleeRange = false;
                    c.f.reach = (std::max)(0.0f, Finite(s.tauntReach, 0.0f));
                    const float rowCd = (std::max)(0.0f, Finite(x.g.tauntCooldownSec, 0.0f));
                    c.f.hardCooldownSec = rowCd > 0.0f ? rowCd : (std::max)(0.0f, Finite(x.t.rowlessTauntCooldownSec, 0.0f));
                    // What he would be taking with this foe on him too (a foe already on him adds nothing).
                    c.f.tauntTtl = Estimate::Ttl(s.self.hp, x.selfIncoming + (foe->targetId == s.self.id ? 0.0f : Estimate::Dps(*foe, s.self)));
                    c.f.tauntChance = Estimate::TauntChance(s.self.level, foe->level);   // ruling Q69
                    x.Push(std::move(c));
                }
            }
            if (x.g.disengage && x.foeCount > 0) {
                Candidate c = x.Make(Family::Retreat, nullptr);
                c.f.disengageClass = true;
                x.Push(std::move(c));
            }
        }

        // ── the three drinks (P2: no stock, no candidate) ────────────────────────────────────────────────
        void Drinks(Ctx& x) {
            const Kit& k = x.s.book.kit;
            const auto drink = [&](Family a_f, std::int32_t a_stock) {
                if (a_stock <= 0) return;
                Candidate c = x.Make(a_f, nullptr);
                c.f.stock = a_stock;
                x.Push(std::move(c));
            };
            drink(Family::DrinkHealth, k.potionsHealth);
            drink(Family::DrinkStamina, k.potionsStamina);
            drink(Family::DrinkMagicka, k.potionsMagicka);
        }

        // ── posture: the categories he carries, against the foe he is fighting ───────────────────────────
        void Postures(Ctx& x, const std::vector<const Combatant*>& a_foes) {
            const Snapshot& s = x.s;
            const Kit&      k = s.book.kit;
            const Combatant* tgt = nullptr;
            for (const Combatant* f : a_foes)
                if (s.currentTargetId != 0 && f->id == s.currentTargetId) { tgt = f; break; }
            if (!tgt)
                for (const Combatant* f : a_foes)
                    if (!tgt || f->dist < tgt->dist) tgt = f;
            if (!tgt) return;
            const bool gated = x.g.resolved && x.g.strict;
            const auto posture = [&](PostureKind a_kind, RoleTag a_role, bool a_classAllows) {
                Candidate c = x.Make(Family::Posture, nullptr);   // the posture Key's target stays 0 (I2 convention)
                c.role = a_role;
                c.f.posture = a_kind;
                c.f.postureCurrent = s.drawn == a_kind;
                c.f.dist = (std::max)(0.0f, Finite(tgt->dist, 0.0f));
                c.f.foeOutOfMeleeReach = tgt->outOfMeleeReach;
                c.f.foeUnholy = tgt->unholy;
                c.f.outOfAmmo = !((k.bow && k.arrows > 0) || (k.crossbow && k.bolts > 0));
                c.f.staffCharged = k.staffCharged;
                c.f.classAllowsWeapon = !gated || a_classAllows;
                x.Push(std::move(c));
            };
            if (k.melee) posture(PostureKind::Melee, RoleTag::Melee, x.g.weaponMelee);
            if (k.bow || k.crossbow)
                posture(PostureKind::Bow, RoleTag::Ranged, (k.bow && x.g.weaponBow) || (k.crossbow && x.g.weaponCrossbow));
            if (k.staff) posture(PostureKind::Staff, RoleTag::Staff, x.g.weaponStaff);
            if (k.silver) posture(PostureKind::Silver, RoleTag::Silver, x.g.weaponMelee);
        }

        // ── casts ────────────────────────────────────────────────────────────────────────────────────────
        void Casts(Ctx& x, const std::vector<const Combatant*>& a_foes) {
            const Snapshot& s = x.s;
            const bool anyBusy  = s.handBusyLeft || s.handBusyRight;
            const bool bothBusy = s.handBusyLeft && s.handBusyRight;
            const float pool    = Finite(s.self.mp, 0.0f);
            const bool poolOk   = std::isfinite(s.self.mpMax) && s.self.mpMax > 0.0f;
            // class_fit (ruling Q34) comes from the LIVE gate, read this tick. SpellEntry::onClass / fromMastered
            // are the same bits as the book measured them (up to 5 s old): the log and the board read those.
            const bool onClass      = !x.g.resolved || x.g.spellsOnClass;
            const bool fromMastered = x.g.resolved && x.g.spellsFromMastered;

            // Recipients of a heal or buff: himself first, then the readable allies.
            std::vector<const Combatant*> party{ &s.self };
            for (const auto& a : s.allies)
                if (Readable(a)) party.push_back(&a);

            bool casterOnMe = false;
            for (const Combatant* f : a_foes)
                if (f->caster && f->targetId != 0 && f->targetId == s.self.id) { casterOnMe = true; break; }

            for (const SpellEntry& e : s.book.entries) {
                if (e.source == SpellSource::Staff) continue;        // staves: the posture layer
                // Scrolls (card S3): a candidate only once a scroll can be read as a cast (Snapshot::scrollRoad), for a
                // scroll user (ruling Q36), with one in stock. Offense / control / heal / buff only (the scroll sets).
                const bool scroll = e.source == SpellSource::Scroll;
                if (scroll && !(s.scrollRoad && x.g.scrollUser && e.count > 0)) continue;
                if (scroll && (e.nature == Prune::Nature::Summon || e.nature == Prune::Nature::Ward)) continue;
                if (e.drop != Prune::Drop::Keep) continue;           // P1 + "never generated"
                const Family fam = Prune::FamilyOf(e.nature);
                if (fam == Family::kCount) continue;
                // Ruling Q34: an off-class spell is a candidate ONLY when a mastered class grants it. (A scroll is
                // Q36's: the scroll-user gate above.)
                if (!scroll && !onClass && !fromMastered) { ++x.out.droppedClass; continue; }
                // P1: a two-hander needs both hands; while a heal stands on one it is not a candidate.
                if (e.twoHanded ? anyBusy : bothBusy) { ++x.out.droppedHand; continue; }
                // P2: cost over the pool drops every pair of this spell. An unreadable pool drops a priced spell.
                const bool priced = e.cost > 0.0f;
                if (priced && (!poolOk || !Prune::Affordable(e.cost, pool))) { ++x.out.droppedCost; continue; }

                const auto base = [&](const Combatant* a_target) {
                    Candidate c = x.Make(scroll ? Family::Scroll : fam, a_target);
                    if (scroll) {
                        c.scrollOf = fam == Family::CastHeal || fam == Family::CastBuff ? fam : Family::CastOffense;
                        c.f.stock = e.count;
                    }
                    c.role = e.role;
                    c.form = e.form;
                    c.formName = e.name;
                    if (priced) { c.f.cost = e.cost; c.f.magicka = pool; c.f.magickaMax = s.self.mpMax; }
                    c.f.handFree = true;   // the P1 test above already dropped a held hand
                    c.f.onClass = scroll ? true : onClass;
                    c.f.fromMastered = fromMastered;
                    return c;
                };

                switch (e.nature) {
                case Prune::Nature::Offense:
                case Prune::Nature::Control: {
                    // Reach: Touch = a swing's reach; a Self-delivered blast reaches its own area; else the aimed
                    // reach (0 = the record gives none: range is then not applicable).
                    const float reach = e.touch ? s.meleeReach : (e.selfDelivery ? e.areaUnits : e.reach);
                    if (e.selfDelivery && !(e.areaUnits > 0.0f)) { ++x.out.droppedTarget; break; }
                    for (const Combatant* foe : x.top) {
                        if (e.undeadOnly && !foe->undead) { ++x.out.droppedTarget; continue; }   // its own target rule
                        Candidate c = base(foe);
                        x.Foe(c.f, *foe, true);
                        c.f.meleeRange = false;
                        c.f.reach = (std::max)(0.0f, Finite(reach, 0.0f));
                        c.f.willWork = Note(s.willWork, e.form, foe->id);
                        if (e.undeadOnly) c.f.targetCond = Tri::Yes;
                        if (e.resistAv >= 0) c.f.resist = Finite(foe->Resist(e.resistAv), 0.0f);
                        if (e.nature == Prune::Nature::Offense) {   // what this cast does to THIS foe, against his best castable option on it
                            c.f.expDamage = Estimate::SpellOn(e, *foe);
                            c.f.foeHealth = (std::max)(0.0f, Finite(foe->hp, 0.0f));
                            float best = 0.0f;
                            for (const SpellEntry& o : s.book.entries) {
                                if (o.source != SpellSource::Spell || o.drop != Prune::Drop::Keep || o.nature != Prune::Nature::Offense) continue;
                                if (o.cost > 0.0f && (!poolOk || !Prune::Affordable(o.cost, pool))) continue;
                                best = (std::max)(best, Estimate::SpellOn(o, *foe));
                            }
                            c.f.bestDamage = best;
                        }
                        if (e.areaUnits > 0.0f) {
                            const Combatant& centre = e.selfDelivery ? s.self : *foe;
                            int inArea = 0;
                            for (const Combatant* o : a_foes)
                                if (Between(*o, centre) <= e.areaUnits) ++inArea;
                            bool safe = true;   // marth: never splash the party (his own blast does not hit him)
                            for (const auto& a : s.allies)
                                if (Between(a, centre) <= e.areaUnits) { safe = false; break; }
                            c.f.hasArea = true;
                            c.f.foesInArea = static_cast<float>(inArea);
                            c.f.splashSafe = safe;
                        }
                        x.Push(std::move(c));
                    }
                    break;
                }
                case Prune::Nature::Heal:
                case Prune::Nature::Buff: {
                    const bool heal = e.nature == Prune::Nature::Heal;
                    for (const Combatant* r : party) {
                        const bool self = r == &s.self;
                        if (!self && e.selfDelivery) continue;   // self-delivery = himself only (design section 2)
                        const float hp = self ? x.selfHp : Pct(r->hp, r->hpMax);
                        if (heal && !Prune::Hurt(hp)) continue;   // hurt targets only
                        if (!heal && std::find(e.activeOn.begin(), e.activeOn.end(), r->id) != e.activeOn.end()) {
                            ++x.out.droppedTarget;                // already active
                            continue;
                        }
                        // Actuation::HealInReach's test: inside the reach and not measured Occluded.
                        if (!self && ((e.reach > 0.0f && r->dist > e.reach) || r->sight == Sight::Occluded)) {
                            ++x.out.droppedTarget;
                            continue;
                        }
                        Candidate c = base(self ? nullptr : r);
                        if (self) c.targetName = s.self.name;
                        c.f.inHealReach = Tri::Yes;
                        c.f.sight = self ? Sight::None : r->sight;
                        c.f.recipientHp = hp;
                        c.f.recipientIsPlayer = r->isPlayer;
                        c.f.recipientDowned = r->downed;
                        if (heal) {
                            c.f.expHeal = Estimate::Pos(e.heal);
                            c.f.recipientMissing = (std::max)(0.0f, Finite(r->hpMax, 0.0f) - Finite(r->hp, 0.0f));
                            c.f.recipientTtl = self ? x.selfTtl : Estimate::Ttl(r->hp, Estimate::IncomingOn(s, *r));
                        }
                        if (heal) {
                            c.f.alreadyActive = Note(s.liveStreams, e.form, self ? 0u : r->id) == Tri::Yes;
                        } else {
                            c.f.meleeBuff = e.meleeBuff;
                            for (const Combatant* foe : a_foes)
                                if (Between(*foe, *r) <= s.meleeReach) { c.f.recipientInMelee = true; break; }
                        }
                        x.Push(std::move(c));
                    }
                    break;
                }
                case Prune::Nature::Summon: {
                    if (e.liveSummon) { ++x.out.droppedTarget; break; }   // no live summon from it
                    Candidate c = base(nullptr);
                    c.f.liveSummon = false;
                    x.Push(std::move(c));
                    break;
                }
                case Prune::Nature::Ward: {
                    Candidate c = base(nullptr);
                    c.f.casterOnMe = casterOnMe;
                    c.f.alreadyActive = std::find(e.activeOn.begin(), e.activeOn.end(), s.self.id) != e.activeOn.end();
                    x.Push(std::move(c));
                    break;
                }
                case Prune::Nature::Unknown: break;
                }
            }
        }

    }

    Generated Generate(const Snapshot& a_s, const ClassGate& a_gate, const Tuning& a_t) {
        Generated out;
        if (!Readable(a_s.self)) { out.selfUnreadable = true; return out; }   // unloaded: skip, never divide

        Ctx x{ a_s, a_gate, a_t, out, 1.0f, 1.0f, 1.0f, 0, 1, {} };
        x.selfHp = Pct(a_s.self.hp, a_s.self.hpMax);
        x.selfSp = Pct(a_s.self.sp, a_s.self.spMax);
        x.selfMp = Pct(a_s.self.mp, a_s.self.mpMax);
        x.selfIncoming = Estimate::IncomingOn(a_s, a_s.self);
        x.selfTtl = Estimate::Ttl(a_s.self.hp, x.selfIncoming);
        for (const Combatant& a : a_s.allies)
            if (a.isPlayer && Readable(a)) { x.playerTtl = Estimate::Ttl(a.hp, Estimate::IncomingOn(a_s, a)); break; }

        out.worstPartyHp = x.selfHp;
        for (const auto& a : a_s.allies) {
            if (!Readable(a)) continue;
            ++x.partyCount;
            out.worstPartyHp = (std::min)(out.worstPartyHp, Pct(a.hp, a.hpMax));
        }
        out.emergency = EmergencyLevel(out.worstPartyHp, a_t);

        // P3: the readable foes, then the top 5 by relevance.
        std::vector<const Combatant*>   foes;
        std::vector<Prune::FoeIn>  rel;
        for (const auto& f : a_s.foes) {
            if (!Readable(f) || f.id == 0 || f.id == a_s.self.id) continue;
            foes.push_back(&f);
            Prune::FoeIn in;
            in.id = f.id;
            in.dist = f.dist;
            in.hpPct = Pct(f.hp, f.hpMax);
            in.onMe = f.targetId != 0 && f.targetId == a_s.self.id;
            in.onPlayer = f.targetId != 0 && f.targetId == a_s.playerId;
            in.onAlly = f.targetIsAlly;
            in.current = a_s.currentTargetId != 0 && f.id == a_s.currentTargetId;
            in.playerTarget = a_s.playerTargetId != 0 && f.id == a_s.playerTargetId;
            rel.push_back(in);
        }
        x.foeCount = static_cast<int>(foes.size());
        for (const std::size_t i : Prune::TopFoes(rel)) x.top.push_back(foes[i]);
        out.foesSeen = foes.size();
        out.foesKept = x.top.size();

        Swings(x);
        if (a_s.hasBook) {
            Drinks(x);
            Postures(x, foes);
            Casts(x, foes);
        }
        return out;
    }

    std::vector<SpellTarget> TopSpellPairs(const Decision& a_d, std::span<const Candidate> a_cands, std::size_t a_n) {
        std::vector<SpellTarget> out;
        for (const auto& r : a_d.ranked) {
            if (out.size() >= a_n) break;
            if (r.idx >= a_cands.size()) continue;
            const Candidate& c = a_cands[r.idx];
            if (c.family != Family::CastOffense && c.family != Family::CastControl) continue;
            if (c.form == 0 || c.target == 0) continue;
            const SpellTarget st{ c.form, c.target };
            if (std::find(out.begin(), out.end(), st) == out.end()) out.push_back(st);
        }
        return out;
    }

}
