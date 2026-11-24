// iaus/SpellWorth.cpp -- the SKELETON of the spell-worth reader (iaus/SpellWorth.h has the design, the unit and the list of what the fill-in
// needs). UNWIRED: no caller. Every TODO body answers "not known" (< 0, or Worth{ known = false }); none invents a number.
#include "iaus/SpellWorth.h"

#include <cmath>

namespace MFO::Iaus {

    namespace {
        struct Knob { std::string_view name; float SpellWorth::* m; };
        // ONE TABLE of names: Set / Get, the tuning file, the tuner and the sim's knob list all read these.
        constexpr Knob kKnobs[] = {
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

        // TODO(S11b): by nature. Offense: (direct + DotOverLife) x the target's resists (Estimate::SpellOn's rule, read once) x AreaFactor.
        // Heal: min(heal, the recipient's missing health). Control: ControlEffect. Buff / Ward / Summon: their functions. Then x
        // redundantEffectScale when a_in.redundant, + killConfirmBonus when the effect reaches the foe's health and no cheaper known spell does,
        // x MagickaShare. seconds = CastSeconds, magicka = a_e.cost. known only when every term the nature needs is known.
        Worth SpellWorthOn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t, const SpellIn& a_in) {
            (void)a_e; (void)a_target; (void)a_s; (void)a_t; (void)a_in;
            return Worth{};
        }

        // TODO(S11b): landSecBase + the family's landSec + (concentration ? ChannelSeconds : a_in.chargeSec, else NOT KNOWN) + recoverSec
        // + (a_in.failShare >= 0 ? a_in.failShare x failedStartSec : 0).
        float CastSeconds(const SpellEntry& a_e, const SpellWorth& a_t, const SpellIn& a_in, float a_concChannelSec) {
            (void)a_e; (void)a_t; (void)a_in; (void)a_concChannelSec;
            return -1.0f;
        }

        // TODO(S11b): max(channelMinSec, a_concChannelSec x channelUptime).
        float ChannelSeconds(const SpellWorth& a_t, float a_concChannelSec) {
            (void)a_t; (void)a_concChannelSec;
            return -1.0f;
        }

        // TODO(S11b): 1 + areaFoesIntoDamage x (foes - 1) - areaFriendPenalty x friends, never under 0; foes < 0 = one foe (factor 1).
        float AreaFactor(const SpellWorth& a_t, float a_foesInArea, float a_friendsInArea) {
            (void)a_t; (void)a_foesInArea; (void)a_friendsInArea;
            return -1.0f;
        }

        // TODO(S11b): a_dotPerSec x min(a_dotSec, max(foeLifeFloorSec, a_foeLifeSec x dotLifeShare)); a_foeLifeSec < 0 = the whole duration.
        float DotOverLife(const SpellWorth& a_t, float a_dotPerSec, float a_dotSec, float a_foeLifeSec) {
            (void)a_t; (void)a_dotPerSec; (void)a_dotSec; (void)a_foeLifeSec;
            return -1.0f;
        }

        // TODO(S11b): a_controlSec x a_foe.hit x a_foe.hitsPerSec x controlDpsScale (the foe's own damage rate; a caster foe: its spell fill).
        float ControlEffect(const SpellWorth& a_t, float a_controlSec, const Combatant& a_foe) {
            (void)a_t; (void)a_controlSec; (void)a_foe;
            return -1.0f;
        }

        // TODO(S11b): a_in.buffPerSec x min(a_in.buffSec, the fight's remaining length) x buffDpsScale.
        float BuffEffect(const SpellWorth& a_t, const SpellIn& a_in) {
            (void)a_t; (void)a_in;
            return -1.0f;
        }

        // TODO(S11b): a_in.wardPerSec x a_wardSec x wardDpsScale.
        float WardEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_wardSec) {
            (void)a_t; (void)a_in; (void)a_wardSec;
            return -1.0f;
        }

        // TODO(S11b): a_in.summonDps x min(a_summonSec, summonLifeSec) x summonDpsScale.
        float SummonEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_summonSec) {
            (void)a_t; (void)a_in; (void)a_summonSec;
            return -1.0f;
        }

        // TODO(S11b): 1 while (a_magicka - a_cost + regen x horizon) covers the casts the horizon holds, else 1 - magickaDryPenalty;
        // horizon = a_fightSec when known, else magickaHorizonSec.
        float MagickaShare(const SpellWorth& a_t, float a_cost, float a_magicka, float a_magickaRegenPerSec, float a_fightSec) {
            (void)a_t; (void)a_cost; (void)a_magicka; (void)a_magickaRegenPerSec; (void)a_fightSec;
            return -1.0f;
        }

    }

}
