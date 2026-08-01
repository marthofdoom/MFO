#pragma once
#include <RE/Skyrim.h>

// MEO integration (task #17). MEO's SKSE C++ API (MEO_API.h / IMEO) lets MFO
// carry a follower's socketed enchant gems onto looted gear when they upgrade,
// and preview an item "with the current gems." All calls no-op safely when MEO
// is absent (Acquire got nullptr) -- MFO never hard-depends on it.
//
// THREADING (MEO's contract): MoveGems is safe from any thread (it queues to
// the main thread). The QUERIES (SocketCapacity / ActorGems / the comparison)
// read live game state without locking -> MAIN THREAD ONLY. MFO's loot tick is
// a job worker, so it must NOT call the queries directly; it schedules the gem
// move on a TESEquipEvent (main thread) instead -- see QueueGemMove.
namespace MFO::MEOBridge {

    // Fetch the IMEO interface. Call once after SKSE messaging is up (kPostLoad).
    void Acquire();
    bool Available();

    // Register the TESEquipEvent sink that flushes pending gem moves. Call at
    // kDataLoaded (after form resolution, with the other sinks).
    void RegisterSink();

    // Save-scoped: the pending-gem-move map keys on (followerFormID, toBase) and
    // holds source-item FormIDs for a swap that hasn't flushed yet. Cleared on
    // revert -- else a stale key could match an equip event for a REUSED FormID
    // next session and move gems onto the wrong actor (save-safety audit).
    void ClearTransientState();

    // Worker-safe. Record that when `a_follower` next EQUIPS `a_toBase`, MEO
    // should move the gems from their old worn item `a_fromBase`/`a_fromUid`
    // into it. The move fires from the equip event (main thread), once the
    // destination is actually worn so MEO can mint its uid. No-op if MEO absent
    // or a_fromUid==0 (the old item carried no gems).
    void QueueGemMove(RE::Actor* a_follower, RE::FormID a_fromBase,
                      std::uint16_t a_fromUid, RE::FormID a_toBase);

    // The ExtraUniqueID of the worn instance of `a_base` on `a_actor`, or 0 if
    // it has none (no gems). MAIN-THREAD read of inventory -- but MFO calls it
    // from the loot tick against a follower whose 3D is loaded and whose worn
    // extras are stable across the tick; it only reads, mirroring MEO's own
    // FindInstanceXList walk. Returns 0 on any miss.
    std::uint16_t WornUid(RE::Actor* a_actor, RE::TESBoundObject* a_base);

    // ── gem-simulated comparison (MAIN THREAD ONLY) ─────────────────────────
    // "See the compared stats WITH the follower's current gems socketed into a
    // candidate." For a board/preview on the render thread -- NOT the worker
    // loot decision. Returns the candidate's socket capacity honoring perks, and
    // the total BASE magnitude of the actor's current gems that would fit it
    // (domain + capacity simulated). 0/0 when MEO absent.
    struct GemPreview {
        int   capacity;      // sockets the candidate would have
        int   gemsHeld;      // gems the actor currently carries (worn)
        int   gemsThatFit;   // how many would move into the candidate
        float magnitudeFit;  // summed base magnitude of the fitting gems
    };
    GemPreview PreviewWithGems(RE::Actor* a_actor, RE::TESBoundObject* a_candidateBase);
}
