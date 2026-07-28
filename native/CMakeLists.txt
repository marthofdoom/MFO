# MFO native plugin — structure copied from MEO/MAO, which copied it from the
# SkyrimDev/HelloWorld-using-CommonLibSSE-NG template. Doctrine: adapt working
# sources, never write a build system from memory.
#
# imgui carries the dx11-binding/win32-binding features for the in-game Field
# Kit. nlohmann-json arrived with the optional Leveling integration (Leveling.cpp):
# it reads/writes another mod's per-follower JSON state, which needs a real,
# format-preserving parser (the gambit vocabulary is still codegen'd, not parsed).
cmake_minimum_required(VERSION 3.21)

project(MFO VERSION 0.6.0 LANGUAGES CXX)

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
    Packages.cpp
    CasterConsent.cpp
    Leveling.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11 nlohmann_json::nlohmann_json)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
