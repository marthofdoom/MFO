#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include "Config.h"
#include "ItemCatalog.h"
#include "logistics/Pricing.h"     // the follower-own barter price model (town phase 2 R2)
#include "logistics/Logistics.h"   // Logistics::PotionRestores -- the SAME classifier CountPotions uses
#include "Vocabulary.h"  // Vocab::IsCastableSpell -- the tome-buy gate
#include <mutex>

namespace MFO::TradeBridge {

    namespace {

        struct TradeOrder {
            RE::ActorHandle      follower;
            RE::FormID           followerId = 0;   // for the town errand's outcome record (below)
            RE::FormID           memoKey = 0;      // town errand: the empty-trade memo's vendor key when no vendor actor resolves
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
            std::string          buyPlan;    // names PlanBuy chose ("Falkreath Helmet x1, ...") for the [econ] report line
            std::uint64_t        sig = 0;       // NO EMPTY TRADES: TradeSignature at dispatch
            float                daysAt = 0.0f; // RE::Calendar days passed at dispatch
            // Town phase 2 R2: the follower's price quote for THIS vendor, taken ONCE at the first PlanBuy /
            // GetBuyPrice of the order so the plan's budget and the gold Papyrus pays use one multiplier.
            Logistics::Pricing::Quote quote;
            bool                 quoted = false;
            // R8: what PlanBuy planned of the smithing materials, and the plan's total units (ReportTrade credits the materials only when
            // the report shows the whole plan was executed).
            std::vector<SmithMat> smithPlanned;
            std::int32_t         plannedTotal = 0;
        };

        std::mutex                                    g_mtx;
        std::unordered_map<std::int32_t, TradeOrder>  g_orders;

        // NO EMPTY TRADES (see TradeBridge.h). Guarded by g_mtx; session state, never
        // saved, cleared with the orders. g_chestGold: each vendor chest's barter gold as
        // the LAST report there read it. g_emptyMemo: per (follower, vendor), the last
        // result was EMPTY -- its signature (TradeSignature folded with that report's
        // chest gold) and the game day it happened.
        // sig = TradeSignature alone (purse, needs, sell rows); gold = the chest's gold that
        // empty report read. Kept APART (field 0928c, Gunmar): folding the gold into the
        // signature let ANOTHER follower's trade at the same chest (which moves its gold)
        // reopen this follower's own empty memo, and he walked up to trade nothing again.
        struct EmptyMemo { std::uint64_t sig = 0; std::int32_t gold = -1; float day = 0.0f; bool logged = false; };
        std::unordered_map<RE::FormID, std::int32_t>     g_chestGold;
        std::unordered_map<std::uint64_t, EmptyMemo>     g_emptyMemo;
        struct StaticSkip { std::uint64_t reason = 0; float day = 0.0f; };
        std::unordered_map<std::uint64_t, StaticSkip>    g_staticSkip;   // pair -> skip (b): the reason sig + the game day it began
        // TOWN ERRAND (logistics/Errand.cpp): the LAST report per follower, so the errand that dispatched
        // the order can see that it finished and what it moved. Guarded by g_mtx; session state.
        std::unordered_map<RE::FormID, TradeOutcome>     g_outcomes;

        std::uint64_t PairKey(RE::FormID a_follower, RE::FormID a_vendor) {
            return (static_cast<std::uint64_t>(a_follower) << 32) | a_vendor;
        }
        std::uint64_t Mix(std::uint64_t a_h, std::uint64_t a_v) {   // hash_combine-style 64-bit fold
            a_h ^= a_v + 0x9E3779B97F4A7C15ull + (a_h << 6) + (a_h >> 2);
            return a_h;
        }
        // The vendor restock cadence: GMST iDaysToRespawnVendor (Skyrim.esm 0x0123C00E,
        // value 2 in the shipped master), read by name. Unreadable -> the master's 2,
        // said once.
        float RestockDays() {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* st  = gsc ? gsc->GetSetting("iDaysToRespawnVendor") : nullptr;
            if (st) return static_cast<float>(std::max(1, st->GetSInt()));
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                spdlog::warn("[econ] GMST iDaysToRespawnVendor unreadable -- the empty-trade backoff uses Skyrim.esm's "
                             "value, 2 game days");
            return 2.0f;
        }
        std::atomic<std::int32_t>                     g_nextToken{ 1 };
        std::atomic<std::int32_t>                     g_sessionFirstToken{ 1 };   // the first token this session issues

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
                // TOWN PHASE 2 R7: the inn's FOOD order. Food is judged from its own effects (Logistics::FoodWorth, the potion judge's
                // archetype split); only an order that asked for food ever sees this, every other trade classifies exactly as before.
                if (b.wantFood && alc->IsFood()) return Logistics::FoodWorth(alc) > 0.0f ? NeedCat::kFood : -1;
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
                // Catalog-first, then the shared kNonBolt-flag fallback -- so uncatalogued
                // vendor ammo still classifies and gets bought (P5).
                // THE SILVER SLOT (86e3p2gp9): while a ranged follower owns none, silver ammo of his kind
                // is that restock first (a pure form read, safe on this VM thread).
                if (b.wantSilverAmmo && Logistics::IsSilverAmmo(am) && Logistics::AmmoIsBolt(am) == b.silverAmmoBolt)
                    return NeedCat::kAmmoSilver;
                return Logistics::AmmoIsBolt(am) ? NeedCat::kBolts : NeedCat::kArrows;
            }
            // ── #21 GEAR (Feature A): weapon/armor/mage-apparel upgrades. The
            // thresholds in b were computed on the worker from the follower's OWN
            // inventory (Logistics reused its loot judge); native only maps a stock
            // form to a category here -- the beats-baseline test is in PlanBuy.
            // ── SMITHING MATERIALS (town phase 2 R8): a form on the follower's smithing want list is a buy of its own,
            // whatever gambits he has (PlanBuy's pass below). Pure id compare; the list is the worker-built smithMats.
            for (const auto& m : b.smithMats)
                if (m.form == id) return NeedCat::kSmithMaterial;
            if (b.buyGear) {
                if (auto* weap = a_form->As<RE::TESObjectWEAP>()) {
                    // THE SILVER SLOT (86e3p2gp9): while he owns none, a silver melee weapon is that
                    // restock first (a pure form read, safe on this VM thread).
                    if (b.wantSilver && Logistics::IsSilverMeleeWeapon(weap) &&
                        (!b.silverOneHand || Logistics::WeaponBuyClass(weap->GetWeaponType()) == 0))
                        return NeedCat::kWeaponSilver;
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
        // Takes the order's price quote once (caller holds g_mtx). vendorKey = the vendor actor, else the
        // shop faction's key the dispatcher filed as memoKey.
        const Logistics::Pricing::Quote& EnsureQuote(TradeOrder& a_o) {
            if (!a_o.quoted) {
                auto vp = a_o.vendor.get();
                const RE::FormID vkey = vp ? vp->GetFormID() : a_o.memoKey;
                a_o.quote  = Logistics::Pricing::Resolve(a_o.follower, a_o.vendor, vkey);
                a_o.quoted = true;
            }
            return a_o.quote;
        }

        std::vector<std::int32_t> PlanBuy(RE::StaticFunctionTag*, std::int32_t a_token,
                                          std::vector<RE::TESForm*> a_forms,
                                          std::vector<std::int32_t> a_counts) {
            std::vector<std::int32_t> plan(a_forms.size(), 0);
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            if (!o) return plan;
            const BuyThresholds& b = o->buy;
            // R2: every candidate is priced with the follower's own buy multiplier, so the plan's budget
            // (and buySpent) is the gold GetBuyPrice makes Papyrus pay.
            const float buyMult = EnsureQuote(*o).buy;

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
                const int baseVal = BaseValue(f);
                if (baseVal <= 0) continue;   // free/valueless stock isn't a real buy
                const int val = std::max(1, static_cast<int>(Logistics::Pricing::Apply(baseVal, buyMult)));
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

            // ── SMITHING MATERIALS (town phase 2 R8, marth: "regardless of gambits a follower will ... buy whats needed for
            //    town errand smithing"): each wanted form the vendor stocks, up to its qty, held to HALF of the purse that is
            //    left (a trip never leaves him broke on ingots). Before the gear pass so the gear reserve sees what is left. ──
            int matLeft = o->budget / 2;   // ONE half-purse budget for ALL the materials, computed once (as R7 does for food)
            for (const auto& m : b.smithMats) {
                int need = m.qty;
                for (auto& c : cands) {
                    if (need <= 0) break;
                    if (c.kind != NeedCat::kSmithMaterial || c.f->GetFormID() != m.form) continue;
                    int qty = std::min(need, c.avail - plan[c.idx]);
                    qty = std::min<int>(qty, matLeft / c.value);
                    if (qty <= 0) continue;
                    plan[c.idx] += qty;
                    need        -= qty;
                    matLeft     -= qty * c.value;
                    o->budget   -= qty * c.value;
                    o->buySpent += qty * c.value;
                }
            }

            // ── FOOD (TOWN PHASE 2 R7, the inn's stock): fill the kFood quota, best worth per gold first, never more than half the
            //    purse. Only an order with wantFood has kFood candidates (ClassifyBuy). ──
            if (b.wantFood) {
                NeedCat* need = nullptr;
                for (auto& n : o->needs) if (n.kind == NeedCat::kFood && n.quota > 0) { need = &n; break; }
                if (need) {
                    std::vector<Cand> foods;
                    for (auto& c : cands) if (c.kind == NeedCat::kFood) foods.push_back(c);
                    std::sort(foods.begin(), foods.end(), [](const Cand& a, const Cand& c2) {
                        const float wa = Logistics::FoodWorth(a.f->As<RE::AlchemyItem>()) / static_cast<float>(a.value);
                        const float wb = Logistics::FoodWorth(c2.f->As<RE::AlchemyItem>()) / static_cast<float>(c2.value);
                        return wa != wb ? wa > wb : a.value < c2.value;
                    });
                    int foodLeft = o->budget / 2;   // R7 review: the cap is half the purse the FOOD pass started with, computed ONCE (not half of what is left per line)
                    for (const auto& c : foods) {
                        if (need->quota <= 0) break;
                        int qty = std::min(c.avail, need->quota);
                        qty = std::min(qty, foodLeft / c.value);
                        if (qty <= 0) continue;
                        foodLeft -= qty * c.value;
                        plan[c.idx] += qty;
                        need->quota -= qty;
                        o->budget   -= qty * c.value;
                        o->buySpent += qty * c.value;
                    }
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
            std::size_t offHandIdx = SIZE_MAX;   // the second one-hander's plan line, named in buyPlan
            std::size_t silverIdx  = SIZE_MAX;   // the silver slot's plan line (86e3p2gp9), named in buyPlan
            std::size_t silverAmmoIdx = SIZE_MAX;   // ... and its ammo line (silver ammo restock)
            if (b.buyGear) {
                // MELEE weapon -- best WeaponScore (damage x perk-style bias, the
                // loot judge's own ranking) above the owned in-class baseline.
                bool primaryBought = false;
                { std::size_t best = SIZE_MAX; float bestScore = b.meleeBaseScore; int bestVal = 0;
                  for (auto& c : cands) {
                      if (c.kind != NeedCat::kWeaponMelee || !affordReserve(c.value)) continue;
                      auto* w = c.f->As<RE::TESObjectWEAP>();
                      const float score = w ? Logistics::WeaponBuyScore(w, b.preferKinds) : 0.0f;
                      if (score > bestScore ||
                          (best != SIZE_MAX && score == bestScore && c.value < bestVal)) {
                          best = c.idx; bestScore = score; bestVal = c.value; }
                  }
                  if (best != SIZE_MAX) { buyOne(best, bestVal); primaryBought = true; }
                }
                // SECOND ONE-HANDER (dual wield by perks, 2026-09-14): a dual
                // wielder (wantOffHand: offHand == 2, melee class 1H -- so every
                // kWeaponMelee candidate here IS a one-hander) also buys ONE more
                // in-class weapon above his SECOND-best owned score. If a primary
                // was just bought, the old best drops to the off-hand and ITS score
                // (meleeBaseScore) is the bar instead. A stock line the primary
                // took the last copy of is skipped; the same form with copies left
                // is a legitimate pair (PickOffHandWeapon takes the second copy).
                if (b.wantOffHand) {
                    const float bar = primaryBought ? b.meleeBaseScore : b.offHandBaseScore;
                    std::size_t best = SIZE_MAX; float bestScore = bar; int bestVal = 0;
                    for (auto& c : cands) {
                        if (c.kind != NeedCat::kWeaponMelee || !affordReserve(c.value)) continue;
                        if (plan[c.idx] >= c.avail) continue;   // the primary took the only copy
                        auto* w = c.f->As<RE::TESObjectWEAP>();
                        const float score = w ? Logistics::WeaponBuyScore(w, b.preferKinds) : 0.0f;
                        if (score > bestScore ||
                            (best != SIZE_MAX && score == bestScore && c.value < bestVal)) {
                            best = c.idx; bestScore = score; bestVal = c.value; }
                    }
                    if (best != SIZE_MAX) { buyOne(best, bestVal); offHandIdx = best; }
                }
                // THE SILVER SLOT (ClickUp 86e3p2gp9): he owns no silver melee weapon and an
                // act.equip_silver rule wants one -- buy ONE, the best affordable by WeaponBuyScore
                // (no owned baseline: he has none), cheaper on a tie.
                if (b.wantSilver) {
                    std::size_t best = SIZE_MAX; float bestScore = -1.0f; int bestVal = 0;
                    for (auto& c : cands) {
                        if (c.kind != NeedCat::kWeaponSilver || !affordReserve(c.value)) continue;
                        auto* w = c.f->As<RE::TESObjectWEAP>();
                        const float score = w ? Logistics::WeaponBuyScore(w, b.preferKinds) : 0.0f;
                        if (score > bestScore ||
                            (best != SIZE_MAX && score == bestScore && c.value < bestVal)) {
                            best = c.idx; bestScore = score; bestVal = c.value; }
                    }
                    if (best != SIZE_MAX) { buyOne(best, bestVal); silverIdx = best; }
                }
                // THE SILVER SLOT's AMMO (ClickUp 86e3p2gp9, marth 2026-10-12): a ranged follower owns no
                // silver ammo of his kind -- buy the best affordable stack (damage, then value), up to
                // kSilverAmmoStock rounds, held to the comfortably-affordable reserve like every gear buy.
                if (b.wantSilverAmmo) {
                    std::size_t best = SIZE_MAX; float bestDmg = -1.0f; int bestVal = 0, bestAvail = 0;
                    for (auto& c : cands) {
                        if (c.kind != NeedCat::kAmmoSilver || !affordReserve(c.value)) continue;
                        auto* am = c.f->As<RE::TESAmmo>();
                        const float dmg = am ? am->GetRuntimeData().data.damage : 0.0f;
                        if (dmg > bestDmg || (dmg == bestDmg && c.value > bestVal)) {
                            best = c.idx; bestDmg = dmg; bestVal = c.value; bestAvail = c.avail; }
                    }
                    if (best != SIZE_MAX) {
                        std::int64_t qty = std::min<std::int64_t>(Logistics::kSilverAmmoStock, bestAvail - plan[best]);
                        qty = std::min<std::int64_t>(qty, (o->budget / 2) / bestVal);
                        if (qty > 0) {
                            plan[best]  += static_cast<std::int32_t>(qty);
                            o->budget   -= static_cast<std::int32_t>(qty) * bestVal;
                            o->buySpent += static_cast<std::int32_t>(qty) * bestVal;
                            silverAmmoIdx = best;
                        }
                    }
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
                // 2 hands 3 feet 4 shield), best ArmorScore above THAT slot's owned
                // baseline. Per-slot so a warrior with a good chestpiece still buys a
                // helmet/boots for bare slots (mirrors the loot judge + the mage-
                // apparel pass below). One best-pick per slot.
                // SCORED (2026-09-14): ArmorScoreOf = rating x the follower's class/
                // perk bias (armorHeavyBias / armorLightBias, worker-computed from
                // his BASE skills + perk votes), the loot judge's own ArmorScore --
                // so buy no longer picks a heavy 18 helmet over a light 13 for a
                // light-skilled follower (13 x 2.0 = 26 > 18 x 1.0). The baseline is
                // the best OWNED score per slot (armorBaseScore), never the raw
                // rating.
                if (b.buyArmor) {
                    for (int slot = 0; slot < 5; ++slot) {
                        std::size_t best = SIZE_MAX;
                        float bestScore = b.armorBaseScore[slot]; int bestVal = 0;
                        for (auto& c : cands) {
                            if (c.kind != NeedCat::kArmor || !affordReserve(c.value)) continue;
                            auto* a = c.f->As<RE::TESObjectARMO>();
                            if (!a || Logistics::ArmorBuySlot(a) != slot) continue;
                            const float score = ArmorScoreOf(a->GetArmorRating(), a->GetArmorType(),
                                                             b.armorHeavyBias, b.armorLightBias);
                            if (score > bestScore ||
                                (best != SIZE_MAX && score == bestScore && c.value < bestVal)) {
                                best = c.idx; bestScore = score; bestVal = c.value; }
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

            // ── AMMO SWAP-UP (THE SWAP-UP RULE, 86e3ebfu3; logistics/SwapUp.cpp) ──
            // marth: "an archer should be willing to sell iron for better arrows".
            // AFTER the gear pass (review round 1 on 4f23c30: gear upgrades come
            // first). The best-ranked ammo of his kind (damage, VALUE breaking a tie)
            // STRICTLY above the weakest tier he relies on (b.ammoBarDmg/Value), up to
            // b.ammoUpgradeQty rounds (what carries his whole keep target). Special
            // rounds (an explosion projectile) are never an upgrade. Units the SUPPLY
            // pass already planned above the bar count toward the quantity, and never
            // more of a stock line than it still has (plan[] < avail -- the MFO-B36
            // shape: one copy is never planned twice). Spend is held to a QUARTER of
            // the purse left after gear. The obsolete stacks this purchase creates
            // sell on the next visit.
            if (b.ammoUpgrade && b.ammoUpgradeQty > 0) {
                const std::int32_t kind = b.ammoWantBolt ? NeedCat::kBolts : NeedCat::kArrows;
                auto aboveBar = [&](float dmg, int val) {
                    return dmg > b.ammoBarDmg || (dmg == b.ammoBarDmg && val > b.ammoBarValue);
                };
                std::int32_t already = 0;
                std::size_t best = SIZE_MAX; float bestDmg = 0.0f; int bestVal = 0; int bestAvail = 0;
                for (auto& c : cands) {
                    if (c.kind != kind) continue;
                    auto* am = c.f->As<RE::TESAmmo>();
                    if (!am || am->GetRuntimeData().data.flags.all(RE::AMMO_DATA::Flag::kNonPlayable) ||
                        Logistics::AmmoIsSpecialBase(am) || Logistics::IsSilverAmmo(am)) continue;   // silver ammo is the silver slot's, not a ladder tier
                    const float dmg = am->GetRuntimeData().data.damage;
                    if (!aboveBar(dmg, c.value)) continue;
                    already += plan[c.idx];
                    const int left = c.avail - plan[c.idx];
                    if (left <= 0) continue;
                    if (best == SIZE_MAX || dmg > bestDmg || (dmg == bestDmg && c.value > bestVal)) {
                        best = c.idx; bestDmg = dmg; bestVal = c.value; bestAvail = left;
                    }
                }
                if (best != SIZE_MAX && bestVal > 0) {
                    std::int64_t qty = static_cast<std::int64_t>(b.ammoUpgradeQty) - already;
                    qty = std::min<std::int64_t>(qty, bestAvail);
                    qty = std::min<std::int64_t>(qty, (o->budget / 4) / bestVal);
                    if (qty > 0) {
                        plan[best]  += static_cast<std::int32_t>(qty);
                        o->budget   -= static_cast<std::int32_t>(qty) * bestVal;
                        o->buySpent += static_cast<std::int32_t>(qty) * bestVal;
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
            // Name the plan for the [econ] report line (2026-09-14): "BUY 2 for
            // 340g" said nothing about WHAT was bought, so a wrong-class helmet
            // could not be traced to this planner from the field log. Papyrus
            // executes the plan verbatim (moves + pays per line), so the planned
            // names ARE the bought names for every line it could afford.
            {
                std::string names; int lines = 0;
                for (std::size_t i = 0; i < plan.size(); ++i) {
                    if (plan[i] <= 0) continue;
                    if (++lines > 12) continue;
                    auto* n = a_forms[i] ? a_forms[i]->As<RE::TESFullName>() : nullptr;
                    if (!names.empty()) names += ", ";
                    names += std::format("'{}' x{}{}", n && n->GetFullName() && *n->GetFullName() ? n->GetFullName() : "?", plan[i],
                                         i == offHandIdx ? " (second one-hander, dual wield)"
                                         : i == silverIdx ? " (silver weapon slot)"
                                         : i == silverAmmoIdx ? " (silver ammo slot)" : "");
                }
                if (lines > 12) names += std::format(", +{} more", lines - 12);
                o->buyPlan = std::move(names);
            }
            o->smithPlanned.clear();
            o->plannedTotal = 0;
            for (std::size_t i = 0; i < plan.size(); ++i) o->plannedTotal += std::max(plan[i], 0);
            for (const auto& m : b.smithMats) {
                std::int32_t n = 0;
                for (std::size_t i = 0; i < plan.size(); ++i)
                    if (plan[i] > 0 && a_forms[i] && a_forms[i]->GetFormID() == m.form) n += plan[i];
                if (n > 0) o->smithPlanned.push_back(SmithMat{ m.form, n });
            }
            return plan;
        }

        // Token-FREE base value of a form. Papyrus pays per-item with this so the
        // buy loop is self-contained (move + pay together) and survives a save that
        // lands mid-loop -- unlike a token'd total, which is gone after a load.
        std::int32_t GetFormValue(RE::StaticFunctionTag*, RE::TESForm* a_form) {
            return BaseValue(a_form);
        }

        // R2 (APPENDED native 11): what the follower PAYS per unit for a form at this order's vendor, base
        // value x his buy multiplier (Speech, then the kModBuyPrices perks; logistics/Pricing.h). A token the
        // session no longer knows (a RunTrade stack saved mid-run) answers the base value, logged: the
        // Papyrus stack resumes and finishes without a Papyrus error.
        std::int32_t GetBuyPrice(RE::StaticFunctionTag*, std::int32_t a_token, RE::TESForm* a_form) {
            const std::int32_t base = BaseValue(a_form);
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            if (!o) {
                spdlog::info("[econ] GetBuyPrice: token {} unknown (stale or reaped) -- the base value {} is used "
                             "for '{:08X}'", a_token, base, a_form ? a_form->GetFormID() : 0u);
                return base;
            }
            return Logistics::Pricing::Apply(base, EnsureQuote(*o).buy);
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
                if (it == g_orders.end()) {
                    // A token from BEFORE this session's first one is a RunTrade stack that
                    // was saved mid-run and resumed after a load (field 0928c: 1000159 at
                    // 19:56:26, 48 s after kDataLoaded). Its order died with that session
                    // (ClearTransientState): GetVendorChest answered none, the resumed stack
                    // aborted, nothing traded. Anything newer is an order this session lost
                    // (the 30 s reap in VendorTrade): that one stays a warning.
                    // Outside [this session's first token, the next one) = not issued this
                    // session. (A counter restarts per PROCESS, so a stale token from a save
                    // made in an earlier process can also land INSIDE a later session's range
                    // -- reported, not changed here.)
                    if (a_token < g_sessionFirstToken.load() || a_token >= g_nextToken.load())
                        spdlog::info("[econ] ReportTrade: token {} is from a trade saved mid-run before the last "
                                     "load -- its order is gone, nothing was traded (the resumed script aborted)",
                                     a_token);
                    else
                        spdlog::warn("[econ] ReportTrade: unknown token {} (issued this session; its order was reaped "
                                     "or never stored)", a_token);
                    return;
                }
                o = std::move(it->second);
                g_orders.erase(it);
            }
            if (a_vendorGold < 0) {
                spdlog::info("[econ] token {} -- Papyrus found no usable chest (aborted)", a_token);
                std::scoped_lock lk(g_mtx);
                g_outcomes[o.followerId] = TradeOutcome{ true, 0, 0, 0, 0 };
                return;
            }
            const std::int32_t postTradeGold = o.probeOnly ? a_vendorGold
                : static_cast<std::int32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(a_vendorGold) -
                                                                      a_soldValue + a_spent));
            {
                // NO EMPTY TRADES: file the result. The chest's gold is the last-seen value
                // for every later signature there; an EMPTY result is remembered for this
                // (follower, vendor) against what was offered, anything else forgets it.
                const auto vpp = o.vendor.get();
                auto* vp = vpp.get();
                std::scoped_lock lk(g_mtx);
                // MFO_Trade.psc reports the chest's gold read BEFORE the trade (:36); a real trade then moved
                // soldValue OUT of the chest (:68) and spent INTO it (:107), both exactly the reported totals.
                // Store what the chest holds NOW. A probe moves no gold (psc `if !probe`), so it keeps the read.
                g_chestGold[o.chestId] = postTradeGold;
                g_outcomes[o.followerId] = TradeOutcome{ false, a_soldCount, a_soldValue, a_boughtCount, a_spent };
                if (!o.probeOnly && o.plannedTotal > 0 && a_boughtCount >= o.plannedTotal)
                    g_outcomes[o.followerId].smithBought = o.smithPlanned;   // R8: the whole plan ran, so its materials arrived
                // The memo key: the vendor actor when it resolves, else the errand's own (the plan keyed SkipTrade by it).
                const RE::FormID vkey = vp ? vp->GetFormID() : o.memoKey;
                if (o.followerId && vkey) {
                    const auto key = PairKey(o.followerId, vkey);
                    if (a_soldCount == 0 && a_boughtCount == 0)
                        g_emptyMemo[key] = EmptyMemo{ o.sig, a_vendorGold, o.daysAt, false };
                    else
                        g_emptyMemo.erase(key);
                }
            }
            auto* fol = o.follower.get().get();
            auto* ven = o.vendor.get().get();
            const char* verb = o.probeOnly ? "WOULD" : "did";
            // Name the SELL rows offered (native's list; Papyrus sells them in this
            // order until the chest's gold runs out -- the counts say how far it got)
            // and the BUY plan (PlanBuy's picks, executed verbatim by Papyrus).
            std::string offered;
            for (std::size_t i = 0; i < o.sell.size(); ++i) {
                if (i >= 12) { offered += std::format(", +{} more", o.sell.size() - 12); break; }
                auto* n = o.sell[i].obj ? o.sell[i].obj->As<RE::TESFullName>() : nullptr;
                if (!offered.empty()) offered += ", ";
                offered += std::format("'{}' x{}", n && n->GetFullName() && *n->GetFullName() ? n->GetFullName() : "?",
                                       o.sell[i].count);
            }
            spdlog::info("[econ] {:08X} '{}' @ '{}': chest gold NOW={} | {} SELL {} for {}g, BUY {} for {}g "
                         "(sell n={}, needs={}, purse={}) | offered [{}] | bought plan [{}]",
                         fol ? fol->GetFormID() : 0u,
                         fol && fol->GetName() ? fol->GetName() : "?",
                         ven && ven->GetName() ? ven->GetName() : "?",
                         postTradeGold, verb, a_soldCount, a_soldValue, a_boughtCount, a_spent,
                         o.sell.size(), o.needs.size(), o.budget + o.buySpent,   // original purse
                         offered, o.buyPlan);
            // R2: ONE price line per trade -- the follower's Speech, the factors, the perk-adjusted values,
            // the clamps and the rounding. The quote was taken at dispatch (or plan time) and priced the sell
            // rows, PlanBuy's budget and every GetBuyPrice of this order.
            if (o.quoted)
                spdlog::info("[econ] {:08X} price @ '{}': {}", fol ? fol->GetFormID() : 0u,
                             ven && ven->GetName() ? ven->GetName() : "?", Logistics::Pricing::Describe(o.quote));
            else
                spdlog::info("[econ] {:08X} price @ '{}': no quote was attached to this order and no buy plan ran", fol ? fol->GetFormID() : 0u,
                             ven && ven->GetName() ? ven->GetName() : "?");
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
        a_vm->RegisterFunction("GetBuyPrice",      "MFO_Trade", GetBuyPrice);   // APPENDED (R2): the 11th native
        spdlog::info("[trade] registered MFO_Trade natives (sell + buy bridge)");
        return true;
    }

    bool VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget, const BuyThresholds& a_buy,
                     std::uint64_t a_sig, float a_daysNow, RE::FormID a_memoKey,
                     const Logistics::Pricing::Quote* a_quote) {
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
            o.followerId = a_follower->GetFormID();
            o.memoKey   = a_memoKey;
            o.vendor    = a_vendor ? a_vendor->GetHandle() : RE::ActorHandle{};
            o.chest     = a_chest->GetHandle();
            o.chestId   = chestId;
            o.dispatchedAt = nowT;
            o.sell      = std::move(a_sell);
            o.needs     = std::move(a_needs);
            o.budget    = a_budget;
            o.buy       = a_buy;
            o.sig       = a_sig;
            o.daysAt    = a_daysNow;
            o.probeOnly = !Config::g_economy.load();
            // ONE quote per trade: the one the sell rows were priced from (R2). A caller without one
            // (none today) falls back to EnsureQuote at the first PlanBuy / GetBuyPrice.
            if (a_quote) { o.quote = *a_quote; o.quoted = true; }
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

    std::uint64_t TradeSignature(std::int32_t a_purse, const std::vector<NeedCat>& a_needs,
                                 const std::vector<SellRow>& a_sell, const std::vector<SmithMat>& a_mats) {
        std::uint64_t h = 0xCBF29CE484222325ull;
        h = Mix(h, static_cast<std::uint32_t>(a_purse));
        h = Mix(h, a_needs.size());
        for (const auto& n : a_needs)
            h = Mix(h, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(n.kind)) << 32) |
                           static_cast<std::uint32_t>(n.quota));
        h = Mix(h, a_sell.size());
        for (const auto& r : a_sell) {
            h = Mix(h, r.obj ? r.obj->GetFormID() : 0u);
            h = Mix(h, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(r.count)) << 32) |
                           static_cast<std::uint32_t>(r.value));
        }
        for (const auto& m : a_mats)   // R8: empty for every pre-R8 caller, so their hashes are unchanged
            h = Mix(h, (static_cast<std::uint64_t>(m.form) << 32) | static_cast<std::uint32_t>(m.qty));
        return h;
    }

    bool SkipTrade(RE::FormID a_follower, RE::FormID a_vendor, RE::FormID a_chest, std::uint64_t a_sig,
                   float a_daysNow, bool a_buyWishes, const std::vector<SellRow>& a_sell,
                   const std::vector<NeedCat>& a_needs, std::string& a_why) {
        a_why.clear();
        const float restock = RestockDays();
        std::scoped_lock lk(g_mtx);
        const auto gIt = g_chestGold.find(a_chest);
        const bool goldKnown = gIt != g_chestGold.end();
        const std::int32_t gold = goldKnown ? gIt->second : -1;
        const auto key = PairKey(a_follower, a_vendor);
        // (a) the last trade here was empty and nothing changed since.
        if (auto mIt = g_emptyMemo.find(key); mIt != g_emptyMemo.end()) {
            auto& m = mIt->second;
            // The chest's gold matters to HIM only through his sell rows (the vendor's gold
            // pays for what he sells; his purse pays for what he buys). So a changed gold
            // reopens the memo only when it now pays for one of his rows that the gold of
            // his empty trade could not.
            const bool goldOpensARow = goldKnown && gold != m.gold &&
                std::any_of(a_sell.begin(), a_sell.end(), [&](const SellRow& r) {
                    return r.count > 0 && r.value <= gold && r.value > m.gold;
                });
            if (a_sig != m.sig || goldOpensARow) {
                g_emptyMemo.erase(mIt);   // HIS side changed, or the chest can now pay for a row: trade again
            } else if (a_daysNow >= m.day + restock) {
                g_emptyMemo.erase(mIt);   // the vendor has restocked since: trade again
            } else {
                if (m.logged) return true;   // said once per memo
                m.logged = true;
                a_why = std::format("the last trade here moved nothing and nothing changed since (purse, needs, "
                                    "{} sell row(s), chest gold {}) -- next try after the vendor restock "
                                    "(iDaysToRespawnVendor = {:.0f} game days: day {:.1f}) or any change",
                                    a_sell.size(), gold, restock, m.day + restock);
                return true;
            }
        }
        // (b) nothing to buy and no offered row the chest's last-seen gold can pay for.
        if (!a_buyWishes && a_needs.empty() && goldKnown) {
            const bool affordable = std::any_of(a_sell.begin(), a_sell.end(),
                                                [gold](const SellRow& r) { return r.count > 0 && r.value <= gold; });
            if (!affordable) {
                // RESTOCK EXPIRY (review F5 on f2c8e2b): the chest's gold is only
                // refreshed by a report, so a skip must expire on its own -- at the
                // vendor restock (iDaysToRespawnVendor game days after the skip began)
                // one trade goes through and its report re-reads the gold.
                const std::uint64_t reason = Mix(a_sig, static_cast<std::uint64_t>(gold));
                auto& last = g_staticSkip[key];
                if (last.reason == reason) {
                    if (a_daysNow >= last.day + restock) {
                        g_staticSkip.erase(key);
                        return false;   // restocked: trade once, the report refreshes the gold
                    }
                    return true;   // said once per (pair, signature)
                }
                last.reason = reason;
                last.day    = a_daysNow;
                a_why = a_sell.empty()
                            ? std::string("nothing to sell and nothing to buy")
                            : std::format("nothing to buy, and the chest's last-seen gold ({}) pays for none of the "
                                          "{} sell row(s)", gold, a_sell.size());
                return true;
            }
        }
        return false;
    }

    std::int32_t LastSeenChestGold(RE::FormID a_chest) {
        std::scoped_lock lk(g_mtx);
        const auto it = g_chestGold.find(a_chest);
        return it == g_chestGold.end() ? -1 : it->second;
    }

    bool HasLiveOrder(RE::FormID a_follower) {
        std::scoped_lock lk(g_mtx);
        for (const auto& [tok, o] : g_orders)
            if (o.followerId == a_follower) return true;
        return false;
    }

    bool TakeOutcome(RE::FormID a_follower, TradeOutcome& a_out) {
        std::scoped_lock lk(g_mtx);
        const auto it = g_outcomes.find(a_follower);
        if (it == g_outcomes.end()) return false;
        a_out = it->second;
        g_outcomes.erase(it);
        return true;
    }

    void DiscardOutcome(RE::FormID a_follower) {
        std::scoped_lock lk(g_mtx);
        g_outcomes.erase(a_follower);
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mtx);
        g_orders.clear();
        g_outcomes.clear();
        g_chestGold.clear();
        g_emptyMemo.clear();
        g_staticSkip.clear();
        Logistics::Pricing::Clear();   // R2: the price cache is session-only
        // Cross-save token guard (Fable audit #3): a RunTrade suspended in a save
        // resumes with its OLD token; jump the counter far past any value the next
        // session could reissue, so the stale token can never name a fresh order
        // (GetVendorChest -> none -> the resumed stack aborts safe).
        g_nextToken.fetch_add(1'000'000);
        g_sessionFirstToken.store(g_nextToken.load());
    }

}
