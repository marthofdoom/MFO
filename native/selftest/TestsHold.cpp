// G0b, THE HOLD EXERCISER (DEV BUILDS ONLY; Docs/research/2026-11/movement-plan.md package G0b and its parity section; marth: "add it to
// debug gambit. I'll test it immediately"). Tests g0b.*: the first thing the dev test gambit runs.
//
// QUESTION: does a ch.24 kApproach_Point + Hold claim keep a follower on a small point, and does he still fight from there? A test is a
// list of LEGS (Tests() below). One leg = pick a point (main thread: Harbinger's FindEmptySpace through APMFBridge::FindStandPoint, MFO's
// own rays through Sightline::PointSeenNow), file ONE claim (apmf/DevHold.cpp, the Disengage.cpp road), sample him every pump tick
// (~133 ms), release, print ONE line:
//   [hold] RESULT <test> leg n/m <what> R=<r>: PASS|FAIL|MEASURED|SKIP ...
// PASS = arrived, and inside R for >= 90% of the time after arrival (gate P2, STRICT: marth 11-24 "spots must be strictly held, no margin"; the R + 25 u number is logged as extra data and never counts). FAIL = he was held by our claim and did not.
// SKIP = the leg could not be measured (no such place here, another MFO claim owned the channel, combat ended).
// A TEST's own verdict: INFO (complete, the numbers are in the evidence); FAIL only when the claim was refused or never carried by the
// engine in every leg; RETRY when nothing could be measured.
// YIELD: a held heal's approach and a disengage run outrank the exerciser: the leg releases at once. The offense approach, the spell range
// close and the give-ground step only get COUNTED (asked=): Harbinger's earlier-claim rule keeps ours for the leg's few seconds, otherwise
// no cover or ranged leg could ever run to its end. No shipped code is changed for this.
// OUT OF COMBAT: ch.24 is an in-combat facet (Harbinger reads Waiting and moves nothing). g0b.hold_idle measures exactly that. Holding a
// point out of combat is another facet (ch.19 travel) and is NOT exercised here.
// QUIET: the ch.33 style claim needs a TESCombatStyle, and the engine has no "stand still" one, so the calmest style of the LOAD ORDER is
// looked up at run time (lowest circle + fallback + strafe that still weighs melee, magic and bows). Exerciser only, never shipped.
// To add a leg or a test: a row in Tests() (append, and bump kCount). To add a kind of point: a Where value and a case in PlanOnMain.
// ALONE: every g0b test is TestDef::exclusive (Runner.cpp). ROLE: a fight test fixes the follower's role for the run (his right hand when it
// starts: melee / bow / hostile spell), gives a plain weapon of that kind, and puts him in a matching TEMP class (snapshot, restored), so the
// class rules want the weapon the test is about. A leg starts only with that kind in hand; a leg that loses it, is leashed, or (idle test)
// is broken by a fight is a SKIP, never a FAIL.
// THREADS: run state is WORKER-ONLY. The planner is a MainThread::Post (FormIDs captured, re-resolved), its answer under the LEAF lock
// g_planMx. Engine requests only through APMFBridge. Release: leg end, HoldEnd (every way a run ends), ClearHoldState (load / revert).
#include "selftest/SelfTest_internal.h"
#include "Followers.h"
#include "MainThread.h"
#include "Sightline.h"
#include "logistics/Logistics.h"   // ForEachLoadedRefInRange
#include "apmf/APMFBridge.h"
#include "classes/Classes.h"

#include <array>
#include <cmath>
#include <optional>

namespace MFO::SelfTest {

    namespace {
        // ── tunables of the exerciser, one table ─────────────────────────────────────────────────
        struct Tuning {
            float passInside   = 0.90f;   // gate P2
            float margin       = 25.0f;   // LOGGED ONLY (marth 11-24: strict, no margin): the share of time inside R + this is printed beside the strict inside-R number, never judged
            float coverWalk    = 1400.0f; // how far he may walk to a cover / flank point
            float coverGap     = 250.0f;  // a cover point stays this far from the foe
            int   coverPerFrame = 10;     // engine sight answers (3 rays each) per frame
            float planWait     = 8.0f;    // a plan answer later than this is a "no point"
            float replanEvery  = 2.0f;    // a place that needs geometry is looked for again this often ...
            float replanFor    = 8.0f;   // ... for this long
            float busyWait     = 8.0f;    // how long a leg waits for another MFO ch.24 claim to end
            float fightWait    = 14.0f;   // how long a test waits for its fight to begin
            float foeHealth    = 20000.0f;   // the spawned foe must outlive the test (it is deleted at the end)
            float neverApplied = 4.0f;    // in combat this long with the area never carried: end the leg
            float doorReach    = 1200.0f;
            float clearance    = 40.0f;   // FindEmptySpace clearance (Concealment's order of size)
            float bodyHeight   = 128.0f;
            float startOutside = 60.0f;   // a leg starts at least this far outside its own radius, else "arrived" proves nothing
            float playerReach  = 1200.0f; // points and starts stay this near the player: past his pursuit leash Harbinger refuses the point
            float roleWait     = 25.0f;   // how long a leg waits for the role's weapon to be in hand
            float roleLost     = 2.0f;    // other things in the right hand this long in a leg = not a valid leg
            float leashedMax   = 1.0f;
        };
        constexpr Tuning kT{};
        constexpr float  kTwoPi = 6.2831853f;

        enum class Where : std::uint8_t { Near, Corner, Flank, Cover, Doorway, Same };   // Same = the previous leg's point
        struct Leg {
            const char* what;
            Where       where;
            float       radius;
            float       sec;              // leg length, seconds
            bool        quiet   = false;  // with the ch.33 calm style
            bool        keep    = false;  // do not release at the end: the next leg re-points this claim
            bool        refile  = false;  // re-point the SAME point half way (P3: does a re-file restart his path)
        };
        enum class Who : std::uint8_t { Any, Fighter, Shooter };
        struct HoldTest {
            const char* id;
            const char* needs;
            Who         who;
            RE::FormID  foe;              // Skyrim.esm base, 0 = no foe (out of combat)
            std::vector<Leg> legs;
            float       foeAt = 700.0f;   // where the foe is placed, units in front of him (the cover test: well away, never in contact, field 11-24c)
        };
        // Skyrim.esm bases, read in the master 2026-11-24: skeleton warrior / skeleton archer / novice fire mage.
        constexpr RE::FormID kMelee = 0x0002D1DE, kArcher = 0x0002D1FC, kMage = 0x00044CE5;

        const std::vector<HoldTest>& Tests() {
            static const auto sweep = [](float a_sec) {
                return std::vector<Leg>{ { "hold", Where::Near, 256, a_sec }, { "hold", Where::Near, 150, a_sec },
                                         { "hold", Where::Near, 100, a_sec }, { "hold", Where::Near, 64, a_sec } };
            };
            static const std::vector<HoldTest> k = {
                { "g0b.cover", "(any follower, any place) a point hidden from the foe that he can walk to; none = SKIP with the counts", Who::Any, kArcher,
                  { { "cover", Where::Cover, 100, 15 }, { "cover", Where::Cover, 64, 15 } }, 1100.0f },
                { "g0b.doorway", "(any follower, any place) a doorway within 1200 units, else open ground (PROXY)", Who::Any, kMelee,
                  { { "doorway", Where::Doorway, 150, 15 }, { "doorway", Where::Doorway, 100, 15 } } },
                { "g0b.hold_idle", "out of combat, nobody near", Who::Any, 0, sweep(5) },
                { "g0b.fighter_vs_melee", "a follower with a melee weapon (or fists) in the right hand", Who::Fighter, kMelee, sweep(14) },
                { "g0b.fighter_vs_archer", "a follower with a melee weapon (or fists) in the right hand", Who::Fighter, kArcher, sweep(14) },
                { "g0b.shooter_vs_melee", "a follower with a bow, crossbow or hostile spell in the right hand", Who::Shooter, kMelee, sweep(14) },
                { "g0b.shooter_vs_mage", "a follower with a bow, crossbow or hostile spell in the right hand", Who::Shooter, kMage, sweep(14) },
                { "g0b.keep", "(any follower) a 20 s hold left alone, then one re-pointed half way", Who::Any, kMelee,
                  { { "kept, no re-file", Where::Near, 150, 22 }, { "re-filed half way", Where::Same, 150, 14, false, false, true } } },
                { "g0b.corners", "(any follower) three points in a row on one re-pointed claim", Who::Any, kArcher,
                  { { "corner 1", Where::Corner, 100, 7, false, true }, { "corner 2", Where::Corner, 100, 7, false, true },
                    { "corner 3", Where::Corner, 100, 9 } } },
                { "g0b.flank", "(any follower) a point to the foe's side", Who::Any, kMelee,
                  { { "flank", Where::Flank, 100, 15 }, { "flank", Where::Flank, 64, 15 } } },
                { "g0b.quiet_melee", "(any follower) the same hold with and without the calm ch.33 style, against a MELEE foe (the result is labelled by HIS role)", Who::Any, kMelee,
                  { { "normal", Where::Near, 150, 13 }, { "CALM", Where::Same, 150, 13, true }, { "normal", Where::Same, 150, 13 }, { "CALM", Where::Same, 150, 13, true } } },
                { "g0b.quiet_mage", "(any follower) the same hold with and without the calm ch.33 style, against a MAGE foe (the result is labelled by HIS role)", Who::Any, kMage,
                  { { "normal", Where::Near, 150, 13 }, { "CALM", Where::Same, 150, 13, true }, { "normal", Where::Same, 150, 13 }, { "CALM", Where::Same, 150, 13, true } } },
            };
            return k;
        }

        // ── the main-thread planner ──────────────────────────────────────────────────────────────
        struct Plan { bool ok = false; RE::NiPoint3 pt{}; std::string why; RE::FormID style = 0; std::string styleText; };
        std::mutex                                g_planMx;     // LEAF: nothing is called under it
        std::unordered_map<std::uint32_t, Plan>   g_plans;      // ticket -> answer
        std::uint32_t                             g_ticket = 1; // WORKER

        // MAIN THREAD. The nearest ordinary (not load) door, an open one first.
        bool FindDoor(RE::Actor* a_f, RE::NiPoint3& a_out, std::string& a_text) {
            std::vector<RE::ObjectRefHandle> doors;   // read after the walk: the callback runs inside the cell's walk
            // never RE::TES::ForEachReferenceInRange: on AE it derefs TES+0x140 as the worldspace (Logistics.h; field crash 2026-11-24)
            Logistics::ForEachLoadedRefInRange(a_f->GetPosition(), kT.doorReach, [&](RE::TESObjectREFR* r) {
                auto* b = r ? r->GetBaseObject() : nullptr;
                if (b && b->Is(RE::FormType::Door) && doors.size() < 64) doors.push_back(r->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            float best = 1e9f;
            bool  any  = false;
            for (auto& h : doors) {
                auto p = h.get();
                auto* d = p.get();
                if (!d || d->IsDisabled() || !d->Is3DLoaded() || d->extraList.GetByType<RE::ExtraTeleport>()) continue;
                const auto  pos  = d->GetPosition();
                if (std::fabs(pos.z - a_f->GetPosition().z) > 200.0f) continue;   // the same floor
                const bool  open = RE::BGSOpenCloseForm::GetOpenState(d) == RE::BGSOpenCloseForm::OPEN_STATE::kOpen;
                const float cost = pos.GetDistance(a_f->GetPosition()) + (open ? 0.0f : 100000.0f);
                if (cost >= best) continue;
                best   = cost;
                any    = true;
                a_out  = pos;
                a_text = std::format("door {:08X} ({})", d->GetFormID(), open ? "open" : "closed");
            }
            return any;
        }

        // MAIN THREAD. The calmest combat style that still weighs every kind of attack.
        RE::FormID FindCalmStyle(RE::Actor* a_f, std::string& a_text) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return 0;
            const RE::TESCombatStyle* cur = nullptr;
            if (auto* cc = a_f->GetActorRuntimeData().combatController) cur = cc->combatStyle;
            if (!cur) cur = a_f->GetActorBase() ? a_f->GetActorBase()->GetCombatStyle() : nullptr;
            const RE::TESCombatStyle* best = nullptr;
            float                     bestScore = 1e9f;
            const auto text = [](const RE::TESCombatStyle* s) {
                return std::format("{:08X} circle {:.2f} fallback {:.2f} strafe {:.2f} offense {:.2f} defense {:.2f} melee/magic/ranged {:.2f}/{:.2f}/{:.2f}",
                                   s->GetFormID(), s->closeRangeData.circleMult, s->closeRangeData.fallbackMult, s->longRangeData.strafeMult,
                                   s->generalData.offensiveMult, s->generalData.defensiveMult, s->generalData.meleeScoreMult,
                                   s->generalData.magicScoreMult, s->generalData.rangedScoreMult);
            };
            for (const auto* s : dh->GetFormArray<RE::TESCombatStyle>()) {
                if (!s || s == cur) continue;
                const auto& g = s->generalData;
                const float move = s->closeRangeData.circleMult + s->closeRangeData.fallbackMult + s->longRangeData.strafeMult;
                if (!std::isfinite(move) || move > 0.30f || s->closeRangeData.circleMult > 0.06f || s->closeRangeData.fallbackMult > 0.06f) continue;
                if (g.offensiveMult < 0.30f || g.meleeScoreMult < 0.5f || g.magicScoreMult < 0.5f || g.rangedScoreMult < 0.5f) continue;
                if (s->flags.any(RE::TESCombatStyle::FLAG::kFlankingStyle)) continue;
                float score = move * 10.0f;
                if (cur)
                    score += std::fabs(g.offensiveMult - cur->generalData.offensiveMult) + std::fabs(g.defensiveMult - cur->generalData.defensiveMult) +
                             std::fabs(g.avoidThreatChance - cur->generalData.avoidThreatChance);
                if (score < bestScore || (score == bestScore && best && s->GetFormID() < best->GetFormID())) { bestScore = score; best = s; }
            }
            a_text = std::format("his style now: {} | calm style: {}", cur ? text(cur) : std::string("none"),
                                 best ? text(best) : std::string("NONE in this load order"));
            return best ? best->GetFormID() : 0;
        }

        // MAIN THREAD. A standable point of the asked kind.
        Plan PlanOnMain(RE::FormID a_fid, RE::FormID a_foe, Where a_where, int a_leg, float a_radius) {
            Plan  o;
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            if (!f || f->IsDead() || !f->Is3DLoaded()) { o.why = "the follower is gone"; return o; }
            auto* foe = a_foe ? RE::TESForm::LookupByID<RE::Actor>(a_foe) : nullptr;
            if (foe && (foe->IsDead() || !foe->Is3DLoaded())) foe = nullptr;
            const auto me = f->GetPosition();
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto nearPlayer = [&](const RE::NiPoint3& a_pt) { return player && a_pt.GetDistance(player->GetPosition()) <= kT.playerReach; };
            if (!nearPlayer(me)) { o.why = "the follower is too far from the player (pursuit leash): the player must stand near him"; return o; }
            const char* proxy = "";   // ANY PLACE: no door here = the Near search below, named in the plan line
            if (a_where == Where::Doorway) {
                o.ok = FindDoor(f, o.pt, o.why) && nearPlayer(o.pt);
                if (o.ok) return o;
                proxy = "PROXY for a doorway (none within reach): ";
            }
            const auto stand = [&](RE::FormID a_org, float a_head, float a_dist, RE::NiPoint3& a_pt) {
                std::uint32_t st = 0;
                return APMFBridge::FindStandPoint(a_org, a_head, a_dist, kT.clearance, a_pt, st) && nearPlayer(a_pt);
            };
            RE::NiPoint3 pt{};
            if (a_where == Where::Flank) {
                if (!foe) { o.why = "no foe"; return o; }
                // the foe's side: 90 degrees off his facing, the side nearer to the follower first
                const auto  fp   = foe->GetPosition();
                const float face = foe->GetAngleZ();
                const float side = std::sin(std::atan2(me.x - fp.x, me.y - fp.y) - face) >= 0.0f ? 1.0f : -1.0f;
                for (const float s : { side, -side })
                    for (const float off : { 1.5708f, 1.2217f, 1.9199f })
                        for (const float d : { 250.0f, 180.0f, 330.0f })
                            if (stand(a_foe, face + s * off, d, pt) && pt.GetDistance(fp) > 120.0f) { o.ok = true; o.pt = pt; o.why = "foe's side"; return o; }
                // the straight walks failed: any point he can walk to at the foe's side
                std::string nav;
                for (const auto& n : WalkPoints(f, foe, kT.coverWalk, nav)) {
                    const float d = n.p.GetDistance(fp);
                    const float off = std::fabs(std::remainder(std::atan2(n.p.x - fp.x, n.p.y - fp.y) - face, kTwoPi));
                    if (d < 130.0f || d > 400.0f || off < 1.0f || off > 2.15f || me.GetDistance(n.p) < 160.0f || !nearPlayer(n.p)) continue;
                    o.ok = true;
                    o.pt = n.p;
                    o.why = std::format("foe's side by the navmesh, walk {:.0f}u", n.walk);
                    return o;
                }
                o.why = "no point at the foe's side (straight walks failed; navmesh: " + nav + ")";
                return o;
            }
            // Near / Corner: a clear standable point 250 to 400 units off, not on top of the foe. A corner turns ~110 degrees per leg.
            const float base = a_where == Where::Corner ? f->GetAngleZ() + 1.92f * static_cast<float>(a_leg)
                               : foe ? std::atan2(foe->GetPosition().x - me.x, foe->GetPosition().y - me.y) + 1.5708f * (a_leg % 2 ? -1.0f : 1.0f)
                                     : f->GetAngleZ() + 1.5708f * static_cast<float>(a_leg);
            for (const float d : { 350.0f, 280.0f, 220.0f })
                for (const float off : { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.8f, -1.8f, 3.1f }) {
                    if (!stand(a_fid, base + off, d, pt)) continue;
                    if (me.GetDistance(pt) < std::max(160.0f, a_radius + kT.startOutside)) continue;                                 // clamped to a wall: too short to measure
                    if (foe && pt.GetDistance(foe->GetPosition()) < 150.0f) continue;
                    if (Sightline::PointSeenNow(f, pt, kT.bodyHeight) != Sightline::PointSight::Seen) continue;   // a clear way by his own ray
                    o.ok = true;
                    o.pt = pt;
                    o.why = std::string(proxy) + "open ground";
                    return o;
                }
            std::string nav;
            for (const auto& n : WalkPoints(f, foe, 600.0f, nav)) {   // a tight place: any point he can walk to
                if (me.GetDistance(n.p) < std::max(160.0f, a_radius + kT.startOutside) || !nearPlayer(n.p)) continue;
                if (foe && n.p.GetDistance(foe->GetPosition()) < 150.0f) continue;
                o.ok = true;
                o.pt = n.p;
                o.why = std::string(proxy) + std::format("by the navmesh, walk {:.0f}u", n.walk);
                return o;
            }
            o.why = "no clear standable point around him (navmesh: " + nav + ")";
            return o;
        }

        // COVER: hidden from the REAL foe's eye by the engine's rays (Sightline::PointSeenNow), over the points he can WALK to, nearest walk
        // first. Sliced: kT.coverPerFrame answers a frame, each slice re-resolves its forms. The miss is diagnosable: every count is in `why`.
        struct CoverJob { std::uint32_t ticket = 0; RE::FormID f = 0, foe = 0; std::vector<NavPoint> pts; std::size_t at = 0, asked = 0;
                          int nearMe = 0, nearFoe = 0, farPlayer = 0, seen = 0, dead = 0; std::string nav; };
        void AnswerPlan(std::uint32_t a_t, Plan a_p) {
            std::lock_guard lk(g_planMx);
            g_plans[a_t] = std::move(a_p);
        }
        void CoverSlice(std::shared_ptr<CoverJob> j) {
            auto* f   = RE::TESForm::LookupByID<RE::Actor>(j->f);
            auto* foe = RE::TESForm::LookupByID<RE::Actor>(j->foe);
            auto* pc  = RE::PlayerCharacter::GetSingleton();
            Plan  o;
            if (!f || !foe || !pc || f->IsDead() || foe->IsDead() || !f->Is3DLoaded() || !foe->Is3DLoaded()) {
                o.why = "the follower or the foe is gone";
                return AnswerPlan(j->ticket, std::move(o));
            }
            if (j->nav.empty()) {
                j->pts = WalkPoints(f, foe, kT.coverWalk, j->nav);
                if (j->nav.empty()) j->nav = "?";
            }
            int left = kT.coverPerFrame;
            for (; j->at < j->pts.size() && left > 0; ++j->at) {
                const auto& n = j->pts[j->at];
                if (f->GetPosition().GetDistance(n.p) < 170.0f) { ++j->nearMe; continue; }
                if (foe->GetPosition().GetDistance(n.p) < kT.coverGap) { ++j->nearFoe; continue; }
                if (pc->GetPosition().GetDistance(n.p) > kT.playerReach) { ++j->farPlayer; continue; }
                --left;
                ++j->asked;
                const auto ps = Sightline::PointSeenNow(foe, n.p, kT.bodyHeight);
                if (ps == Sightline::PointSight::Seen) { ++j->seen; continue; }
                if (ps != Sightline::PointSight::Hidden) { ++j->dead; continue; }
                o.ok = true;
                o.pt = n.p;
                o.why = std::format("hidden from the foe, walk {:.0f}u; asked {} of {} points", n.walk, j->asked, j->pts.size());
                return AnswerPlan(j->ticket, std::move(o));
            }
            if (j->at < j->pts.size()) {
                MainThread::Post([j]() { CoverSlice(j); });
                return;
            }
            o.why = std::format("NO COVER: {}; asked {}, seen by the foe {}, ray unavailable {}, under 170u from him {}, under {:.0f}u from the foe {}, past the player's reach {}",
                                j->nav, j->asked, j->seen, j->dead, j->nearMe, kT.coverGap, j->nearFoe, j->farPlayer);
            AnswerPlan(j->ticket, std::move(o));
        }

        std::uint32_t PostPlan(RE::FormID a_f, RE::FormID a_foe, Where a_where, int a_leg, float a_radius, bool a_style, bool a_point) {
            const auto t = g_ticket++;
            if (a_point && a_where == Where::Cover) {
                auto j    = std::make_shared<CoverJob>();
                j->ticket = t;
                j->f      = a_f;
                j->foe    = a_foe;
                MainThread::Post([j]() { CoverSlice(j); });
                return t;   // a cover leg is never a quiet one: no style is asked for
            }
            MainThread::Post([t, a_f, a_foe, a_where, a_leg, a_radius, a_style, a_point]() {
                Plan p;
                if (a_point) p = PlanOnMain(a_f, a_foe, a_where, a_leg, a_radius);
                else p.ok = true;
                if (a_style)
                    if (auto* f = RE::TESForm::LookupByID<RE::Actor>(a_f)) p.style = FindCalmStyle(f, p.styleText);
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

        // ── the runs (WORKER ONLY) ───────────────────────────────────────────────────────────────
        enum class Phase : std::uint8_t { Start, Planning, Filing, Active, Done };
        struct Stats {
            float t = 0, afterT = 0, insideT = 0, insideMT = 0;       // leg seconds / after arrival / inside R after arrival / inside R + margin after arrival
            float arriveAt = -1, startD = 0, walkedTo = 0, walkedIn = 0, maxD = 0, sumD = 0, lastD = 0, sightT = 0, sightN = 0;
            int   samples = 0, escapes = 0, attacks = 0, shots = 0, casts = 0;
            float face[3]{};                                     // facing the foe: front / side / back, seconds
            float gait[3]{};                                     // still / walk / run, seconds (after arrival)
            float move[3]{};                                     // forward / strafe / backpedal, seconds (the whole leg, moving)
            float sprintV = 0;
            bool  prevIn = false, have = false, engineIn = false;
            RE::NiPoint3 prev{};
            int   prevAtk = 0, prevCast[2]{};
            unsigned asked = 0;                                  // which MFO clients asked for ch.24 meanwhile
            std::uint32_t applied = 0;
            float leashT = 0, roleLostT = 0, engineInT = 0, engineT = 0, playerD = 0;   // validity, and the engine's own inside answer
            bool  everOurs = false, styleLive = false;
            const char* state = "none";
        };
        struct HRun {
            enum class Role : std::uint8_t { None, Melee, Bow, Spell } role = Role::None;   // fixed at the run's start
            int   restarts = 0;   // Start again because he stood inside the radius when the claim was due
            int   test = -1, leg = 0, measured = 0, refusedLegs = 0, skipped = 0, redo = 0;
            Phase phase = Phase::Start;
            float phaseT = 0, waitT = 0, replanT = 0, postedT = 0;
            std::uint32_t ticket = 0;
            RE::FormID    foe = 0, style = 0;
            RE::NiPoint3  pt{};
            bool          havePt = false, claimKept = false, refiled = false;
            bool          ending = false;          // set by the sampler: the leg is over, step() closes it
            std::string   endWhy;
            std::string   summary, hands;
            Stats st;
            float qWalk[2]{}, qSec[2]{}, qAtk[2]{}, qCast[2]{}, qIn[2]{}, qInM[2]{}, qAfter[2]{};   // [0] normal, [1] CALM
        };
        std::unordered_map<RE::FormID, HRun> g_hruns;

        void Toast(std::string a_text) {
            MainThread::Post([t = std::move(a_text)]() { RE::DebugNotification(t.c_str()); });
        }

        const char* Outranked(RE::FormID a_f) {
            if (APMFBridge::HealApproachStands(a_f)) return "a held heal's approach";
            if (APMFBridge::DisengageStands(a_f)) return "a disengage run";
            return nullptr;
        }
        unsigned Asking(RE::FormID a_f) {
            return (APMFBridge::OffenseApproachStands(a_f) ? 1u : 0u) | (APMFBridge::RangeCloseStands(a_f) ? 2u : 0u) |
                   (APMFBridge::DevGroundStepStands(a_f) ? 4u : 0u);
        }
        std::string AskText(unsigned m) {
            std::string o;
            if (m & 1u) o += "offense-approach ";
            if (m & 2u) o += "range-close ";
            if (m & 4u) o += "give-ground ";
            return o.empty() ? "none" : o.substr(0, o.size() - 1);
        }

        using Role = HRun::Role;
        // The RIGHT hand decides (the left takes heals and buffs in every class). A buff or utility spell there is no role at all.
        Role RoleOf(RE::Actor* a) {
            auto* o = a->GetEquippedObject(false);
            if (!o) return Role::Melee;   // fists
            if (auto* w = o->As<RE::TESObjectWEAP>()) {
                if (w->IsStaff()) return Role::None;
                return w->IsBow() || w->IsCrossbow() ? Role::Bow : Role::Melee;
            }
            if (auto* s = o->As<RE::SpellItem>())
                for (const auto* e : s->effects)
                    if (e && e->baseEffect && e->baseEffect->IsHostile()) return Role::Spell;
            return Role::None;
        }
        const char* RoleName(Role r) { return r == Role::Melee ? "melee" : r == Role::Bow ? "bow" : r == Role::Spell ? "spell" : "none"; }
        // ANY follower takes any test (marth 2026-11-24: "requiring specific followers violates the spirit of the testing gambit"): the role the TEST
        // wants is given to him (lent weapon + temp class). What he holds is kept only where it already fits the test.
        Role AssignRole(Who a_who, Role a_now) {
            if (a_who == Who::Fighter) return Role::Melee;
            if (a_who == Who::Shooter) return a_now == Role::Spell ? Role::Spell : Role::Bow;
            return a_now == Role::None ? Role::Melee : a_now;
        }
        // The plain vanilla weapon of the role (Skyrim.esm: IronSword / HuntingBow + IronArrow), so the temp class has one to wield.
        constexpr RE::FormID kIronSword = 0x00012EB7, kHuntingBow = 0x00013985, kIronArrow = 0x0001397D;
        std::string HandsText(RE::Actor* a) {
            auto* r = a->GetEquippedObject(false);
            auto* l = a->GetEquippedObject(true);
            return std::format("R '{}' L '{}'", r && r->GetName() ? r->GetName() : "-", l && l->GetName() ? l->GetName() : "-");
        }

        // WORKER, every unpaused tick.
        void Sample(RE::Actor* a, RE::FormID a_id, HRun& r, float dt) {
            auto& s = r.st;
            s.t += dt;
            const auto p = a->GetPosition();
            const float d = p.GetDistance(r.pt);
            const auto& leg = Tests()[r.test].legs[r.leg];
            const bool in = d <= leg.radius;
            auto* foe = r.foe ? RE::TESForm::LookupByID<RE::Actor>(r.foe) : nullptr;
            if (foe && foe->IsDead()) foe = nullptr;
            const float face = a->GetAngleZ();
            if (s.have && dt > 0.0f) {
                const float mx = p.x - s.prev.x, my = p.y - s.prev.y;
                const float walked = std::hypot(mx, my), v = walked / dt;
                if (s.arriveAt < 0) { s.walkedTo += walked; }
                else {
                    s.afterT += dt;
                    if (in) s.insideT += dt;
                    if (d <= leg.radius + kT.margin) s.insideMT += dt;
                    s.walkedIn += walked;
                    s.gait[v < 25.0f ? 0 : v < 160.0f ? 1 : 2] += dt;
                }
                s.sprintV = std::max(s.sprintV, v < 1500.0f ? v : 0.0f);
                if (v >= 25.0f) {
                    const float c = std::cos(std::atan2(mx, my) - face);
                    s.move[c > 0.7f ? 0 : c > -0.7f ? 1 : 2] += dt;
                }
                if (s.prevIn && !in) ++s.escapes;
            } else {
                s.startD = d;
            }
            if ((in || s.engineIn) && s.arriveAt < 0) s.arriveAt = s.t;   // arrived by distance or by the engine's own area answer (last tick's)
            if (s.arriveAt >= 0) { s.maxD = std::max(s.maxD, d); s.sumD += d; ++s.samples; }
            s.lastD = d;
            if (foe) {
                const auto  fp = foe->GetPosition();
                const float c  = std::cos(std::atan2(fp.x - p.x, fp.y - p.y) - face);
                s.face[c > 0.7f ? 0 : c > -0.7f ? 1 : 2] += dt;
                Sightline::Want(a_id, { r.foe }, Sightline::Basis::Own);
                const auto sv = Sightline::Check(a_id, r.foe, Sightline::Basis::Own);
                if (sv != Sightline::Verdict::Unknown) { s.sightN += dt; if (sv == Sightline::Verdict::Visible) s.sightT += dt; }
            }
            // attacks / shots: attack state edges. casts: a hand caster entering Casting.
            if (auto* as = a->AsActorState()) {
                const int at = static_cast<int>(as->GetAttackState());
                if (s.have && s.prevAtk == 0 && at != 0) ++s.attacks;
                const auto released = [](int x) { return (x >= 11 && x <= 13) || x == 16 || x == 17; };
                if (s.have && !released(s.prevAtk) && released(at)) ++s.shots;
                s.prevAtk = at;
            }
            int hi = 0;
            for (const auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
                auto*     mc = a->GetMagicCaster(src);
                const int cs = mc ? static_cast<int>(mc->state.get()) : 0;
                const int kc = static_cast<int>(RE::MagicCaster::State::kCasting);
                if (s.have && s.prevCast[hi] != kc && cs == kc) ++s.casts;
                s.prevCast[hi++] = cs;
            }
            s.prev   = p;
            s.prevIn = in;
            s.have   = true;

            const auto info = APMFBridge::DevHoldRead(a_id);
            s.everOurs  = s.everOurs || info.ours;
            s.applied   = std::max(s.applied, info.applied);
            s.styleLive = s.styleLive || info.styleLive;
            s.state     = info.state;
            s.asked |= Asking(a_id);
            if (std::string_view(s.state) == "Leashed") s.leashT += dt;
            if (r.foe && RoleOf(a) != r.role) s.roleLostT += dt;
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) s.playerD = std::max(s.playerD, pc->GetPosition().GetDistance(p));
            s.engineIn = info.engineInside == 2;   // 0 not seen, 1 outside, 2 inside (its own bound radius is added to R)
            if (info.engineInside != 0) { s.engineT += dt; if (s.engineIn) s.engineInT += dt; }
            const auto end = [&](std::string why) { r.ending = true; r.endWhy = std::move(why); };
            if (const char* w = Outranked(a_id)) { APMFBridge::DevHoldRelease(a_id); r.claimKept = false; return end(std::format("YIELDED to {}", w)); }
            if (info.ended) return end(std::format("Harbinger ended it: {}", info.state));
            if (!info.ours && s.t > 1.0f && !info.waiting) return end("another ch.24 claim owns him");
            if (r.foe && s.t > kT.neverApplied && s.applied == 0) return end(std::format("the engine never carried the bound (state {})", info.state));
            if (leg.refile && !r.refiled && s.t >= leg.sec * 0.5f) {
                r.refiled = true;
                APMFBridge::DevHoldRepoint(a_id, r.pt, leg.radius);
                spdlog::info("[hold] {:08X} {}: re-filed (Repoint, same point) at {:.1f}s, {:.0f} from the point", a_id, Tests()[r.test].id, s.t, d);
            }
            if (!Tests()[r.test].foe && APMFBridge::InCombat(a)) return end("INVALID: a fight began during the out-of-combat leg");
            if (s.leashT > kT.leashedMax) return end("INVALID: the pursuit leash excluded the point (the player moved off)");
            if (s.roleLostT > kT.roleLost) return end(std::format("INVALID: the {} weapon left his right hand ({})", RoleName(r.role), HandsText(a)));
            if (s.t >= leg.sec) return end("leg time up");
        }

        // Close the running leg: verdict + the RESULT line, or run the same leg again when the fight stopped under it.
        void CloseLeg(RE::FormID a_id, HRun& r) {
            const auto& t   = Tests()[r.test];
            const auto& leg = t.legs[r.leg];
            auto&       s   = r.st;
            const bool invalid = r.endWhy.starts_with("INVALID");
            const bool combatEnded = r.endWhy.find("CombatEnded") != std::string::npos;
            if (!leg.keep || r.endWhy != "leg time up") { APMFBridge::DevHoldRelease(a_id); r.claimKept = false; }
            else r.claimKept = true;
            if (combatEnded && s.t < leg.sec * 0.5f && r.redo < 2) {   // the fight stopped under it: the same leg again (bounded)
                ++r.redo;
                spdlog::info("[hold] {:08X} {} leg {}: combat ended after {:.1f}s, run again ({}/2)", a_id, t.id, r.leg + 1, s.t, r.redo);
                r.phase = Phase::Start;
                return;
            }
            if (invalid && t.foe && r.redo < 1) {   // a leg that measured the wrong thing: once more
                ++r.redo;
                spdlog::info("[hold] {:08X} {} leg {}: {}, run again ({}/1)", a_id, t.id, r.leg + 1, r.endWhy, r.redo);
                r.phase = Phase::Start;
                return;
            }
            const float inside  = s.afterT > 0.2f ? s.insideT / s.afterT : 0.0f;
            const float insideM = s.afterT > 0.2f ? s.insideMT / s.afterT : 0.0f;   // inside R + margin: extra data only, the verdict reads `inside`
            const float avgD   = s.samples ? s.sumD / static_cast<float>(s.samples) : -1.0f;
            const float drift  = s.afterT > 0.2f ? s.walkedIn / s.afterT : 0.0f;
            const char* v      = "MEASURED";
            if (r.endWhy.starts_with("INVALID")) {
                v = "SKIP";
                ++r.skipped;
            } else if (t.foe == 0) {
                v = s.applied == 0 && std::string_view(s.state) == "Waiting" ? "MEASURED(ch.24 waits out of combat, moves nothing)" : "FAIL(expected Waiting)";
            } else if (r.endWhy.starts_with("YIELDED") || combatEnded || r.endWhy.starts_with("another ch.24")) {
                v = "SKIP";
                ++r.skipped;
            } else if (!s.everOurs || s.applied == 0) {
                v = "FAIL(not carried)";
                ++r.refusedLegs;
            } else if (s.arriveAt < 0) {
                v = "FAIL(never arrived)";
            } else {
                v = inside >= kT.passInside ? "PASS" : "FAIL(left the radius)";
            }
            if (v[0] != 'S') ++r.measured;
            if (leg.quiet || std::string_view(leg.what) == "normal") {
                const int q = leg.quiet ? 1 : 0;
                r.qWalk[q] += s.walkedIn; r.qSec[q] += s.t; r.qAtk[q] += static_cast<float>(s.attacks); r.qCast[q] += static_cast<float>(s.casts);
                r.qIn[q] += s.insideT; r.qInM[q] += s.insideMT; r.qAfter[q] += s.afterT;
            }
            const auto foeDist = [&] {
                auto* f = r.foe ? RE::TESForm::LookupByID<RE::Actor>(r.foe) : nullptr;
                return f ? f->GetPosition().GetDistance(s.prev) : -1.0f;
            }();
            spdlog::info("[hold] RESULT {} leg {}/{} '{}' R={:.0f}: {} | {:08X} {} | start {:.0f}u, arrived {:.1f}s after walking {:.0f}u, inside R {:.0f}% / inside R+{:.0f}u {:.0f}% of {:.1f}s, "
                         "escapes {}, dist avg {:.0f} max {:.0f} last {:.0f}, drift {:.0f} u/s | attacks {} shots {} casts {} | facing foe front/side/back "
                         "{:.1f}/{:.1f}/{:.1f}s, moving fwd/strafe/back {:.1f}/{:.1f}/{:.1f}s, gait still/walk/run {:.1f}/{:.1f}/{:.1f}s, top {:.0f} u/s | "
                         "sees foe {:.0f}%, foe at {:.0f}u | state {} applied {} calm-style {} | MFO asked: {} | end: {} | role {}, engine inside {:.0f}% of {:.1f}s, leashed {:.1f}s, role lost {:.1f}s, player at most {:.0f}u",
                         t.id, r.leg + 1, t.legs.size(), leg.what, leg.radius, v, a_id, r.hands, s.startD, s.arriveAt, s.walkedTo, inside * 100.0f, kT.margin, insideM * 100.0f, s.afterT,
                         s.escapes, avgD, s.maxD, s.lastD, drift, s.attacks, s.shots, s.casts, s.face[0], s.face[1], s.face[2], s.move[0], s.move[1],
                         s.move[2], s.gait[0], s.gait[1], s.gait[2], s.sprintV, s.sightN > 0.2f ? 100.0f * s.sightT / s.sightN : -1.0f, foeDist, s.state,
                         s.applied, leg.quiet ? (s.styleLive ? "LIVE" : "NOT LIVE") : "no", AskText(s.asked), r.endWhy,
                         t.foe ? RoleName(r.role) : "-", s.engineT > 0.2f ? 100.0f * s.engineInT / s.engineT : -1.0f, s.engineT, s.leashT, s.roleLostT, s.playerD);
            Toast(std::format("MFO {} {}/{} {} R{:.0f}: {} ({:.0f}% inside R, {:.0f}% inside R+{:.0f}, {} atk, {} cast)", t.id, r.leg + 1, t.legs.size(), leg.what, leg.radius,
                              std::string_view(v).substr(0, 4) == "MEAS" ? "measured" : v, inside * 100.0f, insideM * 100.0f, kT.margin, s.attacks, s.casts));
            r.summary += std::format("{}{} R{:.0f} {:.4} in {:.0f}%/{:.0f}%(+{:.0f}u) arr {:.1f}s drift {:.0f} atk {} shot {} cast {}", r.summary.empty() ? "" : "; ", leg.what,
                                     leg.radius, std::string_view(v), inside * 100.0f, insideM * 100.0f, kT.margin, s.arriveAt, drift, s.attacks, s.shots, s.casts);
            ++r.leg;
            r.redo  = 0;
            r.restarts = 0;
            r.phase = Phase::Start;
        }

        void SkipLeg(RE::FormID a_id, HRun& r, const std::string& a_why) {
            const auto& t   = Tests()[r.test];
            const auto& leg = t.legs[r.leg];
            APMFBridge::DevHoldRelease(a_id);
            r.claimKept = false;
            spdlog::info("[hold] RESULT {} leg {}/{} '{}' R={:.0f}: SKIP | {:08X} | {}", t.id, r.leg + 1, t.legs.size(), leg.what, leg.radius, a_id, a_why);
            r.summary += std::format("{}{} R{:.0f} SKIP ({})", r.summary.empty() ? "" : "; ", leg.what, leg.radius, a_why.substr(0, 60));
            ++r.skipped;
            ++r.leg;
            r.redo = r.restarts = 0;
            r.phase = Phase::Start;
        }

        Step Verdict_(RE::FormID a_id, HRun& r) {
            const auto& t = Tests()[r.test];
            if (r.qSec[0] > 1.0f && r.qSec[1] > 1.0f && r.qAfter[0] > 0.5f && r.qAfter[1] > 0.5f) {
                // LABELLED BY THE FOLLOWER'S ROLE (the table's name is the FOE's kind: field 11-24c, "quiet_melee" ran only bow legs)
                const auto line = std::format("QUIET COMPARE ({} follower vs a {} foe) normal vs CALM: drift {:.0f} vs {:.0f} u/s, inside R {:.0f}% vs {:.0f}%, inside R+{:.0f}u {:.0f}% vs {:.0f}%, "
                                              "attacks/min {:.1f} vs {:.1f}, casts/min {:.1f} vs {:.1f}",
                                              RoleName(r.role), t.foe == kMage ? "mage" : "melee",
                                              r.qWalk[0] / r.qAfter[0], r.qWalk[1] / r.qAfter[1], 100 * r.qIn[0] / r.qAfter[0], 100 * r.qIn[1] / r.qAfter[1],
                                              kT.margin, 100 * r.qInM[0] / r.qAfter[0], 100 * r.qInM[1] / r.qAfter[1],
                                              60 * r.qAtk[0] / r.qSec[0], 60 * r.qAtk[1] / r.qSec[1], 60 * r.qCast[0] / r.qSec[0], 60 * r.qCast[1] / r.qSec[1]);
                spdlog::info("[hold] RESULT {} {:08X}: {}", t.id, a_id, line);
                r.summary = line + " | " + r.summary;
            }
            if (r.measured) {
                std::size_t left = 0;
                {
                    std::lock_guard lk(g_mx);
                    for (const auto& x : Tests())
                        if (&x != &t && !g_results.contains(x.id)) ++left;
                }
                Toast(std::format("MFO {} finished, {} hold test(s) left", t.id, left));
            }
            if (r.summary.size() > 900) r.summary.resize(900);   // 'MFST' evidence is bounded
            if (r.measured == 0) return Step::Retry("nothing measured: " + r.summary);
            if (r.refusedLegs > 0 && r.refusedLegs == r.measured) return Step::Fail("the hold was never carried: " + r.summary);
            return Step::Info(r.hands + " | " + r.summary);
        }

        Step StepHold(Ctx& c, int a_test) {
            const auto& t = Tests()[a_test];
            auto&       r = g_hruns[c.id];
            if (r.test != a_test || !c.s->b[0]) {
                r         = HRun{};
                r.test    = a_test;
                c.s->b[0] = true;
                r.hands   = HandsText(c.follower);
                if (t.foe) {
                    r.role = static_cast<Role>(static_cast<int>(c.s->n[0]));   // FixHold, before anything was given
                    std::string why;
                    const char* slug = r.role == Role::Bow ? "archer" : r.role == Role::Spell ? "evoker" : "warrior";
                    if (c.s->b[1] && !ApplyClass(c.id, slug, 5, false, why)) spdlog::warn("[hold] {:08X} {}: temp class {} refused: {}", c.id, t.id, slug, why);
                }
            }
            c.s->note = std::format("leg {} of {}; {}", r.leg + 1, t.legs.size(), r.summary.substr(0, 300));
            r.phaseT += 0.5f;
            r.foe = c.hostile ? c.hostile->GetFormID() : 0;
            if (r.phase == Phase::Active) {
                if (t.foe && !r.foe && !r.ending) { r.ending = true; r.endWhy = "the foe died (CombatEnded)"; }
                if (r.ending) CloseLeg(c.id, r);
                if (r.phase == Phase::Active) return Step::Continue();
            }
            if (r.leg >= static_cast<int>(t.legs.size())) { r.phase = Phase::Done; return Verdict_(c.id, r); }
            const auto& leg = t.legs[r.leg];
            // the fight must be real (Harbinger's Waiting is an out-of-combat answer), and a foe standing
            if (t.foe) {
                if (!c.hostile || !APMFBridge::InCombat(c.follower)) {
                    r.waitT += 0.5f;
                    if (r.waitT > kT.fightWait) {
                        APMFBridge::DevHoldRelease(c.id);
                        if (r.measured) return Verdict_(c.id, r);
                        return Step::Retry("no fight began (is the foe blocked, or did others kill it)");
                    }
                    return Step::Continue();
                }
                r.waitT = 0.0f;
            } else if (c.inCombat) {
                APMFBridge::DevHoldRelease(c.id);
                return Step::Retry("a fight began during the out-of-combat test");
            }
            if (r.phase == Phase::Start) {
                r.phaseT = 0.0f;
                r.replanT = 0.0f;
                r.postedT = 0.0f;
                r.hands  = HandsText(c.follower);
                const bool point = leg.where != Where::Same || !r.havePt;
                r.ticket = PostPlan(c.id, r.foe, leg.where == Where::Same ? Where::Near : leg.where, r.leg, leg.radius, leg.quiet, point);
                r.phase  = Phase::Planning;
                return Step::Continue();
            }
            if (r.phase == Phase::Planning) {
                if (auto p = TakePlan(r.ticket)) {
                    if (p->ok) {
                        if (leg.where != Where::Same || !r.havePt) { r.pt = p->pt; r.havePt = true; }
                        r.style = p->style;
                        if (leg.quiet) spdlog::info("[hold] {:08X} {}: {}", c.id, t.id, p->styleText);
                        if (leg.quiet && !r.style) { SkipLeg(c.id, r, "no calm combat style in this load order"); return Step::Continue(); }
                        spdlog::info("[hold] {:08X} '{}' {} leg {}/{} '{}': point {:.0f},{:.0f},{:.0f} R {:.0f} ({})", c.id,
                                     c.follower->GetName() ? c.follower->GetName() : "?", t.id, r.leg + 1, t.legs.size(), leg.what, r.pt.x, r.pt.y, r.pt.z,
                                     leg.radius, p->why.empty() ? "the same point" : p->why);
                        r.phase  = Phase::Filing;
                        r.phaseT = 0.0f;
                    } else if (r.phaseT < kT.replanFor) {
                        spdlog::info("[hold] {:08X} {} leg {}: no point yet: {}", c.id, t.id, r.leg + 1, p->why);
                        r.ticket  = 0;
                        r.replanT = 0.0f;
                        r.endWhy  = p->why;
                    } else {
                        SkipLeg(c.id, r, p->why);
                        return Step::Continue();
                    }
                } else if (r.ticket == 0) {
                    r.replanT += 0.5f;
                    if (r.replanT >= kT.replanEvery) {
                        r.ticket  = PostPlan(c.id, r.foe, leg.where == Where::Same ? Where::Near : leg.where, r.leg, leg.radius, leg.quiet, true);
                        r.postedT = 0.0f;
                    }
                    return Step::Continue();
                } else {
                    r.postedT += 0.5f;
                    if (r.postedT > kT.planWait) SkipLeg(c.id, r, "the main-thread planner did not answer");
                    return Step::Continue();
                }
            }
            if (r.phase == Phase::Filing) {
                if (t.foe && RoleOf(c.follower) != r.role) {
                    if (r.phaseT > kT.roleWait) SkipLeg(c.id, r, std::format("the {} weapon never came to his right hand ({})", RoleName(r.role), HandsText(c.follower)));
                    return Step::Continue();
                }
                r.hands = HandsText(c.follower);
                if (leg.where == Where::Near && !r.claimKept && c.follower->GetPosition().GetDistance(r.pt) < leg.radius + 20.0f && r.restarts < 3) {
                    ++r.restarts;   // the fight carried him into it before the claim: another point
                    r.phase = Phase::Start;
                    return Step::Continue();
                }
                const char* busy = Outranked(c.id);
                const unsigned ask = r.claimKept ? 0u : Asking(c.id);   // ours already stands on a kept claim
                if (busy || ask) {
                    if (r.phaseT > kT.roleWait + kT.busyWait)
                        SkipLeg(c.id, r, std::format("another MFO ch.24 claim stood: {}", busy ? busy : AskText(ask)));
                    return Step::Continue();
                }
                const char* no = nullptr;
                if (leg.quiet) no = APMFBridge::DevQuietFile(c.id, r.style);
                if (!no) {
                    if (r.claimKept) no = APMFBridge::DevHoldRepoint(c.id, r.pt, leg.radius) ? nullptr : "the kept claim is gone";
                    else no = APMFBridge::DevHoldFile(c.id, r.pt, leg.radius);
                }
                if (no) {
                    spdlog::warn("[hold] {:08X} {} leg {}: NOT FILED: {}", c.id, t.id, r.leg + 1, no);
                    ++r.refusedLegs;
                    ++r.measured;
                    SkipLeg(c.id, r, std::string("not filed: ") + no);
                    return Step::Continue();
                }
                r.st      = Stats{};
                r.ending  = false;
                r.endWhy.clear();
                r.refiled = false;
                r.phase   = Phase::Active;
            }
            return Step::Continue();
        }

        const char* PreHold(Ctx& c, int a_test) {
            const auto& t = Tests()[a_test];
            if (APMFBridge::DevAbi() < 39) return "Harbinger ABI 39 or newer (ch.24 point claims)";
            if (c.seenFor < 5.0f) return "5 s in the party";
            if (!c.follower->Is3DLoaded()) return "the follower loaded";
            if (c.inCombat) return "out of combat";
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); !pc || pc->IsInCombat()) return "the player out of combat";
            if (c.follower->IsOnMount()) return "the follower on foot";
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc->GetPosition().GetDistance(c.follower->GetPosition()) > kT.playerReach * 0.6f) return "the player near the follower";
            return nullptr;
        }
        void FixHold(Ctx& c, FixtureRequest& q, int a_test) {
            const auto& t = Tests()[a_test];
            if (!t.foe) return;
            const Role role = AssignRole(t.who, RoleOf(c.follower));
            c.s->n[0] = static_cast<double>(static_cast<int>(role));
            c.s->b[1] = Classes::Writable();   // the restore point is taken only now: no point, no temp class
            if (role == Role::Melee) q.items.push_back({ VanillaForm(kIronSword), 1 });
            if (role == Role::Bow) {
                q.items.push_back({ VanillaForm(kHuntingBow), 1 });
                q.items.push_back({ VanillaForm(kIronArrow), 60 });
            }
            q.cls.snapshot    = c.s->b[1];
            q.spawn.clearSpot = true;
            q.spawn.on        = true;
            q.spawn.baseLocal = t.foe;
            q.spawn.health    = kT.foeHealth;
            q.spawn.respawns  = 2;
            q.spawn.distance  = t.foeAt;
        }
        Step TimeoutHold(Ctx& c, int a_test) {
            const auto it = g_hruns.find(c.id);
            if (it == g_hruns.end() || it->second.test != a_test) return Step::Retry("timeout before it began");
            return it->second.measured ? Verdict_(c.id, it->second) : Step::Retry("timeout with nothing measured: " + c.s->note);
        }

        constexpr std::size_t kCount = 12;   // = Tests().size() (checked in RegisterHold)
        template <int I> const char* Pre(Ctx& c) { return PreHold(c, I); }
        template <int I> void Fix(Ctx& c, FixtureRequest& q) { FixHold(c, q, I); }
        template <int I> Step Stp(Ctx& c) { return StepHold(c, I); }
        template <int I> Step Tmo(Ctx& c) { return TimeoutHold(c, I); }
        constexpr int kFirstTests = 2;   // cover, doorway: RegisterHold(true)
        template <int... I> void RegisterEach(bool a_first, std::integer_sequence<int, I...>) {
            const auto one = [a_first](int i, auto pre, auto fix, auto stp, auto tmo) {
                if ((i < kFirstTests) != a_first) return;
                const auto& t   = Tests()[i];
                float       sec = 45.0f;   // the spawn, the first approach
                for (const auto& l : t.legs) sec += 1.5f * l.sec + kT.busyWait + kT.roleWait;
                Register({ t.id, "movement-plan G0b", t.needs, sec, pre, fix, stp, tmo, true });
            };
            (one(I, &Pre<I>, &Fix<I>, &Stp<I>, &Tmo<I>), ...);
        }
    }

    void RegisterHold(bool a_first) {
        if (Tests().size() != kCount) {
            spdlog::error("[hold] {} hold tests in the table, {} expected -- none registered", Tests().size(), kCount);
            return;
        }
        RegisterEach(a_first, std::make_integer_sequence<int, static_cast<int>(kCount)>{});
    }

    void HoldTick(float a_dt) {
        for (auto& [id, r] : g_hruns) {
            if (r.phase != Phase::Active || r.ending) continue;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!a || a->IsDead() || !a->Is3DLoaded()) {
                APMFBridge::DevHoldRelease(id);
                r.claimKept = false;
                r.ending    = true;
                r.endWhy    = "YIELDED: the follower died or unloaded";
                continue;
            }
            Sample(a, id, r, a_dt);
        }
    }

    void HoldEnd(RE::FormID a_follower) {
        APMFBridge::DevHoldRelease(a_follower);
        g_hruns.erase(a_follower);
    }

    void ClearHoldState() {
        APMFBridge::DevHoldReleaseAll();
        g_hruns.clear();
        std::lock_guard lk(g_planMx);
        g_plans.clear();
    }
}
