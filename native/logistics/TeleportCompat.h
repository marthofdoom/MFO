#pragma once
// logistics/TeleportCompat.h -- FOLLOWER-TELEPORT MOD COMPATIBILITY for the loot
// leash (ClickUp 86e3ec824, 2026-09-25). The "accepted" route: MFO never hooks or
// fights another DLL. It DETECTS a known follower-teleport mod, reads that mod's
// own INI distances, and never PLANS a loot trip past them (mid-trip: best-effort,
// teleport recognition as the backstop) while the mod's
// teleport condition holds.
//
// Known mod (primary source: the installed LoreRim list, 1.7.0):
//   Automatic Follower Teleporter NG -- SKSE/Plugins/AutomaticFollowerTeleporter.dll
//   + .ini [Teleport]. Acts ONLY while the PLAYER's weapon is drawn: on the draw it
//   teleports followers farther than fDrawDistance (2000); while drawn, every
//   fDrawnInterval (3 s) it teleports followers farther than fDrawnDistance (1500)
//   to fBehindOffset (450) behind the player. Never a follower in combat (MFO's
//   loot never runs then), never while sheathed, never a waiting follower.
//   bNoTeleportOnHorse skips it while the player is mounted;
//   bOnlyAllowTeleportOnCombatStart confines it to combat start (which MFO's
//   player-combat interrupt already ends every excursion for).
// Checked and NOT a teleporter: Simple Follower Framework (DLL strings, INI keys and
// the shipped .psc sources carry no teleport / MoveTo) -- logged as such.
//
// Detection is LAZY (first query, std::call_once) and the result is immutable
// afterwards, so every read is safe from any thread. It runs on the first loot
// scan / excursion tick, i.e. on the logistics worker, after SKSE has loaded every
// plugin DLL (GetModuleHandle is therefore authoritative for "loaded").
#include "PCH.h"

namespace MFO::Logistics::TeleportCompat {

    // One snapshot of the loot leash for a follower, taken once per call site so a
    // weapon draw between two reads cannot split one decision.
    struct LeashView {
        float leash   = 0.0f;   // candidate-selection leash (player -> ref), game units
        float release = 0.0f;   // mid-trip release distance (follower -> player)
        bool  clamped = false;  // a teleport mod's condition holds and capped the leash
        float teleportAt = 0.0f;// that mod's teleport distance (0 when not clamped)
    };

    // Confidence::LeashRadius(a_follower), capped under the detected teleport
    // distance minus a margin WHILE that mod's teleport condition holds. The release
    // distance is the usual x1.15 hysteresis, also capped under the teleport distance
    // while clamped. Logs each clamp ON/OFF transition once.
    LeashView View(RE::Actor* a_follower);

    // True when a follower-teleport mod was detected AND its condition holds now.
    bool ConditionHolds();

    // A teleport that happened anyway (a draw from past fDrawDistance, a door, any
    // other mod's MoveTo): the follower jumped a long way in one tick, faster than
    // any gait, and landed near the player after being far from him. Pure; the
    // caller keeps the previous observation.
    bool LooksTeleported(const RE::NiPoint3& a_prevPos, float a_prevPlayerDist, float a_dtSec,
                         const RE::NiPoint3& a_curPos, float a_curPlayerDist);

    // The detected mod's display name, or "none detected" -- for the logs.
    const char* DetectedName();
}
