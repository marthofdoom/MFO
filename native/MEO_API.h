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
// QUERIES (GetSocketCapacity / GetActorGems) on the MAIN THREAD only — they read live
// game state (records, inventory, perk globals) without locking. MoveGems is safe from
// ANY thread: it only captures a FormID and queues the work to the main thread.
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
    inline constexpr std::uint32_t kABIVersion = 1;

    // One socketed gem on an actor's worn gear, as reported by GetActorGems.
    // POD only — safe across the DLL boundary.
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

    protected:
        virtual ~IMEO() = default;
    };

    // Dispatched to MEO as the message data. MEO fills `out` synchronously.
    struct InterfaceRequest {
        std::uint32_t abiVersion;  // consumer sets = kABIVersion
        IMEO*         out;         // MEO sets (nullptr on ABI mismatch)
    };
}
