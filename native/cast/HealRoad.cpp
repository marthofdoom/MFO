// THE PER-HAND HEAL ROAD (feat/mfo-perhand-heal, 2026-10-05, batch A).
//
// marth, 2026-09-30 (verbatim):
//   "Shouldnt it charge and hold the heal and then fire as soon as los is clear."
//   "the right hand is free to keep doing its own casts (likely offensive) if its still
//    holding a spell"
//   "This system shouldnt need to have a cap, if another heal target is needed the right
//    hand loses its offensive spell and they both enter this state"
//   "Nothing should ever be locked up, its a very fluid system"
//   "One per follower is fine for this, the other hand would be for a second hurting
//    follower"
//
// THE MODEL. A heal claim (Harbinger ch.8b kIntent_Cast, arbitrated per (actor, hand))
// lives in ONE hand and serves ONE recipient. The first recipient takes the LEFT hand
// (the road that animated in the field: Loadout::Prepare puts the spell there). A
// DIFFERENT recipient wanted while the left already heals someone takes the RIGHT hand,
// displacing a right-hand offense by carried rank (ResolveCastHand / CanPreemptHand:
// the stored rule index, urgent heals take a lower-ranked offense mid-charge), never a
// weapon hand (the weapon-hand-exclusion hard rule). When that heal ends (the recipient
// healed to the rule's threshold, dead, gone, beyond reach) the right hand returns to
// offense: its claim is released and the offense rules take it on their next lap. A
// third recipient waits for a hand on the LEFT exactly as before (MFO-B171: a re-aim
// only in the idle gap between casts).
//
// FLUID, NOT LOCKED. Nothing here is a lock or a cap: every heal lap re-judges both
// hands (this lap's recipient and the rule's heal on the other hand), the gambit scan
// runs every lap, higher-ranked rules preempt at once, and the drop conditions are
// checked every lap. Line of sight never ends a claim (reach only, 2026-09-30b): the
// claim stands, the engine charges and fires when sight clears. That waiting state is
// now PER HAND (`HealHand::losHeld`), logged when it starts and when sight returns, so
// a later round can add a move-to on top of it.
//
// THREADING (#4). Everything in this file runs on the SKSE AddTask job worker, serial
// with CastOn / CastAuto / the bridge's Tick (the same worker every g_castLock access
// uses). `g_healHands` takes no lock for that reason; ResetHealRoad runs from
// ClearCastLocks with the pump drained (ResetAllState: StopPump first). The bridge
// accessors take APMFBridge's own g_mx. Nothing here touches the engine except reads
// (health, life state, position, the magic casters through CastInFlightOnHand).

#include "PCH.h"
#include "Actuation_internal.h"
#include "apmf/APMFBridge.h"
#include "ComposedCast.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::Actuation {

    namespace {
        using Clock = std::chrono::steady_clock;

        inline std::int32_t ApmfHand(std::size_t a_hand) {
            return a_hand == kHandLeft ? APMFBridge::kApmfHandLeft : APMFBridge::kApmfHandRight;
        }
        inline const char* HandWord(std::size_t a_hand) { return a_hand == kHandLeft ? "LEFT" : "RIGHT"; }
        inline RE::FormID Who(RE::FormID a_follower, RE::FormID a_recipient) {
            return a_recipient == 0 ? a_follower : a_recipient;
        }

        // The per-hand record. `active` = a heal claim this record names was claimed on the
        // hand (HealHandClaimed) and has not been seen ending yet. `displacing*` is lap
        // scratch: the right-hand offense this lap's plan would take the hand from, read
        // once by HealHandClaimed for its line.
        struct HealHand {
            bool              active    = false;
            RE::FormID        spell     = 0;
            RE::FormID        recipient = 0;   // 0 = self
            int               rule      = kNoRule;
            bool              losHeld   = false;
            Clock::time_point losSince{};
            RE::FormID        displacing     = 0;
            int               displacingRule = kNoRule;
        };
        struct FollowerHealHands { HealHand hand[kHandCount]; };
        std::unordered_map<RE::FormID, FollowerHealHands> g_healHands;

        // WHAT HEAL STANDS ON A HAND: a live claim (the bridge's per-hand slot), or, in the
        // concentration stream cap's one-lap re-stream gap (MFO-B177), the lock that keeps
        // the hand and its rank for the re-claim. `found` false = no heal on that hand.
        struct HandHeal {
            bool       found     = false;
            bool       claim     = false;
            RE::FormID spell     = 0;
            RE::FormID recipient = 0;   // 0 = self
        };
        HandHeal HealOnHand(RE::FormID a_follower, std::size_t a_hand) {
            const auto ah = ApmfHand(a_hand);
            if (const RE::FormID sp = APMFBridge::GetHealCastSpell(a_follower, ah); sp != 0)
                return { true, true, sp, APMFBridge::GetHealCastTarget(a_follower, ah) };
            const auto it = g_castLock.find(a_follower);
            if (it == g_castLock.end()) return {};
            const auto& lk = it->second.hand[a_hand];
            if (lk.spell == 0 || lk.restreamAt.time_since_epoch().count() == 0) return {};
            const auto window = std::chrono::duration_cast<Clock::duration>(APMFBridge::FacetExpiry());
            if (Clock::now() - lk.restreamAt > window) return {};   // HealRestreamRule's window
            return { true, false, lk.spell, lk.target };
        }

        // THE PER-LAP RECIPIENT JUDGEMENT (the tests CastOn's heal road made for the lap's
        // recipient, moved here and made per hand). a_hand = the hand whose claim serves
        // him, or kHandCount when none does yet (no cast of his can be in flight then).
        //   * dead / gone / essential-down (a heal does not land: fix/mfo-lifestate);
        //   * beyond the heal's REACH (never line of sight: the claim waits charged);
        //   * at or above the rule's own HP threshold, or at full health -- unless a cast
        //     at him is in flight on that hand (D8: it finishes first).
        const char* RecipientLost(RE::Actor* a_follower, RE::Actor* a_recipient, RE::SpellItem* a_spell,
                                  std::size_t a_hand) {
            if (!a_recipient || a_recipient->IsDead() || a_recipient->IsDisabled())
                return "the recipient is dead or gone";
            if (const auto* rst = a_recipient->AsActorState();
                rst && rst->GetLifeState() == RE::ACTOR_LIFE_STATE::kEssentialDown)
                return "the recipient is essential-down (a heal does not land on it)";
            if (a_recipient != a_follower && HealRecipientUnreachable(a_follower, a_recipient, a_spell))
                return "the recipient is beyond the heal's reach";
            const auto fid = a_follower->GetFormID();
            const bool inFlight = a_hand < kHandCount &&
                                  CastInFlightOnHand(a_follower, a_hand, a_spell->GetFormID(),
                                                     CastProxyOnHand(fid, a_hand));
            const float hp = Vocab::HealthPct(a_recipient);
            if (g_firingAllyThreshold >= 0.0f && hp >= std::min(g_firingAllyThreshold, Vocab::kHealFull) &&
                !inFlight)
                return "the recipient is at or above the rule's HP threshold";
            if (hp >= Vocab::kHealFull && !inFlight)
                return "the recipient is at full health";
            return nullptr;
        }

        // THE HELD HEAL THROUGH LOST LINE OF SIGHT, PER HAND. The claim on a_hand stands
        // aimed at a_recipient; when the own-ray verdict says Occluded the heal is being
        // held (charged, waiting) and the engine fires when sight clears. Logged once when
        // it starts and once when sight is back; Unknown changes nothing. Self: no line.
        void NoteLosHold(RE::FormID a_follower, std::size_t a_hand, RE::FormID a_spell, RE::FormID a_recipient) {
            auto& hh = g_healHands[a_follower].hand[a_hand];
            if (a_recipient == 0) { hh.losHeld = false; return; }
            const auto v = Sightline::CheckWithin(a_follower, a_recipient, kHealLosTrustSec, Sightline::Basis::Own);
            const auto now = Clock::now();
            if (v == Sightline::Verdict::Occluded && !hh.losHeld) {
                hh.losHeld  = true;
                hh.losSince = now;
                spdlog::info("[heal-hand] {:08X} {} hand HOLDS heal {:08X} for {:08X} through lost line of "
                             "sight -- the claim stands and is renewed every lap, the cast fires when sight "
                             "clears (rule {})",
                             a_follower, HandWord(a_hand), a_spell, a_recipient, g_firingRule);
            } else if (v == Sightline::Verdict::Visible && hh.losHeld) {
                hh.losHeld = false;
                spdlog::info("[heal-hand] {:08X} {} hand: line of sight to {:08X} is back after {} ms -- the "
                             "held heal {:08X} is free to fire",
                             a_follower, HandWord(a_hand), a_recipient,
                             std::chrono::duration_cast<std::chrono::milliseconds>(now - hh.losSince).count(),
                             a_spell);
            }
        }

        // THE NEVER-OBSERVED BOUND ON A RENEWAL (review F2 on 9895a53, SEV-2). A claim this
        // file renews without the rule re-asking for it (the companion, and the RIGHT hand's
        // in-flight refresh, which has no Prepare repair behind it) must not be renewed
        // forever when it never fires: the SAME window refreshHeldOwnClaim uses
        // (`kHoldLastSeenCapMs` from that hand's lock claim stamp, and no fire observed ON
        // THAT HAND within `kObservedFiringRecencyMs`), with nothing charging or casting on
        // the hand (CastInFlightOnHand: a charged, held cast -- marth's "charge and hold" --
        // is in flight and is kept; an occluded recipient alone is NOT exempt, review R2-3:
        // a claim that is not even charging is bounded). AN UNKNOWN NEVER RELEASES (R2-1): a
        // fire of this form that matched BOTH hands and that no per-hand SpellFire event
        // decided (ComposedCast::UndecidedFire) keeps the bound off, logged once per claim.
        // Past it the claim is RELEASED with a WARN (the
        // twin of HealClaimNeedsRepair's kNeverFired WARN), never silently renewed: the
        // CastClaims.cpp:196-204 note puts that cap at the call site, and a right claim
        // Harbinger can no longer arm (its proxy freed) ends here instead of hanging.
        bool NeverFiredRelease(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_spell, const char* a_who) {
            const auto fid   = a_follower->GetFormID();
            const auto stamp = CastLockClaimStamp(fid, a_hand, a_spell);
            if (stamp == Clock::time_point{}) return false;   // no lock naming it: nothing to bound here
            const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stamp).count();
            if (ageMs < static_cast<long long>(APMFBridge::kHoldLastSeenCapMs)) return false;
            if (ComposedCast::ObservedFiring(fid, ApmfHand(a_hand), a_spell, APMFBridge::kObservedFiringRecencyMs))
                return false;
            if (CastInFlightOnHand(a_follower, a_hand, a_spell, CastProxyOnHand(fid, a_hand))) return false;
            if (ComposedCast::UndecidedFire(fid, ApmfHand(a_hand), a_spell, APMFBridge::kObservedFiringRecencyMs)) {
                static std::unordered_map<std::uint64_t, Clock::time_point> s_noBoundLog;   // worker-serial
                auto& logged = s_noBoundLog[(static_cast<std::uint64_t>(fid) << 1) | a_hand];
                if (logged != stamp) {   // once per claim (its lock stamp)
                    logged = stamp;
                    spdlog::warn("[heal-hand] {:08X} {} hand heal {:08X}: never-fired bound NOT armed -- a fire of "
                                 "this form matched BOTH hands and no per-hand SpellFire event decided it, and an "
                                 "unknown must never release a heal (rule {})",
                                 fid, HandWord(a_hand), a_spell, g_firingRule);
                }
                return false;
            }
            spdlog::warn("[heal-hand] {:08X} {} hand heal {:08X} for {} NEVER FIRED: claimed {} ms ago, no fire "
                         "observed on that hand in the last {} ms and nothing charging -- RELEASED, the hand is "
                         "re-chosen next lap (a seat or proxy problem in Harbinger shows here; rule {})",
                         fid, HandWord(a_hand), a_spell, a_who, ageMs, APMFBridge::kObservedFiringRecencyMs,
                         g_firingRule);
            ReleaseOwnHealClaim(a_follower, a_hand, a_spell, "it never fired within the never-observed window");
            return true;
        }

        // THE CLAIM'S HARBINGER PROXY IS NO LONGER KNOWN TO THE ACTOR (review R2-2 on 57bd5e7).
        // A delivery-flip proxy Harbinger un-taught (a release on the other hand freeing a
        // shared per-owner proxy before APMF's per-claim ref-count, or APMF's PreSaveSweep on
        // save) leaves a claim the engine can never arm, on EITHER hand. The precise test, no
        // timer: proxy != 0 && !actor->HasSpell(proxy), read on the MAIN thread (T1, below).
        // Released for a re-claim (keeping the
        // lock's rank through the re-stream gap, MFO-B177's mechanism) so the next lap's
        // RequestCast has Harbinger mint and teach it again. One WARN per claim (the release
        // ends the claim). Correct with or without APMF's ref-count fix: with it, this simply
        // never fires outside the save sweep.
        // THREADING (review T1 on 0ce73c4): Harbinger teaches and un-teaches proxies
        // (AddSpell / RemoveSpell) on the MAIN thread, so the HasSpell read is made THERE,
        // through MainThread::Post, and its verdict is LATCHED per (follower, hand) under
        // g_proxyMx; the worker acts on the latch on a later lap. At most one read is pending
        // per (follower, hand). A latch belongs to ONE claim: it is keyed on the proxy FormID
        // AND the hand's lock claim stamp (a re-claim re-stamps the lock), so a verdict that
        // lands after a release / re-claim / proxy change is dropped, never applied to the
        // next claim. ResetHealRoad clears it all.
        struct ProxyLatch {
            RE::FormID        proxy    = 0;
            Clock::time_point stamp{};
            bool              pending  = false;
            bool              untaught = false;
            bool              formGone = false;
        };
        std::mutex                                    g_proxyMx;
        std::unordered_map<std::uint64_t, ProxyLatch> g_proxyLatch;   // under g_proxyMx

        bool ProxyUnlearnedRelease(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_spell) {
            const auto fid = a_follower->GetFormID();
            const std::uint64_t key = (static_cast<std::uint64_t>(fid) << 1) | a_hand;
            const RE::FormID proxy = APMFBridge::GetHealCastProxy(fid, ApmfHand(a_hand));
            const auto stamp = CastLockClaimStamp(fid, a_hand, a_spell);
            bool act = false, gone = false;
            {
                std::scoped_lock lk(g_proxyMx);
                if (proxy == 0 || proxy == a_spell || stamp == Clock::time_point{} || !MainThread::IsInstalled()) {
                    g_proxyLatch.erase(key);
                    return false;
                }
                auto& e = g_proxyLatch[key];
                if (e.proxy != proxy || e.stamp != stamp) e = ProxyLatch{ proxy, stamp };   // a new claim / proxy
                if (e.untaught) {
                    act  = true;
                    gone = e.formGone;
                    g_proxyLatch.erase(key);   // consumed: one WARN + one release per claim
                } else if (!e.pending) {
                    e.pending = true;
                    MainThread::Post([fid, key, proxy, stamp]() {
                        auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
                        auto* p     = RE::TESForm::LookupByID<RE::SpellItem>(proxy);
                        const bool untaught = actor && (!p || !actor->HasSpell(p));
                        std::scoped_lock lk2(g_proxyMx);
                        const auto it = g_proxyLatch.find(key);
                        if (it == g_proxyLatch.end() || it->second.proxy != proxy || it->second.stamp != stamp) return;
                        it->second.pending  = false;
                        it->second.untaught = untaught;
                        it->second.formGone = untaught && !p;
                    });
                }
            }
            if (!act) return false;
            spdlog::warn("[heal-hand] {:08X} {} hand heal {:08X}: its Harbinger proxy {:08X} is no longer known to "
                         "the actor{} -- RELEASED for a re-claim, so Harbinger mints and teaches it again (rule {})",
                         fid, HandWord(a_hand), a_spell, proxy, gone ? " (the form is gone)" : "", g_firingRule);
            ReleaseOwnHealClaim(a_follower, a_hand, a_spell,
                                "its Harbinger proxy was un-taught (re-claimed next lap)", /*a_keepSpell=*/true);
            return true;
        }

        // THIS RULE'S HEAL ON THE OTHER HAND (the companion). One rule fires once per lap
        // at one recipient, so a rule healing two recipients would otherwise maintain only
        // the one its picker named this lap and let the other's claim go stale. Re-judged
        // with the same tests as the lap's recipient: lost -> released (a RIGHT hand returns
        // to offense), stream cap -> released for its re-stream, else renewed in place
        // (RefreshOwnedCastOnHand replays the stored claim: TTL heartbeat + `refreshed`, no
        // RequestCast) with its [cfc] watch kept ticking. Only a claim this rule OWNS: a
        // heal another rule holds is that rule's to maintain on its own lap.
        void MaintainCompanion(RE::Actor* a_follower, std::size_t a_hand) {
            const auto fid = a_follower->GetFormID();
            if (g_firingRule == kNoRule) return;
            const auto ah = ApmfHand(a_hand);
            const RE::FormID hs = APMFBridge::GetHealCastSpell(fid, ah);
            if (hs == 0) return;
            const auto lit = g_castLock.find(fid);
            if (lit == g_castLock.end()) return;
            const auto& lk = lit->second.hand[a_hand];
            if (lk.spell != hs || lk.owningRule != g_firingRule) return;
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(hs);
            if (!sp) return;
            const RE::FormID rid = APMFBridge::GetHealCastTarget(fid, ah);
            RE::Actor* rcp = rid == 0 ? a_follower : RE::TESForm::LookupByID<RE::Actor>(rid);
            if (rid != 0 && rcp) Sightline::Want(fid, { rid }, Sightline::Basis::Own);
            // No picker names him this lap, so the picker's own membership tests are made
            // here too (PickAlly / CastAuto: a tracked follower or the player, inside
            // fSharedRadius): a recipient who LEFT the party or the radius is lost.
            const char* gone = nullptr;
            if (rid != 0 && rcp && rcp != RE::PlayerCharacter::GetSingleton() && !Followers::IsTrackedFast(rid))
                gone = "the recipient left the party";
            else if (rid != 0 && rcp &&
                     a_follower->GetPosition().GetDistance(rcp->GetPosition()) > Config::g_sharedRadius.load())
                gone = "the recipient left the shared radius";
            if (const char* lost = gone ? gone : RecipientLost(a_follower, rcp, sp, a_hand)) {
                ReleaseOwnHealClaim(a_follower, a_hand, hs, lost);
                return;
            }
            if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                float capSec = 0.0f;
                if (HealChannelCapped(a_follower, a_hand, hs, capSec)) {
                    ReleaseOwnHealClaim(a_follower, a_hand, hs,
                                        "the concentration heal reached its stream cap "
                                        "(released, re-streams while the rule wins)",
                                        /*a_keepSpell=*/true);
                    return;
                }
            }
            // A WEAPON NOW OWNS THE RIGHT (review F3): the weapon-hand hard rule wins, the
            // right heal is released and its recipient re-picks a hand next lap.
            if (a_hand == kHandRight && RightHandIsWeaponHand(a_follower)) {
                ReleaseOwnHealClaim(a_follower, a_hand, hs, "a weapon now owns the right hand");
                return;
            }
            if (ProxyUnlearnedRelease(a_follower, a_hand, hs)) return;
            NoteLosHold(fid, a_hand, hs, rid);
            if (NeverFiredRelease(a_follower, a_hand, hs, "the other hand's recipient")) return;
            if (APMFBridge::RefreshOwnedCastOnHand(fid, ah, /*a_holdSpell=*/hs))
                ComposedCast::WatchClaim(fid, hs, ah, CastProxyOnHand(fid, a_hand));
        }
    }   // anon

    // ── PER-HAND FIRE EVIDENCE: THE ANIM GRAPH'S SpellFire EVENTS (review R2-1 on 57bd5e7) ──
    // The release step of a cast is graph-gated per hand: MLh_SpellFire_Event (left) /
    // MRh_SpellFire_Event (right) -> StartCastImpl (ENGINE_NOTES, the ALYSLC section). The
    // TESSpellCastEvent names no hand, so these are the per-hand evidence, counted on the
    // EVENT thread (the graph's own dispatch; a mutex, since that is not the worker) and
    // consumed on the worker by ComposedCast::NoteObservedCast (HandFireTake). PASSIVE: the
    // sink only counts; nothing here changes a cast. Attached by the worker (HandFireWatch,
    // from every heal lap) through MainThread::Post, re-posted every 2 s so a rebuilt graph
    // (3D reload) is re-sunk; AddEventSink de-duplicates. The sink is a static singleton, so
    // a graph that dies with its actor never leaves it dangling.
    namespace {
        struct HandFireRec {
            std::uint32_t     seq[kHandCount]   = { 0, 0 };
            std::uint32_t     taken[kHandCount] = { 0, 0 };
            Clock::time_point last[kHandCount]{};
            bool              any = false;   // a SpellFire event was ever seen for this actor
        };
        std::mutex                                   g_handFireMx;
        std::unordered_map<RE::FormID, HandFireRec>  g_handFire;            // under g_handFireMx
        std::unordered_map<RE::FormID, Clock::time_point> g_handFireAttach;  // worker-serial throttle

        class HandFireSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_ev,
                                                  RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override {
                if (!a_ev || !a_ev->holder) return RE::BSEventNotifyControl::kContinue;
                const char* tag = a_ev->tag.c_str();
                if (!tag) return RE::BSEventNotifyControl::kContinue;
                std::size_t h = kHandCount;
                if (_stricmp(tag, "MLh_SpellFire_Event") == 0)      h = kHandLeft;
                else if (_stricmp(tag, "MRh_SpellFire_Event") == 0) h = kHandRight;
                if (h == kHandCount) return RE::BSEventNotifyControl::kContinue;
                std::scoped_lock lk(g_handFireMx);
                auto& r = g_handFire[a_ev->holder->GetFormID()];
                ++r.seq[h];
                r.last[h] = Clock::now();
                r.any     = true;
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        HandFireSink g_handFireSink;
    }

    void HandFireWatch(RE::Actor* a_follower) {
        if (!a_follower || !MainThread::IsInstalled()) return;
        const auto fid = a_follower->GetFormID();
        const auto now = Clock::now();
        auto& last = g_handFireAttach[fid];
        if (last != Clock::time_point{} && now - last < std::chrono::seconds(2)) return;
        last = now;
        MainThread::Post([fid]() {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!actor) return;
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
            if (!actor->GetAnimationGraphManager(mgr) || !mgr) return;
            for (auto& g : mgr->graphs) {
                if (!g) continue;
                static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(g.get())->AddEventSink(&g_handFireSink);
            }
        });
    }

    bool HandFireTake(RE::FormID a_follower, std::uint32_t a_windowMs, bool& a_left, bool& a_right) {
        a_left = a_right = false;
        std::scoped_lock lk(g_handFireMx);
        const auto it = g_handFire.find(a_follower);
        if (it == g_handFire.end()) return false;
        auto& r = it->second;
        const auto now = Clock::now();
        bool* out[kHandCount] = { &a_left, &a_right };
        for (std::size_t h = 0; h < kHandCount; ++h) {
            *out[h] = r.seq[h] != r.taken[h] && now - r.last[h] <= std::chrono::milliseconds(a_windowMs);
            r.taken[h] = r.seq[h];   // consumed: one SpellFire event answers one cast event
        }
        return r.any;
    }

    void HealHandEnded(RE::FormID a_follower, std::size_t a_hand, const char* a_why) {
        const auto it = g_healHands.find(a_follower);
        if (it == g_healHands.end() || a_hand >= kHandCount) return;
        auto& hh = it->second.hand[a_hand];
        if (hh.active) {
            const std::string held = hh.losHeld
                ? std::format("; it was held through lost line of sight for {} ms",
                              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - hh.losSince).count())
                : std::string{};
            if (a_hand == kHandRight)
                spdlog::info("[heal-hand] {:08X} RIGHT hand returns to offense -- the heal {:08X} for {:08X} "
                             "ended: {} (rule {}){}",
                             a_follower, hh.spell, Who(a_follower, hh.recipient), a_why, hh.rule, held);
            else
                spdlog::info("[heal-hand] {:08X} LEFT hand heal {:08X} for {:08X} ended: {} (rule {}){}",
                             a_follower, hh.spell, Who(a_follower, hh.recipient), a_why, hh.rule, held);
        }
        hh = HealHand{};
        if (!it->second.hand[0].active && !it->second.hand[1].active &&
            it->second.hand[0].displacing == 0 && it->second.hand[1].displacing == 0)
            g_healHands.erase(it);
    }

    void HealHandsReconcile(RE::FormID a_follower) {
        const auto it = g_healHands.find(a_follower);
        if (it == g_healHands.end()) return;
        for (std::size_t h = 0; h < kHandCount; ++h) {
            if (!it->second.hand[h].active) continue;
            if (HealOnHand(a_follower, h).found) continue;
            HealHandEnded(a_follower, h, "its claim was released elsewhere (the [heal] / [eval] line above "
                                         "names why: expiry sweep, a higher-ranked rule, combat end)");
            if (g_healHands.find(a_follower) == g_healHands.end()) return;   // erased with the last record
        }
    }

    void HealHandClaimed(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_spell, RE::FormID a_recipient) {
        const auto fid = a_follower ? a_follower->GetFormID() : 0;
        if (fid == 0 || a_hand >= kHandCount) return;
        auto& slots = g_healHands[fid];
        auto& hh    = slots.hand[a_hand];
        const RE::FormID displaced     = hh.displacing;
        const int        displacedRule = hh.displacingRule;
        hh.displacing     = 0;
        hh.displacingRule = kNoRule;
        if (hh.active && hh.spell == a_spell && hh.recipient == a_recipient && hh.rule == g_firingRule) return;
        std::string extra;
        if (a_hand == kHandRight) {
            const auto& left = slots.hand[kHandLeft];
            extra = left.active ? std::format(" -- SECOND recipient: the left hand heals {:08X}",
                                              Who(fid, left.recipient))
                                : std::string(" -- the left hand's heal is not standing");
            if (displaced != 0)
                extra += std::format("; the right hand held offense spell {:08X} (rule {}) before", displaced,
                                     displacedRule);
        }
        spdlog::info("[heal-hand] {:08X} {} hand -> heal {:08X} for {:08X} (rule {}){}",
                     fid, HandWord(a_hand), a_spell, Who(fid, a_recipient), g_firingRule, extra);
        const bool losHeld = hh.active && hh.recipient == a_recipient && hh.losHeld;
        const auto losSince = hh.losSince;
        hh = HealHand{};
        hh.active    = true;
        hh.spell     = a_spell;
        hh.recipient = a_recipient;
        hh.rule      = g_firingRule;
        hh.losHeld   = losHeld;   // the same recipient keeps its held-through-LoS state
        hh.losSince  = losSince;
    }

    std::optional<Outcome> HealRoadLap(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
                                       std::size_t& a_hand) {
        a_hand = kHandLeft;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        RE::Actor* recipient = (a_target == a_follower) ? a_follower : a_target;
        const bool atSelf    = recipient == a_follower;
        const RE::FormID recipKey = atSelf ? 0 : recipient->GetFormID();
        if (!atSelf) Sightline::Want(id, { recipient->GetFormID() }, Sightline::Basis::Own);

        HealHandsReconcile(id);
        HandFireWatch(a_follower);   // the per-hand SpellFire evidence for this follower (R2-1)

        // WHICH HAND ALREADY SERVES THIS RECIPIENT? With this spell (the incumbent: its
        // claim or its re-stream gap), or with another spell (a different rule's heal on
        // him: that hand is contested as before, the recipient never takes a second hand).
        std::size_t servedOn = kHandCount, contestedOn = kHandCount;
        HandHeal onHand[kHandCount];
        for (std::size_t h = 0; h < kHandCount; ++h) {
            onHand[h] = HealOnHand(id, h);
            if (!onHand[h].found || onHand[h].recipient != recipKey) continue;
            if (onHand[h].spell == spellID) servedOn = h;
            else if (contestedOn == kHandCount) contestedOn = h;
        }

        // THIS RULE'S HEAL ON THE OTHER HAND(S): re-judged and maintained every lap.
        for (std::size_t h = 0; h < kHandCount; ++h)
            if (h != servedOn) MaintainCompanion(a_follower, h);
        // ...which may have released one: read both hands again, so a hand freed just now
        // is taken as free (the LEFT first) instead of displacing the right's offense.
        for (std::size_t h = 0; h < kHandCount; ++h)
            if (h != servedOn) onHand[h] = HealOnHand(id, h);

        // THIS LAP'S RECIPIENT, RE-JUDGED (before the hand lock and before the in-flight
        // refresh). Lost -> a claim (or re-stream gap lock) of this rule's aimed at HIM is
        // released now, even mid-cast; transparent either way, the rules below run. Not a
        // fallback: nothing else casts this heal this lap.
        if (const char* lost = RecipientLost(a_follower, recipient, a_spell, servedOn)) {
            if (servedOn < kHandCount) ReleaseOwnHealClaim(a_follower, servedOn, spellID, lost);
            return Outcome{ Result::FailedOther, std::format("animated heal not cast: {}", lost), true };
        }

        // THE HAND. Served -> that hand. Contested by another spell on him -> that hand
        // (the old contest, by rank). Otherwise the LEFT, unless the left already heals a
        // DIFFERENT recipient: then this second recipient takes the RIGHT, when the right
        // holds no heal of its own and no weapon owns it. Both hands healing others -> the
        // LEFT, which re-aims only in its idle gap (MFO-B171), as before.
        if (servedOn == kHandRight && onHand[kHandRight].claim && RightHandIsWeaponHand(a_follower)) {
            // A WEAPON NOW OWNS THE RIGHT (review F3): release; the recipient re-picks next lap.
            ReleaseOwnHealClaim(a_follower, kHandRight, spellID, "a weapon now owns the right hand");
            return Outcome{ Result::FailedOther, "animated heal not cast: a weapon now owns the right hand", true };
        }
        if (servedOn < kHandCount) {
            a_hand = servedOn;
        } else if (contestedOn < kHandCount) {
            a_hand = contestedOn;
        } else if (onHand[kHandLeft].found && !onHand[kHandRight].found &&
                   !RightHandIsWeaponHand(a_follower)) {
            a_hand = kHandRight;
            auto& hh = g_healHands[id].hand[kHandRight];
            hh.displacing = 0;
            hh.displacingRule = kNoRule;
            if (const auto lit = g_castLock.find(id); lit != g_castLock.end() &&
                                                      lit->second.hand[kHandRight].spell != 0) {
                hh.displacing     = lit->second.hand[kHandRight].spell;
                hh.displacingRule = lit->second.hand[kHandRight].owningRule;
            }
        }

        // GAMBIT ORDER ON THE LEFT HAND (review round 2, marth 2026-09-30: "Gambit order
        // wins, so in most cases it's heal. But a poorly ordered gambit board shouldn't be
        // rescued programmatically."). An equip gambit whose hold occupies the left hand
        // and whose rule OUTRANKS this one keeps the hand: the heal does not claim, and a
        // claim of this rule's still standing there is released. Rank is the carried rule
        // index (ForcedHold::rule vs g_firingRule), no tenure, no rescue. The right hand is
        // never a weapon hand here (RightHandIsWeaponHand above), so it has no such gate.
        if (a_hand == kHandLeft) {
            if (const int holdRule = LeftHoldRule(id); holdRule < g_firingRule) {
                ReleaseOwnHealClaim(a_follower, kHandLeft, spellID,
                                    "an equip gambit ranked above this rule holds the left hand");
                const auto now = Clock::now();
                auto& last = g_healOutrankLog[id];
                if (now - last >= std::chrono::seconds(5)) {
                    last = now;
                    spdlog::info("[heal] {:08X} animated heal (spell {:08X}, rule {}) NOT claimed -- equip "
                                 "rule {} holds the left hand and ranks above it (gambit order)",
                                 id, spellID, g_firingRule, holdRule);
                }
                return Outcome{ Result::FailedOther,
                                "animated heal not cast: an equip gambit ranked above holds the left hand", true };
            }
        }

        // [heal-obs]'s "HP before" for a claim fire (HealObsClaimLap).
        HealObsClaimLap(id, recipKey, Vocab::HealthPct(recipient));
        // The held-through-lost-LoS state of the hand already serving him.
        if (servedOn < kHandCount && onHand[servedOn].claim) NoteLosHold(id, servedOn, spellID, recipKey);
        return std::nullopt;
    }

    std::optional<Outcome> HealRefreshGate(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_spell) {
        if (!a_follower || a_hand >= kHandCount) return std::nullopt;
        if (ProxyUnlearnedRelease(a_follower, a_hand, a_spell))
            return Outcome{ Result::FailedOther, "animated heal released: its Harbinger proxy was un-taught", true };
        // The never-observed bound: the RIGHT hand only here (the left lap has HealClaimNeedsRepair).
        if (a_hand == kHandRight && NeverFiredRelease(a_follower, kHandRight, a_spell, "its recipient"))
            return Outcome{ Result::FailedOther, "animated heal released: it never fired on the right hand", true };
        return std::nullopt;
    }

    int RightHealRule(RE::FormID a_follower) {
        // A live claim, OR the lock that keeps the hand through a re-stream / re-claim gap
        // (review R2-4): HealOnHand reads both, so the rank holds across the gap.
        const HandHeal hh = HealOnHand(a_follower, kHandRight);
        if (!hh.found) return kNoRule;
        const auto it = g_castLock.find(a_follower);
        if (it == g_castLock.end()) return kNoRule;
        const auto& lk = it->second.hand[kHandRight];
        return lk.spell == hh.spell ? lk.owningRule : kNoRule;
    }

    void ReleaseHealClaimsAllHands(RE::FormID a_follower, const char* a_why) {
        for (std::size_t h = 0; h < kHandCount; ++h) {
            const auto ah = ApmfHand(h);
            const RE::FormID spell = APMFBridge::GetHealCastSpell(a_follower, ah);
            if (spell == 0) continue;
            ComposedCast::EndHand(a_follower, ah);
            if (auto it = g_castLock.find(a_follower); it != g_castLock.end() && it->second.hand[h].spell == spell)
                it->second.hand[h] = CastLock{};
            HealHandEnded(a_follower, h, a_why);
        }
    }

    void ResetHealRoad() {
        g_healHands.clear();
        g_handFireAttach.clear();
        {
            std::scoped_lock lk(g_proxyMx);
            g_proxyLatch.clear();   // review T1: every pending verdict is dropped with it
        }
        std::scoped_lock lk(g_handFireMx);
        g_handFire.clear();
    }

}
