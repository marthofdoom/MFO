#include "PCH.h"
// Board_Click.cpp -- press-latched mouse clicks for the board's clickables.
//
// WHY: the board's cursor is built from the engine's raw mouse DELTAS
// (Board.cpp kMouseMove), so it can slip a few pixels between press and
// release. ImGui's own Button/Selectable fire only when the release lands on
// the item, so those clicks were silently lost (ported from MEO badab6e's
// pending-press model, which fixed the same field complaint).
//
// HOW: a mouse PRESS on an item records {id, rect}; a left-button RELEASE
// within a small slop of that rect fires the click even if ImGui itself did
// not (the item was left, so its own return stayed false). A drag well away
// still cancels. ImGui's own return stays authoritative whenever it is true,
// so gamepad/keyboard activation (nav) is untouched: the latch is armed only
// while the left mouse button is down. RENDER THREAD ONLY (inside the draw).

#include <imgui.h>
#include <imgui_internal.h>   // GetCurrentWindowRead (popup auto-close parity with ImGui::Selectable)

#include "Board_internal.h"

namespace MFO::Board::Click {

    namespace {
        struct Latch {
            bool    active = false;
            ImGuiID id = 0;
            ImVec2  mn{}, mx{};
        };
        Latch g_latch;

        // a_native = ImGui's own return for the item just submitted.
        // Returns true when the click should fire; sets a_viaLatch when only
        // the latch (not ImGui) produced it.
        bool Resolve(bool a_native, bool& a_viaLatch) {
            a_viaLatch = false;
            const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            // No button down and no release this frame: a stale press (board
            // closed / item gone mid-press). Drop it.
            if (g_latch.active && !released && !down) g_latch.active = false;
            const ImGuiID id = ImGui::GetItemID();
            if (a_native) {
                if (g_latch.active && g_latch.id == id) g_latch.active = false;
                return true;
            }
            if (down && ImGui::IsItemActivated()) {
                g_latch = { true, id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
                return false;
            }
            if (g_latch.active && released && g_latch.id == id) {
                g_latch.active = false;
                const float  slop = 24.0f * ImGui::GetIO().FontGlobalScale;
                const ImVec2 m = ImGui::GetMousePos();
                if (m.x >= g_latch.mn.x - slop && m.x <= g_latch.mx.x + slop &&
                    m.y >= g_latch.mn.y - slop && m.y <= g_latch.mx.y + slop) {
                    a_viaLatch = true;
                    return true;
                }
            }
            return false;
        }
    }

    bool Button(const char* a_label, const ImVec2& a_size) {
        bool v;
        return Resolve(ImGui::Button(a_label, a_size), v);
    }

    bool SmallButton(const char* a_label) {
        bool v;
        return Resolve(ImGui::SmallButton(a_label), v);
    }

    bool InvisibleButton(const char* a_id, const ImVec2& a_size, ImGuiButtonFlags a_flags) {
        bool v;
        return Resolve(ImGui::InvisibleButton(a_id, a_size, a_flags), v);
    }

    bool RadioButton(const char* a_label, bool a_active) {
        bool v;
        return Resolve(ImGui::RadioButton(a_label, a_active), v);
    }

    bool Checkbox(const char* a_label, bool* a_v) {
        bool viaLatch = false;
        const bool r = Resolve(ImGui::Checkbox(a_label, a_v), viaLatch);
        if (viaLatch) *a_v = !*a_v;   // ImGui did not toggle (release missed the box)
        return r;
    }

    bool Selectable(const char* a_label, bool a_selected, ImGuiSelectableFlags a_flags,
                    const ImVec2& a_size) {
        bool viaLatch = false;
        const bool r = Resolve(ImGui::Selectable(a_label, a_selected, a_flags, a_size), viaLatch);
        // ImGui closes the popup a Selectable lives in on its own click; a
        // latched click must do the same (same condition ImGui uses).
        if (viaLatch && !(a_flags & ImGuiSelectableFlags_DontClosePopups)) {
            const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
            if (w && (w->Flags & ImGuiWindowFlags_Popup)) ImGui::CloseCurrentPopup();
        }
        return r;
    }

}
