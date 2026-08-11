#pragma once
#include "PCH.h"

// FOLLOWER-PROGRESSION PROBE — throwaway dev build, LOG ONLY.
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §13: the three "sinker" risks get a
// cheap field probe BEFORE any ESL/board/co-save work. This module is that
// probe and nothing else — it must confirm, on marth's real LoreRim/Tuxborn
// setup, that:
//
//   P1  entry-point perks actually fire on an NPC follower via the SPID
//       pattern (TESNPC::AddPerk + Actor::ApplyPerksFromBase — Actor::AddPerk
//       is a verified no-op on NPCs), observable as (a) HasPerk flipping true
//       on the ACTOR and (b) a perk ABILITY's magic effect appearing in the
//       follower's active effects (the one entry type a log can see without a
//       controlled damage test);
//   P2  a SetBaseActorValue skill write survives Requiem's static stats and a
//       player level-up, with the §4.2 reconcile math logged (cur /
//       lastWrittenBase / natural / desired) so a clobber is visible;
//   P3  the GetPerkIndex-guarded reapply at kPostLoadGame is idempotent —
//       neither the base perk array nor the applied ability doubles.
//
// GATED: bProgProbe in Data/SKSE/Plugins/MFO.ini, DEFAULT OFF, INI-only (an
// experiment, never a setting — the bProbeCastStyle precedent). Off = this
// module is never entered and a normal install is byte-for-byte unaffected.
//
// TRIGGER: iProgProbeKey (DIK code, default 0x27 = semicolon, unbound in
// vanilla). First press runs the full probe (pick follower → dump trees →
// P1 apply → P2 baseline write). Every later press is a STATUS read (re-log
// HasPerk / effect count / base-AV reconcile) — press after levelling to
// answer P2. Save + reload exercises P3 automatically at kPostLoadGame.
//
// THREADING: every entry point below runs on the MAIN thread — OnHotkey is
// only ever reached through MainThread::Post (never AddTask, which is a job
// worker on this runtime [[skse-addtask-runs-on-job-worker]]), and OnPostLoad
// is called from the SKSE messaging handler. All state here is main-thread
// only; no locks needed or taken.
//
// NOTHING here is serialized: no co-save record, no ESL, no UI. Base-form
// perk mutations are runtime-only by nature; the probe state (which perk on
// which follower) lives in DLL globals for the session — which is exactly
// what lets P3 measure a save/reload in the same session.

namespace MFO::ProgProbe {

    // First press = full probe; later presses = status re-read. MAIN THREAD
    // ONLY (post it). No-op unless bProgProbe.
    void OnHotkey();

    // P3: the guarded reapply, called at kPostLoadGame/kNewGame (main thread).
    // No-op unless bProgProbe AND the full probe ran earlier this session.
    void OnPostLoad();

}
