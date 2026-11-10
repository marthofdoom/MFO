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
//
// SELF-DRIVING (marth 2026-11-10: "It needs to be able to test everything on its own."): a test no longer waits for the follower's
// board to hold what it watches. Its fixtures() asks for TEMP ROWS, a CLASS SNAPSHOT, a SPAWNED HOSTILE, memory-only PROBE flags (Setup.cpp,
// Fixtures.cpp); the runner applies them when the fixtures are in, and Finish() puts every one back, on every way a run can end.
// The game being paused (menus) freezes run time; nothing is judged while paused.
//
// THREADING: g_runs / g_seen / g_pick / g_backoff are WORKER-ONLY (no lock: the worker is serial, the save
// callback pauses the pump, ResetAllState / LoadCallback run with it stopped). The shared state (results,
// ledger, acks) is under the LEAF lock g_mx, never held across an engine call.

#include "selftest/SelfTest_internal.h"
#include "Followers.h"
#include "Vocabulary.h"
#include "Config.h"
#include "classes/Classes.h"
#include "MainThread.h"

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
            FixtureRequest req;                 // kept: the spawn spec and the rows are applied/undone from it
            RunSetup       setup;
            bool           setupApplied = false;
            bool           spawnRun = false;
            int            respawnsLeft = 0;
            bool           everInCombat = false;
            float          graceUntil = 0.0f;   // run seconds: until then a spawn run counts as "in combat" (the hostile is walking up)
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
        float                                                  g_classWait = 0.0f;
        // The last precondition answer per test id ("" = it could start): a pending test whose input is missing (the class add-on, a second
        // ally, Harbinger) must not keep the final line from ever printing. WORKER-ONLY.
        std::map<std::string, std::string>                     g_lastPre;
        bool                                                   g_blockedAnnounced = false;

        constexpr float kPickEverySec   = 1.0f;
        constexpr float kStepEverySec   = 0.5f;
        constexpr float kFixtureWaitSec = 10.0f;
        constexpr auto  kBackoff        = std::chrono::seconds(30);
        constexpr float kOrphanRetrySec = 30.0f;
        constexpr const char* kIniPath  = "Data/SKSE/Plugins/MFO_SelfTest.ini";

        const char* WordOf(Verdict v) {
            return v == Verdict::Pass ? "PASS" : v == Verdict::Fail ? "FAIL" : v == Verdict::Info ? "INFO" : "RETRY";
        }
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
        // When nothing is pending: `[selftest] ALL DONE: PASS n, FAIL n, MANUAL n (ids), INFO n` (marth 2026-11-10).
        void LogPending(const char* a_why, bool a_detail) {
            std::vector<const TestDef*>   pending;
            std::map<std::string, Result> results;
            {
                std::lock_guard lk(g_mx);
                results = g_results;
            }
            int pass = 0, fail = 0, info = 0;
            for (const auto& [id, r] : results) (r.info ? info : (r.pass ? pass : fail))++;
            std::string ids;
            for (const auto& d : Reg()) {
                if (results.contains(d.id)) continue;
                pending.push_back(&d);
                ids += (ids.empty() ? "" : ", ") + std::string(d.id);
            }
            int holders = 0;
            for (const auto& [fid, st] : g_followers)
                if (HasRunnerRule(st)) ++holders;
            spdlog::info("[selftest] pending ({} of {}, {}): {} | complete {}: PASS {}, FAIL {}, INFO {} | {} follower(s) "
                         "hold the 'DEV: run self-tests' rule",
                         pending.size(), Reg().size(), a_why, ids.empty() ? "none" : ids, results.size(), pass, fail, info,
                         holders);
            if (a_detail) {
                for (const auto* d : pending)
                    spdlog::info("[selftest]   pending {} ({}): needs {}", d->id, d->ref, d->needs);
                for (const auto& [id, r] : results)
                    spdlog::info("[selftest]   done {}: {} on {:08X} -- {}", id, r.info ? "INFO" : (r.pass ? "PASS" : "FAIL"), r.follower,
                                 r.evidence);
                for (const auto& m : ManualItems())
                    spdlog::info("[selftest]   MANUAL {} ({}): the DLL cannot judge it ({}). LOOK AT: {}", m.id, m.ref, m.why, m.look);
            }
            std::string manual;
            for (const auto& m : ManualItems()) manual += (manual.empty() ? "" : ", ") + std::string(m.id);
            if (pending.empty()) {
                spdlog::info("[selftest] ALL DONE: PASS {}, FAIL {}, MANUAL {} ({}), INFO {}", pass, fail, ManualItems().size(),
                             manual.empty() ? "none" : manual, info);
            } else {
                // every pending test is waiting on an input the runner cannot supply (its last precondition answer): say so, once
                std::string blocked;
                bool        all = true;
                for (const auto* d : pending) {
                    const auto it = g_lastPre.find(d->id);
                    if (it == g_lastPre.end() || it->second.empty()) { all = false; break; }
                    blocked += (blocked.empty() ? "" : "; ") + std::string(d->id) + " (needs " + it->second + ")";
                }
                if (all)
                    spdlog::info("[selftest] ALL RUNNABLE DONE: PASS {}, FAIL {}, MANUAL {} ({}), INFO {} | BLOCKED {}: {}", pass, fail,
                                 ManualItems().size(), manual.empty() ? "none" : manual, info, pending.size(), blocked);
            }
        }

        std::string Trim(std::string a_s) {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            const auto e = a_s.find_last_not_of(" \t\r\n");
            return a_s.substr(b, e - b + 1);
        }

        // ── CARD C5c: DEV CLASS EQUIP (DEV BUILDS ONLY; Docs/SELFTEST.md "Dev class equip") ───────────────────────────────────
        // Nothing can equip a class until the board card C10, so this puts followers into classes for field tests of the C6
        // passive carriers and the H5 miss budgets. [Debug] sDevClassEquip = a comma-separated list of
        //     <follower>:<class slug>:<level>[:mastered]
        // where <follower> is the actor ref's FormID: "Plugin.esp|HEXLOCAL" (load-order independent, preferred) or a full
        // 8-digit hex FormID (this load order only). Applied ONCE per load, per entry, as soon as that follower is a live
        // managed one and the class add-on is Active(). Only the existing State.cpp writers are used (SetUnlocked,
        // ActivateClass, ApplyLevel): the dev override is SetUnlocked(old | class bit), which skips the unlock rule so any class
        // can be tested; `mastered` applies the class cap level (ApplyLevel sets the mastered bit at the cap). This WRITES MCLS
        // like a real equip, so the save carries it: dev saves only. WORKER-ONLY (called from Tick on the pump worker).
        struct DevEntry {
            std::string   raw;
            std::string   follower;   // as written, for the log
            RE::FormID    fid      = 0;
            Classes::Id   cls      = Classes::Id::Melee;
            std::uint8_t  level    = 1;
            bool          mastered = false;
            bool          done     = false;
        };
        std::vector<DevEntry> g_devEntries;
        bool                  g_devParsed         = false;
        float                 g_devWaited         = 0.0f;
        bool                  g_devWarnedWait     = false;
        bool                  g_devWarnedInactive = false;

        std::optional<RE::FormID> DevResolveFollower(const std::string& a_tok) {
            try {
                if (const auto bar = a_tok.find('|'); bar != std::string::npos) {
                    const std::string file  = Trim(a_tok.substr(0, bar));
                    const auto        local = static_cast<RE::FormID>(std::stoul(Trim(a_tok.substr(bar + 1)), nullptr, 16));
                    auto*             dh    = RE::TESDataHandler::GetSingleton();
                    const RE::FormID  id    = (dh && !file.empty()) ? dh->LookupFormID(local, file) : 0;
                    if (id) return id;
                    return std::nullopt;
                }
                const auto id = static_cast<RE::FormID>(std::stoul(a_tok, nullptr, 16));
                if (id) return id;
            } catch (...) {}
            return std::nullopt;
        }

        void DevParse(const std::string& a_text) {
            std::size_t pos = 0;
            while (pos <= a_text.size()) {
                auto end = a_text.find(',', pos);
                if (end == std::string::npos) end = a_text.size();
                const std::string raw = Trim(a_text.substr(pos, end - pos));
                pos                   = end + 1;
                if (raw.empty()) continue;
                std::vector<std::string> f;
                for (std::size_t q = 0;;) {
                    const auto c = raw.find(':', q);
                    f.push_back(Trim(raw.substr(q, c == std::string::npos ? std::string::npos : c - q)));
                    if (c == std::string::npos) break;
                    q = c + 1;
                }
                if (f.size() < 3 || f.size() > 4 || (f.size() == 4 && f[3] != "mastered")) {
                    spdlog::warn("[classes] DEV equip: cannot parse '{}' (want <follower>:<class slug>:<level>[:mastered]) -- skipped", raw);
                    continue;
                }
                DevEntry e;
                e.raw          = raw;
                e.follower     = f[0];
                const auto fid = DevResolveFollower(f[0]);
                if (!fid) {
                    spdlog::warn("[classes] DEV equip: unknown follower '{}' in '{}' (want Plugin.esp|HEXLOCAL or a full hex FormID) -- skipped", f[0], raw);
                    continue;
                }
                e.fid = *fid;
                std::string slug = f[1];
                for (auto& ch : slug) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const auto* def = Classes::FindBySlug(slug);
                if (!def) {
                    spdlog::warn("[classes] DEV equip: unknown class '{}' in '{}' -- skipped", f[1], raw);
                    continue;
                }
                e.cls = def->id;
                const std::uint8_t cap = Classes::LevelCap(e.cls);
                int                lvl = 0;
                try {
                    std::size_t used = 0;
                    lvl              = std::stoi(f[2], &used, 10);
                    if (used != f[2].size()) lvl = 0;
                } catch (...) { lvl = 0; }
                if (cap == 0) {
                    lvl = 0;   // Dragonborn: no level; the field is accepted and ignored
                } else if (lvl < 1 || lvl > cap) {
                    spdlog::warn("[classes] DEV equip: level '{}' in '{}' is not 1..{} for {} -- skipped", f[2], raw,
                                 static_cast<int>(cap), def->slug);
                    continue;
                }
                e.level    = static_cast<std::uint8_t>(lvl);
                e.mastered = f.size() == 4;
                g_devEntries.push_back(std::move(e));
            }
        }

        void DevClassEquipTick(float a_dt) {
            if (!g_devParsed) {
                std::string text;
                {
                    std::lock_guard lk(Config::g_devClassEquipMx);
                    text = Config::g_devClassEquip;
                }
                g_devParsed = true;
                if (text.empty()) return;
                DevParse(text);
                spdlog::warn("[classes] DEV equip (dev build only): {} entr{} queued from [Debug] sDevClassEquip; each applies once this "
                             "load and writes MCLS (dev saves only)",
                             g_devEntries.size(), g_devEntries.size() == 1 ? "y" : "ies");
            }
            if (g_devEntries.empty()) return;
            bool pending = false;
            for (const auto& e : g_devEntries)
                if (!e.done) { pending = true; break; }
            if (!pending) return;
            if (!Classes::Active()) {
                if (!g_devWarnedInactive) {
                    g_devWarnedInactive = true;
                    spdlog::warn("[classes] DEV equip: the class add-on is not Active() (MFO_Classes.esl missing or bClassTree = 0) -- waiting");
                }
                return;
            }
            g_devWaited += a_dt;
            for (auto& e : g_devEntries) {
                if (e.done) continue;
                RE::NiPointer<RE::Actor> hold;
                for (const auto& h : Followers::g_active) {
                    auto p = h.get();
                    if (p && p->GetFormID() == e.fid && !p->IsDead() && !p->IsDisabled()) { hold = p; break; }
                }
                if (!hold || !g_followers.contains(e.fid) || !Followers::IsPersistableID(e.fid)) continue;   // not (yet) a live managed follower
                e.done = true;
                const Classes::ClassDef& def = Classes::Get(e.cls);
                const std::uint8_t       cap = Classes::LevelCap(e.cls);
                const std::uint8_t       lvl = (e.mastered && cap) ? cap : e.level;
                bool ok = Classes::SetUnlocked(e.fid, Classes::GetUnlocked(e.fid) | Classes::Bit(e.cls));   // the dev unlock override
                if (ok) Classes::ActivateClass(e.fid, e.cls);                    // false when already equipped: fine
                if (ok && cap) Classes::ApplyLevel(e.fid, e.cls, lvl);           // false when nothing changed: fine
                const auto prog = Classes::GetProgress(e.fid, e.cls);
                ok = ok && Classes::GetEquipped(e.fid) == static_cast<std::uint8_t>(e.cls) && (!cap || prog.level >= lvl) &&
                     (!e.mastered || !cap || (Classes::GetMastered(e.fid) & Classes::Bit(e.cls)) != 0);
                if (ok) {
                    spdlog::warn("[classes] DEV equip {:08X} ({}) -> {} L{}{} (dev build only)", e.fid, NameOf(hold.get()), def.slug,
                                 static_cast<int>(prog.level), e.mastered ? " mastered" : "");
                } else {
                    spdlog::warn("[classes] DEV equip {:08X} ({}) -> {} REFUSED (a writer refused: class add-on not Writable(), or MCLS kept "
                                 "read-only) -- '{}'",
                                 e.fid, NameOf(hold.get()), def.slug, e.raw);
                }
            }
            if (!g_devWarnedWait && g_devWaited > 60.0f) {
                for (const auto& e : g_devEntries) {
                    if (e.done) continue;
                    g_devWarnedWait = true;
                    spdlog::warn("[classes] DEV equip: {:08X} ('{}') is still not a live managed follower after 60 s -- still waiting", e.fid,
                                 e.follower);
                }
            }
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

        // One end for every way a run stops. a_complete = PASS / FAIL / INFO (persisted); else RETRY / ABORT.
        // EVERY end tears the run's setup down first (rows, class state, probes), then removes its fixtures / spawn / damage.
        void Finish(RE::FormID a_id, Run& a_run, const Step& a_step, const char* a_word, bool a_error = false,
                    std::chrono::seconds a_backoff = std::chrono::seconds(30)) {
            const auto* d    = a_run.def;
            const bool  done = a_step.v == Verdict::Pass || a_step.v == Verdict::Fail || a_step.v == Verdict::Info;
            auto*       a    = RE::TESForm::LookupByID<RE::Actor>(a_id);
            const auto  line = std::format("[selftest] {} {} {:08X} '{}' t={:.1f}s: {}{}", a_word, d->id, a_id,
                                           NameOf(a), a_run.elapsed, a_step.evidence,
                                           a_run.fixtureNote.empty() ? std::string{}
                                                                     : " | setup: " + a_run.fixtureNote);
            if (a_error) spdlog::error("{}", line);
            else spdlog::info("{}", line);
            {
                auto rec = g_followers.find(a_id);
                SetupTeardown(a_id, rec == g_followers.end() ? nullptr : &rec->second, a_run.token, a_run.setup, d->id);
            }
            {
                std::lock_guard lk(g_mx);
                if (done) g_results[d->id] = { a_step.v != Verdict::Fail, a_id, a_step.evidence, a_step.v == Verdict::Info };
                g_acks.erase(a_run.token);
                g_spawnRef.erase(a_run.token);
            }
            PostFixtureCleanup(a_run.token, d->id);
            g_blockedAnnounced = false;
            if (done) LogPending(std::format("after {}", d->id).c_str(), false);
            else g_backoff[{ a_id, d->id }] = Clock::now() + a_backoff;
        }

        void ServiceFollower(RE::Actor* a, RE::FormID a_id, FollowerState& a_st, float a_dt, bool a_blockStarts) {
            bool       inCombat = a->IsInCombat();   // a spawn run may raise it for the grace window below
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
                    const char* why = d.precondition ? d.precondition(c) : nullptr;
                    g_lastPre[d.id] = why ? why : "";
                    if (why) continue;
                    g_blockedAnnounced = false;

                    Run r;
                    r.def       = &d;
                    r.token     = g_nextToken++;
                    r.startedAt = now;
                    FixtureRequest req;
                    if (d.fixtures) d.fixtures(c, req);
                    if (req.spawn.on) {   // ONE spawn-using run at a time, globally: a second waits (this test is tried again next pass)
                        bool other = false;
                        for (const auto& [fid, rr] : g_runs) other = other || rr.spawnRun;
                        if (other) continue;
                    }
                    r.s = std::move(s);
                    r.spawnRun     = req.spawn.on;
                    r.respawnsLeft = req.spawn.on ? req.spawn.respawns : 0;
                    r.graceUntil   = 10.0f;
                    spdlog::info("[selftest] START {} ({}) on {:08X} '{}': timeout {:.0f}s, setup asked: {} item(s), {} spell(s), {} temp row(s), "
                                 "class snapshot {}, hostile {}{}, probes {:#x}",
                                 d.id, d.ref, a_id, NameOf(a), d.timeoutSec, req.items.size(), req.spells.size(), req.rows.size(),
                                 req.cls.snapshot ? "yes" : "no", req.spawn.on ? "yes" : "no",
                                 req.spawn.hurtPct > 0 ? std::format(" (follower hurt {:.0f}%)", req.spawn.hurtPct) : std::string{}, req.probes);
                    r.req = req;
                    if (req.items.empty() && req.spells.empty() && !req.spawn.on && req.spawn.hurtPct <= 0.0f) {
                        r.fixturesReady = true;
                        SetupApply(a_id, a_st, r.token, r.req, r.setup);
                        r.setupApplied = true;
                    } else {
                        PostFixtureAdd(r.token, d.id, a_id, std::move(req));
                    }
                    g_runs.emplace(a_id, std::move(r));
                    return;
                }
                if (!g_blockedAnnounced) {   // nothing started this pass: if every pending test is blocked on a missing input, say so once
                    bool anyPending = false, allBlocked = true;
                    {
                        std::lock_guard lk(g_mx);
                        for (const auto& d : Reg()) {
                            if (g_results.contains(d.id)) continue;
                            anyPending = true;
                            const auto it = g_lastPre.find(d.id);
                            if (it == g_lastPre.end() || it->second.empty()) allBlocked = false;
                        }
                    }
                    if (anyPending && allBlocked && g_runs.empty()) {
                        g_blockedAnnounced = true;
                        LogPending("every pending test is blocked", true);
                    }
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
                if (ack && !ack->ok && ack->refuse) {
                    Finish(a_id, r, Step::Retry("not run: " + ack->note), "RETRY", false, std::chrono::seconds(120));
                    g_runs.erase(it);
                    return;
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
                    SetupApply(a_id, a_st, r.token, r.req, r.setup);
                    r.setupApplied = true;
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
            RE::Actor* hostile = nullptr;
            if (r.spawnRun) {
                RE::FormID fid = 0;
                {
                    std::lock_guard lk(g_mx);
                    if (auto k = g_spawnRef.find(r.token); k != g_spawnRef.end()) fid = k->second;
                }
                if (auto* h = fid ? RE::TESForm::LookupByID<RE::Actor>(fid) : nullptr;
                    h && !h->IsDead() && !h->IsDisabled() && h->GetBaseObject() && h->GetBaseObject()->GetFormID() == VanillaForm(r.req.spawn.baseLocal))
                    hostile = h;   // identity: the spawned base, not whatever now holds the id
                // the hostile died while the test still runs: another one (bounded), and a fresh grace while it walks up
                if (!hostile && r.respawnsLeft > 0 && r.elapsed >= r.graceUntil && r.setupApplied) {
                    --r.respawnsLeft;
                    r.graceUntil = r.elapsed + 8.0f;
                    spdlog::info("[selftest] {} on {:08X}: the hostile is gone -- respawning ({} respawn(s) left after this)", r.def->id, a_id,
                                 r.respawnsLeft);
                    PostRespawn(r.token, r.def->id, a_id, r.req.spawn);
                    return;
                }
                if (inCombat) r.everInCombat = true;
                if (!inCombat && r.elapsed < r.graceUntil) inCombat = true;   // the hostile is still walking up: the fight has not begun, not ended
            }
            Ctx c{ a, a_id, &a_st, inCombat, r.elapsed, r.def->timeoutSec, r.startedAt, g_seen[a_id], &r.s,
                   hostile, r.respawnsLeft, r.spawnRun, r.everInCombat };
            if (r.elapsed >= r.def->timeoutSec) {
                Step s = r.def->onTimeout ? r.def->onTimeout(c) : Step{};
                if (s.v == Verdict::Running)
                    s = Step::Fail(std::format("timeout after {:.0f}s{}", r.def->timeoutSec,
                                               r.s.note.empty() ? std::string{} : ": " + r.s.note));
                Finish(a_id, r, s, WordOf(s.v));
                g_runs.erase(it);
                return;
            }
            if (r.sinceStep < kStepEverySec) return;
            r.sinceStep = 0.0f;
            const Step s = r.def->step(c);
            if (s.v == Verdict::Running) return;
            Finish(a_id, r, s, WordOf(s.v));
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
            if (g_classOrphans.load()) orphans = true;   // a class restore point from an interrupted run: no test starts until it is applied
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

        if (g_classOrphans.load()) {   // WORKER; retried every 30 s while it fails (the writers refuse until the class add-on is Writable)
            g_classWait -= dt;
            if (g_classWait <= 0.0f) {
                g_classWait = kOrphanRetrySec;
                RestoreOrphanClassSnaps();
            }
        }

        DevClassEquipTick(dt);   // card C5c

        std::unordered_set<RE::FormID> live;
        for (const auto& h : Followers::g_active) {
            auto  ptr = h.get();
            auto* a   = ptr.get();
            if (!a || a->IsDead() || a->IsDisabled()) continue;
            const RE::FormID id = a->GetFormID();
            g_seen[id] += dt;
            if (!Followers::IsPersistableID(id)) continue;   // #9: never fixture a runtime actor
            auto rec = g_followers.find(id);
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
        // The world is being replaced (load / revert): no engine call, no table to edit (g_followers is reset too). The one thing that
        // OUTLIVES a load is a probe flag switched on in memory: put it back before the runs are forgotten.
        for (auto& [fid, r] : g_runs) {
            if (r.setup.probes & kProbeAim) Config::g_aimProbe.store(r.setup.probePrev[0]);
        }
        g_runs.clear();
        g_seen.clear();
        g_pick.clear();
        g_backoff.clear();
        g_last       = {};
        g_orphanWait = 0.0f;
        g_classWait  = 0.0f;
        g_lastPre.clear();
        g_blockedAnnounced = false;
        g_devEntries.clear();   // card C5c: applied once per load
        g_devParsed         = false;
        g_devWaited         = 0.0f;
        g_devWarnedWait     = false;
        g_devWarnedInactive = false;
        ClearTestState();
        std::lock_guard lk(g_mx);
        g_results.clear();
        g_ledger.clear();
        g_classSnaps.clear();
        g_runRows.clear();
        g_spawnRef.clear();
        g_classOrphans.store(false);
        g_acks.clear();
        g_resetGeneration       = 0;
        g_orphanCleanupInFlight = false;
        g_afterLoad             = true;
    }

}
