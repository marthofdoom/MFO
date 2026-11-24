#pragma once
// iaus/SpellWorth.h -- WHAT ONE CAST OF A SPELL IS WORTH, IN ONE UNIT FOR EVERY CAST FAMILY (sim v4 batch C, slice S11b; design
// Docs/research/2026-11/iaus-sim-v4.md 7.4, 7.5 and 14).
//
// *** FILLED (S11b), SHIPPED INERT. *** The bodies below are real; the call sites (iaus/Generate.cpp Casts) read them through
// Tuning::spellWorth.live, which ships 0: with it every Facts::expDamage / expRate / bestDamage / effectRate / healRate / controlSec / expHeal is today's
// estimate, bit for bit, and this reader is never called. Raising `live` (the tuning round) blends the reader in where it KNOWS the cast.
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
// WHERE EACH INPUT COMES FROM (S11b as built; Docs/research/2026-11/iaus-sim-v4.md 15.6):
//   the table                  iaus/SpellWorthTable.h, Tuning::spellWorth, `sw.<field>` through Catalogue.cpp SetTuningScalar
//   SpellEntry::chargeSec ... wardPower, engineRow    iaus/Fills.cpp MeasureSpell (the one effect reader), form data only
//   Combatant life, fightSec   Estimate::FoeLifeSec / FightSec below (pure, from the party's weapon damage on the foe: no new engine read, no new thread)
//   SpellIn                    Estimate::MakeSpellIn (ONE builder: the generator and the harness call it)
//   NOT READ (left < 0, the term does not apply): the summoned body's hit RATE and health, failShare, a weapon buff's added damage, `redundant`,
//   the magicka regen rate (counted 0). See iaus-sim-v4.md 15.6 "as built".
#include "iaus/Snapshot.h"
#include "iaus/SpellWorthTable.h"

#include <string_view>

namespace MFO::Iaus {

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
            float magickaRegenPerSec = -1.0f;   // < 0 = not known (counted 0 by MagickaShare)
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
        // consideration calls this and nothing else. known = false when a term the cast's nature needs is not known.
        [[nodiscard]] Worth SpellWorthOn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t,
                                         const SpellIn& a_in, float a_concChannelSec);   // a_concChannelSec = Tuning::concChannelSec

        // The terms, one function each so the harness can pin them. Each returns < 0 when its input is not known.
        [[nodiscard]] float CastSeconds(const SpellEntry& a_e, const SpellWorth& a_t, const SpellIn& a_in, float a_concChannelSec);   // claim to fire + charge / channel + recover (+ failShare x failedStartSec)
        [[nodiscard]] float ChannelSeconds(const SpellWorth& a_t, float a_concChannelSec);                                             // a stream's held seconds
        [[nodiscard]] float AreaFactor(const SpellWorth& a_t, float a_foesInArea, float a_friendsInArea);                              // x the one-foe effect
        [[nodiscard]] float DotOverLife(const SpellWorth& a_t, float a_dotPerSec, float a_dotSec, float a_foeLifeSec);                  // a DoT's damage over the foe's expected life
        [[nodiscard]] float ControlEffect(const SpellWorth& a_t, float a_controlSec, const Combatant& a_foe);                           // held seconds x the foe's damage rate
        [[nodiscard]] float BuffEffect(const SpellWorth& a_t, const SpellIn& a_in);                                                     // prevented or added damage while it is up
        [[nodiscard]] float WardEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_wardSec);
        [[nodiscard]] float SummonEffect(const SpellWorth& a_t, const SpellIn& a_in, float a_summonSec);
        [[nodiscard]] float MagickaShare(const SpellWorth& a_t, float a_cost, float a_castSec, float a_magicka, float a_magickaRegenPerSec, float a_fightSec);   // 0..1: the share of the effect that stands after the pool is weighed against the fight

        // The inputs derived from the Snapshot (pure; the generator and the harness both call these).
        [[nodiscard]] float FoeLifeSec(const Snapshot& a_s, const Combatant& a_foe);   // the foe's health over the party's weapon damage on it; < 0 = no damage measured
        [[nodiscard]] float FightSec(const Snapshot& a_s);                              // the foes' lives added up (the party focuses one at a time); < 0 = any unmeasured
        // THE ONE BUILDER of SpellIn: a_target as in SpellWorthOn; a_foesInArea / a_friendsInArea < 0 when the spell has no area.
        [[nodiscard]] SpellIn MakeSpellIn(const SpellEntry& a_e, const Combatant& a_target, const Snapshot& a_s, const SpellWorth& a_t,
                                          float a_foesInArea, float a_friendsInArea);

    }

}
