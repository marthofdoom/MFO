#include "PCH.h"
#include "ComposedCast.h"
#include "Config.h"
#include "APMFBridge.h"
#include "CastBounds.h"

#include <chrono>
#include <unordered_map>

#include <spdlog/spdlog.h>

// See ComposedCast.h for the shim's shape and why the old hand-drive was
// retired (twice). This TU is now just the HEAL-ONLY gate + the CastBounds
// handshake + a silent-claim diagnostic; APMF's engine seats
// (feat/ai-cast-seats-impl) own equip/drive/delivery entirely.
namespace MFO::ComposedCast {

    namespace {
        using Clock = std::chrono::steady_clock;

        // CastBounds window for a claimed heal (SPEC-FORCED-CAST.md §1.4's Heal
        // case -- offense/buff never reach this module, so the other cases are
        // moot here). Re-armed every tick Try() succeeds (Arm() is idempotent,
        // it just refreshes the expiry), so this is only the ceiling on a
        // crashed/forgotten claim, never a real cap on a continuous heal. Uses
        // APMFBridge::kHealCastTtlMs directly (rather than a second local
        // constant) so this window and the RequestCast payload's req.ttlMs
        // (built inside APMFBridge::ClaimHealCast) can never drift apart.
        constexpr std::uint32_t kHealBoundsTtlMs = APMFBridge::kHealCastTtlMs;

        // Master gate: AE-only (mirrors CastSelfDirect #67); HEAL-ONLY (offense
        // and buff stay on the byte-identical AI-fired / kInstant paths -- the
        // kIntent_Cast claim is reserved for the case the AI would never choose
        // to cast on its own); needs APMF present (the claim keeps the AI/other
        // frameworks off the hand -- without it "legacy = APMF-absent-only"
        // means the silent kInstant heal, never a half-composed one); opt-in
        // behind the (repurposed) bHealAnimPackage toggle, default OFF.
        bool Enabled(RE::Actor* a_follower, RE::SpellItem* a_spell, CasterConsent::SpellKind a_kind) {
            if (!a_follower || !a_spell)                     return false;
            if (a_kind != CasterConsent::SpellKind::Heal)    return false;
            if (!REL::Module::IsAE())                        return false;   // SE/VR -> kInstant
            if (!Config::g_healAnimPackage.load())           return false;   // opt-in, default OFF
            if (!APMFBridge::Available())                    return false;   // APMF absent -> kInstant
            return true;
        }

        // ── silent-claim diagnostic (Task 6, feat/mfo-cast-port) ────────────────
        // "DO NOT add a delivery watchdog or fallback timer" (marth) -- APMF
        // deliberately removed its own CastSpellImmediate guaranteed-delivery
        // fallback (APMF's INVARIANTS.md #0: never manufacture the effect), so if a
        // claim stands but the AI's seats never actually fire the cast, the heal
        // must visibly NOT happen -- that silence IS the correct S1 diagnostic
        // signal. This watch NEVER re-fires, re-claims, or falls back to
        // kInstant on a timeout; it only logs, so the deck log shows the
        // condition instead of a heal quietly never landing with no trace.
        //
        // Keyed on the follower, PER HAND now (feat/per-hand-cast-slots,
        // 2026-09-06: a heal claim, always LEFT, and an offense claim on
        // EITHER hand can be concurrently live on one follower, so a single
        // shared-by-follower slot would let one overwrite the other's
        // diagnostic); identity within a slot is the claimed spell (a spell
        // change is logically a new claim -- see Try() below). Worker-serial,
        // no lock (see the header's THREADING note: Try()/End() and
        // Diagnostics.cpp's ExpectingCast/NoteObservedCast call site are the
        // SAME serialized AddTask job-worker queue).
        struct Watch {
            RE::FormID        spell    = 0;
            // ABI v6 (cast-claim observability, 2026-09-06): the delivery-flip
            // proxy APMF minted for this same claim (0 on ABI < 6 or a claim
            // that minted none) -- ExpectingCast matches on EITHER `spell` OR
            // `proxy`, since a claim whose delivery APMF flipped lands as a
            // cast of the proxy, never the original spell (see ComposedCast.h).
            RE::FormID        proxy    = 0;
            Clock::time_point since{};       // when this exact (follower, spell) claim began
            bool              observed = false;   // SpellSink confirmed a real cast since `since`
            Clock::time_point lastWarn{};      // rate-limit
        };
        struct FollowerWatch { Watch hand[2]; };   // [0] = left, [1] = right
        std::unordered_map<RE::FormID, FollowerWatch> g_watch;

        // Left -> slot 0; anything else (right, or the implicit-right 0) -> slot
        // 1. Mirrors APMFBridge's own OffenseSlot mapping -- kept separate (this
        // TU has no access to that anonymous-namespace helper) but MUST agree
        // with it, since a caller names the SAME hand it just claimed via
        // ClaimOffenseCast/ClaimHealCast.
        inline std::size_t WatchSlot(std::int32_t a_hand) {
            return a_hand == APMFBridge::kApmfHandLeft ? 0 : 1;
        }

        // A claim may stand silent this long before the first warning -- long
        // enough to clear one Try() refresh lap + the engine's own charge time
        // (a native heal is not instant even AI-driven), well short of
        // kHealBoundsTtlMs's 6s ceiling so a genuinely dead claim is flagged
        // long before it would auto-expire on its own anyway.
        constexpr auto kSilentWarnAfter = std::chrono::milliseconds(2000);
        // Re-warn at most this often per still-silent claim -- a claim that
        // never fires must not spam the log every Try() tick.
        constexpr auto kSilentWarnEvery = std::chrono::milliseconds(5000);

        // Called from Try() on every successful HEAL claim (always the LEFT
        // slot), and (feat/offense-cast-seats, 2026-09-05) from the public
        // WatchClaim() below for the offense kIntent_Cast claim (Actuation::
        // CastOn, via ClaimOffenseCast, naming whichever hand(s) it was
        // granted) -- GENERIC over which claim armed it, hence the message
        // below no longer says "heal-cast" specifically. Caller passes the fid
        // already resolved. PER-HAND (feat/per-hand-cast-slots, 2026-09-06):
        // operates on ONE hand slot, so a concurrent claim on the OTHER hand
        // never resets or silences this one. a_proxy (cast-claim observability,
        // 2026-09-06): the SAME claim's minted delivery-flip proxy (0 if none/
        // ABI < 6) -- recorded alongside a_spell so ExpectingCast can match
        // either (see ComposedCast.h).
        void WatchArmed(RE::FormID a_fid, std::size_t a_slot, RE::FormID a_spell, RE::FormID a_proxy) {
            auto& w = g_watch[a_fid].hand[a_slot];
            if (w.spell != a_spell) { w = Watch{}; w.spell = a_spell; w.proxy = a_proxy; w.since = Clock::now(); return; }
            // F4 (RC4): same claim, proxy learned SINCE the watch was armed --
            // APMF mints it lazily during its own Drain, so the very first
            // WatchArmed call for a fresh claim can still see proxy==0. Learn
            // it here instead of caching that 0 forever.
            if (w.proxy == 0 && a_proxy != 0) w.proxy = a_proxy;
            if (w.observed) return;   // already confirmed firing this claim -- stay quiet
            const auto now = Clock::now();
            if (now - w.since < kSilentWarnAfter)  return;   // still within the grace window
            if (now - w.lastWarn < kSilentWarnEvery) return;  // rate-limited
            spdlog::warn("[cfc] {:08X} kIntent_Cast claim live {} ms with NO observed cast "
                         "(spell {:08X}, {} hand) -- APMF's engine seats may not be firing it; "
                         "check APMF.log for the seat state",
                         a_fid,
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - w.since).count(),
                         a_spell, a_slot == 0 ? "left" : "right");
            w.lastWarn = now;
        }
    }

    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind, std::uint32_t a_stopPct) {
        if (!Enabled(a_follower, a_spell, a_kind)) return false;   // -> caller's kInstant apply

        const RE::FormID fid       = a_follower->GetFormID();
        const RE::FormID spellID   = a_spell->GetFormID();
        const bool        selfCast = (!a_target) || (a_target == a_follower);
        const RE::FormID  targetID = selfCast ? 0 : a_target->GetFormID();
        const bool  isConcentration =
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;

        // FORWARD UNCHANGED -- MFO does not inspect delivery, does not proxy, does
        // not substitute. a_spell is always the gambit's own configured spell;
        // spellID/targetID ride to APMF exactly as given. APMF resolves delivery
        // itself: a Self-delivery spell aimed at a non-self target gets APMF's
        // OWN delivery-flip proxy (core/CastProxy.h) rather than landing on the
        // caster -- proven under the retired +ACT drive (deck APMF.log: "driving
        // left hand -- spell 0002F3B8 cast-as FF001A7D target 0009BCB0", Fast
        // Healing/Self correctly proxied onto an ally) and unchanged in shape
        // under the engine seats. This is APMF's job, not MFO's: MFO has no
        // main-thread seat inside this WORKER-context function to safely mint/
        // reconfigure a proxy form itself.

        // F1 (RC1, 2026-09-06 diagnosis): two heal rules alternating on this
        // follower's single heal-claim slot every 1-3 s (Healing Hands vs a
        // kSelf heal) each Release the live claim and RequestCast fresh before
        // the engine's ~2.5 s equip+charge ever completes -- no heal lands.
        // Mirrors the offense per-hand cast lock (Actuation.cpp's HandFree/
        // CastLockLive): if a DIFFERENT spell already holds this slot, hasn't
        // been observed firing yet, and its claim is still live and within its
        // own TTL, HOLD the incumbent instead of swapping it out -- treat this
        // Try() as Applied (the caller skips its own kInstant apply) without
        // touching the claim at all, so the incumbent gets its full charge
        // window instead of being thrashed away mid-flight.
        if (auto it = g_watch.find(fid); it != g_watch.end()) {
            const auto& incumbent = it->second.hand[0];
            if (incumbent.spell != 0 && incumbent.spell != spellID && !incumbent.observed &&
                APMFBridge::IsHealCastActive(fid) &&
                Clock::now() - incumbent.since < std::chrono::milliseconds(APMFBridge::kHealCastTtlMs)) {
                return true;   // hold the incumbent -- caller treats as Applied, no kInstant apply
            }
        }

        // CLAIM (create) or refresh the kIntent_Cast facet: while it stands,
        // APMF's engine seats drive the follower's OWN AI to select/equip/
        // charge/aim/fire/channel this spell natively. hand = LEFT
        // (APMFBridge::kApmfHandLeft), never auto -- an equip gambit's forced
        // weapon owns the RIGHT hand, so the spell must not contest it (see
        // APMFBridge.h's ClaimHealCast doc for the deck-proven failure auto
        // caused). LEFT is also the right fallback with no weapon held -- heals
        // are left-hand almost always regardless. a_stopPct forwards the
        // gambit's own configured heal threshold (0 = none -> full restoration);
        // see this file's Try() doc in ComposedCast.h. Refused (lost arbitration
        // / APMF absent / ABI too old / toggle off) -> degrade to kInstant,
        // byte-identical to today.
        if (!APMFBridge::ClaimHealCast(fid, spellID, targetID, APMFBridge::kApmfHandLeft,
                                       isConcentration, a_stopPct)) {
            CastBounds::Disarm(fid);
            // Heal is always LEFT -- clear only that slot (see End()'s own
            // comment: a concurrent offense watch on the RIGHT hand must
            // survive a refused/ended heal claim).
            if (auto it = g_watch.find(fid); it != g_watch.end()) {
                it->second.hand[0] = Watch{};
                if (it->second.hand[1].spell == 0) g_watch.erase(it);
            }
            spdlog::info("[cfc] {:08X} heal-cast claim refused -- kInstant apply", fid);
            return false;
        }

        // Register (actor, spell) so MFO's OWN CasterConsent hook -- installed
        // globally, so it also intercepts APMF's seat-answered CheckStartCast/
        // CheckCast on the SAME Restore caster vtable -- stands down for the
        // window (§2 HARD-ABORT fix). Idempotent; re-Arm just refreshes the
        // expiry. Same TTL as the claim itself (APMFBridge::kHealCastTtlMs).
        // F4 (RC4): pass the claim's minted delivery-flip proxy once known
        // (0 on ABI < 6 or a claim that minted none -- Arm already handles a
        // 0 fine) instead of the literal 0 this used to hard-code.
        CastBounds::Arm(fid, spellID, APMFBridge::GetHealCastProxy(fid), kHealBoundsTtlMs);

        // Diagnostic only -- never gates the return value or re-fires anything
        // (Task 6: no delivery watchdog). Heal is always the LEFT slot. See
        // WatchArmed's own comment. Fetch the SAME claim's minted delivery-flip
        // proxy (ABI v6; 0 on ABI < 6 or a claim that minted none) so the watch
        // recognises a cast of the proxy, not only spellID, as this claim
        // firing (cast-claim observability, 2026-09-06).
        WatchArmed(fid, WatchSlot(APMFBridge::kApmfHandLeft), spellID,
                  APMFBridge::GetHealCastProxy(fid));

        return true;   // APMF owns the cast: caller skips its kInstant apply.
    }

    void End(RE::FormID a_follower) {
        APMFBridge::ReleaseHealCast(a_follower);
        CastBounds::Disarm(a_follower);
        // Heal is always LEFT -- clear only that slot; a concurrent offense
        // claim on the RIGHT hand (feat/per-hand-cast-slots) must not lose its
        // own watch just because the heal ended.
        auto it = g_watch.find(a_follower);
        if (it != g_watch.end()) {
            it->second.hand[0] = Watch{};
            if (it->second.hand[1].spell == 0) g_watch.erase(it);
        }
    }

    // Matches on spell OR proxy per hand slot (cast-claim observability,
    // 2026-09-06) -- see ComposedCast.h's doc on this function for why: a claim
    // whose delivery APMF flipped through its own minted proxy lands as a cast
    // of the PROXY, never the original spell, so a spell-only match filed that
    // landing as "not ours" and fired a false-alarm [cfc] warning even though
    // the claim genuinely fired.
    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell) {
        const auto it = g_watch.find(a_follower);
        if (it == g_watch.end()) return false;
        const auto& h0 = it->second.hand[0];
        const auto& h1 = it->second.hand[1];
        return (h0.spell == a_spell || (h0.proxy != 0 && h0.proxy == a_spell)) ||
               (h1.spell == a_spell || (h1.proxy != 0 && h1.proxy == a_spell));
    }

    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell) {
        auto it = g_watch.find(a_follower);
        if (it == g_watch.end()) return;
        if (it->second.hand[0].spell == a_spell ||
            (it->second.hand[0].proxy != 0 && it->second.hand[0].proxy == a_spell))
            it->second.hand[0].observed = true;
        if (it->second.hand[1].spell == a_spell ||
            (it->second.hand[1].proxy != 0 && it->second.hand[1].proxy == a_spell))
            it->second.hand[1].observed = true;
    }

    void WatchClaim(RE::FormID a_follower, RE::FormID a_spell, std::int32_t a_hand, RE::FormID a_proxy) {
        WatchArmed(a_follower, WatchSlot(a_hand), a_spell, a_proxy);
    }
    // Clears BOTH hands -- callers mean "nothing is wanted on this follower at
    // all anymore" (a full teardown, e.g. dismissal or Scheduler's !castSeen).
    // A caller releasing only ONE hand's claim uses its own targeted clear
    // instead (End() above, for the heal's always-LEFT slot).
    void ClearWatch(RE::FormID a_follower) { g_watch.erase(a_follower); }

    void Reset() {
        // APMFBridge::ClearTransientState (kPreLoadGame) drops the claim;
        // CastBounds::Reset drops the bound. This shim's own state is just the
        // silent-cast diagnostic watch.
        g_watch.clear();
    }

}
