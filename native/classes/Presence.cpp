// classes/Presence.cpp -- is the class add-on ESL loaded, and is the tree live? (card C1)
//
// Same shape as the progression add-on's manifest scan (Progression.cpp Init, "v1.1 self-declaration"):
// walk the data handler's FLST array (the merged load order) for a list whose FIRST entry is a keyword
// whose editor-id ends in the manifest suffix. The suffix differs from "_MFOAddonManifest" on purpose,
// see the PRESENCE CONTRACT in Classes.h. Absent = inert with ONE named info line, never an error
// (the add-on is optional; Forms failure doctrine).
#include "Classes_internal.h"
#include "Config.h"
#include "apmf/APMFBridge.h"
#include "MainThread.h"
#include "Rapport.h"

namespace MFO::Classes {

    namespace {
        constexpr std::string_view kManifestKeywordSuffix = "_MFOClassesManifest";
        std::atomic<bool> g_present{ false };
        std::atomic<bool> g_harbingerOk{ false };   // Harbinger acquired at ABI >= kMinHarbingerAbi; written ONCE at kDataLoaded
        std::atomic<bool> g_noticed{ false };       // the in-game "did not start" notice went out this session
        RE::BGSListForm* g_manifestList = nullptr;   // the first manifest FLST found; written ONCE at kDataLoaded (card C8 reads its style lookup entry)
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
            if (found == 1) { g_addonFile = plugin; g_manifestList = list; }
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
        const std::uint32_t abi = APMFBridge::AbiVersion();
        const bool harbinger = abi >= kMinHarbingerAbi;
        g_harbingerOk.store(harbinger);
        const bool tree   = Config::g_classTree.load();
        const bool strict = Config::g_classStrict.load();
        if (!present) {
            spdlog::info("[classes] class add-on ESL not loaded -- class tree inert, the three classes (Melee/Ranged/Mage) behave as before");
        } else if (!harbinger) {
            // Loud on purpose, never a silent degrade: the ESL is loaded but the add-on is refused (MCLS stays held, see State.cpp).
            if (abi == 0)
                spdlog::error("[classes] classes add-on not started: Harbinger (APMF.dll) missing or unusable (no interface acquired), "
                              "the class tree needs ABI v{}. The stored class progress is kept untouched.", kMinHarbingerAbi);
            else
                spdlog::error("[classes] classes add-on not started: Harbinger ABI v{} < required v{}. "
                              "The stored class progress is kept untouched.", abi, kMinHarbingerAbi);
        } else if (!tree) {
            spdlog::info("[classes] class add-on ESL loaded but bClassTree = 0 -- class tree inert, the three classes behave as before");
        } else {
            spdlog::info("[classes] class add-on ACTIVE (bClassTree = 1, bClassStrict = {})", strict ? 1 : 0);
            detail::ApplyAuraValues();   // card C11: the MCM /s values into the aura ability effects
        }
        if (present) detail::ResolvePackageStyles();   // card C8: the per-class CSTY lookup (classes/Package.cpp)
        // The table self-check runs whatever the presence: it is a build/data integrity line, not a feature.
        const int defects = detail::SelfCheck();
        if (defects)
            spdlog::error("[classes] table self-check found {} defect(s) -- the class data model is inconsistent", defects);
    }

    std::string_view detail::AddonFileName() { return g_addonFile; }
    RE::BGSListForm* detail::ManifestList()  { return g_manifestList; }

    bool Present() { return g_present.load(); }
    bool HarbingerOk() { return g_harbingerOk.load(); }
    bool Active()  { return g_present.load() && g_harbingerOk.load() && Config::g_classTree.load(); }
    void MaybeNoticeNotStarted() {
        if (g_noticed.load(std::memory_order_relaxed)) return;
        if (!g_present.load() || g_harbingerOk.load() || !Config::g_classTree.load()) return;
        if (Rapport::SessionMinutes() < 0.6) return;   // in the world, like the no-APMF toast
        if (g_noticed.exchange(true)) return;
        MainThread::Post([]() {
            RE::DebugNotification("MFO classes did not start: they need Harbinger (APMF) installed and up to date.");
        });
    }
    bool Strict()  { return Active() && Config::g_classStrict.load(); }
}
