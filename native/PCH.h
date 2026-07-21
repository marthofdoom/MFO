#pragma once

// NOTE (P3, when the ImGui board lands): d3d11.h pulls in windows.h, which
// CommonLibSSE-NG never includes. WIN32_LEAN_AND_MEAN + NOMINMAX are already
// set in CMakePresets, but wingdi.h also #defines GetObject -> GetObjectW,
// which hijacks BGSDefaultObjectManager::GetObject<T>(). #undef GetObject
// AFTER the D3D includes. (ENGINE_NOTES.md §6.)

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

// REQUIRED, not stylistic. add_commonlibsse_plugin generates
// __MFOPlugin.cpp with "..."sv literals and force-includes this PCH; without
// this the build dies with C3688 "invalid literal suffix 'sv'" in a file we
// never wrote. MEO's PCH ends with the same line for the same reason.
using namespace std::literals;
