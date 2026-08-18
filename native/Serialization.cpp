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
#include "CombatStyle.h"
#include "Actuation.h"   // #76: revert drops the equip force-hold records
#include "Sightline.h"
#include "Packages.h"
#include "Papyrus.h"
#include "TradeBridge.h"
#include "MEOBridge.h"
#include "MainThread.h"
#include "Probe.h"
#include "Board.h"
#include "ProgAllocator.h"
#include <unordered_set>   // #69: stock-gear per-follower sets (kRecStock)

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
        constexpr std::uint32_t kMaxStockGear = 512;   // #69: generous headroom over any real loadout

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
        // SEV-1 #3: quiesce the AddTask worker for the whole callback so the
        // count pass (below) and the write pass see the SAME g_followers. Without
        // this, the worker's Refresh/TryEnsureRecord insert (map rehash) or
        // Scheduler::Tick rule-write between the two passes tears the FLWR record
        // or invalidates the iterator (co-save corruption). RAII so every early
        // return still resumes; no epoch bump, so gameplay resumes untouched.
        Diagnostics::PausePump();
        struct ResumeGuard { ~ResumeGuard() { Diagnostics::ResumePump(); } } _resume;

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
            // v4 (#65): the per-follower combat class override, right after rank.
            a_intfc->WriteRecordData(st.combatClassOverride);

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

        // #69: stock gear (kRecStock/'MSTK') -- a SECOND, INDEPENDENT record.
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

        // Progression (kRecProgression/'PRGN') -- a THIRD independent record,
        // same isolation contract as MSTK above. Written even when the addon
        // ESL is absent this session: the data must survive a session where
        // the user temporarily disabled MFO_Progression.esl, not be silently
        // destroyed by the next save. Layout + bounds live with the state
        // owner (ProgAllocator.cpp).
        ProgAllocator::CoSaveSave(a_intfc);

        // #76 force-hold (kRecForcedWeapon/'FWPN') -- a FOURTH independent record,
        // same isolation contract. Persists the force-equip locks so a load can
        // clear stale ones (Actuation owns the layout + bounds).
        Actuation::CoSaveForcedWeapons(a_intfc);
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        g_followers.clear();
        Logistics::ClearStockGear();   // #69: defence in depth -- RevertCallback already
                                        // cleared it, same belt-and-braces as g_followers above

        std::uint32_t type = 0, version = 0, length = 0;
        std::uint32_t loaded = 0, droppedActor = 0, disabledRules = 0,
                      droppedOverride = 0, collisions = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == kRecStock) {
                // #69: independent of FLWR -- its own version guard, its own
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
                // #76: same independence contract. A newer record skips (never
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
        // #69: g_stockGear IS serialized (kRecStock), unlike everything
        // ClearTransientState owns -- kept as its own call so that contract
        // stays true. A load right after this repopulates it; a main-menu
        // revert with no load leaves it empty, same as g_followers above.
        Logistics::ClearStockGear();
        // Progression state IS serialized too (kRecProgression) -- same
        // contract as stock gear. ClearAll also orphans the level-poll chain
        // (generation bump) so a stale tick never runs against the next save.
        ProgAllocator::ClearAll();
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
        Actuation::ClearForcedWeapons();    // #76: equip force-hold records are
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
        Probe::ReleaseAll();                // audit: watch handles outlived a revert (were
                                            // cleared only at kPostLoadGame, not here)
        Rapport::ResetSessionCounters();
        // (StopPump moved to the TOP of this function -- it must precede the clears.)
        Board::SetHud(false);      // else it lingers over the main menu with stale rows
    }

}
