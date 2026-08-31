#include "PCH.h"

// Board_Progression.cpp -- the Field-Orders board's hosted PROGRESSION tab
// (#74 component 3): tab body, class prompt, skill table, perk dome window,
// node/skill popups, respec footer. Mechanically split out of Board.cpp's
// DrawFieldKit (pure move; see Board_internal.h for the shared substrate).
// RENDER THREAD, inside DrawFieldKit's tab bar -- it mutates NOTHING: every
// action queues an EditCmd that ApplyEdits (Board.cpp) re-posts to the MAIN
// thread, where the §5 backend gate re-validates before any engine write.

#include <cmath>

#include <imgui.h>

#include "Board.h"
#include "Board_internal.h"
#include "Progression.h"   // #74: the frozen catalog — lock-free render reads by contract
#include "ProgAllocator.h"

namespace MFO::Board {

    void DrawProgressionTab(const Snapshot& snap, bool progActive,
                            ImGuiTabItemFlags tabSelFlags, ImGuiIO& io,
                            const MenuSkin& skin, bool r1Ready,
                            int& s_tab, bool& pickerDrawnThisFrame) {
                // ── THE PROGRESSION TAB (#74 component 3) ───────────────
                // Emitted ONLY when the addon ESL is detected (§1) — absent
                // means absent, the same optional-addon pattern as the MCM
                // Detected line. The draw reads two immutable sources: the
                // FROZEN catalog (lock-free on this thread by its contract)
                // and the allocator's published value-only views (the generic
                // hosted board-tab payload, snap.boardTabs). It mutates NOTHING —
                // every action queues an EditCmd that ApplyEdits re-posts to the
                // MAIN thread, where the §5 backend gate re-validates before any
                // engine write.
                // v1.1 Phase 6c: the tab CAPTION is the add-on's self-declared
                // label (boardTab MESG FULL), not a DLL "Progression" literal —
                // the first published+active hosted tab's label. Fallback keeps
                // the ID stable if an add-on ever ships an empty label.
                const char* hostedTabLabel = "Field Orders";
                for (const auto& t : snap.boardTabs)
                    if (t.active) { if (!t.label.empty()) hostedTabLabel = t.label.c_str(); break; }
                if (progActive && ImGui::BeginTabItem(hostedTabLabel, nullptr, tabSelFlags)) {
                    s_tab = 2;
                    // 6b: read the concrete payload out of the generic hosted-tab
                    // envelope — the first published+active board tab (one today).
                    const ProgAllocator::BoardProgSnap* progPtr = nullptr;
                    for (const auto& t : snap.boardTabs)
                        if (t.active) { progPtr = t.content.get(); break; }
                    const auto& prog = *progPtr;   // progActive ⇒ a non-null active payload

                    static RE::FormID s_psel = 0;          // selected follower
                    static RE::FormID s_promptedFor = 0;   // class prompt auto-opened for
                    static int   s_skillCur = 0;           // index into the non-empty-tree list
                    static float s_zoom = 1.0f;
                    static bool  s_scrollHome = true;      // recentre on the root
                    static int   s_nodeTree = -1, s_nodeIdx = -1;   // node popup target
                    static RE::ActorValue s_actAv = RE::ActorValue::kNone;   // §16 action-popup skill
                    // Round 4: the A press that OPENS the tree window must be
                    // RELEASED before A can act inside it — without this, the
                    // same edge instantly opened the seeded root's take popup
                    // (the r1Ready guard's class of bug, same cure).
                    static bool s_aGuard = false;

                    const ProgAllocator::BoardFollowerView* who = nullptr;
                    for (const auto& r : prog.rows) if (r.id == s_psel) { who = &r; break; }
                    if (!who && !prog.rows.empty()) {
                        who = &prog.rows.front();
                        s_psel = who->id;
                        s_promptedFor = 0;
                        s_scrollHome = true;
                    }

                    // Tell the main thread whose tree to publish next (atomic
                    // — a plain value, safe from the render thread).
                    ProgAllocator::SetBoardFocus(s_psel);

                    if (!who) {
                        ImGui::TextDisabled("No active follower. Recruit one to manage progression.");
                        ImGui::EndTabItem();
                    } else {
                        const bool popupOpen = ImGui::IsPopupOpen(nullptr,
                            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

                        auto switchFollower = [&](int d) {
                            if (prog.rows.size() < 2) return;
                            int idx = 0;
                            for (int k = 0; k < (int)prog.rows.size(); ++k)
                                if (prog.rows[k].id == s_psel) { idx = k; break; }
                            idx = ((idx + d) % (int)prog.rows.size() + (int)prog.rows.size())
                                  % (int)prog.rows.size();
                            s_psel = prog.rows[idx].id;
                            s_promptedFor = 0;
                            s_scrollHome = true;
                        };

                        // L1/R1 = party switch — the Gambits gating verbatim
                        // (r1Ready keeps the opening R1 press from skipping).
                        if (!ImGui::IsAnyItemActive() && !popupOpen) {
                            if (r1Ready && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                                switchFollower(+1);
                            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                                switchFollower(-1);
                        }

                        // ── FOLLOWER CONTEXT BAR ────────────────────────
                        ImGui::AlignTextToFramePadding();
                        ImGui::BeginDisabled(prog.rows.size() < 2);
                        if (ImGui::SmallButton("<##prevpf")) switchFollower(-1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::PushFont(g_fontHead);
                        ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                        ImGui::TextUnformatted(who->name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        ImGui::SameLine();
                        ImGui::BeginDisabled(prog.rows.size() < 2);
                        if (ImGui::SmallButton(">##nextpf")) switchFollower(+1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (who->enrolled && who->clsId != 0) {
                            ImGui::TextDisabled("level %u", (unsigned)who->level);
                            ImGui::SameLine();
                            std::string cl = std::string("Class: ") +
                                (who->clsName.empty() ? "?" : who->clsName.c_str());
                            if (ImGui::SmallButton(cl.c_str())) ImGui::OpenPopup("##pclass");
                        } else {
                            ImGui::TextDisabled("not enrolled");
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("  [LB]/[RB] change follower");
                        ImGui::Separator();

                        // ── CLASS PROMPT (§15 — REQUIRED behavior) ──────
                        // Selecting an UNENROLLED (or class-less) follower on
                        // this tab pops the picker ONCE per selection: nothing
                        // is assigned until the player explicitly picks; B
                        // backs out (popup cascade) and the button below
                        // reopens. The classes are addon-DECLARED (§18.6): the
                        // picker draws prog.classes (dynamic-N), each carrying
                        // its class-def FormID — nothing hardcoded here.
                        // P2 (deck round 3): ENROLLED-FIRST branching. The
                        // eligibility check (teammate/dismissal quirks) can
                        // flap at runtime — it must gate ENROLLMENT only,
                        // never blank the UI of an already-enrolled follower
                        // (that read as "skills not visible at all").
                        const bool progReady  = who->enrolled && who->clsId != 0;
                        const bool needsClass = !progReady && who->eligible;
                        if (needsClass && s_promptedFor != s_psel && !popupOpen) {
                            ImGui::OpenPopup("##pclass");
                            s_promptedFor = s_psel;
                        }
                        ImGui::SetNextWindowPos(
                            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                        if (ImGui::BeginPopup("##pclass")) {
                            pickerDrawnThisFrame = true;
                            ImGui::PushFont(g_fontHead);
                            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                            ImGui::TextUnformatted("Choose a class");
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                            ImGui::TextDisabled("Skills auto-scale to level by class; perks stay yours to pick.");
                            ImGui::TextDisabled("d-pad move   [A]/E pick   [B]/Esc back");
                            ImGui::Separator();
                            // §18.6: the addon-declared classes (dynamic-N).
                            // Each Selectable carries its class-def FormID in
                            // the EditCmd's perk field (a float param can't
                            // hold a 32-bit FormID losslessly).
                            if (prog.classes.empty())
                                ImGui::TextDisabled("(no classes declared by the addon)");
                            int k = 0;
                            for (const auto& [classId, className] : prog.classes) {
                                const bool cur = (who->clsId == classId);
                                ImGui::PushID(k++);
                                if (ImGui::Selectable(className.c_str(), cur)) {
                                    EditCmd e{ EditKind::AddonAction, s_psel, 0, 0u, 0.0f };
                                    e.verbId = (int)AddonVerb::SetClass;
                                    e.perk = classId;
                                    QueueEdit(e);
                                    ImGui::CloseCurrentPopup();
                                }
                                if (cur) ImGui::SetItemDefaultFocus();
                                ImGui::PopID();
                            }
                            ImGui::EndPopup();
                        }

                        if (!progReady && !who->eligible) {
                            ImGui::Spacing();
                            ImGui::TextWrapped("%s cannot progress: %s.",
                                               who->name.c_str(), who->blocker.c_str());
                        } else if (needsClass) {
                            ImGui::Spacing();
                            ImGui::TextWrapped("%s is not enrolled. Pick a class to begin -- "
                                               "no skills are touched until you do.",
                                               who->name.c_str());
                            ImGui::Spacing();
                            if (ImGui::Button("Choose class...")) ImGui::OpenPopup("##pclass");
                        } else {
                            const auto& cat = Progression::Get();   // frozen — lock-free
                            std::size_t totalNodes = 0;
                            for (const auto& t : cat.skills) totalNodes += t.nodes.size();
                            // State rows align with the catalog ONLY when the
                            // published tree is this follower's and complete —
                            // otherwise draw the shape and say "syncing".
                            const bool stateOk = (prog.treeFor == s_psel) &&
                                                 (prog.nodes.size() == totalNodes);
                            auto stateAt = [&](std::size_t flat)
                                -> const ProgAllocator::BoardNodeView* {
                                return (stateOk && flat < prog.nodes.size())
                                           ? &prog.nodes[flat] : nullptr;
                            };

                            // ── PERK POINTS — the headline number (deck
                            // field fix #1: a hover tooltip made "no points"
                            // and "broken input" look identical; this line is
                            // always on screen and self-explains a zero).
                            ImGui::PushFont(g_fontHead);
                            ImGui::TextColored(skin.accent, "Perk points: %.0f", who->unspentPerk);
                            ImGui::PopFont();
                            // §17: the derived budget, spelled out so a zero
                            // is self-explaining, never "is it broken?".
                            // Cadence comes from the LIVE economy snapshot, not
                            // a literal — the shipped default is 2, and an MCM
                            // override changes it (a hardcoded "2" misinformed).
                            const int lpp = prog.levelsPerPerkPoint > 0 ? prog.levelsPerPerkPoint : 1;
                            if (who->unspentPerk < 1.0f)
                                ImGui::TextDisabled("None to spend: 1 point per %d levels -- level "
                                                    "%u has earned %u, and %u are already spent.",
                                                    lpp, (unsigned)who->level,
                                                    (unsigned)(who->level / lpp),
                                                    (unsigned)who->allocatedRanks);
                            else
                                ImGui::TextDisabled("1 perk point per %d levels. "
                                                    "Pick a skill to open its tree.", lpp);
                            if (!stateOk) ImGui::TextDisabled("syncing follower state...");

                            // ── §16 MANUAL SKILL POINTS (design doc §16:
                            // auto-scaling is a default, never a cage — the
                            // escape hatch for mage/multiclass builds). Off
                            // by default; ON = manual REPLACES auto growth
                            // (round-4 correction, never additive): a flat 5
                            // points/level banks into a visible pool spent
                            // from the skill list below while auto per-level
                            // growth is frozen. The checkbox is a nav item —
                            // A toggles it on the pad.
                            {
                                bool man = who->manualSkills;
                                if (ImGui::Checkbox("Manual skill points", &man)) {
                                    EditCmd e{ EditKind::AddonAction, s_psel, 0, 0u,
                                               man ? 1.0f : 0.0f };
                                    e.verbId = (int)AddonVerb::SetManual;
                                    QueueEdit(e);
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip(
                                        "Manual OVERRIDE: while ON, this follower earns %d skill\n"
                                        "points per level for YOU to place -- INSTEAD OF automatic\n"
                                        "class-based skill growth, never on top of it. Toggle OFF\n"
                                        "to resume auto growth. For mage or multiclass builds the\n"
                                        "class weights won't serve.", prog.manualSkillPtsPerLevel);
                                if (who->manualSkills) {
                                    ImGui::SameLine();
                                    ImGui::PushFont(g_fontHead);
                                    ImGui::TextColored(skin.accent, "  Skill points: %d",
                                                       who->manualAvail);
                                    ImGui::PopFont();
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("  replaces auto growth -- select a skill "
                                                        "to apply (+1 base, cap %g)", prog.skillCap);
                                }
                            }
                            ImGui::Spacing();

                            // ── SKILL PICKER (deck field fix #3) ────────
                            // An explicit list instead of the Y-only cycler:
                            // one row per skill — level (auto-scaled by
                            // class; never manually assigned), perks owned in
                            // that tree, and whether anything is spendable
                            // there NOW. A / click on a row opens the tree in
                            // the dedicated window below. The ScrollY table
                            // is the Gambits pattern — its inner window is
                            // nav-flattened by ImGui, so the d-pad walks the
                            // rows on the deck (field-proven).
                            bool wantOpenTree = false;
                            bool wantSkillAct = false;   // §16: the row action popup
                            const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
                            if (ImGui::BeginTable("##pskillpick", 4,
                                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_ScrollY, ImVec2(0.0f, -footer))) {
                                ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                                ImGui::TableSetupColumn("Perks", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                                ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, 120.0f);
                                ImGui::TableHeadersRow();

                                // P2 (deck round 3): rows come PRIMARILY from
                                // the allocator's 18-line snapshot — its
                                // names are compile-time constants and its
                                // levels are engine reads, so the list can
                                // never be blank for an enrolled follower.
                                // The catalog joins per row by AV for tree
                                // data; a failed join degrades to a visible
                                // "no tree", never an empty row. Catalog-only
                                // fallback if the snapshot hasn't published.
                                const bool haveLines = !who->skills.empty();
                                if (!haveLines) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::TextDisabled("skill levels syncing...");
                                }
                                auto flatBaseOf = [&](int idx) {
                                    std::size_t f = 0;
                                    for (int t2 = 0; t2 < idx; ++t2)
                                        f += cat.skills[t2].nodes.size();
                                    return f;
                                };
                                const int rowCount = haveLines ? (int)who->skills.size()
                                                               : (int)cat.skills.size();
                                // §16 position memory (marth 2026-08-17): the
                                // apply popup pumps repeats open, but when it
                                // closes nav focus drops to the top of the
                                // list. Detect the close and restore focus to
                                // the skill we were on (keyed by AV, the stable
                                // handle the popup uses) so the menu keeps its
                                // place across an apply.
                                static bool s_skillActWasOpen = false;
                                const bool skillActOpen = ImGui::IsPopupOpen("##pskillact");
                                const bool skillActJustClosed = s_skillActWasOpen && !skillActOpen;
                                s_skillActWasOpen = skillActOpen;
                                for (int r = 0; r < rowCount; ++r) {
                                    RE::ActorValue av = RE::ActorValue::kNone;
                                    const char* label = nullptr;
                                    float lvl = 0.0f, alloc = 0.0f;
                                    int catIdx = -1;
                                    if (haveLines) {
                                        const auto& sl = who->skills[r];
                                        av = sl.av; lvl = sl.base; alloc = sl.alloc;
                                        for (int t = 0; t < (int)cat.skills.size(); ++t)
                                            if (cat.skills[t].av == av) { catIdx = t; break; }
                                        label = (catIdx >= 0 && !cat.skills[catIdx].skillName.empty())
                                                    ? cat.skills[catIdx].skillName.c_str()
                                                    : sl.name.c_str();
                                    } else {
                                        catIdx = r; av = cat.skills[r].av;
                                        label = cat.skills[r].skillName.empty()
                                                    ? "(skill)" : cat.skills[r].skillName.c_str();
                                    }
                                    const bool hasTree = catIdx >= 0 &&
                                                         !cat.skills[catIdx].nodes.empty();
                                    int ownedN = 0, availN = 0;
                                    if (hasTree) {
                                        const std::size_t base = flatBaseOf(catIdx);
                                        const auto& skl = cat.skills[catIdx];
                                        for (std::size_t k = 0; k < skl.nodes.size(); ++k)
                                            if (const auto* stn = stateAt(base + k)) {
                                                if (stn->ownedRank > 0 || stn->native) ++ownedN;
                                                if (stn->available) ++availN;
                                            }
                                    }

                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::PushID(r);
                                    // §16: with manual ON, even a tree-less
                                    // skill row is selectable (points can go
                                    // anywhere); the action popup disables
                                    // its tree entry instead.
                                    ImGui::BeginDisabled(!hasTree && !who->manualSkills);
                                    // Keep the just-edited skill highlighted, and
                                    // when the apply popup closes put nav focus
                                    // back on it (not the list top).
                                    const bool isActiveSkill =
                                        (s_actAv != RE::ActorValue::kNone && av == s_actAv);
                                    if (skillActJustClosed && isActiveSkill)
                                        ImGui::SetKeyboardFocusHere();
                                    if (ImGui::Selectable(label, isActiveSkill,
                                                          ImGuiSelectableFlags_SpanAllColumns)) {
                                        s_skillCur = catIdx;   // catalog index (may be -1)
                                        s_actAv    = av;
                                        if (who->manualSkills) {
                                            wantSkillAct = true;   // tree OR +1 — ask
                                        } else {
                                            s_scrollHome = true;
                                            wantOpenTree = true;   // OpenPopup outside the table/PushID
                                        }
                                    }
                                    ImGui::EndDisabled();
                                    ImGui::PopID();
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.0f", lvl);
                                    if (alloc > 0.5f) {
                                        ImGui::SameLine();
                                        ImGui::TextColored(skin.accent, "(+%.0f)", alloc);
                                    }
                                    ImGui::TableNextColumn();
                                    if (hasTree)
                                        ImGui::Text("%d/%d", ownedN,
                                                    (int)cat.skills[catIdx].nodes.size());
                                    else
                                        ImGui::TextDisabled("--");
                                    ImGui::TableNextColumn();
                                    if (availN > 0)
                                        ImGui::TextColored(skin.accent, "%d to spend", availN);
                                    else if (!hasTree)
                                        ImGui::TextDisabled("no tree");
                                }
                                ImGui::EndTable();
                            }

                            // ── §16 SKILL ACTION POPUP (manual ON only) ──
                            // One row press → two possible actions, so the
                            // row asks: apply a pooled point (+1 base, stays
                            // open to pump repeat points — the backend
                            // re-validates the pool per press, a stale double
                            // A is refused, never over-applied) or open the
                            // perk tree. The listPopup idiom: pad A picks,
                            // B/Esc backs out.
                            if (wantSkillAct) ImGui::OpenPopup("##pskillact");
                            ImGui::SetNextWindowPos(
                                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                            if (ImGui::BeginPopup("##pskillact")) {
                                pickerDrawnThisFrame = true;
                                // Keyed on the AV (always valid from the row),
                                // not the catalog index (may be -1 when the
                                // join failed — points still apply).
                                const ProgAllocator::BoardSkillLine* line = nullptr;
                                for (const auto& sl : who->skills)
                                    if (sl.av == s_actAv) { line = &sl; break; }
                                if (s_actAv == RE::ActorValue::kNone || !line) {
                                    ImGui::CloseCurrentPopup();
                                } else {
                                    const bool haveCatRow = s_skillCur >= 0 &&
                                                            s_skillCur < (int)cat.skills.size();
                                    const char* actLabel =
                                        (haveCatRow && !cat.skills[s_skillCur].skillName.empty())
                                            ? cat.skills[s_skillCur].skillName.c_str()
                                            : line->name.c_str();
                                    const float base = line->base, manual = line->manual;

                                    ImGui::PushFont(g_fontHead);
                                    ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                    ImGui::TextUnformatted(actLabel);
                                    ImGui::PopStyleColor();
                                    ImGui::PopFont();
                                    ImGui::TextDisabled("base %.0f (manual +%.0f)  |  %d point(s) "
                                                        "pooled", base, manual, who->manualAvail);
                                    ImGui::TextDisabled("d-pad move   [A]/E pick   [B]/Esc back");
                                    ImGui::Separator();

                                    const bool canApply = who->manualAvail >= 1 &&
                                                          base + 0.5f < prog.skillCap;
                                    ImGui::BeginDisabled(!canApply);
                                    const std::string apply = std::format(
                                        "Apply 1 skill point  ({:.0f} -> {:.0f})", base, base + 1.0f);
                                    if (ImGui::Selectable(apply.c_str(), false,
                                                          ImGuiSelectableFlags_DontClosePopups)) {
                                        EditCmd e{ EditKind::AddonAction, s_psel, 0, 0u,
                                                   (float)(int)s_actAv };
                                        e.verbId = (int)AddonVerb::ApplySkillPoint;
                                        QueueEdit(e);
                                    }
                                    ImGui::EndDisabled();
                                    if (!canApply)
                                        ImGui::TextDisabled(who->manualAvail < 1
                                                                ? "no pooled points"
                                                                : "at the skill cap");
                                    ImGui::BeginDisabled(!haveCatRow ||
                                                         cat.skills[s_skillCur].nodes.empty());
                                    if (ImGui::Selectable("Open perk tree")) {
                                        s_scrollHome = true;
                                        wantOpenTree = true;   // opened at this scope, below
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();
                                }
                                ImGui::EndPopup();
                            }

                            if (wantOpenTree) {
                                s_aGuard = true;   // swallow the opening press (round 4)
                                ImGui::OpenPopup("##ptreewin");
                            }

                            // ── THE DEDICATED TREE WINDOW (deck field fix
                            // #2: the in-tab child was far too small on the
                            // deck). A full-screen POPUP — the listPopup
                            // pattern scaled up, so B/Esc-close, input focus
                            // and the board's cascade all come from ImGui's
                            // own popup machinery, nothing new. Sized every
                            // frame off the LIVE io.DisplaySize (rewritten
                            // per frame from the real backbuffer — deck
                            // handheld 1280x800 and docked 1080p+ both land
                            // right; nothing hardcoded).
                            ImGui::SetNextWindowPos(
                                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                            ImGui::SetNextWindowSize(
                                ImVec2(io.DisplaySize.x * 0.90f, io.DisplaySize.y * 0.90f),
                                ImGuiCond_Always);
                            if (ImGui::BeginPopup("##ptreewin")) {
                                pickerDrawnThisFrame = true;
                                if (s_skillCur < 0 || s_skillCur >= (int)cat.skills.size() ||
                                    cat.skills[s_skillCur].nodes.empty())
                                    ImGui::CloseCurrentPopup();   // stale pick — bail this frame
                                else {
                                const auto& tree = cat.skills[s_skillCur];
                                std::size_t flatBase = 0;
                                for (int t = 0; t < s_skillCur; ++t)
                                    flatBase += cat.skills[t].nodes.size();

                                // ── P1: NO ImGui NAV IN THIS WINDOW ─────
                                // Two deck rounds died on ImGui's spatial
                                // auto-nav (it can only move to SUBMITTED
                                // items; virtualization culls them, and
                                // nothing seeded focus). Everything here is
                                // NoNav; selection is OURS (s_selNode), fed
                                // by explicit key reads below. Header
                                // widgets stay mouse-clickable extras — the
                                // pad reaches the same functions via
                                // LB/RB (zoom), [Y] (tree), [View]
                                // (marginal). Popped before the ##pnode
                                // popup, whose Selectables keep ImGui nav
                                // (the field-proven listPopup pattern).
                                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                                ImGui::AlignTextToFramePadding();
                                ImGui::PushFont(g_fontHead);
                                ImGui::TextColored(skin.accent, "%s", tree.skillName.c_str());
                                ImGui::SameLine();
                                ImGui::TextColored(skin.accent, "  --  %.0f point(s)",
                                                   who->unspentPerk);
                                ImGui::PopFont();
                                ImGui::SameLine();
                                if (ImGui::SmallButton("-##zo")) s_zoom = std::max(0.5f, s_zoom - 0.15f);
                                ImGui::SameLine();
                                if (ImGui::SmallButton("+##zi")) s_zoom = std::min(2.0f, s_zoom + 0.15f);
                                ImGui::SameLine();
                                static bool s_showMarginal = false;
                                ImGui::Checkbox("Show marginal", &s_showMarginal);
                                ImGui::SameLine();
                                ImGui::TextDisabled(" d-pad move  [A] node  [LB]/[RB] zoom  "
                                                    "[Y] next tree  [View] marginal  [B]/Esc back");
                                ImGui::Separator();

                                // ── DOME LAYOUT + P3 FILTER (round 5) ───
                                // The tiered layout is GONE (marth: combat
                                // trees tiered fine, magic trees were a
                                // crammed mess — and it must be universal).
                                // Nodes render from the AUTHORED dome
                                // coordinates — the exact hpos/vpos the
                                // game's own perk menu draws. Every perk mod
                                // ships these (the constellation menu needs
                                // them), so ANY tree — Vokriinator-scale
                                // merges included — arrives WITH good
                                // positions. Layout only: prereq GATING is
                                // the allocator's and unchanged. Cached:
                                // recompute on skill / marginal-toggle
                                // change. Filter unchanged: dead perks never
                                // reached the catalog; marginal hidden by
                                // default except needed passthroughs.
                                static int  s_layoutSkill = -1;
                                static bool s_layoutMarg  = false;
                                static std::vector<int>    s_vis;      // node idx per entry
                                static std::vector<int>    s_visOf;    // node idx -> vis idx / -1
                                static std::vector<char>   s_visPass;  // dimmed passthrough
                                static std::vector<ImVec2> s_visPos;   // dome units, ~1 per node gap
                                static float s_spanX = 1.0f, s_spanY = 1.0f;   // extents in units
                                static int   s_selNode = -1;           // P1 explicit selection
                                static bool  s_reclamp = false;        // pull selection into view once
                                static double s_lastMoveT = 0.0;       // pad-move throttle
                                if (s_layoutSkill != s_skillCur || s_layoutMarg != s_showMarginal) {
                                    // Same tree, different filter (marginal
                                    // toggle)? Keep the player's place.
                                    const bool sameSkill = (s_layoutSkill == s_skillCur);
                                    const int  prevNode  =
                                        (sameSkill && s_selNode >= 0 && s_selNode < (int)s_vis.size())
                                            ? s_vis[s_selNode] : -1;

                                    const int n = (int)tree.nodes.size();
                                    std::vector<char> keep(n, 1), pass(n, 0);
                                    if (!s_showMarginal) {
                                        std::vector<char> needed(n, 0);
                                        for (int i = 0; i < n; ++i)
                                            needed[i] = tree.nodes[i].verdict ==
                                                        Progression::Verdict::kEffective;
                                        for (int guard = 0; guard < 64; ++guard) {
                                            bool changed = false;
                                            for (int i = 0; i < n; ++i) {
                                                if (!needed[i]) continue;
                                                for (const auto pi : tree.nodes[i].parentIndices)
                                                    if (!needed[(int)pi]) {
                                                        needed[(int)pi] = 1;
                                                        changed = true;
                                                    }
                                            }
                                            if (!changed) break;
                                        }
                                        for (int i = 0; i < n; ++i) {
                                            keep[i] = needed[i];
                                            pass[i] = needed[i] &&
                                                      tree.nodes[i].verdict !=
                                                          Progression::Verdict::kEffective;
                                        }
                                    }
                                    // AUTHORED positions, normalized (the
                                    // universal part). Round-5b DATA-TRACED
                                    // convention fix (installed One-Handed
                                    // AVIF, Requiem.esp): the constellation
                                    // position is the COMPOSITE of the
                                    // integer grid slot and the float fine
                                    // offset — x = gridX + hpos, y_up =
                                    // gridY + vpos, with y growing UP the
                                    // dome. Neither float alone reproduces
                                    // the game (vpos-up put Penetrating
                                    // Strikes BELOW Weapon Mastery — the
                                    // field bug; vpos-down put capstones at
                                    // the bottom). Composite: Weapon Mastery
                                    // y=0.0 bottom, Penetrating Strikes 0.9
                                    // straight above it (same x column),
                                    // Stunning Charge 6.2 top — the real
                                    // constellation. Screen y flips once
                                    // (root lands at the canvas bottom).
                                    // Then scale by the MEDIAN nearest-
                                    // neighbour distance so ONE UNIT ≈ ONE
                                    // TYPICAL NODE GAP for any authored
                                    // scale: any coordinate units land
                                    // identically, and one pathological
                                    // close pair cannot inflate the layout
                                    // (median, never min).
                                    s_vis.clear(); s_visPos.clear(); s_visPass.clear();
                                    s_visOf.assign(n, -1);
                                    auto compX = [&](int i) {
                                        return static_cast<float>(tree.nodes[i].gridX) +
                                               tree.nodes[i].hpos;
                                    };
                                    auto compY = [&](int i) {   // grows UP the dome
                                        return static_cast<float>(tree.nodes[i].gridY) +
                                               tree.nodes[i].vpos;
                                    };
                                    // Outlier guard: real files carry
                                    // UNINITIALIZED grid fields on some
                                    // sections (the traced Requiem root sat
                                    // at ~4.5e8) — a malformed overhaul
                                    // could ship that on a perk node and
                                    // detonate the bounding box. Insane
                                    // coords are excluded from the bbox and
                                    // clamped to its edge: still rendered,
                                    // still selectable, never layout-fatal.
                                    auto sane = [](float a_c) { return std::fabs(a_c) < 1.0e5f; };
                                    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
                                    for (int i = 0; i < n; ++i) {
                                        if (!keep[i]) continue;
                                        if (!sane(compX(i)) || !sane(compY(i))) continue;
                                        minX = std::min(minX, compX(i));
                                        maxX = std::max(maxX, compX(i));
                                        minY = std::min(minY, compY(i));
                                        maxY = std::max(maxY, compY(i));
                                    }
                                    if (minX > maxX) { minX = maxX = 0.0f; minY = maxY = 0.0f; }
                                    for (int i = 0; i < n; ++i) {
                                        if (!keep[i]) continue;
                                        const float cx = std::clamp(compX(i), minX, maxX);
                                        const float cy = std::clamp(compY(i), minY, maxY);
                                        s_visOf[i] = (int)s_vis.size();
                                        s_vis.push_back(i);
                                        s_visPos.push_back(ImVec2(cx - minX, maxY - cy));
                                        s_visPass.push_back(pass[i] ? 1 : 0);
                                    }
                                    float dtyp = 1.0f;
                                    if (s_vis.size() >= 2) {
                                        std::vector<float> nn(s_vis.size(), 1e9f);
                                        for (std::size_t a = 0; a < s_vis.size(); ++a)
                                            for (std::size_t b = a + 1; b < s_vis.size(); ++b) {
                                                const float ddx = s_visPos[a].x - s_visPos[b].x;
                                                const float ddy = s_visPos[a].y - s_visPos[b].y;
                                                const float d = std::sqrt(ddx * ddx + ddy * ddy);
                                                nn[a] = std::min(nn[a], d);
                                                nn[b] = std::min(nn[b], d);
                                            }
                                        std::vector<float> nz;
                                        for (const float d : nn)
                                            if (d > 1e-4f && d < 1e8f) nz.push_back(d);
                                        if (!nz.empty()) {
                                            std::nth_element(nz.begin(),
                                                             nz.begin() + nz.size() / 2, nz.end());
                                            dtyp = std::max(1e-3f, nz[nz.size() / 2]);
                                        }
                                    }
                                    s_spanX = 0.0f; s_spanY = 0.0f;
                                    for (auto& p2 : s_visPos) {
                                        p2.x /= dtyp; p2.y /= dtyp;
                                        s_spanX = std::max(s_spanX, p2.x);
                                        s_spanY = std::max(s_spanY, p2.y);
                                    }
                                    // Coincident authored coords: rendered as
                                    // close as authored (never amplified) but
                                    // nudged off dead-center so both stay
                                    // visible and selectable.
                                    for (std::size_t a = 0; a < s_visPos.size(); ++a)
                                        for (std::size_t b = a + 1; b < s_visPos.size(); ++b)
                                            if (std::fabs(s_visPos[a].x - s_visPos[b].x) < 0.02f &&
                                                std::fabs(s_visPos[a].y - s_visPos[b].y) < 0.02f)
                                                s_visPos[b].x += 0.22f * (float)((b - a) % 3 + 1);
                                    // Restore the player's place on a filter
                                    // toggle; otherwise seed the ROOT region
                                    // — lowest on the canvas, centre-most —
                                    // so something is ALWAYS selected before
                                    // any input arrives (P1).
                                    if (prevNode >= 0 && prevNode < (int)s_visOf.size() &&
                                        s_visOf[prevNode] >= 0) {
                                        s_selNode = s_visOf[prevNode];
                                        s_reclamp = true;   // layout shifted — pull it into view
                                    } else {
                                        s_selNode = -1;
                                        float bestScore = 1e9f;
                                        for (int k = 0; k < (int)s_vis.size(); ++k) {
                                            const float score =
                                                (s_spanY - s_visPos[k].y) * 2.0f +
                                                std::fabs(s_visPos[k].x - s_spanX * 0.5f);
                                            if (score < bestScore) { bestScore = score; s_selNode = k; }
                                        }
                                        s_scrollHome = true;   // new tree → land on the root
                                    }
                                    s_layoutSkill = s_skillCur;
                                    s_layoutMarg  = s_showMarginal;
                                }

                                ImGui::BeginChild("##ptcanvas", ImVec2(0.0f, 0.0f),
                                                  ImGuiChildFlags_Borders,
                                                  ImGuiWindowFlags_HorizontalScrollbar);
                                {
                                    const ImVec2 childSz = ImGui::GetWindowSize();
                                    const float pad = 90.0f * s_zoom;
                                    // Per-frame FIT, units → pixels: fill the
                                    // canvas when that keeps nodes readable,
                                    // else clamp the typical node gap between
                                    // 110px and 300px (zoom multiplies after)
                                    // — a Vokriinator-scale tree scrolls at a
                                    // readable density instead of clumping,
                                    // a five-node tree doesn't stretch across
                                    // the screen. No per-tree special cases:
                                    // span and gap both come from the tree's
                                    // own normalized geometry.
                                    const float fitU = std::min(
                                        (childSz.x - 2.0f * pad) / std::max(s_spanX, 0.5f),
                                        (childSz.y - 2.0f * pad) / std::max(s_spanY, 0.5f));
                                    const float unit = std::clamp(fitU, 110.0f, 300.0f) * s_zoom;
                                    // Round 4 feel state: zoom edges recenter
                                    // the selection; the mouse only steers
                                    // selection while it actually MOVES (a
                                    // parked deck cursor must not fight the
                                    // pad); follow-scroll runs only on
                                    // intent, so the wheel can roam freely.
                                    static float s_prevZoom = -1.0f;
                                    const bool zoomChanged = (s_prevZoom >= 0.0f && s_prevZoom != s_zoom);
                                    s_prevZoom = s_zoom;
                                    const bool mouseActive = io.MouseDelta.x != 0.0f ||
                                                             io.MouseDelta.y != 0.0f;
                                    bool selMoved = false;
                                    bool didHome  = false;
                                    const float canvasW =
                                        std::max(s_spanX * unit + pad * 2.0f, childSz.x - 4.0f);
                                    const float canvasH =
                                        std::max(s_spanY * unit + pad * 2.0f, childSz.y - 4.0f);
                                    const ImVec2 origin = ImGui::GetCursorScreenPos();
                                    ImGui::Dummy(ImVec2(canvasW, canvasH));   // the scrollable extent
                                    bool wantNodePopup = false;

                                    // Canvas-local / screen position of a vis
                                    // entry — PURE GEOMETRY, valid for culled
                                    // nodes too (the P1 requirement). The
                                    // tree is centred in whichever canvas
                                    // dimension exceeds its span.
                                    const float xOff = (canvasW - s_spanX * unit) * 0.5f;
                                    const float yOff = (canvasH - s_spanY * unit) * 0.5f;
                                    auto lpos = [&](int k) {
                                        return ImVec2(xOff + s_visPos[k].x * unit,
                                                      yOff + s_visPos[k].y * unit);
                                    };
                                    auto spos = [&](int k) {
                                        const ImVec2 l = lpos(k);
                                        return ImVec2(origin.x + l.x, origin.y + l.y);
                                    };

                                    if (s_scrollHome) {
                                        // Land on the ROOT row (bottom),
                                        // centred. didHome mutes this frame's
                                        // follow-scroll — SetScroll lands
                                        // NEXT frame, so a same-frame clamp
                                        // reading stale scroll would fight it
                                        // (the round-3 open-jitter).
                                        s_scrollHome = false;
                                        didHome = true;
                                        ImGui::SetScrollY(canvasH);
                                        ImGui::SetScrollX(std::max(0.0f, (canvasW - childSz.x) * 0.5f));
                                    }

                                    // ── P1 EXPLICIT NAV ─────────────────
                                    // Deterministic nearest-in-direction over
                                    // the laid-out tree: forward projection
                                    // plus a lateral penalty, minimum score
                                    // wins. Reads d-pad (the input hook also
                                    // folds the left stick into these),
                                    // arrows, and the LStick keys for good
                                    // measure. Never touches ImGui nav.
                                    const bool nodePopupOpen = ImGui::IsPopupOpen("##pnode");
                                    if (!nodePopupOpen) {
                                    if (!s_vis.empty()) {
                                        if (s_selNode < 0 || s_selNode >= (int)s_vis.size())
                                            s_selNode = 0;
                                        auto down3 = [](ImGuiKey a, ImGuiKey b, ImGuiKey c) {
                                            return ImGui::IsKeyPressed(a, true) ||
                                                   ImGui::IsKeyPressed(b, true) ||
                                                   ImGui::IsKeyPressed(c, true);
                                        };
                                        int dx = 0, dy = 0;
                                        if (down3(ImGuiKey_GamepadDpadUp, ImGuiKey_UpArrow,
                                                  ImGuiKey_GamepadLStickUp))         dy = -1;
                                        else if (down3(ImGuiKey_GamepadDpadDown, ImGuiKey_DownArrow,
                                                       ImGuiKey_GamepadLStickDown))  dy = +1;
                                        else if (down3(ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow,
                                                       ImGuiKey_GamepadLStickLeft))  dx = -1;
                                        else if (down3(ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow,
                                                       ImGuiKey_GamepadLStickRight)) dx = +1;
                                        // Throttled to ~8 hops/s — ImGui's raw
                                        // key-repeat (20/s) overshot the
                                        // intended node constantly.
                                        if ((dx != 0 || dy != 0) &&
                                            ImGui::GetTime() - s_lastMoveT >= 0.12) {
                                            const ImVec2 cur = s_visPos[s_selNode];
                                            int best = -1;
                                            float bestScore = 1e9f;
                                            for (int k = 0; k < (int)s_vis.size(); ++k) {
                                                if (k == s_selNode) continue;
                                                const float vx = s_visPos[k].x - cur.x;
                                                const float vy = s_visPos[k].y - cur.y;
                                                const float proj = vx * (float)dx + vy * (float)dy;
                                                if (proj < 0.05f) continue;   // behind / abreast
                                                const float lat = std::fabs(vx * (float)dy) +
                                                                  std::fabs(vy * (float)dx);
                                                const float score = proj + 2.5f * lat;
                                                if (score < bestScore) { bestScore = score; best = k; }
                                            }
                                            if (best >= 0) {
                                                s_selNode   = best;
                                                s_lastMoveT = ImGui::GetTime();
                                                selMoved    = true;
                                            }
                                        }
                                        // A / Enter / E → the detail popup for
                                        // the selection. Single-path: no nav
                                        // items exist here, so ImGui cannot
                                        // also act on the same press. The
                                        // aGuard swallows the press that
                                        // OPENED this window until released.
                                        if (s_aGuard) {
                                            if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) &&
                                                !ImGui::IsKeyDown(ImGuiKey_Enter))
                                                s_aGuard = false;
                                        } else if (s_selNode >= 0 &&
                                            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
                                            s_nodeTree = s_skillCur;
                                            s_nodeIdx  = s_vis[s_selNode];
                                            wantNodePopup = true;
                                        }
                                    } else if (s_aGuard &&
                                               !ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) &&
                                               !ImGui::IsKeyDown(ImGuiKey_Enter)) {
                                        s_aGuard = false;   // release clears even over an empty tree
                                    }
                                        // Pad-reachable window controls — OUTSIDE
                                        // the empty-tree gate on purpose: an
                                        // all-filtered tree must still answer
                                        // [Y]/[View]/zoom or the player is stuck.
                                        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, true))
                                            s_zoom = std::max(0.5f, s_zoom - 0.15f);
                                        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, true))
                                            s_zoom = std::min(2.0f, s_zoom + 0.15f);
                                        if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false))
                                            s_showMarginal = !s_showMarginal;
                                        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
                                            int t2 = s_skillCur;
                                            for (int n2 = 0; n2 < (int)cat.skills.size(); ++n2) {
                                                t2 = (t2 + 1) % (int)cat.skills.size();
                                                if (!cat.skills[t2].nodes.empty()) break;
                                            }
                                            if (t2 != s_skillCur) s_skillCur = t2;
                                        }
                                    }
                                    // FOLLOW-SCROLL — on INTENT only (pad
                                    // move / zoom / filter reflow), never
                                    // continuously: a permanent clamp made
                                    // the mouse wheel unusable (it yanked
                                    // the view back every frame). Geometry-
                                    // driven, so it scrolls TO culled nodes.
                                    if (!didHome && (selMoved || zoomChanged || s_reclamp) &&
                                        s_selNode >= 0 && s_selNode < (int)s_vis.size()) {
                                        s_reclamp = false;
                                        const ImVec2 l = lpos(s_selNode);
                                        if (zoomChanged) {
                                            // Zoom re-centres ON the selection
                                            // — the layout scales under it,
                                            // and losing the node you were
                                            // looking at felt broken.
                                            ImGui::SetScrollX(std::max(0.0f, l.x - childSz.x * 0.5f));
                                            ImGui::SetScrollY(std::max(0.0f, l.y - childSz.y * 0.5f));
                                        } else {
                                            const float m = 90.0f * s_zoom;
                                            const float sx = ImGui::GetScrollX();
                                            const float sy = ImGui::GetScrollY();
                                            if (l.x < sx + m)
                                                ImGui::SetScrollX(std::max(0.0f, l.x - m));
                                            else if (l.x > sx + childSz.x - m)
                                                ImGui::SetScrollX(l.x - childSz.x + m);
                                            if (l.y < sy + m)
                                                ImGui::SetScrollY(std::max(0.0f, l.y - m));
                                            else if (l.y > sy + childSz.y - m)
                                                ImGui::SetScrollY(l.y - childSz.y + m);
                                        }
                                    } else if (didHome || s_reclamp) {
                                        // Home consumed any pending reclamp.
                                        s_reclamp = false;
                                    }

                                    const ImVec2 winPos = ImGui::GetWindowPos();
                                    const float cullPad = unit;   // one node gap of headroom
                                    const float visX0 = winPos.x - cullPad;
                                    const float visX1 = winPos.x + childSz.x + cullPad;
                                    const float visY0 = winPos.y - cullPad;
                                    const float visY1 = winPos.y + childSz.y + cullPad;

                                    auto* dl = ImGui::GetWindowDrawList();
                                    const ImU32 cDim    = ImGui::GetColorU32(ImGuiCol_Border);
                                    const ImU32 cText   = ImGui::GetColorU32(ImGuiCol_Text);
                                    const ImU32 cFaint  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                                    const ImU32 cBtn    = ImGui::GetColorU32(ImGuiCol_Button);
                                    const ImU32 cSel    = ImGui::GetColorU32(ImGuiCol_Header);
                                    const ImU32 cAccent = ImGui::GetColorU32(skin.accent);
                                    // Soft variants for the tapered edge
                                    // underlay and the available-node glow
                                    // (round 5 presentation: constellation,
                                    // not flat scatter).
                                    const ImU32 cAccentSoft = ImGui::GetColorU32(ImVec4(
                                        skin.accent.x, skin.accent.y, skin.accent.z, 0.30f));
                                    const ImVec4 borV = ImGui::GetStyleColorVec4(ImGuiCol_Border);
                                    const ImU32 cDimSoft = ImGui::GetColorU32(ImVec4(
                                        borV.x, borV.y, borV.z, 0.22f));

                                    // Everything filtered (a tree of nothing
                                    // but marginal perks): SAY so instead of
                                    // drawing an empty canvas (round 4).
                                    if (s_vis.empty()) {
                                        const char* msg =
                                            "Nothing in this tree is useful to a follower -- "
                                            "[View] shows marginal perks.";
                                        const ImVec2 ts = ImGui::CalcTextSize(msg);
                                        dl->AddText(ImVec2(winPos.x + (childSz.x - ts.x) * 0.5f,
                                                           winPos.y + (childSz.y - ts.y) * 0.5f),
                                                    cFaint, msg);
                                    }

                                    // EDGES (under the discs), vis→vis. The
                                    // filter keeps every parent of a kept
                                    // node (the needed-fixpoint), so edges
                                    // never dangle; still AABB-culled.
                                    // Round-5 presentation: vertically-eased
                                    // cubic curves with a wide soft underlay
                                    // beneath a thin core — the tapered
                                    // constellation look instead of raw
                                    // straight lines (lateral edges ease to
                                    // straight on their own: the bend scales
                                    // with the vertical run).
                                    for (int k = 0; k < (int)s_vis.size(); ++k) {
                                        const int i = s_vis[k];
                                        const auto& n = tree.nodes[i];
                                        if (n.parentIndices.empty()) continue;
                                        const ImVec2 p1 = spos(k);
                                        const auto* sc = stateAt(flatBase + i);
                                        const bool childOwned = sc && (sc->ownedRank > 0 || sc->native);
                                        for (const auto pi : n.parentIndices) {
                                            const int vk = s_visOf[(int)pi];
                                            if (vk < 0) continue;
                                            const ImVec2 p0 = spos(vk);
                                            if (std::max(p0.x, p1.x) < visX0 ||
                                                std::min(p0.x, p1.x) > visX1 ||
                                                std::max(p0.y, p1.y) < visY0 ||
                                                std::min(p0.y, p1.y) > visY1)
                                                continue;
                                            const auto* sp = stateAt(flatBase + (int)pi);
                                            const bool bothOwned = childOwned && sp &&
                                                (sp->ownedRank > 0 || sp->native);
                                            const float bend = 0.38f * (p1.y - p0.y);
                                            const ImVec2 c0(p0.x, p0.y + bend);
                                            const ImVec2 c1(p1.x, p1.y - bend);
                                            const float core = bothOwned ? 2.5f : 1.4f;
                                            dl->AddBezierCubic(p0, c0, c1, p1,
                                                bothOwned ? cAccentSoft : cDimSoft,
                                                core * 2.6f);
                                            dl->AddBezierCubic(p0, c0, c1, p1,
                                                bothOwned ? cAccent : cDim, core);
                                        }
                                    }

                                    // NODES: owned = accent disc; available =
                                    // bright-ringed; locked = dim; native =
                                    // accent ring on a dim disc (§4.1
                                    // boundary); PASSTHROUGH (marginal kept
                                    // for connectivity) = smaller + dim. The
                                    // InvisibleButtons are MOUSE-ONLY (NoNav
                                    // is pushed): hover steers s_selNode,
                                    // click opens the popup — the same state
                                    // the pad drives, no second path.
                                    const float R = std::max(6.0f, 10.0f * s_zoom);
                                    for (int k = 0; k < (int)s_vis.size(); ++k) {
                                        const int i = s_vis[k];
                                        const auto& n = tree.nodes[i];
                                        const ImVec2 p = spos(k);
                                        if (p.x < visX0 || p.x > visX1 ||
                                            p.y < visY0 || p.y > visY1)
                                            continue;   // culled — geometry above still saw it
                                        const auto* st = stateAt(flatBase + i);
                                        const bool owned    = st && st->ownedRank > 0;
                                        const bool native   = st && st->native;
                                        const bool avail    = st && st->available;
                                        const bool passthru = s_visPass[k] != 0;
                                        const bool selected = (k == s_selNode);
                                        const float r = passthru ? R * 0.72f : R;

                                        ImGui::PushID(k);
                                        ImGui::SetCursorScreenPos(
                                            ImVec2(p.x - R - 4.0f, p.y - R - 4.0f));
                                        const bool clicked = ImGui::InvisibleButton(
                                            "##nd", ImVec2((R + 4.0f) * 2.0f, (R + 4.0f) * 2.0f));
                                        const bool hovered = ImGui::IsItemHovered();
                                        ImGui::PopID();

                                        // Available nodes get a soft outer
                                        // glow — the "you can take this"
                                        // read at a glance (round 5).
                                        if (avail)
                                            dl->AddCircle(p, r + 3.5f, cAccentSoft, 0, 5.0f);
                                        dl->AddCircleFilled(p, r,
                                            owned ? cAccent : avail ? cSel : cBtn);
                                        dl->AddCircle(p, r,
                                            passthru ? cDim
                                                     : (owned || native) ? cAccent
                                                       : avail ? cText : cDim,
                                            0, (avail || native) ? 2.5f : 1.5f);
                                        // THE selection ring — ours, drawn
                                        // every frame the node is on screen.
                                        if (selected)
                                            dl->AddCircle(p, r + 5.0f, cAccent, 0, 3.5f);

                                        if (s_zoom >= 0.7f || selected) {
                                            const ImVec2 ts = ImGui::CalcTextSize(n.name.c_str());
                                            dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y + r + 4.0f),
                                                        owned ? cAccent
                                                              : (avail || selected) ? cText : cFaint,
                                                        n.name.c_str());
                                            if (n.ranks.size() > 1 || owned) {
                                                const std::string rk = std::format("{}/{}",
                                                    st ? (int)st->ownedRank : 0, (int)n.ranks.size());
                                                const ImVec2 rs = ImGui::CalcTextSize(rk.c_str());
                                                dl->AddText(ImVec2(p.x - rs.x * 0.5f,
                                                                   p.y - r - rs.y - 2.0f),
                                                            owned ? cAccent : cFaint, rk.c_str());
                                            }
                                        }

                                        if (hovered) {
                                            // Mouse steers the SAME selection —
                                            // but only while it MOVES; a parked
                                            // deck cursor must not snap the pad's
                                            // selection back every frame.
                                            if (mouseActive) s_selNode = k;
                                            if (owned)
                                                ImGui::SetTooltip("%s -- rank %d/%d (allocated by MFO)",
                                                    n.name.c_str(), (int)st->ownedRank,
                                                    (int)n.ranks.size());
                                            else if (native)
                                                ImGui::SetTooltip("%s -- granted by your load order",
                                                    n.name.c_str());
                                            else if (avail)
                                                ImGui::SetTooltip("%s -- [A] take rank %d (1 point)",
                                                    n.name.c_str(), (int)st->ownedRank + 1);
                                            else if (st && !st->whyNot.empty())
                                                ImGui::SetTooltip("%s -- locked: %s",
                                                    n.name.c_str(), st->whyNot.c_str());
                                            else
                                                ImGui::SetTooltip("%s", n.name.c_str());
                                        }
                                        if (clicked) {
                                            s_selNode  = k;
                                            s_nodeTree = s_skillCur;   // catalog index
                                            s_nodeIdx  = i;
                                            wantNodePopup = true;   // OpenPopup outside PushID
                                        }
                                    }

                                    // NoNav ends here — the detail popup's
                                    // Selectables use ImGui nav (listPopup
                                    // pattern, deck-proven).
                                    ImGui::PopItemFlag();

                                    // ── NODE POPUP (A on a node) ────────
                                    // Available → confirm-take; locked → the
                                    // unmet requirement, never a confirm
                                    // (§5's UI leg). B closes via the popup
                                    // cascade like every other picker.
                                    if (wantNodePopup) ImGui::OpenPopup("##pnode");
                                    ImGui::SetNextWindowPos(
                                        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                                    if (ImGui::BeginPopup("##pnode")) {
                                        pickerDrawnThisFrame = true;
                                        if (s_nodeTree >= 0 && s_nodeTree < (int)cat.skills.size() &&
                                            s_nodeIdx >= 0 &&
                                            s_nodeIdx < (int)cat.skills[s_nodeTree].nodes.size()) {
                                            const auto& nd = cat.skills[s_nodeTree].nodes[s_nodeIdx];
                                            std::size_t fb = 0;
                                            for (int t = 0; t < s_nodeTree; ++t)
                                                fb += cat.skills[t].nodes.size();
                                            const auto* stv = stateAt(fb + s_nodeIdx);
                                            const int ownedR = stv ? (int)stv->ownedRank : 0;

                                            ImGui::PushFont(g_fontHead);
                                            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                            ImGui::TextUnformatted(nd.name.c_str());
                                            ImGui::PopStyleColor();
                                            ImGui::PopFont();
                                            // Field fix #1: the point budget is
                                            // visible IN the take dialog too.
                                            ImGui::TextColored(skin.accent,
                                                "%.0f perk point(s) available",
                                                who->unspentPerk);
                                            if (nd.verdict == Progression::Verdict::kMarginal)
                                                ImGui::TextDisabled("marginal for followers");
                                            if (!nd.description.empty()) {
                                                ImGui::PushTextWrapPos(
                                                    ImGui::GetCursorPosX() + 380.0f);
                                                ImGui::TextUnformatted(nd.description.c_str());
                                                ImGui::PopTextWrapPos();
                                            }
                                            for (int rr = 0; rr < (int)nd.ranks.size(); ++rr)
                                                ImGui::TextDisabled("rank %d: %s%s", rr + 1,
                                                    nd.ranks[rr].skillReq.empty()
                                                        ? "no skill requirement"
                                                        : nd.ranks[rr].skillReq.c_str(),
                                                    rr < ownedR ? "  [owned]" : "");
                                            ImGui::Separator();
                                            if (stv && stv->native) {
                                                ImGui::TextDisabled("Granted by your load order -- "
                                                                    "MFO leaves it untouched.");
                                            } else if (ownedR > 0 &&
                                                       ownedR >= (int)nd.ranks.size()) {
                                                ImGui::TextColored(skin.accent,
                                                    "Fully allocated (%d/%d).",
                                                    ownedR, (int)nd.ranks.size());
                                            } else if (stv && stv->available) {
                                                const std::string take = std::format(
                                                    "Take rank {}  (1 perk point)", ownedR + 1);
                                                if (ImGui::Selectable(take.c_str())) {
                                                    EditCmd e{ EditKind::AddonAction, s_psel,
                                                               0, 0u, 0.0f };
                                                    e.verbId = (int)AddonVerb::AllocPerk;
                                                    e.perk = nd.perkFormID;
                                                    QueueEdit(e);
                                                    ImGui::CloseCurrentPopup();
                                                }
                                                ImGui::SetItemDefaultFocus();
                                            } else if (stv && !stv->whyNot.empty()) {
                                                ImGui::TextWrapped("Locked: %s",
                                                                   stv->whyNot.c_str());
                                            } else {
                                                ImGui::TextDisabled(
                                                    stateOk ? "Locked." : "syncing...");
                                            }
                                            ImGui::TextDisabled("[B]/Esc close");
                                        }
                                        ImGui::EndPopup();
                                    }
                                }
                                ImGui::EndChild();
                                }   // valid-pick else
                                ImGui::EndPopup();
                            }

                            // ── FOOTER: RESPEC + HINTS ──────────────────
                            if (ImGui::Button("Respec")) ImGui::OpenPopup("##prespec");
                            ImGui::SameLine();
                            ImGui::TextDisabled("refund all perks, -%.0f rapport  |  d-pad move   "
                                                "[A] open skill tree   [B] back",
                                                prog.respecRapportCost);
                            ImGui::SetNextWindowPos(
                                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                            if (ImGui::BeginPopup("##prespec")) {
                                pickerDrawnThisFrame = true;
                                ImGui::PushFont(g_fontHead);
                                ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                ImGui::TextUnformatted("Respec?");
                                ImGui::PopStyleColor();
                                ImGui::PopFont();
                                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
                                ImGui::TextWrapped("Every perk MFO allocated to %s is removed and "
                                                   "its points refunded. They will resent the "
                                                   "reset: -%.0f rapport.",
                                                   who->name.c_str(), prog.respecRapportCost);
                                ImGui::PopTextWrapPos();
                                ImGui::Separator();
                                ImGui::PushStyleColor(ImGuiCol_Text, skin.danger);
                                if (ImGui::Selectable("Confirm respec")) {
                                    EditCmd e{ EditKind::AddonAction, s_psel, 0, 0u, 0.0f };
                                    e.verbId = (int)AddonVerb::Respec;
                                    QueueEdit(e);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::PopStyleColor();
                                if (ImGui::Selectable("Cancel")) ImGui::CloseCurrentPopup();
                                ImGui::SetItemDefaultFocus();   // land on Cancel, not the danger row
                                ImGui::EndPopup();
                            }
                        }

                        ImGui::EndTabItem();
                    }
                }
    }

}
