// cast/EquipWeigh.cpp -- WEIGHTING PHASE 1, W1 (ClickUp 86e3jwmk7; Docs/WEIGHTING-INVENTORY.md "Phase 1
// plan" and "Decisions"). marth 2026-10-12: "Direct equip is going. Harbinger has all the
// responsibility now." With Harbinger at ABI >= 22 (APMFBridge::EquipPreferenceSupported) MFO makes NO
// direct weapon / shield / torch equip. It DECLARES:
//
//   1. THE PREFERENCE (ch.26, kIntent_EquipPreference): in combat, the weapons the force-hold ledger
//      holds (FWPN, unchanged) and the shield-by-perks top-up, per hand, rank 0. Harbinger lifts their
//      CalculateScore one step above the best competitor (D1) and the AI equips them by its own rules
//      (range gate, order table, hand occupancy). D3: a hand MFO's own cast claim holds carries only
//      the spell, so no weapon is declared for it. MFO-B55 "unknown, not false" and the silver 2 s
//      dwell keep deciding the LEDGER, so they now decide what is DECLARED.
//   2. THE PLACEMENT (ch.17, the equip authority's own equip pass): the ledger's hands as before
//      (RefreshEquipDeclaration rule 1) plus ONE-SHOT PLACEMENTS for every equip that used to be a
//      direct EquipObject (the shield top-up, the torch, the silver switch-back, the stand-down
//      re-arm, the out-of-combat weapon upgrade, the excluded-weapon heal, the cast's gear give-back).
//      A placement is declared (its hand owned) until it is seen in the hand, the item is gone, or
//      kPlaceTtl passes (a loud "did not land" line: principle 7, nothing retries around it).
//   3. THE ESCALATION (D4): only after the preference has not won for about 2 s IN COMBAT, measured
//      from GetEquipPreferenceState (Lifted / Leading = Harbinger lifted it or it already leads, and
//      the item is still not in its hand), MFO sets kEquipPref_Force and re-places once through
//      ch.17. Every escalation is logged; one that would violate Harbinger's priority sliders
//      (Outranked) is NOT sent and is logged as an ANOMALY to investigate. The force bit is cleared
//      once the item has held for kForceClearAfter, and out of combat.
//
// THREADING. The ledger reads take g_forcedMx (ForcedHoldFor / ShieldTopUpFor). The placement table
// has its own mutex (WantPlacement is reached from the hit sink's AddTask body and from the post-load
// Loadout::Reconcile on the main thread as well as the worker). The escalation watch is worker-serial
// (#4): only ServicePreference (the Scheduler's in-combat reconcile lap) and ClearWeighing (revert,
// drained pump) touch it. No engine write is made here; the engine READS (GetEquippedObject,
// GetInventory, the combat target) are the worker reads Equip.cpp already makes.
#include "Actuation_internal.h"
#include "apmf/APMFBridge.h"

#include <chrono>
#include <mutex>
#include <unordered_set>

namespace MFO::Actuation {

    namespace {
        using Clock = std::chrono::steady_clock;

        // A placement Harbinger's ch.17 equip pass has not put in the hand after this long did not land
        // (its pass is one main-thread hop per declaration; the field once saw a posted pass run 85 s late,
        // MFO-B63, so this is a FLOOR for the log line, sized well past one declaration's round trip and
        // the logistics cadence of ~1 s; principle 9).
        constexpr auto kPlaceTtl = std::chrono::seconds(10);
        // ... counted from the first declaration that CARRIED it (a placement held back because its hand is
        // the ledger's or a cast's is waiting, not failing). A placement never declared in this long is
        // dropped (logged): its hand stayed taken the whole time.
        constexpr auto kPlaceWaitCap = std::chrono::seconds(120);
        // D4: "force after about 2 s" = two engine rescore passes (CombatInventory::Update re-scores about
        // once a second, WEIGHTING-INVENTORY "Rescore cadence").
        constexpr auto kEscalateAfter = std::chrono::seconds(2);
        // Forced and still missing after this much more: said once as an error (principle 7), never retried.
        constexpr auto kForcedMissReport = std::chrono::seconds(4);
        // The force bit comes off once the preferred items have held this long (Harbinger: "Clear the bit once
        // the item holds"). A floor, so one lap of the item in hand does not flap the bit.
        constexpr auto kForceClearAfter = std::chrono::seconds(5);
        // State lines: on a change, at most one per follower per kStateLogFloor; a heartbeat line per
        // follower every kHeartbeat while a preference stands in combat (the LoreRim read-back).
        constexpr auto kStateLogFloor = std::chrono::seconds(2);
        constexpr auto kHeartbeat     = std::chrono::seconds(30);
        // Harbinger reads msSinceScored; a scoring older than this is not a live lift (the engine rescores
        // about once a second, so 3 s is three missed passes).
        constexpr std::uint32_t kLiveScoreMs = 3000;

        struct Wish {
            RE::FormID   form = 0;
            std::uint8_t slot = APMF_API::kEquipSlot_Default;
            Clock::time_point at{};
            Clock::time_point declaredAt{};   // first declaration that carried it (0 = not yet)
            const char*  why  = "?";   // a string literal
        };
        std::mutex                                       g_placeMx;
        std::unordered_map<RE::FormID, std::vector<Wish>> g_place;   // guarded by g_placeMx
        std::unordered_set<std::uint64_t>                 g_skipLogged;   // (follower << 32 | form), guarded by g_placeMx
        // Consecutive did-NOT-land retirements per (follower << 32 | form); a landing erases it. Guarded by g_placeMx.
        std::unordered_map<std::uint64_t, std::uint32_t>  g_failStreak;
        // Followers whose empty ch.26 preference was said this combat episode (worker-serial, #4).
        std::unordered_set<RE::FormID>                    g_emptyPrefLogged;

        const char* NameOf(RE::FormID a_form) {
            const auto* f = a_form ? RE::TESForm::LookupByID(a_form) : nullptr;
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }
        const char* HandName(std::uint8_t a_slot) {
            return a_slot == APMF_API::kEquipSlot_Right ? "right" : a_slot == APMF_API::kEquipSlot_Left ? "left" : "any hand";
        }
        bool InHand(RE::Actor* a_actor, RE::FormID a_form, std::uint8_t a_slot) {
            if (!a_actor || !a_form) return false;
            const auto idOf = [](RE::TESForm* f) { return f ? f->GetFormID() : 0u; };
            const RE::FormID r = idOf(a_actor->GetEquippedObject(false));
            const RE::FormID l = idOf(a_actor->GetEquippedObject(true));
            if (a_slot == APMF_API::kEquipSlot_Right) return r == a_form;
            if (a_slot == APMF_API::kEquipSlot_Left)  return l == a_form;
            return r == a_form || l == a_form;
        }
        bool OneHanded(const RE::TESObjectWEAP* a_w) {
            return a_w && (a_w->IsOneHandedSword() || a_w->IsOneHandedDagger() || a_w->IsOneHandedAxe() ||
                           a_w->IsOneHandedMace() || a_w->IsStaff());
        }
        bool Launcher(const RE::TESForm* a_form) {
            const auto* w = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
            return w && (w->IsBow() || w->IsCrossbow());
        }

        // D3: does MFO's OWN live cast claim hold this hand? Exactly the claims Harbinger's HandHeld reads
        // for MFO (a driving offense or heal claim on that hand; a dual-cast heal holds the right too).
        bool ClaimHolds(RE::FormID a_fid, std::size_t a_hand) {
            const std::int32_t h = (a_hand == kHandLeft) ? APMFBridge::kApmfHandLeft : APMFBridge::kApmfHandRight;
            return APMFBridge::IsOwnedCastActiveOnHand(a_fid, h) || APMFBridge::IsHealCastActiveOnHand(a_fid, h) ||
                   (a_hand == kHandRight && APMFBridge::IsHealCastDual(a_fid));
        }

        const char* StateName(std::uint32_t a_s) {
            switch (a_s) {
            case APMF_API::kEquipPrefState_None:       return "None";
            case APMF_API::kEquipPrefState_Declared:   return "Declared";
            case APMF_API::kEquipPrefState_Lifted:     return "Lifted";
            case APMF_API::kEquipPrefState_Leading:    return "Leading";
            case APMF_API::kEquipPrefState_ZeroScore:  return "ZeroScore";
            case APMF_API::kEquipPrefState_HandHeld:   return "HandHeld";
            case APMF_API::kEquipPrefState_Outranked:  return "Outranked";
            case APMF_API::kEquipPrefState_Unreadable: return "Unreadable";
            case APMF_API::kEquipPrefState_Released:   return "Released";
            default:                                   return "?";
            }
        }

        // The escalation watch, per follower. Worker-serial (#4).
        struct Watch {
            std::vector<APMF_API::APMF_EquipPrefEntry> watched;   // the declaration this episode watches
            Clock::time_point missSince{};      // first lap of the current scored miss (0 = not missing)
            Clock::time_point heldSince{};      // first lap every preferred item was in its hand
            Clock::time_point escalatedAt{};    // when the force bit went on (0 = not this episode)
            bool              forcedMissLogged = false;
            bool              anomalyLogged    = false;   // Outranked / Unreadable said for this episode
            bool              rangeLogged      = false;   // the range-gate skip said for this episode
            bool              unclassifiedLogged = false; // the per-actor-state gap said for this episode
            std::uint32_t     lastState = 0xFFFFFFFFu;
            RE::FormID        lastForm  = 0;
            Clock::time_point lastStateLog{};
            Clock::time_point lastBeat{};
            std::uint32_t     lifts = 0, withheld = 0, forcedDenies = 0, anomalies = 0;   // last seen counters
        };
        std::unordered_map<RE::FormID, Watch> g_watch;

        bool SameDecl(const std::vector<APMF_API::APMF_EquipPrefEntry>& a,
                      const std::vector<APMF_API::APMF_EquipPrefEntry>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i].form != b[i].form || a[i].slot != b[i].slot) return false;
            return true;
        }

        // The preference as the ledger stands NOW (ch.26 entries). Empty = nothing preferred.
        std::vector<APMF_API::APMF_EquipPrefEntry> BuildPreference(RE::Actor* a_follower) {
            std::vector<APMF_API::APMF_EquipPrefEntry> out;
            const RE::FormID fid = a_follower->GetFormID();
            const auto [right, left] = ForcedHoldFor(fid);
            const RE::FormID shield  = ShieldTopUpFor(fid);
            const bool rightHeld = ClaimHolds(fid, kHandRight);
            const bool leftHeld  = ClaimHolds(fid, kHandLeft);
            bool rightBoth = false;
            if (right) {
                auto* w = RE::TESForm::LookupByID<RE::TESObjectWEAP>(right);
                const bool one = OneHanded(w);
                rightBoth = w && !one;
                // D3: a one-hander needs its right hand free of a cast claim; a two-hander / bow both hands.
                if (w && !(one ? rightHeld : (rightHeld || leftHeld)))
                    out.push_back({ right, static_cast<std::uint8_t>(one ? APMF_API::kEquipSlot_Right : APMF_API::kEquipSlot_Default), 0, { 0, 0 } });
            }
            if (left && !rightBoth && !leftHeld)
                out.push_back({ left, static_cast<std::uint8_t>(APMF_API::kEquipSlot_Left), 0, { 0, 0 } });
            // The shield-by-perks top-up is a LEFT-hand item (Harbinger's category 3). kEquipSlot_Left, never
            // Default: a Default entry covers BOTH hands, so under kEquipPref_Force it would refuse the AI's
            // right-hand weapons too (review of e06a54f, SEV-4).
            if (shield && !left && !rightBoth && !leftHeld)
                out.push_back({ shield, static_cast<std::uint8_t>(APMF_API::kEquipSlot_Left), 0, { 0, 0 } });
            return out;
        }
    }

    bool WeighingEquips() { return Config::g_weighWeapons.load() && APMFBridge::EquipPreferenceSupported(); }

    // bWeighWeapons flipped OFF at runtime (Config::Read on MCM close): nothing may stay half-weighted. Drops
    // the pending ch.17 placements, the escalation watch and every ch.26 claim. A no-op while weighing is on
    // or when there is nothing to drop. Worker-serial (the MCM-close task), like g_watch.
    void ReconcileWeighSwitch() {
        if (WeighingEquips()) return;
        bool had = false;
        std::vector<RE::FormID> held;   // followers whose last ch.17 declaration may still carry a placement
        {
            std::scoped_lock lk(g_placeMx);
            had = !g_place.empty();
            for (const auto& [fid, v] : g_place)
                if (!v.empty()) held.push_back(fid);
            g_place.clear();
            g_skipLogged.clear();
            g_failStreak.clear();
        }
        // Re-send each affected follower's declaration WITHOUT the dropped placement, or Harbinger keeps owning
        // Shield|Left / Light|Left and, with ServicePreference now a no-op, nothing rebuilds it in combat.
        for (const RE::FormID fid : held) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!actor || !actor->Is3DLoaded()) continue;
            Logistics::ResendEquipDeclaration(fid);
            DeclareLedger(actor, "weighting off");
        }
        had = had || !g_watch.empty();
        g_watch.clear();
        g_emptyPrefLogged.clear();
        APMFBridge::ClearEquipPreferences();
        if (had) spdlog::info("[equip] weapon weighting switched OFF: ch.26 claims released, pending placements dropped");
    }

    std::uint8_t PlacementSlotFor(const RE::TESForm* a_item, bool a_leftHand) {
        const auto* w = a_item ? a_item->As<RE::TESObjectWEAP>() : nullptr;
        if (w && OneHanded(w)) return a_leftHand ? APMF_API::kEquipSlot_Left : APMF_API::kEquipSlot_Right;
        // A shield or a torch is a LEFT-hand item: Harbinger categorizes it Shield|Left / Light|Left, and its
        // ch.17 pass places it only with Left owned and declared kEquipSlot_Left (FACETS ch.17; review SEV-2).
        if (const auto* ar = a_item ? a_item->As<RE::TESObjectARMO>() : nullptr; ar && ar->IsShield())
            return APMF_API::kEquipSlot_Left;
        if (a_item && a_item->As<RE::TESObjectLIGH>()) return APMF_API::kEquipSlot_Left;
        return APMF_API::kEquipSlot_Default;   // a two-hander, a bow
    }

    void WantPlacement(RE::FormID a_follower, RE::FormID a_form, std::uint8_t a_slot, const char* a_why) {
        if (a_follower == 0 || a_form == 0) return;
        {
            std::scoped_lock lk(g_placeMx);
            auto& v = g_place[a_follower];
            // IDEMPOTENT: the same item for the same hand already waiting is kept as it is (its clocks are not
            // reset and nothing is logged), so a road that asks again every lap until the item lands (a Fired
            // equip gambit, the 5 s excluded-weapon heal) neither spams the log nor keeps a failed placement alive.
            for (const auto& w : v)
                if (w.form == a_form && w.slot == a_slot) return;
            // One wish per form and one per named hand: the newest wins.
            std::erase_if(v, [&](const Wish& w) {
                return w.form == a_form || (a_slot != APMF_API::kEquipSlot_Default && w.slot == a_slot);
            });
            v.push_back({ a_form, a_slot, Clock::now(), {}, a_why ? a_why : "?" });
        }
        spdlog::info("[equip-place] {:08X}: '{}' -> {} through Harbinger's ch.17 equip pass (no direct equip; {})",
                     a_follower, NameOf(a_form), HandName(a_slot), a_why ? a_why : "?");
    }

    bool PlacementPending(RE::FormID a_follower) {
        std::scoped_lock lk(g_placeMx);
        const auto it = g_place.find(a_follower);
        return it != g_place.end() && !it->second.empty();
    }

    std::vector<Placement> PendingPlacements(RE::Actor* a_follower) {
        std::vector<Placement> out;
        if (!a_follower) return out;
        const RE::FormID fid = a_follower->GetFormID();
        std::vector<Wish> wishes;
        {
            std::scoped_lock lk(g_placeMx);
            const auto it = g_place.find(fid);
            if (it == g_place.end()) return out;
            wishes = it->second;
        }
        // Engine reads OUTSIDE the lock. A wish is done when it LANDED (in its hand), the item is no longer
        // his, or it did not land within kPlaceTtl (said loudly, never retried around: principle 7).
        const auto now  = Clock::now();
        const bool dead = a_follower->IsDead();
        auto inv = a_follower->GetInventory();
        std::vector<RE::FormID> done;
        for (const auto& w : wishes) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(w.form);
            const auto iit = obj ? inv.find(obj) : inv.end();
            const bool owned = iit != inv.end() && iit->second.first > 0;
            if (InHand(a_follower, w.form, w.slot)) {
                spdlog::info("[equip-place] {:08X}: '{}' landed in the {} ({:.1f} s; {})", fid, NameOf(w.form),
                             HandName(w.slot), std::chrono::duration<float>(now - w.at).count(), w.why);
                {
                    std::scoped_lock lk(g_placeMx);
                    g_failStreak.erase((static_cast<std::uint64_t>(fid) << 32) | w.form);
                }
                done.push_back(w.form);
            } else if (!owned || dead) {
                spdlog::info("[equip-place] {:08X}: '{}' dropped -- {} ({})", fid, NameOf(w.form),
                             dead ? "he is dead" : "he no longer owns it", w.why);
                done.push_back(w.form);
            } else if (w.declaredAt == Clock::time_point{} && now - w.at >= kPlaceWaitCap) {
                spdlog::warn("[equip-place] {:08X}: '{}' dropped -- never declared in {} s (its hand stayed held by "
                             "the ledger or a cast; {})", fid, NameOf(w.form),
                             std::chrono::duration_cast<std::chrono::seconds>(kPlaceWaitCap).count(), w.why);
                done.push_back(w.form);
            } else if (w.declaredAt != Clock::time_point{} && now - w.declaredAt >= kPlaceTtl) {
                std::uint32_t attempt;
                {
                    std::scoped_lock lk(g_placeMx);
                    attempt = ++g_failStreak[(static_cast<std::uint64_t>(fid) << 32) | w.form];
                }
                spdlog::warn("[equip-place] {:08X}: '{}' did NOT land in the {} within {} s ({}), attempt {} in a row -- "
                             "not seen in the hand at any check within that time (Harbinger may have placed it and "
                             "something removed it: see APMF.log [equip-obs] op=unequip / [equip-auth] readback). Not "
                             "retried here, MFO makes no direct equip.", fid, NameOf(w.form), HandName(w.slot),
                             std::chrono::duration_cast<std::chrono::seconds>(kPlaceTtl).count(), w.why, attempt);
                done.push_back(w.form);
            } else {
                out.push_back({ w.form, w.slot });
            }
        }
        if (!done.empty()) {
            std::scoped_lock lk(g_placeMx);
            if (auto it = g_place.find(fid); it != g_place.end()) {
                std::erase_if(it->second, [&](const Wish& w) {
                    return std::find(done.begin(), done.end(), w.form) != done.end();
                });
                if (it->second.empty()) g_place.erase(it);
            }
        }
        return out;
    }

    void NotePlacementDeclared(RE::FormID a_follower, RE::FormID a_form) {
        std::scoped_lock lk(g_placeMx);
        const auto it = g_place.find(a_follower);
        if (it == g_place.end()) return;
        for (auto& w : it->second)
            if (w.form == a_form && w.declaredAt == Clock::time_point{}) w.declaredAt = Clock::now();
    }

    bool PlacementSkipFirstLog(RE::FormID a_follower, RE::FormID a_form) {
        std::scoped_lock lk(g_placeMx);
        return g_skipLogged.insert((static_cast<std::uint64_t>(a_follower) << 32) | a_form).second;
    }

    void ForgetPlacements(RE::FormID a_follower) {
        {
            std::scoped_lock lk(g_placeMx);
            g_place.erase(a_follower);
            std::erase_if(g_skipLogged, [a_follower](std::uint64_t k) { return (k >> 32) == a_follower; });
            std::erase_if(g_failStreak, [a_follower](const auto& kv) { return (kv.first >> 32) == a_follower; });
        }
        g_emptyPrefLogged.erase(a_follower);
        g_watch.erase(a_follower);   // worker: OnFollowerRemoved runs on the worker (Followers::ReleaseHeldState)
    }

    void NoteCombatEpisodeEnded(RE::FormID a_follower) {
        g_emptyPrefLogged.erase(a_follower);   // worker-serial, like g_watch
    }

    void ClearWeighing() {
        {
            std::scoped_lock lk(g_placeMx);
            g_place.clear();
            g_skipLogged.clear();
            g_failStreak.clear();
        }
        g_emptyPrefLogged.clear();
        g_watch.clear();
    }

    void RefreshPreference(RE::Actor* a_follower, const char* a_why) {
        if (!a_follower || !WeighingEquips()) return;
        APMFBridge::DeclareEquipPreference(a_follower->GetFormID(), BuildPreference(a_follower), a_why);
    }

    void ServicePreference(RE::Actor* a_follower) {
        if (!a_follower || !WeighingEquips()) return;
        const RE::FormID fid = a_follower->GetFormID();
        // A placement still waiting is re-declared until it lands (the set is unchanged, so the rebuild sends
        // nothing; PendingPlacements inside it retires the landed / expired ones and the next build drops them).
        if (PlacementPending(fid)) DeclareLedger(a_follower, "placement pending");

        const auto entries = BuildPreference(a_follower);
        APMFBridge::DeclareEquipPreference(fid, entries, "combat lap");
        auto& w = g_watch[fid];
        const auto now = Clock::now();
        if (entries.empty()) {
            g_watch.erase(fid);
            if (!a_follower->IsInCombat()) g_emptyPrefLogged.erase(fid);   // the episode ended
            else if (g_emptyPrefLogged.insert(fid).second)
                spdlog::info("[equip-pref] {:08X}: ch.26: nothing to prefer (no gambit weapon hold, no shield top-up) "
                             "-- the engine picks", fid);
            return;
        }
        g_emptyPrefLogged.erase(fid);
        if (!SameDecl(w.watched, entries)) {   // a new declaration is a new episode
            const bool forced = APMFBridge::EquipPreferenceForced(fid);
            w = Watch{};
            w.watched = entries;
            if (forced) w.escalatedAt = now;   // the bit survives a re-declaration on the same claim
        }
        const bool inCombat = a_follower->IsInCombat();
        bool allHeld = true;
        RE::FormID missForm = 0; std::uint8_t missSlot = 0;
        for (const auto& e : entries)
            if (!InHand(a_follower, e.form, e.slot)) { allHeld = false; if (!missForm) { missForm = e.form; missSlot = e.slot; } }

        APMF_API::APMF_EquipPrefInfo info{};
        const std::uint32_t state = APMFBridge::EquipPreferenceState(fid, &info);
        const auto handName = [](std::uint32_t s) { return s == APMF_API::kEquipSlot_Right ? "R" : s == APMF_API::kEquipSlot_Left ? "L" : "any"; };
        const auto heldNames = [&]() {
            auto* r = a_follower->GetEquippedObject(false);
            auto* l = a_follower->GetEquippedObject(true);
            return std::format("R='{}' L='{}'", r && r->GetName() ? r->GetName() : "-", l && l->GetName() ? l->GetName() : "-");
        };
        // Harbinger's counters: a rise in force anomalies is ITS ranked refusal standing aside for an
        // outranking source (D4: an anomaly to investigate), said at warn every time it moves.
        if (info.forceAnomalies > w.anomalies)
            spdlog::warn("[equip-pref] {:08X}: FORCE ANOMALY x{} -- a source that outranks MFO on the Equip facet kept "
                         "its equip under kEquipPref_Force (Harbinger's sliders; investigate APMF.log [ch.26])", fid,
                         info.forceAnomalies - w.anomalies);
        // STATE LINE: on a change of state or item (floor kStateLogFloor), and a heartbeat every kHeartbeat.
        const bool changed = state != w.lastState || info.lastForm != w.lastForm;
        if (inCombat && ((changed && now - w.lastStateLog >= kStateLogFloor) || now - w.lastBeat >= kHeartbeat)) {
            spdlog::info("[equip-pref] {:08X}: {} '{}' hand {} engine={:.2f} rival={:.2f} -> {:.2f} ({} ms ago) | "
                         "lifts={} (+{}) withheld={} (+{}) forcedDenies={} anomalies={} force={} | {}{}", fid,
                         StateName(state), NameOf(info.lastForm), handName(info.lastSlot), info.lastEngineScore,
                         info.lastRivalScore, info.lastResultScore,
                         info.msSinceScored == 0xFFFFFFFFu ? std::string("never") : std::to_string(info.msSinceScored),
                         info.lifts, info.lifts - std::min(info.lifts, w.lifts), info.withheld,
                         info.withheld - std::min(info.withheld, w.withheld), info.forcedDenies, info.forceAnomalies,
                         APMFBridge::EquipPreferenceForced(fid) ? "ON" : "off", heldNames(),
                         allHeld ? "" : std::format(" | MISSING '{}' ({})", NameOf(missForm), handName(missSlot)));
            w.lastStateLog = now;
            w.lastBeat     = now;
        }
        w.lastState = state; w.lastForm = info.lastForm;
        w.lifts = info.lifts; w.withheld = info.withheld; w.forcedDenies = info.forcedDenies; w.anomalies = info.forceAnomalies;

        // OUT OF COMBAT the scorer does not run: no miss, no escalation, and a force bit comes off.
        if (!inCombat) {
            w.missSince = {}; w.heldSince = {};
            if (APMFBridge::SetEquipPreferenceForce(fid, false))
                spdlog::info("[equip-pref] {:08X}: force cleared -- out of combat (the score pipeline does not run)", fid);
            w.escalatedAt = {};
            return;
        }
        if (allHeld) {
            w.missSince = {};
            if (w.heldSince == Clock::time_point{}) w.heldSince = now;
            if (now - w.heldSince >= kForceClearAfter && APMFBridge::SetEquipPreferenceForce(fid, false)) {
                spdlog::info("[equip-pref] {:08X}: force cleared -- the preferred item(s) held {:.1f} s (D4 de-escalation)",
                             fid, std::chrono::duration<float>(now - w.heldSince).count());
                w.escalatedAt = {}; w.forcedMissLogged = false;
            }
            return;
        }
        w.heldSince = {};
        // A MISS. Which kind decides whether D4 may escalate.
        // (1) Harbinger's state is PER ACTOR: it describes the item it last scored (info.lastForm). Only a state
        //     about THE MISSING ITEM classifies this miss (review of e06a54f, SEV-3); another item's Lifted /
        //     Leading says nothing about why this one is not in its hand.
        //     KNOWN GAP (re-review of 67722ac, SEV-3): Harbinger's Record overwrites lastForm / state on every
        //     scoring and a pass scores the declared entries back to back, so with two entries the reported one is
        //     nearly always the LAST scored and D4 may never classify the other. Said once per episode, never
        //     masked; the per-entry state read is a Harbinger ABI request (Docs/REVIEW-BACKLOG.md MFO-B262).
        if (info.lastForm != missForm) {
            if (!w.unclassifiedLogged) {
                w.unclassifiedLogged = true;
                spdlog::info("[equip-pref] {:08X}: miss of '{}' not classified -- Harbinger reports only '{}' "
                             "(per-actor state); no D4 for it", fid, NameOf(missForm), NameOf(info.lastForm));
            }
            w.missSince = {};
            return;
        }
        // (2) A SPELL in the missing item's hand is a CROSS-CATEGORY pick (a Restore / Ward / buff spell is walked
        //     before the weapons, the order table, not a score): not a scored miss, and the ch.17 re-placement
        //     would clobber the AI's heal mid-cast (review of e06a54f, SEV-3).
        {
            const auto spellIn = [&](bool a_left) {
                auto* f = a_follower->GetEquippedObject(a_left);
                return f && f->As<RE::SpellItem>() != nullptr;
            };
            const bool spell = missSlot == APMF_API::kEquipSlot_Right ? spellIn(false)
                             : missSlot == APMF_API::kEquipSlot_Left  ? spellIn(true)
                                                                       : (spellIn(false) || spellIn(true));
            if (spell) { w.missSince = {}; return; }
        }
        switch (state) {
        case APMF_API::kEquipPrefState_Lifted:
        case APMF_API::kEquipPrefState_Leading:
            break;   // Harbinger lifted it (or it already leads) and it is still not in its hand: the D4 clock runs
        case APMF_API::kEquipPrefState_Outranked:
            // Forcing here would override a source the sliders rank above MFO: D4 says that is an anomaly
            // to log and investigate, never an escalation. Not sent.
            if (!w.anomalyLogged) {
                w.anomalyLogged = true;
                spdlog::warn("[equip-pref] {:08X}: ANOMALY -- '{}' ({}) is OUTRANKED inside the score seat (withheld={}): "
                             "a source above MFO's Equip priority decides this hand. NO escalation (it would violate "
                             "Harbinger's priority sliders); investigate APMF.log [ch.26] | {}", fid, NameOf(missForm),
                             handName(missSlot), info.withheld, heldNames());
            }
            w.missSince = {};
            return;
        case APMF_API::kEquipPrefState_Unreadable:
            if (!w.anomalyLogged) {
                w.anomalyLogged = true;
                spdlog::error("[equip-pref] {:08X}: Harbinger could not read the engine's category list for '{}' "
                              "(Unreadable) -- no lift, no escalation (APMF.log says why)", fid, NameOf(missForm));
            }
            w.missSince = {};
            return;
        default:
            // ZeroScore (unusable now: a bow with no arrows -- the engine is right), HandHeld (D3: a cast holds
            // the hand), Declared / None / Released (not scored: not in the engine's combat list yet): not a
            // scored miss, the clock does not run.
            w.missSince = {};
            return;
        }
        if (info.msSinceScored > kLiveScoreMs) { w.missSince = {}; return; }   // no live scoring: not a scored miss
        if (w.missSince == Clock::time_point{}) w.missSince = now;
        const float missFor = std::chrono::duration<float>(now - w.missSince).count();
        // THE RANGE GATE (WEIGHTING-INVENTORY W1 risk b): the engine's selector skips a top-scored item that is
        // out of range (0x813A2F), so a preferred bow with the foe inside melee reach is the engine being right,
        // not a miss to escalate on. Said once per episode.
        float tgtDist = -1.0f;
        if (auto tp = a_follower->GetActorRuntimeData().currentCombatTarget.get(); tp)
            tgtDist = a_follower->GetPosition().GetDistance(tp->GetPosition());
        if (Launcher(RE::TESForm::LookupByID(missForm)) && tgtDist >= 0.0f && tgtDist <= Config::g_meleeReach.load()) {
            if (!w.rangeLogged) {
                w.rangeLogged = true;
                spdlog::info("[equip-pref] {:08X}: '{}' preferred but the foe is {:.0f}u away (inside melee reach) -- the "
                             "engine's range gate skips a launcher here, not escalated", fid, NameOf(missForm), tgtDist);
            }
            w.missSince = {};
            return;
        }
        if (w.escalatedAt == Clock::time_point{}) {
            if (now - w.missSince < kEscalateAfter) return;
            if (!APMFBridge::SetEquipPreferenceForce(fid, true) && !APMFBridge::EquipPreferenceForced(fid)) {
                if (!w.anomalyLogged) {
                    w.anomalyLogged = true;
                    spdlog::error("[equip-pref] {:08X}: escalation for '{}' NOT sent -- no live ch.26 claim (refused?)",
                                  fid, NameOf(missForm));
                }
                return;
            }
            w.escalatedAt = now;
            spdlog::warn("[equip-pref] {:08X}: ESCALATE to kEquipPref_Force (D4) -- '{}' not in its {} hand for {:.1f} s "
                         "while Harbinger reads {} (engine={:.2f} rival={:.2f} -> {:.2f}); foe {:.0f}u; ch.17 re-placement "
                         "sent once | {}", fid, NameOf(missForm), handName(missSlot), missFor, StateName(state),
                         info.lastEngineScore, info.lastRivalScore, info.lastResultScore, tgtDist, heldNames());
            // The re-placement: Harbinger equips nothing for a preference, so the declared hands go out once more
            // through ch.17 (its equip pass places what he is not wearing). ONCE per episode: this is the bounded
            // replacement of the retired MFO-B41 re-declare loop.
            Logistics::ResendEquipDeclaration(fid);
            DeclareLedger(a_follower, "D4 escalation: re-place");
            return;
        }
        if (!w.forcedMissLogged && now - w.escalatedAt >= kForcedMissReport) {
            w.forcedMissLogged = true;
            spdlog::error("[equip-pref] {:08X}: FORCED and still missing -- '{}' not in its {} hand {:.1f} s after the "
                          "escalation (state {}, forcedDenies={}, anomalies={}). Not retried (principle 7) | {}", fid,
                          NameOf(missForm), handName(missSlot),
                          std::chrono::duration<float>(now - w.escalatedAt).count(), StateName(state),
                          info.forcedDenies, info.forceAnomalies, heldNames());
        }
    }

}
