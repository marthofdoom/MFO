// cast/PartyGap.cpp -- THE PARTY-FIGHT GAP HOLD (anim-residue plan A1, 2026-11-24).
//
// marth: "Getting animations for everything is non negotiable." The combat table runs for a follower whenever the
// PARTY fights, own combat or not (scheduler/CombatScan.cpp, "party combat, own combat=0, combat table live"). In that
// state he has no CombatController of his own, so ChooseBuffRoad answers DirectNoCombat for every non-heal cast it
// would otherwise have claimed (a cloak, Circle of Strength, Candlelight, a summon, an AUTO buff): the claim roads need
// the controller, the substrate road is never asked off the out-of-combat table (SubstrateRoadFor), and Harbinger
// refuses a substrate open while the player fights. The only animated road for that cast starts when his controller
// appears (his own combat) or after the party fight ends (the substrate). Until then the cast is HELD, not made on a
// direct (unanimated) road.
//
// ONE READER (principle 12): OwnControllerOnlyGap answers "this cast's only missing gate is his own controller, in a
// party fight", HoldOwnControllerOnlyGap adds the rate-limited `[cast-hold]` line the callers share. Callers (all on the
// combat table, all before their direct road): CastOn's self fork and ally fork, CastSelfDirect, CastTargetDirect,
// CastSummonOnce, CastAuto's direct fan. A hold is TRANSPARENT (the caller returns a no-op the combat scan falls past,
// so a gambit that CAN act animated is never starved) and asked again every lap: nothing is latched, so the cast is
// made the moment his controller exists. This gates the source and masks nothing (principle 7): the line says so.
//
// HEALS ARE NOT HELD (marth's default, decision D1 of the plan): a heal in that gap keeps today's instant road and its
// UNANIMATED line, because a withheld heal is a worse fault than an unanimated one. ClassifySpell(Heal) answers false
// here. The heal's animated road in the gap is a Harbinger change (package H-B) and a later MFO package (A3).
//
// To add another kind of cast to the hold: widen the kind test in OwnControllerOnlyGap and nothing else.
// Worker-serial (#4); the rate-limit table takes a leaf lock anyway, like Unanimated.cpp's.
#include "Actuation_internal.h"
#include "PartyGap.h"
#include "apmf/APMFBridge.h"
#include <mutex>

namespace MFO::Actuation {

    namespace {
        constexpr auto kPartyGapLogEvery = std::chrono::seconds(15);
        std::mutex g_partyGapMx;   // leaf lock
        std::map<std::tuple<RE::FormID, RE::FormID, std::string>, std::chrono::steady_clock::time_point> g_partyGapLast;
    }

    bool OwnControllerOnlyGap(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient) {
        if (!a_follower || !a_spell) return false;
        const bool combatTable   = g_firingRule != kNoRule;   // the combat table runs only under party combat
        const bool hasController = a_follower->GetActorRuntimeData().combatController != nullptr;
        const bool buffKind      = CasterConsent::ClassifySpell(a_spell) == CasterConsent::SpellKind::Buff;   // heals keep the instant road (D1)
        if (!combatTable || hasController || !buffKind) return false;   // (the road chooser below runs only for a candidate)
        // DirectNoCombat is ChooseBuffRoad's LAST answer: every seat / row / shape / Harbinger test before it passed, so the
        // controller is the ONLY missing gate. Any other answer (a shape no claim road serves, Harbinger absent) keeps the
        // road it declares; a Claim answer cannot coincide with no controller.
        const bool other = a_recipient && a_recipient != a_follower;
        const bool roadNoCombat = ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false, other ? a_recipient : nullptr) == BuffRoad::DirectNoCombat;
        // A concentration cast has no ChooseBuffRoad claim: its claim road (DirectTarget / DirectSelf, Task 1) needs the same
        // controller plus Harbinger's offense-cast claim gates (the review's F1: buff-kind concentration at another recipient, at self
        // with non-Self delivery, and an ambiguous row at self).
        const bool conc = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        const bool concGates = conc && APMFBridge::Available() && APMFBridge::OffenseCastClaimSupported() && APMFBridge::CastSeatsSupported() &&
                               Config::g_apmfCast.load() && !Config::g_legacyCastHybrid.load();
        const bool onlyController = PartyGap::OnlyGate(roadNoCombat, conc, concGates);
        return PartyGap::Holds(combatTable, hasController, buffKind, onlyController);
    }

    bool HoldOwnControllerOnlyGap(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient, const char* a_site) {
        if (!OwnControllerOnlyGap(a_follower, a_spell, a_recipient)) return false;
        const std::string site = a_site ? a_site : "?";
        const auto now = std::chrono::steady_clock::now();
        bool due = false;
        {
            std::scoped_lock lk(g_partyGapMx);
            auto& last = g_partyGapLast[{ a_follower->GetFormID(), a_spell->GetFormID(), site }];
            if (last == std::chrono::steady_clock::time_point{} || now - last >= kPartyGapLogEvery) { last = now; due = true; }
        }
        if (due) {
            const char* name = a_spell->GetName();
            spdlog::info("[cast-hold] {:08X} {} ({:08X}) held at '{}': the party is fighting and he has no combat controller of "
                         "his own -- the animated claim needs one, so nothing is cast on the unanimated direct road; "
                         "transparent, asked again every lap (his own combat, or the substrate once the fight ends)",
                         a_follower->GetFormID(), name && *name ? name : "?", a_spell->GetFormID(), site);
        }
        return true;
    }

}
