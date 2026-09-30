// cast/Hands.cpp (was native/Actuation_Hands.cpp; moved whole by the wave-1
// subsystem-folder split, 2026-09-24) -- THE PER-HAND CAST LOCK. Split off Actuation.cpp
// mechanically on 2026-09-08 (it stood at 3282 lines, over the project's 2500-line
// hard cap): NO logic changed, nothing was reordered, and every line below is the
// line that was there. This TU owns the lock's IMPLEMENTATION -- the per-hand
// hold/release, the liveness tests (claim live, cast in flight, stale), the rank
// preemption, the hand-free test, and THE JUGGLE (ResolveCastHand). Its shared
// STATE (g_castLock and the three rate-limited log maps, g_firingRule and its ally
// threshold, CastLock / HandPlan, the hand indices) moved to Actuation_internal.h,
// because Actuation.cpp's Fire / CastOn / ConcentrationCast and ClearCastLock(s)
// read and write it across the cut. Anything used on ONE side only stayed
// file-local -- here in the anon-namespace regions below, or over in Actuation.cpp
// (NearestAlly, ForceCast, and the APMF-refusal log, whose names are deliberate
// twins of Actuation_Direct.cpp's and so must stay internal in BOTH TUs).
#include "Actuation_internal.h"
#include "apmf/APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps
#include <limits>         // rank preemption: kNoRule sentinel (numeric_limits<int>::max)

namespace MFO::Actuation {

    namespace {

        inline const char* HandName(std::size_t a_hand) { return a_hand == kHandLeft ? "left" : "right"; }

        void LogCastLockHold(RE::FormID a_follower, std::size_t a_hand,
                             RE::FormID a_wantedSpell, RE::FormID a_lockedSpell) {
            const auto now = std::chrono::steady_clock::now();
            auto& entry = g_lastLockLog[a_follower].hand[a_hand];
            if (entry.first == a_wantedSpell &&
                std::chrono::duration<float>(now - entry.second).count() < 2.0f)
                return;
            entry.first = a_wantedSpell; entry.second = now;
            spdlog::info("[eval] {:08X} cast gambit HELD OFF -- spell {:08X} wants the "
                         "follower's {} hand, spell {:08X} is still firing there",
                         a_follower, a_wantedSpell, HandName(a_hand), a_lockedSpell);
        }

    }   // anon

    void LogCastInFlight(RE::FormID a_follower, std::size_t a_hand, RE::FormID a_spell,
                         bool a_dual) {
        const auto now = std::chrono::steady_clock::now();
        auto& entry = g_lastInFlightLog[a_follower].hand[a_hand];
        if (entry.first == a_spell &&
            std::chrono::duration<float>(now - entry.second).count() < 2.0f)
            return;
        entry.first = a_spell; entry.second = now;
        spdlog::info("[eval] {:08X} cast gambit SATISFIED IN FLIGHT -- spell {:08X} is already "
                     "running on the {} hand; claim refreshed in place (no re-claim, no lock "
                     "re-stamp) and the scan CONTINUES to the rules below it",
                     a_follower, a_spell, a_dual ? "both hands (dual-cast)" : HandName(a_hand));
    }

    // Establish/refresh ONE hand's lock -- call whenever CastOn/
    // ConcentrationCast commits to an outcome that occupies THAT hand
    // ACROSS ticks: a live APMF cast claim (offense or heal) on it, or a
    // genuine concentration stream (claimed via Task 1, or plain
    // direct-force) using it. a_target == 0 for self.
    void HoldCastLock(RE::FormID a_follower, std::size_t a_hand,
                      RE::FormID a_spell, RE::FormID a_target) {
        auto& lock = g_castLock[a_follower].hand[a_hand];
        // A new spell or recipient is a new channel: its stream cap starts over.
        if (lock.spell != a_spell || lock.target != a_target) {
            lock.channelSince = {};
            lock.channelCap   = 0.0f;
        }
        lock.spell = a_spell; lock.target = a_target;
        lock.lastSeen = std::chrono::steady_clock::now();
        lock.claimGoneAt = {};   // fresh hold == fresh cast: reset F8's cap window
        // A waiter was waiting on the OLD owner's charge. A different rule taking
        // the hand ends that wait (the new owner is judged afresh). The SAME rule
        // re-holding (a re-claim after its claim lapsed) keeps it: the waiter is
        // still waiting on the same rule's casts. See CastLock::waiterRule.
        if (lock.owningRule != g_firingRule) {
            lock.waiterRule = kNoRule;
            lock.waitSince  = {};
            lock.waitLast   = {};
        }
        // RANK, carried not inferred: the rule Fire() is acting for owns this
        // hand until it lets go. See CastLock::owningRule.
        lock.owningRule = g_firingRule;
    }

    // Drop ONE hand's lock (2026-09-08). The whole-follower twin is the public
    // ClearCastLock in Actuation.cpp (`:2127`, with ClearCastLocks at `:2143`,
    // where they sit beside the other session-scoped clears); this is the per-hand form the
    // in-flight gate needs when the claim a lock named turns out to be gone --
    // the other hand's independent lock must survive that.
    void ClearCastLockHand(RE::FormID a_follower, std::size_t a_hand) {
        if (auto it = g_castLock.find(a_follower); it != g_castLock.end())
            it->second.hand[a_hand] = CastLock{};
    }

    namespace {

        // Does MFO hold a LIVE cast claim on THIS hand right now? Heals are LEFT
        // always (APMFBridge::ClaimHealCast's own hard rule), so the heal facet
        // only ever answers for the left hand. Factored out of CastLockLive
        // (2026-09-08) because the in-flight gate below asks the same question
        // and the two must never drift apart.
        bool ClaimLiveOnHand(RE::FormID a_follower, std::size_t a_hand) {
            return (a_hand == kHandLeft)
                ? (APMFBridge::IsOwnedCastActiveOnHand(a_follower, APMFBridge::kApmfHandLeft) ||
                   APMFBridge::IsHealCastActive(a_follower))
                : APMFBridge::IsOwnedCastActiveOnHand(a_follower, APMFBridge::kApmfHandRight);
        }

    }   // anon

    // THE DELIVERY-FLIP PROXY STANDING ON ONE HAND, WHICHEVER FACET MINTED IT
    // (review fix, 2026-09-08). An offense claim's proxy lives in
    // GetOffenseCastProxy and a HEAL claim's in GetHealCastProxy, and heals are
    // LEFT always -- so a left-hand lookup that consults only the offense
    // accessor silently answers 0 for every proxied heal. That 0 is not
    // harmless at either call site: the in-flight watch re-arm would never
    // learn the heal's proxy (WatchArmed keeps whatever it has when handed 0),
    // so `observed` could never latch, every proxied heal would warn
    // "[cfc] NO observed cast" forever and read as "not ours" in the SpellSink
    // -- the RC4 false alarm, rebuilt. ONE resolver so the two sites cannot
    // drift. Both accessors answer 0 for a claim that is already gone.
    RE::FormID CastProxyOnHand(RE::FormID a_follower, std::size_t a_hand) {
        const RE::FormID offense = APMFBridge::GetOffenseCastProxy(
            a_follower, a_hand == kHandLeft ? APMFBridge::kApmfHandLeft
                                            : APMFBridge::kApmfHandRight);
        if (offense != 0 || a_hand != kHandLeft) return offense;
        return APMFBridge::GetHealCastProxy(a_follower);
    }

    namespace {

        // ── DOES A WEAPON OWN THIS FOLLOWER'S RIGHT HAND, OR IS ONE COMING BACK? ──
        // (review finding, 2026-09-08.) The gate on right-first hand preference.
        //
        // THE FIELD SHAPE IT AVOIDS, which is documented and was paid for once
        // (Docs/CAST-DELIVERY.md, the 2026-09-05 HAND FIX): a follower was
        // TRANSIENTLY UNARMED in a facet-expiry gap, the cast claim's auto hand
        // resolution took the RIGHT hand because it read free, the equip gambit's
        // own periodic re-equip put the weapon back into that hand ~500 ms later
        // and displaced the spell, and the cast never left rest. The rule written
        // then -- a force-held weapon owns the right hand, a spell claims LEFT --
        // is why heals are LEFT-only and why PlanCastHand refuses anything but
        // Left while APMFBridge::WeaponHandActive is true.
        //
        // WHY THAT GUARD IS NOT ENOUGH HERE. WeaponHandActive reads the LIVE grip
        // and the live APMF equipment CLAIM (APMFBridge.cpp) -- and the gap is
        // exactly the moment both are false: the hand is empty and the claim has
        // expired, while MFO's own force-hold still intends to put the weapon
        // back. PlanCastHand therefore returns EitherFree in that gap, and making
        // right the default landing spot would reproduce the 2026-09-05 shape for
        // every either-free offense cast on a melee or hybrid follower.
        //
        // SO ADD THE DURABLE SIGNAL, not a timer: g_forcedWeapon is Actuation.cpp's
        // T#76 force-hold ledger -- defined and written there, visible to this TU
        // only through the extern in Actuation_internal.h, and READ-ONLY here --
        // (the weapon EquipWeapon is holding for this
        // follower), released by ReconcileForcedWeapon when the equip gambit's
        // condition is known false -- it does NOT lapse with an APMF facet expiry,
        // which is precisely the gap the live reads miss. An entry here means "a
        // weapon is coming back to the right hand", whether or not it is in the
        // hand this instant. Read under g_forcedMx: the map has an OFF-THREAD
        // reader (the SKSE save callback) and every access guards it.
        //
        // ONE RESIDUAL, STATED: the ledger is only written while
        // bWeaponStyleControl is ON (EquipWeapon's else branch is a plain
        // EquipObject with no ledger entry, and ReconcileForcedWeapon releases
        // unconditionally when the switch is off). With the kill-switch off a
        // melee follower in the transient-unarmed gap therefore still lands RIGHT
        // -- the 2026-09-05 shape, on a non-default config. Recorded rather than
        // papered over; closing it needs a signal that does not depend on that
        // feature being on.
        //
        // A pure caster never has an entry, so right-first still applies to
        // exactly the follower marth's ruling was about ("theres two hands, auto
        // should figure it out") and never to one whose right hand is spoken for.
        bool WeaponHandExposure(RE::Actor* a_follower) {
            if (!a_follower) return false;
            if (APMFBridge::WeaponHandActive(a_follower)) return true;   // live grip / live claim
            // UNDER g_forcedMx like every other access to this map (see its
            // declaration): its off-thread reader is the SKSE SAVE callback, so
            // "the writers are worker-serial" is not enough. Nothing else is held
            // here and no engine call sits inside the lock.
            std::scoped_lock lk(g_forcedMx);
            return g_forcedWeapon.contains(a_follower->GetFormID());     // ... or coming back
        }

    }   // anon

    std::chrono::steady_clock::time_point CastLockClaimStamp(RE::FormID a_follower, std::size_t a_hand,
                                                             RE::FormID a_spell) {
        if (a_hand >= kHandCount || a_spell == 0) return {};
        const auto it = g_castLock.find(a_follower);
        if (it == g_castLock.end()) return {};
        const auto& lk = it->second.hand[a_hand];
        return lk.spell == a_spell ? lk.lastSeen : std::chrono::steady_clock::time_point{};
    }

    void ClearLeftCastLockIf(RE::FormID a_follower, RE::FormID a_spell) {
        const auto it = g_castLock.find(a_follower);
        if (it == g_castLock.end() || a_spell == 0) return;
        if (it->second.hand[kHandLeft].spell == a_spell) it->second.hand[kHandLeft] = CastLock{};
    }

    // ── CROSS-TU (2026-09-09) ────────────────────────────────────────────────
    // CastInFlightOnHand was file-local until the heal claim path needed it.
    // ComposedCast.cpp is not one of the three Actuation TUs and cannot include
    // Actuation_internal.h, so the predicate moved OUT of the anonymous namespace
    // to namespace scope (declared in Actuation.h). The BODY is unchanged and it
    // stays the single definition of "in flight"; only its linkage and its
    // indentation moved. Every caller below still reaches it by plain lookup.

    // ── "THE ENGINE STARTED THIS CAST AND HAS NOT FINISHED IT" (F8, 2026-09-08) ──
    // The signal that a charged cast is mid-flight on `a_hand`, read from the
    // engine itself rather than inferred: the follower's MagicCaster for that
    // hand is in a live cast state AND the spell it currently has selected is
    // the one this hand is locked to (or the delivery-flip PROXY APMF minted
    // for it -- a proxied claim never has the original spell selected, the
    // same trap ComposedCast's watch already documents).
    //
    // WHY NOT ComposedCast's `observed` LATCH. That latch is set from
    // Diagnostics' SpellSink, i.e. when a cast actually FIRES -- it answers
    // "did this claim ever deliver", which is the END of the window this
    // needs, not the start of it. There is no MFO-side "charge began" event
    // at all; the caster's own state IS that event.
    //
    // NO VIRTUAL CALL HERE -- THE ARRAY IS READ DIRECTLY, ON PURPOSE (review
    // fix, 2026-09-08; CLAUDE.md principle 6). The obvious spelling is
    // `Actor::GetMagicCaster(CastingSource)` (`RE/A/Actor.h:302`, vtable slot
    // 0x5C), and the bDriveCaster probe further down this file does exactly
    // that -- but that probe is DEFAULT-OFF, and this predicate runs on EVERY
    // in-flight lap of EVERY cast gambit, on the AddTask job worker. If the
    // engine's GetMagicCaster LAZILY ALLOCATES the ActorMagicCaster it hands
    // back, that is an off-main allocation plus a write into
    // `magicCasters[]`, from a thread that is not allowed to do either. That
    // is unknowable from CommonLib: it declares the function and implements
    // NOTHING (grep: only the `override` declaration exists in the pinned
    // tree), so the body is the game's. It is also not checkable here --
    // SkyrimSE.exe is Steam-DRM encrypted on disk (a `.bind` section; verified
    // on the local 1.6.1170 copy), so a static disassembly reads garbage, and
    // this is precisely the class of read that once turned a "passive" probe
    // into a crash.
    //
    // So the question is REMOVED rather than answered: read the already-built
    // caster out of the actor's own array and treat a null slot as "not in
    // flight". A plain member load allocates nothing, dispatches nothing, and
    // needs no proof beyond the layout.
    //
    // SYMBOLS VERIFIED against the PINNED CommonLibSSE-NG (3.7.0 @ c4ab853d):
    //   * `ACTOR_RUNTIME_DATA::magicCasters[SlotTypes::kTotal]`, `Actor.h:667`
    //     @0x1A0, reached through `GetActorRuntimeData()` (`Actor.h:698-706`)
    //     which applies the 0xE0/0xE8 SE-vs-AE shift for us -- the same
    //     accessor MFO already uses in five places, from this same worker
    //     (e.g. `Actuation_Direct.cpp:1569`, `CasterConsent.cpp:851`).
    //     `static_assert(sizeof(Actor) == 0x2B0)` pins the layout.
    //   * `Actor::SlotTypes::kLeftHand == 0` / `kRightHand == 1`
    //     (`Actor.h:139-150`) -- the same numeric values as
    //     `MagicSystem::CastingSource`, but this indexes the ARRAY, so the
    //     array's own enum is what it is written against.
    //   * `ActorMagicCaster : public MagicCaster` at offset 00
    //     (`ActorMagicCaster.h:18-21`), so the stored pointer is a MagicCaster
    //     with no adjustment, and `currentSpell` (MagicItem*, 0x28) and
    //     `state` (stl::enumeration<State, uint32_t>, 0x30) are its own real
    //     members (`MagicCaster.h:80-95`).
    //
    // BOTH READS ARE RACY PLAIN LOADS and are meant to be: the combat thread
    // advances this state machine while we look. A torn or stale answer only
    // shifts the hold by one round-robin lap (~133 ms x party), which is
    // nothing against the 2.3-4.5 s pipeline it is protecting -- and it can
    // only ever make the lock let go one lap early or hold one lap late,
    // never corrupt anything.
    //
    // STATES. kNone is idle; kUnk08/kUnk09 are the interrupt/deselect pair
    // (CommonLib's own annotation), i.e. a cast that is ENDING and must not
    // hold the hand. Everything between is a cast in progress -- request,
    // charge, ready, casting -- and kUnk07 is deliberately INSIDE that range:
    // it is unnamed, it sits immediately before the interrupt states, and
    // treating an unknown mid-sequence state as "already done" is the failure
    // mode this exists to prevent.
    bool CastInFlightOnHand(RE::Actor* a_follower, std::size_t a_hand,
                            RE::FormID a_spell, RE::FormID a_proxy) {
        if (!a_follower || a_spell == 0) return false;
        const std::size_t slot = a_hand == kHandLeft
            ? static_cast<std::size_t>(RE::Actor::SlotTypes::kLeftHand)
            : static_cast<std::size_t>(RE::Actor::SlotTypes::kRightHand);
        RE::MagicCaster* caster = a_follower->GetActorRuntimeData().magicCasters[slot];
        if (!caster) return false;                 // never built -> nothing in flight
        auto* held = caster->currentSpell;
        if (!held) return false;
        const auto cur = held->GetFormID();
        if (cur != a_spell && (a_proxy == 0 || cur != a_proxy)) return false;
        const auto st = caster->state.get();
        return st != RE::MagicCaster::State::kNone &&
               st != RE::MagicCaster::State::kUnk08 &&
               st != RE::MagicCaster::State::kUnk09;
    }

    namespace {

        // HOW LONG THE IN-FLIGHT EXTENSION BELOW MAY RUN AFTER THE CLAIM BEHIND IT
        // WENT AWAY (F8; anchored on CastLock::claimGoneAt, NOT on `lastSeen` --
        // see that field). Sized from a real number, not chosen (#9):
        // APMFBridge::kHealCastTtlMs is the TTL APMF grants every cast claim MFO
        // makes -- the longest window APMF itself will let one cast stand without
        // a renewal. A cast the engine has not finished inside that is not one
        // this is protecting, it is one that is STUCK, and going on holding the
        // hand for it would hide the stall instead of showing it (principle 7).
        // It clears the measured pipeline with room to spare. The two numbers this
        // file quotes are DIFFERENT measurements and neither is a range of the
        // other -- stated once here because two comments phrased them as if they
        // were: 2.3-2.5 s is an offense cast's equip + charge (claim to first
        // CHARGE STATE), and 4.5 s is claim to observed FIRE for the one heal the
        // 2026-09-08 session landed. "2.3-4.5 s" as used elsewhere in this file is
        // the span those two bracket, not a single measured quantity.
        constexpr auto kInFlightHoldCap = std::chrono::milliseconds(APMFBridge::kHealCastTtlMs);

        // WHY CanPreemptHand last said yes on each hand (field 2026-09-29), for
        // PreemptHand's [eval] line, which runs later in the same Fire() (CastOn's
        // commitPreempt). Worker-serial (#4). String literals only.
        const char* g_preemptWhy[kHandCount] = { "the caster was idle between casts",
                                                 "the caster was idle between casts" };

        // Is hand `a_hand`'s lock still LIVE, i.e. is the follower still
        // genuinely occupied on THAT hand right now? "release on completion,
        // on claim release/TTL expiry" (marth): a live APMF cast claim on that
        // SPECIFIC hand is authoritative proof by itself, checked FIRST so a
        // claim that ends EARLY (a heal topping off well inside the window, or
        // APMF's own TTL elapsing) frees the hand immediately rather than
        // riding out a timer. Heals are LEFT always (APMFBridge::
        // ClaimHealCast's hard rule), so a live heal claim only ever proves the
        // LEFT hand busy. Absent a live claim (APMF off/absent, or a plain
        // un-claimed direct-force concentration stream -- its registry is
        // private to Actuation_Direct.cpp's own TU and not queryable from
        // here), fall back to the SAME round-robin-aware staleness window
        // every other facet in this codebase already sizes against (#9: a
        // floor is safe, a guessed budget is not) -- reused via APMFBridge::
        // FacetExpiry(), never separately invented. A still-winning gambit
        // re-fires (and re-Holds its hand's lock) every round-robin lap, well
        // inside this window, so the SAME gambit never sees its own lock go
        // stale; only a gambit that stopped being requested at all does.
        //
        // THIRD ANSWER since F8 (2026-09-08): past that staleness window, a hand
        // whose ENGINE CASTER is still mid-cast of the locked spell is also live
        // -- see the block inside. It takes an RE::Actor* now (rather than a bare
        // FormID) for exactly that read; every caller already had one.
        bool CastLockLive(RE::Actor* a_follower, std::size_t a_hand, CastLock& a_lock) {
            const auto fid = a_follower ? a_follower->GetFormID() : 0;
            const auto now = std::chrono::steady_clock::now();
            if (fid != 0 && ClaimLiveOnHand(fid, a_hand)) {
                a_lock.claimGoneAt = {};   // the claim is here: F8's cap window is not open
                return true;
            }
            const float elapsed = std::chrono::duration<float>(now - a_lock.lastSeen).count();
            if (elapsed <= std::chrono::duration<float>(APMFBridge::FacetExpiry()).count())
                return true;
            // ── F8: A CHARGED CAST IS NOT FREE REAL ESTATE ─────────────────────
            // The claim can lapse (APMF's TTL, or MFO's own sweep) WHILE the
            // engine is still charging the spell it was made for -- and that is
            // exactly when the field saw casts thrown away: three fully Charged
            // MFO casts were discarded by MFO's own next claim, because with the
            // claim gone this function said "stale, take the hand" and the next
            // rule re-pointed it. So a hand whose engine caster is mid-cast of
            // the locked spell stays LOCKED even with no claim behind it: the
            // in-flight cast finishes, and the rule that wants the hand is held
            // off transparently for the ~1-2 s that takes (the SAME "spell gambit
            // locks until complete" contract the heal slot already has).
            //
            // BOUNDED, so a wedged caster cannot own the hand forever -- see
            // kInFlightHoldCap. Past the cap the lock goes stale exactly as it
            // did before this existed, and the stall becomes visible.
            // THE CAP RUNS FROM HERE, NOT FROM `lastSeen`. This line is the first
            // moment the third answer is reached at all -- past the staleness
            // window with no claim standing -- so it is the moment the in-flight
            // protection starts, and the moment its 6 s bound must start with it.
            // See CastLock::claimGoneAt for what anchoring it on `lastSeen` cost.
            if (a_lock.claimGoneAt.time_since_epoch().count() == 0) a_lock.claimGoneAt = now;
            if (now - a_lock.claimGoneAt >= kInFlightHoldCap) return false;
            // The proxy this hand's claim minted, offense or heal (CastProxyOnHand).
            // 0 for a claim that is already gone -- harmless here:
            // CastInFlightOnHand then matches on the ORIGINAL spell alone, and a
            // proxied cast whose claim has vanished simply does not match, so the
            // hand frees exactly as it did before.
            return CastInFlightOnHand(a_follower, a_hand, a_lock.spell,
                                      CastProxyOnHand(fid, a_hand));
        }

    }   // anon

    // THE ONE QUESTION THE DUAL-WIELD EQUIP ASKS BEFORE TAKING A HAND (2026-09-13,
    // declared in Actuation_internal.h). Same two answers HandFree/CastLockLive
    // give the cast side, in the same order: a live claim on the hand is
    // authoritative by itself (a heal claim -- ComposedCast's path -- holds NO
    // g_castLock entry at all, so this first test is the only one that sees it);
    // otherwise the hand's lock, if any, judged by CastLockLive (which also
    // stamps/clears its F8 claimGoneAt exactly as HandFree's call would). A dead
    // lock is NOT cleared here -- HandFree owns that -- reading it as free is
    // all the equip side needs.
    bool CastHandHeld(RE::Actor* a_follower, std::size_t a_hand) {
        if (!a_follower || a_hand >= kHandCount) return false;
        const auto fid = a_follower->GetFormID();
        if (ClaimLiveOnHand(fid, a_hand)) return true;
        auto it = g_castLock.find(fid);
        if (it == g_castLock.end()) return false;
        auto& lock = it->second.hand[a_hand];
        if (lock.spell == 0) return false;
        return CastLockLive(a_follower, a_hand, lock);
    }

    namespace {

        // Is hand `a_hand` available for (a_spell,a_target) right now -- i.e.
        // unlocked, already locked to this EXACT (spell,target) (the SAME
        // gambit refreshing itself), or its lock has gone live-false/stale
        // (dropped right here, in which case it is free)? a_busyOut is set to
        // whichever DIFFERENT spell currently holds it when this returns false.
        //
        // `a_incumbentOut` (F9, 2026-09-08): set when this hand is free BECAUSE
        // THE CALLER ALREADY OWNS IT -- the lock names this exact (spell,target),
        // i.e. the requesting rule is the one whose cast is already running here.
        // It is deliberately NOT the same thing as "free": an unlocked hand and a
        // hand we are already casting on both return true, and only the second one
        // must skip the whole claim/equip/consent path. Left untouched on every
        // other return, so a caller may pass one variable across both hands.
        // ── RANK PREEMPTION (marth's ruling, 2026-09-08) ───────────────────────
        // marth, verbatim: *"Do you mean gambit ordering? Thats intentional. If you
        // mean an offensive cant be overriden by a heal, theres two hands, auto
        // should figure it out, and secondly, a heal should absolutely do so
        // anyway. However gambits are already configured correctly with heals near
        // the top that should overwrite the offense claim, or at leats submit a
        // second auto."*
        //
        // So the gambit list order IS the priority order, and a higher-ranked rule
        // must be able to TAKE a hand from a lower-ranked incumbent -- not be held
        // off transparently, and not wait for the incumbent's condition to go
        // false. Before this, F8's incumbent pin plus F9's in-flight refresh sealed
        // the churn gap a heal used to slip through, which made the starvation
        // DETERMINISTIC rather than occasional; this is the other half of that
        // change, not a new policy.
        //
        // HOW RANK IS DECIDED, WITHOUT INVENTING A PRIORITY SCHEME. The rule index
        // IS the priority, full stop -- so this CARRIES that number rather than
        // deducing it: `CastLock::owningRule` records which gambit claimed the hand,
        // `g_firingRule` is the gambit asking for it now, and the comparison is the
        // whole test. Lower index == higher priority; strictly lower may take the
        // hand, equal is the incumbent itself (IsOwnRetarget), higher is held off.
        //
        // WHAT THIS REPLACED, and why the replacement was necessary rather than
        // tidier (Fable review of ed9cbbc). The first cut READ rank off the scan --
        // "did this hand's incumbent already assert itself earlier in this same
        // lap?" -- which is true often enough to look right and false in two
        // reachable cases: a rule whose condition is TRUE but which exits
        // transparently BEFORE the hand gate (#68 out-of-range, the magicka cost
        // and reserve exits, HasSpell) never asserts at all, so two such rules
        // could take the hand from each other on alternate laps and neither ever
        // reach a charge; and a rule RE-AIMING its own cast read as outranking
        // itself. A carried index has neither failure, and needs no minimum-tenure
        // window -- which matters, because a tenure window would delay exactly the
        // heal marth's ruling says must take the hand at once.
        //
        // AND NEVER MID-CHARGE. Preemption must not resurrect the charged-cast loss
        // F8 just removed, so it is refused while the engine caster on that hand is
        // running the incumbent's cast: only the idle window BETWEEN casts is
        // takeable. That reuses CastInFlightOnHand rather than inventing a second
        // notion of "busy" -- there is one such notion in this file and this is it.
        //
        // ...EXCEPT THAT "NEVER MID-CHARGE" ONLY COVERS THE CHARGE ALREADY RUNNING
        // (field 2026-09-29). A lower-ranked offense whose claim stands re-charges
        // back to back, so the caster is almost never idle on the lap a higher
        // rule looks, and the heal above it starved for ~10 s (Jesper: rule 1 Fast
        // Healing held off by rule 7 Incinerate, 14:30:00.1 to 14:30:09.9, while
        // two allies went down). So the first refusal STAMPS the asker as the
        // hand's waiter (CastLock::waiterRule), and the hand goes to it as soon as
        // the charge that was running then is over:
        //   * BOUNDARY -- the incumbent has FIRED since the wait began
        //     (ComposedCast::ObservedFiring over exactly that span). Whatever it
        //     is charging now started while a higher rule was waiting, and is not
        //     protected. At most one lap of that new charge is lost, never the
        //     charge the protection was for.
        //   * CAP -- the wait has lasted kInFlightHoldCap (APMF's cast TTL, the
        //     longest one cast may stand, see its doc). A fire the watch could not
        //     see (an unlearned proxy) must not starve the waiter forever.
        //   * URGENT HEAL (a_urgentHeal, decided by CastOn) -- the asker is a heal
        //     whose recipient is DOWN or under the rule's threshold, and the
        //     incumbent is an OFFENSE spell. It takes the hand at once, mid-charge
        //     included: marth's ruling, "a heal near the top must take the hand at
        //     once". A lower-ranked HEAL incumbent is still protected mid-charge
        //     (throwing one heal away for another is not what the ruling asks).
        // Rank is still the carried index and nothing else. There is no tenure:
        // the waiter never waits on a timer of its own, only on the incumbent's
        // charge (or the cap that bounds a stuck one).
        bool CanPreemptHand(RE::Actor* a_follower, std::size_t a_hand, CastLock& a_lock,
                            bool a_urgentHeal) {
            const auto fid = a_follower ? a_follower->GetFormID() : 0;
            if (fid == 0) return false;
            // RANK, COMPARED -- not inferred from arrival order. Strictly higher
            // (lower index) only: an equal index is the incumbent ITSELF (see
            // IsOwnRetarget below), and a lower rank is held off exactly as before.
            if (g_firingRule >= a_lock.owningRule) return false;
            if (!CastInFlightOnHand(a_follower, a_hand, a_lock.spell, CastProxyOnHand(fid, a_hand))) {
                g_preemptWhy[a_hand] = "the caster was idle between casts";
                return true;
            }
            if (a_urgentHeal) {
                auto* inc = RE::TESForm::LookupByID<RE::SpellItem>(a_lock.spell);
                if (inc && CasterConsent::ClassifySpell(inc) == CasterConsent::SpellKind::Offense) {
                    g_preemptWhy[a_hand] = "URGENT HEAL: the recipient is down or under the rule's threshold, "
                                           "so the heal takes the hand from a lower-ranked offense at once, "
                                           "mid-charge included (marth's ruling)";
                    return true;
                }
            }
            // Mid-charge. Stamp (or renew) this rule as the hand's waiter. A higher-
            // ranked asker replaces a lower-ranked waiter, and a wait nobody renewed
            // inside FacetExpiry() is over and starts again from now.
            const auto now   = std::chrono::steady_clock::now();
            const bool stale = a_lock.waiterRule == kNoRule ||
                               now - a_lock.waitLast >
                                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       APMFBridge::FacetExpiry());
            if (stale || g_firingRule < a_lock.waiterRule) {
                a_lock.waiterRule = g_firingRule;
                a_lock.waitSince  = now;
            }
            if (a_lock.waiterRule == g_firingRule) a_lock.waitLast = now;
            if (a_lock.waiterRule != g_firingRule) return false;   // a higher rule is already waiting
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - a_lock.waitSince);
            if (waited.count() > 0) {
                const std::int32_t apmfHand = (a_hand == kHandLeft) ? APMFBridge::kApmfHandLeft
                                                                    : APMFBridge::kApmfHandRight;
                if (ComposedCast::ObservedFiring(fid, apmfHand, a_lock.spell,
                                                 static_cast<std::uint32_t>(waited.count()))) {
                    g_preemptWhy[a_hand] = "CHARGE BOUNDARY: the incumbent fired after this rule began waiting, "
                                           "so the charge running now started while a higher-ranked rule "
                                           "waited for the hand";
                    return true;
                }
            }
            if (waited >= kInFlightHoldCap) {
                g_preemptWhy[a_hand] = "WAIT CAP: this rule has waited one full cast TTL for the hand and no "
                                       "incumbent fire was observed, longer than one charge can take";
                return true;
            }
            return false;   // the charge that was running when the wait began is protected
        }

        // ── IS THE INCUMBENT'S TARGET STILL A TARGET? (review finding, 2026-09-08) ──
        // The bound on a rule re-aiming its own cast. Without one, `IsOwnRetarget`
        // let the incumbent Release+RequestCast on EVERY lap its target changed,
        // and the claim-to-first-charge window is 2.3-4.5 s -- so a target that
        // flickers faster than that restarts APMF's window forever and the cast
        // NEVER reaches a charge. That is reachable, not theoretical:
        // Evaluator.cpp's PickAlly re-picks the STRICTLY lowest-HP party member
        // every lap, so two allies trading that slot as hits land make the same
        // heal rule resolve to a different target each lap. Before this branch the
        // rule was held off by its own lock and the first target got healed; the
        // fix must not be worse than what it replaced.
        //
        // NOT A TENURE TIMER -- a minimum-hold window would delay exactly the heal
        // marth's ruling protects. The question asked instead is whether the
        // incumbent's target is still a target AT ALL, using the SAME three tests
        // the evaluator used to pick it (Evaluator.cpp's PickAlly, :354-375, read
        // and mirrored rather than invented):
        //   * it still resolves to a live actor      (PickAlly: IsDead/IsDisabled)
        //   * it is still inside fSharedRadius        (PickAlly: same Config read)
        //   * it is still UNDER the rule's own threshold, when the firing rule's
        //     condition is the ally selector that owns that number
        //     (PickAlly: `hp < min(a_param, kHealFull)`, the identical clamp)
        // Still a target -> the re-aim is a flicker and is HELD. No longer a target
        // -> re-aiming is the only correct move and it proceeds.
        //
        // A SELF-CAST (target 0) could never reach here before animheal phase 2: its
        // lock's target never changed, so HandFree's incumbent match succeeded. The
        // AUTO heal series now moves a self heal to an ally, so a self HEAL lock is
        // judged below (lost at full health); any other self lock answers false.
        bool IncumbentTargetLost(RE::Actor* a_follower, const CastLock& a_lock) {
            if (!a_follower) return false;
            // A SELF HEAL CAN BE RE-AIMED NOW (animheal phase 2): CastAuto's series
            // moves one rule's heal from the caster to an ally, so a target-0 lock
            // does meet a different target here. Its only "lost" is the caster back
            // at full health; every other self lock keeps the answer it always had.
            if (a_lock.target == 0) {
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_lock.spell);
                return sp && SpellHealsHealth(sp) &&
                       CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Offense &&
                       Vocab::HealthPct(a_follower) >= Vocab::kHealFull;
            }
            // ONE DELIBERATE MIRROR GAP: PickAlly's "resolves" means "is in
            // Followers::g_active, or is the player", while this resolves the raw
            // FormID -- so a follower DISMISSED mid-fight still reads as a valid
            // recipient here until it heals above the threshold. Left as-is: the
            // re-aim is then held while that claim is fed, and the hold's own
            // heartbeat stops at kHealHoldNeverObservedMs for a claim that never
            // fires (see refreshHeldOwnClaim), so the cost is bounded by that
            // window rather than by the fight. Reaching g_active from this
            // predicate would bind the cast lock to the roster list for a case the
            // field has never reported.
            auto* victim = RE::TESForm::LookupByID<RE::Actor>(a_lock.target);
            if (!victim) return true;                                   // gone entirely
            if (victim->IsDead() || victim->IsDisabled()) return true;  // PickAlly's test
            if (a_follower->GetPosition().GetDistance(victim->GetPosition()) >
                Config::g_sharedRadius.load())
                return true;                                           // PickAlly's test
            if (g_firingAllyThreshold >= 0.0f) {
                // PickAlly's own boundary, clamp included: it looks for an ally
                // STRICTLY under min(param, kHealFull), so at-or-above that is no
                // longer a candidate -- which is exactly "healed back up past the
                // threshold that made it a target".
                const float ceiling = std::min(g_firingAllyThreshold, Vocab::kHealFull);
                if (Vocab::HealthPct(victim) >= ceiling) return true;
            }
            // A HEAL'S RECIPIENT IS ALSO LOST WHEN THE HEAL CAN NO LONGER REACH IT
            // (animheal phase 2; MFO-B162's fix shape). The ally picker (PickAlly) and
            // CastAuto's series both drop an ally that is beyond the spell's reach or
            // out of sight (HealInReach: distance + the Sightline verdict), so the
            // incumbent is judged by the same test -- otherwise the re-aim to the ally
            // the picker moved to is held while the claim keeps aiming at the one
            // behind the wall. A recipient back at FULL health is lost too, whatever
            // the rule's condition (an AUTO or an explicit-subject heal carries no ally
            // threshold): there is nothing left for this heal to do there. A heal
            // only: an offense lock keeps the three tests above.
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_lock.spell);
            if (sp && SpellHealsHealth(sp) &&
                CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Offense) {
                // KEEP THE INCUMBENT'S VERDICT FRESH (MFO-B178). CastOn wants only the
                // current pick and CastAuto's probe only the candidates that beat the
                // running lowest, so nobody measured the incumbent once the pick moved
                // on and its Occluded verdict aged to Unknown, which reads as "not
                // lost". Sightline::Want is the worker-safe, per-pair rate-limited
                // request (locked map, no engine call here); the verdict lands a frame
                // later and is read by the CheckWithin below on a later lap.
                Sightline::Want(a_follower->GetFormID(), { a_lock.target });
                if (Vocab::HealthPct(victim) >= Vocab::kHealFull || !HealInReach(a_follower, victim, sp) ||
                    Sightline::CheckWithin(a_follower->GetFormID(), a_lock.target, kHealLosTrustSec) ==
                        Sightline::Verdict::Occluded)
                    return true;
            }
            return false;
        }

        // ── ONE REAL HEAL AT A TIME, LOWEST FIRST (animheal phase 2, marth) ─────
        // A claimed heal whose engine cast has FIRED since this lock was taken, and
        // is not charging or channelling now, has done its one cast: the same rule
        // may re-aim at the recipient the picker names NOW (the lowest). That is the
        // per-cast re-pick marth asked for, and it is still not a flicker: the
        // recipient changes only between completed casts, never before the first
        // one lands (the 2.3-4.5 s window IncumbentTargetLost's note guards).
        // ObservedFiring only ever answers for a CLAIM (it reads the [cfc] watch),
        // so the direct road's locks are untouched. `lastSeen` is the lock's claim
        // stamp (neither the in-flight refresh nor a heal REPAIR lap re-stamps it --
        // the repair runs inside the refresh and never reaches lockHands, review
        // round 2 R2-3), so "fired within now - lastSeen" is "fired since this
        // recipient was claimed".
        bool IncumbentHealCastDone(RE::Actor* a_follower, std::size_t a_hand, const CastLock& a_lock) {
            const auto fid = a_follower ? a_follower->GetFormID() : 0;
            if (fid == 0 || a_lock.spell == 0) return false;   // target 0 (self) included
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_lock.spell);
            if (!sp || !SpellHealsHealth(sp) ||
                CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Offense)
                return false;
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - a_lock.lastSeen);
            if (since.count() <= 0) return false;
            const std::int32_t apmfHand = (a_hand == kHandLeft) ? APMFBridge::kApmfHandLeft
                                                                : APMFBridge::kApmfHandRight;
            return ComposedCast::ObservedFiring(fid, apmfHand, a_lock.spell,
                                                static_cast<std::uint32_t>(since.count()));
        }

        // ── THE SAME RULE, RE-AIMED (review finding, 2026-09-08) ────────────────
        // A rule that keeps winning with the same spell but a NEW target fails
        // HandFree's incumbent match (which compares target too), so it arrives at
        // the hand it already owns looking like a stranger. It is not a preemption
        // and must never be reported as one -- there is no rank difference to speak
        // of, and the first cut of this let a flickering cast_target outrank
        // ITSELF and Release+RequestCast its own claim on every lap.
        //
        // WHAT IT IS ALLOWED TO DO, and why that is as far as this goes. Re-aiming
        // in place is not available: APMF's ApplyRepoint writes only `param` and
        // the TTL, never castTarget/castTargetHandle/castProxy (its seats read the
        // resolved handle), and APMF_API.h states param.target is not read for
        // kIntent_Cast -- so a Repoint would renew the window while the claim went
        // on driving the OLD target, which is a silently wrong cast. Retargeting is
        // therefore still Release + RequestCast on APMF's side.
        //
        // THIS PREDICATE IS ONLY THE IDENTITY HALF. "Never mid-charge" is NOT a
        // sufficient bound on its own and an earlier comment here said it was: it
        // protects a cast from the moment charging begins, and the 2.3-4.5 s
        // between claim and first charge is precisely the window a flickering
        // target restarts. The caller pairs this with IncumbentTargetLost above,
        // which is the bound that makes the re-aim safe.
        bool IsOwnRetarget(const CastLock& a_lock, RE::FormID a_spell) {
            return a_lock.spell == a_spell && a_lock.owningRule == g_firingRule &&
                   g_firingRule != kNoRule;
        }

    }   // anon

    // COMMIT the preemption: drop the incumbent's claim on THAT HAND ONLY
    // (ReleaseOffenseCast is whole-follower and would take the other hand's
    // unrelated claim with it), then clear the lock so the caller proceeds as
    // if the hand were free. Caller must have checked CanPreemptHand first.
    void PreemptHand(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_wantedSpell) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        if (fid == 0) return;
        auto it = g_castLock.find(fid);
        if (it == g_castLock.end()) return;
        auto& lock  = it->second.hand[a_hand];
        auto& other = it->second.hand[a_hand == kHandLeft ? kHandRight : kHandLeft];
        const auto lostSpell = lock.spell;
        const auto lostRule  = lock.owningRule;
        // A MIRRORED DUAL CAST OCCUPIES BOTH HANDS AS ONE CLAIM (review
        // finding, 2026-09-08). ReleaseCastClaimOnHand tears the whole claim
        // down from either hand -- there is one APMF handle and no half-release
        // to make -- so the OTHER hand's lock must go with it, or it is left
        // naming a spell nothing is claiming any more, and HandFree's incumbent
        // short-circuit (which does not consult liveness) would keep answering
        // "yours, carry on" to the dual rule until some other rule asked for
        // that hand. The dual signature is what lockHands stamps: both hands
        // locked to the SAME (spell, target) by the SAME rule.
        const bool dualIncumbent = other.spell == lock.spell &&
                                   other.target == lock.target &&
                                   other.owningRule == lock.owningRule &&
                                   lock.spell != 0;
        APMFBridge::ReleaseCastClaimOnHand(fid, a_hand == kHandLeft ? APMFBridge::kApmfHandLeft
                                                                   : APMFBridge::kApmfHandRight);
        // The heal facet has an OWNER, and it is not this function. A heal claim
        // is bookkept by ComposedCast (its [cfc] watch, its CastBounds arm, its
        // hold record), so releasing the APMF claim behind its back leaves all
        // three armed for a claim that no longer exists. End() is the seam that
        // takes them down together. Only when the lock being preempted really
        // is the heal's: an offense claim on the LEFT hand must not drag an
        // unrelated coexisting heal down with it.
        if (a_hand == kHandLeft && lostSpell != 0 &&
            APMFBridge::GetHealCastSpell(fid) == lostSpell)
            ComposedCast::End(fid);
        lock = CastLock{};
        if (dualIncumbent) other = CastLock{};
        const auto now = std::chrono::steady_clock::now();
        auto& entry = g_lastPreemptLog[fid].hand[a_hand];
        if (entry.first == a_wantedSpell &&
            std::chrono::duration<float>(now - entry.second).count() < 2.0f)
            return;
        entry.first = a_wantedSpell; entry.second = now;
        spdlog::info("[eval] {:08X} cast gambit PREEMPTED the {} hand -- rule {} (spell {:08X}) "
                     "outranks rule {} (spell {:08X}) in the gambit list and {}, so the incumbent's "
                     "claim on that hand was released. {}",
                     fid, HandName(a_hand), g_firingRule, a_wantedSpell, lostRule, lostSpell,
                     g_preemptWhy[a_hand],
                     dualIncumbent
                         ? "The incumbent was a DUAL cast holding both hands as ONE claim, so both "
                           "hands were freed -- a dual cast cannot be half-released."
                         : "The other hand is untouched.");
    }

    // ── THE ANIMATED-HEAL CLAIM ROAD'S TWO LOCK HELPERS (animheal phase 2) ──────
    // Declared (with the contract) in Actuation_internal.h.
    //
    // ReleaseOwnHealClaim. A heal claim is released by STATE, never by a timer: the
    // recipient left reach or sight, reached full, or is no longer the AUTO pick.
    // Only THIS rule's claim on THIS spell: a heal claim on another spell belongs to
    // another rule (PreemptHand is the road for taking it), and a LEFT lock another
    // rule owns on the same spell is that rule's (two rules can share a spell).
    // kNoRule (the OOC Logistics caller) owns nothing and so releases nothing it did
    // not claim itself -- it can only meet its own claim here.
    bool ReleaseOwnHealClaim(RE::Actor* a_follower, RE::FormID a_spell, const char* a_why,
                             bool a_keepSpell) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        if (fid == 0 || a_spell == 0) return false;
        if (APMFBridge::GetHealCastSpell(fid) != a_spell) return false;
        auto it = g_castLock.find(fid);
        if (it != g_castLock.end()) {
            const auto& lk = it->second.hand[kHandLeft];
            if (lk.spell == a_spell && lk.owningRule != g_firingRule && lk.owningRule != kNoRule)
                return false;   // another rule's claim on the same spell
        }
        const RE::FormID recipient = APMFBridge::GetHealCastTarget(fid);
        ComposedCast::End(fid, a_keepSpell);
        if (it != g_castLock.end() && it->second.hand[kHandLeft].spell == a_spell)
            it->second.hand[kHandLeft] = CastLock{};
        const auto now = std::chrono::steady_clock::now();
        auto& e = g_releaseHealLog[fid];   // revert-cleared (MFO-B180/B188)
        if (e.first != a_spell || std::chrono::duration<float>(now - e.second).count() >= 2.0f) {
            e = { a_spell, now };
            spdlog::info("[heal] {:08X} heal claim RELEASED -- spell {:08X} at {:08X}: {} (rule {})",
                         fid, a_spell, recipient == 0 ? fid : recipient, a_why, g_firingRule);
        }
        return true;
    }

    // HealClaimNeedsRepair. Is the standing heal claim's spell out of the LEFT hand, or
    // in hand and never fired within the never-observed bound? Returns the repair
    // shape (HealRepair) the refresh lap acts on and LogHealRepair names; it reads
    // the lock and the engine, it touches nothing.
    HealRepair HealClaimNeedsRepair(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        if (fid == 0 || !a_spell) return {};
        const RE::FormID spellID = a_spell->GetFormID();
        if (APMFBridge::GetHealCastSpell(fid) != spellID) return {};   // not this heal's claim
        // The proxy as the refresh the caller has JUST made learned it (round 2,
        // SHADOW R2-1: read before that refresh it was still 0 on lap 2).
        const RE::FormID proxy = CastProxyOnHand(fid, kHandLeft);
        if (CastInFlightOnHand(a_follower, kHandLeft, spellID, proxy)) return {};   // casting: leave it
        // THREE SHAPES, THREE LOG LEVELS (F1 policy, marth 2026-09-30: "we're basically
        // tricking the engine into using our spells. Once equipped it's never failed").
        // The repair IS the policy: re-Prepare the heal into the left hand, stay on the
        // claim, never a direct/unanimated fallback, never a release the state does not
        // call for. What changes per shape is only how loud it is:
        //  - kCooldown (debug): out of hand while Loadout's post-fire cooldown runs. That
        //    is the NORMAL gap between two casts (StartCooldown took it back); Prepare
        //    refuses the re-equip until the cooldown ends. Not news.
        //  - kOutOfHand (info): out of hand with no cooldown (a shield restored after
        //    the heal fired, a 2H / bow given back, an equip gambit's hold returned).
        //    A real repair.
        //  - kNeverFired (WARN): the heal IS in the left hand and the engine has not
        //    fired it within the never-observed bound. The one shape that is not
        //    expected to happen, so it stays loud.
        //  - kBetweenCasts (debug, round 3 R3-2): in hand, fired earlier, not within the
        //    bound. The normal gap between casts of a claim; not news.
        enum Shape : std::uint8_t { kCooldown, kOutOfHand, kNeverFired, kBetweenCasts };
        HealRepair rep;
        rep.shape = kOutOfHand;
        // Left hand = GetEquippedObject(true), the same read Loadout::Read /
        // ReleaseSpell make. A plain member read, racy like CastInFlightOnHand's:
        // at worst one lap early or late.
        auto* held = a_follower->GetEquippedObject(true);
        bool inHand = held && (held->GetFormID() == spellID || (proxy != 0 && held->GetFormID() == proxy));
        const bool foreign = held && !inHand;   // neither the claim spell nor the learned proxy
        // PROXY STILL UNKNOWN (SHADOW R2-1's second half): APMF can mint and select its
        // delivery-flip proxy in its own Drain between the refresh and this read. A
        // spell MFO does not recognise in the left hand of a live claim whose proxy is
        // not learned yet is therefore read as the heal's, never overwritten on a
        // guess. Not a mask: if it is really the AI's own spell, it never fires as
        // this claim, and the kNeverFired WARN below says so and repairs it (round 4:
        // past the bound the guess is stale); once the proxy is learned the spell
        // reads as out of hand and is repaired at once.
        if (!inHand && proxy == 0 && held && held->As<RE::SpellItem>()) inHand = true;
        // PREPARE ONLY WHEN THE HEAL IS OUT OF HAND (review round 3, R3-1). With the
        // heal or its learned proxy in the left hand, Prepare's Read sees the proxy as
        // "a different spell" and its Caster branch EquipSpells the ORIGINAL over
        // APMF's proxy (before the cooldown gate). An in-hand shape is therefore a
        // heartbeat + log only; the spell in hand is already the one APMF drives.
        rep.prepare = !inHand;
        if (!inHand) {
            if (Loadout::CoolingDown(fid)) {
                rep.why   = "the heal is out of the left hand during the post-fire cooldown";
                rep.shape = kCooldown;
            } else {
                rep.why = "the heal is no longer in the left hand";
            }
        } else if (auto it = g_castLock.find(fid); it != g_castLock.end()) {
            const auto& lk  = it->second.hand[kHandLeft];
            const auto  age = std::chrono::steady_clock::now() - lk.lastSeen;
            if (lk.spell == spellID &&
                age >= std::chrono::milliseconds(APMFBridge::kHealHoldNeverObservedMs) &&
                !ComposedCast::ObservedFiring(fid, APMFBridge::kApmfHandLeft, spellID,
                                              APMFBridge::kHealHoldNeverObservedMs)) {
                // WARN ONLY WHEN IT HAS NEVER FIRED (review round 3, R3-2): `lastSeen`
                // is the claim stamp now (R2-3), so "no fire in the last N s" is also
                // the ordinary gap between two casts of a claim that has fired.
                // PER CLAIM (review round 4, spot 2): "fired since this claim was
                // stamped" -- ObservedFiring over now - lastSeen, IncumbentHealCastDone's
                // own form. The fight-wide latch (a_withinMs 0) let a re-claim after
                // FacetExpiry / TTL / a refusal inherit an earlier fire and hide a
                // never-firing claim behind the DEBUG shape (principle 7).
                const auto sinceClaim = std::chrono::duration_cast<std::chrono::milliseconds>(age);
                if (!ComposedCast::ObservedFiring(fid, APMFBridge::kApmfHandLeft, spellID,
                                                  static_cast<std::uint32_t>(sinceClaim.count()))) {
                    rep.why   = "equipped in the left hand but never fired within the never-observed bound";
                    rep.shape = kNeverFired;
                    // A FOREIGN spell held under the "proxy still 0" guess (review round
                    // 4, spot 1): past the bound the guess is stale, so the AI's own
                    // spell is repaired. The claim spell / learned proxy never is (R3-1).
                    if (foreign) rep.prepare = true;
                } else {
                    rep.why   = "in the left hand between casts (fired before, not in the last bound)";
                    rep.shape = kBetweenCasts;
                }
            }
        }
        return rep;
    }

    const char* HealRepairVerdict(Loadout::Ready a_ready) {
        // (An in-hand shape never calls Prepare: CastOn logs "no Prepare" instead.)
        switch (a_ready) {
        case Loadout::Ready::AlreadyReady: return "the heal is in the left hand (Prepare: already ready)";
        case Loadout::Ready::Equipped:     return "the heal was put back in the left hand (Prepare: equipped)";
        case Loadout::Ready::Debounced:    return "the heal was NOT put back (Prepare: debounced)";
        case Loadout::Ready::Failed:       return "the heal was NOT put back (Prepare: FAILED)";
        }
        return "?";
    }

    // THE REPAIR'S LOG LINE, WITH PREPARE'S REAL VERDICT (review round 2, R2-1: the
    // round-1 line said "prepared again" on laps where Prepare had answered Debounced).
    // One 5 s slot per (follower, shape), so a quiet cooldown line never uses up the
    // slot a WARN needs. A Failed verdict is at least WARN whatever the shape.
    void LogHealRepair(RE::FormID a_follower, RE::FormID a_spell, const HealRepair& a_rep,
                       const char* a_verdict, bool a_failed, const std::string& a_prepareWhy) {
        if (!a_rep) return;
        const std::uint8_t shape = a_rep.shape < 4 ? a_rep.shape : 1;
        const auto now  = std::chrono::steady_clock::now();
        auto&      last = g_healRepairLog[a_follower].at[shape];   // revert-cleared (MFO-B180/B188)
        if (std::chrono::duration<float>(now - last).count() < 5.0f) return;
        last = now;
        auto lvl = (shape == 0 || shape == 3) ? spdlog::level::debug :
                   shape == 2                 ? spdlog::level::warn :
                                                spdlog::level::info;
        if (a_failed) lvl = spdlog::level::warn;
        spdlog::log(lvl,
                    "[heal] {:08X} heal claim (spell {:08X}) REPAIR: {} -- claim refreshed; {}{}{}",
                    a_follower, a_spell, a_rep.why, a_verdict,
                    a_prepareWhy.empty() ? "" : ": ", a_prepareWhy);
    }

    // HealChannelCapped. Read the LEFT lock's spell against the engine's own caster:
    // while the follower channels it (CastInFlightOnHand, the one in-flight
    // definition) the clock runs from the first lap it was seen channelling; the
    // moment it is not channelling, the clock resets. A per-channel cap drawn once
    // from DrawConcCap's heal band, like the direct stream's (principle 9: it bounds
    // one channel, it never paces casts).
    bool HealChannelCapped(RE::Actor* a_follower, RE::FormID a_spell, float& a_capSec) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        auto it = g_castLock.find(fid);
        if (fid == 0 || it == g_castLock.end()) return false;
        auto& lk = it->second.hand[kHandLeft];
        if (lk.spell != a_spell || a_spell == 0) return false;
        if (!CastInFlightOnHand(a_follower, kHandLeft, lk.spell, CastProxyOnHand(fid, kHandLeft))) {
            lk.channelSince = {};
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (lk.channelSince.time_since_epoch().count() == 0) {
            lk.channelSince = now;
            lk.channelCap   = DrawConcCap(CasterConsent::SpellKind::Heal);
        }
        a_capSec = lk.channelCap;
        return std::chrono::duration<float>(now - lk.channelSince).count() > lk.channelCap;
    }

    namespace {

        bool HandFree(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_spell,
                     RE::FormID a_target, RE::FormID& a_busyOut, bool& a_incumbentOut) {
            const auto fid = a_follower ? a_follower->GetFormID() : 0;
            auto it = g_castLock.find(fid);
            if (it == g_castLock.end()) return true;
            auto& lock = it->second.hand[a_hand];
            if (lock.spell == 0) return true;
            if (lock.spell == a_spell && lock.target == a_target) {
                a_incumbentOut = true;
                return true;
            }
            if (!CastLockLive(a_follower, a_hand, lock)) { lock = CastLock{}; return true; }
            a_busyOut = lock.spell;
            return false;
        }

    }   // anon

    // THE GATE + THE JUGGLE. Resolves a_pick (a real Loadout::HandPick for
    // an offense spell, or plain Loadout::HandPick::Left for the self/
    // concentration/heal callers that never consult PlanCastHand at all)
    // against the CURRENT per-hand lock state, either returning the
    // resolved HandPlan (request may proceed -- caller HoldCastLock's
    // every hand HandPlan sets, on success) or a transparent HELD-OFF
    // Outcome naming the busy hand(s) (GAMBIT_FLOWS §2 -- a held gambit is
    // not a wall for the rules below it).
    std::optional<Outcome> ResolveCastHand(RE::Actor* a_follower, Loadout::HandPick a_pick,
                                           RE::FormID a_spell, RE::FormID a_target,
                                           HandPlan& a_out, bool a_urgentHeal) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        // Both hands are resolved UP FRONT now (F8/F9, 2026-09-08) instead of
        // inside each case, because the incumbent test below has to see both
        // before any pick is honoured. The only behavioural edge this adds is
        // that a Left/EitherFree request may now clear a STALE right-hand lock
        // one lap earlier than it used to -- clearing a dead lock is what
        // HandFree does either way, and nothing reads it in between.
        RE::FormID busyL = 0, busyR = 0;
        bool incL = false, incR = false;
        const bool leftFree  = HandFree(a_follower, kHandLeft,  a_spell, a_target, busyL, incL);
        const bool rightFree = HandFree(a_follower, kHandRight, a_spell, a_target, busyR, incR);

        // ── THE INCUMBENT PIN (F8's hand half + F9's signal) ────────────────
        // If this exact (spell,target) already holds a hand AND a live cast
        // claim stands there, the request is PINNED to the hand(s) that claim
        // occupies -- whatever `a_pick` says this lap. Loadout::PlanCastHand
        // re-derives its answer every lap from inputs that MOVE (the live
        // weapon grip, magicka, perks), so the same winning rule can ask for
        // DualCast on one lap and a single hand on the next; and a hand-mode
        // change is a CHANGE to APMFBridge::EnsureCastClaimLocked, which means
        // Release + RequestCast, which means the engine tears down the
        // equipment set and InterruptCasts whatever was charging. That is one
        // of the two ways the 2026-09-08 session threw away three fully
        // Charged casts of its own. The incumbent's own hand mode therefore
        // wins for as long as its claim stands; a genuinely new (spell,target)
        // is unaffected, and re-planning resumes the moment the claim ends.
        const bool liveL = incL && ClaimLiveOnHand(fid, kHandLeft);
        const bool liveR = incR && ClaimLiveOnHand(fid, kHandRight);
        if (liveL || liveR) {
            a_out = { liveL, liveR, /*inFlight=*/true };
            return std::nullopt;
        }

        // The lock behind each busy hand, for the tests below. Null when this
        // follower has no lock entry at all (then nothing is busy).
        const auto lockIt = g_castLock.find(fid);
        // TWO DIFFERENT WAYS A BUSY HAND CAN STILL BE AVAILABLE, and they must
        // not be conflated -- the first cut ran both through one predicate and
        // would have reported a rule re-aiming its OWN cast as "preempting"
        // itself, released its claim through the preemption path (which, for a
        // heal, tears down ComposedCast's whole bookkeeping) and logged
        // "rule 3 outranks rule 3".
        //
        //  * `mine`  -- this IS the incumbent, same rule and same spell, just
        //    re-aimed. Nothing is displaced: the hand is already ours and the
        //    ordinary claim path below re-points it (Release + RequestCast on
        //    APMF's side, since a cast claim cannot be retargeted in place).
        //    TWO bounds, and the second is not optional. Never mid-charge --
        //    which protects a cast from the moment charging BEGINS, and NOTHING
        //    before it: the claim-to-first-charge window is 2.3-4.5 s and the
        //    caster reads kNone throughout, so that test alone let a flickering
        //    target restart APMF's window every lap and the cast never charged
        //    at all. So also: only when the incumbent's target is genuinely
        //    LOST (IncumbentTargetLost -- the evaluator's own three tests). A
        //    target that merely lost the "lowest HP" race to another ally is
        //    not lost, and the re-aim waits.
        //  * `outranks` -- a genuinely higher-ranked rule (strictly lower
        //    index), which DOES displace the incumbent and is recorded on the
        //    plan for CastOn to commit at claim time.
        auto mine = [&](std::size_t h) {
            if (lockIt == g_castLock.end()) return false;
            const auto& lk = lockIt->second.hand[h];
            return IsOwnRetarget(lk, a_spell) &&
                   !CastInFlightOnHand(a_follower, h, lk.spell, CastProxyOnHand(fid, h)) &&
                   (IncumbentTargetLost(a_follower, lk) || IncumbentHealCastDone(a_follower, h, lk));
        };
        auto outranks = [&](std::size_t h) {
            return lockIt != g_castLock.end() &&
                   CanPreemptHand(a_follower, h, lockIt->second.hand[h], a_urgentHeal);
        };

        // ── HOLDING A RE-AIM STILL HAS TO KEEP THE CLAIM ALIVE ──────────────
        // (review finding, 2026-09-08 -- CLAUDE.md principle 9, the
        // stale-cadence class that already cost this project a dead heal and a
        // party of unarmed followers.)
        //
        // When `mine` says "your target is still valid, wait", the request
        // leaves as a transparent NoOp -- and on that lap NOTHING touches the
        // incumbent claim. F9's in-flight refresh cannot: it runs only when
        // HandFree's (spell,target) match SUCCEEDS, which is exactly the match
        // a re-aimed target fails. So `refreshed` stops moving while the hold
        // stands, and APMFBridge::Tick() sweeps the claim at FacetExpiry()
        // (~2.45 s at the defaults, floor 0.77 s) -- against a
        // claim-to-first-charge window of 2.3-4.5 s. The hold would then have
        // been protecting a claim its own silence was killing: A claimed at
        // t=0, the ally swap at t=0.5, sweep at ~2.95 s, APMF's Release tears
        // down the equipment set and interrupts the charge, and A never
        // charged. The hold only actually worked for flicker FASTER than
        // FacetExpiry(), where the incumbent's own matching laps kept it fed.
        //
        // So a held lap heartbeats the claim it is holding for, on the hand it
        // is holding -- the SAME RefreshOwnedCastOnHand the in-flight path
        // calls, for the same reason.
        //
        // IT RENEWS APMF'S TTL TOO -- AND THAT IS ONLY SAFE BECAUSE IT IS
        // BOUNDED. The first cut renewed unconditionally, on the argument that
        // the rule had won its lap and was asking for the identical claim.
        // That argument is true and answers the WRONG QUESTION (review,
        // 2026-09-08): not "is the rule asking" but "can the claim being
        // renewed ever FIRE". Nothing in IsOwnRetarget + ClaimLiveOnHand tests
        // that, and IncumbentTargetLost cannot -- it knows about death, radius
        // and the heal threshold, and NOTHING about sight. Concretely: rule R
        // claims S at T1 on the right; T1 steps behind a pillar, still alive
        // and in radius; PickFoe's soft line-of-sight preference picks the
        // sighted T2 every lap; HandFree's target match fails, `mine` is true,
        // the caster is idle because there is no LoS to charge at, and
        // IncumbentTargetLost(T1) is FALSE -- so an unconditional heartbeat
        // renews a claim that can never fire, forever, and every lower-ranked
        // rule queues behind it. The exits would be T1 dying by someone else's
        // hand, leaving the radius, R's condition going false, or combat
        // ending. "The cast fires" is not among them.
        //
        // THE CODEBASE ALREADY DECIDED THIS, one file over: RefreshHealCastClaim
        // refuses to renew AND caps a NEVER-OBSERVED claim at
        // kHealHoldNeverObservedMs (4000 ms, sized from measured
        // claim-to-observed latency), for exactly this failure -- "an incumbent
        // the engine NEVER casts can re-arm indefinitely". This path reaches the
        // same heal slot (RefreshOwnedCastOnHand's LEFT branch replays o.heal),
        // so without a cap here it would renew, indefinitely, the very claim
        // that 4 s cap bounds against a COMPETING rule.
        //
        // SO: HEARTBEAT ONLY A CLAIM THAT IS EITHER FIRING OR STILL YOUNG.
        //   * ComposedCast::ObservedFiring -- the claim has been seen to cast.
        //     A firing claim is by definition not a wedge, and a channelled or
        //     repeating cast should keep its hand for as long as it runs.
        //   * otherwise, only while the claim is younger than the SAME
        //     kHealHoldNeverObservedMs window, anchored on `lastSeen`, which
        //     HoldCastLock stamps at the claim or re-aim and nothing moves
        //     afterwards -- so it measures exactly "how long this claim has
        //     stood without being re-stated". Past it, stop feeding: the
        //     existing FacetExpiry sweep releases, and the next lap's re-aim
        //     proceeds normally.
        // THE REAL BOUND IS THE SUM, NOT THE 4 s. Feeding stops at 4000 ms;
        // the CLAIM then dies when Tick()'s sweep sees `refreshed` go stale at
        // FacetExpiry() -- so the hold actually stands ~4.8 s (0.77 s floor) to
        // ~6.5 s (~2.45 s at the defaults) before the hand frees. Stated
        // because "capped at 4 s" would be a comment the field could not
        // reconcile with the log. A cast already CHARGING is exempt either way
        // (`inFlight`), so nothing in flight is cut short by this.
        // NOT A TENURE and NOT a re-litigation of marth's ruling: this caps how
        // long a rule may wait on its OWN silent claim. It delays no
        // higher-ranked rule -- the opposite direction from the tenure that was
        // overruled, which would have delayed one.
        //
        // Only for a hand whose lock is OURS (IsOwnRetarget). A different,
        // lower-ranked rule being held off must NOT feed the incumbent's claim
        // -- the incumbent's own rule already did that earlier in this scan,
        // and doing it from here would keep a claim alive on behalf of a rule
        // that never asked.
        //
        // AND ONLY FOR A HAND WHOSE HOLD REASON WAS "THE TARGET STILL STANDS".
        // refreshHeldOwnClaim runs on every hold path, including the DualCast
        // one, which is reached when the WHOLE plan fails -- so a single-hand
        // claim of ours whose target IS lost could be renewed every lap just
        // because the OTHER hand is held by a higher-ranked incumbent. Before
        // the heartbeat existed that claim lapsed at FacetExpiry() and freed
        // its hand; feeding it would be a straight regression, so a lost-target
        // hand is fed only while its cast is actually in flight.
        auto refreshHeldOwnClaim = [&]() {
            if (lockIt == g_castLock.end()) return;
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t h = 0; h < kHandCount; ++h) {
                const auto& lk = lockIt->second.hand[h];
                if (!IsOwnRetarget(lk, a_spell) || !ClaimLiveOnHand(fid, h)) continue;
                const std::int32_t apmfHand = (h == kHandLeft) ? APMFBridge::kApmfHandLeft
                                                               : APMFBridge::kApmfHandRight;
                const bool inFlight = CastInFlightOnHand(a_follower, h, lk.spell,
                                                         CastProxyOnHand(fid, h));
                if (IncumbentTargetLost(a_follower, lk) && !inFlight) continue;   // A-3
                // RECENTLY fired, not ever. `observed` is a latch no offense
                // release path clears, so the raw answer means "this spell
                // fired once on this hand this fight" -- across claims and
                // across RULES -- and using it here would leave the cap
                // unbounded for the ordinary case: any rule whose spell had
                // landed even once could then hold its hand forever. Same
                // window as the age cap below, so both halves of "is this
                // claim still alive" are measured against one number.
                const bool fired = ComposedCast::ObservedFiring(
                    fid, apmfHand, lk.spell, APMFBridge::kHealHoldNeverObservedMs);
                const auto  age  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now - lk.lastSeen);
                if (!fired && !inFlight &&
                    age >= std::chrono::milliseconds(APMFBridge::kHealHoldNeverObservedMs))
                    continue;   // silent too long -- stop feeding it, let the sweep run
                // The SPELL, not just the hand: on the LEFT an offense claim
                // and a heal claim can stand together, and a held offense
                // re-aim must not renew a coexisting heal it knows nothing
                // about (that would slip the heal past its own never-observed
                // cap, which gates RefreshHealCastClaim's answer while the
                // claim's LIFE is the `refreshed` stamp this call bumps).
                APMFBridge::RefreshOwnedCastOnHand(fid, apmfHand, /*a_holdSpell=*/lk.spell);
                // FEED THE SILENT-CLAIM WATCH TOO. The "[cfc] ... NO observed
                // cast" warning is emitted only from WatchArmed -- there is no
                // pump -- which is why the in-flight path re-arms on every
                // refresh. Without this the ONLY line a held re-aim produces is
                // LogCastLockHold's "spell X still firing", asserting a fire
                // that may never have happened, and the one diagnostic built
                // for "a claim stands and never fires" would be blind on the
                // one path that renews such a claim (principle 7).
                ComposedCast::WatchClaim(fid, lk.spell, apmfHand, CastProxyOnHand(fid, h));
            }
        };

        switch (a_pick) {
        case Loadout::HandPick::DualCast: {
            if (leftFree && rightFree) { a_out = { true, true, false }; return std::nullopt; }
            // Dual needs BOTH hands, so BOTH must be takeable before either is
            // taken -- releasing one claim and then failing on the other would
            // spend an incumbent's cast for nothing. Checked as a pure
            // predicate first, committed only once the whole plan is possible.
            const bool okL = leftFree  || mine(kHandLeft)  || outranks(kHandLeft);
            const bool okR = rightFree || mine(kHandRight) || outranks(kHandRight);
            if (okL && okR) {
                a_out = { true, true, /*inFlight=*/false,
                          /*preemptLeft=*/ !leftFree  && !mine(kHandLeft),
                          /*preemptRight=*/!rightFree && !mine(kHandRight) };
                return std::nullopt;
            }
            refreshHeldOwnClaim();
            const auto hand      = !leftFree ? kHandLeft : kHandRight;
            const auto busySpell = !leftFree ? busyL : busyR;
            LogCastLockHold(fid, hand, a_spell, busySpell);
            return Outcome{ Result::NoOp,
                std::format("cast gambit locked -- dual-cast needs both hands, spell {:08X} "
                            "still firing ({} hand)", busySpell, HandName(hand)), true };
        }
        case Loadout::HandPick::EitherFree: {
            // ── "THERE ARE TWO HANDS, AUTO SHOULD FIGURE IT OUT" (marth,
            //    2026-09-08) ──────────────────────────────────────────────
            // An either-free plan used to take the LEFT hand whenever it was
            // free, which is where every collision came from: the heal facet is
            // LEFT-ONLY by contract (APMFBridge::ClaimHealCast's hard rule), so
            // an offense cast that idly parks on the left is standing exactly
            // where the next heal has to go, while the right hand sits empty.
            // Prefer a hand MFO has not already claimed; contend for an
            // occupied one only when both are taken. This removes most
            // collisions before preemption is ever needed.
            // RIGHT FIRST, UNCONDITIONALLY (review finding, 2026-09-08). The
            // first cut only preferred the right hand once the left was already
            // CLAIMED -- which never fires in the flow that matters, because a
            // standing heal claim makes `leftFree` false long before that test
            // is reached. The opening state is the one that has to change: two
            // free, unclaimed hands, an either-free offense plan, and a heal
            // that can use no hand but the left. So an either-free offense cast
            // takes the RIGHT hand and leaves the left for the facet with no
            // choice, instead of parking on the left and being displaced later.
            //
            // ...UNLESS A WEAPON OWNS THE RIGHT HAND, OR IS COMING BACK TO IT
            // (review finding, 2026-09-08). PlanCastHand only returns EitherFree
            // when WeaponHandActive is false, and that reads the LIVE grip and
            // the live equipment claim -- both of which are false during the
            // documented transient-unarmed facet-expiry gap, while MFO's own
            // force-hold still intends to put the weapon back ~500 ms later.
            // Defaulting to the right hand there would reproduce the 2026-09-05
            // "spell displaced by the equip gambit, cast never left rest"
            // failure for every either-free offense cast on a melee or hybrid
            // follower. WeaponHandExposure adds the one signal that survives
            // that gap; when it fires, the old LEFT-first order stands.
            const bool preferRight  = !WeaponHandExposure(a_follower);
            const auto firstHand    = preferRight ? kHandRight : kHandLeft;
            const auto secondHand   = preferRight ? kHandLeft  : kHandRight;
            const bool firstFree    = preferRight ? rightFree : leftFree;
            const bool secondFree   = preferRight ? leftFree  : rightFree;
            const bool firstClaimed  = ClaimLiveOnHand(fid, firstHand);
            const bool secondClaimed = ClaimLiveOnHand(fid, secondHand);
            auto planFor = [&](std::size_t h, bool a_preempt) {
                return h == kHandLeft ? HandPlan{ true, false, false, a_preempt, false }
                                      : HandPlan{ false, true, false, false, a_preempt };
            };
            if (firstFree  && !firstClaimed)  { a_out = planFor(firstHand,  false); return std::nullopt; }
            if (secondFree && !secondClaimed) { a_out = planFor(secondHand, false); return std::nullopt; }
            if (firstFree)  { a_out = planFor(firstHand,  false); return std::nullopt; }
            if (secondFree) { a_out = planFor(secondHand, false); return std::nullopt; }
            // Both busy: take one by RANK if this rule outranks its incumbent,
            // in the same preference order.
            if (mine(firstHand))      { a_out = planFor(firstHand,  false); return std::nullopt; }
            if (mine(secondHand))     { a_out = planFor(secondHand, false); return std::nullopt; }
            if (outranks(firstHand))  { a_out = planFor(firstHand,  true);  return std::nullopt; }
            if (outranks(secondHand)) { a_out = planFor(secondHand, true);  return std::nullopt; }
            refreshHeldOwnClaim();
            LogCastLockHold(fid, kHandLeft, a_spell, busyL);
            return Outcome{ Result::NoOp,
                std::format("cast gambit locked -- both hands busy (left: spell {:08X}, "
                            "right: spell {:08X})", busyL, busyR), true };
        }
        case Loadout::HandPick::Left:
        default:
            if (leftFree) { a_out = { true, false, false }; return std::nullopt; }
            // THE CASE marth'S RULING IS ABOUT. Every heal, every self-cast and
            // every concentration stream arrives here, LEFT-only, so this is
            // the hand a heal near the top of the list has to be able to take
            // from an offense claim below it.
            if (mine(kHandLeft)) { a_out = { true, false, false }; return std::nullopt; }
            if (outranks(kHandLeft)) {
                a_out = { true, false, false, /*preemptLeft=*/true, false };
                return std::nullopt;
            }
            refreshHeldOwnClaim();
            LogCastLockHold(fid, kHandLeft, a_spell, busyL);
            return Outcome{ Result::NoOp,
                std::format("cast gambit locked -- spell {:08X} still firing (left hand)",
                            busyL), true };
        }
    }
}
