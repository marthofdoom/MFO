// logistics/ErrandVendors.cpp -- the SHOP'S VENDOR, NPC-FIRST (mfo-272).
//
// marth 2026-10-25: "In some games shopkeepers may be dead so using NPC first is a better option, who runs this shop, is the shop open,
// where is that NPC" and "I'd strongly prefer locating the NPC and navigating to them, if the shop is supposed to be open they will have
// the dialogue". So a catalog shop is modelled from its vendor NPC(s), not from its PLVD place / merchant chest:
//   WHO runs it   = the actors that are members of the vendor faction (final rank >= 0), alive and enabled. None alive = no shop.
//   IS IT OPEN    = the faction's VENV hours (Errand.cpp OpenAt, unchanged).
//   WHERE are they = the actor's current parent cell / position (loaded or not): the walk's destination.
// The place and the chest stay context only (the town membership, the chest the trade moves goods through).
//
// THE INDEX (vendor faction -> actor refs) is built ONCE per game load, on the MAIN thread, from the engine's all-forms map
// (RE::TESForm::GetAllForms, read-locked exactly as LookupByID does): every ActorCharacter form in memory -- every persistent actor (a
// unique vendor is a persistent ref and stays in memory with its cell unloaded) plus the loaded temporary ones -- and the vendor factions
// each is a member of (Actor::VisitFactions: the base NPC's list, then the runtime ExtraFactionChanges, the later entry winning). Nothing
// is hardcoded and nothing is per-list: whatever the load order put in the factions is what is read. Session-only, never saved.
//
// WHY MEMBERSHIP, NOT "THE" VENDOR FACTION: Errand.cpp VendorFactionOf returns an actor's FIRST vendor faction. Adrianne Avenicci is in
// two (Skyrim.esm: ServicesWhiterunWarmaidens 0009CAFF first, ServicesWhiterunWarmaidensForge 000C6E40 second), so every "is this actor
// the vendor of faction X" test that compared VendorFactionOf(a) == X found 0 members of the forge shop (the 2026-10-25 Sven field log).
//
// Threading: Locate / IsMember / LoadedVendor run on the AddTask worker (the planner) and read actors the way the planner already does
// (LookupByID, parent cell, position, VisitFactions). The index build runs on the main thread. Own mutex (a leaf: never held across an
// engine call, never nested with Errand g_mx).

#include "PCH.h"
#include "Errand_internal.h"
#include "LocationTypes.h"
#include "MainThread.h"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Vendors {

    namespace {
        enum class IndexState { Unbuilt, Building, Built };
        std::mutex                                                s_mx;
        IndexState                                                s_state = IndexState::Unbuilt;
        std::uint32_t                                             s_gen = 0;   // bumped by ClearAll: a build that lands after a load is dropped
        std::unordered_map<RE::FormID, std::vector<RE::FormID>>  s_index;     // vendor faction -> member actor refs

        bool Asleep(RE::Actor* a) {
            const auto* st = a->AsActorState();
            if (!st) return false;
            const auto s = st->GetSitSleepState();
            return s == RE::SIT_SLEEP_STATE::kIsSleeping || s == RE::SIT_SLEEP_STATE::kWantToSleep || s == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim;
        }

        // The vendor factions this actor is a member of (final rank >= 0).
        void VendorMemberships(RE::Actor* a, std::vector<RE::FormID>& a_out) {
            std::unordered_map<RE::FormID, std::int8_t> rank;
            a->VisitFactions([&](RE::TESFaction* f, std::int8_t r) {
                if (f && f->IsVendor()) rank[f->GetFormID()] = r;   // base first, then the runtime changes: the later entry wins
                return false;
            });
            for (const auto& [f, r] : rank)
                if (r >= 0) a_out.push_back(f);
        }

        // MAIN THREAD. Collect the actor ids under the all-forms read lock (a type test only), then read each outside it.
        void BuildIndex(std::uint32_t a_gen) {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<RE::FormID> actors;
            {
                const auto& [map, lock] = RE::TESForm::GetAllForms();
                const RE::BSReadLockGuard guard{ lock.get() };
                if (map) {
                    for (const auto& e : *map)
                        if (e.second && e.second->GetFormType() == RE::FormType::ActorCharacter) actors.push_back(e.first);
                }
            }
            std::unordered_map<RE::FormID, std::vector<RE::FormID>> idx;
            std::size_t vendors = 0;
            std::vector<RE::FormID> facs;
            for (const auto id : actors) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || a->IsPlayerRef() || a->IsDeleted()) continue;
                facs.clear();
                VendorMemberships(a, facs);
                if (facs.empty()) continue;
                ++vendors;
                for (const auto f : facs) idx[f].push_back(id);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::scoped_lock lk(s_mx);
            if (a_gen != s_gen) return;   // a load / revert ran meanwhile: this index describes the previous session
            s_index = std::move(idx);
            s_state = IndexState::Built;
            spdlog::info("[errand] vendor index built (main thread, {} ms): {} actor(s) in memory, {} of them vendors, {} vendor faction(s) with a member",
                         ms, actors.size(), vendors, s_index.size());
        }

        // TRUE = built; FALSE = the build is posted / in flight (the planner re-plans shortly).
        bool EnsureIndex() {
            std::uint32_t gen = 0;
            {
                std::scoped_lock lk(s_mx);
                if (s_state == IndexState::Built) return true;
                if (s_state == IndexState::Building) return false;
                s_state = IndexState::Building;
                gen = s_gen;
            }
            if (!MainThread::IsInstalled()) {   // no main-thread road (should not happen in a live game): never a silent empty index
                spdlog::error("[errand] vendor index: the main-thread road is not installed -- the index cannot be built; vendor shops are not located");
                std::scoped_lock lk(s_mx);
                if (gen == s_gen) s_state = IndexState::Built;   // built EMPTY, said loudly above
                return true;
            }
            MainThread::Post([gen]() { BuildIndex(gen); });
            return false;
        }
    }

    bool IsMember(RE::Actor* a_actor, RE::FormID a_faction) {
        if (!a_actor || !a_faction) return false;
        std::optional<std::int8_t> rank;
        a_actor->VisitFactions([&](RE::TESFaction* f, std::int8_t r) {
            if (f && f->GetFormID() == a_faction) rank = r;   // the later entry (a runtime change) wins
            return false;
        });
        return rank && *rank >= 0;
    }

    const char* AnswerName(Answer a_a) {
        switch (a_a) {
        case Answer::Located:   return "located";
        case Answer::Pending:   return "the vendor index is being built";
        case Answer::NoMembers: return "the faction has no member actor in memory";
        case Answer::AllGone:   return "every member is dead or disabled";
        case Answer::NoCell:    return "no living member has a parent cell to walk to";
        }
        return "?";
    }

    Answer Locate(RE::FormID a_faction, Where& a_out) {
        a_out = Where{};
        if (!EnsureIndex()) return Answer::Pending;
        std::vector<RE::FormID> ids;
        {
            std::scoped_lock lk(s_mx);
            if (const auto it = s_index.find(a_faction); it != s_index.end()) ids = it->second;
        }
        // Best first: loaded and awake, then loaded asleep, then unloaded (the shop's people wherever the engine has them now).
        int bestRank = -1;
        for (const auto id : ids) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDeleted() || a->IsPlayerTeammate()) continue;
            if (!IsMember(a, a_faction)) continue;   // removed from the faction since the index was built
            ++a_out.members;
            if (a->IsDead()) { ++a_out.dead; continue; }
            if (a->IsDisabled()) { ++a_out.disabled; continue; }
            auto* cell = a->GetParentCell();
            if (!cell) { ++a_out.noCell; continue; }
            const bool loaded = a->Is3DLoaded();
            const bool asleep = loaded && Asleep(a);
            const int rank = loaded ? (asleep ? 1 : 2) : 0;
            if (rank <= bestRank) continue;
            bestRank = rank;
            a_out.actor = id;
            a_out.name = detail::NameOf(a);
            a_out.loaded = loaded;
            a_out.asleep = asleep;
            a_out.cell = cell->GetFormID();
            a_out.interior = cell->IsInteriorCell();
            a_out.pos = a->GetPosition();
            const RE::BGSLocation* loc = a_out.interior ? cell->GetLocation() : nullptr;
            if (!loc) loc = a->GetCurrentLocation();
            if (!loc) loc = a->GetEditorLocation();
            a_out.town = LocationTypes::TownAnchor(loc);
        }
        if (a_out.actor) return Answer::Located;
        if (a_out.members == 0) return Answer::NoMembers;
        if (a_out.dead + a_out.disabled == a_out.members) return Answer::AllGone;
        return Answer::NoCell;
    }

    RE::NiPointer<RE::Actor> LoadedVendor(RE::FormID a_actor, RE::FormID a_faction, RE::Actor* a_near) {
        auto* a = a_actor ? RE::TESForm::LookupByID<RE::Actor>(a_actor) : nullptr;
        if (!a || !a_near || a->IsDead() || a->IsDisabled() || !a->Is3DLoaded() || Asleep(a) || !IsMember(a, a_faction)) return {};
        auto* vc = a->GetParentCell();
        auto* fc = a_near->GetParentCell();
        if (!vc || !fc) return {};
        if (vc != fc && (vc->IsInteriorCell() || fc->IsInteriorCell())) return {};   // the same cell, or both in the exterior grid
        return RE::NiPointer<RE::Actor>(a);
    }

    std::string Describe(const Where& a_w) {
        return std::format("'{}' ({:08X}) {} in cell {:08X} ({}) at {:.0f},{:.0f},{:.0f}; members {}, dead {}, disabled {}", a_w.name, a_w.actor,
                           a_w.loaded ? (a_w.asleep ? "LOADED, asleep" : "LOADED") : "UNLOADED", a_w.cell, a_w.interior ? "interior" : "exterior",
                           a_w.pos.x, a_w.pos.y, a_w.pos.z, a_w.members, a_w.dead, a_w.disabled);
    }

    void ClearAll() {
        std::scoped_lock lk(s_mx);
        s_index.clear();
        s_state = IndexState::Unbuilt;
        ++s_gen;
    }
}
