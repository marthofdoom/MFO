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
            // Unsigned/hex parse (base 0 so a 0x.. FormID string works). For the
            // merchant-perk FormID key; keeps the default on a bad value.
            auto setU = [&](std::atomic<std::uint32_t>& dst) {
                try {
                    dst.store(static_cast<std::uint32_t>(std::stoul(a_val, nullptr, 0)));
                } catch (...) {
                    spdlog::warn("[config] {}: unparseable value for {} ('{}') -- keeping default",
                                 a_src, a_key, a_val);
                }
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
            else if (a_key == "fDotRecastBurstRatio") setF(g_dotRecastBurstRatio, 0.0f, 10.0f);
            else if (a_key == "fBeneficialRecastFrac")   setF(g_beneficialRecastFrac, 0.1f, 2.0f);
            else if (a_key == "fBeneficialRecastJitter") setF(g_beneficialRecastJitter, 0.0f, 0.9f);
            else if (a_key == "bDriveCaster")       setB(g_driveCaster);
            else if (a_key == "bProbeCastStyle")    setB(g_probeCastStyle);   // P1 probe, dev-only
            else if (a_key == "bCastSelf")          setB(g_castSelf);         // forced self-cast gate, dev-only
            else if (a_key == "bProgProbe")         setB(g_progProbe);        // progression sinker probe, dev-only
            else if (a_key == "iProgProbeKey")      setI(g_progProbeKey, 0, 255);
            else if (a_key == "bProgCatalogDump")   setB(g_progCatalogDump);  // catalog census dump, dev-only
            else if (a_key == "bProgHarness")       setB(g_progHarness);      // allocator dev harness, dev-only
            else if (a_key == "iProgHarnessKey")    setI(g_progHarnessKey, 0, 255);
            else if (a_key == "bSharedGrowth")      setB(g_sharedGrowth);     // progression: benched at half rate
            else if (a_key == "bWeaponStyleControl") setB(g_weaponStyleControl); // default ON, debug kill-switch
            else if (a_key == "bUsePackages")       setB(g_usePackages);
            else if (a_key == "bForceCastOnMiss")   setB(g_forceCastOnMiss);
            else if (a_key == "bCasterHook")        setB(g_casterHook);
            else if (a_key == "iCasterMode")        setI(g_casterMode, 0, 1);
            else if (a_key == "iCastControl")       setI(g_castControl, 0, 4);
            else if (a_key == "bFriendlyFireHold")  setB(g_friendlyFireHold);
            else if (a_key == "bQuashAllyCombat")    setB(g_quashAllyCombat);   // #63 default ON
            else if (a_key == "iOverlayX")          setI(g_overlayX, 0, 2560);
            else if (a_key == "iOverlayY")          setI(g_overlayY, 0, 1440);
            else if (a_key == "bDollsMode")         setB(g_dollsMode);
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
            else if (a_key == "bEconomyBuyGear")    setB(g_economyBuyGear);
            else if (a_key == "bEconomyBuyTomes")   setB(g_economyBuyTomes);
            else if (a_key == "bHmsRedistribute")   setB(g_hmsRedistribute);
            else if (a_key == "fHmsSkewMax") {
                // §HMS: the MCM slider stores a PERCENT (0-100); the atomic holds
                // the FRACTION. Parse + clamp + scale /100 here (not setF, which
                // would store the raw percent). A bad value keeps the default.
                try {
                    float pct = std::stof(a_val);
                    pct = std::clamp(pct, 0.0f, 100.0f);
                    g_hmsSkewMaxFrac.store(pct / 100.0f);
                } catch (...) {
                    spdlog::warn("[config] {}: unparseable value for {} ('{}') -- keeping default",
                                 a_src, a_key, a_val);
                }
            }
            else if (a_key == "bSpeechPricing")     setB(g_speechPricing);
            else if (a_key == "bMerchantPerkBypass") setB(g_merchantPerkBypass);
            else if (a_key == "xMerchantPerkID")    setU(g_merchantPerkID);
            else if (a_key == "bMageWearRobes")     setB(g_mageWearRobes);
            else if (a_key == "bMageApparelStrictSchool") setB(g_mageApparelStrictSchool);
            else if (a_key == "bAutoRetreat")       setB(g_autoRetreat);
            else if (a_key == "bMagicLoadout")      setB(g_magicLoadout);
            else if (a_key == "bMageDaggersOnly")   setB(g_mageDaggersOnly);
            else if (a_key == "bBeastHeadFix")       setB(g_beastHeadFix);   // #62 default ON, debug kill-switch
            else if (a_key == "bRapportToasts")     setB(g_rapportToasts);
            else if (a_key == "iMinPotionMag")      setI(g_minPotionMag, 0, 500);   // 0 = auto floor
            else if (a_key == "fTravelRadius")       setF(g_travelRadius,   64.0f, 8192.0f);   // match the MCM slider max
            else if (a_key == "fLooseAcquireDist")   setF(g_looseAcquireDist, 64.0f, 2048.0f);   // loose gold/ammo pickup reach
            else if (a_key == "fBatchLinger")        setF(g_batchLinger,     0.0f, 15.0f);
            else if (a_key == "fExcursionMax")       setF(g_excursionMax,    5.0f, 300.0f);
            else if (a_key == "fNavmeshGate")        setF(g_navmeshGate,     0.0f, 2048.0f);
            else if (a_key == "iTravelGait")         setI(g_travelGait, 0, 3);   // 0=Walk 1=Jog 2=Run 3=FastWalk
            else if (a_key == "fLeashMin")          setF(g_leashMin,       64.0f, 8192.0f);
            else if (a_key == "fLeashMax")          setF(g_leashMax,       64.0f, 8192.0f);
            else if (a_key == "fChaseMin")          setF(g_chaseMin,       64.0f, 8192.0f);
            else if (a_key == "fChaseMax")          setF(g_chaseMax,       64.0f, 8192.0f);
            else if (a_key == "fMeleeReach")        setF(g_meleeReach,     32.0f, 1024.0f);
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
                // Strip an INLINE comment from the value ("512.0  ; note" -> "512.0").
                // The seed SKSE/Plugins/MFO.ini annotates its values this way, and
                // ParseFloat/ParseInt reject any trailing non-space, so without this
                // every annotated key warned and silently fell back to its default.
                std::string val = Trim(line.substr(eq + 1));
                if (const auto c = val.find_first_of(";#"); c != std::string::npos)
                    val = Trim(val.substr(0, c));
                Apply(Trim(line.substr(0, eq)), val, a_path);
                ++applied;
            }
            spdlog::info("[config] read {} ({} key(s))", a_path, applied);
        }

        void ResetToDefaults() {
            g_allowSummons      = false;
            g_rapportRate       = 2.0f;   // default doubled (marth)
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
            g_usePackages        = true;    // v1.0.27: the forced-cast delivery route
            g_forceCastOnMiss    = true;
            g_castSource         = 3;
            g_equipToCast        = true;
            g_aiCastGrace        = 3.0f;
            g_magickaReserve     = 0.0f;
            g_castCooldown       = 4.0f;
            g_dotRecastBurstRatio = 1.0f;
            g_beneficialRecastFrac   = 0.85f;   // fix #3/#6: recast a light/buff at ~85% of its life
            g_beneficialRecastJitter = 0.20f;   //           +/-20% jitter so the beat is not robotic
            g_driveCaster        = false;
            g_probeCastStyle     = false;   // P1 probe -- OFF everywhere by default
            g_castSelf           = false;   // forced self-cast route -- OFF (gated) until deck-confirmed
            g_progProbe          = false;   // progression sinker probe -- OFF everywhere by default
            g_progProbeKey       = 0x27;    // DIK semicolon, unbound in vanilla
            g_progCatalogDump    = false;   // progression catalog dump -- OFF everywhere by default
            g_progHarness        = false;   // allocator dev harness -- OFF everywhere by default
            g_progHarnessKey     = 0x28;    // DIK apostrophe, unbound in vanilla
            g_sharedGrowth       = true;    // progression: benched enrollees at half rate (§15)
            g_weaponStyleControl = true;    // v1.0.33: standard feature -- ON by default
            g_castControl        = 2;       // mage update: cast-control slider -- center (ignore heals)
            g_friendlyFireHold   = true;    // v1.0.35: hold offensive casts that would hit a teammate
            g_overlayX           = 12;      // #56 combat-overlay margins (px from top-right)
            g_overlayY           = 12;
            g_dollsMode          = false;   // #61 fashionrim -- OFF by default
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
            g_economy            = true;
            g_economyBuyGear     = true;    // #21 gear-buy sub-toggle -- ON, gated under bEconomy
            g_economyBuyTomes    = true;    // #21 tome-buy sub-toggle -- ON, gated under bEconomy
            g_hmsRedistribute    = true;    // §HMS class-redistribution master switch -- ON
            g_hmsSkewMaxFrac     = 0.20f;   // §HMS off-class skew ceiling -- 20% (INI stores percent)
            g_speechPricing      = true;    // #21 sell price follows the speech-scaled vanilla barter curve
            g_merchantPerkBypass = true;    // #21 merchant-perk holder sells outside the vendor filter
            g_merchantPerkID     = 0x00058F7A;  // vanilla Merchant / Ordinator Salesman (Skyrim.esm)
            g_mageWearRobes      = true;    // #21 mage clothing/jewelry dress-up -- ON (loot + buy)
            g_mageApparelStrictSchool = false;  // #21 strict top-2-school apparel filter -- OFF (value-driven default)
            g_autoRetreat        = true;
            g_magicLoadout       = true;
            g_mageDaggersOnly    = true;
            g_rapportToasts      = true;
            g_minPotionMag       = 0;
            g_travelRadius       = 4096.0f;
            g_looseAcquireDist   = 300.0f;
            g_batchLinger        = 4.0f;
            g_excursionMax       = 60.0f;
            g_navmeshGate        = 300.0f;
            g_travelGait         = 2;   // Run -- matches the shipped ESP byte
            g_leashMin           = 512.0f;
            g_leashMax           = 4000.0f;
            g_chaseMin           = 600.0f;
            g_chaseMax           = 3000.0f;
            g_meleeReach         = 200.0f;
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
            { "fRapportRate", "2.000000" },   { "fRapportKill", "1.000000" },
            { "fRapportBossMult", "5.000000" }, { "fRapportDragonMult", "10.000000" },
            { "iBossLevelDelta", "5" },        { "fSharedRadius", "3000.000000" },
            { "bEquipToCast", "1" },
            { "bCasterHook", "1" },            { "bCommandTarget", "1" },
            { "bForceCastOnMiss", "1" },       { "iCastControl", "2" },
            { "iOverlayX", "12" },             { "iOverlayY", "12" },
            { "bDollsMode", "0" },
            { "fCastCooldown", "4.000000" },   { "fAiCastGrace", "3.000000" },
            { "fDotRecastBurstRatio", "1.000000" },
            { "fBeneficialRecastFrac", "0.850000" },
            { "fBeneficialRecastJitter", "0.200000" },
            { "fMagickaReserve", "0.000000" }, { "bLogistics", "1" },
            { "fFirstDibsDelay", "4.000000" }, { "fQuickLootWaiver", "4.000000" },
            { "fChanceRadius", "512.000000" }, { "fFairChance", "6.000000" },
            { "fAbandonDelay", "45.000000" },  { "fDepartRadius", "700.000000" },
            { "fPlayerBubble", "256.000000" }, { "fLootRadius", "3000.000000" },
            { "bLootTravel", "1" },            { "bLootInPlayerHomes", "0" },
            { "bEconomy", "1" },               { "bAutoRetreat", "1" },
            { "bEconomyBuyGear", "1" },        { "bEconomyBuyTomes", "1" },
            { "bHmsRedistribute", "1" },       { "fHmsSkewMax", "20.000000" },
            { "bMageWearRobes", "1" },         { "bMageApparelStrictSchool", "0" },
            { "bMagicLoadout", "1" },          { "bMageDaggersOnly", "1" },
            { "bBeastHeadFix", "1" },
            { "bRapportToasts", "1" },         { "iTravelGait", "2" },
            { "iMinPotionMag", "0" },
            { "fTravelRadius", "4096.000000" },{ "fBatchLinger", "4.000000" },
            { "fLooseAcquireDist", "300.000000" },
            { "fExcursionMax", "60.000000" },  { "fNavmeshGate", "300.000000" },
            { "fLeashMin", "512.000000" },     { "fLeashMax", "4000.000000" },
            { "fMeleeReach", "200.000000" },
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
