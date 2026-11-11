#pragma once
// The FLWR `mfoEnabled` u8: its value space, in ONE place (card I5, 2026-11-10).
// STANDARD HEADERS ONLY (no PCH, no RE::), so tools/flwr_byte_test builds these exact functions with a host
// compiler. Serialization.cpp's FLWR writer and reader call Encode / Decode and nothing else touches the byte.
//
//   bit 0      mfoEnabled (T#78, FLWR v5)
//   bits 1-2   helmetOverride, 0 Default / 1 Wear / 2 Never (86e3phy41); 3 is not a value and reads Default
//   bit 3      iausCombat: the per-follower combat IAUS toggle (card I5)
//   bit 4      RESERVED for the logistics IAUS toggle (card 1dfcuabcr0g): written 0, IGNORED on read
//   bits 5-7   unused: written 0, ignored on read (as every reader of this byte always has)
//
// THE RULE EVERY BIT ABOVE BIT 0 FOLLOWS: it is written ONLY while mfoEnabled is true, so the byte is 0 exactly
// when MFO is off for the follower, and a DLL from before any of these bits (it reads `byte != 0`) sees the same
// enabled state. Version notes and costs: Serialization.h, FLWR "still v5" notes. NOT a layout change, NOT a bump.
#include <cstdint>

namespace MFO::FlwrEnabledByte {

    inline constexpr std::uint8_t kEnabledBit       = 0x01;
    inline constexpr std::uint8_t kIausCombatBit    = 0x08;   // bit 3
    inline constexpr std::uint8_t kIausLogisticsBit = 0x10;   // bit 4: reserved, never written, never read

    struct Fields {
        bool         mfoEnabled     = true;
        std::uint8_t helmetOverride = 0;
        bool         iausCombat     = false;
    };

    // The byte the FLWR writer stores. A disabled follower writes 0 and keeps neither the override nor the toggle.
    [[nodiscard]] constexpr std::uint8_t Encode(bool a_mfoEnabled, std::uint8_t a_helmetOverride,
                                                bool a_iausCombat) noexcept {
        const std::uint8_t helm = a_helmetOverride <= 2 ? a_helmetOverride : 0;
        return static_cast<std::uint8_t>(
            a_mfoEnabled ? (1u | (helm << 1) | (a_iausCombat ? 8u : 0u)) : 0u);
    }

    // What the FLWR reader takes from the byte. enabled stays `byte != 0`, exactly what an older DLL reads. The
    // helmet override and the IAUS toggle count only with bit 0 set; a stray helmet value (3) is Default (#11).
    // Bit 4 and bits 5-7 are not looked at.
    [[nodiscard]] constexpr Fields Decode(std::uint8_t a_byte) noexcept {
        Fields f{};
        f.mfoEnabled = (a_byte != 0);
        const std::uint8_t helm = static_cast<std::uint8_t>((a_byte >> 1) & 3u);
        f.helmetOverride = ((a_byte & 1u) != 0 && helm <= 2) ? helm : 0;
        f.iausCombat     = (a_byte & 1u) != 0 && ((a_byte >> 3) & 1u) != 0;
        return f;
    }

    // ── The older readers and writer, kept as constexpr mirrors so the compiler proves the compatibility claims ──
    // Pre-86e3phy41 DLL (FLWR v5 as first shipped): enabled = byte != 0, writes 0/1.
    [[nodiscard]] constexpr bool LegacyEnabled(std::uint8_t a_byte) noexcept { return a_byte != 0; }
    // The helmet-era DLL (86e3phy41 up to card I5), reader and writer exactly as they stood in Serialization.cpp.
    [[nodiscard]] constexpr std::uint8_t HelmEraDecodeHelm(std::uint8_t a_byte) noexcept {
        const std::uint8_t helm = static_cast<std::uint8_t>((a_byte >> 1) & 3u);
        return ((a_byte & 1u) != 0 && helm <= 2) ? helm : 0;
    }
    [[nodiscard]] constexpr std::uint8_t HelmEraEncode(bool a_mfoEnabled, std::uint8_t a_helmetOverride) noexcept {
        const std::uint8_t helm = a_helmetOverride <= 2 ? a_helmetOverride : 0;
        return static_cast<std::uint8_t>(a_mfoEnabled ? (1u | (helm << 1)) : 0u);
    }

    // Exhaustive: every one of the 256 byte values, and every state the writer can be handed.
    [[nodiscard]] constexpr bool SelfCheck() noexcept {
        for (unsigned v = 0; v < 256; ++v) {
            const auto b = static_cast<std::uint8_t>(v);
            const Fields f = Decode(b);
            if (f.mfoEnabled != LegacyEnabled(b)) return false;            // enabled is what every DLL reads
            if (f.helmetOverride != HelmEraDecodeHelm(b)) return false;    // the helmet read did not move
            if (f.helmetOverride > 2) return false;
            if (f.iausCombat && (b & kEnabledBit) == 0) return false;      // never without bit 0
            if ((b & kEnabledBit) != 0) {
                // With bit 0 set the byte stays nonzero whatever else flips: bit 4 and bits 5-7 change nothing.
                for (unsigned hi = 0; hi < 16; ++hi) {
                    const auto b2 = static_cast<std::uint8_t>((b & 0x0Fu) | (hi << 4));
                    const Fields g = Decode(b2);
                    if (g.mfoEnabled != f.mfoEnabled || g.helmetOverride != f.helmetOverride ||
                        g.iausCombat != f.iausCombat) return false;
                }
            }
        }
        for (int en = 0; en < 2; ++en)
            for (unsigned helm = 0; helm < 256; ++helm)
                for (int iaus = 0; iaus < 2; ++iaus) {
                    const auto hv = static_cast<std::uint8_t>(helm);
                    const std::uint8_t b = Encode(en != 0, hv, iaus != 0);
                    if ((b & 0xF0u) != 0) return false;                              // bits 4-7 never written
                    if ((b != 0) != (en != 0)) return false;                         // 0 exactly when MFO is off
                    if (iaus == 0 && b != HelmEraEncode(en != 0, hv)) return false;  // toggle off = the old bytes
                    const Fields f = Decode(b);
                    if (f.mfoEnabled != (en != 0)) return false;
                    if (f.helmetOverride != ((en != 0 && hv <= 2) ? hv : 0)) return false;
                    if (f.iausCombat != (en != 0 && iaus != 0)) return false;
                    if (Encode(f.mfoEnabled, f.helmetOverride, f.iausCombat) != b) return false;   // stable re-write
                    // The helmet-era DLL loading this byte: same enabled, same override; its next save drops bit 3.
                    if (LegacyEnabled(b) != (en != 0)) return false;
                    if (HelmEraDecodeHelm(b) != f.helmetOverride) return false;
                    if (HelmEraEncode(LegacyEnabled(b), HelmEraDecodeHelm(b)) != static_cast<std::uint8_t>(b & 0xF7u)) return false;
                }
        return true;
    }
    static_assert(SelfCheck(), "FLWR mfoEnabled byte: the value space broke an older reader or a round trip");
}
