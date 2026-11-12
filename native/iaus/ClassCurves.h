#pragma once
// iaus/ClassCurves.h -- the PER-CLASS curve table (ruling Q-class-curves, marth 11-12: "each class should already have its own
// curves"; the edge numbers are found by the sim, tools/iaus_sim). PURE: no engine header, no Classes.h (the harness and the sim
// compile this file; SpellBook.cpp asserts kRows == Classes::kClassCount).
//
// A class row holds (a) CURVE overrides: a (Family, Cons) pair scored with this curve instead of the shared Tuning::curve[cons]
// (read by Score.cpp through Profile::curves), and (b) the hand-picked EDGE constants that decide a class-relevant edge case
// (Scalars). The shipped table is every class row PRESENT with NO override and the shared constants, so every score is
// bit-identical to the uncustomised catalogue until the sim batch fills a row in. The tree-OFF base classes
// (Classes::T1ForOverride: Melee / Ranged / Mage) are rows 0..2, the same rows the T1 classes use.
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

    inline constexpr std::size_t kRows = 35;   // Classes::kClassCount (SpellBook.cpp asserts it); the row index IS the Classes::Id ordinal

    // The class-relevant edge constants. Defaults are the shared constants they replace.
    struct Scalars {
        float drinkHealthLine = kDrinkHealthLine;   // Generate.cpp: below it (health potion in stock) the Knight guard line is off, drink first
        float touchSpentSec   = 3.0f;               // Candidates.cpp: seconds after a touch cast that he is "backing out" (== Reposition::kTouchSpentSec)
    };

    struct Row {
        std::vector<Profile::CurveOverride> curves;
        Scalars scalars;
    };
    using Table = std::array<Row, kRows>;

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
    // the shared curve a_shared when the (family, cons) pair has none. SetScalar: drinkHealthLine | touchSpentSec. False on a bad name.
    bool SetCurveParam(Row& a_row, Family a_f, Cons a_c, const Curve& a_shared, std::string_view a_param, float a_v);
    bool SetScalar(Row& a_row, std::string_view a_name, float a_v);
}
