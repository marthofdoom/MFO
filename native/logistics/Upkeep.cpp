// logistics/Upkeep.cpp -- out-of-combat UPKEEP the tick calls into: potions (the
// classifier, counts, DrinkBest/DrinkPotion), torch, weapon hand-back and shed,
// the looting/trading flags, the combat and dismissal lifecycle hooks, and the
// g_stockGear co-save accessors (CopyStockGear / LoadStockRecord / ClearStockGear).
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
#include "PCH.h"
#include "Logistics.h"
#include "Logistics_internal.h"   // the split modules' shared substrate
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include "cast/Actuation.h"   // cast-in-logistics: reuse the combat cast path (Fire)
#include "CasterConsent.h"  // ClassifySpell: beneficial-vs-hostile OOC cast routing
#include "apmf/APMFBridge.h"   // IsHealCastActive: label the OOC concentration log (F1/F4 fix)
#include "ComposedCast.h"  // HeldOffBy: an Applied that was a HOLD, not a delivery (amendment (b))
#include <algorithm>      // std::sort/std::min/std::erase_if (healing stock cap)
#include <cmath>          // std::sin/cos/sqrt for the view cone
#include <unordered_set>  // keepWeapons: best-of-each-class protection set
#include <array>          // P7: fixed-size per-slot travel-intent table
#include <cctype>         // std::tolower: keyword-name school match (v1.0.31)
#include <utility>        // std::pair: the school-name keyword table (v1.0.31)
#include <string_view>    // #coinfix: editorID string match on an item's own keywords
#include "Confidence.h"   // the confidence leash (core tenet)
#include "Packages.h"     // Option A: LootTravelFill / LootTravelClear
#include "Forms.h"        // g_travelPackage / g_lootQuest (WALK diagnostic)
#include "Probe.h"        // Probe::CrosshairTarget (the QuickLoot-aware claim signal)
#include "ItemCatalog.h"  // load-order item catalog: potion class + never-loot exclusions
#include "MEOBridge.h"    // MEO gem transfer on gear swap (#17) + WornUid
#include "Papyrus.h"      // route 2b acquire probe: VM-dispatched ObjectReference.Activate
#include "MainThread.h"   // the pump (§0.37): live-vendor reads MUST run on the main thread
#include "Sightline.h"    // LoS + line-of-fire gate on the OOC hostile-FF direct fallback
#include "Followers.h"    // #62 on-load beast-head sweep iterates g_active (main thread)
#include "Diagnostics.h"  // SEV-1: PumpTickGate/CurrentPumpEpoch to drain the loot-waiver sink
#include <functional>     // #62 self-reposting on-load sweep closure
#include <memory>         // std::shared_ptr for that closure
#include "TradeBridge.h"  // #21 econ bridge: MFO_Trade Papyrus round-trip (Phase 0 self-test)
#include <mutex>          // #69: g_stockMx -- g_stockGear is a real cross-thread map (worker + co-save)

namespace MFO::Logistics {

    namespace {

        // ── DRINK ───────────────────────────────────────────────────────────
        // Consume the BEST (highest-magnitude) restore potion of a_which the
        // follower already carries. The AlchemyItem equip path IS the vanilla
        // "drink" for an actor (DESIGN §4.5 Tier A). Reads/mutates the named
        // follower only.
        std::uint64_t drinkKey(RE::FormID a_fid, RE::ActorValue a_av) {
            return (static_cast<std::uint64_t>(a_fid) << 8) | static_cast<std::uint8_t>(a_av);
        }

    }

        bool DrinkBest(RE::Actor* a_follower, RE::ActorValue a_which) {
            // COOLDOWN: a restore potion works over its duration; do not chain-
            // drink the stack while the first is still active (M5).
            const auto key = drinkKey(a_follower->GetFormID(), a_which);
            if (auto it = g_drinkUntil.find(key);
                it != g_drinkUntil.end() && std::chrono::steady_clock::now() < it->second) {
                return false;
            }

            // IN COMBAT drink the STRONGEST (survival first). OUT of combat, drink the
            // WEAKEST potion that still covers the missing amount -- topping up with an
            // Ultimate potion to heal a scratch is the wasteful "burns the best potion"
            // tell (Fable P10). If nothing single-handedly covers the deficit, fall to
            // the strongest to close the most gap.
            const bool  inCombat = a_follower->IsInCombat();
            float deficit = 0.0f;
            if (!inCombat) {
                if (auto* avo = a_follower->AsActorValueOwner())
                    deficit = std::max(0.0f, avo->GetPermanentActorValue(a_which) -
                                             avo->GetActorValue(a_which));
            }

            RE::AlchemyItem* strong = nullptr; float strongMag = -1.0f;
            RE::AlchemyItem* cover  = nullptr; float coverMag  = std::numeric_limits<float>::max();
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc || PotionRestores(alc) != a_which) continue;
                // Magnitude of the costliest effect -- the same effect PotionRestores
                // classified on, so ranking is by the resource the potion is FOR.
                const auto* eff = alc->GetCostliestEffectItem();
                const float mag = eff ? eff->GetMagnitude() : 0.0f;
                if (mag > strongMag) { strongMag = mag; strong = alc; }
                if (!inCombat && mag + 0.5f >= deficit && mag < coverMag) { coverMag = mag; cover = alc; }
            }
            RE::AlchemyItem* best = (!inCombat && cover) ? cover : strong;
            if (!best) {
                // REGRESSION GUARD [potprobe]: a wanted drink found no potion. If the
                // follower carries alchemy anyway, a classifier regression (a mod
                // reworking potion archetypes -> PotionRestores misreads them) is the
                // suspect -- dump the raw archetype/AV so it is diagnosable. Silent
                // when the follower simply has no alchemy. Rate-limited 15s/follower.
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextPotProbe;
                auto& pn = s_nextPotProbe[a_follower->GetFormID()];
                const auto tnow = std::chrono::steady_clock::now();
                if (pn.time_since_epoch().count() == 0 || tnow >= pn) {
                    pn = tnow + std::chrono::seconds(15);
                    std::string dump;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* alc = obj->As<RE::AlchemyItem>();
                        if (!alc || alc->IsFood() || alc->IsPoison()) continue;
                        const auto* eff = alc->GetCostliestEffectItem();
                        auto* mgef = eff ? eff->baseEffect : nullptr;
                        dump += std::format(" [{} x{} arch={} primAV={} -> restores={}]",
                            alc->GetFullName() ? alc->GetFullName() : "?", data.first,
                            mgef ? static_cast<int>(mgef->data.archetype) : -1,
                            mgef ? static_cast<int>(mgef->data.primaryAV) : -1,
                            static_cast<int>(PotionRestores(alc)));
                    }
                    if (!dump.empty())
                        spdlog::info("[potprobe] {:08X} want={} -- no match, but carries alchemy:{}",
                                     a_follower->GetFormID(), static_cast<int>(a_which), dump);
                }
                return false;
            }

            auto* mgr = RE::ActorEquipManager::GetSingleton();
            if (!mgr) return false;
            // Equipping a potion on an actor consumes it -- the documented Tier A
            // path. One unit; the engine removes it from the pack.
            mgr->EquipObject(a_follower, best, nullptr, 1);
            // Gate this AV for the potion's own duration (min one logistics
            // interval), so the next tick does not drink the rest of the stack.
            float dur = 1.0f;
            if (const auto* eff = best->GetCostliestEffectItem())
                dur = std::max<float>(1.0f, static_cast<float>(eff->GetDuration()));
            g_drinkUntil[key] = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(static_cast<int>(dur * 1000.0f));
            return true;
        }

    // ── public: pure reads used by the evaluator ────────────────────────────

    RE::ActorValue PotionRestores(RE::AlchemyItem* a_potion) {
        if (!a_potion || a_potion->IsPoison()) return RE::ActorValue::kNone;
        // FOOD is an AlchemyItem with a Restore-Health/Stamina effect, so the
        // archetype gate below would pass it. §4.8.2 is potions ONLY -- exclude
        // food or the follower vacuums city food barrels and counts apples as
        // health potions.
        if (a_potion->IsFood()) return RE::ActorValue::kNone;

        // CATALOG FIRST. The MFO.Synthesis patcher already classified this potion
        // from its real effect record (beneficial, non-recover, on a resource) --
        // Requiem-proof, where the archetype heuristic below misreads a reworked
        // potion as kNone (marth: "5 health potions, won't drink"). A catalog hit
        // is authoritative; a miss falls through to the heuristic so MFO still
        // works with no patcher run.
        if (auto av = Catalog::PotionRestores(a_potion->GetFormID()); av != RE::ActorValue::kNone)
            return av;

        // The COSTLIEST effect is the potion's dominant purpose. Classify by
        // ARCHETYPE, not by name and not by the effect's AV alone: only
        // kValueModifier / kDualValueModifier are real resource restores
        // (fortify is kPeakValueModifier, cure is its own archetype), which is
        // the portable classifier from MRO's Requiem note (DESIGN §3.3).
        const auto* eff  = a_potion->GetCostliestEffectItem();
        auto*       mgef = eff ? eff->baseEffect : nullptr;
        if (!mgef) return RE::ActorValue::kNone;

        const auto arch = mgef->data.archetype;
        if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
            arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) {
            return RE::ActorValue::kNone;
        }
        return mgef->data.primaryAV;
    }

    float PotionMagnitude(RE::AlchemyItem* a_potion) {
        if (!a_potion) return 0.0f;
        const auto want = PotionRestores(a_potion);   // catalog-authoritative type
        if (want == RE::ActorValue::kNone) return 0.0f;
        // Largest magnitude among the potion's restore effects on that resource.
        // Same archetype gate as PotionRestores (value modifiers only, so fortify
        // and cure never count), so the number belongs to the resource the potion
        // is classified under.
        float best = 0.0f;
        for (const auto* e : a_potion->effects) {
            const auto* mgef = e ? e->baseEffect : nullptr;
            if (!mgef) continue;
            const auto arch = mgef->data.archetype;
            if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
                arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) continue;
            if (mgef->data.primaryAV != want) continue;
            const float mag = e->GetMagnitude();
            if (mag > best) best = mag;
        }
        return best;
    }

    bool AmmoIsBolt(RE::TESAmmo* a_ammo) {
        if (!a_ammo) return false;   // defense-in-depth: every current caller null-guards,
                                     // but a future one shouldn't CTD on GetFormID() here
        switch (Catalog::AmmoKind(a_ammo->GetFormID())) {
        case Catalog::Ammo::kArrow: return false;
        case Catalog::Ammo::kBolt:  return true;
        default:                    return a_ammo->IsBolt();
        }
    }

    float PotionLootFloor() {
        return Config::g_minPotionMag.load() > 0
                   ? static_cast<float>(Config::g_minPotionMag.load())
                   : g_autoPotionFloor;
    }

    void ComputeWeakPotionFloor() {
        g_autoPotionFloor = 0.0f;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        std::vector<float> mags;
        for (auto* alc : dh->GetFormArray<RE::AlchemyItem>()) {
            if (!alc || alc->IsPoison() || alc->IsFood()) continue;
            if (PotionRestores(alc) == RE::ActorValue::kNone) continue;   // restore potions only
            if (const float m = PotionMagnitude(alc); m > 0.0f) mags.push_back(m);
        }
        if (mags.size() < 2) {
            spdlog::info("[potfloor] {} restore potion(s) in load order -- no low-power floor", mags.size());
            return;
        }
        std::sort(mags.begin(), mags.end());
        // Tier boundary: a magnitude beyond 1.25x the weakest tier's floor starts a
        // new tier; the SECOND tier's floor is the low-power cutoff (the weakest tier
        // sits below it). Relative, so it adapts to the list's own potion scale.
        const float t1 = mags.front();
        for (float m : mags) if (m > t1 * 1.25f) { g_autoPotionFloor = m; break; }
        std::string ladder; float last = -1.0f;   // distinct-ish tiers, for tuning transparency
        for (float m : mags)
            if (last < 0.0f || m > last * 1.25f) { ladder += std::to_string(static_cast<int>(m + 0.5f)); ladder += ' '; last = m; }
        spdlog::info("[potfloor] restore-potion tiers: {}| auto low-power floor = {:.0f} "
                     "(restore potions below this are ignored; iMinPotionMag overrides)",
                     ladder, g_autoPotionFloor);
    }

    int CountPotions(RE::Actor* a_follower, RE::ActorValue a_which) {
        if (!a_follower) return 0;
        int n = 0;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* alc = obj->As<RE::AlchemyItem>();
            if (alc && PotionRestores(alc) == a_which) n += data.first;
        }
        return n;
    }

    // ARROWS the follower carries -- no bow gate; the "arrows below N" gambit
    // decides whether that matters. Bolts are a separate count/gambit.
    int ArrowCount(RE::Actor* a_follower) { return AmmoCount(a_follower, false); }
    int BoltCount(RE::Actor* a_follower)  { return AmmoCount(a_follower, true);  }

    // Equip a carriable torch the follower holds (moved here from combat, #35 --
    // torch is upkeep; pair with "In an interior"/"At night"). No-op if a light is
    // already in hand or none is carried. APMF equip authority (ABI v9): a torch
    // competes for Light+Left, and Left is owned while a left or bow/two-hander
    // hold stands -- the equip would only draw an `External(MFO.dll) ...
    // verdict=deny` at APMF's seat, so it is skipped there (logged). Light itself
    // is never owned, so with no hold the torch goes on as without APMF.
    bool EquipTorch(RE::Actor* a_follower) {
        if (auto* l = a_follower->GetEquippedObject(true); l && l->As<RE::TESObjectLIGH>())
            return false;
        if (APMFBridge::EquipAuthorityOwns(a_follower->GetFormID(), APMF_API::kEquipCat_Left)) {
            spdlog::info("[equip] {:08X}: torch skipped -- the equip authority owns the left hand (a hold stands)",
                         a_follower->GetFormID());
            return false;
        }
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* light = obj->As<RE::TESObjectLIGH>();
            if (!light || !light->CanBeCarried()) continue;
            // #62 3D-RACE: the equip rebuilds biped 3D -- marshal it to the MAIN
            // THREAD like doEquip / HealExcludedWeapon, never on this job worker.
            // Capture FormIDs and re-resolve on the frame that runs.
            const RE::FormID folID   = a_follower->GetFormID();
            const RE::FormID lightID = light->GetFormID();
            auto doTorch = [folID, lightID]() {
                auto* mgr  = RE::ActorEquipManager::GetSingleton();
                auto* fol  = RE::TESForm::LookupByID<RE::Actor>(folID);
                auto* form = RE::TESForm::LookupByID(lightID);
                auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
                if (mgr && fol && item) mgr->EquipObject(fol, item);
            };
            if (MainThread::IsInstalled()) MainThread::Post(doTorch);
            else                           doTorch();   // VR: pump is a no-op, keep the direct path
            return true;
        }
        return false;
    }

    // ── public: actuation ───────────────────────────────────────────────────

    // Public forwarder so Actuation (the combat dispatcher) drinks through the
    // exact same cooldown-gated path as the logistics table. NOT gated on
    // bLogistics: that flag governs OUT-OF-COMBAT looting/drinking; an in-combat
    // drink is a combat gambit, and the gate is that the player assigned it.
    bool DrinkPotion(RE::Actor* a_follower, RE::ActorValue a_which) {
        if (!a_follower) return false;
        return DrinkBest(a_follower, a_which);
    }

    // HEAL: a follower wielding an EXCLUDED weapon renders it INVISIBLE -- MFO
    // wrongly looted a NON-PLAYABLE creature weapon (field: a Dwarven Sphere
    // Crossbow off an automaton corpse) before the catalog excluded that class. It
    // still FIRES, so combat looks fine, but there's no humanoid mesh. Swap it for
    // the follower's best carried PLAYABLE weapon (or just take it off if they carry
    // none). Fires on IsCreatureWeapon (works off the DLL alone -- curated set +
    // NonPlayable flag, no catalog needed) OR Catalog::IsExcluded (quest/unique,
    // needs the regenerated catalog). A no-op once healed, so it's safe to poll.
    void HealExcludedWeapon(RE::Actor* a_follower) {
        if (!RE::ActorEquipManager::GetSingleton()) return;   // (re-fetched on the main thread in doHeal)
        for (int hand = 0; hand < 2; ++hand) {
            auto* eq  = a_follower->GetEquippedObject(hand == 1);   // false=right, true=left
            auto* bad = eq ? eq->As<RE::TESObjectWEAP>() : nullptr;
            if (!bad || (!Catalog::IsExcluded(bad->GetFormID()) && !IsCreatureWeapon(bad))) continue;
            RE::TESBoundObject* best    = nullptr;
            std::uint16_t       bestDmg = 0;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || Catalog::IsExcluded(obj->GetFormID()) || IsCreatureWeapon(w)) continue;
                if (WeaponClassOf(w->GetWeaponType()) == WepClass::Other) continue;  // no staff/creature
                if (w->GetAttackDamage() > bestDmg) { bestDmg = w->GetAttackDamage(); best = obj; }
            }
            // #62 3D-RACE: EquipObject/UnequipObject/RemoveItem all rebuild the
            // follower's biped 3D and MUST run on the MAIN THREAD, not this AddTask
            // job worker (§0.37 -- the invisible-head mechanism, same hop #62's
            // loot-equip doEquip uses). The whole tail is INTERDEPENDENT: the equip
            // auto-unequips the excluded weapon back into the pack, which the
            // RemoveItem below then has to find and evict -- so marshal it as ONE
            // unit. Capture FormIDs + the creature verdict (a pure read, safe now);
            // re-resolve on the frame that runs.
            //
            // CRITICAL (why the RemoveItem, not just an unequip): un-equipping alone
            // leaves the weapon in the pack, and the engine re-wields it as "best
            // weapon" within seconds -- the poll then churns forever (field-caught:
            // same 'Dwarven Sphere Crossbow' re-healed 3 min apart). So it has to
            // LEAVE the follower. Which way follows the ARMOR fix's rule
            // (KeepHeadClear): a CREATURE weapon is non-playable -- invisible/broken
            // on the PLAYER too -- DELETE it; a merely catalog-excluded weapon is a
            // real playable quest/unique item -> return it to the player. Count the
            // copies so a stack leaves entirely.
            const bool       creature = IsCreatureWeapon(bad);
            const RE::FormID folID    = a_follower->GetFormID();
            const RE::FormID bestID   = best ? best->GetFormID() : 0;
            const RE::FormID badID    = bad->GetFormID();
            auto doHeal = [folID, bestID, badID, creature]() {
                auto* eq      = RE::ActorEquipManager::GetSingleton();
                auto* fol     = RE::TESForm::LookupByID<RE::Actor>(folID);
                auto* badForm = RE::TESForm::LookupByID(badID);
                auto* badObj  = badForm ? badForm->As<RE::TESBoundObject>() : nullptr;
                if (!eq || !fol || !badObj) return;
                RE::TESBoundObject* bestObj = nullptr;
                if (bestID)
                    if (auto* f = RE::TESForm::LookupByID(bestID)) bestObj = f->As<RE::TESBoundObject>();
                if (bestObj) eq->EquipObject(fol, bestObj);   // auto-unequips the excluded one
                else         eq->UnequipObject(fol, badObj);  // nothing real carried -- just take it off
                std::int32_t haveBad = 0;
                for (auto& [obj, data] : fol->GetInventory())
                    if (obj == badObj) { haveBad = data.first; break; }
                if (haveBad > 0) {
                    if (creature)
                        fol->RemoveItem(badObj, haveBad, RE::ITEM_REMOVE_REASON::kRemove,
                                        nullptr, nullptr);
                    else if (auto* player = RE::PlayerCharacter::GetSingleton())
                        fol->RemoveItem(badObj, haveBad, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                        nullptr, player);
                }
                spdlog::info("[heal] {:08X} excluded '{}' -> {}, {} {}x", folID,
                             badObj->GetName() ? badObj->GetName() : "?",
                             bestObj ? (bestObj->GetName() ? bestObj->GetName() : "?") : "(bare hands)",
                             creature ? "DELETED" : "returned-to-player", haveBad);
            };
            if (MainThread::IsInstalled()) MainThread::Post(doHeal);
            else                           doHeal();   // VR: pump is a no-op, keep the direct path
            return;   // one hand per call
        }
    }

    // SHED OFF-ROLE WEAPONS (marth). A follower carrying a weapon of a role they
    // do NOT maintain -- a 2H on a 1H fighter, a crossbow on a bow user, any ranged
    // on a melee-only fighter -- lets their game AI equip the wrong thing "of its
    // own volition", and it is dead weight / a leftover from pre-1.0.12 skill-forced
    // loot. Hand ONE such weapon back to the PLAYER per idle tick (recoverable,
    // never destroyed). The role set is the SAME gambit-driven one LootEquipment
    // uses (keep in sync), so MFO never sheds a class it would loot. NEVER sheds an
    // in-role class, a SOCKETED weapon (gems), a catalog-excluded/quest weapon, a
    // creature weapon, or a staff; never runs for a follower with NO weapon role (a
    // caster); never leaves them with zero in-role weapons. Runs OUT of combat, so
    // unequipping a worn off-role weapon is safe. RemoveItem-to-player is the same
    // worker-safe move LootAmmo already uses.
    bool ShedOffRoleWeapon(RE::Actor* a_follower, const FollowerState& a_state) {
        using WT = RE::WEAPON_TYPE;

        // POST-BATTLE GATE (the field fix): only shed AFTER a fight, never during
        // one. The Scheduler runs this path out of PARTY combat, and ALSO for a
        // follower whose own IsInCombat() reads false inside a party fight (the
        // own-OOC road, fix/mfo-party-combat-gate 2026-09-21: the combat table
        // ran first and took no action). IsInCombat() FLAPS false mid-fight (a
        // moment of LoS loss / a disengage) and the follower gets serviced
        // through the lull -- which used to drop a looted off-role weapon
        // MID-FIGHT (field: a 2h follower handed the player a looted 1h mace
        // during a combat lull). So require a STABLE out-of-combat window:
        // NoteInCombat stamps g_lastCombatSeen from the Scheduler's PARTY-combat
        // branch on every party-combat service, own flag or not, and we bail
        // until kShedPostBattleDwell has passed since that stamp. A one-frame
        // flap re-stamps the instant combat resumes, so the dwell can never
        // mature inside a lull or inside a party fight -- only once the battle
        // has genuinely ended. Worker-only
        // read, no lock (same BSJobs tick as the stamp, #4). No entry at all ->
        // never fought this session -> shed freely (e.g. a freshly recruited
        // follower carrying off-role gear).
        if (auto it = g_lastCombatSeen.find(a_follower->GetFormID());
            it != g_lastCombatSeen.end() && Clock::now() - it->second < kShedPostBattleDwell)
            return false;

        // #69: the SAME stable role signal LootEquipment loots/keeps against
        // (ComputeWeaponRoles, kept in sync by construction) -- never the
        // momentarily-wielded weapon. A follower who carries both a melee and
        // a ranged weapon keeps BOTH roles regardless of which is drawn right
        // now, so this can no longer shed what loot just decided to keep.
        const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);

        // MAGIC LOADOUT (v1.0.29): the mage's one-hand BACKUP is IN-ROLE. A
        // magic user (>= 1 enabled cast gambit -- the SAME gambit-driven test
        // the loot side uses, keep in sync) keeps his sidearm even though no
        // equip-melee gambit authors a melee role for him; without this, a
        // caster who ALSO has a ranged role would shed the dagger the loot
        // pass just fetched, corpse after corpse (loot->shed thrash). A pure
        // caster never reaches the shed at all (the no-role early-return
        // below), so this only matters for the mixed roles.
        int castGambits = 0;
        if (Config::g_magicLoadout.load()) TargetMagicSchool(a_state, castGambits);
        const bool magicUser   = castGambits > 0;
        const bool daggersOnly = Config::g_mageDaggersOnly.load();

        const WepClass meleeRole    = roles.melee;
        const bool     doRanged     = roles.doRanged;
        const bool     wantCrossbow = roles.wantCrossbow;
        if (meleeRole == WepClass::Other && !doRanged) return false;   // no role -> not ours to judge

        // FISTS (marth: "they can hold them, but they shouldn't be valid for
        // fighting, unless progression is installed AND unarmed perks are
        // selected via progression"). The engine's Unarmed pseudo-weapon (0x1F4,
        // WEAPON_TYPE kHandToHandMelee -> WepClass::Other) sits in EVERY actor's
        // inventory. It used to fall through inRole as `true`, so fists alone
        // satisfied the never-disarm guard below and the shed dropped a
        // follower's ONLY real weapon (LoreRim, 2026-09-12: Cosnach, two-handed
        // by skill, sole weapon an Iron Mace -> DROPPED -> fought bare-handed).
        // Now fists are in-role ONLY when the Progression add-on's catalog is
        // built AND this follower has empty-hand-conditioned perk ranks in
        // MFO's OWN allocation record (StyleVotes::unarmed -- never HasPerk, so
        // a natively perked but unenrolled follower still keeps his weapon).
        // Read through the worker-side mirror (StyleVotesFor, Logistics_Loot.cpp:
        // the last main-thread tally, no perk walk on this thread); until the
        // first tally lands the vote is 0 -> fists not valid -> the safe default.
        const auto votes = StyleVotesFor(a_follower);
        const bool progression = Progression::Get().built;
        const bool fistsValid  = progression && votes.unarmed > 0;

        auto inRole = [&](const RE::TESObjectWEAP* w) {
            const WepClass wc = WeaponClassOf(w->GetWeaponType());
            // The magic user's sidearm class is always his to keep (v1.0.29).
            if (magicUser && wc == WepClass::OneHand &&
                (!daggersOnly || w->GetWeaponType() == WT::kOneHandDagger)) return true;
            if (wc == WepClass::OneHand || wc == WepClass::TwoHand) return wc == meleeRole;
            if (wc == WepClass::Ranged) {
                if (!doRanged) return false;
                const auto t = w->GetWeaponType();
                return wantCrossbow ? (t == WT::kCrossbow) : (t == WT::kBow);
            }
            // Fists/other: never a shed target (the loop below only counts or
            // skips), but in-role -- i.e. able to satisfy the never-disarm
            // guard -- only under marth's rule above.
            return fistsValid;
        };
        auto socketed = [](RE::InventoryEntryData* e) {
            if (!e || !e->extraLists) return false;
            for (auto* xl : *e->extraLists)
                if (auto* uid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr; uid && uid->uniqueID != 0)
                    return true;
            return false;
        };

        RE::TESBoundObject* shed = nullptr; std::int32_t shedCount = 0; int inRoleWeapons = 0;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* w = obj->As<RE::TESObjectWEAP>();
            if (!w || w->IsStaff()) continue;
            if (inRole(w)) { ++inRoleWeapons; continue; }
            // Fists that are NOT valid for fighting are still never shed: the
            // Unarmed record is the engine's, not a weapon anyone can drop.
            if (WeaponClassOf(w->GetWeaponType()) == WepClass::Other) continue;
            if (IsCreatureWeapon(w) || Catalog::IsExcluded(obj->GetFormID())) continue;
            // #69: never shed the follower's OWN gear, snapshotted at first
            // management (the Gauldurbow fix) -- an off-role signature weapon
            // stays in the pack, not handed to the player.
            if (IsStockGear(a_follower->GetFormID(), obj->GetFormID())) continue;
            if (!Config::g_lootSpecialItems.load() && socketed(data.second.get())) continue;
            if (!shed) { shed = obj; shedCount = data.first; }   // first off-role, one per tick
        }
        if (!shed) return false;   // nothing off-role in the pack
        // The fists verdict is what decides the never-disarm guard when the only
        // other "weapon" is the Unarmed record, so log it where it matters --
        // with a shed candidate in hand -- once per CHANGE per follower.
        {
            const std::uint32_t packed = (fistsValid ? 1u : 0u) | (progression ? 2u : 0u) |
                                         (static_cast<std::uint32_t>(std::max(votes.unarmed, 0)) << 2);
            auto& last = g_shedFistsLogged[a_follower->GetFormID()];
            if (last != packed + 1u) {   // +1: an absent entry (0) never matches a real verdict
                last = packed + 1u;
                spdlog::info("[shed] {:08X}: fists {} (progression={}, unarmed perks={})",
                             a_follower->GetFormID(), fistsValid ? "valid" : "not valid",
                             progression ? "y" : "n", votes.unarmed);
            }
        }
        if (inRoleWeapons == 0) return false;   // don't disarm

        const char* nm = shed->GetName() ? shed->GetName() : "?";

        // DROP IT ON THE FLOOR near the follower (marth: "drop the handing
        // entirely"). NOT handed to the player anymore -- the follower just sets
        // the off-role weapon down where anyone can pick it back up.
        //
        // THREADING -- this MUST run on the main thread. DropObject spawns a NEW
        // world reference (a dropped-item 3D object placed in the cell): a 3D /
        // cell mutation, exactly the class MainThread::Post exists to marshal off
        // this BSJobs worker (#4/#14, and the #62 equip / loose-loot ActivateRef
        // hops right here in this file do the same). Creating a world ref off the
        // worker races the render/cell threads -> UB/CTD. RemoveItem-to-a-container
        // is worker-safe, but a floor DROP is not -- so we post the whole
        // remove+place to main. Capture FormIDs only (never the worker's stale
        // Actor*/TESBoundObject*) and re-resolve on the frame that runs.
        const RE::FormID folID  = a_follower->GetFormID();
        const RE::FormID itemID = shed->GetFormID();
        const std::int32_t dropCount = shedCount;
        auto doDrop = [folID, itemID, dropCount]() {
            auto* fol  = RE::TESForm::LookupByID<RE::Actor>(folID);
            auto* form = RE::TESForm::LookupByID(itemID);
            auto* item = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!fol || !item) return;
            // DropObject removes from the actor AND places the world ref in one
            // engine call (nullptr drop-loc/rotate -> the engine drops it at the
            // actor's feet).
            //
            // ABI -- CALL THE ENGINE VFUNC DIRECTLY, NOT THROUGH `RE::Actor::DropObject`.
            // Worked example of engineering principle 6 ("COMMONLIB DECLARATIONS
            // ARE NOT ABI-TRUSTWORTHY"), the third this week after GetMagicTarget
            // (hidden sret out-slot, Actuation_Direct.cpp) and ControlMap::
            // ToggleControls (member layout, Board.cpp). CommonLibSSE-NG 3.7.0's
            // wrapper (src/RE/A/Actor.cpp:1438) goes through RelocateVirtual<>,
            // whose RelocateVirtualHelper builds a FREE-function type
            // `Ret(This*, Args...)` (include/REL/Relocation.h:2038-2043) and has
            // NO member_function_non_pod_type handling. ObjectRefHandle is non-POD
            // (BSUntypedPointerHandle has a user dtor), so MSVC returns it through
            // a hidden sret pointer -- and for a FREE function that pointer takes
            // rcx, pushing `this` to rdx. The engine's DropObject is a MEMBER
            // function: rcx = this, rdx = &result, r8 = object, r9 = extraList,
            // [rsp+0x28] = count, [rsp+0x30] = dropLoc, [rsp+0x38] = rotate
            // (1.6.1170 SkyrimSE.exe+0x6781D0, 1.5.97 SkyrimSE.exe+0x5E6150 --
            // identical prologues, both read from the unpacked binaries). So the
            // wrapper hands the engine MFO's stack temporary as the Actor and it
            // crashes on whatever sits at [temp+0xF8] / [temp+0x60]
            // (currentProcess / parentCell). Deterministic on every call, every
            // runtime: 4 CTDs on LoreRim in one evening (v2.0.5, MFO.dll+FD11F
            // is the wrapper's return address on every crash stack). Measured
            // from MFO's own v2.0.5 bytes: the wrapper passes rcx = &result,
            // rdx = this. live-alandtse-ng v7.2.0 carries the same defect.
            //
            // Declaring the return as a POINTER keeps `this` in rcx and passes the
            // sret slot explicitly in rdx, which is byte-for-byte what the engine
            // consumes (it hands rdx back in rax, the sret convention). The vtable
            // index was never wrong: 0xCB on SE/AE (verified against BOTH unpacked
            // binaries, VTABLE_Actor[0] / VTABLE_Character[0] slot 0xCB), 0xCD on
            // VR per CommonLib's table. The VR index is unverified (no VR binary)
            // and unreachable: the `!MainThread::IsInstalled()` guard below skips
            // the shed on VR before this lambda is ever posted.
            // Do NOT reinstate the wrapper, and do NOT substitute
            // RemoveItem(kDropping): it skips the middleHigh queued-3D cleanup and
            // the post-drop fix-ups the real DropObject performs.
            using DropObjectFn = RE::ObjectRefHandle* (*)(
                RE::Actor* a_this, RE::ObjectRefHandle* a_out,
                const RE::TESBoundObject* a_object, RE::ExtraDataList* a_extraList,
                std::int32_t a_count, const RE::NiPoint3* a_dropLoc, const RE::NiPoint3* a_rotate);
            const std::size_t slot = REL::Module::IsVR() ? 0xCD : 0xCB;
            auto* vtbl = *reinterpret_cast<std::uintptr_t**>(fol);
            auto  fn   = reinterpret_cast<DropObjectFn>(vtbl[slot]);
            RE::ObjectRefHandle dropped;
            fn(fol, &dropped, item, nullptr, dropCount, nullptr, nullptr);
            // NEVER MASK A FAILURE (principle 7): an invalid handle means the
            // engine removed nothing / placed nothing. Say so on the frame it ran.
            if (!dropped)
                spdlog::warn("[shed] {:08X}: DropObject '{}' x{} returned an INVALID handle -- nothing dropped",
                             folID, item->GetName() ? item->GetName() : "?", dropCount);
        };
        if (!MainThread::IsInstalled()) {
            // VR: Post() is a documented no-op, and there is NO main thread to
            // fall back to -- a direct DropObject would spawn the world ref right
            // here on the BSJobs worker, the off-thread 3D-create crash class we
            // are avoiding. The shed is non-critical (the weapon just stays in the
            // pack another session), so SKIP rather than risk a CTD.
            spdlog::info("[shed] {:08X}: off-role weapon '{}' SKIPPED -- no main-thread pump for DropObject (VR)",
                         a_follower->GetFormID(), nm);
            return false;
        }
        MainThread::Post(doDrop);

        spdlog::info("[shed] {:08X}: off-role weapon '{}' x{} (class {}) DROPPED on floor | meleeRole={} doRanged={} xbow={}",
                     a_follower->GetFormID(), nm, shedCount,
                     static_cast<int>(WeaponClassOf(shed->As<RE::TESObjectWEAP>()->GetWeaponType())),
                     static_cast<int>(meleeRole), doRanged, wantCrossbow);
        return true;
    }

    // HUD activity glyphs -- see Logistics.h. Worker-domain reads only.
    bool IsLooting(RE::FormID a_id) {
        return SlotOf(a_id) != nullptr;
    }
    bool IsTrading(RE::FormID a_id) {
        auto it = g_econTrade.find(a_id);
        return it != g_econTrade.end() && Clock::now() < it->second;
    }

    // [L] glyph reliability fix (marth 2026-09-06, replaces the IsLooting proxy
    // for the board's "Looting" signal): stamp g_justLooted at the THREE call
    // sites in this file that observe a CONFIRMED acquisition -- LootNearby's own
    // true return (arm's-reach, no excursion), the loose-item Activate readback's
    // inventory delta, and StripCorpse's `moved` -- never at the travel-slot
    // claim (SlotOf/IsLooting), which is neither necessary (arm's-reach loot
    // never claims a slot) nor sufficient (a slot is held the whole walk-there,
    // and an arrival that finds "nothing to take" still held one) for a real
    // pickup.
    //
    // Window sizing (#9 -- a floor, not a guessed expiry): the board snapshot
    // that reads this (PublishSnapshot) drains on this same worker in the
    // Scheduler's round-robin, so a given follower is only revisited every
    // ~partySize * kPumpMs(133ms) -- the same cadence math APMFBridge/
    // Actuation_Direct already use for their suppress windows. The stamp must
    // outlive that gap or a fast pickup between two of THIS follower's own
    // snapshot drains would never be seen lit.
    void MarkJustLooted(RE::FormID a_id, Clock::time_point a_now) {
        const float partySize = static_cast<float>(Followers::g_active.size() + 1);   // + player
        const float windowSec = std::max(1.0f, 0.133f * partySize + 0.5f);
        g_justLooted[a_id] = a_now + std::chrono::milliseconds(
                                          static_cast<long long>(windowSec * 1000.0f));
    }
    bool JustLooted(RE::FormID a_id) {
        auto it = g_justLooted.find(a_id);
        return it != g_justLooted.end() && Clock::now() < it->second;
    }
    // [L] while WALKING (marth 2026-09-23): lit while the follower holds a live
    // loot leg (Walking phase) AND his current package really is a travel
    // package -- the same engine read the service's onTravelNow/legEngaged uses.
    // A dispatched leg the engine has not adopted yet (still on his follow
    // package) reads false. Live read each snapshot, so no window is needed:
    // a walk lasts seconds, far past the round-robin revisit gap.
    bool WalkingLootLeg(RE::Actor* a_follower) {
        if (!a_follower) return false;
        const auto* tr = SlotOf(a_follower->GetFormID());
        return tr && tr->phase == TravelPhase::Walking &&
               Forms::IsTravelPackage(a_follower->GetCurrentPackage());
    }

    // Stamp "seen in combat" for the post-battle shed gate. Called from the
    // Scheduler's in-combat branch, once per in-combat follower per tick, on the
    // SAME BSJobs worker that ServiceFollower/ShedOffRoleWeapon read it on -- so
    // g_lastCombatSeen needs no lock (#4). Cheap: one map write.
    void NoteInCombat(RE::FormID a_id) {
        g_lastCombatSeen[a_id] = Clock::now();
    }

    void ClearTransientState() {
        g_nextTick.clear();
        ClearEquipDeclarations();   // APMF equip authority: the last-sent sets -- the claims themselves
                                    // go in APMFBridge::ClearTransientState (kPreLoadGame); the first
                                    // service after the load re-claims and re-declares from scratch
        g_lastCombatSeen.clear();   // post-battle shed dwell -- save-scoped, live-session only
        g_shedFistsLogged.clear();  // the shed's logged fists verdict -- re-log after a load
        g_claim.clear();
        g_lastLootSource = 0;
        g_drinkUntil.clear();
        g_logiCastUntil.clear();   // OOC cast pacing window -- save-scoped (#6, FF-reuse)
        g_playerLooted.clear();
        g_econScan.clear();   // #21 econ cadence clocks -- save-scoped (Fable audit #7)
        g_econTrade.clear();
        g_econPair.clear();
        g_justLooted.clear();   // [L] glyph stamp -- save-scoped, live-session only
        // Drop any in-flight travel intent and release the engine alias so a
        // revert/load never leaves a follower latched (#55).
        for (int i = 0; i < Packages::kMaxLootSlots; ++i) {
            if (g_travelSlots[i].active) Packages::LootTravelClear("revert", nullptr, i);
            g_travelSlots[i] = TravelIntent{};
        }
        g_travelFailed.clear();
        g_travelUnreach.clear();
        g_stallStrikes.clear();
        g_grabGrow.clear();   // grown-grab radii are per-session verdicts
        ClearGates();         // loot M1: GATED records are per-session skips
        g_actorDefer.clear(); // loot M1: actor-block reorder records
        g_idleCycles.clear();
        g_lastBlocklistReassess = {};
    }

    void ReleaseTravelOnCombat(RE::Actor* a_follower) {
        if (!a_follower) return;
        const int slot = SlotIndexOf(a_follower->GetFormID());
        if (slot >= 0) {
            // EVICT him from the loot alias and re-evaluate NOW so the combat
            // table / his own AI takes over this tick, not on the engine's slow
            // pass. The corpse is not "failed" -- he can finish it after the
            // fight -- so no LRU mark.
            Packages::LootTravelClear("combat", a_follower, slot);
            g_travelSlots[slot] = TravelIntent{};
        }
    }

    void OnFollowerRemoved(RE::FormID a_id) {
        // APMF EQUIP AUTHORITY: the standing claim ends with MFO's management of
        // him -- dismissal AND the T#78 MFO-OFF edge both arrive here through
        // Followers::ReleaseHeldState. APMF hands the worn set back to the engine
        // (nothing is unequipped); the change detector goes with it so a re-recruit
        // declares from scratch. Idempotent (no claim -> no-op, no log).
        APMFBridge::ReleaseEquipAuthority(a_id);
        ForgetEquipDeclaration(a_id);
        // A follower dismissed DURING an excursion may still hold alias 0 (Clear
        // hasn't run). With his framework claim gone, MFO's static-60 claim is his
        // sole one -- he'd walk to the stale corpse and re-latch every load.
        // LootTravelEvictIf no-ops unless a_id is the current holder; between
        // excursions the slot holds the player, so this is normally a no-op.
        Packages::LootTravelEvictIf(a_id);
        // Forget the live intent too, if he was the active traveller (his slot).
        if (const int slot = SlotIndexOf(a_id); slot >= 0)
            g_travelSlots[slot] = TravelIntent{};
    }

    // ── #69: co-save companions for g_stockGear ─────────────────────────────
    // Locked accessors so Serialization.cpp never reaches into the anonymous
    // namespace's map directly -- the same hand-off shape every other module's
    // co-save side uses.

    std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> CopyStockGear() {
        std::scoped_lock lk(g_stockMx);
        return g_stockGear;   // copied out UNDER the lock; SaveCallback writes the copy lock-free
    }

    void LoadStockRecord(RE::FormID a_followerID, std::unordered_set<RE::FormID> a_set) {
        std::scoped_lock lk(g_stockMx);
        g_stockGear[a_followerID] = std::move(a_set);
    }

    void ClearStockGear() {
        std::scoped_lock lk(g_stockMx);
        g_stockGear.clear();
    }

}
