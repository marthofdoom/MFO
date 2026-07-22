#include "PCH.h"
#include "Config.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace MFO::Config {

    namespace {

        constexpr const char* kSeedPath = "Data/SKSE/Plugins/MFO.ini";
        constexpr const char* kMCMPath  = "Data/MCM/Settings/MFO.ini";

        // Skip unparseable values -- never apply 0.0 (INVARIANTS #38).
        // strtof's silent 0.0 zeroed features in MEO.
        bool ParseFloat(const std::string& a_s, float& a_out) {
            try {
                size_t idx = 0;
                const float v = std::stof(a_s, &idx);
                while (idx < a_s.size() && std::isspace(static_cast<unsigned char>(a_s[idx]))) ++idx;
                if (idx != a_s.size()) return false;
                a_out = v;
                return true;
            } catch (...) {
                return false;
            }
        }

        bool ParseInt(const std::string& a_s, int& a_out) {
            try {
                size_t idx = 0;
                const int v = std::stoi(a_s, &idx);
                while (idx < a_s.size() && std::isspace(static_cast<unsigned char>(a_s[idx]))) ++idx;
                if (idx != a_s.size()) return false;
                a_out = v;
                return true;
            } catch (...) {
                return false;
            }
        }

        std::string Trim(std::string s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        void Apply(const std::string& a_key, const std::string& a_val, const char* a_src) {
            float f = 0.0f;
            int   i = 0;

            auto setF = [&](std::atomic<float>& dst, float lo, float hi) {
                if (!ParseFloat(a_val, f)) {
                    spdlog::warn("[config] {}: unparseable value for {} ('{}') -- keeping default",
                                 a_src, a_key, a_val);
                    return;
                }
                // Clamp AT PARSE. A hostile or mistyped value must never become
                // an out-of-range one (INVARIANTS #38).
                dst.store(std::clamp(f, lo, hi));
            };
            auto setI = [&](std::atomic<int>& dst, int lo, int hi) {
                if (!ParseInt(a_val, i)) {
                    spdlog::warn("[config] {}: unparseable value for {} ('{}') -- keeping default",
                                 a_src, a_key, a_val);
                    return;
                }
                dst.store(std::clamp(i, lo, hi));
            };
            auto setB = [&](std::atomic<bool>& dst) {
                if (!ParseInt(a_val, i)) {
                    spdlog::warn("[config] {}: unparseable value for {} ('{}') -- keeping default",
                                 a_src, a_key, a_val);
                    return;
                }
                dst.store(i != 0);
            };

            if      (a_key == "bAllowSummons")      setB(g_allowSummons);
            else if (a_key == "fRapportRate")       setF(g_rapportRate,       0.0f, 100.0f);
            else if (a_key == "fRapportKill")       setF(g_rapportKill,       0.0f, 1000.0f);
            else if (a_key == "fRapportBossMult")   setF(g_rapportBossMult,   0.0f, 1000.0f);
            else if (a_key == "fRapportDragonMult") setF(g_rapportDragonMult, 0.0f, 1000.0f);
            else if (a_key == "fRapportSurvival")   setF(g_rapportSurvival,   0.0f, 1000.0f);
            else if (a_key == "fSharedRadius")      setF(g_sharedRadius,      0.0f, 1000000.0f);
            else if (a_key == "iBossLevelDelta")    setI(g_bossLevelDelta, 1, 1000);   // 0 would make every equal-level kill a boss
            else if (a_key == "iRapportRank2")      setI(g_rank2, 1, 100000000);
            else if (a_key == "iRapportRank3")      setI(g_rank3, 1, 100000000);
            else if (a_key == "iRapportRank4")      setI(g_rank4, 1, 100000000);
            else if (a_key == "iRapportRank5")      setI(g_rank5, 1, 100000000);
            else if (a_key == "bEnableLogging")     setB(g_enableLogging);
            else if (a_key == "bProfileRapport")    setB(g_profileRapport);
            else if (a_key == "bShowHud")           setB(g_showHud);
            else if (a_key == "bSeedTestData")      setB(g_seedTestData);
            else if (a_key == "bSeedEvaluatorRules") setB(g_seedEvaluatorRules);
            else if (a_key == "bProfileEvaluator")  setB(g_profileEvaluator);
            else if (a_key == "fSuppressWindow")    setF(g_suppressWindow, 0.0f, 60.0f);
            else if (a_key == "iCastSource")        setI(g_castSource, 0, 3);
            else if (a_key == "bEquipToCast")       setB(g_equipToCast);
            else if (a_key == "fAiCastGrace")       setF(g_aiCastGrace, 0.0f, 30.0f);
            else if (a_key == "bScreenNotify")      setB(g_screenNotify);
            else if (a_key == "bCommandCast")       setB(g_commandCast);
            else if (a_key == "bCommandTarget")     setB(g_commandTarget);
            else if (a_key == "iFocusKey")          setI(g_focusKey, 0, 255);
            else if (a_key == "fTwoHandedDebounce") setF(g_twoHandedDebounce, 0.0f, 60.0f);
            else if (a_key == "fSharedCombatGrace") setF(g_sharedCombatGrace, 0.0f, 300.0f);
            // Unknown keys are ignored in silence: MCM Helper writes keys we
            // may not know yet, and warning on them would cry wolf every load.
        }

        void ReadFile(const char* a_path) {
            std::ifstream in(a_path, std::ios::binary);
            if (!in) return;   // absent is normal, not an error

            std::string line;
            bool first = true;
            int applied = 0;
            while (std::getline(in, line)) {
                if (first) {
                    first = false;
                    // MCM Helper writes a UTF-8 BOM. Left in place it corrupts
                    // the first key name, silently (INVARIANTS #38).
                    if (line.size() >= 3 &&
                        static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF) {
                        line = line.substr(3);
                    }
                }
                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                Apply(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), a_path);
                ++applied;
            }
            spdlog::info("[config] read {} ({} key(s))", a_path, applied);
        }

        void ResetToDefaults() {
            g_allowSummons      = false;
            g_rapportRate       = 1.0f;
            g_rapportKill       = 1.0f;
            g_rapportBossMult   = 5.0f;
            g_rapportDragonMult = 10.0f;
            g_rapportSurvival   = 1.0f;
            g_sharedRadius      = 3000.0f;
            g_bossLevelDelta    = 5;
            g_rank2 = 250;  g_rank3 = 1000;  g_rank4 = 2500;  g_rank5 = 5000;
            g_enableLogging  = true;
            g_profileRapport = false;
            g_showHud        = true;
            g_seedTestData   = false;
            g_seedEvaluatorRules = false;
            g_profileEvaluator   = false;
            g_suppressWindow     = 1.5f;
            g_castSource         = 3;
            g_equipToCast        = false;
            g_aiCastGrace        = 3.0f;
            g_screenNotify       = false;
            g_commandCast        = false;
            g_commandTarget      = false;
            g_focusKey           = 0x2B;
            g_twoHandedDebounce  = 6.0f;
            g_sharedCombatGrace  = 15.0f;
        }

    }

    void Read() {
        // RESET first, always. Without this an absent key keeps its last
        // in-memory value instead of reverting to default -- MAO §27.
        ResetToDefaults();
        ReadFile(kSeedPath);
        ReadFile(kMCMPath);    // MCM Helper's store wins

        spdlog::set_level(g_enableLogging ? spdlog::level::info : spdlog::level::warn);
    }

}
