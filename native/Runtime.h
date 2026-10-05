#pragma once
#include "PCH.h"
#include "VerifiedAddresses.h"

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
// gates on the exact pair (`isAE1170` / `isSE597`, Board.cpp:1729); this header
// is that predicate made shareable, so the five cast gates, Packages'
// ForceRefToNativeAvailable() and plugin.cpp's `[runtime]` line cannot drift
// from one another.
//
// G1, THE EXACT-VERSION GATE SWEEP (2026-10-04, before 1.7.104 / fork F2).
// The AE side used to be the `IsAE()` BUCKET (every 1.6.x, and every 1.7.x once
// F2 files minor 7 as AE), and ~18 seats refused only VR, so "not VR" read as
// "supported". Every runtime-dependent gate in MFO now asks the predicates
// below, which are EXACT: 1.6.1170.0 or 1.5.97.0, all four version fields, via
// the fork's REL::Module::IsExactly (Relocation.h, mit-3.7 F1) and
// SKSE::RUNTIME_SSE_1_6_1170 / RUNTIME_SSE_1_5_97 (SKSE/Version.h). That is the
// same match the F1 self-check uses to pick its table, so `Known()` is true on
// exactly the builds VerifiedAddresses.h covers. Anything else (VR, any other
// 1.5.x / 1.6.x, and 1.7.104 until F2 adds its own verified arm) is REFUSED:
// the seat or path stays off and says so. Never fall through to the 1.6 path.
// REL::Module::IsAE()/IsSE()/IsVR() survive in MFO only as log labels.
// (Board.cpp:1729's own pair ignores the build field; its seat is also behind
// SeatVerified, whose table match is 4-field exact, so the effect is the same.)
//
// F2b, 1.7.104.0 (Steam), 2026-10-05. The fork (main 57be9d67, registry
// 5ae02e4f) files 1.7.x with AE and reads its ids from the MIT id table
// (revision 3) BUILT INTO this DLL instead of an Address Library: players
// install nothing extra. The two engine classes whose layout changes on
// 1.7.104 that MFO depends on (SkyrimVM +0x10, PlayerCharacter +8) are read
// through the fork's exact-build accessors.
// 1.7.104 joins Known() because EVERY site Known() gates was proven on the
// 1.7.104 executable by disassembly (Docs/ENGINE_NOTES.md section 0.49,
// Docs/VERIFIED-ADDRESSES.md): every hooked vtable slot's target is the same function body as on
// 1.6.1170, every called function is the same body, every MFO-owned offset is
// the same value, and every CommonLib layout MFO reads was compared against the
// 1.7.104 constructors and readers. Where a 1.7.104 value differs from 1.6.1170
// the site has its own exact 1.7.104 arm (never the 1.6.1170 value by bucket).
// No 1.7.104 install exists: this is disassembly proof only, like 1.5.97 was.
namespace MFO::Runtime {

    inline bool IsVerified1_6_1170() {
        return REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_6_1170);
    }

    inline bool IsVerified1_5_97() {
        return REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_5_97);
    }

    // F2b: exactly 1.7.104.0 (SKSE::RUNTIME_SSE_1_7_104, fork SKSE/Version.h).
    inline bool IsVerified1_7_104() {
        return REL::Module::IsExactly(SKSE::RUNTIME_SSE_1_7_104);
    }

    // THE gate for every version-dependent offset, slot, id, layout or path:
    // this build was measured on the running executable. False -> refuse.
    inline bool Known() {
        return IsVerified1_6_1170() || IsVerified1_5_97() || IsVerified1_7_104();
    }

    // "Every engine value the cast-control paths and the native ForceRefTo
    // reach has been verified on this runtime": exactly 1.6.1170, 1.5.97 or
    // 1.7.104 (G1; was the IsAE() bucket or exactly 1.5.97; F2b adds 1.7.104).
    inline bool CastPathsVerified() {
        return Known();
    }

    // The exact build's name for log lines. Labels only, never a gate.
    inline const char* BuildLabel() {
        return IsVerified1_6_1170() ? "1.6.1170"
             : IsVerified1_5_97()   ? "1.5.97"
             : IsVerified1_7_104()  ? "1.7.104"
                                    : "unsupported";
    }

    // Why Known() is false, for the `[runtime]` lines and for any decline
    // reason that wants to say more than "gated". Labels only, never a gate.
    inline const char* GateReason() {
        if (Known())                return "verified";
        if (REL::Module::IsVR())    return "VR";
        return "not supported by this build";
    }

    // The startup line (SKSEPluginLoad, right after the version header): which
    // runtime this is and whether this build supports it.
    inline void LogRuntime() {
        const auto ver = REL::Module::get().version().string(".");
        if (Known()) {
            spdlog::info("[runtime] {} supported by this build (exact {}) -- version-dependent seats open, "
                         "each still subject to the self-check",
                         ver, BuildLabel());
            if (IsVerified1_7_104()) {
                // The ids come from the MIT id table built into this DLL, never an Address Library.
                spdlog::info("[runtime] 1.7.104: ids from the MIT id table built into this DLL, "
                             "revision {}",
                             REL::IDDatabase::get().MitTableRevision());
            }
        } else {
            spdlog::error("[runtime] {} not supported by this build -- all version-dependent seats refused "
                          "(supported: 1.6.1170.0, 1.5.97.0, 1.7.104.0; {})",
                          ver, GateReason());
        }
    }

    // ── SEAT SELF-CHECK (mit-3.7 F1, 2026-09-24) ────────────────────────────
    // Every engine address MFO hooks or calls through its own id is a row in
    // VerifiedAddresses.h (generated by tools/verified_addresses/ from OUR
    // disassembly of 1.6.1170.0 and 1.5.97.0, and of 1.7.104.0 through the
    // fork's id table, see Docs/VERIFIED-ADDRESSES.md).
    // REL::SelfCheck::Run compares the Address Library the game actually loaded
    // with those rows once, the first time anything asks. SeatVerified() is the
    // one gate every install site and seat call uses: it is true only for an
    // address a passing row covers. On a build with no table (anything but the
    // two above) nothing is verified, so every gated seat refuses, by name.
    // It never substitutes the expected RVA and never installs anyway
    // (CLAUDE.md principle 7).
    inline const REL::SelfCheck::Result& SelfCheckResult() {
        static const REL::SelfCheck::Result result = REL::SelfCheck::Run(MFO::VerifiedAddresses::kTables);
        return result;
    }

    // The startup lines. `[selfcheck] ... N/N verified` is the field observable.
    inline void LogSelfCheck() {
        const auto& r = SelfCheckResult();
        if (!r.Covered()) {
            spdlog::error("[selfcheck] {}: game {} has NO verified address table (verified: 1.6.1170.0, "
                          "1.5.97.0, 1.7.104.0) -- every self-checked seat is REFUSED",
                          REL::SelfCheck::kLibrary, r.GameVersion().string("."));
            return;
        }
        for (const auto& f : r.Failures()) {
            spdlog::error("[selfcheck] REFUSED {}: {}", f.row->label, f.reason);
        }
        if (r.Failures().empty()) {
            spdlog::info("[selfcheck] {}: game {} {}/{} verified",
                         REL::SelfCheck::kLibrary, r.GameVersion().string("."), r.Passed(), r.Checked());
        } else {
            spdlog::error("[selfcheck] {}: game {} {}/{} verified, {} REFUSED (listed above; those seats will "
                          "not install)",
                          REL::SelfCheck::kLibrary, r.GameVersion().string("."), r.Passed(), r.Checked(),
                          r.Failures().size());
        }
    }

    // THE gate. True when `a_address` (the vtable / function / call site about to
    // be written or called) is covered by a passing row. False logs the refusal
    // with the seat's name; the caller must then not install / not call.
    inline bool SeatVerified(std::uintptr_t a_address, std::string_view a_seat) {
        const auto& r = SelfCheckResult();
        if (r.IsVerifiedAddress(a_address)) return true;
        spdlog::error("[selfcheck] seat {} REFUSED: 0x{:X} (RVA 0x{:X}) is not a verified address for game {}{}",
                      a_seat, a_address, a_address - REL::Module::get().base(), r.GameVersion().string("."),
                      r.Covered() ? "" : " (no table for this build)");
        return false;
    }
}
