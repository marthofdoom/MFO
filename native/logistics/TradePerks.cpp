// logistics/TradePerks.cpp -- GUARDED ENGINE QUERIES for town phase 2 (round S, ClickUp 86e3tambj).
// Contract, threading and the proven argument shapes: logistics/TradePerks.h.
//
// Each wrapper calls the engine through its OWN REL::Relocation, with the function type read from
// the disassembly (not from CommonLib's declaration), at exactly the address its self-check row
// verified. Evidence per row: Docs/VERIFIED-ADDRESSES.md (rows "TradePerks.*") and
// TOWN-PHASE2-DESIGN.md R0-1 / R0-4 / R0-7.
#include "logistics/TradePerks.h"
#include "Runtime.h"   // Known(), SeatVerified(): the exact-build gate and the mit-3.7 F1 self-check

namespace MFO::Logistics::TradePerks {

    namespace {
        // ── The three engine functions, typed from the disassembly (1.6.1170 / 1.5.97; 1.7.104 is
        // the 1.6.1170 body at the idmap RVA, verified by the generator) ──

        // HandleEntryPoint: C-variadic. ecx = ep (movsxd; ep >= 0x5C returns at once), rdx = owner
        // (null returns), then the condition arguments and the value pointers, all 8-byte pointers:
        // r8/r9 are spilled to their home slots and every argument is read from that spill area.
        // Returns nothing the callers use (the engine's own barter / smithing callers ignore rax).
        using EntryPointFn = void(std::uint32_t, RE::Actor*, ...);
        REL::Relocation<EntryPointFn*>& EntryPointRel() {
            static REL::Relocation<EntryPointFn*> fn{ REL::RelocationID(23073, 23526) };
            return fn;
        }

        // TESCondition::IsTrue: rcx = the condition (its first member is the item list head),
        // rdx = action ref, r8 = target ref; returns al.
        using ConditionFn = bool(const RE::TESCondition*, RE::TESObjectREFR*, RE::TESObjectREFR*);
        REL::Relocation<ConditionFn*>& ConditionRel() {
            static REL::Relocation<ConditionFn*> fn{ REL::RelocationID(29074, 29888) };
            return fn;
        }

        // TESQuest::CreateRefHandleByAliasID: rcx = quest, rdx = the out handle, r8d = alias id.
        // Takes quest->aliasAccessLock for read, writes the invalid handle, then the alias's
        // handle when the alias exists, releases the lock and returns the out handle.
        using AliasFn = RE::ObjectRefHandle&(RE::TESQuest*, RE::ObjectRefHandle&, std::uint32_t);
        REL::Relocation<AliasFn*>& AliasRel() {
            static REL::Relocation<AliasFn*> fn{ REL::RelocationID(24537, 25066) };
            return fn;
        }

        // One verdict per seat, computed once (thread-safe static). An unsupported build is
        // refused BEFORE any id is resolved, so no relocation is built through an id that build
        // may not hold. SeatVerified logs its own refusal line by seat name.
        template <class RelFn>
        bool SeatOk(const char* a_seat, RelFn a_rel) {
            if (!Runtime::Known()) {
                spdlog::error("[selfcheck] seat {} REFUSED: runtime {} is not a supported build ({}) -- "
                              "the town phase-2 query it serves returns no answer",
                              a_seat, REL::Module::get().version().string("."), Runtime::GateReason());
                return false;
            }
            return Runtime::SeatVerified(a_rel().address(), a_seat);
        }

        bool EntryPointSeatOk() {
            static const bool ok = SeatOk("TradePerks.BGSEntryPoint.HandleEntryPoint", EntryPointRel);
            return ok;
        }
        bool ConditionSeatOk() {
            static const bool ok = SeatOk("TradePerks.TESCondition.IsTrue", ConditionRel);
            return ok;
        }
        bool AliasSeatOk() {
            static const bool ok = SeatOk("TradePerks.TESQuest.CreateRefHandleByAliasID", AliasRel);
            return ok;
        }

        // The proven condition-argument count per entry point (the engine's own EP table, read in
        // R0-1, AE 0x1FF6110: argCount counts the Perk Owner, so condition args = argCount - 1).
        // Every one of these carries exactly ONE float value. -1 = not proven: refused.
        int ProvenCondArgs(RE::BGSEntryPoint::ENTRY_POINT a_ep) {
            using EP = RE::BGSEntryPoint::ENTRY_POINT;
            switch (a_ep) {
            case EP::kModBuyPrices:             return 1;   //  8 Perk Owner, Target
            case EP::kModSellPrices:            return 1;   // 60 Perk Owner, Target
            case EP::kModTemperingHealth:       return 1;   // 76 Perk Owner, Item
            case EP::kModLockpickSweetSpot:     return 1;   // 59 Perk Owner, Locked Ref
            case EP::kSetLockpickStartingArc:   return 0;   // 63 Perk Owner
            case EP::kMakeLockpicksUnbreakable: return 0;   // 65 Perk Owner
            // The perk-effect reader's shapes (ClickUp 1dfcuabd2pk; call sites decoded on 1.6.1170 / 1.5.97 / 1.7.104 with
            // tools/entrypoint_census, listed in Docs/research/2026-11/perk-effects.md section 2 and ENGINE_NOTES 0.54):
            case EP::kModPercentBlocked:        return 0;   // 39 Perk Owner
            case EP::kModSpellCost:             return 1;   // 38 Perk Owner, Spell
            case EP::kCanDualCastSpell:         return 1;   // 75 Perk Owner, Spell
            case EP::kModArmorRating:           return 1;   // 85 Perk Owner, Armor
            case EP::kModSpellMagnitude:        return 2;   // 29 Perk Owner, Spell, Target
            case EP::kModAttackDamage:          return 2;   // 35 Perk Owner, Weapon, Target
            default:                            return -1;
            }
        }

        // What the engine reads condition argument a_index of a_ep AS (MFO-B323: a form of another type would be read as that
        // type). The names are the engine's own argument names for the entry point; the types are what its call sites pass.
        enum class ArgKind { Refr, Bound, Spell, Armor, Weapon };
        ArgKind ArgKindOf(RE::BGSEntryPoint::ENTRY_POINT a_ep, int a_index) {
            using EP = RE::BGSEntryPoint::ENTRY_POINT;
            if (a_index == 1) return ArgKind::Refr;   // the second condition argument of both two-argument shapes is the Target
            switch (a_ep) {
            case EP::kModTemperingHealth: return ArgKind::Bound;
            case EP::kModSpellCost: case EP::kCanDualCastSpell: case EP::kModSpellMagnitude: return ArgKind::Spell;
            case EP::kModArmorRating:     return ArgKind::Armor;
            case EP::kModAttackDamage:    return ArgKind::Weapon;
            default:                      return ArgKind::Refr;   // 8 / 60 Target, 59 Locked Ref
            }
        }
        bool ArgTypeOk(RE::BGSEntryPoint::ENTRY_POINT a_ep, int a_index, RE::TESForm* a_arg) {
            const char* want = "reference";
            bool ok = false;
            switch (ArgKindOf(a_ep, a_index)) {
            case ArgKind::Refr:   ok = a_arg->As<RE::TESObjectREFR>() != nullptr; break;
            case ArgKind::Bound:  ok = a_arg->IsBoundObject(); want = "bound object"; break;
            case ArgKind::Spell:  ok = a_arg->As<RE::MagicItem>() != nullptr; want = "magic item"; break;
            case ArgKind::Armor:  ok = a_arg->Is(RE::FormType::Armor); want = "armor"; break;
            case ArgKind::Weapon: ok = a_arg->Is(RE::FormType::Weapon); want = "weapon"; break;
            }
            if (!ok)
                spdlog::error("[trade] EntryPointValue REFUSED: entry point {} condition argument {} ({:08X}) is not a {} "
                              "-- no answer",
                              std::to_underlying(a_ep), a_index, a_arg->GetFormID(), want);
            return ok;
        }

        // Shape + seat + owner checks shared by both overloads. Refusals log every time: each is a
        // caller bug or an unloaded owner, and the caller is required to query at plan time only.
        bool EntryPointCallable(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner, int a_condArgs) {
            const int proven = ProvenCondArgs(a_ep);
            if (proven != a_condArgs) {
                spdlog::error("[trade] EntryPointValue REFUSED: entry point {} called with {} condition argument(s), "
                              "proven shape {} -- no answer",
                              std::to_underlying(a_ep), a_condArgs,
                              proven < 0 ? std::string("none (not a proven entry point)") : std::to_string(proven));
                return false;
            }
            if (!EntryPointSeatOk() || !a_owner) return false;
            // R0-4: with no process, no middle-high data or no perk data, the engine's walk finds no
            // entries and leaves the seed untouched -- an answer that LOOKS like "no perk applies".
            // Same reads as the engine (1.6.1170 id 40024, 1.5.97 id 38965, 1.7.104 0x7283D0:
            // process->middleHigh (+0x8) -> perkData (+0x288)).
            auto* proc = a_owner->GetActorRuntimeData().currentProcess;
            if (!proc || !proc->middleHigh || !proc->middleHigh->perkData) {
                spdlog::warn("[trade] {:08X}: perks NOT evaluated for entry point {} ({}) -- evaluate while he is "
                             "loaded and cache the result",
                             a_owner->GetFormID(), std::to_underlying(a_ep),
                             !proc ? "no process" : !proc->middleHigh ? "no middle-high process data" : "no perk data");
                return false;
            }
            return true;
        }
    }

    std::optional<float> EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner, float a_seed) {
        if (!EntryPointCallable(a_ep, a_owner, 0)) return std::nullopt;
        float v = a_seed;
        EntryPointRel()(std::to_underlying(a_ep), a_owner, &v);
        return v;
    }

    std::optional<float> EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner,
                                         RE::TESForm* a_arg, float a_seed) {
        if (!a_arg) {
            spdlog::error("[trade] EntryPointValue REFUSED: entry point {} with a null condition argument "
                          "(the engine never null-checks it) -- no answer",
                          std::to_underlying(a_ep));
            return std::nullopt;
        }
        // MFO-B323: the conditions read the argument as the engine's own type for that entry point (ArgKindOf). A form of
        // the wrong type would be read as that type, so refuse it.
        if (!ArgTypeOk(a_ep, 0, a_arg)) return std::nullopt;
        if (!EntryPointCallable(a_ep, a_owner, 1)) return std::nullopt;
        float v = a_seed;
        EntryPointRel()(std::to_underlying(a_ep), a_owner, a_arg, &v);
        return v;
    }

    std::optional<float> EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner,
                                         RE::TESForm* a_arg, RE::TESObjectREFR* a_target, float a_seed) {
        if (!a_arg || !a_target) {
            spdlog::error("[trade] EntryPointValue REFUSED: entry point {} with a null condition argument "
                          "(no null is passed here: the two-argument shapes are asked with a real item and a real target) -- no answer",
                          std::to_underlying(a_ep));
            return std::nullopt;
        }
        if (!ArgTypeOk(a_ep, 0, a_arg) || !ArgTypeOk(a_ep, 1, a_target)) return std::nullopt;
        if (!EntryPointCallable(a_ep, a_owner, 2)) return std::nullopt;
        float v = a_seed;
        EntryPointRel()(std::to_underlying(a_ep), a_owner, a_arg, a_target, &v);
        return v;
    }

    bool EntryPointEvaluable(RE::Actor* a_owner) {
        if (!a_owner || !EntryPointSeatOk()) return false;
        auto* proc = a_owner->GetActorRuntimeData().currentProcess;
        return proc && proc->middleHigh && proc->middleHigh->perkData;
    }

    bool EntryPointSeatVerified() {
        return EntryPointSeatOk();
    }

    std::optional<bool> ConditionTrue(const RE::TESCondition& a_cond, RE::TESObjectREFR* a_action,
                                      RE::TESObjectREFR* a_target) {
        if (!ConditionSeatOk()) return std::nullopt;
        return ConditionRel()(&a_cond, a_action, a_target);
    }

    std::optional<RE::ObjectRefHandle> AliasRefHandle(RE::TESQuest* a_quest, std::uint32_t a_aliasID) {
        if (!a_quest || !AliasSeatOk()) return std::nullopt;
        RE::ObjectRefHandle h{};
        AliasRel()(a_quest, h, a_aliasID);
        return h;
    }

    bool AliasReaderVerified() {
        return AliasSeatOk();
    }
}
