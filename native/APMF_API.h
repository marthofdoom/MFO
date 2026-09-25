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
// Request/RequestEx/Repoint/Release/SetSpellAllowList/SetEquipSet/SetEquipSetEx/SetEquipScope
// are SAFE FROM ANY THREAD. They capture POD (a FormID, a copy of the APMF_Param, or — for
// SetSpellAllowList/SetEquipSet/SetEquipSetEx — a copy of the forms/entries array; for
// SetEquipScope — a copy of the APMF_EquipScope) and enqueue the
// work; APMF applies it on the game thread. A client's BSJobs worker may call
// them directly. The APMF_Param pointer passed to RequestEx/Repoint, and the
// RE::FormID* passed to SetSpellAllowList/SetEquipSet, and the APMF_EquipEntry*
// passed to SetEquipSetEx, and the APMF_EquipScope* passed to SetEquipScope, are READ AND COPIED
// synchronously inside the call — APMF never retains the client's pointer, so a
// stack temporary/local array is fine.
//
// THE ONE EXCEPTION (ABI v11): the two SPACE QUERIES, FindEmptySpace and
// FindHostilesInSpace, are synchronous and run ONLY on the game's main thread (the
// player-Update thread). Called from anywhere else they do nothing and return
// kQuery_NotMainThread. See the "ABI v11: SPACE QUERIES" section below. A
// kCastFlag_AtPosition RequestEx is still safe from any thread: it is captured and
// delivered on the game thread like every other request.
//
// ── Exceptions ──
// NO exception ever crosses this boundary. Every APMF-side body (Request,
// RequestEx, Repoint, Release, APMF_GetInterface) is wrapped in a catch-all; a throw
// degrades to kInvalidHandle / no-op / nullptr, never an unwind into the client's
// separately compiled DLL (UB).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstddef>
#include <cstdint>

namespace RE {
    // Identical alias to CommonLib's RE::FormID; a duplicate identical using-alias
    // is legal, so this header composes with a full CommonLib include.
    using FormID = std::uint32_t;
}

namespace APMF_API {

    // ABI v10 (2026-09-22) adds kIntent_Travel (ch.19) + TravelFlags and NOTHING
    // else: ch.19 rides the EXISTING Request/RequestEx/Repoint/Release slots, so
    // there is no APMF_API_v10 struct and no new function pointer. A v9 client is
    // byte-unaffected and keeps working against this build unchanged.
    //
    // ABI v11 (2026-09-23) adds APMF_API_v11: two read-only SPACE QUERIES
    // (FindEmptySpace, FindHostilesInSpace) and their POD structs, plus the
    // kCastFlag_AtPosition bit that lets a kIntent_Cast RequestEx carry a world
    // POINT (APMF_Param.pos), and the kTravel_ToPosition bit that lets kIntent_Travel
    // walk to one. A client must see abiVersion >= 11 before it calls a v11 slot or
    // sets either bit: an older APMF ignores kCastFlag_AtPosition and would treat the
    // request as an ordinary actor-target cast claim (it refuses a zero-form travel).
    //
    // ABI v12 (2026-09-24) adds APMF_API_v12: ONE read-only slot, GetTravelLegState,
    // and its POD struct APMF_TravelLegInfo, so a client can read WHY its travel leg
    // ended (APMF ends a leg but never releases the client's claim, so without it an
    // ended leg is indistinguishable from a package theft). It also adds the travel
    // GAIT bits (kTravel_SpeedSet + a 2-bit speed) and the BLOCKED leg end. A client
    // must see abiVersion >= 12 before it calls the slot or sets a gait bit: an older
    // APMF stores the unknown bits and walks at its authored speed without a word.
    //
    // ABI v13 (2026-09-25) adds kIntent_TargetPin (ch.20) and NOTHING else: no struct,
    // no function-pointer slot, no APMF_Param field. ch.20 rides the EXISTING
    // RequestEx/Repoint/Release slots, exactly as ch.19 did at v10, so there is no
    // APMF_API_v13 struct and a v12 client is byte-unaffected. A client must see
    // abiVersion >= 13 before it asks for the intent: an OLDER APMF has no channel for
    // intent 20 and REFUSES the request (kInvalidHandle, "no channel serves intent 20"
    // in its log), which is the documented degrade -- keep your own targeting.
    inline constexpr std::uint32_t kABIVersion = 13;

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
                                     //       Param: form (the spell FormID). `ival`, `target`
                                     //       and `pos` are ACCEPTED-AND-IGNORED on this intent.
                                     //       RETIRED (feat/ai-cast-seats-impl): `ival` bits 0-1
                                     //       (the +ACT hand mode) and bit 2 (kActFlag_Drive, the
                                     //       +ACT drive opt-in) NO LONGER MEAN ANYTHING. The
                                     //       forced cast drive they selected is gone; a cast that
                                     //       must actually HAPPEN is now a kIntent_Cast (ch.8b)
                                     //       claim, which makes the NPC's own AI cast natively
                                     //       through the engine seats (see kIntent_Cast below).
                                     //       The bits stay RESERVED and are never reused, so a
                                     //       client that still sets them is byte-compatible and
                                     //       simply gets this channel's original gate-only
                                     //       behaviour: the client's OWN AI casts the selected
                                     //       spell, APMF only narrows/denies competitors.
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
                                     //       ARBITRATE + DENY + COMPOSE (Docs/INVARIANTS.md #20).
                                     //       Param: see APMF_CastRequest via RequestCast;
                                     //       RequestEx(form=spell) is the degenerate form (no
                                     //       target/proxy/TTL -> default TTL).
                                     //       WHAT IT DOES NOW (feat/ai-cast-seats-impl): while the
                                     //       claim stands, APMF answers the FIVE engine vfunc seats
                                     //       that drive the combat AI's own cast decision so the
                                     //       NPC's OWN AI selects, equips, charges, aims, fires and
                                     //       channels `spell` at `target` -- a real native animated
                                     //       cast (0x0F CheckShouldEquip, 0x06 CheckStartCast, 0x0A
                                     //       GetMagicTarget, 0x07 CheckStopCast, 0x0D
                                     //       SetupAimController; core/CastSeats.cpp +
                                     //       core/EquipGate.cpp). APMF STILL FIRES NOTHING: it makes
                                     //       no EquipSpell/CastSpell/CastSpellImmediate/anim-graph
                                     //       write of any kind -- every one of those is the engine's
                                     //       own, from the engine's own behavior tree.
                                     //       `target` is now LOAD-BEARING (it was RECORD ONLY): seat
                                     //       0x0A hands it to the engine as the magic target, and
                                     //       0x0D as the aim override. A kSelf-delivery spell needs
                                     //       `proxy` (or APMF mints one -- core/CastProxy.h),
                                     //       because the engine's Self branch always lands on the
                                     //       caster. Still bounded by TTL auto-release; a cast is
                                     //       NEVER a package (invisible to ch.9's 0x49 offer).
        kIntent_EquipAuthority = 17, // ch.17 CLAIM the ENGINE-EQUIP facet, WHOLE (ABI v7, marth
                                     //       2026-09-15: "a command is sent to APMF with what to
                                     //       equip and that is enforced until overridden").
                                     //       Mode: ARBITRATE + DENY + the one #17a-licensed equip.
                                     //       A STANDING authority: no TTL, ended only by Release.
                                     //       Param: ival (an EquipAuthFlags bitmask, see below).
                                     //       The claim alone changes NOTHING -- the client then
                                     //       DECLARES the worn set with SetEquipSet(handle, ...)
                                     //       (APMF_API_v7). From that declaration on: APMF equips
                                     //       every declared item the actor is not already wearing
                                     //       (once, on the main thread, on each declaration --
                                     //       never a re-assert loop), and APMF REFUSES every engine
                                     //       equip of an item that is NOT in the declared set --
                                     //       outfit re-apply, AI weapon/armor choice, combat
                                     //       re-arm, RemoveItem re-equip, another plugin's equip
                                     //       call -- at the ONE non-virtual choke point every
                                     //       engine equip funnels through (core/EquipSink.cpp;
                                     //       Docs/INVARIANTS.md #17a). Papyrus/console equips pass
                                     //       unless kEquipAuth_DenyScript; the PLAYER's own
                                     //       equips on the actor (the trade/gift menu) pass
                                     //       unless kEquipAuth_DenyPlayerMenu (ABI v8). Only
                                     //       ARMO/WEAP/AMMO/LIGH equips are governed at all --
                                     //       potions, food, scrolls, ingredients and books pass
                                     //       (ABI v8; see APMF_API_v8). UNEQUIPS are never
                                     //       denied by this ABI (kEquipAuth_DenyUnequip is
                                     //       RESERVED and refused). The engine's own in-worker
                                     //       displacement still handles slot conflicts, so a
                                     //       declared two-hander displaces a declared shield the
                                     //       ordinary way. The player is NEVER subject to it.
                                     //       Declare->enforce (CLAUDE.md principle 4): an empty
                                     //       declaration (count 0) CLEARS it and the seat passes
                                     //       everything through again; APMF never invents a set.
                                     //       ABI v8: the claim is REFUSED (kInvalidHandle) while
                                     //       the equip seat is not installed (bEquipAuthority=0,
                                     //       VR, a gated runtime, a site-verify refusal, or before
                                     //       kDataLoaded) -- a refused claim means KEEP YOUR OWN
                                     //       EQUIPS; APMF_API_v8::IsEquipAuthorityEnforced tells
                                     //       observe from enforce for an accepted claim.

        // 18 is DELIBERATELY SKIPPED, not free. A design that is NOT YET ON MAIN
        // reserves ch.18 for the NPC attack-selection facet; nothing has been
        // authored for it. Leaving the number unused costs nothing and keeps intent
        // numbers and channel numbers aligned for every channel that exists.
        // (That design's own draft calls itself "ABI v10" -- it will have to become
        // v11, since ch.19 shipped v10 first.)

        kIntent_Travel        = 19,  // ch.19 WALK this actor to a destination (ABI v10, marth
                                     //       2026-09-22). Mode: the framework MOVES the actor,
                                     //       natively, with a package APMF itself ships
                                     //       (Data/APMF.esl). A STANDING claim: no TTL, ended
                                     //       only by Release.
                                     //       Param: form = the DESTINATION's FormID (REQUIRED --
                                     //       a zero form is refused -- unless ival carries
                                     //       kTravel_ToPosition, ABI v11); fval = the arrival radius in
                                     //       game units (0 => the 75u default, clamped to
                                     //       [50, 512] and the clamp logged); ival = a TravelFlags
                                     //       bitmask.
                                     //
                                     //       A DESTINATION IS A REFERENCE OR A CELL, and the
                                     //       FormID's own record type decides which -- no flag,
                                     //       no second field, no ambiguity:
                                     //         * an object REFERENCE (REFR/ACHR: an actor, an
                                     //           XMarker, a container, anything loaded). Arrival
                                     //           = distance to it <= the radius.
                                     //         * a CELL. Arrival = the actor's PARENT CELL is that
                                     //           cell; distance has no meaning for a cell, so the
                                     //           radius is not consulted for this kind.
                                     //       Anything else is REFUSED and logged, never
                                     //       reinterpreted -- but read the two refusal TIMINGS
                                     //       below, because they are not the same and a client
                                     //       that assumes they are will leak a claim.
                                     //
                                     //       A WORLD POSITION (ABI v11): set kTravel_ToPosition in
                                     //       ival, leave form = 0, put the point in param.pos. An
                                     //       AI package's location still carries a form or a handle
                                     //       and NO coordinates, so APMF does what vanilla does: it
                                     //       places its OWN XMarker at the point and the leg's
                                     //       destination is that marker. Everything else is the
                                     //       reference path above, unchanged (radius, combat,
                                     //       the 2-minute stuck end), except that a marker never
                                     //       "dies". APMF deletes the marker when the leg ends for
                                     //       ANY reason and replaces it on a Repoint to a new
                                     //       point (see kTravel_ToPosition). WITHOUT the flag a
                                     //       non-zero param.pos is still REFUSED by name, never
                                     //       silently ignored.
                                     //
                                     //       THE WHOLE CONTRACT, in marth's words: "it goes to
                                     //       the target 50-100u from it. And combat interrupts
                                     //       and cancels the movement. That's all." The leg ENDS
                                     //       on ARRIVAL (inside the radius), on the ACTOR ENTERING
                                     //       COMBAT (`Actor::IsInCombat`), or on the destination
                                     //       dying / being disabled / unloading. Nothing else.
                                     //       (ABI v12 adds one engine-reported end: BLOCKED, the
                                     //       engine holding the actor in its Movement Blocked
                                     //       package for 3 s. The 2-minute stuck end is the
                                     //       safety net. APMF_API_v12::GetTravelLegState says
                                     //       which end it was.)
                                     //
                                     //       ONE INTENT, ONE FACET. ch.19 claims NOTHING on your
                                     //       behalf -- no combat target (ch.6), no attack
                                     //       selection, no casting, no equip (ch.17), no hands, no
                                     //       aggression (ch.11), no movement block (ch.1). It
                                     //       enters no combat, pins no target, and fakes no
                                     //       perception (there is no line-of-sight or detection
                                     //       test anywhere in it). A client that wants any of
                                     //       those claims that intent ITSELF, separately, and the
                                     //       two arbitrate independently. That orthogonality is
                                     //       deliberate: marth, 2026-09-22, "We should avoid
                                     //       combining intents anyway."
                                     //       Convenience COMBO intents (one call that bundles,
                                     //       say, travel + combat target + combat entry) are a
                                     //       recognised and reasonable idea, DEFERRED until after
                                     //       the full MFO port to APMF is complete -- not refused
                                     //       on principle. Until then: one facet, one intent.
                                     //
                                     //       Repoint(handle, &param) moves the destination in
                                     //       place -- no release/re-claim churn. Release ends the
                                     //       claim and the leg.
                                     //       TWO REFUSAL TIMINGS, and the difference matters:
                                     //       * SYNCHRONOUS (RequestEx returns kInvalidHandle
                                     //         before anything is queued): VR, [Travel]
                                     //         bTravel=0, APMF.esl missing, param.form == 0,
                                     //         param.pos non-zero -- and, with kTravel_ToPosition
                                     //         (ABI v11): a non-zero param.form, a non-finite
                                     //         point, or a runtime without XMarker support.
                                     //         Nothing to clean up.
                                     //       * AT ENGAGE (one Drain later, on the game thread):
                                     //         param.form naming a record that is neither an
                                     //         object reference nor a cell. RequestEx already
                                     //         returned a LIVE HANDLE, the log says why the
                                     //         claim does nothing, and the handle stays live
                                     //         until you Release it. RELEASE IT.
                                     //       Why not synchronous: RequestEx is callable FROM ANY
                                     //         THREAD and its whole contract is that it copies
                                     //         POD and enqueues (see the Threading note at the
                                     //         top). Deciding a form's record type needs a form
                                     //         lookup, and APMF does not take the engine's form
                                     //         table off the game thread for an API call --
                                     //         kIntent_Cast's own FromPackage extraction defers
                                     //         for exactly the same reason. Every refusal is
                                     //         logged with its reason either way.

        kIntent_TargetPin     = 20,  // ch.20 PIN this actor's combat target (ABI v13, marth
                                     //       2026-09-25). Mode: DENY the ENGINE'S OWN target
                                     //       selection AT ITS SOURCE (APMF's seat on the combat
                                     //       target selectors, vtable slot 6). A STANDING claim:
                                     //       no TTL, ended only by Release.
                                     //       Param: form = the TARGET actor's FormID (REQUIRED).
                                     //       fval / ival / target / pos are not read.
                                     //
                                     //       WHAT IT DOES. Each combat update the engine asks its
                                     //       target selectors which foe this actor should fight.
                                     //       While the claim stands, APMF answers that question
                                     //       with your target, so the engine's own pick never
                                     //       reaches the actor. The engine then fights your
                                     //       target with its own AI: its own attacks, movement,
                                     //       spells and equips.
                                     //
                                     //       ONLY AMONG THE ENGINE'S OWN COMBAT TARGETS. The
                                     //       answer is replaced only when the engine's selector
                                     //       picked a target (the actor is fighting) AND your
                                     //       target is one of the actor's combat group's targets.
                                     //       Otherwise nothing is written and the log says so.
                                     //       It does NOT start combat and does not make anyone a
                                     //       target: an actor becomes pinnable once something (the
                                     //       engine, or your own combat entry, a separate concern)
                                     //       makes it a combat target of the actor's group. Combat
                                     //       entry is never this intent (INVARIANTS #0).
                                     //       It claims nothing else: no attack selection, no
                                     //       casting, no equip, no movement, no aggression. One
                                     //       intent, one facet (the kIntent_Travel rule).
                                     //       It is a different intent from kIntent_CombatTarget
                                     //       (ch.6), which stays ARBITRATION-ONLY and writes
                                     //       nothing: claim this one to have APMF hold the target.
                                     //
                                     //       THE PIN PAUSES -- the engine's own pick stands for
                                     //       that update -- while the engine has no target or your
                                     //       target is not (yet) one of the group's combat targets.
                                     //       The claim stays; it resumes when both hold again.
                                     //
                                     //       THE PIN ENDS, AND APMF RELEASES THE CLAIM ITSELF
                                     //       (marth: "If APMF can no longer track the target, it's
                                     //       lost, and dropped"), when the target is LOST (the
                                     //       engine can no longer locate it), dead, disabled, not
                                     //       loaded or no longer resolves, or the actor itself
                                     //       dies. The log names the reason ("pin ended: target
                                     //       lost"), IsClaimLive(handle) turns false (ABI v6), and
                                     //       the handle is dead: a mod that wants to keep chasing
                                     //       must pin again. This is the ONLY case in which APMF
                                     //       releases a client's kIntent_TargetPin claim. Your own
                                     //       Release, an outranking claim, a save load, a new game
                                     //       or the actor unloading also end it (never saved).
                                     //
                                     //       The world's reaction to the fight is YOURS (CLAUDE.md
                                     //       principle 2): crime, bounty, faction and aggression
                                     //       consequences happen as the engine does them, and
                                     //       Release does not undo them.
                                     //
                                     //       REFUSED SYNCHRONOUSLY (kInvalidHandle, logged): VR, a
                                     //       runtime other than 1.6.1170 / 1.5.97, [TargetPin]
                                     //       bTargetPin=0, a seat's address self-check failed,
                                     //       before kDataLoaded, param.form == 0, param.form == the
                                     //       actor itself, or the actor is the player (ch.20 pins
                                     //       NPC combat targets only). REFUSED AT ENGAGE
                                     //       (one Drain later; the handle is LIVE and inert, the log
                                     //       says why -- RELEASE IT): param.form is not an Actor.
                                     //       Repoint(handle, &param) moves the pin to a new target
                                     //       in place; a Repoint naming a non-actor, 0 or the actor
                                     //       itself leaves the claim live and inert, logged.
    };

    // ── Travel flags (kIntent_Travel's param.ival, ABI v10) ─────────────────────
    // Read at RequestEx/Repoint time from param.ival. APPEND-ONLY once shipped:
    // never renumber an existing bit; OR in a new bit at the next free position.
    // (These bit values are still fresh: ABI v10 has never been released and has
    // never been mirrored into a client -- MFO's byte-shared copy is at v9 and does
    // not contain this intent at all -- so the numbering below is the first and only
    // one there has ever been.)
    enum TravelFlags : std::uint32_t {
        kTravel_None = 0,

        kTravel_ReleaseOnTargetDead = 1u << 0,
                                        // NAMES THE DEFAULT; it does not switch it on. APMF
                                        //   ends the leg when a destination that was ALIVE when
                                        //   it was targeted DIES during travel, whether or not
                                        //   this bit is set. A destination that was ALREADY DEAD
                                        //   when targeted (a corpse) is a valid place to walk to
                                        //   and never ends the leg by death (marth 2026-09-23:
                                        //   "If the target is dead when targeted, it's fine. But
                                        //   if it dies during travel, drop."). Re-pointing
                                        //   re-samples. A disabled, deleted or unresolvable
                                        //   destination ends the leg regardless. Setting the bit
                                        //   is free and documents intent.

        kTravel_ToPosition = 1u << 1,   // ABI v11. The destination is the WORLD POINT param.pos
                                        //   (in the actor's worldspace; indoors, its cell), not a form:
                                        //   param.form MUST be 0 (a non-zero form with this bit is
                                        //   REFUSED as ambiguous). Gate on abiVersion >= 11: an
                                        //   older APMF refuses a zero form, so the request fails
                                        //   loudly rather than walking somewhere else.
                                        //   APMF places a non-persistent XMarker (Skyrim.esm 0x3B)
                                        //   at the point one hop after the claim publishes, and the
                                        //   leg is an ordinary reference leg to it: same arrival
                                        //   radius, same combat cancel, same 2-minute stuck end. The
                                        //   death rule never applies (a marker cannot die).
                                        //   MARKER LIFETIME = THE LEG. It is deleted (by tracked
                                        //   handle, re-checked FormID and base) when the leg ends
                                        //   for ANY reason: arrival, combat, Release, a Repoint to a
                                        //   different point or to a form, the stuck end, the actor
                                        //   unloading, dying or losing its 3D. A Repoint to a new
                                        //   point places the new marker, re-points the package,
                                        //   then deletes the old one. At most one marker per travel
                                        //   package slot (8). APMF does NOT pick the point (use
                                        //   FindEmptySpace if you want help) and never adjusts it.

        // ── GAIT (ABI v12) ── bits 2-4. Gate on abiVersion >= 12: an older APMF stores the
        //   bits and walks at its authored speed (Run), with no refusal and no log line.
        kTravel_SpeedSet = 1u << 2,     // ABI v12. Use the speed in bits 3-4 for this leg. Without
                                        //   this bit the leg runs at the package record's AUTHORED
                                        //   speed (APMF.esl ships Run) and bits 3-4 are ignored.
        kTravel_SpeedMask = 3u << 3,    // ABI v12. The speed field. The four values are the engine's
                                        //   own PACKAGE_DATA::PreferredSpeed enum, written into the
                                        //   leg's package record (PKDT byte 6) together with the
                                        //   record's "Preferred Speed" flag (0x2000), which the engine
                                        //   requires before it honours the byte at all.
        kTravel_SpeedWalk     = 0u << 3,   // PreferredSpeed 0 = Walk
        kTravel_SpeedJog      = 1u << 3,   // PreferredSpeed 1 = Jog
        kTravel_SpeedRun      = 2u << 3,   // PreferredSpeed 2 = Run
        kTravel_SpeedFastWalk = 3u << 3,   // PreferredSpeed 3 = FastWalk (the engine's fourth value)
                                        //   WHEN IT TAKES EFFECT: the engine copies the record's speed
                                        //   into the actor's running-package state when the package
                                        //   STARTS on the actor (1.6.1170 0x6CE2C0 / 1.5.97 0x63BD40)
                                        //   and the movement code reads that copy. APMF writes the
                                        //   record before it offers the package, so a fresh leg walks
                                        //   at the declared gait. A Repoint that CHANGES the gait of a
                                        //   leg that is already walking rewrites the record, but the
                                        //   running package keeps the speed it started with until the
                                        //   package next starts. APMF logs that case. Release and
                                        //   re-request to change gait mid-walk.
    };

    // ── Travel LEG STATE (ABI v12, APMF_API_v12::GetTravelLegState) ──────────────
    // What ch.19 is doing with an actor's travel leg right now, or why it stopped.
    // APPEND-ONLY: never renumber a value.
    //
    // A LEG ENDS BUT THE CLAIM STANDS. APMF never releases a client's kIntent_Travel
    // claim. When a leg ends (arrival, combat, blocked, ...) APMF drops only its own
    // internal package offer and the claim stays live (IsClaimLive stays true) and does
    // nothing until the client Repoints or Releases it. Read this state to learn WHY
    // the actor stopped walking, instead of mistaking an ended leg for a package theft.
    enum TravelLegState : std::uint32_t {
        kLeg_None            = 0,   // APMF holds no ch.19 state for this actor (never claimed since the
                                    //   last load, or ch.19 is not installed)
        kLeg_Pending         = 1,   // the claim was accepted or re-pointed; the leg starts on the next frame
        kLeg_Walking         = 2,   // the package is offered and the actor is travelling
        kLeg_Arrived         = 3,   // inside the arrival radius (or, for a cell, inside the cell)
        kLeg_Blocked         = 4,   // the engine held the actor in its MOVEMENT BLOCKED package (package
                                    //   type 36) for 3 s of running game time (one missed poll tolerated).
                                    //   stallX/Y/Z says where; blocker/blockerKind say whether an ACTOR
                                    //   stood in front of it (it will likely move: take another item and
                                    //   come back later) or nothing did (a STATIC block such as a closed
                                    //   gate: the route stays closed until the world changes).
        kLeg_CombatCancelled = 5,   // the actor entered combat
        kLeg_DestGone        = 6,   // the destination was deleted, disabled, died during travel, or (a
                                    //   cell) no longer resolves
        kLeg_StuckTimeout    = 7,   // the 2-minute safety net elapsed with none of the above
        kLeg_ActorGone       = 8,   // the actor unloaded, died, or lost its 3D
        kLeg_Failed          = 9,   // the leg could not start or be re-pointed (a destination that is not
                                    //   a reference or a cell, a zero form or a bad point on a Repoint, all
                                    //   8 package slots in use, a declined Location write, a marker that
                                    //   could not be placed). The log says which. destForm / destX..Z name
                                    //   the REFUSED destination. A refused Repoint ENDS the previous leg:
                                    //   the actor does not keep walking to a destination the claim no
                                    //   longer declares.
        kLeg_Released        = 10,  // the ch.19 claim was released
    };

    // What stood in front of the actor when a leg ended kLeg_Blocked (APMF_TravelLegInfo::blockerKind).
    enum TravelBlocker : std::uint32_t {
        kBlocker_None     = 0,   // not a BLOCKED end, or no actor in front: a STATIC block (a closed gate,
                                 //   a wall, clutter). Treat the route as closed until the world changes.
        kBlocker_Player   = 1,   // the player stood in front of the actor
        kBlocker_Teammate = 2,   // a player teammate (a follower) stood in front of the actor
        kBlocker_Actor    = 3,   // another live actor (any NPC or creature) stood in front of the actor
    };

    // GetTravelLegState output. EXACT LAYOUT (byte-shared): 72 bytes, every field 4 bytes.
    // The CALLER sets `size` = sizeof(APMF_TravelLegInfo) as compiled against its header.
    // THE SIZE RULE (frozen): APMF fills the v12 fields when `size` >= kTravelLegInfoV12Size
    // (72, a constant that NEVER changes, unlike sizeof as fields are appended) and writes
    // nothing into a smaller struct. A field a later ABI appends is written ONLY when it lies
    // entirely inside the caller's `size`, so a client built against v12 keeps getting its
    // 72 bytes from every later APMF, and APMF never writes past `size`.
    struct APMF_TravelLegInfo {
        std::uint32_t size;        // +0   the CALLER sets = sizeof(APMF_TravelLegInfo)
        std::uint32_t state;       // +4   a TravelLegState (the same value the call returns)
        RE::FormID    actor;       // +8   the actor asked about
        RE::FormID    destForm;    // +12  the destination FormID this state is about; 0 for a point leg
                                   //        (kTravel_ToPosition) or when state is kLeg_None. Compare it
                                   //        with what you declared: after a Repoint the state refers to
                                   //        the new destination only once it reads Pending or later for it.
        float         destX;       // +16  a point leg's declared point; 0 otherwise
        float         destY;       // +20
        float         destZ;       // +24
        std::uint32_t msInState;   // +28  milliseconds since this state began (saturates at 0xFFFFFFFF)
        std::uint32_t seq;         // +32  a stamp from ONE counter APMF never resets while the game runs
                                   //        (not per actor, not per load): it changes on EVERY state change of
                                   //        this actor's leg and never repeats, so a client can tell a new end
                                   //        from one it already handled, across save loads too. Compare for
                                   //        inequality, not for +1.
        float         stallX;      // +36  kLeg_Blocked: the actor's position when the leg ended; 0 otherwise
        float         stallY;      // +40
        float         stallZ;      // +44
        std::uint32_t blockedMs;   // +48  kLeg_Blocked: how long Movement Blocked held before the end
        std::uint32_t speed;       // +52  the gait written into the leg's package record: 0..3 =
                                   //        PreferredSpeed (Walk, Jog, Run, FastWalk); 0xFFFFFFFF = none
                                   //        written (no kTravel_SpeedSet on the claim, or a build the gait
                                   //        path is not verified on, which the log names), so the record's
                                   //        authored speed applies
        std::uint32_t reserved;    // +56  0
        std::uint32_t ownerHandle; // +60  the kIntent_Travel claim handle whose leg this is (the WINNING claim
                                   //        when the leg was composed). Compare it with YOUR handle: a
                                   //        different value is another client's leg, or an older claim of
                                   //        yours. 0 while a fresh claim is still Pending, and on kLeg_None.
                                   //        During a Pending re-point, and on a kLeg_Failed recorded for a
                                   //        refused re-point that arrived with an OWNER CHANGE, it still
                                   //        names the previous owner (the new claim is not yet published).
        RE::FormID    blocker;     // +64  kLeg_Blocked: the actor found in front of the stalled actor (see
                                   //        blockerKind); 0 for a static block or any other state
        std::uint32_t blockerKind; // +68  a TravelBlocker; kBlocker_None unless state is kLeg_Blocked
    };
    // The v12 prefix size. FROZEN: GetTravelLegState tests the caller's `size` against THIS,
    // never against sizeof(APMF_TravelLegInfo), which grows when a later ABI appends a field.
    inline constexpr std::uint32_t kTravelLegInfoV12Size = 72;
    static_assert(sizeof(APMF_TravelLegInfo) == 72,  "APMF_TravelLegInfo is 72 bytes, byte-shared with clients");
    static_assert(sizeof(APMF_TravelLegInfo) >= kTravelLegInfoV12Size, "the v12 prefix is never shrunk");
    static_assert(alignof(APMF_TravelLegInfo) == 4,  "APMF_TravelLegInfo aligns to 4");
    static_assert(offsetof(APMF_TravelLegInfo, state)     == 4,  "state at +4");
    static_assert(offsetof(APMF_TravelLegInfo, destForm)  == 12, "destForm at +12");
    static_assert(offsetof(APMF_TravelLegInfo, msInState) == 28, "msInState at +28");
    static_assert(offsetof(APMF_TravelLegInfo, seq)       == 32, "seq at +32");
    static_assert(offsetof(APMF_TravelLegInfo, stallX)    == 36, "stallX at +36");
    static_assert(offsetof(APMF_TravelLegInfo, blockedMs) == 48, "blockedMs at +48");
    static_assert(offsetof(APMF_TravelLegInfo, speed)     == 52, "speed at +52");
    static_assert(offsetof(APMF_TravelLegInfo, reserved)  == 56, "reserved at +56");
    static_assert(offsetof(APMF_TravelLegInfo, ownerHandle) == 60, "ownerHandle at +60");
    static_assert(offsetof(APMF_TravelLegInfo, blocker)     == 64, "blocker at +64");
    static_assert(offsetof(APMF_TravelLegInfo, blockerKind) == 68, "blockerKind at +68");

    // ── Equip-authority flags (kIntent_EquipAuthority's param.ival, ABI v7) ──
    // Read at RequestEx/Repoint time from param.ival. APPEND-ONLY: never renumber
    // an existing bit; OR in a new bit at the next free position.
    enum EquipAuthFlags : std::uint32_t {
        kEquipAuth_None        = 0,
        kEquipAuth_DenyUnequip = 1u << 0,   // RESERVED. Not implemented: the unequip twin
                                            //   (UnequipObject -> its own worker) is NOT seated
                                            //   in this ABI. A claim carrying this bit is
                                            //   ACCEPTED with the bit REFUSED (logged, ignored),
                                            //   so a client built against a later APMF that does
                                            //   honour it degrades to "unequips pass", never to
                                            //   a refused claim.
        kEquipAuth_DenyScript  = 1u << 1,   // Also refuse Papyrus (EquipItem/EquipItemEx) and
                                            //   console equips of off-set items. Default OFF:
                                            //   scripts are a quest/mod author's deliberate act
                                            //   and pass through unless the client says otherwise.
        kEquipAuth_ObserveOnly = 1u << 2,   // Log every verdict as `would-deny`, deny nothing.
                                            //   The client-side twin of APMF.ini's
                                            //   [EquipAuthority] bEquipObserveOnly (either one
                                            //   set = observe). The first field build runs in
                                            //   observe mode until the probe criteria in
                                            //   Docs/INTEGRATION.md pass.
        kEquipAuth_DenyPlayerMenu = 1u << 3, // ABI v8. Also refuse the PLAYER's own equips on
                                            //   the actor -- the trade/gift/follower inventory
                                            //   menu (`path=PlayerMenu`). Default OFF (player
                                            //   agency, marth 2026-09-15): the player dressing
                                            //   a follower by hand is a deliberate act and
                                            //   PASSES unless the client sets this bit. The
                                            //   log line reads `verdict=allow (player agency)`.
                                            //   The allow is at the seat only: the next enforce
                                            //   pass re-equips any declared item the player's
                                            //   equip displaced. A client honouring player agency
                                            //   must observe the change (that log line, or its own
                                            //   inventory read) and fold the player's choice into
                                            //   its next declaration.
                                            //   A v7 APMF ignores the bit (unknown bits are
                                            //   not refused), so a v8 client degrades to the
                                            //   v7 behaviour (PlayerMenu denied) there.
    };

    // ABI v7: the bound on SetEquipSet's declared worn set. A full worn set is
    // two hands (or a two-hander), body/head/hands/feet, shield, amulet, ring,
    // circlet, plus a handful of modded slots (cloak, backpack, quiver) -- low
    // teens at the extreme. 32 is comfortably above that; an overflow degrades to
    // "the excess items are treated as off-set" (denied when the engine equips
    // them, never equipped by APMF; never a crash or an unbounded write).
    inline constexpr std::uint32_t kMaxEquipSet = 32;

    // ── ABI v8: a per-item HAND for SetEquipSetEx ──
    // The v7 SetEquipSet carries bare FormIDs and APMF equips them slot-less, so
    // the engine picks the hand: a slot-less one-hander always lands in the RIGHT
    // hand and displaces whatever was there (MFO measured it, 2026-09-15). That
    // makes two hand-held items undeclarable -- a dagger meant for the off-hand
    // evicts the sword. SetEquipSetEx carries one APMF_EquipEntry per item, with
    // the hand the client wants it in. APPEND-ONLY: never renumber a value.
    enum EquipSlot : std::uint8_t {
        kEquipSlot_Default = 0,   // the engine picks (v7 behaviour; the only choice for a
                                  //   two-hander, a bow, ammo, or body armor)
        kEquipSlot_Right   = 1,   // the RIGHT hand (Skyrim.esm EQUP 0x00013F42 `RightHand`)
        kEquipSlot_Left    = 2,   // the LEFT hand  (Skyrim.esm EQUP 0x00013F43 `LeftHand`)
                                  //   -- a one-hand weapon, a shield, a torch
    };

    // One declared item. EXACT LAYOUT (byte-shared with the client, pinned below):
    //   +0  form      RE::FormID (u32)  the item's BASE FormID
    //   +4  slot      u8                an EquipSlot value; anything else is treated
    //                                   as kEquipSlot_Default and logged once
    //   +5  reserved  u8[3]             MUST be 0 (a later ABI may define them; an
    //                                   APMF built against this one ignores them)
    //   size 8, alignment 4. Trivially copyable; APMF copies the array synchronously
    //   inside SetEquipSetEx and never retains the client's pointer.
    struct APMF_EquipEntry {
        RE::FormID   form;
        std::uint8_t slot;
        std::uint8_t reserved[3];
    };
    static_assert(sizeof(APMF_EquipEntry) == 8,  "APMF_EquipEntry is 8 bytes, byte-shared with clients");
    static_assert(alignof(APMF_EquipEntry) == 4, "APMF_EquipEntry aligns to 4");
    static_assert(offsetof(APMF_EquipEntry, form)     == 0, "form at +0");
    static_assert(offsetof(APMF_EquipEntry, slot)     == 4, "slot at +4");
    static_assert(offsetof(APMF_EquipEntry, reserved) == 5, "reserved at +5");

    // ── ABI v9: the equip CATEGORIES a claim OWNS or DENIES (SetEquipScope) ──
    // v7/v8 took the facet WHOLE: a declaration refused every off-set engine equip
    // of a governed type on the actor, so a client that only cared about the hands
    // still had to declare the body armor, the shield, the arrows and the torch --
    // or watch the engine's outfit refresh get refused for them (the first v8 deck
    // run: 117 CombatNode/OutfitApply would-denies, all against hand-only intent).
    // v9 SCOPES the authority. Each governed item COMPETES for one or more of these
    // categories (the map is ONE function, apmf::equipsink::Categorize, used by the
    // seat and by the enforce pass alike):
    //   ARMO, no shield bit (or no bits) -> Armor
    //   ARMO with the shield biped bit   -> Shield | Left
    //   ARMO with the shield bit AND any other biped bit (a modded "shield on
    //     back" piece)                   -> Shield | Left | Armor  (both kinds of
    //                                     bits -> both categories, so an Armor-only
    //                                     scope still holds the body slot against it)
    //   WEAP two-handed (2H sword, 2H axe, bow, crossbow)  -> Right | Left
    //   WEAP one-handed (incl. staff), equip slot LeftHand  -> Left
    //   WEAP one-handed (incl. staff), equip slot RightHand -> Right
    //   WEAP one-handed, any other/no slot (EitherHand)     -> Right | Left  (CONSERVATIVE:
    //                                     the hand is not known yet, so both are competed)
    //   AMMO                           -> Ammo
    //   LIGH (torch)                   -> Light | Left
    // APPEND-ONLY: never renumber a bit; a later ABI adds bits above bit 5 and
    // widens kEquipCat_All in ITS revision only. A v9 APMF masks unknown bits
    // off (logged once per handle), never refuses the call.
    enum EquipCategory : std::uint32_t {
        kEquipCat_None   = 0,         // a VALUE, not a bit: "no category" (an empty mask)
        kEquipCat_Armor  = 1u << 0,   // every ARMO without the shield bit (body, head, hands, feet, jewelry, cloak ...)
        kEquipCat_Shield = 1u << 1,   // an ARMO carrying BipedObjectSlot::kShield (competes for Left too)
        kEquipCat_Right  = 1u << 2,   // the right hand
        kEquipCat_Left   = 1u << 3,   // the left hand
        kEquipCat_Ammo   = 1u << 4,   // AMMO (arrows, bolts)
        kEquipCat_Light  = 1u << 5,   // LIGH (a torch; competes for Left too)
        kEquipCat_All    = 0x3Fu,     // every category this ABI defines
    };

    // The SCOPE of an equip-authority claim (ABI v9). EXACT LAYOUT (byte-shared
    // with the client, pinned below):
    //   +0  owned     u32  EquipCategory mask: the categories the client's declared
    //                      set is AUTHORITATIVE for. An engine equip competing for
    //                      any owned category is held to the set (in-set -> allow,
    //                      off-set -> deny); APMF's enforce pass equips a declared
    //                      item only when EVERY category it competes for is owned.
    //   +4  denied    u32  EquipCategory mask: the categories the actor must not
    //                      equip into AT ALL. An engine equip competing for any
    //                      denied category is refused even if it is in the set,
    //                      and the enforce pass never equips into it.
    //   +8  reserved  u32[2]  MUST be 0 (a later ABI may define them; a v9 APMF
    //                      ignores them).
    //   size 16, alignment 4. Trivially copyable; APMF copies it synchronously
    //   inside SetEquipScope and never retains the client's pointer.
    // The default scope of every claim is {owned = kEquipCat_All, denied = 0},
    // which is v8's behaviour verbatim. An equip competing for NO owned and NO
    // denied category passes the seat untouched (logged, `owned=0`), and the
    // enforce pass skips a declared item in such a category (logged once per
    // handle and set). The script/console and player-menu exemptions sit ABOVE
    // both masks: an exempt path passes whatever the scope says.
    struct APMF_EquipScope {
        std::uint32_t owned;
        std::uint32_t denied;
        std::uint32_t reserved[2];
    };
    static_assert(sizeof(APMF_EquipScope) == 16, "APMF_EquipScope is 16 bytes, byte-shared with clients");
    static_assert(alignof(APMF_EquipScope) == 4, "APMF_EquipScope aligns to 4");
    static_assert(offsetof(APMF_EquipScope, owned)    == 0, "owned at +0");
    static_assert(offsetof(APMF_EquipScope, denied)   == 4, "denied at +4");
    static_assert(offsetof(APMF_EquipScope, reserved) == 8, "reserved at +8");

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
        kCastFlag_LeftHand      = 1u << 1,   // hand hint (default right). Scopes the per-hand deny AND the
                                             //   0x0F equip seat (core/CastSeats.cpp / core/EquipGate.cpp).
        kCastFlag_Concentration = 1u << 2,   // client says the executed cast is a held stream (TTL floor applies).
        kCastFlag_DualCast      = 1u << 3,   // HINT: cast this spell with BOTH hands (dual-cast/empowered),
                                             //   not one. The engine seats (core/EquipGate.cpp, core/CastGate.cpp)
                                             //   admit the claimed spell/proxy as eligible on EITHER hand and deny
                                             //   every competing spell/staff item on BOTH hands while the claim
                                             //   stands, instead of scoping to the single hand kCastFlag_LeftHand
                                             //   would select. It is still only a HINT: the engine's own scoring/
                                             //   magicka/hand-readiness gates decide whether the AI actually arms
                                             //   both hands, and APMF never forces a second hand open -- a follower
                                             //   that cannot afford (or otherwise fails) the second hand simply
                                             //   casts single-hand, exactly as an unclaimed dual-cast attempt would
                                             //   degrade natively (composition, not substitution -- CLAUDE.md
                                             //   principle 3; never masked, principle 7). A client MUST NOT set
                                             //   kCastFlag_LeftHand alongside this bit -- dual-cast already claims
                                             //   both hands, so a hand hint is meaningless and unspecified; if both
                                             //   are set, DualCast takes priority (the seats' hand check is skipped
                                             //   entirely) rather than picking one hand arbitrarily.
                                             //   The mutual exclusivity above is between claims that DRIVE
                                             //   something. A kCastFlag_DenyHandOnly claim drives nothing, so it is
                                             //   outside this collision in both directions: it neither refuses nor
                                             //   is refused by a dual claim, and a dual claim never evicts one. See
                                             //   that flag's own text.

        // ── DENY-ONLY HAND CLAIM (added 2026-09-06; bits 4-7 were free) ──────────
        kCastFlag_DenyHandOnly  = 1u << 4,   // Claim the hand purely to DENY it. The client drives
                                             //   NOTHING here and APMF asks the engine to arm nothing:
                                             //   this is the missing half of the per-hand cast deny.
                                             //
                                             //   THE PROBLEM IT SOLVES. A single-hand kIntent_Cast claim
                                             //   scopes its deny to its OWN hand by design, so the other
                                             //   hand stays fully AI-governed -- and the field showed the
                                             //   AI using it: 25 Stone Runes, 6 Poison Sprays and 8 Raise
                                             //   Zombies all charged on the UNCLAIMED hand while the claim
                                             //   held the other one (2026-09-06 diagnosis, RC3 / audit rows
                                             //   P3-P4). A client that wants the follower to cast ONLY what
                                             //   it asked for had no way to say so, because there is no
                                             //   spell it wants on that second hand -- only silence.
                                             //
                                             //   WHAT APMF DOES WITH IT. The claim stands on exactly the
                                             //   hand kCastFlag_LeftHand selects (default right), with its
                                             //   DRIVEN FORM FORCED TO NONE: APMF zeroes spell, proxy and
                                             //   target on this claim regardless of what the request
                                             //   carried, so no seat can ever seat, drive or classify for
                                             //   it, and no delivery-flip proxy is minted. Every OTHER
                                             //   spell/staff competing for that hand is then denied through
                                             //   the SAME per-hand deny path a real claim already uses
                                             //   (core/CastGate.cpp 0x0A, core/EquipGate.cpp 0x0F). It
                                             //   fabricates no intent (CLAUDE.md principle 4): the client
                                             //   declared "nothing else here", and that is precisely and
                                             //   only what is enforced.
                                             //
                                             //   IT IS STILL A BOUNDED CAST CLAIM. Same TTL rules as any
                                             //   other kIntent_Cast claim (kCastDefaultTtlMs / ttlMs,
                                             //   clamped to kCastMaxTtlMs), and Repoint renews the window
                                             //   the same way -- so a crashed client's deny-only claim
                                             //   expires on its own, exactly like a driving one. A Repoint
                                             //   on THIS claim renews the TTL and nothing else: its
                                             //   `param.form` is forced back to 0, so a heartbeat can never
                                             //   put a driven form onto a hand the client declared closed
                                             //   (see Repoint in APMF_API_v3 below).
                                             //
                                             //   *** BASIS: THE TIE IS ENFORCED FOR YOU. ***
                                             //   A deny-only claim is an ordinary claim in APMF's ordinary
                                             //   arbitration -- highest basis owns -- with ONE rule added,
                                             //   and APMF enforces it rather than asking the client to
                                             //   remember it: AT AN EQUAL BASIS A DENY-ONLY CLAIM LOSES TO A
                                             //   DRIVING CAST CLAIM. (Elsewhere a tie still keeps the
                                             //   earliest claim; this is the one exception, and it applies
                                             //   at EVERY winner-selection APMF makes for the cast channel,
                                             //   so the channel OWNER and the per-hand gates can never
                                             //   disagree about which claim won.)
                                             //   The practical consequence, and the reason it is enforced
                                             //   instead of documented: a client that issues every claim at
                                             //   ONE uniform basis can put the floor down FIRST and still
                                             //   have its own next gambit take the hand the instant that
                                             //   gambit publishes -- no release/re-request gap, no
                                             //   self-deny -- with the floor still standing underneath when
                                             //   the gambit ends. This is enforcement of a fact the CLIENT
                                             //   declared (this flag says the claim drives nothing), not a
                                             //   precedence APMF invented.
                                             //   What is still the client's call is a STRICT inversion: a
                                             //   floor requested at a basis genuinely ABOVE a driving cast
                                             //   claim wins, because there the client really did say the
                                             //   hand must stay shut. Nothing is re-ranked or refused, but
                                             //   it IS reported loudly at claim time ("[ch.8b] ...
                                             //   DENY-ONLY FLOOR OUTRANKS A LIVE CAST ...",
                                             //   core/ControlMap.cpp), because a follower holding a hand and
                                             //   casting nothing is the hardest failure to read from a log
                                             //   (CLAUDE.md principles 5 and 7).
                                             //
                                             //   *** IT COEXISTS WITH A DUAL-CAST CLAIM, BOTH WAYS. ***
                                             //   kCastFlag_DualCast and a single-hand claim are mutually
                                             //   exclusive on one actor (see that flag) -- the loser of that
                                             //   collision is refused outright. A DENY-ONLY claim is OUTSIDE
                                             //   that collision entirely, in both directions: it asks the
                                             //   engine to arm nothing, so there is nothing for a dual claim
                                             //   to collide with. A floor may be requested while a dual
                                             //   claim stands, a dual claim may be requested while a floor
                                             //   stands, neither is refused, and a dual claim NEVER evicts a
                                             //   floor -- so the hand does not reopen for a client tick each
                                             //   time a dual cast starts or ends. On the hands the dual
                                             //   claim occupies it outranks the floor (or, at an equal
                                             //   basis, displaces it by the tie rule above); when it ends
                                             //   the floor is still there, with no gap.
                                             //
                                             //   *** WHAT A FLOORED HAND ADMITS: NOTHING. ***
                                             //   Not "nothing except the client's own spells" -- NOTHING.
                                             //   While the floor stands, core/CastGate.cpp (0x0A CheckCast)
                                             //   and core/EquipGate.cpp (0x0F CheckShouldEquip) deny every
                                             //   spell and staff on that hand, INCLUDING the claiming
                                             //   client's own, because the floor names no form to admit. So
                                             //   the rule is: CLAIM BEFORE YOU EXPECT THE AI TO ARM
                                             //   ANYTHING ON A FLOORED HAND -- a driving kIntent_Cast claim
                                             //   (which, per the tie rule above, takes the hand at an equal
                                             //   basis) is how you reopen it, not an unclaimed cast.
                                             //   VERIFIED, 2026-09-06, because the natural worry is that a
                                             //   client's own DIRECT force-cast would be denied by its own
                                             //   floor: it is NOT. A direct
                                             //   `GetMagicCaster(kInstant)->CastSpellImmediate(...)` force
                                             //   does not consult CheckCast at all, so this gate cannot see
                                             //   it -- MagicCaster::CastSpellImmediate is vtable slot 0x01
                                             //   and CheckCast is 0x0A, and the engine's CastSpellImmediate
                                             //   never calls it. Two independent pieces of field evidence,
                                             //   not a reading of the header: (1) MFO hooks CheckCast on the
                                             //   SAME ActorMagicCaster vtable[0] slot 0x0A and records, in
                                             //   its own source, that "the kInstant ConcProxy direct force
                                             //   (CastSpellImmediate) does NOT deliberate through these
                                             //   hooks, so it is never vetoed and needs no bound"
                                             //   (MFO CasterConsent.cpp); (2) MFO's ENGINE_NOTES 0.9
                                             //   measured a follower with ZERO magicka casting through
                                             //   CastSpellImmediate forever -- a CheckCast consult would
                                             //   have refused it with kMagickaInsufficient on the first
                                             //   try. A floor therefore closes the hand to the AI's
                                             //   deliberation, which is exactly and only what it claims to
                                             //   do; it does not close the client's own direct force.
                                             //
                                             //   COMPATIBILITY. This is an APPEND-ONLY bit inside the
                                             //   already-frozen `flags` word -- no struct field added,
                                             //   reordered or retyped, no vtable slot, so kABIVersion is
                                             //   NOT bumped (a bump would make a client asking for the new
                                             //   version get a null interface from an older APMF.dll --
                                             //   APMF_GetInterface refuses anything above its own
                                             //   kABIVersion). The consequence a client must know: an APMF
                                             //   built before this bit existed IGNORES it, and the claim
                                             //   then stands as a degenerate no-form claim that denies
                                             //   nothing. Ship the pair together.

        // ── POSITION CAST (ABI v11, 2026-09-23; bit 5 was free) ─────────────────
        kCastFlag_AtPosition    = 1u << 5,   // Deliver `param.form` (a SpellItem) at the WORLD POINT
                                             //   `param.pos` instead of at an actor. RequestEx ONLY:
                                             //   APMF_CastRequest has no position field, so RequestCast
                                             //   with this bit is REFUSED by name. Gate on
                                             //   abiVersion >= 11 before setting it (an older APMF
                                             //   ignores the bit and makes an ordinary actor-target claim).
                                             //
                                             //   OFF BY DEFAULT, AND NOT AN ENDPOINT. It rests on
                                             //   Docs/INVARIANTS.md #0 action (e), adopted by marth with a
                                             //   standing condition: the actor does NOT animate, and proper
                                             //   animations are required for ALL actions, so no client may
                                             //   ship a user-facing action on this alone (an animated path
                                             //   through the engine's own cast seats is the goal; this is a
                                             //   stepping stone or the delivery half of one). So
                                             //   [PositionCast] bPositionCast defaults to 0 and every
                                             //   request is REFUSED by name until a user sets it to 1.
                                             //   kIntent_Travel's kTravel_ToPosition does not depend on it.
                                             //
                                             //   WHAT APMF DOES. On the game thread it places a
                                             //   non-persistent XMarker (Skyrim.esm 0x3B) at the point, in
                                             //   the cell that CONTAINS the point (outdoors: the loaded,
                                             //   attached exterior cell of the actor's worldspace at those
                                             //   coordinates, TES::GetCell; indoors: the actor's own cell),
                                             //   and casts the spell FROM
                                             //   that marker, blamed on the actor: the marker's instant
                                             //   caster runs InterruptCast(false) then CastSpellImmediate(
                                             //   spell, false, none, 1.0, false, 0.0, actor). That is the
                                             //   exact sequence the engine's own Papyrus Spell.RemoteCast
                                             //   runs (Docs/ADDRESS-TABLE-2026-09-15.md, ADDENDUM 2026-09-23). The actor
                                             //   does not animate and its hands are untouched. APMF deletes
                                             //   the marker one frame later (core/PositionCast.cpp).
                                             //
                                             //   WHY A MARKER CASTS AND THE ACTOR DOES NOT. The engine
                                             //   places a Target Location spell cast by an NPC at the
                                             //   CASTER's own magic node. It never reads the target it was
                                             //   handed. So "the actor casts at a marker" lands at the
                                             //   actor's hand. A caster that IS the marker lands at the
                                             //   marker. Disassembly on both runtimes, same ADDENDUM.
                                             //
                                             //   WHAT IS ACCEPTED. A SpellItem with Target Location
                                             //   delivery, fire-and-forget, no Summon Creature effect, and
                                             //   NO PROJECTILE on any effect. The engine launches a Target
                                             //   Location projectile only from the player's crosshair pick
                                             //   (never for a marker caster, both runtimes), so a rune, a
                                             //   trap or a lobbed shot would silently place nothing: it is
                                             //   REFUSED on the game thread (logged).
                                             //   Everything else is REFUSED by name in APMF.log:
                                             //   * a SUMMON. The engine applies a summon effect ONLY to the
                                             //     actor that cast it, so a marker can never summon. An
                                             //     NPC's own summon already lands in front of it: the
                                             //     engine's SummonCreatureEffect picks that spot itself.
                                             //   * Self / Touch / Aimed / Target Actor delivery (use an
                                             //     ordinary kIntent_Cast with an actor target).
                                             //   * concentration and constant-effect spells.
                                             //   * disease / ability / addiction spell types (Papyrus
                                             //     RemoteCast refuses the same three).
                                             //   * any other cast flag set alongside this one.
                                             //   * a point whose cell is not loaded and attached (or not in
                                             //     the actor's worldspace).
                                             //   * THE ACTOR'S CAST FACET IS OWNED: a live kIntent_Cast
                                             //     claim that would outrank a driving request at this basis
                                             //     (a higher basis, a kCastFlag_DenyHandOnly floor included,
                                             //     or an equal basis that drives something) refuses the
                                             //     request synchronously.
                                             //   AT THE CALL (kInvalidHandle): the flag, the point, a finite
                                             //   basis and the facet check. ON THE GAME THREAD, logged as
                                             //   "[poscast] request N REFUSED ...": the spell checks (a form
                                             //   lookup cannot be made off the main thread safely), the
                                             //   actor and cell checks, and the facet check AGAIN.
                                             //   A Repoint carrying this bit on a live cast claim is
                                             //   REFUSED and changes nothing.
                                             //
                                             //   IT IS A ONE-SHOT, NOT A CLAIM. It never enters the control
                                             //   map, so no engine seat ever sees it (it can never be read
                                             //   as an actor target) and it holds no facet. RequestEx
                                             //   returns a handle for log correlation only: IsClaimLive is
                                             //   false for it, Repoint and Release are no-ops. The
                                             //   client owns resource cost (APMF charges no magicka, the
                                             //   same as every CastSpellImmediate).

        // ── Bits 8-15: STOP PERCENT (added in-place; the word is byte-frozen) ────
        // The seat that owns a concentration channel's duration is
        // CombatMagicCaster::CheckStopCast (vfunc 0x07, core/CastSeats.cpp). Left
        // native it stops when the TARGET's restore AV reaches
        // lerp(0,0.5,defensiveMult)+0.25 -- so a client healing at a 60% threshold
        // would get a ~0.25s pulse and an InterruptCast. These eight bits let the
        // client state its OWN stop threshold as a whole percent 1..100 of the
        // target's PERMANENT actor value; the seat stops the channel the moment the
        // target reaches it.
        //
        // This is an APPEND-ONLY addition WITHIN the existing `flags` word -- no
        // struct field was added, reordered or retyped, so APMF_CastRequest's layout
        // is byte-identical and a client built before this reads/writes 0 here. 0
        // means "no client threshold": the seat then stops at FULL restoration
        // (pct >= 1.0), on target death/unresolvability, on claim release, or at the
        // claim's TTL -- never a channel that runs forever. Build it with
        // APMF_API::MakeStopPct(80).
        kCastFlag_StopPctShift  = 8,
        kCastFlag_StopPctMask   = 0xFFu << 8,
    };

    // Build the bits 8-15 stop-percent field for CastFlags (see above). `pct` is a
    // whole percent of the target's PERMANENT actor value, 1..100; 0 = "no client
    // threshold" (stop at full restoration). Clamped, never asserts.
    inline constexpr std::uint32_t MakeStopPct(std::uint32_t pct) {
        return ((pct > 100u ? 100u : pct) << kCastFlag_StopPctShift) & kCastFlag_StopPctMask;
    }
    // Read it back. Returns 0 when the client set none.
    inline constexpr std::uint32_t ReadStopPct(std::uint32_t flags) {
        return (flags & kCastFlag_StopPctMask) >> kCastFlag_StopPctShift;
    }

    // ABI v5: cast-window TTL bounds (kIntent_Cast is ALWAYS bounded -- never a
    // standing hold, design.md §5a). ttlMs == 0 -> kCastDefaultTtlMs; any value is
    // clamped to kCastMaxTtlMs. A crashed/forgetful client can never leave a
    // standing cast hold: the claim auto-releases at expiry (ControlMap TTL pass).
    //
    // REPOINT RENEWS THE WINDOW (added 2026-09-06; no ABI change -- no slot, no
    // field, no flag). Calling Repoint(handle, &param) on a live kIntent_Cast claim
    // moves its deadline to now + the SAME (already-clamped) ttlMs it was granted,
    // in addition to updating the stored param. A client that wants to hold a cast
    // longer than one window therefore HEARTBEATS with Repoint instead of letting
    // the claim die and re-requesting: the release/re-request cycle left the actor
    // measurably UNCLAIMED for 0.26-0.61 s every window, and foreign spells equipped
    // and charged inside that gap (2026-09-06 field diagnosis, RC2). The bound is
    // unchanged for a client that stops asking -- the claim still dies ttlMs after
    // its LAST RequestCast/Repoint -- so this is a renewable FLOOR, not a standing
    // hold: crash-safety identical, live state no longer killed mid-cast.
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
        RE::FormID    target;   // intended target actor (0 = self). LOAD-BEARING since
                                //   feat/ai-cast-seats-impl: the 0x0A GetMagicTarget seat hands
                                //   this to the engine as the AI's magic target, and the 0x0D
                                //   SetupAimController seat as its aim override, so the NPC's own
                                //   cast lands here. (It was RECORD ONLY before the seats.)
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
                               //   move-to-point destination. READ SINCE ABI v11 by
                               //   kIntent_Cast when ival carries kCastFlag_AtPosition (a
                               //   position cast; see that flag). When the flag is set,
                               //   (0,0,0) is a legal point: the flag, not the value, says
                               //   the field is in use. An actor target still rides
                               //   kIntent_Cast's APMF_CastRequest (`req.target`,
                               //   load-bearing at the engine seats).
    };

    // ── APMF_Param field usage, per Intent (at a glance) ────────────────────────
    // A client may always pass an APMF_Param (RequestEx/Repoint zero-init a v1-era
    // caller sees as "no param" too). This table says which Intents actually READ
    // a field TODAY versus accept-and-ignore it -- read the per-Intent comments
    // above for the full picture; this is the quick-scan version.
    //
    //   form   kIntent_SelectSpell     the spell FormID
    //   ival   kIntent_SelectSpell     RETIRED/RESERVED -- the +ACT hand mode (bits 0-1) and
    //                                  drive opt-in (bit 2) are no longer read by any
    //                                  channel; ch.8 is gate-only again. Use kIntent_Cast.
    //   target kIntent_SelectSpell     RETIRED/RESERVED (it was the +ACT drive's target)
    //   pos    kIntent_SelectSpell     RESERVED (a location-delivery target; never wired)
    //   form   kIntent_CombatTarget    the target actor
    //   form   kIntent_ShoutPower      the shout/power FormID
    //   form   kIntent_OfferPackage    the TESPackage FormID
    //   form   kIntent_Equipment       optional, gates re-equip when set (see above)
    //   ival   kIntent_CombatAction    a CombatActionCategory bitmask (see below)
    //   form   kIntent_Cast            the spell (degenerate RequestEx form); RequestCast for
    //   ival   kIntent_Cast            the rich payload -- ival = CastFlags on the degenerate form
    //                                  (req.target / req.flags' hand + stop-percent are read by
    //                                  the engine seats, core/CastSeats.cpp)
    //   pos    kIntent_Cast            ABI v11: the WORLD POINT of a position cast, read ONLY
    //                                  when ival has kCastFlag_AtPosition (then form = the spell,
    //                                  ival = that flag alone). Ignored otherwise.
    //   ival   kIntent_EquipAuthority  an EquipAuthFlags bitmask (the worn set itself is NOT a
    //                                  param field -- it is declared with SetEquipSet, ABI v7)
    //   form   kIntent_Travel          the DESTINATION: an object REFERENCE or a CELL (REQUIRED;
    //                                  a 0 form, or any other record type, is refused)
    //   fval   kIntent_Travel          the arrival radius in units (0 => 75u default, clamped 50-512);
    //                                  not consulted when the destination is a CELL
    //   ival   kIntent_Travel          a TravelFlags bitmask (see above)
    //   pos    kIntent_Travel          ABI v11: the destination POINT when ival has kTravel_ToPosition
    //                                  (form must then be 0; APMF walks the actor to its own XMarker
    //                                  there). Without the flag: REFUSED if non-zero.
    //   form   kIntent_TargetPin       ABI v13: the TARGET actor (REQUIRED; 0 or the actor itself
    //                                  is refused). No other field is read.
    //   none   every other Intent      accepted, not yet read by the channel
    //
    // fval is read ONLY by kIntent_Travel (ABI v10); it stays reserved for a
    // future per-request bias on ch.11, scale on ch.1a, factor on ch.16. target is
    // not read by any Intent OTHER than kIntent_SelectSpell yet. pos is read by
    // kIntent_Cast with kCastFlag_AtPosition and by kIntent_Travel with kTravel_ToPosition
    // (both ABI v11). Without its flag, kIntent_Travel refuses a non-zero pos.
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
        //
        // -- ON A kIntent_Cast CLAIM, REPOINT IS A HEARTBEAT, NOT A RE-AIM ---------
        // (pinned 2026-09-06.) For the cast channel specifically, Repoint has two
        // jobs and one hard limit:
        //   * IT RENEWS THE TTL. The deadline moves to now + the SAME (already
        //     clamped) ttlMs the claim was granted, so a client that wants a cast
        //     window longer than one ttlMs HEARTBEATS with Repoint instead of letting
        //     the claim lapse and re-requesting. See kCastDefaultTtlMs below. This is
        //     the intended, invited use.
        //   * IT UPDATES THE NON-FORM PARAM FIELDS, exactly as on every other channel.
        //   * IT CANNOT CHANGE WHAT THE CLAIM CASTS. A `param.form` different from the
        //     claim's current spell is REFUSED (the claim keeps the spell it was
        //     requested with) and the refusal is logged loudly -- never silently
        //     honoured, never silently dropped. A cast claim's delivery-flip proxy,
        //     its resolved target handle and its CastFlags were all resolved AGAINST
        //     its original spell inside RequestCast, and Repoint re-runs none of that,
        //     so honouring a form swap would leave the claim NAMED for spell B while
        //     DRIVING A's proxy at A's target. To cast something else: Release the
        //     handle and RequestCast the new spell.
        //   * ON A kCastFlag_DenyHandOnly CLAIM `param.form` IS ALWAYS FORCED TO 0,
        //     for the same reason RequestCast forces it: that claim drives nothing for
        //     its whole life. A Repoint carrying a form is logged and ignored.
        // A same-form heartbeat -- the shape a client should be sending -- hits none
        // of those limits and behaves exactly as documented above.
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
        // weight `basis`, carrying the rich APMF_CastRequest, for a bounded TTL.
        //
        // WHAT HAPPENS (updated for the shipped v0.9.1/v0.9.2 behaviour -- the older
        // "the CLIENT fires its own animated cast" contract is RETIRED): while the
        // claim stands, APMF answers the engine's own cast-decision points so the
        // NPC's OWN combat AI selects, equips, charges, aims, fires and channels
        // `req.spell` at `req.target`. **APMF still makes NO engine cast call of any
        // kind** -- no EquipSpell, no CastSpell, no CastSpellImmediate, no anim-graph
        // write. The animation is the game's own because the game is the one casting.
        // The CLIENT brings no cast mechanism at all; it only declares what and where.
        //
        // This is what makes a heal-OTHER cast possible: the vanilla combat AI cannot
        // classify a beneficial spell aimed at another actor, so it never builds one
        // as a candidate and never considers casting it. APMF supplies that one
        // missing classification decision and the engine does the rest.
        //
        // `req` is READ AND COPIED synchronously inside the call;
        // APMF never retains the pointer, so a stack temporary is fine. Returns a
        // handle to release later, or kInvalidHandle if the cast channel is not
        // registered. A kCastFlag_FromPackage request whose package carries no
        // readable spell input is REFUSED at drain (the handle becomes inert; APMF
        // never runs, offers, or evaluates the package). Safe from any thread (POD
        // captured; applied on the game thread).
        Handle (*RequestCast)(RE::FormID actor, float basis, const APMF_CastRequest* req);
    };

    // The v6 interface: APMF_API_v5's members verbatim (via prefix EXTENSION, same
    // shape as every prior revision), then two appended cast-claim OBSERVABILITY
    // slots. Neither changes what APMF does -- both are read-only queries closing
    // an MFO<->APMF blind spot diagnosed 2026-09-06: a client that called
    // RequestCast had no way to learn (a) the delivery-flip proxy FormID APMF
    // minted internally for that claim, so it could only ever match the ORIGINAL
    // spell and never recognised its own proxy's cast, and (b) whether its claim
    // had already auto-expired at the TTL (design.md §5a), so it kept believing a
    // long-dead handle was still live. This header is BYTE-SHARED with MFO: the
    // declaration below is authoritative and must be mirrored byte-identically on
    // the client side.
    struct APMF_API_v6 : APMF_API_v5 {
        // Hand back the delivery-flip proxy FormID APMF minted internally for the
        // cast claim behind `handle` (see APMF_CastRequest::proxy and
        // RequestCast's doc comment -- APMF fabricates this substitute FormID for
        // delivery, distinct from the client-supplied `req.proxy`). Returns 0 if
        // `handle` is unknown/stale/released, or if that claim minted no proxy.
        // Read-only; does not affect arbitration or claim lifetime in any way.
        // Safe from any thread (RCU snapshot read, same discipline as
        // TryGetCastClaim/TryGetCastSeatClaim on the APMF side).
        RE::FormID (*GetCastProxy)(Handle handle);

        // True while APMF still holds the claim behind `handle` -- i.e. it has not
        // been released (by the client, or as the losing side of a re-arbitration)
        // and has not auto-expired at its TTL (design.md §5a "never a standing
        // hold"). False for an unknown/stale/already-gone handle. A client should
        // treat `false` exactly like `kInvalidHandle` would have been treated
        // before this existed: re-request rather than keep believing a dead handle
        // is still in force. Read-only; safe from any thread, same discipline as
        // GetCastProxy above.
        bool (*IsClaimLive)(Handle handle);
    };

    // The v7 interface: APMF_API_v6's members verbatim (prefix EXTENSION, same shape
    // as every prior revision), then ONE appended slot: the equip-authority
    // declaration. This header is BYTE-SHARED with MFO: the declaration below is
    // authoritative and must be mirrored byte-identically on the client side.
    //
    // WHY A BUMP (INVARIANTS #14b): a new function-pointer slot needs an
    // `abiVersion >= 7` test before a client may call it. A v1..v6 client reading
    // this object through its own struct pointer still sees exactly its prefix.
    struct APMF_API_v7 : APMF_API_v6 {
        // DECLARE the worn set for the actor behind an EXISTING kIntent_EquipAuthority
        // claim (`handle`, as returned by RequestEx/Request). `forms` are BASE
        // FormIDs (weapons, armor, jewelry -- anything ActorEquipManager equips);
        // `count` is silently clamped to kMaxEquipSet. `forms` is READ AND COPIED
        // synchronously inside the call -- APMF never retains the pointer, same
        // contract as RequestEx/Repoint's `param` and SetSpellAllowList's array (a
        // stack-local array is fine). Applied on the game thread at the next Drain.
        //
        // WHAT HAPPENS, in order, when the declaring claim OWNS the channel:
        //   1. the set is published into the lock-free snapshot the equip seat reads
        //      (Docs/INVARIANTS.md #12), so from the next engine equip on, any item
        //      NOT in `forms` is refused for this actor (ABI v9: within the claim's
        //      OWNED categories only, default all -- see SetEquipScope; path-classified and logged;
        //      Papyrus/console pass unless kEquipAuth_DenyScript; observe-only logs
        //      `would-deny` and refuses nothing);
        //   2. one main-thread pass equips each declared item the actor is not
        //      already wearing, through the engine's own ActorEquipManager::EquipObject
        //      (queued, not forced, sounds on) -- exactly what the client would have
        //      called itself, now issued by the framework so the seat lets it through.
        //      APMF never UNEQUIPS anything: the engine's own worker displaces whatever
        //      occupies a declared item's slot, the ordinary way. No re-assert loop
        //      follows: the seat is what keeps the engine from undoing the set.
        // DECLARE, DO NOT TICK. Call this when the loadout CHANGES. Each call on the
        // owning claim is a declaration event and walks the inventory; an item APMF
        // already queued is held 3 s before it is queued again, so a per-tick
        // re-send is bounded, logged work, not a queue pile-up -- but it is still
        // wasted work. The set MUST BE SIMULTANEOUSLY
        // WEARABLE: a two-hander and a shield, or two items for one slot, make the
        // engine displace one with the other on every pass. That is the client's
        // error and the log will show it; APMF never picks which one wins.
        // A declaration on a claim that does NOT own the channel is STORED (same
        // non-owning semantics as Repoint/SetSpellAllowList) and takes effect the
        // moment that claim wins arbitration: the seat re-arms on ITS set and the
        // same one-pass equip runs for it (a win is a declaration event too).
        // `count == 0` or `forms == nullptr` CLEARS the declaration: the seat passes
        // every engine equip through again (nothing is unequipped), and a Release
        // does the same. A stale/unknown `handle`, or a handle whose claim is on a
        // channel OTHER than kIntent_EquipAuthority, is a silent no-op (mirrors
        // Repoint's own no-op-on-unknown-handle discipline). Thread-safe (enqueues).
        //
        // NOT provided, deliberately: a per-item callback. The seat runs on whatever
        // engine thread performs the equip; running client code there would hand the
        // client a combat-thread re-entrancy hazard it cannot see. The declaration
        // IS the policy; the log line (`[apmf][equip-obs]`) is the observability.
        void (*SetEquipSet)(Handle handle, const RE::FormID* forms, std::uint32_t count);
    };

    // The v8 interface: APMF_API_v7's members verbatim (prefix EXTENSION), then ONE
    // appended slot: the per-item-hand form of the equip-authority declaration.
    // This header is BYTE-SHARED with MFO: the declaration below is authoritative
    // and must be mirrored byte-identically on the client side.
    //
    // WHY A BUMP (INVARIANTS #14b): a new function-pointer slot needs an
    // `abiVersion >= 8` test before a client may call it. A v1..v7 client reading
    // this object through its own struct pointer still sees exactly its prefix;
    // v7's SetEquipSet is unchanged and is now implemented AS SetEquipSetEx with
    // every entry's slot = kEquipSlot_Default.
    struct APMF_API_v8 : APMF_API_v7 {
        // DECLARE the worn set WITH A HAND PER ITEM. Everything SetEquipSet's doc
        // comment says holds here too (same claim, same clamp to kMaxEquipSet, same
        // synchronous copy, same clear on count == 0 / entries == nullptr, same
        // no-op on a stale handle, same declare-do-not-tick rule). The differences:
        //   * `entries[i].slot` names the hand APMF equips `entries[i].form` into:
        //     kEquipSlot_Right / kEquipSlot_Left pass the engine's own RightHand /
        //     LeftHand equip slot (Skyrim.esm EQUP 0x13F42 / 0x13F43) to
        //     ActorEquipManager::EquipObject; kEquipSlot_Default passes none and the
        //     engine picks, exactly as v7 does. Use Right/Left ONLY for hand-held
        //     items (a one-hand weapon, a shield, a torch); declare a two-hander, a
        //     bow, ammo and body armor with kEquipSlot_Default.
        //   * "not worn" for a Right/Left entry means "not in THAT hand" (the
        //     engine's own equipped-object-per-hand read), so a sword the actor holds
        //     in the right hand, declared Left, is re-equipped into the left hand.
        //     A Default entry is "not worn" exactly as in v7 (no worn instance).
        //   * The SAME form may appear twice, once per hand (two identical daggers,
        //     dual-wielded): APMF counts the instances it needs per hand against the
        //     inventory count, equips each, and skips (logged) any instance the
        //     actor does not own enough copies for.
        //   * The DENY half is unchanged: the seat compares FormIDs only. A declared
        //     item is in-set whichever hand the engine tries to put it in; the hand
        //     is what APMF's own equip pass enforces, not what the seat refuses.
        // GOVERNED TYPES (v8; applies to v7's SetEquipSet too): the seat governs only
        // ARMO, WEAP, AMMO and LIGH (torch) equips. Every other form type -- a potion,
        // food, a scroll, an ingredient, a book -- passes the seat untouched (logged
        // once per actor and type at debug level), so DrinkPotion and the AI's own
        // potion use are never refused. APMF's equip pass likewise skips (and logs) a
        // declared item of any other type: equipping a potion is drinking it.
        void (*SetEquipSetEx)(Handle handle, const APMF_EquipEntry* entries, std::uint32_t count);

        // TRUE only when the equip seat is INSTALLED (the two worker call sites
        // byte-verified and patched on this runtime) AND APMF.ini's [EquipAuthority]
        // bEquipObserveOnly is 0 -- i.e. an off-set engine equip on a claimed actor
        // is actually REFUSED, not merely logged `would-deny`. FALSE in observe
        // mode, before kDataLoaded, with bEquipAuthority=0, on VR, on a runtime
        // other than 1.6.1170 / 1.5.97, or after a site-verify refusal. A claim's
        // own kEquipAuth_ObserveOnly bit is not consulted (the client that set it
        // knows). Read-only, safe from any thread, may change once at kDataLoaded
        // and never afterwards.
        //
        // WHY (ABI v8, MFO wiring review SEV-2 F1): a client that turns its OWN
        // equips off on a successful kIntent_EquipAuthority claim must be able to
        // tell "APMF holds the set" from "APMF is only watching". Paired with the
        // v8 rule that a kIntent_EquipAuthority Request/RequestEx returns
        // kInvalidHandle while the seat is NOT installed (so the client's documented
        // degrade path runs), this call distinguishes the remaining case: claimed
        // and installed, but observe-only.
        bool (*IsEquipAuthorityEnforced)(void);
    };

    // The v9 interface: APMF_API_v8's members verbatim (prefix EXTENSION), then ONE
    // appended slot: the SCOPE of an equip-authority claim. This header is
    // BYTE-SHARED with MFO: the declaration below is authoritative and must be
    // mirrored byte-identically on the client side.
    //
    // WHY A BUMP (INVARIANTS #14b): a new function-pointer slot needs an
    // `abiVersion >= 9` test before a client may call it. A v1..v8 client reading
    // this object through its own struct pointer still sees exactly its prefix;
    // every such client runs with the default scope {kEquipCat_All, 0}, which is
    // v8's whole-facet behaviour unchanged.
    struct APMF_API_v9 : APMF_API_v8 {
        // SCOPE the authority of an EXISTING kIntent_EquipAuthority claim (`handle`)
        // to the categories in `scope->owned`, and refuse the actor's equips into the
        // categories in `scope->denied` outright. See APMF_EquipScope for what each
        // mask means and EquipCategory for how an item is mapped to categories.
        //   * `scope` is READ AND COPIED synchronously inside the call (APMF never
        //     retains the pointer; a stack temporary is fine). Applied on the game
        //     thread at the next Drain, like SetEquipSetEx.
        //   * `scope == nullptr` RESETS the claim to the default {kEquipCat_All, 0}.
        //   * Bits outside kEquipCat_All are MASKED OFF (logged once per handle),
        //     never refused: a client built against a later ABI degrades to what
        //     this APMF knows.
        //   * A stale/unknown handle, or a handle on a channel other than
        //     kIntent_EquipAuthority, is a silent no-op (SetEquipSet's discipline).
        //   * The scope is stored on the claim whether or not it owns the channel
        //     (non-owning semantics, like a declaration) and rides the SAME snapshot
        //     read as the set: the seat never sees a set from one generation and a
        //     scope from another. An applied CHANGE on the OWNING claim is a
        //     declaration event -> one enforce hop after Publish, exactly like an
        //     applied SetEquipSetEx. An unchanged re-send fires nothing.
        //   * The seat's verdict for a governed equip on a claimed actor, in order:
        //       script/console without DenyScript              -> allow (exempt)
        //       PlayerMenu without DenyPlayerMenu               -> allow (player agency)
        //       competes & denied                               -> deny (even if in-set)
        //       competes & owned: no declaration or in-set      -> allow
        //       competes & owned: off-set                       -> deny
        //       else (no owned, no denied category)             -> allow (owned=0)
        //     Observe-only turns each deny into `would-deny`.
        //   * The enforce pass equips a declared item only when EVERY category it
        //     competes for is owned and NONE is denied; the rest are skipped and
        //     counted (`skipped-unowned` / `skipped-denied` on the pass line). It
        //     still never unequips anything.
        // A WEAP declared with kEquipSlot_Default that is one-handed competes for
        // BOTH hands (the hand is not known until the engine picks); declare the hand
        // with SetEquipSetEx when only one is owned.
        void (*SetEquipScope)(Handle handle, const APMF_EquipScope* scope);
    };

    // ── ABI v11: SPACE QUERIES ──────────────────────────────────────────────────
    // Two read-only questions a client asks before it declares something that needs a
    // place in the world: "where is an empty, standable spot over there?" and "which
    // hostile actors are inside this sphere?". They serve a position cast
    // (kCastFlag_AtPosition), a travel point (kTravel_ToPosition), and anything else
    // that has to pick a point. APMF ANSWERS; the client DECIDES (CLAUDE.md principle 4).
    //
    // WHAT A QUERY IS NOT. It claims no facet, holds nothing, writes nothing into the
    // world or the control map, and has no handle. One intent is still one facet: a
    // query is not an intent at all, and there is no combined query-and-cast call.
    //
    // THREADING: SYNCHRONOUS, TRUE MAIN THREAD ONLY. Ray casts and the engine's
    // hostility test must run on the game's main thread. A query called from any other
    // thread does no work and returns kQuery_NotMainThread at once. The main thread
    // here is the thread that runs the PLAYER's Actor::Update, the same seat APMF drains
    // its own request queue from. A client already on that thread (for example inside
    // its own player-Update pump; MFO's MainThread::Post runs there) calls straight in.
    // A client on a worker thread moves the call onto that thread first. SKSE's
    // TaskInterface::AddTask is NOT that thread (it runs on a job worker).
    //
    // SIZES. Every v11 struct starts with `size`. The caller sets it to
    // sizeof(struct) as compiled against its header. APMF refuses an input whose size
    // is below the v11 layout (kQuery_BadArgs) and never writes an output past the size
    // the caller declared. A later ABI may append fields; an older caller keeps working.
    //
    // No exception crosses the boundary: a throw inside a query returns kQuery_Failed.

    enum QueryStatus : std::uint32_t {
        kQuery_Ok             = 0,
        kQuery_NotMainThread  = 1,   // called off the main thread; nothing was done
        kQuery_BadArgs        = 2,   // null pointer, short `size`, or a non-positive distance/radius
        kQuery_Unsupported    = 3,   // VR, a runtime APMF has not verified, or before kDataLoaded
        kQuery_NoOrigin       = 4,   // the origin/side FormID is not a loaded reference (actor for side)
        kQuery_NoWorld        = 5,   // the origin has no attached cell or no havok world
        kQuery_Blocked        = 6,   // geometry (or an actor) between the origin and the point; detail = layer
        kQuery_NoGround       = 7,   // nothing to stand on within maxDrop under the point: a void or a drop
        kQuery_NotGround      = 8,   // the ground ray hit something that is not static, terrain or ground; detail = layer
        kQuery_Occupied       = 9,   // a live actor stands inside the clearance; detail = its FormID
        kQuery_NoClearance    = 10,  // geometry inside the clearance radius; detail = layer
        kQuery_Ledge          = 11,  // the clearance ring is not level (a drop or step over 48u at its edge)
        kQuery_Failed         = 12,  // an exception was caught; nothing was returned
    };

    enum SpaceFlags : std::uint32_t {
        kSpaceFlag_None          = 0,
        kSpaceFlag_OriginIsPoint = 1u << 0,   // walk from (originX, originY, originZ), a FEET-level point in
                                              //   the origin reference's cell and worldspace, instead of from
                                              //   the reference's own position
        kSpaceFlag_UseHeading    = 1u << 1,   // walk along `heading` (radians, Skyrim's angle Z: forward is
                                              //   (sin h, cos h)) instead of the origin reference's facing
        kSpaceFlag_ClampToWall   = 1u << 2,   // a wall before `distance` shortens the walk to (hit - clearance)
                                              //   instead of returning kQuery_Blocked. Nothing else is relaxed.
    };

    // FindEmptySpace input. EXACT LAYOUT (byte-shared): 44 bytes.
    struct APMF_SpaceQuery {
        std::uint32_t size;        // = sizeof(APMF_SpaceQuery)
        RE::FormID    origin;      // REQUIRED. A loaded reference, actor or not. Its cell supplies the havok
                                   //   world. If it is an actor, the rays skip its own capsule.
        float         originX;     // read only with kSpaceFlag_OriginIsPoint
        float         originY;
        float         originZ;
        float         heading;     // read only with kSpaceFlag_UseHeading
        float         distance;    // how far along the direction, > 0; clamped to 2048 (logged)
        float         clearance;   // the free radius wanted around the point, > 0; clamped to [16, 512]
        float         maxDrop;     // how far the ground may sit below the origin's feet; 0 => 128; clamped to 1024
        std::uint32_t flags;       // SpaceFlags
        std::uint32_t reserved;    // MUST be 0
    };

    // FindEmptySpace output. EXACT LAYOUT (byte-shared): 24 bytes.
    struct APMF_SpaceResult {
        std::uint32_t size;        // the CALLER sets = sizeof(APMF_SpaceResult)
        std::uint32_t status;      // a QueryStatus (the same value the call returns)
        float         x;           // the ground point, valid only when status == kQuery_Ok
        float         y;
        float         z;
        std::uint32_t detail;      // the collision layer that stopped a ray, or the occupying actor's
                                   //   FormID (kQuery_Occupied); 0 otherwise
    };

    // FindHostilesInSpace input. EXACT LAYOUT (byte-shared): 24 bytes.
    struct APMF_HostileQuery {
        std::uint32_t size;        // = sizeof(APMF_HostileQuery)
        RE::FormID    side;        // REQUIRED. The loaded actor whose side we are on.
        float         x;           // the sphere's centre, in the side actor's cell/worldspace
        float         y;
        float         z;
        float         radius;      // > 0; clamped to 4096 (logged)
    };

    static_assert(sizeof(APMF_SpaceQuery) == 44,   "APMF_SpaceQuery is 44 bytes, byte-shared with clients");
    static_assert(offsetof(APMF_SpaceQuery, origin)   == 4,  "origin at +4");
    static_assert(offsetof(APMF_SpaceQuery, heading)  == 20, "heading at +20");
    static_assert(offsetof(APMF_SpaceQuery, flags)    == 36, "flags at +36");
    static_assert(sizeof(APMF_SpaceResult) == 24,  "APMF_SpaceResult is 24 bytes, byte-shared with clients");
    static_assert(offsetof(APMF_SpaceResult, x)       == 8,  "x at +8");
    static_assert(offsetof(APMF_SpaceResult, detail)  == 20, "detail at +20");
    static_assert(sizeof(APMF_HostileQuery) == 24, "APMF_HostileQuery is 24 bytes, byte-shared with clients");
    static_assert(offsetof(APMF_HostileQuery, radius) == 20, "radius at +20");

    // The most actors FindHostilesInSpace ever writes, whatever capacity the caller passes.
    inline constexpr std::uint32_t kMaxHostileResults = 64;

    // The v11 interface: APMF_API_v9's members verbatim (prefix EXTENSION; there is no
    // APMF_API_v10 struct because ABI v10 added no slot), then the two query slots.
    // This header is BYTE-SHARED with MFO: the declaration below is authoritative and
    // must be mirrored byte-identically on the client side.
    //
    // WHY A BUMP (INVARIANTS #14b): new function-pointer slots need an
    // `abiVersion >= 11` test before a client may call them.
    struct APMF_API_v11 : APMF_API_v9 {
        // Find a standable, empty point `q->distance` units from the origin along the
        // direction. Returns a QueryStatus and writes the same status (plus the point on
        // success) into `*out`. MAIN THREAD ONLY (see the section header).
        //   The walk: a ray at the origin's chest height (feet + 64u) out to the point,
        //   then a ray straight down from there to (feet - maxDrop). The ground it finds
        //   must be static, terrain or ground (the three layers the engine itself accepts
        //   for a Target Location placement); anything else is kQuery_NotGround, and an
        //   actor's capsule under the point is kQuery_Occupied. Then the clearance:
        //   eight level rays of `clearance` length at the point's knee height (+32u)
        //   must hit nothing, four ground rays at the clearance ring must land within 48u
        //   of the point's height, and no live actor may stand within `clearance` + 32u.
        //   Every failure names itself; nothing is retried or relaxed except by
        //   kSpaceFlag_ClampToWall. Cost: at most 14 ray casts plus one pass over the
        //   loaded ("high") actors.
        std::uint32_t (*FindEmptySpace)(const APMF_SpaceQuery* q, APMF_SpaceResult* out);

        // List the live actors inside the sphere that the engine considers HOSTILE to
        // `q->side`. The test is the engine's own `Actor::IsHostileToActor`, asked from
        // each candidate: candidate->IsHostileToActor(side). That is the same call
        // Papyrus Actor.IsHostileToActor makes. No faction list is kept here.
        //   Candidates: every loaded ("high") actor plus the player, in the side actor's
        //   worldspace (or, indoors, its cell), alive, enabled, not the side actor, with
        //   a 3D distance to the centre <= radius.
        //   Output: up to min(capacity, kMaxHostileResults) FormIDs, NEAREST FIRST.
        //   `*outCount` = how many were written. `outTotal` (may be null) = how many
        //   matched before the cap, so a caller can tell a full sphere from a truncated
        //   list. Returns a QueryStatus. MAIN THREAD ONLY.
        //   Cost: one pass over the high actors (one distance each), one engine
        //   hostility test per actor inside the sphere, and a sort of the matches.
        std::uint32_t (*FindHostilesInSpace)(const APMF_HostileQuery* q, RE::FormID* outActors,
                                             std::uint32_t capacity, std::uint32_t* outCount,
                                             std::uint32_t* outTotal);
    };

    // The v12 interface: APMF_API_v11's members verbatim (prefix EXTENSION), then ONE
    // appended slot: the travel leg-state read. This header is BYTE-SHARED with MFO: the
    // declaration below is authoritative and must be mirrored byte-identically on the
    // client side.
    //
    // WHY A BUMP (INVARIANTS #14b): a new function-pointer slot needs an
    // `abiVersion >= 12` test before a client may call it.
    struct APMF_API_v12 : APMF_API_v11 {
        // Read the ch.19 travel leg state of `actor` (see TravelLegState above). Returns the
        // state and, when `out` is non-null and `out->size` covers the v12 layout, fills
        // `*out` (it writes nothing into a shorter struct). `out` may be null for a
        // state-only read. The state describes the actor's leg, which is the leg of the
        // WINNING kIntent_Travel claim on it; a client whose claim is outranked sees the
        // owner's leg, so compare ownerHandle with your own handle.
        //   READ-ONLY and SAFE FROM ANY THREAD: APMF copies a small per-actor record under
        //   a mutex, the same "snapshot read" contract as IsClaimLive. It claims nothing,
        //   holds nothing and changes nothing. The state is updated on the game thread as
        //   the leg changes, so a read right after a Repoint may still show the previous
        //   leg's end for up to a frame (check ownerHandle, destForm and seq).
        //   The state is not saved: after a save load an actor reads kLeg_None until its
        //   claim is made again.
        //   A throw inside returns kLeg_None and writes nothing.
        std::uint32_t (*GetTravelLegState)(RE::FormID actor, APMF_TravelLegInfo* out);
    };

    // Function-pointer type for GetProcAddress(kGetInterfaceExport). Returns the
    // base type; a client that asked for ABI >= N checks p->abiVersion and casts up
    // to APMF_API_vN* (all revisions share v1's identical initial sequence).
    using GetInterface_t = const APMF_API_v1* (*)(std::uint32_t abiVersion);

}
