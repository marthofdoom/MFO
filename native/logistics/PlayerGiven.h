#pragma once
#include "PCH.h"

// logistics/PlayerGiven.h -- THE "PLAYER GAVE / PLAYER PUT ON" RECORD (batch L review
// round, 2026-09-27). MFO could not tell an item the PLAYER gave a follower, or put
// on him in the trade / gift menu, from one he looted: Logistics::IsPlayerPick covers
// worn ARMO and ammo only, and only under APMF equip ENFORCEMENT. marth's rule:
// player-given items are never shipped. This is the missing signal, for ALL types.
//
//   GIVEN     a TESContainerChangedEvent moving an item from the PLAYER into a
//             follower MFO manages (Followers::IsTrackedFast) -> (follower, base).
//   EQUIPPED  a TESEquipEvent (equipped) on such a follower fired while the
//             ContainerMenu or GiftMenu is open. The game is paused in those menus,
//             so his AI cannot equip then: it is the player's hand. An UNEQUIP in
//             the menu clears the bit (the player changed his mind).
//
// An entry is dropped when the base LEAVES his inventory (a TESContainerChangedEvent
// out of him -> a MAIN-thread recount next frame; nothing is dropped on VR, where
// MainThread::Post is a no-op: fail closed, the item stays "given"), and every entry
// on revert (ClearTransientState). NOT SERIALIZED: a load forgets it.
//
// THREAD: the sinks read their inputs at event time and QUEUE the write (AddTask under
// PumpTickGate, Sinks.cpp's pattern); the leave recount runs on the main thread. The
// table is under its own mutex, so every query is safe from the worker and the main
// thread alike.
namespace MFO::PlayerGiven {

    void RegisterSinks();              // kDataLoaded, main thread, once (after form resolution): the equip sink
    // The container half: called from Sinks.cpp's ContainerSink::ProcessEvent (ONE
    // TESContainerChangedEvent sink for logistics), before its logistics gate.
    void OnContainerChanged(const RE::TESContainerChangedEvent& a_ev);
    void ClearTransientState();        // revert (pump drained)
    bool Installed();                  // the sinks are registered (a worn relic ships only then)

    // The player gave this base to this follower, or put it on him in the menu.
    bool IsPlayerGiven(RE::FormID a_follower, RE::FormID a_base);
    // The player put this base on him in the ContainerMenu / GiftMenu (and has not
    // taken it off there since).
    bool IsPlayerEquipped(RE::FormID a_follower, RE::FormID a_base);
}
