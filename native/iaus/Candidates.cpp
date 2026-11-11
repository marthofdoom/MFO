// iaus/Candidates.cpp -- the engine-side GATHER (contract and threading in iaus/Candidates.h). Card I4.
#include "iaus/Candidates.h"

#include "CombatSense.h"
#include "Confidence.h"
#include "Config.h"
#include "Followers.h"
#include "Sightline.h"
#include "Targeting.h"
#include "Taunt.h"
#include "Vocabulary.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
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
        std::unordered_map<RE::FormID, std::vector<SpellTarget>> g_wants;

        // A Gather this long after the last one starts a NEW fight clock when card I6 did not call CombatEnded.
        // Sized from the cadence (principle 9): one follower is served every 133 ms x party, about 1.3 s for ten,
        // so 10 s is more than seven missed laps and far shorter than any real pause between two fights.
        constexpr double kFightGapSec = 10.0;

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
        s.playerId = player ? player->GetFormID() : 0;

        // ── the fight clock ──────────────────────────────────────────────────────────────────────────────
        {
            const auto now = Clock::now();
            auto& fight = g_fights[selfID];
            const bool fresh = fight.seen.time_since_epoch().count() != 0 &&
                               std::chrono::duration<double>(now - fight.seen).count() <= kFightGapSec;
            if (!fresh) fight.start = now;
            fight.seen = now;
            s.combatSec = static_cast<float>(std::chrono::duration<double>(now - fight.start).count());
        }

        // ── the book (main-thread mirror) and his own hands ──────────────────────────────────────────────
        if (const auto book = SpellBook::Read(a_self)) {
            s.book = *book;
            s.hasBook = true;
        }
        s.drawn            = Drawn(a_self);
        s.meleeInRightHand = Taunt::MeleeInRightHand(a_self);
        s.meleeOnly        = Actuation::MeleeOnly(a_self);
        s.handBusyLeft     = Actuation::HealStandsOnHand(selfID, Actuation::kHandLeft);
        s.handBusyRight    = Actuation::HealStandsOnHand(selfID, Actuation::kHandRight);
        s.tauntEnabled     = Config::g_taunt.load(std::memory_order_relaxed);
        s.tauntBusy        = s.tauntEnabled && Taunt::Busy(selfID);   // card I6: act.taunt would refuse him now
        s.tauntReach       = Config::g_tauntRadius.load(std::memory_order_relaxed);
        s.meleeReach       = Config::g_meleeReach.load(std::memory_order_relaxed);

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
                // it is fighting this follower.
                const bool held = fid != 0 && (fid == heldFoeID || fid == entryFoeID);
                if (!(held && tgt.get() == a_self)) continue;
            }
            Combatant u;
            FillUnit(u, foe, a_self);
            if (tgt) {
                u.targetId = tgt->GetFormID();
                u.targetIsAlly = tgt.get() != a_self && !tgt->IsPlayerRef() && Taunt::IsPartyMember(tgt.get());
            }
            auto* avo = foe->AsActorValueOwner();
            u.paralysed = avo && avo->GetActorValue(RE::ActorValue::kParalysis) > 0.0f;
            if (auto* fcc = foe->GetActorRuntimeData().combatController; fcc && fcc->state) u.fleeing = fcc->state->isFleeing;
            u.blocking = foe->IsBlocking();
            {
                auto* r = foe->GetEquippedObject(false);
                auto* l = foe->GetEquippedObject(true);
                u.caster = (r && r->As<RE::SpellItem>()) || (l && l->As<RE::SpellItem>());   // Evaluator's FoeIsCaster
            }
            if (auto* race = foe->GetRace()) u.undead = race->HasKeywordString("ActorTypeUndead");
            u.unholy = Logistics::IsUnholy(foe);
            u.taunted = Taunt::TauntedBy(fid, selfID);   // card I6 (R1): the Taunt module's own live pin, not an engine read
            u.outOfMeleeReach = CombatSense::OutOfMeleeReach(a_self, foe, 0.0f, nullptr, selfHeight);
            u.sight       = ToSight(Sightline::Check(selfID, fid, Sightline::Basis::Own));
            u.sightEngine = ToSight(Sightline::Check(selfID, fid, Sightline::Basis::Engine));
            if (avo)
                for (const int av : resistAvs) {
                    const float r = avo->GetActorValue(static_cast<RE::ActorValue>(av));
                    if (std::isfinite(r)) u.resists.emplace_back(av, r);
                }
            wantFoes.push_back(fid);
            s.foes.push_back(std::move(u));
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
            wantAllies.push_back(u.id);
            s.allies.push_back(std::move(u));
        };
        addAlly(player);
        for (const auto& h : Followers::g_active) {
            auto ptr = h.get();   // HOLD the NiPointer
            addAlly(ptr.get());
        }

        // ── G10: only the pairs the last decision ranked on top (a call is also the prefetch) ────────────
        if (auto it = g_wants.find(selfID); it != g_wants.end())
            for (const auto& w : it->second) {
                bool live = false;
                for (const auto& f : s.foes)
                    if (f.id == w.target) { live = true; break; }
                if (!live) continue;
                auto* sp  = RE::TESForm::LookupByID<RE::SpellItem>(w.spell);
                auto* tgt = RE::TESForm::LookupByID<RE::Actor>(w.target);
                if (!sp || !tgt) continue;
                SpellTargetNote n{ w.spell, w.target, Tri::Unknown };
                switch (Actuation::WillWork(a_self, sp, tgt).verdict) {
                case Actuation::WillWorkVerdict::Yes:     n.answer = Tri::Yes; break;
                case Actuation::WillWorkVerdict::No:      n.answer = Tri::No; break;
                case Actuation::WillWorkVerdict::Unknown: break;
                }
                s.willWork.push_back(n);
            }

        // ── heals already running (worker-serial registries + the bridge's claim table) ──────────────────
        for (const auto& e : s.book.entries) {
            if (e.source != SpellSource::Spell || e.nature != Prune::Nature::Heal || e.drop != Prune::Drop::Keep) continue;
            // Every road a heal can be running on (I4 review F1): the direct SELF stream (g_selfCast), the direct
            // on-target stream (g_targetCast) and a standing Harbinger heal claim on either hand.
            if (Actuation::SelfStreamLive(selfID, e.form) || Actuation::HealClaimLive(selfID, e.form, 0))
                s.liveStreams.push_back({ e.form, 0u, Tri::Yes });
            if (e.selfDelivery) continue;
            for (const auto& a : s.allies)
                if (Actuation::TargetStreamLive(selfID, e.form, a.id) || Actuation::HealClaimLive(selfID, e.form, a.id))
                    s.liveStreams.push_back({ e.form, a.id, Tri::Yes });
        }

        // Ask the main thread to (re)measure the sight lines this tick read (rate-limited per pair inside).
        if (!wantFoes.empty()) {
            Sightline::Want(selfID, wantFoes, Sightline::Basis::Own);
            Sightline::Want(selfID, std::move(wantFoes), Sightline::Basis::Engine);
        }
        if (!wantAllies.empty()) Sightline::Want(selfID, std::move(wantAllies), Sightline::Basis::Own);
        return true;
    }

    bool Build(RE::Actor* a_follower, Set& a_out, const Tuning& a_t) {
        a_out.gen = Generated{};
        if (!Gather(a_follower, a_out.snap)) return false;
        a_out.gate = SpellBook::GateFor(a_out.snap.self.id);
        a_out.gen  = Generate(a_out.snap, a_out.gate, a_t);
        return true;
    }

    void NoteDecision(RE::FormID a_follower, const Decision& a_d, std::span<const Candidate> a_cands) {
        g_wants[a_follower] = TopSpellPairs(a_d, a_cands);
    }

    void CombatEnded(RE::FormID a_follower) {
        g_fights.erase(a_follower);
        g_wants.erase(a_follower);
    }

    void Forget(RE::FormID a_follower) { CombatEnded(a_follower); }

    void Clear() {
        g_fights.clear();
        g_wants.clear();
    }

    std::string Describe(const Set& a_set) {
        const auto& g = a_set.gen;
        const auto n = [&](Family f) { return g.perFamily[static_cast<std::size_t>(f)]; };
        return std::format("foes {}/{} | attack {} power {} taunt {} retreat {} drink {}/{}/{} posture {} | offense {} "
                           "control {} heal {} buff {} summon {} ward {} | dropped cost {} class {} hand {} target {} | "
                           "book {} emergency {:.2f}{}",
                           g.foesKept, g.foesSeen, n(Family::Attack), n(Family::PowerAttack), n(Family::Taunt),
                           n(Family::Retreat), n(Family::DrinkHealth), n(Family::DrinkStamina), n(Family::DrinkMagicka),
                           n(Family::Posture), n(Family::CastOffense), n(Family::CastControl), n(Family::CastHeal),
                           n(Family::CastBuff), n(Family::CastSummon), n(Family::CastWard), g.droppedCost,
                           g.droppedClass, g.droppedHand, g.droppedTarget, a_set.snap.hasBook ? "yes" : "NO",
                           g.emergency, g.selfUnreadable ? " SELF-UNREADABLE" : "");
    }

}
