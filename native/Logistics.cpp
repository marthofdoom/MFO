#include "PCH.h"
#include "Logistics.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Config.h"

// The logistics table. DESIGN.md §4.8. Out-of-combat upkeep: drink H/S/M
// potions, loot arrows / potions / better gear -- one action per idle tick,
// behind bLogistics, and NEVER interleaved with the combat table (§4.8).

namespace MFO::Logistics {

    namespace {

        // ── tuning that is NOT surfaced as config ───────────────────────────
        // The three MCM keys are fFirstDibsDelay / fQuickLootWaiver / bLogistics
        // (the M6 contract). These two are structural, not preferences, so they
        // stay compile-time.

        // §4.8: the idle tick is ~1 s (DESIGN §4.1's out-of-combat rate), NOT the
        // 133 ms combat deadline -- nothing here is reflex-timed, and an
        // inventory walk per tick is only affordable at this cadence.
        constexpr auto kLogisticsInterval = std::chrono::milliseconds(1000);

        // The "consideration radius" (§4.8.3). Tier A is "loot what is already in
        // reach" (§4.8.4): a follower stands next to the corpses anyway, so this
        // is deliberately close. Going and fetching is Tier B and needs
        // positioning, which is out of scope here.
        constexpr float kLootRadius = 600.0f;

        // The loot LRUs are bounded and deliberately NOT serialized (#22h):
        // worst case after a load is one more first-dibs wait on an
        // already-picked corpse, which is not worth a growing co-save record.
        constexpr size_t kLruCap = 256;

        using Clock = std::chrono::steady_clock;

        // ── transient, main-thread-only, cleared on revert ──────────────────
        // Same discipline as Loadout's g_debt: no lock, no cross-thread reader,
        // reconstructed from live state rather than persisted.

        // When each follower may next take a logistics action. The ~1 s cadence
        // gate; also the natural one-action-per-tick rate limit (§4.8.3).
        std::unordered_map<RE::FormID, Clock::time_point> g_nextTick;

        // First time a lootable ref was OBSERVED in a follower's radius. The
        // first-dibs clock counts from here (#22h). Bounded LRU.
        std::unordered_map<RE::FormID, Clock::time_point> g_seen;

        // Refs the PLAYER has taken from -- the waiver (#22h). Presence collapses
        // the delay to fQuickLootWaiver, and the timestamp RESETS on every take
        // so the follower moves in that many seconds after the player's LAST
        // take, not their first (the QuickLoot-IE case). Bounded LRU.
        std::unordered_map<RE::FormID, Clock::time_point> g_playerLooted;

        // Evict the oldest entry when a bounded map is over cap. n <= kLruCap and
        // inserts are rare, so the O(n) scan is cheaper than carrying a deque.
        void EvictOldest(std::unordered_map<RE::FormID, Clock::time_point>& a_map) {
            if (a_map.size() <= kLruCap) return;
            auto oldest = a_map.begin();
            for (auto it = a_map.begin(); it != a_map.end(); ++it) {
                if (it->second < oldest->second) oldest = it;
            }
            a_map.erase(oldest);
        }

        RE::FormID PlayerID() {
            auto* p = RE::PlayerCharacter::GetSingleton();
            return p ? p->GetFormID() : 0x14;   // 0x14 is the fixed player FormID
        }

        // ── loot eligibility (#22h) ─────────────────────────────────────────
        // A ref is eligible once it has sat in radius fFirstDibsDelay seconds --
        // UNLESS the player has taken from it, which collapses the wait to
        // fQuickLootWaiver (never to zero). Reads only our own timers.
        bool LootEligible(RE::FormID a_refID, Clock::time_point a_now) {
            if (const auto w = g_playerLooted.find(a_refID); w != g_playerLooted.end()) {
                return std::chrono::duration<float>(a_now - w->second).count() >=
                       Config::g_quickLootWaiver.load();
            }
            const auto s = g_seen.find(a_refID);
            if (s == g_seen.end()) return false;   // never seen -> never eligible
            return std::chrono::duration<float>(a_now - s->second).count() >=
                   Config::g_firstDibsDelay.load();
        }

        // ── the follower's equipped ranged weapon, for ammo matching ────────
        // Returns the equipped bow/crossbow, or nullptr. Reads the NAMED
        // follower only (#14).
        RE::TESObjectWEAP* EquippedRanged(RE::Actor* a_follower) {
            auto* right = a_follower ? a_follower->GetEquippedObject(false) : nullptr;
            auto* weap  = right ? right->As<RE::TESObjectWEAP>() : nullptr;
            if (weap && (weap->IsBow() || weap->IsCrossbow())) return weap;
            return nullptr;
        }

        // Does this ammo match this ranged weapon's class? A bow fires arrows
        // (!IsBolt); a crossbow fires bolts. IsBolt() is non-const, hence the
        // non-const ammo pointer.
        bool AmmoMatches(RE::TESObjectWEAP* a_weap, RE::TESAmmo* a_ammo) {
            if (!a_weap || !a_ammo) return false;
            const bool wantBolt = a_weap->IsCrossbow();
            return a_ammo->IsBolt() == wantBolt;
        }

        // ── carry-weight guard (§4.8.3) ─────────────────────────────────────
        // Never loot a follower into being overencumbered. Reads the named
        // follower's live weight and carry cap.
        bool FitsCarryWeight(RE::Actor* a_follower, float a_addWeight) {
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return false;
            const float cap  = avo->GetActorValue(RE::ActorValue::kCarryWeight);
            const float have = a_follower->GetWeightInContainer();
            return (have + a_addWeight) <= cap;
        }

        // ── the three loot actions ──────────────────────────────────────────
        // Each transfers from a source ref into the follower and returns true if
        // anything moved. COLLECT-THEN-ACT is the caller's job (#2): these are
        // called AFTER the ForEachReferenceInRange walk has finished, on a
        // handle re-resolved at act time, so a container mutation never happens
        // mid-iteration of the world's ref list.

        bool LootArrows(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
            auto* weap = EquippedRanged(a_follower);
            if (!weap) return false;   // no bow/crossbow -> nothing to match

            // Collect matching ammo tuples first (object, count), THEN transfer
            // -- RemoveItem dispatches TESContainerChangedEvent synchronously, so
            // mutating the source inventory mid-walk is the #2 landmine.
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* ammo = obj->As<RE::TESAmmo>();
                if (AmmoMatches(weap, ammo)) takes.push_back({ obj, data.first });
            }
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        bool LootPotions(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
            struct Take { RE::TESBoundObject* obj; std::int32_t count; };
            std::vector<Take> takes;
            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc) continue;
                // H/S/M restoratives ONLY (§4.8.2). PotionRestores returns kNone
                // for poisons, fortify, cure -- those are left for the player.
                const auto av = PotionRestores(alc);
                if (av == RE::ActorValue::kHealth || av == RE::ActorValue::kStamina ||
                    av == RE::ActorValue::kMagicka) {
                    takes.push_back({ obj, data.first });
                }
            }
            bool moved = false;
            for (const auto& t : takes) {
                if (!FitsCarryWeight(a_follower, t.obj->GetWeight() * t.count)) continue;
                a_src->RemoveItem(t.obj, t.count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                                  nullptr, a_follower);
                moved = true;
            }
            return moved;
        }

        // Is this armor a strict upgrade on at least one slot it covers, and
        // beaten on none? "Better" is the item's OWN rating vs what the follower
        // wears in the same slots -- the §4.8.2 derived-vocabulary principle, so
        // modded gear works with no patch. Reads the named follower.
        //
        // SCOPED (see the module report): "weighted by their armor skill" and
        // the light/heavy-skill steer are a refinement not implemented here --
        // this compares raw armor rating. It never upgrades a follower's whole
        // outfit in one tick, only the single better piece it transfers.
        bool ArmorIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo) {
            const float cand = a_armo->GetArmorRating();
            if (cand <= 0.0f) return false;   // clothing / jewellery -- not armor

            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            static constexpr Slot kSlots[] = {
                Slot::kHead, Slot::kBody, Slot::kHands, Slot::kForearms,
                Slot::kFeet, Slot::kCalves, Slot::kShield,
            };
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            bool overlapsAny = false;
            for (const auto slot : kSlots) {
                if (!(mask & static_cast<std::uint32_t>(slot))) continue;
                overlapsAny = true;
                auto* worn = a_follower->GetWornArmor(slot);
                if (worn && worn->GetArmorRating() >= cand) {
                    return false;   // beaten on a slot it would replace
                }
            }
            // Better only if it actually covers an armor slot (skip amulets/rings
            // whose bits are not in kSlots) and nothing it replaces is >= it.
            return overlapsAny;
        }

        bool LootEquipment(RE::Actor* a_follower, RE::TESObjectREFR* a_src) {
            // Generalized by CATEGORY, never by item (§4.8.2). One better piece
            // per tick (one action per tick, §4.3). Transfer only -- "equip what
            // was looted" is deferred: force-equipping re-enters the equip sinks
            // (INVARIANTS #3) and collides with Loadout's hand/debt ledger, so it
            // needs Loadout coordination and its own milestone. The follower's
            // own AI equips better owned gear in the meantime.
            RE::TESBoundObject* best   = nullptr;
            std::int32_t        bestCt = 0;

            auto* equippedWeap = a_follower->GetEquippedObject(false);
            auto* myWeap       = equippedWeap ? equippedWeap->As<RE::TESObjectWEAP>() : nullptr;
            const std::uint16_t myDmg = myWeap ? myWeap->GetAttackDamage() : 0;

            for (auto& [obj, data] : a_src->GetInventory()) {
                if (!obj || data.first <= 0) continue;

                if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                    if (ArmorIsBetter(a_follower, armo)) { best = obj; bestCt = 1; break; }
                } else if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    // Same category only, and only if the follower HAS a weapon
                    // to beat. With nothing equipped myDmg is 0 and every weapon
                    // would "upgrade" -- an arbitrary-weapon vacuum. A bow must
                    // not beat a sword (§4.8.2 compares within category).
                    if (myWeap && weap->GetWeaponType() == myWeap->GetWeaponType() &&
                        weap->GetAttackDamage() > myDmg) { best = obj; bestCt = 1; break; }
                }
            }
            if (!best) return false;
            if (!FitsCarryWeight(a_follower, best->GetWeight() * bestCt)) return false;

            a_src->RemoveItem(best, bestCt, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                              nullptr, a_follower);
            return true;
        }

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

            RE::AlchemyItem* best = nullptr;
            float bestMag = -1.0f;

            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc || PotionRestores(alc) != a_which) continue;

                // Magnitude of the costliest effect -- the same effect
                // PotionRestores classified on, so "best" is by the resource the
                // potion is actually FOR.
                const auto* eff = alc->GetCostliestEffectItem();
                const float mag = eff ? eff->GetMagnitude() : 0.0f;
                if (mag > bestMag) { bestMag = mag; best = alc; }
            }
            if (!best) return false;

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

        // ── the looting dispatcher ──────────────────────────────────────────
        enum class Category { Arrows, Potions, Equipment };

        // Walk nearby refs, gate them, and perform ONE transfer. Returns true if
        // something was looted. Collect-then-act (#2): the world walk only reads
        // and records timers; all mutation happens afterwards on re-resolved
        // handles.
        bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now) {
            // §22g ABSOLUTE BAR, ahead of every delay and waiver: never mutate a
            // container while the player has ANY container menu open -- it breaks
            // the vanilla menu building its list from that container (MEO m19e).
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return false;
            }
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return false;

            // Eligible loot sources, collected inside the walk and acted on after
            // it. Bounded so a room full of corpses cannot make the tick unbounded.
            std::vector<RE::ObjectRefHandle> candidates;
            candidates.reserve(16);

            tes->ForEachReferenceInRange(a_follower, kLootRadius,
                [&](RE::TESObjectREFR& a_ref) {
                    if (candidates.size() >= 16) return RE::BSContainer::ForEachResult::kStop;
                    RE::TESObjectREFR* ref = &a_ref;
                    if (ref == a_follower) return RE::BSContainer::ForEachResult::kContinue;
                    if (ref->IsDisabled() || ref->IsMarkedForDeletion())
                        return RE::BSContainer::ForEachResult::kContinue;

                    // A lootable source is a CORPSE (dead actor) or a CONTAINER.
                    // Living actors are never looted -- that is pickpocketing.
                    bool lootable = false;
                    if (auto* actor = ref->As<RE::Actor>()) {
                        lootable = actor->IsDead();
                    } else if (auto* base = ref->GetBaseObject()) {
                        lootable = base->Is(RE::FormType::Container);
                    }
                    if (!lootable) return RE::BSContainer::ForEachResult::kContinue;

                    // OWNERSHIP IS ABSOLUTE (#22e). IsOffLimits() is ONLY a crime
                    // check (IsCrimeToActivate) -- and taking from a PLAYER-owned
                    // chest is not a crime, so it would sail through and let a
                    // follower drain the player's own storage. Check GetOwner()
                    // first (any explicit owner, including the player), then keep
                    // IsOffLimits() as the second bar for cell-owned/no-owner
                    // crime cases. No delay or waiver overrides this.
                    if (ref->GetOwner()) return RE::BSContainer::ForEachResult::kContinue;
                    if (ref->IsOffLimits()) return RE::BSContainer::ForEachResult::kContinue;
                    // Locked is locked -- RemoveItem ignores the lock, so the
                    // filter must not.
                    if (ref->IsLocked()) return RE::BSContainer::ForEachResult::kContinue;

                    // Start (or keep) this ref's first-dibs clock. Recording here,
                    // for every candidate whether eligible yet or not, is what
                    // makes the delay measure PRESENCE in radius (#22h).
                    const auto id = ref->GetFormID();
                    if (g_seen.emplace(id, a_now).second) EvictOldest(g_seen);

                    if (LootEligible(id, a_now)) {
                        candidates.push_back(ref->GetHandle());
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            // Act after the walk. Re-resolve each handle at act time (#2); stop at
            // the first successful transfer -- one loot action per tick (§4.8.3).
            for (auto& h : candidates) {
                auto ptr = h.get();
                auto* ref = ptr.get();
                if (!ref) continue;
                bool moved = false;
                switch (a_cat) {
                case Category::Arrows:    moved = LootArrows(a_follower, ref);    break;
                case Category::Potions:   moved = LootPotions(a_follower, ref);   break;
                case Category::Equipment: moved = LootEquipment(a_follower, ref); break;
                }
                if (moved) return true;
            }
            return false;
        }

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

                // Sinks QUEUE; they never touch a main-thread map inline (#1/#4).
                // The timer RESETS on every take, so multiple QuickLoot takes push
                // the follower's window out to the LAST one.
                SKSE::GetTaskInterface()->AddTask([srcID]() {
                    g_playerLooted[srcID] = Clock::now();
                    EvictOldest(g_playerLooted);
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

    int ArrowCount(RE::Actor* a_follower) {
        if (!a_follower) return -1;
        auto* weap = EquippedRanged(a_follower);
        if (!weap) return -1;   // N/A: not a ranged follower

        int n = 0;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            if (AmmoMatches(weap, obj->As<RE::TESAmmo>())) n += data.first;
        }
        return n;
    }

    // ── public: actuation ───────────────────────────────────────────────────

    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!Config::g_logistics.load()) return;   // whole subsystem off by default (#45)
        if (!a_follower) return;

        const auto id  = a_follower->GetFormID();
        const auto now = Clock::now();

        // CADENCE GATE (~1 s). Cheap early-out on the frames between logistics
        // ticks -- the Scheduler calls this every time it services the follower
        // out of combat (up to ~7.5 Hz), but logistics only acts at the idle rate.
        auto& due = g_nextTick[id];
        if (due.time_since_epoch().count() != 0 && now < due) return;
        due = now + kLogisticsInterval;

        if (a_state.logistics().empty()) return;   // no rules -> nothing to run

        // First-match-wins over the LOGISTICS table, same scan as combat (§4.3).
        const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics);
        if (choice.ruleIndex < 0) return;           // §4.4: no match -> no engine call

        const auto& op = choice.actionOpcode;
        bool  acted = false;
        const char* label = op.c_str();

        if      (op == Vocab::kActDrinkHealthPotion)  acted = DrinkBest(a_follower, RE::ActorValue::kHealth);
        else if (op == Vocab::kActDrinkStaminaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kStamina);
        else if (op == Vocab::kActDrinkMagickaPotion) acted = DrinkBest(a_follower, RE::ActorValue::kMagicka);
        else if (op == Vocab::kActLootArrows)         acted = LootNearby(a_follower, Category::Arrows, now);
        else if (op == Vocab::kActLootPotions)        acted = LootNearby(a_follower, Category::Potions, now);
        else if (op == Vocab::kActLootEquipment)      acted = LootNearby(a_follower, Category::Equipment, now);
        else if (op == Vocab::kActWait)               return;   // consume the tick, no log
        else {
            // Unknown / non-logistics opcode in the logistics table: fail closed
            // and say so, never fall through to something else (mirrors Actuation).
            spdlog::info("[logistics] {:08X} rule {} has non-logistics action '{}' -- ignored",
                         id, choice.ruleIndex, op);
            return;
        }

        if (acted) {
            spdlog::info("[logistics] {:08X} rule {} fired: {}", id, choice.ruleIndex, label);
        } else {
            // LOG THE ZERO CASE (#46), but only at debug so the ~1 s idle tick of
            // a follower with nothing to loot/drink does not flood. "ran and
            // found nothing" must be distinguishable from "never ran" (#53); the
            // ServiceFollower call itself is the heartbeat.
            spdlog::debug("[logistics] {:08X} rule {} ({}) found nothing to do",
                          id, choice.ruleIndex, label);
        }
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[logistics] no event source holder -- waiver sink NOT installed");
            return;
        }
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        spdlog::info("[logistics] player-looted waiver sink installed");
    }

    void ClearTransientState() {
        g_nextTick.clear();
        g_seen.clear();
        g_drinkUntil.clear();
        g_playerLooted.clear();
    }

}
