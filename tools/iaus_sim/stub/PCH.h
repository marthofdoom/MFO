#pragma once
// iaus_sim/stub/PCH.h -- HOST STAND-IN for native/PCH.h, on the include path BEFORE native/ for ONE purpose: to
// compile the real native/classes/ClassTable.cpp (the class table, plain constexpr data) into the simulator, so the
// sim's class cells are the game's own rows and never a hand copy. Only the names classes/Classes.h mentions exist.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RE { using FormID = std::uint32_t; class Actor; class TESCombatStyle; class BGSListForm; }
namespace SKSE { class SerializationInterface; }
