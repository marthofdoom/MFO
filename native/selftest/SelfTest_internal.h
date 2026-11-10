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
        Info,      // COMPLETE: a measurement with no pass/fail (the ally-buff strength). Logged INFO, persisted, never run again.
    };

    struct Step {
        Verdict     v = Verdict::Running;
        std::string evidence;
        static Step Continue() { return {}; }
        static Step Pass(std::string e) { return { Verdict::Pass, std::move(e) }; }
        static Step Fail(std::string e) { return { Verdict::Fail, std::move(e) }; }
        static Step Retry(std::string e) { return { Verdict::Retry, std::move(e) }; }
        static Step Info(std::string e) { return { Verdict::Info, std::move(e) }; }
    };

    // Per-run scratch a test keeps between steps. Starts zeroed when the test is picked; the
    // precondition call that picks it fills it first (so step() sees what the precondition found).
    struct Scratch {
        double                  n[8]{};      // counters, start values, timestamps (seconds of run time)
        bool                    b[8]{};
        std::vector<RE::FormID> set[4];      // e.g. the heal / offense spells of the follower's rules
        std::string             note;
    };

    // ── SELF-SETUP (Docs/SELFTEST.md "Self-driving"): everything a run changes is recorded and put back ──────────
    // A gambit row the runner inserts at the FRONT of one of the follower's tables for the run (highest priority, so the
    // test's own situation wins; the player's rows keep their order below it). Removed by uid when the run ends.
    struct TempRow {
        Table         table = Table::Combat;
        std::string   cond;                // Vocab::kCond*
        std::string   act;                 // Vocab::kAct*
        float         condParam = 0.0f;
        RE::FormID    form = 0;            // the spell / item the action names
        std::uint8_t  subject = 0;         // Vocab::Subject ordinal
    };
    // TEMPORARY CLASS STATE: snapshot = copy the follower's whole MCLS state before the run and restore it exactly at the end.
    // A test applies a class itself with ApplyClass() (Setup.cpp) any time after the snapshot.
    struct ClassSpec { bool snapshot = false; };
    // ONE weak hostile from Skyrim.esm (a list-agnostic base form) placed at a distance from the follower, set on him, deleted
    // at the end (and on any abort). hurtPct > 0 also damages the follower's Health by that percent of his max for the run.
    struct SpawnSpec {
        bool        on = false;
        RE::FormID  baseLocal = 0x000E4010;   // Skyrim.esm NPC_ EncMudcrabMedium (verified in the master, 2026-11-10)
        float       hurtPct = 0.0f;
        int         respawns = 3;             // re-spawned when the hostile dies while the test still runs
    };
    // Log-only probe flags switched on IN MEMORY for the run (never the INI file) and put back.
    enum ProbeBit : unsigned { kProbeAim = 1u << 0 };

    // What a test needs present on the follower for its run. The runner adds only what is MISSING
    // and records exactly that in the fixture ledger (Fixtures.cpp).
    struct FixtureRequest {
        struct Item { RE::FormID form = 0; std::int32_t atLeast = 1; };
        std::vector<Item>       items;    // inventory objects: the runner tops the count up to atLeast
        std::vector<RE::FormID> spells;   // spells: taught only if the follower does not know them
        std::vector<TempRow>    rows;     // temporary gambit rows (inserted on the worker, removed by uid)
        ClassSpec               cls;
        SpawnSpec               spawn;
        unsigned                probes = 0;
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
        RE::Actor*                            hostile = nullptr;   // the run's spawned hostile while it lives (re-found each call)
        int                                   respawnsLeft = 0;
        bool                                  spawnRun = false;
        bool                                  everInCombat = false;
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

    // A test a HUMAN must judge (a look, a feel): listed in the pending log and counted in ALL DONE as MANUAL, never run.
    struct ManualItem {
        const char* id;        // frozen
        const char* ref;       // ClickUp id / backlog id
        const char* why;       // why the DLL cannot judge it
        const char* look;      // the exact thing to look at / the pass condition
    };
    const std::vector<ManualItem>& ManualItems();   // Tests.cpp

    void                         Register(const TestDef& a_def);
    const std::vector<TestDef>&  Registry();
    void                         RegisterAll();   // Tests.cpp: one Register() per test
    void                         RegisterDriven();   // TestsDrive.cpp: the self-driving tests (called from RegisterAll, in order)
    // Tests.cpp: the tests' own passive sinks (bound_bow.fires' TESHitEvent sink), posted to the main thread;
    // called every tick, a no-op once live. And the tests' session caches, cleared with ClearAll.
    void                         RegisterSinks();
    void                         ClearTestState();
    void                         ClearDrivenState();   // TestsDrive.cpp: the driven tests' session caches (called by ClearTestState)

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
        bool        pass = false;     // Pass; Info is pass == true && info == true
        RE::FormID  follower = 0;
        std::string evidence;
        bool        info = false;
    };

    enum class FixtureKind : std::uint8_t { Item, Spell, Spawn, Hurt };
    struct Fixture {
        std::uint32_t token = 0;      // the run that added it; 0 = loaded from a save (an orphan)
        std::string   testId;
        RE::FormID    follower = 0;
        FixtureKind   kind = FixtureKind::Item;
        RE::FormID    form = 0;
        std::int32_t  added = 0;      // how many the runner ADDED (item) / 1 (spell taught) / 1 (spawn) / damage x10 (hurt)
        std::int32_t  before = 0;     // the follower's own count before the add (item)
        // Spawn: `form` is the spawned REF's FormID (a runtime 0xFF id: kept RAW in 'MFST', never ResolveFormID'd).
    };
    // A class-state restore point (MCLS snapshot) for a run, in the co-save so a mid-run save is repaired on the next load.
    struct ClassSnap {
        std::uint32_t token = 0;      // 0 = loaded from a save (an orphan)
        RE::FormID    follower = 0;
        bool          hadRecord = false;
        std::uint8_t  equipped = 0xFF;
        std::uint32_t xp[35]{};
        std::uint8_t  level[35]{};
        std::uint64_t mastered = 0, unlocked = 0;
        std::uint16_t thief = 0, masterThief = 0;
    };

    inline std::mutex                     g_mx;
    inline std::map<std::string, Result>  g_results;       // completed tests, by id
    inline std::vector<Fixture>           g_ledger;        // every fixture still on a follower
    inline std::uint32_t                  g_resetGeneration = 0;
    // Fixture add acks, by run token (Fixtures.cpp writes on main, the runner polls on the worker).
    struct FixtureAck { bool ok = false; std::string note; bool refuse = false; };   // refuse: a SAFETY refusal (RETRY, info level)
    inline std::vector<ClassSnap>         g_classSnaps;    // g_mx
    inline std::unordered_map<std::uint32_t, RE::FormID> g_spawnRef;   // run token -> the live spawned ref (g_mx)
    inline std::atomic<bool>              g_classOrphans{ false };
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
    // Main thread, from the closure that spawned the first one: another hostile for the same run (the first died).
    void PostRespawn(std::uint32_t a_token, const std::string& a_testId, RE::FormID a_follower, const SpawnSpec& a_spec);

    // ── Setup.cpp: rows / class state / probes / the safety check (WORKER unless noted) ─────────────────
    struct RunSetup {
        std::vector<std::uint32_t> rowUids;
        bool                       haveSnap = false;
        ClassSnap                  snap;
        unsigned                   probes = 0;
        bool                       probePrev[8]{};
    };
    // Insert the rows (front of their tables), snapshot the class state, switch the probes on. Logs every step at info.
    void SetupApply(RE::FormID a_follower, FollowerState& a_st, std::uint32_t a_token, const FixtureRequest& a_req, RunSetup& a_out);
    // Put all of it back; every failure to restore logs at ERROR. a_st may be null (the follower's record is gone: rows moot).
    void SetupTeardown(RE::FormID a_follower, FollowerState* a_st, std::uint32_t a_token, RunSetup& a_setup, const char* a_why);
    // Restore every class snapshot a load brought in (token 0). WORKER (the writers are mutex-guarded).
    void RestoreOrphanClassSnaps();
    // Apply a class to the follower via the C5c writers (SetUnlocked / SetMastered / SetProgress / SetEquipped), EXACT level.
    // false + a_why when refused. Call only after the run's ClassSpec snapshot. WORKER.
    bool ApplyClass(RE::FormID a_follower, const char* a_slug, std::uint8_t a_level, bool a_mastered, std::string& a_why);
    // true = the situation is safe for a spawned hostile; else a short reason. MAIN THREAD.
    const char* SpawnUnsafeReason(RE::Actor* a_follower);
    // The run-row registry the FLWR writer consults (SelfTest.h HasRunRows / IsRunRow).
    inline std::unordered_map<RE::FormID, std::vector<std::uint32_t>> g_runRows;   // g_mx

}
