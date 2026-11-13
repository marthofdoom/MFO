#pragma once
// iaus/TuningFile.h -- the IAUS tuning TEXT FILES (MFO_Tuning/, marth 2026-11-13): new tuning results reach the game by replacing a file, live, with no rebuild
// and NO in-game access (no MCM entry, no console command, no hotkey, no UI). One file per table, each hot-swapped on its own:
//   Data/SKSE/Plugins/MFO_Tuning/<class>.txt   one class row (melee.txt ... mage-off.txt: the Classes slug, plus the three tree-OFF base cells)
//                                shared.txt   the Tuning globals + the shared curves / weights  (the sim's --set: t.*, w.*, aw.*, c.*)
//                                potions.txt  the worth-the-gold table (WorthTable, `worth.<name>=<v>`)
//                                looting.txt  the loot table (LootTuning, `loot.<name>=<v>`)
// The line syntax is EXACTLY the simulator's (tools/iaus_sim: --curve / --set), so a tuning run's output pastes in unchanged. See the shipped files' headers.
//
// THREADING: Poll() runs on the pump's AddTask worker at the top of Scheduler::Tick, BETWEEN laps. That worker is the ONLY reader of every table it swaps
// (Candidates / IausSource / Generate / Score, the loot plan inside ServiceFollower, Board's GateFor in PublishSnapshot: all serial in the one tick body, and an
// IausSource never outlives its lap), so the swap is an in-place edit with no lock. MAP.md "iaus/TuningFile" has the evidence.
namespace MFO::Iaus::TuningFile {

    // Check the folder (every ~2 s) and rebuild + swap each table whose file changed (a changed file is applied once its stamp has been stable for one more
    // check, so a half-written file is never read). The first call loads everything. Worker only; a no-op between checks.
    void Poll();
}
