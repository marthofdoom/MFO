#pragma once
#include "PCH.h"

// FollowerCatchUp.cpp: a follower the engine left behind on a player world transition is moved to the player.
namespace MFO::FollowerCatchUp {

    // Called once per Followers::Refresh sweep (the serial worker domain, #4; also once on the main thread at
    // kPostLoadGame, where the settle delay makes it a no-op). Reads g_followers and Followers::SecondsSinceTransition.
    void Poll();

    // Revert / load (Followers::ClearTransientState): drop the pre-transition sample so a save load moves nobody.
    void Reset();

}
