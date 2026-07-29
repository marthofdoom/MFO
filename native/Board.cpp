#include "PCH.h"

// d3d11.h drags in windows.h, which CommonLibSSE-NG never includes.
// WIN32_LEAN_AND_MEAN / NOMINMAX come from CMakePresets; the GetObject macro
// does not, and wingdi.h #defines GetObject -> GetObjectW, which silently
// hijacks BGSDefaultObjectManager::GetObject<T>(). #undef AFTER the includes
// (ENGINE_NOTES §9 -- a compile error that reads like nonsense).
#include <d3d11.h>
#include <dxgi.h>
#undef GetObject

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "Board.h"
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "Forms.h"
#include "State.h"
#include "Probe.h"
#include "Vocabulary.h"
#include "Scheduler.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace MFO::Board {

    namespace {

        // ── shared state ────────────────────────────────────────────────────
        // Two mutexes, NEVER nested, per ARCHITECTURE.md §3.1.
        std::mutex g_snapMx;      // guards g_snapshot
        std::mutex g_ioMx;        // guards ALL ImGui IO, across all three threads
        Snapshot   g_snapshot;

        std::atomic<bool> g_ready{ false };   // D3D init succeeded
        std::atomic<bool> g_open{ false };     // full panel -- SWALLOWS input
        // OFF until a game is actually loaded. g_ready goes true at renderer init,
        // long before any save, so defaulting this on drew the HUD over the
        // title screen, loading screens and every vanilla menu.
        std::atomic<bool> g_hud{ false };      // compact readout -- passive, never takes input
        std::atomic<bool> g_wantClose{ false };
        std::atomic<bool> g_cursorInit{ false };
        std::atomic<float> g_cursorX{ 0.0f }, g_cursorY{ 0.0f };
        std::atomic<std::uint64_t> g_frame{ 0 };

        // io.DisplaySize LIES under Proton/upscalers: the Win32 backend reads
        // GetClientRect, which can disagree with the backbuffer. Cache the real
        // size and overwrite every frame (ENGINE_NOTES §9).
        float g_bbW = 0.0f, g_bbH = 0.0f;

        ID3D11Device*        g_device  = nullptr;
        ID3D11DeviceContext* g_context = nullptr;

        bool g_stickNav[4] = { false, false, false, false };   // up/down/left/right
        std::atomic<bool> g_shoutDownSeen{ false };

        // ── input translation ───────────────────────────────────────────────
        ImGuiKey GamepadToImGuiKey(std::uint32_t a_key) {
            using K = RE::BSWin32GamepadDevice::Key;
            switch (static_cast<K>(a_key)) {
            case K::kUp:            return ImGuiKey_GamepadDpadUp;
            case K::kDown:          return ImGuiKey_GamepadDpadDown;
            case K::kLeft:          return ImGuiKey_GamepadDpadLeft;
            case K::kRight:         return ImGuiKey_GamepadDpadRight;
            case K::kA:             return ImGuiKey_GamepadFaceDown;
            case K::kX:             return ImGuiKey_GamepadFaceLeft;
            case K::kY:             return ImGuiKey_GamepadFaceUp;
            case K::kLeftShoulder:  return ImGuiKey_GamepadL1;
            case K::kRightShoulder: return ImGuiKey_GamepadR1;
            default:                return ImGuiKey_None;
            }
            // kB is deliberately ABSENT: it is intercepted as close before
            // translation, so it never reaches ImGui nav as Cancel and fight
            // the close.
        }

        ImGuiKey DIKToImGuiKey(std::uint32_t a_key) {
            switch (a_key) {
            case 0xC8: return ImGuiKey_UpArrow;
            case 0xD0: return ImGuiKey_DownArrow;
            case 0xCB: return ImGuiKey_LeftArrow;
            case 0xCD: return ImGuiKey_RightArrow;
            case 0x1C: return ImGuiKey_Enter;
            case 0x12: return ImGuiKey_Enter;   // E — Skyrim's activate key
            // Text-editing keys, so a typed value (double-click a value) is
            // actually editable and not just fillable.
            case 0x0E: return ImGuiKey_Backspace;
            case 0xD3: return ImGuiKey_Delete;
            case 0xC7: return ImGuiKey_Home;
            case 0xCF: return ImGuiKey_End;
            default:   return ImGuiKey_None;
            }
        }

        // Resolve the live shout binding per device, so it follows a remap.
        std::uint32_t ShoutKey(RE::INPUT_DEVICE a_device) {
            auto* cm = RE::ControlMap::GetSingleton();
            auto* ue = RE::UserEvents::GetSingleton();
            if (!cm || !ue) return 0xFFFFFFFFu;
            const auto key = cm->GetMappedKey(ue->shout, a_device);
            return key == 0xFF ? 0xFFFFFFFFu : key;   // never let kInvalid match a real code
        }

        // ── the panel ───────────────────────────────────────────────────────
    // ── EDIT COMMAND QUEUE ──────────────────────────────────────────────────
    // The board draws on the RENDER thread; the tables live on the main thread.
    // So an edit is enqueued from the draw and APPLIED in PublishSnapshot,
    // which runs on the main thread (#2, the same rule as everything touching
    // g_followers). Never write a rule table from a draw call.
    enum class EditKind : std::uint8_t { Add, Del, MoveUp, MoveDown, Toggle,
                                         CycleCond, CycleAct, SetParam, SetSpell };
    struct EditCmd {
        EditKind kind; RE::FormID fid; int table; std::uint32_t uid; float param;
        RE::FormID spell = 0;
    };
    std::mutex          g_editMx;
    std::vector<EditCmd> g_edits;

    void QueueEdit(EditCmd c) { std::scoped_lock lk(g_editMx); g_edits.push_back(c); }

    // The vocabulary the editor cycles through, opcode + human label + what its
    // param MEANS. Frozen opcode strings (#10); labels are UI only.
    //
    // ParamKind drives the param widget AND fixes the "1000%" bug: a count
    // condition (arrows < 10) stored 10.0 and was rendered as 10*100 = 1000%.
    // Percent params are fractions 0..1 shown as a %; Count params are whole
    // numbers; None conditions take no param at all.
    // Distance = whole units (a range), edited like a count but on a wider
    // scale (a bow reaches ~1500u, a cell is 4096u).
    enum class ParamKind : std::uint8_t { None, Percent, Count, Distance };
    struct VocabEntry { const char* op; const char* label; ParamKind kind = ParamKind::None; };

    // COMBAT and LOGISTICS have DISTINCT vocabularies. The editor shows the set
    // that matches the table you are editing -- so a logistics rule can actually
    // pick "loot potions", which the old single combat-only table made
    // impossible (marth: "no way to assign" the logistics gambits).
    inline constexpr VocabEntry kCondsCombat[] = {
        { Vocab::kCondAlways,        "Always",               ParamKind::None    },
        { Vocab::kCondSelfHpBelow,   "Self HP % below",      ParamKind::Percent },
        { Vocab::kCondSelfMpBelow,   "Self Magicka % below", ParamKind::Percent },
        { Vocab::kCondSelfSpBelow,   "Self Stamina % below", ParamKind::Percent },
        { Vocab::kCondPlayerHpBelow, "Player HP % below",    ParamKind::Percent },
        { Vocab::kCondFoeLowestHp,   "Foe: lowest HP",       ParamKind::None    },
        { Vocab::kCondFoeHpBelow,    "Foe: HP % below",      ParamKind::Percent },
        { Vocab::kCondFoeHighestHp,  "Foe: highest HP",      ParamKind::None    },
        { Vocab::kCondFoeAny,        "Foe: nearest",         ParamKind::None    },
        { Vocab::kCondFoeWithinRange,"Foe within range",     ParamKind::Distance},
        { Vocab::kCondFoeBeyondRange,"Foe beyond range",     ParamKind::Distance},
        { Vocab::kCondFoeAttackingPlayer,"Foe attacking player", ParamKind::None },
        { Vocab::kCondFoeAttackingMe,"Foe attacking me",     ParamKind::None    },
        { Vocab::kCondFoeIsUndead,   "Foe is undead",        ParamKind::None    },
        { Vocab::kCondFoeIsDragon,   "Foe is dragon",        ParamKind::None    },
        { Vocab::kCondFoeCountAtLeast,"Foe count at least",  ParamKind::Count   },
        { Vocab::kCondSelfHpAbove,   "Self HP % above",      ParamKind::Percent },
        { Vocab::kCondSelfMpAbove,   "Self Magicka % above", ParamKind::Percent },
        { Vocab::kCondSelfSpAbove,   "Self Stamina % above", ParamKind::Percent },
        { Vocab::kCondAllyHpBelow,   "Ally HP % below",      ParamKind::Percent },
        { Vocab::kCondIsInterior,    "In an interior",       ParamKind::None    },
        { Vocab::kCondIsNight,       "At night",             ParamKind::None    },
    };
    inline constexpr VocabEntry kActsCombat[] = {
        { Vocab::kActWait,              "Wait" },
        { Vocab::kActCastSelf,          "Cast on self" },
        { Vocab::kActCastTarget,        "Cast at foe/ally" },
        { Vocab::kActAttack,            "Attack" },
        { Vocab::kActDrinkHealthPotion, "Drink health potion" },
        { Vocab::kActDrinkStaminaPotion,"Drink stamina potion" },
        { Vocab::kActDrinkMagickaPotion,"Drink magicka potion" },
        { Vocab::kActEquipRanged,       "Equip ranged weapon" },
        { Vocab::kActEquipMelee,        "Equip melee weapon" },
        { Vocab::kActEquipTorch,        "Equip torch" },
    };
    inline constexpr VocabEntry kCondsLogi[] = {
        { Vocab::kCondAlways,              "Always",                ParamKind::None    },
        { Vocab::kCondSelfHpBelow,         "Self HP % below",       ParamKind::Percent },
        { Vocab::kCondSelfMpBelow,         "Self Magicka % below",  ParamKind::Percent },
        { Vocab::kCondSelfSpBelow,         "Self Stamina % below",  ParamKind::Percent },
        { Vocab::kCondSelfLowHealthPotion, "Health potions below",  ParamKind::Count   },
        { Vocab::kCondSelfLowStaminaPotion,"Stamina potions below", ParamKind::Count   },
        { Vocab::kCondSelfLowMagickaPotion,"Magicka potions below", ParamKind::Count   },
        { Vocab::kCondSelfOutOfArrows,     "Arrows below",          ParamKind::Count   },
    };
    inline constexpr VocabEntry kActsLogi[] = {
        { Vocab::kActDrinkHealthPotion,  "Drink health potion" },
        { Vocab::kActDrinkStaminaPotion, "Drink stamina potion" },
        { Vocab::kActDrinkMagickaPotion, "Drink magicka potion" },
        { Vocab::kActLootArrows,         "Loot arrows" },
        { Vocab::kActLootPotions,        "Loot potions (by condition)" },
        { Vocab::kActLootEquipment,      "Equip better gear only" },
    };
    int cycleIdx(const std::string& op, const VocabEntry* tab, int n, int dir) {
        int cur = 0;
        for (int i = 0; i < n; ++i) if (op == tab[i].op) { cur = i; break; }
        return ((cur + dir) % n + n) % n;
    }
    const char* labelFor(const std::string& op, const VocabEntry* tab, int n) {
        for (int i = 0; i < n; ++i) if (op == tab[i].op) return tab[i].label;
        return op.empty() ? "(unset)" : op.c_str();
    }
    ParamKind kindFor(const std::string& op, const VocabEntry* tab, int n) {
        for (int i = 0; i < n; ++i) if (op == tab[i].op) return tab[i].kind;
        return ParamKind::None;
    }

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

        // Push the selected skin. Returns the count pushed so the caller pops
        // exactly that many (an unbalanced push/pop corrupts every later frame).
        int PushSkin() {
            const int i = std::clamp(Config::g_menuStyle.load(), 0, 3);
            const auto& sk = kSkins[i];
            auto& st = ImGui::GetStyle();
            st.WindowRounding = st.FrameRounding = st.PopupRounding =
                st.ChildRounding = st.TabRounding = st.GrabRounding =
                st.ScrollbarRounding = 0.0f;   // square, always
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
            col(ImGuiCol_SeparatorHovered,sk.accent);
            col(ImGuiCol_NavHighlight,    sk.accent);   // controller focus ring
            return n;
        }

        void DrawFieldKit(const Snapshot& snap) {
            if (g_wantClose.exchange(false)) { g_open = false; return; }

            auto& io = ImGui::GetIO();

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.62f),
                                     ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 400.0f), io.DisplaySize);

            const int skinCols = PushSkin();
            const auto& skin = kSkins[std::clamp(Config::g_menuStyle.load(), 0, 3)];

            if (!ImGui::Begin("MFO Field Kit", nullptr,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::End();
                ImGui::PopStyleColor(skinCols);
                return;
            }

            const auto pv = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
            if (skin.sans) ImGui::TextUnformatted(skin.title);
            else           ImGui::TextUnformatted(skin.title);
            ImGui::PopStyleColor();
            ImGui::TextDisabled("MFO v%u.%u.%u  |  frame %llu  |  %.1f min",
                                pv.major(), pv.minor(), pv.patch(),
                                static_cast<unsigned long long>(snap.frame), snap.minutes);
            ImGui::Separator();

            // LB/RB switch tabs on a controller. ImGui does not do this for a
            // tab bar on its own, so drive it manually: the shoulder edges move
            // a tab index and each BeginTabItem is told to select itself when
            // the index points at it. Gated on !IsAnyItemActive so L1/R1 still
            // serve tweak-fast/slow while a value is being edited. A mouse click
            // resyncs s_tab inside the opened tab body.
            static int s_tab = 0;
            static bool s_tabForce = false;   // apply SetSelected for ONE frame after a shoulder edge
            constexpr int kTabCount = 5;
            // Don't steal L1/R1 while an item is being tweaked (they serve
            // tweak fast/slow) or while a popup is open (a combo would be torn
            // out from under the user).
            if (!ImGui::IsAnyItemActive() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) { s_tab = (s_tab + 1) % kTabCount; s_tabForce = true; }
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) { s_tab = (s_tab + kTabCount - 1) % kTabCount; s_tabForce = true; }
            }
            // SetSelected ONLY on the frame after a shoulder press. Forcing it
            // every frame would re-assert s_tab and revert a mouse click on a
            // different tab (it flashed then snapped back). Between presses the
            // opened tab's body syncs s_tab, so mouse selection sticks.
            auto tabSel = [&](int i) -> ImGuiTabItemFlags {
                return (s_tabForce && s_tab == i) ? ImGuiTabItemFlags_SetSelected : 0;
            };

            if (ImGui::BeginTabBar("##tabs")) {

                if (ImGui::BeginTabItem("Followers", nullptr, tabSel(0))) {
                    s_tab = 0;
                    ImGui::TextDisabled("%zu tracked  (active + retained)", snap.rows.size());
                    ImGui::Spacing();

                    // Reserve the footer, or ScrollY takes the remaining height
                    // and pushes the hint line below the fold.
                    const float footer = ImGui::GetFrameHeightWithSpacing() + 6.0f;
                    if (ImGui::BeginTable("##followers", 7,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY,
                                          ImVec2(0.0f, -footer))) {
                        ImGui::TableSetupColumn("Follower", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("State",   ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableSetupColumn("Rapport", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableSetupColumn("Rank",    ImGuiTableColumnFlags_WidthFixed, 50.0f);
                        ImGui::TableSetupColumn("Slots",   ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        ImGui::TableSetupColumn("H/M/S",   ImGuiTableColumnFlags_WidthFixed, 150.0f);
                        ImGui::TableSetupColumn("Dist",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        ImGui::TableHeadersRow();

                        for (const auto& r : snap.rows) {
                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            if (r.active) ImGui::TextUnformatted(r.name.c_str());
                            else          ImGui::TextDisabled("%s", r.name.c_str());
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%08X", r.id);

                            ImGui::TableNextColumn();
                            if (!r.active) {
                                ImGui::TextDisabled("retained");
                            } else if (r.commanded) {
                                ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "summon");
                            } else if (r.inCombat) {
                                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "IN COMBAT");
                            } else {
                                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "following");
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
                                // will read. Seeing them move is the point.
                                const float w = 42.0f;
                                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                                ImGui::ProgressBar(r.healthPct, ImVec2(w, 0.0f), "");
                                ImGui::PopStyleColor();
                                ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
                                ImGui::ProgressBar(r.magickaPct, ImVec2(w, 0.0f), "");
                                ImGui::PopStyleColor();
                                ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
                                ImGui::ProgressBar(r.staminaPct, ImVec2(w, 0.0f), "");
                                ImGui::PopStyleColor();
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

                // ── THE GAMBIT EDITOR (M7) ──────────────────────────────
                if (ImGui::BeginTabItem("Gambits", nullptr, tabSel(1))) {
                    s_tab = 1;
                    static RE::FormID sel = 0;
                    static int selTable = 0;   // 0 combat, 1 logistics

                    // Follower picker.
                    const FollowerRow* who = nullptr;
                    for (const auto& r : snap.rows) if (r.active && r.id == sel) { who = &r; break; }
                    if (!who) for (const auto& r : snap.rows) if (r.active) { who = &r; sel = r.id; break; }

                    if (!who) {
                        ImGui::TextDisabled("No active follower. Recruit one to edit gambits.");
                    } else {
                        if (ImGui::BeginCombo("Follower", who->name.c_str())) {
                            for (const auto& r : snap.rows) {
                                if (!r.active) continue;
                                ImGui::PushID((int)r.id);
                                if (ImGui::Selectable(r.name.c_str(), r.id == sel)) sel = r.id;
                                if (r.id == sel) ImGui::SetItemDefaultFocus();   // pad opens onto current
                                ImGui::PopID();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("rank %u", who->rank);

                        const bool combat = (selTable == 0);
                        const auto& rules = combat ? who->combat : who->logistics;
                        const int slots = combat ? who->combatSlots : who->logisticsSlots;

                        // The vocabulary for the table being edited.
                        const VocabEntry* condTab = combat ? kCondsCombat : kCondsLogi;
                        const int condN = combat ? (int)std::size(kCondsCombat)
                                                 : (int)std::size(kCondsLogi);
                        const VocabEntry* actTab = combat ? kActsCombat : kActsLogi;
                        const int actN = combat ? (int)std::size(kActsCombat)
                                                : (int)std::size(kActsLogi);

                        if (ImGui::RadioButton("Combat", combat)) selTable = 0;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Logistics", !combat)) selTable = 1;
                        ImGui::SameLine();
                        ImGui::TextDisabled("%d / %d slots used", (int)rules.size(), slots);

                        // Rules top-down = priority order. First match wins.
                        if (ImGui::BeginTable("##rules", 6,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("On",   ImGuiTableColumnFlags_WidthFixed, 34);
                            ImGui::TableSetupColumn("When (condition)", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 104);
                            ImGui::TableSetupColumn("Do (action)",      ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Spell",ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 118);
                            ImGui::TableHeadersRow();

                            for (int i = 0; i < (int)rules.size(); ++i) {
                                const auto& rv = rules[i];
                                ImGui::TableNextRow();
                                ImGui::PushID((int)rv.uid);

                                ImGui::TableNextColumn();
                                bool en = rv.enabled;
                                if (ImGui::Checkbox("##en", &en))
                                    QueueEdit({ EditKind::Toggle, sel, selTable, rv.uid, 0 });

                                // Condition -- cycle with < >
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("<")) QueueEdit({ EditKind::CycleCond, sel, selTable, rv.uid, -1 });
                                ImGui::SameLine();
                                if (ImGui::SmallButton(">")) QueueEdit({ EditKind::CycleCond, sel, selTable, rv.uid,  1 });
                                ImGui::SameLine();
                                ImGui::TextUnformatted(labelFor(rv.condOp, condTab, condN));
                                if (rv.lastFired) { ImGui::SameLine(); ImGui::TextColored(
                                    ImVec4(0.4f,0.9f,0.4f,1), "*"); }

                                // Param cell. The widget matches what the param
                                // MEANS (marth's rule): a percentage is a %-slider,
                                // a count is a whole number, a param-less condition
                                // shows a dash. FLANKED BY < > STEPPERS so a
                                // controller sets any value without entering
                                // tweak mode -- the same nav-focusable SmallButton
                                // the condition/action cyclers use. Mouse-drag and
                                // double-click-to-type still work on the middle.
                                ImGui::TableNextColumn();
                                switch (kindFor(rv.condOp, condTab, condN)) {
                                case ParamKind::Percent: {
                                    if (ImGui::SmallButton("<##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    std::clamp(rv.param - 0.05f, 0.0f, 1.0f) });
                                    ImGui::SameLine(0, 2);
                                    ImGui::SetNextItemWidth(46);
                                    float pct = std::clamp(rv.param, 0.0f, 1.0f) * 100.0f;
                                    if (ImGui::DragFloat("##p", &pct, 0.5f, 0.0f, 100.0f, "%.0f%%"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    std::clamp(pct, 0.0f, 100.0f) / 100.0f });
                                    ImGui::SameLine(0, 2);
                                    if (ImGui::SmallButton(">##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    std::clamp(rv.param + 0.05f, 0.0f, 1.0f) });
                                    break; }
                                case ParamKind::Count: {
                                    // Min 1: "fewer than 0" can never be true.
                                    int n = (int)(rv.param + 0.5f);   // counts are whole
                                    if (ImGui::SmallButton("<##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(n - 1, 1, 999) });
                                    ImGui::SameLine(0, 2);
                                    ImGui::SetNextItemWidth(46);
                                    if (ImGui::DragInt("##p", &n, 0.25f, 1, 999))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(n, 1, 999) });
                                    ImGui::SameLine(0, 2);
                                    if (ImGui::SmallButton(">##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(n + 1, 1, 999) });
                                    break; }
                                case ParamKind::Distance: {
                                    // Whole units, wider scale (step 50, cap ~5000).
                                    int u = (int)(rv.param + 0.5f);
                                    if (ImGui::SmallButton("<##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(u - 50, 0, 5000) });
                                    ImGui::SameLine(0, 2);
                                    ImGui::SetNextItemWidth(46);
                                    if (ImGui::DragInt("##p", &u, 5.0f, 0, 5000, "%du"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(u, 0, 5000) });
                                    ImGui::SameLine(0, 2);
                                    if (ImGui::SmallButton(">##pv"))
                                        QueueEdit({ EditKind::SetParam, sel, selTable, rv.uid,
                                                    (float)std::clamp(u + 50, 0, 5000) });
                                    break; }
                                default:
                                    ImGui::TextDisabled("-");
                                    break;
                                }

                                // Action -- cycle
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("<##a")) QueueEdit({ EditKind::CycleAct, sel, selTable, rv.uid, -1 });
                                ImGui::SameLine();
                                if (ImGui::SmallButton(">##a")) QueueEdit({ EditKind::CycleAct, sel, selTable, rv.uid,  1 });
                                ImGui::SameLine();
                                ImGui::TextUnformatted(labelFor(rv.actOp, actTab, actN));

                                // Spell picker -- only meaningful for cast
                                // actions. Lists the follower's own known spells.
                                ImGui::TableNextColumn();
                                const bool isCast = (rv.actOp == Vocab::kActCastSelf ||
                                                     rv.actOp == Vocab::kActCastTarget);
                                if (!isCast) {
                                    ImGui::TextDisabled("-");
                                } else {
                                    const char* cur = rv.spellName.empty() ? "(pick)" : rv.spellName.c_str();
                                    ImGui::SetNextItemWidth(-1);
                                    if (ImGui::BeginCombo("##sp", cur)) {
                                        for (const auto& [sid, sname] : who->knownSpells) {
                                            if (ImGui::Selectable(sname.c_str(), sid == rv.spell)) {
                                                EditCmd e{ EditKind::SetSpell, sel, selTable, rv.uid, 0 };
                                                e.spell = sid; QueueEdit(e);
                                            }
                                        }
                                        if (who->knownSpells.empty())
                                            ImGui::TextDisabled("follower knows no spells");
                                        ImGui::EndCombo();
                                    }
                                    if (!rv.fail.empty() && ImGui::IsItemHovered())
                                        ImGui::SetTooltip("last: %s", rv.fail.c_str());
                                }

                                // Reorder / delete. The up/dn buttons are
                                // nav-focusable, so reorder is pad-reachable
                                // (the §6.5 floor); a dedicated L1/R1 binding is
                                // a later refinement.
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton(" up ")) QueueEdit({ EditKind::MoveUp, sel, selTable, rv.uid, 0 });
                                ImGui::SameLine();
                                if (ImGui::SmallButton("dn"))  QueueEdit({ EditKind::MoveDown, sel, selTable, rv.uid, 0 });
                                ImGui::SameLine();
                                // Destructive -> two-click arm (§6.6). First click
                                // arms this rule (danger colour); a second within the
                                // window deletes; any other rule disarms it.
                                static std::uint32_t s_armed = 0;
                                const bool armed = (s_armed == rv.uid);
                                if (armed) ImGui::PushStyleColor(ImGuiCol_Button, skin.danger);
                                if (ImGui::SmallButton(armed ? "sure?" : "del")) {
                                    if (armed) { QueueEdit({ EditKind::Del, sel, selTable, rv.uid, 0 }); s_armed = 0; }
                                    else s_armed = rv.uid;
                                }
                                if (armed) ImGui::PopStyleColor();

                                ImGui::PopID();
                            }
                            ImGui::EndTable();
                        }

                        const bool full = (int)rules.size() >= slots;
                        ImGui::BeginDisabled(full);
                        if (ImGui::Button("+ Add rule"))
                            QueueEdit({ EditKind::Add, sel, selTable, 0u, 0 });
                        ImGui::EndDisabled();
                        if (full) { ImGui::SameLine();
                            ImGui::TextDisabled("all %d slots used -- more unlock with rapport", slots); }

                        ImGui::Spacing();
                        ImGui::TextDisabled("Top rule wins -- order is priority. A green * fired last tick. "
                                            "Set a value with < >, drag, or double-click to type.");
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Measurements", nullptr, tabSel(2))) {
                    s_tab = 2;
                    ImGui::TextDisabled("The two numbers this build exists to take.");
                    ImGui::Spacing();

                    const double perMin = snap.minutes > 0.01 ? snap.combatEvents / snap.minutes : 0.0;
                    ImGui::Text("Combat events (teammate-filtered): %u", snap.combatEvents);
                    ImGui::SameLine(); ImGui::TextDisabled("(%.1f/min)", perMin);
                    ImGui::TextWrapped("TESCombatEvent is a global source firing for every actor in the "
                                       "load order. If this climbs fast in a big fight, the teammate "
                                       "filter is not enough and combat-exit needs to move off the sink.");

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    const double kph = snap.minutes > 0.01 ? snap.kills   * 60.0 / snap.minutes : 0.0;
                    const double rph = snap.minutes > 0.01 ? snap.rapport * 60.0 / snap.minutes : 0.0;
                    ImGui::Text("Kills: %u", snap.kills);
                    ImGui::SameLine(); ImGui::TextDisabled("(%.1f/hr)", kph);
                    ImGui::Text("Rapport this session: %u", snap.rapport);
                    ImGui::SameLine();
                    if (rph > 0.0 && rph < 30.0) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(%.1f/hr)", rph);
                    } else {
                        ImGui::TextDisabled("(%.1f/hr)", rph);
                    }
                    ImGui::TextWrapped("BALANCE.md assumes ~45 rapport/hr and that number has NEVER been "
                                       "measured. At half of it, Rank V is ~220 hours and the ladder "
                                       "needs redoing while it is still free to change.");

                    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

                    // Explains a classification without needing the log.
                    ImGui::TextDisabled("Last credited kill");
                    if (!snap.lastValid) {
                        ImGui::TextDisabled("  (none yet)");
                    } else {
                        ImGui::Text("  %s  lvl %u  (you: %u)", snap.lastKillName.c_str(),
                                    snap.lastVictimLevel, snap.lastPlayerLevel);
                        if (snap.lastKillKind == "boss") {
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "  BOSS  x%.0f", snap.bossMult);
                        } else if (snap.lastKillKind == "dragon") {
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "  DRAGON  x%.0f", snap.dragonMult);
                        } else {
                            ImGui::Text("  standard");
                            ImGui::TextDisabled("  boss needs unique, or level >= yours + %d", snap.bossLevelDelta);
                        }
                        ImGui::Text("  awarded %.1f to %d follower(s)", snap.lastAwarded, snap.lastCredited);
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Probe", nullptr, tabSel(3))) {
                    s_tab = 3;
                    ImGui::TextWrapped("M4: fire one engine primitive at a follower and watch what "
                                       "happens. These answer questions no source states -- they are "
                                       "emergent engine behaviour. Nothing here persists.");
                    ImGui::Spacing();

                    // Subject keyed on IDENTITY, never an index (INVARIANTS #31).
                    // The active list is rebuilt per frame and reorders when the
                    // roster changes -- an index would silently retarget the
                    // probe at whoever now occupies that slot.
                    static RE::FormID selId = 0;
                    std::vector<const FollowerRow*> active;
                    for (const auto& r : snap.rows) if (r.active) active.push_back(&r);

                    if (active.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                                           "No active follower. Recruit one first.");
                    } else {
                        const FollowerRow* cur = nullptr;
                        for (const auto* r : active) if (r->id == selId) { cur = r; break; }
                        if (!cur) { cur = active.front(); selId = cur->id; }

                        if (ImGui::BeginCombo("Subject", cur->name.c_str())) {
                            for (const auto* r : active) {
                                const bool chosen = (r->id == selId);
                                if (ImGui::Selectable(r->name.c_str(), chosen)) selId = r->id;
                                if (chosen) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        const RE::FormID subject = selId;

                        ImGui::Spacing();
                        // Single-shot via IsItemActivated equivalent: Button
                        // returns true once per click, which is the safe form.
                        // ImGui::Button fires once per physical click, on
                        // release. That is not the Selectable-return ||
                        // IsItemClicked double-fire shape INVARIANTS #30 bans.
                        auto fire = [&](Probe::Action a) {
                            if (ImGui::Button(Probe::Name(a), ImVec2(-1.0f, 0.0f))) {
                                SKSE::GetTaskInterface()->AddTask([subject, a]() {
                                    Probe::Fire(subject, a);
                                });
                            }
                            const char* b = Probe::Blurb(a);
                            if (b && *b) ImGui::TextDisabled("  %s", b);
                        };

                        ImGui::SeparatorText("Combat targeting");
                        fire(Probe::Action::StartCombatOnNearestFoe);
                        fire(Probe::Action::StopCombat);

                        ImGui::SeparatorText("Casting");
                        fire(Probe::Action::CommandTargetAtCrosshair);
                        fire(Probe::Action::ClearCommandedTarget);
                        fire(Probe::Action::CastHealInstant);
                        fire(Probe::Action::CastHealRightHand);
                        fire(Probe::Action::CastHealLeftHand);
                        fire(Probe::Action::CastHealOther);

                        ImGui::SeparatorText("Packages / stance");
                        fire(Probe::Action::EvaluatePackage);
                        fire(Probe::Action::DrawWeapon);
                        fire(Probe::Action::SheatheWeapon);

                        // The gap M4 found before it ever ran. Shown here rather
                        // than silently omitted -- a probe that hides what it
                        // cannot reach hides its most important result.
                        ImGui::SeparatorText("Not reachable from C++");
                        for (const auto& u : Probe::UnavailableActions()) {
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", u.name);
                            ImGui::TextDisabled("    %s", u.why);
                        }
                        ImGui::TextWrapped("DESIGN 4.5 lists these as Tier B. The Papyrus surface named "
                                           "the right flows, but CommonLibSSE-NG does not bind them -- "
                                           "reaching them needs VM dispatch or a sourced relocation, "
                                           "which is its own milestone.");
                    }

                    ImGui::Spacing(); ImGui::Separator();
                    const auto last = Probe::GetLast();
                    ImGui::TextDisabled("Last probe");
                    if (!last.valid) {
                        ImGui::TextDisabled("  (none yet)");
                    } else {
                        ImGui::Text("  %s on %s", last.action.c_str(), last.subject.c_str());
                        if (last.ok) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "  %s", last.detail.c_str());
                        else         ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "  %s", last.detail.c_str());
                    }

                    ImGui::Spacing(); ImGui::Separator();
                    const auto ret = Probe::GetRetention();
                    ImGui::TextDisabled("Target retention  <-- THE question for DESIGN 4.7");
                    if (!ret.valid) {
                        ImGui::TextDisabled("  not running -- use StartCombat above");
                    } else {
                        ImGui::Text("  %s commanded to attack %s", ret.follower.c_str(), ret.commanded.c_str());
                        ImGui::Text("  now attacking: %s", ret.current.c_str());
                        ImGui::Text("  watching %.1fs, %d sample(s)", ret.watchSeconds, ret.samples);
                        if (!ret.everEngaged) {
                            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                               "  never entered combat -- StartCombat did not take");
                        } else if (ret.commandedDied) {
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                                               "  commanded target DIED -- invalidation, not a re-pick");
                        } else if (ret.changes == 0) {
                            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f),
                                               "  0 changes -- the order STICKS (4.7's model holds)");
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                               "  %d change(s), held commanded target %.1fs",
                                               ret.changes, ret.heldSeconds);
                            ImGui::TextDisabled("    engine re-picked -- 4.7 needs a refresh cadence");
                        }
                        ImGui::TextDisabled("  on commanded target: %s", ret.onCommanded ? "yes" : "NO");
                        ImGui::TextDisabled("  (500ms sampling cannot see a re-pick-and-return inside one tick)");
                        if (!ret.active) ImGui::TextDisabled("  (watch ended)");
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Config", nullptr, tabSel(4))) {
                    s_tab = 4;
                    // SKIN -- live, no reload (§6.7a). g_menuStyle is an atomic,
                    // so a render-thread store is fine; PushSkin reads it next
                    // frame.
                    ImGui::TextUnformatted("Skin");
                    int cur = std::clamp(Config::g_menuStyle.load(), 0, 3);
                    for (int k = 0; k < 4; ++k) {
                        if (ImGui::RadioButton(kSkins[k].name, cur == k))
                            Config::g_menuStyle.store(k);
                        if (k < 3) ImGui::SameLine();
                    }
                    ImGui::Separator();
                    ImGui::Text("rate         %.2f", snap.rate);
                    ImGui::Text("per kill     %.2f", snap.killVal);
                    ImGui::Text("boss mult    %.1fx", snap.bossMult);
                    ImGui::Text("dragon mult  %.1fx", snap.dragonMult);
                    ImGui::Text("shared radius %.0f", snap.radius);
                    ImGui::Text("summons      %s", snap.allowSummons ? "allowed" : "excluded");
                    ImGui::Spacing();
                    ImGui::Text("ranks        %d / %d / %d / %d",
                                snap.rank2, snap.rank3, snap.rank4, snap.rank5);
                    ImGui::Spacing();
                    ImGui::Text("quirk table  %d active, %d inactive",
                                snap.quirksActive, snap.quirksInactive);
                    ImGui::TextDisabled("Inactive means that follower's plugin is not in this load "
                                        "order. Normal, not an error.");
                    ImGui::EndTabItem();
                }

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
            if (ImGui::IsAnyItemActive())
                ImGui::TextDisabled("[A]/E confirm   [B]/Esc cancel   < > adjust   (drag or type too)");
            else if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                ImGui::TextDisabled("[A]/E select   [B]/Esc close   d-pad to move");
            else
                ImGui::TextDisabled("[A]/E select   [B]/Esc close   [LB]/[RB] tabs   d-pad move   -   Skin: %s (Config)",
                                    skin.name);
            ImGui::End();
            ImGui::PopStyleColor(skinCols);
        }

        // The single-monitor problem: with the full panel open, input is
        // swallowed, so you cannot watch it WHILE fighting -- which is exactly
        // when rapport ticks and vitals move. This compact readout draws every
        // frame and takes NO input, so it can sit on screen during combat.
        void DrawHud(const Snapshot& snap) {
            const auto& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always,
                                    ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.42f);
            const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoMove;

            if (ImGui::Begin("##mfohud", nullptr, flags)) {
                ImGui::TextDisabled("MFO");
                ImGui::SameLine();
                // SAY "session". This counter is rapport earned SINCE LOAD, and
                // it sits one line above each follower's LIFETIME rapport. Read
                // as "0 rap" next to a follower showing 5, it looks like the
                // mod lost the save (marth, 2026-07-21 -- exactly that report).
                // "0 eval" is the single most diagnostic number on screen: it
                // separates "no rule matched" from "the evaluator is dead".
                if (snap.evalTicks == 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "[eval: NEVER RAN]");
                } else {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[eval %u tk %.2fms]", snap.evalTicks, snap.evalMs);
                }
                ImGui::TextDisabled("| session %u kill  %u rap  %.0f/hr", snap.kills, snap.rapport,
                                    snap.minutes > 0.01 ? snap.rapport * 60.0 / snap.minutes : 0.0);
                ImGui::Separator();

                int shown = 0;
                for (const auto& r : snap.rows) {
                    if (!r.active) continue;
                    ++shown;
                    ImGui::Text("%-16s", r.name.c_str());
                    ImGui::SameLine();
                    if (r.inCombat) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "[C]");
                    else            ImGui::TextDisabled("[ ]");
                    ImGui::SameLine();
                    ImGui::Text("R%u %u total", r.rank, r.rapport);

                    const float w = 54.0f;
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
                    ImGui::ProgressBar(r.healthPct, ImVec2(w, 4.0f), "");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, 3.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
                    ImGui::ProgressBar(r.magickaPct, ImVec2(w, 4.0f), "");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, 3.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
                    ImGui::ProgressBar(r.staminaPct, ImVec2(w, 4.0f), "");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%.0f", r.distance);
                }
                if (shown == 0) ImGui::TextDisabled("no followers");
            }
            ImGui::End();
        }

        // ── hooks ───────────────────────────────────────────────────────────
        struct WndProcHook {
            static LRESULT thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
                if (uMsg == WM_KILLFOCUS && g_ready.load()) {
                    std::scoped_lock lk(g_ioMx);
                    ImGui::GetIO().ClearInputKeys();
                }
                // TEXT INPUT. The Skyrim ButtonEvent sink delivers KEY codes, not
                // characters, so a text box (double-click a value to type it) got
                // no glyphs. WM_CHAR carries the translated character -- the same
                // path the game's console and name fields use, so it is always
                // pumped -- and ImGui wants it via AddInputCharacter. Only while
                // the panel is open, and only when a widget actually wants text,
                // so ordinary gameplay keypresses are never captured.
                else if (uMsg == WM_CHAR && g_open.load() && g_ready.load()) {
                    std::scoped_lock lk(g_ioMx);
                    auto& io = ImGui::GetIO();
                    if (io.WantTextInput && wParam > 0 && wParam < 0x10000)
                        io.AddInputCharacterUTF16(static_cast<ImWchar16>(wParam));
                }
                return func(hWnd, uMsg, wParam, lParam);
            }
            static inline WNDPROC func;
        };

        struct D3DInitHook {
            static void thunk() {
                func();

                auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
                if (!renderer) { spdlog::error("[board] no renderer -- Field Kit disabled"); return; }

                auto* swapChain = renderer->data.renderWindows[0].swapChain;
                if (!swapChain) { spdlog::error("[board] no swapchain -- Field Kit disabled"); return; }

                DXGI_SWAP_CHAIN_DESC sd{};
                if (FAILED(swapChain->GetDesc(&sd))) {
                    spdlog::error("[board] GetDesc failed -- Field Kit disabled");
                    return;
                }

                // No casts: on the pinned NG these are already the real D3D
                // types. Decorative reinterpret_casts hide a future type change.
                g_device  = renderer->data.forwarder;
                g_context = renderer->data.context;
                if (!g_device || !g_context) {
                    spdlog::error("[board] no device/context -- Field Kit disabled");
                    return;
                }

                ImGui::CreateContext();
                auto& io = ImGui::GetIO();
                io.IniFilename = nullptr;    // never write imgui.ini into the game dir
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

                if (!ImGui_ImplWin32_Init(sd.OutputWindow) ||
                    !ImGui_ImplDX11_Init(g_device, g_context)) {
                    spdlog::error("[board] ImGui backend init failed -- Field Kit disabled");
                    return;
                }

                WndProcHook::func = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrA(sd.OutputWindow, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));

                g_bbW = static_cast<float>(sd.BufferDesc.Width);
                g_bbH = static_cast<float>(sd.BufferDesc.Height);

                g_ready.store(true);
                spdlog::info("[board] Field Kit ready ({}x{})", sd.BufferDesc.Width, sd.BufferDesc.Height);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        struct DXGIPresentHook {
            static void thunk(std::uint32_t a_p1) {
                func(a_p1);

                const bool wantPanel = g_open.load();
                const bool wantHud   = g_hud.load();
                if (!g_ready.load() || (!wantPanel && !wantHud)) return;

                // Copy the snapshot BEFORE taking the IO lock. INVARIANTS #6
                // says these two are never nested; MEO's shipped code actually
                // does nest them, but the rule as written is the stronger one
                // and the first person to touch ImGui IO inside PublishSnapshot
                // would deadlock the render thread. Make the code match the doc.
                Snapshot snap;
                {
                    std::scoped_lock snapLk(g_snapMx);
                    snap = g_snapshot;
                }

                std::scoped_lock lk(g_ioMx);

                // B1: drive the software cursor PER FRAME from panel state. Set
                // once at init it renders an ImGui arrow over ordinary gameplay
                // for the whole session, because the HUD draws every frame.
                ImGui::GetIO().MouseDrawCursor = wantPanel;

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();

                // MUST sit between the two NewFrame calls (ENGINE_NOTES §9).
                if (g_bbW > 0.0f) ImGui::GetIO().DisplaySize = ImVec2(g_bbW, g_bbH);

                if (g_cursorInit.exchange(false)) {
                    ImGui::GetIO().AddMousePosEvent(g_cursorX.load(), g_cursorY.load());
                }

                ImGui::NewFrame();
                if (wantHud)   DrawHud(snap);
                if (wantPanel) DrawFieldKit(snap);
                ImGui::EndFrame();
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        struct InputDispatchHook {
            static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events) {
                // THE FOCUS HOTKEY IS HANDLED BEFORE THE PANEL CHECK, and that
                // is the entire point: a menu button cannot be a target picker,
                // because opening the menu takes the mouse and freezes the
                // crosshair you were supposed to be aiming. This runs while the
                // panel is CLOSED, and deliberately does not swallow the event --
                // the default key is unbound in vanilla, so the game can have it.
                if (a_events && !g_open.load()) {
                    const int fk = Config::g_focusKey.load();
                    if (fk != 0) {
                        for (auto* e = *a_events; e; e = e->next) {
                            if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                            auto* b = static_cast<RE::ButtonEvent*>(e);
                            if (!b->IsDown()) continue;                 // edge only
                            if (b->device.get() != RE::INPUT_DEVICE::kKeyboard) continue;
                            if (static_cast<int>(b->GetIDCode()) != fk) continue;
                            SKSE::GetTaskInterface()->AddTask([]() { Probe::FocusOnCrosshair(); });
                        }
                    }
                }

                if (!g_ready.load() || !g_open.load() || !a_events) {
                    func(a_source, a_events);
                    return;
                }

                std::unique_lock ioLk(g_ioMx);
                auto& io = ImGui::GetIO();

                for (auto* e = *a_events; e; e = e->next) {
                    if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                        auto* b = static_cast<RE::ButtonEvent*>(e);
                        // Skyrim re-fires button events every input frame while a
                        // key is HELD. Filter to real edges at the source rather
                        // than relying on ImGui's dedupe (MEO's filter, verbatim).
                        if (!b->IsDown() && !b->IsUp()) continue;
                        const auto code = b->GetIDCode();
                        const bool down = b->IsDown();

                        // Is ImGui itself using the input right now -- a text box
                        // is active, a combo/popup is open, OR a widget is in
                        // tweak mode (a value drag the pad activated with A)?
                        // Then the BACK key (Esc / gamepad B) must CANCEL that
                        // widget, not close the whole panel. IsAnyItemActive is
                        // the one that covers a DragFloat/DragInt being nudged --
                        // without it, pressing B to back out of a value edit
                        // closed the entire board (the §6.5 controller-parity
                        // floor). Read under the io lock we already hold, so it
                        // is race-free against the render thread.
                        const bool imguiBusy = io.WantTextInput ||
                            ImGui::IsAnyItemActive() ||
                            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                        ImGuiPopupFlags_AnyPopupLevel);

                        switch (b->device.get()) {
                        case RE::INPUT_DEVICE::kMouse:
                            if (code <= 4) io.AddMouseButtonEvent(static_cast<int>(code), down);
                            else if ((code == 8 || code == 9) && down)
                                io.AddMouseWheelEvent(0.0f, code == 8 ? 1.0f : -1.0f);
                            break;

                        case RE::INPUT_DEVICE::kKeyboard:
                            if (code == 0x01 || code == 0x0F) {          // Esc / Tab
                                // A PRESS with nothing open closes the panel;
                                // every other edge drives ImGui's Escape (cancel
                                // the popup/text box). Routing ALL releases to
                                // ImGui -- even when we are no longer busy -- is
                                // deliberate: the popup often closes between the
                                // press and the release, and a dropped release
                                // would leave ImGuiKey_Escape stuck down for the
                                // session, killing cancel after one use.
                                if (down && !imguiBusy) g_wantClose = true;
                                else io.AddKeyEvent(ImGuiKey_Escape, down);
                            } else if (code == ShoutKey(RE::INPUT_DEVICE::kKeyboard)) {
                                // Close on RELEASE, swallow both edges. Closing on
                                // the press leaks the release to the game, which
                                // re-casts the power and instantly reopens.
                                if (down) g_shoutDownSeen = true;
                                else if (g_shoutDownSeen.exchange(false)) g_wantClose = true;
                            } else if (auto k = DIKToImGuiKey(code); k != ImGuiKey_None) {
                                io.AddKeyEvent(k, down);
                            }
                            break;

                        case RE::INPUT_DEVICE::kGamepad:
                            if (static_cast<RE::BSWin32GamepadDevice::Key>(code) ==
                                    RE::BSWin32GamepadDevice::Key::kB) {
                                // Same symmetric routing as keyboard Esc -- a
                                // press with nothing open closes the panel, all
                                // other edges drive ImGui Escape, and every
                                // release reaches ImGui so B-cancel never sticks.
                                if (down && !imguiBusy) g_wantClose = true;
                                else io.AddKeyEvent(ImGuiKey_Escape, down);
                            } else if (code == ShoutKey(RE::INPUT_DEVICE::kGamepad)) {
                                if (down) g_shoutDownSeen = true;
                                else if (g_shoutDownSeen.exchange(false)) g_wantClose = true;
                            } else if (auto k = GamepadToImGuiKey(code); k != ImGuiKey_None) {
                                io.AddKeyEvent(k, down);
                            }
                            break;

                        default:
                            break;
                        }
                    } else if (e->eventType == RE::INPUT_EVENT_TYPE::kMouseMove) {
                        // The engine gives DELTAS, not positions.
                        auto* m = static_cast<RE::MouseMoveEvent*>(e);
                        const float x = std::clamp(g_cursorX.load() + static_cast<float>(m->mouseInputX),
                                                   0.0f, io.DisplaySize.x);
                        const float y = std::clamp(g_cursorY.load() + static_cast<float>(m->mouseInputY),
                                                   0.0f, io.DisplaySize.y);
                        g_cursorX = x; g_cursorY = y;
                        io.AddMousePosEvent(x, y);
                    } else if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
                        auto* th = static_cast<RE::ThumbstickEvent*>(e);
                        if (th->IsLeft()) {
                            // EDGE-TRIGGERED into d-pad keys. ImGui's own nav
                            // repeat handles a held direction; passing the axis
                            // through would scroll continuously.
                            auto edge = [&](int i, bool on, ImGuiKey key) {
                                if (g_stickNav[i] != on) { g_stickNav[i] = on; io.AddKeyEvent(key, on); }
                            };
                            edge(0, th->yValue >  0.5f, ImGuiKey_GamepadDpadUp);
                            edge(1, th->yValue < -0.5f, ImGuiKey_GamepadDpadDown);
                            edge(2, th->xValue < -0.5f, ImGuiKey_GamepadDpadLeft);
                            edge(3, th->xValue >  0.5f, ImGuiKey_GamepadDpadRight);
                        }
                    }
                }

                // Swallow everything: the game sees no input while we are open,
                // so no vanilla menu bleed-through and no control-flag toggling.
                // NULL THE CALLER'S OWN HEAD POINTER, exactly as MEO does --
                // handing the engine a stack-local instead leaves the caller's
                // list intact and changes what a chained hook at the same site
                // observes.
                *a_events = nullptr;
                ioLk.unlock();          // never hold the IO lock across the passthrough
                func(a_source, a_events);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        template <class T>
        void WriteThunkCall(REL::RelocationID a_id, REL::VariantOffset a_off) {
            auto& trampoline = SKSE::GetTrampoline();
            const REL::Relocation<std::uintptr_t> hook{ a_id, a_off };
            T::func = trampoline.write_call<5>(hook.address(), T::thunk);
        }

    }

    bool IsAvailable() { return g_ready.load(); }
    bool IsOpen()      { return g_open.load(); }
    void ClearPendingEdits() { std::scoped_lock lk(g_editMx); g_edits.clear(); }

    void ToggleHud() {
        const bool now = !g_hud.load();
        g_hud.store(now);
        spdlog::info("[board] HUD {}", now ? "on" : "off");
    }
    void SetHud(bool a_on) { g_hud.store(a_on); }

    void Toggle() {
        if (!g_ready.load()) return;
        const bool now = !g_open.load();
        if (now) {
            for (bool& s : g_stickNav) s = false;   // no stuck direction from last time
            g_shoutDownSeen = false;                // the opening press's RELEASE must not close it
            g_cursorInit = true;                    // seed the cursor or the first click misses
            g_cursorX = g_bbW * 0.5f;
            g_cursorY = g_bbH * 0.5f;
        }
        g_open.store(now);
        spdlog::info("[board] {}", now ? "opened" : "closed");
    }

    // Copy a live rule table into flat views for the render thread.
    static void FillRuleViews(std::vector<RuleView>& a_out,
                              const std::vector<Gambit>& a_rules) {
        a_out.clear();
        a_out.reserve(a_rules.size());
        for (const auto& g : a_rules) {
            RuleView v;
            v.uid = g.uid;
            v.condOp = g.conditionOpcode; v.actOp = g.actionOpcode;
            v.param  = g.conditionParam;  v.spell = g.actionParamForm;
            v.enabled = g.enabled;        v.lastFired = g.lastFired; v.fail = g.lastFailReason;
            if (v.spell) {
                if (auto* sp = RE::TESForm::LookupByID(v.spell))
                    v.spellName = sp->GetName() ? sp->GetName() : "?";
            }
            a_out.push_back(std::move(v));
        }
    }

    // Apply queued edits. MAIN THREAD ONLY (called from PublishSnapshot).
    // Give every rule a uid (main thread). Covers seeds, co-save loads and
    // adds -- the board resolves edits by uid, so an unassigned rule must never
    // reach the snapshot.
    void EnsureRuleUIDs() {
        for (auto& [fid, st] : g_followers)
            for (auto& tab : st.tables)
                for (auto& g : tab)
                    if (g.uid == 0) g.uid = NextRuleUID();
    }

    void ApplyEdits() {
        std::vector<EditCmd> todo;
        { std::scoped_lock lk(g_editMx); todo.swap(g_edits); }
        if (todo.empty()) return;
        for (const auto& c : todo) {
            auto it = g_followers.find(c.fid);
            if (it == g_followers.end()) continue;
            const int table = std::clamp(c.table, 0, 1);
            auto& tab = it->second.tables[table];

            if (c.kind == EditKind::Add) {
                // Re-check the slot cap on the MAIN thread -- the draw's gate
                // read a stale snapshot, so two Adds in one window could both
                // pass it and overflow the rank's slots (silently truncated at
                // next load, #11).
                const int slots = SlotsForRank(it->second.rank, static_cast<Table>(table));
                if ((int)tab.size() < slots) {
                    Gambit g; g.uid = NextRuleUID();
                    // Default to a rule that is VALID in this table's vocabulary
                    // -- a logistics rule seeded with the combat-only kActWait
                    // would show a raw opcode and cycle from nowhere.
                    g.conditionOpcode = Vocab::kCondAlways;
                    g.actionOpcode = (table == 1) ? Vocab::kActLootEquipment : Vocab::kActWait;
                    tab.push_back(g);
                }
                continue;
            }

            // Resolve by IDENTITY, not index (#31). A command whose rule is
            // gone -- deleted, reseeded -- is dropped, never misapplied.
            int i = -1;
            for (int k = 0; k < (int)tab.size(); ++k) if (tab[k].uid == c.uid) { i = k; break; }
            if (i < 0) continue;

            switch (c.kind) {
            case EditKind::Del:    tab.erase(tab.begin()+i); break;
            case EditKind::MoveUp: if (i > 0) std::swap(tab[i], tab[i-1]); break;
            case EditKind::MoveDown: if (i+1 < (int)tab.size()) std::swap(tab[i], tab[i+1]); break;
            case EditKind::Toggle: tab[i].enabled = !tab[i].enabled; break;
            case EditKind::CycleCond: {
                // Cycle within THIS table's vocabulary, so logistics rules reach
                // logistics conditions and combat rules reach combat ones.
                const VocabEntry* t = (table == 1) ? kCondsLogi : kCondsCombat;
                const int tn = (table == 1) ? (int)std::size(kCondsLogi)
                                            : (int)std::size(kCondsCombat);
                int n = cycleIdx(tab[i].conditionOpcode, t, tn, (int)c.param);
                tab[i].conditionOpcode = t[n].op; break; }
            case EditKind::CycleAct: {
                const VocabEntry* t = (table == 1) ? kActsLogi : kActsCombat;
                const int tn = (table == 1) ? (int)std::size(kActsLogi)
                                            : (int)std::size(kActsCombat);
                int n = cycleIdx(tab[i].actionOpcode, t, tn, (int)c.param);
                tab[i].actionOpcode = t[n].op; break; }
            case EditKind::SetParam: tab[i].conditionParam = c.param; break;
            case EditKind::SetSpell: tab[i].actionParamForm = c.spell; break;
            default: break;
            }
        }
    }

    void PublishSnapshot() {
        ApplyEdits();       // main-thread: fold in whatever the editor queued
        EnsureRuleUIDs();   // every rule has a stable id before it is snapshotted
        Snapshot s;
        s.frame = g_frame.fetch_add(1);

        s.combatEvents = Rapport::CombatEventCount();
        s.minutes      = Rapport::SessionMinutes();
        s.kills        = Rapport::SessionKills();
        s.rapport      = Rapport::SessionRapport();
        s.evalTicks    = Scheduler::TicksThisSession();
        s.evalMs       = Scheduler::LastTickMs();

        s.rate         = Config::g_rapportRate.load();
        s.killVal      = Config::g_rapportKill.load();
        s.bossMult     = Config::g_rapportBossMult.load();
        s.dragonMult   = Config::g_rapportDragonMult.load();
        s.radius       = Config::g_sharedRadius.load();
        s.allowSummons = Config::g_allowSummons.load();
        s.rank2 = Config::g_rank2.load(); s.rank3 = Config::g_rank3.load();
        s.rank4 = Config::g_rank4.load(); s.rank5 = Config::g_rank5.load();
        {
            const auto lk = Rapport::GetLastKill();
            s.lastKillName    = lk.name;
            s.lastKillKind    = lk.kind;
            s.lastVictimLevel = lk.victimLevel;
            s.lastPlayerLevel = lk.playerLevel;
            s.lastAwarded     = lk.awarded;
            s.lastCredited    = lk.credited;
            s.lastValid       = lk.valid;
        }
        s.bossLevelDelta = Config::g_bossLevelDelta.load();
        s.quirksActive   = Followers::QuirksActive();
        s.quirksInactive = Followers::QuirksInactive();

        auto* player = RE::PlayerCharacter::GetSingleton();

        // Active followers first, with live vitals.
        for (const auto& h : Followers::g_active) {
            auto* a = h.get().get();
            if (!a) continue;
            FollowerRow r;
            r.id        = a->GetFormID();
            r.name      = a->GetName();
            r.active    = true;
            r.teammate  = a->IsPlayerTeammate();
            r.commanded = a->IsCommandedActor();
            r.inCombat  = a->IsInCombat();

            // ONE formula, shared with the evaluator. These bars are how the
            // player checks why a rule did or did not fire, so a HUD that
            // computed HP% differently from the thing deciding would make the
            // Field Kit lie at exactly the moment it is consulted.
            r.healthPct  = Vocab::HealthPct(a);
            r.magickaPct = Vocab::MagickaPct(a);
            r.staminaPct = Vocab::StaminaPct(a);
            if (player) r.distance = a->GetPosition().GetDistance(player->GetPosition());

            if (auto it = g_followers.find(r.id); it != g_followers.end()) {
                r.rapport        = it->second.rapport;
                r.rank           = it->second.rank;
                r.combatRules    = static_cast<std::uint8_t>(it->second.combat().size());
                r.logisticsRules = static_cast<std::uint8_t>(it->second.logistics().size());
                r.combatSlots    = SlotsForRank(it->second.rank, Table::Combat);
                r.logisticsSlots = SlotsForRank(it->second.rank, Table::Logistics);
                FillRuleViews(r.combat,    it->second.combat());
                FillRuleViews(r.logistics, it->second.logistics());
                // The follower's castable spells, for the board's picker. Same
                // VisitSpells pattern the seed uses.
                struct SpellList : RE::Actor::ForEachSpellVisitor {
                    std::vector<std::pair<RE::FormID, std::string>>* out;
                    RE::BSContainer::ForEachResult Visit(RE::SpellItem* sp) override {
                        if (MFO::Vocab::IsCastableSpell(sp))
                            out->emplace_back(sp->GetFormID(),
                                              sp->GetName() ? sp->GetName() : "?");
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                } vis; vis.out = &r.knownSpells;
                a->VisitSpells(vis);
            }
            r.combatSlots    = SlotsForRank(r.rank, Table::Combat);
            r.logisticsSlots = SlotsForRank(r.rank, Table::Logistics);
            s.rows.push_back(std::move(r));
        }

        // Then retained-but-inactive records, so dismissal is visibly
        // non-destructive rather than something you take on faith.
        for (const auto& [id, st] : g_followers) {
            if (Followers::IsTracked(id)) continue;
            FollowerRow r;
            r.id      = id;
            r.name    = std::format("{:08X}", id);
            if (auto* f = RE::TESForm::LookupByID(id)) {
                if (auto* a = f->As<RE::Actor>(); a && a->GetName() && *a->GetName()) {
                    r.name = a->GetName();
                }
            }
            r.active         = false;
            r.rapport        = st.rapport;
            r.rank           = st.rank;
            r.combatRules    = static_cast<std::uint8_t>(st.combat().size());
            r.logisticsRules = static_cast<std::uint8_t>(st.logistics().size());
            r.combatSlots    = SlotsForRank(st.rank, Table::Combat);
            r.logisticsSlots = SlotsForRank(st.rank, Table::Logistics);
            s.rows.push_back(std::move(r));
        }

        {
            std::scoped_lock lk(g_snapMx);
            g_snapshot = std::move(s);
        }
    }

    void Install() {
        SKSE::AllocTrampoline(256);
        WriteThunkCall<D3DInitHook>(REL::RelocationID(75595, 77226),
                                    REL::VariantOffset(0x9, 0x275, 0x0));
        WriteThunkCall<DXGIPresentHook>(REL::RelocationID(75461, 77246), REL::VariantOffset(0x9, 0x9, 0x9));
        WriteThunkCall<InputDispatchHook>(REL::RelocationID(67315, 68617), REL::VariantOffset(0x7B, 0x7B, 0x7B));
        spdlog::info("[board] hooks installed");
    }

}
