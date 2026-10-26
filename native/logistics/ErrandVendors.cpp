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
// mfo-273 (marth 2026-10-26, the Ahlam field case: a vanilla REPLACEMENT vendor of Arcadia's shop was picked while Arcadia lived): "a vendor counts
// only if they would trade with the player now / they will have the dialogue". Applied ONLY while the shop is OPEN (VENV hours): "who is the vendor"
// stays independent of "is he trading now", so a closed shop still reaches the reopen watch. For an open shop a member counts when:
//   (a) SHOP-RELATIVE JobMerchantFaction (Skyrim.esm 00051596): every vanilla merchant is rank -1 in its base record; rank 0 is granted at runtime
//       (e.g. the DialogueWhiterun quest's alias faction list, ALFC) to the town's merchants. So rank >= 0 is required ONLY when at least one member of
//       the shop faction (dead ones included) holds it; vendors that never get it (Dawnguard, Enthir, Urag, quartermasters, modded ones) are not
//       dropped. The sets are computed on the MAIN thread at the index build (BuildIndex: s_jmActors / s_jmShops), read by the worker under s_mx.
//   (b) LOADED: condition function 255 GetOffersServicesNow evaluated on the MAIN thread through the verified TradePerks::ConditionTrue seat
//       (EvaluateOnMain; subject = the member, target = the player), cached per member (kVerdictTtl; a stale answer is served while a posted
//       refresh is in flight; a verdict CHANGE is logged). DISASSEMBLY (1.6.1170, condition fn 0x1403310a0 -> 0x67e270): TRUE iff the Actor
//       (form type 0x3E) has a vendorFaction (+0x168, recomputed by Actor::CalculateVendorFaction 0x66b650 with each faction's VENV hours and
//       vendorConditions) whose VEND list is non-null.
//   (c) UNLOADED (a persistent ref): function 255 is not trusted off screen, so hours only: the shop being open IS the verdict.
// Members that fail are skipped. No member passing an OPEN shop = Answer::NotOffering (not a candidate this plan; the next plan asks again).
// Never the nearest loaded member.
//
// Threading: Locate / IsMember / LoadedVendor run on the AddTask worker (the planner) and read actors the way the planner already does
// (LookupByID, parent cell, position, VisitFactions). The index build runs on the main thread. Own mutex (a leaf: never held across an
// engine call; Invalidate is taken under Errand g_mx at the town change, and nothing here ever takes Errand g_mx).

#include "PCH.h"
#include "Errand_internal.h"
#include "LocationTypes.h"
#include "MainThread.h"
#include "TradePerks.h"

#include <chrono>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Vendors {

    namespace {
        enum class IndexState { Unbuilt, Building, Built };
        std::mutex                                                s_mx;
        IndexState                                                s_state = IndexState::Unbuilt;
        std::uint32_t                                             s_gen = 0;   // bumped by ClearAll: a build that lands after a load is dropped
        std::unordered_map<RE::FormID, std::vector<RE::FormID>>  s_index;     // vendor faction -> member actor refs
        std::chrono::steady_clock::time_point                     s_builtAt{};  // review F1: when the standing index was built (the NoMembers rebuild pacing)
        constexpr auto kRebuildMinSec = std::chrono::seconds(60);

        // mfo-273: the vendor-dialogue gate. JobMerchantFaction (Skyrim.esm 00051596) is what the vanilla OfferServicesTopic INFO tests with
        // GetInFaction beside GetOffersServicesNow (diag-v272: base record rank -1 for every vanilla merchant; the town's dialogue quest alias grants rank 0 at runtime).
        constexpr RE::FormID kJobMerchantFaction = 0x00051596;
        constexpr auto kVerdictTtl = std::chrono::seconds(15);   // a member's offering state moves with the hour / a quest stage; plans run every few seconds
        struct Verdict { bool offers = false; bool refused = false; bool known = false; std::chrono::steady_clock::time_point at{}; };
        std::unordered_map<RE::FormID, Verdict>                   s_verdict;     // loaded member actor -> function 255 answer (guarded by s_mx)
        std::unordered_set<RE::FormID>                            s_verdictPending;
        std::unordered_set<RE::FormID>                            s_jmActors;    // index build (main thread): actors holding JobMerchantFaction rank >= 0
        std::unordered_set<RE::FormID>                            s_jmShops;     // ...and the vendor factions with at least one such member (dead included)
        std::unordered_map<RE::FormID, std::string>               s_verdictSaid; // actor -> the last logged verdict text (a CHANGE is logged)

        void SayVerdict(RE::FormID a_actor, const std::string& a_text) {
            {
                std::scoped_lock lk(s_mx);
                auto& said = s_verdictSaid[a_actor];
                if (said == a_text) return;
                said = a_text;
            }
            spdlog::info("[errand] vendor {:08X}: {}", a_actor, a_text);
        }

        // MAIN THREAD. GetOffersServicesNow (function 255) for the member, target the player. The condition node is built on the stack (the
        // engine only reads it): CommonLib's constructors are plain field init and TESCondition's destructor would free engine nodes.
        void EvaluateOnMain(RE::FormID a_actor, std::uint32_t a_gen) {
            Verdict v;
            v.known = true;
            v.at = std::chrono::steady_clock::now();
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            std::string note;
            if (!a || !pc || !a->Is3DLoaded()) {
                v.refused = true; note = "member / player not resolvable or no longer loaded";
            } else {
                alignas(RE::TESConditionItem) unsigned char buf[sizeof(RE::TESConditionItem)]{};
                auto* item = reinterpret_cast<RE::TESConditionItem*>(buf);
                item->data.comparisonValue.f = 1.0f;
                item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetOffersServicesNow;
                item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
                item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
                struct CondHead { RE::TESConditionItem* head; } head{ item };
                static_assert(sizeof(CondHead) == sizeof(RE::TESCondition));
                if (const auto r = TradePerks::ConditionTrue(*reinterpret_cast<const RE::TESCondition*>(&head), a, pc)) {
                    v.offers = *r;
                    note = std::format("GetOffersServicesNow {}", *r ? "TRUE" : "FALSE");
                } else {
                    v.refused = true; note = "TESCondition::IsTrue seat REFUSED (see the [trade] line)";
                }
            }
            bool stale = false;
            {
                std::scoped_lock lk(s_mx);
                stale = a_gen != s_gen;
                if (!stale) s_verdict[a_actor] = v;
                s_verdictPending.erase(a_actor);
            }
            if (!stale) SayVerdict(a_actor, std::format("loaded vendor-faction member: {} -> {}", note, !v.refused && v.offers ? "counts" : "does NOT count"));
        }

        enum class Counts { Yes, No, Pending };
        // WORKER. Does the LOADED member pass function 255? Pending = no answer yet (an evaluation was posted).
        Counts LoadedCounts(RE::FormID a_actor) {
            const auto now = std::chrono::steady_clock::now();
            Verdict v;
            bool post = false;
            std::uint32_t gen = 0;
            {
                std::scoped_lock lk(s_mx);
                gen = s_gen;
                if (const auto it = s_verdict.find(a_actor); it != s_verdict.end()) v = it->second;
                if ((!v.known || now - v.at >= kVerdictTtl) && MainThread::IsInstalled() && s_verdictPending.insert(a_actor).second) post = true;
            }
            if (post) MainThread::Post([a_actor, gen] { EvaluateOnMain(a_actor, gen); });
            if (!v.known) return MainThread::IsInstalled() ? Counts::Pending : Counts::No;   // no main-thread road: never a silent pass
            return v.refused || !v.offers ? Counts::No : Counts::Yes;
        }

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
            std::unordered_set<RE::FormID> jmActors, jmShops;
            for (const auto id : actors) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || a->IsPlayerRef() || a->IsDeleted()) continue;
                facs.clear();
                VendorMemberships(a, facs);
                if (facs.empty()) continue;
                ++vendors;
                const bool jm = IsMember(a, kJobMerchantFaction);   // mfo-273: shop-relative gate input (dead members included)
                if (jm) jmActors.insert(id);
                for (const auto f : facs) { idx[f].push_back(id); if (jm) jmShops.insert(f); }
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::scoped_lock lk(s_mx);
            if (a_gen != s_gen) return;   // a load / revert ran meanwhile: this index describes the previous session
            s_index = std::move(idx);
            s_jmActors = std::move(jmActors);
            s_jmShops = std::move(jmShops);
            s_state = IndexState::Built;
            s_builtAt = std::chrono::steady_clock::now();
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
        case Answer::Pending:   return "the vendor index or a vendor's trading verdict is being built";
        case Answer::NoMembers: return "the faction has no member actor in memory";
        case Answer::AllGone:   return "every member is dead or disabled";
        case Answer::NoCell:    return "no living member has a parent cell to walk to";
        case Answer::NotOffering: return "no member would trade with the player now (the shop is closed for this plan)";
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
        // mfo-273: the offering gate applies to an OPEN shop only (a closed shop keeps its vendors, for the reopen watch).
        auto* shopFac = RE::TESForm::LookupByID<RE::TESFaction>(a_faction);
        auto* cal = RE::Calendar::GetSingleton();
        const bool gate = shopFac && cal && detail::OpenAtHour(shopFac, cal->GetHour());
        bool shopHasJm = false;
        if (gate) { std::scoped_lock lk(s_mx); shopHasJm = s_jmShops.contains(a_faction); }
        // Best first: loaded and awake, then loaded asleep, then unloaded (the shop's people wherever the engine has them now).
        int bestRank = -1;
        int pendingVerdicts = 0;
        for (const auto id : ids) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDeleted() || a->IsPlayerTeammate()) continue;
            // review F4: an unloaded TEMPORARY actor may be torn down with its cell at any moment; only a loaded one or a PERSISTENT ref
            // (which stays in memory with its cell unloaded) is read from the worker.
            if (!a->Is3DLoaded() && !a->IsPersistent()) continue;
            if (!IsMember(a, a_faction)) continue;   // removed from the faction since the index was built
            ++a_out.members;
            if (a->IsDead()) { ++a_out.dead; continue; }
            if (a->IsDisabled()) { ++a_out.disabled; continue; }
            auto* cell = a->GetParentCell();
            if (!cell) { ++a_out.noCell; continue; }
            const bool loaded = a->Is3DLoaded();
            // mfo-273: would this member trade with the player now? (open shops only; see the file header)
            if (gate) {
                bool jm = true;
                if (shopHasJm) { std::scoped_lock lk(s_mx); jm = s_jmActors.contains(id); }
                if (!jm) {
                    ++a_out.notOffering;
                    SayVerdict(id, "the shop's vendors hold JobMerchantFaction and this member does not -> no vendor dialogue, does NOT count");
                    continue;
                }
                if (loaded) {
                    const auto c = LoadedCounts(id);
                    if (c == Counts::Pending) { ++pendingVerdicts; continue; }
                    if (c == Counts::No) { ++a_out.notOffering; continue; }
                } else {
                    SayVerdict(id, "UNLOADED member: the shop is open (hours only off screen) -> counts");
                }
            }
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
        if (pendingVerdicts > 0) { a_out.actor = 0; return Answer::Pending; }   // mfo-273: a loaded member's offering verdict is in flight; re-plan shortly
        if (a_out.actor) return Answer::Located;
        if (a_out.members == 0) {
            // review F1: nobody of this faction in the snapshot. A vendor may have come into memory since the build (a temporary actor in a
            // cell loaded later): re-arm a rebuild, at most once per kRebuildMinSec (the caller walks to the shop's place meanwhile).
            bool rearm = false;
            {
                std::scoped_lock lk(s_mx);
                if (s_state == IndexState::Built && std::chrono::steady_clock::now() - s_builtAt >= kRebuildMinSec) {
                    s_state = IndexState::Unbuilt;
                    ++s_gen;
                    rearm = true;
                }
            }
            if (rearm) spdlog::info("[errand] vendor index: faction {:08X} has no member in it -- rebuild re-armed (at most once per {} s)", a_faction, kRebuildMinSec.count());
            return Answer::NoMembers;
        }
        if (a_out.dead + a_out.disabled == a_out.members) return Answer::AllGone;
        if (a_out.notOffering > 0) return Answer::NotOffering;
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

    void Invalidate(const char* a_why) {
        {
            std::scoped_lock lk(s_mx);
            if (s_state == IndexState::Unbuilt) return;
            s_state = IndexState::Unbuilt;   // a build in flight lands into the old generation and is dropped
            ++s_gen;
        }
        spdlog::info("[errand] vendor index invalidated ({}) -- rebuilt on the next plan", a_why);
    }

    void ClearAll() {
        std::scoped_lock lk(s_mx);
        s_index.clear();
        s_verdict.clear();
        s_verdictPending.clear();
        s_verdictSaid.clear();
        s_jmActors.clear();
        s_jmShops.clear();
        s_state = IndexState::Unbuilt;
        ++s_gen;
    }
}
