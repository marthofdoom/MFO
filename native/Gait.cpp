#include "PCH.h"
#include "Gait.h"
#include "Config.h"
#include "Forms.h"

namespace MFO::Gait {

    void Apply() {
        auto* pkg = Forms::g_travelPackage;
        if (!pkg) return;   // before Forms::Resolve -- kDataLoaded calls again after

        // Clamped at parse (0..3), so the cast cannot manufacture an
        // out-of-range enum (INVARIANTS #38).
        const int gait = Config::g_travelGait.load();
        const auto want = static_cast<RE::PACKAGE_DATA::PreferredSpeed>(gait);

        const auto had = pkg->packData.maxSpeed.get();
        pkg->packData.maxSpeed = want;
        // The byte is INERT without 0x2000 (§0.35, measured). The ESP ships it
        // set since v0.8.3; re-assert rather than trust, because a stale ESP
        // would otherwise silently disable this whole setting -- the exact
        // failure §0.35 documents. set() ORs; no other flag is touched.
        pkg->packData.packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);

        if (had != want) {
            static constexpr const char* kNames[] = { "Walk", "Jog", "Run", "FastWalk" };
            spdlog::info("[gait] travel package preferredSpeed {} -> {} ({})",
                         std::to_underlying(had), gait, kNames[gait]);
        }
    }

}
