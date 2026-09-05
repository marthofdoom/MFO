#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// APMF (AI Package Management Framework) — SKSE inter-plugin C-ABI.
//
// This is the ONLY file a client mod shares with APMF. APMF and a client
// compile SEPARATELY and are STRICTLY separate DLLs. They interact ONLY through
// this header at runtime. Because the C++ ABI is NOT stable across two separately
// built DLLs, this surface is deliberately C-ABI: a POD struct of function
// pointers with POD argument types only (RE::FormID, a plain enum, floats, and the
// POD APMF_Param below). NO C++ classes, NO STL, NO vtable ever crosses the
// boundary.
//
// APPEND-ONLY CONTRACT. Once shipped, never change or reorder an existing field,
// enum value, or function-pointer slot in a shipped interface struct -- only
// APPEND: add new Intent values at the END, add new function-pointer slots at the
// END of a NEW versioned struct whose leading fields are byte-identical to the
// previous struct, add new fields at the END of APMF_Param, and bump kABIVersion.
// A client built against an older ABI must keep working against every later APMF.
// (The same discipline any byte-shared C-ABI header between two separately
// compiled DLLs needs.)
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
//     const APMF_API::APMF_API_v4* g_apmf = nullptr;   // pick the newest struct you use
//     if (HMODULE h = GetModuleHandleA("APMF.dll")) {
//         auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
//             GetProcAddress(h, APMF_API::kGetInterfaceExport));
//         if (fn) {
//             if (auto* base = fn(APMF_API::kABIVersion)) {          // nullptr on ABI mismatch
//                 if (base->abiVersion >= 4)
//                     g_apmf = reinterpret_cast<const APMF_API::APMF_API_v4*>(base);
//             }
//         }
//     }
//     // If g_apmf is null, APMF is absent or too old — guard every call. (A client
//     // that only needs v2 checks `>= 2` and casts to APMF_API_v2*; a v3 field like
//     // Repoint requires `>= 3`; a v4 field like SetSpellAllowList requires `>= 4`.)
//
// (An exported query fn was chosen over the SKSE-messaging handshake MEO uses: it
// is synchronous, has no message-ordering or sender/receiver routing subtlety, and
// hands over a POD struct with no vtable. The struct-of-fn-pointers shape is the
// ABI contract; the transport is just how you get the pointer.)
//
// ── Threading ──
// Request/RequestEx/Repoint/Release/SetSpellAllowList are SAFE FROM ANY THREAD.
// They capture POD (a FormID, a copy of the APMF_Param, or — for
// SetSpellAllowList — a copy of the forms array) and enqueue the work; APMF
// applies it on the game thread. A client's BSJobs worker may call them
// directly. The APMF_Param pointer passed to RequestEx/Repoint, and the
// RE::FormID* passed to SetSpellAllowList, are READ AND COPIED synchronously
// inside the call — APMF never retains the client's pointer, so a stack
// temporary/local array is fine.
//
// ── Exceptions ──
// NO exception ever crosses this boundary. Every APMF-side body (Request,
// RequestEx, Repoint, Release, APMF_GetInterface) is wrapped in a catch-all; a throw
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

    inline constexpr std::uint32_t kABIVersion = 5;

    // The exported query function's undecorated name and pointer type.
    // const APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion);
    inline constexpr const char* kGetInterfaceExport = "APMF_GetInterface";

    // A control claim handle. 0 is never a valid handle (returned on refusal).
    using Handle = std::uint32_t;
    inline constexpr Handle kInvalidHandle = 0;

    // ABI v4: the bound on SetSpellAllowList's allow-set. Chosen generously above
    // what a follower's realistic known heal/buff spell count needs (typically
    // single digits to low teens even for a heavily-modded mage build) -- an
    // overflow degrades to "the excess spells are treated as non-exempt" (still
    // denied, never a crash or an unbounded write). See SetSpellAllowList below.
    inline constexpr std::uint32_t kMaxSpellAllowList = 32;

    // Which FACET a client claims control of on an NPC. APMF MODERATES that facet
    // (arbitrates who owns it + DENYs competitors); it never generates the behavior --
    // the client executes with its own mechanisms (design.md Section 1a). Each maps
    // to one channel family. APPEND-ONLY: never renumber; add new intents at the end.
    //
    // Each entry below follows the same shape: the facet, its Mode, and which
    // APMF_Param field it uses. Three modes exist (design.md Section 1a's three
    // legal channel actions):
    //   ARBITRATE -- APMF records who owns the facet. The client executes the
    //                 behavior with its own mechanism. APMF calls nothing.
    //   DENY      -- APMF suppresses the losing input at its source (an actor
    //                 value, a vfunc answer, a package offer). No re-assert.
    //   PROMOTE   -- a single bounded one-shot call at Engage/Release, no Tick,
    //                 no re-assert (a stance toggle, a draw/sheathe, an idle).
    // A facet can combine ARBITRATE with DENY (e.g. casting: APMF records the
    // claim AND enforces it). See the param-usage table after APMF_Param below
    // for which Intents actually read a field today vs. accept-and-ignore it.
    enum Intent : std::uint32_t {
        kIntent_None          = 0,   // no facet; Request/RequestEx return kInvalidHandle

        kIntent_MovementBlock = 1,   // ch.1  Full stand-still. Mode: DENY (nulls the actor's
                                     //       own move goal at the source). Param: none.
        kIntent_Disposition   = 2,   // ch.11 Aggression/confidence/assistance/morality bias.
                                     //       Mode: DENY (sets the actor values the AI's own
                                     //       decisions read). Param: fval (reserved).
        kIntent_Headtrack     = 3,   // ch.5  Look-at target. Mode: DENY, known-incomplete
                                     //       (owns one headtrack slot of several the AI
                                     //       writes). Param: form (reserved).
        kIntent_SelectSpell   = 4,   // ch.8  CLAIM the casting facet. Mode: ARBITRATE + DENY
                                     //       (the claim's spell is enforced as the actor's
                                     //       only castable choice; denying a COMPETING
                                     //       framework's own selection is a future gap).
                                     //       Param: form (the spell FormID), ival (+ACT hand
                                     //       mode: 0 auto/1 right/2 left/3 dual -- see
                                     //       core/CastExecutor.h), target (the cast target
                                     //       actor; 0 -> falls back to a winning
                                     //       kIntent_CombatTarget claim, then self), pos
                                     //       (reserved -- a world-location target for a
                                     //       location-delivery spell; NOT yet wired, see
                                     //       core/CastExecutor.cpp).
                                     //       +ACT MODE (feat/cast-act): when driven by
                                     //       core/CastExecutor.cpp, this claim doesn't just
                                     //       narrow the AI -- APMF equips + animates + fires
                                     //       the cast itself. See CastExecutor.h.
        kIntent_WeaponDrawn   = 5,   // ch.4  Draw/sheathe. Mode: PROMOTE (one-shot, sticky).
                                     //       Param: none.
        kIntent_Dialogue      = 6,   // ch.10 Pause the actor's own in-progress dialogue.
                                     //       Mode: DENY (fires once, no ongoing block).
                                     //       Param: none.
        kIntent_Gait          = 7,   // ch.1a Movement speed scale. Mode: DENY (sets the
                                     //       speed-mult actor value). Param: fval (reserved).
        kIntent_Detection     = 8,   // ch.16 Silent movement + reduced detect range. Mode:
                                     //       DENY (sets the detection actor values). Param:
                                     //       fval (reserved).
        kIntent_Stance        = 9,   // ch.3  Sneak/crouch. Mode: PROMOTE (one-shot toggle).
                                     //       Param: ival (reserved).
        kIntent_CombatTarget  = 10,  // ch.6  CLAIM the combat-target facet. Mode: ARBITRATE
                                     //       only (the client writes the target itself;
                                     //       denying a competing framework's own target
                                     //       write is a future gap). Param: form (the
                                     //       target actor).
        kIntent_Idle          = 11,  // ch.12 One-shot idle/animation. Mode: PROMOTE.
                                     //       Param: none.
        kIntent_ShoutPower    = 12,  // ch.14 Shout/power selection. Mode: ARBITRATE only
                                     //       (mirrors ch.6/ch.8, no deny gate yet). Param:
                                     //       form (the shout/power FormID).
        kIntent_Equipment     = 13,  // ch.15 Unequip/equip a worn item (the melee-vs-ranged
                                     //       lever). Mode: DENY (owns the equipped set).
                                     //       Param: form (optional). No param: deny the
                                     //       actor's right-hand item (the channel's
                                     //       default). form set: additionally GATE-ONLY --
                                     //       deny any spell/staff re-arm while the claim
                                     //       stands (no engine write for the form itself;
                                     //       the same underlying hook ch.8's exact-match
                                     //       deny rides). ival is reserved for a future
                                     //       hand-slot discriminator.
        kIntent_CombatAction  = 14,  // ch.7  DENY named combat behavior-tree leaf
                                     //       CATEGORIES. Mode: DENY (graduated from a
                                     //       field-proven probe, Docs/PROBE-ALLOWANCE.md).
                                     //       Param: ival (a CombatActionCategory bitmask,
                                     //       see below).
        kIntent_OfferPackage  = 15,  // ch.9  CLAIM the package-offer facet. Mode: DENY
                                     //       (redirects the package offer to the claimed
                                     //       package; graduated from a field-proven probe).
                                     //       Param: form (the TESPackage FormID).
        kIntent_Cast          = 16,  // ch.8b CLAIM the cast-EXECUTION facet for a bounded window.
                                     //       DENY (composite). Param: see APMF_CastRequest via
                                     //       RequestCast; RequestEx(form=spell) is the degenerate
                                     //       form (no target/proxy/TTL -> default TTL). The CLAIM
                                     //       fans into the SAME three ch.8/ch.7 gates that already
                                     //       exist (0x0A CheckCast exclusivity, 0x0F re-arm deny,
                                     //       the T1 cast leaves via kCombatActionCat_Cast) plus a
                                     //       bounded TTL auto-release. APMF fires NO cast: the
                                     //       CLIENT executes its own animated cast (design.md §1a;
                                     //       MFO SPEC-FORCED-CAST.md). A cast is NEVER a package
                                     //       (kIntent_Cast is invisible to ch.9's 0x49 offer).
    };

    // ── Combat-action CATEGORY bitmask (kIntent_CombatAction's param.ival) ──────
    // Which combat behavior-tree leaf CATEGORY a kIntent_CombatAction claim
    // denies for its actor (ch.7, Docs/CHANNEL-MAP.md). A claim's param.ival is
    // the OR of every category it wants denied; a leaf is denied only when its
    // OWN classified category bit is set in the winning claim's mask -- an
    // all-zero mask (or no claim at all) denies nothing, never a blanket lock.
    // APPEND-ONLY: never renumber an existing bit; OR in a new bit at the next
    // free position for a future category (defense/movement/utility are
    // deliberately NOT assigned a bit yet -- Docs/ALLOWANCE-TEMPLATE.md §3's T1
    // row lists them as never-denied by design until a real client need names
    // one).
    enum CombatActionCategory : std::uint32_t {
        kCombatActionCat_None    = 0,
        kCombatActionCat_Offense = 1u << 0,   // Attack/AttackLow/Bash/RangedAttack/SpecialAttack/
                                               // GroundAttack/FlyingAttack/CastImmediateSpell/
                                               // CastConcentrationSpell/CastShout/PrepareDualCast leaves
        kCombatActionCat_Cast    = 1u << 1,   // CastImmediateSpell, CastConcentrationSpell,
                                               // PrepareDualCast, CastShout leaves (a leaf may carry
                                               // several bits: the four cast leaves carry BOTH Offense
                                               // and Cast). A kIntent_Cast claim denies ONLY these,
                                               // leaving attack/ranged/movement leaves firing.
    };

    // ── Cast flags (kIntent_Cast / APMF_CastRequest::flags, ABI v5) ─────────────
    // A bitmask a client passes with a cast-execution claim. APPEND-ONLY: never
    // renumber an existing bit; OR in a new bit at the next free position.
    enum CastFlags : std::uint32_t {
        kCastFlag_None          = 0,
        kCastFlag_FromPackage   = 1u << 0,   // param.form / req.spell is a TESPackage; APMF extracts
                                             //   ONLY the cast portion (its SPELL-style input + target)
                                             //   and NEVER runs/offers/installs/evaluates the package
                                             //   (design.md §1a; the freeze half is simply never applied
                                             //   because no package is applied).
        kCastFlag_LeftHand      = 1u << 1,   // hand hint (default right). Informational for the gates today.
        kCastFlag_Concentration = 1u << 2,   // client says the executed cast is a held stream (TTL floor applies).
    };

    // ABI v5: cast-window TTL bounds (kIntent_Cast is ALWAYS bounded -- never a
    // standing hold, design.md §5a). ttlMs == 0 -> kCastDefaultTtlMs; any value is
    // clamped to kCastMaxTtlMs. A crashed/forgetful client can never leave a
    // standing cast hold: the claim auto-releases at expiry (ControlMap TTL pass).
    inline constexpr std::uint32_t kCastDefaultTtlMs = 4000;
    inline constexpr std::uint32_t kCastMaxTtlMs     = 15000;

    // ── APMF_CastRequest (ABI v5) ──────────────────────────────────────────────
    // The rich payload for RequestCast. Its own POD (NOT folded into APMF_Param, so
    // the frozen APMF_Param layout is untouched). APPEND-ONLY: never reorder/retype
    // an existing field; add new fields at the END. All fields optional except
    // actor/spell; zero = "none".
    struct APMF_CastRequest {
        RE::FormID    spell;    // the spell the client will fire (or the package if FromPackage)
        RE::FormID    proxy;    // runtime FF-form proxy the client fabricated for delivery (0 if none)
        RE::FormID    target;   // intended target actor (0 = self). RECORD ONLY -- APMF never aims.
        std::uint32_t flags;    // kCastFlag_*
        std::uint32_t ttlMs;    // bounded window; 0 -> kCastDefaultTtlMs. Clamped to kCastMaxTtlMs.
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
        RE::FormID   form;   // a form the channel acts on (a SpellItem, an Actor, a
                             //   TESPackage, a weapon/item...). 0 => channel default.
        float        fval;   // a scalar (a bias / scale / factor). 0 => channel default.
        std::int32_t ival;   // an integer/enum variant (e.g. a category bitmask).
                             //   0 => default.
        // Appended (feat/cast-act, marth 2026-09-05) -- END of the struct, so a
        // zero-init caller (v1..v-this-pass) reads target=0/pos=0/0/0, byte-identical
        // to before these existed. No new vtable slot, no APMF_API_vN, no
        // abiVersion bump -- APMF_Param itself is append-only by design (see the
        // struct's own comment above).
        RE::FormID   target;   // an actor/ref this request acts ON (e.g. kIntent_SelectSpell's
                               //   cast target). 0 => channel-specific fallback.
        float        posX;    // a WORLD-LOCATION target (all-zero => unset). Reserved for a
        float        posY;    //   location-delivery cast (Rune/AoE ground-target) -- the
        float        posZ;    //   CLIENT picks the point (e.g. by enemy-count-in-radius);
                               //   APMF never selects one. Also pre-provisions a future
                               //   move-to-point destination. NOT YET READ by any channel
                               //   (core/CastExecutor.cpp accepts kIntent_SelectSpell's
                               //   `target`; `pos` is documented-reserved until the rune/AoE
                               //   pass wires the location-aim plumbing).
    };

    // ── APMF_Param field usage, per Intent (at a glance) ────────────────────────
    // A client may always pass an APMF_Param (RequestEx/Repoint zero-init a v1-era
    // caller sees as "no param" too). This table says which Intents actually READ
    // a field TODAY versus accept-and-ignore it -- read the per-Intent comments
    // above for the full picture; this is the quick-scan version.
    //
    //   form   kIntent_SelectSpell     the spell FormID
    //   ival   kIntent_SelectSpell     +ACT hand mode: 0 auto/1 right/2 left/3 dual
    //   target kIntent_SelectSpell     the cast target actor (0 -> a winning
    //                                  kIntent_CombatTarget claim -> self)
    //   pos    kIntent_SelectSpell     RESERVED (a location-delivery target; not yet
    //                                  read -- core/CastExecutor.cpp)
    //   form   kIntent_CombatTarget    the target actor
    //   form   kIntent_ShoutPower      the shout/power FormID
    //   form   kIntent_OfferPackage    the TESPackage FormID
    //   form   kIntent_Equipment       optional, gates re-equip when set (see above)
    //   ival   kIntent_CombatAction    a CombatActionCategory bitmask (see below)
    //   form   kIntent_Cast            the spell (degenerate RequestEx form); RequestCast for
    //   ival   kIntent_Cast            the rich payload -- ival = CastFlags on the degenerate form
    //   none   every other Intent      accepted, not yet read by the channel
    //
    // fval is not read by any channel yet (reserved for a future per-request bias
    // on ch.11, scale on ch.1a, factor on ch.16). target/pos are not read by any
    // Intent OTHER than kIntent_SelectSpell yet.
    // ─────────────────────────────────────────────────────────────────────────────

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

    // The v3 interface: APMF_API_v2's members verbatim (identical initial sequence),
    // then the appended Repoint slot. A v1/v2 client reading this object through its
    // own struct pointer sees exactly its prefix; a v3 client reads Repoint too.
    struct APMF_API_v3 {
        std::uint32_t abiVersion;
        Handle (*Request)(RE::FormID actor, Intent intent, float basis);
        void   (*Release)(Handle handle);
        Handle (*RequestEx)(RE::FormID actor, Intent intent, float basis,
                            const APMF_Param* param);

        // RE-POINT an EXISTING claim in place: replace the claim's POD param without
        // releasing/re-requesting, KEEPING THE SAME handle. If the claim currently
        // OWNS its channel, the channel is re-pointed at the new param immediately
        // (e.g. combat-target switches the held foe; cast-select switches the selected
        // spell) with NO release/re-engage churn — so a held target/spell that merely
        // CHANGES stays a single continuous claim for its whole lifetime, and a client
        // reserves Release for when it is genuinely DONE (combat end / cast end), not
        // for a mere retarget. `param` is read+copied synchronously; nullptr is a
        // no-op. No-op on an unknown/stale handle. Thread-safe (enqueues; applied on
        // the game thread). Its NON-owning-claim behavior: the stored param is updated
        // so it takes effect if/when the claim later becomes the owner.
        void (*Repoint)(Handle handle, const APMF_Param* param);
    };

    // The v4 interface: APMF_API_v3's members verbatim (identical initial sequence),
    // then the appended SetSpellAllowList slot. A v1/v2/v3 client reading this
    // object through its own struct pointer sees exactly its prefix; a v4 client
    // reads SetSpellAllowList too.
    struct APMF_API_v4 {
        std::uint32_t abiVersion;
        Handle (*Request)(RE::FormID actor, Intent intent, float basis);
        void   (*Release)(Handle handle);
        Handle (*RequestEx)(RE::FormID actor, Intent intent, float basis,
                            const APMF_Param* param);
        void   (*Repoint)(Handle handle, const APMF_Param* param);

        // Attach a bounded ADDITIONAL allow-set of spell FormIDs to an EXISTING
        // kIntent_SelectSpell claim (`handle`, as returned by RequestEx/Request):
        // the actor's AI may cast the claim's primary `param.form` (the
        // selected spell, unchanged) OR any spell FormID in `forms` -- e.g. a
        // bounded exempt set of heal/buff spells a client wants to allow
        // alongside its chosen spell. `forms` is READ AND COPIED
        // synchronously inside the call -- APMF never retains the pointer, same
        // contract as RequestEx/Repoint's `param` (a stack-local array is fine).
        // `count` is silently clamped to kMaxSpellAllowList; a count over the
        // bound degrades to "the excess spells are treated as non-exempt" (never
        // a crash or an unbounded write). `count == 0` or `forms == nullptr`
        // clears the allow-set -- the claim falls back to today's exact
        // claim.form-only match, so a v1-v3 client, or a v4 client that never
        // calls this, is byte-for-byte unaffected. A stale/unknown `handle`, or a
        // handle whose claim is on a channel OTHER than kIntent_SelectSpell, is a
        // silent no-op (mirrors Repoint's own no-op-on-unknown-handle discipline).
        // Updates the STORED claim whether or not it currently OWNS the channel --
        // same non-owning semantics as Repoint, so a claim that later wins
        // arbitration already carries its allow-set. Thread-safe (enqueues;
        // applied on the game thread, never off it).
        void (*SetSpellAllowList)(Handle handle, const RE::FormID* forms, std::uint32_t count);
    };

    // The v5 interface: APMF_API_v4's members verbatim (via prefix EXTENSION -- the
    // base subobject lays out first, so its identical initial sequence is preserved
    // and a v1..v4 client reading this object through its own struct pointer sees
    // exactly its prefix), then the appended RequestCast slot. This header is
    // BYTE-SHARED with MFO: the declaration below is authoritative and must be
    // mirrored byte-identically on the client side.
    struct APMF_API_v5 : APMF_API_v4 {
        // Claim the cast-EXECUTION facet (ch.8b) for actor `actor` at arbitration
        // weight `basis`, carrying the rich APMF_CastRequest. The claim fans into
        // the SAME three gates cast-select already rides (0x0A CheckCast, 0x0F
        // CheckShouldEquip, the T1 cast leaves) plus a bounded TTL auto-release --
        // NO engine cast call is ever made (design.md §1a; the CLIENT fires its own
        // animated cast). `req` is READ AND COPIED synchronously inside the call;
        // APMF never retains the pointer, so a stack temporary is fine. Returns a
        // handle to release later, or kInvalidHandle if the cast channel is not
        // registered. A kCastFlag_FromPackage request whose package carries no
        // readable spell input is REFUSED at drain (the handle becomes inert; APMF
        // never runs, offers, or evaluates the package). Safe from any thread (POD
        // captured; applied on the game thread).
        Handle (*RequestCast)(RE::FormID actor, float basis, const APMF_CastRequest* req);
    };

    // Function-pointer type for GetProcAddress(kGetInterfaceExport). Returns the
    // base type; a client that asked for ABI >= N checks p->abiVersion and casts up
    // to APMF_API_vN* (all revisions share v1's identical initial sequence).
    using GetInterface_t = const APMF_API_v1* (*)(std::uint32_t abiVersion);

}
