#pragma once
// iaus/Spots.h -- SPOT KINDS: how candidate places are found, which actions pair with them, and how one is scored (S1). PURE.
//
// TO ADD A SPOT KIND: append to Core.h SpotKind, add its row to kSpotKinds and its generator in Spots.cpp, and its class rule in SpotKindsFor.
// TO ADD A CONSIDERATION: a Core.h Cons + SpotFacts field, its curve in Catalogue.cpp, its row in Spots.cpp kSpotSet / SpotInput, its fill in
// MovePass.cpp Fill. Nowhere else.
#include "iaus/ClassCurves.h"
#include "iaus/PartyPicture.h"

namespace MFO::Iaus::Move {

    // What a generator is asked: is this point of the mesh a spot of my kind for this member and this action, and how good (lower = better, world units)?
    struct GenIn {
        const PartyPicture& pic; const Env::View& v; const Tuning& t; const Member& m; const RankedAction& act;
        const Work::Flood& own;
        Env::Vec3 target; bool hasTarget = false; const Foe* foe = nullptr; bool targetIsAlly = false; bool targetUnreachable = false;
    };
    struct Cand { SpotKey key = 0; Env::Vec3 p; std::int32_t tri = -1; float walk = 0.0f; };
    using SpotGen   = bool (*)(const GenIn&, const Cand&, float& a_rank);
    using SpotCheck = bool (*)(const GenIn&, const Cand&, Env::Scratch&);   // the dear half (geometry rays), on the best few only. Null = none

    inline constexpr std::uint64_t F(Family a_f) { return std::uint64_t{ 1 } << static_cast<unsigned>(a_f); }
    template <class... Fs> inline constexpr std::uint64_t Families(Fs... a_f) { return (F(a_f) | ...); }

    bool GenFiring(const GenIn&, const Cand&, float&);      bool CheckSight(const GenIn&, const Cand&, Env::Scratch&);
    bool GenCover(const GenIn&, const Cand&, float&);       bool CheckCover(const GenIn&, const Cand&, Env::Scratch&);
    bool GenHighGround(const GenIn&, const Cand&, float&);
    bool GenChokepoint(const GenIn&, const Cand&, float&);
    bool GenFlank(const GenIn&, const Cand&, float&);
    bool GenHealReach(const GenIn&, const Cand&, float&);
    bool GenPeel(const GenIn&, const Cand&, float&);
    bool GenKite(const GenIn&, const Cand&, float&);
    bool GenRegroup(const GenIn&, const Cand&, float&);
    bool GenRetreatExit(const GenIn&, const Cand&, float&);
    bool GenReachUnreach(const GenIn&, const Cand&, float&);

    struct SpotKindRow { SpotKind kind; const char* name; SpotGen gen; SpotCheck check; std::uint64_t families; std::uint32_t needs; };
    // A Scroll pairs as the family of the spell it reads (RankedAction::Kind).
    inline constexpr std::array<SpotKindRow, kSpotKindCount> kSpotKinds = { {
        { SpotKind::Stay,        "stay",        nullptr,          nullptr,    ~std::uint64_t{ 0 }, 0 },   // always exists, needs no generator
        { SpotKind::Firing,      "firing",      &GenFiring,       &CheckSight, Families(Family::Attack, Family::CastOffense, Family::CastControl, Family::Exploit, Family::Taunt), 0 },
        { SpotKind::Cover,       "cover",       &GenCover,        &CheckCover, Families(Family::Retreat, Family::Disengage, Family::DrinkHealth, Family::DrinkMagicka, Family::DrinkStamina, Family::CastHeal, Family::CastBuff, Family::CastSummon, Family::CastWard), kNeedPin },
        { SpotKind::HighGround,  "high_ground", &GenHighGround,   &CheckSight, Families(Family::Attack, Family::CastOffense, Family::CastControl), 0 },   // offered UNPINNED (point + hold)
        { SpotKind::Chokepoint,  "chokepoint",  &GenChokepoint,   nullptr,     Families(Family::Attack, Family::PowerAttack, Family::Taunt, Family::Block, Family::Bash), kNeedPin },
        { SpotKind::Flank,       "flank",       &GenFlank,        nullptr,     Families(Family::Attack, Family::PowerAttack, Family::Sneak, Family::Step), kNeedPin },
        { SpotKind::HealReach,   "heal_reach",  &GenHealReach,    &CheckSight, Families(Family::CastHeal, Family::CastBuff), 0 },
        { SpotKind::Peel,        "peel",        &GenPeel,         nullptr,     Families(Family::Attack, Family::PowerAttack, Family::Taunt, Family::Bash, Family::CastControl), 0 },
        { SpotKind::Kite,        "kite",        &GenKite,         nullptr,     Families(Family::GiveGround, Family::BackOff, Family::Attack, Family::CastOffense), 0 },
        { SpotKind::Regroup,     "regroup",     &GenRegroup,      nullptr,     Families(Family::Regroup, Family::Retreat), 0 },
        { SpotKind::LootRoute,   "loot_route",  nullptr,          nullptr,     0, 0 },   // the logistics run's: no combat action pairs with it, so it is never offered here
        { SpotKind::RetreatExit, "retreat",     &GenRetreatExit,  nullptr,     Families(Family::Retreat, Family::Disengage, Family::GiveGround, Family::BackOff, Family::DrinkHealth), 0 },
        { SpotKind::ReachUnreach,"reach",       &GenReachUnreach, &CheckSight, Families(Family::Attack, Family::PowerAttack, Family::CastOffense, Family::CastControl), 0 },
    } };
    static_assert([] { for (std::size_t i = 0; i < kSpotKindCount; ++i) if (static_cast<std::size_t>(kSpotKinds[i].kind) != i) return false; return true; }(), "kSpotKinds is indexed by SpotKind");

    // The kinds the mover and the tuning allow now (KindBit mask): `needs` met by the caps, and a pin kind only with t.spotPinKinds on.
    [[nodiscard]] std::uint32_t AllowedKinds(const RoadCaps& a_caps, const Tuning& a_t);
    // The kinds a class may be offered (the ClassGate rule, as families are gated).
    [[nodiscard]] std::uint32_t SpotKindsFor(const ClassGate& a_g);
    [[nodiscard]] inline bool Pairs(SpotKind a_k, const RankedAction& a_a) { return a_a.Kind() < Family::kCount && (kSpotKinds[static_cast<std::size_t>(a_k)].families & F(a_a.Kind())) != 0; }
    // An action that IS a movement: standing still does not serve it.
    [[nodiscard]] bool IsMove(Family a_f);

    // The score of the (action, spot) considerations, 0..(boosts). 0 = vetoed. a_terms (optional) receives every term that applies.
    [[nodiscard]] float ScoreSpot(const SpotFacts& a_f, Family a_family, const Profile& a_p, const Tuning& a_t, std::vector<Term>* a_terms = nullptr);

    // ── geometry helpers of the pass ──
    [[nodiscard]] float Dist(const Env::Vec3& a, const Env::Vec3& b);
    void Flood(const Env::View& a_v, const Env::Vec3& a_from, std::int32_t a_tri, const Env::Walker& a_who, float a_radius, int a_maxTris, Work::Flood& a_out);
    void Points(const Env::View& a_v, std::int32_t a_tri, const Tuning& a_t, std::vector<Cand>& a_out);   // the candidate points of one triangle, appended
    void WallVerts(const Env::View& a_v, std::vector<std::uint8_t>& a_out);
    [[nodiscard]] std::uint64_t DoorwayOf(const Env::View& a_v, const std::vector<std::uint8_t>& a_wallVert, std::int32_t a_tri, int a_edge, const Tuning& a_t);   // 0 = not a doorway
    [[nodiscard]] std::array<std::uint64_t, 2> PortalsAt(const Env::View& a_v, const std::vector<std::uint8_t>& a_wallVert, std::int32_t a_tri, const Env::Vec3& a_p, const Tuning& a_t);
    [[nodiscard]] std::uint8_t ExitsAt(const Env::View& a_v, std::int32_t a_tri, const Env::Vec3& a_p, const std::vector<Foe>& a_foes, const Env::Walker& a_who, const Tuning& a_t);
    [[nodiscard]] bool GeomSees(const Env::View& a_v, const Env::Vec3& a_a, const Env::Vec3& a_b, const Tuning& a_t, Env::Scratch& a_s);
}
