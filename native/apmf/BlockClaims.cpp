// apmf/BlockClaims.cpp -- the gambit action "Block" (act.block) on Harbinger's HELD GUARD (ch.31, ABI v43,
// kCombatMove_Block / kCombatMove_BlockEnd). T2 batch M item 1, the Knight's "blocks a lot".
// MFO only DECLARES: who raises his guard, and for how many seconds. Harbinger raises it by the engine's own Left Attack
// action, keeps it up for the window (the follower's own offense and the engine's own shorter block do not run while it
// stands), lowers it by the Left Release action, and reports each step. A one-shot REQUEST with a duration, never a claim:
// nothing to heartbeat, nothing saved. Keeping a guard up is asking again before the window ends (Harbinger renews, the
// guard does not drop in between).
// Docs: Harbinger Docs/FACETS.md "Block", APMF_API.h "ABI v43: BLOCK"; MFO Docs/GAMBITS.md "Block".
// MFO's own gates run BEFORE the request (gate the source): the weapon is drawn and his hands would block. Harbinger
// tests the same and refuses with a reason; MFO's test only keeps a rule that cannot work from asking every tick.
// Threading: RequestEx is an enqueue (any thread); GetCombatMoveState is a copy under Harbinger's leaf lock (any thread).
// The table below (one row per follower: his last block request) has its own LEAF mutex g_blockMx, never g_mx, and no
// Harbinger call is made while it is held.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kBlockMinAbi = 43;   // the ABI that builds kCombatMove_Block and knows kCombatMove_BlockEnd
        using Clock = std::chrono::steady_clock;
        constexpr auto kRefusedBackoff = std::chrono::seconds(2);   // after a refusal / failure: no new request for this long
        // Review F2 of 88777f7 (marth Q-melee-3: a held guard denies ALL of his own offense, heals included). A guard must
        // therefore always END, whatever the row's cooldown: one hold is renewed at most kMaxRenewals times (so at most
        // (1 + kMaxRenewals) windows back to back), and after ANY hold ends no new guard is asked for kRest. With the class
        // rows' 4 s cooldown neither bound is ever reached; a zero-cooldown Block row guards for two windows, then fights
        // (and drinks, heals, attacks) for kRest. Q-melee-8.
        constexpr std::uint32_t kMaxRenewals = 1;
        constexpr auto          kRest        = std::chrono::milliseconds(1500);

        struct Row {
            std::uint32_t     handle = 0;        // the last kCombatMove_Block request (0 = none)
            Clock::time_point at{};              // when it was sent
            float             holdS = 0.0f;      // the window it asked for
            bool              live = false;      // Harbinger reported Started + HoldLive and no end yet
            bool              startLogged = false;
            bool              doneLogged = false;   // the Refused / Failed / Ended line was written
            std::uint32_t     renewals = 0;         // how many times this guard was renewed (carried across handles)
            Clock::time_point backoffUntil{};       // after a refusal / failure
            Clock::time_point restUntil{};          // after a guard ended (kRest)
        };
        std::mutex g_blockMx;                          // LEAF
        std::unordered_map<RE::FormID, Row> g_rows;    // follower -> his last block request
        std::atomic<std::uint32_t> g_out{ 0 };         // rows with a request out (queued or live): BlockHoldLive's fast path

        void RecountOut() {   // g_blockMx held
            std::uint32_t n = 0;
            for (const auto& [fid, r] : g_rows)
                if (r.handle != 0 && !r.doneLogged) ++n;
            g_out.store(n, std::memory_order_release);
        }

        const char* WhyName(std::uint32_t a_why) {
            switch (a_why) {
            case APMF_API::kCombatMoveWhy_ActorGone:         return "actor gone";
            case APMF_API::kCombatMoveWhy_NotReady:          return "not ready (not loaded, dead, kill move, weapon not drawn)";
            case APMF_API::kCombatMoveWhy_Busy:              return "an attack is in progress";
            case APMF_API::kCombatMoveWhy_SubstrateHeld:     return "a combat substrate holds him";
            case APMF_API::kCombatMoveWhy_Outranked:         return "MFO does not outrank Engine on CombatAction (Harbinger priorities)";
            case APMF_API::kCombatMoveWhy_EngineRefused:     return "the engine refused the action (staggered, recoiling, mid-draw)";
            case APMF_API::kCombatMoveWhy_NotApplied:        return "the engine ran the action as something other than a block";
            case APMF_API::kCombatMoveWhy_Error:             return "an error inside Harbinger";
            case APMF_API::kCombatMoveWhy_NoBlockItem:       return "his hands would not block";
            case APMF_API::kCombatMoveWhy_HeldByOther:       return "another mod holds his guard";
            case APMF_API::kCombatMoveWhy_NoHold:            return "no hold of ours stood";
            case APMF_API::kCombatMoveWhy_HoldTableFull:     return "Harbinger's hold table is full";
            case APMF_API::kCombatMoveWhy_HoldExpired:       return "the window ran out";
            case APMF_API::kCombatMoveWhy_HoldEndedByClient: return "MFO ended it";
            case APMF_API::kCombatMoveWhy_HoldRenewed:       return "renewed by a newer request";
            case APMF_API::kCombatMoveWhy_HoldBroken:        return "it could not be kept (stamina out, hands changed, sheathed, dead, unloaded, a load)";
            default:                                         return "no reason given";
            }
        }

        // One Harbinger read of `a_handle`; `a_info` is filled. Never under g_blockMx.
        std::uint32_t ReadState(const APMF_API::APMF_API_v2* a_api, std::uint32_t a_handle, APMF_API::APMF_CombatMoveStateInfo& a_info) {
            a_info = {};
            a_info.size = sizeof(a_info);
            return reinterpret_cast<const APMF_API::APMF_API_v38*>(a_api)->GetCombatMoveState(a_handle, &a_info);
        }
    }

    bool BlockSupported() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kBlockMinAbi;
    }

    const char* BlockHandsReason(RE::Actor* a_follower) {
        if (!a_follower) return "no actor";
        if (!a_follower->AsActorState()->IsWeaponDrawn()) return "his weapon is not drawn";
        auto* left  = a_follower->GetEquippedObject(true);
        auto* right = a_follower->GetEquippedObject(false);
        if (left) {
            if (const auto* armo = left->As<RE::TESObjectARMO>(); armo && armo->IsShield()) return nullptr;
            if (left->Is(RE::FormType::Light)) return nullptr;
            if (left != right) return "his left hand holds a weapon, a spell or a staff (no block from those hands)";
        }
        const auto* weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
        if (!weap) return "no shield and no melee weapon in hand";
        // GetEquippedObject answers the BASE form, so `left == right` is true for a two-handed weapon (one item, both
        // hands) AND for two copies of the same one-handed weapon (dual wield). Only the first blocks: with a one-hander in
        // each hand Left Attack swings the left weapon (review F1 of 88777f7). The same test as Harbinger's
        // HandsWouldNotBlock (core/CombatMoveBlock.cpp); keep the two identical.
        switch (weap->GetWeaponType()) {
        case RE::WEAPON_TYPE::kOneHandSword:
        case RE::WEAPON_TYPE::kOneHandDagger:
        case RE::WEAPON_TYPE::kOneHandAxe:
        case RE::WEAPON_TYPE::kOneHandMace:
            return left ? "the same one-handed weapon in both hands (dual wield: no block from those hands)" : nullptr;
        case RE::WEAPON_TYPE::kTwoHandSword:
        case RE::WEAPON_TYPE::kTwoHandAxe:
            return nullptr;
        default:
            return "a bow, a crossbow, a staff or bare fists (no block from those hands)";
        }
    }

    bool BlockHoldLive(RE::FormID a_follower) {
        if (g_out.load(std::memory_order_acquire) == 0) return false;
        std::scoped_lock lk(g_blockMx);
        const auto it = g_rows.find(a_follower);
        return it != g_rows.end() && it->second.handle != 0 && !it->second.doneLogged;
    }

    BlockAsk ServiceBlock(RE::FormID a_follower, std::uint32_t* a_handleOut) {
        if (a_handleOut) *a_handleOut = 0;
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kBlockMinAbi) return BlockAsk::NoRoad;
        PollBlock(a_follower);   // the previous request's state is current (and logged) before it is judged
        const auto    now      = Clock::now();
        const float   holdS    = Config::g_blockHoldSeconds.load(std::memory_order_relaxed);
        std::uint32_t renewals = 0;
        {
            std::scoped_lock lk(g_blockMx);
            if (const auto it = g_rows.find(a_follower); it != g_rows.end()) {
                const Row& r = it->second;
                if (now < r.backoffUntil) return BlockAsk::Backoff;
                if (r.handle != 0 && !r.doneLogged) {
                    // A request is out. Not yet run by the game thread, or a live hold with more than 40% of its
                    // window left: nothing to ask. Past that mark a new request RENEWS the hold (no drop in between),
                    // at most kMaxRenewals times: after that the guard runs out at its window (review F2).
                    if (a_handleOut) *a_handleOut = r.handle;
                    if (!r.live) return BlockAsk::Held;
                    const float elapsed = std::chrono::duration<float>(now - r.at).count();
                    if (elapsed < r.holdS * 0.6f || r.renewals >= kMaxRenewals) return BlockAsk::Held;
                    renewals = r.renewals + 1;
                } else if (now < r.restUntil) {
                    return BlockAsk::Resting;
                }
            }
        }
        APMF_API::APMF_Param p{};
        p.ival = static_cast<std::int32_t>(APMF_API::kCombatMove_Block);
        p.fval = holdS;
        const std::uint32_t h = api->RequestEx(a_follower, APMF_API::kIntent_CombatMove, 0.0f, &p);   // basis is not read
        std::scoped_lock lk(g_blockMx);
        Row& r = g_rows[a_follower];
        if (h == APMF_API::kInvalidHandle) {
            r.backoffUntil = now + kRefusedBackoff;
            spdlog::warn("[block] {:08X}: Harbinger refused the block request at the call (its log says why); next try in {} s",
                         a_follower, std::chrono::duration_cast<std::chrono::seconds>(kRefusedBackoff).count());
            return BlockAsk::Refused;
        }
        // A renewed hold's older record ends "renewed" in Harbinger; this row now follows the newer handle only.
        r = Row{};
        r.handle   = h;
        r.at       = now;
        r.holdS    = holdS;
        r.renewals = renewals;
        RecountOut();
        if (a_handleOut) *a_handleOut = h;
        return BlockAsk::Requested;
    }

    void PollBlock(RE::FormID a_follower) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kBlockMinAbi) return;
        Row r;
        {
            std::scoped_lock lk(g_blockMx);
            const auto it = g_rows.find(a_follower);
            if (it == g_rows.end() || it->second.handle == 0 || it->second.doneLogged) return;
            r = it->second;
        }
        APMF_API::APMF_CombatMoveStateInfo info{};
        const std::uint32_t st = ReadState(api, r.handle, info);
        if (st == APMF_API::kCombatMoveState_Queued) return;   // the game thread has not run it yet
        bool startNow = false, doneNow = false, live = false, backoff = false;
        if (st == APMF_API::kCombatMoveState_Started) {
            live = (info.info & APMF_API::kCombatMoveInfo_HoldLive) != 0;
            if (!r.startLogged) {
                startNow = true;
                spdlog::info("[block] {:08X}: guard UP for {:.1f} s (h={})", a_follower, r.holdS, r.handle);
            }
        } else {
            doneNow = true;
            const bool blocked  = (info.info & APMF_API::kCombatMoveInfo_BlockedAHit) != 0;
            const bool reRaised = (info.info & APMF_API::kCombatMoveInfo_ReRaised) != 0;
            if (st == APMF_API::kCombatMoveState_Ended) {
                // A renewal is the same guard carried on by the newer request: not worth a line of its own.
                if (info.why != APMF_API::kCombatMoveWhy_HoldRenewed)
                    spdlog::info("[block] {:08X}: guard DOWN (h={}): {}; blocked a hit: {}{}; renewed {} time(s) (at most {}); no new "
                                 "guard for {} ms", a_follower, r.handle, WhyName(info.why), blocked ? "yes" : "no",
                                 reRaised ? "; the engine dropped it inside the window and Harbinger raised it again" : "",
                                 r.renewals, kMaxRenewals, kRest.count());
            } else if (st == APMF_API::kCombatMoveState_Refused) {
                backoff = true;
                spdlog::info("[block] {:08X}: block h={} REFUSED: {}", a_follower, r.handle, WhyName(info.why));
            } else if (st == APMF_API::kCombatMoveState_Failed) {
                backoff = true;
                spdlog::warn("[block] {:08X}: block h={} FAILED: {}", a_follower, r.handle, WhyName(info.why));
            } else {
                spdlog::warn("[block] {:08X}: block h={} has no record in Harbinger any more (a load, or it was read too late)",
                             a_follower, r.handle);
            }
        }
        std::scoped_lock lk(g_blockMx);
        const auto it = g_rows.find(a_follower);
        if (it == g_rows.end() || it->second.handle != r.handle) return;
        it->second.live        = live && !doneNow;
        it->second.startLogged = it->second.startLogged || startNow;
        it->second.doneLogged  = it->second.doneLogged || doneNow;
        if (backoff) it->second.backoffUntil = Clock::now() + kRefusedBackoff;
        // Review F2: after a guard that really stood has ended, no new guard for kRest (his offense, heals and potions get
        // their turn whatever the row's cooldown is).
        if (doneNow && st == APMF_API::kCombatMoveState_Ended) it->second.restUntil = Clock::now() + kRest;
        RecountOut();
    }

    void ReleaseBlock(RE::FormID a_follower, const char* a_why) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        Row r;
        {
            std::scoped_lock lk(g_blockMx);
            const auto it = g_rows.find(a_follower);
            if (it == g_rows.end()) return;
            r = it->second;
            g_rows.erase(it);
            RecountOut();
        }
        if (!api || api->abiVersion < kBlockMinAbi || r.handle == 0 || r.doneLogged) return;
        // A request is out (queued, or a live hold): lower the guard now. Harbinger answers "no hold" when the request
        // never became one, which is the outcome wanted.
        APMF_API::APMF_Param p{};
        p.ival = static_cast<std::int32_t>(APMF_API::kCombatMove_BlockEnd);
        const std::uint32_t h = api->RequestEx(a_follower, APMF_API::kIntent_CombatMove, 0.0f, &p);
        spdlog::info("[block] {:08X}: guard lowered ({}): block end {} (h={}, the hold was h={})", a_follower, a_why ? a_why : "?",
                     h != APMF_API::kInvalidHandle ? "requested" : "REFUSED at the call (Harbinger's log says why; its window still ends it)",
                     h, r.handle);
    }

    void ClearBlockHolds() {
        std::scoped_lock lk(g_blockMx);
        g_rows.clear();   // Harbinger forgets every hold and record at its own load boundary
        g_out.store(0, std::memory_order_release);
        ResetCombatMoveServed();   // review F3: "is ch.31 served on this game" is asked again after a load
    }
}
