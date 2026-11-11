#pragma once
// iaus_sim/Json.h -- a tiny JSON reader (objects, arrays, numbers, strings, true / false / null). HOST-ONLY.
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace Sim {

    struct J {
        enum T { Nul, Num, Str, Arr, Obj, Bool } t = Nul;
        double n = 0;
        std::string s;
        std::vector<J> a;
        std::vector<std::pair<std::string, J>> o;
        const J& operator[](const char* k) const {
            static const J nul;
            for (const auto& kv : o)
                if (kv.first == k) return kv.second;
            return nul;
        }
        const J& operator[](std::size_t i) const { static const J nul; return i < a.size() ? a[i] : nul; }
        const J& operator[](int i) const { return (*this)[static_cast<std::size_t>(i)]; }
        double num(double d = 0) const { return t == Num || t == Bool ? n : d; }
        bool   yes() const { return (t == Bool || t == Num) && n != 0; }
        bool   has() const { return t != Nul; }
    };

    struct JP {
        const std::string& s;
        std::size_t p = 0;
        void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
        J val() {
            ws(); J j;
            if (s[p] == '{') { j.t = J::Obj; ++p; ws(); if (s[p] == '}') { ++p; return j; }
                for (;;) { ws(); std::string k = str(); ws(); ++p; j.o.emplace_back(std::move(k), val()); ws(); if (s[p++] == '}') break; } }
            else if (s[p] == '[') { j.t = J::Arr; ++p; ws(); if (s[p] == ']') { ++p; return j; }
                for (;;) { j.a.push_back(val()); ws(); if (s[p++] == ']') break; } }
            else if (s[p] == '"') { j.t = J::Str; j.s = str(); }
            else if (!s.compare(p, 4, "true")) { j.t = J::Bool; j.n = 1; p += 4; }
            else if (!s.compare(p, 5, "false")) { j.t = J::Bool; p += 5; }
            else if (!s.compare(p, 4, "null")) { p += 4; }
            else if (!s.compare(p, 3, "NaN")) { p += 3; }
            else { j.t = J::Num; char* e; j.n = std::strtod(s.c_str() + p, &e); p = static_cast<std::size_t>(e - s.c_str()); }
            return j;
        }
        std::string str() { std::string r; ++p; while (s[p] != '"') { if (s[p] == '\\') ++p; r += s[p++]; } ++p; return r; }
    };

}
