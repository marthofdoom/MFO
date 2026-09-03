// Logistics_Economy.cpp -- the #21 ECONOMY side of the logistics family
// (split mechanically out of Logistics.cpp, no logic change): mage-apparel
// scoring, the vendor probe (EconomyProbe + BuildBuyThresholds), College
// tome-gate unlock, owned-gear equip pass, and the public GEAR/TOME buy
// helpers shared with TradeBridge::PlanBuy (see Logistics.h).
#include "Logistics_internal.h"

namespace MFO::Logistics {


        // How strongly this armor boosts the target school: the count of
        // beneficial fortify-<school> effects on its BASE enchantment (a_outMag
        // sums their magnitudes -- the fanciness input below), with a
        // keyword-tag last resort for gear whose school lives nowhere in its
        // effects. A RUNTIME read of the candidate's own record, deliberately
        // NOT a catalog field, so it works out-of-box on modded gear with no
        // patcher run. Pure form data -> worker-safe. Player-enchanted
        // INSTANCES carry their enchant in extra data, not the base form --
        // out of scope (world/corpse mage gear is base-enchanted).
        int SchoolMatchScore(const RE::TESObjectARMO* a_armo, RE::ActorValue a_school,
                             float* a_outMag = nullptr) {
            if (a_outMag) *a_outMag = 0.0f;
            if (!a_armo || a_school == RE::ActorValue::kNone) return 0;
            int score = 0;
            const auto* ench = a_armo->formEnchanting;
            if (ench) {
                for (const auto* e : ench->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (!mgef) continue;
                    using Flag = RE::EffectSetting::EffectSettingData::Flag;
                    // A curse ("Destruction costs MORE") must never read as a boost.
                    if (mgef->data.flags.any(Flag::kDetrimental) ||
                        mgef->data.flags.any(Flag::kHostile)) continue;
                    if (EffectBoostSchool(mgef) != a_school) continue;
                    ++score;
                    if (a_outMag) *a_outMag += e->GetMagnitude();
                }
                // The effects said nothing but the ENCHANTMENT record itself
                // is school-tagged -- count it once (magnitude unknown, so the
                // fanciness rank falls back to gold value alone).
                if (score == 0 && SchoolFromKeywords(ench) == a_school) score = 1;
            }
            // ...and the ARMO record's own tags, for robes whose school is
            // authored only as an item keyword.
            if (score == 0 && SchoolFromKeywords(a_armo) == a_school) score = 1;
            return score;
        }

        // The slots MAGE gear lives on: body robe, head hood (head+hair bits)
        // or circlet, hands, feet. Deliberately NOT amulet/ring (jewellery is
        // the player's Valuables tier, looted separately) and NOT shield/
        // forearms/calves -- school apparel never occupies those, and scoping
        // the preference here means a magic user's OTHER slots keep the plain
        // rating rules (don't strip gear he legitimately uses).
        using BipedSlot = RE::BGSBipedObjectForm::BipedObjectSlot;
        constexpr BipedSlot kMageSlots[] = {
            BipedSlot::kHead, BipedSlot::kHair, BipedSlot::kCirclet,
            BipedSlot::kBody, BipedSlot::kHands, BipedSlot::kFeet,
        };

        // The mage-apparel ranking key. SCHOOL MATCH IS PRIMARY -- a fancy
        // wrong-school robe must never beat a plain right-school one (marth);
        // FANCINESS (base gold value + total fortify-school magnitude) ranks
        // within a school tier, so with MEO's finer enchanted stock in the
        // world the richer robe wins. Both are form-DATA reads, worker-safe.
        // (MEO socket-capacity scoring was considered and rejected: MEO_API
        // queries are main-thread-only per MEOBridge.h, and this runs on the
        // worker -- value+magnitude already ranks MEO gear up, and the
        // existing gem transfer below carries invested gems onto the upgrade.)
        struct MageKey { int score = 0; float fancy = -1.0f; };
        MageKey MageApparelKey(const RE::TESObjectARMO* a_armo, RE::ActorValue a_school) {
            float mag = 0.0f;
            const int score = SchoolMatchScore(a_armo, a_school, &mag);
            const auto value = static_cast<float>(std::max<std::int32_t>(a_armo->GetGoldValue(), 0));
            return { score, value + mag };
        }
        bool MageKeyBeats(const MageKey& a_cand, const MageKey& a_worn) {
            return a_cand.score > a_worn.score ||
                   (a_cand.score == a_worn.score && a_cand.fancy > a_worn.fancy);
        }

        // ── per-candidate apparel diagnostics (v1.0.31) ─────────────────────
        // The v1.0.29 field failure was INVISIBLE: Marcurio's valid Destruction
        // robe scored 0 (its LoreRim enchantment wired no vanilla school AV)
        // and the log only ever showed the winner, so there was nothing to
        // debug from -- just "circlet -> best" and a wrong guess. Dump the
        // REAL record data for every apparel candidate a magic user evaluates:
        // if the broadened chain STILL misses on some robe, the next fix comes
        // from this log, not another guess. Throttled by a session-scope
        // dedupe (one dump per base form + target school): corpses repeat the
        // same base records endlessly and peek passes re-walk them every tick,
        // so without the dedupe this would spam. Touched only from the
        // logistics worker (sequential, like g_svc) -> no lock needed.
        void LogMageApparelDiag(RE::TESObjectARMO* a_armo, RE::ActorValue a_school) {
            static std::unordered_set<std::uint64_t> s_seen;
            const auto key = static_cast<std::uint64_t>(a_armo->GetFormID()) |
                             (static_cast<std::uint64_t>(a_school) << 32);
            if (s_seen.size() > 512) s_seen.clear();   // bounded; worst case is a re-dump
            if (!s_seen.insert(key).second) return;

            float     mag   = 0.0f;
            const int score = SchoolMatchScore(a_armo, a_school, &mag);
            const auto* ench = a_armo->formEnchanting;
            spdlog::info("[loot] apparel {:08X} '{}' rating={:.0f} value={} ench={:08X} kw=[{}] -> school {} score={} mag={:.0f}",
                         a_armo->GetFormID(),
                         a_armo->GetFullName() ? a_armo->GetFullName() : "?",
                         a_armo->GetArmorRating(), a_armo->GetGoldValue(),
                         ench ? ench->GetFormID() : 0, KeywordCsv(a_armo),
                         SchoolName(a_school), score, mag);
            if (!ench) return;
            int i = 0;
            for (const auto* e : ench->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef) { ++i; continue; }
                using Flag = RE::EffectSetting::EffectSettingData::Flag;
                const bool det = mgef->data.flags.any(Flag::kDetrimental) ||
                                 mgef->data.flags.any(Flag::kHostile);
                spdlog::info("[loot] apparel {:08X} eff#{} mgef={:08X} '{}' primAV={} assocSkill={} secAV={} arch={} mag={:.1f} kw=[{}] -> {}",
                             a_armo->GetFormID(), i, mgef->GetFormID(),
                             mgef->GetFullName() ? mgef->GetFullName() : "?",
                             static_cast<int>(mgef->data.primaryAV),
                             static_cast<int>(mgef->data.associatedSkill),
                             static_cast<int>(mgef->data.secondaryAV),
                             static_cast<int>(mgef->data.archetype),
                             e->GetMagnitude(), KeywordCsv(mgef),
                             det ? "curse" : SchoolName(EffectBoostSchool(mgef)));
                ++i;
            }
        }

        // The school-scored apparel path -- since v1.0.31 the magic user's
        // ONLY apparel path (ArmorIsBetter never runs for him; see the loot
        // loop). Same shape as it: a strict upgrade on at least one MAGE slot
        // it covers, beaten on none. Rules (marth, v1.0.31 -- PURE CASTER):
        //   - RATED armor -- heavy OR light, school-enchanted or not -- is
        //     NEVER mage loot. v1.0.29's first line handed "score 0 but
        //     rated" back to the rating path, which is exactly how Marcurio,
        //     a detected Destruction user, looted a Dwarven Heavy Cuirass. A
        //     magic user takes clothing/robes (rating 0) ONLY. Armor he
        //     ALREADY wears is never force-stripped: it just stops being
        //     replaced by more armor, until a school robe displaces it;
        //   - a school-MATCHING piece may replace anything on a mage slot
        //     (the point of the feature: school beats raw rating there);
        //   - a GENERIC piece (score 0 -- plain clothing) must be a GENUINE
        //     upgrade: it may dress a BARE clothing slot (head/body/hands/
        //     feet) or strictly beat worn rating-0 rags on value. It never
        //     fills an empty kHair/kCirclet bit -- a plain circlet is
        //     jewellery, the player's Valuables tier, and "Copper and
        //     Moonstone Circlet -> best" (deck log, v1.0.30) was junk, not an
        //     upgrade -- and it NEVER replaces real worn armor;
        //   - a tie does NOT swap (stable -- no loot thrash).
        // (No ArmorClassSuits call anymore: everything past the rating gate
        // is clothing, which that check passes unconditionally.)
        // #21: superseded by the unified MageApparelBuyKey (loot now uses the same
        // MEO-aware value/school ranking as buy). Kept for reference/possible reuse.
        [[maybe_unused]] bool MageApparelIsBetter(RE::Actor* a_follower, RE::TESObjectARMO* a_armo,
                                 RE::ActorValue a_school, const MageKey& a_key) {
            if (a_armo->GetArmorRating() > 0.0f)
                return false;   // PURE CASTER: rated armor is never mage loot
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
            bool overlapsAny = false;
            bool genuine     = a_key.score > 0;   // school gear justifies itself
            for (const auto slot : kMageSlots) {
                if (!(mask & static_cast<std::uint32_t>(slot))) continue;
                overlapsAny = true;
                auto* worn = a_follower->GetWornArmor(slot);
                if (!worn) {
                    // Open slot -> nothing to beat. A GENERIC piece only earns
                    // its take here when it actually DRESSES him: a bare
                    // clothing slot, never a bare circlet/hair bit.
                    if (a_key.score <= 0 &&
                        (slot == BipedSlot::kHead  || slot == BipedSlot::kBody ||
                         slot == BipedSlot::kHands || slot == BipedSlot::kFeet))
                        genuine = true;
                    continue;
                }
                if (a_key.score <= 0 && worn->GetArmorRating() > 0.0f)
                    return false;      // a generic piece never strips real armor
                if (!MageKeyBeats(a_key, MageApparelKey(worn, a_school)))
                    return false;      // beaten (or tied) on a slot it would replace
                if (a_key.score <= 0) genuine = true;   // strictly beats worn rags
            }
            return overlapsAny && genuine;
        }

        bool VendorTrades(RE::BGSListForm* a_vend, bool a_notBuySell,
                          RE::BGSKeywordForm* a_kwf) {
            if (!a_vend || !a_kwf) return false;
            bool match = false;
            for (auto* f : a_vend->forms) {
                auto* kw = f ? f->As<RE::BGSKeyword>() : nullptr;
                if (kw && a_kwf->HasKeyword(kw)) { match = true; break; }
            }
            return a_notBuySell ? !match : match;
        }


        // ── #21 College-of-Winterhold TOME-GATE UNLOCK ──────────────────────────
        // MECHANISM (measured from the load order): each College spell-tome tier is a
        // leveled list whose chance-none (LVLD=100 => yields nothing) is overridden by
        // a GLOBAL (LVLG). Default 100 => the tome is absent from the chest. Vanilla
        // quest WISkillIncrease02 (QUST 000F2593, subject=PLAYER) sets the tier global
        // to 0 when the player crosses the tier skill, and the tome appears at the next
        // NATURAL chest restock. MFO GENERALIZES that player gate to the whole party:
        // flip the SAME global to 0 once the player OR any active follower reaches the
        // tier skill, so a follower's own skill unlocks College tomes for the party to
        // buy (EconomyProbe -> PlanBuy). Deliberately conservative:
        //   * ONE-WAY -- only ever set 0 (mirrors vanilla; never back to 100, so it
        //     never fights the player's own WISkillIncrease progression tracking);
        //   * NATURAL restock only -- NO forced chest regen, NO merchant mutation;
        //   * idempotent -- once a gate is 0 it is a no-op.
        // The 15 PC{School}{tier} globals are Skyrim.esm (load index 00) stable
        // FormIDs, enumerated with tools/esp_inspect.py (EDID -> FormID below).
        // Thresholds are the vanilla skill-tier boundaries Adept 50 / Expert 75 /
        // Master 100 (WISkillIncrease02 sets Adept@50 and Expert@75 -- confirmed;
        // Master@100 is the tier boundary. In vanilla few/no PURCHASABLE lists read the
        // Master global -- Master tomes are ritual-quest rewards -- but flipping it is
        // harmless: one-way and a no-op if nothing reads it). Any global that fails to
        // resolve (a modlist without it) is skipped safely.
        void UnlockCollegeTomes() {
            // GLOBAL rate-limit (~30s), NOT per follower-tick: a party-wide roster
            // sweep is cheap but pointless to run every 133ms.
            static Clock::time_point s_next{};
            const auto now = Clock::now();
            if (s_next.time_since_epoch().count() != 0 && now < s_next) return;
            s_next = now + std::chrono::seconds(30);

            using AV = RE::ActorValue;
            static constexpr AV kSchoolAV[5] = {
                AV::kAlteration, AV::kConjuration, AV::kDestruction, AV::kIllusion, AV::kRestoration,
            };
            // Party-wide MAX BASE skill per school. BASE (GetBaseActorValue), never
            // GetActorValue, so a temporary Fortify-<school> buff cannot exploit the
            // unlock. Player + every active follower (worker/serial domain -- the same
            // unlocked g_active read Scheduler uses; never off the worker, #4).
            float mx[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
            auto accumulate = [&](RE::Actor* a) {
                auto* o = a ? a->AsActorValueOwner() : nullptr;
                if (!o) return;
                for (int i = 0; i < 5; ++i) mx[i] = std::max(mx[i], o->GetBaseActorValue(kSchoolAV[i]));
            };
            accumulate(RE::PlayerCharacter::GetSingleton());
            for (auto& h : Followers::g_active) {
                auto ptr = h.get();
                accumulate(ptr.get());
            }

            // The gate table (esp_inspect.py, Skyrim.esm). schoolIdx indexes mx[].
            struct Gate { RE::FormID id; int schoolIdx; float threshold; const char* edid; };
            static constexpr Gate kGates[] = {
                { 0x000F2584, 0,  50.f, "PCAlterationAdept"   }, { 0x000F2585, 0,  75.f, "PCAlterationExpert"  }, { 0x000F2586, 0, 100.f, "PCAlterationMaster"  },
                { 0x000F2587, 1,  50.f, "PCConjurationAdept"  }, { 0x000F2588, 1,  75.f, "PCConjurationExpert" }, { 0x000F2589, 1, 100.f, "PCConjurationMaster" },
                { 0x000F258A, 2,  50.f, "PCDestructionAdept"  }, { 0x000F258B, 2,  75.f, "PCDestructionExpert" }, { 0x000F258C, 2, 100.f, "PCDestructionMaster" },
                { 0x000F258D, 3,  50.f, "PCIllusionAdept"     }, { 0x000F258E, 3,  75.f, "PCIllusionExpert"    }, { 0x000F258F, 3, 100.f, "PCIllusionMaster"    },
                { 0x000F2590, 4,  50.f, "PCRestorationAdept"  }, { 0x000F2591, 4,  75.f, "PCRestorationExpert" }, { 0x000F2592, 4, 100.f, "PCRestorationMaster" },
            };

            std::vector<RE::FormID> flips;
            for (const auto& g : kGates) {
                auto* glob = RE::TESForm::LookupByID<RE::TESGlobal>(g.id);
                if (!glob) continue;                       // modlist without it -> safe no-op
                if (glob->value != 0.0f && mx[g.schoolIdx] >= g.threshold) {   // one-way: only unlock
                    flips.push_back(g.id);
                    spdlog::info("[college] unlock {} -- party {} base skill {:.0f} >= {:.0f}",
                                 g.edid, SchoolName(kSchoolAV[g.schoolIdx]), mx[g.schoolIdx], g.threshold);
                }
            }
            if (flips.empty()) return;

            // Game-state mutation goes to MAIN (MFO rule). Capture FormIDs + the
            // target value only; re-resolve on-frame; re-check so it stays ONE-WAY.
            auto doFlip = [flips = std::move(flips)]() {
                for (const RE::FormID id : flips)
                    if (auto* glob = RE::TESForm::LookupByID<RE::TESGlobal>(id); glob && glob->value != 0.0f)
                        glob->value = 0.0f;
            };
            if (MainThread::IsInstalled()) MainThread::Post(doFlip);
            else                           doFlip();   // VR: pump is a no-op, keep the direct path
        }

        // ── #21 PART 2A: wear the best OWNED gear (makes a purchase functional) ──
        // A buy (RunTrade) only TRANSFERS goods into the follower's pack; nothing
        // wears them. This worker-tick pass equips the single best owned upgrade the
        // follower isn't wearing yet -- bought, looted-as-valuable (jewelry), or
        // player-handed -- through the SAME safe AcquireEquip step the loot judge
        // uses (MainThread::Post EquipObject, MEO gem carry, never DoReset3D). ONE
        // item per tick and idempotent (equips only when it beats what's worn), so
        // once worn it no-ops -> no thrash. Apparel/armor only here; WEAPONS are left
        // to the combat equip gambit (Loadout::Prepare picks the best in-class at
        // combat), matching the loot judge's stock-not-equip rule for weapons.
        void EquipBestOwnedGear(RE::Actor* a_follower, const FollowerState& a_state) {
            if (!a_follower || Config::g_dollsMode.load()) return;
            const bool caster         = IsCasterFollower(a_state);
            const bool useMageApparel = caster && Config::g_mageWearRobes.load();
            auto* eqObj  = a_follower->GetEquippedObject(false);
            auto* myWeap = eqObj ? eqObj->As<RE::TESObjectWEAP>() : nullptr;

            RE::TESBoundObject* pick = nullptr;
            if (useMageApparel) {
                // AUTHORITATIVE mage dress-up (oscillation fix). Compute the single
                // best OWNED piece per logical slot (deterministic, FormID tiebreak so
                // two equal-value pieces never flip which one is enforced) and FORCE it
                // worn whenever the follower isn't already wearing exactly it -- so an
                // engine-re-equipped LESSER clothing piece is corrected (replaced) and
                // then falls through to the sell loop as an extra, instead of being
                // worn-protected forever (the Nord-Tribal-vs-Khajiit oscillation).
                // Ranked by the SAME MEO-aware judge as loot/buy. The worn piece is
                // itself a candidate, so a worn BEST never downgrades (no-op), and
                // equipping a replacement auto-unequips the lesser -> never strips naked.
                const std::uint8_t top2 = TopTwoSchoolMask(a_follower);
                const bool schoolPrimary = !MEOBridge::Available() || Config::g_mageApparelStrictSchool.load();
                const bool allowVillain  = IsNecromancerFollower(a_state);

                struct BestPer { RE::TESBoundObject* obj = nullptr; int tier = -1; std::int32_t metric = -1; };
                BestPer bestSlot[6];
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar) continue;
                    const int slot = MageClothingSlot(ar);
                    if (slot < 0) continue;
                    int t = 0; std::int32_t m = 0;
                    // The currently-WORN piece is always its slot's incumbent candidate,
                    // ranked with allowVillain=true (restores the pre-authoritative
                    // asymmetry): a player-equipped villain/necromancer robe on a
                    // non-necromancer stays eligible instead of being excluded from
                    // candidacy, force-replaced by a lesser common piece, and then sold.
                    const bool worn = (WornInLogicalSlot(a_follower, slot) == obj);
                    if (!MageApparelBuyKey(ar, top2, schoolPrimary, allowVillain || worn, t, m)) continue;
                    auto& b = bestSlot[slot];
                    if (!b.obj || t > b.tier || (t == b.tier && m > b.metric) ||
                        (t == b.tier && m == b.metric && obj->GetFormID() > b.obj->GetFormID()))
                        b = { obj, t, m };
                }
                // First slot whose worn piece isn't the computed best -> one
                // authoritative correction per tick (converges over ticks, no thrash).
                for (int slot = 0; slot < 6; ++slot) {
                    auto* best = bestSlot[slot].obj;
                    if (!best) continue;                                    // nothing owned for this slot
                    if (WornInLogicalSlot(a_follower, slot) == best) continue;   // already correct
                    if (!FitsCarryWeight(a_follower, best->GetWeight())) continue;
                    pick = best; break;
                }
            } else {
                // RATED ARMOR: best owned piece that upgrades a worn/bare slot.
                float bestRat = 0.0f;
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || IsCreatureArmor(ar)) continue;
                    // #3 NO IsExcluded skip here: a follower MAY wear an artifact it
                    // already owns (a legit upgrade) -- equip stays permissive, matching
                    // the mage-clothing branch above. Looting/selling/shedding an
                    // artifact is still barred by IsExcluded on those paths.
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    if ((static_cast<std::uint32_t>(ar->GetSlotMask()) &
                         static_cast<std::uint32_t>(Slot::kShield)) != 0) continue;   // shields: leave to the loot role logic
                    if (!ArmorIsBetter(a_follower, ar)) continue;   // strictly better than worn on its slot
                    if (ar->GetArmorRating() > bestRat) { bestRat = ar->GetArmorRating(); pick = obj; }
                }
            }

            if (!pick || !FitsCarryWeight(a_follower, pick->GetWeight())) return;
            if (useMageApparel) {
                // THRASH GUARD for the authoritative mage correction: only OUT OF
                // COMBAT and rate-limited per follower, so an engine tug-of-war over a
                // clothing slot can't drive an equip every frame. (The rated-armor
                // branch is already strictly-better-only, so it needs no rate limit.)
                if (a_follower->IsInCombat()) return;
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextMageFix;
                const auto id  = a_follower->GetFormID();
                const auto now = Clock::now();
                auto& nxt = s_nextMageFix[id];
                if (nxt.time_since_epoch().count() != 0 && now < nxt) return;
                nxt = now + std::chrono::seconds(5);
            }
            AcquireEquip(a_follower, pick, nullptr, myWeap, /*forceStock*/false);   // already owned -> no transfer
        }

        // ── #21 buy thresholds (Features A + B) ─────────────────────────────────
        // Computed ONCE per follower per scan on the WORKER, reusing the loot judge
        // (ComputeWeaponRoles / owned-inventory baseline / MageClothingSlot), then
        // handed to the VM-side PlanBuy as pure numbers. Keeps every follower actor-
        // value / gambit read off the VM thread (task: prefer precompute on worker).
        TradeBridge::BuyThresholds BuildBuyThresholds(RE::Actor* a_follower,
                                                      const FollowerState& a_state) {
            TradeBridge::BuyThresholds buy;
            const bool dolls = Config::g_dollsMode.load();
            // "Is a caster" for apparel/weapon-role (gambit signal == loot mageMode);
            // gate mage apparel additionally on bMageWearRobes (marth).
            const bool caster         = IsCasterFollower(a_state);
            const bool useMageApparel = caster && Config::g_mageWearRobes.load();
            const std::uint8_t top2   = TopTwoSchoolMask(a_follower);

            // ── Feature A: weapon/armor/apparel thresholds (reuse the loot judge) ──
            if (Config::g_economyBuyGear.load()) {
                buy.buyGear = true;
                const bool wantsRanged = TableHasAction(a_state.combat(), Vocab::kActEquipRanged);
                const bool wantsMelee  = TableHasAction(a_state.combat(), Vocab::kActEquipMelee);
                const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);
                const std::uint8_t baseClass = a_state.combatClassOverride;
                const bool baseCaster     = baseClass == 3;
                const bool baseWeaponUser = baseClass == 1 || baseClass == 2;
                // meleeTargetClass / doRanged mirror the loot judge (weapon role uses
                // 'caster' == mageMode, NOT the apparel toggle).
                const WepClass meleeTargetClass =
                    (caster && !baseWeaponUser && (baseCaster || !wantsMelee))
                        ? WepClass::Other : roles.melee;
                const bool doRanged     = (caster && !wantsRanged) ? false : roles.doRanged;
                const bool wantCrossbow = roles.wantCrossbow;
                using WT = RE::WEAPON_TYPE;

                std::uint16_t baseDmg = 0, myRangedDmg = 0;
                float slotRat[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };   // per ArmorBuySlot (0 head..4 shield)
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                        if (IsCreatureWeapon(w) || Catalog::IsExcluded(obj->GetFormID())) continue;
                        if (meleeTargetClass != WepClass::Other &&
                            WeaponClassOf(w->GetWeaponType()) == meleeTargetClass)
                            baseDmg = std::max(baseDmg, w->GetAttackDamage());
                        if (doRanged && w->GetWeaponType() == (wantCrossbow ? WT::kCrossbow : WT::kBow))
                            myRangedDmg = std::max(myRangedDmg, w->GetAttackDamage());
                    } else if (auto* ar = obj->As<RE::TESObjectARMO>()) {
                        if (IsCreatureArmor(ar)) continue;
                        // PER-SLOT owned baseline so a bare head/hands/feet still buys
                        // even when a good chestpiece owns a high overall rating.
                        if (const int as = ArmorBuySlot(ar);
                            as >= 0 && ar->GetArmorRating() > slotRat[as])
                            slotRat[as] = ar->GetArmorRating();
                    }
                }
                buy.meleeClass    = static_cast<std::int32_t>(meleeTargetClass);
                buy.meleeBaseDmg  = baseDmg;
                buy.doRanged      = doRanged;
                buy.wantCrossbow  = wantCrossbow;
                buy.rangedBaseDmg = myRangedDmg;
                buy.buyArmor       = !useMageApparel && !dolls;   // non-caster OR caster-in-armor: rated armor
                // A shield is in-role ONLY for a dedicated one-hand melee follower (not a
                // ranged/caster, even one carrying a 1h backup). For everyone else, saturate
                // the shield slot's baseline so PlanBuy never buys one (matches the sell/keep
                // side's usesShield and the loot path's shieldUseless).
                const bool usesShield = (roles.melee == WepClass::OneHand) && !doRanged && !caster;
                if (!usesShield) slotRat[4] = 1.0e9f;
                for (int s = 0; s < 5; ++s) buy.armorBaseRat[s] = static_cast<std::int32_t>(slotRat[s]);
                buy.buyMageApparel = useMageApparel && !dolls;
                // MEO-aware ranking (marth): value-driven ONLY with MEO present (gems
                // transfer + supply school relevance). Without MEO -- or with the
                // strict toggle -- rank school-enchant first so a mage never trades a
                // helpful enchant for a pricier off-school one. MEOBridge::Available()
                // is a worker-safe bool (no main-thread MEO query on the tick).
                buy.mageSchoolPrimary = !MEOBridge::Available() || Config::g_mageApparelStrictSchool.load();
                buy.isNecromancer  = IsNecromancerFollower(a_state);
                buy.eligibleSchools= top2;

                // Per-slot owned baseline (best owned (tier,metric) per dress-up slot,
                // same ranking as the buy). Best-OWNED (not just worn) so a bought
                // piece raises the bar and is not re-bought next visit; keepArmor
                // protects it from re-sell.
                if (buy.buyMageApparel) {
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* ar = obj->As<RE::TESObjectARMO>();
                        if (!ar || ar->GetArmorRating() > 0.0f) continue;
                        const int slot = MageClothingSlot(ar);
                        if (slot < 0) continue;
                        int tier = 0; std::int32_t metric = 0;
                        // allowVillain=true: an owned piece counts as baseline regardless.
                        if (!MageApparelBuyKey(ar, top2, buy.mageSchoolPrimary, true, tier, metric)) continue;
                        if (tier > buy.mageBaseTier[slot] ||
                            (tier == buy.mageBaseTier[slot] && metric > buy.mageBaseMetric[slot])) {
                            buy.mageBaseTier[slot]   = tier;
                            buy.mageBaseMetric[slot] = metric;
                        }
                    }
                }
            }

            // ── Feature B: tome buy (caster w/ a cast gambit; independent of
            //    bMageWearRobes and g_magicLoadout -- gated on HasCastGambit). ──
            if (Config::g_economyBuyTomes.load() && HasCastGambit(a_state)) {
                buy.buyTomes = true;
                buy.eligibleSchools = top2;
            }
            return buy;
        }

        void EconomyProbe(RE::Actor* a_follower, const FollowerState& a_state,
                          Clock::time_point a_now) {
            const auto& a_logistics = a_state.logistics();
            const auto fid = a_follower->GetFormID();

            // Per-follower SCAN cooldown -- the cell walk itself is the
            // cost being limited here, same pattern as the other diagnostics.
            auto& sn = g_econScan[fid];
            if (sn.time_since_epoch().count() != 0 && a_now < sn) return;
            sn = a_now + std::chrono::seconds(2);   // vendor-scan cadence (was 15s, then 6 -- snappier shopping)

            // Same crash4-safe walk as LootNearby: the follower's OWN attached
            // cell, never TES::ForEachReferenceInRange (§0.30).
            auto* cell = a_follower->GetParentCell();
            if (!cell || !cell->IsAttached()) return;
            const auto  origin      = a_follower->GetPosition();
            const float kLootRadius = Config::g_lootRadius.load();

            // COLLECT-THEN-ACT (#2): handles only inside the walk; the inventory
            // reads happen after it. LIVING actors only -- corpses are loot,
            // not merchants.
            constexpr size_t kMaxVendorCands = 16;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            std::vector<RE::ActorHandle> living;
            living.reserve(kMaxVendorCands);
            cell->ForEachReferenceInRange(origin, kLootRadius,
                [&](RE::TESObjectREFR& a_ref) {
                    if (living.size() >= kMaxVendorCands) return RE::BSContainer::ForEachResult::kStop;
                    auto* actor = a_ref.As<RE::Actor>();
                    if (!actor || actor == a_follower || actor == pc || actor->IsDead() ||
                        actor->IsDisabled() || actor->IsMarkedForDeletion())
                        return RE::BSContainer::ForEachResult::kContinue;
                    // A fellow FOLLOWER is never a merchant. Auri carries a stray
                    // vendor faction (IsVendor && OffersServices both pass), but her
                    // "merchant container" is malformed -- reading it null-derefs a
                    // form cast and CTDs (crash 2026-07-31, thread on the job worker).
                    // Teammates trade through the player, not each other.
                    if (actor->IsPlayerTeammate())
                        return RE::BSContainer::ForEachResult::kContinue;
                    living.push_back(actor->GetHandle());
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            // PHASE 4 anti-thrash + don't-trade-mid-loot.
            // (a) A follower already walking to loot must not break off to trade --
            //     loot and trade never overlap; finish the excursion first.
            if (SlotOf(fid)) return;
            // (b) Per-follower TRADE cooldown: at most one trade dispatch per window,
            //     so a purchase SETTLES (the arrow/potion count updates) before the
            //     next scan re-evaluates the need. Without it a follower near two
            //     vendors traded with BOTH in the same scan and over-bought (field:
            //     Erik +17 @ Ysolda AND +22 @ Adrianne, same second).
            if (auto& tn = g_econTrade[fid]; tn.time_since_epoch().count() != 0 && a_now < tn) return;

            // #21 buy thresholds (weapon/armor/apparel/tome) -- built ONCE on the
            // worker from the follower's own inventory/skills/gambits, reused for
            // every candidate vendor this scan and passed opaque to PlanBuy.
            const TradeBridge::BuyThresholds buy = BuildBuyThresholds(a_follower, a_state);

            // Build A: warm the follower's carried-gem cache on the MAIN thread for
            // the NEXT scan (GetActorGemsCarried is main-thread only; the sell loop
            // below READS the cache, one-scan-stale). No-op when MEO < ABI v2.
            MEOBridge::RequestCarriedGemRefresh(a_follower);

            // #21 SELL-side per-follower params (same for every candidate vendor):
            //  (a) MERCHANT-PERK BYPASS -- a follower holding the "sell anything" perk
            //      (vanilla Merchant / Ordinator Salesman) sells outside the vendor's
            //      buy filter, like the player. Followers hold perks on the base TESNPC,
            //      so HasPerk under-reports -- dual-check GetActorBase()->GetPerkIndex
            //      too (the OwnsExactPerk idiom, ProgAllocator.cpp:506).
            bool sellAnything = false;
            if (Config::g_merchantPerkBypass.load()) {
                if (auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(Config::g_merchantPerkID.load())) {
                    auto* base = a_follower->GetActorBase();
                    sellAnything = (base && base->GetPerkIndex(perk).has_value()) ||
                                   a_follower->HasPerk(perk);
                }
            }
            //  (b) SPEECH-SCALED SELL PRICE -- the vanilla barter curve keyed on the
            //      follower's Speech skill. fBarterMax/fBarterMin are the GMSTs
            //      (hardcoded to their vanilla defaults 3.3 / 2.0; changing the GMST is
            //      out of scope). sellFraction: 0.30 @ speech 0 -> 0.50 @ speech 100.
            //      NOTE: the follower's own kModSellPrices PERK boosts (Haggling/
            //      Salesman ranks) are NOT applied -- see the report; that needs a
            //      CI-verified EPFD read this offline build can't confirm.
            double sellFraction = 1.0;
            if (Config::g_speechPricing.load()) {
                constexpr float kBarterMax = 3.3f, kBarterMin = 2.0f;   // GMST fBarterMax / fBarterMin
                auto* avo = a_follower->AsActorValueOwner();
                const float speech = avo
                    ? std::clamp(avo->GetActorValue(RE::ActorValue::kSpeech), 0.0f, 100.0f) : 0.0f;
                const float priceFactor = kBarterMax - (kBarterMax - kBarterMin) * speech / 100.0f;
                if (priceFactor > 0.0f) sellFraction = 1.0 / static_cast<double>(priceFactor);
            }

            for (auto& h : living) {
                auto  ptr    = h.get();
                auto* vendor = ptr.get();
                if (!vendor) continue;

                // First vendor faction that actually offers services. rank < 0
                // is a runtime REMOVAL (ExtraFactionChanges) -- skip it.
                RE::TESFaction* fac = nullptr;
                vendor->VisitFactions([&](RE::TESFaction* a_fac, std::int8_t a_rank) {
                    if (!a_fac || a_rank < 0) return false;
                    if (a_fac->IsVendor() && a_fac->OffersServices()) { fac = a_fac; return true; }
                    return false;
                });
                if (!fac) continue;

                // Per-(follower,vendor) log cooldown (60 s).
                const auto pairKey = (static_cast<std::uint64_t>(fid) << 32) | vendor->GetFormID();
                auto& pn = g_econPair[pairKey];
                if (pn.time_since_epoch().count() != 0 && a_now < pn) continue;
                pn = a_now + std::chrono::seconds(12);   // per-(follower,vendor) retry cadence (was 60s, then 20)

                const auto& vv   = fac->vendorData.vendorValues;
                auto*       vend = fac->vendorData.vendorSellBuyList;

                // Only a chest that is a real container REFR (has a base
                // TESContainer) is even a candidate -- a malformed merchant link
                // (fake vendor faction) has none. But we NO LONGER read its
                // inventory natively: GetInventory on an unpopulated merchant chest
                // CTDs on any thread (§0.37). The chest read moves to Papyrus.
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;
                // SELL list: the follower's OWN unworn weapons/armour (jewellery is
                // ARMO, so IsJewelryPiece rides along) that clear the never-loot
                // catalog and the vendor's VEND filter -- all native-SAFE reads. Also
                // count the PURSE in the same pass: Actor::GetGoldAmount() CTDs here
                // (Actor.cpp:445, null-deref on the InventoryChanges the worker tick
                // may be mutating -- crash 2026-08-01), but the GetInventory snapshot
                // is safe, so sum Gold001 (0x0000000F) straight from it.
                // KEEP THE LOADOUT: a follower who fights with a bow AND a melee weapon
                // only ever has ONE worn at a time, so the sheathed other reads unworn
                // and was SOLD (marth). Protect the best weapon of EACH weapon CLASS --
                // 1H, 2H, bow, crossbow, staff kept SEPARATELY (Fable: merging 1H+2H or
                // bow+crossbow by raw damage let a junk greatsword/crossbow win the keep
                // and the real weapon get sold). Only worse in-class duplicates are junk.
                std::unordered_set<RE::TESBoundObject*> keepWeapons;
                {
                    // bucket: 1=1H 2=2H 3=bow 4=crossbow 5=staff; -1 = don't protect.
                    auto bucketOf = [](RE::WEAPON_TYPE wt) -> int {
                        using WT = RE::WEAPON_TYPE;
                        switch (wt) {
                            case WT::kBow:          return 3;
                            case WT::kCrossbow:     return 4;
                            case WT::kStaff:        return 5;
                            case WT::kTwoHandSword:
                            case WT::kTwoHandAxe:   return 2;
                            case WT::kHandToHandMelee: return -1;   // never protect fists
                            default:                return 1;      // 1h sword/dagger/axe/mace
                        }
                    };
                    std::unordered_map<int, std::pair<RE::TESBoundObject*, std::uint16_t>> best;
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* w = obj->As<RE::TESObjectWEAP>();
                        if (!w || IsCreatureWeapon(w)) continue;
                        const int b = bucketOf(w->GetWeaponType());
                        if (b < 0) continue;
                        auto& slot = best[b];
                        if (!slot.first || w->GetAttackDamage() >= slot.second)
                            slot = { obj, w->GetAttackDamage() };
                    }
                    for (auto& [b, s] : best) if (s.first) keepWeapons.insert(s.first);
                }

                // #21 KEEP-ARMOR: protect what the follower WEARS + its single best
                // next-upgrade PER LOGICAL SLOT from being sold, so a just-BOUGHT
                // upgrade is not re-sold as junk (keepWeapons does this for weapons) --
                // but EVERYTHING ELSE (extra/old clothing, spare armor) stays sellable.
                // BUG FIXED: the old version bucketed by the RAW GetSlotMask() bitmask,
                // so two same-logical-slot robes with different modded slot-bit combos
                // (plain robe = body; "Blue Mage Robes" = head+body) each survived as
                // "best in its own bucket" -> a mage kept ALL his clothing and sold
                // none. Now: bucket by LOGICAL slot (MageClothingSlot for clothing/
                // jewelry, primary biped slot for rated armor), keep ONE best per slot,
                // ranked to MATCH what EquipBestOwnedGear would actually wear.
                std::unordered_set<RE::TESBoundObject*> keepArmor;
                std::unordered_map<int, RE::TESBoundObject*> bestBySlot;   // logical-slot key -> best obj (worn-inferior convergence)
                if (Config::g_economyBuyGear.load()) {
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    const bool caster         = IsCasterFollower(a_state);
                    const bool useMageApparel = caster && Config::g_mageWearRobes.load() &&
                                                !Config::g_dollsMode.load();
                    const std::uint8_t top2   = useMageApparel ? TopTwoSchoolMask(a_follower) : 0;
                    const bool schoolPrimary  = !MEOBridge::Available() ||
                                                Config::g_mageApparelStrictSchool.load();
                    const bool allowVillain   = useMageApparel && IsNecromancerFollower(a_state);
                    // A shield is in-role ONLY for a dedicated one-hand MELEE follower. A
                    // ranged (bow) or caster follower never equips one, so it must NOT be
                    // kept -- it is dead weight that should sell. (The off-role WEAPON shed
                    // drops wrong-role weapons; a shield is armor and slipped past it.)
                    const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);
                    const bool usesShield   = (roles.melee == WepClass::OneHand) &&
                                              !roles.doRanged && !caster;

                    // (a) Always keep what is WORN, regardless of the bucket math (the
                    //     sell loop's IsWorn gate already bars worn gear; this is belt-
                    //     and-suspenders and also pins the jewelry logical slots).
                    for (int ls = 0; ls < 6; ++ls)
                        if (auto* w = WornInLogicalSlot(a_follower, ls)) keepArmor.insert(w);
                    for (auto sl : { Slot::kForearms, Slot::kCalves })
                        if (auto* w = a_follower->GetWornArmor(sl)) keepArmor.insert(w);
                    if (usesShield)
                        if (auto* w = a_follower->GetWornArmor(Slot::kShield)) keepArmor.insert(w);

                    // (b) The single best NEXT-UPGRADE per LOGICAL slot.
                    auto armorLogicalSlot = [](std::uint32_t mask) -> int {
                        if (mask & static_cast<std::uint32_t>(Slot::kBody))   return 1;
                        if (mask & static_cast<std::uint32_t>(Slot::kHead))   return 0;
                        if (mask & static_cast<std::uint32_t>(Slot::kHands))  return 2;
                        if (mask & static_cast<std::uint32_t>(Slot::kFeet))   return 3;
                        if (mask & static_cast<std::uint32_t>(Slot::kShield)) return 4;
                        return -1;
                    };
                    struct Best { RE::TESBoundObject* obj = nullptr; float primary = -1e9f; float secondary = -1e9f; };
                    std::unordered_map<int, Best> best;   // key: clothing 0..5, rated armor 10..14
                    for (auto& [obj, data] : a_follower->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* ar = obj->As<RE::TESObjectARMO>();
                        if (!ar) continue;
                        int key = -1; float primary = 0.0f, secondary = 0.0f;
                        if (useMageApparel) {
                            // A mage's body/clothing slot holds ONE item -- a robe (rating 0)
                            // OR a rated-armor outfit both compete for the SAME biped slot, so
                            // bucket BOTH by MageClothingSlot (one bucket per slot) instead of
                            // splitting rated-vs-clothing (which let two body pieces both survive).
                            // Rated armor competes at plain tier by BASE value (no gem), so a
                            // pricier outfit beats a cheaper robe and only one survives
                            // (marth: Khajiit ~800 must beat Nord ~100, not bucket apart).
                            const int cs = MageClothingSlot(ar);
                            if (cs < 0) continue;   // shields/other -> a mage doesn't wear them, sells
                            int t = 0; std::int32_t m = 0;
                            if (ar->GetArmorRating() > 0.0f) {
                                t = 0; m = std::max<std::int32_t>(ar->GetGoldValue(), 0);
                            } else if (!MageApparelBuyKey(ar, top2, schoolPrimary, allowVillain, t, m)) {
                                continue;
                            }
                            key = cs; primary = static_cast<float>(t); secondary = static_cast<float>(m);
                        } else if (ar->GetArmorRating() > 0.0f) {                // non-mage: rated armor by biped slot
                            const int ls = armorLogicalSlot(static_cast<std::uint32_t>(ar->GetSlotMask()));
                            if (ls < 0) continue;
                            if (ls == 4 && !usesShield) continue;   // don't keep a shield for a non-shield-user -> it sells
                            key = 10 + ls;
                            primary   = ar->GetArmorRating();
                            secondary = static_cast<float>(std::max<std::int32_t>(ar->GetGoldValue(), 0));
                        } else {
                            continue;   // a non-mage's clothing/jewelry is sellable junk (nothing wears it)
                        }
                        auto& b = best[key];
                        if (!b.obj || primary > b.primary ||
                            (primary == b.primary && secondary > b.secondary))
                            b = { obj, primary, secondary };
                    }
                    for (auto& [k, b] : best) if (b.obj) { keepArmor.insert(b.obj); bestBySlot[k] = b.obj; }
                }

                std::vector<TradeBridge::SellRow> sell;
                int purse = 0;
                // GEM HANDLING (marth: NEVER HOARD -- ungem-then-sell where possible,
                // sell-as-is otherwise; only the shipped/OFF behavior protects). A
                // gemmed junk item must still SELL. GetActorGemsCarried (MEO v2) scans
                // the WHOLE inventory for real socketed gems; the (base,uid)->slots set
                // is cached per follower (main-thread refresh above), read here.
                //   * MEO v3 (unsocket): a WARM-cache gemmed item -> queue UnsocketGem
                //     for each slot (UnsocketItemGems, de-duped) and DON'T sell it this
                //     scan; it sells once the async unsocket lands + the cache refreshes
                //     (it drops out of the gemmed set). Gems accumulate as loose gems.
                //     Unconditional -- already never hoards, toggle doesn't change it.
                //   * MEO v2 only (detect, no unsocket -- can't extract): bLootSpecialItems
                //     ON (default) -- SELL it AS-IS, gems included (never hoard; v2 has
                //     no extraction path so this is the only way it ever sells). OFF --
                //     PROTECT (shipped behavior): hold the gemmed item, don't sell.
                //   * COLD cache (not yet warmed, v2/v3): conservative -- don't sell
                //     ANY nonzero-uid instance this scan (can't know slots yet); brief,
                //     never permanent, unaffected by the toggle. Once warm: v3 extracts,
                //     v2 sells-as-is (ON) or protects (OFF).
                //   * MEO < v2 / absent: NO gem handling (worn + keepWeapons/keepArmor
                //     still protect worn gems); the bare-uid over-block never returns.
                // MULTI-INSTANCE: GetInventory aggregates a base's instances into one
                // entry with multiple extraLists -- check EVERY uid.
                const bool gemSupported = MEOBridge::CarriedGemsSupported();
                const bool gemWarmed    = gemSupported && MEOBridge::CacheWarmed(fid);
                const bool gemUnsocket  = MEOBridge::CarriedGemUnsocketSupported();   // v3
                const bool sellSocketed = Config::g_lootSpecialItems.load();   // v2-only: sell as-is instead of hoarding
                // Returns true = DON'T sell this scan; side-effect (v3 warm) = queue
                // the ungem so it can sell later.
                auto gemHold = [&](RE::InventoryEntryData* e, RE::FormID base) -> bool {
                    if (!gemSupported || !e || !e->extraLists) return false;
                    bool hold = false;
                    for (auto* xl : *e->extraLists) {
                        auto* uid = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (!uid || uid->uniqueID == 0) continue;
                        if (!gemWarmed) { hold = true; continue; }   // cold: protect, can't extract yet
                        if (MEOBridge::IsCarriedGemmed(fid, base, uid->uniqueID)) {
                            if (gemUnsocket) {   // v3: extract this instance's gems -> sells ungemmed later
                                hold = true;
                                MEOBridge::UnsocketItemGems(a_follower, base, uid->uniqueID);
                            } else if (!sellSocketed) {
                                hold = true;   // v2-only, toggle OFF: protect (shipped behavior)
                            }
                            // v2-only, toggle ON: no extraction path -- fall through and
                            // sell it as-is (gems included) rather than hoard it forever.
                        }
                    }
                    return hold;
                };

                // [sell] per-item EXCLUSION diagnostic. Rate-limited ~15s/follower via a
                // worker-sequential static (one ServiceFollower at a time -- same pattern
                // as s_nextMageFix/s_nextHeal, no lock). Names WHY each weap/armo is not
                // offered, so a "sell n=0" is never a black box again. INFO-level, cheap.
                static std::unordered_map<RE::FormID, Clock::time_point> s_nextSellDiag;
                bool diag = false;
                {
                    const auto now = Clock::now();
                    auto& nx = s_nextSellDiag[fid];
                    if (now >= nx) { diag = true; nx = now + std::chrono::seconds(15); }
                }
                auto sdiag = [&](RE::TESBoundObject* o, const char* why) {
                    if (diag) spdlog::info("[sell] {:08X} '{}' -> {}", fid,
                                           o && o->GetName() ? o->GetName() : "?", why);
                };
                if (diag)
                    spdlog::info("[sell] {:08X} scanning (sellAnything={})", fid, sellAnything);

                const bool umaSell = IsCasterFollower(a_state) && Config::g_mageWearRobes.load() &&
                                     !Config::g_dollsMode.load();

                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    if (obj->GetFormID() == 0x0000000F) { purse += static_cast<int>(data.first); continue; }
                    auto* weap = obj->As<RE::TESObjectWEAP>();
                    auto* armo = obj->As<RE::TESObjectARMO>();
                    if (!weap && !armo) continue;
                    if (IsStockGear(fid, obj->GetFormID())) { sdiag(obj, "stock"); continue; }        // #69 own signature gear
                    // CONVERGENCE (marth): a WORN piece that is NOT its slot's best (a strictly
                    // kept-better exists) is a redundant inferior. The engine keeps re-applying
                    // it from the follower's DEFAULT OUTFIT, so unequip-and-wait never wins the
                    // race. Instead flag it and SELL it even while worn -- it bypasses the
                    // keepArmor/IsWorn gates below, the gem (if any) is extracted first, and the
                    // trade's RemoveItem unequips it on sale. Once it's gone the engine has
                    // nothing to put back (Jesper's Nord Tribal outfit, Auri's spare boots).
                    bool redundantInferior = false;
                    if (armo && data.second && data.second->IsWorn() && !bestBySlot.empty()) {
                        int sk = -1;
                        if (umaSell)                            { sk = MageClothingSlot(armo); }   // mage: robes + rated armor share one body bucket
                        else if (armo->GetArmorRating() > 0.0f) { const int ls = ArmorBuySlot(armo); if (ls >= 0) sk = 10 + ls; }
                        if (sk >= 0) {
                            auto bit = bestBySlot.find(sk);
                            if (bit != bestBySlot.end() && bit->second && bit->second != obj)
                                redundantInferior = true;
                        }
                    }
                    // Force-sell also covers BLACKLISTED apparel (marth's annoyance list) --
                    // never keep/wear it; sell it even while worn (RemoveItem unequips it).
                    const bool forceSell = redundantInferior || (armo && IsBlacklistedApparel(armo));
                    if (weap && keepWeapons.count(obj))     { sdiag(obj, "keepWeap"); continue; }     // loadout weapon, not junk
                    if (armo && keepArmor.count(obj) && !forceSell) { sdiag(obj, "keepArmor"); continue; }   // #21 best-in-slot (a worn redundant/blacklisted piece bypasses -> sells)
                    RE::BGSKeywordForm* kwf = weap
                        ? static_cast<RE::BGSKeywordForm*>(weap)
                        : static_cast<RE::BGSKeywordForm*>(armo);
                    auto* entry = data.second.get();
                    if (forceSell)                              sdiag(obj, "force-sell");           // redundant/blacklisted worn -> sell (RemoveItem unequips it)
                    else if (entry && entry->IsWorn())          { sdiag(obj, "worn"); continue; }     // never sell worn gear
                    if (gemHold(entry, obj->GetFormID()))       { sdiag(obj, "gemHold"); continue; }  // ungem-then-sell (v3) / protect (v2)
                    if (Catalog::IsExcluded(obj->GetFormID()))  { sdiag(obj, "excluded"); continue; } // #3 artifacts/quest
                    // #21 merchant-perk bypass: a "sell anything" follower ignores the
                    // vendor's VEND filter (like the player); otherwise the filter holds.
                    if (!sellAnything && !VendorTrades(vend, vv.notBuySell, kwf)) { sdiag(obj, "vendor-filter"); continue; }
                    if (diag) sdiag(obj, "SELL");
                    // #21 speech-scaled sell price (base instance value * sellFraction).
                    const std::int32_t baseVal = entry ? entry->GetValue() : 0;
                    sell.push_back(TradeBridge::SellRow{
                        obj, static_cast<std::int32_t>(data.first),
                        static_cast<std::int32_t>(std::lround(baseVal * sellFraction)),
                        armo && IsJewelryPiece(armo) });
                }
                // Highest-value first: the vendor's barter gold is limited (field log:
                // sale total often > vendor gold), so sell the most valuable junk first
                // to convert the most worth per visit. Papyrus caps at the chest's gold.
                std::sort(sell.begin(), sell.end(),
                          [](const auto& a, const auto& b) { return a.value > b.value; });

                // BUY NEEDS: each supply gambit BELOW its threshold -> the category
                // + how many MORE to acquire. Papyrus enumerates the vendor's ACTUAL
                // stock and TradeBridge::PlanBuy matches it to these (best affordable
                // first, capped at quota + purse). Native can't pre-name the stock --
                // the field log proved guessing gives stock=0 -- so it names the
                // NEED and lets the enumeration find the match.
                std::vector<TradeBridge::NeedCat> needs;
                auto addNeed = [&](TradeBridge::NeedCat::Kind a_kind, int have, int want) {
                    if (have < want) needs.push_back({ static_cast<std::int32_t>(a_kind),
                                                       static_cast<std::int32_t>(want - have) });
                };
                for (const auto& g : a_logistics) {
                    if (!g.enabled) continue;
                    const auto& c    = g.conditionOpcode;
                    const int   want = static_cast<int>(g.conditionParam);
                    if      (c == Vocab::kCondSelfLowHealthPotion)  addNeed(TradeBridge::NeedCat::kPotHealth,  CountPotions(a_follower, RE::ActorValue::kHealth),  want);
                    else if (c == Vocab::kCondSelfLowStaminaPotion) addNeed(TradeBridge::NeedCat::kPotStamina, CountPotions(a_follower, RE::ActorValue::kStamina), want);
                    else if (c == Vocab::kCondSelfLowMagickaPotion) addNeed(TradeBridge::NeedCat::kPotMagicka, CountPotions(a_follower, RE::ActorValue::kMagicka), want);
                    else if (c == Vocab::kCondSelfOutOfArrows)      addNeed(TradeBridge::NeedCat::kArrows,     ArrowCount(a_follower),                             want);
                    else if (c == Vocab::kCondSelfOutOfBolts)       addNeed(TradeBridge::NeedCat::kBolts,      BoltCount(a_follower),                              want);
                }

                // Only burn the cooldown + stop scanning if a trade ACTUALLY
                // dispatched (Fable audit #8): a chest already busy with another
                // follower's order, or the bridge being down, must not cost this
                // follower its 8 s window -- try the next vendor / next scan.
                if (TradeBridge::VendorTrade(a_follower, vendor, chest,
                                             std::move(sell), std::move(needs), purse, buy)) {
                    g_econTrade[fid] = a_now + std::chrono::seconds(8);
                    break;
                }
            }   // for (living vendors)
        }
    // ── #21 economy GEAR/TOME buy helpers (see Logistics.h) ─────────────────────
    // Thin wrappers over the anon-namespace loot judge, callable from the VM-side
    // buy planner (TradeBridge::PlanBuy) so buy and loot classify gear identically.
    int WeaponBuyClass(RE::WEAPON_TYPE a_type) {
        return static_cast<int>(WeaponClassOf(a_type));   // WepClass: 0=1H 1=2H 2=Ranged 3=Other
    }

    int SpellSchoolBit(RE::SpellItem* a_spell) {
        if (!a_spell) return -1;
        const auto* eff  = a_spell->GetCostliestEffectItem();
        const auto* mgef = eff ? eff->baseEffect : nullptr;
        if (!mgef) return -1;
        // SAME 5-school order TargetMagicSchool tallies in (its kSchools array):
        // 0 Alteration, 1 Conjuration, 2 Destruction, 3 Illusion, 4 Restoration.
        // Read from the MGEF "Magic Skill" (associatedSkill), the school field the
        // spell side already trusts (TargetMagicSchool).
        switch (mgef->data.associatedSkill) {
        case RE::ActorValue::kAlteration:  return 0;
        case RE::ActorValue::kConjuration: return 1;
        case RE::ActorValue::kDestruction: return 2;
        case RE::ActorValue::kIllusion:    return 3;
        case RE::ActorValue::kRestoration: return 4;
        default:                            return -1;
        }
    }

    // ── VILLAIN-CODED APPAREL (mage buy path only) ──────────────────────────────
    // The modded list's priciest clothing is necromancer / black-mage regalia, so a
    // value-driven buy would make every mage follower converge on evil robes (marth).
    // A mage NEVER buys a villain-coded robe unless the follower is itself a
    // necromancer (detected PRINCIPLED, by a Reanimate cast gambit -- see
    // EconomyProbe/isNecromancer, never by name). Match is case-insensitive over the
    // EditorID OR the full (display) name. EDIT THIS ONE LIST to extend the coding.
    static bool IsVillainCodedApparel(RE::TESObjectARMO* a_armo) {
        static constexpr const char* kVillainWords[] = {
            "necromancer", "black mage", "blackmage", "black robe",
        };
        if (!a_armo) return false;
        const char* ed   = a_armo->GetFormEditorID();
        const char* name = a_armo->GetFullName();
        for (const char* w : kVillainWords)
            if (ContainsNoCase(ed, w) || ContainsNoCase(name, w)) return true;
        return false;
    }

    // Which SCHOOL, if any, this apparel/jewelry piece's enchantment fortifies
    // (first beneficial school-boost effect, else a school-tagged keyword), or
    // kNone for plain / non-school pieces. Reuses EffectBoostSchool/SchoolFromKeywords
    // -- the same enchant reader the loot judge trusts. For the strict-school filter.
    static RE::ActorValue ApparelFortifiedSchool(RE::TESObjectARMO* a_armo) {
        if (!a_armo) return RE::ActorValue::kNone;
        if (const auto* ench = a_armo->formEnchanting) {
            for (const auto* e : ench->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef) continue;
                using Flag = RE::EffectSetting::EffectSettingData::Flag;
                if (mgef->data.flags.any(Flag::kDetrimental) ||
                    mgef->data.flags.any(Flag::kHostile)) continue;   // a curse is not a fortify
                if (auto s = EffectBoostSchool(mgef); s != RE::ActorValue::kNone) return s;
            }
        }
        return SchoolFromKeywords(a_armo);
    }

    // The LOGICAL mage-apparel slot a candidate occupies, or -1 if it is not a
    // dress-up slot: 0 head (hat/hood/circlet), 1 body (robe), 2 hands (gloves),
    // 3 feet (shoes/boots), 4 ring, 5 amulet. A mage treats jewelry as apparel
    // (marth), and a circlet counts as head whether the catalog flags it clothing
    // or jewelry. Body/hands/feet win over the head bits an item may also carry.
    int MageClothingSlot(RE::TESObjectARMO* a_armo) {
        if (!a_armo) return -1;
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
        auto has = [&](Slot s) { return (mask & static_cast<std::uint32_t>(s)) != 0; };
        if (has(Slot::kBody))  return 1;
        if (has(Slot::kHands)) return 2;
        if (has(Slot::kFeet))  return 3;
        if (has(Slot::kRing))  return 4;
        if (has(Slot::kAmulet)) return 5;
        if (has(Slot::kHead) || has(Slot::kHair) || has(Slot::kCirclet)) return 0;
        return -1;
    }

    int ArmorBuySlot(RE::TESObjectARMO* a_armo) {
        if (!a_armo) return -1;
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask());
        auto has = [&](Slot s) { return (mask & static_cast<std::uint32_t>(s)) != 0; };
        if (has(Slot::kBody))   return 1;
        if (has(Slot::kHead))   return 0;
        if (has(Slot::kHands))  return 2;
        if (has(Slot::kFeet))   return 3;
        if (has(Slot::kShield)) return 4;
        return -1;
    }

    bool MageApparelBuyKey(RE::TESObjectARMO* a_armo, std::uint8_t a_top2Mask,
                           bool a_schoolPrimary, bool a_allowVillain,
                           int& out_tier, std::int32_t& out_metric) {
        out_tier = 0; out_metric = 0;
        if (!a_armo) return false;
        if (IsBlacklistedApparel(a_armo)) return false;   // never rank blacklisted apparel as wearable
        if (a_armo->GetArmorRating() > 0.0f) return false;   // rated armor is kArmor -- clothing/jewelry are rating 0
        if (!a_allowVillain && IsVillainCodedApparel(a_armo)) return false;   // no evil regalia unless the follower is a necromancer
        const std::int32_t value = std::max<std::int32_t>(a_armo->GetGoldValue(), 0);

        // VALUE-PRIMARY (MEO present + not strict): pure gold value, one flat tier.
        if (!a_schoolPrimary) { out_tier = 0; out_metric = value; return true; }

        // SCHOOL-PRIMARY (MEO absent OR strict): tier 2 = fortifies a top-2 school
        // (ranked by score then fanciness within), tier 1 = plain (no school fortify),
        // tier 0 = off-school enchant. So a cheap school robe beats a pricey wrong-
        // school one, and a bare slot still fills with plain clothing.
        static constexpr RE::ActorValue kBySchoolBit[5] = {
            RE::ActorValue::kAlteration, RE::ActorValue::kConjuration,
            RE::ActorValue::kDestruction, RE::ActorValue::kIllusion,
            RE::ActorValue::kRestoration,
        };
        bool matched = false; int bestScore = 0; float bestMag = 0.0f;
        for (int bit = 0; bit < 5; ++bit) {
            if (!((a_top2Mask >> bit) & 1)) continue;
            float mag = 0.0f;
            const int sc = SchoolMatchScore(a_armo, kBySchoolBit[bit], &mag);
            if (sc > 0 && (!matched || sc > bestScore || (sc == bestScore && mag > bestMag))) {
                matched = true; bestScore = sc; bestMag = mag;
            }
        }
        if (matched) {
            out_tier = 2;
            out_metric = value + static_cast<std::int32_t>(bestMag);
            return true;
        }
        out_tier   = (ApparelFortifiedSchool(a_armo) == RE::ActorValue::kNone) ? 1 : 0;  // plain > off-school
        out_metric = value;
        return true;
    }
}
