#include "PCH.h"
#include "Leveling.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

// PCH deliberately never pulls <Windows.h> (CommonLibSSE-NG does not), so we
// declare the two Win32 calls we need, exactly as Targeting.cpp does.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char*);
extern "C" __declspec(dllimport) unsigned long __stdcall
    GetModuleFileNameA(void*, char*, unsigned long);

// See Leveling.h for the legal/rules basis. Independent implementation; the
// only things taken from their mod are interface facts (file paths, JSON field
// names, the mechanic values) that interop requires. Bundles nothing of theirs.

namespace MFO::Leveling {

    namespace {
        namespace fs = std::filesystem;
        using json = nlohmann::json;

        std::atomic<bool> g_detected{ false };

        // Their loaded DLL is the "is this mod active" signal -- soft, no ESP.
        constexpr const char* kTheirDll = "FollowerLvlSysRedone.dll";
        // On-disk locations (interface facts). State is per save + per follower;
        // the perk config is a single shared file. NOTE the two folder names
        // genuinely differ between the state dir and the config dir.
        constexpr const char* kStateDir  = "Data/SKSE/Plugins/Follower Leveling System Redone";
        constexpr const char* kPerksFile = "Data/SKSE/Plugins/FollowerLevelingSystemRedone/PerksAndSpells.json";

        constexpr int kPointsPerLevel = 5;
        constexpr int kVitalPerLevel  = 5;   // Health/Magicka/Stamina per level
        constexpr int kThresholds[]   = { 20, 40, 60, 80, 100 };

        const std::vector<Skill> g_skills = {
            { RE::ActorValue::kOneHanded,   "One-Handed",  "OneHanded"   },
            { RE::ActorValue::kTwoHanded,   "Two-Handed",  "TwoHanded"   },
            { RE::ActorValue::kArchery,     "Archery",     "Archery"     },
            { RE::ActorValue::kBlock,       "Block",       "Blocking"    },
            { RE::ActorValue::kHeavyArmor,  "Heavy Armor", "HeavyArmor"  },
            { RE::ActorValue::kLightArmor,  "Light Armor", "LightArmor"  },
            { RE::ActorValue::kSneak,       "Sneak",       "Sneak"       },
            { RE::ActorValue::kDestruction, "Destruction", "Destruction" },
            { RE::ActorValue::kRestoration, "Restoration", "Restoration" },
            { RE::ActorValue::kConjuration, "Conjuration", "Conjuration" },
            { RE::ActorValue::kIllusion,    "Illusion",    "Illusion"    },
            { RE::ActorValue::kAlteration,  "Alteration",  "Alteration"  },
        };

        // Game root = the folder holding the exe. REL::Module::filename() is only
        // the basename, so use the full exe path from Win32; fall back to CWD
        // (the game root under SKSE/MO2) if that ever fails.
        fs::path GameRoot() {
            char buf[260]{};
            const auto n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
            if (n == 0 || n >= sizeof(buf)) { std::error_code ec; return fs::current_path(ec); }
            return fs::path(std::string(buf, n)).parent_path();
        }

        // Follower's editor id -- the state file's basename. Needs po3 Tweaks
        // for the editorID map; empty means we cannot key their file, so skip.
        std::string EditorID(RE::Actor* a) {
            if (!a) return {};
            auto* base = a->GetActorBase();
            const char* e = base ? base->GetFormEditorID() : nullptr;
            if (!e || !*e) {
                // Editor IDs need po3 Tweaks. Without them we cannot key their
                // per-follower file, so the whole tab silently no-ops -- say so
                // once rather than leave the player wondering.
                static std::atomic<bool> warned{ false };
                if (!warned.exchange(true))
                    spdlog::warn("[leveling] no editor IDs available -- install po3 Tweaks "
                                 "(powerofthree's Tweaks) for the Leveling tab to work");
                return "";
            }
            return e;
        }

        // Their per-follower state path: <root>/<stateDir>/<charID>/<edid>.json.
        fs::path StatePath(RE::Actor* a) {
            const auto edid = EditorID(a);
            if (edid.empty()) return {};
            auto* slm = RE::BGSSaveLoadManager::GetSingleton();
            if (!slm) return {};
            const auto charID = slm->currentPlayerID;   // per-character save id (0xD0)
            return GameRoot() / kStateDir / std::to_string(charID) / (edid + ".json");
        }

        // Read a JSON file, or return null json on any failure (fail-soft).
        json ReadJson(const fs::path& p) {
            std::error_code ec;
            if (p.empty() || !fs::exists(p, ec)) return json{};
            try {
                std::ifstream in(p);
                if (!in) return json{};
                json j; in >> j; return j;
            } catch (const std::exception& e) {
                spdlog::warn("[leveling] parse failed {}: {}", p.string(), e.what());
                return json{};
            }
        }

        RE::ActorValueOwner* AVO(RE::Actor* a) { return a ? a->AsActorValueOwner() : nullptr; }
    }

    void RefreshDetection() {
        const bool present = GetModuleHandleA(kTheirDll) != nullptr;
        g_detected.store(present);
        spdlog::info("[leveling] Follower Leveling System Redone {}",
                     present ? "DETECTED -- interop tab enabled" : "not present -- tab hidden");
    }
    bool Detected() { return g_detected.load(); }

    const std::vector<Skill>& Skills() { return g_skills; }

    State Read(RE::Actor* a) {
        State s;
        if (!Detected() || !a) return s;
        auto* avo = AVO(a);
        if (avo) {
            for (std::size_t i = 0; i < g_skills.size(); ++i)
                s.skill[i] = static_cast<int>(avo->GetBaseActorValue(g_skills[i].av) + 0.5f);
        }
        const auto j = ReadJson(StatePath(a));
        if (j.is_object() && !j.empty()) {
            s.has = true;
            // Type-checked, never value(): a hand-corrupted "level":"four" would
            // throw type_error out of value() and take the process down.
            if (auto it = j.find("level"); it != j.end() && it->is_number())
                s.level = it->get<int>();
            if (auto it = j.find("skillPoints"); it != j.end() && it->is_number())
                s.points = it->get<int>();
        }
        return s;
    }

    bool RaiseSkill(RE::Actor* a, RE::ActorValue av) {
        if (!Detected()) return false;
        auto* avo = AVO(a);
        if (!avo) return false;
        avo->SetBaseActorValue(av, avo->GetBaseActorValue(av) + 1.0f);
        return true;
    }

    bool LowerSkill(RE::Actor* a, RE::ActorValue av) {
        if (!Detected()) return false;
        auto* avo = AVO(a);
        if (!avo) return false;
        const float cur = avo->GetBaseActorValue(av);
        if (cur <= 0.0f) return false;
        avo->SetBaseActorValue(av, cur - 1.0f);
        return true;
    }

    // Defensive write-back: re-read the file, change ONLY the two fields we own,
    // preserve every other key, create the dirs. Never corrupts their state.
    namespace {
        bool WriteState(RE::Actor* a, int level, int points) {
            const auto path = StatePath(a);
            if (path.empty()) return false;
            std::error_code ec;
            const bool existed = fs::exists(path, ec);
            json j = ReadJson(path);
            if (existed && !j.is_object()) {
                // Present but unparseable -- refuse rather than clobber whatever
                // (possibly recoverable) state and every donor field it holds.
                spdlog::warn("[leveling] existing state unreadable, not overwriting {}", path.string());
                return false;
            }
            if (!j.is_object()) j = json::object();
            j["level"]       = level;
            j["skillPoints"] = points;
            try {
                fs::create_directories(path.parent_path(), ec);
                std::ofstream out(path, std::ios::trunc);
                if (!out) return false;
                out << j.dump(4);
                return true;
            } catch (const std::exception& e) {
                spdlog::warn("[leveling] write failed {}: {}", path.string(), e.what());
                return false;
            }
        }
    }

    void ApplyProgression(RE::Actor* a) {
        if (!Detected() || !a) return;
        auto* base = a->GetActorBase();
        auto* avo  = AVO(a);
        if (!base || !avo) return;

        const json cfg = ReadJson(GameRoot() / kPerksFile);
        if (!cfg.is_object()) return;

        for (const auto& sk : g_skills) {
            auto it = cfg.find(sk.key);
            if (it == cfg.end() || !it->is_object()) continue;
            const int level = static_cast<int>(avo->GetBaseActorValue(sk.av) + 0.5f);

            for (const int t : kThresholds) {
                if (level < t) continue;
                auto tier = it->find(std::to_string(t));
                if (tier == it->end() || !tier->is_object()) continue;

                if (auto pit = tier->find("perk"); pit != tier->end() && pit->is_string()) {
                    const auto edid = pit->get<std::string>();
                    if (auto* perk = RE::TESForm::LookupByEditorID<RE::BGSPerk>(edid))
                        base->AddPerk(perk, 1);   // returns false if already held -- fine
                    else if (!edid.empty())
                        spdlog::warn("[leveling] perk editorID not found (need po3 Tweaks?): {}", edid);
                }
                if (auto sit = tier->find("spell"); sit != tier->end() && sit->is_string()) {
                    const auto edid = sit->get<std::string>();
                    if (auto* spell = RE::TESForm::LookupByEditorID<RE::SpellItem>(edid)) {
                        if (!a->HasSpell(spell)) a->AddSpell(spell);
                    } else if (!edid.empty()) {
                        spdlog::warn("[leveling] spell editorID not found: {}", edid);
                    }
                }
            }
        }
    }

    bool LevelUp(RE::Actor* a) {
        if (!Detected() || !a) return false;
        const State s = Read(a);
        if (s.points < kPointsPerLevel) return false;

        // Persist FIRST. If the write fails we grant nothing, so a retry is
        // clean; granting vitals first would double them on the retry.
        if (!WriteState(a, s.level + 1, s.points - kPointsPerLevel)) return false;

        if (auto* avo = AVO(a)) {
            for (const auto av : { RE::ActorValue::kHealth, RE::ActorValue::kMagicka,
                                   RE::ActorValue::kStamina }) {
                avo->SetBaseActorValue(av, avo->GetBaseActorValue(av) + kVitalPerLevel);
            }
        }
        ApplyProgression(a);   // grant any newly-crossed threshold perks/spells
        return true;
    }

}
