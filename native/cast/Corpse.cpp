// cast/Corpse.cpp -- CARD G7 "CORPSE TARGET / REANIMATE" (class plan card G7; marth 2026-11-11: G7 joins T2).
//
// PickCorpse(follower, units, spell) is the read behind the gambit selector `cond.corpse_within`: the corpse a
// "Corpse: Within -> Cast at target (a Reanimate spell)" rule should raise, or nothing.
//
// A CORPSE CANDIDATE (measured on the MAIN thread): an actor in the process lists' high list (the list the
// engine's own reanimate search walks) that is dead, loaded, not disabled or deleted, not the player and not
// a player teammate (a fallen follower is never raised), whose race carries the ActorTypeNPC keyword (the
// card's "humanoid"), within kScanUnits of the follower. Its level is read with it.
//
// THE PICK (worker): among the candidates still dead and within `units` of the follower now,
//   * with a spell on the row: skip one whose claim never fired this fight (cast/BuffRoad.cpp's hold-off, per
//     corpse) and one cast::WillWork (card G10) says the spell cannot raise (level cap, effect conditions).
//     A corpse WillWork has not measured yet is skipped for this lap (the call posts the measurement; the next
//     lap has it), so a rule never raises on a guess;
//   * the HIGHEST LEVEL corpse wins (the strongest thrall the spell can raise, the engine's own preference),
//     the nearest on a tie.
// At most kMaxConsider candidates are judged per call (highest level first), which bounds the WillWork posts.
//
// WHAT HAPPENS TO THE PICK. Fire hands it to CastOn as the cast target; a Reanimate spell takes the buff claim
// road (cast/BuffRoad.cpp) and the claim names the corpse with Harbinger's kCastFlag_CorpseTarget (ABI v42:
// the engine's own reanimate cast is aimed at it, and only if the engine's own test accepts it). On an older
// Harbinger the claim keeps target 0 and the engine picks its own corpse (the pre-G7 road); the selector then
// only decides WHETHER.
//
// THREADING (the SelfStatus mirror shape). The high list is resized by the main thread, so the scan is ONE
// MainThread::Post per follower; the worker (the Evaluator, on the AddTask job worker) reads a mutex-guarded
// mirror. Refresh at kRefreshSec, trusted for kTrustSec, a lost post re-asked after kAskRetrySec. No fresh
// measurement = no corpse (fail closed). No pump (VR): never a corpse. The mirror holds FormIDs and ages out by
// itself: every pick re-resolves the actor and re-tests dead / distance, so a stale entry cannot name a
// living actor (no co-save, no ResetAllState hook; CorpseClear exists for a future caller).
//
// FORK SYMBOLS READ (CommonLibSSE-NG mit-3.7 fork): ProcessLists::highActorHandles (ProcessLists.h:69),
// Actor::IsDead / IsPlayerTeammate / GetRace / GetLevel (Actor.h), TESObjectREFR::IsDisabled / Is3DLoaded,
// TESForm::IsDeleted, BGSKeywordForm::HasKeywordString.
#include "Actuation_internal.h"
#include "MainThread.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace MFO::Actuation {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Sizes from the real cadence (principle 9): the Evaluator asks once per follower per service lap
        // (133 ms x party, ~1.3 s for ten). A corpse does not move and stays a corpse for minutes; a new one
        // appears when something dies. Refresh at one second, trust for five (more than three missed
        // refreshes), re-ask a lost post after three.
        constexpr double kRefreshSec  = 1.0;
        constexpr double kTrustSec    = 5.0;
        constexpr double kAskRetrySec = 3.0;
        // One cell. A row's own `units` narrows it; units 0 means this whole radius.
        constexpr float       kScanUnits   = 4096.0f;
        constexpr std::size_t kMaxSeen     = 32;   // corpses kept per follower (highest level first)
        constexpr std::size_t kMaxConsider = 8;    // corpses judged per pick

        struct Seen {
            RE::FormID    id    = 0;
            std::uint16_t level = 0;
        };
        struct Entry {
            std::vector<Seen> corpses;   // highest level first
            Clock::time_point stamp{};   // when the last MAIN-thread scan finished (epoch = never)
            Clock::time_point asked{};   // when the scan in flight was posted (epoch = none in flight)
        };
        std::mutex                            g_mx;
        std::unordered_map<RE::FormID, Entry> g_cache;

        double Age(const Clock::time_point& a_then, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_then).count();
        }

        // MAIN THREAD ONLY (the high list is resized here).
        std::vector<Seen> Scan(RE::Actor* a_follower) {
            std::vector<Seen> out;
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl) return out;
            const auto origin = a_follower->GetPosition();
            for (auto& h : pl->highActorHandles) {
                auto  ptr = h.get();   // HOLD the NiPointer
                auto* a   = ptr.get();
                if (!a || a == a_follower) continue;
                if (!a->IsDead() || a->IsDisabled() || a->IsDeleted() || !a->Is3DLoaded()) continue;
                if (a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                auto* race = a->GetRace();
                if (!race || !race->HasKeywordString("ActorTypeNPC")) continue;
                if (origin.GetDistance(a->GetPosition()) > kScanUnits) continue;
                out.push_back({ a->GetFormID(), a->GetLevel() });
            }
            std::stable_sort(out.begin(), out.end(), [](const Seen& x, const Seen& y) { return x.level > y.level; });
            if (out.size() > kMaxSeen) out.resize(kMaxSeen);
            return out;
        }

        void PostScan(RE::FormID a_fid) {
            MainThread::Post([a_fid]() {
                std::vector<Seen> corpses;
                // A LOADED follower only: an unloaded one is recorded seeing none (fail closed).
                if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid); f && f->Is3DLoaded()) corpses = Scan(f);
                std::scoped_lock lk(g_mx);
                auto& e   = g_cache[a_fid];
                e.corpses = std::move(corpses);
                e.stamp   = Clock::now();
                e.asked   = Clock::time_point{};
            });
        }

    }

    void CorpseClear() {
        std::scoped_lock lk(g_mx);
        g_cache.clear();
    }

    RE::ActorHandle PickCorpse(RE::Actor* a_follower, float a_units, RE::FormID a_spell) {
        RE::ActorHandle best;
        if (!a_follower || !MainThread::IsInstalled()) return best;   // VR: no pump, no read (never a corpse)
        const RE::FormID fid = a_follower->GetFormID();
        const auto       now = Clock::now();
        std::vector<Seen> seen;
        bool ask = false;
        {
            std::scoped_lock lk(g_mx);
            auto& e = g_cache[fid];
            const bool measured = e.stamp.time_since_epoch().count() != 0;
            if (measured && Age(e.stamp, now) <= kTrustSec) seen = e.corpses;
            const bool stale    = !measured || Age(e.stamp, now) >= kRefreshSec;
            const bool inFlight = e.asked.time_since_epoch().count() != 0 && Age(e.asked, now) < kAskRetrySec;
            if (stale && !inFlight) { e.asked = now; ask = true; }
        }
        if (ask) PostScan(fid);   // outside the mutex: Post takes its own queue lock
        if (seen.empty()) return best;

        auto* spell = a_spell ? RE::TESForm::LookupByID<RE::SpellItem>(a_spell) : nullptr;
        const float reach  = a_units > 0.0f ? (std::min)(a_units, kScanUnits) : kScanUnits;
        const auto  origin = a_follower->GetPosition();
        std::uint16_t bestLevel = 0;
        float         bestDist  = 0.0f;
        std::size_t   judged    = 0;
        for (const auto& s : seen) {   // highest level first
            if (best && s.level < bestLevel) break;   // nothing below can beat the pick
            auto* a = RE::TESForm::LookupByID<RE::Actor>(s.id);
            // Re-tested now: the mirror is up to kTrustSec old, and a corpse may have been raised since.
            if (!a || !a->IsDead() || a->IsDisabled() || a->IsPlayerTeammate()) continue;
            const float d = origin.GetDistance(a->GetPosition());
            if (d > reach) continue;
            if (spell) {
                if (judged++ >= kMaxConsider) break;
                if (BuffCorpseHeldOff(fid, s.id, a_spell)) continue;
                if (WillWork(a_follower, spell, a).verdict != WillWorkVerdict::Yes) continue;   // No, or not measured yet
            }
            if (!best || d < bestDist) {
                best      = a->GetHandle();
                bestLevel = s.level;
                bestDist  = d;
            }
        }
        return best;
    }

}
