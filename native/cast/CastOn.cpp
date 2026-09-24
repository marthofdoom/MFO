// cast/CastOn.cpp -- CastOn (the AI-first hybrid cast of one spell at one
// target), its APMF-refusal log, and the per-follower cast-lock clears
// (ClearCastLock / ClearCastLocks).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "Runtime.h"      // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include "Logistics_internal.h" // 2026-09-13: EquipWeapon consumes THE weapon-style decision
                          // (ComputeWeaponRoles / WeaponScore) -- not in Logistics.h, and that
                          // header was outside the change's boundary. First non-Logistics include.
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps
#include <limits>         // rank preemption: kNoRule sentinel (numeric_limits<int>::max)

namespace MFO::Actuation {

    namespace {

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

    }

        // a_rangeGate (#68): true only for an OBVIOUS target (ladder rungs
        // 1-3 -- a selector, an explicit subject, a condition-implied actor).
        // The PLAYER FALLBACK (rung 4) always passes false: a rule that
        // exists to cover "nobody obvious applies" must fire regardless of
        // distance, or the fallback itself becomes just another thing that
        // can silently not happen. kActCastSelf/kActCastPlayer never set
        // this (unchanged -- they are not part of the #68 ladder at all).
        Outcome CastOn(RE::Actor* a_follower, RE::FormID a_spellID, RE::Actor* a_target,
                       bool a_rangeGate) {
            // RUNTIME GUARD: Runtime::CastPathsVerified() -- the AE bucket (pre-
            // existing) or EXACTLY 1.5.97; VR and every other 1.5.x refused (feat/
            // mfo-1.5.97-pass, 2026-09-15). This was the T#67 AE-only gate: the mage cast-control
            // path CRASHED on Skyrim SE 1.5.97 -- a reporter's crash log pinned an
            // EXCEPTION_ACCESS_VIOLATION to Scheduler::Tick -> Actuation::Fire ->
            // CastOn on the SKSE job worker, a byte read off the poisoned pointer
            // 0x0101010101010101. Root cause, measured on both unpacked binaries:
            // Loadout::LeftHandSlot()'s BGSDefaultObjectManager::GetObject read
            // (CommonLib 3.7.0 derefs the objectInit bool array AS A POINTER at
            // +0xB80). That read is gone -- the slot is LookupByID(0x00013F43) on
            // every runtime (Docs/ADDRESS-TABLE-2026-09-15.md rows "held branch
            // Loadout.cpp:63,81 LookupByID<BGSEquipSlot>(0x00013F43)" and
            // "BGSDefaultObjectManager::objects[]/objectInit[] count": 364 SE / 366
            // AE). NOTE what that changes on 1.6.1170 too: the GetObject read there
            // returned nullptr in the field (see LeftHandSlot()), so this is the
            // first build whose spell equips carry the LeftHand slot on AE. The rest
            // of this path's 1.5.97 values are CONFIRMED in the same table: the
            // forced-cast package route (row "Packages.cpp:302-305 TESQuest::
            // ForceRefTo" 24523/25052), the CombatController reads (row
            // "attackerHandle @0x28 ... (all < 0x68)"; `combatGroup` at +0x00 is
            // CommonLib's own SE-origin declaration, not a table row), and the seats
            // a claimed cast rides on (rows "14 seat vtables", "30
            // CombatInventoryItemMagicT combos", "GetMagicTarget sret"). An
            // unverified runtime keeps the transparent decline: the follower's own
            // vanilla AI keeps casting (mobile, animated). Gate here (not just the
            // package route) so no cast-control code runs at all on a runtime the
            // table did not confirm. Mirrors CastSelfDirect / CastTargetDirect /
            // CastAuto / ComposedCast::Enabled.
            if (!Runtime::CastPathsVerified())
                return { Result::FailedOther,
                         "cast control is verified on 1.6 and 1.5.97 only (this runtime uses the follower's own AI casting)", true };
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
            // every ask **THIS FUNCTION** made lived AFTER a successful `Prepare`, so a
            // claim for a spell the actor does not know was STRUCTURALLY IMPOSSIBLE
            // *from `CastOn`*.
            //
            // SCOPE, and do not let this sentence grow (re-review 2026-09-07 -- an
            // earlier draft stated it of MFO generally, which is FALSE): it was only
            // ever true of `CastOn`'s asks. The combat `kActCastAuto` route
            // (`Actuation.cpp:~1916` -> `Actuation_Direct.cpp`'s `CastAuto`) bypasses
            // this function entirely, and `Actuation_Direct.cpp` has no `HasSpell`
            // anywhere -- so `CastSelfDirect`/`CastTargetDirect` ask APMF with no
            // competence gate on main AND here. That is a PRE-EXISTING hole of the
            // same shape, unchanged by this branch and briefed separately; it is not
            // fixed by the gate below and must not be described as though it were.
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
            //
            // BEHAVIOUR DELTA VS MAIN, deliberate and worth stating because nothing
            // else records it: sitting HERE also puts the check ahead of the SELF-CAST
            // fork (`:~787`) and the CONCENTRATION fork (`:~842`), which had no
            // competence check at all on main -- direct force does not need the spell
            // known or in hand. So a `cast_self`/concentration gambit naming a spell
            // the follower does not know now FAILS TRANSPARENTLY instead of being
            // force-applied. That is correct under DESIGN §5.4 (a rule they cannot run
            // fails and says so) and consistent with what the same gambit does through
            // every other route, but it IS a change: a list relying on MFO force-casting
            // an unknown self-spell stops working, visibly and with a reason string.
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
            // ONE lambda, because the in-flight gate below may have to run this a
            // SECOND time: if the claim its hand-pin was based on turns out to be
            // gone, the pin is void and the plan has to be re-derived from a clean
            // lock state rather than carried forward (a heal pinned to the RIGHT
            // hand by a stale lock would otherwise claim and lock the wrong hand).
            auto resolveHands = [&]() -> std::optional<Outcome> {
                if (!offenseSpell)
                    return ResolveCastHand(a_follower, Loadout::HandPick::Left, a_spellID,
                                           lockTargetKey, handPlan);
                const auto pick = Loadout::PlanCastHand(a_follower, spell,
                                                        APMFBridge::WeaponHandActive(a_follower));
                return ResolveCastHand(a_follower, pick, a_spellID, lockTargetKey, handPlan);
            };
            if (auto held = resolveHands()) return *held;

            // ── SATISFIED IN FLIGHT: REFRESH, DO NOT RE-FIRE, DO NOT END THE LAP ──
            // (F9, RC2 of Docs/DIAG-2026-09-08-field.md, 2026-09-08.)
            //
            // THE DEFECT. `HandFree` reports a hand FREE for a request that names
            // the (spell,target) already locked there -- correctly, it is the same
            // gambit refreshing itself -- so the rule ran its whole path again and
            // returned Fired, and Fired ENDS THE SCAN. A self-heal whose condition
            // stayed true therefore won every lap for 44 s: 22 consecutive
            // `fired rule 1 (act.cast_self)` lines, ZERO offense rules ever
            // reached, zero ClaimOffenseCast traffic -- while the follower's right
            // hand, which no MFO claim ever occupied, was left to its own AI (2
            // Poison Sprays, 5 Stone Runes, 3 Raise Zombies). One in-flight cast
            // monopolised the whole evaluator.
            //
            // THE FIX. A rule whose cast is ALREADY RUNNING has nothing to do this
            // lap except keep its claim alive, and that is not an action: refresh
            // the standing claim IN PLACE and return TRANSPARENT, so the scan
            // continues to the rules below (GAMBIT_FLOWS §2 -- a held gambit is
            // not a wall). The heal keeps the left hand, an offense rule below
            // gets its lap and takes the right, and with F10's floor the right
            // hand is closed rather than free whenever no offense rule wants it.
            //
            // THREE THINGS IT MUST NOT DO, all of them load-bearing:
            //   * NO DOUBLE CLAIM. RefreshOwnedCastOnHand replays the claim's OWN
            //     stored tuple through EnsureCastClaimLocked, so it lands on the
            //     unchanged fast path BY CONSTRUCTION: liveness check, lazy proxy
            //     read, TTL heartbeat, `refreshed` stamp -- and no RequestCast.
            //   * IT MUST BUMP `refreshed`. That stamp is what Tick()'s
            //     FacetExpiry sweep reads; leaving it still would have the sweep
            //     release the very claim this path exists to protect (measured
            //     exactly that way in the field -- h=9 died 2.4 s after rule 1
            //     started winning every lap, to the tenth of a second).
            //   * NO LOCK RE-STAMP. `lockHands` is deliberately NOT called here.
            //     The live claim is itself proof the lock is live (CastLockLive
            //     asks the claim first), so re-stamping would only extend the
            //     lock's staleness fallback PAST the claim's death -- a hand held
            //     by a cast that has ended.
            //
            // NOT A MASK (principle 7): a refresh that comes back not-live means
            // the claim really is gone (APMF aged it out, or refused it), and this
            // falls straight through to the normal path below, which re-claims and
            // reports its own outcome exactly as before.
            if (handPlan.inFlight) {
                const std::int32_t claimHand =
                    (handPlan.left && handPlan.right) ? APMFBridge::kApmfHandDualCast :
                    handPlan.left                     ? APMFBridge::kApmfHandLeft :
                                                        APMFBridge::kApmfHandRight;
                if (APMFBridge::RefreshOwnedCastOnHand(id, claimHand)) {
                    // KEEP THE [cfc] WATCH TICKING. ComposedCast::WatchClaim's own
                    // contract is "call every tick the caller's OWN claim call
                    // returns live" -- and on this path the refresh above IS that
                    // call. Skipping it would silence the silent-claim warning for
                    // exactly the claims it exists to catch (a claim that stands
                    // for seconds and never fires) and freeze the lazily-learned
                    // delivery-flip proxy at whatever it was on lap 1.
                    //
                    // `CastProxyOnHand`, NOT `GetOffenseCastProxy` (review fix,
                    // 2026-09-08). This gate carries HEAL claims too -- a self or
                    // ally heal is the commonest thing to be in flight here -- and
                    // a heal's proxy lives in the heal accessor. Arming with the
                    // offense accessor alone hands WatchArmed a 0 on every lap; it
                    // keeps whatever it already has, and lap 1 typically has 0
                    // because APMF mints the proxy lazily during its own Drain. The
                    // replay above DOES learn it (into the claim's own `proxy`) --
                    // the watch just never hears about it, so `observed` can never
                    // latch and every proxied heal warns "[cfc] NO observed cast"
                    // for as long as it runs. Same resolver as CastLockLive's, so
                    // the two cannot drift.
                    if (handPlan.left)
                        ComposedCast::WatchClaim(id, a_spellID, APMFBridge::kApmfHandLeft,
                                                 CastProxyOnHand(id, kHandLeft));
                    if (handPlan.right)
                        ComposedCast::WatchClaim(id, a_spellID, APMFBridge::kApmfHandRight,
                                                 CastProxyOnHand(id, kHandRight));
                    // NOTHING TO RE-STAMP HERE. Rank lives in the lock's
                    // owningRule, written once when the hand was claimed, so an
                    // in-flight refresh has no rank bookkeeping to do -- and
                    // `lastSeen` must stay frozen (F8's cap depends on it; see
                    // CastLock::claimGoneAt).
                    LogCastInFlight(id, handPlan.left ? kHandLeft : kHandRight, a_spellID,
                                    handPlan.left && handPlan.right);
                    return { Result::NoOp, "cast already in flight (claim refreshed)", true };
                }
                // THE PIN IS VOID. The refresh reports the claim is really gone --
                // APMF never published it, or the fail-closed split dropped it --
                // so the lock that named it is stale and the hand(s) it pinned this
                // request to may not be the hand(s) this request should use at all
                // (a LEFT-always heal pinned to the RIGHT by a stale right-hand
                // lock would then claim, equip and lock the wrong hand). Drop those
                // locks and re-derive the plan from scratch, then fall through to
                // the normal path -- which re-claims and reports its OWN outcome,
                // loudly if APMF refuses again (LogApmfRefusal). Nothing is masked:
                // the refusal is still made in the open, one lap later.
                if (handPlan.left)  ClearCastLockHand(id, kHandLeft);
                if (handPlan.right) ClearCastLockHand(id, kHandRight);
                handPlan = HandPlan{};
                if (auto held = resolveHands()) return *held;
            }
            // Hold BOTH resolved hands (only ever one, unless a DualCast plan
            // resolved both) on every success path below -- one lambda so the
            // ownedCast/composed-cast/self/concentration call sites all stay
            // in sync with `handPlan` rather than re-deriving it.
            // ── DISPLACE THE INCUMBENT, AS LATE AS POSSIBLE ────────────────────
            // ResolveCastHand only RECORDS that this plan needs a hand taken (see
            // HandPlan::preemptLeft/preemptRight); the release happens here, and
            // every call below sits immediately before the claim or dispatch it is
            // for. Anything that can still refuse the asker -- an unaffordable
            // self-cast, an APMF refusal, a heal ComposedCast holds off, a Prepare
            // that fails -- therefore refuses BEFORE the incumbent has been touched,
            // instead of leaving a hand nobody holds at lap rate.
            //
            // Idempotent by construction: the flags are cleared as they are spent,
            // so a path that reaches two of these calls displaces once.
            auto commitPreempt = [&]() {
                if (handPlan.preemptLeft) {
                    handPlan.preemptLeft = false;
                    PreemptHand(a_follower, kHandLeft, a_spellID);
                }
                if (handPlan.preemptRight) {
                    handPlan.preemptRight = false;
                    PreemptHand(a_follower, kHandRight, a_spellID);
                }
                // The dual-wield LEFT-hand weapon hold is deliberately NOT yielded
                // here (Fable F1 on f771399): this lambda runs BEFORE refusals that
                // still return transparently (an APMF claim refusal, a heal
                // ComposedCast holds off, a Prepare that debounces or fails), and a
                // yield ahead of a refusal costs a weapon->spell->weapon flicker per
                // lap. The hold yields at the left hand's true point of no return
                // instead: inside Loadout::Prepare, immediately before its
                // EquipSpell, through the LeftHandYield callback passed below.
            };

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
                commitPreempt();   // the claim happens inside CastSelfDirect
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
                    // Unaffordable / unverified runtime / no caster: transparent, the rules
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
                commitPreempt();   // the claim happens inside ConcentrationCast
                return ConcentrationCast(a_follower, spell, a_target);
            }

            // RESTORATION forks off HERE (fix/mfo-combat-restoration-direct,
            // 2026-09-21): a fire-and-forget heal/ward/Restoration-school cast at
            // an ally or the player takes the DIRECT road (RestorationCastDirect
            // above) and never the ch.8b AI-fired heal claim + equip/grace/force
            // machinery below. Offense keeps that road. Everything above this
            // line (range, competence, magicka reserve, the per-hand lock,
            // satisfied-in-flight) already ran. `a_target != a_follower`: a self
            // target only reaches here with bCastSelf OFF, and that dev-only
            // world keeps its old road byte-identical (the heal twin below
            // already scopes itself the same way).
            if (a_target != a_follower && IsRestorationSpell(spell)) {
                commitPreempt();   // the hand is taken inside CastTargetDirect
                return RestorationCastDirect(a_follower, spell, a_target);
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
            // fCastCooldown NO LONGER PACES AN APMF-OWNED CAST, AND THAT IS CORRECT.
            // SETTLED -- marth 2026-09-07, memory `cast-cooldown-inert-is-correct`.
            // DO NOT re-raise this as a regression, and do NOT "restore" it.
            //
            // WHAT ACTUALLY CHANGED, precisely (verified by reading the readers, not
            // inferred from this path -- the first draft of this note over-claimed
            // that the knob "stops limiting anything", which is FALSE):
            // `CasterConsent::ClientCastClaimed` (`CasterConsent.cpp:231-235`)
            // early-passes while an APMF cast claim is live, so MFO's deny thunks
            // never compute a verdict -- which means the pacing deny at
            // `CasterConsent.cpp:762-775` cannot fire for a follower holding a claim.
            // On main the claim went un-refreshed through a cooldown, `FacetExpiry`
            // swept it, and that deny re-engaged for the tail. Here the claim is
            // refreshed throughout, so it does not. **That ONE effect is what went
            // away.**
            //
            // WHAT THE KNOB STILL GOVERNS -- it is NOT inert generally:
            //   * the APMF-ABSENT path, and ONLY that one: no claim of any kind, so
            //     no early-pass, so the pacing deny bites exactly as before.
            //     `bLegacyCastHybrid` / `bApmfCast`-off do NOT belong on this line --
            //     an earlier draft listed them and was wrong (re-review 2026-09-07).
            //     Those two gate `ownedCast` only. `ComposedCast::Enabled`
            //     (`ComposedCast.cpp:38-45`) gates on Heal + AE + `bHealAnimPackage` +
            //     `APMFBridge::Available()` and reads NEITHER of them, and the heal
            //     `else if` in the pre-flight below runs whenever `ownedCast` is false
            //     -- so with APMF present and `bHealAnimPackage` on, a heal gambit
            //     still mints a claim on those paths, `IsHealCastActive` answers true,
            //     and `ClientCastClaimed` stands the deny down exactly as on the owned
            //     path. Concretely: `bLegacyCastHybrid` ON + APMF present +
            //     `bHealAnimPackage` ON + `cast_target Healing Hands` at an ally inside
            //     a cooldown -- the OFFENSE ask is gone, the HEAL ask is not;
            //   * `Loadout::StartCooldown` still stamps (from the `[cast]` SpellSink,
            //     `Diagnostics.cpp:263`, and from this file's own cast sites) and
            //     still `ReleaseSpell`s -- the spell is taken back either way;
            //   * `Prepare` still returns `Debounced`, so MFO still does not re-equip
            //     inside the window;
            //   * the direct-force FF apply beats (`Actuation_Direct.cpp:989`, `:1282`)
            //     and `CastAuto`'s broadcast interval (`:1509`) are untouched.
            //
            // WHY LOSING IT ON THE OWNED PATH IS RIGHT. marth, verbatim: *"the cast
            // cool down is currently irrelevent as you say. a cast can only cast as
            // fast as a cast. No reason to be slower."* The engine's own equip ->
            // charge -> fire pipeline (~2.3-2.5s measured for an offense cast, 2.95s
            // claim-to-observed on the one heal that landed) already paces casting at
            // the fastest a cast can physically occur, so a cooldown stacked on top can
            // only make a follower SLOWER than the engine allows -- it cannot make
            // anything safer. Losing it there is **the absence of a redundant limiter,
            // not a lost guard**, and gating the ask on `Loadout::CoolingDown` would
            // re-add the redundancy. Reviving it needs a NEW justification (a magicka
            // economy, or a thrash the engine itself does not bound) -- never "it used
            // to fire".
            //
            // KEEP THIS SEPARATE FROM THE HAND LOCK. The cooldown not pacing an owned
            // cast is fine. A claim standing WITHOUT its hand lock is not: that is a
            // different defect with its own fix in the Debounced arm below (SEV-2),
            // about the missing LOCK, not about the cooldown.
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
                    commitPreempt();
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
                    commitPreempt();
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
                // LAST COMMIT POINT, for the APMF-ABSENT world. With no claim to
                // make, `Loadout::Prepare` putting the spell in the hand IS this
                // path's claim on it -- so the incumbent is displaced here and not
                // before, and every transparent refusal above still refuses without
                // having touched it. A no-op when the plan asked for no displacement,
                // and a no-op on the owned path (already spent above).
                commitPreempt();
                // The LEFT-hand weapon hold yields INSIDE Prepare, right before its
                // EquipSpell (F1): after Prepare's own refusals, never before them.
                // Prepare always equips into the LEFT slot, so this is keyed to the
                // physical hand it takes, not to handPlan.left. Returns whether a
                // hold was released, so Prepare books no gear debt for it (F2).
                switch (Loadout::Prepare(a_follower, spell, why, [](RE::Actor* a_actor) {
                            return YieldForcedLeftHand(a_actor, "a cast is taking the left hand");
                        })) {
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

                        // APMF was never asked (non-Heal kind / unverified runtime / toggle off / APMF
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
                    // above, the only Failed path that can still be reached holding a
                    // claim is a null EQUIP manager -- `RE::ActorEquipManager::
                    // GetSingleton() == nullptr` in `Loadout.cpp` -- which is
                    // negligible and self-clearing.)
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
        g_lastPreemptLog.erase(a_follower);   // rank-preemption twin of g_lastLockLog
        g_lastInFlightLog.erase(a_follower);   // F9: the in-flight twin of g_lastLockLog
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
        g_lastPreemptLog.clear();
        g_lastInFlightLog.clear();   // F9
        g_lastApmfRefusal.clear();
    }

}
