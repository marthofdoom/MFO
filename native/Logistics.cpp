#include "PCH.h"
#include "Logistics.h"
#include "Logistics_internal.h"   // the split modules' shared substrate
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Actuation.h"   // cast-in-logistics: reuse the combat cast path (Fire)
#include "CasterConsent.h"  // ClassifySpell: beneficial-vs-hostile OOC cast routing
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

// <windows.h> is BANNED outside Board.cpp (it #defines GetObject and hijacks
// BGSDefaultObjectManager::GetObject<T>) -- so declare the one Win32 call we
// need by hand, exactly as Targeting.cpp does. Used only for QuickLoot presence.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);

// The logistics table. DESIGN.md §4.8. Out-of-combat upkeep: drink H/S/M
// potions, loot arrows / potions / better gear -- one action per idle tick,
// behind bLogistics, and NEVER interleaved with the combat table (§4.8).

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


        // ── #62 BROKEN-GEAR sink ────────────────────────────────────────────
        // A trade / AI equip can put (or re-assert) a non-playable creature item or
        // a foreign non-rendering head piece on a follower -> invisible head. This
        // sink catches such equips at the EQUIP EVENT and posts KeepHeadClear, which
        // DELETES the creature junk and hands back the wrong-race piece (see its
        // definition -- plugin-ownership keeps the follower's own native gear).
        // Scoped to any TEAMMATE, out of combat, NAMED armor/weapon equips only (a
        // cheap trigger gate), debounced ~1s. KeepHeadClear's own unequip fires an
        // equipped=false event, ignored below -- no cascade.
        class BeastHeadSink final : public RE::BSTEventSink<RE::TESEquipEvent> {
        public:
            static BeastHeadSink* GetSingleton() { static BeastHeadSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_ev || !a_ev->equipped || !a_ev->actor)
                    return RE::BSEventNotifyControl::kContinue;
                if (!Config::g_beastHeadFix.load())
                    return RE::BSEventNotifyControl::kContinue;
                // Only WORN gear (armor or weapon) matters here.
                auto* base = a_ev->baseObject ? RE::TESForm::LookupByID(a_ev->baseObject) : nullptr;
                if (!base || (!base->Is(RE::FormType::Armor) && !base->Is(RE::FormType::Weapon)))
                    return RE::BSEventNotifyControl::kContinue;
                // TRIGGER only on NAMED gear equips (a cheap gate to avoid firing on
                // a follower's own hidden native-gear churn). The actual keep/clear
                // separation is done by plugin ownership inside KeepHeadClear.
                auto* named = base->As<RE::TESFullName>();
                if (!named || !named->GetFullName() || !*named->GetFullName())
                    return RE::BSEventNotifyControl::kContinue;
                auto* actor = a_ev->actor->As<RE::Actor>();
                if (!actor || !actor->IsPlayerTeammate() || actor->IsInCombat())
                    return RE::BSEventNotifyControl::kContinue;
                // DEBOUNCE (~1s/follower): coalesces a full-set trade's burst of
                // equip events. Main-thread, so the static map needs no lock.
                static std::unordered_map<RE::FormID, Clock::time_point> s_lastReset;
                const auto       now = Clock::now();
                const RE::FormID id  = actor->GetFormID();
                if (auto it = s_lastReset.find(id);
                    it != s_lastReset.end() && now - it->second < std::chrono::seconds(1))
                    return RE::BSEventNotifyControl::kContinue;
                s_lastReset[id] = now;
                // A trade/AI equip can leave (or re-assert) a non-rendering head item
                // on a follower. Post KeepHeadClear (next frame, after the
                // equip settles): it takes OFF any worn head-slot piece with no mesh
                // for his race -- the invisible-head cause -- and leaves everything
                // else, including his hidden native gear, untouched.
                MainThread::Post([id]() {
                    if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id))
                        KeepHeadClear(a);
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ── the player-looted waiver sink (#22h) ────────────────────────────
        class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
        public:
            static ContainerSink* GetSingleton() { static ContainerSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                                  RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                // Logistics off -> the waiver map is never read, so do no work.
                if (!Config::g_logistics.load()) return RE::BSEventNotifyControl::kContinue;

                // DIRECTION FILTER IS MANDATORY (#22h): only items ENTERING the
                // player count as a "take". Without it the sink re-triggers on
                // its own removal, which is MAO's infinite-credit loop. This also
                // encodes "the waiver keys on TAKING, never on looking" -- a
                // QuickLoot glance opens no menu and moves no item, so it fires
                // nothing here.
                if (a_event->newContainer != PlayerID()) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID srcID = a_event->oldContainer;
                if (srcID == 0) return RE::BSEventNotifyControl::kContinue;   // spawned into player, no source

                // #5 IGNORE MFO's OWN HAND-BACKS. ShedOffRoleWeapon, HealExcludedWeapon,
                // KeepHeadClear and TradeBridge deliveries all RemoveItem(follower ->
                // player); that fires this sink with oldContainer = a follower. Counting
                // it as a player "take" flips g_lastLootSource / the previous source's
                // rejected flag on a corpse the player may be mid-looting (a false
                // release). A genuine player take never comes OUT of a managed follower
                // or a teammate. IsTrackedFast is the off-worker-safe roster check
                // (Wave 1); the teammate lookup catches followers MFO doesn't manage.
                if (Followers::IsTrackedFast(srcID))
                    return RE::BSEventNotifyControl::kContinue;
                if (auto* srcActor = RE::TESForm::LookupByID<RE::Actor>(srcID);
                    srcActor && srcActor->IsPlayerTeammate())
                    return RE::BSEventNotifyControl::kContinue;

                // Sinks QUEUE; they never touch a main-thread map inline (#1/#4).
                // The timer RESETS on every take, so multiple QuickLoot takes push
                // the follower's window out to the LAST one.
                // SEV-1: the body mutates the save-scoped loot maps (g_playerLooted/
                // g_claim/g_lastLootSource), so it runs under PumpTickGate like every
                // other MFO AddTask body -- else a revert's StopPump/ResetAllState (or a
                // save's PausePump) could clear those maps while this writes them (#4).
                const auto epoch = MFO::Diagnostics::CurrentPumpEpoch();
                SKSE::GetTaskInterface()->AddTask([srcID, epoch]() {
                    MFO::Diagnostics::PumpTickGate gate(epoch);
                    if (!gate) return;
                    g_playerLooted[srcID] = Clock::now();
                    EvictOldest(g_playerLooted);
                    // R1: the player's take is from a DIFFERENT source than their
                    // last -> release the previous one's claim (they finished
                    // there). Runs on the same worker queue as the loot walk, so
                    // touching g_claim here is serial with it (no race, §0.30).
                    if (g_lastLootSource != 0 && g_lastLootSource != srcID) {
                        if (auto it = g_claim.find(g_lastLootSource); it != g_claim.end())
                            it->second.rejected = true;
                    }
                    g_lastLootSource = srcID;
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

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
    // already in hand or none is carried.
    bool EquipTorch(RE::Actor* a_follower) {
        if (auto* l = a_follower->GetEquippedObject(true); l && l->As<RE::TESObjectLIGH>())
            return false;
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
        // one. The Scheduler runs this path solely out of combat, but IsInCombat()
        // FLAPS false mid-fight (a moment of LoS loss / a disengage) and the
        // follower gets serviced through the lull -- which used to drop a looted
        // off-role weapon MID-FIGHT (field: a 2h follower handed the player a
        // looted 1h mace during a combat lull). So require a STABLE out-of-combat
        // window: NoteInCombat stamps g_lastCombatSeen from the Scheduler's
        // in-combat branch (the only place combat=true is observable here), and we
        // bail until kShedPostBattleDwell has passed since that stamp. A one-frame
        // flap re-stamps the instant combat resumes, so the dwell can never mature
        // inside a lull -- only once the battle has genuinely ended. Worker-only
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
            return true;   // fists/other -> never a shed target
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
            if (IsCreatureWeapon(w) || Catalog::IsExcluded(obj->GetFormID())) continue;
            // #69: never shed the follower's OWN gear, snapshotted at first
            // management (the Gauldurbow fix) -- an off-role signature weapon
            // stays in the pack, not handed to the player.
            if (IsStockGear(a_follower->GetFormID(), obj->GetFormID())) continue;
            if (!Config::g_lootSpecialItems.load() && socketed(data.second.get())) continue;
            if (!shed) { shed = obj; shedCount = data.first; }   // first off-role, one per tick
        }
        if (!shed || inRoleWeapons == 0) return false;   // don't disarm

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
            // DropObject removes from the actor AND places the world ref in one
            // engine call (nullptr drop-loc/rotate -> the engine drops it at the
            // actor's feet). Re-check the count on-frame: the follower may have
            // used/traded some between the worker read and this main-thread run.
            if (fol && item)
                fol->DropObject(item, nullptr, dropCount, nullptr, nullptr);
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

    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower) return;
        // #78: per-follower MFO master switch. The Scheduler already gates the
        // whole tick (combat + this OOC path) on mfoEnabled and releases held
        // state on the OFF edge, so this is pure defence-in-depth: a disabled
        // follower gets no logistics / economy / loot even if reached another way.
        if (!a_state.mfoEnabled) return;
        g_svc = &a_state;   // loot code reads the gambit table through this (worker-sequential)

        // #69: snapshot the follower's OWN gear the very FIRST time MFO manages
        // them -- before HealExcludedWeapon/ShedOffRoleWeapon/loot touch a
        // single item. First management is recruit for a new follower (nothing
        // looted yet); a no-op for anyone already snapshotted this session or
        // loaded from the co-save.
        EnsureStockSnapshot(a_follower);

        const auto id  = a_follower->GetFormID();
        const auto now = Clock::now();

        // Undo a wrongly-looted non-playable weapon (invisible on a follower) before
        // anything else this tick -- cheap + rate-limited (5 s), a no-op once healed.
        {
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextHeal;
            auto& hn = s_nextHeal[id];
            if (hn.time_since_epoch().count() == 0 || now >= hn) {
                hn = now + std::chrono::seconds(5);
                HealExcludedWeapon(a_follower);
            }
        }

        // GLOBAL travel-intent BACKSTOP, keyed to NO follower in particular, and
        // run BEFORE the logistics early-return on purpose. The per-follower
        // arrival branch below only runs when the TRAVELLER is serviced out of
        // combat -- but a traveller who died, was dismissed, dropped off the
        // roster, or got pulled down the combat branch is never serviced there,
        // so its intent (and the loot quest's raised priority) would stick all
        // session. This fires for EVERY follower, and also when the subsystem is
        // toggled OFF mid-travel -- otherwise turning bLogistics/bLootTravel off
        // would strand a traveller at priority 60. (An in-session load does NOT
        // revert the raised priority -- form data keeps the runtime value -- so
        // ReleaseAll's kPreLoadGame write is the real reset, not this; this is
        // the LIVE release. See ReleaseAll.)
        // Keyed to the whole-EXCURSION cap, not the per-leg deadline: during a
        // Hold the leg deadline is stale and would wrongly fire this. The cap
        // catches an excursion whose traveller is no longer being serviced.
        // (Combat yield is NOT here -- ServiceFollower is skipped for in-combat
        // followers, so this backstop never runs for the one who matters. The
        // yield lives in the Scheduler's combat branch, ReleaseTravelOnCombat.)
        const bool off = !Config::g_logistics.load() || !Config::g_lootTravel.load();
        // P7: sweep EVERY slot -- each traveller has his own excursion cap, and a
        // subsystem toggle-off must free them all. (Global backstop, keyed to no
        // follower in particular; runs once per serviced follower, idempotent.)
        for (int i = 0; i < Packages::kMaxLootSlots; ++i) {
            auto& tr = g_travelSlots[i];
            if (!tr.active) continue;
            const bool capped = now > tr.startTime + std::chrono::seconds(
                                          static_cast<int>(Config::g_excursionMax.load()));
            if (capped || off) {
                // On a cap hit blacklist the current target so it isn't re-picked
                // immediately. NOT on toggle-off (that corpse never "failed").
                if (!off) {
                    if (auto tp = tr.target.get()) MarkTravelFailed(tp->GetFormID(), now);
                }
                Packages::LootTravelClear(off ? "subsystem off" : "excursion cap", nullptr, i);
                tr = TravelIntent{};
            }
        }

        if (!Config::g_logistics.load()) return;   // whole subsystem off by default (#45)

        // CADENCE GATE (~1 s). Cheap early-out on the frames between logistics
        // ticks -- the Scheduler calls this every time it services the follower
        // out of combat (up to ~7.5 Hz), but logistics only acts at the idle rate.
        auto& due = g_nextTick[id];
        if (due.time_since_epoch().count() != 0 && now < due) return;
        due = now + kLogisticsInterval;

        // Hand back one off-role weapon per idle tick (AI-usable wrong-role gear /
        // pre-1.0.12 leftovers). Cheap when the pack is clean; stops on its own.
        ShedOffRoleWeapon(a_follower, a_state);

        // ── BATCH EXCURSION driver. While THIS follower is on a loot excursion
        // (claimed at priority 60), drive it: walk to the current target, grab it
        // on arrival, then seek the NEXT target and retarget WITHOUT releasing --
        // he hoovers a batch instead of returning to the player after each corpse.
        // Release only on combat, the excursion cap, leaving the leash, or the
        // batch running dry (after a short dibs linger).
        if (const int slot = SlotIndexOf(id); slot >= 0) {
            TravelIntent& tr = g_travelSlots[slot];
            auto* pc = RE::PlayerCharacter::GetSingleton();
            // HARD interrupts -> END the excursion.
            const bool overCap = now > tr.startTime + std::chrono::seconds(
                                          static_cast<int>(Config::g_excursionMax.load()));
            // HYSTERESIS on the leash (×1.15): a corpse near the leash edge puts
            // the follower in a thin band (>leash from player, still walking to an
            // in-leash target) where a bare check would release then instantly
            // re-fill to the same corpse and oscillate. The margin ends the batch
            // only once he is clearly past the leash.
            const bool outOfLeash = pc &&
                a_follower->GetPosition().GetDistance(pc->GetPosition())
                    > Confidence::LeashRadius(a_follower) * 1.15f;
            const bool inHome = !Config::g_lootInPlayerHomes.load() && InPlayerHome();
            if (a_follower->IsInCombat() || overCap || outOfLeash || inHome) {
                Packages::LootTravelClear(a_follower->IsInCombat() ? "combat"
                                          : (overCap ? "excursion cap"
                                          : (inHome ? "player home" : "left leash")),
                                          a_follower, slot);
                tr = TravelIntent{};
                // fall through to a normal eval this tick (combat table / follow).
            } else if (tr.acquirePending) {
                // ── ACQUIRE READBACK (route 2b probe). One tick after the
                // Activate dispatch, observe what the VM call actually did --
                // dispatch is asynchronous (Papyrus.h), so the previous tick's
                // call has had its frame(s) by now. Ref gone / inventory up =
                // the engine took it natively; ref standing with a flat count =
                // the activation was a no-op.
                tr.acquirePending = false;
                auto aptr = tr.target.get();
                auto* aref = aptr.get();
                const bool refGone = !aref || aref->IsDeleted() || aref->IsDisabled();
                std::int32_t post = 0;
                for (auto& [obj, n] : a_follower->GetInventoryCounts())
                    if (obj && obj->GetFormID() == tr.acquireBase) { post = n; break; }
                const std::int32_t delta = post - tr.acquirePre;
                if (refGone || delta != 0)
                    spdlog::info("[acquire] {:08X}: TOOK {:08X} -- ref {}, inv {:+}",
                                 id, tr.acquireRefID,
                                 refGone ? "gone" : "persists", delta);
                else
                    spdlog::info("[acquire] {:08X}: ACTIVATE NO-OP -- ref persists, inv unchanged",
                                 id);
                // Either way this leg is DONE: blocklist the ref (a persisting
                // no-op must not be re-picked every tick) and continue the batch.
                MarkTravelFailed(tr.acquireRefID, now);
                tr.phase = TravelPhase::Holding;
                tr.lingerUntil = now + BatchLingerDur();
                return;
            } else if (tr.phase == TravelPhase::Walking) {
                auto tptr  = tr.target.get();
                auto* tref = tptr.get();
                const float dist = tref ?
                    a_follower->GetPosition().GetDistance(tref->GetPosition()) : 1e9f;
                // DIAGNOSTIC (v0.8.6): is MFO's travel package driving him, and is
                // he closing on the target? onTravelPkg=true + shrinking dist =
                // working; onTravelPkg=true + flat dist = UNREACHABLE (no path).
                // WALK diagnostic -- RATE-LIMITED to ~once per 4 s per follower
                // (was every ~1 s tick: ~60 lines a session). The claim + gate are
                // proven now; this is kept only as a debugging surface for a future
                // freeze, so it need not be dense. dist shrinking is the real
                // signal; pathSpeed is noisy (reads 0 mid-walk) and kept only as a
                // hint. navdist calibrates the gate.
                if (tref) {
                    static std::unordered_map<RE::FormID, Clock::time_point> s_nextWalkDiag;
                    auto& nxt = s_nextWalkDiag[id];
                    if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                        nxt = now + std::chrono::seconds(4);
                        auto* cur = a_follower->GetCurrentPackage();
                        float pathSpeed = -1.0f;
                        if (auto* proc = a_follower->GetActorRuntimeData().currentProcess)
                            if (auto* high = proc->high)
                                pathSpeed = high->pathingCurrentMovementSpeed.Length();
                        spdlog::info("[loot] {:08X} WALK->{:08X}: onTravelPkg={} curPkg={:08X} prio={} "
                                     "dist={:.0f} pathSpeed={:.1f} navdist={:.0f}",
                                     id, tref->GetFormID(),
                                     Forms::IsTravelPackage(cur),
                                     cur ? cur->GetFormID() : 0u,
                                     Forms::g_lootQuest ? static_cast<int>(Forms::g_lootQuest->data.priority) : -1,
                                     dist, pathSpeed, NavmeshReach(a_follower, tref));
                    }
                }
                // Track real movement for the no-progress check below (update on
                // any >kMoveEps world move since the last note).
                const RE::NiPoint3 fpos = a_follower->GetPosition();
                if (tref && fpos.GetDistance(tr.lastPos) > kMoveEps) {
                    tr.lastPos    = fpos;
                    tr.progressAt = now;
                }
                const bool gone = !tref || tref->IsDisabled() || tref->IsMarkedForDeletion();

                // ARRIVAL is checked BEFORE the stall/deadline giveup: a follower
                // standing ON the corpse has ARRIVED, not "stalled walking" -- and
                // the considering/sneak HOLD below legitimately keeps him stationary
                // (he's waiting out YOUR QuickLoot). Letting the no-progress timer
                // fire during that hold would falsely blacklist a corpse he reached
                // and is politely waiting on (audit).
                // #(loose-loot fix): a LOOSE item Activates from a longer reach
                // (g_looseAcquireDist, default 300) -- the engine picks it up
                // regardless of physical distance, so a follower who walked as
                // close as the navmesh allows still grabs a pile the corpse-tight
                // 200u arm's reach would strand (marth: reachable gold, pickup
                // distance too small). A corpse gets its GROWN grab radius
                // (stall cure): each path-fail against it widened the from-range
                // reach up to kGrabRadiusMax, so a leg that stalls short of a
                // body on rubble converts to an arrival next tick instead of
                // striking toward the blocklist. A grown (beyond plain arm's
                // reach) arrival honours the player bubble -- never hoover a
                // body the player is standing over.
                float arrivalDist = kArrivalDist;
                if (tref && LooseRef(tref)) {
                    arrivalDist = Config::g_looseAcquireDist.load();
                } else if (tref) {
                    const float grown = GrabRadiusFor(tref->GetFormID());
                    if (grown > arrivalDist) {
                        auto* pcG = RE::PlayerCharacter::GetSingleton();
                        // Clutter (arrows/bolts/lockpicks) is exempt from the
                        // bubble -- tr.cat carries this leg's category.
                        if (IsClutterCat(tr.cat) || !pcG ||
                            pcG->GetPosition().GetDistance(tref->GetPosition()) >
                                        Config::g_playerBubble.load())
                            arrivalDist = grown;
                    }
                }
                if (!gone && dist <= arrivalDist) {
                    // REACHED it -> this body is provably reachable: clear any stall
                    // strike so a merely-transient earlier block (boxed in by an actor,
                    // a door) never accumulates toward the sticky verdict. Only bodies
                    // he can NEVER close on keep striking -> those go sticky (above).
                    g_stallStrikes.erase(tref->GetFormID());
                    g_stealStrikes.erase(StealKey(id, tref->GetFormID()));   // reachable -> reset steal back-off
                    // MUTATION BAR (#22g / #22g-QL) + sneak courtesy hold.
                    if (PlayerIsConsidering(tref->GetFormID()) || PlayerActivelyStealthing())
                        return;   // arrived, holding under the bar -- retry next tick
                    // ── ACQUIRE PROBE (route 2b): a LOOSE ref has no inventory
                    // to transfer -- StripCorpse/LootHere would no-op on it.
                    // Dispatch ObjectReference.Activate(follower) through the VM
                    // instead: the ENGINE runs the pickup on its own scheduling.
                    // NO PickUpObject and NO SKSE AddTask -- both land on the
                    // §0.30 worker, the crash4 class. Readback next tick.
                    if (LooseRef(tref)) {
                        auto* base = tref->GetBaseObject();
                        const char* iname = tref->GetDisplayFullName();
                        const auto icount = tref->extraList.GetCount();
                        std::int32_t pre = 0;
                        for (auto& [obj, n] : a_follower->GetInventoryCounts())
                            if (obj && base && obj->GetFormID() == base->GetFormID()) { pre = n; break; }
                        // NATIVE activate on the MAIN THREAD (loose-loot fix, marth:
                        // "it's simple -- check other looting mods"). The VM
                        // ObjectReference.Activate dispatch returned false for EVERY
                        // gold pile (field: Xelzaz reached them, `ACTIVATE dispatch
                        // FAILED` x9, never picked up). TESObjectREFR::ActivateRef is
                        // the exact call the engine runs when the PLAYER presses
                        // activate -- the reliable path loot mods use -- marshalled
                        // to the main thread via MainThread::Post so it stays off the
                        // §0.30 job worker (no PickUpObject/AddTask; the same hop
                        // #62's equip uses). Fire-and-forget: the readback next tick
                        // (inventory delta) decides took vs no-op.
                        const RE::FormID refID = tref->GetFormID();
                        const RE::FormID folID = a_follower->GetFormID();
                        auto doActivate = [refID, folID]() {
                            auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID);
                            auto* f = RE::TESForm::LookupByID<RE::Actor>(folID);
                            if (r && f) r->ActivateRef(f, 0, nullptr, 1, false);   // full processing = the pickup
                        };
                        if (!MainThread::IsInstalled()) {
                            // NO main-thread pump (VR): ActivateRef is a loose-item
                            // pickup / 3D teardown that must NEVER run on this job
                            // worker (the crash4 class). Don't fall back to doActivate()
                            // off-thread -- sticky the ref and move on, so the batch
                            // isn't churned re-attempting an unpickable pile.
                            MarkTravelSticky(tref->GetFormID(), now);
                            spdlog::info("[acquire] {:08X}: loose ref {:08X} STICKY -- no main-thread pump for ActivateRef",
                                         id, tref->GetFormID());
                            tr.phase = TravelPhase::Holding;
                            tr.lingerUntil = now + BatchLingerDur();
                            return;
                        }
                        MainThread::Post(doActivate);
                        spdlog::info("[acquire] {:08X}: ACTIVATE (native/main-thread) ref {:08X} ('{}' x{})",
                                     id, tref->GetFormID(), iname ? iname : "?", icount);
                        tr.acquirePending = true;
                        tr.acquireRefID   = tref->GetFormID();
                        tr.acquireBase    = base ? base->GetFormID() : 0;
                        tr.acquirePre     = pre;
                        return;   // the activate IS this tick's action; readback next tick
                    }
                    // Take EVERYTHING his gambits want in this one visit, not just
                    // the category the trip was for -- else gold trips strand the
                    // arrows (marth's 340u/382u bodies). Only THEN is the corpse
                    // genuinely DONE and safe to blocklist -- UNLESS a Gear/Valuables
                    // tier is still under the player's dibs (#2): then DON'T mark it
                    // DONE, so the linger revisits it once the claim releases.
                    bool leftWaiting = false;
                    const bool moved = StripCorpse(a_follower, a_state, tref, now, &leftWaiting);
                    if (moved || !leftWaiting)
                        g_grabGrow.erase(tref->GetFormID());   // handled -> stale grow verdict
                    if (!leftWaiting)
                        MarkTravelFailed(tref->GetFormID(), now);   // fully stripped -> DONE
                    tr.phase = TravelPhase::Holding;
                    tr.lingerUntil = now + BatchLingerDur();
                    spdlog::info("[loot] {:08X}: arrived -- {}{} (batch continues)", id,
                                 moved ? "looted" : "nothing to take",
                                 leftWaiting ? " (loot still under player's dibs -- corpse kept)" : "");
                    return;   // the transfer IS this tick's action; seek next tick
                }

                // ── PACKAGE-THEFT GUARD (RC#4, deck 12:25:18): the loot alias was
                // still filled at 60 yet curPkg flipped to a runtime FF package (a
                // scene / dialogue / framework claim) and pathSpeed died -- the
                // follower stood there while the stall clock convicted the REF and
                // 5-min stickied a perfectly reachable gold pile. An external hold
                // is NOT a reachability verdict: pause the stall/deadline clocks,
                // re-assert the claim (EvaluatePackage; the fill is intact, so the
                // engine re-picks travel the moment the scene lets go), and only
                // after kStealGrace concede the leg -- TRANSIENT blocklist, never
                // sticky/strike. Arrival above still runs during the hold, so a
                // steal beside the pile still grabs it.
                //
                // QUIET HOLD (field-proven 2026-09-03, ch.9 0x49 probe): for an
                // APMF-routed leg this whole guard is REDUNDANT and actively
                // harmful. The probe showed APMF's CheckForCurrentAliasPackage
                // hook already holds the loot-travel package on its own for a
                // framework-locked follower (Cicero: engine's own alias answer
                // was the framework package, 0x49 overrode it back to the APMF
                // package every tick) -- so a momentary framework curPkg here is
                // the expected, benign gap between the engine's own re-eval and
                // the hook's override, NOT a theft. Re-asserting (EvaluatePackage)
                // and accruing strikes against a hold that's already self-healing
                // is "as good as a fail" (marth): it escalates to abandon on a
                // leg that was never actually lost. Skip the guard entirely for
                // these legs -- 0x49 IS the re-assert. Legacy alias-route legs
                // (APMF absent, IsAPMFTravelHeld false) run the unchanged guard
                // below, byte-identical.
                bool stealAbandon = false;
                if (!gone && tref && Packages::IsAPMFTravelHeld(slot)) {
                    // Nothing to do: the 0x49 hold self-heals. Fall through to the
                    // arrival/stall/deadline checks below unconditionally -- a
                    // genuinely stuck APMF leg (path fail, not a package steal)
                    // still stalls/deadlines out the normal way.
                } else if (!gone && tref) {
                    if (!Forms::IsTravelPackage(a_follower->GetCurrentPackage())) {
                        const auto skey = StealKey(id, tref->GetFormID());
                        if (tr.stolenSince.time_since_epoch().count() == 0) {
                            tr.stolenSince = now;
                            const int strikes = ++g_stealStrikes[skey];
                            auto* curp = a_follower->GetCurrentPackage();
                            spdlog::info("[loot] {:08X} travel pkg stolen mid-walk "
                                         "(curPkg={:08X}) -- re-asserting claim, grace {}s (strike {}/{})",
                                         id, curp ? curp->GetFormID() : 0u,
                                         std::chrono::duration_cast<std::chrono::seconds>(kStealGrace).count(),
                                         strikes, kStealStrikeMax);
                        }
                        // BACK-OFF (deck RC): a claim stolen kStealStrikeMax times is
                        // fighting a package that will not release -- stop re-asserting,
                        // abandon the leg to the transient blocklist and move on. Also
                        // concede immediately IN COMBAT: a combat package legitimately
                        // outranks loot-travel and must never be fought (the Scheduler
                        // clears loot on combat anyway, but this tick may beat it).
                        const bool tooManySteals = g_stealStrikes[skey] >= kStealStrikeMax;
                        const bool folInCombat   = a_follower->IsInCombat();
                        if (tooManySteals || folInCombat) {
                            MarkTravelFailed(tref->GetFormID(), now);   // transient only -- ref was reachable
                            g_stealStrikes.erase(skey);
                            spdlog::info("[loot] {:08X} leg {:08X} abandoned -- {} -- transient skip",
                                         id, tref->GetFormID(),
                                         folInCombat ? "in combat, conceding to combat package"
                                                     : "claim stolen too many times, backing off");
                            tr.stolenSince = {};
                            stealAbandon = true;
                            tr.phase = TravelPhase::Holding;
                            tr.lingerUntil = now + BatchLingerDur();
                            // no return -- fall into Holding
                        } else if (now - tr.stolenSince <= kStealGrace) {
                            tr.progressAt = now;   // stolen time never counts against the ref
                            if (tr.deadline < now + std::chrono::seconds(4))
                                tr.deadline = now + std::chrono::seconds(4);
                            a_follower->EvaluatePackage(true, false);   // nudge; never resetAI
                            return;   // hold the leg -- reclaim pending
                        }
                        // External claim outlasted the grace: give the LEG up, keep
                        // the ref honest (25s transient only) and seek/release below.
                        MarkTravelFailed(tref->GetFormID(), now);
                        g_stealStrikes.erase(skey);
                        spdlog::info("[loot] {:08X} leg {:08X} abandoned -- package held "
                                     "externally past grace -- transient skip",
                                     id, tref->GetFormID());
                        tr.stolenSince = {};
                        stealAbandon = true;
                        tr.phase = TravelPhase::Holding;
                        tr.lingerUntil = now + BatchLingerDur();
                        // no return -- fall into Holding
                    } else if (tr.stolenSince.time_since_epoch().count() != 0) {
                        // Reclaimed: resume the leg with a fresh movement budget
                        // AND a fresh distance-scaled deadline (the steal-era one
                        // was only ever nudged 4 s ahead -- too short to walk out).
                        tr.stolenSince = {};
                        tr.progressAt  = now;
                        tr.deadline    = TravelDeadline(dist, now);
                    }
                }

                // NOT arrived: leg fails on vanished target, no-progress (no path),
                // or the leg deadline. Blacklist it and seek another leg THIS tick.
                const bool stalled = tref && now - tr.progressAt > kNoProgress;
                if (stealAbandon) {
                    // handled above -- skip the stall/deadline blame path
                } else if (gone || stalled || now > tr.deadline) {
                    if (tref) {
                        // A stall (zero progress) is a reachability verdict -> strike
                        // toward sticky; a vanished target or a plain deadline is not.
                        // A LOOSE pile that stalls is beyond even the generous
                        // g_looseAcquireDist Activate reach AND never moves -> a
                        // single verdict is geometric, so sticky it straight off
                        // rather than strike-accrue across a cluster (the churn loop).
                        if (stalled) {
                            if (LooseRef(tref)) MarkTravelSticky(tref->GetFormID(), now);
                            else {
                                MarkTravelStalled(tref->GetFormID(), now);
                                // A REAL walked-and-couldn't-close verdict: widen
                                // this body's from-range grab radius so the next
                                // scan (or this excursion's next leg) takes it
                                // from where he can actually stand (stall cure).
                                NotePathFail(tref->GetFormID());
                            }
                        } else {
                            MarkTravelFailed(tref->GetFormID(), now);
                        }
                    }
                    if (stalled && tref)
                        spdlog::info("[loot] {:08X} unreachable {:08X} (no progress, dist={:.0f}) -- skipping",
                                     id, tref->GetFormID(), dist);
                    tr.phase = TravelPhase::Holding;
                    tr.lingerUntil = now + BatchLingerDur();
                    // no return -- fall into Holding
                } else {
                    return;   // still walking to the current target
                }
            }

            // ── HOLDING: seek the next leg (retarget to a walkable corpse) or
            // grab one within arm's reach; else the batch is exhausted -> release
            // and return to the player. marth's "no pauses" loot-ordering spec:
            // a category that's still under the player's dibs (g_scanSawWaiting)
            // no longer earns a linger-and-re-scan hold here -- that WAS a real,
            // if short (fBatchLinger-bounded), stall-in-place. Release at once
            // instead; the dibs-waiting item re-enters naturally (TierReleased
            // flips, normal-mode loot picks it up) once it clears -- ordering
            // (free-tier swept first, see the pass-0/pass-1 split above) is what
            // keeps the follower busy in the meantime, not holding position.
            if (tr.active && tr.phase == TravelPhase::Holding) {
                // The two loot bars (a ContainerMenu open, or the player sneaking)
                // make LootNearby early-return BEFORE it scans, so the excursion
                // scan would come back empty with g_scanSawWaiting false and be
                // misread as "batch exhausted" -> premature release. Treat them as
                // a HOLD (bounded by the excursion cap), the same courtesy arrival
                // gives -- don't let a menu/crouch gut the batch.
                auto* ui = RE::UI::GetSingleton();
                if ((ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) ||
                    PlayerActivelyStealthing())
                    return;   // hold, retry next tick
                if (RunExcursionScan(a_follower, a_state, now)) {
                    // Retargeted (phase now Walking) or grabbed a cluster corpse
                    // (still Holding) -- productive, so extend the linger.
                    if (tr.phase == TravelPhase::Holding)
                        tr.lingerUntil = now + BatchLingerDur();
                    return;
                }
                // No pause on an all-waiting dibs category (marth): even a
                // g_scanSawWaiting-bounded hold here is a visible stall, so
                // release the excursion immediately instead of lingering --
                // the still-waiting item re-enters on a later tick once
                // TierReleased flips. tr.lingerUntil is still written above
                // (productive-scan case) but no longer gates a release.
                Packages::LootTravelClear("batch done", a_follower, slot);
                tr = TravelIntent{};
                return;
            }
        }

        if (a_state.logistics().empty()) return;   // no rules -> nothing to run

        // FALL-THROUGH scan. Try matching rules in order until one ACTUALLY
        // acts. A rule that matches but whose action finds nothing -- "loot
        // potions" with no potions nearby, "loot better gear" with no upgrade,
        // a drink on cooldown -- must NOT shadow the useful rules below it. That
        // shadow was why looting looked dead: the near-always-true low-potions
        // rule (a Requiem follower rarely carries >=2) won every tick and found
        // nothing, so "loot equipment" was never reached. Still at most ONE real
        // action per tick (§4.8.3); the loop only skips PAST non-acting matches.
        bool  acted = false;
        int   fired = -1;
        std::string label;   // a COPY -- `choice` is loop-scoped; c_str() would dangle
        // LOOT RUN ORDERING (marth: "no-dibs + closest only first, then the
        // dibs items later in the runs, no pauses"). Pass 0 walks the gambit
        // table with dibs-tier loot ops (Equipment/Gold/Jewelry/SoulGems/
        // Valuables) deferred, so free-tier loot -- and drink/cast/torch,
        // untouched -- always gets first crack at the tick regardless of the
        // player's gambit-table order. Pass 1 is the original unrestricted
        // walk: a released dibs item still loots normally once nothing
        // free-tier fired. Ordering only -- TierReleased's timing is untouched.
        for (int pass = 0; pass < 2 && !acted; ++pass) {
        const bool restrictFree = (pass == 0);
        for (int start = 0; ; ) {
            const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
            if (choice.ruleIndex < 0) break;          // nothing (more) matched
            const auto& op = choice.actionOpcode;
            fired = choice.ruleIndex;
            label = op;

            if (restrictFree && IsDibsTierLootOp(op)) {
                start = choice.ruleIndex + 1; continue;   // dibs-tier: defer to pass 1
            }

            if      (op == Vocab::kActDrinkHealthPotion)  acted = DrinkBest(a_follower, RE::ActorValue::kHealth);
            else if (op == Vocab::kActDrinkStaminaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kStamina);
            else if (op == Vocab::kActDrinkMagickaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kMagicka);
            else if (op == Vocab::kActLootArrows)         acted = LootNearby(a_follower, Category::Arrows, now);
            else if (op == Vocab::kActLootBolts)          acted = LootNearby(a_follower, Category::Bolts,  now);
            else if (op == Vocab::kActLootPotions)        acted = LootNearby(a_follower, Category::Potions, now);   // any drinkable
            else if (op == Vocab::kActLootHealthPotion)   acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kHealth);
            else if (op == Vocab::kActLootStaminaPotion)  acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kStamina);
            else if (op == Vocab::kActLootMagickaPotion)  acted = LootNearby(a_follower, Category::Potions, now, RE::ActorValue::kMagicka);
            else if (op == Vocab::kActLootEquipment)      acted = LootNearby(a_follower, Category::Equipment, now);
            else if (op == Vocab::kActLootGold)           acted = LootNearby(a_follower, Category::Gold, now);
            else if (op == Vocab::kActLootJewelry)        acted = LootNearby(a_follower, Category::Jewelry, now);
            else if (op == Vocab::kActLootSoulGems)       acted = LootNearby(a_follower, Category::SoulGems, now);
            else if (op == Vocab::kActLootLockpicks)      acted = LootNearby(a_follower, Category::Lockpicks, now);
            else if (op == Vocab::kActLootIngredients)    acted = LootNearby(a_follower, Category::Ingredients, now);
            else if (op == Vocab::kActLootValuables)      acted = LootNearby(a_follower, Category::Valuables, now);
            else if (op == Vocab::kActEquipTorch)         acted = EquipTorch(a_follower);   // #35: torch is upkeep
            else if (op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                     op == Vocab::kActCastPlayer) {
                // CAST IN LOGISTICS: out-of-combat gambit casting -- candlelight on
                // SELF or on the PLAYER (light follows you), magelight, self-buffs,
                // out-of-combat heals. Out of combat the follower's AI never casts
                // on its own, so the combat AI-grace path never fired (deck
                // 2026-08-06). ROUTING:
                //   * SELF + bCastSelf ON  -> Actuation::CastSelfDirect, the
                //     UNIVERSAL direct trigger (SPEC-self-cast-forced): equip +
                //     drive the caster + apply the effect + spend magicka, no
                //     package/alias so it drives package-locked custom followers
                //     too. This is the ONLY self route while the gate is on -- a
                //     concentration ward instant-applied here had no channel and
                //     STUCK forever, even after the rule was disabled (deck
                //     2026-08-17); the direct trigger is a bounded one-shot.
                //   * ANY CONCENTRATION spell at a NON-SELF target (player, ally,
                //     foe) -> Actuation::CastTargetDirect, the KNOWN-WORKING DIRECT
                //     FORCE (CastSpellImmediate straight onto the target + magicka
                //     deduct + bounded stream). NO package -- so it beats a package-
                //     locked custom follower's §4.6 alias lock (Lucien 2F00591F: the
                //     package route [pkg]-DECLINED every tick and his on-player heal
                //     never landed; deck build 5f8e873). Bounded/released by
                //     TargetCastReconcile (hostile 1-4s LoS+LoF-gated, heal 6s but
                //     re-applies while wounded, utility 4s + dispel-on-stop); the
                //     apply beats at ~1s (kConcApplyPeriod -- per-second authored
                //     magnitude, the heal cadence contract).
                //   * SELF (gate off) CONCENTRATION -> skipped legibly (self needs
                //     bCastSelf); never direct-applied on self behind the toggle.
                //   * SELF (gate off) + PLAYER, FIRE-AND-FORGET -> CastSpellImmediate
                //     (applies the effect to any target; no animation, but it lands),
                //     posted to the MAIN thread (#14).
                //   * FIRE-AND-FORGET hostile at a FOE -> the animated package (CastAt),
                //     with a DIRECT-FORCE fallback when the package §4.6-declines
                //     (package-locked follower) so the cell still delivers.
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(choice.actionParam);
                if (!sp || !a_follower->HasSpell(sp)) {
                    start = choice.ruleIndex + 1; continue;   // unknown spell -> next rule
                }
                // SUMMON SPAM GUARD (v1.1.1). A conjured/reanimated creature is a
                // COMMANDED ACTOR, not a caster-side magic effect, so every already-
                // active guard below (each reads a magic effect on the cast TARGET)
                // misses it and this gambit re-summons every cadence (marth, field).
                // Suppress while the follower already commands a LIVE summon from
                // THIS spell -- caster-side, per-spell, keyed on the live actor so a
                // killed/expired summon recasts at once. No-op for any non-summon
                // spell (candlelight/buffs/heals carry no summon archetype), so the
                // beneficial/light routing below is unchanged. Fall to the next rule
                // exactly like the "buff still on the target" skip.
                if (Actuation::CasterHasLiveSummon(a_follower, sp)) {
                    start = choice.ruleIndex + 1; continue;
                }
                // Resolve the EFFECT target: self, the player, or the current foe.
                RE::Actor* tgt = a_follower;
                if (op == Vocab::kActCastPlayer) {
                    tgt = RE::PlayerCharacter::GetSingleton();
                } else if (op == Vocab::kActCastTarget) {
                    // AUTO (marth): the board's default "Auto" pick (Subject::Self,
                    // no subject actor, no selector target) infers the set from the
                    // spell. Out of combat only the BENEFICIAL branches have targets
                    // -- a self-delivery buff (Candlelight) -> the caster, an aimed
                    // heal/buff -> injured/uncovered party -- and the hostile branch
                    // finds no combat group and NoOps. Actuation::CastAuto owns the
                    // whole routing/cadence/magicka/already-active guard, so run it
                    // and fall through (fired -> stop this tick; NoOp -> next rule).
                    auto p = choice.target.get();
                    const bool autoPick =
                        choice.subjectActorForm == 0 &&
                        static_cast<Vocab::Subject>(choice.subject) == Vocab::Subject::Self &&
                        !p.get();
                    if (autoPick) {
                        acted = (Actuation::CastAuto(a_follower, sp->GetFormID()).result ==
                                 Actuation::Result::Fired);
                        if (acted) break;
                        start = choice.ruleIndex + 1; continue;
                    }
                    if (p.get()) {
                        tgt = p.get();
                    } else {
                        // NOT auto, and no selector target this tick: a MANUAL pick
                        // (Target = Nearest ally / a named follower / Player).
                        // Resolve it through the SAME ladder the combat Fire path
                        // uses (Actuation::ResolveCastTarget) so a logistics "Cast
                        // at foe/ally, Target = Nearest ally" rule actually FIRES out
                        // of combat instead of being silently dropped (Wave 3 #9 /
                        // review SEV-2). A foe selector that found nobody OOC already
                        // routed through the AUTO branch above (Self + no-actor +
                        // no-target -> CastAuto, which NoOps with no combat group),
                        // so reaching here is a beneficial manual pick -- the ladder's
                        // player fallback rung is safe, never a hostile self-cast.
                        bool castFallbackPlayer = false;
                        tgt = Actuation::ResolveCastTarget(a_follower, choice, castFallbackPlayer);
                        if (!tgt) { start = choice.ruleIndex + 1; continue; }   // unresolvable -> next rule
                    }
                }
                if (!tgt) { start = choice.ruleIndex + 1; continue; }
                const bool conc = sp->GetCastingType() ==
                                  RE::MagicSystem::CastingType::kConcentration;
                // FORCED SELF-CAST (SPEC-self-cast-forced): a bCastSelf-armed SELF
                // cast routes to Actuation::CastSelfDirect (direct effect apply +
                // magicka, self-paced channel, NO package/equip/animation), NOT the
                // silent immediate apply -- so self never takes the immediate route
                // while the gate is on. `selfPkg` is a legacy name for that route.
                const bool selfPkg   = (op == Vocab::kActCastSelf) && Config::g_castSelf.load();

                // ── CONCENTRATION -> DIRECT FORCE (package-lock-proof) ──────────
                // marth's ruling after the deck fail: OOC concentration delivery
                // must "always use the known working force" and "avoid the package
                // route for anything." The package route c539257 added here
                // §4.6-DECLINED every tick for package-locked custom followers
                // (Lucien 2F00591F -- prio-80 quest owns the cast alias), so his
                // on-PLAYER heal never landed. So every NON-self concentration cast
                // (player, ally, foe) now delivers through Actuation::CastTargetDirect
                // -- CastSpellImmediate straight onto the target + magicka deduct, NO
                // package -- the SAME force that already heals SELF (CastSelfDirect).
                // It is bounded/released by TargetCastReconcile (hostile 1-4s
                // LoS+LoF-gated, heal 6s but re-applies while the HP rule wins so a
                // wounded target is topped up, utility 4s + dispel-on-stop for a
                // lingering ward). SELF is NOT delivered here: selfPkg (bCastSelf on)
                // took CastSelfDirect above; a self target with the gate OFF is
                // skipped legibly (never direct-applied on self behind the toggle).
                // The stream self-paces at ~1s (kConcApplyPeriod -- a concentration
                // magnitude/cost is authored PER SECOND, so the 4s fCastCooldown
                // would quarter heal throughput, the "heals feel broken" bug), so
                // it bypasses the FF already-active pre-skip and the
                // g_logiCastUntil window below.
                if (!selfPkg && conc) {
                    if (tgt == a_follower) {
                        // Self with the gate off -- self-casting is disabled; decline
                        // legibly rather than direct-apply behind bCastSelf's back.
                        spdlog::info("[cast] {:08X} self concentration {:08X} skipped -- "
                                     "needs bCastSelf", id, sp->GetFormID());
                        start = choice.ruleIndex + 1; continue;
                    }
                    // Same SEV-3 seed as the FF-at-foe branch below: CastTargetDirect's
                    // offense gate reads Sightline::Check (Actuation_Direct), and OOC
                    // nothing else Want()s this pair. Warm it a frame ahead so an OOC
                    // damage STREAM (Flames at a foe) actually holds on a wall instead
                    // of channelling through it. Only for a HOSTILE target -- a heal
                    // stream at an ally is never LoS-gated, so skip the raycast there.
                    if (tgt->IsHostileToActor(a_follower))
                        Sightline::Want(id, { tgt->GetFormID() });
                    const auto r = Actuation::CastTargetDirect(a_follower, sp, tgt);
                    if (r == Actuation::SelfCast::Applied) {
                        spdlog::info("[logistics] {:08X} OOC concentration {:08X} -> {:08X} "
                                     "(direct force, bounded)", id, sp->GetFormID(), tgt->GetFormID());
                        break;                          // delivered -> done this tick
                    }
                    // Refreshed (paced this tick) OR Declined (unaffordable / off-AE /
                    // LoS+LoF-held for offense): both TRANSPARENT -- fall to the next
                    // rule. NEVER the FF direct-apply below (a per-second effect there
                    // has no bounded channel and would stick, the old stuck-ward bug).
                    start = choice.ruleIndex + 1; continue;
                }

                // ── FIRE-AND-FORGET from here down (concentration handled above) ──
                // ROUTE A kActCastTarget CAST (field 2026-08-18: "logistics when ally
                // hp below doesn't seem to count player"). The ally SELECTOR already
                // resolves the wounded PLAYER as `tgt` (Evaluator::PickAlly considers
                // the player), but this branch used to send EVERY cast_target through
                // Packages::CastAt -- the animated FOE package (alias 0, a QNAM foe-
                // target). A beneficial heal aimed through the foe package does NOT
                // beneficially land on the player/ally, so the heal never happened.
                // FIX: only a HOSTILE spell aimed at an actual FOE takes the foe
                // package. A beneficial spell, OR ANY spell aimed at an ally/player,
                // is applied DIRECTLY to `tgt` (CastSpellImmediate) exactly like the
                // self/player immediate route below -- MFO applies effects directly,
                // bypassing engine delivery, the same way CastAuto/CastSelfDirect and
                // combat's CastOn already land a heal on whoever needs it.
                const bool hostileSpell =
                    CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Offense;
                const bool foeTarget = tgt != a_follower && tgt->IsHostileToActor(a_follower);
                const bool castAtFoe = (op == Vocab::kActCastTarget) && hostileSpell && foeTarget;
                // self(gate off)/player -> immediate; beneficial-or-ally cast_target
                // -> immediate (direct heal); hostile-on-foe cast_target -> CastAt.
                const bool immediate = !selfPkg && !castAtFoe;
                // Skip re-casting a buff still on the TARGET (candlelight up on him
                // or the player), then pace by the spell's DURATION (a 60s light
                // refreshes as it expires; a 3s floor keeps instant spells off the
                // ~1s tick -- heals stay condition-gated). The package self route
                // does NOT skip on effect-present: a still-active channel is paced
                // by its own hold below, and a stuck instant-applied ward from an
                // old build must not block the animated re-cast that replaces it.
                // (Concentration never reaches here -- it routed to the bounded
                // stream above -- so `immediate` is a FIRE-AND-FORGET beneficial
                // cast whose duration buff must not be re-stacked while still up.)
                if (immediate) {
                    auto* ei   = sp->GetCostliestEffectItem();
                    auto* mgef = ei ? ei->baseEffect : nullptr;
                    if (auto* mt = tgt->AsMagicTarget(); mgef && mt && mt->HasMagicEffect(mgef)) {
                        start = choice.ruleIndex + 1; continue;
                    }
                }
                // Pace per (follower, SPELL) -- keying on the follower alone would
                // let one cast's window starve a sibling cast rule (candlelight
                // blocking an OOC heal). Composite key like g_drinkUntil. The window
                // map is g_logiCastUntil at namespace scope (cleared on revert).
                const std::uint64_t castKey = (static_cast<std::uint64_t>(id) << 32) | sp->GetFormID();
                // SELF bypasses this window: it is a self-PACED channel that must
                // be re-fired every service to stay refreshed (the channel's own
                // registry paces the effect + releases when the rule stops). The
                // window would starve those re-fires and tear the channel down.
                if (!selfPkg)
                    if (auto it = g_logiCastUntil.find(castKey); it != g_logiCastUntil.end() && now < it->second) {
                        start = choice.ruleIndex + 1; continue;   // within this spell's window
                    }
                if (selfPkg) {
                    // The UNIVERSAL DIRECT TRIGGER (SPEC-self-cast-forced).
                    // Actuation::CastSelfDirect applies the effect DIRECTLY
                    // (CastSpellImmediate, kInstant) + spends magicka -- NO equip,
                    // NO package, NO animation (deferred): a held light spell let
                    // the follower's AI spam-cast it into a light-limit CTD, so the
                    // equip/channel scaffolding was removed. It drives package-
                    // locked custom followers too. Re-fire it EVERY service so the
                    // channel stays alive while the rule wins -- no s_logiCastUntil
                    // window here. F3: it returns Applied / Refreshed / Declined, so
                    // a mere pacing REFRESH (no effect applied this tick) does NOT
                    // count as this tick's action -- otherwise an "always ->
                    // cast_self" would break the scan every tick and starve
                    // loot/drink. Only a real Applied is the action.
                    const auto r = Actuation::CastSelfDirect(a_follower, sp);
                    if (r == Actuation::SelfCast::Applied) {
                        acted = true;
                    } else {
                        // Refreshed (channel kept alive, paced out this tick) OR
                        // Declined (unaffordable / off-AE) -- both TRANSPARENT: the
                        // refresh already happened inside CastSelfDirect, so just
                        // fall through to let the rules below run this tick.
                        start = choice.ruleIndex + 1; continue;
                    }
                } else if (immediate) {
                    // FIRE-AND-FORGET beneficial direct-apply (self-gate-off / player /
                    // ally). Concentration can NEVER reach here -- it delivered via the
                    // direct-force stream (CastTargetDirect) above -- so there is no
                    // stuck-per-second-effect hazard on this path: an FF spell's effect
                    // has its own authored duration and releases itself.
                    // CastSpellImmediate applies the effect to `tgt` for any delivery.
                    // No charge animation, but the light/buff/heal lands. Spends no
                    // magicka, so gate on affordability and deduct the cost by hand.
                    // THREADING (#14): the engine cast MUST run on the MAIN thread --
                    // this block used to call CastSpellImmediate INLINE on the AddTask
                    // job worker (the prime suspect for the queued 1.5.x act.cast_target
                    // AV crash reports). Post it like ApplySelfEffect/ApplyTargetEffect
                    // do: re-resolve by FormID inside (never carry an Actor* across
                    // threads) and clamp the deduct to the main-thread pool so it can
                    // never drive magicka negative.
                    const float cost = sp->CalculateMagickaCost(a_follower);
                    auto* avo = a_follower->AsActorValueOwner();
                    if (avo && avo->GetActorValue(RE::ActorValue::kMagicka) < cost) {
                        start = choice.ruleIndex + 1; continue;   // can't afford it
                    }
                    auto doCast = [casterID = id, tgtID = tgt->GetFormID(),
                                   spID = sp->GetFormID()] {
                        auto* f = RE::TESForm::LookupByID<RE::Actor>(casterID);
                        auto* t = RE::TESForm::LookupByID<RE::Actor>(tgtID);
                        auto* s = RE::TESForm::LookupByID<RE::SpellItem>(spID);
                        if (!f || !t || !s) return;
                        auto* caster = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                        if (!caster) return;   // F4: no caster -> no cast, no deduct
                        auto* mavo = f->AsActorValueOwner();
                        const float pool = mavo ? mavo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                        caster->CastSpellImmediate(s, false, t, 1.0f, false, 0.0f, f);
                        const float c     = s->CalculateMagickaCost(f);
                        const float spend = mavo ? std::min(c, pool) : 0.0f;   // never negative
                        if (mavo && spend > 0.0f)
                            mavo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                    RE::ActorValue::kMagicka, -spend);
                    };
                    // VR has no pump (Post is a documented no-op there): fall back
                    // to the old inline call rather than silently casting nothing.
                    if (MainThread::IsInstalled()) MainThread::Post(doCast);
                    else                           doCast();
                    acted = true;   // optimistic, same as the posted self/target applies
                } else {
                    // FF HOSTILE at a FOE -> the animated alias-0 foe package. For a
                    // PACKAGE-LOCKED custom follower (Lucien 2F00591F) that claim
                    // §4.6-declines every tick, and "packages off" declines it too --
                    // either way the cell must still deliver (marth: no combo may
                    // silently not work). DIRECT-FORCE fallback: silent one-shot
                    // CastSpellImmediate, LoS + line-of-fire gated (never a firebolt
                    // into a wall or through a teammate), affordability-gated, posted
                    // to the main thread with the deduct clamped -- the same delivery
                    // combat's silent force-half uses when ITS package declines.
                    //
                    // SEED THE LoS CACHE (2026-08-18 review SEV-3). This is the
                    // ONLY Sightline::Check in the OOC path, and nothing here ever
                    // Want()ed the pair -- so the cache stayed cold, Check read
                    // Unknown forever, and "!= Occluded" passed every time: the
                    // wall-gate was INERT (an OOC firebolt into masonry, or through
                    // a tent that HAS LoS collision, was never held). The combat
                    // paths all warm one tick ahead (Evaluator Want->Check, the F7
                    // auto-cast Want->Check); the OOC path never joined that pattern
                    // because PickFoe -- its only other seeder -- walks the COMBAT
                    // target list and does not run for a follower who is casting at
                    // a hostile while out of combat. Want() the pair EVERY service
                    // (unconditional, before the package attempt) so the measurement
                    // lands next frame and the fallback's Check gates on a real
                    // verdict. Fail-open is preserved: the FIRST tick still reads
                    // Unknown and passes, exactly like the combat gates on their
                    // first sighting -- walls do not move, so the second tick holds.
                    Sightline::Want(id, { tgt->GetFormID() });
                    acted = Packages::Available() &&
                            Packages::CastAt(a_follower, sp, tgt) == Packages::Decline::None;
                    if (!acted &&
                        Sightline::Check(id, tgt->GetFormID()) != Sightline::Verdict::Occluded &&
                        !Sightline::TeammateInFireLine(id, tgt->GetFormID())) {
                        const float cost = sp->CalculateMagickaCost(a_follower);
                        auto* avo = a_follower->AsActorValueOwner();
                        if (!avo || avo->GetActorValue(RE::ActorValue::kMagicka) >= cost) {
                            auto doCast = [casterID = id, tgtID = tgt->GetFormID(),
                                           spID = sp->GetFormID()] {
                                auto* f = RE::TESForm::LookupByID<RE::Actor>(casterID);
                                auto* t = RE::TESForm::LookupByID<RE::Actor>(tgtID);
                                auto* s = RE::TESForm::LookupByID<RE::SpellItem>(spID);
                                if (!f || !t || !s) return;
                                auto* caster = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                                if (!caster) return;
                                auto* mavo = f->AsActorValueOwner();
                                const float pool = mavo ? mavo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                                caster->CastSpellImmediate(s, false, t, 1.0f, false, 0.0f, f);
                                const float c     = s->CalculateMagickaCost(f);
                                const float spend = mavo ? std::min(c, pool) : 0.0f;
                                if (mavo && spend > 0.0f)
                                    mavo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                            RE::ActorValue::kMagicka, -spend);
                            };
                            if (MainThread::IsInstalled()) MainThread::Post(doCast);
                            else                           doCast();   // VR: no pump
                            spdlog::info("[logistics] {:08X} OOC hostile FF {:08X} at {:08X} -- "
                                         "package declined, DIRECT FORCE fallback",
                                         id, sp->GetFormID(), tgt->GetFormID());
                            acted = true;
                        }
                    }
                }
                if (acted && !selfPkg) {
                    auto* ei = sp->GetCostliestEffectItem();
                    // 3s floor keeps instant spells off the ~1s tick; 300s ceiling
                    // mirrors the cast duration cap so a pathological effect duration
                    // can't open an unbounded window (which a stale FF-reused key
                    // would then carry into the next save).
                    const float dur = std::clamp(
                        ei ? static_cast<float>(ei->GetDuration()) * 0.9f : 3.0f, 3.0f, 300.0f);
                    g_logiCastUntil[castKey] = now + std::chrono::duration_cast<Clock::duration>(
                                                        std::chrono::duration<float>(dur));
                    const char* route =
                          op == Vocab::kActCastPlayer                   ? "player (immediate)"
                        : (op == Vocab::kActCastTarget && immediate)    ? "ally/player (immediate)"
                        : immediate                                     ? "self (immediate)"
                                                                        : "foe (package)";
                    spdlog::info("[logistics] {:08X} OOC cast {:08X} ({}), refresh in {:.0f}s",
                                 id, sp->GetFormID(), route, dur);
                }
            }
            else if (op == Vocab::kActWait) {
                return;   // Wait consumes the tick and suppresses below (#3.3) -- stops the scan.
            }
            else {
                // Unknown / non-logistics opcode: skip PAST it so a stray rule
                // cannot shadow the rest (does not fall through to a real action).
                spdlog::info("[logistics] {:08X} rule {} has non-logistics action '{}' -- skipped",
                             id, choice.ruleIndex, op);
                start = choice.ruleIndex + 1;
                continue;
            }

            if (acted) break;                  // did something real -> done this tick
            start = choice.ruleIndex + 1;      // matched but no-op -> try the next rule
        }
        }   // end pass loop (no-dibs-first loot ordering)

        if (acted) {
            g_idleCycles.erase(id);   // productive -> not idle
            spdlog::info("[logistics] {:08X} rule {} fired: {}", id, fired, label);
        } else {
            // IDLE this tick. After a few idle ticks with no excursion running,
            // wipe the travel blocklist so previously-skipped bodies (the 340u/382u
            // arrow corpses, or ones that were unreachable before he moved) get a
            // fresh assessment -- bounded to once per kReassessCooldown so a
            // genuinely-unreachable body can't drive a re-attempt loop.
            if (!AnyTravelActive()) {
                const int ic = ++g_idleCycles[id];
                if (ic >= kIdleReassessCycles && !g_travelFailed.empty() &&
                    (g_lastBlocklistReassess.time_since_epoch().count() == 0 ||
                     now - g_lastBlocklistReassess >= kReassessCooldown)) {
                    // AGE-GATED wipe (RC#4 churn): the map is GLOBAL, so follower
                    // B's idle reassess used to erase a verdict follower A recorded
                    // 200 ms earlier (deck 12:25:09.988) -- A instantly re-picked
                    // the just-failed target and burned another whole trip on it.
                    // Only entries that have aged past kReassessMinAge are cleared;
                    // fresh verdicts ride out their own cooldown.
                    constexpr auto kReassessMinAge = std::chrono::seconds(10);
                    const size_t before = g_travelFailed.size();
                    std::erase_if(g_travelFailed, [&](const auto& kv) {
                        return now - kv.second >= kReassessMinAge;
                    });
                    const size_t n = before - g_travelFailed.size();
                    // NOTE: g_stallStrikes deliberately SURVIVES the reassess. Wiping
                    // it here defeated the 2-strike sticky entirely: a geometrically
                    // unreachable body (navmesh path ends short -- e.g. deck 0002CFBF
                    // navdist=148 < gate 300, dist frozen every walk) stalls once per
                    // excursion, but the reassess reset its strike to 0 before the 2nd
                    // could promote it, so it was re-picked forever and burned whole
                    // excursions. Strikes now accumulate across excursions; a body that
                    // proves reachable has its strike cleared on ARRIVAL (below), so a
                    // merely-transient block never falsely reaches sticky.
                    if (n > 0) {
                        // Commit only on a real wipe: an all-fresh map keeps the
                        // idle counter armed and retries once entries age, without
                        // burning the cooldown on a no-op.
                        g_lastBlocklistReassess = now;
                        g_idleCycles[id] = 0;
                        spdlog::info("[loot] {:08X} idle {} ticks -- cleared {} blocklisted "
                                     "bodies to reassess", id, ic, n);
                    }
                }
            }
            // Heartbeat so "serviced, nothing to do" is distinguishable from
            // "never ran" (#53) -- promoted from debug (never written at info
            // level) to a RATE-LIMITED info line, once per follower per ~30 s.
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextIdleLog;
            auto& nxt = s_nextIdleLog[id];
            if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                nxt = now + std::chrono::seconds(30);
                spdlog::info("[logistics] {:08X} serviced -- nothing to loot/drink right now", id);
            }
            // ECONOMY PROBE (#21) -- DISABLED again 2026-07-31 after the MAIN-THREAD
            // run STILL crashed (crash-2026-07-31-15-49-45, vendor Ma'dran). That
            // is the decisive result: the vendor CTD is NOT a thread race. The pump
            // is proven live ("[mainthread] first drain ..."), the probe ran ON the
            // main thread, and it still null-derefed inside chest->GetInventory()
            // on the merchant's chest (RSI=Chest, a form cast -> null -> deref). The
            // chest is a persistent merchant container that no barter menu has
            // populated, so its InventoryChanges is not set up and native
            // GetInventory faults on it -- on ANY thread. GetContainer() passes (it
            // IS a real container); the fault is deeper.
            //
            // So the real barter cannot read merchant stock via native
            // TESObjectREFR::GetInventory on an unpopulated merchant chest. It needs
            // a safe merchant-read (Papyrus, which is what the game's own barter menu
            // uses; or force-initialising the chest's InventoryChanges first). The
            // main-thread pump (§0.37) is still correct + needed for the TRANSACTION
            // (mutations must run on main), just not sufficient for the READ.
            // See [[economy-vendor-detection-excludes-teammates]] + ENGINE_NOTES.
            // #21 econ scan. GATED behind bEconomy (off by default) AND po3 presence
            // (Fable RC#3): with the toggle off, nothing runs -- no dispatch, no
            // Papyrus, no log -- so a user who hasn't opted in (or lacks po3) never
            // touches the merchant path. Runs on the WORKER, not MainThread::Post
            // (Fable #1/#4): the reads (follower GetInventory / CountPotions /
            // g_travel) must share the thread with the loot/heal/loadout mutations in
            // this same task or they race InventoryChanges (the Actor.cpp:445 CTD
            // class). The merchant read + transaction run in Papyrus (VM), so nothing
            // here needs main; dispatch is worker->VM, like DispatchActivate.
            // #21 AUTO-LEARN carried spell tomes -- a mage-build follower studies any
            // castable tome it carries (bought OR handed over by the player), on the
            // WORKER. Independent of the economy master + the buy toggle (learning a
            // tome you own is always desirable); gated to CASTERS so warriors don't
            // eat tomes. One tome per tick (see LearnCarriedTomes).
            if (HasCastGambit(a_state))
                LearnCarriedTomes(a_follower);
            // #21 College tome-gate unlock: generalize the vanilla player-skill gate to
            // the party (flip the tier global to 0 once the player OR any follower
            // reaches the skill). Rate-limited GLOBALLY inside; part of the follower
            // tome-access feature, so gated on the tome-buy toggle (no new setting).
            if (Config::g_economy.load() && Config::g_economyBuyTomes.load())
                UnlockCollegeTomes();
            // #21 PART 2A: wear the best owned upgrade the follower isn't wearing yet
            // (bought / looted-as-valuable jewelry / player-handed), via the shared
            // safe equip path. Idempotent + one-per-tick, so it converges and never
            // thrashes; dolls mode (handled inside) is the manual-dress escape hatch.
            EquipBestOwnedGear(a_follower, a_state);
            // GEM RECONCILE (MEO v3). Decoupled from the ungem-then-sell event: each
            // idle management scan, re-socket a follower's OWN loose gems into his worn
            // gear's empty sockets so a gem extracted for a sale never stays loose.
            // CONSERVATION always runs; the effect-aware tier is gated on bMeoAwareGems.
            // Posts to the main thread inside; a full no-op when MEO < v3 (the loose
            // gem simply stays as it does today -- no regression).
            if (MEOBridge::GemReconcileSupported()) {
                MEOBridge::GemReconcilePrefs prefs;
                prefs.caster = IsCasterFollower(a_state);
                if (prefs.caster) {
                    int castGambits = 0;
                    prefs.school = static_cast<std::uint32_t>(TargetMagicSchool(a_state, castGambits));
                }
                MEOBridge::RequestGemReconcile(a_follower, Config::g_meoAwareGems.load(), prefs);
            }
            if (Config::g_economy.load() && Po3Present())
                EconomyProbe(a_follower, a_state, now);
        }
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[logistics] no event source holder -- waiver sink NOT installed");
            return;
        }
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        holder->AddEventSink<RE::TESEquipEvent>(BeastHeadSink::GetSingleton());   // #62 beast-head reattach
        spdlog::info("[logistics] player-looted waiver + beast-head equip sinks installed");
    }

    // #62 ON-LOAD broken-gear clean. A follower can come up from a save already
    // wearing non-playable creature gear (the verified Inigo/draugr-helmet case) or
    // a foreign non-rendering head item -> invisible head. So on load, run
    // KeepHeadClear once per loaded teammate. Retries ~2s so a follower that finishes
    // loading a few frames late (or before Followers::Refresh populates g_active) is
    // still caught; a `done` set makes it fire once per follower. The follower's own
    // native gear is left untouched. Called from kPostLoadGame/kNewGame; main thread.
    // Gated on bBeastHeadFix.
    void SweepBeastHeadsOnLoad() {
        if (!Config::g_beastHeadFix.load()) return;
        auto done  = std::make_shared<std::unordered_set<RE::FormID>>();
        auto tries = std::make_shared<int>(120);   // ~2s window for late-loading followers
        auto self  = std::make_shared<std::function<void()>>();
        *self = [done, tries, self]() {
            // SEV-1: this runs on the MAIN thread (MainThread::Post), but g_active is
            // rebuilt by Followers::Refresh on the JOB WORKER, so a raw walk here races
            // that reassignment (#4). Read the lock-guarded FormID snapshot instead;
            // late-loading teammates not yet in it are caught by the ~2s retry below,
            // exactly as when g_active itself was still filling.
            auto snap = Followers::ActiveSnapshot();
            if (snap) for (const RE::FormID id : *snap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || !a->IsPlayerTeammate() || !a->Is3DLoaded()) continue;
                if (done->count(id)) continue;
                KeepHeadClear(a);
                done->insert(id);
            }
            if (--(*tries) > 0) MainThread::Post(*self);
            else                *self = nullptr;   // break the self-capture cycle (running fn is the pump's copy)
        };
        MainThread::Post(*self);
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
        g_lastCombatSeen.clear();   // post-battle shed dwell -- save-scoped, live-session only
        g_claim.clear();
        g_lastLootSource = 0;
        g_drinkUntil.clear();
        g_logiCastUntil.clear();   // OOC cast pacing window -- save-scoped (#6, FF-reuse)
        g_playerLooted.clear();
        g_econScan.clear();   // #21 econ cadence clocks -- save-scoped (Fable audit #7)
        g_econTrade.clear();
        g_econPair.clear();
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
