#pragma once
// logistics/Pricing.h -- the follower's BARTER PRICE MODEL (town phase 2 round R2, ClickUp 86e3tambj).
// Design: _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md round R2 / item (d) and "R0 RESULTS" R0-1, R0-2.
// marth Q6: the follower's own Speech and perks drive the prices, exactly as the player's do.
//
// THE FORMULA (proven from the engine, both runtimes; every constant is a GMST read at runtime):
//   speech = clamp(Speech AV, 0, 100)
//   buy    = max(fBarterMax - (fBarterMax - fBarterMin) * speech * 0.01, fBarterBuyMin)
//   sell   = min(1 / buy, fBarterSellMax)
//   buy    = EntryPoint 8  kModBuyPrices (owner = follower, Target = the vendor), SEEDED with buy
//   sell   = EntryPoint 60 kModSellPrices (same arguments), SEEDED with sell
//   buy    = max(fMinBuyMult, buy, 1.0)        sell = min(fMaxSellMult, sell, 1.0)
// A unit price is base value x multiplier, rounded with std::lround (half away from zero), PER UNIT
// (the engine rounds in the barter swf, which is not a native fact; this is MFO's one documented choice).
//
// THREADING: Resolve is callable from any thread (the worker tick, the Papyrus VM thread). The perk
// step needs the MAIN thread and a LOADED follower (TradePerks::EntryPointValue), so Resolve never
// evaluates perks itself: it reads the session cache and, when the entry is missing or stale and the
// follower is loaded, Posts ONE refresh to the main thread. No cached perk value -> the Speech-only
// factor, and the line says so ("source=speech-only"); it is never silent. Session-only: nothing here is
// saved and nothing touches the co-save. Clear() runs from TradeBridge::ClearTransientState.
#include "PCH.h"

#include <cstdint>
#include <string>

namespace MFO::Logistics::Pricing {

    struct Quote {
        float buy  = 1.0f;          // final buy multiplier (>= 1.0)
        float sell = 1.0f;          // final sell multiplier (<= 1.0)
        float speech = 0.0f;        // the clamped Speech the factors were built from
        float buySeed  = 1.0f;      // the Speech factor (the entry-point seed)
        float sellSeed = 1.0f;
        float buyPerk  = 1.0f;      // after entry point 8 / 60, before the final clamp
        float sellPerk = 1.0f;
        bool  perksEvaluated = false;
        bool  refreshPending = false; // no cached value yet AND a main-thread refresh is queued/pending: the caller may defer one cycle
        bool  modelOn = true;       // false: bSpeechPricing off or a GMST unreadable -> multipliers are 1.0
        std::string source;         // "cache" / "cache-stale" / "speech-only" / "off" / "gmst-unreadable"
        std::string note;           // why perks were not evaluated, or which clamps applied
    };

    // The follower's quote for a vendor. a_vendorKey = the vendor actor's FormID, or the shop faction's when
    // no vendor actor is loaded. Any thread. Posts a main-thread refresh when it is needed and possible.
    Quote Resolve(RE::ActorHandle a_follower, RE::ActorHandle a_vendor, RE::FormID a_vendorKey);

    // base value x multiplier -> unit price. std::lround, per unit. a_base <= 0 stays 0.
    std::int32_t Apply(std::int32_t a_base, float a_mult);

    // One [econ] line body: Speech, factors, perk-adjusted values, clamps, rounding.
    std::string Describe(const Quote& a_q);

    // Session reset (load / revert).
    void Clear();
}
