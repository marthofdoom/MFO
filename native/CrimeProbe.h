#pragma once
#include "PCH.h"

// [crime-probe] PASSIVE BOUNTY PROBE (ClickUp 1dfcuabc4dp, "500 gold bounties appear in towns"). DEFAULT OFF,
// INI-only: [Debug] bCrimeGoldProbe = 1. LOG-ONLY: it reads, it never writes game state, sends no events and
// holds nothing. Once a second (MAIN thread, via MainThread::Post) it reads the player's violent and
// non-violent crime gold for every faction and, on any INCREASE, logs one line (time, cell, location,
// faction, old -> new) plus one line per managed follower (distance, cell, errand, package, combat target).
// 1.6.1170 only (the two PlayerCharacter vfuncs it calls were disassembled there, see CrimeProbe.cpp).
namespace MFO::CrimeProbe {

    // Called from the sleeper thread every pump wake. Cheap: a flag check and a counter. At ~1 s it posts ONE
    // main-thread sample (never stacks a second while one is queued). No-op while the INI switch is off.
    void Pulse();

    // Register the passive TESHitEvent sink ([crime-probe] FRIENDLY HIT lines: a hit by the player or a managed
    // follower on an NPC that is neither hostile to the player nor in combat with him). Once, at kDataLoaded,
    // from Diagnostics::Install. The sink returns at once while the INI switch is off.
    void RegisterHitSink();

}
