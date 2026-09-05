#include "PCH.h"
#include "ComposedCast.h"
#include "Config.h"
#include "APMFBridge.h"
#include "CastBounds.h"

#include <spdlog/spdlog.h>

// See ComposedCast.h for the shim's shape and why the old hand-drive was
// retired. This TU is now just the HEAL-ONLY gate + the CastBounds handshake;
// APMF (feat/cast-act) owns equip/drive/delivery entirely.
namespace MFO::ComposedCast {

    namespace {
        // CastBounds window for a claimed heal (SPEC-FORCED-CAST.md §1.4's Heal
        // case -- offense/buff never reach this module, so the other cases are
        // moot here). Re-armed every tick Try() succeeds (Arm() is idempotent,
        // it just refreshes the expiry), so this is only the ceiling on a
        // crashed/forgotten claim, never a real cap on a continuous heal.
        constexpr std::uint32_t kHealBoundsTtlMs = 6000;

        // Master gate: AE-only (mirrors CastSelfDirect #67); HEAL-ONLY (offense
        // and buff stay on the byte-identical AI-fired / kInstant paths -- the
        // declarative SelectSpell +ACT contract is reserved for the case the
        // AI would never choose to cast on its own); needs APMF present (the
        // deny keeps the AI/other frameworks off the hand -- without it "legacy
        // = APMF-absent-only" means the silent kInstant heal, never a
        // half-composed one); opt-in behind the (repurposed) bHealAnimPackage
        // toggle, default OFF.
        bool Enabled(RE::Actor* a_follower, RE::SpellItem* a_spell, CasterConsent::SpellKind a_kind) {
            if (!a_follower || !a_spell)                     return false;
            if (a_kind != CasterConsent::SpellKind::Heal)    return false;
            if (!REL::Module::IsAE())                        return false;   // SE/VR -> kInstant
            if (!Config::g_healAnimPackage.load())           return false;   // opt-in, default OFF
            if (!APMFBridge::Available())                    return false;   // APMF absent -> kInstant
            return true;
        }
    }

    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind) {
        if (!Enabled(a_follower, a_spell, a_kind)) return false;   // -> caller's kInstant apply

        const RE::FormID fid       = a_follower->GetFormID();
        const RE::FormID spellID   = a_spell->GetFormID();
        const bool        selfCast = (!a_target) || (a_target == a_follower);
        const RE::FormID  targetID = selfCast ? 0 : a_target->GetFormID();

        // FORWARD UNCHANGED -- MFO does not inspect delivery, does not proxy, does
        // not substitute. a_spell is always the gambit's own configured spell;
        // spellID/targetID ride to APMF exactly as given. APMF resolves delivery
        // itself: a Self-delivery spell aimed at a non-self target gets APMF's
        // OWN delivery-flip proxy, minted synchronously on ITS confirmed-main
        // Engage/Repoint path (core/CastExecutor.cpp's `proxy::Acquire`, called
        // from StartHandDrive) -- proven in the field (deck APMF.log: "driving
        // left hand -- spell 0002F3B8 cast-as FF001A7D target 0009BCB0", Fast
        // Healing/Self correctly proxied onto an ally). This is APMF's job, not
        // MFO's: MFO has no main-thread seat inside this WORKER-context function
        // to safely mint/reconfigure a proxy form itself (an earlier revision of
        // this function declined the mismatch case instead, reasoning MFO would
        // need to build its own proxy -- WRONG: no such proxy is MFO's to build,
        // and declining silently killed animated heal-other for every
        // Self-delivery gambit spell, the primary case this workstream exists to
        // deliver. Reverted 2026-09-05, marth).

        // CLAIM (create) or refresh/re-point the declarative heal-cast facet:
        // APMF equips + drives the animated cast + guarantees delivery. hand =
        // LEFT (APMFBridge::kApmfHandLeft), never auto -- an equip gambit's forced
        // weapon owns the RIGHT hand, so the spell must not contest it (see
        // APMFBridge.h's ClaimHealCast doc for the deck-proven failure auto caused:
        // it grabbed the right hand while briefly unarmed, then the equip gambit's
        // own re-equip shoved the spell back out ~500ms later). LEFT is also the
        // right fallback with no weapon held -- heals are left-hand almost always
        // regardless. An intelligent per-perk/loadout hand choice (dual-cast when
        // both hands are free, etc.) is a future pass, not built here. Refused
        // (lost arbitration / APMF absent / toggle off) -> degrade to kInstant,
        // byte-identical to today.
        if (!APMFBridge::ClaimHealCast(fid, spellID, targetID, APMFBridge::kApmfHandLeft)) {
            CastBounds::Disarm(fid);
            spdlog::info("[cfc] {:08X} heal-cast claim refused -- kInstant apply", fid);
            return false;
        }

        // Register (actor, spell) so MFO's OWN CasterConsent hook -- installed
        // globally, so it also intercepts APMF's driven CheckCast/
        // RequestCastImpl on the SAME hand caster -- stands down for the window
        // (§2 HARD-ABORT fix). Idempotent; re-Arm just refreshes the expiry.
        CastBounds::Arm(fid, spellID, 0, kHealBoundsTtlMs);

        return true;   // APMF owns the cast: caller skips its kInstant apply.
    }

    void End(RE::FormID a_follower) {
        APMFBridge::ReleaseHealCast(a_follower);
        CastBounds::Disarm(a_follower);
    }

    bool ExpectingCast(RE::FormID, RE::FormID) { return false; }   // drive retired; APMF owns its own observation
    void NoteObservedCast(RE::FormID, RE::FormID) {}                // no-op, kept for Diagnostics.cpp's call site

    void Reset() {
        // APMFBridge::ClearTransientState (kPreLoadGame) drops the claim;
        // CastBounds::Reset drops the bound. This shim holds no local state.
    }

}
