#pragma once
// i18n/Strings.h -- the PUBLIC face of MFO's display-text table (ClickUp
// 86e3gmxmg). Every player-facing ImGui string is a stable key with an English
// default compiled in (Strings_keys.h). A translator ships
// Data/Interface/Translations/MFO_<LANGUAGE>.txt (UTF-16 LE with BOM,
// `$MFO_<Key><TAB>text`, the MCM Helper / Skyrim convention) and never touches
// the DLL. Docs/TRANSLATING.md is the guide.
//
// PRECEDENCE per key (first hit wins): MFO_<LANGUAGE>.txt entry  >  the game's
// own localized term (a Skyrim_<LANG>.txt $key or a GMST, only for keys that
// declare one in kGameTerms, Strings.cpp)  >  the English default in code.
//
// THREADING (the point of the file split, CLAUDE.md #4):
//   * Reload() is MAIN THREAD ONLY. It is the ONLY place that touches engine
//     state (the Scaleform translator, GMSTs, sLanguage). It builds a complete
//     immutable table and publishes it with ONE release store.
//   * Get/Fmt/Label are LOCK-FREE and safe from ANY thread (render, worker,
//     main): one acquire load of the published pointer, then plain reads of
//     immutable data. A published table is NEVER freed or mutated (a later
//     Reload publishes a new one and leaks the old, a few KB, so a pointer a
//     render frame still holds can never dangle).
//   * Before the first Reload() every lookup answers the English default.
//
// FORMATTING: placeholders are `{1}`, `{2}` ... (1-based, reorderable by a
// translator). A string is NEVER fed to printf/std::format as the format.
// Reload() validates each override against the key's declared argument count
// (only `{N}` with 1<=N<=argc is legal, no other brace) and DROPS a bad one,
// loudly, back to the English default. Fmt() therefore cannot misformat at
// render time. Argument "type" is the caller's: Arg formats ints and floats
// itself (Arg::F) so a translation can never ask for a different conversion.
#include <cstdint>
#include <initializer_list>
#include <string>

namespace MFO::Str {

    enum class K : std::uint16_t {
#define MFO_STR(id, en, argc) id,
#include "i18n/Strings_keys.h"
#undef MFO_STR
        Count_
    };

    // One formatted argument. Implicit from integers and strings; floats go
    // through Arg::F(value, decimals) (decimals < 0 means %g).
    struct Arg {
        std::string s;
        Arg(int v);
        Arg(unsigned v);
        Arg(long v);
        Arg(unsigned long v);
        Arg(long long v);
        Arg(unsigned long long v);
        Arg(const char* v) : s(v ? v : "") {}
        Arg(const std::string& v) : s(v) {}
        static Arg F(double v, int decimals);
    };

    // The text for a key (translated or default). Never null. The pointer is
    // stable for the life of the process. For a key with argc > 0 this is the
    // RAW template (with its {N} tokens): use Fmt() to render those.
    const char* Get(K k);

    // Render a key's template with args. Extra args are ignored, a missing
    // arg renders empty. Safe by construction (see the header comment).
    std::string Fmt(K k, std::initializer_list<Arg> args);

    // "text###suffix": the translated text as an ImGui label with a stable ID
    // (so a language change or two keys translated to the same word can never
    // collide or shift an ImGui ID, which controller navigation depends on).
    // Returns a thread_local ring buffer valid until 8 further Label() calls
    // on the same thread, i.e. long enough for the one ImGui call it feeds.
    const char* Label(K k, const char* idSuffix);

    // MAIN THREAD ONLY. (Re)build and publish the table. Idempotent. Call at
    // kDataLoaded, and again at kPostLoadGame/kNewGame while HasOverrides() is
    // false (the Scaleform translator may not hold MFO's file that early).
    void Reload(const char* why);

    // True once the last Reload found at least one MFO_<LANGUAGE>.txt entry.
    bool HasOverrides();

    // The number of keys (for the self-test log).
    constexpr std::size_t KeyCount() { return static_cast<std::size_t>(K::Count_); }
}
