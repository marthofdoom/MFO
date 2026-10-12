#pragma once
#include "PCH.h"

// logistics/GemRules.h -- MEO-AWARE GEM RULES (ClickUp 86e3p2gp8, marth's rulings).
//   * A socketed item NEVER leaves a follower through MFO (sell / drop / shed / hand-back /
//     museum donation / swap-up drop). Every removal path asks CheckRemoval* first: an item with
//     socketed gems is HELD (its gems are queued out through MEO; the removal happens on a later
//     pass once the item reads gem-free). The PLAYER's own trade-menu take is not an MFO path.
//   * Looted gear is probed for gems (ProbeLootedGear) and, if it is not worth keeping gemless,
//     gets put back where it was found (container only).
// ALL of it is gated on bMeoAwareGems AND the MEO interface (ABI >= 2 to see gems, >= 3 to strip).
// With the toggle OFF every function here is a no-op and the legacy sell path is unchanged.
// MEO_API.h is untouched (byte-shared); only existing calls are used, via MEOBridge.
namespace MFO::Logistics::GemRules {

    // bMeoAwareGems && MEO present with ABI >= 2.
    bool Active();

    enum class Verdict { Clear, Held };

    // May ALL instances of a_obj leave a_actor? Held = at least one instance carries socketed
    // gems (stripped via MEO, or unknown because the cache is cold) -> the caller SKIPS the item
    // and tries again on a later pass. Clear when inactive. a_why names the path for the log.
    // CACHED: any thread; reads the per-follower carried-gem cache (one scan stale).
    Verdict CheckRemovalCached(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why);
    // LIVE: MAIN THREAD ONLY; refreshes the cache first, so the read is current.
    Verdict CheckRemovalLive(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why);

    // ── looted gear ──────────────────────────────────────────────────────────
    // WORKER (LootEquipment). A weapon/armor in a_src that is not otherwise being taken but may
    // carry gems (MEO mints an ExtraUniqueID when it sockets: a uid==0 item has none) and has not
    // been probed yet. Returns the item + the source's ExtraDataList for that instance, or null.
    struct GemCandidate { RE::TESBoundObject* obj = nullptr; RE::ExtraDataList* xl = nullptr; std::uint16_t uid = 0; };
    GemCandidate FindGemCandidate(RE::Actor* a_follower, RE::TESObjectREFR* a_src);
    // WORKER: take the candidate (this instance only) and hand it to the main-thread probe.
    // True if it arrived.
    bool TakeAndProbe(RE::Actor* a_follower, RE::TESObjectREFR* a_src, const GemCandidate& a_c);
    // WORKER: schedule the main-thread follow-up of pending probes for a_follower (cheap no-op
    // when none are pending).
    void PumpProbes(RE::Actor* a_follower);

    void ClearTransient();   // revert (pump drained)
}
