#include "PCH.h"

// d3d11.h drags in windows.h, which CommonLibSSE-NG never includes.
// WIN32_LEAN_AND_MEAN / NOMINMAX come from CMakePresets; the GetObject macro
// does not, and wingdi.h #defines GetObject -> GetObjectW, which silently
// hijacks BGSDefaultObjectManager::GetObject<T>(). #undef AFTER the includes
// (ENGINE_NOTES §9 -- a compile error that reads like nonsense).
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <cstring>
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
#include "Board_internal.h"   // shared Board-family substrate (mechanical split)
#include "Progression.h"   // #74: the frozen catalog — lock-free render reads by contract
#include "Followers.h"
#include "Rapport.h"
#include "Config.h"
#include "Logistics.h"   // IsLooting/IsTrading for the [C][L][T] activity glyphs
#include "Forms.h"
#include "State.h"
#include "Probe.h"
#include "ProgProbe.h"
#include "ProgAllocator.h"
#include "MainThread.h"
#include "Vocabulary.h"
#include "Scheduler.h"
#include "Diagnostics.h"  // SEV-1: PumpTickGate/CurrentPumpEpoch to drain the focus-fire sink

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace MFO::Board {

    namespace {

        // ── shared state ────────────────────────────────────────────────────
        // Two mutexes, NEVER nested, per ARCHITECTURE.md §3.1.
        std::mutex g_snapMx;      // guards g_snapshot
        Snapshot   g_snapshot;

        std::atomic<bool> g_ready{ false };   // D3D init succeeded
        // OFF until a game is actually loaded. g_ready goes true at renderer init,
        // long before any save, so defaulting this on drew the HUD over the
        // title screen, loading screens and every vanilla menu.
        std::atomic<bool> g_hud{ false };      // compact readout -- passive, never takes input
        std::atomic<bool> g_cursorInit{ false };
        std::atomic<float> g_cursorX{ 0.0f }, g_cursorY{ 0.0f };
        std::atomic<std::uint64_t> g_frame{ 0 };
        // CLOSE GRACE: a steady_clock DEADLINE (ns since the steady epoch, 0 =
        // inactive) up to which the input hook keeps swallowing after a board
        // close, so the button PRESS that closed the board doesn't leak its
        // release (or a held edge) to the game and pop the Tween menu. Ends early
        // on the first release; this is only the safety cap. Set by CloseBoard().
        // NOTE: keyed to WALL time, not g_frame — g_frame ticks only in
        // PublishSnapshot (~532ms while the board is CLOSED), so the old
        // "g_frame + 30" cap was ~16s, not ~0.5s: a missed release could swallow
        // real input for up to 16s.
        std::atomic<std::int64_t> g_closeGrace{ 0 };
        constexpr std::chrono::milliseconds kCloseGraceMs{ 500 };   // ~30 frames @60fps

        // io.DisplaySize LIES under Proton/upscalers: the Win32 backend reads
        // GetClientRect, which can disagree with the backbuffer. Cache the real
        // size and overwrite every frame (ENGINE_NOTES §9).
        float g_bbW = 0.0f, g_bbH = 0.0f;

        // Baked typefaces (MEO parity). body is added first so it is the DEFAULT
        // font -- all board text uses it; head is pushed for the drawn title.
        // Null -> the TTF was missing and ImGui's default bitmap font is used
        // (with the FontGlobalScale fallback).
        ImFont* g_fontBody = nullptr;

        ID3D11Device*        g_device  = nullptr;
        ID3D11DeviceContext* g_context = nullptr;

        // ── overlay health probe (marth validates the whole pipeline via one
        // grep of MFO.log over SSH; see the [overlay-probe] lines) ────────────
        std::atomic<std::uint64_t> g_probePresent{ 0 };   // rendered Present frames
        std::atomic<std::uint64_t> g_probeResize { 0 };   // ResizeBuffers fired
        std::atomic<std::uint64_t> g_probeKbd{ 0 }, g_probeMouse{ 0 }, g_probePad{ 0 };
        std::atomic<bool> g_hooksInstalled{ false };  // Present/Resize vtable hooks in
        std::atomic<bool> g_wndProcSwapped{ false };  // WndProc swap done (old proc captured)
        std::atomic<bool> g_controlsBlocked{ false }; // ControlMap consume edge (board open)

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

    }   // ── the anonymous namespace PAUSES here ────────────────────────────
    // CloseBoard needs EXTERNAL linkage: the module split moved DrawFieldKit
    // into Board_FieldKit.cpp, and its [B]/Esc close path calls this. It stays
    // DEFINED here, unmoved, at MFO::Board scope (declared in Board_internal.h)
    // so the overlay-probe atomics it reports stay file-local to this TU. The
    // anonymous namespace RESUMES immediately after it; body indentation is left
    // exactly as it was so the split shows no reformatting.
        // Close the board AND open the input-swallow grace, so the button press
        // that closed it can't leak its release to the game (Tween menu). The
        // grace ends early on the first release; kCloseGraceMs is only the safety
        // cap. WALL-clock deadline (see g_closeGrace) so a missed release can't
        // swallow input for the ~16s the old frame-count cap allowed.
        void CloseBoard() {
            g_open.store(false);
            g_closeGrace.store((std::chrono::steady_clock::now() + kCloseGraceMs)
                                   .time_since_epoch().count());
            // ACCEPTANCE GATE: one line that names the state of every overlay
            // component, emitted on each board close. If this reads present>0 +
            // every hook "ok", the whole pipeline is proven from the deck log.
            spdlog::info("[overlay-probe] SUMMARY present={} resize={} wndproc={} imgui={} "
                         "hooks={} input kbd={} mouse={} pad={} consumed-while-open={}",
                         g_probePresent.load(), g_probeResize.load(),
                         g_wndProcSwapped.load() ? "ok" : "no",
                         g_ready.load() ? "ok" : "no",
                         g_hooksInstalled.load() ? "ok" : "no",
                         g_probeKbd.load(), g_probeMouse.load(), g_probePad.load(),
                         g_controlsBlocked.load() ? "yes" : "no");
        }

    namespace {   // ── the anonymous namespace RESUMES ───────────────────────

        // A concise "what it does" line for a spell, synthesized from its costliest
        // effect (effect name + magnitude/duration/area). Spells carry no authored
        // DESC field -- the game composes the magic-menu tooltip from effects -- so
        // this mirrors that. MAIN-THREAD only (pure form-data reads; called from
        // PublishSnapshot), cached into the row so the render thread never re-reads.
        inline std::string SpellTooltip(RE::SpellItem* a_spell) {
            if (!a_spell) return {};
            auto* eff  = a_spell->GetCostliestEffectItem();
            auto* mgef = eff ? eff->baseEffect : nullptr;
            if (!eff || !mgef) return {};
            std::string s = (mgef->GetFullName() && *mgef->GetFullName()) ? mgef->GetFullName() : "";
            const int mag  = static_cast<int>(eff->GetMagnitude() + 0.5f);
            const int dur  = static_cast<int>(eff->effectItem.duration);
            const int area = static_cast<int>(eff->effectItem.area);
            if (mag  > 0) { if (!s.empty()) s += ' '; s += std::to_string(mag); }
            if (dur  > 0) { s += " for " + std::to_string(dur) + "s"; }
            if (area > 0) { s += " in " + std::to_string(area) + "ft"; }
            return s;
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
                    // Activity strip: red C combat, green L looting, blue T trading.
                    // Persistent bracket slots (marth 2026-09-06): the bracket is
                    // ALWAYS drawn -- only the letter comes and goes -- so the strip's
                    // width never shifts as state changes. Empty slot = dim "[ ]",
                    // live slot = coloured "[C]"/"[L]"/"[T]", butted into [ ][ ][ ].
                    if (r.inCombat) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "[C]");
                    else            ImGui::TextDisabled("[ ]");
                    ImGui::SameLine(0.0f, 0.0f);
                    if (r.looting)  ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "[L]");
                    else            ImGui::TextDisabled("[ ]");
                    ImGui::SameLine(0.0f, 0.0f);
                    if (r.trading)  ImGui::TextColored(ImVec4(0.45f, 0.6f, 1.0f, 1.0f), "[T]");
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

        // ── overlay hook: runtime swapchain-vtable + input sink ──────────────
        // WHY THIS SHAPE (v1.1 version-fragility kill). The old overlay installed
        // THREE call-site trampolines (write_call<5>) at HARDCODED in-function
        // byte offsets: D3DInit (+0x9/0x275), DXGIPresent (+0x9), InputDispatch
        // (+0x7B). Address Library resolves each function BASE on every runtime
        // but NOT the byte offset of the instruction inside it, so on 1.5.x /
        // 1.7.x write_call patched mid-instruction and crashed at load. This
        // rewrite carries ZERO game offsets:
        //   * Present (vtable slot 8) + ResizeBuffers (slot 13) are hooked on the
        //     LIVE IDXGISwapChain vtable. Those slots are frozen by the COM/DXGI
        //     ABI, identical on every Windows/D3D11 build regardless of the
        //     Skyrim version, which is exactly why validating on 1.6.1170
        //     generalizes to all runtimes. The swapchain is the SAME object the
        //     old code reached (BSGraphics::Renderer data.renderWindows[0].
        //     swapChain), so the pixels are byte-identical on 1.6.1170.
        //   * Input rides a BSTEventSink<InputEvent*> on BSInputDeviceManager
        //     (CommonLib API, no offset) for READING every device incl. gamepad,
        //     plus the WndProc swap (LazyInit) for text/focus. Game input is
        //     CONSUMED while the board is open by disabling controls (ControlMap),
        //     because a sink cannot null the event array the way the old dispatch
        //     hook did.
        //
        // PORTABLE UNIT (MAO/MEO copy verbatim): CreateRTV/ReleaseRTV, LazyInit,
        // SyncControlBlock, PresentThunk, ResizeBuffersThunk, HookSwapchainVtable,
        // TryInstallHooks, InputSink, Install(). Only the two Draw* calls in
        // PresentThunk and the hotkeys in InputSink are mod-specific.

        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using ResizeFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT);

        IDXGISwapChain*         g_swapChain   = nullptr;   // the hooked swapchain (probe)
        ID3D11RenderTargetView* g_rtv         = nullptr;   // backbuffer RTV for the overlay draw
        PresentFn               g_origPresent = nullptr;
        ResizeFn                g_origResize  = nullptr;
        std::once_flag          g_lazyOnce;
        constexpr std::uint64_t kProbeEveryN = 600;        // ~10s @60fps

        // The overlay draws at IDXGISwapChain::Present time, so it must bind the
        // backbuffer RTV itself (the old code piggybacked on Skyrim's own present
        // wrapper, which left one bound). Created in LazyInit + after every
        // ResizeBuffers, released before a resize.
        void CreateRTV(IDXGISwapChain* a_swap) {
            if (!g_device || g_rtv) return;
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(a_swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
                g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
                backBuffer->Release();
            }
        }
        void ReleaseRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

        // Build the ImGui context + DX11/Win32 backends the first time Present
        // fires (device / context / swapchain are all live by then). This is the
        // old D3DInitHook body, verbatim, minus the trampoline plumbing.
        void LazyInit(IDXGISwapChain* a_swap) {
            g_swapChain = a_swap;
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) { spdlog::error("[overlay-probe] no renderer -- Field Kit disabled"); return; }

            DXGI_SWAP_CHAIN_DESC sd{};
            if (FAILED(a_swap->GetDesc(&sd))) {
                spdlog::error("[overlay-probe] GetDesc failed -- Field Kit disabled");
                return;
            }

            // No casts: on the pinned NG these are already the real D3D types.
            g_device  = renderer->data.forwarder;
            g_context = renderer->data.context;
            if (!g_device || !g_context) {
                spdlog::error("[overlay-probe] no device/context -- Field Kit disabled");
                return;
            }
            spdlog::info("[overlay-probe] device={} context={} (non-null)",
                         static_cast<void*>(g_device), static_cast<void*>(g_context));

            ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;    // never write imgui.ini into the game dir
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
            // LOAD-BEARING: gamepad nav needs NavEnableGamepad AND HasGamepad. Our
            // vendored backend compiles its XInput poll out (v1.0.59), so this
            // init-time set persists and the sink-fed Gamepad* keys drive nav.
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

            // Bake real typefaces at backbuffer scale (MEO parity). MUST run
            // before the DX11 backend builds the atlas. Files optional.
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
                spdlog::info("[overlay-probe] fonts: body={} head={} (scale {:.2f})",
                             g_fontBody ? "ok" : "default", g_fontHead ? "ok" : "default", uiScale);
            }

            if (!ImGui_ImplWin32_Init(sd.OutputWindow) ||
                !ImGui_ImplDX11_Init(g_device, g_context)) {
                spdlog::error("[overlay-probe] ImGui backend init FAILED -- Field Kit disabled");
                return;
            }

            // WndProc swap: the sole window-message input path (text glyphs via
            // WM_CHAR, focus loss via WM_KILLFOCUS). Unchanged from the old code.
            WndProcHook::func = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(sd.OutputWindow, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
            g_wndProcSwapped.store(WndProcHook::func != nullptr);
            spdlog::info("[overlay-probe] wndproc swapped (old={} captured)",
                         reinterpret_cast<void*>(WndProcHook::func));

            g_bbW = static_cast<float>(sd.BufferDesc.Width);
            g_bbH = static_cast<float>(sd.BufferDesc.Height);
            CreateRTV(a_swap);

            g_ready.store(true);
            spdlog::info("[overlay-probe] imgui backend initialized -- Field Kit ready ({}x{})",
                         sd.BufferDesc.Width, sd.BufferDesc.Height);
        }

        // Consume game input while the board is open. A BSTEventSink cannot null
        // the shared event array (the old InputDispatch trampoline did), so we
        // disable the gameplay/menu control categories instead. Edge-triggered off
        // (board open || close-grace still running) and driven from Present every
        // frame, so controls re-enable the instant the grace window ends even when
        // no further input arrives (matching the old grace, which swallowed the
        // closing button's release so it could not leak to the game -> Tween menu).
        // ControlMap mutation is main-thread only, so it rides MainThread::Post.
        void SyncControlBlock() {
            const bool graceOn = g_closeGrace.load() != 0 &&
                std::chrono::steady_clock::now().time_since_epoch().count() < g_closeGrace.load();
            const bool want = g_open.load() || graceOn;
            if (want == g_controlsBlocked.load()) return;
            g_controlsBlocked.store(want);
            MainThread::Post([want]() {
                auto* cm = RE::ControlMap::GetSingleton();
                if (!cm) return;
                using F = RE::ControlMap::UEFlag;
                const auto flags = static_cast<F>(
                    static_cast<std::uint32_t>(F::kMovement)  | static_cast<std::uint32_t>(F::kLooking)  |
                    static_cast<std::uint32_t>(F::kActivate)  | static_cast<std::uint32_t>(F::kMenu)     |
                    static_cast<std::uint32_t>(F::kPOVSwitch) | static_cast<std::uint32_t>(F::kFighting) |
                    static_cast<std::uint32_t>(F::kSneaking)  | static_cast<std::uint32_t>(F::kMainFour) |
                    static_cast<std::uint32_t>(F::kWheelZoom));
                // ────────────────────────────────────────────────────────────
                // DO NOT call RE::ControlMap::ToggleControls HERE. This is the
                // worked example for engineering principle 6 -- "COMMONLIB
                // DECLARATIONS ARE NOT ABI-TRUSTWORTHY". The pinned
                // CommonLibSSE-NG 3.7.0 ControlMap.h is an *SE 1.5.97* layout and
                // its ToggleControls is not a call into the engine at all: it is
                // an inline C++ REIMPLEMENTATION that writes the member the header
                // calls `enabledControls`. That member's offset is wrong on our
                // runtime, so the reimplementation writes the wrong field.
                //
                //   UserEvents.h says INPUT_CONTEXT_ID::kTotal == 17. On
                //   1.6.1130+ there are EIGHTEEN input contexts, so every member
                //   after `controlMap[]` sits 8 bytes later:
                //
                //     member                 header (SE, 17)   runtime (AE, 18)
                //     controlMap[]             0x60              0x60   (same)
                //     linkedMappings           0xE8              0xF0
                //     contextPriorityStack     0x100             0x108
                //       .data                  0x100             0x108
                //       .capacity              0x108             0x110
                //       .size                  0x110             0x118
                //     enabledControls          0x118             0x120
                //     unk11C                   0x11C             0x124
                //
                //   So the header's `enabledControls` at +0x118 IS
                //   `contextPriorityStack.size` on this runtime. With our mask
                //   0x3EF (= 1007) every board OPEN did `size &= ~0x3EF` (1 -> 0)
                //   and every board CLOSE did `size |= 0x3EF` (-> 1007). The
                //   engine's per-frame user-event mapping pass (AE id 68542,
                //   0x140CD4F70) then walks a 1007-deep context stack out of a
                //   buffer holding one or two entries -- `mov eax,[rdi+0x118]`,
                //   `rbx = [rdi+0x108] + (size-1)*4`, `movsxd rax,[rbx]`,
                //   `mov r13,[rdi+rax*8+0x60]` -- and that last load is the AV at
                //   SkyrimSE.exe+0CD509D. (Field-reproduced 2026-09-08/09, ~6 min
                //   into a session, same save, after 30+ previously clean hours.)
                //   The same corruption is also a latent out-of-bounds WRITE: the
                //   next PushInputContext runs with size >= capacity and writes
                //   past the buffer. Calling the engine removes both.
                //
                // Call the engine's own ToggleControls instead. Signature and ids
                // are VERIFIED against the disassembled binaries, not the header:
                //   AE id 68545 -> 0xCD5650 (versionlib-1-6-1170-0)
                //   SE id 67245 -> 0xC11C60 (version-1-5-97-0)
                // Both disassemble to the same body: (rcx=this, edx=flags,
                // r8b=enable, r9b=storeState), writing enabledControls
                // (SE +0x118 / AE +0x120) and, when storeState, unk11C
                // (SE +0x11C / AE +0x124), then SendEvent on the BSTEventSource
                // at +0x08. storeState=true reproduces exactly what the header's
                // inline version did to unk11C, so behaviour is unchanged.
                //
                // VR: RelocationID's two-arg form mirrors the SE id into the VR
                // slot, and no VR id has been verified. The overlay already
                // REFUSES VR in Install() before any hook is placed, so
                // PresentThunk (and therefore this function-local static) can
                // never run there -- the explicit guard below is the same
                // belt-and-suspenders `Probe.cpp:34` uses for an unsourced VR id,
                // so a future caller of SyncControlBlock cannot resolve 67245
                // against the VR address library and call into garbage.
                //
                // Input consumption is UNCHANGED by using the correct field: the
                // mapping pass blanks `userEvent` when
                // (enabledControls & mapping.flags) != mapping.flags, which is what
                // stops the GAME reacting, while MFO's own InputSink matches on
                // ButtonEvent::GetIDCode() only (`:682`, `:710`, `:728`, `:749`,
                // `:785` -- it never reads `userEvent`), so the board keeps seeing
                // every key and pad button while it is open.
                // ────────────────────────────────────────────────────────────
                if (REL::Module::IsVR()) return;   // no sourced VR id -- refuse, never guess
                using ToggleFn = void(RE::ControlMap*, RE::UserEvents::USER_EVENT_FLAG,
                                      bool a_enable, bool a_storeState);
                static REL::Relocation<ToggleFn> toggle{ REL::RelocationID(67245, 68545) };
                toggle(cm, flags, !want, true);   // want -> disable, else re-enable
            });
        }

        // The overlay's render frame + input consume-sync. Hooked into vtable slot
        // 8. We draw ImGui onto the current backbuffer, THEN call the original
        // Present (the standard swapchain-hook order; the old code hooked Skyrim's
        // higher-level present wrapper so it drew after -- steady-state pixels are
        // identical). Runs on the render thread, install-once.
        HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_this, UINT a_sync, UINT a_flags) {
            std::call_once(g_lazyOnce, [&] { LazyInit(a_this); });

            SyncControlBlock();   // every frame -- drives the grace-expiry re-enable

            const bool wantPanel = g_open.load();
            const bool wantHud   = g_hud.load();
            if (g_ready.load() && (wantPanel || wantHud)) {
                // Copy the snapshot BEFORE the IO lock (#6: never nested).
                Snapshot snap;
                {
                    std::scoped_lock snapLk(g_snapMx);
                    snap = g_snapshot;
                }
                std::scoped_lock lk(g_ioMx);

                ImGui::GetIO().MouseDrawCursor = wantPanel;

                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();

                // MUST sit between the two NewFrame calls (ENGINE_NOTES §9).
                if (g_bbW > 0.0f) ImGui::GetIO().DisplaySize = ImVec2(g_bbW, g_bbH);
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
                if (g_rtv) g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

                const auto n = g_probePresent.fetch_add(1) + 1;
                if (n % kProbeEveryN == 0)
                    spdlog::info("[overlay-probe] present frames={} (rendering)", n);
            }

            return g_origPresent(a_this, a_sync, a_flags);
        }

        // Backbuffer resize (vtable slot 13). Drop the RTV, let the engine resize,
        // then rebuild the RTV and the cached backbuffer size. Same render thread
        // as Present (not nested), so the io lock is uncontended here.
        HRESULT STDMETHODCALLTYPE ResizeBuffersThunk(IDXGISwapChain* a_this, UINT a_count,
                                                     UINT a_w, UINT a_h, DXGI_FORMAT a_fmt,
                                                     UINT a_flags) {
            {
                std::scoped_lock lk(g_ioMx);
                ReleaseRTV();
            }
            const HRESULT hr = g_origResize(a_this, a_count, a_w, a_h, a_fmt, a_flags);
            {
                std::scoped_lock lk(g_ioMx);
                if (g_ready.load()) CreateRTV(a_this);
                if (a_w != 0 && a_h != 0) {
                    g_bbW = static_cast<float>(a_w);
                    g_bbH = static_cast<float>(a_h);
                }
            }
            const auto n = g_probeResize.fetch_add(1) + 1;
            spdlog::info("[overlay-probe] ResizeBuffers fired #{} ({}x{})", n, a_w, a_h);
            return hr;
        }

        // Swap vtable slots 8 (Present) and 13 (ResizeBuffers) on the live
        // IDXGISwapChain. Both indices are frozen by the COM/DXGI ABI, so this is
        // version-independent -- no Address Library id, no in-function offset.
        bool HookSwapchainVtable(IDXGISwapChain* a_swap) {
            void** vtbl = *reinterpret_cast<void***>(a_swap);
            auto patch = [](void** slot, void* hook, void** orig) -> bool {
                DWORD prot = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &prot)) return false;
                *orig = *slot;
                *slot = hook;
                VirtualProtect(slot, sizeof(void*), prot, &prot);
                return true;
            };
            const bool okP = patch(&vtbl[8],  reinterpret_cast<void*>(&PresentThunk),
                                   reinterpret_cast<void**>(&g_origPresent));
            const bool okR = patch(&vtbl[13], reinterpret_cast<void*>(&ResizeBuffersThunk),
                                   reinterpret_cast<void**>(&g_origResize));
            spdlog::info("[overlay-probe] swapchain={} present-hook(slot8)={} resize-hook(slot13)={}",
                         static_cast<void*>(a_swap), okP ? "ok" : "FAIL", okR ? "ok" : "FAIL");
            return okP && okR;
        }

        // Poll for the live swapchain (it may not be up the instant Install runs
        // at kDataLoaded), then install the vtable hooks once. Bounded retries so
        // a genuinely absent swapchain can't spin the task queue forever.
        void TryInstallHooks() {
            if (g_hooksInstalled.load()) return;
            static std::atomic<int> s_attempts{ 0 };
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            IDXGISwapChain* swap = renderer ? renderer->data.renderWindows[0].swapChain : nullptr;
            if (!swap) {
                if (s_attempts.fetch_add(1) < 600) {   // ~ generous startup grace
                    SKSE::GetTaskInterface()->AddTask([] { TryInstallHooks(); });
                } else {
                    spdlog::error("[overlay-probe] swapchain never appeared -- Field Kit disabled");
                }
                return;
            }
            spdlog::info("[overlay-probe] swapchain acquired (non-null) after {} poll(s) -- hooking vtable",
                         s_attempts.load());
            if (HookSwapchainVtable(swap)) g_hooksInstalled.store(true);
        }

        // ── input: the sole engine-side input path (replaces InputDispatchHook) ─
        // A BSTEventSink on BSInputDeviceManager receives the EXACT same InputEvent
        // stream the old dispatch trampoline saw (keyboard / mouse / gamepad /
        // thumbstick), version-independently. The per-event translation below is
        // the old InputDispatchHook body verbatim; the one thing a sink cannot do
        // is null the array to consume, so consumption is handled by
        // SyncControlBlock (ControlMap) instead. Runs on the input thread.
        class InputSink final : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static InputSink* GetSingleton() { static InputSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
                                                  RE::BSTEventSource<RE::InputEvent*>*) override {
                using Ctrl = RE::BSEventNotifyControl;

                // THE FOCUS HOTKEY IS HANDLED BEFORE THE PANEL CHECK, and that is
                // the entire point: a menu button cannot be a target picker,
                // because opening the menu takes the mouse and freezes the
                // crosshair you were supposed to be aiming. This runs while the
                // panel is CLOSED and never consumes -- the default key is unbound
                // in vanilla, so the game can have it.
                if (a_events && !g_open.load()) {
                    const int fk = Config::g_focusKey.load();
                    if (fk != 0) {
                        for (auto* e = *a_events; e; e = e->next) {
                            if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                            auto* b = static_cast<RE::ButtonEvent*>(e);
                            if (!b->IsDown()) continue;                 // edge only
                            if (b->device.get() != RE::INPUT_DEVICE::kKeyboard) continue;
                            if (static_cast<int>(b->GetIDCode()) != fk) continue;
                            // SEV-1: FocusOnCrosshair reads follower membership; run its
                            // deferred body under PumpTickGate like every MFO AddTask body
                            // so a revert/save draining the pump makes it bail (#4).
                            // (Follow-up, OUT OF THIS WAVE'S SCOPE: FocusOnCrosshair in
                            // Probe.cpp still probes via the unlocked Followers::IsTracked
                            // walk -- switch it to IsTrackedFast to fully close the read.)
                            const auto epoch = MFO::Diagnostics::CurrentPumpEpoch();
                            SKSE::GetTaskInterface()->AddTask([epoch]() {
                                MFO::Diagnostics::PumpTickGate gate(epoch);
                                if (!gate) return;
                                Probe::FocusOnCrosshair();
                            });
                        }
                    }

                    // PROGRESSION PROBE HOTKEY (dev-only, bProgProbe=0 for everyone
                    // else). The probe MUTATES engine state (AddPerk /
                    // SetBaseActorValue) so it rides MainThread::Post, never AddTask
                    // -- AddTask drains on a job worker in this runtime (§0.37).
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
                    // bProgHarness=0 for everyone else). Same shape; mutates engine
                    // state so it rides MainThread::Post. The verb comes from the
                    // addon's MFOP_DevCmd GLOB (console-set) -- see ProgAllocator.h.
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

                    // CSTY RE-ASSERT A/B TOGGLE HOTKEY (Docs/SPEC-COMBATSTYLE-
                    // SOURCEGATE.md). Same shape as the keys above, but the
                    // toggle itself is a bare atomic bool CombatStyle::ApplyTick
                    // already reads with memory_order_relaxed -- no engine
                    // mutation, so this flips it in place rather than routing
                    // through MainThread::Post/AddTask. Lets marth flip
                    // bCstyReassert mid-battle to A/B the positioning stutter.
                    {
                        const int ck = Config::g_cstyReassertKey.load();
                        if (ck != 0) {
                            for (auto* e = *a_events; e; e = e->next) {
                                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) continue;
                                auto* b = static_cast<RE::ButtonEvent*>(e);
                                if (!b->IsDown()) continue;                 // edge only
                                if (b->device.get() != RE::INPUT_DEVICE::kKeyboard) continue;
                                if (static_cast<int>(b->GetIDCode()) != ck) continue;
                                const bool now = !Config::g_cstyReassert.load();
                                Config::g_cstyReassert.store(now);
                                spdlog::info("[wstyle] bCstyReassert HOTKEY -> {}", now ? "ON (re-assert every tick)"
                                                                                        : "OFF (one-shot stance + equip gate only)");
                            }
                        }
                    }
                }

                // CLOSE GRACE: the board just closed (g_open false) but we are
                // still inside the swallow window. The consume is now done by
                // ControlMap (controls stay disabled until the grace ends), so the
                // sink only has to END the grace early the instant we see a release
                // -- then SyncControlBlock re-enables controls on the next frame.
                if (a_events && !g_open.load() && g_closeGrace.load() != 0 &&
                    std::chrono::steady_clock::now().time_since_epoch().count() < g_closeGrace.load()) {
                    for (auto* e = *a_events; e; e = e->next)
                        if (e->eventType == RE::INPUT_EVENT_TYPE::kButton &&
                            !static_cast<RE::ButtonEvent*>(e)->IsDown()) { g_closeGrace.store(0); break; }
                    return Ctrl::kContinue;
                }

                if (!g_ready.load() || !g_open.load() || !a_events) {
                    return Ctrl::kContinue;
                }

                std::scoped_lock ioLk(g_ioMx);
                auto& io = ImGui::GetIO();

                for (auto* e = *a_events; e; e = e->next) {
                    if (e->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                        auto* b = static_cast<RE::ButtonEvent*>(e);
                        // Skyrim re-fires button events every input frame while a
                        // key is HELD. Filter to real edges (MEO's filter, verbatim).
                        if (!b->IsDown() && !b->IsUp()) continue;
                        const auto code = b->GetIDCode();
                        const bool down = b->IsDown();

                        switch (b->device.get()) {
                        case RE::INPUT_DEVICE::kMouse:
                            g_probeMouse.fetch_add(1);
                            if (code <= 4) io.AddMouseButtonEvent(static_cast<int>(code), down);
                            else if ((code == 8 || code == 9) && down)
                                io.AddMouseWheelEvent(0.0f, code == 8 ? 1.0f : -1.0f);
                            break;

                        case RE::INPUT_DEVICE::kKeyboard:
                            g_probeKbd.fetch_add(1);
                            if (code == 0x01 || code == 0x0F) {          // Esc / Tab
                                // Forward Escape (both edges) so ImGui's keyboard
                                // nav-cancel closes an open picker and the render
                                // thread's resolver sees the press. The board-close
                                // decision is made THERE (off IsKeyPressed).
                                io.AddKeyEvent(ImGuiKey_Escape, down);
                            } else if (code == ShoutKey(RE::INPUT_DEVICE::kKeyboard)) {
                                // Close on RELEASE. Closing on the press leaks the
                                // release to the game, which re-casts the power and
                                // instantly reopens.
                                if (down) g_shoutDownSeen = true;
                                else if (g_shoutDownSeen.exchange(false)) g_wantClose = true;
                            } else if (auto k = DIKToImGuiKey(code); k != ImGuiKey_None) {
                                io.AddKeyEvent(k, down);
                            }
                            break;

                        case RE::INPUT_DEVICE::kGamepad:
                            g_probePad.fetch_add(1);
                            if (static_cast<RE::BSWin32GamepadDevice::Key>(code) ==
                                    RE::BSWin32GamepadDevice::Key::kB) {
                                // CASCADED BACK. THIS SINK is B's one and only
                                // source (v1.0.59): forward it as GamepadFaceRight,
                                // ImGui's nav-cancel, off Skyrim's ButtonEvent
                                // stream -- the stream that survives Steam Input's
                                // kb<->pad mode flips, unlike the Win32 backend's
                                // XInput poll (compiled out on the vendored backend
                                // TU). ImGui nav-cancel closes the innermost picker
                                // at NewFrame; the render-thread resolver decides the
                                // board close off this same keypress.
                                io.AddKeyEvent(ImGuiKey_GamepadFaceRight, down);
                            } else if (auto k = GamepadToImGuiKey(code); k != ImGuiKey_None) {
                                // R1 -> GamepadR1 becomes the in-board party-switch
                                // while open (the opening R1 press was seen while
                                // CLOSED, so it never reaches here).
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
                            // EDGE-TRIGGERED into d-pad keys. ImGui's own nav repeat
                            // handles a held direction; passing the axis through
                            // would scroll continuously.
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

                // The game still RECEIVES these events (a sink cannot null the
                // array), but SyncControlBlock has disabled the gameplay/menu
                // control categories for the whole time the board is open, so they
                // are inert -- no vanilla menu bleed-through, no control-flag churn.
                return Ctrl::kContinue;
            }
        };

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
            // v1.1 Phase 6c: the generic add-on-action carrier. EditKind names
            // only AddonAction; the specific verb rides c.verbId (AddonVerb).
            // The verb→backend dispatch below stays progression-shaped (Phase
            // 7/9 routes it through the manifest).
            if (c.kind == EditKind::AddonAction) {
                const int   verb  = c.verbId;
                const auto  fid   = c.fid;
                const auto  perk  = c.perk;   // §18.6: SetClass carries the class-def id here
                const float param = c.param;
                // Capture the revert/reload generation at POST time. If a revert
                // (ClearAll) or reload (OnPostLoad) bumps it before this closure
                // runs, MainThread::Clear was meant to drop us — but if the queue
                // raced the Clear, bail here so a stale edit can't land on the
                // NEXT save's same-FormID actor.
                const int gen = ProgAllocator::PollGeneration();
                MainThread::Post([verb, fid, perk, param, gen]() {
                    if (ProgAllocator::PollGeneration() != gen) {
                        spdlog::info("[prog] board edit dropped: superseded by revert/reload "
                                     "(actor {:08X})", fid);
                        return;
                    }
                    auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
                    if (!actor) {
                        spdlog::info("[prog] board edit dropped: actor {:08X} unresolvable", fid);
                        return;
                    }
                    // item 2b: SetClass/Respec below reach Followers::TryEnsureRecord
                    // on THIS main thread. That is only safe because a board-
                    // addressable follower already has a g_followers record (so
                    // TryEnsureRecord finds, never inserts). This scope arms the
                    // tripwire that logs a HAZARD if any of them ever inserts.
                    Followers::BoardEditScope boardEditGuard;
                    switch (static_cast<AddonVerb>(verb)) {
                    case AddonVerb::SetClass:
                        // The §15 onboarding as ONE player action: enroll on
                        // first contact (refuses with a named line when
                        // already enrolled), then the class pick auto-scales.
                        // §18.6: perk holds the declared class-def FormID.
                        if (auto it2 = ProgAllocator::g_prog.find(fid);
                            it2 == ProgAllocator::g_prog.end() || !it2->second.enrolled)
                            ProgAllocator::Enroll(actor);
                        ProgAllocator::SetClass(actor, perk);
                        break;
                    case AddonVerb::AllocPerk:
                        ProgAllocator::AllocatePerk(actor, perk);
                        break;
                    case AddonVerb::Respec:
                        ProgAllocator::Respec(actor);
                        break;
                    case AddonVerb::SetManual:   // #74 §16
                        ProgAllocator::SetManualSkills(actor, param > 0.5f);
                        break;
                    case AddonVerb::ApplySkillPoint:   // #74 §16 — param = AV ordinal
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

            // T#78: the per-follower MFO master switch. A per-FOLLOWER edit like
            // SetClassOverride above (uid/table unused, keyed on c.fid). This
            // ONLY flips the bool. ApplyEdits drains on the SAME task worker that
            // owns g_followers and runs Scheduler::Tick (MAP: the Board.h
            // "MAIN THREAD ONLY" header is corrected), so this write and the
            // tick's read are same-thread, lock-free (#4) -- identical to how the
            // combat stance is written here and read on the tick. The release-on-
            // disable is deliberately NOT done here: it runs on the OFF edge in
            // Scheduler's per-follower tick, sharing the exact helper + worker as
            // the dismissal sweep (Followers::ReleaseHeldState).
            if (c.kind == EditKind::SetMfoEnabled) {
                it->second.mfoEnabled = (c.param > 0.5f);
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
                // When the ParamKind changes (e.g. Percent -> Count), the old
                // param is meaningless in the new kind — a stale 0.5 left on a
                // Count condition reads as silently always/never-true. Snap it to
                // the new kind's default.
                const ParamKind oldKind = kindFor(tab[i].conditionOpcode, t, tn);
                const ParamKind newKind = t[n].kind;
                tab[i].conditionOpcode = t[n].op;
                if (oldKind != newKind) {
                    switch (newKind) {
                    case ParamKind::Percent:  tab[i].conditionParam = 0.5f;   break;
                    case ParamKind::Count:    tab[i].conditionParam = 5.0f;   break;
                    case ParamKind::Distance: tab[i].conditionParam = 500.0f; break;
                    case ParamKind::None:
                    default:                  tab[i].conditionParam = 0.0f;   break;
                    }
                }
                break; }
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
        // #74 / 6b component 3: the GENERIC hosted board-tab list (refcount copy
        // of the allocator's published views, one envelope per declared add-on
        // tab; built on the MAIN thread — this drain runs on the task worker, so
        // it must never read g_prog itself, only the immutable snap).
        s.boardTabs = ProgAllocator::CopyBoardTabViews();

        auto* player = RE::PlayerCharacter::GetSingleton();

        // Active followers first, with live vitals.
        //
        // THREADING (#4 + Wave-1): this drain runs on the serial TASK WORKER —
        // the same domain that owns g_active/g_followers and runs Scheduler::Tick
        // and Followers::Refresh — so the roster reads here do not race those.
        // The one cross-thread writer that CAN touch state mid-drain is a Board
        // Prog edit (SetClass/Respec) that rides MainThread::Post; item 2b proves
        // that path never INSERTS into g_followers for a rostered follower. For
        // defence-in-depth we still walk the Wave-1 any-thread FormID mirror
        // (Followers::ActiveSnapshot) instead of the raw g_active handle vector,
        // matching the discipline every other off-serial reader now follows.
        auto activeIds = Followers::ActiveSnapshot();
        for (RE::FormID fid : *activeIds) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!a) continue;
            FollowerRow r;
            r.id        = a->GetFormID();
            r.name      = a->GetName() ? a->GetName() : "?";   // null-guard like every sibling
            r.active    = true;
            r.teammate  = a->IsPlayerTeammate();
            r.commanded = a->IsCommandedActor();
            r.inCombat  = a->IsInCombat();
            // Activity glyphs (worker-domain reads; this drain IS the worker, #4).
            // r.looting reads JustLooted, NOT IsLooting: IsLooting is a travel-
            // slot proxy (true the whole walk-there, even on an arrival that
            // finds nothing; false for arm's-reach loot that never claims a
            // slot at all) -- neither necessary nor sufficient for a real
            // pickup. JustLooted stamps only at a confirmed acquisition.
            r.looting   = Logistics::JustLooted(r.id);
            r.trading   = Logistics::IsTrading(r.id);

            // ONE formula, shared with the evaluator. These bars are how the
            // player checks why a rule did or did not fire, so a HUD that
            // computed HP% differently from the thing deciding would make the
            // Field Kit lie at exactly the moment it is consulted.
            r.healthPct  = Vocab::HealthPct(a);
            r.magickaPct = Vocab::MagickaPct(a);
            r.staminaPct = Vocab::StaminaPct(a);
            // Raw H/M/S current+max for the Followers-tab readout (SAME reads as the
            // pct bars; precomputed here on main so the render thread reads floats).
            r.healthCur  = Vocab::VitalCur(a, RE::ActorValue::kHealth);
            r.healthMax  = Vocab::VitalMax(a, RE::ActorValue::kHealth);
            r.magickaCur = Vocab::VitalCur(a, RE::ActorValue::kMagicka);
            r.magickaMax = Vocab::VitalMax(a, RE::ActorValue::kMagicka);
            r.staminaCur = Vocab::VitalCur(a, RE::ActorValue::kStamina);
            r.staminaMax = Vocab::VitalMax(a, RE::ActorValue::kStamina);
            if (player) {
                r.distance   = a->GetPosition().GetDistance(player->GetPosition());
                r.playerName = player->GetName() ? player->GetName() : "Player";
            }
            // #68: the cast-target picker's follower list -- every OTHER
            // active teammate by name (never this row's own actor). Same
            // roster ActiveSnapshot feeds the outer loop.
            for (RE::FormID aid : *activeIds) {
                if (aid == fid) continue;
                auto* ally = RE::TESForm::LookupByID<RE::Actor>(aid);
                if (!ally) continue;
                r.alliesForPicker.emplace_back(ally->GetFormID(),
                                               ally->GetName() ? ally->GetName() : "?");
            }

            if (auto it = g_followers.find(r.id); it != g_followers.end()) {
                r.rapport        = it->second.rapport;
                r.rank           = it->second.rank;
                r.combatClassOverride = it->second.combatClassOverride;
                r.mfoEnabled     = it->second.mfoEnabled;   // T#78
                r.combatRules    = static_cast<std::uint8_t>(it->second.combat().size());
                r.logisticsRules = static_cast<std::uint8_t>(it->second.logistics().size());
                r.combatSlots    = SlotsForRank(it->second.rank, Table::Combat);
                r.logisticsSlots = SlotsForRank(it->second.rank, Table::Logistics);
                FillRuleViews(r.combat,    it->second.combat());
                FillRuleViews(r.logistics, it->second.logistics());
                // The follower's castable spells, for the board's picker. Same
                // VisitSpells pattern the seed uses. Precompute the tooltip metrics
                // HERE (main thread): CalculateMagickaCost is the ACTOR overload
                // (bakes in this follower's skill+perks -- the SAME number that gates
                // casting in Actuation) and the description is synthesized from the
                // spell's effects; both are cached so the render thread reads values.
                struct SpellList : RE::Actor::ForEachSpellVisitor {
                    std::vector<FollowerRow::SpellPick>* out;
                    RE::Actor*                           follower;
                    RE::BSContainer::ForEachResult Visit(RE::SpellItem* sp) override {
                        if (sp && MFO::Vocab::IsCastableSpell(sp)) {
                            FollowerRow::SpellPick p;
                            p.id          = sp->GetFormID();
                            p.name        = sp->GetName() ? sp->GetName() : "?";
                            p.magickaCost = follower ? static_cast<int>(sp->CalculateMagickaCost(follower) + 0.5f) : 0;
                            p.tooltip     = SpellTooltip(sp);
                            out->push_back(std::move(p));
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                } vis; vis.out = &r.knownSpells; vis.follower = a;
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
                                                      sp->GetName() ? sp->GetName() : "?",
                                                      static_cast<int>(sp->CalculateMagickaCost(a) + 0.5f),
                                                      SpellTooltip(sp) });
                    }
                }
            }
            r.combatSlots    = SlotsForRank(r.rank, Table::Combat);
            r.logisticsSlots = SlotsForRank(r.rank, Table::Logistics);
            s.rows.push_back(std::move(r));
        }

        // Then retained-but-inactive records, so dismissal is visibly
        // non-destructive rather than something you take on faith. This g_followers
        // iteration is worker-domain (see the threading note above); the only
        // rehash hazard is a MAIN-thread insert, which item 2b closes by proving
        // the Board Prog-edit path never inserts for a rostered follower. Active
        // membership goes through the Wave-1 any-thread mirror (IsTrackedFast).
        for (const auto& [id, st] : g_followers) {
            if (Followers::IsTrackedFast(id)) continue;
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
            r.mfoEnabled     = st.mfoEnabled;   // T#78
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
        // REFUSE-AND-GATE before any install (item 4). The overlay was never
        // VR-safe (same as the combat vtable hooks, plugin.cpp:293-295); refuse
        // VR and name the runtime so the deck log says exactly what ran. The
        // vtable + sink path below carries ZERO game offsets, so on flat-screen
        // SE / AE / future runtimes it is version-independent -- the gate is
        // belt-and-suspenders plus the right place to refuse VR, and it now runs
        // BEFORE install (the old call-site trampolines installed unconditionally
        // before the version was even read).
        const auto& mod = REL::Module::get();
        if (REL::Module::IsVR()) {
            spdlog::warn("[overlay-probe] VR runtime ({}) -- Field Kit overlay REFUSED", mod.version().string());
            return;
        }
        spdlog::info("[overlay-probe] install begin (runtime {}, flat-screen)", mod.version().string());

        // Input: a version-independent BSTEventSink on BSInputDeviceManager reads
        // every device incl. gamepad (replaces the InputDispatch call-site
        // trampoline). No byte offset. The WndProc swap is done later, in LazyInit.
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputSink::GetSingleton());
            spdlog::info("[overlay-probe] input sink registered on BSInputDeviceManager");
        } else {
            spdlog::error("[overlay-probe] no BSInputDeviceManager -- input sink NOT registered");
        }

        // Present / ResizeBuffers: poll for the live swapchain, then patch vtable
        // slots 8 / 13. Deferred to a task because the swapchain may not be up the
        // instant Install() runs at kDataLoaded.
        TryInstallHooks();
        spdlog::info("[overlay-probe] install kicked (swapchain poll running)");
    }

}
