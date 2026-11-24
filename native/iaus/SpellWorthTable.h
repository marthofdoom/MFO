#pragma once
// iaus/SpellWorthTable.h -- THE NAMED TABLE of the spell-worth reader (iaus/SpellWorth.h). Split out of SpellWorth.h only because iaus/Core.h's Tuning holds
// it by value and SpellWorth.h includes iaus/Snapshot.h (which includes Core.h). Standard headers only. Every number is a labelled PLACEHOLDER; the sim's
// `spells` stage searches them (tools/iaus_sim/tune_driver.py SW_KNOBS). SHIPPED INERT: `live` = 0 keeps every decision on today's estimate.
#include <cstddef>
#include <string_view>

namespace MFO::Iaus {

    // THE NAMED TABLE. One struct, one name per field (`sw.<field>` in shared.txt, the tuner and the sim). PLACEHOLDERS, not searched yet.
    struct SpellWorth {
        // ── how far the reader is LIVE ───────────────────────────────────────────────────────────────────────────────────────────────
        // 0 (SHIPPED) = every Facts::expDamage / expRate / bestDamage / effectRate / healRate / bestHealRate / controlSec / expHeal stays today's estimate, bit for bit, and the
        // reader is never called. 1 = the reader's answer where it KNOWS the cast; between = the blend. A cast the reader does not know keeps today's estimate
        // at any value (never a 0 that reads as "worthless"). Values become live through the tuning round, not here.
        float live = 0.0f;
        // ── seconds a cast costs ─────────────────────────────────────────────────────────────────────────────────────────────────────
        // claim to fire: from the decision to the moment the engine's caster starts the spell. Base, then what each cast family adds (the field's
        // claim-to-fire tables differ by family: iaus-sim-v3.md 13.2; a per-ROW split needs the engine row on SpellEntry, which nothing reads yet).
        float landSecBase = 0.9f;
        float landSecOffense = 0.0f;
        float landSecControl = 0.0f;
        float landSecHeal = 0.0f;
        float landSecBuff = 0.0f;
        float landSecSummon = 0.0f;
        float landSecWard = 0.0f;
        float recoverSec = 0.5f;            // after the release, before his next action
        // a concentration spell: the share of Tuning::concChannelSec a stream really holds (line-of-fire holds, the foe walking out), and the
        // shortest channel that counts at all
        float channelUptime = 0.7f;
        float channelMinSec = 0.5f;
        // ── area ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
        float areaFoesIntoDamage = 1.0f;    // how far Facts::foesInArea multiplies the effect: 0 = one foe's worth (today), 1 = every foe in the area counts in full
        float areaFriendPenalty = 1.0f;     // a party member in the area: his share of the damage, times this, comes OFF the effect
        // ── damage over time ─────────────────────────────────────────────────────────────────────────────────────────────────────────
        float dotLifeShare = 1.0f;          // a DoT counts for min(its duration, the foe's expected life x this)
        float foeLifeFloorSec = 2.0f;       // a foe's expected life is never read shorter than this (one more exchange)
        // what +1 armor rating is worth as a share of the physical damage it stops (the engine's fArmorScalingFactor 0.12, percent per point), and the
        // share the engine never lets armor exceed (fMaxArmorRating 80 percent); a resist is read the same way against 85 points (the engine's cap)
        float armorPctPerPoint = 0.12f;
        float armorMitigationCap = 0.8f;
        // ── the other families, in the same unit ─────────────────────────────────────────────────────────────────────────────────────
        float controlDpsScale = 1.0f;       // control: held seconds x the foe's damage rate x this
        float buffDpsScale = 1.0f;          // buff: damage prevented or added per second it is up x this
        float wardDpsScale = 1.0f;          // ward: incoming magic damage per second absorbed x this
        float summonDpsScale = 1.0f;        // summon: (its damage rate + the damage it draws) x this
        float summonLifeSec = 20.0f;        // how long a summoned body is expected to stand in a fight (capped by the spell's own duration)
        float redundantEffectScale = 0.0f;  // a spell whose effect archetype is already live on the target (another spell's fire DoT, a weaker armor spell): its effect x this
        float killConfirmBonus = 0.0f;      // added share for the cheapest spell that finishes a foe before it acts again
        // ── magicka per fight ────────────────────────────────────────────────────────────────────────────────────────────────────────
        float magickaHorizonSec = 30.0f;    // the fight length the pool is weighed against when Snapshot::fightSec is unknown
        float magickaDryPenalty = 0.5f;     // the share of a cast's effect that is charged when paying for it would leave him dry before the horizon
        // ── a start that never fires (never a veto: marth "run every spell") ─────────────────────────────────────────────────────────
        float failedStartSec = 4.0f;        // the seconds a claim holds his hand before the never-started release, charged by the row's failure share
    };

    // `sw.<field>` by name (the WorthTable / LootTuning pattern). false = unknown name or not a finite number.
    [[nodiscard]] bool SetSpellWorthKnob(SpellWorth& a_t, std::string_view a_name, float a_v);
    [[nodiscard]] bool GetSpellWorthKnob(const SpellWorth& a_t, std::string_view a_name, float& a_out);
    [[nodiscard]] std::string_view SpellWorthKnobName(std::size_t a_i);   // the names one by one ("" past the end): a list that cannot drift from Set / Get

}
