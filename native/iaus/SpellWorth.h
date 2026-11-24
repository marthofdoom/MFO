#pragma once
// iaus/SpellWorth.h -- WHAT ONE CAST OF A SPELL IS WORTH, IN ONE UNIT FOR EVERY CAST FAMILY (sim v4 batch C, slice S11b; design
// Docs/research/2026-11/iaus-sim-v4.md 7.4, 7.5 and 14).
//
// *** SKELETON: UNWIRED. *** Nothing in the DLL, the harness or the sim calls this file. Every body marked TODO returns "not known" and
// Estimate::SpellWorthOn returns Worth{ known = false }. It is compiled (native/CMakeLists.txt, tools/iaus_sim/run.sh) so the interface cannot
// rot, and nothing else. Filling it is its own brief (native tier B: logic inside the estimate, no engine type, no seat, no co-save, no ABI).
//
// WHY (marth 2026-11-23, Deck on 2.11.9: "the spells being chosen were still pretty ineffective"). Today a spell is ranked by its magnitude per
// cast, with one cast time for every fire-and-forget spell, a stream counted for one second, an area spell counted on one foe, a DoT counted for
// its whole duration whatever the foe's life, and the families compared by one constant each (the action weights). The inputs below do not exist.
//
// THE UNIT: FOE HEALTH POINTS REMOVED OR PARTY HEALTH POINTS KEPT, PER CAST.
//   offense   the health the cast removes from the foes it reaches
//   heal      the health it restores (never more than is missing)
//   control   the damage the controlled foe would have dealt in the seconds it is held: seconds x that foe's damage rate
//   buff      the damage the buff prevents (armor, resist) or adds (a weapon buff) over the time it lasts in this fight
//   ward      the incoming magic damage it absorbs while it is up
//   summon    the damage the summoned body deals plus the damage it draws, over its life in this fight
// and beside it the SECONDS the cast costs (claim to fire, charge or channel, recover) and the MAGICKA, so a caller can read effect per second,
// effect per magicka, and whether the pool lasts the fight. ONE reader for every cast family: Estimate::SpellWorthOn. "To add a term to spell
// worth, add a field to SpellWorth and read it in SpellWorthOn" (CLAUDE.md principle 12). Two readers of what a spell is worth is how the next
// 11-13 loot bug is born.
//
// PURE: standard headers and iaus/Snapshot.h only (no RE::, no SKSE), so tools/iaus_harness and tools/iaus_sim build it with the host compiler.
// EVERY NUMBER in the table is a labelled PLACEHOLDER. The sim's `spells` stage searches them (tools/iaus_sim/tune_driver.py SW_KNOBS) once the
// bodies exist and a consideration reads the result.
//
// WHAT THE FILL-IN NEEDS (precisely; each is a TODO at its function):
//   1. `SpellWorth worth{}` as a member of iaus/Core.h Tuning (beside `WorthTable worth`; pick a name that does not collide, e.g. `spellWorth`),
//      and the `sw.<field>` names in iaus/Catalogue.cpp SetTuningScalar through SetSpellWorthKnob below (the WorthTable / LootTuning pattern).
//   2. tools/tune_app/mfo_tune/tuningfiles.py: the `sw.*` names; tools/iaus_sim/Tune.h: a kSpellWorthKnobs table like kWorthKnobs;
//      tools/iaus_sim/tune_driver.py: move each field from SW_NOT_SEARCHED to SW_KNOBS with its values.
//   3. Inputs the Snapshot does not hold yet (each needs its own reader, main thread where it touches the engine, #83 / #85):
//        SpellEntry::chargeSec        SPIT charge time (MagicItem data), today Tuning::spellCastSec stands in for every spell
//        SpellEntry::dotPerSec, dotSec  the damage-over-time part apart from the direct part (Fills.cpp MeasureSpell sums them into `damage`)
//        SpellEntry::engineRow        the row cast/Archetype.cpp predicts (the claim-to-fire seconds differ by row: iaus-sim-v3.md 13.2)
//        SpellEntry::summonHit, summonHitsPerSec, summonHp   the summoned actor's base (ACHR / NPC_ of the summon effect)
//        SpellEntry::buffArmor, buffResist, buffResistAv, buffSec   what an armor / resist spell adds and for how long
//        SpellEntry::wardPower        a ward's magnitude
//        Combatant::lifeSec           a foe's expected remaining life (Estimate::Ttl over the party's damage on it exists for SELF only)
//        Snapshot::fightSec           the expected remaining length of the fight (for magicka per fight)
//      Until a reader exists the matching SpellIn field stays < 0 and its term is NOT APPLICABLE (never a made-up number: principle 7).
//   4. The call sites, ONE change each, all in iaus/Generate.cpp Casts: Facts::expDamage / expRate / bestDamage / effectRate / healRate /
//      controlSec become reads of SpellWorthOn (the DamageValue, Overkill, HealRate and ControlWorth considerations keep their curves), and
//      Estimate::UnmeasuredWorth keeps answering for a spell SpellWorthOn does not know.
//   5. tools/iaus_harness: a batch that pins each term (a stream cut short, an area on four foes, a DoT on a foe with two seconds to live, a
//      control on a foe that hits for nothing, an empty pool at mid fight). The lab's own check is the regret report (`--spell-regret`).
#include "iaus/Snapshot.h"

#include <string_view>

namespace MFO::Iaus {

    // THE NAMED TABLE. One struct, one name per field (`sw.<field>` in shared.txt, the tuner and the sim). PLACEHOLDERS, not searched yet.
    struct SpellWorth {
        // ── seconds a cast costs ─────────────────────────────────────────────────────────────────────────────────────────────────────
        // claim to fire: from the decision to the moment the engine's caster starts the spell. Base, then what each cast family adds (the field's
        // claim-to-fire tables differ by family: iaus-sim-v3.md 13.2; the per-ROW split waits for SpellEntry::engineRow).
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

    namespace Estimate {

        // What the reader needs beside the SpellEntry and the Snapshot. A field < 0 = NOT KNOWN: its term does not apply (item 3 of the header).
        struct SpellIn {
            float chargeSec = -1.0f;        // the spell's own charge time (fire-and-forget)
            float dotPerSec = -1.0f;        // damage over time, per second
            float dotSec = -1.0f;           // ... for this long
            float directDamage = -1.0f;     // the direct part of SpellEntry::damage (with dotPerSec known: damage = direct + dotPerSec x dotSec)
            float failShare = -1.0f;        // the share of his claims on this spell's row that never start (0..1)
            float foesInArea = -1.0f;       // Facts::foesInArea at this target
            float friendsInArea = -1.0f;    // party members inside the area at this target
            float foeLifeSec = -1.0f;       // the target's expected remaining life
            float fightSec = -1.0f;         // the fight's expected remaining length
            float summonDps = -1.0f;        // the summoned body's damage rate
            float buffPerSec = -1.0f;       // a buff: damage prevented or added per second while it is up
            float buffSec = -1.0f;          // ... for this long
            float wardPerSec = -1.0f;       // a ward: incoming magic damage per second it would absorb
            bool  redundant = false;        // its effect archetype is already live on the target from another spell
        };

        struct Worth {
            bool  known = false;            // false: the reader could not value this cast (the caller keeps today's estimate; never a 0 that reads as "worthless")
            float effect = 0.0f;            // the unit above, per cast
            float seconds = 0.0f;           // claim to fire + charge or channel + recover
            float magicka = 0.0f;           // what the cast costs him
            [[nodiscard]] float PerSecond() const { return known && seconds > 0.0f ? effect / seconds : 0.0f; }
            [[nodiscard]] float PerMagicka() const { return known && magicka > 0.0f ? effect / magicka : 0.0f; }
        };

        // THE ONE READER. a_target: the foe (offense, control), the recipient (heal, buff, ward), himself (summon). Every cast family's worth
        // consideration calls this and nothing else. SKELETON: returns Worth{} (known = false).
        [[nodiscard]] Worth SpellWorthOn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t,
                                         const SpellIn& a_in);

        // The terms, one function each so the harness can pin them. Each returns < 0 while it is a TODO or its input is not known.
        [[nodiscard]] float CastSeconds(const SpellEntry& a_e, const SpellWorth& a_t, const SpellIn& a_in, float a_concChannelSec);   // claim to fire + charge / channel + recover (+ failShare x failedStartSec)
        [[nodiscard]] float ChannelSeconds(const SpellWorth& a_t, float a_concChannelSec);                                             // a stream's held seconds
        [[nodiscard]] float AreaFactor(const SpellWorth& a_t, float a_foesInArea, float a_friendsInArea);                              // x the one-foe effect
        [[nodiscard]] float DotOverLife(const SpellWorth& a_t, float a_dotPerSec, float a_dotSec, float a_foeLifeSec);                  // a DoT's damage over the foe's expected life
        [[nodiscard]] float ControlEffect(const SpellWorth& a_t, float a_controlSec, const Combatant& a_foe);                           // held seconds x the foe's damage rate
        [[nodiscard]] float BuffEffect(const SpellWorth& a_t, const SpellIn& a_in);                                                     // prevented or added damage while it is up
        [[nodiscard]] float WardEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_wardSec);
        [[nodiscard]] float SummonEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_summonSec);
        [[nodiscard]] float MagickaShare(const SpellWorth& a_t, float a_cost, float a_magicka, float a_magickaRegenPerSec, float a_fightSec);   // 0..1: the share of the effect that stands after the pool is weighed against the fight

    }

}
