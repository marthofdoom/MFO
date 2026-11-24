// [probe-light] (DEV BUILDS ONLY; movement-plan.md G0b item 2, gate P8): the engine's own cached light level of the player and the
// followers. Read only, log only.
// LAYOUT: AIProcess+0x10 = high, HighProcessData+0x3A8 = the level (engine getter id 39509 / 38499). The getter's address is a
// VerifiedAddresses row (Light.AIProcess.CachedLightLevel) AND its 22 live bytes are compared here before the first read, so the two
// offsets come from the running executable. 1.7.104 has no row (the getter is not in the id map): refused by name, nothing is read.
// Only an actor in HIGH process has a level; another one prints "no high process", never 0.
// THREADS: the worker collects FormIDs (#4) and posts; the read is on the MAIN thread, actors re-resolved by FormID.
#include "selftest/SelfTest_internal.h"
#include "Followers.h"
#include "MainThread.h"
#include "Runtime.h"

#include <cmath>
#include <cstring>

namespace MFO::SelfTest {

    namespace {
        static_assert(offsetof(RE::AIProcess, high) == 0x10, "AIProcess::high moved: re-verify [probe-light]");
        static_assert(offsetof(RE::HighProcessData, lightLevel) == 0x3A8, "HighProcessData::lightLevel moved: re-verify [probe-light]");

        constexpr float kPostEvery = 2.0f;    // the engine refreshes an NPC every 2 s
        constexpr float kMinChange = 2.0f;    // a line when any level moved this much ...
        constexpr auto  kMaxQuiet  = std::chrono::seconds(60);   // ... or this long passed
        float g_wait = 0.0f;   // worker

        // MAIN THREAD. Once: is the cached-level layout proven on this executable?
        bool LayoutVerified() {
            static const bool ok = [] {
                if (!Runtime::IsVerified1_6_1170() && !Runtime::IsVerified1_5_97()) {   // asked BEFORE any id is resolved (a missing id is fatal)
                    spdlog::warn("[probe-light] REFUSED: runtime {} has no verified light-level row (1.6.1170 and 1.5.97 only); no level is read",
                                 REL::Module::get().version().string("."));
                    return false;
                }
                const REL::Relocation<std::uintptr_t> getter{ REL::RelocationID(38499, 39509) };
                if (!Runtime::SeatVerified(getter.address(), "Light.AIProcess.CachedLightLevel")) return false;
                constexpr std::uint8_t want[22] = { 0x48, 0x8B, 0x41, 0x10, 0x48, 0x85, 0xC0, 0x74, 0x09, 0xF3, 0x0F, 0x10, 0x80, 0xA8, 0x03,
                                                    0x00, 0x00, 0xC3, 0x0F, 0x57, 0xC0, 0xC3 };
                const bool same = std::memcmp(reinterpret_cast<const void*>(getter.address()), want, sizeof(want)) == 0;
                if (same) spdlog::info("[probe-light] layout verified: the engine's getter reads AIProcess+0x10 (high) +0x3A8 on this executable");
                else spdlog::error("[probe-light] REFUSED: the light getter's bytes differ from the verified ones; no level is read");
                return same;
            }();
            return ok;
        }

        // MAIN THREAD.
        std::string Row(RE::Actor* a, float& a_level) {
            auto*       proc  = a->GetActorRuntimeData().currentProcess;
            const char* n     = a->GetName();
            const auto  head  = std::format("{:08X} '{}'", a->GetFormID(), n && *n ? n : "?");
            a_level           = -1.0f;
            if (!proc || !proc->high) return head + " no high process";
            a_level = proc->high->lightLevel;
            return std::format("{} {:.1f}{}", head, a_level, a->IsSneaking() ? " sneaking" : "");
        }

        // MAIN THREAD only.
        std::map<RE::FormID, float>                             g_last;
        std::chrono::steady_clock::time_point                   g_lastLine{};
    }

    void LightTick(float a_dt) {
        g_wait -= a_dt;
        if (g_wait > 0.0f) return;
        g_wait = kPostEvery;
        std::vector<RE::FormID> ids;
        for (const auto& h : Followers::g_active) {
            auto p = h.get();
            if (p && !p->IsDead()) ids.push_back(p->GetFormID());
        }
        MainThread::Post([ids]() {
            if (!LayoutVerified()) return;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc || !pc->Is3DLoaded()) return;
            bool                       changed = false;
            std::map<RE::FormID, float> now;
            std::string                text;
            const auto                 add = [&](RE::Actor* a) {
                float lv = -1.0f;
                text += (text.empty() ? "" : " | ") + Row(a, lv);
                now[a->GetFormID()] = lv;
                const auto it = g_last.find(a->GetFormID());
                changed = changed || it == g_last.end() || std::fabs(it->second - lv) >= kMinChange;
            };
            add(pc);
            for (const auto id : ids)
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && a->Is3DLoaded()) add(a);
            const auto t = std::chrono::steady_clock::now();
            if (!changed && t - g_lastLine < kMaxQuiet) return;
            g_lastLine       = t;
            g_last           = std::move(now);
            const auto* cell = pc->GetParentCell();
            spdlog::info("[probe-light] {} cell {:08X}: {}", cell && cell->IsInteriorCell() ? "INTERIOR" : "exterior",
                         cell ? cell->GetFormID() : 0u, text);
        });
    }
}
