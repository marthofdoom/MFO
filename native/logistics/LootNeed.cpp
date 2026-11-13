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
#include "iaus/LootNeed.h"

#include <unordered_map>

namespace MFO::Logistics {

    namespace {
        using Iaus::LootKind;
        using AV = RE::ActorValue;

        constexpr auto kLogEvery = std::chrono::seconds(10);
        constexpr RE::FormID kLockpick = 0x0000000A;   // LootLockpicks' form

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

        // One ALCH read from ALL its effects: plain restores (a value modifier on health / stamina / magicka without the Recover flag;
        // with it, or Peak, it is a fortify and ConsumableBuffs names it), every buff kind (catalog first, the runtime rule otherwise),
        // and for a poison the strength of all its hostile effects. A drug (hostile effects, no poison flag) is read for sale only.
        Iaus::Consumable ReadConsumable(RE::AlchemyItem* a_alc) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            Iaus::Consumable c;
            c.food    = a_alc->IsFood();
            c.poison  = a_alc->IsPoison();
            c.hostile = c.poison || PotionIsDrug(a_alc);
            c.value   = static_cast<float>(a_alc->GetGoldValue());
            c.weight  = a_alc->GetWeight();
            for (auto* e : a_alc->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef) continue;
                const bool bad = mgef->IsHostile() || mgef->IsDetrimental();
                if (c.poison) {
                    if (bad) c.poisonPower += e->GetMagnitude() * std::max(1.0f, static_cast<float>(e->GetDuration()));
                    continue;
                }
                if (c.hostile || bad) continue;
                const auto arch = mgef->data.archetype;
                if (arch != Arch::kValueModifier && arch != Arch::kDualValueModifier) continue;
                if (mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover)) continue;
                const int f = FamilyOf(mgef->data.primaryAV);
                if (f < 0) continue;
                auto& m = c.restoreMag[static_cast<std::size_t>(f)];
                m = std::max(m, e->GetMagnitude());
            }
            if (!c.hostile) c.fx = ConsumableBuffs(a_alc);
            return c;
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
            inv.restore[Iaus::kRestoreHealth]  = CountPotions(a_actor, AV::kHealth);
            inv.restore[Iaus::kRestoreStamina] = CountPotions(a_actor, AV::kStamina);
            inv.restore[Iaus::kRestoreMagicka] = CountPotions(a_actor, AV::kMagicka);
            for (auto& [obj, data] : a_actor->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                if (obj->GetFormID() == kLockpick) { inv.lockpicks += data.first; continue; }
                auto* alc = obj->As<RE::AlchemyItem>();
                if (!alc) continue;
                if (alc->IsPoison()) { inv.poisons += data.first; continue; }
                std::array<bool, Iaus::kBuffKinds> seen{};
                for (const auto& fx : ConsumableBuffs(alc)) {
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
    }

    bool IausLootOn(const FollowerState& a_state) {
        return Config::g_iausLoot.load() && Config::g_iausAvailable.load() && a_state.mfoEnabled && a_state.iausCombat;
    }

    std::vector<Category> IausLootPlan(RE::Actor* a_follower, const FollowerState& a_state) {
        std::vector<Category> out;
        if (!a_follower) return out;
        const auto inv = ReadLootInv(a_follower, a_state);
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
        return out;
    }

    bool LootConsumablesNeed(RE::Actor* a_follower, RE::TESObjectREFR* a_src, bool a_peek) {
        if (!a_follower || !a_src || !g_svc) return false;
        auto inv = ReadLootInv(a_follower, *g_svc);
        const RE::FormID fid = a_follower->GetFormID();
        // LOW-POWER FLOOR (marth: "ignore low power potions entirely"), as LootPotions: a POTION's restore below the floor does not count
        // as a restore. Its other effects (buff kinds) and its sale gold still do. Food is never floored (its restores are small by design).
        const float floor = PotionLootFloor();
        struct Item { RE::TESBoundObject* obj; Iaus::Consumable c; std::int32_t count; float score; };
        std::vector<Item> items;
        for (auto& [obj, data] : a_src->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* alc = obj->As<RE::AlchemyItem>();
            if (!alc) continue;
            Iaus::Consumable c = ReadConsumable(alc);
            if (!c.food && floor > 0.0f)
                for (auto& m : c.restoreMag)
                    if (m > 0.0f && m < floor) m = 0.0f;
            const auto v = Iaus::ScoreConsumable(c, data.first, inv);
            if (LogDue(fid, obj->GetFormID()))
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
                inv = ReadLootInv(a_follower, *g_svc);
            }
        }
        return moved;
    }

}
