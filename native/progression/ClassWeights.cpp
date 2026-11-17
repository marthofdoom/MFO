// progression/ClassWeights.cpp -- THE CLASS-WEIGHTED AUTO SKILL POINTS (ClickUp 1dfcuabd3tr, marth 2026-11-13: "the class add one
// classes should strongly weight, and we need to have very minor crafting allocation too").
//
// WHAT: Progression's auto skill points follow its own ClassDef skill list (SkillScale.cpp WeightsFor, triangular over the add-on's list).
// With the Classes add-on ACTIVE and a class equipped, that class now pulls the grant strongly toward the skills its ROW implies, and
// EVERY follower gets a very small capped share into the crafting skills. This file holds the TUNABLES (one named table) and the two PURE
// pieces (the class profile, the blend); SkillScale.cpp::GrantWeightsFor reads the live actor and calls them.
//
// THE CLASS ROW CARRIES NO SKILL LIST, so the profile is DERIVED from what the row does declare (classes/Classes.h ClassDef):
//   weapons  : Weapon:: bits -> OneHanded (dagger / 1H; a mage's dagger does not count), TwoHanded, Marksman (bow / crossbow), Block (shield);
//   armor    : any melee weapon or a shield -> Heavy + Light (SkillScale prunes to the follower's dominant one, the WeightsFor rule);
//              a bow / crossbow / non-mage dagger without melee -> Light; a pure caster -> none;
//   schools  : the applied default gambit rows' SpellRole -> school (Fire/Frost/Shock/Area/Destruction -> Destruction, Heal / TurnUndead /
//              Ward -> Restoration, Summon / BoundWeapon / Reanimate -> Conjuration, Armor / Paralyze -> Alteration, Fear / Calm -> Illusion),
//              one unit per row;
//   unrestricted rows (T1, Dragonborn) fall back to the LINE (Melee: both weapons + armor; Ranged: bow + light; Mage / Hybrid: nothing).
// A class that yields no profile leaves the Progression weights untouched.
//
// TO CHANGE: edit `kClassWeightTuning` (shares, ceiling, units) or the three tables in ProfileFor (weapon bit / role / armor rules).
// Nothing here is serialized (PRGN / MCLS untouched): the weights only decide where FUTURE points go, placed points never move.
// THREADING: pure; the caller runs where RecomputeSkills runs today.
#include "PCH.h"
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"
#include "classes/Classes.h"

namespace MFO::ProgAllocator {

    // PLACEHOLDERS (marth 2026-11-13: strongly weighted class, very minor crafting). Tune here.
    const ClassWeightTuning kClassWeightTuning{
        /*classShare*/        0.60f,   // fraction of the grant that follows the class profile (rest = the Progression list's own weights)
        /*craftShare*/        0.04f,   // TOTAL fraction into Smithing / Enchanting / Alchemy, split equally over the ones still under the ceiling; 0 = off
        /*craftCeiling*/      50.0f,   // a crafting skill whose base is at or above this takes no crafting share
        /*weaponUnit*/        2.0f,
        /*shieldUnit*/        1.0f,
        /*armorUnit*/         1.0f,
        /*schoolUnitPerRow*/  1.0f,
    };

    namespace {
        using AV = RE::ActorValue;
        using Vec = std::vector<std::pair<AV, float>>;

        void Add(Vec& a_v, AV a_av, float a_w) {
            if (a_w <= 0.0f) return;
            for (auto& [av, w] : a_v)
                if (av == a_av) { w += a_w; return; }
            a_v.emplace_back(a_av, a_w);
        }

        AV SchoolOf(Classes::SpellRole a_role) {
            using R = Classes::SpellRole;
            switch (a_role) {
            case R::Destruction: case R::Fire: case R::Frost: case R::Shock: case R::Area: return AV::kDestruction;
            case R::Heal: case R::TurnUndead: case R::Ward:                                return AV::kRestoration;
            case R::Summon: case R::BoundWeapon: case R::Reanimate:                        return AV::kConjuration;
            case R::Armor: case R::Paralyze:                                               return AV::kAlteration;
            case R::Fear: case R::Calm:                                                    return AV::kIllusion;
            default:                                                                       return AV::kNone;   // None / Attack: no school
            }
        }

        void Normalize(Vec& a_v) {
            float t = 0.0f;
            for (const auto& e : a_v) t += e.second;
            if (!(t > 0.0f)) { a_v.clear(); return; }
            for (auto& e : a_v) e.second /= t;
        }
    }

    // The class row's skill profile (unnormalized -> normalized to sum 1; empty = the row implies none).
    std::vector<std::pair<RE::ActorValue, float>> ClassProfile(const Classes::ClassDef& a_def) {
        const auto& T = kClassWeightTuning;
        namespace W = Classes::Weapon;
        Vec v;
        const bool mage = a_def.line == Classes::Line::Mage;
        bool melee = false, bow = false;

        if (a_def.weapons & W::kUnrestricted) {
            switch (a_def.line) {
            case Classes::Line::Melee:
                Add(v, AV::kOneHanded, T.weaponUnit); Add(v, AV::kTwoHanded, T.weaponUnit); melee = true; break;
            case Classes::Line::Ranged:
                Add(v, AV::kArchery, T.weaponUnit); bow = true; break;
            default: break;   // Mage / Hybrid unrestricted (T1 Mage, Dragonborn): the gambit rows below, else nothing
            }
        } else {
            if (a_def.weapons & W::kOneHand)  { Add(v, AV::kOneHanded, T.weaponUnit); melee = true; }
            if (a_def.weapons & W::kTwoHand)  { Add(v, AV::kTwoHanded, T.weaponUnit); melee = true; }
            if (a_def.weapons & W::kDagger && !mage) { Add(v, AV::kOneHanded, T.weaponUnit); bow = true; }   // dagger line (Thief): light armor
            if (a_def.weapons & (W::kBow | W::kCrossbow)) { Add(v, AV::kArchery, T.weaponUnit); bow = true; }
            if (a_def.weapons & W::kShield)   { Add(v, AV::kBlock, T.shieldUnit); melee = true; }
        }
        if (melee) { Add(v, AV::kHeavyArmor, T.armorUnit); Add(v, AV::kLightArmor, T.armorUnit); }   // pruned to the dominant one by the caller
        else if (bow) Add(v, AV::kLightArmor, T.armorUnit);

        for (std::uint8_t i = 0; a_def.gambits && i < a_def.gambitCount; ++i) {
            const auto& g = a_def.gambits[i];
            if (!g.IsApplied() || g.pendingRuling) continue;
            const AV s = SchoolOf(g.role);
            if (s != AV::kNone) Add(v, s, T.schoolUnitPerRow);
        }
        Normalize(v);
        return v;
    }

    // The blend. a_base = the Progression list's weights (sum 1), a_profile = ClassProfile (sum 1 or empty), a_craftUnder = the crafting skills
    // still under the ceiling. Pure. Empty a_base stays empty (a class with no skill list grants nothing today and still does not).
    std::vector<std::pair<RE::ActorValue, float>> BlendGrantWeights(const std::vector<std::pair<RE::ActorValue, float>>& a_base,
                                                                    const std::vector<std::pair<RE::ActorValue, float>>& a_profile,
                                                                    const std::vector<RE::ActorValue>& a_craftUnder) {
        const auto& T = kClassWeightTuning;
        if (a_base.empty()) return a_base;
        Vec out;
        const float cs = a_profile.empty() ? 0.0f : std::clamp(T.classShare, 0.0f, 1.0f);
        for (const auto& [av, w] : a_base) Add(out, av, w * (1.0f - cs));
        for (const auto& [av, w] : a_profile) Add(out, av, w * cs);
        const float craft = a_craftUnder.empty() ? 0.0f : std::clamp(T.craftShare, 0.0f, 0.5f);
        if (craft > 0.0f) {
            for (auto& e : out) e.second *= (1.0f - craft);
            for (const auto av : a_craftUnder) Add(out, av, craft / static_cast<float>(a_craftUnder.size()));
        }
        return out;
    }
}
