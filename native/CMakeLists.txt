# MFO native plugin — structure copied from MEO/MAO, which copied it from the
# SkyrimDev/HelloWorld-using-CommonLibSSE-NG template. Doctrine: adapt working
# sources, never write a build system from memory.
#
# imgui carries the dx11-binding/win32-binding features for the in-game Field
# Kit. The gambit VOCABULARY is still codegen'd into a header (no parser for
# that) — but the load-order ITEM CATALOG (mfo_items.json, produced by the
# MFO.Synthesis patcher) is read at runtime, so nlohmann_json now ships, exactly
# as it does in MAO for mao_tiers.json. Categorizing items from real records
# beats guessing their type at runtime (Requiem-proof).
cmake_minimum_required(VERSION 3.21)

project(MFO VERSION 0.8.32 LANGUAGES CXX)

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
    Evaluator.cpp
    Actuation.cpp
    Scheduler.cpp
    Logistics.cpp
    Loadout.cpp
    Papyrus.cpp
    Targeting.cpp
    MainThread.cpp
    Packages.cpp
    Gait.cpp
    CasterConsent.cpp
    ItemCatalog.cpp
    MEOBridge.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 nlohmann_json::nlohmann_json)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
