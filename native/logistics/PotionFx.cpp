// logistics/PotionFx.cpp -- see PotionFx.h. feat/buff-potions (ClickUp 1dfcuabd18f).
#include "logistics/PotionFx.h"

#include "ItemCatalog.h"
#include "cast/Actuation.h"
#include "logistics/Logistics.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace MFO::Logistics {

    namespace {
        using Clock = std::chrono::steady_clock;
        using Iaus::BuffKind;
        using AV = RE::ActorValue;

        // (follower << 32 | potion form) -> when its own duration ends. WORKER only: DrinkPotionForm is called from Actuation::Fire on the
        // worker, like DrinkBest and g_drinkUntil; cleared by ClearTransientState on the main thread after the pump stopped (kPreLoadGame).
        std::unordered_map<std::uint64_t, Clock::time_point> g_buffUntil;

        // feat/consumables fix F2: a drink is only PROVEN by the carried count dropping. (follower << 32 | form) -> the count carried when
        // EquipObject was issued; ConfirmDrinks reads it back on the next pass. WORKER only, like g_buffUntil; cleared with it.
        struct PendingDrink { std::int32_t before = 0; std::string name; };
        std::unordered_map<std::uint64_t, PendingDrink> g_pendingDrink;

        std::int32_t CarriedCount(RE::Actor* a_follower, const RE::AlchemyItem* a_alc) {
            for (auto& [obj, data] : a_follower->GetInventory())
                if (obj == a_alc) return data.first;
            return 0;
        }

        void ConfirmOne(RE::Actor* a_follower, std::uint64_t a_key, const PendingDrink& a_p) {
            const auto form = static_cast<RE::FormID>(a_key & 0xFFFFFFFFu);
            auto* alc = RE::TESForm::LookupByID<RE::AlchemyItem>(form);
            const std::int32_t now = alc ? CarriedCount(a_follower, alc) : 0;
            if (now < a_p.before)
                spdlog::info("[iaus] consumable {:08X}: '{}' ({:08X}) CONSUMED, carried {} -> {}", a_follower->GetFormID(), a_p.name, form, a_p.before, now);
            else   // no masking, no retry: the equip road was issued and the count did not drop
                spdlog::warn("[iaus] consumable {:08X}: '{}' ({:08X}) NOT CONSUMED, carried {} -> {} after EquipObject", a_follower->GetFormID(), a_p.name, form, a_p.before, now);
        }

        // THE RULE, runtime side. Written to match installer/MFO.Synthesis/Catalog.cs ClassifyFx line for line (the numbers of the actor
        // values are the same on both sides: Mutagen's ActorValue enum and RE::ActorValue agree, checked 2026-11-12). Returns false = the
        // effect is not a kind (a restore, a damage, a utility effect).
        bool KindOf(RE::EffectArchetypes::ArchetypeID a_arch, AV a_av, bool a_recover, BuffKind& a_kind, int& a_skillAv) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            a_skillAv = -1;
            switch (a_arch) {
            case Arch::kCurePoison:  a_kind = BuffKind::CurePoison;  return true;
            case Arch::kCureDisease: a_kind = BuffKind::CureDisease; return true;
            case Arch::kInvisibility: a_kind = BuffKind::Invisibility; return true;
            case Arch::kValueModifier:
            case Arch::kDualValueModifier:
            case Arch::kPeakValueModifier: break;
            default: return false;
            }
            const bool peak = a_arch == Arch::kPeakValueModifier;
            switch (a_av) {
            case AV::kResistFire:    a_kind = BuffKind::ResistFire;   return true;
            case AV::kResistFrost:   a_kind = BuffKind::ResistFrost;  return true;
            case AV::kResistShock:   a_kind = BuffKind::ResistShock;  return true;
            case AV::kPoisonResist:  a_kind = BuffKind::ResistPoison; return true;
            case AV::kResistMagic:   a_kind = BuffKind::ResistMagic;  return true;
            case AV::kWaterBreathing: a_kind = BuffKind::WaterBreathing; return true;
            case AV::kHealRate: case AV::kHealRateMult:       a_kind = BuffKind::RegenHealth;  return true;
            case AV::kStaminaRate: case AV::kStaminaRateMult: a_kind = BuffKind::RegenStamina; return true;
            case AV::kMagickaRate: case AV::kMagickaRateMult: a_kind = BuffKind::RegenMagicka; return true;
            case AV::kAttackDamageMult: case AV::kMeleeDamage: case AV::kUnarmedDamage: a_kind = BuffKind::FortifyAttack; return true;
            // A resource effect is a FORTIFY only with the Recover flag or the Peak archetype; a plain one is a restore (its own fields).
            case AV::kHealth:  if (!(a_recover || peak)) return false; a_kind = BuffKind::FortifyHealth;  return true;
            case AV::kStamina: if (!(a_recover || peak)) return false; a_kind = BuffKind::FortifyStamina; return true;
            case AV::kMagicka: if (!(a_recover || peak)) return false; a_kind = BuffKind::FortifyMagicka; return true;
            default: break;
            }
            // The 18 skills as the BASE skill number 6..23. Vanilla fortify-skill potions use the PowerModifier actor values 135..152
            // (base + 129) and some the *Modifier values 96..113 (base + 90); only a few use the base value (fork ActorValues.h).
            if (const int n = Iaus::BaseSkillAv(static_cast<int>(a_av)); n >= 0) { a_kind = BuffKind::FortifySkill; a_skillAv = n; return true; }
            return false;
        }

        std::vector<Iaus::PotionFx> ClassifyRuntime(const RE::AlchemyItem* a_potion) {
            std::vector<Iaus::PotionFx> out;
            for (const auto* e : a_potion->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef) continue;
                BuffKind kind = BuffKind::kCount;
                int      skill = -1;
                const bool recover = mgef->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover);
                if (!KindOf(mgef->data.archetype, mgef->data.primaryAV, recover, kind, skill)) continue;
                Iaus::PotionFx fx;
                fx.kind      = kind;
                fx.magnitude = e->GetMagnitude();
                fx.duration  = static_cast<float>(e->GetDuration());
                fx.av        = skill;
                out.push_back(fx);
            }
            return out;
        }
    }

    std::vector<Iaus::PotionFx> PotionBuffs(RE::AlchemyItem* a_potion) {
        // Never a food (an apple is not a potion), a poison, or a drug: the same refusals PotionRestores makes.
        if (!a_potion || a_potion->IsFood() || a_potion->IsPoison() || PotionIsDrug(a_potion)) return {};
        if (const auto* cat = Catalog::PotionEffects(a_potion->GetFormID())) return *cat;
        return ClassifyRuntime(a_potion);
    }

    std::vector<Iaus::PotionFx> ConsumableBuffs(RE::AlchemyItem* a_item) {
        if (!a_item || a_item->IsPoison() || PotionIsDrug(a_item)) return {};
        if (const auto* cat = Catalog::PotionEffects(a_item->GetFormID())) return *cat;
        return ClassifyRuntime(a_item);
    }

    bool DrinkPotionForm(RE::Actor* a_follower, RE::FormID a_potion) {
        if (!a_follower || a_potion == 0) return false;
        if (const char* why = Actuation::CannotActReason(a_follower)) {
            Actuation::NoteRefusedApply(a_follower->GetFormID(), "drink", why, 0, a_follower->GetFormID());
            return false;
        }
        auto* alc = RE::TESForm::LookupByID<RE::AlchemyItem>(a_potion);
        if (!alc) return false;
        // A snapshot is up to seconds old: re-judge the FORM here, never trust the candidate alone. feat/consumables: through the ONE shared
        // reader (ReadConsumable), so a food or drink with a restore or a buff effect passes like a potion; a poison or a drug never does.
        const auto item = ReadConsumable(alc);
        if (item.hostile) return false;
        const auto& fx = item.fx;
        bool restores = false;
        for (std::size_t i = 0; i < Iaus::kRestoreFamilies; ++i)
            if (item.restoreMag[i] > 0.0f || item.foodRestoreMag[i] > 0.0f) restores = true;
        if (fx.empty() && !restores) return false;

        const std::int32_t carriedBefore = CarriedCount(a_follower, alc);
        if (carriedBefore <= 0) return false;

        // Its own duration: do not drink the rest of the stack while the first is still working (the restore potions' M5 gate).
        const std::uint64_t key = (static_cast<std::uint64_t>(a_follower->GetFormID()) << 32) | a_potion;
        const auto now = Clock::now();
        if (auto it = g_buffUntil.find(key); it != g_buffUntil.end() && now < it->second) return false;

        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) return false;
        if (const auto pend = g_pendingDrink.find(key); pend != g_pendingDrink.end()) { ConfirmOne(a_follower, key, pend->second); g_pendingDrink.erase(pend); }
        mgr->EquipObject(a_follower, alc, nullptr, 1);   // the AlchemyItem equip path IS the vanilla drink; one unit, the engine removes it
        g_pendingDrink[key] = PendingDrink{ carriedBefore, std::string(alc->GetName()) };   // proven (or not) on the next pass: ConfirmDrinks

        g_buffUntil[key] = now + std::chrono::milliseconds(static_cast<int>((std::max)(1.0f, item.maxDuration) * 1000.0f));   // the reader's longest effect (restores included) gates the stack
        return true;
    }

    void ConfirmDrinks(RE::Actor* a_follower) {
        if (!a_follower || g_pendingDrink.empty()) return;
        const auto fid = static_cast<std::uint64_t>(a_follower->GetFormID());
        for (auto it = g_pendingDrink.begin(); it != g_pendingDrink.end();) {
            if ((it->first >> 32) == fid) { ConfirmOne(a_follower, it->first, it->second); it = g_pendingDrink.erase(it); }
            else ++it;
        }
    }

    void ClearBuffDrinks() { g_buffUntil.clear(); g_pendingDrink.clear(); }
}
