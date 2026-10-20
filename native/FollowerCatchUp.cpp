// FollowerCatchUp.cpp -- "followers stop getting left behind" (marth 2026-10-20: "Sven seems to like not to fast
// travel with me sometimes ... we can stop it").
//
// WHY. The engine brings a follower along on a world transition (fast travel, load door, teleport) only if he is
// near the player and on his follow package at that moment. A follower mid-loot, mid-turn or just out of reach
// stays behind, unloaded, and the player has to walk back for him.
//
// WHAT. After Followers::Refresh's world-transition marker (LoadingMenu, worker gap, interior / worldspace change,
// teleport jump; Followers::SecondsSinceTransition) has been quiet for kSettleS (the engine's own carry gets the
// first chance), every roster follower who did NOT arrive with the player is put beside the player by the SAME
// mechanism the town errand RECALL uses (logistics/Errand.cpp RecallToPlayer): the vanilla Papyrus
// ObjectReference.MoveTo through Papyrus::DispatchMoveTo, ~250 u behind the player's heading. The move is POSTED to
// the main thread and re-checks there. Each follower is moved at most once per transition.
//
// "ARRIVED WITH THE PLAYER" = his 3D is loaded AND he is in the player's cell (interior or exterior), or both are
// outdoors in the same worldspace within kNearU. Anything else is left behind.
//
// NEVER MOVED: not an eligible teammate (dismissed, dead, disabled, summon-not-allowed), told to wait, MFO-toggled
// off, in combat, or holding a live errand (physical, off-screen Sim record, inn stay: Errand::Active). The errand
// code owns that follower's return: when the player leaves the town, ErrandSim RECALLs him itself, so this file
// stands aside for the whole life of the record and only ONE mover ever acts. A menu or loading screen means
// "retry on the next poll". Each skip reason is logged once per follower per transition.
//
// THREAD. Poll is on the Followers::Refresh domain (reads g_followers like Refresh does); the MoveTo is posted to
// the main thread. Session-only state, self-resetting when a new transition is seen: nothing is serialized.
#include "PCH.h"
#include "FollowerCatchUp.h"
#include "Followers.h"
#include "Config.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "apmf/APMFBridge.h"
#include "logistics/Errand.h"

#include <set>
#include <unordered_set>

namespace MFO::FollowerCatchUp {

    namespace {
        constexpr float kSettleS = 3.0f;      // real seconds after the last transition mark before anyone is judged
        constexpr float kWindowS = 60.0f;     // stop judging this long after the transition (a later gap is a choice)
        constexpr float kNearU   = 4096.0f;   // same-worldspace "with the player" bar (= Followers' teleport jump)
        constexpr float kBehindU = 250.0f;    // MoveTo offset behind the player's heading (RecallToPlayer's kBehind)

        float                                  g_lastSince = -1.0f;   // detects a NEW transition (the value fell)
        std::unordered_set<RE::FormID>         g_done;                // moved this transition
        std::set<std::pair<RE::FormID, int>>   g_skipLogged;          // (follower, reason) logged this transition

        enum Skip { kNotEligible = 0, kMfoOff, kWaiting, kCombat, kErrand };

        void SkipOnce(RE::FormID a_id, Skip a_why, const char* a_name, const char* a_text) {
            if (!g_skipLogged.insert({ a_id, static_cast<int>(a_why) }).second) return;
            spdlog::info("[catchup] {:08X} '{}': not moved after the world transition -- {}", a_id, a_name, a_text);
        }

        bool Menu() {
            auto* ui = RE::UI::GetSingleton();
            return !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }

        // The posted half (MAIN thread): re-resolve, re-check, MoveTo.
        void MoveOnMain(RE::FormID a_id, float a_since, std::string a_where) {
            auto* f  = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!f || !pc || f->IsDead() || f->IsDisabled() || !pc->Is3DLoaded() || Menu()) return;
            if (f->Is3DLoaded() && f->GetParentCell() == pc->GetParentCell()) return;   // the engine got him there meanwhile
            const float az = pc->GetAngleZ();
            const float dx = -kBehindU * std::sin(az), dy = -kBehindU * std::cos(az);
            const bool  ok = Papyrus::DispatchMoveTo(f, pc, dx, dy, 0.0f);
            spdlog::info("[catchup] {:08X} '{}' left behind after a world transition ({:.0f} s ago, {}) -- moved to the player "
                         "(+{:.0f},{:.0f}) {}", a_id, f->GetName() ? f->GetName() : "?", a_since, a_where, dx, dy,
                         ok ? "dispatched" : "FAILED to dispatch");
        }
    }

    void Poll() {
        if (!Config::g_followerCatchUp.load()) return;
        const float since = Followers::SecondsSinceTransition();
        if (g_lastSince < 0.0f || since < g_lastSince) { g_done.clear(); g_skipLogged.clear(); }   // a new transition (or the first)
        g_lastSince = since;
        if (since < kSettleS || since > kWindowS) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || !pc->Is3DLoaded() || Menu() || !MainThread::IsInstalled()) return;   // retry on the next poll

        auto*      pcell = pc->GetParentCell();
        auto*      pws   = pc->GetWorldspace();
        const auto ppos  = pc->GetPosition();
        for (const auto& [id, st] : g_followers) {
            if (g_done.contains(id)) continue;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsPlayerRef()) continue;
            const char* name = a->GetName() ? a->GetName() : "?";
            if (!Followers::IsEligibleFollower(a)) { SkipOnce(id, kNotEligible, name, "not an eligible teammate (dismissed / dead / disabled)"); continue; }
            if (!st.mfoEnabled)                    { SkipOnce(id, kMfoOff, name, "MFO is toggled off for him"); continue; }
            auto* avo = a->AsActorValueOwner();
            if (avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) { SkipOnce(id, kWaiting, name, "told to wait"); continue; }
            if (Logistics::Errand::Active(id))     { SkipOnce(id, kErrand, name, "he holds a live errand (the errand code owns his return)"); continue; }

            // Arrived with the player?
            auto*       cell   = a->GetParentCell();
            const bool  loaded = a->Is3DLoaded();
            bool        with   = false;
            std::string where;
            if (loaded && cell && cell == pcell) with = true;
            else if (loaded && cell && pcell && !cell->IsInteriorCell() && !pcell->IsInteriorCell() &&
                     a->GetWorldspace() == pws) {
                const float d = a->GetPosition().GetDistance(ppos);
                with = d <= kNearU;
                where = std::format("{:.0f} units away", d);
            }
            if (with) continue;
            if (where.empty()) {
                const char* cn = cell ? cell->GetName() : nullptr;
                const std::string cellTxt = cn && *cn ? std::string(cn) : (cell ? std::format("{:08X}", cell->GetFormID()) : std::string("none"));
                where = std::format("was in cell {}{}", cellTxt, loaded ? "" : ", unloaded");
            }
            if (APMFBridge::InCombat(a)) { SkipOnce(id, kCombat, name, "he is in combat"); continue; }

            g_done.insert(id);
            MainThread::Post([id, since, where]() { MoveOnMain(id, since, where); });
        }
    }

}
