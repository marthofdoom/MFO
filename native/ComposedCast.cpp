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

        // CLAIM (create) or refresh/re-point the declarative heal-cast facet:
        // APMF equips + drives the animated cast + guarantees delivery. hand =
        // auto (0) -- APMF picks a free hand; an intelligent per-perk hand
        // choice is a future pass. Refused (lost arbitration / APMF absent /
        // toggle off) -> degrade to kInstant, byte-identical to today.
        if (!APMFBridge::ClaimHealCast(fid, spellID, targetID, /*hand*/ 0)) {
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
