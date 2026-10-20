// logistics/ErrandShops.cpp -- TOWN PHASE 2 R6 (ClickUp 86e3t1mx7): FENCES and SPOUSE STORES.
// Design: _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md round R6 / item 1.c, rulings Q1 + Q6 (TOWN-PHASE2-RULINGS.md).
//
// MFO decides and declares only: this file holds the catalog GATES (Errand.cpp's BuildCatalog asks them) and the STOLEN
// classification (Economy.cpp's sell rows ask it). No engine action, no trade code; the trade still goes through the existing
// Papyrus trade road. NO co-save change: a fence or spouse stop is an ordinary StopKind::Shop stop (the faction says what it is).
//
//   FENCE   a pure-fence faction (buysStolen && !buysNonStolen) is a candidate ONLY when BOTH hold (Q1):
//             (1) the FOLLOWER owns the fence perk (Config::g_fencePerkID, the base-perk dual check Economy.cpp uses for the
//                 Merchant perk -- followers hold perks on the base TESNPC), AND
//             (2) the faction's own vendorConditions pass (e.g. Tonilia: GetStageDone on the Thieves Guild quest = the fence is
//                 unlocked for the PLAYER). Evaluated with TradePerks::ConditionTrue (the self-checked TESCondition::IsTrue):
//                 action = the vendor actor when he is loaded, else the PLAYER (a quest-stage condition has no subject);
//                 target = the player. The design's "subject = the vendor actor when known, else the player".
//   SPOUSE  a conditioned NON-fence vendor faction is a candidate ONLY when the player's spouse -- the actor in
//           RelationshipMarriageFIN (QUST 00021382) alias 0 LoveInterest (R0-7), read with TradePerks::AliasRefHandle -- is a
//           member (rank >= 0) of that faction AND its conditions pass with the SPOUSE as the action subject (the ServicesSpouse*
//           factions carry GetInFaction(<job faction>) on the subject).
//   Every other conditioned vendor stays dropped (marth 2026-10-18, Q10), exactly as before.
//
// THREADING: the gates are called from the AddTask job worker (Errand.cpp BuildCatalog). TESCondition::IsTrue is MAIN-THREAD ONLY
// (TradePerks.h), so a gate never evaluates itself: it reads a session cache and, when the entry is missing or older than
// kRefresh, Posts ONE evaluation to the main thread and answers Pending (the catalog skips the shop this plan; the plan cadence
// retries). A stale entry is used while its refresh is in flight. The perk test and the alias read are data reads / the
// alias reader (any thread). The stolen classification reads the follower's inventory on the worker (the serial #4 domain, the
// same GetInventory the sell-row build already does). Session-only: nothing here is saved.
#include "PCH.h"
#include "Errand_internal.h"
#include "TradePerks.h"
#include "Config.h"
#include "MainThread.h"

#include <chrono>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MFO::Logistics::ErrandShops {

    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr RE::FormID kMarriageQuest  = 0x00021382;   // RelationshipMarriageFIN (Skyrim.esm), R0-7
        constexpr std::uint32_t kLoveInterestAlias = 0;      // alias 0 LoveInterest (R0-7; also the LoreRim winner)
        constexpr RE::FormID kPlayerFaction  = 0x00000DB1;
        constexpr auto kRefresh = std::chrono::seconds(20);   // a quest stage / a spouse's faction changes rarely; a stale answer is served meanwhile
        constexpr auto kLogEvery = std::chrono::seconds(120); // an unchanged decision is said at most this often per follower x vendor

        struct Entry { bool pass = false; bool refused = false; std::string note; Clock::time_point at{}; };
        std::mutex g_mx;
        std::unordered_map<std::uint64_t, Entry> g_cond;            // (faction << 32 | subject actor, 0 = the player) -> the condition result
        std::unordered_set<std::uint64_t> g_pending;                // evaluations queued on the main thread
        struct Said { std::string text; Clock::time_point at{}; };
        std::unordered_map<std::uint64_t, Said> g_said;             // (follower << 32 | faction) -> the last decision line
        std::unordered_set<RE::FormID> g_mixedSaid;                 // mixed stolen/normal bases said once per session
        bool g_noSpouseSaid = false;                                // "no spouse" said once per load
        bool g_aliasRefusedSaid = false;

        std::uint64_t Key(RE::FormID a_fac, RE::FormID a_subject) { return (static_cast<std::uint64_t>(a_fac) << 32) | a_subject; }

        // ONE decision line per follower x vendor: logged when it changes, otherwise at most every kLogEvery.
        void Say(RE::FormID a_follower, RE::FormID a_fac, const std::string& a_text) {
            const auto now = Clock::now();
            const auto k = Key(a_follower, a_fac);
            {
                std::scoped_lock lk(g_mx);
                auto& s = g_said[k];
                if (s.text == a_text && now - s.at < kLogEvery) return;
                s.text = a_text; s.at = now;
            }
            spdlog::info("[errand] {:08X}: {}", a_follower, a_text);
        }

        const char* FacName(const RE::TESFaction* a_fac) {
            const char* n = a_fac ? a_fac->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        bool HasPerk(RE::Actor* a_actor, RE::FormID a_perk) {
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(a_perk);
            if (!perk || !a_actor) return false;
            auto* base = a_actor->GetActorBase();
            return (base && base->GetPerkIndex(perk).has_value()) || a_actor->HasPerk(perk);   // the Economy.cpp Merchant-perk dual check
        }

        // MAIN THREAD. Evaluate the faction's vendorConditions and cache the answer.
        void EvaluateOnMain(RE::FormID a_fac, RE::ActorHandle a_subject) {
            RE::FormID subjectId = 0;
            Entry e;
            e.at = Clock::now();
            auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(a_fac);
            auto* pc  = RE::PlayerCharacter::GetSingleton();
            auto sp = a_subject.get();
            RE::TESObjectREFR* subject = pc;
            if (sp.get()) { subject = sp.get(); subjectId = sp.get()->GetFormID(); }
            if (!fac || !pc || !subject) {
                e.refused = true; e.note = "faction / player / subject not resolvable";
            } else if (!fac->vendorData.vendorConditions || !fac->vendorData.vendorConditions->head) {
                e.pass = true; e.note = "no conditions";   // an empty list is true, as in the engine
            } else if (const auto r = TradePerks::ConditionTrue(*fac->vendorData.vendorConditions, subject, pc)) {
                e.pass = *r;
                e.note = std::format("conditions {} with subject {:08X}{}", *r ? "TRUE" : "FALSE", subject->GetFormID(),
                                     subject == pc ? " (the player)" : "");
            } else {
                e.refused = true; e.note = "TESCondition::IsTrue seat REFUSED (see the [trade] line)";
            }
            std::scoped_lock lk(g_mx);
            g_cond[Key(a_fac, subjectId)] = std::move(e);
            g_pending.erase(Key(a_fac, subjectId));
        }

        // Any thread. Open / Closed from the cache, Pending while the first answer is being computed. a_why is the decision text.
        Gate ConditionGate(const RE::TESFaction* a_fac, RE::ActorHandle a_subject, std::string& a_why) {
            auto sp = a_subject.get();
            const RE::FormID subjectId = sp.get() ? sp.get()->GetFormID() : 0u;
            const auto k = Key(a_fac->GetFormID(), subjectId);
            const auto now = Clock::now();
            bool have = false, post = false;
            Entry e;
            {
                std::scoped_lock lk(g_mx);
                if (auto it = g_cond.find(k); it != g_cond.end()) { have = true; e = it->second; }
                if ((!have || now - e.at >= kRefresh) && MainThread::IsInstalled() && g_pending.insert(k).second) post = true;
            }
            if (post)
                MainThread::Post([fid = a_fac->GetFormID(), subj = a_subject] { EvaluateOnMain(fid, subj); });
            if (!have) {
                if (!MainThread::IsInstalled()) { a_why = "conditions cannot be evaluated (no main-thread road)"; return Gate::Closed; }
                a_why = "conditions being evaluated on the main thread";
                return Gate::Pending;
            }
            a_why = e.note;
            if (e.refused) return Gate::Closed;   // a refused query is "no answer", never "pass" (principle 7)
            return e.pass ? Gate::Open : Gate::Closed;
        }

        // The spouse actor, or nullptr (with a said-once reason). Any thread (the alias reader takes the quest's lock itself).
        RE::ActorHandle SpouseHandle() {
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(kMarriageQuest);
            if (!quest) return {};
            const auto h = TradePerks::AliasRefHandle(quest, kLoveInterestAlias);
            if (!h) {
                std::scoped_lock lk(g_mx);
                if (!g_aliasRefusedSaid) { g_aliasRefusedSaid = true;
                    spdlog::warn("[errand] spouse store: the alias reader seat was REFUSED -- spouse occupancy UNKNOWN, no spouse store is offered"); }
                return {};
            }
            auto ref = h->get();
            auto* actor = ref.get() ? ref.get()->As<RE::Actor>() : nullptr;
            if (!actor) {
                std::scoped_lock lk(g_mx);
                if (!g_noSpouseSaid) { g_noSpouseSaid = true;
                    spdlog::info("[errand] spouse store: {} -- no spouse store this load",
                                 *h ? "the LoveInterest reference is not resolvable" : "the LoveInterest alias is empty"); }
                return {};
            }
            return actor->GetHandle();
        }

        bool InFaction(RE::Actor* a_actor, const RE::TESFaction* a_fac) {
            bool in = false;
            a_actor->VisitFactions([&](RE::TESFaction* f, std::int8_t rank) {
                if (f == a_fac && rank >= 0) { in = true; return true; }
                return false;
            });
            return in;
        }

        // An item is STOLEN when a copy carries an owner that is neither the follower's side (the follower, the player, a faction
        // the follower belongs to, PlayerFaction) nor empty.
        bool StolenOwner(RE::TESForm* a_owner, RE::Actor* a_follower) {
            if (!a_owner) return false;
            const RE::FormID id = a_owner->GetFormID();
            if (id == 0x14 || id == a_follower->GetFormID() || id == kPlayerFaction) return false;
            if (auto* f = a_owner->As<RE::TESFaction>()) return !InFaction(a_follower, f);
            return true;
        }
    }

    bool IsFence(const RE::TESFaction* a_fac) {
        if (!a_fac) return false;
        const auto& vv = a_fac->vendorData.vendorValues;
        return vv.buysStolen && !vv.buysNonStolen;
    }

    bool IsFenceFaction(RE::FormID a_fac) {
        return a_fac && IsFence(RE::TESForm::LookupByID<RE::TESFaction>(a_fac));
    }

    Gate FenceGate(RE::Actor* a_follower, const RE::TESFaction* a_fac, RE::ActorHandle a_vendor) {
        const RE::FormID fid = a_follower->GetFormID();
        // (1) the follower's fence perk. Missing = not a candidate (the gate itself).
        if (!HasPerk(a_follower, Config::g_fencePerkID.load())) {
            Say(fid, a_fac->GetFormID(), std::format("fence '{}' ({:08X}): CLOSED -- he does not own the fence perk ({:08X})",
                                                    FacName(a_fac), a_fac->GetFormID(), Config::g_fencePerkID.load()));
            return Gate::Closed;
        }
        // (2) the fence's own conditions, evaluated against the player's progress (Q1).
        std::string why;
        const Gate g = ConditionGate(a_fac, a_vendor, why);
        Say(fid, a_fac->GetFormID(), std::format("fence '{}' ({:08X}): {} -- fence perk owned; {}", FacName(a_fac), a_fac->GetFormID(),
                                                g == Gate::Open ? "OPEN" : g == Gate::Pending ? "PENDING" : "CLOSED", why));
        return g;
    }

    Gate SpouseGate(RE::Actor* a_follower, const RE::TESFaction* a_fac, RE::ActorHandle a_vendor) {
        const RE::FormID fid = a_follower->GetFormID();
        const auto spouse = SpouseHandle();
        auto sp = spouse.get();
        auto* spouseActor = sp.get();
        if (!spouseActor) return Gate::Closed;   // the reason was said once per load
        // A loaded vendor must BE the spouse (another conditioned vendor is not a spouse store).
        if (auto vp = a_vendor.get(); vp.get() && vp.get() != spouseActor) return Gate::Closed;
        if (!InFaction(spouseActor, a_fac)) return Gate::Closed;   // not his shop (the other 24 ServicesSpouse* factions): no line, it is the gate
        std::string why;
        const Gate g = ConditionGate(a_fac, spouse, why);
        Say(fid, a_fac->GetFormID(), std::format("spouse store '{}' ({:08X}) of {:08X} '{}': {} -- {}", FacName(a_fac), a_fac->GetFormID(),
                                                spouseActor->GetFormID(), spouseActor->GetName() ? spouseActor->GetName() : "?",
                                                g == Gate::Open ? "OPEN" : g == Gate::Pending ? "PENDING" : "CLOSED", why));
        return g;
    }

    std::unordered_map<const RE::TESBoundObject*, Stolen> ClassifyStolen(RE::Actor* a_follower) {
        std::unordered_map<const RE::TESBoundObject*, Stolen> out;
        if (!a_follower) return out;
        for (auto& [obj, data] : a_follower->GetInventory()) {
            if (!obj || data.first <= 0 || !data.second) continue;
            std::int32_t stolen = 0;
            if (data.second->extraLists)
                for (auto* xl : *data.second->extraLists) {
                    if (!xl) continue;
                    auto* own = xl->GetByType<RE::ExtraOwnership>();
                    if (!own || !StolenOwner(own->owner, a_follower)) continue;
                    auto* cnt = xl->GetByType<RE::ExtraCount>();
                    stolen += cnt && cnt->count > 0 ? static_cast<std::int32_t>(cnt->count) : 1;
                }
            out[obj] = stolen <= 0 ? Stolen::None : stolen >= data.first ? Stolen::All : Stolen::Mixed;
        }
        return out;
    }

    void NoteMixed(RE::Actor* a_follower, const RE::TESBoundObject* a_obj) {
        if (!a_obj) return;
        {
            std::scoped_lock lk(g_mx);
            if (!g_mixedSaid.insert(a_obj->GetFormID()).second) return;
        }
        spdlog::info("[econ] {:08X}: '{}' ({:08X}) is carried partly stolen, partly not -- sold NOWHERE (RemoveItem cannot choose the copy)",
                     a_follower ? a_follower->GetFormID() : 0u, a_obj->GetName() ? a_obj->GetName() : "?", a_obj->GetFormID());
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_cond.clear();
        g_pending.clear();
        g_said.clear();
        g_mixedSaid.clear();
        g_noSpouseSaid = false;
        g_aliasRefusedSaid = false;
    }
}
