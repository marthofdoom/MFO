// EngageOnSight.cpp -- the hidden out-of-combat gambit "nearest visible enemy"
// (ClickUp 86e3errnu). Contract, threads and the no-loop rule: EngageOnSight.h.
// Harbinger side: Docs/INTEGRATION.md "Starting a fight" (ch.21) and "The recipe:
// enter combat + pin"; MFO's ch.21 claim table is apmf/CombatEntry.cpp.
#include "PCH.h"
#include "EngageOnSight.h"
#include "apmf/APMFBridge.h"      // ch.21 entry table (CombatEntryOffered / RequestCombatEntry / ...)
#include "Config.h"
#include "Confidence.h"           // LeashRadius (the enemy must be inside it, from the player) / Of
#include "MainThread.h"
#include "Sightline.h"            // MeasureNow -- the SEES test, on the main thread
#include "Targeting.h"            // the ch.20 pin (or the latch) for the same target
#include "logistics/Logistics.h"  // ReleaseTravelOnCombat -- the engage ends his loot trip

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MFO::EngageOnSight {

    namespace {

        using Clock = std::chrono::steady_clock;

        // One probe per follower at most this often. Sightline's own per-viewer repost
        // floor (kRepostSeconds) -- the scan and up to kMaxMeasured sightline
        // measurements are bounded to the same ~3 Hz per follower the combat
        // selectors' LoS already runs at, not the 133 ms pump.
        constexpr float kProbeFloorSecs = 0.3f;
        // Nearest-first, the sightline is measured for at most this many non-given-up
        // candidates per probe (the first VISIBLE one wins). Each measurement is one
        // engine HasLineOfSight plus, only on a CLEAR, MFO's 3-sample ray.
        constexpr int kMaxMeasured = 3;
        // Status lines (why the gambit is not acting) are transition-only (#79) with
        // this floor between two lines for one follower, so a gate that flaps (the
        // player crouching, confidence at the floor) cannot flood the log.
        constexpr float kStatusFloorSecs = 5.0f;

        // ── MAIN-THREAD PROBE MIRROR ─────────────────────────────────────────────
        // Written by the main-thread probe, read by the worker. A real lock (#4:
        // cross-thread); a LEAF -- nothing is called while it is held.
        struct ProbeResult {
            std::uint32_t seq = 0;                  // the worker's post sequence this answers
            std::vector<RE::FormID> candidates;     // every enemy in the leash, nearest (to him) first
            RE::FormID chosen   = 0;                // nearest non-given-up candidate measured VISIBLE
            float      dSelf    = 0.0f;             // chosen: distance from him
            float      dPlayer  = 0.0f;             // chosen: distance from the player
            float      leash    = 0.0f;             // the leash the probe used
            int        measured = 0;                // sightline measurements made
            int        occluded = 0;                // of those, OCCLUDED (or unknown)
        };
        std::mutex                                   g_probeMx;
        std::unordered_map<RE::FormID, ProbeResult>  g_probes;   // guarded by g_probeMx
        // Session generation: a probe posted before a revert/load that lands after it
        // writes nothing (ClearTransientState bumps it inside the StopPump bracket).
        std::atomic<std::uint32_t>                   g_gen{ 1 };

        // ── WORKER STATE (worker-only, no lock -- #4) ────────────────────────────
        struct Note {
            std::uint32_t postedSeq   = 0;          // last probe this follower posted
            std::uint32_t consumedSeq = 0;          // last probe result acted on
            Clock::time_point lastPost{};
            // Given-up targets -> the postedSeq current when the entry ended. A target
            // leaves this map only when a probe posted AFTER that (seq > value) lands
            // without it among the candidates.
            std::unordered_map<RE::FormID, std::uint32_t> givenUp;
            std::string       status;               // last status line (transition key)
            Clock::time_point statusAt{};
        };
        std::unordered_map<RE::FormID, Note> g_notes;

        const char* NameOf(RE::Actor* a_actor) {
            const char* n = a_actor ? a_actor->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        float SecsSince(Clock::time_point a_t, Clock::time_point a_now) {
            if (a_t.time_since_epoch().count() == 0) return 1.0e9f;
            return std::chrono::duration<float>(a_now - a_t).count();
        }

        // Transition-only status line, floored per follower. An EMPTY status resets
        // the key silently (the toggle off is not news).
        void Status(Note& a_note, RE::Actor* a_f, RE::FormID a_id, const std::string& a_status, Clock::time_point a_now) {
            if (a_status == a_note.status) return;
            if (a_status.empty()) { a_note.status.clear(); return; }
            if (SecsSince(a_note.statusAt, a_now) < kStatusFloorSecs) return;   // keep the old key: re-tried next lap
            a_note.status   = a_status;
            a_note.statusAt = a_now;
            spdlog::info("[engage-on-sight] {} ({:08X}): {}", NameOf(a_f), a_id, a_status);
        }

        const char* PinOutcomeName(Targeting::CommandOutcome a_o) {
            switch (a_o) {
            case Targeting::CommandOutcome::Changed:     return "pinned";
            case Targeting::CommandOutcome::Unchanged:   return "already pinned";
            case Targeting::CommandOutcome::Suppressed:  return "suppressed (Harbinger ended a pin on this foe)";
            default:                                     return "unavailable";
            }
        }

        // MAIN THREAD ONLY (posted). The highActorHandles walk (resized by the main
        // thread, §0.30) and the sightline measurement (a physics query, §0.30) both
        // run here. Everything crosses back as FormIDs.
        void RunProbe(RE::FormID a_id, std::uint32_t a_seq, std::uint32_t a_gen, float a_leash,
                      const std::vector<RE::FormID>& a_givenUp) {
            if (g_gen.load(std::memory_order_relaxed) != a_gen) return;
            auto* self = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* pl   = RE::ProcessLists::GetSingleton();
            if (!self || self->IsDead() || !self->Is3DLoaded() || !pc || !pl) return;   // does not land
            const auto selfPos = self->GetPosition();
            const auto pcPos   = pc->GetPosition();

            std::vector<std::pair<float, RE::FormID>> found;
            for (auto& hh : pl->highActorHandles) {
                auto  ptr = hh.get();          // HOLD the NiPointer
                auto* a   = ptr.get();
                if (!a || a == self) continue;
                if (a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                if (a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) continue;
                if (a->GetPosition().GetDistance(pcPos) > a_leash) continue;   // the leash is from the PLAYER
                if (!a->IsHostileToActor(pc) && !a->IsHostileToActor(self)) continue;
                found.emplace_back(a->GetPosition().GetDistance(selfPos), a->GetFormID());
            }
            std::sort(found.begin(), found.end());

            ProbeResult r;
            r.seq   = a_seq;
            r.leash = a_leash;
            r.candidates.reserve(found.size());
            for (const auto& [d, fid] : found) r.candidates.push_back(fid);
            for (const auto& [d, fid] : found) {
                if (r.measured >= kMaxMeasured) break;
                if (std::find(a_givenUp.begin(), a_givenUp.end(), fid) != a_givenUp.end()) continue;
                ++r.measured;
                if (Sightline::MeasureNow(a_id, fid) != Sightline::Verdict::Visible) { ++r.occluded; continue; }
                auto* t = RE::TESForm::LookupByID<RE::Actor>(fid);
                r.chosen  = fid;
                r.dSelf   = d;
                r.dPlayer = t ? t->GetPosition().GetDistance(pcPos) : 0.0f;
                break;
            }

            std::lock_guard lk(g_probeMx);
            if (g_gen.load(std::memory_order_relaxed) != a_gen) return;
            auto& slot = g_probes[a_id];
            if (slot.seq >= a_seq && slot.seq != 0) return;   // an older post landing late
            slot = std::move(r);
        }

        void PostProbe(Note& a_note, RE::FormID a_id, float a_leash, Clock::time_point a_now) {
            if (SecsSince(a_note.lastPost, a_now) < kProbeFloorSecs) return;
            a_note.lastPost = a_now;
            const std::uint32_t seq = ++a_note.postedSeq;
            const std::uint32_t gen = g_gen.load(std::memory_order_relaxed);
            std::vector<RE::FormID> givenUp;
            givenUp.reserve(a_note.givenUp.size());
            for (const auto& [fid, s] : a_note.givenUp) givenUp.push_back(fid);
            // Capture by value: FormIDs and numbers only (the frame re-resolves).
            MainThread::Post([a_id, seq, gen, a_leash, givenUp = std::move(givenUp)]() {
                RunProbe(a_id, seq, gen, a_leash, givenUp);
            });
        }

    }

    bool Service(RE::Actor* a_f, RE::FormID a_id, bool a_retreatCooling, float a_minConfidence) {
        if (!a_f || !a_id) return false;
        const auto now = Clock::now();
        auto& note = g_notes[a_id];

        // 1) HIS ENTRY, if he holds one. Runs whatever the gates say, so an entry the
        //    Harbinger sweep ended is always recorded as given up.
        {
            RE::FormID et = 0;
            const auto st = APMFBridge::CombatEntryStateOf(a_id, &et);
            if (st == APMFBridge::EntryState::Standing) return true;   // pending: his lap, no logistics
            if (st == APMFBridge::EntryState::Ended) {
                note.givenUp[et] = note.postedSeq;
                // The recipe drops both handles. Clear the pin only if it is still OURS
                // (still on this target): once he fought, his gambits may have moved it.
                bool pinCleared = false;
                if (auto cur = Targeting::Current(a_id).get(); cur && cur->GetFormID() == et) {
                    Targeting::Clear(a_id);
                    pinCleared = true;
                }
                APMFBridge::ForgetCombatEntry(a_id);
                spdlog::info("[engage-on-sight] {} ({:08X}): entry on {:08X} is over -- target given up until it "
                             "leaves his candidates{}", NameOf(a_f), a_id, et,
                             pinCleared ? "; its pin released" : "");
            }
        }

        // 2) GATES. Off is silent; every other stand-down is a transition line.
        if (!Config::g_engageOnSight.load()) { Status(note, a_f, a_id, {}, now); return false; }
        if (REL::Module::IsVR()) {
            Status(note, a_f, a_id, "inert: VR (Harbinger serves no ch.21 there, and MFO has no direct road)", now);
            return false;
        }
        if (!Config::g_autoRetreat.load()) {
            Status(note, a_f, a_id, "standing down: bAutoRetreat is OFF (the gambit runs only while a follower "
                                    "who starts a fight can break it off)", now);
            return false;
        }
        if (!APMFBridge::CombatEntryOffered()) {
            Status(note, a_f, a_id, "inert: Harbinger ch.21 combat entry unavailable (Harbinger absent, older "
                                    "than ABI 14, or its ch.21 seat refused) -- MFO has no direct road", now);
            return false;
        }
        if (!Config::g_engageOnSightSneaking.load()) {
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsSneaking()) {
                Status(note, a_f, a_id, "standing down: you are sneaking (bEngageOnSightSneaking is OFF)", now);
                return false;
            }
        }
        if (a_retreatCooling) {
            Status(note, a_f, a_id, "standing down: auto-retreat cooldown", now);
            return false;
        }
        const float conf = Confidence::Of(a_f);
        if (conf < a_minConfidence) {
            Status(note, a_f, a_id, fmt::format("standing down: confidence {:.2f} is under the retreat floor "
                                                "{:.2f} (he would only start a fight to flee it)",
                                                conf, a_minConfidence), now);
            return false;
        }
        Status(note, a_f, a_id, "armed: watching for a visible enemy inside the leash", now);

        // 3) THE NEWEST PROBE RESULT (the one answering his last post), once.
        ProbeResult r;
        bool have = false;
        {
            std::lock_guard lk(g_probeMx);
            if (auto it = g_probes.find(a_id);
                it != g_probes.end() && it->second.seq == note.postedSeq && it->second.seq > note.consumedSeq) {
                r    = it->second;
                have = true;
            }
        }
        bool engaged = false;
        if (have) {
            note.consumedSeq = r.seq;
            // A given-up target leaves the set once a probe posted AFTER it was given up
            // no longer lists it (out of the leash, dead, disabled, unloaded, no longer
            // hostile). Seeing it again is a FRESH sighting.
            for (auto it = note.givenUp.begin(); it != note.givenUp.end();) {
                const bool listed = std::find(r.candidates.begin(), r.candidates.end(), it->first) != r.candidates.end();
                if (r.seq > it->second && !listed) {
                    spdlog::info("[engage-on-sight] {} ({:08X}): {:08X} left his candidates -- no longer given up",
                                 NameOf(a_f), a_id, it->first);
                    it = note.givenUp.erase(it);
                } else {
                    ++it;
                }
            }
            if (r.chosen && !note.givenUp.contains(r.chosen)) {
                auto* t = RE::TESForm::LookupByID<RE::Actor>(r.chosen);
                if (t && !t->IsDead() && !t->IsDisabled()) {
                    std::uint32_t h = 0;
                    switch (APMFBridge::RequestCombatEntry(a_id, r.chosen, &h)) {
                    case APMFBridge::EntryResult::Filed: {
                        // The recipe's second handle: pin the same target (ch.20 waits until the
                        // entry has made it a combat-group target). Through Targeting, so there is
                        // ONE pin per follower and his foe gambits re-point it once he fights.
                        const char* pin = "off (bCommandTarget)";
                        if (Targeting::Commandable())
                            pin = PinOutcomeName(Targeting::CommandEx(a_id, t->GetHandle()));
                        // Preempt logistics the way player combat does: his loot trip ends now.
                        Logistics::ReleaseTravelOnCombat(a_f);
                        spdlog::info("[engage-on-sight] {} ({:08X}) ENGAGE {} ({:08X}): nearest visible enemy, "
                                     "{:.0f}u from him, {:.0f}u from you (leash {:.0f}u), sightline=VISIBLE "
                                     "({} measured, {} not visible), {} candidate(s); ch.21 entry h={}; "
                                     "ch.20 pin: {}; confidence {:.2f}",
                                     NameOf(a_f), a_id, NameOf(t), r.chosen, r.dSelf, r.dPlayer, r.leash,
                                     r.measured, r.occluded, r.candidates.size(), h, pin, conf);
                        engaged = true;
                        break;
                    }
                    case APMFBridge::EntryResult::Standing:
                        engaged = true;   // cannot happen (checked above); treat as pending
                        break;
                    case APMFBridge::EntryResult::SeatAbsent:   // logged once by the bridge
                    case APMFBridge::EntryResult::Invalid:
                    default:
                        break;
                    }
                }
            }
        }
        if (engaged) return true;

        // 4) THE NEXT PROBE. The leash is read here, on the worker, exactly as the loot
        //    leash is (Confidence::LeashRadius -- out of combat it is vitality only).
        PostProbe(note, a_id, Confidence::LeashRadius(a_f), now);
        return false;
    }

    void Forget(RE::FormID a_id) {
        APMFBridge::ForgetCombatEntry(a_id);
        g_notes.erase(a_id);
        std::lock_guard lk(g_probeMx);
        g_probes.erase(a_id);
    }

    void ClearTransientState() {
        g_gen.fetch_add(1, std::memory_order_relaxed);
        g_notes.clear();
        std::lock_guard lk(g_probeMx);
        g_probes.clear();
    }

}
