// tools/flwr_byte_test -- emulated round trip of the FLWR `mfoEnabled` byte (card I5, the IAUS toggle bits).
// RUN: tools/flwr_byte_test/run.sh   (host g++ or CXX=clang++; exit code = number of failed checks).
// Not part of the CMake/CI build: the DLL never links it.
//
// It compiles native/FlwrEnabledByte.h, the SAME Encode / Decode the FLWR writer and reader in
// native/Serialization.cpp call (nothing is mirrored for the current DLL). The two OLDER DLLs' expressions are
// mirrors, by necessity (their code is gone from the tree): `byte != 0` is the pre-helmet reader, and
// HelmEraDecodeHelm / HelmEraEncode are the helmet-era lines exactly as they stood before card I5
// (git show 12aa000:native/Serialization.cpp, the writer at :572-573 and the reader at :853-855).
//
// What it walks: mfoEnabled 0/1 x helmetOverride 0/1/2 (and the stray 3) x iausCombat 0/1, each saved through a
// one-byte "co-save", then loaded back with every garbage pattern in bit 4 (the reserved logistics bit) and
// bits 5-7, by this DLL and by both older ones; then the older DLL's re-save is loaded by this DLL again.
#include "FlwrEnabledByte.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

using namespace MFO::FlwrEnabledByte;

namespace {
    int g_failed = 0;
    int g_checks = 0;
    void Check(bool ok, const char* what) {
        ++g_checks;
        if (!ok) { ++g_failed; std::printf("  [FAIL] %s\n", what); }
    }
}

int main() {
    std::printf("FLWR mfoEnabled byte: bit0 enabled | bits1-2 helmet | bit3 iausCombat | bit4 reserved | bits5-7 unused\n\n");
    std::printf("state in (en helm iaus) -> byte | this DLL reads (en helm iaus) | pre-helmet DLL en | helmet-era DLL "
                "(en helm) -> its re-save -> this DLL reads\n");

    for (int en = 0; en < 2; ++en)
        for (unsigned helm = 0; helm < 4; ++helm)
            for (int iaus = 0; iaus < 2; ++iaus) {
                const bool          e  = en != 0;
                const bool          i  = iaus != 0;
                const auto          hv = static_cast<std::uint8_t>(helm);
                const std::uint8_t  wantHelm = (e && hv <= 2) ? hv : std::uint8_t{ 0 };
                const std::uint8_t  saved = Encode(e, hv, i);   // the one byte SaveCallback writes

                // Writer properties.
                Check((saved != 0) == e, "byte is 0 exactly when MFO is off for him");
                Check((saved & 0xF0u) == 0, "bits 4-7 are never written (bit 4 = logistics, reserved)");
                Check(i || saved == HelmEraEncode(e, hv), "toggle off writes the helmet-era bytes, bit for bit");
                Check(((saved & kIausCombatBit) != 0) == (e && i), "bit 3 is set only for an enabled follower with the toggle on");

                // This DLL loads its own byte.
                const Fields f = Decode(saved);
                Check(f.mfoEnabled == e, "round trip: mfoEnabled");
                Check(f.helmetOverride == wantHelm, "round trip: helmetOverride (stray 3 and MFO-off read Default)");
                Check(f.iausCombat == (e && i), "round trip: iausCombat (lost only when MFO is off for him)");
                Check(Encode(f.mfoEnabled, f.helmetOverride, f.iausCombat) == saved, "re-save is byte-identical");

                // Garbage in bit 4 and bits 5-7 on an ENABLED byte (bit 0 set): nothing this DLL reads moves.
                // (A disabled follower's byte is 0; garbage there is a different, nonzero byte and is covered by
                // the 256-value sweep below.)
                bool garbageOk = true;
                if (e)
                    for (unsigned hi = 0; hi < 16; ++hi) {
                        const auto   dirty = static_cast<std::uint8_t>(saved | (hi << 4));
                        const Fields g     = Decode(dirty);
                        garbageOk = garbageOk && g.mfoEnabled == f.mfoEnabled &&
                                    g.helmetOverride == f.helmetOverride && g.iausCombat == f.iausCombat;
                        garbageOk = garbageOk && LegacyEnabled(dirty) == e && HelmEraDecodeHelm(dirty) == wantHelm;
                    }
                Check(garbageOk, "bit 4 (and bits 5-7) garbage is ignored on read, by this DLL and the older ones");

                // The older DLLs load this DLL's byte.
                const bool         oldEn   = LegacyEnabled(saved);        // both older readers
                const std::uint8_t oldHelm = HelmEraDecodeHelm(saved);    // helmet-era reader
                Check(oldEn == e, "older DLL: `byte != 0` is still exactly mfoEnabled");
                Check(oldHelm == wantHelm, "older (helmet-era) DLL: reads the same helmet override");

                // The helmet-era DLL saves; this DLL loads that.
                const std::uint8_t resaved = HelmEraEncode(oldEn, oldHelm);
                const Fields       back    = Decode(resaved);
                Check(back.mfoEnabled == e && back.helmetOverride == wantHelm, "through the helmet-era DLL: enabled + helmet kept");
                Check(!back.iausCombat, "through the helmet-era DLL: the toggle is dropped (back on gambits)");
                // The pre-helmet DLL saves 0/1; this DLL loads that.
                const Fields back0 = Decode(static_cast<std::uint8_t>(oldEn ? 1 : 0));
                Check(back0.mfoEnabled == e && back0.helmetOverride == 0 && !back0.iausCombat,
                      "through the pre-helmet DLL: enabled kept, override and toggle dropped");

                std::printf("  %d %u %d -> 0x%02X | %d %u %d | %d | %d %u -> 0x%02X -> %d %u %d\n", en, helm, iaus,
                            static_cast<unsigned>(saved), f.mfoEnabled ? 1 : 0, static_cast<unsigned>(f.helmetOverride),
                            f.iausCombat ? 1 : 0, oldEn ? 1 : 0, oldEn ? 1 : 0, static_cast<unsigned>(oldHelm),
                            static_cast<unsigned>(resaved), back.mfoEnabled ? 1 : 0,
                            static_cast<unsigned>(back.helmetOverride), back.iausCombat ? 1 : 0);
            }

    // Every byte a save could hold (a hand-edited or corrupt one included).
    int iausOn = 0;
    for (unsigned v = 0; v < 256; ++v) {
        const auto   b = static_cast<std::uint8_t>(v);
        const Fields f = Decode(b);
        Check(f.mfoEnabled == LegacyEnabled(b), "all 256: enabled == the older DLL's `byte != 0`");
        Check(f.helmetOverride == HelmEraDecodeHelm(b), "all 256: helmet == the helmet-era read");
        Check(f.iausCombat == ((b & 0x09u) == 0x09u), "all 256: iausCombat == bit 0 && bit 3, nothing else");
        Check(Decode(static_cast<std::uint8_t>(b ^ kIausLogisticsBit)).iausCombat == f.iausCombat,
              "all 256: flipping bit 4 never changes iausCombat");
        if (f.iausCombat) ++iausOn;
    }
    Check(iausOn == 64, "all 256: exactly the 64 bytes with bits 0 and 3 set read the toggle on");
    Check(SelfCheck(), "FlwrEnabledByte::SelfCheck (the static_assert the DLL build carries)");

    // The records that existed before this card: 0, 1, and 1|helm<<1 (1, 3, 5). None reads the toggle on.
    for (const unsigned v : { 0u, 1u, 3u, 5u })
        Check(!Decode(static_cast<std::uint8_t>(v)).iausCombat, "every pre-I5 byte reads iausCombat off (default OFF)");
    Check(!Fields{}.iausCombat, "default-constructed: iausCombat off");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
