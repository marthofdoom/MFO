#pragma once
#include "PCH.h"

// THE DEV-BUILD SELF-TEST GAMBIT (ClickUp 1dfcuabcqr7, marth 2026-11-09). Docs/SELFTEST.md.
//
// "for dev builds just have a gambit that runs the tests and once a test is complete its not run by
// that gambit so on dev builds i just leave the gambit on and it tests each test when its possible"
//
// The ONLY header another subsystem may include from native/selftest/. Everything is compiled in
// ONLY when CMake's MFO_DEV_SELFTEST option is ON (it defines MFO_DEV_SELFTEST). In a release build
// the selftest .cpp files are not compiled at all and every entry below is an empty inline, so a
// release DLL carries no opcode, no string and no state of this mechanism.
//
// Wiring (each is one call / one entry, all named in MAP.md "selftest/"):
//   Scheduler::Tick           -> SelfTest::Tick()        (the AddTask worker, every pump tick)
//   Serialization SaveCallback -> SelfTest::CoSave()      (writes 'MFST', dev builds only)
//   Serialization LoadCallback -> SelfTest::CoLoad()      (dev builds only; a release build skips the
//                                                          record as an unknown type)
//   ResetAllState / LoadCallback top -> SelfTest::ClearAll()

namespace MFO::SelfTest {

#ifdef MFO_DEV_SELFTEST

    // Worker (Scheduler::Tick, after Taunt::Tick, before the empty-party return): advances every
    // follower's running test, starts the next pending test whose preconditions hold, posts fixture
    // adds / removals to the main thread, logs the pending list after a load and after each completion.
    void Tick();

    // SaveCallback (main thread, pump paused): the 'MFST' record (completed results + fixture ledger).
    void CoSave(SKSE::SerializationInterface* a_intfc);
    // LoadCallback: read 'MFST' (a_version already checked <= kSelfTestVersion by the caller). Loaded
    // fixture ledger entries are ORPHANS (no run owns them after a load): the first Tick removes them.
    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);

    // ResetAllState (after StopPump + MainThread::Clear) and the top of LoadCallback: forget every run,
    // result and ledger entry. NO engine call: the world is being replaced, and the save being loaded
    // carries its own ledger.
    void ClearAll();

    // SaveCallback (main thread, pump paused), FLWR writer: rows a RUNNING self-test inserted into a follower's tables are skipped
    // so a save made mid-run never carries them (Docs/SELFTEST.md "Mid-run saves"). Leaf-locked reads.
    bool HasRunRows(RE::FormID a_follower);
    bool IsRunRow(RE::FormID a_follower, std::uint32_t a_uid);

#else

    inline void Tick() {}
    inline void CoSave(SKSE::SerializationInterface*) {}
    inline void ClearAll() {}

#endif

}
