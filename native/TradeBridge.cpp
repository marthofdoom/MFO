#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include "Config.h"
#include "ItemCatalog.h"
#include "Logistics.h"   // Logistics::PotionRestores -- the SAME classifier CountPotions uses
#include "Vocabulary.h"  // Vocab::IsCastableSpell -- the tome-buy gate
#include <mutex>

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
            BuyThresholds        buy{};      // #21 gear/tome buy thresholds (worker-computed)
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
        std::int32_t ClassifyBuy(RE::TESForm* a_form, const BuyThresholds& b) {
            if (!a_form) return -1;
            const auto id = a_form->GetFormID();
            (void)id;
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
            // ── #21 GEAR (Feature A): weapon/armor/mage-apparel upgrades. The
            // thresholds in b were computed on the worker from the follower's OWN
            // inventory (Logistics reused its loot judge); native only maps a stock
            // form to a category here -- the beats-baseline test is in PlanBuy.
            if (b.buyGear) {
                if (auto* weap = a_form->As<RE::TESObjectWEAP>()) {
                    const int wc = Logistics::WeaponBuyClass(weap->GetWeaponType());  // 0=1H 1=2H 2=Ranged 3=Other
                    if (b.meleeClass != 3 && wc == b.meleeClass) return NeedCat::kWeaponMelee;
                    if (b.doRanged && wc == 2) {
                        const bool isXbow = weap->GetWeaponType() == RE::WEAPON_TYPE::kCrossbow;
                        if (isXbow == b.wantCrossbow) return NeedCat::kWeaponRanged;
                    }
                    return -1;
                }
                if (auto* armo = a_form->As<RE::TESObjectARMO>()) {
                    const bool rated = armo->GetArmorRating() > 0.0f;
                    // A mage wants rating-0 school clothing; a non-mage wants rated
                    // armor. (School match + villain-coding are judged in PlanBuy.)
                    if (b.buyMageApparel && !rated) return NeedCat::kMageApparel;
                    if (b.buyArmor && rated) {
                        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
                        const bool isShield = (static_cast<std::uint32_t>(armo->GetSlotMask()) &
                                               static_cast<std::uint32_t>(Slot::kShield)) != 0;
                        if (isShield && b.meleeClass != 0) return -1;   // a shield needs a free off-hand (1H melee only)
                        return NeedCat::kArmor;
                    }
                    return -1;
                }
            }
            // ── #21 SPELL TOMES (Feature B): a book teaching a castable spell. The
            // school / top-2 / known / afford gating is in PlanBuy (needs the token).
            if (b.buyTomes) {
                if (auto* book = a_form->As<RE::TESObjectBOOK>()) {
                    if (!book->TeachesSpell()) return -1;
                    auto* sp = book->data.teaches.spell;
                    if (!sp || !MFO::Vocab::IsCastableSpell(sp)) return -1;
                    return NeedCat::kSpellTome;
                }
            }
            return -1;
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
            if (!o) return plan;
            const BuyThresholds& b = o->buy;

            // Classify every stock line ONCE (supply + gear + tome kinds).
            struct Cand { std::size_t idx; std::int32_t kind, value, avail; RE::TESForm* f; };
            std::vector<Cand> cands;
            for (std::size_t i = 0; i < a_forms.size(); ++i) {
                auto* f = a_forms[i];
                const int avail = (i < a_counts.size()) ? a_counts[i] : 0;
                if (!f || avail <= 0) continue;
                if (Catalog::IsExcluded(f->GetFormID())) continue;
                const int kind = ClassifyBuy(f, b);
                if (kind < 0) continue;
                const int val = BaseValue(f);
                if (val <= 0) continue;   // free/valueless stock isn't a real buy
                cands.push_back(Cand{ i, kind, val, avail, f });
            }

            // ── SUPPLY (potions/ammo): fill each below-threshold quota, best
            //    (highest value) first, bounded by the shared purse. Unchanged. ──
            if (!o->needs.empty()) {
                std::vector<Cand> sup;
                for (auto& c : cands) if (c.kind <= NeedCat::kBolts) sup.push_back(c);
                std::sort(sup.begin(), sup.end(),
                          [](const Cand& a, const Cand& b) { return a.value > b.value; });
                for (const auto& c : sup) {
                    if (o->budget < c.value) continue;   // can't afford even one
                    NeedCat* need = nullptr;
                    for (auto& n : o->needs) if (n.kind == c.kind && n.quota > 0) { need = &n; break; }
                    if (!need) continue;
                    int qty = c.avail;
                    if (qty > need->quota)         qty = need->quota;
                    if (qty > o->budget / c.value) qty = o->budget / c.value;
                    if (qty <= 0) continue;
                    plan[c.idx] += qty;
                    need->quota -= qty;
                    o->budget   -= qty * c.value;
                    o->buySpent += qty * c.value;
                }
            }

            // "Comfortably affordable" reserve rule (marth): a gear/tome buy only
            // when its value is <= 50% of the REMAINING purse, so a follower never
            // goes broke. Evaluated against the shrinking budget, so each category
            // leaves at least half of what was left before it.
            auto affordReserve = [&](int value) {
                return value > 0 && static_cast<std::int64_t>(value) * 2 <= o->budget;
            };
            auto buyOne = [&](std::size_t idx, int value) {
                plan[idx] += 1; o->budget -= value; o->buySpent += value;
            };

            // ── GEAR (Feature A): at most ONE upgrade per category per window. ──
            if (b.buyGear) {
                // MELEE weapon -- best damage above the owned in-class baseline.
                { std::size_t best = SIZE_MAX; int bestDmg = b.meleeBaseDmg, bestVal = 0;
                  for (auto& c : cands) {
                      if (c.kind != NeedCat::kWeaponMelee || !affordReserve(c.value)) continue;
                      auto* w = c.f->As<RE::TESObjectWEAP>();
                      const int dmg = w ? static_cast<int>(w->GetAttackDamage()) : 0;
                      if (dmg > bestDmg ||
                          (best != SIZE_MAX && dmg == bestDmg && c.value < bestVal)) {
                          best = c.idx; bestDmg = dmg; bestVal = c.value; }
                  }
                  if (best != SIZE_MAX) buyOne(best, bestVal);
                }
                // RANGED weapon -- best damage above the owned baseline.
                { std::size_t best = SIZE_MAX; int bestDmg = b.rangedBaseDmg, bestVal = 0;
                  for (auto& c : cands) {
                      if (c.kind != NeedCat::kWeaponRanged || !affordReserve(c.value)) continue;
                      auto* w = c.f->As<RE::TESObjectWEAP>();
                      const int dmg = w ? static_cast<int>(w->GetAttackDamage()) : 0;
                      if (dmg > bestDmg ||
                          (best != SIZE_MAX && dmg == bestDmg && c.value < bestVal)) {
                          best = c.idx; bestDmg = dmg; bestVal = c.value; }
                  }
                  if (best != SIZE_MAX) buyOne(best, bestVal);
                }
                // ARMOR (plain rated, non-mage) -- PER LOGICAL SLOT (0 head 1 body
                // 2 hands 3 feet 4 shield), best rating above THAT slot's owned
                // baseline. Per-slot so a warrior with a good chestpiece still buys a
                // helmet/boots for bare slots (mirrors the loot judge + the mage-
                // apparel pass below). One best-pick per slot.
                if (b.buyArmor) {
                    for (int slot = 0; slot < 5; ++slot) {
                        std::size_t best = SIZE_MAX;
                        int bestRat = b.armorBaseRat[slot], bestVal = 0;
                        for (auto& c : cands) {
                            if (c.kind != NeedCat::kArmor || !affordReserve(c.value)) continue;
                            auto* a = c.f->As<RE::TESObjectARMO>();
                            if (!a || Logistics::ArmorBuySlot(a) != slot) continue;
                            const int rat = static_cast<int>(a->GetArmorRating());
                            if (rat > bestRat ||
                                (best != SIZE_MAX && rat == bestRat && c.value < bestVal)) {
                                best = c.idx; bestRat = rat; bestVal = c.value; }
                        }
                        if (best != SIZE_MAX) buyOne(best, bestVal);
                    }
                }
                // MAGE APPAREL + JEWELRY -- PER SLOT (head/body/hands/feet/ring/
                // amulet), buy the MOST VALUABLE affordable clothing/jewelry piece
                // that beats what the follower already owns in that slot (marth:
                // mages wear expensive clothing, swapped for pricier finds). Villain-
                // coded regalia is refused unless the follower is a necromancer;
                // bMageApparelStrict filters OUT off-school enchanted pieces. One buy
                // per slot per window; slots processed in order so the shrinking
                // purse (affordReserve) bounds the total. (Whether a bought piece is
                // then WORN is the follower/AI's business -- keepArmor below stops it
                // being re-sold; see the report on equipping bought apparel.)
                if (b.buyMageApparel) {
                    for (int slot = 0; slot < 6; ++slot) {
                        std::size_t best = SIZE_MAX;
                        int bestTier = b.mageBaseTier[slot];       // must beat the owned baseline
                        std::int32_t bestMetric = b.mageBaseMetric[slot];
                        int bestBuyVal = 0;
                        for (auto& c : cands) {
                            if (c.kind != NeedCat::kMageApparel || !affordReserve(c.value)) continue;
                            auto* a = c.f->As<RE::TESObjectARMO>();
                            if (!a || Logistics::MageClothingSlot(a) != slot) continue;
                            int tier = 0; std::int32_t metric = 0;
                            if (!Logistics::MageApparelBuyKey(a, b.eligibleSchools, b.mageSchoolPrimary,
                                                              b.isNecromancer, tier, metric)) continue;
                            // Rank: higher school tier first, then higher metric
                            // (value, or value+fortify magnitude for a school piece).
                            if (tier > bestTier || (tier == bestTier && metric > bestMetric)) {
                                best = c.idx; bestTier = tier; bestMetric = metric; bestBuyVal = c.value;
                            }
                        }
                        if (best != SIZE_MAX) buyOne(best, bestBuyVal);
                    }
                }
            }

            // ── SPELL TOMES (Feature B): every qualifying tome, re-checking the
            //    reserve rule against the shrinking purse. A tome buys iff its spell
            //    is in a top-2 school (b.eligibleSchools) and the follower does not
            //    already know it. ──
            if (b.buyTomes) {
                auto* fol = o->follower.get().get();
                for (auto& c : cands) {
                    if (c.kind != NeedCat::kSpellTome || !affordReserve(c.value)) continue;
                    auto* book = c.f->As<RE::TESObjectBOOK>();
                    auto* sp   = (book && book->TeachesSpell()) ? book->data.teaches.spell : nullptr;
                    if (!sp) continue;
                    const int bit = Logistics::SpellSchoolBit(sp);
                    if (bit < 0 || !((b.eligibleSchools >> bit) & 1)) continue;   // not a top-2 school
                    if (fol && fol->HasSpell(sp)) continue;                       // already known
                    buyOne(c.idx, c.value);
                }
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
                     std::int32_t a_budget, const BuyThresholds& a_buy) {
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
            o.buy       = a_buy;
            o.probeOnly = !Config::g_economy.load();
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
