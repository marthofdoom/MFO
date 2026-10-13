// cast/EquipSilver.cpp -- act.equip_silver, "switch to silver" (ClickUp 86e3p2gp9, marth
// 2026-10-12), and the switch BACK to the main weapon.
//
// THE RULING. In combat, while the rule's condition holds (normally "Foe targeted: Unholy"), the follower
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
// RANGED FOLLOWERS USE SILVER AMMO (marth 2026-10-12: "for bows/crossbows it's silver ammo"). When the
// main weapon in his hand is a bow or crossbow at the moment the gambit acts, nothing is swapped in
// the weapon hand: the gambit nocks his best owned silver ammo of the launcher's kind (Logistics::
// BestSilverAmmo: a bow takes arrows only, a crossbow bolts only) with a plain equip (no force-hold,
// so no lock to leak), and the previous ammo is put back when the foe is no longer Unholy. The ammo
// episode (Episode::ammo) is its OWN truth: the Scheduler reads act.equip_silver as a MELEE equip
// (category 1 -- melee stance, melee hand claim) and only a Fired / satisfied outcome feeds that, so the
// ammo road always returns a TRANSPARENT FailedSkill (the scan falls past it, an archer never gets the
// melee stance) and ReconcileSilver ignores the Scheduler's silver truth for an ammo episode: it
// restores after kSilverDwell once he holds no launcher, is out of combat, or his target is not Unholy
// (the same target gate). SilverHoldEnded (every release: combat end, death, dismissal) restores too.
// UNDER THE APMF EQUIP AUTHORITY with a launcher hold in the ledger, Ammo is an OWNED category whose
// declaration is built in logistics/EquipAuthority.cpp (rule 2): it reads the open episode
// (SilverAmmoEpisode) and names the silver ammo, so the road opens the episode and re-declares
// (DeclareLedger) instead of a direct equip; the restore ends the episode and re-declares.
//
// THREADING: Fire / the Scheduler's reconcile run on the job worker (serial). g_silver has its own
// small lock (never held across an engine call or together with g_forcedMx) because the revert
// clear and ReleaseForcedWeapon reach it from the ledger's lifecycle.
#include "Actuation_internal.h"
#include "apmf/APMFBridge.h"   // the equip authority + ch.15 claim, exactly as EquipWeapon uses them
#include "CombatSense.h"      // FoeCount: the target gate's "foes remain" (MFO-B55 unknown)
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
            bool       ammo      = false;     // a SILVER AMMO episode (silver = the ammo; ammoPrev = the ammo it replaced)
            RE::FormID ammoPrev  = 0;
            std::chrono::steady_clock::time_point lastNock{};   // the last time the ammo road nocked the silver ammo
            bool       aiOverrideLogged = false;                // the engine AI re-chose its own ammo: said once per episode
            std::chrono::steady_clock::time_point trueAt{};   // last lap the rule held (or was unknown)
        };
        std::mutex                              g_silverMx;
        std::unordered_map<RE::FormID, Episode> g_silver;
        constexpr auto kSilverDwell = std::chrono::seconds(2);
        // FLOOR between re-nocks inside ONE ammo episode (the engine AI may keep re-choosing its own ammo):
        // a few seconds, so the road cannot re-equip silver every lap (principle 9: a floor, from the lap cadence).
        constexpr auto kRenockFloor = std::chrono::seconds(4);

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

    namespace {

        bool IsLauncher(const RE::TESForm* a_form) {
            const auto* w = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
            return w && (w->IsBow() || w->IsCrossbow());
        }

        // Ammo is an owned APMF category while the equip authority is supported and his ledger holds a launcher.
        bool AmmoUnderAuthority(RE::FormID a_id) {
            if (!Config::g_weaponStyleControl.load() || !APMFBridge::EquipAuthoritySupported()) return false;
            std::scoped_lock lk(g_forcedMx);
            const auto h = g_forcedWeapon.find(a_id);
            return h != g_forcedWeapon.end() && IsLauncher(h->second.right);
        }

        // The ammo he has nocked right now (the inventory entry that is worn), and its stack count.
        RE::TESAmmo* WornAmmo(RE::Actor* a_actor, std::int32_t& a_outCount) {
            a_outCount = 0;
            if (!a_actor) return nullptr;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* am = obj->As<RE::TESAmmo>();
                auto* entry = data.second.get();
                if (am && entry && entry->IsWorn()) { a_outCount = static_cast<std::int32_t>(data.first); return am; }
            }
            return nullptr;
        }

        // Put the previous ammo back (or, with none to put back, take the silver off and let the AI nock
        // its own), on the main thread. Only while the silver ammo is still the worn one: anything else
        // that changed the ammo since owns it now.
        void RestoreSilverAmmo(RE::FormID a_id, const Episode& a_ep, const char* a_why) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!actor || actor->IsDead() || actor->IsDisabled()) return;
            std::int32_t wornCount = 0;
            auto* worn = WornAmmo(actor, wornCount);
            if (!worn || worn->GetFormID() != a_ep.silver) {
                spdlog::info("[silver] {:08X}: {} -- the ammo changed since the silver was nocked, nothing to put back", a_id, a_why);
                return;
            }
            auto* prev = OwnedObject(actor, a_ep.ammoPrev);
            spdlog::info("[silver] {:08X}: {} -- silver ammo '{}' out, previous ammo '{}' back", a_id, a_why,
                         NameOf(worn), prev ? NameOf(prev) : "none (the AI nocks its own)");
            // Episode already erased: under the authority the declaration reverts to the ordinary ammo and APMF equips it.
            if (AmmoUnderAuthority(a_id)) { DeclareLedger(actor, "silver ammo ended"); return; }
            const RE::FormID silverId = a_ep.silver, prevId = prev ? a_ep.ammoPrev : 0;
            auto act = [a_id, silverId, prevId]() {
                auto* a   = RE::TESForm::LookupByID<RE::Actor>(a_id);
                auto* mgr = RE::ActorEquipManager::GetSingleton();
                if (!a || !mgr || a->IsDead() || a->IsDisabled()) return;
                if (prevId != 0) {
                    auto* p = OwnedObject(a, prevId);
                    if (!p) return;
                    std::int32_t n = 1;
                    auto inv = a->GetInventory();
                    if (const auto it = inv.find(p); it != inv.end() && it->second.first > 0) n = it->second.first;
                    mgr->EquipObject(a, p, nullptr, static_cast<std::uint32_t>(n), nullptr, true, false);   // forceEquip = FALSE
                } else if (auto* sv = OwnedObject(a, silverId)) {
                    mgr->UnequipObject(a, sv, nullptr, 1, nullptr, true, false);
                }
            };
            if (MainThread::IsInstalled()) MainThread::Post(act); else act();
        }

        // THE AMMO EPISODE'S OWN TRUTH (the Scheduler's silver truth is a MELEE equip's and is ignored for
        // an ammo episode): a launcher in the weapon hand, in a fight, and his target still Unholy -- or no
        // usable target while his group still has foes (UNKNOWN, kept: the MFO-B55 rule).
        bool AmmoEpisodeHolds(RE::Actor* a_follower) {
            if (!IsLauncher(a_follower->GetEquippedObject(false))) return false;
            const bool foes = CombatSense::FoeCount(a_follower) > 0;
            if (!a_follower->IsInCombat() && !foes) return false;
            auto  tp  = a_follower->GetActorRuntimeData().currentCombatTarget.get();
            auto* tgt = tp.get();
            const bool usable = tgt && !tgt->IsDead() && !tgt->IsDisabled() && tgt->IsHostileToActor(a_follower);
            return usable ? Logistics::IsUnholy(tgt) : foes;
        }

        // The ranged road of act.equip_silver. ALWAYS a transparent non-NoOp outcome (see the file header).
        Outcome EquipSilverAmmo(RE::Actor* a_follower, const RE::TESObjectWEAP* a_launcher) {
            const auto id       = a_follower->GetFormID();
            const bool wantBolt = a_launcher->IsCrossbow();
            const char* kind    = wantBolt ? "bolts" : "arrows";
            bool open = false, openAmmo = false;
            { std::scoped_lock lk(g_silverMx); if (auto it = g_silver.find(id); it != g_silver.end()) { open = true; openAmmo = it->second.ammo; } }
            if (open && !openAmmo)
                return { Result::FailedSkill, "a melee silver episode is still open (it switches back first)", true };

            std::int32_t wornCount = 0, bestCount = 0;
            RE::TESAmmo* worn = WornAmmo(a_follower, wornCount);
            RE::TESAmmo* best = Logistics::BestSilverAmmo(a_follower, wantBolt, &bestCount);
            if (!best)
                return { Result::FailedSkill, std::format("no silver {} carried", kind), true };   // transparent -- cannot act
            // Already silver (this stack or another silver stack of his kind): nothing to do, no churn.
            if (worn && Logistics::IsSilverAmmo(worn) && Logistics::AmmoIsBolt(worn) == wantBolt) {
                if (openAmmo) { std::scoped_lock lk(g_silverMx); if (auto it = g_silver.find(id); it != g_silver.end()) it->second.trueAt = std::chrono::steady_clock::now(); }
                return { Result::FailedSkill, std::format("silver {} already nocked", kind), true };
            }

            // RE-NOCK FLOOR: inside an open episode the silver ammo is not worn (the AI re-chose its own) --
            // say so once, and do not re-equip more often than kRenockFloor.
            if (openAmmo) {
                std::scoped_lock lk(g_silverMx);
                if (auto it = g_silver.find(id); it != g_silver.end()) {
                    if (!it->second.aiOverrideLogged) {
                        it->second.aiOverrideLogged = true;
                        spdlog::warn("[silver] {:08X}: the engine put its own ammo '{}' back over the silver while the episode is open -- "
                                     "re-nocking at most every {} s", id, worn ? NameOf(worn) : "none",
                                     std::chrono::duration_cast<std::chrono::seconds>(kRenockFloor).count());
                    }
                    if (std::chrono::steady_clock::now() - it->second.lastNock < kRenockFloor)
                        return { Result::FailedSkill, "silver ammo re-nock held off (re-nock floor)", true };
                }
            }

            // Under the equip authority with a launcher hold in the ledger, Ammo is an OWNED category:
            // the episode below is what the declaration (logistics/EquipAuthority.cpp rule 2) names, and
            // APMF equips the declared set -- no direct equip (EquipWeapon's rule).
            const bool authority = AmmoUnderAuthority(id);
            if (authority && !APMFBridge::ClaimEquipAuthority(id))
                return { Result::FailedOther, "silver ammo not equipped: the equip authority refused the claim (MFO leaves the hands to the engine)", true };

            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return { Result::FailedOther, "no equip manager", true };
            if (!authority)
                mgr->EquipObject(a_follower, best, nullptr, static_cast<std::uint32_t>(bestCount), nullptr, true, false);   // plain equip: no lock to leak
            {
                std::scoped_lock lk(g_silverMx);
                auto [it, fresh] = g_silver.try_emplace(id);
                it->second.ammo   = true;
                it->second.silver = best->GetFormID();
                it->second.direct = false;
                if (fresh && worn) it->second.ammoPrev = worn->GetFormID();   // the ammo the silver replaced; never overwritten while the episode is open
                it->second.trueAt   = std::chrono::steady_clock::now();
                it->second.lastNock = it->second.trueAt;
            }
            if (authority) DeclareLedger(a_follower, "gambit equip silver ammo");   // after the episode opens: the declaration names it
            spdlog::info("[silver] {:08X}: GAMBIT silver ammo -- '{}' x{} nocked ({}), replaces '{}'", id, NameOf(best), bestCount,
                         kind, worn ? NameOf(worn) : "none");
            return { Result::FailedSkill, std::format("nocked silver {}", kind), true };
        }
    }

    Outcome EquipSilver(RE::Actor* a_follower) {
        if (!a_follower) return { Result::FailedOther, "no follower", true };
        const auto id = a_follower->GetFormID();
        RE::TESForm* const heldRight = a_follower->GetEquippedObject(false);
        RE::TESForm* const heldLeft  = a_follower->GetEquippedObject(true);
        auto* rightW = heldRight ? heldRight->As<RE::TESObjectWEAP>() : nullptr;
        const bool launcher = IsLauncher(heldRight);   // a bow / crossbow in the weapon hand: the ammo road

        // THE TARGET GATE (review F1 on 102e24c; marth: "when the TARGET is no longer Unholy"). The
        // rule's condition only arms it; silver is for the foe he is FIGHTING: his own
        // currentCombatTarget must be Unholy. Not Unholy -> transparent FailedSkill, so the
        // Scheduler reads the silver rule as not holding (silverKnownFalse) and the 2 s dwell
        // switches back. No usable target (null / dead / disabled / not hostile) while his group
        // still has foes is UNKNOWN, not false (the MFO-B55 rule): an open episode or a silver
        // weapon in hand is kept (transparent NoOp = held this lap); otherwise nothing is equipped.
        {
            auto  tp  = a_follower->GetActorRuntimeData().currentCombatTarget.get();
            auto* tgt = tp.get();
            const bool usable = tgt && !tgt->IsDead() && !tgt->IsDisabled() && tgt->IsHostileToActor(a_follower);
            if (usable && !Logistics::IsUnholy(tgt))
                return { Result::FailedSkill, "combat target is not Unholy", true };
            if (!usable) {
                bool episode = false;
                { std::scoped_lock lk(g_silverMx); episode = g_silver.find(id) != g_silver.end(); }
                const bool silverInHand = rightW && Logistics::IsSilverMeleeWeapon(rightW);
                if ((episode || silverInHand) && CombatSense::FoeCount(a_follower) > 0) {
                    if (launcher)   // never a NoOp for a ranged follower: the Scheduler would read a melee hand claim
                        return { Result::FailedSkill, "no usable combat target, foes remain -- silver ammo kept (unknown, not false)", true };
                    return { Result::NoOp, "no usable combat target, foes remain -- silver kept (unknown, not false)", true };
                }
                return { Result::FailedSkill, "no usable combat target", true };
            }
        }

        // A RANGED follower (bow / crossbow in the weapon hand) uses silver AMMO, never a melee swap.
        if (launcher) return EquipSilverAmmo(a_follower, rightW);

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
        // An open AMMO episode ends first (he is no longer on a launcher): erase, restore OUTSIDE the lock,
        // then a FRESH melee episode opens below -- the two kinds never merge.
        {
            Episode endedAmmo; bool hadAmmo = false;
            {
                std::scoped_lock lk(g_silverMx);
                if (auto it = g_silver.find(id); it != g_silver.end() && it->second.ammo) {
                    endedAmmo = it->second; g_silver.erase(it); hadAmmo = true;
                }
            }
            if (hadAmmo) RestoreSilverAmmo(id, endedAmmo, "main weapon is melee again");
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

    void ReconcileSilver(RE::Actor* a_follower, bool a_held, bool a_knownFalse, bool a_mainHoldReleasable) {
        if (!a_follower) return;
        const auto id  = a_follower->GetFormID();
        const auto now = std::chrono::steady_clock::now();
        Episode ep;
        {
            std::unique_lock lk(g_silverMx);
            auto it = g_silver.find(id);
            if (it == g_silver.end()) return;
            if (it->second.ammo) {
                // A SILVER AMMO episode has its OWN truth (AmmoEpisodeHolds), never the Scheduler's melee one.
                lk.unlock();
                const bool holds = AmmoEpisodeHolds(a_follower);
                Episode done;
                {
                    std::scoped_lock lk2(g_silverMx);
                    auto it2 = g_silver.find(id);
                    if (it2 == g_silver.end() || !it2->second.ammo) return;
                    if (holds) { it2->second.trueAt = now; return; }
                    if (now - it2->second.trueAt < kSilverDwell) return;
                    done = it2->second;
                    g_silver.erase(it2);
                }
                RestoreSilverAmmo(id, done, "no longer Unholy");
                return;
            }
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
                // Review F3 on 102e24c: a main hold whose own equip rule is ALSO released this lap
                // (a_mainHoldReleasable = no equip rule holds and their truth is KNOWN false, dwell or not) is NOT restored -- the general
                // reconcile right after would strip it again (unequip, equip, unequip, empty hand). It
                // takes the non-forced re-arm below instead.
                if (ep.mainHeld && mainObj && !a_mainHoldReleasable) {
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
        std::unique_lock lk(g_silverMx);
        auto it = g_silver.find(a_follower);
        if (it == g_silver.end()) return 0;
        const Episode ep = it->second;
        g_silver.erase(it);
        if (ep.ammo) {   // a silver AMMO episode: the previous ammo goes back (nothing to re-arm in the hand)
            lk.unlock();
            RestoreSilverAmmo(a_follower, ep, "silver hold released (combat end / release)");
            return 0;
        }
        if (ep.direct || !a_heldRight || a_heldRight->GetFormID() != ep.silver) return 0;
        return ep.mainRight;
    }

    RE::FormID SilverAmmoEpisode(RE::FormID a_follower) {
        std::scoped_lock lk(g_silverMx);
        const auto it = g_silver.find(a_follower);
        return (it != g_silver.end() && it->second.ammo) ? it->second.silver : 0;
    }

    void ClearSilverEpisodes() {
        std::scoped_lock lk(g_silverMx);
        g_silver.clear();
    }

}
