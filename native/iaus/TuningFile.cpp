// iaus/TuningFile.cpp -- see TuningFile.h. Spec parser = the simulator's (tools/iaus_sim/RealClasses.cpp ApplyCurveSpecs, Tune.h SetParam / ParseSets): the two are
// DUPLICATED until the sim moves onto this parser (Docs/REVIEW-BACKLOG.md). Every edit goes through the native edit surface next to its table:
// ClassCurves::SetCurveParam / SetScalar / SetWeight / SetFamilyWeight, SetTuningScalar (Catalogue.cpp), SetWorthKnob (ConsumableWorth.h), SetLootKnob (LootNeed.h).
#include "PCH.h"
#include "iaus/TuningFile.h"

#include "classes/Classes.h"
#include "iaus/ClassCurves.h"
#include "iaus/LootNeed.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace MFO::Iaus::TuningFile {

    namespace {
        namespace CC = ClassCurves;
        namespace fs = std::filesystem;

        constexpr const char*  kDir = "Data/SKSE/Plugins/MFO_Tuning";
        constexpr auto         kCheckEvery = std::chrono::seconds(2);
        constexpr std::size_t  kMaxBytes = 256 * 1024;

        enum class Kind { Class, Shared, Potions, Looting };

        struct Stamp {
            bool exists = false;
            std::int64_t mtime = 0;
            std::uintmax_t size = 0;
            bool operator==(const Stamp&) const = default;
        };

        struct Entry {
            std::string name;          // the file name without .txt (the class slug, or shared / potions / looting)
            Kind kind = Kind::Class;
            std::size_t row = 0;       // Kind::Class: the ClassCurves row
            Stamp last;                // what was last loaded
            std::optional<Stamp> pending;   // a change seen once; applied when seen unchanged on the next check
        };

        struct State {
            std::vector<Entry> files;
            bool started = false;
            std::chrono::steady_clock::time_point lastCheck{};
            WorthTable worth = ShippedTuning().worth;   // potions.txt applied; shared.txt rebuilds copy it back into the live Tuning
        };
        State g;

        struct Counts { int applied = 0; int rejected = 0; };

        std::string_view Trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        }
        std::string Lower(std::string_view s) { std::string o(s); for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return o; }

        bool ParseFloat(std::string_view s, float& out) {
            s = Trim(s);
            if (s.empty()) return false;
            const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
            return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(out);
        }

        // A family / consideration token: an index, or a Name() (the sim's `index`: the first display-name match; several considerations share a name, so use the index there).
        template <class NameOf>
        long IndexOf(std::string_view tok, std::size_t n, NameOf nameOf) {
            if (tok.empty()) return -1;
            if (std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                if (tok.size() > 6) return -1;
                const long v = std::atol(std::string(tok).c_str());
                return static_cast<std::size_t>(v) < n ? v : -1;
            }
            const std::string want = Lower(tok);
            for (std::size_t i = 0; i < n; ++i) if (Lower(nameOf(i)) == want) return static_cast<long>(i);
            return -1;
        }
        long FamilyOf(std::string_view t) { return IndexOf(t, kFamilies, [](std::size_t i) { return std::string(Name(static_cast<Family>(i))); }); }
        long ConsOf(std::string_view t) { return IndexOf(t, kCons, [](std::size_t i) { return std::string(Name(static_cast<Cons>(i))); }); }

        std::uint64_t Fnv1a(const std::string& s) {
            std::uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
            return h;
        }

        // One spec line of a class file: [<class>:]<path>=<v>. a_shared = the live shared tuning a curve override starts from (the sim passes Tune().t).
        bool ApplyClassSpec(CC::Row& a_row, std::string_view a_path, float a_v, const Tuning& a_shared, std::string& a_err) {
            if (a_path.rfind("scalar.", 0) == 0) {
                if (!CC::SetScalar(a_row, a_path.substr(7), a_v)) { a_err = "unknown scalar (drinkHealthLine | touchSpentSec | schoolPeak | schoolFloor | schoolShape | schoolMasteredBump)"; return false; }
                return true;
            }
            if (a_path.rfind("family.", 0) == 0) {
                const long ff = FamilyOf(a_path.substr(7));
                if (ff < 0 || !CC::SetFamilyWeight(a_row, static_cast<Family>(ff), a_v)) { a_err = "unknown family or bad weight (family.<family>=<v>, v > 0)"; return false; }
                return true;
            }
            if (a_path == "equip.crossover") {   // the posture EDGE: both distance curves' midpoint b at once (sim: the same two lines)
                for (const Cons k : { Cons::PostureRanged, Cons::PostureMelee })
                    if (!CC::SetCurveParam(a_row, Family::Posture, k, a_shared.curve[static_cast<std::size_t>(k)], "b", a_v)) { a_err = "bad value for equip.crossover"; return false; }
                return true;
            }
            const auto d1 = a_path.find('.');
            const auto d2 = d1 == std::string_view::npos ? d1 : a_path.find('.', d1 + 1);
            if (d2 == std::string_view::npos) { a_err = "bad path (<family>.<cons>.<param> | scalar.<name> | family.<family> | equip.crossover)"; return false; }
            const long fi = FamilyOf(a_path.substr(0, d1));
            const long ki = ConsOf(a_path.substr(d1 + 1, d2 - d1 - 1));
            if (fi < 0 || ki < 0) { a_err = "unknown family or consideration"; return false; }
            const std::string_view param = a_path.substr(d2 + 1);
            if (param == "w") {
                if (!CC::SetWeight(a_row, static_cast<Family>(fi), static_cast<Cons>(ki), a_v)) { a_err = "bad weight (v > 0)"; return false; }
                return true;
            }
            if (!CC::SetCurveParam(a_row, static_cast<Family>(fi), static_cast<Cons>(ki), a_shared.curve[static_cast<std::size_t>(ki)], param, a_v)) {
                a_err = "unknown curve param or bad value (a | b | floor | zeroAbove | zeroBelow | w | px<i> / py<i> on a Piece curve)"; return false;
            }
            return true;
        }

        // One entry of shared.txt (the sim's --set): t.<name> | w.<family>.<cons> | aw.<family> | c.<cons>.a|b|floor.
        bool ApplySharedEntry(Tuning& a_t, std::vector<Profile::WeightOverride>& a_w, std::string_view a_name, float a_v, std::string& a_err) {
            auto idx = [&](std::string_view s) -> long {
                if (s.empty() || s.size() > 6 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return -1;
                return std::atol(std::string(s).c_str());
            };
            if (a_name.rfind("w.", 0) == 0) {
                const auto dot = a_name.find('.', 2);
                const long fi = dot == std::string_view::npos ? -1 : idx(a_name.substr(2, dot - 2)), ci = dot == std::string_view::npos ? -1 : idx(a_name.substr(dot + 1));
                if (fi < 0 || ci < 0 || static_cast<std::size_t>(fi) >= kFamilies || static_cast<std::size_t>(ci) >= kCons || !(a_v > 0.0f)) { a_err = "bad weight (w.<family index>.<cons index>=<v>, v > 0)"; return false; }
                for (auto& o : a_w) if (o.family == static_cast<Family>(fi) && o.cons == static_cast<Cons>(ci)) { o.w = a_v; return true; }
                a_w.push_back({ static_cast<Family>(fi), static_cast<Cons>(ci), a_v });
                return true;
            }
            if (a_name.rfind("aw.", 0) == 0) {
                const long fi = idx(a_name.substr(3));
                if (fi < 0 || static_cast<std::size_t>(fi) >= kFamilies) { a_err = "bad family index (aw.<family index>=<v>)"; return false; }
                a_t.actionWeight[static_cast<std::size_t>(fi)] = a_v;
                return true;
            }
            if (a_name.rfind("c.", 0) == 0) {
                const auto dot = a_name.find('.', 2);
                const long ci = dot == std::string_view::npos ? -1 : idx(a_name.substr(2, dot - 2));
                if (ci < 0 || static_cast<std::size_t>(ci) >= kCons) { a_err = "bad consideration index (c.<cons index>.a|b|floor=<v>)"; return false; }
                Curve& c = a_t.curve[static_cast<std::size_t>(ci)];
                const std::string_view f = a_name.substr(dot + 1);
                if (f == "a") c.a = a_v; else if (f == "b") c.b = a_v; else if (f == "floor") c.floor = a_v;
                else { a_err = "unknown curve field (a | b | floor)"; return false; }
                return true;
            }
            if (a_name.rfind("st.", 0) == 0 || a_name.rfind("pk.", 0) == 0 || a_name.rfind("row.", 0) == 0) { a_err = "a simulator-only knob (the class package): the game has no table for it"; return false; }
            if (a_name.rfind("worth.", 0) == 0) { a_err = "belongs in potions.txt"; return false; }
            if (a_name.rfind("loot.", 0) == 0) { a_err = "belongs in looting.txt"; return false; }
            if (!SetTuningScalar(a_t, a_name, a_v)) { a_err = "unknown t.<name> scalar"; return false; }
            return true;
        }

        // Walk a file line by line. a_apply(entryText, err) -> bool for each entry (a comma list splits into entries when a_split).
        template <class Apply>
        Counts Walk(const std::string& a_text, const std::string& a_file, bool a_split, Apply a_apply, bool a_quiet) {
            Counts n;
            std::istringstream in(a_text);
            std::string raw;
            int line = 0;
            while (std::getline(in, raw)) {
                ++line;
                std::string_view v = raw;
                if (const auto hash = v.find('#'); hash != std::string_view::npos) v = v.substr(0, hash);
                v = Trim(v);
                if (v.empty()) continue;
                for (const char* lead : { "--set", "--curve" })   // a tuning run's own output pastes in unchanged
                    if (v.rfind(lead, 0) == 0 && v.size() > std::char_traits<char>::length(lead) && std::isspace(static_cast<unsigned char>(v[std::char_traits<char>::length(lead)]))) {
                        v = Trim(v.substr(std::char_traits<char>::length(lead)));
                        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = Trim(v.substr(1, v.size() - 2));
                        break;
                    }
                std::size_t p = 0;
                while (p <= v.size()) {
                    const auto q = a_split ? v.find(',', p) : std::string_view::npos;
                    const std::string_view ent = Trim(v.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                    if (!ent.empty()) {
                        std::string err;
                        if (a_apply(ent, err)) ++n.applied;
                        else { ++n.rejected; spdlog::warn("[tuning] MFO_Tuning/{}:{}: rejected '{}': {}", a_file, line, std::string(ent), err); }
                    }
                    if (q == std::string_view::npos) break;
                    p = q + 1;
                }
            }
            (void)a_quiet;
            return n;
        }

        // name=value split shared by every file kind; the value must parse whole and be finite.
        bool SplitNameValue(std::string_view a_ent, std::string_view& a_name, float& a_v, std::string& a_err) {
            const auto eq = a_ent.find('=');
            if (eq == std::string_view::npos) { a_err = "no '=' (expected <name>=<value>)"; return false; }
            a_name = Trim(a_ent.substr(0, eq));
            if (a_name.empty()) { a_err = "empty name"; return false; }
            if (!ParseFloat(a_ent.substr(eq + 1), a_v)) { a_err = "unparsable value"; return false; }
            return true;
        }

        std::optional<std::string> ReadFile(const std::string& a_name) {
            std::ifstream f(std::string(kDir) + "/" + a_name + ".txt", std::ios::binary);
            if (!f) return std::nullopt;
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (s.size() > kMaxBytes) { spdlog::warn("[tuning] MFO_Tuning/{}.txt is {} bytes (cap {}): ignored, shipped values", a_name, s.size(), kMaxBytes); return std::nullopt; }
            return s;
        }

        void LogLoaded(const std::string& a_name, const Counts& n, std::uint64_t a_hash, bool a_quiet) {
            if (a_quiet) { spdlog::debug("[tuning] loaded MFO_Tuning/{}.txt: {} applied, {} rejected, hash {:016x}", a_name, n.applied, n.rejected, a_hash); return; }
            spdlog::info("[tuning] loaded MFO_Tuning/{}.txt: {} applied, {} rejected, hash {:016x}", a_name, n.applied, n.rejected, a_hash);
        }

        // ── the four rebuilds: each starts from the SHIPPED table, then applies its file (so deleting a line reverts it) ──────────────
        void RebuildPotions(const Entry& e, bool a_firstLoad) {
            WorthTable w = ShippedTuning().worth;
            const auto text = ReadFile(e.name);
            if (text) {
                const Counts n = Walk(*text, e.name + ".txt", false, [&](std::string_view ent, std::string& err) {
                    std::string_view name; float v = 0.0f;
                    if (!SplitNameValue(ent, name, v, err)) return false;
                    if (name.rfind("worth.", 0) != 0) { err = "a potions.txt line is worth.<name>=<v>"; return false; }
                    if (!SetWorthKnob(w, name.substr(6), v)) { err = "unknown worth knob or non-finite value"; return false; }
                    return true; }, false);
                LogLoaded(e.name, n, Fnv1a(*text), a_firstLoad && n.applied + n.rejected == 0);
            } else if (!a_firstLoad) spdlog::info("[tuning] MFO_Tuning/{}.txt not present: shipped values", e.name);
            g.worth = w;
            LiveTuning().worth = w;
        }

        void RebuildLooting(const Entry& e, bool a_firstLoad) {
            LootTuning t = ShippedLootTuning();
            const auto text = ReadFile(e.name);
            if (text) {
                const Counts n = Walk(*text, e.name + ".txt", false, [&](std::string_view ent, std::string& err) {
                    std::string_view name; float v = 0.0f;
                    if (!SplitNameValue(ent, name, v, err)) return false;
                    if (name.rfind("loot.", 0) != 0) { err = "a looting.txt line is loot.<name>=<v>"; return false; }
                    if (!SetLootKnob(t, name.substr(5), v)) { err = "unknown loot knob or bad value (an int knob takes a whole number, consumableSellRoad 0 or 1)"; return false; }
                    return true; }, false);
                LogLoaded(e.name, n, Fnv1a(*text), a_firstLoad && n.applied + n.rejected == 0);
            } else if (!a_firstLoad) spdlog::info("[tuning] MFO_Tuning/{}.txt not present: shipped values", e.name);
            LiveLootTuning() = t;
        }

        void RebuildShared(const Entry& e, bool a_firstLoad) {
            Tuning t = ShippedTuning();
            std::vector<Profile::WeightOverride> weights;
            const auto text = ReadFile(e.name);
            if (text) {
                const Counts n = Walk(*text, e.name + ".txt", true, [&](std::string_view ent, std::string& err) {
                    std::string_view name; float v = 0.0f;
                    if (!SplitNameValue(ent, name, v, err)) return false;
                    if (name.find(':') != std::string_view::npos) { err = "a class line belongs in that class's own file"; return false; }
                    return ApplySharedEntry(t, weights, name, v, err); }, false);
                LogLoaded(e.name, n, Fnv1a(*text), a_firstLoad && n.applied + n.rejected == 0);
            } else if (!a_firstLoad) spdlog::info("[tuning] MFO_Tuning/{}.txt not present: shipped values", e.name);
            t.worth = g.worth;   // potions.txt owns the worth table
            LiveTuning() = t;
            CC::SetSharedWeights(weights);
        }

        void RebuildClass(const Entry& e, bool a_firstLoad, bool a_quiet) {
            CC::Row row = CC::Defaults()[e.row];
            const auto text = ReadFile(e.name);
            if (text) {
                const Tuning& shared = LiveTuning();
                const Counts n = Walk(*text, e.name + ".txt", false, [&](std::string_view ent, std::string& err) {
                    const auto eq = ent.find('=');
                    if (eq == std::string_view::npos) { err = "no '=' (expected [<class>:]<path>=<value>)"; return false; }
                    std::string_view lhs = Trim(ent.substr(0, eq));
                    float v = 0.0f;
                    if (!ParseFloat(ent.substr(eq + 1), v)) { err = "unparsable value"; return false; }
                    if (const auto colon = lhs.find(':'); colon != std::string_view::npos) {
                        const std::string_view slug = Trim(lhs.substr(0, colon));
                        if (slug != e.name) { err = "names class '" + std::string(slug) + "' but this is " + e.name + ".txt"; return false; }
                        lhs = Trim(lhs.substr(colon + 1));
                    }
                    return ApplyClassSpec(row, lhs, v, shared, err); }, a_quiet);
                LogLoaded(e.name, n, Fnv1a(*text), a_quiet || (a_firstLoad && n.applied + n.rejected == 0));
            } else if (!a_firstLoad && !a_quiet) spdlog::info("[tuning] MFO_Tuning/{}.txt not present: shipped values", e.name);
            CC::SetLiveRow(e.row, row);
        }

        Stamp StatOf(const Entry& e) {
            std::error_code ec;
            const fs::path p = fs::path(kDir) / (e.name + ".txt");
            Stamp s;
            s.exists = fs::is_regular_file(p, ec) && !ec;
            if (!s.exists) return s;
            ec.clear(); s.mtime = fs::last_write_time(p, ec).time_since_epoch().count();
            ec.clear(); s.size = fs::file_size(p, ec);
            return s;
        }

        void BuildFileList() {
            const char* const kOff[] = { "melee-off", "ranged-off", "mage-off" };
            g.files.clear();
            g.files.push_back({ "shared", Kind::Shared, 0, {}, {} });
            g.files.push_back({ "potions", Kind::Potions, 0, {}, {} });
            g.files.push_back({ "looting", Kind::Looting, 0, {}, {} });
            for (std::size_t i = 0; i < CC::kClassRows; ++i)
                g.files.push_back({ std::string(Classes::Get(static_cast<Classes::Id>(i)).slug), Kind::Class, i, {}, {} });
            for (std::size_t i = 0; i < 3; ++i) g.files.push_back({ kOff[i], Kind::Class, CC::kBaseOffFirst + i, {}, {} });
        }

        void RebuildAllClasses(bool a_firstLoad, bool a_quiet) {
            for (const Entry& e : g.files) if (e.kind == Kind::Class) RebuildClass(e, a_firstLoad, a_quiet);
        }
    }

    void Poll() {
        const auto now = std::chrono::steady_clock::now();
        if (g.started && now - g.lastCheck < kCheckEvery) return;
        g.lastCheck = now;

        if (!g.started) {   // FIRST CALL: load everything, before any table is read by a lap
            g.started = true;
            BuildFileList();
            int present = 0;
            std::error_code ec;
            const bool dir = fs::is_directory(kDir, ec) && !ec;
            for (Entry& e : g.files) { e.last = StatOf(e); if (e.last.exists) ++present; }
            if (!dir) spdlog::info("[tuning] MFO_Tuning/ folder not found: shipped values for every table");
            else spdlog::info("[tuning] MFO_Tuning/: {} of {} files present, the rest use shipped values", present, g.files.size());
            for (const Entry& e : g.files) if (e.kind == Kind::Potions) RebuildPotions(e, true);
            for (const Entry& e : g.files) if (e.kind == Kind::Looting) RebuildLooting(e, true);
            for (const Entry& e : g.files) if (e.kind == Kind::Shared) RebuildShared(e, true);
            RebuildAllClasses(true, false);
            return;
        }

        std::vector<const Entry*> changed;
        for (Entry& e : g.files) {
            const Stamp cur = StatOf(e);
            if (cur == e.last) { e.pending.reset(); continue; }
            if (e.pending && *e.pending == cur) { e.last = cur; e.pending.reset(); changed.push_back(&e); }
            else e.pending = cur;   // first sight of a change: wait one more check so a file still being written is not read half-done
        }
        if (changed.empty()) return;

        bool sharedChanged = false;
        for (const Entry* e : changed) if (e->kind == Kind::Potions) RebuildPotions(*e, false);
        for (const Entry* e : changed) if (e->kind == Kind::Looting) RebuildLooting(*e, false);
        for (const Entry* e : changed) if (e->kind == Kind::Shared) { RebuildShared(*e, false); sharedChanged = true; }
        if (sharedChanged) {   // a class curve override starts from the live shared curve, so every class row is re-applied (quietly) on top of the new shared table
            spdlog::info("[tuning] shared.txt changed: class rows re-applied on top of it");
            RebuildAllClasses(false, true);
            for (const Entry* e : changed) if (e->kind == Kind::Class) RebuildClass(*e, false, false);   // the changed ones log their own line
        } else {
            for (const Entry* e : changed) if (e->kind == Kind::Class) RebuildClass(*e, false, false);
        }
    }
}
