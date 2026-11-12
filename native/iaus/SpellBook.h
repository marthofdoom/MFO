#pragma once
#include "PCH.h"

#include "classes/Classes.h"
#include "iaus/Core.h"
#include "iaus/Prune.h"
#include "iaus/Snapshot.h"

// iaus/SpellBook.h -- the per-follower MAIN-THREAD MIRROR the IAUS candidate generator reads (card I4, design
// section 1.5). The idea of IAUS for MFO is LandingCrew's (credit in iaus/Core.h).
//
// WHAT IT HOLDS, per follower: every castable spell he knows (Actor::VisitSpells), every scroll and staff he
// carries (count, staff charge), and the small KIT facts that need an inventory walk (potion stock, which weapon
// categories he carries, ammunition). Each spell entry caches what the record says: nature (the action family it
// is a candidate for), element (the costliest hostile effect's resistVariable), magicka cost for THIS caster,
// reach, area, the class bits, whether only the unanimated road can cast it, whether its summon is alive and on
// which party members its effect is already running.
//
// IAUS UNDERSTANDS SPELLS LIVE FROM THEIR RECORDS (marth): nothing here reads Synthesis data or a per-list table.
// The nature comes from cast/'s own classifiers (CasterConsent::ClassifySpell, Actuation::ClassifyArchetype,
// IsSummonSpell, SpellNatureUndeclared, CastRoadUnsupported), never from a second rule. A spell whose record does
// not say who it is for (Prune::Nature::Unknown) is logged once per session and is never a candidate.
//
// THREADING (the SelfStatus pattern, SelfStatus.h):
//   - MEASURED on the MAIN thread only: one MainThread::Post per refresh. VisitSpells, GetInventory, the active
//     effect lists and CalculateMagickaCost all read state the main thread mutates.
//   - READ on the AddTask job worker (Read): a leaf-mutex map read that hands out an immutable snapshot
//     (shared_ptr<const Book>). The worker never walks a spell list, an inventory or an effect list for IAUS.
//   - The mutex is a LEAF: no engine call, no spdlog call and no other MFO lock is ever taken under it.
//   - Invalidate / OnContainerChanged: any thread (they only mark an entry dirty under the leaf mutex).
//   - Clear(): ResetAllState, after StopPump, beside SelfStatus::Clear (card I6 wires it; nothing calls it yet).
//     A refresh that was already running when Clear ran is dropped by a generation check, so a pre-load book can
//     never be written into the next session.
//   - VR (no pump, MainThread::IsInstalled false): Read returns nullptr forever, so an IAUS follower there has no
//     spell candidates. A documented degrade, never a worker-side list walk.
//
// REFRESH TRIGGERS (all inside Read, so no other file drives them): no measurement yet (the first Read of a
// fight is "combat start"), a book kRefreshSec (5 s) old, or a dirty mark (an item entered or left his inventory:
// logistics/Sinks.cpp's one TESContainerChangedEvent sink calls OnContainerChanged; card I6 calls Invalidate after
// a Fired summon, drink or scroll). A lost refresh (a load drops the pump's queue) is re-asked after kAskRetrySec.
namespace MFO::Iaus::SpellBook {

    using Source = SpellSource;   // the content types are plain data in iaus/Snapshot.h (the simulator builds them)
    using Entry  = SpellEntry;
    using Kit    = Iaus::Kit;
    using Book   = Iaus::Book;

    // The follower's class as the generator gates on it (design section 2; the fields are in iaus/Snapshot.h): the
    // EQUIPPED class when the class tree is on (Classes::Active), else the T1 class of his resolved combat class
    // (Followers::ResolvedClassFast: the Gambit-tab pick, or the Auto vote). ANY THREAD: the Classes:: accessors
    // and ResolvedClassFast are mirrors. resolved == false (a benched follower left on Auto): nothing is gated.
    [[nodiscard]] ClassGate GateFor(RE::FormID a_follower);
    // A casting class (ruling Q34 / Q36): the mage line and every hybrid (all six T3 hybrids, Grand Battlemage
    // and Dragonborn pair a weapon line with a magic school).
    [[nodiscard]] bool CastingClass(const Classes::ClassDef& a_def);

    // Would IAUS weigh this spell for a follower with this class gate? (batch I item 7, the Teach Spells page.) The static
    // verdict MakeEntry gives every book entry (castable, a road MFO can cast it by, not a placement spell, a nature the record
    // states, an effect list IAUS can weight: item 6) plus the class fit of ruling Q34 (the live ClassGate's spellsOnClass /
    // spellsFromMastered, exactly as Generate reads them). Form data only: any thread. a_why (optional) gets a static literal
    // when the answer is no.
    [[nodiscard]] bool WouldWeigh(RE::SpellItem* a_spell, const ClassGate& a_gate, const char** a_why = nullptr);

    // WORKER. The last measured book, or nullptr when there is none fresh enough (never read, older than
    // kTrustSec, VR). Posts ONE main-thread refresh when the book is missing, kRefreshSec old or dirty.
    [[nodiscard]] std::shared_ptr<const Book> Read(RE::Actor* a_follower);

    // ANY THREAD. Mark a_follower's book dirty: the next Read re-measures it at once.
    void Invalidate(RE::FormID a_follower);
    // ANY THREAD (the TESContainerChangedEvent sink): an item moved between a_old and a_new. Marks the book of
    // either side dirty when one exists (a container that is not a mirrored follower costs one map lookup).
    void OnContainerChanged(RE::FormID a_old, RE::FormID a_new);

    // ResetAllState, after StopPump (card I6 wires it). Drops every book and the once-per-session log ledgers.
    void Clear();

    [[nodiscard]] const char* NatureName(Prune::Nature a_n);

}
