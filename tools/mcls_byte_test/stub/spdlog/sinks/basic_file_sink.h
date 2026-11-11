#pragma once
// tools/mcls_byte_test/stub/spdlog -- HOST STAND-IN for spdlog: every line is formatted with std::format and kept in
// a list the test inspects (and prints with MCLS_TEST_VERBOSE=1). See stub/RE/Skyrim.h.
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace spdlog {
    struct Line {
        char        level;   // 'I' info, 'W' warn, 'E' error
        std::string text;
    };
    inline std::vector<Line>& Lines() {
        static std::vector<Line> s;
        return s;
    }
    template <class... A>
    void info(std::format_string<A...> a_f, A&&... a_a) { Lines().push_back({ 'I', std::format(a_f, std::forward<A>(a_a)...) }); }
    template <class... A>
    void warn(std::format_string<A...> a_f, A&&... a_a) { Lines().push_back({ 'W', std::format(a_f, std::forward<A>(a_a)...) }); }
    template <class... A>
    void error(std::format_string<A...> a_f, A&&... a_a) { Lines().push_back({ 'E', std::format(a_f, std::forward<A>(a_a)...) }); }
}
