#include "PCH.h"
#include "State.h"
#include "Serialization.h"
#include "Followers.h"
#include "Rapport.h"
#include "Diagnostics.h"
#include "Scheduler.h"
#include "Logistics.h"
#include "Loadout.h"
#include "Targeting.h"
#include "CasterConsent.h"
#include "Packages.h"
#include "Papyrus.h"
#include "TradeBridge.h"
#include "Board.h"

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
            if (!a_intfc->ReadRecordData(len)) return false;
            if (len > kMaxOpcodeLen) {
                spdlog::error("[cosave] opcode length {} exceeds max {} -- aborting read", len, kMaxOpcodeLen);
                return false;
            }
            a_out.resize(len);
            if (len == 0) return true;
            return a_intfc->ReadRecordData(a_out.data(), len) == len;
        }
    }

    void SaveCallback(SKSE::SerializationInterface* a_intfc) {
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

        std::uint32_t written = 0, skippedRuntime = 0;
        for (const auto& [formID, st] : g_followers) {
            // Defence in depth. Followers::TryEnsureRecord should prevent a
            // 0xFF id ever getting in here; if one did, writing it would
            // silently re-attach the record to an unrelated form next session.
            if (!Followers::IsPersistableID(formID)) { ++skippedRuntime; continue; }
            a_intfc->WriteRecordData(formID);
            a_intfc->WriteRecordData(st.rapport);
            a_intfc->WriteRecordData(st.rank);

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
                        // follows. The reader will short-read and abort. Say so
                        // plainly rather than leaving one terse error (F3).
                        spdlog::error("[cosave] WRITE FAILED mid-record for {:08X}. This save's MFO "
                                      "data is TRUNCATED -- followers after this one will not load. "
                                      "Re-save before relying on it.", formID);
                        return;
                    }
                    a_intfc->WriteRecordData(g.conditionParam);
                    a_intfc->WriteRecordData(g.subjectSelector);
                    if (!WriteString(a_intfc, g.actionOpcode)) {
                        spdlog::error("[cosave] WRITE FAILED mid-record for {:08X}. This save's MFO "
                                      "data is TRUNCATED -- followers after this one will not load. "
                                      "Re-save before relying on it.", formID);
                        return;
                    }
                    a_intfc->WriteRecordData(g.actionParamForm);
                    const std::uint8_t flags = g.enabled ? 1u : 0u;
                    a_intfc->WriteRecordData(flags);
                }
            }

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
        // "never ran" must not look identical.
        spdlog::info("[cosave] saved {} follower record(s), schema v{}{}", written, kSchemaVersion,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) record(s)", skippedRuntime)
                                    : std::string{});
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        g_followers.clear();

        std::uint32_t type = 0, version = 0, length = 0;
        std::uint32_t loaded = 0, droppedActor = 0, disabledRules = 0,
                      droppedOverride = 0, collisions = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecFollowers) {
                spdlog::warn("[cosave] unknown record type {:08X} -- skipped", type);
                continue;
            }

            // INVARIANTS.md #12: SKSE does not round-trip unread records. A
            // newer save read by this older DLL loses data on the next save.
            // Warn LOUDLY -- never log a comforting falsehood.
            if (version > kSchemaVersion) {
                spdlog::error("[cosave] SAVE IS NEWER (v{}) THAN THIS DLL (v{}). "
                              "Records this build cannot read WILL BE DESTROYED on the next save. "
                              "Do not save over this file with this version.",
                              version, kSchemaVersion);
                g_sawNewerSave.store(true);   // surfaced on-screen at kPostLoadGame
                return;
            }

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
                        if (!ReadString(a_intfc, g.actionOpcode)) return;

                        RE::FormID rawParam = 0;
                        if (!a_intfc->ReadRecordData(rawParam)) return;
                        std::uint8_t flags = 0;
                        if (!a_intfc->ReadRecordData(flags)) return;
                        g.enabled = (flags & 1u) != 0;

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
        // The equip ledger describes a LIVE loadout. On revert the world is
        // about to be replaced, so there is nothing to give back -- but the
        // ledger must not survive to re-equip gear into the next save (#16).
        Loadout::ClearTransientState();
        // The latch is a live commanded target; it cannot outlive the world.
        Targeting::ClearAll();
        CasterConsent::ClearTransientState();
        Board::ClearPendingEdits();
        // The alias fill is the one piece of MFO state the ENGINE persists for
        // us whether we want it or not, so revert cannot just forget it the way
        // it forgets the ledgers above -- forgetting is exactly how a follower
        // comes back latched. ReleaseAll re-reads the alias from the quest and
        // clears it even when this module believes it holds nothing, because
        // after a revert that belief is worth nothing (#16).
        Packages::ReleaseAll("revert");
        Papyrus::ClearTransientState();   // counters are session-scoped like every sibling
        TradeBridge::ClearTransientState();   // #21: drop pending trade orders -- their
                                              // actor/chest handles are this-session only,
                                              // and a resumed RunTrade with a stale token
                                              // fails SAFE (GetVendorChest -> none -> abort)
        Rapport::ResetSessionCounters();
        // (StopPump moved to the TOP of this function -- it must precede the clears.)
        Board::SetHud(false);      // else it lingers over the main menu with stale rows
    }

}
