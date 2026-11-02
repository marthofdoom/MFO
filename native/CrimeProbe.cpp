#include "PCH.h"
#include "CrimeProbe.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "Diagnostics.h"
#include "Runtime.h"
#include "logistics/Errand.h"

// See CrimeProbe.h. READ-ONLY EVERYWHERE.
//
// THE CRIME-GOLD READ (decided 2026-11-01): PlayerCharacter::GetViolentCrimeGoldValue / GetNonViolentCrimeGoldValue
// (CommonLib vfuncs 0x12B / 0x12C), NOT a walk of the crimeGoldMap member. Disassembled in
// binaries/1.6.1170/SkyrimSE.unpacked.exe (1.6.1170.0): the PlayerCharacter primary vtable is at RVA 0x18AB9C0
// (VerifiedAddresses row "MainThread.PlayerCharacter.Update", id 208040; slot 0xAD there is Actor::Update, the
// slot MainThread already hooks), slot 0x12B -> 0x746620 and slot 0x12C -> 0x7466C0. Both are
// `uint f(PlayerCharacter* this, TESFaction* f)`: they hash-look-up the faction in the map at this+0x3F0 and
// return the raw dword at value+0 (0x12B, violent) / value+4 (0x12C, non-violent), 0 when the faction has no
// entry and when the faction is null. Neither writes. The vfunc road needs no map layout, no hash-table
// walk and no CommonLib member offset, which is why it is the safer road. It also means the faction list
// comes from TESDataHandler (every TESFaction, ~1 s cadence, two lookups each), so no assumption is made about
// which factions are crime factions. globalCrimes is deliberately NOT read: its ProcessLists offset (0xB0) and
// the Crime node layout are not disassembled here, and a wrong read of a live list is not passive.
namespace MFO::CrimeProbe {

    namespace {

        constexpr std::uint32_t kPeriodMs = 1000;

        std::atomic<std::int64_t>  g_lastPostMs{ 0 };
        std::atomic<bool>          g_seatChecked{ false };
        std::atomic<bool>          g_seatOk{ false };
        std::atomic<bool>          g_inFlight{ false };
        std::atomic<bool>          g_refusedLogged{ false };

        // MAIN thread only (the sample is the only reader/writer).
        struct Gold { std::uint32_t violent = 0, nonViolent = 0; };
        std::unordered_map<RE::FormID, Gold> g_last;
        std::uint64_t                        g_lastEpoch = ~0ull;

        const char* NameOr(const char* a_n, const char* a_alt = "?") { return (a_n && *a_n) ? a_n : a_alt; }

        void LogFollowers(RE::PlayerCharacter* a_pc) {
            auto snap = Followers::ActiveSnapshot();   // the immutable off-worker copy: g_active is worker-written
            if (!snap || snap->empty()) { spdlog::info("[crime-probe]   followers: none managed"); return; }
            const auto pcPos = a_pc->GetPosition();
            for (const RE::FormID id : *snap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a) { spdlog::info("[crime-probe]   follower {:08X}: not resolved", id); continue; }
                const auto* cell = a->GetParentCell();
                const auto* pkg  = a->GetCurrentPackage();
                auto        tgt  = a->GetActorRuntimeData().currentCombatTarget.get();
                const auto  errand = Logistics::Errand::Describe(id);
                spdlog::info("[crime-probe]   follower {:08X} '{}' dist={:.0f} cell='{}' errand='{}' pkg={:08X} "
                             "combatTarget={:08X} '{}' lastCast=n/a",
                             id, NameOr(a->GetName()), a->GetPosition().GetDistance(pcPos),
                             cell ? NameOr(cell->GetName(), "(unnamed)") : "(none)", errand.empty() ? "-" : errand,
                             pkg ? pkg->GetFormID() : 0u, tgt ? tgt->GetFormID() : 0u,
                             tgt ? NameOr(tgt->GetName()) : "");
            }
        }

        void Sample() {
            struct Clear { ~Clear() { g_inFlight.store(false, std::memory_order_release); } } clear;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!pc || !dh) return;

            // A load / new pump epoch reseeds the baseline silently, so a save's own bounties never read as increases.
            const auto epoch = Diagnostics::CurrentPumpEpoch();
            const bool reseed = (epoch != g_lastEpoch);
            if (reseed) { g_last.clear(); g_lastEpoch = epoch; }

            bool logged = false;   // ONE header block per sample, followers once
            for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
                if (!fac) continue;
                const Gold now{ pc->GetViolentCrimeGoldValue(fac), pc->GetNonViolentCrimeGoldValue(fac) };
                auto  it  = g_last.find(fac->GetFormID());
                const Gold old = (it != g_last.end()) ? it->second : Gold{};
                if (now.violent == 0 && now.nonViolent == 0 && it == g_last.end()) continue;   // never held crime gold: no entry kept
                g_last[fac->GetFormID()] = now;
                if (reseed) continue;

                for (int v = 0; v < 2; ++v) {
                    const std::uint32_t o = v ? old.violent : old.nonViolent;
                    const std::uint32_t n = v ? now.violent : now.nonViolent;
                    if (n <= o) continue;
                    const auto* cal  = RE::Calendar::GetSingleton();
                    const auto* cell = pc->GetParentCell();
                    const auto* loc  = pc->GetCurrentLocation();
                    spdlog::info("[crime-probe] BOUNTY UP day={:.4f} hour={:.2f} cell='{}' location='{}' "
                                 "faction={:08X} '{}' {} {} -> {} (+{})",
                                 cal ? cal->GetDaysPassed() : -1.0f, cal ? cal->GetHour() : -1.0f,
                                 cell ? NameOr(cell->GetName(), "(unnamed)") : "(none)",
                                 loc ? NameOr(loc->GetName(), "(unnamed)") : "(none)",
                                 fac->GetFormID(), NameOr(fac->GetName()), v ? "violent" : "nonviolent", o, n, n - o);
                    logged = true;
                }
            }
            if (logged) LogFollowers(pc);
        }

    }

    void Pulse() {
        if (!Config::g_crimeGoldProbe.load(std::memory_order_relaxed)) return;
        if (!Runtime::IsVerified1_6_1170()) {
            if (!g_refusedLogged.exchange(true))
                spdlog::warn("[crime-probe] refused: the vfuncs 0x12B/0x12C were disassembled on 1.6.1170 only ({})",
                             Runtime::GateReason());
            return;
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - g_lastPostMs.load(std::memory_order_relaxed) < kPeriodMs) return;
        if (!g_seatChecked.exchange(true)) {   // the PlayerCharacter vtable base must match the verified row before slots 0x12B/0x12C are trusted
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            g_seatOk = Runtime::SeatVerified(vtbl.address(), "MainThread.PlayerCharacter.Update");
            if (!g_seatOk) spdlog::warn("[crime-probe] refused: the PlayerCharacter vtable did not self-check");
        }
        if (!g_seatOk.load()) return;
        g_lastPostMs.store(nowMs, std::memory_order_relaxed);
        if (g_inFlight.exchange(true, std::memory_order_acq_rel)) return;
        MainThread::Post([]() { Sample(); });
    }

}
