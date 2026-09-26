// logistics/TeleportCompat.cpp -- follower-teleport mod detection + the loot-leash
// clamp and teleport recognition. See TeleportCompat.h for the contract and the
// primary-source facts about the detected mod.
#include "PCH.h"
#include "TeleportCompat.h"
#include "Confidence.h"   // the confidence leash (core tenet) -- the value being capped

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// <windows.h> is banned outside Board.cpp (it #defines GetObject -- ENGINE_NOTES
// §9). Same direct declaration Targeting.cpp uses for its SmartNPCTargetSelector
// check: it drags in nothing and cannot drift with the library.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);

namespace MFO::Logistics::TeleportCompat {

    namespace {

        // Selection sits this far under the teleport distance; the mid-trip release
        // sits kReleaseMargin under it. The 200 u band between them is the clamped
        // leash's hysteresis (the unclamped x1.15 plays the same role), and the
        // release margin is what one ~1 s logistics tick of running covers less the
        // arrival reach, so MFO ends the leg before the teleporter's next check.
        constexpr float kSelectMargin  = 350.0f;
        constexpr float kReleaseMargin = 150.0f;
        constexpr float kLeashFloor    = 64.0f;    // Config's own fLeashMin floor
        constexpr float kHysteresis    = 1.15f;    // the excursion driver's leash margin

        // Teleport recognition thresholds (LooksTeleported). A follower's fastest
        // gait (sprint) is well under the speed floor, so a leg-sized jump at this
        // rate is not a walk; "landed near the player after being far" names the
        // teleporters' shape (AFT lands fBehindOffset = 450 behind him).
        constexpr float kJumpMin      = 800.0f;    // u moved between two observations
        constexpr float kJumpSpeedMin = 900.0f;    // u/s implied by that move
        constexpr float kClosedMin    = 600.0f;    // u closer to the player than before
        constexpr float kLandNearMin  = 1000.0f;   // u from the player after the jump

        struct Detected {
            bool        present         = false;
            const char* name            = "none detected";
            float       drawDistance    = 2000.0f;   // AFT defaults (DLL strings + shipped INI)
            float       drawnDistance   = 1500.0f;
            float       behindOffset    = 450.0f;
            bool        noTeleportHorse = false;
            bool        onlyCombatStart = false;
        };

        Detected         g_det;
        std::once_flag   g_detOnce;
        std::atomic<int> g_lastClamp{ -1 };   // -1 unknown, 0 off, 1 on -- transition log

        std::string Trim(std::string s) {
            auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && sp(static_cast<unsigned char>(s.back()))) s.pop_back();
            std::size_t i = 0;
            while (i < s.size() && sp(static_cast<unsigned char>(s[i]))) ++i;
            return s.substr(i);
        }

        bool ParseBool(const std::string& v, bool a_fallback) {
            std::string l;
            for (char c : v) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (l == "true" || l == "1")  return true;
            if (l == "false" || l == "0") return false;
            return a_fallback;
        }

        // AFT's own INI, [Teleport] section only. Absent = the DLL's defaults (which
        // is what the DLL itself runs with). A malformed value keeps the default and
        // says so -- never a silent guess.
        void ReadAftIni(Detected& d) {
            constexpr const char* kPath = "Data/SKSE/Plugins/AutomaticFollowerTeleporter.ini";
            std::ifstream in(kPath, std::ios::binary);
            if (!in) {
                spdlog::warn("[teleport-compat] {} not readable -- using AFT's built-in defaults "
                             "(fDrawDistance {:.0f}, fDrawnDistance {:.0f})",
                             kPath, d.drawDistance, d.drawnDistance);
                return;
            }
            std::string line, section;
            bool first = true;
            int  read  = 0;
            while (std::getline(in, line)) {
                if (first) {
                    first = false;
                    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF)
                        line = line.substr(3);   // UTF-8 BOM (the shipped INI has one)
                }
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                if (line[0] == '[') { section = line; continue; }
                if (section != "[Teleport]") continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                const std::string key = Trim(line.substr(0, eq));
                std::string       val = Trim(line.substr(eq + 1));
                if (const auto c = val.find_first_of(";#"); c != std::string::npos) val = Trim(val.substr(0, c));
                auto num = [&](float& a_out) {
                    char* end = nullptr;
                    const float f = std::strtof(val.c_str(), &end);
                    if (end == val.c_str() || !(f > 0.0f)) {
                        spdlog::warn("[teleport-compat] AFT {} = '{}' unparsable -- keeping {:.0f}",
                                     key, val, a_out);
                        return;
                    }
                    a_out = f;
                    ++read;
                };
                if      (key == "fDrawDistance")                   num(d.drawDistance);
                else if (key == "fDrawnDistance")                  num(d.drawnDistance);
                else if (key == "fBehindOffset")                   num(d.behindOffset);
                else if (key == "bNoTeleportOnHorse")              { d.noTeleportHorse = ParseBool(val, d.noTeleportHorse); ++read; }
                else if (key == "bOnlyAllowTeleportOnCombatStart") { d.onlyCombatStart = ParseBool(val, d.onlyCombatStart); ++read; }
            }
            spdlog::info("[teleport-compat] read {} ({} key(s))", kPath, read);
        }

        void Detect() {
            namespace fs = std::filesystem;
            std::error_code ec;

            // Automatic Follower Teleporter NG.
            constexpr const char* kAftDll = "AutomaticFollowerTeleporter.dll";
            if (::GetModuleHandleA(kAftDll)) {
                g_det.present = true;
                g_det.name    = "Automatic Follower Teleporter NG";
                ReadAftIni(g_det);
                const float d = std::min(g_det.drawDistance, g_det.drawnDistance);
                spdlog::warn("[teleport-compat] {} IS LOADED -- it teleports followers back to the player "
                             "while his weapon is DRAWN (fDrawDistance {:.0f} on the draw, fDrawnDistance "
                             "{:.0f} while drawn, landing {:.0f} behind him; bNoTeleportOnHorse={}, "
                             "bOnlyAllowTeleportOnCombatStart={}). MFO does not fight it: {}",
                             g_det.name, g_det.drawDistance, g_det.drawnDistance, g_det.behindOffset,
                             g_det.noTeleportHorse, g_det.onlyCombatStart,
                             g_det.onlyCombatStart
                                 ? "it only acts at combat start, when MFO's player-combat interrupt has "
                                   "already ended every loot trip -- NO leash clamp needed."
                                 : std::format("while drawn, the loot leash is capped at {:.0f} (select) / "
                                               "{:.0f} (release), under its {:.0f}.",
                                               std::max(kLeashFloor, d - kSelectMargin),
                                               std::max(kLeashFloor, d - kReleaseMargin), d).c_str());
            } else if (fs::exists("Data/SKSE/Plugins/AutomaticFollowerTeleporter.dll", ec)) {
                spdlog::warn("[teleport-compat] AutomaticFollowerTeleporter.dll is INSTALLED but NOT LOADED "
                             "(SKSE refused it?) -- no leash clamp.");
            }

            // Simple Follower Framework: checked, no teleport (TeleportCompat.h).
            if (::GetModuleHandleA("SimpleFollowerFramework.dll"))
                spdlog::info("[teleport-compat] SimpleFollowerFramework.dll is loaded -- it has no follower "
                             "teleport (checked: DLL strings, INI keys, shipped .psc); nothing to adjust.");

            if (!g_det.present)
                spdlog::info("[teleport-compat] no known follower-teleport mod loaded -- loot leash unclamped.");
        }

        const Detected& Det() {
            std::call_once(g_detOnce, Detect);
            return g_det;
        }

        float TeleportDistance(const Detected& d) { return std::min(d.drawDistance, d.drawnDistance); }
    }

    bool ConditionHolds() {
        const auto& d = Det();
        if (!d.present || d.onlyCombatStart) return false;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return false;
        const auto* st = pc->AsActorState();
        if (!st || !st->IsWeaponDrawn()) return false;
        if (d.noTeleportHorse && pc->IsOnMount()) return false;
        return true;
    }

    LeashView View(RE::Actor* a_follower) {
        LeashView v;
        const float base = Confidence::LeashRadius(a_follower);
        v.leash   = base;
        v.release = base * kHysteresis;
        const bool on = ConditionHolds();
        if (on) {
            const float d = TeleportDistance(Det());
            v.clamped    = true;
            v.teleportAt = d;
            v.leash      = std::min(base, std::max(kLeashFloor, d - kSelectMargin));
            v.release    = std::min(v.leash * kHysteresis, std::max(kLeashFloor, d - kReleaseMargin));
        }
        // One line per transition (the worker is the only caller; the exchange keeps
        // it one line even if that ever changes).
        if (Det().present) {
            const int now = on ? 1 : 0;
            if (g_lastClamp.exchange(now) != now) {
                if (on)
                    spdlog::info("[teleport-compat] player weapon DRAWN ({} active): loot leash capped at "
                                 "{:.0f} select / {:.0f} release (teleports past {:.0f})",
                                 Det().name, v.leash, v.release, v.teleportAt);
                else
                    spdlog::info("[teleport-compat] {} idle (weapon sheathed / its condition off): loot "
                                 "leash back to the confidence leash", Det().name);
            }
        }
        return v;
    }

    bool LooksTeleported(const RE::NiPoint3& a_prevPos, float a_prevPlayerDist, float a_dtSec,
                         const RE::NiPoint3& a_curPos, float a_curPlayerDist) {
        if (!(a_dtSec > 0.05f)) return false;
        const float jump = a_prevPos.GetDistance(a_curPos);
        if (jump < kJumpMin || jump / a_dtSec < kJumpSpeedMin) return false;
        const float landNear = std::max(kLandNearMin, Det().behindOffset + 550.0f);
        return a_curPlayerDist <= landNear && a_prevPlayerDist - a_curPlayerDist >= kClosedMin;
    }

    const char* DetectedName() { return Det().name; }
}
