#pragma once
// iaus/EnvTrust.h -- TRUST: which parts of the mesh the pass may plan on (S3, iaus-sim-v4.md 8.9). PURE: standard headers only, no engine type, no caller in
// the DLL yet. ONE reader for everybody: spots and routes ask Of / RouteLevel here and nowhere else.
//
// A REGION is a cell of 512 x 512 x 256 units. A MARK is one sign, seen once, at a place, going a way. Marks decay. A region is
//   trusted     no mark
//   doubted     one mark (one blocked leg is only a blocked leg)
//   distrusted  two different signs, or one sign Rules::repeat times, or a static sign whose row says so alone
// WHAT TAKES A MARK AWAY: its own decay. A walk ACROSS the place it was seen at, THE WAY it was going (the player's or a follower's): an edge walkable one way only
// is not proven by the other way. A stamp change (a door, a cell attached): everything goes.
// A place the player himself stood on lately reads trusted whatever its region says (Table::trail).
//
// TO ADD A SIGN: one row of kTrustSigns (and its enumerator, in order). A lived sign also needs the LegEvent or the Note call of whoever sees it.
// TO CHANGE A NUMBER: a sign's own in its row, everything else in Rules. The price of low trust: Catalogue.cpp SpotRegionTrust / SpotPathTrust.
#include "iaus/EnvQuery.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace MFO::Iaus::Trust {

    using FormID = std::uint32_t;
    using Region = std::int64_t;
    enum class Level : std::uint8_t { Trusted = 0, Doubted, Distrusted };
    [[nodiscard]] const char* Name(Level);

    enum class Sign : std::uint8_t {
        FarFromGround = 0, DanglingLink, OneWayLinks, IslandUnderParty, InsideGeometry, DoorlessPortal,   // static: the snapshot's builder reads them (G1), Note
        NoGround, EngineRefused, LengthFar, MovedUnwalked, UnplannedMove, Falling,                        // lived
        kCount };
    inline constexpr std::size_t kSigns = static_cast<std::size_t>(Sign::kCount);
    struct SignRow { Sign sign; const char* name; bool lived; float threshold; Level alone; float decaySec; const char* says; };
    // THE SIGNS. threshold: in the unit the row says, read by Seen(). alone: the level one mark argues for by itself. Every number a PLACEHOLDER ([probe-trust]).
    inline constexpr std::array<SignRow, kSigns> kTrustSigns = { {
        { Sign::FarFromGround,    "far-from-ground",   false, 96.0f, Level::Distrusted, 300.0f, "units between a sampled triangle and the ground under it: floating or buried navmesh" },
        { Sign::DanglingLink,     "dangling-link",     false, 1.0f,  Level::Doubted,    300.0f, "links of the region to a missing or stale triangle: a plugin conflict" },
        { Sign::OneWayLinks,      "one-way-links",     false, 0.02f, Level::Doubted,    300.0f, "share of the region's links whose other side does not name them (vanilla 0.00005)" },
        { Sign::IslandUnderParty, "island",            false, 1.0f,  Level::Doubted,    300.0f, "the flood under the party finds no way off" },
        { Sign::InsideGeometry,   "inside-geometry",   false, 1.0f,  Level::Distrusted, 300.0f, "triangles the rays say stand in or over geometry" },
        { Sign::DoorlessPortal,   "doorless-portal",   false, 1.0f,  Level::Doubted,    300.0f, "door portals whose door is null, disabled or deleted" },
        { Sign::NoGround,         "no-ground",         true,  0.0f,  Level::Doubted,    30.0f,  "a leg ended STUCK with a clear path, no door and no actor in the lane" },
        { Sign::EngineRefused,    "engine-refused",    true,  0.0f,  Level::Doubted,    30.0f,  "the engine ended a path we found" },
        { Sign::LengthFar,        "length-far",        true,  1.6f,  Level::Doubted,    30.0f,  "walked length / our route's length" },
        { Sign::MovedUnwalked,    "moved-unwalked",    true,  2.5f,  Level::Doubted,    30.0f,  "distance between two samples / what his speed allows: a teleport, a warp, a script" },
        { Sign::UnplannedMove,    "unplanned-move",    true,  1.0f,  Level::Doubted,    30.0f,  "a drop, a ladder or a parkour move on a leg whose route had none" },
        { Sign::Falling,          "falling",           true,  120.0f, Level::Doubted,   30.0f,  "height lost with no link under it" },
    } };
    static_assert([] { for (std::size_t i = 0; i < kSigns; ++i) if (static_cast<std::size_t>(kTrustSigns[i].sign) != i) return false; return true; }(), "kTrustSigns is indexed by Sign");
    // Does a measured value make the sign? (>= the row's threshold.) The one comparison of every sign.
    [[nodiscard]] inline bool Seen(Sign a_s, float a_value) { return a_value >= kTrustSigns[static_cast<std::size_t>(a_s)].threshold; }

    // THE RULES. One table. PLACEHOLDERS.
    struct Rules {
        float regionU = 512.0f, regionZ = 256.0f;
        int   repeat = 2;              // one LIVED sign this often is distrust. 1 = the 11-17 GATED mistake, the negative scenario holds it off
        float repeatSec = 1.5f;        // the same sign of the same body in the same region within this is one event
        float aheadU = 32.0f;          // what stopped him is this far ahead of where he stood: the region that gets the mark
        float clearU = 64.0f;          // a walk across a mark passes this near
        float routeU = 128.0f;         // ... a planned route, whose corners are that much coarser
        float levelU = 96.0f;          // ... and on this level
        float trailU = 96.0f;          // a place this near to where the player stood is proven
        float trailSec = 60.0f, trailStepU = 96.0f;
        float lengthFloorU = 250.0f;   // LengthFar needs this much more walked, whatever the ratio
        float refusedSec = 10.0f;      // a spot a leg was refused at is not offered to that member again for this long
        float doubtMult = 1.5f, distrustMult = 6.0f;   // a route's cost through such a region (FindPath avoid)
        int   marks = 96, trail = 48, refused = 32;    // memory bounds, the oldest goes
    };
    inline constexpr Rules kTrustRules{};

    struct Mark { Region region = 0; Sign sign = Sign::kCount; FormID who = 0; double t = 0.0; Env::Vec3 at; float dx = 0.0f, dy = 0.0f; float a = 0.0f, b = 0.0f; };   // a, b: the evidence (planned / walked length, a measured value)
    struct Refusal { FormID who = 0; std::uint64_t spot = 0; double t = 0.0; };
    struct Trail { Env::Vec3 p; double t = 0.0; };
    struct Line { Region region = 0; Level level = Level::Trusted; Mark why; };

    // Learned for the session, shared by the party, in no save (it lives in the party picture).
    struct Table {
        Rules rules;
        std::vector<Mark> marks;         // oldest first
        std::vector<Refusal> refused;
        std::vector<Trail> trail;        // the player's, oldest first
        std::vector<std::pair<Region, Level>> said;   // what was logged: once per region and level
        std::vector<Line> log;           // to print: the holder takes it ([env-trust])
        std::uint32_t gen = 0;           // moves when a mark comes or goes
        [[nodiscard]] bool Clean() const { return marks.empty(); }
    };

    // How one leg of a spot claim ended, as its holder saw it.
    enum class End : std::uint8_t { Arrived = 0, Stuck, Blocked, Refused, Walking /* not an end: the length is checked on the way */ };
    struct LegEvent {
        FormID who = 0; std::uint64_t spot = 0; End end = End::Arrived; double t = 0.0;
        bool laneClear = true;           // no door and no body in the lane (an end they explain is no sign)
        Env::Vec3 at, toward;            // where he stands, where he was going next
        float planned = 0.0f, walked = 0.0f;                        // FindPath's length, his own
        std::span<const Env::Vec3> route; std::uint32_t passed = 0; // the planned route's points (PathFact::via) and which of them he came by (bits)
    };

    [[nodiscard]] Region RegionOf(const Env::Vec3& a_p, const Rules& a_r = kTrustRules);
    [[nodiscard]] Level Of(const Table& a_t, Region a_region);
    [[nodiscard]] Level Of(const Table& a_t, const Env::Vec3& a_p);              // a place to stand on: the player's trail proves it
    // A route from a_from over a_via. The part inside the region he already stands in counts only where it crosses a mark: leaving a bad region is not depending on it.
    [[nodiscard]] Level RouteLevel(const Table& a_t, const Env::Vec3& a_from, std::span<const Env::Vec3> a_via);
    [[nodiscard]] bool  Refused(const Table& a_t, FormID a_who, std::uint64_t a_spot);

    bool Note(Table& a_t, Sign a_s, FormID a_who, const Env::Vec3& a_at, const Env::Vec3& a_toward, double a_now, float a_a = 0.0f, float a_b = 0.0f);   // one mark. false = the same event again
    int  Leg(Table& a_t, const LegEvent& a_e);                                    // what an end says: marks and refusals. Returns the marks made
    void Walked(Table& a_t, bool a_player, const Env::Vec3& a_from, const Env::Vec3& a_to, double a_now);   // a short piece of somebody's walk (one sample): clears, and the player's trail
    void Forget(Table& a_t);                                                      // a stamp change. What was said stays said
    void Decay(Table& a_t, double a_now);
    [[nodiscard]] std::string Text(const Line& a_l, const char* a_cell);          // the [env-trust] line
}
