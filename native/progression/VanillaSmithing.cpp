// progression/VanillaSmithing.cpp -- the VANILLA Smithing tree for followers, on every list
// (marth 2026-10-28: "use vanilla smithing for followers, the whole vanilla tree", "Vanilla
// tree and rules for every list", and "Keep native book perks").
//
// Why: Requiem ("Requiem Smithing Books Give Perks.esp") overrides the vanilla Smithing perk
// records (Craftsmanship is vanilla Steel Smithing 000CB40D) with a never-true sentinel
// (GetLevel >= 1 AND GetLevel < 0) plus its own skill thresholds, and grants them only by
// reading a book. A follower cannot read a book, so the list's own conditions lock the
// perks forever. For followers the board therefore uses the Skyrim.esm tree: the same perk
// forms, the vanilla prerequisite edges and the vanilla skill thresholds. The list's
// conditions on these perks are not evaluated for a follower. The perk EFFECTS stay the
// winning record's, so recipes that test HasPerk keep working.
//
// The table is base-game data (Skyrim.esm), constant across lists, never keyed on a list or a
// plugin. Every row was read out of Skyrim.esm with tools/esp_inspect.py: the perk-level
// conditions of each PERK record (GetBaseActorValue Smithing >= N, HasPerk prerequisites)
// and the AVSmithing (00000450) tree edges. Skyrim.esm is load index 00, so the FormID is the
// raw id.
//
// Three hooks: Progression.cpp BuildCatalog calls ApplyVanillaSmithing (load time, before the
// catalog freezes), PerkGate.cpp GateNextRank calls VanillaGateOk for a flagged node,
// StripNativePerks skips the exempt perks and runs the fix-forward sweep.
#include "PCH.h"
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"
#include "Progression.h"
#include "i18n/Strings.h"

namespace MFO::Progression {

    namespace {
        struct VanillaPerk {
            RE::FormID perk;
            float      level;        // GetBaseActorValue(Smithing) >= level (0 = none)
            RE::FormID parents[2];   // HasPerk prerequisites, ANY one of them satisfies (0 = unused)
        };
        constexpr VanillaPerk kVanilla[] = {
            { 0x000CB40D,   0, { 0,          0          } },   // Steel Smithing (Craftsmanship)
            { 0x000CB40E,  30, { 0x000CB40D, 0          } },   // Dwarven Smithing
            { 0x000CB410,  50, { 0x000CB40E, 0          } },   // Orcish Smithing
            { 0x000CB412,  80, { 0x000CB410, 0          } },   // Ebony Smithing
            { 0x000CB413,  90, { 0x000CB412, 0          } },   // Daedric Smithing
            { 0x000CB40F,  30, { 0x000CB40D, 0          } },   // Elven Smithing
            { 0x000CB414,  50, { 0x000CB40F, 0          } },   // Advanced Armors
            { 0x000CB411,  70, { 0x000CB414, 0          } },   // Glass Smithing
            { 0x0005218E,  60, { 0x000CB40D, 0          } },   // Arcane Blacksmith
            { 0x00052190, 100, { 0x000CB413, 0x000CB411 } },   // Dragon Armor (Daedric OR Glass)
        };
        const VanillaPerk* Find(RE::FormID a_id) {
            for (const auto& v : kVanilla)
                if (v.perk == a_id) return &v;
            return nullptr;
        }
    }

    bool IsVanillaSmithingPerk(RE::FormID a_perk) { return Find(a_perk) != nullptr; }

    void ApplyVanillaSmithing(SkillTree& a_tree) {
        int applied = 0;
        std::string notes;
        for (auto& node : a_tree.nodes) {
            const VanillaPerk* v = Find(node.perkFormID);
            if (!v) {
                // Not in Skyrim.esm: the list ADDED this perk, so it is not part of the vanilla tree.
                // Left exactly as built (the list's own edges and conditions still rule it).
                notes += std::format(" +list:{}({:08X})", node.name, node.perkFormID);
                continue;
            }
            if (node.ranks.size() != 1) {
                // The list gave a vanilla perk extra ranks: not the vanilla shape, leave it alone.
                notes += std::format(" !multirank:{}({:08X})", node.name, node.perkFormID);
                continue;
            }
            node.parentIndices.clear();
            node.parentPerkIDs.clear();
            node.condPrereqPerkIDs.clear();
            for (const auto pid : v->parents) {
                if (!pid) continue;
                for (std::size_t j = 0; j < a_tree.nodes.size(); ++j) {
                    if (a_tree.nodes[j].perkFormID != pid) continue;
                    node.parentIndices.push_back(static_cast<std::uint32_t>(j));
                    node.parentPerkIDs.push_back(pid);
                    node.condPrereqPerkIDs.push_back(pid);
                    break;
                }
                // A vanilla parent absent from the catalog (filtered) cannot be held by a follower:
                // it is simply not required, the same bridging rule the catalog applies everywhere.
            }
            auto& rank = node.ranks[0];
            if (v->level > 0.0f) {
                rank.skillReqAV  = a_tree.av;
                rank.skillReqVal = v->level;
                rank.skillReq    = std::format("{} {:g}", a_tree.skillName, v->level);
            } else {
                rank.skillReqAV  = RE::ActorValue::kNone;
                rank.skillReqVal = 0.0f;
                rank.skillReq.clear();
            }
            node.vanillaGate = true;
            ++applied;
        }
        spdlog::info("[prog] vanilla Smithing tree: {}/{} Skyrim.esm perk(s) now gate on the vanilla "
                     "edges + skill thresholds for followers (list conditions not evaluated); "
                     "left on the list's own rules:{}", applied, std::size(kVanilla),
                     notes.empty() ? " none" : notes);
    }

}

namespace MFO::ProgAllocator {

    // The vanilla gate for a flagged node: Smithing skill >= the vanilla level AND (when the node
    // has vanilla prerequisites present in the catalog) ANY one of them owned. The lock reason
    // names exactly what is missing. Replaces the generic parent walk and the list's perkConditions.
    bool VanillaGateOk(RE::Actor* a_actor, RE::TESNPC* a_base, const ProgState& a_st,
                       const Progression::PerkNodeView& a_node, std::string& a_whyNot) {
        std::string skillPart, perkPart;
        const auto& rank = a_node.ranks[0];
        if (rank.skillReqAV != RE::ActorValue::kNone) {
            float have = 0.0f;
            if (auto* avo = a_actor->AsActorValueOwner()) have = avo->GetBaseActorValue(rank.skillReqAV);
            if (have < rank.skillReqVal)
                skillPart = Str::Fmt(Str::K::Pg_VanSkill, { rank.skillReq, Str::Arg::F(have, 0) });
        }
        if (!a_node.parentPerkIDs.empty()) {
            bool any = false;
            for (const auto pid : a_node.parentPerkIDs)
                if (OwnsAnyRank(a_actor, a_base, a_st, pid)) { any = true; break; }
            if (!any) {
                for (const auto pid : a_node.parentPerkIDs) {
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    const std::string one = (nm && *nm) ? nm : "prerequisite";
                    perkPart = perkPart.empty() ? one : Str::Fmt(Str::K::Pg_VanOr, { perkPart, one });
                }
            }
        }
        if (skillPart.empty() && perkPart.empty()) return true;
        const std::string both = (!skillPart.empty() && !perkPart.empty())
                                     ? Str::Fmt(Str::K::Pg_VanAnd, { skillPart, perkPart })
                                     : (skillPart.empty() ? perkPart : skillPart);
        a_whyNot = Str::Fmt(Str::K::Pg_VanReq, { both });
        return false;
    }

    // Fix-forward for saves written before the exemption: the vanilla Smithing perks recorded in
    // strippedPerks go back through the existing RestoreNativePerksImpl and leave the list; every
    // other stripped perk stays recorded. The PRGN layout is untouched (the list only shrinks).
    // MAIN THREAD (called from StripNativePerks).
    int RestoreStrippedVanillaSmithing(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st) {
        std::vector<RE::FormID> vanilla, others;
        for (const auto fid : a_st.strippedPerks)
            (Progression::IsVanillaSmithingPerk(fid) ? vanilla : others).push_back(fid);
        if (vanilla.empty()) return 0;
        const bool heldBefore = a_st.nativeHeld;
        a_st.strippedPerks = std::move(vanilla);
        const int restored = RestoreNativePerksImpl(a_actor, a_base, a_st);   // restores, clears the list, nativeHeld=false
        a_st.strippedPerks = std::move(others);
        a_st.nativeHeld = heldBefore;
        spdlog::info("[prog] {:08X}: vanilla Smithing perks recorded as stripped by an earlier build were "
                     "restored ({} put back on the base) and dropped from the stripped list ({} other "
                     "stripped perk(s) kept)", a_actor->GetFormID(), restored, a_st.strippedPerks.size());
        return restored;
    }

}
