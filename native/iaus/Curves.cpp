// iaus/Curves.cpp -- the curve library (contract in iaus/Core.h). PURE: no engine header is used here.
#include "iaus/Core.h"

#include <algorithm>
#include <cmath>

namespace MFO::Iaus {

    namespace {
        float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
    }

    float Eval(const Curve& c, float x) {
        if (std::isnan(x)) return 0.0f;                          // a NaN input never scores
        if (x > c.zeroAbove || x < c.zeroBelow) return 0.0f;     // the Step veto
        float y = 0.0f;
        switch (c.kind) {
        case CurveKind::Lin:  y = c.a * x + c.b; break;
        case CurveKind::Pow:  y = std::pow(Clamp01(x), c.a); break;
        case CurveKind::Logi: y = 1.0f / (1.0f + std::exp(-c.a * (x - c.b))); break;
        case CurveKind::Inv:  y = 1.0f - std::pow(Clamp01(x), c.a); break;
        case CurveKind::Bin:  y = x >= 0.5f ? c.b : c.a; break;
        case CurveKind::Piece: {
            if (c.np == 0) { y = 0.0f; break; }
            if (x <= c.px[0]) { y = c.py[0]; break; }
            const std::size_t last = static_cast<std::size_t>(c.np) - 1;
            if (x >= c.px[last]) { y = c.py[last]; break; }
            for (std::size_t i = 1; i <= last; ++i) {
                if (x <= c.px[i]) {
                    const float span = c.px[i] - c.px[i - 1];
                    const float t = span > 0.0f ? (x - c.px[i - 1]) / span : 1.0f;
                    y = c.py[i - 1] + t * (c.py[i] - c.py[i - 1]);
                    break;
                }
            }
            break;
        }
        }
        if (std::isnan(y)) return 0.0f;
        return (std::max)(y, c.floor);
    }

    Curve Lin(float m, float b, float floor) { Curve c; c.kind = CurveKind::Lin; c.a = m; c.b = b; c.floor = floor; return c; }
    Curve Pow(float k, float floor) { Curve c; c.kind = CurveKind::Pow; c.a = k; c.floor = floor; return c; }
    Curve Logi(float k, float x0, float floor) { Curve c; c.kind = CurveKind::Logi; c.a = k; c.b = x0; c.floor = floor; return c; }
    Curve Inv(float k, float floor) { Curve c; c.kind = CurveKind::Inv; c.a = k; c.floor = floor; return c; }
    Curve Bin(float lo, float hi) { Curve c; c.kind = CurveKind::Bin; c.a = lo; c.b = hi; return c; }

    Curve Piece(std::span<const std::array<float, 2>> pts) {
        Curve c;
        c.kind = CurveKind::Piece;
        const std::size_t n = (std::min)(pts.size(), c.px.size());
        for (std::size_t i = 0; i < n; ++i) { c.px[i] = pts[i][0]; c.py[i] = pts[i][1]; }
        c.np = static_cast<std::uint8_t>(n);
        return c;
    }

    Curve VetoAbove(Curve c, float t) { c.zeroAbove = t; return c; }
    Curve VetoBelow(Curve c, float t) { c.zeroBelow = t; return c; }

}
