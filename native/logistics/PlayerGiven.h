#pragma once
#include "PCH.h"

// logistics/PlayerGiven.h -- THE "PLAYER GAVE / PLAYER PUT ON" RECORD (batch L review
// round, 2026-09-27). MFO could not tell an item the PLAYER gave a follower, or put
// on him in the trade / gift menu, from one he looted: Logistics::IsPlayerPick covers
// worn ARMO and ammo only, and only under APMF equip ENFORCEMENT. marth's rule
// (2026-09-27, ClickUp 86e3faccn): "What you drop on the ground is fair game. As
// well as regular items given to followers. Relics however should be recorded."
// So this records MUSEUM RELICS ONLY (Lotd::MayBeRelic: any base the LOTD slot
// table accepts, needed now or not, at the time of the gift / equip; fail closed
// before the first snapshot). A non-relic gift is not recorded and nothing blocks it.
//
//   GIVEN     a TESContainerChangedEvent moving a RELIC from the PLAYER into a
//             follower MFO manages (Followers::IsTrackedFast) -> (follower, base).
//   EQUIPPED  a TESEquipEvent (equipped) of a RELIC on such a follower fired while the
//             ContainerMenu or GiftMenu is open. The game is paused in those menus,
//             so his AI cannot equip then: it is the player's hand. An UNEQUIP in
//             the menu clears the bit (the player changed his mind). An equip that
//             MFO's own FWPN hold names is NOT recorded (MFO's equip landing in a menu).
//   AI-EQUIPPED  an equip with NO such menu open of a relic MFO had seen UNWORN in
//             his pack as a kept relic (Lotd::SeenUnwornKept), not held by MFO and not
//             the player's: the positive proof the worn-relic ship and the relic swap
//             require. NOT a player bit (IsPlayerGiven ignores it).
//
// An entry is dropped when the base LEAVES his inventory (a TESContainerChangedEvent
// out of him -> a MAIN-thread recount next frame; nothing is dropped on VR, where
// MainThread::Post is a no-op: fail closed, the item stays "given"), and every entry
// on revert (ClearRecord, ResetAllState). SAVED: the PLAYER bits (GIVEN, EQUIPPED)
// ride their own co-save record 'PGIV' v1 (Serialization.h), so a relic the player
// gave never ships and is never swapped out after a reload. The AI-EQUIPPED mark is
// NOT saved (after a load it is false until his AI equips it again: fails closed).
//
// THREAD: the sinks read their inputs at event time and QUEUE the write (AddTask under
// PumpTickGate, Sinks.cpp's pattern); the leave recount runs on the main thread. The
// table is under its own mutex, so every query is safe from the worker and the main
// thread alike. CoSave / CoLoad run on the SKSE save / load callbacks (main thread,
// the pump paused / stopped); they take the same mutex (CoSave snapshots under it and
// writes from the copy -- no lock across a WriteRecordData).
namespace MFO::PlayerGiven {

    void RegisterSinks();              // kDataLoaded, main thread, once (after form resolution): the equip sink
    // The container half: called from Sinks.cpp's ContainerSink::ProcessEvent (ONE
    // TESContainerChangedEvent sink for logistics), before its logistics gate.
    void OnContainerChanged(const RE::TESContainerChangedEvent& a_ev);
    void ClearRecord();                // revert: ResetAllState, after StopPump (the record is SAVED, 'PGIV')
    std::size_t ForgetFollower(RE::FormID a_follower);   // roster removal (86e3eewaf): drop his PGIV entries, returns how many
    void CoSave(SKSE::SerializationInterface* a_intfc);                          // 'PGIV' v1
    void CoLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version); // 'PGIV' (version-checked by the caller)
    bool Installed();                  // the sinks are registered (a worn relic ships only then)

    // The player gave this RELIC to this follower, or put it on him in the menu.
    bool IsPlayerGiven(RE::FormID a_follower, RE::FormID a_base);
    // The player put this base on him in the ContainerMenu / GiftMenu (and has not
    // taken it off there since).
    bool IsPlayerEquipped(RE::FormID a_follower, RE::FormID a_base);
    // POSITIVE PROOF his own AI put this relic on (review round 3): an equip with no
    // trade / gift menu open, of a base Lotd::SeenUnwornKept (MFO saw it unworn in his
    // pack as a kept relic), not named by an MFO hold, not the player's. NOT saved:
    // after a load it is false until the AI equips it again (fails closed).
    bool IsAiEquipped(RE::FormID a_follower, RE::FormID a_base);
}
