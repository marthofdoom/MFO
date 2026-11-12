// iaus/ClassCurves.cpp -- the per-class curve table (contract in iaus/ClassCurves.h). PURE.
#include "iaus/ClassCurves.h"

#include "iaus/Reposition.h"

#include <cmath>

namespace MFO::Iaus::ClassCurves {

    static_assert(Scalars{}.touchSpentSec == Reposition::kTouchSpentSec, "Scalars::touchSpentSec is Reposition::kTouchSpentSec");
    static_assert(ClassGate{}.touchSpentSec == Reposition::kTouchSpentSec, "ClassGate::touchSpentSec is Reposition::kTouchSpentSec");

    namespace {
        // One Row per class. A row holds only what a search or a design ruling put there, each with its evidence; everything else scores
        // with the shared catalogue.
        // School peaks (home schools): marth's School Circle save 2026-11-12, https://claude.ai/artifact/2wzN86WzfTHoZpie1jxGrY
        // Circle order: Destruction - Alteration - Restoration - Illusion - Conjuration (Core.h kSchoolCircle). Wizard, Mage and the other
        // classes have NO home school (flat, no mastery bump); Arch-Mage is "any school" (schoolAny, below).
        // THE WAVE'S SHAPE IS SHIPPED AND FIXED: floor 0.35, sharpness 1.5, mastery bump 0.15 on every row that has a home school
        // (marth's School Circle save 2026-11-12; marth: "My settings there are truth. The school circle I just designed isn't something
        // that should get tweaked"). A fixed design input: NEVER searched, never adjusted by the sim (tools/iaus_sim/edge_search.py has no
        // school parameter on purpose). Weights on the circle from a peak: 1.0 at home, 0.694 one step away, 0.369 two steps away.
        constexpr float kSchoolFloor = 0.35f, kSchoolShape = 1.5f, kSchoolMasteredBump = 0.15f;
        void SeedSchoolPeaks(Table& t) {
            using Id = std::size_t;
            constexpr Id kEvoker = 8, kConjurer = 9, kHealer = 10, kWarlock = 11, kNecromancer = 18, kPriest = 19,
                         kSpellsword = 20, kSorcerer = 21, kTemplar = 22, kNightblade = 23, kWitchhunter = 24, kScout = 25,
                         kGrandBattlemage = 31, kSaint = 33;
            t[kEvoker].scalars.schoolPeak = kDestruction;   t[kSpellsword].scalars.schoolPeak = kDestruction;   t[kGrandBattlemage].scalars.schoolPeak = kDestruction;
            t[kConjurer].scalars.schoolPeak = kConjuration; t[kNecromancer].scalars.schoolPeak = kConjuration;  t[kSorcerer].scalars.schoolPeak = kConjuration;
            t[kHealer].scalars.schoolPeak = kRestoration;   t[kPriest].scalars.schoolPeak = kRestoration;       t[kTemplar].scalars.schoolPeak = kRestoration;
            t[kScout].scalars.schoolPeak = kRestoration;    t[kSaint].scalars.schoolPeak = kRestoration;
            t[kWarlock].scalars.schoolPeak = kIllusion;     t[kNightblade].scalars.schoolPeak = kIllusion;      t[kWitchhunter].scalars.schoolPeak = kIllusion;
            for (Row& r : t)   // every row with a home school carries marth's fixed shape (a row without one stays flat: SchoolWeight ignores the floor)
                if (r.scalars.schoolPeak >= 0) { r.scalars.schoolFloor = kSchoolFloor; r.scalars.schoolShape = kSchoolShape; r.scalars.schoolMasteredBump = kSchoolMasteredBump; }
        }

        // THE EDGE-NUMBER SEARCH (sim batch 2026-11-12, Docs/research/2026-11/iaus-sim-v2.md section 32; tools/iaus_sim/edge_search.py).
        // Rule: a value is written here only if it beats noise on the seeds it was picked on, on seeds it was not, on fresh seeds across the
        // built-in world and two lists' real enemies, loses nothing with stealth off or as shipped, AND passes the three fixed gates of
        // section 31.11. Searched per class for the 12 Beta 1 classes and the 3 base rows: the health potion's DrinkNeed midpoint and
        // slope, the magicka / stamina potion's midpoint, touchSpentSec, the Knight's guard-line boost and drink line (1236 values in 165
        // sweeps on three worlds). NO VALUE PASSED EVERYTHING, so no row carries a searched override:
        //   - the health line is flat between 0.20 and 0.70 on the built-in world and its best value differs by list (the Knight wants 0.70
        //     on one list and 0.34 or lower on another): no class gets its own;
        //   - the Knight's guard-line boost is on its plateau at the shared 2.0 (guard share 61.9 % at 1.0, 67.1 at 1.5, 67.5 from 2.0 up);
        //   - touchSpentSec could not be rated (the lab never opens a touch window): its 3 s is still a hand value;
        //   - CLOSEST CANDIDATE, not shipped: Warrior, stamina potion DrinkNeed midpoint 0.34 -> 0.15
        //       (SetCurveParam(t[4], Family::DrinkStamina, Cons::DrinkNeed, DefaultTuning().curve[Cons::DrinkNeed], "b", 0.15f)).
        //     96 fresh seeds: built-in +0.287 J (se 0.079), stealth off +0.275, as shipped +0.279, LoreRim +0.065 (se 0.031), Tuxborn
        //     +0.109 (se 0.054); about 0.13 J of it is the stamina potion he no longer spends. But on the six fixed gate seeds of 31.11 the
        //     Warrior is -0.50 J and the 12-class mean falls by 0.04 at all three gates, and those gates are hard constraints.

        Table MakeDefaults() {
            Table t{};
            SeedSchoolPeaks(t);
            t[kArchMage].scalars.schoolAny = true;   // marth 11-12: "obviously Arch mage gets any school"
            for (std::size_t i = 0; i < 3; ++i) t[kBaseOffFirst + i] = t[i];   // each tree-OFF base row starts as its T1 row
            return t;
        }
    }

    const Table& Defaults() {
        static const Table t = MakeDefaults();
        return t;
    }

    namespace { constexpr float kMinSchoolWeight = 1.0e-3f; }   // the lowest a school weight can be (a searched floor 0 or a huge shape must not veto)

    float SchoolWeight(const Scalars& a_s, std::size_t a_school) {
        if (a_s.schoolAny || a_s.schoolPeak < 0 || a_s.schoolPeak >= static_cast<int>(kSchools) || a_school >= kSchools) return 1.0f;
        if (a_s.schoolAny || !(a_s.schoolFloor < 1.0f)) return 1.0f;   // Arch-Mage (any school) / flat (also a NaN floor): exactly 1.0, no arithmetic
        auto pos = [](std::size_t a_id) {   // the school's seat on the circle
            for (std::size_t i = 0; i < kSchools; ++i) if (static_cast<std::size_t>(kSchoolCircle[i]) == a_id) return i;
            return std::size_t{ 0 };
        };
        const std::size_t peak = pos(static_cast<std::size_t>(a_s.schoolPeak));
        a_school = pos(a_school);
        const std::size_t diff = a_school > peak ? a_school - peak : peak - a_school;
        const std::size_t d = diff < kSchools - diff ? diff : kSchools - diff;
        if (d == 0) return 1.0f;
        const float fl = a_s.schoolFloor > kMinSchoolWeight ? a_s.schoolFloor : kMinSchoolWeight;   // never 0: affinity is a weight, never a veto
        const float shape = a_s.schoolShape > 0.0f ? a_s.schoolShape : 1.0f;
        constexpr double kTwoPi = 6.283185307179586;
        const double wave = 0.5 + 0.5 * std::cos(kTwoPi * static_cast<double>(d) / static_cast<double>(kSchools));
        const float w = fl + (1.0f - fl) * static_cast<float>(std::pow(wave, static_cast<double>(shape)));
        return w > kMinSchoolWeight ? w : kMinSchoolWeight;
    }

    Profile Build(const Row& a_row) {
        Profile p;
        p.curves = a_row.curves;
        p.weights = a_row.weights;
        for (const FamilyWeight& fw : a_row.families)
            if (static_cast<std::size_t>(fw.family) < kFamilies) p.family[static_cast<std::size_t>(fw.family)] = fw.w;
        for (std::size_t s = 0; s < kSchools; ++s) p.school[s] = SchoolWeight(a_row.scalars, s);
        return p;
    }

    const Profile& ProfileFor(std::uint8_t a_classId) {
        static const std::array<Profile, kRows + 1> profiles = [] {
            std::array<Profile, kRows + 1> a{};
            for (std::size_t i = 0; i < kRows; ++i) a[i] = Build(Defaults()[i]);
            return a;   // [kRows] = the empty default
        }();
        return profiles[a_classId < kRows ? a_classId : kRows];
    }

    void Apply(const Row& a_row, std::uint8_t a_classId, ClassGate& a_gate) {
        a_gate.curveClass      = a_classId;
        float line = a_row.scalars.drinkHealthLine;
        if (!a_row.scalars.drinkHealthLineSet)   // the guard-line cut-off follows the row's own DrinkNeed midpoint unless a value was set
            for (const auto& o : a_row.curves)
                if (o.family == Family::DrinkHealth && o.cons == Cons::DrinkNeed && o.curve.kind == CurveKind::Logi) line = o.curve.b;
        a_gate.drinkHealthLine = line;
        a_gate.touchSpentSec   = a_row.scalars.touchSpentSec;
    }

    void ApplyMastered(const Table& a_table, std::uint64_t a_mask, ClassGate& a_gate) {
        for (std::size_t i = 0; i < kClassRows; ++i) {
            if ((a_mask & (std::uint64_t{ 1 } << i)) == 0) continue;
            const Scalars& sc = a_table[i].scalars;
            if (sc.schoolPeak < 0 || sc.schoolPeak >= static_cast<int>(kSchools) || !(sc.schoolMasteredBump > 0.0f)) continue;
            a_gate.schoolBump[static_cast<std::size_t>(sc.schoolPeak)] += sc.schoolMasteredBump;
        }
    }

    void ApplyMastered(std::uint64_t a_mask, ClassGate& a_gate) { ApplyMastered(Defaults(), a_mask, a_gate); }

    void Apply(std::uint8_t a_classId, ClassGate& a_gate) {
        if (a_classId >= kRows) return;   // no class: the gate keeps the shared constants
        Apply(Defaults()[a_classId], a_classId, a_gate);
    }

    bool SetCurveParam(Row& a_row, Family a_f, Cons a_c, const Curve& a_shared, std::string_view a_param, float a_v) {
        // A Piece point: px<i> / py<i>. Checked against the curve it will edit (the row's override, else the shared one) BEFORE an override is made.
        const bool point = a_param.size() == 3 && a_param[0] == 'p' && (a_param[1] == 'x' || a_param[1] == 'y') && a_param[2] >= '0' && a_param[2] <= '5';
        if (!point && a_param != "a" && a_param != "b" && a_param != "floor" && a_param != "zeroAbove" && a_param != "zeroBelow") return false;
        if (point) {
            const Curve* cur = &a_shared;
            for (const auto& e : a_row.curves) if (e.family == a_f && e.cons == a_c) { cur = &e.curve; break; }
            const std::size_t i = static_cast<std::size_t>(a_param[2] - '0');
            if (cur->kind != CurveKind::Piece || i >= cur->np || !std::isfinite(a_v)) return false;
            if (a_param[1] == 'x' && ((i > 0 && a_v < cur->px[i - 1]) || (i + 1 < cur->np && a_v > cur->px[i + 1]))) return false;
        }
        Profile::CurveOverride* o = nullptr;
        for (auto& e : a_row.curves) if (e.family == a_f && e.cons == a_c) { o = &e; break; }
        if (!o) { a_row.curves.push_back({ a_f, a_c, a_shared }); o = &a_row.curves.back(); }
        if (point) { (a_param[1] == 'x' ? o->curve.px : o->curve.py)[static_cast<std::size_t>(a_param[2] - '0')] = a_v; return true; }
        if (a_param == "a") o->curve.a = a_v;
        else if (a_param == "b") o->curve.b = a_v;
        else if (a_param == "floor") o->curve.floor = a_v;
        else if (a_param == "zeroAbove") o->curve.zeroAbove = a_v;
        else o->curve.zeroBelow = a_v;
        return true;
    }

    bool SetScalar(Row& a_row, std::string_view a_name, float a_v) {
        if (a_name == "drinkHealthLine") { a_row.scalars.drinkHealthLine = a_v; a_row.scalars.drinkHealthLineSet = true; }
        else if (a_name == "touchSpentSec") a_row.scalars.touchSpentSec = a_v;
        else if (a_name == "schoolPeak") a_row.scalars.schoolPeak = static_cast<std::int8_t>(!std::isfinite(a_v) || a_v < 0.0f ? -1 : (a_v >= static_cast<float>(kSchools) ? -1 : static_cast<int>(a_v)));
        else if (a_name == "schoolFloor") a_row.scalars.schoolFloor = a_v;
        else if (a_name == "schoolShape") a_row.scalars.schoolShape = a_v;
        else if (a_name == "schoolMasteredBump") a_row.scalars.schoolMasteredBump = a_v;
        else return false;
        return true;
    }

    bool SetWeight(Row& a_row, Family a_f, Cons a_c, float a_w) {
        if (!std::isfinite(a_w) || !(a_w > 0.0f) || static_cast<std::size_t>(a_f) >= kFamilies || static_cast<std::size_t>(a_c) >= kCons) return false;
        for (auto& e : a_row.weights) if (e.family == a_f && e.cons == a_c) { e.w = a_w; return true; }
        a_row.weights.push_back({ a_f, a_c, a_w });
        return true;
    }

    bool SetFamilyWeight(Row& a_row, Family a_f, float a_w) {
        if (!std::isfinite(a_w) || !(a_w > 0.0f) || static_cast<std::size_t>(a_f) >= kFamilies) return false;
        for (auto& e : a_row.families) if (e.family == a_f) { e.w = a_w; return true; }
        a_row.families.push_back({ a_f, a_w });
        return true;
    }
}
