// iaus/Generate.cpp -- the PURE candidate generator (contract in iaus/Generate.h). Card I4.
// Standard headers only: tools/iaus_harness/run.sh builds this file with the host compiler.
#include "iaus/Generate.h"

#include "iaus/Disengage.h"   // card G3: "has a foe found him" (the seam to G2 Layer 1)
#include "iaus/Estimate.h"
#include "iaus/Reposition.h"   // batch I: the spell range, the back-off need, the hidden step

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
            // G2 Layer 1 (iaus/Hide.h): hidden from every listed foe while he sneaks (Yes), seen by one (No), or not known.
            Tri  unseen = Tri::Unknown;
            bool hold = false;          // the hold row stands: unseen Yes, a weapon posture
            bool strikeReady = false;   // some unaware foe is in reach (melee) or in sight (bow): the blow from stealth is ready
            bool holdQuiet = false;     // hold && !strikeReady: nothing LOUD is generated (no taunt, no cast whose sound the engine emits)
            bool guard = false;         // batch I item 4: a held guard stands (Snapshot::guardHeld): nothing that would drop it is generated
            bool fleeRun = false;       // card G3: a non-sneak flee run stands: no offense is generated, his own heals are boosted
            // An unaware foe he could strike now. Melee: inside the swing reach and not airborne beyond it; bow: in the
            // follower's own ray and inside the chase cap. A caster's posture (None / Staff) never holds or strikes.
            [[nodiscard]] bool StrikeReady(const Combatant& a_foe) const { return Reposition::StrikeReady(s, a_foe); }

            void Push(Candidate&& a_c) {
                ++out.perFamily[static_cast<std::size_t>(a_c.family)];
                (a_c.family == Family::Posture ? out.postures : out.actions).push_back(std::move(a_c));
            }
            // The reach of a swing he times himself (PowerAttack, Bash): the learned one when there is one (ruling Q68),
            // else fMeleeReach (MFO-B687). No other family reads the learned value (Q70).
            [[nodiscard]] float SwingReach() const {
                const float learned = Finite(s.swingReach, 0.0f);
                return (std::max)(0.0f, learned > 0.0f ? learned : Finite(s.meleeReach, 0.0f));
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
                f.mortal = s.mortal;
                f.selfSneaking = s.selfSneaking;
                f.unseen = unseen;
                f.strikeReady = strikeReady;
            }
            void Foe(Facts& f, const Combatant& a_foe, bool a_ownRay) const {
                f.dist = (std::max)(0.0f, Finite(a_foe.dist, 0.0f));
                f.sight = a_ownRay ? a_foe.sight : a_foe.sightEngine;
                f.occludedSec = Finite(a_ownRay ? a_foe.occludedSec : a_foe.occludedSecEngine, 0.0f);   // the age of the ray f.sight reads
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
                f.foeHelpless = a_foe.paralysed;
                f.foeTaunted = a_foe.taunted;
                f.foeOutOfMeleeReach = a_foe.outOfMeleeReach;
                f.foeUnholy = a_foe.unholy;
                f.foeUnaware = a_foe.detectsMe == Tri::No ? Tri::Yes : (a_foe.detectsMe == Tri::Yes ? Tri::No : Tri::Unknown);
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
            if (x.fleeRun) return;   // card G3: during the flee he heals, he does not swing, shout or start another retreat
            const bool ranged = s.drawn == PostureKind::Bow || s.drawn == PostureKind::Staff;
            const bool melee  = s.drawn == PostureKind::Melee || s.drawn == PostureKind::Silver;
            for (const Combatant* foe : x.top) {
                // G2 Layer 1: while the hold row stands (he sneaks, no listed foe detects him) a blow is named only on a foe he
                // can strike now. An unaware foe still out of reach is waited for, not chased: the chase is the noise.
                const bool holdsOff = x.hold && foe->detectsMe == Tri::No && !x.StrikeReady(*foe);
                // Today's reach gate: a melee-only follower never names a foe airborne out of his swing.
                if (x.guard) break;   // a held guard: no blow, no taunt (Fire refuses them; his Retreat below is not one)
                if (!holdsOff && !(s.meleeOnly && foe->outOfMeleeReach)) {
                    Candidate c = x.Make(Family::Attack, foe);
                    x.Foe(c.f, *foe, ranged);
                    c.role = ranged ? RoleTag::Ranged : RoleTag::Melee;
                    c.f.meleeRange = !ranged;
                    if (!ranged) c.f.chase = (std::max)(0.0f, Finite(s.chaseCap, 0.0f));   // act.attack names the target; the engine closes
                    c.f.reach = (std::max)(0.0f, Finite(ranged ? s.chaseCap : s.meleeReach, 0.0f));
                    x.Push(std::move(c));
                }
                if (!holdsOff && x.g.powerAttack && melee && !foe->outOfMeleeReach) {
                    Candidate c = x.Make(Family::PowerAttack, foe);
                    x.Foe(c.f, *foe, false);
                    c.role = RoleTag::Melee;
                    c.f.meleeRange = true;
                    c.f.reach = x.SwingReach();
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
                if (s.tauntEnabled && !s.tauntBusy && x.g.taunt && s.meleeInRightHand && onParty && !x.holdQuiet) {   // G2: a shout is noise
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
                c.f.rangedDrawn = ranged;
                c.f.threatTtl = Estimate::Ttl(s.self.hp, Estimate::ThreatOn(s, s.self));
                for (const Combatant& f : s.foes)   // the nearest foe on him that has to close to hurt him
                    if (f.targetId != 0 && f.targetId == s.self.id && !f.caster && !f.rangedWeapon && !f.paralysed && !f.fleeing && std::isfinite(f.dist))
                        c.f.meleeThreatDist = (std::min)(c.f.meleeThreatDist, (std::max)(0.0f, f.dist));
                x.Push(std::move(c));
            }
        }

        // ── G2 Layer 1: the hold row (Family::Sneak). He sneaks and no listed foe's group detects him: stay still and quiet
        // (no attack, no cast, no chase; MFO names nothing, the Wait idiom). It stands only while no blow from stealth is ready
        // (Cons::HoldStill gates it to 0 the moment one is); then the Attack / Power attack on that foe carry Cons::BoostStrike
        // and the engine's own sneak attack does the damage. No actor value is written anywhere.
        void Holds(Ctx& x) {
            if (!x.hold) return;
            Candidate c = x.Make(Family::Sneak, nullptr);
            x.Push(std::move(c));
            // G18 (batch I item 5, marth 11-12 Q-SIM-3): the hold row has no exit. Hidden with no blow ready, he steps quietly toward a foe
            // that does not detect him to gain a strike (blade) or a line of fire (bow), staying hidden: a short H3 run (Concealment Kind::Step)
            // to a point the main thread picked. Only with a fresh point; it outranks the hold row (Catalogue aw), so the hold stands only
            // when there is no step to take. Never a reveal by design: the run denies every attack and cast and ends the moment a foe has him.
            if (x.s.stepRoad && x.s.stepPoint && !x.strikeReady) {
                if (const Combatant* tgt = Reposition::StepTarget(x.s)) {
                    Candidate st = x.Make(Family::Step, tgt);
                    st.form = Reposition::kStepForm;
                    st.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.stepCooldownSec, 0.0f));
                    x.Push(std::move(st));
                }
            }
        }

        // ── give ground / block / bash: lab families, each behind its road flag (Harbinger capabilities that do not exist yet) ──
        void Guards(Ctx& x) {
            const Snapshot& s = x.s;
            if (x.foeCount == 0 || x.fleeRun) return;   // card G3: no guard, bash or second disengage during the flee
            const bool ranged = s.drawn == PostureKind::Bow || s.drawn == PostureKind::Staff;
            const bool melee  = s.drawn == PostureKind::Melee || s.drawn == PostureKind::Silver;
            const float strike = Estimate::StrikeDistance(s);
            const Combatant* nearOn = nullptr;      // the nearest foe that targets him and has to close
            const Combatant* swingOn = nullptr;     // a foe whose swing at him is in the air, in striking distance
            for (const Combatant& f : s.foes) {
                if (!Readable(f) || f.targetId == 0 || f.targetId != s.self.id || f.paralysed || f.fleeing || !std::isfinite(f.dist)) continue;
                if (Estimate::MustClose(f) && (!nearOn || f.dist < nearOn->dist)) nearOn = &f;
                if (f.swinging && f.dist <= strike && (!swingOn || f.dist < swingOn->dist)) swingOn = &f;
            }
            if (s.groundRoad && nearOn) {
                Candidate c = x.Make(Family::GiveGround, nearOn);
                c.f.dist = (std::max)(0.0f, nearOn->dist);
                c.f.rangedDrawn = ranged;
                c.f.meleeThreatDist = c.f.dist;
                c.f.threatTtl = Estimate::Ttl(s.self.hp, Estimate::ThreatOn(s, s.self));
                c.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.groundCooldownSec, 0.0f));
                x.Push(std::move(c));
            }
            // Card G3 (marth 2026-11-11: "the main purpose is to become hidden, and it simply doubles as a version of flee").
            // Generated only when the road is open AND Concealment already knows a point, so a chosen run can start at once.
            //   * A SNEAK class (kSneakAlways + kDisengage) that a foe has found: run to a point no foe sees, crouch, and the
            //     run ENDS the moment the engine says nobody detects him (Concealment::Service); G2's hold row takes over.
            //   * ANY OTHER class with kDisengage, class tree ON (never with the tree off): a FLEE to a point away from the foes
            //     when he is hurt (at or under the retreat's own health line) and a foe is on him. Healing is boosted while it runs.
            if (s.concealRoad && s.concealPoint && !s.fleeRun) {
                const bool hide = x.g.sneakClass && x.g.disengage && Disengage::Found(s);
                const bool flee = !x.g.sneakClass && x.g.treeOn && x.g.disengage && x.selfHp <= Disengage::kFleeHealthLine &&
                                  Disengage::Threatened(s);
                if (hide || flee) {
                    Candidate c = x.Make(Family::Disengage, nullptr);
                    c.f.disengageClass = true;
                    c.f.disengageFlee = !hide;
                    c.f.detectShare = Disengage::DetectShare(s);
                    c.f.rangedDrawn = ranged;
                    c.f.threatTtl = Estimate::Ttl(s.self.hp, Estimate::ThreatOn(s, s.self));
                    for (const Combatant& f : s.foes)   // RetreatDanger's kite term reads the same two facts the Retreat family fills
                        if (f.targetId != 0 && f.targetId == s.self.id && !f.caster && !f.rangedWeapon && !f.paralysed && !f.fleeing && std::isfinite(f.dist))
                            c.f.meleeThreatDist = (std::min)(c.f.meleeThreatDist, (std::max)(0.0f, f.dist));
                    c.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.disengageCooldownSec, 0.0f));
                    x.Push(std::move(c));
                }
            }
            // BLOCK (batch I item 4, marth 11-12 MFO-B800): only for a class whose table carries the Block row (Q-melee-2:
            // Knight, Great Knight, Ebony Warrior; everyone else blocks by the engine's own style). Generated while the guard
            // stands too, so the renewal past 60% of the window is asked for (ServiceBlock).
            // BACK OFF (batch I): a short H3 run to a point farther from the foes, then he fights on. A mage inside the minimum range of the spell
            // in his hand (H4), or a Mercenary two or more foes press (Q-T2-3). Only with the road open and a fresh point. Not during a held guard.
            if (s.backOffRoad && s.backOffPoint && !x.guard) {
                const float need = Reposition::BackOffNeed(s, x.g);
                if (need > 0.0f) {
                    Candidate c = x.Make(Family::BackOff, nullptr);
                    c.form = Reposition::kBackOffForm;
                    c.f.backOffNeed = need;
                    c.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.backOffCooldownSec, 0.0f));
                    x.Push(std::move(c));
                }
            }
            if (s.blockRoad && x.g.block && melee && swingOn) {
                Candidate c = x.Make(Family::Block, swingOn);
                c.f.foeSwinging = true;
                const float rowCd = (std::max)(0.0f, Finite(x.g.blockCooldownSec, 0.0f));
                c.f.hardCooldownSec = rowCd > 0.0f ? rowCd : (std::max)(0.0f, Finite(x.t.blockCooldownSec, 0.0f));
                x.Push(std::move(c));
            }
            if (s.bashRoad && melee && !x.guard)
                for (const Combatant* foe : x.top) {
                    if (foe->outOfMeleeReach) continue;
                    Candidate c = x.Make(Family::Bash, foe);
                    x.Foe(c.f, *foe, false);
                    c.role = RoleTag::Melee;
                    c.f.meleeRange = true;
                    c.f.reach = x.SwingReach();
                    c.f.foeCasting = foe->casting;
                    c.f.foeSwinging = foe->swinging;
                    c.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.bashCooldownSec, 0.0f));
                    x.Push(std::move(c));
                }
        }
        // ── regroup: close on the player or a healer while fighting (Harbinger ch.24). Only once the road exists. ──
        void Regroups(Ctx& x) {
            const Snapshot& s = x.s;
            if (!s.regroupRoad || x.foeCount == 0 || x.fleeRun) return;
            for (const Combatant& a : s.allies) {
                if (!Readable(a) || a.downed || !(a.isPlayer || a.healer)) continue;
                Candidate c = x.Make(Family::Regroup, &a);
                c.f.dist = (std::max)(0.0f, Finite(a.dist, 0.0f));
                c.f.recipientIsPlayer = a.isPlayer;
                c.f.anchorHealer = a.healer;
                c.f.recipientTtl = Estimate::Ttl(a.hp, Estimate::IncomingOn(s, a));
                c.f.hardCooldownSec = (std::max)(0.0f, Finite(x.t.regroupCooldownSec, 0.0f));
                // The anchor is "safer" when every foe that is hitting him stands clearly farther from the anchor.
                bool any = false, safer = true;
                const float strike = Estimate::StrikeDistance(s);
                for (const Combatant& f : s.foes) {
                    if (f.targetId == 0 || f.targetId != s.self.id || f.paralysed || f.fleeing) continue;
                    if (Estimate::MustClose(f) && Estimate::Apart(f, s.self) > strike) continue;
                    any = true;
                    if (Estimate::Apart(f, a) < Estimate::Apart(f, s.self) + 150.0f) { safer = false; break; }
                }
                c.f.anchorSafer = any && safer;
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
                c.f.fleeHeal = x.fleeRun && a_f == Family::DrinkHealth;   // card G3: a health potion while he flees is drunk first
                x.Push(std::move(c));
            };
            drink(Family::DrinkHealth, k.potionsHealth);
            drink(Family::DrinkStamina, k.potionsStamina);
            // What the magicka would buy: the casts of his best measured spell the fight still asks for. Offense first (the
            // kept foes' health against his best spell's damage), else healing (the party's missing health against his best
            // heal). Unknown (no measured spell) leaves the potion scored as before.
            if (k.potionsMagicka > 0) {
                float bestDmg = 0.0f, bestHeal = 0.0f;
                for (const SpellEntry& e : x.s.book.entries) {
                    if (e.source != SpellSource::Spell || e.drop != Prune::Drop::Keep) continue;
                    if (!e.measured) continue;   // MFO-B684: an unmeasured entry says nothing
                    if (e.nature == Prune::Nature::Offense) bestDmg = (std::max)(bestDmg, Estimate::Pos(e.damage));
                    else if (e.nature == Prune::Nature::Heal) bestHeal = (std::max)(bestHeal, Estimate::Pos(e.heal));
                }
                float wanted = -1.0f;
                if (bestDmg > 0.0f) {
                    float hp = 0.0f;
                    for (const Combatant* f : x.top) hp += (std::max)(0.0f, Finite(f->hp, 0.0f));
                    wanted = hp / bestDmg;
                } else if (bestHeal > 0.0f) {
                    float missing = (std::max)(0.0f, Finite(x.s.self.hpMax, 0.0f) - Finite(x.s.self.hp, 0.0f));
                    for (const auto& a : x.s.allies)
                        if (Readable(a)) missing += (std::max)(0.0f, Finite(a.hpMax, 0.0f) - Finite(a.hp, 0.0f));
                    wanted = missing / bestHeal;
                }
                {
                    Candidate c = x.Make(Family::DrinkMagicka, nullptr);
                    c.f.stock = k.potionsMagicka;
                    c.f.castsWanted = std::isfinite(wanted) ? wanted : -1.0f;
                    x.Push(std::move(c));
                }
            }
        }

        // ── posture: the categories he carries, against the foe he is fighting ───────────────────────────
        void Postures(Ctx& x, const std::vector<const Combatant*>& a_foes) {
            const Snapshot& s = x.s;
            if (x.guard) return;   // a held guard: no weapon change (cast/Fire.cpp refuses the equips)
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
            if (x.guard) return;   // a held guard denies all his own casts, heals included (marth Q-melee-3)
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

            // His best castable offense spell on each kept foe (DamageValue's denominator), ONCE per tick: one pass over
            // the book per foe, not one per (spell, foe) pair.
            // MFO-B684: "castable" means the SAME tests the candidate loop below makes for that spell on that foe (class
            // Q34, hand P1, cost P2, its own target rule, a blast with no area) plus the reach the range curve vetoes at.
            // Without them a strong undead-only or out-of-reach spell shrank every offense score against a foe it can
            // never be cast on.
            constexpr float kRangeVeto = 1.15f;   // Cons::RangeSpell and RangeMelee veto above 1.15 x the reach (Catalogue.cpp)
            std::vector<float> bestOn(x.top.size(), 0.0f);
            for (const SpellEntry& o : s.book.entries) {
                if (o.source != SpellSource::Spell || o.drop != Prune::Drop::Keep || o.nature != Prune::Nature::Offense) continue;
                if (!o.measured || !(o.damage > 0.0f)) continue;
                if (!onClass && !fromMastered) continue;
                if (o.twoHanded ? anyBusy : bothBusy) continue;
                if (o.cost > 0.0f && (!poolOk || !Prune::Affordable(o.cost, pool))) continue;
                if (o.selfDelivery && !(o.areaUnits > 0.0f)) continue;
                const float oReach = (std::max)(0.0f, Finite(o.touch ? s.meleeReach : (o.selfDelivery ? o.areaUnits : o.reach), 0.0f));
                for (std::size_t ti = 0; ti < x.top.size(); ++ti) {
                    const Combatant& foe = *x.top[ti];
                    if (o.undeadOnly && !foe.undead) continue;
                    if (oReach > 0.0f && Finite(foe.dist, 0.0f) > oReach * kRangeVeto) continue;
                    bestOn[ti] = (std::max)(bestOn[ti], Estimate::SpellOn(o, foe));
                }
            }
            for (const SpellEntry& e : s.book.entries) {
                if (e.source == SpellSource::Staff) continue;        // staves: the posture layer
                if (x.holdQuiet && !e.silent) continue;              // G2: hidden and no blow ready -> a loud cast is not made (a soundless one is)
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
                    if (x.fleeRun) break;   // card G3: no offense while he flees
                    // Reach: Touch = a swing's reach; a Self-delivered blast reaches its own area; else the aimed
                    // reach (0 = the record gives none: range is then not applicable).
                    const float reach = e.touch ? s.meleeReach : (e.selfDelivery ? e.areaUnits : e.reach);
                    if (e.selfDelivery && !(e.areaUnits > 0.0f)) { ++x.out.droppedTarget; break; }
                    for (std::size_t ti = 0; ti < x.top.size(); ++ti) {
                        const Combatant* foe = x.top[ti];
                        if (e.undeadOnly && !foe->undead) { ++x.out.droppedTarget; continue; }   // its own target rule
                        Candidate c = base(foe);
                        x.Foe(c.f, *foe, true);
                        c.f.meleeRange = false;
                        c.f.reach = (std::max)(0.0f, Finite(reach, 0.0f));
                        c.f.willWork = Note(s.willWork, e.form, foe->id);
                        if (e.undeadOnly) c.f.targetCond = Tri::Yes;
                        if (e.nature == Prune::Nature::Control && e.measured) c.f.controlSec = Estimate::Pos(e.controlSec);
                        if (e.resistAv >= 0) c.f.resist = Finite(foe->Resist(e.resistAv), 0.0f);
                        if (e.nature == Prune::Nature::Offense) {   // what this cast does to THIS foe, against his best castable option on it
                            c.f.expDamage = e.measured ? Estimate::SpellOn(e, *foe) : -1.0f;   // < 0: not measured (MFO-B684)
                            c.f.foeHealth = (std::max)(0.0f, Finite(foe->hp, 0.0f));
                            c.f.bestDamage = bestOn[ti];
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
                        c.f.fleeHeal = heal && self && x.fleeRun;   // card G3: his own heal while he flees is cast first
                        c.f.recipientIsPlayer = r->isPlayer;
                        c.f.recipientDowned = r->downed;
                        if (heal) {
                            c.f.expHeal = e.measured ? Estimate::Pos(e.heal) : 0.0f;
                            c.f.recipientMissing = (std::max)(0.0f, Finite(r->hpMax, 0.0f) - Finite(r->hp, 0.0f));
                            c.f.recipientTtl = self ? x.selfTtl : Estimate::Ttl(r->hp, Estimate::IncomingOn(s, *r));
                            c.f.recipientBeingHealed = r->beingHealed;
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
        x.fleeRun = a_s.fleeRun;
        x.guard = a_s.guardHeld;
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

        // G2 Layer 1: hidden from every listed foe? (one detected foe = seen; one unknown = not known; none sneaking = not hidden)
        if (!foes.empty()) {
            bool anyYes = false, anyUnknown = false;
            for (const Combatant* f : foes) {
                if (f->detectsMe == Tri::Yes) anyYes = true;
                else if (f->detectsMe == Tri::Unknown) anyUnknown = true;
            }
            x.unseen = anyYes ? Tri::No : (anyUnknown ? Tri::Unknown : (a_s.selfSneaking ? Tri::Yes : Tri::No));
        }
        x.hold = a_s.hideRoad && x.unseen == Tri::Yes && (a_s.drawn == PostureKind::Melee || a_s.drawn == PostureKind::Silver || a_s.drawn == PostureKind::Bow);
        if (x.hold)
            for (const Combatant* f : foes)
                if (x.StrikeReady(*f)) { x.strikeReady = true; break; }
        x.holdQuiet = x.hold && !x.strikeReady;

        Swings(x);
        Holds(x);
        Regroups(x);
        Guards(x);
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

    std::vector<AskedPair> NextAskList(std::span<const AskedPair> a_prev, std::span<const SpellTarget> a_top,
                                       double a_now, double a_keepNoSec, std::size_t a_cap) {
        const auto live = [&](const AskedPair& p) {
            const double age = a_now - p.lastNo;
            return p.refused && std::isfinite(age) && age >= 0.0 && age <= a_keepNoSec;
        };
        std::vector<AskedPair> out;
        out.reserve((std::min)(a_cap, a_top.size() + a_prev.size()));
        for (const SpellTarget& t : a_top) {
            if (out.size() >= a_cap) break;
            AskedPair e{ t, false, 0.0 };
            for (const AskedPair& p : a_prev)
                if (p.pair == t && live(p)) { e = p; break; }
            if (std::find_if(out.begin(), out.end(), [&](const AskedPair& o) { return o.pair == t; }) == out.end())
                out.push_back(e);
        }
        std::vector<AskedPair> kept;
        for (const AskedPair& p : a_prev)
            if (live(p) && std::find_if(out.begin(), out.end(), [&](const AskedPair& o) { return o.pair == p.pair; }) == out.end() &&
                std::find_if(kept.begin(), kept.end(), [&](const AskedPair& o) { return o.pair == p.pair; }) == kept.end())
                kept.push_back(p);
        std::stable_sort(kept.begin(), kept.end(), [](const AskedPair& a, const AskedPair& b) { return a.lastNo > b.lastNo; });
        for (const AskedPair& p : kept) {
            if (out.size() >= a_cap) break;
            out.push_back(p);
        }
        return out;
    }

}
