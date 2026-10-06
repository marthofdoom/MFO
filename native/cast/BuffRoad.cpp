// cast/BuffRoad.cpp -- THE ANIMATED SELF-BUFF CLAIM ROAD (feat/mfo-remaining-cast-kinds, batch A
// release gate "remaining in-combat cast kinds animated", ClickUp 86e3h87hd, checklist R6 / R8).
//
// WHAT IT IS. A cast_self of a NON-heal, NON-offense (Buff-kind) spell whose engine caster ROW
// exists natively -- Armor (Oakflesh / Stoneflesh class), Cloak, Invisibility, BoundItem (bound
// weapons) and Ward -- is a ch.8b cast claim (target 0, LEFT hand), cast by the follower's OWN AI,
// animated, exactly like the heal claim road (cast/HealRoad.cpp) but with none of the heal-only
// machinery (no recipient, no health bar). The claim is the SAME RequestCast claim every offense
// cast makes (APMFBridge::ClaimOffenseCast); the spell goes into the LEFT hand with Loadout::Prepare
// (the proven "once equipped it never failed" half) and consent is latched, as on every other claim.
//
// WHY ONLY THESE ROWS (Harbinger Docs/STATUS.md "What the engine does", read from APMF main 7fd5908).
// The engine builds a caster only for a spell whose highest-scoring effect has a row in its 23-row
// table. A SELF-delivery spell keys its own row natively (no seat 0 needed). Harbinger seats WHETHER /
// WHERE / HOW LONG (0x06 / 0x0A / 0x07 / 0x0D) on the Restore and Offensive casters ONLY; 0x0F (the
// equip gate) covers every item type. So for Armor / Cloak / Invisibility / BoundItem / Ward the claim
// admits the item and the type's OWN CheckStartCast decides when it fires. That is ENGINE TIMING, not
// a Harbinger answer, and it is UNPROVEN until the `[ctcensus]` verdicts exist (Harbinger STATUS
// "Q-L"). It is NEVER masked: a claim that does not fire stays loud through the same `[cfc]`
// silent-claim warning every claim has, and nothing re-routes it to the direct road.
//
// WHAT IS NOT HERE, and what it needs from Harbinger (stated in the report, not hacked in MFO):
//   * Light (Candlelight): excluded. MFO's own equipped Light spell left in the hand let the AI spam
//     it (55 lights, ShadowSceneNode CTD, deck 2026-08-19); the claim road leaves the spell to the AI
//     between casts, so it stays direct until a seat can bound it.
//   * Summon / Reanimate: Harbinger's summon seat task (ClickUp 86e3dvkwm: a cast seat on
//     CombatMagicCasterSummon, its self-claim match, classify skipping summons).
//   * A spell with NO row (Muffle, fortify / resist, Night Eye, Detect Life, Calm / Frenzy / Courage,
//     cures): no caster is ever built; only a seat-0 row substitution in Harbinger could serve it.
//   * A buff cast AT an ally or the player, and AUTO's beneficial fan at others: seat 0 flips the self
//     bit, so the Armor / Cloak / ... caster would be built, but 0x0A (GetMagicTarget) is not seated
//     on those types, so the engine would aim it at the combat target (a foe).
// Those keep the direct road, with one `[buff]` line each naming the reason.
//
// Worker-serial (#4), like every cast-road decision: the log dedup and the channel ledger are
// unlocked on purpose.
#include "Direct_internal.h"   // Actuation_internal.h + LogApmfRefusal
#include "ComposedCast.h"
#include "Loadout.h"
#include "Runtime.h"
#include "apmf/APMFBridge.h"

namespace MFO::Actuation {

    namespace {

        bool BuffYieldLeft(RE::Actor* a_actor, RE::TESBoundObject** a_left, RE::TESBoundObject** a_right) {
            return YieldForcedLeftHand(a_actor, "a self-buff cast claim is taking the left hand", false, a_left, a_right);
        }

        // One line per (follower, spell, road) per 15 s -- a road that answers every lap must not flood.
        std::unordered_map<std::uint64_t, std::pair<std::uint8_t, std::chrono::steady_clock::time_point>> g_roadLog;
        bool RoadLogDue(RE::FormID a_fid, RE::FormID a_spell, BuffRoad a_road) {
            const auto now = std::chrono::steady_clock::now();
            auto& e = g_roadLog[(static_cast<std::uint64_t>(a_fid) << 32) | a_spell];
            if (e.first == static_cast<std::uint8_t>(a_road) && now - e.second < std::chrono::seconds(15))
                return false;
            e = { static_cast<std::uint8_t>(a_road), now };
            return true;
        }

        // The concentration claim's per-channel bound (exact bounding covers every spell class): the
        // drawn cap, keyed to the LEFT lock's claim stamp (CastLockClaimStamp) so a re-claim, which
        // stamps a new lock, draws afresh. One entry per follower (one LEFT claim at a time).
        struct Chan { RE::FormID spell = 0; std::chrono::steady_clock::time_point stamp; float cap = 0.0f; };
        std::unordered_map<RE::FormID, Chan> g_chan;

        // True while the spell's costliest effect is on the caster (a duration buff already up).
        bool EffectActive(RE::Actor* a_actor, RE::SpellItem* a_spell) {
            auto* ei   = a_spell->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            auto* mt   = a_actor->AsMagicTarget();
            return mgef && mt && mt->HasMagicEffect(mgef);
        }

        // End this follower's buff claim on a_spell: the claim, its [cfc] watch, the LEFT lock that
        // names it, the channel ledger, and (a_takeBack) MFO's equipped spell. Idempotent.
        void EndBuffClaim(RE::Actor* a_actor, RE::SpellItem* a_spell, bool a_takeBack) {
            const auto id = a_actor->GetFormID();
            APMFBridge::ReleaseCastClaimOnHand(id, APMFBridge::kApmfHandLeft);
            ComposedCast::ClearWatchHand(id, APMFBridge::kApmfHandLeft);
            ClearLeftCastLockIf(id, a_spell->GetFormID());
            g_chan.erase(id);
            if (a_takeBack) Loadout::ReleaseSpellIf(id, a_spell->GetFormID());
        }

    }

    BuffRoad ChooseBuffRoad(RE::Actor* a_follower, RE::SpellItem* a_spell, bool a_log) {
        if (!a_follower || !a_spell) return BuffRoad::NotBuff;
        if (CasterConsent::ClassifySpell(a_spell) != CasterConsent::SpellKind::Buff) return BuffRoad::NotBuff;
        const auto fid     = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const auto name    = a_spell->GetName() ? a_spell->GetName() : "?";
        auto say = [&](BuffRoad r, const char* why) {
            if (a_log && RoadLogDue(fid, spellID, r))
                spdlog::info("[buff] {:08X} {} ({:08X}): {} -- DIRECT road, unanimated", fid, name, spellID, why);
            return r;
        };

        // The documented degrade contract: Harbinger absent / too old / claims switched off.
        if (!APMFBridge::Available() || !APMFBridge::OffenseCastClaimSupported() ||
            !Config::g_apmfCast.load() || Config::g_legacyCastHybrid.load() || !Config::g_equipToCast.load())
            return BuffRoad::DirectDegrade;

        // Only a Self-delivery spell keys its own row natively; anything else would need seat 0
        // (and 0x0A on the caster type) for a target the engine cannot be told about.
        if (a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
            return say(BuffRoad::DirectNoSeat, "not Self delivery (no 0x0A seat on the Armor / Cloak / Ward / ... "
                                               "casters: needs Harbinger)");

        const auto row = ClassifyArchetype(a_spell).row;
        switch (row) {
        case EngineRow::Armor: case EngineRow::Cloak: case EngineRow::Invisibility:
        case EngineRow::BoundItem: case EngineRow::Ward:
            break;
        case EngineRow::Light:
            return say(BuffRoad::DirectNoSeat, "Light row: a spell left in the hand let the AI spam it "
                                               "(55-light CTD 2026-08-19); needs a Harbinger seat to bound it");
        case EngineRow::Summon: case EngineRow::Reanimate:
            return say(BuffRoad::DirectNoSeat, "Summon/Reanimate row: needs Harbinger's summon seat "
                                               "(ClickUp 86e3dvkwm)");
        default:
            return say(BuffRoad::DirectNoRow, "no native caster row (the engine builds no caster for it; "
                                              "needs a Harbinger seat-0 row substitution)");
        }

        // THE TEST IS THE OBJECT THE SEATS HANG OFF (CastTargetDirect's note): no controller, no caster.
        if (!a_follower->GetActorRuntimeData().combatController)
            return say(BuffRoad::DirectNoCombat, "no combat controller (the animated cast needs the "
                                                 "follower's own combat AI)");
        return BuffRoad::Claim;
    }

    SelfCast BuffSelfClaim(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const bool conc    = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        NoteArchetypeRoad(a_follower, a_spell, ArchRoad::ConcClaim);   // [archetype] probe (passive)

        // A duration buff already up is not cast again (the direct road's already-active guard; the
        // claim must not make the AI stack it). A concentration ward is ACTIVE while it channels, so
        // the guard is for fire-and-forget only; the channel is bounded by BuffRefreshGate.
        if (!conc && EffectActive(a_follower, a_spell)) {
            EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
            return SelfCast::Declined;
        }

        std::string why;
        switch (Loadout::Prepare(a_follower, a_spell, why, BuffYieldLeft)) {
        case Loadout::Ready::AlreadyReady:
        case Loadout::Ready::Equipped:
            break;
        case Loadout::Ready::Debounced:
        case Loadout::Ready::Failed:
            spdlog::debug("[buff] {:08X} {:08X}: not claimed this lap -- Loadout::Prepare: {}", id, spellID, why);
            return SelfCast::Declined;
        }
        CasterConsent::Want(id, spellID);

        // target 0 = self (Harbinger resolves a self claim to the claimant for a Self-delivery spell,
        // APMF 85a8f2f); LEFT, the hand Prepare equipped; stopPct 0 (a heal-only concept).
        if (!APMFBridge::ClaimOffenseCast(id, spellID, /*target=*/0, APMFBridge::kApmfHandLeft, conc, /*stopPct=*/0)) {
            // Present and capable means APMF REFUSED: fail closed, loudly, never the direct road.
            LogApmfRefusal(id, "self-buff cast", spellID, /*target=*/0, "left");
            return SelfCast::Declined;
        }
        ComposedCast::WatchClaim(id, spellID, APMFBridge::kApmfHandLeft,
                                 APMFBridge::GetOffenseCastProxy(id, APMFBridge::kApmfHandLeft));
        if (RoadLogDue(id, spellID, BuffRoad::Claim))
            spdlog::info("[buff] {:08X} {} ({:08X}): self-buff CLAIM (animated; the engine's own {} caster "
                         "decides when it fires)", id, a_spell->GetName() ? a_spell->GetName() : "?", spellID,
                         EngineRowName(ClassifyArchetype(a_spell).row));
        return SelfCast::Applied;
    }

    std::optional<Outcome> BuffRefreshGate(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        if (ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false) != BuffRoad::Claim) return std::nullopt;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
            const auto stamp = CastLockClaimStamp(id, kHandLeft, spellID);
            if (stamp == std::chrono::steady_clock::time_point{}) return std::nullopt;   // no lock names it yet
            auto& ch = g_chan[id];
            if (ch.spell != spellID || ch.stamp != stamp)
                ch = { spellID, stamp, DrawConcCap(CasterConsent::SpellKind::Buff) };
            const float sec = std::chrono::duration<float>(std::chrono::steady_clock::now() - stamp).count();
            if (sec < ch.cap) return std::nullopt;
            // Past the drawn cap: release instead of renewing. The spell stays in the hand and the
            // next lap re-claims while the rule wins (a re-stream, never a stop or a cooldown).
            spdlog::info("[buff] {:08X} {:08X}: concentration self-buff reached its {:.1f} s stream cap -- claim "
                         "released, re-streams while the rule wins", id, spellID, ch.cap);
            EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/false);
            return Outcome{ Result::NoOp, "self-buff stream cap", true };
        }
        if (!EffectActive(a_follower, a_spell)) return std::nullopt;
        spdlog::info("[buff] {:08X} {:08X}: self-buff is up -- claim released (the cast landed)", id, spellID);
        EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
        return Outcome{ Result::NoOp, "self-buff already up: claim released", true };
    }

}
