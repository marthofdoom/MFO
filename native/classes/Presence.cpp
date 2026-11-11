// classes/Presence.cpp -- is the class add-on ESL loaded, and is the tree live? (card C1)
//
// Same shape as the progression add-on's manifest scan (Progression.cpp Init, "v1.1 self-declaration"):
// walk the data handler's FLST array (the merged load order) for a list whose FIRST entry is a keyword
// whose editor-id ends in the manifest suffix. The suffix differs from "_MFOAddonManifest" on purpose,
// see the PRESENCE CONTRACT in Classes.h. Absent = inert with ONE named info line, never an error
// (the add-on is optional; Forms failure doctrine).
#include "Classes_internal.h"
#include "Config.h"

namespace MFO::Classes {

    namespace {
        constexpr std::string_view kManifestKeywordSuffix = "_MFOClassesManifest";
        std::atomic<bool> g_present{ false };
        std::string g_addonFile;   // the plugin the manifest came from (card C6 resolves its forms by local id). Written ONCE at kDataLoaded, read later.

        bool EdidHasSuffix(RE::TESForm* a_form, std::string_view a_suffix) {
            const char* e = a_form ? a_form->GetFormEditorID() : nullptr;
            if (!e) return false;
            const std::string_view id{ e };
            return id.size() >= a_suffix.size() &&
                   id.substr(id.size() - a_suffix.size()) == a_suffix;
        }
    }

    bool detail::DetectManifest() {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[classes] TESDataHandler unavailable -- class add-on detection skipped");
            return false;
        }
        int found = 0;
        for (auto* list : dh->GetFormArray<RE::BGSListForm>()) {
            if (!list || list->forms.empty()) continue;
            auto* kw = list->forms.front() ? list->forms.front()->As<RE::BGSKeyword>() : nullptr;
            if (!EdidHasSuffix(kw, kManifestKeywordSuffix)) continue;
            ++found;
            std::string plugin;
            if (auto* file = list->GetFile(0)) plugin = std::string(file->GetFilename());
            if (found == 1) g_addonFile = plugin;
            const char* kwEdid = kw->GetFormEditorID();
            spdlog::info("[classes] class add-on manifest {:08X} found (from \"{}\", keyword \"{}\", {} entr(ies))",
                         list->GetFormID(), plugin, kwEdid ? kwEdid : "", list->forms.size());
        }
        if (found > 1)
            spdlog::warn("[classes] {} class add-on manifests found -- using presence only, none is preferred", found);
        return found > 0;
    }

    void OnDataLoaded() {
        const bool present = detail::DetectManifest();
        g_present.store(present);
        const bool tree   = Config::g_classTree.load();
        const bool strict = Config::g_classStrict.load();
        if (!present) {
            spdlog::info("[classes] class add-on ESL not loaded -- class tree inert, the three classes (Melee/Ranged/Mage) behave as before");
        } else if (!tree) {
            spdlog::info("[classes] class add-on ESL loaded but bClassTree = 0 -- class tree inert, the three classes behave as before");
        } else {
            spdlog::info("[classes] class add-on ACTIVE (bClassTree = 1, bClassStrict = {})", strict ? 1 : 0);
            detail::ApplyAuraValues();   // card C11: the MCM /s values into the aura ability effects
        }
        // The table self-check runs whatever the presence: it is a build/data integrity line, not a feature.
        const int defects = detail::SelfCheck();
        if (defects)
            spdlog::error("[classes] table self-check found {} defect(s) -- the class data model is inconsistent", defects);
    }

    std::string_view detail::AddonFileName() { return g_addonFile; }

    bool Present() { return g_present.load(); }
    bool Active()  { return g_present.load() && Config::g_classTree.load(); }
    bool Strict()  { return Active() && Config::g_classStrict.load(); }
}
