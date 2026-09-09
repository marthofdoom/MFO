#pragma once
// Actuation_internal.h -- the Actuation family's SHARED substrate. One TU
// (Actuation.cpp) used to hold all of this; two mechanical module splits
// (Actuation.cpp / Actuation_Direct.cpp, then Actuation_Hands.cpp on
// 2026-09-08) moved what crosses a TU boundary here as `inline` (ONE shared
// instance across the THREE TUs -- never per-TU copies): the cross-module
// concentration numbers (sustain windows, the cadence contract, the
// randomized stream time-cap draw), and the per-hand cast lock's shared
// state + cross-TU entry points. Everything else stays file-local in its
// module. NOT a public API: only the three Actuation*.cpp TUs may include
// this.

#include "PCH.h"
#include "Actuation.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "CasterConsent.h"
#include "Targeting.h"
#include "Logistics.h"
#include "Followers.h"   // #68: g_active -- NearestAlly walks the maintained teammate list
#include "Serialization.h" // T#76: FWPN record ids for the force-hold co-save
#include "MainThread.h"   // T#76: defer the load-time force-lock release to the main thread
#include <limits>        // #68: std::numeric_limits for NearestAlly's distance seed
#include "Packages.h"    // #35: act.flee reuses the retreat package; hybrid forced cast
#include "Sightline.h"   // LoS gate on the forced cast -- no firebolts into walls
#include "Temperament.h" // flair #1: per-follower timing seed (grace offset)
#include "Confidence.h"  // AUTO cast: ChaseRadius bounds the "nearby enemies" fan-out

#include <optional>      // ForceCast's tri-state return -- not in the PCH
#include <random>        // fix #3/#6: jittered beneficial-recast window (worker-serial RNG)

namespace MFO::Actuation {

        // These three are the per-beat SUSTAIN WINDOW (the duration
        // SustainConcentrationEffect pins on the live AE each ~1 s beat so it never
        // lapses between beats) -- NOT the stream's time cap. The STREAM CAP is a
        // randomized per-stream band (DrawConcCap below); keep each window > the
        // ~1-1.5 s beat gap so the AE bridges beats.
        inline constexpr float kConcHealCap     = 6.0f;   // heal sustain window (per beat)
        inline constexpr float kConcUtilityHold = 4.0f;   // non-self utility/ward sustain window
        inline constexpr float kConcSelfUtilityCap = 15.0f;   // self utility/ward sustain window
        // HEAL stop-at-full: a heal stream ends the moment the recipient is at/near
        // full Health (marth: "heal always to 100%"), with the random cap as backstop.
        // Uses the SAME mark as the re-dispatch condition (Vocab::kHealFull, 99.95%)
        // so the stream stop and the heal re-dispatch agree -- a topped-off target
        // both stops re-triggering AND has its live stream cut.
        inline constexpr float kHealFullPct = Vocab::kHealFull;

        // THE CADENCE CONTRACT (critical -- "heals feel broken" regression).
        // A concentration spell's cost is authored PER SECOND, and the ENGINE
        // channels its magnitude continuously through the sustained real effect
        // (SustainConcentrationEffect). The ~1 s beat is the stream's
        // heartbeat: each beat DEDUCTS one second's cost (CalculateMagickaCost
        // on a concentration spell returns the per-second cost, so the 1 s
        // beat is the authored drain) and RE-ARMS the sustained effect's
        // rolling window. Pacing the beat by fCastCooldown (default 4 s) would
        // under-charge the channel 4x and let the effect lapse between re-arms
        // (the original "heals feel broken" shape). Fire-and-forget spells
        // keep the fCastCooldown beat: their magnitude is per CAST, and a 1 s
        // beat would multiply it. Sticky concentration wards beat at 1 s too,
        // but the already-active guard in Apply{Self,Target}Effect keeps a
        // still-up ward from re-stacking.
        inline constexpr float kConcApplyPeriod = 1.0f;

        // ONE source of truth for the concentration STREAM TIME-CAP (marth: loose,
        // human timing -- each stream lasts a slightly different, RANDOMIZED duration
        // so channels never feel like a fixed constant). Drawn ONCE when a stream
        // starts (stored in the stream state, never serialized -> no save/determinism
        // concern), from a uniform band by nature:
        //   healing / utility / buff -> [8, 15] s
        //   offense / hostile        -> [2, 6] s
        // The cap GUARANTEES every concentration stream ENDS even if the exact gambit
        // condition check is unreliable ("won't stop when the condition is met"); the
        // gambit re-evaluates between bursts, so a full/satisfied target is not
        // re-served. Healing ALSO ends early at ~full recipient HP (kHealFullPct, in
        // the reconciles), with this cap as the backstop. Consumed by BOTH
        // TargetCastReconcile (non-self streams) and SelfCastReconcile (self streams),
        // so the two paths can never drift on the numbers.
        inline float DrawConcCap(CasterConsent::SpellKind a_kind) {
            static std::mt19937 rng{ std::random_device{}() };   // worker-serial, no lock (#4)
            const float lo = (a_kind == CasterConsent::SpellKind::Offense) ? 2.0f : 8.0f;
            const float hi = (a_kind == CasterConsent::SpellKind::Offense) ? 6.0f : 15.0f;
            return std::uniform_real_distribution<float>(lo, hi)(rng);
        }

        // ══ THE PER-HAND CAST LOCK: SHARED SUBSTRATE ════════════════════════════
        // Moved here VERBATIM from Actuation.cpp by the 2026-09-08 mechanical split
        // (Actuation.cpp stood at 3282 lines, over the project's 2500-line hard cap).
        // These are the entities the lock's IMPLEMENTATION (Actuation_Hands.cpp) and
        // its CALLERS (Actuation.cpp: Fire, CastOn, ConcentrationCast, ClearCastLock /
        // ClearCastLocks) both need, so they can no longer be file-local. `inline` ==
        // ONE instance across the three Actuation TUs, never a per-TU copy. Everything
        // used on only ONE side of the cut stayed file-local in its own module, in its
        // original position. No logic changed and nothing was reordered.

        // ── TASK 2 (feat/cast-gambit-concentration) / PER-HAND (feat/per-hand-
        // cast-slots, 2026-09-06): THE FIRING-SPELL GAMBIT LOCK ────────────────
        // marth: "while a spell gambit is actively firing, another spell gambit
        // must not preempt or re-point it, even if it would otherwise win the
        // rule evaluation" -- UNLESS the two spells would use DIFFERENT hands,
        // in which case both fire concurrently. Field-proven 2026-09-06: a
        // per-FOLLOWER lock (Task 2's original shape) serialised a heal and an
        // offense spell onto ONE hand while the follower's OTHER hand sat idle
        // -- deck log alternated HELD OFF between the two spells tick to tick
        // (whichever grabbed the lock first that tick kept it; the other lost
        // its own scan-fallthrough attempt) and suppressed ~14 of 36 wanted
        // casts in one capture. The parallel APMF change makes kIntent_Cast
        // arbitrate PER (actor, hand), so two concurrent claims -- LEFT and
        // RIGHT -- can now genuinely coexist on one follower; this lock is
        // rekeyed to match, one slot PER HAND, so a spell firing in one hand
        // never holds off a different spell that would use the other.
        //
        // Both the OWNED-CAST claim (APMF's engine seats mid-charge/mid-
        // decision on the offense or heal facet) and a CONCENTRATION stream
        // (claimed via Task 1, or the plain direct-force fallback) are
        // genuinely multi-tick: the round-robin combat scan can swing to a
        // DIFFERENT winning cast rule between services before either finishes,
        // and every one of those paths re-Claims/re-registers on ITS OWN
        // (spell,target) the instant it is asked to -- so a flip mid-cast on
        // the SAME hand silently tears down the in-flight charge/channel and
        // starts a new one, wasting it (the bug this closes).
        //
        // WHICH HAND does a given (spell,target) request occupy? Mirrors each
        // branch's own hand policy below exactly, so the lock's key always
        // matches what actually gets claimed/executed (see ResolveCastHand):
        //   * self-target, or ANY concentration spell (self or on-target --
        //     ConcentrationCast/CastSelfDirect/CastTargetDirect all claim LEFT
        //     unconditionally, heal or offense) -- forced LEFT.
        //   * a Heal/Buff spell reaching the composed-cast branch (or falling
        //     through to the legacy hybrid) -- forced LEFT, the heal-claim
        //     hard rule (APMFBridge::ClaimHealCast's own doc).
        //   * an Offense spell at a real (non-self) target -- the ONLY case
        //     with a real hand DECISION: Loadout::PlanCastHand's Left /
        //     DualCast / EitherFree, resolved against the CURRENT per-hand
        //     lock state -- "the juggle": EitherFree tries LEFT then RIGHT;
        //     DualCast needs BOTH hands free-or-refreshing at once, never a
        //     partial claim (a dual-cast that only half-lands is not a
        //     dual-cast, it is a corrupted single one -- CLAUDE.md principle 7,
        //     no fallback that makes a degrade look like the real thing). The
        //     hard, non-negotiable weapon-hand-exclusion rule is enforced
        //     UPSTREAM, inside PlanCastHand itself (a_weaponHandActive -> Left,
        //     unconditional, checked before Dual/EitherFree are even
        //     considered) -- this resolver only ever sees Right/Dual as
        //     candidates once PlanCastHand has ALREADY established the
        //     follower's right hand is genuinely free, so it cannot open a
        //     path to claiming a weapon hand.
        //
        // SCOPE (deliberate, not an oversight, unchanged from Task 2): CastAuto
        // is out of scope -- its own sequential-most-hurt hysteresis is a
        // DELIBERATE target-cycling design ([[cast-fanning-known-good-
        // behavior]]), not the re-pointing bug this lock exists for. The
        // out-of-combat Logistics dispatch never calls CastOn/ConcentrationCast
        // at all (it calls CastSelfDirect/CastTargetDirect/CastAuto directly,
        // single-pass, no suppression-window rule scan to re-point FROM), so it
        // needs no gate. The legacy AI-first-grace + force-on-miss hybrid does
        // not ITSELF hold this lock (it already self-protects via its own grace
        // window + the aiCastOther miss detector) but IS still gated by it, on
        // the same resolved hand as every other branch -- a live claim on one
        // hand still holds a legacy attempt at a DIFFERENT spell off that SAME
        // hand, while the other hand stays free.
        //
        // STATE: TWO slots per follower now (index 0 = left, 1 = right),
        // worker-serial (#4 discipline, same as g_forcedWeapon above) -- the
        // (spell,target) currently occupying EACH hand, and the last tick it
        // was reaffirmed. target == 0 means self.
        enum : std::size_t { kHandLeft = 0, kHandRight = 1, kHandCount = 2 };

        // ── RANK: THE RULE INDEX, CARRIED, NEVER INFERRED ───────────────────────
        // `kNoRule` sorts BELOW every real rule (lower index == higher priority),
        // so an unknown asker can never outrank an incumbent and an unowned hand is
        // never protected by rank alone.
        inline constexpr int kNoRule = std::numeric_limits<int>::max();

        // The rule index Fire() is currently acting for -- set at the top of Fire()
        // (Actuation.cpp, the ONE entry point into every actuation in this family)
        // and read by HoldCastLock and the preempt test, over in
        // Actuation_Hands.cpp. Worker-serial, no lock: the same #4 discipline as
        // g_castLock and the cast lock's other maps beside it in this header, and
        // for the same reason -- the per-follower Scheduler service is serialized
        // on the AddTask job worker, so there is never a second Fire() in flight.
        // NOT an anon-namespace variable any more (it was one until the 2026-09-08
        // split): it is `inline` at MFO::Actuation scope, so there is exactly ONE
        // instance shared by all three Actuation TUs, never a per-TU copy. Read
        // "no lock" as "one writer thread", never as "not shared".
        // Deliberately NOT threaded through six signatures: every reader is reached
        // only from Fire(), so a parameter would be the same value re-typed at each
        // hop with more places to get it wrong. Actuation_Direct.cpp's OOC callers
        // (Logistics -> CastSelfDirect/CastTargetDirect) live in another TU, never
        // touch the hand gate, and cannot see this at all.
        inline int g_firingRule = kNoRule;

        // THE FIRING RULE'S ALLY-HP THRESHOLD, or < 0 when its condition is not an
        // ally selector. Carried the same way and for the same reason as
        // g_firingRule: the incumbent-target validity test below has to ask the
        // question the EVALUATOR asked -- "is this ally still under the threshold
        // that made it a target?" -- and `Eval::Choice::conditionParam` is the only
        // place that number exists. Stored as a float rather than the opcode string
        // so Fire() copies no string per dispatch; the opcode is resolved to
        // "ally selector or not" once, at the single write site.
        inline float g_firingAllyThreshold = -1.0f;

        struct CastLock {
            RE::FormID spell  = 0;
            RE::FormID target = 0;
            std::chrono::steady_clock::time_point lastSeen{};
            // WHEN THE CLAIM BEHIND THIS HOLD WAS FIRST SEEN ABSENT (F8 review
            // fix, 2026-09-08). The in-flight protection's cap CANNOT be measured
            // from `lastSeen`: F9's in-flight path deliberately does NOT re-stamp
            // the lock (the live claim is its own proof of liveness), so
            // `lastSeen` freezes at the lap the rule FIRST claimed. Anchoring the
            // cap there made the protection DEAD for any claim older than the cap
            // -- a claim refreshed in flight for 20 s that then lapsed would fail
            // both the staleness test and the cap on the very next lap and hand
            // the charging spell straight to another rule, which is the exact
            // failure F8 exists to remove. Stamped instead at the moment the third
            // answer below is first reached, cleared whenever the claim is seen
            // live again, and cleared by HoldCastLock (a fresh hold is a fresh
            // cast, and its charge deserves a fresh window). Epoch == "not set".
            std::chrono::steady_clock::time_point claimGoneAt{};
            // WHICH GAMBIT OWNS THIS HAND -- the rule's INDEX in the follower's
            // combat table, stamped by HoldCastLock from g_firingRule (rank
            // preemption, marth 2026-09-08; corrected from a lap-arrival
            // INFERENCE after the Fable review of ed9cbbc).
            //
            // THE GAMBIT LIST ORDER IS THE PRIORITY ORDER (marth: "Thats
            // intentional ... gambits are already configured correctly with heals
            // near the top that should overwrite the offense claim"), so rank is a
            // number the evaluator already knows and this simply CARRIES it. The
            // first version tried to READ rank off the scan instead -- "did this
            // hand's incumbent assert itself earlier in this same lap?" -- and that
            // was wrong in two ways the review found. (a) An incumbent whose rule
            // exits transparently BEFORE the hand gate never asserts at all: the
            // #68 out-of-range exit, the magicka cost / reserve exits and the
            // HasSpell gate all sit above it, so two rules whose conditions are
            // both true could take the hand from each other on alternate laps and
            // neither would ever reach a charge. (b) A rule RETARGETING its own
            // cast fails the incumbent match on `target` while its own stamp is
            // last lap's, so it read as outranking ITSELF. A real index comparison
            // cannot be fooled by either, and it needs no tenure window -- which
            // matters, because a tenure window would put a delay back in front of
            // exactly the heal marth's ruling says must take the hand at once.
            //
            // LOWER INDEX == HIGHER PRIORITY. kNoRule when nothing owns the hand.
            //
            // BOARD EDITS: an index is positional, so reordering the gambit list
            // mid-combat can leave this naming a different rule until the claim
            // ends (the suppression window has the same exposure and re-resolves a
            // uid for it). Bounded by the claim's own TTL and one hand, and
            // self-correcting on the next claim; not worth a second identity here.
            int owningRule = kNoRule;
        };
        struct FollowerCastLocks { CastLock hand[kHandCount]; };
        inline std::unordered_map<RE::FormID, FollowerCastLocks> g_castLock;

        // Rate-limited [eval] log -- one line per (follower, hand, spell) per
        // ~2s window, the SAME dedup shape as CasterConsent.cpp's
        // g_lastDenied/g_lastConcDeny, so a rule held off every tick it keeps
        // losing does not spam the log at scan rate. Per-hand now too, so a
        // busy left hand and a busy right hand each get their own dedup entry.
        struct FollowerLockLog {
            std::pair<RE::FormID, std::chrono::steady_clock::time_point> hand[kHandCount];
        };
        inline std::unordered_map<RE::FormID, FollowerLockLog> g_lastLockLog;

        // ── THE IN-FLIGHT REFRESH LINE (F9, 2026-09-08) ─────────────────────────
        // Principle 5: a mechanism that silently changes which rules get a lap
        // must be visible, or the next field log is read against the old model.
        // ONE line the first time a rule goes satisfied-in-flight, then at most
        // one per 2 s for as long as it stays there -- the SAME per-(follower,
        // hand, spell) dedup shape and window as LogCastLockHold above, and its
        // OWN map, so an in-flight refresh and a hold-off on the same hand cannot
        // silence each other. Rate matters here: without a window this fires on
        // every round-robin lap of a multi-second cast (~7.5 Hz solo).
        inline std::unordered_map<RE::FormID, FollowerLockLog> g_lastInFlightLog;

        // Rate-limited [eval] line -- same per-(follower, hand, spell) 2 s dedup as
        // LogCastLockHold, own map so a preemption and a hold-off on the same hand
        // cannot silence each other. Preemption is the event that explains why a
        // cast stopped, so it must be legible in the log it appears in.
        inline std::unordered_map<RE::FormID, FollowerLockLog> g_lastPreemptLog;

        // Which hand(s) a (spell,target) request would occupy if it proceeds.
        // Both false never happens on a non-held return (ResolveCastHand always
        // sets at least one before returning std::nullopt).
        // `inFlight` (F9, 2026-09-08): EVERY hand this plan occupies is already
        // locked to this exact (spell,target) AND MFO holds a live cast claim
        // there -- i.e. the rule asking is the incumbent and its cast is already
        // running. The caller must then REFRESH that claim and fall through to
        // the rules below instead of re-claiming and re-firing; see CastOn's
        // in-flight gate for why that is the whole of RC2.
        // `preemptLeft`/`preemptRight` (review finding, 2026-09-08): this plan is
        // only possible if the named hand's incumbent claim is DISPLACED first --
        // recorded here rather than done on the spot, because ResolveCastHand runs
        // well before the asker's own claim call and the asker can still fail
        // transparently in between (an unaffordable self-cast, an APMF refusal, a
        // heal held off by ComposedCast, a Prepare that fails). Committing at
        // resolve time turned every such failure into a hand nobody holds, at lap
        // rate: the incumbent was already released and its lock zeroed, so a rule
        // that deterministically fails killed a working one below it. CastOn
        // commits this immediately before the claim it is for, and not one line
        // earlier -- see CommitPreempt at the call site.
        struct HandPlan {
            bool left = false; bool right = false; bool inFlight = false;
            bool preemptLeft = false; bool preemptRight = false;
        };

        // ── T#76 FORCE-HOLD STATE: DECLARED here, DEFINED in Actuation.cpp ───────
        // g_forcedWeapon / g_forcedMx ALREADY had external linkage (namespace scope,
        // never the anon namespace) and their definitions stay exactly where they were,
        // with their full comment; only these two declarations are new, so
        // Actuation_Hands.cpp's WeaponHandExposure can still read the map across the
        // cut. *** EVERY ACCESS TAKES g_forcedMx *** -- the map has an OFF-THREAD
        // reader (the SKSE save callback, CoSaveForcedWeapons). See the definition.
        extern std::unordered_map<RE::FormID, RE::TESBoundObject*> g_forcedWeapon;
        extern std::mutex g_forcedMx;

        // ── THE LOCK'S CROSS-TU ENTRY POINTS ────────────────────────────────────
        // Defined in Actuation_Hands.cpp, each still carrying the doc comment it has
        // always had -- read it THERE, it was not copied here. These six were
        // file-local while one TU held everything; the split gives them external
        // linkage (unchanged bodies, unchanged callers, unchanged order) because
        // CastOn / ConcentrationCast in Actuation.cpp call them across the cut. NOT a
        // public API -- Actuation.h is that; nothing outside the Actuation TUs may
        // call these.
        void LogCastInFlight(RE::FormID a_follower, std::size_t a_hand, RE::FormID a_spell,
                             bool a_dual);
        void HoldCastLock(RE::FormID a_follower, std::size_t a_hand,
                          RE::FormID a_spell, RE::FormID a_target);
        void ClearCastLockHand(RE::FormID a_follower, std::size_t a_hand);
        RE::FormID CastProxyOnHand(RE::FormID a_follower, std::size_t a_hand);
        void PreemptHand(RE::Actor* a_follower, std::size_t a_hand, RE::FormID a_wantedSpell);
        std::optional<Outcome> ResolveCastHand(RE::Actor* a_follower, Loadout::HandPick a_pick,
                                               RE::FormID a_spell, RE::FormID a_target,
                                               HandPlan& a_out);

}
