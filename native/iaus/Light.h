#pragma once
// iaus/Light.h -- LIGHT: how lit is this place, who wants the dark, and what light does to being detected (S4, movement-plan.md 3). PURE: standard headers only, no
// engine type. ONE reader for each question, for every consumer (the spots, the cast rows, later cast/Auto.cpp and logistics/Service.cpp). Nothing in the DLL fills
// the inputs yet (G6), and every reader is NOT APPLICABLE without them: UNKNOWN LIGHT IS NEVER DARK.
//
// WHERE A LEVEL COMES FROM. Samples with a place and a time: actors are light meters (the engine's own cached level of an actor in high process), a point sampler if
// probe L1 proves one, the lab's field. Add keeps the freshest one a cell. At answers with the nearest one and how much it is worth (distance and age).
//
// TO ADD A SOURCE: a Source row and whoever calls Add. TO ADD A REASON TO WANT THE DARK: a Combatant field and one line of WantsNow. TO CHANGE A NUMBER: t.light* (Core.h
// Tuning). TO ADD A CONSUMER of a spot's light: MovePass.cpp FillStatic.
#include "iaus/EnvQuery.h"
#include "iaus/Snapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace MFO::Iaus::Light {

    enum class Source : std::uint8_t { Actor = 0, Sampler, Lab };
    struct Sample { Env::Vec3 p; float level = 0.0f; double t = 0.0; Source source = Source::Actor; };
    struct Read { float level = -1.0f, conf = 0.0f; [[nodiscard]] bool Known() const { return level >= 0.0f; } };   // level < 0 = unknown

    inline constexpr float kCellU = 128.0f;             // one sample a cell of this
    inline constexpr std::size_t kFieldSamples = 256;   // memory bound, the oldest goes
    struct Field { std::vector<Sample> samples; };      // in the order they were added (a replaced one keeps its place)

    inline void Add(Field& a_f, const Sample& a_s) {
        if (!std::isfinite(a_s.p.x) || !std::isfinite(a_s.p.y) || !std::isfinite(a_s.p.z) || !std::isfinite(a_s.level) || a_s.level < 0.0f || !std::isfinite(a_s.t)) return;
        const auto cell = [](float v) { return std::floor(v / kCellU); };
        for (Sample& o : a_f.samples)
            if (cell(o.p.x) == cell(a_s.p.x) && cell(o.p.y) == cell(a_s.p.y) && cell(o.p.z) == cell(a_s.p.z)) { if (a_s.t >= o.t) o = a_s; return; }
        if (a_f.samples.size() >= kFieldSamples) a_f.samples.erase(std::min_element(a_f.samples.begin(), a_f.samples.end(), [](const Sample& x, const Sample& y) { return x.t < y.t; }));
        a_f.samples.push_back(a_s);
    }
    inline void Decay(Field& a_f, double a_now, const Tuning& a_t) {
        std::erase_if(a_f.samples, [&](const Sample& s) { return !(a_now - s.t >= 0.0 && a_now - s.t < static_cast<double>(a_t.lightMaxAgeSec)); });
    }
    // The level at a place. Unknown when nothing near and fresh enough says (confidence under t.lightMinConf).
    [[nodiscard]] inline Read At(const Field& a_f, const Env::Vec3& a_p, double a_now, const Tuning& a_t) {
        Read best; float bd = kInf;
        for (const Sample& s : a_f.samples) {
            const float dx = s.p.x - a_p.x, dy = s.p.y - a_p.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            const double age = a_now - s.t;
            if (!(d < a_t.lightNearU) || std::fabs(s.p.z - a_p.z) > a_t.lightStoreyU || !(age >= 0.0 && age < static_cast<double>(a_t.lightMaxAgeSec))) continue;
            const float conf = (1.0f - d / a_t.lightNearU) * (1.0f - static_cast<float>(age) / a_t.lightMaxAgeSec);
            if (conf < a_t.lightMinConf || !(d < bd)) continue;   // the NEAREST one that counts
            bd = d; best = { s.level, conf };
        }
        return best;
    }

    // ── who wants the dark ──
    // Right now: he sneaks (the player too), he is on a hide or a hidden step run, or he is invisible.
    [[nodiscard]] inline bool WantsNow(const Combatant& a_c) { return a_c.sneaking || a_c.hideRun || a_c.invisible; }
    // For a CAST at him: a stealth character always (marth: "simply never cast on stealth characters"), anybody else who wants it now or did a moment ago. The hold is the
    // one hysteresis between the cast and the decast: who stands up for a second is not lit, who crouches is decast at once.
    [[nodiscard]] inline bool WantsDark(const Combatant& a_c, const Tuning& a_t) { return WantsNow(a_c) || a_c.sneakClass || a_c.darkAgoSec < a_t.lightDarkHoldSec; }
    // Whose light of OURS is ended (marth 11-24): a stealth class wants the dark whether he crouches or not, anybody else (the player too) while he wants it now.
    [[nodiscard]] inline bool WantsLightEnded(const Combatant& a_c) { return WantsNow(a_c) || a_c.sneakClass; }
    // Whom a light goes to: 0 the player, 1 a member in his view, 2 not known whether he is, 3 out of it.
    [[nodiscard]] inline float RecipientRank(const Combatant& a_c) { return a_c.isPlayer ? 0.0f : a_c.inPlayerView == Tri::Yes ? 1.0f : a_c.inPlayerView == Tri::Unknown ? 2.0f : 3.0f; }
    // A light cast's TIER (Decide.cpp: a lower one is always first): 0 the player whose light is known, 1 a member whose light is known, 2 and 3 the same where nobody
    // knows how lit he stands. Inside a tier the score says (view and need). Facts::lightRank 0 = the player (RecipientRank).
    inline constexpr int kCastTiers = 4;
    [[nodiscard]] inline int CastTier(const Facts& a_f) { return (a_f.recipientLight >= 0.0f ? 0 : 2) + (a_f.lightRank == 0.0f ? 0 : 1); }
    // A light of OURS that may be ended: cast by this party (the caller asks InParty), never the player's own and never an item's (LiveLight::fromItem).
    [[nodiscard]] inline bool MayEnd(const LiveLight& a_l, std::uint32_t a_playerId) { return a_l.spell != 0 && a_l.caster != 0 && a_l.caster != a_playerId && !a_l.fromItem; }
    [[nodiscard]] inline bool InParty(const Snapshot& a_s, std::uint32_t a_id) { return a_id != 0 && (a_id == a_s.self.id || std::any_of(a_s.allies.begin(), a_s.allies.end(), [&](const Combatant& a) { return a.id == a_id; })); }

    // ── the engine's detection formula ──
    // Moved whole from tools/iaus_sim/Detect.h (source and grades: there, and Docs/research/2026-11/calculate-detection-re.md). The sim includes this one.
    // Values: what Skyrim.esm ships. A holder reads the list's own by name.
    struct DetectGmst {
        float base = -15.0f, skillMult = 0.5f, alertMod = 0.0f, combatMod = -0.4f;
        float percMin = 0.0f, percMax = 100.0f;
        float maxDist = 2500.0f, extDistMult = 2.1f, flyDistMult = 3.0f, attenExp = 2.0f;
        float nonTargetDistMult = 0.75f, losAngle = 60.0f, losMultExt = 0.5f, losMultInt = 0.75f, viewCone = 190.0f;
        float soundsMult = 1.0f, soundLosMult = 0.3f, actionMult = 2.0f, runningMult = 2.0f, weightBase = 12.0f, weightMult = 0.5f;
        float lightMult = 0.33f, lightExtMult = 0.5f, lightMoveMult = 0.01f, lightRunMult = 0.2f, lightMod = 15.0f, lightMax = 300.0f, largeSizeMult = 1.0f;
        float lostTime = 10.0f, lostNoticedDist = 256.0f;
        float barSneak = 70.0f, bar = 25.0f;
        float spMax = 100.0f, spRegenLostWait = 10.0f;
        float updateMin = 1.0f;
        float soundSilent = 10.0f, soundNormal = 50.0f, actionTimer = 1.2f;
    };
    // The numbers DetectLevel reads, in order (the golden dump's line). A new one goes at the end.
    template <class G, class F> void EachFormulaNumber(G& g, F&& f) {
        for (auto* p : { &g.base, &g.skillMult, &g.alertMod, &g.combatMod, &g.percMin, &g.percMax, &g.maxDist, &g.extDistMult, &g.flyDistMult, &g.attenExp, &g.nonTargetDistMult, &g.losAngle, &g.losMultExt, &g.losMultInt,
                         &g.viewCone, &g.soundsMult, &g.soundLosMult, &g.actionMult, &g.runningMult, &g.weightBase, &g.weightMult, &g.lightMult, &g.lightExtMult, &g.lightMoveMult, &g.lightRunMult, &g.lightMod, &g.lightMax,
                         &g.largeSizeMult, &g.barSneak }) f(*p);
    }

    struct DetectIn {      // O = the observer (a foe), T = the target (the follower)
        float dist = 0;
        float angleDeg = 0;         // O's heading to T, 0 = straight ahead
        bool  losGeom = true;       // nothing solid between them (the caller's ray); used only inside the shrunk range
        bool  exterior = false, flying = false;
        bool  inCombatO = true;
        bool  targetIsOsTarget = false;   // T is O's own combat target (else: range x fDetectionCombatNonTargetDistanceMult, alert + fSneakCombatMod)
        bool  oHasTarget = true;
        float skillO = 15, skillT = 15;   // Sneak actor values; skillT already carries perk entry 57
        bool  sneakingT = false, moving = false, running = false, invisible = false;
        float noiseMult = 1;        // T's MovementNoiseMult actor value
        float moveMod = 1;          // perk entry 48 ModDetectionMovement on lightMove (owner T)
        float weight = 0;           // T's equipped weight
        float action = 0;           // T's actionValue: the noise of what he is doing now
        float light = 0;            // T's light level, 0 .. iLightLevelMax (the caller's model of the cached level, after the interior / exterior mod)
    };
    struct DetectOut {
        int   level = -1000;        // the caller's init: the sight road did not run
        bool  ran = false, los = false, inViewCone = false;
        float sound = 0, light = 0, skill = 0, score = 0, range = 0;
    };

    // The light term alone: what a level adds to the score of somebody in view, before distance and movement.
    [[nodiscard]] inline float DetectLightTerm(float a_level, bool a_exterior, const DetectGmst& g) { return std::clamp(a_level, 0.0f, g.lightMax) * (a_exterior ? g.lightExtMult : g.lightMult) + g.lightMod; }

    // Section 3 (the range) then section 4 (the score) of calculate-detection-re.md.
    [[nodiscard]] inline DetectOut DetectLevel(const DetectGmst& g, const DetectIn& in) {
        DetectOut o;
        const float maxDist = g.maxDist * (in.exterior ? g.extDistMult : 1.0f) * (in.flying ? g.flyDistMult : 1.0f);
        float range = maxDist;
        if (in.inCombatO && !in.targetIsOsTarget) range *= g.nonTargetDistMult;   // a non-player target that is not O's own combat target
        o.range = range;
        if (!(in.dist < range)) return o;   // outside the first range nothing of the sight road runs
        o.ran = true;
        float shrunk = range;
        if (in.angleDeg > g.losAngle)
            shrunk *= 1.0f + (in.angleDeg - g.losAngle) / (180.0f - g.losAngle) * ((in.exterior ? g.losMultExt : g.losMultInt) - 1.0f);
        o.los = in.losGeom && in.dist < shrunk;   // the LOS test only inside the shrunk range
        o.inViewCone = o.los && in.angleDeg < 0.5f * g.viewCone;

        const bool busyO = in.oHasTarget && !in.targetIsOsTarget;
        const float alert = 1.0f + (in.inCombatO ? g.alertMod : 0.0f) + (busyO ? g.combatMod : 0.0f);
        const float perception = g.percMin + (g.percMax - g.percMin) * std::clamp(in.skillO, 0.0f, 100.0f) / 100.0f;
        const float distF = std::pow((std::max)(0.0f, (maxDist - in.dist) / maxDist), g.attenExp);

        const float soundF = distF * (o.los ? 1.0f : g.soundLosMult);
        const float moveNoise = (in.moving ? 1.0f : 0.0f) * (std::max)(in.noiseMult, 0.0f) * (g.weightBase + g.weightMult * in.weight);
        o.sound = (std::max)(0.0f, (std::trunc(g.actionMult) * in.action + (in.running ? g.runningMult : 1.0f) * moveNoise) * g.soundsMult * soundF);

        const float lightMove = (1.0f + g.lightMoveMult * (in.moving ? 1.0f : 0.0f) + g.lightRunMult * (in.running ? 1.0f : 0.0f)) * in.moveMod;
        const float lightBase = DetectLightTerm(in.light, in.exterior, g);
        o.light = (std::max)(0.0f, (o.inViewCone && !in.invisible ? 1.0f : 0.0f) * distF * lightMove * lightBase * g.largeSizeMult);

        o.skill = (distF * alert * perception - (in.sneakingT ? in.skillT : 0.0f)) * g.skillMult;
        o.score = g.base + o.sound + o.light + o.skill;
        o.level = o.score >= 1.0f ? static_cast<int>(o.score) : o.score > 0.0f ? 1 : static_cast<int>(o.score);
        if (o.level == 0) o.level = -1;   // CalculateDetection: a result of exactly 0 becomes -1
        return o;
    }
}
