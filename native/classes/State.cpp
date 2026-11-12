// classes/State.cpp -- per-follower class state and its co-save record 'MCLS' (class update card C3).
//
// WHAT IT HOLDS: per follower, the equipped class, XP + level for every class, the mastered / unlocked
// bitsets, the Thief / Master Thief stealth-entry fight counters (card C13), the COMBAT and the LOGISTICS
// gambit table saved under each class he switched away from (marth Q8 / Q29, card C12), the tables he had
// with classes OFF, and Dragonborn's picked T4 (marth Q60). Nothing here accrues XP, decides mastery,
// applies a gambit or touches the engine: those are cards C4 / C5 / C6 / C12. This file only holds, guards
// and persists the state, and exposes the accessors (Classes.h) those cards call.
//
// ── 'MCLS' BYTE LAYOUT (little-endian, SKSE record body). v1 = everything down to the v1 tables; v2 (card
//    C12) APPENDS the block marked "v2" to every follower entry and changes NOTHING before it. ───────────
//   header
//     u32  rowSchema       the gambit-row version the rows below were written at, in FLWR's version numbers
//                          (Serialization.h GambitWire::kRowSchema = the FLWR version at which the row last
//                          changed; card G9 made it its own constant, MFO-B569). Written = kRowSchema (6 since G9;
//                          a pre-G9 DLL wrote kSchemaVersion = 5, the same value space). Read: 1..kRowSchema; above ->
//                          KEPT VERBATIM; 0 -> corrupt, ABORTED (nothing kept).
//     u8   classCount      classes per follower in this record; written = kClassCount (35). Read: 1..35; above ->
//                          KEPT VERBATIM; 0 -> corrupt, ABORTED (nothing kept).
//     u32  followerCount   <= 4096 (#11; more = the record is ABORTED)
//   per follower (followerCount times)
//     u32  followerFormID  ResolveFormID; unresolvable -> the entry is consumed and DROPPED (#8)
//     u8   equipped        an Id ordinal (< classCount) or 0xFF = none; anything else -> none (logged, #11)
//     per class i in [0, classCount): u32 xp (whole XP points), u8 level
//     u64  mastered        bit i = Id i mastered; bits >= classCount are dropped on read (logged, #11)
//     u64  unlocked        same rule
//     u16  thiefFights
//     u16  masterThiefFights
//     u8   tableCount      stored class tables that follow, <= classCount (more = ABORTED)
//     per table
//       u8 classId         < classCount, unique within the follower (otherwise ABORTED)
//       u8 rowCount        <= kMaxStoredRows = 64 (more = ABORTED)
//       rowCount x ROW     the FLWR gambit row, GambitWire::WriteRow / ReadRow at rowSchema (Serialization.h:
//                          str cond, f32 param, u8 subject, u32 subjectActor, str action, u32 actionParam,
//                          u8 flags, then u16 cooldownTenths ONLY when rowSchema >= 6 (card G9); str = u32 len
//                          <= 64 + bytes). An unresolvable actionParam DISABLES that rule with a marker, as FLWR
//                          does; an unresolvable subjectActor reads as 0. A rowSchema-5 row (written by a pre-G9
//                          DLL) ends at its flags byte and reads with cooldown 0; a pre-G9 DLL meets rowSchema 6
//                          and KEEPS the record verbatim (the rule below), so a downgrade freezes, never loses, it.
//     ── v2 only (read under `a_version >= 2`; a v1 entry ENDS at its last v1 table) ──
//     u8   pickedT4        marth Q60, Dragonborn's picked T4 combat behaviour: the Id ordinal of a TIER-4 class
//                          (< classCount) or 0xFF = none; anything else -> none (logged, #11)
//     u8   logiTableCount  stored LOGISTICS tables that follow (marth Q29), <= classCount (more = ABORTED)
//     per logistics table
//       u8 classId         < classCount, unique within the follower's logistics tables (otherwise ABORTED)
//       u8 rowCount        <= kMaxStoredRows = 64 (more = ABORTED)
//       rowCount x ROW     the same row as above, at rowSchema
//     u8   offMask         the tables he had with classes OFF: bit 0 = a COMBAT table follows, bit 1 = a
//                          LOGISTICS table follows (combat first). Any other bit = ABORTED (no writer sets one)
//     per set bit: u8 rowCount (<= 64, more = ABORTED), rowCount x ROW
//   Followers are written in ascending FormID order and tables in Id order, so the bytes are deterministic.
//   SIZES: a follower's v1 part is 201 bytes + its tables; the v2 block is 3 bytes + its tables. A row is at
//   most 152 bytes (two 64-byte opcodes), a table at most 64 rows, so one follower is bounded at about 0.7 MB
//   and a real one is a few hundred bytes; the whole record is refused over kMaxRecordBytes.
//
// ── VERSION AND COMPAT RULES ────────────────────────────────────────────────────────────────────────
//   * Extension is BY VERSION, never by blob size: v2 APPENDS fields at the end of the per-follower entry,
//     read under `if (a_version >= 2)`; the v1 reader stays forever (#12). A new class APPENDS an Id (Classes.h
//     rule): the writer then writes the larger classCount, and an older DLL meets classCount > its kClassCount.
//   * v1 -> v2: a v1 record is read by the SAME code with the v2 block skipped, so its followers load with no
//     picked T4, no logistics tables and no classes-OFF tables; the next save writes v2. v2 -> v1 DLL: the older
//     DLL meets version 2 > its 1 and KEEPS the record verbatim (the rule below), frozen for that session.
//   * A record this DLL cannot parse -- a version > kClassesVersion, a rowSchema > GambitWire::kRowSchema (a newer FLWR
//     row, e.g. card G9's cooldown) or a classCount > kClassCount -- is KEPT VERBATIM (its version + bytes) and
//     written back unchanged on every save; the class state is then FROZEN for the session (every setter
//     refuses), so the kept bytes and the live state can never disagree. Logged loudly once per load, and shown ONCE on screen at
//     kPostLoadGame ("class progress is read-only this session", plugin.cpp via ConsumeReadOnlyNotice; review F-1).
//     A ZERO rowSchema or classCount is corruption, not "newer": ABORTED, nothing kept (review F-2).
//     CAVEAT: kept bytes carry the FormIDs as they were at THAT save; if the load order changed meanwhile they
//     will resolve wrongly when the newer DLL reads them back. Only a parse (this DLL cannot) could fix that.
//   * Version 0 is no schema: skipped, nothing kept (the ERND / MFO-B309 precedent).
//   * A short read or an over-bound count ABORTS the record: the followers parsed before it are KEPT, the rest
//     is lost at the next save (the FLWR / PGIV precedent, "keep what parsed"); logged loudly.
//
// ── ESL ABSENT OR bClassTree = 0 (marth Q9: class progress is KEPT) ─────────────────────────────────
//   The record holds no form of the class ESL, so it is PARSED whatever the add-on's presence, and the parsed
//   state is held UNCHANGED (every setter refuses while !Active()) and written back on every save. The progress
//   is the same; only the FormIDs are re-resolved for the current load order. That is a deliberate step past
//   "copy the raw blob": removing MFO_Classes.esl renumbers the light plugins after it, so a raw copy would
//   write stale FormIDs that resolve to the WRONG follower or spell next load. One info line per session says
//   the progress was kept and re-written.
//
// ── UNRESOLVABLE FOLLOWER ON LOAD ───────────────────────────────────────────────────────────────────
//   The entry is consumed to its end and DROPPED, counted in the load line: the FLWR precedent
//   (Serialization.cpp ReadFollowersRecord, `if (!resolved) { ++droppedActor; continue; }`) and PGIV's
//   (logistics/PlayerGiven.cpp CoLoad, `#8: unresolvable -> DROP`). Two raw ids resolving to one: the later
//   entry overwrites, logged (the FLWR collision precedent).
//
// ── THREADING ───────────────────────────────────────────────────────────────────────────────────────
//   g_mx guards g_store and the kept blob. Every accessor locks; none calls the engine; the co-save writer
//   snapshots under the lock and writes from the copy (the PGIV / MSTK rule: never a lock across
//   WriteRecordData). ClearAll runs from ResetAllState after Diagnostics::StopPump() and from LoadCallback.
#include "Classes_internal.h"
#include "Config.h"
#include "Followers.h"       // IsPersistableID (#9)
#include "Serialization.h"   // kRecClasses / kClassesVersion, CoSaveBuffer, GambitWire (+ kRowSchema)
#include <cstring>
#include <map>

namespace MFO::Classes {

    namespace {
        constexpr std::uint32_t kMaxSavedFollowers = 4096;          // #11
        constexpr std::uint32_t kMaxRecordBytes    = 256u << 20;    // a sanity bound on the record body (#11);
                                                                    // a real record is a few KB
        constexpr std::size_t   kOffRowSchema = 0, kOffClassCount = 4, kOffFollowerCount = 5;   // header offsets
        constexpr std::uint8_t  kNoClassCombatBit = 1u << 0, kNoClassLogisticsBit = 1u << 1;            // v2 offMask

        using Snapshot = std::vector<std::pair<RE::FormID, FollowerClasses>>;   // ascending FormID

        std::mutex g_mx;
        std::unordered_map<RE::FormID, FollowerClasses> g_store;
        // A record this DLL could not parse, kept verbatim (see VERSION AND COMPAT RULES).
        bool                      g_kept = false;
        std::uint32_t             g_keptVersion = 0;
        std::vector<std::uint8_t> g_keptBytes;

        std::atomic<bool> g_loggedInactiveKeep{ false };   // once per SESSION (never reset by a load)
        // Review F-1: a kept (newer) record makes every setter refuse; the player must SEE that, or XP earned this
        // session would vanish without a word. Set by CoLoad's keep, consumed once at kPostLoadGame (plugin.cpp).
        std::atomic<bool> g_readOnlyNotice{ false };

        constexpr Mask RosterMask(std::size_t a_n) { return a_n >= 64 ? ~Mask{ 0 } : ((Mask{ 1 } << a_n) - 1); }

        struct Header {
            std::uint32_t rowSchema = 0;
            std::uint8_t  classCount = 0;
            std::uint32_t followerCount = 0;
        };
        struct LoadStats {
            std::uint32_t loaded = 0, droppedFollower = 0, collisions = 0, disabledRules = 0,
                          maskedBits = 0, badEquipped = 0, tables = 0, rows = 0,
                          logiTables = 0, offTables = 0, badPickedT4 = 0;   // v2
        };
        enum class ReadResult { Ok, Unparseable, Aborted };

        // ── codec (templated on the stream: SKSE::SerializationInterface or CoSaveBuffer) ─────────────
        // a_rowSchema: production always writes GambitWire::kRowSchema. The self-test alone writes an older row
        // schema (card G9: v5) into a CoSaveBuffer, to prove the reader takes a pre-G9 record.
        // a_version: production always writes kClassesVersion. The self-test alone writes 1 (the entry WITHOUT the
        // v2 block, exactly the v1 writer's bytes: the code above the `a_version >= 2` gate is the v1 writer,
        // untouched) to prove the v1 -> v2 upgrade.
        template <class W>
        bool WriteBody(W& w, const Snapshot& a_snap, std::uint32_t a_rowSchema = GambitWire::kRowSchema,
                       std::uint32_t a_version = kClassesVersion) {
            bool ok = true;
            auto put = [&](const auto& v) { if (!w.WriteRecordData(v)) ok = false; };
            // One stored table of the v2 block: u8 rowCount + the rows (the v1 tables above keep their own loop).
            auto putRows = [&](const std::vector<Gambit>& a_rows) {
                put(static_cast<std::uint8_t>(a_rows.size()));   // <= kMaxStoredRows, enforced by the writers / the reader
                for (const auto& g : a_rows) {
                    bool rowOk = false;
                    if constexpr (std::is_same_v<W, CoSaveBuffer>)
                        rowOk = a_rowSchema == GambitWire::kRowSchema ? GambitWire::WriteRow(w, g)
                                                                      : GambitWire::WriteRowAt(w, g, a_rowSchema);
                    else
                        rowOk = a_rowSchema == GambitWire::kRowSchema && GambitWire::WriteRow(w, g);
                    if (!rowOk) return false;
                }
                return true;
            };
            put(a_rowSchema);   // rowSchema
            put(static_cast<std::uint8_t>(kClassCount));
            put(static_cast<std::uint32_t>(a_snap.size()));
            for (const auto& [fid, fc] : a_snap) {
                put(fid);
                put(fc.equipped);
                for (const auto& p : fc.progress) { put(p.xp); put(p.level); }
                put(fc.mastered);
                put(fc.unlocked);
                put(fc.thiefFights);
                put(fc.masterThiefFights);
                std::uint8_t tableCount = 0;
                for (const auto& t : fc.tables) if (t) ++tableCount;
                put(tableCount);
                for (std::size_t i = 0; i < kClassCount; ++i) {
                    const auto& t = fc.tables[i];
                    if (!t) continue;
                    put(static_cast<std::uint8_t>(i));
                    put(static_cast<std::uint8_t>(t->size()));   // <= kMaxStoredRows, enforced by StoreTable / the reader
                    for (const auto& g : *t) {
                        bool rowOk = false;
                        if constexpr (std::is_same_v<W, CoSaveBuffer>)
                            rowOk = a_rowSchema == GambitWire::kRowSchema ? GambitWire::WriteRow(w, g)
                                                                          : GambitWire::WriteRowAt(w, g, a_rowSchema);
                        else
                            rowOk = a_rowSchema == GambitWire::kRowSchema && GambitWire::WriteRow(w, g);
                        if (!rowOk) return false;
                    }
                }
                if (a_version >= 2) {   // ── the v2 block (card C12) ──
                    put(fc.pickedT4);
                    std::uint8_t logiCount = 0;
                    for (const auto& t : fc.logiTables) if (t) ++logiCount;
                    put(logiCount);
                    for (std::size_t i = 0; i < kClassCount; ++i) {
                        const auto& t = fc.logiTables[i];
                        if (!t) continue;
                        put(static_cast<std::uint8_t>(i));
                        if (!putRows(*t)) return false;
                    }
                    put(static_cast<std::uint8_t>((fc.offCombat ? kNoClassCombatBit : 0u) | (fc.offLogistics ? kNoClassLogisticsBit : 0u)));
                    if (fc.offCombat && !putRows(*fc.offCombat)) return false;
                    if (fc.offLogistics && !putRows(*fc.offLogistics)) return false;
                }
            }
            return ok;
        }

        // Header first: a header this DLL cannot read returns Unparseable having touched nothing in a_out.
        template <class R>
        ReadResult ReadBody(R& r, std::uint32_t a_version, std::unordered_map<RE::FormID, FollowerClasses>& a_out,
                            LoadStats& a_st, Header& a_h) {
            if (r.ReadRecordData(a_h.rowSchema) != sizeof(a_h.rowSchema) ||
                r.ReadRecordData(a_h.classCount) != sizeof(a_h.classCount) ||
                r.ReadRecordData(a_h.followerCount) != sizeof(a_h.followerCount)) {
                spdlog::error("[cosave] MCLS: short read on the record header -- ABORTING class state load");
                return ReadResult::Aborted;
            }
            // Review F-2: a ZERO rowSchema or classCount is corruption (no writer ever produced either), not a newer
            // save: ABORT and keep nothing. Unparseable (kept verbatim) is reserved for values ABOVE this DLL's limits.
            if (a_h.rowSchema == 0 || a_h.classCount == 0) {
                spdlog::error("[cosave] MCLS: corrupt header (rowSchema {}, classCount {}) -- ABORTING class state load, nothing kept",
                              a_h.rowSchema, a_h.classCount);
                return ReadResult::Aborted;
            }
            if (a_h.rowSchema > GambitWire::kRowSchema || a_h.classCount > kClassCount)
                return ReadResult::Unparseable;
            if (a_h.followerCount > kMaxSavedFollowers) {
                spdlog::error("[cosave] MCLS: implausible follower count {} (max {}) -- ABORTING class state load",
                              a_h.followerCount, kMaxSavedFollowers);
                return ReadResult::Aborted;
            }
            const std::size_t n = a_h.classCount;
            const Mask roster = RosterMask(n);
            auto bail = [&](const char* a_why, std::uint32_t a_i) {
                spdlog::error("[cosave] MCLS: {} at follower {}/{} -- ABORTING, keeping {} follower(s) parsed so far",
                              a_why, a_i, a_h.followerCount, a_st.loaded);
                return ReadResult::Aborted;
            };

            for (std::uint32_t i = 0; i < a_h.followerCount; ++i) {
                RE::FormID rawFid = 0;
                if (r.ReadRecordData(rawFid) != sizeof(rawFid)) return bail("short read", i);
                RE::FormID fid = 0;
                const bool resolved = r.ResolveFormID(rawFid, fid);   // #8: unresolvable -> DROP (after consuming)

                FollowerClasses fc{};
                if (r.ReadRecordData(fc.equipped) != sizeof(fc.equipped)) return bail("short read", i);
                if (fc.equipped != kNoClass && fc.equipped >= n) {
                    ++a_st.badEquipped;
                    fc.equipped = kNoClass;   // #11: clamp at ingestion
                }
                for (std::size_t c = 0; c < n; ++c) {
                    auto& p = fc.progress[c];
                    if (r.ReadRecordData(p.xp) != sizeof(p.xp) || r.ReadRecordData(p.level) != sizeof(p.level))
                        return bail("short read", i);
                }
                if (r.ReadRecordData(fc.mastered) != sizeof(fc.mastered) ||
                    r.ReadRecordData(fc.unlocked) != sizeof(fc.unlocked) ||
                    r.ReadRecordData(fc.thiefFights) != sizeof(fc.thiefFights) ||
                    r.ReadRecordData(fc.masterThiefFights) != sizeof(fc.masterThiefFights))
                    return bail("short read", i);
                if ((fc.mastered | fc.unlocked) & ~roster) {
                    ++a_st.maskedBits;
                    fc.mastered &= roster;
                    fc.unlocked &= roster;
                }
                std::uint8_t tableCount = 0;
                if (r.ReadRecordData(tableCount) != sizeof(tableCount)) return bail("short read", i);
                if (tableCount > n) return bail("table count over the class count", i);
                for (std::uint8_t t = 0; t < tableCount; ++t) {
                    std::uint8_t classId = 0, rowCount = 0;
                    if (r.ReadRecordData(classId) != sizeof(classId) || r.ReadRecordData(rowCount) != sizeof(rowCount))
                        return bail("short read", i);
                    if (classId >= n) return bail("stored table for a class beyond the class count", i);
                    if (fc.tables[classId]) return bail("two stored tables for one class", i);
                    if (rowCount > kMaxStoredRows) return bail("stored table over the row bound", i);
                    std::vector<Gambit> rows;
                    rows.reserve(rowCount);
                    for (std::uint8_t k = 0; k < rowCount; ++k) {
                        Gambit g{};
                        bool paramUnresolved = false;
                        if (!GambitWire::ReadRow(r, a_h.rowSchema, g, paramUnresolved)) return bail("short read in a gambit row", i);
                        if (paramUnresolved) ++a_st.disabledRules;
                        rows.push_back(std::move(g));
                    }
                    a_st.rows += rowCount;
                    ++a_st.tables;
                    fc.tables[classId] = std::move(rows);
                }
                // ── the v2 block (card C12): appended per-follower fields. A v1 entry ends above. ──
                if (a_version >= 2) {
                    // One stored table: u8 rowCount + the rows. nullptr = read; otherwise the reason to abort with.
                    auto readRows = [&](std::vector<Gambit>& a_rows) -> const char* {
                        std::uint8_t rowCount = 0;
                        if (r.ReadRecordData(rowCount) != sizeof(rowCount)) return "short read";
                        if (rowCount > kMaxStoredRows) return "stored table over the row bound";
                        a_rows.reserve(rowCount);
                        for (std::uint8_t k = 0; k < rowCount; ++k) {
                            Gambit g{};
                            bool paramUnresolved = false;
                            if (!GambitWire::ReadRow(r, a_h.rowSchema, g, paramUnresolved)) return "short read in a gambit row";
                            if (paramUnresolved) ++a_st.disabledRules;
                            a_rows.push_back(std::move(g));
                        }
                        a_st.rows += rowCount;
                        return nullptr;
                    };
                    std::uint8_t logiCount = 0;
                    if (r.ReadRecordData(fc.pickedT4) != sizeof(fc.pickedT4) || r.ReadRecordData(logiCount) != sizeof(logiCount))
                        return bail("short read", i);
                    if (fc.pickedT4 != kNoClass && (fc.pickedT4 >= n || kTable[fc.pickedT4].tier != 4)) {
                        ++a_st.badPickedT4;
                        fc.pickedT4 = kNoClass;   // #11: clamp at ingestion
                    }
                    if (logiCount > n) return bail("logistics table count over the class count", i);
                    for (std::uint8_t t = 0; t < logiCount; ++t) {
                        std::uint8_t classId = 0;
                        if (r.ReadRecordData(classId) != sizeof(classId)) return bail("short read", i);
                        if (classId >= n) return bail("stored logistics table for a class beyond the class count", i);
                        if (fc.logiTables[classId]) return bail("two stored logistics tables for one class", i);
                        std::vector<Gambit> rows;
                        if (const char* why = readRows(rows)) return bail(why, i);
                        ++a_st.logiTables;
                        fc.logiTables[classId] = std::move(rows);
                    }
                    std::uint8_t offMask = 0;
                    if (r.ReadRecordData(offMask) != sizeof(offMask)) return bail("short read", i);
                    if (offMask & ~(kNoClassCombatBit | kNoClassLogisticsBit)) return bail("unknown classes-OFF table flags", i);
                    if (offMask & kNoClassCombatBit) {
                        std::vector<Gambit> rows;
                        if (const char* why = readRows(rows)) return bail(why, i);
                        ++a_st.offTables;
                        fc.offCombat = std::move(rows);
                    }
                    if (offMask & kNoClassLogisticsBit) {
                        std::vector<Gambit> rows;
                        if (const char* why = readRows(rows)) return bail(why, i);
                        ++a_st.offTables;
                        fc.offLogistics = std::move(rows);
                    }
                }

                if (!resolved) {
                    ++a_st.droppedFollower;
                    continue;
                }
                if (a_out.contains(fid)) {
                    ++a_st.collisions;
                    spdlog::warn("[cosave] MCLS: {:08X} already loaded; a second entry resolved to the same id and OVERWRITES it", fid);
                }
                a_out[fid] = std::move(fc);
                ++a_st.loaded;
            }
            return ReadResult::Ok;
        }

        Snapshot SortedCopy(const std::unordered_map<RE::FormID, FollowerClasses>& a_m) {
            const std::map<RE::FormID, const FollowerClasses*> order = [&] {
                std::map<RE::FormID, const FollowerClasses*> o;
                for (const auto& [fid, fc] : a_m) o.emplace(fid, &fc);
                return o;
            }();
            Snapshot s;
            s.reserve(order.size());
            for (const auto& [fid, fc] : order) s.emplace_back(fid, *fc);
            return s;
        }

        // ── round-trip self-test (bClassCoSaveSelfTest, default OFF) ─────────────────────────────────
        // A row compares on its SERIALIZED fields only, with the writer's transforms (#9: a 0xFF subjectActor is
        // written as 0; card G9: the cooldown is clamped to kMaxCooldownTenths, and a row schema below 6 carries none).
        bool SameRow(const Gambit& a_w, const Gambit& a_r, std::uint32_t a_rowSchema = GambitWire::kRowSchema) {
            const RE::FormID expActor = Followers::IsPersistableID(a_w.subjectActorForm) ? a_w.subjectActorForm : 0;
            const std::uint16_t expCd = a_rowSchema >= 6 ? (std::min)(a_w.cooldownTenths, kMaxCooldownTenths) : 0;
            return a_w.conditionOpcode == a_r.conditionOpcode && a_w.actionOpcode == a_r.actionOpcode &&
                   std::memcmp(&a_w.conditionParam, &a_r.conditionParam, sizeof(float)) == 0 &&
                   a_w.subjectSelector == a_r.subjectSelector && expActor == a_r.subjectActorForm &&
                   a_w.actionParamForm == a_r.actionParamForm && a_w.enabled == a_r.enabled &&
                   expCd == a_r.cooldownTenths;
        }

        // "" = the same table (presence, row count, every row); otherwise what differs.
        std::string TableMismatch(const std::optional<std::vector<Gambit>>& ta, const std::optional<std::vector<Gambit>>& tb,
                                  std::uint32_t a_rowSchema) {
            if (ta.has_value() != tb.has_value()) return "presence";
            if (!ta) return {};
            if (ta->size() != tb->size()) return "row count";
            for (std::size_t k = 0; k < ta->size(); ++k)
                if (!SameRow((*ta)[k], (*tb)[k], a_rowSchema)) return std::format("row {}", k);
            return {};
        }

        // a = what was written, b = what was read back. a_version = the record version it went through: at 1 the v2
        // fields are not on the wire, so b must hold NONE of them whatever a holds.
        std::string FirstMismatch(const FollowerClasses& a, const FollowerClasses& b,
                                  std::uint32_t a_rowSchema = GambitWire::kRowSchema, std::uint32_t a_version = kClassesVersion) {
            if (a_version >= 2) {
                if (a.pickedT4 != b.pickedT4) return "pickedT4";
                for (std::size_t c = 0; c < kClassCount; ++c)
                    if (auto m = TableMismatch(a.logiTables[c], b.logiTables[c], a_rowSchema); !m.empty())
                        return std::format("logistics table[{}] {}", Get(static_cast<Id>(c)).slug, m);
                if (auto m = TableMismatch(a.offCombat, b.offCombat, a_rowSchema); !m.empty()) return "classes-OFF combat table " + m;
                if (auto m = TableMismatch(a.offLogistics, b.offLogistics, a_rowSchema); !m.empty()) return "classes-OFF logistics table " + m;
            } else {
                if (b.pickedT4 != kNoClass || b.offCombat || b.offLogistics) return "a v2 field read from a v1 record";
                for (const auto& t : b.logiTables) if (t) return "a v2 logistics table read from a v1 record";
            }
            if (a.equipped != b.equipped) return "equipped";
            for (std::size_t c = 0; c < kClassCount; ++c)
                if (!(a.progress[c] == b.progress[c])) return std::format("progress[{}]", Get(static_cast<Id>(c)).slug);
            if (a.mastered != b.mastered) return "mastered";
            if (a.unlocked != b.unlocked) return "unlocked";
            if (a.thiefFights != b.thiefFights || a.masterThiefFights != b.masterThiefFights) return "stealth fight counters";
            for (std::size_t c = 0; c < kClassCount; ++c) {
                const auto& ta = a.tables[c];
                const auto& tb = b.tables[c];
                if (ta.has_value() != tb.has_value()) return std::format("table[{}] presence", Get(static_cast<Id>(c)).slug);
                if (!ta) continue;
                if (ta->size() != tb->size()) return std::format("table[{}] row count", Get(static_cast<Id>(c)).slug);
                for (std::size_t k = 0; k < ta->size(); ++k)
                    if (!SameRow((*ta)[k], (*tb)[k], a_rowSchema)) return std::format("table[{}] row {}", Get(static_cast<Id>(c)).slug, k);
            }
            return {};
        }

        // Writes a_snap, reads it back with the identity resolver, compares every field, writes the read-back
        // state again and requires byte-identical output. Pure memory.
        bool RoundTrip(const Snapshot& a_snap, std::string_view a_label, std::vector<std::uint8_t>* a_bytesOut = nullptr) {
            auto fail = [&](const std::string& a_why) {
                spdlog::error("[classes] MCLS self-test ({}): FAIL -- {}", a_label, a_why);
                return false;
            };
            CoSaveBuffer first;
            if (!WriteBody(first, a_snap)) return fail("the writer refused (an opcode over 64 bytes?)");
            CoSaveBuffer rd;
            rd.Bytes() = first.Bytes();
            std::unordered_map<RE::FormID, FollowerClasses> back;
            LoadStats st{};
            Header h{};
            const auto res = ReadBody(rd, kClassesVersion, back, st, h);
            if (res != ReadResult::Ok) return fail(res == ReadResult::Unparseable ? "own output read as unparseable" : "own output aborted");
            if (rd.Remaining()) return fail(std::format("{} trailing byte(s) after the record", rd.Remaining()));
            if (back.size() != a_snap.size()) return fail(std::format("{} follower(s) written, {} read", a_snap.size(), back.size()));
            for (const auto& [fid, fc] : a_snap) {
                const auto it = back.find(fid);
                if (it == back.end()) return fail(std::format("{:08X} missing after read-back", fid));
                if (auto why = FirstMismatch(fc, it->second); !why.empty()) return fail(std::format("{:08X}: {} differs", fid, why));
            }
            CoSaveBuffer second;
            if (!WriteBody(second, SortedCopy(back))) return fail("the re-write refused");
            if (second.Bytes() != first.Bytes()) return fail("re-written bytes differ from the first write");
            spdlog::info("[classes] MCLS self-test ({}): PASS -- {} follower(s), {} combat + {} logistics + {} classes-OFF table(s), "
                         "{} row(s), {} byte(s); re-write byte-identical",
                         a_label, a_snap.size(), st.tables, st.logiTables, st.offTables, st.rows, first.Bytes().size());
            if (a_bytesOut) *a_bytesOut = first.Bytes();
            return true;
        }

        Gambit Row(std::string a_cond, std::string a_act, float a_param, std::uint8_t a_subj, RE::FormID a_subjActor,
                   RE::FormID a_actParam, bool a_enabled, std::uint16_t a_cooldownTenths = 0) {
            Gambit g{};
            g.conditionOpcode = std::move(a_cond);
            g.actionOpcode = std::move(a_act);
            g.conditionParam = a_param;
            g.subjectSelector = a_subj;
            g.subjectActorForm = a_subjActor;
            g.actionParamForm = a_actParam;
            g.enabled = a_enabled;
            g.cooldownTenths = a_cooldownTenths;
            return g;
        }

        // The synthetic state covers every field and every bound: all 35 classes' progress, full and partial
        // masks, both counters at 0 / max, an engaged EMPTY table, a full kMaxStoredRows table, a 64-byte
        // opcode, an appended-subject row, a disabled row, a 0xFF subject actor (written as 0), the last Id, and
        // (card G9) cooldowns at 0 / 10 s / the 600 s bound / over the bound (written clamped), then the same state
        // written at rowSchema 5 (pre-G9) must read back with every cooldown 0.
        // Then the corrupt variants of the bytes must be refused the right way.
        // v2 (card C12): follower A also carries a picked T4, logistics tables (rows, an engaged EMPTY one, a full one, the
        // last Id) and both classes-OFF tables; follower B carries NO v2 state, so his v2 block is the three bytes FF 00 00
        // at the very end of the record (the corrupt variants patch them). The same state written as v1 must read back
        // with every v2 field empty, and re-written (v2) must round-trip byte-identically: the v1 -> v2 upgrade.
        void RunSelfTest() {
            Snapshot s;
            FollowerClasses a{};
            a.equipped = static_cast<std::uint8_t>(Id::Knight);
            for (std::size_t c = 0; c < kClassCount; ++c)
                a.progress[c] = { static_cast<std::uint32_t>(c * 1000u + 7u), static_cast<std::uint8_t>(c % 10 + 1) };
            a.progress[kClassCount - 1].xp = 0xFFFFFFFFu;
            a.mastered = Bit(Id::Melee) | Bit(Id::Knight);
            a.unlocked = RosterMask(kClassCount);
            a.thiefFights = 0;
            a.masterThiefFights = 0xFFFF;
            a.tables[static_cast<std::size_t>(Id::Melee)] = std::vector<Gambit>{
                Row("cond.self_hp_below", "act.drink_health", 0.5f, 0, 0, 0x00039BE5, true),
                Row("cond.enemy_any", "act.cast_spell", 0.0f, 3 /*Enemy, appended*/, 0xFF000801 /*runtime -> 0*/, 0x00012FCD, true),
                Row("cond.always", "act.wait", 0.0f, 0, 0, 0, false),
                // card G9: a 10 s power-attack cooldown, the 600 s bound, and an over-bound value (written clamped)
                Row("cond.foe_blocking", "act.power_attack", 0.0f, 0, 0, 0, true, 100),
                Row("cond.foe_stronger_than_me", "act.power_attack", 0.0f, 0, 0, 0, true, kMaxCooldownTenths),
                Row("cond.foe_blocking", "act.power_attack", 0.0f, 0, 0, 0, false, 0xFFFF),
            };
            a.tables[static_cast<std::size_t>(Id::Mage)] = std::vector<Gambit>{};   // engaged and empty
            a.tables[static_cast<std::size_t>(Id::Dragonborn)] = std::vector<Gambit>{
                Row(std::string(GambitWire::kMaxOpcodeLen, 'c'), "", -1.25f, 4, 0xFE00A123, 0xFE00A456, true),
            };
            std::vector<Gambit> full;
            for (std::size_t k = 0; k < kMaxStoredRows; ++k)
                full.push_back(Row("cond.ally_hp_below", "act.cast_spell", static_cast<float>(k) / 64.0f,
                                   static_cast<std::uint8_t>(k % 3), 0x00000014 + static_cast<RE::FormID>(k), 0, (k & 1) == 0,
                                   static_cast<std::uint16_t>((k * 97u) % (kMaxCooldownTenths + 1u))));
            a.tables[static_cast<std::size_t>(Id::Warrior)] = std::move(full);
            // ── v2 fields ──
            a.pickedT4 = static_cast<std::uint8_t>(Id::Listener);
            a.logiTables[static_cast<std::size_t>(Id::Knight)] = std::vector<Gambit>{
                Row("cond.self_hp_below", "act.drink_health", 0.5f, 0, 0, 0, true),
                Row("cond.self_out_of_arrows", "act.loot_arrows", 10.0f, 0, 0, 0, true),
                Row("cond.always", "act.loot_equipment", 0.0f, 0, 0, 0, false),
            };
            a.logiTables[static_cast<std::size_t>(Id::Melee)] = std::vector<Gambit>{};   // engaged and empty
            a.logiTables[static_cast<std::size_t>(Id::Dragonborn)] = std::vector<Gambit>{
                Row("cond.always", std::string(GambitWire::kMaxOpcodeLen, 'a'), 2.0f, 1, 0, 0xFE00A789, true, 5),
            };
            std::vector<Gambit> fullLogi;
            for (std::size_t k = 0; k < kMaxStoredRows; ++k)
                fullLogi.push_back(Row("cond.self_low_health_potion", "act.loot_potions", static_cast<float>(k), 0, 0,
                                       0x00000F00 + static_cast<RE::FormID>(k), (k % 3) != 0));
            a.logiTables[static_cast<std::size_t>(Id::Archer)] = std::move(fullLogi);
            a.offCombat = std::vector<Gambit>{
                Row("cond.self_hp_below", "act.drink_health", 0.3f, 0, 0, 0, true),
                Row("cond.foe_lowest_hp", "act.attack", 0.0f, 0, 0, 0, true),
                Row("cond.always", "act.wait", 0.0f, 0, 0, 0, true),
            };
            a.offLogistics = std::vector<Gambit>{};   // engaged and empty
            s.emplace_back(0x00013BBB, std::move(a));

            FollowerClasses b{};   // a follower with nothing but one counter and one partial mask
            b.mastered = Bit(Id::Mage);
            b.thiefFights = 4;
            s.emplace_back(0xFE012345, std::move(b));

            std::vector<std::uint8_t> bytes;
            if (!RoundTrip(s, "synthetic", &bytes)) return;

            // Card G9 (FLWR v6): the SAME state written as a pre-G9 DLL wrote it (rowSchema 5: no cooldown field)
            // must read back Ok with every row's cooldown 0, consume exactly its bytes, and be exactly 2 bytes per
            // row shorter than the v6 write. Re-written by this DLL it is a valid v6 record (the RoundTrip above
            // proves v6 -> v6; this proves v5 -> parse).
            {
                std::size_t rowCount = 0;
                for (const auto& [fid, fc] : s) {
                    for (const auto& t : fc.tables)
                        if (t) rowCount += t->size();
                    for (const auto& t : fc.logiTables)   // v2: the row schema covers every stored table
                        if (t) rowCount += t->size();
                    if (fc.offCombat) rowCount += fc.offCombat->size();
                    if (fc.offLogistics) rowCount += fc.offLogistics->size();
                }
                CoSaveBuffer v5;
                if (!WriteBody(v5, s, 5)) {
                    spdlog::error("[classes] MCLS self-test (rows v5): FAIL -- the v5 row writer refused");
                    return;
                }
                const std::size_t expect5 = bytes.size() - 2 * rowCount;
                CoSaveBuffer rd;
                rd.Bytes() = v5.Bytes();
                std::unordered_map<RE::FormID, FollowerClasses> back;
                LoadStats st{};
                Header h{};
                const auto res = ReadBody(rd, kClassesVersion, back, st, h);
                std::string why;
                if (v5.Bytes().size() != expect5)
                    why = std::format("{} bytes, expected {} (v6 {} - 2 x {} rows)", v5.Bytes().size(), expect5, bytes.size(), rowCount);
                else if (res != ReadResult::Ok || h.rowSchema != 5)
                    why = std::format("read result {} at rowSchema {}", static_cast<int>(res), h.rowSchema);
                else if (rd.Remaining())
                    why = std::format("{} trailing byte(s)", rd.Remaining());
                else if (back.size() != s.size())
                    why = std::format("{} follower(s) written, {} read", s.size(), back.size());
                else
                    for (const auto& [fid, fc] : s) {
                        const auto it = back.find(fid);
                        if (it == back.end()) { why = std::format("{:08X} missing", fid); break; }
                        if (auto m = FirstMismatch(fc, it->second, 5); !m.empty()) { why = std::format("{:08X}: {} differs", fid, m); break; }
                    }
                if (!why.empty()) {
                    spdlog::error("[classes] MCLS self-test (rows v5): FAIL -- {}", why);
                    return;
                }
                spdlog::info("[classes] MCLS self-test (rows v5, pre-G9): PASS -- {} row(s) read with cooldown 0, {} byte(s) = v6 - 2 x rows",
                             rowCount, expect5);
            }

            // Card C12 (MCLS v2): the SAME state as a v1 DLL wrote it (no v2 block) must read back Ok at version 1 with
            // every v1 field intact and every v2 field EMPTY, consume exactly its bytes, and be shorter than the v2 write
            // by exactly the v2 blocks. Read at version 2 the v1 bytes must ABORT (a v1 entry has no v2 block: the
            // version, not the blob size, decides). The read-back state then round-trips as v2: the v1 -> v2 upgrade.
            {
                auto fail = [](const std::string& a_why) { spdlog::error("[classes] MCLS self-test (v1 -> v2): FAIL -- {}", a_why); };
                CoSaveBuffer v1;
                if (!WriteBody(v1, s, GambitWire::kRowSchema, 1)) return fail("the v1 writer refused");
                // The v2 blocks, measured from an independent encode of each follower with and without his v2 fields.
                std::size_t v2Bytes = 0;
                for (const auto& [fid, fc] : s) {
                    FollowerClasses bare = fc;
                    bare.pickedT4 = kNoClass;
                    for (auto& t : bare.logiTables) t.reset();
                    bare.offCombat.reset();
                    bare.offLogistics.reset();
                    CoSaveBuffer with, without;
                    if (!WriteBody(with, Snapshot{ { fid, fc } }) || !WriteBody(without, Snapshot{ { fid, bare } })) return fail("an encode refused");
                    v2Bytes += 3 + (with.Bytes().size() - without.Bytes().size());   // 3 = pickedT4 + logiTableCount + offMask
                }
                if (v1.Bytes().size() + v2Bytes != bytes.size())
                    return fail(std::format("v1 {} byte(s) + v2 blocks {} != v2 {}", v1.Bytes().size(), v2Bytes, bytes.size()));
                CoSaveBuffer rd;
                rd.Bytes() = v1.Bytes();
                std::unordered_map<RE::FormID, FollowerClasses> back;
                LoadStats st{};
                Header h{};
                if (ReadBody(rd, 1, back, st, h) != ReadResult::Ok) return fail("the v1 bytes did not read Ok at version 1");
                if (rd.Remaining()) return fail(std::format("{} trailing byte(s) after the v1 body", rd.Remaining()));
                if (back.size() != s.size()) return fail(std::format("{} follower(s) written, {} read", s.size(), back.size()));
                for (const auto& [fid, fc] : s) {
                    const auto it = back.find(fid);
                    if (it == back.end()) return fail(std::format("{:08X} missing", fid));
                    if (auto m = FirstMismatch(fc, it->second, GambitWire::kRowSchema, 1); !m.empty())
                        return fail(std::format("{:08X}: {} differs", fid, m));
                }
                {
                    spdlog::info("[classes] MCLS self-test (v1 -> v2): reading the v1 bytes as v2 -- the ABORTING line that follows is EXPECTED");
                    CoSaveBuffer asV2;
                    asV2.Bytes() = v1.Bytes();
                    std::unordered_map<RE::FormID, FollowerClasses> out;
                    LoadStats st2{};
                    Header h2{};
                    if (ReadBody(asV2, 2, out, st2, h2) != ReadResult::Aborted) return fail("v1 bytes read at version 2 were not aborted");
                }
                spdlog::info("[classes] MCLS self-test (v1 -> v2): PASS -- {} v1 byte(s) read with no v2 state; the v2 write is {} byte(s) "
                             "longer; re-writing what was read:", v1.Bytes().size(), v2Bytes);
                if (!RoundTrip(SortedCopy(back), "v1 state re-written as v2")) return;
            }

            // Corrupt variants: each must be refused, never fabricated (#11).
            constexpr int kVariants = 11;
            spdlog::info("[classes] MCLS self-test: planting {} corrupt variants -- the reader's ABORTING lines that follow are EXPECTED", kVariants);
            int bad = 0;
            auto expect = [&](std::string_view a_name, std::vector<std::uint8_t> a_b, ReadResult a_want) {
                CoSaveBuffer rd;
                rd.Bytes() = std::move(a_b);
                std::unordered_map<RE::FormID, FollowerClasses> out;
                LoadStats st{};
                Header h{};
                const auto got = ReadBody(rd, kClassesVersion, out, st, h);
                if (got != a_want) {
                    ++bad;
                    spdlog::error("[classes] MCLS self-test (corrupt: {}): FAIL -- not refused as expected", a_name);
                }
            };
            auto patch32 = [](std::vector<std::uint8_t> a_b, std::size_t a_off, std::uint32_t a_v) {
                std::memcpy(a_b.data() + a_off, &a_v, sizeof(a_v));
                return a_b;
            };
            {
                auto t = bytes;
                t.pop_back();
                expect("truncated by one byte", std::move(t), ReadResult::Aborted);
            }
            expect("follower count over the bound", patch32(bytes, kOffFollowerCount, kMaxSavedFollowers + 1), ReadResult::Aborted);
            expect("newer row schema", patch32(bytes, kOffRowSchema, GambitWire::kRowSchema + 1), ReadResult::Unparseable);
            expect("row schema 0 (corrupt, not newer)", patch32(bytes, kOffRowSchema, 0), ReadResult::Aborted);
            {
                auto t = bytes;
                t[kOffClassCount] = 0;
                expect("class count 0 (corrupt, not newer)", std::move(t), ReadResult::Aborted);
            }
            {
                auto t = bytes;
                t[kOffClassCount] = static_cast<std::uint8_t>(kClassCount + 1);
                expect("more classes than this DLL knows", std::move(t), ReadResult::Unparseable);
            }
            {
                // follower 0's tableCount byte: header 9 + fid 4 + equipped 1 + progress 35*5 + masks 16 + counters 4
                auto t = bytes;
                const std::size_t off = 9 + 4 + 1 + kClassCount * 5 + 16 + 4;
                t[off] = static_cast<std::uint8_t>(kClassCount + 1);
                expect("table count over the class count", std::move(t), ReadResult::Aborted);
            }
            // v2: the LAST follower (B) has no v2 state, so the record ends with his block: pickedT4 FF, logiTableCount 00,
            // offMask 00.
            if (bytes.size() < 3 || bytes[bytes.size() - 3] != kNoClass || bytes[bytes.size() - 2] != 0 || bytes[bytes.size() - 1] != 0) {
                ++bad;
                spdlog::error("[classes] MCLS self-test (corrupt variants): FAIL -- the record does not end with an empty v2 block");
            } else {
                {
                    auto t = bytes;
                    t[t.size() - 1] = 0x04;
                    expect("unknown classes-OFF table flag", std::move(t), ReadResult::Aborted);
                }
                {
                    auto t = bytes;
                    t[t.size() - 1] = kNoClassCombatBit;   // says a table follows; the record ends
                    expect("classes-OFF table flagged but missing", std::move(t), ReadResult::Aborted);
                }
                {
                    auto t = bytes;
                    t[t.size() - 2] = static_cast<std::uint8_t>(kClassCount + 1);
                    expect("logistics table count over the class count", std::move(t), ReadResult::Aborted);
                }
                {
                    // A picked T4 that is not a tier-4 class is CLAMPED to none (counted), never an abort.
                    auto t = bytes;
                    t[t.size() - 3] = static_cast<std::uint8_t>(Id::Knight);
                    CoSaveBuffer rd;
                    rd.Bytes() = std::move(t);
                    std::unordered_map<RE::FormID, FollowerClasses> out;
                    LoadStats st{};
                    Header h{};
                    const auto got = ReadBody(rd, kClassesVersion, out, st, h);
                    const auto it = out.find(0xFE012345);
                    if (got != ReadResult::Ok || st.badPickedT4 != 1 || it == out.end() || it->second.pickedT4 != kNoClass) {
                        ++bad;
                        spdlog::error("[classes] MCLS self-test (corrupt: picked T4 that is not tier 4): FAIL -- not cleared to none");
                    }
                }
            }
            if (bad == 0)
                spdlog::info("[classes] MCLS self-test (corrupt variants): PASS -- {} of {} refused", kVariants, kVariants);
        }

        // A write is allowed only on a live tree with a parsed (not kept) record. Caller holds g_mx.
        bool WritableLocked() { return Active() && !g_kept; }

        template <class F>
        bool Mutate(RE::FormID a_fid, F&& a_f) {
            if (a_fid == 0 || !Followers::IsPersistableID(a_fid)) return false;   // #9
            {
                std::scoped_lock lk(g_mx);
                if (!WritableLocked()) return false;
                a_f(g_store[a_fid]);
            }
            // Card C6: the carriers follow the state (equip / level / mastery). Outside g_mx: RefreshCarriers only queues work
            // for the main thread (its own mutex), never an engine call here.
            RefreshCarriers(a_fid);
            return true;
        }

        bool ValidId(Id a_id) { return static_cast<std::size_t>(a_id) < kClassCount; }

        // A table the record can hold: within the row bound, and no opcode the row writer would refuse (refuse at
        // the door instead of at save).
        bool Storable(const std::vector<Gambit>& a_rows) {
            if (a_rows.size() > kMaxStoredRows) return false;
            for (const auto& g : a_rows)
                if (g.conditionOpcode.size() > GambitWire::kMaxOpcodeLen || g.actionOpcode.size() > GambitWire::kMaxOpcodeLen)
                    return false;
            return true;
        }

        // Review F-7: drop the RUNTIME-ONLY fields (State.h:29 uid, and the display-only lastFired / lastFailReason /
        // lastFiredAt) so a restored table re-mints its uids (Board.cpp assigns one to every uid == 0 rule) and shows
        // no stale outcome. The serialized fields are untouched.
        void Scrub(std::vector<Gambit>& a_rows) {
            for (auto& g : a_rows) {
                g.uid = 0;
                g.lastFired = false;
                g.lastFailReason.clear();
                g.lastFiredAt = {};
            }
        }

        std::optional<std::vector<Gambit>>& SlotOf(FollowerClasses& a_fc, Id a_class, TableKind a_kind) {
            return (a_kind == TableKind::Logistics ? a_fc.logiTables : a_fc.tables)[static_cast<std::size_t>(a_class)];
        }
    }

    // ── co-save ──────────────────────────────────────────────────────────────────────────────────────
    void CoSave(SKSE::SerializationInterface* a_intfc) {
        Snapshot snap;
        bool kept = false;
        std::uint32_t keptVersion = 0;
        std::vector<std::uint8_t> keptBytes;
        std::uint32_t skippedRuntime = 0;
        {
            std::scoped_lock lk(g_mx);
            if (g_kept) {
                kept = true;
                keptVersion = g_keptVersion;
                keptBytes = g_keptBytes;
            } else {
                std::unordered_map<RE::FormID, FollowerClasses> copy;
                for (const auto& [fid, fc] : g_store) {
                    if (!Followers::IsPersistableID(fid)) { ++skippedRuntime; continue; }   // #9 (Mutate refuses them; belt and braces)
                    copy.emplace(fid, fc);
                }
                snap = SortedCopy(copy);
                if (snap.size() > kMaxSavedFollowers) {
                    // Review F-9: the bound drops the HIGHEST FormIDs. Say so -- silent drops are how "the mod ate my
                    // save" starts (the FLWR truncation warning's rule).
                    spdlog::warn("[cosave] MCLS: {} follower(s) with class state truncated to {} -- the entries above {:08X} are "
                                 "NOT saved", snap.size(), kMaxSavedFollowers, snap[kMaxSavedFollowers - 1].first);
                    snap.resize(kMaxSavedFollowers);
                }
            }
        }

        if (kept) {
            if (!a_intfc->OpenRecord(kRecClasses, keptVersion)) {
                spdlog::error("[cosave] OpenRecord('MCLS') failed -- the kept class record is NOT saved");
                return;
            }
            if (!keptBytes.empty() && !a_intfc->WriteRecordData(keptBytes.data(), static_cast<std::uint32_t>(keptBytes.size())))
                spdlog::error("[cosave] MCLS: writing the kept record failed -- this save's class record is TRUNCATED");
            spdlog::info("[cosave] wrote back the kept class record (MCLS v{}, {} byte(s)) unchanged", keptVersion, keptBytes.size());
            return;
        }
        if (snap.empty()) return;   // no class state: no record (the ERND precedent)

        // Encode into memory first, so a refusal never leaves a half-written record (#11).
        CoSaveBuffer buf;
        if (!WriteBody(buf, snap)) {
            spdlog::error("[cosave] MCLS: encoding refused (an opcode over {} bytes in a stored table) -- the class record is NOT "
                          "saved this time; the state is still held, re-save after fixing the rule", GambitWire::kMaxOpcodeLen);
            return;
        }
        if (buf.Bytes().size() > kMaxRecordBytes)
            spdlog::error("[cosave] MCLS: {} byte(s) is over the {} byte load bound -- a load will REFUSE this record",
                          buf.Bytes().size(), kMaxRecordBytes);
        if (!a_intfc->OpenRecord(kRecClasses, kClassesVersion)) {
            spdlog::error("[cosave] OpenRecord('MCLS') failed -- class state NOT saved");
            return;
        }
        if (!a_intfc->WriteRecordData(buf.Bytes().data(), static_cast<std::uint32_t>(buf.Bytes().size())))
            spdlog::error("[cosave] MCLS: WriteRecordData failed -- this save's class record is TRUNCATED");

        std::size_t tables = 0, logiTables = 0, offTables = 0;
        for (const auto& [fid, fc] : snap) {
            for (const auto& t : fc.tables) if (t) ++tables;
            for (const auto& t : fc.logiTables) if (t) ++logiTables;
            offTables += (fc.offCombat ? 1u : 0u) + (fc.offLogistics ? 1u : 0u);
        }
        spdlog::info("[cosave] saved class state for {} follower(s) ({} combat + {} logistics + {} classes-OFF stored table(s), "
                     "{} byte(s)), schema v{} rows v{}{}",
                     snap.size(), tables, logiTables, offTables, buf.Bytes().size(), kClassesVersion, GambitWire::kRowSchema,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) id(s)", skippedRuntime) : std::string{});
        if (!Active() && !g_loggedInactiveKeep.exchange(true))
            spdlog::info("[classes] class add-on inactive ({}) -- class progress for {} follower(s) KEPT and written back unchanged "
                         "(marth Q9); nothing changes it until the add-on is active again",
                         !Present() ? "MFO_Classes.esl not loaded" : !HarbingerOk() ? "Harbinger missing or too old" : "bClassTree = 0", snap.size());
    }

    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length) {
        if (a_version < 1) {   // the ERND / MFO-B309 precedent: no writer ever produced v0
            spdlog::error("[cosave] MCLS record with version {} (no such schema) -- skipped, nothing kept", a_version);
            return;
        }
        if (a_length > kMaxRecordBytes) {
            spdlog::error("[cosave] MCLS record of {} byte(s) is over the {} byte bound -- REFUSED and not kept (#11)",
                          a_length, kMaxRecordBytes);
            return;
        }
        // Read the body whole: the bytes are then still in hand if the header says this DLL cannot parse it.
        CoSaveBuffer buf(a_intfc);   // reads from memory, resolves FormIDs through SKSE
        buf.Bytes().resize(a_length);
        if (a_length && a_intfc->ReadRecordData(buf.Bytes().data(), a_length) != a_length) {
            spdlog::error("[cosave] MCLS: short read of the {} byte record -- ABORTING class state load", a_length);
            return;
        }

        auto keep = [&](const std::string& a_why) {
            std::scoped_lock lk(g_mx);
            if (g_kept || !g_store.empty())
                spdlog::warn("[cosave] MCLS: a SECOND class record in one save -- the earlier one is replaced");
            g_store.clear();
            g_kept = true;
            g_keptVersion = a_version;
            g_keptBytes = std::move(buf.Bytes());
            g_readOnlyNotice.store(true);
            spdlog::error("[cosave] CLASS SAVE IS NEWER THAN THIS DLL ({}) -- the record ({} byte(s)) is KEPT VERBATIM and written "
                          "back unchanged on every save; class progress is FROZEN this session. Update MFO to use it.",
                          a_why, g_keptBytes.size());
        };
        if (a_version > kClassesVersion) {
            keep(std::format("MCLS v{}, this DLL reads v{}", a_version, kClassesVersion));
            return;
        }

        std::unordered_map<RE::FormID, FollowerClasses> parsed;
        LoadStats st{};
        Header h{};
        const auto res = ReadBody(buf, a_version, parsed, st, h);
        if (res == ReadResult::Unparseable) {
            buf.Rewind();
            keep(std::format("rows v{} / {} classes, this DLL reads rows v1..v{} / {} classes", h.rowSchema, h.classCount,
                             GambitWire::kRowSchema, kClassCount));
            return;
        }
        if (res == ReadResult::Ok && buf.Remaining())
            spdlog::warn("[cosave] MCLS: {} unread byte(s) after the v{} body -- ignored (#11)", buf.Remaining(), a_version);

        // C5 repair (NOT a layout change): a record saved with a class at its level cap but the mastered bit clear (a save that
        // raced an older writer) is healed here, only while the tree is Active() so an inactive add-on still writes back unchanged
        // (marth Q9). The unlocked set is refreshed from the repaired mastered set (additive).
        if (Active()) {
            int healed = 0;
            for (auto& [fid, fc] : parsed) {
                Mask add = 0;
                for (std::size_t c = 0; c < kClassCount; ++c) {
                    const std::uint8_t cap = LevelCap(static_cast<Id>(c));
                    if (cap != 0 && fc.progress[c].level >= cap && !(fc.mastered & (Mask{ 1 } << c))) add |= Mask{ 1 } << c;
                }
                if (!add) continue;
                fc.mastered |= add;
                fc.unlocked |= ComputeUnlocked(fc.mastered) & RosterMask(kClassCount);
                ++healed;
            }
            if (healed) spdlog::warn("[classes] repaired {} follower record(s): a class at its level cap had no mastered bit", healed);
        }

        const auto loadedCount = parsed.size();
        Snapshot forTest;
        if (Config::g_classCoSaveSelfTest.load()) forTest = SortedCopy(parsed);
        {
            std::scoped_lock lk(g_mx);
            if (g_kept || !g_store.empty())
                spdlog::warn("[cosave] MCLS: a SECOND class record in one save -- its entries are merged over the first");
            g_kept = false;
            g_keptBytes.clear();
            for (auto& [fid, fc] : parsed) g_store[fid] = std::move(fc);
        }
        spdlog::info("[cosave] loaded class state for {} follower(s){} (MCLS v{}, rows v{}, {} classes): {} combat + {} logistics + "
                     "{} classes-OFF stored table(s), {} row(s); "
                     "dropped {} unresolvable follower(s); disabled {} rule(s) with missing targets; {} id collision(s); "
                     "{} bad equipped id(s) cleared; {} mask(s) with bits beyond the roster trimmed; {} bad picked T4 cleared{}{}",
                     loadedCount, res == ReadResult::Aborted ? " (RECORD ABORTED, see above)" : "", a_version, h.rowSchema,
                     h.classCount, st.tables, st.logiTables, st.offTables, st.rows, st.droppedFollower, st.disabledRules,
                     st.collisions, st.badEquipped, st.maskedBits, st.badPickedT4,
                     a_version < kClassesVersion ? " -- an older record: the next save writes it as v2" : "",
                     Active() ? "" : " -- add-on inactive: held unchanged (marth Q9)");
        if (!forTest.empty()) RoundTrip(forTest, "loaded state");
        RefreshAllCarriers();   // card C6: re-apply the carriers to every stored follower once the world is up (queued, main thread)
    }

    void BeginLoad() {
        ClearAll();
        g_readOnlyNotice.store(false);   // only THIS load's record may raise it
        if (Config::g_classCoSaveSelfTest.load()) RunSelfTest();
    }

    void ClearAll() {
        {
            std::scoped_lock lk(g_mx);
            g_store.clear();
            g_kept = false;
            g_keptVersion = 0;
            g_keptBytes.clear();
            g_keptBytes.shrink_to_fit();
        }
        ResetCarriers();   // card C6: the pending set, the granted-perk memory and the pump generation are save-scoped too
        detail::XpReset();   // card C4: the fight-pending credits are save-scoped too
    }

    std::vector<RE::FormID> detail::StoredFollowerIds() {
        std::scoped_lock lk(g_mx);
        std::vector<RE::FormID> out;
        out.reserve(g_store.size());
        for (const auto& [fid, fc] : g_store) out.push_back(fid);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool ConsumeReadOnlyNotice() { return g_readOnlyNotice.exchange(false); }

    bool Writable() {
        std::scoped_lock lk(g_mx);
        return WritableLocked();
    }

    // ── reads ────────────────────────────────────────────────────────────────────────────────────────
    std::optional<FollowerClasses> GetFollower(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        if (it == g_store.end()) return std::nullopt;
        return it->second;
    }

    std::uint8_t GetEquipped(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it == g_store.end() ? kNoClass : it->second.equipped;
    }

    ClassProgress GetProgress(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return {};
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it == g_store.end() ? ClassProgress{} : it->second.progress[static_cast<std::size_t>(a_class)];
    }

    Mask GetMastered(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it == g_store.end() ? 0 : it->second.mastered;
    }

    Mask GetUnlocked(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it == g_store.end() ? 0 : it->second.unlocked;
    }

    std::pair<std::uint16_t, std::uint16_t> GetStealthFights(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        if (it == g_store.end()) return { 0, 0 };
        return { it->second.thiefFights, it->second.masterThiefFights };
    }

    bool HasStoredTable(RE::FormID a_follower, Id a_class, TableKind a_kind) {
        if (!ValidId(a_class)) return false;
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it != g_store.end() && SlotOf(it->second, a_class, a_kind).has_value();
    }

    std::optional<std::vector<Gambit>> GetStoredTable(RE::FormID a_follower, Id a_class, TableKind a_kind) {
        if (!ValidId(a_class)) return std::nullopt;
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        if (it == g_store.end()) return std::nullopt;
        return SlotOf(it->second, a_class, a_kind);
    }

    std::uint8_t GetPickedT4(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it == g_store.end() ? kNoClass : it->second.pickedT4;
    }

    // ── writes (all refuse unless Writable()) ────────────────────────────────────────────────────────
    bool SetEquipped(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return false;
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.equipped = static_cast<std::uint8_t>(a_class); });
    }

    bool ClearEquipped(RE::FormID a_follower) {
        return Mutate(a_follower, [](FollowerClasses& fc) { fc.equipped = kNoClass; });
    }

    bool SetProgress(RE::FormID a_follower, Id a_class, ClassProgress a_progress) {
        if (!ValidId(a_class)) return false;
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.progress[static_cast<std::size_t>(a_class)] = a_progress; });
    }

    bool SetMastered(RE::FormID a_follower, Mask a_mastered) {
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.mastered = a_mastered & RosterMask(kClassCount); });
    }

    bool SetUnlocked(RE::FormID a_follower, Mask a_unlocked) {
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.unlocked = a_unlocked & RosterMask(kClassCount); });
    }

    bool SetStealthFights(RE::FormID a_follower, std::uint16_t a_thief, std::uint16_t a_masterThief) {
        return Mutate(a_follower, [&](FollowerClasses& fc) {
            fc.thiefFights = a_thief;
            fc.masterThiefFights = a_masterThief;
        });
    }

    bool StoreTable(RE::FormID a_follower, Id a_class, std::vector<Gambit> a_rows, TableKind a_kind) {
        if (!ValidId(a_class) || !Storable(a_rows)) return false;
        Scrub(a_rows);
        return Mutate(a_follower, [&](FollowerClasses& fc) { SlotOf(fc, a_class, a_kind) = std::move(a_rows); });
    }

    bool ClearStoredTable(RE::FormID a_follower, Id a_class, TableKind a_kind) {
        if (!ValidId(a_class)) return false;
        return Mutate(a_follower, [&](FollowerClasses& fc) { SlotOf(fc, a_class, a_kind).reset(); });
    }

    bool SetPickedT4(RE::FormID a_follower, Id a_t4) {
        if (!ValidId(a_t4) || Get(a_t4).tier != 4) return false;
        bool done = false;
        Mutate(a_follower, [&](FollowerClasses& fc) {
            if (!(fc.mastered & Bit(a_t4))) return;   // marth Q60: any MASTERED T4
            fc.pickedT4 = static_cast<std::uint8_t>(a_t4);
            done = true;
        });
        return done;
    }

    bool ClearPickedT4(RE::FormID a_follower) {
        return Mutate(a_follower, [](FollowerClasses& fc) { fc.pickedT4 = kNoClass; });
    }

    // Card C12, the ONE compound class switch (Classes_internal.h): under a single g_mx hold it stores the live tables
    // under what he is leaving (his class, or the classes-OFF slot), equips a_to under ActivateClass's rules (or takes
    // the class off for kNoClass), and hands back what was stored for where he is going, CONSUMING it (the equipped
    // class's tables are FLWR's live ones, never a second copy here). A co-save snapshot therefore never sees the
    // equip without the store. Refused (done == false, NOTHING changed): not Writable(), a 0xFF id, a_to out of range,
    // locked, already there, or a live table the record cannot hold.
    detail::Swap detail::SwapClass(RE::FormID a_follower, std::uint8_t a_to, std::vector<Gambit> a_liveCombat,
                                   std::vector<Gambit> a_liveLogistics) {
        Swap out;
        if (a_to != kNoClass && a_to >= kClassCount) return out;
        if (!Storable(a_liveCombat) || !Storable(a_liveLogistics)) {
            spdlog::error("[classes] {:08X}: class switch REFUSED -- a live table ({} combat / {} logistics row(s)) is over the {} row "
                          "store bound or holds an opcode over {} bytes; nothing changed",
                          a_follower, a_liveCombat.size(), a_liveLogistics.size(), kMaxStoredRows, GambitWire::kMaxOpcodeLen);
            return out;
        }
        Scrub(a_liveCombat);
        Scrub(a_liveLogistics);
        Mutate(a_follower, [&](FollowerClasses& fc) {
            if (fc.equipped == a_to) return;   // already there
            if (a_to != kNoClass && !(((fc.unlocked | ComputeUnlocked(fc.mastered)) >> static_cast<unsigned>(a_to)) & 1u)) return;   // locked
            out.from = fc.equipped;
            if (fc.equipped == kNoClass) {
                fc.offCombat = std::move(a_liveCombat);
                fc.offLogistics = std::move(a_liveLogistics);
            } else if (fc.equipped < kClassCount) {   // always (clamped at load, validated by every setter)
                fc.tables[fc.equipped] = std::move(a_liveCombat);
                fc.logiTables[fc.equipped] = std::move(a_liveLogistics);
            }
            auto take = [](std::optional<std::vector<Gambit>>& a_slot) {
                std::optional<std::vector<Gambit>> t = std::move(a_slot);
                a_slot.reset();
                return t;
            };
            if (a_to == kNoClass) {
                out.combat = take(fc.offCombat);
                out.logistics = take(fc.offLogistics);
            } else {
                auto& p = fc.progress[a_to];
                if (LevelCap(static_cast<Id>(a_to)) != 0 && p.level == 0) p.level = 1;   // ActivateClass's rule: first use starts at level 1
                out.combat = take(fc.tables[a_to]);
                out.logistics = take(fc.logiTables[a_to]);
            }
            fc.equipped = a_to;
            out.done = true;
        });
        return out;
    }

    // Card C5 compound writers: the whole read-decide-write under ONE g_mx hold, so a CoSave snapshot (main thread) can never
    // see a level at its cap with mastered clear, and two writers cannot lose each other's update. LevelCap / ComputeUnlocked /
    // UnlockSatisfied are pure (Mastery.cpp, Classes.cpp); nothing here touches the engine.
    bool ApplyLevel(RE::FormID a_follower, Id a_class, std::uint8_t a_level) {
        if (!ValidId(a_class)) return false;
        const std::uint8_t cap = LevelCap(a_class);
        if (cap == 0) return false;   // Dragonborn: no level
        bool changed = false, newlyMastered = false;
        std::uint8_t finalLevel = 0;
        const bool ran = Mutate(a_follower, [&](FollowerClasses& fc) {
            if (!(((fc.unlocked | ComputeUnlocked(fc.mastered)) >> static_cast<unsigned>(a_class)) & 1u)) return;   // locked
            auto& p = fc.progress[static_cast<std::size_t>(a_class)];
            const std::uint8_t lvl = std::max<std::uint8_t>(p.level, std::min(a_level, cap));   // a level is never taken away
            if (lvl != p.level) { p.level = lvl; changed = true; }
            finalLevel = lvl;
            if (lvl >= cap && !(fc.mastered & Bit(a_class))) { fc.mastered |= Bit(a_class); changed = newlyMastered = true; }
            const Mask unlocked = (fc.unlocked | ComputeUnlocked(fc.mastered)) & RosterMask(kClassCount);   // never revoked
            if (unlocked != fc.unlocked) { fc.unlocked = unlocked; changed = true; }
        });
        if (ran && newlyMastered)
            spdlog::info("[classes] {:08X} mastered {} (level {})", a_follower, Get(a_class).slug, static_cast<int>(finalLevel));
        return ran && changed;
    }

    // Card C4 compound XP writer: the add, the level, the mastery and the unlocks under ONE g_mx hold (the ApplyLevel shape), and
    // only while a_class is the EQUIPPED class. No layout change: ClassProgress{xp, level} is what MCLS already stores.
    XpResult AwardXp(RE::FormID a_follower, Id a_class, std::uint32_t a_points, std::span<const std::uint32_t> a_cumulative) {
        XpResult out;
        if (!ValidId(a_class) || a_points == 0) return out;
        const std::uint8_t cap = LevelCap(a_class);
        if (cap == 0 || a_cumulative.size() != cap) return out;   // Dragonborn, or a curve that could never reach mastery
        Mutate(a_follower, [&](FollowerClasses& fc) {
            if (fc.equipped != static_cast<std::uint8_t>(a_class)) return;
            auto& p = fc.progress[static_cast<std::size_t>(a_class)];
            if (p.level >= cap) return;   // mastered: nothing more to earn on this class
            out.levelBefore = p.level;
            const std::uint64_t sum = static_cast<std::uint64_t>(p.xp) + a_points;
            p.xp = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, a_cumulative.back()));   // the total at the cap is the ceiling
            const std::uint8_t lvl = std::max<std::uint8_t>(std::max<std::uint8_t>(p.level, 1), LevelForXp(p.xp, a_cumulative));
            p.level = std::min(lvl, cap);
            if (p.level >= cap && !(fc.mastered & Bit(a_class))) { fc.mastered |= Bit(a_class); out.mastered = true; }
            const Mask unlocked = (fc.unlocked | ComputeUnlocked(fc.mastered)) & RosterMask(kClassCount);   // never revoked
            if (unlocked != fc.unlocked) fc.unlocked = unlocked;
            out.xp = p.xp;
            out.levelAfter = p.level;
            out.applied = true;
        });
        if (out.applied && out.mastered)
            spdlog::info("[classes] {:08X} mastered {} (level {})", a_follower, Get(a_class).slug, static_cast<int>(out.levelAfter));
        return out;
    }

    bool ActivateClass(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return false;
        bool done = false;
        Mutate(a_follower, [&](FollowerClasses& fc) {
            if (!(((fc.unlocked | ComputeUnlocked(fc.mastered)) >> static_cast<unsigned>(a_class)) & 1u)) return;   // locked
            if (fc.equipped == static_cast<std::uint8_t>(a_class)) return;                                          // already it
            auto& p = fc.progress[static_cast<std::size_t>(a_class)];
            if (LevelCap(a_class) != 0 && p.level == 0) p.level = 1;   // a class used for the first time starts at level 1
            fc.equipped = static_cast<std::uint8_t>(a_class);
            done = true;
        });
        return done;
    }
}
