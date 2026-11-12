#pragma once
// iaus_sim/Detect.h -- THE ENGINE'S DETECTION FORMULA as a pure function (sim v2 stealth lab). HOST-ONLY.
//
// Source: Docs/research/2026-11/calculate-detection-re.md (CalculateDetection, AE 37774; the sight / stealth score AE 26365;
// the range AE 26364; the combat group's use of the level, UpdateTargetDetection 0x805090). Every term below is the card's,
// in the card's order. Grades are the card's too: DISASM for the formula and the range, LIKELY for the view-cone byte of the
// immediate-loss road. NOTHING here is FIELD yet (probes P1-P3 of g2-hide-design.md): the numbers are a faithful reading of
// the disassembly, not an observation of the game.
//
// GAME SETTINGS are read by NAME from the list's own data (Data::G) and fall back to the values SKYRIM.ESM ships. Those are
// not the executable's image defaults the card quotes (the card says so: "a plugin can override any GMST"): Skyrim.esm itself
// overrides many of them (fSneakBaseValue -15 not -25, fSneakMaxDistance 2500 not 1500, fSneakLightMult 0.33 not 1.4,
// fSneakAlertMod 0 not 0.2, fSneakActionMult 2 not 1, fDetectionSneakLightMod 15 not 0, iLightLevelMax 300 not 100).
//
// NOT MODELLED (stated, each a row of the doc's model table): the noise road and the projectile road of CalculateDetection
// (section 2 roads 2 and 3: their scorer AE 26366 was read by its strings only; a positive level there stores the NOISE
// POINT, not his position), NightEye, blindness, sleep, ambush packages, the race size class (1.0), perk entries 47 and 15.
#include "iaus_sim/Data.h"

#include <algorithm>
#include <cmath>

namespace Sim::Detect {

    struct Gmst {
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
    inline Gmst Load(const Data& D) {
        Gmst g; auto f = [&](const char* k, float d) { return static_cast<float>(D.G(k, d)); };
        g.base = f("fSneakBaseValue", g.base); g.skillMult = f("fSneakSkillMult", g.skillMult); g.alertMod = f("fSneakAlertMod", g.alertMod); g.combatMod = f("fSneakCombatMod", g.combatMod);
        g.percMin = f("fSneakPerceptionSkillMin", g.percMin); g.percMax = f("fSneakPerceptionSkillMax", g.percMax);
        g.maxDist = f("fSneakMaxDistance", g.maxDist); g.extDistMult = f("fSneakExteriorDistanceMult", g.extDistMult); g.flyDistMult = f("fSneakFlyingDistanceMult", g.flyDistMult); g.attenExp = f("fSneakDistanceAttenuationExponent", g.attenExp);
        g.nonTargetDistMult = f("fDetectionCombatNonTargetDistanceMult", g.nonTargetDistMult); g.losAngle = f("fDetectionLOSDistanceAngle", g.losAngle);
        g.losMultExt = f("fDetectionLOSDistanceMultExterior", g.losMultExt); g.losMultInt = f("fDetectionLOSDistanceMultInterior", g.losMultInt); g.viewCone = f("fDetectionViewCone", g.viewCone);
        g.soundsMult = f("fSneakSoundsMult", g.soundsMult); g.soundLosMult = f("fSneakSoundLosMult", g.soundLosMult); g.actionMult = f("fSneakActionMult", g.actionMult); g.runningMult = f("fSneakRunningMult", g.runningMult);
        g.weightBase = f("fSneakEquippedWeightBase", g.weightBase); g.weightMult = f("fSneakEquippedWeightMult", g.weightMult);
        g.lightMult = f("fSneakLightMult", g.lightMult); g.lightExtMult = f("fSneakLightExteriorMult", g.lightExtMult); g.lightMoveMult = f("fSneakLightMoveMult", g.lightMoveMult); g.lightRunMult = f("fSneakLightRunMult", g.lightRunMult);
        g.lightMod = f("fDetectionSneakLightMod", g.lightMod); g.lightMax = f("iLightLevelMax", g.lightMax); g.largeSizeMult = f("fDetectionLargeActorSizeMult", g.largeSizeMult);
        g.lostTime = f("fCombatDetectionLostTimeLimit", g.lostTime); g.lostNoticedDist = f("fCombatDetectionLostCheckNoticedDistance", g.lostNoticedDist);
        g.barSneak = f("iCombatStealthPointSneakDetectionThreshold", g.barSneak); g.bar = f("iCombatStealthPointDetectionThreshold", g.bar);
        g.spMax = f("fCombatStealthPointMax", g.spMax); g.spRegenLostWait = f("fCombatStealthPointRegenLostWaitTime", g.spRegenLostWait);
        g.updateMin = std::clamp(f("fDetectionUpdateTimeMin", g.updateMin), 0.1f, 5.0f);
        g.soundSilent = f("iSoundLevelSilent", g.soundSilent); g.soundNormal = f("iSoundLevelNormal", g.soundNormal); g.actionTimer = f("fDetectionActionTimer", g.actionTimer);
        return g;
    }

    struct In {            // O = the observer (a foe), T = the target (the follower)
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
    struct Out {
        int   level = -1000;        // the caller's init: the sight road did not run
        bool  ran = false, los = false, inViewCone = false;
        float sound = 0, light = 0, skill = 0, score = 0, range = 0;
    };

    // Section 3 (the range) then section 4 (the score).
    [[nodiscard]] inline Out Calculate(const Gmst& g, const In& in) {
        Out o;
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
        const float distF = std::pow(std::max(0.0f, (maxDist - in.dist) / maxDist), g.attenExp);

        const float soundF = distF * (o.los ? 1.0f : g.soundLosMult);
        const float moveNoise = (in.moving ? 1.0f : 0.0f) * std::max(in.noiseMult, 0.0f) * (g.weightBase + g.weightMult * in.weight);
        o.sound = std::max(0.0f, (std::trunc(g.actionMult) * in.action + (in.running ? g.runningMult : 1.0f) * moveNoise) * g.soundsMult * soundF);

        const float lightMove = (1.0f + g.lightMoveMult * (in.moving ? 1.0f : 0.0f) + g.lightRunMult * (in.running ? 1.0f : 0.0f)) * in.moveMod;
        const float lightBase = std::clamp(in.light, 0.0f, g.lightMax) * (in.exterior ? g.lightExtMult : g.lightMult) + g.lightMod;
        o.light = std::max(0.0f, (o.inViewCone && !in.invisible ? 1.0f : 0.0f) * distF * lightMove * lightBase * g.largeSizeMult);

        o.skill = (distF * alert * perception - (in.sneakingT ? in.skillT : 0.0f)) * g.skillMult;
        o.score = g.base + o.sound + o.light + o.skill;
        o.level = o.score >= 1.0f ? static_cast<int>(o.score) : o.score > 0.0f ? 1 : static_cast<int>(o.score);
        if (o.level == 0) o.level = -1;   // CalculateDetection: a result of exactly 0 becomes -1
        return o;
    }

    // ── sneak-attack and detection PROFILES: synthetic TEST INPUT for the built-in fixture world (--sneak-profile) ──
    // A profile is laid over the Data the run loaded. `vanilla` is what Skyrim.esm ships (the three sneak-attack perks and the
    // Stealth ranks); it is the built-in world's default. `steep` is a LoreRim-LIKE profile: the SHAPE of a Requiem-style list
    // (a flatter skill duel, light and movement count for more, a narrower view cone, a dagger tree up to x4 on the weapon's
    // x3, a late bow perk). It is lab data so that a tuned curve can be checked against a list where a sneak attack is worth far
    // more and hiding is harder. PROVENANCE (MFO-B785): its game-setting values are EQUAL to the cached LoreRim extract, typed in
    // here. Read it as "LoreRim's settings", not as a hand-made shape. It is not read from a list's files at run time, and
    // nothing in native/ sees it (the no-list-specific-data rule is about native/ logic, which this never reaches).
    // With --list the list's own values stand unless a profile is asked for by name.
    inline bool ApplyProfile(Data& D, const std::string& name) {
        auto rows = [](std::initializer_list<StealthRow> r) { return std::vector<StealthRow>(r); };
        if (name == "vanilla") {
            D.sneakAtk.clear();
            D.sneakAtk["bow"] = rows({ { 15, 40.0f, 1.5f, "DeadlyAim" } });
            D.sneakAtk["dagger"] = rows({ { 15, 50.0f, 2.5f, "AssassinsBlade" }, { 15, 30.0f, 2.0f, "Backstab" } });
            D.sneakAtk["oneHand"] = rows({ { 15, 30.0f, 2.0f, "Backstab" } });
            D.sneakSkillRows = rows({ { 15, 0.0f, 20.0f, "Stealth" }, { 15, 20.0f, 25.0f, "Stealth" }, { 15, 40.0f, 30.0f, "Stealth" }, { 15, 60.0f, 35.0f, "Stealth" }, { 15, 80.0f, 40.0f, "Stealth" } });
            D.sneakNoiseRows.clear();
            return true;
        }
        if (name == "steep") {
            for (const auto& kv : std::initializer_list<std::pair<const char*, double>>{ { "fSneakBaseValue", -7.5 }, { "fSneakSkillMult", 0.425 }, { "fSneakLightMult", 0.5 }, { "fSneakLightMoveMult", 0.5 },
                     { "fSneakSoundsMult", 0.7 }, { "fSneakSoundLosMult", 0.25 }, { "fSneakEquippedWeightMult", 0.15 }, { "fDetectionViewCone", 160.0 }, { "fDetectionSneakLightMod", 10.0 },
                     { "fCombatStealthPointMax", 80.0 }, { "fCombatStealthPointRegenLostWaitTime", 30.0 }, { "fDetectionUpdateTimeMin", 0.1 }, { "fCombatSneakHandMult", 3.0 } })
                D.gmst[kv.first] = kv.second;
            D.sneakAtk.clear();
            D.sneakAtk["dagger"] = rows({ { kAvOneHanded, 25.0f, 2.0f, "DaggerFocus" }, { kAvOneHanded, 50.0f, 3.0f, "DaggerFocus" }, { kAvOneHanded, 75.0f, 4.0f, "DaggerFocus" } });
            D.sneakAtk["bow"] = rows({ { 15, 75.0f, 2.0f, "Anatomy" } });
            D.sneakSkillRows = rows({ { 15, 20.0f, 30.0f, "Stealth" }, { 15, 25.0f, 15.0f, "Muffled" }, { 15, 75.0f, 10.0f, "Acrobatics" }, { 15, 100.0f, 25.0f, "Shadowrunner" } });
            D.sneakNoiseRows = rows({ { 15, 100.0f, 0.5f, "Shadowrunner" } });
            return true;
        }
        return name == "list";   // the loaded data as it is
    }

}
