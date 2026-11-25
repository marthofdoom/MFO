#include "PCH.h"
#include "iaus/AreaActors.h"
#include "iaus/AreaCensus.h"         // kAreaGuard
#include "CombatSense.h"             // BodyRadius (field reads only)
#include "EngageOnSight.h"            // FightingParty: the one "is he fighting the party" reader
#include "MainThread.h"
#include "logistics/InvSnapshot.h"   // the one worker-tick clock every main-built table counts freshness in

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// iaus/AreaActors.cpp -- see AreaActors.h. INVARIANTS #84.
namespace MFO::AreaActors {

    namespace {

        // ~8 s since the last ask. A caster's slowest asker is one LAP (party size x 133 ms, about 1.3 s for ten followers).
        constexpr std::uint64_t kWantTtlTicks = 60;
        constexpr std::size_t   kMaxCasters   = 64;   // bits of Row::hostileTo

        struct Row {
            Iaus::AreaActor a;
            std::uint64_t   hostileTo = 0;   // bit i = hostile to Table::casters[i]
        };
        struct Table {
            std::vector<Row>        rows;
            std::vector<RE::FormID> casters;
            std::uint64_t           builtTick = 0, builtAge = 0;   // InvSnap::TickClock at the build
        };
        std::mutex                                    g_mx;   // LEAF: never held across an engine call
        std::shared_ptr<const Table>                  g_table;
        std::unordered_map<RE::FormID, std::uint64_t> g_wants;   // caster -> the last tick he asked
        std::atomic<std::uint64_t> g_asks{ 0 }, g_ready{ 0 }, g_missNever{ 0 }, g_missOld{ 0 }, g_missCaster{ 0 }, g_builds{ 0 }, g_usTotal{ 0 }, g_usMax{ 0 };
        std::atomic<std::uint64_t> g_lastRows{ 0 }, g_lastCasters{ 0 };
        std::atomic<bool>          g_overCap{ false };

        // The player's own animals: ExtraOwnership names the player's base NPC (0x7), as SetActorOwner(PlayerRef.GetActorBase()) writes it (the test Harbinger's
        // cast guard Bystander() makes, marth 2026-11-02 "Bystanders only"). Field reads on the main thread.
        constexpr RE::FormID kPlayerBase = 0x7;
        bool OwnedByPlayer(RE::Actor* a_a) {
            const auto* own = a_a->extraList.GetByType<RE::ExtraOwnership>();
            return own && own->owner && own->owner->GetFormID() == kPlayerBase;
        }
        // A summon commanded by the player, a teammate or one of the askers (M3 F2): the party's own.
        bool PartyCommanded(RE::Actor* a_a, const std::vector<RE::NiPointer<RE::Actor>>& a_casters) {
            const auto cmd = a_a->GetCommandingActor();   // HOLD the NiPointer
            if (!cmd) return false;
            if (cmd->IsPlayerRef() || cmd->IsPlayerTeammate()) return true;
            return std::any_of(a_casters.begin(), a_casters.end(), [&](const auto& c) { return c.get() == cmd.get(); });
        }

        Iaus::AreaKind KindOf(RE::Actor* a_a, const std::vector<RE::NiPointer<RE::Actor>>& a_casters) {
            if (OwnedByPlayer(a_a)) return Iaus::AreaKind::PlayerHorse;
            if (a_a->IsCommandedActor()) return PartyCommanded(a_a, a_casters) ? Iaus::AreaKind::PartySummon : Iaus::AreaKind::Commanded;
            if (a_a->IsAMount()) return Iaus::AreaKind::Mount;
            if (a_a->IsPlayerTeammate()) return Iaus::AreaKind::Teammate;
            if (a_a->IsGuard()) return Iaus::AreaKind::Guard;
            return Iaus::AreaKind::Person;
        }

    }

    void Want(RE::FormID a_caster) {
        if (!a_caster || !MainThread::IsInstalled()) return;
        const auto now = InvSnap::CurrentTick();
        std::scoped_lock lk(g_mx);
        g_wants[a_caster] = now;
    }

    Iaus::AreaPicture Read(RE::FormID a_caster, std::vector<Iaus::AreaActor>& a_out) {
        a_out.clear();
        Want(a_caster);
        g_asks.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<const Table> t;
        {
            std::scoped_lock lk(g_mx);
            t = g_table;
        }
        const auto clk = InvSnap::Now();
        if (!t) { g_missNever.fetch_add(1, std::memory_order_relaxed); return Iaus::AreaPicture::NotReady; }
        if (!InvSnap::FreshEnough(clk, t->builtTick, t->builtAge, Iaus::kAreaGuard.maxAgeTicks)) {
            g_missOld.fetch_add(1, std::memory_order_relaxed);
            return Iaus::AreaPicture::NotReady;
        }
        const auto it = std::find(t->casters.begin(), t->casters.end(), a_caster);
        if (it == t->casters.end()) { g_missCaster.fetch_add(1, std::memory_order_relaxed); return Iaus::AreaPicture::NotReady; }
        const std::uint64_t bit = std::uint64_t{ 1 } << static_cast<std::size_t>(it - t->casters.begin());
        a_out.reserve(t->rows.size());
        for (const Row& r : t->rows)
            if (r.a.id != a_caster && (r.hostileTo & bit) == 0) a_out.push_back(r.a);
        g_ready.fetch_add(1, std::memory_order_relaxed);
        return Iaus::AreaPicture::Ready;
    }

    // MAIN THREAD.
    void BuildPass(std::uint64_t) {
        const auto clk = InvSnap::Now();
        std::vector<RE::FormID> wanted;
        {
            std::scoped_lock lk(g_mx);
            std::erase_if(g_wants, [&](const auto& kv) { return clk.tick > kv.second + kWantTtlTicks; });
            if (g_wants.empty()) { g_table.reset(); return; }   // nobody fights: nothing to list
            for (const auto& kv : g_wants) wanted.push_back(kv.first);
            // Not due yet, by the one refresh rule; a caster the standing table does not know makes it due.
            if (g_table && clk.age - g_table->builtAge < Iaus::kAreaGuard.refreshTicks && g_table->builtTick >= clk.lastPaused &&
                std::all_of(wanted.begin(), wanted.end(), [](RE::FormID id) {
                    return std::find(g_table->casters.begin(), g_table->casters.end(), id) != g_table->casters.end(); }))
                return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::sort(wanted.begin(), wanted.end());
        auto t = std::make_shared<Table>();
        t->builtTick = clk.tick;
        t->builtAge  = clk.age;
        std::vector<RE::NiPointer<RE::Actor>> casters;   // HOLD them across the walk
        for (const auto id : wanted) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDeleted()) continue;   // not in the table: his reads answer NotReady (counted)
            if (t->casters.size() >= kMaxCasters) {
                if (!g_overCap.exchange(true))
                    spdlog::error("[area-guard] more than {} followers ask for the area picture: the rest stay NotReady, their area spells score 0", kMaxCasters);
                break;
            }
            t->casters.push_back(id);
            casters.emplace_back(a);
        }
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            std::unordered_set<RE::FormID> seen;
            // F3: where the askers stand. An actor farther than kAreaGuard.cullRange from EVERY one of them cannot be in any area they ask about, so he is not
            // listed and IsHostileToActor is never called for him.
            std::vector<RE::NiPoint3> from;
            for (const auto& c : casters) from.push_back(c->GetPosition());
            const float cull2 = Iaus::kAreaGuard.cullRange * Iaus::kAreaGuard.cullRange;
            const auto add = [&](RE::Actor* a) {
                if (!a || a->IsDeleted() || a->IsDisabled() || a->IsDead() || !a->Is3DLoaded()) return;
                const auto p = a->GetPosition();
                if (!std::any_of(from.begin(), from.end(), [&](const RE::NiPoint3& c) {
                        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
                        return !(dx * dx + dy * dy + dz * dz > cull2); }))   // NaN stays listed: an unreadable position is never "far"
                    return;
                if (!seen.insert(a->GetFormID()).second) return;
                Row r;
                r.a.id = a->GetFormID();
                r.a.x = p.x; r.a.y = p.y; r.a.z = p.z;
                r.a.radius = CombatSense::BodyRadius(a);
                r.a.kind = KindOf(a, casters);
                // F1 (the crime hole): an actor with a CRIME FACTION who is not fighting the party is somebody whose being hit is a crime, whatever the engine's
                // "would attack" answer says (a bounty, a vampire or a werewolf names every guard and townsperson hostile). He is listed as hostile to NOBODY,
                // so every asker reads him as a bystander (the rule EngageOnSight::IsEnemy applies to a fight it would start).
                r.a.crimeNotFighting = a->GetCrimeFaction() && !EngageOnSight::FightingParty(a);
                if (!r.a.crimeNotFighting)
                    for (std::size_t i = 0; i < casters.size(); ++i)
                        if (casters[i].get() != a && a->IsHostileToActor(casters[i].get())) r.hostileTo |= std::uint64_t{ 1 } << i;   // asked FROM the actor, as Gather's brawl gate does
                t->rows.push_back(r);
            };
            for (auto* list : { &pl->highActorHandles, &pl->middleHighActorHandles })
                for (auto& h : *list) {
                    auto a = h.get();   // HOLD the NiPointer
                    add(a.get());
                }
            add(RE::PlayerCharacter::GetSingleton());   // never in the process lists
        } else {
            return;   // no list = no table: the standing one ages out and the guard closes
        }
        const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        g_usTotal.fetch_add(us, std::memory_order_relaxed);
        auto worst = g_usMax.load(std::memory_order_relaxed);
        while (us > worst && !g_usMax.compare_exchange_weak(worst, us, std::memory_order_relaxed)) {}
        g_builds.fetch_add(1, std::memory_order_relaxed);
        g_lastRows.store(t->rows.size(), std::memory_order_relaxed);
        g_lastCasters.store(t->casters.size(), std::memory_order_relaxed);
        std::scoped_lock lk(g_mx);
        g_table = std::move(t);
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_table.reset();
        g_wants.clear();
    }

    void LogStats() {
        const auto builds = g_builds.load(std::memory_order_relaxed);
        spdlog::info("[area-guard] since start: asks={} ready={} notReady(never built={} too old={} caster not in it={}) | builds={} avg={} us worst={} us "
                     "(last: {} actors, {} casters)",
                     g_asks.load(std::memory_order_relaxed), g_ready.load(std::memory_order_relaxed), g_missNever.load(std::memory_order_relaxed),
                     g_missOld.load(std::memory_order_relaxed), g_missCaster.load(std::memory_order_relaxed), builds,
                     builds ? g_usTotal.load(std::memory_order_relaxed) / builds : 0, g_usMax.load(std::memory_order_relaxed),
                     g_lastRows.load(std::memory_order_relaxed), g_lastCasters.load(std::memory_order_relaxed));
    }

}
