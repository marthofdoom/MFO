#pragma once
#include "PCH.h"

// THE ONE PLACE THAT SAYS WHICH GAME VERSIONS MFO'S VERIFIED PATHS OPEN ON
// (feat/mfo-1.5.97-pass round 2, Fable SEV-3 on 87cabc1, 2026-09-15).
//
// Pinned CommonLibSSE-NG 3.7.0 classifies the runtime from the exe's SECOND
// version field alone (REL/Relocation.h `load_version`): 4 -> VR, 6 -> AE,
// anything else -> SE. So `REL::Module::IsSE()` is the DEFAULT bucket: it is
// true on every 1.5.x (1.5.3 .. 1.5.97, all of which have address libraries)
// and on any future major.minor that is not 1.6 / 1.4 -- a 1.7.x binary reads
// as SE too (moot only while no format-1 `version-1.7.x.bin` exists, because
// `IDDatabase::load` then dies fatally at plugin init). Every 1.5 value in
// Docs/ADDRESS-TABLE-2026-09-15.md was confirmed against 1.5.97 and ONLY
// 1.5.97, so the bucket is not the gate (CLAUDE.md rule 11: per-runtime paths,
// each licensed by its own disassembly). Board.cpp's input trampoline already
// gates on the exact pair (`isAE1170` / `isSE597`, Board.cpp:1594); this header
// is that predicate made shareable, so the five cast gates, Packages'
// ForceRefToNativeAvailable() and plugin.cpp's `[runtime]` line cannot drift
// from one another.
//
// THE AE SIDE IS THE PRE-EXISTING `IsAE()` BUCKET, ON PURPOSE. The cast gates
// were `IsAE()` since v1.0.48 and shipped on every 1.6.x that way; narrowing
// AE to exactly 1.6.1170 is outside the 1.5.97 pass's brief (coordinator call,
// round 2) and is recorded in MAP.md's RUNTIME GATES entry as the open
// question it is. `IsVerified1_6_1170()` exists so the `[runtime]` line can say
// whether the AE build in use is the one every offset was measured on.
namespace MFO::Runtime {

    inline bool IsVerified1_6_1170() {
        const auto v = REL::Module::get().version();
        return v.major() == 1 && v.minor() == 6 && v.patch() == 1170;
    }

    inline bool IsVerified1_5_97() {
        const auto v = REL::Module::get().version();
        return v.major() == 1 && v.minor() == 5 && v.patch() == 97;
    }

    // "Every engine value the cast-control paths and the native ForceRefTo
    // reach has been verified on this runtime": the AE bucket (pre-existing)
    // or exactly 1.5.97. False on VR, on any other 1.5.x, and on anything the
    // library would otherwise file under SE.
    inline bool CastPathsVerified() {
        return REL::Module::IsAE() || IsVerified1_5_97();
    }

    // Why CastPathsVerified() is false, for the `[runtime]` line and for any
    // decline reason that wants to say more than "gated".
    inline const char* GateReason() {
        if (CastPathsVerified())    return "verified";
        if (REL::Module::IsVR())    return "VR";
        if (REL::Module::IsSE())    return "unverified 1.5.x";
        return "unverified runtime";
    }
}
