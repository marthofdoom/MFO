// logistics/LootNeed.cpp -- IAUS LOOTING, the engine side (ClickUp 1dfcuabd18f, branch feat/iaus-loot).
//
// marth 2026-11-12: "still find loot the same way we currently do, but then iaus what gets looted by current inventory" / "That way they
// would loot potions they may want to use in battle later"; "when IAUS looting is ON, the looting gambit rows are disabled (replaced, never
// stacked)". marth 2026-11-13: "Potions shoudl also read all effects, in fact food and drink as well"; poisons: "All our current lists allow
// it. if not via the lust itself(im sure it does) via MAOs perk".
//
// FINDING is unchanged (LootScan: LootNearby / StripCorpse / the excursion). This file reads the follower's inventory into the pure
// scorer's input (iaus/LootNeed.h) and answers two questions for the three loot walks (Service.cpp's rule walk, RunExcursionScan,
// StripCorpse): WHICH categories are worth taking now (IausLootPlan) and, for consumables, WHICH items and how many
// (LootConsumablesNeed). Every take still goes through TheftGate::Takeable / Take and the walks' GuardInterim / TierReleased gates.
//
// THREADING: the AddTask worker only, inside ServiceFollower (g_svc), exactly like the loot code it serves. The only state is the log
// rate-limit map below, worker-only and not save-scoped (a stale entry only delays one log line).
#include "Logistics_internal.h"
#include "TheftGate.h"
#include "PotionFx.h"
#include "classes/Classes.h"   // THIEF STEALS BY NEED: Classes::Active(), GetEquipped(), Get().gambits (does his class ship act.steal?)
#include "iaus/LootNeed.h"

#include <cstring>
#include <unordered_map>

namespace MFO::Logistics {

    namespace {
        using Iaus::LootKind;
        using AV = RE::ActorValue;

        constexpr auto kLogEvery = std::chrono::seconds(10);
        constexpr RE::FormID kLockpick = 0x0000000A;   // LootLockpicks' form

        // LOW-POWER FLOOR (marth: "ignore low power potions entirely"), as LootPotions: a POTION's restore below the floor does not count as
        // a restore on the ITEM side. Its other effects and a food are never floored (a food's restores are small by design). A magnitude of
        // 1.0 is ReadConsumable's "classified, unreadable" marker and is exempt like LootPotions' mag == 0.
        void ApplyLootFloor(Iaus::Consumable& a_c) {
            const float floor = PotionLootFloor();
            if (a_c.food || floor <= 0.0f) return;
            for (auto& m : a_c.restoreMag)
                if (m > 1.0f && m < floor) m = 0.0f;
        }

        // (follower << 32) ^ key -> last [iaus-loot] line. WORKER only.
        std::unordered_map<std::uint64_t, Clock::time_point> g_logAt;

        bool LogDue(RE::FormID a_follower, std::uint32_t a_key) {
            const auto now = Clock::now();
            if (g_logAt.size() > 4096) g_logAt.clear();
            const std::uint64_t k = (static_cast<std::uint64_t>(a_follower) << 32) ^ a_key;
            auto it = g_logAt.find(k);
            if (it != g_logAt.end() && now - it->second < kLogEvery) return false;
            g_logAt[k] = now;
            return true;
        }

        // THIEF STEALS BY NEED: a "Thief-line" follower = his EQUIPPED class ships an applied act.steal default row (Thief, Master Thief,
        // Nightingale, Listener today; read from the class table, never a list of ids). Classes OFF / no class equipped: false.
        bool IsThiefLine(RE::FormID a_fid) {
            if (!a_fid || !Classes::Active()) return false;
            const std::uint8_t eq = Classes::GetEquipped(a_fid);
            if (eq >= Classes::kClassCount) return false;
            const Classes::ClassDef& def = Classes::Get(static_cast<Classes::Id>(eq));
            for (std::size_t i = 0; i < def.gambitCount; ++i) {
                const Classes::DefaultGambit& d = def.gambits[i];
                if (d.IsApplied() && !d.pendingRuling && d.act && std::strcmp(d.act, Vocab::kActSteal) == 0) return true;
            }
            return false;
        }

        // The tuning a theft is scored with: NEED only. A sale score (consumableSellRoad) is a reason to take free loot, never to steal.
        Iaus::LootTuning StealTuning() {
            Iaus::LootTuning t = Iaus::DefaultLootTuning();
            t.consumableSellRoad = false;
            return t;
        }

        Category CatOf(LootKind a_k) {
            switch (a_k) {
            case LootKind::Arrows:      return Category::Arrows;
            case LootKind::Bolts:       return Category::Bolts;
            case LootKind::Potions:     return Category::Potions;
            case LootKind::Equipment:   return Category::Equipment;
            case LootKind::Gold:        return Category::Gold;
            case LootKind::Jewelry:     return Category::Jewelry;
            case LootKind::SoulGems:    return Category::SoulGems;
            case LootKind::Lockpicks:   return Category::Lockpicks;
            case LootKind::Ingredients: return Category::Ingredients;
            case LootKind::Valuables:   return Category::Valuables;
            case LootKind::kCount:      break;
            }
            return Category::Gold;
        }

        // Does he carry a launcher of the kind (bow for arrows, crossbow for bolts)? marth 2026-11-12: ammo need = class + gear.
        bool CarriesLauncher(RE::Actor* a_actor, bool a_bolt) {
            const auto want = a_bolt ? RE::WEAPON_TYPE::kCrossbow : RE::WEAPON_TYPE::kBow;
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (auto* w = obj->As<RE::TESObjectWEAP>(); w && w->GetWeaponType() == want) return true;
            }
            return false;
        }

        int FamilyOf(AV a_av) {
            switch (a_av) {
            case AV::kHealth:  return Iaus::kRestoreHealth;
            case AV::kStamina: return Iaus::kRestoreStamina;
            case AV::kMagicka: return Iaus::kRestoreMagicka;
            default:           return -1;
            }
        }

        // THE ONE CONSUMABLE READER (marth 2026-11-13: "design with future tweaks in mind"): every consumer reads an ALCH through this and
        // nothing else -- the ITEM side (what a source holds) AND the STOCK side (what he carries, ReadLootInv), so the two can never
        // disagree about what an item is (review F1). To classify a new kind of consumable, edit here (or the catalog row / ClassifyRuntime
        // behind ConsumableBuffs); the scorer in iaus/LootNeed.h never looks at a form.
        //   restore family: EXACTLY what the drink / count roads use (PotionRestores: catalog first, costliest-effect archetype otherwise,
        //     never a food, a poison or a drug) with PotionMagnitude; a mixed potion is one family, as it is for CountPotions.
        //   buff kinds: ConsumableBuffs (catalog fx row first, the runtime rule otherwise).
        //   poison: the strength of all its hostile effects. A drug (hostile effects, no poison flag) carries no need.
        // No low-power floor here (the stock side must count every potion): the item side applies PotionLootFloor on top.
        Iaus::Consumable ReadConsumableImpl(RE::AlchemyItem* a_alc) {
            Iaus::Consumable c;
            c.food    = a_alc->IsFood();
            c.poison  = a_alc->IsPoison();
            c.hostile = c.poison || PotionIsDrug(a_alc);
            c.value   = static_cast<float>(a_alc->GetGoldValue());
            c.weight  = a_alc->GetWeight();
            if (c.poison) {
                for (auto* e : a_alc->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (mgef && (mgef->IsHostile() || mgef->IsDetrimental()))
                        c.poisonPower += e->GetMagnitude() * std::max(1.0f, static_cast<float>(e->GetDuration()));
                }
                return c;
            }
            if (c.hostile) return c;
            for (const auto* e : a_alc->effects)
                if (e) c.maxDuration = std::max(c.maxDuration, static_cast<float>(e->GetDuration()));   // the use side's stack gate (DrinkPotionForm)
            if (const int f = FamilyOf(PotionRestores(a_alc)); f >= 0) {
                const float mag = PotionMagnitude(a_alc);
                c.restoreMag[static_cast<std::size_t>(f)] = mag > 0.0f ? mag : 1.0f;   // 1 = classified, magnitude unreadable (counts, never floored)
            }
            c.fx = ConsumableBuffs(a_alc);
            for (const auto& fx : c.fx) c.maxDuration = std::max(c.maxDuration, fx.duration);   // a catalog row's duration can differ from the record's
            if (c.food) {
                // feat/consumables: the USE side reads a food's restore too (the loot side's restoreMag stays 0 for food, review F1).
                for (const auto* e : a_alc->effects) {
                    const auto* mgef = e ? e->baseEffect : nullptr;
                    if (!mgef) continue;
                    const auto arch = mgef->data.archetype;
                    if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier && arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier) continue;
                    if (mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover)) continue;   // a Recover value-modifier is a fortify (a buff kind)
                    const int f = FamilyOf(mgef->data.primaryAV);
                    if (f < 0) continue;
                    const float mag = e->GetMagnitude();
                    auto& slot = c.foodRestoreMag[static_cast<std::size_t>(f)];
                    const float m = mag > 0.0f ? mag : 1.0f;
                    if (m > slot) { slot = m; c.foodRestoreDur[static_cast<std::size_t>(f)] = static_cast<float>(e->GetDuration()); }
                }
            }
            return c;
        }

        // The lowest charge fraction (1 = full or none) over every extra list of a weapon stack: ExtraCharge against the copy's own
        // ExtraEnchantment maximum, else the base enchantment's (the same two sources InventoryEntryData::GetEnchantmentCharge reads).
        float LowestChargeFrac(RE::TESObjectWEAP* a_w, const RE::InventoryEntryData* a_entry) {
            float low = 1.0f;
            if (!a_entry->extraLists) return low;
            for (auto* xList : *a_entry->extraLists) {
                if (!xList) continue;
                const auto* xCharge = xList->GetByType<RE::ExtraCharge>();
                if (!xCharge) continue;   // no ExtraCharge on this copy: full
                const auto* xEnch = xList->GetByType<RE::ExtraEnchantment>();
                float max = 0.0f;
                if (xEnch && xEnch->enchantment && xEnch->charge != 0) max = static_cast<float>(xEnch->charge);
                else if (a_w->formEnchanting && a_w->amountofEnchantment != 0) max = static_cast<float>(a_w->amountofEnchantment);
                if (max > 0.0f) low = std::min(low, xCharge->charge / max);
            }
            return low;
        }

        Iaus::LootInv ReadLootInv(RE::Actor* a_actor, const FollowerState& a_state) {
            Iaus::LootInv inv;
            const WeaponRoles roles = ComputeWeaponRoles(a_actor, a_state);
            inv.usesArrows  = UsesAmmoKind(&a_state, roles, false) && CarriesLauncher(a_actor, false);
            inv.usesBolts   = UsesAmmoKind(&a_state, roles, true) && CarriesLauncher(a_actor, true);
            inv.arrowTarget = AmmoKeepTarget(&a_state, false, inv.usesArrows);
            inv.boltTarget  = AmmoKeepTarget(&a_state, true, inv.usesBolts);
            inv.arrows      = AmmoCount(a_actor, false);
            inv.bolts       = AmmoCount(a_actor, true);
            inv.melee       = roles.melee != WepClass::Other;
            inv.ranged      = roles.doRanged;
            inv.caster      = IsCasterFollower(a_state);
            inv.sneaks      = TableHasAction(a_state.logistics(), Vocab::kActSneak) || TableHasAction(a_state.combat(), Vocab::kActSneak);
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() == kLockpick) { inv.lockpicks += data.first; continue; }
                if (IsSoulGemItem(obj)) { inv.soulGems += data.first; continue; }   // filled or empty
                if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                    // SOUL GEMS' USE (marth 2026-11-13): the lowest charge over EVERY copy of an enchanted weapon (each extra list; review
                    // R2-2: InventoryEntryData::GetEnchantmentCharge stops at the first list with a charge). Staves and bound weapons do not take gems.
                    if (data.second && !w->IsStaff() && !w->IsBound())
                        inv.weaponChargeFrac = std::min(inv.weaponChargeFrac, LowestChargeFrac(w, data.second.get()));
                    continue;
                }
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc) continue;
                const Iaus::Consumable c = ReadConsumable(alc);   // the SAME reader the item side uses
                if (c.poison) { inv.poisons += data.first; continue; }
                for (std::size_t f = 0; f < Iaus::kRestoreFamilies; ++f)
                    if (c.restoreMag[f] > 0.0f) inv.restore[f] += data.first;
                std::array<bool, Iaus::kBuffKinds> seen{};
                for (const auto& fx : c.fx) {
                    const auto i = static_cast<std::size_t>(fx.kind);
                    if (i >= Iaus::kBuffKinds || seen[i]) continue;
                    seen[i] = true;
                    inv.buff[i] += data.first;
                }
            }
            if (auto* avo = a_actor->AsActorValueOwner()) inv.capacity = avo->GetActorValue(AV::kCarryWeight);
            inv.carried = a_actor->GetWeightInContainer();
            return inv;
        }

        // ONE inventory read per ServiceFollower tick (review F4): keyed on the tick's sequence number + the follower, dropped after a real
        // take. Worker only (like g_svc).
        struct InvCache { std::uint64_t seq = 0; RE::FormID fid = 0; bool valid = false; Iaus::LootInv inv; };
        InvCache g_invCache;

        const Iaus::LootInv& CachedLootInv(RE::Actor* a_actor, const FollowerState& a_state) {
            if (!g_invCache.valid || g_invCache.seq != g_svcSeq || g_invCache.fid != a_actor->GetFormID()) {
                g_invCache.inv   = ReadLootInv(a_actor, a_state);
                g_invCache.seq   = g_svcSeq;
                g_invCache.fid   = a_actor->GetFormID();
                g_invCache.valid = true;
            }
            return g_invCache.inv;
        }
    }

    Iaus::Consumable ReadConsumable(RE::AlchemyItem* a_alc) { return a_alc ? ReadConsumableImpl(a_alc) : Iaus::Consumable{}; }

    void IausLootInvDirty() { g_invCache.valid = false; }

    bool IausLootValuablesTake(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower || !IausLootOn(a_state)) return false;
        return Iaus::ScoreLootKind(LootKind::Valuables, CachedLootInv(a_follower, a_state)).take;
    }

    bool IausLooseConsumableWanted(RE::Actor* a_follower, RE::AlchemyItem* a_alc) {
        if (!a_follower || !a_alc || !g_svc) return false;
        Iaus::Consumable c = ReadConsumable(a_alc);
        ApplyLootFloor(c);
        return Iaus::ScoreConsumable(c, 1, CachedLootInv(a_follower, *g_svc)).take;   // a peek: no log
    }

    bool IausNeedStealWants(RE::Actor* a_follower, RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base) {
        if (!a_follower || !a_ref || !a_base || !g_svc) return false;
        // OWNED only (an explicit owner, or a crime to take: the cell-owned case). An unowned loose item is the ordinary IAUS road's.
        if (!a_ref->GetOwner() && !a_ref->IsOffLimits()) return false;
        const Iaus::LootTuning st = StealTuning();
        const auto& inv = CachedLootInv(a_follower, *g_svc);
        const std::int32_t count = std::max<std::int32_t>(a_ref->extraList.GetCount(), 1);
        Iaus::LootVerdict need;
        if (a_base->GetFormID() == kLockpick)
            need = Iaus::ScoreLootKind(LootKind::Lockpicks, inv, st);
        else if (IsSoulGemItem(a_base))
            need = Iaus::ScoreLootKind(LootKind::SoulGems, inv, st);
        else if (auto* alc = a_base->As<RE::AlchemyItem>(); alc && IsDrinkablePotion(alc)) {
            Iaus::Consumable c = ReadConsumable(alc);
            ApplyLootFloor(c);
            need = Iaus::ScoreConsumable(c, count, inv, st);
        } else
            return false;   // not a need-scored kind: coin, valuables, jewellery and the rest never steal by need
        const float value = static_cast<float>(std::max<std::int32_t>(a_base->GetGoldValue(), 0)) * static_cast<float>(count);
        const auto v = Iaus::ScoreStealNeed(need, value, st);
        const RE::FormID fid = a_follower->GetFormID();
        if (LogDue(fid, a_ref->GetFormID()))
            spdlog::info("[loot] {:08X} thief-need {} {:08X} '{}' ({:08X}) x{}: {}", fid, v.take ? "take" : "skip", a_ref->GetFormID(),
                         a_base->GetName() ? a_base->GetName() : "?", a_base->GetFormID(), count, v.why);
        return v.take;
    }

    bool IausLootOn(const FollowerState& a_state) {
        return Config::g_iausLoot.load() && Config::g_iausAvailable.load() && a_state.mfoEnabled && a_state.iausCombat;
    }

    std::vector<Category> IausLootPlan(RE::Actor* a_follower, const FollowerState& a_state) {
        std::vector<Category> out;
        if (!a_follower) return out;
        const auto& inv = CachedLootInv(a_follower, a_state);
        const RE::FormID fid = a_follower->GetFormID();
        std::vector<std::pair<float, Category>> takes;
        for (std::size_t i = 0; i < Iaus::kLootKinds; ++i) {
            const auto k = static_cast<LootKind>(i);
            const auto v = Iaus::ScoreLootKind(k, inv);
            if (LogDue(fid, 0xFFFF0000u | static_cast<std::uint32_t>(i)))
                spdlog::info("[iaus-loot] {:08X}: {} {} score={:.2f} ({})", fid, Iaus::LootKindName(k), v.take ? "TAKE" : "leave", v.score, v.why);
            if (v.take) takes.emplace_back(v.score, CatOf(k));
        }
        std::stable_sort(takes.begin(), takes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        out.reserve(takes.size());
        for (const auto& t : takes) out.push_back(t.second);
        // THIEF STEALS BY NEED (marth 2026-11-13): after every free-loot category, a Thief-line follower also arms the steal road when a NEED
        // kind (the kinds that score by need: potions, lockpicks, soul gems) reaches stealNeedMin before risk. Per-item and witness gates follow.
        if (IsThiefLine(fid)) {
            const Iaus::LootTuning st = StealTuning();
            for (const LootKind k : { LootKind::Potions, LootKind::Lockpicks, LootKind::SoulGems }) {
                const auto v = Iaus::ScoreLootKind(k, inv, st);
                if (v.score < st.stealNeedMin) continue;
                if (LogDue(fid, 0xFFFE0000u | static_cast<std::uint32_t>(k)))
                    spdlog::info("[loot] {:08X} thief-need plan: steal road armed for {} (need {:.2f} >= min {:.2f}; {})", fid, Iaus::LootKindName(k),
                                 static_cast<double>(v.score), static_cast<double>(st.stealNeedMin), v.why);
                out.push_back(Category::Steal);
                break;
            }
        }
        return out;
    }

    bool LootConsumablesNeed(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek) {
        if (!a_follower || !a_src || !g_svc) return false;
        Iaus::LootInv inv = CachedLootInv(a_follower, *g_svc);
        const RE::FormID fid = a_follower->GetFormID();
        struct Item { RE::TESBoundObject* obj; Iaus::Consumable c; std::int32_t count; float score; };
        std::vector<Item> items;
        for (auto& [obj, data] : a_src->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* alc = obj->As<RE::AlchemyItem>();
            if (!alc) continue;
            Iaus::Consumable c = ReadConsumable(alc);
            ApplyLootFloor(c);
            const auto v = Iaus::ScoreConsumable(c, data.first, inv);
            // F8: only a real take / leave decision is logged, never the HasLoot peek (it re-judges every distinct item every few seconds).
            if (!a_peek && LogDue(fid, obj->GetFormID()))
                spdlog::info("[iaus-loot] {:08X}: item {:08X} '{}' x{} from {:08X}: {} score={:.2f} n={} ({})", fid, obj->GetFormID(),
                             obj->GetName() ? obj->GetName() : "?", data.first, a_src->GetFormID(), v.take ? "TAKE" : "leave", v.score,
                             v.takeCount, v.why);
            if (!v.take) continue;
            if (!TheftGate::Takeable(a_follower, a_src, obj)) continue;   // theft gate: no copy he may own, no take
            if (a_peek) return true;
            items.push_back({ obj, std::move(c), data.first, v.score });
        }
        if (a_peek) return false;
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
        bool moved = false;
        for (const auto& it : items) {
            // Re-judged against what he carries NOW: an earlier take in this visit may have filled the need.
            const auto v = Iaus::ScoreConsumable(it.c, it.count, inv);
            if (!v.take) continue;
            if (!FitsCarryWeight(a_follower, it.obj->GetWeight() * static_cast<float>(v.takeCount))) continue;
            if (TheftGate::Take(a_follower, a_src, it.obj, v.takeCount, TheftGate::Road::Container) > 0) {
                moved = true;
                IausLootInvDirty();   // a real take: the cached read is stale
                inv = CachedLootInv(a_follower, *g_svc);
            }
        }
        return moved;
    }

}
