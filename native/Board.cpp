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
        void DrawFieldKit(const Snapshot& snap) {
            if (g_wantClose.exchange(false)) { g_open = false; return; }

            auto& io = ImGui::GetIO();

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                    ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.60f, io.DisplaySize.y * 0.62f),
                                     ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f, 400.0f), io.DisplaySize);

            if (!ImGui::Begin("MFO Field Kit", nullptr,
                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::End();
                return;
            }

            const auto pv = SKSE::PluginDeclaration::GetSingleton()->GetVersion();
            ImGui::TextDisabled("MFO v%u.%u.%u  |  frame %llu  |  %.1f min",
                                pv.major(), pv.minor(), pv.patch(),
                                static_cast<unsigned long long>(snap.frame), snap.minutes);
            ImGui::Separator();

            if (ImGui::BeginTabBar("##tabs")) {

                if (ImGui::BeginTabItem("Followers")) {
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

                if (ImGui::BeginTabItem("Measurements")) {
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

                if (ImGui::BeginTabItem("Probe")) {
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

                if (ImGui::BeginTabItem("Config")) {
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

            ImGui::Separator();
            ImGui::TextDisabled("Field Orders / Esc / B closes.  Rule editing arrives at M7.");
            ImGui::End();
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

                        switch (b->device.get()) {
                        case RE::INPUT_DEVICE::kMouse:
                            if (code <= 4) io.AddMouseButtonEvent(static_cast<int>(code), down);
                            else if ((code == 8 || code == 9) && down)
                                io.AddMouseWheelEvent(0.0f, code == 8 ? 1.0f : -1.0f);
                            break;

                        case RE::INPUT_DEVICE::kKeyboard:
                            if (code == 0x01 || code == 0x0F) {          // Esc / Tab
                                if (down) g_wantClose = true;
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
                                    RE::BSWin32GamepadDevice::Key::kB && down) {
                                g_wantClose = true;
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

    void PublishSnapshot() {
        Snapshot s;
        s.frame = g_frame.fetch_add(1);

        s.combatEvents = Rapport::CombatEventCount();
        s.minutes      = Rapport::SessionMinutes();
        s.kills        = Rapport::SessionKills();
        s.rapport      = Rapport::SessionRapport();

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
