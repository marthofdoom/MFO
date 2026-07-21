# MFO native plugin — structure copied from MEO/MAO, which copied it from the
# SkyrimDev/HelloWorld-using-CommonLibSSE-NG template. Doctrine: adapt working
# sources, never write a build system from memory.
#
# P0 deliberately has NO imgui / nlohmann_json dependency. The board (P3) adds
# imgui with the dx11-binding/win32-binding features; the vocabulary is
# codegen'd into a header at commit time, so there is no runtime JSON parser
# in the DLL (ARCHITECTURE.md §8.2).
cmake_minimum_required(VERSION 3.21)

project(MFO VERSION 0.0.1 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    Serialization.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
