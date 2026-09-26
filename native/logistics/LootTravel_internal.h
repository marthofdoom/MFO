#pragma once
// logistics/LootTravel_internal.h -- the loot TRAVEL substrate: travel intents
// and the per-slot table, stall / sticky / gate / actor-block state, and scan
// timing. NOT self-contained: included ONLY by Logistics_internal.h, at the
// point where this block used to sit (it uses Clock, Category, ... above it,
// and the declarations below it use LootMode from here).
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.

namespace MFO::Logistics {

        // OPTION A travel state -- up to kMaxLootSlots travellers AT ONCE (one loot
        // quest, four alias pairs; see g_travelSlots below). Worker-tick-only, NOT
        // serialized: this just remembers the intent; the engine-side alias fill is
        // cleared by Packages on load (#55). kArrivalDist ~= arm's reach: once the
        // engine walks the follower this close, the existing inventory transfer runs.
        constexpr float kArrivalDist = 200.0f;   // was 160 -- perf pass: fewer "stalled a hair short" strandings
        // A BATCH EXCURSION, not a single trip. The follower stays claimed
        // (priority 60) across corpses: Walking = en route to `target`; Holding =
        // arrived/leg-failed, seeking the next leg or waiting out a dibs timer.
        enum class TravelPhase { Walking, Holding };
        struct TravelIntent {
            RE::FormID          follower = 0;
            RE::ObjectRefHandle target;
            Category            cat  = Category::Arrows;
            RE::ActorValue      want = RE::ActorValue::kNone;
            Clock::time_point   deadline{};       // per-LEG walk deadline
            bool                active = false;
            TravelPhase         phase = TravelPhase::Walking;
            Clock::time_point   startTime{};      // excursion start -> fExcursionMax cap
            Clock::time_point   lingerUntil{};    // Hold bound -> fBatchLinger
            // NO-PROGRESS detection: an UNREACHABLE target (no navmesh path) sits
            // at a flat distance -- the follower is on the travel package but
            // FROZEN in place (deck: dist=1449 unchanged for 13 s). We track his
            // own WORLD position (not distance-to-target, which plateaus on a
            // detour around a wall): if he has not MOVED for kNoProgress seconds,
            // the target is unreachable -- give up NOW, well before the leg
            // deadline, so he stops cycling unreachable bodies and the excursion
            // ends -> he follows.
            RE::NiPoint3        lastPos{};          // his position at progressAt
            Clock::time_point   progressAt{};       // last time he actually moved
            // PACKAGE-THEFT episode start (deck 12:25:18: onTravelPkg flipped
            // false mid-walk, curPkg=FF001780 -- a runtime scene/framework
            // package took the follower while the loot alias was STILL filled).
            // While an external package holds him, the leg's stall/deadline
            // clocks must not run -- the REF is not to blame -- and the claim is
            // re-asserted (EvaluatePackage) each tick for a bounded grace.
            // Zero = currently on the travel package.
            Clock::time_point   stolenSince{};
            // APMF LEG ENGAGEMENT (DIAG-2026-09-06 fix 3/4). An APMF-routed leg
            // is only "walking" once the engine has actually ADOPTED the offered
            // travel package -- i.e. IsTravelPackage(GetCurrentPackage()) has read
            // true at least once for THIS leg. The field session that produced the
            // diagnosis had 0 of 6 dispatches ever reach that state, yet the
            // stall/deadline clocks (and the theft-guard bypass below) all assumed
            // it. So the observation is recorded per leg and nothing is inferred
            // from the claim merely having been REQUESTED.
            //   legEngaged        -- sticky-true once observed on the travel package
            //   legStart          -- leg dispatch time, so the deadline VALUE can be
            //                        reported (deadline - legStart) when it expires
            //   nextLegPkgDiag    -- throttle shared by the two per-leg package
            //                        reports (never engaged / engaged-then-displaced;
            //                        the two are mutually exclusive on legEngaged, so
            //                        one timer serves both). Zero means "report on the
            //                        next Walking tick", which is how a fresh leg gets
            //                        its first line immediately -- same shape as the
            //                        s_nextWalkDiag throttle, but stored per LEG so a
            //                        new dispatch is never swallowed by the previous
            //                        leg's window.
            bool                legEngaged = false;
            Clock::time_point   legStart{};
            Clock::time_point   nextLegPkgDiag{};
            // MOVEMENT BLOCKED backstop (loot M1): first tick this leg saw the
            // engine's kMovementBlocked runtime package, and the legStart it was
            // seen under (a retarget rewrites legStart, so a stale onset can never
            // gate the NEXT leg). Zero = not blocked. Worker-only, NOT serialized.
            Clock::time_point   blockedSince{};
            Clock::time_point   blockedLeg{};
            // loot M2 (Harbinger ABI v12 leg state): the seq of the last ch.19 leg
            // END already logged (each end is logged once), and the legStart for
            // which "Movement Blocked held, Harbinger still says walking" was
            // warned (once per leg). Worker-only, NOT serialized.
            std::uint32_t       ch19SeqLogged = 0;
            bool                ch19SeqLoggedSet = false;
            Clock::time_point   ch19MbWarnedLeg{};
            // ACQUIRE PROBE (route 2b) readback: after an Activate dispatch at a
            // LOOSE ref, the NEXT tick observes what the engine actually did
            // (dispatch is asynchronous -- Papyrus.h -- so same-tick reads lie).
            bool                acquirePending = false;
            RE::FormID          acquireRefID = 0;   // the loose ref, for the log (its handle may die)
            RE::FormID          acquireBase  = 0;   // its base object -- the inventory-delta key
            std::int32_t        acquirePre   = 0;   // follower's count of base BEFORE dispatch
        };
        // P7 MULTI-SLOT: up to kMaxLootSlots concurrent loot excursions, one per
        // slot. Slot i maps to the loot quest's alias PAIR (actor 2*i, target
        // 2*i+1). Slot 0 is byte-identical to the shipped single-slot state.
        // Logistics owns this follower->slot map; Packages is passed the slot.
        inline std::array<TravelIntent, Packages::kMaxLootSlots> g_travelSlots{};

        // The active slot whose intent belongs to a_follower (nullptr if none).
        inline TravelIntent* SlotOf(RE::FormID a_follower) {
            if (!a_follower) return nullptr;
            for (auto& t : g_travelSlots)
                if (t.active && t.follower == a_follower) return &t;
            return nullptr;
        }
        // Index of a_follower's active slot, or -1.
        inline int SlotIndexOf(RE::FormID a_follower) {
            if (!a_follower) return -1;
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (g_travelSlots[i].active && g_travelSlots[i].follower == a_follower)
                    return i;
            return -1;
        }
        // First free (inactive) slot, or -1 if all kMaxLootSlots are busy.
        inline int FreeSlotIndex() {
            for (int i = 0; i < Packages::kMaxLootSlots; ++i)
                if (!g_travelSlots[i].active) return i;
            return -1;
        }
        // Is ANY excursion in flight? Gates the global blocklist reassess.
        inline bool AnyTravelActive() {
            for (auto& t : g_travelSlots)
                if (t.active) return true;
            return false;
        }
        constexpr float kMoveEps    = 50.0f;                  // real-movement threshold (units)
        // 5s (was 7, was 4). 4s false-positived on momentary repositioning; 7s was
        // the fix -- but most of what 7s actually absorbed turned out to be the
        // PACKAGE-THEFT case (a scene/framework package holding the follower, deck
        // 12:25:18), which the excursion driver now detects and exempts from the
        // stall clock entirely (stolenSince). With theft out of the stall path, a
        // genuine on-package zero-movement stall is a much cleaner geometric
        // verdict, so it can fail FASTER (marth RC#4: ~9s wasted per dead leg).
        constexpr auto  kNoProgress = std::chrono::seconds(5);
        // How long a stolen leg re-asserts the alias claim before giving the leg
        // up (transient blocklist ONLY, never sticky -- the ref was reachable for
        // all we know). Long enough to outlast a follower one-liner/idle scene,
        // short enough that a standing external hold (a long scripted scene)
        // releases the batch instead of parking the excursion.
        constexpr auto  kStealGrace = std::chrono::seconds(10);
        // PACKAGE-THEFT BACK-OFF. A single steal that reclaims within grace is
        // normal, but a claim that keeps getting stolen is fighting a package
        // that will not release (a persistent scene, or a combat gambit) -- so
        // re-asserting forever just churns (deck: "travel pkg stolen ...
        // re-asserting claim" logged every few seconds for both followers, the
        // leg never completing). Count displacements of the SAME follower+target
        // claim; after kStealStrikeMax, ABANDON the leg to the transient
        // blocklist (MarkTravelFailed, never sticky -- the ref was reachable)
        // and move on. Keyed (follower<<32 | target). Reset on arrival/loot
        // (provably reachable) or a target change (a new key starts fresh).
        // Worker-tick-only, erased on resolution like g_stallStrikes, never
        // serialized.
        inline std::unordered_map<std::uint64_t, int> g_stealStrikes;
        constexpr int kStealStrikeMax = 4;
        inline std::uint64_t StealKey(RE::FormID a_fol, RE::FormID a_tgt) {
            return (static_cast<std::uint64_t>(a_fol) << 32) | a_tgt;
        }

        // Set by an excursion-mode scan when it found loot it could NOT act on
        // because the player's dibs have not released yet (dNotYet > 0). The Hold
        // logic reads it: something still worth waiting for -> linger; else the
        // batch is exhausted -> return to the player. Worker-tick-only.
        inline bool g_scanSawWaiting = false;

        // kNormal: not (yet) on an excursion -- arm's-reach transfer OR START one
        // by walking to a far corpse (LootTravelFill, claim at 60). kExcursion:
        // already claimed and driving a batch -- the closest eligible candidate
        // drives the tick, arm's-reach -> grab, farther-but-walkable -> RETARGET
        // without releasing (LootTravelRetarget). One action per tick either way.
        enum class LootMode { kNormal, kExcursion };

        // Targets a walk FAILED to reach (navmesh-blocked, or the follower could
        // not close the distance before the deadline). Skipped for a cooldown so
        // closest-first does not re-pick the same unreachable corpse every tick
        // and churn the follower in place (the v0.8.1 loop). Bounded LRU, not
        // serialized. Keyed by the target FormID.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_travelFailed;
        constexpr auto kTravelFailCooldown = std::chrono::seconds(25);

        // GROWN GRAB (stall cure, marth): the best cure for a path-failed BODY
        // is to take its contents FROM RANGE, not to retry the walk -- the
        // transfer is RemoveItem, which never needed physical reach; arm's
        // reach is a courtesy, not an engine limit. Every path-fail against a
        // specific ref (the off-navmesh pre-gate, or a walked no-progress
        // stall) widens that ref's in-place grab radius by kGrabGrowStep, up
        // to kGrabRadiusMax -- so a body on rubble/stairs the follower stands
        // near but can never path the last gap to gets hoovered a tick later
        // instead of blocklisted (the "stands idle among lootable corpses"
        // stall). NON-LOOSE only: a loose pile is a physical Activate at the
        // item and cannot act from range. The player-bubble and leash gates
        // still apply to a grown grab, and dibs (TierReleased) always applies.
        // Counter clears on a successful grab and on revert; crude size bound
        // (these are transient per-cell verdicts, not worth a real LRU).
        inline std::unordered_map<RE::FormID, int> g_grabGrow;
        constexpr float kGrabGrowStep  = 100.0f;
        constexpr float kGrabRadiusMax = 600.0f;
        inline float GrabRadiusFor(RE::FormID a_id) {
            auto it = g_grabGrow.find(a_id);
            if (it == g_grabGrow.end()) return kArrivalDist;
            return std::min(kArrivalDist + kGrabGrowStep * static_cast<float>(it->second),
                            kGrabRadiusMax);
        }
        inline void NotePathFail(RE::FormID a_id) {
            if (!a_id) return;
            if (g_grabGrow.size() >= 256 && !g_grabGrow.contains(a_id)) g_grabGrow.clear();
            ++g_grabGrow[a_id];
        }

        // STICKY unreachable set. The transient block above is WIPED by the idle
        // reassess (so a body that becomes reachable once the follower moves gets
        // re-tried) -- but a GEOMETRICALLY unreachable target never will: an
        // off-navmesh item gives a short navmesh path that ENDS far from the item,
        // so the follower walks "there", can't close the last gap, and the wipe
        // makes him re-pick it forever (marth's frozen-Erik loop, v0.8.29: arrow
        // 00020169 navdist=18 vs dist=630, ping-ponged with 0002016A). So the
        // SECOND stall on a ref promotes it here: a cooldown the reassess does
        // NOT clear. One stall is still just transient (could be a momentary block);
        // two is a verdict. ONLY a ref the follower ACTUALLY WALKED to and could
        // not close on may strike (the Walking-phase no-progress verdict, plus the
        // loose/unacquirable direct-sticky below) -- the off-navmesh PRE-gate is a
        // heuristic that never attempted a walk, so it stays transient-only
        // (stairs/rubble false-positives were 5-min-poisoning reachable loot: the
        // "follower stands idle among lootable corpses" stall). 60 s (was 5 min):
        // still far above the ~2-4 s churn cycle this set exists to break, but a
        // stale geometric verdict now recovers within a minute. The PRIMARY stall
        // cure is the GROWN GRAB above -- a path-failed body is usually taken
        // from range before it can ever reach this set; the sticky window is only
        // the fallback for bodies beyond even kGrabRadiusMax and loose piles.
        inline std::unordered_map<RE::FormID, Clock::time_point> g_travelUnreach;
        inline std::unordered_map<RE::FormID, int>               g_stallStrikes;
        constexpr auto kTravelStickyCooldown = std::chrono::seconds(60);

        // GATED (loot M1, 2026-09-24, marth: "never return to follow when valid
        // items are reachable ... effective skipping, done intelligently"). A leg
        // whose follower sat in the engine's kMovementBlocked package for
        // kBlockedGate marks its target GATED and ALSO skips the items that lie
        // behind the same block (GatedNow and the cone rule, below MarkTravelSticky),
        // then the excursion continues with the next reachable item. A SKIP,
        // never a verdict: no strike, never sticky, not in g_travelFailed (so
        // the idle reassess neither clears nor feeds it). Re-admitted by a door/
        // lever near it (TESOpenCloseEvent / ACTI|DOOR TESActivateEvent), by the
        // gated item's own cell attaching again -- ONLY when the block may
        // actually have cleared (marth 2026-09-24: "no timer"). An ACTOR in
        // front of him is not a gate: that is a REORDER (FindActorBlocker /
        // g_actorDefer below), never a GATED verdict.
        // The records live below under a mutex (GateSink erases on the
        // main thread); g_gateCount lets every other read skip the lock.
        constexpr auto  kBlockedGate  = std::chrono::seconds(3);   // healthy MB blips ~1 s, freezes 39-45 s (deck-0924b)
        constexpr auto  kEngageWindow = std::chrono::seconds(3);   // engage seen at 1.1-2.4 s (deck-0924b, field-0923b)
        // CH19 concede (review R4): a CH19 leg never engaged by kCh19Concede is
        // handed back. Engage was seen up to ~2.4 s (2nd Walking tick); at the
        // 1 s cadence 5 s is that worst case plus two more ticks, where 3 s left
        // under one. NOT ENGAGED and the theft-guard gate keep kEngageWindow.
        constexpr auto  kCh19Concede  = std::chrono::seconds(5);
        inline std::atomic<int> g_gateCount{ 0 };
        inline bool GatedNow(RE::FormID a_id, Clock::time_point a_now);   // defined below MarkTravelSticky

        inline bool TravelFailedRecently(RE::FormID a_id, Clock::time_point a_now) {
            auto su = g_travelUnreach.find(a_id);
            if (su != g_travelUnreach.end() && a_now < su->second + kTravelStickyCooldown)
                return true;
            auto it = g_travelFailed.find(a_id);
            if (it != g_travelFailed.end() && a_now < it->second + kTravelFailCooldown) return true;
            return g_gateCount.load(std::memory_order_relaxed) > 0 && GatedNow(a_id, a_now);
        }
        inline void MarkTravelFailed(RE::FormID a_id, Clock::time_point a_now) {
            if (a_id) { g_travelFailed[a_id] = a_now; EvictOldest(g_travelFailed); }
        }
        // A STALL (the follower WALKED at the ref and made ZERO progress -- a real
        // attempted-and-failed leg, never a pre-dispatch heuristic) -- transient-
        // block it like any fail, but on the 2nd strike promote it to the sticky
        // set so the idle reassess can't resurrect it into a re-attempt loop.
        inline void MarkTravelStalled(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            MarkTravelFailed(a_id, a_now);
            if (++g_stallStrikes[a_id] >= 2) {
                g_stallStrikes.erase(a_id);
                g_travelUnreach[a_id] = a_now;
                EvictOldest(g_travelUnreach);
                spdlog::info("[loot] {:08X} STICKY-unreachable (2nd stall) -- won't re-pick "
                             "for {}s (survives idle reassess)", a_id,
                             std::chrono::duration_cast<std::chrono::seconds>(kTravelStickyCooldown).count());
            }
        }
        // STRAIGHT to sticky, no strike accrual (loose-loot fix, marth field report:
        // Xelzaz cycled 000D1EFx loose gold forever). Two definitive verdicts skip
        // the 2-strike patience: (1) a LOOSE ref that stalls -- a gold/ammo pile
        // never moves, so one no-progress verdict is GEOMETRIC (off-navmesh, on a
        // table/shelf), not a transient block; and (2) a ref the follower REACHED
        // but could not acquire (Activate dispatch failed) -- arrival ERASED its
        // strikes (3551) so it would otherwise re-pick every 25s. Either way the
        // excursion abandons it for the sticky window instead of looping.
        inline void MarkTravelSticky(RE::FormID a_id, Clock::time_point a_now) {
            if (!a_id) return;
            g_stallStrikes.erase(a_id);
            g_travelFailed[a_id]  = a_now;  EvictOldest(g_travelFailed);
            g_travelUnreach[a_id] = a_now;  EvictOldest(g_travelUnreach);
            spdlog::info("[loot] {:08X} STICKY-unreachable (loose/unacquirable) -- won't re-pick "
                         "for {}s", a_id,
                         std::chrono::duration_cast<std::chrono::seconds>(kTravelStickyCooldown).count());
        }

        // ── GATED items (loot M1, 2026-09-24) ───────────────────────────────────
        // Contract: kBlockedGate above. The records are cross-thread state: the
        // worker marks and reads them, GateSink (Logistics.cpp) erases them on the
        // main thread -> a plain mutex (the g_stockMx precedent);
        // g_gateCount mirrors the size so the scan's hot path (TravelFailedRecently,
        // called from the candidate sort) takes no lock while nothing is gated.
        //
        // "BEHIND THE SAME BLOCK". When a leg is declared blocked the follower is
        // standing AT the obstruction (S = his position; the engine gives him
        // kMovementBlocked because he is pressing into it) and the gated target T
        // lies beyond it. Item I is behind the same block when, from S:
        //   (1) same space: same interior cell, or same worldspace (interior
        //       coordinates of two cells are unrelated);
        //   (2) on T's side: inside the 90-degree cone (+-45) around S->T, flat (xy),
        //       so the room entered through that opening is covered and everything
        //       behind or beside him (the way he came) is not;
        //   (3) past the block, not at it: |SI| >= 64 u and <= |ST| + 1024 u (about
        //       one dungeon room past the target);
        //   (4) on T's floor: |I.z - T.z| <= 256 u (one storey), so a landing above or
        //       a pit below the cone is not swept in.
        // Why this shape: S and T are the only geometry M1 has (navmesh reachability
        // is round M3). A wrong INCLUSION costs nothing permanent: the item re-admits
        // on a door/lever event near it or its cell re-attaching. A wrong
        // EXCLUSION costs one more walk that ends in its own blocked verdict (<= 4 s)
        // and gates it individually. So the rule takes the room behind the opening
        // and stays out of everything on the follower's side.
        constexpr float       kGateConeCos     = 0.70710678f;   // cos 45
        constexpr float       kGateMinBehind   = 64.0f;
        constexpr float       kGateBeyond      = 1024.0f;
        constexpr float       kGateFloorBand   = 256.0f;
        // A lever/door event re-admits every gate whose block OR target is within
        // this radius: a lever sits within sight of its gate, 2048 u (~29 m)
        // covers a large hall. Too wide only costs one re-probe walk.
        constexpr float       kGateEventRadius = 2048.0f;
        constexpr std::size_t kGateMax         = 64;   // memory bound only; eviction is logged

        struct GateRecord {
            RE::FormID        target = 0;
            RE::FormID        follower = 0;
            RE::FormID        space = 0;
            RE::NiPoint3      blockPos{};
            RE::NiPoint3      targetPos{};
            Clock::time_point since{};
            std::unordered_set<RE::FormID> logged;   // GATED-BEHIND lines already printed
        };
        inline std::mutex              g_gateMx;
        inline std::vector<GateRecord> g_gates;   // guarded by g_gateMx

        inline RE::FormID SpaceKey(const RE::TESObjectREFR* a_ref) {
            auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
            if (!cell) return 0;
            if (cell->IsInteriorCell()) return cell->GetFormID();
            auto* ws = a_ref->GetWorldspace();
            return ws ? ws->GetFormID() : 0;
        }
        inline bool BehindBlock(const GateRecord& g, const RE::NiPoint3& p) {
            const float tx = g.targetPos.x - g.blockPos.x, ty = g.targetPos.y - g.blockPos.y;
            const float ix = p.x - g.blockPos.x,           iy = p.y - g.blockPos.y;
            const float lt = std::sqrt(tx * tx + ty * ty), li = std::sqrt(ix * ix + iy * iy);
            if (lt < 1.0f || li < kGateMinBehind || li > lt + kGateBeyond) return false;
            if (std::fabs(p.z - g.targetPos.z) > kGateFloorBand) return false;
            return (tx * ix + ty * iy) / (lt * li) >= kGateConeCos;
        }
        // Caller holds g_gateMx. Every re-admit is logged -- the skip is visible
        // both ways, never a silent list change.
        template <class Pred>
        void EraseGatesIf(const char* a_why, RE::FormID a_by, Pred&& a_pred) {
            std::erase_if(g_gates, [&](const GateRecord& g) {
                if (!a_pred(g)) return false;
                spdlog::info("[loot] GATED {:08X} re-admitted -- {} ({:08X}); items behind its "
                             "block are eligible again", g.target, a_why, a_by);
                return true;
            });
            g_gateCount.store(static_cast<int>(g_gates.size()), std::memory_order_relaxed);
        }
        inline void ReadmitNear(RE::TESObjectREFR* a_ref, const char* a_why) {
            if (!a_ref) return;
            const RE::FormID   space = SpaceKey(a_ref);
            const RE::NiPoint3 p     = a_ref->GetPosition();
            std::lock_guard lk(g_gateMx);
            EraseGatesIf(a_why, a_ref->GetFormID(), [&](const GateRecord& g) {
                return space && g.space == space &&
                       (p.GetDistance(g.blockPos) <= kGateEventRadius ||
                        p.GetDistance(g.targetPos) <= kGateEventRadius);
            });
        }
        inline void ClearGates() {
            std::lock_guard lk(g_gateMx);
            g_gates.clear();
            g_gateCount.store(0, std::memory_order_relaxed);
        }

        inline bool GatedNow(RE::FormID a_id, Clock::time_point) {
            if (!a_id) return false;
            auto*              ref   = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
            const RE::FormID   space = SpaceKey(ref);
            const RE::NiPoint3 pos   = ref ? ref->GetPosition() : RE::NiPoint3{};
            std::lock_guard lk(g_gateMx);
            for (auto& g : g_gates) {
                if (g.target == a_id) return true;
                if (!ref || !space || space != g.space || !BehindBlock(g, pos)) continue;
                if (g.logged.size() < 64 && g.logged.insert(a_id).second)
                    spdlog::info("[loot] {:08X} GATED-BEHIND the block {:08X} ran into on the way to "
                                 "{:08X} -- skipped (a skip, not a failure)", a_id, g.follower, g.target);
                return true;
            }
            return false;
        }

        // a_blockPos (loot M2): where the block happened when it is known better than
        // "where he stands now" -- Harbinger's BLOCKED end reports the stall point, and
        // by the next logistics tick the ended leg may already have let him drift back
        // toward the player. Null = his current position (MFO's own MB timer, M1).
        inline void MarkGated(RE::Actor* a_follower, RE::TESObjectREFR* a_target, float a_blockedSecs,
                              Clock::time_point a_now, const RE::NiPoint3* a_blockPos = nullptr) {
            if (!a_follower || !a_target) return;
            GateRecord g;
            g.target    = a_target->GetFormID();
            g.follower  = a_follower->GetFormID();
            g.space     = SpaceKey(a_target);
            g.blockPos  = a_blockPos ? *a_blockPos : a_follower->GetPosition();
            g.targetPos = a_target->GetPosition();
            g.since     = a_now;
            spdlog::info("[loot] {:08X} target {:08X} GATED -- Movement Blocked {:.1f}s at ({:.0f},{:.0f},{:.0f}), "
                         "{:.0f} u short; skipping it and the items behind the same block (45-degree cone past "
                         "the block, <= +1024 u, same floor) until a door/gate/lever event near it or its "
                         "cell re-attaching. No actor in front of him. No strike, not sticky, no timer.",
                         g.follower, g.target, a_blockedSecs, g.blockPos.x, g.blockPos.y, g.blockPos.z,
                         g.blockPos.GetDistance(g.targetPos));
            std::lock_guard lk(g_gateMx);
            std::erase_if(g_gates, [&](const GateRecord& o) { return o.target == g.target; });
            if (g_gates.size() >= kGateMax) {   // oldest first, and said out loud
                spdlog::info("[loot] GATED {:08X} re-admitted -- gate table full ({})",
                             g_gates.front().target, kGateMax);
                g_gates.erase(g_gates.begin());
            }
            g_gates.push_back(std::move(g));
            g_gateCount.store(static_cast<int>(g_gates.size()), std::memory_order_relaxed);
        }

        // ── ACTOR BLOCK -> REORDER, not a gate (review R2, marth 2026-09-24: "During
        // an actor block, do a loot reorder, not a loot drop. That way two of our
        // followers blocking each other both reverse direction.") ───────────────
        // At the Movement Blocked verdict, is a living ACTOR (the player, a
        // follower, any NPC) right in front of him? FORWARD = either his facing
        // (GetAngleZ: +Y at 0, x = sin, y = cos) or the straight line S->T -- the
        // path can bend toward a doorway before it points at T, and he faces the
        // way he is walking. Numbers:
        //   kActorBlockReach 200 u: two humanoid capsules in contact sit ~50-70 u
        //     apart center to center, so a jam is well inside it, with room for a
        //     second actor one step behind the first; a guard standing further back
        //     behind a real portcullis is outside it.
        //   kActorBlockCos 45 degrees each side: a doorway jam is shoulder to
        //     shoulder, off the axis; narrower misses exactly that case.
        //   kActorBlockZ 128 u: same floor / same stair flight, not a balcony.
        // Worker-thread read: the follower's and the player's ATTACHED cells walked
        // with TESObjectCELL::ForEachReferenceInRange (the cell's own lock, the
        // Logistics_Loot / Economy precedent), plus the player explicitly.
        constexpr float kActorBlockReach = 200.0f;
        constexpr float kActorBlockCos   = 0.70710678f;
        constexpr float kActorBlockZ     = 128.0f;
        struct ActorBlocker { RE::FormID id = 0; RE::NiPoint3 pos{}; float dist = 0.0f; };
        inline ActorBlocker FindActorBlocker(RE::Actor* a_f, RE::TESObjectREFR* a_t) {
            ActorBlocker best;
            if (!a_f || !a_t) return best;
            const RE::NiPoint3 S  = a_f->GetPosition();
            const float        az = a_f->GetAngleZ();
            const float        hx = std::sin(az), hy = std::cos(az);
            float tx = a_t->GetPosition().x - S.x, ty = a_t->GetPosition().y - S.y;
            const float lt = std::sqrt(tx * tx + ty * ty);
            if (lt > 1.0f) { tx /= lt; ty /= lt; } else { tx = hx; ty = hy; }
            float bestD = kActorBlockReach + 1.0f;
            auto consider = [&](RE::Actor* x) {
                if (!x || x == a_f || x->IsDead() || x->IsDisabled() || x->IsMarkedForDeletion()) return;
                const RE::NiPoint3 P = x->GetPosition();
                const float dx = P.x - S.x, dy = P.y - S.y, d = std::sqrt(dx * dx + dy * dy);
                if (d < 1.0f || d > kActorBlockReach || std::fabs(P.z - S.z) > kActorBlockZ) return;
                if ((dx * hx + dy * hy) / d < kActorBlockCos && (dx * tx + dy * ty) / d < kActorBlockCos) return;
                if (d < bestD) { bestD = d; best = { x->GetFormID(), P, d }; }
            };
            auto* pc = RE::PlayerCharacter::GetSingleton();
            consider(pc);
            RE::TESObjectCELL* cells[2] = { a_f->GetParentCell(), pc ? pc->GetParentCell() : nullptr };
            if (cells[1] == cells[0]) cells[1] = nullptr;
            for (auto* c : cells) {
                if (!c || !c->IsAttached()) continue;
                c->ForEachReferenceInRange(S, kActorBlockReach, [&](RE::TESObjectREFR& r) {
                    consider(r.As<RE::Actor>());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            return best;
        }
        // Per-follower reorder record. The blocked item stays VALID; it only sorts
        // after his other valid candidates, and items that lie toward the blocker
        // sort after the ones leading away, so two jammed followers each turn
        // around. Erased when the deferred item's turn comes (it is dispatched
        // again), when he has no excursion, and on revert. Worker-only, no lock.
        struct ActorDefer { RE::FormID item = 0; RE::FormID blocker = 0; RE::NiPoint3 blockerPos{}; };
        inline std::unordered_map<RE::FormID, ActorDefer> g_actorDefer;

        // THE candidate order (review R1). Every key -- failed/gated, the actor-
        // block tier, the distance -- is taken ONCE per candidate before the sort,
        // so a main-thread gate erase mid-sort cannot flip a key between two
        // comparisons (a non-strict-weak comparator is UB in std::sort). Tiers:
        // 0 free, 1 free but toward this follower's blocker, 2 his deferred item,
        // 3 failed-recently or GATED (deprioritized, never removed; unresolvable
        // handles too). Closest first inside a tier. With no defer record this is
        // exactly the old two-tier "un-failed first, then closest" order.
        inline void SortLootCandidates(std::vector<RE::ObjectRefHandle>& a_c, RE::Actor* a_f,
                                       const RE::NiPoint3& a_origin, Clock::time_point a_now) {
            const ActorDefer* df = nullptr;
            if (a_f)
                if (auto it = g_actorDefer.find(a_f->GetFormID()); it != g_actorDefer.end()) df = &it->second;
            const float bx = df ? df->blockerPos.x - a_origin.x : 0.0f;
            const float by = df ? df->blockerPos.y - a_origin.y : 0.0f;
            const float bl = std::sqrt(bx * bx + by * by);
            struct Keyed { int tier; float dist; RE::ObjectRefHandle h; };
            std::vector<Keyed> k;
            k.reserve(a_c.size());
            for (auto& h : a_c) {
                auto p = h.get();
                if (!p) { k.push_back({ 3, 1e30f, h }); continue; }
                const RE::FormID   pid = p->GetFormID();
                const RE::NiPoint3 P   = p->GetPosition();
                int tier = 0;
                if (TravelFailedRecently(pid, a_now)) tier = 3;
                else if (df && pid == df->item)      tier = 2;
                else if (df && bl > 1.0f) {
                    const float dx = P.x - a_origin.x, dy = P.y - a_origin.y, d = std::sqrt(dx * dx + dy * dy);
                    if (d > 1.0f && (dx * bx + dy * by) / (d * bl) >= kActorBlockCos) tier = 1;
                }
                k.push_back({ tier, a_origin.GetDistance(P), h });
            }
            std::sort(k.begin(), k.end(), [](const Keyed& a, const Keyed& b) {
                return a.tier != b.tier ? a.tier < b.tier : a.dist < b.dist;
            });
            for (std::size_t i = 0; i < k.size(); ++i) a_c[i] = k[i].h;
        }

        // IDLE REASSESS (marth: "to be fair, reassess all nearby bodies if nothing
        // else matches for a few cycles"). The blocklist is a churn-guard, not a
        // verdict: a body skipped as unreachable may be reachable once the follower
        // has moved, and a body marked DONE is (post-StripCorpse) genuinely empty
        // so re-scanning it is a cheap HasLoot=false, never a wasted trip. When a
        // follower services NOTHING for a few consecutive idle ticks and no
        // excursion is running, wipe the blocklist so the next scan looks at every
        // body fresh. Bounded to once per window so a truly-unreachable body can't
        // make him re-attempt it every few seconds.
        inline std::unordered_map<RE::FormID, int> g_idleCycles;
        inline Clock::time_point            g_lastBlocklistReassess{};
        constexpr int  kIdleReassessCycles   = 4;                       // ~4 s idle
        constexpr auto kReassessCooldown      = std::chrono::seconds(15);

        // fBatchLinger as a duration. The default is sub-second-grained (1.5 s),
        // so it must NOT truncate through whole seconds -- convert via ms.
        inline std::chrono::milliseconds BatchLingerDur() {
            return std::chrono::milliseconds(
                static_cast<int>(Config::g_batchLinger.load() * 1000.0f));
        }

        // Travel deadline scaled to the distance: enough time to actually walk
        // there at a jog, never so long the follower is stuck if the path is
        // blocked. ~150 u/s effective + 5 s slack, clamped to [6, 20] s.
        inline Clock::time_point TravelDeadline(float a_dist, Clock::time_point a_now) {
            const float secs = std::clamp(a_dist / 150.0f + 5.0f, 6.0f, 20.0f);
            return a_now + std::chrono::seconds(static_cast<int>(secs));
        }

}
