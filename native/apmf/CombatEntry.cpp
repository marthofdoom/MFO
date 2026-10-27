// apmf/CombatEntry.cpp -- the ch.21 combat-entry claim table (ABI v14,
// kIntent_CombatEntry): MFO's client side of Harbinger's "Starting a fight".
// Its one caller is the engage-on-sight gambit (native/EngageOnSight.cpp,
// ClickUp 86e3errnu). A new claim family, so its own file in apmf/ (CLAUDE.md
// "a new mechanism is a new file in its subsystem"); the shape mirrors the
// ch.20 pin table in apmf/Excursion.cpp. Tick() / ClearTransientState()
// (apmf/Bridge.cpp) reach it through APMFBridge_internal.h.
//
// Contract: APMF_API.h kIntent_CombatEntry; Harbinger Docs/INTEGRATION.md "Starting a
// fight" / "How combat entry ends". One claim per follower. MFO never Repoints it: a new
// target is a NEW request (INTEGRATION.md: "To try again, send a NEW RequestEx. An ended
// handle is dead"), and an ended entry is only ever forgotten here, never re-filed.
// An entry may also end because Harbinger's held-engagement rules fired (range drop, no path progress,
// arrived but not fighting). Never a synchronous refusal. Engage-on-sight is an aggressive setting: MFO
// adds no waiting of its own (marth 2026-10-12), the given-up set in EngageOnSight is the only no-loop rule.
//
// ABI v31 (marth 2026-10-27): Harbinger says WHY it ended a ch.20 pin or a ch.21 entry (GetClaimEndReason).
// A NOT ENGAGED end (kClaimEnd_NotEngaged, or kClaimEnd_NoPathProgress, which Harbinger counts as not
// engaged) makes MFO GIVE THAT TARGET UP for that follower at the moment the end is noticed, whatever the
// party's combat state: the pin table does not re-pin it and engage-on-sight does not enter it. The give-up
// lifts on STATE only, never on a timer ("no waiting ever"): the target moved materially from where it was
// given up, or it BECOMES hostile to the party or starts fighting a party member (a rising edge, so a target
// still facing him when it was given up does not lift it at once). The registry lives here, beside the
// entry table, because both tables feed it. Below ABI 31 nothing is recorded: today's rules hold (the
// documented degrade, said once per session by ClaimEndReasonOf).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"   // g_engageOnSight -- the sweep's kill switch

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kEntryMinAbi = 14;   // the ABI that first serves intent 21

        // IsClaimLive reads APMF's PUBLISHED map, which a RequestEx reaches only at APMF's
        // next writer Drain (its PlayerCharacter::Update seat -- not ticking under a
        // game-pausing menu). A never-seen-live entry is judged ended only after this many
        // UNPAUSED sweeps (~2 s of running game at the pump's 133 ms). A FLOOR, not an
        // expiry (principle 9): it only delays noticing an end. Same value and reasoning
        // as the ch.20 pin table's kPinNeverLiveSweeps (apmf/Excursion.cpp).
        constexpr std::uint32_t kEntryNeverLiveSweeps = 15;

        struct CombatEntryClaim {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;   // invalid once ENDED
            RE::FormID       target   = 0;
            std::uint32_t    unpausedSweeps = 0;   // sweeps with the game running since filing
            bool             everLive = false;     // seen IsClaimLive at least once
            bool             ended    = false;     // Harbinger ended it (IsClaimLive went false)
            std::uint32_t    endReason = APMF_API::kClaimEnd_None;   // ABI v31, read when the end is noticed
        };
        std::unordered_map<RE::FormID, CombatEntryClaim> g_entries;   // guarded by g_mx

        // A synchronous refusal with valid params = the ch.21 seat is not installed (VR,
        // runtime, [CombatEntry] bCombatEntry=0, self-check). Session-stable: the seat
        // installs once at APMF's kDataLoaded, so this is never reset (a load does not
        // install it). Atomic so CombatEntryOffered() reads it without g_mx.
        std::atomic<bool> g_entrySeatRefused{ false };

        bool EntryGamePaused() {
            auto* ui = RE::UI::GetSingleton();   // the same read the pin table / Scheduler make on this worker
            return ui && ui->GameIsPaused();
        }

        // Liveness of one filed entry; marks it ENDED (and says so, once). g_mx HELD.
        bool EntryEndedLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower, CombatEntryClaim& e) {
            if (e.ended) return true;
            if (e.handle == APMF_API::kInvalidHandle) return false;
            // v6 IsClaimLive: this table only ever holds handles from an ABI >= 14 APMF.
            if (reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(e.handle)) {
                e.everLive = true;
                return false;
            }
            if (!e.everLive) {
                if (EntryGamePaused() || e.unpausedSweeps < kEntryNeverLiveSweeps) return false;
            }
            // ABI v31: read the reason before the Release (a client Release does not clear Harbinger's
            // record, but reading first keeps the order plain). Below v31 it reads kClaimEnd_None.
            e.endReason = ClaimEndReasonOf(api, e.handle);
            spdlog::info("[engage-on-sight] {:08X}: combat entry against {:08X} ENDED (h={}, {}). Harbinger "
                         "ends it on engine refusal, entry not attempted, combat ended, or a dead / disabled / "
                         "unloaded / unresolvable follower or target, the engage-range rule or its watchdogs; APMF's "
                         "'[ch.21] entry ended' line names the reason. Unless MFO sees it as a range drop (EngageOnSight), "
                         "it does not ask again for this target while it stays a candidate. End reason: {}.",
                         follower, e.target, e.handle, e.everLive ? "was live" : "never seen live",
                         ClaimEndReasonName(e.endReason));
            if (IsNotEngagedEnd(e.endReason)) RecordEngageGiveUp(follower, e.target, e.endReason, "ch.21 entry");
            // ALWAYS Release before dropping the handle: a no-op on a claim Harbinger already
            // released, and it FIFO-cancels one still pending, so a false "never live"
            // judgement can never leave an orphan claim.
            api->Release(e.handle);
            e.handle = APMF_API::kInvalidHandle;
            e.ended  = true;
            return true;
        }
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). g_mx HELD.
    void SweepCombatEntriesLocked(const APMF_API::APMF_API_v2* api) {
        if (g_entries.empty() || !api || api->abiVersion < kEntryMinAbi) return;
        // KILL SWITCH: bEngageOnSight flipped OFF mid-session forgets every entry.
        // Release STOPS NOTHING (Harbinger's contract), so a fight already begun goes on.
        if (!Config::g_engageOnSight.load()) {
            spdlog::info("[engage-on-sight] bEngageOnSight off -- releasing {} entry claim(s) "
                         "(no fight is stopped)", g_entries.size());
            ClearCombatEntriesLocked();
            return;
        }
        const bool paused = EntryGamePaused();
        for (auto& [fid, e] : g_entries) {
            if (!paused && !e.ended && !e.everLive) ++e.unpausedSweeps;
            EntryEndedLocked(api, fid, e);
        }
    }

    // ClearTransientState's teardown (declared in APMFBridge_internal.h). g_mx HELD.
    void ClearCombatEntriesLocked() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        for (auto& [fid, e] : g_entries)
            if (api && e.handle != APMF_API::kInvalidHandle) api->Release(e.handle);
        g_entries.clear();
    }

    bool CombatEntryOffered() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kEntryMinAbi && !g_entrySeatRefused.load(std::memory_order_relaxed);
    }

    EntryResult RequestCombatEntry(RE::FormID a_follower, RE::FormID a_target, std::uint32_t* a_outHandle,
                                   float a_engageRange) {
        if (a_outHandle) *a_outHandle = APMF_API::kInvalidHandle;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kEntryMinAbi || g_entrySeatRefused.load(std::memory_order_relaxed))
            return EntryResult::SeatAbsent;
        // APMF refuses these synchronously too; filtering them here keeps a synchronous
        // refusal meaning exactly ONE thing (the seat is not installed). 0x14 = the player.
        if (a_follower == 0 || a_target == 0 || a_target == a_follower || a_follower == 0x14)
            return EntryResult::Invalid;

        std::scoped_lock lock(g_mx);
        if (g_entries.contains(a_follower)) return EntryResult::Standing;

        APMF_API::APMF_Param prm{};
        prm.form = a_target;   // REQUIRED: the target ACTOR.
        // Harbinger's range rule (fix/apmf-engagement-held): fval = the engage reaction distance from the
        // FOLLOWER (min(leash, fEngageOnSightRange)). A target farther than that (Harbinger drops on the first poll, no debounce) that is not actively
        // hostile is dropped and the follower's combat ended. Older Harbinger builds do not read it (and 0 =
        // no range), so it is safe on any version. Negative / NaN is sent as 0.
        if (a_engageRange > 0.0f) prm.fval = a_engageRange;
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_CombatEntry, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            if (!g_entrySeatRefused.exchange(true)) {
                spdlog::warn("[engage-on-sight] Harbinger REFUSED the ch.21 combat-entry claim ({:08X} -> "
                             "{:08X}) at the call: the ch.21 seat is not installed (VR, a runtime other than "
                             "1.6.1170 / 1.5.97, [CombatEntry] bCombatEntry=0 in APMF.ini, or its address "
                             "self-check; APMF's log names it). The engage-on-sight gambit is INERT for this "
                             "session. MFO has no direct road.",
                             a_follower, a_target);
            }
            return EntryResult::SeatAbsent;
        }
        CombatEntryClaim e{};
        e.handle = h;
        e.target = a_target;
        g_entries.emplace(a_follower, e);
        if (a_outHandle) *a_outHandle = h;
        return EntryResult::Filed;
    }

    EntryState CombatEntryStateOf(RE::FormID a_follower, RE::FormID* a_outTarget, std::uint32_t* a_outEndReason) {
        if (a_outTarget) *a_outTarget = 0;
        if (a_outEndReason) *a_outEndReason = APMF_API::kClaimEnd_None;
        std::scoped_lock lock(g_mx);
        const auto it = g_entries.find(a_follower);
        if (it == g_entries.end()) return EntryState::None;
        if (a_outTarget) *a_outTarget = it->second.target;
        if (a_outEndReason) *a_outEndReason = it->second.endReason;
        return it->second.ended ? EntryState::Ended : EntryState::Standing;
    }

    void ForgetCombatEntry(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        const auto it = g_entries.find(a_follower);
        if (it == g_entries.end()) return;
        if (api && it->second.handle != APMF_API::kInvalidHandle) api->Release(it->second.handle);
        g_entries.erase(it);
    }

    // ── ABI v31 CLAIM END REASON + THE NOT-ENGAGED GIVE-UP (marth 2026-10-27) ────────────────
    // See the file header and APMFBridge.h. The registry has its OWN leaf mutex: RecordEngageGiveUp is
    // called by the pin / entry sweeps with g_mx HELD (order g_mx -> g_giveUpMx), and nothing here ever
    // takes g_mx. The actor reads (position, combat target, IsHostileToActor) are made without g_giveUpMx
    // in the lift check, on a copy, so the leaf mutex never covers an engine call there.
    namespace {
        constexpr std::uint32_t kEndReasonMinAbi = 31;   // the ABI that first serves GetClaimEndReason
        // MOVED MATERIALLY: farther than this from where it was given up. 512u (about 7 m) is four times
        // the 128u step Harbinger's own no-path-progress test calls "the target moved", so a cat pacing on
        // its ledge stays given up while one that comes down off it is a new situation. A distance, not a wait.
        constexpr float       kGiveUpMoveU          = 512.0f;
        constexpr std::size_t kGiveUpMaxPerFollower = 16;   // bound per follower; the oldest record goes first

        struct GiveUp {
            RE::FormID    target   = 0;
            RE::NiPoint3  pos{};              // where it stood when it was given up
            std::uint32_t reason   = APMF_API::kClaimEnd_None;
            bool          fighting = false;   // fighting the party, as last seen (its rising edge lifts)
            bool          hostile  = false;   // hostile to the player or him, as last seen (its rising edge lifts)
            bool          asked    = false;   // a caller that logs its refusal was told "given up" already
        };
        std::mutex                                          g_giveUpMx;   // LEAF
        std::unordered_map<RE::FormID, std::vector<GiveUp>> g_giveUps;    // follower -> records, g_giveUpMx

        const char* GiveUpName(RE::Actor* a) {
            const char* n = a ? a->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        // Fighting the party now: its currentCombatTarget resolves to the player or a teammate (the same
        // test as EngageOnSight's FightingParty). Worker or main thread, no lock held.
        bool FightingPartyNow(RE::Actor* a_t) {
            auto  tp = a_t->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
            auto* t  = tp.get();
            return t && (t->IsPlayerRef() || t->IsPlayerTeammate());
        }

        // The engine's own hostility to the player or to him (the read EngageOnSight's IsEnemy makes).
        bool HostileToPartyNow(RE::Actor* a_t, RE::Actor* a_f) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            return (pc && a_t->IsHostileToActor(pc)) || (a_f && a_t->IsHostileToActor(a_f));
        }

        enum class Verdict { Keep, Lift, Gone };

        // The lift check on a COPY, g_giveUpMx NOT held (PinTarget calls it with g_mx held, which is fine:
        // nothing here takes g_mx). On Keep the copy's fighting / hostile flags are set to what
        // was seen now, so a falling edge is remembered and a later rising edge lifts.
        Verdict Judge(RE::FormID a_follower, GiveUp& a_g, std::string& a_why) {
            auto* t = RE::TESForm::LookupByID<RE::Actor>(a_g.target);
            if (!t || t->IsDead() || t->IsDisabled()) return Verdict::Gone;
            const float moved = t->GetPosition().GetDistance(a_g.pos);
            if (moved > kGiveUpMoveU) {
                a_why = fmt::format("it moved {:.0f}u from where it was given up", moved);
                return Verdict::Lift;
            }
            const bool fighting = FightingPartyNow(t);
            if (fighting && !a_g.fighting) {
                a_why = "it started fighting the party";
                return Verdict::Lift;
            }
            const bool hostile = HostileToPartyNow(t, RE::TESForm::LookupByID<RE::Actor>(a_follower));
            if (hostile && !a_g.hostile) {
                a_why = "it turned hostile to the party";
                return Verdict::Lift;
            }
            a_g.fighting = fighting;
            a_g.hostile  = hostile;
            return Verdict::Keep;
        }

        // Applies one verdict to the live record (matched by target: it may have changed meanwhile) and logs
        // a lift or a drop. Returns true while the target stays given up. Takes g_giveUpMx.
        bool Apply(RE::FormID a_follower, const GiveUp& a_seen, Verdict a_v, const std::string& a_why,
                   bool* a_firstAsk) {
            {
                std::scoped_lock lk(g_giveUpMx);
                const auto fit = g_giveUps.find(a_follower);
                if (fit == g_giveUps.end()) return false;
                auto& v  = fit->second;
                auto  it = std::find_if(v.begin(), v.end(), [&](const GiveUp& g) { return g.target == a_seen.target; });
                if (it == v.end()) return false;
                if (a_v == Verdict::Keep) {
                    it->fighting = a_seen.fighting;
                    it->hostile  = a_seen.hostile;
                    if (a_firstAsk) {
                        *a_firstAsk = !it->asked;
                        it->asked   = true;
                    }
                    return true;
                }
                v.erase(it);
                if (v.empty()) g_giveUps.erase(fit);
            }
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            auto* t = RE::TESForm::LookupByID<RE::Actor>(a_seen.target);
            if (a_v == Verdict::Lift)
                spdlog::info("[engage-give-up] {} ({:08X}): {} ({:08X}) is no longer given up, {}. It may be "
                             "engaged again.",
                             GiveUpName(f), a_follower, GiveUpName(t), a_seen.target, a_why);
            else
                spdlog::info("[engage-give-up] {} ({:08X}): {:08X} dropped from the given-up list, it is dead, "
                             "disabled or no longer resolves.",
                             GiveUpName(f), a_follower, a_seen.target);
            return false;
        }
    }

    std::uint32_t ClaimEndReasonOf(const APMF_API::APMF_API_v2* api, APMF_API::Handle h) {
        if (!api || h == APMF_API::kInvalidHandle) return APMF_API::kClaimEnd_None;
        if (api->abiVersion < kEndReasonMinAbi) {
            // THE DOCUMENTED DEGRADE (not a mask): an older Harbinger (0.15.1 is ABI 30) cannot say why a claim
            // ended, so nothing is given up on a not-engaged end and today's rules hold (the pin's no-loop
            // rule, engage-on-sight's range-drop guess). Said once per session.
            static std::atomic<bool> s_said{ false };
            if (!s_said.exchange(true))
                spdlog::info("[engage-give-up] Harbinger ABI {} is older than {}: it cannot say why a claim ended, "
                             "so a not-engaged target is not given up (today's rules apply). Said once.",
                             api->abiVersion, kEndReasonMinAbi);
            return APMF_API::kClaimEnd_None;
        }
        return reinterpret_cast<const APMF_API::APMF_API_v31*>(api)->GetClaimEndReason(h);
    }

    bool ClaimEndReasonOffered() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kEndReasonMinAbi;
    }

    bool IsNotEngagedEnd(std::uint32_t a_reason) {
        return a_reason == APMF_API::kClaimEnd_NotEngaged || a_reason == APMF_API::kClaimEnd_NoPathProgress;
    }

    const char* ClaimEndReasonName(std::uint32_t a_reason) {
        switch (a_reason) {
        case APMF_API::kClaimEnd_None:           return "none (no record, or Harbinger older than ABI 31)";
        case APMF_API::kClaimEnd_NotEngaged:     return "not engaged (past the engage range, not attacking the party)";
        case APMF_API::kClaimEnd_NoPathProgress: return "no path progress (counts as not engaged)";
        case APMF_API::kClaimEnd_ArrivedIdle:    return "arrived but not fighting";
        case APMF_API::kClaimEnd_CombatEnded:    return "the engine ended the fight";
        case APMF_API::kClaimEnd_EntryRefused:   return "the engine refused the entry";
        case APMF_API::kClaimEnd_NotAttempted:   return "entry not attempted";
        case APMF_API::kClaimEnd_OwnerDead:      return "the follower died";
        case APMF_API::kClaimEnd_TargetDead:     return "the target died";
        case APMF_API::kClaimEnd_TargetGone:     return "the target is gone";
        case APMF_API::kClaimEnd_TargetLost:     return "the engine lost the target";
        case APMF_API::kClaimEnd_NotATarget:     return "no longer a combat target";
        default:                                 return "unknown (newer Harbinger)";
        }
    }

    void RecordEngageGiveUp(RE::FormID follower, RE::FormID target, std::uint32_t reason, const char* road) {
        if (follower == 0 || target == 0) return;
        auto* t = RE::TESForm::LookupByID<RE::Actor>(target);
        if (!t || t->IsDead() || t->IsDisabled()) return;   // nothing left to give up
        auto* f = RE::TESForm::LookupByID<RE::Actor>(follower);
        GiveUp g{};
        g.target   = target;
        g.pos      = t->GetPosition();
        g.reason   = reason;
        g.fighting = FightingPartyNow(t);
        g.hostile  = HostileToPartyNow(t, f);
        {
            std::scoped_lock lk(g_giveUpMx);
            auto& v = g_giveUps[follower];
            // The pin and the entry end in the same Harbinger step: the first record stands.
            if (std::any_of(v.begin(), v.end(), [&](const GiveUp& o) { return o.target == target; })) return;
            if (v.size() >= kGiveUpMaxPerFollower) v.erase(v.begin());
            v.push_back(g);
        }
        spdlog::info("[engage-give-up] {} ({:08X}): gives up {} ({:08X}) now. Harbinger ended the {} as {}. "
                     "It is engaged again only when it moves more than {:.0f}u from here, turns hostile to the "
                     "party or starts fighting a party member.",
                     GiveUpName(f), follower, GiveUpName(t), target, road, ClaimEndReasonName(reason), kGiveUpMoveU);
    }

    void ClearEngageGiveUps() {
        std::scoped_lock lk(g_giveUpMx);
        g_giveUps.clear();
    }

    bool EngageGivenUp(RE::FormID a_follower, RE::FormID a_target, bool* a_firstAsk) {
        if (a_firstAsk) *a_firstAsk = false;
        GiveUp copy{};
        {
            std::scoped_lock lk(g_giveUpMx);
            const auto fit = g_giveUps.find(a_follower);
            if (fit == g_giveUps.end()) return false;
            const auto it = std::find_if(fit->second.begin(), fit->second.end(),
                                         [&](const GiveUp& g) { return g.target == a_target; });
            if (it == fit->second.end()) return false;
            copy = *it;
        }
        std::string why;
        const Verdict v = Judge(a_follower, copy, why);
        return Apply(a_follower, copy, v, why, a_firstAsk);
    }

    std::vector<RE::FormID> EngageGiveUpsOf(RE::FormID a_follower) {
        std::vector<GiveUp> copies;
        {
            std::scoped_lock lk(g_giveUpMx);
            const auto fit = g_giveUps.find(a_follower);
            if (fit == g_giveUps.end()) return {};
            copies = fit->second;
        }
        std::vector<RE::FormID> out;
        out.reserve(copies.size());
        for (auto& c : copies) {
            std::string why;
            const Verdict v = Judge(a_follower, c, why);
            if (Apply(a_follower, c, v, why, nullptr)) out.push_back(c.target);
        }
        return out;
    }

    void ForgetEngageGiveUps(RE::FormID a_follower) {
        std::scoped_lock lk(g_giveUpMx);
        g_giveUps.erase(a_follower);
    }

}
