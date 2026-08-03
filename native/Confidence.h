#pragma once
#include "PCH.h"
#include "Vocabulary.h"   // HealthPct / StaminaPct / MagickaPct
#include "Config.h"
#include "CombatSense.h"  // FoeCount -- combat-group foe tally (#23)

// THE CONFIDENCE LEASH -- a CORE TENET (DESIGN.md). marth, 2026-07-29.
//
// A follower's willingness to operate AWAY FROM THE PLAYER is not a fixed
// follow distance -- it is a live readout of how confident that follower is to
// survive on their own right now. Bold in an easy fight (the leash grows, they
// push ahead, engage the enemy nearest THEM, range out to loot); cautious in a
// hard one (the leash shrinks, they fall back and fight at the player's side,
// grabbing only nearby loot). "Nearest enemy" and "loot distance" are still
// measured from the FOLLOWER; the invisible string is how far from the PLAYER
// the follower is willing to be, and it tightens in danger and loosens in
// safety. The player never sees a number -- they just feel it.
//
// ONE PRIMITIVE, TWO CONSUMERS: the loot leash (Logistics -- whether to travel
// to a ref and how far) now, and combat target distance (Targeting/Evaluator --
// how far to chase the nearest foe) next. Neither reinvents "how brave is this
// follower"; both read Of()/LeashRadius().
namespace MFO::Confidence {

    // [0,1]: how safe a_follower feels operating alone. v1 is VITALITY-driven
    // (mostly health, plus the resources they would actually fight with),
    // knocked down while in a live fight. The area-difficulty / foe-strength
    // scan that makes a hard AREA register before blows even land is the next
    // refinement (DESIGN); v1 already captures the spirit, because a follower
    // who just survived a hard fight reads hurt -> low -> stays close, and one
    // strolling an easy zone reads healthy -> high -> ranges out.
    inline float Of(RE::Actor* a_follower) {
        if (!a_follower) return 0.0f;
        const float hp = Vocab::HealthPct(a_follower);
        const float st = Vocab::StaminaPct(a_follower);
        const float mg = Vocab::MagickaPct(a_follower);
        float c = 0.6f * hp + 0.2f * st + 0.2f * mg;
        // A live fight is less safe to wander off in than a lull -- and being
        // MOBBED is worse than a duel (#23). Each foe past the first tightens
        // the multiplier; the curve is pinned so a TWO-foe fight lands ~0.63
        // (the old flat 0.65, so existing leash/chase tuning is unchanged at the
        // common case), a lone foe reads braver, and a FIVE-foe pack drops to
        // ~0.225 -- strictly UNDER the 0.25 auto-retreat floor the Scheduler
        // watches, so a full-health follower still falls back when genuinely
        // swarmed (at 0.13 it landed exactly ON 0.25 and never crossed the strict
        // < gate). Clamped so a mob can't zero it out.
        if (a_follower->IsInCombat()) {
            const int   foes = std::max(1, CombatSense::FoeCount(a_follower));
            const float mult = std::clamp(0.90f - 0.135f * static_cast<float>(foes),
                                          0.15f, 0.90f);
            c *= mult;
        }
        return std::clamp(c, 0.0f, 1.0f);
    }

    // The confidence-scaled leash from the player, in game units: lerp between
    // the scared floor and the confident ceiling (Config, MCM-tunable).
    inline float LeashRadius(RE::Actor* a_follower) {
        const float lo = Config::g_leashMin.load();
        const float hi = Config::g_leashMax.load();
        return lo + Of(a_follower) * std::max(0.0f, hi - lo);
    }

    // THE SECOND CONSUMER this header always promised (#22): the confidence-
    // scaled COMBAT chase radius, in game units, measured from the FOLLOWER. How
    // far out he will pick a foe to engage. Hurt/mobbed -> low -> he fights only
    // what is on top of him and holds near the player; healthy/safe -> high ->
    // he ranges across the field. Gates PickFoe so "attack the weakest foe"
    // (distance-blind) can't send him charging THROUGH a pack to a distant
    // target -- Erik's 20-Falmer suicide charge. The floor stays well above
    // melee reach so he always defends himself against an adjacent foe.
    inline float ChaseRadius(RE::Actor* a_follower) {
        const float lo = Config::g_chaseMin.load();
        const float hi = Config::g_chaseMax.load();
        return lo + Of(a_follower) * std::max(0.0f, hi - lo);
    }
}
