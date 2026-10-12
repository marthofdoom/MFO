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

    // One INFO line: "[gems] rules ACTIVE (MEO ABI n, strip available: yes/no)" or "inactive (<why>)".
    // Called once after the MEO interface is acquired and again after every MCM re-read.
    void LogStatus();

    enum class Verdict { Clear, Held };

    // May ALL instances of a_obj leave a_actor? Held = at least one instance carries socketed
    // gems (stripped via MEO, or unknown because the cache is cold) -> the caller SKIPS the item
    // and tries again on a later pass. Clear when inactive. a_why names the path for the log.
    // CACHED: any thread; reads the per-follower carried-gem cache (one scan stale).
    // HOLDS (MEOBridge::HoldForRemoval): the first check of an item arms a hold (the gem reconcile then
    // leaves it alone) and returns Held for that pass; a removal can only proceed on a later pass, once the
    // item reads gem-free under an EXISTING hold. A hold ends when the item leaves the follower, or when the
    // removal is abandoned (ReleaseHoldsExcept) -- never by time. a_touched collects the checked items so a
    // sweep can release the rest.
    enum class Reason : std::uint8_t { Sale = 1, Museum = 2, Evict = 4, Heal = 8, Shed = 16, SwapUp = 32 };
    using HoldKey = std::pair<RE::FormID, std::uint16_t>;   // item base, uid
    Verdict CheckRemovalCached(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, Reason a_reason,
                               std::vector<HoldKey>* a_touched = nullptr);
    // LIVE: MAIN THREAD ONLY; refreshes the cache first, so the read is current.
    Verdict CheckRemovalLive(RE::Actor* a_actor, RE::TESBoundObject* a_obj, const char* a_why, Reason a_reason,
                             std::vector<HoldKey>* a_touched = nullptr);
    // The removal path is no longer a candidate for everything it did not touch this pass: let go of its holds.
    void ReleaseHoldsExcept(RE::FormID a_follower, Reason a_reason, const std::vector<HoldKey>& a_touched);

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

    // ── LOOSE world gear (marth: "the naming scheme should identify gemmed gear as loose items") ──
    // WORKER (LootScan, Category::Equipment): is this loose ref MEO-named as socketed? Matches MEO's
    // exact title "[Support N . ]Gem N[ + Gem N...] BaseName" (N = I..V) AND a nonzero ExtraUniqueID.
    // Unidentified loose gear stays ignored. Returns false for an item already picked up once.
    bool LooseNamedGemmed(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_base);
    // WORKER (Service, just before the activate is posted): true if this ref was recognised by name;
    // records it as taken so a returned item is never re-recognised.
    bool NoteLoosePickup(RE::TESObjectREFR* a_ref);
    // MAIN THREAD, before/after the pickup ActivateRef: capture the ref's uid/position/angle, then
    // register the probe once the item is (or is about to be) in his pack.
    struct LooseCapture { RE::FormID base = 0; std::uint16_t uid = 0; RE::NiPoint3 pos{}; RE::NiPoint3 rot{}; };
    LooseCapture CaptureLoose(RE::TESObjectREFR* a_ref);
    void BeginLooseProbe(RE::Actor* a_follower, const LooseCapture& a_cap, bool a_sellGambit);
    bool SellGambitActive();   // WORKER: does the serviced follower have a loot-valuables (to sell) gambit?

    void ClearTransient();   // revert (pump drained)
}
