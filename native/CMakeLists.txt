# MFO native plugin — structure copied from MEO/MAO, which copied it from the
# SkyrimDev/HelloWorld-using-CommonLibSSE-NG template. Doctrine: adapt working
# sources, never write a build system from memory.
#
# imgui carries the dx11-binding/win32-binding features for the in-game Field
# Kit. There is deliberately still NO json dependency: the vocabulary is
# codegen'd into a header at commit time, so no runtime parser ships in the
# DLL (ARCHITECTURE.md §8.2).
cmake_minimum_required(VERSION 3.21)

project(MFO VERSION 0.8.6 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

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
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui d3d11)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
