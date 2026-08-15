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
#include <imgui_internal.h>   // nav state (NavWindow/NavLayer/NavId) for the cascaded B back-out
#include <imgui_impl_dx11.h>
// VENDORED (native/imgui_impl_win32.h, upstream v1.92.8), not vcpkg: the TU is
// compiled into this target with IMGUI_IMPL_WIN32_DISABLE_GAMEPAD so the
// backend's XInput poll cannot race the input hook's gamepad feed (v1.0.59).
#include "imgui_impl_win32.h"

#include "Board.h"
#include "Progression.h"   // #74: the frozen catalog — lock-free render reads by contract
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "Forms.h"
#include "State.h"
#include "Probe.h"
#include "ProgProbe.h"
#include "ProgAllocator.h"
#include "MainThread.h"
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
        std::atomic<bool> g_wantClose{ false };   // keyboard shout-key close (unconditional)
        std::atomic<bool> g_cursorInit{ false };
        std::atomic<float> g_cursorX{ 0.0f }, g_cursorY{ 0.0f };
        std::atomic<std::uint64_t> g_frame{ 0 };
        // CLOSE GRACE: the frame up to which the input hook keeps swallowing after a
        // board close, so the button PRESS that closed the board doesn't leak its
        // release (or a held edge) to the game and pop the Tween menu. Ends early on
        // the first release; this is only the safety cap. Set by CloseBoard().
        std::atomic<std::uint64_t> g_closeGrace{ 0 };

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
            // kB is deliberately ABSENT here: the hook forwards it through an
            // explicit branch (see InputDispatchHook) that documents B's
            // special role as the nav-cancel / cascaded-back key.
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
                                         SetCond, SetAct, TeachSpell,
                                         SetSubject, SetSubjectActor,   // #68
                                         SetClassOverride,   // #65
                                         ProgSetClass, ProgAllocPerk, ProgRespec,   // #74 comp 3
                                         ProgSetManual, ProgApplySkillPoint };   // #74 §16 (param:
                                                            // 0/1 toggle | AV ordinal, same
                                                            // param-carries-ordinal shape as
                                                            // SetClassOverride)
    struct EditCmd {
        EditKind kind; RE::FormID fid; int table; std::uint32_t uid; float param;
        RE::FormID spell = 0;
        RE::FormID book  = 0;   // #4 TeachSpell: the spellbook to consume
        RE::FormID subjectActor = 0;   // #68 SetSubjectActor: the specific follower
        RE::FormID perk  = 0;   // #74 ProgAllocPerk: the catalog node (rank-1 form)
    };
    // #68: written into a Gambit's subjectSelector when SetSubjectActor picks
    // a SPECIFIC follower, so a stale subject value from an earlier Self/
    // Player/NearestAlly pick cannot linger and mislead. Deliberately OUTSIDE
    // Vocab::Subject's real range (0/1/2) -- ResolveCastTarget's switch falls
    // into its `default:` case (Self) if the specific actor ever becomes
    // unavailable, exactly the same graceful demotion an unrecognised value
    // would get anyway.
    constexpr std::uint8_t kSubjectSpecificFollower = 0xFF;
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
        { Vocab::kCondDark,          "When dark",            ParamKind::None    },
    };
    inline constexpr VocabEntry kActsCombat[] = {
        { Vocab::kActWait,              "Wait" },
        { Vocab::kActCastSelf,          "Cast on self" },
        { Vocab::kActCastTarget,        "Cast at foe/ally" },
        // cast_player is LOGISTICS-ONLY for now: in combat CastOn's package/grace
        // path delivers a self-delivery buff to the follower, not the player
        // (Fable, 2026-08-06). Combat player-casts are a follow-up.
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
        { Vocab::kCondDark,                "When dark",             ParamKind::None    },
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
        // Cast in logistics (mage update): out-of-combat casting -- self-buffs,
        // candlelight, out-of-combat heals. Same opcodes as combat; the
        // logistics scan dispatches them through Actuation::Fire.
        { Vocab::kActCastSelf,           "Cast on self" },
        { Vocab::kActCastTarget,         "Cast at foe/ally" },
        { Vocab::kActCastPlayer,         "Cast on player" },
        { Vocab::kActWait,               "Wait" },   // gate lower rules, e.g. "carry weight > 90% -> Wait"
    };

    // #65: the combat-class override picker's labels, index-matched to
    // FollowerState::combatClassOverride / CombatStyle::Stance (0=Auto,
    // 1=Melee, 2=Ranged, 3=Cast/Mage). Auto is the default -- "no override".
    inline constexpr const char* kClassNames[4] = { "Auto", "Melee", "Ranged", "Mage" };

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

        // Close the board AND open the input-swallow grace, so the button press
        // that closed it can't leak its release to the game (Tween menu). The
        // grace ends early on the first release; 30 frames is only the safety cap.
        inline void CloseBoard() {
            g_open.store(false);
            g_closeGrace.store(g_frame.load() + 30);
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
            // #74: the tab count is RUNTIME now — the Progression tab exists
            // only when the addon ESL is detected (§1: absent = the tab is
            // absent entirely, the same optional-addon pattern as the MCM
            // Detected line), and the View-cycle must not step onto a tab
            // that was never emitted.
            const bool progActive = snap.prog && snap.prog->active;
            const int kTabCount = progActive ? 3 : 2;   // Followers, Gambits[, Progression]
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
                        // #65: per-follower combat-class OVERRIDE trigger. Auto
                        // (0) is the default and reads as "no override" -- the
                        // #64 custom-follower guarantee lives in Scheduler
                        // (Auto never forces anything); this button just opens
                        // the picker that sets/clears it. The popup itself is
                        // drawn below, once listPopup exists.
                        {
                            const int curClass = std::clamp<int>(who->combatClassOverride, 0, 3);
                            std::string cl = std::string("Class: ") + kClassNames[curClass];
                            if (ImGui::SmallButton(cl.c_str())) ImGui::OpenPopup("##class");
                        }
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
                                pickerDrawnThisFrame = true;   // ground truth: a picker is on screen
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
                            listPopup("##class", "Combat class", 4,
                                [&](int k) { return kClassNames[k]; }, curClass,
                                [&](int k) {
                                    QueueEdit({ EditKind::SetClassOverride, sel, selTable,
                                                0u, (float)k });
                                });
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
                            ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed, 24);
                            ImGui::TableSetupColumn("On",   ImGuiTableColumnFlags_WidthFixed, 30);
                            ImGui::TableSetupColumn("When (target / condition)",
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthFixed, 84);
                            ImGui::TableSetupColumn("Do (action)",
                                                            ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Spell",ImGuiTableColumnFlags_WidthStretch);
                            // #68: who a Cast-on-target row hits (Self / player
                            // by name / Nearest ally / a specific follower).
                            ImGui::TableSetupColumn("Target",ImGuiTableColumnFlags_WidthStretch);
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
                                                         rv.actOp == Vocab::kActCastTarget ||
                                                         rv.actOp == Vocab::kActCastPlayer);
                                    if (!isCast) {
                                        ImGui::TextDisabled("-");
                                    } else {
                                        const char* cur = rv.spellName.empty()
                                                          ? "(pick spell)" : rv.spellName.c_str();
                                        if (ImGui::Selectable(cur)) ImGui::OpenPopup("##spell");
                                        track();
                                        if (!rv.fail.empty() && ImGui::IsItemHovered())
                                            ImGui::SetTooltip("last: %s", rv.fail.c_str());
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
                                            ImGui::TextUnformatted("Spell");
                                            ImGui::PopStyleColor();
                                            ImGui::PopFont();
                                            ImGui::TextDisabled("d-pad move   [A]/E pick   [B]/Esc back");
                                            ImGui::Separator();
                                            if (who->knownSpells.empty() && who->teachableSpells.empty())
                                                ImGui::TextDisabled("no spells known, no spellbooks carried");
                                            for (int k = 0; k < (int)who->knownSpells.size(); ++k) {
                                                const bool curSel = who->knownSpells[k].first == rv.spell;
                                                ImGui::PushID((int)who->knownSpells[k].first);   // dup names -> unique IDs
                                                if (ImGui::Selectable(who->knownSpells[k].second.c_str(), curSel)) {
                                                    EditCmd e{ EditKind::SetSpell, sel, selTable, rv.uid, 0 };
                                                    e.spell = who->knownSpells[k].first;
                                                    QueueEdit(e);
                                                }
                                                if (curSel) {
                                                    ImGui::SetItemDefaultFocus();
                                                    if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
                                                }
                                                ImGui::PopID();
                                            }
                                            if (!who->teachableSpells.empty()) {
                                                ImGui::Separator();
                                                ImGui::TextDisabled("Teach from spellbook (consumes it):");
                                                for (const auto& t : who->teachableSpells) {
                                                    const bool armed = (s_teachArmed == t.book);
                                                    if (armed) ImGui::PushStyleColor(ImGuiCol_Text, skin.danger);
                                                    const std::string lbl = armed
                                                        ? (t.name + "  -- teach? DESTROYS the book")
                                                        : (t.name + "  (spellbook)");
                                                    ImGui::PushID((int)t.book);   // dup names -> unique IDs
                                                    if (ImGui::Selectable(lbl.c_str(), false,
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
                                                          ? "Auto" : rv.subjectName.c_str();
                                        if (ImGui::Selectable(cur)) ImGui::OpenPopup("##target");
                                        track();

                                        // Option list, rebuilt fresh each frame the
                                        // popup is open: Self, the player by name,
                                        // Nearest ally, then every OTHER active
                                        // follower by name. form==0 means "use kind
                                        // (the Subject enum)"; form!=0 means "this
                                        // SPECIFIC follower" and kind is ignored.
                                        struct TargetOpt {
                                            std::uint8_t kind; RE::FormID form; const std::string* label;
                                        };
                                        // #68: subject 0 (Vocab::Subject::Self) on a cast-AT-target
                                        // row means AUTO -- the ladder resolves the selector/
                                        // condition target if any, else the player. It is NOT
                                        // "cast on the follower" (that is the separate Cast-on-self
                                        // action). Labelled Auto so the picker reads true.
                                        static const std::string kAutoLbl = "Auto (target, else you)";
                                        static const std::string kAllyLbl = "Nearest ally";
                                        std::vector<TargetOpt> opts;
                                        opts.push_back({ (std::uint8_t)Vocab::Subject::Self,        0, &kAutoLbl });
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

                                        listPopup("##target", "Target", (int)opts.size(),
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
                                     rv.actOp == Vocab::kActCastTarget ||
                                     rv.actOp == Vocab::kActCastPlayer) && !rv.spellName.empty())
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

                // ── THE PROGRESSION TAB (#74 component 3) ───────────────
                // Emitted ONLY when the addon ESL is detected (§1) — absent
                // means absent, the same optional-addon pattern as the MCM
                // Detected line. The draw reads two immutable sources: the
                // FROZEN catalog (lock-free on this thread by its contract)
                // and the allocator's published value-only views (snap.prog).
                // It mutates NOTHING — every action queues an EditCmd that
                // ApplyEdits re-posts to the MAIN thread, where the §5
                // backend gate re-validates before any engine write.
                if (progActive && ImGui::BeginTabItem("Progression", nullptr, tabSel(2))) {
                    s_tab = 2;
                    const auto& prog = *snap.prog;

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
                        if (who->enrolled && who->cls != 0) {
                            ImGui::TextDisabled("level %u", (unsigned)who->level);
                            ImGui::SameLine();
                            std::string cl = std::string("Class: ") +
                                ProgAllocator::ClassName(static_cast<ProgAllocator::Class>(who->cls));
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
                        // reopens. The class ordinals are the allocator's own
                        // (#65-aligned); their skill weights come from the
                        // ESL FormLists read at Init — nothing hardcoded here.
                        // P2 (deck round 3): ENROLLED-FIRST branching. The
                        // eligibility check (teammate/dismissal quirks) can
                        // flap at runtime — it must gate ENROLLMENT only,
                        // never blank the UI of an already-enrolled follower
                        // (that read as "skills not visible at all").
                        const bool progReady  = who->enrolled && who->cls != 0;
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
                            for (int k = 1; k <= 3; ++k) {
                                const bool cur = (who->cls == k);
                                ImGui::PushID(k);
                                if (ImGui::Selectable(
                                        ProgAllocator::ClassName(static_cast<ProgAllocator::Class>(k)),
                                        cur)) {
                                    QueueEdit({ EditKind::ProgSetClass, s_psel, 0, 0u, (float)k });
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
                            if (who->unspentPerk < 1.0f)
                                ImGui::TextDisabled("None to spend: 1 point per 2 levels -- level "
                                                    "%u has earned %u, and %u are already spent.",
                                                    (unsigned)who->level, (unsigned)(who->level / 2),
                                                    (unsigned)who->allocatedRanks);
                            else
                                ImGui::TextDisabled("1 perk point per 2 levels. "
                                                    "Pick a skill to open its tree.");
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
                                if (ImGui::Checkbox("Manual skill points", &man))
                                    QueueEdit({ EditKind::ProgSetManual, s_psel, 0, 0u,
                                                man ? 1.0f : 0.0f });
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip(
                                        "Manual OVERRIDE: while ON, this follower earns 5 skill\n"
                                        "points per level for YOU to place -- INSTEAD OF automatic\n"
                                        "class-based skill growth, never on top of it. Toggle OFF\n"
                                        "to resume auto growth. For mage or multiclass builds the\n"
                                        "class weights won't serve.");
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
                                    if (ImGui::Selectable(label, false,
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
                                        QueueEdit({ EditKind::ProgApplySkillPoint, s_psel, 0, 0u,
                                                    (float)(int)s_actAv });
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
                                                    EditCmd e{ EditKind::ProgAllocPerk, s_psel,
                                                               0, 0u, 0.0f };
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
                                    QueueEdit({ EditKind::ProgRespec, s_psel, 0, 0u, 0.0f });
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
            // #56: margins from the top-right corner are MCM-adjustable so the
            // overlay can dodge another mod's HUD (default 12/12 = unchanged).
            // Clamp to the display so a big slider value can't push it off-screen
            // (the right pivot would otherwise vanish it on a narrow Deck screen).
            const float mx = std::clamp(static_cast<float>(Config::g_overlayX.load()),
                                        0.0f, std::max(0.0f, io.DisplaySize.x - 16.0f));
            const float my = std::clamp(static_cast<float>(Config::g_overlayY.load()),
                                        0.0f, std::max(0.0f, io.DisplaySize.y - 16.0f));
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - mx, my),
                                    ImGuiCond_Always, ImVec2(1.0f, 0.0f));
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
                // LOAD-BEARING: gamepad nav (nav_gamepad_active in imgui.cpp) needs
                // NavEnableGamepad AND HasGamepad. Stock imgui_impl_win32 rewrites
                // HasGamepad every frame from its XInput poll -- deaf poll (Steam
                // Input kb<->pad flip) = flag cleared = ALL hook-fed gamepad nav
                // dead. Our vendored backend compiles that poll out, so this
                // init-time set persists and the hook-fed Gamepad* keys always
                // drive nav (v1.0.59).
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

                    // PROGRESSION PROBE HOTKEY (dev-only, bProgProbe=0 for
                    // everyone else). Same shape as the focus key above, but
                    // the probe MUTATES engine state (AddPerk/SetBaseActorValue)
                    // so it rides MainThread::Post, never AddTask — AddTask
                    // drains on a job worker in this runtime (§0.37).
                    if (Config::g_progProbe.load()) {
                        const int pk = Config::g_progProbeKey.load();
                        if (pk != 0) {
                            for (auto* e = *a_events; e; e = e->next) {
                                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                                auto* b = static_cast<RE::ButtonEvent*>(e);
                                if (!b->IsDown()) continue;                 // edge only
                                if (b->device.get() != RE::INPUT_DEVICE::kKeyboard) continue;
                                if (static_cast<int>(b->GetIDCode()) != pk) continue;
                                MainThread::Post([]() { ProgProbe::OnHotkey(); });
                            }
                        }
                    }

                    // PROGRESSION ALLOCATOR HARNESS HOTKEY (dev-only,
                    // bProgHarness=0 for everyone else). Same shape as the
                    // probe key above; the harness mutates engine state
                    // (AddPerk/SetBaseActorValue) so it rides MainThread::Post,
                    // never AddTask (§0.37). The verb comes from the addon's
                    // MFOP_DevCmd GLOB (console-set) — see ProgAllocator.h.
                    if (Config::g_progHarness.load()) {
                        const int hk = Config::g_progHarnessKey.load();
                        if (hk != 0) {
                            for (auto* e = *a_events; e; e = e->next) {
                                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                                auto* b = static_cast<RE::ButtonEvent*>(e);
                                if (!b->IsDown()) continue;                 // edge only
                                if (b->device.get() != RE::INPUT_DEVICE::kKeyboard) continue;
                                if (static_cast<int>(b->GetIDCode()) != hk) continue;
                                MainThread::Post([]() { ProgAllocator::OnHarnessHotkey(); });
                            }
                        }
                    }
                }

                // CLOSE GRACE: the board just closed (g_open false) but we are still
                // inside the swallow window. Eat every event so the button PRESS that
                // closed the board can't leak its release/held edge to the game and
                // pop the Tween menu. End the grace the instant we see a release, so
                // the dead window is only as long as the closing button is held.
                if (a_events && !g_open.load() && g_frame.load() < g_closeGrace.load()) {
                    for (auto* e = *a_events; e; e = e->next)
                        if (e->eventType == RE::INPUT_EVENT_TYPE::kButton &&
                            !static_cast<RE::ButtonEvent*>(e)->IsDown()) { g_closeGrace.store(0); break; }
                    *a_events = nullptr;
                    func(a_source, a_events);
                    return;
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
                        // The BACK keys never decide the close here -- this thread
                        // can't read ImGui. Keyboard Esc/Tab forward Escape; gamepad
                        // B forwards GamepadFaceRight (ImGui's nav-cancel) below. The
                        // render thread resolves the board close off that keypress
                        // and the on-screen picker state (see DrawFieldKit's end).
                        switch (b->device.get()) {
                        case RE::INPUT_DEVICE::kMouse:
                            if (code <= 4) io.AddMouseButtonEvent(static_cast<int>(code), down);
                            else if ((code == 8 || code == 9) && down)
                                io.AddMouseWheelEvent(0.0f, code == 8 ? 1.0f : -1.0f);
                            break;

                        case RE::INPUT_DEVICE::kKeyboard:
                            if (code == 0x01 || code == 0x0F) {          // Esc / Tab
                                // Forward Escape (both edges) so ImGui's keyboard
                                // nav-cancel closes an open picker and the render
                                // thread's resolver sees the press. The board-close
                                // decision is made THERE (off IsKeyPressed), never
                                // here -- this thread can't read ImGui state.
                                io.AddKeyEvent(ImGuiKey_Escape, down);
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
                                // CASCADED BACK. THIS HOOK is B's one and only
                                // source (v1.0.59): forward it as GamepadFaceRight,
                                // ImGui's nav-cancel, off Skyrim's ButtonEvent
                                // stream -- the stream that survives Steam Input's
                                // kb<->pad mode flips, unlike the Win32 backend's
                                // XInput poll, which went deaf on a flip and took
                                // B (and, via its HasGamepad rewrite, ALL gamepad
                                // nav) down with it. That poll is now compiled out
                                // (IMGUI_IMPL_WIN32_DISABLE_GAMEPAD on the vendored
                                // backend TU), so there is no second B path to race
                                // this one -- the race that a hook-side B caused
                                // back when the backend ALSO fed B is structurally
                                // gone. ImGui nav-cancel closes the innermost
                                // picker at NewFrame; the render-thread resolver
                                // (DrawFieldKit's end) decides the board close off
                                // this same keypress. The game never sees B
                                // (swallowed below).
                                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, down);
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
        if (now) g_open.store(true);
        else     CloseBoard();   // grace-swallow the release so it can't leak (Tween menu)
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
            // #68: the cast-target subject, resolved to a display name here
            // (main thread) so the draw call never touches an engine pointer.
            v.subject = g.subjectSelector;
            v.subjectActorForm = g.subjectActorForm;
            if (v.subjectActorForm) {
                RE::Actor* act = nullptr;
                if (auto* f = RE::TESForm::LookupByID(v.subjectActorForm)) act = f->As<RE::Actor>();
                v.subjectName = act ? (act->GetName() ? act->GetName() : "?") : "(follower gone)";
            } else {
                switch (static_cast<Vocab::Subject>(v.subject)) {
                case Vocab::Subject::Player:
                    if (auto* pc = RE::PlayerCharacter::GetSingleton())
                        v.subjectName = pc->GetName() ? pc->GetName() : "Player";
                    break;
                case Vocab::Subject::NearestAlly: v.subjectName = "Nearest ally"; break;
                case Vocab::Subject::Self:
                default:                          v.subjectName = "Auto"; break;   // #68: subject 0 = Auto ladder, not self
                }
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
            // #74 component 3: progression verbs. These mutate the ENGINE
            // (AddPerk / SetBaseActorValue / rapport) and g_prog is main-
            // thread-only (the poll's thread) — so they ride MainThread::Post
            // exactly like the harness hotkey, never this drain. The
            // allocator re-validates everything at apply time (§5's backend
            // gate): a stale board can only be REFUSED, never over-allocate.
            // Republish the views right after so the tab echoes on the next
            // snapshot instead of waiting out the poll cadence.
            if (c.kind == EditKind::ProgSetClass || c.kind == EditKind::ProgAllocPerk ||
                c.kind == EditKind::ProgRespec || c.kind == EditKind::ProgSetManual ||
                c.kind == EditKind::ProgApplySkillPoint) {
                const auto  kind  = c.kind;
                const auto  fid   = c.fid;
                const auto  perk  = c.perk;
                const float param = c.param;
                const auto  cls   = static_cast<ProgAllocator::Class>(
                    std::clamp(static_cast<int>(c.param + 0.5f), 0, 3));
                MainThread::Post([kind, fid, perk, cls, param]() {
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
                    if (!actor) {
                        spdlog::info("[prog] board edit dropped: actor {:08X} unresolvable", fid);
                        return;
                    }
                    switch (kind) {
                    case EditKind::ProgSetClass:
                        // The §15 onboarding as ONE player action: enroll on
                        // first contact (refuses with a named line when
                        // already enrolled), then the class pick auto-scales.
                        if (auto it2 = ProgAllocator::g_prog.find(fid);
                            it2 == ProgAllocator::g_prog.end() || !it2->second.enrolled)
                            ProgAllocator::Enroll(actor);
                        ProgAllocator::SetClass(actor, cls);
                        break;
                    case EditKind::ProgAllocPerk:
                        ProgAllocator::AllocatePerk(actor, perk);
                        break;
                    case EditKind::ProgRespec:
                        ProgAllocator::Respec(actor);
                        break;
                    case EditKind::ProgSetManual:   // #74 §16
                        ProgAllocator::SetManualSkills(actor, param > 0.5f);
                        break;
                    case EditKind::ProgApplySkillPoint:   // #74 §16 — param = AV ordinal
                        ProgAllocator::ApplyManualSkillPoint(actor,
                            static_cast<RE::ActorValue>(static_cast<int>(param + 0.5f)));
                        break;
                    default:
                        break;
                    }
                    ProgAllocator::PublishBoardViews();
                });
                continue;
            }

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

            // #65: a per-FOLLOWER edit, not per-gambit -- keyed on c.fid alone
            // (uid/table are unused, same "no per-rule identity" shape as Add
            // above). Absolute pick by index, same c.param convention as
            // SetCond/SetAct; clamp so a stray value never writes an
            // out-of-range stance ordinal into the co-save.
            if (c.kind == EditKind::SetClassOverride) {
                it->second.combatClassOverride =
                    static_cast<std::uint8_t>(std::clamp((int)(c.param + 0.5f), 0, 3));
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
            case EditKind::TeachSpell: {
                // #4: teach the spell (AddSpell) and CONSUME one spellbook from
                // the player, then set it as the gambit's spell. Runs where every
                // board edit runs -- the same task the evaluator drains, so
                // RemoveItem is the blessed worker-safe inventory path (§0.32) and
                // AddSpell is the same mutation class as the equips already issued
                // here. GUARD against a stale snapshot: teach ONLY if the follower
                // still lacks the spell AND the player still carries the book, so
                // a double-queued edit never eats a second book or grants a free
                // spell after the book was consumed/sold.
                auto* follower = c.fid ? RE::TESForm::LookupByID<RE::Actor>(c.fid) : nullptr;
                auto* spell    = c.spell ? RE::TESForm::LookupByID<RE::SpellItem>(c.spell) : nullptr;
                auto* book     = c.book ? RE::TESForm::LookupByID<RE::TESBoundObject>(c.book) : nullptr;
                auto* pc       = RE::PlayerCharacter::GetSingleton();
                bool hasBook = false;
                if (pc && book)
                    for (auto& [o, d] : pc->GetInventory())
                        if (o == book && d.first > 0) { hasBook = true; break; }
                if (follower && spell) {
                    if (follower->HasSpell(spell)) {
                        // already learned (a prior edit taught it) -> just assign,
                        // never consume a second book.
                        tab[i].actionParamForm = c.spell;
                    } else if (hasBook) {
                        follower->AddSpell(spell);
                        pc->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        spdlog::info("[board] taught {:08X} spell {:08X}, consumed book {:08X}",
                                     c.fid, c.spell, c.book);
                        tab[i].actionParamForm = c.spell;
                    }
                    // else: book gone AND spell unknown -> do NOT set an uncastable spell.
                }
                break;
            }
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
            // #68: absolute picks from the target popup, same shape as
            // SetCond/SetAct. Self/Player/NearestAlly CLEAR any stale
            // specific-follower pick; a specific follower gets the sentinel
            // subjectSelector so a later demotion (the actor goes away)
            // reads as Self, never a leftover Player/NearestAlly nobody chose.
            case EditKind::SetSubject:
                tab[i].subjectSelector   = static_cast<std::uint8_t>(c.param + 0.5f);
                tab[i].subjectActorForm  = 0;
                break;
            case EditKind::SetSubjectActor:
                tab[i].subjectActorForm = c.subjectActor;
                tab[i].subjectSelector  = kSubjectSpecificFollower;
                break;
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
        // #74 component 3: refcount copy of the allocator's published views
        // (built on the MAIN thread; this drain runs on the task worker, so
        // it must never read g_prog itself — only the immutable snap).
        s.prog = ProgAllocator::CopyBoardViews();

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
            if (player) {
                r.distance   = a->GetPosition().GetDistance(player->GetPosition());
                r.playerName = player->GetName() ? player->GetName() : "Player";
            }
            // #68: the cast-target picker's follower list -- every OTHER
            // active teammate by name (never this row's own actor). Same
            // g_active walk Actuation::NearestAlly does at cast time.
            for (const auto& h2 : Followers::g_active) {
                auto* ally = h2.get().get();
                if (!ally || ally == a) continue;
                r.alliesForPicker.emplace_back(ally->GetFormID(),
                                               ally->GetName() ? ally->GetName() : "?");
            }

            if (auto it = g_followers.find(r.id); it != g_followers.end()) {
                r.rapport        = it->second.rapport;
                r.rank           = it->second.rank;
                r.combatClassOverride = it->second.combatClassOverride;
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

                // #4: teachable spells -- books in the PLAYER's pack whose spell
                // this follower does not yet know. Offered in the picker as
                // "Name (spellbook)"; teaching consumes the book. Read-only here
                // (a snapshot); the actual AddSpell + book consume runs in
                // ApplyEdits on the main thread.
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    for (auto& [obj, data] : pc->GetInventory()) {
                        if (!obj || data.first <= 0) continue;
                        auto* book = obj->As<RE::TESObjectBOOK>();
                        if (!book || !book->TeachesSpell()) continue;
                        auto* sp = book->data.teaches.spell;
                        if (!sp || !MFO::Vocab::IsCastableSpell(sp)) continue;
                        if (a->HasSpell(sp)) continue;   // already known -> not teachable
                        r.teachableSpells.push_back({ sp->GetFormID(), book->GetFormID(),
                                                      sp->GetName() ? sp->GetName() : "?" });
                    }
                }
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
            r.combatClassOverride = st.combatClassOverride;
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
