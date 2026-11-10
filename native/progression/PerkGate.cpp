// progression/PerkGate.cpp -- PERKS: the grant plumbing (P1/P3-proven), the
// perk-condition evaluator, the B-prime native-perk strip / restore, the §5
// double gate (GateNextRank), the rank grant and the session reapply.
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
#include <cstring>   // memcpy (EngineItemTrue)
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"   // the split family's shared substrate
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

namespace MFO::ProgAllocator {

    namespace {

        // ── perk grant plumbing (P1/P3-proven paths only) ───────────────────

        // A follower's perks live on the BASE (TESNPC), not the actor —
        // Actor::HasPerk alone reports FALSE for a base/native perk the
        // follower genuinely owns (e.g. a root Mastery perk). Ownership must
        // test the base index too, exactly like OwnsAnyRank (§ deck
        // 2026-08-15: Force of Nature read its owned Destruction-Mastery
        // prereq as MISSING because HasPerk-only missed the base perk).
        bool OwnsExactPerk(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            return a_perk && ((a_base && a_base->GetPerkIndex(a_perk).has_value()) ||
                              a_actor->HasPerk(a_perk));
        }

        // RANK-AWARE prereq ownership (RC65 #1). A HasPerk prereq that names
        // rank J of a MULTI-RANK chain is a ">= J" gate, not "== J": owning a
        // LATER rank K>J in the same `nextPerk` chain necessarily means the
        // follower already passed rank J. But the engine's perk list may carry
        // only the HIGHEST rank held, so an EXACT-form test reports the lower
        // prereq MISSING (marth's "gated == when the skill has multiple
        // levels"). So we walk `nextPerk` FORWARD from the required rank and
        // accept ownership of the rank itself OR any later rank. We deliberately
        // NEVER walk backward — owning an EARLIER rank must NOT satisfy a
        // later-rank prereq. Bounded + cycle-guarded (cap 16, like
        // Progression.cpp:449) against a malformed overhaul record.
        bool OwnsPerkForm(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            RE::BGSPerk* const head = a_perk;
            int rankNo = 0;
            for (RE::BGSPerk* r = a_perk; r && rankNo < 16; r = r->nextPerk, ++rankNo) {
                if (rankNo > 0 && r == head) break;   // looped chain — bail
                if (OwnsExactPerk(a_actor, a_base, r)) return true;
            }
            return false;
        }

        // Is this perk something a follower could actually allocate on the
        // board — i.e. it lives in a catalog node that survived the §3 filter
        // (at least one non-dead rank)? A HasPerk PREREQ pointing at a perk
        // that is NOT board-allocatable (filtered player-only rank, or a perk
        // in no MFO tree at all) can never be satisfied on a follower, so §5.1
        // already bridges around it — §5.2 must NOT hard-block on it.
        bool PerkAllocatableInCatalog(RE::FormID a_perk) {
            if (!a_perk) return false;
            // O(1) via the index: the map ties a_perk to the sole node holding it
            // (as one of its ranks); allocatable iff that node has any non-dead rank.
            const auto& idx = NodeIndex();
            auto it = idx.find(a_perk);
            if (it == idx.end() || !it->second.node) return false;
            for (const auto& r : it->second.node->ranks)
                if (r.verdict != Progression::Verdict::kDead) return true;
            return false;
        }

        // Is this perk KNOWN to the catalog AT ALL — present in some node's
        // ranks (kept OR dead) or in a tree's `filtered` list? Distinct from
        // PerkAllocatableInCatalog, which additionally requires a non-dead
        // (allocatable) rank. A prereq HasPerk on a perk in NO tree at all
        // (a quest/SPID-granted gate) is NOT known here — §5.2 must then DEFER
        // it to the engine rather than granting it (a follower may genuinely
        // lack such a perk). Only a KNOWN-but-filtered perk is safe to bypass.
        bool PerkKnownToCatalog(RE::FormID a_perk) {
            if (!a_perk) return false;
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes)
                    for (const auto& r : node.ranks)
                        if (r.perkFormID == a_perk) return true;
                for (const auto& f : tree.filtered)
                    if (f.perkFormID == a_perk) return true;
            }
            return false;
        }

        // "Alias twins" (Progression.cpp BuildPerkTwins): the in-tree rank an off-tree perk stands for, or
        // the perk itself when it has no twin.
        RE::FormID TwinOf(RE::FormID a_perk) {
            const auto& m = Progression::Get().perkTwin;
            const auto it = m.find(a_perk);
            return it == m.end() ? a_perk : it->second;
        }

        // §5.2b (deck 2026-08-15): a skill perk's own conditions normally
        // reduce to a pure AND-chain of the skill-level gate
        // (GetBaseActorValue-on-self >= N) plus, in some overhauls, HasPerk
        // <prereq> items that encode a prerequisite as a condition. When such a
        // prereq perk has been FILTERED out of the tree, the engine's
        // whole-chain IsTrue locks the node forever even though the skill is
        // met and §5.1's bridging already routed the prerequisite around the
        // filtered perk. We evaluate the common shapes ourselves, item by item,
        // keeping MFO's own AND/OR grouping:
        //   - GetBaseActorValue / GetActorValue / GetLevel on self, all six
        //     comparison operators.
        //   - HasPerk <P> on self, all six operators. A board-allocatable P (or an
        //     off-tree ALIAS TWIN of one) is tested by ownership: the rank itself
        //     or any LATER rank of its chain counts as held, so "HasPerk(rank1) == 0"
        //     is false for a follower holding rank 2 (GrantRank removes rank K-1,
        //     the engine's own HasPerk(rank1) would read 0 and let an exclusion
        //     through). A KNOWN-but-filtered P with a "req" shape is satisfied
        //     (§5.1 bridges it).
        // Any OTHER item (a shape we will not guess: quest, faction, global,
        // GetLevel-on-target, a HasPerk on a perk in no tree...) is handed to the
        // engine ALONE (EngineItemTrue) and slots into the same grouping. One
        // exotic item no longer sends the whole perk to the engine.
        // One condition item's truth for THIS follower, tri-stated:
        //   kTrue/kFalse — we evaluated it; kUnknown — the caller asks the engine
        //   about this ONE item.
        enum class Tri { kTrue, kFalse, kUnknown };

        const char* OpText(RE::CONDITION_ITEM_DATA::OpCode a_op) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            switch (a_op) {
            case Op::kEqualTo:              return "==";
            case Op::kNotEqualTo:           return "!=";
            case Op::kGreaterThan:          return ">";
            case Op::kGreaterThanOrEqualTo: return ">=";
            case Op::kLessThan:             return "<";
            case Op::kLessThanOrEqualTo:    return "<=";
            }
            return "op?";
        }

        // Quest / world progress and player misc stats mean the PLAYER's world even when the item is
        // "run on subject" (marth: "world progress maps player"). Quest stage, quest completion,
        // quest variable, global value and GetPCMiscStat read global state or the player's stats whatever
        // ref they are run on, so the engine item is handed the PLAYER as its subject.
        bool IsWorldProgressFn(RE::FUNCTION_DATA::FunctionID a_fn) {
            using Fn = RE::FUNCTION_DATA::FunctionID;
            return a_fn == Fn::kGetStage || a_fn == Fn::kGetStageDone || a_fn == Fn::kGetQuestRunning ||
                   a_fn == Fn::kGetQuestCompleted || a_fn == Fn::kGetQuestVariable ||
                   a_fn == Fn::kGetGlobalValue || a_fn == Fn::kGetPCMiscStat;
        }

        // The engine's verdict on ONE item: a stack copy of the item, next = null, isOR cleared, wrapped
        // in a TESCondition-shaped holder (a TESCondition's only member is the list head, and its
        // destructor would free the list, so no TESCondition is ever constructed) and run through
        // TESCondition::IsTrue, the same verified call the whole-list fallback used. The copy is
        // memcpy'd into raw storage (no constructor) and lives for this call only.
        bool EngineItemTrue(const RE::TESConditionItem* a_item, RE::Actor* a_actor) {
            alignas(RE::TESConditionItem) std::byte buf[sizeof(RE::TESConditionItem)];
            std::memcpy(buf, a_item, sizeof(RE::TESConditionItem));
            auto* one = reinterpret_cast<RE::TESConditionItem*>(buf);
            one->next = nullptr;
            one->data.flags.isOR = false;
            struct ListHead { RE::TESConditionItem* head; } holder{ one };
            static_assert(sizeof(ListHead) == sizeof(RE::TESCondition));
            RE::TESObjectREFR* subject = a_actor;
            const auto obj = a_item->data.object.get();
            if (IsWorldProgressFn(a_item->data.functionData.function.get()) &&
                (obj == RE::CONDITIONITEMOBJECT::kSelf || obj == RE::CONDITIONITEMOBJECT::kTarget))
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) subject = pc;
            return reinterpret_cast<const RE::TESCondition*>(&holder)->IsTrue(subject, subject);
        }

        Tri EvalCondItem(const RE::CONDITION_ITEM_DATA& d, RE::Actor* a_actor,
                         RE::TESNPC* a_base, RE::ActorValueOwner* a_avo,
                         std::uint16_t a_progLevel) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            using Fn = RE::FUNCTION_DATA::FunctionID;
            const auto fn  = d.functionData.function.get();
            const float rhs = d.flags.global
                                  ? (d.comparisonValue.g ? d.comparisonValue.g->value : 0.0f)
                                  : d.comparisonValue.f;
            auto cmp = [&](float lhs) -> Tri {
                bool r = false;
                switch (d.flags.opCode) {
                case Op::kEqualTo:              r = lhs == rhs; break;
                case Op::kNotEqualTo:           r = lhs != rhs; break;
                case Op::kGreaterThan:          r = lhs >  rhs; break;
                case Op::kGreaterThanOrEqualTo: r = lhs >= rhs; break;
                case Op::kLessThan:             r = lhs <  rhs; break;
                case Op::kLessThanOrEqualTo:    r = lhs <= rhs; break;
                default: return Tri::kUnknown;
                }
                return r ? Tri::kTrue : Tri::kFalse;
            };
            if (fn == Fn::kGetBaseActorValue || fn == Fn::kGetActorValue) {
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf || !a_avo) return Tri::kUnknown;
                const auto av = static_cast<RE::ActorValue>(
                    reinterpret_cast<std::uintptr_t>(d.functionData.params[0]));
                return cmp(fn == Fn::kGetBaseActorValue ? a_avo->GetBaseActorValue(av)
                                                        : a_avo->GetActorValue(av));
            }
            if (fn == Fn::kGetLevel) {
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf) return Tri::kUnknown;
                return cmp(static_cast<float>(std::max<int>(
                    static_cast<int>(a_actor->GetLevel()), static_cast<int>(a_progLevel))));
            }
            if (fn == Fn::kHasPerk) {
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf) return Tri::kUnknown;
                auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                RE::FormID pid = form ? form->GetFormID() : 0;
                const RE::FormID rawPid = pid;
                pid = TwinOf(pid);   // "alias twins": an off-tree copy stands for its in-tree twin
                if (PerkAllocatableInCatalog(pid)) {
                    // Held if EITHER the twin or the raw perk itself is owned (a follower the load order
                    // gave the off-tree perk natively keeps passing), rank-aware for both.
                    const bool held = OwnsPerkForm(a_actor, a_base, PerkByID(pid)) ||
                                      (rawPid != pid && OwnsPerkForm(a_actor, a_base, PerkByID(rawPid)));
                    return cmp(held ? 1.0f : 0.0f);
                }
                // Not allocatable: only the "req" shape on a KNOWN-but-filtered perk is decided here.
                const bool isReqShape =
                    (d.flags.opCode == Op::kEqualTo && rhs == 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThanOrEqualTo && rhs > 0.0f && rhs <= 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThan && rhs >= 0.0f && rhs < 1.0f);
                if (!isReqShape) return Tri::kUnknown;   // exclusion / odd shape on a non-catalog perk: the engine decides
                // A KNOWN-but-filtered catalog perk can never be held by a follower and §5.1
                // bridges around it, so it is satisfied (the Destruction-Mastery case). A perk
                // in NO tree (quest/SPID-granted gate) is genuinely unknown: the engine decides.
                return PerkKnownToCatalog(pid) ? Tri::kTrue : Tri::kUnknown;
            }
            return Tri::kUnknown;
        }

        // §5.2 gate over a perk's own conditions, with correct AND/OR grouping:
        // items joined by the isOR flag form a disjunction (the group passes if
        // ANY member is true); groups are ANDed. A kUnknown item is asked of the
        // engine ALONE (EngineItemTrue) and joins the same grouping. This is what
        // lets a FILTERED HasPerk (kTrue) unlock a node whether it stands alone
        // or sits inside an OR group.
        enum class CondEval { kPass, kFail };
        CondEval EvalPerkConditions(RE::Actor* a_actor, RE::TESNPC* a_base,
                                    RE::BGSPerk* a_rank, std::uint16_t a_progLevel) {
            auto* avo = a_actor->AsActorValueOwner();
            bool result = true;
            for (auto* it = a_rank->perkConditions.head; it;) {
                bool groupVal = false;                 // OR accumulator for this group
                for (;;) {
                    Tri t = EvalCondItem(it->data, a_actor, a_base, avo, a_progLevel);
                    if (t == Tri::kUnknown) t = EngineItemTrue(it, a_actor) ? Tri::kTrue : Tri::kFalse;
                    groupVal = groupVal || (t == Tri::kTrue);
                    const bool orNext = it->data.flags.isOR;   // OR'd with the next item
                    it = it->next;
                    if (!orNext || !it) break;                 // group ends (or list ends)
                }
                result = result && groupVal;
            }
            return result ? CondEval::kPass : CondEval::kFail;
        }

        // EVALUATIVE dump of a perk's conditions — appended to the refusal on
        // ANY block, annotating each item with the follower's live value and
        // a [T]/[F] verdict, so a genuinely-locked perk names its exact
        // blocker (which skill/level/perk, and what the follower actually has)
        // instead of an opaque "perkConditions false". Diagnostic; bounded.
        std::string DumpConditions(RE::BGSPerk* a_rank, RE::Actor* a_actor,
                                   RE::TESNPC* a_base, std::uint16_t a_progLevel) {
            using Fn = RE::FUNCTION_DATA::FunctionID;
            auto* avo = a_actor->AsActorValueOwner();
            std::string out;
            int n = 0;
            for (auto* it = a_rank->perkConditions.head; it && n < 8; it = it->next, ++n) {
                const auto& d = it->data;
                const auto fn = d.functionData.function.get();
                const char* op = OpText(d.flags.opCode);
                const float v = d.flags.global
                                    ? (d.comparisonValue.g ? d.comparisonValue.g->value : 0.0f)
                                    : d.comparisonValue.f;
                if (!out.empty()) out += (d.flags.isOR ? " OR " : " AND ");
                if (fn == Fn::kGetBaseActorValue || fn == Fn::kGetActorValue) {
                    const auto av = static_cast<RE::ActorValue>(
                        reinterpret_cast<std::uintptr_t>(d.functionData.params[0]));
                    const float have = avo ? (fn == Fn::kGetBaseActorValue
                                                  ? avo->GetBaseActorValue(av)
                                                  : avo->GetActorValue(av))
                                           : 0.0f;
                    out += std::format("{} {} {:g} (have {:.0f})", AvName(av), op, v, have);
                } else if (fn == Fn::kGetLevel) {
                    const int have = std::max<int>(static_cast<int>(a_actor->GetLevel()),
                                                   static_cast<int>(a_progLevel));
                    out += std::format("GetLevel {} {:g} (have {})", op, v, have);
                } else if (fn == Fn::kHasPerk) {
                    auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                    const RE::FormID rawPid = form ? form->GetFormID() : 0;
                    const RE::FormID pid = TwinOf(rawPid);
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    out += std::format("HasPerk({}) {} {:g} [{}, {}]",
                                       nm && *nm ? nm : "?", op, v,
                                       (OwnsPerkForm(a_actor, a_base, pp) ||
                                        (rawPid != pid && OwnsPerkForm(a_actor, a_base, PerkByID(rawPid))))
                                           ? "owned" : "MISSING",
                                       PerkAllocatableInCatalog(pid) ? "in-tree" : "filtered");
                } else {
                    out += std::format("fn#{} obj{} {} {:g}", static_cast<int>(fn),
                                       static_cast<int>(d.object.get()), op, v);
                }
            }
            return out;
        }

        // ── live perk-entry settle (ClickUp 1dfcuabcrqq) ────────────────────
        // A middle-high process is where entry-point entries live (AIProcess
        // ->middleHigh +0x288: 92 BSTArrays, one per entry point). Without one
        // the engine applies nothing (Actor::ApplyPerksFromBase returns at
        // once, AE 0x715950 `cmp [rcx+8],0`), and the arrays are rebuilt from
        // the base with the process (Actor::Revert frees them at a load,
        // Actor::FinishLoadGame re-applies the base). So no process = no-op.
        bool HasMiddleHigh(RE::Actor* a_actor) {
            auto* proc = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
            return proc && proc->middleHigh;
        }

        RE::BGSEntryPointPerkEntry* AsEntryPoint(RE::BGSPerkEntry* a_entry) {
            return (a_entry && a_entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint)
                       ? static_cast<RE::BGSEntryPointPerkEntry*>(a_entry)
                       : nullptr;
        }

        std::uint32_t EntryPointOf(const RE::BGSEntryPointPerkEntry* a_ep) {
            return static_cast<std::uint32_t>(a_ep->entryData.entryPoint.get());
        }

        // Counts one entry point's live array: every entry, and the copies of
        // one entry pointer. Actor::ForEachPerkEntry (vfunc 0x100) walks
        // middleHigh+0x288[ep] and calls Visit (slot 0) while it returns
        // kContinue (AE 0x7158d0, SE 0x681f90; 1.7.104 slot 0x100 = 0x69f9c0).
        struct EntryCountVisitor final : RE::PerkEntryVisitor {
            const RE::BGSPerkEntry* want  = nullptr;
            std::uint32_t           total = 0;
            std::uint32_t           match = 0;
            RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry* a_entry) override {
                ++total;
                if (a_entry == want) ++match;
                return RE::BSContainer::ForEachResult::kContinue;
            }
        };
        EntryCountVisitor CountLive(RE::Actor* a_actor, std::uint32_t a_ep, const RE::BGSPerkEntry* a_want) {
            EntryCountVisitor v;
            v.want = a_want;
            if (a_ep < RE::BGSEntryPoint::ENTRY_POINTS::kTotal)
                a_actor->ForEachPerkEntry(static_cast<RE::BGSEntryPoint::ENTRY_POINT>(a_ep), v);
            return v;
        }

        // [perk-probe] session state: the ONE follower it watches, and its line budget.
        constexpr int           kProbeLines = 64;
        std::atomic<RE::FormID> g_probeActor{ 0 };
        std::atomic<int>        g_probeLinesLeft{ kProbeLines };

    }

        // Replaces Actor::ApplyPerksFromBase at every MFO site. That call is
        // NOT a settle: it walks the WHOLE base perk array (TESNPC+0x138) and
        // queues one BSTaskPool task 0x5F per perk with old rank 0 -> the
        // base rank (AE 0x715950 -> 0x3909f0 -> 0x656ad0; the handler
        // 0x659063 calls the rank change 0x390580 with no presence check), and
        // the entry-point insert (0x38f640 -> 0x7155e0) always inserts (binary
        // search on priority/rank, comparator 0x390540, no pointer test). So
        // every call DUPLICATED every entry-point entry of every base perk,
        // and it never removed anything. Same on SE 1.5.97 and 1.7.104.
        //
        // This is the engine's own per-perk rank change (0x390580) for ONE
        // perk, made presence-checked: an entry whose rank+1 == a_oldRank is
        // removed (every live copy, so an old duplicate cannot outlive the
        // perk), else one whose rank+1 == a_newRank is applied, an entry-point
        // entry only when its pointer is not already in its array. Ability and
        // quest entries go through ApplyPerkEntry / RemovePerkEntry as the
        // engine's path does (an ability's AddSpell does not double). The
        // engine's post-change event (0x391760 / 0x391520) is not sent: it has
        // no in-binary sink registration. MAIN THREAD.
        LivePerkChange SetLivePerkRank(RE::Actor* a_actor, RE::BGSPerk* a_perk,
                                       std::uint8_t a_oldRank, std::uint8_t a_newRank) {
            LivePerkChange out;
            if (!a_actor || !a_perk || a_oldRank == a_newRank) return out;
            if (!HasMiddleHigh(a_actor)) {
                out.noProcess = true;
                return out;
            }
            for (auto* entry : a_perk->perkEntries) {
                if (!entry) continue;
                const unsigned r  = static_cast<unsigned>(entry->header.rank) + 1u;
                auto*          ep = AsEntryPoint(entry);
                if (a_oldRank != 0 && r == a_oldRank) {
                    if (ep) {
                        const auto copies = CountLive(a_actor, EntryPointOf(ep), entry).match;
                        for (std::uint32_t i = 0; i < copies; ++i) entry->RemovePerkEntry(a_actor);
                        out.removed += static_cast<int>(copies);
                    } else {
                        entry->RemovePerkEntry(a_actor);
                        ++out.removed;
                    }
                } else if (a_newRank != 0 && r == a_newRank) {
                    if (ep && CountLive(a_actor, EntryPointOf(ep), entry).match > 0) {
                        ++out.alreadyLive;
                        continue;
                    }
                    entry->ApplyPerkEntry(a_actor);
                    ++out.applied;
                }
            }
            return out;
        }

        // The rank the base holds a_perk at (0 = not on the base).
        std::uint8_t BaseRankOf(RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            if (!a_base || !a_perk) return 0;
            const auto idx = a_base->GetPerkIndex(a_perk);
            if (!idx || *idx >= a_base->perkCount) return 0;
            return static_cast<std::uint8_t>(a_base->perks[*idx].currentRank);
        }

        // Every entry-point entry the BASE says should be live (entry
        // rank+1 == the base rank), counted in the actor's arrays. Pure read.
        PerkEntryCensus CensusBasePerkEntries(RE::Actor* a_actor, RE::TESNPC* a_base) {
            PerkEntryCensus c;
            if (!a_actor || !a_base) return c;
            c.process = HasMiddleHigh(a_actor);
            if (!c.process) return c;
            for (std::uint32_t i = 0; i < a_base->perkCount; ++i) {
                const auto& pr = a_base->perks[i];
                if (!pr.perk) continue;
                ++c.basePerks;
                const unsigned baseRank = static_cast<std::uint8_t>(pr.currentRank);
                for (auto* entry : pr.perk->perkEntries) {
                    auto* ep = AsEntryPoint(entry);
                    if (!ep || static_cast<unsigned>(entry->header.rank) + 1u != baseRank) continue;
                    ++c.epEntries;
                    const auto copies = CountLive(a_actor, EntryPointOf(ep), entry).match;
                    if (copies == 0) ++c.missing;
                    if (copies > 1) {
                        ++c.duplicated;
                        c.extraCopies += static_cast<int>(copies - 1);
                        if (static_cast<int>(copies) > c.maxCopies) {
                            c.maxCopies = static_cast<int>(copies);
                            c.worstPerk = pr.perk->GetFormID();
                            c.worstEp   = EntryPointOf(ep);
                        }
                    }
                }
            }
            return c;
        }

#ifdef MFO_DEV_SELFTEST
    // Dev builds only (ProgAllocator.h): the self-test read. Same census the probe prints.
    bool ReadPerkEntryView(RE::FormID a_id, PerkEntryView& a_out) {
        a_out = {};
        const auto it = g_prog.find(a_id);
        if (it == g_prog.end()) return false;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
        auto* base  = actor ? actor->GetActorBase() : nullptr;
        if (!base) return false;
        a_out.enrolled       = it->second.enrolled;
        a_out.allocatedRanks = AllocatedRanks(it->second);
        const auto c         = CensusBasePerkEntries(actor, base);
        a_out.process        = c.process;
        a_out.basePerks      = c.basePerks;
        a_out.epEntries      = c.epEntries;
        a_out.missing        = c.missing;
        a_out.duplicated     = c.duplicated;
        a_out.extraCopies    = c.extraCopies;
        a_out.maxCopies      = c.maxCopies;
        a_out.worstPerk      = c.worstPerk;
        a_out.worstEp        = c.worstEp;
        return true;
    }
#endif

        // ── [perk-probe] PASSIVE probe (bPerkEntryProbe, INI-only, default OFF) ──
        // Reads only. Watches the FIRST follower that reaches a perk mutation
        // site this session; at most kProbeLines lines per session.
        PerkEntryProbe ProbeBefore(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            PerkEntryProbe p;
            if (!Config::g_perkEntryProbe.load(std::memory_order_relaxed) || !a_actor || !a_base) return p;
            if (g_probeLinesLeft.load(std::memory_order_relaxed) <= 0) return p;
            RE::FormID       expect = 0;
            const RE::FormID me     = a_actor->GetFormID();
            if (!g_probeActor.compare_exchange_strong(expect, me) && expect != me) return p;
            p.on    = true;
            p.actor = me;
            // The entry point to watch: the mutated perk's first entry-point
            // entry, else the first one of any base perk.
            auto pick = [&p](RE::BGSPerk* a_from) {
                if (!a_from || p.watch) return;
                for (auto* e : a_from->perkEntries)
                    if (auto* ep = AsEntryPoint(e)) {
                        p.watch = e;
                        p.perk  = a_from;
                        p.ep    = EntryPointOf(ep);
                        return;
                    }
            };
            pick(a_perk);
            for (std::uint32_t i = 0; i < a_base->perkCount && !p.watch; ++i) pick(a_base->perks[i].perk);
            if (p.watch) {
                const auto v    = CountLive(a_actor, p.ep, p.watch);
                p.epTotalBefore = v.total;
                p.copiesBefore  = v.match;
            }
            p.before = CensusBasePerkEntries(a_actor, a_base);
            return p;
        }

        void ProbeAfter(const PerkEntryProbe& a_p, RE::Actor* a_actor, RE::TESNPC* a_base, const char* a_site) {
            if (!a_p.on || !a_actor || !a_base) return;
            if (g_probeLinesLeft.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
            const auto    after = CensusBasePerkEntries(a_actor, a_base);
            std::uint32_t total = 0, copies = 0;
            if (a_p.watch) {
                const auto v = CountLive(a_actor, a_p.ep, a_p.watch);
                total        = v.total;
                copies       = v.match;
            }
            spdlog::info("[perk-probe] {} {:08X} '{}': ep {} entries {}->{}, perk '{}' ({:08X}) copies {}->{} | "
                         "base census: duplicated {}->{} (extra copies {}->{}, max {}x{}), missing {}->{}, "
                         "{} entry-point entries on {} base perks{}",
                         a_site, a_p.actor, NameOf(a_actor), a_p.ep, a_p.epTotalBefore, total,
                         a_p.perk ? NameOf(a_p.perk) : "-", a_p.perk ? a_p.perk->GetFormID() : 0u,
                         a_p.copiesBefore, copies, a_p.before.duplicated, after.duplicated,
                         a_p.before.extraCopies, after.extraCopies, after.maxCopies,
                         after.worstPerk ? std::format(" ({:08X} ep {})", after.worstPerk, after.worstEp)
                                         : std::string(),
                         a_p.before.missing, after.missing, after.epEntries, after.basePerks,
                         after.process ? "" : " [no middle-high process: nothing is live]");
        }

        // §17: how many CATALOG-TREE perk ranks the follower already owns —
        // captured ONCE at enrollment as the budget debit. Per node, the
        // highest rank form present counts that many ranks (rank K implies
        // 1..K, each of which would have cost a point). Perks outside the
        // trees (racial, quest, passives) are invisible here on purpose.
        int CountNativeTreeRanks(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st) {
            int total = 0;
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
                    // A native vanilla Smithing perk is kept and free (no debit). A rank MFO itself
                    // allocated on such a node still counts: AllocatedRanks subtracts it below.
                    if (tree.av == RE::ActorValue::kSmithing && node.vanillaGate) {
                        auto* al = FindAlloc(a_st, node.perkFormID);
                        if (al && al->rank >= 1 && al->rank <= node.ranks.size()) {
                            auto* mine = PerkByID(node.ranks[al->rank - 1].perkFormID);
                            if (mine && (a_base->GetPerkIndex(mine).has_value() || a_actor->HasPerk(mine)))
                                total += al->rank;
                        }
                        continue;
                    }
                    for (int r = static_cast<int>(node.ranks.size()); r >= 1; --r) {
                        auto* perk = PerkByID(node.ranks[static_cast<std::size_t>(r - 1)].perkFormID);
                        if (perk && (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk))) {
                            total += r;
                            break;
                        }
                    }
                }
            }
            return total;
        }

        // ── B′ NATIVE-PERK STRIP (marth 2026-09-13): every CATALOG rank the
        // base holds that MFO did not grant is removed + recorded (a set).
        // Non-catalog untouched; an ACTOR-only rank is logged + left. MAIN THREAD.
        int StripNativePerks(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st) {
            int stripped = 0;
            const auto probe = ProbeBefore(a_actor, a_base, nullptr);
            RestoreStrippedVanillaSmithing(a_actor, a_base, a_st);   // fix-forward for saves stripped by an earlier build
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
                    if (tree.av == RE::ActorValue::kSmithing && node.vanillaGate) continue;   // native vanilla Smithing perks are kept
                    auto* alloc = FindAlloc(a_st, node.perkFormID);
                    const RE::FormID mfoForm = (alloc && alloc->rank >= 1 && alloc->rank <= node.ranks.size())
                        ? node.ranks[alloc->rank - 1].perkFormID : 0;
                    for (const auto& rank : node.ranks) {
                        auto* perk = PerkByID(rank.perkFormID);
                        if (!perk || rank.perkFormID == mfoForm) continue;   // MFO's own grant stays
                        if (!a_base->GetPerkIndex(perk).has_value()) {
                            if (a_actor->HasPerk(perk))
                                spdlog::info("[prog] {:08X}: native '{}' ({:08X}) is actor-held, not on the base "
                                             "-- cannot strip, left in place", a_actor->GetFormID(),
                                             NameOf(perk), rank.perkFormID);
                            continue;
                        }
                        const std::uint8_t had = BaseRankOf(a_base, perk);
                        a_base->RemovePerk(perk);
                        SetLivePerkRank(a_actor, perk, had, 0);   // its live entries go with it
                        if (std::find(a_st.strippedPerks.begin(), a_st.strippedPerks.end(), rank.perkFormID)
                                == a_st.strippedPerks.end() && a_st.strippedPerks.size() < kMaxPerkAllocs)
                            a_st.strippedPerks.push_back(rank.perkFormID);
                        ++stripped;
                        spdlog::info("[prog] {:08X}: STRIP native {} perk '{}' ({:08X}) -- list-given, "
                                     "now the player's to give", a_actor->GetFormID(), tree.skillName,
                                     NameOf(perk), rank.perkFormID);
                    }
                }
            }
            if (stripped) ProbeAfter(probe, a_actor, a_base, "strip");
            a_st.nativeHeld = true;
            // §17 debit = tree ranks STILL natively held (unstrippable), never MFO's.
            a_st.nativeTreePerksAtEnroll = static_cast<std::uint16_t>(
                std::clamp(CountNativeTreeRanks(a_actor, a_base, a_st) - AllocatedRanks(a_st), 0, 0xFFFF));
            return stripped;
        }
        // The safe-removal restore (public RestoreNativePerks): every recorded
        // native rank back on the base, record cleared. Called by the roster
        // removal (ForgetFollower, Verbs.cpp).
        int RestoreNativePerksImpl(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st) {
            int restored = 0;
            const auto probe = ProbeBefore(a_actor, a_base, nullptr);
            for (const auto fid : a_st.strippedPerks) {
                auto* perk = PerkByID(fid);
                if (!perk || a_base->GetPerkIndex(perk).has_value()) continue;
                a_base->AddPerk(perk, 1);
                SetLivePerkRank(a_actor, perk, 0, 1);
                ++restored;
                spdlog::info("[prog] {:08X}: RESTORE native perk '{}' ({:08X}) -- uninstall",
                             a_actor->GetFormID(), NameOf(perk), fid);
            }
            if (restored) ProbeAfter(probe, a_actor, a_base, "restore");
            a_st.strippedPerks.clear();
            a_st.nativeHeld = false;
            return restored;
        }

        // The §5 double gate. Returns the 1-based rank this follower may take
        // NEXT on this node, or 0 with a_whyNot filled. Pure read — callers
        // decide whether the refusal is worth a log line (AllocatePerk always
        // logs; the auto-picker probes many nodes quietly).
        int GateNextRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         ProgState& a_st, const Progression::PerkNodeView& a_node,
                         std::string& a_whyNot) {
            auto* alloc = FindAlloc(a_st, a_node.perkFormID);
            const int have = alloc ? alloc->rank : 0;

            // v1: never build on NATIVE ownership. If the load order (Requiem
            // stats, SPID distributions) already gave the follower this perk,
            // MFO neither stacks on it nor ever removes it at respec — the
            // clean ownership boundary for the unique-base v1.
            if (have == 0 && OwnsAnyRank(a_actor, a_base, a_st, a_node.perkFormID)) {
                a_whyNot = "owned natively (granted by the load order, not MFO)";
                return 0;
            }
            if (have >= static_cast<int>(a_node.ranks.size())) {
                a_whyNot = std::format("already at max rank {}", have);
                return 0;
            }
            // The L1 companion: an upgrade (have > 0) requires MFO's OWN rank
            // form to actually be on the base. When reapply deferred to a
            // natively-appeared rank, granting rank N+1 would RemovePerk a
            // form MFO never placed and stack entries on the native one —
            // the node is frozen until the native grant goes away or respec
            // clears the alloc.
            if (have > 0) {
                auto* ours = PerkByID(a_node.ranks[static_cast<std::size_t>(have - 1)].perkFormID);
                if (!ours || !a_base->GetPerkIndex(ours).has_value()) {
                    // ASCII only -- renders in the board's locked tooltip.
                    a_whyNot = "MFO's rank is not on the base (native rank owns this node) -- frozen";
                    return 0;
                }
            }
            const auto& rank = a_node.ranks[static_cast<std::size_t>(have)];
            if (rank.verdict == Progression::Verdict::kDead) {
                a_whyNot = std::format("rank {} is not NPC-effective", have + 1);
                return 0;
            }
            // Crafting stream: a crafting-skill rank may use a craft point OR a general
            // point (overflow); any other rank needs a general point.
            const auto pools = PerkPoolsOf(a_st);
            const auto nodeRef = FindNode(a_node.perkFormID);
            const bool craftNode = nodeRef.tree && IsCraftSkill(nodeRef.tree->av);
            if (pools.general < 1 && !(craftNode && pools.craft >= 1)) {
                // MFO-B418: per pool. General: earned = floor(level/N), spent = ranks it paid for
                // (all ranks minus the ones the craft pool paid). Craft: only when the stream is on.
                const int lvl         = static_cast<int>(a_st.progressionLevel);
                const int earnedGen   = lvl / std::max(1, g_econ.levelsPerPerkPoint);
                const int craftEarned = g_econ.levelsPerCraftPerkPoint > 0 ? lvl / g_econ.levelsPerCraftPerkPoint : 0;
                const int craftSpent  = craftEarned > 0 ? std::min(CraftTreeRanks(a_st), craftEarned) : 0;
                const int genSpent    = AllocatedRanks(a_st) - craftSpent;
                if (craftEarned > 0)
                    a_whyNot = std::format("no perk points (general: earned {}, {} spent; craft: earned {}, {} spent)",
                                           earnedGen, genSpent, craftEarned, craftSpent);
                else
                    a_whyNot = std::format("no perk points (earned {}, {} spent)", earnedGen, genSpent);
                return 0;
            }
            // §5.1 prereq: reachable iff root-reachable (empty parentPerkIDs
            // — includes lines that BRIDGE to the root through filtered
            // nodes, round 4) OR at least one parent perk owned at rank ≥1.
            // parentPerkIDs holds the bridged KEPT ancestors — allocatable
            // prereqs, the vanilla any-line rule. The perk's own conditions
            // (§5.2 below) stay the final authority on top.
            // A vanilla Smithing node (progression/VanillaSmithing.cpp) gates here on the vanilla
            // skill + prerequisite instead, and skips the list's perkConditions at §5.2.
            if (a_node.vanillaGate && !VanillaGateOk(a_actor, a_base, a_st, a_node, a_whyNot)) return 0;
            if (have == 0 && !a_node.vanillaGate && !a_node.parentPerkIDs.empty()) {
                bool anyParent = false;
                for (const auto pid : a_node.parentPerkIDs)
                    if (OwnsAnyRank(a_actor, a_base, a_st, pid)) { anyParent = true; break; }
                if (!anyParent) {
                    a_whyNot = std::format("prerequisite not met ({} parent perk(s), none owned)",
                                           a_node.parentPerkIDs.size());
                    return 0;
                }
            }
            // §5.2 the perk's own conditions, evaluated ON THE FOLLOWER — the
            // same record the player's skill menu gates on, so skill-level and
            // any overhaul-added condition enforce player-identically.
            auto* rankForm = PerkByID(rank.perkFormID);
            if (!rankForm) {
                a_whyNot = std::format("rank form {:08X} did not resolve", rank.perkFormID);
                return 0;
            }
            const CondEval ce = a_node.vanillaGate ? CondEval::kPass
                                    : EvalPerkConditions(a_actor, a_base, rankForm, a_st.progressionLevel);
            const bool condPass = (ce == CondEval::kPass);
            if (!condPass) {
                // Name the real blocker. In the §5.2 fail path it is usually a
                // still-allocatable, unowned condition-prereq — say so — before
                // falling back to the skill string with the follower's value.
                std::string detail;
                for (const auto rawPid : a_node.condPrereqPerkIDs) {
                    const auto pid = TwinOf(rawPid);
                    if (!PerkAllocatableInCatalog(pid)) continue;   // filtered — not the blocker
                    if (OwnsAnyRank(a_actor, a_base, a_st, pid))    continue;   // owned — not it
                    if (rawPid != pid && OwnsAnyRank(a_actor, a_base, a_st, rawPid)) continue;   // raw twin owned natively
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    detail = std::format(" (needs perk: {})", nm && *nm ? nm : "prerequisite");
                    break;
                }
                if (detail.empty() && !rank.skillReq.empty()) {
                    if (rank.skillReqAV != RE::ActorValue::kNone) {
                        if (auto* avo = a_actor->AsActorValueOwner())
                            detail = std::format(" (needs {}, have {:.0f})", rank.skillReq,
                                                 avo->GetBaseActorValue(rank.skillReqAV));
                        else
                            detail = std::format(" (needs {})", rank.skillReq);
                    } else {
                        detail = std::format(" (needs {})", rank.skillReq);
                    }
                }
                detail += std::format(" [cond: {}]",
                                      DumpConditions(rankForm, a_actor, a_base, a_st.progressionLevel));
                a_whyNot = std::format("perkConditions false on follower{}", detail);
                return 0;
            }
            return have + 1;
        }

        // The mutation: rank K replaces rank K−1 on the BASE (the player's
        // own single-highest-rank shape — two ranks of one perk both present
        // would double-apply their entries), then SetLivePerkRank settles the
        // actor for those two perks only, the way the engine's own rank change
        // does (rank K-1's entries out, rank K's in). Main thread only.
        bool GrantRank(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st,
                       const Progression::PerkNodeView& a_node, int a_targetRank) {
            const auto& rank = a_node.ranks[static_cast<std::size_t>(a_targetRank - 1)];
            auto* target = PerkByID(rank.perkFormID);
            if (!target) {
                spdlog::error("[prog] grant aborted: rank form {:08X} unresolvable", rank.perkFormID);
                return false;
            }
            const auto probe = ProbeBefore(a_actor, a_base, target);
            if (a_targetRank > 1) {
                if (auto* prev = PerkByID(a_node.ranks[static_cast<std::size_t>(a_targetRank - 2)].perkFormID)) {
                    const std::uint8_t had = BaseRankOf(a_base, prev);
                    a_base->RemovePerk(prev);
                    if (had) SetLivePerkRank(a_actor, prev, had, 0);
                }
            }
            a_base->AddPerk(target, 1);
            const auto live = SetLivePerkRank(a_actor, target, 0, 1);
            ProbeAfter(probe, a_actor, a_base, "grant");
            if (live.alreadyLive)
                spdlog::info("[prog] {:08X} grant: {} entry-point entr{} of {:08X} already live, not re-applied",
                             a_actor->GetFormID(), live.alreadyLive, live.alreadyLive == 1 ? "y" : "ies",
                             rank.perkFormID);

            auto* alloc = FindAlloc(a_st, a_node.perkFormID);
            if (!alloc) {
                a_st.perks.push_back({ a_node.perkFormID, 0 });
                alloc = &a_st.perks.back();
            }
            alloc->rank = static_cast<std::uint8_t>(a_targetRank);
            // §17: no decrement — the pool is derived, and the rank we just
            // recorded is the debit.

            spdlog::info("[prog] {:08X} GRANTED {} rank {}/{} ({:08X}) — {} point(s) left",
                         a_actor->GetFormID(), a_node.name, a_targetRank,
                         a_node.ranks.size(), rank.perkFormID, PerkPointsAvailable(a_st));
            return true;
        }

        // ── the guarded session reapply (P3) ────────────────────────────────
        // Base-form perk mutations are runtime-only; the co-save alloc is the
        // truth, reasserted here: AddPerk only when the base lacks the target
        // rank form, and then SetLivePerkRank for THAT perk only (presence-
        // checked). The engine already applied the rest of the base at the
        // load (Actor::FinishLoadGame); the old ApplyPerksFromBase here put a
        // second copy of every base entry-point entry on the actor each load. Skills re-reconcile through the same §4.2 path.
        void ReapplyFollower(RE::Actor* a_actor, ProgState& a_st) {
            auto* base = a_actor->GetActorBase();
            if (!base) return;

            int reAdded = 0, present = 0, dropped = 0, nativeDeferred = 0, noProcess = 0;
            const auto probe = ProbeBefore(a_actor, base, nullptr);
            for (const auto& alloc : a_st.perks) {
                auto ref = FindNode(alloc.nodePerkID);
                if (!ref.node || alloc.rank == 0 ||
                    alloc.rank > static_cast<std::uint8_t>(ref.node->ranks.size())) {
                    ++dropped;   // catalog changed under the save — counted, left for load-time validation
                    continue;
                }
                auto* target = PerkByID(ref.node->ranks[alloc.rank - 1].perkFormID);
                if (!target) { ++dropped; continue; }
                if (base->GetPerkIndex(target).has_value()) { ++present; continue; }
                // ANOTHER rank of this node on the base at reapply time is
                // NATIVE ownership: base perk mutations do not survive a load
                // (P3), so at session start the array holds only what the
                // load order authored — and mid-session MFO's own grants keep
                // exactly the target rank present (GrantRank swaps at
                // upgrade). Removing it here would strip a perk MFO never
                // granted (SPID/Requiem gave it between sessions) — the L1
                // violation. Stacking ours on top would double entries. So:
                // touch NOTHING, defer with a named line. The alloc is kept —
                // respec later refunds it without removing the native form
                // (RemovePerk is GetPerkIndex-guarded on OUR rank form only).
                bool nativeRank = false;
                for (const auto& r : ref.node->ranks) {
                    if (r.perkFormID == target->GetFormID()) continue;
                    if (auto* sib = PerkByID(r.perkFormID); sib && base->GetPerkIndex(sib).has_value()) {
                        nativeRank = true;
                        break;
                    }
                }
                if (nativeRank) {
                    ++nativeDeferred;
                    spdlog::info("[prog] {:08X} reapply: {} rank {} NOT re-added — the load order "
                                 "now grants another rank of this node natively; MFO never removes "
                                 "or stacks on a rank it did not grant",
                                 a_actor->GetFormID(), ref.node->name, alloc.rank);
                    continue;
                }
                base->AddPerk(target, 1);
                if (SetLivePerkRank(a_actor, target, 0, 1).noProcess) ++noProcess;
                ++reAdded;
            }
            ProbeAfter(probe, a_actor, base, "reapply");

            RecomputeSkills(a_actor, a_st, /*log*/ false);
            RecomputeHMS(a_actor, a_st, /*log*/ false);   // §HMS: capture baseline / hold target

            spdlog::info("[prog] {:08X} {} reapply: {} perk(s) re-added, {} already on base, "
                         "{} unresolvable, {} deferred to native ownership — live entries applied per "
                         "perk{}",
                         a_actor->GetFormID(), NameOf(a_actor), reAdded, present, dropped, nativeDeferred,
                         noProcess ? std::format(" ({} with no middle-high process: nothing live to apply to; the "
                                                 "engine rebuilds from the base at its next Load3D)", noProcess)
                                   : std::string());
            a_st.applied = true;
            PublishReapplyPending();   // EQUIP-SELF-VIOLATIONS: the equip declaration may now build from his perks
        }

}
