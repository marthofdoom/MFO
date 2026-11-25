// STRICT MOVEMENT ORDER TESTS (DEV BUILDS ONLY; hm1-seat-brief.md 13.2, tight-movement-plan.md 10; Harbinger ch.24 kApproach_Strict, ABI v48,
// piece P-A: the implicit Stand order). Tests strict.*: they run right after g0b.cover.
// QUESTION: told to stand on a point, does he stand there (R 64 and R 32), fight from there as often as he fights free, and go back to his own
// fight when the order ends? A test is a list of LEGS (Tests()). One line per leg: [strict] RESULT <test> leg n/m ... PASS|FAIL|MEASURED|SKIP.
//   Free    = no order. The baseline of his attack rate (and, at a test's end, the proof that he fights free again).
//   Stand   = one strict claim (apmf/DevHold.cpp), measured from his arrival, knocked / staggered / down time excluded. Then released.
//   Release = a Stand leg, then the order is released with the foe some way off: how long until he moves or attacks.
// NOT SERVED (Harbinger absent, under ABI 48, kMoveCaps_Strict off, or the claim refused by CanServe): SKIP with the reason, nothing is set up,
// and the test runs again on a load where the ABI is there (Runner.cpp ApplyCodeRetest). Nothing is ever filed as a plain area instead.
// A lunge may carry a melee follower off the point (ruling): a melee leg passes by the inside share OR by every excursion being walked back in time.
// ANY FOLLOWER, ANY PLACE: role = what he holds (stand_bow / stand_melee give theirs), the test lends weapon, arrows, temp class and health,
// spawns the foe. The point is searched on the navmesh, then around him, and his own spot is always a legal last answer.
// NOT MEASURED HERE (Harbinger's own log says it): path restarts, the leaf histogram, the area geometry after the release.
// To add a leg or a test: a row in Tests() (append, bump kCount). To change a pass number: kT.
// THREADS: run state WORKER-ONLY. The planner and the health loan are MainThread::Posts (FormIDs captured, re-resolved); the plan answers are
// under the LEAF lock g_planMx. Requests only through APMFBridge. Claims end at leg end, HoldEnd (every way a run ends), ClearHoldState.
#include "selftest/SelfTest_internal.h"
#include "MainThread.h"
#include "apmf/APMFBridge.h"
#include "classes/Classes.h"
#include "Followers.h"
#include "Sightline.h"   // the bow lane: his own ray to the foe, a teammate in the line

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace MFO::SelfTest {

    namespace {
        struct Tuning {
            float insideBow = 0.97f, insideMelee = 0.95f;   // share of held time inside R
            float maxBow = 15.0f;                            // farthest he may stand past R, bow
            float rateBow = 0.7f, rateMelee = 0.8f;          // of his free-fight rate
            float walkBackSpeed = 60.0f, walkBackPlus = 1.0f;   // an excursion of D units must be over in D / speed + plus seconds
            float arriveSpeed = 60.0f, arrivePlus = 3.0f;    // arrival within path / backpedal speed + plus
            float relMoveV = 25.0f;                          // a speed that counts as "he moves again"
            float relIn = 1.5f;                              // seconds (11-25: one area period 1.0 s plus a path build, Harbinger measured 1.06 s)
            float relWatch = 4.0f;
            float relFoe = 250.0f, relFoeWait = 8.0f;        // the release waits this long for the foe to be at least this far
            float freeShare = 0.5f;                          // drift and attacks after the order, of those before it
            float minRate = 3.0f;                            // a baseline under this many per minute cannot judge a rate
            float fightWait = 20.0f, planWait = 8.0f, roleWait = 25.0f, roleLost = 2.0f;
            float startMin = 130.0f, startMax = 320.0f, foeGap = 130.0f, playerReach = 1200.0f;
            float neverLive = 5.0f;                          // filed, in combat, and no phase after this long
            float healEvery = 2.0f;
            // 11-25 BOW LANE (diag-strict-1125 A2): a bow point needs a line to the foe (his own ray, no teammate within the fire-line pad). Blocked this
            // long = re-point (at most maxRepoints times, the existing Repoint road); a leg whose lane share ends under laneMin is INVALID, not a verdict.
            float laneRepointSec = 3.0f, laneMin = 0.8f;
            int   maxRepoints = 2;
            float foeHealth = 20000.0f;
        };
        constexpr Tuning kT{};

        enum class Kind : std::uint8_t { Free, Stand, Release };
        enum class Who : std::uint8_t { Bow, Melee, AsHeld };
        struct Leg { const char* what; Kind kind; float radius; float sec; bool askFree = false; bool judge = true; float minInside = 0.0f; };   // minInside > 0: a floor on the inside share, lunges or not   // Stand: sec = held time after arrival
        struct Test { const char* id; const char* needs; Who who; RE::FormID foe; float foeAt; std::vector<Leg> legs; };
        constexpr RE::FormID kMelee = 0x0002D1DE, kArcher = 0x0002D1FC;   // Skyrim.esm skeleton warrior / archer (TestsHold.cpp)
        constexpr RE::FormID kIronSword = 0x00012EB7, kHuntingBow = 0x00013985, kIronArrow = 0x0001397D;

        const std::vector<Test>& Tests() {
            static const std::vector<Test> k = {
                { "strict.stand_bow", "(any follower, any place) a bow is lent; stands on a point with a melee foe in contact, R 64 and R 32", Who::Bow, kMelee, 700.0f,
                  { { "free fight", Kind::Free, 0, 15 }, { "stand", Kind::Stand, 64, 20 }, { "stand", Kind::Stand, 32, 20 } } },
                { "strict.stand_melee", "(any follower, any place) a one-hander is lent; stands on a point with a melee foe in contact, R 64 and R 32", Who::Melee, kMelee, 700.0f,
                  { { "free fight", Kind::Free, 0, 15 }, { "stand", Kind::Stand, 64, 20 }, { "stand", Kind::Stand, 32, 20 } } },
                { "strict.keep", "(any follower, any place) one Stand order left alone for 30 s: FAIL under 90% inside", Who::AsHeld, kMelee, 700.0f,
                  { { "kept", Kind::Stand, 64, 30, false, true, 0.90f } } },
                { "strict.release", "(any follower, any place) a Stand order released with the foe at a distance: he fights again within 1.5 s", Who::Melee, kArcher, 700.0f,
                  { { "released", Kind::Release, 64, 6 }, { "released", Kind::Release, 64, 6 } } },
                { "strict.free", "(any follower, any place) the same free fight before and after a Stand order", Who::AsHeld, kMelee, 700.0f,
                  { { "free before", Kind::Free, 0, 15 }, { "stand between", Kind::Stand, 64, 6, true, false }, { "free after", Kind::Free, 0, 15 } } },
            };
            return k;
        }

        // ── main thread: the point ───────────────────────────────────────────────────────────────
        struct Plan { bool ok = false; RE::NiPoint3 pt{}; float walk = 0.0f; std::string why; bool ownSpot = false; };
        std::mutex                              g_planMx;   // LEAF
        std::unordered_map<std::uint32_t, Plan> g_plans;
        std::uint32_t                           g_ticket = 1;   // WORKER

        // a_bow: the point must have a line to the foe (BowLane). a_not: a re-point must differ from the point he stands on.
        bool BowLane(RE::Actor* a_f, RE::Actor* a_foe, const RE::NiPoint3& a_p) {
            if (!a_foe) return true;
            if (Sightline::PointSeenNow(a_foe, a_p, 128.0f) == Sightline::PointSight::Hidden) return false;   // a wall; Unavailable is not a wall
            return !Sightline::TeammateInFireLineFrom(a_f->GetFormID(), a_p, a_foe->GetFormID());
        }
        Plan PlanOnMain(RE::FormID a_f, RE::FormID a_foe, bool a_bow, bool a_haveNot, RE::NiPoint3 a_not) {
            Plan  o;
            auto* f  = RE::TESForm::LookupByID<RE::Actor>(a_f);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!f || !pc || f->IsDead() || !f->Is3DLoaded()) { o.why = "the follower is gone"; return o; }
            auto* foe = a_foe ? RE::TESForm::LookupByID<RE::Actor>(a_foe) : nullptr;
            if (foe && (foe->IsDead() || !foe->Is3DLoaded())) foe = nullptr;
            const auto me = f->GetPosition();
            int lanes = 0;
            const auto fit = [&](const RE::NiPoint3& p) {
                const float d = me.GetDistance(p);
                return d >= kT.startMin && d <= kT.startMax && p.GetDistance(pc->GetPosition()) <= kT.playerReach &&
                       (!foe || p.GetDistance(foe->GetPosition()) >= kT.foeGap) && (!a_haveNot || p.GetDistance(a_not) >= 60.0f) &&
                       (!a_bow || (++lanes <= 12 && BowLane(f, foe, p)));   // bounded: each lane read is three rays under the world lock
            };
            std::string nav;
            for (const auto& n : WalkPoints(f, foe, 450.0f, nav)) {
                if (!fit(n.p)) continue;
                o = { true, n.p, n.walk, std::format("by the navmesh, walk {:.0f}u", n.walk) };
                return o;
            }
            for (int i = 0; i < 8; ++i)
                for (const float d : { 250.0f, 180.0f }) {
                    std::uint32_t st = 0;
                    RE::NiPoint3  p{};
                    if (!APMFBridge::FindStandPoint(a_f, f->GetAngleZ() + 0.7854f * static_cast<float>(i), d, 40.0f, p, st) || !fit(p)) continue;
                    o = { true, p, me.GetDistance(p), "open ground (navmesh: " + nav + ")" };
                    return o;
                }
            if (a_haveNot) { o.why = "no other point with a clear lane here"; return o; }
            if (a_bow && !BowLane(f, foe, me)) { o.why = "no point with a clear lane to the foe (his own spot included; navmesh: " + nav + ")"; return o; }
            o = { true, me, 0.0f, "HIS OWN SPOT, no other point here (navmesh: " + nav + ")", true };
            return o;
        }
        std::uint32_t PostPlan(RE::FormID a_f, RE::FormID a_foe, bool a_bow, const RE::NiPoint3* a_not = nullptr) {
            const auto t = g_ticket++;
            const bool haveNot = a_not != nullptr;
            const RE::NiPoint3 notPt = a_not ? *a_not : RE::NiPoint3{};
            MainThread::Post([t, a_f, a_foe, a_bow, haveNot, notPt]() {
                Plan p = PlanOnMain(a_f, a_foe, a_bow, haveNot, notPt);
                std::lock_guard lk(g_planMx);
                g_plans[t] = std::move(p);
            });
            return t;
        }
        std::optional<Plan> TakePlan(std::uint32_t a_t) {
            std::lock_guard lk(g_planMx);
            const auto it = g_plans.find(a_t);
            if (it == g_plans.end()) return std::nullopt;
            auto p = std::move(it->second);
            g_plans.erase(it);
            return p;
        }
        // The test must not cost him his life or end in bleedout: damage on his Health is given back while it runs.
        void PostHeal(RE::FormID a_f) {
            MainThread::Post([a_f]() {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(a_f);
                auto* avo = f && !f->IsDead() ? f->AsActorValueOwner() : nullptr;
                if (!avo) return;
                const float lost = -f->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
                if (lost > 0.0f) avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, lost);
            });
        }

        // ── the runs (WORKER ONLY) ───────────────────────────────────────────────────────────────
        enum class Phase : std::uint8_t { Start, Planning, Filing, Active };
        struct Stats {
            float t = 0, heldT = 0, insideT = 0, excludedT = 0, arriveAt = -1, startD = 0, drift = 0, moveT = 0;
            float maxD = 0, worstBack = 0, worstOver = 0, roleLostT = 0, leashT = 0, notCarriedT = 0, phaseT[9]{};
            std::vector<float> dist;               // per sample after the arrival
            int   excursions = 0, inAttack = 0, late = 0, attacks = 0, shots = 0, attacks0 = 0, shots0 = 0;   // the 0s: at the arrival
            // 11-25: attacks counts None -> attack EDGES, so a chain of 8 swings counted 1. swings = entries into a swing or bash state (chained ones too).
            int   swings = 0, swings0 = 0, chain = 0, chainMax = 0, travelStarts = 0, travelSwings = 0;
            float laneT = 0, laneBlockedT = 0, laneRun = 0, laneAskT = 0, repointAt = 0;
            bool  repointing = false;
            int   repointsN = 0;
            bool  out = false, have = false, everLive = false, dropped = false;
            float outT = 0, outMax = 0;
            RE::NiPoint3 prev{};
            int   prevAtk = 0;
            std::uint32_t lastPhase = 99, gen = 0, displaced = 0, passed = 0, genChanges = 0, applied = 0;
            float stopError = 0;
            float maxStopError = 0;
            std::string timeline;
            const char* state = "none";
            // Release
            bool  released = false;
            float relT = 0, relMoved = -1, relAtk = -1, relFoeD = -1, relWait = 0;
            std::uint32_t relPhase = 0;
            std::int32_t  setFree = -1;
            bool          askedFree = false;
        };
        struct SRun {
            int   test = -1, leg = 0, redo = 0, passed = 0, failed = 0, measured = 0, skipped = 0;
            bool  bow = false;
            Phase phase = Phase::Start;
            float phaseT = 0, waitT = 0, healT = 0, pathLen = 0;
            std::uint32_t ticket = 0, repointTicket = 0;
            int           repoints = 0;
            RE::FormID    foe = 0;
            RE::NiPoint3  pt{};
            bool          ownSpot = false;   // the planner found no other point: the arrival proves nothing
            bool          ending = false;
            std::string   endWhy, summary, hands;
            Stats st;
            float baseAtk = -1, baseDrift = -1;   // the first Free leg, per minute / units per second
        };
        std::unordered_map<RE::FormID, SRun> g_sruns;

        void Toast(std::string a_text) {
            MainThread::Post([t = std::move(a_text)]() { RE::DebugNotification(t.c_str()); });
        }
        // 1 = bow / crossbow, 0 = melee weapon or fists, -1 = something else (staff, spell)
        int HeldKind(RE::Actor* a) {
            auto* o = a->GetEquippedObject(false);
            if (!o) return 0;
            if (auto* w = o->As<RE::TESObjectWEAP>()) return w->IsStaff() ? -1 : w->IsBow() || w->IsCrossbow() ? 1 : 0;
            return -1;
        }
        std::string Hands(RE::Actor* a) {
            auto* r = a->GetEquippedObject(false);
            auto* l = a->GetEquippedObject(true);
            return std::format("R '{}' L '{}'", r && r->GetName() ? r->GetName() : "-", l && l->GetName() ? l->GetName() : "-");
        }
        float Quantile(std::vector<float> v, float q) {
            if (v.empty()) return -1.0f;
            std::sort(v.begin(), v.end());
            return v[static_cast<std::size_t>(q * static_cast<float>(v.size() - 1) + 0.5f)];
        }
        void ReleaseClaim(RE::FormID a_id) { APMFBridge::DevHoldRelease(a_id); }

        float MayBeOut(const Stats& s) { return s.outMax / kT.walkBackSpeed + kT.walkBackPlus; }
        void  CloseExcursion(Stats& s) {
            const float may = MayBeOut(s);
            s.worstBack = std::max(s.worstBack, s.outT);
            s.worstOver = std::max(s.worstOver, s.outT - may);
            if (s.outT > may) ++s.late;
            s.out = false;
        }

        // WORKER, every unpaused tick.
        void Sample(RE::Actor* a, RE::FormID a_id, SRun& r, float dt) {
            auto&       s   = r.st;
            const auto& leg = Tests()[r.test].legs[r.leg];
            s.t += dt;
            r.healT += dt;
            if (r.healT >= kT.healEvery) { r.healT = 0.0f; PostHeal(a_id); }
            const auto end = [&](std::string why) { r.ending = true; r.endWhy = std::move(why); };
            const auto p   = a->GetPosition();
            float      v   = 0.0f;
            if (s.have && dt > 0.0f) {
                const float step = std::hypot(p.x - s.prev.x, p.y - s.prev.y);
                v = step / dt;
                if (v < 1500.0f) { s.drift += step; s.moveT += dt; }
            }
            bool attackStart = false, busy = false, attacking = false;
            // Travelling or Returning under OUR order (not after the release): Harbinger refuses new attacks then (11-25 fix 2).
            const bool travelling = leg.kind != Kind::Free && !s.released && [&] { const auto ph = APMFBridge::DevStrictRead(a_id).phase; return ph == 2 || ph == 4; }();
            if (auto* as = a->AsActorState()) {
                const int at = static_cast<int>(as->GetAttackState());
                const auto released = [](int x) { return (x >= 11 && x <= 13) || x == 16 || x == 17; };
                attacking = at != 0;
                if (s.have && s.prevAtk == 0 && at != 0) { ++s.attacks; attackStart = true; if (travelling) ++s.travelStarts; }
                if (at == 0) s.chain = 0;
                if (s.have && (at == 2 || at == 6) && !(s.prevAtk == 2 || s.prevAtk == 6)) {   // a swing or bash begins, a chained one too
                    ++s.swings;
                    s.chainMax = std::max(s.chainMax, ++s.chain);
                    if (travelling) ++s.travelSwings;
                }
                if (s.have && !released(s.prevAtk) && released(at)) ++s.shots;
                s.prevAtk = at;
                busy = as->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || as->actorState2.staggered != 0 || as->IsBleedingOut();
            }
            busy = busy || a->IsInKillMove();
            s.prev = p;
            s.have = true;
            if (HeldKind(a) != (r.bow ? 1 : 0)) s.roleLostT += dt; else s.roleLostT = 0.0f;
            if (s.roleLostT > kT.roleLost) return end(std::format("INVALID: the {} left his right hand ({})", r.bow ? "bow" : "melee weapon", Hands(a)));
            if (leg.kind == Kind::Free) {
                if (s.t >= leg.sec) end("leg time up");
                return;
            }
            if (s.released) {   // Release: after the order
                s.relT += dt;
                const auto st = APMFBridge::DevStrictRead(a_id);
                s.relPhase = st.phase;
                if (s.relMoved < 0 && v >= kT.relMoveV && s.relT > dt) s.relMoved = s.relT;
                if (s.relAtk < 0 && attackStart) s.relAtk = s.relT;
                if (s.relT >= kT.relWatch) end("leg time up");
                return;
            }
            // ── under the order ──
            if (APMFBridge::HealApproachStands(a_id) || APMFBridge::DisengageStands(a_id)) return end("INVALID: a held heal's approach or a disengage run outranks the test");
            const auto hold = APMFBridge::DevHoldRead(a_id);
            const auto st   = APMFBridge::DevStrictRead(a_id);
            s.state   = hold.state;
            s.applied = std::max(s.applied, hold.applied);
            if (hold.ended) {
                s.dropped = hold.dropped;
                // 11-25: Harbinger's own [hold] watchdog drops a hold whose fighter makes no "progress" for 20 s and gives the foe up. That measures nothing
                // about the order: the leg is INVALID (it runs again, bounded), never a verdict.
                if (hold.dropped) return end("INVALID: Harbinger's [hold] watchdog dropped the hold (EngineDropped), the foe was given up; the leg runs again");
                return end(std::format("Harbinger ended it: {}", hold.state));
            }
            if (!hold.ours && s.t > 1.0f && !hold.waiting) return end("INVALID: another ch.24 claim owns him");
            if (std::string_view(hold.state) == "Leashed" && (s.leashT += dt) > 1.0f) return end("INVALID: the pursuit leash excluded the point (the player moved off)");
            const auto ph = std::min<std::uint32_t>(st.phase, 8);
            s.phaseT[ph] += dt;
            if (ph != s.lastPhase) {
                if (s.timeline.size() < 380)
                    s.timeline += std::format("{}{:.1f}s {}", s.timeline.empty() ? "" : ", ", s.t, APMFBridge::DevStrictPhaseName(ph));
                else if (!s.timeline.ends_with("...")) s.timeline += " ...";
                s.lastPhase = ph;
            }
            if (st.ours) {
                s.everLive = s.everLive || st.phase >= 2;
                if (s.gen != 0 && st.gen != s.gen) ++s.genChanges;
                s.gen = st.gen;
                s.displaced = st.displaced;
                s.passed |= st.passed;
                s.stopError = st.stopError;
                s.maxStopError = std::max(s.maxStopError, st.stopError);
                if (st.setFree >= 0) s.setFree = st.setFree;
            }
            if (!s.everLive && s.t > kT.neverLive) return end(std::format("the order never answered (ch.24 {}, phase {}, applied {})", hold.state, APMFBridge::DevStrictPhaseName(ph), s.applied));
            const float d = p.GetDistance(r.pt);
            if (s.dist.empty() && s.arriveAt < 0 && s.startD == 0.0f) s.startD = d;
            const bool down = busy || (st.passed & 1u) != 0;   // kMovePass_EssentialDown
            if (s.arriveAt < 0) {
                if (d <= leg.radius && (ph == 3 || d <= 24.0f)) { s.arriveAt = s.t; s.attacks0 = s.attacks; s.shots0 = s.shots; s.swings0 = s.swings; }
                else if (s.t - s.excludedT > std::max(r.pathLen, s.startD) / kT.arriveSpeed + kT.arrivePlus) return end("never arrived in time");
                if (down) s.excludedT += dt;
                if (s.arriveAt < 0) return;
            }
            if (down) { s.excludedT += dt; return; }
            if (s.repointing) {   // walking to the spot he was re-pointed to: not held time
                if (d <= leg.radius && (ph == 3 || d <= 24.0f)) s.repointing = false;
                else {
                    s.excludedT += dt;
                    if (s.t - s.repointAt > r.pathLen / kT.arriveSpeed + kT.arrivePlus) return end("never arrived at the spot he was re-pointed to");
                    return;
                }
            }
            if (r.bow && leg.kind == Kind::Stand) {   // the lane: his own ray to the foe and no teammate in the line
                if (r.foe && (s.laneAskT += dt) >= 0.5f) { s.laneAskT = 0.0f; Sightline::Want(a_id, { r.foe }, Sightline::Basis::Own); }
                const bool blocked = r.foe && (Sightline::TeammateInFireLine(a_id, r.foe) || Sightline::Check(a_id, r.foe, Sightline::Basis::Own) == Sightline::Verdict::Occluded);
                s.laneT += dt;
                if (blocked) { s.laneBlockedT += dt; s.laneRun += dt; } else s.laneRun = 0.0f;
                if (s.laneRun >= kT.laneRepointSec && !r.repointTicket && r.repoints < kT.maxRepoints) { r.repointTicket = PostPlan(a_id, r.foe, true, &r.pt); s.laneRun = 0.0f; }
            }
            if (r.repointTicket) {
                if (auto p = TakePlan(r.repointTicket)) {
                    r.repointTicket = 0;
                    if (p->ok && APMFBridge::DevHoldRepoint(a_id, p->pt, leg.radius)) {
                        spdlog::info("[strict] {:08X} lane blocked {:.1f}s: re-pointed {:.0f},{:.0f},{:.0f} -> {:.0f},{:.0f},{:.0f} ({}/{})", a_id, kT.laneRepointSec, r.pt.x, r.pt.y, r.pt.z,
                                     p->pt.x, p->pt.y, p->pt.z, r.repoints + 1, kT.maxRepoints);
                        ++r.repoints;
                        ++s.repointsN;
                        r.pt = p->pt;
                        r.pathLen = p->walk;
                        s.repointing = true;
                        s.repointAt = s.t;
                        if (s.out) { s.out = false; --s.excursions; }   // the old point's excursion is not the order's
                        return;
                    }
                    spdlog::info("[strict] {:08X} lane blocked: no re-point ({})", a_id, p->ok ? "the repoint call failed" : p->why);
                }
            }
            s.heldT += dt;
            s.dist.push_back(d);
            s.maxD = std::max(s.maxD, d);
            if (d <= leg.radius) s.insideT += dt;
            if (!s.out && d > leg.radius) { s.out = true; s.outT = 0; s.outMax = d; ++s.excursions; if (attacking) ++s.inAttack; }
            if (s.out) {
                s.outT += dt;
                s.outMax = std::max(s.outMax, d);
                const bool back = d <= 24.0f || (ph == 3 && d <= leg.radius);
                if (back || s.outT > MayBeOut(s) + 3.0f) CloseExcursion(s);
            }
            if (leg.askFree && !s.askedFree && s.heldT > 2.0f) { s.askedFree = true; APMFBridge::DevStrictAskFree(a_id); }
            if (s.heldT < leg.sec) return;
            if (leg.kind == Kind::Stand) return end("leg time up");
            // Release: with the foe some way off (bounded wait, then as it is)
            auto* foe = r.foe ? RE::TESForm::LookupByID<RE::Actor>(r.foe) : nullptr;
            s.relFoeD = foe ? foe->GetPosition().GetDistance(p) : -1.0f;
            s.relWait += dt;
            if ((s.relFoeD < kT.relFoe || d > 24.0f + leg.radius || attacking || v >= kT.relMoveV) && s.relWait < kT.relFoeWait) return;
            ReleaseClaim(a_id);
            s.released = true;
        }

        std::string StatLine(const Stats& s, float a_minutesT) {
            const float in = s.heldT > 0.2f ? 100.0f * s.insideT / s.heldT : -1.0f;
            return std::format("arrived {:.1f}s (start {:.0f}u), inside {:.1f}% of {:.1f}s held (excluded {:.1f}s), dist p50/p90/p99/max {:.0f}/{:.0f}/{:.0f}/{:.0f}, "
                               "displacedCount {}, stopError {:.0f} (max {:.0f}), excursions {} (in an attack {}, walked back late {}, worst {:.1f}s), swings {} (longest chain {}, began travelling {}, swings travelling {}), lane clear {:.0f}% ({} re-point), "
                               "standing {:.1f}s travelling {:.1f}s returning {:.1f}s not carried {:.1f}s yielded {:.1f}s pending {:.1f}s, attacks {} shots {} in {:.0f}s, "
                               "EngineDropped {}, order changes {}, applied {}, passed 0x{:X}, ch.24 {} | timeline: {}",
                               s.arriveAt, s.startD, in, s.heldT, s.excludedT, Quantile(s.dist, 0.5f), Quantile(s.dist, 0.9f), Quantile(s.dist, 0.99f), s.maxD,
                               s.displaced, s.stopError, s.maxStopError, s.excursions, s.inAttack, s.late, s.worstBack, s.swings, s.chainMax, s.travelStarts, s.travelSwings,
                               s.laneT > 0.5f ? 100.0f * (1.0f - s.laneBlockedT / s.laneT) : -1.0f, s.repointsN, s.phaseT[3], s.phaseT[2], s.phaseT[4], s.phaseT[7],
                               s.phaseT[6], s.phaseT[1], s.attacks, s.shots, a_minutesT, s.dropped ? 1 : 0, s.genChanges, s.applied, s.passed, s.state, s.timeline);
        }

        void CloseLeg(RE::FormID a_id, SRun& r) {
            const auto& t   = Tests()[r.test];
            const auto& leg = t.legs[r.leg];
            auto&       s   = r.st;
            ReleaseClaim(a_id);
            if (r.repointTicket) { TakePlan(r.repointTicket); r.repointTicket = 0; }
            const float laneShare = s.laneT > 0.5f ? 1.0f - s.laneBlockedT / s.laneT : -1.0f;
            if (r.bow && leg.kind == Kind::Stand && s.laneT >= 3.0f && laneShare < kT.laneMin && !r.endWhy.starts_with("INVALID"))
                r.endWhy = std::format("INVALID: his line to the foe was clear only {:.0f}% of the held time (needs {:.0f}%, {} re-point(s)); the bow could not have shot", 100 * laneShare,
                                       100 * kT.laneMin, r.repoints);
            const bool invalid = r.endWhy.starts_with("INVALID") || r.endWhy.find("CombatEnded") != std::string::npos || r.endWhy.starts_with("the foe died");
            if (invalid && r.redo < 2) {   // it measured the wrong thing: again (bounded)
                ++r.redo;
                spdlog::info("[strict] {:08X} {} leg {}: {}, run again ({}/2)", a_id, t.id, r.leg + 1, r.endWhy, r.redo);
                r.phase = Phase::Start;
                return;
            }
            if (s.out && s.outT > MayBeOut(s)) CloseExcursion(s);   // still out at the leg's end and already late
            const bool  order = leg.kind != Kind::Free;   // under an order only the held time counts
            const float over  = order ? s.heldT : s.t;
            const int   hits  = (r.bow ? s.shots : s.swings) - (order ? (r.bow ? s.shots0 : s.swings0) : 0);   // melee: swings, a chain of 8 is 8
            const float rate  = over > 1.0f ? 60.0f * static_cast<float>(hits) / over : 0.0f;
            const float drift = s.moveT > 1.0f ? s.drift / s.moveT : 0.0f;
            std::string fail, extra, note;   // note: in the RESULT and in the evidence
            if (order && r.ownSpot) note += " [HIS OWN SPOT, the arrival proves nothing]";
            const auto  no = [&](std::string x) { fail += (fail.empty() ? "" : "; ") + std::move(x); };
            bool judged = leg.judge;
            if (leg.kind == Kind::Free) {
                if (r.baseAtk < 0) {
                    r.baseAtk   = rate;
                    r.baseDrift = drift;
                    judged      = false;
                } else {   // the free fight after the order
                    if (r.baseDrift > 20.0f && drift < kT.freeShare * r.baseDrift) no(std::format("he moves {:.0f} u/s, {:.0f} before the order", drift, r.baseDrift));
                    if (r.baseAtk >= kT.minRate && rate < kT.freeShare * r.baseAtk) no(std::format("{:.1f} {}/min, {:.1f} before", rate, r.bow ? "shots" : "attacks", r.baseAtk));
                    if (APMFBridge::DevStrictRead(a_id).phase != 0) no("an order still stands on him");
                }
                extra = std::format("{:.1f} {}/min, drift {:.0f} u/s over {:.0f}s", rate, r.bow ? "shots" : "attacks", drift, s.t);
            } else {
                const float inside = s.heldT > 0.2f ? s.insideT / s.heldT : 0.0f;
                if (s.dropped) no("EngineDropped");
                if (r.endWhy.starts_with("Harbinger ended")) no(r.endWhy);
                if (r.endWhy.starts_with("the order never") || r.endWhy.starts_with("never arrived")) no(r.endWhy);
                if (s.genChanges) no(std::format("the order was replaced {} time(s)", s.genChanges));
                if (s.arriveAt >= 0 && fail.empty()) {
                    if (r.bow) {
                        if (inside < kT.insideBow) no(std::format("inside {:.1f}% < {:.0f}%", 100 * inside, 100 * kT.insideBow));
                        if (s.maxD > leg.radius + kT.maxBow) no(std::format("max {:.0f} > R + {:.0f}", s.maxD, kT.maxBow));
                    } else if (leg.minInside > 0.0f && inside < leg.minInside) {   // a floor lunges do not excuse (strict.keep: 11-25 it read PASS at 70%)
                        no(std::format("inside {:.1f}% < the {:.0f}% floor", 100 * inside, 100 * leg.minInside));
                    } else if (inside < kT.insideMelee && s.late > 0) {   // lunges are allowed: either inside, or every excursion walked back in time
                        no(std::format("inside {:.1f}% < {:.0f}% and {} of {} excursions walked back late (worst {:.1f}s over)", 100 * inside, 100 * kT.insideMelee, s.late,
                                       s.excursions, s.worstOver));
                    }
                    // Harbinger refuses new attacks while he travels or returns (11-25 fix 2): a melee leg sees none begin there. A swing that was already
                    // under way continues and a chained follow-up is the combo seat's (fix 7), so those are data only.
                    if (!r.bow && s.travelStarts > 0)
                        no(std::format("{} attack(s) BEGAN while travelling or returning ({} swing(s) in those phases, longest chain {})", s.travelStarts, s.travelSwings, s.chainMax));
                    // the rate: only a whole leg against a baseline of the same test
                    if (leg.kind == Kind::Stand && leg.judge && t.legs.front().kind == Kind::Free) {   // the stand_* rows
                        const float need = (r.bow ? kT.rateBow : kT.rateMelee) * r.baseAtk;
                        if (r.baseAtk < kT.minRate) {
                            judged = false;
                            note += std::format(" [RATE NOT JUDGED, free {:.1f}/min]", r.baseAtk);
                        } else if (rate < need) no(std::format("{:.1f} {}/min < {:.1f} ({:.1f} free)", rate, r.bow ? "shots" : "attacks", need, r.baseAtk));
                    }
                    if (leg.kind == Kind::Release && s.released) {
                        const float first = s.relMoved < 0 ? s.relAtk : s.relAtk < 0 ? s.relMoved : std::min(s.relMoved, s.relAtk);
                        if (s.relWait >= kT.relFoeWait) {   // let go as it was, perhaps mid-attack or mid-move: the time says nothing
                            judged = false;
                            note += " [RELEASE TIME NOT JUDGED, the foe never came off]";
                        } else if (first < 0 || first > kT.relIn) no(std::format("after the release he moved or attacked at {:.1f}s (> {:.1f}s)", first, kT.relIn));
                        if (s.relPhase != 0) no("an order still stands on him");
                    }
                }
                if (!leg.judge && !s.dropped && !fail.empty()) { r.summary += (r.summary.empty() ? "(" : "; (") + fail + ") "; fail.clear(); }
                extra = StatLine(s, s.t) + std::format(" | {:.1f} {}/min ({:.1f} free), drift {:.0f} u/s", rate, r.bow ? "shots" : "attacks", r.baseAtk, drift);
                if (leg.kind == Kind::Release)
                    extra += std::format(" | released with the foe at {:.0f}u: moved {:.2f}s, attacked {:.2f}s, phase now {}", s.relFoeD, s.relMoved, s.relAtk,
                                         APMFBridge::DevStrictPhaseName(s.relPhase));
                if (leg.askFree) extra += std::format(" | SetMoveOrder(Free) answered {} (0 = Ok, 6 = mode not served yet, -1 = no answer)", s.setFree);
            }
            const char* v = invalid ? "SKIP" : !fail.empty() ? "FAIL" : judged ? "PASS" : "MEASURED";
            if (invalid) ++r.skipped;
            else if (!fail.empty()) ++r.failed;
            else if (judged) ++r.passed;
            else ++r.measured;
            spdlog::info("[strict] RESULT {} leg {}/{} '{}' R={:.0f}: {}{}{} | {:08X} {} {} | {} | end: {}", t.id, r.leg + 1, t.legs.size(), leg.what, leg.radius, v,
                         fail.empty() ? std::string{} : " (" + fail + ")", note, a_id, r.bow ? "bow" : "melee", r.hands, extra, r.endWhy);
            const float in = s.heldT > 0.2f ? 100.0f * s.insideT / s.heldT : 0.0f;
            Toast(leg.kind == Kind::Free ? std::format("MFO {} {}/{} {}: {} ({:.1f}/min)", t.id, r.leg + 1, t.legs.size(), leg.what, v, rate)
                                         : std::format("MFO {} {}/{} {} R{:.0f}: {} ({:.0f}% inside, max {:.0f}, {:.1f}/min)", t.id, r.leg + 1, t.legs.size(), leg.what,
                                                       leg.radius, v, in, s.maxD, rate));
            r.summary += std::format("{}{} R{:.0f} {}{}{} in {:.0f}% max {:.0f} p90 {:.0f} arr {:.1f}s disp {} err {:.0f} {:.1f}/min", r.summary.empty() ? "" : "; ", leg.what, leg.radius,
                                     v, fail.empty() ? std::string{} : " (" + fail + ")", note, in, s.maxD, Quantile(s.dist, 0.9f), s.arriveAt, s.displaced, s.maxStopError, rate);
            ++r.leg;
            r.redo  = 0;
            r.phase = Phase::Start;
        }
        void SkipLeg(RE::FormID a_id, SRun& r, const std::string& a_why) {
            const auto& t = Tests()[r.test];
            ReleaseClaim(a_id);
            spdlog::info("[strict] RESULT {} leg {}/{} '{}': SKIP | {:08X} | {}", t.id, r.leg + 1, t.legs.size(), t.legs[r.leg].what, a_id, a_why);
            r.summary += std::format("{}{} SKIP ({})", r.summary.empty() ? "" : "; ", t.legs[r.leg].what, a_why.substr(0, 80));
            ++r.skipped;
            ++r.leg;
            r.redo  = 0;
            r.phase = Phase::Start;
        }
        Step VerdictOf(SRun& r) {
            auto text = r.hands + " | " + r.summary;
            if (text.size() > 900) text.resize(900);   // 'MFST' evidence is bounded
            if (r.failed) return Step::Fail(std::move(text));
            if (r.passed == 0) return Step::Retry("no leg could be judged: " + text);
            if (const int unrun = static_cast<int>(Tests()[r.test].legs.size()) - r.leg; unrun > 0)
                return Step::Info(std::format("{} passed, {} skipped, {} legs not run: ", r.passed, r.skipped, unrun) + text);
            if (r.skipped) return Step::Info(std::format("{} passed, {} skipped: ", r.passed, r.skipped) + text);
            return Step::Pass(std::move(text));
        }

        Step StepStrict(Ctx& c, int a_test) {
            const auto& t = Tests()[a_test];
            if (c.s->b[3]) {   // not served when the test began: nothing was set up (FixStrict)
                const auto why = APMFBridge::DevStrictServe(c.id, c.follower->GetPosition(), 64.0f);
                return Step::Skip("NOT SERVED: " + (why.empty() ? std::string("refused when the test began") : why));
            }
            auto& r = g_sruns[c.id];
            if (r.test != a_test || !c.s->b[0]) {
                r         = SRun{};
                r.test    = a_test;
                r.bow     = c.s->b[2];
                r.hands   = Hands(c.follower);
                c.s->b[0] = true;
                if (const auto why = APMFBridge::DevStrictServe(c.id, c.follower->GetPosition(), 64.0f); !why.empty()) return Step::Skip("NOT SERVED: " + why);
                std::string why;
                const char* slug = r.bow ? "archer" : "warrior";
                if (c.s->b[1] && !ApplyClass(c.id, slug, 5, false, why)) spdlog::warn("[strict] {:08X} {}: temp class {} refused: {}", c.id, t.id, slug, why);
            }
            c.s->note = std::format("leg {} of {}; {}", r.leg + 1, t.legs.size(), r.summary.substr(0, 300));
            r.phaseT += 0.5f;
            r.foe = c.hostile ? c.hostile->GetFormID() : 0;
            if (r.phase == Phase::Active) {
                if (!r.foe && !r.ending) { r.ending = true; r.endWhy = "the foe died"; }
                if (r.ending) CloseLeg(c.id, r);
                if (r.phase == Phase::Active) return Step::Continue();
            }
            if (r.leg >= static_cast<int>(t.legs.size())) return VerdictOf(r);
            const auto& leg = t.legs[r.leg];
            if (!c.hostile || !APMFBridge::InCombat(c.follower)) {
                r.waitT += 0.5f;
                if (r.waitT > kT.fightWait) {
                    ReleaseClaim(c.id);
                    return r.passed || r.failed ? VerdictOf(r) : Step::Retry("no fight began (is the foe blocked, or did others kill it)");
                }
                return Step::Continue();
            }
            r.waitT = 0.0f;
            if (r.phase == Phase::Start) {
                r.phaseT = 0.0f;
                r.ending = false;
                r.endWhy.clear();
                r.repointTicket = 0;
                r.repoints      = 0;
                r.ticket = leg.kind == Kind::Free ? 0 : PostPlan(c.id, r.foe, r.bow);
                r.phase  = leg.kind == Kind::Free ? Phase::Filing : Phase::Planning;
                return Step::Continue();
            }
            if (r.phase == Phase::Planning) {
                auto p = TakePlan(r.ticket);
                if (!p) {
                    if (r.phaseT > kT.planWait) SkipLeg(c.id, r, "the main-thread planner did not answer");
                    return Step::Continue();
                }
                if (!p->ok) { SkipLeg(c.id, r, p->why); return Step::Continue(); }
                r.pt      = p->pt;
                r.pathLen = p->walk;
                r.ownSpot = p->ownSpot;
                spdlog::info("[strict] {:08X} '{}' {} leg {}/{} '{}': point {:.0f},{:.0f},{:.0f} R {:.0f} ({})", c.id, c.follower->GetName() ? c.follower->GetName() : "?", t.id,
                             r.leg + 1, t.legs.size(), leg.what, r.pt.x, r.pt.y, r.pt.z, leg.radius, p->why);
                r.phase  = Phase::Filing;
                r.phaseT = 0.0f;
            }
            if (r.phase == Phase::Filing) {
                if (HeldKind(c.follower) != (r.bow ? 1 : 0)) {
                    if (r.phaseT > kT.roleWait) SkipLeg(c.id, r, std::format("the {} never came to his right hand ({})", r.bow ? "bow" : "melee weapon", Hands(c.follower)));
                    return Step::Continue();
                }
                r.hands = Hands(c.follower);
                if (leg.kind != Kind::Free) {
                    if (APMFBridge::HealApproachStands(c.id) || APMFBridge::DisengageStands(c.id)) {
                        if (r.phaseT > kT.roleWait) SkipLeg(c.id, r, "a held heal's approach or a disengage run stood");
                        return Step::Continue();
                    }
                    if (const char* no = APMFBridge::DevHoldFile(c.id, r.pt, leg.radius, true)) {
                        spdlog::error("[strict] {:08X} {} leg {}: served, but NOT FILED: {}", c.id, t.id, r.leg + 1, no);
                        ++r.failed;
                        SkipLeg(c.id, r, std::string("not filed: ") + no);
                        return Step::Continue();
                    }
                }
                r.st    = Stats{};
                r.healT = 0.0f;
                r.phase = Phase::Active;
            }
            return Step::Continue();
        }

        const char* PreStrict(Ctx& c) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (c.seenFor < 5.0f) return "5 s in the party";
            if (!c.follower->Is3DLoaded()) return "the follower loaded";
            if (c.inCombat || !pc || pc->IsInCombat()) return "out of combat";
            if (c.follower->IsOnMount()) return "the follower on foot";
            if (pc->GetPosition().GetDistance(c.follower->GetPosition()) > kT.playerReach * 0.6f) return "the player near the follower";
            return nullptr;
        }
        void FixStrict(Ctx& c, FixtureRequest& q, int a_test) {
            const auto& t = Tests()[a_test];
            c.s->b[3] = !APMFBridge::DevStrictServe(c.id, c.follower->GetPosition(), 64.0f).empty();
            if (c.s->b[3]) return;   // nothing to set up: the step closes it as SKIP
            const bool bow = t.who == Who::Bow || (t.who == Who::AsHeld && HeldKind(c.follower) == 1);
            c.s->b[2] = bow;
            c.s->b[1] = Classes::Writable();
            if (bow) {
                q.items.push_back({ VanillaForm(kHuntingBow), 1 });
                q.items.push_back({ VanillaForm(kIronArrow), 200 });
            } else {
                q.items.push_back({ VanillaForm(kIronSword), 1 });
            }
            q.cls.snapshot    = c.s->b[1];
            q.spawn.on        = true;
            q.spawn.clearSpot = true;
            q.spawn.baseLocal = t.foe;
            q.spawn.health    = kT.foeHealth;
            q.spawn.respawns  = 2;
            q.spawn.distance  = t.foeAt;
        }
        Step TimeoutStrict(Ctx& c, int a_test) {
            const auto it = g_sruns.find(c.id);
            if (it == g_sruns.end() || it->second.test != a_test) return Step::Retry("timeout before it began");
            auto& r = it->second;
            return r.passed || r.failed ? VerdictOf(r) : Step::Retry("timeout with nothing judged: " + c.s->note);
        }

        constexpr std::size_t kCount = 5;   // = Tests().size() (checked in RegisterStrict)
        template <int I> void Fix(Ctx& c, FixtureRequest& q) { FixStrict(c, q, I); }
        template <int I> Step Stp(Ctx& c) { return StepStrict(c, I); }
        template <int I> Step Tmo(Ctx& c) { return TimeoutStrict(c, I); }
        template <int... I> void RegisterEach(std::integer_sequence<int, I...>) {
            const auto one = [](int i, auto fix, auto stp, auto tmo) {
                const auto& t   = Tests()[i];
                float       sec = 45.0f;
                for (const auto& l : t.legs) sec += 1.5f * l.sec + 30.0f;
                Register({ t.id, "hm1-seat-brief 13.2", t.needs, sec, &PreStrict, fix, stp, tmo, true });
            };
            (one(I, &Fix<I>, &Stp<I>, &Tmo<I>), ...);
        }
    }

    void RegisterStrict() {
        if (Tests().size() != kCount) {
            spdlog::error("[strict] {} tests in the table, {} expected -- none registered", Tests().size(), kCount);
            return;
        }
        RegisterEach(std::make_integer_sequence<int, static_cast<int>(kCount)>{});
    }

    void StrictTick(float a_dt) {
        for (auto& [id, r] : g_sruns) {
            if (r.phase != Phase::Active || r.ending) continue;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDead() || !a->Is3DLoaded()) {
                APMFBridge::DevHoldRelease(id);
                r.ending = true;
                r.endWhy = "INVALID: the follower died or unloaded";
                continue;
            }
            Sample(a, id, r, a_dt);
        }
    }

    void StrictEnd(RE::FormID a_follower) { g_sruns.erase(a_follower); }

    void ClearStrictState() {
        g_sruns.clear();
        std::lock_guard lk(g_planMx);
        g_plans.clear();
    }
}
