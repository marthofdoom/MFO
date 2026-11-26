#pragma once
#include "PCH.h"

// compat/Quartermaster.h -- SKYRIM QUARTERMASTER (Nexus SSE 198145) AWARENESS.
//
// Quartermaster dresses followers in the player's outfit sets (combat / casual / home / cold) and picks their weapons
// by hand rules. The sets a follower is not wearing right now sit in his own pack, so without this:
//   * a town errand sells them as unworn junk (Economy.cpp sell loop), and
//   * MFO's own armor pick, declared to Harbinger with the Armor category owned, makes Harbinger refuse every
//     set switch Quartermaster makes, and MFO's pick wins.
// Quartermaster 1.1.2+ exports a small C API from Quartermaster.dll (no header, GetProcAddress); without it, or with
// an older Quartermaster, every query answers false and MFO behaves exactly as before.
//
//   KeepsItem(follower, base)  a base form one of his Quartermaster sets or hand rules names: never sold.
//   DressesArmor(follower)     Quartermaster puts his armor on: MFO treats him as Fashionrim (bDollsMode) for armor
//                              only, and does not own the Armor category in his Harbinger declaration.
//
// ANY THREAD: Quartermaster answers from a snapshot it writes once per scan under its own lock.
namespace MFO::QuartermasterCompat {

    bool Present();
    bool KeepsItem(RE::FormID a_follower, RE::FormID a_item);
    bool DressesArmor(RE::FormID a_follower);

    // Fashionrim for this follower: the global bDollsMode, or Quartermaster dresses him.
    bool DollsFor(RE::FormID a_follower);
    inline bool DollsFor(const RE::Actor* a_follower) {
        return DollsFor(a_follower ? a_follower->GetFormID() : 0);
    }
}
