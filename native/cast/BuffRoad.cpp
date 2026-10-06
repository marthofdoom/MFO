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
// A self cast of a Light, Summon / Reanimate or rowless spell reaches ChooseBuffRoad and keeps the direct
// road with one `[buff]` line naming the reason. A buff at an ally or the player, and AUTO's beneficial
// fan, never pass through ChooseBuffRoad at all (they are NOT logged here); they keep their own roads.
//
// Worker-serial (#4), like every cast-road decision: the log dedup and the channel ledger are
// unlocked on purpose.
#include "Direct_internal.h"   // Actuation_internal.h + LogApmfRefusal
#include "ComposedCast.h"
#include "Loadout.h"
#include "MainThread.h"
#include <unordered_set>
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

        // THE "BUFF IS UP" READ IS MADE ON THE MAIN THREAD (review F6): HasMagicEffect walks the live
        // active-effect list, which the engine mutates on the main thread, so the worker never reads
        // it. MainThread::Post makes the read and LATCHES the verdict per (follower, spell) under
        // g_upMx; the worker reads the latch (the heal road's proxy-check shape, HealRoad.cpp
        // ProxyUnlearnedRelease). STALE-WHILE-REVALIDATE (review R2-3, principle 9): the worker always
        // gets the LAST verdict, or Unknown when there has never been one; a verdict older than
        // kUpRefresh only posts a fresh read and is never discarded for its age (a party whose service
        // lap exceeds any fixed window would otherwise never see a fresh verdict). At most one read is
        // pending per key. ResetBuffRoad clears it all.
        constexpr auto kUpRefresh = std::chrono::milliseconds(2000);
        struct UpLatch { bool pending = false; bool up = false; std::chrono::steady_clock::time_point at{}; };
        std::mutex                                    g_upMx;
        std::unordered_map<std::uint64_t, UpLatch>    g_up;   // under g_upMx

        enum class Up : std::uint8_t { Unknown, No, Yes };
        Up BuffUp(RE::Actor* a_actor, RE::SpellItem* a_spell) {
            const auto fid = a_actor->GetFormID();
            const auto sid = a_spell->GetFormID();
            const std::uint64_t key = (static_cast<std::uint64_t>(fid) << 32) | sid;
            Up out = Up::Unknown;
            bool post = false;
            {
                std::scoped_lock lock(g_upMx);
                auto& l = g_up[key];
                const bool seen = l.at.time_since_epoch().count() != 0;
                if (seen) out = l.up ? Up::Yes : Up::No;
                if (!l.pending && (!seen || std::chrono::steady_clock::now() - l.at >= kUpRefresh)) {
                    l.pending = true;
                    post = true;
                }
            }
            if (post)
                MainThread::Post([fid, sid, key] {
                    bool up = false;
                    auto* a  = RE::TESForm::LookupByID<RE::Actor>(fid);
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sid);
                    if (a && sp) {
                        auto* ei   = sp->GetCostliestEffectItem();
                        auto* mgef = ei ? ei->baseEffect : nullptr;
                        auto* mt   = a->AsMagicTarget();
                        up = mgef && mt && mt->HasMagicEffect(mgef);
                    }
                    std::scoped_lock lock(g_upMx);
                    auto it = g_up.find(key);
                    if (it == g_up.end()) return;   // reset while the read was queued: dropped
                    it->second = { false, up, std::chrono::steady_clock::now() };   // pending cleared
                });
            return out;
        }

        // A claim that never fired is RELEASED once and then HELD OFF (review F1): per follower, the
        // spells whose claim NEVER fired this fight (a set, review R2-4: two rules must not ping-pong).
        // A spell stays transparent until a state change: the follower loses its CombatController or
        // ClearCastLock(follower) (combat end, dismiss), or the load reverts (ResetBuffRoad). The
        // g_unsightedCharge pattern. Worker-serial (#4).
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_neverFired;

        // BUFFROAD'S OWN "FIRED THIS FIGHT" SET (review R3-1): the [cfc] watch's `observed` latch is
        // wiped by six things (EndBuffClaim's own watch clear, WatchArmed's re-arm for another spell,
        // APMFBridge::Tick's expiry sweep, the Scheduler's ClearWatch, dismissal, End/EndHand), so it
        // is not a fight-wide record. A spell is inserted whenever a fire is observed recently on a
        // refresh lap, or the up-read says the buff landed; it is the ONLY input to the
        // fired-earlier / never-fired split. Erased by ResetBuffFollower / ResetBuffRoad.
        // Worker-serial (#4).
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_fired;

        // End this follower's buff claim on a_spell: the claim, its [cfc] watch, the LEFT lock that
        // names it, and (a_takeBack) MFO's equipped spell. Idempotent.
        void EndBuffClaim(RE::Actor* a_actor, RE::SpellItem* a_spell, bool a_takeBack) {
            const auto id = a_actor->GetFormID();
            APMFBridge::ReleaseCastClaimOnHand(id, APMFBridge::kApmfHandLeft);
            ComposedCast::ClearWatchHand(id, APMFBridge::kApmfHandLeft);
            ClearLeftCastLockIf(id, a_spell->GetFormID());
            if (a_takeBack) Loadout::ReleaseSpellIf(id, a_spell->GetFormID());
        }

    }

    void ResetBuffRoad() {
        g_roadLog.clear();
        g_neverFired.clear();
        g_fired.clear();
        std::scoped_lock lock(g_upMx);
        g_up.clear();
    }

    void ResetBuffFollower(RE::FormID a_follower) {
        g_neverFired.erase(a_follower);
        g_fired.erase(a_follower);
    }

    bool BuffUpTransparent(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        if (!a_follower || !a_spell) return false;
        if (ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false) != BuffRoad::Claim) return false;
        // A never-fired hold-off (F1) is transparent without a preempt too.
        if (const auto nf = g_neverFired.find(a_follower->GetFormID());
            nf != g_neverFired.end() && nf->second.count(a_spell->GetFormID()))
            return true;
        if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) return false;
        // Unknown (the read is posted, no verdict yet) is transparent too: a guess here would preempt.
        return BuffUp(a_follower, a_spell) != Up::No;
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

        const auto arch = ClassifyArchetype(a_spell);
        // F3: a spell with ANY Light effect, a Light shape, or several effects mapping to different rows
        // (the classifier then picks the costliest, which can name Armor for a spell that also lights)
        // may be built as the engine's Light caster: the spam shape. Direct, until Harbinger bounds Light.
        bool anyLight = false;
        for (auto* eff : a_spell->effects)
            if (eff && eff->baseEffect && eff->baseEffect->data.archetype == RE::EffectArchetype::kLight)
                anyLight = true;
        if (anyLight || arch.shape == SpellShape::Light || arch.rowApprox)
            return say(BuffRoad::DirectNoSeat, "a Light effect or an ambiguous row (several effects map to "
                                               "different caster rows): the engine may build its Light caster, "
                                               "the 55-light spam CTD shape of 2026-08-19; needs a Harbinger "
                                               "bound for Light");
        switch (arch.row) {
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
        if (!a_follower->GetActorRuntimeData().combatController) {
            g_neverFired.erase(fid);   // the fight ended: a never-fired hold-off is a per-fight latch
            return say(BuffRoad::DirectNoCombat, "no combat controller (the animated cast needs the "
                                                 "follower's own combat AI)");
        }
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
        // A claim that never fired this fight stays released (review F1): transparent until the fight
        // ends or the load reverts (the latch is cleared there). Never the direct road.
        if (const auto nf = g_neverFired.find(id); nf != g_neverFired.end() && nf->second.count(spellID))
            return SelfCast::Declined;
        // The up-read is made on the main thread and latched (F6); no verdict yet is a transparent
        // no-cast this lap (the read is posted), never a guess.
        if (!conc) {
            const Up up = BuffUp(a_follower, a_spell);
            if (up == Up::Yes) {
                g_fired[id].insert(spellID);   // R3-1: it landed
                EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                return SelfCast::Declined;
            }
            if (up == Up::Unknown) return SelfCast::Declined;
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
        const bool conc    = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;

        // R3-1: a fire observed within the recency window is recorded in BuffRoad's own set.
        if (ComposedCast::ObservedFiring(id, APMFBridge::kApmfHandLeft, spellID,
                                         APMFBridge::kObservedFiringRecencyMs))
            g_fired[id].insert(spellID);

        // NEVER FIRED (review F1), the heal road's NeverFiredRelease shape (HealRoad.cpp): the claim's
        // lock stamp is older than kHoldLastSeenCapMs, no fire observed on LEFT within
        // kObservedFiringRecencyMs, and nothing charging or casting. Released ONCE with a WARN and the
        // rule held off for this fight (g_neverFired); NEVER the direct road (no decline-fallback).
        // A claim that has fired, or is charging (a held charge is in flight), is never cut here.
        if (const auto stamp = CastLockClaimStamp(id, kHandLeft, spellID);
            stamp != std::chrono::steady_clock::time_point{}) {
            const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stamp).count();
            if (ageMs >= static_cast<long long>(APMFBridge::kHoldLastSeenCapMs) &&
                !ComposedCast::ObservedFiring(id, APMFBridge::kApmfHandLeft, spellID,
                                              APMFBridge::kObservedFiringRecencyMs) &&
                !CastInFlightOnHand(a_follower, kHandLeft, spellID, CastProxyOnHand(id, kHandLeft))) {
                // "NEVER fired" is BuffRoad's own fired-this-fight set (g_fired), NOT the recency window
                // (review R2-1) and NOT the [cfc] watch latch (R3-1, wiped by six things): an
                // Invisibility broken by an attack, or a reactive ward idle between triggers, fired
                // and worked.
                if (const auto fi = g_fired.find(id); fi != g_fired.end() && fi->second.count(spellID)) {
                    spdlog::info("[buff] {:08X} {} ({:08X}): fired earlier this fight, idle for {} ms with "
                                 "nothing charging -- claim released (no hold-off; the rule re-claims when it "
                                 "wants it again)", id, a_spell->GetName() ? a_spell->GetName() : "?", spellID,
                                 ageMs);
                    EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                    return Outcome{ Result::NoOp, "self-buff claim idle after a fire: released", true };
                }
                spdlog::warn("[buff] {:08X} {} ({:08X}) NEVER FIRED this fight: claimed {} ms ago, nothing "
                             "charging -- claim RELEASED and the rule held off until the next cast-rule reset "
                             "(no direct fallback; the engine's {} caster did not fire it: see Harbinger's [ctcensus] "
                             "verdict; rule {})",
                             id, a_spell->GetName() ? a_spell->GetName() : "?", spellID, ageMs,
                             EngineRowName(ClassifyArchetype(a_spell).row), g_firingRule);
                g_neverFired[id].insert(spellID);
                EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                return Outcome{ Result::NoOp, "self-buff claim never fired: released, held off this fight", true };
            }
        }

        if (conc) {
            // THE WARD'S STREAM CAP, on the lock's own channel clock (HealChannelCapped: the clock runs
            // only while the engine channels it; a cap drawn once per channel from DrawConcCap's non-
            // offense band, 8-15 s). Release + re-stream while the rule wins, never a stop or a
            // cooldown. THE RANK STAYS (F4, MFO-B177's mechanism): the lock is kept with `restreamAt`
            // for the gap to this rule's re-claim, so a lower-ranked rule cannot take the left hand on
            // this transparent lap; HoldCastLock clears it on the re-claim.
            float capSec = 0.0f;
            if (!HealChannelCapped(a_follower, kHandLeft, spellID, capSec)) return std::nullopt;
            spdlog::info("[buff] {:08X} {:08X}: concentration self-buff reached its {:.1f} s stream cap -- claim "
                         "released, re-streams while the rule wins (the spell stays in hand, the lock keeps "
                         "its rank)", id, spellID, capSec);
            APMFBridge::ReleaseCastClaimOnHand(id, APMFBridge::kApmfHandLeft);   // the watch stays: it HAS fired
            if (auto it = g_castLock.find(id); it != g_castLock.end()) {
                auto& lk = it->second.hand[kHandLeft];
                if (lk.spell == spellID && g_firingRule != kNoRule && lk.owningRule == g_firingRule) {
                    lk.restreamAt   = std::chrono::steady_clock::now();
                    lk.channelSince = {};
                    lk.channelCap   = 0.0f;
                } else {
                    ClearLeftCastLockIf(id, spellID);
                }
            }
            return Outcome{ Result::NoOp, "self-buff stream cap", true };
        }
        if (BuffUp(a_follower, a_spell) != Up::Yes) return std::nullopt;
        g_fired[id].insert(spellID);   // R3-1: it landed
        spdlog::info("[buff] {:08X} {:08X}: self-buff is up -- claim released (the cast landed)", id, spellID);
        EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
        return Outcome{ Result::NoOp, "self-buff already up: claim released", true };
    }

}
