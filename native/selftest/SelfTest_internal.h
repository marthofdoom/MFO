#pragma once
// INTERNAL to native/selftest/ (Runner.cpp, Fixtures.cpp, Record.cpp, Tests.cpp). Never include it
// from another subsystem: use selftest/SelfTest.h. Compiled only with MFO_DEV_SELFTEST.
#ifndef MFO_DEV_SELFTEST
#error "selftest/ is compiled only in a dev build (CMake -DMFO_DEV_SELFTEST=ON)"
#endif

#include "PCH.h"
#include "State.h"
#include "selftest/SelfTest.h"

#include <map>

namespace MFO::SelfTest {

    // ── THE TEST REGISTRY (data-driven; Docs/SELFTEST.md "Adding a test") ────────────────────────
    // A test is one TestDef, registered by one Register() call in Tests.cpp's RegisterAll(). The
    // runner owns everything else: picking, fixtures, timeout, logging, persistence.

    enum class Verdict : std::uint8_t {
        Running,   // keep calling step()
        Pass,      // COMPLETE: logged, persisted, never run again
        Fail,      // COMPLETE: logged with the reason, persisted, never run again
        Retry,     // NOT complete: the situation ended before the test could judge (combat ended, the
                   // rule never needed to fire). Logged, fixtures removed, the test stays pending.
    };

    struct Step {
        Verdict     v = Verdict::Running;
        std::string evidence;
        static Step Continue() { return {}; }
        static Step Pass(std::string e) { return { Verdict::Pass, std::move(e) }; }
        static Step Fail(std::string e) { return { Verdict::Fail, std::move(e) }; }
        static Step Retry(std::string e) { return { Verdict::Retry, std::move(e) }; }
    };

    // Per-run scratch a test keeps between steps. Starts zeroed when the test is picked; the
    // precondition call that picks it fills it first (so step() sees what the precondition found).
    struct Scratch {
        double                  n[8]{};      // counters, start values, timestamps (seconds of run time)
        bool                    b[8]{};
        std::vector<RE::FormID> set[4];      // e.g. the heal / offense spells of the follower's rules
        std::string             note;
    };

    // What a test needs present on the follower for its run. The runner adds only what is MISSING
    // and records exactly that in the fixture ledger (Fixtures.cpp).
    struct FixtureRequest {
        struct Item { RE::FormID form = 0; std::int32_t atLeast = 1; };
        std::vector<Item>       items;    // inventory objects: the runner tops the count up to atLeast
        std::vector<RE::FormID> spells;   // spells: taught only if the follower does not know them
    };

    // Everything a test reads. Built by the runner on the WORKER each call (re-found, #2). Pure reads
    // only inside precondition / step (INVARIANTS #23): engine mutation is the runner's job (fixtures).
    struct Ctx {
        RE::Actor*                            follower = nullptr;
        RE::FormID                            id = 0;
        const FollowerState*                  state = nullptr;
        bool                                  inCombat = false;
        float                                 elapsed = 0.0f;   // unpaused run seconds since the drive began
        float                                 timeout = 0.0f;
        std::chrono::steady_clock::time_point startedAt{};      // wall clock of the drive start
        float                                 seenFor = 0.0f;   // unpaused seconds this follower has been
                                                                //   in the party this session
        Scratch*                              s = nullptr;
    };

    struct TestDef {
        const char* id = "";        // FROZEN once shipped in a dev build: it is the 'MFST' key (#10)
        const char* ref = "";       // ClickUp task / backlog id the test automates
        const char* needs = "";     // one line for the pending log: when the test can run
        float       timeoutSec = 120.0f;
        // nullptr = the test can start now. Otherwise the missing condition (static text). Called at
        // most once a second per follower while the test is pending.
        const char* (*precondition)(Ctx&) = nullptr;
        // Optional: what must be present for the run (only the missing part is added).
        void (*fixtures)(Ctx&, FixtureRequest&) = nullptr;
        // Called every 0.5 s of run time once the fixtures are in place.
        Step (*step)(Ctx&) = nullptr;
        // Optional: the verdict at timeout. nullptr = FAIL "timeout after N s".
        Step (*onTimeout)(Ctx&) = nullptr;
    };

    void                         Register(const TestDef& a_def);
    const std::vector<TestDef>&  Registry();
    void                         RegisterAll();   // Tests.cpp: one Register() per test
    // Tests.cpp: the tests' own passive sinks (bound_bow.fires' TESHitEvent sink), posted to the main thread;
    // called every tick, a no-op once live. And the tests' session caches, cleared with ClearAll.
    void                         RegisterSinks();
    void                         ClearTestState();

    // ── shared read helpers (Runner.cpp) ─────────────────────────────────────────────────────────
    // Count of a_form in a_actor's own inventory (any thread the codebase already walks inventories
    // on: the worker, as Logistics::CountPotions does, or main). 0 for a null actor / form.
    std::int32_t CountItem(RE::Actor* a_actor, RE::FormID a_form);
    // A Skyrim.esm form by its LOCAL id (load-order agnostic). 0 when the master does not hold it.
    RE::FormID   VanillaForm(RE::FormID a_localID);
    std::string  FormName(RE::FormID a_form);

    // ── the persisted state (Record.cpp writes / reads it) ───────────────────────────────────────
    // Guarded by g_mx. g_mx is a LEAF lock: no engine call and no other lock is ever taken under it,
    // so the worker, a main-thread fixture closure and the save callback can never deadlock on it.
    struct Result {
        bool        pass = false;
        RE::FormID  follower = 0;
        std::string evidence;
    };

    enum class FixtureKind : std::uint8_t { Item, Spell };
    struct Fixture {
        std::uint32_t token = 0;      // the run that added it; 0 = loaded from a save (an orphan)
        std::string   testId;
        RE::FormID    follower = 0;
        FixtureKind   kind = FixtureKind::Item;
        RE::FormID    form = 0;
        std::int32_t  added = 0;      // how many the runner ADDED (item) / 1 (spell taught)
        std::int32_t  before = 0;     // the follower's own count before the add (item)
    };

    inline std::mutex                     g_mx;
    inline std::map<std::string, Result>  g_results;       // completed tests, by id
    inline std::vector<Fixture>           g_ledger;        // every fixture still on a follower
    inline std::uint32_t                  g_resetGeneration = 0;
    // Fixture add acks, by run token (Fixtures.cpp writes on main, the runner polls on the worker).
    struct FixtureAck { bool ok = false; std::string note; };
    inline std::unordered_map<std::uint32_t, FixtureAck> g_acks;
    inline bool                           g_orphanCleanupInFlight = false;
    inline bool                           g_afterLoad = true;   // first Tick after a load / revert:
                                                                //   reset INI + pending log

    // Fixtures.cpp. Both POST to the main thread (MainThread::Post) and do the engine work there; the
    // ledger change happens in the SAME main-thread closure as the engine change, so a save (also on
    // the main thread) always sees the two agree.
    void PostFixtureAdd(std::uint32_t a_token, const std::string& a_testId, RE::FormID a_follower,
                        FixtureRequest a_req);
    // Removes every ledger entry of a_token (0 = the orphans a load brought in).
    void PostFixtureCleanup(std::uint32_t a_token, const char* a_why);

}
