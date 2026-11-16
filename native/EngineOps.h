#pragma once
#include "PCH.h"

// ENGINE WRITES RUN ON THE MAIN THREAD (INVARIANTS #83, ENGINE_NOTES 0.30 amendment 2026-11-16b; stability audit M2 / M3).
//
// MFO's tick runs on a job worker (ENGINE_NOTES 0.30) and nothing proves that worker and the main thread exclusive (#74). An
// engine call that WRITES an actor's inventory, equip state, spell hands, magic caster, package or animation graph on the
// worker races the main thread's own readers and writers of the same lists. The proof that even the "queued" kind is not
// inventory-safe is the unequip: InventoryChanges' unwear (1.6.1170 0x22CA20) queues only a WEAPON's 3D to the main thread;
// the worn extra is cleared, the copy is merged back into its stack and an ExtraDataList node is unlinked and freed on the
// CALLING thread, for every form type. So no engine write stays on the tick.
//
// Every function here has ONE behaviour on both threads:
//   * where MainThread::EngineWritesAllowed() (the main seat, the main thread outside a worker tick, or no pump installed:
//     VR / an unverified build, the documented legacy path) it makes the engine call IN PLACE, exactly as the call it replaces;
//   * anywhere else (the worker tick, an AddTask closure) it POSTS the call to the main thread: FormIDs and values only, both
//     forms re-resolved on the frame that runs, first in first out with every other post (so "unequip A, equip B, draw" keeps
//     its order). The caller never waits. Latency: the next player Update (one frame; a paused game holds it until it resumes).
// The worker keeps its gates and its ledgers (it records what it ASKED for, as it always did: an equip was already allowed
// to land late). A posted call that cannot run (the actor no longer resolves, the item left his pack in between) is SAID
// (`[engine-op]`, rate-limited per actor and form) and does nothing. It is never retried and never worked around.
//
// TO ADD A NEW ENGINE WRITE: add a function here (the in-place body + the posted body through Run), call
// MainThread::NoteEngineWrite on the line before the engine call, and list it in INVARIANTS #83. Never call the engine's
// write from tick code directly.
namespace MFO::EngineOps {

    // ActorEquipManager::EquipObject / UnequipObject with a null ExtraDataList (no caller passes one: an extra-list pointer
    // does not survive the hop). The parameters after a_actor / a_object are the engine's own, in its order and with its defaults.
    // Posted Equip only: he must still hold at least one a_object on the frame that runs (a live count), else it is said and skipped.
    void Equip(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count = 1, const RE::BGSEquipSlot* a_slot = nullptr,
               bool a_queueEquip = true, bool a_forceEquip = false, bool a_playSounds = true, bool a_applyNow = false);
    void Unequip(RE::Actor* a_actor, RE::TESBoundObject* a_object, std::uint32_t a_count = 1, const RE::BGSEquipSlot* a_slot = nullptr,
                 bool a_queueEquip = true, bool a_forceEquip = false, bool a_playSounds = true, bool a_applyNow = false);

    // ActorEquipManager::EquipSpell / Actor::DeselectSpell (a hand-held spell is SELECTED, not inventory-equipped).
    void EquipSpell(RE::Actor* a_actor, RE::SpellItem* a_spell, const RE::BGSEquipSlot* a_slot);
    void DeselectSpell(RE::Actor* a_actor, RE::SpellItem* a_spell);

    // Actor::DrawWeaponMagicHands.
    void DrawHands(RE::Actor* a_actor, bool a_draw);

    // Actor::EvaluatePackage(true, false). resetAI is ALWAYS false (CLAUDE.md "five things" 3: the combat group).
    void EvaluatePackage(RE::Actor* a_actor);

    // MagicCaster::InterruptCast on the actor's caster for a_source (no caster there = nothing).
    void InterruptCast(RE::Actor* a_actor, RE::MagicSystem::CastingSource a_source, bool a_refund);

    // THE DRINK: ActorEquipManager::EquipObject(actor, potion, nullptr, 1), the AlchemyItem equip path (the vanilla drink; the
    // engine removes the unit and plays what it always played). Posted: on the frame that runs he must still be able to act
    // (Actuation::CannotActReason) and still hold one, else it is said and nothing is drunk. a_road names the caller in the log.
    void Drink(RE::Actor* a_actor, RE::AlchemyItem* a_potion, const char* a_road);

    // How many a_object a_ref holds, read from the LIVE inventory. MAIN THREAD (it names itself: MainThread::NoteLiveRead).
    std::int32_t LiveCountOnMain(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_object);

    // Counters for the `[overlap]` line: calls made in place, calls posted, posted calls that could not run.
    struct Stats { std::uint64_t inPlace = 0, posted = 0, skipped = 0; };
    Stats GetStats();
}
