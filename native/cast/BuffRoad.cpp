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
// WHY THESE ROWS NEED NO NEW SEAT (the original scope; Harbinger Docs/STATUS.md "What the engine does", read from APMF main 7fd5908).
// The engine builds a caster only for a spell whose highest-scoring effect has a row in its 23-row
// table. A SELF-delivery spell keys its own row natively (no seat 0 needed). Harbinger seats WHETHER /
// WHERE / HOW LONG (0x06 / 0x0A / 0x07 / 0x0D) on the Restore and Offensive casters ONLY; 0x0F (the
// equip gate) covers every item type. So for Armor / Cloak / Invisibility / BoundItem / Ward the claim
// admits the item and the type's OWN CheckStartCast decides when it fires. That is ENGINE TIMING, not
// a Harbinger answer, and it is UNPROVEN until the `[ctcensus]` verdicts exist (Harbinger STATUS
// "Q-L"). It is NEVER masked: a claim that does not fire stays loud through the same `[cfc]`
// silent-claim warning every claim has, and nothing re-routes it to the direct road.
//
// FROM feat/mfo-claim-road-summon-ally-rowless (Harbinger main ff2f884 seats 0x06 / 0x07 / 0x0A / 0x0D on
// Ward, Summon, Cloak, Light, Invisibility, BoundItem, Armor and Script) the road also serves, behind
// APMFBridge::CastSeatsSupported() (ABI >= 19: no capability bit exists, see its note):
//   * SUMMON / REANIMATE (a placement spell, target 0): Harbinger places it natively; the engine decides
//     WHETHER (a Reanimate needs a corpse it finds itself, so a claim that never fires is the
//     never-fired release's case, F1 below). "Up" is SummonGate (cast/Summon.cpp: live / landing / the
//     summon LIMIT), made on the main thread like every up-read here.
//   * ROWLESS spells (Muffle, fortify / resist, Night Eye, Detect Life, cures, Courage ...): claimed
//     normally, served by Harbinger's Script row ("served as Script" on its side). An INSTANT rowless spell
//     has no effect to read, so its claim ends once a fire is observed (one cast per claim).
//   * LIGHT: a kSelf Candlelight now rides the claim (Harbinger's 0x06 also requires "not already
//     applied", which bounds the 55-light spam of 2026-08-19). Aimed / targeted Light (Magelight) and any
//     ambiguous (rowApprox) row stay direct.
//   * A BUFF AT AN ALLY OR THE PLAYER (a_recipient): claim target = the recipient (ClaimOffenseCast), the
//     delivery-flip proxy + seat 0 key the row and 0x0A / 0x0D aim at the recipient. One claim at a time on
//     the LEFT hand; AUTO's beneficial fan (cast/Auto.cpp) is a SERIES, lowest HP first, each recipient
//     handed to CastOn. The heal road's second-recipient RIGHT hand is NOT reused (it would need
//     HealRoad surgery); a buff's recipients take the LEFT in turn.
// Below ABI 19 those kinds keep the direct road with one `[buff]` line (DirectNoCap): on an older Harbinger such
// a claim never fires and the never-fired release has no direct fallback, so claiming there would silently
// stop the cast. A self Armor / Cloak / Invisibility / BoundItem / Ward claim needs none of it (native rows).
// A concentration cast at another recipient keeps its own claim road (cast/DirectTarget.cpp).
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
        // it. MainThread::Post makes the read and LATCHES the verdict per (recipient, spell) under
        // g_upMx; the worker reads the latch (the heal road's proxy-check shape, HealRoad.cpp
        // ProxyUnlearnedRelease). STALE-WHILE-REVALIDATE (review R2-3, principle 9): the worker always
        // gets the LAST verdict, or Unknown when there has never been one; a verdict older than
        // kUpRefresh only posts a fresh read and is never discarded for its age (a party whose service
        // lap exceeds any fixed window would otherwise never see a fresh verdict). At most one read is
        // pending per key. ResetBuffRoad clears it all.
        // The RECIPIENT is the actor the effect lands on (the follower for a self buff, the ally or the
        // player for a buff at one). A SUMMON / REANIMATE spell's "up" is SummonGate on the caster
        // (live, landing, or the summon LIMIT: Blocked), not a magic effect on the recipient.
        constexpr auto kUpRefresh = std::chrono::milliseconds(2000);
        enum class Up : std::uint8_t { Unknown, No, Yes, Blocked };   // Blocked: a summon the list's limit refuses
        struct UpLatch { bool pending = false; Up up = Up::Unknown; std::chrono::steady_clock::time_point at{}; };
        std::mutex                                    g_upMx;
        std::unordered_map<std::uint64_t, UpLatch>    g_up;   // under g_upMx

        // (recipient, spell): one id for "this spell on this actor", the key of every per-spell set below.
        constexpr std::uint64_t SK(RE::FormID a_recipient, RE::FormID a_spell) {
            return (static_cast<std::uint64_t>(a_recipient) << 32) | a_spell;
        }

        Up BuffUp(RE::Actor* a_recipient, RE::SpellItem* a_spell, bool a_placement) {
            const auto rid = a_recipient->GetFormID();
            const auto sid = a_spell->GetFormID();
            const std::uint64_t key = SK(rid, sid);
            Up out = Up::Unknown;
            bool post = false;
            {
                std::scoped_lock lock(g_upMx);
                auto& l = g_up[key];
                const bool seen = l.at.time_since_epoch().count() != 0;
                if (seen) out = l.up;
                if (!l.pending && (!seen || std::chrono::steady_clock::now() - l.at >= kUpRefresh)) {
                    l.pending = true;
                    post = true;
                }
            }
            if (post)
                MainThread::Post([rid, sid, key, a_placement] {
                    Up up = Up::No;
                    auto* a  = RE::TESForm::LookupByID<RE::Actor>(rid);
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sid);
                    if (a && sp) {
                        if (a_placement) {
                            // a summon / reanimate: the CASTER's own gate (the recipient is the caster)
                            switch (SummonGate(a, sp).v) {
                            case SummonGateVerdict::Clear: up = Up::No; break;
                            case SummonGateVerdict::Limit: up = Up::Blocked; break;
                            default:                       up = Up::Yes; break;   // live or landing
                            }
                        } else {
                            auto* ei   = sp->GetCostliestEffectItem();
                            auto* mgef = ei ? ei->baseEffect : nullptr;
                            auto* mt   = a->AsMagicTarget();
                            up = (mgef && mt && mt->HasMagicEffect(mgef)) ? Up::Yes : Up::No;
                        }
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
        std::unordered_map<RE::FormID, std::unordered_set<std::uint64_t>> g_neverFired;   // follower -> SK(recipient, spell)

        // BUFFROAD'S OWN "FIRED THIS FIGHT" SET (review R3-1): the [cfc] watch's `observed` latch is
        // wiped by six things (EndBuffClaim's own watch clear, WatchArmed's re-arm for another spell,
        // APMFBridge::Tick's expiry sweep, the Scheduler's ClearWatch, dismissal, End/EndHand), so it
        // is not a fight-wide record. A spell is inserted whenever a fire is observed recently on a
        // refresh lap, or the up-read says the buff landed; it is the ONLY input to the
        // fired-earlier / never-fired split. Erased by ResetBuffFollower / ResetBuffRoad.
        // Worker-serial (#4).
        std::unordered_map<RE::FormID, std::unordered_set<std::uint64_t>> g_fired;   // follower -> SK(recipient, spell)

        // FIRED BUT NEVER LANDED (review R2-1; marth: every spell is exactly bounded). Per follower, SK(recipient,
        // spell) -> when a fire of this claim was FIRST observed. The refresh lap releases the claim (WARN, held off
        // for the fight) when the up-read has still not said Yes kHoldLastSeenCapMs after that: a claim that fires but
        // never lands (an explicit-self claim whose self-flip proxy did not land, or an older Harbinger without that proxy aiming a projectile at its own shooter) would otherwise be
        // re-cast until the magicka runs dry, holding LEFT. Erased when the claim ends. Worker-serial (#4).
        std::unordered_map<RE::FormID, std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>> g_fireSeen;

        // End this follower's buff claim on a_spell: the claim, its [cfc] watch, the LEFT lock that
        // names it, and (a_takeBack) MFO's equipped spell. Idempotent.
        void EndBuffClaim(RE::Actor* a_actor, RE::SpellItem* a_spell, bool a_takeBack) {
            const auto id = a_actor->GetFormID();
            // MFO-B230 F8: the LEFT offense slot is released unless the left lock names a DIFFERENT spell
            // (a stranger's claim holds it); no lock, or this spell's lock, releases as before.
            const auto lit = g_castLock.find(id);
            const RE::FormID leftLock = lit == g_castLock.end() ? 0 : lit->second.hand[kHandLeft].spell;
            if (leftLock == 0 || leftLock == a_spell->GetFormID()) {
                APMFBridge::ReleaseCastClaimOnHand(id, APMFBridge::kApmfHandLeft);
                ComposedCast::ClearWatchHand(id, APMFBridge::kApmfHandLeft);
            }
            ClearLeftCastLockIf(id, a_spell->GetFormID());
            if (const auto fs = g_fireSeen.find(id); fs != g_fireSeen.end())
                for (auto it = fs->second.begin(); it != fs->second.end();)
                    it = (static_cast<RE::FormID>(it->first & 0xFFFFFFFFu) == a_spell->GetFormID()) ? fs->second.erase(it) : std::next(it);
            if (a_takeBack) Loadout::ReleaseSpellIf(id, a_spell->GetFormID());
        }


        // An INSTANT spell (no effect has a duration, not a concentration stream) leaves nothing a "buff is
        // up" read can see (a cure, a dispel, an instant restore): its claim ends once a fire is observed.
        bool InstantSpell(RE::SpellItem* a_spell) {
            if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) return false;
            for (auto* eff : a_spell->effects)
                if (eff && eff->effectItem.duration > 0) return false;
            return true;
        }

        // A claim lap's label for the `[buff]` line: which kind of claim this is.
        const char* ClaimLabel(const SpellArchetype& a_arch, bool a_other) {
            switch (a_arch.row) {
            case EngineRow::Summon:    return "summon";
            case EngineRow::Reanimate: return "reanimate";
            case EngineRow::NoRow: case EngineRow::Unknown: case EngineRow::Script:
                return a_other ? "rowless ally-buff (served as Script by Harbinger)"
                               : "rowless self-buff (served as Script by Harbinger)";
            case EngineRow::Light:     return a_other ? "light at an ally" : "light";
            default:                   return a_other ? "ally-buff" : "self-buff";
            }
        }

    }

    void ResetBuffRoad() {
        g_roadLog.clear();
        g_neverFired.clear();
        g_fired.clear();
        g_fireSeen.clear();
        std::scoped_lock lock(g_upMx);
        g_up.clear();
    }

    void ResetBuffFollower(RE::FormID a_follower) {
        g_neverFired.erase(a_follower);
        g_fired.erase(a_follower);
        g_fireSeen.erase(a_follower);
    }

    bool BuffPlacementSpell(RE::SpellItem* a_spell) {
        if (!a_spell) return false;
        const auto shape = ClassifyArchetype(a_spell).shape;   // any Summon / Reanimate effect (Harbinger's IsPlacementSpell)
        return shape == SpellShape::Summon || shape == SpellShape::Reanimate;
    }

    // The static (spell-record-only) fallbacks of ChooseBuffRoad, shared with the board's spell picker
    // (cast/SpellSupport.cpp) so the two can never disagree. Declared in Actuation.h.
    // A SELF cast of a NON-kSelf, non-placement buff (marth 2026-10-05: "a self cast is valid if its needed, just
    // needs to be animated"). Harbinger leaves a TARGET-0 claim of such a spell unresolved (APMF-B39: the seats would
    // not serve it), but an EXPLICIT self FormID resolves to the claimant's handle (core/ControlMap.cpp), seat 0
    // flips the row's self bit for the driven form and 0x0A answers the claimed target = the caster. It is
    // therefore claimed like an ally buff with target = the caster's own FormID. Aimed, Touch and TargetActor
    // are all served (marth 2026-10-05: "proxy aimed buffs, and touch buffs"): Harbinger's self-flip proxy
    // (feat/apmf-self-delivery-proxy, no ABI bump) mints a kSelf copy of the spell for a claim whose target is
    // the claimant's own FormID, so no ray is aimed at the shooter. A kTargetLocation non-placement spell
    // stays direct (rune-shaped, batch D); a FLAGGED hostile / detrimental spell is never a Buff kind (CasterConsent::ClassifySpell tests the
    // effect flags only; an unflagged Calm / Frenzy / Demoralize / Paralysis archetype can still classify Buff: MFO-B233 F1).
    bool ExplicitSelfBuff(RE::SpellItem* a_spell) {
        return a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf &&
               a_spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration &&   // keeps the Task-1 target-0 claim
               !BuffPlacementSpell(a_spell);
    }

    const char* BuffAmbiguousRowWhy(const SpellArchetype& a_arch) {
        if (!a_arch.rowApprox) return nullptr;
        return "an ambiguous row (several effects map to different caster rows: "
               "the engine may build a different caster than the one judged)";
    }

    const char* BuffAimedLightWhy(RE::SpellItem* a_spell, const SpellArchetype& a_arch) {
        bool anyLight = false;
        for (auto* eff : a_spell->effects)
            if (eff && eff->baseEffect && eff->baseEffect->data.archetype == RE::EffectArchetype::kLight)
                anyLight = true;
        // Harbinger serves Light for kSelf delivery only, and refuses aimed Magelight.
        if ((anyLight || a_arch.shape == SpellShape::Light || a_arch.row == EngineRow::Light) &&
            a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
            return "aimed / targeted Light (Magelight): Harbinger serves Light for Self delivery only";
        return nullptr;
    }

    const char* BuffSelfRuneWhy(RE::SpellItem* a_spell, bool a_explicitSelf) {
        if (a_explicitSelf && a_spell->GetDelivery() == RE::MagicSystem::Delivery::kTargetLocation)
            return "self cast of a target-location buff: rune-shaped, Harbinger's self-flip proxy does not serve it (batch D)";
        return nullptr;
    }

    bool BuffUpTransparent(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient) {
        if (!a_follower || !a_spell) return false;
        const bool other = a_recipient && a_recipient != a_follower;
        RE::Actor* recip = other ? a_recipient : a_follower;
        if (ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false, other ? recip : nullptr) != BuffRoad::Claim) return false;
        // A never-fired hold-off (F1) is transparent without a preempt too.
        if (const auto nf = g_neverFired.find(a_follower->GetFormID());
            nf != g_neverFired.end() && nf->second.count(SK(recip->GetFormID(), a_spell->GetFormID())))
            return true;
        if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) return false;
        // nothing an up-read could see -- except a placement spell, whose "up" is SummonGate whatever its duration
        if (InstantSpell(a_spell) && !BuffPlacementSpell(a_spell)) return false;
        // Unknown (the read is posted, no verdict yet) is transparent too: a guess here would preempt.
        return BuffUp(recip, a_spell, BuffPlacementSpell(a_spell)) != Up::No;
    }

    BuffRoad ChooseBuffRoad(RE::Actor* a_follower, RE::SpellItem* a_spell, bool a_log, RE::Actor* a_recipient) {
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

        const bool other = a_recipient && a_recipient != a_follower;
        // A concentration cast at another recipient keeps its own claim road (cast/DirectTarget.cpp); quiet.
        if (other && a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
            return BuffRoad::DirectNoSeat;

        // A buff at another recipient is keyed with seat 0's self flip (a_seat0: the claim's driven form),
        // as the heal claim road is; a self buff keys its own row natively.
        const bool explicitSelf = !other && ExplicitSelfBuff(a_spell);   // keyed like an ally claim (seat 0 flip)
        const auto arch = ClassifyArchetype(a_spell, /*a_seat0=*/other || explicitSelf);
        const bool placement = arch.shape == SpellShape::Summon || arch.shape == SpellShape::Reanimate;
        if (other && placement)   // CastOn hands a placement spell the follower as its recipient; defensive
            return say(BuffRoad::DirectNoSeat, "a summon / reanimate names no recipient (target 0)");

        // A spell with several effects mapping to different rows (the classifier then picks the costliest,
        // which can name Armor for a spell that also lights) may be built as another caster than the one
        // the claim was judged for: the 55-light spam shape. Direct.
        if (const char* why = BuffAmbiguousRowWhy(arch))
            return say(BuffRoad::DirectNoSeat, why);
        const auto delivery = a_spell->GetDelivery();
        if (!placement) {
            if (const char* why = BuffAimedLightWhy(a_spell, arch))
                return say(BuffRoad::DirectNoSeat, why);
            // A self cast of a non-Self buff is claimed with the caster's own FormID as the target (see
            // ExplicitSelfBuff); Aimed / Touch / TargetActor drive Harbinger's self-flip proxy. Only a
            // kTargetLocation (non-placement, rune-shaped) spell stays direct.
            if (!other && !explicitSelf && delivery != RE::MagicSystem::Delivery::kSelf)
                return say(BuffRoad::DirectNoSeat, "not Self delivery (a concentration self cast of a non-Self spell "
                                                   "keeps the target-0 claim of CastSelfDirect)");
            if (const char* why = BuffSelfRuneWhy(a_spell, explicitSelf))
                return say(BuffRoad::DirectNoSeat, why);
        }
        // needCap: the kinds that exist only with Harbinger's buff / summon / rowless seats. The native-row
        // self buffs (Armor / Cloak / Invisibility / BoundItem / Ward at self) need none.
        bool needCap = other || explicitSelf;
        switch (arch.row) {
        case EngineRow::Armor: case EngineRow::Cloak: case EngineRow::Invisibility:
        case EngineRow::BoundItem: case EngineRow::Ward:
            break;
        case EngineRow::Light: case EngineRow::Summon: case EngineRow::Reanimate:
        case EngineRow::NoRow: case EngineRow::Unknown: case EngineRow::Script:
            needCap = true;   // Light / placement / rowless (Harbinger serves a rowless spell as Script)
            break;
        default:
            return say(BuffRoad::DirectNoRow, "its row belongs to another road (Offensive / Restore / Stagger / "
                                              "Disarm / TargetEffect / Paralyze)");
        }
        if (needCap && !APMFBridge::CastSeatsSupported())
            return say(BuffRoad::DirectNoCap, "Harbinger lacks the buff / summon / rowless seats (ABI < 19): this "
                                              "kind stays direct until Harbinger is updated (an older Harbinger "
                                              "would never fire the claim)");

        // THE TEST IS THE OBJECT THE SEATS HANG OFF (CastTargetDirect's note): no controller, no caster.
        if (!a_follower->GetActorRuntimeData().combatController) {
            g_neverFired.erase(fid);   // the fight ended: a never-fired hold-off is a per-fight latch
            return say(BuffRoad::DirectNoCombat, "no combat controller (the animated cast needs the "
                                                 "follower's own combat AI)");
        }
        return BuffRoad::Claim;
    }

    SelfCast BuffClaim(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient) {
        const bool other = a_recipient && a_recipient != a_follower;
        RE::Actor* recip = other ? a_recipient : a_follower;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const auto rid     = recip->GetFormID();
        const auto sk      = SK(rid, spellID);
        const bool conc    = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        const bool placement = BuffPlacementSpell(a_spell);
        const bool instant   = !placement && InstantSpell(a_spell);   // a zero-duration summon still gets SummonGate
        NoteArchetypeRoad(a_follower, a_spell, ArchRoad::ConcClaim);   // [archetype] probe (passive)

        // A duration buff already up is not cast again (the direct road's already-active guard; the
        // claim must not make the AI stack it). A concentration ward is ACTIVE while it channels, so
        // the guard is for fire-and-forget only; the channel is bounded by BuffRefreshGate.
        // A claim that never fired this fight stays released (review F1): transparent until the fight
        // ends or the load reverts (the latch is cleared there). Never the direct road.
        if (const auto nf = g_neverFired.find(id); nf != g_neverFired.end() && nf->second.count(sk))
            return SelfCast::Declined;
        // The up-read is made on the main thread and latched (F6); no verdict yet is a transparent
        // no-cast this lap (the read is posted), never a guess. An INSTANT spell has nothing to read.
        if (!conc && !instant) {
            const Up up = BuffUp(recip, a_spell, placement);
            if (up == Up::Yes) {
                g_fired[id].insert(sk);   // R3-1: it landed
                EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                return SelfCast::Declined;
            }
            if (up == Up::Blocked) {   // a summon the commanded-actor list refuses: not cast, not "fired"
                EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                spdlog::debug("[buff] {:08X} {:08X}: summon limit reached -- not claimed", id, spellID);
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
        // APMF 85a8f2f, and a placement spell's target 0 to the claimant too); a buff at another recipient, or a
        // non-kSelf buff at the caster (explicitSelf), names that actor. LEFT, the hand Prepare equipped; stopPct 0 (a heal-only concept).
        const bool explicitSelf = !other && ExplicitSelfBuff(a_spell);
        const RE::FormID target = (other || explicitSelf) ? rid : 0;
        if (!APMFBridge::ClaimOffenseCast(id, spellID, target, APMFBridge::kApmfHandLeft, conc, /*stopPct=*/0)) {
            // Present and capable means APMF REFUSED: fail closed, loudly, never the direct road.
            LogApmfRefusal(id, "buff cast", spellID, target, "left");
            return SelfCast::Declined;
        }
        ComposedCast::WatchClaim(id, spellID, APMFBridge::kApmfHandLeft,
                                 APMFBridge::GetOffenseCastProxy(id, APMFBridge::kApmfHandLeft));
        if (RoadLogDue(id, spellID, BuffRoad::Claim)) {
            const auto arch = ClassifyArchetype(a_spell, /*a_seat0=*/other || explicitSelf);
            spdlog::info("[buff] {:08X} {} ({:08X}): {} CLAIM at {:08X} (animated; the engine's own {} caster "
                         "decides when it fires)", id, a_spell->GetName() ? a_spell->GetName() : "?", spellID,
                         ClaimLabel(arch, other), rid, EngineRowName(arch.row));
        }
        return SelfCast::Applied;
    }

    SelfCast BuffSelfClaim(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        return BuffClaim(a_follower, a_spell, nullptr);
    }

    std::optional<Outcome> BuffRefreshGate(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_recipient) {
        const bool other = a_recipient && a_recipient != a_follower;
        RE::Actor* recip = other ? a_recipient : a_follower;
        if (ChooseBuffRoad(a_follower, a_spell, /*a_log=*/false, other ? recip : nullptr) != BuffRoad::Claim)
            return std::nullopt;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const auto rid     = recip->GetFormID();
        const auto sk      = SK(rid, spellID);
        const bool conc    = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        const bool placement = BuffPlacementSpell(a_spell);
        const bool instant   = !placement && InstantSpell(a_spell);   // a zero-duration summon still gets SummonGate

        // R3-1: a fire observed within the recency window is recorded in BuffRoad's own set.
        if (ComposedCast::ObservedFiring(id, APMFBridge::kApmfHandLeft, spellID,
                                         APMFBridge::kObservedFiringRecencyMs))
            g_fired[id].insert(sk);

        // R2-1: remember when this claim's fire was first observed (the bound below).
        if (!conc && !instant && ComposedCast::ObservedFiring(id, APMFBridge::kApmfHandLeft, spellID,
                                                              APMFBridge::kObservedFiringRecencyMs))
            g_fireSeen[id].emplace(sk, std::chrono::steady_clock::now());

        // AN INSTANT SPELL leaves no effect for an up-read to see (a cure, a dispel): its claim ends once a
        // fire is observed SINCE THIS CLAIM (the lock's stamp, the shape CastOn's own bound uses), one cast
        // per claim. The rule re-claims next lap if it still wins.
        if (instant) {
            if (const auto stamp = CastLockClaimStamp(id, kHandLeft, spellID);
                stamp != std::chrono::steady_clock::time_point{}) {
                const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - stamp).count();
                if (ComposedCast::ObservedFiring(id, APMFBridge::kApmfHandLeft, spellID,
                                                 static_cast<std::uint32_t>(ageMs + 1))) {
                    g_fired[id].insert(sk);
                    spdlog::info("[buff] {:08X} {} ({:08X}): instant cast fired -- claim released (one cast per "
                                 "claim)", id, a_spell->GetName() ? a_spell->GetName() : "?", spellID);
                    EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                    return Outcome{ Result::NoOp, "instant buff cast fired: claim released", true };
                }
            }
        }

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
                if (const auto fi = g_fired.find(id); fi != g_fired.end() && fi->second.count(sk)) {
                    spdlog::info("[buff] {:08X} {} ({:08X}): fired earlier this fight, idle for {} ms with "
                                 "nothing charging -- claim released (no hold-off; the rule re-claims when it "
                                 "wants it again)", id, a_spell->GetName() ? a_spell->GetName() : "?", spellID,
                                 ageMs);
                    EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                    return Outcome{ Result::NoOp, "buff claim idle after a fire: released", true };
                }
                spdlog::warn("[buff] {:08X} {} ({:08X}) at {:08X} NEVER FIRED this fight: claimed {} ms ago, "
                             "nothing charging -- claim RELEASED and the rule held off until the next cast-rule "
                             "reset (no direct fallback; the engine's {} caster did not fire it: see "
                             "Harbinger's [ctcensus] verdict; rule {})",
                             id, a_spell->GetName() ? a_spell->GetName() : "?", spellID, rid, ageMs,
                             EngineRowName(ClassifyArchetype(a_spell, /*a_seat0=*/other || ExplicitSelfBuff(a_spell)).row), g_firingRule);
                g_neverFired[id].insert(sk);
                EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                return Outcome{ Result::NoOp, "buff claim never fired: released, held off this fight", true };
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
        if (instant) return std::nullopt;   // no up-read for an instant spell; the fire release is above
        const Up up = BuffUp(recip, a_spell, placement);
        if (up == Up::Blocked) {
            spdlog::info("[buff] {:08X} {:08X}: the summon limit is reached -- claim released", id, spellID);
            EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
            return Outcome{ Result::NoOp, "summon limit reached: claim released", true };
        }
        if (up != Up::Yes) {
            // FIRED BUT NEVER LANDED (R2-1): a fire was observed and the up-read still says No after the cap.
            if (const auto fs = g_fireSeen.find(id); fs != g_fireSeen.end()) {
                if (const auto it = fs->second.find(sk); it != fs->second.end() && up == Up::No) {
                    const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - it->second).count();
                    if (ageMs >= static_cast<long long>(APMFBridge::kHoldLastSeenCapMs)) {
                        spdlog::warn("[buff] {:08X} {} ({:08X}) at {:08X} FIRED but NEVER LANDED: first fire observed "
                                     "{} ms ago, the buff is still not up -- claim RELEASED and the rule held off "
                                     "until the next cast-rule reset (no direct fallback; rule {})",
                                     id, a_spell->GetName() ? a_spell->GetName() : "?", spellID, rid, ageMs,
                                     g_firingRule);
                        g_neverFired[id].insert(sk);
                        EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
                        return Outcome{ Result::NoOp, "buff claim fired but never landed: released, held off this fight", true };
                    }
                }
            }
            return std::nullopt;
        }
        g_fired[id].insert(sk);   // R3-1: it landed
        spdlog::info("[buff] {:08X} {:08X}: buff is up on {:08X} -- claim released (the cast landed)", id, spellID, rid);
        EndBuffClaim(a_follower, a_spell, /*a_takeBack=*/true);
        return Outcome{ Result::NoOp, "buff already up: claim released", true };
    }

}
