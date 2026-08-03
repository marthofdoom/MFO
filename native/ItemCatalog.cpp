#include "ItemCatalog.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace MFO::Catalog {

    namespace {
        // Keyed by RESOLVED runtime FormID (LookupForm applied the load-order
        // offset), so lookups are a plain hash hit against a live form's GetFormID.
        std::unordered_map<RE::FormID, RE::ActorValue> g_potion;
        std::unordered_map<RE::FormID, Cure>           g_cure;       // cure-poison/disease potions
        std::unordered_map<RE::FormID, bool>           g_ammoBolt;   // true = bolt, false = arrow
        std::unordered_set<RE::FormID>                 g_jewelry;
        std::unordered_set<RE::FormID>                 g_soulgems;
        std::unordered_set<RE::FormID>                 g_excluded;
        bool                                           g_loaded = false;

        RE::ActorValue AvOf(const std::string& a_s) {
            if (a_s == "health")  return RE::ActorValue::kHealth;
            if (a_s == "stamina") return RE::ActorValue::kStamina;
            if (a_s == "magicka") return RE::ActorValue::kMagicka;
            return RE::ActorValue::kNone;
        }

        Cure CureOf(const std::string& a_s) {
            if (a_s == "poison")  return Cure::kPoison;
            if (a_s == "disease") return Cure::kDisease;
            if (a_s == "both")    return Cure::kBoth;
            return Cure::kNone;
        }

        // "0x03EADD" (or plain decimal) -> local FormID, masked to 24 bits. The
        // plugin's load-order byte(s) come from LookupForm, never from the file.
        RE::FormID Raw(const std::string& a_id) {
            return static_cast<RE::FormID>(std::strtoul(a_id.c_str(), nullptr, 0)) & 0xFFFFFF;
        }

        // Resolve a {plugin,id} row to a live form's runtime FormID, or 0.
        RE::FormID Resolve(RE::TESDataHandler* a_dh, const nlohmann::json& a_e) {
            const std::string plugin = a_e.value("plugin", std::string{});
            const std::string id     = a_e.value("id", std::string{});
            if (plugin.empty() || id.empty()) return 0;
            auto* form = a_dh->LookupForm(Raw(id), plugin);
            return form ? form->GetFormID() : 0;
        }
    }

    bool Loaded() { return g_loaded; }

    void Load() {
        g_potion.clear();
        g_cure.clear();
        g_ammoBolt.clear();
        g_jewelry.clear();
        g_soulgems.clear();
        g_excluded.clear();
        g_loaded = false;

        std::ifstream f("Data/SKSE/Plugins/MFO/mfo_items.json");
        auto*         dh = RE::TESDataHandler::GetSingleton();
        if (!f || !dh) {
            spdlog::info("[catalog] no mfo_items.json (run the MFO.Synthesis patcher) "
                         "-- runtime heuristics only");
            return;
        }

        try {
            nlohmann::json j;
            f >> j;
            int np = 0, nc = 0, nj = 0, nx = 0, miss = 0;

            // A potion row can restore, cure, or both -- resolve ONCE, then record
            // whichever fields it carries. A cure-only potion has no "restores",
            // so the old "skip if restores kNone" would have dropped it entirely.
            for (const auto& e : j.value("potions", nlohmann::json::array())) {
                const auto av  = AvOf(e.value("restores", std::string{}));
                const auto cur = CureOf(e.value("cures", std::string{}));
                if (av == RE::ActorValue::kNone && cur == Cure::kNone) continue;
                auto fid = Resolve(dh, e);
                if (!fid) { ++miss; continue; }
                if (av  != RE::ActorValue::kNone) { g_potion[fid] = av;  ++np; }
                if (cur != Cure::kNone)           { g_cure[fid]   = cur; ++nc; }
            }
            int na = 0;
            for (const auto& e : j.value("ammo", nlohmann::json::array())) {
                const auto kind = e.value("kind", std::string{});
                if (kind != "arrow" && kind != "bolt") continue;
                if (auto fid = Resolve(dh, e)) { g_ammoBolt[fid] = (kind == "bolt"); ++na; } else ++miss;
            }
            for (const auto& e : j.value("jewelry", nlohmann::json::array())) {
                if (auto fid = Resolve(dh, e)) { g_jewelry.insert(fid); ++nj; } else ++miss;
            }
            int ns = 0;
            for (const auto& e : j.value("soulgems", nlohmann::json::array())) {
                if (auto fid = Resolve(dh, e)) { g_soulgems.insert(fid); ++ns; } else ++miss;
            }
            for (const auto& e : j.value("exclude", nlohmann::json::array())) {
                if (auto fid = Resolve(dh, e)) { g_excluded.insert(fid); ++nx; } else ++miss;
            }

            g_loaded = true;
            spdlog::info("[catalog] loaded {} potions ({} cure), {} ammo, {} jewellery, "
                         "{} soul gems, {} excluded ({} rows unresolved in this load order)",
                         np, nc, na, nj, ns, nx, miss);
        } catch (const std::exception& ex) {
            spdlog::warn("[catalog] mfo_items.json parse failed: {} -- runtime heuristics only",
                         ex.what());
        }
    }

    RE::ActorValue PotionRestores(RE::FormID a_potion) {
        auto it = g_potion.find(a_potion);
        return it == g_potion.end() ? RE::ActorValue::kNone : it->second;
    }

    Cure PotionCures(RE::FormID a_potion) {
        auto it = g_cure.find(a_potion);
        return it == g_cure.end() ? Cure::kNone : it->second;
    }
    bool CuresPoison(RE::FormID a_potion) {
        const auto c = PotionCures(a_potion);
        return c == Cure::kPoison || c == Cure::kBoth;
    }
    bool CuresDisease(RE::FormID a_potion) {
        const auto c = PotionCures(a_potion);
        return c == Cure::kDisease || c == Cure::kBoth;
    }

    Ammo AmmoKind(RE::FormID a_ammo) {
        auto it = g_ammoBolt.find(a_ammo);
        if (it == g_ammoBolt.end()) return Ammo::kUnknown;
        return it->second ? Ammo::kBolt : Ammo::kArrow;
    }

    bool IsExcluded(RE::FormID a_item) { return g_excluded.contains(a_item); }
    bool IsJewelry(RE::FormID a_item)  { return g_jewelry.contains(a_item); }
    bool IsSoulGem(RE::FormID a_item)  { return g_soulgems.contains(a_item); }
}
