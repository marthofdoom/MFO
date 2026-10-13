// cast/EquipBestFit.cpp -- the ch.26 ALWAYS-PREFER (marth 2026-10-12): MFO always sends Harbinger a weapon
// preference, its best-fit weapon for the follower, so a close call between two weapons is tipped toward the
// one his skills and perks point at even when no equip gambit fires. Gambit holds override it (BuildPreference in
// EquipWeigh.cpp), Harbinger lifts it one step above the engine's best rival so it only tips close calls, and it
// never lifts a 0-scored item. A NUDGE ONLY: never escalated (D4 watches the gambit entries alone), no ch.17
// placement. Pure decision here, no engine write.
//
// THE DIRECTION is the one the gem logic uses (Logistics::CombatBestFitFor over ComputeWeaponRoles + StyleVotesFor,
// the same answers GemReconcilePrefsFor reads), so a preferred weapon and the gems socketed into it agree:
//   class   the higher melee skill (1H vs 2H) against the ranged skill, a tie by owned perk ranks, then melee;
//   kind    inside the class the perk-preferred kinds bias WeaponScore (x1.5) exactly as the combat pick does;
//   rating  Logistics::CombatWeaponScore / CombatRangedScore -- the combat pick's own judge (damage x bias, and
//           an ammo tier for a launcher), `>=` so the last equal wins as there;
//   left    a shield / a second one-hander only when the perks vote for it (roles.offHand), through the combat
//           pick's own PickShield / PickOffHandWeapon, gated on bWeaponStyleControl like the combat pick.
// A launcher is chosen only with matching ammo carried. Excluded: staves, non-playable / creature / quest
// weapons (Logistics::CombatWeaponExcluded), a museum relic while any other weapon of the class is carried.
// CADENCE: the inventory is read at most once per kBestFitRefresh per follower; every other call returns the cache.
// THREADING: worker-serial (#4) -- g_followers, the inventory and the perk mirror are the reads the combat equip
// already makes on this thread. No co-save.
#include "Actuation_internal.h"
#include "ItemCatalog.h"
#include "apmf/APMFBridge.h"

#include <chrono>

namespace MFO::Actuation {

    namespace {
        using Clock = std::chrono::steady_clock;

        // Re-pick after this long. The perks and the inventory change slowly (a loot, a trade, a level), the engine
        // rescores about once a second, so a few seconds is far inside the useful life of a nudge (principle 9: a
        // stale pick is replaced, never an expiry that kills live state).
        constexpr auto kBestFitRefresh = std::chrono::seconds(5);

        struct Cached {
            Clock::time_point at{};
            BestFit           fit;
        };
        std::unordered_map<RE::FormID, Cached> g_bestFit;   // worker-serial (#4)

        bool IsTwoHander(const RE::TESObjectWEAP* a_w) {
            return a_w && (a_w->IsTwoHandedSword() || a_w->IsTwoHandedAxe());
        }
        bool IsOneHander(const RE::TESObjectWEAP* a_w) {
            return a_w && (a_w->IsOneHandedSword() || a_w->IsOneHandedDagger() || a_w->IsOneHandedAxe() ||
                           a_w->IsOneHandedMace());
        }

        // The best carried melee weapon of a_cls (1 one-handed, 2 two-handed), or nullptr.
        RE::TESObjectWEAP* PickMelee(RE::Actor* a_follower, std::uint8_t a_cls, bool a_daggerMelee,
                                     const Logistics::CombatWeaponStyle& a_style) {
            RE::TESObjectWEAP* best = nullptr; float bestScore = 0.0f;
            RE::TESObjectWEAP* bestRelic = nullptr; float bestRelicScore = 0.0f;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w->IsStaff() || Logistics::CombatWeaponExcluded(w)) continue;
                if (a_cls == 1 ? !IsOneHander(w) : !IsTwoHander(w)) continue;
                if (a_daggerMelee && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                const float score = Logistics::CombatWeaponScore(a_style, w);
                if (BestFitIsMuseumRelic(a_follower, w)) {
                    if (score >= bestRelicScore) { bestRelicScore = score; bestRelic = w; }
                    continue;
                }
                if (score >= bestScore) { bestScore = score; best = w; }
            }
            return best ? best : bestRelic;
        }

        // The best carried launcher WITH matching ammo, or nullptr.
        RE::TESObjectWEAP* PickRanged(RE::Actor* a_follower, const Logistics::CombatWeaponStyle& a_style) {
            const Logistics::CombatAmmo ammo = Logistics::CombatAmmoOwned(a_follower);
            RE::TESObjectWEAP* best = nullptr; float bestScore = 0.0f;
            RE::TESObjectWEAP* bestRelic = nullptr; float bestRelicScore = 0.0f;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || !(w->IsBow() || w->IsCrossbow()) || Logistics::CombatWeaponExcluded(w)) continue;
                if (ammo.For(w) <= 0) continue;   // a launcher only with ammo
                const float score = Logistics::CombatRangedScore(a_style, w, ammo);
                if (BestFitIsMuseumRelic(a_follower, w)) {
                    if (score >= bestRelicScore) { bestRelicScore = score; bestRelic = w; }
                    continue;
                }
                if (score >= bestScore) { bestScore = score; best = w; }
            }
            return best ? best : bestRelic;
        }

        BestFit Compute(RE::Actor* a_follower) {
            BestFit out;
            const auto fit = g_followers.find(a_follower->GetFormID());
            if (fit == g_followers.end()) return out;
            const Logistics::CombatBestFit dir = Logistics::CombatBestFitFor(a_follower, fit->second);
            // A base mage's melee sidearm is a dagger (the combat pick's rule, cast/Equip.cpp daggerMelee).
            const bool daggerMelee = Followers::GetBaseClass(a_follower) == 3 && Config::g_mageDaggersOnly.load();

            RE::TESObjectWEAP* main = nullptr;
            std::uint8_t cls = 0;   // 1 one-handed, 2 two-handed, 3 ranged
            const auto tryRanged = [&]() {
                if (!dir.hasRanged || main) return;
                if (auto* w = PickRanged(a_follower, dir.style)) { main = w; cls = 3; out.skill = dir.rangedSkill; }
            };
            const auto tryMelee = [&]() {
                if (dir.meleeClass == 0 || main) return;
                if (auto* w = PickMelee(a_follower, dir.meleeClass, daggerMelee, dir.style)) {
                    main = w; cls = dir.meleeClass; out.skill = dir.meleeSkill;
                }
            };
            // The leading class first, the other as the fallback (a leading bow with no arrows, a melee class
            // with no weapon carried): a follower with ANY usable weapon gets a pick.
            if (dir.rangedFirst) { tryRanged(); tryMelee(); } else { tryMelee(); tryRanged(); }
            if (!main) return out;

            out.main     = main->GetFormID();
            out.mainSlot = cls == 1 ? APMF_API::kEquipSlot_Right : APMF_API::kEquipSlot_Default;
            out.cls      = cls == 1 ? "one-handed" : cls == 2 ? "two-handed" : main->IsCrossbow() ? "crossbow" : "bow";
            // The left hand: only beside a one-hander, only when the perks vote for it, only with the
            // weapon-style control on (the combat pick's own gate).
            if (cls == 1 && dir.style.offHand != 0 && Config::g_weaponStyleControl.load()) {
                if (dir.style.offHand == 2) {
                    if (auto* l = BestFitOffHandWeapon(a_follower, dir.style, daggerMelee, main)) out.left = l->GetFormID();
                } else if (auto* sh = BestFitShield(a_follower)) {
                    out.left = sh->GetFormID();
                }
            }
            return out;
        }

        const char* NameOfForm(RE::FormID a_form) {
            const auto* f = a_form ? RE::TESForm::LookupByID(a_form) : nullptr;
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "-";
        }
    }

    BestFit BestFitFor(RE::Actor* a_follower) {
        if (!a_follower || !WeighingEquips()) return {};
        const RE::FormID fid = a_follower->GetFormID();
        const auto now = Clock::now();
        auto it = g_bestFit.find(fid);
        if (it != g_bestFit.end() && now - it->second.at < kBestFitRefresh) return it->second.fit;
        const BestFit fresh = Compute(a_follower);
        const bool first   = it == g_bestFit.end();
        const bool changed = first || it->second.fit.main != fresh.main || it->second.fit.left != fresh.left;
        auto& c = g_bestFit[fid];
        c.at  = now;
        c.fit = fresh;
        if (changed && (fresh.main || fresh.left))
            spdlog::info("[equip-pref] {:08X}: best fit R='{}' L='{}' (skill {:.0f}, from perk direction; {})", fid,
                         NameOfForm(fresh.main), NameOfForm(fresh.left), fresh.skill, fresh.cls);
        return fresh;
    }

    void ForgetBestFit(RE::FormID a_follower) { g_bestFit.erase(a_follower); }
    void ClearBestFit() { g_bestFit.clear(); }

}
