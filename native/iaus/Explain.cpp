// iaus/Explain.cpp -- the explain record (design section 6): the [iaus] decision line, the [iaus-why] per-candidate
// breakdown, the log pacing, and the board readout. PURE (iaus/Core.h): it builds strings, Engine.cpp logs them.
#include "iaus/Iaus_internal.h"

#include <cmath>
#include <format>

namespace MFO::Iaus {

    namespace {
        constexpr std::string_view kRoman[] = { "I", "II", "III", "IV", "V" };

        std::string FormRef(std::string_view name, std::uint32_t id) {
            if (name.empty()) return std::format("{:08X}", id);
            return std::format("{}({:08X})", name, id);
        }

        std::string Num(float v) {
            if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
            if (std::abs(v) >= 10.0f) return std::format("{:.0f}", v);
            return std::format("{:.2f}", v);
        }
    }

    std::string Label(const Candidate& c) {
        std::string s;
        if (c.family == Family::Posture) {
            s = std::format("equip {}", Name(c.f.posture));
            if (c.f.postureCurrent) s += " (keep)";
            return s;
        }
        s = std::string(Name(c.family));
        if (c.form != 0) { s += ' '; s += FormRef(c.formName, c.form); }
        if (c.target != 0) { s += " -> "; s += FormRef(c.targetName, c.target); }
        return s;
    }

    std::string FormatDecision(const Decision& d, std::span<const Candidate> cands, const Tuning& t, int pickedStep,
                               std::span<const WalkNote> walk, double ms) {
        std::string s;
        const std::string_view rank = kRoman[detail::RankRow(d.rapportRank)];
        auto scoreOf = [&](std::uint16_t idx) {
            for (const auto& r : d.ranked)
                if (r.idx == idx) return r;
            return Scored{ idx, 0.0f, 0.0f, 1.0f, false };
        };

        if (d.verdict == Verdict::Idle) {
            s = std::format("idle: nothing above the floor {:.2f}", t.actFloor);
        } else {
            const bool fired = pickedStep >= 0 && static_cast<std::size_t>(pickedStep) < d.order.size();
            const Step& head = fired ? d.order[static_cast<std::size_t>(pickedStep)] : d.order.front();
            const Scored hs = scoreOf(head.idx);
            if (d.verdict == Verdict::Hold) s = "hold=";
            else if (fired) s = "pick=";
            else s = "none fired, first=";
            s += std::format("{} {:.3f}", Label(cands[head.idx]), hs.score);
            int shown = 1;
            for (const auto& r : d.ranked) {
                if (shown >= 3) break;
                if (r.idx == head.idx) continue;
                ++shown;
                s += std::format(" | {} {} {:.3f}", shown, Label(cands[r.idx]), r.score);
            }
            s += std::format(" | n={}/{} r={}", d.evaluated, d.generated, rank);
            s += d.rolled ? std::format(" roll={:.2f}", d.roll) : std::string(" roll=-");
            s += hs.running ? std::format(" mom=+{:.0f}%", (t.momentum - 1.0f) * 100.0f) : std::string(" mom=0");
            if (hs.focus > 1.0f) s += std::format(" pf=x{:.2f}", hs.focus);
            s += std::format(" walk={}", fired ? pickedStep + 1 : 0);
            if (!walk.empty()) {
                s += " (";
                bool first = true;
                int n = 0;
                for (const auto& w : walk) {
                    ++n;
                    if (!first) s += ", ";
                    first = false;
                    const Candidate& c = cands[w.idx];
                    const std::string_view what = c.formName.empty() ? Name(c.family) : c.formName;
                    s += std::format("{}: {} '{}'", n, what, w.reason);
                }
                s += ")";
            }
        }
        if (d.vetoed || d.pruned) s += std::format(" veto={} pruned={}", d.vetoed, d.pruned);
        if (d.overCap) s += std::format(" OVER-CAP({} offered)", d.generated);
        if (d.badFacts) s += std::format(" BAD-FACTS={}", d.badFacts);
        s += std::format(" {:.3f}ms", ms);
        return s;
    }

    std::string FormatWhy(const Candidate& c, const Mind& m, const Profile& p, const Tuning& t, std::uint8_t rank,
                          float bestHeal) {
        std::vector<Term> terms;
        Cons vetoBy = Cons::kCount;
        float focus = 1.0f;
        const float score = ScoreOne(c, m, p, t, rank, bestHeal, &terms, &vetoBy, &focus);
        std::string s = std::format("{} = {:.3f}", Label(c), score);
        if (score <= 0.0f) {
            const bool bad = !FactsFinite(c.f);
            s += std::format(" VETO {}", bad ? std::string_view("BAD-FACT (NaN/inf)")
                                             : (vetoBy == Cons::kCount ? std::string_view("weight") : Name(vetoBy)));
        }
        for (const auto& tm : terms) {
            const Role role = RoleOf(tm.cons);
            if (role == Role::Gate) { if (tm.raw <= 0.0f) s += std::format(" | {} FAIL", Name(tm.cons)); continue; }
            s += std::format(" | {} x={} {}", Name(tm.cons), Num(tm.x), Num(role == Role::Boost ? tm.raw : tm.comp));
            if (role == Role::Shape && tm.w != 1.0f) s += std::format(" w{:.1f}", tm.w);
        }
        const auto fi = static_cast<std::size_t>(c.family);
        s += std::format(" | aw {:.2f} cw {:.2f}", fi < kFamilies ? t.actionWeight[fi] : 0.0f,
                         detail::ClassWeight(c, p));
        if (focus > 1.0f) s += std::format(" pf {:.2f}", focus);
        return s;
    }

    bool ShouldLog(Mind& m, const Key& pick, Verdict verdict, bool fired, double now, const Tuning& t) {
        // The verdict and the fired flag are part of the identity (review round 1, finding 5): Act -> Hold on the
        // same key, a pick -> "none fired", and Idle (whose key is the default Key{}) all log at once.
        const auto v = static_cast<std::uint8_t>(verdict);
        if (m.hasLogged && m.logged == pick && m.loggedVerdict == v && m.loggedFired == fired &&
            now - m.loggedAt < t.logRepeatSec)
            return false;
        m.hasLogged = true;
        m.logged = pick;
        m.loggedVerdict = v;
        m.loggedFired = fired;
        m.loggedAt = now;
        return true;
    }

    Readout MakeReadout(const Decision& d, std::span<const Candidate> cands, const Mind& m, const Tuning& t,
                        double now, const Profile* p) {
        Readout r;
        static const Profile kDefault;   // uncustomised catalogue: what the scan scores with until card I8
        const Profile& prof = p ? *p : kDefault;
        r.rapportRank = d.rapportRank;
        r.actFloor = t.actFloor;
        r.verdict = d.verdict;
        for (const auto& s : d.ranked) {
            if (r.topN >= r.top.size()) break;
            r.top[r.topN].label = Label(cands[s.idx]);
            r.top[r.topN].score = s.score;
            r.top[r.topN].why = FormatWhy(cands[s.idx], m, prof, t, d.rapportRank, d.bestHeal);
            ++r.topN;
        }
        if (!d.order.empty()) r.pick = Label(cands[d.order.front().idx]);
        r.runningSec = m.hasRunning ? (std::max)(0.0, now - m.decidedAt) : 0.0;
        return r;
    }

}
