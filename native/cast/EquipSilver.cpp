// cast/EquipSilver.cpp -- act.equip_silver, "switch to silver" (ClickUp 86e3p2gp9, marth
// 2026-10-12), and the switch BACK to the main weapon.
//
// THE RULING. In combat, while the rule's condition holds (normally "Foe: Unholy"), the follower
// puts his best owned silver melee weapon (Logistics::BestSilverWeapon -- the tracked slot,
// logistics/Silver.cpp) in the MAIN weapon's hand(s), replacing the main weapon; the off-hand is
// untouched. When the condition is no longer true, the main weapon comes back.
//
// ROAD (mirrors EquipWeapon, cast/Equip.cpp, on purpose): the SAME T#76 force-hold ledger
// (g_forcedWeapon .right) and, with Harbinger's equip authority live, the SAME ch.17 declaration
// from that ledger (DeclareLedger); without it the same forced EquipObject. The ch.15 equipment
// claim, the hold's keep / release on the general lifecycle (ReconcileForcedWeapon, the
// stand-down at combat end, death, dismissal, revert, the FWPN co-save) all apply to it unchanged
// because it IS a ledger hold. For the Scheduler it is a melee equip (category 1): the hand claim,
// the melee stance and the equip gate hold exactly as for act.equip_melee.
//
// OFF-HAND UNTOUCHED: with something in the left hand (a shield, a second weapon, a spell, a
// torch) and a one-handed (or no) main weapon, only a ONE-HANDED silver weapon qualifies; a
// two-handed silver weapon is used only when the main weapon already takes both hands or the
// left hand is empty. The ledger's .left is never written here.
//
// THE EPISODE. The first lap that switches remembers what it replaced: the main right-hand weapon
// and whether that was itself an MFO hold (an equip gambit's, with its rank). It is kept until the
// switch back, so a silver weapon a heal claim yielded and the rule put back still knows the main
// weapon. In memory only (a session starts hold-free, as the ledger does): NOTHING IS SAVED.
//
// THE SWITCH BACK (ReconcileSilver, called by the Scheduler just before ReconcileForcedWeapon).
// The silver rule's OWN truth (Scheduler: silverHeld / silverKnownFalse, the positional rule the
// equip lifecycle uses), with its own 2 s dwell (principle 9: a floor so a selector that drops for
// a lap does not thrash the hand). Then, if the ledger's right hand is still the silver weapon:
//   * main was an MFO hold -> the hold is RESTORED on the main weapon (ledger .right = main, its
//     rank back) and placed the way EquipWeapon places one (declared / forced equip);
//   * otherwise -> the silver hold ends (ledger .right cleared, the ch.15 claim released when the
//     entry empties, re-declared so the hand is unowned) and the main weapon is re-armed NON-forced
//     two frames later on the main thread (the stand-down re-arm's shape, #62).
// Any other release of a silver hold (combat end, kill switch, death, dismissal) goes through
// ReleaseForcedWeapon, which asks SilverHoldEnded and re-arms the main weapon instead of the
// silver one.
//
// THREADING: Fire / the Scheduler's reconcile run on the job worker (serial). g_silver has its own
// small lock (never held across an engine call or together with g_forcedMx) because the revert
// clear and ReleaseForcedWeapon reach it from the ledger's lifecycle.
#include "Actuation_internal.h"
#include "apmf/APMFBridge.h"   // the equip authority + ch.15 claim, exactly as EquipWeapon uses them
#include <chrono>
#include <mutex>

namespace MFO::Actuation {

    namespace {

        struct Episode {
            RE::FormID silver    = 0;         // the silver weapon the gambit put in the main hand
            RE::FormID mainRight = 0;         // the main weapon it replaced (0 = none: a spell / empty)
            bool       mainHeld  = false;     // ... was itself an MFO force-hold (an equip gambit's)
            int        mainRule  = kNoRule;   // ... and that hold's rank
            bool       direct    = false;     // bWeaponStyleControl off: plain equip, no ledger
            std::chrono::steady_clock::time_point trueAt{};   // last lap the rule held (or was unknown)
        };
        std::mutex                              g_silverMx;
        std::unordered_map<RE::FormID, Episode> g_silver;
        constexpr auto kSilverDwell = std::chrono::seconds(2);

        // Two-hander / bow / crossbow: takes the left hand too (Equip.cpp's TakesBothHands twin).
        bool TakesBoth(const RE::TESForm* a_form) {
            const auto* w = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
            return w && (w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow());
        }

        const char* NameOf(const RE::TESForm* a_form) {
            const char* n = a_form ? a_form->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        RE::TESBoundObject* OwnedObject(RE::Actor* a_actor, RE::FormID a_form) {
            if (!a_actor || a_form == 0) return nullptr;
            auto* form = RE::TESForm::LookupByID(a_form);
            auto* obj  = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!obj) return nullptr;
            auto inv = a_actor->GetInventory();
            const auto it = inv.find(obj);
            return (it != inv.end() && it->second.first > 0) ? obj : nullptr;
        }

        // Non-forced re-arm of the main weapon, two main-thread hops after the silver unequip
        // (the stand-down re-arm's shape: the unequip is queued; #62: equips belong on main).
        // Re-resolved on the frame that runs; never evicts what something already put there.
        void PostMainReArm(RE::FormID a_id, RE::FormID a_main) {
            if (a_main == 0 || !MainThread::IsInstalled()) return;
            auto reArm = [a_id, a_main]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
                auto* mgr   = RE::ActorEquipManager::GetSingleton();
                if (!actor || !mgr || actor->IsDead() || actor->IsDisabled()) return;
                auto* obj = OwnedObject(actor, a_main);
                if (!obj || actor->GetEquippedObject(false)) return;
                mgr->EquipObject(actor, obj, nullptr, 1, nullptr, true, false);   // forceEquip = FALSE
                spdlog::info("[silver] {:08X}: main weapon '{}' back in hand (non-forced; the AI owns it)",
                             a_id, NameOf(obj));
            };
            MainThread::Post([reArm]() { MainThread::Post(reArm); });
        }
    }

    Outcome EquipSilver(RE::Actor* a_follower) {
        if (!a_follower) return { Result::FailedOther, "no follower", true };
        const auto id = a_follower->GetFormID();
        RE::TESForm* const heldRight = a_follower->GetEquippedObject(false);
        RE::TESForm* const heldLeft  = a_follower->GetEquippedObject(true);
        auto* rightW = heldRight ? heldRight->As<RE::TESObjectWEAP>() : nullptr;

        // SATISFIED: a silver melee weapon is already in the main hand. Transparent, like
        // EquipWeapon's "already holding" (the Scheduler reads it as the melee hand claim), and
        // the rank stamp when the ledger holds it.
        if (rightW && Logistics::IsSilverMeleeWeapon(rightW)) {
            std::scoped_lock lk(g_forcedMx);
            if (auto h = g_forcedWeapon.find(id); h != g_forcedWeapon.end() && h->second.right == rightW)
                h->second.rule = g_firingRule;
            return { Result::NoOp, "already holding silver", true };
        }

        // THE PICK: the tracked slot, one-handed only when the off-hand is in use and the main
        // weapon is not already two-handed (the off-hand stays untouched).
        const FollowerState* state = nullptr;
        if (auto it = g_followers.find(id); it != g_followers.end()) state = &it->second;   // worker-serial (#4)
        const bool leftBusy    = heldLeft && heldLeft != heldRight;   // a two-hander reads in both hands
        const bool oneHandOnly = leftBusy && !TakesBoth(heldRight);
        RE::TESObjectWEAP* best = Logistics::BestSilverWeapon(a_follower, state, oneHandOnly);
        if (!best)
            return { Result::FailedSkill, oneHandOnly ? "no one-handed silver weapon carried (the off-hand is in use)"
                                                      : "no silver weapon carried",
                     true };   // transparent -- cannot act, rules below run

        // GAMBIT ORDER vs a heal claim, exactly EquipWeapon's two gates.
        if (TakesBoth(best) && HealClaimTakesLeftFrom(a_follower, g_firingRule))
            return { Result::NoOp, "held off: a higher-ranked heal claim holds the left hand", true };
        if (const int healRule = RightHealRule(id); healRule < g_firingRule)
            return { Result::NoOp, "held off: a higher-ranked heal claim holds the right hand", true };

        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) return { Result::FailedOther, "no equip manager", true };

        // THE AUTHORITY, before anything is touched (EquipWeapon's NO DECLINE-FALLBACK rule):
        // supported but refused -> MFO stays out, nothing unequipped, no episode.
        const bool styleControl = Config::g_weaponStyleControl.load();
        const bool authority = styleControl && APMFBridge::EquipAuthoritySupported();
        if (authority && !APMFBridge::ClaimEquipAuthority(id))
            return { Result::NoOp, "equip authority refused (MFO leaves the hands to the engine)", true };

        // THE EPISODE: remember the main weapon on the first switch only.
        RE::TESBoundObject* oldForced = nullptr;
        bool mainWasHeld = false; int mainRule = kNoRule;
        {
            std::scoped_lock lk(g_forcedMx);
            if (auto h = g_forcedWeapon.find(id); h != g_forcedWeapon.end() && h->second.right) {
                if (h->second.right != best) oldForced = h->second.right;
                mainWasHeld = rightW && h->second.right == rightW;
                mainRule    = h->second.rule;
            }
        }
        RE::FormID mainId = 0;
        {
            std::scoped_lock lk(g_silverMx);
            auto [it, fresh] = g_silver.try_emplace(id);
            if (fresh) {
                if (rightW && rightW->GetWeaponType() != RE::WEAPON_TYPE::kHandToHandMelee) {
                    it->second.mainRight = rightW->GetFormID();
                    it->second.mainHeld  = mainWasHeld;
                    it->second.mainRule  = mainWasHeld ? mainRule : kNoRule;
                }
            }
            it->second.silver = best->GetFormID();
            it->second.direct = !styleControl;
            it->second.trueAt = std::chrono::steady_clock::now();
            mainId = it->second.mainRight;
        }

        if (!styleControl) {
            mgr->EquipObject(a_follower, best);   // kill switch off: EquipWeapon's plain equip, no ledger
        } else {
            // The old right-hand hold leaves first (forced unequip clears its lock; unequips are
            // never seated), then the silver weapon is held in the right hand of the ledger.
            if (oldForced) mgr->UnequipObject(a_follower, oldForced, nullptr, 1, nullptr, true, true);
            if (!authority) mgr->EquipObject(a_follower, best, nullptr, 1, nullptr, true, true);   // T#76 force-hold
            {
                std::scoped_lock lk(g_forcedMx);
                auto& hold = g_forcedWeapon[id];
                hold.right = best;
                hold.rule  = g_firingRule;
            }
            if (authority) DeclareLedger(a_follower, "gambit equip silver");
        }
        a_follower->DrawWeaponMagicHands(true);
        auto* mainForm = mainId ? RE::TESForm::LookupByID(mainId) : nullptr;
        spdlog::info("[silver] {:08X}: GAMBIT switch to silver '{}' dmg={} (replaces main '{}'{}){}{}", id,
                     NameOf(best), best->GetAttackDamage(), mainForm ? NameOf(mainForm) : "none",
                     mainWasHeld ? ", an equip-gambit hold" : "", oneHandOnly ? " [one-handed: off-hand in use]" : "",
                     !styleControl ? " [plain equip: bWeaponStyleControl off]"
                                   : authority ? " [declared: APMF equip authority]" : "");
        return { Result::Fired, "switched to silver" };
    }

    void ReconcileSilver(RE::Actor* a_follower, bool a_held, bool a_knownFalse) {
        if (!a_follower) return;
        const auto id  = a_follower->GetFormID();
        const auto now = std::chrono::steady_clock::now();
        Episode ep;
        {
            std::scoped_lock lk(g_silverMx);
            auto it = g_silver.find(id);
            if (it == g_silver.end()) return;
            // Held, or truth UNKNOWN (a higher rule stopped the scan above it): the dwell clock
            // measures SUSTAINED known-false only (the T#76 dwell's rule).
            if (a_held || !a_knownFalse) { it->second.trueAt = now; return; }
            if (now - it->second.trueAt < kSilverDwell) return;
            ep = it->second;
            g_silver.erase(it);
        }

        auto* mgr     = RE::ActorEquipManager::GetSingleton();
        auto* mainObj = OwnedObject(a_follower, ep.mainRight);   // still his? (sold / handed back -> nothing to restore)

        if (ep.direct) {
            // bWeaponStyleControl off: plain swap back, only while the silver weapon is still in hand.
            auto* held = a_follower->GetEquippedObject(false);
            if (held && held->GetFormID() == ep.silver && mainObj && mgr && MainThread::IsInstalled()) {
                const RE::FormID mainId = ep.mainRight;
                MainThread::Post([id, mainId]() {
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                    auto* m     = RE::ActorEquipManager::GetSingleton();
                    auto* obj   = actor ? OwnedObject(actor, mainId) : nullptr;
                    if (actor && m && obj && !actor->IsDead()) m->EquipObject(actor, obj);
                });
            }
            spdlog::info("[silver] {:08X}: no longer Unholy -- main weapon '{}' back (plain equip)", id,
                         mainObj ? NameOf(mainObj) : "none");
            return;
        }

        RE::TESBoundObject* silverObj = nullptr;
        bool restoreHold = false, entryGone = false;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(id);
            if (it != g_forcedWeapon.end() && it->second.right && it->second.right->GetFormID() == ep.silver) {
                silverObj = it->second.right;
                if (ep.mainHeld && mainObj) {
                    it->second.right = mainObj;
                    it->second.rule  = ep.mainRule;
                    restoreHold = true;
                } else {
                    it->second.right = nullptr;
                    if (it->second.Empty()) { g_forcedWeapon.erase(it); entryGone = true; }
                }
            }
        }
        if (!silverObj) {
            // Another equip took the hand since (a category flip, a release): it owns it now.
            spdlog::info("[silver] {:08X}: no longer Unholy -- the silver hold is already gone, nothing to switch back", id);
            return;
        }
        const bool authority = APMFBridge::EquipAuthoritySupported();
        if (mgr) mgr->UnequipObject(a_follower, silverObj, nullptr, 1, nullptr, true, true);   // clears the lock
        if (restoreHold) {
            if (authority) DeclareLedger(a_follower, "silver ended: main weapon hold restored");
            else if (mgr)  mgr->EquipObject(a_follower, mainObj, nullptr, 1, nullptr, true, true);   // T#76 force-hold
        } else {
            if (entryGone) APMFBridge::ReleaseEquipment(id);   // the hands are free: the ch.15 claim goes too
            if (authority) DeclareLedger(a_follower, "silver ended");   // the right hand is unowned again
            PostMainReArm(id, mainObj ? ep.mainRight : 0);
        }
        spdlog::info("[silver] {:08X}: no longer Unholy -- '{}' out, main weapon '{}' back{}", id, NameOf(silverObj),
                     mainObj ? NameOf(mainObj) : "none (the AI re-arms)",
                     restoreHold ? " as the equip gambit's hold" : mainObj ? " non-forced" : "");
    }

    RE::FormID SilverHoldEnded(RE::FormID a_follower, const RE::TESBoundObject* a_heldRight) {
        std::scoped_lock lk(g_silverMx);
        auto it = g_silver.find(a_follower);
        if (it == g_silver.end()) return 0;
        const Episode ep = it->second;
        g_silver.erase(it);
        if (ep.direct || !a_heldRight || a_heldRight->GetFormID() != ep.silver) return 0;
        return ep.mainRight;
    }

    void ClearSilverEpisodes() {
        std::scoped_lock lk(g_silverMx);
        g_silver.clear();
    }

}
