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
// THREADING. SubstrateRoadFor: WORKER only (ChooseBuffRoad's maps are worker-serial). FileSubstrateCast: worker, or
// MAIN for a self recipient (the summon files from its main-thread gate; the reach gate, which reads the Sightline
// cache, runs only for another recipient). The maps here are guarded by g_roadMx (a leaf: never held across an
// APMFBridge call that takes g_subMx). The player / actor reads are the plain member
// reads the OOC table already makes on the worker (PlayerActivelyStealthing, HasMagicEffect, the AV reads).
#include "Actuation_internal.h"
#include "Runtime.h"            // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"    // the claim pair (apmf/Substrate.cpp), HealCastClaimSupported

#include <algorithm>
#include <format>
#include <string>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace MFO::Actuation {

    namespace {
        using RoadClock = std::chrono::steady_clock;

        std::mutex g_roadMx;
        // (follower, spell, recipient) -> this cast's pacing on this road. PER RECIPIENT, so AUTO's series reaches the
        // next ally in need while the one just served is paced. TWO windows (review F4 on 733c20cc): the SHORT floor
        // (max(fCastCooldown, 3 s)) always; the LONG one (duration x 0.9) only once Harbinger's pair for it FIRED
        // (APMFBridge::SubstrateFiredSince), so a refused / never-fired open is retried at the floor, not after the
        // buff's whole duration. Guarded by g_roadMx; cleared on revert (ResetSubstrateRoad).
        struct Pace { RoadClock::time_point filed{}; float longSecs = 0.0f; RE::FormID target = 0; };
        std::map<std::tuple<RE::FormID, RE::FormID, RE::FormID>, Pace> g_subPace;
        // (follower << 32 | spell) -> the last "stays on the direct road" line. Guarded by g_roadMx.
        std::unordered_map<std::uint64_t, RoadClock::time_point> g_directNoted;
        // follower -> the last "player sneaking" line. Guarded by g_roadMx.
        std::unordered_map<RE::FormID, RoadClock::time_point> g_sneakNoted;
        // (follower, spell, which check) -> the last "OPEN refused" reason line. Guarded by g_roadMx.
        std::map<std::tuple<RE::FormID, RE::FormID, int>, RoadClock::time_point> g_refuseNoted;

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

        // Rate-limited (10 s per follower, spell, check) reason line for a follower-wide Refused: the check that said no
        // and its values. Silent refusals hid a 21 s gap in the field (2026-10-26d). Takes the already-formatted detail.
        void NoteRefused(RE::FormID a_f, RE::SpellItem* a_s, int a_check, const std::string& a_detail) {
            const auto now = RoadClock::now();
            {
                std::scoped_lock lock(g_roadMx);
                auto& t = g_refuseNoted[{ a_f, a_s->GetFormID(), a_check }];
                if (t != RoadClock::time_point{} && now - t < std::chrono::seconds(10)) return;
                t = now;
            }
            spdlog::info("[substrate] {:08X} OPEN refused this lap: {} -- {} ({:08X}) road=substrate-claim",
                         a_f, a_detail, a_s->GetName() ? a_s->GetName() : "?", a_s->GetFormID());
        }

        // The pacing windows. SHORT: fCastCooldown, at least 3 s (a refused or never-fired open is not re-filed every
        // lap). LONG: the shape the OOC table's own g_logiCastUntil uses (duration x 0.9, 300 s ceiling), applied only
        // after a FIRE, so a fired buff is not re-cast while its effect is up. A placement spell has none: SummonGate.
        float ShortPaceSecs() { return std::max(3.0f, Config::g_castCooldown.load()); }
        float LongPaceSecs(RE::SpellItem* a_s) {
            auto* ei = a_s->GetCostliestEffectItem();
            const float flo = ShortPaceSecs();
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
                // ChooseHealRoad's self-flip bar (review SEV-4 (e) on 733c20cc): a non-Self heal at its own caster whose
                // self-flip claim fired and never landed stays direct, as on the combat table.
                else if (!other && a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf &&
                         HealSelfFlipBarred(a_follower->GetFormID(), a_spell->GetFormID()))
                    why = "its self-flip claim fired but never landed earlier (barred, as ChooseHealRoad bars it)";
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
        if (!a_follower || !a_spell) return SubstrateTry::Refused;
        RE::Actor* recip = a_recipient ? a_recipient : a_follower;
        const bool other = recip != a_follower;
        const auto fid     = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const auto rid     = recip->GetFormID();
        const char* site   = a_site ? a_site : "?";

        // ── FOLLOWER-WIDE gates (Refused: nothing for ANY recipient this lap) ──
        // 1. THE PENDING LATCH: one pair per follower. Its open is in flight (next frame, entry one frame later), it
        //    is held, or its close grace runs: the direct road must not fire beside it, and nor may a second pair.
        if (APMFBridge::SubstrateCastPending(fid)) {
            NoteRefused(fid, a_spell, 1, std::format("a substrate pair already stands for him (pending latch, site {})", site));
            return SubstrateTry::Refused;
        }

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
            return SubstrateTry::Refused;
        }

        // 2b. THE PRECONDITIONS HARBINGER'S OPEN REFUSES BY RULE (80 refusals in one 2026-10-27 session, each one applying
        //     the cast stance, minting a proxy and filing a ch.8b claim before the no). Asked here, at the source, so a
        //     request Harbinger always refuses is never filed: the player is in real combat (no substrate), he is not
        //     loaded / not high process, he is in a scene, the dialogue menu is open. Cheap plain reads only. Not cast
        //     this lap, never the direct road. What is left as an [error] now is unexpected.
        {
            const char* pre = nullptr;
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* proc = a_follower->GetActorRuntimeData().currentProcess;
            if (pc && pc->IsInCombat())                   pre = "the player is in combat (real combat: Harbinger opens no substrate)";
            else if (!a_follower->Is3DLoaded())           pre = "he is not loaded";
            else if (!proc || !proc->InHighProcess())     pre = "he is not in high process";
            else if (a_follower->GetCurrentScene())       pre = "he is in a scene";
            else if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME))
                                                          pre = "the dialogue menu is open";
            if (pre) {
                NoteRefused(fid, a_spell, 8, std::format("{} (site {})", pre, site));
                return SubstrateTry::Refused;
            }
        }

        // 3. CAN HE ACT (fix/mfo-can-act; plain member loads), DOES HE KNOW IT (INVARIANTS #20, the competence gate, asked
        //    here so no recipient is tried), and COMPETENCE (§5.3, CastSelfDirect's gate: the real cost and the
        //    reserve floor). The engine's own cast spends the magicka on this road.
        if (const char* cant = CannotActReason(a_follower)) {
            NoteRefused(fid, a_spell, 3, std::format("he cannot act ({})", cant));
            return SubstrateTry::Refused;
        }
        if (!a_follower->HasSpell(a_spell)) {
            NoteRefused(fid, a_spell, 4, "he does not know the spell (HasSpell false)");
            return SubstrateTry::Refused;
        }
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) {
                NoteRefused(fid, a_spell, 5, std::format("magicka {:.0f} is below the cost {:.0f}", have, cost));
                return SubstrateTry::Refused;
            }
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) {
                    NoteRefused(fid, a_spell, 6, std::format("magicka {:.0f} - cost {:.0f} would breach the reserve floor "
                                                              "{:.0f} ({:.2f} x max {:.0f})", have, cost, reserve * mx, reserve, mx));
                    return SubstrateTry::Refused;
                }
            }
        }

        // ── PER-RECIPIENT gates (Skipped: another recipient may still be served) ──
        // 4. NORMAL REACH FOR A HEAL on someone else (HealInReach, the immediate branch's gate). Want()ed so the
        //    verdict lands next frame.
        if (other && HealsHealth(a_spell)) {
            Sightline::Want(fid, { rid }, Sightline::Basis::Own);
            if (!HealInReach(a_follower, recip, a_spell)) return SubstrateTry::Skipped;
        }

        // The ch.8b target convention (BuffClaim / ComposedCast::Try): 0 = self for a Self-delivery or placement
        // spell; the caster's own FormID for a self cast of any other delivery (Harbinger's self-flip proxy, substrate
        // S1 R2 / ExplicitSelfBuff); the recipient otherwise.
        const bool placement = BuffPlacementSpell(a_spell);
        RE::FormID target = rid;
        if (!other && (placement || a_spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf)) target = 0;

        // 5. PACING and ALREADY UP (not for a placement spell: SummonGate decides a summon).
        const auto now = RoadClock::now();
        if (!placement) {
            Pace pace{};
            bool have = false;
            {
                std::scoped_lock lock(g_roadMx);
                if (const auto it = g_subPace.find({ fid, spellID, rid }); it != g_subPace.end()) { pace = it->second; have = true; }
            }
            if (have) {
                const float age = std::chrono::duration<float>(now - pace.filed).count();
                if (age < ShortPaceSecs()) return SubstrateTry::Skipped;
                if (age < pace.longSecs && APMFBridge::SubstrateFiredSince(fid, spellID, pace.target, pace.filed))
                    return SubstrateTry::Skipped;
            }
            auto* ei   = a_spell->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            if (auto* mt = recip->AsMagicTarget(); mgef && mt && ei->GetDuration() > 0 && mt->HasMagicEffect(mgef))
                return SubstrateTry::Skipped;
        }

        // 6. FILE THE PAIR. MFO PUTS NOTHING IN HAND (marth 2026-10-19: "Remember we are porting everything to
        //    harbinger, so having MFO do something will just need redoing later"): the equip of the claimed spell is
        //    Harbinger's (a separate APMF round lets the engine's own AI equip it while a driving claim stands). The
        //    one line below tells the field run whether the spell was already in the left hand at the open.
        const bool filed = APMFBridge::OpenSubstrateCast(fid, spellID, target, site);
        if (filed) {
            auto* left  = a_follower->GetEquippedObject(true);
            auto* right = a_follower->GetEquippedObject(false);
            spdlog::info("[substrate] {:08X} OPEN: spell in left hand = {} (left {:08X}, right {:08X}; MFO equips "
                         "nothing, Harbinger's substrate owns the equip) road=substrate-claim",
                         fid, left == a_spell ? "yes" : "no", left ? left->GetFormID() : 0u,
                         right ? right->GetFormID() : 0u);
        }
        if (!placement) {
            std::scoped_lock lock(g_roadMx);
            if (g_subPace.size() > 64)
                for (auto it = g_subPace.begin(); it != g_subPace.end();)
                    it = now - it->second.filed > std::chrono::seconds(310) ? g_subPace.erase(it) : std::next(it);
            g_subPace[{ fid, spellID, rid }] = Pace{ now, LongPaceSecs(a_spell), target };
        }
        if (!filed)
            NoteRefused(fid, a_spell, 7, std::format("Harbinger refused the open (site {}, target {:08X})", site, target));
        return filed ? SubstrateTry::Filed : SubstrateTry::Refused;   // refused: logged as an ERROR, no direct road
    }

    bool SubstrateLastWindowUnfired(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_recipient) {
        Pace pace{};
        {
            std::scoped_lock lock(g_roadMx);
            const auto it = g_subPace.find({ a_follower, a_spell, a_recipient });
            if (it == g_subPace.end()) return false;
            pace = it->second;
        }
        return !APMFBridge::SubstrateFiredSince(a_follower, a_spell, pace.target, pace.filed);
    }

    void ResetSubstrateRoad() {
        std::scoped_lock lock(g_roadMx);
        g_subPace.clear();
        g_directNoted.clear();
        g_sneakNoted.clear();
        g_refuseNoted.clear();
    }

    SubstrateTry TrySubstrateCast(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient,
                                  bool a_oocTable, const char* a_site) {
        if (!SubstrateRoadFor(a_follower, a_spell, a_recipient, a_oocTable)) return SubstrateTry::NotTaken;
        return FileSubstrateCast(a_follower, a_spell, a_recipient, a_site);
    }
}
