// iaus/Candidates.cpp -- the engine-side GATHER (contract and threading in iaus/Candidates.h). Card I4.
#include "iaus/Candidates.h"

#include "CombatSense.h"
#include "classes/Classes.h"   // batch I item 2: the Thief's free stealth entry
#include "Confidence.h"
#include "Config.h"
#include "Followers.h"
#include "Sightline.h"
#include "Targeting.h"
#include "Taunt.h"
#include "Vocabulary.h"
#include "APMF_API.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
#include "Concealment.h"   // card G3: the concealment pick (asked here, answered on the main thread)
#include "iaus/Disengage.h"
#include "iaus/Engine.h"    // Q-I-2: the Mind's last fired decision (touchSpent)
#include "iaus/Fills.h"
#include "iaus/Hide.h"
#include "iaus/Reposition.h"   // batch I: H4, the back-off, the hidden step
#include "iaus/SpellBook.h"
#include "logistics/Logistics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_map>


namespace MFO::Iaus::Candidates {

    static_assert(Prune::kHealFull == Vocab::kHealFull, "the pure generator's heal-full mark is Vocab::kHealFull");

    namespace {

        using Clock = std::chrono::steady_clock;

        // g_followers domain (#4): the scan's worker only, or ResetAllState with the pump drained.
        struct Fight {
            Clock::time_point start{};
            Clock::time_point seen{};
        };
        std::unordered_map<RE::FormID, Fight>                    g_fights;
        std::unordered_set<std::uint64_t>                        g_lockLogged;   // (follower << 32 | spell): the recast-lockout drop line, once per spell per fight
        std::unordered_map<std::uint64_t, std::uint32_t>         g_replaceLogged;   // (follower << 32 | spell, target mix) -> the active spell it would replace: logged once per change
        std::unordered_map<RE::FormID, std::uint32_t>             g_chosenCast;   // Q-I-2: the cast form ranked first on the last decision
        std::unordered_map<RE::FormID, std::vector<AskedPair>>   g_wants;   // G10's ask list (Generate.h NextAskList)

        // fix/mage-buff-loop: drop a follower's self-buff recast lockout and its once-per-fight log marks. Worker domain.
        void ClearBuffLockState(RE::FormID a_follower) {
            Actuation::ClearSelfBuffLocks(a_follower);
            std::erase_if(g_lockLogged, [a_follower](std::uint64_t k) { return static_cast<RE::FormID>(k >> 32) == a_follower; });
            std::erase_if(g_replaceLogged, [a_follower](const auto& kv) { return static_cast<RE::FormID>(kv.first >> 32) == a_follower; });
        }

        // A Gather this long after the last one starts a NEW fight clock when card I6 did not call CombatEnded.
        // Sized from the cadence (principle 9): one follower is served every 133 ms x party, about 1.3 s for ten,
        // so 10 s is more than seven missed laps and far shorter than any real pause between two fights.
        constexpr double kFightGapSec = 10.0;

        // How long a refused (spell, target) pair stays on the G10 ask list after its last No (sim finding D1).
        // Gather re-stamps the No on every lap the answer still stands, so this only has to outlive the gap
        // between two answers. Sized from the cadences (principle 9): cast/WillWork.cpp serves a measured verdict
        // for its kTrustSec (6 s) and re-measures it in the background from kRefreshSec (2 s) on each call, and
        // one follower is served every 133 ms x party (about 1.3 s for ten). 6 s is that whole trust window: a
        // pair with no No for that long has no verdict left in the mirror either (a load, a lost post), and then
        // G10's own contract holds again: Unknown passes, the rule is never held.
        constexpr double kRefusedKeepSec = 6.0;

        double NowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

        // Occlusion age per (self, foe, ray basis), for the keep-current-attack hold (Tuning::occlusionHoldSec, marth
        // Q93). Each basis has its own age: a candidate reads the age of the ray it reads (Generate's Foe: the engine ray
        // for a melee swing). Worker only (the gather runs on the serial pump job). Returns seconds occluded without a
        // break, 0 = not. The hold's expiry is logged once per occlusion, for the foe he is on.
        float NoteOcclusion(RE::FormID a_self, RE::FormID a_foe, bool a_engine, bool a_occluded, bool a_current, float a_holdSec) {
            struct Rec { std::chrono::steady_clock::time_point since; bool logged = false; };
            static std::unordered_map<std::uint64_t, Rec> g_occBasis[2];
            auto& occ = g_occBasis[a_engine ? 1 : 0];
            const std::uint64_t key = (static_cast<std::uint64_t>(a_self) << 32) | a_foe;
            if (!a_occluded) { occ.erase(key); return 0.0f; }
            if (occ.size() > 512) occ.clear();
            const auto now = std::chrono::steady_clock::now();
            auto [it, fresh] = occ.try_emplace(key, Rec{ now, false });
            const float sec = std::chrono::duration<float>(now - it->second.since).count();
            if (a_current && !it->second.logged && sec >= a_holdSec) {
                it->second.logged = true;
                spdlog::info("[iaus] {:08X}: keep-current-attack hold on {:08X} EXPIRED, {} ray occluded {:.1f} s (hold {:.1f} s)",
                             a_self, a_foe, a_engine ? "engine" : "own", sec, a_holdSec);
            }
            return (std::max)(sec, 1.0e-3f);
        }

        Sight ToSight(Sightline::Verdict a_v) {
            switch (a_v) {
            case Sightline::Verdict::Visible:  return Sight::Visible;
            case Sightline::Verdict::Occluded: return Sight::Occluded;
            case Sightline::Verdict::Unknown:  break;
            }
            return Sight::Unknown;
        }

        // Vocab::Pct's maximum (permanent + the temporary modifier, where fortify lands), as absolute numbers.
        void ReadPool(RE::Actor* a_actor, RE::ActorValue a_av, float& a_cur, float& a_max) {
            a_cur = 0.0f;
            a_max = 0.0f;
            auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
            if (!avo) return;
            const float mx = avo->GetPermanentActorValue(a_av) +
                             a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av);
            const float cur = avo->GetActorValue(a_av);
            if (!std::isfinite(mx) || !std::isfinite(cur) || mx <= 0.0f) return;   // unreadable: max stays 0
            a_max = mx;
            a_cur = std::clamp(cur, 0.0f, mx);
        }

        // feat/buff-potions: the resist actor value of a spell's COSTLIEST hostile effect (SpellBook's rule for the book's element), -1 = none.
        int SpellResistAv(const RE::MagicItem* a_sp) {
            if (!a_sp) return -1;
            float best = -1.0f;
            int   av = -1;
            for (const auto* eff : a_sp->effects) {
                const auto* base = eff ? eff->baseEffect : nullptr;
                if (!base || !(base->IsHostile() || base->IsDetrimental()) || !(eff->cost > best)) continue;
                best = eff->cost;
                const auto rv = base->data.resistVariable;
                av = rv == RE::ActorValue::kNone ? -1 : static_cast<int>(rv);
            }
            return av;
        }

        // feat/buff-potions: the element a foe hits with, for the resist potions' threat term: the spell in a hand, else the one it is casting
        // right now (plain loads of the combat thread's fields, the Fills.cpp reads: a stale answer is one lap old, never corrupt). -1 = none known.
        // The magicCasters loop covers every caster slot including the shout / power slot, so a breath or shout being cast IS read while it casts.
        // Not read: an enchanted weapon, an innate creature attack, a power not being cast this lap.
        int FoeElementAv(RE::Actor* a_foe) {
            for (const bool left : { false, true }) {
                auto* o = a_foe->GetEquippedObject(left);
                if (const int av = SpellResistAv(o ? o->As<RE::SpellItem>() : nullptr); av >= 0) return av;
            }
            for (std::size_t i = 0; i < RE::Actor::SlotTypes::kTotal; ++i) {
                const RE::MagicCaster* c = a_foe->GetActorRuntimeData().magicCasters[i];
                if (!c) continue;
                const auto cs = c->state.get();
                if (cs == RE::MagicCaster::State::kNone || cs == RE::MagicCaster::State::kUnk08 || cs == RE::MagicCaster::State::kUnk09) continue;
                if (const int av = SpellResistAv(c->currentSpell); av >= 0) return av;
            }
            return -1;
        }

        // The reads every unit shares. a_self == a_actor for the follower himself.
        void FillUnit(Combatant& u, RE::Actor* a_actor, RE::Actor* a_self) {
            u.id   = a_actor->GetFormID();
            u.name = a_actor->GetName() ? a_actor->GetName() : "?";
            ReadPool(a_actor, RE::ActorValue::kHealth, u.hp, u.hpMax);
            ReadPool(a_actor, RE::ActorValue::kMagicka, u.mp, u.mpMax);
            ReadPool(a_actor, RE::ActorValue::kStamina, u.sp, u.spMax);
            const auto pos = a_actor->GetPosition();
            u.x = pos.x; u.y = pos.y; u.z = pos.z;
            if (a_actor != a_self) {
                const float d = a_self->GetPosition().GetDistance(pos);
                u.dist = std::isfinite(d) ? d : 0.0f;
            }
            u.level    = static_cast<int>(a_actor->GetLevel());
            u.isPlayer = a_actor->IsPlayerRef();
            const auto* st = a_actor->AsActorState();
            u.downed = st && st->IsBleedingOut();   // Evaluator's ActorDowned
        }

        PostureKind Drawn(RE::Actor* a_self) {
            auto* r = a_self->GetEquippedObject(false);
            auto* l = a_self->GetEquippedObject(true);
            auto* rw = r ? r->As<RE::TESObjectWEAP>() : nullptr;
            auto* lw = l ? l->As<RE::TESObjectWEAP>() : nullptr;
            if ((rw && rw->IsStaff()) || (lw && lw->IsStaff())) return PostureKind::Staff;
            if (rw && (rw->IsBow() || rw->IsCrossbow())) return PostureKind::Bow;
            if (rw && rw->IsMelee()) return Logistics::IsSilverMeleeWeapon(rw) ? PostureKind::Silver : PostureKind::Melee;
            return PostureKind::None;   // spells, a torch or bare hands: no weapon category is drawn
        }

    }

    bool Gather(RE::Actor* a_self, Snapshot& a_out) {
        a_out = Snapshot{};
        if (!a_self || a_self->IsDead() || a_self->IsDisabled() || !a_self->Is3DLoaded()) return false;
        Snapshot& s = a_out;
        const RE::FormID selfID = a_self->GetFormID();
        auto* player = RE::PlayerCharacter::GetSingleton();

        FillUnit(s.self, a_self, a_self);
        // The engine fills (iaus/Fills.h): the measured estimates of every unit, probe first, each input behind its
        // own [Debug] switch (all default OFF). Unarmed, it reads nothing.
        Fills::Pass fills;
        fills.Unit(s.self, a_self, Fills::Role::Self);
        s.playerId = player ? player->GetFormID() : 0;
        // G2 Layer 1: the stance flag, an actor-state bit (no engine call), counted only when the sneak is MFO's: bSneakInCombat on or
        // G1's ch.3 claim live. With the defaults an engine-forced sneak mid-fight changes nothing (identical to main).
        s.selfSneaking = a_self->IsSneaking() && (Config::g_sneakInCombat.load(std::memory_order_relaxed) ||
                                                  APMFBridge::SneakClaimHeld(selfID));
        s.hideRoad     = s.selfSneaking && APMFBridge::HideHoldSupported();   // asked only while it could matter (logs the absence once)
        const std::uint32_t selfHandle = a_self->GetHandle().native_handle();

        // ── the fight clock ──────────────────────────────────────────────────────────────────────────────
        bool newFight = false;   // batch I item 2: this lap starts a new fight for him
        {
            const auto now = Clock::now();
            auto& fight = g_fights[selfID];
            const bool fresh = fight.seen.time_since_epoch().count() != 0 &&
                               std::chrono::duration<double>(now - fight.seen).count() <= kFightGapSec;
            if (!fresh) {
                fight.start = now; newFight = true;
                ClearBuffLockState(selfID);   // fix/mage-buff-loop: the lockout and its log are per fight
            }
            fight.seen = now;
            s.combatSec = static_cast<float>(std::chrono::duration<double>(now - fight.start).count());
        }

        // ── the book (main-thread mirror) and his own hands ──────────────────────────────────────────────
        if (const auto book = SpellBook::Read(a_self)) {
            s.book = *book;
            s.hasBook = true;
            // fix/mage-buff-loop, on the snapshot's own COPY of the book (the shared one stays immutable).
            auto* player = RE::PlayerCharacter::GetSingleton();
            for (auto& e : s.book.entries) {
                if (e.source != SpellSource::Spell || e.drop != Prune::Drop::Keep) continue;
                // 1dfcuabd309 (marth 11-13: "proxied spell versions need to be considered as well"): a Self-delivery HEAL is
                // offered to an ally where a proxy road serves it (Harbinger's heal-claim delivery-flip proxy, or MFO's
                // ConcProxy for a concentration heal). The candidate stays keyed to the SOURCE spell; the proxy is minted by
                // the road at fire time and never enters the book (SpellBook skips 0xFF forms).
                if (e.nature == Prune::Nature::Heal) {
                    if (e.selfDelivery && player && player != a_self)
                        if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(e.form))
                            e.allyRoad = Actuation::HealServedAtAlly(a_self, sp, player);
                    continue;
                }
                if (e.nature != Prune::Nature::Buff) continue;
                // Item 1: a buff he applied to himself and whose effect leaves no active effect of its own (SelfHazard /
                // ScriptRider) is invisible to the book's activeOn. The recast lockout stands in for it while it lasts.
                if (Actuation::SelfBuffRecastLocked(selfID, e.form) &&
                    std::find(e.activeOn.begin(), e.activeOn.end(), selfID) == e.activeOn.end()) {
                    e.activeOn.push_back(selfID);
                    if (g_lockLogged.insert((static_cast<std::uint64_t>(selfID) << 32) | e.form).second)
                        spdlog::info("[iaus] {:08X} buff {} ({:08X}) dropped as a candidate: recast lockout (he applied it to himself "
                                     "and its authored life has not run out; once per spell per fight)", selfID, e.name, e.form);
                }
                // Item 5: a Self-delivery buff is offered to an ally only where a proxy road serves it (the claim road's flip
                // proxy, or for a concentration buff CastOn's concentration fork: Harbinger's claim / MFO's ConcProxy).
                if (e.selfDelivery && player && player != a_self)
                    if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(e.form))
                        e.allyRoad = Actuation::BuffServedAtAlly(a_self, sp, player);
            }
        }
        s.drawn            = Drawn(a_self);
        // H4 (batch I): the spell in his hand, right first (a one-handed cast comes from it; a dual cast holds the same in both). The book
        // supplies its reach / delivery (iaus/Reposition.h EquippedEntry); 0 = none.
        {
            auto* r = a_self->GetEquippedObject(false);
            auto* l = a_self->GetEquippedObject(true);
            auto* rs = r ? r->As<RE::SpellItem>() : nullptr;
            auto* ls = l ? l->As<RE::SpellItem>() : nullptr;
            if (rs) { s.equippedSpell = rs->GetFormID(); if (ls && ls != rs) s.equippedSpellOther = ls->GetFormID(); }
            else if (ls) s.equippedSpell = ls->GetFormID();
        }
        s.meleeInRightHand = Taunt::MeleeInRightHand(a_self);
        s.meleeOnly        = Actuation::MeleeOnly(a_self);
        s.handBusyLeft     = Actuation::HealStandsOnHand(selfID, Actuation::kHandLeft);
        s.handBusyRight    = Actuation::HealStandsOnHand(selfID, Actuation::kHandRight);
        s.tauntEnabled     = Config::g_taunt.load(std::memory_order_relaxed);
        s.tauntBusy        = s.tauntEnabled && Taunt::Busy(selfID);   // card I6: act.taunt would refuse him now
        s.tauntReach       = Config::g_tauntRadius.load(std::memory_order_relaxed);
        s.meleeReach       = Config::g_meleeReach.load(std::memory_order_relaxed);
        // Q-I-2: the touch spell of his hands (if any) cast within kTouchSpentSec = he is backing out to the distance spell's range.
        // The Mind's running decision is the last fired one (worker-only, which this gather is).
        if (const auto cc = g_chosenCast.find(selfID); cc != g_chosenCast.end()) s.chosenCast = cc->second;
        if (s.equippedSpellOther != 0) {
            const std::uint32_t tf = Reposition::HandsRange(s.book, s.equippedSpell, s.equippedSpellOther, s.meleeReach, false, 0u).touchForm;
            const Mind& m = Engine::MindFor(selfID);
            const double ago = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() - m.firedAt;
            s.touchAgo   = tf != 0 && m.hasRunning && m.running.form == tf && ago >= 0.0 ? ago : -1.0;
            s.touchSpent = s.touchAgo >= 0.0 && s.touchAgo <= static_cast<double>(Reposition::kTouchSpentSec);   // Build re-derives it with the class's touchSpentSec
        }
        // The Bash family exists only while its road does: Harbinger's combat move (ch.31, ABI v38) and the switch that
        // stays OFF until the field run proves that road ([Debug] bIausBashRoad). Harbinger absent = no candidate.
        s.bashRoad         = Config::g_iausBashRoad.load(std::memory_order_relaxed) && APMFBridge::CombatMoveSupported();
        // The held guard (batch I item 4, marth 11-12, MFO-B800): Harbinger serves the move on this game (asked once per load,
        // the ABI v35 dry run) and his hands can block now. The CLASS row decides who may (ClassGate::block); guardHeld is the
        // held-guard gate Generate reads (a guard that stands denies his own offense, marth Q-melee-3).
        s.guardHeld        = APMFBridge::BlockHoldLive(selfID);
        s.blockRoad        = APMFBridge::BlockSupported() &&
                             APMFBridge::CombatMoveUnservedReason(selfID, APMF_API::kCombatMove_Block) == nullptr &&
                             APMFBridge::BlockHandsReason(a_self) == nullptr;
        // The same for a step back: Harbinger's ch.24 on a point (ABI v39) and [Debug] bIausGroundRoad.
        s.groundRoad       = Config::g_iausGroundRoad.load(std::memory_order_relaxed) && APMFBridge::GroundStepSupported();

        // The resist actor values the book's offense spells key on (the exploit consideration's read).
        std::vector<int> resistAvs;
        for (const auto& e : s.book.entries)
            if (e.resistAv >= 0 && std::find(resistAvs.begin(), resistAvs.end(), e.resistAv) == resistAvs.end())
                resistAvs.push_back(e.resistAv);

        // ── targets ──────────────────────────────────────────────────────────────────────────────────────
        if (auto cur = Targeting::Current(selfID).get(); cur) s.currentTargetId = cur->GetFormID();
        else if (auto eng = a_self->GetActorRuntimeData().currentCombatTarget.get(); eng) s.currentTargetId = eng->GetFormID();
        if (player)
            if (auto pt = player->GetActorRuntimeData().currentCombatTarget.get(); pt) s.playerTargetId = pt->GetFormID();

        // ── foes: P3's gates, the SAME set and order of tests as Evaluator's PickFoe ─────────────────────
        // Everything that takes another lock is read BEFORE the combat group's read lock (#23: ChaseRadius ->
        // Confidence -> CombatSense::FoeLoad takes that same lock; the bridge reads take its g_mx).
        s.chaseCap = Confidence::ChaseRadius(a_self);
        RE::FormID heldFoeID = 0, entryFoeID = 0;
        {
            if (auto pp = APMFBridge::PinnedTarget(selfID).get(); pp) heldFoeID = pp->GetFormID();
            RE::FormID et = 0;
            if (APMFBridge::CombatEntryStateOf(selfID, &et) == APMFBridge::EntryState::Standing) entryFoeID = et;
        }
        const std::vector<RE::FormID> givenUp = APMFBridge::EngageGiveUpsOf(selfID);

        // Under the group lock: copy handles only. Every actor read is made after it is released.
        std::vector<RE::ActorHandle> raw;
        if (auto* cc = a_self->GetActorRuntimeData().combatController; cc && cc->combatGroup) {
            RE::BSReadLockGuard lk(cc->combatGroup->lock);
            for (const auto& t : cc->combatGroup->targets) {
                if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;   // the engine lost track of it
                raw.push_back(t.targetHandle);
            }
        }
        const float selfHeight = CombatSense::BodyHeight(a_self);
        std::vector<RE::FormID> wantFoes;
        for (const auto& h : raw) {
            auto ptr  = h.get();   // HOLD the NiPointer
            auto* foe = ptr.get();
            if (!foe || foe == a_self) continue;
            const RE::FormID fid = foe->GetFormID();
            if (APMFBridge::IsSubstrateAnchor(fid)) continue;                       // Harbinger's anchor is never a foe
            if (std::find(givenUp.begin(), givenUp.end(), fid) != givenUp.end()) continue;   // given up (ABI v31)
            if (foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
            if (!foe->IsHostileToActor(a_self)) continue;                           // the brawl gate (#34)
            auto tgt = foe->GetActorRuntimeData().currentCombatTarget.get();        // HOLD it for the tests
            const float dist = a_self->GetPosition().GetDistance(foe->GetPosition());
            if (dist > s.chaseCap) {
                // The chase cap waiver (marth 2026-10-19): kept while Harbinger holds the engagement on it AND
                // it is fighting this follower OR the player (marth 2026-11-13: "fighting the player is enough doesnt need
                // to be HIM"; Jesper dropped a circling dragon that targeted the player).
                const bool held = fid != 0 && (fid == heldFoeID || fid == entryFoeID);
                if (!(held && (tgt.get() == a_self || (tgt && tgt->IsPlayerRef())))) continue;
            }
            Combatant u;
            FillUnit(u, foe, a_self);
            if (tgt) {
                u.targetId = tgt->GetFormID();
                u.targetIsAlly = tgt.get() != a_self && !tgt->IsPlayerRef() && Taunt::IsPartyMember(tgt.get());
            }
            auto* avo = foe->AsActorValueOwner();
            u.liveFx = SpellBook::FoeFx(fid);   // the main-thread mirror's last read (empty until the first one lands)
            u.paralysed = avo && avo->GetActorValue(RE::ActorValue::kParalysis) > 0.0f;
            if (auto* fcc = foe->GetActorRuntimeData().combatController; fcc && fcc->state) u.fleeing = fcc->state->isFleeing;
            u.blocking = foe->IsBlocking();
            {
                auto* r = foe->GetEquippedObject(false);
                auto* l = foe->GetEquippedObject(true);
                u.caster = (r && r->As<RE::SpellItem>()) || (l && l->As<RE::SpellItem>());   // Evaluator's FoeIsCaster
            }
            u.incomingAv = FoeElementAv(foe);   // feat/buff-potions: the element its spell hits with (resist potions' threat term)
            if (auto* race = foe->GetRace()) u.undead = race->HasKeywordString("ActorTypeUndead");
            u.unholy = Logistics::IsUnholy(foe);
            u.tauntedByMe = Taunt::TauntedBy(fid, selfID);
            u.taunted = Taunt::IsPinned(fid);   // card I6 (R1): the Taunt module's own live pin by ANY taunter (Q69), not an engine read
            u.outOfMeleeReach = CombatSense::OutOfMeleeReach(a_self, foe, 0.0f, nullptr, selfHeight);
            // G2 Layer 1: the foe's OWN group's record of him (iaus/Hide.h). The follower's group lock above is released, so this
            // read lock is taken alone and nothing is nested (the TauntProbe.cpp ServiceP2 shape).
            u.detectsMe = Hide::Verdict(Hide::Read(foe, selfHandle));
            u.sight       = ToSight(Sightline::Check(selfID, fid, Sightline::Basis::Own));
            u.sightEngine = ToSight(Sightline::Check(selfID, fid, Sightline::Basis::Engine));
            const float holdSec = DefaultTuning().occlusionHoldSec;   // the log line only: Gather has no Tuning
            u.occludedSec       = NoteOcclusion(selfID, fid, false, u.sight == Sight::Occluded, fid == s.currentTargetId, holdSec);
            u.occludedSecEngine = NoteOcclusion(selfID, fid, true, u.sightEngine == Sight::Occluded, fid == s.currentTargetId, holdSec);
            if (avo)
                for (const int av : resistAvs) {
                    const float r = avo->GetActorValue(static_cast<RE::ActorValue>(av));
                    if (std::isfinite(r)) u.resists.emplace_back(av, r);
                }
            fills.Unit(u, foe, Fills::Role::Foe);
            wantFoes.push_back(fid);
            s.foes.push_back(std::move(u));
        }

        // ── batch I item 2: the Thief's free stealth entry (classes/StealthEntry.cpp). NEAR-INERT, said plainly (review F3): Hide::Verdict is
        // already No when a foe's group has no record of him, on every fight; Unknown only means the foe has no combat group this lap. On the
        // guaranteed fight such a foe is read as unaware; a Yes verdict spends the entry. A real guarantee needs the H10 lever (not built).
        s.freeEntry = Classes::StealthEntryLive(selfID, newFight);
        if (s.freeEntry) {
            bool anyYes = false;
            for (const auto& f : s.foes) if (f.detectsMe == Tri::Yes) { anyYes = true; break; }
            if (anyYes) {
                Classes::StealthEntrySpend(selfID, "a foe's own group record has him (the engine's verdict)");
                s.freeEntry = false;
            } else {
                for (auto& f : s.foes) if (f.detectsMe == Tri::Unknown) f.detectsMe = Tri::No;
            }
        }

        // ── allies: the player and the active followers (the Evaluator's PickAlly set, without its radius) ──
        std::vector<RE::FormID> wantAllies;
        const auto addAlly = [&](RE::Actor* a) {
            if (!a || a == a_self || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded()) return;
            for (const auto& have : s.allies)
                if (have.id == a->GetFormID()) return;
            Combatant u;
            FillUnit(u, a, a_self);
            u.sight = ToSight(Sightline::Check(selfID, u.id, Sightline::Basis::Own));
            u.sightEngine = u.sight;
            fills.Unit(u, a, Fills::Role::Ally);
            wantAllies.push_back(u.id);
            s.allies.push_back(std::move(u));
        };
        addAlly(player);
        for (const auto& h : Followers::g_active) {
            auto ptr = h.get();   // HOLD the NiPointer
            addAlly(ptr.get());
        }
        // Review F2: a concentration Self buff at an ally runs as a minted 0xFF proxy (Harbinger's claim or the ConcProxy), so
        // the book's activeOn (keyed to ae->spell == source) never reads it up there. The stream registries are keyed to the
        // SOURCE form: a live stream at the ally = up there. (After the allies are built; the buff branch runs before them.)
        if (s.hasBook)
            for (auto& e : s.book.entries) {
                if (!e.allyRoad || e.nature != Prune::Nature::Buff) continue;
                for (const auto& al : s.allies)
                    if (Actuation::TargetStreamLive(selfID, e.form, al.id) &&
                        std::find(e.activeOn.begin(), e.activeOn.end(), al.id) == e.activeOn.end())
                        e.activeOn.push_back(al.id);
            }

        // NO BUFF THAT REPLACES AN ACTIVE BUFF IS VALID (marth 2026-11-13: "no spell that replaced an active buff is valid. The tuning
        // should pick the correct buff and only rebuff it if it runs out", fix/iaus-claim-thrash). The LIVE half is the book's: SpellBook
        // marks a party member active for a spell whose effect shares a base MGEF with a live one (any source, a proxy cast included) or
        // dispels it by keyword (the engine's own two rules, iaus/Snapshot.h FxReplaces). The IN-FLIGHT half is here: a buff whose
        // STANDING CLAIM is on a follower's hand (his lock names it and APMF's claim stands: true before any caster is built, which is the
        // window the thrash lived in) marks its recipient active for every OTHER buff its effects would replace. It returns when the live
        // one has run out: there is no refresh margin to read. Which buff is first cast is the tuning's job; nothing here ranks them.
        if (s.hasBook) {
            auto& es = s.book.entries;
            const auto buffLike = [](const SpellEntry& e) {
                return e.source == SpellSource::Spell && e.drop == Prune::Drop::Keep && !e.fx.empty() &&
                       (e.nature == Prune::Nature::Buff || e.nature == Prune::Nature::Ward);
            };
            for (std::size_t i = 0; i < es.size(); ++i) {
                if (!buffLike(es[i])) continue;
                RE::FormID target = 0;
                bool       stands = false;
                for (const std::size_t hand : { Actuation::kHandLeft, Actuation::kHandRight })
                    if (Actuation::CastClaimStandingOnHand(selfID, hand, es[i].form, target)) { stands = true; break; }
                if (!stands) continue;
                for (std::size_t j = 0; j < es.size(); ++j) {
                    if (j == i || !buffLike(es[j])) continue;   // its own candidate stays: the claim road answers "in flight"
                    bool byKw = false;
                    const bool replaces = std::any_of(es[j].fx.begin(), es[j].fx.end(), [&](const FxKey& a) {
                        return std::any_of(es[i].fx.begin(), es[i].fx.end(), [&](const FxKey& l) { return FxReplaces(a, l, byKw); });
                    });
                    if (!replaces || std::find(es[j].activeOn.begin(), es[j].activeOn.end(), target) != es[j].activeOn.end()) continue;
                    es[j].activeOn.push_back(target);
                    const std::uint64_t key = (static_cast<std::uint64_t>(selfID) << 32) | (es[j].form ^ (target * 0x9E3779B1u));
                    auto& said = g_replaceLogged[key];
                    if (said != es[i].form) {
                        said = es[i].form;
                        spdlog::info("[iaus] {:08X} buff {} ({:08X}) pruned on {:08X}: it would replace {} ({:08X}), whose claim stands there "
                                     "({}); offered again when that one has run out",
                                     selfID, es[j].name, es[j].form, target, es[i].name, es[i].form, byKw ? "dispel by keyword" : "same effect");
                    }
                }
            }
        }

        // ── G10: the pairs the last decision ranked on top, plus the pairs still refused (a call is also the
        // prefetch). A No is stamped on its entry, so NoteDecision keeps a refused pair on the list although the
        // decision no longer ranks it (D1: it used to drop off and come back as Unknown every second lap).
        if (auto it = g_wants.find(selfID); it != g_wants.end()) {
            const double nowSec = NowSec();
            for (auto& w : it->second) {
                bool live = false;
                for (const auto& f : s.foes)
                    if (f.id == w.pair.target) { live = true; break; }
                if (!live) continue;   // out of the fight this lap: not asked, the entry ages on its own
                auto* sp  = RE::TESForm::LookupByID<RE::SpellItem>(w.pair.spell);
                auto* tgt = RE::TESForm::LookupByID<RE::Actor>(w.pair.target);
                if (!sp || !tgt) continue;
                SpellTargetNote n{ w.pair.spell, w.pair.target, Tri::Unknown };
                switch (Actuation::WillWork(a_self, sp, tgt).verdict) {
                case Actuation::WillWorkVerdict::Yes:     n.answer = Tri::Yes; w.refused = false; break;
                case Actuation::WillWorkVerdict::No:      n.answer = Tri::No; w.refused = true; w.lastNo = nowSec; break;
                case Actuation::WillWorkVerdict::Unknown: break;   // no fresh verdict: Unknown passes (G10), the entry keeps its age
                }
                s.willWork.push_back(n);
            }
        }

        // ── heals already running (worker-serial registries + the bridge's claim table) ──────────────────
        for (const auto& e : s.book.entries) {
            if (e.source != SpellSource::Spell || e.nature != Prune::Nature::Heal || e.drop != Prune::Drop::Keep) continue;
            // Every road a heal can be running on (I4 review F1): the direct SELF stream (g_selfCast), the direct
            // on-target stream (g_targetCast) and a standing Harbinger heal claim on either hand.
            if (Actuation::SelfStreamLive(selfID, e.form) || Actuation::HealClaimLive(selfID, e.form, 0))
                s.liveStreams.push_back({ e.form, 0u, Tri::Yes });
            if (e.selfDelivery && !e.allyRoad) continue;   // review F1: a Self heal served at an ally (allyRoad) can be streaming there
            for (const auto& a : s.allies)
                if (Actuation::TargetStreamLive(selfID, e.form, a.id) || Actuation::HealClaimLive(selfID, e.form, a.id))
                    s.liveStreams.push_back({ e.form, a.id, Tri::Yes });
        }

        // Ask the main thread to (re)measure the sight lines this tick read (rate-limited per pair inside).
        if (!wantFoes.empty()) {
            SpellBook::WantFoeFx(wantFoes);   // the foes' live effects, read on the main thread (the next lap sees them)
            Sightline::Want(selfID, wantFoes, Sightline::Basis::Own);
            Sightline::Want(selfID, std::move(wantFoes), Sightline::Basis::Engine);
        }
        if (!wantAllies.empty()) Sightline::Want(selfID, std::move(wantAllies), Sightline::Basis::Own);
        fills.Finish(s, a_self);   // Snapshot::mortal and this lap's [iaus-fill] probe lines
        return true;
    }

    bool Build(RE::Actor* a_follower, Set& a_out, const Tuning& a_t) {
        a_out.gen = Generated{};
        if (!Gather(a_follower, a_out.snap)) return false;
        a_out.gate = SpellBook::GateFor(a_out.snap.self.id);
        // Per-class curves: the class's own touch window (the default row = Reposition::kTouchSpentSec, so unchanged until a row differs).
        if (a_out.snap.touchAgo >= 0.0) a_out.snap.touchSpent = a_out.snap.touchAgo <= static_cast<double>(a_out.gate.touchSpentSec);
        // Card G3: the disengage exists only while its road does, and a point is asked for only when the generator could use
        // one: a sneak class that a foe has found (a hiding place), or a class-tree-ON non-sneak class under the flee's own
        // test (hurt and a foe on him: a point away from the foes). The pick runs on the main thread, spread over frames
        // (Concealment.cpp); `concealPoint` is true once a fresh answer is in. A flee that already runs is a fact, not a pick.
        a_out.snap.concealRoad  = Concealment::RoadOpen();
        a_out.snap.concealPoint = false;
        a_out.snap.fleeRun      = a_out.snap.concealRoad && Concealment::FleeRunning(a_out.snap.self.id);
        bool concealWanted      = false;   // batch I: a hide or a flee is wanted this lap (its pick owns the single pick slot)
        if (a_out.snap.concealRoad && !a_out.snap.fleeRun) {
            const Snapshot& sn = a_out.snap;
            const float hpPct = sn.self.hpMax > 0.0f ? sn.self.hp / sn.self.hpMax : 1.0f;
            const bool hide = a_out.gate.sneakClass && a_out.gate.disengage && Disengage::Found(sn);
            const bool flee = !a_out.gate.sneakClass && a_out.gate.treeOn && a_out.gate.disengage &&
                              hpPct <= Disengage::kFleeHealthLine && Disengage::Threatened(sn);
            concealWanted = hide || flee;
            if (hide || flee) {
                std::vector<RE::FormID> foes;
                foes.reserve(sn.foes.size());
                for (const auto& f : sn.foes) foes.push_back(f.id);
                a_out.snap.concealPoint = Concealment::Prepare(a_follower, foes, /*a_flee=*/!hide);
            }
        }
        // BATCH I: the other two short H3 runs share the hide road's switch and its pick machinery (Concealment::PrepareKind). A pick is asked
        // for only while the generator could use it, and never while a hide run or a flee stands or is wanted (a hide or a flee outranks both).
        {
            Snapshot& sn = a_out.snap;
            sn.backOffRoad = Concealment::RoadOpen();
            sn.stepRoad    = sn.backOffRoad;
            const bool hideOrFlee = concealWanted || sn.fleeRun;
            if (sn.backOffRoad && !hideOrFlee && !sn.foes.empty()) {
                std::vector<RE::FormID> foes;
                foes.reserve(sn.foes.size());
                for (const auto& f : sn.foes) foes.push_back(f.id);
                if (Reposition::BackOffNeed(sn, a_out.gate) > 0.0f)
                    sn.backOffPoint = Concealment::PrepareKind(a_follower, foes, Concealment::Kind::BackOff);
                else if (const Combatant* tgt = Reposition::StepTarget(sn))
                    sn.stepPoint = Concealment::PrepareKind(a_follower, foes, Concealment::Kind::Step, tgt->id, sn.drawn == PostureKind::Bow);
            }
        }
        {   // the spellsHold latch (Generate::Postures): last tick's answer for this follower goes in, this tick's is kept
            static std::unordered_map<RE::FormID, bool> s_spellsHold;
            const RE::FormID id = a_follower->GetFormID();
            const auto it = s_spellsHold.find(id);
            a_out.snap.spellsHoldPrev = it != s_spellsHold.end() && it->second;
            a_out.gen  = Generate(a_out.snap, a_out.gate, a_t);
            s_spellsHold[id] = a_out.gen.spellsHold;
        }
        return true;
    }

    void NoteDecision(RE::FormID a_follower, const Decision& a_d, std::span<const Candidate> a_cands) {
        auto& asked = g_wants[a_follower];
        asked = NextAskList(asked, TopSpellPairs(a_d, a_cands), NowSec(), kRefusedKeepSec);
        // Q-I-2: the first-ranked candidate, when it is a cast (a spell form), is "the cast IAUS chose" for the touch range.
        std::uint32_t chosen = 0;
        if (!a_d.ranked.empty() && a_d.ranked.front().idx < a_cands.size()) {
            const Candidate& top = a_cands[a_d.ranked.front().idx];
            if (top.family == Family::CastOffense || top.family == Family::CastControl) chosen = top.form;
        }
        g_chosenCast[a_follower] = chosen;
    }

    void CombatEnded(RE::FormID a_follower) {
        ClearBuffLockState(a_follower);   // fix/mage-buff-loop
        Classes::StealthEntryForget(a_follower);   // batch I item 2: the next fight is counted afresh
        g_fights.erase(a_follower);
        g_wants.erase(a_follower);
        g_chosenCast.erase(a_follower);
    }

    void Forget(RE::FormID a_follower) { CombatEnded(a_follower); }

    void Clear() {
        Actuation::ClearSelfBuffLocks(0);   // (ClearSelfCasts does the same; the pair stays whole on its own)
        g_lockLogged.clear();
        Classes::StealthEntryClear();
        g_fights.clear();
        g_wants.clear();
        g_chosenCast.clear();
        Fills::Clear();   // the fills' tracker and pending hit records: the same domain and the same moment
    }

    std::string Describe(const Set& a_set) {
        const auto& g = a_set.gen;
        const auto n = [&](Family f) { return g.perFamily[static_cast<std::size_t>(f)]; };
        return std::format("foes {}/{} | attack {} power {} taunt {} retreat {} drink {}/{}/{} buff-potion {} posture {} | offense {} "
                           "control {} heal {} buff {} summon {} ward {} hold {} | dropped cost {} class {} hand {} target {} | "
                           "book {} emergency {:.2f}{}",
                           g.foesKept, g.foesSeen, n(Family::Attack), n(Family::PowerAttack), n(Family::Taunt),
                           n(Family::Retreat), n(Family::DrinkHealth), n(Family::DrinkStamina), n(Family::DrinkMagicka), n(Family::DrinkBuff),
                           n(Family::Posture), n(Family::CastOffense), n(Family::CastControl), n(Family::CastHeal),
                           n(Family::CastBuff), n(Family::CastSummon), n(Family::CastWard), n(Family::Sneak), g.droppedCost,
                           g.droppedClass, g.droppedHand, g.droppedTarget, a_set.snap.hasBook ? "yes" : "NO",
                           g.emergency, g.selfUnreadable ? " SELF-UNREADABLE" : "");
    }

}
