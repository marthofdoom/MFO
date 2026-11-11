// tools/mcls_byte_test -- emulated save round trip of the 'MCLS' class co-save record (class update card C12, MCLS v2).
// RUN: tools/mcls_byte_test/run.sh   (host g++ or CXX=clang++; exit code = number of failed checks).
// Not part of the CMake/CI build: the DLL never links it.
//
// WHAT IS REAL: native/classes/State.cpp (the record's writer, reader, the store and every accessor), Classes.cpp,
// ClassTable.cpp, Mastery.cpp, and the gambit-row codec copied text for text out of native/Serialization.cpp
// (gen_rowcodec.py). WHAT IS A STAND-IN: the engine headers (stub/), the class add-on's presence gate and the passive
// carriers (below; neither is part of the record), and Followers::IsPersistableID (the real one-liner).
//
// THE OLDER DLL IS REAL TOO: run.sh builds this same file a second time, with -DMCLS_OLD_DLL, against the tree of the
// last commit that wrote MCLS v1 (f21f6c7, `git archive`). That binary
//   --emit-v1          writes a v1 record with the v1 writer (it must equal the committed golden_v1.hex), and
//   --downgrade <hex>  loads this DLL's v2 record as the v1 DLL does: kept verbatim, frozen, written back unchanged.
//
// What the current build checks:
//   1. the in-DLL self-test (bClassCoSaveSelfTest: synthetic v2 round trip, rows v5, v1 -> v2, corrupt variants);
//   2. golden_v1.hex (written by the v1 DLL) loads with every field equal to the same state built through the
//      writers, with no v2 state, and saves as v2 = each v1 follower entry + FF 00 00, byte for byte (the entries are
//      found by an INDEPENDENT walk of the documented layout, not by the codec);
//   3. a state with every v2 field (through SwapClass / SetPickedT4, the only roads) saves, loads and saves again
//      byte-identically, and the independent walk consumes the record exactly;
//   4. the class switch: store under what he leaves, hand back and consume what was stored, the classes-OFF slot,
//      and every refusal changes nothing;
//   5. inactive add-on (marth Q9), FormID re-resolution, an unresolvable follower, a newer record kept verbatim;
//   6. every truncation and every single-byte corruption of the v2 record: never a crash, a truncation never loads
//      clean.
#include "classes/Classes_internal.h"
#include "Config.h"
#include "Followers.h"
#include "Serialization.h"
#include "Vocabulary.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "rowcodec.inc"   // GENERATED: the real row codec (gen_rowcodec.py)

// ── stand-ins for what is not part of the record ──────────────────────────────────────────────────────────────────
namespace MFO::Followers {
    bool IsPersistableID(RE::FormID a_actorID) { return (a_actorID >> 24) != 0xFF; }   // native/Followers.cpp, verbatim
}
namespace MFO::Classes {
    static bool g_testActive = true;
    bool Present() { return true; }
    bool Active() { return g_testActive; }
    bool Strict() { return false; }
    void RefreshCarriers(RE::FormID) {}
    void RefreshAllCarriers() {}
    void ResetCarriers() {}
}

using namespace MFO;
using namespace MFO::Classes;
using Bytes = std::vector<std::uint8_t>;

namespace {
    int g_failed = 0, g_checks = 0;
    bool Check(bool a_ok, const std::string& a_what) {
        ++g_checks;
        if (!a_ok) { ++g_failed; std::printf("  [FAIL] %s\n", a_what.c_str()); }
        return a_ok;
    }
    [[maybe_unused]] std::size_t CountLines(char a_level, std::string_view a_needle = {}) {
        std::size_t n = 0;
        for (const auto& l : spdlog::Lines())
            if (l.level == a_level && (a_needle.empty() || l.text.find(a_needle) != std::string::npos)) ++n;
        return n;
    }
    bool HasLine(std::string_view a_needle) {
        for (const auto& l : spdlog::Lines())
            if (l.text.find(a_needle) != std::string::npos) return true;
        return false;
    }
    [[maybe_unused]] void FlushLog(const char* a_section) {
        if (std::getenv("MCLS_TEST_VERBOSE")) {
            std::printf("--- log: %s\n", a_section);
            for (const auto& l : spdlog::Lines()) std::printf("    %c %s\n", l.level, l.text.c_str());
        }
        spdlog::Lines().clear();
    }

    std::string ToHex(const Bytes& a_b) {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (std::size_t i = 0; i < a_b.size(); ++i) {
            s += d[a_b[i] >> 4];
            s += d[a_b[i] & 15];
            s += (i % 32 == 31) ? '\n' : ' ';
        }
        s += '\n';
        return s;
    }
    bool FromHexFile(const char* a_path, Bytes& a_out) {
        std::ifstream f(a_path);
        if (!f) return false;
        std::string tok;
        while (f >> tok) {
            if (tok.size() != 2) return false;
            a_out.push_back(static_cast<std::uint8_t>(std::strtoul(tok.c_str(), nullptr, 16)));
        }
        return true;
    }

    Gambit Row(std::string a_cond, std::string a_act, float a_param, std::uint8_t a_subj, RE::FormID a_subjActor,
               RE::FormID a_actParam, bool a_enabled, std::uint16_t a_cd = 0) {
        Gambit g{};
        g.uid = 77;   // runtime-only: must never reach the record
        g.conditionOpcode = std::move(a_cond);
        g.actionOpcode = std::move(a_act);
        g.conditionParam = a_param;
        g.subjectSelector = a_subj;
        g.subjectActorForm = a_subjActor;
        g.actionParamForm = a_actParam;
        g.enabled = a_enabled;
        g.cooldownTenths = a_cd;
        return g;
    }
    std::vector<Gambit> Rows(std::size_t a_n, RE::FormID a_base) {
        std::vector<Gambit> v;
        for (std::size_t k = 0; k < a_n; ++k)
            v.push_back(Row("cond.foe_any", "act.cast_target", static_cast<float>(k) * 0.25f, static_cast<std::uint8_t>(k % 3), 0,
                            a_base + static_cast<RE::FormID>(k), (k & 1) == 0, static_cast<std::uint16_t>(k * 10)));
        return v;
    }

    constexpr RE::FormID kA = 0x00013BBB, kB = 0xFE012345, kC = 0x0A000D62;

    // A v1 state, built ONLY through writers the v1 DLL has, so both builds lay down the same thing.
    void BuildV1State() {
        ClearAll();
        Check(SetMastered(kA, Bit(Id::Melee) | Bit(Id::Knight)), "v1 state: SetMastered");
        Check(SetUnlocked(kA, ComputeUnlocked(Bit(Id::Melee) | Bit(Id::Knight)) | Bit(Id::Dragonborn)), "v1 state: SetUnlocked");
        for (std::size_t c = 0; c < kClassCount; ++c)
            SetProgress(kA, static_cast<Id>(c), { static_cast<std::uint32_t>(c * 1000u + 7u), static_cast<std::uint8_t>(c % 10) });
        SetProgress(kA, Id::Dragonborn, { 0xFFFFFFFFu, 0 });
        Check(SetEquipped(kA, Id::Knight), "v1 state: SetEquipped");
        SetStealthFights(kA, 3, 0xFFFF);
        StoreTable(kA, Id::Melee, { Row("cond.self_hp_pct_below", "act.drink_health_potion", 0.5f, 0, 0, 0x00039BE5, true),
                                    Row("cond.foe_any", "act.cast_target", 0.0f, 3, 0xFF000801, 0x00012FCD, true),
                                    Row("cond.always", "act.wait", 0.0f, 0, 0, 0, false),
                                    Row("cond.foe_blocking", "act.power_attack", 0.0f, 0, 0, 0, true, 100) });
        StoreTable(kA, Id::Warrior, {});   // engaged and empty
        StoreTable(kA, Id::Dragonborn, { Row(std::string(GambitWire::kMaxOpcodeLen, 'c'), "", -1.25f, 4, 0xFE00A123, 0xFE00A456, true) });
        SetProgress(kB, Id::Mage, { 123456, 7 });
        SetMastered(kB, Bit(Id::Mage));
        SetEquipped(kC, Id::Archer);
        StoreTable(kC, Id::Ranged, Rows(kMaxStoredRows, 0x00000014));
    }

    struct Saved {
        bool          any = false;
        std::uint32_t version = 0;
        Bytes         data;
    };
    Saved Save() {
        SKSE::SerializationInterface io;
        CoSave(&io);
        Saved s;
        if (io.written.size() == 1 && io.written[0].type == kRecClasses) {
            s.any = true;
            s.version = io.written[0].version;
            s.data = io.written[0].data;
        }
        return s;
    }
    void Load(const Bytes& a_body, std::uint32_t a_version, std::function<bool(RE::FormID, RE::FormID&)> a_resolver = {}) {
        BeginLoad();   // what LoadCallback does first
        SKSE::SerializationInterface io;
        io.in = a_body;
        io.resolver = std::move(a_resolver);
        CoLoad(&io, a_version, static_cast<std::uint32_t>(a_body.size()));
    }
}

#ifdef MCLS_OLD_DLL
// ── the v1 DLL (tree f21f6c7) ─────────────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    static_assert(kClassesVersion == 1, "MCLS_OLD_DLL must be built against the v1 tree");
    if (argc == 2 && std::string_view(argv[1]) == "--emit-v1") {
        BuildV1State();
        const Saved s = Save();
        if (!s.any || s.version != 1 || g_failed) { std::fprintf(stderr, "old DLL: the v1 save failed\n"); return 2; }
        std::fputs(ToHex(s.data).c_str(), stdout);
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--downgrade") {
        Bytes v2;
        if (!FromHexFile(argv[2], v2)) { std::fprintf(stderr, "old DLL: cannot read %s\n", argv[2]); return 2; }
        Load(v2, 2);
        Check(HasLine("CLASS SAVE IS NEWER THAN THIS DLL"), "v1 DLL: says the save is newer, loudly");
        Check(!Writable(), "v1 DLL: class state is frozen (no setter runs)");
        Check(!SetEquipped(kA, Id::Mage), "v1 DLL: a setter refuses");
        Check(ConsumeReadOnlyNotice(), "v1 DLL: the on-screen read-only notice is raised");
        const Saved s = Save();
        Check(s.any && s.version == 2, "v1 DLL: writes the record back as version 2");
        Check(s.data == v2, "v1 DLL: writes the v2 bytes back UNCHANGED");
        std::printf("v1 DLL meets the v2 record (%zu bytes): %d checks, %d failed\n", v2.size(), g_checks, g_failed);
        return g_failed;
    }
    std::fprintf(stderr, "usage (old DLL build): --emit-v1 | --downgrade <v2.hex>\n");
    return 2;
}
#else
// ── this DLL ──────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
    bool SameRows(const std::optional<std::vector<Gambit>>& a, const std::optional<std::vector<Gambit>>& b) {
        if (a.has_value() != b.has_value()) return false;
        if (!a) return true;
        if (a->size() != b->size()) return false;
        for (std::size_t k = 0; k < a->size(); ++k) {
            const Gambit& x = (*a)[k];
            const Gambit& y = (*b)[k];
            if (x.conditionOpcode != y.conditionOpcode || x.actionOpcode != y.actionOpcode ||
                std::memcmp(&x.conditionParam, &y.conditionParam, sizeof(float)) != 0 || x.subjectSelector != y.subjectSelector ||
                x.subjectActorForm != y.subjectActorForm || x.actionParamForm != y.actionParamForm || x.enabled != y.enabled ||
                x.cooldownTenths != y.cooldownTenths)
                return false;
        }
        return true;
    }
    bool SameState(const FollowerClasses& a, const FollowerClasses& b) {
        if (a.equipped != b.equipped || a.mastered != b.mastered || a.unlocked != b.unlocked || a.thiefFights != b.thiefFights ||
            a.masterThiefFights != b.masterThiefFights || a.pickedT4 != b.pickedT4)
            return false;
        for (std::size_t c = 0; c < kClassCount; ++c)
            if (!(a.progress[c] == b.progress[c]) || !SameRows(a.tables[c], b.tables[c]) || !SameRows(a.logiTables[c], b.logiTables[c]))
                return false;
        return SameRows(a.offCombat, b.offCombat) && SameRows(a.offLogistics, b.offLogistics);
    }
    // What a row looks like after a save and a load with the identity resolver: a 0xFF subject actor is written as 0.
    std::vector<Gambit> AsSaved(std::vector<Gambit> a_rows) {
        for (auto& g : a_rows) {
            if (!Followers::IsPersistableID(g.subjectActorForm)) g.subjectActorForm = 0;
            g.cooldownTenths = (std::min)(g.cooldownTenths, kMaxCooldownTenths);
        }
        return a_rows;
    }
    std::map<RE::FormID, FollowerClasses> Store() {
        std::map<RE::FormID, FollowerClasses> m;
        for (const RE::FormID fid : detail::StoredFollowerIds())
            if (auto fc = GetFollower(fid)) m.emplace(fid, *fc);
        return m;
    }
    // The store as a save and a load leave it (the writer's transforms on every stored table).
    std::map<RE::FormID, FollowerClasses> SavedForm(std::map<RE::FormID, FollowerClasses> a_m) {
        for (auto& [fid, f] : a_m) {
            for (auto& t : f.tables) if (t) t = AsSaved(*t);
            for (auto& t : f.logiTables) if (t) t = AsSaved(*t);
            if (f.offCombat) f.offCombat = AsSaved(*f.offCombat);
            if (f.offLogistics) f.offLogistics = AsSaved(*f.offLogistics);
        }
        return a_m;
    }
    bool SameStore(const std::map<RE::FormID, FollowerClasses>& a, const std::map<RE::FormID, FollowerClasses>& b) {
        if (a.size() != b.size()) return false;
        for (const auto& [fid, fc] : a) {
            const auto it = b.find(fid);
            if (it == b.end() || !SameState(fc, it->second)) return false;
        }
        return true;
    }

    // ── the INDEPENDENT walk of the documented layout (State.cpp header). It shares no code with the codec. ──────
    struct Walk {
        const Bytes& b;
        std::size_t  p = 0;
        bool         ok = true;
        std::uint32_t U32() { std::uint32_t v = 0; if (p + 4 > b.size()) { ok = false; return 0; } std::memcpy(&v, b.data() + p, 4); p += 4; return v; }
        std::uint8_t  U8() { if (p + 1 > b.size()) { ok = false; return 0; } return b[p++]; }
        void Skip(std::size_t n) { if (p + n > b.size()) ok = false; else p += n; }
        void Str() { const std::uint32_t n = U32(); if (n > 64) ok = false; else Skip(n); }
        void RowsOf(std::uint32_t a_rowSchema) {
            const std::uint8_t n = U8();
            for (std::uint8_t k = 0; ok && k < n; ++k) {
                Str(); Skip(4 + 1 + 4);        // condition, f32 param, u8 subject, u32 subjectActor
                Str(); Skip(4 + 1);            // action, u32 actionParam, u8 flags
                if (a_rowSchema >= 6) Skip(2); // u16 cooldownTenths
            }
        }
    };
    struct Entry { std::size_t begin = 0, v1End = 0, end = 0; };
    // The follower entries of a record body at a_version. false = the bytes do not follow the documented layout.
    bool WalkRecord(const Bytes& a_b, std::uint32_t a_version, std::vector<Entry>& a_out, std::size_t& a_consumed) {
        Walk w{ a_b };
        const std::uint32_t rowSchema = w.U32();
        const std::uint8_t  classes = w.U8();
        const std::uint32_t followers = w.U32();
        for (std::uint32_t i = 0; w.ok && i < followers; ++i) {
            Entry e;
            e.begin = w.p;
            w.Skip(4 + 1 + std::size_t{ classes } * 5 + 8 + 8 + 2 + 2);   // fid, equipped, xp+level per class, masks, counters
            const std::uint8_t tables = w.U8();
            for (std::uint8_t t = 0; w.ok && t < tables; ++t) { w.U8(); w.RowsOf(rowSchema); }
            e.v1End = w.p;
            if (a_version >= 2) {
                w.U8();   // pickedT4
                const std::uint8_t logi = w.U8();
                for (std::uint8_t t = 0; w.ok && t < logi; ++t) { w.U8(); w.RowsOf(rowSchema); }
                const std::uint8_t offMask = w.U8();
                if (offMask & 1) w.RowsOf(rowSchema);
                if (offMask & 2) w.RowsOf(rowSchema);
            }
            e.end = w.p;
            a_out.push_back(e);
        }
        a_consumed = w.p;
        return w.ok;
    }

    void TestSelfTest() {
        std::printf("1. the in-DLL self-test (bClassCoSaveSelfTest)\n");
        Config::g_classCoSaveSelfTest.store(true);
        BeginLoad();
        Config::g_classCoSaveSelfTest.store(false);
        Check(HasLine("MCLS self-test (synthetic): PASS"), "self-test: synthetic v2 round trip PASS");
        Check(HasLine("MCLS self-test (rows v5, pre-G9): PASS"), "self-test: rows v5 PASS");
        Check(HasLine("MCLS self-test (v1 -> v2): PASS"), "self-test: v1 -> v2 PASS");
        Check(HasLine("MCLS self-test (v1 state re-written as v2): PASS"), "self-test: the upgraded state round-trips as v2");
        Check(HasLine("MCLS self-test (corrupt variants): PASS -- 11 of 11 refused"), "self-test: 11 corrupt variants refused");
        Check(CountLines('E', "FAIL") == 0, "self-test: no FAIL line");
        FlushLog("self-test");
    }

    Bytes TestGolden(const Bytes& a_golden) {
        std::printf("2. golden_v1.hex (written by the v1 DLL, %zu bytes) -> this DLL\n", a_golden.size());
        BuildV1State();
        const auto viaWriters = SavedForm(Store());   // a 0xFF subject actor is written as 0
        const Saved fresh = Save();
        Check(fresh.any && fresh.version == 2, "this DLL writes MCLS version 2");

        Load(a_golden, 1);
        Check(CountLines('E') == 0, "v1 load: no error line");
        Check(HasLine("MCLS v1"), "v1 load: the load line names the v1 record");
        const auto loaded = Store();
        Check(loaded.size() == 3, "v1 load: 3 followers");
        Check(SameStore(viaWriters, loaded), "v1 load: every field equals the state built through the writers");
        for (const auto& [fid, fc] : loaded) {
            bool none = fc.pickedT4 == kNoClass && !fc.offCombat && !fc.offLogistics;
            for (const auto& t : fc.logiTables) none = none && !t;
            Check(none, std::format("v1 load: {:08X} has no v2 state", fid));
        }
        Check(Writable(), "v1 load: class state is writable (not frozen)");
        const Saved up = Save();
        Check(up.any && up.version == 2, "v1 -> v2: the next save is version 2");
        Check(up.data == fresh.data, "v1 -> v2: same bytes as the state saved straight from the writers");

        // v2 = header + each v1 entry + FF 00 00, found by the independent walk.
        std::vector<Entry> ents;
        std::size_t used = 0;
        Check(WalkRecord(a_golden, 1, ents, used) && used == a_golden.size() && ents.size() == 3,
              "independent walk: the golden bytes follow the documented v1 layout exactly");
        Bytes want(a_golden.begin(), a_golden.begin() + 9);
        for (const auto& e : ents) {
            want.insert(want.end(), a_golden.begin() + static_cast<std::ptrdiff_t>(e.begin), a_golden.begin() + static_cast<std::ptrdiff_t>(e.end));
            want.insert(want.end(), { 0xFF, 0x00, 0x00 });
        }
        Check(up.data == want, "v1 -> v2: the v2 record is each v1 entry, untouched, plus FF 00 00");

        Load(up.data, 2);
        Check(CountLines('E') == 0 && SameStore(viaWriters, Store()), "v2 load of the upgraded record: same state, no error");
        Check(Save().data == up.data, "v2 -> v2: byte-identical");
        FlushLog("golden");
        return up.data;
    }

    // Builds a state with EVERY v2 field through the only roads (SwapClass, SetPickedT4). Returns the saved v2 bytes.
    Bytes TestSwitchAndV2() {
        std::printf("3 + 4. the class switch and a record with every v2 field\n");
        ClearAll();
        const auto c0 = Rows(3, 0x100), l0 = Rows(4, 0x200), c1 = Rows(5, 0x300), l1 = Rows(2, 0x400), c2 = Rows(1, 0x500),
                   l2 = std::vector<Gambit>{}, c3 = Rows(kMaxStoredRows, 0x600), l3 = Rows(7, 0x700);
        Check(SetMastered(kA, Bit(Id::Melee)), "switch: Melee mastered (Knight and Warrior unlock)");

        auto before = Store();
        auto s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::GreatKnight), c0, l0);
        Check(!s.done && SameStore(before, Store()), "switch: a LOCKED class is refused and nothing changes");
        s = detail::SwapClass(kA, kNoClass, c0, l0);
        Check(!s.done && SameStore(before, Store()), "switch: classes OFF -> classes OFF is refused and nothing changes");
        s = detail::SwapClass(kA, 200, c0, l0);
        Check(!s.done && SameStore(before, Store()), "switch: an id out of range is refused");
        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Knight), Rows(kMaxStoredRows + 1, 1), l0);
        Check(!s.done && SameStore(before, Store()) && CountLines('E', "class switch REFUSED") == 1, "switch: a live table over 64 rows is refused, loudly");
        s = detail::SwapClass(0xFF000123, static_cast<std::uint8_t>(Id::Melee), c0, l0);
        Check(!s.done && SameStore(before, Store()), "switch: a 0xFF runtime follower id is refused");
        g_testActive = false;
        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Knight), c0, l0);
        Check(!s.done && SameStore(before, Store()), "switch: refused while the class tree is not active");
        g_testActive = true;

        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Knight), c0, l0);   // classes OFF -> Knight (first equip)
        auto fc = *GetFollower(kA);
        Check(s.done && s.from == kNoClass && !s.combat && !s.logistics, "OFF -> Knight: done, nothing stored for Knight (first equip)");
        Check(fc.equipped == static_cast<std::uint8_t>(Id::Knight) && fc.progress[static_cast<std::size_t>(Id::Knight)].level == 1,
              "OFF -> Knight: equipped, started at level 1");
        Check(SameRows(fc.offCombat, c0) && SameRows(fc.offLogistics, l0), "OFF -> Knight: the pre-class tables are in the classes-OFF slot");
        Check(fc.offCombat && !fc.offCombat->empty() && (*fc.offCombat)[0].uid == 0, "stored rows carry no runtime uid");

        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Warrior), c1, l1);   // Knight -> Warrior (first equip)
        fc = *GetFollower(kA);
        Check(s.done && s.from == static_cast<std::uint8_t>(Id::Knight) && !s.combat && !s.logistics, "Knight -> Warrior: done, first equip");
        Check(SameRows(fc.tables[static_cast<std::size_t>(Id::Knight)], c1) && SameRows(fc.logiTables[static_cast<std::size_t>(Id::Knight)], l1),
              "Knight -> Warrior: Knight's combat AND logistics tables are stored (marth Q29)");

        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Knight), c2, l2);   // Warrior -> Knight (restore)
        fc = *GetFollower(kA);
        Check(s.done && SameRows(s.combat, c1) && SameRows(s.logistics, l1), "Warrior -> Knight: Knight's own tables come back (marth Q8)");
        Check(!fc.tables[static_cast<std::size_t>(Id::Knight)] && !fc.logiTables[static_cast<std::size_t>(Id::Knight)],
              "Warrior -> Knight: the handed-back tables are consumed from the record");
        Check(SameRows(fc.tables[static_cast<std::size_t>(Id::Warrior)], c2) && fc.logiTables[static_cast<std::size_t>(Id::Warrior)] &&
                  fc.logiTables[static_cast<std::size_t>(Id::Warrior)]->empty(),
              "Warrior -> Knight: Warrior's tables are stored, the EMPTY logistics table as an engaged empty one");
        before = Store();
        s = detail::SwapClass(kA, static_cast<std::uint8_t>(Id::Knight), c3, l3);
        Check(!s.done && SameStore(before, Store()), "Knight -> Knight is refused and nothing changes");

        // Dragonborn's picked T4 (marth Q60): a tier-4 class he has mastered.
        Check(!SetPickedT4(kA, Id::Knight), "picked T4: a class that is not tier 4 is refused");
        Check(!SetPickedT4(kA, Id::Listener), "picked T4: a T4 he has not mastered is refused");
        Check(GetPickedT4(kA) == kNoClass, "picked T4: still none");
        Check(SetMastered(kA, Bit(Id::Melee) | Bit(Id::Listener)) && SetPickedT4(kA, Id::Listener) &&
                  GetPickedT4(kA) == static_cast<std::uint8_t>(Id::Listener), "picked T4: a mastered T4 is stored");

        // A second follower who goes back to classes OFF (so a record without off tables, with stored class tables).
        Check(detail::SwapClass(kC, static_cast<std::uint8_t>(Id::Mage), c3, l3).done, "second follower: OFF -> Mage");
        s = detail::SwapClass(kC, kNoClass, c0, l1);
        fc = *GetFollower(kC);
        Check(s.done && s.from == static_cast<std::uint8_t>(Id::Mage) && SameRows(s.combat, c3) && SameRows(s.logistics, l3),
              "Mage -> classes OFF: the pre-class tables come back");
        Check(fc.equipped == kNoClass && !fc.offCombat && !fc.offLogistics && SameRows(fc.tables[static_cast<std::size_t>(Id::Mage)], c0) &&
                  SameRows(fc.logiTables[static_cast<std::size_t>(Id::Mage)], l1),
              "Mage -> classes OFF: the slot is consumed, Mage's tables are stored");
        // A third with only a logistics table stored through the plain writers, a 0xFF subject actor and an over-bound cooldown.
        Check(StoreTable(kB, Id::Healer, { Row("cond.always", "act.loot_equipment", 1.0f, 0, 0xFF000801, 0xFE00A456, true, 0xFFFF) }, TableKind::Logistics) &&
                  HasStoredTable(kB, Id::Healer, TableKind::Logistics) && !HasStoredTable(kB, Id::Healer, TableKind::Combat),
              "StoreTable(Logistics) stores the logistics table only");
        Check(CountLines('E') == 1, "no error line beyond the one expected refusal");
        FlushLog("switch");

        const auto live = Store();
        const Saved a = Save();
        Check(a.any && a.version == 2, "v2 state: saved as version 2");
        std::vector<Entry> ents;
        std::size_t used = 0;
        Check(WalkRecord(a.data, 2, ents, used) && used == a.data.size() && ents.size() == 3,
              "independent walk: the v2 bytes follow the documented v2 layout exactly");
        Load(a.data, 2);
        Check(!SameStore(live, Store()) && CountLines('E') == 0 && SameStore(SavedForm(live), Store()),
              "v2 load: every field (v1 and v2) equals what was saved (a 0xFF subject actor as 0, the cooldown clamped)");
        Check(Save().data == a.data, "v2 -> v2: byte-identical");
        Check(HasLine("2 combat + 3 logistics + 2 classes-OFF stored table(s)"), "v2 load line counts the three kinds of table");
        FlushLog("v2 round trip");
        return a.data;
    }

    void TestCompat(const Bytes& a_golden, const Bytes& a_v2) {
        std::printf("5. inactive add-on, FormID re-resolution, a newer record\n");
        // marth Q9: with the add-on inactive the state is held and written back (the record is re-encoded, now as v2).
        g_testActive = false;
        Load(a_v2, 2);
        const auto held = Store();
        Check(!Writable() && !SetEquipped(kA, Id::Mage) && !detail::SwapClass(kA, kNoClass, {}, {}).done, "inactive: every writer refuses");
        Check(SameStore(held, Store()) && Save().data == a_v2, "inactive: the v2 record is written back byte-identically (marth Q9)");
        Load(a_golden, 1);
        const Saved up = Save();
        g_testActive = true;
        Load(a_golden, 1);
        Check(up.version == 2 && up.data == Save().data, "inactive: a v1 record is held and written back as the same v2 bytes an active add-on writes");
        FlushLog("inactive");

        // The load order moved: one follower re-resolves, one is gone, a row's spell re-resolves, another's is gone.
        const auto remap = [](RE::FormID a_old, RE::FormID& a_new) {
            if (a_old == kB) return false;                                  // that follower's plugin is gone
            if (a_old == 0x00000101) return false;                          // a stored row's spell is gone
            a_new = (a_old == kC) ? 0x0B000D62 : (a_old == 0x00000100 ? 0x05000100 : a_old);
            return true;
        };
        Load(a_v2, 2, remap);
        const auto moved = Store();
        Check(moved.size() == 2 && moved.contains(kA) && moved.contains(0x0B000D62), "re-resolve: the gone follower is dropped, the moved one re-keyed");
        if (moved.contains(kA) && moved.at(kA).offCombat && moved.at(kA).offCombat->size() == 3) {
            const auto& off = *moved.at(kA).offCombat;   // a v2 table: its rows go through the same resolve as a v1 table's
            Check(off[0].actionParamForm == 0x05000100 && off[0].enabled, "re-resolve: a v2 table row's spell follows the load order");
            Check(off[1].actionParamForm == 0 && !off[1].enabled, "re-resolve: a v2 table row whose spell is gone is DISABLED, not guessed");
            Check(off[2].actionParamForm == 0x00000102, "re-resolve: the rows around it are untouched");
        } else {
            Check(false, "re-resolve: the classes-OFF combat table is there");
        }
        Check(HasLine("dropped 1 unresolvable follower(s); disabled 2 rule(s)"), "re-resolve: the drop and the disabled rows are counted in the load line");
        FlushLog("re-resolve");

        // A record from a NEWER DLL (version 3) is kept verbatim and written back unchanged; class state is frozen.
        Bytes v3 = a_v2;
        v3.push_back(0xAB);
        Load(v3, 3);
        Check(!Writable() && ConsumeReadOnlyNotice(), "newer record: frozen, the read-only notice is raised");
        const Saved kept = Save();
        Check(kept.any && kept.version == 3 && kept.data == v3, "newer record: written back as version 3, bytes unchanged");
        Load({}, 0);
        Check(Store().empty() && !Save().any, "version 0: skipped, nothing kept, nothing written");
        FlushLog("newer");
    }

    void TestCorruption(const Bytes& a_v2) {
        std::printf("6. every truncation and every single-byte corruption of the v2 record (%zu bytes)\n", a_v2.size());
        std::vector<Entry> ents;
        std::size_t used = 0;
        WalkRecord(a_v2, 2, ents, used);
        std::size_t cleanTruncations = 0, lostSilently = 0;
        for (std::size_t n = 0; n < a_v2.size(); ++n) {
            Load(Bytes(a_v2.begin(), a_v2.begin() + static_cast<std::ptrdiff_t>(n)), 2);
            if (CountLines('E') == 0) ++cleanTruncations;
            // "keep what parsed": exactly the followers whose whole entry is inside the truncated bytes.
            std::size_t whole = 0;
            for (const auto& e : ents) if (e.end <= n) ++whole;
            if (Store().size() != whole) ++lostSilently;
            spdlog::Lines().clear();
        }
        Check(cleanTruncations == 0, std::format("truncation: none of {} truncated records loads without an error line", a_v2.size()));
        Check(lostSilently == 0, "truncation: the followers parsed before the cut are kept, never more, never fewer");
        std::size_t loads = 0;
        for (std::size_t i = 0; i < a_v2.size(); ++i)
            for (const std::uint8_t v : { std::uint8_t{ 0x00 }, std::uint8_t{ 0xFF }, static_cast<std::uint8_t>(a_v2[i] ^ 0x80), static_cast<std::uint8_t>(a_v2[i] + 1) }) {
                if (v == a_v2[i]) continue;
                Bytes t = a_v2;
                t[i] = v;
                Load(t, 2);
                const Saved s = Save();   // whatever was kept must still encode
                if (s.any) Load(s.data, s.version);
                ++loads;
                spdlog::Lines().clear();
            }
        Check(loads > 0, std::format("corruption: {} single-byte variants loaded and re-saved without a crash", loads));
        ClearAll();
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: mcls_byte_test <golden_v1.hex> [<v2-out.hex>]\n"); return 2; }
    Bytes golden;
    if (!FromHexFile(argv[1], golden) || golden.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::printf("MCLS co-save record, version %u (rows v%u, %zu classes)\n\n", kClassesVersion, GambitWire::kRowSchema, kClassCount);
    TestSelfTest();
    const Bytes upgraded = TestGolden(golden);
    const Bytes v2 = TestSwitchAndV2();
    TestCompat(golden, v2);
    TestCorruption(v2);
    (void)upgraded;
    if (argc >= 3) std::ofstream(argv[2]) << ToHex(v2);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed;
}
#endif
