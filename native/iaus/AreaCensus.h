#pragma once
// iaus/AreaCensus.h -- WHO DOES THIS AREA HIT (package M2 of Docs/research/2026-11/placed-spells-plan.md).
//
// PURE (standard headers + Snapshot.h, never an RE:: type). ONE READER for every area consumer: the aimed blast and the self blast (Generate.cpp,
// the splash_safe gate, Cast Offense / Exploit / Scroll / Staff and the hostile area of Cast Control) and S11b's friends-in-area count; the wall,
// location burst and rune packages (M3, M5, M6) call it with their own point. The LINE OF FIRE keeps its own reader (Estimate::InLineOfFire).
//
// BYSTANDER = any living, enabled actor that is not hostile to the caster and not one of the party members the snapshot lists: townsfolk, guards,
// other mods' followers, anybody's commanded summon that is not hostile to him, mounts (the player's horse). The rows come from the main-built
// table (iaus/AreaActors.h); Gather leaves out the caster, his foes and his allies by id. A bystander in the area, or a picture that cannot be
// trusted, closes the gate: the spell stays a candidate, this situation scores 0.
//
// To add an area consumer: call AreaCensus at its point. To change who is a bystander: AreaActors.cpp BuildPass (what is listed) and Candidates.cpp
// Gather (what is left out). To change a number: kAreaGuard below.
#include "iaus/Estimate.h"
#include "iaus/Snapshot.h"

namespace MFO::Iaus {

    // THE ONE TABLE of the area guard's numbers. Nothing here is searched by the tuner: it is a crime guard, not a preference.
    struct AreaGuardTuning {
        // PLACEHOLDER. Added to the radius for BYSTANDERS only (an ally is read live in the same lap). Sized from the cadence: the table a lap reads is
        // at most maxAgeTicks x 133 ms old and normally one tick plus a frame (~0.15 to 0.3 s); a running NPC covers ~370 units/s, so 128 units is
        // about a third of a second of running.
        float         safetyMargin = 128.0f;
        std::uint32_t refreshTicks = 1;   // unpaused worker ticks (133 ms) between two builds while somebody wants the table
        std::uint32_t maxAgeTicks = 3;    // older than this = NotReady = the gate is closed (never read as "nobody there")
        // PLACEHOLDER, no reader until package M6 (runes): a rune stands for its life, not the cast moment, so out of combat in a settlement it scores 0.
        bool          runeOutOfCombatInSettlement = false;
    };
    inline constexpr AreaGuardTuning kAreaGuard{};

    struct AreaHits {
        int  foes = 0;            // the decision's foes inside the radius
        int  allies = 0;          // party members the snapshot lists, inside the radius + the caller's ally pad
        int  bystanders = 0;      // ... inside the radius + a body + kAreaGuard.safetyMargin
        bool blind = false;       // the bystander picture cannot be trusted (AreaPicture::NotReady): nobody may read bystanders == 0 as "none"
        std::uint32_t firstBystander = 0;   // for the log: the nearest one
        AreaKind      firstKind = AreaKind::Person;
        float         firstDist = 0.0f;
        [[nodiscard]] bool BystandersSafe() const { return !blind && bystanders == 0; }
    };

    namespace Estimate {

        // An unreadable foe or ally position is "far" (Generate.cpp Between's rule, unchanged). An unreadable BYSTANDER position, centre or radius is a hit:
        // the guard never reads a broken number as "clear".
        [[nodiscard]] inline float AreaDist(float a_x, float a_y, float a_z, float a_cx, float a_cy, float a_cz) {
            const float dx = a_x - a_cx, dy = a_y - a_cy, dz = a_z - a_cz;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // (center, radius, caster): the caster is a_s.self. a_foes = the foes the decision counts (Generate's kept list, the numbers AreaValue always read).
        // a_allyPad = how far past the radius an ally still counts (0 for his own blast, kFireBodyPad for a blast at a target).
        template <class FoeList>   // any range of const Combatant*
        [[nodiscard]] AreaHits AreaCensus(const Snapshot& a_s, const FoeList& a_foes, float a_cx, float a_cy, float a_cz, float a_radius, float a_allyPad) {
            AreaHits h;
            const auto dist = [&](float x, float y, float z) { const float d = AreaDist(x, y, z, a_cx, a_cy, a_cz); return std::isfinite(d) ? d : 1.0e9f; };
            for (const Combatant* f : a_foes) if (dist(f->x, f->y, f->z) <= a_radius) ++h.foes;
            for (const Combatant& a : a_s.allies) if (dist(a.x, a.y, a.z) <= a_radius + a_allyPad) ++h.allies;
            h.blind = a_s.areaPicture == AreaPicture::NotReady;
            if (a_s.areaPicture != AreaPicture::Ready) return h;   // NotApplicable: a host with no bystanders (the simulator, the harness)
            for (const AreaActor& b : a_s.areaActors) {
                const float body = (std::max)(kFireBodyPad, Pos(b.radius));
                const float d = AreaDist(b.x, b.y, b.z, a_cx, a_cy, a_cz);
                if (d > a_radius + body + kAreaGuard.safetyMargin) continue;   // a NaN never passes this test, so it counts as a hit
                if (h.bystanders++ == 0 || d < h.firstDist) { h.firstBystander = b.id; h.firstKind = b.kind; h.firstDist = d; }
            }
            return h;
        }
        template <class FoeList>
        [[nodiscard]] AreaHits AreaCensus(const Snapshot& a_s, const FoeList& a_foes, const Combatant& a_centre, float a_radius, float a_allyPad) {
            return AreaCensus(a_s, a_foes, a_centre.x, a_centre.y, a_centre.z, a_radius, a_allyPad);
        }

        [[nodiscard]] inline const char* AreaKindName(AreaKind a_k) {
            switch (a_k) {
            case AreaKind::Guard:     return "guard";
            case AreaKind::Teammate:  return "teammate";
            case AreaKind::Commanded: return "summon";
            case AreaKind::Mount:     return "mount";
            case AreaKind::Person:    break;
            }
            return "bystander";
        }

    }

}
