// iaus/ClassCurves.cpp -- the per-class curve table (contract in iaus/ClassCurves.h). PURE.
#include "iaus/ClassCurves.h"

#include "iaus/Reposition.h"

namespace MFO::Iaus::ClassCurves {

    static_assert(Scalars{}.touchSpentSec == Reposition::kTouchSpentSec, "Scalars::touchSpentSec is Reposition::kTouchSpentSec");
    static_assert(ClassGate{}.touchSpentSec == Reposition::kTouchSpentSec, "ClassGate::touchSpentSec is Reposition::kTouchSpentSec");

    namespace {
        // PLACEHOLDER STRUCTURE: one Row per class, nothing overridden. The sim batch fills a row here (a CurveOverride for a
        // (family, cons) pair and/or a Scalars value) with the evidence for it; until then the class scores with the shared catalogue.
        Table MakeDefaults() {
            Table t{};
            for (std::size_t i = 0; i < 3; ++i) t[kBaseOffFirst + i] = t[i];   // each tree-OFF base row starts as its T1 row
            return t;
        }
    }

    const Table& Defaults() {
        static const Table t = MakeDefaults();
        return t;
    }

    Profile Build(const Row& a_row) {
        Profile p;
        p.curves = a_row.curves;
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

    void Apply(std::uint8_t a_classId, ClassGate& a_gate) {
        if (a_classId >= kRows) return;   // no class: the gate keeps the shared constants
        Apply(Defaults()[a_classId], a_classId, a_gate);
    }

    bool SetCurveParam(Row& a_row, Family a_f, Cons a_c, const Curve& a_shared, std::string_view a_param, float a_v) {
        if (a_param != "a" && a_param != "b" && a_param != "floor" && a_param != "zeroAbove" && a_param != "zeroBelow") return false;
        Profile::CurveOverride* o = nullptr;
        for (auto& e : a_row.curves) if (e.family == a_f && e.cons == a_c) { o = &e; break; }
        if (!o) { a_row.curves.push_back({ a_f, a_c, a_shared }); o = &a_row.curves.back(); }
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
        else return false;
        return true;
    }
}
