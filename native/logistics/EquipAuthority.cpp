// logistics/EquipAuthority.cpp -- wear the best OWNED gear, and the APMF EQUIP
// AUTHORITY declaration (scope + worn set, player picks, bound weapons).
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // feat/mfo-equip-authority: the ch.17 claim + SetEquipSet declaration
#include "Lotd.h"               // HoldFromSale: a museum-needed relic is never MFO's armor choice
#include "PlayerGiven.h"        // ... unless the PLAYER gave it / put it on him
#include "progression/ProgAllocator.h"   // ReapplyPending: defer the FIRST declaration until his perks are back
#include <algorithm>     // std::any_of / std::sort in the declaration builder
#include <unordered_set> // g_playerPicks -- the player-dressed pieces per follower

namespace MFO::Logistics {

        // ── #21 PART 2A: wear the best OWNED gear (makes a purchase functional) ──
        // A buy (RunTrade) only TRANSFERS goods into the follower's pack; nothing
        // wears them. This worker-tick pass equips the single best owned upgrade the
        // follower isn't wearing yet -- bought, looted-as-valuable (jewelry), or
        // player-handed -- through the SAME safe AcquireEquip step the loot judge
        // uses (MainThread::Post EquipObject, MEO gem carry, never DoReset3D). ONE
        // item per tick and idempotent (equips only when it beats what's worn), so
        // once worn it no-ops -> no thrash. Apparel/armor here; a better owned PRIMARY
        // weapon is routed through the same step below (EquipBestOwnedWeapon, MFO-B28), the
        // rest of the weapon choice is left to the combat equip gambit.
        namespace {
            // F10 (Fable round 2): the judged armor pick is computed ONCE per
            // logistics tick. EquipBestOwnedGear hands its pick here (0 = it ran
            // and found nothing to wear) and ServiceFollower's exit guard
            // (RefreshEquipDeclaration below), on the same worker call, consumes
            // it instead of walking the inventory and scoring every piece a
            // second time. Keyed by follower so a slot left over from a tick that
            // never reached the guard (it always does -- the guard is on that
            // call's stack -- but belt and braces) can never be read for a
            // different follower. Worker-serial like g_lastDeclared.
            struct HandedPick { RE::FormID follower = 0; RE::FormID pick = 0; bool set = false; };
            HandedPick g_handedPick;

            // MUSEUM-NEEDED ITEMS TAKE PRIORITY OVER EQUIPMENT (marth 2026-09-28): a
            // relic this follower covers for the museum (Lotd::HoldFromSale, the same
            // "never sell" test the economy and cast/Equip.cpp IsMuseumRelic read) is
            // never MFO's armor choice -- not the judged pick, not a declared worn
            // piece, not a "player put on" record -- unless the PLAYER gave it to him or
            // put it on him (PlayerGiven, SAVED 'PGIV'): the player's choice stands, as
            // in IsMuseumRelic. With Armor always an OWNED category, leaving it out of
            // the declared set is what makes APMF's seat refuse its re-equip (the
            // engine's OutfitApply, SPID, MEO External), in and out of combat; a worn
            // one ships through the deposit (Lotd.cpp WornRelicShippable), which
            // unequips it at the crate. ONLY while the relic can actually be shipped
            // (review SEV-4 on f2c8e2b: otherwise a relic is dead weight he may neither
            // wear nor deposit): Lotd::KeepForDeposit = HoldFromSale AND an enabled
            // act.loot_museum rule AND the deposit can run (Harbinger v17). Worker.
            bool RelicOffBody(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_obj) {
                return a_follower && a_obj && Lotd::KeepForDeposit(a_follower, a_state, a_obj) &&
                       !PlayerGiven::IsPlayerGiven(a_follower->GetFormID(), a_obj->GetFormID());
            }

            // THE MAGE JUDGE'S PER-SLOT BEST, factored out of ComputeOwnedGearPick's mage
            // branch VERBATIM (2026-09-28, Jesper's set 6 / set 7 flip) so the APMF
            // declaration can declare the WHOLE set while the legacy single pick stays as
            // it was. Per logical slot (MageClothingSlot: 0 head / circlet, 1 body, 2
            // hands, 3 feet, 4 ring, 5 amulet -- jewelry included), the best OWNED piece by
            // MageApparelBuyKey (rating-0 clothing and jewelry only: rated armor is not a
            // candidate; tier, then the value metric -- TESForm::GetGoldValue, the engine's
            // InventoryEntryData value of the base form, its own enchantment included --
            // FormID tiebreak so two equal pieces never flip). Worker.
            struct MageSlotBest { RE::TESBoundObject* obj = nullptr; int tier = -1; std::int32_t metric = -1; };
            void MageBestPerSlot(RE::Actor* a_follower, const FollowerState& a_state, MageSlotBest (&a_out)[6]) {
                const std::uint8_t top2 = TopTwoSchoolMask(a_follower);
                const bool schoolPrimary = Config::g_mageApparelStrictSchool.load();
                const bool allowVillain  = IsNecromancerFollower(a_state);
                const bool wearsHelmets  = WearsHelmets(&a_state);
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || ArmorWontRender(a_follower, ar)) continue;   // wont-render armor is never worn (sells)
                    if (RelicOffBody(a_follower, a_state, obj)) continue;   // museum relic: never the choice
                    if (!wearsHelmets && IsHelmetPiece(ar)) continue;      // 86e3phy41 "Wear helmets" OFF: never the choice
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
                    auto& b = a_out[slot];
                    if (!b.obj || t > b.tier || (t == b.tier && m > b.metric) ||
                        (t == b.tier && m == b.metric && obj->GetFormID() > b.obj->GetFormID()))
                        b = { obj, t, m };
                }
            }
        }

        // THE PICK, factored out of EquipBestOwnedGear VERBATIM (feat/mfo-equip-
        // authority, 2026-09-15) so the APMF equip-authority DECLARATION below
        // (RefreshEquipDeclaration) names exactly the piece this pass would
        // wear -- one judge, not two that can drift. Returns the single best
        // owned upgrade the follower is not wearing yet (nullptr = nothing to
        // wear), the ArmorPref the rated branch scored with (for the [equip]
        // line) and whether the mage-apparel branch decided. Worker; pure
        // inventory/form reads plus LogArmorClassIfChanged's change-only line.
        RE::TESBoundObject* ComputeOwnedGearPick(RE::Actor* a_follower, const FollowerState& a_state,
                                                 ArmorPref& a_outPref, bool& a_outMageMode) {
            a_outMageMode = false;
            if (!a_follower || Config::g_dollsMode.load()) return nullptr;
            const bool caster         = UsesMageClothing(a_state, a_follower);
            const bool useMageApparel = caster && Config::g_mageWearRobes.load();
            a_outMageMode = useMageApparel;

            RE::TESBoundObject* pick = nullptr;
            ArmorPref&          armorPref = a_outPref;   // filled by the rated branch (the [equip] line reads it)
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
                MageSlotBest bestSlot[6];
                MageBestPerSlot(a_follower, a_state, bestSlot);
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
                // RATED ARMOR: best owned piece that upgrades a worn/bare slot, by
                // ArmorScore (rating x the follower's class/perk bias -- Logistics_
                // internal.h). THIS is the wear decision that displaces a worn
                // off-class piece: the engine's own auto-equip is skill-blind
                // (highest rating per slot) and MFO never un-wears, so an owned
                // light cuirass (26 x 2.0 = 52) must out-score the worn heavy one
                // (31 x 1.0) HERE or the follower stays in heavy forever (field,
                // 2026-09-14). Equipping the pick auto-unequips the worn piece.
                armorPref = ArmorPrefFor(a_follower);
                LogArmorClassIfChanged(a_follower, armorPref);   // one [armor] line per {class, worn-set} change
                float bestScore = 0.0f;
                const bool wearsHelmets = WearsHelmets(&a_state);
                for (auto& [obj, data] : a_follower->GetInventory()) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || IsCreatureArmor(ar) || ArmorWontRender(a_follower, ar)) continue;
                    if (RelicOffBody(a_follower, a_state, obj)) continue;   // museum relic: never the choice
                    if (!wearsHelmets && IsHelmetPiece(ar)) continue;      // 86e3phy41 "Wear helmets" OFF: never the choice
                    // #3 NO IsExcluded skip here: a follower MAY wear an artifact it
                    // already owns (a legit upgrade) -- equip stays permissive, matching
                    // the mage-clothing branch above. Looting/selling/shedding an
                    // artifact is still barred by IsExcluded on those paths.
                    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                    if ((static_cast<std::uint32_t>(ar->GetSlotMask()) &
                         static_cast<std::uint32_t>(Slot::kShield)) != 0) continue;   // shields: leave to the loot role logic
                    if (!ArmorIsBetter(armorPref, a_follower, ar)) continue;   // strictly higher score than worn on its slot
                    const float sc = ArmorScore(armorPref, ar);
                    if (sc > bestScore) { bestScore = sc; pick = obj; }
                }
            }

            if (!pick || !FitsCarryWeight(a_follower, pick->GetWeight())) return nullptr;
            return pick;
        }

        // OWNED PRIMARY WEAPON UPGRADE (MFO-B28 / B32). A weapon the follower OWNS (bought, handed, looted loose
        // or as a stock) that out-scores the one in his right hand, in the SAME melee class, goes through the one
        // acquire step with a_src == nullptr and forceStock false: AcquireEquip captures the replaced weapon's
        // MEO gems and queues the move, exactly as the loot path does for a primary upgrade (LootEquipment.cpp,
        // the `best == bestBackup || trueSecond || silverStock` split). Nothing else is routed here: the mage
        // backup, a dual wielder's true second one-hander and the silver slot stay stock-only on the loot side
        // and are never an "upgrade over the held weapon", so this branch cannot reach them. No weapon
        // ENFORCEMENT is added: AcquireEquip's own B134 rule still stocks instead of equipping when equip
        // authority is supported but no claim stands, and weapons stay observe-only under the declaration.
        // STAYS OUT of every hold or choice that is not MFO's to override: a standing gambit force-hold in
        // either hand (ForcedHoldFor), the PLAYER in combat (the follower's own IsInCombat flaps and
        // serviceOwnOoc services laps inside a party fight), an open silver episode (its silver weapon in
        // hand, or silver ammo nocked), a bound weapon, and a held weapon the PLAYER chose (given,
        // put on through the menu, or an excluded artifact while bLootSpecialItems is off: player agency).
        // Strictly better only (no flip). A per-follower ~5 s scan cadence sits in front of the inventory
        // walk; an equip AcquireEquip declined (B134 no-claim stock, cross-role) is not retried for the
        // same {held, candidate} pair, so it neither spams the log nor re-walks.
        static void EquipBestOwnedWeapon(RE::Actor* a_follower, const FollowerState& a_state, RE::TESObjectWEAP* a_myWeap) {
            if (!a_follower || !a_myWeap || a_follower->IsInCombat()) return;
            const auto id = a_follower->GetFormID();
            if (auto* pcc = RE::PlayerCharacter::GetSingleton(); pcc && pcc->IsInCombat()) return;
            if (const auto hold = Actuation::ForcedHoldFor(id); hold.first != 0 || hold.second != 0) return;
            if (Actuation::SilverAmmoEpisode(id) != 0 || IsSilverMeleeWeapon(a_myWeap)) return;
            if (a_myWeap->IsBound()) return;
            if (PlayerGiven::IsPlayerGiven(id, a_myWeap->GetFormID()) ||
                PlayerGiven::IsPlayerEquipped(id, a_myWeap->GetFormID()) ||
                (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(a_myWeap->GetFormID()))) return;
            // B134 state: equip authority supported but no claim stands -> AcquireEquip only stocks.
            if (APMFBridge::EquipAuthoritySupported() && !APMFBridge::IsEquipAuthorityClaimed(id)) return;
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextWeapUp;
            static std::unordered_map<RE::FormID, std::pair<RE::FormID, RE::FormID>> s_declined;   // {held, candidate}
            const auto now = Clock::now();
            auto& nxt = s_nextWeapUp[id];
            if (nxt.time_since_epoch().count() != 0 && now < nxt) return;
            nxt = now + std::chrono::seconds(5);
            const EquipmentContext ctx = BuildEquipmentContext(a_follower, &a_state);
            if (ctx.meleeTargetClass == WepClass::Other) return;   // no melee role (pure caster / ranged user)
            if (WeaponClassOf(a_myWeap->GetWeaponType()) != ctx.meleeTargetClass) return;   // never cross-class
            const float heldScore = WeaponScore(ctx.roles, a_myWeap);
            auto* leftEq   = a_follower->GetEquippedObject(true);
            auto* leftForm = leftEq ? leftEq->As<RE::TESObjectWEAP>() : nullptr;   // a dual wielder's other hand
            RE::TESBoundObject* best = nullptr;
            float               bestScore = heldScore;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w == a_myWeap || IsCreatureWeapon(w) || w->IsBound()) continue;
                if (!Config::g_lootSpecialItems.load() && Catalog::IsExcluded(obj->GetFormID())) continue;
                // the combat pick's relic rule (cast/Equip.cpp IsMuseumRelic), so the two agree
                if (Lotd::HoldFromSale(id, w) && !PlayerGiven::IsPlayerEquipped(id, w->GetFormID())) continue;
                if (w == leftForm && data.first < 2) continue;           // his only copy is in the left hand
                if (WeaponClassOf(w->GetWeaponType()) != ctx.meleeTargetClass) continue;
                const float sc = WeaponScore(ctx.roles, w);
                if (sc > bestScore) { bestScore = sc; best = obj; }
            }
            if (!best || !FitsCarryWeight(a_follower, best->GetWeight())) return;
            const std::pair<RE::FormID, RE::FormID> pair{ a_myWeap->GetFormID(), best->GetFormID() };
            if (const auto it = s_declined.find(id); it != s_declined.end() && it->second == pair) return;
            spdlog::info("[equip] {:08X}: OWNED weapon '{}' score={:.1f} <- held '{}' score={:.1f}",
                         id, best->GetName() ? best->GetName() : "?", bestScore,
                         a_myWeap->GetName() ? a_myWeap->GetName() : "?", heldScore);
            if (AcquireEquip(a_follower, best, nullptr, a_myWeap, /*forceStock*/false))   // already owned -> no transfer
                s_declined.erase(id);
            else
                s_declined[id] = pair;   // declined: not retried until the held weapon or the best candidate changes
        }

        void EquipBestOwnedGear(RE::Actor* a_follower, const FollowerState& a_state) {
            if (!a_follower || Config::g_dollsMode.load()) return;
            auto* eqObj  = a_follower->GetEquippedObject(false);
            auto* myWeap = eqObj ? eqObj->As<RE::TESObjectWEAP>() : nullptr;
            ArmorPref armorPref;
            bool      useMageApparel = false;
            RE::TESBoundObject* pick = ComputeOwnedGearPick(a_follower, a_state, armorPref, useMageApparel);
            g_handedPick = { a_follower->GetFormID(), pick ? pick->GetFormID() : 0, true };   // F10: for the declaration guard
            if (!pick) {
                // No armor to wear this tick: one tick, one action, so the owned WEAPON upgrade (MFO-B28/B32)
                // runs only when armor had nothing to do.
                EquipBestOwnedWeapon(a_follower, a_state, myWeap);
                return;
            }
            // APMF EQUIP AUTHORITY (feat/mfo-equip-authority): with the standing
            // claim live, the DECLARATION is the equip path -- this tick's set
            // (RefreshEquipDeclaration, sent from ServiceFollower's tail) names
            // this same pick and APMF equips it. The keep/sell/loot decisions and
            // the MEO gem carry are untouched; only the direct EquipObject hop is
            // skipped, inside AcquireEquip, which says so in its own line.
            const bool declared = EquipAuthorityLive(a_follower->GetFormID());
            if (!useMageApparel) {
                // [equip] DIAGNOSTIC: what goes on, over what, and the score each
                // side got -- the line that proves (or disproves) the class swap.
                auto* na = pick->As<RE::TESObjectARMO>();
                RE::TESObjectARMO* old = nullptr;
                if (const int ls = ArmorBuySlot(na); ls >= 0 && ls <= 3) old = WornInLogicalSlot(a_follower, ls);
                auto typeTag = [](RE::TESObjectARMO* a) -> const char* {
                    using AT = RE::BGSBipedObjectForm::ArmorType;
                    if (!a) return "-";
                    switch (a->GetArmorType()) {
                    case AT::kHeavyArmor: return "Heavy";
                    case AT::kLightArmor: return "Light";
                    default:              return "Clothing";
                    }
                };
                spdlog::info("[equip] {:08X}: OWNED armor '{}' [{}] rat={:.0f} score={:.1f} <- worn '{}' [{}] rat={:.0f} score={:.1f} | class {} bias h/l={:.2f}/{:.2f}{}",
                             a_follower->GetFormID(),
                             na && na->GetFullName() ? na->GetFullName() : "?", typeTag(na),
                             na ? na->GetArmorRating() : 0.0f, na ? ArmorScore(armorPref, na) : 0.0f,
                             old && old->GetFullName() ? old->GetFullName() : "(bare)", typeTag(old),
                             old ? old->GetArmorRating() : 0.0f, old ? ArmorScore(armorPref, old) : 0.0f,
                             armorPref.heavyClass ? "HEAVY" : "LIGHT", armorPref.heavyBias, armorPref.lightBias,
                             declared ? " | left to the APMF equip declaration" : "");
            }
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

        // ── COMBAT WEAPON SEAM (MFO-B19, declared in Logistics.h): thin wrappers over the internal inline
        // WeaponRoles / WeaponScore / RangedPickScore / AmmoOwned so cast/Equip.cpp no longer includes
        // Logistics_internal.h. Same arithmetic, no new behaviour.
        namespace {
            WeaponRoles RolesOf(const CombatWeaponStyle& a_style) {
                WeaponRoles r;
                r.preferKinds = a_style.preferKinds;
                r.offHand     = a_style.offHand;
                return r;
            }
        }

        CombatWeaponStyle CombatStyleFor(RE::Actor* a_follower, const FollowerState& a_state) {
            const WeaponRoles r = ComputeWeaponRoles(a_follower, a_state);
            return { r.preferKinds, r.offHand };
        }

        int CombatAmmo::For(const RE::TESObjectWEAP* a_w) const { return RangedUsesBolts(a_w) ? bolts : arrows; }

        CombatAmmo CombatAmmoOwned(RE::Actor* a_follower) {
            const AmmoOwned a = CountAmmoOwned(a_follower);
            return { a.arrows, a.bolts };
        }

        float CombatWeaponScore(const CombatWeaponStyle& a_style, const RE::TESObjectWEAP* a_weapon) {
            return WeaponScore(RolesOf(a_style), a_weapon);
        }

        float CombatRangedScore(const CombatWeaponStyle& a_style, const RE::TESObjectWEAP* a_weapon,
                                const CombatAmmo& a_ammo) {
            return RangedPickScore(RolesOf(a_style), a_weapon, AmmoOwned{ a_ammo.arrows, a_ammo.bolts });
        }

        // ── APMF EQUIP AUTHORITY: THE DECLARATION (feat/mfo-equip-authority, 2026-09-15) ──
        // Port #1 of an MFO engine mechanism into APMF (ch.17, ABI v7). MFO no
        // longer holds a follower's gear by force-equipping it and hoping the
        // engine leaves it alone; it DECLARES the worn set and APMF's #17a seat
        // refuses every other engine equip on that actor (the AI putting a shield
        // back on a dual-wielder within <1 s -- Fable 2026-09-15 -- outfit re-
        // apply, RemoveItem re-equip). DECLARE -> ENFORCE (CLAUDE.md principle 4):
        // this composes only what MFO already decides elsewhere, never a new
        // opinion.
        // SCOPED (ABI v9, feat/mfo-equip-authority-v9, 2026-09-16): the whole-
        // facet v8 authority froze a follower with NO equip gambit (STATUS F6: the
        // declaration read "the weapon currently in the hand" and then refused
        // the AI's own melee<->ranged switch -- 117 would-denies, none against a
        // hold). Now MFO DECLARES WHAT IT OWNS AND ACTS DIRECTLY IN WHAT IT DOES
        // NOT: the declaration carries a SCOPE (DeclareEquipScope, sent before
        // the set) of owned/denied EquipCategory bits, computed here:
        //   owned  = Armor | Right (a right hold) | Left (a left hold)
        //          | Right+Left+Ammo (a held bow/crossbow) | Right+Left (a held 2H)
        //   denied = Shield iff roles.offHand==2 under bWeaponStyleControl
        // Light never owned; Shield never owned. A hand without a hold is
        // UNOWNED: the seat allows the AI's own equips there (`owned=0`).
        //   * the HANDS: a_holdRight / a_holdLeft (Actuation's ForcedHold ledger,
        //     the source of truth for a gambit equip) and NOTHING ELSE -- the
        //     v8 "else the weapon currently in that hand" fallback and its torch
        //     read are gone (they were the F6 freeze). ABI v8: a one-hand weapon
        //     carries its HAND (kEquipSlot_Right/Left) so APMF places it there
        //     itself; a bow/two-hander is Default. A two-hander/bow in the right
        //     empties the declared left. a_leftReserved (a cast on the left):
        //     nothing left. EVERY road passes the ledger (the OOC service road
        //     through Actuation::ForcedHoldFor, Fable F1 on 7857446): a one-tick
        //     IsInCombat flap must not demote a standing hold to Armor-only;
        //     once ReleaseForcedWeapon erases the ledger the next OOC tick
        //     declares Armor-only on its own.
        //   * BOUND weapons (live BoundItemEffect): declared only into an OWNED
        //     hand (a one-hander needs its hand, a bound bow/2H both).
        //   * AMMO only under a bow/crossbow HOLD (Ammo owned exactly then), of the
        //     matching kind (bolts for a crossbow, arrows otherwise): a player ammo
        //     pick while owned, else a worn special / bound round as is, else the
        //     swap-up rule's best stack by damage (value on a tie) when it holds
        //     kAmmoDeclareMin rounds or is already worn, else the worn stack (rule
        //     2 below). The archer AI's own ammo equips pass in the unowned category.
        //   * the SHIELD: NEVER declared. offHand==2 (dual wield by perks) ->
        //     the category is DENIED (the field fix, now without owning a hand);
        //     offHand==1 -> Actuation's direct EquipShieldOnMain (an unowned-
        //     category equip the seat allows); offHand==0 -> the engine's own.
        //   * the JUDGED ARMOR PICK (OOC road only, a_judgeArmor -- the combat
        //     road declares worn armor as is, F9): exactly ComputeOwnedGearPick's
        //     single best owned upgrade (the piece EquipBestOwnedGear would wear
        //     this tick, handed over from that pass when it ran, F10);
        //     it REPLACES every worn ARMO its slot mask overlaps (what the engine
        //     displaces). One pick per declaration, so a multi-slot upgrade
        //     converges over ticks the same way the direct pass always has, and
        //     the set is simultaneously wearable by construction.
        //   * everything else WORN (jewelry, clothing, circlets, cloaks, modded
        //     slots -- what MFO does not judge) stays declared, so APMF never
        //     strips it. Dolls mode (no judging) declares the worn set as-is.
        // Sent only when it CHANGES ("declare, do not tick" -- APMF_API.h's
        // SetEquipSet doc): the last sent (set, owned, denied) is kept per
        // follower, sorted, and compared; a freshly minted claim (F2) forgets it
        // so the new handle is declared at once. Scope then set, one call. There
        // is deliberately NO forced re-issue (F3) -- but under ENFORCEMENT a
        // declared ARMO that is not worn at the next service IS a change and is
        // re-sent, throttled to kDeclDriftHold (MFO-B63, MFO-B41's shape for
        // armor), and nothing is declared at all while the actor's 3D is absent.
        // Gated on FollowerState::mfoEnabled + APMFBridge::EquipAuthoritySupported();
        // off -> any standing claim is released and the cache dropped, so the
        // direct equip paths run byte-identical to a world with no APMF.
        // THREAD: the worker (#4) -- ServiceFollower's tail (OOC, logistics cadence),
        // Actuation::EquipWeapon (the Scheduler tick, in combat), OnFollowerRemoved.
        // g_lastDeclared is worker-serial like g_nextTick; cleared on revert.
        namespace {
            // The last SENT declaration per follower: the (form, hand) pairs
            // sorted PLUS the v9 scope (owned, denied) -- the change detector. A
            // scope-only change (a new hold owning a hand the set already named)
            // is a declaration event too. Worker-serial like g_nextTick.
            using DeclKey = std::pair<RE::FormID, std::uint8_t>;
            struct SentDecl {
                std::vector<DeclKey> keys;
                std::uint32_t        owned  = 0;
                std::uint32_t        denied = 0;
                // APMF ABI v25: the (form, uid) of every entry that names an item INSTANCE (the chosen
                // ring / amulet copy), sorted, so a changed instance of the same base form re-sends.
                std::vector<std::pair<RE::FormID, std::uint16_t>> uids;
                bool operator==(const SentDecl&) const = default;
            };
            std::unordered_map<RE::FormID, SentDecl> g_lastDeclared;
            // Once-per-deferral latch for the "declaration DEFERRED" line (equip-self-violations); erased when a
            // declaration is actually sent and on forget/revert, so every load logs it again. Worker-serial.
            std::unordered_set<RE::FormID> g_deferLogged;
            // Once per (follower, item) latch for "hold not declared: no longer carried" (key = id<<32 | form).
            // Worker-serial; erased per follower on forget, cleared on revert.
            std::unordered_set<std::uint64_t> g_uncarriedLogged;
            // The worn pieces the LAST build withheld as museum relics (RelicOffBody),
            // per follower, worker-serial like g_lastDeclared (field 0928c, Cicero's
            // Dawnguard Helmet): when the relic verdict flips back, that piece is worn
            // and absent from the last set -- which rule 4b would otherwise read as the
            // PLAYER putting it on. MFO withheld it itself; it is never a player pick.
            std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_lastWithheld;

            // MFO-B63: WHEN the last declaration actually went out, per follower.
            // APMF exposes no "the enforcement pass ran" query -- its pass is a
            // posted hop, and the field caught one posted at 08:23:16 that did not
            // run until 08:24:42 (85 s) -- so "nothing is in flight" is answered by
            // TIME, and by the only clock MFO owns: how long ago it last sent. The
            // hold is APMF's own 3 s per-item re-issue hold, the same constant
            // MFO-B41's weapon detector throttles on (Actuation.cpp kB41Hold), so a
            // piece the engine keeps taking back off costs one re-send per 3 s and
            // never one per service tick (~1 s). Worker-serial like g_lastDeclared.
            std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastDeclSentAt;
            constexpr auto kDeclDriftHold = std::chrono::seconds(3);

            // "Armor+Right+Left" / "none" for the declare line (ABI v9 EquipCategory).
            std::string CategoryNames(std::uint32_t a_mask) {
                std::string out;
                const auto add = [&](std::uint32_t bit, const char* name) {
                    if (!(a_mask & bit)) return;
                    if (!out.empty()) out += '+';
                    out += name;
                };
                add(APMF_API::kEquipCat_Armor,  "Armor");
                add(APMF_API::kEquipCat_Shield, "Shield");
                add(APMF_API::kEquipCat_Right,  "Right");
                add(APMF_API::kEquipCat_Left,   "Left");
                add(APMF_API::kEquipCat_Ammo,   "Ammo");
                add(APMF_API::kEquipCat_Light,  "Light");
                return out.empty() ? "none" : out;
            }

            // PLAYER AGENCY (round 3, ABI v8): armor the PLAYER put on a follower
            // through the trade/gift menu (APMF's seat allows `path=PlayerMenu`
            // by default; MFO does NOT set kEquipAuth_DenyPlayerMenu). Detected as
            // a worn ARMO that was not in the last declaration and is not this
            // tick's judged pick -- nothing of MFO's put it there -- and kept per
            // follower while owned: the mage-apparel pick may not displace it on
            // a tie (only a STRICTLY better key does; the rated branch is strictly-
            // better-than-worn by construction), and the economy keeps it instead
            // of selling it once MFO's pick displaces it (IsPlayerPick below).
            // Worker-serial; cleared with the declarations.
            std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> g_playerPicks;

            struct EquipDecl {
                std::vector<APMF_API::APMF_EquipEntry> entries;
                std::string                            names;
                void Add(RE::TESBoundObject* a_obj, std::uint8_t a_slot = APMF_API::kEquipSlot_Default) {
                    if (!a_obj) return;
                    const RE::FormID id = a_obj->GetFormID();
                    for (const auto& e : entries)
                        if (e.form == id && e.slot == a_slot) return;   // same form, same hand: once
                    entries.push_back(APMF_API::APMF_EquipEntry{ id, a_slot, { 0, 0, 0 } });
                    if (!names.empty()) names += ", ";
                    const char* nm = a_obj->GetName();
                    names += (nm && *nm) ? nm : "?";
                    if (a_slot == APMF_API::kEquipSlot_Right)      names += " (R)";
                    else if (a_slot == APMF_API::kEquipSlot_Left)  names += " (L)";
                }
                // Drop every entry declared for a named hand (a bound weapon the
                // engine put THERE outranks the ledger's entry for that hand).
                void DropHand(std::uint8_t a_slot) {
                    if (a_slot == APMF_API::kEquipSlot_Default) return;
                    std::erase_if(entries, [&](const APMF_API::APMF_EquipEntry& e) { return e.slot == a_slot; });
                }
            };

            bool SlotsOverlap(const RE::TESObjectARMO* a, const RE::TESObjectARMO* b) {
                if (!a || !b) return false;
                return (static_cast<std::uint32_t>(a->GetSlotMask()) &
                        static_cast<std::uint32_t>(b->GetSlotMask())) != 0;
            }

            // THE SLATE (MFO-B63 remainder): the judge's best OWNED piece for EVERY
            // governed slot, not just the one pick. The declaration used to be "one
            // judged pick + everything else worn", so a piece the engine's load-time
            // OutfitApply (SPID / outfit) put on was adopted into the set BECAUSE it was
            // worn, and APMF had no reason to refuse it (field 2026-09-22: Jesper's robes
            // fell out, Jesper's Armor went in). Declaring the best of each slot makes the
            // set a function of what he OWNS. Indices are logical slots: rated uses
            // ArmorBuySlot 0 head / 1 body / 2 hands / 3 feet (4 shield is NEVER declared),
            // mage uses MageClothingSlot 0-5 (jewelry included). Null = nothing for that slot.
            // ComputeOwnedGearPick is untouched (the legacy APMF-absent road and every other
            // caller keep it); the slate is a SIBLING read only by RefreshEquipDeclaration.
            // jewelInst[0] / [1] = the inventory INSTANCE (ExtraDataList + ExtraUniqueID) behind slot[4] / slot[5] when
            // ChooseJewelry chose it (null / 0 = a base-form pick). RefreshEquipDeclaration names that copy in the
            // ch.17 entry (APMF ABI v25, SetEquipEntryUid) when APMF supports it and the copy has a uid.
            struct JewelInst { RE::ExtraDataList* xl = nullptr; std::uint16_t uid = 0; };

            // IS THE DECLARED COPY WORN (APMF ABI v25 review F1). uid 0: any copy of the base worn (the entry
            // names no instance, InventoryEntryData::IsWorn). uid != 0: only the list carrying THAT ExtraUniqueID
            // counts, worn when it has ExtraWorn or ExtraWornLeft (a plain copy A worn while gemmed copy B is
            // declared is NOT worn). kMissing: the follower carries no list with that uid (the next ChooseJewelry
            // re-chooses). Shared by the drift re-send and slateLanded so the two cannot disagree.
            enum class DeclWorn { kNo, kYes, kMissing };
            DeclWorn DeclaredCopyWorn(const RE::TESObjectREFR::InventoryItemMap& a_inv, RE::TESBoundObject* a_obj,
                                      std::uint16_t a_uid) {
                const auto it = a_inv.find(a_obj);
                if (it == a_inv.end() || !it->second.second) return a_uid ? DeclWorn::kMissing : DeclWorn::kNo;
                if (a_uid == 0) return it->second.second->IsWorn() ? DeclWorn::kYes : DeclWorn::kNo;
                if (it->second.second->extraLists)
                    for (auto* xl : *it->second.second->extraLists) {
                        const auto* u = xl ? xl->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (!u || u->uniqueID != a_uid) continue;
                        return (xl->HasType(RE::ExtraDataType::kWorn) || xl->HasType(RE::ExtraDataType::kWornLeft))
                                   ? DeclWorn::kYes : DeclWorn::kNo;
                    }
                return DeclWorn::kMissing;
            }
            struct GearSlate { RE::TESObjectARMO* slot[6] = {}; bool mage = false; JewelInst jewelInst[2]; };

            // RULE 5's OVERLAP TEST, ONE copy for the declaration and the sell side: a WORN
            // non-shield piece rides in the declaration when neither the pick nor any other
            // slate piece shares a biped bit with it. DeclaredSetKeeps calls it too, so
            // "what the declaration keeps" cannot drift between the two.
            bool Rule5Overlaps(RE::TESObjectARMO* a_ar, RE::TESObjectARMO* a_pick,
                               RE::TESObjectARMO* const* a_slate, std::size_t a_n) {
                if (a_pick && a_ar != a_pick && SlotsOverlap(a_ar, a_pick)) return true;
                for (std::size_t i = 0; i < a_n; ++i)
                    if (a_slate[i] != a_ar && SlotsOverlap(a_ar, a_slate[i])) return true;
                return false;
            }

            // ── JEWELRY FOR EVERY FOLLOWER (marth 2026-10-14: "Followers don't seem to consider jewelry
            // slots (except mages) ... a piece of jewelry with the right enchants could be useful") ──
            // The rated slate above only ever names head/body/hands/feet, so a non-mage never chose a
            // ring or an amulet (it only kept whatever he happened to wear, rule 5). USEFULNESS SCORE of
            // a piece = sum over its enchantment's beneficial effects of magnitude x unit weight x
            // RELEVANCE to THIS follower: a skill fortify (and its Modifier / PowerModifier AVs) is
            // weighted by his standing in that skill (base AV / 100, so a one-hander values Fortify
            // One-handed, a mage Fortify Destruction); a magic school counts only for a caster
            // (IsCasterFollower, else 0); Magicka only for a caster; non-combat skills (smithing,
            // alchemy, speech ...) 0; Health / Stamina, regen rates, resists, armor rating and carry
            // weight carry a flat positive unit weight. Detrimental / hostile effects are skipped (a
            // curse is not a boost, as SchoolMatchScore). Pure form-data reads, worker-safe. The
            // enchantment is the INSTANCE's when the inventory entry carries one (a player-enchanted
            // ring), else the base form's.
            // EMPTY JEWELRY AS A GEM HOST (marth 2026-10-14: "they'll need to equip empty jewelry so that they
            // can socket it"): with MEO present and its carried-gem support, an UNENCHANTED ring / amulet scores a
            // small constant (kEmptyHostScore, below any genuinely enchanted piece) so it is worn when nothing
            // better exists and MEO has a socket target. Without MEO it scores 0 and is not worn.
            constexpr float kEmptyHostScore = 0.05f;
            // The enchantment LIVE on an inventory instance: its ExtraEnchantment (a player-enchanted piece or an
            // MEO-socketed one) if the list carries one, else the base form's. Explicit extra-list read (the
            // fork's InventoryEntryData::GetEnchantmentCharge reads the same ExtraEnchantment ahead of the base).
            const RE::EnchantmentItem* InstanceEnchant(const RE::TESObjectARMO* a_ar, RE::ExtraDataList* a_xl) {
                if (a_xl)
                    if (auto* xe = a_xl->GetByType<RE::ExtraEnchantment>(); xe && xe->enchantment) return xe->enchantment;
                return a_ar ? a_ar->formEnchanting : nullptr;
            }
            float JewelryScore(RE::Actor* a_follower, bool a_caster, const RE::TESObjectARMO* a_ar,
                               const RE::EnchantmentItem* ench) {
                if (!a_follower || !a_ar) return 0.0f;
                auto* avo = a_follower->AsActorValueOwner();
                if (!avo) return 0.0f;
                if (!ench)   // unenchanted: a gem host only with MEO's carried-gem support AND if MEO can socket this piece
                    return (MEOBridge::Available() && MEOBridge::CarriedGemsSupported() &&
                            MEOBridge::SocketableArmor(a_ar)) ? kEmptyHostScore : 0.0f;
                using AV = RE::ActorValue;
                const auto standing = [&](AV a_skill) {   // 0..1.5 of the follower's BASE skill / 100
                    return std::clamp(avo->GetBaseActorValue(a_skill) / 100.0f, 0.0f, 1.5f);
                };
                float total = 0.0f;
                for (const auto* e : ench->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (!mgef) continue;
                    using Flag = RE::EffectSetting::EffectSettingData::Flag;
                    if (mgef->data.flags.any(Flag::kDetrimental) || mgef->data.flags.any(Flag::kHostile)) continue;
                    using Arch = RE::EffectArchetypes::ArchetypeID;
                    const auto arch = mgef->data.archetype;
                    if (arch != Arch::kValueModifier && arch != Arch::kPeakValueModifier &&
                        arch != Arch::kDualValueModifier) continue;   // not a stat boost (waterbreathing, light ...)
                    const float mag = e->GetMagnitude();
                    if (!(mag > 0.0f)) continue;
                    // a magic school (cost / power fortify): the caster's top skills, nobody else's
                    if (const AV school = EffectBoostSchool(mgef); school != AV::kNone) {
                        if (a_caster) total += mag * standing(school);
                        continue;
                    }
                    float unit = 0.0f;   // flat unit weight, or
                    float rel  = 1.0f;   // the standing of the skill the AV fortifies
                    switch (mgef->data.primaryAV) {
                    case AV::kOneHanded: case AV::kOneHandedModifier: case AV::kOneHandedPowerModifier:
                        unit = 1.0f; rel = standing(AV::kOneHanded); break;
                    case AV::kTwoHanded: case AV::kTwoHandedModifier: case AV::kTwoHandedPowerModifier:
                        unit = 1.0f; rel = standing(AV::kTwoHanded); break;
                    case AV::kArchery: case AV::kMarksmanModifier: case AV::kMarksmanPowerModifier:
                        unit = 1.0f; rel = standing(AV::kArchery); break;
                    case AV::kBlock: case AV::kBlockModifier: case AV::kBlockPowerModifier:
                        unit = 1.0f; rel = standing(AV::kBlock); break;
                    case AV::kHeavyArmor: case AV::kHeavyArmorModifier: case AV::kHeavyArmorPowerModifier:
                        unit = 1.0f; rel = standing(AV::kHeavyArmor); break;
                    case AV::kLightArmor: case AV::kLightArmorModifier: case AV::kLightArmorPowerModifier:
                        unit = 1.0f; rel = standing(AV::kLightArmor); break;
                    case AV::kSneak: case AV::kSneakingModifier: case AV::kSneakingPowerModifier:
                        unit = 1.0f; rel = standing(AV::kSneak); break;
                    case AV::kHealth: case AV::kStamina:                    unit = 0.5f; break;
                    case AV::kMagicka:                                      unit = a_caster ? 0.5f : 0.0f; break;
                    case AV::kHealRate: case AV::kStaminaRate:              unit = 5.0f; break;
                    case AV::kMagickaRate:                                  unit = a_caster ? 5.0f : 0.0f; break;
                    case AV::kHealRateMult: case AV::kStaminaRateMult:      unit = 1.0f; break;
                    case AV::kMagickaRateMult:                              unit = a_caster ? 1.0f : 0.0f; break;
                    case AV::kResistFire: case AV::kResistFrost: case AV::kResistShock: case AV::kResistMagic:
                        unit = 1.0f; break;
                    case AV::kPoisonResist: case AV::kResistDisease:        unit = 0.3f; break;
                    case AV::kDamageResist:                                 unit = 0.3f; break;
                    case AV::kCarryWeight:                                  unit = 0.3f; break;
                    default: break;   // every other AV (non-combat skills, speed, ...) is worth 0
                    }
                    total += mag * unit * rel;
                }
                return total;
            }

            // The ring (logical slot 4) and amulet (5) the declaration names, folded into the slate
            // (a_gs.slot[4] / [5]) AFTER the rated / mage slate was built. Per slot: the best CARRIED piece
            // by JewelryScore, a piece declared only when its score > 0 -- with nothing scoring, the slot
            // stays as the slate left it (non-mage: empty, so rule 5 declares whatever he WEARS; a worn
            // ring is never stripped for no reason). Ties go to the worn piece, then the higher FormID.
            // Filtered like the rest of the builder: creature armor, a museum relic (RelicOffBody),
            // blacklisted apparel, and an excluded artifact while bLootSpecialItems is off unless it is
            // worn or the player's (Catalog::IsExcluded, as the weapon branches). BEST SCORE WINS (marth
            // 2026-10-14, as rated armor): a ring the player put on him is only the INCUMBENT -- it is kept while
            // nothing strictly outscores it (ties go to the worn piece) and is replaced by a strictly higher
            // JewelryScore; no player-pick guard applies to jewelry (playerPieceWins skips slots 4 / 5 too).
            // A quest-bound worn piece is the one exception. MAGE RECONCILIATION: MageBestPerSlot already
            // put its value-ranked ring / amulet in the slate; the usefulness winner replaces it only if
            // its score is STRICTLY higher than the mage pick's own JewelryScore (so exactly one of the
            // two methods declares each slot, the higher-scoring, the mage's on a tie). A mage candidate
            // must also pass MageApparelBuyKey (villain-coded / blacklisted / rated pieces stay out).
            void ChooseJewelry(RE::Actor* a_follower, const FollowerState& a_state, GearSlate& a_gs,
                               const RE::TESObjectREFR::InventoryItemMap& a_inv) {
                const RE::FormID id = a_follower->GetFormID();
                const bool caster = IsCasterFollower(a_state);
                const std::uint8_t top2 = a_gs.mage ? TopTwoSchoolMask(a_follower) : 0;
                const bool schoolPrimary = Config::g_mageApparelStrictSchool.load();
                const bool allowVillain  = a_gs.mage && IsNecromancerFollower(a_state);
                const bool meoGems = MEOBridge::Available() && MEOBridge::CarriedGemsSupported();
                const auto uidOf = [](RE::ExtraDataList* a_xl) -> std::uint16_t {
                    auto* u = a_xl ? a_xl->GetByType<RE::ExtraUniqueID>() : nullptr;
                    return u ? u->uniqueID : 0;
                };
                for (const int slot : { 4, 5 }) {
                    auto* worn = WornInLogicalSlot(a_follower, slot);
                    // The worn instance (its list carries ExtraWorn; a single-instance entry has none to find).
                    RE::ExtraDataList* wornXl = nullptr;
                    if (worn) {
                        if (const auto it = a_inv.find(worn); it != a_inv.end() && it->second.second && it->second.second->extraLists) {
                            for (auto* xl : *it->second.second->extraLists)
                                if (xl && xl->GetByType<RE::ExtraWorn>()) { wornXl = xl; break; }
                        }
                    }
                    // A QUEST-BOUND worn piece (the engine's per-instance IsQuestObject, as the loot guard) is never
                    // displaced, by a higher score or by the mage pick: kept as worn.
                    if (worn)
                        if (const auto it = a_inv.find(worn); it != a_inv.end() && IsQuestObjectInstance(it->second.second.get())) {
                            a_gs.slot[slot] = worn;
                            a_gs.jewelInst[slot - 4] = { wornXl, uidOf(wornXl) };
                            continue;
                        }
                    // MEO GUARD: MEO exposes no "reconcile in progress" signal (MEOBridge has none), so a worn piece
                    // that is GEMMED in MEO's carried-gem cache is kept as it is -- swapping it would race the gem
                    // moves MEO's reconcile / QueueGemMove make onto it.
                    if (meoGems && worn && uidOf(wornXl) != 0 &&
                        MEOBridge::IsCarriedGemmed(id, worn->GetFormID(), uidOf(wornXl)) &&
                        !RelicOffBody(a_follower, a_state, worn)) {
                        a_gs.slot[slot] = worn;
                        a_gs.jewelInst[slot - 4] = { wornXl, uidOf(wornXl) };
                        continue;
                    }
                    RE::TESObjectARMO* best = nullptr; float bestScore = 0.0f; JewelInst bestInst; bool bestIsWorn = false;
                    for (auto& [obj, data] : a_inv) {
                        if (!obj || data.first <= 0) continue;
                        auto* ar = obj->As<RE::TESObjectARMO>();
                        if (!ar || IsCreatureArmor(ar) || ArmorWontRender(a_follower, ar) || ar->IsShield() || MageClothingSlot(ar) != slot) continue;
                        if (RelicOffBody(a_follower, a_state, obj) || IsBlacklistedApparel(ar)) continue;
                        if (IsQuestObjectInstance(data.second.get())) continue;   // a quest-bound piece is never a candidate
                        if (ar != worn && !Config::g_lootSpecialItems.load() && Catalog::IsExcluded(ar->GetFormID()) &&
                            !PlayerGiven::IsPlayerGiven(id, ar->GetFormID())) continue;
                        if (a_gs.mage) {
                            int t = 0; std::int32_t m = 0;
                            if (!MageApparelBuyKey(ar, top2, schoolPrimary, allowVillain || ar == worn, t, m)) continue;
                        }
                        // EVERY INSTANCE is scored on its own (a gemmed copy is not the plain copy of the same base);
                        // an entry with no extra lists is one plain instance.
                        std::vector<RE::ExtraDataList*> insts;
                        if (data.second && data.second->extraLists)
                            for (auto* xl : *data.second->extraLists) insts.push_back(xl);
                        if (insts.empty()) insts.push_back(nullptr);
                        for (auto* xl : insts) {
                            const std::uint16_t uid = uidOf(xl);
                            if (meoGems && uid != 0 && MEOBridge::RemovalHeld(id, ar->GetFormID(), uid)) continue;   // on its way out
                            const float sc = JewelryScore(a_follower, caster, ar, InstanceEnchant(ar, xl));
                            if (!(sc > 0.0f)) continue;
                            const bool isWorn = (ar == worn) && (xl == wornXl);
                            if (!best || sc > bestScore || (sc == bestScore && isWorn && !bestIsWorn) ||
                                (sc == bestScore && !bestIsWorn && ar->GetFormID() > best->GetFormID())) {
                                best = ar; bestScore = sc; bestInst = { xl, uid }; bestIsWorn = isWorn;
                            }
                        }
                    }
                    if (!best) continue;
                    if (a_gs.mage && a_gs.slot[slot]) {
                        RE::ExtraDataList* mxl = nullptr;
                        if (const auto it = a_inv.find(a_gs.slot[slot]); it != a_inv.end() && it->second.second && it->second.second->extraLists)
                            for (auto* xl : *it->second.second->extraLists) { if (xl && (a_gs.slot[slot] != worn || xl == wornXl)) { mxl = xl; break; } }
                        const float mageScore = JewelryScore(a_follower, caster, a_gs.slot[slot], InstanceEnchant(a_gs.slot[slot], mxl));
                        if (!(bestScore > mageScore)) continue;   // the mage's own pick scores as high: it stands
                    }
                    a_gs.slot[slot] = best;
                    a_gs.jewelInst[slot - 4] = bestInst;
                }
            }

            // Worker. OOC only (the caller gates on judgeArmor). MAGE: MageBestPerSlot, the
            // very ranking the mage set already declared. RATED: ArmorIsBetter judges per
            // BIPED BIT while ArmorBuySlot / WornInLogicalSlot are LOGICAL slots, so a slate
            // derived from logical slots could name a piece the wear judge refused (APMF would
            // put it on). The gate `ArmorIsBetter || already worn in that slot` keeps the
            // slate a SUBSET of {worn} U {pieces the wear judge would already act on}.
            // INCUMBENCY: a worn rating>0 piece whose score >= the slot's best keeps it (never
            // downgrade real armor; a tie is the worn one, exactly ArmorIsBetter's beatenBy).
            // FormID tiebreak so two equal candidates never flip. No FitsCarryWeight (it is
            // the pick's own gate, a declaration is not a carry decision). No
            // LogArmorClassIfChanged: the pick's judge owns the one [armor] line per change.
            // Reads the caller's materialised inventory, so the slate adds NO GetInventory
            // walk (mage excepted: MageBestPerSlot walks its own, as it did before).
            void ComputeOwnedGearSlate(RE::Actor* a_follower, const FollowerState& a_state, GearSlate& a_out,
                                       const RE::TESObjectREFR::InventoryItemMap& a_inv) {
                a_out = GearSlate{};
                if (!a_follower || Config::g_dollsMode.load()) return;
                if (UsesMageClothing(a_state, a_follower) && Config::g_mageWearRobes.load()) {
                    a_out.mage = true;
                    MageSlotBest best[6];
                    MageBestPerSlot(a_follower, a_state, best);
                    for (int i = 0; i < 6; ++i)
                        a_out.slot[i] = best[i].obj ? best[i].obj->As<RE::TESObjectARMO>() : nullptr;
                    ChooseJewelry(a_follower, a_state, a_out, a_inv);   // ring / amulet: usefulness vs the mage's value pick
                    return;
                }
                using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                const auto isShieldBit = [](RE::TESObjectARMO* a) {
                    return (static_cast<std::uint32_t>(a->GetSlotMask()) &
                            static_cast<std::uint32_t>(Slot::kShield)) != 0;
                };
                const ArmorPref pref = ArmorPrefFor(a_follower);
                const bool wearsHelmets = WearsHelmets(&a_state);
                float bestScore[4] = {};
                for (auto& [obj, data] : a_inv) {
                    if (!obj || data.first <= 0) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || IsCreatureArmor(ar) || ArmorWontRender(a_follower, ar) || isShieldBit(ar)) continue;
                    if (RelicOffBody(a_follower, a_state, obj)) continue;   // museum relic: never the choice
                    if (!wearsHelmets && IsHelmetPiece(ar)) continue;      // 86e3phy41 "Wear helmets" OFF: never the choice
                    const int s = ArmorBuySlot(ar);
                    if (s < 0 || s > 3) continue;
                    if (ar->GetArmorRating() <= 0.0f) continue;             // rating-0 clothing can never win
                    if (!ArmorIsBetter(pref, a_follower, ar) && WornInLogicalSlot(a_follower, s) != ar) continue;
                    const float sc = ArmorScore(pref, ar);
                    auto*& win = a_out.slot[s];
                    if (!win || sc > bestScore[s] || (sc == bestScore[s] && ar->GetFormID() > win->GetFormID())) {
                        win = ar; bestScore[s] = sc;
                    }
                }
                for (int s = 0; s < 4; ++s) {
                    auto* worn = WornInLogicalSlot(a_follower, s);
                    if (!worn || worn->GetArmorRating() <= 0.0f || isShieldBit(worn)) continue;
                    if (RelicOffBody(a_follower, a_state, worn)) continue;  // rule 5 withholds it
                    if (!wearsHelmets && IsHelmetPiece(worn)) continue;    // 86e3phy41: rule 5 withholds it
                    if (ArmorScore(pref, worn) >= (a_out.slot[s] ? bestScore[s] : 0.0f)) a_out.slot[s] = worn;
                }
                ChooseJewelry(a_follower, a_state, a_out, a_inv);   // slots 4 / 5: the rated slate names none
            }
        }

        // THE SELL SIDE'S QUESTION TO THE WEAR JUDGE (fix/mfo-cicero-gear, 2026-10-01):
        // is a_worn one of the pieces the armor choice DECLARES for this follower? Builds the
        // same GearSlate RefreshEquipDeclaration declares (mage: MageBestPerSlot; rated: best
        // owned per slot with worn-incumbency), so the economy's redundant-inferior force-sell
        // can never contradict it (Cicero's Dawnguard helmet: sold + gem-held + re-socketed
        // + re-equipped 89 times). Worker. One inventory walk, only called for a worn
        // force-sell candidate, so it is off the common path.
        bool DeclaredSetKeeps(RE::Actor* a_follower, const FollowerState& a_state, RE::TESObjectARMO* a_worn) {
            if (!a_follower || !a_worn) return false;
            GearSlate gs;
            const auto inv = a_follower->GetInventory();
            ComputeOwnedGearSlate(a_follower, a_state, gs, inv);
            for (int i = 0; i < 6; ++i)
                if (gs.slot[i] == a_worn) return true;
            // RULE 5 (RefreshEquipDeclaration): a WORN piece no pick/slate piece overlaps is
            // declared too, so the choice keeps it (a rated helmet in a mage set with no
            // clothing for that slot; a circlet beside a helmet on another bit). Same test.
            const auto it = inv.find(a_worn);
            if (it == inv.end() || it->second.first <= 0 || !it->second.second || !it->second.second->IsWorn())
                return false;
            if (a_worn->IsShield() || RelicOffBody(a_follower, a_state, a_worn)) return false;
            ArmorPref pref; bool mageMode = false;
            auto* pickObj = ComputeOwnedGearPick(a_follower, a_state, pref, mageMode);
            auto* pick = (!mageMode && pickObj) ? pickObj->As<RE::TESObjectARMO>() : nullptr;
            RE::TESObjectARMO* sl[6]; std::size_t n = 0;
            for (int i = 0; i < 6; ++i) if (gs.slot[i]) sl[n++] = gs.slot[i];
            return !Rule5Overlaps(a_worn, pick, sl, n);
        }

        // The ring and amulet the armor choice DECLARES for this follower (null = none named), for the sell
        // side's keep set (SwapUp.cpp): the piece the declaration chose must never be sold from under it.
        // Same GearSlate RefreshEquipDeclaration builds. Worker; one inventory walk.
        void DeclaredJewelry(RE::Actor* a_follower, const FollowerState& a_state,
                             RE::TESObjectARMO*& a_outRing, RE::TESObjectARMO*& a_outAmulet) {
            a_outRing = a_outAmulet = nullptr;
            if (!a_follower) return;
            GearSlate gs;
            const auto inv = a_follower->GetInventory();
            ComputeOwnedGearSlate(a_follower, a_state, gs, inv);
            a_outRing = gs.slot[4]; a_outAmulet = gs.slot[5];
        }

        // NO DECLINE-FALLBACK (marth 2026-09-28: "MFO's fallback is deprecated"): with
        // Harbinger present and the authority supported, MFO's equips go through the
        // declaration ONLY. A REFUSED claim -- the equip seat not installed (the engine
        // then equips as in vanilla) or refused with the seat installed (an ERROR) -- no
        // longer hands the follower back to MFO's direct equips. The whole-APMF-absent
        // legacy degrade (EquipAuthoritySupported() false: no APMF, ABI < 9, or
        // bApmfEquipAuthority off) is unchanged. RefreshEquipDeclaration logs each case.
        bool EquipAuthorityLive(RE::FormID a_follower) {
            (void)a_follower;
            return APMFBridge::EquipAuthoritySupported();
        }

        void ForgetEquipDeclaration(RE::FormID a_follower) {
            g_lastDeclared.erase(a_follower); g_playerPicks.erase(a_follower); g_lastDeclSentAt.erase(a_follower);
            g_lastWithheld.erase(a_follower); g_deferLogged.erase(a_follower);
            std::erase_if(g_uncarriedLogged, [a_follower](std::uint64_t k) { return static_cast<RE::FormID>(k >> 32) == a_follower; });
        }
        void ResendEquipDeclaration(RE::FormID a_follower) { g_lastDeclared.erase(a_follower); }   // MFO-B41: the change detector only
        void ClearEquipDeclarations() {
            g_lastDeclared.clear(); g_playerPicks.clear(); g_lastDeclSentAt.clear(); g_lastWithheld.clear(); g_deferLogged.clear(); g_uncarriedLogged.clear();
        }

        bool IsPlayerPick(RE::FormID a_follower, RE::FormID a_form) {
            const auto it = g_playerPicks.find(a_follower);
            return it != g_playerPicks.end() && it->second.count(a_form) != 0;
        }

        // ── BOUND WEAPONS (round 4, probe sweep 2026-09-15) ──────────────────
        // A Bound Sword/Bow/Battleaxe is put in the hand by the engine's own
        // BoundItemEffect through ActorEquipManager::EquipObject (APMF seat path
        // `BoundItem`), i.e. an equip of a WEAP the follower never owned. Under
        // enforcement that is an off-set equip and is REFUSED -- a mage who casts
        // Bound Sword would hold nothing. So the bound weapon is DECLARED for as
        // long as its effect is live: the live ActiveEffects on the caster whose
        // base MGEF archetype is kBoundWeapon carry the weapon as the effect's
        // associatedForm (EffectSetting::EffectSettingData::associatedForm,
        // RE/E/EffectSetting.h:71; the archetype at :88). Same active-effect-list
        // road CasterHasLiveSummon walks (Actuation_Direct.cpp), on the worker,
        // read-only; a dispelled/inactive effect is over. Sorted, deduped: the
        // combat road compares it lap to lap to trigger a declaration (a bound
        // cast is the AI's own as often as MFO's, and neither passes EquipWeapon).
        std::vector<RE::FormID> LiveBoundWeapons(RE::Actor* a_follower) {
            std::vector<RE::FormID> out;
            if (!a_follower) return out;
            auto* mt = a_follower->AsMagicTarget();
            auto* list = mt ? mt->GetActiveEffectList() : nullptr;
            if (!list) return out;
            using AF = RE::ActiveEffect::Flag;
            for (auto* ae : *list) {
                if (!ae || !ae->effect || !ae->effect->baseEffect) continue;
                if (ae->flags.any(AF::kDispelled, AF::kInactive)) continue;
                const auto& d = ae->effect->baseEffect->data;
                if (d.archetype != RE::EffectSetting::Archetype::kBoundWeapon) continue;
                auto* w = d.associatedForm ? d.associatedForm->As<RE::TESObjectWEAP>() : nullptr;
                if (!w) continue;
                out.push_back(w->GetFormID());
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            return out;
        }

        void RefreshEquipDeclaration(RE::Actor* a_follower, const FollowerState& a_state,
                                     RE::FormID a_holdRight, RE::FormID a_holdLeft,
                                     bool a_leftReserved, bool a_judgeArmor, const char* a_why) {
            if (!a_follower) return;
            const RE::FormID id = a_follower->GetFormID();
            if (!a_state.mfoEnabled || !APMFBridge::EquipAuthoritySupported()) {
                APMFBridge::ReleaseEquipAuthority(id);   // idempotent; logs only when a claim stood
                g_lastDeclared.erase(id);
                return;
            }
            bool fresh = false;
            static std::unordered_set<RE::FormID> s_refusalLogged;   // worker-serial like g_lastDeclared
            if (!APMFBridge::ClaimEquipAuthority(id, &fresh)) {
                // APMF REFUSED (its equip seat is not installed, or the channel is
                // unavailable). NO DECLINE-FALLBACK (marth 2026-09-28): MFO does NOT
                // switch to its direct equips for him (EquipAuthorityLive stays true
                // while supported; Actuation's authority likewise), so nothing MFO
                // decides is equipped until Harbinger takes the claim. Said LOUDLY, once
                // per refusal streak per follower (principle 7): the bridge's own
                // warning line predates this rule and still says the direct paths run --
                // they do not.
                // Two cases, told apart by what the bridge knows (APMF_API.h v8: a
                // kIntent_EquipAuthority claim is refused while the equip seat is NOT
                // installed; IsEquipAuthorityEnforced() true proves the seat installed):
                //  * seat not installed: Harbinger neither equips nor blocks, so the
                //    ENGINE equips as in vanilla and MFO stays out (marth 2026-09-28:
                //    "MFO's fallback is deprecated") -- WARN;
                //  * seat installed, claim refused anyway: the loud ERROR.
                // Neither case makes an MFO equip or unequip.
                if (s_refusalLogged.insert(id).second) {
                    if (APMFBridge::IsEquipAuthorityEnforced())
                        spdlog::error("[equip-auth] {:08X}: Harbinger REFUSED the equip-authority claim although its "
                                      "equip seat is installed and enforcing -- MFO makes NO equips for him (no "
                                      "decline-fallback). Nothing MFO picks is equipped until the claim is taken. See "
                                      "APMF.log for why it refused.", id);
                    else
                        spdlog::warn("[equip-auth] {:08X}: Harbinger's equip seat is not installed -- MFO leaves "
                                     "equipment to the engine (no MFO equips or unequips for him)", id);
                }
                g_lastDeclared.erase(id);
                return;
            }
            s_refusalLogged.erase(id);   // taken: a later refusal is a new streak and is said again
            // F2: a handle minted since the last declaration (first claim, a
            // re-mint after APMF forgot the old one, the toggle's OFF->ON, a New
            // Game) carries NO set on APMF's side -- the change detector must not
            // answer "unchanged" against it. Forget, so this rebuild is sent.
            if (fresh) g_lastDeclared.erase(id);

            // MFO-B63: NEVER DECLARE INTO A 3D-ABSENT ACTOR. APMF's equip pass
            // needs a loaded actor: with the 3D gone it RECORDS the set, skips the
            // pass and says so ("not loaded (actor resolved, 3D absent) --
            // declaration of 7 item(s) recorded, equip pass skipped. Re-declare
            // once the actor is loaded"), and by contract it has no re-assert tick
            // of its own -- re-declaring is MFO's job. The field failure was
            // exactly this: the Followers sweep dropped and re-added all three
            // followers across a load screen, the re-add declared into a 3D-absent
            // Jesper, and SEND-ONLY-ON-CHANGE then swallowed every later rebuild
            // because it matched the set APMF had filed and never applied.
            // Skipping the send ALSO marks him dirty (the cache is dropped), so the
            // first loaded tick sends the rebuild as a change. The standing claim
            // above is kept either way -- it is what makes the scope hold across
            // the transition.
            if (!a_follower->Is3DLoaded()) {
                g_lastDeclared.erase(id);
                g_lastDeclSentAt.erase(id);   // no drift hold against the first loaded tick
                g_handedPick = {};            // F10's hand-off is still ONE tick's, consumed or not
                return;
            }

            // EQUIP-SELF-VIOLATIONS (marth 2026-10-13): NEVER BUILD THE SCOPE FROM A PERK-LESS ACTOR. The
            // `denied` mask and the hold roles are perk-derived (roles.offHand / melee), and base-form perks do
            // not survive a load until the guarded progression reapply runs -- the 2026-10-13 Cicero deck run
            // declared denied=none ~3 s before `[prog] reapply` and a shield passed for 11 s. While his reapply is
            // pending nothing is sent (the claim above stands, exactly as in the 3D-absent skip: APMF allows
            // everything on a fresh claim) and the cache is dropped so the first post-reapply build is sent.
            if (ProgAllocator::ReapplyPending(id)) {
                if (g_deferLogged.insert(id).second)
                    spdlog::info("[equip-auth] {:08X}: declaration DEFERRED -- his progression perks are not reapplied "
                                 "yet (the scope would be built from a perk-less actor)", id);
                g_lastDeclared.erase(id);
                g_handedPick = {};
                return;
            }

            EquipDecl decl;
            const WeaponRoles roles = ComputeWeaponRoles(a_follower, a_state);
            auto inv = a_follower->GetInventory();
            const auto resolve = [](RE::FormID a_fid) -> RE::TESBoundObject* {
                if (!a_fid) return nullptr;
                auto* form = RE::TESForm::LookupByID(a_fid);
                return form ? form->As<RE::TESBoundObject>() : nullptr;
            };

            // 1. THE HANDS -- the ForcedHold ledger ONLY (ABI v9, SCOPED authority).
            //    A hand is DECLARED and OWNED iff a hold stands in it. There is
            //    deliberately NO "else the weapon currently in that hand" read
            //    and NO torch read any more: that fallback WAS the F6 freeze --
            //    it declared the AI's own pick and then refused the AI's next
            //    switch (117 CombatNode would-denies in one deck run, none
            //    against a hold). With no hold the hands are UNOWNED, the seat
            //    answers `owned=0 verdict=allow`, and the follower switches
            //    weapons on his own exactly as without APMF. a_leftReserved (a
            //    cast holds/claims the left): nothing left, owned or declared --
            //    the hold yields to the cast anyway (YieldForcedLeftHand).
            const RE::FormID holdLeft = a_leftReserved ? 0 : a_holdLeft;
            // DROP A HOLD THE FOLLOWER NO LONGER CARRIES (Harbinger tier-3 review F2): Harbinger refuses, at
            // combat selection, every candidate for an OWNED hand that is not in the declared set, so a hold
            // naming a sold / dropped / looted-away weapon would leave the hand owned with nothing usable.
            // Not declared -> the hand is unowned and the AI arms it normally; the next equip gambit fire
            // re-picks. `inv` is read on this same worker call (the builder's own GetInventory above).
            const auto resolveCarried = [&](RE::FormID a_fid, const char* a_what) -> RE::TESBoundObject* {
                auto* obj = resolve(a_fid);
                if (!obj) return nullptr;
                const auto it = inv.find(obj);
                if (it != inv.end() && it->second.first > 0) return obj;
                if (g_uncarriedLogged.insert((static_cast<std::uint64_t>(id) << 32) | a_fid).second)
                    spdlog::info("[equip-auth] {:08X}: {} {:08X} '{}' not declared -- no longer carried (the hand is "
                                 "left unowned; the next equip gambit re-picks)", id, a_what, a_fid,
                                 obj->GetName() ? obj->GetName() : "?");
                return nullptr;
            };
            RE::TESBoundObject* right = resolveCarried(a_holdRight, "right hold");
            RE::TESBoundObject* left  = resolveCarried(holdLeft, "left hold");
            auto* rightW = right ? right->As<RE::TESObjectWEAP>() : nullptr;
            const bool rightRanged    = rightW && (rightW->IsBow() || rightW->IsCrossbow());
            const bool rightTwoHanded = rightW && (rightRanged || rightW->IsTwoHandedSword() || rightW->IsTwoHandedAxe());
            if (rightTwoHanded) left = nullptr;   // wearability: both hands are the right's
            // THE SCOPE (ABI v9). Armor is ALWAYS owned (rules 4/4b/5 below: the
            // judged pick, the player's picks, everything else worn). A hand is
            // owned iff held; a held bow/crossbow owns BOTH hands and the Ammo it
            // fires; a held two-hander owns both hands. Shield is NEVER owned
            // (offHand==1 keeps Actuation's direct EquipShieldOnMain; offHand==0
            // is the engine's own shield behaviour) and is DENIED by category
            // when the perks vote dual wield under bWeaponStyleControl -- the
            // original field fix, now without owning a hand (Cicero's 15 shield
            // denies stay denies without freezing his bow). Light is never owned:
            // EquipTorch and the AI's night torch pass, EXCEPT while Left is
            // owned (EquipTorch's own gate, APMFBridge::EquipAuthorityOwns).
            // THE HOLD WINS OVER RANGE (F1/F6 precedent, marth 2026-09-14): a
            // dual-wield hold under an enemy at range is NOT released for it --
            // the exits are the gambit's own condition, combat end, or a spell
            // taking the left. Releasing on range would be invented intent.
            std::uint32_t owned = APMF_API::kEquipCat_Armor;
            if (right)          owned |= APMF_API::kEquipCat_Right;
            if (left)           owned |= APMF_API::kEquipCat_Left;
            if (rightRanged)    owned |= APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left | APMF_API::kEquipCat_Ammo;
            if (rightTwoHanded) owned |= APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left;
            // SHIELD DENIED FOR A FOLLOWER WHO DOES NOT USE ONE (marth 2026-09-28: "stop
            // Jesper from equipping a shield"; pure casters, archers and two-handers do
            // not take up shields, a follower who DOES fight one-hander + shield keeps
            // his). Decided from the combat judge's OWN data (cast/Equip.cpp EquipWeapon:
            // the WeaponRoles, and its base-MAGE rule `baseClass == 3 &&
            // bMageDaggersOnly` that makes a mage's melee a dagger sidearm, not a
            // fighting style). The Shield CATEGORY is denied -- nothing declared,
            // nothing forced, no hand owned -- when
            //   * the perks vote dual wield (roles.offHand == 2, under bWeaponStyleControl:
            //     the original field fix, unchanged), or
            //   * he has no ONE-HAND melee role (roles.melee is TwoHand or Other: an
            //     archer or a two-hander never fights with a shield), or
            //   * he is a PURE CASTER: base class Mage (combatClassOverride 3, what
            //     Followers::GetBaseClass reads) under bMageDaggersOnly, with no shield
            //     perk vote (roles.offHand != 1) -- Jesper. A caster with any other base
            //     class and a one-hand weapon (a spellsword) is NOT denied (review R2-3).
            // A one-hand fighter with shield votes, or with no off-hand vote at all, is
            // unaffected. APMF_API.h v9 Categorize: only an ARMO with the shield biped
            // bit competes for Shield (field: cat=Shield+Left), so a left-hand weapon
            // (Left), a torch (Light+Left) and a spell (not a governed type) are never
            // refused by it. Economy's worn-shield force-sell keeps its original
            // dual-wield scope (Economy.cpp deniedWorn).
            const bool pureCaster = a_state.combatClassOverride == 3 && Config::g_mageDaggersOnly.load() &&
                                    roles.offHand != 1;
            const bool noShieldUser =
                (roles.offHand == 2 && Config::g_weaponStyleControl.load()) ||
                roles.melee != WepClass::OneHand || pureCaster;
            const std::uint32_t denied = noShieldUser ? static_cast<std::uint32_t>(APMF_API::kEquipCat_Shield) : 0u;
            // THE HAND (ABI v8): a ONE-HAND weapon names its hand -- the ledger's
            // right/left -- so APMF itself places a dual-wielder's off-hand weapon
            // in the LEFT (the v7 slot-less pass put it in the right and evicted
            // the sword, F2/F3). A two-hander, a bow: Default, the engine picks
            // (APMF_API.h; a two-hander competes for both hands from its TYPE,
            // never from the slot). The same form may stand once per hand (two
            // identical daggers).
            const auto handOf = [](RE::TESBoundObject* a_obj, std::uint8_t a_hand) -> std::uint8_t {
                auto* w = a_obj ? a_obj->As<RE::TESObjectWEAP>() : nullptr;
                const bool oneHand = w && (w->IsOneHandedSword() || w->IsOneHandedDagger() ||
                                           w->IsOneHandedAxe()   || w->IsOneHandedMace());
                return oneHand ? a_hand : APMF_API::kEquipSlot_Default;
            };
            decl.Add(right, handOf(right, APMF_API::kEquipSlot_Right));
            decl.Add(left,  handOf(left,  APMF_API::kEquipSlot_Left));

            // 1b. BOUND WEAPONS (see LiveBoundWeapons above). Declared while the
            //     effect is live, with the hand the engine actually put it in when
            //     it is held (the BoundItemEffect equips into the CASTING hand --
            //     MFO's gambit spells are cast LEFT, the AI's from either); a
            //     one-hander not (yet) held goes Right unless the ledger's left
            //     names it; a bound bow/two-hander is Default. A bound weapon held
            //     in a hand REPLACES the ledger's entry for that hand (the engine
            //     already displaced it; declaring both is two items for one slot).
            //     ONLY INTO AN OWNED HAND (v9): an entry in an unowned category
            //     is one APMF's pass only logs as "unowned entry skipped", and the
            //     seat lets a BoundItem equip into an unowned hand through
            //     (`owned=0`) with no declaration at all -- so a one-hander needs
            //     its hand owned, a bound bow/two-hander needs BOTH. Logged once
            //     per bound form, when it is new since the last declaration.
            //     Dropped from the set by the next rebuild after the effect ends
            //     (the combat road triggers on that change too).
            const auto* lastSent = [&]() -> const std::vector<DeclKey>* {
                const auto it = g_lastDeclared.find(id);
                return it == g_lastDeclared.end() ? nullptr : &it->second.keys;
            }();
            const auto lastSentHas = [&](RE::FormID a_form) {
                if (!lastSent) return false;
                return std::any_of(lastSent->begin(), lastSent->end(), [&](const DeclKey& k) { return k.first == a_form; });
            };
            for (const RE::FormID bf : LiveBoundWeapons(a_follower)) {
                auto* w = RE::TESForm::LookupByID<RE::TESObjectWEAP>(bf);
                if (!w) continue;
                const bool oneHand = w->IsOneHandedSword() || w->IsOneHandedDagger() ||
                                     w->IsOneHandedAxe()   || w->IsOneHandedMace();
                std::uint8_t hand = APMF_API::kEquipSlot_Default;
                if (oneHand) {
                    auto* heldR = a_follower->GetEquippedObject(false);
                    auto* heldL = a_follower->GetEquippedObject(true);
                    if (heldR == w)                 hand = APMF_API::kEquipSlot_Right;
                    else if (heldL == w)            hand = APMF_API::kEquipSlot_Left;
                    else if (holdLeft == bf)        hand = APMF_API::kEquipSlot_Left;
                    else                            hand = APMF_API::kEquipSlot_Right;
                    const std::uint32_t needs = hand == APMF_API::kEquipSlot_Left
                                                  ? APMF_API::kEquipCat_Left : APMF_API::kEquipCat_Right;
                    if (!(owned & needs)) continue;   // unowned hand: the seat lets it through undeclared
                    decl.DropHand(hand);
                } else {
                    // A bound bow/two-hander takes both hands: nothing else is held.
                    if ((owned & (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left)) !=
                        (APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left)) continue;
                    decl.DropHand(APMF_API::kEquipSlot_Right);
                    decl.DropHand(APMF_API::kEquipSlot_Left);
                }
                decl.Add(w, hand);
                if (!lastSentHas(bf))
                    spdlog::info("[equip-auth] {:08X}: declare bound '{}' ({})", id,
                                 w->GetName() ? w->GetName() : "?",
                                 hand == APMF_API::kEquipSlot_Right ? "right"
                                 : hand == APMF_API::kEquipSlot_Left ? "left" : "default");
            }

            // 1c. ONE-SHOT PLACEMENTS (WEIGHTING W1, cast/EquipWeigh.cpp; marth 2026-10-12: "Direct equip is
            //     going. Harbinger has all the responsibility now."). Under Harbinger ABI >= 22 every equip MFO
            //     used to make directly (the shield top-up, the torch, the silver switch-back, an owned / looted
            //     weapon upgrade, the excluded-weapon heal, the cast's gear give-back) is declared HERE until it
            //     is seen in its hand (Actuation::PendingPlacements retires it: landed, gone, or a loud 10 s
            //     "did not land"). Its category is OWNED for that long so Harbinger's equip pass places it (an
            //     entry in an unowned category is only logged as skipped). THE LEDGER WINS: a placement for a
            //     hand a hold already declares is left out (two items for one slot is not wearable at once,
            //     ch.17 "the set must be wearable"); a left-hand item is left out while a cast holds the left
            //     (a_leftReserved, D3); a shield is left out while the Shield category is DENIED for him.
            if (Actuation::WeighingEquips()) {
                const bool rightDeclared = right != nullptr;
                const bool leftDeclared  = left != nullptr || rightTwoHanded;
                for (const auto& pl : Actuation::PendingPlacements(a_follower)) {
                    auto* obj = resolveCarried(pl.form, "placement");
                    if (!obj) continue;
                    std::uint32_t needs = 0;
                    std::uint8_t  slot  = pl.slot;
                    bool skip = false;
                    if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                        const bool twoHands = w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow();
                        if (twoHands) {
                            needs = APMF_API::kEquipCat_Right | APMF_API::kEquipCat_Left;
                            skip  = rightDeclared || leftDeclared || a_leftReserved;
                        } else if (pl.slot == APMF_API::kEquipSlot_Left) {
                            needs = APMF_API::kEquipCat_Left;
                            skip  = leftDeclared || a_leftReserved;
                        } else {
                            needs = APMF_API::kEquipCat_Right;
                            skip  = rightDeclared;
                        }
                    } else if (auto* ar = obj->As<RE::TESObjectARMO>(); ar && ar->IsShield()) {
                        // Harbinger categorizes a shield Shield|Left and a torch Light|Left (EquipSink Categorize)
                        // and its pass skips an entry whose categories are not ALL owned (review of e06a54f,
                        // SEV-2): both own Left too, and are declared kEquipSlot_Left (FACETS ch.17).
                        needs = APMF_API::kEquipCat_Shield | APMF_API::kEquipCat_Left;
                        slot  = APMF_API::kEquipSlot_Left;
                        skip  = leftDeclared || a_leftReserved || (denied & APMF_API::kEquipCat_Shield) != 0;
                    } else if (obj->As<RE::TESObjectLIGH>()) {
                        needs = APMF_API::kEquipCat_Light | APMF_API::kEquipCat_Left;
                        slot  = APMF_API::kEquipSlot_Left;
                        skip  = leftDeclared || a_leftReserved;
                    } else {
                        continue;   // armor rides rules 4/5, ammo rule 2: nothing else is placed this way
                    }
                    if (skip) {
                        if (Actuation::PlacementSkipFirstLog(id, pl.form))   // once per (follower, item); cleared on revert / dismissal
                            spdlog::info("[equip-place] {:08X}: '{}' not declared this build -- its hand is held by "
                                         "the ledger or a cast, or its category is denied (kept pending)", id,
                                         obj->GetName() ? obj->GetName() : "?");
                        continue;
                    }
                    owned |= needs;
                    decl.Add(obj, slot);
                    Actuation::NotePlacementDeclared(id, pl.form);
                }
            }

            // 1d. THE STANDING TORCH (feat/mfo-torch-ch17, marth 2026-10-20). While a torch rule's CONDITION holds
            //     (read off his table at this build: Upkeep.cpp TorchDeclaredFor), the carried Light is declared
            //     with Light|Left OWNED, so Harbinger's equip pass keeps it in hand and its torch seat blocks every
            //     other source while the claim owns Light. NOT declared (Light unowned, the engine manages light as
            //     normal) when no torch rule holds, in combat, while he is seated / sleeping / in furniture, or when
            //     Left is already owned by anything above (a hold, a two-hander, a bound weapon, a cast, a 1c
            //     placement): THE LEDGER WINS, as in 1c.
            {
                const bool leftFree = !(owned & APMF_API::kEquipCat_Left) && !a_leftReserved;
                if (const RE::FormID torch = TorchDeclaredFor(a_follower, a_state, leftFree); torch) {
                    if (auto* obj = resolveCarried(torch, "torch"); obj && obj->As<RE::TESObjectLIGH>()) {
                        owned |= APMF_API::kEquipCat_Light | APMF_API::kEquipCat_Left;
                        decl.Add(obj, APMF_API::kEquipSlot_Left);
                    }
                }
            }

            // 2. AMMO -- only under a bow/crossbow HOLD (v9: Ammo is owned exactly
            //    then). With no ranged hold the archer AI's own ammo equips pass
            //    in an unowned category; declaring ammo there would only be an
            //    entry the pass skips as unowned.
            //    THE SWAP-UP RULE (86e3ebfu3, logistics/SwapUp.cpp), in this order:
            //    (a) a PLAYER AMMO PICK stands (review round 1 on 4f23c30): under
            //        enforcement, with Ammo OWNED by the last declaration, a worn
            //        ammo that declaration did not carry can only have come through
            //        the trade/gift menu (APMF's seat refuses the AI's own equips in
            //        an owned category and allows path=PlayerMenu) -- it is recorded
            //        in g_playerPicks like rule 4b's armor, declared while owned,
            //        and HeldAmmo pins it (never shed, never sold);
            //    (b) a worn SPECIAL round (explosion projectile / enchanted) or a
            //        worn NON-PLAYABLE one (a Bound Bow's arrows) stays as it is;
            //    (c) else the BEST ordinary stack by the rule's rank (damage, value
            //        breaking a tie; the worn stack wins a tie) -- but over a DIFFERENT
            //        worn stack only when it holds at least kAmmoDeclareMin (20)
            //        rounds (MFO-B44: owning Ammo pins the archer to the declared
            //        stack, so a 5-arrow better stack would leave him dry after 5
            //        shots with the category refused). 20 is about one fight's
            //        volley for an NPC archer at vanilla draw speed (~2-3 s a shot,
            //        under a minute of shooting); below it the worn stack keeps the
            //        hold. The keep target (>= 50) is deliberately NOT the bar: a
            //        better stack of 30 is worth nocking even though it does not yet
            //        retire the old one.
            if (rightRanged) {
                const bool wantBolt = rightW->IsCrossbow();
                auto& ammoPicks = g_playerPicks[id];
                const auto lastIt = g_lastDeclared.find(id);
                const bool ammoWasOwned = lastIt != g_lastDeclared.end() &&
                                          (lastIt->second.owned & APMF_API::kEquipCat_Ammo) != 0;
                RE::TESAmmo* worn = nullptr; bool wornSpecial = false;
                RE::TESAmmo* best = nullptr; float bestDmg = 0.0f; std::int32_t bestVal = 0;
                std::int32_t bestCount = 0; bool bestWorn = false;
                RE::TESAmmo* pickAmmo = nullptr; bool pickWorn = false;
                for (auto& [obj, data] : inv) {
                    if (!obj || data.first <= 0) continue;
                    auto* am = obj->As<RE::TESAmmo>();
                    if (!am || AmmoIsBolt(am) != wantBolt) continue;
                    auto* entry = data.second.get();
                    const bool isWorn = entry && entry->IsWorn();
                    const bool special = !AmmoSwapEligible(am) || AmmoIsSpecial(am, entry);
                    if (isWorn) { worn = am; wornSpecial = special; }
                    if (ammoPicks.count(am->GetFormID()) && (!pickAmmo || (isWorn && !pickWorn))) {
                        pickAmmo = am; pickWorn = isWorn;
                    }
                    if (special) continue;
                    const float        dmg = AmmoDamage(am);
                    const std::int32_t val = entry ? entry->GetValue() : 0;
                    if (!best || AmmoRankAbove(dmg, val, bestDmg, bestVal) ||
                        (dmg == bestDmg && val == bestVal && isWorn && !bestWorn)) {
                        best = am; bestDmg = dmg; bestVal = val;
                        bestCount = static_cast<std::int32_t>(data.first); bestWorn = isWorn;
                    }
                }
                // SILVER AMMO (86e3p2gp9): the declaration flips silver<->ordinary at an episode open / close, and the
                // rebuild can run before APMF's swap lands; that worn ammo is NOT a player pick. Never record one while
                // an episode is open or a declaration was sent within kDeclDriftHold.
                const auto sentAt = g_lastDeclSentAt.find(id);
                const bool ammoSwapInFlight = Actuation::SilverAmmoEpisode(id) != 0 ||
                    (sentAt != g_lastDeclSentAt.end() &&
                     std::chrono::steady_clock::now() - sentAt->second < kDeclDriftHold);
                if (worn && !ammoSwapInFlight && APMFBridge::IsEquipAuthorityEnforced() && ammoWasOwned &&
                    !lastSentHas(worn->GetFormID()) && !ammoPicks.count(worn->GetFormID())) {
                    ammoPicks.insert(worn->GetFormID());
                    pickAmmo = worn; pickWorn = true;
                    spdlog::info("[equip-auth] {:08X}: player put on ammo '{}' -- kept", id,
                                 worn->GetName() ? worn->GetName() : "?");
                }
                RE::TESAmmo* declared = nullptr;
                if (pickAmmo)                                        declared = pickAmmo;   // (a)
                else if (worn && wornSpecial)                        declared = worn;       // (b)
                else if (best && (!worn || best == worn || bestCount >= kAmmoDeclareMin))
                                                                     declared = best;       // (c)
                else                                                 declared = worn ? worn : best;
                // SILVER AMMO (86e3p2gp9): an open silver-ammo episode declares its silver ammo; with none open,
                // a worn silver stack that only survived as a 'special' worn round yields to the ordinary best.
                if (const RE::FormID sid = Actuation::SilverAmmoEpisode(id)) {
                    auto* sa = RE::TESForm::LookupByID<RE::TESAmmo>(sid);
                    if (sa && AmmoIsBolt(sa) == wantBolt && inv.find(sa) != inv.end()) declared = sa;
                } else if (!pickAmmo && worn && declared == worn && IsSilverAmmo(worn) && best) {
                    declared = best;
                }
                decl.Add(declared);
            }

            // 3. THE SHIELD: NOT DECLARED (v9). Shield is never an owned category:
            //    offHand==1 puts it on by Actuation's direct EquipShieldOnMain
            //    (the pre-authority road, an unowned-category equip the seat
            //    allows), offHand==2 DENIES the category above (no hand owned for
            //    it), offHand==0 is the engine's own shield behaviour. A worn
            //    shield stays out of rule 5 too (it competes for Shield+Left).

            // 4. THE JUDGED ARMOR PICK (EquipBestOwnedGear's own pick, one per declaration).
            //    OOC road only (a_judgeArmor): legacy never wears armor in combat
            //    (EquipBestOwnedGear is OOC-only, its mage branch !IsInCombat), so
            //    the combat road (DeclareFromLedger) declares the worn armor as is
            //    (F9). Consumed from EquipBestOwnedGear's hand-off when that pass
            //    ran this tick, else computed here (F10).
            RE::TESObjectARMO* pick = nullptr;
            // THE MAGE SET (2026-09-28, Jesper's set 6 / set 7 flip; marth: "mage rules
            // are clothing items, not armor, and most expensive wins"). For a mage-apparel
            // follower the OOC road declares the WHOLE per-slot best set at once
            // (MageBestPerSlot: every logical slot with an owned best, jewelry included,
            // the existing ranking unchanged) instead of the mage judge's ONE correction
            // per tick plus "everything else worn". THE FLIP CAME FROM THAT PAIR: the
            // engine's OutfitApply / SPID strip unequipped robes AND gauntlets, the judge
            // re-picked one slot per tick, and rule 5 only re-declared what was WORN, so
            // tick 1 declared the robes without the stripped gauntlets and tick 2 put
            // them back. The set is now a function of what he OWNS, not of what is worn,
            // so a strip cannot change it; the MFO-B63 drift re-send (below) behaves as on
            // main. No piece is kept because it WAS declared. Built in body-first order;
            // a best that overlaps a piece already chosen is skipped (never two items for
            // one slot). The legacy (APMF-absent) EquipBestOwnedGear single pick is
            // unchanged, and the combat road (a_judgeArmor false) still declares worn.
            const bool mageSetMode = a_judgeArmor && !Config::g_dollsMode.load() &&
                                     UsesMageClothing(a_state, a_follower) && Config::g_mageWearRobes.load();
            // "WEAR HELMETS" (ClickUp 86e3phy41, marth 2026-10-12). OFF for him (board override, else
            // bWearHelmets): the pick, the slate and the mage set already hold no head piece (the judges
            // skip IsHelmetPiece at the source), rule 4b records none, and rule 5 declares none. APMF_API.h
            // has no head CATEGORY (Armor = every non-shield ARMO), but Armor is ALWAYS OWNED here, so a
            // head piece left out of the set is an OFF-SET equip the seat refuses (OutfitApply, SPID, the
            // AI) -- the head-slot block, expressed with the existing scope. APMF never unequips and MFO
            // does not either (marth 2026-10-20), so a head piece he is already WEARING stays on until the
            // Harbinger strip facet lands; it is simply not declared.
            const bool wearsHelmets = WearsHelmets(&a_state);
            if (a_judgeArmor && !mageSetMode) {
                RE::TESBoundObject* pickObj = nullptr;
                if (g_handedPick.set && g_handedPick.follower == id) {
                    pickObj = resolve(g_handedPick.pick);
                } else {
                    ArmorPref pref; bool mageMode = false;
                    pickObj = ComputeOwnedGearPick(a_follower, a_state, pref, mageMode);
                }
                pick = pickObj ? pickObj->As<RE::TESObjectARMO>() : nullptr;
            }
            g_handedPick = {};   // one tick's hand-off, consumed or not

            // THE SLATE (MFO-B63 remainder), OOC only: the best owned piece for EVERY governed
            // slot, built into ONE ORDERED ACCEPTANCE with the pick first and everything-else-
            // worn (rule 5) last. accept() skips null, shield, a duplicate, and any piece that
            // overlaps one already accepted (the pick included): the engine cannot wear two items
            // sharing a biped bit, and the exclusion is what stops APMF tolerating the engine's
            // outfit piece beside the declared one. Body first, so a cuirass that also covers
            // the hands wins those bits before the gauntlets are considered. For the mage set
            // this is the order it always had.
            std::vector<RE::TESObjectARMO*> slate;
            // The chosen ring / amulet COPY (ChooseJewelry), read below to name it in the declaration (ABI v25).
            RE::TESObjectARMO* jewelForm[2] = {};
            RE::ExtraDataList* jewelXl[2]   = {};
            std::uint16_t      jewelUid[2]  = {};
            // The uid the declaration will name for a_ar (0 = the plain base-form entry).
            const auto declUid = [&](const RE::TESBoundObject* a_ar) -> std::uint16_t {
                for (int j = 0; j < 2; ++j)
                    if (a_ar && a_ar == jewelForm[j]) return jewelUid[j];
                return 0;
            };
            if (a_judgeArmor) {
                GearSlate gs;
                ComputeOwnedGearSlate(a_follower, a_state, gs, inv);
                for (int j = 0; j < 2; ++j) { jewelForm[j] = gs.slot[4 + j]; jewelXl[j] = gs.jewelInst[j].xl; }
                // The copy's uid, read NOW from the follower's own list (the engine rewrites uids on a container
                // transfer; MEOBridge::WornUid reads the same field). 0 = no uid, or APMF older than ABI v25.
                if (APMFBridge::EquipInstanceSupported())
                    for (int j = 0; j < 2; ++j)
                        if (jewelForm[j] && jewelXl[j])
                            if (const auto* u = jewelXl[j]->GetByType<RE::ExtraUniqueID>()) jewelUid[j] = u->uniqueID;
                const auto accept = [&](RE::TESObjectARMO* b) {
                    if (!b || b->IsShield()) return;
                    if (pick && SlotsOverlap(pick, b)) return;
                    // Head is a LOGICAL slot (IsHeadSlotMask: Head|Hair|Circlet) while SlotsOverlap
                    // is per biped bit, so two head pieces on different bits are one slot too.
                    if (std::any_of(slate.begin(), slate.end(),
                                    [&](RE::TESObjectARMO* c) {
                                        return c == b || SlotsOverlap(c, b) ||
                                               (IsHeadSlotMask(static_cast<std::uint32_t>(c->GetSlotMask())) &&
                                                IsHeadSlotMask(static_cast<std::uint32_t>(b->GetSlotMask())));
                                    }))
                        return;
                    slate.push_back(b);
                };
                for (const int slot : { 1, 0, 2, 3, 4, 5 }) accept(gs.slot[slot]);
                // [jewelry]: one line per follower when the chosen ring / amulet CHANGES (worker-serial).
                static std::unordered_map<RE::FormID, std::pair<RE::FormID, RE::FormID>> s_jewelLogged;
                const std::pair<RE::FormID, RE::FormID> now{ gs.slot[4] ? gs.slot[4]->GetFormID() : 0,
                                                             gs.slot[5] ? gs.slot[5]->GetFormID() : 0 };
                if (const auto jl = s_jewelLogged.find(id); jl == s_jewelLogged.end() ? (now.first || now.second)
                                                                                      : jl->second != now) {
                    s_jewelLogged[id] = now;
                    const bool jc = IsCasterFollower(a_state);
                    const auto jscore = [&](RE::TESObjectARMO* a, const JewelInst& ji) {
                        return a ? JewelryScore(a_follower, jc, a, InstanceEnchant(a, ji.xl)) : 0.0f;
                    };
                    spdlog::info("[jewelry] {:08X}: ring '{}' score={:.1f} | amulet '{}' score={:.1f}", id,
                                 gs.slot[4] && gs.slot[4]->GetName() ? gs.slot[4]->GetName() : "(none)", jscore(gs.slot[4], gs.jewelInst[0]),
                                 gs.slot[5] && gs.slot[5]->GetName() ? gs.slot[5]->GetName() : "(none)", jscore(gs.slot[5], gs.jewelInst[1]));
                }
            }

            // 4b. THE PLAYER'S OWN CHOICE (round 3, ABI v8: APMF lets the trade/gift
            //     menu equip through). A worn non-shield ARMO that the last
            //     declaration did not carry and that is not this tick's pick was put
            //     there by the player: record it, log it once, and let it stand.
            //     ONLY UNDER ENFORCEMENT (Fable F-A on e0b52b5): in observe mode APMF
            //     equips the pick non-forced and the engine takes the old piece
            //     back, which reads exactly like a trade-menu dress -- so with
            //     IsEquipAuthorityEnforced() false NOTHING is recorded (the
            //     "player put on" line cannot occur in observe mode). Under
            //     enforcement an off-set engine equip is refused, so a new worn
            //     piece can only be the player's (or a script's) -- and only on a
            //     tick where MFO is not contesting the slot: this tick's pick is
            //     absent, or already worn (a landed pick proves MFO took the slot,
            //     not that something took it back). The rated
            //     judge only ever picks a STRICTLY better-scored piece than what is
            //     worn (ArmorIsBetter), so MFO's pick wins back the slot exactly when
            //     it should; the MAGE judge breaks ties by FormID, so a pick that
            //     merely ties the player's piece on (tier, metric) is dropped here.
            //     A displaced player piece stays recorded while owned: the economy
            //     KEEPS it (IsPlayerPick) instead of selling it as a redundant
            //     inferior. Prune what the follower no longer owns.
            auto& picks = g_playerPicks[id];
            for (auto it = picks.begin(); it != picks.end();) {
                auto* form = RE::TESForm::LookupByID(*it);
                auto* obj  = form ? form->As<RE::TESBoundObject>() : nullptr;
                auto invIt = obj ? inv.find(obj) : inv.end();
                if (invIt == inv.end() || invIt->second.first <= 0) it = picks.erase(it);
                else                                                ++it;
            }
            const auto* last = [&]() -> const std::vector<DeclKey>* {
                const auto it = g_lastDeclared.find(id);
                return it == g_lastDeclared.end() ? nullptr : &it->second.keys;
            }();
            const auto lastHas = [&](RE::FormID a_form) {
                if (!last) return true;   // no declaration yet: nothing can be "new since"
                return std::any_of(last->begin(), last->end(), [&](const DeclKey& k) { return k.first == a_form; });
            };
            const bool pickWorn = [&]() {
                if (!pick) return false;
                const auto it = inv.find(pick);
                return it != inv.end() && it->second.second && it->second.second->IsWorn();
            }();
            const auto wornNow = [&](RE::TESObjectARMO* a_ar) {
                return DeclaredCopyWorn(inv, a_ar, declUid(a_ar)) == DeclWorn::kYes;   // review F1: the declared COPY
            };
            // MAGE ROAD ONLY (the old mageSetLanded): MFO is contesting a slot until every set
            // piece is on him. The RATED road keeps main's `(!pick || pickWorn)` and main's
            // 4b skip (`ar == pick` only): the rated slate holds every worn incumbent, so
            // gating or skipping on it would stop a player-traded piece ever being recorded.
            const bool slateLanded = std::all_of(slate.begin(), slate.end(), wornNow);
            const bool mayRecordPlayerPick = APMFBridge::IsEquipAuthorityEnforced() &&
                                             (mageSetMode ? slateLanded : (!pick || pickWorn));
            if (mayRecordPlayerPick) {
                for (auto& [obj, data] : inv) {
                    if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || ar->IsShield() || ar == pick) continue;
                    if (!wearsHelmets && IsHelmetPiece(ar)) continue;   // 86e3phy41: rule 5 withholds it, never "kept"
                    if (mageSetMode && std::find(slate.begin(), slate.end(), ar) != slate.end()) continue;
                    const RE::FormID f = ar->GetFormID();
                    if (lastHas(f) || picks.count(f)) continue;
                    // MFO withheld it itself on the last build (a relic verdict that has
                    // since flipped back): not the player's choice.
                    if (const auto w = g_lastWithheld.find(id); w != g_lastWithheld.end() && w->second.count(f)) continue;
                    // A museum relic left out of the set (RelicOffBody) is not a player
                    // pick just because it is worn and undeclared: the player's own
                    // relic dressing is PlayerGiven's record, which RelicOffBody honours.
                    if (RelicOffBody(a_follower, a_state, obj)) continue;
                    picks.insert(f);
                    const int pcs = MageClothingSlot(ar);
                    spdlog::info("[equip-auth] {:08X}: player put on '{}' -- {}", id,
                                 ar->GetName() ? ar->GetName() : "?",
                                 (pcs == 4 || pcs == 5) ? "kept while nothing scores higher (jewelry: best score wins)" : "kept");
                }
            }
            // PLAYER AGENCY: the player's worn piece (g_playerPicks) overlapping a CHALLENGER
            // stands unless the challenger strictly beats it. Returns that piece (nullptr = the
            // challenger stands). Applied to the pick AND every slate entry. MAGE judge: keep the
            // player's piece on a tie of (tier, metric). RATED judge: ArmorIsBetter and the
            // slate's incumbency already demand a strictly higher score than the worn piece, so
            // nothing extra is needed there.
            const auto playerPieceWins = [&](RE::TESObjectARMO* a_challenger) -> RE::TESObjectARMO* {
                if (!a_challenger || picks.empty()) return nullptr;
                if (!(UsesMageClothing(a_state, a_follower) && Config::g_mageWearRobes.load())) return nullptr;
                if (const int cs = MageClothingSlot(a_challenger); cs == 4 || cs == 5) return nullptr;   // jewelry: best JewelryScore wins (ChooseJewelry)
                const std::uint8_t top2 = TopTwoSchoolMask(a_follower);
                const bool schoolPrimary = Config::g_mageApparelStrictSchool.load();
                int ct = 0; std::int32_t cm = 0;
                const bool ranks = MageApparelBuyKey(a_challenger, top2, schoolPrimary, IsNecromancerFollower(a_state), ct, cm);
                for (auto& [obj, data] : inv) {
                    if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || ar == a_challenger || !picks.count(ar->GetFormID()) || !SlotsOverlap(ar, a_challenger)) continue;
                    int wt = 0; std::int32_t wm = 0;
                    MageApparelBuyKey(ar, top2, schoolPrimary, true, wt, wm);
                    const bool strictlyBetter = ranks && (ct > wt || (ct == wt && cm > wm));
                    if (!strictlyBetter) return ar;   // the player's piece stands
                }
                return nullptr;
            };
            if (pick && playerPieceWins(pick)) pick = nullptr;
            for (auto& b : slate)
                if (auto* w = playerPieceWins(b)) b = w;   // the player's piece stands in that slot
            decl.Add(pick);
            for (auto* b : slate) decl.Add(b);

            // 5. EVERYTHING ELSE WORN that is apparel, minus what the pick (or the mage
            //    set) displaces. For a mage set this is only what no set piece covers:
            //    pieces outside the dress-up slots, and slots he owns no clothing for.
            //    Shields are never declared (rule 3: an unowned category). A museum
            //    relic is never declared (RelicOffBody): Armor is owned, so the seat
            //    refuses its re-equip; worn now, it ships at the next deposit.
            std::unordered_set<RE::FormID> withheldNow;   // -> g_lastWithheld after this build
            for (auto& [obj, data] : inv) {
                if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                auto* ar = obj->As<RE::TESObjectARMO>();
                if (!ar || ar->IsShield()) continue;
                // 86e3phy41 "Wear helmets" OFF: a worn head piece is never declared, so the seat refuses
                // its re-equip (Armor is owned). MFO does NOT take it off itself (marth 2026-10-20: no MFO
                // UnequipObject; a narrow Harbinger "strip these named items" facet is a separate tier-A
                // round, adopted then). Until then a helmet already worn stays worn. withheldNow keeps it
                // out of rule 4b's "player put on" record on the build after the toggle flips back ON.
                if (!wearsHelmets && IsHelmetPiece(ar)) {
                    withheldNow.insert(ar->GetFormID());
                    continue;
                }
                if (Rule5Overlaps(ar, pick, slate.data(), slate.size())) continue;
                if (RelicOffBody(a_follower, a_state, obj)) {
                    withheldNow.insert(ar->GetFormID());
                    // Once per (follower, relic) per session: the field proof of item 3.
                    static std::unordered_set<std::uint64_t> s_relicLogged;   // worker-serial like g_lastDeclared
                    if (s_relicLogged.insert((static_cast<std::uint64_t>(id) << 32) | ar->GetFormID()).second)
                        spdlog::info("[equip-auth] {:08X}: museum relic '{}' is worn -- kept OUT of the declared set "
                                     "(its re-equip is refused; it ships at the next deposit)", id,
                                     ar->GetName() ? ar->GetName() : "?");
                    continue;
                }
                decl.Add(ar);
            }
            // THE CHOSEN JEWELRY COPY (APMF ABI v25): the ring / amulet entry names the instance ChooseJewelry
            // chose, so a gemmed (or empty socketable) copy is worn rather than another copy of the same base.
            // ONLY the two jewelry entries (Default slot): a uid on a hand entry would take it out of the combat
            // AI's weapon switching (APMF docs, review F1). Only when the slate kept the piece (accept() may
            // drop it), APMF is ABI >= 25, and the copy has a nonzero ExtraUniqueID, read NOW from the follower's
            // own inventory list (the engine rewrites uids on a container transfer; MEOBridge::WornUid reads the
            // same field). No uid: the plain base-form entry, as before.
            for (int j = 0; j < 2; ++j) {
                const std::uint16_t uid = jewelUid[j];   // 0 unless APMF >= v25 and the copy has a uid
                if (uid == 0 || std::find(slate.begin(), slate.end(), jewelForm[j]) == slate.end()) continue;
                for (auto& e : decl.entries) {
                    if (e.form != jewelForm[j]->GetFormID() || e.slot != APMF_API::kEquipSlot_Default) continue;
                    APMF_API::SetEquipEntryUid(e, uid);
                    // Next to its item in the declare line's names (review SEV-5).
                    const char* nm = jewelForm[j]->GetName();
                    const std::string item = (nm && *nm) ? nm : "?";
                    const std::string tag  = fmt::format("{} [{} uid {:04X}]", item, j == 0 ? "ring" : "amulet", uid);
                    if (const auto at = decl.names.find(item); at != std::string::npos) decl.names.replace(at, item.size(), tag);
                    else decl.names += ", " + tag;
                    break;
                }
            }

            // Every build, sent or not: what 4b must not read as the player's next tick.
            if (withheldNow.empty()) g_lastWithheld.erase(id);
            else                     g_lastWithheld[id] = std::move(withheldNow);

            // SEND ONLY ON CHANGE.
            // SEND ONLY ON CHANGE -- no forced re-issue (F3, Fable round 2): a
            // re-send is a declaration EVENT, and under the v7 ABI APMF's slot-less
            // pass answers it by putting the declared off-hand weapon in the RIGHT
            // hand (evicting the sword) before MFO's own hop puts both back -- four
            // engine ops and a visible flicker per re-send. The top-up's "give it
            // back" is Actuation's own explicit placement (PostHandEquipsDeferred),
            // which needs no re-declaration: the item is already in-set.
            // The key is (sorted entries, owned, denied): a scope-only change is
            // sent too. SCOPE FIRST, then the set, in one call -- two enqueues,
            // normally applied in APMF's same Drain (one Publish, both enforce
            // hops after it). A Drain CAN land between them (Fable F3 on 7857446,
            // MFO-B45): then the seat sees the new scope with the old set for one
            // frame, self-healing on the next Drain -- accepted, never a stale
            // scope over a NEW set (the order is fixed).
            SentDecl sent;
            sent.keys.reserve(decl.entries.size());
            for (const auto& e : decl.entries) {
                sent.keys.emplace_back(e.form, e.slot);
                if (const auto uid = APMF_API::EquipEntryUid(e); uid != 0) sent.uids.emplace_back(e.form, uid);
            }
            std::sort(sent.keys.begin(), sent.keys.end());
            std::sort(sent.uids.begin(), sent.uids.end());
            sent.owned  = owned;
            sent.denied = denied;
            // ... EXCEPT that "unchanged" must not mean "already on his body"
            // (MFO-B63, MFO-B41's shape generalised from the weapon ledger to
            // armor). A declared ARMO that is NOT WORN at this service is a piece
            // APMF was told to put on and did not: the enforcement hop never ran,
            // or it ran into a 3D-absent actor, or the engine took the piece back
            // off. That is a CHANGE on the body even though the set compares equal,
            // and swallowing it is what left a follower bare for 2 min 36 s while
            // MFO re-picked the right robes seven times. So: re-send, throttled to
            // kDeclDriftHold since the last send, which is also the "nothing is in
            // flight" test (there is no APMF query for it -- see g_lastDeclSentAt).
            // A piece he no longer owns is skipped: the next rebuild drops it
            // anyway, and it would otherwise re-send forever. ARMO keys only --
            // weapons and ammo are MFO-B41's business, not this detector's.
            // OBSERVE MODE IS EXEMPT: with bEquipObserveOnly APMF equips non-forced
            // and the engine is free to take the piece straight back off, so
            // "declared but not worn" is the DESIGNED state there and re-sending
            // would only churn the log and the engine ops it warns about above.
            if (auto it = g_lastDeclared.find(id); it != g_lastDeclared.end() && it->second == sent) {
                bool resend = false;
                if (APMFBridge::IsEquipAuthorityEnforced()) {
                    RE::TESObjectARMO* adrift = nullptr;
                    for (const auto& k : sent.keys) {
                        auto* ar = RE::TESForm::LookupByID<RE::TESObjectARMO>(k.first);
                        if (!ar) continue;                                          // not an ARMO: skip
                        const auto iit = inv.find(ar);
                        if (iit == inv.end() || iit->second.first <= 0) continue;    // not owned any more
                        // Review F1 (ABI v25): a key with a uid is worn only if THAT copy is; no list with the uid ->
                        // skip (the next ChooseJewelry re-chooses). uid 0: any copy worn, as before.
                        std::uint16_t uid = 0;
                        for (const auto& u : sent.uids) if (u.first == k.first) { uid = u.second; break; }
                        const DeclWorn w = DeclaredCopyWorn(inv, ar, uid);
                        if (w != DeclWorn::kNo) continue;
                        adrift = ar;
                        break;
                    }
                    const auto at = g_lastDeclSentAt.find(id);
                    resend = adrift != nullptr &&
                             (at == g_lastDeclSentAt.end() ||
                              std::chrono::steady_clock::now() - at->second >= kDeclDriftHold);
                    if (resend)
                        spdlog::info("[equip-auth] {:08X}: declared '{}' is not worn -- re-declaring (MFO-B63)",
                                     id, adrift->GetName() ? adrift->GetName() : "?");
                }
                if (!resend) return;
            }
            if (!APMFBridge::DeclareEquipScope(id, owned, denied) ||        // logged in the bridge; retried next refresh
                !APMFBridge::DeclareEquipSet(id, decl.entries)) {
                g_lastDeclared.erase(id);
                return;
            }
            g_lastDeclared[id] = std::move(sent);
            g_deferLogged.erase(id);
            g_lastDeclSentAt[id] = std::chrono::steady_clock::now();   // MFO-B63: the drift throttle's clock
            spdlog::info("[equip-auth] {:08X}: declare n={} [{}] owned={} denied={} ({}{})", id,
                         decl.entries.size(), decl.names, CategoryNames(owned), CategoryNames(denied),
                         a_why ? a_why : "?", fresh ? ", new claim" : "");
        }

}
