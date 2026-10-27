#include "PCH.h"
#include "State.h"
#include "Serialization.h"
#include "Followers.h"
#include "Rapport.h"
#include "Diagnostics.h"
#include "Scheduler.h"
#include "logistics/Logistics.h"
#include "Loadout.h"
#include "Targeting.h"
#include "Vocabulary.h"   // IsAppendedSubject: the FLWR flags byte for an Enemy / Caster target
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "cast/Actuation.h"   // T#76: revert drops the equip force-hold records
#include "Sightline.h"
#include "Packages.h"
#include "Papyrus.h"
#include "TradeBridge.h"
#include "MEOBridge.h"
#include "MainThread.h"
#include "SelfStatus.h"   // ResetAllState: drop the poisoned-state mirror
#include "Probe.h"
#include "Board.h"
#include "progression/ProgAllocator.h"
#include "logistics/PlayerGiven.h"   // 86e3faccn: the 'PGIV' player-given relic record
#include "logistics/Errand.h"        // 86e3t1mx7 (town R2): the 'ERND' errand record
#include <cmath>                     // std::isfinite: ERND float ingestion (#11)
#include "logistics/GemRules.h"   // revert: clear the looted-gear gem probes
#include <unordered_set>   // T#69: stock-gear per-follower sets (kRecStock)

// P0: the co-save. Schema in ARCHITECTURE.md §7; rules in INVARIANTS.md §B.
//
// The five rules this file exists to obey:
//   #8  Every persisted FormID passes ResolveFormID. Unresolvable -> DROP.
//   #9  Never persist a runtime-created (0xFF) FormID.
//   #10 Persist stable string opcodes, never enum ordinals.
//   #11 Bound every count, bail on short reads, clamp at ingestion.
//   SCHEMA HISTORY (INVARIANTS #12 -- readers kept forever):
//     v1  v0.1.0-v0.3.0. Had a tutored-spell block between the tables and the
//         overrides. Still READ below; never written again.
//     v2  tutoring removed (DESIGN.md 5.4).
//     v3  #68: subjectActorForm (a specific cast-target follower) added
//         right after subjectSelector, per gambit. Read gated on
//         version >= 3; a v1/v2 record simply has none (defaults to 0).
//     v4  #65: combatClassOverride (per-follower forced combat stance)
//         added right after st.rank. Read gated on version >= 4; an older
//         record simply has none (defaults to 0 -- Auto, no override).
//     v5  T#78: mfoEnabled (per-follower MFO master switch) added as one u8
//         right after combatClassOverride. Read gated on version >= 5; an
//         older record has none (defaults to true -- MFO enabled).
//
//   #12 Versioned schema; readers kept FOREVER; SKSE does NOT round-trip
//       unread records, so a downgraded DLL DESTROYS newer ones -> warn loud.

namespace MFO {

    namespace {
        // Bounds. A truncated or hostile record must stop the parse, never
        // fabricate (INVARIANTS.md #11).
        constexpr std::uint32_t kMaxFollowers = 4096;
        constexpr std::uint32_t kMaxOpcodeLen = 64;
        constexpr std::uint16_t kMaxOverrides = 64;
        constexpr std::uint16_t kMaxTutoredV1 = 512;
        constexpr std::uint32_t kMaxStockGear = 512;   // T#69: generous headroom over any real loadout

        std::atomic<bool> g_sawNewerSave{ false };   // v1 only, consumed and discarded

        bool WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s) {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            if (len > kMaxOpcodeLen) {
                spdlog::error("[cosave] opcode too long ({}), refusing to write: {}", len, a_s);
                return false;
            }
            return a_intfc->WriteRecordData(len) &&
                   (len == 0 || a_intfc->WriteRecordData(a_s.data(), len));
        }

        // Returns false on short read or implausible length -- caller must abort.
        bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_out) {
            std::uint32_t len = 0;
            if (a_intfc->ReadRecordData(len) != sizeof(len)) return false;   // MFO-B309: a 1-3 byte partial read is a short read (#11)
            if (len > kMaxOpcodeLen) {
                spdlog::error("[cosave] opcode length {} exceeds max {} -- aborting read", len, kMaxOpcodeLen);
                return false;
            }
            a_out.resize(len);
            if (len == 0) return true;
            return a_intfc->ReadRecordData(a_out.data(), len) == len;
        }

        // ── 'ERND' (town R2; layout in Serialization.h) ─────────────────────────────────────────────
        // The opcode tables are the WIRE identity (#10): never rename a string; a new stage APPENDS one.
        constexpr std::uint32_t kMaxErrands = 256;   // #11: one per follower, generous headroom
        using ErrStage = Logistics::Errand::SavedStage;
        using ErrDest  = Logistics::Errand::SavedDest;
        constexpr std::pair<ErrStage, const char*> kErrStageOps[] = {
            { ErrStage::PhysWalking,  "phys.walking" },  { ErrStage::PhysAtVendor, "phys.at_vendor" },
            { ErrStage::PhysTrading,  "phys.trading" },  { ErrStage::PhysTaking,   "phys.taking" },
            { ErrStage::SimOut,       "sim.out" },       { ErrStage::SimTrade,     "sim.trade" },
            { ErrStage::SimReturn,    "sim.return" },    { ErrStage::SimRejoin,    "sim.rejoin" },
        };
        constexpr std::pair<ErrDest, const char*> kErrDestOps[] = {
            { ErrDest::VendorActor, "vendor_actor" }, { ErrDest::Ref, "ref" }, { ErrDest::Cell, "cell" },
        };
        // R5 (ERND v2): the itinerary's opcodes. A later round APPENDS a stop kind (fence, spouse, smith, home; R7 appended temple and inn).
        using ErrStopKind  = Logistics::Errand::SavedStopKind;
        using ErrStopState = Logistics::Errand::SavedStopState;
        constexpr std::pair<ErrStopKind, const char*> kErrStopKindOps[] = {
            { ErrStopKind::Shop, "shop" }, { ErrStopKind::Temple, "temple" }, { ErrStopKind::Inn, "inn" }, { ErrStopKind::Smith, "smith" },   // R7 appended the last two, R8 "smith" (NO layout change)
        };
        constexpr std::pair<ErrStopState, const char*> kErrStopStateOps[] = {
            { ErrStopState::Pending, "pending" }, { ErrStopState::Active, "active" },
            { ErrStopState::Done, "done" },       { ErrStopState::Skipped, "skipped" },
        };
        constexpr std::size_t kMaxErrandStops = Logistics::Errand::kMaxTripStopsOnWire;   // #11
        template <class E, std::size_t N>
        const char* OpOf(const std::pair<E, const char*> (&a_tab)[N], E a_v) {
            for (const auto& [v, op] : a_tab) if (v == a_v) return op;
            return nullptr;
        }
        template <class E, std::size_t N>
        bool FromOp(const std::pair<E, const char*> (&a_tab)[N], const std::string& a_op, E& a_out) {
            for (const auto& [v, op] : a_tab) if (a_op == op) { a_out = v; return true; }
            return false;
        }
        // #9: a 0xFF (runtime) id is never written; 0 means "none".
        RE::FormID PersistId(RE::FormID a_id) { return (a_id >> 24) == 0xFF ? 0u : a_id; }

        void WriteErrandRecord(SKSE::SerializationInterface* a_intfc) {
            auto snap = Logistics::Errand::SnapshotForSave();
            std::uint32_t skippedRuntime = 0;
            std::erase_if(snap, [&](const Logistics::Errand::Saved& e) {
                const bool drop = e.follower == 0 || (e.follower >> 24) == 0xFF;   // #9: a runtime follower is session-only
                if (drop) ++skippedRuntime;
                return drop;
            });
            if (snap.size() > kMaxErrands) snap.resize(kMaxErrands);
            if (snap.empty()) {   // design 2.7: written only when errands exist
                if (skippedRuntime)
                    spdlog::info("[cosave] no errand record written -- SKIPPED {} runtime (0xFF) follower(s)", skippedRuntime);
                return;
            }
            if (!a_intfc->OpenRecord(kRecErrand, kErrandVersion)) {
                spdlog::error("[cosave] OpenRecord('ERND') failed -- town errands NOT saved");
                return;
            }
            bool ok = a_intfc->WriteRecordData(static_cast<std::uint32_t>(snap.size()));
            for (const auto& e : snap) {
                const char* stageOp = OpOf(kErrStageOps, e.stage);
                const char* destOp  = OpOf(kErrDestOps, e.destKind);
                ok = ok && a_intfc->WriteRecordData(e.follower) &&
                     WriteString(a_intfc, stageOp ? stageOp : "") &&
                     a_intfc->WriteRecordData(static_cast<std::uint8_t>(e.flags & Logistics::Errand::Saved::kAllFlags)) &&
                     a_intfc->WriteRecordData(PersistId(e.town)) && a_intfc->WriteRecordData(PersistId(e.faction)) &&
                     a_intfc->WriteRecordData(PersistId(e.chest)) && a_intfc->WriteRecordData(PersistId(e.vendorActor)) &&
                     WriteString(a_intfc, destOp ? destOp : "") &&
                     a_intfc->WriteRecordData(PersistId(e.destForm)) && a_intfc->WriteRecordData(e.destRad) &&
                     a_intfc->WriteRecordData(PersistId(e.entryDoor)) && a_intfc->WriteRecordData(PersistId(e.lastCell)) &&
                     a_intfc->WriteRecordData(e.lastPos.x) && a_intfc->WriteRecordData(e.lastPos.y) &&
                     a_intfc->WriteRecordData(e.lastPos.z) && a_intfc->WriteRecordData(e.startDays) &&
                     a_intfc->WriteRecordData(e.tEnter) && a_intfc->WriteRecordData(e.tArrive) &&
                     a_intfc->WriteRecordData(e.tTradeEnd) && a_intfc->WriteRecordData(e.tReturn);
                // v2 (R5): the itinerary, APPENDED to the v1 entry. Always >= 1 stop (an entry built without one writes its head).
                std::vector<Logistics::Errand::SavedStop> stops(e.stops.begin(),
                                                                e.stops.begin() + static_cast<std::ptrdiff_t>(std::min(e.stops.size(), kMaxErrandStops)));
                std::uint8_t cur = e.cur;
                if (stops.empty()) {
                    Logistics::Errand::SavedStop h;
                    h.faction = e.faction; h.chest = e.chest; h.vendorActor = e.vendorActor; h.destKind = e.destKind;
                    h.destForm = e.destForm; h.destRad = e.destRad; h.state = ErrStopState::Active;
                    h.tArrive = e.tArrive; h.tWorkEnd = e.tTradeEnd;
                    stops.push_back(h);
                    cur = 0;
                }
                if (cur >= stops.size()) cur = static_cast<std::uint8_t>(stops.size() - 1);
                ok = ok && a_intfc->WriteRecordData(static_cast<std::uint8_t>(stops.size()));
                for (const auto& st : stops) {
                    const char* kindOp  = OpOf(kErrStopKindOps, st.kind);
                    const char* sdOp    = OpOf(kErrDestOps, st.destKind);
                    const char* stateOp = OpOf(kErrStopStateOps, st.state);
                    ok = ok && WriteString(a_intfc, kindOp ? kindOp : "") &&
                         a_intfc->WriteRecordData(PersistId(st.faction)) && a_intfc->WriteRecordData(PersistId(st.chest)) &&
                         a_intfc->WriteRecordData(PersistId(st.vendorActor)) && WriteString(a_intfc, sdOp ? sdOp : "") &&
                         a_intfc->WriteRecordData(PersistId(st.destForm)) && a_intfc->WriteRecordData(st.destRad) &&
                         WriteString(a_intfc, stateOp ? stateOp : "") &&
                         a_intfc->WriteRecordData(st.tArrive) && a_intfc->WriteRecordData(st.tWorkEnd);
                }
                ok = ok && a_intfc->WriteRecordData(cur);
            }
            if (!ok)
                spdlog::error("[cosave] a write into 'ERND' failed -- the errand record is INCOMPLETE (the load aborts it at the short read)");
            spdlog::info("[cosave] saved {} town errand(s), schema v{}{}", snap.size(), kErrandVersion,
                         skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) follower(s)", skippedRuntime) : std::string{});
        }

        // 'ERND' body (after the version guard). A short read / implausible count / bad opcode string aborts THIS record
        // (what was parsed is still staged); an entry it cannot honestly map is DROPPED with a line (#13).
        void ReadErrandRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
            using Logistics::Errand::Saved;
            // A read counts only when it delivered every byte (a partial read is a short read, #11).
            auto rd = [&](auto& a_v) { return a_intfc->ReadRecordData(a_v) == sizeof(a_v); };
            std::uint32_t count = 0;
            if (!rd(count)) {
                spdlog::error("[cosave] short read on ERND count -- ABORTING errand load");
                return;
            }
            if (count > kMaxErrands) {
                spdlog::error("[cosave] implausible ERND count {} -- ABORTING errand load", count);
                return;
            }
            std::vector<Saved> staged;
            std::uint32_t droppedFollower = 0, droppedOpcode = 0, unresolvedRef = 0, shopGone = 0, clockReset = 0;
            std::uint32_t stopsSkipped = 0, droppedTrip = 0;   // R5 (v2)
            auto resolve = [&](RE::FormID a_raw, RE::FormID& a_out) -> bool {   // 0 stays 0; unresolvable -> 0
                a_out = 0;
                if (a_raw == 0) return true;
                if (a_intfc->ResolveFormID(a_raw, a_out) && a_out != 0) return true;
                a_out = 0;
                ++unresolvedRef;
                return false;
            };
            for (std::uint32_t i = 0; i < count; ++i) {
                Saved e;
                RE::FormID rawFollower = 0, rawTown = 0, rawFac = 0, rawChest = 0, rawVendor = 0, rawDest = 0, rawDoor = 0, rawCell = 0;
                std::string stageOp, destOp;
                std::uint8_t flags = 0;
                const bool ok = rd(rawFollower) && ReadString(a_intfc, stageOp) &&
                                rd(flags) && rd(rawTown) &&
                                rd(rawFac) && rd(rawChest) &&
                                rd(rawVendor) && ReadString(a_intfc, destOp) &&
                                rd(rawDest) && rd(e.destRad) &&
                                rd(rawDoor) && rd(rawCell) &&
                                rd(e.lastPos.x) && rd(e.lastPos.y) &&
                                rd(e.lastPos.z) && rd(e.startDays) &&
                                rd(e.tEnter) && rd(e.tArrive) &&
                                rd(e.tTradeEnd) && rd(e.tReturn);
                if (!ok) {
                    spdlog::error("[cosave] short read in ERND at errand {}/{} -- ABORTING (staged {} so far)", i, count, staged.size());
                    break;
                }
                // v2 (R5): the itinerary tail. Read IN FULL before any per-entry drop below, so the stream stays aligned.
                struct RawStop {
                    std::string kindOp, destOp, stateOp;
                    RE::FormID  fac = 0, chest = 0, vendor = 0, dest = 0;
                    float       rad = 0.0f;
                    double      tArrive = 0.0, tWorkEnd = 0.0;
                };
                std::vector<RawStop> rawStops;
                std::uint8_t rawCur = 0;
                if (a_version >= 2) {
                    std::uint8_t n = 0;
                    if (!rd(n)) {
                        spdlog::error("[cosave] short read on the ERND v2 stop count at errand {}/{} -- ABORTING (staged {} so far)", i, count, staged.size());
                        break;
                    }
                    if (n == 0 || n > kMaxErrandStops) {   // #11: the writer always writes 1..8
                        spdlog::error("[cosave] implausible ERND v2 stop count {} at errand {}/{} -- ABORTING (staged {} so far)", static_cast<int>(n), i, count, staged.size());
                        break;
                    }
                    bool sok = true;
                    for (std::uint8_t k = 0; k < n && sok; ++k) {
                        RawStop rs;
                        sok = ReadString(a_intfc, rs.kindOp) && rd(rs.fac) && rd(rs.chest) && rd(rs.vendor) &&
                              ReadString(a_intfc, rs.destOp) && rd(rs.dest) && rd(rs.rad) && ReadString(a_intfc, rs.stateOp) &&
                              rd(rs.tArrive) && rd(rs.tWorkEnd);
                        if (sok) rawStops.push_back(std::move(rs));
                    }
                    if (!sok || !rd(rawCur)) {
                        spdlog::error("[cosave] short read in the ERND v2 itinerary at errand {}/{} -- ABORTING (staged {} so far)", i, count, staged.size());
                        break;
                    }
                }
                if (!a_intfc->ResolveFormID(rawFollower, e.follower) || e.follower == 0) {   // #8
                    ++droppedFollower;
                    spdlog::info("[cosave] ERND: errand of follower {:08X} DROPPED -- the follower does not resolve (his plugin is gone)", rawFollower);
                    continue;
                }
                if (!FromOp(kErrStageOps, stageOp, e.stage) || !FromOp(kErrDestOps, destOp, e.destKind)) {   // #13
                    ++droppedOpcode;
                    spdlog::warn("[cosave] ERND: errand of {:08X} DROPPED -- unknown stage '{}' / destination '{}'", e.follower, stageOp, destOp);
                    continue;
                }
                e.flags = static_cast<std::uint8_t>(flags & Saved::kAllFlags);   // #11
                resolve(rawTown, e.town);
                const bool facOk = resolve(rawFac, e.faction) && e.faction != 0;
                const bool chestOk = resolve(rawChest, e.chest) && e.chest != 0;
                resolve(rawVendor, e.vendorActor);
                resolve(rawDest, e.destForm);
                resolve(rawDoor, e.entryDoor);
                resolve(rawCell, e.lastCell);
                if (!facOk || !chestOk) {   // the shop is gone: no trade can happen, but he is still brought back
                    ++shopGone;
                    e.flags = static_cast<std::uint8_t>(e.flags & ~Saved::kHadOrder);
                    if (!(e.flags & Saved::kTradeDone)) e.flags |= Saved::kAbandoned;
                }
                // #11: clamp at ingestion.
                if (!std::isfinite(e.destRad)) e.destRad = 0.0f;
                e.destRad = std::clamp(e.destRad, 0.0f, 4096.0f);
                if (!std::isfinite(e.lastPos.x) || !std::isfinite(e.lastPos.y) || !std::isfinite(e.lastPos.z)) {
                    e.lastPos = {};
                    e.lastCell = 0;
                }
                if (!std::isfinite(e.startDays) || e.startDays < 0.0f) e.startDays = 0.0f;
                const bool clockOk = std::isfinite(e.tEnter) && std::isfinite(e.tArrive) && std::isfinite(e.tTradeEnd) &&
                                     std::isfinite(e.tReturn) && e.tEnter >= 0.0 && e.tArrive >= 0.0 && e.tTradeEnd >= 0.0 &&
                                     e.tReturn >= 0.0;
                if (!clockOk) {
                    ++clockReset;
                    e.tEnter = e.tArrive = e.tTradeEnd = e.tReturn = 0.0;   // adoption makes a fresh walk estimate
                } else {
                    e.tArrive = std::max(e.tArrive, e.tEnter);
                    e.tTradeEnd = std::max(e.tTradeEnd, e.tArrive);
                    e.tReturn = std::max(e.tReturn, e.tTradeEnd);
                }
                // The itinerary. v1: ONE stop, the entry's own shop (cur 0, Active). v2: the saved stops; stops[cur] is the entry's head
                // (the adoption writes the resolved head fields over it). A stop that cannot be honestly mapped -- unknown opcode, its
                // shop gone -- is SKIPPED (#13: never acted on, logged); the CURRENT stop's opcodes unknown drops the entry.
                if (a_version < 2) {
                    Logistics::Errand::SavedStop h;
                    h.faction = e.faction; h.chest = e.chest; h.vendorActor = e.vendorActor; h.destKind = e.destKind;
                    h.destForm = e.destForm; h.destRad = e.destRad; h.state = ErrStopState::Active;
                    h.tArrive = e.tArrive; h.tWorkEnd = e.tTradeEnd;
                    e.stops.push_back(h);
                    e.cur = 0;
                } else {
                    if (rawCur >= rawStops.size()) {
                        ++droppedTrip;
                        spdlog::warn("[cosave] ERND: errand of {:08X} DROPPED -- its current stop {} is outside its {} stop(s)", e.follower,
                                     static_cast<int>(rawCur), rawStops.size());
                        continue;
                    }
                    bool curBad = false;
                    for (std::size_t k = 0; k < rawStops.size(); ++k) {
                        const auto& rs = rawStops[k];
                        Logistics::Errand::SavedStop st;
                        const bool opsOk = FromOp(kErrStopKindOps, rs.kindOp, st.kind) && FromOp(kErrDestOps, rs.destOp, st.destKind) &&
                                           FromOp(kErrStopStateOps, rs.stateOp, st.state);
                        if (!opsOk && k == rawCur) { curBad = true; break; }
                        const bool facOk2 = resolve(rs.fac, st.faction) && st.faction != 0;
                        const bool chestOk2 = resolve(rs.chest, st.chest) && st.chest != 0;
                        resolve(rs.vendor, st.vendorActor);
                        resolve(rs.dest, st.destForm);
                        st.destRad = std::isfinite(rs.rad) ? std::clamp(rs.rad, 0.0f, 4096.0f) : 0.0f;
                        st.tArrive = std::isfinite(rs.tArrive) && rs.tArrive >= 0.0 ? rs.tArrive : 0.0;
                        st.tWorkEnd = std::isfinite(rs.tWorkEnd) && rs.tWorkEnd >= 0.0 ? std::max(rs.tWorkEnd, st.tArrive) : st.tArrive;
                        if (k != rawCur && (!opsOk || ((!facOk2 || !chestOk2) && st.kind == ErrStopKind::Shop && st.state == ErrStopState::Pending))) {   // R7: a temple / inn stop carries no shop
                            if (!opsOk) { st.kind = ErrStopKind::Shop; st.destKind = ErrDest::Cell; }
                            st.state = ErrStopState::Skipped;
                            ++stopsSkipped;
                            spdlog::info("[cosave] ERND: stop {} of {:08X}'s trip SKIPPED -- {}", k + 1, e.follower,
                                         !opsOk ? std::format("unknown kind '{}' / destination '{}' / state '{}'", rs.kindOp, rs.destOp, rs.stateOp)
                                                : std::string("its shop (faction / chest) does not resolve"));
                        }
                        e.stops.push_back(st);
                    }
                    if (curBad) {
                        ++droppedOpcode;
                        spdlog::warn("[cosave] ERND: errand of {:08X} DROPPED -- unknown opcode in its current stop", e.follower);
                        continue;
                    }
                    e.cur = rawCur;
                }
                staged.push_back(e);
            }
            const auto n = staged.size();
            Logistics::Errand::RestoreFromSave(std::move(staged));
            spdlog::info("[cosave] loaded {} town errand(s) from schema v{} (staged until the world is up); dropped {} unresolvable follower(s), {} "
                         "unknown opcode(s), {} bad current stop(s); {} unresolvable reference(s) zeroed, {} shop(s) gone (trade given up), {} "
                         "clock(s) reset, {} later stop(s) skipped", n, a_version, droppedFollower, droppedOpcode, droppedTrip, unresolvedRef,
                         shopGone, clockReset, stopsSkipped);
        }
    }

    void SaveCallback(SKSE::SerializationInterface* a_intfc) {
        // SEV-1 #3: quiesce the AddTask worker for the whole callback so the
        // count pass (below) and the write pass see the SAME g_followers. Without
        // this, the worker's Refresh/TryEnsureRecord insert (map rehash) or
        // Scheduler::Tick rule-write between the two passes tears the FLWR record
        // or invalidates the iterator (co-save corruption). RAII so every early
        // return still resumes; no epoch bump, so gameplay resumes untouched.
        Diagnostics::PausePump();
        struct ResumeGuard { ~ResumeGuard() { Diagnostics::ResumePump(); } } _resume;
        // Review N1 (roster removal round): apply any queued respec rapport debits NOW,
        // before the FLWR pass. Rapport::Spend is deferred to the worker (F5), so a save
        // in the window after a respec would write PRGN with the respec applied but FLWR
        // without the debit, and the load would drop the queued debit: a free respec.
        // Legal here for the same reason this callback may read g_followers at all: the
        // worker is drained (PausePump). No layout change.
        Rapport::DrainPendingSpends();

        if (!a_intfc->OpenRecord(kRecFollowers, kSchemaVersion)) {
            spdlog::error("[cosave] OpenRecord('{}') failed -- NOTHING SAVED", "FLWR");
            return;
        }

        // Count only what will actually be written, or the reader expects more
        // records than follow and desyncs.
        std::uint32_t persistable = 0;
        for (const auto& [formID, st] : g_followers) {
            if (Followers::IsPersistableID(formID)) ++persistable;
        }
        a_intfc->WriteRecordData(persistable);

        // Record isolation: a WriteString failure below TRUNCATES this FLWR
        // record (counts already written overstate what follows), but FLWR is
        // ONE of FOUR independent records. Stop writing FLWR and STILL fall
        // through to MSTK/PRGN/FWPN -- a bare `return` here silently skipped
        // all three, so a load then lost every follower's stock gear,
        // progression and force-hold state, not just the truncated FLWR tail.
        // On the normal path this flag is never cleared and the write stream
        // is byte-identical to before.
        bool flwrOk = true;
        std::uint32_t written = 0, skippedRuntime = 0;
        for (const auto& [formID, st] : g_followers) {
            // Defence in depth. Followers::TryEnsureRecord should prevent a
            // 0xFF id ever getting in here; if one did, writing it would
            // silently re-attach the record to an unrelated form next session.
            if (!Followers::IsPersistableID(formID)) { ++skippedRuntime; continue; }
            a_intfc->WriteRecordData(formID);
            a_intfc->WriteRecordData(st.rapport);
            a_intfc->WriteRecordData(st.rank);
            // v4 (#65): the per-follower combat class override, right after rank.
            a_intfc->WriteRecordData(st.combatClassOverride);
            // v5 (T#78): the per-follower MFO master switch, one explicit u8 (0/1)
            // right after combatClassOverride. Written unconditionally at the
            // current schema (v5); the READ is version-gated so pre-v5 saves
            // (which never wrote it) never try to consume the byte.
            // (still v5, VALUE-SPACE only, ClickUp 86e3phy41): bits 1-2 carry the per-follower
            // helmet override (0 Default / 1 Wear / 2 Never), ONLY while mfoEnabled is true. An
            // older DLL reads `byte != 0`, so the enabled state it sees is unchanged; a disabled
            // follower still writes 0 and his override is not kept (Serialization.h, FLWR notes).
            const std::uint8_t helm = st.helmetOverride <= 2 ? st.helmetOverride : 0;
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(st.mfoEnabled ? (1u | (helm << 1)) : 0u));

            // Two tables, written in Table enum order (DESIGN.md 4.8).
            // The table COUNT is written explicitly so a future third table
            // is an append, not a reinterpretation of existing bytes.
            const auto tableCount = static_cast<std::uint8_t>(MFO::Table::kCount);
            a_intfc->WriteRecordData(tableCount);
            for (std::uint8_t t = 0; t < tableCount; ++t) {
                const auto& list = st.tables[t];
                // Clamp the COUNT and the LOOP together. Casting the count to
                // u8 while writing the whole list desyncs the stream at 256
                // entries -- everything after it becomes garbage.
                const size_t n = std::min<size_t>(list.size(), 0xFF);
                if (list.size() > n) {
                    spdlog::warn("[cosave] {:08X} table {}: {} rules truncated to {} -- "
                                 "silent drops are how 'the mod ate my save' starts",
                                 formID, t, list.size(), n);
                }
                const auto gambitCount = static_cast<std::uint8_t>(n);
                a_intfc->WriteRecordData(gambitCount);
                for (size_t gi = 0; gi < n; ++gi) {
                    const auto& g = list[gi];
                    if (!WriteString(a_intfc, g.conditionOpcode)) {
                        // The follower and gambit counts are already written, so
                        // the record is now TRUNCATED and overstates what
                        // follows. The reader will short-read and abort FLWR
                        // only. Say so plainly rather than leaving one terse
                        // error (F3), then break out to the sibling records.
                        spdlog::error("[cosave] WRITE FAILED mid-record for {:08X}. This save's FLWR "
                                      "record is TRUNCATED -- this follower and those after it will "
                                      "not load; stock-gear/progression/force-hold records are still "
                                      "written below. Re-save before relying on it.", formID);
                        flwrOk = false;
                        break;
                    }
                    a_intfc->WriteRecordData(g.conditionParam);
                    a_intfc->WriteRecordData(g.subjectSelector);
                    // v3 (#68): the specific-follower cast target, if any.
                    // Runtime (0xFF) actor ids are NEVER persisted
                    // (INVARIANTS #9) -- a summoned/cloned teammate targeted
                    // this session just falls back down the resolution
                    // ladder next load, same shape as an unresolvable
                    // actionParamForm below.
                    const RE::FormID subjActorOut =
                        Followers::IsPersistableID(g.subjectActorForm) ? g.subjectActorForm : 0;
                    a_intfc->WriteRecordData(subjActorOut);
                    if (!WriteString(a_intfc, g.actionOpcode)) {
                        spdlog::error("[cosave] WRITE FAILED mid-record for {:08X}. This save's FLWR "
                                      "record is TRUNCATED -- this follower and those after it will "
                                      "not load; stock-gear/progression/force-hold records are still "
                                      "written below. Re-save before relying on it.", formID);
                        flwrOk = false;
                        break;
                    }
                    a_intfc->WriteRecordData(g.actionParamForm);
                    // flags bit 0 = enabled. bit 1 = enabled, for a rule whose subjectSelector is an
                    // APPENDED Vocab::Subject (Enemy / Caster, 2026-10-06), and then bit 0 stays CLEAR:
                    // an older MFO.dll reads only bit 0, so it loads that rule DISABLED rather than
                    // resolving the unknown subject to its player-fallback rung (a curse or a damage
                    // spell aimed at the player). No version bump: the layout is unchanged, and a v6
                    // FLWR would make an older DLL skip (and on its next save destroy) every follower.
                    const bool appendedSubj = Vocab::IsAppendedSubject(g.subjectSelector);
                    const std::uint8_t flags =
                        !g.enabled ? 0u : (appendedSubj ? 2u : 1u);
                    a_intfc->WriteRecordData(flags);
                }
                if (!flwrOk) break;   // out of the table loop
            }
            if (!flwrOk) break;       // out of the follower loop -> MSTK/PRGN/FWPN below

            const size_t on = std::min<size_t>(st.overrides.size(), kMaxOverrides);
            if (st.overrides.size() > on) {
                spdlog::warn("[cosave] {:08X}: {} overrides truncated to {}",
                             formID, st.overrides.size(), on);
            }
            const auto overrideCount = static_cast<std::uint16_t>(on);
            a_intfc->WriteRecordData(overrideCount);
            for (size_t oi = 0; oi < on; ++oi) {
                const auto& o = st.overrides[oi];
                a_intfc->WriteRecordData(o.package);
                a_intfc->WriteRecordData(o.priority);
            }
            ++written;
        }

        // Log the zero case too (INVARIANTS.md #46): "saved nothing" and
        // "never ran" must not look identical. A truncated FLWR says so here
        // as well, so the summary line never reads as a clean save.
        spdlog::info("[cosave] saved {} follower record(s){}, schema v{}{}", written,
                     flwrOk ? "" : " (FLWR TRUNCATED -- see error above)", kSchemaVersion,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) record(s)", skippedRuntime)
                                    : std::string{});

        // T#69: stock gear (kRecStock/'MSTK') -- a SECOND, INDEPENDENT record.
        // Never bumps or touches kSchemaVersion/FLWR above. CopyStockGear()
        // takes the lock once and hands back a copy; everything below runs
        // lock-free over that copy (a lock must never be held across an
        // OpenRecord/WriteRecordData call).
        if (!a_intfc->OpenRecord(kRecStock, kStockVersion)) {
            spdlog::error("[cosave] OpenRecord('{}') failed -- stock gear NOT saved", "MSTK");
            return;
        }
        const auto stock = Logistics::CopyStockGear();
        std::uint32_t stockPersistable = 0;
        for (const auto& [formID, set] : stock) {
            if (Followers::IsPersistableID(formID)) ++stockPersistable;
        }
        a_intfc->WriteRecordData(stockPersistable);

        std::uint32_t stockWritten = 0, stockSkippedRuntime = 0;
        for (const auto& [formID, set] : stock) {
            if (!Followers::IsPersistableID(formID)) { ++stockSkippedRuntime; continue; }
            a_intfc->WriteRecordData(formID);
            const size_t n = std::min<size_t>(set.size(), kMaxStockGear);
            if (set.size() > n) {
                spdlog::warn("[cosave] {:08X}: {} stock-gear item(s) truncated to {}",
                             formID, set.size(), n);
            }
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(n));
            size_t emitted = 0;
            for (const auto base : set) {
                if (emitted >= n) break;
                a_intfc->WriteRecordData(base);
                ++emitted;
            }
            ++stockWritten;
        }
        spdlog::info("[cosave] saved stock-gear for {} follower(s), schema v{}{}", stockWritten, kStockVersion,
                     stockSkippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) record(s)", stockSkippedRuntime)
                                         : std::string{});

        // General follower-allocation-state slot (kRecProgression/'PRGN') -- a
        // THIRD independent record, same isolation contract as MSTK above. General
        // host machinery: it frames/versions/bounds a per-follower allocation-state
        // blob and holds no add-on judgment (Serialization.h §PRGN). Written even
        // when the add-on ESL is absent this session: the data must survive a
        // session where the user temporarily disabled MFO_Progression.esl, not be
        // silently destroyed by the next save. Layout + bounds live with the state
        // owner (ProgAllocator.cpp). The 'PRGN' fourCC + name are historical/frozen.
        ProgAllocator::CoSaveSave(a_intfc);

        // T#76 force-hold (kRecForcedWeapon/'FWPN') -- a FOURTH independent record,
        // same isolation contract. Persists the force-equip locks so a load can
        // clear stale ones (Actuation owns the layout + bounds).
        Actuation::CoSaveForcedWeapons(a_intfc);

        // 86e3faccn (kRecPlayerGiven/'PGIV') -- a FIFTH independent record, same
        // isolation contract: the museum relics the player gave a follower or put on
        // him (PlayerGiven owns the layout, bounds and the lock).
        PlayerGiven::CoSave(a_intfc);

        // 86e3t1mx7 (kRecErrand/'ERND') -- a SIXTH independent record, same isolation
        // contract: the town errands in progress (written only when one exists).
        WriteErrandRecord(a_intfc);
    }

    namespace {
        // Per-record READERS. Each record is loaded by its own
        // function so that a short read / implausible count inside one record
        // aborts THAT record only: the `return`s below leave the helper, and
        // LoadCallback's dispatch loop continues to the next record
        // (GetNextRecordInfo seeks past the unread remainder). PRGN and FWPN
        // already had this shape (ProgAllocator::CoSaveLoad,
        // Actuation::CoLoadForcedWeapons); MSTK and FLWR were inline and their
        // `return`s left LoadCallback itself, abandoning every sibling record
        // unread -- which the next save then wrote back EMPTY (INVARIANTS #12:
        // SKSE does not round-trip unread records). Bodies are moved verbatim.

        // 'MSTK' body (after the version guard). Reads the whole record or
        // aborts it with a log line; never touches g_followers.
        void ReadStockRecord(SKSE::SerializationInterface* a_intfc) {
            std::uint32_t followerCount = 0;
            if (!a_intfc->ReadRecordData(followerCount)) {
                spdlog::error("[cosave] short read on stock-gear follower count -- ABORTING stock load");
                return;
            }
            if (followerCount > kMaxFollowers) {
                spdlog::error("[cosave] implausible stock-gear follower count {} -- ABORTING stock load",
                              followerCount);
                return;
            }
            std::uint32_t stockLoaded = 0, stockDroppedActor = 0, stockDroppedItem = 0;
            for (std::uint32_t i = 0; i < followerCount; ++i) {
                RE::FormID rawID = 0;
                if (!a_intfc->ReadRecordData(rawID)) {
                    spdlog::error("[cosave] short read at stock-gear follower {}/{} -- ABORTING stock load",
                                  i, followerCount);
                    return;
                }
                std::uint32_t itemCount = 0;
                if (!a_intfc->ReadRecordData(itemCount)) {
                    spdlog::error("[cosave] short read on stock-gear item count -- ABORTING stock load");
                    return;
                }
                if (itemCount > kMaxStockGear) {
                    spdlog::error("[cosave] implausible stock-gear item count {} -- ABORTING stock load",
                                  itemCount);
                    return;
                }

                RE::FormID resolvedID = 0;
                const bool resolved = a_intfc->ResolveFormID(rawID, resolvedID);

                std::unordered_set<RE::FormID> set;
                for (std::uint32_t bi = 0; bi < itemCount; ++bi) {
                    RE::FormID rawBase = 0;
                    // NOTE: read unconditionally, even when the follower
                    // itself won't resolve -- bailing early would desync
                    // the byte stream for every record after this one.
                    if (!a_intfc->ReadRecordData(rawBase)) {
                        spdlog::error("[cosave] short read at stock-gear item {}/{} -- ABORTING stock load",
                                      bi, itemCount);
                        return;
                    }
                    if (!resolved) continue;   // actor gone -- whole set goes with it, below
                    RE::FormID resolvedBase = 0;
                    if (a_intfc->ResolveFormID(rawBase, resolvedBase)) {
                        set.insert(resolvedBase);
                    } else {
                        ++stockDroppedItem;   // INVARIANTS #8: DROP, never fabricate
                    }
                }

                if (!resolved) { ++stockDroppedActor; continue; }
                Logistics::LoadStockRecord(resolvedID, std::move(set));
                ++stockLoaded;
            }
            spdlog::info("[cosave] loaded stock-gear for {} follower(s); dropped {} unresolvable "
                         "actor(s), {} unresolvable item(s)",
                         stockLoaded, stockDroppedActor, stockDroppedItem);
        }

        // 'FLWR' body (after the version guard). Followers parsed before an
        // abort are KEPT (already in g_followers). a_complete is set only when
        // the record was consumed to the end, so the caller can say which.
        void ReadFollowersRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t version,
                                 bool& a_complete, std::uint32_t& loaded, std::uint32_t& droppedActor,
                                 std::uint32_t& disabledRules, std::uint32_t& droppedOverride,
                                 std::uint32_t& collisions) {
            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(count)) {
                spdlog::error("[cosave] short read on follower count -- ABORTING, "
                              "keeping {} follower(s) parsed so far", loaded);
                return;
            }
            if (count > kMaxFollowers) {
                spdlog::error("[cosave] implausible follower count {} (max {}) -- ABORTING", count, kMaxFollowers);
                return;
            }

            for (std::uint32_t i = 0; i < count; ++i) {
                RE::FormID rawID = 0;
                if (!a_intfc->ReadRecordData(rawID)) {
                    spdlog::error("[cosave] short read at follower {}/{} -- ABORTING", i, count);
                    return;
                }

                // INVARIANTS.md #8: resolve or DROP. Never guess.
                RE::FormID resolvedID = 0;
                const bool resolved = a_intfc->ResolveFormID(rawID, resolvedID);

                FollowerState st{};
                if (!a_intfc->ReadRecordData(st.rapport)) return;
                if (!a_intfc->ReadRecordData(st.rank)) return;
                st.rank = std::clamp<std::uint8_t>(st.rank, 1, kMaxRank);

                // v4 (#65): STRICTLY version-gated, same discipline as the v3
                // subjectActorForm read below -- a v1/v2/v3 record never wrote
                // this field, so reading it unconditionally would consume the
                // next field's bytes and desync the rest of the stream.
                // st.combatClassOverride already defaults to 0 (FollowerState{}),
                // exactly "Auto / no override" for every pre-v4 record.
                if (version >= 4) {
                    if (!a_intfc->ReadRecordData(st.combatClassOverride)) return;
                    st.combatClassOverride = std::clamp<std::uint8_t>(st.combatClassOverride, 0, 3);
                }

                // v5 (T#78): the per-follower MFO master switch, STRICTLY version-
                // gated -- same discipline as v3/v4 above. A v1-v4 record never
                // wrote this byte, so reading it unconditionally would consume the
                // next field's bytes and desync the stream. st.mfoEnabled already
                // defaults to true (FollowerState{}), exactly "MFO enabled" for
                // every pre-v5 record.
                if (version >= 5) {
                    std::uint8_t mfoEnabledByte = 1;
                    if (!a_intfc->ReadRecordData(mfoEnabledByte)) return;
                    // Bit 0 = enabled; bits 1-2 = the helmet override (86e3phy41, value-space only).
                    // Every record written before it holds 0 or 1 here and reads unchanged. A
                    // stray value (bits 1-2 == 3, or bits 1-2 set without bit 0) is clamped to
                    // Default (#11), and enabled stays `byte != 0`, exactly what an older DLL reads.
                    st.mfoEnabled = (mfoEnabledByte != 0);
                    const std::uint8_t helm = static_cast<std::uint8_t>((mfoEnabledByte >> 1) & 3u);
                    st.helmetOverride = ((mfoEnabledByte & 1u) != 0 && helm <= 2) ? helm : 0;
                }

                std::uint8_t tableCount = 0;
                if (!a_intfc->ReadRecordData(tableCount)) return;
                if (tableCount > static_cast<std::uint8_t>(MFO::Table::kCount)) {
                    // A NEWER save with more tables than this build knows.
                    // The version guard above should already have caught it;
                    // refuse rather than misread the byte stream.
                    spdlog::error("[cosave] record declares {} tables, this build knows {} -- ABORTING",
                                  tableCount, static_cast<int>(MFO::Table::kCount));
                    return;
                }

                for (std::uint8_t t = 0; t < tableCount; ++t) {
                    std::uint8_t gambitCount = 0;
                    if (!a_intfc->ReadRecordData(gambitCount)) return;

                    // Clamp to THIS TABLE's slot maximum for the rank, not a
                    // global constant -- combat and logistics differ.
                    const auto table = static_cast<MFO::Table>(t);
                    const std::uint8_t slotMax = SlotsForRank(st.rank, table);
                    if (gambitCount > slotMax) {
                        // raw AND resolved: resolvedID is 0 when resolution
                        // failed, which made this line print 00000000.
                        spdlog::warn("[cosave] follower raw {:08X} / resolved {:08X} table {}: "
                                     "{} gambits exceeds rank {} max {} -- extra rules DROPPED",
                                     rawID, resolvedID, t, gambitCount, st.rank, slotMax);
                    }

                    for (std::uint8_t gi = 0; gi < gambitCount; ++gi) {
                        Gambit g{};
                        if (!ReadString(a_intfc, g.conditionOpcode)) return;
                        if (!a_intfc->ReadRecordData(g.conditionParam)) return;
                        if (!a_intfc->ReadRecordData(g.subjectSelector)) return;

                        // v3 (#68): STRICTLY version-gated -- a v1/v2 record
                        // never wrote this field, so reading it unconditionally
                        // would consume the next field's bytes and desync the
                        // whole rest of the stream. g.subjectActorForm already
                        // defaults to 0 (Gambit{}), which is exactly "use
                        // subjectSelector instead" -- the pre-#68 behaviour.
                        if (version >= 3) {
                            RE::FormID rawSubjActor = 0;
                            if (!a_intfc->ReadRecordData(rawSubjActor)) return;
                            if (rawSubjActor != 0) {
                                RE::FormID resolvedSubjActor = 0;
                                if (a_intfc->ResolveFormID(rawSubjActor, resolvedSubjActor)) {
                                    g.subjectActorForm = resolvedSubjActor;
                                }
                                // else: that follower is gone. Leave it 0 --
                                // DESIGN's ladder falls back to subjectSelector,
                                // never disables the rule (unlike a missing
                                // actionParamForm, which has no fallback rung).
                            }
                        }

                        if (!ReadString(a_intfc, g.actionOpcode)) return;

                        RE::FormID rawParam = 0;
                        if (!a_intfc->ReadRecordData(rawParam)) return;
                        std::uint8_t flags = 0;
                        if (!a_intfc->ReadRecordData(flags)) return;
                        // bit 0 OR bit 1 (the appended-subject enabled bit, see the writer). Every
                        // record written before 2026-10-06 holds 0 or 1 here, so it reads unchanged.
                        g.enabled = (flags & 3u) != 0;

                        if (rawParam != 0) {
                            RE::FormID resolvedParam = 0;
                            if (a_intfc->ResolveFormID(rawParam, resolvedParam)) {
                                g.actionParamForm = resolvedParam;
                            } else {
                                // DESIGN.md §3.3: disable THIS rule, with a
                                // marker. Never guess, never drop the whole list.
                                g.actionParamForm = 0;
                                g.enabled = false;
                                g.lastFailReason = "action target missing from load order";
                                ++disabledRules;
                            }
                        }
                        // MIGRATION (#35): "Equip torch" moved combat->logistics. A
                        // legacy rule in the COMBAT table (t==0) would now hit the
                        // fail-closed unknown-action path and, since combat has no
                        // fall-through, WIN every tick and block every lower combat
                        // rule. Redirect it into the logistics table instead of
                        // dropping it, so the follower still lights up on schedule.
                        if (t == 0 && g.actionOpcode == "act.equip_torch") {
                            st.tables[1].push_back(std::move(g));
                            continue;
                        }
                        // NOTE: the read must CONSUME every gambit even when
                        // over the slot cap -- bailing early would desync the
                        // byte stream for everything after it.
                        if (gi < slotMax) st.tables[t].push_back(std::move(g));
                    }
                }

                // v1 carried a tutored-spell block here. Consume and discard
                // it: skipping the READ would desync every byte after it.
                if (version == 1) {
                    std::uint16_t tutoredCount = 0;
                    if (!a_intfc->ReadRecordData(tutoredCount)) return;
                    if (tutoredCount > kMaxTutoredV1) {
                        spdlog::error("[cosave] v1 tutored count {} implausible -- ABORTING", tutoredCount);
                        return;
                    }
                    for (std::uint16_t ti = 0; ti < tutoredCount; ++ti) {
                        RE::FormID rawSpell = 0;
                        std::uint32_t grantedAt = 0;
                        if (!a_intfc->ReadRecordData(rawSpell)) return;
                        if (!a_intfc->ReadRecordData(grantedAt)) return;
                    }
                    if (tutoredCount) {
                        spdlog::info("[cosave] discarded {} v1 tutored-spell entr(ies) -- "
                                     "acquisition is out of scope; the spells themselves are "
                                     "untouched on the actor", tutoredCount);
                    }
                }

                std::uint16_t overrideCount = 0;
                if (!a_intfc->ReadRecordData(overrideCount)) return;
                if (overrideCount > kMaxOverrides) {
                    spdlog::error("[cosave] implausible override count {} -- ABORTING", overrideCount);
                    return;
                }
                for (std::uint16_t oi = 0; oi < overrideCount; ++oi) {
                    PackageOverride o{};
                    RE::FormID rawPkg = 0;
                    if (!a_intfc->ReadRecordData(rawPkg)) return;
                    if (!a_intfc->ReadRecordData(o.priority)) return;
                    RE::FormID resolvedPkg = 0;
                    if (a_intfc->ResolveFormID(rawPkg, resolvedPkg)) {
                        o.package = resolvedPkg;
                        st.overrides.push_back(o);
                    } else {
                        // A dropped record gets a counter and a line. Silent
                        // drops are how "the mod ate my save" starts.
                        ++droppedOverride;
                    }
                }

                if (!resolved) {
                    ++droppedActor;
                    continue;   // the actor is gone; its whole record goes
                }
                if (g_followers.contains(resolvedID)) {
                    // Two raw ids resolving to one -- the first record would be
                    // silently overwritten.
                    ++collisions;
                    spdlog::warn("[cosave] {:08X} already loaded; a second record resolved to the "
                                 "same id and OVERWRITES it", resolvedID);
                }
                // MIGRATION BACKFILL: a record saved before default gambits
                // existed loads with both tables empty and, since load assigns
                // g_followers[] directly (never TryEnsureRecord), would stay
                // blank forever. A fully-empty board is never a desirable end
                // state, so give it the base kit. Any record with even one rule
                // is left exactly as authored.
                if (st.combat().empty() && st.logistics().empty()) {
                    Followers::ApplyDefaultKit(st);
                    spdlog::info("[cosave] {:08X} had an empty board -- backfilled default kit", resolvedID);
                }
                g_followers[resolvedID] = std::move(st);
                ++loaded;
            }
            a_complete = true;
        }
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        g_followers.clear();
        Logistics::ClearStockGear();   // T#69: defence in depth -- RevertCallback already
                                        // cleared it, same belt-and-braces as g_followers above
        PlayerGiven::ClearRecord();    // 86e3faccn: same belt-and-braces for 'PGIV'
        Logistics::Errand::ClearRestorePending();   // 86e3t1mx7: same belt-and-braces for 'ERND'

        std::uint32_t type = 0, version = 0, length = 0;
        std::uint32_t loaded = 0, droppedActor = 0, disabledRules = 0,
                      droppedOverride = 0, collisions = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == kRecStock) {
                // T#69: independent of FLWR -- its own version guard, its own
                // ResolveFormID/DROP discipline (INVARIANTS #8), never
                // fabricates. A newer stock record this DLL can't fully parse
                // is skipped (GetNextRecordInfo seeks past it on the next
                // loop, same as any unknown record type below) rather than
                // aborting the whole load -- it's a separate record, not a
                // mid-stream desync risk like a newer FLWR would be.
                if (version > kStockVersion) {
                    spdlog::error("[cosave] STOCK-GEAR SAVE IS NEWER (v{}) THAN THIS DLL (v{}) -- "
                                  "skipped; it WILL BE DESTROYED if this DLL saves over this file.",
                                  version, kStockVersion);
                    g_sawNewerSave.store(true);   // surfaced on-screen at kPostLoadGame
                    continue;
                }
                ReadStockRecord(a_intfc);
                continue;
            }
            if (type == kRecProgression) {
                // Same independence contract as MSTK: own version guard, own
                // ResolveFormID/DROP discipline; a newer PRGN skips THIS
                // record only (GetNextRecordInfo seeks past it), never aborts
                // the whole load.
                if (version > kProgVersion) {
                    spdlog::error("[cosave] PROGRESSION SAVE IS NEWER (v{}) THAN THIS DLL (v{}) -- "
                                  "skipped; it WILL BE DESTROYED if this DLL saves over this file.",
                                  version, kProgVersion);
                    g_sawNewerSave.store(true);   // surfaced on-screen at kPostLoadGame
                    continue;
                }
                ProgAllocator::CoSaveLoad(a_intfc, version);
                continue;
            }
            if (type == kRecForcedWeapon) {
                // T#76: same independence contract. A newer record skips (never
                // aborts). CoLoad resolves + releases every stale force-lock.
                if (version > kForcedWeaponVersion) {
                    spdlog::error("[cosave] FORCED-WEAPON SAVE IS NEWER (v{}) THAN THIS DLL (v{}) -- "
                                  "skipped; a mid-hold force-lock in it will NOT be cleared.",
                                  version, kForcedWeaponVersion);
                    g_sawNewerSave.store(true);
                    continue;
                }
                Actuation::CoLoadForcedWeapons(a_intfc, version);
                continue;
            }
            if (type == kRecPlayerGiven) {
                // 86e3faccn: same independence contract. A newer record skips (never
                // aborts); an old save without it simply never reaches here (empty).
                if (version > kPlayerGivenVersion) {
                    spdlog::error("[cosave] PLAYER-GIVEN SAVE IS NEWER (v{}) THAN THIS DLL (v{}) -- "
                                  "skipped; it WILL BE DESTROYED if this DLL saves over this file.",
                                  version, kPlayerGivenVersion);
                    g_sawNewerSave.store(true);
                    continue;
                }
                PlayerGiven::CoLoad(a_intfc, version);
                continue;
            }
            if (type == kRecErrand) {
                // 86e3t1mx7 (town R2): same independence contract. A newer record skips (never
                // aborts); an old save without it simply never reaches here (nothing staged).
                if (version > kErrandVersion) {
                    spdlog::error("[cosave] ERRAND SAVE IS NEWER (v{}) THAN THIS DLL (v{}) -- "
                                  "skipped; it WILL BE DESTROYED if this DLL saves over this file.",
                                  version, kErrandVersion);
                    g_sawNewerSave.store(true);
                    continue;
                }
                if (version < 1) {   // MFO-B309: no shipped writer produced v0; never parse it as v1 (#11)
                    spdlog::error("[cosave] ERND record with version {} (no such schema) -- skipped, nothing staged", version);
                    continue;
                }
                ReadErrandRecord(a_intfc, version);
                continue;
            }
            if (type != kRecFollowers) {
                spdlog::warn("[cosave] unknown record type {:08X} -- skipped", type);
                continue;
            }

            // INVARIANTS.md #12: SKSE does not round-trip unread records. A
            // newer save read by this older DLL loses data on the next save.
            // Warn LOUDLY -- never log a comforting falsehood.
            //
            // CONTINUE, not return: this record is INDEPENDENT of MSTK/PRGN/FWPN,
            // and GetNextRecordInfo seeks past this unread FLWR body on the next
            // loop (same as the sibling newer-version guards above). Returning
            // here abandoned those siblings unread, so a downgraded DLL wrote them
            // back EMPTY on the next save -- destroying stock-gear/progression/
            // force-hold data the user could otherwise recover by re-upgrading.
            // We read nothing from the FLWR body before this point, so there is no
            // mid-stream desync to fear.
            if (version > kSchemaVersion) {
                spdlog::error("[cosave] SAVE IS NEWER (v{}) THAN THIS DLL (v{}). "
                              "Records this build cannot read WILL BE DESTROYED on the next save. "
                              "Do not save over this file with this version.",
                              version, kSchemaVersion);
                g_sawNewerSave.store(true);   // surfaced on-screen at kPostLoadGame
                continue;
            }

            bool complete = false;
            ReadFollowersRecord(a_intfc, version, complete, loaded, droppedActor,
                                disabledRules, droppedOverride, collisions);
            if (!complete) {
                // Truncated/short FLWR: the record-local abort above already
                // logged the site. State it in terms of what SURVIVES, and
                // CONTINUE so MSTK/PRGN/FWPN still load (they are written
                // independently of a truncated FLWR -- SaveCallback).
                spdlog::error("[cosave] FLWR record ABORTED mid-stream -- keeping {} follower(s) "
                              "parsed so far; stock-gear/progression/force-hold records still "
                              "load independently", loaded);
            }
        }

        // INVARIANTS.md #47: split the skip counters by REASON. A single
        // aggregate hides a systematic failure inside ordinary attrition.
        spdlog::info("[cosave] loaded {} follower(s); dropped {} unresolvable actor(s), "
                     "{} unresolvable override(s); disabled {} rule(s) with missing targets; "
                     "{} id collision(s)",
                     loaded, droppedActor, droppedOverride, disabledRules, collisions);
    }

    bool ConsumeNewerSaveWarning() { return g_sawNewerSave.exchange(false); }

    void RevertCallback(SKSE::SerializationInterface*) {
        ResetAllState();
        spdlog::info("[cosave] revert -- all save-scoped state cleared");
    }

    void ResetAllState() {
        // STOP + DRAIN THE PUMP FIRST. Every clear below wipes a save-scoped map
        // that a job-worker tick inserts into; StopPump now waits for any in-flight
        // tick to finish, so the clears can't race a concurrent insert (audit:
        // concurrent unordered_map insert+clear is UB). Restarted on the next
        // kPostLoadGame/kNewGame. (Was called LAST here, after the clears -- the race.)
        Diagnostics::StopPump();
        // Drop any main-thread work queued before the load -- its captured handles
        // would re-resolve against the NEXT session's (reused) handle table. Safe
        // now that StopPump halted the worker that feeds the queue (audit).
        MainThread::Clear();
        SelfStatus::Clear();   // the poisoned-state mirror (gambit conditions): a pre-load answer must not survive the load
        g_followers.clear();
        // ARCHITECTURE §7: revert zeroes EVERYTHING save-scoped. The active
        // handle list is save-scoped too -- handle slots get reused, so a
        // stale handle from the previous save can resolve to a DIFFERENT
        // actor, and the sleeper pump can queue a Refresh into the load window
        // and read them.
        Followers::g_active.clear();
        Followers::ClearTransientState();   // streak map is save-scoped (F1)
        Scheduler::ClearTransientState();   // suppression + round-robin cursor likewise
        Logistics::ClearTransientState();   // logistics cadence clocks + loot LRUs (#22h)
        Logistics::ClearStyleMirror();      // perk-style mirror + its Post in-flight latches (Fable F4)
        // T#69: g_stockGear IS serialized (kRecStock), unlike everything
        // ClearTransientState owns -- kept as its own call so that contract
        // stays true. A load right after this repopulates it; a main-menu
        // revert with no load leaves it empty, same as g_followers above.
        Logistics::ClearStockGear();
        // Progression state IS serialized too (kRecProgression) -- same
        // contract as stock gear. ClearAll also orphans the level-poll chain
        // (generation bump) so a stale tick never runs against the next save.
        ProgAllocator::ClearAll();
        // 86e3faccn: the player-given relic record IS serialized too (kRecPlayerGiven)
        // -- same contract as stock gear: its own call, after StopPump (its queued
        // writes are PumpTickGate'd; its main-thread recounts were dropped by
        // MainThread::Clear above). A load right after this repopulates it.
        PlayerGiven::ClearRecord();
        // 86e3t1mx7 (town R2): the 'ERND' entries a load staged for adoption. Logistics::
        // ClearTransientState above already dropped them with every live errand (Errand::
        // ClearTransientState); kept as its own call beside PGIV so the serialized-record
        // contract reads in one place. A load right after this stages them again.
        Logistics::Errand::ClearRestorePending();
        // The equip ledger describes a LIVE loadout. On revert the world is
        // about to be replaced, so there is nothing to give back -- but the
        // ledger must not survive to re-equip gear into the next save (#16).
        Loadout::ClearTransientState();
        Actuation::ClearSelfCasts();   // drop forced self-cast + AUTO fan-out pacing state (no equip/stow to undo)
        // The latch is a live commanded target; it cannot outlive the world.
        Targeting::ClearAll();
        CasterConsent::ClearTransientState();
        CombatStyle::ClearAll();            // owned stances are this-session,
                                            // per-combat controller identities
        Actuation::ClearForcedWeapons();    // T#76: equip force-hold records are
                                            // this-session too, exactly like the
                                            // owned stances above -- the world is
                                            // being replaced, no engine call
        Sightline::ClearTransientState();   // LoS cache keys are this-session
                                            // FormID pairs -- same rule as the latch
        Board::ClearPendingEdits();
        // The alias fill is the one piece of MFO state the ENGINE persists for
        // us whether we want it or not, so revert cannot just forget it the way
        // it forgets the ledgers above -- forgetting is exactly how a follower
        // comes back latched. ReleaseAll re-reads the alias from the quest and
        // clears it even when this module believes it holds nothing, because
        // after a revert that belief is worth nothing (#16).
        Packages::ReleaseAll("revert");
        Packages::ClearTransientState();  // #53 heartbeat counters + retreat hold are session-scoped like every sibling
        Papyrus::ClearTransientState();   // counters are session-scoped like every sibling
        TradeBridge::ClearTransientState();   // #21: drop pending trade orders -- their
                                              // actor/chest handles are this-session only,
                                              // and a resumed RunTrade with a stale token
                                              // fails SAFE (GetVendorChest -> none -> abort)
        MEOBridge::ClearTransientState();   // audit: pending gem-move map was never cleared
        Logistics::GemRules::ClearTransient();   // looted-gear gem probes (main-thread ledger) + the probed-key set
        Diagnostics::ResetAttackObserve();  // [atk-obs] slot table is session-scoped like every sibling (worker drained above)
        Probe::ReleaseAll();                // audit: watch handles outlived a revert (were
                                            // cleared only at kPostLoadGame, not here)
        Rapport::ResetSessionCounters();
        // (StopPump moved to the TOP of this function -- it must precede the clears.)
        Board::SetHud(false);      // else it lingers over the main menu with stale rows
    }

}
