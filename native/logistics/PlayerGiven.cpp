// logistics/PlayerGiven.cpp -- the "player gave / player put on" record. Module doc
// in PlayerGiven.h.
#include "PCH.h"
#include "PlayerGiven.h"
#include "Followers.h"    // IsTrackedFast (the locked roster mirror, safe from any thread, #74); IsPersistableID (#9)
#include "MainThread.h"   // the leave recount runs on the main thread
#include "Diagnostics.h"  // PumpTickGate / CurrentPumpEpoch: queued writes drain with StopPump / PausePump
#include "Lotd.h"         // SeenUnwornKept (the AI-equip mark's proof); MayBeRelic (what is recorded)
#include "Serialization.h"   // kRecPlayerGiven / kPlayerGivenVersion
#include "cast/Actuation.h"   // ForcedHoldFor: an in-menu equip MFO's own hold names is not the player's

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::PlayerGiven {

    namespace {
        constexpr RE::FormID   kPlayerID = 0x14;
        constexpr std::uint8_t kGiven    = 1;   // moved player -> him
        constexpr std::uint8_t kEquipped = 2;   // put on him with the trade / gift menu open
        constexpr std::uint8_t kAiEquipped = 4; // HIS AI put on a kept relic MFO saw unworn (no menu open)
        constexpr std::uint8_t kPlayerBits = kGiven | kEquipped;

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
            if (have <= 0) ClearBits(a_follower, a_base, kGiven | kEquipped | kAiEquipped, "left his inventory -- record dropped");
        }

        bool GiveMenuOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) || ui->IsMenuOpen(RE::GiftMenu::MENU_NAME));
        }

        // The sinks QUEUE (Sinks.cpp's ContainerSink pattern, #1/#4): the decision's
        // inputs are read at event time (the direction, the roster mirror, the open
        // menu), the table is written in an AddTask body under PumpTickGate, so a write
        // queued before a revert never lands after ClearRecord.
        template <class F>
        void Queue(F&& a_fn) {
            const auto epoch = Diagnostics::CurrentPumpEpoch();
            SKSE::GetTaskInterface()->AddTask([epoch, fn = std::forward<F>(a_fn)]() {
                Diagnostics::PumpTickGate gate(epoch);
                if (!gate) return;
                fn();
            });
        }

        class EquipSink final : public RE::BSTEventSink<RE::TESEquipEvent> {
        public:
            static EquipSink* GetSingleton() { static EquipSink s; return &s; }

            RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_ev,
                                                  RE::BSTEventSource<RE::TESEquipEvent>*) override {
                if (!a_ev || !a_ev->actor || !a_ev->baseObject) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID id = a_ev->actor->GetFormID();
                if (!Followers::IsTrackedFast(id)) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID base = a_ev->baseObject;
                if (!GiveMenuOpen()) {
                    // NO MENU: the AI-equip mark (review round 3, POSITIVE proof for the
                    // worn-relic ship and the relic swap). Only an equip of a base MFO had
                    // seen UNWORN in his pack as a kept relic, and not one MFO's own hold
                    // names (MFO's last-resort relic pick). Decided in the queued body on
                    // the worker, where both of those are read.
                    if (a_ev->equipped)
                        Queue([id, base]() {
                            if (!Lotd::SeenUnwornKept(id, base)) return;
                            const auto [hr, hl] = Actuation::ForcedHoldFor(id);
                            if (hr == base || hl == base) return;
                            if (Bits(id, base) & kPlayerBits) return;
                            Mark(id, base, kAiEquipped, "put on by his own AI (a kept relic MFO saw unworn in his pack)");
                        });
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (a_ev->equipped)
                    Queue([id, base]() {
                        // MFO's OWN equip landing while the menu is open (review round 3):
                        // a weapon MFO holds is named by its FWPN ledger, which the worker
                        // wrote before the equip was posted -- not the player's choice. The
                        // relic logic reads only weapons, so this is the case that matters;
                        // any other MFO equip mislabelled here only fails closed (never shipped).
                        const auto [hr, hl] = Actuation::ForcedHoldFor(id);
                        if (hr == base || hl == base) return;
                        // Relics only (marth 2026-09-27): a non-relic the player puts on him
                        // is fair game and is not recorded.
                        if (!Lotd::MayBeRelic(base)) return;
                        ClearBits(id, base, kAiEquipped, "now the player's choice (trade / gift menu open)");
                        Mark(id, base, kEquipped, "museum relic put on him by the player (trade / gift menu open)");
                    });
                else
                    Queue([id, base]() { ClearBits(id, base, kEquipped, "taken off him by the player (trade / gift menu open)"); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void OnContainerChanged(const RE::TESContainerChangedEvent& a_ev) {
        if (!a_ev.baseObj) return;
        const RE::FormID from = a_ev.oldContainer;
        const RE::FormID to   = a_ev.newContainer;
        const RE::FormID base = a_ev.baseObj;
        if (from == kPlayerID && to && Followers::IsTrackedFast(to)) {
            // Relics only (marth 2026-09-27: "regular items given to followers" are fair
            // game; "Relics however should be recorded"). Decided in the queued body.
            Queue([to, base]() {
                if (!Lotd::MayBeRelic(base)) return;
                Mark(to, base, kGiven, "museum relic given by the player -- never shipped");
            });
        } else if (from && from != kPlayerID && Bits(from, base)) {
            // Left a follower with a record: recount on the MAIN thread next frame.
            MainThread::Post([from, base]() { RecountOnMain(from, base); });
        }
    }

    void RegisterSinks() {
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            spdlog::warn("[player-given] no event source holder -- the player-given record is NOT installed "
                         "(nothing is recorded as given; LOTD then never ships a worn relic)");
            return;
        }
        // The container half has NO sink of its own: Sinks.cpp's ContainerSink calls
        // OnContainerChanged (one TESContainerChangedEvent sink for all of logistics).
        holder->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        g_installed.store(true);
        spdlog::info("[player-given] equip sink installed (container transfers via the logistics ContainerSink)");
    }

    void ClearRecord() {
        std::scoped_lock lk(g_mx);
        g_rec.clear();
    }

    std::size_t ForgetFollower(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_rec.find(a_follower);
        if (it == g_rec.end()) return 0;
        const std::size_t n = it->second.size();
        g_rec.erase(it);
        return n;
    }

    // ── the 'PGIV' co-save record (Serialization.h has the layout) ──────────────
    namespace {
        constexpr std::uint32_t kMaxSavedFollowers = 4096;   // #11: bound every count
        constexpr std::uint32_t kMaxSavedEntries   = 4096;   // per follower (relics only)
    }

    void CoSave(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecPlayerGiven, kPlayerGivenVersion)) {
            spdlog::error("[cosave] OpenRecord('PGIV') failed -- player-given relics NOT saved");
            return;
        }
        // SNAPSHOT under the lock, write from the copy: a lock is never held across a
        // WriteRecordData (the MSTK / FWPN rule). Only the PLAYER bits are saved; the
        // AI-equip mark stays session-only (it fails closed after a load).
        std::vector<std::pair<RE::FormID, std::vector<std::pair<RE::FormID, std::uint8_t>>>> snap;
        std::uint32_t skippedRuntime = 0;
        {
            std::scoped_lock lk(g_mx);
            for (const auto& [fid, m] : g_rec) {
                if (!Followers::IsPersistableID(fid)) { ++skippedRuntime; continue; }   // #9
                std::vector<std::pair<RE::FormID, std::uint8_t>> v;
                for (const auto& [base, bits] : m) {
                    const auto pb = static_cast<std::uint8_t>(bits & kPlayerBits);
                    if (!pb) continue;
                    if (!Followers::IsPersistableID(base)) { ++skippedRuntime; continue; }   // #9 (a 0xFF base)
                    if (v.size() >= kMaxSavedEntries) break;
                    v.emplace_back(base, pb);
                }
                if (!v.empty()) snap.emplace_back(fid, std::move(v));
                if (snap.size() >= kMaxSavedFollowers) break;
            }
        }
        std::uint32_t entries = 0;
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(snap.size()));
        for (const auto& [fid, v] : snap) {
            a_intfc->WriteRecordData(fid);
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(v.size()));
            for (const auto& [base, bits] : v) {
                a_intfc->WriteRecordData(base);
                a_intfc->WriteRecordData(bits);
                ++entries;
            }
        }
        spdlog::info("[cosave] saved {} player-given relic(s) for {} follower(s), schema v{}{}", entries,
                     snap.size(), kPlayerGivenVersion,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) id(s)", skippedRuntime)
                                    : std::string{});
    }

    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t /*a_version*/) {
        // v1 is the only version. A short read / implausible count aborts THIS record
        // only (the dispatch loop seeks past the rest); what was parsed is kept.
        std::uint32_t followers = 0;
        if (!a_intfc->ReadRecordData(followers)) {
            spdlog::error("[cosave] short read on PGIV follower count -- ABORTING player-given load");
            return;
        }
        if (followers > kMaxSavedFollowers) {
            spdlog::error("[cosave] implausible PGIV follower count {} -- ABORTING player-given load", followers);
            return;
        }
        std::uint32_t loaded = 0, droppedFollower = 0, droppedBase = 0;
        for (std::uint32_t i = 0; i < followers; ++i) {
            RE::FormID rawFid = 0;
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(rawFid) || !a_intfc->ReadRecordData(n)) {
                spdlog::error("[cosave] short read in PGIV -- ABORTING (kept {} relic(s))", loaded);
                return;
            }
            if (n > kMaxSavedEntries) {
                spdlog::error("[cosave] implausible PGIV entry count {} -- ABORTING (kept {} relic(s))", n, loaded);
                return;
            }
            RE::FormID fid = 0;
            const bool fidOk = a_intfc->ResolveFormID(rawFid, fid);   // #8: unresolvable -> DROP
            if (!fidOk) ++droppedFollower;
            for (std::uint32_t k = 0; k < n; ++k) {
                RE::FormID rawBase = 0;
                std::uint8_t bits  = 0;
                if (!a_intfc->ReadRecordData(rawBase) || !a_intfc->ReadRecordData(bits)) {
                    spdlog::error("[cosave] short read in PGIV -- ABORTING (kept {} relic(s))", loaded);
                    return;
                }
                if (!fidOk) continue;
                bits = static_cast<std::uint8_t>(bits & kPlayerBits);   // #11: clamp at ingestion
                RE::FormID base = 0;
                if (!a_intfc->ResolveFormID(rawBase, base) || !RE::TESForm::LookupByID(base)) {
                    ++droppedBase;   // its plugin is gone: the entry goes with it
                    continue;
                }
                if (!bits) continue;
                {
                    std::scoped_lock lk(g_mx);
                    g_rec[fid][base] |= bits;
                }
                ++loaded;
            }
        }
        spdlog::info("[cosave] loaded {} player-given relic(s); dropped {} unresolvable follower(s), {} "
                     "unresolvable relic(s)", loaded, droppedFollower, droppedBase);
    }

    bool Installed() { return g_installed.load(); }
    bool IsPlayerGiven(RE::FormID a_follower, RE::FormID a_base) { return (Bits(a_follower, a_base) & kPlayerBits) != 0; }
    bool IsAiEquipped(RE::FormID a_follower, RE::FormID a_base) { return (Bits(a_follower, a_base) & kAiEquipped) != 0; }
    bool IsPlayerEquipped(RE::FormID a_follower, RE::FormID a_base) { return (Bits(a_follower, a_base) & kEquipped) != 0; }
}
