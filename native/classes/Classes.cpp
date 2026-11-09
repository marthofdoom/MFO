// classes/Classes.cpp -- accessors over the compiled class table, the unlock predicate, and the
// load-time self-check (card C2). Pure data reads: nothing here touches the engine.
#include "Classes_internal.h"
#include "CombatStyle.h"
#include <bit>

namespace MFO::Classes {

    // The T1 root ordinals ARE State.h combatClassOverride (and CombatStyle::Stance), FROZEN. If either ever
    // moved, the table's rootT1 column would silently lie, so pin them here.
    static_assert(static_cast<std::uint8_t>(CombatStyle::Stance::Melee)  == 1, "combatClassOverride 1 = Melee");
    static_assert(static_cast<std::uint8_t>(CombatStyle::Stance::Ranged) == 2, "combatClassOverride 2 = Ranged");
    static_assert(static_cast<std::uint8_t>(CombatStyle::Stance::Cast)   == 3, "combatClassOverride 3 = Mage");

    const ClassDef& Get(Id a_id) {
        const auto i = static_cast<std::size_t>(a_id);
        return kTable[i < kClassCount ? i : 0];   // out of range -> Melee, never UB (callers pass a valid Id)
    }

    const ClassDef* FindBySlug(std::string_view a_slug) {
        for (const auto& d : kTable)
            if (d.slug == a_slug) return &d;
        return nullptr;
    }

    bool UnlockSatisfied(const ClassDef& a_def, Mask a_mastered) {
        const auto& u = a_def.unlock;
        if ((a_mastered & u.allOf) != u.allOf) return false;
        if (std::popcount(a_mastered & u.poolA) < static_cast<int>(u.needA)) return false;
        if (std::popcount(a_mastered & u.poolB) < static_cast<int>(u.needB)) return false;
        return true;
    }

    const ClassDef* T1ForOverride(std::uint8_t a_combatClassOverride) {
        if (a_combatClassOverride < 1 || a_combatClassOverride > 3) return nullptr;
        // T1 classes are table entries 0..2 in override order 1..3 (Melee, Ranged, Mage).
        return &kTable[a_combatClassOverride - 1];
    }

    int detail::SelfCheck() {
        int defects = 0;
        auto bad = [&](const ClassDef& d, const char* why) {
            ++defects;
            spdlog::error("[classes] self-check: {} -- {}", d.slug, why);
        };

        int perTier[6] = {};
        int perLine[4] = {};
        int hybridsT3 = 0;
        int placeholders = 0, applied = 0;
        for (std::size_t i = 0; i < kClassCount; ++i) {
            const auto& d = kTable[i];
            if (static_cast<std::size_t>(d.id) != i) bad(d, "id does not match its table index");
            if (d.tier < 1 || d.tier > 5) { bad(d, "tier out of range"); continue; }
            ++perTier[d.tier];
            ++perLine[static_cast<int>(d.line)];
            if (d.tier == 3 && d.line == Line::Hybrid) ++hybridsT3;
            if (d.rootT1 > 3) bad(d, "rootT1 is not a combatClassOverride ordinal");
            if (d.tier > 1 && d.rootT1 == 0 && d.id != Id::Dragonborn) bad(d, "no T1 root");
            for (std::size_t j = 0; j < i; ++j)
                if (kTable[j].slug == d.slug) bad(d, "duplicate slug");
            if (FindBySlug(d.slug) != &d) bad(d, "slug does not resolve back to itself");

            // Every parent id resolves: the masks may only name real classes of the tier before.
            const Mask all = (Mask{ 1 } << kClassCount) - 1;
            if (d.parentMask & ~all) bad(d, "parentMask names a class beyond the roster");
            if (d.tier == 1) {
                if (d.parentMask != 0) bad(d, "a tier-1 class has parents");
            } else {
                if (d.parentMask == 0) bad(d, "no parents");
                for (std::size_t p = 0; p < kClassCount; ++p) {
                    if (!(d.parentMask & (Mask{ 1 } << p))) continue;
                    const auto& par = kTable[p];
                    if (par.tier + 1 != d.tier) {
                        ++defects;
                        spdlog::error("[classes] self-check: {} lists parent {} of tier {} (expected {})",
                                      d.slug, par.slug, par.tier, d.tier - 1);
                    }
                }
            }
            const auto& u = d.unlock;
            if ((u.allOf & u.poolA) || (u.allOf & u.poolB) || (u.poolA & u.poolB)) bad(d, "unlock pools overlap");
            if (std::popcount(u.poolA) < u.needA || std::popcount(u.poolB) < u.needB) bad(d, "unlock needs more than its pool holds");
            if (d.gambitCount == 0 && d.tier > 1) bad(d, "no default gambits");
            for (int g = 0; g < d.gambitCount; ++g) {
                const auto& r = d.gambits[g];
                const std::string_view c = r.cond, a = r.act;
                if (r.IsApplied()) {
                    ++applied;
                    if (c.rfind("cond.", 0) != 0 || a.rfind("act.", 0) != 0) bad(d, "an applied row carries a non-base opcode");
                } else {
                    ++placeholders;
                    if (c.rfind("todo.", 0) != 0 && a.rfind("todo.", 0) != 0) bad(d, "a placeholder row names no todo opcode");
                }
            }
        }
        if (perTier[1] != 3 || perTier[2] != 9 || perTier[3] != 14 || perTier[4] != 8 || perTier[5] != 1) {
            ++defects;
            spdlog::error("[classes] self-check: tier counts are not 3/9/14/8/1");
        }
        if (hybridsT3 != 6) {
            ++defects;
            spdlog::error("[classes] self-check: {} tier-3 hybrids, expected 6", hybridsT3);
        }

        spdlog::info("[classes] table: {} classes (T1 {}, T2 {}, T3 {} [{} hybrid], T4 {}, T5 {}); lines melee {} ranged {} mage {} hybrid {}; "
                     "default gambit rows {} applied, {} placeholder (not applied); {}",
                     kClassCount, perTier[1], perTier[2], perTier[3], hybridsT3, perTier[4], perTier[5],
                     perLine[0], perLine[1], perLine[2], perLine[3], applied, placeholders,
                     defects ? "SELF-CHECK FAILED" : "all parents resolve");
        return defects;
    }
}
