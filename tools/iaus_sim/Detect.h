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
#include "iaus/Light.h"

#include <algorithm>
#include <cmath>

namespace Sim::Detect {

    // The formula itself is the game's own pure code (native/iaus/Light.h, S4): one reader for the lab, the spots and G6.
    using Gmst = MFO::Iaus::Light::DetectGmst;
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

    using In = MFO::Iaus::Light::DetectIn;
    using Out = MFO::Iaus::Light::DetectOut;
    [[nodiscard]] inline Out Calculate(const Gmst& g, const In& in) { return MFO::Iaus::Light::DetectLevel(g, in); }

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
