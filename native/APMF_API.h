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

    inline constexpr std::uint32_t kABIVersion = 6;

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
                               //   move-to-point destination. NOT YET READ by any channel:
                               //   an actor target rides kIntent_Cast's APMF_CastRequest
                               //   (`req.target`, load-bearing at the engine seats), and
                               //   `pos` stays documented-reserved until a rune/AoE pass
                               //   wires location aiming.
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

    // Function-pointer type for GetProcAddress(kGetInterfaceExport). Returns the
    // base type; a client that asked for ABI >= N checks p->abiVersion and casts up
    // to APMF_API_vN* (all revisions share v1's identical initial sequence).
    using GetInterface_t = const APMF_API_v1* (*)(std::uint32_t abiVersion);

}
