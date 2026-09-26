// logistics/LootScan.cpp -- the loot SCAN: LootNearby (candidate search, claims,
// travel legs, move-on), StripCorpse and the excursion scan.
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // ROAD 2 (A/B): ch.19 kIntent_Travel loot travel
#include "TeleportCompat.h"    // follower-teleport mod leash clamp (86e3ec824)

namespace MFO::Logistics {

        // ROAD 2's ARRIVAL RADIUS (bLootTravelViaApmfTravel), game units. NOT a new
        // number: it is EXACTLY what the ch.9 control road writes into
        // MFO_APMFLootTravelPackage<slot>'s stop radius (Packages.cpp kAPMFTravelRadius),
        // so he parks in the same place on both roads and the A/B compares the delivery
        // mechanism only. It is INSIDE ch.19's [50,512] clamp (a clamp would break that
        // parity silently) and BELOW MFO's kArrivalDist
        // of 200, which a corpse's grown grab radius only widens -- so MFO declares
        // ARRIVED and transfers while he is still closing. Above 200 he would park
        // beyond MFO's reach, ch.19 would end the leg there, and no pickup would run.
        constexpr float kCh19LootArrivalRadius = 128.0f;

        bool LootNearby(RE::Actor* a_follower, Category a_cat, Clock::time_point a_now,
                        RE::ActorValue a_potionWant,
                        LootMode a_mode) {
            // §22g ABSOLUTE BAR, ahead of every delay and waiver: never mutate a
            // container while the player has ANY container menu open -- it breaks
            // the vanilla menu building its list from that container (MEO m19e).
            if (auto* ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return false;
            }
            // PLAYER-COMBAT INTERRUPT (marth: "player entering combat must
            // immediately cancel a loot excursion and pull followers back").
            // A follower must never BEGIN or CONTINUE a loot action while the
            // player is fighting -- looting is an out-of-combat errand and it
            // does not get to keep running the tick the player needs him
            // back for. Deliberately the PLAYER's IsInCombat(), never this
            // follower's own: a follower can sit a full room away from the
            // fight for several ticks before his OWN combat state flips,
            // which is exactly the "wanders off looting mid-fight" gap this
            // closes. The engine-side claim/alias itself is released by the
            // fast global backstop in ServiceFollower (Logistics.cpp,
            // ~133 ms-scale -- runs on every out-of-combat service call for
            // ANY follower, not gated on this follower's own 1 s logistics
            // cadence); returning false HERE, at every loot call site (the
            // per-tick dispatch loop and RunExcursionScan both route through
            // this one function), stops the loot judge from re-arming or
            // continuing a leg in the meantime -- so the cancel and the loot
            // system's own re-evaluation never fight each other on the next
            // tick. No blacklist, no state left behind: when combat ends the
            // very next service tick re-evaluates from a clean slate and can
            // resume the SAME corpse immediately.
            if (auto* pcCombat = RE::PlayerCharacter::GetSingleton(); pcCombat && pcCombat->IsInCombat())
                return false;
            // BEHAVIOUR LAYER: hold looting while the player is ACTIVELY stealthing,
            // not merely crouched. A stealth build spends most of the dungeon in
            // sneak; a blanket block there means logistics looting effectively never
            // happens (Fable). Hold only when sneaking coincides with a drawn weapon
            // or combat -- the moments a follower breaking off would actually blow it.
            if (PlayerActivelyStealthing()) {
                static Clock::time_point s_nextSneakLog{};
                if (a_now >= s_nextSneakLog) {
                    s_nextSneakLog = a_now + std::chrono::seconds(10);
                    spdlog::info("[loot] {:08X} holding -- player stealthing (sneak + armed/combat)",
                                 a_follower->GetFormID());
                }
                return false;
            }
            // PLAYER HOME: don't loot your own house unless opted in (default OFF).
            if (!Config::g_lootInPlayerHomes.load() && InPlayerHome())
                return false;
            // WALK ACTOR-ANCHORED CELLS, NOT TES::ForEachReferenceInRange.
            // crash4 (2026-07-22, exterior Wilderness): TES::ForEachReferenceInRange
            // ends its exterior branch with `worldSpace->GetSkyCell()`, and
            // TES::worldSpace was a TORN pointer (0x450FE000_45242000 -- two
            // mismatched 32-bit halves) because the engine was mid worldspace/cell
            // stream. It chases three engine-owned pointers (gridCells, worldSpace,
            // skycell) that churn during a transition. A ref's parent cell, when
            // ATTACHED, iterates only its own reference list (no worldspace deref
            // at all -- see TESObjectCELL::ForEachReferenceInRange). The IsAttached
            // gate also skips the walk outright during a transition, which is
            // exactly when those pointers are unstable.
            //
            // RC#4 (marth: "skipped a second gold pile on the SAME table"): a
            // single-cell walk goes BLIND across exterior cell borders -- the deck
            // scan's ref count swung 1394 -> 132 -> 84 as the follower crossed
            // boundaries, and the table's other pile/potions sat unseen in the
            // neighbour cell. So scan up to three ACTOR-ANCHORED attached cells --
            // the follower's, the player's, and the live travel TARGET's -- each
            // reached through a ref we already hold (same safety class as before;
            // still zero TES/worldspace derefs). The target's cell is the one that
            // makes an excursion loot a table OUT: once he walks at pile 1, pile 2
            // beside it becomes visible even from another cell.
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached()) return false;
            const auto origin = a_follower->GetPosition();
            const float kLootRadius = Config::g_lootRadius.load();   // tunable, one snapshot per walk

            // THE CONFIDENCE LEASH (core tenet, DESIGN behaviour layer). A
            // candidate must be within this follower's confidence-scaled distance
            // FROM THE PLAYER -- bold when safe (leash -> max, ranges out),
            // cautious when hurt/fighting (leash -> min, stays close). Measured to
            // the PLAYER, while the scan radius above is measured to the FOLLOWER.
            // TELEPORT-MOD CLAMP (86e3ec824): while a detected follower-teleport
            // mod's condition holds (AFT: the player's weapon drawn) the leash is
            // capped under its teleport distance, so no trip is ever planned past
            // where that mod yanks him back. Unclamped it IS LeashRadius.
            const TeleportCompat::LeashView leashView = TeleportCompat::View(a_follower);
            const float leash = leashView.leash;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const RE::NiPoint3 playerPos = pc ? pc->GetPosition() : origin;

            // HOW FAR HE WILL WALK to a body (follower-relative). marth's tenet:
            // the confidence LEASH is his range, not a fixed room. So the walk
            // limit IS the leash -- bold when safe (ranges across rooms), cautious
            // when hurt. fTravelRadius is only a hard ceiling now (default raised
            // high), for anyone who wants to clamp it below the leash. Previously
            // the fixed 768 hid most in-leash bodies (deck: 6 eligible, only the 2
            // inside 768 ever attempted).
            const float walkLimit = std::min(leash, Config::g_travelRadius.load());

            // Eligible loot sources, collected inside the walk and acted on after
            // it. Bounded so a room full of corpses cannot make the tick unbounded
            // -- but the cap is applied in ARBITRARY cell-list order, BEFORE the
            // closest-first sort, so keep it generous: a low cap could drop a NEAR
            // source in favour of far ones iterated earlier ("ignores near, walks
            // far", audit). The HasLoot gate already trims to the few bodies that
            // hold the wanted category, so hitting even this is uncommon.
            constexpr size_t kMaxCandidates = 48;
            std::vector<RE::ObjectRefHandle> candidates;
            candidates.reserve(kMaxCandidates);

            // DIAGNOSTIC counters (marth: "nothing looted" -- find WHICH stage
            // drops the corpse). Logged rate-limited below.
            int dRefs = 0, dLootable = 0, dOwned = 0, dOffLimits = 0, dLocked = 0, dNotYet = 0, dLeash = 0, dEmpty = 0;

            // The scanned cell set (see the crash4/RC#4 note above): follower's
            // cell always; player's and live travel-target's when attached and
            // distinct. All anchored to refs in hand -- never TES globals.
            RE::TESObjectCELL* cells[3] = { cell, nullptr, nullptr };
            int nCells = 1;
            auto addCell = [&](RE::TESObjectREFR* a_anchor) {
                if (!a_anchor) return;
                auto* c = a_anchor->GetParentCell();
                if (!c || !c->IsAttached()) return;
                for (int i = 0; i < nCells; ++i)
                    if (cells[i] == c) return;   // dedupe (interiors: all one cell)
                if (nCells < 3) cells[nCells++] = c;
            };
            addCell(pc);
            if (auto* sl = SlotOf(a_follower->GetFormID())) {
                auto tp = sl->target.get();
                addCell(tp.get());
            }

            // Lazily built ONCE per LootNearby(Category::Equipment) call (the
            // Equipment loose-item test needs it; every other category
            // ignores it). Built from the follower's own gear, so it does not
            // change ref-to-ref within this one scan -- see BuildEquipmentContext.
            bool             equipCtxBuilt = false;
            EquipmentContext equipCtx;

            auto scanOne =
                [&](RE::TESObjectREFR& a_ref) {
                    if (candidates.size() >= kMaxCandidates) return RE::BSContainer::ForEachResult::kStop;
                    ++dRefs;
                    RE::TESObjectREFR* ref = &a_ref;
                    if (ref == a_follower) return RE::BSContainer::ForEachResult::kContinue;
                    if (ref->IsDisabled() || ref->IsMarkedForDeletion())
                        return RE::BSContainer::ForEachResult::kContinue;

                    // A lootable ref is a CORPSE (dead actor) or a CONTAINER --
                    // things we TRANSFER an inventory out of. Living actors are
                    // never touched (that is pickpocketing).
                    //
                    // LOOSE world items are excluded from the plain lootable
                    // test above for the ACQUIRE MECHANISM's sake, never by
                    // item type. Picking a loose ref up natively is
                    // PickUpObject, which tears down the ref's 3D and mutates
                    // the cell -- and this whole tick runs on a BSJobs JOB
                    // WORKER (§0.30), overlapping the streaming threads, the
                    // crash4 class. Re-queuing via SKSE AddTask does NOT
                    // escape it: the task queue itself is drained inside
                    // Job_Post_process on a worker (§0.30, INVARIANTS #72), so
                    // there is no main-thread hop to be had from here. Route
                    // 2b sidesteps the mechanism problem entirely -- the
                    // ENGINE does the pickup via ObjectReference.ActivateRef,
                    // marshalled to the MAIN thread (Logistics.cpp's excursion
                    // arrival), which works identically for ANY object type.
                    // So the whitelist below is a PER-CATEGORY ELIGIBILITY
                    // test, generalized (marth: "act.loot_soul_gems should
                    // pick up a loose soul gem" etc, same rule as a
                    // container, one decision path, two sources): every
                    // branch calls the EXACT predicate the matching
                    // HasLoot/Loot* container function already runs on a
                    // single item, so a loose item and a contained one are
                    // judged identically. NEVER-LOOT quest/catalog gating
                    // (LooseSpecialItemBlocked, just above LootNearby) applies
                    // wherever the container version applies it; categories
                    // whose container form has no such gate (Arrows/Bolts/
                    // Potions/Lockpicks/Gold) get none here either.
                    bool lootable = false;
                    bool loose    = false;   // route 2b: a loose WORLD item, not an inventory
                    if (auto* actor = ref->As<RE::Actor>()) {
                        lootable = actor->IsDead();
                    } else if (auto* base = ref->GetBaseObject()) {
                        lootable = base->Is(RE::FormType::Container);
                        // ACQUIRE PROBE (route 2b) WHITELIST: a loose world
                        // ref that HasLoot's matching category would have
                        // taken out of a container is a candidate too. It
                        // rides the SAME excursion machinery (walk to it;
                        // every gate below still applies) and the acquire
                        // happens at ARRIVAL via a native ActivateRef in the
                        // excursion driver -- NEVER an in-place PickUpObject
                        // (the worker trap the comment above describes).
                        if (!lootable) {
                            constexpr RE::FormID kGold001Ref  = 0x0000000F;   // see LootGold
                            constexpr RE::FormID kLockpickRef = 0x0000000A;   // see LootLockpicks
                            switch (a_cat) {
                            case Category::Arrows:
                            case Category::Bolts:
                                if (auto* ammo = base->As<RE::TESAmmo>();
                                    ammo && AmmoIsBolt(ammo) == (a_cat == Category::Bolts))
                                    lootable = loose = true;
                                break;
                            case Category::Gold:
                                // Gold001 OR an OCF coin/purse -- same test
                                // LootGold runs per inventory item.
                                if (base->GetFormID() == kGold001Ref || IsCoinLoot(base))
                                    lootable = loose = true;
                                break;
                            case Category::Valuables:
                                // Gold folds into Valuables (marth 2026-09-05)
                                // plus any value-dense loose MISC (the
                                // LootValuables/IsValuableMisc rule) -- a
                                // loose ref qualifies iff a container holding
                                // it would have been looted. The MISC branch
                                // is quest/catalog-gated like its container
                                // form (LootValuables); gold is not, matching
                                // LootGold.
                                if (base->GetFormID() == kGold001Ref || IsCoinLoot(base)) {
                                    lootable = loose = true;
                                } else if (IsValuableMisc(base) &&
                                           !LooseSpecialItemBlocked(ref, base->GetFormID())) {
                                    lootable = loose = true;
                                }
                                break;
                            case Category::Potions:
                                // Loose potion on a surface (marth field: Stenvar
                                // walked past a health potion on a table). The
                                // ownership gate below (ref->GetOwner) still
                                // skips inn/shop/home stock. Match LootPotions'
                                // item test (want + low-power floor) so a loose
                                // potion qualifies iff the loot would take it.
                                if (auto* alc = base->As<RE::AlchemyItem>()) {
                                    const bool wantOk =
                                        a_potionWant == RE::ActorValue::kNone
                                            ? IsDrinkablePotion(alc)
                                            : (PotionRestores(alc) == a_potionWant);
                                    const float fl  = PotionLootFloor();
                                    const float mag = PotionMagnitude(alc);
                                    if (wantOk && !(fl > 0.0f && mag > 0.0f && mag < fl))
                                        lootable = loose = true;
                                }
                                break;
                            case Category::Lockpicks:
                                // FormID match, same as LootLockpicks -- Free
                                // tier, weightless, nobody competes for it.
                                if (base->GetFormID() == kLockpickRef)
                                    lootable = loose = true;
                                break;
                            case Category::Jewelry:
                                // Same predicate LootJewelry runs per item
                                // (IsJewelryPiece), plus its quest/catalog gate.
                                if (auto* armo = base->As<RE::TESObjectARMO>();
                                    armo && IsJewelryPiece(armo) &&
                                    !LooseSpecialItemBlocked(ref, armo->GetFormID()))
                                    lootable = loose = true;
                                break;
                            case Category::SoulGems:
                                // Same predicate LootSoulGems runs per item
                                // (IsSoulGemItem), plus its quest/catalog gate.
                                if (IsSoulGemItem(base) &&
                                    !LooseSpecialItemBlocked(ref, base->GetFormID()))
                                    lootable = loose = true;
                                break;
                            case Category::Ingredients:
                                // Same predicate LootIngredients runs per item
                                // (IsIngredientItem), plus its quest/catalog gate.
                                if (IsIngredientItem(base) &&
                                    !LooseSpecialItemBlocked(ref, base->GetFormID()))
                                    lootable = loose = true;
                                break;
                            case Category::Equipment:
                                // GENERALIZED (marth): a bare weapon/armor
                                // lying on the ground is judged by the EXACT
                                // rule LootEquipment applies inside a
                                // container scan -- LooseEquipmentQualifies
                                // shares LootEquipment's context builder and
                                // per-item predicate (both just above
                                // LootHere), so the two never drift apart.
                                // IsCreatureWeapon/IsCreatureArmor (checked
                                // inside LooseEquipmentQualifies) preserve the
                                // exact protection that stopped a follower
                                // looting a creature's own weapon.
                                if ((base->As<RE::TESObjectWEAP>() || base->As<RE::TESObjectARMO>()) &&
                                    !LooseSpecialItemBlocked(ref, base->GetFormID())) {
                                    if (!equipCtxBuilt) {
                                        equipCtx      = BuildEquipmentContext(a_follower);
                                        equipCtxBuilt = true;
                                    }
                                    if (LooseEquipmentQualifies(a_follower, base, equipCtx))
                                        lootable = loose = true;
                                }
                                break;
                            default: break;
                            }
                        }
                    }
                    if (!lootable) return RE::BSContainer::ForEachResult::kContinue;
                    ++dLootable;

                    // #66 (the reported bug): LOTD drop-off / income / sell boxes
                    // in TOWNS and INNS -- ALWAYS skipped, regardless of the
                    // player-home toggle (they sit in ordinary public cells, so
                    // they'd otherwise be fair game). Matched by container base
                    // (IsLOTDDropOff); inert if LOTD is not installed.
                    if (!loose && IsLOTDDropOff(ref)) {
                        ++dOffLimits;
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // #66 (home storage): also skip containers in the player's OWN
                    // space -- LOTD museum halls + every vanilla/Hearthfire/mod
                    // player home (RefInPlayerStorage: LocTypePlayerHouse location
                    // or player/PlayerFaction-owned cell). CONTAINER-only: a corpse
                    // in a home is still fair loot, and the ref-level GetOwner() bar
                    // below never fires on these (ref-unowned). This half IS gated
                    // by bLootInPlayerHomes, so a player who wants followers tidying
                    // their own chests still can (the town/inn boxes above are not).
                    if (!loose && !Config::g_lootInPlayerHomes.load()) {
                        auto* cbase = ref->GetBaseObject();
                        if (cbase && cbase->Is(RE::FormType::Container) && RefInPlayerStorage(ref)) {
                            ++dOffLimits;
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                    }

                    // OWNERSHIP IS ABSOLUTE (#22e). IsOffLimits() is ONLY a crime
                    // check (IsCrimeToActivate) -- and taking from a PLAYER-owned
                    // chest is not a crime, so it would sail through and let a
                    // follower drain the player's own storage. Check GetOwner()
                    // first (any explicit owner, including the player), then keep
                    // IsOffLimits() as the second bar for cell-owned/no-owner
                    // crime cases. No delay or waiver overrides this.
                    if (auto* owner = ref->GetOwner()) {
                        ++dOwned;
                        // REGRESSION GUARD [ownprobe]: the soak proved every owned drop
                        // was a shop/faction container -- but a KILLED corpse must never
                        // read as owned (that would silently eat combat loot). Log ONLY
                        // that anomaly (dead actor), rate-limited -- silent normally.
                        if (auto* act = ref->As<RE::Actor>(); act && act->IsDead()) {
                            static std::unordered_map<RE::FormID, Clock::time_point> s_nextOP;
                            auto& op = s_nextOP[ref->GetFormID()];
                            if (op.time_since_epoch().count() == 0 || a_now >= op) {
                                op = a_now + std::chrono::seconds(20);
                                spdlog::info("[ownprobe] {:08X} CORPSE {:08X} dropped as OWNED (owner {:08X}) "
                                             "-- ownership gate may be eating combat loot",
                                             a_follower->GetFormID(), ref->GetFormID(), owner->GetFormID());
                            }
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    if (ref->IsOffLimits()) { ++dOffLimits; return RE::BSContainer::ForEachResult::kContinue; }
                    // Locked -- UNLESS the lockpick judge admits it (LP-M1,
                    // logistics/Lockpick.cpp): the excursion then picks it on
                    // arrival before any transfer. Owned locks are already barred above.
                    if (ref->IsLocked() && !Lockpick::Admit(a_follower, ref, a_now)) {
                        ++dLocked; return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // Beyond the confidence leash from the player -- too far for
                    // this follower's nerve right now. This is the invisible
                    // string: the same corpse is in-reach when they feel safe and
                    // out-of-reach when they do not.
                    //
                    // EXCEPT what is already at the follower's OWN feet (#30): the
                    // leash bounds how far he TRAVELS from you, not what he grabs
                    // where he already stands. A follower out looting a corpse that
                    // WAS in leash would otherwise ignore a potion beside it that
                    // sits a hair past the player-bubble -- the "found the potion
                    // right in front of him only after several runs" report. If
                    // he's within arrival distance of the ref, no excursion is
                    // needed, so the leash does not apply.
                    if (playerPos.GetDistance(ref->GetPosition()) > leash &&
                        a_follower->GetPosition().GetDistance(ref->GetPosition()) > kArrivalDist) {
                        ++dLeash; return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // MUST HOLD WHAT WE WANT. A read-only peek: never make him walk
                    // to a body that hasn't got the category this gambit is looting
                    // (marth). This is the biggest cut -- a fight leaves many
                    // corpses, few with the one thing (e.g. arrows) he's after --
                    // and it kills the "walk to an empty barrel and stall" trips.
                    // A LOOSE ref (route 2b) IS the loot -- it has no inventory
                    // for HasLoot to peek, so the gate does not apply.
                    if (!loose && !HasLoot(a_follower, ref, a_cat, a_potionWant)) {
                        // ARROW PROBE (temp, v0.8.17). marth: "multiple bodies with
                        // arrows available" yet the arrow scan calls every corpse
                        // empty. When an ARROW scan judges a lootable body empty,
                        // dump the ammo it ACTUALLY holds -- if it shows "Iron Arrow
                        // xN isBolt=0", HasLoot(arrows) is wrong on a body that has
                        // them (vs the body genuinely lacking arrows / being an
                        // adjacent-cell corpse the scan never sees). Per-body 15s.
                        if (a_cat == Category::Arrows) {
                            static std::unordered_map<RE::FormID, Clock::time_point> s_nextAP;
                            auto& an = s_nextAP[ref->GetFormID()];
                            if (an.time_since_epoch().count() == 0 || a_now >= an) {
                                an = a_now + std::chrono::seconds(15);
                                std::string ad; int tot = 0;
                                for (auto& [obj, data] : ref->GetInventory()) {
                                    ++tot;
                                    if (auto* am = obj ? obj->As<RE::TESAmmo>() : nullptr)
                                        ad += std::format(" [{} x{} isBolt={}]",
                                            am->GetFullName() ? am->GetFullName() : "?",
                                            data.first, am->IsBolt() ? 1 : 0);
                                }
                                if (!ad.empty())
                                    spdlog::info("[arrowprobe] {:08X} EMPTY-verdict body {:08X} "
                                                 "invItems={} but HAS ammo:{}",
                                                 a_follower->GetFormID(), ref->GetFormID(), tot, ad);
                            }
                        }
                        ++dEmpty; return RE::BSContainer::ForEachResult::kContinue;
                    }

                    // CLAIM-AND-RELEASE (dibs redesign) -- factored into TierReleased
                    // so the arm's-reach StripCorpse honours the SAME per-tier grace
                    // (#2). Eligibility is per value-tier and releases on EVIDENCE
                    // ABOUT THE PLAYER (near/facing/departed/abandon), not a wall
                    // clock; Free tiers (ammo/potions/lockpicks) release at once. The
                    // accrual it runs is idempotent-per-tick (a_now dedupe), so this
                    // scan drives it exactly as the inline version did.
                    if (TierReleased(a_cat, ref, playerPos, a_now))
                        candidates.push_back(ref->GetHandle());
                    else ++dNotYet;
                    return RE::BSContainer::ForEachResult::kContinue;
                };
            for (int ci = 0; ci < nCells; ++ci) {
                cells[ci]->ForEachReferenceInRange(origin, kLootRadius, scanOne);
                if (candidates.size() >= kMaxCandidates) break;   // cap hit mid-set
            }

            // One diagnostic line per follower PER CATEGORY per ~10 s, so the walk
            // composition is visible without flooding the ~1 s tick. Keyed by
            // (follower, category) -- a shared key would hide every category but
            // the first scanned in the window, which is exactly what made "he
            // never loots arrows" un-diagnosable (the arrow scan's empty=N never
            // reached the log). cat= names which scan this line is.
            {
                static std::unordered_map<std::uint64_t, Clock::time_point> s_nextWalkLog;
                const auto key = (static_cast<std::uint64_t>(a_follower->GetFormID()) << 8)
                               | static_cast<std::uint64_t>(a_cat);
                auto& nxt = s_nextWalkLog[key];
                if (nxt.time_since_epoch().count() == 0 || a_now >= nxt) {
                    nxt = a_now + std::chrono::seconds(10);
                    // Include the follower's OWN state so a "3 eligible but
                    // nothing looted" is unambiguous -- an eligible corpse yields
                    // nothing if it simply does not hold the thing the winning
                    // rule wants (arrows, or the specific potion).
                    spdlog::info("[loot] {:08X} cat={} r={:.0f} leash={:.0f}: {} refs, {} lootable, {} eligible "
                                 "(dropped owned={} locked={} waiting={} farleash={} empty={}) | self arrows={} bolts={} "
                                 "potH={} potS={} potM={}",
                                 a_follower->GetFormID(), CatName(a_cat), kLootRadius, leash, dRefs, dLootable,
                                 (int)candidates.size(), dOwned, dLocked, dNotYet, dLeash, dEmpty,
                                 ArrowCount(a_follower), BoltCount(a_follower),
                                 CountPotions(a_follower, RE::ActorValue::kHealth),
                                 CountPotions(a_follower, RE::ActorValue::kStamina),
                                 CountPotions(a_follower, RE::ActorValue::kMagicka));
                }
            }

            // REACHABLE FIRST, THEN CLOSEST (unified loot-failure model, marth):
            // a target that recently FAILED to path is DEPRIORITIZED, not
            // removed -- it sorts to the BACK (body or loose item alike), and
            // within each group closest-first, so the follower clears every
            // easy target before circling back to a path-troubled one (by which
            // time its grown grab radius usually lets it be taken from range).
            // An unresolvable handle sorts last. (Bounded at kMaxCandidates=48.)
            // Keys snapshotted ONCE, plus the loot-M1 actor-block reorder tiers:
            // SortLootCandidates, Logistics_internal.h (review R1: a mid-sort gate
            // erase can no longer flip a key between comparisons).
            SortLootCandidates(candidates, a_follower, origin, a_now);

            // CHURN GUARD (#48): never ARM a new excursion the release gate
            // (the excursion driver's x1.15 "left leash" margin) would kill on
            // the very next tick. The arm gate below measures the CORPSE to the
            // player, the release gate measures the FOLLOWER -- so a follower
            // parked past the leash would arm toward an in-leash corpse, be
            // judged "left leash", release (evicting into the actor alias),
            // then re-arm ~1/sec (deck: 5x in 8s, each eviction ejecting the
            // player from furniture pre-marker). Arm's-reach grabs are
            // unaffected -- they need no travel, so they are outside this guard.
            const bool followerBeyondLeash =
                origin.GetDistance(playerPos) > leashView.release;   // leash x1.15, teleport-capped

            // Act after the walk. Re-resolve each handle at act time (#2). Perf
            // pass: an IN-REACH source is drained in place (StripCorpse -- every
            // wanted category, dibs-gated) and the loop KEEPS GOING, so all loot
            // already within arm's reach clears in this one tick (bounded by
            // kMaxCandidates=48 + physical reach). Movement-class actions (arming
            // or retargeting a walk) still end the tick -- one DISPATCH per tick
            // (§4.8.3); only the in-place mutations batch.
            int drained = 0;   // in-reach sources drained this tick (normal mode)
            for (auto& h : candidates) {
                auto ptr = h.get();
                auto* ref = ptr.get();
                if (!ref) continue;
                const RE::FormID rid = ref->GetFormID();

                // CLAIM MUTATION BAR (#22g / #22g-QL): never take from a source
                // the player is CONSIDERING right now -- a vanilla ContainerMenu
                // OR (QuickLoot list) the crosshair on it. Mutating it would yank
                // an item out from under the menu/HUD the player is reading.
                if (PlayerIsConsidering(rid)) continue;

                const float df = origin.GetDistance(ref->GetPosition());

                // GROWN GRAB radius for this ref (stall cure): widens past
                // kArrivalDist with each path-fail so a body he can stand near
                // but never path the last gap to is taken from range. Loose
                // refs keep flat arm's reach (their acquire is a physical
                // Activate). A grab BEYOND plain arm's reach also honours the
                // player bubble -- never hoover a body you are standing over.
                const float grabR = LooseRef(ref) ? kArrivalDist : GrabRadiusFor(rid);
                // Clutter (arrows/bolts/lockpicks) is exempt from the bubble --
                // grabbed right under the player's feet, never deferred.
                const bool  grabOk = df <= kArrivalDist ||
                    (df <= grabR &&
                     (IsClutterCat(a_cat) ||
                      playerPos.GetDistance(ref->GetPosition()) > Config::g_playerBubble.load()));

                // ── EXCURSION MODE: the follower is already claimed (priority 60)
                // and driving a batch. The closest eligible candidate decides the
                // tick: within arm's reach -> grab it (a mutation); farther but
                // walkable -> RETARGET the excursion to it (movement, no release,
                // no turn-around). Do NOT start a new fill and do NOT release.
                if (a_mode == LootMode::kExcursion) {
                    // P7: excursion mode means this follower is already claimed, so
                    // a live slot must exist -- resolve it up front and drive it.
                    const int s = SlotIndexOf(a_follower->GetFormID());
                    if (s < 0) continue;   // no live slot: nothing to drive
                    TravelIntent& tr = g_travelSlots[s];
                    // A LOCKED ref (admitted by the lockpick judge) is never drained in
                    // place: it retargets like a loose ref, and the driver's ARRIVAL picks
                    // it first (LP-M1).
                    if (grabOk && !LooseRef(ref) && !ref->IsLocked()) {
                        // DRAIN IN PLACE (perf pass): he is on it (or within its
                        // grown grab radius), so take EVERYTHING his gambits want
                        // in this visit -- the same full strip the arrival path
                        // runs (every tier dibs-gated through TierReleased inside
                        // StripCorpse), not one category per tick.
                        bool leftWaiting = false;
                        const bool moved = g_svc
                            ? StripCorpse(a_follower, *g_svc, ref, a_now, &leftWaiting)
                            : LootHere(a_follower, ref, a_cat, a_potionWant);
                        if (leftWaiting) g_scanSawWaiting = true;
                        if (moved) {
                            g_grabGrow.erase(rid);      // grabbed -> stale grow verdict
                            g_travelFailed.erase(rid);  // back to normal sort priority
                            return true;
                        }
                        continue;   // nothing takeable here yet -- try the next
                    }
                    // COMMIT TO THE CURRENT LEG. RETARGET (below) resets the
                    // no-progress tracker (progressAt), so re-picking the closest
                    // ref EVERY tick meant an unreachable leg never accumulated the
                    // kNoProgress stall that sticky-blocklists it -- both followers
                    // churned unreachable corpses forever (v0.8.31: pathSpeed=0,
                    // navdist<<dist, legs switching every 2-4 s < the 7 s timer).
                    // While already walking to a still-valid, not-yet-stalled
                    // target, do NOT retarget: let the Walking-phase arrival/stall
                    // logic finish this leg, THEN the scan picks the next. Arm's-
                    // reach grabs (above) still fire; only the churn is stopped.
                    if (tr.phase == TravelPhase::Walking) {
                        auto  tptr = tr.target.get();
                        auto* cur  = tptr.get();
                        if (cur && !cur->IsDisabled() && !cur->IsMarkedForDeletion() &&
                            !TravelFailedRecently(cur->GetFormID(), a_now) &&
                            a_now - tr.progressAt <= kNoProgress)
                            return false;   // stay the course
                    }
                    // A LOOSE ref (route 2b) falls through to RETARGET even at
                    // arm's reach: the acquire runs at the driver's ARRIVAL
                    // (Activate dispatch), never as an in-place transfer here.
                    if (!Config::g_lootTravel.load())                              continue;
                    if (df > walkLimit)                                           continue;
                    // WALK-only anti-churn commit: never re-WALK a target inside
                    // its fail cooldown -- Retarget/Fill reset the no-progress
                    // clock, so an instant re-walk would defeat the stall verdict
                    // (the frozen-Erik loop). The unified failure model still
                    // keeps the ref IN THE RUNNING: it sorts to the back, and the
                    // in-range grown-grab path above never consults this list.
                    if (TravelFailedRecently(rid, a_now))                          continue;
                    // Clutter is exempt from the bubble (IsClutterCat, above).
                    if (!IsClutterCat(a_cat) &&
                        playerPos.GetDistance(ref->GetPosition())
                            <= Config::g_playerBubble.load())                      continue;
                    // OFF-NAVMESH GATE: if no navmesh is near the ref, the Travel
                    // package can't build a path and he'd freeze -- skip + blocklist
                    // (25s LRU, so the scan isn't re-run) BEFORE dispatch.
                    // TRANSIENT ONLY (stall-bug fix): this is a pre-dispatch
                    // HEURISTIC -- nearest-VERTEX misjudges stairs/rubble/body-
                    // piles -- and it used to accrue a stall strike, so two ticks
                    // of false verdict 5-min-stickied reachable loot (the
                    // "follower stands idle among corpses" stall). Never sticky
                    // a ref no walk was ever attempted at; the idle reassess
                    // wipes this transient block, so it re-tries once he moves.
                    // Each verdict also GROWS the ref's from-range grab radius
                    // (NotePathFail), so the usual cure is a later-tick hoover,
                    // not a retry of the walk.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelFailed(rid, a_now);   // off-navmesh -> transient only
                        NotePathFail(rid);              // -> widen its grab radius
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    // ROAD CHOICE, LEG BOUNDARY (A/B). A leg in flight FINISHES ON THE
                    // ROAD IT STARTED: the decision is the live ch.19 leg itself
                    // (APMF-side truth, one record per slot), never the switch, so a
                    // mid-excursion flip cannot split a slot. Road 2 re-points in place.
                    const bool ch19Leg = APMFBridge::HasLootTravelLeg(s);
                    const bool legTook = ch19Leg
                        ? APMFBridge::ClaimLootTravel(a_follower->GetFormID(), rid, s,
                                                      kCh19LootArrivalRadius)
                        : Packages::LootTravelRetarget(a_follower, ref, s);
                    if (!ch19Leg && legTook)
                        spdlog::info("[loot-road] {:08X}: RETARGET road=MFOPKG dest={:08X} slot={} "
                                     "radius={:.0f} handle=-",
                                     a_follower->GetFormID(), rid, s, 128.0f);
                    if (legTook) {
                        tr.target   = ref->GetHandle();
                        tr.cat      = a_cat;
                        tr.want     = a_potionWant;
                        tr.deadline = TravelDeadline(df, a_now);
                        tr.phase    = TravelPhase::Walking;
                        tr.lastPos    = origin;   // reset no-progress tracker
                        tr.progressAt = a_now;
                        tr.stolenSince = {};      // fresh leg -> fresh theft episode
                        // Fresh leg -> fresh ENGAGEMENT observation (DIAG-2026-09-06):
                        // a retarget rewrites the package's runtime target, so the
                        // previous leg's "engaged" reading says nothing about this one.
                        tr.legEngaged        = false;
                        tr.legStart          = a_now;
                        tr.nextLegPkgDiag    = {};   // -> first Walking tick reports
                        return true;   // new leg -- the excursion continues at 60
                    }
                    continue;
                }

                // ── NORMAL MODE: START an excursion by walking to a far corpse,
                // or transfer one within arm's reach. A LOOSE ref (route 2b)
                // takes the excursion path REGARDLESS of distance -- there is no
                // in-place transfer for it, the acquire is the driver's arrival
                // Activate -- so it must claim/walk even from arm's reach.
                // A LOCKED ref (admitted by the lockpick judge) takes the excursion
                // path at any distance too: its pick runs at the driver's ARRIVAL
                // (LP-M1), never as an in-place transfer.
                if ((!grabOk || LooseRef(ref) || ref->IsLocked()) && Config::g_lootTravel.load()) {
                    // Already drained in-reach loot this tick: that WAS the
                    // action. Candidates are closest-first, so everything in
                    // reach came before this far/loose one; arming a walk on
                    // top would stack a movement dispatch onto the mutations.
                    // The next tick's scan arms the excursion.
                    if (drained > 0) return true;
                    // CHURN GUARD (#48, computed above): he's already past the
                    // release margin -- an arm now dies "left leash" next tick.
                    // Skip the candidate entirely: it is far (or loose), so
                    // there is no in-place transfer to fall through to.
                    if (followerBeyondLeash) continue;
                    // Too far to WALK without abandoning the player -- leave it
                    // (following brings them closer later; also fairer to you).
                    if (df > walkLimit) continue;
                    // WALK-only anti-churn commit (see the excursion path note):
                    // no re-walk inside the fail cooldown; the ref still sorts
                    // last (not skipped) and the grown grab never checks this.
                    if (TravelFailedRecently(rid, a_now)) continue;
                    // CONVERGENCE YIELD: never walk to loot the player is right
                    // next to -- you win the race for the corpse you're heading to.
                    // Clutter is exempt from the bubble (IsClutterCat, above).
                    if (!IsClutterCat(a_cat) &&
                        playerPos.GetDistance(ref->GetPosition()) <= Config::g_playerBubble.load())
                        continue;
                    // P7: claim the first FREE slot; skip if all are busy.
                    const int s = FreeSlotIndex();
                    if (s < 0) continue;
                    // OFF-NAVMESH GATE (see the excursion path): no mesh near the
                    // ref -> no path -> freeze. Skip + blocklist before dispatch.
                    // TRANSIENT ONLY -- a pre-dispatch heuristic never earns a
                    // sticky strike (see the excursion-path note); it GROWS the
                    // ref's from-range grab radius instead.
                    if (NavmeshReach(a_follower, ref) > Config::g_navmeshGate.load()) {
                        MarkTravelFailed(rid, a_now);   // off-navmesh -> transient only
                        NotePathFail(rid);              // -> widen its grab radius
                        spdlog::info("[loot] {:08X}: {:08X} off-navmesh -- skipped (no path to it)",
                                     a_follower->GetFormID(), rid);
                        continue;
                    }
                    // ROAD CHOICE, EXCURSION START (A/B). The switch is read ONCE,
                    // here, at the only edge that starts an excursion; everything after
                    // follows the road that took. ROAD 2 claims ch.19 kIntent_Travel
                    // with the loot ref as the destination and NEVER calls
                    // LootTravelFill, so MFO's own package is never pointed and
                    // APMFBridge::OfferPackage is never called for this excursion --
                    // exactly ONE ch.9 package-offer claim exists for this follower
                    // either way (on road 2, APMF's own internal one at MFO's basis),
                    // never two. IsAPMFTravelHeld guards the one ordering that could
                    // stack them: a slot still flagged APMF-routed from a previous
                    // excursion holds a live MFO ch.9 offer, so it stays on road 1.
                    // RETREAT PRECEDENCE is re-checked here: on road 1 that guard is
                    // inside LootTravelFill, which road 2 never calls.
                    // A REFUSED ch.19 claim (APMF absent/pre-v10, APMF.esl missing,
                    // [Travel] bTravel=0, VR, all 8 APMF travel slots busy) falls back
                    // to road 1 for THIS excursion and says so: a refusal means APMF
                    // will do nothing at all. That is the ONLY fallback -- a road-1
                    // FAILURE still fails closed as today, and so does a road-2 one.
                    bool travelTook = false;
                    if (Config::g_lootTravelViaApmfTravel.load() &&
                        !Packages::IsAPMFTravelHeld(s) &&
                        !Packages::IsRetreating(a_follower->GetFormID()))
                        travelTook = APMFBridge::ClaimLootTravel(a_follower->GetFormID(), rid, s,
                                                                 kCh19LootArrivalRadius);
                    if (!travelTook) {
                        travelTook = Packages::LootTravelFill(a_follower, ref, s);
                        if (travelTook)
                            spdlog::info("[loot-road] {:08X}: DISPATCH road=MFOPKG dest={:08X} slot={} "
                                         "radius={:.0f} handle=-",
                                         a_follower->GetFormID(), rid, s, 128.0f);
                    }
                    if (travelTook) {
                        g_travelSlots[s].active    = true;
                        g_travelSlots[s].follower  = a_follower->GetFormID();
                        g_travelSlots[s].target    = ref->GetHandle();
                        g_travelSlots[s].cat       = a_cat;
                        g_travelSlots[s].want      = a_potionWant;
                        g_travelSlots[s].deadline  = TravelDeadline(df, a_now);
                        g_travelSlots[s].phase     = TravelPhase::Walking;
                        g_travelSlots[s].startTime = a_now;   // excursion begins now
                        g_travelSlots[s].lastPos    = origin;  // reset no-progress tracker
                        g_travelSlots[s].progressAt = a_now;
                        g_travelSlots[s].stolenSince = {};     // slots are reused -- clear stale episode
                        // Slots are reused -- clear the stale engagement observation too,
                        // or a new dispatch inherits the previous excursion's verdict.
                        g_travelSlots[s].legEngaged        = false;
                        g_travelSlots[s].legStart          = a_now;
                        g_travelSlots[s].nextLegPkgDiag    = {};
                        return true;   // committed to the walk; transfer on arrival
                    }
                    // Travel UNAVAILABLE (off AE, records unresolved, quest not
                    // running) -- fall through to the arm's-reach transfer below
                    // rather than never looting this candidate (the SE fallback).
                }

                // Corpse/container within (grown) reach: DRAIN it -- every
                // category his gambits want (StripCorpse; each tier still
                // dibs-gated through TierReleased), then keep going to the next
                // in-reach source. A far/loose ref that fell through here
                // (travel unavailable -- the SE fallback) keeps the old
                // one-transfer-per-tick shape.
                if (grabOk && !LooseRef(ref) && g_svc) {
                    if (StripCorpse(a_follower, *g_svc, ref, a_now, nullptr)) {
                        ++drained;
                        g_grabGrow.erase(rid);      // grabbed -> stale grow verdict
                        g_travelFailed.erase(rid);  // back to normal sort priority
                    }
                    continue;
                }
                if (LootHere(a_follower, ref, a_cat, a_potionWant)) return true;
            }
            if (drained > 0) {
                if (drained > 1)
                    spdlog::info("[loot] {:08X}: drained {} in-reach sources in one tick",
                                 a_follower->GetFormID(), drained);
                return true;
            }
            // SKIP-REASON DIAGNOSTIC (v0.8.36). Nothing acted this tick even though
            // the scan collected candidates -- recompute WHY the CLOSEST eligible
            // body was passed over, so "loot efficiency is bad" is a measured fact,
            // not a guess (marth: Eric found the potion in front of him only after
            // several runs). Rate-limited per (follower,cat) 10 s, mirroring the scan
            // log. The committed-to-leg early-out (return false while Walking) never
            // reaches here, so this fires only on a genuine no-op tick.
            if (!candidates.empty()) {
                static std::unordered_map<std::uint64_t, Clock::time_point> s_nextSkipLog;
                const auto key = (static_cast<std::uint64_t>(a_follower->GetFormID()) << 8)
                               | static_cast<std::uint64_t>(a_cat);
                auto& nxt = s_nextSkipLog[key];
                if (nxt.time_since_epoch().count() == 0 || a_now >= nxt) {
                    nxt = a_now + std::chrono::seconds(10);
                    auto p0 = candidates.front().get();
                    if (auto* r0 = p0.get()) {
                        const auto  r0id = r0->GetFormID();
                        const float dF   = origin.GetDistance(r0->GetPosition());
                        const float dP   = playerPos.GetDistance(r0->GetPosition());
                        spdlog::info("[lootskip] {:08X} cat={} closest={:08X} distF={:.0f} distPlayer={:.0f} "
                                     "walkLimit={:.0f} | considering={} bubble={}(<{:.0f}) blocklist={} "
                                     "aliasBusy={} navdist={:.0f}(gate {:.0f})",
                                     a_follower->GetFormID(), CatName(a_cat), r0id, dF, dP, walkLimit,
                                     PlayerIsConsidering(r0id),
                                     // Clutter is bubble-exempt (IsClutterCat) --
                                     // report false/exempt so the log stays truthful.
                                     !IsClutterCat(a_cat) && dP <= Config::g_playerBubble.load(),
                                     Config::g_playerBubble.load(),
                                     TravelFailedRecently(r0id, a_now),
                                     (FreeSlotIndex() < 0 && SlotIndexOf(a_follower->GetFormID()) < 0),
                                     NavmeshReach(a_follower, r0), Config::g_navmeshGate.load());
                    }
                }
            }
            // Excursion Hold decision needs to know if loot is still WAITING on
            // the player's dibs (vs genuinely nothing left).
            if (a_mode == LootMode::kExcursion && dNotYet > 0) g_scanSawWaiting = true;
            return false;
        }

        // ARM'S-REACH FULL STRIP (marth: "he takes the body's gold but leaves its
        // Iron Arrow (9)"). On arrival the follower is standing ON the corpse, so
        // take EVERYTHING his gambits currently want in this ONE visit -- not just
        // the category the trip was for. Without this, a gold trip loots the gold,
        // marks the corpse DONE (blocklisted ~25s), then walks back to the player,
        // stranding the arrows/potions in a body now past arm's reach AND on the
        // blocklist -- exactly the 340u/382u arrow bodies in the deck test.
        //
        // Transfers straight from the KNOWN corpse (no world scan, no retarget).
        // The Evaluate walk means a category is taken only if its gambit CONDITION
        // is true right now (out of arrows, low on potions; gold's Always) -- #28,
        // never loot a thing the player's rules don't ask for. Wait ends the sweep
        // (the deliberate STOP gambit). Returns true if anything moved.
        bool StripCorpse(RE::Actor* a_follower, const FollowerState& a_state,
                         RE::TESObjectREFR* a_corpse, Clock::time_point a_now,
                         bool* a_leftWaiting) {
            bool moved = false;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const RE::NiPoint3 playerPos = pc ? pc->GetPosition() : a_follower->GetPosition();
            for (int start = 0; ; ) {
                const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
                if (choice.ruleIndex < 0) break;
                const auto& op = choice.actionOpcode;
                Category cat = Category::Gold; RE::ActorValue want = RE::ActorValue::kNone;
                bool isLoot = true;
                if      (op == Vocab::kActLootArrows)         cat = Category::Arrows;
                else if (op == Vocab::kActLootBolts)          cat = Category::Bolts;
                else if (op == Vocab::kActLootPotions)      { cat = Category::Potions; }
                else if (op == Vocab::kActLootHealthPotion) { cat = Category::Potions; want = RE::ActorValue::kHealth;  }
                else if (op == Vocab::kActLootStaminaPotion){ cat = Category::Potions; want = RE::ActorValue::kStamina; }
                else if (op == Vocab::kActLootMagickaPotion){ cat = Category::Potions; want = RE::ActorValue::kMagicka; }
                else if (op == Vocab::kActLootEquipment)      cat = Category::Equipment;
                else if (op == Vocab::kActLootGold)           cat = Category::Gold;
                else if (op == Vocab::kActLootJewelry)        cat = Category::Jewelry;
                else if (op == Vocab::kActLootSoulGems)       cat = Category::SoulGems;
                else if (op == Vocab::kActLootLockpicks)      cat = Category::Lockpicks;
                else if (op == Vocab::kActLootIngredients)    cat = Category::Ingredients;
                else if (op == Vocab::kActLootValuables)      cat = Category::Valuables;
                else if (op == Vocab::kActWait) break;   // user's STOP gambit ends the sweep
                else isLoot = false;
                if (isLoot) {
                    // CLAIM-AND-RELEASE parity with the scan (#2): a free-tier
                    // arm's-reach strip must not snatch a fresh kill's Gear/Valuables
                    // ahead of the player's dibs. Gate the non-free tiers by the SAME
                    // predicate LootNearby uses; if it's still the player's, leave it
                    // and flag WAITING so the caller does NOT mark the body DONE --
                    // the excursion linger revisits it once the claim releases.
                    if (!TierReleased(cat, a_corpse, playerPos, a_now)) {
                        if (a_leftWaiting) *a_leftWaiting = true;
                        start = choice.ruleIndex + 1;
                        continue;
                    }
                    // Equipment moves ONE piece per call (§4.3's shape) while the
                    // other categories take all they want internally -- but this
                    // visit is the corpse's LAST (it's marked DONE right after we
                    // return). One call stranded everything past the first take:
                    // a body with a better sword AND the bow the equip-ranged
                    // gambit needs (dual-primary) yielded only the sword, and the
                    // bow/cuirass sat blocklisted for 25 s or forever. Drain the
                    // category: each take raises the inventory-derived baseline
                    // (armor now baselines against carried pieces too, #3), so this
                    // terminates; the guard is belt-and-braces (2 weapon roles + 7
                    // armor slots).
                    int guard = (cat == Category::Equipment) ? 10 : 1;
                    while (guard-- > 0 && LootHere(a_follower, a_corpse, cat, want)) moved = true;
                }
                start = choice.ruleIndex + 1;
            }
            return moved;
        }

        // A LEG BOUNDARY on an active excursion: run ONLY the follower's LOOT
        // gambits, in excursion mode, to grab an arm's-reach corpse or retarget
        // to the next walkable one. LOOT-ONLY (no drink/wait) so a leg boundary
        // never fires a second real action in the tick (the arrival transfer was
        // this tick's mutation; a retarget is movement). Returns true if it acted
        // (grabbed or retargeted). Sets g_scanSawWaiting via LootNearby.
        bool RunExcursionScan(RE::Actor* a_follower, const FollowerState& a_state,
                              Clock::time_point a_now) {
            g_scanSawWaiting = false;
            // LOOT RUN ORDERING (marth: no-dibs + closest first, dibs later in
            // the run). Pass 0 tries only free-tier categories, in gambit-table
            // order, so a leg boundary always grabs/retargets to what's
            // immediately available before ever considering a dibs-gated
            // category; pass 1 is the original unrestricted walk (a released
            // dibs item still retargets to normally once nothing free-tier hit).
            for (int pass = 0; pass < 2; ++pass) {
            const bool restrictFree = (pass == 0);
            for (int start = 0; ; ) {
                const auto choice = Eval::Evaluate(a_follower, a_state, Table::Logistics, start);
                if (choice.ruleIndex < 0) break;
                const auto& op = choice.actionOpcode;
                if (restrictFree && IsDibsTierLootOp(op)) {
                    start = choice.ruleIndex + 1; continue;   // dibs-tier: defer to pass 1
                }
                bool acted = false, isLoot = true;
                if      (op == Vocab::kActLootArrows)         acted = LootNearby(a_follower, Category::Arrows,  a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootBolts)          acted = LootNearby(a_follower, Category::Bolts,   a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootPotions)        acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootHealthPotion)   acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kHealth,  LootMode::kExcursion);
                else if (op == Vocab::kActLootStaminaPotion)  acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kStamina, LootMode::kExcursion);
                else if (op == Vocab::kActLootMagickaPotion)  acted = LootNearby(a_follower, Category::Potions, a_now, RE::ActorValue::kMagicka, LootMode::kExcursion);
                else if (op == Vocab::kActLootEquipment)      acted = LootNearby(a_follower, Category::Equipment, a_now, RE::ActorValue::kNone,  LootMode::kExcursion);
                else if (op == Vocab::kActLootGold)           acted = LootNearby(a_follower, Category::Gold,    a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootJewelry)        acted = LootNearby(a_follower, Category::Jewelry, a_now, RE::ActorValue::kNone,    LootMode::kExcursion);
                else if (op == Vocab::kActLootSoulGems)       acted = LootNearby(a_follower, Category::SoulGems, a_now, RE::ActorValue::kNone,   LootMode::kExcursion);
                else if (op == Vocab::kActLootLockpicks)      acted = LootNearby(a_follower, Category::Lockpicks, a_now, RE::ActorValue::kNone,  LootMode::kExcursion);
                else if (op == Vocab::kActLootIngredients)    acted = LootNearby(a_follower, Category::Ingredients, a_now, RE::ActorValue::kNone, LootMode::kExcursion);
                else if (op == Vocab::kActLootValuables)      acted = LootNearby(a_follower, Category::Valuables, a_now, RE::ActorValue::kNone,   LootMode::kExcursion);
                else if (op == Vocab::kActWait) return false;   // Wait is the user's deliberate STOP
                                                         // gambit (#3.3): end the batch -- returning
                                                         // false with nothing "waiting" makes Holding
                                                         // release. A hard return (not `break`) so
                                                         // pass 1 can't override the user's stop.
                else isLoot = false;
                if (isLoot && acted) return true;
                start = choice.ruleIndex + 1;   // skip non-loot / no-op, try next
            }
            }
            return false;
        }
}
