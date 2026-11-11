// scheduler/CombatScan.cpp -- Scheduler::Tick: the per-follower service lap with
// the COMBAT SCAN (the Eval::Evaluate walk into Actuation::Fire), its
// RECONCILIATION (stance, silver, forced weapon, heal sustain, cast release) and
// the outcome bookkeeping, plus the two reconciliation helpers only the lap uses
// (MeleeClampDwell, EquipRangeUndecidable). Runs on the AddTask job worker (#4).
// Split out of native/Scheduler.cpp by IAUS card I0 (2026-11-10): a pure move,
// proven function by function with tools/splitcheck.
#include "PCH.h"
#include "Scheduler.h"
#include "scheduler/Scheduler_internal.h"
#include "scheduler/ChoiceSource.h"   // IAUS card I1: the combat scan reads its choices through the seam
#include "Evaluator.h"
#include "cast/Actuation.h"
#include "Followers.h"
#include "Config.h"
#include "Loadout.h"
#include "apmf/APMFBridge.h"   // crisp release of owned-cast claims when no cast rule holds
#include "ComposedCast.h" // ClearWatch -- drop the shared [cfc] silent-claim watch alongside it
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "Packages.h"
#include "Vocabulary.h"
#include "logistics/Logistics.h"
#include "logistics/Errand.h"   // engage in a party fight: not while he is away on a town errand
#include "State.h"
#include "Cooldowns.h"      // card G9: the per-rule cooldown last-fired store (runtime-only)
#include "Confidence.h"   // retreat probe: the fill gate reads Of()
#include "CombatSense.h"  // party combat: the [sense] foe tally (FoeCount), reused, not re-scanned
#include "Forms.h"        // retreat probe: g_retreatPackage for the onPkg readout
#include "Targeting.h"    // flair #5: retarget hesitation reads the current latch
#include "Temperament.h"  // flair #1: per-follower timing seed
#include "Rapport.h"      // #63 quash backstop routes through QuashAllyPair
#include "TauntProbe.h"    // taunt S0 passive probes (bTauntProbe)
#include "Taunt.h"         // taunt S2: Taunt::Tick (every pump) / Clear
#include "EngageOnSight.h" // the hidden OOC gambit "nearest visible enemy" (party-OOC branch)
#include "progression/ProgAllocator.h" // §HMS: publish fired combat action pool for the level-up skew
#include "selftest/SelfTest.h" // dev builds only: the self-test runner tick (an empty inline in release)
#include <cstring>          // [deadtgt] strcmp on the CannotActReason strings
#include <unordered_set>   // T#78: MFO-OFF one-time-release latch (g_mfoDisabledSwept)

namespace MFO::Scheduler {

    namespace {

        // The dwell is the CENTER of a Temperament band (flair #1), not a fixed
        // value: a deliberate follower commits to the melee a beat longer, an
        // eager one shorter -- each consistently themselves, so the party's swaps
        // never pulse in robotic unison. MeleeClampDwell(id) = 3.0s +/- 1.0s.
        constexpr float kMeleeClampDwellCenter = 3.0f;   // seconds, MCM-band center
        constexpr float kMeleeClampDwellSpread = 2.0f;   // full width -> [2.0, 4.0]s
        [[nodiscard]] float MeleeClampDwell(RE::FormID a_id) {
            return kMeleeClampDwellCenter + (Temperament(a_id) - 0.5f) * kMeleeClampDwellSpread;
        }

        // MFO-B55 (FIXED 2026-09-21, Deck/Fable: Cicero's two mid-fight weapon
        // vanishes at 21:57:34.226 and 21:57:44.075 were this dwell releasing a
        // foe-keyed hold while his group still had foes). The equip range rules
        // (cond.foe_within_range / cond.foe_beyond_range) are TARGET-relative:
        // the Evaluator reads the follower's currentCombatTarget and returns
        // "no match" when that handle is null, dead, disabled or non-hostile
        // (Evaluator.cpp PickFoe, the target-relative branch). The engine nulls
        // currentCombatTarget on a LoS loss / a group retarget while the combat
        // GROUP still lists live, known foes -- so on those laps the rule's
        // condition is not FALSE, it is UNDECIDABLE: the foe never left range,
        // the follower merely has no target to measure against this instant.
        // Reading it as known-false let the T#76 dwell (2-4 s) time out on an
        // engine-side null and ReconcileForcedWeapon release the hold.
        //
        // Returns true when an equip rule whose condition is target-relative
        // was EVALUATED this lap (index below the scan's stop; the whole table
        // when nothing matched) while the follower's target is unusable AND his
        // own combat group still has foes (CombatSense::FoeCount: live,
        // enabled, not kTargetLost -- the same tally the [sense] line and the
        // party gate read). The caller then treats the equip truth as UNKNOWN:
        // the dwell clock refreshes, equipCondKnownFalse stays false, and the
        // hold survives exactly as it does under a higher rule's stop. A
        // genuinely empty group (FoeCount 0: every foe dead, gone or lost) or a
        // real party-OOC still reads false and releases through the dwell /
        // the party-OOC debounce as before. Pure reads; the group lock is taken
        // inside FoeCount with no other lock held (the Evaluator's nesting
        // warning).
        [[nodiscard]] bool EquipRangeUndecidable(RE::Actor* a_self, const std::vector<Gambit>& a_rules, int a_stopIdx) {
            bool rangeRuleEvaluated = false;
            for (int i = 0; i < static_cast<int>(a_rules.size()) && i < a_stopIdx; ++i) {
                const auto& r = a_rules[i];
                if (r.actionOpcode != Vocab::kActEquipMelee && r.actionOpcode != Vocab::kActEquipRanged) continue;
                if (r.conditionOpcode == Vocab::kCondFoeWithinRange || r.conditionOpcode == Vocab::kCondFoeBeyondRange) {
                    rangeRuleEvaluated = true;
                    break;
                }
            }
            if (!rangeRuleEvaluated) return false;
            auto  tp  = a_self->GetActorRuntimeData().currentCombatTarget.get();
            auto* tgt = tp.get();
            const bool targetUsable = tgt && !tgt->IsDead() && !tgt->IsDisabled() && tgt->IsHostileToActor(a_self);
            if (targetUsable) return false;            // the rule measured a real target: its truth is real
            return CombatSense::FoeCount(a_self) > 0;  // no target, foes remain -> undecidable
        }

        // THE TABLE SOURCE (IAUS card I1): the gambit table behind the ChoiceSource seam. Each method is a row
        // read the pre-I1 scan made inline on `ruleIndex`, moved here with the same guards; rank IS the row
        // index for a table choice (Rank::OfTableRow). Docs/research/2026-11/iaus-i1-seam.md lists every read.
        class TableSource final : public ChoiceSource {
        public:
            TableSource(RE::Actor* a_self, RE::FormID a_id) : m_self(a_self), m_id(a_id) {}

            [[nodiscard]] Eval::Source Kind() const override { return Eval::Source::Table; }

            bool Next(Eval::Choice& a_out) override {
                const auto rec = g_followers.find(m_id);
                if (rec == g_followers.end()) return false;
                a_out = Eval::Evaluate(m_self, rec->second, Table::Combat, m_start);
                return true;
            }

            void Reject(const Eval::Choice& a_choice) override { m_start = Rank::NextTableRow(a_choice.rank); }

            // #11: re-anchor the positional suppression window to the fired rule's IDENTITY. Suppression is
            // positional (INVARIANTS #26), but the anchor is the rule that FIRED, not a frozen index -- a board
            // Del/Move during the window shifts every index, so a stored index would suppress the wrong rule.
            // Re-resolve the fired uid to its CURRENT index each tick; if the rule was deleted, the window is void.
            [[nodiscard]] int SuppressionAnchor(bool a_inWindow, int a_firedRank,
                                                std::uint32_t a_firedIdentity) const override {
                int anchor = a_firedRank;   // fallback for a pre-#11 (uid==0) entry
                if (a_inWindow && a_firedIdentity != 0) {
                    anchor = Rank::kNone;
                    if (const auto rr = g_followers.find(m_id); rr != g_followers.end()) {
                        const auto& tbl = rr->second.combat();
                        for (int i = 0; i < static_cast<int>(tbl.size()); ++i)
                            if (tbl[i].uid == a_firedIdentity) { anchor = Rank::OfTableRow(i); break; }
                    }
                }
                return anchor;
            }

            [[nodiscard]] std::uint32_t FiredIdentity(const Eval::Choice& a_choice) const override {
                if (const Gambit* rule = Row(a_choice)) return rule->uid;
                return 0;
            }

            [[nodiscard]] std::string CoolingReason(const Eval::Choice& a_choice,
                                                    std::chrono::steady_clock::time_point a_now) const override {
                std::string coolReason;
                if (const Gambit* rule = Row(a_choice))
                    if (rule->cooldownTenths != 0 && rule->uid != 0)
                        if (const float left = Cooldowns::Remaining(m_id, Cooldowns::Kind::Rule, rule->uid, CooldownSec(*rule), a_now);
                            left > 0.0f)
                            coolReason = Cooldowns::CoolingReason(left);
                return coolReason;
            }

            [[nodiscard]] bool HesitationExempt(const Eval::Choice& a_choice) const override {
                const Gambit* rule = Row(a_choice);
                if (!rule) return false;
                const auto& cop = rule->conditionOpcode;
                return cop == Vocab::kCondFoeAttackingMe ||
                       cop == Vocab::kCondFoeAttackingMeMelee ||
                       cop == Vocab::kCondFoeAttackingMeRanged;
            }

            void NoteSkipReason(const Eval::Choice& a_choice, const std::string& a_reason) override {
                if (Gambit* rule = Row(a_choice)) rule->lastFailReason = a_reason;
            }

            void Record(const Eval::Choice& a_choice, const Actuation::Outcome& a_outcome,
                        std::chrono::steady_clock::time_point a_now) override {
                if (Gambit* rule = Row(a_choice)) {
                    // Display only -- the evaluator never reads these back (#22).
                    rule->lastFired = (a_outcome.result == Actuation::Result::Fired);
                    rule->lastFailReason = a_outcome.reason;
                    if (rule->lastFired) rule->lastFiredAt = a_now;   // flair #9: board pulse
                    // Card G9: a real FIRE starts this rule's cooldown. Not an approach leg (Outcome::approach: the
                    // power attack closing to melee), or the swing it is closing for would be cooled out.
                    if (rule->lastFired && !a_outcome.approach && rule->cooldownTenths != 0 && rule->uid != 0)
                        Cooldowns::NoteFired(m_id, Cooldowns::Kind::Rule, rule->uid, a_now);
                }
            }

            // POSITIONAL (Fable SEV-2 2026-08-17): Eval returns the FIRST match from index 0, so EVERY row above
            // the stop was evaluated this lap and did not hold.
            [[nodiscard]] bool EquipEvaluatedAboveStop(const Eval::Choice& a_stop) const override {
                if (const auto r2 = g_followers.find(m_id); r2 != g_followers.end()) {
                    const auto& rules = r2->second.combat();
                    const int stopIdx = Rank::TableStop(a_stop.rank, static_cast<int>(rules.size()));
                    for (int i = 0; i < static_cast<int>(rules.size()) && i < stopIdx; ++i) {
                        const auto& aop = rules[i].actionOpcode;
                        if (aop == Vocab::kActEquipMelee || aop == Vocab::kActEquipRanged) return true;
                    }
                }
                return false;
            }

            [[nodiscard]] bool SilverEvaluatedAboveStop(const Eval::Choice& a_stop) const override {
                if (const auto r2 = g_followers.find(m_id); r2 != g_followers.end()) {
                    const auto& rules = r2->second.combat();
                    const int stopIdx = Rank::TableStop(a_stop.rank, static_cast<int>(rules.size()));
                    for (int i = 0; i < static_cast<int>(rules.size()) && i < stopIdx; ++i)
                        if (rules[i].actionOpcode == Vocab::kActEquipSilver) return true;
                }
                return false;
            }

            [[nodiscard]] bool UnholySilverEvaluated(const Eval::Choice& a_stop) const override {
                if (const auto r4 = g_followers.find(m_id); r4 != g_followers.end()) {
                    const auto& rules = r4->second.combat();
                    const int stopIdx = Rank::TableStop(a_stop.rank, static_cast<int>(rules.size()));
                    for (int i = 0; i < static_cast<int>(rules.size()) && i < stopIdx; ++i)
                        if (rules[i].actionOpcode == Vocab::kActEquipSilver &&
                            rules[i].conditionOpcode == Vocab::kCondFoeIsUnholy) return true;
                }
                return false;
            }

            [[nodiscard]] bool EquipRangeUndecidable(RE::Actor* a_self, const Eval::Choice& a_stop) const override {
                if (const auto r3 = g_followers.find(m_id); r3 != g_followers.end()) {
                    const auto& rules = r3->second.combat();
                    const int stopIdx = Rank::TableStop(a_stop.rank, static_cast<int>(rules.size()));
                    return ::MFO::Scheduler::EquipRangeUndecidable(a_self, rules, stopIdx);   // the free helper above
                }
                return false;
            }

        private:
            // The row a table choice names, or nullptr (no choice, record gone, or the row no longer exists).
            [[nodiscard]] Gambit* Row(const Eval::Choice& a_choice) const {
                if (!Rank::IsChoice(a_choice.rank)) return nullptr;
                const auto rr = g_followers.find(m_id);
                if (rr == g_followers.end() || a_choice.rank >= static_cast<int>(rr->second.combat().size())) return nullptr;
                return &rr->second.combat()[a_choice.rank];
            }

            RE::Actor* m_self;
            RE::FormID m_id;
            int        m_start = 0;   // Evaluate's a_startIndex: the walk resumes past a rejected row
        };

        // ONE LAP'S WALK of the combat scan (card I1 gave it its own function). Its outputs, read by the
        // reconciliation and the bookkeeping in Tick:
        struct ScanOut {
            bool castSeen   = false;   // H3: some cast rule's condition held this tick
            std::vector<int> matchedCast;   // the cast rules whose condition held this tick (incl. suppressed / cannot-act): HealSustainLap's "the rule is still asking"
            int  handClaim  = 0;       // H2: 0 = none, 1 = melee, 2 = ranged
            int  wantStance = 0;       // combat-style ownership: the equip stance that won
            // T#76: the equip gambit's CONDITION held this tick (0=none,1=melee,
            // 2=ranged). Drives the force-hold / stance-clamp release lifecycle.
            // Superset of wantStance: like castSeen (H3) it is ALSO set when the
            // suppression window stops the scan AT the equip rule -- so the ticks
            // right after an equip fires (its own window) do NOT read as "condition
            // false" and yank the weapon. equipHeld == 0 after the scan == the
            // condition went false / was superseded -> release both holds.
            int  equipHeld  = 0;
            // SILVER (ClickUp 86e3p2gp9): the act.equip_silver rule's condition held this tick (same
            // four sites as equipHeld). It is a melee equip for everything above (category 1), and its
            // own switch-back to the main weapon reads this (Actuation::ReconcileSilver, below).
            bool silverHeld = false;
            bool stopped    = false;   // scan ended on a Fired / opaque hold
            bool suppressed = false;   // scan stopped at the suppression window
            std::string chain;         // 1.4: skip-chain, e.g. "0(no potion),2(satisfied)"
            Eval::Choice choice;
            Actuation::Outcome outcome;
        };

        // The walk itself: try the source's choices in order until one produces a REAL outcome (THE COMBAT SCAN
        // in Tick says why). Same gates, same order, same chain text as before I1; only the row reads went to a_src.
        void Scan(ChoiceSource& a_src, RE::Actor* f, RE::FormID id, std::chrono::steady_clock::time_point now,
                  const char* cannotAct, bool windowActive, int firedRuleNow, ScanOut& a_out) {
            auto& castSeen = a_out.castSeen;
            auto& matchedCast = a_out.matchedCast;
            auto& handClaim = a_out.handClaim;
            auto& wantStance = a_out.wantStance;
            auto& equipHeld = a_out.equipHeld;
            auto& silverHeld = a_out.silverHeld;
            auto& stopped = a_out.stopped;
            auto& suppressed = a_out.suppressed;
            auto& chain = a_out.chain;
            auto& choice = a_out.choice;
            auto& outcome = a_out.outcome;

            for (;;) {
                // The source re-finds the record each pass (INVARIANTS #2): a transparent
                // outcome may still have dispatched engine events (a failing cast
                // self-releases the spell), so never hold an iterator across one.
                if (!a_src.Next(choice)) break;           // the record is gone: the last choice stands
                if (!Rank::IsChoice(choice.rank)) break;  // nothing (more) matched

                const auto& op = choice.actionOpcode;
                // A SUMMON rule is not a cast channel (fix/mfo-summon-oneshot): it holds
                // no hand and no claim, so it must not keep the H3 cast loan alive
                // (castSeen) or be passed as castFacetHeld to the OOC service.
                const bool isCast  = (op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                                      op == Vocab::kActCastPlayer) &&
                                     !Actuation::IsSummonSpell(
                                         RE::TESForm::LookupByID<RE::SpellItem>(choice.actionParam));
                const bool isSilver = op == Vocab::kActEquipSilver;   // a melee equip (category 1) everywhere below
                const bool isEquip = op == Vocab::kActEquipMelee || op == Vocab::kActEquipRanged || isSilver;

                // SUPPRESSION IS POSITIONAL, NEVER ABSOLUTE (INVARIANTS #26) --
                // and it survives the scan unchanged: during a window the scan
                // stops COLD when the next candidate reaches the fired rule's
                // index; it never scans past it. Rules ABOVE the fired rule are
                // still tried first (transparent ones fall through as usual), so a
                // dying follower's rule-1 heal preempts exactly as before.
                if (Rank::StopsAtAnchor(windowActive, choice.rank, firedRuleNow)) {
                    if (isCast) { castSeen = true; matchedCast.push_back(choice.rank); }   // its condition held: keep the loan (H3)
                    // T#76: same shape for the equip force-hold -- the equip rule's
                    // condition held this tick (that is why Evaluate returned it),
                    // the scan just stopped at it inside its own suppression window.
                    // Keep the force-hold; do NOT read this as "condition false".
                    if (isEquip) equipHeld = (op == Vocab::kActEquipRanged) ? 2 : 1;
                    silverHeld = silverHeld || isSilver;
                    suppressed = true;
                    break;
                }

                // H2 -- THE HAND CLAIM. A satisfied equip rule above has already
                // decided this tick's weapon category; a lower CONTRADICTORY equip
                // is skipped WITHOUT firing, whatever its condition says. This is
                // first-match-wins applied to the hand as a resource -- without it
                // the fall-through would re-manufacture the melee<->ranged thrash
                // in mixed packs (D4). Per marth (§7.2) the claim binds only
                // equip-vs-equip: a cast rule below still fires (its off-hand
                // borrow is a loan, not a contradiction -- spellsword lists work).
                // Per-scan state only; nothing persists across ticks (#22).
                if (isEquip && handClaim != 0) {
                    const int cat = (op == Vocab::kActEquipRanged) ? 2 : 1;
                    if (cat != handClaim) {
                        chain += std::format("{}{}(hand claimed)",
                                             chain.empty() ? "" : ",", choice.rank);
                        a_src.Reject(choice);
                        continue;
                    }
                }

                // H3: the condition of a cast rule held this tick -- whether it
                // now fires, waits out its grace, sits debounced, or fails (the
                // failure paths self-release internally). Only a tick where NO
                // cast condition held releases the loan, after the loop.
                if (isCast) { castSeen = true; matchedCast.push_back(choice.rank); }

                // HE CANNOT ACT (fix/mfo-can-act): bleeding out, down, knocked down,
                // paralysed or in a kill move. The rule is passed over TRANSPARENTLY and
                // never reaches Fire -- no cast, drink, latch or swing. Its condition DID
                // hold, so it is treated like the suppression-window stop above: a cast
                // rule keeps the loan (castSeen, set just above), an equip rule keeps its
                // hold (equipHeld), so a knockdown does not strip a weapon or a claim that
                // his next standing lap needs. The skip-chain line is deduped, so a stable
                // down logs once.
                if (cannotAct) {
                    if (isEquip) equipHeld = (op == Vocab::kActEquipRanged) ? 2 : 1;
                    silverHeld = silverHeld || isSilver;
                    chain += std::format("{}{}(cannot act: {})", chain.empty() ? "" : ",",
                                         choice.rank, cannotAct);
                    a_src.Reject(choice);
                    continue;
                }

                // CARD G9: PER-RULE COOLDOWN (Gambit::cooldownTenths; the last-fired store is Cooldowns.h, runtime-only).
                // A rule still cooling down is passed over TRANSPARENTLY and never reaches Fire, its reason going to the
                // chain and to the board the G10 way (the transparent branch below). Actuation-side pacing, the same
                // shape as the drink / cast cooldowns: the evaluator chose this rule and is not told otherwise (#22).
                // FailedOther, never NoOp: a transparent NoOp on an equip rule reads as "satisfied" (the H2 claim).
                // Like the cast cooldown, it runs after H3 above, so a cooling cast rule still holds its loan.
                const std::string coolReason = a_src.CoolingReason(choice, now);

                // ── FLAIR #5: RETARGET HESITATION ────────────────────────────────
                // A switch of an existing latch must win twice: first winning
                // service proposes and holds ("sizing up <name>" -- an opaque
                // beat, like the attack latch itself); the second service that
                // proposes the same foe commits. First engagement is exempt (no
                // latch yet), and so is self-defense (foe_attacking_me*).
                if (coolReason.empty() && op == Vocab::kActAttack && choice.target) {
                    const auto cur = Targeting::Current(id);
                    // Only a LIVE current target earns the hesitation beat. A stale
                    // latch to a DEAD/gone foe must not stall a switch (Opus review:
                    // an oscillating winner + a dead latch would leave the follower
                    // stuck on a corpse, never re-engaging) -- resolve it, and if it
                    // is not a living actor, commit the switch immediately below.
                    auto curPtr = cur.get();
                    auto* curActor = curPtr.get();
                    const bool curLive = curActor && !curActor->IsDead() && !curActor->IsDisabled();
                    if (curLive && !(cur == choice.target)) {
                        const bool exempt = a_src.HesitationExempt(choice);   // self-defence (foe_attacking_me*)
                        if (!exempt) {
                            auto& prop = g_proposedTarget[id];
                            if (!(prop == choice.target)) {
                                prop = choice.target;
                                auto tp = choice.target.get();
                                outcome = { Actuation::Result::NoOp,
                                            std::format("sizing up {}",
                                                        tp && tp->GetName() ? tp->GetName() : "?") };
                                stopped = true;   // opaque: consumes the tick, once
                                break;
                            }
                            g_proposedTarget.erase(id);   // won twice -> commit below
                        }
                    } else {
                        g_proposedTarget.erase(id);       // first latch / same foe
                    }
                }

                // COMBAT-START TIMING (field 2026-10-06 14:25:25, marth: "Its only in combat gambits, so him casting
                // it out of combat is a timing issue"): a combat-table cast whose follower is in combat by MFO's own
                // reckoning but whose combat controller is not up yet must WAIT, transparently, never take the
                // out-of-combat direct / fan road (unanimated, all targets at once). Once the controller is up the
                // normal animated claim road applies. Logistics' OOC casts and the retreat self-heal keep their
                // direct road (they do not pass through here).
                const bool castOp = op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                                    op == Vocab::kActCastPlayer;
                if (!coolReason.empty())
                    outcome = { Actuation::Result::FailedOther, coolReason, true };
                else if (castOp && APMFBridge::InCombat(f) && !f->GetActorRuntimeData().combatController)
                    outcome = { Actuation::Result::NoOp,
                                "waiting for his combat controller (a combat gambit's cast never takes the direct road)",
                                true };
                else
                    outcome = Actuation::Fire(f, choice);

                if (outcome.transparent) {
                    // A satisfied equip (transparent NoOp on an equip action) is
                    // the H2 claim; every other transparent outcome just records
                    // its reason in the chain and the scan moves on.
                    const bool satisfiedEquip =
                        isEquip && outcome.result == Actuation::Result::NoOp;
                    if (satisfiedEquip) handClaim = wantStance = equipHeld = (op == Vocab::kActEquipRanged) ? 2 : 1;
                    if (satisfiedEquip) silverHeld = silverHeld || isSilver;
                    chain += std::format("{}{}({})", chain.empty() ? "" : ",",
                                         choice.rank,
                                         satisfiedEquip        ? "satisfied"
                                         // G9: no countdown in the chain, or its dedup'd log line changes every second
                                         : !coolReason.empty() ? "cooling down"
                                                               : outcome.reason.c_str());
                    // G10: a skipped rule's "will not work" reason reaches the board (lastFailReason is otherwise written for the stopping rule only).
                    // G9's "cooling down (Ns)" takes the same road.
                    if (outcome.reason.starts_with("will not work:") || outcome.reason.starts_with(Cooldowns::kCoolingPrefix))
                        a_src.NoteSkipReason(choice, outcome.reason);
                    a_src.Reject(choice);
                    continue;
                }

                // A FIRED equip is the winning stance too (the satisfied case above
                // handles an already-correct hand; this is the real swap).
                if (isEquip && outcome.result == Actuation::Result::Fired) {
                    wantStance = equipHeld = (op == Vocab::kActEquipRanged) ? 2 : 1;
                    silverHeld = silverHeld || isSilver;
                }

                stopped = true;                    // Fired or opaque hold -> done
                break;
            }
        }

    }

    void Tick() {
        const auto now = std::chrono::steady_clock::now();

        // NEVER CATCH UP (INVARIANTS #24). Fire at most once per wake and reset
        // the anchor to now -- a `last += interval` loop turns a load screen
        // into a burst of queued evaluations on the most loaded frame there is.
        // Tolerance: the pump paces at kTickInterval, but this anchor is set at
        // TASK-EXECUTION time and AddTask latency jitters. Without slack, a wake
        // landing a few ms early is skipped entirely and the gap silently
        // doubles to 266 ms -- safe in direction, but it halves response speed
        // at random. The pump is the only caller; it does the real pacing.
        constexpr auto kSlack = std::chrono::milliseconds(10);
        if (g_lastTick.time_since_epoch().count() != 0 && (now - g_lastTick) < (kTickInterval - kSlack)) return;
        g_lastTick = now;

        // OBSERVE THE PACKAGE STATE MACHINE FIRST, and unconditionally.
        //
        // It must run BEFORE every early return below, because the conditions
        // those returns test are exactly the conditions under which a
        // commanded action needs releasing: the party emptied, the follower
        // left combat, the game paused, the record vanished. Pump() is the only
        // thing that advances Requested -> Filled -> Running -> Done, and the
        // fill it is watching is engine state that OUTLIVES the session -- so a
        // tick that returns early without pumping is a tick that can strand a
        // latch in the save.
        Packages::Pump();

        // TAUNT (S2, ClickUp 86e3g6duw): advance every taunt attempt (idle confirmation) and every pinned foe (the
        // duration and the early ends) EVERY pump, before the early returns: a taunt must release the same tick its
        // taunter goes down or the party empties, and none of that is a follower service. Pauses with the game.
        Taunt::Tick();
        SelfTest::Tick();   // dev builds only (native/selftest/, Docs/SELFTEST.md); before the empty-party return: it also
                            // removes fixtures a load brought in. An empty inline in a release build.

        auto& active = Followers::g_active;
        if (active.empty()) return;

        const auto t0 = std::chrono::steady_clock::now();
        ++g_ticks;

        // PARTY COMBAT, once per tick, before the round-robin picks anyone: the
        // player's flag, every managed active follower's flag, and -- for the
        // followers whose flag is TRUE this lap -- the [sense] foe tally
        // (CombatSense::FoeCount -- the follower's own combat group, the same
        // read the [sense] line prints; there is no radius in that scan and
        // none is added here). O(party) vfunc reads plus one combat-group
        // read-lock per FIGHTING follower; taken with NO other lock held, so it
        // cannot nest the group lock the Evaluator's scan warns about. Dead /
        // disabled followers are skipped exactly as the service below skips
        // them. Transition-only log (#79): one line per ON/OFF edge.
        {
            bool player = false;
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsInCombat())
                player = true;
            int followersFighting = 0;
            int foes = 0;
            int substrate = 0;
            std::unordered_set<RE::FormID> substrateNow;
            std::unordered_set<RE::FormID> leakNow;
            for (const auto& h : active) {
                auto  ptr = h.get();          // HOLD the NiPointer across the group read
                auto* a   = ptr.get();
                if (!a || a->IsDead() || a->IsDisabled()) continue;
                // CONTROLLER LIFETIME (tier-A carve-out, Fable field diagnosis
                // 2026-09-21; evidence in ENGINE_NOTES "CombatController lifetime"
                // and the commit): Actor::combatController is a raw, unguarded
                // pointer that Actor::StopCombat (vtable slot 0xE5) frees INLINE
                // and nulls, and StopCombat runs on BSJobs WORKER threads from
                // the "Combat and magic" frame job (UpdateCombat's only call
                // site is a job callback) plus 30-odd other virtual call sites
                // (EvaluatePackage, KillImmediate, the Papyrus native). There is
                // no refcount and no lock on it; the engine gives this worker
                // nothing to hold it with. IsInCombat() itself dereferences it
                // (`cc && !cc->inactive`), so every IsInCombat() read MFO has
                // ever made carries the same exposure; what this block added
                // was a SECOND deref -- `cc->combatGroup->lock`, a read-lock
                // acquire on the group -- for every follower every 133 ms, in
                // AND out of combat, i.e. through every controller teardown and
                // rebuild (the engine rebuilt both fighting followers' at
                // 21:57:34.9). Gate the group read on a same-lap IsInCombat():
                // that is the pre-build shape (FoeCount ran only inside the
                // combat branch), it drops the out-of-combat group read to
                // zero, and it leaves the in-combat one where every [sense] /
                // Evaluator read already is. The `foes` term of the boolean is
                // therefore implied by followersFighting; it still feeds the
                // transition line. A real fix (an atomic controller epoch
                // published from a StopCombat seat) is an engine seat for its
                // own brief.
                if (a->IsInCombat()) {
                    // A Harbinger COMBAT SUBSTRATE (ABI v26) is not a fight: not party combat, and its group (the
                    // anchor) is never read for foes. Real combat inside the window flips SubstrateOnly false at once.
                    if (APMFBridge::SubstrateOnly(a->GetFormID())) {
                        ++substrate;
                        substrateNow.insert(a->GetFormID());
                        if (g_substrateNoted.insert(a->GetFormID()).second)
                            spdlog::info("[sched] {:08X} is in a Harbinger combat SUBSTRATE: MFO treats him as OUT of "
                                         "combat (no party combat, no combat HUD, no combat style, no combat table) "
                                         "until it closes or real combat starts", a->GetFormID());
                        continue;
                    }
                    // A teammate whose ONLY target is Harbinger's anchor (a leak, not a fight): not party combat, no
                    // combat table (APMFBridge::InCombat is false for him), no style (Targeting hook). Never masked:
                    // one loud ERROR per episode.
                    if (CombatSense::AnchorOnlyLeak(a)) {
                        leakNow.insert(a->GetFormID());
                        if (g_anchorLeakNoted.insert(a->GetFormID()).second)
                            spdlog::error("[sched] {:08X} is in combat with ONLY Harbinger's substrate anchor -- a "
                                          "Harbinger leak, not a fight; MFO stays out", a->GetFormID());
                        continue;
                    }
                    ++followersFighting;
                    foes = std::max(foes, CombatSense::FoeCount(a));
                }
            }
            for (auto it = g_anchorLeakNoted.begin(); it != g_anchorLeakNoted.end();)
                it = leakNow.contains(*it) ? std::next(it) : g_anchorLeakNoted.erase(it);
            for (auto it = g_substrateNoted.begin(); it != g_substrateNoted.end();) {
                if (substrateNow.contains(*it)) { ++it; continue; }
                spdlog::info("[sched] {:08X} left the Harbinger combat substrate (closed, or real combat: his combat "
                             "counts from now)", *it);
                it = g_substrateNoted.erase(it);
            }
            const bool party = player || followersFighting > 0 || foes > 0;
            if (party != g_partyCombat) {
                g_partyCombat = party;
                spdlog::info("[sched] party combat {} (player={} followers={} foes={} substrate-only={})",
                             party ? "ON" : "OFF", player ? 1 : 0, followersFighting, foes, substrate);
            }
        }

        // ROUND-ROBIN, ONE PER TICK. Cost is O(1) in party size; a large party
        // pays in response time, not framerate (§4.1a). Resume AFTER whoever
        // was serviced last, by identity, so a reordered list cannot double-
        // service or starve anyone.
        size_t start = 0;
        if (g_lastServiced != 0) {
            for (size_t i = 0; i < active.size(); ++i) {
                auto* c = active[i].get().get();
                if (c && c->GetFormID() == g_lastServiced) { start = i + 1; break; }
            }
        }
        const size_t idx = start % active.size();

        auto* f = active[idx].get().get();
        if (!f) {
            // ADVANCE ANYWAY. Followers::Refresh deliberately re-pushes handles
            // that fail to resolve, for up to kMissesBeforeDrop sweeps -- the
            // hold exists because handles transiently fail (a 117 ms flicker
            // was eating kills before it). But a held null sits at a fixed
            // position, so a cursor that does not move past it lands on the
            // same entry every tick and NOBODY in the party is evaluated for up
            // to ~1.6 s. In combat, which is the only time it matters.
            // g_activeIds is maintained in lockstep for exactly this: it still
            // knows who this slot is when the handle cannot say.
            if (idx < Followers::g_activeIds.size()) g_lastServiced = Followers::g_activeIds[idx];
            return;
        }
        const auto id = f->GetFormID();
        g_lastServiced = id;

        // Cheap disqualifiers before any evaluation.
        if (f->IsDead() || f->IsDisabled()) {
            // A follower who dies/leaves mid-fight never reaches the combat-exit
            // teardown below, so drop weapon-stance ownership here too: otherwise
            // AnyActive() stays true (every combatant pays the per-tick lookup)
            // and a resurrection re-owns the stale stance on the fresh
            // controller without an equip win. The live CSTY already died with
            // the controller. Idempotent erase-miss when unowned.
            CombatStyle::Clear(id);
            // T#76: and the equip force-hold. Unlike the CSTY (which died with the
            // per-combat controller), the prevent-removal LOCK lives on the
            // ActorEquipManager, so it must be explicitly force-unequipped or a
            // resurrected follower comes back stuck with the weapon, unable to
            // cast. Idempotent (no record -> no-op).
            Actuation::ReleaseForcedWeapon(f);
            g_meleeClampTrueAt.erase(id);   // T#76 hysteresis dwell dies with the follower
            DeadTgtEnd(id, "follower down/disabled", 0);   // [deadtgt]: never leave a window open across a death
            return;
        }

        // A cast/drink/loot issued into a paused game resolves strangely on
        // unpause, and menus are exactly when the player is editing the list
        // that drives it. Applies to BOTH tables, so it gates before the branch.
        {
            // UNPAUSED SERVICE CLOCK (see g_serviceClock): advance only on an
            // unpaused service, capped per step; a paused one just re-anchors.
            auto* ui = RE::UI::GetSingleton();
            const bool paused = ui && ui->GameIsPaused();
            if (!paused && g_serviceClockAt.time_since_epoch().count() != 0) {
                const double dt = std::chrono::duration<double>(now - g_serviceClockAt).count();
                g_serviceClock += std::clamp(dt, 0.0, kServiceClockMaxStep);
            }
            g_serviceClockAt = now;
            if (paused) return;
        }

        const auto it = g_followers.find(id);
        if (it == g_followers.end()) return;          // no record -> nothing to run

        // CONFIDENCE v2 HP TREND: one health sample per own unpaused service (both
        // tables), on the service clock, so every Of() read later in this service
        // (loot leash, chase cap, retreat gate, ch.23 leash, engage-on-sight) sees a
        // trend that includes this lap. Cadence = his service period (Confidence.h).
        Confidence::NoteHealth(id, Vocab::HealthPct(f), g_serviceClock);

        // CAN HE ACT? (fix/mfo-can-act, field 0928c: a bleeding-out Jesper kept firing
        // instant heals.) Read ONCE per service, plain member loads (CannotActReason,
        // cast/Actuation.h). Bookkeeping above and below keeps running; the ACTION
        // roads skip: every combat rule is passed over transparently in the scan below
        // (its condition still counts, so held equips and cast loans are kept, not
        // released), and Logistics::ServiceFollower returns before it acts. His live
        // heal/offense streams are ended by the reconciles ("caster-down"). The passive
        // [bleed] line logs his life-state transitions (down / up).
        Actuation::NoteLifeState(f);
        const char* const cannotAct = Actuation::CannotActReason(f);
        // A CASTER WHO CANNOT ACT OWNS NO HEAL CLAIM (animheal phase 2, review F6): the
        // scan below passes his rules over (their conditions still count, so nothing
        // else is released), and a standing heal claim would otherwise live until the
        // FacetExpiry sweep. Guarded so a downed follower costs one bridge read per
        // service and logs once per claim.
        // THE RELEASE READS WITHOUT THE QUEUED KNOCK (review round 2, R2-4): the C3
        // contract (cast/Actuation.h, CannotActReason) is that a merely QUEUED knock
        // is momentary and may never become a knockdown, so it never frees a live heal
        // stream. The scan's own gate above keeps the default read.
        if (APMFBridge::GetHealCastSpell(id) != 0) {
            if (const char* down = Actuation::CannotActReason(f, /*a_countPendingKnock=*/false)) {
                spdlog::info("[heal] {:08X} cannot act ({}) -- the standing heal claim (spell {:08X}) "
                             "is RELEASED", id, down, APMFBridge::GetHealCastSpell(id));
                ComposedCast::End(id);
            }
        }

        // T#78: THE PER-FOLLOWER MFO MASTER SWITCH. When OFF, MFO leaves this
        // follower completely untouched -- no combat gambits, no logistics /
        // economy, no cast / equip / loot -- so he behaves as a vanilla /
        // engine-default follower. The check is a plain bool read on the worker
        // (#4). On the ON->OFF edge (first disabled tick) release everything MFO
        // was holding so he genuinely reverts -- forced equip, AI-package
        // aliases, cast / retreat / target latches, stance, MFO-equipped gear --
        // ONCE, latched in g_mfoDisabledSwept. This runs on the SAME worker as
        // the dismissal sweep that shares ReleaseHeldState, so the context is
        // identical. Re-enabling erases the latch, re-arming a future release.
        if (!it->second.mfoEnabled) {
            if (g_mfoDisabledSwept.insert(id).second)
                Followers::ReleaseHeldState(id);
            DeadTgtEnd(id, "MFO switched off for this follower", 0);   // [deadtgt]: close, no stale duration later
            return;
        }
        g_mfoDisabledSwept.erase(id);

        // THE TWO TABLES NEVER INTERLEAVE (§4.8) -- keyed on PARTY combat.
        // Combat runs while the party fights; logistics -- upkeep -- runs out of
        // it. Without the split a seeded heal rule fires while shopping in
        // Whiterun. Logistics is cadence-gated to the ~1 s idle rate INSIDE
        // ServiceFollower, so calling it every service is cheap; it acts at most
        // once per idle tick and is off by default.
        //
        // GATE 1 / GATE 2 key on g_partyCombat, NOT on this follower's own
        // IsInCombat() (Deck 2026-09-21 -- see the g_partyCombat doc above): his
        // own flag flaps false mid-fight on a LoS loss, and keying either gate on
        // it re-creates the Jesper freeze (stuck on the OOC table while the party
        // fought) and the Cicero hold teardown (the 2-tick debounce below
        // force-unequipping mid-fight). ownCombat still steers the steps that
        // genuinely belong to HIS fight (loot-travel eviction, the once-per-fight
        // "own combat=0" note) and the own-OOC logistics service at the
        // no-action exits of the combat table.
        const bool ownCombat = APMFBridge::InCombat(f);   // a Harbinger substrate is not his fight (ABI v26)
        if (!g_partyCombat) {
            // THE WHOLE PARTY-OOC TEARDOWN IS DEBOUNCED over 2 consecutive PARTY-
            // out-of-combat services (SEV-4, Fable field diagnosis 2026-09-21).
            // Before this only ReleaseForcedWeapon waited for the second service;
            // everything else below ran on the FIRST party-OFF tick. A party-OFF/
            // ON flap of one service is real: at 21:57:34.884/35.029 (145 ms) the
            // engine tore down and re-created both fighting followers' combat
            // controllers (the [wstyle] OWNED lines re-printed), the gate mirrored
            // it, and the first-tick teardown dropped a caster's consent latch,
            // cast lock and stance ownership mid-fight -- the sub-tick leak
            // v1.0.32 closed, re-opened for one lap by a controller restart. Party
            // combat does not flap with one actor's LoS (that is why the gates key
            // on it), so two consecutive party-OOC services is the real "fight
            // over" edge for every release here, not just the hold's. The
            // logistics service below still runs on every party-OOC tick.
            // Idempotent teardown (every call is an erase-miss / no-op when
            // nothing is held).
            if (++g_outOfCombatTicks[id] >= 2) {
                // NO RETREAT TEARDOWN HERE (86e3erv94 item 2). This used to
                // RetreatClear("combat ended") and erase the retreat note -- but a
                // retreat's own StopCombat is often what ENDS the party fight (the
                // only fighter just disengaged), so the retreat cancelled itself
                // 0.54 s after dispatch (deck-MFO-v9.log:1544-1582). A retreat now
                // ends only in ServiceRetreat (arrival-stay end, timeout, no live
                // foes near him), which runs just below on this branch too; the
                // note carries the cooldown across the fight edge and is never
                // erased here.
                g_combatEnteredAt.erase(id);   // flair #3: re-arm the ready beat
                Actuation::NoteCombatEpisodeEnded(id);   // re-arm the once-per-episode ch.26 "nothing to prefer" line
                g_proposedTarget.erase(id);    // flair #5: no proposal outlives a fight
                APMFBridge::ReleasePursuitLeash(id, "combat ended");   // ch.23: the in-combat leash ends with the fight
                APMFBridge::ReleaseHealApproach(id, "combat ended", APMFBridge::kApproachAnyHand, true);   // ch.24: and the held-heal approach
                APMFBridge::ReleaseOffenseApproach(id, "combat ended", true);   // ch.24 offense approach
                g_reachHold.erase(id);   // fix/mfo-unreachable-flyer: and its reach hold (no line: the fight is over)

                // v1.0.30: the cast-control latch dies with the fight. The [cast]
                // sink no longer clears it on a successful cast (the latch must
                // span cast cooldowns -- the between-casts leak), so combat end is
                // now an explicit release point. Without it, a latch from the last
                // fight lingers and denies the follower's own casting at the start
                // of the NEXT fight before his first service, wanting a spell no
                // rule may still name. Cheap and idempotent out of combat: no
                // combat caster runs the hook, and Clear on an unlatched id is an
                // uncontended erase-miss.
                CasterConsent::Clear(id);
                // The firing-spell gambit lock (Task 2, feat/cast-gambit-
                // concentration) dies with the fight too, same reasoning as the
                // cast-control latch just above -- a lock left standing from the
                // last fight would hold off the FIRST cast rule of the next one.
                // The heal claim ends with the fight too (animheal phase 2, review F6),
                // by state rather than by the FacetExpiry sweep. No-op when none stands.
                Actuation::HealHandsCombatEnded(id);   // [heal-hold] ... combat-ended, once per heal record
                ComposedCast::End(id);
                Actuation::ClearCastLock(id);
                // Weapon-stance ownership dies with the fight too. The live CSTY
                // already reverted when the per-combat controller was destroyed;
                // this drops the stale bookkeeping so the next fight re-baselines.
                // Idempotent out of combat (uncontended erase-miss when unowned).
                // Runs every party-OOC lap, also while an OOC substrate pair stands: Clear drops only the
                // combat-table stance (and releases its equip claim), the pair's own substrate-cast bit
                // (apmf/Substrate.cpp) and a pending restore survive it (review F1).
                CombatStyle::Clear(id);
                g_partyCombatNoted.erase(id);  // party combat: re-arm the once-per-fight note
                g_stallProbe.erase(id);        // [stall-probe]: the party fight is over
                DeadTgtEnd(id, "combat over", 0);   // [deadtgt]: closes an open window
                g_noneMatchedNoted.erase(id);  // and the once-per-fight "none matched" line
                g_equipRangeUndecidableNoted.erase(id);   // MFO-B55: and the once-per-fight undecidable note
                // T#76: the equip force-hold dies with the fight too -- combat end is
                // one of its release points. The prevent-removal LOCK is on the
                // ActorEquipManager (it did NOT die with the controller), so this
                // force-unequips to clear it. This was the FIRST release to be
                // debounced (SEV-2, re-keyed to party combat 2026-09-21): the
                // follower's own IsInCombat flaps mid-fight, and counting HIS flag
                // here released Cicero's hold three times in one dragon fight
                // (08:09:45, 08:10:07, 08:10:31 -- weapons vanishing and
                // returning). Idempotent (no record -> no-op).
                //
                // STAND DOWN (marth 2026-09-22: "no one ever sees an unarmed follower,
                // they sheathe weapons. Not unequip."). THIS is the site that reads as a
                // stand-down: the party fight is genuinely over (two consecutive party-OOC
                // services), the hold is simply finished and no specific item has to leave
                // the hand. The lock is still force-cleared -- it has to be -- but the same
                // weapon goes straight back on NON-forced and the follower sheathes, so the
                // "weapons vanishing" this comment already describes stops being visible.
                // See ReleaseForcedWeapon's doc in cast/Actuation.h for why the unequip itself
                // cannot go away.
                Actuation::ReleaseForcedWeapon(f, /*a_standDown=*/true);
                // T#76 hysteresis dwell erased on the SAME 2-tick debounce (Fable
                // SEV-3): an un-debounced erase on a 1-tick IsInCombat flap would
                // drop the dwell, and the flap-back tick at the gambit's false edge
                // would then release instantly, skipping the commit window.
                g_meleeClampTrueAt.erase(id);
            }

            // A RETREAT OUTLIVES THE PARTY FIGHT (86e3erv94 item 2): drive it here
            // too. While he holds one it is his action -- no logistics this lap (a
            // loot walk armed under it would fight the retreat travel for the
            // actor, the same reason the combat table's retreat exit skips
            // serviceOwnOoc).
            {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                const float dPlayer = pc ? f->GetPosition().GetDistance(pc->GetPosition()) : 0.0f;
                if (ServiceRetreat(f, id, dPlayer, pc != nullptr)) {
                    g_lastTickMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0).count();
                    return;
                }
            }

            // ENGAGE ON SIGHT (ClickUp 86e3errnu): the hidden OOC gambit "nearest visible
            // enemy" (EngageOnSight.h). After the retreat (a retreating follower never
            // starts a fight) and BEFORE logistics: on a lap it engages (or while its
            // Harbinger ch.21 entry is pending) it preempts logistics the way player
            // combat does -- his loot trip was ended inside Service. The retreat cooldown
            // and the engage bar (kEngageConfidence) are passed from here so they have ONE definition.
            {
                const auto& rn = g_retreatNotes[id];
                const bool cooling = rn.rearmLaps > 0 || now < rn.rearmAt;
                if (EngageOnSight::Service(f, id, cooling, kEngageConfidence)) {
                    g_lastTickMs = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0).count();
                    return;
                }
            }

            Logistics::ServiceFollower(f, it->second);
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
        }

        g_outOfCombatTicks.erase(id);   // T#76: party back in combat -> re-arm the debounce
        // A teammate in combat with ONLY Harbinger's anchor (a leak, logged by the party-combat count) runs no combat
        // table: no foes exist for it. Every other site still reads him as IN combat (raw), so no OOC work starts.
        if (CombatSense::AnchorOnlyLeak(f)) {
            g_lastTickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return;
        }
        // POST-BATTLE SHED GATE: stamp "seen fighting" so Logistics only drops
        // off-role weapons once the PARTY has been stably out of combat for a
        // dwell -- never during a mid-fight IsInCombat() lull. Stamped on every
        // party-combat service, own flag or not: the shed's "only ever sees an
        // UNHELD pack" guarantee (MAP, SHED INTERACTION) rests on the hold being
        // released (the party-OOC branch above) BEFORE the dwell can mature, and
        // now that the hold survives an own-flag flap inside a party fight, the
        // dwell must not mature inside one either. Same worker tick as the
        // shed's read, so the map needs no lock (#4).
        Logistics::NoteInCombat(id);

        // ENGAGE IN A PARTY FIGHT (ClickUp 1dfcuabc4du): nothing else starts a fight for a follower whose own flag is
        // false while the party fights, so he stood by ("oblivious"). Same gambit, joining mode (EngageOnSight.h
        // ServiceInFight). Not while retreating (the retreat's StopCombat is what left him out of combat), not on a
        // town errand (away), and it adds no action of its own: a pending / filed entry only holds the OOC logistics
        // road off this lap (engagePending, read by serviceOwnOoc below).
        bool engagePending = false;
        if (!ownCombat && !Logistics::Errand::Active(id)) {
            const auto& rn = g_retreatNotes[id];
            if (rn.phase == RetreatPhase::None) {
                const bool cooling = rn.rearmLaps > 0 || now < rn.rearmAt;
                // One follower is serviced per 133 ms pump tick (Diagnostics kPumpMs), so he is seen every N x 133 ms.
                const float periodSecs = 0.133f * static_cast<float>(std::max<std::size_t>(1, Followers::g_activeIds.size()));
                engagePending = EngageOnSight::ServiceInFight(f, id, cooling, kEngageConfidence, periodSecs);
            }
        }

        // OWN-OOC INSIDE A PARTY FIGHT: the combat table runs (gate 1), but this
        // follower has no combat controller of his own, so every foe-keyed rule
        // falls through transparently and the OOC service road must still run
        // for him -- he is merely NEAR a fight, and logistics (loot, economy,
        // the equip declaration) must not starve. serviceOwnOoc() is called at
        // each NO-ACTION exit of the combat table below (empty rules, ready
        // beat, scan ended without a Fired / opaque hold) and NEVER on a tick
        // that acted: the combat table and the logistics table each fire at
        // most one real action per tick (§4.3), and running the second only
        // when the first produced nothing keeps that to ONE action per tick per
        // follower -- no rule can double-fire. The retreat-holder exit skips it
        // too: the retreat travel IS his action, and a loot excursion armed
        // under it would fight it for the actor. Re-finds the record by id at
        // call time (INVARIANTS #2): the scan may have dispatched engine events.
        //
        // a_castFacetHeld -- THE CAST FACET IS THE COMBAT TABLE'S FOR ANY LAP A
        // COMBAT CAST RULE'S CONDITION HELD (cross-branch SEV-3, Fable
        // restoration review, 2026-09-21). A restoration cast on the combat
        // table is a DIRECT stream now (RestorationCastDirect / CastSelfDirect:
        // the follower's single g_targetCast / g_selfCast channel, paced at
        // ~1 s), and its paced lap returns a TRANSPARENT NoOp so the rules
        // below it can run -- which the post-scan exit read as "no action" and
        // handed the lap to logistics. Logistics' own OOC cast dispatch has no
        // stream guard: a different (spell, target) at the same hurt ally --
        // OOC Healing Hands against combat Fast Healing -- hit `stream RELEASE
        // (switch)`, re-minted with lastApply at the epoch and applied at
        // once; the next lap the combat rule switched it back. One cast per
        // lap alternating two heals, magicka drained, TargetCastEndActor
        // churn, and that lap's OOC DeclareAtExit read the left hand as free
        // (its leftReserved is claim liveness only) under the direct road's
        // live LEFT cast lock. The lap OWNER answers it, not Logistics: the
        // scan's `castSeen` is ALREADY the signal this file defines the cast
        // lock's lifetime by (`!castSeen` -> ReleaseOffenseCast + ClearWatch
        // + ClearCastLock below), so "a cast rule's condition held" IS "the
        // combat table holds the cast facet this lap" -- the same one-action
        // rule as a Fired lap, applied to a channel that is being SUSTAINED
        // rather than opened. Deliberately the condition, not a live-stream
        // read: a Declined lap (unaffordable, out of range) still belongs to
        // the rule that is asking, exactly as in the follower's own combat
        // where logistics never runs at all; and on every lap that DOES reach
        // logistics `castSeen` is false, so the lock was cleared above and the
        // OOC declaration's claim-only leftReserved is exact again -- both
        // halves close on one signal. Lower-priority logistics (loot, economy,
        // the OOC declaration) wait one lap per cast lap -- the same wait the
        // follower's own combat imposes for the whole fight -- while the
        // combat road's own declaration sites (EquipWeapon's gambit equip and
        // top-up, ReconcileForcedWeapon's bound-weapon / B41 re-send) keep
        // running on those laps as they do in his own combat.
        const auto serviceOwnOoc = [&](bool a_castFacetHeld) {
            if (ownCombat || a_castFacetHeld || engagePending) return;
            if (const auto rec = g_followers.find(id); rec != g_followers.end())
                Logistics::ServiceFollower(f, rec->second);
        };
        if (!ownCombat && g_partyCombatNoted.insert(id).second)
            spdlog::info("[sched] {:08X}: party combat, own combat=0 -- combat table live", id);
        if (ownCombat) g_stallProbe.erase(id);   // [stall-probe]: only while own combat is OFF
        else           StallProbe(f, id);
        DeadTgtService(f, id, ownCombat);   // [deadtgt]: passive, ClickUp 86e3eb078
        TauntProbe::Service(f, id, ownCombat);   // [taunt-probe]: passive, gated by bTauntProbe (ClickUp 86e3g6duw)
        APMFBridge::ReleaseOffenseCastOnDeadTarget(id);   // ClickUp 86e3eb078: a dead target ends the channel at once

        // OWNED MODEL: keep this follower's APMF combat-target claim (if any) alive for
        // the WHOLE fight. Refreshes the expiry timestamp only (no create, no re-point)
        // every in-combat tick, so a non-targeting rule (potion/flee/wait) does not let
        // it lapse mid-battle -- it is released by the expiry sweep ONLY once combat
        // ends (this stops being called). Re-pointing the claim stays with the cast/attack
        // directives (ClaimCombatTarget). No-op without APMF or if no claim is held.
        APMFBridge::RefreshCombatTarget(id);

        // IN HIS OWN COMBAT: end any loot excursion for this follower FIRST, so he
        // fights instead of staying claimed by the loot quest at priority 60
        // (batches last up to 60 s and would otherwise run right through the
        // fight, making him look passive). Must be before the combat-rules
        // early-out below, so it yields even for a follower with no combat
        // gambits. Keyed on HIS flag, not party combat: the eviction is
        // unconditional, and an own-OOC follower's service (serviceOwnOoc) may
        // legitimately arm a loot walk near a fight he is not in -- evicting it
        // every party-combat tick would churn arm/evict at the logistics
        // cadence. The PLAYER's combat already ends every excursion inside
        // ServiceFollower (the player-combat interrupt backstop).
        if (ownCombat) Logistics::ReleaseTravelOnCombat(f);

        // ── #59: CONTINUOUS CAST CONTROL -- refresh the persistent filter ────
        // The consent LATCH only stands while a cast rule wins the scan, and
        // Clear drops it on any combat-state flap -- so between forced casts
        // the slider's filter used to vanish and the follower's AI slipped its
        // own spells in (Serana's Ice Spike between forced Sunfires). This
        // hands CasterConsent the follower's configured cast-gambit spells
        // every combat service tick, BEFORE the ready-beat and suppression
        // early-outs below (a suppressed scan must not starve the filter), so
        // the deny hooks can run the slider's policy continuously -- exact
        // allows only these spells, partial applies the kind-filter -- latched
        // or not. An empty vector ERASES: control off, log mode, or no cast
        // gambit configured means this follower is never under the filter (a
        // pure-melee follower's own casting stays untouched). Combat end /
        // dismissal / revert-load sweep the entry (CasterConsent::Clear/
        // ClearAll); out of combat nothing reads it and the hard gate checks
        // IsInCombat besides.
        {
            std::vector<RE::FormID> gambits;
            if (Config::g_castControl.load() > 0 && Config::g_casterMode.load() != 0) {
                for (const auto& g : it->second.combat()) {
                    const auto& op = g.actionOpcode;
                    if ((op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                         op == Vocab::kActCastPlayer) && g.actionParamForm != 0)
                        gambits.push_back(g.actionParamForm);
                }
            }
            // ch.25 CAST GUARD (Harbinger client mode, ABI >= 21; a no-op otherwise --
            // MFO's own CheckCast hook carries both guards then, see CasterConsent::
            // InstallHook). The friendly-fire hold (bFriendlyFireHold) and the
            // concentration bound on the AI's own hand channel, filed where the
            // allow-list cannot refuse the spell: a gambit concentration spell at ANY
            // iCastControl >= 1 (the old 0x06 never-force-YES rule denied the AI's own
            // attempt at the latched concentration spell at every level), and, at Exact
            // only, a follower with no cast gambit (no allow-list at all;
            // ConcUnboundedDeny was Exact-only). Same switches the hooks read:
            // bCasterHook gated both (it gated the install), log mode turns both bounds off.
            if (APMFBridge::CastGuardSupported()) {
                const bool hookOn = Config::g_casterHook.load();
                const int  lvl    = Config::g_castControl.load();
                bool channel = false;
                if (hookOn && lvl >= 1 && Config::g_casterMode.load() != 0) {
                    channel = lvl >= 4 && gambits.empty();
                    for (const auto sid : gambits) {
                        auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sid);
                        if (sp && sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                            channel = true;
                            break;
                        }
                    }
                }
                APMFBridge::ServiceCastGuard(id, hookOn && Config::g_friendlyFireHold.load(), channel);
            }
            CasterConsent::NoteGambits(id, std::move(gambits));
        }

        // COMBAT SENSE sample (#23, Confidence v2): the inputs Of() folds in, for
        // tuning. own = his OWN combat flag -- on the combat table it can read 0
        // while the PARTY fights (he has no combat group yet, so foes=0 and Of() is
        // his out-of-combat read, vitality x trend: not a stale tally). load = the
        // weighted foe load (CombatSense::FoeLoad; 1.0 = one even foe), hpLoss =
        // the HP-trend loss rate in %/s, trend = its factor. Purely observational;
        // throttled ~3 s / follower.
        {
            auto& nxt = g_nextSense[id];
            if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                nxt = now + std::chrono::seconds(3);
                auto* spc = RE::PlayerCharacter::GetSingleton();
                int         foes = 0;
                const float load = CombatSense::FoeLoad(f, &foes);
                const float loss = Confidence::HpLossRate(id);
                spdlog::info("[sense] {:08X}: own={} foes={} load={:.2f} hpLoss={:.1f}%/s trend={:.2f} "
                             "confidence={:.2f} leash={:.0f} chase={:.0f} dPlayer={:.0f}",
                             id, APMFBridge::InCombat(f) ? 1 : 0, foes, load, loss * 100.0f,
                             Confidence::TrendFactor(Vocab::HealthPct(f), loss), Confidence::Of(f),
                             Confidence::LeashRadius(f), Confidence::ChaseRadius(f),
                             spc ? f->GetPosition().GetDistance(spc->GetPosition()) : -1.0f);
            }
        }

        // #63 QUASH ALLY COMBAT -- tick backstop. The TESCombatEvent quash
        // (Rapport) catches a follower ENTERING combat against a teammate; but if
        // BOTH were already fighting foes when friendly fire crossed them, no
        // enter-event fires for the crossfire. So each combat tick, if the serviced
        // follower is actively targeting a player TEAMMATE, end it on both sides.
        // Via Rapport::QuashAllyPair (v1.0.53 deck freeze): the StopCombat runs
        // on the MAIN-thread pump, never here on the worker -- the old inline
        // pair here raced the event-sink quash mutating the SAME actors'
        // combat state mid-dispatch -- and the shared per-pair cooldown keeps
        // this backstop and the sink from ping-ponging every tick.
        if (Config::g_quashAllyCombat.load()) {
            auto tp = f->GetActorRuntimeData().currentCombatTarget.get();
            if (auto* tgt = tp.get(); tgt && tgt != f && tgt->IsPlayerTeammate()) {
                if (Rapport::QuashAllyPair(id, tgt->GetFormID())) {
                    spdlog::info("[peace] {:08X} targeting teammate {:08X} -- quashed (StopCombat both)",
                                 id, tgt->GetFormID());
                }
            }
        }

        // ── AUTO-RETREAT (leash safety, bAutoRetreat, default OFF) ────────────
        // The confidence leash taken to its conclusion: a follower who is badly
        // outmatched (confidence below threshold -- by the leash tenet he WANTS
        // to be at the player's side) AND far from the player in combat falls
        // back under a kIgnoreCombat Travel package that outranks his combat
        // controller's locomotion (APMF ch.9 offer; legacy alias when APMF is
        // absent). ON by default (Config.h g_autoRetreat -- the one place MFO
        // acts without an authored rule; the MCM toggle is the override). The
        // driver (ServiceRetreat) runs TRAVEL -> STAY -> release on every
        // service from both tables; after a release a cooldown gates the next
        // fill (kRetreatCooldownLaps / kRetreatCooldownSecs). Logging is
        // transition-only.
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const float dPlayer = pc ?
                f->GetPosition().GetDistance(pc->GetPosition()) : 0.0f;

            if (ServiceRetreat(f, id, dPlayer, pc != nullptr)) {
                APMFBridge::ReleasePursuitLeash(id, "retreating");   // ch.23: the retreat has its own road
                APMFBridge::ReleaseHealApproach(id, "retreating");   // ch.24: so does the held-heal approach
                APMFBridge::ReleaseOffenseApproach(id, "retreating");
                // While falling back, do NOT run the gambit table: a cast rule
                // would fill the COMMAND alias (also priority 60) on the same
                // actor and fight the retreat travel for the alias. A retreating
                // follower is disengaging, not gambitting -- that is the point.
                // Deliberately NO serviceOwnOoc() here either: a loot travel
                // armed under the retreat would fight it for the same actor.
                //
                // THE ONE EXCEPTION (marth 2026-10-01: "Heal while retreating? Themselves,
                // yes. others no."): a Heal-kind act.cast_self rule. cast_self is self-
                // targeted by definition, so no heal on others, no offense and no buff
                // reaches Fire. Only once StopCombat has landed (out of combat): then the
                // follower has no CombatController and CastOn takes the direct self road
                // (ChooseHealRoad DirectNoCombat), which owns no package and no alias, so the
                // retreat travel is untouched and nothing is left for a later StopCombat to
                // cut. Before it lands, nothing runs (a claim minted now would be cut by it).
                if (!APMFBridge::InCombat(f) && !Actuation::CannotActReason(f) && Config::g_castSelf.load()) {
                    TableSource retreatTable(f, id);   // the combat TABLE, by name (card I1; IAUS: card I6 decides)
                    for (;;) {
                        Eval::Choice sc;
                        if (!retreatTable.Next(sc)) break;
                        if (!Rank::IsChoice(sc.rank)) break;
                        retreatTable.Reject(sc);
                        // act.cast_self, or a cast_target row whose TARGET is "Self" (Subject::Caster,
                        // 2026-10-06): the same self road (Fire -> CastOn(f, spell, f)), self by definition.
                        const bool selfRule =
                            sc.actionOpcode == Vocab::kActCastSelf ||
                            (sc.actionOpcode == Vocab::kActCastTarget && sc.subjectActorForm == 0 &&
                             static_cast<Vocab::Subject>(sc.subject) == Vocab::Subject::Caster);
                        if (!selfRule) continue;
                        auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sc.actionParam);
                        if (!sp || !Actuation::HealsHealth(sp)) continue;
                        // The EXACT decision CastOn will make: only a package-free direct road runs here
                        // (NoCombat, NoSeat, Degrade and NotHeal all reach CastSelfDirect with bCastSelf
                        // on). An inactive controller still answers Claim, which is skipped (review F2).
                        if (ComposedCast::ChooseHealRoad(f, sp, f) == ComposedCast::HealRoad::Claim) continue;
                        const Actuation::Outcome o = Actuation::Fire(f, sc);
                        if (o.transparent) continue;
                        auto& last = g_retreatHealLog[id];
                        if (now - last > std::chrono::seconds(3)) {
                            last = now;
                            spdlog::info("[retreat-heal] {:08X}: self-heal rule {} (spell {:08X}) while retreating: {}",
                                         id, sc.rank, sc.actionParam, o.reason);
                        }
                        break;
                    }
                }
                g_lastTickMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                return;
            }

            auto& note = g_retreatNotes[id];
            if (Config::g_autoRetreat.load() && note.rearmLaps == 0 && now >= note.rearmAt &&
                APMFBridge::InCombat(f) && pc && pc->IsInCombat() && dPlayer > kRetreatMinDist) {
                // Read BEFORE the fill: the fill posts a StopCombat, and once it
                // lands Of() loses the combat/foe multiplier, so any later read
                // would log a misleading 0.8-1.0 (assess-confidence-leash step 4).
                const float conf = Confidence::Of(f);
                if (conf >= kRetreatConfidence) { note.belowSince = -1.0; note.belowSamples = 0; }
                else {
                    if (note.belowSince < 0.0) { note.belowSince = g_serviceClock; note.belowSamples = 0; }
                    ++note.belowSamples;
                }
                const double belowFor = note.belowSince < 0.0 ? 0.0 : g_serviceClock - note.belowSince;
                if (conf < kRetreatConfidence && belowFor >= kRetreatBelowSecs && note.belowSamples >= 2) {
                    // P2 (field 2026-10-01): a cast in flight finishes (the heal rule's own D8).
                    // Delay the fill while the follower's own cast is live, one sense lap at a
                    // time, bounded by kRetreatCastDeferMs from the first delay so a held charge
                    // or a long stream cannot postpone the retreat for good.
                    bool deferFill = false;
                    if (!Actuation::OwnCastInFlight(f)) note.castDeferSince = {};
                    else {
                        // A stamp from an earlier lap-run that never reached a fill is stale.
                        if (note.castDeferSince.time_since_epoch().count() == 0 ||
                            now - note.castDeferSince > std::chrono::milliseconds(2 * kRetreatCastDeferMs)) {
                            note.castDeferSince = now;
                            spdlog::info("[retreat] {:08X}: fill delayed -- own cast in flight "
                                         "(bounded {} ms, confidence={:.2f} dPlayer={:.0f})",
                                         id, kRetreatCastDeferMs, conf, dPlayer);
                        }
                        if (now - note.castDeferSince < std::chrono::milliseconds(kRetreatCastDeferMs))
                            deferFill = true;
                        else
                            spdlog::warn("[retreat] {:08X}: cast still in flight after {} ms -- filling anyway",
                                         id, kRetreatCastDeferMs);
                    }
                    if (!deferFill) {
                    note.castDeferSince = {};
                    int         foes = 0;
                    const float load = CombatSense::FoeLoad(f, &foes);
                    const float loss = Confidence::HpLossRate(id);
                    if (Packages::RetreatFill(f)) {
                        Actuation::ReleaseHealClaimForRetreat(f);
                        note.phase           = RetreatPhase::Travel;
                        note.fleeHoldAt      = -1.0;
                        note.took            = false;
                        note.outOfCombatSeen = false;
                        note.reentries       = 0;
                        note.skipStay        = false;
                        note.fightConf       = conf;
                        spdlog::info("[retreat] {:08X}: falling back -- confidence={:.2f} (pre-StopCombat) "
                                     "foes={} load={:.2f} hpLoss={:.1f}%/s belowFloor={:.1f}s/{}samples dPlayer={:.0f}",
                                     id, conf, foes, load, loss * 100.0f, belowFor, note.belowSamples, dPlayer);
                        note.belowSince = -1.0; note.belowSamples = 0;
                    } else {
                        // NOT silent (principle 7) and NOT one-per-fight: the fill
                        // failed (RetreatFill logged why), so wait out the cooldown
                        // and try again within this fight.
                        StartRetreatCooldown(note, now);
                        spdlog::warn("[retreat] {:08X}: fill FAILED -- confidence={:.2f} foes={} dPlayer={:.0f}; "
                                     "retry after {} own services and {:.0f}s", id, conf, foes, dPlayer,
                                     kRetreatCooldownLaps, kRetreatCooldownSecs);
                    }
                    }   // !deferFill
                }
            } else {
                note.belowSince = -1.0; note.belowSamples = 0;   // gate not met: the run restarts
            }
        }

        // ── IN-COMBAT LEASH (Harbinger ch.23, ABI v16; apmf/PursuitLeash.cpp) ──
        // Anchor = the player, radius = Confidence::LeashRadius (the tenet's "how far from
        // the PLAYER he is willing to be"). Filed on the first combat-table service that is
        // not a retreat, Repointed only on a band-sized change, released on retreat start
        // (a fill on THIS lap included), the party-OOC teardown, dismissal and load. No-op
        // when Harbinger is absent or older than v16 (MFO had no in-combat leash before).
        // REACH HOLD (fix/mfo-unreachable-flyer, marth "hold by you"): a melee-only follower
        // whose every foe is airborne out of his reach gets the MINIMUM radius (fLeashMin)
        // instead, so the engine's range-keeping around a flyer cannot walk him off the
        // party; the confidence radius comes back when a foe is in reach again.
        if (Packages::IsRetreating(id)) APMFBridge::ReleasePursuitLeash(id, "retreat started");
        else                            ServiceLeashWithReachHold(f, id);

        if (it->second.combat().empty()) {
            // #65: no combat gambits, but a class override is still a valid STYLE
            // pick -- apply it (same master-toggle gating as the main stance path
            // below) before this early-out, so "just make this follower Ranged/
            // Mage" works even with an empty rule table. Auto (0) does nothing --
            // the #64 custom-follower guarantee holds on the empty path too.
            if (const std::uint8_t ov = Logistics::EffectiveClassPick(id, it->second); ov != 0) {
                const auto forced = static_cast<CombatStyle::Stance>(ov);
                if (forced == CombatStyle::Stance::Cast) {
                    if (Config::g_castControl.load() > 0) CombatStyle::Want(id, forced);
                } else if (Config::g_weaponStyleControl.load()) {
                    CombatStyle::Want(id, forced, false,
                                      forced == CombatStyle::Stance::Melee && Logistics::CombatStyleFor(f, it->second).offHand == 2);
                }
            }
            serviceOwnOoc(/*castFacetHeld=*/false);   // no combat rules -> no cast rule can hold the facet -> the OOC road runs
            return;      // no rules -> nothing else to run
        }

        // ── FLAIR #3: COMBAT-ENTRY READY BEAT ────────────────────────────────
        // The first serviced tick of a fight stamps the entry; the table holds
        // for one human beat (~200-350 ms by temperament) before the opening
        // action. Once per combat entry -- never delays mid-fight reactions --
        // and silent: no engine call, no log, exactly a slightly-later opening.
        {
            const auto beat = g_combatEnteredAt.try_emplace(id, now).first;
            const auto hold = std::chrono::milliseconds(
                200 + static_cast<int>(150.0f * Temperament(id)));
            if (now < beat->second + hold) {
                serviceOwnOoc(/*castFacetHeld=*/false);   // the beat holds the combat table only (no scan ran -> no cast rule held), not logistics
                g_lastTickMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                return;
            }
        }

        // ── THE COMBAT SCAN (GAMBIT_FLOWS H1 + H2 + H3) ──────────────────────
        // Mirrors the logistics fall-through (Logistics.cpp): try matching
        // rules in order until one produces a REAL outcome. A rule whose
        // outcome is TRANSPARENT -- satisfied equip, unaffordable cast, empty
        // potion stack, cast cooldown, 2H debounce -- must not shadow the
        // rules below it: that shadow is why a spellsword never fell back to
        // steel and a follower died holding a heal below an empty drink rule
        // (D1/D2). Still AT MOST ONE real action per tick (§4.3): transparent
        // outcomes are by definition non-mutating, and the scan stops cold on
        // the first Fired or opaque hold (act.wait, a latched attack, the
        // cast-grace wait -- those legitimately occupy the tick).
        auto& recent = g_recent[id];
        const bool inWindow = now < recent.until;
        // #11: the window's anchor is the fired decision's IDENTITY, re-resolved to its
        // CURRENT rank each tick by the source (TableSource::SuppressionAnchor).
        TableSource table(f, id);   // card I6 picks the IAUS source here for an iausCombat follower
        ChoiceSource& src = table;
        const int firedRuleNow = src.SuppressionAnchor(inWindow, recent.firedRule, recent.firedUID);
        const bool windowActive = Rank::WindowActive(inWindow, firedRuleNow);

        ScanOut scan;
        Scan(src, f, id, now, cannotAct, windowActive, firedRuleNow, scan);
        auto& castSeen = scan.castSeen;
        auto& matchedCast = scan.matchedCast;
        auto& wantStance = scan.wantStance;
        auto& equipHeld = scan.equipHeld;
        auto& silverHeld = scan.silverHeld;
        auto& stopped = scan.stopped;
        auto& suppressed = scan.suppressed;
        auto& chain = scan.chain;
        auto& choice = scan.choice;
        auto& outcome = scan.outcome;

        // COMBAT-STYLE OWNERSHIP. The actual combatStyle write happens on the
        // combat thread (CombatStyle::ApplyTick from the UpdateCombat hook); this
        // job-worker tick only records the desired stance. PRIORITY:
        //   1. An equip gambit that claimed the hand this tick -> weapon stance
        //      (v1.0.33), gated by bWeaponStyleControl. Held until battle end or
        //      the next equip flips it.
        //   2. Otherwise, a follower a CAST gambit owns (latched) -> the CASTER
        //      stance (mage update), gated by iCastControl>0 -- swapped to pure
        //      MELEE when he cannot afford the gambit spell (magicka dry), back
        //      to caster when it regenerates (marth: cast-till-dry-then-melee).
        // Only UPDATE on a real signal; a one-tick heal above must not drop the
        // stance (the "hold until it changes" contract), so no Want otherwise.
        // #65: the per-follower COMBAT-CLASS OVERRIDE is the follower's RESTING
        // stance -- it applies whenever NO equip gambit is active this tick, and
        // Auto (0) changes NOTHING (the #64 custom-follower guarantee: falls
        // straight through to the gambit-driven logic). It does NOT supersede an
        // ACTIVE equip gambit. FFXII TRUE OVERRIDE (marth): while an
        // act.equip_melee/ranged condition holds TRUE, the ordered weapon stance
        // is ENFORCED over the class override AND the cast latch for the true
        // duration, reverting to the resting override the tick it goes false.
        // Without this, a CASTER-class override (or a Cast/Melee override vs an
        // equip_ranged order) defeated the T#76 clamp for exactly the mages it
        // was built for -- Lucien: cast override held, dagger<->spell thrash,
        // the equip gate never armed (equipOrder stayed false).
        // Re-find the record (same INVARIANTS #2 discipline as `rec` above:
        // g_followers could have shifted under the scan).
        // v1.1: the actor-keyed g_followers find + combatClassOverride read is the
        // general GetBaseClass primitive (byte-identical; still the same #2 re-find).
        const std::uint8_t classOverride = Followers::GetBaseClass(id);

        // T#76: is the equip gambit's condition KNOWN false this tick? Two ways:
        //  (a) the scan ran to COMPLETION without firing (nothing matched) --
        //      classic condKnownFalse; OR
        //  (b) POSITIONAL (Fable SEV-2 2026-08-17): Eval returns the FIRST match
        //      from index 0, so EVERY rule above the stop was evaluated this tick.
        //      An act.equip_* rule that sits above the stop yet left equipHeld==0
        //      (it neither fired, satisfied, nor held in its suppression window)
        //      had a FALSE condition -- its truth IS known even though a lower
        //      rule stopped the scan. Without (b) the canonical layout (equip
        //      rule ABOVE the attack/cast fallback that stops the scan every acting
        //      tick) never releases the clamp until combat end -- the "for the
        //      duration of true" revert would never fire mid-fight. Hoisted here:
        //      the stance arbitration preserves a held clamp only on a TRULY
        //      unknown tick; the weapon/stance release paths reuse it.
        const bool condKnownFalse = !stopped && !suppressed;
        bool equipCondKnownFalse = condKnownFalse;
        if (!equipCondKnownFalse && equipHeld == 0) {
            // Stop: where the scan halted, or past-end if nothing matched
            // (choice.rank < 0 -> the whole list was evaluated).
            if (src.EquipEvaluatedAboveStop(choice))
                equipCondKnownFalse = true;   // evaluated above the stop, did not hold
        }
        // SILVER (ClickUp 86e3p2gp9): the silver rule's OWN truth, by the same two ways -- the scan
        // ran to completion, or an act.equip_silver rule sat above the stop and did not hold. Its
        // own, because another melee equip holding the hand (equipHeld == 1) must not keep the
        // silver weapon in it once the target is no longer Unholy. Deliberately NOT folded into
        // equipCondKnownFalse above: a false silver rule above the stop says nothing about an
        // act.equip_melee rule below it, whose hold must survive its unknown laps as before.
        bool silverKnownFalse = !silverHeld && condKnownFalse;
        if (!silverHeld && !silverKnownFalse) {
            if (src.SilverEvaluatedAboveStop(choice)) silverKnownFalse = true;
        }
        // "Foe targeted: Unholy" is target-relative like the range conditions: with no usable
        // currentCombatTarget the Evaluator returns empty, which is UNKNOWN, not false, while the
        // follower's own group still has foes (MFO-B55 shape; the Cicero line-of-sight-loss thrash).
        // Demote silverKnownFalse so ReconcileSilver keeps refreshing trueAt and the silver weapon
        // stays. Gated: an act.equip_silver + cond.foe_is_unholy rule was EVALUATED above the stop,
        // the current target is unusable (Eval::CurrentTargetIfTrackable, PickFoe's own gates), and
        // CombatSense::FoeCount > 0. A genuinely empty group still reads false and releases.
        if (silverKnownFalse && !silverHeld) {
            const bool unholyRuleEvaluated = src.UnholySilverEvaluated(choice);
            if (unholyRuleEvaluated && !Eval::CurrentTargetIfTrackable(f) && CombatSense::FoeCount(f) > 0)
                silverKnownFalse = false;
        }
        // MFO-B55 FIX: a "known false" that came from a target-relative equip
        // rule reading a NULL / dead / non-hostile currentCombatTarget while the
        // follower's own group still has foes is NOT known -- demote it to
        // UNKNOWN (see EquipRangeUndecidable). Logged once per fight so the
        // field log shows the hold being kept on an engine-side null, not silence.
        bool equipRangeUndecidable = false;
        if (equipCondKnownFalse && equipHeld == 0) {
            equipRangeUndecidable = src.EquipRangeUndecidable(f, choice);
            if (equipRangeUndecidable) {
                equipCondKnownFalse = false;
                if (g_equipRangeUndecidableNoted.insert(id).second)
                    spdlog::info("[sched] {:08X}: equip range rule has no usable combat target while own group has foes -- "
                                 "hold kept (truth unknown, not false; MFO-B55)", id);
            }
        }

        // T#76 HYSTERESIS: gate the RELEASE on MeleeClampDwell(id) of SUSTAINED
        // known-false so a foe crossing the gambit's range threshold back and
        // forth doesn't re-sheathe the follower every tick (marth: reads human).
        // equipHeld != 0 (clamp true this tick) resets the dwell; a genuinely
        // false gambit still reverts after the dwell. `equipReleaseOK` supersedes
        // raw `equipCondKnownFalse` at every release/preserve site below -- so the
        // weapon, the CSTY clamp, and the equip gate all hold together through the
        // commit window. Entry stays prompt (branch 1 below is not gated by this).
        const auto nowTp = std::chrono::steady_clock::now();
        // Refresh the dwell clock while the gambit is TRUE (equipHeld) OR its truth
        // is UNKNOWN-but-held (a higher rule stopped the scan above the equip rule).
        // The clock therefore advances ONLY on genuinely KNOWN-false ticks, so the
        // dwell measures SUSTAINED known-false -- not time-since-last-true. Without
        // the unknown-tick refresh (Fable SEV-3), a higher-rule burst (e.g. a heal)
        // longer than the dwell pre-expires it, and the first known-false tick after
        // it releases instantly -- the very first-tick twitch T#76 exists to remove.
        // heldOrder is reused by preserve branch 2 below (stable across the block:
        // only worker Want/Release mutate the equipOrder flag, none run between).
        const bool heldOrder = CombatStyle::HoldsEquipOrder(id);
        // MFO-B55: an undecidable lap (null target, foes remain) refreshes the
        // dwell like an unknown-but-held tick does, with or without a stance
        // order, so the commit window is measured from the last DECIDABLE lap.
        if (equipHeld != 0 || (!equipCondKnownFalse && (heldOrder || equipRangeUndecidable)))
            g_meleeClampTrueAt[id] = nowTp;
        bool dwellExpired = true;
        if (const auto dit = g_meleeClampTrueAt.find(id); dit != g_meleeClampTrueAt.end())
            dwellExpired = std::chrono::duration<float>(nowTp - dit->second).count() >= MeleeClampDwell(id);
        const bool equipReleaseOK = equipCondKnownFalse && dwellExpired;

        CombatStyle::Stance stance = CombatStyle::Stance::None;
        // STANCE PRIORITY (FFXII true-override): an ACTIVE equip gambit is enforced
        // over the #65 class override and the cast latch for its true duration;
        // the override is the RESTING stance under it. Only the equip branch sets
        // stanceIsEquipOrder -- it rides into CombatStyle::Want so the equip gate
        // denies the follower's AI re-arming spells/staves over the forced weapon
        // (a cast latch / override Want with false RELEASES a standing hold).
        bool stanceIsEquipOrder = false;
        if (wantStance && Config::g_weaponStyleControl.load()) {
            // 1. ACTIVE equip gambit (top priority). Enforced over any class
            //    override / cast latch. equipOrder=true arms the CheckShouldEquip
            //    gate, which still EXEMPTS a higher cast gambit's latched spell
            //    (§7.2) -- the mage left-hand-casts an ordered spell while the
            //    dagger stays force-held. Gated by bWeaponStyleControl -> off
            //    falls through to the resting override below (MFO never touches
            //    weapon stances with the kill-switch off).
            stance = (wantStance == 2) ? CombatStyle::Stance::Ranged
                                       : CombatStyle::Stance::Melee;
            stanceIsEquipOrder = true;
        } else if (!equipReleaseOK && heldOrder) {
            // 2. PRESERVE the held clamp. A HIGHER rule stopped the scan before
            //    the equip rule this tick AND the equip rule sits BELOW the stop
            //    (so its truth is genuinely UNKNOWN) and an equip order is HELD --
            //    leave stance None so NO Want fires
            //    and the standing clamp (weapon + gate) survives while the higher
            //    rule's transient action (an ordered cast/heal) runs. Without
            //    this the override / cast latch below would overwrite the clamp to
            //    Cast mid-fight and disarm the gate -- the thrash reopens. Release
            //    happens only when the gambit is KNOWN false (ReleaseEquipOrder).
        } else if (classOverride != 0) {
            const auto forced = static_cast<CombatStyle::Stance>(classOverride);
            // 3. RESTING stance: the #65 class override (no equip gambit active/
            //    held). A forced PICK, not a bypass of the kill-switches --
            //    bWeaponStyleControl off still means MFO never touches weapon
            //    stances, iCastControl==0 still means it never touches the
            //    caster stance, override or not.
            if (forced == CombatStyle::Stance::Cast) {
                if (Config::g_castControl.load() > 0) stance = forced;
            } else if (Config::g_weaponStyleControl.load()) {
                stance = forced;
            }
        } else if (Config::g_castControl.load() > 0) {
            if (const RE::FormID cs = CasterConsent::WantedSpell(id)) {
                float cost = 0.0f;
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(cs))
                    cost = sp->CalculateMagickaCost(f);   // marth: cast till DRY, no reserve floor
                const float mag = f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
                if (mag >= cost) {
                    stance = CombatStyle::Stance::Cast;
                } else if (Config::g_weaponStyleControl.load()) {
                    // Magicka dry -> pure melee until it regenerates. This writes
                    // MFO_MeleeStyle, so it honours the weapon-style kill-switch:
                    // with it off, leave the stance to the AI (None -> no update).
                    stance = CombatStyle::Stance::Melee;
                }
            }
        }
        if (stance != CombatStyle::Stance::None) {
            // DUAL-WIELD GATE (marth 2026-10-27, perk/class gated): the melee stance allows the engine AI to dual
            // wield (MFO_MeleeStyle, kAllowDualWielding) ONLY for a follower whose perks vote dual wield -- the same
            // offHand == 2 predicate that fills his left hand (cast/Equip.cpp). Everyone else gets the single-weapon
            // twin, so an unperked follower carrying two one-handers is not AI-dual-wielded.
            const bool allowDual = stance == CombatStyle::Stance::Melee &&
                                   Logistics::CombatStyleFor(f, it->second).offHand == 2;
            CombatStyle::Want(id, stance, stanceIsEquipOrder, allowDual);
        }

        // T#76: EQUIP FORCE-HOLD / STANCE-CLAMP LIFECYCLE (gambit TRUE -> FALSE).
        // While the equip gambit's condition held this tick, `equipHeld` names
        // its category (superset of wantStance: also set inside the equip's own
        // suppression window, so the ticks right after a swap are NOT read as
        // "condition false"): above it clamped the CSTY stance, and this
        // reconcile keeps the weapon force-equipped. When the condition goes
        // FALSE / is superseded (equipHeld == 0), release BOTH so the follower's
        // AI reverts and a mage can cast again -- the critical correctness path
        // (a weapon force-locked or a stance clamped forever is worse than the
        // oscillation). The two releases are scoped so a genuinely-active other
        // owner survives:
        //  - ReconcileForcedWeapon releases the weapon unless an equip gambit of
        //    the forced category held this tick (a melee<->ranged flip re-forced
        //    the new weapon in EquipWeapon, so it stays consistent).
        //  - ReleaseEquipOrder drops the stance ONLY if it is still an equip
        //    order: if a cast latch or the #65 override set a stance this tick,
        //    the Want above already flipped equipOrder off, so their hold stands.
        // SEV-2 (Fable 2026-08-17): release only when the equip condition is
        // KNOWN false -- AND the T#76 hysteresis dwell has expired (`equipReleaseOK`
        // = equipCondKnownFalse && dwellExpired, computed above the stance block).
        // equipCondKnownFalse is true when the scan ran to completion (nothing
        // matched) OR the equip rule sat ABOVE the stop and did not hold
        // (positional known-false); a scan stopped by a higher rule with the equip
        // rule BELOW it leaves the truth UNKNOWN, and releasing then yanks the
        // weapon out mid-fight the tick a higher rule fires (the re-oscillation T#76
        // exists to close). Keep the hold on an unknown tick OR within the commit
        // dwell; combat-end / death / dismissal / revert still release
        // unconditionally elsewhere.
        // SILVER FIRST (ClickUp 86e3p2gp9): once its rule is known false for its own dwell, the
        // silver hold is turned back into the main weapon BEFORE the general reconcile below, so
        // that reconcile sees the main weapon (or no hold), never a silver hold to strip bare.
        Actuation::ReconcileSilver(f, silverHeld, silverKnownFalse, equipCondKnownFalse && equipHeld == 0);
        Actuation::ReconcileForcedWeapon(f, equipHeld, equipReleaseOK);
        // AnyActive() is a lock-free atomic mirror -- keep the common no-stance
        // tick off g_mx (the module's own discipline). The kill-switch also drops
        // the stance clamp: with bWeaponStyleControl off, branch 1 never set an
        // equip Want and ApplyTick/gate have no config check, so a hold from
        // before the flip would strand the CSTY/gate until combat end otherwise.
        if ((equipHeld == 0 && equipReleaseOK && CombatStyle::AnyActive()) ||
            (!Config::g_weaponStyleControl.load() && CombatStyle::HoldsEquipOrder(id))) {
            CombatStyle::ReleaseEquipOrder(id);
            g_meleeClampTrueAt.erase(id);   // dwell consumed -> reset for the next clamp
        }
        // Harbinger client mode: the equip order as Harbinger's ch.15 claim where no
        // ledger hold carries one (no-op with Harbinger absent: the gate is installed).
        CombatStyle::ServiceOrderClaim(f);

        // H3 -- SCAN-AWARE SPELL RELEASE (the frequency limiter, fall-through
        // aware). A gambit spell stays in the follower's hand only while some
        // cast rule still wants it -- because their AI casts what they hold,
        // and MFO controls what they hold. Under the scan, "wants it" means
        // the rule's CONDITION held this tick (fired, in grace, debounced, or
        // suppressed-in-window) even when a lower rule ended up acting --
        // releasing on "the final winner is not a cast" would yank the spell
        // mid-grace every time a skipped cast let a lower rule run (D6).
        //
        // v1.0.32: the CONSENT latch is deliberately NOT released here any
        // more. H3 fires on a SINGLE service tick where no cast condition
        // held -- but the engine's combat casters tick many times inside one
        // service interval, so a momentary condition flicker (foe HP hovering
        // on a threshold, a target swap) dropped the deny for a whole
        // service-to-service gap and the AI slipped its OWN spell in through
        // it -- the sub-tick leak v1.0.30's cooldown-spanning latch still
        // had. Once a cast rule has been winning, exclusive control now holds
        // for the COMBAT'S DURATION; the release points are combat end (the
        // non-combat branch above, so it can never outlive the fight),
        // dismissal (Followers::Refresh), and revert/load (ClearTransient-
        // State). The SPELL still releases on H3 -- hand occupancy is pacing,
        // and their AI cannot cast what they are not holding either way.
        // HEAL CLAIMS LAST UNTIL THE RECIPIENT IS FULL (ClickUp 86e3m36qr): a standing heal no rule asked for
        // this tick is kept and driven on by HealSustainLap; its hand(s) are exempt from the release below.
        const unsigned healHeld = Actuation::HealSustainLap(f, matchedCast);
        if (!castSeen) {
            if (healHeld == 0) Loadout::ReleaseSpell(id, /*a_includeRight=*/true);   // a held heal's spell stays in hand; MFO-B1: the right-hand offense spell too
            // Crisp release of the APMF offense-cast facet-CLAIM (per-cast, ch.8b
            // kIntent_Cast, PORTED feat/offense-cast-seats off the retired ch.8
            // kIntent_SelectSpell claim this call used to release) the moment no
            // cast rule's condition holds -- the same "the cast gambit stopped
            // wanting it" signal that releases the spell in hand. This releases
            // ONLY the offense-cast claim; the combat-TARGET claim is per-COMBAT
            // and is deliberately NOT dropped here (kept alive by
            // RefreshCombatTarget above, released only at combat end), so a
            // cast->melee transition keeps the target facet claimed rather than
            // releasing it. No-op when APMF is absent / no offense-cast claim held.
            APMFBridge::ReleaseOffenseCast(id);
            // ...and the HEAL claim with it (animheal phase 2, review F6): the same
            // "no cast rule wants it" signal, so the seats cannot drive one more
            // heal at a recipient that no longer qualifies while the ~2.45 s
            // FacetExpiry sweep catches up. No-op when no heal claim stands.
            if (healHeld == 0) ComposedCast::End(id);
            // Clear the shared [cfc] silent-claim watch this claim armed
            // (Actuation::CastOn's ComposedCast::WatchClaim call) -- a no-op if
            // nothing was armed (heal claims clear their own watch via
            // ComposedCast::End instead).
            if (healHeld == 0) ComposedCast::ClearWatch(id);
            // Task 2: no cast rule's condition held this tick at all, so
            // whatever the lock was protecting is no longer being requested
            // either -- release it now rather than riding out its own
            // live-check/staleness window.
            if (healHeld == 0) Actuation::ClearCastLock(id);
            else Actuation::ReleaseUnheldCastHands(id, healHeld);   // the held heal's hand(s) stay
        }

        const char* name = f->GetName() ? f->GetName() : "?";   // flair #11

        if (!stopped) {
            // §4.4: nothing acted -> NO ENGINE CALL was made. Either no rule
            // matched, every match was transparent, or the scan reached the
            // suppression window. 1.4: keep the fall-through legible with ONE
            // skip-chain line, dedup'd on the whole chain (#22j) so a stable
            // situation logs once, not at 7.5 Hz.
            if (!chain.empty()) {
                std::string line = std::format("rules {} skipped -> {}", chain,
                                               suppressed ? "suppressed (window)"
                                                          : "no action");
                if (recent.lastChain != line) {
                    recent.lastChain = std::move(line);
                    spdlog::info("[eval] {} ({:08X}) {}", name, id, recent.lastChain);
                }
            } else if (!suppressed) {
                // EMPTY chain, not suppressed: Evaluate returned no rule on the
                // first pass -- EVERY condition in the table was false this lap.
                // Once per follower per fight (g_noneMatchedNoted), so a table
                // that is visibly RUNNING and matching nothing (an own-OOC
                // follower inside a party fight: every foe-keyed rule reads his
                // own empty group) is distinguishable from a table that never
                // ran. Re-armed in the party-OOC branch.
                if (g_noneMatchedNoted.insert(id).second) {
                    std::size_t nRules = 0;
                    if (const auto rr = g_followers.find(id); rr != g_followers.end())
                        nRules = rr->second.combat().size();
                    spdlog::info("[eval] {} ({:08X}) combat table: {} rules, none matched", name, id, nRules);
                }
            }
            // The combat table took no action -> this tick's one action slot goes
            // to logistics, UNLESS a cast rule's condition held (castSeen): then
            // the cast facet is the combat table's for this lap (see the lambda).
            serviceOwnOoc(/*castFacetHeld=*/castSeen);
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
        }

        // The source re-finds rather than reusing an iterator: Fire() dispatches engine
        // events synchronously, and INVARIANTS #2 says re-find live records by
        // key at act time instead of holding one across an engine call.
        src.Record(choice, outcome, now);

        // §HMS (F3): a FIRED combat gambit is the REAL off-class-usage signal.
        // Publish the action's exercised pool into ProgAllocator's race-free
        // per-follower mirror (worker thread here; the main HMS poll consumes it).
        // Self-gating: a no-op when HMS is off or the action is neutral.
        if (outcome.result == Actuation::Result::Fired)
            ProgAllocator::NoteCombatFire(f, choice.actionOpcode);

        switch (outcome.result) {
        case Actuation::Result::Fired:
            // Suppress only on a REAL action. A wait or a failed cast must not
            // buy quiet time, or a follower who cannot afford a heal would go
            // silent instead of falling through on the next tick.
            //
            // FLAIR #2 -- the window is TEMPO'd per follower (+-12% by the
            // temperament seed) so a shared trigger no longer opens and closes
            // the whole party's windows in lockstep. The MCM knob stays the
            // center; the [eval] timestamps keep the actual cadence readable.
            recent.until = now + std::chrono::milliseconds(static_cast<int>(
                Config::g_suppressWindow.load() * 1000.0f *
                (0.88f + 0.24f * Temperament(id))));
            recent.firedRule = choice.rank;
            // #11: record the fired rule's IDENTITY so the next tick's window
            // re-anchors to its current index after any board edit.
            recent.firedUID = src.FiredIdentity(choice);
            recent.failRule  = -1;
            recent.failReason.clear();
            recent.lastChain.clear();
            // 1.4 / Decision 3: when the fire came through a skip chain, ONE
            // line carries the whole story; otherwise the classic fired line.
            if (chain.empty()) {
                spdlog::info("[eval] {} ({:08X}) fired rule {} ({})",
                             name, id, choice.rank, choice.actionOpcode);
            } else {
                spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} fired ({})",
                             name, id, chain, choice.rank, choice.actionOpcode);
            }
            break;

        case Actuation::Result::FailedSkill:
        case Actuation::Result::FailedOther:
            // §5.3: say WHY -- but ON TRANSITION ONLY. A permanently failing
            // rule is the WINNING rule every tick (failures correctly do not
            // suppress), so logging it unconditionally means ~7.5 lines/sec
            // per follower, each with a synchronous flush on the main thread:
            // both a frame cost and a flood that drowns the signal (#22j).
            // (Post-H1, failures are transparent and rarely terminate the
            // scan; this case remains for any opaque wall left by design.)
            if (recent.failRule != choice.rank || recent.failReason != outcome.reason) {
                recent.failRule   = choice.rank;
                recent.failReason = outcome.reason;
                if (chain.empty()) {
                    spdlog::info("[eval] {} ({:08X}) rule {} ({}) did NOT fire: {}",
                                 name, id, choice.rank, choice.actionOpcode, outcome.reason);
                } else {
                    spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} ({}) did NOT fire: {}",
                                 name, id, chain, choice.rank, choice.actionOpcode, outcome.reason);
                }
            }
            break;

        case Actuation::Result::NoOp:
            // LOG THE SILENT PATH TOO, on transition.
            //
            // A NoOp with a reason is a decision MFO made -- "giving their AI a
            // chance", "caster has not selected the spell yet", "already on that
            // target". Every one of those was invisible, and a whole field
            // session was spent on a probe whose most likely outcome was a
            // silent NoOp: the log showed no [drive] line and no rule 0, which
            // read as "nothing happened" when it may have been happening every
            // tick. That is #53 in the one place it costs a test.
            //
            // Reasonless NoOps (act.wait, no rule matched) stay quiet -- those
            // really are nothing.
            if (!outcome.reason.empty() &&
                (recent.failRule != choice.rank || recent.failReason != outcome.reason)) {
                recent.failRule   = choice.rank;
                recent.failReason = outcome.reason;
                if (chain.empty()) {
                    spdlog::info("[eval] {} ({:08X}) rule {} ({}) held off: {}",
                                 name, id, choice.rank, choice.actionOpcode, outcome.reason);
                } else {
                    spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} ({}) held off: {}",
                                 name, id, chain, choice.rank, choice.actionOpcode, outcome.reason);
                }
            }
            break;

        default:
            break;
        }

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        g_lastTickMs = ms;
        if (Config::g_profileEvaluator.load()) {
            spdlog::info("[eval-profile] tick #{} on {:08X}: {:.3f} ms ({} active)",
                         g_ticks.load(), id, ms, active.size());
        }
    }

}
