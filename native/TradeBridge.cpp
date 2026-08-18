#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include "Config.h"
#include "ItemCatalog.h"
#include "Logistics.h"   // Logistics::PotionRestores -- the SAME classifier CountPotions uses
#include "Vocabulary.h"  // Vocab::IsCastableSpell -- the one gate for what counts as a spell
#include <mutex>
#include <unordered_set>

namespace MFO::TradeBridge {

    namespace {

        struct TradeOrder {
            RE::ActorHandle      follower;
            RE::ActorHandle      vendor;
            RE::ObjectRefHandle  chest;
            RE::FormID           chestId = 0;  // for the per-chest in-flight guard
            std::chrono::steady_clock::time_point dispatchedAt{};  // for the stale-order reap
            std::vector<SellRow> sell;
            std::vector<NeedCat> needs;      // mutated by PlanBuy as quotas fill
            std::int32_t         budget = 0; // follower purse; PlanBuy spends it down
            std::int32_t         buySpent = 0;
            bool                 probeOnly = true;
            // Taught-spell FormIDs the follower ALREADY carries a tome for (collected
            // worker-side). PlanBuy skips buying a second copy before the first is
            // learned. Empty unless bFollowerBuySpells.
            std::unordered_set<RE::FormID> ownedTomeSpells;
        };

        std::mutex                                    g_mtx;
        std::unordered_map<std::int32_t, TradeOrder>  g_orders;
        std::atomic<std::int32_t>                     g_nextToken{ 1 };

        TradeOrder* Find(std::int32_t a_token) {   // caller holds g_mtx
            auto it = g_orders.find(a_token);
            return it == g_orders.end() ? nullptr : &it->second;
        }

        int BaseValue(RE::TESForm* a_form) {
            auto* vf = a_form ? a_form->As<RE::TESValueForm>() : nullptr;
            return vf ? vf->value : 0;
        }

        // Which buy category is this stock form, if any? -1 = not a supply the
        // gambits ask for. Classification is catalog-first (the Synthesis patcher
        // read the real records), so it needs a current mfo_items.json.
        std::int32_t ClassifyBuy(RE::TESForm* a_form) {
            if (!a_form) return -1;
            const auto id = a_form->GetFormID();
            if (auto* alc = a_form->As<RE::AlchemyItem>()) {
                // The SAME classifier the follower counts/drinks with (catalog-first,
                // then the archetype heuristic) -- catalog-only left the vendor's
                // health potions unclassified, so a follower that NEEDED potions (its
                // count uses this classifier) bought none (field: Arcadia, BUY 0).
                const auto rv = Logistics::PotionRestores(alc);
                if (rv != RE::ActorValue::kHealth && rv != RE::ActorValue::kStamina &&
                    rv != RE::ActorValue::kMagicka)
                    return -1;
                // IGNORE LOW POWER on the BUY side too (P5): loot skips weak restore
                // potions, so buying them would contradict the rule. Same floor.
                if (const float f = Logistics::PotionLootFloor();
                    f > 0.0f && Logistics::PotionMagnitude(alc) > 0.0f &&
                    Logistics::PotionMagnitude(alc) < f)
                    return -1;
                switch (rv) {
                    case RE::ActorValue::kHealth:  return NeedCat::kPotHealth;
                    case RE::ActorValue::kStamina: return NeedCat::kPotStamina;
                    default:                       return NeedCat::kPotMagicka;
                }
            }
            if (auto* am = a_form->As<RE::TESAmmo>()) {
                // Catalog-first, then the shared IsBolt() fallback -- so uncatalogued
                // vendor ammo still classifies and gets bought (P5).
                return Logistics::AmmoIsBolt(am) ? NeedCat::kBolts : NeedCat::kArrows;
            }
            // SPELL TOME (town-update follower-buy-spells). Form-only classification;
            // the per-FOLLOWER gate (skill tier / magicka / already-known) is applied
            // in PlanBuy where the follower handle is in hand. IGNORED entirely unless
            // the scan actually asked for kSpellTome (feature gated in the econ scan).
            if (auto* book = a_form->As<RE::TESObjectBOOK>()) {
                if (book->TeachesSpell()) {
                    auto* sp = book->data.teaches.spell;
                    if (sp && MFO::Vocab::IsCastableSpell(sp)) return NeedCat::kSpellTome;
                }
            }
            return -1;
        }

        // Per-FOLLOWER spell-tome gate (Parts 1+3 of the town-update spec). True only
        // if this follower should actually acquire THIS tome: it teaches a castable
        // spell the follower does not already know or carry, the follower's OWN magic
        // skill meets the spell's minimum-skill tier (so a skilled mage follower can
        // buy a tier the player's skill-gated menu might have hidden -- MFO buys via
        // RemoveItem, not the barter menu), and the follower's magicka pool covers a
        // single cast. Gold is bounded separately by the shared purse in PlanBuy.
        bool FollowerCanUseTome(RE::Actor* a_fol, RE::TESForm* a_form,
                                const std::unordered_set<RE::FormID>& a_owned) {
            if (!a_fol || !a_form) return false;
            auto* book = a_form->As<RE::TESObjectBOOK>();
            if (!book || !book->TeachesSpell()) return false;
            auto* sp = book->data.teaches.spell;
            if (!sp || !MFO::Vocab::IsCastableSpell(sp)) return false;
            if (a_fol->HasSpell(sp))              return false;   // already knows it
            if (a_owned.count(sp->GetFormID()))   return false;   // already carries a tome for it
            auto* avo = a_fol->AsActorValueOwner();
            if (!avo) return false;
            // MAGICKA: pool must cover at least one cast (else uncastable -> pointless).
            const float cost = sp->CalculateMagickaCost(a_fol);
            const float maxMag = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                                 a_fol->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                              RE::ActorValue::kMagicka);
            if (cost > 0.0f && maxMag < cost) return false;
            // SKILL TIER: the spell's associated school skill on the FOLLOWER must meet
            // the effect's minimum-skill level (Novice 0 / Apprentice 25 / Adept 50 /
            // Expert 75 / Master 100). +0.5 guards float slop on the base AV read.
            if (auto* eff = sp->GetAVEffect()) {
                const int          minSkill = eff->GetMinimumSkillLevel();
                const RE::ActorValue school = sp->GetAssociatedSkill();
                if (minSkill > 0 && school != RE::ActorValue::kNone &&
                    avo->GetActorValue(school) + 0.5f < static_cast<float>(minSkill))
                    return false;
            }
            return true;
        }

        // ── Registered natives (called by MFO_Trade, on the VM thread) ────────
        RE::Actor* GetTradeFollower(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->follower.get().get() : nullptr;
        }

        RE::TESObjectREFR* GetVendorChest(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->chest.get().get() : nullptr;
        }

        RE::Actor* GetVendorActor(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->vendor.get().get() : nullptr;
        }

        bool GetProbeOnly(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->probeOnly : true;
        }

        std::vector<RE::TESForm*> GetSellForms(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<RE::TESForm*> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.obj); }
            return out;
        }
        std::vector<std::int32_t> GetSellCounts(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.count); }
            return out;
        }
        std::vector<std::int32_t> GetSellValues(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.value); }
            return out;
        }

        // BUY PLAN: given the vendor's ACTUAL stock (Papyrus enumerated it), decide
        // how many of each form to buy. "Best they can afford, up to the number
        // needed" (marth): rank the needed-category stock by value DESC and buy the
        // best first, filling each category's quota, bounded by the shared purse.
        // Returns qty[] parallel to a_forms; the spent gold is cached for
        // GetBuySpent. Mutates the order's quotas/budget (single-shot per trade).
        std::vector<std::int32_t> PlanBuy(RE::StaticFunctionTag*, std::int32_t a_token,
                                          std::vector<RE::TESForm*> a_forms,
                                          std::vector<std::int32_t> a_counts) {
            std::vector<std::int32_t> plan(a_forms.size(), 0);
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            if (!o || o->needs.empty()) return plan;

            // The follower is needed to gate spell-tome candidates (its own skill /
            // magicka / already-known). A null handle just skips the tome gate.
            RE::Actor* const fol = o->follower.get().get();

            // Candidate stock lines of a needed category, value-desc.
            struct Cand { std::size_t idx; std::int32_t kind, value, avail; };
            std::vector<Cand> cands;
            for (std::size_t i = 0; i < a_forms.size(); ++i) {
                auto* f = a_forms[i];
                const int avail = (i < a_counts.size()) ? a_counts[i] : 0;
                if (!f || avail <= 0) continue;
                if (Catalog::IsExcluded(f->GetFormID())) continue;
                const int kind = ClassifyBuy(f);
                if (kind < 0) continue;
                // SPELL TOME: only a candidate if THIS follower should actually
                // acquire it (Parts 1+3). Filters the tome to the follower's skill,
                // magicka, and not-already-known/carried -- so a warrior never buys a
                // Master tome and a mage never buys a spell it can't cast.
                if (kind == NeedCat::kSpellTome &&
                    !FollowerCanUseTome(fol, f, o->ownedTomeSpells)) continue;
                const int val = BaseValue(f);
                if (val <= 0) continue;   // free/valueless stock isn't a real buy
                cands.push_back(Cand{ i, kind, val, avail });
            }
            std::sort(cands.begin(), cands.end(),
                      [](const Cand& a, const Cand& b) { return a.value > b.value; });

            // Within one plan, never buy two tomes that teach the SAME spell (two
            // vendor lines of the same book, or two books of one spell).
            std::unordered_set<RE::FormID> plannedTomeSpells;

            for (const auto& c : cands) {
                if (o->budget < c.value) continue;   // can't afford even one
                // Remaining quota for this category.
                NeedCat* need = nullptr;
                for (auto& n : o->needs) if (n.kind == c.kind && n.quota > 0) { need = &n; break; }
                if (!need) continue;
                RE::FormID tomeSpell = 0;
                if (c.kind == NeedCat::kSpellTome) {
                    // One tome grants the spell for good -- never stock duplicates,
                    // and dedup same-spell lines within this plan.
                    auto* bk = a_forms[c.idx]->As<RE::TESObjectBOOK>();
                    auto* sp = bk && bk->TeachesSpell() ? bk->data.teaches.spell : nullptr;
                    tomeSpell = sp ? sp->GetFormID() : 0;
                    if (tomeSpell && plannedTomeSpells.count(tomeSpell)) continue;
                }
                int qty = c.avail;
                if (c.kind == NeedCat::kSpellTome) qty = 1;   // one copy is enough
                if (qty > need->quota)              qty = need->quota;
                if (qty > o->budget / c.value)      qty = o->budget / c.value;
                if (qty <= 0) continue;
                plan[c.idx] += qty;
                need->quota  -= qty;
                o->budget    -= qty * c.value;
                o->buySpent  += qty * c.value;
                if (tomeSpell) plannedTomeSpells.insert(tomeSpell);
            }
            return plan;
        }

        // Token-FREE base value of a form. Papyrus pays per-item with this so the
        // buy loop is self-contained (move + pay together) and survives a save that
        // lands mid-loop -- unlike a token'd total, which is gone after a load.
        std::int32_t GetFormValue(RE::StaticFunctionTag*, RE::TESForm* a_form) {
            return BaseValue(a_form);
        }

        // Papyrus reports the result; native logs it + frees the token.
        void ReportTrade(RE::StaticFunctionTag*, std::int32_t a_token,
                         std::int32_t a_soldValue, std::int32_t a_soldCount,
                         std::int32_t a_boughtCount, std::int32_t a_spent,
                         std::int32_t a_vendorGold) {
            TradeOrder o;
            {
                std::scoped_lock lk(g_mtx);
                auto it = g_orders.find(a_token);
                if (it == g_orders.end()) { spdlog::warn("[econ] ReportTrade: unknown token {}", a_token); return; }
                o = std::move(it->second);
                g_orders.erase(it);
            }
            if (a_vendorGold < 0) {
                spdlog::info("[econ] token {} -- Papyrus found no usable chest (aborted)", a_token);
                return;
            }
            auto* fol = o.follower.get().get();
            auto* ven = o.vendor.get().get();
            const char* verb = o.probeOnly ? "WOULD" : "did";
            spdlog::info("[econ] {:08X} '{}' @ '{}': chest gold={} | {} SELL {} for {}g, BUY {} for {}g "
                         "(sell n={}, needs={}, purse={})",
                         fol ? fol->GetFormID() : 0u,
                         fol && fol->GetName() ? fol->GetName() : "?",
                         ven && ven->GetName() ? ven->GetName() : "?",
                         a_vendorGold, verb, a_soldCount, a_soldValue, a_boughtCount, a_spent,
                         o.sell.size(), o.needs.size(), o.budget + o.buySpent);   // original purse
        }

    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) { spdlog::error("[trade] RegisterFuncs: no VM -- econ bridge disabled"); return false; }
        a_vm->RegisterFunction("GetTradeFollower", "MFO_Trade", GetTradeFollower);
        a_vm->RegisterFunction("GetVendorChest",   "MFO_Trade", GetVendorChest);
        a_vm->RegisterFunction("GetVendorActor",   "MFO_Trade", GetVendorActor);
        a_vm->RegisterFunction("GetProbeOnly",     "MFO_Trade", GetProbeOnly);
        a_vm->RegisterFunction("GetSellForms",     "MFO_Trade", GetSellForms);
        a_vm->RegisterFunction("GetSellCounts",    "MFO_Trade", GetSellCounts);
        a_vm->RegisterFunction("GetSellValues",    "MFO_Trade", GetSellValues);
        a_vm->RegisterFunction("PlanBuy",          "MFO_Trade", PlanBuy);
        a_vm->RegisterFunction("GetFormValue",     "MFO_Trade", GetFormValue);
        a_vm->RegisterFunction("ReportTrade",      "MFO_Trade", ReportTrade);
        spdlog::info("[trade] registered MFO_Trade natives (sell + buy bridge)");
        return true;
    }

    bool VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget,
                     std::unordered_set<RE::FormID> a_ownedTomeSpells) {
        auto* q = Forms::g_tradeQuest;
        if (!q || !q->IsRunning()) return false;
        if (!a_follower || !a_chest)  return false;

        const RE::FormID chestId = a_chest->GetFormID();
        std::int32_t token;
        const auto nowT = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk(g_mtx);
            // REAP stale orders first: a dispatch that the VM accepted but never ran
            // (dropped stack, broken .pex) would otherwise sit forever and, via the
            // per-chest guard below, block that merchant permanently. RunTrade
            // completes in well under a second; 30 s is a safe grave. (audit #8)
            for (auto it = g_orders.begin(); it != g_orders.end(); ) {
                if (nowT - it->second.dispatchedAt > std::chrono::seconds(30)) it = g_orders.erase(it);
                else ++it;
            }

            // PER-CHEST in-flight guard (Fable audit #2): never let two followers
            // hold a live order against the SAME merchant chest -- both would read
            // the same barter gold and the chest would pay out twice (gold minted
            // from nothing). The loser waits for the winner's order to complete
            // (ReportTrade frees it) and trades next scan.
            for (auto& [tok, o] : g_orders)
                if (o.chestId == chestId) return false;

            token = g_nextToken.fetch_add(1);
            TradeOrder o;
            o.follower  = a_follower->GetHandle();
            o.vendor    = a_vendor ? a_vendor->GetHandle() : RE::ActorHandle{};
            o.chest     = a_chest->GetHandle();
            o.chestId   = chestId;
            o.dispatchedAt = nowT;
            o.sell      = std::move(a_sell);
            o.needs     = std::move(a_needs);
            o.budget    = a_budget;
            o.probeOnly = !Config::g_economy.load();
            o.ownedTomeSpells = std::move(a_ownedTomeSpells);
            g_orders[token] = std::move(o);
        }
        if (!Papyrus::DispatchTradeRun(q, token)) {
            std::scoped_lock lk(g_mtx);
            g_orders.erase(token);
            spdlog::warn("[econ] dispatch failed for token {}", token);
            return false;
        }
        return true;
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mtx);
        g_orders.clear();
        // Cross-save token guard (Fable audit #3): a RunTrade suspended in a save
        // resumes with its OLD token; jump the counter far past any value the next
        // session could reissue, so the stale token can never name a fresh order
        // (GetVendorChest -> none -> the resumed stack aborts safe).
        g_nextToken.fetch_add(1'000'000);
    }

}
