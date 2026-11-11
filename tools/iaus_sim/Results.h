#pragma once
// iaus_sim/Results.h -- one JSON results file per run, for the dashboard (tools/iaus_dash). HOST-ONLY.
//
// SCHEMA 1 (stable: fields are added, never renamed). Default path ~/.cache/mfo-iaus-sim/runs/<run_id>.json (cache
// only, never committed). The schema is written out in Docs/research/2026-11/iaus-sim-v2.md "Results file".
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Sim {

    struct JsonOut {
        std::string s;
        static std::string Esc(const std::string& v) { std::string r; for (char ch : v) { if (ch == '"' || ch == '\\') r += '\\'; if (static_cast<unsigned char>(ch) < 0x20) { r += ' '; continue; } r += ch; } return r; }
        static std::string Num(double v) { if (!(v == v) || v > 1e300 || v < -1e300) return "null"; char b[40]; std::snprintf(b, sizeof b, "%.6g", v); return b; }
        static std::string Str(const std::string& v) { return "\"" + Esc(v) + "\""; }
        static std::string Map(const std::map<std::string, double>& m) { std::string r = "{"; bool first = true; for (const auto& kv : m) { if (!first) r += ","; first = false; r += Str(kv.first) + ":" + Num(kv.second); } return r + "}"; }
    };

    // A measurable ruling: a class pattern ("*", a slug, "line:Mage", "line:Mage:tier>=2"), a metric of the results file and a bound.
    struct Ruling { const char* id; const char* text; const char* cls; const char* metric; const char* op; double target; };

    // marth's rulings that a number can check. Sources: Docs/research/2026-11/class-walkthrough-decisions.md and the IAUS rulings in
    // Docs/research/2026-11 (the mage melee ruling: bf36aec). More are added as the class playstyles are written.
    inline const std::vector<Ruling>& Rulings() {
        static const std::vector<Ruling> r = {
            { "R-MAGE-MELEE-T1", "a T1 mage almost never melees (low single digits)", "mage", "melee_share_pct", "<=", 5.0 },
            { "R-MAGE-MELEE-T2+", "pure mages up the tree melee even less", "line:Mage:tier>=2", "melee_share_pct", "<=", 3.0 },
            { "R-NO-SPLASH", "never splash the party", "*", "splash_ally_hits_per_fight", "<=", 0.0 },
            { "R-BAD-FACTS", "no candidate ever carries a NaN / inf fact", "*", "bad_facts", "<=", 0.0 },
            { "R-INVALID", "IAUS never offers what the world cannot do", "*", "invalid", "<=", 0.0 },
            { "R-BUDGET", "Decide stays inside fIausBudgetMs (0.5 ms)", "*", "decide_us_max", "<=", 500.0 },
        };
        return r;
    }
    inline bool RulingApplies(const Ruling& r, const std::string& cls, const std::string& line, int tier) {
        const std::string p = r.cls;
        if (p == "*") return true;
        if (p.rfind("line:", 0) == 0) { const auto rest = p.substr(5); const auto c = rest.find(':'); const std::string ln = rest.substr(0, c);
            if (ln != line) return false; if (c == std::string::npos) return true; const auto t = rest.find("tier>="); return t == std::string::npos || tier >= std::atoi(rest.c_str() + t + 6); }
        return p == cls;
    }
    inline bool RulingPass(const Ruling& r, double v) { const std::string o = r.op; return o == "<=" ? v <= r.target : o == ">=" ? v >= r.target : o == "<" ? v < r.target : v > r.target; }

    struct GapRow { const char* id; const char* family; const char* cls; const char* desc; const char* scenario; const char* status; };
    inline const std::vector<GapRow>& GapRows() {   // the doc's GAP LEDGER; delta_u is filled from the scenario of the same run when it ran
        static const std::vector<GapRow> g = {
            { "G1", "taunt", "knight line", "no survivability or sacrifice term in the taunt", "hurt-knight", "open" },
            { "G1b", "taunt", "knight line", "taunt against a far stronger foe (no healer / with a healer)", "knight-boss", "open" },
            { "G2", "retreat / movement", "*", "does not back off when outmatched", "kite", "open" },
            { "G3", "heal", "healers", "reactive, oversized heals", "predictive-heal", "open" },
            { "G4", "heal", "healers", "two followers heal the same target", "double-heal", "open" },
            { "G5", "cast offense", "casters", "no overkill term", "overkill", "open" },
            { "G6", "casts", "casters", "no magicka budget across the fight", "mp-budget", "open" },
            { "G7", "attack target", "*", "no value for killing the healer or archer first", "focus-healer", "open" },
            { "G8", "attack", "mage lines", "mage lines melee more than the ruling allows", "", "open" },
            { "G9", "attack target", "*", "target switches far above the field rate", "", "open" },
            { "G10", "cast control", "casters", "a refused (spell, target) pair came back every second lap", "", "closing" },
            { "G11", "cast offense", "casters", "area casts that reach an ally", "splash", "open" },
            { "G12", "power attack / bash", "melee lines", "a stagger is not used to interrupt a caster", "interrupt", "open" },
            { "G13", "control + attack", "*", "attacks do not follow an ally's control", "control-setup", "open" },
            { "G14", "attack target", "*", "the foe on the player is not handled first", "peel", "open" },
        };
        return g;
    }

    inline std::string RunId(const std::string& list) {
        std::time_t t = std::time(nullptr); std::tm tm{}; localtime_r(&t, &tm); char b[40]; std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
        char r[16]; std::snprintf(r, sizeof r, "-%04x", static_cast<unsigned>(std::clock() ^ static_cast<std::clock_t>(t * 2654435761u)) & 0xffffu); return std::string(b) + "-" + list + r;
    }
    inline std::string IsoNow() { std::time_t t = std::time(nullptr); std::tm tm{}; localtime_r(&t, &tm); char b[40]; std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &tm); return b; }

    // My metric names -> the schema's fixed keys (the raw names are written too).
    inline std::map<std::string, double> SchemaMetrics(const std::map<std::string, double>& m) {
        std::map<std::string, double> o = m;
        auto put = [&](const char* key, const char* mine) { auto i = m.find(mine); if (i != m.end()) o[key] = i->second; };
        put("idle_pct", "idle%"); put("idle_cand_pct", "iC%"); put("hold_pct", "hold%"); put("hold_low_pct", "holdLow%"); put("switches_per_min", "sw/min"); put("target_switch_per_min", "tsw/min");
        put("heal_latency_s", "heal.s"); put("healed_pct", "healed%"); put("overheal_pct", "overheal%"); put("double_heal_pct", "dblheal%"); put("overkill_pct", "overkill%"); put("mp_at_boss_pct", "mpAtLate%");
        put("melee_share_pct", "b.melee"); put("taunt_per_min", "taunt/min"); put("power_per_min", "pow/min"); put("follower_death_pct", "down%"); put("player_death_pct", "pdead%"); put("ttk_s", "ttfk.s");
        put("damage_taken_per_foe", "takenPerFoe"); put("bad_facts", "bad"); put("invalid", "invalid"); put("decide_us_mean", "dec.us"); put("decide_us_max", "dec.max");
        put("splash_ally_hits_per_fight", "splashHits/f"); put("utility", "U"); put("win_pct", "win%"); put("fight_s", "dur.s");
        return o;
    }

    struct ResultRow { std::string cls, line, bracket, rank = "all"; int tier = 1; bool planned = false; long fights = 0; std::map<std::string, double> metrics; bool hasGap = false; double mainGap = 0; };

    // Which way is better, and the unit, for every metric the dashboard may colour.
    struct MetricInfo { const char* name; const char* better; const char* unit; };
    inline const std::vector<MetricInfo>& MetricInfos() {
        static const std::vector<MetricInfo> v = {
            { "idle_pct", "neutral", "%" }, { "idle_cand_pct", "lower", "%" }, { "hold_pct", "neutral", "%" }, { "hold_low_pct", "lower", "%" }, { "switches_per_min", "lower", "/min" },
            { "target_switch_per_min", "lower", "/min" }, { "heal_latency_s", "lower", "s" }, { "healed_pct", "higher", "%" }, { "overheal_pct", "lower", "%" }, { "double_heal_pct", "lower", "%" },
            { "overkill_pct", "lower", "%" }, { "mp_at_boss_pct", "higher", "%" }, { "melee_share_pct", "neutral", "%" }, { "taunt_per_min", "neutral", "/min" }, { "power_per_min", "neutral", "/min" },
            { "follower_death_pct", "lower", "%" }, { "player_death_pct", "lower", "%" }, { "ttk_s", "lower", "s" }, { "damage_taken_per_foe", "lower", "" }, { "bad_facts", "lower", "" }, { "invalid", "lower", "" },
            { "decide_us_mean", "lower", "us" }, { "decide_us_max", "lower", "us" }, { "utility", "higher", "" }, { "win_pct", "higher", "%" }, { "fight_s", "lower", "s" },
            { "splash_ally_hits_per_fight", "lower", "" }, { "main_gap", "lower", "" }, { "gap", "lower", "" }, { "iaus_u", "higher", "" }, { "oracle_u", "higher", "" },
            { "intr%", "higher", "%" }, { "resisted%", "lower", "%" }, { "immune%", "lower", "%" }, { "weak%", "higher", "%" }, { "splash%", "lower", "%" }, { "ctrlFail%", "lower", "%" },
            { "peel.s", "lower", "s" }, { "press%", "lower", "%" }, { "prio%", "higher", "%" }, { "waste%", "lower", "%" }, { "dwh/f", "lower", "" }, { "wwDecl/min", "lower", "/min" },
            { "dry%", "lower", "%" }, { "downs/f", "lower", "" }, { "pMinHp%", "higher", "%" }, { "ctlFocus%", "higher", "%" }, { "taken/s", "lower", "" }, { "pTaken/s", "lower", "" },
            { "dps", "higher", "" }, { "loss%", "lower", "%" }, { "timeout%", "lower", "%" }, { "tntDown%", "lower", "%" }, { "b.cast", "neutral", "%" }, { "b.shoot", "neutral", "%" }, { "b.free", "neutral", "%" },
        };
        return v;
    }
    struct ScenarioRow { std::string name, cls; double iausU = 0, oracleU = 0, gap = 0; bool hasOracle = false; std::map<std::string, double> metrics, oracleMetrics; };

    struct Results {
        std::string runId, list, label, oracleKind = "none"; std::uint64_t seed = 1; int fights = 0, oracleK = 0; double oracleH = 0;
        std::vector<ResultRow> rows; std::vector<ScenarioRow> scen;
        void Write(std::string path) const {
            namespace fs = std::filesystem;
            const std::string id = !runId.empty() ? runId : RunId(list);
            if (path.empty()) { const char* h = std::getenv("HOME"); path = std::string(h ? h : ".") + "/.cache/mfo-iaus-sim/runs/" + id + ".json"; }
            std::error_code ec; fs::create_directories(fs::path(path).parent_path(), ec);
            const char* br = std::getenv("SIM_BRANCH"); const char* sha = std::getenv("SIM_SHA");
            std::string j = "{\"schema\":1,\"run_id\":" + JsonOut::Str(id) + ",\"time\":" + JsonOut::Str(IsoNow()) + ",\"branch\":" + JsonOut::Str(br ? br : "") + ",\"sha\":" + JsonOut::Str(sha ? sha : "")
                          + ",\"list\":" + JsonOut::Str(list) + ",\"seed\":" + std::to_string(seed) + ",\"fights_per_cell\":" + std::to_string(fights) + ",\"label\":" + JsonOut::Str(label)
                          + ",\"oracle\":{\"k\":" + std::to_string(oracleK) + ",\"horizon_s\":" + JsonOut::Num(oracleH) + ",\"kind\":" + JsonOut::Str(oracleKind) + "},\n\"classes\":[";
            for (std::size_t i = 0; i < rows.size(); ++i) { const ResultRow& r = rows[i]; std::map<std::string, double> mix, met;
                for (const auto& kv : SchemaMetrics(r.metrics)) { if (kv.first.rfind("a.", 0) == 0) mix[kv.first.substr(2)] = kv.second; else met[kv.first] = kv.second; }
                j += std::string(i ? ",\n" : "\n") + "{\"class\":" + JsonOut::Str(r.cls) + ",\"tier\":" + std::to_string(r.tier) + ",\"line\":" + JsonOut::Str(r.line) + ",\"planned\":" + (r.planned ? "true" : "false")
                   + ",\"bracket\":" + JsonOut::Str(r.bracket) + ",\"rank\":" + JsonOut::Str(r.rank) + ",\"fights\":" + std::to_string(r.fights) + ",\"main_gap\":" + (r.hasGap ? JsonOut::Num(r.mainGap) : "null") + ",\"metrics\":" + JsonOut::Map(met) + ",\"action_mix\":" + JsonOut::Map(mix) + "}"; }
            j += "],\n\"scenarios\":[";
            for (std::size_t i = 0; i < scen.size(); ++i) { const ScenarioRow& s = scen[i];
                j += std::string(i ? ",\n" : "\n") + "{\"name\":" + JsonOut::Str(s.name) + ",\"class\":" + JsonOut::Str(s.cls) + ",\"iaus_u\":" + JsonOut::Num(s.iausU) + ",\"oracle_u\":" + (s.hasOracle ? JsonOut::Num(s.oracleU) : "null")
                   + ",\"gap\":" + (s.hasOracle ? JsonOut::Num(s.gap) : "null") + ",\"metrics\":" + JsonOut::Map(SchemaMetrics(s.metrics)) + ",\"oracle_metrics\":" + JsonOut::Map(SchemaMetrics(s.oracleMetrics)) + "}"; }
            j += "],\n\"rulings\":["; bool first = true;
            for (const Ruling& r : Rulings()) for (const ResultRow& row : rows) {
                if (row.bracket != "natural" || !RulingApplies(r, row.cls, row.line, row.tier)) continue;
                const auto sm = SchemaMetrics(row.metrics); auto it = sm.find(r.metric); if (it == sm.end() || it->second < 0) continue;
                j += std::string(first ? "\n" : ",\n") + "{\"id\":" + JsonOut::Str(r.id) + ",\"text\":" + JsonOut::Str(r.text) + ",\"class\":" + JsonOut::Str(row.cls) + ",\"metric\":" + JsonOut::Str(r.metric) + ",\"op\":" + JsonOut::Str(r.op)
                   + ",\"target\":" + JsonOut::Num(r.target) + ",\"value\":" + JsonOut::Num(it->second) + ",\"pass\":" + (RulingPass(r, it->second) ? "true" : "false") + "}"; first = false; }
            j += "],\n\"gaps\":[";
            for (std::size_t i = 0; i < GapRows().size(); ++i) { const GapRow& g = GapRows()[i]; double dU = 0; bool has = false;
                for (const ScenarioRow& s : scen) if (s.hasOracle && s.name == g.scenario) { dU = s.gap; has = true; }
                j += std::string(i ? ",\n" : "\n") + "{\"id\":" + JsonOut::Str(g.id) + ",\"family\":" + JsonOut::Str(g.family) + ",\"class\":" + JsonOut::Str(g.cls) + ",\"desc\":" + JsonOut::Str(g.desc) + ",\"scenario\":" + JsonOut::Str(g.scenario)
                   + ",\"delta_u\":" + (has ? JsonOut::Num(dU) : "null") + ",\"status\":" + JsonOut::Str(g.status) + "}"; }
            j += "],\n\"metric_info\":{";
            for (std::size_t i = 0; i < MetricInfos().size(); ++i) { const MetricInfo& mi = MetricInfos()[i]; j += std::string(i ? "," : "") + JsonOut::Str(mi.name) + ":{\"better\":" + JsonOut::Str(mi.better) + ",\"unit\":" + JsonOut::Str(mi.unit) + "}"; }
            j += "}}\n";
            const std::string tmp = path + ".tmp";   // atomic: the dashboard never reads a half-written file
            if (std::FILE* f = std::fopen(tmp.c_str(), "w")) { std::fwrite(j.data(), 1, j.size(), f); std::fclose(f); fs::rename(tmp, path, ec);
                if (ec) std::fprintf(stderr, "cannot rename %s: %s\n", tmp.c_str(), ec.message().c_str()); else std::printf("results: %s\n", path.c_str()); }
            else std::fprintf(stderr, "cannot write %s\n", tmp.c_str());
        }
    };

}
