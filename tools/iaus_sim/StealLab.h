#pragma once
// iaus_sim/StealLab.h -- cards G13 (act.steal) and G14 (act.pick_locks): the WITNESS PRE-CHECK against the engine's detection.
// HOST-ONLY. T2 batch S, iaus-sim-v2.md section 31.8.
//
// What is compared. MFO never takes an owned item unless `logistics/Steal.cpp Evaluate` proves that nobody would witness it.
// That proof is GEOMETRIC (Harbinger's SenseActor: a sight ray inside a view cone, a heard noise, plain nearness), while the
// engine decides a witness by its DETECTION formula (Detect.h, calculate-detection-re.md). The lab draws town scenes, asks
// both, and counts where they disagree:
//   take + detected  = a crime the pre-check would let through (must be about zero: the check is meant to fail closed)
//   refused + nobody = a take the engine would have let pass unseen (the price of failing closed)
//
// FACT (native/logistics/Steal.cpp Evaluate, read at 33c643f; questions Q-L-1..6):
//   - refused unless the follower sneaks; the witness radius is max(iCrimeAlarmRecDistance, Harbinger's hearing default 1536),
//     and above Harbinger's 8192 the check refuses;
//   - a guard that sees the follower or the item refuses (TheftGate::GuardCheck);
//   - every loaded, live non-teammate actor (animals too) within the radius of the follower, the player or the item is asked
//     with ALL senses toward the follower, then toward the player; any hit refuses;
//   - the item's owner in line of sight of the follower or of the player refuses at ANY distance;
//   - more than 96 actors refuses; a deferred sight ray is "pending" (asked again, never acted on).
// FACT (APMF_API.h "ABI v18"): the senses are sight (a ray, a range), hearing (a new engine noise of the target inside the
//   hearing radius, or the target fighting nearby), proximity (256 units by default). At most 8 sight pairs are measured per frame.
// MODEL (everything else): the scenes; Harbinger's sight cone taken as the engine's fDetectionViewCone; "a new engine noise" =
//   the target moves at the run speed (a sneaking walk and standing still make none); a guard's sight = an unblocked line
//   inside the radius; the ENGINE's witness = an actor within iCrimeAlarmRecDistance of the thief whose detection level of the
//   follower OR of the player is above 0 (theft-and-crime-alarm-re.md: the witness loop is not read yet; "witness detection
//   after the swap runs FOR THE PLAYER" is why the player is scored too); the player's Sneak is scaled by
//   fPlayerDetectionSneakMult; iCrimeAlarmRecDistance falls back to 4000 when the list does not carry it (not verified against
//   Skyrim.esm in this round).
#include "iaus_sim/Setup.h"

namespace Sim {

    struct StealTally { long n = 0, take = 0, takeDetF = 0, takeDetP = 0, refused = 0, refusedClear = 0, truthClear = 0, pairs = 0, rounds = 0;
        long why[6] = {};   // 0 not sneaking, 1 guard, 2 senses the follower, 3 senses the player, 4 the owner sees, 5 radius / count
    };

    inline int RunStealLab(const Data& D, const std::vector<SimClass>& classes, int n, std::uint64_t seed) {
        const SimClass* thief = nullptr; for (const SimClass& c : classes) if (c.name == "thief") thief = &c;
        if (!thief) { std::fprintf(stderr, "--steal-lab needs the real class table (no --classes legacy)\n"); return 2; }
        const Detect::Gmst g = Detect::Load(D);
        const float alarm = static_cast<float>(D.G("iCrimeAlarmRecDistance", 4000.0)), radius = std::max(alarm, 1536.0f);
        const float playerMult = static_cast<float>(D.G("fPlayerDetectionSneakMult", 0.4));
        struct Occ { float x, y, r; };
        struct Npc { float x, y, hx, hy, skill; bool guard, owner; };
        StealTally T[3];   // 0 dark (the Thief's own default row: Dark -> Steal), 1 dim, 2 bright
        Rng R; R.g.seed(seed * 0x9E3779B97F4A7C15ULL + 4242u);
        for (int it = 0; it < n; ++it) {
            const bool interior = R.P(0.6f);
            const float light = interior ? R.U(0.05f, 0.6f) : R.P(0.5f) ? R.U(0.1f, 0.25f) : R.U(0.7f, 1.0f);
            StealTally& t = T[light <= 0.3f ? 0 : light <= 0.6f ? 1 : 2]; ++t.n;
            const int level = R.I(12, 45);
            const FollowerBuild fb = BuildFollower(D, thief->fam, thief->kit, thief->gate, level, R);
            const float skillF = fb.spec.Skill(15) + StealthCombine(D.sneakSkillRows, fb.spec, false);
            const float weightF = static_cast<float>(fb.spec.pieces) * 3.0f + (fb.spec.melee.Has() ? fb.spec.melee.weight : 0.0f);
            const bool sneakF = !R.P(0.05f), moveF = R.P(0.5f);
            const float ia = R.U(0.0f, 6.2831853f), id = R.U(60.0f, 140.0f), ix = id * std::cos(ia), iy = id * std::sin(ia);   // the item
            const float pa = R.U(0.0f, 6.2831853f), pd = R.U(200.0f, 1500.0f), px = pd * std::cos(pa), py = pd * std::sin(pa);   // the player
            const bool sneakP = R.P(0.5f), moveP = R.P(0.5f); const float skillP = R.U(15.0f, 60.0f);
            std::vector<Occ> occ; for (int i = interior ? R.I(1, 5) : R.I(0, 2); i > 0; --i) { Occ o{ R.U(-1500.0f, 1500.0f), R.U(-1500.0f, 1500.0f), R.U(60.0f, 160.0f) }; if (std::hypot(o.x, o.y) > o.r + 80.0f && std::hypot(o.x - px, o.y - py) > o.r + 80.0f) occ.push_back(o); }
            std::vector<Npc> npc; for (int i = R.I(0, 6); i > 0; --i) { const float a = R.U(0.0f, 6.2831853f), d = R.U(150.0f, 3000.0f), h = R.U(0.0f, 6.2831853f); npc.push_back({ d * std::cos(a), d * std::sin(a), std::cos(h), std::sin(h), R.U(15.0f, 50.0f), R.P(0.15f), false }); }
            if (!npc.empty() && R.P(0.5f)) npc[static_cast<std::size_t>(R.I(0, static_cast<int>(npc.size()) - 1))].owner = true;
            auto los = [&](float ax, float ay, float bx, float by) { for (const Occ& o : occ) { const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy; if (l2 < 1) continue;
                    const float tt = std::clamp(((o.x - ax) * dx + (o.y - ay) * dy) / l2, 0.0f, 1.0f); if (std::hypot(ax + tt * dx - o.x, ay + tt * dy - o.y) < o.r) return false; } return true; };
            auto angle = [](const Npc& o, float tx, float ty) { const float bx = tx - o.x, by = ty - o.y, lb = std::hypot(bx, by); return lb < 1.0f ? 0.0f : std::acos(std::clamp((o.hx * bx + o.hy * by) / lb, -1.0f, 1.0f)) * 57.29578f; };
            // Harbinger's SenseActor, all senses (MODEL where the header does not say): sight, a heard noise, nearness
            auto senses = [&](const Npc& o, float tx, float ty, bool noisy) { const float d = std::hypot(o.x - tx, o.y - ty);
                if (d <= 256.0f) return true;
                if (d <= radius && noisy) return true;
                return d <= radius && angle(o, tx, ty) < 0.5f * g.viewCone && los(o.x, o.y, tx, ty); };
            // the engine's detection level of a target (Detect.h; nobody is in combat in a town)
            auto detLevel = [&](const Npc& o, float tx, float ty, bool sneaking, bool moving, bool running, float skill, float weight, float noiseMult) {
                Detect::In in; in.dist = std::hypot(o.x - tx, o.y - ty); in.angleDeg = angle(o, tx, ty); in.losGeom = los(o.x, o.y, tx, ty); in.exterior = !interior;
                in.inCombatO = false; in.oHasTarget = false; in.targetIsOsTarget = true; in.skillO = o.skill; in.skillT = skill; in.sneakingT = sneaking; in.moving = moving; in.running = running;
                in.noiseMult = noiseMult; in.moveMod = 1.0f; in.weight = weight; in.light = g_lightScale * light;
                return Detect::Calculate(g, in).level; };
            // ── the pre-check (Steal.cpp Evaluate, in its own order) ──
            int why = -1; long pairs = 0;
            if (!sneakF) why = 0;
            else if (radius > 8192.0f) why = 5;
            else {
                for (const Npc& o : npc) if (o.guard && ((std::hypot(o.x, o.y) <= radius && los(o.x, o.y, 0, 0)) || (std::hypot(o.x - ix, o.y - iy) <= radius && los(o.x, o.y, ix, iy)))) { why = 1; break; }
                if (why < 0) for (const Npc& o : npc) {
                    const float near = std::min({ std::hypot(o.x, o.y), std::hypot(o.x - px, o.y - py), std::hypot(o.x - ix, o.y - iy) });
                    if (near > radius && !o.owner) continue;
                    if (near <= radius) { pairs += 2;
                        if (senses(o, 0, 0, moveF && !sneakF)) { why = 2; break; }
                        if (senses(o, px, py, moveP && !sneakP)) { why = 3; break; } }
                    if (o.owner) { pairs += 2; if (los(o.x, o.y, 0, 0) || los(o.x, o.y, px, py)) { why = 4; break; } }
                }
            }
            // ── the engine (MODEL of the unread witness loop): any actor inside the alarm distance of the thief that detects him or the player ──
            bool detF = false, detP = false;
            for (const Npc& o : npc) { if (std::hypot(o.x, o.y) > alarm) continue;
                if (detLevel(o, 0, 0, sneakF, moveF, moveF && !sneakF, skillF, weightF, sneakF ? thief->noiseClass : 1.0f) > 0) detF = true;
                if (detLevel(o, px, py, sneakP, moveP, moveP && !sneakP, skillP * playerMult, 20.0f, 1.0f) > 0) detP = true; }
            t.pairs += pairs; t.rounds += std::max<long>(1, (pairs + 7) / 8);
            if (!(detF || detP)) ++t.truthClear;
            if (why < 0) { ++t.take; if (detF) ++t.takeDetF; if (detP && !detF) ++t.takeDetP; }
            else { ++t.refused; ++t.why[why]; if (!(detF || detP)) ++t.refusedClear; }
        }
        std::printf("STEAL LAB (cards G13 / G14: logistics/Steal.cpp Evaluate against Detect.h)  list=%s  scenes=%d  seed=%llu  witness radius=%.0f (iCrimeAlarmRecDistance %.0f)\n", D.name.c_str(), n, (unsigned long long)seed, radius, alarm);
        std::printf("%-7s %7s %7s %10s %10s %9s %11s %9s | refused because: %7s %6s %8s %8s %6s\n", "light", "scenes", "take%", "take+detF%", "take+detP%", "refused%", "refusedClr%", "clear%", "standing", "guard", "sensesF", "sensesP", "owner");
        static const char* nm[3] = { "dark", "dim", "bright" };
        for (int i = 0; i < 3; ++i) { const StealTally& t = T[i]; const double n0 = std::max<double>(1, double(t.n)), tk = std::max<double>(1, double(t.take)), rf = std::max<double>(1, double(t.refused));
            std::printf("%-7s %7ld %7.1f %10.2f %10.2f %9.1f %11.1f %9.1f |                  %7.1f %6.1f %8.1f %8.1f %6.1f\n", nm[i], t.n, 100.0 * double(t.take) / n0, 100.0 * double(t.takeDetF) / tk, 100.0 * double(t.takeDetP) / tk, 100.0 * double(t.refused) / n0,
                        100.0 * double(t.refusedClear) / rf, 100.0 * double(t.truthClear) / n0, 100.0 * double(t.why[0]) / rf, 100.0 * double(t.why[1]) / rf, 100.0 * double(t.why[2]) / rf, 100.0 * double(t.why[3]) / rf, 100.0 * double(t.why[4]) / rf); }
        long pairs = 0, rounds = 0, all = 0; for (const StealTally& t : T) { pairs += t.pairs; rounds += t.rounds; all += t.n; }
        std::printf("sight pairs per check: %.1f mean; at 8 pairs a frame and a 400 ms re-ask that is %.2f rounds, so %.0f ms of latency past the first answer (mean)\n", double(pairs) / std::max<double>(1, double(all)), double(rounds) / std::max<double>(1, double(all)), 400.0 * (double(rounds) / std::max<double>(1, double(all)) - 1.0));
        std::printf("columns: take%% the pre-check allows the take; take+detF%% / take+detP%% of those the engine's formula has an actor detecting the follower / only the player (a crime let through); refusedClr%% of the refusals nobody would have detected; clear%% scenes the engine's formula calls unwitnessed\n");
        std::printf("G14: act.pick_locks opens UNOWNED chests only (Q-L-6); its owned-lock path asks this same check and nothing passes it today, so the table is G13's.\n");
        return 0;
    }

}
