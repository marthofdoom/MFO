# MFO x Tailor compatibility: diagnosis (read-only, 2026-11-09)

ClickUp 1dfcuabcrr7. Report: "Tailor cannot preview outfits", user on **MFO 2.8.1**, and (marth's comment) it hits
**followers and non-followers**. No log, crash, runtime version or exact symptom wording is attached. "Cannot preview"
could mean: the UI won't open, it opens but gets no input, the preview camera/actor won't start, or pieces don't show.
That ambiguity is the biggest gap in the evidence.

Tailor source: `scratchpad/ext-tailor/src` (GPL-3.0, read only). MFO at `/mnt/gaming/modlists/Projects/marth-follower-overhaul`.
Harbinger (APMF) at `/mnt/gaming/modlists/Projects/ai-package-management-framework`.

## Hook inventory (what each side actually patches)

| Seat | Tailor | MFO | Overlap? |
|---|---|---|---|
| `BSInputDeviceManager::PollInputDevices` call at `RelocationID(67315,68617)+0x7B` | `write_call<5>` at **kPostPostLoad** (`src/main.cpp:145-148`, `src/ui/imgui/InputDispatchHook.cpp:98-112`); checks for `E8` first, chains the existing target | `write_call<5>` at **SKSEPluginLoad** (`native/Board.cpp:1919`, `WriteThunkCall` `:1209`, thunk `:1193-1199`); 1.6.1170 + 1.5.97 only | **YES, same 5 bytes.** Chained, not clobbered: MFO patches first, Tailor reads MFO's E8 and becomes the outer hook. Order is engine -> Tailor thunk -> MFO thunk -> real dispatch -> sinks (Tailor's ImGui sink is one of them) |
| BSInputDeviceManager event sink | `ImGuiHost::Initialize` `AddEventSink(this)` (`ImGuiHost.cpp:104`), its ImGui input source | not registered on the trampoline runtimes (sink path only elsewhere) | downstream of MFO's thunk |
| `CursorMenu` vfunc 0x5 AdvanceMovie | `TailorPreviewSession.cpp:57-87`, ticks the preview | none | no |
| D3D11 / IDXGISwapChain | none. Draws inside its own IMenu `TailorMenu::PostDisplay` (`TailorMenu.cpp:24` -> `ImGuiHost::DrawFrame` `ImGuiHost.cpp:309`) | Present/ResizeBuffers vtable slots 8/13 (`Board.cpp:843-880`, thunk `:730`) | no (Tailor never hooks Present) |
| WndProc | **none** (no `SetWindowLongPtr` anywhere in Tailor) | `SetWindowLongPtrA` subclass (`Board.cpp:546-548`), thunk acts only on WM_KILLFOCUS and on WM_CHAR **while the board is open** (`:357-374`) | no |
| ImGui | static imgui 1.92.6 via xmake (`xmake.lua:12`), saves/restores the current context around its draw (`ImGuiHost.cpp:38-42`) | static imgui 1.92.8, `x64-windows-static-md` (`native/CMakePresets.json:15`), vendored win32 backend | separate GImGui per DLL. No shared context |
| Equip | plain `ActorEquipManager` Equip/Unequip (`OutfitManager.cpp` ~244-288), TESEquipEvent + NiNodeUpdate sinks (`TailorPreviewSession.cpp:89-110`) | `BeastHeadSink` TESEquipEvent (`logistics/Sinks.cpp:53-120`): observer for tracked followers, KeepHeadClear only for `IsPlayerTeammate()` | teammates only |
| Harbinger equip deny | n/a | `EquipSink.cpp:568-585`: player and every **unclaimed** actor go straight to the original worker | managed followers only |
| Other MFO globals | n/a | `Character::UpdateCombat` (`Targeting.cpp:200`), `PlayerCharacter::Update` drain (`MainThread.cpp:115`), MenuOpenClose sink (Journal close only, `Diagnostics.cpp:155-183`) | nothing Tailor's preview touches |
| Other Harbinger globals | n/a | Character slots 0x49/0xDC/0xE3/0xE1/0xE5, AV owner, combat casters. No camera, menu, swapchain or input-dispatch seat (grep). `Sightline` only CALLS `bhkWorld::PickObject`, no hook | no |

## Hypotheses, ranked

### H1. Input-dispatch chain at 67315/68617+0x7B: MFO swallows Tailor's input (LOW-MEDIUM; the only real byte overlap)
- For: both patch the same call. MFO's thunk sets the caller's head pointer to `nullptr` when `Feed` returns true
  (`Board.cpp:1196`), so every downstream sink, Tailor's ImGui sink (`ImGuiHost.cpp:104`) included, gets nothing.
- Against: `Feed` returns true only while the MFO board is **open** or during the short **close grace** right after
  it closes (`Board.cpp:1051-1060`). With the board closed it returns false and passes everything through. Tailor
  chains correctly: it installs later and wraps MFO, and its E8 check passes because MFO wrote an E8. When Tailor hides
  keys, it hands MFO a stack-local `visible[]` array (`InputDispatchHook.cpp:72-76`); MFO only nulls it while its board
  is up. MFO's hotkeys (focus, board, HUD) never consume.
- It would bite only if the MFO board is open (or stuck open in MFO's state while invisible) when Tailor is used, or if
  an MFO hotkey shares Tailor's toggle key (MFO's then fires too, but still does not consume).
- Settles it: MFO log `[overlay-probe] input trampoline installed on 68617+0x7B` plus `[board] opened/closed` lines
  around the attempt. Tailor log `Tailor input dispatch guard installed` versus the warn `...call is unsupported...`.
  Test: with the MFO board never opened this session, does Tailor preview work? (H1 predicts yes.)

### H2. Not MFO: the cause is another mod in the user's list, or Tailor itself (MEDIUM, by elimination)
- For: nothing MFO or Harbinger does globally touches a non-follower's equip, the CursorMenu, Tailor's IMenu, the
  camera, or the swapchain path Tailor renders on. The report has no log tying it to MFO. "Followers and non-followers"
  removes every follower-scoped mechanism.
- Against: the user saw it go away (presumably) when MFO was disabled. That needs confirming, and if true it points
  back at H1/H4.
- Settles it: a bisect (MFO.dll alone removed; MFO.dll kept but APMF.dll removed) plus both logs.

### H3. WndProc subclass (the 2.8.1 bug) (LOW for this report)
- For: the user is on 2.8.1, which called the previous WNDPROC directly (`return func(...)`). 2.8.2 (`01c1cc3`, in tags
  v2.8.2 and v2.9.0) routes it through `CallWindowProcA`.
- Against: the 2.8.1 defect is a **crash** (AV at a `0xFFFFxxxx` handle) and needs a Unicode subclass BELOW MFO (e.g.
  Community Shaders). Tailor installs no WndProc at all, so it can't be that subclass. A user reporting "can't
  preview" rather than a CTD isn't hitting it. Side effect still open as MFO-B560: `SetWindowLongPtrA` makes the window
  ANSI, so non-ASCII WM_CHAR below MFO arrives as ANSI. At most that garbles non-ASCII typing in Tailor's text fields,
  it can't block the preview.

### H4. D3D11 / ImGui collisions (two Present hooks, shared ImGui globals) (VERY LOW)
- Against: Tailor hooks no Present. Both ImGui copies are static, so each DLL has its own `GImGui`. Tailor
  saves/restores the current context. MFO's Present returns straight to the original when neither the board nor the
  HUD is up. MFO's DX11 backend backs up and restores render state when it does draw. The two Win32 backends share an
  HWND, but neither registers a WndProc handler. The only shared effect is `::SetCursor` calls when one backend's
  cursor shape changes, and Tailor uses the engine's Scaleform cursor (`kUsesCursor`, `TailorMenu.cpp:9`).

### H5. Equip side (MFO equip authority / Harbinger ch.17 / BeastHeadSink) (VERY LOW for non-followers)
- Harbinger's sink passes unclaimed actors (`EquipSink.cpp:568-585`). BeastHeadSink only acts on teammates
  (`Sinks.cpp:102`). MFO's armor logic only runs on tracked followers. This could still produce a **separate**
  follower-only symptom: MFO or Harbinger re-dressing or denying ARMO on a managed follower, logged as Tailor's
  "Some outfit pieces could not be equipped". That is the coordinator's first theory, still plausible for followers
  only. Settles it: `[apmf][equip...]` deny lines and MFO `[armor-obs]` lines naming the previewed follower.

### H6. Install order / trampoline reservations (VERY LOW)
- Each DLL reserves its own trampoline (MFO `AllocTrampoline(64)`, `Board.cpp:1909`; Tailor 28, `main.cpp:209`).
  MFO patches before Tailor, which is the order Tailor's comment expects. Order would only matter if a THIRD mod
  wrote a non-E8 (jmp) at that site. Then MFO logs `holds opcode XX, not E8` and Tailor warns `unsupported`.

## Answers

1. **Most likely cause:** the source shows **no MFO hook collision that breaks Tailor while the MFO board is closed**.
   The one real overlap is the shared input-dispatch call site (H1), and it is chained correctly. It only starves
   Tailor's input while MFO's board is open or in its close grace. So the leading candidates are: (a) H1, if the user
   had MFO's board open or the two UIs are being used together; or (b) not MFO at all (H2). Treat MFO as unconfirmed
   until a bisect and the logs arrive. Separately, on managed followers only, an equip deny or re-dress (H5) can make
   preview pieces fail. That's a distinct follower-only bug, and it can't explain the non-followers.
2. **Does 2.8.2 already fix it?** **Unknown, probably no.** The only Board change in v2.8.1..v2.8.2 is the
   `CallWindowProcA` dispatch (`01c1cc3`). That fixes a crash with a Unicode subclasser (Community Shaders), and Tailor
   neither subclasses nor crashes. It's still the first thing to ask, because it's a free check.
   Housekeeping: the local `main` ref is stale (`0aeaffa`, 2026-10-13) and the current branch
   `feat/mfo-hide-unsupported-spells` predates v2.8.2 (its `Board.cpp:374` still reads `return func(...)`).
   `origin/main` (`c3018f1`) has the fix. A merge keeps it, but don't build a test DLL from this branch unrebased.
3. **Fix shape if it's ours (no code):**
   - H1: make MFO's consume the narrowest possible. Null the batch only while the board is actually DRAWN and the
     game window has focus, and never while another menu (`TailorMenu`, or any menu that isn't MFO's) is on top.
     Optionally close or refuse to open the board while a foreign cursor menu is open. Keep the null-the-head
     mechanism (it was right to restore it); only gate it better. Log a `[board] consume suppressed: menu X open`
     line so a field log proves it.
   - H5 (followers): don't punch a blanket hole in deny-completeness. Either Tailor becomes a ranked Harbinger source
     for the armor facet (the user ranks it in the Harbinger menu, which is user config), or MFO's claim scopes ARMO
     out (`SetEquipScope` owned/denied masks) while a Tailor preview is active, detected through Tailor's public mod API
     (`src/api/TailorInterface1.h`, `docs/api/TailorAPI.h`) and integrated by interface only, no GPL code copied.
   - H3: nothing new. Upgrade to 2.8.2. MFO-B560 (ANSI WM_CHAR) stays in the backlog.
4. **Ask the user:**
   - Their MFO version **and whether it still happens on 2.8.2**, and their Tailor and Harbinger (APMF) versions.
   - Their Skyrim runtime (1.6.1170 / 1.5.97 / 1.6.640 / 1.7.x). Off 1.6.1170/1.5.97, MFO uses the sink + ControlMap
     path instead of the trampoline.
   - Exactly what fails: the Tailor menu won't open / opens but mouse or keys are dead / preview camera doesn't
     start / "Select a loaded NPC to preview." / "Some outfit pieces could not be equipped" / a crash.
   - Does it go away with MFO.dll removed? With MFO kept but APMF.dll removed?
   - Did they open the MFO board (Field Orders) that session, and is it open when they use Tailor? Their MFO
     iBoardKey/iHudKey/focus key versus Tailor's hotkey.
   - Logs from the same session: `Documents/My Games/Skyrim Special Edition/SKSE/MFO.log`, `APMF.log` (Harbinger),
     Tailor's SKSE log, and any CrashLogger file if it crashed.
5. **In-game tests (Deck, Tuxborn), once Tailor is installed:**
   - T1: fresh load, never open the MFO board, Tailor preview on a non-follower NPC and on the player. Expected: works
     (H1/H2 baseline).
   - T2: open and close the MFO board, then preview right away and again after 2 s (close-grace window).
   - T3: MFO board open, then open Tailor: does Tailor get mouse/keys? (H1 predicts no.)
   - T4: preview on a managed follower, then grep MFO/APMF logs for equip denies and `[armor-obs]` on that FormID (H5).
   - T5: confirm in the logs that both input hooks installed (MFO `input trampoline installed`, Tailor `input dispatch
     guard installed`) and that neither reports a non-E8 site.
   - T6: type a non-ASCII character in a Tailor text field (MFO-B560 side effect only).
