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
        // v1.1: the addon-manifest sentinel keyword is retired — add-ons self-
        // declare with their OWN keyword (Progression::Init, edid-suffix match).

        // ── M9 records ──────────────────────────────────────────────────
        g_commandQuest = Look<RE::TESQuest>(kCommandQuest, "MFO_CommandQuest");
        g_castPackage  = Look<RE::TESPackage>(kCastPackage, "MFO_CastPackage");
        // FORCED SELF-CAST (SPEC-self-cast-forced): t6/no-QNAM self package on
        // command-quest alias 2. A miss disables ONLY the self route (named log
        // line via Look), never a hard requirement.
        g_castPackageSelf = Look<RE::TESPackage>(kCastPackageSelf, "MFO_CastPackageSelf");
        g_lootQuest    = Look<RE::TESQuest>(kLootQuest, "MFO_LootQuest");        // Option A
        g_travelPackage = Look<RE::TESPackage>(kTravelPackage, "MFO_TravelPackage");  // Option A (slot 0)
        // P7 slots 1-3. Diagnostic-only (WALK readout); a missing extra disables
        // nothing, so these use the same named-miss log without gating anything.
        g_travelPackage1 = Look<RE::TESPackage>(kTravelPackage1, "MFO_TravelPackage1");  // P7 slot 1
        g_travelPackage2 = Look<RE::TESPackage>(kTravelPackage2, "MFO_TravelPackage2");  // P7 slot 2
        g_travelPackage3 = Look<RE::TESPackage>(kTravelPackage3, "MFO_TravelPackage3");  // P7 slot 3
        // APMF loot-travel (ch.9 0x49 route). A miss disables ONLY that slot's APMF
        // route -- LootTravelFill falls back to the alias route (named log, never a
        // hard requirement).
        g_apmfLootTravelPackage0 = Look<RE::TESPackage>(kAPMFLootTravelPackage0, "MFO_APMFLootTravelPackage0");
        g_apmfLootTravelPackage1 = Look<RE::TESPackage>(kAPMFLootTravelPackage1, "MFO_APMFLootTravelPackage1");
        g_apmfLootTravelPackage2 = Look<RE::TESPackage>(kAPMFLootTravelPackage2, "MFO_APMFLootTravelPackage2");
        g_apmfLootTravelPackage3 = Look<RE::TESPackage>(kAPMFLootTravelPackage3, "MFO_APMFLootTravelPackage3");
        g_retreatQuest   = Look<RE::TESQuest>(kRetreatQuest, "MFO_RetreatQuest");        // RETREAT PROBE
        g_retreatPackage = Look<RE::TESPackage>(kRetreatPackage, "MFO_RetreatPackage");  // RETREAT PROBE
        g_apmfRetreatPackage = Look<RE::TESPackage>(kAPMFRetreatPackage, "MFO_APMFRetreatPackage");  // APMF ch.9 0x49 route
        // APMF animated-heal packages (ch.9 0x49 route, OPT-IN bHealAnimPackage).
        // A miss disables ONLY the animated heal -- HealAnimFill declines and the
        // caller keeps the byte-identical kInstant heal (named log, never a hard
        // requirement).
        g_apmfHealSelfPackage   = Look<RE::TESPackage>(kAPMFHealSelfPackage,   "MFO_APMFHealSelfPackage");
        g_apmfHealPlayerPackage = Look<RE::TESPackage>(kAPMFHealPlayerPackage, "MFO_APMFHealPlayerPackage");
        g_tradeQuest     = Look<RE::TESQuest>(kTradeQuest, "MFO_TradeQuest");            // #21 econ bridge
        // P1 probe style. A miss disables ONLY the (default-off) style-swap
        // probe -- ProbeStyleTick refuses to swap on a null pointer and says
        // nothing else about it, per the "one feature, named log line" rule.
        g_probeCastStyle = Look<RE::TESCombatStyle>(kProbeCastStyle, "MFO_CastStyle");   // P1 probe
        // Stance-ownership CSTYs (bWeaponStyleControl, default ON). A miss
        // disables ONLY that stance's swap -- CombatStyle::ApplyTick refuses to
        // swap on a null target and says nothing else, per "one feature, one
        // named line".
        g_meleeStyle  = Look<RE::TESCombatStyle>(kMeleeStyle,  "MFO_MeleeStyle");
        g_rangedStyle = Look<RE::TESCombatStyle>(kRangedStyle, "MFO_RangedStyle");
        g_castStyle   = g_probeCastStyle;   // Cast stance reuses the pure-mage CSTY (0x832)

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
