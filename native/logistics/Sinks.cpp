// logistics/Sinks.cpp -- the engine EVENT SINKS (broken-gear / beast-head equip,
// container change, the gated re-admit open/close sink), their registration
// and the on-load beast-head sweep.
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
                if (!a_ev || !a_ev->actor)
                    return RE::BSEventNotifyControl::kContinue;
                // ── [armor-obs] PASSIVE OBSERVER (2026-09-14, zero new hooks) ──
                // Every rated-ARMO equip AND unequip on a tracked follower, as the
                // engine reports it -- the ground truth of WHO is wearing WHAT, so
                // the next field log can tell an MFO wear decision ([equip]/[armor])
                // from the engine's own skill-blind auto-equip (the 2026-09-14
                // finding: MFO judged, the engine wore). Membership through the
                // locked FormID mirror (Followers::IsTrackedFast, #74) -- never
                // g_followers / g_active from a sink (#4). Pure reads, no post,
                // no rate limit: these are per-change lines. Runs BEFORE the
                // beast-head gates below (equipped-only, bBeastHeadFix), which
                // are that fix's, not this observer's.
                if (auto* obsActor = a_ev->actor->As<RE::Actor>();
                    obsActor && a_ev->baseObject && Followers::IsTrackedFast(obsActor->GetFormID())) {
                    if (auto* armo = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_ev->baseObject);
                        armo && armo->GetArmorRating() > 0.0f) {
                        using AT = RE::BGSBipedObjectForm::ArmorType;
                        const char* type = "Clothing";
                        switch (armo->GetArmorType()) {
                        case AT::kHeavyArmor: type = "Heavy"; break;
                        case AT::kLightArmor: type = "Light"; break;
                        default: break;
                        }
                        spdlog::info("[armor-obs] {:08X} '{}': {} '{}' ({:08X}) [{}] rat={:.0f}",
                                     obsActor->GetFormID(), obsActor->GetName() ? obsActor->GetName() : "?",
                                     a_ev->equipped ? "EQUIP" : "UNEQUIP",
                                     armo->GetFullName() ? armo->GetFullName() : "?", armo->GetFormID(),
                                     type, armo->GetArmorRating());
                    }
                }
                if (!a_ev->equipped)
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

    // ── GATED re-admit sink (loot M1). The gate records, the "behind the same
    // block" rule and GatedNow/MarkGated live in Logistics_internal.h (the scan
    // reads them through TravelFailedRecently).
    namespace {
        // Re-admit signals. Main thread (engine event dispatch); each one only
        // erases records under g_gateMx and is a no-op while nothing is gated.
        // Layouts: fork TESOpenCloseEvent.h / TESActivateEvent.h /
        // TESCellAttachDetachEvent.h; holder sources +0x8F0 (verified in both
        // binaries, SendOpenCloseEvent SE 14190 / AE 14299), +0x58, +0x1B8.
        // Review R3: a follower/teammate using a door is navmesh portal traffic,
        // not a gate opening -- counting it re-admits the portcullis he just
        // skipped and walks him back into it. The player and other NPCs count.
        bool ByFollower(RE::TESObjectREFR* a_by) {
            auto* a = a_by ? a_by->As<RE::Actor>() : nullptr;
            return a && (a->IsPlayerTeammate() || Followers::IsTrackedFast(a->GetFormID()));
        }
        class GateSink final : public RE::BSTEventSink<RE::TESOpenCloseEvent>,
                               public RE::BSTEventSink<RE::TESActivateEvent>,
                               public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
        public:
            static GateSink* GetSingleton() { static GateSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESOpenCloseEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESOpenCloseEvent>*) override {
                if (a_ev && g_gateCount.load(std::memory_order_relaxed) > 0 && !ByFollower(a_ev->activeRef.get()))
                    ReadmitNear(a_ev->ref.get(), a_ev->opened ? "door/gate OPENED near it"
                                                              : "door/gate CLOSED near it");
                return RE::BSEventNotifyControl::kContinue;
            }
            // A lever portcullis may animate without SetOpen (no open/close
            // event), but its LEVER always fires an activation. Activators and
            // doors only -- item pickups and containers are not gate signals.
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESActivateEvent>*) override {
                if (!a_ev || g_gateCount.load(std::memory_order_relaxed) == 0 || ByFollower(a_ev->actionRef.get()))
                    return RE::BSEventNotifyControl::kContinue;
                auto* r    = a_ev->objectActivated.get();
                auto* base = r ? r->GetBaseObject() : nullptr;
                if (base && (base->Is(RE::FormType::Activator) || base->Is(RE::FormType::Door)))
                    ReadmitNear(r, "lever/door ACTIVATED near it");
                return RE::BSEventNotifyControl::kContinue;
            }
            // Fired PER REFERENCE: the gated item's own cell attached again (the
            // player left and came back, the world may have changed).
            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
                if (!a_ev || !a_ev->attached || g_gateCount.load(std::memory_order_relaxed) == 0)
                    return RE::BSEventNotifyControl::kContinue;
                if (auto* r = a_ev->reference.get()) {
                    const RE::FormID rid = r->GetFormID();
                    std::lock_guard lk(g_gateMx);
                    EraseGatesIf("its cell attached again", rid,
                                 [rid](const GateRecord& g) { return g.target == rid; });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[logistics] no event source holder -- waiver sink NOT installed");
            return;
        }
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        holder->AddEventSink<RE::TESEquipEvent>(BeastHeadSink::GetSingleton());   // #62 beast-head reattach
        // loot M1: GATED re-admit signals (a door/gate/lever near a gate, a gated
        // item's cell attaching again). Each is a no-op while nothing is gated.
        holder->AddEventSink<RE::TESOpenCloseEvent>(GateSink::GetSingleton());
        holder->AddEventSink<RE::TESActivateEvent>(GateSink::GetSingleton());
        holder->AddEventSink<RE::TESCellAttachDetachEvent>(GateSink::GetSingleton());
        spdlog::info("[logistics] player-looted waiver + beast-head equip + loot gate sinks installed");
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

}
