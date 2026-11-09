// THE SELF-TEST RUNNER (dev builds only; Docs/SELFTEST.md).
//
// THE GAMBIT IS A STANDING MARKER, NOT AN ACTION. A follower runs self-tests while his COMBAT table holds
// an ENABLED rule whose action is act.dev_selftest (board label "DEV: run self-tests"; its WHEN cell is
// not read). The Evaluator SKIPS that rule (Evaluator.cpp), so it never wins a scan and never takes a
// tick from the follower's real gambits, which are exactly what most tests watch.
//
// PER FOLLOWER, ONE TEST AT A TIME, on the AddTask worker (Scheduler::Tick, worker-serial like the rest of
// the tick, #4):
//   pick   -- once a second, the first test in registry order that is not COMPLETE, not running on another
//             follower, not backing off for this follower, and whose precondition() returns nullptr;
//   fixtures -- the missing items / spells it asked for are added on the MAIN thread (Fixtures.cpp); the
//             run waits for that ack (10 s, else ABORT, loud);
//   drive  -- step() every 0.5 s of unpaused run time until it returns a verdict or the timeout fires
//             (onTimeout(), default FAIL "timeout");
//   end    -- PASS / FAIL: logged as `[selftest] PASS|FAIL <id> <evidence>`, kept in g_results (the 'MFST'
//             co-save) and never run again; RETRY / ABORT: logged, stays pending, 30 s backoff on that
//             follower. Every end removes the run's fixtures (only what it added).
// A run whose follower leaves the party, dies, or loses the rule is aborted the same way.
// The game being paused (menus) freezes run time; nothing is judged while paused.
//
// THREADING: g_runs / g_seen / g_pick / g_backoff are WORKER-ONLY (no lock: the worker is serial, the save
// callback pauses the pump, ResetAllState / LoadCallback run with it stopped). The shared state (results,
// ledger, acks) is under the LEAF lock g_mx, never held across an engine call.

#include "selftest/SelfTest_internal.h"
#include "Followers.h"
#include "Vocabulary.h"

#include <fstream>
#include <optional>
#include <unordered_set>

namespace MFO::SelfTest {

    namespace {
        using Clock = std::chrono::steady_clock;

        std::vector<TestDef>& Reg() {
            static std::vector<TestDef> r;
            return r;
        }

        struct Run {
            const TestDef* def = nullptr;
            std::uint32_t  token = 0;
            bool           fixturesReady = false;
            float          fixtureWait = 0.0f;
            float          elapsed = 0.0f;
            float          sinceStep = 1.0e9f;   // step on the first drive tick
            Clock::time_point startedAt{};
            Scratch        s;
            std::string    fixtureNote;
        };

        // WORKER-ONLY (see the header comment).
        bool                                                   g_registered = false;   // per launch
        std::unordered_map<RE::FormID, Run>                    g_runs;
        std::unordered_map<RE::FormID, float>                  g_seen;   // unpaused party seconds
        std::unordered_map<RE::FormID, float>                  g_pick;   // seconds until the next pick
        std::map<std::pair<RE::FormID, std::string>, Clock::time_point> g_backoff;
        std::uint32_t                                          g_nextToken = 1;
        Clock::time_point                                      g_last{};
        float                                                  g_orphanWait = 0.0f;

        constexpr float kPickEverySec   = 1.0f;
        constexpr float kStepEverySec   = 0.5f;
        constexpr float kFixtureWaitSec = 10.0f;
        constexpr auto  kBackoff        = std::chrono::seconds(30);
        constexpr float kOrphanRetrySec = 30.0f;
        constexpr const char* kIniPath  = "Data/SKSE/Plugins/MFO_SelfTest.ini";

        const char* NameOf(RE::Actor* a) {
            const char* n = a ? a->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        bool HasRunnerRule(const FollowerState& a_st) {
            for (const auto& g : a_st.combat())
                if (g.enabled && g.actionOpcode == Vocab::kActDevSelftest) return true;
            return false;
        }

        // `[selftest] pending: ...` (marth 2026-11-09: "the test gambit should also log which tests are
        // still pending"). a_detail = one line per test (after a load), else the one summary line.
        void LogPending(const char* a_why, bool a_detail) {
            std::vector<const TestDef*>   pending;
            std::map<std::string, Result> results;
            {
                std::lock_guard lk(g_mx);
                results = g_results;
            }
            int pass = 0, fail = 0;
            for (const auto& [id, r] : results) (r.pass ? pass : fail)++;
            std::string ids;
            for (const auto& d : Reg()) {
                if (results.contains(d.id)) continue;
                pending.push_back(&d);
                ids += (ids.empty() ? "" : ", ") + std::string(d.id);
            }
            int holders = 0;
            for (const auto& [fid, st] : g_followers)
                if (HasRunnerRule(st)) ++holders;
            spdlog::info("[selftest] pending ({} of {}, {}): {} | complete {}: PASS {}, FAIL {} | {} follower(s) "
                         "hold the 'DEV: run self-tests' rule",
                         pending.size(), Reg().size(), a_why, ids.empty() ? "none" : ids, results.size(), pass,
                         fail, holders);
            if (!a_detail) return;
            for (const auto* d : pending)
                spdlog::info("[selftest]   pending {} ({}): needs {}", d->id, d->ref, d->needs);
            for (const auto& [id, r] : results)
                spdlog::info("[selftest]   done {}: {} on {:08X} -- {}", id, r.pass ? "PASS" : "FAIL", r.follower,
                             r.evidence);
        }

        std::string Trim(std::string a_s) {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            const auto e = a_s.find_last_not_of(" \t\r\n");
            return a_s.substr(b, e - b + 1);
        }

        // RESET (Docs/SELFTEST.md "Re-running tests"): Data/SKSE/Plugins/MFO_SelfTest.ini
        //   iResetGeneration=N   raise N above the value the save holds to clear results ONCE
        //   sResetOnly=id,id     optional: clear only these ids (empty = every result)
        // The applied generation is stored in 'MFST', so the same INI value never clears twice.
        void ApplyResetIni() {
            std::ifstream in(kIniPath, std::ios::binary);
            if (!in) return;
            std::uint32_t gen = 0;
            bool          haveGen = false;
            std::vector<std::string> only;
            std::string line;
            while (std::getline(in, line)) {
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);   // BOM
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = Trim(line.substr(0, eq));
                std::string       val = line.substr(eq + 1);
                if (const auto sc = val.find(';'); sc != std::string::npos) val = val.substr(0, sc);   // trailing comment
                val = Trim(val);
                if (key == "iResetGeneration") {
                    try {
                        gen     = static_cast<std::uint32_t>(std::stoul(val));
                        haveGen = true;
                    } catch (...) {
                        spdlog::error("[selftest] {}: iResetGeneration '{}' is not a number -- ignored", kIniPath, val);
                    }
                } else if (key == "sResetOnly") {
                    std::size_t p = 0;
                    while (p <= val.size()) {
                        const auto c = val.find(',', p);
                        auto       id = Trim(val.substr(p, c == std::string::npos ? std::string::npos : c - p));
                        if (!id.empty()) only.push_back(std::move(id));
                        if (c == std::string::npos) break;
                        p = c + 1;
                    }
                }
            }
            if (!haveGen) return;
            std::lock_guard lk(g_mx);
            if (gen <= g_resetGeneration) return;
            std::size_t cleared = 0;
            if (only.empty()) {
                cleared = g_results.size();
                g_results.clear();
            } else {
                for (const auto& id : only) cleared += g_results.erase(id);
            }
            spdlog::info("[selftest] {}: reset generation {} -> {}: cleared {} result(s) ({})", kIniPath,
                         g_resetGeneration, gen, cleared, only.empty() ? "all" : "sResetOnly");
            g_resetGeneration = gen;
        }

        // One end for every way a run stops. a_complete = PASS / FAIL (persisted); else RETRY / ABORT.
        void Finish(RE::FormID a_id, Run& a_run, const Step& a_step, const char* a_word, bool a_error = false) {
            const auto* d    = a_run.def;
            const bool  done = a_step.v == Verdict::Pass || a_step.v == Verdict::Fail;
            auto*       a    = RE::TESForm::LookupByID<RE::Actor>(a_id);
            const auto  line = std::format("[selftest] {} {} {:08X} '{}' t={:.1f}s: {}{}", a_word, d->id, a_id,
                                           NameOf(a), a_run.elapsed, a_step.evidence,
                                           a_run.fixtureNote.empty() ? std::string{}
                                                                     : " | fixtures: " + a_run.fixtureNote);
            if (a_error) spdlog::error("{}", line);
            else spdlog::info("{}", line);
            {
                std::lock_guard lk(g_mx);
                if (done) g_results[d->id] = { a_step.v == Verdict::Pass, a_id, a_step.evidence };
                g_acks.erase(a_run.token);
            }
            PostFixtureCleanup(a_run.token, d->id);
            if (done) LogPending(std::format("after {}", d->id).c_str(), false);
            else g_backoff[{ a_id, d->id }] = Clock::now() + kBackoff;
        }

        void ServiceFollower(RE::Actor* a, RE::FormID a_id, const FollowerState& a_st, float a_dt, bool a_blockStarts) {
            const bool inCombat = a->IsInCombat();
            auto       it       = g_runs.find(a_id);
            if (it == g_runs.end()) {
                if (a_blockStarts) return;
                auto& wait = g_pick[a_id];
                wait -= a_dt;
                if (wait > 0.0f) return;
                wait = kPickEverySec;
                const auto now = Clock::now();
                for (const auto& d : Reg()) {
                    {
                        std::lock_guard lk(g_mx);
                        if (g_results.contains(d.id)) continue;
                    }
                    bool claimed = false;
                    for (const auto& [fid, r] : g_runs)
                        if (r.def == &d) { claimed = true; break; }
                    if (claimed) continue;
                    if (auto b = g_backoff.find({ a_id, d.id }); b != g_backoff.end()) {
                        if (now < b->second) continue;
                        g_backoff.erase(b);
                    }
                    Scratch s;
                    Ctx     c{ a, a_id, &a_st, inCombat, 0.0f, d.timeoutSec, now, g_seen[a_id], &s };
                    if (d.precondition && d.precondition(c)) continue;

                    Run r;
                    r.def       = &d;
                    r.token     = g_nextToken++;
                    r.startedAt = now;
                    FixtureRequest req;
                    if (d.fixtures) d.fixtures(c, req);
                    r.s = std::move(s);
                    spdlog::info("[selftest] START {} ({}) on {:08X} '{}': timeout {:.0f}s, fixtures asked: {} item(s), "
                                 "{} spell(s)",
                                 d.id, d.ref, a_id, NameOf(a), d.timeoutSec, req.items.size(), req.spells.size());
                    if (req.items.empty() && req.spells.empty()) {
                        r.fixturesReady = true;
                    } else {
                        PostFixtureAdd(r.token, d.id, a_id, std::move(req));
                    }
                    g_runs.emplace(a_id, std::move(r));
                    return;
                }
                return;
            }

            Run& r = it->second;
            if (!r.fixturesReady) {
                r.fixtureWait += a_dt;
                std::optional<FixtureAck> ack;
                {
                    std::lock_guard lk(g_mx);
                    if (auto k = g_acks.find(r.token); k != g_acks.end()) {
                        ack = std::move(k->second);
                        g_acks.erase(k);
                    }
                }
                if (ack && !ack->ok) {
                    Finish(a_id, r, Step::Retry("fixtures FAILED: " + ack->note), "ABORT", true);
                    g_runs.erase(it);
                    return;
                }
                if (ack) {
                    r.fixturesReady = true;
                    r.fixtureNote   = ack->note;
                    r.startedAt     = Clock::now();
                    spdlog::info("[selftest] fixtures for {} on {:08X}: {}", r.def->id, a_id, r.fixtureNote);
                } else if (r.fixtureWait > kFixtureWaitSec) {
                    Finish(a_id, r,
                           Step::Retry(std::format("fixtures not applied within {:.0f}s (is the main-thread pump "
                                                   "running?)",
                                                   kFixtureWaitSec)),
                           "ABORT", true);
                    g_runs.erase(it);
                }
                return;
            }

            r.elapsed += a_dt;
            r.sinceStep += a_dt;
            Ctx c{ a, a_id, &a_st, inCombat, r.elapsed, r.def->timeoutSec, r.startedAt, g_seen[a_id], &r.s };
            if (r.elapsed >= r.def->timeoutSec) {
                Step s = r.def->onTimeout ? r.def->onTimeout(c) : Step{};
                if (s.v == Verdict::Running)
                    s = Step::Fail(std::format("timeout after {:.0f}s{}", r.def->timeoutSec,
                                               r.s.note.empty() ? std::string{} : ": " + r.s.note));
                Finish(a_id, r, s, s.v == Verdict::Pass ? "PASS" : s.v == Verdict::Fail ? "FAIL" : "RETRY");
                g_runs.erase(it);
                return;
            }
            if (r.sinceStep < kStepEverySec) return;
            r.sinceStep = 0.0f;
            const Step s = r.def->step(c);
            if (s.v == Verdict::Running) return;
            Finish(a_id, r, s, s.v == Verdict::Pass ? "PASS" : s.v == Verdict::Fail ? "FAIL" : "RETRY");
            g_runs.erase(it);
        }
    }

    // ── registry ─────────────────────────────────────────────────────────────────────────────────
    void Register(const TestDef& a_def) {
        for (const auto& d : Reg()) {
            if (std::string_view(d.id) == a_def.id) {
                spdlog::error("[selftest] duplicate test id '{}' -- second registration ignored", a_def.id);
                return;
            }
        }
        if (!a_def.step) {
            spdlog::error("[selftest] test '{}' has no step() -- not registered", a_def.id);
            return;
        }
        Reg().push_back(a_def);
    }
    const std::vector<TestDef>& Registry() { return Reg(); }

    // ── read helpers ─────────────────────────────────────────────────────────────────────────────
    std::int32_t CountItem(RE::Actor* a_actor, RE::FormID a_form) {
        if (!a_actor || !a_form) return 0;
        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_form);
        if (!obj) return 0;
        const auto counts = a_actor->GetInventoryCounts([obj](RE::TESBoundObject& o) { return &o == obj; });
        const auto it     = counts.find(obj);
        return it == counts.end() ? 0 : it->second;
    }

    RE::FormID VanillaForm(RE::FormID a_localID) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupFormID(a_localID, "Skyrim.esm") : 0;
    }

    std::string FormName(RE::FormID a_form) {
        auto*       f = a_form ? RE::TESForm::LookupByID(a_form) : nullptr;
        const char* n = f ? f->GetName() : nullptr;
        return (n && *n) ? std::string(n) : std::format("{:08X}", a_form);
    }

    // ── public ───────────────────────────────────────────────────────────────────────────────────
    void Tick() {
        if (!g_registered) {
            g_registered = true;
            RegisterAll();
            spdlog::warn("[selftest] DEV SELF-TEST BUILD (MFO_DEV_SELFTEST): {} test(s) registered. Never ship this "
                         "DLL.",
                         Reg().size());
        }
        RegisterSinks();   // posted once (retried every 5 s until live; a revert can drop the Post)
        const auto now = Clock::now();
        float      dt  = g_last.time_since_epoch().count() != 0
                             ? std::chrono::duration<float>(now - g_last).count()
                             : 0.0f;
        g_last         = now;
        dt             = std::clamp(dt, 0.0f, 0.5f);   // a load screen / hitch never jumps a timeout

        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) return;   // run time frozen

        bool afterLoad = false;
        bool orphans   = false;
        bool postOrphans = false;
        {
            std::lock_guard lk(g_mx);
            afterLoad   = g_afterLoad;
            g_afterLoad = false;
            for (const auto& f : g_ledger)
                if (f.token == 0) { orphans = true; break; }
            g_orphanWait -= dt;
            if (orphans && !g_orphanCleanupInFlight && g_orphanWait <= 0.0f) {
                g_orphanCleanupInFlight = true;
                g_orphanWait            = kOrphanRetrySec;
                postOrphans             = true;
            }
        }
        if (afterLoad) {
            ApplyResetIni();
            LogPending("after load", true);
        }
        if (postOrphans) {
            spdlog::info("[selftest] removing fixture(s) a test added before this save was made (the test was "
                         "interrupted); no test starts until they are gone");
            PostFixtureCleanup(0, "left by an interrupted test");
        }

        std::unordered_set<RE::FormID> live;
        for (const auto& h : Followers::g_active) {
            auto  ptr = h.get();
            auto* a   = ptr.get();
            if (!a || a->IsDead() || a->IsDisabled()) continue;
            const RE::FormID id = a->GetFormID();
            g_seen[id] += dt;
            if (!Followers::IsPersistableID(id)) continue;   // #9: never fixture a runtime actor
            const auto rec = g_followers.find(id);
            if (rec == g_followers.end() || !rec->second.mfoEnabled || !HasRunnerRule(rec->second)) continue;
            live.insert(id);
            ServiceFollower(a, id, rec->second, dt, orphans);
        }
        for (auto it = g_runs.begin(); it != g_runs.end();) {
            if (live.contains(it->first)) { ++it; continue; }
            Finish(it->first, it->second,
                   Step::Retry("the follower left the party, died, or lost the 'DEV: run self-tests' rule"), "ABORT");
            it = g_runs.erase(it);
        }
    }

    void ClearAll() {
        g_runs.clear();
        g_seen.clear();
        g_pick.clear();
        g_backoff.clear();
        g_last       = {};
        g_orphanWait = 0.0f;
        ClearTestState();
        std::lock_guard lk(g_mx);
        g_results.clear();
        g_ledger.clear();
        g_acks.clear();
        g_resetGeneration       = 0;
        g_orphanCleanupInFlight = false;
        g_afterLoad             = true;
    }

}
