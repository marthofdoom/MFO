// logistics/Silver.cpp -- THE SILVER WEAPON (ClickUp 86e3p2gp9, marth 2026-10-12: "We need an
// equip silver gambit and a silver weapon tracking system independent of main weapon").
//
// Owns the KNOWLEDGE half of the feature; the hand half (act.equip_silver, the switch back to the
// main weapon) lives in cast/EquipSilver.cpp and reads only the public API below (Logistics.h).
//
//   * UNHOLY (cond.foe_is_unholy, marth named it): "exactly what the load order's silver bonuses
//     hit". Not a hand-written list: the target-tab conditions of the LIVE SilverPerk record
//     (Skyrim.esm 0x10D685, whatever plugin overrides it last) and the conditions of the LIVE
//     silver-enchantment effect (Skyrim.esm MGEF 0x10B116 EnchSilverSwordFFContact) are read
//     once, at first use, and interpreted here. Measured on Tuxborn (local "Tuxborn - Deck"
//     profile, 1409 plugins, 2026-10-12): the winning SilverPerk is Tales of Skyrim -
//     Berserkyr.esp's (WerewolfBeastRace OR DLC2WerebearBeastRace OR ActorTypeUndead OR
//     ActorTypeGhost) and the winning MGEF is also Berserkyr's (two Berserkyr werebear factions
//     OR ActorTypeUndead OR ActorTypeGhost OR WerewolfFaction). Vanilla vampire races all carry
//     ActorTypeUndead, so vampires are covered through it.
//     Interpreted functions: HasKeyword (the engine's own actor keyword read, TESObjectREFR::
//     HasKeyword: race, base NPC and quest-alias keywords), GetIsRace, GetInFaction -- run on Subject only, a literal comparison value. Anything
//     else is an UNKNOWN item: logged once, LOUDLY, and evaluated FALSE (fails toward "not
//     unholy", the evaluator's fail-closed rule). With neither record readable (or nothing in
//     them this file understands) the FALLBACK is marth's: ActorTypeUndead, Vampire, or
//     WerewolfBeastRace -- also logged.
//     We evaluate the conditions ourselves rather than calling TESCondition::IsTrue: the
//     Evaluator asks this inside the combat group's READ lock on the job worker (#23: pure reads,
//     nothing else inside), and these three functions are plain data reads there.
//   * SILVER WEAPON: a weapon carrying a keyword the SilverPerk's WEAPON tab tests positively
//     (WeapMaterialSilver in every list measured; that keyword is the fallback), or one whose
//     enchantment carries the silver effect itself. Melee only for the gambit (bow / crossbow /
//     staff never qualify): the silver bonus is a strike bonus and the gambit replaces the main
//     weapon in the weapon hand.
//   * THE TRACKED SLOT: deterministic, so NOTHING IS SAVED -- the follower's best owned silver
//     melee weapon (WeaponScore, the loot judge's own ranking), plus his best one-handed one
//     (what the gambit uses when his off-hand is busy). A follower whose combat table holds an
//     act.equip_silver rule (enabled or not: a rule switched off for a moment still says he
//     keeps one) never sells, drops or hands back either (SwapUp's keep set, the shed).
//     "Never replaced by best weapon logic": the keep set protects them whatever the 1H / 2H
//     buckets decide.
//   * RESTOCK: an ENABLED silver rule and no silver melee weapon owned -> the loot judge takes one
//     (stocked, never equipped in place) and the buy planner buys one (under bEconomyBuyGear).
//
// THREADING: the rule tables are built once under std::call_once and are immutable after, so
// IsUnholy / IsSilverWeapon are safe on any thread (worker, the Papyrus VM thread's PlanBuy).
// The inventory walks (BestSilverWeapon, TrackedSilver, WantsSilverWeapon) are worker / main
// pump only like every other GetInventory caller here.
#include "Logistics_internal.h"
#include <mutex>

namespace MFO::Logistics {

    namespace {

        // Skyrim.esm forms (ids measured off Tuxborn's Skyrim.esm, 2026-10-12).
        constexpr RE::FormID kSilverPerk          = 0x0010D685;   // PERK SilverPerk
        constexpr RE::FormID kSilverEnchEffect    = 0x0010B116;   // MGEF EnchSilverSwordFFContact
        constexpr RE::FormID kWeapMaterialSilver  = 0x0010AA1A;   // KYWD WeapMaterialSilver
        constexpr RE::FormID kActorTypeUndead     = 0x00013796;   // KYWD ActorTypeUndead
        constexpr RE::FormID kVampire             = 0x000A82BB;   // KYWD Vampire
        constexpr RE::FormID kWerewolfBeastRace   = 0x000CDD84;   // RACE WerewolfBeastRace

        enum class Fn : std::uint8_t { kHasKeyword, kGetIsRace, kGetInFaction, kUnknown };

        struct Item {
            Fn                                fn    = Fn::kUnknown;
            const RE::TESForm*                form  = nullptr;
            RE::CONDITION_ITEM_DATA::OpCode   op    = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
            float                             comp  = 1.0f;
            bool                              isOR  = false;
        };
        struct Rule {
            std::vector<Item> items;
            std::string       source;   // for the log
        };
        struct Rules {
            std::vector<Rule>                      unholy;          // OR across rules
            std::vector<const RE::BGSKeyword*>     silverKeywords;
            const RE::EffectSetting*               silverEffect = nullptr;
            bool                                   fallback = false;
        };

        std::once_flag g_once;
        Rules          g_rules;

        const char* FnName(Fn a_fn) {
            switch (a_fn) {
            case Fn::kHasKeyword:   return "HasKeyword";
            case Fn::kGetIsRace:    return "GetIsRace";
            case Fn::kGetInFaction: return "GetInFaction";
            default:                return "?";
            }
        }
        std::string FormLabel(const RE::TESForm* a_form) {
            if (!a_form) return "none";
            const char* e = a_form->GetFormEditorID();
            return std::format("{:08X}{}{}", a_form->GetFormID(), (e && *e) ? " " : "", (e && *e) ? e : "");
        }
        std::string LastFile(const RE::TESForm* a_form) {
            const auto* f = a_form ? a_form->GetFile(-1) : nullptr;
            return f ? std::string(f->GetFilename()) : std::string("?");
        }

        // Positive "has it" test (== 1, != 0, > 0, >= 1): the weapon tab's keyword reading.
        bool PositiveTest(const RE::CONDITION_ITEM_DATA& a_d) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            if (a_d.flags.global) return false;
            const float v = a_d.comparisonValue.f;
            switch (a_d.flags.opCode) {
            case Op::kEqualTo:              return v >= 1.0f;
            case Op::kNotEqualTo:           return v == 0.0f;
            case Op::kGreaterThan:          return v >= 0.0f;
            case Op::kGreaterThanOrEqualTo: return v > 0.0f;
            default:                        return false;
            }
        }

        bool Compare(float a_value, RE::CONDITION_ITEM_DATA::OpCode a_op, float a_comp) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            switch (a_op) {
            case Op::kEqualTo:              return a_value == a_comp;
            case Op::kNotEqualTo:           return a_value != a_comp;
            case Op::kGreaterThan:          return a_value >  a_comp;
            case Op::kGreaterThanOrEqualTo: return a_value >= a_comp;
            case Op::kLessThan:             return a_value <  a_comp;
            case Op::kLessThanOrEqualTo:    return a_value <= a_comp;
            }
            return false;
        }

        // One condition list -> one Rule. Each item's run-on must be Subject and its comparison a
        // literal; HasKeyword / GetIsRace / GetInFaction with a resolved form of the right type are
        // understood, everything else is kept as an UNKNOWN item (false) and named in the log.
        Rule ReadRule(const RE::TESCondition& a_cond, std::string a_source, int& a_known, int& a_unknown) {
            using FID = RE::FUNCTION_DATA::FunctionID;
            Rule r;
            r.source = std::move(a_source);
            for (auto* it = a_cond.head; it; it = it->next) {
                const auto& d = it->data;
                Item item;
                item.isOR = d.flags.isOR;
                item.op   = d.flags.opCode;
                item.comp = d.flags.global ? 0.0f : d.comparisonValue.f;
                const auto fn = d.functionData.function.get();
                auto* form    = static_cast<RE::TESForm*>(d.functionData.params[0]);
                const bool subjectLiteral = !d.flags.global &&
                                            d.object.get() == RE::CONDITIONITEMOBJECT::kSelf;
                if (subjectLiteral && form) {
                    if (fn == FID::kHasKeyword && form->As<RE::BGSKeyword>())       item.fn = Fn::kHasKeyword;
                    else if (fn == FID::kGetIsRace && form->As<RE::TESRace>())      item.fn = Fn::kGetIsRace;
                    else if (fn == FID::kGetInFaction && form->As<RE::TESFaction>()) item.fn = Fn::kGetInFaction;
                }
                if (item.fn == Fn::kUnknown) {
                    ++a_unknown;
                    spdlog::warn("[silver] {}: condition function {} (run-on {}, {}) is not one MFO reads -- "
                                 "it counts as FALSE for Unholy", r.source, static_cast<int>(fn),
                                 static_cast<int>(d.object.get()), d.flags.global ? "global comparison" : "literal");
                } else {
                    item.form = form;
                    ++a_known;
                }
                r.items.push_back(item);
            }
            return r;
        }

        void Build() {
            Rules out;
            // 1. SilverPerk: every weapon-damage entry point's WEAPON tab (1) names the silver
            //    keywords, its TARGET tab (2) is an Unholy rule.
            if (auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(kSilverPerk)) {
                const std::string file = LastFile(perk);
                int entryNo = 0;
                for (auto* entry : perk->perkEntries) {
                    if (!entry) continue;
                    if (static_cast<int>(entry->GetType()) != static_cast<int>(RE::PERK_ENTRY_TYPE::kEntryPoint)) continue;
                    auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
                    const auto point = ep->entryData.entryPoint.get();
                    if (point != RE::BGSEntryPoint::ENTRY_POINTS::kCalculateWeaponDamage &&
                        point != RE::BGSEntryPoint::ENTRY_POINTS::kModAttackDamage) continue;
                    ++entryNo;
                    const std::size_t tabs = std::min<std::size_t>(ep->entryData.numArgs, ep->conditions.size());
                    if (tabs >= 2) {
                        for (auto* it = ep->conditions[1].head; it; it = it->next) {
                            const auto& d = it->data;
                            if (d.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kHasKeyword &&
                                d.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kWornHasKeyword) continue;
                            if (!PositiveTest(d)) continue;
                            auto* kw = static_cast<RE::TESForm*>(d.functionData.params[0]);
                            auto* k  = kw ? kw->As<RE::BGSKeyword>() : nullptr;
                            if (k && std::find(out.silverKeywords.begin(), out.silverKeywords.end(), k) == out.silverKeywords.end())
                                out.silverKeywords.push_back(k);
                        }
                    }
                    if (tabs >= 3 && ep->conditions[2].head) {
                        int known = 0, unknown = 0;
                        Rule r = ReadRule(ep->conditions[2], std::format("SilverPerk entry {} ({})", entryNo, file),
                                          known, unknown);
                        if (known > 0) out.unholy.push_back(std::move(r));
                    }
                }
            } else {
                spdlog::warn("[silver] SilverPerk (Skyrim.esm {:08X}) not found -- Unholy falls back", kSilverPerk);
            }
            // 2. The silver enchantment's effect: its own conditions, run on the struck actor.
            if (auto* fx = RE::TESForm::LookupByID<RE::EffectSetting>(kSilverEnchEffect)) {
                out.silverEffect = fx;
                if (fx->conditions.head) {
                    int known = 0, unknown = 0;
                    Rule r = ReadRule(fx->conditions, std::format("silver enchantment effect ({})", LastFile(fx)),
                                      known, unknown);
                    if (known > 0) out.unholy.push_back(std::move(r));
                }
            }
            // 3. Fallbacks (marth's): the silver keyword; undead / vampires / werewolves.
            if (out.silverKeywords.empty())
                if (auto* k = RE::TESForm::LookupByID<RE::BGSKeyword>(kWeapMaterialSilver)) out.silverKeywords.push_back(k);
            if (out.unholy.empty()) {
                out.fallback = true;
                Rule r;
                r.source = "fallback (no readable silver perk / enchantment conditions)";
                auto add = [&r](Fn a_fn, RE::FormID a_id) {
                    if (auto* f = RE::TESForm::LookupByID(a_id)) r.items.push_back(Item{ a_fn, f, RE::CONDITION_ITEM_DATA::OpCode::kEqualTo, 1.0f, true });
                };
                add(Fn::kHasKeyword, kActorTypeUndead);
                add(Fn::kHasKeyword, kVampire);
                add(Fn::kGetIsRace, kWerewolfBeastRace);
                if (!r.items.empty()) out.unholy.push_back(std::move(r));
            }
            // The resolved rules, once (the field read: what does Unholy MEAN in this list?).
            for (const auto& r : out.unholy) {
                std::string s;
                for (const auto& i : r.items) {
                    s += std::format("{}{}({}){}", s.empty() ? "" : " ", i.fn == Fn::kUnknown ? "UNKNOWN" : FnName(i.fn),
                                     FormLabel(i.form), i.isOR ? " OR" : " AND");
                }
                spdlog::info("[silver] Unholy rule from {}{}: {}", r.source, out.fallback ? " [FALLBACK]" : "", s);
            }
            std::string kws;
            for (auto* k : out.silverKeywords) kws += (kws.empty() ? "" : ", ") + FormLabel(k);
            spdlog::info("[silver] silver weapon = keyword {}{}", kws.empty() ? "none" : kws,
                         out.silverEffect ? std::format(" or enchantment effect {}", FormLabel(out.silverEffect)) : "");
            g_rules = std::move(out);
        }

        const Rules& R() {
            std::call_once(g_once, Build);
            return g_rules;
        }

        // The engine's OWN actor keyword read (review F4 on 102e24c): TESObjectREFR::HasKeyword ->
        // the HasKeywordHelper vfunc 0x48 (fork 57be9d67, RE/T/TESObjectREFR.h:428 / Actor.h:290), the
        // function the HasKeyword condition itself uses -- race, base NPC AND quest-alias keywords.
        // A data read, fit for the combat group's read lock like IsInFaction beside it.
        bool ActorHasKeyword(RE::Actor* a_actor, const RE::BGSKeyword* a_kw) {
            return a_kw && a_actor->HasKeyword(a_kw);
        }

        float ItemValue(const Item& a_item, RE::Actor* a_actor) {
            switch (a_item.fn) {
            case Fn::kHasKeyword:   return ActorHasKeyword(a_actor, a_item.form->As<RE::BGSKeyword>()) ? 1.0f : 0.0f;
            case Fn::kGetIsRace:    return a_actor->GetRace() == a_item.form ? 1.0f : 0.0f;
            case Fn::kGetInFaction: return a_actor->IsInFaction(a_item.form->As<RE::TESFaction>()) ? 1.0f : 0.0f;
            default:                return 0.0f;
            }
        }

        // The engine's list semantics: OR binds tighter than AND. An item flagged OR joins the
        // next item's group; a group closes at an item without the flag (or at the end).
        bool RuleTrue(const Rule& a_rule, RE::Actor* a_actor) {
            bool all = true, group = false, open = false;
            for (const auto& i : a_rule.items) {
                const bool v = i.fn != Fn::kUnknown && Compare(ItemValue(i, a_actor), i.op, i.comp);
                group = group || v;
                open  = true;
                if (!i.isOR) { all = all && group; group = false; open = false; }
            }
            if (open) all = all && group;
            return all;
        }

        bool IsSilverMelee(const RE::TESObjectWEAP* a_w) {
            if (!a_w || a_w->IsStaff() || a_w->IsBow() || a_w->IsCrossbow()) return false;
            if (a_w->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee) return false;
            if (IsCreatureWeapon(a_w)) return false;   // non-playable: invisible on a humanoid
            return IsSilverWeapon(a_w);
        }

        bool HasSilverRule(const FollowerState& a_state, bool a_enabledOnly) {
            for (const auto& g : a_state.combat())
                if (g.actionOpcode == Vocab::kActEquipSilver && (g.enabled || !a_enabledOnly)) return true;
            return false;
        }
    }

    void PrimeSilverRules() { (void)R(); }

    bool IsSilverWeapon(const RE::TESObjectWEAP* a_weap) {
        if (!a_weap) return false;
        const Rules& r = R();
        for (auto* k : r.silverKeywords)
            if (a_weap->HasKeyword(k)) return true;
        if (r.silverEffect && a_weap->formEnchanting)
            for (auto* e : a_weap->formEnchanting->effects)
                if (e && e->baseEffect == r.silverEffect) return true;
        return false;
    }

    bool IsUnholy(RE::Actor* a_actor) {
        if (!a_actor) return false;
        for (const auto& rule : R().unholy)
            if (RuleTrue(rule, a_actor)) return true;
        return false;
    }

    RE::TESObjectWEAP* BestSilverWeapon(RE::Actor* a_follower, const FollowerState* a_state, bool a_oneHandOnly) {
        if (!a_follower) return nullptr;
        const WeaponRoles roles = a_state ? ComputeWeaponRoles(a_follower, *a_state) : WeaponRoles{};
        RE::TESObjectWEAP* best = nullptr; float bestScore = -1.0f;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* w = obj->As<RE::TESObjectWEAP>();
            if (!IsSilverMelee(w)) continue;
            if (a_oneHandOnly && WeaponClassOf(w->GetWeaponType()) != WepClass::OneHand) continue;
            const float s = WeaponScore(roles, w);
            if (s > bestScore) { bestScore = s; best = w; }
        }
        return best;
    }

    std::pair<RE::TESBoundObject*, RE::TESBoundObject*> TrackedSilver(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower || !HasSilverRule(a_state, /*a_enabledOnly*/ false)) return { nullptr, nullptr };
        return { BestSilverWeapon(a_follower, &a_state, false), BestSilverWeapon(a_follower, &a_state, true) };
    }

    bool SilverNeedsOneHand(RE::Actor* a_follower, const FollowerState& a_state) {
        return a_follower && (ComputeWeaponRoles(a_follower, a_state).offHand != 0 || IsCasterFollower(a_state));
    }

    bool WantsSilverWeapon(RE::Actor* a_follower, const FollowerState& a_state) {
        if (!a_follower || !HasSilverRule(a_state, /*a_enabledOnly*/ true)) return false;
        // Review F2 on 102e24c: only a silver weapon the gambit can USE counts as owned -- with the
        // off-hand in use (a shield / second weapon by perks, or a caster's spell hand) that is a
        // one-hander, so a lone silver greatsword does not end the restock.
        const bool oneHand = SilverNeedsOneHand(a_follower, a_state);
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0) continue;
            auto* w = obj->As<RE::TESObjectWEAP>();
            if (!IsSilverMelee(w)) continue;
            if (oneHand && WeaponClassOf(w->GetWeaponType()) != WepClass::OneHand) continue;
            return false;
        }
        return true;
    }

    bool IsSilverMeleeWeapon(const RE::TESObjectWEAP* a_weap) { return IsSilverMelee(a_weap); }

}
