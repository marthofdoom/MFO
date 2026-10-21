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
// ObjectReference.MoveTo through Papyrus::DispatchMoveTo, a short distance IN FRONT of the player's heading (run 4: behind put one on a gate). The move is POSTED to
// the main thread and re-checks there. Only an UNLOADED follower is ever moved, and each follower is finished (moved,
// seen arriving, or errand-owned) at most once per transition.
//
// IS IT A REAL TRANSITION? A menu pause (> 2.5 s worker gap) also stamps the marker. So a quiet sample (player place
// key + position + the followers loaded with the player) is kept while no mark is fresh; a new mark acts only if the
// place key changed or the player moved > kNearU from that sample, and only followers in the sampled loaded set are
// judged. After a save load / revert there is no sample (Reset), so nobody is moved.
//
// "ARRIVED WITH THE PLAYER" = he is in the player's cell (interior or exterior), or both are outdoors in the same
// worldspace within kNearU, EVEN IF his 3D is not loaded yet (run 4: the engine had already placed him; a second MoveTo
// from the stale position dropped him on a gate). Anything else is left behind.
//
// NEVER MOVED: not an eligible teammate (dismissed, dead, disabled, summon-not-allowed), told to wait, MFO-toggled
// off, in combat, a horse / mount or mounted, or holding a live errand (physical, off-screen Sim record, inn stay: Errand::Active). The errand
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
        constexpr float kFrontU  = 200.0f;    // MoveTo offset IN FRONT of the player's heading (marth 2026-10-20; Errand RECALL's 250 u behind is separate)

        float                                  g_lastSince = -1.0f;   // detects a NEW transition (the value fell)
        std::unordered_set<RE::FormID>         g_done;                // finished this transition (moved, arrived, or owned by an errand)
        bool                                   g_decided   = false;   // the "was it a real transition" verdict was taken this transition

        // The QUIET pre-transition sample (F1): updated only while no transition mark is fresh (since > kSettleS), frozen into g_pre
        // when a new transition is seen. A menu pause (> kRefreshGapS worker gap) stamps a "transition" too, but the place and the
        // loaded set did not change, so it is told apart from fast travel / a door / a teleport by comparing against this sample.
        struct Sample { bool valid = false; const void* key = nullptr; RE::NiPoint3 pos{}; std::unordered_set<RE::FormID> loaded; };
        Sample g_sample, g_pre;
        std::set<std::pair<RE::FormID, int>>   g_skipLogged;          // (follower, reason) logged this transition

        enum Skip { kNotEligible = 0, kMfoOff, kWaiting, kCombat, kErrand, kMount };

        void SkipOnce(RE::FormID a_id, Skip a_why, const char* a_name, const char* a_text) {
            if (!g_skipLogged.insert({ a_id, static_cast<int>(a_why) }).second) return;
            spdlog::info("[catchup] {:08X} '{}': not moved after the world transition -- {}", a_id, a_name, a_text);
        }

        // Interior cell, else worldspace: the same place key Followers::Refresh uses for its transition mark.
        const void* PlaceKey(RE::PlayerCharacter* a_pc) {
            auto* cell = a_pc->GetParentCell();
            return (cell && cell->IsInteriorCell()) ? static_cast<const void*>(cell) : static_cast<const void*>(a_pc->GetWorldspace());
        }

        // With the player: he is in the player's cell, or both are outdoors in the same worldspace within kNearU. His 3D need NOT be loaded.
        bool WithPlayer(RE::Actor* a, RE::PlayerCharacter* a_pc) {
            auto* cell = a->GetParentCell();
            auto* pcell = a_pc->GetParentCell();
            if (!cell || !pcell) return false;
            if (cell == pcell) return true;
            if (cell->IsInteriorCell() || pcell->IsInteriorCell() || a->GetWorldspace() != a_pc->GetWorldspace()) return false;
            return a->GetPosition().GetDistance(a_pc->GetPosition()) <= kNearU;
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
            if (f->Is3DLoaded()) return;   // only an UNLOADED follower is ever moved (RecallToPlayer's precedent): the engine has him now
            if (WithPlayer(f, pc)) return; // the engine already placed him in the player's cell / near him (3D not up yet): do not move him again
            const float az = pc->GetAngleZ();
            const float dx = kFrontU * std::sin(az), dy = kFrontU * std::cos(az);
            const bool  ok = Papyrus::DispatchMoveTo(f, pc, dx, dy, 0.0f);
            spdlog::info("[catchup] {:08X} '{}' left behind after a world transition ({:.0f} s ago, {}) -- moved to the player "
                         "(+{:.0f},{:.0f}) {}", a_id, f->GetName() ? f->GetName() : "?", a_since, a_where, dx, dy,
                         ok ? "dispatched" : "FAILED to dispatch");
        }
    }

    void Reset() {
        g_lastSince = -1.0f; g_done.clear(); g_skipLogged.clear(); g_decided = false; g_sample = Sample{}; g_pre = Sample{};
    }

    void Poll() {
        if (!Config::g_followerCatchUp.load()) return;
        const float since = Followers::SecondsSinceTransition();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (since >= 0.0f && (g_lastSince < 0.0f || since < g_lastSince)) {   // a new transition mark
            g_done.clear(); g_skipLogged.clear(); g_decided = false;
            g_pre = g_sample;   // the quiet state from BEFORE the mark (the sample only updates while since > kSettleS)
        }
        g_lastSince = since;
        if (!pc || !pc->Is3DLoaded()) return;

        if (since < 0.0f || since > kSettleS) {   // quiet: refresh the pre-transition sample
            if (!Menu()) {
                g_sample.valid = true; g_sample.key = PlaceKey(pc); g_sample.pos = pc->GetPosition(); g_sample.loaded.clear();
                for (const auto& [id, st] : g_followers) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                    if (a && !a->IsPlayerRef() && WithPlayer(a, pc)) g_sample.loaded.insert(id);
                }
            }
        }
        if (since < kSettleS || since > kWindowS) return;
        if (Menu() || !MainThread::IsInstalled()) return;   // retry on the next poll

        if (!g_decided) {
            g_decided = true;
            if (!g_pre.valid) {
                spdlog::info("[catchup] world transition seen ({:.0f} s ago) but there is no pre-transition sample (save load / first session) -- nobody moved", since);
                g_pre.loaded.clear();
            } else if (PlaceKey(pc) == g_pre.key && pc->GetPosition().GetDistance(g_pre.pos) <= kNearU) {
                spdlog::info("[catchup] transition mark ({:.0f} s ago) but the player's place and position did not change (a menu pause) -- nobody moved", since);
                g_pre.loaded.clear();
            }
        }
        if (g_pre.loaded.empty()) return;

        for (const auto& [id, st] : g_followers) {
            if (g_done.contains(id) || !g_pre.loaded.contains(id)) continue;   // only a follower who was loaded WITH the player before
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsPlayerRef()) continue;
            const char* name = a->GetName() ? a->GetName() : "?";
            if (!Followers::IsEligibleFollower(a)) { SkipOnce(id, kNotEligible, name, "not an eligible teammate (dismissed / dead / disabled)"); continue; }
            if (!st.mfoEnabled)                    { SkipOnce(id, kMfoOff, name, "MFO is toggled off for him"); continue; }
            if (a->IsAMount() || a->HasKeywordString("ActorTypeHorse")) { g_done.insert(id); continue; }   // a horse / mount is never moved
            if (a->IsOnMount())                    { SkipOnce(id, kMount, name, "he is mounted"); continue; }
            auto* avo = a->AsActorValueOwner();
            if (avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) { SkipOnce(id, kWaiting, name, "told to wait"); continue; }
            if (Logistics::Errand::Active(id)) {   // the errand code owns his return (ErrandSim RECALL): done for this transition even after the record drops
                g_done.insert(id);
                SkipOnce(id, kErrand, name, "he holds a live errand (the errand code owns his return)");
                continue;
            }

            if (WithPlayer(a, pc)) { g_done.insert(id); continue; }   // with the player (3D loaded or not yet): the engine has him
            if (a->Is3DLoaded()) continue;   // loaded elsewhere = not ours to move
            if (APMFBridge::InCombat(a)) { SkipOnce(id, kCombat, name, "he is in combat"); continue; }

            auto*             cell = a->GetParentCell();
            const char*       cn   = cell ? cell->GetName() : nullptr;
            const std::string cellTxt = cn && *cn ? std::string(cn) : (cell ? std::format("{:08X}", cell->GetFormID()) : std::string("none"));
            const std::string where = std::format("was in cell {}, unloaded", cellTxt);
            g_done.insert(id);
            MainThread::Post([id, since, where]() { MoveOnMain(id, since, where); });
        }
    }

}
