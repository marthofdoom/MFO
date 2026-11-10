// progression/Inherit.cpp -- HIDDEN PLAYER-ONLY PERK CENSUS (ClickUp 86e3eevgz, card C1).
//
// DISCOVERY + CLASSIFICATION + A LOG, NOTHING ELSE. This file changes NOTHING in the game: it adds no
// perk to any actor, writes no co-save field, and holds no state the rest of MFO reads. A later card
// (C2) is the one that copies a perk onto a follower; this one only answers "which perks does the
// player hold that sit in no ActorValueInfo perk tree, and what would we do with each".
//
// THREADING: every engine read here runs on the MAIN THREAD. The only entry, InheritCensusPoll, is
// called from PollWork (progression/Poll.cpp), which PollTick runs on the MainThread pump. The static
// state below is read and written only there. No other thread touches it; nothing here locks.
//
// DISCOVERY. Tree set = every built-in actor value's AVIF plus every AVIF record in the data, each
// perkTree walked breadth-first through BGSSkillPerkTreeNode::children, every node perk followed down
// its BGSPerk::nextPerk chain (the rank forms). Built lazily on the first poll (data is loaded by then;
// the doc's "at kDataLoaded" without touching plugin.cpp). The player's perks come from TWO engine
// roads, and a perk granted any other way is NOT seen (see the block above BuildCandidates):
//   (a) Actor::HasPerk over every BGSPerk form in no tree (the base record + addedPerks), because
//       PLAYER_RUNTIME_DATA is empty in this build's multi-runtime CommonLib config;
//   (b) the active-effect list: a magic effect's "Perk to Apply" lands in standingStonePerks, which
//       HasPerk never reads (standing stones, racial / SPID abilities, Requiem-style rule abilities).
// Standing-stone perks arrive through (b) and are NOT separable from other effect-applied perks.
//
// WHEN. A continuous time-sliced sweep (kSweepPerPoll HasPerk calls per poll). At the end of each full
// pass the held set is hashed: the FIRST pass of a load generation (g_pollGen) logs the full list, later
// passes log only the perks ADDED / REMOVED when the hash changed.
//
// CLASSIFICATION (Docs/hidden-perks research section e, defaults decided 2026-11-09 pending Q43-50):
//   rule 1  every EP entry is player-only by engine code with no follower activity (or never evaluated
//           by the executable), and the perk has no ability / quest entry  -> "no follower use"
//   rule 3  every firing part (each EP entry's owner tab, each ability effect) is pinned by the NARROW
//           Progression::ConditionPlayerGated shape                          -> "player only"
//   rule 4  a script part: an EP entry with function kAddActivateChoice, an ability whose MGEF is
//           archetype kScript, or a quest entry                              -> "script part"
//           (a script ATTACHED to an MGEF through the VM is NOT detected: see the report)
//   else    "copy" (the verdict a later card would act on; THIS card copies nothing)
//   Duplicate (rules 0 and 2) is per FOLLOWER, so it is reported as a count over the active, MFO-managed
//   followers, not folded into the verdict. "Identical" = same entry point + function + function data
//   (value / spell / text) + identical conditions on every tab. Anything less is not a duplicate.
//   EP22 is dropped (no follower activity: the skill rides a PlayerCharacter field).
#include "PCH.h"
#include "ProgAllocator_internal.h"
#include "Config.h"
#include "Followers.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace MFO::ProgAllocator {

    namespace {

        // ── the executable's entry-point classes (Docs/ENGINE_NOTES.md 0.54, tools/entrypoint_census) ──
        // kPlayerEngine : every HandleEntryPoint site passes the player singleton as owner (all 3 runtimes)
        // kPlayerActivity: the engine takes a generic owner but only the player does the activity (EP56)
        // kAny          : evaluated with any actor as owner once the perk is on that actor
        // kNever        : no call site on any runtime
        enum class EpClass : std::uint8_t { kAny, kPlayerEngine, kPlayerActivity, kNever };

        struct EpInfo {
            EpClass     cls{ EpClass::kAny };
            const char* use{ nullptr };   // the follower activity that asks this EP, nullptr = none today
        };

        constexpr std::size_t kEpCount = 92;   // BGSEntryPoint::ENTRY_POINT 0..91

        struct EpTable {
            EpInfo e[kEpCount];
            constexpr EpTable() : e{} {
                // class 1 WITH a follower activity (the activity is the reason to copy)
                e[8]  = { EpClass::kPlayerEngine, "Pricing" };
                e[60] = { EpClass::kPlayerEngine, "Pricing" };
                e[59] = { EpClass::kPlayerEngine, "Lockpick" };
                e[63] = { EpClass::kPlayerEngine, "Lockpick" };
                e[65] = { EpClass::kPlayerEngine, "Lockpick" };
                e[76] = { EpClass::kPlayerEngine, "ErrandSmith" };
                e[75] = { EpClass::kPlayerEngine, "DualCast" };
                e[61] = { EpClass::kPlayerEngine, "Steal(future)" };
                e[9]  = { EpClass::kPlayerEngine, "Loot(future)" };
                // class 1 with NO follower activity (86/90/16: lockpick extras not consumed today; 22: dropped)
                for (int i : { 86, 90, 16, 22, 5, 72, 66, 73, 89, 77, 79, 80, 49, 83, 43, 46, 14, 74, 81, 82,
                               3, 19, 21, 24, 25, 84, 20 })
                    e[i] = { EpClass::kPlayerEngine, nullptr };
                // class 2, player-only by activity
                e[56] = { EpClass::kPlayerActivity, "Steal(future)" };
                // class 3: never evaluated
                for (int i : { 13, 44, 45, 62, 64 })
                    e[i] = { EpClass::kNever, nullptr };
            }
        };
        constexpr EpTable kEp{};

        const char* ClassName(EpClass c) {
            switch (c) {
            case EpClass::kPlayerEngine:   return "player-engine";
            case EpClass::kPlayerActivity: return "player-activity";
            case EpClass::kNever:          return "never-called";
            default:                       return "any-actor";
            }
        }

        enum class Verdict : std::uint8_t { kCopy, kNoUse, kPlayerOnly, kScript };
        const char* VerdictName(Verdict v) {
            switch (v) {
            case Verdict::kNoUse:      return "not-copied:no-follower-use";
            case Verdict::kPlayerOnly: return "not-copied:player-only";
            case Verdict::kScript:     return "not-copied:script-part";
            default:                   return "copy";
            }
        }

        // ── the perk-tree set (built once, main thread) ─────────────────────────────────────────────
        std::unordered_set<RE::FormID> g_treePerks;
        bool                           g_treeBuilt = false;
        std::size_t                    g_treeCount = 0;
        std::vector<RE::BGSPerk*>      g_treePtrs;   // the same perks as pointers (HasPerk sanity count)

        void AddChain(RE::BGSPerk* a_perk, std::unordered_set<RE::FormID>& a_out) {
            // rank chain: bounded and cycle-guarded like Progression.cpp's BuildSkill walker
            for (int i = 0; a_perk && i < 16; ++i) {
                if (!a_out.insert(a_perk->GetFormID()).second) break;
                g_treePtrs.push_back(a_perk);
                a_perk = a_perk->nextPerk;
            }
        }

        void WalkTree(RE::BGSSkillPerkTreeNode* a_root, std::unordered_set<RE::FormID>& a_out) {
            std::vector<RE::BGSSkillPerkTreeNode*>        stack{ a_root };
            std::unordered_set<RE::BGSSkillPerkTreeNode*> seen;
            while (!stack.empty() && seen.size() < 100000) {
                auto* n = stack.back();
                stack.pop_back();
                if (!n || !seen.insert(n).second) continue;
                AddChain(n->perk, a_out);
                for (auto* c : n->children) stack.push_back(c);
            }
        }

        void BuildTreeSet() {
            g_treeBuilt = true;
            std::unordered_set<RE::ActorValueInfo*> avifs;
            std::size_t fromList = 0, fromRecords = 0;
            if (auto* avl = RE::ActorValueList::GetSingleton()) {
                for (std::uint32_t av = 0; av < static_cast<std::uint32_t>(RE::ActorValue::kTotal); ++av)
                    if (auto* info = avl->GetActorValue(static_cast<RE::ActorValue>(av)))
                        if (avifs.insert(info).second) ++fromList;
            }
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* info : dh->GetFormArray<RE::ActorValueInfo>())
                    if (info && avifs.insert(info).second) ++fromRecords;
            }
            std::size_t trees = 0;
            for (auto* info : avifs) {
                if (!info || !info->perkTree) continue;
                ++trees;
                WalkTree(info->perkTree, g_treePerks);
            }
            g_treeCount = g_treePerks.size();
            spdlog::info("[inherit-census] tree set: {} perk form(s) from {} tree(s) ({} AVIF via the built-in list + {} more from records)",
                         g_treeCount, trees, fromList, fromRecords);
            if (g_treeCount == 0)
                spdlog::error("[inherit-census] tree set is EMPTY -- every perk the player holds would look hidden; census withheld");
        }

        // ── the player's perks ──────────────────────────────────────────────────────────────────────
        // TWO roads, because no single engine road sees every perk (disassembly of 1.6.1170, Opus review):
        //  (a) Actor::HasPerk -> ForEachPerk: the player's BASE TESNPC perks + the runtime addedPerks list.
        //      PLAYER_RUNTIME_DATA is EMPTY in this build's CommonLib config (SE+AE+VR enabled: the members
        //      are only emitted for a single-runtime build), so those lists are not reachable by name; the
        //      census asks the engine instead, over every BGSPerk form that is in no tree.
        //  (b) A perk applied through a magic effect's "Perk to Apply" (EffectSetting::data.perk) is written
        //      by ApplyTemporaryPerk (vfunc 0xFD) into the standingStonePerks array, which HasPerk NEVER
        //      reads. Its only caller is ActiveEffect start. So the active-effect list is walked too:
        //      standing stones, racial / SPID abilities and rule abilities (Requiem style) come in this way.
        // A perk granted by any OTHER road is NOT seen. No raw offsets are used.
        std::vector<RE::BGSPerk*> g_candidates;   // every BGSPerk form NOT in the tree set (built once)
        bool                      g_candBuilt = false;
        std::size_t               g_formsSeen = 0;

        void BuildCandidates() {
            g_candBuilt = true;
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* p : dh->GetFormArray<RE::BGSPerk>()) {
                    if (!p) continue;
                    ++g_formsSeen;
                    if (g_treePerks.count(p->GetFormID())) continue;
                    g_candidates.push_back(p);
                }
            }
            spdlog::info("[inherit-census] candidate forms: {} perk form(s) in the data, {} in no tree", g_formsSeen, g_candidates.size());
        }

        bool InPlayerBase(RE::PlayerCharacter* a_player, RE::BGSPerk* a_perk) {
            auto* base = a_player->GetActorBase();
            auto* arr  = base ? static_cast<RE::BGSPerkRankArray*>(base) : nullptr;
            if (!arr || !arr->perks) return false;
            for (std::uint32_t i = 0; i < arr->perkCount; ++i)
                if (arr->perks[i].perk == a_perk) return true;
            return false;
        }

        // One perk the player holds that is in no tree, and WHICH road saw it.
        struct Held {
            RE::BGSPerk* perk   = nullptr;
            bool         engine = false;   // HasPerk answered yes (base record or addedPerks)
            std::string  via;              // "mgef:<MGEF id>/<spell id>[:inactive]" for each active effect that applies it
        };

        // Road (b): every non-dispelled active effect on the player whose base MGEF has a Perk to Apply.
        // MAIN THREAD. Read-only walk of the active-effect list (the production road: cast/Direct.cpp,
        // cast/Summon.cpp, ProgProbe.cpp).
        void CollectMgefPerks(RE::PlayerCharacter* a_player, std::vector<Held>& a_out) {
            auto* mt = a_player->AsMagicTarget();
            if (!mt) return;
            auto* list = mt->GetActiveEffectList();
            if (!list) return;
            for (auto* ae : *list) {
                if (!ae || ae->flags.any(RE::ActiveEffect::Flag::kDispelled)) continue;
                auto* mg = ae->GetBaseObject();
                if (!mg || !mg->data.perk || g_treePerks.count(mg->data.perk->GetFormID())) continue;
                std::string tag = std::format("mgef:{:08X}/{:08X}{}", mg->GetFormID(), ae->spell ? ae->spell->GetFormID() : 0u,
                                              ae->flags.any(RE::ActiveEffect::Flag::kInactive) ? ":inactive" : "");
                bool merged = false;
                for (auto& h : a_out)
                    if (h.perk == mg->data.perk) {
                        if (h.via.find(tag) == std::string::npos) h.via += "," + tag;
                        merged = true;
                        break;
                    }
                if (!merged) a_out.push_back(Held{ mg->data.perk, false, std::move(tag) });
            }
        }

        // ── condition + function-data identity (rule 2) ─────────────────────────────────────────────
        // Same fields as Progression::SameConditions, applied to one condition LIST (an EP entry's tab).
        // Two EMPTY lists are identical here (an entry with no conditions twins another with none);
        // SameConditions' "an empty perk-level list is never identical" guards a different question.
        bool SameCondList(const RE::TESCondition& a_a, const RE::TESCondition& a_b) {
            auto* x = a_a.head;
            auto* y = a_b.head;
            for (; x && y; x = x->next, y = y->next) {
                const auto& p = x->data;
                const auto& q = y->data;
                if (p.functionData.function.get() != q.functionData.function.get() ||
                    p.functionData.params[0] != q.functionData.params[0] ||
                    p.functionData.params[1] != q.functionData.params[1] ||
                    std::memcmp(&p.flags, &q.flags, sizeof(p.flags)) != 0 ||
                    p.object.get() != q.object.get() ||
                    p.dataID != q.dataID ||
                    p.runOnRef != q.runOnRef)
                    return false;
                if (p.flags.global ? p.comparisonValue.g != q.comparisonValue.g
                                   : p.comparisonValue.f != q.comparisonValue.f)
                    return false;
            }
            return !x && !y;
        }

        std::size_t TabCount(const RE::BGSEntryPointPerkEntry& a_ep) {
            return std::min<std::size_t>(a_ep.entryData.numArgs, a_ep.conditions.size());
        }

        // Only the function-data shapes whose payload is readable through a verified member are
        // comparable; any other shape (two-value, leveled list, activate choice ...) is "not identical".
        bool SameFuncData(RE::BGSEntryPointFunctionData* a_a, RE::BGSEntryPointFunctionData* a_b) {
            if (!a_a || !a_b) return a_a == a_b;
            const auto t = a_a->GetType();
            if (t != a_b->GetType()) return false;
            using FT = RE::BGSEntryPointFunctionData::FunctionType;
            switch (t) {
            case FT::kOneValue:
                return static_cast<RE::BGSEntryPointFunctionDataOneValue*>(a_a)->data ==
                       static_cast<RE::BGSEntryPointFunctionDataOneValue*>(a_b)->data;
            case FT::kSpellItem:
                return static_cast<RE::BGSEntryPointFunctionDataSpellItem*>(a_a)->spell ==
                       static_cast<RE::BGSEntryPointFunctionDataSpellItem*>(a_b)->spell;
            case FT::kText: {
                const char* x = static_cast<RE::BGSEntryPointFunctionDataText*>(a_a)->text.c_str();
                const char* y = static_cast<RE::BGSEntryPointFunctionDataText*>(a_b)->text.c_str();
                return x && y && std::strcmp(x, y) == 0;
            }
            default:
                return false;
            }
        }

        bool TwinEntry(const RE::BGSEntryPointPerkEntry& a_a, const RE::BGSEntryPointPerkEntry& a_b) {
            if (a_a.entryData.entryPoint.get() != a_b.entryData.entryPoint.get() ||
                a_a.entryData.function.get() != a_b.entryData.function.get())
                return false;
            const auto tabs = TabCount(a_a);
            if (tabs != TabCount(a_b)) return false;
            if (!SameFuncData(a_a.functionData, a_b.functionData)) return false;
            for (std::size_t t = 0; t < tabs; ++t)
                if (!SameCondList(a_a.conditions[t], a_b.conditions[t])) return false;
            return true;
        }

        // ── EVIDENCE for telling a RULE perk from a BONUS (marth 2026-11-10); logged, never decided on ──
        // Polarity of an entry point from the OWNER's side: +1 = a higher value helps the owner,
        // -1 = a lower value helps the owner, 0 = not computable (target-dependent, depends on the
        // activity, or unknown). A HEURISTIC HINT for reading the census, not a verdict.
        int Polarity(std::size_t a_ep) {
            switch (a_ep) {
            case 0: case 1: case 2: case 6: case 10: case 18: case 21: case 23: case 26: case 28:
            case 29: case 30: case 34: case 35: case 39: case 40: case 56: case 59: case 60:
            case 66: case 76: case 77: case 85:
                return +1;
            case 8: case 17: case 27: case 33: case 36: case 38: case 41: case 42:
                return -1;
            default:
                return 0;
            }
        }

        enum class Dir : std::uint8_t { kUnknown, kFavour, kAgainst };
        const char* DirName(Dir d) { return d == Dir::kFavour ? "favour" : d == Dir::kAgainst ? "against" : "unknown"; }

        // Only a one-value Multiply (x v) or Add (+ v) has a computable sign of change.
        Dir EntryDir(const RE::BGSEntryPointPerkEntry& a_ep) {
            using EF = RE::BGSEntryPointPerkEntry::EntryData::Function;
            const auto idx = static_cast<std::size_t>(a_ep.entryData.entryPoint.get());
            const int pol = Polarity(idx);
            if (pol == 0 || !a_ep.functionData ||
                a_ep.functionData->GetType() != RE::BGSEntryPointFunctionData::FunctionType::kOneValue)
                return Dir::kUnknown;
            const float v = static_cast<RE::BGSEntryPointFunctionDataOneValue*>(a_ep.functionData)->data;
            float delta = 0.0f;
            switch (a_ep.entryData.function.get()) {
            case EF::kMultiplyValue: delta = v - 1.0f; break;
            case EF::kAddValue:      delta = v; break;
            default:                 return Dir::kUnknown;
            }
            if (delta == 0.0f) return Dir::kUnknown;
            return ((delta > 0.0f ? +1 : -1) * pol) > 0 ? Dir::kFavour : Dir::kAgainst;
        }

        // ── one candidate's analysis ────────────────────────────────────────────────────────────────
        struct Analysis {
            std::vector<RE::BGSEntryPointPerkEntry*> eps;
            int  abilities    = 0;
            int  quests       = 0;
            bool scriptEp     = false;   // an EP entry with kAddActivateChoice
            bool scriptMgef   = false;   // an ability effect whose MGEF is archetype kScript
            bool allEpNoUse   = true;    // every EP entry is class-1-no-use or never-called
            bool allPartsPinned = true;  // every EP owner tab / ability effect is ConditionPlayerGated
            int  firingParts  = 0;
            Progression::PerkOwnerFacts owner;
            bool condSkill    = false;   // a condition reads an actor value (GetActorValue / GetBaseActorValue / ...Percent)
            bool condGlobal   = false;   // a condition reads a global (GetGlobalValue)
            bool condAny      = false;   // at least one condition exists
            bool condPlayerish = false;  // a condition tests the player / the player's faction in ANY shape (evidence only,
                                         // never a verdict input: GetPCIs*/IsPC*/GetPCInFaction, GetInFaction PlayerFaction,
                                         // GetIsID/GetIsReference naming the player incl. non-literal and OR-grouped)
            int  favour = 0, against = 0, unknownDir = 0;   // per-entry direction tally (EP entries)
            Verdict verdict   = Verdict::kCopy;
            const char* reason = "";
        };

        bool AbilityPinned(RE::SpellItem* a_spell) {
            if (!a_spell || a_spell->effects.empty()) return false;
            for (auto* eff : a_spell->effects) {
                if (!eff) return false;
                if (!Progression::ConditionPlayerGated(eff->conditions) &&
                    !(eff->baseEffect && Progression::ConditionPlayerGated(eff->baseEffect->conditions)))
                    return false;
            }
            return true;
        }

        void ScanConds(const RE::TESCondition& a_cond, Analysis& a) {
            using Fn = RE::FUNCTION_DATA::FunctionID;
            for (auto* it = a_cond.head; it; it = it->next) {
                a.condAny = true;
                const auto fn = it->data.functionData.function.get();
                if (fn == Fn::kGetActorValue || fn == Fn::kGetBaseActorValue || fn == Fn::kGetActorValuePercent) a.condSkill = true;
                else if (fn == Fn::kGetGlobalValue) a.condGlobal = true;
                if (fn == Fn::kGetPCIsClass || fn == Fn::kGetPCIsRace || fn == Fn::kGetPCIsSex || fn == Fn::kGetPCInFaction ||
                    fn == Fn::kIsPCSleeping || fn == Fn::kIsPCAMurderer) {
                    a.condPlayerish = true;
                } else if (fn == Fn::kGetInFaction || fn == Fn::kGetIsID || fn == Fn::kGetIsReference) {
                    auto* form = static_cast<RE::TESForm*>(it->data.functionData.params[0]);
                    const auto id = form ? form->GetFormID() : 0u;
                    // 0xDB1 = PlayerFaction (Skyrim.esm); 0x7 / 0x14 = the player base / PlayerRef
                    if (fn == Fn::kGetInFaction ? id == 0xDB1u : (id == 0x7u || id == 0x14u)) a.condPlayerish = true;
                }
            }
        }

        void Analyse(RE::BGSPerk* a_perk, Analysis& a) {
            using EF = RE::BGSEntryPointPerkEntry::EntryData::Function;
            for (auto* entry : a_perk->perkEntries) {
                if (!entry) continue;
                switch (entry->GetType()) {
                case RE::PERK_ENTRY_TYPE::kEntryPoint: {
                    auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
                    a.eps.push_back(ep);
                    const auto idx = static_cast<std::size_t>(ep->entryData.entryPoint.get());
                    if (ep->entryData.function.get() == EF::kAddActivateChoice) a.scriptEp = true;
                    const EpInfo info = idx < kEpCount ? kEp.e[idx] : EpInfo{ EpClass::kAny, nullptr };
                    const bool noUse = (info.cls == EpClass::kPlayerEngine && !info.use) || info.cls == EpClass::kNever;
                    if (!noUse) a.allEpNoUse = false;
                    ++a.firingParts;
                    const auto tabs = TabCount(*ep);
                    if (tabs == 0 || !Progression::ConditionPlayerGated(ep->conditions[0])) a.allPartsPinned = false;
                    switch (EntryDir(*ep)) {
                    case Dir::kFavour:  ++a.favour; break;
                    case Dir::kAgainst: ++a.against; break;
                    default:            ++a.unknownDir; break;
                    }
                    for (std::size_t t = 0; t < tabs; ++t) {
                        ScanConds(ep->conditions[t], a);
                        Progression::PerkOwnerFacts f;
                        Progression::ReadOwnerFacts(ep->conditions[t],
                                                    t == 0 ? Progression::OwnerCtx::kSelf : Progression::OwnerCtx::kOther, f);
                        a.owner.Merge(f);
                    }
                    break;
                }
                case RE::PERK_ENTRY_TYPE::kAbility: {
                    auto* ab = static_cast<RE::BGSAbilityPerkEntry*>(entry);
                    ++a.abilities;
                    ++a.firingParts;
                    a.allEpNoUse = false;   // an ability is a live effect on whoever holds it
                    if (!AbilityPinned(ab->ability)) a.allPartsPinned = false;
                    if (ab->ability) {
                        for (auto* eff : ab->ability->effects) {
                            if (!eff) continue;
                            Progression::PerkOwnerFacts f;
                            ScanConds(eff->conditions, a);
                            if (eff->baseEffect) ScanConds(eff->baseEffect->conditions, a);
                            Progression::ReadOwnerFacts(eff->conditions, Progression::OwnerCtx::kSelf, f);
                            if (eff->baseEffect) {
                                Progression::ReadOwnerFacts(eff->baseEffect->conditions, Progression::OwnerCtx::kSelf, f);
                                if (eff->baseEffect->HasArchetype(RE::EffectSetting::Archetype::kScript)) a.scriptMgef = true;
                            }
                            a.owner.Merge(f);
                        }
                    }
                    break;
                }
                default:   // kQuest
                    ++a.quests;
                    a.allEpNoUse = false;
                    break;
                }
            }

            // rule 1, then rule 3, then rule 4 (the research order); first hit wins
            if (!a.eps.empty() && a.abilities == 0 && a.quests == 0 && a.allEpNoUse) {
                a.verdict = Verdict::kNoUse;
                a.reason  = "every entry point is player-only in the executable with no follower activity (or never evaluated)";
            } else if (a.firingParts > 0 && a.allPartsPinned) {
                a.verdict = Verdict::kPlayerOnly;
                a.reason  = "every firing part is behind an AND-required is-player condition";
            } else if (a.scriptEp || a.scriptMgef || a.quests > 0) {
                a.verdict = Verdict::kScript;
                a.reason  = a.scriptEp ? "entry point function Add Activate Choice carries a script fragment"
                          : a.scriptMgef ? "an ability effect has a Script archetype"
                                         : "a quest-stage entry";
            } else {
                a.verdict = Verdict::kCopy;
                a.reason  = "engine evaluates it with any owner, or a followed activity asks for it";
            }
        }

        // rules 0 and 2 against ONE follower base: is the candidate already there, or twinned entry by entry?
        bool DuplicateOn(RE::BGSPerk* a_perk, const Analysis& a, RE::TESNPC* a_base) {
            auto* arr = static_cast<RE::BGSPerkRankArray*>(a_base);
            if (!arr->perks) return false;
            for (std::uint32_t i = 0; i < arr->perkCount; ++i)
                if (arr->perks[i].perk == a_perk) return true;   // rule 0: same form
            if (a.eps.empty() || a.abilities > 0 || a.quests > 0) return false;   // rule 2 needs a pure-EP perk
            for (auto* ep : a.eps) {
                bool twin = false;
                for (std::uint32_t i = 0; i < arr->perkCount && !twin; ++i) {
                    auto* q = arr->perks[i].perk;
                    if (!q || q == a_perk) continue;
                    for (auto* qe : q->perkEntries) {
                        if (!qe || qe->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) continue;
                        if (TwinEntry(*ep, *static_cast<RE::BGSEntryPointPerkEntry*>(qe))) { twin = true; break; }
                    }
                }
                if (!twin) return false;
            }
            return true;
        }

        void AppendEp(std::string& a_s, const RE::BGSEntryPointPerkEntry& a_ep) {
            const auto idx = static_cast<std::uint32_t>(a_ep.entryData.entryPoint.get());
            const EpInfo info = idx < kEpCount ? kEp.e[idx] : EpInfo{ EpClass::kAny, nullptr };
            a_s += std::format(" EP{}:{}[{} use={} fn={}", idx, Progression::EntryPointName(idx),
                               idx < kEpCount ? ClassName(info.cls) : "unknown-ep", info.use ? info.use : "none",
                               static_cast<int>(a_ep.entryData.function.get()));
            if (a_ep.functionData && a_ep.functionData->GetType() == RE::BGSEntryPointFunctionData::FunctionType::kOneValue)
                a_s += std::format(" val={:g}", static_cast<RE::BGSEntryPointFunctionDataOneValue*>(a_ep.functionData)->data);
            const auto tabs = TabCount(a_ep);
            a_s += std::format(" dir={} tabs={}", DirName(EntryDir(a_ep)), tabs);
            if (tabs > 0 && Progression::ConditionPlayerGated(a_ep.conditions[0])) a_s += " pinned";
            a_s += "]";
        }

        // ── state (main thread only) ────────────────────────────────────────────────────────────────
        // The sweep is a CONTINUOUS, time-sliced pass over the candidate forms: kSweepPerPoll HasPerk calls
        // per poll (~2 s), so a pass over a few thousand perks spans a few dozen polls and no frame pays
        // for it. Hidden perks change only by script AddPerk / an effect starting (principle 9: no timer or
        // level trigger is needed). The signature is compared at the end of each full pass.
        constexpr std::size_t kSweepPerPoll   = 250;
        constexpr int         kMaxCensusLines = 600;
        int                       g_seenGen    = -1;
        std::size_t               g_treeIdx    = 0;       // tree-perk HasPerk sanity count: once per load generation
        std::size_t               g_treeHeld   = 0;
        bool                      g_treeCounted = false;
        std::size_t               g_candIdx    = 0;
        std::vector<RE::BGSPerk*> g_passHeld;             // HasPerk-found perks so far in the current pass
        bool                      g_fullLogged = false;   // the first full dump of this generation is done
        bool                      g_haveSig    = false;
        std::uint64_t             g_lastSig    = 0;
        std::vector<RE::BGSPerk*> g_prev;                 // the held set at the last logged census (for added/removed)

        // The follower bases rules 0/2 compare against: active, MFO-managed, persistable, not summons.
        std::vector<RE::TESNPC*> FollowerBases() {
            std::vector<RE::TESNPC*> bases;
            const auto snap = Followers::ActiveSnapshot();
            for (const RE::FormID fid : *snap) {
                if (!Followers::IsPersistableID(fid) || !Followers::IsMfoEnabled(fid)) continue;
                auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
                if (!a || a->IsCommandedActor()) continue;
                if (auto* b = a->GetActorBase()) bases.push_back(b);
            }
            return bases;
        }

        // mode full = every perk gets a line; delta = only perks added since the last logged census get
        // a line, removed perks get a REMOVED line. The summary always counts the WHOLE held set.
        void RunCensus(RE::PlayerCharacter* a_player, std::uint64_t a_sig, const std::vector<Held>& held, bool a_full) {
            const auto bases = FollowerBases();
            std::unordered_set<RE::FormID> prev, now;
            for (auto* p : g_prev) prev.insert(p->GetFormID());
            for (const auto& h : held) now.insert(h.perk->GetFormID());

            std::size_t counts[4] = {}, playerRef = 0, dupSome = 0, scriptEp = 0, scriptMgef = 0, questParts = 0, hiddenFlag = 0,
                        viaEngine = 0, viaMgef = 0, added = 0, removed = 0;
            int lines = 0;
            for (const auto& h : held) {
                auto* p = h.perk;
                Analysis a;
                Analyse(p, a);
                ++counts[static_cast<int>(a.verdict)];
                if (a.owner.playerRefRunOn || a.owner.playerRefParam) ++playerRef;
                if (a.scriptEp) ++scriptEp;
                if (a.scriptMgef) ++scriptMgef;
                if (a.quests > 0) ++questParts;
                if (p->data.hidden) ++hiddenFlag;
                if (h.engine) ++viaEngine;
                if (!h.via.empty()) ++viaMgef;
                std::size_t dups = 0;
                for (auto* b : bases) if (DuplicateOn(p, a, b)) ++dups;
                if (dups) ++dupSome;
                const bool isNew = !prev.count(p->GetFormID());
                if (isNew && !a_full) ++added;
                if (!a_full && !isNew) continue;     // delta: unchanged perks are not re-listed
                if (lines >= kMaxCensusLines) continue;
                ++lines;
                const char* edid = p->GetFormEditorID();
                // src: which road saw it. "base" = the player's base record; "has-perk" = HasPerk yes but not in
                // the base record (runtime-added); "mgef:..." = applied by an active effect's Perk to Apply.
                std::string src;
                if (h.engine) src = InPlayerBase(a_player, p) ? "base" : "has-perk(not-in-base)";
                if (!h.via.empty()) src += (src.empty() ? "" : "+") + h.via;
                const char* fullName = p->GetName();
                RE::BSString descBuf;
                p->TESDescription::GetDescription(descBuf, p);
                const bool hasDesc = descBuf.c_str() && *descBuf.c_str();
                std::string eps;
                for (auto* ep : a.eps) AppendEp(eps, *ep);
                spdlog::info("[inherit-census] {}{:08X} edid='{}' name='{}' hidden={} playable={} hasName={} hasDesc={} src={} "
                             "entries={} verdict={} reason='{}' ep={} ability={} quest={} "
                             "dirFavour={} dirAgainst={} dirUnknown={} condSkill={} condGlobal={} condPlayerPin={} condPlayerish={} condAny={} "
                             "playerRefRunOn={} playerRefParam={} selfPin={} dupOnFollowers={}/{}{}",
                             a_full ? "" : "ADDED ", p->GetFormID(), edid ? edid : "", NameOf(p), p->data.hidden ? 1 : 0,
                             p->data.playable ? 1 : 0, (fullName && *fullName) ? 1 : 0, hasDesc ? 1 : 0, src,
                             a.eps.size() + static_cast<std::size_t>(a.abilities + a.quests),
                             VerdictName(a.verdict), a.reason, a.eps.size(), a.abilities, a.quests,
                             a.favour, a.against, a.unknownDir, a.condSkill ? 1 : 0, a.condGlobal ? 1 : 0,
                             (a.firingParts > 0 && a.allPartsPinned) ? 1 : 0, a.condPlayerish ? 1 : 0, a.condAny ? 1 : 0,
                             a.owner.playerRefRunOn ? 1 : 0, a.owner.playerRefParam ? 1 : 0, a.owner.selfPin ? 1 : 0,
                             dups, bases.size(), eps);
            }
            if (!a_full) {
                for (auto* p : g_prev) {
                    if (now.count(p->GetFormID())) continue;
                    ++removed;
                    const char* edid = p->GetFormEditorID();
                    spdlog::info("[inherit-census] REMOVED {:08X} edid='{}' name='{}'", p->GetFormID(), edid ? edid : "", NameOf(p));
                }
            }
            if (lines >= kMaxCensusLines)
                spdlog::warn("[inherit-census] per-perk lines capped at {} (more exist; the summary counts all)", kMaxCensusLines);
            spdlog::info("[inherit-census] summary mode={} sig={:016X} gen={} held-in-no-tree={} (seen by HasPerk={} by active-effect Perk-to-Apply={}) "
                         "added={} removed={} treePerksHeld={} of {} | copy={} no-follower-use={} player-only={} script-part={} "
                         "(activate-choice={} script-mgef={} quest={}) | playerRef={} duplicateOnSomeFollower={} hiddenFlag={} followers={} "
                         "candidateForms={} | sources: HasPerk (base+added) and active-effect Perk to Apply ONLY; a perk granted any other way is not seen",
                         a_full ? "full" : "delta", a_sig, g_pollGen, held.size(), viaEngine, viaMgef, added, removed,
                         g_treeHeld, g_treePtrs.size(), counts[0], counts[1], counts[2], counts[3], scriptEp, scriptMgef, questParts,
                         playerRef, dupSome, hiddenFlag, bases.size(), g_candidates.size());
            g_prev.clear();
            for (const auto& h : held) g_prev.push_back(h.perk);
        }

        // One time slice of the sweep. Returns true when a full pass has just completed.
        bool SweepSlice(RE::PlayerCharacter* a_player) {
            std::size_t budget = kSweepPerPoll;
            if (!g_treeCounted) {   // sanity figure, once per load generation: 0 on a mid-game character = HasPerk is not answering
                for (; g_treeIdx < g_treePtrs.size() && budget; ++g_treeIdx, --budget)
                    if (a_player->HasPerk(g_treePtrs[g_treeIdx])) ++g_treeHeld;
                if (g_treeIdx < g_treePtrs.size()) return false;
                g_treeCounted = true;
            }
            for (; g_candIdx < g_candidates.size() && budget; ++g_candIdx, --budget)
                if (a_player->HasPerk(g_candidates[g_candIdx])) g_passHeld.push_back(g_candidates[g_candIdx]);
            return g_candIdx >= g_candidates.size();
        }

    }

    // MAIN THREAD (PollWork). Per poll: at most kSweepPerPoll HasPerk calls. When a full pass over the
    // candidate forms completes, the active-effect list is walked once, the held set is hashed, and the
    // census logs only if this is the first pass of the load generation (full dump) or the hash changed
    // (delta: the added / removed perks only).
    void InheritCensusPoll(RE::PlayerCharacter* a_player) {
        if (!a_player || !Config::g_inheritPerkCensus.load()) return;
        if (!g_treeBuilt) BuildTreeSet();
        if (g_treeCount == 0) return;   // already logged loudly; a census against an empty tree set would be noise
        if (!g_candBuilt) BuildCandidates();
        if (g_seenGen != g_pollGen) {   // a load: start over (the per-load guard DumpLedgerOnce uses)
            g_seenGen = g_pollGen;
            g_treeIdx = 0; g_treeHeld = 0; g_treeCounted = false; g_candIdx = 0;
            g_passHeld.clear(); g_fullLogged = false; g_haveSig = false; g_prev.clear();
        }
        if (!SweepSlice(a_player)) return;

        std::vector<Held> held;
        held.reserve(g_passHeld.size() + 8);
        for (auto* p : g_passHeld) held.push_back(Held{ p, true, {} });
        std::vector<Held> mg;
        CollectMgefPerks(a_player, mg);
        for (auto& m : mg) {
            bool merged = false;
            for (auto& h : held)
                if (h.perk == m.perk) { h.via = m.via; merged = true; break; }
            if (!merged) held.push_back(std::move(m));
        }
        // deterministic order for the hash: the engine-found part keeps candidate order, the effect-only
        // part (list order can vary) is sorted by form id
        std::stable_sort(held.begin(), held.end(), [](const Held& x, const Held& y) {
            if (x.engine != y.engine) return x.engine;
            return !x.engine && x.perk->GetFormID() < y.perk->GetFormID();
        });
        std::uint64_t sig = 1469598103934665603ull;   // FNV-1a
        for (const auto& h : held) { sig ^= h.perk->GetFormID(); sig *= 1099511628211ull; }
        g_candIdx = 0;
        g_passHeld.clear();

        if (g_fullLogged && g_haveSig && sig == g_lastSig) return;
        const bool full = !g_fullLogged;
        g_fullLogged = true;
        g_haveSig    = true;
        g_lastSig    = sig;
        RunCensus(a_player, sig, held, full);
    }

}
