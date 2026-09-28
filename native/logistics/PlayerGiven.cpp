// logistics/PlayerGiven.cpp -- the "player gave / player put on" record. Module doc
// in PlayerGiven.h.
#include "PCH.h"
#include "PlayerGiven.h"
#include "Followers.h"    // IsTrackedFast: the locked roster mirror, safe from any thread (#74)
#include "MainThread.h"   // the leave recount runs on the main thread

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::PlayerGiven {

    namespace {
        constexpr RE::FormID   kPlayerID = 0x14;
        constexpr std::uint8_t kGiven    = 1;   // moved player -> him
        constexpr std::uint8_t kEquipped = 2;   // put on him with the trade / gift menu open

        std::atomic<bool> g_installed{ false };
        std::mutex g_mx;
        // follower -> base -> bits. Guarded by g_mx.
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, std::uint8_t>> g_rec;

        const char* NameOf(RE::FormID a_base) {
            auto* f  = RE::TESForm::LookupByID(a_base);
            auto* fn = f ? f->As<RE::TESFullName>() : nullptr;
            const char* n = fn ? fn->GetFullName() : nullptr;
            return n && *n ? n : "?";
        }

        std::uint8_t Bits(RE::FormID a_follower, RE::FormID a_base) {
            std::scoped_lock lk(g_mx);
            const auto it = g_rec.find(a_follower);
            if (it == g_rec.end()) return 0;
            const auto jt = it->second.find(a_base);
            return jt == it->second.end() ? 0 : jt->second;
        }

        void Mark(RE::FormID a_follower, RE::FormID a_base, std::uint8_t a_bits, const char* a_why) {
            bool added = false;
            {
                std::scoped_lock lk(g_mx);
                auto& v = g_rec[a_follower][a_base];
                added   = (v | a_bits) != v;
                v |= a_bits;
            }
            if (added)
                spdlog::info("[player-given] {:08X}: '{}' ({:08X}) {}", a_follower, NameOf(a_base), a_base, a_why);
        }

        void ClearBits(RE::FormID a_follower, RE::FormID a_base, std::uint8_t a_bits, const char* a_why) {
            bool cleared = false;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_rec.find(a_follower);
                if (it == g_rec.end()) return;
                const auto jt = it->second.find(a_base);
                if (jt == it->second.end() || !(jt->second & a_bits)) return;
                jt->second &= static_cast<std::uint8_t>(~a_bits);
                cleared = true;
                if (!jt->second) it->second.erase(jt);
                if (it->second.empty()) g_rec.erase(it);
            }
            if (cleared)
                spdlog::info("[player-given] {:08X}: '{}' ({:08X}) {}", a_follower, NameOf(a_base), a_base, a_why);
        }

        // MAIN THREAD (posted by the leave event): the item left him; drop the entry
        // only when no copy of that base is left in his inventory.
        void RecountOnMain(RE::FormID a_follower, RE::FormID a_base) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            if (!a) return;   // unresolvable: keep (fail closed); revert clears it
            std::int32_t have = 0;
            for (auto& [obj, cnt] : a->GetInventoryCounts(
                     [a_base](RE::TESBoundObject& x) { return x.GetFormID() == a_base; }, false))
                have += cnt;
            if (have <= 0) ClearBits(a_follower, a_base, kGiven | kEquipped, "left his inventory -- record dropped");
        }

        bool GiveMenuOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME));
        }

        class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
        public:
            static ContainerSink* GetSingleton() { static ContainerSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
                if (!a_ev || !a_ev->baseObj) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID from = a_ev->oldContainer;
                const RE::FormID to   = a_ev->newContainer;
                if (from == kPlayerID && to && Followers::IsTrackedFast(to)) {
                    Mark(to, a_ev->baseObj, kGiven, "given by the player -- never shipped");
                } else if (from && from != kPlayerID && Bits(from, a_ev->baseObj)) {
                    const RE::FormID base = a_ev->baseObj;
                    MainThread::Post([from, base]() { RecountOnMain(from, base); });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        class EquipSink final : public RE::BSTEventSink<RE::TESEquipEvent> {
        public:
            static EquipSink* GetSingleton() { static EquipSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_ev || !a_ev->actor || !a_ev->baseObject) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID id = a_ev->actor->GetFormID();
                if (!Followers::IsTrackedFast(id) || !GiveMenuOpen()) return RE::BSEventNotifyControl::kContinue;
                if (a_ev->equipped)
                    Mark(id, a_ev->baseObject, kEquipped, "put on him by the player (trade / gift menu open)");
                else
                    ClearBits(id, a_ev->baseObject, kEquipped, "taken off him by the player (trade / gift menu open)");
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[player-given] no event source holder -- the player-given record is NOT installed "
                         "(nothing is recorded as given; LOTD then never ships a worn relic)");
            return;
        }
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        holder->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        g_installed.store(true);
        spdlog::info("[player-given] container + equip sinks installed");
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_rec.clear();
    }

    bool Installed() { return g_installed.load(); }
    bool IsPlayerGiven(RE::FormID a_follower, RE::FormID a_base) { return Bits(a_follower, a_base) != 0; }
    bool IsPlayerEquipped(RE::FormID a_follower, RE::FormID a_base) { return (Bits(a_follower, a_base) & kEquipped) != 0; }
}
