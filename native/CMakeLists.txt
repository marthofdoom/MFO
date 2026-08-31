# MFO native plugin — structure copied from MEO/MAO, which copied it from the
# SkyrimDev/HelloWorld-using-CommonLibSSE-NG template. Doctrine: adapt working
# sources, never write a build system from memory.
#
# imgui carries the dx11-binding feature for the in-game Field Kit. The win32
# platform backend is NOT taken from vcpkg: imgui_impl_win32.cpp is vendored
# here (upstream v1.92.8, byte-exact — must match the vcpkg-pinned imgui) and
# compiled into this target with IMGUI_IMPL_WIN32_DISABLE_GAMEPAD, so the
# backend's own XInput poll is compiled OUT. The input hook in Board.cpp is the
# single gamepad source; the XInput poll went deaf on Steam Input kb<->pad mode
# flips and its per-frame HasGamepad rewrite killed gamepad nav (v1.0.59).
# Re-adding vcpkg's win32-binding feature would duplicate every
# ImGui_ImplWin32_* symbol at link — the loud failure is intentional.
# The gambit VOCABULARY is still codegen'd into a header (no parser for
# that) — but the load-order ITEM CATALOG (mfo_items.json, produced by the
# MFO.Synthesis patcher) is read at runtime, so nlohmann_json now ships, exactly
# as it does in MAO for mao_tiers.json. Categorizing items from real records
# beats guessing their type at runtime (Requiem-proof).
cmake_minimum_required(VERSION 3.21)

project(MFO VERSION 1.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    Serialization.cpp
    Forms.cpp
    Config.cpp
    Followers.cpp
    Rapport.cpp
    Diagnostics.cpp
    Board.cpp
    Probe.cpp
    ProgProbe.cpp
    Progression.cpp
    ProgAllocator.cpp
    Evaluator.cpp
    Actuation.cpp
    Actuation_Direct.cpp
    Scheduler.cpp
    Logistics.cpp
    Logistics_Cast.cpp
    Logistics_Economy.cpp
    Logistics_Loot.cpp
    Loadout.cpp
    Papyrus.cpp
    TradeBridge.cpp
    Targeting.cpp
    MainThread.cpp
    Packages.cpp
    Gait.cpp
    CasterConsent.cpp
    CombatStyle.cpp
    Sightline.cpp
    ItemCatalog.cpp
    MEOBridge.cpp
    imgui_impl_win32.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
# The vendored backend TU compiles standalone, exactly as vcpkg compiled it:
# no PCH (it must not inherit CommonLibSSE's headers/macros). The gamepad
# define only touches guarded regions of imgui_impl_win32.cpp, so applying it
# target-wide is safe and keeps the TU's build identical to a per-file define.
# /w: third-party code compiled under OUR flag set for the first time -- no
# warning may become an error we'd have to patch upstream code for.
set_source_files_properties(imgui_impl_win32.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/w>")
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# Emit a PDB even in Release so a field crash symbolizes to a source line. /Zi
# adds debug info; /DEBUG writes the .pdb; /OPT:REF,ICF undo the size cost the
# linker would otherwise skip once /DEBUG is on. The CI already uploads MFO.pdb
# beside the DLL -- it was just never being generated.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DEBUG /OPT:REF /OPT:ICF>)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 nlohmann_json::nlohmann_json)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
