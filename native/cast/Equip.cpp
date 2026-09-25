// cast/Equip.cpp -- the WEAPON HOLD: EquipWeapon and its weapon-style helpers,
// the T#76 force-hold ledger (g_forcedWeapon) and its lifecycle, and the FWPN
// co-save (CoSaveForcedWeapons / CoLoadForcedWeapons, whole in this file).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "Runtime.h"      // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include "Logistics_internal.h" // 2026-09-13: EquipWeapon consumes THE weapon-style decision
                          // (ComputeWeaponRoles / WeaponScore) -- not in Logistics.h, and that
                          // header was outside the change's boundary. First non-Logistics include.
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps
#include <limits>         // rank preemption: kNoRule sentinel (numeric_limits<int>::max)

namespace MFO::Actuation {

    // T#76: the weapon a follower is being force-held on, keyed by FormID. Written
    // by EquipWeapon (the fire) and cleared by ReleaseForcedWeapon/Reconcile/
    // ClearForcedWeapons -- all on the job-worker serial tick, the same thread the
    // scheduler already drives Fire on. Persists ACROSS ticks by design -- it is
    // engine-hold bookkeeping like CombatStyle's owned-stance map, not per-scan
    // evaluator state (#22). Session-scoped: cleared on revert
    // (ClearForcedWeapons). The pointer is a weapon form, stable for the session.
    //
    // *** EVERY ACCESS TAKES g_forcedMx. *** This comment used to end "so no lock
    // (the g_followers discipline: #4)" while the very next paragraph said "guard
    // every access", and the file has been following the second one since
    // 2026-08-17 -- two rules in twelve lines, which is how an unguarded read gets
    // written in good faith (one was, in the first cut of WeaponHandExposure, and
    // the review caught it). The #4 no-lock discipline does NOT apply here, for the
    // reason the SEV-1 note below states: this map has an OFF-THREAD reader. Only
    // the WRITERS are worker-serial. Corrected 2026-09-08; do not restore the
    // no-lock claim.
    //
    // (It is also no longer read only by the save: WeaponHandExposure reads it
    // to decide the either-free hand preference.) 2026-09-13: the value is a
    // ForcedHold {right, left} -- the T#76 right hold as before plus the dual-
    // wield LEFT hold (shape: Actuation_internal.h). FWPN layout UNCHANGED.
    std::unordered_map<RE::FormID, ForcedHold> g_forcedWeapon;
    // SEV-1 (Fable 2026-08-17): the worker tick mutates this map, but the SKSE
    // SAVE callback reads it (CoSaveForcedWeapons) off that thread with no pump
    // barrier -- a concurrent insert would invalidate its iterator mid-save
    // (CTD / truncated save). Guard every access; NEVER hold it across an
    // Equip/UnequipObject engine call (the MSTK "copy then act" discipline).
    std::mutex g_forcedMx;

    namespace {

        // ── WEAPON STYLE BY PERKS, the COMBAT half (2026-09-13) ──────────────
        // marth: "perks in dual wielding -> the follower dual wields; perks in
        // greatswords -> strongly prefers greatswords." EquipWeapon consumes THE
        // decision the loot/keep/buy judges already run (Logistics::
        // ComputeWeaponRoles -> WeaponRoles{preferKinds, offHand} + WeaponScore)
        // instead of re-deriving it: the pick INSIDE the ordered class is by
        // WeaponScore (damage x kStyleBias for a perk-preferred kind -- a bias,
        // never a filter; preferKinds == 0 -> the score IS the uint16 damage, so
        // the old `>=` order holds exactly); offHand == 2 fills the LEFT hand
        // with a second in-role one-hander, force-held in the same T#76 ledger
        // (ForcedHold::left); offHand == 1 puts the best carried shield there
        // (plain equip, no hold -- the dueling style keeps shields itself);
        // offHand == 0 -> the left hand is never touched. AUTHORITY ORDER is
        // unchanged: class override / stance / the equip gambits decide melee-
        // vs-ranged-vs-cast; the roles' melee CLASS is deliberately NOT a filter
        // here (the pick still spans both melee classes) so the default case
        // cannot change.
        // THE LEFT HAND DEFERS TO A CAST (heals are LEFT-only: ClaimHealCast;
        // marth 2026-09-14: "the weapon should always yield to a spell on left
        // hand"). One question (CastHandHeld, Actuation_Hands.cpp), four guards:
        // never filled while a claim/lock is live on it; never filled while a
        // LEFT gear debt is open (Loadout::OwesLeft -- a hold over an unpaid
        // debt would Debounce every later Prepare until combat end, Fable F-A);
        // a cast taking the left hand YIELDS the hold at the point of no return,
        // INSIDE Loadout::Prepare immediately before its EquipSpell (the
        // LeftHandYield callback CastOn passes -- after Prepare's own refusals,
        // never before them, Fable F1); ReconcileForcedWeapon yields the tick a
        // claim is seen live there (the ComposedCast heal path never passes
        // CastOn). Once the lock lapses AND the 5 s floor a yield stamps has
        // passed, the satisfied lap tops the hand back up.
        // KILL SWITCH: every left-hand write is gated on bWeaponStyleControl.
        // THREADING: the job-worker tick calling ActorEquipManager directly, the
        // T#76 right-hand road since v1.0.33 (#62's Post is the armor/3D case).
        // ── Tier-A EQUIP actions (§4.5) ──────────────────────────────────────
        // Equip the best weapon of a category from the follower's OWN inventory.
        // IDEMPOTENT: a no-op when already holding that category, so a persistently
        // winning rule does not re-equip every tick. Direct ActorEquipManager,
        // the same path LootEquipment uses; runs in combat, so a weapon it equips
        // can coexist with a left-hand spell but is NOT tracked by Loadout's hand
        // ledger -- acceptable because equip and cast are alternative gambits
        // (first-match-wins fires only one per tick).

        // THE inventory-side decision, read off the same g_followers record the
        // Scheduler drives this tick from (worker-serial, #4 -- GetBaseClass's
        // own path). No record (Fire is public) -> default roles: no votes.
        Logistics::WeaponRoles WeaponRolesFor(RE::Actor* a_follower) {
            const auto it = g_followers.find(a_follower->GetFormID());
            if (it == g_followers.end()) return {};
            return Logistics::ComputeWeaponRoles(a_follower, it->second);
        }

        // Sword / dagger / war axe / mace (TESObjectWEAP's own WEAPON_TYPE tests)
        // -- what a left hand can pair with. Not 2H, bow, crossbow, staff, fists.
        bool IsOneHandMelee(const RE::TESObjectWEAP* a_w) {
            return a_w && (a_w->IsOneHandedSword() || a_w->IsOneHandedDagger() ||
                           a_w->IsOneHandedAxe()   || a_w->IsOneHandedMace());
        }

        // Best carried one-hander for the LEFT hand by the SAME WeaponScore, that
        // is NOT the right hand's only instance (a different form, or a second
        // copy: count >= 2 -- with ONE copy the engine would MOVE it out of the
        // right hand). Same eligibility as the right: no staff, no non-playable
        // creature gear, daggers only for a bMageDaggersOnly base mage.
        RE::TESObjectWEAP* PickOffHandWeapon(RE::Actor* a_follower, const Logistics::WeaponRoles& a_roles,
                                             bool a_daggerMelee, const RE::TESObjectWEAP* a_right) {
            RE::TESObjectWEAP* best = nullptr; float bestScore = 0.0f;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w->IsStaff() || !IsOneHandMelee(w)) continue;
                if ((w->GetFormFlags() & (1u << 2)) != 0) continue;   // non-playable: invisible on a humanoid
                if (a_daggerMelee && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                if (w == a_right && data.first < 2) continue;         // the right hand's only copy
                const float score = Logistics::WeaponScore(a_roles, w);
                if (!best || score >= bestScore) { bestScore = score; best = w; }
            }
            return best;
        }

        // The best carried shield by armor rating, or nullptr.
        RE::TESObjectARMO* PickShield(RE::Actor* a_follower) {
            RE::TESObjectARMO* best = nullptr; float bestAr = 0.0f;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* a = obj->As<RE::TESObjectARMO>();
                if (!a || !a->IsShield()) continue;
                if ((a->GetFormFlags() & (1u << 2)) != 0) continue;   // non-playable
                const float ar = a->GetArmorRating();
                if (!best || ar >= bestAr) { bestAr = ar; best = a; }
            }
            return best;
        }

        // SHIELD EQUIP ON THE MAIN THREAD (Fable F3 on f771399, #62): an ARMOR
        // equip from this job worker races the render thread's 3D rebuild (the
        // invisible-head class of bug) -- the same reason Logistics_Loot.cpp's
        // equipIt hop exists. Capture FormIDs only, never the worker's Actor*/
        // item pointers; re-resolve on the frame that runs; null-check both. A
        // plain equip, no ledger entry: the AI keeps shields on its own. VR (no
        // pump): the inline call, as the loot precedent does.
        void EquipShieldOnMain(RE::FormID a_follower, RE::FormID a_shield) {
            auto doEquip = [a_follower, a_shield]() {
                auto* fol  = RE::TESForm::LookupByID<RE::Actor>(a_follower);
                auto* form = RE::TESForm::LookupByID(a_shield);
                auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
                if (auto* eq = RE::ActorEquipManager::GetSingleton(); fol && item && eq)
                    eq->EquipObject(fol, item);
            };
            if (MainThread::IsInstalled()) MainThread::Post(doEquip);
            else                           doEquip();
        }

        // THE LAST REFUSAL REASON LOGGED PER FOLLOWER (Deck 2026-09-14 finding,
        // round 2 of feat/mfo-1.5.97-pass): `[equip] ... + off-hand 'Ebony Dagger'
        // (dual wield by perks)` printed five times with ZERO `[hold]` lines --
        // EquipLeftHeld returned false every time (LeftHandSlot() was null on
        // 1.6.1170, see Loadout.cpp) and nothing said so. Principle 7: a refused
        // precondition is an ERROR line, once per follower per reason (a repeat of
        // the same reason is silent until it changes or a hold succeeds, which
        // clears the entry). Worker-serial, no lock (#4, as g_offHandRetryAt: both
        // call sites are EquipWeapon on the job worker).
        std::unordered_map<RE::FormID, const char*> g_leftHeldRefusal;

        // Put a_weap in the LEFT hand force-held and record ForcedHold::left. Map
        // under the lock, engine calls outside it (the SEV-1 discipline). A
        // different weapon already held there is force-unequipped first, as the
        // right-hand path swaps its own lock. false = a precondition failed (no
        // manager / no left slot form / no weapon), NO write to the ledger, an
        // error line naming the precondition (rate-limited above), and a_whyNot
        // (if given) set to that name so the caller's own line can say it too.
        //
        // LEGACY ONLY (APMF absent / the equip authority not live): under the
        // authority the caller writes the ledger through RecordLeftHold below and
        // APMF places the weapon in the LEFT hand itself (ABI v8 SetEquipSetEx,
        // kEquipSlot_Left); no engine call of MFO's is made.
        bool EquipLeftHeld(RE::Actor* a_follower, RE::TESObjectWEAP* a_weap,
                           const char** a_whyNot = nullptr) {
            auto* mgr  = RE::ActorEquipManager::GetSingleton();
            auto* slot = Loadout::LeftHandSlot();
            const char* why = !a_follower ? "follower"
                            : !mgr        ? "equip manager"
                            : !slot       ? "left slot form (Loadout::LeftHandSlot())"
                            : !a_weap     ? "weapon"
                            : nullptr;
            if (why) {
                if (a_whyNot) *a_whyNot = why;
                if (a_follower) {
                    const auto fid = a_follower->GetFormID();
                    auto it = g_leftHeldRefusal.find(fid);
                    if (it == g_leftHeldRefusal.end() || it->second != why) {
                        g_leftHeldRefusal[fid] = why;
                        spdlog::error("[hold] {:08X}: EquipLeftHeld REFUSED -- {} null; the off-hand "
                                      "'{}' is NOT held (logged once per reason)", fid, why,
                                      a_weap && a_weap->GetName() ? a_weap->GetName() : "?");
                    }
                } else {
                    spdlog::error("[hold] EquipLeftHeld REFUSED -- follower null");
                }
                return false;
            }
            const auto id = a_follower->GetFormID();
            g_leftHeldRefusal.erase(id);   // a success re-arms the once-per-reason line
            RE::TESBoundObject* oldLeft = nullptr;
            {
                std::scoped_lock lk(g_forcedMx);
                if (auto it = g_forcedWeapon.find(id); it != g_forcedWeapon.end() &&
                    it->second.left && it->second.left != a_weap)
                    oldLeft = it->second.left;
            }
            if (oldLeft)   // the LEFT slot, as every left-hand unequip (F4 parity)
                mgr->UnequipObject(a_follower, oldLeft, nullptr, 1, slot, true, true);
            mgr->EquipObject(a_follower, a_weap, nullptr, 1, slot, true, true);
            {
                std::scoped_lock lk(g_forcedMx);
                g_forcedWeapon[id].left = a_weap;
            }
            return true;
        }

        // Defined in the anon namespace below EquipWeapon (its doc is there): the
        // two-hop left-hand readback. Under the authority it is the probe's proof
        // that APMF's `hand=left` equip reached the engine (criterion 7).
        void LogLeftHandReadback(RE::FormID a_id);

        // ── APMF EQUIP AUTHORITY: the LEFT hold WITHOUT an engine call (ABI v8) ──
        // Under the authority MFO no longer force-equips either hand: the ledger
        // is written (it stays the source of truth the declaration reads --
        // RefreshEquipDeclaration declares .right as kEquipSlot_Right and .left
        // as kEquipSlot_Left) and APMF's own pass places each weapon in the hand
        // the entry names. The ONE engine call kept is the force-UNEQUIP of a
        // DIFFERENT weapon MFO itself had locked in the left (a hold placed by the
        // legacy path before the authority went live): a prevent-removal lock is
        // MFO's own, APMF's non-forced equip cannot displace through it, and
        // unequips are never seated. Returns the old hold it cleared, or nullptr.
        RE::TESBoundObject* RecordLeftHold(RE::Actor* a_follower, RE::TESObjectWEAP* a_weap) {
            if (!a_follower || !a_weap) return nullptr;
            const auto id = a_follower->GetFormID();
            g_leftHeldRefusal.erase(id);
            RE::TESBoundObject* oldLeft = nullptr;
            {
                std::scoped_lock lk(g_forcedMx);
                auto& hold = g_forcedWeapon[id];
                if (hold.left && hold.left != a_weap) oldLeft = hold.left;
                hold.left = a_weap;
            }
            if (oldLeft)
                if (auto* mgr = RE::ActorEquipManager::GetSingleton())
                    mgr->UnequipObject(a_follower, oldLeft, nullptr, 1, Loadout::LeftHandSlot(), true, true);
            return oldLeft;
        }

        // Declare this follower's worn set from the ledger as it stands NOW
        // (the FollowerState read off g_followers, worker-serial like
        // WeaponRolesFor; a_leftReserved from the lock/claim the equip side
        // already yields to). THE COMBAT ROAD: judgeArmor=false -- legacy never
        // wears armor in combat, so the worn armor is declared as is (F9). No
        // forced re-issue exists (F3): the declaration changes when the ledger
        // does, and APMF places the hands the entries name (ABI v8).
        // MFO-B41 (round 4): the ledger as this road last declared it, per
        // follower. A lap whose ledger is UNCHANGED but whose weapon is in
        // NEITHER hand (something un-seated took it off; the equip rule Fired
        // again) would compare equal and send nothing -- so the change detector
        // is dropped first and the same set goes out again, which is APMF's
        // "give it back". Worker-serial (#4), cleared with the ledger.
        std::unordered_map<RE::FormID, std::pair<RE::FormID, RE::FormID>> g_lastLedgerDeclared;
        // ... throttled to APMF's own 3 s per-item re-issue hold, so a weapon the
        // engine keeps taking back off (observe mode) costs one re-send per 3 s,
        // never one per lap.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastB41At;
        constexpr auto kB41Hold = std::chrono::seconds(3);

        // The live bound-weapon set as this road last saw it, per follower --
        // ReconcileForcedWeapon compares against it every in-combat lap so a
        // Bound Sword the AI (or MFO) just cast, and its expiry, each trigger ONE
        // re-declaration (the set itself is rebuilt by Logistics). Worker-serial.
        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_boundSeen;

        void DeclareFromLedger(RE::Actor* a_follower, const char* a_why) {
            if (!a_follower) return;
            const auto id = a_follower->GetFormID();
            const auto it = g_followers.find(id);
            if (it == g_followers.end()) return;
            RE::FormID right = 0, left = 0;
            {
                std::scoped_lock lk(g_forcedMx);
                if (auto h = g_forcedWeapon.find(id); h != g_forcedWeapon.end()) {
                    right = h->second.right ? h->second.right->GetFormID() : 0;
                    left  = h->second.left  ? h->second.left->GetFormID()  : 0;
                }
            }
            // MFO-B41: unchanged ledger + its weapon in neither hand -> re-send.
            if (auto prev = g_lastLedgerDeclared.find(id);
                prev != g_lastLedgerDeclared.end() && prev->second == std::make_pair(right, left)) {
                auto* hr = a_follower->GetEquippedObject(false);
                auto* hl = a_follower->GetEquippedObject(true);
                const auto heldId = [](RE::TESForm* f) { return f ? f->GetFormID() : 0u; };
                const bool rightGone = right && heldId(hr) != right && heldId(hl) != right;
                const bool leftGone  = left  && heldId(hl) != left  && heldId(hr) != left;
                const auto now = std::chrono::steady_clock::now();
                auto at = g_lastB41At.find(id);
                if ((rightGone || leftGone) && (at == g_lastB41At.end() || now - at->second >= kB41Hold)) {
                    g_lastB41At[id] = now;
                    Logistics::ResendEquipDeclaration(id);
                    spdlog::info("[equip-auth] {:08X}: ledger weapon in neither hand -- re-declaring (MFO-B41)", id);
                }
            }
            g_lastLedgerDeclared[id] = { right, left };
            Logistics::RefreshEquipDeclaration(a_follower, it->second, right, left,
                                               CastHandHeld(a_follower, kHandLeft), /*judgeArmor*/false, a_why);
        }

        // Off-hand TOP-UP cadence on the equip rule's SATISFIED lap (the AI drew
        // the sword at combat start, or a cast just gave the left back): the
        // answer costs an inventory walk, so it is asked at most once per
        // follower per kOffHandRetry. A yield RE-STAMPS this to now + kOffHandRetry
        // (a FLOOR, principle 9 -- Fable F1 on f771399): a hand a cast freed
        // refills no sooner than 5 s after the yield, so a persistently refused
        // cast can cost at most one weapon<->spell flicker per 5 s, never one per
        // lap. Worker-serial, no lock (#4, as g_castLock). Cleared with the
        // ledger; erased on release so the next combat starts fresh.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_offHandRetryAt;
        constexpr auto kOffHandRetry = std::chrono::seconds(5);

    }

        Outcome EquipWeapon(RE::Actor* a_follower, bool a_ranged) {
            // BOTH HANDS decide "already holding" (T#75). The old guard read only
            // the RIGHT hand -- but a caster keeps a SPELL there, so the melee
            // weapon her off-hand still held was invisible to it and a
            // persistently-winning equip rule re-equipped the SAME weapon every
            // service tick (deck, Serana: 'Ebony Tanto' at gambit cadence,
            // visible weapon thrash while her own AI fought back for the
            // spell). The rule's goal is a weapon CATEGORY in hand -- EITHER
            // hand satisfies it.
            // marth: a base MAGE's melee sidearm is a DAGGER -- the same base-class
            // rule the loot path uses (#65 combatClassOverride==3/Cast, gated by
            // bMageDaggersOnly). For such a follower a MELEE equip order means a
            // dagger specifically: a looted sword/mace he still owns must NOT count
            // as "already holding the category" (else the satisfied NoOp below keeps
            // him on it forever) and must NOT win the draw. baseClass read on the
            // worker's g_followers-serial path (same as Scheduler / Actuation:25).
            // v1.1: actor-keyed g_followers find + combatClassOverride read == the
            // general GetBaseClass primitive (byte-identical; same serial-worker path).
            const std::uint8_t baseClass = Followers::GetBaseClass(a_follower);
            const bool daggerMelee = !a_ranged && baseClass == 3 && Config::g_mageDaggersOnly.load();
            const auto holdsCategory = [a_ranged, daggerMelee](RE::TESForm* a_held) {
                auto* w = a_held ? a_held->As<RE::TESObjectWEAP>() : nullptr;
                if (!w || w->IsStaff()) return false;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) return false;
                return !daggerMelee || w->GetWeaponType() == RE::WEAPON_TYPE::kOneHandDagger;
            };
            // TRANSPARENT (satisfied): the rule's goal already holds, so
            // the scan falls past it -- AND the scheduler reads this
            // exact shape (transparent NoOp on an equip action) as the
            // H2 hand-claim: a lower CONTRADICTORY equip is skipped
            // without firing, so fall-through cannot manufacture the
            // melee<->ranged thrash of GAMBIT_FLOWS D4.
            RE::TESForm* const heldRight = a_follower->GetEquippedObject(false);
            RE::TESForm* const heldLeft  = a_follower->GetEquippedObject(true);
            if (holdsCategory(heldRight) || holdsCategory(heldLeft)) {
                // ── OFF-HAND TOP-UP on the satisfied lap (2026-09-13) ─────────
                // Melee, style control ON, a one-hander in the RIGHT hand, no
                // WEAPON in the left. Cheap reads first; the inventory walk only
                // inside the retry cadence and with no cast holding the left.
                // Every early-out is the SAME transparent NoOp as before.
                if (!a_ranged && Config::g_weaponStyleControl.load()) {
                    auto* rightW = heldRight ? heldRight->As<RE::TESObjectWEAP>() : nullptr;
                    auto* leftW  = heldLeft  ? heldLeft->As<RE::TESObjectWEAP>()  : nullptr;
                    auto* leftA  = heldLeft  ? heldLeft->As<RE::TESObjectARMO>()  : nullptr;
                    const auto id  = a_follower->GetFormID();
                    const auto now = std::chrono::steady_clock::now();
                    auto retry = g_offHandRetryAt.find(id);
                    const bool due = retry == g_offHandRetryAt.end() || now >= retry->second;
                    // !OwesLeft (Fable F-A on 1ac3c6b): a heal that displaced the
                    // AI's own shield/off-hand booked a LEFT debt; a force-hold
                    // placed over that unpaid debt can never be settled by the AI
                    // (locked) and Prepare's debt gate then Debounces every later
                    // cast until combat end. The repay (hit / combat end) clears
                    // the debt, then the top-up may fill the hand.
                    if (rightW && IsOneHandMelee(rightW) && !leftW && due &&
                        !CastHandHeld(a_follower, kHandLeft) && !Loadout::OwesLeft(id)) {
                        g_offHandRetryAt[id] = now + kOffHandRetry;
                        const Logistics::WeaponRoles roles = WeaponRolesFor(a_follower);
                        // TRANSPARENT either way (Fable F5 on f771399): a refill is a
                        // side effect of a SATISFIED rule, not this tick's action. A
                        // Fired here armed the Scheduler's suppression window (rules
                        // below the equip -- heals -- starved ~1.5 s per refill),
                        // stamped lastFired and fed ProgAllocator::NoteCombatFire.
                        // g_offHandRetryAt is stamped ABOVE, before the attempt, on
                        // purpose: it is the FLOOR on how often the inventory walk
                        // runs (principle 9), not a record of success -- a refused
                        // hold retries in kOffHandRetry, and the only success-state
                        // is the ledger .left EquipLeftHeld writes itself.
                        // APMF EQUIP AUTHORITY (feat/mfo-equip-authority, ABI v8): with
                        // the claim live the ledger is written (RecordLeftHold) and
                        // the set is DECLARED with the off-hand as kEquipSlot_Left;
                        // APMF places it. Send-on-change (F3): the new .left IS the
                        // change. The shield branch is always direct (v9). The
                        // readback (two hops) is the probe's proof of the hand.
                        // Claim-or-keep here as well as in the OOC service, so a
                        // follower first seen IN combat (a load mid-fight) is
                        // claimed on his first equip lap, not at combat end.
                        const bool authority = APMFBridge::EquipAuthoritySupported() &&
                                               APMFBridge::ClaimEquipAuthority(id);
                        if (roles.offHand == 2) {
                            if (auto* w = PickOffHandWeapon(a_follower, roles, daggerMelee, rightW)) {
                                const char* whyNot = nullptr;
                                bool held = false;
                                if (authority) {
                                    RecordLeftHold(a_follower, w);
                                    DeclareFromLedger(a_follower, "off-hand top-up");
                                    LogLeftHandReadback(id);
                                    held = true;
                                } else {
                                    held = EquipLeftHeld(a_follower, w, &whyNot);
                                }
                                if (held) {
                                    a_follower->DrawWeaponMagicHands(true);
                                    spdlog::info("[equip] {:08X}: GAMBIT equip off-hand '{}' (dual wield by "
                                                 "perks, top-up{})", id, w->GetFullName() ? w->GetFullName() : "?",
                                                 authority ? ", declared" : "");
                                } else {
                                    spdlog::info("[equip] {:08X}: off-hand '{}' NOT held ({} null, top-up)",
                                                 id, w->GetFullName() ? w->GetFullName() : "?",
                                                 whyNot ? whyNot : "?");
                                }
                            }
                        } else if (roles.offHand == 1 && !(leftA && leftA->IsShield())) {
                            if (auto* sh = PickShield(a_follower)) {
                                EquipShieldOnMain(id, sh->GetFormID());   // v9: Shield is never owned -- always direct
                                spdlog::info("[equip] {:08X}: GAMBIT equip shield '{}' (shield by perks, "
                                             "top-up)", id, sh->GetFullName() ? sh->GetFullName() : "?");
                            }
                        }
                    }
                }
                return { Result::NoOp, "already holding that category", true };
            }
            // THE PICK: WeaponScore inside the ordered class (banner above). Ranged
            // never has a preferred kind, so a bow/crossbow pick is plain damage.
            const Logistics::WeaponRoles roles = WeaponRolesFor(a_follower);
            RE::TESObjectWEAP* best = nullptr; float bestScore = 0.0f;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w->IsStaff()) continue;
                // NON-PLAYABLE (record-header flag bit 2) is creature/automaton
                // gear -- fires but is INVISIBLE on a humanoid (same check as
                // Logistics' IsCreatureWeapon). Loot never stocks these, but a
                // player-given stray must not win the combat equip either.
                if ((w->GetFormFlags() & (1u << 2)) != 0) continue;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) continue;
                // base MAGE melee = daggers only (see holdsCategory above).
                if (daggerMelee && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                // `>=` on the float score orders EXACTLY as the old `>=` on the
                // uint16 damage did when no kind is preferred (last equal wins).
                const float score = Logistics::WeaponScore(roles, w);
                if (score >= bestScore) { bestScore = score; best = w; }
            }
            if (!best) return { Result::FailedSkill, a_ranged ? "no ranged weapon carried"
                                                              : "no melee weapon carried",
                                true };   // transparent -- cannot act, rules below run (§2)
            const std::uint16_t bestDmg = best->GetAttackDamage();
            // Off-hand plan (one-hander in the right; offHand 2 -> a second one-
            // hander, 1 -> a shield), gated on bWeaponStyleControl. A live cast
            // claim/lock on the left WINS: nothing goes there while one stands.
            RE::TESObjectWEAP* offHandW  = nullptr;
            RE::TESObjectARMO* offHandSh = nullptr;
            bool               offHandHeld   = false;     // EquipLeftHeld's verdict for offHandW
            const char*        offHandWhyNot = nullptr;   // ... and its reason when false
            bool               authority     = false;     // APMF equip authority live for this follower (set below)
            const bool offHandWanted = !a_ranged && roles.offHand != 0 && IsOneHandMelee(best) &&
                                       Config::g_weaponStyleControl.load();
            const bool leftCastHeld  = offHandWanted && CastHandHeld(a_follower, kHandLeft);
            if (offHandWanted && !leftCastHeld) {
                // The WEAPON hold (a prevent-removal lock) is also refused over an
                // open LEFT gear debt (F-A, same reason as the top-up); the shield
                // is a plain equip the AI can take back, so no gate.
                if (roles.offHand == 2) {
                    if (!Loadout::OwesLeft(a_follower->GetFormID()))
                        offHandW = PickOffHandWeapon(a_follower, roles, daggerMelee, best);
                } else {
                    offHandSh = PickShield(a_follower);
                }
            }
            if (auto* mgr = RE::ActorEquipManager::GetSingleton()) {
                if (Config::g_weaponStyleControl.load()) {
                    // T#76 FORCE-HOLD. forceEquip=true (7th arg, after extraData,
                    // count, slot, queueEquip) sets the engine's prevent-removal
                    // lock so the follower's OWN combat AI cannot auto-unequip the
                    // weapon to re-arm a spell -- the both-hands caster thrash
                    // v1.0.62 narrowed but did not close. The both-hands "already
                    // holding that category" NoOp above then keeps the rule
                    // satisfied with no re-equip until the gambit's condition goes
                    // false, when the scheduler releases it (ReconcileForcedWeapon).
                    const auto id = a_follower->GetFormID();
                    // A DIFFERENT weapon may still be locked from before -- a
                    // category flip (melee<->ranged), OR a base-mage daggers-only
                    // SAME-category swap (a looted sword was force-locked, then
                    // holdsCategory rejected it and the loop picked a dagger; the
                    // satisfied NoOp no longer catches this melee->melee case since
                    // c97035f). Force-unequip the old lock first, then lock the new.
                    // Read/write the map UNDER the lock; do the engine calls
                    // OUTSIDE it (SEV-1 discipline). The LEFT hold (2026-09-13)
                    // follows the same rule, released FIRST: a bow or two-hander
                    // needs that hand free and a prevent-removal lock refuses it.
                    RE::TESBoundObject* oldForced = nullptr;
                    RE::TESBoundObject* oldLeft   = nullptr;
                    {
                        std::scoped_lock lk(g_forcedMx);
                        if (auto old = g_forcedWeapon.find(id); old != g_forcedWeapon.end()) {
                            if (old->second.right && old->second.right != best) oldForced = old->second.right;
                            if (old->second.left  && old->second.left != offHandW) oldLeft = old->second.left;
                        }
                    }
                    // APMF EQUIP AUTHORITY (feat/mfo-equip-authority, ABI v8): with
                    // the standing claim live MFO makes NO equip call. The old
                    // holds are still force-UNEQUIPPED (they may carry MFO's own
                    // prevent-removal lock from a legacy-path hold, which APMF's
                    // non-forced equip could not displace; unequips are never
                    // seated), the ledger is written exactly as below, and the
                    // set is DECLARED from it with .right as kEquipSlot_Right and
                    // .left as kEquipSlot_Left -- APMF's pass places each in its
                    // hand. The shield is always direct (v9: never owned). Without the authority:
                    // byte-identical to before (right force-equipped here,
                    // EquipLeftHeld below).
                    authority = APMFBridge::EquipAuthoritySupported() && APMFBridge::ClaimEquipAuthority(id);
                    if (oldLeft)   // the LEFT slot, as every left-hand unequip (F4 parity)
                        mgr->UnequipObject(a_follower, oldLeft, nullptr, 1, Loadout::LeftHandSlot(), true, true);
                    if (oldForced)
                        mgr->UnequipObject(a_follower, oldForced, nullptr, 1, nullptr, true, true);
                    if (!authority)
                        mgr->EquipObject(a_follower, best, nullptr, 1, nullptr, true, true);
                    {
                        std::scoped_lock lk(g_forcedMx);
                        auto& hold = g_forcedWeapon[id];
                        hold.right = best;
                        if (oldLeft) hold.left = nullptr;
                    }
                    // Off-hand (style control ON only -- see the plan above). The
                    // hold's OUTCOME feeds the `[equip]` line below: it says
                    // "+ off-hand" only for a hold that actually happened.
                    if (offHandW) {
                        if (authority) { RecordLeftHold(a_follower, offHandW); offHandHeld = true; }
                        else           offHandHeld = EquipLeftHeld(a_follower, offHandW, &offHandWhyNot);   // force-held, ledger .left
                    } else if (offHandSh) {
                        EquipShieldOnMain(id, offHandSh->GetFormID());   // plain (F3: main thread); v9: Shield is never owned, always direct
                    }
                    if (authority) {
                        DeclareFromLedger(a_follower, a_ranged ? "gambit equip ranged" : "gambit equip melee");
                        if (offHandHeld) LogLeftHandReadback(id);   // criterion 7: the hand reached the engine
                    }
                } else {
                    mgr->EquipObject(a_follower, best);   // kill-switch off: today's behaviour exactly
                }
            }
            // FLAIR #4: visibly COMMIT to the new tool -- steel out, squared up
            // -- rather than leaving the swap in whatever draw state the last
            // weapon had. The exact call Loadout.cpp:240 already ships for the
            // cast path; drawing while already drawn is a no-op, and this path
            // only runs on a real equip Fired (which buys a suppression window),
            // so it cannot repeat-fire.
            a_follower->DrawWeaponMagicHands(true);
            const auto nm = [](RE::TESBoundObject* o) { return o && o->GetName() ? o->GetName() : "?"; };
            spdlog::info("[equip] {:08X}: GAMBIT equip {} '{}' dmg={}{}{}{}{}", a_follower->GetFormID(),
                         a_ranged ? "ranged" : "melee", nm(best), bestDmg,
                         Logistics::WeaponScore(roles, best) != static_cast<float>(bestDmg) ? " (perk-preferred kind)" : "",
                         offHandW  ? (offHandHeld
                                        ? std::format(" + off-hand '{}' (dual wield by perks)", nm(offHandW))
                                        : std::format(" off-hand '{}' NOT held ({} null)", nm(offHandW),
                                                      offHandWhyNot ? offHandWhyNot : "?"))
                         : offHandSh ? std::format(" + shield '{}' (shield by perks)", nm(offHandSh)) : std::string{},
                         leftCastHeld ? " (off-hand skipped: a cast holds the left hand)" : "",
                         authority ? " [declared: APMF equip authority]" : "");
            return { Result::Fired, a_ranged ? "equipped ranged" : "equipped melee" };
        }

        // (EquipTorch moved to Logistics -- torch is upkeep, not a combat action, #35.)

    // ── T#76: EQUIP FORCE-HOLD lifecycle ──────────────────────────────────────
    namespace {
        // READBACK (Fable F4 on f771399, principle 5 -- observe the path, do not
        // assume it): after a left-hand hold is released or yielded, log what the
        // LEFT hand holds. What is KNOWN: the unequip is QUEUED (a_queueEquip=true)
        // on the follower's own AI process and drains in the follower's update; a
        // read on this same call would only ever show the weapon still there.
        // What is NOT established (Fable F-C on 1ac3c6b): the in-frame order of
        // that drain against MainThread's queue, which drains inside the PLAYER's
        // Update vfunc -- a single Post could run BEFORE the follower's update of
        // the same frame and read stale. So the read is posted TWICE (a posted fn
        // may Post again; the repost lands NEXT frame, MainThread.h) -- one full
        // frame boundary after the drain, whatever the two updates' order within
        // a frame. Readings: a spell = conclusive (the slot-named unequip cleared
        // the lock); none = informative; the weapon = the lock survived -- with
        // the double Post no longer confusable with a stale read. VR (no pump)
        // reads inline and is then only an honest "queued, not yet applied".
        void LogLeftHandReadback(RE::FormID a_id) {
            auto read = [a_id]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
                if (!actor) return;
                auto* held = actor->GetEquippedObject(true);
                spdlog::info("[hold] {:08X}: left readback = {}", a_id,
                             held ? (held->GetName() && *held->GetName() ? held->GetName() : "?")
                                  : "none");
            };
            if (MainThread::IsInstalled()) MainThread::Post([read]() { MainThread::Post(read); });
            else                           read();
        }
    }

    std::pair<RE::FormID, RE::FormID> ForcedHoldFor(RE::FormID a_follower) {
        std::scoped_lock lk(g_forcedMx);
        const auto it = g_forcedWeapon.find(a_follower);
        if (it == g_forcedWeapon.end()) return { 0, 0 };
        return { it->second.right ? it->second.right->GetFormID() : 0u,
                 it->second.left  ? it->second.left->GetFormID()  : 0u };
    }

    void ReleaseForcedWeapon(RE::Actor* a_follower, bool a_standDown) {
        if (!a_follower) return;
        const auto id = a_follower->GetFormID();
        ForcedHold hold;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(id);
            if (it == g_forcedWeapon.end()) return;
            hold = it->second;
            g_forcedWeapon.erase(it);
        }
        g_offHandRetryAt.erase(id);   // next combat's off-hand top-up starts fresh
        g_leftHeldRefusal.erase(id);  // ... and a refused hold is reported afresh
        // forceEquip=true on the UNequip. CORRECTED 2026-09-22 (disassembly, Fable
        // spot check): the reason that stood here for months was WRONG, and a
        // rules-shaped comment that teaches a false fact is worse than none.
        //   * WRONG: "a plain unequip would be REFUSED against a forced item and
        //     the follower would stay stuck holding the weapon". `UnequipObject`
        //     does not work that way. Its dispatcher (AE `0x6CB550`, at
        //     `0x6CB5DA`) clears `ExtraCannotWear` UNCONDITIONALLY with a constant
        //     zero, BEFORE it even reads the force byte -- so a plain unequip
        //     releases the lock exactly as a forced one does, and "stuck holding
        //     it forever" was never a reachable state on this path.
        //   * The only non-forced refusal anywhere near here is on the EQUIP side
        //     (`0x69FB2A`): an equip with `!forceEquip` is refused when the
        //     CALLER-SUPPLIED extraData carries kCannotWear. MFO passes `nullptr`,
        //     so that gate never even applies to us.
        // The force flag STAYS: it is harmless, it is what every other release
        // path in this file passes (one shape, not two), and it keeps the call
        // correct if the dispatcher's unconditional clear is ever version-specific.
        // What it is NOT is load-bearing against a freeze. The lock itself is
        // `ExtraCannotWear` (extra type `0x3D`) on the WORN item's ExtraDataList --
        // see Docs/ENGINE_NOTES.md for where it is written and why the queued path
        // cannot set it either.
        // Engine calls OUTSIDE the lock. BOTH hands (2026-09-13): the
        // dual-wield left hold is released by the same call, same flags.
        // The LEFT hold names its slot (F4), mirroring EquipLeftHeld; the right
        // keeps the slot-less call it has always had.
        if (auto* mgr = RE::ActorEquipManager::GetSingleton()) {
            if (hold.left)  mgr->UnequipObject(a_follower, hold.left,  nullptr, 1, Loadout::LeftHandSlot(), true, true);
            if (hold.right) mgr->UnequipObject(a_follower, hold.right, nullptr, 1, nullptr, true, true);
        }
        // ── STAND DOWN = SHEATHE, NOT UNEQUIP (marth 2026-09-22) ─────────────────
        // The force-unequip above stays exactly as it was -- it is the only way to
        // clear the prevent-removal lock, and a follower left locked to a weapon can
        // never cast again. What it also does is take the weapon OFF THE BODY, so the
        // follower stands there empty-handed until something re-arms him: the field saw
        // Cicero's weapons "vanishing and returning" three times in one dragon fight,
        // and at a real combat end nothing re-arms until the NEXT fight.
        //
        // So on a stand-down the SAME weapon(s) go straight back on, NON-FORCED (no
        // lock, so the AI owns them again and may swap them whenever it likes), and the
        // follower is sheathed. Nothing else changes: the ledger is already erased, the
        // ch.15 claim is released below, and with no hold the ch.17 declaration leaves
        // both hands UNOWNED (RefreshEquipDeclaration rule 1), so APMF's seat refuses
        // none of this.
        //
        // TWO MAIN-THREAD HOPS, deliberately. The unequip above is a QUEUED engine op
        // (a_queueEquip = true) and re-equipping the same object in the same breath
        // would race it, so this rides the SAME double-post idiom LogLeftHandReadback
        // in this file already uses to observe a settled hold -- roughly two frames,
        // far below anything a player can see. It is also the #62 rule: an equip
        // rebuilds biped 3D and belongs on the main thread, never on this job worker.
        // Re-resolved on the frame that runs (the captured pointers may be stale by
        // then), skipped for a dead/disabled follower, skipped for an item the follower
        // no longer owns (sold/handed back between the hops), and skipped for a hand
        // something has ALREADY put a weapon into -- it re-arms, it never evicts.
        if (a_standDown && (hold.left || hold.right) && MainThread::IsInstalled()) {
            const RE::FormID rightID = hold.right ? hold.right->GetFormID() : 0;
            const RE::FormID leftID  = hold.left  ? hold.left->GetFormID()  : 0;
            auto reArm = [id, rightID, leftID]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                auto* mgr   = RE::ActorEquipManager::GetSingleton();
                if (!actor || !mgr || actor->IsDead() || actor->IsDisabled()) return;
                auto inv = actor->GetInventory();
                const auto owns = [&inv](RE::FormID a_form) -> RE::TESBoundObject* {
                    if (a_form == 0) return nullptr;
                    auto* form = RE::TESForm::LookupByID(a_form);
                    auto* obj  = form ? form->As<RE::TESBoundObject>() : nullptr;
                    if (!obj) return nullptr;
                    const auto it = inv.find(obj);
                    return (it != inv.end() && it->second.first > 0) ? obj : nullptr;
                };
                int put = 0;
                if (auto* r = owns(rightID); r && !actor->GetEquippedObject(false)) {
                    mgr->EquipObject(actor, r, nullptr, 1, nullptr, true, false);   // forceEquip = FALSE
                    ++put;
                }
                if (auto* l = owns(leftID); l && !actor->GetEquippedObject(true)) {
                    mgr->EquipObject(actor, l, nullptr, 1, Loadout::LeftHandSlot(), true, false);
                    ++put;
                }
                // SHEATHE: drop the drawn state, leave the weapon equipped. Only out of
                // combat -- a stand-down that raced a fresh fight must not put a drawn
                // weapon away (the engine draws it again anyway, but forcing the graph
                // the other way mid-engagement is not ours to do).
                //
                // A TENSION, STATED RATHER THAN BURIED (`Docs/GAMBIT_FLAIR.md:299`
                // rejected a post-combat sheathe flourish with "sheathing is the
                // vanilla AI's; sending sheath/pose animation events against it is a
                // tug-of-war with the engine"). This is NOT that: it is the engine's
                // own `Actor::DrawWeaponMagicHands(false)` (vfunc 0A6, pinned
                // `include/RE/A/Actor.h:360`, the same call `Loadout.cpp:341/429` and
                // `Probe.cpp:270` already make on a follower), not a graph event, and
                // it is only made OUT OF COMBAT where the vanilla AI wants the weapon
                // sheathed anyway -- so it agrees with the AI instead of contending
                // with it. It is also NOT what delivers marth's ask: the RE-ARM above
                // is (an equipped weapon is drawn on the body, an unequipped one is
                // not). If this one line ever proves to fight the AI, deleting it
                // costs nothing and the fix still stands.
                if (!actor->IsInCombat()) actor->DrawWeaponMagicHands(false);
                if (put)
                    spdlog::info("[equip] {:08X}: stand-down re-arm -- {} weapon(s) put back NON-forced "
                                 "and sheathed (the hold is over; the AI owns them again)", id, put);
            };
            MainThread::Post([reArm]() { MainThread::Post(reArm); });
        }
        // APMF ch.15: hands are free again -- release the equipment claim too, so
        // MFO's own EquipGateThunk re-enforces immediately if APMF is absent (no-op
        // if APMF is absent/off or no claim was ever made). This is the single choke
        // point every release path (Reconcile's release branch, Followers.cpp/
        // Scheduler.cpp teardown) funnels through.
        APMFBridge::ReleaseEquipment(id);
        spdlog::info("[equip] {:08X}: force-hold released{}", id,
                     hold.left ? " (both hands)" : "");
        if (hold.left) LogLeftHandReadback(id);
    }

    // ── THE LEFT HAND YIELDS TO A CAST (2026-09-13) ─────────────────────────
    // Drop ONLY the dual-wield left hold (force-unequip clears its prevent-
    // removal lock; ForcedHold::left = null; the right hold stays). Callers:
    // CastOn's commitPreempt the instant a plan takes the left -- BEFORE
    // Prepare's EquipSpell would meet the lock -- and ReconcileForcedWeapon when
    // a claim/lock is seen live on the left (the ComposedCast heal path never
    // passes CastOn). Re-arms the top-up so the hand refills once the cast's
    // lock lapses. A left-ONLY hold (AI-equipped right) erases the entry and
    // releases the APMF equipment claim, so the claim never outlives the hold.
    // Idempotent. Worker-serial; map under the lock, engine call outside it.
    bool YieldForcedLeftHand(RE::Actor* a_follower, const char* a_why) {
        if (!a_follower) return false;
        const auto id = a_follower->GetFormID();
        RE::TESBoundObject* left = nullptr;
        bool lastHold = false;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(id);
            if (it == g_forcedWeapon.end() || !it->second.left) return false;
            left = it->second.left;
            it->second.left = nullptr;
            if (it->second.Empty()) { g_forcedWeapon.erase(it); lastHold = true; }
        }
        // A FLOOR, not a reset (F1, principle 9): the top-up may refill the hand
        // no sooner than kOffHandRetry from this yield. Erasing the stamp let a
        // refused cast (yield -> EquipSpell -> refusal -> top-up -> yield...) cycle
        // the left hand weapon<->spell every lap; now it costs one per 5 s at most.
        g_offHandRetryAt[id] = std::chrono::steady_clock::now() + kOffHandRetry;
        // LEFT SLOT on the unequip (F4), mirroring the equip: a slot-less unequip
        // by object is unverified against a one-hander the engine could resolve
        // to the RIGHT hand, which would leave the left prevent-removal lock
        // standing and the hand unable to take a spell (heals are left-only).
        if (auto* mgr = RE::ActorEquipManager::GetSingleton())
            mgr->UnequipObject(a_follower, left, nullptr, 1, Loadout::LeftHandSlot(), true, true);
        if (lastHold) APMFBridge::ReleaseEquipment(id);
        spdlog::info("[equip] {:08X}: left-hand hold yielded ({}){}", id, a_why,
                     lastHold ? " -- no hold remains" : "");
        LogLeftHandReadback(id);
        return true;
    }

    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance, bool a_condKnownFalse) {
        if (!a_follower) return;
        // BOUND WEAPONS under the APMF equip authority (round 4): this runs every
        // in-combat lap for every serviced follower, hold or no hold -- the one
        // per-lap seat Actuation owns -- so it is where a Bound Sword/Bow the
        // follower just cast (MFO's gambit or the AI's own; neither passes
        // EquipWeapon) is noticed and DECLARED before the engine's BoundItemEffect
        // equip would be refused as off-set, and where its expiry drops it again.
        // One lap-to-lap compare of the live bound set; a change is ONE
        // re-declaration (send-on-change inside). Cheap: an active-effect scan,
        // no inventory walk, and only with the authority live.
        {
            const auto id = a_follower->GetFormID();
            if (Logistics::EquipAuthorityLive(id)) {
                auto bound = Logistics::LiveBoundWeapons(a_follower);
                auto& seen = g_boundSeen[id];
                if (seen != bound) {
                    seen = std::move(bound);
                    DeclareFromLedger(a_follower, seen.empty() ? "bound weapon ended" : "bound weapon live");
                }
            } else if (auto it = g_boundSeen.find(id); it != g_boundSeen.end()) {
                g_boundSeen.erase(it);
            }
        }
        // KEEP the hold while the feature is ON and an equip gambit of the forced
        // weapon's OWN category held THIS tick (a_wantStance, set by a fired-or-
        // satisfied equip). RELEASE only when we actually KNOW the condition is
        // false: a_condKnownFalse (the scan ran to completion — Evaluate returns
        // only MATCHING rules, so a scan STOPPED by a higher rule leaves the
        // equip's truth UNKNOWN; releasing then force-unequips the weapon mid-
        // fight the tick a heal/attack preempts — the SEV-2 churn). A category
        // mismatch under condKnownFalse also releases (a stale/flipped record).
        // Feature OFF -> release immediately, regardless of the scan. Decide
        // UNDER the lock, Release OUTSIDE it (Release re-locks -> no self-deadlock).
        // (2026-09-13) FIRST: a claim/lock live on the LEFT hand while the dual-
        // wield left hold survives -> the cast wins, the weapon yields (covers
        // the ComposedCast heal claim, which never passes commitPreempt). Decided
        // outside the lock; Yield locks itself. Ordered BEFORE the release/claim
        // decision so a yield that emptied a left-only entry reads as "nothing
        // forced" and no equipment claim is re-engaged for a dead hold.
        bool leftHeld = false;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(a_follower->GetFormID());
            leftHeld = it != g_forcedWeapon.end() && it->second.left != nullptr;
        }
        if (leftHeld && CastHandHeld(a_follower, kHandLeft))
            YieldForcedLeftHand(a_follower, "a cast claim is live on the left hand");
        bool release = false;
        RE::FormID heldWeaponForm = 0;   // captured under the lock; used outside it below
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(a_follower->GetFormID());
            if (it == g_forcedWeapon.end()) return;   // nothing forced -> nothing to do
            // The category of the hold: the right-hand weapon's, or -- a left-
            // only hold (AI-equipped right, MFO-held dual-wield left) -- the
            // left's, which is one-handed melee by construction.
            RE::TESBoundObject* primary = it->second.right ? it->second.right : it->second.left;
            if (!Config::g_weaponStyleControl.load()) {
                release = true;                        // kill-switch: always release
            } else if (a_condKnownFalse) {
                auto* w  = primary ? primary->As<RE::TESObjectWEAP>() : nullptr;
                const int cat = (w && (w->IsBow() || w->IsCrossbow())) ? 2 : 1;
                release = (a_wantStance != cat);       // condition confirmed false / flipped
            }
            // else: scan stopped early -> UNKNOWN -> keep the hold this tick.
            if (!release && primary) heldWeaponForm = primary->GetFormID();
        }
        if (release) {
            ReleaseForcedWeapon(a_follower);
        } else if (heldWeaponForm != 0) {
            // APMF ch.15: keep the equipment claim alive for as long as the
            // force-hold holds the hands. ClaimEquipment both ENGAGES it (the
            // first reconcile tick after EquipWeapon sets g_forcedWeapon) and
            // REFRESHES it (every tick after, same "the claim call also
            // refreshes" idiom ClaimOffenseCast uses) -- so there is never a tick
            // where the hold survives but the claim is left to expire under
            // APMFBridge's expiry backstop. That backstop is FacetExpiry()
            // (round-robin-aware), not the flat kExpiry -- this reconcile call is
            // itself one round-robin lap of Scheduler::Tick, same as cast-select,
            // and a flat 500ms sweep proved it could drop a still-wanted claim
            // (Cicero deck capture, 2026-09-05).
            APMFBridge::ClaimEquipment(a_follower->GetFormID(), heldWeaponForm);
        }
    }

    void ClearForcedWeapons() {
        g_offHandRetryAt.clear();   // worker-serial twin of the ledger (revert/load, #4 path)
        g_leftHeldRefusal.clear();
        g_lastLedgerDeclared.clear();   // round 4: the declaration road's ledger mirror + bound set
        g_boundSeen.clear();
        g_lastB41At.clear();
        std::scoped_lock lk(g_forcedMx);
        g_forcedWeapon.clear();
    }

    // T#76 force-hold co-save. Persist the force-equip locks so a load clears the
    // stale ones the .ess carried (the engine's forceEquip serializes; the map
    // does not). INDEPENDENT record — its own version guard + ResolveFormID/DROP.
    constexpr std::uint32_t kMaxForcedWeapons = 64;   // party is tiny; a generous cap

    void CoSaveForcedWeapons(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecForcedWeapon, kForcedWeaponVersion)) {
            spdlog::error("[cosave] OpenRecord('FWPN') failed -- force-holds NOT saved");
            return;
        }
        // SEV-1: SNAPSHOT under the lock, then write from the copy — a lock must
        // never be held across a WriteRecordData (the MSTK/CopyStockGear rule).
        std::vector<std::pair<RE::FormID, RE::FormID>> snap;   // {followerID, weaponID | 0}
        {
            std::scoped_lock lk(g_forcedMx);
            for (const auto& [id, hold] : g_forcedWeapon) {
                if (!Followers::IsPersistableID(id)) continue;   // #9: never write a 0xFF follower
                // LAYOUT UNCHANGED (v1, 2026-09-13): a dual-wield hold is TWO
                // (follower, weapon) pairs, one per held hand -- CoLoad has always
                // released each pair by OBJECT (no slot), so a v1 reader of either
                // vintage clears both. No version bump, no new reader.
                for (RE::TESBoundObject* w : { hold.right, hold.left }) {
                    if (!w) continue;
                    const RE::FormID wid = w->GetFormID();
                    // #9 for the WEAPON too: a player-enchanted/created weapon has a
                    // 0xFF id that ResolveFormID passes through unresolved -> the load
                    // would unequip the WRONG object and the real lock would survive.
                    // Persist weapon=0; CoLoad then sweeps BOTH hands to clear it.
                    snap.emplace_back(id, Followers::IsPersistableID(wid) ? wid : 0u);
                }
            }
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(snap.size()));
        for (const auto& [id, wid] : snap) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(wid);
        }
        spdlog::info("[cosave] saved {} force-hold(s), schema v{}", snap.size(), kForcedWeaponVersion);
    }

    void CoLoadForcedWeapons(SKSE::SerializationInterface* a_intfc, std::uint32_t /*a_version*/) {
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) return;
        if (count > kMaxForcedWeapons) {
            spdlog::error("[cosave] implausible force-hold count {} -- ABORTING FWPN load", count);
            return;
        }
        std::uint32_t released = 0, dropped = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID rawFollower = 0, rawWeapon = 0;
            if (!a_intfc->ReadRecordData(rawFollower)) return;
            if (!a_intfc->ReadRecordData(rawWeapon))   return;
            RE::FormID follower = 0, weapon = 0;
            if (!a_intfc->ResolveFormID(rawFollower, follower)) { ++dropped; continue; }   // #8 DROP
            // weapon==0 means "was a created/enchanted (0xFF) weapon at save" —
            // resolve only a real id; anything unresolvable falls to the both-
            // hands sweep so a stale lock still clears.
            if (rawWeapon != 0 && !a_intfc->ResolveFormID(rawWeapon, weapon)) weapon = 0;
            // The lock lives in the actor's inventory extra-data, not the map, so
            // clear it on the follower. Defer to the main thread (the load callback
            // is off it and the actor may not be 3D-loaded yet; the force-unequip
            // is an inventory op). NO map repopulation / erase — the map is empty
            // post-revert, so touching it here would be the only cross-thread
            // access (SEV-1). A session starts hold-free; the equip gambit
            // re-forces next combat if its condition still holds.
            MainThread::Post([follower, weapon]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(follower);
                if (!actor) return;
                auto* mgr = RE::ActorEquipManager::GetSingleton();
                if (!mgr) return;
                if (weapon != 0) {
                    if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(weapon)) {
                        // The pair carries no hand (layout v1), so name the slot
                        // from the LIVE hands (F4): a form held in the LEFT is
                        // unequipped with the left slot, one in the RIGHT with the
                        // default -- BOTH when the same form sits in each hand (a
                        // count>=2 same-form dual hold). Held in neither: the
                        // slot-less call by object, exactly as before.
                        const bool inLeft  = actor->GetEquippedObject(true)  == obj;
                        const bool inRight = actor->GetEquippedObject(false) == obj;
                        if (inLeft)  mgr->UnequipObject(actor, obj, nullptr, 1, Loadout::LeftHandSlot(), true, true);
                        if (inRight) mgr->UnequipObject(actor, obj, nullptr, 1, nullptr, true, true);
                        if (!inLeft && !inRight)
                            mgr->UnequipObject(actor, obj, nullptr, 1, nullptr, true, true);
                    }
                } else {
                    // Unknown weapon (created/enchanted): force-unequip the held
                    // WEAPON in either hand to clear the lock. #8: the force-hold
                    // only ever locks a weapon, but GetEquippedObject also returns
                    // spells, shields and torches -- unequipping one of those would
                    // strip an unrelated off-hand item on load. Weapons only. The
                    // left hand names its slot (F4).
                    for (bool leftHand : { false, true }) {
                        if (auto* held = actor->GetEquippedObject(leftHand))
                            if (auto* wep = held->As<RE::TESObjectWEAP>())
                                mgr->UnequipObject(actor, wep, nullptr, 1,
                                                   leftHand ? Loadout::LeftHandSlot() : nullptr, true, true);
                    }
                }
                spdlog::info("[equip] {:08X}: stale force-hold cleared on load", follower);
            });
            ++released;
        }
        spdlog::info("[cosave] loaded {} force-hold(s) to clear on load ({} dropped unresolvable)",
                     released, dropped);
    }

}
