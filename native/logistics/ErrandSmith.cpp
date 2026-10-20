// logistics/ErrandSmith.cpp -- TOWN PHASE 2 R8 (ClickUp 86e3t1mx7): SMITHING / TEMPERING ERRANDS.
// Design: _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md round R8 / item 1.f and "R0 RESULTS" R0-3..R0-5, rulings Q5 and
// "ONE TRIP PER TOWN VISIT" (TOWN-PHASE2-RULINGS.md).
//
// WHAT HE DOES. A follower with a WORN weapon or armor piece his Smithing can improve takes it into his town trip: the planner
// (Augment) appends a Smith stop after the shops (the temple and the inn, R7, come after it), the shop stops buy the improve
// recipe's materials whatever his gambits say (FillMaterials -> BuyThresholds::smithMats), the field loots them while they are on
// his want list (WantsMaterials / Wanted), and at the smithy he works at a sharpening wheel (weapons) or an armor table (armor).
//
// THE WANT LIST (BuildOnMain, MAIN thread, the follower loaded). For every worn weapon / armor piece (one line per worn copy, so a
// dual-wielded pair is two lines):
//   * the engine's temper formula, PROVEN in R0-3 from the SmithingMenu improve-list build (AE id 51360):
//         raw    = max(1, ((skill - 15) * fSmithingConditionFactor + 1) * 0.01 * (fHealthDataValue6 - fHealthDataValue1) + fHealthDataValue1)
//         delta  = raw - fHealthDataValue1
//         delta > 0 : HandleEntryPoint(76 kModTemperingHealth, owner = the follower, Item = the worn base object, &delta)
//                     target = round_half_up((max(delta, 0) + fHealthDataValue1) * 10) / 10
//         else      : target = raw
//     skill = his Smithing AV, UNCLAMPED (fortify counts); the three constants are GMSTs read BY NAME at use (Requiem / LoreRim may
//     override them); the entry point goes through TradePerks::EntryPointValue (self-checked seat, main thread, loaded owner).
//   * the piece is wanted when target exceeds the worn copy's ExtraHealth (default 1.0) by a tempering step (0.05).
//   * THE IMPROVE RECIPE. Verified in the records (Skyrim.esm, tools/esp_inspect.py): a temper recipe is a COBJ whose created
//     object (CNAM) is the BASE ITEM ITSELF, whose bench keyword (BNAM) is CraftingSmithingSharpeningWheel 00088108 (weapons) or
//     CraftingSmithingArmorTable 000ADB78 (armor), and whose components (CNTO) are the materials (one iron ingot for TemperWeapon-
//     IronSword 000ADB77). Verified in the code (1.6.1170 disassembly of the improve-list build at 0x90b860..0x90b978): the menu
//     looks the item up in a hash map of recipes keyed by the created object's FormID and, on a miss, retries with the weapon's
//     templateWeapon (+0x208) / the armor's templateArmor (+0x220). This file does the same (FindRecipes) over an index built once
//     from every COBJ with one of the two bench keywords.
//   * THE RECIPE'S CONDITIONS. Vanilla's are `EPTemperingItemIsEnchanted == 0 OR HasPerk(ArcaneBlacksmith)`. The item-context
//     functions (659 / 660) read the engine's tempering context, which MFO does not own, so a recipe that has one is evaluated by
//     hand: an OR-group is TRUE when one of its item-context nodes is known true (IsEnchanted is answered from the worn copy and the
//     base form); a group that would need another node evaluated on its own is INDETERMINATE and the piece is skipped (logged once
//     per recipe). Today that means an ENCHANTED piece is not tempered (it needs the engine's own context); backlog. A recipe with
//     no item-context node goes whole through TradePerks::ConditionTrue(follower).
//   * the materials are the recipe components; the want list keeps what he carries and the SHORTFALL (the buy list).
//
// APPLYING IT (ApplyOnMain / VerifyOnMain, MAIN thread). R0-5: WornObject.SetItemHealthPercent writes ExtraHealth.health (an ABSOLUTE
// multiplier, 1.1 = Fine) on the worn copy -- armor by biped slot (pure inventory data), a weapon through the actor's process.
// It is dispatched through Papyrus (a global SKSE native, no engine-address seat), then the ExtraHealth is READ BACK natively and
// ONLY a piece that took has its materials consumed (RemoveItem); a piece that did not take costs nothing and says why (probe
// P-SMITH-W for weapons). MFO never animates or moves him: the station animation is a Harbinger ch.9 offer (OfferWork).
//
// STAGING (kTripHooked). The module is complete, but the trip chain's hook points (Errand.cpp TryStart / Tick, ErrandSim.cpp) belong to
// R7's marked insertion point and are made once R7 is merged. Until then kTripHooked is FALSE and the three entry points that would
// make a follower BUY or LOOT materials (Prepare, FillMaterials, WantsMaterials) answer "off": a follower must not shop for
// materials a trip can never use. Flipping it is part of the hook-up.
//
// THREADING: see Errand_internal.h (Smith). One leaf mutex; the engine, Papyrus and TradeBridge are never called under it.
#include "PCH.h"
#include "Errand_internal.h"
#include "TradePerks.h"
#include "Config.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "Scheduler.h"
#include "apmf/APMFBridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Smith {

    namespace {
        // STAGING GATE, see the header comment. Flipped to true by the hook-up round (after R7 merges).
        constexpr bool kTripHooked = false;

        constexpr RE::FormID kKwSharpeningWheel = 0x00088108;   // CraftingSmithingSharpeningWheel (weapon temper bench)
        constexpr RE::FormID kKwArmorTable      = 0x000ADB78;   // CraftingSmithingArmorTable (armor temper bench)
        constexpr float      kTemperStep        = 0.05f;        // health moves in tenths; less than a half step is no improvement
        constexpr float      kReadbackTol       = 0.005f;       // a written float reads back exactly; this only absorbs the compare
        constexpr float      kStationRadius     = 1500.0f;      // a smithy's furniture is found within this of the follower (the design's 1500 u)
        constexpr auto       kFresh             = std::chrono::seconds(30);    // a want list younger than this is current for a plan
        constexpr auto       kLootRefresh       = std::chrono::seconds(120);   // the field's refresh cadence while materials are wanted
        constexpr auto       kPendingStale      = std::chrono::seconds(15);    // a queued build that never ran is re-queued
        constexpr double     kBuildWaitSec      = 10.0;         // the physical stop waits this long for its fresh build
        constexpr double     kEntrySec          = 10.0;         // furniture entry is confirmed within this of the offer (design 1.f)
        constexpr double     kRealCapSec        = 150.0;        // the work never takes more real seconds than this (a slow timescale)
        constexpr double     kApplyWaitSec      = 10.0;         // a dispatched temper that has not read back after this did not take

        using Clock = std::chrono::steady_clock;

        double NowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

        struct Comp { RE::FormID form = 0; std::int32_t count = 0; };
        struct Line {
            RE::FormID    item = 0;                 // the worn base object
            std::string   name;
            bool          weapon = false;
            int           hand = 1;                 // weapons: 0 = left (ExtraWornLeft), 1 = right (ExtraWorn)
            std::uint32_t slot = 0;                 // armor: the lowest biped slot bit (SKSE matches the worn piece by it)
            float         current = 1.0f, target = 1.0f;
            std::vector<Comp> comps;                // the improve recipe's materials
        };
        using Have = std::unordered_map<RE::FormID, std::int32_t>;

        struct Want {
            bool                     built = false, pending = false;
            Clock::time_point        at{}, pendingSince{};
            std::string              note;          // why the list is empty (a refusal, no recipes, ...)
            float                    skill = 0.0f;
            std::vector<Line>        lines;         // improvable worn pieces, biggest gain first
            Have                     have;          // what he carried of every component at the build
            std::vector<TradeBridge::SmithMat> buy; // the shortfall over ALL lines
        };
        struct Work {
            enum class Ph { Build, Working, Applying } ph = Ph::Build;
            double                   beganClk = 0.0, workClk = 0.0, applyClk = 0.0, lastVerifyClk = 0.0, offerClk = 0.0;
            float                    startDays = 0.0f;
            std::string              who, place, stationName;
            std::vector<Line>        lines;         // chosen at the end of the build
            std::vector<char>        st;            // per line: 0 not issued, 1 issued, 2 took, 3 failed
            bool                     animated = false, entered = false, entryLogged = false, applyPosted = false;
            RE::FormID               station = 0;
            int                      took = 0, failed = 0;
        };
        struct Plan {                               // what Augment planned: the off-screen stage applies it (no inventory reads off screen)
            std::vector<Line>        lines;
            Have                     have;
            std::vector<TradeBridge::SmithMat> buy;
        };

        std::mutex                                   g_mx;
        std::unordered_map<RE::FormID, Want>         g_want;
        std::unordered_map<RE::FormID, Work>         g_work;
        std::unordered_map<RE::FormID, Plan>         g_plan;

        // MAIN-thread only: the temper recipe index and the once-per-recipe condition log.
        std::unordered_map<RE::FormID, std::vector<const RE::BGSConstructibleObject*>> g_recipes;   // created object -> recipes
        bool                                         g_recipesBuilt = false;
        std::unordered_set<RE::FormID>               g_condSaid;

        // ── Harbinger work offer (STUB until R7's apmf/ErrandWork.cpp merges) ───────────────────────────────────────
        // R7 HOOK-UP: replace the bodies with APMFBridge::OfferErrandWork(a_fid, APMFBridge::ErrandWorkKind::Activate, a_station, 150.0f, a_why),
        // APMFBridge::ReleaseErrandWork(a_fid) and APMFBridge::ErrandWorkHeld(a_fid). MFO declares the work and its target;
        // Harbinger's ch.9 hook hands the Activate package to the follower. MFO never animates or moves him.
        bool OfferWork(RE::FormID, RE::TESObjectREFR*, std::string& a_why) {
            a_why = "the ch.9 work-package API (apmf/ErrandWork.cpp, R7) is not merged in this build";
            return false;
        }
        void ReleaseWork(RE::FormID) {}

        // ── small helpers ──────────────────────────────────────────────────────────────────────────────────────────
        const char* NameOf(const RE::TESForm* a_form) {
            auto* n = a_form ? const_cast<RE::TESForm*>(a_form)->As<RE::TESFullName>() : nullptr;
            const char* s = n ? n->GetFullName() : nullptr;
            return s && *s ? s : "?";
        }

        bool ReadGmst(const char* a_name, float& a_out) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* st = gsc ? gsc->GetSetting(a_name) : nullptr;
            if (!st) return false;
            a_out = st->GetFloat();
            return true;
        }

        std::uint32_t LowestBit(std::uint32_t a_mask) { return a_mask & (~a_mask + 1u); }

        Have CountsOf(RE::Actor* a_f) {
            Have have;
            if (!a_f) return have;
            for (auto& [obj, data] : a_f->GetInventory())
                if (obj && data.first > 0) have[obj->GetFormID()] += data.first;
            return have;
        }

        // Which lines can he pay from a_have, biggest gain first? a_have is consumed by the lines that fit.
        std::vector<std::size_t> Affordable(const std::vector<Line>& a_lines, Have a_have) {
            std::vector<std::size_t> ok;
            for (std::size_t i = 0; i < a_lines.size(); ++i) {
                bool fits = true;
                for (const auto& c : a_lines[i].comps)
                    if (a_have[c.form] < c.count) { fits = false; break; }
                if (!fits) continue;
                for (const auto& c : a_lines[i].comps) a_have[c.form] -= c.count;
                ok.push_back(i);
            }
            return ok;
        }

        // The shortfall of ALL lines' components over what he carries (what the shops are asked for).
        std::vector<TradeBridge::SmithMat> Shortfall(const std::vector<Line>& a_lines, const Have& a_have) {
            std::unordered_map<RE::FormID, std::int32_t> need;
            std::vector<RE::FormID> order;
            for (const auto& l : a_lines)
                for (const auto& c : l.comps) {
                    if (!need.contains(c.form)) order.push_back(c.form);
                    need[c.form] += c.count;
                }
            std::vector<TradeBridge::SmithMat> out;
            for (const auto f : order) {
                const auto it = a_have.find(f);
                const std::int32_t owned = it != a_have.end() ? it->second : 0;
                if (need[f] > owned) out.push_back(TradeBridge::SmithMat{ f, need[f] - owned });
            }
            return out;
        }

        // ── the temper recipe index (MAIN thread) ──────────────────────────────────────────────────────────────────
        void EnsureRecipes() {
            if (g_recipesBuilt) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            g_recipesBuilt = true;
            int weap = 0, armo = 0;
            for (auto* co : dh->GetFormArray<RE::BGSConstructibleObject>()) {
                if (!co || !co->createdItem || !co->benchKeyword) continue;
                const auto kw = co->benchKeyword->GetFormID();
                if (kw != kKwSharpeningWheel && kw != kKwArmorTable) continue;
                g_recipes[co->createdItem->GetFormID()].push_back(co);
                (kw == kKwSharpeningWheel ? weap : armo)++;
            }
            spdlog::info("[smith] temper recipe index: {} weapon recipe(s) (bench keyword {:08X}), {} armor recipe(s) ({:08X}), "
                         "{} created item(s)", weap, kKwSharpeningWheel, armo, kKwArmorTable, g_recipes.size());
        }

        // The recipes the engine's improve list would offer for this item: keyed by the item, else by its template.
        std::vector<const RE::BGSConstructibleObject*> FindRecipes(RE::TESBoundObject* a_obj, bool a_weapon) {
            std::vector<const RE::BGSConstructibleObject*> out;
            const RE::FormID wantKw = a_weapon ? kKwSharpeningWheel : kKwArmorTable;
            RE::FormID ids[2] = { a_obj->GetFormID(), 0 };
            if (auto* w = a_obj->As<RE::TESObjectWEAP>(); w && w->templateWeapon) ids[1] = w->templateWeapon->GetFormID();
            if (auto* a = a_obj->As<RE::TESObjectARMO>(); a && a->templateArmor) ids[1] = a->templateArmor->GetFormID();
            for (const auto id : ids) {
                if (!id) continue;
                const auto it = g_recipes.find(id);
                if (it == g_recipes.end()) continue;
                for (const auto* co : it->second)
                    if (co->benchKeyword && co->benchKeyword->GetFormID() == wantKw) out.push_back(co);
                if (!out.empty()) break;   // the engine tries the item first and the template only on a miss
            }
            return out;
        }

        enum class Cond { Pass, Fail, Unknown };

        bool CompareNode(const RE::TESConditionItem* a_n, float a_lhs) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            float rhs = 0.0f;
            if (a_n->data.flags.global) rhs = a_n->data.comparisonValue.g ? a_n->data.comparisonValue.g->value : 0.0f;
            else                        rhs = a_n->data.comparisonValue.f;
            switch (a_n->data.flags.opCode) {
            case Op::kEqualTo:              return a_lhs == rhs;
            case Op::kNotEqualTo:           return a_lhs != rhs;
            case Op::kGreaterThan:          return a_lhs > rhs;
            case Op::kGreaterThanOrEqualTo: return a_lhs >= rhs;
            case Op::kLessThan:             return a_lhs < rhs;
            case Op::kLessThanOrEqualTo:    return a_lhs <= rhs;
            }
            return false;
        }

        // MAIN thread. See the header comment: a recipe with an item-context node is evaluated by hand, group by group.
        Cond EvalConditions(const RE::BGSConstructibleObject* a_co, RE::Actor* a_f, bool a_enchanted, std::string& a_note) {
            using FID = RE::FUNCTION_DATA::FunctionID;
            const RE::TESConditionItem* head = a_co->conditions.head;
            if (!head) return Cond::Pass;
            bool itemCtx = false;
            for (auto* n = head; n; n = n->next) {
                const auto fn = n->data.functionData.function.get();
                if (fn == FID::kEPTemperingItemIsEnchanted || fn == FID::kEPTemperingItemHasKeyword) itemCtx = true;
            }
            if (!itemCtx) {
                const auto r = TradePerks::ConditionTrue(a_co->conditions, a_f, a_f);
                if (!r) { a_note = "the condition evaluator seat was refused"; return Cond::Unknown; }
                return *r ? Cond::Pass : Cond::Fail;
            }
            // OR-groups: a node's isOR flag joins it to the NEXT node; a group ends at a node without it; groups are ANDed.
            bool anyGroupFail = false, anyGroupUnknown = false;
            bool groupTrue = false, groupUnknown = false;
            for (auto* n = head; n; n = n->next) {
                const auto fn = n->data.functionData.function.get();
                if (fn == FID::kEPTemperingItemIsEnchanted) { if (CompareNode(n, a_enchanted ? 1.0f : 0.0f)) groupTrue = true; }
                else groupUnknown = true;   // another item-context node, or a node the engine alone can evaluate
                if (!n->data.flags.isOR) {   // the group ends here
                    if (!groupTrue) { (groupUnknown ? anyGroupUnknown : anyGroupFail) = true; }
                    groupTrue = false; groupUnknown = false;
                }
            }
            if (groupTrue || groupUnknown) {   // a list whose last node still carries the OR flag
                if (!groupTrue) { (groupUnknown ? anyGroupUnknown : anyGroupFail) = true; }
            }
            if (anyGroupFail) return Cond::Fail;
            if (anyGroupUnknown) {
                a_note = "an OR-group needs a node the engine alone can evaluate in its tempering context";
                return Cond::Unknown;
            }
            return Cond::Pass;
        }

        // ── the worn-piece read (an inventory read; the follower is loaded) ───────────────────────────────────────
        struct Worn {
            RE::TESBoundObject* obj = nullptr;
            bool                weapon = false, left = false, enchanted = false;
            std::uint32_t       slot = 0;
            float               health = 1.0f;
        };

        std::vector<Worn> ReadWorn(RE::Actor* a_f) {
            std::vector<Worn> out;
            for (auto& [obj, data] : a_f->GetInventory()) {
                if (!obj || data.first <= 0 || !data.second || !data.second->extraLists) continue;
                auto* weap = obj->As<RE::TESObjectWEAP>();
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!weap && !armo) continue;
                if (!data.second->IsWorn()) continue;
                for (auto* xl : *data.second->extraLists) {
                    if (!xl) continue;
                    const bool worn = xl->HasType(RE::ExtraDataType::kWorn), wornL = xl->HasType(RE::ExtraDataType::kWornLeft);
                    if (!worn && !wornL) continue;
                    Worn w;
                    w.obj = obj;
                    w.weapon = weap != nullptr;
                    w.left = wornL && !worn;
                    w.slot = armo ? LowestBit(static_cast<std::uint32_t>(armo->GetSlotMask())) : 0u;
                    const auto* eh = xl->GetByType<RE::ExtraHealth>();
                    w.health = eh ? eh->health : 1.0f;
                    w.enchanted = xl->HasType(RE::ExtraDataType::kEnchantment) || (weap ? weap->formEnchanting != nullptr : armo->formEnchanting != nullptr);
                    out.push_back(w);
                }
            }
            return out;
        }

        // The worn copy's ExtraHealth for a line (default 1.0). False = the piece is no longer worn.
        bool WornHealth(RE::Actor* a_f, const Line& a_l, float& a_out) {
            for (const auto& w : ReadWorn(a_f))
                if (w.obj->GetFormID() == a_l.item && w.weapon == a_l.weapon && (!a_l.weapon || (w.left ? 0 : 1) == a_l.hand)) {
                    a_out = w.health;
                    return true;
                }
            return false;
        }

        std::string LineText(const Line& a_l) {
            std::string mats;
            for (const auto& c : a_l.comps)
                mats += std::format("{}{} x{}", mats.empty() ? "" : ", ", NameOf(RE::TESForm::LookupByID(c.form)), c.count);
            return std::format("'{}' {:08X} {} {:.2f} -> {:.1f} [{}]", a_l.name, a_l.item,
                               a_l.weapon ? (a_l.hand == 0 ? "left hand" : "right hand") : std::format("slot {:#x}", a_l.slot),
                               a_l.current, a_l.target, mats);
        }

        // ── the want-list BUILD (MAIN thread) ─────────────────────────────────────────────────────────────────────
        void StoreWant(RE::FormID a_fid, Want&& a_w) {
            std::scoped_lock lk(g_mx);
            a_w.built = true;
            a_w.pending = false;
            a_w.at = Clock::now();
            g_want[a_fid] = std::move(a_w);
        }

        void BuildOnMain(RE::ActorHandle a_h, RE::FormID a_fid) {
            auto fp = a_h.get();
            auto* f = fp.get();
            Want w;
            // the handle may now resolve to another actor (a reused slot): never file under the captured id then
            if (!f || f->GetFormID() != a_fid) { std::scoped_lock lk(g_mx); g_want[a_fid].pending = false; return; }
            if (!Config::g_errandSmith.load()) { w.note = "bErrandSmith = 0"; StoreWant(a_fid, std::move(w)); return; }
            if (!f->Is3DLoaded()) { w.note = "not loaded (the entry point needs a process)"; StoreWant(a_fid, std::move(w)); return; }
            EnsureRecipes();
            auto* avo = f->AsActorValueOwner();
            float hdv1 = 0, hdv6 = 0, factor = 0;
            if (!avo || !ReadGmst("fHealthDataValue1", hdv1) || !ReadGmst("fHealthDataValue6", hdv6) ||
                !ReadGmst("fSmithingConditionFactor", factor)) {
                w.note = "a temper GMST (fHealthDataValue1 / fHealthDataValue6 / fSmithingConditionFactor) is unreadable";
                spdlog::error("[smith] {:08X}: tempering OFF -- {}", a_fid, w.note);
                StoreWant(a_fid, std::move(w));
                return;
            }
            w.skill = avo->GetActorValue(RE::ActorValue::kSmithing);   // UNCLAMPED (R0-3): a fortify counts
            const float raw = std::max(1.0f, ((w.skill - 15.0f) * factor + 1.0f) * 0.01f * (hdv6 - hdv1) + hdv1);
            const float delta0 = raw - hdv1;

            const auto worn = ReadWorn(f);
            w.have = CountsOf(f);
            std::string notes;
            bool refused = false;
            for (const auto& p : worn) {
                const char* nm = NameOf(p.obj);
                // THE ENGINE'S FORMULA, the item as the entry point's argument.
                float target = raw;
                float delta1 = delta0;
                if (delta0 > 0.0f) {
                    const auto ep = TradePerks::EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT::kModTemperingHealth, f, p.obj, delta0);
                    if (!ep) { refused = true; break; }
                    delta1 = *ep;
                    target = std::floor((std::max(delta1, 0.0f) + hdv1) * 10.0f + 0.5f) * 0.1f;   // round half up, in tenths
                }
                if (target <= p.health + kTemperStep) {
                    notes += std::format(" | '{}' stays {:.2f} (target {:.1f})", nm, p.health, target);
                    continue;
                }
                // THE IMPROVE RECIPE the engine would list, and its conditions with HIM as the subject.
                const auto recipes = FindRecipes(p.obj, p.weapon);
                const RE::BGSConstructibleObject* chosen = nullptr;
                std::string why = recipes.empty() ? "no temper recipe" : "";
                for (const auto* co : recipes) {
                    std::string note;
                    const auto c = EvalConditions(co, f, p.enchanted, note);
                    if (c == Cond::Pass) { chosen = co; break; }
                    why = c == Cond::Fail ? "recipe conditions fail for him" : std::format("recipe conditions not evaluated ({})", note);
                    if (c == Cond::Unknown && g_condSaid.insert(co->GetFormID()).second)
                        spdlog::info("[smith] recipe {:08X} for '{}' skipped: {} (an enchanted piece needs the engine's tempering "
                                     "context; backlog)", co->GetFormID(), nm, note);
                }
                if (!chosen) { notes += std::format(" | '{}' skipped ({})", nm, why); continue; }
                Line l;
                l.item = p.obj->GetFormID(); l.name = nm; l.weapon = p.weapon; l.hand = p.left ? 0 : 1; l.slot = p.slot;
                l.current = p.health; l.target = target;
                chosen->requiredItems.ForEachContainerObject([&](RE::ContainerObject& a_co) {
                    if (a_co.obj && a_co.count > 0) l.comps.push_back(Comp{ a_co.obj->GetFormID(), a_co.count });
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                // P-SMITH-T: every number of the formula, for the field check.
                spdlog::info("[smith] {:08X} P-SMITH-T '{}': Smithing {:.1f}, fHealthDataValue1 {:.3f} fHealthDataValue6 {:.3f} "
                             "fSmithingConditionFactor {:.4f} -> raw {:.3f}, delta {:.4f} -> after EP 76 {:.4f} -> target {:.1f}; "
                             "worn copy {:.2f}; recipe {:08X}", a_fid, nm, w.skill, hdv1, hdv6, factor, raw, delta0, delta1, target,
                             p.health, chosen->GetFormID());
                w.lines.push_back(std::move(l));
            }
            if (refused) {
                w.lines.clear();
                w.note = "the tempering entry point was refused (the seat did not verify, or he has no perk data): tempering is OFF "
                         "for him; MFO never substitutes a guessed value";
                spdlog::warn("[smith] {:08X} '{}': {}", a_fid, f->GetName(), w.note);
            }
            std::stable_sort(w.lines.begin(), w.lines.end(), [](const Line& a, const Line& b) {
                const float ga = a.target - a.current, gb = b.target - b.current;
                return ga != gb ? ga > gb : a.item < b.item;
            });
            w.buy = Shortfall(w.lines, w.have);
            std::string mats;
            for (const auto& m : w.buy) mats += std::format("{}{} x{}", mats.empty() ? "" : ", ", NameOf(RE::TESForm::LookupByID(m.form)), m.qty);
            spdlog::info("[smith] {:08X} '{}': want list built -- Smithing {:.0f}, {} worn piece(s) read, {} improvable{}{}; to buy: {}",
                         a_fid, f->GetName(), w.skill, worn.size(), w.lines.size(), refused ? " (REFUSED)" : "", notes,
                         mats.empty() ? "nothing" : mats);
            for (const auto& l : w.lines) spdlog::info("[smith] {:08X}   want {}", a_fid, LineText(l));
            StoreWant(a_fid, std::move(w));
        }

        void QueueBuild(RE::Actor* a_f) {
            const auto fid = a_f->GetFormID();
            {
                std::scoped_lock lk(g_mx);
                auto& w = g_want[fid];
                if (w.pending && Clock::now() - w.pendingSince < kPendingStale) return;
                w.pending = true;
                w.pendingSince = Clock::now();
            }
            MainThread::Post([h = a_f->GetHandle(), fid] { BuildOnMain(h, fid); });
        }

        // ── the station ────────────────────────────────────────────────────────────────────────────────────────────
        // A sharpening wheel (weapons) or an armor table (armor) in the follower's loaded cell, nearest first. Data reads only.
        std::vector<RE::ObjectRefHandle> FindStations(RE::Actor* a_f, RE::FormID a_keyword) {
            std::vector<RE::ObjectRefHandle> out;
            auto* cell = a_f ? a_f->GetParentCell() : nullptr;
            if (!cell || !cell->IsAttached()) return out;
            std::vector<std::pair<float, RE::ObjectRefHandle>> found;
            const auto at = a_f->GetPosition();
            cell->ForEachReferenceInRange(at, kStationRadius, [&](RE::TESObjectREFR* a_ref) {
                auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
                auto* fur = base ? base->As<RE::TESFurniture>() : nullptr;
                if (fur && fur->HasKeywordID(a_keyword) && !a_ref->IsDisabled() && !a_ref->IsMarkedForDeletion() && found.size() < 16)
                    found.emplace_back(a_ref->GetPosition().GetDistance(at), a_ref->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& p : found) out.push_back(p.second);
            return out;
        }

        // ── the temper APPLY (MAIN thread) ─────────────────────────────────────────────────────────────────────────
        void ApplyOnMain(RE::ActorHandle a_h, RE::FormID a_fid) {
            std::vector<Line> lines;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_work.find(a_fid);
                if (it == g_work.end()) return;
                lines = it->second.lines;
            }
            auto fp = a_h.get();
            auto* f = fp.get();
            std::vector<char> st(lines.size(), 0);
            if (!f || f->GetFormID() != a_fid || !f->Is3DLoaded()) {
                std::fill(st.begin(), st.end(), static_cast<char>(3));
                spdlog::warn("[smith] {:08X}: the temper was NOT applied -- he is no longer loaded; no material consumed", a_fid);
            } else {
                Have have = CountsOf(f);
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    const Line& l = lines[i];
                    float cur = 1.0f;
                    if (!WornHealth(f, l, cur)) {
                        st[i] = 3;
                        spdlog::warn("[smith] {:08X} '{}': NOT applied -- that piece is no longer worn", a_fid, l.name);
                        continue;
                    }
                    if (cur >= l.target - kReadbackTol) { st[i] = 2; continue; }   // already there (another round did it)
                    bool fits = true;
                    for (const auto& c : l.comps) if (have[c.form] < c.count) { fits = false; break; }
                    if (!fits) {   // R8 hot spot 6: re-clamped against what he carries NOW, nothing removed for a line that cannot be paid
                        st[i] = 3;
                        spdlog::warn("[smith] {:08X} '{}': NOT applied -- the materials are no longer in his inventory", a_fid, l.name);
                        continue;
                    }
                    for (const auto& c : l.comps) have[c.form] -= c.count;   // reserved for this line
                    const bool ok = Papyrus::DispatchWornHealth(f, l.weapon ? l.hand : 0, l.weapon ? 0 : static_cast<std::int32_t>(l.slot), l.target);
                    spdlog::info("[smith] {:08X} P-SMITH-T apply {} -- WornObject.SetItemHealthPercent {} (read back follows)",
                                 a_fid, LineText(l), ok ? "DISPATCHED" : "FAILED to dispatch");
                    st[i] = ok ? 1 : 3;
                }
            }
            std::scoped_lock lk(g_mx);
            if (const auto it = g_work.find(a_fid); it != g_work.end()) {
                it->second.st = std::move(st);
                it->second.applyClk = NowSec();
                it->second.applyPosted = true;
            }
        }

        void VerifyOnMain(RE::ActorHandle a_h, RE::FormID a_fid) {
            std::vector<Line> lines;
            std::vector<char> st;
            double applyClk = 0.0;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_work.find(a_fid);
                if (it == g_work.end() || !it->second.applyPosted) return;
                lines = it->second.lines; st = it->second.st; applyClk = it->second.applyClk;
            }
            auto fp = a_h.get();
            auto* f = fp.get();
            bool changed = false;
            for (std::size_t i = 0; i < lines.size() && i < st.size(); ++i) {
                if (st[i] != 1) continue;
                const Line& l = lines[i];
                float got = 1.0f;
                const bool have = f && f->GetFormID() == a_fid && WornHealth(f, l, got);
                if (have && got >= l.target - kReadbackTol) {
                    // IT TOOK: now, and only now, the materials go (clamped to what he carries).
                    const Have counts = CountsOf(f);
                    for (const auto& c : l.comps) {
                        const auto it = counts.find(c.form);
                        const std::int32_t n = std::min(c.count, it != counts.end() ? it->second : 0);
                        if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(c.form); obj && n > 0)
                            f->RemoveItem(obj, n, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        if (n < c.count)
                            spdlog::warn("[smith] {:08X} '{}': only {} of {} {} could be consumed (the temper already took)", a_fid, l.name,
                                         n, c.count, NameOf(RE::TESForm::LookupByID(c.form)));
                    }
                    spdlog::info("[smith] {:08X} P-SMITH-T read back '{}': {:.2f} (target {:.1f}) -- TOOK; materials consumed",
                                 a_fid, l.name, got, l.target);
                    st[i] = 2; changed = true;
                } else if (NowSec() - applyClk > kApplyWaitSec) {
                    spdlog::warn("[smith] {:08X} P-SMITH-T read back '{}': {} -- the temper did NOT take after {:.0f} s ({}); no material "
                                 "consumed", a_fid, l.name, have ? std::format("{:.2f}", got) : std::string("(not worn)"), kApplyWaitSec,
                                 l.weapon ? "weapons need a process and the right hand index (P-SMITH-W)" : "armor matches by biped slot");
                    st[i] = 3; changed = true;
                }
            }
            if (!changed) return;
            std::scoped_lock lk(g_mx);
            if (const auto it = g_work.find(a_fid); it != g_work.end()) it->second.st = std::move(st);
        }
    }   // anonymous namespace

    // ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
    // public (Errand_internal.h / Errand.h)
    // ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
    Prep Prepare(RE::Actor* a_follower) {
        if (!kTripHooked || !a_follower || !Config::g_errandSmith.load() || !MainThread::IsInstalled()) return Prep::Off;
        const auto fid = a_follower->GetFormID();
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_want.find(fid);
            if (it != g_want.end() && it->second.built && Clock::now() - it->second.at < kFresh) return Prep::Ready;
        }
        QueueBuild(a_follower);
        return Prep::Pending;
    }

    void FillMaterials(RE::FormID a_follower, std::vector<TradeBridge::SmithMat>& a_out) {
        a_out.clear();
        if (!kTripHooked || !Config::g_errandSmith.load()) return;
        std::scoped_lock lk(g_mx);
        const auto it = g_want.find(a_follower);
        if (it != g_want.end() && it->second.built) a_out = it->second.buy;
    }

    bool Augment(RE::Actor* a_follower, const std::vector<Trip::Cand>& a_cands, std::vector<Stop>& a_stops, std::string& a_line) {
        if (!a_follower || !Config::g_errandSmith.load()) return false;
        const auto fid = a_follower->GetFormID();
        Want w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_want.find(fid);
            if (it == g_want.end() || !it->second.built) return false;
            w = it->second;
        }
        const char* who = a_follower->GetName();
        if (w.lines.empty()) return false;   // (the build said why: the note and the want-list line)
        if (a_stops.size() + 3 > kMaxTripStops) {   // room for the temple and the inn after it (R7)
            spdlog::info("[smith] {:08X} '{}': no smith stop -- the trip already has {} stop(s)", fid, who, a_stops.size());
            return false;
        }
        const auto affordable = Affordable(w.lines, w.have);
        // A planned shop stop that buys materials (BuyThresholds::smithMats rode its order) makes the lines payable by the time he
        // reaches the forge.
        const Stop* matStop = nullptr;
        for (const auto& s : a_stops)
            if (s.kind == StopKind::Shop && s.order && s.order->wishMaterials) { matStop = &s; break; }
        const bool buysMaterials = matStop != nullptr;   // (matStop points into a_stops, which grows below)
        if (affordable.empty() && !buysMaterials) {
            spdlog::info("[smith] {:08X} '{}': no smith stop -- {} improvable piece(s) but the materials are neither carried nor on "
                         "any planned shop stop's list", fid, who, w.lines.size());
            return false;
        }
        // THE SMITHY: a candidate whose VEND trades ore / ingots or weapons. The shop that buys his materials first, else the nearest.
        const Trip::Cand* pick = nullptr;
        for (const auto& c : a_cands) {
            if (!c.carriesForge) continue;
            const bool better = !pick ||
                (matStop && c.faction == matStop->faction && pick->faction != matStop->faction) ||
                (!(matStop && pick->faction == matStop->faction) && c.dist < pick->dist);
            if (better) pick = &c;
        }
        if (!pick) {
            spdlog::info("[smith] {:08X} '{}': no smith stop -- no open shop in this town trades ore / ingots or weapons (no smithy)", fid, who);
            return false;
        }
        Stop s;
        s.kind = StopKind::Smith;
        s.faction = pick->faction; s.chest = pick->chest; s.vendorActor = pick->actorId; s.vendorName = pick->name;
        s.destKind = pick->kind; s.destForm = pick->destForm; s.destRad = pick->destRad;
        s.pos = pick->pos; s.havePos = pick->havePos;
        s.state = StopState::Pending;
        a_stops.push_back(std::move(s));
        {
            std::scoped_lock lk(g_mx);
            g_plan[fid] = Plan{ w.lines, w.have, w.buy };
        }
        a_line += std::format(" | smith stop {} at '{}': {} improvable piece(s), {} payable from carried materials, buys {}", a_stops.size(),
                              pick->name, w.lines.size(), affordable.size(), buysMaterials ? "on the shop stops" : "nothing");
        return true;
    }

    bool Begin(RE::Actor* a_follower, const Stop& a_stop, const std::string& a_followerName, Clock::time_point) {
        if (!a_follower || !Config::g_errandSmith.load() || !MainThread::IsInstalled()) return false;
        const auto fid = a_follower->GetFormID();
        Work wk;
        wk.beganClk = NowSec();
        wk.who = a_followerName;
        wk.place = a_stop.vendorName.empty() ? "the forge" : a_stop.vendorName;
        {
            std::scoped_lock lk(g_mx);
            g_want[fid].built = false;   // a FRESH build at the forge: the materials he bought on this trip are in his inventory now
            g_work[fid] = std::move(wk);
        }
        QueueBuild(a_follower);
        spdlog::info("[smith] {:08X} '{}': at the smithy '{}' -- reading what he can temper now", fid, a_followerName,
                     a_stop.vendorName.empty() ? "?" : a_stop.vendorName);
        return true;
    }

    StepOut Step(RE::Actor* a_follower, Clock::time_point) {
        StepOut out;
        if (!a_follower) { out.res = Result::Failed; out.why = "no follower"; return out; }
        const auto fid = a_follower->GetFormID();
        const double now = NowSec();
        Work wk;
        Want want;
        bool haveWant = false;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_work.find(fid);
            if (it == g_work.end()) { out.res = Result::Failed; out.why = "no smith work"; return out; }
            wk = it->second;
            if (const auto iw = g_want.find(fid); iw != g_want.end() && iw->second.built && !iw->second.pending) { want = iw->second; haveWant = true; }
        }
        auto store = [&] { std::scoped_lock lk(g_mx); if (const auto it = g_work.find(fid); it != g_work.end()) it->second = wk; };

        switch (wk.ph) {
        case Work::Ph::Build: {
            if (!haveWant) {
                if (now - wk.beganClk > kBuildWaitSec) { out.res = Result::Failed; out.why = "the want list was never built (main thread did not answer)"; }
                return out;
            }
            // WHAT HE CAN DO NOW: the lines whose materials he carries (biggest gain first), at a station of the right kind.
            const auto pay = Affordable(want.lines, want.have);
            if (want.lines.empty() || pay.empty()) {
                out.res = Result::Done;
                out.why = want.lines.empty() ? (want.note.empty() ? "nothing to improve" : "nothing to improve (" + want.note + ")")
                                             : "no improvable piece has its materials in hand";
                return out;
            }
            bool needWheel = false, needTable = false;
            for (const auto i : pay) (want.lines[i].weapon ? needWheel : needTable) = true;
            const auto wheels = needWheel ? FindStations(a_follower, kKwSharpeningWheel) : std::vector<RE::ObjectRefHandle>{};
            const auto tables = needTable ? FindStations(a_follower, kKwArmorTable) : std::vector<RE::ObjectRefHandle>{};
            for (const auto i : pay) {
                const Line& l = want.lines[i];
                if (l.weapon ? wheels.empty() : tables.empty()) {
                    spdlog::info("[smith] {:08X} '{}': '{}' skipped -- no {} within {:.0f} u of him here", fid, wk.who, l.name,
                                 l.weapon ? "sharpening wheel" : "armor table", kStationRadius);
                    continue;
                }
                wk.lines.push_back(l);
            }
            if (wk.lines.empty()) { out.res = Result::Done; out.why = "no sharpening wheel / armor table near him"; return out; }
            // THE ANIMATION: Harbinger's ch.9 Activate work package on the first station (wheel if there are weapon lines).
            const auto& stations = !wheels.empty() && needWheel ? wheels : tables;
            std::string offerWhy = "bErrandSmithFurniture = 0";
            if (Config::g_errandSmithFurniture.load()) {
                if (auto sp = stations.front().get(); sp) {
                    wk.station = sp->GetFormID();
                    wk.stationName = NameOf(sp->GetBaseObject());
                    wk.animated = OfferWork(fid, sp.get(), offerWhy);
                    wk.offerClk = now;
                } else offerWhy = "the station reference vanished";
            }
            if (!wk.animated)
                spdlog::warn("[smith] {:08X} '{}': working UNANIMATED -- {} (P-SMITH-1: the station animation did not run)", fid, wk.who, offerWhy);
            detail::ToastSmithing(wk.who, wk.place);
            if (auto* cal = RE::Calendar::GetSingleton()) wk.startDays = cal->GetDaysPassed();
            wk.workClk = now;
            wk.ph = Work::Ph::Working;
            spdlog::info("[smith] {:08X} '{}': working at '{}' ({}{}) -- {} piece(s), {} game min", fid, wk.who, wk.place,
                         wk.stationName.empty() ? "no station" : wk.stationName, wk.animated ? ", animated" : "", wk.lines.size(),
                         Config::g_smithWorkGameMin.load());
            store();
            return out;
        }
        case Work::Ph::Working: {
            if (wk.animated && !wk.entryLogged && now - wk.offerClk >= 1.0) {
                const auto s = a_follower->AsActorState()->GetSitSleepState();
                if (s != RE::SIT_SLEEP_STATE::kNormal) {
                    wk.entered = true; wk.entryLogged = true;
                    spdlog::info("[smith] {:08X} '{}': P-SMITH-1 -- ENTERED the station (sit/sleep state {})", fid, wk.who, static_cast<int>(s));
                } else if (now - wk.offerClk > kEntrySec) {
                    wk.entryLogged = true;
                    spdlog::warn("[smith] {:08X} '{}': P-SMITH-1 -- NOT entered after {:.0f} s: working unanimated", fid, wk.who, kEntrySec);
                }
            }
            bool over = now - wk.workClk >= kRealCapSec;
            if (!over)
                if (auto* cal = RE::Calendar::GetSingleton())
                    over = (cal->GetDaysPassed() - wk.startDays) * 24.0f * 60.0f >= static_cast<float>(Config::g_smithWorkGameMin.load());
            if (!over) { store(); return out; }
            ReleaseWork(fid);
            wk.ph = Work::Ph::Applying;
            wk.applyPosted = false;
            store();
            MainThread::Post([h = a_follower->GetHandle(), fid] { ApplyOnMain(h, fid); });
            return out;
        }
        case Work::Ph::Applying: {
            std::vector<char> st;
            bool applyRan = false, verifyDue = false;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_work.find(fid);
                if (it == g_work.end()) { out.res = Result::Failed; out.why = "work vanished"; return out; }
                st = it->second.st;
                applyRan = it->second.applyPosted;
                if (applyRan && now - it->second.lastVerifyClk >= 1.0) { it->second.lastVerifyClk = now; verifyDue = true; }
            }
            if (verifyDue) MainThread::Post([h = a_follower->GetHandle(), fid] { VerifyOnMain(h, fid); });
            if (!applyRan || st.empty()) {
                if (now - wk.workClk > kRealCapSec + kApplyWaitSec * 2) { out.res = Result::Failed; out.why = "the apply never ran on the main thread"; }
                return out;
            }
            int took = 0, failed = 0, open = 0;
            for (const char c : st) (c == 2 ? took : c == 3 ? failed : open)++;
            if (open > 0) return out;
            out.res = Result::Done;
            out.why = std::format("{} piece(s) tempered, {} not applied", took, failed);
            return out;
        }
        }
        return out;
    }

    void End(RE::FormID a_follower, const char* a_why) {
        bool had = false;
        {
            std::scoped_lock lk(g_mx);
            had = g_work.erase(a_follower) > 0;
        }
        if (had) {
            ReleaseWork(a_follower);
            spdlog::info("[smith] {:08X}: work ended ({})", a_follower, a_why ? a_why : "?");
        }
    }

    bool Working(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        return g_work.contains(a_follower);
    }

    std::string Describe(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_work.find(a_follower);
        if (it == g_work.end()) return {};
        return std::format("Working at {}", it->second.place);
    }

    double SimWorkDays() { return static_cast<double>(Config::g_smithWorkGameMin.load()) / (24.0 * 60.0); }

    std::string SimFinish(RE::FormID a_follower, const std::string& a_followerName, bool a_materialsBought) {
        Plan plan;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_plan.find(a_follower);
            if (it == g_plan.end()) {
                const std::string s = "no smith plan (it is session-only: lost with a load)";
                spdlog::info("[smith] {:08X} '{}': OFF-SCREEN smithing -- {}", a_follower, a_followerName, s);
                return s;
            }
            plan = std::move(it->second);
            g_plan.erase(it);
        }
        auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
        if (!f) {
            const std::string s = "the follower is gone";
            spdlog::info("[smith] {:08X} '{}': OFF-SCREEN smithing -- {}", a_follower, a_followerName, s);
            return s;
        }
        // No inventory is read off screen (house rule): the plan's snapshot is the truth, plus the materials the earlier shop
        // stops were asked for when their trade reported.
        Have have = plan.have;
        if (a_materialsBought) for (const auto& m : plan.buy) have[m.form] += m.qty;
        int applied = 0, deferred = 0, skipped = 0;
        for (const auto& l : plan.lines) {
            if (l.weapon) {   // R0-5: a weapon is matched through the actor's process, which an unloaded follower may not have (P-SMITH-W)
                ++deferred;
                spdlog::info("[smith] {:08X} '{}': off screen -- {} DEFERRED (a weapon temper needs a process; the next visit does it)",
                             a_follower, a_followerName, LineText(l));
                continue;
            }
            bool fits = true;
            for (const auto& c : l.comps) if (have[c.form] < c.count) { fits = false; break; }
            if (!fits) {
                ++skipped;
                spdlog::info("[smith] {:08X} '{}': off screen -- {} SKIPPED (materials not in hand{})", a_follower, a_followerName, LineText(l),
                             a_materialsBought ? "" : "; the shop stops had not reported");
                continue;
            }
            for (const auto& c : l.comps) have[c.form] -= c.count;
            const bool ok = Papyrus::DispatchWornHealth(f, 0, static_cast<std::int32_t>(l.slot), l.target);
            for (const auto& c : l.comps)
                if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(c.form)) Papyrus::DispatchRemoveItem(f, obj, c.count);
            ++applied;
            spdlog::info("[smith] {:08X} '{}': off screen -- {} temper {} (armor is pure inventory data, R0-5)", a_follower, a_followerName,
                         LineText(l), ok ? "DISPATCHED" : "FAILED to dispatch");
        }
        const std::string s = std::format("{} armor piece(s) tempered, {} weapon(s) deferred, {} skipped", applied, deferred, skipped);
        spdlog::info("[smith] {:08X} '{}': OFF-SCREEN smithing done -- {}", a_follower, a_followerName, s);
        return s;
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_want.clear();
        g_work.clear();
        g_plan.clear();
    }

    // ── the loot side (Errand.h) ───────────────────────────────────────────────────────────────────────────────────
    bool WantsMaterials(RE::Actor* a_follower) {
        if (!kTripHooked || !a_follower || !Config::g_errandSmith.load() || !Config::g_townErrands.load() || !MainThread::IsInstalled()) return false;
        const auto fid = a_follower->GetFormID();
        bool want = false, stale = true;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_want.find(fid);
            if (it != g_want.end() && it->second.built) {
                want = !it->second.buy.empty();
                stale = Clock::now() - it->second.at >= kLootRefresh;
            }
        }
        if (stale && a_follower->Is3DLoaded()) QueueBuild(a_follower);
        return want;
    }

    std::int32_t Wanted(RE::FormID a_follower, RE::FormID a_form) {
        if (!kTripHooked) return 0;
        std::scoped_lock lk(g_mx);
        const auto it = g_want.find(a_follower);
        if (it == g_want.end() || !it->second.built) return 0;
        for (const auto& m : it->second.buy)
            if (m.form == a_form) return std::max<std::int32_t>(m.qty, 0);
        return 0;
    }

    void NoteTaken(RE::FormID a_follower, RE::FormID a_form, std::int32_t a_count) {
        std::scoped_lock lk(g_mx);
        const auto it = g_want.find(a_follower);
        if (it == g_want.end()) return;
        auto& buy = it->second.buy;
        for (auto& m : buy) if (m.form == a_form) m.qty -= a_count;
        std::erase_if(buy, [](const TradeBridge::SmithMat& m) { return m.qty <= 0; });
        it->second.have[a_form] += a_count;
    }
}
