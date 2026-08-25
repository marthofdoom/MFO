#pragma once
#include <RE/Skyrim.h>
#include "MEO_API.h"   // MEO_API::GemInfo for the CarriedGems wrapper (Build A)

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

    // ── Build A: ACCURATE carried-gem sell-skip (MEO ABI v2) ─────────────────
    // The economy sell path must skip only items that ACTUALLY carry a socketed
    // gem (not any bare-uid instance -- that over-block sold nothing in Tuxborn).
    // GetActorGemsCarried scans the WHOLE inventory (worn OR carried) for socketed
    // gems. It is MAIN-THREAD only, so the worker reads a per-follower cache that
    // the main thread refreshes.

    // Raw wrapper over IMEO::GetActorGemsCarried. MAIN-THREAD ONLY. Returns 0 (no
    // gems written) when MEO is absent or its Version() < 2.
    std::uint32_t CarriedGems(RE::Actor* a_actor, MEO_API::GemInfo* a_out, std::uint32_t a_max);

    // Rebuild a_actor's carried-gemmed (base,uid) cache from CarriedGems.
    // MAIN-THREAD ONLY (calls the query). Stores an empty set when MEO < v2.
    void RefreshCarriedGems(RE::Actor* a_actor);

    // Worker-callable: schedule a MAIN-THREAD RefreshCarriedGems for a_follower so
    // the cache is warm for the NEXT scan (one-scan-stale is fine -- gem state
    // changes rarely). No-op when MEO < v2 (cache stays empty -> no skip).
    void RequestCarriedGemRefresh(RE::Actor* a_follower);

    // Worker-callable READ: is the item instance (a_base, a_uid) a gem-carrying
    // item in a_follower's cache? False for uid 0 / empty cache / MEO < v2.
    bool IsCarriedGemmed(RE::FormID a_followerID, RE::FormID a_base, std::uint16_t a_uid);

    // Worker-callable READ: has a_follower's carried-gem cache been populated at
    // least once (a RefreshCarriedGems completed for it)? The key EXISTS iff so,
    // even with an empty gem set. Until warmed, the sell path stays conservative
    // (skip any nonzero-uid instance) so a first-scan gemmed spare can't sell.
    bool CacheWarmed(RE::FormID a_followerID);

    // Worker-callable: is the carried-gem feature usable (MEO present, ABI >= 2)?
    // The cold-cache conservative skip must be gated on this -- WITHOUT MEO v2 the
    // cache never warms, so "not warmed -> skip any uid" would become the permanent
    // bare-uid over-block bug for old-MEO users. False -> the sell path does NO
    // gem-skip at all (worn + keepWeapons/keepArmor still protect worn gems).
    bool CarriedGemsSupported();

    // Worker-callable: does MEO support UNSOCKETING (ABI >= 3)? When true the sell
    // path UNGEMS-THEN-SELLS: it extracts a gemmed junk item's gems to the follower's
    // own inventory (UnsocketItemGems), so the item sells ungemmed on a later scan
    // while the loose gem is kept. When false (v2 only) it falls back to protect-and-
    // skip (detection without extraction).
    bool CarriedGemUnsocketSupported();

    // Worker-callable: queue UnsocketGem for EVERY filled gem slot of the follower's
    // carried item (a_base,a_uid) -> returns the gems to the follower's OWN inventory
    // (banked XP intact). No-op if MEO < v3, the item isn't in the gem cache, or an
    // extract for it is already pending (de-duped so an async unsocket isn't
    // re-queued each scan). Any-thread (UnsocketGem queues to the main thread).
    void UnsocketItemGems(RE::Actor* a_actor, RE::FormID a_base, std::uint16_t a_uid);

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
