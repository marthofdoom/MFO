#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MEO (marth's Enchanting Overhaul) — SKSE inter-plugin C++ API, v1.
//
// For SKSE-plugin consumers (e.g. MFO). Copy this header into your project. MEO
// must be present in the load order; if it isn't, Request() returns nullptr and
// every call is your responsibility to guard.
//
// Fetch the interface once, after SKSE messaging is up (e.g. in your own
// kPostLoad handler), then keep the pointer:
//
//     MEO_API::InterfaceRequest req{ MEO_API::kABIVersion, nullptr };
//     SKSE::GetMessagingInterface()->Dispatch(
//         MEO_API::kMessage_RequestInterface, &req, sizeof(req),
//         MEO_API::kPluginName);          // CommonLibSSE-NG Dispatch is 4-arg
//     MEO_API::IMEO* g_meo =
//         (req.out && req.out->Version() >= 1) ? req.out : nullptr;
//
// Include this header AFTER CommonLibSSE (it uses RE:: / SKSE::). Threading: call the
// QUERIES (GetSocketCapacity / GetActorGems / GetActorGemsCarried) on the MAIN THREAD
// only — they read live game state (records, inventory, perk globals) without locking.
// MoveGems is safe from ANY thread: it only captures a FormID and queues the work to
// the main thread.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace RE {
    class Actor;
    class TESBoundObject;
    using FormID = std::uint32_t;
}

namespace MEO_API {
    inline constexpr const char*   kPluginName = "MEO";
    inline constexpr std::uint32_t kMessage_RequestInterface = 0x4D454F41;  // 'MEOA'
    // v3: + GetEmptySocketCount / GetGemDetails / GetLooseGems / SocketGem /
    //      UnsocketGem — a follower-management surface that reads and mutates a
    //      specific ACTOR's OWN inventory (never MEO's shared player pouch).
    inline constexpr std::uint32_t kABIVersion = 3;

    // One socketed gem on an actor's gear (worn or merely carried), as reported by
    // GetActorGems / GetActorGemsCarried. POD only — safe across the DLL boundary.
    struct GemInfo {
        char          gid[64];    // stable catalog id, e.g. "firedamage"
        char          name[64];   // display name, e.g. "Fire"
        std::uint8_t  level;      // 1..5
        bool          isArmor;    // armor-domain gem (else weapon-domain)
        bool          isSupport;  // Echo/Conduit/Focus support gem
        float         magnitude;  // BASE effect magnitude at this level (0 for support);
                                  // excludes Focus/Conduit link modifiers + the 2-of-a-kind cap
        RE::FormID    itemBase;   // base form of the worn item it sits in
        std::uint16_t itemUid;    // that item instance's ExtraUniqueID
        std::uint8_t  slot;       // socket slot index (0-based)
    };

    // ABI v2 GemInfo + gem XP/level and the effective (as-applied) magnitude, as
    // reported by GetGemDetails. POD only — safe across the DLL boundary.
    struct GemDetail {
        char          gid[64];
        char          name[64];
        std::uint8_t  level;              // 1..5
        bool          isArmor;
        bool          isSupport;
        float         baseMagnitude;      // base at this level (== GemInfo.magnitude)
        float         effectiveMagnitude; // as actually applied: base × a linked Focus
                                          // boost when present (no 2-of-a-kind cap for
                                          // followers). A Conduit-remapped off-domain gem
                                          // reports baseMagnitude (the remap changes the
                                          // effect's identity — see note below). 0 for support.
        float         xp;                 // banked XP toward the next level
        float         xpToNext;           // XP threshold for the next level (0 if maxed / single-level)
        RE::FormID    itemBase;
        std::uint16_t itemUid;
        std::uint8_t  slot;
    };

    // A loose (unsocketed) gem in an actor's OWN inventory, as reported by
    // GetLooseGems. Instances with banked XP report their uid + xp; a plain stack
    // reports gemUid=0 and count>=1. POD only.
    struct LooseGemInfo {
        char          gid[64];
        char          name[64];
        std::uint8_t  level;      // 1..5
        bool          isArmor;
        bool          isSupport;
        float         magnitude;  // base at this level (0 for support)
        float         xp;         // banked XP (0 for a plain gem)
        float         xpToNext;   // XP threshold for the next level (0 if maxed / single-level)
        RE::FormID    gemBase;    // the gem MISC base form
        std::uint16_t gemUid;     // instance uid (0 = plain/stacked, no banked XP)
        std::uint32_t count;      // stack count (1 for a banked instance; >=1 for a plain stack)
    };

    class IMEO {
    public:
        // == kABIVersion. Check before using (future versions only ADD methods).
        virtual std::uint32_t Version() = 0;

        // Socket slots `a_itemBase` would have, honoring the player's socket perks
        // (Twinned Fitting / Master Jeweler). 0 if the base can't be socketed.
        virtual int GetSocketCapacity(RE::TESBoundObject* a_itemBase) = 0;

        // Write up to a_max GemInfo for the gems currently socketed across a_actor's
        // WORN gear, and return the TRUE count (may exceed a_max; entries past a_max
        // are not written). Pass a_out=nullptr to just get the count. Use this +
        // GetSocketCapacity to preview a looted item "with the current gems."
        virtual std::uint32_t GetActorGems(RE::Actor* a_actor, GemInfo* a_out,
                                           std::uint32_t a_max) = 0;

        // Move a_actor's socketed gems from one worn item instance to another — e.g.
        // when the follower equips looted gear. Up to the destination's capacity and
        // domain (weapon gems -> weapons, armor gems -> armor; support gems fit any
        // dual-socket item); any gem that doesn't fit returns to the shared pouch
        // with its banked XP intact. The destination's uid is minted in place if it
        // has none (a_toUid==0). Queued to the main thread — safe from any thread.
        // Returns true if the move was accepted for queuing.
        virtual bool MoveGems(RE::Actor* a_actor, RE::FormID a_fromBase,
                              std::uint16_t a_fromUid, RE::FormID a_toBase,
                              std::uint16_t a_toUid) = 0;

        // ── ABI v2 (check Version() >= 2 before calling) ────────────────────────
        // Like GetActorGems, but scans the actor's ENTIRE inventory — every carried
        // weapon/armor instance with socketed gems, whether equipped or not — not
        // just worn gear. Worn items are included (they are carried too). Reports
        // only SOCKETED gems; loose gems in the pouch are not items and are not
        // listed. Same GemInfo output, count semantics, and MAIN-THREAD-only rule
        // as GetActorGems.
        virtual std::uint32_t GetActorGemsCarried(RE::Actor* a_actor, GemInfo* a_out,
                                                  std::uint32_t a_max) = 0;

        // ── ABI v3 (check Version() >= 3 before calling) ────────────────────────
        // A follower-management surface. Every method operates on a_actor's OWN
        // inventory — the reads never see MEO's shared player pouch, and the
        // mutations route returned gems to a_actor's own inventory, NOT the pouch.
        // (MEO's own gem-menu still uses the shared pouch; that is unchanged.)
        // All are MAIN-THREAD-only EXCEPT SocketGem/UnsocketGem, which queue to
        // the main thread and are safe from any thread (like MoveGems).

        // Empty socket slots on a specific carried item instance: the item's socket
        // capacity (driven by the PLAYER's socket perks, exactly as MEO's own menu
        // applies them to follower gear) minus the slots already filled. 0 if the
        // item can't be socketed, or if a_itemUid is nonzero and that instance isn't
        // on a_actor. (a_itemUid == 0 reports the base's capacity — nothing filled.)
        virtual int GetEmptySocketCount(RE::Actor* a_actor, RE::FormID a_itemBase,
                                        std::uint16_t a_itemUid) = 0;

        // Per-gem detail (level, XP + xp-to-next, base + effective magnitude) for the
        // gems socketed in ONE carried item instance (a_itemBase/a_itemUid), or across
        // ALL carried items when a_itemBase == 0. Writes up to a_max GemDetail and
        // returns the TRUE count (entries past a_max are not written; a_out may be
        // nullptr to just get the count).
        virtual std::uint32_t GetGemDetails(RE::Actor* a_actor, RE::FormID a_itemBase,
                                            std::uint16_t a_itemUid, GemDetail* a_out,
                                            std::uint32_t a_max) = 0;

        // Loose (unsocketed) gems in a_actor's OWN inventory — the gems available to
        // socket into that actor's gear. Writes up to a_max LooseGemInfo, returns the
        // TRUE count. a_out may be nullptr to just get the count.
        virtual std::uint32_t GetLooseGems(RE::Actor* a_actor, LooseGemInfo* a_out,
                                           std::uint32_t a_max) = 0;

        // Socket a loose gem FROM a_actor's own inventory into slot a_slot of the
        // carried item a_itemBase/a_itemUid. Identify the gem by instance uid
        // (a_gemUid != 0, preserving its banked XP) or by base form for a plain stack
        // (a_gemUid == 0, consumes one, level 1). Domain/capacity are enforced; a gem
        // already in a_slot is evicted back to a_actor's own inventory. The item's uid
        // is minted in place if a_itemUid == 0. Queued to the main thread — safe from
        // any thread. Returns true if accepted for queuing (not whether it fit).
        virtual bool SocketGem(RE::Actor* a_actor, RE::FormID a_itemBase,
                               std::uint16_t a_itemUid, std::uint8_t a_slot,
                               RE::FormID a_gemBase, std::uint16_t a_gemUid) = 0;

        // Unsocket the gem in slot a_slot of a_actor's carried item a_itemBase/
        // a_itemUid; it returns to a_actor's OWN inventory (NOT the shared pouch) with
        // its banked XP intact. Queued to the main thread. Returns true if accepted
        // for queuing.
        virtual bool UnsocketGem(RE::Actor* a_actor, RE::FormID a_itemBase,
                                 std::uint16_t a_itemUid, std::uint8_t a_slot) = 0;

    protected:
        virtual ~IMEO() = default;
    };

    // Dispatched to MEO as the message data. MEO fills `out` synchronously.
    struct InterfaceRequest {
        std::uint32_t abiVersion;  // consumer sets = kABIVersion
        IMEO*         out;         // MEO sets (nullptr on ABI mismatch)
    };
}
