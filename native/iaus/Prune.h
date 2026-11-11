#pragma once
// iaus/Prune.h -- the PURE pieces of candidate generation (card I4, design sections 1.6 and 2).
//
// Standard headers only, no RE:: type, no engine call, no global state: the same rule as iaus/Core.h, so
// tools/iaus_harness builds and checks these with the host compiler. iaus/SpellBook.cpp (main thread) and
// iaus/Candidates.cpp (worker) feed them plain values they read from the engine.
//
// What lives here: the spell NATURE decision (which action family a spell is a candidate for), the "never
// generated" list, the element -> RoleTag map, the P2 resource test, the P3 foe relevance order and a guarded
// ratio. Nothing here scores: scoring is iaus/Core.h, and this file never touches a curve or a weight.
#include "iaus/Core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MFO::Iaus::Prune {

    // a / b when both are finite and b > 0, else a_fallback. Every pct fact goes through this: an unloaded
    // actor reads a 0 maximum, and a NaN / inf fact vetoes the whole candidate (FactsFinite).
    [[nodiscard]] inline float SafeRatio(float a_num, float a_den, float a_fallback) {
        if (!std::isfinite(a_num) || !std::isfinite(a_den) || !(a_den > 0.0f)) return a_fallback;
        const float r = a_num / a_den;
        return std::isfinite(r) ? r : a_fallback;
    }
    // A float fact that must be finite: a_fallback (the Facts default) when it is not.
    [[nodiscard]] inline float Finite(float a_v, float a_fallback) { return std::isfinite(a_v) ? a_v : a_fallback; }

    // ── Spell nature ─────────────────────────────────────────────────────────────────────────────────────
    // Which family a spell is a candidate for. Unknown = the record does not say who the spell is for (marth:
    // IAUS understands spells LIVE from their records, no Synthesis data): no candidate, logged once per spell.
    enum class Nature : std::uint8_t { Unknown = 0, Offense, Control, Heal, Buff, Summon, Ward };

    // CasterConsent::ClassifySpell's answer, as plain values (cast/ owns that rule: it is NOT re-derived here).
    enum class Kind : std::uint8_t { Offense = 0, Buff, Heal };

    struct NatureIn {
        Kind kind = Kind::Buff;          // CasterConsent::ClassifySpell
        bool summon = false;             // Actuation::IsSummonSpell (kSummonCreature; Reanimate never matches)
        bool reanimate = false;          // SpellShape::Reanimate (needs a corpse: no IAUS family today)
        bool ward = false;               // SpellShape::Ward
        bool selfCloak = false;          // SpellShape::Cloak with Self delivery: worn, so a buff on himself
        bool control = false;            // an effect archetype Calm / Frenzy / Demoralize / Paralysis / TurnUndead
        bool scriptCostliest = false;    // SpellShape::Script (the costliest effect is archetype Script)
        bool natureUndeclared = false;   // Actuation::SpellNatureUndeclared
    };

    // Order matters: a summon / ward is its own family whatever ClassifySpell calls it; a control archetype is
    // Control even when the record also flags it hostile; a self cloak harms whoever stands near but is CAST on
    // himself, so it is a (melee) buff; an Offense or Heal kind is a nature the RECORD states
    // (a hostile flag, a beneficial Health effect), so a Script effect beside it does not hide it; only a
    // Buff-kind spell whose costliest effect is Script (or that cast/ calls undeclared) is Unknown.
    [[nodiscard]] inline Nature ClassifyNature(const NatureIn& a_in) {
        if (a_in.reanimate) return Nature::Unknown;
        if (a_in.summon) return Nature::Summon;
        if (a_in.ward) return Nature::Ward;
        if (a_in.control) return Nature::Control;
        if (a_in.selfCloak) return Nature::Buff;
        if (a_in.kind == Kind::Offense) return Nature::Offense;
        if (a_in.kind == Kind::Heal) return Nature::Heal;
        if (a_in.natureUndeclared || a_in.scriptCostliest) return Nature::Unknown;
        return Nature::Buff;
    }

    // ── "Never generated" (design section 2) and P1 (design 1.6) ─────────────────────────────────────────
    struct StaticIn {
        bool castable = true;            // Vocab::IsCastableSpell (kSpell, not constant effect)
        bool roadUnsupported = false;    // Actuation::CastRoadUnsupported (the animation rule)
        bool targetLocation = false;     // Delivery::kTargetLocation, or a Rune / Wall / TLBurst shape
        Nature nature = Nature::Unknown;
    };
    enum class Drop : std::uint8_t { Keep = 0, NotCastable, Unanimated, Placement, UnknownNature };
    [[nodiscard]] inline Drop StaticDrop(const StaticIn& a_in) {
        if (!a_in.castable) return Drop::NotCastable;
        if (a_in.roadUnsupported) return Drop::Unanimated;
        if (a_in.targetLocation) return Drop::Placement;
        if (a_in.nature == Nature::Unknown) return Drop::UnknownNature;
        return Drop::Keep;
    }
    [[nodiscard]] inline const char* DropName(Drop a_d) {
        switch (a_d) {
        case Drop::Keep:          return "keep";
        case Drop::NotCastable:   return "not castable";
        case Drop::Unanimated:    return "unanimated road only";
        case Drop::Placement:     return "target location";
        case Drop::UnknownNature: return "nature unknown";
        }
        return "?";
    }

    [[nodiscard]] inline Family FamilyOf(Nature a_n) {
        switch (a_n) {
        case Nature::Offense: return Family::CastOffense;
        case Nature::Control: return Family::CastControl;
        case Nature::Heal:    return Family::CastHeal;
        case Nature::Buff:    return Family::CastBuff;
        case Nature::Summon:  return Family::CastSummon;
        case Nature::Ward:    return Family::CastWard;
        case Nature::Unknown: break;
        }
        return Family::kCount;   // no family: never a candidate
    }

    // The resist actor values an element maps from (RE::ActorValue ordinals, fork include/RE/A/ActorValues.h:
    // kResistFire 41, kResistShock 42, kResistFrost 43). Anything else that harms is OtherDamage.
    inline constexpr int kAvResistFire = 41;
    inline constexpr int kAvResistShock = 42;
    inline constexpr int kAvResistFrost = 43;
    [[nodiscard]] inline RoleTag RoleOf(Nature a_n, int a_resistAv) {
        switch (a_n) {
        case Nature::Offense:
            if (a_resistAv == kAvResistFire) return RoleTag::Fire;
            if (a_resistAv == kAvResistFrost) return RoleTag::Frost;
            if (a_resistAv == kAvResistShock) return RoleTag::Shock;
            return RoleTag::OtherDamage;
        case Nature::Control: return RoleTag::Control;
        case Nature::Heal:    return RoleTag::Heal;
        case Nature::Buff:    return RoleTag::Buff;
        case Nature::Summon:  return RoleTag::Summon;
        case Nature::Ward:    return RoleTag::Ward;
        case Nature::Unknown: break;
        }
        return RoleTag::None;
    }

    // ── P2: resource ─────────────────────────────────────────────────────────────────────────────────────
    // "cost > current magicka drops every pair of that spell". A free spell (cost <= 0) always passes. A
    // non-finite cost or pool drops it (a broken read is not an affordable spell).
    [[nodiscard]] inline bool Affordable(float a_cost, float a_magicka) {
        if (!std::isfinite(a_cost) || !std::isfinite(a_magicka)) return false;
        return !(a_cost > 0.0f) || a_cost <= a_magicka;
    }

    // ── P3: the top foes by a cheap relevance score ──────────────────────────────────────────────────────
    struct FoeIn {
        std::uint32_t id = 0;
        float dist = 0.0f;
        float hpPct = 1.0f;
        bool  onMe = false;
        bool  onPlayer = false;
        bool  onAlly = false;
        bool  current = false;        // Targeting::Current (or the engine's current combat target)
        bool  playerTarget = false;
    };
    // Higher is more relevant. Distance is the base (0..1 over 4096 units, near = high); who the foe is on, the
    // follower's own target and the player's target each add more than the whole distance span, so an engaged
    // foe always outranks a merely near one; a nearly dead foe gets a small finish bonus.
    [[nodiscard]] inline float FoeRelevance(const FoeIn& a_f) {
        const float d = std::clamp(Finite(a_f.dist, 4096.0f), 0.0f, 4096.0f);
        float s = 1.0f - d / 4096.0f;
        if (a_f.current) s += 3.0f;
        if (a_f.onPlayer) s += 2.5f;
        if (a_f.playerTarget) s += 2.0f;
        if (a_f.onMe) s += 2.0f;
        if (a_f.onAlly) s += 1.5f;
        s += 0.5f * (1.0f - std::clamp(Finite(a_f.hpPct, 1.0f), 0.0f, 1.0f));
        return s;
    }
    inline constexpr std::size_t kTopFoes = 5;
    // Indices into a_foes of the a_keep most relevant, most relevant first. Ties go to the lower FormID, so the
    // order never depends on the combat group's iteration order.
    [[nodiscard]] inline std::vector<std::size_t> TopFoes(const std::vector<FoeIn>& a_foes,
                                                           std::size_t a_keep = kTopFoes) {
        std::vector<std::size_t> idx(a_foes.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            const float ra = FoeRelevance(a_foes[a]);
            const float rb = FoeRelevance(a_foes[b]);
            if (ra != rb) return ra > rb;
            return a_foes[a].id < a_foes[b].id;
        });
        if (idx.size() > a_keep) idx.resize(a_keep);
        return idx;
    }

    // ── Heal / buff recipients ───────────────────────────────────────────────────────────────────────────
    // "hurt targets only": below Vocab::kHealFull (0.9995), the same mark the heal road stops at.
    inline constexpr float kHealFull = 0.9995f;
    [[nodiscard]] inline bool Hurt(float a_hpPct) { return std::isfinite(a_hpPct) && a_hpPct < kHealFull; }

    // EFIT area is in FEET; the world is in units (64 units = 1 yard, so 1 foot = 21.333 units).
    inline constexpr float kUnitsPerFoot = 64.0f / 3.0f;
    [[nodiscard]] inline float AreaUnits(float a_areaFeet) {
        return (std::isfinite(a_areaFeet) && a_areaFeet > 0.0f) ? a_areaFeet * kUnitsPerFoot : 0.0f;
    }
    // The radius the never-splash rule and foes-in-area read (I4 review F2): the LARGER of the EFIT area and the
    // explosion radius of the spell's effect / projectile (BGSExplosion radius, already in units). The explosion
    // is what actually deals the area damage of an aimed blast and can exceed the EFIT area; an under-sized
    // radius would let a blast that reaches an ally pass as splash-safe. A broken read counts as 0, never as NaN.
    //
    // MFO-B661 (card I6): the explosion may only ENLARGE a spell that already has an EFIT area, or count on its own
    // when it CARRIES A PAYLOAD (a_explosionPayload: BGSExplosion damage > 0 or an enchantment). A bolt's cosmetic
    // impact explosion (no damage, no enchantment, EFIT area 0) gives the spell NO area: otherwise SplashSafe
    // would veto every cast at a foe a melee ally stands beside. a_explosionUnits is the largest radius among the
    // spell's explosions; a_explosionPayload says whether ANY of them carries a payload.
    [[nodiscard]] inline float BlastUnits(float a_areaFeet, float a_explosionUnits, bool a_explosionPayload) {
        const float area = AreaUnits(a_areaFeet);
        const float ex = (std::isfinite(a_explosionUnits) && a_explosionUnits > 0.0f) ? a_explosionUnits : 0.0f;
        if (!(area > 0.0f) && !a_explosionPayload) return 0.0f;
        return (std::max)(area, ex);
    }

}
