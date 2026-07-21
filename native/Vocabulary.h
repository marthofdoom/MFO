#pragma once
#include "PCH.h"

// The gambit vocabulary. DESIGN.md §3.
//
// HAND-WRITTEN for now. ARCHITECTURE.md §1 plans this as codegen from a design
// JSON (gen_vocab_header.py) -- correct once there are dozens of entries, but a
// six-entry table does not need a code generator. The opcode STRINGS are a
// frozen serialization contract (INVARIANTS #10): they land in co-saves, so
// their spelling may never change once shipped. Codegen, when it lands, must
// emit exactly these strings.
//
// M5 FIRST SLICE: cheap conditions only (one actor-value read on a known
// actor -- no world snapshot, so no cost-tier machinery yet), and cast/wait
// actions. No foe/ally-scan conditions, no standing orders, no drink/equip.

namespace MFO::Vocab {

    // Who a condition reads / an action targets.
    enum class Subject : std::uint8_t {
        Self   = 0,
        Player = 1,
        // Ally/Foe selectors arrive with the world snapshot in a later slice.
    };

    // ── conditions (stable opcode strings) ──────────────────────────────────
    inline constexpr const char* kCondAlways        = "cond.always";
    inline constexpr const char* kCondSelfHpBelow   = "cond.self_hp_pct_below";
    inline constexpr const char* kCondSelfMpBelow   = "cond.self_mp_pct_below";
    inline constexpr const char* kCondSelfSpBelow   = "cond.self_sp_pct_below";
    inline constexpr const char* kCondPlayerHpBelow = "cond.player_hp_pct_below";

    // ── actions ─────────────────────────────────────────────────────────────
    inline constexpr const char* kActWait      = "act.wait";
    inline constexpr const char* kActCastSelf  = "act.cast_self";    // param = SpellItem
    inline constexpr const char* kActCastTarget= "act.cast_target";  // param = SpellItem, subject = target

    // A cheap actor-value-percentage read. Reads THE NAMED actor's state
    // (INVARIANTS #15 -- say whose).
    //
    // Returns 1.0 (full) on any failure, so an unreadable actor never triggers
    // a "below X%" condition. Failing toward NOT acting is the safe default:
    // MFO doing nothing is always compatible, MFO acting on garbage is not.
    //
    // THE MAXIMUM IS NOT GetPermanentActorValue. That call is base + permanent
    // and OMITS the temporary modifier -- which is exactly where fortify-health
    // from gear and potions lands. A follower at 100 permanent wearing +50
    // fortify would read 150/100, clamp to 1.0 while untouched, and still read
    // "full" after losing 50 HP; a "HP < 40%" rule would not fire until 40/150
    // = 27% of true maximum. In a heavily-modded order every follower wears
    // that gear, so this would have shipped as "the heal always comes late".
    inline float Pct(RE::Actor* a_actor, RE::ActorValue a_av) {
        if (!a_actor) return 1.0f;
        auto* avo = a_actor->AsActorValueOwner();
        if (!avo) return 1.0f;
        const float mx = avo->GetPermanentActorValue(a_av) +
                         a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av);
        if (mx <= 0.0f) return 1.0f;
        return std::clamp(avo->GetActorValue(a_av) / mx, 0.0f, 1.0f);
    }

    inline float HealthPct(RE::Actor* a)  { return Pct(a, RE::ActorValue::kHealth); }
    inline float MagickaPct(RE::Actor* a) { return Pct(a, RE::ActorValue::kMagicka); }
    inline float StaminaPct(RE::Actor* a) { return Pct(a, RE::ActorValue::kStamina); }

}
