#include "PCH.h"
#include "State.h"
#include "Serialization.h"
#include "Forms.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "Diagnostics.h"

// MFO — marth's Follower Overhaul.
// Scope as of M3 (DESIGN.md §10, ROADMAP.md): the DLL loads, resolves its
// forms, grants the Field Orders power, detects followers, accrues Rapport,
// and round-trips all of it through the co-save.
//
// NO code hooks -- Tier A needs none (ARCHITECTURE.md §5). Event sinks only:
// death + combat (Rapport) and spell-cast (the diagnostic dump).
//
// Read before editing: Docs/INVARIANTS.md, Docs/ARCHITECTURE.md.

namespace {

    // Log to the GAME-ROOT-RELATIVE path, not SKSE::log::log_directory().
    //
    // Why: under MO2 + USVFS, a game-root-relative write is redirected into
    // the profile's Overwrite folder, which is where every other SKSE plugin's
    // log in this setup actually lands (ActorLimitFix.log, BugFixesSSE.log,
    // ScrambledBugs.log, ...). log_directory() resolves instead to the Proton
    // prefix's My Games\...\SKSE\ — a different filesystem entirely, which on
    // this machine is a umu prefix (~/Games/umu/489830/...) that holds only
    // skse64's own logs. A log nobody can find alongside the others is a log
    // that does not get read.
    //
    // MRO documents the same trick. Fall back to log_directory() if the
    // redirect target is not writable (e.g. running outside MO2).
    void SetupLog() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;

        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Data/SKSE/Plugins/MFO.log", true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                        (*dir / "MFO.log").string(), true);
                } catch (const spdlog::spdlog_ex&) {
                    return;   // no log is survivable; a crash here is not
                }
            } else {
                return;
            }
        }

        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);   // flush every line: a CTD must not eat the last one
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    // P0 test seam. INVARIANTS.md #37 in spirit: a testing relaxation is ONE
    // compile-time constant with its flip-back condition stated, never
    // scattered `// TODO re-enable` checks (MAO §37).
    //
    // FLIP BACK TO 0 once the board (P3) can author rules. Until then this is
    // the only way to get a rule list into the co-save to prove round-trip.
    constexpr bool kP0SeedTestData = true;

    void SeedTestData() {
        if constexpr (!kP0SeedTestData) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Deliberately keyed on the player so P0 needs no follower present.
        // Two rules, one with a form param and one without, so the load path
        // exercises both ResolveFormID branches.
        auto& st = MFO::g_followers[player->GetFormID()];
        if (!st.combat().empty() || !st.logistics().empty()) return;   // seeded or loaded

        st.rank = 1;
        st.rapport = 0;

        // Combat table: one rule with no form param, one with, so the load
        // path exercises both ResolveFormID branches.
        MFO::Gambit g1{};
        g1.conditionOpcode = "cond.self_hp_below";
        g1.conditionParam  = 0.5f;
        g1.actionOpcode    = "act.wait";
        st.combat().push_back(g1);

        MFO::Gambit g2{};
        g2.conditionOpcode = "cond.always";
        g2.actionOpcode    = "act.cast_spell";
        g2.actionParamForm = 0x00012FCD;   // Healing (Skyrim.esm) — resolves everywhere
        st.combat().push_back(g2);

        // Logistics table (DESIGN.md 4.8) -- proves both tables round-trip
        // independently, which is the point of P0.
        MFO::Gambit g3{};
        g3.conditionOpcode = "cond.potions_below";
        g3.conditionParam  = 3.0f;
        g3.actionOpcode    = "act.loot_potion_health";
        st.logistics().push_back(g3);

        spdlog::info("[p0] seeded {} combat + {} logistics gambit(s) on {:08X}",
                     st.combat().size(), st.logistics().size(), player->GetFormID());
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // ARCHITECTURE.md §9 order: config -> forms -> sinks -> vocabulary.
            // Sinks must come AFTER form resolution or they fire against
            // unresolved forms.
            spdlog::info("[startup] kDataLoaded");
            MFO::Config::Read();            // config first -- everything else reads it
            MFO::Forms::Resolve();          // then forms
            MFO::Followers::ResolveQuirks();
            MFO::Rapport::RegisterSinks();  // sinks LAST, or they fire against unresolved forms
            MFO::Diagnostics::Install();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            // MUST run AFTER the co-save has loaded, or ledgers look empty and
            // reconcile revokes nothing (ARCHITECTURE.md §9).
            //
            // Also the anchor for re-asserting GlobalVariables: their values
            // are SAVE-PERSISTED, so a kDataLoaded write is overwritten when a
            // save loads (INVARIANTS.md, MRO's DR-handshake incident).
            spdlog::info("[startup] {} — {} follower record(s) live",
                         a_msg->type == SKSE::MessagingInterface::kNewGame ? "kNewGame" : "kPostLoadGame",
                         MFO::g_followers.size());
            MFO::Forms::EnsurePlayerSetup();
            MFO::Followers::Refresh();
            SeedTestData();
            MFO::Diagnostics::StartPump();
            MFO::Diagnostics::DumpReport("load");
            break;

        default:
            break;
        }
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

    const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
    const auto  ver    = plugin->GetVersion();

    // A STALE BINARY VOIDS EVERY TEST (INVARIANTS.md #44). This header is the
    // mandatory first check before believing any in-game result — MEO was
    // bitten twice. Keep it as the first line the log ever prints.
    spdlog::info("=== MFO {}.{}.{} loading — game {} ===",
                 ver.major(), ver.minor(), ver.patch(),
                 REL::Module::get().version().string());

    // NOTE: the ImGui board (M7) installs its three trampoline hooks HERE,
    // before the renderer initializes, with SKSE::AllocTrampoline(256).
    // Nothing installs a code hook today — Tier A needs none.

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(MFO::kSerID);
    serialization->SetSaveCallback(MFO::SaveCallback);
    serialization->SetLoadCallback(MFO::LoadCallback);
    serialization->SetRevertCallback(MFO::RevertCallback);

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== MFO loaded (M3: detection + rapport, no gameplay actions) ===");
    return true;
}
