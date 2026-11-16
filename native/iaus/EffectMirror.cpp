#include "PCH.h"
#include "EffectMirror.h"
#include "MainThread.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "logistics/InvSnapshot.h"   // CurrentTick: the one worker-tick counter both snapshots count freshness in

// iaus/EffectMirror.cpp -- see EffectMirror.h. INVARIANTS #84.
namespace MFO::EffectMirror {

    namespace {

        // Counted in worker ticks (133 ms), like InvSnapshot.cpp. Every mirrored actor is rebuilt every pass, so a readable mirror is
        // normally one tick old; four = three missed passes = the main thread is stalled or the game is paused.
        constexpr std::uint64_t kMaxAgeTicks  = 4;
        constexpr std::uint64_t kWantTtlTicks = 112;   // ~15 s since the last ask (an asker's cadence is one LAP, party size x 133 ms)

        struct Slot {
            Ptr           mirror;
            std::uint64_t builtTick = 0;
            std::uint64_t builtAge = 0;     // InvSnap::TickClock::age at the build (unpaused ticks)
            std::uint64_t wantedTick = 0;   // 0 = never asked by name (present only because he is in the high process list)
            std::uint64_t partyTick = 0;    // the last tick WantParty named him (the party's rule applies while it is recent)
            std::uint64_t builtPass = 0;    // the post tick of the pass that published the mirror
        };
        std::mutex                           g_mx;   // LEAF
        std::unordered_map<RE::FormID, Slot> g_slots;
        std::atomic<std::uint64_t> g_asks{ 0 }, g_hits{ 0 }, g_missNever{ 0 }, g_missOld{ 0 }, g_live{ 0 }, g_builds{ 0 }, g_passes{ 0 };
        std::atomic<std::uint64_t> g_lastPassActors{ 0 };

        // Under g_mx. The party's rule for a slot WantParty named this tick or the last, the age rule for anyone else.
        bool SlotReadable(const Slot& a_s, const InvSnap::TickClock& a_clk) {
            if (!a_s.mirror) return false;
            if (a_s.partyTick != 0 && a_s.partyTick + 1 >= a_clk.tick) return InvSnap::PassReached(a_clk, true, a_s.builtPass);
            return InvSnap::FreshEnough(a_clk, a_s.builtTick, a_s.builtAge, kMaxAgeTicks);
        }

        Ptr Ask(RE::FormID a_id) {
            const auto clk = InvSnap::Now();
            g_asks.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk(g_mx);
            auto& s = g_slots[a_id];
            s.wantedTick = clk.tick;
            if (!s.mirror) { g_missNever.fetch_add(1, std::memory_order_relaxed); return nullptr; }
            if (!SlotReadable(s, clk)) { g_missOld.fetch_add(1, std::memory_order_relaxed); return nullptr; }
            g_hits.fetch_add(1, std::memory_order_relaxed);
            return s.mirror;
        }

    }

    bool Mirror::HasMagicEffect(const RE::EffectSetting* a_base) const {
        return a_base && std::binary_search(engineHas.begin(), engineHas.end(), a_base);
    }

    Ptr BuildLive(RE::Actor* a_actor) {
        if (!a_actor) return nullptr;
        auto m   = std::make_shared<Mirror>();
        m->actor = a_actor->GetFormID();
        auto* mt   = a_actor->AsMagicTarget();
        auto* list = mt ? mt->GetActiveEffectList() : nullptr;
        if (!list) return m;
        m->hasList = true;
        for (auto* ae : *list) {
            if (!ae) continue;
            Fx f;
            f.spell     = ae->spell;
            f.base      = ae->effect ? ae->effect->baseEffect : nullptr;
            f.inactive  = ae->flags.any(RE::ActiveEffect::Flag::kInactive);
            f.dispelled = ae->flags.any(RE::ActiveEffect::Flag::kDispelled);
            f.duration  = ae->duration;
            f.elapsed   = ae->elapsedSeconds;
            f.magnitude = ae->magnitude;
            RE::ActorHandle h{};
            if (auto* se = skyrim_cast<RE::SummonCreatureEffect*>(ae))   { f.summon = true;    h = se->commandedActor; }
            else if (auto* re = skyrim_cast<RE::ReanimateEffect*>(ae))   { f.reanimate = true; h = re->commandedActor; }
            if (f.summon || f.reanimate) {
                if (auto cmd = h.get()) {   // HOLD the NiPointer across the reads
                    f.commandedResolved = true;
                    f.commandedLive     = !cmd->IsDead() && !cmd->IsDeleted();
                    f.commandedId       = cmd->GetFormID();
                }
            }
            // MagicTarget::HasMagicEffect(base) is true exactly when some node of this same list (vslot 0x38 GetActiveEffectList) has
            // GetBaseObject() == base, no flag test (disassembled in all three executables, ENGINE_NOTES 0.30 amendment 2026-11-16).
            // So the engine's answers are the set of non-null bases this walk sees: no per-base engine call (review F7 of 35a8a778).
            if (f.base) m->engineHas.push_back(f.base);
            m->fx.push_back(f);
        }
        std::sort(m->engineHas.begin(), m->engineHas.end());
        m->engineHas.erase(std::unique(m->engineHas.begin(), m->engineHas.end()), m->engineHas.end());
        return m;
    }

    Ptr Get(RE::Actor* a_actor) {
        if (!a_actor) return nullptr;
        if (MainThread::LiveReadsAllowed()) {
            g_live.fetch_add(1, std::memory_order_relaxed);
            return BuildLive(a_actor);
        }
        return Ask(a_actor->GetFormID());
    }

    Ptr Get(RE::FormID a_actor) {
        if (!a_actor) return nullptr;
        if (MainThread::LiveReadsAllowed()) {
            g_live.fetch_add(1, std::memory_order_relaxed);
            return BuildLive(RE::TESForm::LookupByID<RE::Actor>(a_actor));
        }
        return Ask(a_actor);
    }

    bool Ready(RE::FormID a_actor) {
        if (!a_actor) return false;
        if (MainThread::LiveReadsAllowed()) return true;
        const auto clk = InvSnap::Now();
        std::scoped_lock lk(g_mx);
        auto& s = g_slots[a_actor];
        s.wantedTick = clk.tick;
        return SlotReadable(s, clk);
    }

    void Want(RE::FormID a_actor) {
        if (!a_actor || !MainThread::IsInstalled()) return;
        const auto now = InvSnap::CurrentTick();
        std::scoped_lock lk(g_mx);
        g_slots[a_actor].wantedTick = now;
    }

    void WantParty(RE::FormID a_actor) {
        if (!a_actor || !MainThread::IsInstalled()) return;
        const auto now = InvSnap::CurrentTick();
        std::scoped_lock lk(g_mx);
        auto& s = g_slots[a_actor];
        s.wantedTick = now;
        s.partyTick  = now;
    }

    // MAIN THREAD.
    void BuildPass(std::uint64_t a_postTick) {
        const auto clk    = InvSnap::Now();
        const auto a_tick = clk.tick;
        g_passes.fetch_add(1, std::memory_order_relaxed);

        std::vector<RE::FormID> wanted;
        {
            std::scoped_lock lk(g_mx);
            for (auto it = g_slots.begin(); it != g_slots.end();) {
                // Forget an actor nobody asked about for ~15 s and the high list no longer rebuilds (its mirror aged out).
                const bool asked = it->second.wantedTick != 0 && a_tick <= it->second.wantedTick + kWantTtlTicks;
                const bool fresh = it->second.mirror && InvSnap::FreshEnough(clk, it->second.builtTick, it->second.builtAge, kMaxAgeTicks);
                if (!asked && !fresh) { it = g_slots.erase(it); continue; }
                if (asked) wanted.push_back(it->first);
                ++it;
            }
        }
        if (wanted.empty()) { g_lastPassActors.store(0, std::memory_order_relaxed); return; }   // no party, no asker: nothing to mirror

        std::vector<std::pair<RE::FormID, Ptr>> built;
        std::unordered_set<RE::FormID> seen;
        const auto add = [&](RE::Actor* a_actor) {
            // (No IsDeleted refusal: the tick gate waits on a party member's mirror, and a flagged actor's list is still readable here.)
            if (!a_actor || !seen.insert(a_actor->GetFormID()).second) return;
            built.emplace_back(a_actor->GetFormID(), BuildLive(a_actor));
        };
        add(RE::PlayerCharacter::GetSingleton());
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            for (auto& h : pl->highActorHandles) {
                auto a = h.get();   // HOLD the NiPointer across the walk
                add(a.get());
            }
        }
        for (const auto id : wanted)
            if (!seen.contains(id)) add(RE::TESForm::LookupByID<RE::Actor>(id));
        g_builds.fetch_add(built.size(), std::memory_order_relaxed);
        g_lastPassActors.store(built.size(), std::memory_order_relaxed);

        std::scoped_lock lk(g_mx);
        for (auto& [id, m] : built) {
            auto& s     = g_slots[id];
            s.mirror    = std::move(m);
            s.builtTick = a_tick;
            s.builtAge  = clk.age;
            s.builtPass = a_postTick;
        }
    }

    void Clear() {
        std::scoped_lock lk(g_mx);
        g_slots.clear();
    }

    void LogStats() {
        spdlog::info("[fxmirror] since start: asks={} answered={} notYet(never built={} too old={}) liveReads(main)={} | passes={} mirrors built={} "
                     "(last pass {} actors)",
                     g_asks.load(std::memory_order_relaxed), g_hits.load(std::memory_order_relaxed),
                     g_missNever.load(std::memory_order_relaxed), g_missOld.load(std::memory_order_relaxed),
                     g_live.load(std::memory_order_relaxed), g_passes.load(std::memory_order_relaxed),
                     g_builds.load(std::memory_order_relaxed), g_lastPassActors.load(std::memory_order_relaxed));
    }

}
