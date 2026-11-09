// 'MFST' -- THE SELF-TEST CO-SAVE RECORD (dev builds only; Serialization.h kRecSelfTest / kSelfTestVersion).
//
// Written ONLY by a dev build (this file is not compiled into a release DLL). A release build that loads a
// dev save meets 'MFST' as an unknown record type: LoadCallback's `type != kRecFollowers` branch logs
// "unknown record type ... skipped" and GetNextRecordInfo seeks past it; that DLL's next save drops it
// (#12: SKSE does not round-trip unread records). Nothing else reads it.
//
// LAYOUT v1 (all little-endian, via SerializationInterface):
//   u32 resetGeneration                      the MFO_SelfTest.ini iResetGeneration last applied
//   u32 resultCount (<= kMaxResults)
//     per result: str id, str verdict ("pass" | "fail"), u32 follower (raw FormID, 0 = none), lstr evidence
//   u32 fixtureCount (<= kMaxFixtures)
//     per fixture: u32 follower, str kind ("item" | "spell"), u32 form, i32 added, i32 before, str testId
//   str  = u32 len (<= kMaxStr) + bytes;  lstr = u32 len (<= kMaxEvidence) + bytes
// Strings, never ordinals (#10). Every count bounded and a short read aborts THIS record only (#11). Every
// FormID passes ResolveFormID; an unresolvable fixture is DROPPED with an error line (it cannot be cleaned:
// its follower or its form left the load order, #8 / #13). A result whose follower no longer resolves keeps
// follower 0 (the verdict stands). A result id this build has no test for is KEPT, so a later dev build
// that knows it again still sees it complete.

#include "selftest/SelfTest_internal.h"
#include "Serialization.h"
#include "Followers.h"

namespace MFO::SelfTest {

    namespace {
        constexpr std::uint32_t kMaxResults  = 256;
        constexpr std::uint32_t kMaxFixtures = 1024;
        constexpr std::uint32_t kMaxStr      = 64;
        constexpr std::uint32_t kMaxEvidence = 1024;

        bool WriteStr(SKSE::SerializationInterface* a_i, std::string_view a_s, std::uint32_t a_max) {
            const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(a_s.size(), a_max));
            return a_i->WriteRecordData(len) && (len == 0 || a_i->WriteRecordData(a_s.data(), len));
        }
        bool ReadStr(SKSE::SerializationInterface* a_i, std::string& a_out, std::uint32_t a_max) {
            std::uint32_t len = 0;
            if (a_i->ReadRecordData(len) != sizeof(len)) return false;
            if (len > a_max) {
                spdlog::error("[selftest] MFST string length {} exceeds {} -- record aborted", len, a_max);
                return false;
            }
            a_out.resize(len);
            return len == 0 || a_i->ReadRecordData(a_out.data(), len) == len;
        }
        template <class T>
        bool Rd(SKSE::SerializationInterface* a_i, T& a_v) { return a_i->ReadRecordData(a_v) == sizeof(T); }
    }

    void CoSave(SKSE::SerializationInterface* a_intfc) {
        // Copy under the leaf lock, write from the copy (no SKSE call under g_mx).
        std::uint32_t                gen;
        std::map<std::string, Result> results;
        std::vector<Fixture>         ledger;
        {
            std::lock_guard lk(g_mx);
            gen     = g_resetGeneration;
            results = g_results;
            ledger  = g_ledger;
        }
        if (results.size() > kMaxResults) results.erase(std::next(results.begin(), kMaxResults), results.end());
        // #9: never a runtime (0xFF) follower or form. The runner refuses to run on a runtime follower and
        // never adds a runtime form, so this only guards the contract.
        std::erase_if(ledger, [](const Fixture& f) {
            return !Followers::IsPersistableID(f.follower) || (f.form >> 24) == 0xFF;
        });
        if (ledger.size() > kMaxFixtures) ledger.resize(kMaxFixtures);

        if (!a_intfc->OpenRecord(kRecSelfTest, kSelfTestVersion)) {
            spdlog::error("[selftest] OpenRecord('MFST') failed -- self-test results and fixture ledger NOT saved");
            return;
        }
        bool ok = a_intfc->WriteRecordData(gen) &&
                  a_intfc->WriteRecordData(static_cast<std::uint32_t>(results.size()));
        for (const auto& [id, r] : results) {
            if (!ok) break;
            const RE::FormID fol = Followers::IsPersistableID(r.follower) ? r.follower : 0;
            ok = WriteStr(a_intfc, id, kMaxStr) && WriteStr(a_intfc, r.pass ? "pass" : "fail", kMaxStr) &&
                 a_intfc->WriteRecordData(fol) && WriteStr(a_intfc, r.evidence, kMaxEvidence);
        }
        ok = ok && a_intfc->WriteRecordData(static_cast<std::uint32_t>(ledger.size()));
        for (const auto& f : ledger) {
            if (!ok) break;
            ok = a_intfc->WriteRecordData(f.follower) &&
                 WriteStr(a_intfc, f.kind == FixtureKind::Item ? "item" : "spell", kMaxStr) &&
                 a_intfc->WriteRecordData(f.form) && a_intfc->WriteRecordData(f.added) &&
                 a_intfc->WriteRecordData(f.before) && WriteStr(a_intfc, f.testId, kMaxStr);
        }
        if (!ok)
            spdlog::error("[selftest] MFST write FAILED mid-record -- the next load reads what precedes the "
                          "failure and aborts the rest of this record");
        else
            spdlog::info("[selftest] saved {} result(s), {} fixture ledger entr(ies), reset generation {}, MFST v{}",
                         results.size(), ledger.size(), gen, kSelfTestVersion);
    }

    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        if (a_version < 1) {
            spdlog::error("[selftest] MFST record with version {} (no such schema) -- skipped", a_version);
            return;
        }
        std::uint32_t                 gen = 0, nRes = 0, nFix = 0;
        std::map<std::string, Result> results;
        std::vector<Fixture>          ledger;
        std::uint32_t                 droppedFix = 0, droppedRes = 0;

        auto commit = [&](const char* a_state) {
            std::lock_guard lk(g_mx);
            g_resetGeneration = gen;
            g_results         = std::move(results);
            g_ledger          = std::move(ledger);
            spdlog::info("[selftest] loaded MFST v{} ({}): {} result(s), {} fixture ledger entr(ies) to remove, "
                         "reset generation {}; dropped {} result(s), {} fixture(s)",
                         a_version, a_state, g_results.size(), g_ledger.size(), g_resetGeneration, droppedRes,
                         droppedFix);
        };

        if (!Rd(a_intfc, gen) || !Rd(a_intfc, nRes)) { commit("empty: short header"); return; }
        if (nRes > kMaxResults) {
            spdlog::error("[selftest] MFST result count {} > {} -- record aborted", nRes, kMaxResults);
            commit("aborted at the result count");
            return;
        }
        for (std::uint32_t i = 0; i < nRes; ++i) {
            std::string id, verdict, ev;
            RE::FormID  raw = 0;
            if (!ReadStr(a_intfc, id, kMaxStr) || !ReadStr(a_intfc, verdict, kMaxStr) || !Rd(a_intfc, raw) ||
                !ReadStr(a_intfc, ev, kMaxEvidence)) {
                commit("aborted mid-results (short read)");
                return;
            }
            if (verdict != "pass" && verdict != "fail") {   // #13: an unknown verdict is dropped, never guessed
                spdlog::error("[selftest] MFST result '{}' has unknown verdict '{}' -- dropped (stays pending)", id,
                              verdict);
                ++droppedRes;
                continue;
            }
            RE::FormID fol = 0;
            if (raw && !a_intfc->ResolveFormID(raw, fol)) fol = 0;
            results[id] = { verdict == "pass", fol, std::move(ev) };
        }
        if (!Rd(a_intfc, nFix)) { commit("no fixture block (short read)"); return; }
        if (nFix > kMaxFixtures) {
            spdlog::error("[selftest] MFST fixture count {} > {} -- fixture block aborted", nFix, kMaxFixtures);
            commit("aborted at the fixture count");
            return;
        }
        for (std::uint32_t i = 0; i < nFix; ++i) {
            RE::FormID   rawFol = 0, rawForm = 0;
            std::string  kind, testId;
            std::int32_t added = 0, before = 0;
            if (!Rd(a_intfc, rawFol) || !ReadStr(a_intfc, kind, kMaxStr) || !Rd(a_intfc, rawForm) ||
                !Rd(a_intfc, added) || !Rd(a_intfc, before) || !ReadStr(a_intfc, testId, kMaxStr)) {
                commit("aborted mid-fixtures (short read)");
                return;
            }
            RE::FormID fol = 0, form = 0;
            const bool kindOk = kind == "item" || kind == "spell";
            if (!kindOk || !a_intfc->ResolveFormID(rawFol, fol) || !a_intfc->ResolveFormID(rawForm, form) ||
                added <= 0 || added > 100000 || before < 0) {
                spdlog::error("[selftest] MFST fixture follower {:08X} kind '{}' form {:08X} x{} (test '{}') cannot "
                              "be mapped -- DROPPED, it will NOT be removed (its follower or form left the load "
                              "order, or the entry is corrupt)",
                              rawFol, kind, rawForm, added, testId);
                ++droppedFix;
                continue;
            }
            ledger.push_back({ 0u, std::move(testId), fol, kind == "item" ? FixtureKind::Item : FixtureKind::Spell,
                               form, added, before });
        }
        commit("complete");
    }

}
