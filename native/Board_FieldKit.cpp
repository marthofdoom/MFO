#include "PCH.h"

// Board_FieldKit.cpp -- the Field-Orders board's FULL PANEL: DrawFieldKit, the
// whole tab bar (Followers / Gambits, plus the hosted add-on tab it delegates
// to Board_Progression.cpp) and the panel-only helpers it is the sole caller
// of (the skin push, the picker submenu predicates, the class-name labels, the
// spell hover tooltip). Mechanically split out of Board.cpp when that TU
// crossed the 2500-line cap -- a PURE MOVE, same seam and same pattern as the
// 2026-08-31 Board_Progression.cpp split (see Board_internal.h for the shared
// substrate).
//
// RENDER THREAD. It mutates NOTHING: every action queues an EditCmd that
// ApplyEdits (Board.cpp) re-posts to the MAIN thread, where the backend gate
// re-validates before any engine write (#4).

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <imgui.h>
#include <imgui_internal.h>   // nav state (NavWindow/NavLayer/NavId) for the cascaded B back-out

#include "Board.h"
#include "Board_internal.h"   // shared Board-family substrate (mechanical split)
#include "Progression.h"   // #74: the frozen catalog - lock-free render reads by contract
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "logistics/Logistics.h"   // IsLooting/IsTrading for the [C][L][T] activity glyphs
#include "logistics/Lotd.h"        // GambitOffered: the LOTD museum gambit is offered only while LOTD awareness is on
#include "Forms.h"
#include "State.h"
#include "Probe.h"
#include "ProgProbe.h"
#include "progression/ProgAllocator.h"
#include "MainThread.h"
#include "Vocabulary.h"
#include "Scheduler.h"
#include "Diagnostics.h"

namespace MFO::Board {

    namespace {

    // #65: the combat-class override picker's labels, index-matched to
    // FollowerState::combatClassOverride / CombatStyle::Stance (0=Auto,
    // 1=Melee, 2=Ranged, 3=Cast/Mage). Auto is the default -- "no override".
    // (display text: i18n keys, index-matched)
    inline constexpr Str::K kClassKeys[4] = { Str::K::Gb_ClassAuto, Str::K::Gb_ClassMelee,
                                              Str::K::Gb_ClassRanged, Str::K::Gb_ClassMage };
    inline const char* ClassName(int i) { return Str::Get(kClassKeys[std::clamp(i, 0, 3)]); }

    // "50%" / "3" / "500u" -- a rule's value as the player reads it. Display text.
    inline std::string ValueText(ParamKind pk, float param) {
        if (pk == ParamKind::Percent)
            return Str::Fmt(Str::K::Gb_FmtPct, { (int)(std::clamp(param, 0.0f, 1.0f) * 100.0f + 0.5f) });
        if (pk == ParamKind::Count) return std::to_string((int)(param + 0.5f));
        if (pk == ParamKind::Distance) return Str::Fmt(Str::K::Gb_FmtDist, { (int)(param + 0.5f) });
        return {};
    }

    // ── PICKER SUBMENU GROUPING ─────────────────────────────────────────
    // The list-picker popups below are flat scans over kCondsCombat /
    // kActsLogi. As those tables grew, "Foe: ..." conditions and the loot
    // actions needed grouping into nested submenus so the flat list stays
    // scannable. Grouping is PRESENTATION ONLY: condTab/actTab stay the
    // single flat source of truth (cycleIdx/labelFor/kindFor above, and the
    // co-save, never see a group) -- these predicates just say which entries
    // a picker should collapse into one nested-popup row.
    bool IsFoeCond(const char* op) {
        return op && std::strncmp(op, "cond.foe_", 9) == 0;
    }
    bool IsPotionLootAct(const char* op) {
        return op && (!std::strcmp(op, Vocab::kActLootPotions)      ||
                      !std::strcmp(op, Vocab::kActLootHealthPotion) ||
                      !std::strcmp(op, Vocab::kActLootStaminaPotion)||
                      !std::strcmp(op, Vocab::kActLootMagickaPotion));
    }
    bool IsMiscLootAct(const char* op) {
        return op && (!std::strcmp(op, Vocab::kActLootIngredients) ||
                      !std::strcmp(op, Vocab::kActLootSoulGems)    ||
                      !std::strcmp(op, Vocab::kActLootJewelry));
    }

        // Push the selected skin. Returns the count pushed so the caller pops
        // exactly that many (an unbalanced push/pop corrupts every later frame).
        int PushSkin() {
            const int i = std::clamp(Config::g_menuStyle.load(), 0, 3);
            const auto& sk = kSkins[i];
            auto& st = ImGui::GetStyle();
            st.WindowRounding = st.FrameRounding = st.PopupRounding =
                st.ChildRounding = st.TabRounding = st.GrabRounding =
                st.ScrollbarRounding = 0.0f;   // square, always
            // Roominess parity with MEO/MAO. Their ApplyMenuStyle sets these
            // exact values; MFO left them at ImGui's cramped defaults
            // (WindowPadding 8,8 / FramePadding 4,3 / ItemSpacing 8,4), which is
            // most of what read as "less polished". Set on the persistent style
            // like the rounding above -- MEO mutates GetStyle() directly, not
            // via push/pop, so there is nothing to balance.
            st.WindowBorderSize    = 1.0f;
            st.ChildBorderSize     = 1.0f;
            st.WindowPadding       = ImVec2(18.0f, 14.0f);
            st.ItemSpacing         = ImVec2(10.0f, 7.0f);
            st.FramePadding        = ImVec2(10.0f, 7.0f);
            st.ScrollbarSize       = 14.0f;
            st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
            int n = 0;
            auto col = [&](ImGuiCol c, const ImVec4& v){ ImGui::PushStyleColor(c, v); ++n; };
            col(ImGuiCol_WindowBg,        sk.winBg);
            col(ImGuiCol_ChildBg,         sk.panel);
            col(ImGuiCol_PopupBg,         sk.panel);
            col(ImGuiCol_Border,          sk.border);
            col(ImGuiCol_Text,            sk.text);
            col(ImGuiCol_TextDisabled,    sk.dim);
            col(ImGuiCol_FrameBg,         sk.btn);
            col(ImGuiCol_FrameBgHovered,  sk.sel);
            col(ImGuiCol_FrameBgActive,   sk.sel);
            col(ImGuiCol_Button,          sk.btn);
            col(ImGuiCol_ButtonHovered,   sk.sel);
            col(ImGuiCol_ButtonActive,    sk.accent);
            col(ImGuiCol_Header,          sk.sel);
            col(ImGuiCol_HeaderHovered,   sk.sel);
            col(ImGuiCol_HeaderActive,    sk.accent);
            col(ImGuiCol_TitleBg,         sk.panel);
            col(ImGuiCol_TitleBgActive,   sk.panel);
            col(ImGuiCol_Tab,             sk.btn);
            col(ImGuiCol_TabHovered,      sk.sel);
            col(ImGuiCol_TabActive,       sk.accent);
            col(ImGuiCol_TableHeaderBg,   sk.panel);
            col(ImGuiCol_TableRowBg,      sk.winBg);
            col(ImGuiCol_TableRowBgAlt,   sk.panel);
            col(ImGuiCol_CheckMark,       sk.accent);
            col(ImGuiCol_SliderGrab,      sk.accent);
            // MEO wires all three of these; MFO left them at ImGui's default
            // debug-grey, which clashed with every skin. Separator takes the
            // skin border; the scrollbar uses the skin's `track` field, which
            // was defined per-skin but never applied until now.
            col(ImGuiCol_Separator,       sk.border);
            col(ImGuiCol_ScrollbarBg,     sk.track);
            col(ImGuiCol_ScrollbarGrab,   ImVec4(sk.dim.x, sk.dim.y, sk.dim.z, 0.60f));
            col(ImGuiCol_SeparatorHovered,sk.accent);
            col(ImGuiCol_NavHighlight,    sk.accent);   // controller focus ring
            return n;
        }

        // Render the spell-picker hover tooltip from PRECOMPUTED values (no actor
        // read on the render thread, #4): the follower's magicka cost + the "what it
        // does" line. Call right after the entry's Selectable.
        inline void DrawSpellHoverTooltip(int a_magickaCost, const std::string& a_desc) {
            if (!ImGui::IsItemHovered()) return;
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(Str::Fmt(Str::K::Gb_MagickaCost, { a_magickaCost }).c_str());
            if (!a_desc.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_WhatItDoes));
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
                ImGui::TextUnformatted(a_desc.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTooltip();
        }

    }   // ── end of the panel-only helpers ─────────────────────────────

    // ── REMEMBERED WINDOW RECT (marth: "It should remember, yes") ─────────
    // The board's pos/size live in Data/SKSE/Plugins/MFO_UI.ini as FRACTIONS
    // of the display (resolution independent), game-root-relative like
    // MFO.log so MO2's VFS lands it in overwrite. RENDER THREAD ONLY: read
    // once lazily at the first open, written synchronously when the board
    // closes and only if the rect changed (tiny file, temp + rename so a
    // crash cannot leave half a file). Not the co-save, not the MCM store.
    namespace {
        constexpr const char* kUiPath = "Data/SKSE/Plugins/MFO_UI.ini";
        struct WinMem {
            bool  loaded = false;   // read attempted
            bool  have   = false;   // stored rect valid
            float x = 0, y = 0, w = 0, h = 0;      // fractions to restore
            bool  dirty = false;    // live rect differs from baseline
            float lx = 0, ly = 0, lw = 0, lh = 0;  // live fractions
        };
        WinMem s_mem;

        bool ParseF(const std::string& a_v, float& a_out) {
            float f = 0;
            const char* b = a_v.data();
            const char* e = b + a_v.size();
            const auto r = std::from_chars(b, e, f);
            if (r.ec != std::errc() || r.ptr != e || !std::isfinite(f)) return false;
            a_out = f;
            return true;
        }

        void LoadWinMem() {
            s_mem.loaded = true;
            std::ifstream in(kUiPath);
            if (!in) return;
            float v[4] = {}; bool got[4] = {}; int ver = 0;
            std::string line;
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                auto trim = [](std::string t) {
                    const auto a = t.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
                    if (a == std::string::npos) return std::string();
                    return t.substr(a, t.find_last_not_of(" \t\r\n") - a + 1);
                };
                const std::string k = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
                static const char* names[4] = {"boardX", "boardY", "boardW", "boardH"};
                for (int i = 0; i < 4; ++i)
                    if (k == names[i]) got[i] = ParseF(val, v[i]);
                if (k == "version") { float f; if (ParseF(val, f)) ver = static_cast<int>(f); }
            }
            if (ver != 1 || !(got[0] && got[1] && got[2] && got[3])) return;
            // Out of range -> centred default. x/y may be negative (a window
            // parked partly off the left/top edge); the restore clamp fixes it.
            if (v[0] < -1.0f || v[0] > 1.0f || v[1] < -1.0f || v[1] > 1.0f ||
                v[2] < 0.05f || v[2] > 1.0f || v[3] < 0.05f || v[3] > 1.0f) return;
            s_mem.x = v[0]; s_mem.y = v[1]; s_mem.w = v[2]; s_mem.h = v[3];
            s_mem.have = true;
        }

        void SaveWinMem() {
            // std::to_chars: locale-independent, so from_chars always reads it back.
            std::string buf = "version=1\n";
            const auto put = [&buf](const char* a_key, float a_v) {
                char num[32];
                const auto r = std::to_chars(num, num + sizeof(num), a_v, std::chars_format::fixed, 5);
                buf += a_key; buf += '='; buf.append(num, r.ptr); buf += '\n';
            };
            put("boardX", s_mem.lx); put("boardY", s_mem.ly);
            put("boardW", s_mem.lw); put("boardH", s_mem.lh);
            const std::string tmp = std::string(kUiPath) + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out) { spdlog::warn("[board] could not write {}", tmp); return; }
                out << buf;
                out.close();
                if (out.fail()) { spdlog::warn("[board] short write to {}", tmp); return; }
            }
            std::error_code ec;
            std::filesystem::rename(tmp, kUiPath, ec);   // replaces an existing file
            if (ec) { spdlog::warn("[board] rename {} failed: {}", tmp, ec.message()); return; }
            s_mem.x = s_mem.lx; s_mem.y = s_mem.ly; s_mem.w = s_mem.lw; s_mem.h = s_mem.lh;
            s_mem.have = true;
            s_mem.dirty = false;
        }
    }

    // Called EVERY frame from the Present thunk (render thread). Writes the
    // remembered rect once the board has closed. Cheap when nothing changed.
    void FlushBoardWindowMemory(bool a_open) {
        // ONE attempt per close: clear dirty first so a failed save is not
        // retried (and re-logged) every frame while the board stays closed.
        if (!a_open && s_mem.dirty) { s_mem.dirty = false; SaveWinMem(); }
    }


        void DrawFieldKit(const Snapshot& snap) {
            // Shout-key close already fires on the key's RELEASE with both edges
            // swallowed -- no trailing edge to leak -- so it needs no grace.
            if (g_wantClose.exchange(false)) { g_open.store(false); return; }

            // CASCADED BACK is decided at the END of this function, on the render
            // thread, off ImGui's OWN nav state (see the comment there). B steps
            // back through whatever nav layer we're in and closes the board only at
            // the true top. s_prevNavBusy carries last frame's "B has something to
            // back out of" verdict.
            static bool s_prevNavBusy = false;
            bool        pickerDrawnThisFrame = false;   // still drives the footer hint

            auto& io = ImGui::GetIO();

            // R1 opened the board (it casts the Field Orders power). Wait for that
            // opening press to be RELEASED before R1 counts as a party-switch, so
            // opening does not immediately advance the follower. IsKeyDown reflects
            // only edges fed while OPEN; the guard is belt-and-suspenders on top of
            // the fact that the opening DOWN was seen while closed and never fed.
            static bool s_r1Guard = false;
            if (g_justOpened.exchange(false)) {
                s_r1Guard = true; s_prevNavBusy = true;
                // STUCK-KEY SWEEP (v1.0.59). The press that CLOSED the last
                // session (B / Esc) never gets its release fed: the hook stops
                // feeding the instant g_open is false and the close grace
                // swallows the up-edge. With the hook as the only gamepad
                // source (the backend's absolute-state XInput poll used to
                // paper over this) that key stays down in io.KeysData, and
                // AddKeyEvent's duplicate filter would eat the NEXT session's
                // first press of it -- a dead first B after every B-close.
                // Clear keyboard+gamepad state once per open (mouse untouched;
                // same call WM_KILLFOCUS already uses). Render thread, under
                // g_ioMx via the present hook, so it can't tear the queue.
                io.ClearInputKeys();
            }
            if (s_r1Guard && !ImGui::IsKeyDown(ImGuiKey_GamepadR1)) s_r1Guard = false;
            const bool r1Ready = !s_r1Guard;

            if (!s_mem.loaded) LoadWinMem();
            if (s_mem.have) {
                // Restore the remembered rect: size from fractions clamped to
                // the constraint range, then the position clamped so the whole
                // window (so the whole title bar) stays on this display.
                const float w = std::clamp(s_mem.w * io.DisplaySize.x, 620.0f, std::max(620.0f, io.DisplaySize.x));
                const float h = std::clamp(s_mem.h * io.DisplaySize.y, 400.0f, std::max(400.0f, io.DisplaySize.y));
                const float x = std::clamp(s_mem.x * io.DisplaySize.x, 0.0f, std::max(0.0f, io.DisplaySize.x - w));
                const float y = std::clamp(s_mem.y * io.DisplaySize.y, 0.0f, std::max(0.0f, io.DisplaySize.y - h));
                ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Appearing);
                ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
            } else {
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.62f),
                                         ImGuiCond_Appearing);
            }
            ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 400.0f), io.DisplaySize);

            const int skinCols = PushSkin();
            const auto& skin = kSkins[std::clamp(Config::g_menuStyle.load(), 0, 3)];

            if (!ImGui::Begin(Str::Label(Str::K::Win_Title, "mfoboard"), nullptr,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::End();
                ImGui::PopStyleColor(skinCols);
                return;
            }

            // Remember the live rect (fractions of the display). The first frame
            // of an open is the baseline (what we just set), so merely opening
            // never dirties the file; any later move/resize does.
            if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
                const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
                const float fx = wp.x / io.DisplaySize.x, fy = wp.y / io.DisplaySize.y;
                const float fw = ws.x / io.DisplaySize.x, fh = ws.y / io.DisplaySize.y;
                const bool  differs = std::fabs(fx - s_mem.lx) > 0.0005f || std::fabs(fy - s_mem.ly) > 0.0005f ||
                                      std::fabs(fw - s_mem.lw) > 0.0005f || std::fabs(fh - s_mem.lh) > 0.0005f;
                const bool  matchesSaved = s_mem.have && std::fabs(fx - s_mem.x) <= 0.0005f &&
                    std::fabs(fy - s_mem.y) <= 0.0005f && std::fabs(fw - s_mem.w) <= 0.0005f &&
                    std::fabs(fh - s_mem.h) <= 0.0005f;
                if (ImGui::IsWindowAppearing()) { s_mem.lx = fx; s_mem.ly = fy; s_mem.lw = fw; s_mem.lh = fh; s_mem.dirty = false; }
                else if (differs) {
                    s_mem.lx = fx; s_mem.ly = fy; s_mem.lw = fw; s_mem.lh = fh;
                    s_mem.dirty = !matchesSaved;
                }
            }

            // Centered display title flanked by drawn rules -- MEO's signature
            // header (styled on MEO only now, per marth). The window's own title
            // bar carries the mod name ("Follower Overhaul"); this drawn header
            // names the FEATURE, "Field Orders", matching MFO_FieldOrdersPower.
            {
                auto*        dl    = ImGui::GetWindowDrawList();
                ImGui::PushFont(g_fontHead);   // header face (MEO parity); null -> default
                const char*  title = Str::Get(Str::K::Win_Header);
                const ImVec2 ts    = ImGui::CalcTextSize(title);
                const float  tx    = (ImGui::GetWindowSize().x - ts.x) * 0.5f;
                const ImVec2 wp    = ImGui::GetWindowPos();
                const float  ry    = wp.y + ImGui::GetCursorPosY() + ts.y * 0.5f;
                const ImU32  rule  = ImGui::GetColorU32(ImGuiCol_Separator);
                dl->AddLine(ImVec2(wp.x + 26.0f, ry), ImVec2(wp.x + tx - 18.0f, ry), rule);
                dl->AddLine(ImVec2(wp.x + tx + ts.x + 18.0f, ry),
                            ImVec2(wp.x + ImGui::GetWindowSize().x - 26.0f, ry), rule);
                ImGui::SetCursorPosX(tx);
                ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            ImGui::Spacing();
            ImGui::Separator();

            // TAB SWITCH moved OFF the shoulders: L1/R1 are now the FFXII
            // party-switch (change follower) inside the Gambits tab. The
            // View/Back pad button cycles the two panels instead -- ImGui nav
            // never binds GamepadBack, so nothing collides. A mouse click on a
            // tab still resyncs s_tab inside the opened body.
            static int s_tab = 0;
            static bool s_tabForce = false;   // apply SetSelected for ONE frame after an edge
            // #74 / v1.1 Phase 6a+b: the tab count is RUNTIME and HOST-DRIVEN — the
            // Field-Orders board hosts one tab per add-on that published a board-tab
            // view (snap.boardTabs, the GENERIC payload built off the manifest's
            // declared tabs), NOT a hardcoded progression case. Absent add-on = zero
            // hosted tabs = the tab is absent entirely (§1, the optional-addon
            // pattern), and the View-cycle must not step onto a tab that was never
            // emitted. A hosted tab counts only when its payload is `active`. (Today
            // one add-on, the progression tab; the body below still reads the concrete
            // payload — a later slice generalizes the widgets + label.)
            int hostedBoardTabs = 0;
            for (const auto& t : snap.boardTabs)
                if (t.active) ++hostedBoardTabs;
            const bool progActive = hostedBoardTabs > 0;
            const int kTabCount = progActive ? 3 : 2;   // Followers, Gambits[, hosted tab]
            if (s_tab >= kTabCount) s_tab = 0;   // the addon vanished under a stale pick
            // Gated like everything else: never while an item is being tweaked or
            // a picker popup is open.
            if (!ImGui::IsAnyItemActive() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false)) { s_tab = (s_tab + 1) % kTabCount; s_tabForce = true; }
            }
            // SetSelected ONLY on the frame after a shoulder press. Forcing it
            // every frame would re-assert s_tab and revert a mouse click on a
            // different tab (it flashed then snapped back). Between presses the
            // opened tab's body syncs s_tab, so mouse selection sticks.
            auto tabSel = [&](int i) -> ImGuiTabItemFlags {
                return (s_tabForce && s_tab == i) ? ImGuiTabItemFlags_SetSelected : 0;
            };

            if (ImGui::BeginTabBar("##tabs")) {

                if (ImGui::BeginTabItem(Str::Label(Str::K::Fk_Tab, "tabfollowers"), nullptr, tabSel(0))) {
                    s_tab = 0;
                    ImGui::TextDisabled("%s", Str::Fmt(Str::K::Fk_Tracked, { snap.rows.size() }).c_str());
                    ImGui::Spacing();

                    // Reserve the footer, or ScrollY takes the remaining height
                    // and pushes the hint line below the fold.
                    const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
                    if (ImGui::BeginTable("##followers", 8,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY,
                                          ImVec2(0.0f, -footer))) {
                        // T#78: the per-follower MFO master switch, FIRST column.
                        // ON = MFO manages him; OFF = MFO leaves him vanilla.
                        // Fixed columns are measured (FitW): the English widths are the
                        // floor, a longer translation widens its column instead of clipping.
                        // The State column also holds every state word the rows can show.
                        const char* colFollower = Str::Get(Str::K::Fk_ColFollower);
                        ImGui::TableSetupColumn("MFO",     ImGuiTableColumnFlags_WidthFixed, 34.0f);
                        ImGui::TableSetupColumn(colFollower, ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColState), ImGuiTableColumnFlags_WidthFixed,
                            FitW(110.0f, { Str::Get(Str::K::Fk_ColState), Str::Get(Str::K::Fk_StateRetained),
                                           Str::Get(Str::K::Fk_StateSummon), Str::Get(Str::K::Fk_StateCombat),
                                           Str::Get(Str::K::Fk_StateLooting), Str::Get(Str::K::Fk_StateTrading),
                                           Str::Get(Str::K::Fk_StateFollowing) }));
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColRapport), ImGuiTableColumnFlags_WidthFixed,
                            FitW(90.0f, { Str::Get(Str::K::Fk_ColRapport) }));
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColRank), ImGuiTableColumnFlags_WidthFixed,
                            FitW(50.0f, { Str::Get(Str::K::Fk_ColRank) }));
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColSlots), ImGuiTableColumnFlags_WidthFixed,
                            FitW(70.0f, { Str::Get(Str::K::Fk_ColSlots) }));
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColVitals), ImGuiTableColumnFlags_WidthFixed,
                            FitW(150.0f, { Str::Get(Str::K::Fk_ColVitals) }));
                        ImGui::TableSetupColumn(Str::Get(Str::K::Fk_ColDist), ImGuiTableColumnFlags_WidthFixed,
                            FitW(70.0f, { Str::Get(Str::K::Fk_ColDist) }));
                        ImGui::TableHeadersRow();

                        for (const auto& r : snap.rows) {
                            ImGui::TableNextRow();

                            // T#78: FIRST column -- the per-follower MFO toggle.
                            // A local mirror + QueueEdit on change: the render
                            // thread NEVER writes g_followers; the edit rides the
                            // main-thread ApplyEdits path like every other board
                            // edit (#4). Unique ImGui id per row via PushID(id).
                            ImGui::TableNextColumn();
                            ImGui::PushID(static_cast<int>(r.id));
                            bool en = r.mfoEnabled;
                            if (Click::Checkbox("##mfo", &en))
                                QueueEdit({ EditKind::SetMfoEnabled, r.id, 0, 0u,
                                            en ? 1.0f : 0.0f });
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", Str::Get(en ? Str::K::Fk_TipMfoOn
                                                                        : Str::K::Fk_TipMfoOff));
                            ImGui::PopID();

                            ImGui::TableNextColumn();
                            if (r.active) ImGui::TextUnformatted(r.name.c_str());
                            else          ImGui::TextDisabled("%s", r.name.c_str());
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%08X", r.id);
                            if (!r.active && !r.cell.empty()) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("- %s", r.cell.c_str());
                            }

                            ImGui::TableNextColumn();
                            if (!r.active) {
                                ImGui::TextDisabled("%s", Str::Get(Str::K::Fk_StateRetained));
                            } else if (r.commanded) {
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "%s", Str::Get(Str::K::Fk_StateSummon));
                            } else if (r.inCombat) {
                                // Same color code as the HUD [C][L][T] strip.
                                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", Str::Get(Str::K::Fk_StateCombat));
                            } else if (r.looting) {
                                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", Str::Get(Str::K::Fk_StateLooting));
                            } else if (r.trading) {
                                ImGui::TextColored(ImVec4(0.45f, 0.6f, 1.0f, 1.0f), "%s", Str::Get(Str::K::Fk_StateTrading));
                            } else {
                                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", Str::Get(Str::K::Fk_StateFollowing));
                            }

                            ImGui::TableNextColumn();
                            ImGui::Text("%u", r.rapport);

                            ImGui::TableNextColumn();
                            ImGui::Text("%u", r.rank);

                            ImGui::TableNextColumn();
                            ImGui::Text("%u / %u", r.combatRules, r.combatSlots);

                            ImGui::TableNextColumn();
                            if (r.active) {
                                // The three bars are the conditions the evaluator
                                // will read. Seeing them move is the point. The CURRENT
                                // value sits on each bar; hover for current / max
                                // (all precomputed on main into the snapshot -- #4).
                                const float w = 42.0f;
                                auto bar = [&](float pct, float cur, const ImVec4& col) {
                                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                                    ImGui::ProgressBar(pct, ImVec2(w, 0.0f),
                                                       std::to_string(static_cast<int>(cur + 0.5f)).c_str());
                                    ImGui::PopStyleColor();
                                };
                                ImGui::BeginGroup();
                                bar(r.healthPct,  r.healthCur,  ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                                ImGui::SameLine(0.0f, 3.0f);
                                bar(r.magickaPct, r.magickaCur, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
                                ImGui::SameLine(0.0f, 3.0f);
                                bar(r.staminaPct, r.staminaCur, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
                                ImGui::EndGroup();
                                if (ImGui::IsItemHovered()) {
                                    auto line = [](Str::K w, float cur, float mx) {
                                        return Str::Fmt(Str::K::Fk_TipVitalLine,
                                            { Str::Get(w), Str::Arg::F(cur, 0), Str::Arg::F(mx, 0) });
                                    };
                                    ImGui::SetTooltip("%s\n%s\n%s",
                                        line(Str::K::Word_Health,  r.healthCur,  r.healthMax).c_str(),
                                        line(Str::K::Word_Magicka, r.magickaCur, r.magickaMax).c_str(),
                                        line(Str::K::Word_Stamina, r.staminaCur, r.staminaMax).c_str());
                                }
                            } else {
                                ImGui::TextDisabled("--");
                            }

                            ImGui::TableNextColumn();
                            if (r.active) ImGui::Text("%.0f", r.distance);
                            else          ImGui::TextDisabled("--");
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }

                // ── THE GAMBIT EDITOR (M7) -- FFXII-faithful ────────────
                // Rebuilt to mirror Final Fantasy XII's Gambit screen: one
                // party member in context (L1/R1 switch), a dense numbered list
                // of ON/OFF gambit rows, and -- the signature FFXII interaction
                // -- NO dropdowns. Highlight a slot's condition or action, press
                // A, and a large full-height SCROLLING LIST of every choice
                // opens; scroll, A to pick, B to back out. Every pick still
                // funnels through the same QueueEdit machinery, so the frozen
                // opcode / serialization model is untouched -- this is a new face
                // on the old edits, presentation only.
                if (ImGui::BeginTabItem(Str::Label(Str::K::Gb_Tab, "tabgambits"), nullptr, tabSel(1))) {
                    s_tab = 1;
                    static RE::FormID sel = 0;
                    static int selTable = 0;   // 0 combat, 1 logistics

                    // The active party, in snapshot order -- L1/R1 and the on
                    // screen < > cycle through this.
                    std::vector<RE::FormID> party;
                    for (const auto& r : snap.rows) if (r.active) party.push_back(r.id);

                    const FollowerRow* who = nullptr;
                    for (const auto& r : snap.rows) if (r.active && r.id == sel) { who = &r; break; }
                    if (!who) for (const auto& r : snap.rows) if (r.active) { who = &r; sel = r.id; break; }

                    if (!who) {
                        ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_NoFollower));
                        ImGui::EndTabItem();
                    } else {
                        auto switchFollower = [&](int d) {
                            if (party.size() < 2) return;
                            int idx = 0;
                            for (int k = 0; k < (int)party.size(); ++k)
                                if (party[k] == sel) { idx = k; break; }
                            idx = ((idx + d) % (int)party.size() + (int)party.size())
                                  % (int)party.size();
                            sel = party[idx];
                        };

                        const bool popupOpen = ImGui::IsPopupOpen(nullptr,
                            ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

                        // L1/R1 = switch party member (FFXII). Gated exactly like
                        // the old tab code; R1 additionally waits for the opening
                        // press to be released (r1Ready) so opening the board does
                        // not skip a follower.
                        if (!ImGui::IsAnyItemActive() && !popupOpen) {
                            if (r1Ready && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                                switchFollower(+1);
                            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                                switchFollower(-1);
                        }

                        // ── PARTY CONTEXT BAR ───────────────────────────
                        ImGui::AlignTextToFramePadding();
                        ImGui::BeginDisabled(party.size() < 2);
                        if (Click::SmallButton("<##prevf")) switchFollower(-1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::PushFont(g_fontHead);
                        ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                        ImGui::TextUnformatted(who->name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", Str::Fmt(Str::K::Gb_Rank, { who->rank }).c_str());
                        ImGui::SameLine();
                        // #65: per-follower combat-class OVERRIDE trigger. Auto
                        // (0) is the default and reads as "no override" -- the
                        // #64 custom-follower guarantee lives in Scheduler
                        // (Auto never forces anything); this button just opens
                        // the picker that sets/clears it. The popup itself is
                        // drawn below, once listPopup exists.
                        {
                            const int curClass = std::clamp<int>(who->combatClassOverride, 0, 3);
                            std::string cl = Str::Fmt(Str::K::Gb_ClassBtn, { ClassName(curClass) }) + "###classbtn";
                            if (Click::SmallButton(cl.c_str())) ImGui::OpenPopup("##class");
                        }
                        ImGui::SameLine();
                        ImGui::BeginDisabled(party.size() < 2);
                        if (Click::SmallButton(">##nextf")) switchFollower(+1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("  %s", Str::Get(Str::K::Gb_SwitchHint));

                        const bool combat = (selTable == 0);
                        const auto& rules = combat ? who->combat : who->logistics;
                        const int slots = combat ? who->combatSlots : who->logisticsSlots;

                        const VocabEntry* condTab = combat ? kCondsCombat : kCondsLogi;
                        const int condN = combat ? (int)std::size(kCondsCombat)
                                                 : (int)std::size(kCondsLogi);
                        const VocabEntry* actTab = combat ? kActsCombat : kActsLogi;
                        const int actN = combat ? (int)std::size(kActsCombat)
                                                : (int)std::size(kActsLogi);

                        // Combat / Logistics PAGE selector (FFXII flips pages the
                        // same way). Segmented radios, not a dropdown.
                        if (Click::RadioButton(Str::Label(Str::K::Gb_PageCombat, "pgcombat"), combat)) selTable = 0;
                        ImGui::SameLine();
                        if (Click::RadioButton(Str::Label(Str::K::Gb_PageLogistics, "pglogi"), !combat)) selTable = 1;
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", Str::Fmt(Str::K::Gb_SlotsUsed, { (int)rules.size(), slots }).c_str());
                        ImGui::Separator();

                        // ── THE LIST-PICKER (the FFXII interaction) ─────
                        // A big centred scrolling list of Selectables. Opened by
                        // pressing A on a row cell; nav lands on the current pick;
                        // A chooses, B backs out (the input hook's cascaded Escape
                        // closes this popup before it would ever close the board).
                        // onPick receives the chosen index. PushID(k) keeps IDs
                        // unique even when two entries share a label (e.g. spells).
                        auto listPopup = [&](const char* pid, const char* title,
                                             int count, auto labelAt, int current,
                                             auto onPick) {
                            ImGui::SetNextWindowPos(
                                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                            ImGui::SetNextWindowSize(
                                ImVec2(std::max(360.0f, io.DisplaySize.x * 0.30f),
                                       io.DisplaySize.y * 0.55f), ImGuiCond_Appearing);
                            if (ImGui::BeginPopup(pid)) {
                                pickerDrawnThisFrame = true;   // ground truth: a picker is on screen
                                ImGui::PushFont(g_fontHead);
                                ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                ImGui::TextUnformatted(title);
                                ImGui::PopStyleColor();
                                ImGui::PopFont();
                                ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_PickHint));
                                ImGui::Separator();
                                for (int k = 0; k < count; ++k) {
                                    const bool cur = (k == current);
                                    ImGui::PushID(k);
                                    if (Click::Selectable(labelAt(k), cur)) {
                                        onPick(k);
                                        ImGui::CloseCurrentPopup();
                                    }
                                    if (cur) {
                                        ImGui::SetItemDefaultFocus();
                                        if (ImGui::IsWindowAppearing())
                                            ImGui::SetScrollHereY(0.5f);
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndPopup();
                            }
                        };

                        // #65: the combat-class override popup, triggered by
                        // the "Class:" button drawn in the party context bar
                        // above -- placed here (after listPopup exists) rather
                        // than up there; a BeginPopup's SCREEN position comes
                        // from SetNextWindowPos inside listPopup, not from
                        // where in the draw this call sits, so this is purely
                        // a code-organization choice. Per-FOLLOWER, not
                        // per-gambit -- SetClassOverride is keyed on `sel`
                        // alone (uid/table are unused, mirrors Add).
                        {
                            const int curClass = std::clamp<int>(who->combatClassOverride, 0, 3);
                            listPopup("##class", Str::Get(Str::K::Gb_TitleClass), 4,
                                [&](int k) { return ClassName(k); }, curClass,
                                [&](int k) {
                                    QueueEdit({ EditKind::SetClassOverride, sel, selTable,
                                                0u, (float)k });
                                });
                        }

                        // Submenu grouping for the WHEN (condTab) and DO (actTab)
                        // pickers -- built once per table, not per row. A -1 slot
                        // in a *TopIdx marks the nested-group row (rendered as
                        // "Foe:" / "Loot potions:" / "Loot misc:"); the matching
                        // *Idx vector below holds the real condTab/actTab indices
                        // that group opens onto. Empty when the current table has
                        // no members of that group (e.g. combat's actTab has no
                        // loot actions) -- the group row is simply never added.
                        std::vector<int> condTopIdx;      // indices into condTab; -1 = "Foe:" row
                        std::vector<int> condFoeIdx;       // condTab indices shown in the Foe: submenu
                        int condFoeSlot = -1;
                        for (int k = 0; k < condN; ++k) {
                            if (IsFoeCond(condTab[k].op)) {
                                if (condFoeSlot < 0) { condFoeSlot = (int)condTopIdx.size();
                                                        condTopIdx.push_back(-1); }
                                condFoeIdx.push_back(k);
                            } else {
                                condTopIdx.push_back(k);
                            }
                        }
                        std::vector<int> actTopIdx;       // indices into actTab; -1 = potions, -2 = misc
                        std::vector<int> actPotionIdx;     // actTab indices in the Loot potions: submenu
                        std::vector<int> actMiscIdx;       // actTab indices in the Loot misc: submenu
                        int actPotionSlot = -1, actMiscSlot = -1;
                        for (int k = 0; k < actN; ++k) {
                            // LOTD: "Loot museum items" appears only while LOTD awareness
                            // is on (detected + bLootLOTD). A rule already holding it
                            // still shows its label (labelFor), it just is not offered.
                            if (!std::strcmp(actTab[k].op, Vocab::kActLootMuseum) &&
                                !Lotd::GambitOffered()) continue;
                            if (IsPotionLootAct(actTab[k].op)) {
                                if (actPotionSlot < 0) { actPotionSlot = (int)actTopIdx.size();
                                                          actTopIdx.push_back(-1); }
                                actPotionIdx.push_back(k);
                            } else if (IsMiscLootAct(actTab[k].op)) {
                                if (actMiscSlot < 0) { actMiscSlot = (int)actTopIdx.size();
                                                        actTopIdx.push_back(-2); }
                                actMiscIdx.push_back(k);
                            } else {
                                actTopIdx.push_back(k);
                            }
                        }

                        // ── DENSE GAMBIT LIST ───────────────────────────
                        // Tight padding so a full page of gambits reads like
                        // FFXII's, not a few fat rows. Scrolls when long.
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));

                        std::uint32_t rowFocusUid = 0;
                        const float listH = ImGui::GetTextLineHeightWithSpacing() * 11.0f;

                        if (ImGui::BeginTable("##rules", 8,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY, ImVec2(0.0f, listH))) {
                            // Fixed columns are measured (FitW): English widths are the floor.
                            // The button column is up + dn + the wider of del / sure?, each
                            // with frame padding, plus the item gaps between them.
                            const float btnW = [&] {
                                const float fp = ImGui::GetStyle().FramePadding.x * 2.0f;
                                const float del = (std::max)(ImGui::CalcTextSize(Str::Get(Str::K::Gb_Del)).x,
                                                             ImGui::CalcTextSize(Str::Get(Str::K::Gb_DelSure)).x);
                                return ImGui::CalcTextSize(Str::Get(Str::K::Gb_Up)).x + fp +
                                       ImGui::CalcTextSize(Str::Get(Str::K::Gb_Down)).x + fp +
                                       del + fp + ImGui::GetStyle().ItemSpacing.x * 2.0f;
                            }();
                            ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 24);
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_ColOn), ImGuiTableColumnFlags_WidthFixed,
                                                    FitW(30.0f, { Str::Get(Str::K::Gb_ColOn) }));
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_When),
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_Value), ImGuiTableColumnFlags_WidthFixed,
                                                    FitW(84.0f, { Str::Get(Str::K::Gb_Value) }));
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_Do),
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_Spell),ImGuiTableColumnFlags_WidthStretch);
                            // #68: who a Cast-on-target row hits (Self / player
                            // by name / Ally: Nearest / a specific follower).
                            ImGui::TableSetupColumn(Str::Get(Str::K::Gb_Target),ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, (std::max)(96.0f, btnW));
                            ImGui::TableHeadersRow();

                            for (int i = 0; i < (int)rules.size(); ++i) {
                                const auto& rv = rules[i];
                                ImGui::TableNextRow();

                                // FLAIR #9: the FFXII "line lights up" pulse.
                                if (rv.firedAt.time_since_epoch().count() != 0) {
                                    const float age = std::chrono::duration<float>(
                                        std::chrono::steady_clock::now() - rv.firedAt).count();
                                    if (age >= 0.0f && age < 1.0f) {
                                        ImVec4 pulse = skin.accent;
                                        pulse.w = 0.30f * (1.0f - age);
                                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                               ImGui::GetColorU32(pulse));
                                    }
                                }

                                ImGui::PushID((int)rv.uid);
                                auto track = [&] { if (ImGui::IsItemFocused()) rowFocusUid = rv.uid; };

                                // #
                                ImGui::TableNextColumn();
                                ImGui::AlignTextToFramePadding();
                                ImGui::TextDisabled("%d", i + 1);

                                // ON / OFF toggle
                                ImGui::TableNextColumn();
                                bool en = rv.enabled;
                                if (Click::Checkbox("##en", &en))
                                    QueueEdit({ EditKind::Toggle, sel, selTable, rv.uid, 0 });
                                track();

                                // WHEN -- Selectable that opens the condition list.
                                ImGui::TableNextColumn();
                                {
                                    std::string cl = Str::Fmt(Str::K::Gb_WhenRow,
                                                              { labelFor(rv.condOp, condTab, condN) });
                                    ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                    const bool clicked = Click::Selectable(cl.c_str());
                                    ImGui::PopStyleColor();
                                    if (clicked) ImGui::OpenPopup("##cond");
                                    track();
                                    if (rv.lastFired) { ImGui::SameLine();
                                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "*"); }
                                    // Foe: conditions collapse to one nested-popup
                                    // row (condTopIdx/condFoeIdx, built once above
                                    // the row loop) so the flat list stays short.
                                    const bool curIsFoeCond = IsFoeCond(rv.condOp.c_str());
                                    int curC = 0;
                                    for (int t = 0; t < (int)condTopIdx.size(); ++t) {
                                        if (condTopIdx[t] == -1) { if (curIsFoeCond) { curC = t; break; } }
                                        else if (rv.condOp == condTab[condTopIdx[t]].op) { curC = t; break; }
                                    }
                                    // Defer the child-popup open to THIS (row) scope: OpenPopup
                                    // and the ##condfoe BeginPopup must hash against the SAME ID
                                    // stack. Calling OpenPopup inside the ##cond popup's onPick
                                    // mismatches the ID -> an invisible, input-blocking popup.
                                    bool openCondFoe = false;
                                    listPopup("##cond", Str::Get(Str::K::Gb_When),
                                        (int)condTopIdx.size(),
                                        [&](int t) { return condTopIdx[t] == -1
                                                            ? Str::Get(Str::K::Gb_TitleFoe) : condTab[condTopIdx[t]].label(); },
                                        curC,
                                        [&](int t) {
                                            if (condTopIdx[t] == -1) { openCondFoe = true; return; }
                                            QueueEdit({ EditKind::SetCond, sel, selTable,
                                                        rv.uid, (float)condTopIdx[t] });
                                        });
                                    if (openCondFoe) ImGui::OpenPopup("##condfoe");
                                    if (condFoeSlot >= 0) {
                                        int curF = 0;
                                        for (int t = 0; t < (int)condFoeIdx.size(); ++t)
                                            if (rv.condOp == condTab[condFoeIdx[t]].op) { curF = t; break; }
                                        listPopup("##condfoe", Str::Get(Str::K::Gb_TitleFoe), (int)condFoeIdx.size(),
                                            [&](int t) { return condTab[condFoeIdx[t]].label(); }, curF,
                                            [&](int t) {
                                                QueueEdit({ EditKind::SetCond, sel, selTable,
                                                            rv.uid, (float)condFoeIdx[t] });
                                            });
                                    }
                                }

                                // VALUE -- opens a preset list matched to what the
                                // param MEANS. No sliders / no typing, so the whole
                                // editor is list-driven and pad-first.
                                ImGui::TableNextColumn();
                                {
                                    const ParamKind pk = kindFor(rv.condOp, condTab, condN);
                                    if (pk == ParamKind::None) {
                                        ImGui::TextDisabled("-");
                                    } else {
                                        const std::string vs = ValueText(pk, rv.param);
                                        if (Click::Selectable(vs.c_str())) ImGui::OpenPopup("##val");
                                        track();

                                        std::vector<std::pair<float, std::string>> pv;
                                        if (pk == ParamKind::Percent) {
                                            for (int p = 5; p <= 100; p += 5)
                                                pv.emplace_back(p / 100.0f, Str::Fmt(Str::K::Gb_FmtPct, { p }));
                                        } else if (pk == ParamKind::Count) {
                                            for (int n = 1; n <= 20; ++n)
                                                pv.emplace_back((float)n, std::to_string(n));
                                            for (int n : { 25, 30, 40, 50, 75, 100 })
                                                pv.emplace_back((float)n, std::to_string(n));
                                        } else {   // Distance
                                            for (int u = 0; u <= 2000; u += 100)
                                                pv.emplace_back((float)u, Str::Fmt(Str::K::Gb_FmtDist, { u }));
                                            for (int u : { 2500, 3000, 4000, 5000 })
                                                pv.emplace_back((float)u, Str::Fmt(Str::K::Gb_FmtDist, { u }));
                                        }
                                        int curV = 0; float best = 1e9f;
                                        for (int k = 0; k < (int)pv.size(); ++k) {
                                            float d = pv[k].first - rv.param; if (d < 0) d = -d;
                                            if (d < best) { best = d; curV = k; }
                                        }
                                        listPopup("##val", Str::Get(Str::K::Gb_Value), (int)pv.size(),
                                            [&](int k) { return pv[k].second.c_str(); }, curV,
                                            [&](int k) {
                                                QueueEdit({ EditKind::SetParam, sel, selTable,
                                                            rv.uid, pv[k].first });
                                            });
                                    }
                                }

                                // DO -- action list.
                                ImGui::TableNextColumn();
                                {
                                    std::string al = Str::Fmt(Str::K::Gb_DoRow,
                                                              { labelFor(rv.actOp, actTab, actN) });
                                    const bool wait = (rv.actOp == Vocab::kActWait);
                                    if (wait) ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                    const bool clicked = Click::Selectable(al.c_str());
                                    if (wait) ImGui::PopStyleColor();
                                    if (clicked) ImGui::OpenPopup("##act");
                                    track();
                                    // The 4 potion loot actions and the 3 misc loot
                                    // actions (Ingredients/SoulGems/Jewelry) each
                                    // collapse to one nested-popup row (actTopIdx/
                                    // actPotionIdx/actMiscIdx, built once above the
                                    // row loop). Combat's actTab has no loot actions,
                                    // so those slots simply never appear there.
                                    const bool curIsPotionAct = IsPotionLootAct(rv.actOp.c_str());
                                    const bool curIsMiscAct   = IsMiscLootAct(rv.actOp.c_str());
                                    int curA = 0;
                                    for (int t = 0; t < (int)actTopIdx.size(); ++t) {
                                        if (actTopIdx[t] == -1) { if (curIsPotionAct) { curA = t; break; } }
                                        else if (actTopIdx[t] == -2) { if (curIsMiscAct) { curA = t; break; } }
                                        else if (rv.actOp == actTab[actTopIdx[t]].op) { curA = t; break; }
                                    }
                                    // Defer child-popup opens to THIS (row) scope -- same ID-stack
                                    // reason as ##condfoe above (an OpenPopup inside the ##act popup
                                    // mismatches the child's BeginPopup ID -> invisible hung popup).
                                    bool openActPotions = false, openActMisc = false;
                                    listPopup("##act", Str::Get(Str::K::Gb_Do), (int)actTopIdx.size(),
                                        [&](int t) { return actTopIdx[t] == -1 ? Str::Get(Str::K::Gb_TitleLootPotions)
                                                            : actTopIdx[t] == -2 ? Str::Get(Str::K::Gb_TitleLootMisc)
                                                            : actTab[actTopIdx[t]].label(); },
                                        curA,
                                        [&](int t) {
                                            if (actTopIdx[t] == -1) { openActPotions = true; return; }
                                            if (actTopIdx[t] == -2) { openActMisc = true; return; }
                                            QueueEdit({ EditKind::SetAct, sel, selTable,
                                                        rv.uid, (float)actTopIdx[t] });
                                        });
                                    if (openActPotions) ImGui::OpenPopup("##actpotions");
                                    if (openActMisc)    ImGui::OpenPopup("##actmisc");
                                    if (actPotionSlot >= 0) {
                                        int curP = 0;
                                        for (int t = 0; t < (int)actPotionIdx.size(); ++t)
                                            if (rv.actOp == actTab[actPotionIdx[t]].op) { curP = t; break; }
                                        listPopup("##actpotions", Str::Get(Str::K::Gb_TitleLootPotions), (int)actPotionIdx.size(),
                                            [&](int t) { return actTab[actPotionIdx[t]].label(); }, curP,
                                            [&](int t) {
                                                QueueEdit({ EditKind::SetAct, sel, selTable,
                                                            rv.uid, (float)actPotionIdx[t] });
                                            });
                                    }
                                    if (actMiscSlot >= 0) {
                                        int curM = 0;
                                        for (int t = 0; t < (int)actMiscIdx.size(); ++t)
                                            if (rv.actOp == actTab[actMiscIdx[t]].op) { curM = t; break; }
                                        listPopup("##actmisc", Str::Get(Str::K::Gb_TitleLootMisc), (int)actMiscIdx.size(),
                                            [&](int t) { return actTab[actMiscIdx[t]].label(); }, curM,
                                            [&](int t) {
                                                QueueEdit({ EditKind::SetAct, sel, selTable,
                                                            rv.uid, (float)actMiscIdx[t] });
                                            });
                                    }
                                }

                                // SPELL -- only for cast actions; its own list.
                                ImGui::TableNextColumn();
                                {
                                    const bool isCast = (rv.actOp == Vocab::kActCastSelf ||
                                                         rv.actOp == Vocab::kActCastTarget ||
                                                         rv.actOp == Vocab::kActCastPlayer);
                                    if (!isCast) {
                                        ImGui::TextDisabled("-");
                                    } else {
                                        const char* cur = rv.spellName.empty()
                                                          ? Str::Get(Str::K::Gb_PickSpell) : rv.spellName.c_str();
                                        if (Click::Selectable(cur)) ImGui::OpenPopup("##spell");
                                        track();
                                        if (!rv.fail.empty() && ImGui::IsItemHovered())
                                            ImGui::SetTooltip("%s", Str::Fmt(Str::K::Gb_LastFail, { rv.fail }).c_str());
                                        // #4: known spells (click = pick) PLUS
                                        // teachable-from-spellbook spells (a second
                                        // click confirms and CONSUMES the book).
                                        // Drawn by hand rather than listPopup so both
                                        // groups fit one gamepad-navigable popup.
                                        ImGui::SetNextWindowPos(
                                            ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                                        ImGui::SetNextWindowSize(
                                            ImVec2(std::max(360.0f, io.DisplaySize.x * 0.30f),
                                                   io.DisplaySize.y * 0.55f), ImGuiCond_Appearing);
                                        if (ImGui::BeginPopup("##spell")) {
                                            pickerDrawnThisFrame = true;
                                            static RE::FormID s_teachArmed = 0;
                                            if (ImGui::IsWindowAppearing()) s_teachArmed = 0;   // never open pre-armed
                                            ImGui::PushFont(g_fontHead);
                                            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                            ImGui::TextUnformatted(Str::Get(Str::K::Gb_Spell));
                                            ImGui::PopStyleColor();
                                            ImGui::PopFont();
                                            ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_PickHint));
                                            ImGui::Separator();
                                            if (who->knownSpells.empty() && who->teachableSpells.empty())
                                                ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_NoSpells));
                                            for (int k = 0; k < (int)who->knownSpells.size(); ++k) {
                                                const auto& sp = who->knownSpells[k];
                                                const bool curSel = sp.id == rv.spell;
                                                ImGui::PushID((int)sp.id);   // dup names -> unique IDs
                                                if (Click::Selectable(sp.name.c_str(), curSel)) {
                                                    EditCmd e{ EditKind::SetSpell, sel, selTable, rv.uid, 0 };
                                                    e.spell = sp.id;
                                                    QueueEdit(e);
                                                }
                                                DrawSpellHoverTooltip(sp.magickaCost, sp.tooltip);   // precomputed (#4)
                                                if (curSel) {
                                                    ImGui::SetItemDefaultFocus();
                                                    if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
                                                }
                                                ImGui::PopID();
                                            }
                                            if (!who->teachableSpells.empty()) {
                                                ImGui::Separator();
                                                ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_TeachHeader));
                                                for (const auto& t : who->teachableSpells) {
                                                    const bool armed = (s_teachArmed == t.book);
                                                    if (armed) ImGui::PushStyleColor(ImGuiCol_Text, skin.danger);
                                                    const std::string lbl = armed
                                                        ? Str::Fmt(Str::K::Gb_TeachArmed, { t.name })
                                                        : Str::Fmt(Str::K::Gb_Spellbook, { t.name });
                                                    ImGui::PushID((int)t.book);   // dup names -> unique IDs
                                                    if (Click::Selectable(lbl.c_str(), false,
                                                                          ImGuiSelectableFlags_DontClosePopups)) {
                                                        if (armed) {
                                                            EditCmd e{ EditKind::TeachSpell, sel, selTable, rv.uid, 0 };
                                                            e.spell = t.spell; e.book = t.book;
                                                            QueueEdit(e);
                                                            s_teachArmed = 0;
                                                            ImGui::CloseCurrentPopup();
                                                        } else {
                                                            s_teachArmed = t.book;   // arm: show the warning first
                                                        }
                                                    }
                                                    DrawSpellHoverTooltip(t.magickaCost, t.tooltip);   // precomputed (#4)
                                                    ImGui::PopID();
                                                    if (armed) ImGui::PopStyleColor();
                                                }
                                            }
                                            ImGui::EndPopup();
                                        }
                                    }
                                }

                                // TARGET -- #68, act.cast_target ONLY. cast_self
                                // and cast_player ignore the subject entirely
                                // (Fire() hardcodes their target), so a live
                                // picker there would edit a field nothing reads.
                                ImGui::TableNextColumn();
                                {
                                    const bool isCastTarget = (rv.actOp == Vocab::kActCastTarget);
                                    if (!isCastTarget) {
                                        ImGui::TextDisabled("-");
                                    } else {
                                        const char* cur = rv.subjectName.empty()
                                                          ? Str::Get(Str::K::Gb_SubjAuto) : rv.subjectName.c_str();
                                        if (Click::Selectable(cur)) ImGui::OpenPopup("##target");
                                        track();

                                        // Option list, rebuilt fresh each frame the
                                        // popup is open: Auto, Enemy, Self, the player by name,
                                        // Ally: Nearest, then every OTHER active
                                        // follower by name. form==0 means "use kind
                                        // (the Subject enum)"; form!=0 means "this
                                        // SPECIFIC follower" and kind is ignored.
                                        struct TargetOpt {
                                            std::uint8_t kind; RE::FormID form; const std::string* label;
                                        };
                                        // subject 0 (Vocab::Subject::Self) on a cast-AT-target row
                                        // is AUTO -- MFO infers WHO from the spell: hostile -> all
                                        // nearby enemies, a beneficial self-buff -> the caster, a
                                        // beneficial heal/aimed buff -> the party members who need
                                        // it. It is NOT "cast on the follower" (that is the separate
                                        // Cast-on-self action). The manual picks below still target
                                        // exactly who you choose.
                                        // Not static: the text is translated, a reload republishes it.
                                        const std::string kAutoLbl = Str::Get(Str::K::Gb_SubjAutoInfer);
                                        const std::string kAllyLbl = Str::Get(Str::K::Gb_SubjAlly);
                                        // marth 2026-10-06: "Enemy (gambit target)" = the foe MFO's
                                        // targeting gambits commanded (cast/Fire.cpp CommandedEnemy), combat
                                        // table only (out of combat nothing is commanded); "Self" = the caster,
                                        // the act.cast_self road. Both appended Vocab::Subject values.
                                        const std::string kEnemyLbl = Str::Get(Str::K::Gb_SubjEnemy);
                                        const std::string kSelfLbl  = Str::Get(Str::K::Gb_SubjSelf);
                                        std::vector<TargetOpt> opts;
                                        opts.push_back({ (std::uint8_t)Vocab::Subject::Self,        0, &kAutoLbl });
                                        if (selTable == 0)
                                            opts.push_back({ (std::uint8_t)Vocab::Subject::Enemy,   0, &kEnemyLbl });
                                        opts.push_back({ (std::uint8_t)Vocab::Subject::Caster,      0, &kSelfLbl });
                                        opts.push_back({ (std::uint8_t)Vocab::Subject::Player,      0, &who->playerName });
                                        opts.push_back({ (std::uint8_t)Vocab::Subject::NearestAlly, 0, &kAllyLbl });
                                        for (const auto& al : who->alliesForPicker)
                                            opts.push_back({ 0, al.first, &al.second });

                                        int curT = 0;
                                        if (rv.subjectActorForm != 0) {
                                            for (int k = 0; k < (int)opts.size(); ++k)
                                                if (opts[k].form == rv.subjectActorForm) { curT = k; break; }
                                        } else {
                                            for (int k = 0; k < (int)opts.size(); ++k)
                                                if (opts[k].form == 0 && opts[k].kind == rv.subject) { curT = k; break; }
                                        }

                                        listPopup("##target", Str::Get(Str::K::Gb_Target), (int)opts.size(),
                                            [&](int k) { return opts[k].label->c_str(); }, curT,
                                            [&](int k) {
                                                if (opts[k].form != 0) {
                                                    EditCmd e{ EditKind::SetSubjectActor, sel, selTable, rv.uid, 0 };
                                                    e.subjectActor = opts[k].form;
                                                    QueueEdit(e);
                                                } else {
                                                    QueueEdit({ EditKind::SetSubject, sel, selTable,
                                                                rv.uid, (float)opts[k].kind });
                                                }
                                            });
                                    }
                                }

                                // REORDER / DELETE (kept on buttons so reorder does
                                // not fight the shoulder party-switch, per spec).
                                ImGui::TableNextColumn();
                                if (Click::SmallButton(Str::Label(Str::K::Gb_Up, "up"))) QueueEdit({ EditKind::MoveUp, sel, selTable, rv.uid, 0 });
                                track();
                                ImGui::SameLine();
                                if (Click::SmallButton(Str::Label(Str::K::Gb_Down, "dn"))) QueueEdit({ EditKind::MoveDown, sel, selTable, rv.uid, 0 });
                                track();
                                ImGui::SameLine();
                                static std::uint32_t s_armed = 0;
                                const bool armed = (s_armed == rv.uid);
                                if (armed) ImGui::PushStyleColor(ImGuiCol_Button, skin.danger);
                                // Delete (and its armed confirm) stays on stock ImGui on purpose: sliding off
                                // before release must still abort a destructive action.
                                if (ImGui::SmallButton(Str::Label(armed ? Str::K::Gb_DelSure : Str::K::Gb_Del,
                                                                   armed ? "delarmed" : "del"))) {
                                    if (armed) { QueueEdit({ EditKind::Del, sel, selTable, rv.uid, 0 }); s_armed = 0; }
                                    else s_armed = rv.uid;
                                }
                                if (armed) ImGui::PopStyleColor();
                                track();

                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }
                        ImGui::PopStyleVar(3);

                        // Y / FaceUp = toggle the ON/OFF of the nav-focused row
                        // from anywhere in it (FFXII "flip the line"). ImGui's
                        // FaceUp nav action only acts on text-input widgets, and
                        // the editor now has none, so it is inert here -- no
                        // collision. Gated like the shoulder bindings.
                        if (rowFocusUid != 0 && !ImGui::IsAnyItemActive() && !popupOpen &&
                            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
                            QueueEdit({ EditKind::Toggle, sel, selTable, rowFocusUid, 0 });

                        // ── FULL-WIDTH READ-ONLY SUMMARY (Deck legibility) ──
                        if (!rules.empty()) {
                            ImGui::Spacing();
                            ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_FullRules));
                            ImGui::Separator();
                            ImGui::PushTextWrapPos(0.0f);
                            for (int i = 0; i < (int)rules.size(); ++i) {
                                const auto& rv = rules[i];
                                const std::string valTxt = ValueText(kindFor(rv.condOp, condTab, condN), rv.param);
                                const std::string cond = valTxt.empty()
                                    ? Str::Fmt(Str::K::Gb_SumWhen, { i + 1, labelFor(rv.condOp, condTab, condN) })
                                    : Str::Fmt(Str::K::Gb_SumWhenVal, { i + 1, labelFor(rv.condOp, condTab, condN), valTxt });
                                std::string act = labelFor(rv.actOp, actTab, actN);
                                if ((rv.actOp == Vocab::kActCastSelf ||
                                     rv.actOp == Vocab::kActCastTarget ||
                                     rv.actOp == Vocab::kActCastPlayer) && !rv.spellName.empty())
                                    act = Str::Fmt(Str::K::Gb_SumCast, { act, rv.spellName });
                                if (!rv.enabled) {
                                    ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                    ImGui::TextWrapped("%s", Str::Fmt(Str::K::Gb_SumOff,
                                                       { cond, act }).c_str());
                                    ImGui::PopStyleColor();
                                } else {
                                    ImGui::TextColored(skin.accent, "%s", cond.c_str());
                                    ImGui::SameLine(0, 0);
                                    ImGui::TextDisabled("  %s  ", Str::Get(Str::K::Gb_SumArrow));
                                    ImGui::SameLine(0, 0);
                                    if (rv.actOp == Vocab::kActWait)
                                        ImGui::TextDisabled("%s", act.c_str());
                                    else
                                        ImGui::TextUnformatted(act.c_str());
                                    if (rv.lastFired) {
                                        ImGui::SameLine();
                                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "*");
                                    }
                                }
                            }
                            ImGui::PopTextWrapPos();
                            ImGui::Spacing();
                        }

                        const bool full = (int)rules.size() >= slots;
                        ImGui::BeginDisabled(full);
                        if (Click::Button(Str::Label(Str::K::Gb_AddRule, "addrule")))
                            QueueEdit({ EditKind::Add, sel, selTable, 0u, 0 });
                        ImGui::EndDisabled();
                        if (full) { ImGui::SameLine();
                            ImGui::TextDisabled("%s", Str::Fmt(Str::K::Gb_AllUsed, { slots }).c_str()); }

                        ImGui::Spacing();
                        ImGui::TextDisabled("%s", Str::Get(Str::K::Gb_FooterHelp));

                        ImGui::EndTabItem();
                    }
                }

                // ── THE PROGRESSION TAB (#74 component 3) ───────────────
                // Body moved to Board_Progression.cpp (mechanical module
                // split) — DrawProgressionTab draws the hosted tab in place.
                DrawProgressionTab(snap, progActive, tabSel(2), io, skin,
                                   r1Ready, s_tab, pickerDrawnThisFrame);

                ImGui::EndTabBar();
            }
            s_tabForce = false;   // consumed this frame; mouse clicks own s_tab again

            ImGui::Separator();
            // CONTEXTUAL PROMPTS. What the ACCEPT and BACK keys do changes with
            // state -- editing a value vs a combo open vs plain navigation -- so
            // the hint follows the state. Text glyphs (no controller font is
            // loaded) with the keyboard equivalent inline, so one strip serves
            // pad and keyboard. Drawn on the render thread between NewFrame and
            // Render, where IsAnyItemActive/IsPopupOpen are valid.
            // CASCADED BACK, decided here on the render thread off ImGui's OWN
            // nav-cancel key -- the only signal that can't race the picker close.
            // B's ONE source is the input hook (v1.0.59): it forwards physical B as
            // GamepadFaceRight off Skyrim's ButtonEvent stream; the Win32 backend's
            // own XInput poll -- which used to feed B but went deaf on Steam Input
            // kb<->pad mode flips -- is compiled out of the vendored backend TU
            // (IMGUI_IMPL_WIN32_DISABLE_GAMEPAD), so no second path races this one.
            // The hook feeds on the input thread under g_ioMx; the whole render
            // frame (NewFrame -> Render) holds the same lock, so a fed edge is
            // either fully visible to this frame's NewFrame or waits for the next
            // -- exactly how the other hook-fed nav keys (and Esc) already land.
            // By the time this runs, ImGui's nav-cancel has ALREADY closed any open
            // picker this frame (NavUpdateCancelRequest, gated on nav_gamepad_active
            // = NavEnableGamepad + HasGamepad, both pinned on at init now that the
            // backend can't rewrite HasGamepad per frame).
            // Rule: a cancel press (B / Esc) when NO picker was up last frame
            // AND none opened this frame is a root back -> close the board. A cancel
            // press with a picker up is the picker-back ImGui just handled -> keep
            // the board. Same key, same thread as the close: they can't disagree.
            // Everything B (nav-cancel) can back OUT of this frame, before it closes
            // the board: an open picker popup, OR nav focus sitting inside the gambit
            // table (a scrolling child region ImGui's nav-cancel exits to the parent).
            // Read on THIS, the render, thread, where these are all valid. Guard the
            // close on LAST frame's state -- this frame's may already be one level up,
            // since NewFrame ran nav-cancel before us.
            // Read ImGui's OWN nav state (valid on this, the render, thread). B has
            // something to back OUT of when a popup is open, when nav sits inside a
            // child window (any ScrollY table / BeginChild), or when nav is in the
            // menu layer. Only when NONE of those hold is B a true top-level exit
            // that closes the board. Guard on LAST frame's verdict, since NewFrame
            // already ran nav-cancel for this frame's B before we get here.
            ImGuiContext& gg = *ImGui::GetCurrentContext();
            const bool navPopup = gg.OpenPopupStack.Size > 0;
            const bool navChild = gg.NavWindow && (gg.NavWindow->Flags & ImGuiWindowFlags_ChildWindow);
            const bool navMenu  = gg.NavLayer != ImGuiNavLayer_Main;
            const bool navBusy  = navPopup || navChild || navMenu;
            const bool cancelPressed = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                       ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (cancelPressed) {
                const bool closing = !s_prevNavBusy && !navBusy;
                // DIAGNOSTIC: dump the full nav state so the exact "gambits portion"
                // vs "true top" distinction is visible in the log (marth).
                spdlog::info("[bcancel] busyNow={} busyPrev={} popup={} child={} menu={} "
                             "navId={:08X} navWin=\"{}\" -> {}",
                             navBusy, s_prevNavBusy, navPopup, navChild, navMenu,
                             gg.NavId, gg.NavWindow ? gg.NavWindow->Name : "null",
                             closing ? "CLOSE BOARD" : "back out");
                if (closing) CloseBoard();
            }
            s_prevNavBusy = navBusy;   // ground truth for next frame
            if (pickerDrawnThisFrame)
                ImGui::TextDisabled("%s", Str::Get(Str::K::Ft_Picker));
            else
                ImGui::TextDisabled("%s", Str::Get(Str::K::Ft_Main));
            ImGui::End();
            ImGui::PopStyleColor(skinCols);
        }

}
