#pragma once
// iaus/ClassCurves.h -- the PER-CLASS curve table (ruling Q-class-curves, marth 11-12: "each class should already have its own
// curves"; the edge numbers are found by the sim, tools/iaus_sim). PURE: no engine header, no Classes.h (the harness and the sim
// compile this file; SpellBook.cpp asserts kRows == Classes::kClassCount).
//
// A class row holds (a) CURVE overrides: a (Family, Cons) pair scored with this curve instead of the shared Tuning::curve[cons]
// (read by Score.cpp through Profile::curves), and (b) the hand-picked EDGE constants that decide a class-relevant edge case
// (Scalars). The shipped table is every class row PRESENT with NO override and the shared constants, so every score is
// bit-identical to the uncustomised catalogue until the sim batch fills a row in. The tree-OFF base classes
// (Classes::T1ForOverride: Melee / Ranged / Mage) have their OWN rows 35..37, seeded from the T1 rows 0..2.
//
// THREADING: the shipped table and the profiles built from it are immutable after their first (thread-safe) use; any thread may
// read them. The sim edits its own COPY of the table (Table), never the shipped one.
#include "iaus/Core.h"
#include "iaus/Snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace MFO::Iaus::ClassCurves {

    inline constexpr std::size_t kClassRows = 35;                  // Classes::kClassCount (SpellBook.cpp asserts it); rows 0..34: the row index IS the Classes::Id ordinal
    inline constexpr std::size_t kBaseOffFirst = kClassRows;       // rows 35..37: the tree-OFF base classes (melee-off / ranged-off / mage-off), one per T1 root
    inline constexpr std::size_t kRows = kClassRows + 3;           // marth: the tree-OFF bases are their OWN cells that loosely follow the tree-ON versions

    // The class-relevant edge constants. Defaults are the shared constants they replace.
    struct Scalars {
        float drinkHealthLine = kDrinkHealthLine;   // Generate.cpp: below it (health potion in stock) the Knight guard line is off, drink first
        bool  drinkHealthLineSet = false;           // false: Apply derives it from the row's DrinkHealth/DrinkNeed midpoint override (b) when there is one
        float touchSpentSec   = 3.0f;               // Candidates.cpp: seconds after a touch cast that he is "backing out" (== Reposition::kTouchSpentSec)
        // School affinity (ruling Q-school-affinity): the score multiplier of a spell of school s is a smooth wave over the school
        // circle (Destruction - Alteration - Restoration - Illusion - Conjuration - back): SchoolWeight(). schoolFloor 1.0 = FLAT
        // (every weight exactly 1.0: a row with no home school), so schoolPeak only matters with a floor below 1. A home-school row
        // SHIPS marth's fixed shape (floor 0.35, sharpness 1.5, mastered bump 0.15; ClassCurves.cpp SeedSchoolPeaks, never searched).
        std::int8_t schoolPeak  = -1;               // School identity he peaks on (Core.h School); -1 = none (non-caster / all-rounder)
        float       schoolFloor = 1.0f;             // the weight at the far side of the circle, 0..1; 1 = flat
        float       schoolShape = 1.0f;             // sharpness: 1 = plain cosine wave, > 1 narrower peak
        // marth 11-12: "cross class tweaks from mastered classes that boost the weight a bit". When a follower has MASTERED this class,
        // his weight at THIS row's schoolPeak rises by this much (summed over mastered casting classes, clamped at the peak weight 1.0).
        float       schoolMasteredBump = 0.0f;      // 0 on a row with no home school; a home-school row ships marth's fixed 0.15 (ClassCurves.cpp, never searched)
        // marth 11-12: "obviously Arch mage gets any school". A FIXED rule, not a searched param (no SetScalar name): flat 1.0 over every school.
        bool        schoolAny = false;
    };

    inline constexpr std::size_t kArchMage = 26;    // Classes::Id::ArchMage (SpellBook.cpp asserts it); its row is schoolAny

    // weight(s) = floor + (1 - floor) * (0.5 + 0.5 * cos(2 pi d / kSchools))^shape, d = circular step distance from the peak along kSchoolCircle.
    // Exactly 1.0 when flat (floor >= 1 or no peak) and at d = 0. Never below the floor, never above 1.
    [[nodiscard]] float SchoolWeight(const Scalars& a_s, std::size_t a_school);   // a_school = School identity; distance is measured along Core.h kSchoolCircle

    struct Row {
        std::vector<Profile::CurveOverride> curves;
        Scalars scalars;
    };
    using Table = std::array<Row, kRows>;

    // The mastered casting classes' bumps onto a gate: for each class in the mask (bit i = Classes::Id i) whose row has a peak school,
    // gate.schoolBump[peak] += that row's schoolMasteredBump. The shipped table, and any Table (the sim's edited copy).
    void ApplyMastered(std::uint64_t a_masteredMask, ClassGate& a_gate);
    void ApplyMastered(const Table& a_table, std::uint64_t a_masteredMask, ClassGate& a_gate);

    // The shipped table (placeholder structure: every row present, no override, shared defaults).
    [[nodiscard]] const Table& Defaults();

    // The Profile a class scores with, built ONCE per row from Defaults() and cached (a class change is a different reference,
    // never a rebuild). An id outside the table (0xFF: no class resolved) is the empty default Profile.
    [[nodiscard]] const Profile& ProfileFor(std::uint8_t a_classId);

    // The shipped row's edge constants onto a gate (SpellBook::GateFor), and the same from any Row (the sim's edited copy).
    void Apply(std::uint8_t a_classId, ClassGate& a_gate);
    void Apply(const Row& a_row, std::uint8_t a_classId, ClassGate& a_gate);

    // A fresh Profile holding a row's curve overrides (the sim builds its own from an edited Table).
    [[nodiscard]] Profile Build(const Row& a_row);

    // Sim / search edit surface. SetCurveParam: param is a | b | floor | zeroAbove | zeroBelow; the override is created as a copy of
    // the shared curve a_shared when the (family, cons) pair has none. SetScalar: drinkHealthLine | touchSpentSec | schoolPeak | schoolFloor | schoolShape | schoolMasteredBump. False on a bad name.
    bool SetCurveParam(Row& a_row, Family a_f, Cons a_c, const Curve& a_shared, std::string_view a_param, float a_v);
    bool SetScalar(Row& a_row, std::string_view a_name, float a_v);
}
