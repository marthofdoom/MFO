#include "PCH.h"
#include "MainThread.h"

namespace MFO::MainThread {

    namespace {

        // The queue crosses threads BY DESIGN -- workers Post, the player's
        // Update drains -- so it gets a real lock (INVARIANTS #4). Contention
        // is a handful of Posts per second against one swap per frame; a plain
        // mutex is plenty, and swap-then-run below keeps the critical section
        // to a pointer exchange.
        std::mutex g_mx;
        std::vector<std::function<void()>> g_queue;

        std::atomic<bool> g_installed{ false };

        // Set when Install() REFUSES (VR). Post() then drops fns instead of
        // queueing them forever into a queue nothing will ever drain.
        std::atomic<bool> g_dead{ false };

        struct PlayerUpdateHook {
            static void thunk(RE::PlayerCharacter* a_this, float a_delta) {
                // Original FIRST, always -- the pump piggybacks on the engine's
                // frame; it never runs ahead of the engine's own bookkeeping.
                // Same doctrine as Targeting's UpdateCombat thunk.
                func(a_this, a_delta);

                // SWAP-THEN-RUN: take the whole queue under the lock, run it
                // outside. A drained fn can then Post() again without
                // deadlocking on the mutex it just came out of -- the repost
                // simply waits for the next frame.
                std::vector<std::function<void()>> batch;
                {
                    std::lock_guard lk(g_mx);
                    if (g_queue.empty()) return;   // the every-frame path: one lock, no work
                    batch.swap(g_queue);
                }

                // One-time proof-of-life. The crash stacks put AddTask drains
                // on BSJobs::JobThread; this line prints from inside the
                // player's Update, so its presence in the log IS the evidence
                // that Post()ed work runs on the main thread.
                static bool s_first = true;   // main-thread-only, no atomic needed
                if (s_first) {
                    s_first = false;
                    spdlog::info("[mainthread] first drain: {} fn(s) ran inside player Update -- pump is live on the main thread",
                                 batch.size());
                }

                for (auto& fn : batch) {
                    if (fn) fn();
                }
            }

            static inline REL::Relocation<decltype(thunk)> func;

            // 0x0AD is Actor::Update(float a_delta) on SE/AE, read from the
            // PINNED CommonLibSSE-NG (vcpkg port 3.7.0 = CharmedBaryon/
            // CommonLibSSE @ c4ab853), include/RE/A/Actor.h:367:
            //   SKYRIM_REL_VR_VIRTUAL void Update(float a_delta);   // 0AD
            // -- NOT guessed, and NOT Targeting's 0xE4 (that is UpdateCombat).
            // VTABLE_PlayerCharacter[0] (include/RE/Offsets_VTABLE.h:2231) is
            // the primary, TESObjectREFR-lineage vtable, where inherited slots
            // keep Actor's indices; writing the slot there hooks ONLY the
            // player -- one call per frame, nothing per-NPC, unlike the
            // Character-wide UpdateCombat hook.
            static constexpr std::size_t idx = 0x0AD;   // Actor::Update(float)
        };

    }

    void Post(std::function<void()> a_fn) {
        if (!a_fn || g_dead.load(std::memory_order_relaxed)) return;
        std::lock_guard lk(g_mx);
        g_queue.push_back(std::move(a_fn));
    }

    void Clear() {
        std::lock_guard lk(g_mx);
        g_queue.clear();
    }

    void Install() {
        // SKYRIM_REL_VR_VIRTUAL: on VR the Update slot sits at a DIFFERENT
        // index, and writing 0x0AD there would vector every frame into an
        // arbitrary virtual -- an instant CTD pointing nowhere near MFO. Same
        // refusal, for the same reason, as Targeting's UpdateCombat hook.
        if (REL::Module::IsVR()) {
            g_dead = true;
            spdlog::warn("[mainthread] VR runtime detected -- Update vtable index is not "
                         "verified for VR; pump NOT installed (Post becomes a no-op).");
            return;
        }
        if (g_installed.exchange(true)) return;

        // A VTABLE INDEX, not an AddressLib offset -- version-resilient the
        // same way Targeting's hook is (§0.12's lesson).
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
        PlayerUpdateHook::func =
            vtbl.write_vfunc(PlayerUpdateHook::idx, PlayerUpdateHook::thunk);

        spdlog::info("[mainthread] player Update vfunc hook installed (PlayerCharacter vtbl idx 0x{:X})",
                     PlayerUpdateHook::idx);
    }

}
