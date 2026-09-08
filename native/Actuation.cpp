// Actuation.cpp -- the combat-rule ACTUATION dispatch (split mechanically,
// no logic change; the direct-delivery streams + apply substrate live in
// Actuation_Direct.cpp): Fire and its verbs, ForceCast, the bounded
// concentration stream entry (ConcentrationCast), CastOn, EquipWeapon +
// the T#76 force-hold bookkeeping and its FWPN co-save, NearestAlly and
// ResolveCastTarget. Shared concentration numbers: Actuation_internal.h.
#include "Actuation_internal.h"
#include "APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps

namespace MFO::Actuation {

    // T#76: the weapon a follower is being force-held on, keyed by FormID. Written
    // by EquipWeapon (the fire) and cleared by ReleaseForcedWeapon/Reconcile/
    // ClearForcedWeapons -- ALL on the job-worker serial tick, the same thread
    // the scheduler already drives Fire on, so no lock (the g_followers
    // discipline: #4). Persists ACROSS ticks by design -- it is engine-hold
    // bookkeeping like CombatStyle's owned-stance map, not per-scan evaluator
    // state (#22), and is never read back by the evaluator. Session-scoped:
    // cleared on revert (ClearForcedWeapons). The pointer is a weapon form, which
    // is stable for the session.
    std::unordered_map<RE::FormID, RE::TESBoundObject*> g_forcedWeapon;
    // SEV-1 (Fable 2026-08-17): the worker tick mutates this map, but the SKSE
    // SAVE callback reads it (CoSaveForcedWeapons) off that thread with no pump
    // barrier -- a concurrent insert would invalidate its iterator mid-save
    // (CTD / truncated save). Guard every access; NEVER hold it across an
    // Equip/UnequipObject engine call (the MSTK "copy then act" discipline).
    std::mutex g_forcedMx;

    namespace {

        // #68: the nearest living player-teammate that is not a_follower
        // himself. Walks the maintained g_active list (not a world sweep) --
        // same precedent as Evaluator.cpp's PickAlly, just by DISTANCE
        // instead of lowest HP, because "nearest ally" as a cast SUBJECT is
        // a positional pick, not a triage one.
        RE::Actor* NearestAlly(RE::Actor* a_follower) {
            if (!a_follower) return nullptr;
            const auto selfPos = a_follower->GetPosition();
            RE::Actor* best = nullptr;
            float bestDist = std::numeric_limits<float>::max();
            for (const auto& h : Followers::g_active) {
                auto ptr = h.get();   // HOLD the NiPointer (Targeting rule)
                auto* ally = ptr.get();
                if (!ally || ally == a_follower) continue;
                if (ally->IsDead() || !ally->Is3DLoaded()) continue;
                const float d = selfPos.GetDistance(ally->GetPosition());
                if (d < bestDist) { bestDist = d; best = ally; }
            }
            return best;
        }

        // ResolveCastTarget moved OUT of this anon namespace to public
        // MFO::Actuation scope (just below, after this block closes) so the
        // logistics OOC cast_target path (Logistics.cpp) can share the exact
        // same resolution ladder as the combat Fire path. It still calls
        // NearestAlly (anon, above) -- an internal-linkage helper is visible
        // throughout this TU.

        // THE HYBRID'S FORCE HALF. The AI-first grace (below, in CastOn) stays
        // the preferred path because it is MOBILE -- the follower strafes and
        // closes while casting, which no package cast does (§0.27 roots him).
        // But when the AI MISSED -- it cast a DIFFERENT spell during the grace
        // (Marcurio's Chain-Lightning-over-Firebolt, the swap that also resets
        // the grace clock forever), or the grace elapsed with no cast of ours
        // -- the configured spell is FORCED through the proven-but-unwired
        // package route (§0.21-0.23: animated, targeted, every axis proven).
        //
        // Returns nullopt when the package route is unavailable/declined
        // structurally, so CastOn falls through to the legacy silent apply --
        // never a regression, exactly the old grace-expiry behaviour.
        std::optional<Outcome> ForceCast(RE::Actor* a_follower, RE::SpellItem* a_spell,
                                         RE::Actor* a_target, bool a_aiCastOther) {
            if (!Config::g_forceCastOnMiss.load()) return std::nullopt;
            if (!Packages::Available())            return std::nullopt;

            const auto id   = a_follower->GetFormID();
            const bool self = !a_target || a_target == a_follower;

            // LINE OF SIGHT, required for the forced shot only. The AI path
            // repositions on its own; a package cast fires from where he
            // stands, so an OCCLUDED verdict holds the force and lets the
            // rules below (attack -> the AI closes/repositions) run instead.
            // TRANSPARENT: a wall between him and the foe must not wall off
            // the rest of his list too. Unknown passes -- fail-open, so a
            // runtime where the raycast cannot run (VR: no main-thread pump)
            // degrades to today's behaviour instead of an inert forced cast.
            if (!self) {
                const auto v = Sightline::Check(id, a_target->GetFormID());
                if (v == Sightline::Verdict::Occluded) {
                    spdlog::debug("[cast] {:08X}: forced cast HELD -- no line of sight to {:08X}",
                                  id, a_target->GetFormID());
                    return Outcome{ Result::NoOp, "forced cast held (no line of sight)", true };
                }
            }

            const auto d = self ? Packages::CastSelf(a_follower, a_spell)
                                : Packages::CastAt(a_follower, a_spell, a_target);
            if (d == Packages::Decline::None) {
                // The decision line the deck log needs: WHICH miss branch fired.
                spdlog::info("[cast] {:08X} {} FORCED {} ({:08X}) {} -- {} (LoS {})",
                             id, a_follower->GetName() ? a_follower->GetName() : "?",
                             a_spell->GetName() ? a_spell->GetName() : "?",
                             a_spell->GetFormID(),
                             self ? std::string("on self")
                                  : std::format("at {:08X}", a_target->GetFormID()),
                             a_aiCastOther ? "their AI cast a DIFFERENT spell"
                                           : "grace elapsed, no cast",
                             self ? "n/a"
                                  : Sightline::VerdictName(
                                        Sightline::Check(id, a_target->GetFormID())));
                // The package carries the spell itself, but the latch stays
                // (v1.0.30): clearing consent here re-opened the same
                // between-casts gap the sink's Clear did -- until the next
                // service re-Want()ed, the AI could answer the forced cast
                // with a cast of its OWN. NoteOurCast retires the miss flag
                // (it was just consumed -- kept, it would short-circuit the
                // re-armed grace below into an instant re-force) and keeps
                // the deny standing. Give the hand back on the cooldown, and
                // re-arm the grace so the NEXT firing of this rule offers the
                // AI a fresh window.
                CasterConsent::NoteOurCast(id);
                Loadout::ArmGrace(id);
                Loadout::StartCooldown(id);
                return Outcome{ Result::Fired,
                                a_aiCastOther ? "forced cast (AI cast its own spell)"
                                              : "forced cast (grace elapsed)" };
            }
            if (d == Packages::Decline::Busy) {
                // Another follower holds the single cast alias -- a seconds-
                // long wait during which this one FIGHTS; transparent so the
                // rules below run (same shape as the loadout debounce).
                return Outcome{ Result::NoOp, "cast package busy", true };
            }
            // Contention / no record / quest stopped: structural. Fall through
            // to the silent path rather than dropping the rule -- the decline
            // was already logged with its reason by Packages.
            return std::nullopt;
        }

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

        inline const char* HandName(std::size_t a_hand) { return a_hand == kHandLeft ? "left" : "right"; }

        struct CastLock {
            RE::FormID spell  = 0;
            RE::FormID target = 0;
            std::chrono::steady_clock::time_point lastSeen{};
        };
        struct FollowerCastLocks { CastLock hand[kHandCount]; };
        std::unordered_map<RE::FormID, FollowerCastLocks> g_castLock;

        // Rate-limited [eval] log -- one line per (follower, hand, spell) per
        // ~2s window, the SAME dedup shape as CasterConsent.cpp's
        // g_lastDenied/g_lastConcDeny, so a rule held off every tick it keeps
        // losing does not spam the log at scan rate. Per-hand now too, so a
        // busy left hand and a busy right hand each get their own dedup entry.
        struct FollowerLockLog {
            std::pair<RE::FormID, std::chrono::steady_clock::time_point> hand[kHandCount];
        };
        std::unordered_map<RE::FormID, FollowerLockLog> g_lastLockLog;

        void LogCastLockHold(RE::FormID a_follower, std::size_t a_hand,
                             RE::FormID a_wantedSpell, RE::FormID a_lockedSpell) {
            const auto now = std::chrono::steady_clock::now();
            auto& entry = g_lastLockLog[a_follower].hand[a_hand];
            if (entry.first == a_wantedSpell &&
                std::chrono::duration<float>(now - entry.second).count() < 2.0f)
                return;
            entry.first = a_wantedSpell; entry.second = now;
            spdlog::info("[eval] {:08X} cast gambit HELD OFF -- spell {:08X} wants the "
                         "follower's {} hand, spell {:08X} is still firing there",
                         a_follower, a_wantedSpell, HandName(a_hand), a_lockedSpell);
        }

        // ── APMF REFUSAL: the ONE trace a cast that did NOT happen leaves ─────────
        // marth 2026-09-07 (verbatim): "without APMF, it needs to just work,
        // whatever unpolished way works. With APMF theres no fallback for it.
        // APMF shoudl work". So a claim APMF is CAPABLE of granting and refuses
        // anyway is a BUG IN APMF, not a condition to degrade through: the cast
        // does not happen, nothing routes around it, and this line is the whole
        // diagnosis (CLAUDE.md principle 7 -- an unmasked failure diagnoses in
        // ONE field cycle; a masked one hides indefinitely).
        //
        // spdlog::error, not info: this is a contract breach, not a decision.
        // Rate-limited to one line per (follower, spell, target) per ~5 s
        // (principle 8: a rule that keeps losing is loud by construction and must
        // not bury the log at the 133 ms service cadence). The key deliberately
        // omits the intent string: the intents that can collide on ONE
        // (follower, spell, target) triple are mutually exclusive by construction
        // (offense vs heal split on CasterConsent::ClassifySpell; the
        // self-concentration stream is the only one with target == 0), so no
        // distinct refusal is ever swallowed.
        //
        // ONE ENTRY PER KEY, NOT ONE SLOT PER FOLLOWER (F2-2, deploy-gate review
        // 2026-09-07). This held a single last-value slot, the same shape as
        // LogCastLockHold above -- and that shape is WRONG here for a reason that
        // does not apply there. Failing closed is TRANSPARENT, so the rule scan
        // does not stop at the first refusal: a follower with two refused cast
        // rules reaches BOTH every tick, each call overwrote the other's key, the
        // 5 s window never matched, and the throttle degraded to 2 error lines
        // per 133 ms tick -- the exact log burial the throttle exists to prevent.
        // A small per-follower vector keyed on the full (spell, target) pair
        // fixes it. Expired entries are swept on every touch and the vector is
        // capped, so it cannot grow: at the cap the OLDEST entry is reused, which
        // degrades toward MORE logging, never less (a silent diagnostic is the
        // failure mode that must not happen). Worker-serial, no lock (#4), same
        // as every other map in this anon namespace.
        constexpr float       kApmfRefusalEverySec = 5.0f;
        constexpr std::size_t kApmfRefusalMaxKeys  = 8;   // >> the realistic cast-rule count

        struct ApmfRefusalLog {
            RE::FormID spell  = 0;
            RE::FormID target = 0;
            std::chrono::steady_clock::time_point when{};
        };
        std::unordered_map<RE::FormID, std::vector<ApmfRefusalLog>> g_lastApmfRefusal;

        // True = this exact (follower, spell, target) was already logged inside the
        // window, so the caller stays quiet. Otherwise stamps it and returns false.
        bool ApmfRefusalThrottled(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                                  std::chrono::steady_clock::time_point a_now) {
            auto& keys = g_lastApmfRefusal[a_follower];
            // Sweep expired entries first -- they carry no information and this is
            // what keeps the vector small without an unbounded-growth guard.
            std::erase_if(keys, [&](const ApmfRefusalLog& e) {
                return std::chrono::duration<float>(a_now - e.when).count() >= kApmfRefusalEverySec;
            });
            for (auto& e : keys) {
                if (e.spell == a_spell && e.target == a_target) return true;   // still inside the window
            }
            if (keys.size() >= kApmfRefusalMaxKeys) {
                // Every key is live and we are at the cap: reuse the oldest slot.
                auto oldest = std::min_element(keys.begin(), keys.end(),
                                               [](const ApmfRefusalLog& l, const ApmfRefusalLog& r) {
                                                   return l.when < r.when;
                                               });
                *oldest = ApmfRefusalLog{ a_spell, a_target, a_now };
            } else {
                keys.push_back(ApmfRefusalLog{ a_spell, a_target, a_now });
            }
            return false;
        }

        void LogApmfRefusal(RE::FormID a_follower, const char* a_what, RE::FormID a_spell,
                            RE::FormID a_target, const char* a_hand) {
            if (ApmfRefusalThrottled(a_follower, a_spell, a_target,
                                     std::chrono::steady_clock::now()))
                return;
            spdlog::error("[apmf] {:08X} APMF REFUSED the {} claim -- spell {:08X}, target {:08X}, "
                          "{} hand. APMF is the COMMITTED route: NOT falling back to the legacy "
                          "hybrid, this cast does NOT happen this tick. Fix the refusal in APMF.",
                          a_follower, a_what, a_spell, a_target, a_hand);
        }

        // Establish/refresh ONE hand's lock -- call whenever CastOn/
        // ConcentrationCast commits to an outcome that occupies THAT hand
        // ACROSS ticks: a live APMF cast claim (offense or heal) on it, or a
        // genuine concentration stream (claimed via Task 1, or plain
        // direct-force) using it. a_target == 0 for self.
        void HoldCastLock(RE::FormID a_follower, std::size_t a_hand,
                          RE::FormID a_spell, RE::FormID a_target) {
            auto& lock = g_castLock[a_follower].hand[a_hand];
            lock.spell = a_spell; lock.target = a_target;
            lock.lastSeen = std::chrono::steady_clock::now();
        }

        // Is hand `a_hand`'s lock still LIVE, i.e. is the follower still
        // genuinely occupied on THAT hand right now? "release on completion,
        // on claim release/TTL expiry" (marth): a live APMF cast claim on that
        // SPECIFIC hand is authoritative proof by itself, checked FIRST so a
        // claim that ends EARLY (a heal topping off well inside the window, or
        // APMF's own TTL elapsing) frees the hand immediately rather than
        // riding out a timer. Heals are LEFT always (APMFBridge::
        // ClaimHealCast's hard rule), so a live heal claim only ever proves the
        // LEFT hand busy. Absent a live claim (APMF off/absent, or a plain
        // un-claimed direct-force concentration stream -- its registry is
        // private to Actuation_Direct.cpp's own TU and not queryable from
        // here), fall back to the SAME round-robin-aware staleness window
        // every other facet in this codebase already sizes against (#9: a
        // floor is safe, a guessed budget is not) -- reused via APMFBridge::
        // FacetExpiry(), never separately invented. A still-winning gambit
        // re-fires (and re-Holds its hand's lock) every round-robin lap, well
        // inside this window, so the SAME gambit never sees its own lock go
        // stale; only a gambit that stopped being requested at all does.
        bool CastLockLive(RE::FormID a_follower, std::size_t a_hand, const CastLock& a_lock) {
            const bool claimLive = (a_hand == kHandLeft)
                ? (APMFBridge::IsOwnedCastActiveOnHand(a_follower, APMFBridge::kApmfHandLeft) ||
                   APMFBridge::IsHealCastActive(a_follower))
                : APMFBridge::IsOwnedCastActiveOnHand(a_follower, APMFBridge::kApmfHandRight);
            if (claimLive) return true;
            const float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - a_lock.lastSeen).count();
            return elapsed <= std::chrono::duration<float>(APMFBridge::FacetExpiry()).count();
        }

        // Is hand `a_hand` available for (a_spell,a_target) right now -- i.e.
        // unlocked, already locked to this EXACT (spell,target) (the SAME
        // gambit refreshing itself), or its lock has gone live-false/stale
        // (dropped right here, in which case it is free)? a_busyOut is set to
        // whichever DIFFERENT spell currently holds it when this returns false.
        bool HandFree(RE::FormID a_follower, std::size_t a_hand, RE::FormID a_spell,
                     RE::FormID a_target, RE::FormID& a_busyOut) {
            auto it = g_castLock.find(a_follower);
            if (it == g_castLock.end()) return true;
            auto& lock = it->second.hand[a_hand];
            if (lock.spell == 0) return true;
            if (lock.spell == a_spell && lock.target == a_target) return true;
            if (!CastLockLive(a_follower, a_hand, lock)) { lock = CastLock{}; return true; }
            a_busyOut = lock.spell;
            return false;
        }

        // Which hand(s) a (spell,target) request would occupy if it proceeds.
        // Both false never happens on a non-held return (ResolveCastHand always
        // sets at least one before returning std::nullopt).
        struct HandPlan { bool left = false; bool right = false; };

        // THE GATE + THE JUGGLE. Resolves a_pick (a real Loadout::HandPick for
        // an offense spell, or plain Loadout::HandPick::Left for the self/
        // concentration/heal callers that never consult PlanCastHand at all)
        // against the CURRENT per-hand lock state, either returning the
        // resolved HandPlan (request may proceed -- caller HoldCastLock's
        // every hand HandPlan sets, on success) or a transparent HELD-OFF
        // Outcome naming the busy hand(s) (GAMBIT_FLOWS §2 -- a held gambit is
        // not a wall for the rules below it).
        std::optional<Outcome> ResolveCastHand(RE::FormID a_follower, Loadout::HandPick a_pick,
                                               RE::FormID a_spell, RE::FormID a_target,
                                               HandPlan& a_out) {
            RE::FormID busy = 0;
            switch (a_pick) {
            case Loadout::HandPick::DualCast: {
                RE::FormID busyL = 0, busyR = 0;
                const bool leftFree  = HandFree(a_follower, kHandLeft,  a_spell, a_target, busyL);
                const bool rightFree = HandFree(a_follower, kHandRight, a_spell, a_target, busyR);
                if (leftFree && rightFree) { a_out = { true, true }; return std::nullopt; }
                const auto hand      = !leftFree ? kHandLeft : kHandRight;
                const auto busySpell = !leftFree ? busyL : busyR;
                LogCastLockHold(a_follower, hand, a_spell, busySpell);
                return Outcome{ Result::NoOp,
                    std::format("cast gambit locked -- dual-cast needs both hands, spell {:08X} "
                                "still firing ({} hand)", busySpell, HandName(hand)), true };
            }
            case Loadout::HandPick::EitherFree: {
                RE::FormID busyL = 0, busyR = 0;
                if (HandFree(a_follower, kHandLeft, a_spell, a_target, busyL)) {
                    a_out = { true, false }; return std::nullopt;
                }
                if (HandFree(a_follower, kHandRight, a_spell, a_target, busyR)) {
                    a_out = { false, true }; return std::nullopt;
                }
                LogCastLockHold(a_follower, kHandLeft, a_spell, busyL);
                return Outcome{ Result::NoOp,
                    std::format("cast gambit locked -- both hands busy (left: spell {:08X}, "
                                "right: spell {:08X})", busyL, busyR), true };
            }
            case Loadout::HandPick::Left:
            default:
                if (HandFree(a_follower, kHandLeft, a_spell, a_target, busy)) {
                    a_out = { true, false }; return std::nullopt;
                }
                LogCastLockHold(a_follower, kHandLeft, a_spell, busy);
                return Outcome{ Result::NoOp,
                    std::format("cast gambit locked -- spell {:08X} still firing (left hand)",
                                busy), true };
            }
        }

        // ── CONCENTRATION: the BOUNDED DIRECT-FORCE STREAM ───────────────────
        // A concentration spell has no "one cast" for the fire-and-forget
        // machinery to observe: force-YESing it to the AI made a PERMANENT
        // held stream (Lucien's Flames through Xelzaz -- the v1.0.53 freeze),
        // and skipping it was rejected -- exact bounding must cover EVERY
        // spell class, no AI escape hatch (marth). So MFO streams it ITSELF.
        // DELIVERY IS THE DIRECT FORCE (CastSelfDirect / CastTargetDirect =
        // CastSpellImmediate straight onto the target + hand magicka deduct),
        // NEVER the AI package: the package route (v1.0.58-65, Packages::
        // CastAt + CastHold) §4.6-DECLINED every tick for a package-locked
        // custom follower (Lucien 2F00591F -- his own quest owns the package
        // alias at prio 80, MFO claims at 60), which with the consent denies
        // below meant a TOTAL cast lockout. The bound is stated UP FRONT and
        // enforced by Self/TargetCastReconcile each tick:
        //   hostile  -- 1-4 s by temperament (each mage's breath is
        //               consistently their own), LoS + line-of-fire re-checked
        //               on EVERY apply (the ffWatch analog);
        //   heal     -- until the target tops off, capped;
        //   utility  -- a capped window, ward dispelled on release.
        // Bounding is RELEASE + RE-STREAM while the rule keeps winning, never
        // a stop. The AI-first grace is deliberately NOT offered here: the
        // CheckStartCast/CheckCast hooks deny the AI's own attempt at a wanted
        // concentration spell (an AI channel cannot be bounded), so MFO's
        // direct stream is the ONLY open channel -- and it always delivers,
        // because a direct apply passes through neither hook (see
        // ConcentrationCast below). Every exit is bounded: an applied beat, a
        // transparent pace-out, or a legible failure (§5.3) -- never "leave it
        // to their AI".

        Outcome ConcentrationCast(RE::Actor* a_follower, RE::SpellItem* a_spell,
                                  RE::Actor* a_target) {
            const auto id   = a_follower->GetFormID();
            const bool self = !a_target || a_target == a_follower;

            // SELF concentration: routed to the UNIVERSAL direct trigger, not the
            // package (SPEC-self-cast-forced). In practice CastOn intercepts self
            // BEFORE this fork, so this is defence-in-depth -- but it must never
            // fall to the inert alias package, which equips but never fires and
            // is declined outright on package-locked custom followers.
            if (self) {
                // F3 tri-state (mirrors Logistics.cpp's OOC cast dispatch,
                // ~`:1500-1560`, + CastOn's self fork -- the old cite here pointed at
                // Logistics.cpp:3987-3996, which has not existed since that file was
                // split; it is 2065 lines): only a real Applied is this tick's action;
                // a Refreshed
                // (channel paced, nothing applied) is TRANSPARENT so it does not
                // starve the rules below it, and the fired log never lies.
                if (Config::g_castSelf.load()) {
                    switch (CastSelfDirect(a_follower, a_spell)) {
                    case SelfCast::Applied:
                        HoldCastLock(id, kHandLeft, a_spell->GetFormID(), 0);   // TASK 2
                        return { Result::Fired, "self-cast (direct trigger)" };
                    case SelfCast::Refreshed:
                        HoldCastLock(id, kHandLeft, a_spell->GetFormID(), 0);   // TASK 2
                        return { Result::NoOp, "self-cast refresh (paced)", true };
                    // Fable SEV-2 (2026-09-06): a HELD outcome is not a cast.
                    // ComposedCast::Try held this spell off because a DIFFERENT
                    // spell's live claim owns the follower's single heal slot, so
                    // nothing was claimed and nothing was applied. It used to
                    // arrive as Applied (the hold rode home inside Try()'s bare
                    // `true`), which reported a fired cast that never happened AND
                    // re-stamped the Task-2 hand lock with the HELD-OFF spell --
                    // on any lap where no lock was live yet (fresh after
                    // ClearCastLock, the Scheduler's no-cast-rule path, an
                    // OOC->combat transition) that re-label starved BOTH rules
                    // until a TTL. Transparent NoOp, its own label, NO lock.
                    case SelfCast::Held:
                        return { Result::NoOp, "self-cast held off (another heal owns the claim)", true };
                    case SelfCast::Declined:
                    default:
                        break;
                    }
                }
                return { Result::FailedOther, "self-cast could not fire", true };
            }

            // ── DIRECT FORCE is THE delivery, combat included (marth 2026-08-18:
            // "always use the known working force; avoid the package route") ──
            // v1.0.58-65 delivered combat concentration through the AI PACKAGE
            // (Packages::CastAt + CastHold). For a package-locked custom
            // follower (Lucien 2F00591F: his own quest owns the package alias
            // at prio 80, MFO claims at 60) the engine §4.6-DECLINED that claim
            // EVERY tick -- and because the consent latch denies the AI's own
            // attempt at a wanted concentration spell (an AI channel cannot be
            // bounded), the dead package was "the ONLY open channel": a TOTAL
            // combat cast lockout. CastTargetDirect (CastSpellImmediate straight
            // onto the target + hand magicka deduct) beats BOTH halves of that
            // trap:
            //   * no package -> no §4.6 alias arbitration to lose;
            //   * no AI deliberation -> the CheckStartCast (0x06, combat-AI
            //     advisory) and CheckCast (0x0A, pre-charge) consent hooks
            //     never see it. Both sit on the AI's own casting pipeline
            //     (RequestCastImpl -> ... -> FinishCastImpl, whose precondition
            //     is CheckCast -- ENGINE_NOTES §0.13/§0.14); CastSpellImmediate
            //     skips that state machine entirely (the same reason it never
            //     animates). Field proof, deck 2026-08-18: Lucien's AI cast of
            //     Healing (0005AD5C) is "[consent] HARD-ABORTED" while MFO's
            //     own direct SELF-CAST applies land on the same actor, same
            //     minute, hooks live.
            // So the consent hooks may keep DENYING the follower's own AI
            // stream while MFO's direct stream delivers -- coherent, never a
            // lockout. The package delivery is REMOVED here, not demoted: two
            // live delivery paths split the cadence/magicka semantics (the
            // package stream spends no magicka and ROOTS the caster mid-fight,
            // and its ~per-hold beat cannot meet the kConcApplyPeriod heal
            // contract), and the animation it bought is deferred anyway. The
            // old bForceCastOnMiss+bUsePackages gate went with it -- the direct
            // force needs neither (OOC already ships without them), so combat
            // and OOC concentration now run the IDENTICAL delivery. No
            // Loadout::CoolingDown gate / StartCooldown either: the channel
            // self-paces (kConcApplyPeriod / fCastCooldown by kind), and a 4 s
            // cooldown hole would let TargetCastReconcile's ~2 s stale window
            // tear down a still-winning stream. LoS + line-of-fire for hostile
            // offense are re-checked on EVERY apply inside CastTargetDirect
            // (the ffWatch analog); bounding lives in TargetCastReconcile
            // (hostile 1-4 s Temperament, heal 6 s, utility 4 s + ward dispel)
            // -- release + re-stream while the rule wins, never a stop.
            switch (CastTargetDirect(a_follower, a_spell, a_target)) {
            case SelfCast::Applied:
                // Exclusive control while the rule governs: keep the consent
                // latch armed so the slider denies COMPETING AI spells and the
                // concentration deny keeps the AI's own unbounded attempt at
                // this spell off. The direct stream is unaffected (above).
                CasterConsent::Want(id, a_spell->GetFormID());
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::Fired, "concentration (direct force)" };
            case SelfCast::Refreshed:
                // Live stream, paced out this tick -- the wait IS the action;
                // transparent like the FF form.
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::NoOp, "concentration direct refresh (paced)", true };
            // Fable SEV-2 (2026-09-06): a HELD outcome is not a cast.
            // ComposedCast::Try held this spell off because a DIFFERENT
            // spell's live claim owns the follower's single heal slot, so
            // nothing was claimed and nothing was applied. It used to
            // arrive as Applied (the hold rode home inside Try()'s bare
            // `true`), which reported a fired cast that never happened AND
            // re-stamped the Task-2 hand lock with the HELD-OFF spell --
            // on any lap where no lock was live yet (fresh after
            // ClearCastLock, the Scheduler's no-cast-rule path, an
            // OOC->combat transition) that re-label starved BOTH rules
            // until a TTL. Transparent NoOp, its own label, NO lock.
            case SelfCast::Held:
                return { Result::NoOp, "concentration held off (another heal owns the claim)", true };
            case SelfCast::Declined:
            default:
                // Unaffordable (§5.3) / LoS or line-of-fire held (offense) /
                // off-AE: transparent + legible, the rules below run.
                return { Result::FailedOther, "concentration direct-force declined", true };
            }
        }

        // a_rangeGate (#68): true only for an OBVIOUS target (ladder rungs
        // 1-3 -- a selector, an explicit subject, a condition-implied actor).
        // The PLAYER FALLBACK (rung 4) always passes false: a rule that
        // exists to cover "nobody obvious applies" must fire regardless of
        // distance, or the fallback itself becomes just another thing that
        // can silently not happen. kActCastSelf/kActCastPlayer never set
        // this (unchanged -- they are not part of the #68 ladder at all).
        Outcome CastOn(RE::Actor* a_follower, RE::FormID a_spellID, RE::Actor* a_target,
                       bool a_rangeGate = false) {
            // T#67 SE/VR GUARD (mirrors the CasterConsent hook guards). The mage
            // cast-control path CRASHES on Skyrim SE 1.5.97: a reporter's crash log
            // pinned an EXCEPTION_ACCESS_VIOLATION to Scheduler::Tick -> Actuation::
            // Fire -> CastOn on the SKSE job worker (byte read off a poisoned
            // pointer), an SE-only divergence in the equip/cost work below. The
            // forced-cast PACKAGE route already declines off AE, but CastOn's own
            // Loadout::Prepare (spell equip) + CalculateMagickaCost run FIRST and
            // are what fault. This whole feature is AE-developed and AE-tested, so
            // off AE we decline the cast rule TRANSPARENTLY -- the follower's own
            // vanilla AI keeps casting (mobile, animated), exactly the graceful
            // degradation the VR guards already give. Gate here (not just the
            // package route) so no cast-control code runs at all off AE.
            if (!REL::Module::IsAE())
                return { Result::FailedOther,
                         "cast control is AE-only (SE/VR use the follower's own AI casting)", true };
            // TRANSPARENT (GAMBIT_FLOWS §2): a cast that provably cannot run this
            // tick must not wall off the rules below it -- FFXII skips an
            // unaffordable gambit and runs the next line.
            if (!a_target) return { Result::FailedOther, "no valid target", true };

            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!spell) {
                return { Result::FailedOther,
                         std::format("spell {:08X} not in load order", a_spellID), true };
            }

            // #68 OUT-OF-RANGE SKIP, obvious targets only (a_rangeGate).
            // Self and Touch delivery are CONTACT/self spells -- there is no
            // "aimed" range to exceed, so they are never out of range no
            // matter the distance. For Aimed/TargetActor/TargetLocation,
            // SpellItem::GetRange() is the SPIT record's own Range field
            // (confirmed in the pinned CommonLibSSE-NG headers: RE::MagicItem
            // declares the virtual, RE::SpellItem overrides it to return
            // data.range). A ZERO/unset range FAILS OPEN -- plenty of vanilla
            // spells ship with no declared cap, and reading that as "cannot
            // reach anyone" would silently disable rules on data we cannot
            // read confidently, exactly the failure mode §5.3 exists to avoid.
            // TRANSPARENT (GAMBIT_FLOWS §2): a target merely being far away
            // must not wall off the gambits below this one.
            if (a_rangeGate && a_target && a_target != a_follower) {
                const auto delivery = spell->GetDelivery();
                if (delivery != RE::MagicSystem::Delivery::kSelf &&
                    delivery != RE::MagicSystem::Delivery::kTouch) {
                    const float range = spell->GetRange();
                    if (range > 0.0f) {
                        const float dist = a_follower->GetPosition().GetDistance(a_target->GetPosition());
                        if (dist > range) {
                            return { Result::NoOp,
                                     std::format("target beyond {} range ({:.0f}/{:.0f})",
                                                 spell->GetName() ? spell->GetName() : "spell",
                                                 dist, range),
                                     true };
                        }
                    }
                }
            }

            // ── DOES THE FOLLOWER EVEN KNOW THIS SPELL? (SEV-1, pre-merge review
            // 2026-09-07) ───────────────────────────────────────────────────────────
            // `Loadout::Prepare` has always made this its FIRST check
            // (`Loadout.cpp:246-249` -> `Ready::Failed`, INVARIANTS #20 / DESIGN §5.4),
            // and on main that ordering was load-bearing in a way nothing wrote down:
            // every APMF ask lived AFTER a successful `Prepare`, so a claim for a spell
            // the actor does not know was STRUCTURALLY IMPOSSIBLE.
            //
            // Moving the ask into the pre-flight below broke that for free. A
            // `cast_target Firebolt` rule on a follower who lacks Firebolt (roster
            // swap, shared template list, progression removal) would ask APMF every
            // lap, `Prepare` would answer Failed, and the claim would just STAND: the
            // rule's condition still holds so `castSeen` is true and Scheduler's
            // `!castSeen` release never fires, and the per-lap refresh keeps
            // `FacetExpiry` from sweeping it. While it stands,
            // `CasterConsent::ClientCastClaimed` early-passes on `IsOwnedCastActive`,
            // so MFO's OWN deny is stood down for that follower indefinitely -- and
            // APMF's seats are handed a spell the actor cannot select. That is MFO
            // declaring intent it already knows cannot run (principle 4).
            //
            // So the competence gate moves AHEAD of the ask, beside the magicka gate
            // it belongs with. `Prepare`'s own identical check STAYS as defence in
            // depth -- this is a second gate, not a relocation.
            if (!a_follower->HasSpell(spell)) {
                // TRANSPARENT, and the SAME reason string `Prepare` produces, so the
                // board and the log read identically whichever gate caught it.
                return { Result::FailedSkill, "follower does not know this spell", true };
            }

            // COMPETENCE IS NOT PERMISSION (DESIGN.md §5.3). MFO does not top up
            // magicka, discount the cost, or substitute a cheaper spell -- if
            // the follower cannot afford it the rule FAILS and the tick falls
            // through, and the board says why. This is the whole point: a list
            // you wrote that your follower cannot run is legible, not silent.
            auto* avo = a_follower->AsActorValueOwner();
            float cost = 0.0f;
            if (avo) {
                cost = spell->CalculateMagickaCost(a_follower);
                const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
                if (cost > have) {
                    // Take it back. The rule keeps winning while the condition
                    // holds, so wantsCast stays true and the scheduler's release
                    // never runs -- leaving a spell they cannot afford in their
                    // hand for their AI to keep trying.
                    Loadout::ReleaseSpell(a_follower->GetFormID());
                    spdlog::debug("[eval] {:08X} has {:.0f} magicka, needs {:.0f}",
                                  a_follower->GetFormID(), have, cost);
                    // TRANSPARENT: this is the spellsword flow -- reserve/empty
                    // pool falls through to steel (GAMBIT_FLOWS D1, §3.5).
                    return { Result::FailedSkill,
                             std::format("insufficient magicka (needs {:.0f})", cost), true };
                }

                // THE RESERVE. §5.3 says the follower's competence decides what
                // they can run -- but a gambit that empties the pool leaves them
                // unable to do anything ELSE they know, which is not what a
                // player means by "heal yourself when hurt". Keep a floor.
                // M2 from review: the floor is about what is left AFTER the
                // cast. "Do not start below 25%" still lets a 40%-of-pool spell
                // finish at nearly zero, which is not what "keep something for
                // everything else you know" means.
                const float reserve = Config::g_magickaReserve.load();
                if (reserve > 0.0f) {
                    auto* avo2 = a_follower->AsActorValueOwner();
                    const float mx = avo2
                        ? avo2->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                          a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                            RE::ActorValue::kMagicka)
                        : 0.0f;
                    if (mx > 0.0f && (have - cost) < reserve * mx) {
                        Loadout::ReleaseSpell(a_follower->GetFormID());
                        return { Result::FailedSkill,
                                 std::format("magicka reserve (floor {:.0f})", reserve * mx),
                                 true };   // transparent -- fall to steel (§3.5)
                    }
                }
            }

            // TASK 2 (feat/cast-gambit-concentration) / PER-HAND (feat/per-hand-
            // cast-slots, 2026-09-06): THE FIRING-SPELL GAMBIT LOCK. Checked
            // here -- after range/competence (a request that could not run
            // anyway is not "held off", it simply fails as before) and BEFORE
            // every branch below that would touch equip/consent/claims for a
            // possibly-DIFFERENT spell. See ResolveCastHand's own doc (above
            // ConcentrationCast) for the full rationale; in short, a DIFFERENT
            // (spell,target) than one already occupying THIS HAND across ticks
            // (an owned-cast claim or a concentration stream) is held off,
            // transparently, until that hand's lock goes live-false or stale --
            // the OTHER hand is untouched.
            //
            // WHICH HAND would this request use if it proceeds? Self-target
            // and every concentration spell are LEFT unconditionally (matches
            // ConcentrationCast/CastSelfDirect/CastTargetDirect's own hand
            // policy below); a Heal/Buff spell is LEFT always too (the
            // composed-cast branch's/legacy heal's hard rule); an Offense
            // spell at a real target is the only real DECISION, resolved via
            // Loadout::PlanCastHand -- see ResolveCastHand for the juggle.
            // `handPlan` is reused below at every HoldCastLock/ClaimOffenseCast
            // call site in this function so the lock and the actual APMF claim
            // never disagree on which hand(s) they occupy.
            const auto id = a_follower->GetFormID();
            const RE::FormID lockTargetKey =
                (!a_target || a_target == a_follower) ? 0 : a_target->GetFormID();
            const bool selfOrConc =
                (!a_target || a_target == a_follower) ||
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            const bool offenseSpell =
                !selfOrConc && CasterConsent::ClassifySpell(spell) == CasterConsent::SpellKind::Offense;
            HandPlan handPlan;
            if (!offenseSpell) {
                if (auto held = ResolveCastHand(id, Loadout::HandPick::Left, a_spellID,
                                                lockTargetKey, handPlan))
                    return *held;
            } else {
                const auto pick = Loadout::PlanCastHand(a_follower, spell,
                                                        APMFBridge::WeaponHandActive(a_follower));
                if (auto held = ResolveCastHand(id, pick, a_spellID, lockTargetKey, handPlan))
                    return *held;
            }
            // Hold BOTH resolved hands (only ever one, unless a DualCast plan
            // resolved both) on every success path below -- one lambda so the
            // ownedCast/composed-cast/self/concentration call sites all stay
            // in sync with `handPlan` rather than re-deriving it.
            auto lockHands = [&](RE::FormID a_sp, RE::FormID a_tg) {
                if (handPlan.left)  HoldCastLock(id, kHandLeft,  a_sp, a_tg);
                if (handPlan.right) HoldCastLock(id, kHandRight, a_sp, a_tg);
            };

            // SELF-CAST forks off FIRST (SPEC-self-cast-forced): a bCastSelf-
            // armed cast_self -- concentration OR fire-and-forget -- fires
            // through the UNIVERSAL direct trigger (CastSelfDirect), BEFORE the
            // concentration fork and before the equip/grace/package machinery.
            // Self needs neither AI-grace nor an alias package (which package-
            // locked custom followers decline), so it bypasses both. It is a
            // CHANNEL, self-paced by its own registry -- do NOT gate on
            // Loadout::CoolingDown or call StartCooldown: StartCooldown ->
            // ReleaseSpell would rip the spell out of the hand and kill the
            // animation. Re-fire it every combat tick while the rule wins (it
            // refreshes the channel + applies the effect on the channel's own
            // beat -- ~1 s kConcApplyPeriod for concentration, fCastCooldown
            // for FF); the reconcile releases it when the rule goes false.
            if (a_target == a_follower && Config::g_castSelf.load()) {
                // F3 tri-state (mirrors Logistics.cpp's OOC cast dispatch,
                // ~`:1500-1560` -- see the corrected cite at the self fork above):
                // only a real Applied is THIS tick's action. A Refreshed tick (the channel is
                // winning but paced out -- nothing applied) is TRANSPARENT so a
                // persistently-true combat cast_self does not starve attack/drink/
                // heal below it, and `lastFired`/`[eval] fired` never lies on a
                // no-op tick.
                switch (CastSelfDirect(a_follower, spell)) {
                case SelfCast::Applied:
                    // TASK 2: hold the lock (LEFT -- handPlan resolved above) --
                    // a claimed (heal/instant) or concentration self-cast is
                    // multi-tick; an FF instant apply with no live claim
                    // self-releases within one round-robin lap via
                    // CastLockLive's staleness fallback (harmless).
                    lockHands(a_spellID, 0);
                    return { Result::Fired, "self-cast channel (direct trigger)" };
                case SelfCast::Refreshed:
                    lockHands(a_spellID, 0);
                    // Channel kept alive, no effect/magicka this tick -- fall past.
                    return { Result::NoOp, "self-cast refresh (paced)", true };
                // Fable SEV-2 (2026-09-06): a HELD outcome is not a cast.
                // ComposedCast::Try held this spell off because a DIFFERENT
                // spell's live claim owns the follower's single heal slot, so
                // nothing was claimed and nothing was applied. It used to
                // arrive as Applied (the hold rode home inside Try()'s bare
                // `true`), which reported a fired cast that never happened AND
                // re-stamped the Task-2 hand lock with the HELD-OFF spell --
                // on any lap where no lock was live yet (fresh after
                // ClearCastLock, the Scheduler's no-cast-rule path, an
                // OOC->combat transition) that re-label starved BOTH rules
                // until a TTL. Transparent NoOp, its own label, NO lock.
                case SelfCast::Held:
                    return { Result::NoOp, "self-cast held off (another heal owns the claim)", true };
                case SelfCast::Declined:
                default:
                    // Unaffordable / off-AE / no caster: transparent, the rules
                    // below run (the follower is not stuck on a cast that can't go).
                    return { Result::FailedOther, "self-cast could not fire", true };
                }
            }

            // CONCENTRATION forks off HERE -- after the range and competence
            // gates (a stream obeys §5.3 and #68 like any cast), BEFORE the
            // equip/grace/force machinery, all of which assumes a
            // fire-and-forget release to observe. The bounded stream is its
            // own actuation (see ConcentrationCast above). (Self concentration
            // never reaches here -- the self fork above intercepts it.)
            if (spell->GetCastingType() ==
                RE::MagicSystem::CastingType::kConcentration) {
                return ConcentrationCast(a_follower, spell, a_target);
            }

            // THE FOLLOWER CASTS IT -- MFO does not cast on their behalf.
            //
            // ActorMagicCaster is driven by the ANIMATION GRAPH (it inherits
            // SimpleAnimationGraphManagerHolder and sinks BSAnimationGraphEvent),
            // so CastSpellImmediate can never animate no matter which casting
            // source issues it -- tested across all four, ENGINE_NOTES §0.13.
            // The only animated path is the vanilla one every enemy mage uses:
            // put the spell in a hand and let the actor cast.
            //
            // The off hand is what makes this affordable (DESIGN §4.5b).
            // ONE new mechanism this release (#45): equipping. The cast
            // itself still goes through CastSpellImmediate, deliberately.
            //
            // `Projectile::LaunchSpell` was the obvious partner and is WRONG
            // here: it launches a projectile, and a Self-delivery spell -- the
            // flagship `Self HP < 40% -> Cast Healing` gambit -- has no
            // projectile to launch. That path would have equipped the spell and
            // applied NOTHING, strictly worse than the silent heal it replaced.
            // It also bypasses MagicCaster entirely, so it spends no magicka,
            // which would make §5.3's gate a tautology (ENGINE_NOTES §0.9).
            //
            // So: put the spell in their hand, then cast through THAT HAND's
            // caster. Whether holding the spell is enough to make the graph
            // animate is the open question this build exists to answer -- and
            // if the answer is no, behaviour is exactly what it is today rather
            // than a regression.
            // ── ASK APMF BEFORE TOUCHING THE FOLLOWER'S HANDS (F3-2, deploy-gate
            // review 2026-09-07) ───────────────────────────────────────────────────
            // "Fail closed" was only half-true: both APMF asks used to live INSIDE
            // the equip switch below, i.e. AFTER `Loadout::Prepare` had already put
            // MFO's spell in the follower's LEFT hand and AFTER `CasterConsent::Want`
            // had latched consent for it. Nothing undid either on a refusal, so a
            // refused tick still left two side effects standing -- and the field shape
            // is ugly: when another owner holds the cast facet with ITS spell equipped,
            // `Prepare` short-circuits only if MFO's own spell is already in hand
            // (Loadout.cpp), so MFO re-equipped over that owner EVERY 133 ms tick while
            // logging "this cast does NOT happen", and with consent latched the
            // follower's own AI could then cast MFO's spell unforced. That is a
            // fallback nobody asked for, dressed as a refusal -- the exact mask
            // fix/mfo-no-decline-fallback exists to remove (principle 7).
            //
            // So the ask moves AHEAD of the equip and the consent, and a refusal
            // returns from here: no equip, no consent, no Targeting::Command, no lock --
            // nothing to undo, because nothing was done.
            //
            // GATED ON bEquipToCast, exactly like the block below: both asks were
            // inside `if (Config::g_equipToCast.load())` before this change and must
            // stay inside it, or a toggle-off build would start claiming facets it
            // never claimed.
            //
            // CONSEQUENCE, DELIBERATE AND DOCUMENTED: a claim can now be made on a tick
            // whose `Loadout::Prepare` then returns Debounced/Failed, which could not
            // happen before. That is correct under the owned model -- APMF's own seats
            // do the equipping while the claim stands, so MFO declining to thrash ITS
            // gear is not a reason to drop APMF's claim.
            //
            // HOW IT ENDS -- and the first version of this note got this WRONG (SEV-2,
            // pre-merge review 2026-09-07). It said the claim is "released crisply by
            // the Scheduler's !castSeen path either way". It is not, in the common
            // case: through a Debounced stretch the rule's condition still HOLDS, so
            // `castSeen` is true and Scheduler's `!castSeen` release never runs, while
            // the per-lap refresh keeps `FacetExpiry` from sweeping it. The claim ends
            // when the RULE stops holding -- not when Prepare starts debouncing.
            //
            // OPEN DECISION, NOT DECIDED HERE (SEV-3, same review -- marth's call).
            // `Loadout::Ready::Debounced` is PRIMARILY the `fCastCooldown` case
            // (`Loadout.cpp:~317`, `CoolingDown`), not just the two-hander/gear-debt
            // waits the paragraph below describes. On main a cooldown stretch left the
            // claim UN-refreshed, `FacetExpiry` swept it (~2.95 s at defaults, solo),
            // and `CasterConsent`'s pacing deny re-engaged for the tail. Here the claim
            // is refreshed through the whole cooldown, so that deny never re-engages
            // and **fCastCooldown is effectively inert under the owned model.** That
            // may well be RIGHT -- under the owned model the follower's own AI paces
            // the cast, which is the entire point -- but it is a real behaviour change
            // to a shipped knob and marth has not been shown it. Deliberately NOT
            // gated on `Loadout::CoolingDown` pending that call. See Config.h's
            // fCastCooldown entry.
            //
            // The SUCCESS paths are unchanged: they fall through into the switch below
            // exactly as before and still return from their old positions.
            const bool ownedCast =
                APMFBridge::Available() && Config::g_apmfCast.load() &&
                !Config::g_legacyCastHybrid.load() &&
                a_target && a_target != a_follower &&
                a_target != RE::PlayerCharacter::GetSingleton() &&
                CasterConsent::ClassifySpell(spell) == CasterConsent::SpellKind::Offense;

            // Did the offense claim STAND this tick? Consumed by the owned-cast branch
            // below in place of the ask it used to make itself.
            bool ownedClaimHeld = false;
            // The heal ask's outcome, likewise consumed below. NotApplicable when the
            // ask was never made (bEquipToCast off, self-target, or the owned-cast
            // branch owns this spell instead).
            auto composed = ComposedCast::TryResult::NotApplicable;

            if (Config::g_equipToCast.load()) {
                if (ownedCast) {
                    // HAND POLICY (integration/2026-09-06, PER-HAND feat/per-hand-cast-
                    // slots 2026-09-06): `handPlan` was already resolved by the per-hand
                    // cast-gambit lock gate above (Loadout::PlanCastHand, juggled against
                    // the CURRENT lock state -- the hard, non-negotiable weapon-hand-
                    // exclusion rule lives inside PlanCastHand itself, checked before
                    // Dual/EitherFree are even considered). Reuse it here rather than
                    // recomputing -- the lock and the actual APMF claim must never
                    // disagree on which hand(s) they occupy. Translate the resolved
                    // HandPlan into the bridge's a_hand encoding directly (not `HandFor`,
                    // which only maps a bare HandPick and cannot express an
                    // already-juggled concrete Right).
                    //
                    // concentration = false: this branch never sees one (concentration
                    // forked off to ConcentrationCast's direct-force stream, above,
                    // before any of this runs). stopPct = 0: a heal-only concept (a
                    // client restore threshold); offense has no such threshold in scope.
                    const std::int32_t claimHand =
                        (handPlan.left && handPlan.right) ? APMFBridge::kApmfHandDualCast :
                        handPlan.left                     ? APMFBridge::kApmfHandLeft :
                                                             APMFBridge::kApmfHandRight;
                    ownedClaimHeld =
                        APMFBridge::ClaimOffenseCast(a_follower->GetFormID(), spell->GetFormID(),
                                                     a_target->GetFormID(), claimHand,
                                                     /*concentration=*/false, /*stopPct=*/0);
                    if (!ownedClaimHeld && APMFBridge::OffenseCastClaimSupported()) {
                        // APMF is PRESENT + CAPABLE and it REFUSED. FAIL CLOSED: no
                        // legacy hybrid, no force, no CastSpellImmediate -- and now no
                        // equip and no consent either. FailedSkill/transparent, NOT an
                        // opaque NoOp: the taxonomy's transparent case is "the rule
                        // provably cannot run this tick" (Actuation.h), which is exactly
                        // this. Failing closed means MFO does not route the CAST around
                        // APMF, not that the follower is frozen out of every rule below.
                        LogApmfRefusal(a_follower->GetFormID(), "offense-cast",
                                       spell->GetFormID(), a_target->GetFormID(),
                                       handPlan.left && handPlan.right ? "dual"
                                           : handPlan.left             ? "left"
                                                                       : "right");
                        return { Result::FailedSkill, "APMF refused the cast claim", true };
                    }
                    // Not held and NOT supported -> APMF is ABSENT for this facet (its
                    // ABI predates RequestCast, or bApmfCast raced off). The DECLARED
                    // degrade contract, not a refusal: equip/consent/legacy hybrid run
                    // below exactly as in a world with no APMF. Silent -- APMFBridge
                    // warns once per session about a too-old ABI itself.
                } else if (a_target && a_target != a_follower) {
                    // The heal twin. `ownedCast` is Offense-only by its own
                    // classification check, so a Heal/Buff spell never took that branch
                    // and fell straight into the AI-first-grace + force-on-miss hybrid
                    // even with APMF present -- while the SAME kIntent_Cast claim
                    // (`ComposedCast::Try`) already handles the identical spell/target
                    // pair when it arrives via `CastSelfDirect`/`CastTargetDirect`.
                    // Try() is fully self-gating (HEAL-ONLY, AE + APMF +
                    // bHealAnimPackage), so a Buff/Offense kind degrades to
                    // NotApplicable immediately, byte-identical to not calling it at all.
                    //
                    // `a_target != a_follower` scopes this to ally/player targets only,
                    // matching the audit's own framing -- a self-target CAN reach this
                    // far when bCastSelf (dev-only, default off) never forked it off,
                    // and self-cast is a separate, already-gated mechanism
                    // (`CastSelfDirect`) this must not silently annex.
                    //
                    // stopPct = 0 (stop at full restoration): no per-gambit numeric
                    // threshold is in scope here, matching every ComposedCast::Try call
                    // site except CastAuto's own (CAST-DELIVERY.md's STOP-PERCENT note).
                    composed = ComposedCast::Try(a_follower, spell, a_target,
                                                 CasterConsent::ClassifySpell(spell),
                                                 /*stopPct=*/0);
                    if (composed == ComposedCast::TryResult::ApmfRefused) {
                        // FAIL CLOSED, same reasoning as the offense ask above and
                        // DISTINCT from Held (which is another heal owning the slot, not
                        // APMF's arbitration saying no). Heals are LEFT always
                        // (ClaimHealCast's own hard rule), so the hand is not a variable.
                        LogApmfRefusal(a_follower->GetFormID(), "heal-cast",
                                       spell->GetFormID(), a_target->GetFormID(), "left");
                        return { Result::FailedSkill, "APMF refused the heal claim", true };
                    }
                }
            }

            bool equipped = false;
            if (Config::g_equipToCast.load()) {
                std::string why;
                switch (Loadout::Prepare(a_follower, spell, why)) {
                case Loadout::Ready::AlreadyReady:
                case Loadout::Ready::Equipped: {
                    equipped = true;

                    // INFLUENCE (§0.28). The spell is now in their hand; latch
                    // CONSENT so their own combat AI's CheckStartCast returns
                    // true for it. The AI then casts it AS a combat action --
                    // mobile, animated, correctly timed -- rather than vetoing
                    // it (§0.16). This is why the grace wait below now usually
                    // succeeds: MFO removed the veto that made it fail. No-op
                    // if bCasterHook is off; observe-only in log mode.
                    CasterConsent::Want(a_follower->GetFormID(), spell->GetFormID());

                    // ── OWNED CAST MODEL (default when APMF present; MODERATOR redesign,
                    // marth 2026-09-02). A REAL, AI-DECIDED, MOBILE, ANIMATED cast, made
                    // by the FOLLOWER'S OWN combat AI -- MFO does NOT force it. This is the
                    // same real cast MFO could already produce (equip + consent + a cast-
                    // biased combat style => the AI's own vanilla animated cast, ENGINE_NOTES
                    // §0.15a/§0.27/§0.28), but WITHOUT its old movement cost: the animated
                    // cast never needed the rooting UseMagic package (that was only the
                    // legacy force route) -- so we drop the package entirely and the follower
                    // keeps kiting while it casts.
                    //
                    // DIVISION OF LABOUR (the whole point of routing through APMF; UPDATED
                    // feat/offense-cast-seats, 2026-09-05 -- the offense claim moved off
                    // ch.8 kIntent_SelectSpell onto ch.8b kIntent_Cast, the SAME facet
                    // ClaimHealCast uses, so APMF now does more than arbitrate):
                    //   * MFO EXECUTES the equip/target/consent half with its OWN proven
                    //     mechanisms, same as before:
                    //       - PUT the spell in hand: Loadout::Prepare's EquipSpell into the
                    //         LEFT hand slot (above, §0.28) -- this is what physically makes
                    //         the spell castable at all, and is why the claim below also
                    //         always passes hand=LEFT (the claimed hand and the
                    //         physically-equipped hand must never disagree);
                    //       - COMMAND our target: Targeting::Command -> currentCombatTarget
                    //         (its UpdateCombat hook re-asserts it);
                    //       - CONSENT: CasterConsent::Want (granted just above) -- lets the
                    //         AI consider casting OUR spell at all (a veto-removal, never an
                    //         invented consent).
                    //   * APMF CLAIMS + DRIVES the cast + combat-target facets: MFO claims
                    //     kIntent_Cast (ClaimOffenseCast, below) naming the exact spell and
                    //     target, and while that claim stands APMF's five engine vfunc seats
                    //     drive the follower's OWN combat AI to select/equip/charge/aim/
                    //     fire/channel EXACTLY that spell at EXACTLY that target -- so
                    //     (unlike the retired gate-only claim, which only ARBITRATEd+DENIED
                    //     a competitor's own selection) the gambit's named spell is now the
                    //     one that actually fires, not whatever the AI would have picked.
                    //     APMF still fires NOTHING itself -- no EquipSpell/CastSpell/
                    //     CastSpellImmediate/anim-graph write of any kind; every engine call
                    //     is the AI's own, from its own behavior tree.
                    // GRANULAR: we claim ONLY the cast + combat-target facets -- we do NOT
                    // touch the movement facet (no SetDontMove, no block), so the follower
                    // keeps moving under its own control WHILE its AI casts. That granular
                    // non-interruption is exactly APMF's value for casting.
                    //
                    // NO forced cast on this path -- CastSpellImmediate NEVER runs here; force
                    // survives only in the LEGACY hybrid below (bLegacyCastHybrid / APMF
                    // absent). If a follower's combat style still will not DECIDE to cast,
                    // that is a magic-score bias question (raise it), NOT a reason to force.
                    // Concentration never reaches this branch (its bounded direct fork
                    // returned above -- exact-bounding intact).
                    //
                    // `ownedClaimHeld` was resolved by the PRE-FLIGHT above, before the
                    // equip and the consent (F3-2). A refusal already returned from
                    // there, so reaching this branch means APMF HOLDS the claim; a
                    // false here means only "APMF absent for this facet", which falls
                    // through to the legacy hybrid exactly as it always did.
                    if (ownedClaimHeld) {
                        // CLAIM the cast-EXECUTION facet FIRST (kIntent_Cast/RequestCast,
                        // ch.8b -- PORTED feat/offense-cast-seats, 2026-09-05, off the
                        // retired ch.8 kIntent_SelectSpell gate-only claim this site used
                        // to make: that channel only ARBITRATEd+DENIED a competing
                        // framework's own selection, so the follower's own AI still picked
                        // WHATEVER SPELL IT WANTED -- the long-standing "target right,
                        // spell wrong" defect. This claim instead rides the SAME
                        // kIntent_Cast facet ClaimHealCast uses: while it stands, APMF's
                        // five engine seats drive the follower's OWN combat AI to
                        // select/equip/charge/aim/fire/channel THIS spell at THIS target
                        // natively, closing the defect. Call EVERY tick this branch wins,
                        // same "the claim call also refreshes" idiom every other APMF
                        // claim here uses (EnsureCastClaimLocked, APMFBridge.cpp, no-ops
                        // an unchanged claim but always stamps offenseRefreshed, which is
                        // what keeps it alive against the round-robin-aware FacetExpiry()
                        // backstop -- a per-follower dedupe latch here would starve the
                        // claim mid-cast exactly like the 2026-09-02 attempt did for the
                        // old claim, reverted same day per Fable review).
                        //
                        // hand: NO LONGER always LEFT -- see the PER-HAND HAND POLICY
                        // paragraph just below, which resolves it via Loadout::
                        // PlanCastHand/handPlan instead (integration/2026-09-06, then
                        // feat/per-hand-cast-slots). NOTE (open item, flagged not
                        // fixed by this pass): `Loadout::Prepare`'s own `EquipSpell`
                        // call, above, still ALWAYS equips into `LeftHandSlot()`
                        // unconditionally -- a Right or DualCast claimHand can now
                        // diverge from the physically-equipped hand, a gap this pass
                        // did not close (see this branch's own MAP.md/CAST-DELIVERY.md
                        // entry). concentration = false: this branch never sees one
                        // (concentration forked off to ConcentrationCast's direct-force
                        // stream, above, before the equip/ownedCast machinery runs at
                        // all). stopPct = 0: a heal-only concept (a client restore
                        // threshold); offense has no such threshold in scope.
                        //
                        // FAIL CLOSED, NOT a fall-through (fix/mfo-no-decline-fallback,
                        // marth 2026-09-07: "With APMF theres no fallback for it. APMF
                        // shoudl work"). The ask, and the refusal that fails closed on
                        // it, both live in the PRE-FLIGHT above so that a refusal leaves
                        // no equip/consent side effects standing (F3-2); the hand policy
                        // that picks `claimHand` from `handPlan` is documented there too.
                        {
                            // ARBITRATE the combat-target facet too (ch.6, unchanged) --
                            // separate from the cast claim's own `target` field (which only
                            // feeds the magic-target/aim seats): this keeps APMF the single
                            // arbiter of currentCombatTarget against a competing framework
                            // while the AI is casting.
                            APMFBridge::ClaimCombatTarget(a_follower->GetFormID(),
                                                          a_target->GetFormID(), /*create=*/true);

                            // EXECUTION (MFO's own): command our target every tick too --
                            // Targeting::Command (Targeting.h:37-41) itself dedupes on an
                            // unchanged latch and only reports a real change, so this is
                            // equally cheap. Consent was granted above; APMF's engine seats
                            // now supply the AI's DECISION+SPELL+TARGET directly. The
                            // follower's own AI then casts our spell at our target -- full
                            // animation, still mobile.
                            Targeting::Command(a_follower->GetFormID(), a_target->GetHandle());

                            // Reuse the SAME [cfc]-style silent-claim diagnostic the heal
                            // path arms via ComposedCast::Try -- if this claim stands but
                            // the SpellSink never observes it actually firing, a
                            // rate-limited [cfc] warning names the follower/spell (no
                            // watchdog, no re-fire, no fallback -- silence is the signal).
                            // PER HAND now: watch every hand this claim actually occupies
                            // (both, for a DualCast plan), so a right-hand-only offense
                            // claim gets its own watch slot instead of sharing (and
                            // potentially losing to) a concurrent left-hand heal's. Fetch
                            // the SAME claim's minted delivery-flip proxy (ABI v6; 0 on
                            // ABI < 6 or no proxy minted) so the watch recognises a cast of
                            // the proxy, not only the original spell, as this claim firing
                            // (cast-claim observability, 2026-09-06).
                            if (handPlan.left)
                                ComposedCast::WatchClaim(a_follower->GetFormID(), spell->GetFormID(),
                                                         APMFBridge::kApmfHandLeft,
                                                         APMFBridge::GetOffenseCastProxy(
                                                             a_follower->GetFormID(), APMFBridge::kApmfHandLeft));
                            if (handPlan.right)
                                ComposedCast::WatchClaim(a_follower->GetFormID(), spell->GetFormID(),
                                                         APMFBridge::kApmfHandRight,
                                                         APMFBridge::GetOffenseCastProxy(
                                                             a_follower->GetFormID(), APMFBridge::kApmfHandRight));

                            // TASK 2: this IS the multi-tick "actively firing"
                            // case the gambit lock exists for -- hold it (on
                            // whichever hand(s) handPlan resolved) so a different
                            // cast rule cannot re-point APMF's engine seats
                            // mid-charge/mid-decision ON THAT SAME HAND. The
                            // other hand, if any, stays free for a different
                            // spell.
                            lockHands(a_spellID, a_target->GetFormID());

                            // OPAQUE hold: the AI is deciding+casting; firing lower rules
                            // now risks disturbing that decision (the §0.6 confound). No
                            // force, ever, here.
                            return { Result::NoOp, "owned cast: AI deciding (animated, mobile)" };
                        }
                    }

                    // COMPOSED CAST for a Heal/Buff spell aimed at an ally or the player,
                    // fire-and-forget (API-PORT-AUDIT.md #1). `ownedCast` above is
                    // Offense-only by its own classification check, so it never fires for
                    // a Heal/Buff spell -- without this, that case fell straight into the
                    // AI-first-grace + force-on-miss hybrid below even with APMF present,
                    // even though the SAME kIntent_Cast claim (`ComposedCast::Try`) already
                    // handles the identical spell/target pair when it arrives via
                    // `CastSelfDirect`/`CastTargetDirect` (concentration, or self-target FF).
                    // This call mirrors those two exactly -- same signature, same
                    // HEAL-ONLY internal gate (`ComposedCast::Enabled`, ComposedCast.cpp),
                    // so a Buff kind here degrades to false immediately, byte-identical to
                    // not calling it at all.
                    //
                    // IN-COMBAT ONLY: CastOn only runs from Actuation::Fire, the combat-
                    // rule dispatch (this file's own header comment) -- a live
                    // CombatController is already guaranteed here. The out-of-combat
                    // mirror (Logistics.cpp's FF beneficial direct-apply) is deliberately
                    // untouched: whether kIntent_Cast's engine seats function without a
                    // live CombatController is an open question (API-PORT-AUDIT.md §5.1).
                    //
                    // stopPct = 0 (stop at full restoration): no per-gambit numeric
                    // threshold is in scope here, matching every ComposedCast::Try call
                    // site except CastAuto's own (CAST-DELIVERY.md's STOP-PERCENT note).
                    //
                    // FOUR OUTCOMES (ComposedCast::TryResult -- Held from the Fable
                    // SEV-2 pass, the NotApplicable/ApmfRefused split from
                    // fix/mfo-no-decline-fallback, marth 2026-09-07):
                    // NotApplicable (Buff kind, AE/APMF/toggle absent, ABI < 5) still
                    // falls straight through to the SAME grace/ForceCast hybrid below,
                    // byte-identical to today -- with APMF absent a heal must never
                    // silently vanish. ApmfRefused (APMF present, capable, and it said
                    // NO) FAILS CLOSED: the heal does not happen, loudly, and MFO does
                    // not route around APMF into the force hybrid. Held is neither --
                    // another heal owns the slot and nothing happened at all.
                    //
                    // `a_target != a_follower` scopes this to ally/player targets only,
                    // matching the audit's own framing -- a self-target CAN reach this far
                    // when bCastSelf (dev-only, default off) never forked it off at :522,
                    // and self-cast is a separate, already-gated mechanism (CastSelfDirect)
                    // this fix must not silently annex.
                    // `composed` was resolved by the PRE-FLIGHT above (F3-2), before the
                    // equip and the consent, so an ApmfRefused outcome already returned
                    // from there with nothing done. It stays NotApplicable when the ask
                    // was never made at all.
                    {
                        switch (composed) {
                        case ComposedCast::TryResult::Claimed:
                            // TASK 2: same multi-tick "actively firing" reasoning as the
                            // ownedCast branch above -- hold the lock (LEFT -- handPlan
                            // resolved above, this is a Heal/Buff spell) so a different
                            // cast rule cannot re-point APMF's engine seats mid-charge/
                            // mid-decision.
                            lockHands(a_spellID, a_target->GetFormID());

                            // OPAQUE hold: the AI is deciding+casting; firing lower rules now
                            // risks disturbing that decision (the §0.6 confound). No force,
                            // ever, here.
                            return { Result::NoOp, "composed cast: AI deciding (animated, mobile)" };

                        // Fable SEV-2 (2026-09-06): HELD is not a claim. A DIFFERENT
                        // spell's live claim owns this follower's heal slot, so
                        // nothing was claimed here. Folded into Try()'s old bare
                        // `true`, this branch re-stamped the Task-2 lock with the
                        // HELD-OFF spell and returned an OPAQUE NoOp that walled off
                        // every rule below -- so on a lap with no live lock yet both
                        // rules starved until a TTL. TRANSPARENT NoOp, no lock: the
                        // incumbent keeps its slot, and the rules below this one still
                        // get their tick. Deliberately NOT a fall-through to the
                        // AI-first grace/ForceCast hybrid below -- that would force
                        // exactly the competing cast the hold exists to prevent.
                        case ComposedCast::TryResult::Held:
                            return { Result::NoOp,
                                     "composed cast: held off (another heal owns the claim)", true };

                        // FAIL CLOSED (fix/mfo-no-decline-fallback, marth 2026-09-07).
                        // APMF is present AND capable AND it refused this heal claim, so
                        // there is no fallback: NOT the AI-first grace/ForceCast hybrid,
                        // NOT a kInstant apply. The PRE-FLIGHT above already returned on
                        // this outcome (F3-2 -- so that no equip/consent side effect is
                        // left standing), which makes this arm unreachable today. It is
                        // kept, and kept IDENTICAL, deliberately: it is listed so the
                        // switch stays exhaustive without a `default:` (F3-6), and if a
                        // future edit ever lets this outcome reach here it must still
                        // fail closed rather than silently take the hybrid below.
                        // DISTINCT from Held above -- Held is another heal owning the
                        // slot, this is APMF's arbitration saying no.
                        case ComposedCast::TryResult::ApmfRefused:
                            LogApmfRefusal(a_follower->GetFormID(), "heal-cast",
                                           spell->GetFormID(), a_target->GetFormID(), "left");
                            return { Result::FailedSkill,
                                     "APMF refused the heal claim", true };

                        // APMF was never asked (non-Heal kind / SE / toggle off / APMF
                        // absent / ABI < 5) -- the degrade contract, unchanged. NO
                        // `default:` (F3-6): all four enumerators are listed, so a fifth
                        // TryResult value must break the build instead of silently
                        // taking the force hybrid below.
                        case ComposedCast::TryResult::NotApplicable:
                            break;   // -> the AI-first grace/ForceCast hybrid below
                        }
                    }

                    // GIVE THE FOLLOWER'S OWN AI A CHANCE FIRST.
                    //
                    // This is the whole point and it is easy to destroy. The
                    // animated cast comes from the follower's combat AI firing
                    // a spell we put in their hand -- but if MFO silent-casts
                    // in the same tick it equips, the heal lands instantly, the
                    // low-HP condition disappears, and the AI never has a
                    // reason to cast. The evidence we are trying to collect is
                    // destroyed by the act of collecting it: exactly the §0.6
                    // confound, rebuilt.
                    //
                    // So: hold off for fAiCastGrace. If a [cast] line appears in
                    // that window, the AI did it and it animated. If the window
                    // passes and the condition is still true, fall through to
                    // the silent cast -- an unanimated heal beats no heal.
                    const float held = Loadout::SecondsSinceEquip(a_follower->GetFormID());
                    // FLAIR #6: each caster has their own patience. The MCM knob
                    // stays the CENTER; the temperament seed spreads the band
                    // +-13% so two mages who both fail to animate do not land
                    // twin silent heals on the same beat. Deterministic per
                    // follower; the reason string (the dedup key) is unchanged.
                    const float grace = Config::g_aiCastGrace.load() *
                        (0.87f + 0.26f * Temperament(a_follower->GetFormID()));

                    // THE MISS DETECTOR. "The AI cast a different spell" ends
                    // the grace EARLY: that cast swapped the equipped spell,
                    // MFO re-equipped it this tick, and SecondsSinceEquip
                    // restarted -- so without this flag the grace clock resets
                    // forever and the configured spell is never cast (the
                    // Marcurio/Firebolt field report). A cast of OUR spell is
                    // the success path and never reaches here: the sink starts
                    // the cooldown and retires the miss flag -- keeping the
                    // latch (v1.0.30) -- so Prepare() debounces the next tick
                    // with the deny still standing.
                    const bool aiCastOther =
                        CasterConsent::OtherCastSeen(a_follower->GetFormID());
                    if (held < grace && !aiCastOther) {
                        // A STABLE reason string. The transition logger compares
                        // reasons, so embedding the elapsed time here would make
                        // every tick a "new" reason and log at 7.5 Hz.
                        // OPAQUE (marth, GAMBIT_FLOWS §7.1): the hold IS the cast
                        // happening -- firing lower rules mid-grace risks
                        // disturbing the AI's cast (the §0.6 confound).
                        return { Result::NoOp, "waiting for their AI to cast it" };
                    }

                    // THE AI MISSED -- grace spent with nothing, or spent on
                    // its own spell. Force the CONFIGURED spell through the
                    // package route; only a structural decline falls through
                    // to the silent apply below (never a regression).
                    if (auto forced = ForceCast(a_follower, spell, a_target, aiCastOther)) {
                        return *forced;
                    }
                    break;
                }
                case Loadout::Ready::Debounced:
                    // NOT a rule failure -- the follower is willing and able,
                    // MFO is declining to thrash their gear.
                    //
                    // RE-ASSERT THE LATCH FIRST (v1.0.30). The rule is still
                    // WINNING -- this wait is part of the cast's own pacing --
                    // so exclusive control must hold through it. The sink no
                    // longer drops the latch on a cast, and (v1.0.32) neither
                    // does an H3 condition flicker -- but this branch is still
                    // reachable UNLATCHED: combat end clears the latch while a
                    // cooldown stamped in the LAST fight survives into the
                    // next one, so the first services of a new fight can land
                    // here with no latch standing. Without this line that
                    // left the deny off for the rest of the cooldown -- the
                    // residual leak. Idempotent overwrite (spell only, never
                    // the pace), same call the Ready path makes; observe-only
                    // in log mode, exactly like every other Want.
                    CasterConsent::Want(a_follower->GetFormID(), spell->GetFormID());

                    // STAMP THE HAND LOCK IF A CLAIM IS STANDING (SEV-2, pre-merge
                    // review 2026-09-07). "A live claim implies a hand lock" was an
                    // invariant main got for free: every claim was minted inside the
                    // Equipped arm, which always calls `lockHands`. The pre-flight can
                    // now mint on a Debounced tick, and `HandFree` returns true on
                    // `lock.spell == 0` BEFORE it ever consults `CastLockLive` -- so an
                    // unlocked-but-claimed hand read as free. Two ways that bites:
                    //   * THRASH: a fight starting inside `fCastCooldown` (which
                    //     survives combat end -- the very case this branch exists for)
                    //     with two same-hand cast rules true. A mints, debounces
                    //     unlocked; B passes HandFree, claims, and
                    //     `EnsureCastClaimLocked`'s param change Releases A and
                    //     RequestCasts B; next lap A does the same to B. APMF's seats
                    //     get re-pointed twice a lap for the whole cooldown.
                    //   * INVISIBLE NON-OWNER: a heal minted unlocked, then a lower
                    //     same-hand offense rule claims the same hand. APMF's tie rule
                    //     is "earliest keeps it", so the newcomer is a NON-OWNER -- but
                    //     `ClaimOffenseCast` returns true (the handle IS valid), so we
                    //     report the opaque "owned cast: AI deciding" and stamp a lock
                    //     for Firebolt while APMF is driving the heal.
                    // Restoring the stamp restores the invariant; the lock is
                    // claim-driven via `CastLockLive`, so this is the cheap correct
                    // one. (The Failed arm needs no equivalent: with the HasSpell gate
                    // above, the only way to reach it holding a claim is a null
                    // graph/actor manager, which is negligible and self-clearing.)
                    if (ownedClaimHeld || composed == ComposedCast::TryResult::Claimed)
                        lockHands(a_spellID, lockTargetKey);

                    // TRANSPARENT (marth, GAMBIT_FLOWS §7.1): cast cooldown /
                    // two-handed debounce / gear debt are seconds-long waits
                    // during which the follower FIGHTS -- the rules below run.
                    return { Result::NoOp, why, true };
                case Loadout::Ready::Failed:
                default:
                    return { Result::FailedSkill, why, true };   // transparent (§2)
                }
            }

            // PROBE ONLY, DEFAULT OFF (bCommandCast).
            //
            // This was built believing Actor.DoCombatSpellApply was Bethesda's
            // "cast this spell at THIS target, as a combat action" verb. IT IS
            // NOT. Actor.psc's own comment reads "Apply a spell to a target in
            // combat"; the Papyrus index positions it as the alternative to
            // AddSpell; and every shipped call site in ten modlists uses it as
            // an INSTANT SILENT APPLY -- including Bethesda's own Dawnguard
            // shield script, which uses it to eject the player. It is the
            // Papyrus twin of CastSpellImmediate.
            //
            // It stays behind a default-off flag for ONE measurement: whether a
            // proc-apply verb deducts magicka (if it does not, §0.9's finding
            // that our pre-check is the only gate stands unchanged). It is not
            // the animation answer, and the animation answer is not a spell
            // verb at all -- see ENGINE_NOTES §0.14.
            if (Config::g_commandCast.load() && Papyrus::Available()) {
                if (Papyrus::DoCombatSpellApply(a_follower, spell, a_target)) {
                    // Re-arm here too, or the one-shot bug simply survives on
                    // the bCommandCast + bEquipToCast combination.
                    if (equipped) Loadout::ArmGrace(a_follower->GetFormID());
                    return { Result::Fired, "dispatched" };
                }
                // Fall through to the silent path rather than dropping the
                // rule: an unanimated heal beats no heal.
            }

            // Cast from the hand the spell is actually in when we equipped it;
            // otherwise honour iCastSource. CastSpellImmediate is PROVEN to
            // deduct magicka (§0.9) and to work for self-delivery spells, which
            // is why it stays the verb.
            using CS = RE::MagicSystem::CastingSource;
            CS src = CS::kInstant;
            if (equipped) {
                // AlreadyReady can mean the spell was in the RIGHT hand all
                // along (a caster follower's own choice) -- match the hand the
                // spell is actually in. A freshly-queued equip may not read
                // back yet, so the left hand stays the default.
                src = (a_follower->GetEquippedObject(false) == spell &&
                       a_follower->GetEquippedObject(true)  != spell)
                          ? CS::kRightHand
                          : CS::kLeftHand;
            } else {
                switch (Config::g_castSource.load()) {
                case 0:  src = CS::kLeftHand;  break;
                case 1:  src = CS::kRightHand; break;
                case 2:  src = CS::kOther;     break;
                default: src = CS::kInstant;   break;
                }
            }
            // DRIVE THE CASTER (probe, bDriveCaster, default off).
            //
            // Every path that APPLIES a spell is silent, proven three times.
            // The animation comes from the caster's own state machine
            // advancing, which the combat AI normally kicks off. The animation
            // events cannot do it -- ten modlists use BeginCastLeft and its
            // siblings ONLY with RegisterForAnimationEvent, so the graph EMITS
            // them. That leaves the caster itself.
            //
            // EXPECTATION, stated so the result is falsifiable: this should
            // start the request and charge, and then WEDGE, because the release
            // step comes from the animation graph (MRh_SpellFire_Event ->
            // StartCastImpl) and nothing will play that animation. If so, the
            // shipped answer is a forced casting package instead (§0.17).
            //
            // The follower goes unhealed while this runs. That is deliberate:
            // a same-tick silent fallback would rebuild the §0.6 confound and
            // we could not tell which path produced the effect.
            if (Config::g_driveCaster.load() && equipped) {
                auto* hand = a_follower->GetMagicCaster(src);
                if (hand) {
                    // VERIFY, DO NOT FORCE. There is no SetCurrentSpell at the
                    // pinned CommonLib rev -- only SetCurrentSpellImpl, a no-op
                    // on ActorMagicCaster. Writing `currentSpell` by hand would
                    // desync the engine's own select/deselect bookkeeping, and
                    // the equip is queued so it may not have landed yet.
                    if (hand->currentSpell != spell) {
                        return { Result::NoOp, "caster has not selected the spell yet" };
                    }

                    // ONLY FROM REST. MSCO's shipped hook exists specifically to
                    // DENY RequestCastImpl once the caster is past its early
                    // states; re-requesting mid-sequence is how a follower ends
                    // up with charge-glow hands for a whole fight and a caster
                    // that accepts nothing further.
                    const auto st = hand->state.get();
                    if (st != RE::MagicCaster::State::kNone) {
                        hand->InterruptCast(true);      // refunds magicka
                        // The state number belongs in its OWN line, not in the
                        // reason: the reason is the dedup key, and a number that
                        // moves makes every tick a new message.
                        spdlog::info("[drive] {:08X} caster busy at state {} -- interrupted",
                                     a_follower->GetFormID(), static_cast<std::uint32_t>(st));
                        return { Result::NoOp, "caster busy -- interrupted" };
                    }

                    // Ask FIRST why it might refuse. This line is most of the
                    // probe's evidentiary value: it separates "the engine
                    // rejected the cast" from "the engine accepted it and the
                    // graph never released".
                    float strength = 1.0f;
                    RE::MagicSystem::CannotCastReason reason{};
                    const bool ok = hand->CheckCast(spell, false, &strength, &reason, false);

                    if (a_target) hand->desiredTarget = a_target->CreateRefHandle();
                    hand->RequestCastImpl();

                    // ORDER IS LOAD-BEARING: ArmGrace before StartCooldown, so
                    // the minimum hold stops the cooldown yanking the spell out
                    // of their hand mid-cast.
                    Loadout::ArmGrace(a_follower->GetFormID());
                    Loadout::StartCooldown(a_follower->GetFormID());

                    spdlog::info("[drive] {:08X} {} -- CheckCast={} reason={} state {} -> {}",
                                 a_follower->GetFormID(),
                                 spell->GetName() ? spell->GetName() : "?",
                                 ok ? "OK" : "REFUSED",
                                 static_cast<std::uint32_t>(reason),
                                 static_cast<std::uint32_t>(st),
                                 static_cast<std::uint32_t>(hand->state.get()));
                    return { Result::Fired, "caster driven" };
                }
            }

            // THREADING (#14): CastOn runs on the AddTask job worker
            // (Scheduler::Tick -> Fire), and this silent cast used to call
            // CastSpellImmediate INLINE here -- the same off-main pattern the
            // queued 1.5.x act.cast_target AV reports point at. Post the engine
            // apply + deduct to the MAIN thread, re-resolving by FormID inside
            // (never carry Actor* across threads). Fired is returned
            // optimistically, the same contract as ApplySelfEffect/
            // ApplyTargetEffect. `src` is captured by value (an enum).
            //
            // CHARGE THEM (inside the post). Measured: package casts and
            // CastSpellImmediate both spend NOTHING (ENGINE_NOTES §0.22), so
            // without this §5.3's competence gate is decorative. The cost is
            // CalculateMagickaCost(a_follower), the ACTOR overload (skill +
            // perks -- marth, 2026-07-22). INVARIANTS #16 forbids hand-writing
            // state a flow PRODUCES; this flow produces no deduction at all,
            // so this fills a gap rather than duplicating one -- the same call
            // DAC makes. Clamped to the live pool (#6) so a deduct can never
            // drive magicka negative.
            {
                auto doCast = [fid = a_follower->GetFormID(),
                               tid = a_target ? a_target->GetFormID() : 0,
                               spid = spell->GetFormID(), src] {
                    auto* f  = RE::TESForm::LookupByID<RE::Actor>(fid);
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spid);
                    // A null target stays null (the original inline call passed
                    // a_target through as-is; CastSpellImmediate accepts null).
                    auto* t  = tid ? RE::TESForm::LookupByID<RE::Actor>(tid) : nullptr;
                    if (!f || !sp || (tid && !t)) return;
                    auto* caster = f->GetMagicCaster(src);
                    if (!caster) caster = f->GetMagicCaster(CS::kInstant);
                    if (!caster) return;   // F4: no caster -> no cast, no deduct
                    auto* mavo = f->AsActorValueOwner();
                    const float pool = mavo ? mavo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                    caster->CastSpellImmediate(sp, false, t, 1.0f, false, 0.0f, f);
                    const float c     = sp->CalculateMagickaCost(f);
                    const float spend = mavo ? std::min(c, pool) : 0.0f;
                    if (mavo && spend > 0.0f)
                        mavo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                RE::ActorValue::kMagicka, -spend);
                };
                // VR has no pump (Post is a documented no-op): fall back inline.
                if (MainThread::IsInstalled()) MainThread::Post(doCast);
                else                           doCast();
            }

            // Restart the AI's window. Without this the grace is a ONE-SHOT:
            // the clock was armed at the first equip and never re-armed, so
            // after MFO's first cast every later one fired instantly and the
            // follower's own AI was never given another opening. The field log
            // showed it as rule 0 firing every 1.6s -- the suppression window,
            // not the 3s grace.
            if (equipped) {
                Loadout::ArmGrace(a_follower->GetFormID());
                Loadout::StartCooldown(a_follower->GetFormID());
            }

            return { Result::Fired, equipped ? "silent (their AI declined)" : "" };
        }

        // ── Tier-A EQUIP actions (§4.5) ──────────────────────────────────────
        // Equip the best weapon of a category from the follower's OWN inventory.
        // IDEMPOTENT: a no-op when already holding that category, so a persistently
        // winning rule does not re-equip every tick. Direct ActorEquipManager,
        // the same path LootEquipment uses; runs in combat, so a weapon it equips
        // can coexist with a left-hand spell but is NOT tracked by Loadout's hand
        // ledger -- acceptable because equip and cast are alternative gambits
        // (first-match-wins fires only one per tick).
        Outcome EquipWeapon(RE::Actor* a_follower, bool a_ranged) {
            // BOTH HANDS decide "already holding" (T#75). The old guard read only
            // the RIGHT hand -- but a caster keeps a SPELL there, so the melee
            // weapon her off-hand still held was invisible to it and a
            // persistently-winning equip rule re-equipped the SAME weapon every
            // service tick (deck, Serana: 'Ebony Tanto' at gambit cadence,
            // visible weapon thrash while her own AI fought back for the
            // spell). The rule's goal is a weapon CATEGORY in hand -- EITHER
            // hand satisfies it.
            // marth: a base MAGE's melee sidearm is a DAGGER -- the same base-class
            // rule the loot path uses (#65 combatClassOverride==3/Cast, gated by
            // bMageDaggersOnly). For such a follower a MELEE equip order means a
            // dagger specifically: a looted sword/mace he still owns must NOT count
            // as "already holding the category" (else the satisfied NoOp below keeps
            // him on it forever) and must NOT win the draw. baseClass read on the
            // worker's g_followers-serial path (same as Scheduler / Actuation:25).
            // v1.1: actor-keyed g_followers find + combatClassOverride read == the
            // general GetBaseClass primitive (byte-identical; same serial-worker path).
            const std::uint8_t baseClass = Followers::GetBaseClass(a_follower);
            const bool daggerMelee = !a_ranged && baseClass == 3 && Config::g_mageDaggersOnly.load();
            const auto holdsCategory = [a_ranged, daggerMelee](RE::TESForm* a_held) {
                auto* w = a_held ? a_held->As<RE::TESObjectWEAP>() : nullptr;
                if (!w || w->IsStaff()) return false;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) return false;
                return !daggerMelee || w->GetWeaponType() == RE::WEAPON_TYPE::kOneHandDagger;
            };
            // TRANSPARENT (satisfied): the rule's goal already holds, so
            // the scan falls past it -- AND the scheduler reads this
            // exact shape (transparent NoOp on an equip action) as the
            // H2 hand-claim: a lower CONTRADICTORY equip is skipped
            // without firing, so fall-through cannot manufacture the
            // melee<->ranged thrash of GAMBIT_FLOWS D4.
            if (holdsCategory(a_follower->GetEquippedObject(false)) ||
                holdsCategory(a_follower->GetEquippedObject(true)))
                return { Result::NoOp, "already holding that category", true };
            RE::TESObjectWEAP* best = nullptr; std::uint16_t bestDmg = 0;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w->IsStaff()) continue;
                // NON-PLAYABLE (record-header flag bit 2) is creature/automaton
                // gear -- fires but is INVISIBLE on a humanoid (same check as
                // Logistics' IsCreatureWeapon). Loot never stocks these, but a
                // player-given stray must not win the combat equip either.
                if ((w->GetFormFlags() & (1u << 2)) != 0) continue;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) continue;
                // base MAGE melee = daggers only (see holdsCategory above).
                if (daggerMelee && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                if (w->GetAttackDamage() >= bestDmg) { bestDmg = w->GetAttackDamage(); best = w; }
            }
            if (!best) return { Result::FailedSkill, a_ranged ? "no ranged weapon carried"
                                                              : "no melee weapon carried",
                                true };   // transparent -- cannot act, rules below run (§2)
            if (auto* mgr = RE::ActorEquipManager::GetSingleton()) {
                if (Config::g_weaponStyleControl.load()) {
                    // T#76 FORCE-HOLD. forceEquip=true (7th arg, after extraData,
                    // count, slot, queueEquip) sets the engine's prevent-removal
                    // lock so the follower's OWN combat AI cannot auto-unequip the
                    // weapon to re-arm a spell -- the both-hands caster thrash
                    // v1.0.62 narrowed but did not close. The both-hands "already
                    // holding that category" NoOp above then keeps the rule
                    // satisfied with no re-equip until the gambit's condition goes
                    // false, when the scheduler releases it (ReconcileForcedWeapon).
                    const auto id = a_follower->GetFormID();
                    // A DIFFERENT weapon may still be locked from before -- a
                    // category flip (melee<->ranged), OR a base-mage daggers-only
                    // SAME-category swap (a looted sword was force-locked, then
                    // holdsCategory rejected it and the loop picked a dagger; the
                    // satisfied NoOp no longer catches this melee->melee case since
                    // c97035f). Force-unequip the old lock first, then lock the new.
                    // Read/write the map UNDER the lock; do the engine calls
                    // OUTSIDE it (SEV-1 discipline).
                    RE::TESBoundObject* oldForced = nullptr;
                    {
                        std::scoped_lock lk(g_forcedMx);
                        if (auto old = g_forcedWeapon.find(id);
                            old != g_forcedWeapon.end() && old->second && old->second != best)
                            oldForced = old->second;
                    }
                    if (oldForced)
                        mgr->UnequipObject(a_follower, oldForced, nullptr, 1, nullptr, true, true);
                    mgr->EquipObject(a_follower, best, nullptr, 1, nullptr, true, true);
                    {
                        std::scoped_lock lk(g_forcedMx);
                        g_forcedWeapon[id] = best;
                    }
                } else {
                    mgr->EquipObject(a_follower, best);   // kill-switch off: today's behaviour exactly
                }
            }
            // FLAIR #4: visibly COMMIT to the new tool -- steel out, squared up
            // -- rather than leaving the swap in whatever draw state the last
            // weapon had. The exact call Loadout.cpp:240 already ships for the
            // cast path; drawing while already drawn is a no-op, and this path
            // only runs on a real equip Fired (which buys a suppression window),
            // so it cannot repeat-fire.
            a_follower->DrawWeaponMagicHands(true);
            spdlog::info("[equip] {:08X}: GAMBIT equip {} '{}' dmg={}", a_follower->GetFormID(),
                         a_ranged ? "ranged" : "melee",
                         best->GetFullName() ? best->GetFullName() : "?", bestDmg);
            return { Result::Fired, a_ranged ? "equipped ranged" : "equipped melee" };
        }

        // (EquipTorch moved to Logistics -- torch is upkeep, not a combat action, #35.)

    }

    // THE RESOLUTION LADDER (#68) -- PUBLIC so combat (Fire) AND logistics
    // (out-of-combat cast_target) resolve a manual target the SAME way. Given a
    // cast-target choice, decides WHO gets cast at, first rung to match wins:
    //   1 (+3) a selector (or Evaluate()'s player-HP-condition special case)
    //       already named someone THIS TICK -- a_choice.target.
    //   2   an explicit subject the player picked on the row: a SPECIFIC
    //       follower (subjectActorForm) takes precedence over the enum when both
    //       are somehow set; Self/Player always resolve; NearestAlly falls on
    //       down the ladder if nobody is in range.
    //   4   otherwise -- THE PLAYER. The catch-all for actor-agnostic rules
    //       ("when dark -> cast Magelight") that would otherwise resolve to
    //       nobody. a_outIsFallbackPlayer marks this rung so CastOn's out-of-
    //       range skip knows to WAIVE it -- a fallback must always fire, never
    //       quietly vanish because the player happens to be far away.
    RE::Actor* ResolveCastTarget(RE::Actor* a_follower, const Eval::Choice& a_choice,
                                 bool& a_outIsFallbackPlayer) {
        a_outIsFallbackPlayer = false;
        if (auto ptr = a_choice.target.get(); ptr.get()) return ptr.get();

        auto* player = RE::PlayerCharacter::GetSingleton();

        // A SPECIFIC follower, gone/dead/unloaded falls on down the ladder
        // rather than bailing -- a "questionable" target must never wall off the
        // whole rule (the #68 problem statement).
        if (a_choice.subjectActorForm != 0) {
            if (auto* specific = RE::TESForm::LookupByID<RE::Actor>(a_choice.subjectActorForm)) {
                if (!specific->IsDead() && specific->Is3DLoaded()) return specific;
            }
        }

        switch (static_cast<Vocab::Subject>(a_choice.subject)) {
        case Vocab::Subject::Player:
            return player;
        case Vocab::Subject::NearestAlly:
            if (auto* ally = NearestAlly(a_follower)) return ally;
            break;   // nobody in range -- fall to the player default below
        case Vocab::Subject::Self:
        default:
            // #68 (marth): on a CAST-AT-TARGET row the default subject (Self=0 --
            // what every pre-#68 row and every freshly-added row carries) means
            // AUTO: run the ladder to the PLAYER fallback, so an actor-agnostic
            // rule ("when dark -> cast Magelight") lights up around the player
            // instead of resolving to nobody (the #68 bug) or to the caster.
            // Deliberate self-casting is the separate "Cast on self" action
            // (kActCastSelf), which never routes through this ladder. A stale
            // specific-follower sentinel (0xFF) lands here too and correctly
            // falls through rather than disarming the rule.
            break;
        }

        a_outIsFallbackPlayer = true;
        return player;
    }

    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice) {
        // No rule matched -> NO ENGINE CALL AT ALL. Not a no-op command, not a
        // neutral order: nothing (§4.4's do-nothing guarantee, which is what
        // makes an MFO follower byte-identical to a vanilla one when idle).
        if (!a_follower || a_choice.ruleIndex < 0) return { Result::NoOp, {} };

        const auto& op = a_choice.actionOpcode;

        if (op == Vocab::kActWait) {
            // Deliberately consumes the tick without acting -- the FFXII
            // "do nothing on purpose" idiom that lets a rule suppress the ones
            // below it (DESIGN.md §3.3). OPAQUE by design (GAMBIT_FLOWS §7.1):
            // Wait is the authored suppress idiom; it must keep walling.
            return { Result::NoOp, {} };
        }
        if (op == Vocab::kActAttack) {
            // THE ATTACK VERB (DESIGN §4.7a). There is no engine call that says
            // "swing at this one" -- the combat controller owns that entirely
            // and exposes nothing. What MFO can do is decide WHO, by latching
            // the choice; the hook then re-asserts it after every combat update,
            // because the engine re-picks continuously (ENGINE_NOTES §0.6).
            //
            // So this is cheap and idempotent by design: a rule that keeps
            // winning simply keeps naming the same foe.
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe) return { Result::FailedOther, "chosen foe no longer resolves", true };

            // A commanded Attack directive gets the SAME APMF combat-target arbitration
            // a cast directive already gets (ch.6 plumbing, no new APMF work): claim (or
            // re-point) the facet at the chosen foe. create=true => a pure-melee follower
            // with no prior cast-directive claim this fight now gets one created here,
            // instead of getting zero arbitration until/unless it later casts.
            // (MFO's own Targeting::Command below is what actually commands the target;
            // APMF only arbitrates the facet, it executes nothing.) No-op without APMF.
            if (APMFBridge::Available() && Config::g_apmfCast.load() &&
                !Config::g_legacyCastHybrid.load())
                APMFBridge::ClaimCombatTarget(a_follower->GetFormID(), foe->GetFormID(), /*create=*/true);

            // Ask the HOOK, not the config. They disagree whenever install was
            // refused with the flag on -- VR, today. Reporting Fired there would
            // buy a suppression window for a latch nothing reads.
            if (!Targeting::IsHooked()) {
                return { Result::FailedOther, "targeting hook not installed", true };
            }

            // Re-commanding the SAME foe is not an action. The latch persists
            // and the hook re-asserts it every combat update, so a rule that
            // keeps winning would otherwise report Fired every suppression
            // window forever -- nine identical log lines in one fight, and a
            // suppression window burned each time that a higher rule could
            // have used.
            // OPAQUE by design (GAMBIT_FLOWS D3): Attack is an ACTIVITY --
            // FFXII's own semantics put reactive rules ABOVE it, and lines
            // below an active Attack never run. Do NOT make this transparent.
            if (!Targeting::Command(a_follower->GetFormID(), a_choice.target)) {
                return { Result::NoOp, "already on that target" };
            }
            // Log the foe's HP% too, so "Foe: lowest HP -> Attack" is legible in
            // the log -- confirms the SELECTOR actually picked the weakest foe.
            return { Result::Fired,
                     std::format("target {} ({}% hp)",
                                 foe->GetName() ? foe->GetName() : "?",
                                 static_cast<int>(Vocab::HealthPct(foe) * 100.0f)) };
        }
        // SUMMON SPAM GUARD (v1.1.1, COMBAT path). Mirrors the out-of-combat skip
        // in Logistics::ServiceFollower. A conjured/reanimated creature is a
        // COMMANDED ACTOR, not a caster-side magic effect, so the AI-first cast
        // grace below re-fires a summon gambit every grace window even while the
        // creature is still alive (marth, field). Suppress the cast while the
        // follower already commands a LIVE summon from THIS spell -- caster-side,
        // per-spell (Twin-Souls-aware via the helper), keyed on the live actor so
        // a killed/expired summon recasts at once. TRANSPARENT, so the combat scan
        // falls PAST it to the next rule exactly like the cast-grace hold. No-op
        // for any non-summon spell (CasterHasLiveSummon returns false), so buffs/
        // heals/candlelight and every non-summon combat cast are byte-identical and
        // the grace/pacing below is untouched. Reads only the active-effect list --
        // the same off-worker read as the existing combat guards, no new lock.
        if (op == Vocab::kActCastSelf || op == Vocab::kActCastPlayer ||
            op == Vocab::kActCastTarget) {
            if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_choice.actionParam);
                sp && CasterHasLiveSummon(a_follower, sp)) {
                return { Result::NoOp, "summon still live", true };
            }
            // (APMF cast ownership is engaged inside CastOn's FF-non-self hostile
            // branch, where the RESOLVED target is known -- see the OWNED cast model
            // there. Not here: the target is not resolved for kActCastTarget yet.)
        }
        if (op == Vocab::kActCastSelf) {
            return CastOn(a_follower, a_choice.actionParam, a_follower);
        }
        if (op == Vocab::kActCastPlayer) {
            return CastOn(a_follower, a_choice.actionParam, RE::PlayerCharacter::GetSingleton());
        }
        if (op == Vocab::kActCastTarget) {
            // AUTO (marth): the board's DEFAULT target pick ("Auto", carried as
            // Subject::Self on a cast-target row) infers the set from the spell's
            // nature and fans the cast out (CastAuto). It engages ONLY when
            // nothing more specific named a target -- no explicit subject actor,
            // no selector/condition target this tick. A MANUAL pick (Player /
            // Nearest ally / a specific follower) or a selector that chose a foe
            // keeps the single-target ladder path below, unchanged.
            auto tp = a_choice.target.get();
            const bool autoPick =
                a_choice.subjectActorForm == 0 &&
                static_cast<Vocab::Subject>(a_choice.subject) == Vocab::Subject::Self &&
                !tp.get();
            if (autoPick) {
                // #2: a beneficial HEAL AUTO fan must match the firing gambit's own
                // status requirement, not blanket-heal everyone below full. When the
                // firing rule's condition is a health-below gate, its param IS the
                // threshold (heal only allies under it); otherwise 1.0 keeps the
                // whole-party / anyone-below-full behaviour ("Always -> Heal (Auto)",
                // world-gated buffs). Non-heal spells ignore the threshold entirely.
                float healThreshold = 1.0f;
                if (a_choice.conditionOpcode == Vocab::kCondSelfHpBelow   ||
                    a_choice.conditionOpcode == Vocab::kCondPlayerHpBelow ||
                    a_choice.conditionOpcode == Vocab::kCondAllyHpBelow)
                    healThreshold = a_choice.conditionParam;
                return CastAuto(a_follower, a_choice.actionParam, healThreshold);
            }

            // #68: the full resolution ladder -- a selector/condition target
            // first, then the row's explicit subject, then the player as the
            // last rung. a_rangeGate is OFF only for that last rung (the
            // fallback must fire regardless of distance); every obvious
            // target above it is range-checked inside CastOn.
            bool isFallbackPlayer = false;
            auto* target = ResolveCastTarget(a_follower, a_choice, isFallbackPlayer);
            return CastOn(a_follower, a_choice.actionParam, target, !isFallbackPlayer);
        }

        if (op == Vocab::kActEquipRanged) return EquipWeapon(a_follower, true);
        if (op == Vocab::kActEquipMelee)  return EquipWeapon(a_follower, false);
        // kActEquipTorch moved to the LOGISTICS table (#35, marth: never needed in
        // combat) -- handled by Logistics::ServiceFollower now, not here.

        if (op == Vocab::kActFlee) {
            // Disengage by reusing the RETREAT package -- travel to the player
            // under kIgnoreCombat, the proven machinery that pulls a follower off
            // a fight (Packages::RetreatFill). Cleared on combat end / arrival by
            // the retreat driver. #35. APMF SHOWPIECE: when APMF is present,
            // RetreatFill routes this through APMF's ch.9 0x49 package-offer
            // channel instead of MFO_RetreatQuest's alias/static-priority-60
            // race (same fix as loot-travel, for a follower package-locked by
            // an outranking custom AI framework); the alias route below is the
            // APMF-ABSENT degrade only. Transparent here -- RetreatFill owns
            // the routing decision.
            if (Packages::RetreatFill(a_follower))
                return { Result::Fired, "flee -> retreat to player" };
            return { Result::FailedOther, "retreat unavailable", true };
        }
        if (op == Vocab::kActPowerAttack) {
            // "Foe blocking -> Power attack" (marth): get in MELEE range of the
            // chosen foe, THEN power-attack that foe. There is no engine verb for
            // "power-attack X" -- MFO owns WHO (the target latch, like Attack) and
            // the range GATE (fire the swing only when actually adjacent); the
            // engine's own combat AI owns the MOVEMENT that closes the distance.
            //
            // The chosen foe is a_choice.target -- for a foe selector (incl.
            // kCondFoeBlocking) PickFoe already picked the specific blocking,
            // hostile, in-chase-cap foe (Evaluator.cpp PickFoe; #22b: that
            // selection is EVALUATOR state, we only ACT on it here). This gate is
            // GENERAL: any gambit whose action is Power attack swings only when in
            // reach of ITS intended target, not "any foe blocked -> swing in air"
            // (marth, field: the follower power-attacked at whatever range the
            // instant any foe blocked).
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe) return { Result::FailedOther, "chosen foe no longer resolves", true };
            if (!Targeting::IsHooked())
                return { Result::FailedOther, "targeting hook not installed", true };
            // Only meaningful with a MELEE weapon drawn -- a power attack from a bow
            // or empty hands is nonsense and would just burn the tick (Fable).
            auto* r = a_follower->GetEquippedObject(false);
            auto* w = r ? r->As<RE::TESObjectWEAP>() : nullptr;
            const auto wt = w ? w->GetWeaponType() : RE::WEAPON_TYPE::kHandToHandMelee;
            const bool melee = w && wt != RE::WEAPON_TYPE::kBow && wt != RE::WEAPON_TYPE::kCrossbow &&
                                    wt != RE::WEAPON_TYPE::kStaff;
            // TRANSPARENT: "foe blocking -> power attack" while holding a bow
            // must not starve the attack rule below for as long as the foe
            // blocks (GAMBIT_FLOWS §3.6). Checked BEFORE any latch so a rejected
            // (bow-held) power attack mutates NOTHING and falls through cleanly.
            if (!melee) return { Result::FailedSkill, "no melee weapon drawn for a power attack", true };

            // RANGE GATE. GetDistance is a pure read of already-loaded actor data
            // (the kCondFoeWithinRange path reads distance the same way on this
            // same worker tick), so no MainThread::Post is needed here.
            const float dist = a_follower->GetPosition().GetDistance(foe->GetPosition());
            const char* foeName = foe->GetName() ? foe->GetName() : "?";

            // OUT of melee reach -> CLOSE first. Latch the chosen foe as the combat
            // target: the combat-thread hook re-asserts it every update and the
            // engine's own combat AI walks the follower in (the same "approach"
            // plain Attack relies on -- MFO invents no movement). OPAQUE like Attack
            // (GAMBIT_FLOWS D3): while closing on the blocking foe the tick belongs
            // to this rule; a lower rule must not pull him off it. Latching then
            // reporting Fired is a single consistent outcome (no fall-through), so
            // it does NOT reintroduce the "latch + transparent-reject" two-mutation
            // tick the anim path below still guards against.
            if (dist > Config::g_meleeReach.load()) {
                Targeting::Command(a_follower->GetFormID(), a_choice.target);
                return { Result::Fired, std::format("closing to melee on {}", foeName) };
            }

            // IN melee reach -- SWING. Try the anim FIRST and commit the target
            // latch ONLY if it accepted. Latch-then-reject (Opus review) was a
            // two-mutation tick: Command wrote the latch, then the transparent
            // reject fell through and let a LOWER rule also fire -- and a FAILED
            // power attack still committed a target. Ordering the graph check ahead
            // means a rejected power attack mutates NOTHING and falls through
            // cleanly (§4.3, GAMBIT_FLOWS §3.6). `sent` still isn't proof the swing
            // landed; field-verify. Report Fired only on accept so a reject buys no
            // suppression window (Fable: Fired-on-reject went visibly passive).
            if (!a_follower->NotifyAnimationGraph("attackPowerStartInPlace"))
                return { Result::FailedOther, "power-attack anim rejected", true };
            Targeting::Command(a_follower->GetFormID(), a_choice.target);
            return { Result::Fired, std::format("power attack -> {}", foeName) };
        }

        // IN-COMBAT DRINKING. The same drink action logistics runs OUT of combat,
        // available here so a survival gambit ("Self HP < 30% -> Drink health
        // potion") can fire mid-fight. Logistics::DrinkPotion is cooldown-gated
        // per resource (~the potion's own duration), so a persistently-winning
        // rule cannot chain-drink the whole stack -- and it drinks only what the
        // follower already carries (§5.3: MFO hands out nothing).
        if (op == Vocab::kActDrinkHealthPotion || op == Vocab::kActDrinkStaminaPotion ||
            op == Vocab::kActDrinkMagickaPotion) {
            const auto av = (op == Vocab::kActDrinkHealthPotion)  ? RE::ActorValue::kHealth  :
                            (op == Vocab::kActDrinkStaminaPotion) ? RE::ActorValue::kStamina :
                                                                    RE::ActorValue::kMagicka;
            if (Logistics::DrinkPotion(a_follower, av))
                return { Result::Fired, "drank a potion" };
            // TRANSPARENT: the worst concrete D1 -- a follower must not die
            // holding a heal rule below an out-of-potions drink rule (§3.1).
            return { Result::FailedSkill, "no matching potion, or still on cooldown", true };
        }

        // Unknown action opcode: fail closed and say so. Likely a rule from a
        // newer vocabulary; it must never fall through to SOMETHING ELSE for
        // this rule -- but the scan may fall PAST it, exactly as the logistics
        // scan skips a stray opcode so it cannot shadow the rules below
        // (Logistics.cpp fall-through precedent).
        return { Result::FailedOther, std::format("unknown action '{}'", op), true };
    }

    // ── T#76: EQUIP FORCE-HOLD lifecycle ──────────────────────────────────────
    void ReleaseForcedWeapon(RE::Actor* a_follower) {
        if (!a_follower) return;
        const auto id = a_follower->GetFormID();
        RE::TESBoundObject* obj = nullptr;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(id);
            if (it == g_forcedWeapon.end()) return;
            obj = it->second;
            g_forcedWeapon.erase(it);
        }
        // forceEquip=true on the UNequip clears the prevent-removal lock the
        // force-equip set; a plain unequip would be REFUSED against a forced
        // item and the follower would stay stuck holding the weapon, unable to
        // cast -- the worse-than-oscillation failure this whole feature must not
        // create. Engine call OUTSIDE the lock.
        if (auto* mgr = RE::ActorEquipManager::GetSingleton(); mgr && obj)
            mgr->UnequipObject(a_follower, obj, nullptr, 1, nullptr, true, true);
        // APMF ch.15: hands are free again -- release the equipment claim too, so
        // MFO's own EquipGateThunk re-enforces immediately if APMF is absent (no-op
        // if APMF is absent/off or no claim was ever made). This is the single choke
        // point every release path (Reconcile's release branch, Followers.cpp/
        // Scheduler.cpp teardown) funnels through.
        APMFBridge::ReleaseEquipment(id);
        spdlog::info("[equip] {:08X}: force-hold released", id);
    }

    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance, bool a_condKnownFalse) {
        if (!a_follower) return;
        // KEEP the hold while the feature is ON and an equip gambit of the forced
        // weapon's OWN category held THIS tick (a_wantStance, set by a fired-or-
        // satisfied equip). RELEASE only when we actually KNOW the condition is
        // false: a_condKnownFalse (the scan ran to completion — Evaluate returns
        // only MATCHING rules, so a scan STOPPED by a higher rule leaves the
        // equip's truth UNKNOWN; releasing then force-unequips the weapon mid-
        // fight the tick a heal/attack preempts — the SEV-2 churn). A category
        // mismatch under condKnownFalse also releases (a stale/flipped record).
        // Feature OFF -> release immediately, regardless of the scan. Decide
        // UNDER the lock, Release OUTSIDE it (Release re-locks -> no self-deadlock).
        bool release = false;
        RE::FormID heldWeaponForm = 0;   // captured under the lock; used outside it below
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(a_follower->GetFormID());
            if (it == g_forcedWeapon.end()) return;   // nothing forced -> nothing to do
            if (!Config::g_weaponStyleControl.load()) {
                release = true;                        // kill-switch: always release
            } else if (a_condKnownFalse) {
                auto* w  = it->second ? it->second->As<RE::TESObjectWEAP>() : nullptr;
                const int cat = (w && (w->IsBow() || w->IsCrossbow())) ? 2 : 1;
                release = (a_wantStance != cat);       // condition confirmed false / flipped
            }
            // else: scan stopped early -> UNKNOWN -> keep the hold this tick.
            if (!release && it->second) heldWeaponForm = it->second->GetFormID();
        }
        if (release) {
            ReleaseForcedWeapon(a_follower);
        } else if (heldWeaponForm != 0) {
            // APMF ch.15: keep the equipment claim alive for as long as the
            // force-hold holds the hands. ClaimEquipment both ENGAGES it (the
            // first reconcile tick after EquipWeapon sets g_forcedWeapon) and
            // REFRESHES it (every tick after, same "the claim call also
            // refreshes" idiom ClaimOffenseCast uses) -- so there is never a tick
            // where the hold survives but the claim is left to expire under
            // APMFBridge's expiry backstop. That backstop is FacetExpiry()
            // (round-robin-aware), not the flat kExpiry -- this reconcile call is
            // itself one round-robin lap of Scheduler::Tick, same as cast-select,
            // and a flat 500ms sweep proved it could drop a still-wanted claim
            // (Cicero deck capture, 2026-09-05).
            APMFBridge::ClaimEquipment(a_follower->GetFormID(), heldWeaponForm);
        }
    }

    void ClearForcedWeapons() {
        std::scoped_lock lk(g_forcedMx);
        g_forcedWeapon.clear();
    }

    // TASK 2 (feat/cast-gambit-concentration): drop ONE follower's firing-spell
    // gambit lock now -- the exit paths the lock's own doc comment (above
    // ConcentrationCast) enumerates beyond its own live-check/staleness release:
    // dismissal (Followers::ReleaseHeldState) and combat ending / no cast rule
    // holding (Scheduler's out-of-combat teardown + its !castSeen release,
    // alongside ReleaseOffenseCast/ComposedCast::ClearWatch -- the SAME two
    // spots the offense-cast claim itself is crisply released from). Idempotent
    // (erase-miss -> no-op), worker-serial, no lock (#4).
    void ClearCastLock(RE::FormID a_follower) {
        g_castLock.erase(a_follower);
        g_lastLockLog.erase(a_follower);
        g_lastApmfRefusal.erase(a_follower);
        // F3-7: Actuation_Direct.cpp keeps an IDENTICAL refusal-log twin in its own
        // anon namespace; drop that follower's entries here too so the two maps
        // really do share a release point instead of only claiming to.
        ClearApmfRefusalLog(a_follower);
    }

    // Revert/load: drop every follower's lock. No engine call -- the world is
    // being replaced (mirrors ClearForcedWeapons/ClearSelfCasts). Called from
    // ClearSelfCasts() (Actuation_Direct.cpp), the existing Serialization.cpp
    // revert call site, rather than adding a new one.
    void ClearCastLocks() {
        g_castLock.clear();
        g_lastLockLog.clear();
        g_lastApmfRefusal.clear();
    }

    // T#76 force-hold co-save. Persist the force-equip locks so a load clears the
    // stale ones the .ess carried (the engine's forceEquip serializes; the map
    // does not). INDEPENDENT record — its own version guard + ResolveFormID/DROP.
    constexpr std::uint32_t kMaxForcedWeapons = 64;   // party is tiny; a generous cap

    void CoSaveForcedWeapons(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecForcedWeapon, kForcedWeaponVersion)) {
            spdlog::error("[cosave] OpenRecord('FWPN') failed -- force-holds NOT saved");
            return;
        }
        // SEV-1: SNAPSHOT under the lock, then write from the copy — a lock must
        // never be held across a WriteRecordData (the MSTK/CopyStockGear rule).
        std::vector<std::pair<RE::FormID, RE::FormID>> snap;   // {followerID, weaponID | 0}
        {
            std::scoped_lock lk(g_forcedMx);
            for (const auto& [id, w] : g_forcedWeapon) {
                if (!w || !Followers::IsPersistableID(id)) continue;   // #9: never write a 0xFF follower
                const RE::FormID wid = w->GetFormID();
                // #9 for the WEAPON too: a player-enchanted/created weapon has a
                // 0xFF id that ResolveFormID passes through unresolved -> the load
                // would unequip the WRONG object and the real lock would survive.
                // Persist weapon=0; CoLoad then sweeps BOTH hands to clear it.
                snap.emplace_back(id, Followers::IsPersistableID(wid) ? wid : 0u);
            }
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(snap.size()));
        for (const auto& [id, wid] : snap) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(wid);
        }
        spdlog::info("[cosave] saved {} force-hold(s), schema v{}", snap.size(), kForcedWeaponVersion);
    }

    void CoLoadForcedWeapons(SKSE::SerializationInterface* a_intfc, std::uint32_t /*a_version*/) {
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) return;
        if (count > kMaxForcedWeapons) {
            spdlog::error("[cosave] implausible force-hold count {} -- ABORTING FWPN load", count);
            return;
        }
        std::uint32_t released = 0, dropped = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID rawFollower = 0, rawWeapon = 0;
            if (!a_intfc->ReadRecordData(rawFollower)) return;
            if (!a_intfc->ReadRecordData(rawWeapon))   return;
            RE::FormID follower = 0, weapon = 0;
            if (!a_intfc->ResolveFormID(rawFollower, follower)) { ++dropped; continue; }   // #8 DROP
            // weapon==0 means "was a created/enchanted (0xFF) weapon at save" —
            // resolve only a real id; anything unresolvable falls to the both-
            // hands sweep so a stale lock still clears.
            if (rawWeapon != 0 && !a_intfc->ResolveFormID(rawWeapon, weapon)) weapon = 0;
            // The lock lives in the actor's inventory extra-data, not the map, so
            // clear it on the follower. Defer to the main thread (the load callback
            // is off it and the actor may not be 3D-loaded yet; the force-unequip
            // is an inventory op). NO map repopulation / erase — the map is empty
            // post-revert, so touching it here would be the only cross-thread
            // access (SEV-1). A session starts hold-free; the equip gambit
            // re-forces next combat if its condition still holds.
            MainThread::Post([follower, weapon]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(follower);
                if (!actor) return;
                auto* mgr = RE::ActorEquipManager::GetSingleton();
                if (!mgr) return;
                if (weapon != 0) {
                    if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(weapon))
                        mgr->UnequipObject(actor, obj, nullptr, 1, nullptr, true, true);
                } else {
                    // Unknown weapon (created/enchanted): force-unequip the held
                    // WEAPON in either hand to clear the lock. #8: the force-hold
                    // only ever locks a weapon, but GetEquippedObject also returns
                    // spells, shields and torches -- unequipping one of those would
                    // strip an unrelated off-hand item on load. Weapons only.
                    for (bool leftHand : { false, true }) {
                        if (auto* held = actor->GetEquippedObject(leftHand))
                            if (auto* wep = held->As<RE::TESObjectWEAP>())
                                mgr->UnequipObject(actor, wep, nullptr, 1, nullptr, true, true);
                    }
                }
                spdlog::info("[equip] {:08X}: stale force-hold cleared on load", follower);
            });
            ++released;
        }
        spdlog::info("[cosave] loaded {} force-hold(s) to clear on load ({} dropped unresolvable)",
                     released, dropped);
    }

}
