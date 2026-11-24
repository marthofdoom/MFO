// iaus_sim/sim.cpp -- OFFLINE IAUS combat simulator, v2 (a game-faithful world). HOST-ONLY, never part of the DLL.
//
// Drives the REAL pure layers (iaus/Core.h: Decide / DecidePosture / OnFired; iaus/Generate.h: Generate) inside a
// world built from the installed list's own records and game settings (tools/list_balance: extract_world.py ->
// compile_world.py -> ~/.cache/mfo-list-balance/<list>.sim.json). List data is TEST INPUT only; nothing in native/
// reads it. Models, sources and proof levels: Docs/research/2026-11/iaus-sim-v2.md.
#include "iaus_sim/Results.h"
#include "iaus_sim/Scenarios.h"
#include "iaus_sim/Rating.h"
#include "iaus_sim/Tune.h"
#include "iaus_sim/StealLab.h"
#include "iaus_sim/PotionWorth.h"
#include "iaus_sim/Logistics.h"
#include "iaus_sim/EnvTest.h"     // SIM V4: --env-test
#include "iaus_sim/LootCli.h"     // SIM V4 batch L: looting in the fight world (--loot-*)
#include "iaus/Iaus_internal.h"   // detail::SetFor: which considerations a family reads (--list-uses, for the per-class curve search)

#include <atomic>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <thread>

using namespace Sim;

namespace {

    bool g_guardWhy = false;
    bool g_postureReport = false;   // --posture-report: what he holds and how close the foes on him come (the archer study, batch 3); metrics pr.* and a POSTURE section
    bool g_objEstimates = true, g_objScrollRoad = false, g_tableMode = false;
    bool g_oracleMode = false; OracleCfg g_oc;   // --headline-oracle: the objective is played by the rollout oracle over the current IAUS
    void RunMode(Fight& F, bool timeIt = false) { if (g_oracleMode) { OraclePolicy O; O.cfg = g_oc; RunFight(F, O); } else if (g_tableMode) { TablePolicy P; RunFight(F, P); } else { IausPolicy P; P.timeIt = timeIt; RunFight(F, P); } }

    struct Agg {   // a cell's tallies, summed
        Tally s; long fights = 0, wins = 0, losses = 0, playerDeaths = 0, forced = 0, timeouts = 0;
        double durSum = 0, firstKillSum = 0; long firstKillN = 0; double uSum = 0, pMinSum = 0, mpLateSum = 0; long mpLateN = 0, fightsWithDown = 0, foes = 0;
        double minTgtSum = 0; long minTgtN = 0;   // sim v3: the nearest the subject came to his target, per fight
        void Add(const Tally& t, bool forcedBracket) {
            ++fights; wins += t.won; losses += t.lost; playerDeaths += t.playerDied; forced += forcedBracket; timeouts += !t.won && !t.lost; durSum += t.dur;
            if (t.firstKill >= 0) { firstKillSum += t.firstKill; ++firstKillN; }
            fightsWithDown += t.downs > 0; foes += t.foesFaced;
            pMinSum += t.playerMinHp; if (t.mpAtLate >= 0) { mpLateSum += t.mpAtLate; ++mpLateN; }
#define S(f) s.f += t.f
            S(ticks); S(idle); S(hold); S(act); S(fired); S(switches); S(badFacts); S(invalid); S(postureSwaps);
            for (std::size_t i = 0; i < kFamilies; ++i) { s.hist[i] += t.hist[i]; s.actSec[i] += t.actSec[i]; }
            for (std::size_t i = 0; i < 10; ++i) { s.bodySec[i] += t.bodySec[i]; s.bodyIausSec[i] += t.bodyIausSec[i]; } S(upSec);
            S(lowEpisodes); S(lowCandEpisodes); S(lowHealed); S(lowCandTicks); S(lowHoldTicks); S(lowHealSecSum);
            S(noCandTicks); S(candTicks); S(idleCand); S(reachTicks); S(idleReach); S(taunts); S(tauntRedundant); S(tauntFail);
            S(dSum); S(dN); s.dMax = std::max(s.dMax, t.dMax);
            S(downs); S(partyDowns); S(dmgDone); S(dmgTaken); S(partyDmgTaken); S(playerDmgTaken); S(healDone);
            S(mpSum); S(hpSum); S(spSum); S(resN); S(mpDrySec);
            S(overkill); S(iausDmg); S(overheal); S(healTotal); S(healCasts); S(doubleHeals);
            S(foeCasts); S(foeCastsInterrupted); S(interruptsByFollowers);
            S(elemCasts); S(resistedCasts); S(weakCasts); S(weakOpp); S(immuneCasts); S(splashAllyHits); S(areaCasts);
            S(ctrlCasts); S(ctrlFails); S(ctrlOnControlled); S(hitsOnControlled); S(powOnControlled); S(powerAttacks); S(powerWhiffs); S(inc2); S(guard2); for (int k = 0; k < 6; ++k) S(ung2[k]); S(regroups); S(grounds); S(blocks); S(bashes);
            S(playerPressureSec); S(peelSum); S(peelN); S(peelMiss); S(tgtSwitches); S(tauntsOutmatched); S(tauntThenDown); S(tauntSaves);
            S(arrowsShot); S(arrowsHit); S(dodgeTries); S(dodges); S(aimCorrected); S(blockRenewals); S(powRefused); S(backOffs); S(backOffArrived); S(steps); S(stepArrived); S(stepRevealed); S(averted); S(avertKept); S(raises); S(mixPowerDenied); S(mixNormalDenied); S(mixFloorLifted); S(leashTurns); S(engPower); S(dndRuns); S(guardSec); S(feetSec); S(runsBegun); S(runsSafe); S(runDmg); S(runSafeSec); S(runLenSec); S(runDmgAfter5); S(wpnHits); S(sneakHits); S(flankHits); S(hides); S(hideOk); S(hideFail); S(flees); S(fleeHealed); S(fleeArrived); S(fleeFail); S(runDowns); S(stealthTry); S(stealthMiss); S(runSec); S(holdSec); S(fleeHpGain); S(firstHits); S(sneakOpens); S(healAtN); S(sneakDmg); S(hiddenSec); S(sneakSec); S(tgtDistSum); S(playerDistSum); S(contactSec); S(goInBadSec); S(summonSec); S(undeadDmg); S(healAtSum); S(cloakSec); S(posN); S(tgtDistN);
            for (std::size_t i = 0; i < 10; ++i) { s.decBandSec[i] += t.decBandSec[i]; s.decBandLoss[i] += t.decBandLoss[i]; s.decLethal[i] += t.decLethal[i]; s.decToDownSec[i] += t.decToDownSec[i]; s.decCross[i] += t.decCross[i]; s.decDownAfter[i] += t.decDownAfter[i]; }
            S(drinksH); S(downsWithPotH); S(downsMidDrink); S(potFracN); S(drinkAtSum); S(potHeal); S(potWaste); S(potFracSum);
            for (int k = 0; k < 4; ++k) S(drawnSec[k]); for (int k = 0; k < 3; ++k) S(nearOnMeSec[k]); S(drawSpoiled); S(pointBlankShots);
            // sim v3
            S(castPick); S(castFired); S(castNoLos); S(castLofHeld); S(castLofExpired); S(castNotBuilt); S(castDirect); S(castThrash); S(castLost); S(beats); S(beatLof); S(beatNoLos); S(streams); S(castLofSec); S(claimSec); S(streamSec);
            S(capLaps); S(capDropLaps); S(capStarveLaps); S(capWaived); S(capSum); S(inc); S(incGuard); S(incArc); S(incDown); S(handLostSec); S(handLostDmg); S(reDraws);
            S(castUnfired); S(castRepeat); S(castDeferred); S(healFullExit); S(unfiredSec); S(capKept); S(selfHealN); S(selfHealLowN); S(healLateN); S(selfHealAtSum); S(healYield); S(firstHitN); S(firstHitSum); S(offPickN); S(offPickDmg); S(offPickCost); S(healPickN); S(healPickSize); S(healPickConc); S(healPickSelf);   // sim mirror batch 11-17
            S(areaUpSec); S(areaFoeSec); S(areaAllySec); S(areaOutSec); S(areaRecast); S(areaOwnCasts); S(strafes);
            if (t.minTgtDist < 1e8) { minTgtSum += t.minTgtDist; ++minTgtN; }
            // sim v4
            S(pathLegs); S(pathUnreach); S(pathBlocked); S(pathReblock); S(pathStuckLegs); S(pathOsc); S(pathDrops); S(pathDoors); S(pathPlans); S(detourN); for (int k = 0; k < 16; ++k) S(detourHist[k]);
            S(pathStuckSec); S(pathHoldUnreachSec); S(pathWalked); S(detourSum); S(envBad); S(envStuckForever); S(pathAwaySec); S(pathCorneredSec); S(castHeldTurns); S(pathHoldSec); S(pathRepeat); S(castZeroFires); S(castZeroHeld);
            S(castUnseated); S(castNeverShort); S(castNeverLong); S(castHeldOff); S(castInterrupted); S(foeCastInterrupted); S(castOccRelease); S(neverSec); S(apprDrops); S(apprBlocked);
            S(deathsWithHeal); S(retreatSec); S(kiteDmgTaken); S(wastedCasts); S(hitsWhileCtl); S(wwDeclined); S(scrollsUsed); S(extActions); S(moveOrderSec);
#undef S
            // kill order: a priority foe (healer / caster / archer) that dies before every grunt is "good"
            bool gruntDead = false; for (const std::string& k : t.killOrder) { if (k == "grunt" || k == "boss") gruntDead = true; else (gruntDead ? s.killOrderBad : s.killOrderGood)++; }
        }
    };

    std::map<std::string, double> Metrics(const Agg& a) {
        const Tally& s = a.s; std::map<std::string, double> m;
        const double t = std::max<double>(1, double(s.ticks)), f = std::max<double>(1, double(s.fired)), secs = std::max(1e-9, a.durSum), mins = secs / 60.0, n = std::max<double>(1, double(a.fights));
        auto pct = [](double x, double y) { return y > 0 ? 100.0 * x / y : -1.0; };
        m["idle%"] = 100.0 * double(s.idle) / t; m["hold%"] = 100.0 * double(s.hold) / t; m["sw/min"] = double(s.switches) / mins;
        m["heal.s"] = s.lowHealed ? s.lowHealSecSum / double(s.lowHealed) : -1; m["healed%"] = pct(double(s.lowHealed), double(s.lowCandEpisodes));
        m["noHealCand%"] = pct(double(s.lowEpisodes - s.lowCandEpisodes), double(s.lowEpisodes)); m["holdLow%"] = pct(double(s.lowHoldTicks), double(s.lowCandTicks));
        m["bad"] = double(s.badFacts); m["invalid"] = double(s.invalid); m["dec.us"] = s.dSum / std::max<double>(1, double(s.dN)); m["dec.max"] = s.dMax;
        m["win%"] = 100.0 * double(a.wins) / n; m["loss%"] = 100.0 * double(a.losses) / n; m["pdead%"] = 100.0 * double(a.playerDeaths) / n; m["timeout%"] = 100.0 * double(a.timeouts) / n;
        m["downs/f"] = double(s.downs) / n; m["nocand%"] = 100.0 * double(s.noCandTicks) / t;
        m["iC%"] = pct(double(s.idleCand), double(s.candTicks)); m["iR%"] = pct(double(s.idleReach), double(s.reachTicks));
        m["taunt/min"] = double(s.taunts) / mins; m["tR%"] = s.taunts ? 100.0 * double(s.tauntRedundant) / double(s.taunts) : 0.0;
        { double as = 0; for (std::size_t i = 0; i < kFamilies; ++i) as += s.actSec[i]; as = std::max(1e-9, as);
          for (std::size_t i = 0; i < kFamilies; ++i) if (s.hist[i]) m[std::string("a.") + std::string(Name(Family(i)))] = 100.0 * s.actSec[i] / as; }
        { const double up = std::max(1e-9, s.upSec); auto bs = [&](Act a) { return 100.0 * s.bodySec[static_cast<std::size_t>(a)] / up; }; auto bi = [&](Act a) { return 100.0 * s.bodyIausSec[static_cast<std::size_t>(a)] / up; };
          m["b.melee"] = bs(Act::Swing) + bs(Act::Power) + bs(Act::Bash); m["b.power"] = bs(Act::Power); m["b.shoot"] = bs(Act::Shoot); m["b.cast"] = bs(Act::Cast); m["b.castIaus"] = bi(Act::Cast);
          m["b.block"] = bs(Act::Block); m["b.other"] = bs(Act::Drink) + bs(Act::Swap) + bs(Act::Taunt); m["b.free"] = bs(Act::None); (void)f; }
        m["posture/min"] = double(s.postureSwaps) / mins;
        // absolute outcomes
        m["dur.s"] = a.durSum / n; m["ttfk.s"] = a.firstKillN ? a.firstKillSum / double(a.firstKillN) : -1;
        m["dps"] = s.dmgDone / secs; m["taken/s"] = s.dmgTaken / secs; m["pTaken/s"] = s.playerDmgTaken / secs;
        m["mp.avg%"] = s.resN ? 100.0 * s.mpSum / double(s.resN) : -1; m["hp.avg%"] = s.resN ? 100.0 * s.hpSum / double(s.resN) : -1; m["dry%"] = 100.0 * s.mpDrySec / secs;
        m["forced%"] = 100.0 * double(a.forced) / n;
        // intelligence
        m["overkill%"] = pct(s.overkill, s.iausDmg); m["overheal%"] = pct(s.overheal, s.healTotal); m["dblheal%"] = pct(double(s.doubleHeals), double(s.healCasts));
        m["intr%"] = pct(double(s.interruptsByFollowers), double(s.foeCasts)); m["resisted%"] = pct(double(s.resistedCasts), double(s.elemCasts));
        m["immune%"] = pct(double(s.immuneCasts), double(s.elemCasts)); m["weak%"] = pct(double(s.weakCasts), double(s.elemCasts));
        m["splash%"] = pct(double(s.splashAllyHits), double(s.areaCasts)); m["ctrlFail%"] = pct(double(s.ctrlFails), double(s.ctrlCasts)); m["ctrlDup%"] = pct(double(s.ctrlOnControlled), double(s.ctrlCasts));
        m["peel.s"] = s.peelN ? s.peelSum / double(s.peelN) : -1; m["press%"] = 100.0 * s.playerPressureSec / secs;
        m["prio%"] = pct(double(s.killOrderGood), double(s.killOrderGood + s.killOrderBad)); m["waste%"] = pct(double(s.wastedCasts), double(s.fired));
        m["tsw/min"] = double(s.tgtSwitches) / mins; m["dwh/f"] = double(s.deathsWithHeal) / n; m["tntDown%"] = pct(double(s.tauntThenDown), double(s.taunts));
        m["down%"] = 100.0 * double(a.fightsWithDown) / n; m["takenPerFoe"] = a.foes ? s.dmgTaken / double(a.foes) : -1;
        m["U"] = a.uSum / n; m["pMinHp%"] = 100.0 * a.pMinSum / n; m["mpAtLate%"] = a.mpLateN ? 100.0 * a.mpLateSum / double(a.mpLateN) : -1; m["splashHits/f"] = double(s.splashAllyHits) / n;
        m["ctlFocus%"] = pct(double(s.hitsOnControlled), double(s.hitsWhileCtl)); m["retreat%"] = 100.0 * s.retreatSec / secs;
        m["wwDecl/min"] = double(s.wwDeclined) / mins; m["ext/f"] = double(s.extActions) / n; m["scroll/f"] = double(s.scrollsUsed) / n; m["move%"] = 100.0 * s.moveOrderSec / secs;
        // the class playstyle lab: stealth, position, the class rules (iaus-classes.md "Metrics")
        { const double up = std::max(1e-9, s.upSec);
          m["sneak%"] = 100.0 * s.sneakSec / up; m["hidden%"] = 100.0 * s.hiddenSec / up; m["sneakAtk%"] = pct(double(s.sneakHits), double(s.wpnHits)); m["sneakDmg%"] = pct(s.sneakDmg, s.dmgDone);
          m["opener%"] = pct(double(s.sneakOpens), double(s.firstHits)); m["flank%"] = pct(double(s.flankHits), double(s.wpnHits)); m["hide/min"] = double(s.hides) / mins;
          // card G3 / G2: hide runs that ended hidden, runs that failed, flees per fight and how they ended, the blows MFO's group read called
          // "unaware" that the engine's per-pair level refused the multiplier, his time on a run and in the hold row
          m["hideOk%"] = pct(double(s.hideOk), double(s.hides)); m["hideFail%"] = pct(double(s.hideFail), double(s.hides)); m["flee/f"] = double(s.flees) / n; m["fleeHealed%"] = pct(double(s.fleeHealed), double(s.flees)); m["fleeFail%"] = pct(double(s.fleeFail), double(s.flees));
          // T2 batch S (section 31)
          m["dodge%"] = pct(double(s.dodges), double(s.dodgeTries)); m["dodge/f"] = double(s.dodges) / n; m["aimFix%"] = pct(double(s.aimCorrected), double(s.arrowsShot)); m["arrowHit%"] = pct(double(s.arrowsHit), double(s.arrowsShot));
          m["guard2%"] = pct(double(s.guard2), double(s.inc2)); { static const char* kn[6] = { "ung2stagger%", "ung2rest%", "ung2hurt%", "ung2drink%", "ung2busy%", "ung2free%" }; if (g_guardWhy) for (int k = 0; k < 6; ++k) m[kn[k]] = pct(double(s.ung2[k]), double(s.inc2)); } m["inc2/f"] = double(s.inc2) / n; m["guard%"] = 100.0 * s.guardSec / up; m["block/f"] = double(s.blocks) / n; m["powRefused/f"] = double(s.powRefused) / n; m["backOff/f"] = double(s.backOffs) / n; m["backOffOk%"] = pct(double(s.backOffArrived), double(s.backOffs));
          m["step/f"] = double(s.steps) / n; m["stepOk%"] = pct(double(s.stepArrived), double(s.steps)); m["stepSeen%"] = pct(double(s.stepRevealed), double(s.steps)); m["averted/f"] = double(s.averted) / n; m["avertKept/f"] = double(s.avertKept) / n;
          m["raise/f"] = double(s.raises) / n; m["mixPowDeny/f"] = double(s.mixPowerDenied) / n; m["mixNormDeny/f"] = double(s.mixNormalDenied) / n; m["mixLift/f"] = double(s.mixFloorLifted) / n; m["leashTurn/f"] = double(s.leashTurns) / n; m["engPower/f"] = double(s.engPower) / n; m["feet%"] = 100.0 * s.feetSec / up;
          m["run/f"] = double(s.runsBegun) / n; m["runSafe%"] = pct(double(s.runsSafe), double(s.runsBegun)); m["runSafe.s"] = s.runsSafe ? s.runSafeSec / double(s.runsSafe) : -1; m["runLen.s"] = s.runsBegun ? s.runLenSec / double(s.runsBegun) : -1;
          m["runDmg%"] = s.runsBegun ? 100.0 * s.runDmg / double(s.runsBegun) : -1; m["runDmgLate%"] = s.runsBegun ? 100.0 * s.runDmgAfter5 / double(s.runsBegun) : -1;   // the share of his health he lost per G3 run (and after its 5th second)
          m["fleeHpGain%"] = s.flees ? 100.0 * s.fleeHpGain / double(s.flees) : -1; m["stealthMiss%"] = pct(double(s.stealthMiss), double(s.stealthTry)); m["runDown%"] = pct(double(s.runDowns), double(s.hides + s.flees)); m["run%"] = 100.0 * s.runSec / up; m["still%"] = 100.0 * s.holdSec / up;
          if (g_postureReport) { m["pr.bow%"] = 100.0 * s.drawnSec[0] / up; m["pr.melee%"] = 100.0 * s.drawnSec[1] / up; m["pr.on256%"] = 100.0 * s.nearOnMeSec[0] / up; m["pr.on540%"] = 100.0 * (s.nearOnMeSec[0] + s.nearOnMeSec[1]) / up;
              m["pr.spoiled/f"] = double(s.drawSpoiled) / n; m["pr.pointBlank%"] = pct(double(s.pointBlankShots), double(s.arrowsShot)); }
          m["tgtDist"] = s.tgtDistN ? s.tgtDistSum / double(s.tgtDistN) : -1; m["pDist"] = s.posN ? s.playerDistSum / double(s.posN) : -1; m["contact%"] = 100.0 * s.contactSec / up;
          m["miss%"] = s.arrowsShot >= 5 ? 100.0 * double(s.arrowsShot - s.arrowsHit) / double(s.arrowsShot) : -1; m["powShare%"] = s.bodySec[static_cast<std::size_t>(Act::Swing)] + s.bodySec[static_cast<std::size_t>(Act::Power)] > 1.0 ? 100.0 * s.bodySec[static_cast<std::size_t>(Act::Power)] / (s.bodySec[static_cast<std::size_t>(Act::Swing)] + s.bodySec[static_cast<std::size_t>(Act::Power)]) : -1;
          m["goInBad%"] = 100.0 * s.goInBadSec / up; m["summonUp%"] = 100.0 * s.summonSec / up; m["healAt%"] = s.healAtN ? 100.0 * s.healAtSum / double(s.healAtN) : -1; m["healLate%"] = s.healAtN ? 100.0 * double(s.healLateN) / double(s.healAtN) : -1; m["selfHealAt%"] = s.selfHealN ? 100.0 * s.selfHealAtSum / double(s.selfHealN) : -1; m["selfHealLow%"] = s.selfHealN ? 100.0 * double(s.selfHealLowN) / double(s.selfHealN) : -1; m["undeadDmg%"] = pct(s.undeadDmg, s.dmgDone); }
        // sim batch (section 32): the drink record (the full decline curve is the --decline report)
        m["drinkH/f"] = double(s.drinksH) / n; m["drinkAt%"] = s.drinksH ? 100.0 * s.drinkAtSum / double(s.drinksH) : -1; m["potWaste%"] = pct(s.potWaste, s.potHeal); m["dnPot/f"] = double(s.downsWithPotH) / n; m["dnMidDrink/f"] = double(s.downsMidDrink) / n;
        // SIM V3 (iaus-sim-v3.md): what became of the casts his decisions fired, the chase cap, the guard by direction, the weapon hand, the areas.
        // fired% = released / picked. landed% = released at a target he could see / picked. Each cause is a share of the picks.
        { const double pk = double(s.castPick);
          m["dl.pick/f"] = pk / n; m["dl.fired%"] = pct(double(s.castFired), pk); m["dl.landed%"] = pct(double(s.castFired - s.castNoLos), pk); m["dl.noLos%"] = pct(double(s.castNoLos), pk);
          m["dl.lofHeld%"] = pct(double(s.castLofHeld), pk); m["dl.lofExpired%"] = pct(double(s.castLofExpired), pk); m["dl.lofHold.s"] = s.castLofHeld ? s.castLofSec / double(s.castLofHeld) : -1;
          m["dl.notBuilt%"] = pct(double(s.castNotBuilt), pk); m["dl.direct%"] = pct(double(s.castDirect), pk); m["dl.thrash%"] = pct(double(s.castThrash), pk); m["dl.lost%"] = pct(double(s.castLost), pk);
          m["dl.unfired%"] = pct(double(s.castUnfired), pk); m["dl.repeat%"] = pct(double(s.castRepeat), pk); m["dl.unfired.s"] = s.castUnfired ? s.unfiredSec / double(s.castUnfired) : -1; m["dl.deferred/f"] = double(s.castDeferred) / n; m["dl.healFull/f"] = double(s.healFullExit) / n;   // sim mirror batch 11-17 (Data.h Delivery, cast/CastOn.cpp deferrals, cast/HealRoad.cpp full exit)
          m["tfh.s"] = s.firstHitN ? s.firstHitSum / double(s.firstHitN) : -1; m["nohit%"] = 100.0 * (1.0 - double(s.firstHitN) / n); m["hits/f"] = double(s.wpnHits) / n;   // the field scenarios (11-17): time to his first damage, fights with none, weapon hits
          m["cast.dmg"] = s.offPickN ? s.offPickDmg / double(s.offPickN) : -1; m["cast.cost"] = s.offPickN ? s.offPickCost / double(s.offPickN) : -1; m["off/f"] = double(s.offPickN) / n;
          m["heal.size"] = s.healPickN ? s.healPickSize / double(s.healPickN) : -1; m["heal.conc%"] = pct(double(s.healPickConc), double(s.healPickN)); m["heal.self%"] = pct(double(s.healPickSelf), double(s.healPickN)); m["heal/f"] = double(s.healPickN) / n; m["healYield/f"] = double(s.healYield) / n;
          m["cap.kept/f"] = double(s.capKept) / n;   // foes kept past the chase cap as noApproach (11-17 A3)
          m["dl.claim.s"] = s.castFired ? s.claimSec / double(s.castFired) : -1; m["dl.beatLof%"] = pct(double(s.beatLof), double(s.beats + s.beatLof + s.beatNoLos)); m["dl.stream.s"] = s.streams ? s.streamSec / double(s.streams) : -1;
          m["cap.mean"] = s.capLaps ? s.capSum / double(s.capLaps) : -1; m["cap.drop%"] = pct(double(s.capDropLaps), double(s.capLaps)); m["cap.starve%"] = pct(double(s.capStarveLaps), double(s.capLaps)); m["cap.waived/f"] = double(s.capWaived) / n;
          m["inc/f"] = double(s.inc) / n; m["guardAll%"] = pct(double(s.incGuard), double(s.inc)); m["arcMiss%"] = pct(double(s.incArc), double(s.inc)); m["guardDown%"] = pct(double(s.incDown), double(s.inc));
          const double up = std::max(1e-9, s.upSec);
          m["handLost%"] = 100.0 * s.handLostSec / up; m["handLostDmg/f"] = s.handLostDmg / n; m["reDraw/f"] = double(s.reDraws) / n;
          m["area.up%"] = 100.0 * s.areaUpSec / up; m["area.foe.s/f"] = s.areaFoeSec / n; m["area.ally.s/f"] = s.areaAllySec / n; m["area.out%"] = pct(s.areaOutSec, s.areaUpSec); m["area.recast%"] = pct(double(s.areaRecast), double(s.areaOwnCasts));
          m["drawnMelee%"] = 100.0 * s.drawnSec[1] / up;
          m["minTgt"] = a.minTgtN ? a.minTgtSum / double(a.minTgtN) : -1; m["strafe/f"] = double(s.strafes) / n; }
        // SIM V4 (iaus-sim-v4.md 2.5, 5): the six odd-pathing numbers and their parts, the cast starts. Only in a world with a v4 switch on,
        // so a v2 / v3 run's metric set (and every baseline of it) is what it was.
        if (g_v4.Any()) { const double up = std::max(1e-9, s.upSec);
          m["stuck%"] = 100.0 * s.pathStuckSec / up; m["reblock/f"] = double(s.pathReblock) / n; m["osc/min"] = double(s.pathOsc) / mins; m["unreach%"] = pct(double(s.pathUnreach), double(s.pathLegs));
          m["detour"] = s.detourN ? s.detourSum / double(s.detourN) : -1; { long acc = 0; double p90 = -1; for (int k = 0; k < 16 && s.detourN; ++k) { acc += s.detourHist[k]; if (double(acc) >= 0.9 * double(s.detourN)) { p90 = 1.0 + 0.25 * double(k + 1); break; } } m["detour.p90"] = p90; }
          m["holdUnreach.s/f"] = s.pathHoldUnreachSec / n; m["blocked/f"] = double(s.pathBlocked) / n; m["stuckLegs/f"] = double(s.pathStuckLegs) / n; m["legs/f"] = double(s.pathLegs) / n; m["plans/f"] = double(s.pathPlans) / n;
          m["hold%"] = 100.0 * s.pathHoldSec / up; m["repeat/f"] = double(s.pathRepeat) / n; m["away%"] = 100.0 * s.pathAwaySec / up; m["cornered%"] = 100.0 * s.pathCorneredSec / up; m["cs.heldTurns/f"] = double(s.castHeldTurns) / n;
          m["doors/f"] = double(s.pathDoors) / n; m["drops/f"] = double(s.pathDrops) / n; m["walked/f"] = s.pathWalked / n; m["env.bad"] = double(s.envBad); m["env.stuckForever"] = double(s.envStuckForever);
          m["cs.unseated/f"] = double(s.castUnseated) / n; m["cs.neverShort/f"] = double(s.castNeverShort) / n; m["cs.neverLong/f"] = double(s.castNeverLong) / n; m["cs.heldOff/f"] = double(s.castHeldOff) / n; m["cs.zeroFire/f"] = double(s.castZeroFires) / n; m["cs.zeroHeld/f"] = double(s.castZeroHeld) / n; m["cs.never.s/f"] = s.neverSec / n;
          m["cs.intr/f"] = double(s.castInterrupted) / n; m["cs.foeIntr/f"] = double(s.foeCastInterrupted) / n; m["cs.occRel/f"] = double(s.castOccRelease) / n; m["appr.drop/f"] = double(s.apprDrops) / n; m["appr.block/f"] = double(s.apprBlocked) / n; }
        m["pow/min"] = double(s.powerAttacks) / mins; m["powCtl%"] = pct(double(s.powOnControlled), double(s.powerAttacks)); m["powWhiff%"] = pct(double(s.powerWhiffs), double(s.powerAttacks)); m["ground/min"] = double(s.grounds) / mins; m["block/min"] = double(s.blocks) / mins; m["bash/min"] = double(s.bashes) / mins;
        return m;
    }

    // The oracle's setting in words (Oracle.h OracleCfg): the stopping rule, or the labelled diagnostic cuts. Printed and recorded wherever an oracle ran.
    std::string OracleDesc(const OracleCfg& oc) {
        char b[300]; if (oc.K > 0) std::snprintf(b, sizeof b, "DIAGNOSTIC fixed K=%d", oc.K); else std::snprintf(b, sizeof b, "streams until separated at p=%g, from %d, ceiling %d", oc.pOne, oc.nMin, oc.ceiling);
        std::string d = b; if (oc.H >= 150.0) d += ", rollouts to the fight's end"; else { std::snprintf(b, sizeof b, ", DIAGNOSTIC rollouts cut at %.0f s", oc.H); d += b; }
        if (oc.every > 0.0) { std::snprintf(b, sizeof b, ", DIAGNOSTIC a decision every %.2f s", oc.every); d += b; } else d += ", every decision point";
        d += ", every candidate"; if (!oc.split) d += ", DIAGNOSTIC one shared random stream"; if (!g_classRulings) d += ", DIAGNOSTIC class rulings OFF"; return d; }
    int OracleKOf(const OracleCfg& oc) { return oc.K > 0 ? oc.K : oc.ceiling; }   // the results file's "k": the most streams an option can get

    // What the oracle's search did (Oracle.h OracleStats), one line: sizes per decision, and how often the safety ceiling ended a race.
    std::string StopLine(const OracleStats& s) {
        const double d = std::max<double>(1, double(s.decisions)); char b[400];
        std::snprintf(b, sizeof b, "decisions %ld  options/dec %.1f  streams/dec %.1f  rollouts/dec %.0f  ceiling bound %ld (%.2f%%)  standing there %.1f  unresolved lead mean %.3f max %.3f U", s.decisions, double(s.options) / d, double(s.streams) / d, double(s.rollouts) / d,
                      s.bound, 100.0 * double(s.bound) / d, s.bound ? double(s.standing) / double(s.bound) : 0.0, s.bound ? s.unresolved / double(s.bound) : 0.0, s.unresolvedMax);
        return b; }

    // ── the named scenarios: IAUS, and (with --oracle) the expert oracle on the SAME fights ──
    int RunScenarios(const Data& D, const std::vector<SimClass>& classes, const std::string& which, std::uint64_t seed, int fights, int threads, bool oracle, const OracleCfg& oc, bool g10Sticky, int trace, Results& out, bool estimates, bool scrollRoad) {
        const std::vector<Scenario> all = AllScenarios(true, true, true); std::vector<const Scenario*> pick;   // "all" = the 17 of the headline; "stealth" = the stealth lab's; "field" = sim v3's field failures; "all,stealth" = both
        for (const Scenario& s : all) if (!(s.parked && !g_parked) && ((!s.field && std::strncmp(s.name, "dive-", 5) != 0 && (!s.stealth || g_stealth > 0) && ("," + which + ",").find(",all,") != std::string::npos) || (s.stealth && ("," + which + ",").find(",stealth,") != std::string::npos) || (s.field && !s.env && ("," + which + ",").find(",field,") != std::string::npos) || (s.env && ("," + which + ",").find(",env,") != std::string::npos) || (!std::strncmp(s.name, "dive-", 5) && ("," + which + ",").find(",pressure,") != std::string::npos) || ("," + which + ",").find(std::string(",") + s.name + ",") != std::string::npos)) pick.push_back(&s);
        if (pick.empty()) { std::fprintf(stderr, "unknown scenario '%s'. Known:", which.c_str()); for (const Scenario& s : all) std::fprintf(stderr, " %s", s.name); std::fprintf(stderr, "\n"); return 2; }
        for (const Scenario* s : pick) if (s->env && !g_v4.env) { std::fprintf(stderr, "scenario '%s' needs an environment: --world v4 (or --v4 env=on)\n", s->name); return 2; }
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        struct Res { Agg iaus, orc; std::map<std::string, Disagreement> dis; long dec = 0, disagreed = 0; double dU = 0; std::string cls; OracleStats st; };
        std::vector<Res> res(pick.size()); std::atomic<int> next{ 0 }; std::mutex mx; const int jobs = static_cast<int>(pick.size()) * fights;
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int si = job / fights, f = job % fights;
            Rng R; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919);
            FightSpec fs = pick[static_cast<std::size_t>(si)]->make(D, classes, R); fs.seed = R.g(); fs.rank = g_forceRank ? g_forceRank : 1 + f % 5;
            { std::scoped_lock lk(mx); res[static_cast<std::size_t>(si)].cls = fs.cls->name; }
            Fight F = BuildFight(D, fs, t1); F.g10Sticky = g10Sticky; F.estimates = estimates; F.scrollRoad = scrollRoad; Fight G = F;
            if (trace == f) { std::scoped_lock lk(mx); std::printf("\nTRACE %s fight %d (%s)\n", pick[static_cast<std::size_t>(si)]->name, f, oracle ? "oracle arm" : "IAUS arm");
                for (const Body& x : F.b) std::printf("   %-10s id %-4u L%-2d hp %.0f mp %.0f sp %.0f  AR %.0f  melee %s %.0f  ranged %.0f  spells %zu  run %.0f\n", x.c.name.c_str(), x.c.id, x.c.level, x.c.hp, x.c.mp, x.c.sp, x.spec->ar, x.spec->melee.name.c_str(), WeaponDamage(D.rules, *x.spec, x.spec->melee, false), x.spec->ranged.Has() ? WeaponDamage(D.rules, *x.spec, x.spec->ranged, false) : 0.0f, x.ctl >= 0 ? F.ctl[static_cast<std::size_t>(x.ctl)].build->spells.size() : x.spec->spells.size(), x.spec->run);
                (oracle ? G : F).trace = stdout; }
            RunMode(F, true); const double uI = Utility(F);
            double uO = 0; OraclePolicy O; std::map<std::string, Disagreement> dl;
            if (oracle) { O.cfg = oc; O.log = &dl; RunFight(G, O); uO = Utility(G); }
            std::scoped_lock lk(mx); Res& r = res[static_cast<std::size_t>(si)]; r.iaus.uSum += uI; r.iaus.Add(F.t, false);
            if (oracle) { r.orc.uSum += uO; r.orc.Add(G.t, false); r.dec += O.decisions; r.disagreed += O.disagreed; r.dU += O.dUSum; r.st.Merge(O.st);
                for (auto& kv : dl) { Disagreement& g = r.dis[kv.first]; const Disagreement& a = kv.second; g.n += a.n; g.dU += a.dU; g.iFoeHp += a.iFoeHp; g.oFoeHp += a.oFoeHp; g.iOnPlayer += a.iOnPlayer; g.oOnPlayer += a.oOnPlayer; g.iCaster += a.iCaster; g.oCaster += a.oCaster;
                    g.iResist += a.iResist; g.oResist += a.oResist; g.iRecHp += a.iRecHp; g.oRecHp += a.oRecHp; g.selfHp += a.selfHp; g.selfMp += a.selfMp; g.foes += a.foes; g.sameTarget += a.sameTarget; g.iDist += a.iDist; g.oDist += a.oDist; g.playerHp += a.playerHp; g.nearDist += a.nearDist; } } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        long bad = 0, inv = 0; double gapSum = 0;
        std::printf("\nSCENARIOS (%d fights each, ranks I..V in turn)%s\n", fights, oracle ? "  IAUS | ORACLE (rollout search)" : "");
        for (std::size_t i = 0; i < pick.size(); ++i) {
            const Scenario& sc = *pick[i]; const auto mi = Metrics(res[i].iaus); const auto mo = Metrics(res[i].orc); bad += res[i].iaus.s.badFacts; inv += res[i].iaus.s.invalid;
            { ScenarioRow sr; sr.name = sc.name; sr.cls = res[i].cls; sr.iausU = mi.at("U"); sr.metrics = mi; if (oracle) { sr.hasOracle = true; sr.oracleU = mo.at("U"); sr.gap = sr.oracleU - sr.iausU; sr.oracleMetrics = mo; } out.scen.push_back(std::move(sr)); }
            std::printf("\n%-18s %s\n", sc.name, sc.what);
            std::string keys = sc.metrics; std::size_t p = 0;
            std::printf("  %-8s", "");
            while (p <= keys.size()) { const std::size_t q = keys.find(',', p); const std::string k = keys.substr(p, q == std::string::npos ? std::string::npos : q - p); std::printf(" %12s", k.c_str()); if (q == std::string::npos) break; p = q + 1; }
            auto row = [&](const char* nm, const std::map<std::string, double>& m) { std::printf("\n  %-8s", nm); std::size_t p2 = 0;
                while (p2 <= keys.size()) { const std::size_t q = keys.find(',', p2); const std::string k = keys.substr(p2, q == std::string::npos ? std::string::npos : q - p2); auto it = m.find(k); std::printf(" %12.2f", it == m.end() ? -1.0 : it->second); if (q == std::string::npos) break; p2 = q + 1; } };
            row("IAUS", mi); if (oracle) { row("ORACLE", mo); const double gap = mo.at("U") - mi.at("U"); gapSum += gap;
                std::printf("\n  U gap (oracle - IAUS) = %+.2f   oracle decisions %ld, IAUS disagreed on %ld (%.0f%%), mean cost %.2f U", gap, res[i].dec, res[i].disagreed, res[i].dec ? 100.0 * double(res[i].disagreed) / double(res[i].dec) : 0.0, res[i].disagreed ? res[i].dU / double(res[i].disagreed) : 0.0);
                std::vector<std::pair<double, std::string>> top; for (auto& kv : res[i].dis) top.push_back({ kv.second.dU, kv.first }); std::sort(top.rbegin(), top.rend());
                for (std::size_t k = 0; k < top.size() && k < 7; ++k) { const Disagreement& g = res[i].dis[top[k].second]; const double n = double(g.n);
                    std::printf("\n    %-34s n=%-5ld sum dU=%7.1f  foeHp %.2f>%.2f onPlayer %.2f>%.2f caster %.2f>%.2f resist %.0f>%.0f recipHp %.2f>%.2f dist %.0f>%.0f  selfHp %.2f mp %.2f playerHp %.2f nearFoe %.0f foes %.1f sameTgt %.2f", top[k].second.c_str(), g.n, g.dU,
                                g.iFoeHp / n, g.oFoeHp / n, g.iOnPlayer / n, g.oOnPlayer / n, g.iCaster / n, g.oCaster / n, g.iResist / n, g.oResist / n, g.iRecHp / n, g.oRecHp / n, g.iDist / n, g.oDist / n, g.selfHp / n, g.selfMp / n, g.playerHp / n, g.nearDist / n, g.foes / n, g.sameTarget / n); }
                std::printf("\n    oracle used: %.2f extended actions per fight, %.2f scrolls per fight, %.1f%% of the time on a move order", mo.at("ext/f"), mo.at("scroll/f"), mo.at("move%"));
                std::printf("\n    %s", StopLine(res[i].st).c_str()); for (const auto& kv : res[i].st.refused) std::printf("\n    refused by the class filter: %ld x %s", kv.second, kv.first.c_str()); }
            std::printf("\n");
        }
        if (oracle) std::printf("\nMEAN U GAP over %zu scenario(s): %+.2f\n", pick.size(), gapSum / double(pick.size()));
        { std::map<std::string, std::pair<double, int>> byClass; std::map<std::string, Agg> agg;
          for (std::size_t i = 0; i < pick.size(); ++i) { auto& b = byClass[res[i].cls]; if (oracle) { b.first += out.scen[i].gap; ++b.second; } Agg& a = agg[res[i].cls]; a.uSum += res[i].iaus.uSum; a.s = res[i].iaus.s; a.fights += res[i].iaus.fights; a.durSum += res[i].iaus.durSum; }
          for (auto& kv : byClass) { const SimClass* sc = ClassBySlug(classes, kv.first.c_str()); ResultRow rr; rr.cls = kv.first; rr.line = sc->line; rr.tier = sc->tier; rr.bracket = "scenarios"; rr.fights = agg[kv.first].fights; rr.mode = g_tableMode ? "table" : "iaus"; rr.package = sc->package; rr.defaults = sc->defaults;
              rr.metrics["U"] = agg[kv.first].fights ? agg[kv.first].uSum / double(agg[kv.first].fights) : 0; if (kv.second.second) { rr.hasGap = true; rr.mainGap = kv.second.first / kv.second.second; } out.rows.push_back(std::move(rr)); } }
        std::printf("\nCHECKS: BAD-FACTS ticks=%ld (must be 0)  invalid picks=%ld (must be 0)  => %s\n", bad, inv, bad == 0 && inv == 0 ? "PASS" : "FAIL");
        return bad == 0 && inv == 0 ? 0 : 1;
    }

    // The headline's scenarios: the 17 of main, and while a stealth road is modelled (--stealth g1|hide|g3, the default is g3) the
    // stealth / disengage scenarios that are not parked. The 17 keep their indexes, so their fights are the same in every mode.
    std::vector<Scenario> HeadlineScenarios() { std::vector<Scenario> v; for (Scenario& s : AllScenarios(g_stealth > 0, false, g_casterPressure)) if (!s.parked || g_parked) v.push_back(std::move(s)); return v; }
    const char* StealthName() { static const char* n[] = { "off", "g1", "hide", "g3" }; return n[std::clamp(g_stealth, 0, 3)]; }
    // The T2 batch S switches of this run (section 31): "all" (the default), "off" (--t2 off), else what differs from the default.
    struct T2Flag { const char* name; bool T2::* m; };
    inline constexpr T2Flag kT2Flags[] = { { "c8", &T2::c8 }, { "dodge", &T2::dodge }, { "aversion", &T2::aversion }, { "aim", &T2::aim }, { "block", &T2::block }, { "range", &T2::range }, { "hands", &T2::hands },
                                           { "backoff", &T2::backoff }, { "step", &T2::step }, { "unweighted", &T2::unweighted }, { "reanimate", &T2::reanimate } };
    // SIM V3: the world of this run. "" = v2 (every v3 switch off: the headline and the #meta line are v2's own, byte for byte);
    // else " world=v3" plus what differs from the v3 defaults.
    struct V3Flag { const char* name; bool V3::* m; };
    inline constexpr V3Flag kV3Flags[] = { { "height", &V3::height }, { "chase", &V3::chase }, { "lof", &V3::lof }, { "delivery", &V3::delivery }, { "hands", &V3::hands }, { "stance", &V3::stance }, { "sidestep", &V3::sidestep },
                                           { "casterhold", &V3::casterhold }, { "blockdir", &V3::blockdir }, { "areas", &V3::areas }, { "perks", &V3::perks }, { "hittail", &V3::hittail }, { "burst", &V3::burst } };
    // SIM V4: the world's v4 part. " world=v4" when every default switch is on, plus what differs and where the places come from.
    struct V4Flag { const char* name; bool V4::* m; bool def; };
    inline constexpr V4Flag kV4Flags[] = { { "env", &V4::env, true }, { "path", &V4::path, true }, { "bodies", &V4::bodies, true }, { "los", &V4::los, true }, { "caststart", &V4::caststart, true }, { "interrupt", &V4::interrupt, true },
                                           { "drops", &V4::drops, false }, { "foedrops", &V4::foedrops, false } };
    std::string g_envLabel = "fixtures", g_seats = "today", g_sightTag;
    std::string WorldTag(const char* lead) {
        if (!g_v3.Any() && !g_v4.Any()) return "";
        std::string o = std::string(lead) + (g_v4.Any() ? "world=v4" : "world=v3");
        if (g_v4.Any()) { std::string d; for (const V4Flag& f : kV4Flags) if ((g_v4.*(f.m)) != f.def) d += std::string(d.empty() ? "" : ",") + f.name + (f.def ? "=off" : "=on");
            if (!d.empty()) o += "[" + d + "]"; if (g_v4.env) { char hx[24]; std::snprintf(hx, sizeof hx, "#%08llx", static_cast<unsigned long long>(g_envPool.Hash() & 0xFFFFFFFFull)); o += "+env=" + g_envLabel + hx; } if (g_seats != "today") o += "+seats=" + g_seats; if (g_seatImmunitySeen) o += "+immunity=seen"; o += g_sightTag; } std::string off; for (const V3Flag& f : kV3Flags) if (!(g_v3.*(f.m))) off += std::string(off.empty() ? "" : ",") + f.name + "=off";
        if (!off.empty()) o += "(" + off + ")";
        if (g_v3.chase && g_waiver != 2) o += g_waiver == 1 ? "+waiver=self" : "+waiver=none"; if (g_v3.delivery && g_deliveryOld) o += "+delivery=old";
        if (!g_mirror) o += "+mirror=off"; if (g_v3.chase && g_engageOnSight) o += "+engage-on-sight"; if (g_v3.delivery && g_deliverSec[0] >= 0.0f) o += "+deliver-sec"; if (g_v3.delivery && g_deliverSec[0] < 0.0f && g_deliverUnfired != 1.0f) o += "+deliver-unfired"; if (g_v3.blockdir && g_blockArc != 180.0f) o += "+block-arc"; if (g_v3.sidestep && g_sideStep == 2) o += "+side-step=away"; if (g_casterPressure) o += "+caster-pressure";
        return o;
    }
    std::string g_potTag;
    std::string T2Name() {
        std::string o; bool anyOn = g_t2.feet || g_t2.c13built; for (const T2Flag& f : kT2Flags) anyOn |= g_t2.*(f.m);
        if (!anyOn) o = "off";
        else { if (!g_t2.feet) o += "feet-stop,"; for (const T2Flag& f : kT2Flags) if (!(g_t2.*(f.m))) o += std::string(f.name) + "=off,"; if (!g_t2.c13built) o += "c13=lever,";
            if (o.empty()) o = "all"; else o.pop_back(); }
        if (g_t2.dnd) o += "+dnd"; if (g_aimMiss != 0.12f) o += "+aim-miss"; if (g_aimScatter > 0.0f) o += "+aim-scatter"; if (g_dodgeFlight != 0.2f) o += "+dodge-flight"; if (!g_teach.empty()) o += "+teach"; if (!g_fillSwing) o += "+no-fill-swing"; if (g_mixPartial) o += "+mix-partial"; if (!g_utilResources) o += "+utility-no-resources"; if (g_potionOver > 0.0f) o += "+potion-over";
        o += g_potTag;   // "" = the built-in potion-worth table; "+potworth=<file>" / "+potworth=legacy" otherwise
        return o;
    }
    bool g_printSwitch = false; std::atomic<long> g_swCount{ 0 }; std::atomic<long> g_swMs{ 0 };   // --print-switch: the subject's decision switches and fight time of the objective's fights
    // --print-consume (tune 2.11.1, needs --consumables on): the subject's buff-potion and food uses per objective fight, per kind (the
    // logistics world's use counts: what a carried buff potion or food is worth there, Logistics.h ConsumeUses).
    bool g_printConsume = false; std::atomic<long> g_conFights{ 0 }; std::array<std::atomic<long>, MFO::Iaus::kBuffKinds + 3> g_conUses{};
    inline void NoteSwitch(const Fight& F) { if (g_printSwitch) { g_swCount += F.t.switches; g_swMs += static_cast<long>(F.now * 1000.0); }
        if (g_printConsume && !F.ctl.empty()) { ++g_conFights; for (std::size_t i = 0; i < MFO::Iaus::kBuffKinds; ++i) g_conUses[i] += F.ctl[0].buffUses[i]; for (std::size_t i = 0; i < 3; ++i) g_conUses[MFO::Iaus::kBuffKinds + i] += F.ctl[0].foodUses[i]; } }
    // --potion-worth <file>: lines "<class> <kind 0|1|2> <m1> <m2> <m3> <m4>" (# comments). PotionWorth.h is the same for the built-in world.
    bool LoadPotWorth(const char* path) { std::FILE* f = std::fopen(path, "r"); if (!f) return false; char line[512]; g_potWorth.clear();
        while (std::fgets(line, sizeof line, f)) { char name[128]; int t = 0; float m[4]; if (line[0] == '#' || std::sscanf(line, "%127s %d %f %f %f %f", name, &t, &m[0], &m[1], &m[2], &m[3]) != 6 || t < 0 || t > 2) continue;
            for (int k = 0; k < 4; ++k) g_potWorth[name].m[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)] = m[k]; }
        std::fclose(f); return !g_potWorth.empty(); }
    void BuiltinPotWorth() { g_potWorth.clear(); for (const PotionWorthRow& r : kPotionWorthBuiltin) for (int k = 0; k < 4; ++k) g_potWorth[r.cls].m[static_cast<std::size_t>(r.kind)][static_cast<std::size_t>(k)] = r.m[k]; }

    // Mean utility of IAUS (no oracle) over the scenarios and the natural bracket of every class: the search objective.
    std::string g_optClass;   // --opt-class <slug>: the objective is that class only (its natural / balanced / strong1 brackets and its scenarios)
    double ObjectiveClass(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, double* scenU, double* natU) {
        const std::vector<Scenario> all = HeadlineScenarios(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        int ci = -1; for (std::size_t i = 0; i < classes.size(); ++i) if (classes[i].name == g_optClass) ci = static_cast<int>(i); if (ci < 0) return 0;
        std::vector<int> mine; { Rng R; R.g.seed(1); for (std::size_t i = 0; i < all.size(); ++i) { FightSpec fs = all[i].make(D, classes, R); if (fs.cls->name == g_optClass) mine.push_back(static_cast<int>(i)); } }
        const int br[3] = { 0, 1, 3 }; const int nb = 3 * fights, ns = static_cast<int>(mine.size()) * fights; std::atomic<int> next{ 0 }; std::vector<double> u(static_cast<std::size_t>(ns + nb), 0.0);
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= ns + nb) return; Rng R;
            if (job < ns) { const int si = mine[static_cast<std::size_t>(job / fights)], f = job % fights; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919);
                FightSpec fs = all[static_cast<std::size_t>(si)].make(D, classes, R); fs.seed = R.g(); fs.rank = g_forceRank ? g_forceRank : 1 + f % 5; Fight F = BuildFight(D, fs, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); NoteSwitch(F); u[static_cast<std::size_t>(job)] = Utility(F); }
            else { const int k = job - ns, b = br[k / fights], f = k % fights; R.g.seed(seed * 1000003ULL + std::uint64_t(ci) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(f) * 32452843ULL);
                Fight F = RandomFight(D, classes[static_cast<std::size_t>(ci)], g_forceRank ? g_forceRank : 1 + f % 5, kBrackets[b], R, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); NoteSwitch(F); u[static_cast<std::size_t>(job)] = Utility(F); } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        double a = 0, b = 0; for (int i = 0; i < ns; ++i) a += u[static_cast<std::size_t>(i)]; for (int i = ns; i < ns + nb; ++i) b += u[static_cast<std::size_t>(i)];
        a = ns ? a / ns : 0; b /= std::max(1, nb); if (scenU) *scenU = a; if (natU) *natU = b; return ns ? 0.35 * a + 0.65 * b : b;
    }

    std::string g_objScope;   // --classes a,b: the headline J and the --opt objective cover those classes only (empty = all; --opt-class wins)
    double Objective(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, double* scenU = nullptr, double* natU = nullptr) {
        if (!g_optClass.empty()) return ObjectiveClass(D, classes, seed, fights, threads, scenU, natU);
        const std::vector<Scenario> all0 = HeadlineScenarios(); const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        std::vector<Scenario> all; std::vector<int> allIdx; std::vector<int> cls;   // the scenarios and the natural-bracket classes the objective covers
        const auto inScope = [&](const std::string& n) { return g_objScope.empty() || ("," + g_objScope + ",").find("," + n + ",") != std::string::npos; };
        if (g_objScope.empty()) { all = all0; for (std::size_t i = 0; i < all0.size(); ++i) allIdx.push_back(static_cast<int>(i)); for (std::size_t i = 0; i < classes.size(); ++i) cls.push_back(static_cast<int>(i)); }
        else { Rng R0; R0.g.seed(1); for (std::size_t i = 0; i < all0.size(); ++i) { FightSpec fs = all0[i].make(D, classes, R0); if (inScope(fs.cls->name)) { all.push_back(all0[i]); allIdx.push_back(static_cast<int>(i)); } }
            for (std::size_t i = 0; i < classes.size(); ++i) if (inScope(classes[i].name)) cls.push_back(static_cast<int>(i)); }
        const int ns = static_cast<int>(all.size()) * fights, nb = static_cast<int>(cls.size()) * std::max(1, fights / 2); std::atomic<int> next{ 0 };
        std::vector<double> u(static_cast<std::size_t>(ns + nb), 0.0);
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= ns + nb) return; Rng R;
            if (job < ns) { const int si = job / fights, f = job % fights; R.g.seed(seed * 2654435761ULL + std::uint64_t(allIdx[static_cast<std::size_t>(si)]) * 97 + std::uint64_t(f) * 7919);   // the ORIGINAL scenario index, so a scoped run replays the all-class fights (B714)
                FightSpec fs = all[static_cast<std::size_t>(si)].make(D, classes, R); fs.seed = R.g(); fs.rank = g_forceRank ? g_forceRank : 1 + f % 5; Fight F = BuildFight(D, fs, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); NoteSwitch(F); u[static_cast<std::size_t>(job)] = Utility(F); }
            else { const int k = job - ns, c = cls[static_cast<std::size_t>(k / std::max(1, fights / 2))], f = k % std::max(1, fights / 2); R.g.seed(seed * 1000003ULL + std::uint64_t(c) * 7919 + std::uint64_t(f) * 32452843ULL);
                Fight F = RandomFight(D, classes[static_cast<std::size_t>(c)], g_forceRank ? g_forceRank : 1 + f % 5, kBrackets[0], R, t1); F.estimates = g_objEstimates; F.scrollRoad = g_objScrollRoad; RunMode(F); NoteSwitch(F); u[static_cast<std::size_t>(job)] = Utility(F); } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        double a = 0, b = 0; for (int i = 0; i < ns; ++i) a += u[static_cast<std::size_t>(i)]; for (int i = ns; i < ns + nb; ++i) b += u[static_cast<std::size_t>(i)];
        a /= std::max(1, ns); b /= std::max(1, nb); if (scenU) *scenU = a; if (natU) *natU = b; return ns ? 0.5 * (a + b) : b;   // a scope with no scenario is natural only (as ObjectiveClass), never half of it
    }

    // ── HOW USEFUL IS CARD G3 (marth 11-12; section 31.9) ─────────────────────────────────────────────────────────────────────
    // The same fights three ways: no G3 run (--stealth hide), the run as built (g3), and the D&D variant (g3 + --disengage-dnd: he is
    // no valid target for 6 s from the pick; NOT built in the game). Per class: its natural fights and its own scenarios, n of each
    // per seed. A fight is "touched" when a run began in it under g3. Reported: J-style means, what the run costs and buys, and
    // how many touched fights end differently (won / lost, or the number of times he went down).
    struct DRow { double u = 0; bool won = false, lost = false; long downs = 0, runs = 0, safe = 0, runDowns = 0, flees = 0, fleeHealed = 0, fleeArrived = 0, fleeFail = 0, hides = 0, hideOk = 0; double runDmg = 0, safeSec = 0, runLen = 0, taken = 0, dur = 0, late = 0; };
    int DisengageReport(const Data& D, const std::vector<SimClass>& classes, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int threads, bool estimates, bool scrollRoad) {
        const std::vector<SimClass> t1(classes.begin(), classes.begin() + 3);
        const int keepStealth = g_stealth; const bool keepDnd = g_t2.dnd;
        std::vector<int> cls; for (std::size_t i = 0; i < classes.size(); ++i) { const SimClass& c = classes[i];
            if (only.empty() ? (c.gate.disengage && c.tier <= 2) : ("," + only + ",").find("," + c.name + ",") != std::string::npos) cls.push_back(static_cast<int>(i)); }
        g_stealth = 3; const std::vector<Scenario> all = AllScenarios(true);
        std::printf("DISENGAGE REPORT (card G3)  list=%s  seeds=%zu  n=%d natural fights + n of each own scenario per seed  t2=%s\n", D.name.c_str(), seeds.size(), n, T2Name().c_str());
        std::printf("modes: none = g3 without the hide run and the flee, g3 = the run as built, dnd = g3 + --disengage-dnd (not a valid target for 6 s; NOT built)\n\n");
        std::printf("%-10s %-5s %6s %6s %6s %7s %6s | %6s %7s %7s %8s %8s %7s %7s | %7s %7s %7s %7s\n", "class", "mode", "U", "win%", "dn/f", "tkn/s", "dur.s", "run/f", "len.s", "safe%", "safe.s", "runDmg%", "late%", "runDn%", "touch%", "dU.tch", "flip%", "dDn.tch");
        for (int ci : cls) { const SimClass& sc = classes[static_cast<std::size_t>(ci)];
            std::vector<int> mine; { Rng R; R.g.seed(1); for (std::size_t i = 0; i < all.size(); ++i) { if (all[i].parked && !g_parked) continue; FightSpec fs = all[i].make(D, classes, R); if (fs.cls->name == sc.name) mine.push_back(static_cast<int>(i)); } }
            const int per = n * (1 + static_cast<int>(mine.size())), jobs = per * static_cast<int>(seeds.size());
            std::vector<DRow> res[3]; for (auto& v : res) v.assign(static_cast<std::size_t>(jobs), DRow{});
            for (int mode = 0; mode < 3; ++mode) {
                g_noG3 = mode == 0; g_t2.dnd = mode == 2;   // 'none' keeps every other road of g3 (the back-off, the step): only the hide run and the flee are not asked for
                std::atomic<int> next{ 0 };
                auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const std::uint64_t seed = seeds[static_cast<std::size_t>(job / per)]; const int k = job % per, f = k % n, which = k / n; Rng R; Fight F;
                    if (which == 0) { R.g.seed(seed * 1000003ULL + std::uint64_t(ci) * 7919 + 15485863ULL * std::uint64_t(1 + f % 5) + std::uint64_t(f) * 32452843ULL); F = RandomFight(D, sc, 1 + f % 5, kBrackets[0], R, t1); }
                    else { const int si = mine[static_cast<std::size_t>(which - 1)]; R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919); FightSpec fs = all[static_cast<std::size_t>(si)].make(D, classes, R); fs.seed = R.g(); fs.rank = 1 + f % 5; F = BuildFight(D, fs, t1); }
                    F.estimates = estimates; F.scrollRoad = scrollRoad; RunMode(F);
                    DRow& r = res[mode][static_cast<std::size_t>(job)]; const Tally& t = F.t; r.u = Utility(F); r.won = t.won; r.lost = t.lost; r.downs = t.downs; r.runs = t.runsBegun; r.safe = t.runsSafe; r.runDowns = t.runDowns; r.runDmg = t.runDmg; r.late = t.runDmgAfter5; r.safeSec = t.runSafeSec; r.runLen = t.runLenSec;
                    r.taken = t.dmgTaken; r.dur = t.dur; r.flees = t.flees; r.fleeHealed = t.fleeHealed; r.fleeArrived = t.fleeArrived; r.fleeFail = t.fleeFail; r.hides = t.hides; r.hideOk = t.hideOk; } };
                std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join();
            }
            static const char* mn[3] = { "none", "g3", "dnd" };
            for (int mode = 0; mode < 3; ++mode) { DRow a; long wins = 0, touched = 0, flips = 0; double dU = 0, dDn = 0;
                for (int j = 0; j < jobs; ++j) { const DRow& r = res[mode][static_cast<std::size_t>(j)]; const DRow& base = res[0][static_cast<std::size_t>(j)];
                    a.u += r.u; wins += r.won; a.downs += r.downs; a.runs += r.runs; a.safe += r.safe; a.runDowns += r.runDowns; a.runDmg += r.runDmg; a.late += r.late; a.safeSec += r.safeSec; a.runLen += r.runLen; a.taken += r.taken; a.dur += r.dur;
                    if (mode > 0 && r.runs > 0) { ++touched; dU += r.u - base.u; dDn += static_cast<double>(r.downs - base.downs); if (r.won != base.won || r.lost != base.lost || r.downs != base.downs) ++flips; } }
                const double nj = jobs, nr = std::max<double>(1, double(a.runs));
                std::printf("%-10s %-5s %6.2f %6.1f %6.2f %7.2f %6.1f | %6.2f %7.2f %7.1f %8.2f %8.1f %7.1f %7.1f |", sc.name.c_str(), mn[mode], a.u / nj, 100.0 * double(wins) / nj, double(a.downs) / nj, a.taken / std::max(1e-9, a.dur), a.dur / nj,
                            double(a.runs) / nj, a.runs ? a.runLen / nr : -1.0, a.runs ? 100.0 * double(a.safe) / nr : -1.0, a.safe ? a.safeSec / double(a.safe) : -1.0, a.runs ? 100.0 * a.runDmg / nr : -1.0, a.runs ? 100.0 * a.late / nr : -1.0, a.runs ? 100.0 * double(a.runDowns) / nr : -1.0);
                if (mode > 0) std::printf(" %7.1f %+7.2f %7.1f %+7.2f\n", 100.0 * double(touched) / nj, touched ? dU / double(touched) : 0.0, touched ? 100.0 * double(flips) / double(touched) : 0.0, touched ? dDn / double(touched) : 0.0); else std::printf("\n"); }
        }
        std::printf("\ncolumns: run/f runs begun per fight; len.s mean run length; safe%% runs after which no foe stood within 400 units inside 20 s (he still up); safe.s the mean time to that; runDmg%% the share of his health lost on the run; late%% of it after the run's 5th second; runDn%% runs he went down on;\n         touch%% fights with a run; dU.tch mean utility change of those fights against 'none'; flip%% touched fights whose result or down count differs; dDn.tch mean change of his downs in them\n");
        g_stealth = keepStealth; g_t2.dnd = keepDnd; g_noG3 = false; return 0;
    }

    // ── WHAT A POTION IS WORTH: THE MEASUREMENT (marth 11-12, Q-potion-value "Sim calibrates it") ────────────────────────────────
    // Per class cell and potion kind (0 health, 1 magicka, 2 stamina): the SAME natural fights played with the subject's stock of that
    // kind forced to k = 0..4 (the other kinds as drawn). Printed per (class, kind, k): the mean Utility WITHOUT the potion charge, the
    // paired difference to k - 1 with its standard error (what one more potion at the start of a fight buys), and how many he drank.
    // tools/iaus_sim/potion_worth.py turns this into the worth of a potion kept for the fights that follow (a chain of fights).
    int PotionCalib(const Data& D, const std::vector<SimClass>& all, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int threads, bool estimates, bool scrollRoad) {
        const std::vector<SimClass> t1(all.begin(), all.begin() + 3);
        std::vector<int> cls; for (std::size_t i = 0; i < all.size(); ++i) if (only.empty() || ("," + only + ",").find("," + all[i].name + ",") != std::string::npos) cls.push_back(static_cast<int>(i));
        const int perClass = n * static_cast<int>(seeds.size()), jobs = perClass * static_cast<int>(cls.size()), K = kPotWorthMax;
        std::printf("POTCAL-HEAD list=%s seeds=%zu fights=%d per seed, class, kind and stock (natural bracket) stealth=%s t2=%s\n", D.name.c_str(), seeds.size(), n, StealthName(), T2Name().c_str());
        for (int t = 0; t < 3; ++t) {
            std::vector<std::vector<float>> u(static_cast<std::size_t>(K + 1), std::vector<float>(static_cast<std::size_t>(jobs))); std::vector<std::vector<std::uint8_t>> used = std::vector<std::vector<std::uint8_t>>(static_cast<std::size_t>(K + 1), std::vector<std::uint8_t>(static_cast<std::size_t>(jobs)));
            for (int k = 0; k <= K; ++k) { g_forcePot[t] = k; std::atomic<int> next{ 0 };
                auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int ci = job / perClass, q = job % perClass, si = q / n, f = q % n; const int gi = cls[static_cast<std::size_t>(ci)];
                    Rng R; R.g.seed(seeds[static_cast<std::size_t>(si)] * 1000003ULL + std::uint64_t(gi) * 7919 + std::uint64_t(f) * 32452843ULL + 0xCA11BULL);
                    Fight F = RandomFight(D, all[static_cast<std::size_t>(gi)], 1 + f % 5, kBrackets[0], R, t1); F.estimates = estimates; F.scrollRoad = scrollRoad; RunMode(F);
                    const Kit& kit = F.ctl[0].book.kit; const int left = t == 0 ? kit.potionsHealth : t == 1 ? kit.potionsMagicka : kit.potionsStamina;
                    u[static_cast<std::size_t>(k)][static_cast<std::size_t>(job)] = static_cast<float>(Utility(F, false)); used[static_cast<std::size_t>(k)][static_cast<std::size_t>(job)] = static_cast<std::uint8_t>(std::clamp(k - left, 0, K)); } };
                std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
            g_forcePot[t] = -1;
            for (std::size_t ci = 0; ci < cls.size(); ++ci) for (int k = 0; k <= K; ++k) { double su = 0, sd = 0, sdd = 0; long hist[kPotWorthMax + 1] = {};
                for (int q = 0; q < perClass; ++q) { const std::size_t j = ci * static_cast<std::size_t>(perClass) + static_cast<std::size_t>(q); su += u[static_cast<std::size_t>(k)][j]; ++hist[used[static_cast<std::size_t>(k)][j]];
                    if (k > 0) { const double d = double(u[static_cast<std::size_t>(k)][j]) - double(u[static_cast<std::size_t>(k - 1)][j]); sd += d; sdd += d * d; } }
                const double nn = perClass, md = sd / nn, var = perClass > 1 ? std::max(0.0, (sdd - nn * md * md) / (nn - 1)) : 0.0;
                std::printf("POTCAL %s %d %d %d %.4f %.4f %.4f", all[static_cast<std::size_t>(cls[ci])].name.c_str(), t, k, perClass, su / nn, md, std::sqrt(var / nn));
                for (int x = 0; x <= K; ++x) std::printf(" %.5f", double(hist[x]) / nn); std::printf("\n"); }
        }
        return 0;
    }

    // ── HEALTH-DECLINE CURVES AND THE DRINK LINE (marth 11-12: "the sim should help come up with health decline curves and perfect
    //    drinking times as well. If it uses real enemies and such") ────────────────────────────────────────────────────────────
    // Per class: n fights per seed in each of the natural, balanced and strong1 brackets (the loaded world's own encounters: a list's
    // real actors with --list, the fixtures without) and n per seed of every rating cell. Printed per class:
    //   the DECLINE CURVE (share of his maximum health lost per second while a foe is on him, by the tenth of health he is in),
    //   the share of those seconds in which the last 2 s of damage would down him (too late to drink),
    //   what followed a first fall under 70 / 50 / 40 / 30 / 20 / 10 percent (how often he went down, and how long it took),
    //   his drinks (where he drank, how much of the potion ran over, downs with a potion in the kit, downs mid-gulp),
    //   and two lines read off the curve: NO-WASTE (1 - potion / max health: above it part of the potion is lost) and LAST-SAFE (the
    //   lowest tenth in which under 10% of the pressed seconds were lethal-in-2-s: under it a potion comes too late too often).
    // The best line itself is SEARCHED (tools/iaus_sim/edge_search.py over --curve <class>:drink_health.32.b): this report says where to look.
    // ── --damage-report N: INCOMING DAMAGE ON FOLLOWERS, in the field logs' own terms (calib_damage.py; iaus-sim-v3.md 15) ──────────────
    // The game logs no per-hit damage. What the Deck logs hold is the [sense] line (Confidence::HpLossRate per follower about every 3.3 s:
    // the NET health fraction lost over the last 5 s, the loss capped at 25 % of his maximum, over max(span, 5 s)) and the [bleed] DOWN line.
    // This report computes the SAME numbers for every follower of N natural fights per class, so the lab and the field compare like for
    // like: SENSE per foes bucket, DOWNS per follower-minute, BURST (from his last no-loss sample to the down). It adds what only the lab
    // can see: the seconds from his last moment above 75 % health to the down, and his worst loss inside one second.
    int g_gapBracket = 0;      // --gap-bracket <name>: the strength bracket of the gap pass's "natural" fights (default natural). strong1 is the FIELD-PRESSURE read: the bracket whose downs per follower-minute match the Deck logs (iaus-sim-v3.md 15)
    int g_damageBracket = 0;   // --damage-bracket <name>: which strength bracket the report's fights are drawn in (default natural)
    // ── SIM V4: THE PATHING GATE (iaus-sim-v4.md 2.5, 8.5). `--path-report N`: N natural fights per class and per ENVIRONMENT CLASS of the run's
    // pool, the six odd-pathing numbers, each against its bound. A bound is the ORACLE's value on the same fights plus the row's margin when
    // `--path-oracle K` plays the first K fights of each cell with the class oracle too (the ORCPATH lines, the ORCSURV pattern of Playstyles.h),
    // and the gate then reads IAUS on those same K fights. NOT A TUNE GATE YET: until an oracle pass has written measured bounds the driver must not read it.
    // Without it the bound is the row's own value. THOSE VALUES ARE PLACEHOLDERS: no oracle pass has measured them on this world yet (that is one of
    // the pre-tune measurements, iaus-sim-v4.md 10.3 item 3). To change a gate: edit kPathBounds, nothing else. `--path-gate` makes a FAIL the exit code.
    struct PathBound { const char* metric; double bound, margin; const char* why; };
    inline constexpr PathBound kPathBounds[] = {
        { "stuck%",          3.0, 1.0, "share of his fight time in a leg that makes no progress" },
        { "reblock/f",       0.25, 0.10, "BLOCKED endings beside his previous block, per fight" },
        { "osc/min",         8.0, 1.5, "legs that turn back within 3 s, per minute" },
        { "unreach%",        5.0, 2.0, "legs begun toward a goal no route reaches" },
        { "detour.p90",      3.0, 0.5, "walked over the route he was given plus what his goal moved, the 90th percentile leg" },
        { "holdUnreach.s/f", 1.0, 0.5, "seconds per fight with his attack on a foe he can neither reach nor shoot" },
        { "hold%",           5.0, 1.0, "share of his fight time asked to walk while a hold ran (the retry wait, the approach backoff, a foe past the drop cap)" },
        { "repeat/f",        1.0, 0.5, "UNREACHABLE or STUCK endings beside his previous ending of that kind, per fight" },
    };
    int PathReport(const Data& D, const std::vector<SimClass>& all, const std::vector<SimClass>& scope, std::uint64_t seed, int n, int oracleN, const OracleCfg& oc, int threads, bool estimates, bool scrollRoad, bool gate) {
        const std::vector<SimClass> t1(all.begin(), all.begin() + 3);
        std::vector<std::string> ecs; for (const SimEnv* e : g_envPool.draw) if (std::find(ecs.begin(), ecs.end(), e->cls) == ecs.end()) ecs.push_back(e->cls);
        std::sort(ecs.begin(), ecs.end());
        std::printf("PATHING GATE  %d natural fights per class and environment class  seed=%llu  places=%s (%zu, %zu classes)  bounds=%s%s\n", n, static_cast<unsigned long long>(seed), g_envLabel.c_str(), g_envPool.draw.size(), ecs.size(),
                    oracleN > 0 ? "the class oracle on the same fights + margin" : "kPathBounds (PLACEHOLDERS, not oracle-measured)", WorldTag("  ").c_str());
        int fails = 0, cells = 0, blind = 0; long bad = 0, forever = 0; double cpu = 0; long fightsRun = 0;
        for (const std::string& ec : ecs) {
            std::vector<const SimEnv*> sub; for (const SimEnv* e : g_envPool.draw) if (e->cls == ec) sub.push_back(e);
            struct Cell { Agg iaus, pair /* IAUS on the fights the oracle also played (review F9) */, orc; }; std::vector<Cell> cell(scope.size()); std::mutex mx; std::atomic<int> next{ 0 }; const int jobs = static_cast<int>(scope.size()) * n;
            auto work = [&]() { tl_envDraw = &sub; for (;;) { const int job = next.fetch_add(1); if (job >= jobs) break; const int ci = job / n, f = job % n; int gi = 0; for (std::size_t i = 0; i < all.size(); ++i) if (all[i].name == scope[static_cast<std::size_t>(ci)].name) gi = static_cast<int>(i);
                    Rng R; R.g.seed(seed * 1000003ULL + std::uint64_t(gi) * 7919 + std::uint64_t(f) * 32452843ULL + 0x9A7A11ULL);
                    Fight F = RandomFight(D, scope[static_cast<std::size_t>(ci)], 1 + f % 5, kBrackets[0], R, t1); F.estimates = estimates; F.scrollRoad = scrollRoad; Fight G = F;
                    if (const char* tr = std::getenv("SIM_PATHTRACE"); tr && std::atoi(tr) == job && ec == (std::getenv("SIM_PATHTRACE_ENV") ? std::getenv("SIM_PATHTRACE_ENV") : ec)) F.trace = stdout;   // diagnostic: one fight's event trace (use --threads 1)
                    const auto t0 = std::chrono::steady_clock::now(); RunMode(F); const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); const double u = Utility(F);
                    double uO = 0; const bool orc = f < oracleN; if (orc) { OraclePolicy O; O.cfg = oc; O.cfg.allFollowers = false; RunFight(G, O); uO = Utility(G); }
                    std::scoped_lock lk(mx); Cell& c = cell[static_cast<std::size_t>(ci)]; c.iaus.uSum += u; c.iaus.Add(F.t, false); cpu += sec; ++fightsRun; if (orc) { c.orc.uSum += uO; c.orc.Add(G.t, false); c.pair.uSum += u; c.pair.Add(F.t, false); } }
                tl_envDraw = nullptr; };
            { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
            std::printf("\n%-28s (%zu place%s)  %9s %9s %9s %9s %9s %9s %9s %9s %9s %7s\n", ec.c_str(), sub.size(), sub.size() == 1 ? "" : "s", "stuck%", "reblock/f", "osc/min", "unreach%", "detour90", "holdUnr.s", "hold%", "repeat/f", "U", "gate");
            for (std::size_t ci = 0; ci < scope.size(); ++ci) { const auto m = Metrics(cell[ci].iaus); std::map<std::string, double> mo, mp; if (oracleN > 0) { mo = Metrics(cell[ci].orc); mp = Metrics(cell[ci].pair); }
                bad += static_cast<long>(m.at("env.bad")); forever += static_cast<long>(m.at("env.stuckForever")); std::string why; ++cells;
                for (const PathBound& b : kPathBounds) { const double v = oracleN > 0 ? mp.at(b.metric) : m.at(b.metric), lim = oracleN > 0 ? std::max(0.0, mo.at(b.metric)) + b.margin : b.bound;
                    if (oracleN > 0) std::printf("ORCPATH %s %s %s  iaus %.3f  oracle %.3f  bound %.3f\n", scope[ci].name.c_str(), ec.c_str(), b.metric, v, mo.at(b.metric), lim);
                    if (v > lim) { char t[96]; std::snprintf(t, sizeof t, "%s%s %.2f > %.2f", why.empty() ? "" : ", ", b.metric, v, lim); why += t; } }
                char det[16]; if (m.at("detour.p90") < 0) { std::snprintf(det, sizeof det, "no legs"); ++blind; } else std::snprintf(det, sizeof det, "%.2f", m.at("detour.p90"));   // no arrived leg over 200 units: the row is NOT MEASURED, and says so (it used to print -1.00 and pass)
                if (m.at("env.bad") > 0) why += std::string(why.empty() ? "" : ", ") + "env.bad"; if (m.at("env.stuckForever") > 0) why += std::string(why.empty() ? "" : ", ") + "stuck forever";
                std::printf("  %-26s %12s %9.2f %9.2f %9.2f %9.2f %9s %9.2f %9.2f %9.2f %9.2f %7s%s%s\n", scope[ci].name.c_str(), "", m.at("stuck%"), m.at("reblock/f"), m.at("osc/min"), m.at("unreach%"), det, m.at("holdUnreach.s/f"), m.at("hold%"), m.at("repeat/f"), m.at("U"), why.empty() ? "PASS" : "FAIL", why.empty() ? "" : "  ", why.c_str());
                if (!why.empty()) { ++fails; std::printf("PATHGATE FAIL %s %s: %s\n", scope[ci].name.c_str(), ec.c_str(), why.c_str()); } }
        }
        if (blind) std::printf("\nDETOUR NOT MEASURED in %d cell(s) (\"no legs\": no leg over 200 units arrived, a bow that stands and shoots): the detour bound is not tested there\n", blind);
        std::printf("\nPATH GATES: %d cell(s), %d FAIL  |  env.bad %ld (must be 0)  stuck forever %ld (must be 0)  |  %.2f ms CPU per IAUS fight (%ld fights)  => %s\n", cells, fails, bad, forever, fightsRun ? 1000.0 * cpu / double(fightsRun) : 0.0, fightsRun, fails == 0 && bad == 0 && forever == 0 ? "PASS" : "FAIL");
        return gate && (fails || bad || forever) ? 1 : 0;
    }

    int DamageReport(const Data& D, const std::vector<SimClass>& all, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int threads, bool estimates, bool scrollRoad) {
        const std::vector<SimClass> t1(all.begin(), all.begin() + 3);
        std::vector<int> cls; for (std::size_t i = 0; i < all.size(); ++i) if (only.empty() || ("," + only + ",").find("," + all[i].name + ",") != std::string::npos) cls.push_back(static_cast<int>(i));
        struct DA { long n[3]{}, losing[3]{}, cap[3]{}; double sum[3]{}; long downs = 0, downB[3]{}, calmDown = 0; double minutes = 0, foeSec = 0, pileSec = 0; std::vector<double> burst, from75, peak; long fights = 0; double dur = 0;
            double fHp[3]{}, fAr[3]{}, fLvl[3]{}; long fN[3]{}, fDowns[3]{}, fFast[3]{}; double foeLvl = 0, foeHp = 0; long foeN = 0; };   // per follower line (melee, ranged, mage): his health, physical mitigation (0..1), level; his downs and those under 3 s from above 75 %
        std::vector<DA> agg(cls.size()); std::atomic<int> next{ 0 }; std::mutex mx; const int perClass = n * static_cast<int>(seeds.size()), jobs = perClass * static_cast<int>(cls.size());
        constexpr double kStep = 3.3, kWindow = 5.0, kMinStep = 0.25, kMinSpan = 1.0; constexpr float kCap = 0.25f;   // CombatScan.cpp's throttle as logged (median spacing 3.3 s); Confidence.h kTrendWindow / kTrendMinStep / kTrendMinSpan / kTrendBurstCap
        auto bucket = [](int foes) { return foes <= 2 ? 0 : foes <= 4 ? 1 : 2; };
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int ci = job / perClass, k = job % perClass, si = k / n, f = k % n; const std::uint64_t seed = seeds[static_cast<std::size_t>(si)];
            const int gi = cls[static_cast<std::size_t>(ci)]; const SimClass& sc = all[static_cast<std::size_t>(gi)]; Rng R;
            R.g.seed(seed * 1000003ULL + std::uint64_t(gi) * 7919 + 15485863ULL * std::uint64_t(1 + f % 5) + std::uint64_t(f) * 32452843ULL);   // the natural fights of the gap pass
            Fight F = RandomFight(D, sc, g_forceRank ? g_forceRank : 1 + f % 5, kBrackets[g_damageBracket], R, t1); F.estimates = estimates; F.scrollRoad = scrollRoad;
            struct FS { std::vector<std::pair<double, float>> ring; double nextSense = 0, lastCalm = -1, last75 = 0, lastSenseLoss = -1; int lastFoes = 0; bool up = true; std::vector<std::pair<double, float>> sec; float peak = 0; };
            std::vector<FS> fs(F.ctl.size()); for (std::size_t i = 0; i < fs.size(); ++i) fs[i].nextSense = 1.1 * double(i);
            DA a; IausPolicy P; F.SetArena();
            for (const Ctl& c : F.ctl) { const Body& x = F.b[static_cast<std::size_t>(c.body)]; const int fm = static_cast<int>(c.fam); a.fHp[fm] += x.c.hpMax; a.fAr[fm] += ArmorMitigation(D.rules, x.spec->ar, x.spec->pieces); a.fLvl[fm] += x.c.level; ++a.fN[fm]; }
            for (const Body& o : F.b) if (o.role == Sim::Role::Foe) { a.foeLvl += o.c.level; a.foeHp += o.c.hpMax; ++a.foeN; }
            while (!F.over) {
                if (F.now >= F.nextPump && !F.ctl.empty()) { F.nextPump += TM::pump; const int c = F.rr; P.Serve(F, c); F.rr = (F.rr + 1) % static_cast<int>(F.ctl.size());
                    FS& q = fs[static_cast<std::size_t>(c)]; const Body& x = F.b[static_cast<std::size_t>(F.ctl[static_cast<std::size_t>(c)].body)]; const float hp = std::max(0.0f, x.c.hp) / std::max(1.0f, x.c.hpMax);
                    if (q.ring.empty() || F.now - q.ring.back().first >= kMinStep) { q.ring.push_back({ F.now, hp }); if (q.ring.size() > 32) q.ring.erase(q.ring.begin()); }   // Confidence::NoteHealth: once per own service
                    if (F.now >= q.nextSense) { q.nextSense += kStep; int foes = 0, onHim = 0; for (const Body& o : F.b) if (o.role == Sim::Role::Foe && Fight::Up(o) && o.airUntil >= 0) { ++foes; onHim += o.c.targetId == x.c.id; }
                        float rate = 0;   // Confidence::HpLossRate
                        if (q.ring.size() >= 2) { const auto& nw = q.ring.back(); const auto& nw1 = q.ring[q.ring.size() - 2]; const double window = std::max(kWindow, 2.5 * (nw.first - nw1.first)); const std::pair<double, float>* old = &nw1;
                            for (std::size_t kk = 3; kk <= q.ring.size(); ++kk) { const auto& c2 = q.ring[q.ring.size() - kk]; if (nw.first - c2.first > window) break; old = &c2; }
                            const double span = nw.first - old->first; if (span >= kMinSpan) rate = std::max(0.0f, std::min(kCap, old->second - nw.second) / static_cast<float>(std::max(span, kWindow))); }
                        if (foes > 0) { const int b = bucket(foes); ++a.n[b]; a.losing[b] += rate > 0.0f; a.sum[b] += 100.0 * rate; a.cap[b] += 100.0f * rate >= 4.9f; a.minutes += kStep / 60.0; a.foeSec += foes; a.pileSec += onHim; }
                        if (rate <= 0.0f) q.lastCalm = F.now; q.lastSenseLoss = rate; q.lastFoes = foes; } }
                F.StepWorld();
                for (std::size_t i = 0; i < fs.size(); ++i) { FS& q = fs[i]; const Body& x = F.b[static_cast<std::size_t>(F.ctl[i].body)]; const float hp = std::max(0.0f, x.c.hp) / std::max(1.0f, x.c.hpMax); const bool up = Fight::Up(x);
                    q.sec.push_back({ F.now, hp }); while (q.sec.size() > 1 && F.now - q.sec.front().first > 1.0) q.sec.erase(q.sec.begin()); q.peak = std::max(q.peak, q.sec.front().second - hp);
                    if (q.up && !up) { ++a.downs; ++a.downB[bucket(std::max(1, q.lastFoes))]; if (q.lastCalm >= 0) a.burst.push_back(F.now - q.lastCalm); a.calmDown += q.lastSenseLoss == 0.0; a.from75.push_back(F.now - q.last75); const int fm = static_cast<int>(F.ctl[i].fam); ++a.fDowns[fm]; a.fFast[fm] += F.now - q.last75 < 3.0; }
                    if (up && hp > 0.75f) q.last75 = F.now; q.up = up; }
            }
            for (const FS& q : fs) a.peak.push_back(100.0 * q.peak);
            a.fights = 1; a.dur = F.now;
            std::scoped_lock lk(mx); DA& g = agg[static_cast<std::size_t>(ci)];
            for (int b = 0; b < 3; ++b) { g.n[b] += a.n[b]; g.losing[b] += a.losing[b]; g.cap[b] += a.cap[b]; g.sum[b] += a.sum[b]; g.downB[b] += a.downB[b]; }
            g.downs += a.downs; g.calmDown += a.calmDown; g.minutes += a.minutes; g.foeSec += a.foeSec; g.pileSec += a.pileSec; g.fights += 1; g.dur += a.dur;
            for (int b = 0; b < 3; ++b) { g.fHp[b] += a.fHp[b]; g.fAr[b] += a.fAr[b]; g.fLvl[b] += a.fLvl[b]; g.fN[b] += a.fN[b]; g.fDowns[b] += a.fDowns[b]; g.fFast[b] += a.fFast[b]; } g.foeLvl += a.foeLvl; g.foeHp += a.foeHp; g.foeN += a.foeN;
            g.burst.insert(g.burst.end(), a.burst.begin(), a.burst.end()); g.from75.insert(g.from75.end(), a.from75.begin(), a.from75.end()); g.peak.insert(g.peak.end(), a.peak.begin(), a.peak.end()); } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        std::printf("INCOMING DAMAGE ON FOLLOWERS (every follower of the fight, IAUS)  list=%s  seeds=%zu  %d %s fights per class and seed  world=%s\n", D.name.c_str(), seeds.size(), n, kBrackets[g_damageBracket].name, g_v3.Any() ? "v3" : "v2");
        auto pct = [](std::vector<double> v, double p) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, static_cast<std::size_t>(p * double(v.size())))]; };
        auto under = [](const std::vector<double>& v, double x) { long c = 0; for (double q : v) c += q < x; return c; };
        DA tot; std::vector<std::pair<std::string, const DA*>> rows; for (std::size_t i = 0; i < cls.size(); ++i) { const DA& g = agg[i]; rows.push_back({ all[static_cast<std::size_t>(cls[i])].name, &g });
            for (int b = 0; b < 3; ++b) { tot.n[b] += g.n[b]; tot.losing[b] += g.losing[b]; tot.cap[b] += g.cap[b]; tot.sum[b] += g.sum[b]; tot.downB[b] += g.downB[b]; }
            tot.downs += g.downs; tot.calmDown += g.calmDown; tot.minutes += g.minutes; tot.foeSec += g.foeSec; tot.pileSec += g.pileSec; tot.fights += g.fights; tot.dur += g.dur;
            for (int b = 0; b < 3; ++b) { tot.fHp[b] += g.fHp[b]; tot.fAr[b] += g.fAr[b]; tot.fLvl[b] += g.fLvl[b]; tot.fN[b] += g.fN[b]; tot.fDowns[b] += g.fDowns[b]; tot.fFast[b] += g.fFast[b]; } tot.foeLvl += g.foeLvl; tot.foeHp += g.foeHp; tot.foeN += g.foeN;
            tot.burst.insert(tot.burst.end(), g.burst.begin(), g.burst.end()); tot.from75.insert(tot.from75.end(), g.from75.begin(), g.from75.end()); tot.peak.insert(tot.peak.end(), g.peak.begin(), g.peak.end()); }
        rows.push_back({ "ALL", &tot }); static const char* bn[3] = { "1-2", "3-4", "5+" };
        for (const auto& [name, gp] : rows) { const DA& g = *gp; long N = 0, L = 0, C = 0; double S = 0;
            for (int b = 0; b < 3; ++b) { N += g.n[b]; L += g.losing[b]; C += g.cap[b]; S += g.sum[b];
                if (g.n[b]) std::printf("SIM SENSE %-10s foes %-3s n=%-6ld losing %5.1f%%  mean %.2f %%/s  at-cap %5.1f%%  mean-when-losing %.2f %%/s\n", name.c_str(), bn[b], g.n[b], 100.0 * double(g.losing[b]) / double(g.n[b]), g.sum[b] / double(g.n[b]), 100.0 * double(g.cap[b]) / double(g.n[b]), g.losing[b] ? g.sum[b] / double(g.losing[b]) : 0.0); }
            if (N) std::printf("SIM SENSE %-10s foes all n=%-6ld losing %5.1f%%  mean %.2f %%/s  at-cap %5.1f%%  mean-when-losing %.2f %%/s   (foes a sample %.1f, of them on him %.2f)\n", name.c_str(), N, 100.0 * double(L) / double(N), S / double(N), 100.0 * double(C) / double(N), L ? S / double(L) : 0.0, g.foeSec / double(N), g.pileSec / double(N));
            std::printf("SIM DOWNS %-10s %ld in %.1f follower-minutes of combat = %.3f per follower-minute; by foes on the last sample: 1-2:%ld 3-4:%ld 5+:%ld   (%ld fights, mean %.0f s)\n", name.c_str(), g.downs, g.minutes, g.minutes > 0 ? double(g.downs) / g.minutes : 0.0, g.downB[0], g.downB[1], g.downB[2], g.fights, g.fights ? g.dur / double(g.fights) : 0.0);
            std::printf("SIM BURST %-10s n=%zu  median %.1f s  under 3.5 s: %ld  under 7 s: %ld  (a down whose LAST sample read no loss: %ld)  |  from above 75%% health to the down: median %.1f s, p25 %.1f s, under 1 s: %ld, under 3 s: %ld  |  worst loss inside one second, per follower and fight: median %.0f%%, p90 %.0f%%, max %.0f%%\n", name.c_str(), g.burst.size(), pct(g.burst, 0.5), under(g.burst, 3.5), under(g.burst, 7.0), g.calmDown, pct(g.from75, 0.5), pct(g.from75, 0.25), under(g.from75, 1.0), under(g.from75, 3.0), pct(g.peak, 0.5), pct(g.peak, 0.9), pct(g.peak, 0.999));
            static const char* fn[3] = { "melee", "ranged", "mage" }; std::printf("SIM WHO   %-10s", name.c_str());
            for (int b = 0; b < 3; ++b) if (g.fN[b]) std::printf("  %s: level %.0f health %.0f armor %.2f, %ld downs (%ld under 3 s from above 75%%)", fn[b], g.fLvl[b] / double(g.fN[b]), g.fHp[b] / double(g.fN[b]), g.fAr[b] / double(g.fN[b]), g.fDowns[b], g.fFast[b]);
            std::printf("  | foes: level %.0f health %.0f\n", g.foeN ? g.foeLvl / double(g.foeN) : 0.0, g.foeN ? g.foeHp / double(g.foeN) : 0.0); }
        return 0;
    }

    int DeclineReport(const Data& D, const std::vector<SimClass>& all, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int threads, bool estimates, bool scrollRoad) {
        const std::vector<SimClass> t1(all.begin(), all.begin() + 3);
        std::vector<int> cls; for (std::size_t i = 0; i < all.size(); ++i) if (only.empty() || ("," + only + ",").find("," + all[i].name + ",") != std::string::npos) cls.push_back(static_cast<int>(i));
        const int br[3] = { 0, 1, 3 }, perSeed = 3 * n + kRatingCells * n, perClass = perSeed * static_cast<int>(seeds.size()), jobs = perClass * static_cast<int>(cls.size());
        std::vector<Agg> agg(cls.size()); std::atomic<int> next{ 0 }; std::mutex mx;
        struct Heals { long self = 0, other = 0, castSelf = 0; }; std::vector<Heals> heals(cls.size());   // fights in which he KNOWS a self heal / a heal for others, and his self-heal casts
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int ci = job / perClass, k = job % perClass, si = k / perSeed, j = k % perSeed; const std::uint64_t seed = seeds[static_cast<std::size_t>(si)];
            const int gi = cls[static_cast<std::size_t>(ci)]; const SimClass& sc = all[static_cast<std::size_t>(gi)]; Rng R; Fight F;
            if (j < 3 * n) { const int b = br[j / n], f = j % n; R.g.seed(seed * 1000003ULL + std::uint64_t(gi) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(f) * 32452843ULL + 0xDEC11ULL); F = RandomFight(D, sc, 1 + f % 5, kBrackets[b], R, t1); }
            else { const int q = j - 3 * n, cell = q / n, f = q % n; R.g.seed(seed * 0x9E3779B97F4A7C15ULL + std::uint64_t(sc.tier) * 1000003ULL + std::uint64_t(cell) * 7919ULL + std::uint64_t(f) * 104729ULL + 0xBA77E21ULL); FightSpec fs = RatingFight(D, sc, all, cell, f, R); F = BuildFight(D, fs, t1); }
            bool hs = false, ho = false; for (const SimSpell& sp : F.ctl[0].build->spells) { if (sp.e.source != SpellSource::Spell) continue; hs |= sp.e.nature == Prune::Nature::Heal && sp.e.selfDelivery; ho |= sp.healOther; }
            F.estimates = estimates; F.scrollRoad = scrollRoad; RunMode(F); const double u = Utility(F);
            std::scoped_lock lk(mx); agg[static_cast<std::size_t>(ci)].uSum += u; agg[static_cast<std::size_t>(ci)].Add(F.t, false); heals[static_cast<std::size_t>(ci)].self += hs; heals[static_cast<std::size_t>(ci)].other += ho; heals[static_cast<std::size_t>(ci)].castSelf += F.t.hist[static_cast<std::size_t>(Family::CastHeal)]; } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        std::printf("HEALTH DECLINE AND THE DRINK LINE  list=%s  seeds=%zu  per class and seed: %d natural + %d balanced + %d strong1 fights and %d of each of the %d rating cells  stealth=%s t2=%s\n", D.name.c_str(), seeds.size(), n, n, n, n, kRatingCells, StealthName(), T2Name().c_str());
        std::printf("\nDECLINE CURVE: %% of his maximum health lost per second while a foe is on him, by the tenth of health he is in (pressed seconds per fight in brackets under 5)\n%-12s", "class"); for (int k = 9; k >= 0; --k) std::printf(" %3d-%-3d", k * 10, k * 10 + 10); std::printf("   all\n");
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const Tally& s = agg[ci].s; std::printf("%-12s", all[static_cast<std::size_t>(cls[ci])].name.c_str()); double ts = 0, tl = 0;
            for (int k = 9; k >= 0; --k) { ts += s.decBandSec[k]; tl += s.decBandLoss[k]; if (s.decBandSec[k] > 1.0) std::printf(" %7.2f", 100.0 * s.decBandLoss[k] / s.decBandSec[k]); else std::printf(" %7s", "-"); } std::printf(" %5.2f\n", ts > 0 ? 100.0 * tl / ts : 0.0); }
        std::printf("\nTOO LATE TO DRINK: %% of the pressed seconds in which the damage of his last 2 s is at least the health he has left\n%-12s", "class"); for (int k = 9; k >= 0; --k) std::printf(" %3d-%-3d", k * 10, k * 10 + 10); std::printf("\n");
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const Tally& s = agg[ci].s; std::printf("%-12s", all[static_cast<std::size_t>(cls[ci])].name.c_str());
            for (int k = 9; k >= 0; --k) { if (s.decBandSec[k] > 1.0) std::printf(" %7.1f", 100.0 * s.decLethal[k] / s.decBandSec[k]); else std::printf(" %7s", "-"); } std::printf("\n"); }
        std::printf("\nAFTER HE FIRST FALLS UNDER A LINE: %% of fights that reach it | then %% that end with him down | seconds from the line to the down\n%-12s", "class"); for (int k : { 7, 5, 4, 3, 2, 1 }) std::printf("   under %2d%%: reach  down    s", k * 10); std::printf("\n");
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const Tally& s = agg[ci].s; const double nf = std::max<double>(1, double(agg[ci].fights)); std::printf("%-12s", all[static_cast<std::size_t>(cls[ci])].name.c_str());
            for (int k : { 7, 5, 4, 3, 2, 1 }) std::printf("              %5.1f %5.1f %4.1f", 100.0 * double(s.decCross[k]) / nf, s.decCross[k] ? 100.0 * double(s.decDownAfter[k]) / double(s.decCross[k]) : -1.0, s.decDownAfter[k] ? s.decToDownSec[k] / double(s.decDownAfter[k]) : -1.0); std::printf("\n"); }
        std::printf("\nHIS DRINKS, AND THE TWO LINES THE CURVE GIVES\n%-12s %6s %7s %8s %7s %7s %8s %8s %6s | %8s %9s %9s\n", "class", "U", "drink/f", "drinkAt%", "pot%hp", "waste%", "dnPot/f", "dnMid/f", "dn/f", "no-waste", "last-safe", "line now");
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const Tally& s = agg[ci].s; const SimClass& sc = all[static_cast<std::size_t>(cls[ci])]; const double nf = std::max<double>(1, double(agg[ci].fights));
            const double pot = s.potFracN ? s.potFracSum / double(s.potFracN) : -1.0; int safe = 0; for (int k = 0; k < 10; ++k) { if (s.decBandSec[k] > 1.0 && s.decLethal[k] / s.decBandSec[k] < 0.10) { safe = k; break; } safe = k + 1; }
            std::printf("%-12s %6.2f %7.2f %8.1f %7.1f %7.1f %8.3f %8.3f %6.2f | %8.2f %9.2f %9.2f\n", sc.name.c_str(), agg[ci].uSum / nf, double(s.drinksH) / nf, s.drinksH ? 100.0 * s.drinkAtSum / double(s.drinksH) : -1.0, 100.0 * pot, s.potHeal > 0 ? 100.0 * s.potWaste / s.potHeal : -1.0,
                        double(s.downsWithPotH) / nf, double(s.downsMidDrink) / nf, double(s.downs) / nf, pot >= 0 ? 1.0 - pot : -1.0, 0.1 * safe, static_cast<double>(sc.gate.drinkHealthLine)); }
        std::printf("\nTHE HEALS HE KNOWS (marth 11-12: any mage line with heals can use them, not only the Restoration line)\n%-12s %9s %10s %9s %8s\n", "class", "selfHeal%", "healOther%", "healDec/f", "dwh/f");
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const double nf = std::max<double>(1, double(agg[ci].fights));
            std::printf("%-12s %9.1f %10.1f %9.2f %8.3f\n", all[static_cast<std::size_t>(cls[ci])].name.c_str(), 100.0 * double(heals[ci].self) / nf, 100.0 * double(heals[ci].other) / nf, double(heals[ci].castSelf) / nf, double(agg[ci].s.deathsWithHeal) / nf); }
        std::printf("columns: selfHeal%% / healOther%% fights in which his book holds a heal on himself / on others; healDec/f heal decisions fired per fight; dwh/f times a party member (he included) went down while he had an affordable heal for him\n");
        std::printf("\ncolumns: drink/f health potions begun per fight; drinkAt%% his health when he began; pot%%hp the potion as a share of his maximum; waste%% the part of the potions that ran over his maximum; dnPot/f downs with a health potion still in his kit;\n         dnMid/f downs between the start of a gulp and its effect; no-waste = 1 - pot%%hp; last-safe = the lowest tenth of health in which under 10%% of the pressed seconds were too late to drink; line now = his ClassCurves drink line\n");
        long bad = 0; for (const Agg& a : agg) bad += a.s.badFacts + a.s.invalid;
        std::printf("CHECKS: BAD-FACTS + invalid picks=%ld (must be 0)  => %s\n", bad, bad == 0 ? "PASS" : "FAIL"); return bad == 0 ? 0 : 1;
    }

    // ── THE RATING BATTERY (Rating.h): every class of the run plays its tier's battery, the same fights for the whole tier ──
    // seeds: one battery per seed (the run's --seeds, else --seed). Fills `out` (ranked inside each tier) and prints the table.
    // Returns the BAD-FACTS + invalid pick count of the battery's fights (must be 0, as everywhere).
    long RatingPass(const Data& D, const std::vector<SimClass>& all, const std::vector<int>& cls, const std::vector<std::uint64_t>& seeds, int n, int threads, bool estimates, bool scrollRoad, std::vector<RatingRow>& out, bool print = true) {
        const std::vector<SimClass> t1(all.begin(), all.begin() + 3);
        const int perClass = kRatingCells * n * static_cast<int>(seeds.size()), jobs = static_cast<int>(cls.size()) * perClass;
        std::vector<std::vector<double>> u(cls.size(), std::vector<double>(static_cast<std::size_t>(perClass), 0.0)); std::atomic<int> next{ 0 }; std::atomic<long> bad{ 0 };
        auto work = [&]() { for (;;) { const int job = next.fetch_add(1); if (job >= jobs) return; const int ci = job / perClass, k = job % perClass, si = k / (kRatingCells * n), cell = (k / n) % kRatingCells, f = k % n;
            const SimClass& sc = all[static_cast<std::size_t>(cls[static_cast<std::size_t>(ci)])];
            Rng R; R.g.seed(seeds[static_cast<std::size_t>(si)] * 0x9E3779B97F4A7C15ULL + std::uint64_t(sc.tier) * 1000003ULL + std::uint64_t(cell) * 7919ULL + std::uint64_t(f) * 104729ULL + 0xBA77E21ULL);   // tier, cell, fight, seed: NEVER the class
            FightSpec fs = RatingFight(D, sc, all, cell, f, R); Fight F = BuildFight(D, fs, t1); F.estimates = estimates; F.scrollRoad = scrollRoad; RunMode(F);
            u[static_cast<std::size_t>(ci)][static_cast<std::size_t>(k)] = Utility(F); bad += F.t.badFacts + F.t.invalid; } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }
        out.clear();
        for (std::size_t ci = 0; ci < cls.size(); ++ci) { const SimClass& sc = all[static_cast<std::size_t>(cls[ci])]; RatingRow r; r.cls = sc.name; r.tier = sc.tier; r.fights = perClass;
            double m = 0; for (double x : u[ci]) m += x; m /= std::max(1, perClass); double v = 0; for (double x : u[ci]) v += (x - m) * (x - m);
            r.score = m; r.se = perClass > 1 ? std::sqrt(v / double(perClass - 1) / double(perClass)) : 0.0;
            for (int a = 0; a < kRatingArchN; ++a) { double am = 0; long an = 0; for (int k = 0; k < perClass; ++k) if (((k / n) % kRatingCells) / 4 == a) { am += u[ci][static_cast<std::size_t>(k)]; ++an; } r.byArch[kRatingArch[a].name] = an ? am / double(an) : 0.0; }
            out.push_back(std::move(r)); }
        RankRatings(out, u);
        if (print) {
            std::printf("\nRATING (Tx): one shared battery per tier (%d cells = %d foe archetypes x 2 group sizes x 2 level bands, %d fight(s) per cell, %zu seed(s); the same fights for every class of a tier)\n%-17s %-3s %6s %5s %6s %7s %-5s", kRatingCells, kRatingArchN, n, seeds.size(), "class", "T", "rating", "se", "rank", "vs tier", "flag");
            for (int a = 0; a < kRatingArchN; ++a) std::printf(" %7s", kRatingArch[a].name); std::printf("  weakest (vs tier there)\n");
            std::vector<std::size_t> order(out.size()); for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return out[a].tier != out[b].tier ? out[a].tier < out[b].tier : out[a].rank < out[b].rank; });
            for (std::size_t i : order) { const RatingRow& r = out[i]; char rk[16]; std::snprintf(rk, sizeof rk, "%d/%d", r.rank, r.of);
                std::printf("%-17s T%-2d %6.2f %5.2f %6s %+7.2f %-5s", r.cls.c_str(), r.tier, r.score, r.se, rk, r.delta, r.below ? "BELOW" : "");
                for (int a = 0; a < kRatingArchN; ++a) std::printf(" %7.1f", r.byArch.at(kRatingArch[a].name));
                if (!r.weakest.empty()) std::printf("  %s %+.1f", r.weakest.c_str(), r.weakestDelta); std::printf("\n"); }
            std::printf("flag BELOW = under the tier's mean by more than 2.0 U and by more than two standard errors of the paired difference\n");
        }
        return bad.load();
    }

    // The slugs of a run that a gap-ledger class group covers (Results.h GapRows: "knight line", "casters" ...). A group is a rule over
    // the class cell (its line, gate and flags), never a list of names, except the Knight line, which IS three named classes.
    bool InGapGroup(const SimClass& c, const std::string& g) {
        if (g == "*") return true;
        if (g == "knight line") return c.name == "knight" || c.name == "great-knight" || c.name == "ebony-warrior";
        if (g == "healers") return std::find(c.ensure.begin(), c.ensure.end(), "healOther") != c.ensure.end();
        if (g == "casters") return c.gate.spellsOnClass;
        if (g == "mage lines") return c.line == "Mage";
        if (g == "melee lines") return c.line == "Melee";
        if (g == "sneak classes") return c.sneakAlways;
        if (g == "disengage classes") return c.gate.treeOn && c.gate.disengage;
        if (g == "classes without kDisengage") return c.gate.treeOn && !c.gate.disengage;
        return c.name == g;
    }

    std::vector<std::uint64_t> g_seeds;   // --seeds a,b,c: the objective is the mean over these seeds (empty = the single --seed)
    double ObjectiveMulti(const Data& D, const std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, double* scenU = nullptr, double* natU = nullptr) {
        if (g_seeds.empty()) return Objective(D, classes, seed, fights, threads, scenU, natU);
        double j = 0, a = 0, b = 0; for (std::uint64_t sd : g_seeds) { double x = 0, y = 0; j += Objective(D, classes, sd, fights, threads, &x, &y); a += x; b += y; }
        const double n = static_cast<double>(g_seeds.size()); if (scenU) *scenU = a / n; if (natU) *natU = b / n; return j / n;
    }
    std::string ObjectiveCovers() { return !g_optClass.empty() ? g_optClass : g_objScope.empty() ? std::string("all classes") : g_objScope; }

    int Optimise(const Data& D, const std::vector<SimClass>& classes, const std::string& params, std::uint64_t seed, int fights, int threads, int passes) {
        std::vector<std::string> ps; { std::size_t p = 0; while (p <= params.size()) { const auto q = params.find(',', p); ps.push_back(params.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; } }
        for (const std::string& p : ps) if (!SetParam(p, GetParam(p))) { std::fprintf(stderr, "unknown parameter '%s' (see --list-params)\n", p.c_str()); return 2; }
        double sU = 0, nU = 0; double best = ObjectiveMulti(D, classes, seed, fights, threads, &sU, &nU);
        std::printf("OPTIMISE list=%s  %zu parameter(s), %d pass(es), %d fights per scenario\n", D.name.c_str(), ps.size(), passes, fights);
        if (!g_optClass.empty() || !g_objScope.empty() || !g_seeds.empty()) std::printf("objective covers: %s  seeds=%zu\n", ObjectiveCovers().c_str(), g_seeds.empty() ? size_t(1) : g_seeds.size());
        std::printf("start: J=%.3f (scenarios %.2f, natural %.2f)\n", best, sU, nU);
        for (int pass = 0; pass < passes; ++pass) for (const std::string& p : ps) {
            const double v0 = GetParam(p); double bv = v0;
            std::vector<double> tries = { v0 * 0.5, v0 * 0.75, v0 * 1.33, v0 * 2.0 }; if (std::fabs(v0) < 0.3) { tries.push_back(v0 + 0.15); tries.push_back(std::max(0.0, v0 - 0.15)); }
            if (p == "pk.leash") tries = { 0.0, 600.0, 1000.0, 1500.0, 2500.0 };
            for (double v : tries) { SetParam(p, v); const double j = ObjectiveMulti(D, classes, seed, fights, threads); if (j > best + 0.05) { best = j; bv = v; } }
            SetParam(p, bv); std::printf("  pass %d  %-14s %8.3f -> %8.3f   J=%.3f\n", pass + 1, p.c_str(), v0, bv, best); std::fflush(stdout);
        }
        ObjectiveMulti(D, classes, seed, fights, threads, &sU, &nU);
        std::printf("end:   J=%.3f (scenarios %.2f, natural %.2f)\n--set ", best, sU, nU);
        for (std::size_t i = 0; i < ps.size(); ++i) std::printf("%s%s=%.4g", i ? "," : "", ps[i].c_str(), GetParam(ps[i]));
        std::printf("\n"); return 0;
    }

    // ── default gambit rows (card C12): text form, the row library a class may use, and a hill-climb over row sets ──
    std::string RowsText(const std::vector<GRow>& rows) {
        std::string o; char b[160];
        for (const GRow& r : rows) { std::snprintf(b, sizeof b, "%s%s|%s|%g|%d|%g", o.empty() ? "" : ";", r.cond.c_str(), r.act.c_str(), r.param, r.role, r.cooldown); o += b; }
        return o;
    }
    bool ParseRows(const std::string& text, std::vector<GRow>& out) {
        out.clear(); std::size_t p = 0;
        while (p < text.size()) { const auto q = text.find(';', p); const std::string one = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
            std::vector<std::string> f; std::size_t a = 0; for (;;) { const auto bq = one.find('|', a); f.push_back(one.substr(a, bq == std::string::npos ? std::string::npos : bq - a)); if (bq == std::string::npos) break; a = bq + 1; }
            if (f.size() < 2) return false; GRow r; r.cond = f[0]; r.act = f[1]; r.param = f.size() > 2 ? static_cast<float>(std::atof(f[2].c_str())) : 0; r.role = f.size() > 3 ? std::atoi(f[3].c_str()) : 0; r.cooldown = f.size() > 4 ? static_cast<float>(std::atof(f[4].c_str())) : 0; out.push_back(r);
            if (q == std::string::npos) break; p = q + 1; }
        return true;
    }
    // Every row the board lets THIS class pick (ruling Q59: a pick that does not match the class is impossible): only vocabulary
    // opcodes, gated by the class's own gate and weapon set.
    std::vector<GRow> RowLibrary(const SimClass& c) {
        std::vector<GRow> lib; auto add = [&](const char* cond, const char* act, float p = 0, int role = 0, float cd = 0) { lib.push_back({ cond, act, p, role, cd, true }); };
        for (const char* k : { "cond.foe_any", "cond.foe_lowest_hp", "cond.foe_highest_hp", "cond.foe_is_caster", "cond.foe_is_ranged", "cond.foe_attacking_player", "cond.foe_attacking_ally", "cond.foe_attacking_me",
                               "cond.foe_fleeing", "cond.foe_stronger_than_me", "cond.foe_weaker_than_me", "cond.foe_highest_level" }) add(k, "act.attack");
        add("cond.foe_hp_pct_below", "act.attack", 0.3f);
        if (c.gate.powerAttack) for (const char* k : { "cond.foe_blocking", "cond.foe_stronger_than_me", "cond.foe_attacking_player", "cond.foe_is_caster", "cond.foe_any" }) add(k, "act.power_attack", 0, 0, 10.0f);
        if (c.gate.taunt) { add("cond.foe_attacking_ally", "act.taunt"); add("cond.foe_attacking_player", "act.taunt"); }
        add("cond.self_hp_pct_below", "act.drink_health_potion", 0.3f); add("cond.self_hp_pct_below", "act.drink_health_potion", 0.5f);
        add("cond.self_sp_pct_below", "act.drink_stamina_potion", 0.25f);
        if (c.gate.spellsOnClass) {
            add("cond.self_mp_pct_below", "act.drink_magicka_potion", 0.2f); add("cond.self_mp_pct_below", "act.drink_magicka_potion", 0.4f);
            for (const char* k : { "cond.foe_any", "cond.foe_is_caster", "cond.foe_lowest_hp", "cond.foe_attacking_player", "cond.foe_highest_hp" }) add(k, "act.cast_target", 0, kRoleAttack);
            add("cond.foe_weak_fire", "act.cast_target", 0, kRoleFire); add("cond.foe_weak_frost", "act.cast_target", 0, kRoleFrost); add("cond.foe_weak_shock", "act.cast_target", 0, kRoleShock);
            add("cond.foe_count_at_least", "act.cast_target", 3, kRoleArea);
            add("cond.self_hp_pct_below", "act.cast_self", 0.4f, kRoleHeal); add("cond.self_hp_pct_below", "act.cast_self", 0.6f, kRoleHeal);
            add("cond.ally_hp_pct_below", "act.cast_target", 0.5f, kRoleHeal); add("cond.player_hp_pct_below", "act.cast_player", 0.5f, kRoleHeal); add("cond.ally_downed", "act.cast_target", 0, kRoleHeal);
            add("cond.foe_any", "act.cast_self", 0, kRoleArmor); add("cond.foe_any", "act.cast_self", 0, kRoleSummon); add("cond.foe_count_at_least", "act.cast_self", 2, kRoleSummon);
            add("cond.foe_stronger_than_me", "act.cast_target", 0, kRoleParalyze); add("cond.foe_attacking_player", "act.cast_target", 0, kRoleParalyze); add("cond.foe_count_at_least", "act.cast_target", 3, kRoleFear);
            add("cond.foe_is_undead", "act.cast_target", 0, kRoleTurnUndead);
        }
        if (c.kit.oneHand || c.kit.twoHand || c.kit.dagger) { add("cond.foe_any", "act.equip_melee"); add("cond.foe_within_range", "act.equip_melee", 200.0f); add("cond.self_out_of_arrows", "act.equip_melee", 1.0f); }
        if (c.kit.bow || c.kit.crossbow) { add("cond.foe_any", "act.equip_ranged"); add("cond.foe_beyond_range", "act.equip_ranged", 400.0f); }
        if (c.gate.disengage) { add("cond.self_hp_pct_below", "act.flee", 0.25f); add("cond.foe_attacking_me_melee", "act.flee"); }
        return lib;
    }
    // Ruling Q74: the ORDER of a class's default rows such that every prefix (3 rows at rank I, then 4, then all) is a good
    // table on its own. Every permutation of the set is scored as the mean of J over the prefixes 3 .. n (table mode).
    int OptimisePrefix(const Data& D, std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads) {   // fights = --opt-fights or --fights; the objective is the mean over --seeds (ObjectiveMulti)
        int ci = -1; for (std::size_t i = 0; i < classes.size(); ++i) if (classes[i].name == g_optClass) ci = static_cast<int>(i);
        if (ci < 0) { std::fprintf(stderr, "--opt-prefix needs --opt-class <slug>\n"); return 2; }
        SimClass& c = classes[static_cast<std::size_t>(ci)];
        std::vector<GRow> cur; { bool any = false; for (const GRow& r : c.rows) any |= r.applied; for (const GRow& r : c.rows) if (r.applied || !any) cur.push_back(r); }
        const int n = static_cast<int>(cur.size());
        auto evalK = [&](const std::vector<GRow>& rows, int k) { c.rows = rows; for (GRow& r : c.rows) r.applied = true; g_rowPrefix = k; const double j = ObjectiveMulti(D, classes, seed, fights, threads); g_rowPrefix = 0; return j; };
        auto line = [&](const char* tag, const std::vector<GRow>& rows) { double sum = 0; std::printf("%s", tag); for (int k = std::min(3, n); k <= n; ++k) { const double j = evalK(rows, k); sum += j; std::printf(" J%d=%.3f", k, j); }
            const double mean = sum / std::max(1, n - std::min(3, n) + 1); std::printf(" mean=%.3f  %s\n", mean, RowsText(rows).c_str()); return mean; };
        std::printf("OPT-PREFIX class=%s list=%s rows=%d\n", c.name.c_str(), D.name.c_str(), n);
        const double start = line("today:", cur);
        if (n <= 3 || n > 6) { std::printf("end: nothing to order (%d rows)\n--rows %s\n", n, RowsText(cur).c_str()); c.rows = cur; return 0; }
        std::vector<int> idx(static_cast<std::size_t>(n)); for (int i = 0; i < n; ++i) idx[static_cast<std::size_t>(i)] = i;
        std::vector<int> bestIdx = idx; double best = start;
        while (std::next_permutation(idx.begin(), idx.end())) {
            std::vector<GRow> rows; for (int i : idx) rows.push_back(cur[static_cast<std::size_t>(i)]);
            double sum = 0; for (int k = 3; k <= n; ++k) sum += evalK(rows, k);
            const double mean = sum / (n - 2); if (mean > best + 0.15) { best = mean; bestIdx = idx; }
        }
        std::vector<GRow> rows; for (int i : bestIdx) rows.push_back(cur[static_cast<std::size_t>(i)]);
        line("best: ", rows); c.rows = rows; std::printf("--rows %s\n", RowsText(rows).c_str()); return 0;
    }

    int OptimiseRows(const Data& D, std::vector<SimClass>& classes, std::uint64_t seed, int fights, int threads, int cap, int maxEvals) {
        int ci = -1; for (std::size_t i = 0; i < classes.size(); ++i) if (classes[i].name == g_optClass) ci = static_cast<int>(i);
        if (ci < 0) { std::fprintf(stderr, "--opt-rows needs --opt-class <slug>\n"); return 2; }
        SimClass& c = classes[static_cast<std::size_t>(ci)]; const std::vector<GRow> lib = RowLibrary(c);
        std::vector<GRow> cur; { bool any = false; for (const GRow& r : c.rows) any |= r.applied; for (const GRow& r : c.rows) if (r.applied || !any) cur.push_back(r); }
        auto eval = [&](const std::vector<GRow>& rows) { c.rows = rows; for (GRow& r : c.rows) r.applied = true; return ObjectiveMulti(D, classes, seed, fights, threads); };
        const std::vector<GRow> start = cur;
        double best = eval(cur); int evals = 1; std::printf("OPT-ROWS class=%s list=%s cap=%d library=%zu rows\nstart: J=%.3f  %s\n", c.name.c_str(), D.name.c_str(), cap, lib.size(), best, RowsText(cur).c_str());
        Rng R; R.g.seed(seed * 77 + 5);
        for (bool improved = true; improved && evals < maxEvals;) {
            improved = false; std::vector<std::vector<GRow>> nb;
            for (std::size_t i = 0; i + 1 < cur.size(); ++i) { auto n = cur; std::swap(n[i], n[i + 1]); nb.push_back(n); }                          // reorder
            for (std::size_t i = 0; i < cur.size(); ++i) { if (cur.size() > 1) { auto n = cur; n.erase(n.begin() + static_cast<long>(i)); nb.push_back(n); }   // drop
                if (cur[i].param > 0 && cur[i].param <= 1.0f) for (float k : { 0.75f, 1.33f }) { auto n = cur; n[i].param = std::clamp(cur[i].param * k, 0.05f, 0.95f); nb.push_back(n); }
                else if (cur[i].param > 1.0f) for (float k : { 0.66f, 1.5f }) { auto n = cur; n[i].param = cur[i].param * k; nb.push_back(n); } }
            for (const GRow& l : lib) { bool have = false; for (const GRow& r : cur) have |= r.cond == l.cond && r.act == l.act && r.role == l.role; if (have) continue;
                if (static_cast<int>(cur.size()) < cap) for (std::size_t pos : { std::size_t{ 0 }, cur.size() / 2, cur.size() }) { auto n = cur; n.insert(n.begin() + static_cast<long>(pos), l); nb.push_back(n); }   // add
                for (std::size_t i = 0; i < cur.size(); ++i) { auto n = cur; n[i] = l; nb.push_back(n); } }                                             // replace
            std::shuffle(nb.begin(), nb.end(), R.g);
            // THE CLASS STAYS ITSELF: every kind of action today's defaults hold (act + spell role) must still be there, and the
            // power-attack rows may not multiply (each row has its own 10 s cooldown, so stacking them is a way round ruling Q7).
            auto legal = [&](const std::vector<GRow>& n) {
                for (const GRow& k : start) { bool have = false; for (const GRow& r : n) have |= r.act == k.act && r.role == k.role; if (!have) return false; }
                int pw = 0, pw0 = 0; for (const GRow& r : n) pw += r.act == "act.power_attack"; for (const GRow& r : start) pw0 += r.act == "act.power_attack";
                return pw <= std::max(1, pw0);
            };
            for (const auto& n : nb) { if (evals >= maxEvals) break; if (!legal(n)) continue; const double j = eval(n); ++evals; if (j > best + 0.3) { best = j; cur = n; improved = true; std::printf("  %4d evals  J=%.3f  %s\n", evals, best, RowsText(cur).c_str()); std::fflush(stdout); break; } }
        }
        c.rows = cur; std::printf("end:   J=%.3f after %d evals\n--rows %s\n", best, evals, RowsText(cur).c_str()); return 0;
    }

}

// THE WORKER (tune 2.11.1, coordinator 11-13 "a persistent sim worker per core that loads worlds and binary once"): `sim [flags] --serve
// [<sim.json> ...]` parses each named list ONCE, then reads one job per stdin line (the arguments of a normal run, separated by TABs), forks,
// and the child runs exactly what `sim [flags] <job args>` would (fresh copies of every global, the parsed world shared copy-on-write), its
// stderr joined to stdout; after the child exits the worker prints "@@END <exit code>". A forked child is the same program with the same
// state a fresh process would have, except that LoadData of a preloaded path starts from its parsed JSON tree (Load.h PreloadedJson).
static int RealMain(int argc, char** argv);
int main(int argc, char** argv) {
    int at = -1; for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--serve")) { at = i; break; }
    if (at < 0) return RealMain(argc, argv);
    for (int i = at + 1; i < argc; ++i) Sim::PreloadJson(argv[i]);
    std::string line;
    for (int ch; ; ) {
        line.clear(); while ((ch = std::fgetc(stdin)) != EOF && ch != '\n') line.push_back(static_cast<char>(ch));
        if (ch == EOF && line.empty()) return 0;
        std::vector<std::string> toks; std::size_t p = 0; while (p <= line.size()) { const auto q = line.find('\t', p); toks.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p)); if (q == std::string::npos) break; p = q + 1; }
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            dup2(1, 2);
            std::vector<char*> av; av.push_back(argv[0]); for (int i = 1; i < at; ++i) av.push_back(argv[i]); for (auto& t : toks) if (!t.empty()) av.push_back(t.data()); av.push_back(nullptr);
            const int rc = RealMain(static_cast<int>(av.size()) - 1, av.data());
            std::fflush(stdout); std::fflush(stderr); _exit(rc);
        }
        int status = 0; waitpid(pid, &status, 0);
        std::printf("@@END %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0)); std::fflush(stdout);
    }
}
static int RealMain(int argc, char** argv) {
    int disengageReport = 0, stealLab = 0;
    Loot::Opts lootOpts; std::string lootList;   // SIM V4 batch L
    std::string envFile, envSet, envClass; bool envTest = false, envList = false, pathGate = false; int pathReport = 0, pathOracle = 0, soloWalk = 0;   // SIM V4
    std::vector<std::string> curveSpecs;
    int potCalibN = 0; std::string potWorthPath;
    const char* listPath = nullptr; const char* cmp = nullptr; const char* outBase = nullptr; std::uint64_t seed = 1; int fights = 8, threads = 8; bool optPrefix = false; bool tauntVisible = true, engineCasts = true, estimates = true, scrollRoad = false, g10Sticky = false, legacy = false, mech = false, printClasses = false, oracle = false; std::string only, scenario; OracleCfg oc; int trace = -1; std::string jsonPath, label, sets, opt, rowsText, printRows, sneakProfile; bool noJson = false, listParams = false, listUses = false, optRows = false; bool fightsGiven = false; bool noClassRulings = false; int optFights = 0, optPasses = 2, rowCap = 5, rowEvals = 500, headlineOracle = 0, classOracle = -1, gapScen = 2, ratingN = -1; bool ratingOnly = false, gapDump = false; int logiN = 0, logiK = 8; std::string logiPolicy = "baseline"; std::string logiUses; bool logiScore = false; int declineN = 0, damageN = 0;
    for (int i = 1; i < argc; ++i) {
        // T2 batch S (section 31): `--<road> off|on` per road, `--t2 off` = every road off and the feet stop (the lab of section 30)
        bool t2Arg = false;
        for (const T2Flag& f : kT2Flags) if (!std::strcmp(argv[i] + (argv[i][0] == '-' && argv[i][1] == '-' ? 2 : 0), f.name) && argv[i][0] == '-' && i + 1 < argc && (!std::strcmp(argv[i + 1], "off") || !std::strcmp(argv[i + 1], "on"))) { g_t2.*(f.m) = !std::strcmp(argv[++i], "on"); t2Arg = true; break; }
        if (t2Arg) continue;
        // SIM V3 (iaus-sim-v3.md section 2): --world v2|v3, --v3 <name>=on|off[,...], --waiver, --delivery, --deliver-sec, --block-arc
        if (!std::strcmp(argv[i], "--world") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "v2")) { g_v3.AllOff(); g_v4.AllOff(); } else if (!std::strcmp(v, "v3")) { g_v3 = V3{}; g_v4.AllOff(); } else if (!std::strcmp(v, "v4")) { g_v3 = V3{}; g_v4.Defaults(); } else { std::fprintf(stderr, "--world v2|v3|v4\n"); return 2; } }
        // SIM V4 (iaus-sim-v4.md 1.4): --v4 <name>=on|off[,...], the places (--env <file> | --env-set <set>|all [--env-class <class>]), --sight, --seats, --env-test, --path-report
        else if (!std::strcmp(argv[i], "--v4") && i + 1 < argc) { std::string v = argv[++i]; std::size_t p0 = 0;
            while (p0 <= v.size()) { const std::size_t q = v.find(',', p0); const std::string tok = v.substr(p0, q == std::string::npos ? std::string::npos : q - p0); const std::size_t eq = tok.find('=');
                const std::string nm = tok.substr(0, eq), val = eq == std::string::npos ? "on" : tok.substr(eq + 1); bool known = false;
                if (val != "on" && val != "off") { std::fprintf(stderr, "--v4 %s: on|off\n", tok.c_str()); return 2; }
                for (const V4Flag& f : kV4Flags) if (nm == f.name) { g_v4.*(f.m) = val == "on"; known = true; }
                if (!known) { std::fprintf(stderr, "unknown --v4 switch '%s'. Known:", nm.c_str()); for (const V4Flag& f : kV4Flags) std::fprintf(stderr, " %s", f.name); std::fprintf(stderr, "\n"); return 2; }
                if (q == std::string::npos) break; p0 = q + 1; } }
        else if (!std::strcmp(argv[i], "--env") && i + 1 < argc) envFile = argv[++i];
        else if (!std::strcmp(argv[i], "--env-set") && i + 1 < argc) envSet = argv[++i];
        else if (!std::strcmp(argv[i], "--env-class") && i + 1 < argc) envClass = argv[++i];
        else if (!std::strcmp(argv[i], "--env-test")) envTest = true;
        // SIM V4 BATCH L (iaus-sim-v4.md 14.7): looting in the fight world. Any of these turns the v4 world on.
        else if (!std::strcmp(argv[i], "--loot-report") && i + 1 < argc) lootOpts.report = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--loot-oracle") && i + 1 < argc) lootOpts.oracle = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--loot-cost") && i + 1 < argc) lootOpts.cost = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--loot-scenario") && i + 1 < argc) lootOpts.scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--loot-test")) lootOpts.test = true;
        else if (!std::strcmp(argv[i], "--loot-score")) lootOpts.score = true;
        else if (!std::strcmp(argv[i], "--loot-trace") && i + 1 < argc) lootOpts.trace = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--loot-plan") && i + 1 < argc) { const std::string v = argv[++i]; lootOpts.plan = -2; for (int q = 0; q < static_cast<int>(Loot::Plan::kCount); ++q) if (v == Loot::PlanName(static_cast<Loot::Plan>(q))) lootOpts.plan = q; if (v == "mixed") lootOpts.plan = -1; if (lootOpts.plan == -2) { std::fprintf(stderr, "--loot-plan mixed|stands|walks-on|opens-chest|loots-too|faces-away\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--loot-wave") && i + 1 < argc) { const std::string v = argv[++i]; lootOpts.wave = v == "mixed" ? -2.0 : v == "off" ? -1.0 : std::atof(v.c_str()); }
        else if (!std::strcmp(argv[i], "--loot-sec") && i + 1 < argc) lootOpts.lootSec = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--loot-guards") && i + 1 < argc) lootOpts.guards = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--loot-museum") && i + 1 < argc) Loot::g_museumShare = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--loot-list") && i + 1 < argc) lootList = argv[++i];
        else if (!std::strcmp(argv[i], "--loot-rule") && i + 1 < argc) { const std::string kv = argv[++i]; const auto eq = kv.find('='); const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
            if (eq == std::string::npos || !Loot::SetRule(Loot::g_rules, k, v == "old1117" ? 1.0f : v == "today" ? 0.0f : static_cast<float>(std::atof(v.c_str())))) { std::fprintf(stderr, "--loot-rule %s: unknown rule (Loot.h Rules; gate=old1117|today)\n", kv.c_str()); return 2; } }
        else if (!std::strcmp(argv[i], "--env-list")) envList = true;
        else if (!std::strcmp(argv[i], "--solo-walk") && i + 1 < argc) soloWalk = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--path-report") && i + 1 < argc) pathReport = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--path-oracle") && i + 1 < argc) pathOracle = std::max(0, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--path-gate")) pathGate = true;
        else if (!std::strcmp(argv[i], "--sight") && i + 1 < argc) { const std::string v = argv[++i]; g_sightTag = "+sight=" + v;   // one sight layer at a time (2.4): walls=off | terrain=off | gap=<units>
            if (v == "walls=off") g_sightT.walls = false; else if (v == "terrain=off") g_sightT.terrain = false; else if (!v.compare(0, 4, "gap=")) g_sightT.gapU = g_sightT.gapExteriorU = static_cast<float>(std::atof(v.c_str() + 4)); else { std::fprintf(stderr, "--sight walls=off|terrain=off|gap=<units>\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--seat-immunity") && i + 1 < argc) { const std::string v = argv[++i]; if (v != "seen" && v != "unseen") { std::fprintf(stderr, "--seat-immunity seen|unseen\n"); return 2; } g_seatImmunitySeen = v == "seen"; }
        else if (!std::strcmp(argv[i], "--seats") && i + 1 < argc) { g_seats = argv[++i]; if (g_seats != "today" && g_seats != "old") { std::fprintf(stderr, "--seats today|old\n"); return 2; } g_seatsOld = g_seats == "old"; }
        else if (!std::strcmp(argv[i], "--v3") && i + 1 < argc) { std::string v = argv[++i]; std::size_t p0 = 0;
            while (p0 <= v.size()) { const std::size_t q = v.find(',', p0); const std::string tok = v.substr(p0, q == std::string::npos ? std::string::npos : q - p0); const std::size_t eq = tok.find('=');
                const std::string nm = tok.substr(0, eq), val = eq == std::string::npos ? "on" : tok.substr(eq + 1); bool known = false;
                if (val != "on" && val != "off") { std::fprintf(stderr, "--v3 %s: on|off\n", tok.c_str()); return 2; }
                for (const V3Flag& f : kV3Flags) if (nm == f.name) { g_v3.*(f.m) = val == "on"; known = true; }
                if (!known) { std::fprintf(stderr, "unknown --v3 switch '%s'. Known:", nm.c_str()); for (const V3Flag& f : kV3Flags) std::fprintf(stderr, " %s", f.name); std::fprintf(stderr, "\n"); return 2; }
                if (q == std::string::npos) break; p0 = q + 1; } }
        else if (!std::strcmp(argv[i], "--waiver") && i + 1 < argc) { const char* v = argv[++i]; g_waiver = !std::strcmp(v, "none") ? 0 : !std::strcmp(v, "self") ? 1 : !std::strcmp(v, "player") ? 2 : -1; if (g_waiver < 0) { std::fprintf(stderr, "--waiver none|self|player\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--delivery") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "old")) g_deliveryOld = true; else if (!std::strcmp(v, "main")) g_deliveryOld = false; else { std::fprintf(stderr, "--delivery main|old\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--deliver-sec") && i + 1 < argc) { if (std::sscanf(argv[++i], "%f,%f", &g_deliverSec[0], &g_deliverSec[1]) != 2 || g_deliverSec[0] < 0 || g_deliverSec[1] < 0) { std::fprintf(stderr, "--deliver-sec <offense>,<heal> (seconds)\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--engage-on-sight")) g_engageOnSight = true;   // ch.21 EngageOnSight modelled in every fight (ships OFF)
        else if (!std::strcmp(argv[i], "--mirror") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "off")) g_mirror = false; else if (!std::strcmp(v, "on")) g_mirror = true; else { std::fprintf(stderr, "--mirror on|off\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--deliver-unfired") && i + 1 < argc) { g_deliverUnfired = static_cast<float>(std::atof(argv[++i])); if (!(g_deliverUnfired >= 0.0f)) { std::fprintf(stderr, "--deliver-unfired <k >= 0> (scales the measured unfired share of every claim; 0 = every claim fires)\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--side-step") && i + 1 < argc) { const char* v = argv[++i]; g_sideStep = !std::strcmp(v, "random") ? 1 : !std::strcmp(v, "away") ? 2 : 0; if (!g_sideStep) { std::fprintf(stderr, "--side-step random|away (off: --v3 sidestep=off)\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--caster-pressure")) g_casterPressure = true;
        else if (!std::strcmp(argv[i], "--block-arc") && i + 1 < argc) { g_blockArc = static_cast<float>(std::atof(argv[++i])); if (!(g_blockArc > 0.0f && g_blockArc <= 360.0f)) { std::fprintf(stderr, "--block-arc <degrees, 1..360>\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--t2") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "off")) g_t2.AllOff(); else if (!std::strcmp(v, "on")) g_t2 = T2{}; else { std::fprintf(stderr, "--t2 off|on\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--feet-stop")) g_t2.feet = false;                 // Q-SIM-5: the lab before (a swap, a potion and a cast stop his feet)
        else if (!std::strcmp(argv[i], "--c13") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "built")) g_t2.c13built = true; else if (!std::strcmp(v, "lever")) g_t2.c13built = false; else { std::fprintf(stderr, "--c13 built|lever\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--disengage-dnd")) g_t2.dnd = true;               // marth 11-12: a disengage drops him as a valid target for 6 s (NOT built in the game)
        else if (!std::strcmp(argv[i], "--aim-miss") && i + 1 < argc) g_aimMiss = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--aim-scatter") && i + 1 < argc) g_aimScatter = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--dodge-flight") && i + 1 < argc) g_dodgeFlight = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--mix-partial")) g_mixPartial = true;            // ch.18 denies on a window that is not full yet (not Harbinger's reading)
        else if (!std::strcmp(argv[i], "--no-fill-swing")) g_fillSwing = false;           // [Debug] bIausFillSwing OFF (the shipped default): no Block candidate
        else if (!std::strcmp(argv[i], "--teach") && i + 1 < argc) g_teach.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--disengage-report") && i + 1 < argc) disengageReport = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--steal-lab") && i + 1 < argc) stealLab = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list") && i + 1 < argc) listPath = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--fights") && i + 1 < argc) { fights = std::atoi(argv[++i]); fightsGiven = true; }
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) cmp = argv[++i];
        else if (!std::strcmp(argv[i], "--baseline") && i + 1 < argc) outBase = argv[++i];
        else if (!std::strcmp(argv[i], "--taunt-visible")) tauntVisible = true;
        else if (!std::strcmp(argv[i], "--taunt-hidden")) tauntVisible = false;
        else if (!std::strcmp(argv[i], "--no-engine-casts")) engineCasts = false;
        else if (!std::strcmp(argv[i], "--g10-sticky")) g10Sticky = true;
        else if (!std::strcmp(argv[i], "--no-estimates")) estimates = false;
        else if (!std::strcmp(argv[i], "--calm-holds")) g_calmBreaks = false;
        else if (!std::strcmp(argv[i], "--roads") && i + 1 < argc) { const std::string v = argv[++i]; g_groundRoad = v.find("ground") != std::string::npos; g_blockRoad = v.find("block") != std::string::npos; g_bashRoad = v.find("bash") != std::string::npos; }
        else if (!std::strcmp(argv[i], "--stealth") && i + 1 < argc) { const char* v = argv[++i]; g_stealth = !std::strcmp(v, "off") ? 0 : !std::strcmp(v, "g1") ? 1 : !std::strcmp(v, "hide") ? 2 : (!std::strcmp(v, "g3") || !std::strcmp(v, "aware")) ? 3 : std::clamp(std::atoi(v), 0, 3); }   // the stealth / disengage roads modelled as existing (World.h g_stealth); default g3
        else if (!std::strcmp(argv[i], "--sneak-profile") && i + 1 < argc) sneakProfile = argv[++i];   // vanilla | steep (lorerim-like) | list: synthetic sneak data laid over the loaded world (Detect.h ApplyProfile)
        else if (!std::strcmp(argv[i], "--light-scale") && i + 1 < argc) g_lightScale = static_cast<float>(std::atof(argv[++i]));   // the light level of full daylight (World.h g_lightScale, MODEL 100)
        else if (!std::strcmp(argv[i], "--blow-stands")) g_blowStands = true;         // the blow from stealth is struck standing (World.h g_blowStands)
        else if (!std::strcmp(argv[i], "--class-rulings")) g_classRulings = true;      // the oracle is held to the class playstyles (Playstyles.h): the default since the standing rule of 11-17 (kept as a no-op)
        else if (!std::strcmp(argv[i], "--no-class-rulings")) { g_classRulings = false; noClassRulings = true; }   // DIAGNOSTIC: the written rulings are not applied (the class-bound ability filter of Oracle.h ClassRefuses still is)
        else if (!std::strcmp(argv[i], "--parked")) g_parked = true;                  // the PARKED rows (T3+ playstyles, passives, stealth scenarios) take effect; Beta 1 = T1 + T2 only without it
        else if (!std::strcmp(argv[i], "--class-kit")) g_classKit = true;              // the subject knows the spells his class's default rows name
        else if (!std::strcmp(argv[i], "--logistics") && i + 1 < argc) logiN = std::max(1, std::atoi(argv[++i]));   // THE LOGISTICS WORLD (Logistics.h): N episodes per seed and class, baseline vs the loot oracle
        else if (!std::strcmp(argv[i], "--logi-k") && i + 1 < argc) logiK = std::max(1, std::atoi(argv[++i]));      // the loot oracle's sampled futures per option
        else if (!std::strcmp(argv[i], "--logi-policy") && i + 1 < argc) logiPolicy = argv[++i];                     // baseline (today's rules); iaus is refused until the post-T2 loot scorer exists
        else if (!std::strcmp(argv[i], "--oracle-subject-only")) g_oracleSubjectOnly = true;   // the gap pass's oracle plans for the subject only (Oracle.h g_oracleSubjectOnly)
        else if (!std::strcmp(argv[i], "--gap-dump")) gapDump = true;                        // the gap pass prints GAPFIGHT / GAPSIT / GAPGEN / GAPDEC lines (tune_driver.py stage gap)
        else if (!std::strcmp(argv[i], "--class-oracle") && i + 1 < argc) { classOracle = std::max(0, std::atoi(argv[++i])); if (classOracle > 0 && !noClassRulings) g_classRulings = true; }   // N natural fights per class with the class-constrained oracle: the per-class oracle gap
        else if (!std::strcmp(argv[i], "--gap-scenarios") && i + 1 < argc) gapScen = std::max(0, std::atoi(argv[++i]));   // the gap pass: N fights of each of a class's own named scenarios (default 2; 0 = natural fights only)
        else if (!std::strcmp(argv[i], "--rating") && i + 1 < argc) ratingN = std::max(0, std::atoi(argv[++i]));        // the rating battery: N fights per cell (default 3 in a class report run; 0 = off)
        else if (!std::strcmp(argv[i], "--decline") && i + 1 < argc) declineN = std::max(1, std::atoi(argv[++i]));         // the health-decline curves and the drink record of --classes over --seeds: N fights per bracket and rating cell per seed
        else if (!std::strcmp(argv[i], "--potion-calib") && i + 1 < argc) potCalibN = std::max(1, std::atoi(argv[++i]));   // the potion-worth measurement of --classes over --seeds (PotionCalib above)
        else if (!std::strcmp(argv[i], "--potion-worth") && i + 1 < argc) potWorthPath = argv[++i];                          // a potion-worth table for this world (potion_worth.py), or "legacy" = the older fixed rate
        else if (!std::strcmp(argv[i], "--posture-report")) g_postureReport = true;
        else if (!std::strcmp(argv[i], "--ranged-perks")) { if (i + 1 < argc && !std::strcmp(argv[i + 1], "off")) { g_rangedPerks = false; ++i; } else if (i + 1 < argc && !std::strcmp(argv[i + 1], "on")) ++i; }   // BATCH 3: the bow skill's ModAttackDamage chain (default ON; "off" = the lab before the fix)
        else if (!std::strcmp(argv[i], "--point-blank")) g_pointBlank = true;
        else if (!std::strcmp(argv[i], "--named-follower") && i + 1 < argc) g_namedFollower = argv[++i];                   // BATCH 3 sanity cell (test input)
        else if (!std::strcmp(argv[i], "--foe-styles")) g_foeStyles = true;                                                // PART C: foe backpedal from its CSTY Fallback Mult
        else if (!std::strcmp(argv[i], "--space") && i + 1 < argc) g_spaceSize = std::max(0.0f, static_cast<float>(std::atof(argv[++i])));   // PART C: enclosed fights bounded (radius >= R)
        else if (!std::strcmp(argv[i], "--space-force") && i + 1 < argc) { const char* v = argv[++i]; g_spaceForce = !std::strcmp(v, "enclosed") ? 1 : !std::strcmp(v, "open") ? 0 : -1; }                                              // BATCH 3: the hold under fCombatRangedAttackHoldTimeMinDistance
        else if (!std::strcmp(argv[i], "--draw-spoil") && i + 1 < argc) g_drawSpoil = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);   // BATCH 3 MODEL
        else if (!std::strcmp(argv[i], "--buff-dur") && i + 1 < argc) { const char* v = argv[++i]; if (!std::strcmp(v, "real")) g_buffDurReal = true; else if (std::strcmp(v, "lab")) { std::fprintf(stderr, "--buff-dur real|lab\n"); return 2; } }                                         // DIAGNOSTIC (batch 3, the archer study): POSTURE section + pr.* metrics
        else if (!std::strcmp(argv[i], "--guard-why")) g_guardWhy = true;                                                   // DIAGNOSTIC: the unguarded blows of GUARD UNDER TAUNT by cause, as ung2*% metrics (in --baseline output)
        else if (!std::strcmp(argv[i], "--print-switch")) g_printSwitch = true;
        else if (!std::strcmp(argv[i], "--print-consume")) g_printConsume = true;
        else if (!std::strcmp(argv[i], "--logi-score")) logiScore = true;                                                // tune 2.11.1: the IAUS loot arm only, SEED <s> J=<mean J_log> per seed (one class)                                           // after every SEED line: CONSUSE <seed> <fights> <uses per buff kind (17)> <food uses health stamina magicka>
        else if (!std::strcmp(argv[i], "--consumables") && i + 1 < argc) { const std::string v = argv[++i]; if (v != "on" && v != "off") { std::fprintf(stderr, "--consumables on|off\n"); return 2; } g_consumables = v == "on"; }   // tune 2.11.1: gold, values, buff potions, food in the kits (Setup.h FillConsumables)
        else if (!std::strcmp(argv[i], "--logi-consumables")) Logi::g_logiConsumables = true;                            // tune 2.11.1: the logistics fixture's buff potions, food, poisons and soul gems (Logistics.h)
        else if (!std::strcmp(argv[i], "--logi-uses") && i + 1 < argc) logiUses = argv[++i];                              // tune 2.11.1: per class, the combat world's uses per fight of each buff kind and food (CONSUSE lines)                                             // after every SEED line: SWITCH <seed> <decision switches of the subject> <fight minutes>
        else if (!std::strcmp(argv[i], "--utility-no-resources")) g_utilResources = false;                              // DIAGNOSTIC: Utility without its "resources left" term (Oracle.h); never a gate, never a baseline
        else if (!std::strcmp(argv[i], "--potion-over") && i + 1 < argc) g_potionOver = static_cast<float>(std::atof(argv[++i]));   // EXPERIMENT: at-once potions are delivered over S seconds (Load.h g_potionOver)
        else if (!std::strcmp(argv[i], "--rating-only")) ratingOnly = true;                                               // print the rating table of --classes (over --seeds) and stop: no class cells, no results file
        else if (!std::strcmp(argv[i], "--regroup-road")) g_regroupRoad = true;       // the ch.24 regroup client is modelled as existing
        else if (!std::strcmp(argv[i], "--follower-death")) g_followerDeath = true;   // a follower at 0 health is dead for good (LoreRim's default)
        else if (!std::strcmp(argv[i], "--mortal-blind")) g_mortalKnown = false;       // ... and IAUS is not told (Snapshot::mortal stays false)   // the first control model: a Calm is never broken by a blow
        else if (!std::strcmp(argv[i], "--reach") && i + 1 < argc) { const char* v = argv[++i]; g_learnReach = !std::strcmp(v, "off") ? 0 : !std::strcmp(v, "cold") ? 1 : 2; }   // ruling Q68: off | cold | warm (default)
        else if (!std::strcmp(argv[i], "--scroll-road")) scrollRoad = true;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) g_tableMode = !std::strcmp(argv[++i], "table");
        else if (!std::strcmp(argv[i], "--classes") && i + 1 < argc) { only = argv[++i]; if (only == "legacy") { legacy = true; only.clear(); } else if (only == "real") only.clear(); else if (only == "beta1") only = kBeta1Classes; else if (only == "beta1-tree") only = kBeta1TreeClasses; }   // beta1 = the 12 T1 + T2 classes + the 3 tree-OFF base cells; beta1-tree = the 12 alone (the J of sections 30 / 31)
        else if (!std::strcmp(argv[i], "--profiles") && i + 1 < argc) mech = !std::strcmp(argv[++i], "mech");
        else if (!std::strcmp(argv[i], "--print-classes")) printClasses = true;
        else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--oracle")) oracle = true;
        else if (!std::strcmp(argv[i], "--no-ext")) oc.ext = false;
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
        else if (!std::strcmp(argv[i], "--no-json")) noJson = true;
        else if (!std::strcmp(argv[i], "--curve") && i + 1 < argc) curveSpecs.push_back(argv[++i]);   // per-class curves: <class>:<family>.<cons>.<param>=<v> | <class>:scalar.<name>=<v>
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) { if (!sets.empty()) sets += ","; sets += argv[++i]; }
        else if (!std::strcmp(argv[i], "--opt") && i + 1 < argc) opt = argv[++i];
        else if (!std::strcmp(argv[i], "--opt-passes") && i + 1 < argc) optPasses = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--opt-class") && i + 1 < argc) g_optClass = argv[++i];
        else if (!std::strcmp(argv[i], "--opt-fights") && i + 1 < argc) optFights = std::atoi(argv[++i]);   // fights per scenario for each --opt evaluation (default: --fights)
        else if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) { g_seeds.clear(); const std::string v = argv[++i]; std::size_t p = 0; while (p <= v.size()) { const auto q = v.find(',', p); const std::string t = v.substr(p, q == std::string::npos ? std::string::npos : q - p); if (!t.empty()) g_seeds.push_back(std::strtoull(t.c_str(), nullptr, 10)); if (q == std::string::npos) break; p = q + 1; } if (g_seeds.empty()) { std::fprintf(stderr, "--seeds needs at least one seed (a,b,c)\n"); return 2; } }
        else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) rowsText = argv[++i];          // <class>=<cond>|<act>|<param>|<role>|<cooldown>;...
        else if (!std::strcmp(argv[i], "--print-rows") && i + 1 < argc) printRows = argv[++i];
        else if (!std::strcmp(argv[i], "--opt-rows")) optRows = true;
        else if (!std::strcmp(argv[i], "--opt-prefix")) optPrefix = true;                          // ruling Q74: order the class's rows so every prefix is a good table
        else if (!std::strcmp(argv[i], "--row-prefix") && i + 1 < argc) g_rowPrefix = std::atoi(argv[++i]);   // every follower holds only his top N rows
        else if (!std::strcmp(argv[i], "--rank") && i + 1 < argc) g_forceRank = std::atoi(argv[++i]);          // the subject's rapport rank in every fight
        else if (!std::strcmp(argv[i], "--headline-oracle") && i + 1 < argc) headlineOracle = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--row-cap") && i + 1 < argc) rowCap = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--row-evals") && i + 1 < argc) rowEvals = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list-params")) listParams = true;
        else if (!std::strcmp(argv[i], "--list-uses")) listUses = true;
        else if (!std::strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--oracle-k") && i + 1 < argc) { oc.K = std::atoi(argv[++i]); }   // DIAGNOSTIC: a fixed ceiling of K streams instead of the stopping rule
        else if (!std::strcmp(argv[i], "--oracle-h") && i + 1 < argc) { oc.H = std::atof(argv[++i]); }   // DIAGNOSTIC below Fight::maxSec: a rollout is cut there
        else if (!std::strcmp(argv[i], "--oracle-ceiling") && i + 1 < argc) oc.ceiling = std::max(1, std::atoi(argv[++i]));   // the stopping rule's safety ceiling (Oracle.h OracleCfg)
        else if (!std::strcmp(argv[i], "--damage-report") && i + 1 < argc) damageN = std::max(1, std::atoi(argv[++i]));   // incoming damage on followers in the field logs' terms (DamageReport; calib_damage.py reads the field side)
        else if (!std::strcmp(argv[i], "--gap-bracket") && i + 1 < argc) { const char* v = argv[++i]; bool ok = false; for (std::size_t b = 0; b < std::size(kBrackets); ++b) if (!std::strcmp(v, kBrackets[b].name)) { g_gapBracket = static_cast<int>(b); ok = true; } if (!ok) { std::fprintf(stderr, "unknown --gap-bracket '%s'\n", v); return 2; } }
        else if (!std::strcmp(argv[i], "--damage-bracket") && i + 1 < argc) { const char* v = argv[++i]; bool ok = false; for (std::size_t b = 0; b < std::size(kBrackets); ++b) if (!std::strcmp(v, kBrackets[b].name)) { g_damageBracket = static_cast<int>(b); ok = true; } if (!ok) { std::fprintf(stderr, "unknown --damage-bracket '%s'\n", v); return 2; } }
        else if (!std::strcmp(argv[i], "--oracle-shared-stream")) oc.split = false;   // DIAGNOSTIC: the rollouts draw from ONE stream in world order, as before the per-actor streams (World.h Fight::split)
        else if (!std::strcmp(argv[i], "--oracle-every") && i + 1 < argc) oc.every = std::atof(argv[++i]);   // DIAGNOSTIC: a throttle on his decisions (0 = every decision point)
        else { std::fprintf(stderr, "usage: run.sh [--list sim.json] [--curve <class>:<family>.<cons>.<param>=<v>] [--seed N] [--fights N] [--threads N] [--compare baseline] [--baseline out] [--taunt-visible] [--no-engine-casts] [--g10-sticky] [--classes legacy|real|slug,slug] [--profiles mech] [--print-classes] [--scenario all|name,name [--oracle [--oracle-k N] [--oracle-h S] [--no-ext]] [--trace N]] [--json path|--no-json] [--label text] [--class-oracle N] [--gap-scenarios N] [--rating N] [--rating-only] [--seeds a,b,c] [--opt p,q [--opt-passes N] [--opt-fights N] [--opt-class slug]]\n"); return 2; } }
    if (listParams) { ListParams(); return 0; }
    if (listUses) {   // one line per (family, consideration) the catalogue scores: what a per-class curve override can reach (tools/iaus_sim/edge_search.py wide)
        static const char* kn[] = { "Lin", "Pow", "Logi", "Inv", "Bin", "Piece" }; const Tuning& t0 = MFO::Iaus::DefaultTuning();
        for (std::size_t f = 0; f < kFamilies; ++f) { if (static_cast<Family>(f) == Family::Scroll) continue;
            for (const MFO::Iaus::detail::Use& u : MFO::Iaus::detail::SetFor(static_cast<Family>(f), static_cast<Family>(f))) { const Curve& c = t0.curve[static_cast<std::size_t>(u.cons)];
                std::printf("USE %zu %s %zu %s %s %g %g %g %g\n", f, std::string(Name(static_cast<Family>(f))).c_str(), static_cast<std::size_t>(u.cons), std::string(Name(u.cons)).c_str(), kn[static_cast<std::size_t>(c.kind)], static_cast<double>(c.a), static_cast<double>(c.b), static_cast<double>(c.floor), static_cast<double>(u.w));
                if (c.kind == MFO::Iaus::CurveKind::Piece) { std::printf("PIECE %zu %zu %d", f, static_cast<std::size_t>(u.cons), int(c.np)); for (std::size_t i = 0; i < c.np; ++i) std::printf(" %g %g", static_cast<double>(c.px[i]), static_cast<double>(c.py[i])); std::printf("\n"); } } }
        for (std::size_t f = 0; f < kFamilies; ++f) std::printf("FAMILY %zu %s %g\n", f, std::string(Name(static_cast<Family>(f))).c_str(), static_cast<double>(t0.actionWeight[f]));
        return 0; }
    ActiveTuning() = &Tune().t; ProfileHook() = &ApplyWeights; PackageHook() = &ApplyPackage;
    if (!sets.empty() && !ParseSets(sets.c_str())) return 2;
    if (!curveSpecs.empty()) { std::string err; if (legacy) err = "--curve needs the real class table (not --classes legacy)"; if (!err.empty() || !ApplyCurveSpecs(curveSpecs, Tune().t, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        // A per-class override is a COPY of the shared curve taken when the spec is applied: a search over the shared curve of the same
        // consideration would not move that class, and its result would read as if it had. Refused, never silently half-applied.
        for (std::size_t ci : g_curveConsEdited) { const std::string tag = "c." + std::to_string(ci) + "."; if (("," + opt).find("," + tag) != std::string::npos) { std::fprintf(stderr, "--opt %s* together with a --curve on consideration %zu is refused: the per-class override is a copy of the shared curve, so the search would not move it\n", tag.c_str(), ci); return 2; } } }
    if (logiN > 0 && listPath) { std::fprintf(stderr, "--logistics with --list is REFUSED: the list extracts (tools/list_balance) hold no containers, no gold, MISC, gems or ingredients, no vendor chests or gold, and no armor or potion weights (iaus-sim-v2.md 34.6). The logistics world is the built-in fixture until the extractor carries them.\n"); return 2; }
    if (logiN > 0 && logiPolicy != "baseline" && logiPolicy != "iaus") { std::fprintf(stderr, "--logi-policy %s: baseline | iaus\n", logiPolicy.c_str()); return 2; }
    if (logiScore && logiPolicy != "iaus") { std::fprintf(stderr, "--logi-score needs --logi-policy iaus\n"); return 2; }
    if (Logi::g_logiConsumables && (logiUses.empty() || !Logi::LoadUses(logiUses))) { std::fprintf(stderr, "--logi-consumables needs a readable --logi-uses <file> (the combat world's uses per fight, CONSUSE)\n"); return 2; }
    Data Dm = listPath ? LoadData(listPath) : BuiltinData();
    // The potion-worth table of this world: the committed built-in one, a list's own (--potion-worth <file>), or the older fixed rate.
    // A list run WITHOUT its table is refused a silent fallback: it prices potions with the built-in world's worth and says so in the headline.
    if (!potWorthPath.empty()) { const auto sl = potWorthPath.find_last_of('/'); g_potTag = "+potworth=" + (sl == std::string::npos ? potWorthPath : potWorthPath.substr(sl + 1)); }
    if (potWorthPath == "legacy") g_potLegacy = true;
    else if (!potWorthPath.empty()) { if (!LoadPotWorth(potWorthPath.c_str())) { std::fprintf(stderr, "cannot read --potion-worth '%s'\n", potWorthPath.c_str()); return 2; } }
    else if (listPath && potCalibN == 0 && !listUses && !listParams && !printClasses) {   // REFUSED, never a silent fallback: the built-in table is the built-in world's
        std::fprintf(stderr, "a --list run needs its own potion-worth table: --potion-worth <table> (tools/iaus_sim/potion_worth.py --list ... --out <table>; tune_driver.py measures it), or --potion-worth legacy for the older fixed rate\n"); return 2; }
    else BuiltinPotWorth();
    if (sneakProfile == "lorerim-like") sneakProfile = "steep";
    if (sneakProfile.empty()) sneakProfile = listPath ? "list" : "vanilla";   // the built-in world has no sneak data of its own: Skyrim.esm's
    if (!Detect::ApplyProfile(Dm, sneakProfile)) { std::fprintf(stderr, "unknown --sneak-profile '%s' (vanilla | steep | lorerim-like | list)\n", sneakProfile.c_str()); return 2; }
    const Data& D = Dm;
    std::vector<SimClass> allClasses = legacy ? LegacyClasses() : RealClasses(mech);
    if (!rowsText.empty()) { const auto eq = rowsText.find('='); bool ok = eq != std::string::npos; std::vector<GRow> rows;
        if (ok) ok = ParseRows(rowsText.substr(eq + 1), rows); bool found = false;
        if (ok) for (SimClass& c : allClasses) if (c.name == rowsText.substr(0, eq)) { c.rows = rows; c.defaults = "lab"; found = true; }
        if (!ok || !found) { std::fprintf(stderr, "bad --rows (use <class>=<cond>|<act>|<param>|<role>|<cooldown>;...)\n"); return 2; } }
    if (!printRows.empty()) { for (const SimClass& c : allClasses) if (c.name == printRows) { std::vector<GRow> cur; bool any = false; for (const GRow& r : c.rows) any |= r.applied; for (const GRow& r : c.rows) if (r.applied || !any) cur.push_back(r); std::printf("%s\n", RowsText(cur).c_str()); } return 0; }
    const std::vector<SimClass> t1(allClasses.begin(), allClasses.begin() + 3);
    if (printClasses) { PrintClassTable(allClasses); return 0; }
    // SIM V4: the places of this run. The fixtures are always loaded (the scenarios name them). --env / --env-set replace what an ordinary fight draws from.
    const bool lootRun = lootOpts.report || lootOpts.oracle || lootOpts.cost || lootOpts.test || !lootOpts.scenario.empty();
    if (lootRun && !g_v4.env) g_v4.Defaults();
    if (!lootList.empty()) { std::string err; if (!Loot::LoadLootList(lootList, &err)) { std::fprintf(stderr, "--loot-list: %s\n", err.c_str()); return 2; } }
    if (g_v4.env || envTest || envList || pathReport || soloWalk) {
        if (!LoadFixtures(g_envPool)) { std::fprintf(stderr, "cannot load the fixture environments from %s:", g_envDir.c_str()); for (const EnvSkip& k : g_envPool.skipped) std::fprintf(stderr, " %s (%s)", k.file.c_str(), k.why.c_str()); std::fprintf(stderr, "\n(python3 tools/iaus_sim/envs/make_fixtures.py writes them; SIM_ENV_DIR points elsewhere)\n"); return 2; }
        if (!envFile.empty() || !envSet.empty()) { g_envPool.draw.clear(); const std::size_t sk0 = g_envPool.skipped.size();
            if (!envFile.empty()) { if (!g_envPool.Add(envFile, true)) { std::fprintf(stderr, "--env %s: %s\n", envFile.c_str(), g_envPool.skipped.back().why.c_str()); return 2; } if (g_envPool.draw.empty()) g_envPool.draw.push_back(g_envPool.all.back().get()); g_envLabel = g_envPool.all.back()->name; }
            else { const int n = LoadSet(g_envPool, envSet, envClass); if (n <= 0) { std::fprintf(stderr, "--env-set %s%s%s: no environment loaded (the library is ~/.cache/mfo-env, built by tools/envextract; MFO_ENV_CACHE points elsewhere)\n", envSet.c_str(), envClass.empty() ? "" : " --env-class ", envClass.c_str()); return 2; }
                g_envLabel = envSet + (envClass.empty() ? "" : ":" + envClass); }
            if (g_envPool.skipped.size() > sk0) { std::fprintf(stderr, "ENV SKIPPED %zu anomalous place(s):", g_envPool.skipped.size() - sk0); for (std::size_t k = sk0; k < g_envPool.skipped.size(); ++k) std::fprintf(stderr, " %s (%s)", g_envPool.skipped[k].file.c_str(), g_envPool.skipped[k].why.c_str()); std::fprintf(stderr, "\n"); } }
        if (envList) { for (const auto& e : g_envPool.all) { std::size_t solid = 0, face = 0; for (const EQ::Wall& w : e->walls) (w.kind == EQ::WallKind::Solid ? solid : face)++; std::uint32_t comps = 0; for (std::uint32_t c : e->walk) comps = std::max(comps, c + 1);
                std::printf("ENV %-44s %-28s %s tris %6zu comps %4u comp0 %5.1f%% doors %3zu entries %3zu spawns %3zu walls %6zu faces %5zu%s%s\n", e->name.c_str(), e->cls.c_str(), e->interior ? "int" : "ext", e->tris.size(), comps, 100.0 * double(e->tris0) / double(e->tris.size()), e->doors0.size(), e->entries.size(), e->spawns.size(), solid, face, e->defects ? " DEFECTS" : "", e->pool ? "" : " scenario-only"); }
            return 0; }
        if (envTest) return RunEnvTests(D, allClasses);
        if (soloWalk) return RunSoloWalk(D, allClasses, soloWalk);
    }
    {   // B711: a class name that matches nothing is an error, never a silently empty scope
        const auto known = [&](const std::string& n) { for (const SimClass& c : allClasses) if (c.name == n) return true; return false; };
        std::vector<std::string> bad; std::size_t p = 0; while (!only.empty() && p <= only.size()) { const auto q = only.find(',', p); const std::string t = only.substr(p, q == std::string::npos ? std::string::npos : q - p); if (!t.empty() && !known(t)) bad.push_back(t); if (q == std::string::npos) break; p = q + 1; }
        if (!g_optClass.empty() && !known(g_optClass)) bad.push_back(g_optClass);
        if (!bad.empty()) { std::fprintf(stderr, "unknown class slug(s):"); for (const std::string& b : bad) std::fprintf(stderr, " %s", b.c_str()); std::fprintf(stderr, "  (see --print-classes)\n"); return 2; } }
    g_objEstimates = estimates; g_objScrollRoad = scrollRoad;
    if (stealLab > 0) return RunStealLab(D, allClasses, stealLab, seed);
    if (lootRun) { const std::vector<std::uint64_t> sd = g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds;
        if (lootOpts.test) return Loot::RunScenarios(D, allClasses, t1, "all", true, false);
        if (!lootOpts.scenario.empty()) return Loot::RunScenarios(D, allClasses, t1, lootOpts.scenario, false, lootOpts.trace >= 0);
        if (lootOpts.cost) return Loot::RunCost(D, allClasses, t1, only, seed, lootOpts);
        if (lootOpts.oracle) return Loot::RunOracleReport(D, allClasses, t1, only, sd, threads, lootOpts);
        return Loot::RunReport(D, allClasses, t1, only, sd, threads, lootOpts); }
    if (logiN > 0) return Logi::Run(allClasses, only, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, logiN, logiK, true, logiPolicy, logiScore);
    if (disengageReport > 0) return DisengageReport(D, allClasses, only, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, disengageReport, threads, estimates, scrollRoad);
    if (optRows) return OptimiseRows(D, allClasses, seed, optFights > 0 ? optFights : fights, threads, rowCap, rowEvals);
    if (optPrefix) return OptimisePrefix(D, allClasses, seed, optFights > 0 ? optFights : fights, threads);
    if (g_optClass.empty() && !only.empty()) g_objScope = only;   // --classes scopes the headline J and the objective (--opt-class wins)
    if (!opt.empty()) return Optimise(D, allClasses, opt, seed, optFights > 0 ? optFights : fights, threads, optPasses);
    // A CLASS REPORT RUN (the default mode, writing a results file) always fills the per-class gap and the rating, at a small price:
    // 4 natural fights per class and 2 of each of its own scenarios under the oracle, and 3 fights per rating cell. --no-json runs
    // (searches, baselines) skip the gap pass unless --class-oracle asks for it; --class-oracle 0 / --rating 0 turn a pass off.
    bool autoGap = false; if (classOracle < 0) { classOracle = noJson || !scenario.empty() ? 0 : 4; autoGap = classOracle > 0; }
    // The automatic gap pass runs THE oracle (marth 11-17: no reduced setting; it ran a cut K = 6, 90 s oracle before). Its price is the
    // oracle's (iaus-sim-v3.md 14.4); --class-oracle 0 turns the pass off, --oracle-k / --oracle-h give a labelled diagnostic oracle.
    if (ratingN < 0) ratingN = !scenario.empty() ? 0 : 3;
    if (declineN > 0 && oracle) { g_oracleMode = true; g_oc = oc; if (!noClassRulings) g_classRulings = true; std::printf("PLAYED BY THE CLASS ORACLE (%s): where the oracle drinks is a second reading of the drink line\n", OracleDesc(oc).c_str()); }
    if (potCalibN > 0) return PotionCalib(D, allClasses, only, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, potCalibN, threads, estimates, scrollRoad);
    if (pathReport > 0) { if (!g_v4.env) { std::fprintf(stderr, "--path-report needs an environment: --world v4\n"); return 2; }
        std::vector<SimClass> sc; for (const SimClass& c : allClasses) if (("," + (only.empty() ? std::string(kBeta1Classes) : only) + ",").find("," + c.name + ",") != std::string::npos) sc.push_back(c);
        if (pathOracle > 0 && !noClassRulings) g_classRulings = true;
        return PathReport(D, allClasses, sc, seed, pathReport, std::min(pathOracle, pathReport), oc, threads, estimates, scrollRoad, pathGate); }
    if (damageN > 0) return DamageReport(D, allClasses, only, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, damageN, threads, estimates, scrollRoad);
    if (declineN > 0) return DeclineReport(D, allClasses, only, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, declineN, threads, estimates, scrollRoad);
    if (ratingOnly) { std::vector<int> ci; for (std::size_t i = 0; i < allClasses.size(); ++i) if (only.empty() || ("," + only + ",").find("," + allClasses[i].name + ",") != std::string::npos) ci.push_back(static_cast<int>(i));
        std::vector<RatingRow> rr; const long bad = RatingPass(D, allClasses, ci, g_seeds.empty() ? std::vector<std::uint64_t>{ seed } : g_seeds, std::max(1, ratingN), threads, estimates, scrollRoad, rr);
        std::printf("CHECKS: BAD-FACTS + invalid picks=%ld (must be 0)  => %s\n", bad, bad == 0 ? "PASS" : "FAIL"); return bad == 0 ? 0 : 1; }
    // Fights per scenario for the headline J. B710: a --seeds run honours --fights. B755: a full run's headline is ALWAYS 24, whatever
    // --fights says (that flag sizes the class cells), so two archived full runs never differ in J because of their cell size.
    const int hf = !g_seeds.empty() && fightsGiven ? fights : 24;
    if (!g_seeds.empty()) {   // J per seed, mean and standard error; no full tables
        std::vector<double> js; for (std::uint64_t sd : g_seeds) { double a = 0, b = 0; js.push_back(Objective(D, allClasses, sd, hf, threads, &a, &b)); std::printf("SEED %llu  J=%.2f (scenarios %.2f, natural %.2f)\n", (unsigned long long)sd, js.back(), a, b); if (g_printSwitch) { std::printf("SWITCH %llu %ld %.3f\n", (unsigned long long)sd, g_swCount.exchange(0), double(g_swMs.exchange(0)) / 60000.0); }
            if (g_printConsume) { std::printf("CONSUSE %llu %ld", (unsigned long long)sd, g_conFights.exchange(0)); for (auto& u : g_conUses) std::printf(" %ld", u.exchange(0)); std::printf("\n"); } }
        double m = 0; for (double x : js) m += x; m /= double(js.size()); double v = 0; for (double x : js) v += (x - m) * (x - m);
        const double se = js.size() > 1 ? std::sqrt(v / double(js.size() - 1) / double(js.size())) : -1.0;   // n=1 has no spread: print n/a
        char seb[32]; if (se < 0) std::snprintf(seb, sizeof seb, "n/a"); else std::snprintf(seb, sizeof seb, "%.2f", se);
        std::printf("HEADLINE  J mean=%.2f  se=%s  n=%zu seeds  fights=%d per scenario  covers=%s  mode=%s  stealth=%s%s  t2=%s\n", m, seb, js.size(), hf, ObjectiveCovers().c_str(), g_tableMode ? "table" : "iaus", StealthName(), WorldTag("  ").c_str(), T2Name().c_str()); return 0; }
    Results R; R.list = D.name; R.seed = seed; R.fights = fights; R.mode = g_tableMode ? "table" : "iaus"; R.stealth = StealthName(); R.sneakProfile = sneakProfile;
    g_objEstimates = estimates; g_objScrollRoad = scrollRoad;
    if (!noJson && opt.empty() && headlineOracle > 0) {   // the oracle on the first fights of the same seeds
        g_oracleMode = true; g_oc = oc; R.oracleJ = Objective(D, allClasses, seed, headlineOracle, threads, &R.oracleJScen, &R.oracleJNat); g_oracleMode = false; R.hasOracleJ = true; R.oracleFights = headlineOracle; R.oracleCovers = ObjectiveCovers();
        R.oracleOver = std::string("rollout oracle (") + OracleDesc(oc) + ", " + (oc.ext ? "extended" : "catalogue") + " actions) planning over " + (g_tableMode ? "IAUS (table mode is not planned over)" : estimates ? "IAUS with estimates" : "IAUS as on main") + (sets.empty() ? "" : " + --set");
        std::printf("HEADLINE  ORACLE J=%.2f (scenarios %.2f, natural %.2f)  %d fights per scenario, %d per class natural  covers=%s  [%s]\n", R.oracleJ, R.oracleJScen, R.oracleJNat, headlineOracle, std::max(1, headlineOracle / 2), ObjectiveCovers().c_str(), R.oracleOver.c_str()); }
    if (!noJson && opt.empty()) { R.jVal = Objective(D, allClasses, seed, hf, threads, &R.jScen, &R.jNat); R.hasJ = true; R.jFights = hf; R.jCovers = ObjectiveCovers(); std::printf("HEADLINE  J=%.2f (scenarios %.2f, natural %.2f)  fights=%d per scenario  mode=%s  covers=%s  stealth=%s\n", R.jVal, R.jScen, R.jNat, hf, R.mode.c_str(), R.jCovers.c_str(), StealthName()); } R.label = sets.empty() ? label : label + " [--set " + sets + "]";
    if (!scenario.empty()) {
        std::printf("IAUS SIM %s  list=%s  seed=%llu  scenario=%s  oracle=%d (%s)\n", g_v3.Any() ? "v3" : "v2", D.name.c_str(), (unsigned long long)seed, scenario.c_str(), int(oracle), OracleDesc(oc).c_str());
        if (oracle) { R.oracleKind = std::string(oc.ext ? "rollout over IAUS, class-bound extended action set" : "rollout over IAUS, catalogue actions") + "; " + OracleDesc(oc); R.oracleK = OracleKOf(oc); R.oracleH = oc.H; }
        const int rc = RunScenarios(D, allClasses, scenario, seed, fights, trace >= 0 ? 1 : threads, oracle, oc, g10Sticky, trace, R, estimates, scrollRoad); if (!noJson) R.Write(jsonPath); return rc; }
    std::vector<SimClass> classes;   // --classes a,b,c narrows the cells (slugs)
    for (const SimClass& c : allClasses) if (only.empty() || ("," + only + ",").find("," + c.name + ",") != std::string::npos) classes.push_back(c);
    if (classes.empty()) { std::fprintf(stderr, "no class matches --classes %s\n", only.c_str()); return 2; }
    std::printf("IAUS SIM %s  list=%s  seed=%llu  fights/cell=%d (x5 rapport ranks)  taunted-visible=%d  engine-casts=%d  classes=%s  profiles=%s  mode=%s\n", g_v3.Any() ? "v3" : "v2", D.name.c_str(), (unsigned long long)seed, fights, int(tauntVisible), int(engineCasts), legacy ? "legacy" : "real", mech ? "mech" : "default", g_tableMode ? "table" : "iaus");
    std::printf("world: specs=%zu encounters=%zu foeSpells=%zu  spell pool: raw=%d taught/kept=%d  natures: offense=%d control=%d heal=%d buff=%d summon=%d ward=%d  dropped: placement=%d notCastable=%d reanimate=%d unknown(script)=%d unweighted=%d  kept for table rows: reanimate=%d\n",
                D.specs.size(), D.encounters.size(), D.foeSpells.size(), D.rawSpells, D.taughtSpells, D.natures[1], D.natures[2], D.natures[3], D.natures[4], D.natures[5], D.natures[6], D.dropPlace, D.dropNotCast, D.dropReanim, D.dropUnknown, D.dropUnweighted, D.keptReanim);
    const Rules& r = D.rules;
    std::printf("rules: dmgSkill %.2f..%.2f  armor %.3f/pt max %.0f%% +%.1f%%/piece  armorSkill %.2f..%.2f  power x%.2f  regen(combat) mp %.2f sp %.2f  cost(base %.4f mult %.2f scale %.2f)  hp/level %.1f  toPC x%.2f byPC x%.2f\n",
                r.dmgSkillMin, r.dmgSkillMax, r.armorScale, r.armorMax, r.armorBasePiece * 100, r.armorSkillLo, r.armorSkillHi, 1 + r.powBonus, r.combatMp, r.combatSp, r.costBase, r.costMult, r.costScale, r.hpPerLevel, r.diffToPC, r.diffByPC);

    const int nc = static_cast<int>(classes.size());
    std::vector<Agg> cell(static_cast<std::size_t>(nc * kNB)); std::vector<Agg> rankCell(static_cast<std::size_t>(nc * 5));
    std::atomic<int> next{ 0 }; std::mutex mx;
    auto work = [&]() {
        for (;;) {
            const int job = next.fetch_add(1); if (job >= nc * kNB) return;
            const int c = job / kNB, b = job % kNB; Agg A; Agg RA[5];
            for (int rk = 1; rk <= 5; ++rk) for (int f = 0; f < fights; ++f) {
                Rng R; R.g.seed(seed * 1000003ULL + std::uint64_t(c) * 7919 + std::uint64_t(b) * 104729 + std::uint64_t(rk) * 15485863ULL + std::uint64_t(f) * 32452843ULL);
                bool forced = false; Fight F = RandomFight(D, classes[static_cast<std::size_t>(c)], rk, kBrackets[b], R, t1, &forced);
                F.tauntVisible = tauntVisible; F.engineCasts = engineCasts; F.g10Sticky = g10Sticky; F.estimates = estimates; F.scrollRoad = scrollRoad;
                RunMode(F, true);
                A.uSum += Utility(F); A.Add(F.t, forced); RA[rk - 1].Add(F.t, forced);
            }
            std::scoped_lock lk(mx); cell[static_cast<std::size_t>(job)] = A;
            for (int rk = 0; rk < 5; ++rk) { Agg& d = rankCell[static_cast<std::size_t>(c * 5 + rk)]; d.s.ticks += RA[rk].s.ticks; d.s.idle += RA[rk].s.idle; d.s.hold += RA[rk].s.hold; d.s.switches += RA[rk].s.switches; d.durSum += RA[rk].durSum; }
        }
    };
    { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(work); for (auto& x : th) x.join(); }

    std::map<std::string, std::map<std::string, double>> all; long totalBad = 0, totalInvalid = 0;
    for (int c = 0; c < nc; ++c) for (int b = 0; b < kNB; ++b) { const Agg& A = cell[static_cast<std::size_t>(c * kNB + b)];
        all[classes[static_cast<std::size_t>(c)].name + "|" + kBrackets[b].name] = Metrics(A); totalBad += A.s.badFacts; totalInvalid += A.s.invalid;
        { const SimClass& sc = classes[static_cast<std::size_t>(c)]; ResultRow rr; rr.cls = sc.name; rr.line = sc.line; rr.tier = sc.tier; rr.planned = sc.planned; rr.bracket = kBrackets[b].name; rr.fights = A.fights; rr.metrics = Metrics(A); rr.metrics["persona"] = Persona(sc.name, SchemaMetrics(rr.metrics)); rr.mode = g_tableMode ? "table" : "iaus"; rr.package = sc.package; rr.defaults = sc.defaults; R.rows.push_back(std::move(rr)); } }
    long ratingBad = 0;
    if (ratingN > 0) {   // THE RATING BATTERY: every class of the run on its tier's shared fights (Rating.h)
        std::vector<int> ci; for (const SimClass& c : classes) for (std::size_t i = 0; i < allClasses.size(); ++i) if (allClasses[i].name == c.name) ci.push_back(static_cast<int>(i));
        ratingBad = RatingPass(D, allClasses, ci, { seed }, ratingN, threads, estimates, scrollRoad, R.ratings); R.ratingFights = ratingN; R.ratingCells = kRatingCells;
    }
    for (const GapRow& g : GapRows()) { auto& v = R.groups[g.cls]; if (v.empty()) for (const SimClass& c : classes) if (InGapGroup(c, g.cls)) v.push_back(c.name); }
    if (autoGap && !noClassRulings) g_classRulings = true;   // the automatic gap pass is the class oracle (rulings in force), as --class-oracle is
    if (classOracle > 0) {   // THE GAP PASS: per class, the SAME fights played by IAUS and by the oracle held to the class's rulings: its natural fights and its own named scenarios
        const std::vector<Scenario> hs = HeadlineScenarios(); std::vector<std::pair<int, int>> sj;   // (headline scenario index, class index in `classes`)
        if (gapScen > 0) { Rng R0; R0.g.seed(1); for (std::size_t i = 0; i < hs.size(); ++i) { FightSpec fs = hs[i].make(D, allClasses, R0); for (int c = 0; c < nc; ++c) if (classes[static_cast<std::size_t>(c)].name == fs.cls->name) sj.push_back({ static_cast<int>(i), c }); } }
        struct CO { double uI = 0, uO = 0; Agg orc, iaus; std::vector<double> d; double sI = 0, sO = 0; long sN = 0; std::map<std::string, SitRec> sit; std::array<long, kFamilies> gen{}; long dec = 0; OracleStats st; }; std::vector<CO> co(static_cast<std::size_t>(nc));
        struct SO { double uI = 0, uO = 0; Agg iaus, orc; }; std::vector<SO> so(sj.size());
        std::vector<int> gidx(static_cast<std::size_t>(nc), 0); for (int c = 0; c < nc; ++c) for (std::size_t i = 0; i < allClasses.size(); ++i) if (allClasses[i].name == classes[static_cast<std::size_t>(c)].name) gidx[static_cast<std::size_t>(c)] = static_cast<int>(i);
        std::atomic<int> nx{ 0 }; const int natJobs = nc * classOracle, jobs = natJobs + static_cast<int>(sj.size()) * gapScen;
        auto w2 = [&]() { for (;;) { const int job = nx.fetch_add(1); if (job >= jobs) return; Rng R; Fight F; int c = 0, sidx = -1;
            if (job < natJobs) { c = job / classOracle; const int f = job % classOracle;   // seeded by the class's index in the FULL table, so a class's gap fights do not depend on what else --classes named
                R.g.seed(seed * 1000003ULL + std::uint64_t(gidx[static_cast<std::size_t>(c)]) * 7919 + 15485863ULL * std::uint64_t(1 + f % 5) + std::uint64_t(f) * 32452843ULL);
                F = RandomFight(D, classes[static_cast<std::size_t>(c)], 1 + f % 5, kBrackets[g_gapBracket], R, t1); }
            else { sidx = (job - natJobs) / gapScen; const int f = (job - natJobs) % gapScen, si = sj[static_cast<std::size_t>(sidx)].first; c = sj[static_cast<std::size_t>(sidx)].second;
                R.g.seed(seed * 2654435761ULL + std::uint64_t(si) * 97 + std::uint64_t(f) * 7919); FightSpec fs = hs[static_cast<std::size_t>(si)].make(D, allClasses, R); fs.seed = R.g(); fs.rank = g_forceRank ? g_forceRank : 1 + f % 5; F = BuildFight(D, fs, t1); }
            F.tauntVisible = tauntVisible; F.engineCasts = engineCasts; F.estimates = estimates; F.scrollRoad = scrollRoad; Fight Gf = F;
            RunMode(F); OraclePolicy O; O.cfg = oc; O.cfg.allFollowers = !g_oracleSubjectOnly; std::map<std::string, SitRec> sl; std::array<long, kFamilies> gn{}; if (gapDump) { O.sit = &sl; O.gen = &gn; }
            RunFight(Gf, O); const double uI = Utility(F), uO = Utility(Gf);
            std::scoped_lock lk(mx); CO& r = co[static_cast<std::size_t>(c)]; r.d.push_back(uO - uI); r.st.Merge(O.stSubj);   // the SUBJECT's search only (review F-C): with --oracle-subject-only it is all of it
            if (gapDump) { std::printf("GAPFIGHT %s %s %d %.4f %.4f\n", classes[static_cast<std::size_t>(c)].name.c_str(), sidx < 0 ? "nat" : hs[static_cast<std::size_t>(sj[static_cast<std::size_t>(sidx)].first)].name, job, uI, uO);
                for (const auto& kv : sl) { SitRec& q = r.sit[kv.first]; q.n += kv.second.n; q.dU += kv.second.dU; } for (std::size_t f = 0; f < kFamilies; ++f) r.gen[f] += gn[f]; r.dec += O.subjDec; }
            if (sidx < 0) { r.uI += uI; r.uO += uO; r.orc.uSum += uO; r.orc.Add(Gf.t, false); r.iaus.uSum += uI; r.iaus.Add(F.t, false); }
            else { r.sI += uI; r.sO += uO; ++r.sN; SO& q = so[static_cast<std::size_t>(sidx)]; q.uI += uI; q.uO += uO; q.iaus.uSum += uI; q.iaus.Add(F.t, false); q.orc.uSum += uO; q.orc.Add(Gf.t, false); } } };
        { std::vector<std::thread> th; for (int i = 0; i < threads; ++i) th.emplace_back(w2); for (auto& x : th) x.join(); }
        if (g_gapBracket != 0) std::printf("\nGAP BRACKET %s: the fights below called natural are drawn in the %s strength bracket\n", kBrackets[g_gapBracket].name, kBrackets[g_gapBracket].name);
        std::printf("\nCLASS ORACLE GAP (%d natural fights per class + %d of each of its own scenarios, class rulings %s; %s)\n%-17s %7s %7s %7s  %7s %7s  %7s %5s %6s   (gap = oracle U - IAUS U over all of the class's gap fights; persona: the oracle's own play against the class signature)\n", classOracle, gapScen, g_classRulings ? "IN FORCE" : "off", OracleDesc(oc).c_str(), "class", "IAUS U", "orc U", "nat gap", "pers.I", "pers.O", "GAP", "se", "fights");
        R.oracleKind = std::string("rollout over IAUS, class-bound extended action set") + (g_classRulings ? ", class rulings in force" : "") + (g_oracleSubjectOnly ? ", subject only" : "") + "; " + OracleDesc(oc); R.oracleK = OracleKOf(oc); R.oracleH = oc.H;
        for (int c = 0; c < nc; ++c) { const SimClass& sc = classes[static_cast<std::size_t>(c)]; const CO& r = co[static_cast<std::size_t>(c)]; const double n = classOracle;
            ScenarioRow sr; sr.name = "class:" + sc.name; sr.cls = sc.name; sr.iausU = r.uI / n; sr.oracleU = r.uO / n; sr.gap = sr.oracleU - sr.iausU; sr.hasOracle = true; sr.oracleMetrics = Metrics(r.orc);
            const double pO = Persona(sc.name, SchemaMetrics(sr.oracleMetrics)); sr.oracleMetrics["persona"] = pO; double pI = -1;
            ClassGapRow cg; cg.cls = sc.name; cg.fights = static_cast<long>(r.d.size()); cg.natFights = classOracle; cg.natGap = sr.gap; cg.scenFights = r.sN; cg.scenGap = r.sN ? (r.sO - r.sI) / double(r.sN) : 0.0;
            { double m = 0; for (double x : r.d) m += x; m /= std::max<double>(1, double(r.d.size())); double v = 0; for (double x : r.d) v += (x - m) * (x - m); cg.gap = m; cg.se = r.d.size() > 1 ? std::sqrt(v / double(r.d.size() - 1) / double(r.d.size())) : 0.0; }
            for (ResultRow& rr : R.rows) if (rr.cls == sc.name && rr.bracket == "natural") { rr.hasGap = true; rr.mainGap = cg.gap; pI = rr.metrics["persona"]; sr.metrics = rr.metrics; }
            { auto mi = Metrics(r.iaus); auto G = [](const std::map<std::string, double>& m, const char* k) { auto i = m.find(k); return i == m.end() ? -1.0 : i->second; };   // THE ORACLE-RELATIVE SURVIVAL READ (marth 11-17 "Oracle downs"; Playstyles.h): the same natural fights, IAUS and the class oracle
              std::printf("ORCSURV %s %d  iaus downs/f %.3f selfHealLow%% %.1f  |  oracle downs/f %.3f selfHealLow%% %.1f\n", sc.name.c_str(), classOracle, G(mi, "downs/f"), G(mi, "selfHealLow%"), G(sr.oracleMetrics, "downs/f"), G(sr.oracleMetrics, "selfHealLow%"));
              std::printf("ORCHEAL %s %d  iaus healLate%% %.1f healAt%% %.1f dwh/f %.3f  |  oracle healLate%% %.1f healAt%% %.1f dwh/f %.3f\n", sc.name.c_str(), classOracle, G(mi, "healLate%"), G(mi, "healAt%"), G(mi, "dwh/f"), G(sr.oracleMetrics, "healLate%"), G(sr.oracleMetrics, "healAt%"), G(sr.oracleMetrics, "dwh/f"));   // the heal-timing and ally-survival bands (Playstyles.h): his heals on another begun under Policy.h kOneHitLine, allies down while he held a heal
              std::printf("ORCSTOP %s %s\n", sc.name.c_str(), StopLine(r.st).c_str());   // the search's stopping rule at work (Oracle.h OracleCfg), the SUBJECT's decisions over all the class's gap fights
              for (const auto& kv : r.st.refused) std::printf("ORCREFUSED %s %ld %s\n", sc.name.c_str(), kv.second, kv.first.c_str()); }   // options the class filter refused, by reason
            std::printf("%-17s %7.2f %7.2f %+7.2f  %7.1f %7.1f  %+7.2f %5.2f %6ld\n", sc.name.c_str(), sr.iausU, sr.oracleU, sr.gap, pI, pO, cg.gap, cg.se, cg.fights); R.scen.push_back(std::move(sr)); R.classGaps.push_back(std::move(cg)); }
        for (std::size_t i = 0; i < sj.size(); ++i) { const Scenario& scn = hs[static_cast<std::size_t>(sj[i].first)]; const SO& q = so[i]; const double n = gapScen;
            ScenarioRow sr; sr.name = scn.name; sr.cls = classes[static_cast<std::size_t>(sj[i].second)].name; sr.iausU = q.uI / n; sr.oracleU = q.uO / n; sr.gap = sr.oracleU - sr.iausU; sr.hasOracle = true; sr.metrics = Metrics(q.iaus); sr.oracleMetrics = Metrics(q.orc);
            std::printf("  %-18s %-12s IAUS %6.2f  oracle %6.2f  gap %+6.2f\n", scn.name, sr.cls.c_str(), sr.iausU, sr.oracleU, sr.gap); R.scen.push_back(std::move(sr)); }
        if (gapDump) for (int c = 0; c < nc; ++c) { const CO& r = co[static_cast<std::size_t>(c)]; const char* cn = classes[static_cast<std::size_t>(c)].name.c_str();
            std::printf("GAPDEC %s %ld\n", cn, r.dec);
            for (std::size_t f = 0; f < kFamilies; ++f) std::printf("GAPGEN %s %s %ld\n", cn, std::string(MFO::Iaus::Name(static_cast<Family>(f))).c_str(), r.gen[f]);
            for (const auto& kv : r.sit) std::printf("GAPSIT %s %s %ld %.4f\n", cn, kv.first.c_str(), kv.second.n, kv.second.dU); }
    }
    if (!noJson) R.Write(jsonPath);
    auto G = [](const std::map<std::string, double>& m, const char* k) { auto i = m.find(k); return i == m.end() ? 0.0 : i->second; };
    std::printf("\nDECISIONS\n%-20s %5s %5s %5s %6s %6s %5s %5s %5s %4s %4s %5s %5s %5s  decisions (%% of deciding time)\n", "class|bracket", "idle", "hold", "sw/m", "heal.s", "heal%", "noHC%", "holdL", "iC%", "iR%", "inv", "dec.us", "tnt/m", "tR%");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %5.1f %5.1f %5.1f %6.2f %6.1f %5.0f %5.1f %5.1f %4.1f %4.0f %5.1f %5.1f %5.0f  ", kv.first.c_str(), G(m, "idle%"), G(m, "hold%"), G(m, "sw/min"), G(m, "heal.s"), G(m, "healed%"), G(m, "noHealCand%"), G(m, "holdLow%"), G(m, "iC%"), G(m, "iR%"), G(m, "invalid"), G(m, "dec.us"), G(m, "taunt/min"), G(m, "tR%"));
        for (auto& a : m) if (a.first.rfind("a.", 0) == 0 && a.second >= 1.0) std::printf("%s=%.0f ", a.first.c_str() + 2, a.second);
        std::printf("\n"); }
    std::printf("\nOUTCOMES (absolute)\n%-20s %5s %5s %5s %5s %6s %6s %6s %6s %6s %6s %5s %5s %5s\n", "class|bracket", "win%", "loss%", "pdead", "dn/f", "dur.s", "ttfk", "dps", "tkn/s", "pTkn/s", "mp%", "dry%", "tmo%", "frc%");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %5.0f %5.0f %5.0f %5.2f %6.1f %6.1f %6.1f %6.1f %6.1f %6.0f %5.1f %5.0f %5.0f\n", kv.first.c_str(), G(m, "win%"), G(m, "loss%"), G(m, "pdead%"), G(m, "downs/f"), G(m, "dur.s"), G(m, "ttfk.s"), G(m, "dps"), G(m, "taken/s"), G(m, "pTaken/s"), G(m, "mp.avg%"), G(m, "dry%"), G(m, "timeout%"), G(m, "forced%")); }
    std::printf("\nBODY (what he does, %% of the time he is up): the behaviour signature\n%-20s %6s %6s %6s %6s %6s %6s %6s %6s\n", "class|bracket", "melee", "power", "shoot", "cast", "castI", "block", "other", "free");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f\n", kv.first.c_str(), G(m, "b.melee"), G(m, "b.power"), G(m, "b.shoot"), G(m, "b.cast"), G(m, "b.castIaus"), G(m, "b.block"), G(m, "b.other"), G(m, "b.free")); }
    if (g_postureReport) { std::printf("\nPOSTURE (--posture-report: %% of the time he is up with the bow / a melee weapon drawn; a foe targeting HIM within 256 / 540 units; draws a hit spoiled per fight; shots fired under 256 units)\n%-20s %6s %6s %6s %6s %6s %6s %8s %6s\n", "class|bracket", "bow%", "melee%", "on256", "on540", "contct", "b.mel", "spoil/f", "pb%");
        for (auto& kv : all) { auto& m = kv.second; std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %8.2f %6.1f\n", kv.first.c_str(), G(m, "pr.bow%"), G(m, "pr.melee%"), G(m, "pr.on256%"), G(m, "pr.on540%"), G(m, "contact%"), G(m, "b.melee"), G(m, "pr.spoiled/f"), G(m, "pr.pointBlank%")); } }
    std::printf("\nGUARD UNDER TAUNT (Q-S-5: share of the melee blows that land on him that meet his guard, while two or more foes are taunted onto him; marth: >= 60%% for the Knight line)\n%-20s %8s %8s\n", "class|bracket", "guard2%", "blows/f");
    { std::map<std::string, std::pair<double, double>> agg;   // per class over its brackets (equal fights per bracket): blows-weighted share
      for (auto& kv : all) { auto& m = kv.second; if (G(m, "inc2/f") <= 0.0) continue; std::printf("%-20s %8.1f %8.2f\n", kv.first.c_str(), G(m, "guard2%"), G(m, "inc2/f"));
          auto& a = agg[kv.first.substr(0, kv.first.find('|'))]; a.first += G(m, "guard2%") * G(m, "inc2/f"); a.second += G(m, "inc2/f"); }
      for (auto& kv : agg) std::printf("%-20s %8.1f %8.2f\n", (kv.first + "|ALL").c_str(), kv.second.first / kv.second.second, kv.second.second); }
    std::printf("\nINTELLIGENCE (-1 = no sample)\n%-20s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n", "class|bracket", "ovkil%", "ovhl%", "dblh%", "intr%", "resis%", "immun%", "weak%", "splsh%", "ctlF%", "peel.s", "press%", "prio%", "waste%", "tsw/m", "dwh/f", "wwD/m");
    for (auto& kv : all) { auto& m = kv.second;
        std::printf("%-20s %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.2f %6.1f %6.1f %6.1f %6.1f %6.2f %6.2f\n", kv.first.c_str(), G(m, "overkill%"), G(m, "overheal%"), G(m, "dblheal%"), G(m, "intr%"), G(m, "resisted%"), G(m, "immune%"), G(m, "weak%"), G(m, "splash%"), G(m, "ctrlFail%"), G(m, "peel.s"), G(m, "press%"), G(m, "prio%"), G(m, "waste%"), G(m, "tsw/min"), G(m, "dwh/f"), G(m, "wwDecl/min")); }
    std::printf("\nIdle%% by rapport rank (all brackets):\n");
    for (int c = 0; c < nc; ++c) for (int rk = 0; rk < 5; ++rk) { const Agg& A = rankCell[static_cast<std::size_t>(c * 5 + rk)]; const double t = std::max<double>(1, double(A.s.ticks));
        std::printf("  %-12s rank%d  idle %.1f%%  hold %.1f%%  sw/min %.1f\n", classes[static_cast<std::size_t>(c)].name.c_str(), rk + 1, 100.0 * double(A.s.idle) / t, 100.0 * double(A.s.hold) / t, double(A.s.switches) / std::max(1e-9, A.durSum / 60.0)); }
    totalBad += ratingBad;   // the rating battery's fights are held to the same two checks
    std::printf("\nCHECKS: BAD-FACTS ticks=%ld (must be 0)  invalid picks=%ld (must be 0)  => %s\n", totalBad, totalInvalid, totalBad == 0 && totalInvalid == 0 ? "PASS" : "FAIL");
    char meta[480]; std::snprintf(meta, sizeof meta, "#meta fights=%d seed=%llu stealth=%s profile=%s classes=%s%s t2=%s", fights, (unsigned long long)seed, StealthName(), sneakProfile.c_str(), only.empty() ? (legacy ? "legacy" : "real") : only.c_str(), WorldTag(" ").c_str(), T2Name().c_str());
    if (outBase) { std::ofstream o(outBase); o << meta << "\n"; for (auto& kv : all) for (auto& m : kv.second) if (m.first.find("dec.") == std::string::npos) o << kv.first << "|" << m.first << "=" << m.second << "\n"; std::printf("baseline written: %s\n", outBase); }
    if (cmp) {
        std::ifstream f(cmp); std::string line; int diffs = 0; std::printf("\nCOMPARE vs %s (|delta| > max(0.5, 5%%)):\n", cmp);
        while (std::getline(f, line)) {
            if (!line.empty() && line[0] == '#') {   // B755: a baseline made at another cell size, seed, stealth road or class set is not comparable: refuse, never print a delta
                if (line != meta) { std::printf("COMPARE REFUSED: the baseline was made with\n  %s\nthis run is\n  %s\n", line.c_str(), meta); return 2; }
                continue; }
            const auto eq = line.rfind('='); if (eq == std::string::npos) continue; const std::string k = line.substr(0, eq); const double old = std::atof(line.c_str() + eq + 1);
            const auto p = k.rfind('|'); const auto ci = all.find(k.substr(0, p)); double nv = 0; if (ci != all.end()) { auto mi = ci->second.find(k.substr(p + 1)); if (mi != ci->second.end()) nv = mi->second; }
            if (k.find("dec.") != std::string::npos) continue;
            if (std::fabs(nv - old) > std::max(0.5, 0.05 * std::fabs(old))) { std::printf("  %-34s %9.2f -> %9.2f\n", k.c_str(), old, nv); ++diffs; } }
        std::printf("%d metric(s) changed\n", diffs); }
    return totalBad == 0 && totalInvalid == 0 ? 0 : 1;
}
