// cast/Unanimated.cpp -- THE UNANIMATED ROAD LOG (combat-substrate slice S1, 2026-10-13).
//
// marth: "Getting animations for everything is non negotiable." Every in-combat follower cast is meant to be a
// ch.8b cast claim, cast by the follower's OWN AI with the real hand animation. A cast that is still delivered on
// a DIRECT road (CastSpellImmediate on the kInstant caster, which plays no animation: ENGINE_NOTES §0.8 / §0.13)
// is the residue, and principle 7 says the residue must never be masked: each such road calls
// NoteUnanimatedRoad once per cast attempt, and the field log then shows exactly which roads are still
// unanimated and why:
//
//     [cast] <id>: UNANIMATED road '<road>' for '<spell>' -- <reason>
//
// WARN, rate-limited per (follower, spell, road). With Harbinger absent the direct road is the documented
// degrade contract (APMF absent = legacy), so the line is INFO there and still says so.
//
// COMBAT TABLE ONLY. g_firingRule is a real rule index only inside Fire() (the combat table); the out-of-combat
// service reaches the same direct entries with kNoRule and is the substrate's slice (S3/S5), so it logs nothing
// here (the OOC heal keeps its own `out-of-combat heal -- INSTANT road` line, ComposedCast::NoteOocDirectHeal).
//
// Worker-serial in practice (Fire runs on the AddTask job worker), but the rate-limit table takes its own leaf
// lock: cheap, and it keeps the helper honest if a caller ever moves.
#include "Actuation_internal.h"
#include "ComposedCast.h"
#include "Runtime.h"
#include "apmf/APMFBridge.h"
#include <mutex>

namespace MFO::Actuation {

    namespace {
        constexpr auto kUnanimatedLogEvery = std::chrono::seconds(30);
        std::mutex g_unanimatedMx;
        std::map<std::tuple<RE::FormID, RE::FormID, std::string>, std::chrono::steady_clock::time_point> g_unanimatedLast;
    }

    namespace {
        // The combat-table test and the rate limit, shared by both entries. True = log this one.
        bool UnanimatedDue(RE::Actor* a_follower, const char* a_road, RE::SpellItem* a_spell) {
            if (g_firingRule == kNoRule) return false;   // not the combat table
            if (!a_follower || !a_spell) return false;
            const std::string road = a_road ? a_road : "?";
            std::scoped_lock lk(g_unanimatedMx);
            const auto now = std::chrono::steady_clock::now();
            auto& last = g_unanimatedLast[{ a_follower->GetFormID(), a_spell->GetFormID(), road }];
            if (last != std::chrono::steady_clock::time_point{} && now - last < kUnanimatedLogEvery) return false;
            last = now;
            return true;
        }

        void EmitUnanimated(RE::Actor* a_follower, const char* a_road, RE::SpellItem* a_spell,
                            std::string_view a_reason) {
            const char* name = a_spell->GetName();
            if (!name || !*name) name = "?";
            if (APMFBridge::Available())
                spdlog::warn("[cast] {:08X}: UNANIMATED road '{}' for '{}' -- {}", a_follower->GetFormID(),
                             a_road ? a_road : "?", name, a_reason);
            else
                spdlog::info("[cast] {:08X}: UNANIMATED road '{}' for '{}' -- {} (Harbinger absent: the documented "
                             "direct degrade)", a_follower->GetFormID(), a_road ? a_road : "?", name, a_reason);
        }
    }

    void NoteUnanimatedRoad(RE::Actor* a_follower, const char* a_road, RE::SpellItem* a_spell,
                            std::string_view a_reason) {
        if (UnanimatedDue(a_follower, a_road, a_spell)) EmitUnanimated(a_follower, a_road, a_spell, a_reason);
    }

    void NoteUnanimatedRoadWhy(RE::Actor* a_follower, const char* a_road, RE::SpellItem* a_spell,
                               RE::Actor* a_recipient) {
        // the reason is computed AFTER the gate: UnanimatedWhy asks the road choosers, which are combat-table business
        if (UnanimatedDue(a_follower, a_road, a_spell))
            EmitUnanimated(a_follower, a_road, a_spell, UnanimatedWhy(a_follower, a_spell, a_recipient));
    }

    std::string UnanimatedWhy(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient) {
        if (!a_follower || !a_spell) return "no caster or spell";
        const bool other = a_recipient && a_recipient != a_follower;
        switch (CasterConsent::ClassifySpell(a_spell)) {
        case CasterConsent::SpellKind::Heal:
            switch (ComposedCast::ChooseHealRoad(a_follower, a_spell, other ? a_recipient : a_follower)) {
            case ComposedCast::HealRoad::DirectNoCombat:
                return "no combat controller: the animated heal needs the follower's own combat AI";
            case ComposedCast::HealRoad::DirectDegrade:
                return "Harbinger absent or too old, or the animated heal road is switched off "
                       "(bHealAnimPackage, bEquipToCast)";
            case ComposedCast::HealRoad::DirectNoSeat:
                return "the heal has no seat in Harbinger";
            case ComposedCast::HealRoad::Claim:
                return "the heal claim road was not taken on this lap (a switch flipped between the road "
                       "choice and the cast)";
            case ComposedCast::HealRoad::NotHeal:
            default:
                break;
            }
            return "the heal road is not a claim";
        case CasterConsent::SpellKind::Buff: {
            const char* why = nullptr;
            const auto road = ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false, other ? a_recipient : nullptr, &why);
            if (road == BuffRoad::Claim)
                return Config::g_castSelf.load()
                           ? "the buff claim road was not taken on this lap"
                           : "bCastSelf is off (self casts are not claimed)";
            if (why) return why;
            return "the buff road is not a claim";
        }
        case CasterConsent::SpellKind::Offense:
        default:
            if (!APMFBridge::Available() || !APMFBridge::OffenseCastClaimSupported() || !Config::g_apmfCast.load() ||
                Config::g_legacyCastHybrid.load())
                return "Harbinger absent or too old, or the claim road is switched off (bApmfCast, bLegacyCastHybrid)";
            if (!a_follower->GetActorRuntimeData().combatController)
                return "no combat controller: the animated cast needs the follower's own combat AI";
            return "this offense shape has no claim road (self delivery, or a claim the road does not serve)";
        }
    }

}
