#pragma once
#include "classes/Classes.h"
#include "Vocabulary.h"

// classes/ INTERNAL state and cross-file helpers. Only the .cpp files of native/classes/ include this.
// (Public surface = classes/Classes.h.)

namespace MFO::Classes {

    // The compiled table, indexed by static_cast<size_t>(Id). Defined constexpr in ClassTable.cpp, which
    // also static_asserts its structural rules at COMPILE time (id == index, T1 roots, unlock pools).
    extern const std::array<ClassDef, kClassCount> kTable;

    namespace detail {
        // "_MFOClassesManifest" keyword-suffix scan (Presence.cpp). MAIN THREAD, once, at kDataLoaded.
        // Returns true when the class ESL's manifest was found; logs the one presence line.
        bool DetectManifest();
        // Load-time table self-check (Classes.cpp): logs counts per tier and verifies every parent
        // resolves. Returns the number of defects found (0 = clean); every defect is logged loudly.
        int SelfCheck();
        // The plugin file the manifest came from ("MFO_Classes.esl"), empty before OnDataLoaded / when absent (Presence.cpp).
        // Card C6 resolves its carrier forms by local id in this file.
        std::string_view AddonFileName();
        // Every follower with a stored class record (State.cpp; a copy taken under g_mx). Card C6 re-applies the carriers to all of
        // them after a load.
        std::vector<RE::FormID> StoredFollowerIds();
    }
}
