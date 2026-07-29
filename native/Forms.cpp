#include "PCH.h"
#include "Forms.h"

namespace MFO::Forms {

    namespace {
        template <class T>
        T* Look(RE::FormID a_local, const char* a_name) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                spdlog::error("[forms] TESDataHandler unavailable");
                return nullptr;
            }
            auto* f = dh->LookupForm<T>(a_local, kPlugin);
            if (!f) {
                // Name the form. A bare "lookup failed" count is the aggregate
                // that hides a 100%-systematic failure (INVARIANTS #47).
                spdlog::error("[forms] MISSING {} (0x{:03X} in {}) -- is the plugin installed and enabled?",
                              a_name, a_local, kPlugin);
            } else {
                spdlog::info("[forms] resolved {} -> {:08X}", a_name, f->GetFormID());
            }
            return f;
        }
    }

    bool Resolve() {
        g_fieldOrders = Look<RE::SpellItem>(kFieldOrdersSpell, "MFO_FieldOrdersPower");
        g_grantedKywd = Look<RE::BGSKeyword>(kGrantedKeyword, "MFO_GrantedSpell");

        // ── M9 records ──────────────────────────────────────────────────
        g_commandQuest = Look<RE::TESQuest>(kCommandQuest, "MFO_CommandQuest");
        g_castPackage  = Look<RE::TESPackage>(kCastPackage, "MFO_CastPackage");

        // ANSWER THE RECORD QUESTIONS AT LOAD, not when behaviour depends on
        // them. Three things can be wrong with a hand-authored PACK and each
        // fails silently in game: the record does not load, the alias package
        // list did not attach, or the template pointer did not survive the
        // round-trip. All three are readable right here.
        if (g_castPackage) {
            spdlog::info("[m9] MFO_CastPackage resolved -> {:08X}, procedureType={}, hasData={}",
                         g_castPackage->GetFormID(),
                         static_cast<std::uint32_t>(g_castPackage->procedureType.get()),
                         g_castPackage->data ? "Y" : "n");
        } else {
            spdlog::error("[m9] MFO_CastPackage did NOT resolve -- the PACK record is malformed "
                          "or the ESP is stale. M9 cannot work.");
        }

        if (g_commandQuest) {
            spdlog::info("[m9] MFO_CommandQuest resolved -> {:08X}, {} alias(es)",
                         g_commandQuest->GetFormID(),
                         static_cast<int>(g_commandQuest->aliases.size()));
            for (auto* a : g_commandQuest->aliases) {
                if (!a) continue;
                auto* ra = skyrim_cast<RE::BGSRefAlias*>(a);
                spdlog::info("[m9]   alias {} '{}' fillType={} isRefAlias={}",
                             a->aliasID,
                             a->aliasName.empty() ? "?" : a->aliasName.c_str(),
                             ra ? static_cast<std::uint32_t>(ra->fillType.get()) : 0u,
                             ra ? "Y" : "n");
            }
        } else {
            spdlog::error("[m9] MFO_CommandQuest did NOT resolve -- no alias to deliver a package.");
        }

        const bool ok = g_fieldOrders != nullptr;
        if (!ok) {
            spdlog::error("[forms] Field Orders power did not resolve -- the board cannot be opened. "
                          "Everything else still runs.");
        }
        return ok;
    }

    void EnsurePlayerSetup() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (!g_fieldOrders) {
            // Do NOT latch a failed grant. A missing ESP must retry next load,
            // never burn the one-time flag (INVARIANTS, one-time grants).
            spdlog::warn("[setup] skipping power grant -- form unresolved; will retry next load");
            return;
        }

        // HasSpell/AddSpell is the engine's own flow and is idempotent-safe
        // when guarded. This replaces the Papyrus startup quest entirely --
        // which is why MFO_StartupQuest ships with no VMAD.
        if (!player->HasSpell(g_fieldOrders)) {
            player->AddSpell(g_fieldOrders);
            spdlog::info("[setup] granted Field Orders power");
        } else {
            // Log the zero case too, or "already had it" and "never ran" look
            // identical in the log (INVARIANTS #46).
            spdlog::info("[setup] Field Orders power already present");
        }
    }

}
