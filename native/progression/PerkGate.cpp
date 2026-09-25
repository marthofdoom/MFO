// progression/PerkGate.cpp -- PERKS: the grant plumbing (P1/P3-proven), the
// perk-condition evaluator, the B-prime native-perk strip / restore, the §5
// double gate (GateNextRank), the rank grant and the session reapply.
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
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

        // §5.2b (deck 2026-08-15): a skill perk's own conditions normally
        // reduce to a pure AND-chain of the skill-level gate
        // (GetBaseActorValue-on-self >= N) plus, in some overhauls, HasPerk
        // <prereq> items that encode a prerequisite as a condition. When such a
        // prereq perk has been FILTERED out of the tree, the engine's
        // whole-chain IsTrue locks the node forever even though the skill is
        // met and §5.1's bridging already routed the prerequisite around the
        // filtered perk. We evaluate that common shape ourselves:
        //   - GetBaseActorValue-on-self, op >= / >  : real skill gate, enforced.
        //   - HasPerk <P>, "req" shape (==1 / >=1 / >0): if P is board-
        //     allocatable it is a real in-tree prereq → enforce ownership; if
        //     P is filtered/absent → treat as satisfied (§5.1 owns it).
        // Anything else — an OR group, an exclusion HasPerk (==0), GetLevel, a
        // quest/faction/global condition, an unhandled opcode — returns
        // kFallback and the caller runs the engine evaluator UNCHANGED, so no
        // exotic perk's gating is ever relaxed by guesswork.
        // One condition item's truth for THIS follower, tri-stated:
        //   kTrue/kFalse — we evaluated it; kUnknown — a shape we won't guess
        //   (the caller then defers the whole perk to the engine).
        // Handled: GetBaseActorValue/GetActorValue-on-self (>= / >), GetLevel-
        // on-self counting MFO's progression levels, and HasPerk "req" shape.
        // A HasPerk on a FILTERED/absent perk is kTrue — the follower can never
        // hold it, §5.1's bridging already owns that prerequisite, so it must
        // not lock the node (the Force-of-Nature / Destruction-Mastery case).
        enum class Tri { kTrue, kFalse, kUnknown };
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
                switch (d.flags.opCode) {
                case Op::kGreaterThan:          return lhs >  rhs ? Tri::kTrue : Tri::kFalse;
                case Op::kGreaterThanOrEqualTo: return lhs >= rhs ? Tri::kTrue : Tri::kFalse;
                default: return Tri::kUnknown;   // <, ==, != — unusual on a gate, defer
                }
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
                const bool isReqShape =
                    (d.flags.opCode == Op::kEqualTo && rhs == 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThanOrEqualTo && rhs > 0.0f && rhs <= 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThan && rhs >= 0.0f && rhs < 1.0f);
                if (!isReqShape) return Tri::kUnknown;   // exclusion (==0) / odd shape — defer
                auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                const RE::FormID pid = form ? form->GetFormID() : 0;
                if (!PerkAllocatableInCatalog(pid)) {
                    // A KNOWN-but-filtered catalog perk can never be held by a
                    // follower and §5.1 bridges around it → treat as satisfied
                    // (the Destruction-Mastery case). But a perk in NO tree at
                    // all (quest/SPID-granted gate) is genuinely unknown — don't
                    // grant it; defer to the engine's IsTrue.
                    return PerkKnownToCatalog(pid) ? Tri::kTrue : Tri::kUnknown;
                }
                return OwnsPerkForm(a_actor, a_base, PerkByID(pid)) ? Tri::kTrue : Tri::kFalse;
            }
            return Tri::kUnknown;   // GetLevel-on-target / quest / faction / … — don't guess
        }

        // §5.2 gate over a perk's own conditions, with correct AND/OR grouping:
        // items joined by the isOR flag form a disjunction (the group passes if
        // ANY member is true); groups are ANDed. Any kUnknown item makes the
        // whole perk defer (kFallback) so the engine — never guesswork — decides
        // exotic gating. This is what lets a FILTERED HasPerk (kTrue) unlock a
        // node whether it stands alone or sits inside an OR group.
        enum class CondEval { kPass, kFail, kFallback };
        CondEval EvalPerkConditions(RE::Actor* a_actor, RE::TESNPC* a_base,
                                    RE::BGSPerk* a_rank, std::uint16_t a_progLevel) {
            auto* avo = a_actor->AsActorValueOwner();
            bool result = true;
            for (auto* it = a_rank->perkConditions.head; it;) {
                bool groupVal = false;                 // OR accumulator for this group
                for (;;) {
                    const Tri t = EvalCondItem(it->data, a_actor, a_base, avo, a_progLevel);
                    if (t == Tri::kUnknown) return CondEval::kFallback;
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
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            auto* avo = a_actor->AsActorValueOwner();
            std::string out;
            int n = 0;
            for (auto* it = a_rank->perkConditions.head; it && n < 8; it = it->next, ++n) {
                const auto& d = it->data;
                const auto fn = d.functionData.function.get();
                const char* op =
                    d.flags.opCode == Op::kEqualTo             ? "==" :
                    d.flags.opCode == Op::kGreaterThan         ? ">"  :
                    d.flags.opCode == Op::kGreaterThanOrEqualTo ? ">=" : "op?";
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
                    const RE::FormID pid = form ? form->GetFormID() : 0;
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    out += std::format("HasPerk({}) {} {:g} [{}, {}]",
                                       nm && *nm ? nm : "?", op, v,
                                       OwnsPerkForm(a_actor, a_base, pp) ? "owned" : "MISSING",
                                       PerkAllocatableInCatalog(pid) ? "in-tree" : "filtered");
                } else {
                    out += std::format("fn#{} obj{} {} {:g}", static_cast<int>(fn),
                                       static_cast<int>(d.object.get()), op, v);
                }
            }
            return out;
        }

    }

        // §17: how many CATALOG-TREE perk ranks the follower already owns —
        // captured ONCE at enrollment as the budget debit. Per node, the
        // highest rank form present counts that many ranks (rank K implies
        // 1..K, each of which would have cost a point). Perks outside the
        // trees (racial, quest, passives) are invisible here on purpose.
        int CountNativeTreeRanks(RE::Actor* a_actor, RE::TESNPC* a_base) {
            int total = 0;
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
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
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
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
                        a_base->RemovePerk(perk);
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
            if (stripped) a_actor->ApplyPerksFromBase();
            a_st.nativeHeld = true;
            // §17 debit = tree ranks STILL natively held (unstrippable), never MFO's.
            a_st.nativeTreePerksAtEnroll = static_cast<std::uint16_t>(
                std::clamp(CountNativeTreeRanks(a_actor, a_base) - AllocatedRanks(a_st), 0, 0xFFFF));
            return stripped;
        }
        // The safe-removal restore (public RestoreNativePerks): every recorded
        // native rank back on the base, record cleared. Uncalled today.
        int RestoreNativePerksImpl(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st) {
            int restored = 0;
            for (const auto fid : a_st.strippedPerks) {
                auto* perk = PerkByID(fid);
                if (!perk || a_base->GetPerkIndex(perk).has_value()) continue;
                a_base->AddPerk(perk, 1);
                ++restored;
                spdlog::info("[prog] {:08X}: RESTORE native perk '{}' ({:08X}) -- uninstall",
                             a_actor->GetFormID(), NameOf(perk), fid);
            }
            if (restored) a_actor->ApplyPerksFromBase();
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
            if (PerkPointsAvailable(a_st) < 1) {
                a_whyNot = std::format("no perk points (earned {}, {} spent)",
                                       static_cast<int>(a_st.progressionLevel) /
                                           std::max(1, g_econ.levelsPerPerkPoint),
                                       AllocatedRanks(a_st));
                return 0;
            }
            // §5.1 prereq: reachable iff root-reachable (empty parentPerkIDs
            // — includes lines that BRIDGE to the root through filtered
            // nodes, round 4) OR at least one parent perk owned at rank ≥1.
            // parentPerkIDs holds the bridged KEPT ancestors — allocatable
            // prereqs, the vanilla any-line rule. The perk's own conditions
            // (§5.2 below) stay the final authority on top.
            if (have == 0 && !a_node.parentPerkIDs.empty()) {
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
            const CondEval ce = EvalPerkConditions(a_actor, a_base, rankForm, a_st.progressionLevel);
            const bool condPass = (ce == CondEval::kFallback)
                                      ? rankForm->perkConditions.IsTrue(a_actor, a_actor)
                                      : (ce == CondEval::kPass);
            if (!condPass) {
                // Name the real blocker. In the §5.2 fail path it is usually a
                // still-allocatable, unowned condition-prereq — say so — before
                // falling back to the skill string with the follower's value.
                std::string detail;
                for (const auto pid : a_node.condPrereqPerkIDs) {
                    if (!PerkAllocatableInCatalog(pid)) continue;   // filtered — not the blocker
                    if (OwnsAnyRank(a_actor, a_base, a_st, pid))    continue;   // owned — not it
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
        // would double-apply their entries), then ApplyPerksFromBase settles
        // the actor. Main thread only.
        bool GrantRank(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st,
                       const Progression::PerkNodeView& a_node, int a_targetRank) {
            const auto& rank = a_node.ranks[static_cast<std::size_t>(a_targetRank - 1)];
            auto* target = PerkByID(rank.perkFormID);
            if (!target) {
                spdlog::error("[prog] grant aborted: rank form {:08X} unresolvable", rank.perkFormID);
                return false;
            }
            if (a_targetRank > 1) {
                if (auto* prev = PerkByID(a_node.ranks[static_cast<std::size_t>(a_targetRank - 2)].perkFormID))
                    a_base->RemovePerk(prev);
            }
            a_base->AddPerk(target, 1);
            a_actor->ApplyPerksFromBase();

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
        // rank form, ApplyPerksFromBase only on change (no doubling — P3's
        // measured verdict). Skills re-reconcile through the same §4.2 path.
        void ReapplyFollower(RE::Actor* a_actor, ProgState& a_st) {
            auto* base = a_actor->GetActorBase();
            if (!base) return;

            int reAdded = 0, present = 0, dropped = 0, nativeDeferred = 0;
            bool changed = false;
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
                ++reAdded;
                changed = true;
            }
            if (changed) a_actor->ApplyPerksFromBase();

            RecomputeSkills(a_actor, a_st, /*log*/ false);
            RecomputeHMS(a_actor, a_st, /*log*/ false);   // §HMS: capture baseline / hold target

            spdlog::info("[prog] {:08X} {} reapply: {} perk(s) re-added, {} already on base, "
                         "{} unresolvable, {} deferred to native ownership — ApplyPerksFromBase {}",
                         a_actor->GetFormID(), NameOf(a_actor), reAdded, present, dropped,
                         nativeDeferred, changed ? "called" : "SKIPPED (no change)");
            a_st.applied = true;
        }

}
