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
        // PLACEHOLDER (M4 F1). How far past a SELF BLAST's radius (and a body, kFireBodyPad) a PARTY member or the player closes it. Nothing protects the party at the
        // release (Harbinger's line of fire skips Self delivery, its bystander hold excludes the party), and a charge is never cut, so it covers the walk between the
        // last open lap and the blast: about one second of running (~370 units/s). Never under safetyMargin (static_assert below).
        float         selfBlastAllyMargin = 384.0f;
        std::uint32_t refreshTicks = 1;   // unpaused worker ticks (133 ms) between two builds while somebody wants the table
        std::uint32_t maxAgeTicks = 3;    // older than this = NotReady = the gate is closed (never read as "nobody there")
        // PLACEHOLDER, no reader until package M6 (runes): a rune stands for its life, not the cast moment, so out of combat in a settlement it scores 0.
        bool          runeOutOfCombatInSettlement = false;
        // PLACEHOLDER (M3 review F3). An actor farther than this from EVERY asking follower is not listed at all, so IsHostileToActor is not called for it
        // (AreaActors.cpp BuildPass). The widest placed area is a hazard or a Fire Storm (about 2200 units) round a foe inside the chase cap; Harbinger's
        // cast guard keeps nothing past 4096 for the same reason.
        float         cullRange = 6000.0f;
        // PLACEHOLDER (M3 review F2). A wall's hazard LIVES (30 s, 10 hits), so a bystander who walks in after the cast moment is hit and it is a crime owned by
        // the caster: the margin round a wall's footprint AND its stream line is a walking distance, not the 128 sized for one 0.15 to 0.3 s cast moment
        // (about 1000 units = 3 s of a walk-to-run). BYSTANDERS only: a party member who walks in is the test list's and REVIEW-BACKLOG MFO-B871's open
        // half, it is not a crime. Read by Generate.cpp's wall block only.
        float         wallMargin = 1000.0f;
        // PLACEHOLDER (M3 review F6). The least footprint a wall is guarded with when its hazard radius could not be read (0 would leave everybody past the
        // foe unguarded); a vanilla barrier is 3 to 3.5 ft = 64 to 75 units. SpellBook.cpp, the wall entry.
        float         wallFloorUnits = 64.0f;
    };
    inline constexpr AreaGuardTuning kAreaGuard{};
    static_assert(kAreaGuard.selfBlastAllyMargin >= kAreaGuard.safetyMargin);

    // THE PADS OF ONE ENTRY'S AREA (the one place): how far past the radius a party member, and a bystander, still counts.
    // A hostile self blast: body + movement margin. A self area that harms nobody (Harmony): 0. An area at the target: a body (the ch.25 cast guard's area + 64).
    [[nodiscard]] inline float AreaAllyPad(const SpellEntry& a_e) {
        if (!a_e.selfDelivery) return Estimate::kFireBodyPad;
        return a_e.nature == Prune::Nature::Offense || a_e.hostileArea ? Estimate::kFireBodyPad + kAreaGuard.selfBlastAllyMargin : 0.0f;
    }
    [[nodiscard]] inline float AreaBystanderMargin(const SpellEntry& a_e) { return a_e.wall || a_e.leavesHazard ? kAreaGuard.wallMargin : kAreaGuard.safetyMargin; }

    // Does this listed actor count as a bystander for this snapshot? A party summon and the player's horse only when the MCM option says so
    // (Config::g_areaSummonsBlock, default OFF: Harbinger's ruling "Bystanders only", marth 2026-11-02). The ONE place that option is enforced.
    [[nodiscard]] inline bool CountsAsBystander(const Snapshot& a_s, const AreaActor& a_b) {
        if (a_b.kind == AreaKind::PartySummon || a_b.kind == AreaKind::PlayerHorse) return a_s.areaPartyOwnedBlock;
        return true;
    }

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

    // Is there a cast candidate of this spell whose area the census left OPEN (Cons::SplashSafe's own reading: Catalogue.cpp)? A standing blast claim is kept only then.
    [[nodiscard]] inline bool AreaOpenFor(const std::vector<Candidate>& a_actions, std::uint32_t a_form) {
        for (const Candidate& c : a_actions)
            if (c.form == a_form && (c.family == Family::CastOffense || c.family == Family::CastControl) && c.f.areaGuardOk &&
                (c.family == Family::CastControl || c.f.splashSafe)) return true;
        return false;
    }

    namespace Estimate {

        // An unreadable foe or ally position is "far" (Generate.cpp Between's rule, unchanged). An unreadable BYSTANDER position, centre or radius is a hit:
        // the guard never reads a broken number as "clear".
        [[nodiscard]] inline float AreaDist(float a_x, float a_y, float a_z, float a_cx, float a_cy, float a_cz) {
            const float dx = a_x - a_cx, dy = a_y - a_cy, dz = a_z - a_cz;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // (center, radius, caster): the caster is a_s.self. a_bystanderMargin: how far past the radius a bystander still counts (a wall passes kAreaGuard.wallMargin). a_foes = the foes the decision counts (Generate's kept list, the numbers AreaValue always read).
        // a_allyPad = how far past the radius an ally still counts (AreaAllyPad). a_foeRadius: the foes are counted inside this instead (< 0 = a_radius; SpellEntry::FoeUnits).
        template <class FoeList>   // any range of const Combatant*
        [[nodiscard]] AreaHits AreaCensus(const Snapshot& a_s, const FoeList& a_foes, float a_cx, float a_cy, float a_cz, float a_radius, float a_allyPad,
                                              float a_bystanderMargin = kAreaGuard.safetyMargin, float a_foeRadius = -1.0f) {
            AreaHits h;
            if (!(a_foeRadius >= 0.0f)) a_foeRadius = a_radius;
            const auto dist = [&](float x, float y, float z) { const float d = AreaDist(x, y, z, a_cx, a_cy, a_cz); return std::isfinite(d) ? d : 1.0e9f; };
            for (const Combatant* f : a_foes) if (dist(f->x, f->y, f->z) <= a_foeRadius) ++h.foes;
            for (const Combatant& a : a_s.allies) if (dist(a.x, a.y, a.z) <= a_radius + a_allyPad) ++h.allies;
            h.blind = a_s.areaPicture == AreaPicture::NotReady;
            if (a_s.areaPicture != AreaPicture::Ready) return h;   // NotApplicable: a host with no bystanders (the simulator, the harness)
            for (const AreaActor& b : a_s.areaActors) {
                if (!CountsAsBystander(a_s, b)) continue;
                const float body = (std::max)(kFireBodyPad, Pos(b.radius));
                const float d = AreaDist(b.x, b.y, b.z, a_cx, a_cy, a_cz);
                if (d > a_radius + body + a_bystanderMargin) continue;   // a NaN never passes this test, so it counts as a hit
                if (h.bystanders++ == 0 || d < h.firstDist) { h.firstBystander = b.id; h.firstKind = b.kind; h.firstDist = d; }
            }
            return h;
        }
        template <class FoeList>
        [[nodiscard]] AreaHits AreaCensus(const Snapshot& a_s, const FoeList& a_foes, const Combatant& a_centre, float a_radius, float a_allyPad,
                                              float a_bystanderMargin = kAreaGuard.safetyMargin, float a_foeRadius = -1.0f) {
            return AreaCensus(a_s, a_foes, a_centre.x, a_centre.y, a_centre.z, a_radius, a_allyPad, a_bystanderMargin, a_foeRadius);
        }

        // THE LINE (M3, a wall's stream): bystanders along the segment from a_from to a_to, where they project BETWEEN the two ends (one behind the caster or
        // past the target is the area census's business, not the stream's), within the corridor max(kFireLinePad, a_areaUnits + his body) + a_margin (the hazard's footprint runs along the whole stream, M3 review F4).
        // The same bystander rules as the area census: an unreadable position is a hit, an unreadable picture is blind. Allies on the line are
        // Estimate::InLineOfFire's, not counted here.
        [[nodiscard]] inline AreaHits AreaCensusLine(const Snapshot& a_s, const Combatant& a_from, const Combatant& a_to, float a_areaUnits, float a_margin) {
            AreaHits h;
            h.blind = a_s.areaPicture == AreaPicture::NotReady;
            if (a_s.areaPicture != AreaPicture::Ready) return h;
            const float abx = a_to.x - a_from.x, aby = a_to.y - a_from.y, abz = a_to.z - a_from.z;
            const float len2 = abx * abx + aby * aby + abz * abz;
            for (const AreaActor& b : a_s.areaActors) {
                if (!CountsAsBystander(a_s, b)) continue;
                float d = std::numeric_limits<float>::quiet_NaN();   // NaN = unreadable = a hit
                bool between = true;
                if (std::isfinite(len2)) {
                    const float apx = b.x - a_from.x, apy = b.y - a_from.y, apz = b.z - a_from.z;
                    if (len2 <= 1.0f) d = std::sqrt(apx * apx + apy * apy + apz * apz);
                    else {
                        const float t = (apx * abx + apy * aby + apz * abz) / len2;
                        if (std::isfinite(t) && !(t >= 0.0f && t <= 1.0f)) between = false;
                        const float dx = apx - abx * t, dy = apy - aby * t, dz = apz - abz * t;
                        d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                }
                if (!between) continue;
                const float reach = (std::max)(kFireLinePad, Pos(a_areaUnits) + Pos(b.radius)) + a_margin;
                if (d > reach) continue;   // a NaN never passes this test, so it counts as a hit
                if (h.bystanders++ == 0 || d < h.firstDist) { h.firstBystander = b.id; h.firstKind = b.kind; h.firstDist = d; }
            }
            return h;
        }

        [[nodiscard]] inline const char* AreaKindName(AreaKind a_k) {
            switch (a_k) {
            case AreaKind::Guard:     return "guard";
            case AreaKind::Teammate:  return "teammate";
            case AreaKind::Commanded: return "summon";
            case AreaKind::Mount:     return "mount";
            case AreaKind::PartySummon: return "party summon";
            case AreaKind::PlayerHorse: return "player's horse";
            case AreaKind::Person:    break;
            }
            return "bystander";
        }

    }

}
