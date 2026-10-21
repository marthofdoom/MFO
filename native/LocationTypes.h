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


    // The TOWN a location sits in, or null: the OUTERMOST location on the parent chain (guard 8) that carries
    // LocTypeCity / LocTypeTown / LocTypeSettlement (D1, town run 3), met BEFORE a fight site (Clearable / Dungeon) -- the
    // same innermost-first fight-site test as Classify, so a bandit mine inside a hold is never "a town" and a shop
    // interior under Whiterun resolves to WhiterunLocation. Town errands (logistics/Errand.cpp, ClickUp
    // 86e3t1mx7) compare the anchor of the PLAYER's location with the anchor of the follower's. The
    // keywords are Skyrim.esm FormIDs resolved once, exactly as in Classify (a modlist that lacks one
    // simply has no anchors of that type). Pure reads; callable from the job worker like Classify.
    inline const RE::BGSLocation* TownAnchor(const RE::BGSLocation* a_loc) {
        struct Kw { RE::FormID id; bool town; };
        static const std::vector<std::pair<const RE::BGSKeyword*, bool>> s_kws = [] {
            constexpr Kw kList[] = {
                { 0x000F5E80, false },   // LocTypeClearable
                { 0x000130DB, false },   // LocTypeDungeon
                { 0x00013168, true },    // LocTypeCity
                { 0x00013166, true },    // LocTypeTown
                { 0x00013167, true },    // LocTypeSettlement
            };
            std::vector<std::pair<const RE::BGSKeyword*, bool>> v;
            auto* dh = RE::TESDataHandler::GetSingleton();
            for (const auto& k : kList) {
                auto* kw = dh ? dh->LookupForm<RE::BGSKeyword>(k.id & 0x00FFFFFF, "Skyrim.esm") : nullptr;
                if (kw) v.emplace_back(kw, k.town);
            }
            return v;
        }();
        // D1 (marth 2026-10-20): the OUTERMOST City / Town / Settlement on the chain, not the innermost. Markarth's "Outskirts" interiors sit
        // under SalviusFarmLocation (LocTypeSettlement) under MarkarthLocation (City): the innermost anchor made the Dormitory / temple "a
        // farm", not Markarth. A fight site met BEFORE any town still means "not a town"; one met ABOVE a town just ends the walk.
        // COLLEGE OF WINTERHOLD (marth 2026-10-21: "the college is unique, so it can be a standalone rule, its to make sure mages buy spells from those
        // vendors"): Skyrim.esm WinterholdCollegeLocation 00076F3A is its OWN town. Its parent is WinterholdHoldLocation 0001676B (a sibling of
        // WinterholdLocation 00018A51, NOT below it) and it carries no City / Town / Settlement keyword (Guild, Habitation), so the walk found no
        // anchor for any College cell. Resolved by FormID once (absent in the modlist = no rule); the outermost walk ends AT it.
        static const RE::BGSLocation* const s_college = [] {
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<RE::BGSLocation>(0x00076F3A, "Skyrim.esm") : nullptr;
        }();
        const RE::BGSLocation* best = nullptr;
        int guard = 0;
        for (auto* loc = a_loc; loc && guard < 8; loc = loc->parentLoc, ++guard) {
            for (const auto& [kw, town] : s_kws)
                if (!town && loc->HasKeyword(kw)) return best;   // best is null when the fight site is innermost: not a town
            if (s_college && loc == s_college) return loc;      // standalone: the College stops the walk and IS the town
            for (const auto& [kw, town] : s_kws)
                if (town && loc->HasKeyword(kw)) { best = loc; break; }
        }
        return best;
    }


    // ── TOWN PHASE 2 R7: inn / temple (ClickUp 86e3t1mx7; design TOWN-PHASE2-DESIGN.md 1.g) ────────────────────────────
    // A location that IS an inn / a temple: walking from the innermost location out, LocTypeInn (0001CB87) / LocTypeTemple
    // (0001CD56) is met BEFORE a town-level tag (City / Town / Settlement) or a fight site (Clearable / Dungeon). So an inn's
    // interior location (which carries the keyword itself) is an inn, while the town above it never is. Pure reads.
    inline bool HasKeywordInChainBefore(const RE::BGSLocation* a_loc, RE::FormID a_kw) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* want = dh ? dh->LookupForm<RE::BGSKeyword>(a_kw & 0x00FFFFFF, "Skyrim.esm") : nullptr;
        if (!want) return false;
        static const std::vector<const RE::BGSKeyword*> s_stop = [] {
            std::vector<const RE::BGSKeyword*> v;
            auto* d = RE::TESDataHandler::GetSingleton();
            for (const RE::FormID id : { 0x00013168u, 0x00013166u, 0x00013167u, 0x000F5E80u, 0x000130DBu })   // City Town Settlement Clearable Dungeon
                if (auto* k = d ? d->LookupForm<RE::BGSKeyword>(id, "Skyrim.esm") : nullptr) v.push_back(k);
            return v;
        }();
        // F6 (town run 3): a location's OWN keywords decide first. It inherits Inn / Temple from a parent ONLY when it carries no place-type
        // keyword of its own (Inn, Temple, Store, House, Dwelling, Guild, PlayerHouse): a Store whose parent location is an Inn is a shop, not
        // an inn (Markarth's WasteCollector / EEGoods were planned as "the inn").
        static const std::vector<const RE::BGSKeyword*> s_place = [] {
            std::vector<const RE::BGSKeyword*> v;
            auto* d = RE::TESDataHandler::GetSingleton();
            for (const RE::FormID id : { 0x0001CB87u, 0x0001CD56u, 0x0001CB86u, 0x0001CB85u, 0x000130DCu, 0x0001CD5Au, 0x000FC1A3u })   // Inn Temple Store House Dwelling Guild PlayerHouse
                if (auto* k = d ? d->LookupForm<RE::BGSKeyword>(id, "Skyrim.esm") : nullptr) v.push_back(k);
            return v;
        }();
        int guard = 0;
        for (auto* loc = a_loc; loc && guard < 8; loc = loc->parentLoc, ++guard) {
            if (loc->HasKeyword(want)) {
                if (loc == a_loc) return true;   // its OWN keyword decides
                // Run 4: INHERITED from a parent only when the parent's ONLY place-type keyword is the wanted one. A parent carrying
                // several (Old Temple's parent was an all-keyword district) is a district, not the place: no inheritance across it.
                for (auto* k : s_place) if (k != want && loc->HasKeyword(k)) return false;
                return true;
            }
            for (auto* k : s_place) if (loc->HasKeyword(k)) return false;   // another kind of place: no inheritance across it
            for (auto* k : s_stop) if (loc->HasKeyword(k)) return false;
        }
        return false;
    }
    inline bool IsInn(const RE::BGSLocation* a_loc)    { return HasKeywordInChainBefore(a_loc, 0x0001CB87); }   // LocTypeInn
    inline bool IsTemple(const RE::BGSLocation* a_loc) { return HasKeywordInChainBefore(a_loc, 0x0001CD56); }   // LocTypeTemple

    // The first inn / temple location under a town anchor (a_town == TownAnchor(...) of the player), or null. A scan of the
    // Location form array (a few hundred forms), so callers cache per town. Names the place for logs and the Board; the
    // errand's walk itself goes to the interior CELL found through the town's load doors (logistics/ErrandRest.cpp).
    inline const RE::BGSLocation* InnOf(const RE::BGSLocation* a_town) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !a_town) return nullptr;
        for (auto* loc : dh->GetFormArray<RE::BGSLocation>())
            if (loc && IsInn(loc) && TownAnchor(loc) == a_town) return loc;
        return nullptr;
    }
    inline const RE::BGSLocation* TempleOf(const RE::BGSLocation* a_town) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !a_town) return nullptr;
        for (auto* loc : dh->GetFormArray<RE::BGSLocation>())
            if (loc && IsTemple(loc) && TownAnchor(loc) == a_town) return loc;
        return nullptr;
    }

}
