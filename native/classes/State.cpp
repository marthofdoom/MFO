// classes/State.cpp -- per-follower class state and its co-save record 'MCLS' (class update card C3).
//
// WHAT IT HOLDS: per follower, the equipped class, XP + level for every class, the mastered / unlocked
// bitsets, the Thief / Master Thief stealth-entry fight counters (card C13), and the COMBAT gambit table
// saved under each class he switched away from (marth Q8, card C12). Nothing here accrues XP, decides
// mastery, applies a gambit or touches the engine: those are cards C4 / C5 / C6 / C12. This file only
// holds, guards and persists the state, and exposes the accessors (Classes.h) those cards call.
//
// ── 'MCLS' v1 BYTE LAYOUT (little-endian, SKSE record body) ─────────────────────────────────────────
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
//   Followers are written in ascending FormID order and tables in Id order, so the bytes are deterministic.
//
// ── VERSION AND COMPAT RULES ────────────────────────────────────────────────────────────────────────
//   * Extension is BY VERSION, never by blob size: a v2 APPENDS fields at the end of the per-follower entry,
//     read under `if (a_version >= 2)`; the v1 reader stays forever (#12). A new class APPENDS an Id (Classes.h
//     rule): the writer then writes the larger classCount, and an older DLL meets classCount > its kClassCount.
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
                          maskedBits = 0, badEquipped = 0, tables = 0, rows = 0;
        };
        enum class ReadResult { Ok, Unparseable, Aborted };

        // ── codec (templated on the stream: SKSE::SerializationInterface or CoSaveBuffer) ─────────────
        // a_rowSchema: production always writes GambitWire::kRowSchema. The self-test alone writes an older row
        // schema (card G9: v5) into a CoSaveBuffer, to prove the reader takes a pre-G9 record.
        template <class W>
        bool WriteBody(W& w, const Snapshot& a_snap, std::uint32_t a_rowSchema = GambitWire::kRowSchema) {
            bool ok = true;
            auto put = [&](const auto& v) { if (!w.WriteRecordData(v)) ok = false; };
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
                // v2+ appends its per-follower fields HERE, under `if (a_version >= 2)`.
                (void)a_version;

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

        std::string FirstMismatch(const FollowerClasses& a, const FollowerClasses& b,
                                  std::uint32_t a_rowSchema = GambitWire::kRowSchema) {
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
            spdlog::info("[classes] MCLS self-test ({}): PASS -- {} follower(s), {} table(s), {} row(s), {} byte(s); re-write byte-identical",
                         a_label, a_snap.size(), st.tables, st.rows, first.Bytes().size());
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
        // Then five corrupt variants of the bytes must be refused the right way.
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
                for (const auto& [fid, fc] : s)
                    for (const auto& t : fc.tables)
                        if (t) rowCount += t->size();
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

            // Corrupt variants: each must be refused, never fabricated (#11).
            constexpr int kVariants = 7;
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
            if (bad == 0)
                spdlog::info("[classes] MCLS self-test (corrupt variants): PASS -- {} of {} refused", kVariants, kVariants);
        }

        // A write is allowed only on a live tree with a parsed (not kept) record. Caller holds g_mx.
        bool WritableLocked() { return Active() && !g_kept; }

        template <class F>
        bool Mutate(RE::FormID a_fid, F&& a_f) {
            if (a_fid == 0 || !Followers::IsPersistableID(a_fid)) return false;   // #9
            std::scoped_lock lk(g_mx);
            if (!WritableLocked()) return false;
            a_f(g_store[a_fid]);
            return true;
        }

        bool ValidId(Id a_id) { return static_cast<std::size_t>(a_id) < kClassCount; }
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

        std::size_t tables = 0;
        for (const auto& [fid, fc] : snap)
            for (const auto& t : fc.tables) if (t) ++tables;
        spdlog::info("[cosave] saved class state for {} follower(s) ({} stored table(s), {} byte(s)), schema v{} rows v{}{}",
                     snap.size(), tables, buf.Bytes().size(), kClassesVersion, GambitWire::kRowSchema,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) id(s)", skippedRuntime) : std::string{});
        if (!Active() && !g_loggedInactiveKeep.exchange(true))
            spdlog::info("[classes] class add-on inactive ({}) -- class progress for {} follower(s) KEPT and written back unchanged "
                         "(marth Q9); nothing changes it until the add-on is active again",
                         Present() ? "bClassTree = 0" : "MFO_Classes.esl not loaded", snap.size());
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
        spdlog::info("[cosave] loaded class state for {} follower(s){} (MCLS v{}, rows v{}, {} classes): {} stored table(s), {} row(s); "
                     "dropped {} unresolvable follower(s); disabled {} rule(s) with missing targets; {} id collision(s); "
                     "{} bad equipped id(s) cleared; {} mask(s) with bits beyond the roster trimmed{}",
                     loadedCount, res == ReadResult::Aborted ? " (RECORD ABORTED, see above)" : "", a_version, h.rowSchema,
                     h.classCount, st.tables, st.rows, st.droppedFollower, st.disabledRules, st.collisions, st.badEquipped,
                     st.maskedBits, Active() ? "" : " -- add-on inactive: held unchanged (marth Q9)");
        if (!forTest.empty()) RoundTrip(forTest, "loaded state");
    }

    void BeginLoad() {
        ClearAll();
        g_readOnlyNotice.store(false);   // only THIS load's record may raise it
        if (Config::g_classCoSaveSelfTest.load()) RunSelfTest();
    }

    void ClearAll() {
        std::scoped_lock lk(g_mx);
        g_store.clear();
        g_kept = false;
        g_keptVersion = 0;
        g_keptBytes.clear();
        g_keptBytes.shrink_to_fit();
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

    bool HasStoredTable(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return false;
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        return it != g_store.end() && it->second.tables[static_cast<std::size_t>(a_class)].has_value();
    }

    std::optional<std::vector<Gambit>> GetStoredTable(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return std::nullopt;
        std::scoped_lock lk(g_mx);
        const auto it = g_store.find(a_follower);
        if (it == g_store.end()) return std::nullopt;
        return it->second.tables[static_cast<std::size_t>(a_class)];
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

    bool StoreTable(RE::FormID a_follower, Id a_class, std::vector<Gambit> a_rows) {
        if (!ValidId(a_class) || a_rows.size() > kMaxStoredRows) return false;
        for (const auto& g : a_rows)   // the row writer refuses these; refuse at the door instead of at save
            if (g.conditionOpcode.size() > GambitWire::kMaxOpcodeLen || g.actionOpcode.size() > GambitWire::kMaxOpcodeLen)
                return false;
        // Review F-7: drop the RUNTIME-ONLY fields (State.h:29 uid, and the display-only lastFired / lastFailReason /
        // lastFiredAt) so a restored table re-mints its uids (Board.cpp assigns one to every uid == 0 rule) and shows
        // no stale outcome. The serialized fields are untouched.
        for (auto& g : a_rows) {
            g.uid = 0;
            g.lastFired = false;
            g.lastFailReason.clear();
            g.lastFiredAt = {};
        }
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.tables[static_cast<std::size_t>(a_class)] = std::move(a_rows); });
    }

    bool ClearStoredTable(RE::FormID a_follower, Id a_class) {
        if (!ValidId(a_class)) return false;
        return Mutate(a_follower, [&](FollowerClasses& fc) { fc.tables[static_cast<std::size_t>(a_class)].reset(); });
    }
}
