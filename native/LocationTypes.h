#pragma once
// LocationTypes.h -- ONE table of Skyrim.esm location-type keywords and ONE innermost-first
// classifier, shared by the engage-on-sight town/inn filter (EngageOnSight.cpp IsCivilised)
// and the lockpick load-door destination bar (logistics/Lockpick.cpp LoadDoorBar), so the two
// can never disagree on what a lived-in place is (LP-M2 closing round, tier-A review of c6fc73d).
// Moved here from EngageOnSight.cpp unchanged (the list, its FormIDs and its reasoning).
//
// A location is CIVILISED when, walking from the innermost location out through parentLoc, a
// civilised keyword is met BEFORE a fight-site one. Fight site = LocTypeClearable /
// LocTypeDungeon: a bandit mine or the Ratway under Riften is a fight site even though its
// parent is a town, and the innermost tag decides. Civilised = every Skyrim.esm LocType that
// marks a lived-in place (erring toward civilised, per marth): Inn, City, Town, Settlement,
// Habitation, HabitationHasInn, Dwelling, House, PlayerHouse, Store, Guild, Temple, Castle,
// Barracks, Jail, StewardsDwelling, Farm, Mine, LumberMill, OrcStronghold. NOT the LocTypeHold*
// family: those tag a whole hold, wilderness included. Resolved ONCE by FormID in Skyrim.esm
// (EditorIDs are not reliably kept); a keyword that does not resolve is simply absent from the
// set. FormIDs verified against Skyrim.esm's KYWD records.
//
// Threading: pure reads (the form map, the location's keyword list); the one-time resolve is a
// function-local static (thread-safe initialisation). Called from the main thread
// (engage-on-sight) and the tick's job worker (the lockpick judge).
#include "PCH.h"
#include <iterator>
#include <utility>
#include <vector>

namespace MFO::LocationTypes {

    enum class Kind {
        kUntagged,    // no location, or no location type on its whole chain
        kFightSite,   // the innermost tagged location is Clearable / Dungeon
        kCivilised,   // the innermost tagged location is a lived-in place
    };

    inline Kind Classify(const RE::BGSLocation* a_loc) {
        struct Kw { RE::FormID id; bool civil; };
        static const std::vector<std::pair<const RE::BGSKeyword*, bool>> s_kws = [] {
            constexpr Kw kList[] = {
                { 0x000F5E80, false },   // LocTypeClearable
                { 0x000130DB, false },   // LocTypeDungeon
                { 0x0001CB87, true },    // LocTypeInn
                { 0x00013168, true },    // LocTypeCity
                { 0x00013166, true },    // LocTypeTown
                { 0x00013167, true },    // LocTypeSettlement
                { 0x00039793, true },    // LocTypeHabitation
                { 0x000A6E84, true },    // LocTypeHabitationHasInn
                { 0x000130DC, true },    // LocTypeDwelling
                { 0x0001CB85, true },    // LocTypeHouse
                { 0x000FC1A3, true },    // LocTypePlayerHouse
                { 0x0001CB86, true },    // LocTypeStore
                { 0x0001CD5A, true },    // LocTypeGuild
                { 0x0001CD56, true },    // LocTypeTemple
                { 0x0001CD57, true },    // LocTypeCastle
                { 0x0001CD55, true },    // LocTypeBarracks
                { 0x0001CD59, true },    // LocTypeJail
                { 0x000504F9, true },    // LocTypeStewardsDwelling
                { 0x00018EF0, true },    // LocTypeFarm
                { 0x00018EF1, true },    // LocTypeMine
                { 0x00018EF2, true },    // LocTypeLumberMill
                { 0x000130E9, true },    // LocTypeOrcStronghold
            };
            std::vector<std::pair<const RE::BGSKeyword*, bool>> v;
            auto* dh = RE::TESDataHandler::GetSingleton();
            for (const auto& k : kList) {
                auto* kw = dh ? dh->LookupForm<RE::BGSKeyword>(k.id & 0x00FFFFFF, "Skyrim.esm") : nullptr;
                if (kw) v.emplace_back(kw, k.civil);
            }
            spdlog::info("[location-types] {} of {} Skyrim.esm location keywords resolved (the town/inn "
                         "filter and the lockpick load-door bar share them)", v.size(), std::size(kList));
            return v;
        }();
        int guard = 0;
        for (auto* loc = a_loc; loc && guard < 8; loc = loc->parentLoc, ++guard) {
            for (const auto& [kw, civil] : s_kws) {
                if (!civil && loc->HasKeyword(kw)) return Kind::kFightSite;   // innermost first
            }
            for (const auto& [kw, civil] : s_kws) {
                if (civil && loc->HasKeyword(kw)) return Kind::kCivilised;
            }
        }
        return Kind::kUntagged;
    }

}
