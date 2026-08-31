#pragma once
// Actuation_internal.h -- the Actuation family's SHARED substrate. One TU
// (Actuation.cpp) used to hold all of this; the mechanical module split
// (Actuation.cpp / Actuation_Direct.cpp) moved the cross-module concentration
// numbers (sustain windows, the cadence contract, the randomized stream
// time-cap draw) here as `inline` (ONE shared instance across the two TUs --
// never per-TU copies). Everything else stays file-local in its module. NOT
// a public API: only the two Actuation*.cpp TUs may include this.

#include "PCH.h"
#include "Actuation.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "CasterConsent.h"
#include "Targeting.h"
#include "Logistics.h"
#include "Followers.h"   // #68: g_active -- NearestAlly walks the maintained teammate list
#include "Serialization.h" // #76: FWPN record ids for the force-hold co-save
#include "MainThread.h"   // #76: defer the load-time force-lock release to the main thread
#include <limits>        // #68: std::numeric_limits for NearestAlly's distance seed
#include "Packages.h"    // #35: act.flee reuses the retreat package; hybrid forced cast
#include "Sightline.h"   // LoS gate on the forced cast -- no firebolts into walls
#include "Temperament.h" // flair #1: per-follower timing seed (grace offset)
#include "Confidence.h"  // AUTO cast: ChaseRadius bounds the "nearby enemies" fan-out

#include <optional>      // ForceCast's tri-state return -- not in the PCH
#include <random>        // fix #3/#6: jittered beneficial-recast window (worker-serial RNG)

namespace MFO::Actuation {

        // These three are the per-beat SUSTAIN WINDOW (the duration
        // SustainConcentrationEffect pins on the live AE each ~1 s beat so it never
        // lapses between beats) -- NOT the stream's time cap. The STREAM CAP is a
        // randomized per-stream band (DrawConcCap below); keep each window > the
        // ~1-1.5 s beat gap so the AE bridges beats.
        inline constexpr float kConcHealCap     = 6.0f;   // heal sustain window (per beat)
        inline constexpr float kConcUtilityHold = 4.0f;   // non-self utility/ward sustain window
        inline constexpr float kConcSelfUtilityCap = 15.0f;   // self utility/ward sustain window
        // HEAL stop-at-full: a heal stream ends the moment the recipient is at/near
        // full Health (marth: "heal always to 100%"), with the random cap as backstop.
        // Uses the SAME mark as the re-dispatch condition (Vocab::kHealFull, 99.95%)
        // so the stream stop and the heal re-dispatch agree -- a topped-off target
        // both stops re-triggering AND has its live stream cut.
        inline constexpr float kHealFullPct = Vocab::kHealFull;

        // THE CADENCE CONTRACT (critical -- "heals feel broken" regression).
        // A concentration spell's cost is authored PER SECOND, and the ENGINE
        // channels its magnitude continuously through the sustained real effect
        // (SustainConcentrationEffect). The ~1 s beat is the stream's
        // heartbeat: each beat DEDUCTS one second's cost (CalculateMagickaCost
        // on a concentration spell returns the per-second cost, so the 1 s
        // beat is the authored drain) and RE-ARMS the sustained effect's
        // rolling window. Pacing the beat by fCastCooldown (default 4 s) would
        // under-charge the channel 4x and let the effect lapse between re-arms
        // (the original "heals feel broken" shape). Fire-and-forget spells
        // keep the fCastCooldown beat: their magnitude is per CAST, and a 1 s
        // beat would multiply it. Sticky concentration wards beat at 1 s too,
        // but the already-active guard in Apply{Self,Target}Effect keeps a
        // still-up ward from re-stacking.
        inline constexpr float kConcApplyPeriod = 1.0f;

        // ONE source of truth for the concentration STREAM TIME-CAP (marth: loose,
        // human timing -- each stream lasts a slightly different, RANDOMIZED duration
        // so channels never feel like a fixed constant). Drawn ONCE when a stream
        // starts (stored in the stream state, never serialized -> no save/determinism
        // concern), from a uniform band by nature:
        //   healing / utility / buff -> [8, 15] s
        //   offense / hostile        -> [2, 6] s
        // The cap GUARANTEES every concentration stream ENDS even if the exact gambit
        // condition check is unreliable ("won't stop when the condition is met"); the
        // gambit re-evaluates between bursts, so a full/satisfied target is not
        // re-served. Healing ALSO ends early at ~full recipient HP (kHealFullPct, in
        // the reconciles), with this cap as the backstop. Consumed by BOTH
        // TargetCastReconcile (non-self streams) and SelfCastReconcile (self streams),
        // so the two paths can never drift on the numbers.
        inline float DrawConcCap(CasterConsent::SpellKind a_kind) {
            static std::mt19937 rng{ std::random_device{}() };   // worker-serial, no lock (#4)
            const float lo = (a_kind == CasterConsent::SpellKind::Offense) ? 2.0f : 8.0f;
            const float hi = (a_kind == CasterConsent::SpellKind::Offense) ? 6.0f : 15.0f;
            return std::uniform_real_distribution<float>(lo, hi)(rng);
        }
}
