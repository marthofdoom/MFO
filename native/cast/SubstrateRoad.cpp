// cast/SubstrateRoad.cpp -- THE OUT-OF-COMBAT SUBSTRATE ROAD (substrate round R3 = slice S3; plan
// _evidence/2026-10-18-designs/SUBSTRATE-TO-OOC-PLAN.md "R3 detail"; rulings SUBSTRATE-RULINGS.md Q4 / Q6 / Q7).
// marth: followers cast heals, buffs and summons out of combat with the real cast animation; substrate combat is not
// combat for any client; the legacy direct cast is used only when Harbinger is absent.
//
// This file is the CAST side: which out-of-combat casts take the road (SubstrateRoadFor) and the per-cast gates in
// front of the file (FileSubstrateCast). The claim pair and its state machine are apmf/Substrate.cpp. The call
// sites stay minimal (logistics/Service.cpp, cast/Auto.cpp, cast/Summon.cpp); the logic lives here.
//
// WHICH CASTS. The road gives the CLAIM road a controller, so it serves exactly what the combat table's claim roads
// serve, minus the controller test: a Heal (ComposedCast::ChooseHealRoad's gates: bHealAnimPackage, a v5+
// Harbinger, bEquipToCast, a verified runtime) and a Buff / summon ChooseBuffRoad answers Claim or DirectNoCombat
// for (DirectNoCombat is its LAST test, so every seat / row / shape test before it passed). Not converted, by scope:
// concentration (slice S5) and hostile spells (round R4). Anything else keeps today's road and says why.
//
// NO FALLBACK (principle 7; Q4 "shouldn't happen"). Once a cast takes this road, a refusal is a logged no-cast this
// lap, never the direct road. Today's road runs only when the switch is OFF, Harbinger is absent, or Harbinger is
// present but reports the substrate unavailable (a loud once-per-session error, the declared degrade).
//
// THREADING. SubstrateRoadFor: WORKER only (ChooseBuffRoad's maps are worker-serial). FileSubstrateCast: worker or
// MAIN (the summon decides on the main thread); its two maps are guarded by g_roadMx (a leaf: never held across an
// APMFBridge call that takes g_subMx). The player / actor reads are the plain member reads the OOC table already
// makes on the worker (PlayerActivelyStealthing, HasMagicEffect, the AV reads).
#include "Actuation_internal.h"
#include "Runtime.h"            // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"    // the claim pair (apmf/Substrate.cpp), HealCastClaimSupported

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::Actuation {

    namespace {
        using RoadClock = std::chrono::steady_clock;

        std::mutex g_roadMx;
        // (follower << 32 | spell) -> the end of this spell's pacing window on this road. Guarded by g_roadMx.
        std::unordered_map<std::uint64_t, RoadClock::time_point> g_subPace;
        // (follower << 32 | spell) -> the last "stays on the direct road" line. Guarded by g_roadMx.
        std::unordered_map<std::uint64_t, RoadClock::time_point> g_directNoted;
        // follower -> the last "player sneaking" line. Guarded by g_roadMx.
        std::unordered_map<RE::FormID, RoadClock::time_point> g_sneakNoted;

        std::uint64_t Key(RE::FormID a_f, RE::FormID a_s) { return (static_cast<std::uint64_t>(a_f) << 32) | a_s; }

        // Rate-limited line naming why an out-of-combat cast keeps today's road while the switch is on. 60 s per
        // (follower, spell): the OOC table asks every ~1 s.
        void NoteDirect(RE::Actor* a_f, RE::SpellItem* a_s, const char* a_why) {
            const auto now = RoadClock::now();
            {
                std::scoped_lock lock(g_roadMx);
                auto& t = g_directNoted[Key(a_f->GetFormID(), a_s->GetFormID())];
                if (t != RoadClock::time_point{} && now - t < std::chrono::seconds(60)) return;
                t = now;
            }
            spdlog::info("[substrate] {:08X} {} ({:08X}): stays on today's direct road (unanimated) -- {}",
                         a_f->GetFormID(), a_s->GetName() ? a_s->GetName() : "?", a_s->GetFormID(), a_why);
        }

        // The pacing window, the shape the OOC table's own g_logiCastUntil uses (duration x 0.9, 300 s ceiling) with
        // a floor of fCastCooldown (at least 3 s), so a refused or never-fired open is not re-filed every lap and a
        // fired buff is not re-cast while its effect is up. A placement spell has none: SummonGate decides.
        float PaceSecs(RE::SpellItem* a_s) {
            auto* ei = a_s->GetCostliestEffectItem();
            const float flo = std::max(3.0f, Config::g_castCooldown.load());
            const float dur = ei ? static_cast<float>(ei->GetDuration()) * 0.9f : 0.0f;
            return std::clamp(dur, flo, std::max(flo, 300.0f));
        }
    }

    bool SubstrateRoadFor(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient, bool a_oocTable) {
        if (!Config::g_substrateCasts.load()) return false;   // OFF: today's road, byte for byte (nothing else read)
        if (!a_oocTable || !a_follower || !a_spell) return false;
        if (APMFBridge::SubstrateAvailability() != APMFBridge::SubstrateAvail::Available)
            return false;   // absent: silent; too old / unavailable: logged once per session inside

        const bool other = a_recipient && a_recipient != a_follower;
        const char* why = nullptr;
        if (!Runtime::CastPathsVerified()) {
            why = "the cast roads are verified on 1.6.1170 / 1.5.97 / 1.7.104 only";
        } else if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
            why = "a concentration spell (its substrate road is slice S5)";
        } else {
            switch (CasterConsent::ClassifySpell(a_spell)) {
            case CasterConsent::SpellKind::Offense:
                why = "an attack spell (out-of-combat hostile casts are round R4)";
                break;
            case CasterConsent::SpellKind::Heal:
                // ChooseHealRoad's gates without the controller test (ComposedCast.cpp): the animated heal road.
                if (!APMFBridge::HealCastClaimSupported() || !Config::g_equipToCast.load())
                    why = "the animated heal road is switched off (bHealAnimPackage / bEquipToCast)";
                else if (a_spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation)
                    why = "a target-location heal (rune-shaped: no claim serves it)";
                break;
            case CasterConsent::SpellKind::Buff: {
                // ChooseBuffRoad, log-free. DirectNoCombat is its LAST answer (every seat / row / shape test passed,
                // only the controller is missing): exactly the cast the substrate gives a controller.
                const char* bw = nullptr;
                const auto road = ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false, other ? a_recipient : nullptr, &bw);
                if (road != BuffRoad::Claim && road != BuffRoad::DirectNoCombat)
                    why = bw ? bw : "the buff claim road does not serve this spell";
                break;
            }
            }
        }
        if (why) {
            NoteDirect(a_follower, a_spell, why);
            return false;
        }
        return true;
    }

    SubstrateTry FileSubstrateCast(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient,
                                   const char* a_site) {
        if (!a_follower || !a_spell) return SubstrateTry::Skipped;
        RE::Actor* recip = a_recipient ? a_recipient : a_follower;
        const bool other = recip != a_follower;
        const auto fid     = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const char* site   = a_site ? a_site : "?";

        // 1. THE PENDING LATCH: one pair per follower. Its open is in flight (next frame, entry one frame later), it
        //    is held, or its close grace runs: the direct road must not fire beside it, and nor may a second pair.
        if (APMFBridge::SubstrateCastPending(fid)) return SubstrateTry::Skipped;

        // 2. THE PLAYER SNEAKING (marth 2026-10-19 Q6: "refuse the open while the player sneaks"). Not cast this lap.
        if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsSneaking()) {
            const auto now = RoadClock::now();
            bool say = false;
            {
                std::scoped_lock lock(g_roadMx);
                auto& t = g_sneakNoted[fid];
                if (t == RoadClock::time_point{} || now - t >= std::chrono::seconds(10)) { t = now; say = true; }
            }
            if (say)
                spdlog::info("[substrate] {:08X} OPEN refused: the player is sneaking (ruling Q6) -- {} ({:08X}) is "
                             "not cast while you sneak, never the direct road road=substrate-claim",
                             fid, a_spell->GetName() ? a_spell->GetName() : "?", spellID);
            return SubstrateTry::Skipped;
        }

        // 3. CAN HE ACT (fix/mfo-can-act; plain member loads) and COMPETENCE (§5.3, CastSelfDirect's gate: the real
        //    cost and the reserve floor). The engine's own cast spends the magicka on this road.
        if (CannotActReason(a_follower)) return SubstrateTry::Skipped;
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return SubstrateTry::Skipped;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return SubstrateTry::Skipped;
            }
        }

        // 4. NORMAL REACH FOR A HEAL on someone else (HealInReach, the immediate branch's gate; WORKER: the caller
        //    passes the caster on the main thread). Want()ed so the verdict lands next frame.
        if (other && HealsHealth(a_spell)) {
            Sightline::Want(fid, { recip->GetFormID() }, Sightline::Basis::Own);
            if (!HealInReach(a_follower, recip, a_spell)) return SubstrateTry::Skipped;
        }

        // 5. PACING and ALREADY UP (not for a placement spell: SummonGate decides a summon).
        const bool placement = BuffPlacementSpell(a_spell);
        const auto now = RoadClock::now();
        if (!placement) {
            {
                std::scoped_lock lock(g_roadMx);
                if (const auto it = g_subPace.find(Key(fid, spellID)); it != g_subPace.end() && now < it->second)
                    return SubstrateTry::Skipped;
            }
            auto* ei   = a_spell->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            if (auto* mt = recip->AsMagicTarget(); mgef && mt && ei->GetDuration() > 0 && mt->HasMagicEffect(mgef))
                return SubstrateTry::Skipped;
        }

        // 6. FILE THE PAIR. The ch.8b target convention (BuffClaim / ComposedCast::Try): 0 = self for a Self-delivery
        //    or placement spell; the caster's own FormID for a self cast of any other delivery (Harbinger's self-flip
        //    proxy, substrate S1 R2 / ExplicitSelfBuff); the recipient otherwise.
        RE::FormID target = recip->GetFormID();
        if (!other && (placement || a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf)) target = 0;
        const bool filed = APMFBridge::OpenSubstrateCast(fid, spellID, target, site);
        if (!placement) {
            std::scoped_lock lock(g_roadMx);
            if (g_subPace.size() > 64)
                for (auto it = g_subPace.begin(); it != g_subPace.end();)
                    it = now >= it->second ? g_subPace.erase(it) : std::next(it);
            g_subPace[Key(fid, spellID)] = now + std::chrono::duration_cast<RoadClock::duration>(
                                                     std::chrono::duration<float>(PaceSecs(a_spell)));
        }
        return filed ? SubstrateTry::Filed : SubstrateTry::Skipped;   // refused: logged as an ERROR, no direct road
    }

    SubstrateTry TrySubstrateCast(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient,
                                  bool a_oocTable, const char* a_site) {
        if (!SubstrateRoadFor(a_follower, a_spell, a_recipient, a_oocTable)) return SubstrateTry::NotTaken;
        return FileSubstrateCast(a_follower, a_spell, a_recipient, a_site);
    }
}
