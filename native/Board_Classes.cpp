#include "PCH.h"

// Board_Classes.cpp -- the Field-Orders board's CLASSES tab (class update card C10, ClickUp 86e3dhaba): the class tree for the
// selected follower, an Equip / Switch / Classes-off / Reset action set, mastery and level at a glance.
// Shown only while Classes::Active() (class ESL present AND bClassTree = 1); absent otherwise, like the hosted add-on tab.
//
// THREADING: DrawClassesTab runs on the RENDER thread and mutates NOTHING. It reads the class state through the Classes::
// accessors (every one takes State.cpp's own mutex, safe from any thread, never held across ImGui) and the board snapshot. Every
// action queues an EditCmd (EditKind::ClassAction); ApplyEdits (Board.cpp) drains it on the worker that owns g_followers and calls
// ApplyClassEdit below, which is where the WORKER-ONLY Classes::EquipClass / UnequipClass / ResetClassTables run (Classes.h).

#include <imgui.h>

#include "Board_internal.h"
#include "classes/Classes.h"

namespace MFO::Board {

    namespace {
        using Classes::Id;

        // "Knight, Warrior" for every class in a mask.
        std::string MaskNames(Classes::Mask a_mask) {
            std::string out;
            for (std::size_t i = 0; i < Classes::kClassCount; ++i) {
                if (!(a_mask & Classes::Bit(static_cast<Id>(i)))) continue;
                if (!out.empty()) out += ", ";
                out += Classes::Get(static_cast<Id>(i)).name;
            }
            return out;
        }

        // Why a class is locked: its UnlockRule in words (allOf, then each pool's count).
        std::string RequirementText(const Classes::ClassDef& a_def) {
            const auto& u = a_def.unlock;
            std::string out;
            auto add = [&](const std::string& a_part) { if (!out.empty()) out += "   "; out += a_part; };
            if (u.allOf) add(Str::Fmt(Str::K::Cl_ReqAll, { MaskNames(u.allOf) }));
            if (u.needA && u.poolA) add(Str::Fmt(Str::K::Cl_ReqNOf, { static_cast<int>(u.needA), MaskNames(u.poolA) }));
            if (u.needB && u.poolB) add(Str::Fmt(Str::K::Cl_ReqNOf, { static_cast<int>(u.needB), MaskNames(u.poolB) }));
            return out;
        }

        // The class's passive state for a follower, as a short label ("" = none).
        const char* PassiveLabel(bool a_equipped, bool a_mastered) {
            if (a_equipped) return Str::Get(Str::K::Cl_PassiveFull);
            if (a_mastered) return Str::Get(Str::K::Cl_PassiveQuarter);
            return "";
        }

        // One class row. a_full = the unlocked, actionable row; otherwise the locked text row (name + why).
        void DrawClassRow(RE::FormID a_fid, const Classes::ClassDef& a_def, std::uint8_t a_equipped,
                          Classes::Mask a_mastered, const MenuSkin& a_skin, bool a_unlocked) {
            const Id id = a_def.id;
            const bool isEquipped = (a_equipped == static_cast<std::uint8_t>(id));
            const bool mastered = (a_mastered & Classes::Bit(id)) != 0;
            ImGui::PushID(static_cast<int>(id));
            ImGui::TableNextRow();

            // Name (the equipped class is highlighted, a locked one is greyed).
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (!a_unlocked) {
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.dim);
                ImGui::TextUnformatted(std::string(a_def.name).c_str());
                ImGui::PopStyleColor();
            } else if (isEquipped) {
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
                ImGui::Text("> %s", std::string(a_def.name).c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextUnformatted(std::string(a_def.name).c_str());
            }

            // Level, bar, XP.
            ImGui::TableSetColumnIndex(1);
            if (a_unlocked) {
                const std::uint8_t cap = Classes::LevelCap(id);
                const Classes::ClassProgress pr = Classes::GetProgress(a_fid, id);
                if (cap == 0) {
                    ImGui::TextDisabled("%s", Str::Get(Str::K::Cl_LevelNone));
                } else {
                    const int lvl = std::clamp<int>(pr.level, 0, cap);
                    const std::string ov = Str::Fmt(Str::K::Cl_Level, { lvl, static_cast<int>(cap) });
                    ImGui::ProgressBar(static_cast<float>(lvl) / static_cast<float>(cap), ImVec2(120.0f, 0.0f), ov.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", Str::Fmt(Str::K::Cl_Xp, { static_cast<unsigned>(pr.xp) }).c_str());
                }
            } else {
                ImGui::TextDisabled("%s", Str::Fmt(Str::K::Cl_Level, { 0, static_cast<int>(Classes::LevelCap(id)) }).c_str());
            }

            // State: equipped / mastered / passive / locked, with the reason a locked one is locked.
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            if (!a_unlocked) {
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.dim);
                ImGui::TextWrapped("%s  %s", Str::Get(Str::K::Cl_Locked), RequirementText(a_def).c_str());
                ImGui::PopStyleColor();
            } else {
                std::string st = isEquipped ? Str::Get(Str::K::Cl_Equipped) : Str::Get(Str::K::Cl_Unlocked);
                if (mastered) { st += "  ["; st += Str::Get(Str::K::Cl_Mastered); st += "]"; }
                const char* pl = PassiveLabel(isEquipped, mastered);
                if (*pl) { st += "  "; st += pl; }
                if (isEquipped) ImGui::TextColored(a_skin.accent, "%s", st.c_str());
                else ImGui::TextUnformatted(st.c_str());
            }

            // Action: only an unlocked, not-yet-equipped class can be equipped (Switch when another class is on).
            ImGui::TableSetColumnIndex(3);
            if (a_unlocked && !isEquipped) {
                const bool sw = (a_equipped != Classes::kNoClass);
                if (Click::Button(Str::Label(sw ? Str::K::Cl_Switch : Str::K::Cl_Equip, "clsequip"))) {
                    EditCmd e{ EditKind::ClassAction, a_fid, 0, 0u, static_cast<float>(static_cast<int>(id)) };
                    e.verbId = static_cast<int>(ClassVerb::Equip);
                    QueueEdit(e);
                }
            }
            ImGui::PopID();
        }

        void DrawTable(const char* a_id, RE::FormID a_fid, const std::vector<Id>& a_ids, std::uint8_t a_equipped,
                       Classes::Mask a_mastered, const MenuSkin& a_skin, bool a_unlocked) {
            if (a_ids.empty()) return;
            if (!ImGui::BeginTable(a_id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return;
            ImGui::TableSetupColumn(Str::Get(Str::K::Cl_ColClass), ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn(Str::Get(Str::K::Cl_ColLevel), ImGuiTableColumnFlags_WidthFixed, 230.0f);
            ImGui::TableSetupColumn(Str::Get(Str::K::Cl_ColState), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (Id id : a_ids) DrawClassRow(a_fid, Classes::Get(id), a_equipped, a_mastered, a_skin, a_unlocked);
            ImGui::EndTable();
        }
    }

    // WORKER (ApplyEdits). c.param carries the class Id ordinal for Equip; Unequip / Reset ignore it.
    void ApplyClassEdit(const EditCmd& a_cmd, FollowerState& a_st) {
        switch (static_cast<ClassVerb>(a_cmd.verbId)) {
        case ClassVerb::Equip: {
            const int ord = static_cast<int>(a_cmd.param + 0.5f);
            if (ord < 0 || ord >= static_cast<int>(Classes::kClassCount)) return;
            const auto r = Classes::EquipClass(a_cmd.fid, static_cast<Id>(ord), a_st);
            spdlog::info("[classes] board equip {:08X} -> {}: {}", a_cmd.fid,
                         std::string(Classes::Get(static_cast<Id>(ord)).name),
                         r == Classes::EquipResult::Refused ? "refused" :
                         r == Classes::EquipResult::Restored ? "restored" :
                         r == Classes::EquipResult::Defaults ? "defaults" : "kept");
            break; }
        case ClassVerb::Unequip:
            spdlog::info("[classes] board unequip {:08X}: {}", a_cmd.fid,
                         Classes::UnequipClass(a_cmd.fid, a_st) ? "done" : "refused");
            break;
        case ClassVerb::Reset:
            spdlog::info("[classes] board reset {:08X}: {}", a_cmd.fid,
                         Classes::ResetClassTables(a_cmd.fid, a_st) ? "done" : "refused");
            break;
        default:
            break;
        }
    }

    void DrawClassesTab(const Snapshot& snap, bool a_active, int a_tabIdx, ImGuiTabItemFlags a_tabSelFlags,
                        const MenuSkin& a_skin, bool a_r1Ready, int& s_tab, bool& a_pickerDrawn) {
        if (!a_active) return;
        if (!ImGui::BeginTabItem(Str::Label(Str::K::Cl_Tab, "tabclasses"), nullptr, a_tabSelFlags)) return;
        s_tab = a_tabIdx;

        static RE::FormID s_sel = 0;
        std::vector<RE::FormID> party;
        for (const auto& r : snap.rows) if (r.active || r.away) party.push_back(r.id);
        const FollowerRow* who = nullptr;
        for (const auto& r : snap.rows) if ((r.active || r.away) && r.id == s_sel) { who = &r; break; }
        if (!who) for (const auto& r : snap.rows) if (r.active || r.away) { who = &r; s_sel = r.id; break; }
        if (!who) {
            ImGui::TextDisabled("%s", Str::Get(Str::K::Cl_NoFollower));
            ImGui::EndTabItem();
            return;
        }

        auto switchFollower = [&](int d) {
            if (party.size() < 2) return;
            int idx = 0;
            for (int k = 0; k < static_cast<int>(party.size()); ++k)
                if (party[k] == s_sel) { idx = k; break; }
            idx = ((idx + d) % static_cast<int>(party.size()) + static_cast<int>(party.size())) % static_cast<int>(party.size());
            s_sel = party[idx];
        };
        const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // L1/R1 = party switch, gated exactly like the Gambits and Progression tabs (r1Ready keeps the opening press from skipping).
        if (!ImGui::IsAnyItemActive() && !popupOpen) {
            if (a_r1Ready && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) switchFollower(+1);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) switchFollower(-1);
        }

        // Follower context bar.
        ImGui::AlignTextToFramePadding();
        ImGui::BeginDisabled(party.size() < 2);
        if (Click::SmallButton("<##prevcf")) switchFollower(-1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushFont(g_fontHead);
        ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
        ImGui::TextUnformatted(who->name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::BeginDisabled(party.size() < 2);
        if (Click::SmallButton(">##nextcf")) switchFollower(+1);
        ImGui::EndDisabled();

        const RE::FormID fid = who->id;
        const std::uint8_t equipped = Classes::GetEquipped(fid);
        const Classes::Mask mastered = Classes::GetMastered(fid);
        const bool hasClass = (equipped != Classes::kNoClass) && equipped < Classes::kClassCount;
        const Classes::ClassDef* eq = hasClass ? &Classes::Get(static_cast<Id>(equipped)) : nullptr;

        if (eq) ImGui::TextColored(a_skin.accent, "%s", Str::Fmt(Str::K::Cl_Current, { std::string(eq->name) }).c_str());
        else ImGui::TextDisabled("%s", Str::Get(Str::K::Cl_Off));
        ImGui::TextDisabled("%s", Str::Get(Str::K::Cl_Note));

        // Follower-level actions: classes off, and the reset (with an in-page confirm popup).
        ImGui::BeginDisabled(!hasClass);
        if (Click::Button(Str::Label(Str::K::Cl_Unequip, "clsoff"))) {
            EditCmd e{ EditKind::ClassAction, fid, 0, 0u, 0.0f };
            e.verbId = static_cast<int>(ClassVerb::Unequip);
            QueueEdit(e);
        }
        ImGui::EndDisabled();
        if (hasClass && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Str::Get(Str::K::Cl_TipUnequip));
        ImGui::SameLine();
        // Tier 3+ ships no default gambit set (Classes.h), so ResetClassTables would refuse: grey it and say why.
        const bool canReset = hasClass && eq->tier <= 2;
        ImGui::BeginDisabled(!canReset);
        if (Click::Button(Str::Label(Str::K::Cl_Reset, "clsreset"))) ImGui::OpenPopup("##clsreset");
        ImGui::EndDisabled();
        if (hasClass && !canReset && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", Str::Get(Str::K::Cl_TipResetNone));

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopup("##clsreset")) {
            a_pickerDrawn = true;
            ImGui::PushFont(g_fontHead);
            ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
            ImGui::TextUnformatted(Str::Get(Str::K::Cl_ResetTitle));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
            ImGui::TextWrapped("%s", Str::Fmt(Str::K::Cl_ResetBody, { who->name, eq ? std::string(eq->name) : std::string() }).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, a_skin.danger);
            // Destructive confirm: stock ImGui on purpose, so sliding off before release still aborts it.
            if (ImGui::Selectable(Str::Label(Str::K::Cl_ResetYes, "clsresetok"))) {
                EditCmd e{ EditKind::ClassAction, fid, 0, 0u, 0.0f };
                e.verbId = static_cast<int>(ClassVerb::Reset);
                QueueEdit(e);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor();
            if (Click::Selectable(Str::Label(Str::K::Cl_ResetNo, "clsresetno"))) ImGui::CloseCurrentPopup();
            ImGui::SetItemDefaultFocus();   // land on Cancel, not the danger row
            ImGui::EndPopup();
        }
        ImGui::Separator();

        // The tree. T1 roots with their T2 children (a T2's rootT1 is the combatClassOverride ordinal, root Id = rootT1 - 1),
        // then T3 / T4 (an unlocked one is a normal row, a locked one is greyed text), T5 only once unlocked.
        auto unlockedNow = [&](Id id) { return Classes::IsUnlocked(fid, id); };
        for (std::size_t r = 0; r < 3; ++r) {
            const Id root = static_cast<Id>(r);
            std::vector<Id> ids{ root };
            for (std::size_t i = 0; i < Classes::kClassCount; ++i) {
                const auto& d = Classes::Get(static_cast<Id>(i));
                if (d.tier == 2 && d.rootT1 == r + 1) ids.push_back(d.id);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
            ImGui::TextUnformatted(std::string(Classes::Get(root).name).c_str());
            ImGui::PopStyleColor();
            std::string tid = "##clstree" + std::to_string(r);
            // Unlocked rows first-class; a locked T2 is still shown as a locked row (it has a reason).
            if (ImGui::BeginTable(tid.c_str(), 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthFixed, 230.0f);
                ImGui::TableSetupColumn("##s", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                for (Id id : ids) DrawClassRow(fid, Classes::Get(id), equipped, mastered, a_skin, unlockedNow(id));
                ImGui::EndTable();
            }
        }
        for (int tier = 3; tier <= 5; ++tier) {
            std::vector<Id> open, locked;
            for (std::size_t i = 0; i < Classes::kClassCount; ++i) {
                const auto& d = Classes::Get(static_cast<Id>(i));
                if (d.tier != tier) continue;
                if (unlockedNow(d.id)) open.push_back(d.id);
                else if (tier < 5) locked.push_back(d.id);   // T5 stays hidden until unlocked
            }
            if (!open.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.accent);
                ImGui::TextUnformatted(Str::Fmt(Str::K::Cl_Tier, { tier }).c_str());
                ImGui::PopStyleColor();
                std::string tid = "##clsT" + std::to_string(tier);
                DrawTable(tid.c_str(), fid, open, equipped, mastered, a_skin, true);
            }
            if (!locked.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, a_skin.dim);
                ImGui::TextUnformatted(Str::Fmt(Str::K::Cl_Tier, { tier }).c_str());
                ImGui::PopStyleColor();
                std::string tid = "##clsL" + std::to_string(tier);
                DrawTable(tid.c_str(), fid, locked, equipped, mastered, a_skin, false);
            }
        }
        ImGui::EndTabItem();
    }

}
