#include "PCH.h"
#include "i18n/Strings.h"

#include <cstdio>
#include <cstring>

// i18n/Strings.cpp -- the immutable text table, its loader and the safe
// formatter. See Strings.h for the threading + precedence contract.

namespace MFO::Str {

    namespace {

        struct Default { const char* name; const char* en; int argc; };
        constexpr Default kDefaults[] = {
#define MFO_STR(id, en, argc) { #id, en, argc },
#include "i18n/Strings_keys.h"
#undef MFO_STR
        };
        static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) == static_cast<std::size_t>(K::Count_),
                      "kDefaults and K must come from the same X-macro list");

        // THE GAME-TERM BRIDGE. A key listed here, when MFO_<LANGUAGE>.txt has no
        // entry for it, borrows the game's own localized word: each candidate is
        // tried in order, a "$Key" via the Scaleform translator (the game's
        // Interface/Translations/Skyrim_<LANG>.txt table) or "gmst:sName" (a Game
        // Setting string, localized by the STRINGS files). English is exempt: the
        // compiled English defaults are MFO's own, tuned for the layout, so an
        // English game never changes spelling or case. argc must be 0.
        // Every resolution is logged once at Reload ([i18n] game term ...), so a
        // wrong candidate shows in MFO.log and is fixed by editing this list.
        struct GameTerm { K key; const char* cand[3]; };
        constexpr GameTerm kGameTerms[] = {
            { K::Word_Health,  { "$Health",  "gmst:sHealth",  nullptr } },
            { K::Word_Magicka, { "$Magicka", "gmst:sMagicka", nullptr } },
            { K::Word_Stamina, { "$Stamina", "gmst:sStamina", nullptr } },
            { K::Pg_Cancel,    { "$Cancel",  nullptr,         nullptr } },
        };

        enum class Src : std::uint8_t { English, Mfo, Game };

        struct Seg { std::string lit; int idx = 0; };   // idx 0 = literal only

        struct Entry {
            std::string      text;     // the active text (raw template)
            std::vector<Seg> segs;     // parsed for Fmt
            Src              src = Src::English;
        };

        struct Table {
            std::vector<Entry> e;
            bool               overrides = false;
        };

        std::atomic<const Table*> g_table{ nullptr };
        std::atomic<bool>         g_hasOverrides{ false };

        // Parse "{N}" tokens. Returns false on ANY other brace, on N == 0 or
        // N > argc. Literal text keeps '%' and everything else verbatim.
        bool Parse(const std::string& s, int argc, std::vector<Seg>& out) {
            out.clear();
            std::string lit;
            for (std::size_t i = 0; i < s.size(); ++i) {
                const char c = s[i];
                if (c == '}') return false;
                if (c != '{') { lit.push_back(c); continue; }
                std::size_t j = i + 1;
                int n = 0, digits = 0;
                while (j < s.size() && s[j] >= '0' && s[j] <= '9' && digits < 3) {
                    n = n * 10 + (s[j] - '0'); ++j; ++digits;
                }
                if (digits == 0 || j >= s.size() || s[j] != '}') return false;
                if (n < 1 || n > argc) return false;
                out.push_back({ std::move(lit), n });
                lit.clear();
                i = j;
            }
            out.push_back({ std::move(lit), 0 });
            return true;
        }

        // The maximum {N} a compiled English default uses (self-check that the
        // macro's argc matches the template). Uses the same parser.
        int MaxIndex(const std::vector<Seg>& segs) {
            int m = 0;
            for (const auto& s : segs) m = std::max(m, s.idx);
            return m;
        }

        // MAIN THREAD. Is the Scaleform translator up? (Same three lookups the
        // SKSE wrapper makes; checked first so a not-yet-ready translator is ONE
        // log line instead of one warning per key.) The AddRef the lookup takes
        // is left, exactly as the SKSE wrapper leaves it: a singleton state.
        bool TranslatorUp() {
            const auto mgr    = RE::BSScaleformManager::GetSingleton();
            const auto loader = mgr ? mgr->loader : nullptr;
            const auto tr     = loader ? loader->GetStateAddRef<RE::GFxTranslator>(RE::GFxState::StateType::kTranslator) : nullptr;
            return skyrim_cast<RE::BSScaleformTranslator*>(tr) != nullptr;
        }

        // MAIN THREAD. The Scaleform translator lookup, the wrapper SKSE ships.
        // false = key absent (an unknown key either leaves the result empty or
        // echoes the key itself; both count as absent).
        bool Translate(const std::string& a_key, std::string& a_out) {
            std::string r;
            if (!SKSE::Translation::Translate(a_key, r)) return false;
            if (r.empty() || r == a_key) return false;
            a_out = std::move(r);
            return true;
        }

        // The file stores `\n` for a line break (a translation file is one entry per
        // line). Only `\n` and `\\` are escapes; anything else keeps its backslash.
        std::string Unescape(const std::string& s) {
            std::string o;
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    if (s[i + 1] == 'n')  { o.push_back('\n'); ++i; continue; }
                    if (s[i + 1] == '\\') { o.push_back('\\'); ++i; continue; }
                }
                o.push_back(s[i]);
            }
            return o;
        }

        std::string GmstString(const char* a_name) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* s   = gsc ? gsc->GetSetting(a_name) : nullptr;
            if (!s || s->GetType() != RE::Setting::Type::kString) return {};
            const char* t = s->GetString();
            return t ? t : "";
        }

        std::string Language() {
            auto* ini = RE::INISettingCollection::GetSingleton();
            auto* s   = ini ? ini->GetSetting("sLanguage:General") : nullptr;
            if (s && s->GetType() == RE::Setting::Type::kString && s->data.s && *s->data.s)
                return s->data.s;
            return "ENGLISH";
        }

        // Fill a table from the translator. Returns the count of MFO entries used.
        // `rejected` collects keys whose override failed validation.
        int Fill(Table& t, bool a_english, std::vector<std::string>& rejected) {
            int hits = 0;
            if (!TranslatorUp()) {
                spdlog::warn("[i18n] the Scaleform translator is not up yet: English defaults for now");
                return 0;
            }
            for (std::size_t i = 0; i < t.e.size(); ++i) {
                const auto& d = kDefaults[i];
                std::string raw;
                if (!Translate(std::string("$MFO_") + d.name, raw)) continue;
                raw = Unescape(raw);
                std::vector<Seg> segs;
                if (!Parse(raw, d.argc, segs)) { rejected.emplace_back(d.name); continue; }
                t.e[i].text = std::move(raw);
                t.e[i].segs = std::move(segs);
                t.e[i].src  = Src::Mfo;
                ++hits;
            }
            if (!a_english) {
                for (const auto& g : kGameTerms) {
                    auto& en = t.e[static_cast<std::size_t>(g.key)];
                    if (en.src == Src::Mfo) continue;
                    for (const char* c : g.cand) {
                        if (!c) break;
                        std::string v;
                        if (std::strncmp(c, "gmst:", 5) == 0) v = GmstString(c + 5);
                        else if (!Translate(c, v)) v.clear();
                        std::vector<Seg> segs;
                        if (v.empty() || !Parse(v, 0, segs)) {
                            spdlog::info("[i18n] game term {} <- {}: not found", kDefaults[static_cast<std::size_t>(g.key)].name, c);
                            continue;
                        }
                        spdlog::info("[i18n] game term {} <- {}: '{}'", kDefaults[static_cast<std::size_t>(g.key)].name, c, v);
                        en.text = std::move(v);
                        en.segs = std::move(segs);
                        en.src  = Src::Game;
                        break;
                    }
                }
            }
            return hits;
        }

        const Table* Build(bool a_english, int& a_hits, std::vector<std::string>& a_rejected) {
            auto* t = new Table();
            t->e.resize(static_cast<std::size_t>(K::Count_));
            for (std::size_t i = 0; i < t->e.size(); ++i) {
                t->e[i].text = kDefaults[i].en;
                if (!Parse(t->e[i].text, kDefaults[i].argc, t->e[i].segs)) {
                    // A broken COMPILED default is a programming error. Loud, and
                    // the entry renders its raw text with no substitution.
                    spdlog::error("[i18n] compiled default {} has a bad placeholder: '{}'",
                                  kDefaults[i].name, kDefaults[i].en);
                    t->e[i].segs.assign(1, Seg{ t->e[i].text, 0 });
                } else if (MaxIndex(t->e[i].segs) != kDefaults[i].argc) {
                    spdlog::warn("[i18n] default {} declares argc={} but uses up to {{{}}}",
                                 kDefaults[i].name, kDefaults[i].argc, MaxIndex(t->e[i].segs));
                }
            }
            a_hits = Fill(*t, a_english, a_rejected);
            t->overrides = a_hits > 0;
            return t;
        }

    }   // namespace

    Arg::Arg(int v) : s(std::to_string(v)) {}
    Arg::Arg(unsigned v) : s(std::to_string(v)) {}
    Arg::Arg(long v) : s(std::to_string(v)) {}
    Arg::Arg(unsigned long v) : s(std::to_string(v)) {}
    Arg::Arg(long long v) : s(std::to_string(v)) {}
    Arg::Arg(unsigned long long v) : s(std::to_string(v)) {}
    Arg Arg::F(double v, int decimals) {
        char buf[64];
        if (decimals < 0) std::snprintf(buf, sizeof buf, "%g", v);
        else              std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        return Arg(static_cast<const char*>(buf));
    }

    const char* Get(K k) {
        const auto i = static_cast<std::size_t>(k);
        if (i >= static_cast<std::size_t>(K::Count_)) return "";
        if (const Table* t = g_table.load(std::memory_order_acquire)) return t->e[i].text.c_str();
        return kDefaults[i].en;
    }

    std::string Fmt(K k, std::initializer_list<Arg> args) {
        const auto i = static_cast<std::size_t>(k);
        if (i >= static_cast<std::size_t>(K::Count_)) return {};
        const Table* t = g_table.load(std::memory_order_acquire);
        if (!t) {
            // Before the first Reload: parse the English default on the spot.
            std::vector<Seg> segs;
            if (!Parse(kDefaults[i].en, kDefaults[i].argc, segs)) return kDefaults[i].en;
            std::string o;
            for (const auto& s : segs) {
                o += s.lit;
                if (s.idx >= 1 && static_cast<std::size_t>(s.idx) <= args.size())
                    o += (args.begin() + (s.idx - 1))->s;
            }
            return o;
        }
        std::string o;
        for (const auto& s : t->e[i].segs) {
            o += s.lit;
            if (s.idx >= 1 && static_cast<std::size_t>(s.idx) <= args.size())
                o += (args.begin() + (s.idx - 1))->s;
        }
        return o;
    }

    const char* Label(K k, const char* idSuffix) {
        thread_local std::string ring[8];
        thread_local unsigned    n = 0;
        std::string& s = ring[n++ & 7u];
        s = Get(k);
        s += "###";
        s += idSuffix ? idSuffix : "";
        return s.c_str();
    }

    bool HasOverrides() { return g_hasOverrides.load(std::memory_order_acquire); }

    void Reload(const char* why) {
        const std::string lang = Language();
        const bool english = _stricmp(lang.c_str(), "ENGLISH") == 0;
        int hits = 0;
        std::vector<std::string> rejected;
        const Table* t = Build(english, hits, rejected);
        if (hits == 0) {
            // Nothing in the translator yet. The engine loads every
            // Interface/Translations/*_<LANG>.txt itself, but only when its UI
            // starts; if MFO_<LANG>.txt is not there, ask SKSE to parse it now.
            // ParseTranslation never overwrites an existing key, so this is safe
            // when the engine already has the file, and it is a quiet no-op when
            // the file does not exist (the English-defaults case).
            SKSE::Translation::ParseTranslation("MFO");
            delete t;   // never published, so it is safe to free
            rejected.clear();
            t = Build(english, hits, rejected);
        }
        // English game: the shipped MFO_ENGLISH.txt is read back by the engine's own
        // parser. Prove it reproduces the compiled defaults byte for byte, per key.
        int mismatches = 0;
        if (english && hits > 0) {
            for (std::size_t i = 0; i < t->e.size(); ++i) {
                if (t->e[i].src == Src::Mfo && t->e[i].text != kDefaults[i].en) {
                    ++mismatches;
                    spdlog::warn("[i18n] english mismatch {}: loaded '{}' vs compiled '{}'",
                                 kDefaults[i].name, t->e[i].text, kDefaults[i].en);
                }
            }
            spdlog::info("[i18n] english check: mfoOverrides={} mismatches={}", hits, mismatches);
        }
        g_table.store(t, std::memory_order_release);   // the old table (if any) is leaked ON PURPOSE
        g_hasOverrides.store(hits > 0, std::memory_order_release);
        spdlog::info("[i18n] reload ({}): language={} keys={} mfoOverrides={} rejected={} "
                     "(MFO_{}.txt {})",
                     why ? why : "?", lang, KeyCount(), hits, rejected.size(), lang,
                     hits > 0 ? "in use" : "absent or empty: English defaults");
        for (const auto& r : rejected)
            spdlog::warn("[i18n] MFO_{}.txt: $MFO_{} dropped (bad or out-of-range {{N}} placeholder), "
                         "using the English default", lang, r);
    }
}
