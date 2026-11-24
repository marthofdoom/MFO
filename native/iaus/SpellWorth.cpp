// iaus/SpellWorth.cpp -- the spell-worth reader (iaus/SpellWorth.h has the design and the unit). PURE: no RE::, no SKSE. Every term answers "not known"
// (< 0, or Worth{ known = false }) when its input was not read; none invents a number. SHIPPED INERT: Generate.cpp reads it only while
// Tuning::spellWorth.live > 0.
#include "iaus/SpellWorth.h"

#include "iaus/Estimate.h"

#include <algorithm>
#include <cmath>

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

    namespace Estimate {

        namespace {
            constexpr float kResistCapPoints = 85.0f;   // the engine's cap on a resist (fMaxResistance's default 85)

            [[nodiscard]] float Fin(float a_v, float a_or) { return std::isfinite(a_v) ? a_v : a_or; }

            // Incoming health per second on a_who from the foes that target him and can reach him (IncomingOn's own filter), split by kind: a_magic = the spells
            // in their hands, else the weapons; a_av >= 0 keeps only the foes whose element's resist is that actor value (Combatant::incomingAv).
            [[nodiscard]] float IncomingKind(const Snapshot& a_s, const Combatant& a_who, bool a_magic, int a_av) {
                float sum = 0.0f;
                const float strike = StrikeDistance(a_s);
                for (const Combatant& f : a_s.foes) {
                    if (f.targetId == 0 || f.targetId != a_who.id || f.paralysed || f.fleeing) continue;
                    if (MustClose(f) && Apart(f, a_who) > strike) continue;
                    if (f.hitIsMagic != a_magic) continue;
                    if (a_av >= 0 && f.incomingAv != a_av) continue;
                    sum += Dps(f, a_who);
                }
                return sum;
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

        float CastSeconds(const SpellEntry& a_e, const SpellWorth& a_t, const SpellIn& a_in, float a_concChannelSec) {
            float body = 0.0f;
            if (Concentrates(a_e)) {
                body = ChannelSeconds(a_t, a_concChannelSec);
                if (body < 0.0f) return -1.0f;
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
            return a_in.wardPerSec * a_wardSec * a_t.wardDpsScale;
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
            if (!a_e.concentration) {   // a stream's measured damage is per second already (Fills.cpp counts it for 1 s): no DoT split
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
            if (a_e.nature == Prune::Nature::Buff && (a_e.buffArmor >= 0.0f || a_e.buffResist >= 0.0f)) {
                float per = 0.0f;
                if (a_e.buffArmor >= 0.0f) {   // armor: the share of the physical damage coming in that the added rating stops, within the engine's cap
                    const float cur = (std::clamp)(Fin(a_target.armor, 0.0f), 0.0f, 1.0f);
                    const float add = (std::min)((std::max)(0.0f, a_t.armorMitigationCap - cur), a_e.buffArmor * a_t.armorPctPerPoint / 100.0f);
                    per += IncomingKind(a_s, a_target, false, -1) * add / (std::max)(0.05f, 1.0f - cur);
                }
                if (a_e.buffResist >= 0.0f) {  // resist: the added points on the element's AV (below the engine's cap) against the magic coming in of that element
                    const float cur = (std::min)(kResistCapPoints, Pos(a_target.Resist(a_e.buffResistAv)));
                    const float add = (std::min)(kResistCapPoints - cur, a_e.buffResist) / 100.0f;
                    per += IncomingKind(a_s, a_target, true, a_e.buffResistAv) * add / (std::max)(0.15f, 1.0f - cur / 100.0f);
                }
                in.buffPerSec = per;
            }
            if (a_e.nature == Prune::Nature::Ward && a_e.wardPower >= 0.0f && a_e.buffSec > 0.0f)
                in.wardPerSec = (std::min)(IncomingKind(a_s, a_target, true, -1), a_e.wardPower / a_e.buffSec);   // a ward absorbs up to its power
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
            const float secs = CastSeconds(a_e, a_t, a_in, a_concChannelSec);
            if (!(secs >= 0.0f)) return w;
            const bool conc = Concentrates(a_e);
            const float channel = conc ? ChannelSeconds(a_t, a_concChannelSec) : 1.0f;   // a stream's measured effect is per second: x the seconds it is held
            float effect = -1.0f;
            switch (a_e.nature) {
            case Prune::Nature::Offense: {
                if (!a_e.measured || !(Pos(a_e.damage) > 0.0f)) return w;
                float raw = -1.0f;
                if (!conc && a_in.dotPerSec >= 0.0f && a_in.dotSec >= 0.0f) {   // the DoT part apart from the direct part
                    const float direct = a_in.directDamage >= 0.0f ? a_in.directDamage : (std::max)(0.0f, Pos(a_e.damage) - a_in.dotPerSec * a_in.dotSec);
                    raw = direct + DotOverLife(a_t, a_in.dotPerSec, a_in.dotSec, a_in.foeLifeSec);
                } else {
                    raw = Pos(a_e.damage) * channel;
                }
                const float resisted = SpellOn(a_e, a_target) / Pos(a_e.damage);   // the foe's resists, the one rule (Estimate::SpellOn), as a share
                effect = raw * resisted * AreaFactor(a_t, a_in.foesInArea, a_in.friendsInArea);
                if (a_in.redundant) effect *= a_t.redundantEffectScale;
                if (a_t.killConfirmBonus > 0.0f && Pos(a_target.hp) > 0.0f && effect >= a_target.hp) {   // the cheapest known spell that finishes the foe gets the bonus
                    bool cheaper = false;
                    for (const SpellEntry& o : a_s.book.entries) {
                        if (&o == &a_e || o.nature != Prune::Nature::Offense || !o.measured || !(o.cost < a_e.cost)) continue;
                        if (SpellOn(o, a_target) >= a_target.hp) { cheaper = true; break; }
                    }
                    if (!cheaper) effect *= 1.0f + a_t.killConfirmBonus;
                }
                break;
            }
            case Prune::Nature::Heal: {
                if (!a_e.measured || !(Pos(a_e.heal) > 0.0f) || !(Pos(a_target.hpMax) > 0.0f)) return w;
                const float missing = (std::max)(0.0f, a_target.hpMax - Fin(a_target.hp, 0.0f));
                effect = (std::min)(Pos(a_e.heal) * channel, missing);   // never more than is missing
                break;
            }
            case Prune::Nature::Control: {
                if (!a_e.measured || !(Pos(a_e.controlSec) > 0.0f)) return w;
                const float held = ControlHeldSec(a_t, a_e.controlSec, a_in.foeLifeSec);
                effect = ControlEffect(a_t, held, a_target);
                break;
            }
            case Prune::Nature::Buff:   effect = BuffEffect(a_t, a_in); break;
            case Prune::Nature::Ward:   effect = WardEffect(a_t, a_in, a_in.buffSec); break;
            case Prune::Nature::Summon: effect = SummonEffect(a_t, a_in, a_in.buffSec); break;
            default: return w;
            }
            if (!(effect >= 0.0f)) return w;
            w.seconds = secs;
            w.magicka = a_e.cost > 0.0f ? a_e.cost * channel : 0.0f;   // a stream's cost is per second
            effect *= MagickaShare(a_t, w.magicka, secs, a_s.self.mp, (std::max)(0.0f, a_in.magickaRegenPerSec), a_in.fightSec);
            w.effect = effect;
            w.known = true;
            return w;
        }

    }

}
