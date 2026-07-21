#include "PCH.h"
#include "State.h"
#include "Serialization.h"

// P0: the co-save. Schema in ARCHITECTURE.md §7; rules in INVARIANTS.md §B.
//
// The five rules this file exists to obey:
//   #8  Every persisted FormID passes ResolveFormID. Unresolvable -> DROP.
//   #9  Never persist a runtime-created (0xFF) FormID.
//   #10 Persist stable string opcodes, never enum ordinals.
//   #11 Bound every count, bail on short reads, clamp at ingestion.
//   #12 Versioned schema; readers kept FOREVER; SKSE does NOT round-trip
//       unread records, so a downgraded DLL DESTROYS newer ones -> warn loud.

namespace MFO {

    namespace {
        // Bounds. A truncated or hostile record must stop the parse, never
        // fabricate (INVARIANTS.md #11).
        constexpr std::uint32_t kMaxFollowers = 4096;
        constexpr std::uint32_t kMaxOpcodeLen = 64;
        constexpr std::uint16_t kMaxTutored   = 512;
        constexpr std::uint16_t kMaxOverrides = 64;

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

        const auto count = static_cast<std::uint32_t>(g_followers.size());
        a_intfc->WriteRecordData(count);

        std::uint32_t written = 0;
        for (const auto& [formID, st] : g_followers) {
            a_intfc->WriteRecordData(formID);
            a_intfc->WriteRecordData(st.rapport);
            a_intfc->WriteRecordData(st.rank);

            const auto gambitCount = static_cast<std::uint8_t>(st.gambits.size());
            a_intfc->WriteRecordData(gambitCount);
            for (const auto& g : st.gambits) {
                if (!WriteString(a_intfc, g.conditionOpcode)) return;
                a_intfc->WriteRecordData(g.conditionParam);
                a_intfc->WriteRecordData(g.subjectSelector);
                if (!WriteString(a_intfc, g.actionOpcode)) return;
                a_intfc->WriteRecordData(g.actionParamForm);
                const std::uint8_t flags = g.enabled ? 1u : 0u;
                a_intfc->WriteRecordData(flags);
            }

            const auto tutoredCount = static_cast<std::uint16_t>(st.tutored.size());
            a_intfc->WriteRecordData(tutoredCount);
            for (const auto& t : st.tutored) {
                a_intfc->WriteRecordData(t.spell);
                a_intfc->WriteRecordData(t.grantedAtVersion);
            }

            const auto overrideCount = static_cast<std::uint16_t>(st.overrides.size());
            a_intfc->WriteRecordData(overrideCount);
            for (const auto& o : st.overrides) {
                a_intfc->WriteRecordData(o.package);
                a_intfc->WriteRecordData(o.priority);
            }
            ++written;
        }

        // Log the zero case too (INVARIANTS.md #46): "saved nothing" and
        // "never ran" must not look identical.
        spdlog::info("[cosave] saved {} follower record(s), schema v{}", written, kSchemaVersion);
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        g_followers.clear();

        std::uint32_t type = 0, version = 0, length = 0;
        std::uint32_t loaded = 0, droppedActor = 0, droppedSpell = 0, disabledRules = 0;

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
                // TODO(P0): also surface a kPostLoadGame message box, per MAO §13.
                return;
            }

            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(count)) {
                spdlog::error("[cosave] short read on follower count -- ABORTING");
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

                std::uint8_t gambitCount = 0;
                if (!a_intfc->ReadRecordData(gambitCount)) return;
                // Clamp to the rank's slot maximum, not to a global constant.
                const std::uint8_t slotMax = SlotsForRank(st.rank);
                if (gambitCount > slotMax) {
                    spdlog::warn("[cosave] follower {:08X}: {} gambits exceeds rank {} max {} -- extra rules DROPPED",
                                 resolvedID, gambitCount, st.rank, slotMax);
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
                            // DESIGN.md §3.3: disable THIS rule, with a marker.
                            // Never guess, and never drop the whole list.
                            g.actionParamForm = 0;
                            g.enabled = false;
                            g.lastFailReason = "action target missing from load order";
                            ++disabledRules;
                        }
                    }
                    if (gi < slotMax) st.gambits.push_back(std::move(g));
                }

                std::uint16_t tutoredCount = 0;
                if (!a_intfc->ReadRecordData(tutoredCount)) return;
                if (tutoredCount > kMaxTutored) {
                    spdlog::error("[cosave] implausible tutored count {} -- ABORTING", tutoredCount);
                    return;
                }
                for (std::uint16_t ti = 0; ti < tutoredCount; ++ti) {
                    TutoredSpell t{};
                    RE::FormID rawSpell = 0;
                    if (!a_intfc->ReadRecordData(rawSpell)) return;
                    if (!a_intfc->ReadRecordData(t.grantedAtVersion)) return;
                    RE::FormID resolvedSpell = 0;
                    if (a_intfc->ResolveFormID(rawSpell, resolvedSpell)) {
                        t.spell = resolvedSpell;
                        st.tutored.push_back(t);
                    } else {
                        // DESIGN.md §5.4: drop with a log line rather than
                        // attempting a revoke against a dangling id.
                        ++droppedSpell;
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
                    }
                }

                if (!resolved) {
                    ++droppedActor;
                    continue;   // the actor is gone; its whole record goes
                }
                g_followers[resolvedID] = std::move(st);
                ++loaded;
            }
        }

        // INVARIANTS.md #47: split the skip counters by REASON. A single
        // aggregate hides a systematic failure inside ordinary attrition.
        spdlog::info("[cosave] loaded {} follower(s); dropped {} unresolvable actor(s), "
                     "{} unresolvable tutored spell(s); disabled {} rule(s) with missing targets",
                     loaded, droppedActor, droppedSpell, disabledRules);
    }

    void RevertCallback(SKSE::SerializationInterface*) {
        ResetAllState();
        spdlog::info("[cosave] revert -- all save-scoped state cleared");
    }

    void ResetAllState() {
        g_followers.clear();
    }

}
