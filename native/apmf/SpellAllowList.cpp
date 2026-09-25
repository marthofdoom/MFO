// apmf/SpellAllowList.cpp -- the ch.8 CAST-SELECT candidate refusal: the
// gate-only claim that carries the follower's spell allow-list
// (PublishSpellAllowList / ReleaseSpellAllowList).
// Split out of the old native/APMFBridge.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The bridge's shared claim state lives in apmf/APMFBridge_internal.h.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // wave-1 split: the bridge's shared claim state
#include "APMF_API.h"
#include "ComposedCast.h"   // ObservedFiring (the idle-hand floor's unobserved gate) + ClearWatchHand
#include "cast/Actuation.h"     // CastInFlightOnHand -- THE one in-flight definition; the idle-hand
                           // floor's gate is armed on CHARGE, not on claim age alone (2026-09-22)
                            // (the expiry sweep drops the swept claim's [cfc] watch) -- both called
                            // from Tick() ONLY, which runs inside the AddTask job-worker body
                            // (Diagnostics.cpp) = ComposedCast's own serialized context; ComposedCast
                            // never takes g_mx, so calling it under g_mx cannot deadlock.
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
#include "MainThread.h"
#include "Packages.h"   // kMaxLootSlots -- the ch.19 loot-travel leg table is keyed by loot SLOT
#include "Rapport.h"

#include <array>   // F10: the two-slot (driving / idle-hand floor) log throttles
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>   // ch.17 claim-refusal throttle
#include <algorithm>       // ch.8 allow-list: sort + unique over the published form set

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    // ── ch.8 CAST-SELECT CANDIDATE REFUSAL (ABI v4; fix/mfo-spell-authority-0922) ──
    // APMFBridge.h's block doc carries the whole design, the field evidence and the
    // APMF-side verification. This is the plumbing.
    namespace {

        // Is the gate available AT ALL? Same three reads every caller below makes,
        // in one place, so a `false` from PublishSpellAllowList with this true can
        // only mean APMF refused the claim. ABI FLOOR 4 (SetSpellAllowList): below
        // it a gate-only claim would be `form==0 && allowCount==0`, which APMF
        // reads as "channel default -> ALLOW" -- a claim that denies nothing, so
        // none is made. Also inert with the cast facet off, so the one global
        // bApmfCast kill switch still turns every ch.8/ch.8b claim in this file off
        // together.
        bool SpellAllowListUsable() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || !Config::g_apmfSpellAllowList.load() || !Config::g_apmfCast.load()) return false;
            if (api->abiVersion < 4) {
                static std::atomic<bool> s_warnedSelAbi{ false };
                if (!s_warnedSelAbi.exchange(true))
                    spdlog::warn("[cast-select] APMF ABI v{} has no SetSpellAllowList (need >= 4) -- the "
                                 "candidate-refusal gate is OFF: no ch.8 claim is made at all (a gate-only "
                                 "claim with no allow-set is 'channel default -> ALLOW' on APMF's side, so "
                                 "it would deny nothing). MFO's cast-time deny carries the load, exactly as "
                                 "before this gate existed.", api->abiVersion);
                return false;
            }
            return true;
        }

        // EVERYTHING CasterConsent's own continuous deny DELIBERATELY EXEMPTS,
        // enumerated onto the allow-list. CtrlUnlatchedDeny is NORMAL SPELLS ONLY
        // (`Is(FormType::Spell)` + `GetSpellType() == kSpell`), and APMF's allowance
        // is a pure FormID-set test that cannot be told "spells only" -- so the
        // exempt forms have to be named or MFO's own gate would deny what MFO's own
        // deny lets through (the "our own deny bites us" failure, twice paid for).
        //
        // WHAT IS ENUMERATED, and why exactly this:
        //   * STAVES carried in the inventory, AND each staff's enchantment
        //     (TESEnchantableForm::formEnchanting). APMF's t2a is installed on 15
        //     CombatInventoryItemStaff templates as well as the 15 magic ones and
        //     sees the STAFF's own FormID there (EquipGate.cpp: subjectForm =
        //     a_this->item->GetFormID()), while t2c's CheckCast sees the MagicItem
        //     the staff casts -- two different FormIDs for one staff, so both go on.
        //   * SCROLLS carried in the inventory (ScrollItem, SpellType kScroll).
        //   * COMBAT POTIONS carried in the inventory -- every AlchemyItem that is
        //     NOT food and NOT poison. THIS ONE IS NOT PRECAUTIONARY: the exact
        //     own-goal has already SHIPPED ONCE. Docs/ENGINE_NOTES.md:1759-1762,
        //     verbatim: *"The deny was suppressing combat potions.
        //     `CombatMagicCasterRestore` is also the drink-potion caster (§0.29
        //     scope fact, now load-bearing): the unconditional !isWanted deny
        //     covered a latched follower's own combat drinking. v1.0.32 denies only
        //     `formType == Spell`."* So a follower's combat drinking DOES deliberate
        //     through a hooked caster, it was field-observed being suppressed, and
        //     `formType == Spell` is the ONLY reason MFO's own deny stopped doing it.
        //     APMF's allowance cannot be told "spells only", so a potion has to be
        //     NAMED or this gate re-creates a regression we already paid for -- and
        //     marth's `act.drink_health_potion` gambit depends on it working.
        //     Food and poison are excluded because neither is drunk through this
        //     road (a poison is applied to a weapon, not cast) and both would only
        //     spend the 32-form budget. `IsFood()` is vfunc 5D and `IsPoison()` is
        //     vfunc 61 on MagicItem, both overridden by AlchemyItem (pinned
        //     include/RE/A/AlchemyItem.h:71-72); `AlchemyItem : MagicItem` at offset
        //     000 (:15-16) so it is a TESBoundObject, and it is registered in
        //     FormTraits.h:198 so `As<AlchemyItem>()` is a valid form-type switch.
        //     `IsMedicine()` (vfunc 62) is deliberately NOT used as the filter: it
        //     is an authored flag a mod potion can simply lack, and a missing flag
        //     would put us straight back to denying someone's healing potion.
        //   * POWERS / LESSER POWERS / VOICE POWERS the actor knows, from the base
        //     spell list (TESNPC::GetSpellList(), verified against pinned
        //     CommonLibSSE-NG 3.7.0 TESSpellList.h: `SpellItem** spells` +
        //     `std::uint32_t numSpells`) and from the runtime-added list
        //     (ACTOR_RUNTIME_DATA::addedSpells, Actor.h:666). kAbility is
        //     DELIBERATELY NOT enumerated: an ability is passive, never selected
        //     and never cast, and a heavily-modded follower's ability list alone
        //     would exhaust the 32-form budget and fail the whole gate open.
        //
        // NOT ENUMERATED, with the proof rather than a hope: MFO's OWN ConcProxy
        // delivery-spell pool (Actuation_Direct.cpp). Those are IFormFactory-created
        // 0xFF SpellItems that are never added to any actor's spell list, so
        // CombatInventory can never build a candidate for one and t2a can never see
        // one; and the road that casts them is GetMagicCaster(kInstant)->
        // CastSpellImmediate (MagicCaster vtable slot 01) while APMF's gates hook
        // CheckCast (slot 0A) and CheckShouldEquip (slot 0F) -- so t2c never sees
        // one either. Verified against pinned CommonLibSSE-NG include/RE/M/
        // MagicCaster.h (slot 01 vs slot 0A), the same verification APMFBridge.cpp's
        // idle-hand floor note already rests on.
        //
        // `a_outPotions` (may be null) receives how many of the appended forms were
        // COMBAT POTIONS, so the overflow error below can say whether a follower lost
        // the gate to his own alchemy hoard rather than leaving the field to guess.
        void AppendDenyExemptForms(RE::Actor* a_actor, std::vector<RE::FormID>& out,
                                   std::uint32_t* a_outPotions = nullptr) {
            if (a_outPotions) *a_outPotions = 0;
            if (!a_actor) return;
            const auto add = [&out](RE::FormID f) { if (f != 0) out.push_back(f); };
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* w = obj->As<RE::TESObjectWEAP>(); w && w->IsStaff()) {
                    add(w->GetFormID());
                    if (auto* ench = w->formEnchanting) add(ench->GetFormID());
                    continue;
                }
                if (auto* sc = obj->As<RE::ScrollItem>()) { add(sc->GetFormID()); continue; }
                // Combat potions: not food, not poison. See the block doc above --
                // this is the v1.0.32 regression, guarded rather than re-paid for.
                if (auto* al = obj->As<RE::AlchemyItem>(); al && !al->IsFood() && !al->IsPoison()) {
                    add(al->GetFormID());
                    if (a_outPotions) ++*a_outPotions;
                }
            }
            const auto castable = [](const RE::SpellItem* s) {
                if (!s) return false;
                const auto t = s->GetSpellType();
                return t == RE::MagicSystem::SpellType::kPower ||
                       t == RE::MagicSystem::SpellType::kLesserPower ||
                       t == RE::MagicSystem::SpellType::kVoicePower;
            };
            if (auto* npc = a_actor->GetActorBase())
                if (auto* sl = npc->GetSpellList())
                    for (std::uint32_t i = 0; i < sl->numSpells; ++i)
                        if (sl->spells && castable(sl->spells[i])) add(sl->spells[i]->GetFormID());
            for (auto* s : a_actor->GetActorRuntimeData().addedSpells)
                if (castable(s)) add(s->GetFormID());
        }
    }

    bool PublishSpellAllowList(RE::FormID a_follower, const std::vector<RE::FormID>& a_spells) {
        if (a_follower == 0) return false;
        if (a_spells.empty() || !SpellAllowListUsable()) {
            ReleaseSpellAllowList(a_follower);   // idempotent; logs only when a claim stood
            return false;
        }
        // BUILD FIRST, OUTSIDE g_mx: the inventory walk and the spell-list walk are
        // engine reads, and holding a leaf lock across them would be gratuitous.
        // The proxies come from the map and are appended under the lock below.
        std::vector<RE::FormID> list = a_spells;
        std::uint32_t potionCount = 0;
        AppendDenyExemptForms(RE::TESForm::LookupByID<RE::Actor>(a_follower), list, &potionCount);

        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        // APMF's OWN delivery-flip PROXIES for this follower's live cast claims.
        // Both seats already exempt a ch.8b claim's spell-or-proxy BEFORE they
        // consult this channel, so this is belt-and-braces for a proxy on a caster
        // the seat cannot resolve to a hand (an instant/kOther caster degrades to
        // the actor-wide read, where the per-hand exemption does not fire).
        for (const auto& c : { o.offense[0], o.offense[1], o.heal })
            if (c.proxy != 0) list.push_back(c.proxy);
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());

        // OVERFLOW FAILS OPEN AND LOUD -- NEVER TRUNCATES (#7). APMF's documented
        // degrade for a longer list is "the excess are treated as non-exempt", i.e.
        // DENIED, which on this channel would silently disarm the follower. So a
        // list that does not fit means NO GATE for this follower at all, said once
        // per follower per overflow streak, and the cast-time deny carries the load
        // exactly as it did before this gate existed.
        if (list.size() > APMF_API::kMaxSpellAllowList) {
            if (g_selectOverflow.insert(a_follower).second)
                spdlog::error("[cast-select] {:08X}: allow-list needs {} forms, over kMaxSpellAllowList {} "
                              "-- the gate is NOT claimed for this follower (a truncated list would DENY "
                              "the excess and disarm him, and could deny his POTIONS). {} gambit spell(s) "
                              "+ {} combat potion(s) + the deny-exempt staves/scrolls/powers + live "
                              "proxies. MFO's cast-time deny still applies, so nothing is muted -- but "
                              "the candidate-refusal gate is INERT for this follower until the list fits "
                              "(a large alchemy hoard is the usual cause; see Docs/REVIEW-BACKLOG.md "
                              "MFO-B65).",
                              a_follower, list.size(), APMF_API::kMaxSpellAllowList, a_spells.size(),
                              potionCount);
            // Release inside the lock we already hold, not through the public
            // function (which would re-lock).
            if (o.selectHandle != APMF_API::kInvalidHandle) {
                RE::FormID dummy = 0;
                ReleaseHandleLocked(o.selectHandle, dummy);
                o.selectList.clear();
            }
            EraseIfEmpty(g_owned.find(a_follower));
            return false;
        }
        g_selectOverflow.erase(a_follower);

        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) {   // raced Acquire/unload between the check above and here
            EraseIfEmpty(g_owned.find(a_follower));   // `o` may be the entry we just default-created
            return false;
        }
        bool freshClaim = false;
        if (o.selectHandle != APMF_API::kInvalidHandle) {
            // Same F2 re-validation the ch.17 standing claim does, and for the same
            // reasons (APMF's release-all hotkey, its unload sweep, a New Game that
            // reused the base FormIDs): a handle APMF has forgotten makes
            // SetSpellAllowList a silent no-op. ABI >= 6 only -- IsClaimLive is a v6
            // slot. Failing to validate is the SAFE direction here (no claim on
            // APMF's side == ALLOW), so this is correctness, not safety.
            const auto now = std::chrono::steady_clock::now();
            if (api->abiVersion >= 6 && now - o.selectMintedAt >= kEquipAuthValidateAfter &&
                !reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(o.selectHandle)) {
                spdlog::warn("[cast-select] {:08X}: gate-only claim handle {} is no longer live on APMF's "
                             "side (released or swept there; SetSpellAllowList on it would be a silent "
                             "no-op) -- re-minting", a_follower, o.selectHandle);
                o.selectHandle = APMF_API::kInvalidHandle;   // no Release: APMF already forgot it
                o.selectList.clear();
            }
        }
        if (o.selectHandle == APMF_API::kInvalidHandle) {
            // GATE-ONLY: param.form stays 0. The claim names no spell and drives
            // nothing -- every ALLOW comes from the allow-set attached below, so
            // there is exactly ONE place that decides what this follower may use.
            APMF_API::APMF_Param p{};
            o.selectHandle = api->RequestEx(a_follower, APMF_API::kIntent_SelectSpell, kOwnBasis, &p);
            if (o.selectHandle == APMF_API::kInvalidHandle) {
                if (g_selectRefused.insert(a_follower).second)
                    spdlog::warn("[cast-select] {:08X}: APMF refused the gate-only kIntent_SelectSpell "
                                 "claim (RequestEx returned kInvalidHandle) -- no candidate refusal for "
                                 "this follower; MFO's cast-time deny carries the load.", a_follower);
                o.selectList.clear();
                EraseIfEmpty(g_owned.find(a_follower));
                return false;
            }
            g_selectRefused.erase(a_follower);
            o.selectMintedAt = std::chrono::steady_clock::now();
            o.selectList.clear();          // a fresh handle carries no allow-set
            freshClaim = true;
        }
        o.selectRefreshed = std::chrono::steady_clock::now();
        if (o.selectList == list) return true;   // unchanged -- one compare, no enqueue

        // COPIED inside the call (APMF_API.h: read synchronously, never retained);
        // a stack/heap temporary is the documented shape.
        reinterpret_cast<const APMF_API::APMF_API_v4*>(api)->SetSpellAllowList(
            o.selectHandle, list.data(), static_cast<std::uint32_t>(list.size()));
        o.selectList = list;
        // ONE LINE PER CLAIM/UPDATE/RELEASE, naming the count and the forms -- the
        // field has to be able to read "what was this follower allowed to use" off
        // the log without re-deriving it from the gambit table.
        std::string forms;
        forms.reserve(list.size() * 9);
        for (const auto f : list) {
            if (!forms.empty()) forms += ' ';
            forms += std::format("{:08X}", f);
        }
        spdlog::info("[cast-select] {:08X}: allow-list {} n={} ({} gambit + {} potion + exempt/proxy) "
                     "[{}] -- gate-only ch.8 claim {}, so the AI may ONLY equip or charge these",
                     a_follower, freshClaim ? "CLAIMED" : "updated", list.size(), a_spells.size(),
                     potionCount, forms, o.selectHandle);
        return true;
    }

    void ReleaseSpellAllowList(RE::FormID a_follower) {
        if (a_follower == 0) return;
        std::scoped_lock lock(g_mx);
        g_selectOverflow.erase(a_follower);
        g_selectRefused.erase(a_follower);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        if (it->second.selectHandle != APMF_API::kInvalidHandle) {
            RE::FormID dummy = 0;
            ReleaseHandleLocked(it->second.selectHandle, dummy);
            spdlog::info("[cast-select] {:08X}: allow-list RELEASED -- the AI's own spell selection is "
                         "its own again", a_follower);
        }
        it->second.selectList.clear();
        EraseIfEmpty(it);
    }

    bool IsSpellAllowListClaimed(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.selectHandle != APMF_API::kInvalidHandle;
    }

}
