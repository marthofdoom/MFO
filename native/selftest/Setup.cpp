// THE SELF-SETUP LEDGERS (dev builds only; Docs/SELFTEST.md "Self-driving"). marth 2026-11-10: "It needs to be able to test
// everything on its own."
//
// A run may change four things on a follower besides his fixtures (Fixtures.cpp): TEMPORARY GAMBIT ROWS, his CLASS STATE (MCLS),
// log-only PROBE flags (memory only, never the INI), and a SPAWNED HOSTILE (Fixtures.cpp, main thread). Every change is recorded
// when it is made and put back when the run ends, whatever way it ends (verdict, timeout, ABORT, follower gone). Every setup and
// teardown step logs at info; a restore that did not take logs at ERROR (never masked).
//
// WORKER-ONLY for rows and class state (g_followers is main-thread / serial-worker only, #4; the Classes writers are mutex-guarded).
// The shared registries (g_runRows for the FLWR writer, g_classSnaps for 'MFST') are under the LEAF lock g_mx.
//
// MID-RUN SAVES (the safety design):
//   * ROWS: the FLWR writer (Serialization.cpp, `#ifdef MFO_DEV_SELFTEST`) skips every row whose uid is registered here, so a save
//     taken mid-run never contains a run row; the row count it writes is of the rows it wrote. Layout unchanged.
//   * CLASS STATE: MCLS is written as it stands (the temporary class IS in it), so the restore point is in 'MFST' v2 and the first
//     tick after the load restores it (RestoreOrphanClassSnaps) before any test starts.
//   * SPAWN / HURT / FIXTURES: the fixture ledger in 'MFST' (Fixtures.cpp), orphan-cleaned the same way.

#include "selftest/SelfTest_internal.h"
#include "Config.h"
#include "Followers.h"
#include "apmf/APMFBridge.h"   // IsSubstrateAnchor: Harbinger's ghost stand-in is not a bystander
#include "LocationTypes.h"
#include "classes/Classes.h"
#include "logistics/Storage.h"   // storage.chest_roundtrip: arm / disarm the DEV round trip (86e3t212u)

namespace MFO::SelfTest {

    namespace {

        bool SameSnap(const ClassSnap& a, const Classes::FollowerClasses& b) {
            if (a.equipped != b.equipped || a.mastered != b.mastered || a.unlocked != b.unlocked || a.thief != b.thiefFights ||
                a.masterThief != b.masterThiefFights)
                return false;
            for (std::size_t i = 0; i < Classes::kClassCount; ++i)
                if (a.xp[i] != b.progress[i].xp || a.level[i] != b.progress[i].level) return false;
            return true;
        }

        // Write the snapshot back through the State.cpp writers (the only road; nothing writes MCLS raw). Verifies by reading it back.
        bool RestoreClass(RE::FormID a_fid, const ClassSnap& a_snap) {
            bool ok = true;
            if (a_snap.equipped == Classes::kNoClass || a_snap.equipped >= Classes::kClassCount) ok &= Classes::ClearEquipped(a_fid);
            else ok &= Classes::SetEquipped(a_fid, static_cast<Classes::Id>(a_snap.equipped));
            for (std::size_t i = 0; i < Classes::kClassCount; ++i)
                ok &= Classes::SetProgress(a_fid, static_cast<Classes::Id>(i), { a_snap.xp[i], a_snap.level[i] });
            ok &= Classes::SetMastered(a_fid, a_snap.mastered);
            ok &= Classes::SetUnlocked(a_fid, a_snap.unlocked);
            ok &= Classes::SetStealthFights(a_fid, a_snap.thief, a_snap.masterThief);
            Followers::RepublishActiveMirror();   // C10 HOOK (Classes.h): the equip changed; WORKER
            const auto now = Classes::GetFollower(a_fid);
            return ok && now && SameSnap(a_snap, *now);
        }

        void FillSnap(ClassSnap& o, const std::optional<Classes::FollowerClasses>& fc) {
            o.hadRecord = fc.has_value();
            if (!fc) return;
            o.equipped = fc->equipped;
            for (std::size_t i = 0; i < Classes::kClassCount; ++i) {
                o.xp[i]    = fc->progress[i].xp;
                o.level[i] = fc->progress[i].level;
            }
            o.mastered    = fc->mastered;
            o.unlocked    = fc->unlocked;
            o.thief       = fc->thiefFights;
            o.masterThief = fc->masterThiefFights;
        }

        std::string ClassName(std::uint8_t a_eq) {
            return a_eq < Classes::kClassCount ? std::string(Classes::Get(static_cast<Classes::Id>(a_eq)).slug) : std::string("none");
        }
    }

    // ── public: rows bookkeeping for the FLWR writer ─────────────────────────────────────────────
    bool HasRunRows(RE::FormID a_follower) {
        std::lock_guard lk(g_mx);
        const auto it = g_runRows.find(a_follower);
        return it != g_runRows.end() && !it->second.empty();
    }
    bool IsRunRow(RE::FormID a_follower, std::uint32_t a_uid) {
        std::lock_guard lk(g_mx);
        const auto it = g_runRows.find(a_follower);
        return it != g_runRows.end() && std::find(it->second.begin(), it->second.end(), a_uid) != it->second.end();
    }

    void SetupApply(RE::FormID a_follower, FollowerState& a_st, std::uint32_t a_token, const FixtureRequest& a_req, RunSetup& a_out) {
        // 1. class snapshot FIRST (so a failure below still restores)
        if (a_req.cls.snapshot) {
            const auto fc = Classes::GetFollower(a_follower);
            ClassSnap  s;
            s.token    = a_token;
            s.follower = a_follower;
            FillSnap(s, fc);
            a_out.snap     = s;
            a_out.haveSnap = true;
            {
                std::lock_guard lk(g_mx);
                g_classSnaps.push_back(s);
            }
            spdlog::info("[selftest] setup {:08X}: class snapshot taken (equipped {}, mastered mask {:X}, unlocked mask {:X}, record {})",
                         a_follower, ClassName(s.equipped), s.mastered, s.unlocked, s.hadRecord ? "existed" : "none");
        }
        // 2. rows (front of the table = highest priority; the player's rows keep their order below)
        for (const auto& r : a_req.rows) {
            Gambit g;
            g.uid             = NextRuleUID();
            g.conditionOpcode = r.cond;
            g.actionOpcode    = r.act;
            g.conditionParam  = r.condParam;
            g.actionParamForm = r.form;
            g.subjectSelector = r.subject;
            g.enabled         = true;
            auto& tbl         = a_st.tables[static_cast<std::size_t>(r.table)];
            {
                std::lock_guard lk(g_mx);   // registered BEFORE the row exists: a save can never see an unregistered run row
                g_runRows[a_follower].push_back(g.uid);
            }
            tbl.insert(tbl.begin(), std::move(g));
            a_out.rowUids.push_back(tbl.front().uid);
            spdlog::info("[selftest] setup {:08X}: TEMP ROW uid {} inserted at the front of the {} table: {} -> {} (form {:08X} '{}', "
                         "param {}, subject {})", a_follower, a_out.rowUids.back(), r.table == Table::Combat ? "combat" : "logistics",
                         r.cond, r.act, r.form, r.form ? FormName(r.form) : std::string("-"), r.condParam, static_cast<int>(r.subject));
        }
        // 3. probes (memory only)
        if (a_req.probes & kProbeAim) {
            a_out.probePrev[0] = Config::g_aimProbe.exchange(true);
            a_out.probes |= kProbeAim;
            spdlog::info("[selftest] setup {:08X}: probe bAimProbe switched ON in memory for the run (was {}; the INI is untouched)",
                         a_follower, a_out.probePrev[0] ? "on" : "off");
        }
        if (a_req.probes & kProbeStorageMove) {   // storage.chest_roundtrip: Storage::Tick moves one Iron Dagger to the MFO chest and back
            Logistics::Storage::DevArmRoundTrip(a_follower, VanillaForm(0x0001397E));
            a_out.probes |= kProbeStorageMove;
        }
    }

    void SetupTeardown(RE::FormID a_follower, FollowerState* a_st, std::uint32_t a_token, RunSetup& a_setup, const char* a_why) {
        // rows
        if (!a_setup.rowUids.empty()) {
            std::size_t removed = 0;
            if (a_st) {
                for (auto& tbl : a_st->tables) {
                    const auto before = tbl.size();
                    std::erase_if(tbl, [&](const Gambit& g) {
                        return std::find(a_setup.rowUids.begin(), a_setup.rowUids.end(), g.uid) != a_setup.rowUids.end();
                    });
                    removed += before - tbl.size();
                }
                std::size_t left = 0;
                for (const auto& tbl : a_st->tables)
                    for (const auto& g : tbl)
                        if (std::find(a_setup.rowUids.begin(), a_setup.rowUids.end(), g.uid) != a_setup.rowUids.end()) ++left;
                if (left || removed != a_setup.rowUids.size())
                    spdlog::error("[selftest] teardown ({}) {:08X}: removed {} of {} temp row(s), {} STILL in his tables", a_why, a_follower,
                                  removed, a_setup.rowUids.size(), left);
                else
                    spdlog::info("[selftest] teardown ({}) {:08X}: {} temp row(s) removed; his own rows untouched", a_why, a_follower, removed);
            } else {
                spdlog::info("[selftest] teardown ({}) {:08X}: his record is gone ({} temp row(s) went with it)", a_why, a_follower,
                             a_setup.rowUids.size());
            }
            std::lock_guard lk(g_mx);
            if (auto it = g_runRows.find(a_follower); it != g_runRows.end()) {
                std::erase_if(it->second, [&](std::uint32_t u) {
                    return std::find(a_setup.rowUids.begin(), a_setup.rowUids.end(), u) != a_setup.rowUids.end();
                });
                if (it->second.empty()) g_runRows.erase(it);
            }
            a_setup.rowUids.clear();
        }
        // class state
        if (a_setup.haveSnap) {
            if (RestoreClass(a_follower, a_setup.snap)) {
                spdlog::info("[selftest] teardown ({}) {:08X}: class state restored exactly (equipped {}, mastered mask {:X}, unlocked mask {:X})",
                             a_why, a_follower, ClassName(a_setup.snap.equipped), a_setup.snap.mastered, a_setup.snap.unlocked);
                std::lock_guard lk(g_mx);
                std::erase_if(g_classSnaps, [&](const ClassSnap& s) { return s.token == a_token && s.follower == a_follower; });
            } else {
                spdlog::error("[selftest] teardown ({}) {:08X}: CLASS STATE NOT RESTORED (a writer refused, or the read-back differs). The restore "
                              "point stays in the ledger and is retried every 30 s; until then he is in a TEST class", a_why, a_follower);
                std::lock_guard lk(g_mx);
                for (auto& s : g_classSnaps)
                    if (s.token == a_token && s.follower == a_follower) s.token = 0;   // an orphan now: the retry road
                g_classOrphans.store(true);
            }
            a_setup.haveSnap = false;
        }
        // probes
        if (a_setup.probes & kProbeAim) {
            Config::g_aimProbe.store(a_setup.probePrev[0]);
            spdlog::info("[selftest] teardown ({}) {:08X}: probe bAimProbe restored to {}", a_why, a_follower, a_setup.probePrev[0] ? "on" : "off");
            a_setup.probes &= ~kProbeAim;
        }
        if (a_setup.probes & kProbeStorageMove) {
            Logistics::Storage::DevDisarmRoundTrip(a_follower);
            spdlog::info("[selftest] teardown ({}) {:08X}: the storage round trip disarmed", a_why, a_follower);
            a_setup.probes &= ~kProbeStorageMove;
        }
    }

    void RestoreOrphanClassSnaps() {
        std::vector<ClassSnap> mine;
        {
            std::lock_guard lk(g_mx);
            for (const auto& s : g_classSnaps)
                if (s.token == 0) mine.push_back(s);
        }
        if (mine.empty()) { g_classOrphans.store(false); return; }
        for (const auto& s : mine) {
            if (!Classes::Writable()) {
                spdlog::error("[selftest] class restore point for {:08X} (left by an interrupted run) cannot be applied: the class add-on is not "
                              "Writable() (ESL missing, bClassTree 0, or MCLS read-only) -- kept, retried", s.follower);
                continue;
            }
            const bool ok = RestoreClass(s.follower, s);
            if (ok) spdlog::info("[selftest] {:08X}: class state restored from the restore point an interrupted run left in the save "
                                 "(equipped {})", s.follower, ClassName(s.equipped));
            else spdlog::error("[selftest] {:08X}: class restore point from an interrupted run did NOT apply -- kept, retried", s.follower);
            if (ok) {
                std::lock_guard lk(g_mx);
                std::erase_if(g_classSnaps, [&](const ClassSnap& x) { return x.token == 0 && x.follower == s.follower; });
            }
        }
        std::lock_guard lk(g_mx);
        bool any = false;
        for (const auto& s : g_classSnaps) any = any || s.token == 0;
        g_classOrphans.store(any);
    }

    bool ApplyClass(RE::FormID a_follower, const char* a_slug, std::uint8_t a_level, bool a_mastered, std::string& a_why) {
        if (!Classes::Writable()) { a_why = "the class add-on is not Writable() (MFO_Classes.esl + bClassTree=1 needed, MCLS not read-only)"; return false; }
        const auto* def = Classes::FindBySlug(a_slug);
        if (!def) { a_why = std::string("unknown class slug ") + a_slug; return false; }
        const std::uint8_t cap = Classes::LevelCap(def->id);
        const std::uint8_t lvl = cap ? (a_mastered ? cap : std::min<std::uint8_t>(a_level, cap)) : 0;
        auto fc = Classes::GetFollower(a_follower).value_or(Classes::FollowerClasses{});
        bool ok = Classes::SetUnlocked(a_follower, fc.unlocked | Classes::Bit(def->id));
        auto p  = fc.progress[static_cast<std::size_t>(def->id)];
        p.level = lvl;
        ok = ok && Classes::SetProgress(a_follower, def->id, p);
        if (a_mastered && cap) ok = ok && Classes::SetMastered(a_follower, fc.mastered | Classes::Bit(def->id));
        ok = ok && Classes::SetEquipped(a_follower, def->id);
        const auto now = Classes::GetFollower(a_follower);
        Followers::RepublishActiveMirror();   // C10 HOOK (Classes.h): the equip changed; WORKER
        ok = ok && now && now->equipped == static_cast<std::uint8_t>(def->id) && now->progress[static_cast<std::size_t>(def->id)].level == lvl;
        if (!ok) { a_why = std::format("a Classes writer refused or the read-back differs for {} L{}", a_slug, static_cast<int>(lvl)); return false; }
        spdlog::info("[selftest] setup {:08X}: TEMP CLASS {} L{}{} applied through the C5c writers (restored when the run ends)", a_follower,
                     a_slug, static_cast<int>(lvl), a_mastered ? " mastered" : "");
        return true;
    }

    // MAIN THREAD. The refusal reasons are the brief's: never in a town / inhabited cell, never near essential or non-hostile NPCs.
    const char* SpawnUnsafeReason(RE::Actor* a_follower) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !a_follower) return "no player / follower";
        if (player->IsInCombat()) return "the player is already in a fight";
        if (LocationTypes::Classify(player->GetCurrentLocation()) == LocationTypes::Kind::kCivilised)
            return "this is an inhabited place (town / inn / house / farm): no hostile is spawned here";
        // The returned pointer is a function-local static buffer: MAIN THREAD only (both callers are MainThread::Post bodies),
        // valid until the next call. A blocker is named by display name + FormID so the log says WHO refused the spawn.
        static std::string s_why;
        bool blocked = false;
        constexpr float kNear2 = 3500.0f * 3500.0f;
        constexpr RE::FormID kCurrentFollowerFaction = 0x0005C84E;   // vanilla Skyrim.esm CurrentFollowerFaction
        auto* cff = RE::TESForm::LookupByID<RE::TESFaction>(kCurrentFollowerFaction);
        const auto playerMountPtr = player->QLastRiddenMount().get();
        RE::Actor* playerMount = playerMountPtr.get();   // the horse the player last rode
        // The PARTY never blocks (followers are essential by design): the player, the test follower, tracked followers,
        // teammates and CurrentFollowerFaction members.
        auto isParty = [&](RE::Actor* x) {
            return x == player || x == a_follower || Followers::IsTrackedFast(x->GetFormID()) || x->IsPlayerTeammate() ||
                   (cff && x->IsInFaction(cff));
        };
        auto nameOf = [](RE::Actor* x) { const char* n = x->GetName(); return std::string(n && *n ? n : "(unnamed)"); };
        // Diagnosis detail for the refusal line (a blocker the player cannot see is the field suspect): base, 3D, teammate, tracked, distance, cell.
        auto detail = [&](RE::Actor* x) {
            const auto* base = x->GetBaseObject();
            const auto* cell = x->GetParentCell();
            return std::format("base {:08X} 3D={} teammate={} tracked={} dist={:.0f} cell {:08X}", base ? base->GetFormID() : 0u,
                               x->Is3DLoaded(), x->IsPlayerTeammate(), Followers::IsTrackedFast(x->GetFormID()),
                               x->GetPosition().GetDistance(a_follower->GetPosition()), cell ? cell->GetFormID() : 0u);
        };
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            pl->ForEachHighActor([&](RE::Actor* a) {
                if (!a || a->IsDead() || a->IsDisabled() || isParty(a) || APMFBridge::IsSubstrateAnchor(a->GetFormID())) return RE::BSContainer::ForEachResult::kContinue;
                // summons / reanimated of the player or a party member
                if (auto cmd = a->GetCommandingActor(); cmd && isParty(cmd.get())) return RE::BSContainer::ForEachResult::kContinue;
                // the player's last-ridden mount, or any mount a party member sits on
                if (a == playerMount) return RE::BSContainer::ForEachResult::kContinue;
                if (a->IsAMount()) {
                    RE::NiPointer<RE::Actor> rider;
                    if (a->GetMountedBy(rider) && rider && isParty(rider.get())) return RE::BSContainer::ForEachResult::kContinue;
                }
                if (a->GetPosition().GetSquaredDistance(a_follower->GetPosition()) > kNear2) return RE::BSContainer::ForEachResult::kContinue;
                if (a->IsEssential() || a->IsProtected()) {
                    s_why = std::format("an essential / protected NPC is within 3500 units: '{}' {:08X} ({})", nameOf(a), a->GetFormID(), detail(a));
                    blocked = true;
                    return RE::BSContainer::ForEachResult::kStop;
                }
                if (!a->IsHostileToActor(player)) {
                    s_why = std::format("a non-hostile NPC is within 3500 units (inhabited area): '{}' {:08X} ({})", nameOf(a), a->GetFormID(), detail(a));
                    blocked = true;
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        return blocked ? s_why.c_str() : nullptr;
    }

}
