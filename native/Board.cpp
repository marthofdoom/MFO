#include "PCH.h"

// d3d11.h drags in windows.h, which CommonLibSSE-NG never includes.
// WIN32_LEAN_AND_MEAN / NOMINMAX come from CMakePresets; the GetObject macro
// does not, and wingdi.h #defines GetObject -> GetObjectW, which silently
// hijacks BGSDefaultObjectManager::GetObject<T>(). #undef AFTER the includes
// (ENGINE_NOTES §9 -- a compile error that reads like nonsense).
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <filesystem>
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
        // Published by the RENDER thread each frame: is a list-picker popup / active
        // widget open? The input hook (a DIFFERENT thread) must NOT call ImGui::
        // IsPopupOpen itself -- the ImGui context belongs to the render thread and
        // the cross-thread read returned false even with a picker open, so B closed
        // the whole board instead of backing out one level (the cascaded-back bug).
        std::atomic<bool> g_uiBusy{ false };
        std::atomic<bool> g_cursorInit{ false };
        std::atomic<float> g_cursorX{ 0.0f }, g_cursorY{ 0.0f };
        std::atomic<std::uint64_t> g_frame{ 0 };

        // io.DisplaySize LIES under Proton/upscalers: the Win32 backend reads
        // GetClientRect, which can disagree with the backbuffer. Cache the real
        // size and overwrite every frame (ENGINE_NOTES §9).
        float g_bbW = 0.0f, g_bbH = 0.0f;

        // Baked typefaces (MEO parity). body is added first so it is the DEFAULT
        // font -- all board text uses it; head is pushed for the drawn title.
        // Null -> the TTF was missing and ImGui's default bitmap font is used
        // (with the FontGlobalScale fallback).
        ImFont* g_fontBody = nullptr;
        ImFont* g_fontHead = nullptr;

        ID3D11Device*        g_device  = nullptr;
        ID3D11DeviceContext* g_context = nullptr;

        bool g_stickNav[4] = { false, false, false, false };   // up/down/left/right
        std::atomic<bool> g_shoutDownSeen{ false };
        // Set on OPEN. R1 (the RShoulder) is the Field Orders power on the deck,
        // so the press that OPENS the board is an R1 hold; the draw waits for
        // that press to be released before R1 counts as a party-switch, so the
        // board does not skip a follower the instant it appears (same class as
        // the B sticky-release guard).
        std::atomic<bool> g_justOpened{ false };

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
            // View/Back cycles the panels (Followers <-> Gambits). ImGui nav
            // never binds GamepadBack, so it is a free command button.
            case K::kBack:          return ImGuiKey_GamepadBack;
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
    // SetCond/SetAct are ABSOLUTE picks (the FFXII list-picker chooses an index
    // directly, rather than cycling): c.param carries the index into this
    // table's condition/action vocabulary. Cycle* remain for any legacy caller.
    enum class EditKind : std::uint8_t { Add, Del, MoveUp, MoveDown, Toggle,
                                         CycleCond, CycleAct, SetParam, SetSpell,
                                         SetCond, SetAct };
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
        { Vocab::kCondFoeWithinRange,"Foe targeted within range", ParamKind::Distance},
        { Vocab::kCondFoeBeyondRange,"Foe targeted beyond range", ParamKind::Distance},
        { Vocab::kCondFoeAttackingPlayer,"Foe attacking player", ParamKind::None },
        { Vocab::kCondFoeAttackingMe,"Foe attacking me",     ParamKind::None    },
        { Vocab::kCondFoeAttackingMeMelee, "Foe attacking me: melee",  ParamKind::None },
        { Vocab::kCondFoeAttackingMeRanged,"Foe attacking me: ranged", ParamKind::None },
        { Vocab::kCondFoeIsUndead,   "Foe is undead",        ParamKind::None    },
        { Vocab::kCondFoeIsDragon,   "Foe is dragon",        ParamKind::None    },
        { Vocab::kCondFoeIsCaster,   "Foe is a spellcaster", ParamKind::None    },
        { Vocab::kCondFoeIsRanged,   "Foe is ranged",        ParamKind::None    },
        { Vocab::kCondFoeWeakerThanMe, "Foe is weaker than me", ParamKind::None },
        { Vocab::kCondFoeBlocking,   "Foe is blocking",      ParamKind::None    },
        { Vocab::kCondFoeFleeing,    "Foe is fleeing",       ParamKind::None    },
        { Vocab::kCondFoeWeakFire,   "Foe: weak to fire",    ParamKind::None    },
        { Vocab::kCondFoeWeakFrost,  "Foe: weak to frost",   ParamKind::None    },
        { Vocab::kCondFoeWeakShock,  "Foe: weak to shock",   ParamKind::None    },
        { Vocab::kCondFoeCountAtLeast,"Foe count at least",  ParamKind::Count   },
        { Vocab::kCondSelfHpAbove,   "Self HP % above",      ParamKind::Percent },
        { Vocab::kCondSelfMpAbove,   "Self Magicka % above", ParamKind::Percent },
        { Vocab::kCondSelfSpAbove,   "Self Stamina % above", ParamKind::Percent },
        { Vocab::kCondAllyHpBelow,   "Ally HP % below",      ParamKind::Percent },
        // SUPPLY-STATE conditions (#10). Mirrors kCondsLogi -- lets a combat
        // gambit react to what the follower is CARRYING, not just health bars:
        // "Arrows below 5 -> Equip melee weapon", "Health potions below 2 ->
        // Cast heal on self" (conserve the stack). ConditionTrue answers these
        // the same in either table (it walks the follower's inventory, table-
        // agnostic), so no evaluator change is needed -- this is pure UI exposure.
        { Vocab::kCondSelfLowHealthPotion, "Health potions below",  ParamKind::Count   },
        { Vocab::kCondSelfLowStaminaPotion,"Stamina potions below", ParamKind::Count   },
        { Vocab::kCondSelfLowMagickaPotion,"Magicka potions below", ParamKind::Count   },
        { Vocab::kCondSelfOutOfArrows,     "Arrows below",          ParamKind::Count   },
        { Vocab::kCondSelfOutOfBolts,      "Bolts below",           ParamKind::Count   },
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
        { Vocab::kActPowerAttack,       "Power attack (experimental)" },
        { Vocab::kActFlee,              "Flee to player" },
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
        { Vocab::kCondSelfOutOfBolts,      "Bolts below",           ParamKind::Count   },
        { Vocab::kCondSelfCarryWeightAbove,"Carry weight % above",  ParamKind::Percent },
        { Vocab::kCondIsInterior,          "In an interior",        ParamKind::None    },
        { Vocab::kCondIsNight,             "At night",              ParamKind::None    },
    };
    inline constexpr VocabEntry kActsLogi[] = {
        { Vocab::kActDrinkHealthPotion,  "Drink health potion" },
        { Vocab::kActDrinkStaminaPotion, "Drink stamina potion" },
        { Vocab::kActDrinkMagickaPotion, "Drink magicka potion" },
        { Vocab::kActLootArrows,         "Loot arrows" },
        { Vocab::kActLootBolts,          "Loot bolts" },
        { Vocab::kActLootPotions,        "Loot potions (any)" },
        { Vocab::kActLootHealthPotion,   "Loot health potions" },
        { Vocab::kActLootStaminaPotion,  "Loot stamina potions" },
        { Vocab::kActLootMagickaPotion,  "Loot magicka potions" },
        { Vocab::kActLootEquipment,      "Loot better equipment" },
        { Vocab::kActLootGold,           "Loot gold" },
        { Vocab::kActLootJewelry,        "Loot jewellery" },
        { Vocab::kActLootSoulGems,       "Loot soul gems" },
        { Vocab::kActLootLockpicks,      "Loot lockpicks" },
        { Vocab::kActEquipTorch,         "Equip torch" },
        { Vocab::kActWait,               "Wait" },   // gate lower rules, e.g. "carry weight > 90% -> Wait"
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

        void DrawFieldKit(const Snapshot& snap) {
            if (g_wantClose.exchange(false)) { g_open = false; return; }

            auto& io = ImGui::GetIO();

            // R1 opened the board (it casts the Field Orders power). Wait for that
            // opening press to be RELEASED before R1 counts as a party-switch, so
            // opening does not immediately advance the follower. IsKeyDown reflects
            // only edges fed while OPEN; the guard is belt-and-suspenders on top of
            // the fact that the opening DOWN was seen while closed and never fed.
            static bool s_r1Guard = false;
            if (g_justOpened.exchange(false)) s_r1Guard = true;
            if (s_r1Guard && !ImGui::IsKeyDown(ImGuiKey_GamepadR1)) s_r1Guard = false;
            const bool r1Ready = !s_r1Guard;

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.62f),
                                     ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 400.0f), io.DisplaySize);

            const int skinCols = PushSkin();
            const auto& skin = kSkins[std::clamp(Config::g_menuStyle.load(), 0, 3)];

            if (!ImGui::Begin("Follower Overhaul", nullptr,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::End();
                ImGui::PopStyleColor(skinCols);
                return;
            }

            // Centered display title flanked by drawn rules -- MEO's signature
            // header (styled on MEO only now, per marth). The window's own title
            // bar carries the mod name ("Follower Overhaul"); this drawn header
            // names the FEATURE, "Field Orders", matching MFO_FieldOrdersPower.
            {
                auto*        dl    = ImGui::GetWindowDrawList();
                ImGui::PushFont(g_fontHead);   // header face (MEO parity); null -> default
                const char*  title = "Field Orders";
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
            constexpr int kTabCount = 2;   // Followers, Gambits (diagnostics tabs removed, marth)
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
                if (ImGui::BeginTabItem("Gambits", nullptr, tabSel(1))) {
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
                        ImGui::TextDisabled("No active follower. Recruit one to edit gambits.");
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
                        if (ImGui::SmallButton("<##prevf")) switchFollower(-1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::PushFont(g_fontHead);
                        ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                        ImGui::TextUnformatted(who->name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        ImGui::SameLine();
                        ImGui::TextDisabled("rank %u", who->rank);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(party.size() < 2);
                        if (ImGui::SmallButton(">##nextf")) switchFollower(+1);
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextDisabled("  [LB]/[RB] change follower");

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
                        if (ImGui::RadioButton("Combat", combat)) selTable = 0;
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Logistics", !combat)) selTable = 1;
                        ImGui::SameLine();
                        ImGui::TextDisabled("%d / %d slots used", (int)rules.size(), slots);
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
                                ImGui::PushFont(g_fontHead);
                                ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                ImGui::TextUnformatted(title);
                                ImGui::PopStyleColor();
                                ImGui::PopFont();
                                ImGui::TextDisabled("d-pad move   [A]/E pick   [B]/Esc back");
                                ImGui::Separator();
                                for (int k = 0; k < count; ++k) {
                                    const bool cur = (k == current);
                                    ImGui::PushID(k);
                                    if (ImGui::Selectable(labelAt(k), cur)) {
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

                        // ── DENSE GAMBIT LIST ───────────────────────────
                        // Tight padding so a full page of gambits reads like
                        // FFXII's, not a few fat rows. Scrolls when long.
                        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 3.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));

                        std::uint32_t rowFocusUid = 0;
                        const float listH = ImGui::GetTextLineHeightWithSpacing() * 11.0f;

                        if (ImGui::BeginTable("##rules", 7,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY, ImVec2(0.0f, listH))) {
                            ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 24);
                            ImGui::TableSetupColumn("On",   ImGuiTableColumnFlags_WidthFixed, 30);
                            ImGui::TableSetupColumn("When (target / condition)",
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthFixed, 84);
                            ImGui::TableSetupColumn("Do (action)",
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Spell",ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 96);
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
                                if (ImGui::Checkbox("##en", &en))
                                    QueueEdit({ EditKind::Toggle, sel, selTable, rv.uid, 0 });
                                track();

                                // WHEN -- Selectable that opens the condition list.
                                ImGui::TableNextColumn();
                                {
                                    std::string cl = "When ";
                                    cl += labelFor(rv.condOp, condTab, condN);
                                    ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
                                    const bool clicked = ImGui::Selectable(cl.c_str());
                                    ImGui::PopStyleColor();
                                    if (clicked) ImGui::OpenPopup("##cond");
                                    track();
                                    if (rv.lastFired) { ImGui::SameLine();
                                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "*"); }
                                    int curC = 0;
                                    for (int k = 0; k < condN; ++k)
                                        if (rv.condOp == condTab[k].op) { curC = k; break; }
                                    listPopup("##cond", "When (target / condition)", condN,
                                        [&](int k) { return condTab[k].label; }, curC,
                                        [&](int k) {
                                            QueueEdit({ EditKind::SetCond, sel, selTable,
                                                        rv.uid, (float)k });
                                        });
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
                                        std::string vs;
                                        if (pk == ParamKind::Percent)
                                            vs = std::to_string((int)(std::clamp(rv.param, 0.0f, 1.0f)
                                                                      * 100.0f + 0.5f)) + "%";
                                        else if (pk == ParamKind::Count)
                                            vs = std::to_string((int)(rv.param + 0.5f));
                                        else
                                            vs = std::to_string((int)(rv.param + 0.5f)) + "u";
                                        if (ImGui::Selectable(vs.c_str())) ImGui::OpenPopup("##val");
                                        track();

                                        std::vector<std::pair<float, std::string>> pv;
                                        if (pk == ParamKind::Percent) {
                                            for (int p = 5; p <= 100; p += 5)
                                                pv.emplace_back(p / 100.0f, std::to_string(p) + "%");
                                        } else if (pk == ParamKind::Count) {
                                            for (int n = 1; n <= 20; ++n)
                                                pv.emplace_back((float)n, std::to_string(n));
                                            for (int n : { 25, 30, 40, 50, 75, 100 })
                                                pv.emplace_back((float)n, std::to_string(n));
                                        } else {   // Distance
                                            for (int u = 0; u <= 2000; u += 100)
                                                pv.emplace_back((float)u, std::to_string(u) + "u");
                                            for (int u : { 2500, 3000, 4000, 5000 })
                                                pv.emplace_back((float)u, std::to_string(u) + "u");
                                        }
                                        int curV = 0; float best = 1e9f;
                                        for (int k = 0; k < (int)pv.size(); ++k) {
                                            float d = pv[k].first - rv.param; if (d < 0) d = -d;
                                            if (d < best) { best = d; curV = k; }
                                        }
                                        listPopup("##val", "Value", (int)pv.size(),
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
                                    std::string al = "-> ";
                                    al += labelFor(rv.actOp, actTab, actN);
                                    const bool wait = (rv.actOp == Vocab::kActWait);
                                    if (wait) ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                    const bool clicked = ImGui::Selectable(al.c_str());
                                    if (wait) ImGui::PopStyleColor();
                                    if (clicked) ImGui::OpenPopup("##act");
                                    track();
                                    int curA = 0;
                                    for (int k = 0; k < actN; ++k)
                                        if (rv.actOp == actTab[k].op) { curA = k; break; }
                                    listPopup("##act", "Do (action)", actN,
                                        [&](int k) { return actTab[k].label; }, curA,
                                        [&](int k) {
                                            QueueEdit({ EditKind::SetAct, sel, selTable,
                                                        rv.uid, (float)k });
                                        });
                                }

                                // SPELL -- only for cast actions; its own list.
                                ImGui::TableNextColumn();
                                {
                                    const bool isCast = (rv.actOp == Vocab::kActCastSelf ||
                                                         rv.actOp == Vocab::kActCastTarget);
                                    if (!isCast) {
                                        ImGui::TextDisabled("-");
                                    } else {
                                        const char* cur = rv.spellName.empty()
                                                          ? "(pick spell)" : rv.spellName.c_str();
                                        if (ImGui::Selectable(cur)) ImGui::OpenPopup("##spell");
                                        track();
                                        if (!rv.fail.empty() && ImGui::IsItemHovered())
                                            ImGui::SetTooltip("last: %s", rv.fail.c_str());
                                        if (who->knownSpells.empty()) {
                                            ImGui::SetNextWindowPos(
                                                ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                                            if (ImGui::BeginPopup("##spell")) {
                                                ImGui::TextDisabled("follower knows no spells");
                                                ImGui::EndPopup();
                                            }
                                        } else {
                                            int curS = -1;
                                            for (int k = 0; k < (int)who->knownSpells.size(); ++k)
                                                if (who->knownSpells[k].first == rv.spell) { curS = k; break; }
                                            listPopup("##spell", "Spell", (int)who->knownSpells.size(),
                                                [&](int k) { return who->knownSpells[k].second.c_str(); },
                                                curS,
                                                [&](int k) {
                                                    EditCmd e{ EditKind::SetSpell, sel, selTable,
                                                               rv.uid, 0 };
                                                    e.spell = who->knownSpells[k].first;
                                                    QueueEdit(e);
                                                });
                                        }
                                    }
                                }

                                // REORDER / DELETE (kept on buttons so reorder does
                                // not fight the shoulder party-switch, per spec).
                                ImGui::TableNextColumn();
                                if (ImGui::SmallButton("up")) QueueEdit({ EditKind::MoveUp, sel, selTable, rv.uid, 0 });
                                track();
                                ImGui::SameLine();
                                if (ImGui::SmallButton("dn")) QueueEdit({ EditKind::MoveDown, sel, selTable, rv.uid, 0 });
                                track();
                                ImGui::SameLine();
                                static std::uint32_t s_armed = 0;
                                const bool armed = (s_armed == rv.uid);
                                if (armed) ImGui::PushStyleColor(ImGuiCol_Button, skin.danger);
                                if (ImGui::SmallButton(armed ? "sure?" : "del")) {
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
                            ImGui::TextDisabled("Full rules (read-only) -- top wins");
                            ImGui::Separator();
                            ImGui::PushTextWrapPos(0.0f);
                            for (int i = 0; i < (int)rules.size(); ++i) {
                                const auto& rv = rules[i];
                                std::string cond = std::to_string(i + 1) + ".  When ";
                                cond += labelFor(rv.condOp, condTab, condN);
                                switch (kindFor(rv.condOp, condTab, condN)) {
                                case ParamKind::Percent:
                                    cond += " " + std::to_string((int)(std::clamp(rv.param, 0.0f, 1.0f)
                                                                        * 100.0f + 0.5f)) + "%"; break;
                                case ParamKind::Count:
                                    cond += " " + std::to_string((int)(rv.param + 0.5f)); break;
                                case ParamKind::Distance:
                                    cond += " " + std::to_string((int)(rv.param + 0.5f)) + "u"; break;
                                default: break;
                                }
                                std::string act = labelFor(rv.actOp, actTab, actN);
                                if ((rv.actOp == Vocab::kActCastSelf ||
                                     rv.actOp == Vocab::kActCastTarget) && !rv.spellName.empty())
                                    act += " (" + rv.spellName + ")";
                                if (!rv.enabled) {
                                    ImGui::PushStyleColor(ImGuiCol_Text,
                                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                    ImGui::TextWrapped("%s   ->   %s   [off]",
                                                       cond.c_str(), act.c_str());
                                    ImGui::PopStyleColor();
                                } else {
                                    ImGui::TextColored(skin.accent, "%s", cond.c_str());
                                    ImGui::SameLine(0, 0);
                                    ImGui::TextDisabled("  ->  ");
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
                        if (ImGui::Button("+ Add rule"))
                            QueueEdit({ EditKind::Add, sel, selTable, 0u, 0 });
                        ImGui::EndDisabled();
                        if (full) { ImGui::SameLine();
                            ImGui::TextDisabled("all %d slots used -- more unlock with rapport", slots); }

                        ImGui::Spacing();
                        ImGui::TextDisabled("Highlight a slot and press [A]/E to open its list. "
                                            "[Y] toggles the highlighted line. Top rule wins.");

                        ImGui::EndTabItem();
                    }
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
            // PUBLISH the busy state for the input hook (which can't read the ImGui
            // context safely from its own thread). Computed here, between NewFrame
            // and Render, where IsPopupOpen/IsAnyItemActive are valid. This is what
            // makes B a CASCADED back: busy -> B closes the innermost picker; not
            // busy (root) -> B closes the board.
            const bool popupBusy = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            g_uiBusy.store(popupBusy || ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput,
                           std::memory_order_relaxed);
            if (popupBusy)
                ImGui::TextDisabled("[A]/E pick   [B]/Esc back   d-pad to move");
            else
                ImGui::TextDisabled("[A]/E open list   [B]/Esc back-or-close   [LB]/[RB] follower   "
                                    "[View] tab   [Y] toggle line   d-pad move   -   Skin in MCM");
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

                // Bake real typefaces at backbuffer scale -- FontGlobalScale on
                // ImGui's default bitmap font was blurry above 1080p and the
                // biggest "less polished vs MEO" tell (§919 note). The faces are
                // MEO's own (same author). MUST run before the DX11 backend
                // builds the atlas. Files optional: a missing face falls back to
                // the default bitmap font + the FontGlobalScale path below.
                {
                    const float uiScale = std::max(1.0f,
                        static_cast<float>(sd.BufferDesc.Height) / 1080.0f);
                    namespace fs = std::filesystem;
                    constexpr const char* kBodyTTF = "Data/SKSE/Plugins/MFO/fonts/body.ttf";
                    constexpr const char* kHeadTTF = "Data/SKSE/Plugins/MFO/fonts/head.ttf";
                    if (fs::exists(kBodyTTF))
                        g_fontBody = io.Fonts->AddFontFromFileTTF(kBodyTTF, std::floor(19.0f * uiScale));
                    if (fs::exists(kHeadTTF))
                        g_fontHead = io.Fonts->AddFontFromFileTTF(kHeadTTF, std::floor(27.0f * uiScale));
                    if (!g_fontHead) g_fontHead = g_fontBody;   // head falls back to body, not default
                    spdlog::info("[board] fonts: body={} head={} (scale {:.2f})",
                                 g_fontBody ? "ok" : "default", g_fontHead ? "ok" : "default", uiScale);
                }

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

                // With a baked face the pixel size is already correct, so scale
                // is 1.0 (MEO's own rule). Only the default-bitmap FALLBACK --
                // when the TTF was missing -- needs FontGlobalScale, the same
                // io.DisplaySize.y/1080 path MEO uses when its fonts are absent.
                if (g_bbH > 0.0f)
                    ImGui::GetIO().FontGlobalScale =
                        g_fontBody ? 1.0f : std::max(1.0f, g_bbH / 1080.0f);

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
                        // Read the render thread's PUBLISHED busy flag. Calling
                        // ImGui::IsPopupOpen/IsAnyItemActive here is cross-thread (the
                        // ImGui context belongs to the render thread) and read FALSE
                        // even with a picker open -- so B closed the whole board
                        // instead of backing out one level. g_uiBusy is computed each
                        // frame where those reads are valid (see the render publish).
                        const bool imguiBusy = g_uiBusy.load(std::memory_order_relaxed);

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
                                // CASCADED BACK. imguiBusy is true whenever a
                                // list-picker popup / text box / active widget is
                                // open, so B drives ImGui's Escape and closes the
                                // INNERMOST context (the picker) without touching
                                // the board. Only at the ROOT -- nothing sub-open
                                // -- does a B press close the board. Every release
                                // still reaches ImGui so B-cancel never sticks, and
                                // one press walks up exactly one level.
                                if (down && !imguiBusy) g_wantClose = true;
                                else io.AddKeyEvent(ImGuiKey_Escape, down);
                            } else if (auto k = GamepadToImGuiKey(code); k != ImGuiKey_None) {
                                // NB: the gamepad shout-close branch was removed on
                                // purpose. R1 is the Field Orders power on the deck;
                                // while the board is OPEN it must NOT re-close it
                                // (that is B's job now). Instead R1 -> GamepadR1 and
                                // becomes the in-board party-switch. The opening R1
                                // press was seen while the board was CLOSED (hook
                                // passthrough), so it never reaches here -- only
                                // presses made while open do. The game never sees
                                // the swallowed R1, so the power is not re-cast.
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
            g_justOpened = true;                    // R1 party-switch waits for a release after open
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
            v.firedAt = g.lastFiredAt;    // flair #9: the row pulse ages this
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
            case EditKind::SetCond: {
                // Absolute pick by index into THIS table's condition vocabulary
                // (writes the frozen opcode string, exactly as CycleCond does).
                const VocabEntry* t = (table == 1) ? kCondsLogi : kCondsCombat;
                const int tn = (table == 1) ? (int)std::size(kCondsLogi)
                                            : (int)std::size(kCondsCombat);
                const int n = std::clamp((int)(c.param + 0.5f), 0, tn - 1);
                tab[i].conditionOpcode = t[n].op; break; }
            case EditKind::SetAct: {
                const VocabEntry* t = (table == 1) ? kActsLogi : kActsCombat;
                const int tn = (table == 1) ? (int)std::size(kActsLogi)
                                            : (int)std::size(kActsCombat);
                const int n = std::clamp((int)(c.param + 0.5f), 0, tn - 1);
                tab[i].actionOpcode = t[n].op; break; }
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
