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
        // COMBAT POTIONS are NOT appended to `out`: they go to `a_potions` as
        // candidates, because they are the one part of the list that may be TRIMMED
        // to fit (SelectPotions below; field 2026-09-26: Serana carried 35-46 potions,
        // needed 38-50 forms and lost the whole gate). Everything else is appended.
        struct PotionCand {
            RE::FormID    form = 0;
            std::uint64_t cat  = 0;   // (archetype, primary AV) of the costliest effect
            int           prio = 3;   // 0 restore Health, 1 Magicka, 2 Stamina, 3 everything else
            float         mag  = 0.0f;
            std::uint32_t dur  = 0;
            std::int32_t  gold = 0;
        };

        PotionCand MakePotionCand(RE::AlchemyItem* a_al) {
            PotionCand c;
            c.form = a_al->GetFormID();
            c.gold = a_al->GetGoldValue();
            const auto* eff  = a_al->GetCostliestEffectItem();
            const auto* mgef = eff ? eff->baseEffect : nullptr;
            if (!eff || !mgef) return c;   // cat 0: an effect-less potion is its own category
            const auto arch = mgef->data.archetype;
            const auto av   = mgef->data.primaryAV;
            c.cat = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(arch)) << 32) |
                    static_cast<std::uint32_t>(av);
            c.mag = eff->GetMagnitude();
            c.dur = eff->GetDuration();
            using Arch = RE::EffectArchetypes::ArchetypeID;
            if (arch == Arch::kValueModifier || arch == Arch::kDualValueModifier) {
                if (av == RE::ActorValue::kHealth) c.prio = 0;
                else if (av == RE::ActorValue::kMagicka) c.prio = 1;
                else if (av == RE::ActorValue::kStamina) c.prio = 2;
            }
            return c;
        }

        // THE POTION TRIM (marth 2026-09-27: "keep the best available potion in each
        // category within the 32 cap", no limit raise). All of them when they fit.
        // Otherwise: a CATEGORY is the costliest effect's (archetype, primary AV), so
        // Restore Health, Fortify One-handed and Resist Fire are three categories;
        // BEST inside one = magnitude, then duration, then value. Kept round-robin by
        // rank -- every category's best first, then every category's second best --
        // until the budget is spent; inside a rank the restore Health / Magicka /
        // Stamina categories come first, then by the category best's value. A potion
        // left off the list is one his AI cannot choose in combat (APMF denies what
        // the allow-set does not name); MFO's own drink gambits equip a potion
        // directly (ActorEquipManager) and are not on this channel.
        std::vector<RE::FormID> SelectPotions(std::vector<PotionCand> a_cands, std::size_t a_budget) {
            std::vector<RE::FormID> kept;
            if (a_cands.size() <= a_budget) {
                for (const auto& c : a_cands) kept.push_back(c.form);
                return kept;
            }
            std::unordered_map<std::uint64_t, std::vector<PotionCand>> byCat;
            for (const auto& c : a_cands) byCat[c.cat].push_back(c);
            std::vector<std::vector<PotionCand>*> order;
            for (auto& [cat, v] : byCat) {
                std::sort(v.begin(), v.end(), [](const PotionCand& a, const PotionCand& b) {
                    if (a.mag != b.mag) return a.mag > b.mag;
                    if (a.dur != b.dur) return a.dur > b.dur;
                    if (a.gold != b.gold) return a.gold > b.gold;
                    return a.form < b.form;
                });
                order.push_back(&v);
            }
            std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
                const auto& x = a->front();
                const auto& y = b->front();
                if (x.prio != y.prio) return x.prio < y.prio;
                if (x.gold != y.gold) return x.gold > y.gold;
                return x.cat < y.cat;
            });
            for (std::size_t rank = 0; kept.size() < a_budget; ++rank) {
                bool any = false;
                for (auto* v : order) {
                    if (rank >= v->size()) continue;
                    any = true;
                    if (kept.size() >= a_budget) break;
                    kept.push_back((*v)[rank].form);
                }
                if (!any) break;
            }
            return kept;
        }

        void AppendDenyExemptForms(RE::Actor* a_actor, std::vector<RE::FormID>& out,
                                   std::vector<PotionCand>& a_potions) {
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
                if (auto* al = obj->As<RE::AlchemyItem>(); al && !al->IsFood() && !al->IsPoison())
                    a_potions.push_back(MakePotionCand(al));
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
        std::vector<PotionCand> potions;
        AppendDenyExemptForms(RE::TESForm::LookupByID<RE::Actor>(a_follower), list, potions);
        std::uint32_t potionCount = 0;   // potions KEPT on the list (set below)

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
        // exactly as it did before this gate existed. POTIONS are the exception by
        // design (SelectPotions: the best of each category is kept in the budget the
        // other forms leave), so this path now fires only when the NON-potion forms
        // alone (gambit spells, staves, scrolls, powers, proxies) do not fit.
        if (list.size() > APMF_API::kMaxSpellAllowList) {
            if (g_selectOverflow.insert(a_follower).second)
                spdlog::error("[cast-select] {:08X}: allow-list needs {} forms WITHOUT any potion, over "
                              "kMaxSpellAllowList {} -- the gate is NOT claimed for this follower (a "
                              "truncated list would DENY the excess and disarm him). {} gambit spell(s) "
                              "+ the deny-exempt staves/scrolls/powers + live proxies ({} combat "
                              "potion(s) carried, none fit). MFO's cast-time deny still applies, so "
                              "nothing is muted -- but the candidate-refusal gate is INERT for this "
                              "follower until the list fits (see Docs/REVIEW-BACKLOG.md MFO-B65).",
                              a_follower, list.size(), APMF_API::kMaxSpellAllowList, a_spells.size(),
                              potions.size());
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
        {
            const auto kept = SelectPotions(potions, APMF_API::kMaxSpellAllowList - list.size());
            potionCount = static_cast<std::uint32_t>(kept.size());
            list.insert(list.end(), kept.begin(), kept.end());
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }

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
        spdlog::info("[cast-select] {:08X}: allow-list {} n={} ({} gambit + {} of {} potion(s){} + "
                     "exempt/proxy) [{}] -- gate-only ch.8 claim {}, so the AI may ONLY equip or charge these",
                     a_follower, freshClaim ? "CLAIMED" : "updated", list.size(), a_spells.size(),
                     potionCount, potions.size(),
                     potionCount < potions.size() ? ", trimmed to the best of each category by the cap" : "",
                     forms, o.selectHandle);
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
