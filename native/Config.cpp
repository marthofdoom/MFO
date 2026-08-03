#include "PCH.h"
#include "Config.h"
#include "Gait.h"

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
            else if (a_key == "fSharedRadius")      setF(g_sharedRadius,      0.0f, 1000000.0f);
            else if (a_key == "iBossLevelDelta")    setI(g_bossLevelDelta, 1, 1000);   // 0 would make every equal-level kill a boss
            else if (a_key == "iRapportRank2")      setI(g_rank2, 1, 100000000);
            else if (a_key == "iRapportRank3")      setI(g_rank3, 1, 100000000);
            else if (a_key == "iRapportRank4")      setI(g_rank4, 1, 100000000);
            else if (a_key == "iRapportRank5")      setI(g_rank5, 1, 100000000);
            else if (a_key == "bEnableLogging")     setB(g_enableLogging);
            else if (a_key == "bProfileRapport")    setB(g_profileRapport);
            else if (a_key == "iMenuStyle")         setI(g_menuStyle, 0, 3);
            else if (a_key == "bShowHud")           setB(g_showHud);
            else if (a_key == "bSeedTestData")      setB(g_seedTestData);
            else if (a_key == "bSeedEvaluatorRules") setB(g_seedEvaluatorRules);
            else if (a_key == "bDebugUnlockSlots")   setB(g_debugUnlockSlots);
            else if (a_key == "bProfileEvaluator")  setB(g_profileEvaluator);
            else if (a_key == "fSuppressWindow")    setF(g_suppressWindow, 0.0f, 60.0f);
            else if (a_key == "iCastSource")        setI(g_castSource, 0, 3);
            else if (a_key == "bEquipToCast")       setB(g_equipToCast);
            else if (a_key == "fAiCastGrace")       setF(g_aiCastGrace, 0.0f, 30.0f);
            else if (a_key == "fMagickaReserve")    setF(g_magickaReserve, 0.0f, 0.9f);
            else if (a_key == "fCastCooldown")      setF(g_castCooldown, 0.0f, 60.0f);
            else if (a_key == "bDriveCaster")       setB(g_driveCaster);
            else if (a_key == "bUsePackages")       setB(g_usePackages);
            else if (a_key == "bCasterHook")        setB(g_casterHook);
            else if (a_key == "iCasterMode")        setI(g_casterMode, 0, 1);
            else if (a_key == "bCommandCast")       setB(g_commandCast);
            else if (a_key == "bCommandTarget")     setB(g_commandTarget);
            else if (a_key == "iFocusKey")          setI(g_focusKey, 0, 255);
            else if (a_key == "fTwoHandedDebounce") setF(g_twoHandedDebounce, 0.0f, 60.0f);
            else if (a_key == "fSharedCombatGrace") setF(g_sharedCombatGrace, 0.0f, 300.0f);
            else if (a_key == "bLogistics")         setB(g_logistics);
            else if (a_key == "fFirstDibsDelay")    setF(g_firstDibsDelay,   0.0f, 600.0f);
            else if (a_key == "fQuickLootWaiver")   setF(g_quickLootWaiver,  0.0f, 600.0f);
            // Claim-and-Release loot priority
            else if (a_key == "fChanceRadius")      setF(g_chanceRadius,    64.0f, 8192.0f);
            else if (a_key == "fFairChance")        setF(g_fairChance,       0.0f, 120.0f);
            else if (a_key == "fAbandonDelay")      setF(g_abandonDelay,     0.0f, 600.0f);
            else if (a_key == "fDepartRadius")      setF(g_departRadius,    64.0f, 8192.0f);
            else if (a_key == "fPlayerBubble")      setF(g_playerBubble,     0.0f, 4096.0f);
            else if (a_key == "fLootRadius")        setF(g_lootRadius,     64.0f, 4096.0f);
            else if (a_key == "bLootTravel")        setB(g_lootTravel);
            else if (a_key == "bLootInPlayerHomes") setB(g_lootInPlayerHomes);
            else if (a_key == "bEconomy")           setB(g_economy);
            else if (a_key == "bAutoRetreat")       setB(g_autoRetreat);
            else if (a_key == "bRapportToasts")     setB(g_rapportToasts);
            else if (a_key == "fTravelRadius")       setF(g_travelRadius,   64.0f, 8192.0f);   // match the MCM slider max
            else if (a_key == "fBatchLinger")        setF(g_batchLinger,     0.0f, 15.0f);
            else if (a_key == "fExcursionMax")       setF(g_excursionMax,    5.0f, 300.0f);
            else if (a_key == "fNavmeshGate")        setF(g_navmeshGate,     0.0f, 2048.0f);
            else if (a_key == "iTravelGait")         setI(g_travelGait, 0, 3);   // 0=Walk 1=Jog 2=Run 3=FastWalk
            else if (a_key == "fLeashMin")          setF(g_leashMin,       64.0f, 8192.0f);
            else if (a_key == "fLeashMax")          setF(g_leashMax,       64.0f, 8192.0f);
            else if (a_key == "fChaseMin")          setF(g_chaseMin,       64.0f, 8192.0f);
            else if (a_key == "fChaseMax")          setF(g_chaseMax,       64.0f, 8192.0f);
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
            g_sharedRadius      = 3000.0f;
            g_bossLevelDelta    = 5;
            g_rank2 = 250;  g_rank3 = 1000;  g_rank4 = 2500;  g_rank5 = 5000;
            g_enableLogging  = true;
            g_profileRapport = false;
            g_showHud        = false;
            g_menuStyle      = 0;
            g_seedTestData   = false;
            g_seedEvaluatorRules = false;
            g_debugUnlockSlots   = false;
            g_profileEvaluator   = false;
            g_suppressWindow     = 1.5f;
            g_casterHook         = true;
            g_casterMode         = 1;
            g_usePackages        = false;
            g_castSource         = 3;
            g_equipToCast        = true;
            g_aiCastGrace        = 3.0f;
            g_magickaReserve     = 0.0f;
            g_castCooldown       = 4.0f;
            g_driveCaster        = false;
            g_commandCast        = false;
            g_commandTarget      = true;
            g_focusKey           = 0x2B;
            g_twoHandedDebounce  = 6.0f;
            g_sharedCombatGrace  = 15.0f;
            g_logistics          = true;
            g_firstDibsDelay     = 4.0f;
            g_quickLootWaiver    = 4.0f;
            g_chanceRadius       = 512.0f;
            g_fairChance         = 6.0f;
            g_abandonDelay       = 45.0f;
            g_departRadius       = 700.0f;
            g_playerBubble       = 256.0f;
            g_lootRadius         = 3000.0f;
            g_lootTravel         = true;
            g_lootInPlayerHomes  = false;
            g_economy            = false;
            g_autoRetreat        = false;
            g_rapportToasts      = true;
            g_travelRadius       = 4096.0f;
            g_batchLinger        = 4.0f;
            g_excursionMax       = 60.0f;
            g_navmeshGate        = 300.0f;
            g_travelGait         = 2;   // Run -- matches the shipped ESP byte
            g_leashMin           = 512.0f;
            g_leashMax           = 4000.0f;
            g_chaseMin           = 600.0f;
            g_chaseMax           = 3000.0f;
        }

    }

    // MCM MIGRATION SELF-HEAL (see Config.h). MCM Helper's live store is the MO2
    // overwrite copy of Data/MCM/Settings/MFO.ini; on an EXISTING save it lacks
    // any toggle added since -- the control reads -1 / won't bind, and a DOWNLOADED
    // update can't be hand-seeded. Ensure every MCM ModSetting key exists here
    // before MCM Helper reads, appending any missing one with its default. All MFO
    // ModSettings live under [General], so appending at EOF keeps them in-section.
    // Runs ONCE at load (kDataLoaded). If the store is absent (fresh install) MCM
    // Helper creates it complete, so there is nothing to do. Degrades to prior
    // behavior on ANY I/O error -- never worse than today.
    //
    // KEEP kMcmDefaults IN SYNC with out/MCM/Settings/MFO.ini -- it is the sixth
    // place a new toggle is wired (atomic, parse+reset, both inis, config.json).
    void EnsureMcmDefaults() {
        static constexpr std::pair<const char*, const char*> kMcmDefaults[] = {
            { "fRapportRate", "1.000000" },   { "fRapportKill", "1.000000" },
            { "fRapportBossMult", "5.000000" }, { "fRapportDragonMult", "10.000000" },
            { "iBossLevelDelta", "5" },        { "fSharedRadius", "3000.000000" },
            { "bAllowSummons", "0" },          { "bEquipToCast", "1" },
            { "bCasterHook", "1" },            { "bCommandTarget", "1" },
            { "fCastCooldown", "4.000000" },   { "fAiCastGrace", "3.000000" },
            { "fMagickaReserve", "0.000000" }, { "bLogistics", "1" },
            { "fFirstDibsDelay", "4.000000" }, { "fQuickLootWaiver", "4.000000" },
            { "fChanceRadius", "512.000000" }, { "fFairChance", "6.000000" },
            { "fAbandonDelay", "45.000000" },  { "fDepartRadius", "700.000000" },
            { "fPlayerBubble", "256.000000" }, { "fLootRadius", "3000.000000" },
            { "bLootTravel", "1" },            { "bLootInPlayerHomes", "0" },
            { "bEconomy", "0" },               { "bAutoRetreat", "0" },
            { "bRapportToasts", "1" },         { "iTravelGait", "2" },
            { "fTravelRadius", "4096.000000" },{ "fBatchLinger", "4.000000" },
            { "fExcursionMax", "60.000000" },  { "fNavmeshGate", "300.000000" },
            { "fLeashMin", "512.000000" },     { "fLeashMax", "4000.000000" },
            { "iMenuStyle", "0" },             { "bShowHud", "0" },
            { "bDebugUnlockSlots", "0" },
        };

        std::ifstream in(kMCMPath, std::ios::binary);
        if (!in) return;   // no store yet -> MCM Helper will create it complete
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string text = ss.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);   // strip MCM Helper's UTF-8 BOM before scanning

        // PRESENT = some line begins (after leading ws) with the exact key token
        // followed by ws/'='. Line-by-line so "bLoot..." never matches a prefix.
        auto present = [&text](const char* a_key) {
            const std::string key = a_key;
            std::stringstream ls(text);
            std::string line;
            while (std::getline(ls, line)) {
                const auto b = line.find_first_not_of(" \t\r");
                if (b == std::string::npos) continue;
                if (line.compare(b, key.size(), key) == 0) {
                    auto a = b + key.size();
                    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
                    if (a < line.size() && line[a] == '=') return true;
                }
            }
            return false;
        };

        std::string add;
        int n = 0;
        for (const auto& [k, v] : kMcmDefaults)
            if (!present(k)) { add += k; add += " = "; add += v; add += "\n"; ++n; }
        if (add.empty()) return;

        std::ofstream out(kMCMPath, std::ios::binary | std::ios::app);
        if (!out) return;
        if (!text.empty() && text.back() != '\n') out << "\n";   // don't glue onto the last line
        out << add;
        out.close();
        spdlog::info("[config] MCM self-heal: seeded {} missing setting(s) into the store", n);
    }

    void Read() {
        // RESET first, always. Without this an absent key keeps its last
        // in-memory value instead of reverting to default -- MAO §27.
        ResetToDefaults();
        ReadFile(kSeedPath);
        ReadFile(kMCMPath);    // MCM Helper's store wins

        spdlog::set_level(g_enableLogging ? spdlog::level::info : spdlog::level::warn);

        // Config -> engine-record derived state, refreshed by EVERY read so the
        // MCM-close re-read (Diagnostics' MenuSink) applies it live without each
        // caller remembering to (the g_showHud note above is the pattern this
        // avoids repeating). No-ops before Forms::Resolve; kDataLoaded calls it
        // again after. A single u8 store on MFO's own PACK record -- safe from
        // the task-queue thread Read() runs on at MCM close.
        Gait::Apply();
    }

}
