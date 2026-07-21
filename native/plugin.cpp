#include "PCH.h"
#include "State.h"
#include "Serialization.h"

// MFO — marth's Follower Overhaul.
// P0 scope (DESIGN.md §10): the DLL loads, logs its version, and the co-save
// round-trips a hand-authored rule list across save / load / load-order
// change. NO gameplay, NO hooks, NO sinks yet.
//
// Read before editing: Docs/INVARIANTS.md, Docs/ARCHITECTURE.md.

namespace {

    void SetupLog() {
        auto path = SKSE::log::log_directory();
        if (!path) return;
        *path /= "MFO.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
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
        if (!st.gambits.empty()) return;   // already seeded or loaded

        st.rank = 1;
        st.rapport = 0;

        MFO::Gambit g1{};
        g1.conditionOpcode = "cond.self_hp_below";
        g1.conditionParam  = 0.5f;
        g1.actionOpcode    = "act.wait";
        st.gambits.push_back(g1);

        MFO::Gambit g2{};
        g2.conditionOpcode = "cond.always";
        g2.actionOpcode    = "act.cast_spell";
        g2.actionParamForm = 0x00012FCD;   // Healing (Skyrim.esm) — resolves everywhere
        st.gambits.push_back(g2);

        spdlog::info("[p0] seeded {} test gambit(s) on {:08X}", st.gambits.size(), player->GetFormID());
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // ARCHITECTURE.md §9: config -> forms -> sinks -> vocabulary.
            // P0 has none of those yet; this is the ordering anchor.
            spdlog::info("[startup] kDataLoaded");
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
            SeedTestData();
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

    // NOTE: the ImGui board (P3) installs its three trampoline hooks HERE,
    // before the renderer initializes, with SKSE::AllocTrampoline(256).
    // P0 installs no hooks at all — Tier A needs none (ARCHITECTURE.md §5).

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(MFO::kSerID);
    serialization->SetSaveCallback(MFO::SaveCallback);
    serialization->SetLoadCallback(MFO::LoadCallback);
    serialization->SetRevertCallback(MFO::RevertCallback);

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== MFO loaded (P0: co-save only, no gameplay) ===");
    return true;
}
