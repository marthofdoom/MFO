#include "PCH.h"
#include "State.h"
#include "Serialization.h"
#include "Forms.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "Diagnostics.h"
#include "Board.h"
#include "Probe.h"
#include "Vocabulary.h"
#include "Loadout.h"
#include "Targeting.h"
#include "CasterConsent.h"
#include "Packages.h"
#include "Logistics.h"

// MFO — marth's Follower Overhaul.
// Scope as of M3 (DESIGN.md §10, ROADMAP.md): the DLL loads, resolves its
// forms, grants the Field Orders power, detects followers, accrues Rapport,
// and round-trips all of it through the co-save.
//
// Hooks: THREE, all for the Field Kit overlay (Board.cpp) -- D3DInit,
// DXGIPresent, InputDispatch. No GAMEPLAY hooks; Tier A actuation needs none
// (ARCHITECTURE.md §5). Event sinks: death + combat (Rapport), spell-cast
// (the Field Kit opener).
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

    // Test seam, now `bSeedTestData` in MFO.ini and DEFAULT OFF.
    //
    // NOTHING MFO HOLDS REACHES A SAVE UNLESS YOU SAVE. The co-save callbacks
    // only fire on save/load, so with this off and no saving, MFO is purely
    // observational -- it reads state and draws it, and leaves nothing behind.
    void SeedTestData() {
        if (!MFO::Config::g_seedTestData.load()) return;

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
        g1.conditionOpcode = MFO::Vocab::kCondSelfHpBelow;
        g1.conditionParam  = 0.5f;
        g1.actionOpcode    = "act.wait";
        st.combat().push_back(g1);

        MFO::Gambit g2{};
        g2.conditionOpcode = "cond.always";
        g2.actionOpcode    = MFO::Vocab::kActCastSelf;
        g2.actionParamForm = 0x00012FCC;   // Healing (Skyrim.esm). NOT 0x12FCD -- that is FLAMES.
        st.combat().push_back(g2);

        // Logistics table (DESIGN.md 4.8) -- proves both tables round-trip
        // independently, which is the point of P0.
        MFO::Gambit g3{};
        g3.conditionOpcode = MFO::Vocab::kCondSelfLowHealthPotion;
        g3.conditionParam  = 3.0f;   // "fewer than 3 health potions"
        g3.actionOpcode    = MFO::Vocab::kActLootPotions;
        st.logistics().push_back(g3);

        spdlog::info("[p0] seeded {} combat + {} logistics gambit(s) on {:08X}",
                     st.combat().size(), st.logistics().size(), player->GetFormID());
    }

    // M5 test seam: a default combat table, so the evaluator is testable
    // before the board exists (M7). `bSeedEvaluatorRules`, default OFF.
    //
    //   rule 0:  Self HP < 40%  ->  Cast Healing on self
    //   rule 1:  Always         ->  Wait
    //
    // Rule 1 matters: it proves first-match-wins by CONSUMING the tick, so a
    // healthy follower demonstrably reaches rule 1 and stops rather than
    // falling out of the scan.
    void SeedEvaluatorRules() {
        if (!MFO::Config::g_seedEvaluatorRules.load()) return;

        int seeded = 0;
        for (const auto& h : MFO::Followers::g_active) {
            auto* a = h.get().get();
            if (!a) continue;
            auto* rec = MFO::Followers::TryEnsureRecord(a->GetFormID());
            if (!rec) continue;

            // ALWAYS RESEED WHEN THE FLAG IS ON.
            //
            // This used to try to be clever -- keep existing rules unless every
            // cast rule named an unknown spell -- and the cleverness kept
            // fighting the test it exists to serve: a stale Healing rule
            // survived in the co-save and blocked a session, and adding the
            // attack rule would have made a record reseed on every single load.
            //
            // The flag IS the guard. It defaults off and never runs in normal
            // play, so "authored rules are safe" is enforced by not turning it
            // on, not by a heuristic that has to guess which rules were mine.
            if (!rec->combat().empty()) {
                spdlog::info("[eval] {:08X} {} -- replacing {} existing rule(s) (bSeedEvaluatorRules)",
                             a->GetFormID(), a->GetName(), rec->combat().size());
                rec->combat().clear();
            }
            // Clear LOGISTICS too. It was appended unconditionally below, so
            // every load with the flag on used to DUPLICATE the four defaults
            // (4, then 8, then 12 ...). Reseed means replace, both tables.
            rec->logistics().clear();

            // SEED A SPELL THEY ACTUALLY KNOW.
            //
            // The first seed hardcoded Healing (0x12FCC), and the field proved
            // why that is useless: Cosnach is a Markarth brawler who neither
            // knows it nor could afford it (29 magicka against a cost of 78).
            // The competence gate correctly refused every tick -- and the
            // animation test it was blocking never got to run. A rule the
            // follower CANNOT run is a fine thing to test once; it is a
            // terrible default.
            //
            // So: pick the cheapest spell from their OWN list. That also makes
            // the seed honest about §5.3 -- what a follower can do is a property
            // of the follower, not of MFO's hardcoded FormID.
            RE::SpellItem* chosen = nullptr;
            float bestCost = std::numeric_limits<float>::max();

            class Cheapest : public RE::Actor::ForEachSpellVisitor {
            public:
                Cheapest(RE::Actor* a_actor, RE::SpellItem*& a_out, float& a_best)
                    : actor(a_actor), out(a_out), best(a_best) {}
                RE::BSContainer::ForEachResult Visit(RE::SpellItem* a_spell) override {
                    if (!a_spell) return RE::BSContainer::ForEachResult::kContinue;
                    if (!MFO::Vocab::IsCastableSpell(a_spell))
                        return RE::BSContainer::ForEachResult::kContinue;
                    const float cost = a_spell->CalculateMagickaCost(actor);
                    if (cost < best) { best = cost; out = a_spell; }
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                RE::Actor* actor; RE::SpellItem*& out; float& best;
            } visitor(a, chosen, bestCost);
            a->VisitSpells(visitor);

            if (!chosen) {
                spdlog::info("[eval] {:08X} {} knows no castable spell -- seeding Wait only",
                             a->GetFormID(), a->GetName());
            } else {
                MFO::Gambit heal{};
                heal.conditionOpcode = MFO::Vocab::kCondSelfHpBelow;
                heal.conditionParam  = 0.60f;   // generous: this is a test seam
                heal.actionOpcode    = MFO::Vocab::kActCastSelf;
                heal.actionParamForm = chosen->GetFormID();
                rec->combat().push_back(heal);
                spdlog::info("[eval] {:08X} {} -> rule 0 casts {} ({:08X}, {:.0f} magicka)",
                             a->GetFormID(), a->GetName(),
                             chosen->GetName() ? chosen->GetName() : "?",
                             chosen->GetFormID(), bestCost);
            }

            // rule 1: Foe: lowest HP -> Attack. The FFXII opener, and the
            // reason the whole targeting mechanism exists. Costs nothing when
            // out of combat: no combat group means no candidate means the rule
            // is simply false and the tick falls through.
            MFO::Gambit attack{};
            attack.conditionOpcode = MFO::Vocab::kCondFoeLowestHp;
            attack.actionOpcode    = MFO::Vocab::kActAttack;
            rec->combat().push_back(attack);

            MFO::Gambit wait{};
            wait.conditionOpcode = MFO::Vocab::kCondAlways;
            wait.actionOpcode    = MFO::Vocab::kActWait;
            rec->combat().push_back(wait);

            // DEFAULT LOGISTICS TABLE -- the base gambits a follower should have
            // out of the box. Drink a health potion when hurt, keep arrows and
            // potions topped up, grab a better piece of gear. These do nothing
            // unless bLogistics is on, but they should EXIST so the board is not
            // empty and the behaviour is available.
            auto logi = [&](const char* cond, float p, const char* act) {
                MFO::Gambit g{};
                g.conditionOpcode = cond; g.conditionParam = p; g.actionOpcode = act;
                rec->logistics().push_back(g);
            };
            logi(MFO::Vocab::kCondSelfHpBelow,          0.50f, MFO::Vocab::kActDrinkHealthPotion);
            logi(MFO::Vocab::kCondSelfOutOfArrows,      10.0f, MFO::Vocab::kActLootArrows);
            logi(MFO::Vocab::kCondSelfLowHealthPotion,   2.0f, MFO::Vocab::kActLootPotions);
            logi(MFO::Vocab::kCondAlways,               0.0f,  MFO::Vocab::kActLootEquipment);

            ++seeded;
            spdlog::info("[eval] seeded default rules on {:08X} {}", a->GetFormID(), a->GetName());
        }
        if (seeded == 0) {
            spdlog::info("[eval] seed pass: no followers to seed");
        }
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
            MFO::Targeting::InstallHook();   // vfunc hook: once, never per-load
            MFO::CasterConsent::InstallHook();   // the influence hook, likewise
            MFO::Rapport::RegisterSinks();  // sinks LAST, or they fire against unresolved forms
            MFO::Logistics::RegisterSinks();   // the player-looted waiver sink (§4.8.3)
            MFO::Diagnostics::Install();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            // THE ONE EDGE WHERE MFO STILL OWNS THE OUTGOING WORLD.
            //
            // Every other piece of MFO state is session-scoped and dies with a
            // revert. An alias fill does not: the engine SERIALIZES IT INTO THE
            // .ess, so a follower who is in MFO's alias when the player loads a
            // different save is written into that transition and comes back
            // latched -- carrying a package MFO has no memory of handing out.
            //
            // This is #22d's shape ("the latch is never serialized, and is
            // cleared on load") inverted, and the inversion is what makes it
            // dangerous: the engine persists this one FOR us, so the bug
            // outlives the session instead of ending with it.
            spdlog::info("[startup] kPreLoadGame — releasing package alias before the world swaps");
            MFO::Packages::ReleaseAll("kPreLoadGame");
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
            // INVARIANTS #12: a downgraded DLL DESTROYS newer co-save records
            // on its next save. A log line is not enough -- the user never
            // reads it until the data is gone.
            if (MFO::ConsumeNewerSaveWarning()) {
                RE::DebugMessageBox(
                    "MFO: this save was written by a NEWER version of the mod.\n\n"
                    "Its follower data could not be read, and SAVING NOW WILL DESTROY IT.\n\n"
                    "Load an older save, or reinstall the newer MFO before saving.");
            }
            MFO::Probe::ReleaseAll();   // nothing the probe did outlives a session
            // Sweep an alias fill restored FROM the save we just loaded. The
            // kPreLoadGame release above only covers saves this session wrote;
            // a save from an earlier session, or from a build before the
            // release paths existed, arrives already latched. Reconciling on
            // the way IN is the only thing that can clean those up.
            MFO::Packages::ReleaseAll("post-load reconcile");
            MFO::Forms::EnsurePlayerSetup();
            MFO::Followers::Refresh();
            SeedTestData();
            SeedEvaluatorRules();   // after Refresh(): it iterates g_active
            MFO::Loadout::Reconcile();  // undo a save taken mid-cast
            MFO::Board::SetHud(MFO::Config::g_showHud.load());
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

    // The Field Kit's three trampoline hooks MUST be installed here, before
    // the renderer initializes. This is the only place they can go.
    MFO::Board::Install();

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(MFO::kSerID);
    serialization->SetSaveCallback(MFO::SaveCallback);
    serialization->SetLoadCallback(MFO::LoadCallback);
    serialization->SetRevertCallback(MFO::RevertCallback);

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);

    spdlog::info("=== MFO loaded (M3 + Field Kit overlay) ===");
    return true;
}
