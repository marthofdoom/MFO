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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
