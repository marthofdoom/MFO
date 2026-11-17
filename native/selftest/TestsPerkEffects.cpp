// THE PERK-EFFECT SELF-TEST (dev builds only; ClickUp 1dfcuabd2pk, iaus/PerkEffects.h). Same rules as TestsT2.cpp: reads only inside step(),
// nothing is given to him and nothing is changed. One test:
//
//   perk.effects_table: waits for the follower's perk-effect table (built on the main thread by the snapshot pass), then judges
//     (a) the table itself: every group read, every multiplier finite and inside the reader's own band;
//     (b) THE CENSUS AGAINST THE ENGINE: a multiplier that is not 1.00 needs at least one entry of its entry point in his base perks (35 weapon
//         damage, 85 armor rating, 38 spell cost, 29 spell magnitude, 39 block). A multiplier with no such entry means a perk reaches him by a road
//         the census does not read, or the engine was asked with a wrong shape: either is a finding;
//     (c) THE WIRING: the real generator and scorer on a synthetic one-spell fight carrying his table. With his Destruction group forced to "no
//         perk" the cast scores S0; forced to half cost it must score S0 x (the live spell_perks curve at 0.5 / at 0), and a table whose spell group
//         is unread must score exactly S0 at any multiplier.
//   The evidence line is his real table (the `[perk-fx]` text), so a pass also records what the engine said about his perks.

#include "selftest/SelfTest_internal.h"
#include "iaus/Generate.h"
#include "iaus/PerkEffects.h"

#include <cmath>

namespace MFO::SelfTest {

    namespace {

        constexpr std::uint32_t kProbeSpell = 0x00012FCD;   // a FormID used as a plain key in the synthetic book (never resolved)

        // The synthetic cast's score with a_perks in the snapshot; -1 = the generator made no such candidate.
        float CastScore(RE::FormID a_self, const Iaus::PerkRead& a_perks) {
            Iaus::Snapshot s;
            s.self.id = a_self; s.self.hp = s.self.hpMax = 100.0f; s.self.sp = s.self.spMax = 100.0f; s.self.mp = s.self.mpMax = 200.0f; s.self.level = 10;
            s.playerId = 0x14;
            Iaus::Combatant foe;
            foe.id = 0x00000F02; foe.hp = foe.hpMax = 100.0f; foe.x = 600.0f; foe.dist = 600.0f; foe.level = 10; foe.targetId = a_self;
            foe.sight = Iaus::Sight::Visible; foe.sightEngine = Iaus::Sight::Visible;
            s.foes.push_back(foe);
            s.currentTargetId = foe.id;
            s.hasBook = true;
            Iaus::SpellEntry e;
            e.form = kProbeSpell; e.name = "perk self-test bolt"; e.nature = Iaus::Prune::Nature::Offense; e.role = Iaus::RoleTag::Fire;
            e.school = static_cast<std::int8_t>(Iaus::kDestruction); e.cost = 30.0f; e.reach = 2000.0f;
            s.book.entries.push_back(e);
            s.combatSec = 3.0f;
            s.perks = a_perks;
            const auto& t = Iaus::DefaultTuning();
            const auto  gen = Iaus::Generate(s, Iaus::ClassGate{}, t);
            for (const auto& cand : gen.actions)
                if (cand.family == Iaus::Family::CastOffense && cand.form == kProbeSpell)
                    return Iaus::ScoreOne(cand, Iaus::Mind{}, Iaus::Profile{}, t, 3, 0.0f, nullptr);
            return -1.0f;
        }

        Step PerkStep(Ctx& c) {
            const auto table = PerkEffects::Get(c.id);
            if (!table) {
                if (c.elapsed > 30.0f)
                    return Step::Fail("no perk-effect table after 30 s: the [perk-fx] status line at start says whether the reader is ON, and a "
                                      "`perks cannot be evaluated now` line says why he was skipped");
                return Step::Continue();
            }
            const auto& r = table->read;
            // (a) the table itself
            if (!r.read || !r.armorRead || !r.spellsRead) return Step::Fail("a group of the table is unread (weapons / armor / spells): " + table->text);
            const auto sane = [](float a_v) { return std::isfinite(a_v) && a_v >= 0.0f && a_v <= 100.0f; };
            bool ok = sane(r.blockPct);
            for (const float v : table->weaponKind) ok = ok && sane(v);
            for (const float v : r.weaponDamage) ok = ok && sane(v);
            for (const float v : r.armorRating) ok = ok && sane(v);
            for (std::size_t i = 0; i < Iaus::kSchools; ++i) ok = ok && sane(r.spellCost[i]) && sane(r.spellMagnitude[i]);
            if (!ok) return Step::Fail("a multiplier is not finite or outside 0..100: " + table->text);
            // (b) the census against the engine
            const auto has = [&](RE::BGSEntryPoint::ENTRY_POINT a_ep) { return table->entryCount[static_cast<std::size_t>(a_ep)] > 0; };
            using EP = RE::BGSEntryPoint::ENTRY_POINT;
            bool weaponMoved = false, armorMoved = false, costMoved = false, magMoved = false;
            for (const float v : table->weaponKind) weaponMoved = weaponMoved || v != 1.0f;
            for (const float v : r.armorRating) armorMoved = armorMoved || v != 1.0f;
            for (std::size_t i = 0; i < Iaus::kSchools; ++i) { costMoved = costMoved || r.spellCost[i] != 1.0f; magMoved = magMoved || r.spellMagnitude[i] != 1.0f; }
            const char* orphan = weaponMoved && !has(EP::kModAttackDamage)     ? "weapon damage (entry point 35)"
                               : armorMoved && !has(EP::kModArmorRating)       ? "armor rating (entry point 85)"
                               : costMoved && !has(EP::kModSpellCost)          ? "spell cost (entry point 38)"
                               : magMoved && !has(EP::kModSpellMagnitude)      ? "spell magnitude (entry point 29)"
                               : r.blockPct != 1.0f && !has(EP::kModPercentBlocked) ? "block (entry point 39)"
                               : nullptr;
            if (orphan)
                return Step::Fail(std::format("the engine moved {} but his base perks hold no entry of that entry point (a perk road the census does "
                                              "not read, or a wrong ask): {}", orphan, table->text));
            // (c) the wiring, on his own table with the Destruction group forced
            const auto d = static_cast<std::size_t>(Iaus::kDestruction);
            Iaus::PerkRead none = r;
            none.spellsRead = true; none.spellCount[d] = 1; none.spellCost[d] = 1.0f; none.spellMagnitude[d] = 1.0f;
            Iaus::PerkRead half = none;
            half.spellCost[d] = 0.5f;
            Iaus::PerkRead unread = half;
            unread.spellsRead = false;
            const float s0 = CastScore(c.id, none), s1 = CastScore(c.id, half), s2 = CastScore(c.id, unread);
            const auto& curve = Iaus::DefaultTuning().curve[static_cast<std::size_t>(Iaus::Cons::BoostSpellPerks)];
            const float at0 = Iaus::Eval(curve, 0.0f), at5 = Iaus::Eval(curve, 0.5f);
            const std::string ev = std::format("{} | wiring: cast scores {:.4f} with no perk, {:.4f} at half cost (live spell_perks curve x{:.3f} at 0, x{:.3f} "
                                               "at 0.5), {:.4f} with the spell group unread",
                                               table->text, s0, s1, at0, at5, s2);
            if (!(s0 > 0.0f) || !(at0 > 0.0f)) return Step::Fail("the synthetic cast did not score: " + ev);
            if (std::fabs(s1 / s0 - at5 / at0) > 1e-3f) return Step::Fail("the half-cost cast does not score by the spell_perks curve: " + ev);
            if (std::fabs(s2 * at0 - s0) > 1e-4f * s0) return Step::Fail("an unread spell group still moved the score: " + ev);
            return Step::Pass(ev);
        }

    }

    void RegisterPerkEffects() {
        Register({ "perk.effects_table", "1dfcuabd2pk",
                   "(no setup) his perk-effect table is read as the main thread built it; the census is checked against the engine's answers and the "
                   "real Generate + ScoreOne run on a synthetic one-spell fight carrying it (no spawn, nothing changes)",
                   60.0f, nullptr, nullptr, PerkStep, nullptr });
    }

}
