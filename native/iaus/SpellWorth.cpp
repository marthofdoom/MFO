// iaus/SpellWorth.cpp -- the spell-worth reader (iaus/SpellWorth.h has the design and the unit). PURE: no RE::, no SKSE. Every term answers "not known"
// (< 0, or Worth{ known = false }) when its input was not read; none invents a number. SHIPPED INERT: Generate.cpp reads it only while
// Tuning::spellWorth.live > 0.
#include "iaus/SpellWorth.h"

#include "iaus/Estimate.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace MFO::Iaus {

    namespace {
        struct Knob { std::string_view name; float SpellWorth::* m; };
        // ONE TABLE of names: Set / Get, the tuning file, the tuner and the sim's knob list all read these.
        constexpr Knob kKnobs[] = {
            { "sw.live", &SpellWorth::live }, { "sw.armorPctPerPoint", &SpellWorth::armorPctPerPoint }, { "sw.armorMitigationCap", &SpellWorth::armorMitigationCap },
            { "sw.landSecBase", &SpellWorth::landSecBase }, { "sw.landSecOffense", &SpellWorth::landSecOffense }, { "sw.landSecControl", &SpellWorth::landSecControl },
            { "sw.landSecHeal", &SpellWorth::landSecHeal }, { "sw.landSecBuff", &SpellWorth::landSecBuff }, { "sw.landSecSummon", &SpellWorth::landSecSummon },
            { "sw.landSecWard", &SpellWorth::landSecWard }, { "sw.recoverSec", &SpellWorth::recoverSec }, { "sw.channelUptime", &SpellWorth::channelUptime },
            { "sw.channelMinSec", &SpellWorth::channelMinSec }, { "sw.areaFoesIntoDamage", &SpellWorth::areaFoesIntoDamage }, { "sw.areaFriendPenalty", &SpellWorth::areaFriendPenalty },
            { "sw.dotLifeShare", &SpellWorth::dotLifeShare }, { "sw.foeLifeFloorSec", &SpellWorth::foeLifeFloorSec }, { "sw.controlDpsScale", &SpellWorth::controlDpsScale },
            { "sw.buffDpsScale", &SpellWorth::buffDpsScale }, { "sw.wardDpsScale", &SpellWorth::wardDpsScale }, { "sw.summonDpsScale", &SpellWorth::summonDpsScale },
            { "sw.summonLifeSec", &SpellWorth::summonLifeSec }, { "sw.redundantEffectScale", &SpellWorth::redundantEffectScale }, { "sw.killConfirmBonus", &SpellWorth::killConfirmBonus },
            { "sw.magickaHorizonSec", &SpellWorth::magickaHorizonSec }, { "sw.magickaDryPenalty", &SpellWorth::magickaDryPenalty }, { "sw.failedStartSec", &SpellWorth::failedStartSec },
        };
    }

    bool SetSpellWorthKnob(SpellWorth& a_t, std::string_view a_name, float a_v) {
        if (!std::isfinite(a_v)) return false;
        for (const Knob& k : kKnobs) if (a_name == k.name) { a_t.*(k.m) = a_v; return true; }
        return false;
    }
    bool GetSpellWorthKnob(const SpellWorth& a_t, std::string_view a_name, float& a_out) {
        for (const Knob& k : kKnobs) if (a_name == k.name) { a_out = a_t.*(k.m); return true; }
        return false;
    }

    std::string_view SpellWorthKnobName(std::size_t a_i) { return a_i < std::size(kKnobs) ? kKnobs[a_i].name : std::string_view{}; }

    namespace Estimate {

        namespace {
            constexpr float kResistCapPoints = 85.0f;   // the engine's cap on a resist (fMaxResistance's default 85)

            [[nodiscard]] float Fin(float a_v, float a_or) { return std::isfinite(a_v) ? a_v : a_or; }

            // The foes whose damage is coming in on a_who NOW: they target him and can reach him (IncomingOn's own filter). a_magic = a spell in the hand, else a weapon.
            template <class Fn>
            void ForIncoming(const Snapshot& a_s, const Combatant& a_who, bool a_magic, Fn a_fn) {
                const float strike = StrikeDistance(a_s);
                for (const Combatant& f : a_s.foes) {
                    if (f.targetId == 0 || f.targetId != a_who.id || f.paralysed || f.fleeing) continue;
                    if (MustClose(f) && Apart(f, a_who) > strike) continue;
                    if (f.hitIsMagic == a_magic) a_fn(f);
                }
            }
            [[nodiscard]] float IncomingKind(const Snapshot& a_s, const Combatant& a_who, bool a_magic) {
                float sum = 0.0f;
                ForIncoming(a_s, a_who, a_magic, [&](const Combatant& f) { sum += Dps(f, a_who); });
                return sum;
            }

            // The magic damage per second a resist buff stops. Dps carries NO resist for a magic hit, so each foe's is taken through the engine's two terms
            // (SpellOn's rule: the primary resist, kPoisonResist for a poison hit and else kResistMagic, then the element) before and after the buff.
            [[nodiscard]] float ResistPrevented(const Snapshot& a_s, const Combatant& a_who, const SpellEntry& a_e) {
                const auto added = [&](int av) { float m = 0.0f; for (const auto& r : a_e.buffResists) if (r.first == av) m = (std::max)(m, Pos(r.second)); return m; };
                const auto keep = [](float points) { return 1.0f - (std::min)(kResistCapPoints, Fin(points, 0.0f)) / 100.0f; };
                float per = 0.0f;
                ForIncoming(a_s, a_who, true, [&](const Combatant& f) {
                    const int   prim = f.incomingAv == kAvPoisonResist ? kAvPoisonResist : kAvResistMagic;
                    const float p = prim == kAvResistMagic ? a_who.magicResist : a_who.Resist(prim);
                    const bool  el = f.incomingAv >= 0 && f.incomingAv != prim;
                    const float e = el ? a_who.Resist(f.incomingAv) : 0.0f;
                    const float before = keep(p) * keep(e), after = keep(Fin(p, 0.0f) + added(prim)) * keep(Fin(e, 0.0f) + (el ? added(f.incomingAv) : 0.0f));
                    per += Dps(f, a_who) * (std::max)(0.0f, before - after);
                });
                return per;
            }

            [[nodiscard]] bool Concentrates(const SpellEntry& a_e) { return a_e.concentration; }
        }

        // The family's claim-to-fire seconds (the table's landSec<Family>).
        namespace {
            [[nodiscard]] float LandSec(const SpellEntry& a_e, const SpellWorth& a_t) {
                float fam = 0.0f;
                switch (a_e.nature) {
                case Prune::Nature::Offense: fam = a_t.landSecOffense; break;
                case Prune::Nature::Control: fam = a_t.landSecControl; break;
                case Prune::Nature::Heal:    fam = a_t.landSecHeal; break;
                case Prune::Nature::Buff:    fam = a_t.landSecBuff; break;
                case Prune::Nature::Summon:  fam = a_t.landSecSummon; break;
                case Prune::Nature::Ward:    fam = a_t.landSecWard; break;
                default: break;
                }
                return a_t.landSecBase + fam;
            }
        }

        float ChannelSeconds(const SpellWorth& a_t, float a_concChannelSec) {
            if (!std::isfinite(a_concChannelSec)) return -1.0f;
            return (std::max)(a_t.channelMinSec, a_concChannelSec * a_t.channelUptime);
        }

        float HeldSeconds(const SpellEntry& a_e, const Combatant& a_target, const SpellWorth& a_t, const SpellIn& a_in, float a_concChannelSec) {
            float sec = ChannelSeconds(a_t, a_concChannelSec);
            if (sec < 0.0f) return sec;
            if ((a_e.nature == Prune::Nature::Offense || a_e.nature == Prune::Nature::Control) && a_in.foeLifeSec >= 0.0f)
                sec = (std::min)(sec, (std::max)(a_t.foeLifeFloorSec, a_in.foeLifeSec));   // the foe is dead: the stream is let go
            if (a_e.nature == Prune::Nature::Heal && Pos(a_e.heal) > 0.0f && Pos(a_target.hpMax) > 0.0f)
                sec = (std::min)(sec, (std::max)(a_t.channelMinSec, (std::max)(0.0f, a_target.hpMax - Fin(a_target.hp, 0.0f)) / a_e.heal));   // he is full
            return sec;
        }

        float OffenseOnTarget(const SpellEntry& a_e, const Combatant& a_foe, const SpellWorth& a_t, const SpellIn& a_in, float a_heldSec) {
            if (!a_e.measured || !(Pos(a_e.damage) > 0.0f)) return -1.0f;
            const float resisted = SpellOn(a_e, a_foe) / Pos(a_e.damage);   // the foe's resists, the one rule (Estimate::SpellOn), as a share
            if (Concentrates(a_e)) return Pos(a_e.damage) * (std::max)(0.0f, a_heldSec) * resisted;
            if (a_in.dotPerSec >= 0.0f && a_in.dotSec >= 0.0f) {   // the DoT part apart from the direct part
                const float direct = a_in.directDamage >= 0.0f ? a_in.directDamage : (std::max)(0.0f, Pos(a_e.damage) - a_in.dotPerSec * a_in.dotSec);
                return (direct + DotOverLife(a_t, a_in.dotPerSec, a_in.dotSec, a_in.foeLifeSec)) * resisted;
            }
            return Pos(a_e.damage) * resisted;
        }

        float CastSeconds(const SpellEntry& a_e, const SpellWorth& a_t, const SpellIn& a_in, float a_heldSec) {
            float body = 0.0f;
            if (Concentrates(a_e)) {
                body = a_heldSec;
                if (!(body >= 0.0f)) return -1.0f;
            } else {
                if (!(a_in.chargeSec >= 0.0f)) return -1.0f;   // the charge time was not read: NOT KNOWN
                body = a_in.chargeSec;
            }
            const float fail = a_in.failShare >= 0.0f ? (std::clamp)(a_in.failShare, 0.0f, 1.0f) * a_t.failedStartSec : 0.0f;
            return (std::max)(0.0f, LandSec(a_e, a_t) + body + a_t.recoverSec + fail);
        }

        float AreaFactor(const SpellWorth& a_t, float a_foesInArea, float a_friendsInArea) {
            if (!(a_foesInArea >= 0.0f)) return 1.0f;   // no area read: one foe
            const float foes = (std::max)(1.0f, a_foesInArea);
            const float friends = a_friendsInArea > 0.0f ? a_friendsInArea : 0.0f;
            return (std::max)(0.0f, 1.0f + a_t.areaFoesIntoDamage * (foes - 1.0f) - a_t.areaFriendPenalty * friends);
        }

        float DotOverLife(const SpellWorth& a_t, float a_dotPerSec, float a_dotSec, float a_foeLifeSec) {
            if (!(a_dotPerSec >= 0.0f) || !(a_dotSec >= 0.0f)) return -1.0f;
            if (!(a_foeLifeSec >= 0.0f)) return a_dotPerSec * a_dotSec;   // the foe's life is not known: the whole duration
            const float life = (std::max)(a_t.foeLifeFloorSec, a_foeLifeSec * a_t.dotLifeShare);
            return a_dotPerSec * (std::min)(a_dotSec, life);
        }

        float ControlHeldSec(const SpellWorth& a_t, float a_controlSec, float a_foeLifeSec) {
            if (!(a_controlSec >= 0.0f)) return -1.0f;
            return a_foeLifeSec >= 0.0f ? (std::min)(a_controlSec, (std::max)(a_t.foeLifeFloorSec, a_foeLifeSec)) : a_controlSec;
        }

        float ControlEffect(const SpellWorth& a_t, float a_controlSec, const Combatant& a_foe) {
            const float hit = Pos(a_foe.hit), rate = Pos(a_foe.hitsPerSec);
            if (!(a_controlSec >= 0.0f) || !(hit > 0.0f) || !(rate > 0.0f)) return -1.0f;   // a foe whose damage was not measured: NOT KNOWN
            return a_controlSec * hit * rate * a_t.controlDpsScale;
        }

        float BuffEffect(const SpellWorth& a_t, const SpellIn& a_in) {
            if (!(a_in.buffPerSec >= 0.0f) || !(a_in.buffSec >= 0.0f)) return -1.0f;
            const float sec = a_in.fightSec >= 0.0f ? (std::min)(a_in.buffSec, a_in.fightSec) : a_in.buffSec;
            return a_in.buffPerSec * sec * a_t.buffDpsScale;
        }

        float WardEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_wardSec) {
            if (!(a_in.wardPerSec >= 0.0f) || !(a_wardSec >= 0.0f)) return -1.0f;
            const float taken = a_in.wardPerSec * a_wardSec;
            return (a_in.wardPower >= 0.0f ? (std::min)(taken, a_in.wardPower) : taken) * a_t.wardDpsScale;   // a ward breaks at its power
        }

        float SummonEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_summonSec) {
            if (!(a_in.summonDps >= 0.0f) || !(a_summonSec >= 0.0f)) return -1.0f;
            return a_in.summonDps * (std::min)(a_summonSec, a_t.summonLifeSec) * a_t.summonDpsScale;
        }

        float MagickaShare(const SpellWorth& a_t, float a_cost, float a_castSec, float a_magicka, float a_magickaRegenPerSec, float a_fightSec) {
            if (!(a_cost > 0.0f)) return 1.0f;   // a scroll, a staff, a free spell
            const float horizon = a_fightSec > 0.0f ? a_fightSec : a_t.magickaHorizonSec;
            const float regen = a_magickaRegenPerSec > 0.0f ? a_magickaRegenPerSec : 0.0f;
            const float casts = a_castSec > 0.0f ? (std::max)(1.0f, horizon / a_castSec) : 1.0f;
            const float have = (std::max)(0.0f, Fin(a_magicka, 0.0f)) + regen * horizon;
            return have >= a_cost * casts ? 1.0f : (std::clamp)(1.0f - a_t.magickaDryPenalty, 0.0f, 1.0f);
        }

        float FoeLifeSec(const Snapshot& a_s, const Combatant& a_foe) {
            float dps = Dps(a_s.self, a_foe);
            for (const Combatant& a : a_s.allies) dps += Dps(a, a_foe);
            if (!(dps > 0.0f) || !(Pos(a_foe.hp) > 0.0f)) return -1.0f;
            return Pos(a_foe.hp) / dps;
        }

        float FightSec(const Snapshot& a_s) {
            float sum = 0.0f;
            for (const Combatant& f : a_s.foes) {
                if (!(Pos(f.hp) > 0.0f)) continue;
                const float life = FoeLifeSec(a_s, f);
                if (life < 0.0f) return -1.0f;   // one foe's life unknown: the fight's length is not known
                sum += life;
            }
            return sum > 0.0f ? sum : -1.0f;
        }

        SpellIn MakeSpellIn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t,
                            float a_foesInArea, float a_friendsInArea) {
            SpellIn in;
            in.chargeSec = a_e.chargeSec;
            if (!Concentrates(a_e)) {   // a stream's measured damage is per second already (Fills.cpp counts it for 1 s): no DoT split
                in.dotPerSec = a_e.dotPerSec;
                in.dotSec = a_e.dotSec;
                in.directDamage = a_e.directDamage;
            }
            in.foesInArea = a_foesInArea;
            in.friendsInArea = a_friendsInArea;
            in.fightSec = FightSec(a_s);
            in.buffSec = a_e.buffSec;
            const bool foeSide = a_e.nature == Prune::Nature::Offense || a_e.nature == Prune::Nature::Control;
            if (foeSide) in.foeLifeSec = FoeLifeSec(a_s, a_target);
            if (a_e.nature == Prune::Nature::Buff && (a_e.buffArmor >= 0.0f || !a_e.buffResists.empty())) {
                float per = 0.0f;
                if (a_e.buffArmor >= 0.0f) {   // armor: the share of the physical damage coming in that the added rating stops, within the engine's cap
                    const float cur = (std::clamp)(Fin(a_target.armor, 0.0f), 0.0f, 1.0f);
                    const float add = (std::min)((std::max)(0.0f, a_t.armorMitigationCap - cur), a_e.buffArmor * a_t.armorPctPerPoint / 100.0f);
                    per += IncomingKind(a_s, a_target, false) * add / (std::max)(0.05f, 1.0f - cur);
                }
                if (!a_e.buffResists.empty()) per += ResistPrevented(a_s, a_target, a_e);
                in.buffPerSec = per;
            }
            if (a_e.nature == Prune::Nature::Ward && a_e.wardPower >= 0.0f) {
                in.wardPerSec = IncomingKind(a_s, a_target, true);
                in.wardPower = a_e.wardPower;
            }
            if (a_e.nature == Prune::Nature::Summon && a_e.summonHit >= 0.0f && a_e.summonHitsPerSec >= 0.0f && a_e.summonHp >= 0.0f && a_e.buffSec > 0.0f) {
                const float life = (std::min)(a_e.buffSec, a_t.summonLifeSec);
                const float draw = life > 0.0f ? (std::min)(a_e.summonHp / life, IncomingOn(a_s, a_target)) : 0.0f;   // the damage it takes off him
                in.summonDps = a_e.summonHit * a_e.summonHitsPerSec + draw;
            }
            return in;
        }

        Worth SpellWorthOn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t, const SpellIn& a_in,
                           float a_concChannelSec) {
            Worth w;
            const bool conc = Concentrates(a_e);
            const float held = conc ? HeldSeconds(a_e, a_target, a_t, a_in, a_concChannelSec) : 1.0f;   // a stream's measured effect is per second: x the seconds it is held
            const float secs = CastSeconds(a_e, a_t, a_in, held);
            if (!(secs >= 0.0f)) return w;
            float effect = -1.0f;
            switch (a_e.nature) {
            case Prune::Nature::Offense: {
                const float one = OffenseOnTarget(a_e, a_target, a_t, a_in, held);   // ONE foe: no area, no bonus, no magicka
                if (!(one >= 0.0f)) return w;
                // a stream is let go when the foe drops: it overkills by at most its own second
                w.onTarget = conc ? (std::min)(one, (std::max)(Pos(a_target.hp), SpellOn(a_e, a_target))) : one;
                effect = one * AreaFactor(a_t, a_in.foesInArea, a_in.friendsInArea);
                if (a_in.redundant) effect *= a_t.redundantEffectScale;
                if (a_t.killConfirmBonus > 0.0f && Pos(a_target.hp) > 0.0f && one >= a_target.hp) {   // the cheapest known spell that finishes the foe gets the bonus
                    bool cheaper = false;
                    for (const SpellEntry& o : a_s.book.entries) {
                        if (&o == &a_e || o.form == a_e.form || o.nature != Prune::Nature::Offense || !o.measured || !(o.cost < a_e.cost)) continue;
                        SpellIn oin;   // the same one-foe figure, from the entry's own parts
                        oin.foeLifeSec = a_in.foeLifeSec;
                        if (!Concentrates(o)) { oin.dotPerSec = o.dotPerSec; oin.dotSec = o.dotSec; oin.directDamage = o.directDamage; }
                        if (OffenseOnTarget(o, a_target, a_t, oin, Concentrates(o) ? HeldSeconds(o, a_target, a_t, oin, a_concChannelSec) : 1.0f) >= a_target.hp) { cheaper = true; break; }
                    }
                    if (!cheaper) effect *= 1.0f + a_t.killConfirmBonus;
                }
                break;
            }
            case Prune::Nature::Heal: {
                if (!a_e.measured || !(Pos(a_e.heal) > 0.0f) || !(Pos(a_target.hpMax) > 0.0f)) return w;
                const float missing = (std::max)(0.0f, a_target.hpMax - Fin(a_target.hp, 0.0f));
                w.onTarget = conc ? (std::min)(Pos(a_e.heal) * held, (std::max)(missing, Pos(a_e.heal))) : Pos(a_e.heal);   // UNCAPPED (a stream is let go when he is full)
                effect = (std::min)(Pos(a_e.heal) * held, missing);   // never more than is missing
                break;
            }
            case Prune::Nature::Control: {
                if (!a_e.measured || !(Pos(a_e.controlSec) > 0.0f)) return w;
                effect = ControlEffect(a_t, ControlHeldSec(a_t, a_e.controlSec, a_in.foeLifeSec), a_target);
                break;
            }
            case Prune::Nature::Buff:   effect = BuffEffect(a_t, a_in); break;
            case Prune::Nature::Ward:   effect = WardEffect(a_t, a_in, conc ? held : a_in.buffSec); break;
            case Prune::Nature::Summon: effect = SummonEffect(a_t, a_in, a_in.buffSec); break;
            default: return w;
            }
            if (!(effect >= 0.0f)) return w;
            w.seconds = secs;
            w.magicka = a_e.cost > 0.0f ? a_e.cost * held : 0.0f;   // a stream's cost is per second
            effect *= MagickaShare(a_t, w.magicka, secs, a_s.self.mp, (std::max)(0.0f, a_in.magickaRegenPerSec), a_in.fightSec);
            w.effect = effect;
            w.known = true;
            return w;
        }

    }

}
