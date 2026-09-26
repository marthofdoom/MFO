// logistics/EquipAuthority.cpp -- wear the best OWNED gear, and the APMF EQUIP
// AUTHORITY declaration (scope + worn set, player picks, bound weapons).
// Split out of the old native/Logistics*.cpp by the wave-2 subsystem-folder split
// (2026-09-25): a pure move, proven function by function with tools/splitcheck.
#include "Logistics_internal.h"
#include "apmf/APMFBridge.h"   // feat/mfo-equip-authority: the ch.17 claim + SetEquipSet declaration
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
        // once worn it no-ops -> no thrash. Apparel/armor only here; WEAPONS are left
        // to the combat equip gambit (Loadout::Prepare picks the best in-class at
        // combat), matching the loot judge's stock-not-equip rule for weapons.
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
            const bool caster         = IsCasterFollower(a_state);
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
                    if (!ArmorIsBetter(armorPref, a_follower, ar)) continue;   // strictly higher score than worn on its slot
                    const float sc = ArmorScore(armorPref, ar);
                    if (sc > bestScore) { bestScore = sc; pick = obj; }
                }
            }

            if (!pick || !FitsCarryWeight(a_follower, pick->GetWeight())) return nullptr;
            return pick;
        }

        void EquipBestOwnedGear(RE::Actor* a_follower, const FollowerState& a_state) {
            if (!a_follower || Config::g_dollsMode.load()) return;
            auto* eqObj  = a_follower->GetEquippedObject(false);
            auto* myWeap = eqObj ? eqObj->As<RE::TESObjectWEAP>() : nullptr;
            ArmorPref armorPref;
            bool      useMageApparel = false;
            RE::TESBoundObject* pick = ComputeOwnedGearPick(a_follower, a_state, armorPref, useMageApparel);
            g_handedPick = { a_follower->GetFormID(), pick ? pick->GetFormID() : 0, true };   // F10: for the declaration guard
            if (!pick) return;
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
                             declared ? " | declared (APMF equip authority)" : "");
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
                bool operator==(const SentDecl&) const = default;
            };
            std::unordered_map<RE::FormID, SentDecl> g_lastDeclared;

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
        }

        bool EquipAuthorityLive(RE::FormID a_follower) {
            return APMFBridge::EquipAuthoritySupported() && APMFBridge::IsEquipAuthorityClaimed(a_follower);
        }

        void ForgetEquipDeclaration(RE::FormID a_follower) {
            g_lastDeclared.erase(a_follower); g_playerPicks.erase(a_follower); g_lastDeclSentAt.erase(a_follower);
        }
        void ResendEquipDeclaration(RE::FormID a_follower) { g_lastDeclared.erase(a_follower); }   // MFO-B41: the change detector only
        void ClearEquipDeclarations() { g_lastDeclared.clear(); g_playerPicks.clear(); g_lastDeclSentAt.clear(); }

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
            if (!APMFBridge::ClaimEquipAuthority(id, &fresh)) {
                // APMF REFUSED (its equip seat is not installed): the bridge logged
                // it once; IsEquipAuthorityClaimed is false, so every direct equip
                // path runs for this follower exactly as without APMF (F1/F5).
                g_lastDeclared.erase(id);
                return;
            }
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
            RE::TESBoundObject* right = resolve(a_holdRight);
            RE::TESBoundObject* left  = resolve(holdLeft);
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
            const std::uint32_t denied = (roles.offHand == 2 && Config::g_weaponStyleControl.load())
                                           ? static_cast<std::uint32_t>(APMF_API::kEquipCat_Shield) : 0u;
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
                if (worn && APMFBridge::IsEquipAuthorityEnforced() && ammoWasOwned &&
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
            if (a_judgeArmor) {
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
            const bool mayRecordPlayerPick = APMFBridge::IsEquipAuthorityEnforced() && (!pick || pickWorn);
            if (mayRecordPlayerPick) {
                for (auto& [obj, data] : inv) {
                    if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                    auto* ar = obj->As<RE::TESObjectARMO>();
                    if (!ar || ar->IsShield() || ar == pick) continue;
                    const RE::FormID f = ar->GetFormID();
                    if (lastHas(f) || picks.count(f)) continue;
                    picks.insert(f);
                    spdlog::info("[equip-auth] {:08X}: player put on '{}' -- kept", id,
                                 ar->GetName() ? ar->GetName() : "?");
                }
            }
            if (pick && !picks.empty()) {
                const bool caster = IsCasterFollower(a_state);
                if (caster && Config::g_mageWearRobes.load()) {
                    // Mage judge: keep the player's worn piece on a tie of (tier, metric).
                    const std::uint8_t top2 = TopTwoSchoolMask(a_follower);
                    const bool schoolPrimary = !MEOBridge::Available() || Config::g_mageApparelStrictSchool.load();
                    int pt = 0; std::int32_t pm = 0;
                    const bool pickRanks = MageApparelBuyKey(pick, top2, schoolPrimary, IsNecromancerFollower(a_state), pt, pm);
                    for (auto& [obj, data] : inv) {
                        if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                        auto* ar = obj->As<RE::TESObjectARMO>();
                        if (!ar || ar == pick || !picks.count(ar->GetFormID()) || !SlotsOverlap(ar, pick)) continue;
                        int wt = 0; std::int32_t wm = 0;
                        MageApparelBuyKey(ar, top2, schoolPrimary, true, wt, wm);
                        const bool strictlyBetter = pickRanks && (pt > wt || (pt == wt && pm > wm));
                        if (!strictlyBetter) { pick = nullptr; break; }   // the player's piece stands
                    }
                }
                // Rated judge: ArmorIsBetter already demands a strictly higher score than the worn piece.
            }
            decl.Add(pick);

            // 5. EVERYTHING ELSE WORN that is apparel, minus what the pick displaces.
            //    Shields are never declared (rule 3: an unowned category).
            for (auto& [obj, data] : inv) {
                if (!obj || data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                auto* ar = obj->As<RE::TESObjectARMO>();
                if (!ar || ar->IsShield()) continue;
                if (pick && ar != pick && SlotsOverlap(ar, pick)) continue;
                decl.Add(ar);
            }

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
            for (const auto& e : decl.entries) sent.keys.emplace_back(e.form, e.slot);
            std::sort(sent.keys.begin(), sent.keys.end());
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
                        if (iit->second.second && iit->second.second->IsWorn()) continue;
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
            g_lastDeclSentAt[id] = std::chrono::steady_clock::now();   // MFO-B63: the drift throttle's clock
            spdlog::info("[equip-auth] {:08X}: declare n={} [{}] owned={} denied={} ({}{})", id,
                         decl.entries.size(), decl.names, CategoryNames(owned), CategoryNames(denied),
                         a_why ? a_why : "?", fresh ? ", new claim" : "");
        }

}
