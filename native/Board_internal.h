#pragma once
// Board_internal.h -- the Board family's SHARED substrate. One TU (Board.cpp)
// used to hold all of this in a single anonymous namespace; the mechanical
// module split (Board.cpp / Board_Progression.cpp) moved the cross-module
// state, types, and small helpers here as `inline` (ONE shared instance across
// the TUs -- never per-TU copies), and declares the cross-module draw entry
// next to the module that defines it. Single-module state stays file-local in
// its module. NOT a public API: only the Board*.cpp TUs may include this.
// (Same pattern as Logistics_internal.h.)

#include "PCH.h"
#include "Board.h"

#include <imgui.h>

#include <mutex>
#include <vector>

namespace MFO::Board {

    // ── EDIT COMMAND QUEUE ──────────────────────────────────────────────────
    // The board draws on the RENDER thread; the tables live on the main thread.
    // So an edit is enqueued from the draw and APPLIED in PublishSnapshot,
    // which runs on the main thread (#2, the same rule as everything touching
    // g_followers). Never write a rule table from a draw call.
    // SetCond/SetAct are ABSOLUTE picks (the FFXII list-picker chooses an index
    // directly, rather than cycling): c.param carries the index into this
    // table's condition/action vocabulary. Cycle* remain for any legacy caller.
    enum class EditKind : std::uint8_t { Add, Del, MoveUp, MoveDown, Toggle,
                                         CycleCond, CycleAct, SetParam, SetSpell,
                                         SetCond, SetAct, TeachSpell,
                                         SetSubject, SetSubjectActor,   // #68
                                         SetClassOverride,   // #65
                                         SetMfoEnabled,      // #78 per-follower MFO switch
                                         AddonAction };   // v1.1 Phase 6c: ONE generic
                                                            // add-on-verb carrier. The specific
                                                            // verb rides EditCmd::verbId (an
                                                            // add-on-agnostic int) — EditKind no
                                                            // longer enumerates progression verbs.
    // v1.1 Phase 6c: the verbs an EditKind::AddonAction can carry. The CARRIER
    // (EditKind/EditCmd) is add-on-agnostic; these ids + the dispatch in
    // ApplyEdits stay progression-shaped until Phase 7/9 routes verbs through
    // the manifest. Args reuse the existing EditCmd fields: `fid` = the follower,
    // `perk` = a form arg (class-def / catalog node), `param` = an int/bool arg
    // (manual toggle 0/1 | AV ordinal) — the {verbId, follower, arg-union} shape.
    enum class AddonVerb : int { SetClass = 0, AllocPerk, Respec,
                                 SetManual, ApplySkillPoint };
    struct EditCmd {
        EditKind kind; RE::FormID fid; int table; std::uint32_t uid; float param;
        RE::FormID spell = 0;
        RE::FormID book  = 0;   // #4 TeachSpell: the spellbook to consume
        RE::FormID subjectActor = 0;   // #68 SetSubjectActor: the specific follower
        RE::FormID perk  = 0;   // AddonAction form arg (class-def / catalog node)
        int        verbId = 0;  // v1.1 Phase 6c: AddonVerb for EditKind::AddonAction
    };
    // #68: written into a Gambit's subjectSelector when SetSubjectActor picks
    // a SPECIFIC follower, so a stale subject value from an earlier Self/
    // Player/NearestAlly pick cannot linger and mislead. Deliberately OUTSIDE
    // Vocab::Subject's real range (0/1/2) -- ResolveCastTarget's switch falls
    // into its `default:` case (Self) if the specific actor ever becomes
    // unavailable, exactly the same graceful demotion an unrecognised value
    // would get anyway.
    constexpr std::uint8_t kSubjectSpecificFollower = 0xFF;
    inline std::mutex          g_editMx;
    inline std::vector<EditCmd> g_edits;

    inline void QueueEdit(EditCmd c) { std::scoped_lock lk(g_editMx); g_edits.push_back(c); }

    // Header font (drawn titles / accent headers). Loaded by Board.cpp's
    // D3DInitHook; read by both draw TUs. Null -> ImGui default font.
    inline ImFont* g_fontHead = nullptr;

    // ── SKINS (DESIGN §6.7a, standing family rule) ──────────────────────
    // Four named skins, palettes copied VERBATIM from MEO's kSkins so MFO
    // is the same brand, not merely similar. Square corners, flat fills --
    // ImGui's honest range, closer to Skyrim than its debug grey.
    struct MenuSkin {
        const char* name;
        ImVec4 winBg, panel, border, text, dim, sel, accent, btn, track, danger;
        bool   sans;
        const char* title;
    };
    inline constexpr MenuSkin kSkins[4] = {
        { "Ebony & Brass",
          { 0.04f,0.04f,0.06f,0.98f }, { 0.07f,0.07f,0.10f,0.98f },
          { 0.55f,0.48f,0.27f,0.60f }, { 0.91f,0.89f,0.84f,1.00f },
          { 0.58f,0.55f,0.47f,1.00f }, { 0.34f,0.29f,0.16f,0.85f },
          { 0.78f,0.70f,0.45f,1.00f }, { 0.13f,0.11f,0.07f,0.90f },
          { 1.00f,1.00f,1.00f,0.08f }, { 0.76f,0.29f,0.24f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Dwemer Parchment",
          { 0.92f,0.88f,0.80f,0.99f }, { 0.95f,0.92f,0.85f,1.00f },
          { 0.54f,0.45f,0.25f,0.85f }, { 0.21f,0.17f,0.12f,1.00f },
          { 0.48f,0.43f,0.34f,1.00f }, { 0.86f,0.81f,0.66f,1.00f },
          { 0.43f,0.29f,0.16f,1.00f }, { 0.89f,0.84f,0.72f,1.00f },
          { 0.00f,0.00f,0.00f,0.10f }, { 0.55f,0.23f,0.18f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Soul Cairn",
          { 0.07f,0.06f,0.13f,0.98f }, { 0.10f,0.08f,0.19f,0.98f },
          { 0.35f,0.31f,0.55f,0.70f }, { 0.85f,0.84f,0.92f,1.00f },
          { 0.55f,0.52f,0.66f,1.00f }, { 0.16f,0.14f,0.31f,0.90f },
          { 0.53f,0.85f,0.92f,1.00f }, { 0.13f,0.10f,0.23f,0.90f },
          { 1.00f,1.00f,1.00f,0.08f }, { 0.76f,0.29f,0.24f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Quicksilver",
          { 0.04f,0.05f,0.06f,0.94f }, { 0.07f,0.08f,0.10f,0.96f },
          { 0.22f,0.25f,0.29f,1.00f }, { 0.83f,0.85f,0.88f,1.00f },
          { 0.47f,0.50f,0.54f,1.00f }, { 0.14f,0.19f,0.23f,0.90f },
          { 0.56f,0.72f,0.80f,1.00f }, { 0.09f,0.11f,0.13f,0.90f },
          { 1.00f,1.00f,1.00f,0.07f }, { 0.76f,0.29f,0.24f,1.00f },
          true, "F O L L O W E R   O V E R H A U L" },
    };

    // ── THE PROGRESSION TAB (#74 component 3) ───────────────────────────
    // Defined in Board_Progression.cpp (mechanical split of DrawFieldKit's
    // hosted-tab body). Render thread, inside DrawFieldKit's tab bar. The
    // reference params are DrawFieldKit locals/statics the body reads or
    // writes in place: s_tab (the tab-cycle static), pickerDrawnThisFrame
    // (drives the footer hint). tabSelFlags carries tabSel(2).
    void DrawProgressionTab(const Snapshot& snap, bool progActive,
                            ImGuiTabItemFlags tabSelFlags, ImGuiIO& io,
                            const MenuSkin& skin, bool r1Ready,
                            int& s_tab, bool& pickerDrawnThisFrame);

}
