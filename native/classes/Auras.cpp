// classes/Auras.cpp -- the MCM /s values pushed into the aura ability effects (card C11).
//
// marth 2026-11-10: "All our /s numbers will probably need to be factored per list, or available in an mcm for tweaking."
// Ruling: every /s value is an INI / MCM value (Config.h: fPriestSunPerSec 2, fSaintHealPerSec 30, fSaintUndeadPerSec 30), the
// default is scaled at runtime, and there is NEVER a per-list table (no-list-specific-data). MFO patches the aura abilities'
// effect magnitudes at data load and on every MCM change. This is a DATA write to the add-on's OWN spell forms (the effect list
// of an ability SPEL in MFO_Classes.esl): no engine action, no Harbinger road needed.
//
// THE SCALE: a cloak-style aura applies its effect once per pulse, so the magnitude of the effect is (value per second) x
// (seconds between pulses). kAuras carries the pulse length per row (1.0 s = a 1-per-second pulse). The runtime value is read from
// Config on every apply, so an MCM edit takes effect on the next pulse with no reload.
//
// STATE OF THE CARRIERS (be loud about it): the Priest sun cloak and the Saint aura are NOT built in MFO_Classes.esl yet
// (classes/Carriers.cpp kMissing says so, "card C11"); building a cloak (a Cloak-archetype MGEF with its area-applied damage / heal
// effects) is ESL generator work with its own byte audit and its own field run. Until then every kAuras row has localId = 0 =
// "not built": the values are held and logged, nothing is patched, and the day a row gets its local id (and effect index) this
// file patches it with no other change. The patch finds the SPEL by local id in the add-on's own file (the same resolution the
// carriers use, Carriers.cpp Resolve), never by a list FormID.
//
// THREADING: the write is a plain float store into a SpellItem's effect list, posted to the MAIN thread (MainThread::Post) like
// every other engine-form write here; OnDataLoaded (main) applies directly.
#include "Classes_internal.h"
#include "Config.h"
#include "MainThread.h"

namespace MFO::Classes {

    namespace {
        struct AuraRow {
            const char*                  what;        // log name
            RE::FormID                   localId;     // the ability SPEL's local id in the add-on file; 0 = not built yet
            std::size_t                  effectIndex; // which of the SPEL's effects carries the magnitude
            float                        pulseSec;    // seconds between applications of that effect
            const std::atomic<float>*    perSec;      // the MCM / INI value, per second
        };
        const AuraRow kAuras[] = {
            { "Priest sun (undead / Daedra in 6 m)",  0, 0, 1.0f, &Config::g_priestSunPerSec },
            { "Saint aura heal (allies)",             0, 0, 1.0f, &Config::g_saintHealPerSec },
            { "Saint aura damage (undead)",           0, 1, 1.0f, &Config::g_saintUndeadPerSec },
        };

        // MAIN thread.
        void Apply() {
            auto* dh = RE::TESDataHandler::GetSingleton();
            const std::string file{ detail::AddonFileName() };
            int patched = 0, unbuilt = 0;
            std::string line;
            for (const auto& a : kAuras) {
                const float perSec = a.perSec->load();
                line += std::format("{}{} {:g}/s", line.empty() ? "" : "; ", a.what, perSec);
                if (a.localId == 0 || !dh || file.empty()) { ++unbuilt; continue; }
                auto* sp = dh->LookupForm<RE::SpellItem>(a.localId, file);
                if (!sp || a.effectIndex >= sp->effects.size() || !sp->effects[a.effectIndex]) {
                    spdlog::error("[classes] aura: {} (local id 0x{:03X}) or its effect #{} not found in {} -- NOT patched", a.what, a.localId,
                                  a.effectIndex, file);
                    continue;
                }
                sp->effects[a.effectIndex]->effectItem.magnitude = perSec * a.pulseSec;
                ++patched;
            }
            spdlog::info("[classes] aura values: {} -- {} effect(s) patched, {} aura ability(ies) not in the ESL yet (values held)",
                         line, patched, unbuilt);
        }
    }

    // Called at the end of OnDataLoaded (main thread) when the add-on is present.
    void detail::ApplyAuraValues() { Apply(); }

    // Any thread (the MCM-close task on the pump worker): the write itself goes to the main thread.
    void OnConfigRead() {
        if (!Active()) return;
        MainThread::Post([] { Apply(); });
    }
}
