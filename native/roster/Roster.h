#pragma once
#include "PCH.h"

// roster/Roster.h -- the roster subsystem's ONE public header (ClickUp 86e3eewaf,
// marth 2026-09-24: "Remove from roster"). Mandatory quest followers pile up on the
// Followers tab because a dismissed follower keeps his g_followers record forever
// (State.h, "dismissed followers stay"). Removal deletes everything MFO holds for
// one FormID and undoes what MFO did to the actor, so the NPC is as it was before
// MFO. A later recruit starts from scratch: fresh default gambits, a fresh HMS
// capture with a new retro flag (the repair path for a bad HMS block).
//
// Offered only for a follower who is NOT currently following: one still in the
// party would be re-adopted on the next Refresh with default data. The per-follower
// MFO switch (mfoEnabled, T#78) is the "keep them, leave them alone" road.
namespace MFO::Roster {

    // WORKER ONLY: called from Board::ApplyEdits (the serial task worker that owns
    // g_followers, #4). Refuses (one [roster] line, nothing touched) when there is no
    // record, the actor does not resolve, he is currently following, or the main-
    // thread pump is not installed. Otherwise runs the dismissal release road
    // (Followers::ReleaseHeldState) right here and posts the main-thread phase:
    // pump paused -> re-check -> progression undo -> erase every record.
    void RequestRemove(RE::FormID a_follower);
}
