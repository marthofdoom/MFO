#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// APMF (AI Package Management Framework) — SKSE inter-plugin C-ABI.
//
// This is the ONLY file a client shares with APMF. APMF and a client (e.g. MFO)
// compile SEPARATELY and are STRICTLY separate DLLs; they interact ONLY through
// this header at runtime. Because the C++ ABI is NOT stable across two separately
// built DLLs, this surface is deliberately C-ABI: a POD struct of function
// pointers with POD argument types only (RE::FormID, a plain enum, floats, and the
// POD APMF_Param below). NO C++ classes, NO STL, NO vtable ever crosses the
// boundary.
//
// APPEND-ONLY CONTRACT. Once shipped, never change or reorder an existing field,
// enum value, or function-pointer slot in a shipped interface struct — only
// APPEND: add new Intent values at the END, add new function-pointer slots at the
// END of a NEW versioned struct whose leading fields are byte-identical to the
// previous struct, add new fields at the END of APMF_Param, and bump kABIVersion.
// A client built against an older ABI must keep working against every later APMF.
// (Same discipline MFO applies to MEO_API.h.)
//
// ── VERSIONING SHAPE (COM-style prefix extension) ──
// Each ABI revision adds a struct APMF_API_vN whose LEADING members are exactly,
// in order, the members of APMF_API_v(N-1). Because the layouts share an identical
// initial sequence, the SAME static object can be handed to every client:
//   * a v1 client reads it as APMF_API_v1* and sees only the v1 prefix,
//   * a v2 client reads it as APMF_API_v2* and sees the appended slots too.
// The `abiVersion` field (first member of every revision) tells a client how far
// it may safely read. APMF_GetInterface returns the base type (APMF_API_v1*); a
// newer client checks `p->abiVersion >= N` and reinterpret_casts up to APMF_API_vN.
//
// ── How a client obtains the interface (chosen mechanism: exported query fn) ──
// APMF exports one undecorated C function, "APMF_GetInterface", returning a
// pointer to a static POD interface (no ownership, never freed). Fetch it once
// after SKSE load (e.g. your kPostLoad/kDataLoaded), then keep the pointer:
//
//     #include "APMF_API.h"
//     const APMF_API::APMF_API_v2* g_apmf = nullptr;
//     if (HMODULE h = GetModuleHandleA("APMF.dll")) {
//         auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
//             GetProcAddress(h, APMF_API::kGetInterfaceExport));
//         if (fn) {
//             if (auto* base = fn(APMF_API::kABIVersion)) {          // nullptr on ABI mismatch
//                 if (base->abiVersion >= 2)
//                     g_apmf = reinterpret_cast<const APMF_API::APMF_API_v2*>(base);
//             }
//         }
//     }
//     // If g_apmf is null, APMF is absent or too old — guard every call.
//
// (An exported query fn was chosen over the SKSE-messaging handshake MEO uses: it
// is synchronous, has no message-ordering or sender/receiver routing subtlety, and
// hands over a POD struct with no vtable. The struct-of-fn-pointers shape is the
// ABI contract; the transport is just how you get the pointer.)
//
// ── Threading ──
// Request/RequestEx/Release are SAFE FROM ANY THREAD. They capture POD (a FormID,
// a copy of the APMF_Param) and enqueue the work; APMF applies it on the game
// thread. A client's BSJobs worker may call them directly. The APMF_Param pointer
// passed to RequestEx is READ AND COPIED synchronously inside the call — APMF never
// retains the client's pointer, so a stack temporary is fine.
//
// ── Exceptions ──
// NO exception ever crosses this boundary. Every APMF-side body (Request,
// RequestEx, Release, APMF_GetInterface) is wrapped in a catch-all; a throw
// degrades to kInvalidHandle / no-op / nullptr, never an unwind into the client's
// separately compiled DLL (UB).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace RE {
    // Identical alias to CommonLib's RE::FormID; a duplicate identical using-alias
    // is legal, so this header composes with a full CommonLib include.
    using FormID = std::uint32_t;
}

namespace APMF_API {

    inline constexpr std::uint32_t kABIVersion = 2;

    // The exported query function's undecorated name and pointer type.
    // const APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion);
    inline constexpr const char* kGetInterfaceExport = "APMF_GetInterface";

    // A control claim handle. 0 is never a valid handle (returned on refusal).
    using Handle = std::uint32_t;
    inline constexpr Handle kInvalidHandle = 0;

    // What a client wants APMF to do to an NPC. Each maps to exactly one channel
    // family. APPEND-ONLY: never renumber; add new intents at the end.
    enum Intent : std::uint32_t {
        kIntent_None          = 0,
        kIntent_MovementBlock = 1,   // ch.1  full stand-still (block the move intent)
        kIntent_Disposition   = 2,   // ch.11 aggression/confidence/assistance/morality bias
        kIntent_Headtrack     = 3,   // ch.5  look-at (known-incomplete block)
        kIntent_SelectSpell   = 4,   // ch.8  own the cast selection (right hand)
        kIntent_WeaponDrawn   = 5,   // ch.4  draw/sheathe
        kIntent_Dialogue      = 6,   // ch.10 pause current dialogue (one-shot)
        kIntent_Gait          = 7,   // ch.1a gait scale (kSpeedMult)
        kIntent_Detection     = 8,   // ch.16 silent movement + reduced detect range (AVs)
        kIntent_Stance        = 9,   // ch.3  sneak/crouch
        kIntent_CombatTarget  = 10,  // ch.6  combat-target STEER (StartCombat)
        kIntent_Idle          = 11,  // ch.12 one-shot idle/animation
        kIntent_ShoutPower    = 12,  // ch.14 shout/power selection
        kIntent_Equipment     = 13,  // ch.15 unequip/equip a worn item (melee-vs-ranged lever)
    };

    // ── Channel PARAM (ABI v2) ─────────────────────────────────────────────────
    // A POD payload a client passes to RequestEx to say WHICH thing a channel acts
    // on: the spell for cast-select, the target for combat-target, a scalar bias
    // for the AV channels. It is a plain struct (every field always present) rather
    // than a union so it stays trivially POD and unambiguous, and so a channel can
    // read whichever field it needs. All-zero (`{}`) means "no param" — a channel
    // that receives no param (or the v1 Request path) falls back to its default
    // (e.g. cast-select's built-in Firebolt), so v1 behavior is preserved exactly.
    //
    // APPEND-ONLY: never reorder/retype an existing field; add new fields at the END
    // (a v1-era caller zero-inits the whole struct, so appended fields read 0).
    struct APMF_Param {
        RE::FormID   form;   // a form the channel acts on: cast-select => the SpellItem;
                             //   combat-target => the target Actor. 0 => channel default.
        float        fval;   // a scalar: disposition bias / gait scale / detection factor.
        std::int32_t ival;   // an integer/enum variant (e.g. a stance code). 0 => default.
    };

    // The v1 interface: a POD struct of function pointers. NO vtable. `abiVersion`
    // is the first field so a client can sanity-check the layout it received.
    // FROZEN — never edit this struct; a later ABI appends a new struct below.
    struct APMF_API_v1 {
        std::uint32_t abiVersion;   // == kABIVersion of the APMF that produced it

        // Register a control claim: engage `intent`'s channel on the NPC `actor`
        // (a FormID) at arbitration weight `basis`. Returns a handle to release
        // later, or kInvalidHandle if no channel serves `intent`. When two clients
        // claim the SAME channel on the SAME NPC, the higher `basis` owns it; on a
        // tie the earlier claim owns it. The channel stays engaged until the LAST
        // claim is released. Thread-safe (enqueues; applied on the game thread).
        // Equivalent to RequestEx(actor, intent, basis, nullptr): no param, so the
        // channel uses its default.
        Handle (*Request)(RE::FormID actor, Intent intent, float basis);

        // Release a claim previously returned by Request/RequestEx. When the last
        // claim on a channel is released, APMF restores the AI (un-blocks). No-op on
        // an unknown/stale handle. Thread-safe (enqueues). (Complete is a synonym
        // for Release; there is one release operation.)
        void (*Release)(Handle handle);
    };

    // The v2 interface: APMF_API_v1's members verbatim (identical initial sequence),
    // then the appended RequestEx slot. A v1 client reading this object as
    // APMF_API_v1* sees exactly the v1 prefix; a v2 client reads RequestEx too.
    struct APMF_API_v2 {
        std::uint32_t abiVersion;
        Handle (*Request)(RE::FormID actor, Intent intent, float basis);
        void   (*Release)(Handle handle);

        // Like Request, but carries a POD param telling the channel WHICH thing to
        // act on (see APMF_Param). `param` may be nullptr (== Request: channel
        // default). The pointee is read and copied synchronously inside the call;
        // APMF never retains the pointer, so a stack temporary is fine. For
        // cast-select, param->form is the SpellItem the follower's AI should select
        // and cast. Returns a handle, or kInvalidHandle if no channel serves
        // `intent`. Thread-safe (enqueues; applied on the game thread).
        Handle (*RequestEx)(RE::FormID actor, Intent intent, float basis,
                            const APMF_Param* param);
    };

    // Function-pointer type for GetProcAddress(kGetInterfaceExport). Returns the
    // base type; a client that asked for ABI >= 2 checks p->abiVersion and casts up
    // to APMF_API_v2*.
    using GetInterface_t = const APMF_API_v1* (*)(std::uint32_t abiVersion);

}
