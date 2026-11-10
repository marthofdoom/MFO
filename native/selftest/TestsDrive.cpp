// THE SELF-DRIVING SELF-TESTS (dev builds only; Docs/SELFTEST.md "Self-driving"). marth 2026-11-10: "I can boot into lorerim and use the
// debug gambit on someone. It needs to be able to test everything on its own."
//
// Each test here asks the runner (through fixtures()) for everything its situation needs: TEMP ROWS in the follower's table, a CLASS
// SNAPSHOT (the test then applies a class through the C5c writers), a SPAWNED weak hostile, memory-only PROBE flags, fixture items /
// spells. The runner applies them, the test judges from OBSERVED state or the dev-only internal counters the subsystems expose
// (CostParity::DevEvents, Actuation::DevBuffClaimCount, Classes::DevReadCarriers, APMFBridge::DevReadAim, ...), and the runner puts every
// change back. A verdict is never derived from a timer alone: a window that ends without the observation is RETRY (or INFO with
// the numbers), not a pass.
//
// Reads only inside step() except the class writers a test calls on purpose (WORKER, mutex-guarded, snapshotted + restored by the
// runner) and the posted main-thread reads (MainThread::Post) whose results are cached under a leaf lock.

#include "selftest/SelfTest_internal.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "Vocabulary.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
#include "cast/CostParity.h"
#include "classes/Classes.h"
#include "progression/ProgAllocator.h"

#include <cmath>

namespace MFO::SelfTest {

    namespace {

        // Skyrim.esm local ids, verified against the master's own records with tools/esp_inspect.py (2026-11-10).
        constexpr RE::FormID kHealing     = 0x00012FCC;   // SPEL Healing (self, fire-and-forget, Restoration)
        constexpr RE::FormID kFlames      = 0x00012FCD;   // SPEL Flames (concentration, Destruction)
        constexpr RE::FormID kFastHealing = 0x0002F3B8;   // SPEL FastHealing (concentration self heal, Restoration)
        constexpr RE::FormID kOakflesh    = 0x0005AD5C;   // SPEL Oakflesh (self Armor)
        constexpr RE::FormID kWardLesser  = 0x00013018;   // SPEL WardLesser (self Ward)
        constexpr RE::FormID kCourage     = 0x0004DEE8;   // SPEL Courage (aimed influence)
        constexpr RE::FormID kHuntingBow  = 0x00013985;   // WEAP HuntingBow
        constexpr RE::FormID kIronArrow   = 0x0001397D;   // AMMO IronArrow

        double NowMs() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        double Mag(RE::Actor* a) {
            auto* avo = a ? a->AsActorValueOwner() : nullptr;
            return avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0;
        }
        RE::FormID HandForm(RE::Actor* a, bool a_left) {
            auto* f = a ? a->GetEquippedObject(a_left) : nullptr;
            return f ? f->GetFormID() : 0;
        }
        TempRow Row(const char* a_cond, const char* a_act, RE::FormID a_form = 0, float a_param = 0.0f, std::uint8_t a_subject = 0) {
            TempRow r;
            r.cond = a_cond; r.act = a_act; r.form = a_form; r.condParam = a_param; r.subject = a_subject;
            return r;
        }
        // Is a_spell channelling in either hand of a_actor? (the same read CostParity::Channelling does)
        bool Channelling(RE::Actor* a_actor, RE::FormID a_spell) {
            for (const auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
                auto* mc = a_actor ? a_actor->GetMagicCaster(src) : nullptr;
                if (!mc || !mc->currentSpell || mc->currentSpell->GetFormID() != a_spell) continue;
                if (mc->state.get() == RE::MagicCaster::State::kCasting) return true;
            }
            return false;
        }
        const char* ClassPre() {
            if (!Classes::Active()) return "the class add-on live (MFO_Classes.esl + bClassTree = 1)";
            if (!Classes::Writable()) return "MCLS writable (the loaded record was newer than this DLL: read-only)";
            return nullptr;
        }
        bool V(RE::FormID a_local, RE::FormID& a_out) { a_out = VanillaForm(a_local); return a_out != 0; }
        // The standing fight rows: any enemy -> attack (so the follower fights the spawned hostile while the test's own rows run above).
        void AddFightRows(FixtureRequest& r) { r.rows.push_back(Row(Vocab::kCondFoeAny, Vocab::kActAttack)); }

        // ── cached main-thread reads (the carriers) ───────────────────────────────────────────────────
        struct CarrierRead { Classes::DevCarrierView v; bool found = false; bool inFlight = false; std::chrono::steady_clock::time_point at{}; };
        std::mutex                                g_cvMx;   // leaf lock
        std::unordered_map<RE::FormID, CarrierRead> g_cv;
        void RequestCarrier(RE::FormID a_id) {
            {
                std::lock_guard lk(g_cvMx);
                auto& r = g_cv[a_id];
                if (r.inFlight) return;
                r.inFlight = true;
            }
            MainThread::Post([a_id]() {
                const auto v = Classes::DevReadCarriers(a_id);
                std::lock_guard lk(g_cvMx);
                g_cv[a_id] = { v, true, false, std::chrono::steady_clock::now() };
            });
        }
        bool FreshCarrier(RE::FormID a_id, std::chrono::steady_clock::time_point a_notBefore, Classes::DevCarrierView& a_out) {
            std::lock_guard lk(g_cvMx);
            auto it = g_cv.find(a_id);
            if (it == g_cv.end() || !it->second.found || it->second.at < a_notBefore) return false;
            a_out = it->second.v;
            return true;
        }

        // ── costparity.* ───────────────────────────────────────────────────────────────────────────────
        // marth 2026-11-09 "if we know the real cost just charge that": the follower must pay the PLAYER-curve cost P, not the engine's E, on
        // whichever road cast it. The follower is hurt and a temp row (self HP < 95% -> cast the heal on himself, highest priority) makes him
        // cast while the spawned hostile keeps him in combat. Judged from the CostParity billing events (the road, P, E of every bill) AND the
        // measured magicka drop: Fire-and-forget: drop == sum of P over the casts; concentration: drain rate == P per second.
        const char* CostPre(Ctx& c, RE::FormID a_local, double a_minPool) {
            if (!CostParity::Active()) return "bFollowerSpellCostParity on and the six cost GMSTs read ([costparity] ACTIVE at startup)";
            RE::FormID sp = 0;
            if (!V(a_local, sp)) return "the Skyrim.esm heal spell resolvable";
            if (c.inCombat) return "out of combat (the runner starts its own fight)";
            if (Mag(c.follower) < a_minPool) return "the follower's magicka above the spell's cost (wait for it to regenerate)";
            c.s->set[0] = { sp };
            return nullptr;
        }
        const char* CostFFPre(Ctx& c) { return CostPre(c, kHealing, 80.0); }
        const char* CostConcPre(Ctx& c) { return CostPre(c, kFastHealing, 100.0); }
        void CostFix(Ctx& c, FixtureRequest& r) {
            r.spells.push_back(c.s->set[0][0]);
            r.rows.push_back(Row(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, c.s->set[0][0], 0.95f));
            AddFightRows(r);
            r.spawn.on = true;
            r.spawn.hurtPct = 45.0f;
        }
        std::string RoadsSeen(unsigned a_mask) {
            std::string o;
            if (a_mask & 1) o += "direct ";
            if (a_mask & 2) o += "animated-once ";
            if (a_mask & 4) o += "animated-concentration-slice ";
            return o.empty() ? "none" : o;
        }
        // n[0] last pre-event pool, n[1] max seq at start, n[2] pool just before the first event, n[3] its time (ms), n[4] sum P, n[5] sum E,
        // n[6] events, n[7] last event ms. b[0] init, b[1] events seen, b[2] some event did not apply parity.
        Step CostFFStep(Ctx& c) {
            auto&            s  = *c.s;
            const RE::FormID sp = s.set[0][0];
            const double     pool = Mag(c.follower);
            const auto       ev = CostParity::DevEvents();
            if (!s.b[0]) {
                s.b[0] = true;
                for (const auto& e : ev) s.n[1] = std::max<double>(s.n[1], e.seq);
                s.n[0] = pool;
                s.n[3] = NowMs();
                return Step::Continue();
            }
            double sumP = 0, sumE = 0, last = 0;
            int    cnt = 0;
            unsigned roads = 0;
            bool   allApply = true, anyApply = false;
            for (const auto& e : ev) {
                if (e.seq <= s.n[1] || e.caster != c.id || e.spell != sp) continue;
                ++cnt;
                last = std::max(last, e.atMs);
                roads |= 1u << static_cast<unsigned>(e.road);
                sumE += e.engine;
                sumP += e.applies ? e.parity : e.engine;
                if (e.applies) anyApply = true; else allApply = false;
            }
            if (cnt == 0) {
                s.n[0] = pool;
                s.n[3] = NowMs();
                s.note = std::format("no billing event for '{}' yet (the rule must cast it: self HP < 95%, in combat); pool {:.0f}", FormName(sp), pool);
                return c.inCombat ? Step::Continue() : Step::Retry("the fight ended before the follower cast the heal; " + s.note);
            }
            if (!s.b[1]) { s.b[1] = true; s.n[2] = s.n[0]; }
            s.n[4] = sumP; s.n[5] = sumE; s.n[6] = cnt; s.n[7] = last;
            s.note = std::format("{} bill(s) via [{}]: engine cost sum {:.2f}, player cost sum {:.2f}; pool {:.1f} -> {:.1f}", cnt, RoadsSeen(roads), sumE, sumP, s.n[2], pool);
            if (NowMs() - last < 1800.0) return Step::Continue();   // the top-up / the second cast may still land
            if (!anyApply)
                return Step::Info("parity does NOT apply to this follower and spell (the engine cost stands: the player curve is not costlier at his skill, "
                                  "or a Set-value perk, or a power); " + s.note);
            const double drop = s.n[2] - pool;
            const double regen = 0.5 * (NowMs() - s.n[3]) / 1000.0;
            const double tol = 2.0 + 0.1 * sumP + regen;
            if (s.n[2] < sumP + 2.0) return Step::Retry("the pool was too low to tell P from the clamp at 0; " + s.note);
            if (std::fabs(drop - sumP) <= tol)
                return Step::Pass(std::format("magicka fell {:.1f}, expected the player-curve sum {:.1f} (tol {:.1f}); {}", drop, sumP, tol, s.note));
            if (std::fabs(drop - sumE) <= tol && std::fabs(sumP - sumE) > 2 * tol && allApply)
                return Step::Fail(std::format("magicka fell {:.1f} = the ENGINE cost sum {:.1f}, not the player-curve sum {:.1f}: the follower was not charged P; {}",
                                              drop, sumE, sumP, s.note));
            return Step::Retry(std::format("magicka fell {:.1f}, matching neither P {:.1f} nor E {:.1f} (other magicka use, absorb, or a cast the events did not see); {}",
                                           drop, sumP, sumE, s.note));
        }
        Step CostTimeout(Ctx& c) { return Step::Retry("no billing event in the window (the heal rule never cast); " + c.s->note); }

        std::vector<std::pair<double, double>> g_cs;   // concentration samples (ms, pool); WORKER-only, one run at a time per test
        // b[0] init, b[1] first event seen, n[1] max seq, n[2] index of the sample before the first event, n[4] last channel-open ms,
        // n[5] pool then.
        Step CostConcStep(Ctx& c) {
            auto&            s  = *c.s;
            const RE::FormID sp = s.set[0][0];
            const double     pool = Mag(c.follower);
            const auto       ev = CostParity::DevEvents();
            if (!s.b[0]) {
                s.b[0] = true;
                g_cs.clear();
                for (const auto& e : ev) s.n[1] = std::max<double>(s.n[1], e.seq);
            }
            g_cs.emplace_back(NowMs(), pool);
            float parity = 0, engine = 0;
            int   cnt = 0;
            bool  applies = false;
            unsigned roads = 0;
            for (const auto& e : ev) {
                if (e.seq <= s.n[1] || e.caster != c.id || e.spell != sp) continue;
                ++cnt;
                roads |= 1u << static_cast<unsigned>(e.road);
                engine = e.engine;
                if (e.applies) { applies = true; parity = e.parity; }
            }
            if (cnt > 0 && !s.b[1]) { s.b[1] = true; s.n[2] = g_cs.size() >= 2 ? static_cast<double>(g_cs.size() - 2) : 0.0; }
            const bool chan = Channelling(c.follower, sp);
            if (chan) { s.n[4] = NowMs(); s.n[5] = pool; }
            s.note = std::format("{} bill(s) via [{}], channel {}, pool {:.1f}", cnt, RoadsSeen(roads), chan ? "open" : "closed", pool);
            if (!s.b[1]) return c.inCombat ? Step::Continue() : Step::Retry("the fight ended before the concentration heal was cast; " + s.note);
            if (chan || NowMs() - s.n[4] < 1500.0) return Step::Continue();   // still channelling (or just ended)
            if (!applies)
                return Step::Info("parity does NOT apply to this follower and spell (the engine cost stands); " + s.note);
            const auto [t1, p1] = g_cs[static_cast<std::size_t>(s.n[2])];
            const double dt = (s.n[4] - t1) / 1000.0;
            if (dt < 3.0) return Step::Retry(std::format("the channel lasted only {:.1f}s (needs >= 3 s to read a rate); {}", dt, s.note));
            const double rate = (p1 - s.n[5]) / dt;
            const double tol = std::max(1.0, 0.2 * parity) + 1.0;
            if (std::fabs(rate - parity) <= tol)
                return Step::Pass(std::format("drain {:.2f}/s over {:.1f}s, expected P {:.2f}/s (tol {:.2f}; engine would be {:.2f}/s); {}", rate, dt, parity, tol, engine, s.note));
            if (std::fabs(rate - engine) <= tol && std::fabs(parity - engine) > 2 * tol)
                return Step::Fail(std::format("drain {:.2f}/s over {:.1f}s = the ENGINE rate {:.2f}/s, not P {:.2f}/s; {}", rate, dt, engine, parity, s.note));
            return Step::Retry(std::format("drain {:.2f}/s over {:.1f}s matches neither P {:.2f} nor E {:.2f}; {}", rate, dt, parity, engine, s.note));
        }

        // ── b1a.weapon_stays_right ─────────────────────────────────────────────────────────────────────
        // MFO-B1a (92b6e84): heal LEFT + a one-hander RIGHT + an offense cast rule: the weapon stays, no right claim is minted. The temp rows
        // are the heal (self HP < 95%) and the offense (any enemy -> Flames). Observed: the RIGHT hand over the run. n[0] samples, n[1]
        // heal-left samples, n[2] right-hand SPELL samples, n[3] weapon-right samples; set[0] = start weapon.
        bool IsMeleeWeapon(RE::TESForm* f) {
            auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
            return w && !w->IsBow() && !w->IsCrossbow() && !w->IsStaff() && !w->IsBound();
        }
        const char* B1aPre(Ctx& c) {
            RE::FormID h = 0, f = 0;
            if (!V(kHealing, h) || !V(kFlames, f)) return "the Skyrim.esm Healing / Flames resolvable";
            auto* rf = c.follower->GetEquippedObject(false);
            if (!IsMeleeWeapon(rf)) return nullptr;   // the weapon-less case is judged by b1.heal_left_offense_right; this one reports INFO
            c.s->set[0] = { rf->GetFormID() };
            return nullptr;
        }
        void B1aFix(Ctx&, FixtureRequest& r) {
            RE::FormID h = VanillaForm(kHealing), f = VanillaForm(kFlames);
            r.spells.push_back(h);
            r.spells.push_back(f);
            r.rows.push_back(Row(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, h, 0.95f));
            r.rows.push_back(Row(Vocab::kCondFoeAny, Vocab::kActCastTarget, f));
            AddFightRows(r);
            r.spawn.on = true;
            r.spawn.hurtPct = 40.0f;
        }
        Step B1aStep(Ctx& c) {
            auto& s = *c.s;
            if (s.set[0].empty()) return Step::Info("not applicable: the follower's right hand holds no one-handed weapon (b1.heal_left_offense_right covers the caster loadout)");
            const RE::FormID L = HandForm(c.follower, true), R = HandForm(c.follower, false);
            const RE::FormID h = VanillaForm(kHealing), f = VanillaForm(kFlames);
            s.n[0] += 1;
            if (L == h) s.n[1] += 1;
            if (R == s.set[0][0]) s.n[3] += 1;
            auto* rf = R ? RE::TESForm::LookupByID(R) : nullptr;
            if (rf && rf->As<RE::SpellItem>()) s.n[2] += 1;
            s.note = std::format("samples {:.0f}: heal-left {:.0f}, start weapon '{}' in the right {:.0f}, a SPELL in the right {:.0f} (Flames is '{}'); last L '{}' R '{}'",
                                 s.n[0], s.n[1], FormName(s.set[0][0]), s.n[3], s.n[2], FormName(f), L ? FormName(L) : "empty", R ? FormName(R) : "empty");
            if (s.n[1] >= 2 && s.n[2] > 0)
                return Step::Fail("a right claim displaced the weapon with a spell while the heal held the left hand; " + s.note);
            if (s.n[1] >= 3 && s.n[3] >= 3 && s.n[2] == 0 && c.elapsed >= 20.0f)
                return Step::Pass("heal in the left, the weapon stayed in the right, no spell was ever planted in the right hand; " + s.note);
            if (!c.inCombat) return Step::Retry("the fight ended first; " + s.note);
            return Step::Continue();
        }
        Step B1aTimeout(Ctx& c) {
            return Step::Retry("the window ended without >= 3 heal-left + weapon-right samples (the heal never held the left hand long enough); " + c.s->note);
        }

        // ── buff.animated_claim_road ───────────────────────────────────────────────────────────────────
        // bbf90fd: Oakflesh (Armor) and Lesser Ward (Ward) cast by a follower go through the animated self-buff CLAIM road (cast/BuffRoad.cpp):
        // a ch.8b claim on the LEFT hand, the engine's own caster fires. Two always-true temp rows cast them in turn (the first becomes
        // "buff already up", transparent, and the scan falls to the second). Judged per spell by the road's own counters (claims filed, casts
        // landed) AND the effect being active on him; the archetype must predict a row (not NoRow).
        const char* BuffPre(Ctx&) {
            RE::FormID o = 0, w = 0;
            if (!V(kOakflesh, o) || !V(kWardLesser, w)) return "the Skyrim.esm Oakflesh / Lesser Ward resolvable";
            return nullptr;
        }
        void BuffFix(Ctx&, FixtureRequest& r) {
            const RE::FormID o = VanillaForm(kOakflesh), w = VanillaForm(kWardLesser);
            r.spells.push_back(o);
            r.spells.push_back(w);
            r.rows.push_back(Row(Vocab::kCondAlways, Vocab::kActCastSelf, o));
            r.rows.push_back(Row(Vocab::kCondAlways, Vocab::kActCastSelf, w));
            AddFightRows(r);
            r.spawn.on = true;
        }
        bool EffectUp(RE::Actor* a, RE::SpellItem* sp) {
            auto* mt = a ? a->AsMagicTarget() : nullptr;
            if (!mt || !sp) return false;
            for (auto* e : sp->effects) {
                auto* mgef = e ? e->baseEffect : nullptr;
                if (mgef && mt->HasMagicEffect(mgef)) return true;
            }
            return false;
        }
        // n[0], n[1] claim counts at the start (Oakflesh, Ward); n[2], n[3] landed at the start. b[0] init. b[1], b[2] spell done.
        Step BuffStep(Ctx& c) {
            auto&            s = *c.s;
            const RE::FormID sp[2] = { VanillaForm(kOakflesh), VanillaForm(kWardLesser) };
            if (!s.b[0]) {
                s.b[0] = true;
                for (int i = 0; i < 2; ++i) { s.n[i] = Actuation::DevBuffClaimCount(sp[i]); s.n[2 + i] = Actuation::DevBuffLandedCount(sp[i]); }
                if (!APMFBridge::CastSeatsSupported())
                    return Step::Info("Harbinger is absent or older than ABI 19: the animated claim road is not served and these buffs take the direct road by design "
                                      "(a documented degrade, nothing to verify)");
            }
            std::string detail;
            for (int i = 0; i < 2; ++i) {
                auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(sp[i]);
                const auto claims = Actuation::DevBuffClaimCount(sp[i]) - static_cast<std::uint32_t>(s.n[i]);
                const auto landed = Actuation::DevBuffLandedCount(sp[i]) - static_cast<std::uint32_t>(s.n[2 + i]);
                const bool up = EffectUp(c.follower, spell);
                const auto arch = spell ? Actuation::ClassifyArchetype(spell) : Actuation::SpellArchetype{};
                if (claims > 0 && (up || landed > 0)) s.b[1 + i] = true;
                detail += std::format("{}{} '{}': claim(s) filed {}, landed {}, effect active {}, predicted row {}", i ? "; " : "", i + 1, FormName(sp[i]), claims, landed,
                                      up ? "yes" : "no", Actuation::EngineRowName(arch.row));
            }
            s.note = detail;
            if (s.b[1] && s.b[2]) return Step::Pass("both buffs rode the animated claim road: " + detail + " (the caster the engine built is judged by the effect landing; [ctcensus] is log-only)");
            if (!c.inCombat && c.elapsed > 15.0f) return Step::Retry("the fight ended before both buffs landed; " + detail);
            return Step::Continue();
        }
        Step BuffTimeout(Ctx& c) {
            // A claim that never fires is the failure this test exists to catch, but only when the rule was actually reached.
            return Step::Fail("not both buffs completed the claim road in the window: " + c.s->note);
        }

        // ── class.c6_carriers ───────────────────────────────────────────────────────────────────────────
        // 84678a5: the class passive carriers follow the class state. Five scenarios, applied through the C5c writers (the runner restores the
        // snapshot): the rank read-back (faction MFOC_P_<Name>) must equal CarrierRanks() for every class that HAS a carrier, with the explicit
        // expectations the test list names. n[0] scenario, n[1] elapsed when applied; b[0] applied.
        const char* CarrierPre(Ctx&) { return ClassPre(); }
        void ClassSnapFix(Ctx&, FixtureRequest& r) { r.cls.snapshot = true; }
        struct Scn { const char* name; };
        constexpr Scn kScn[5] = { { "Warrior L7 equipped" }, { "Warrior mastered, Knight L3 equipped" }, { "T4 Living Legend equipped over mastered T3s" },
                                  { "Mercenary L5 equipped" }, { "classes OFF (nothing equipped)" } };
        bool ApplyScn(Ctx& c, int i, std::string& why) {
            const RE::FormID id = c.id;
            using Classes::Id;
            switch (i) {
            case 0: Classes::SetMastered(id, 0); return ApplyClass(id, "warrior", 7, false, why);
            case 1: Classes::SetMastered(id, Classes::Bit(Id::Warrior) | Classes::Bit(Id::Knight)); return ApplyClass(id, "knight", 3, false, why);
            case 2: {
                Classes::Mask m = 0;
                for (int k = static_cast<int>(Id::GreatKnight); k <= static_cast<int>(Id::Scout); ++k) m |= Classes::Bit(static_cast<Id>(k));
                Classes::SetMastered(id, m);
                return ApplyClass(id, "living-legend", 3, false, why);
            }
            case 3: Classes::SetMastered(id, 0); return ApplyClass(id, "mercenary", 5, false, why);
            default: why.clear(); return Classes::ClearEquipped(id);
            }
        }
        Step CarrierStep(Ctx& c) {
            auto&       s = *c.s;
            const int   i = static_cast<int>(s.n[0]);
            if (i >= 5) return Step::Pass("all five scenarios read back as CarrierRanks() predicts; " + s.note);
            if (!s.b[0]) {
                std::string why;
                if (!ApplyScn(c, i, why)) return Step::Retry(std::format("scenario {} '{}' could not be applied: {}", i, kScn[i].name, why));
                Classes::RefreshCarriers(c.id);
                s.b[0] = true;
                s.n[1] = c.elapsed;
                s.n[2] = 0;
                s.n[3] = NowMs();
                RequestCarrier(c.id);
                return Step::Continue();
            }
            Classes::DevCarrierView v;
            const auto notBefore = std::chrono::steady_clock::now() - std::chrono::milliseconds(static_cast<long long>(NowMs() - s.n[3]));
            if (!FreshCarrier(c.id, notBefore, v)) { RequestCarrier(c.id); return Step::Continue(); }
            const auto fc = Classes::GetFollower(c.id);
            if (!fc) return Step::Retry("the follower has no class record");
            if (!v.resolved) return Step::Retry("the carrier forms are not resolved / the follower is not loaded (the ESL carriers did not resolve: see [classes] carriers)");
            const auto want = Classes::CarrierRanks(*fc);
            std::string diff;
            int topCapRanks = 0;
            for (std::size_t k = 0; k < Classes::kClassCount; ++k) {
                if (!v.hasFaction[k]) continue;
                if (v.ranks[k] != want[k])
                    diff += std::format(" {}: read {} want {};", Classes::Get(static_cast<Classes::Id>(k)).slug, v.ranks[k], static_cast<int>(want[k]));
                if (v.ranks[k] == 20 && i == 2) ++topCapRanks;
            }
            std::string expectFail;
            auto rk = [&](Classes::Id id) { return v.ranks[static_cast<std::size_t>(id)]; };
            if (i == 0 && rk(Classes::Id::Warrior) != 7) expectFail = "Warrior L7 must read rank 7";
            if (i == 1 && (rk(Classes::Id::Warrior) != Classes::kRankMasteredUnequipped || rk(Classes::Id::Knight) != 3))
                expectFail = "mastered Warrior must read 100 (quarter) with Knight L3 equipped";
            if (i == 2 && topCapRanks < 2) expectFail = std::format("a T4 must lift its two T3 parents to the cap (20): {} found", topCapRanks);
            if (i == 3 && !v.mercFull) expectFail = "equipped Mercenary must carry the full ability spell";
            if (i == 4) {
                for (std::size_t k = 0; k < Classes::kClassCount; ++k) if (v.ranks[k] != 0) expectFail = "classes OFF must leave every rank 0";
                if (v.mercFull || v.mercQuarter || v.scoutQuarter || v.scoutLevel) expectFail += " abilities must be removed";
            }
            if (i == 0) {
                if (v.perksOnBase != v.perksExpected) expectFail += std::format(" the carrier perks on the base NPC: {} of {}", v.perksOnBase, v.perksExpected);
                if (v.processLive && v.entriesLive < v.entriesExpected) expectFail += std::format(" live perk entries {} of {}", v.entriesLive, v.entriesExpected);
            }
            const bool ok = diff.empty() && expectFail.empty();
            if (!ok) {
                s.n[2] += 1;
                if (c.elapsed - s.n[1] < 8.0f) { RequestCarrier(c.id); return Step::Continue(); }   // the reconcile retries on the main thread; allow 8 s
                return Step::Fail(std::format("scenario {} '{}': mismatch after 8 s:{}{} {}", i, kScn[i].name, diff, diff.empty() ? "" : ";", expectFail));
            }
            s.note += std::format("[{}: OK{}] ", kScn[i].name, i == 0 ? std::format(" perks {}/{} on base, entries {}/{} live{}", v.perksOnBase, v.perksExpected, v.entriesLive,
                                                                                    v.entriesExpected, v.processLive ? "" : " (no live process: entries not counted)") : std::string{});
            s.n[0] += 1;
            s.b[0] = false;
            return Step::Continue();
        }

        // ── class.scout_speed ──────────────────────────────────────────────────────────────────────────
        // Scout +20% speed must apply at once (reviewer SEV-3-field: not only after he loots something). SpeedMult read before and after
        // equipping a mastered Scout through the writers; no looting is involved. n[0] before, b[0] applied.
        Step ScoutStep(Ctx& c) {
            auto&  s = *c.s;
            auto*  avo = c.follower->AsActorValueOwner();
            const double sp = avo ? avo->GetActorValue(RE::ActorValue::kSpeedMult) : 0.0;
            if (!s.b[0]) {
                s.n[0] = sp;
                std::string why;
                if (!ApplyClass(c.id, "scout", 20, true, why)) return Step::Retry("could not equip a mastered Scout: " + why);
                Classes::RefreshCarriers(c.id);
                s.b[0] = true;
                s.n[1] = c.elapsed;
                return Step::Continue();
            }
            RequestCarrier(c.id);
            Classes::DevCarrierView v;
            const bool haveV = FreshCarrier(c.id, std::chrono::steady_clock::now() - std::chrono::seconds(2), v);
            s.note = std::format("SpeedMult {:.1f} -> {:.1f} ({:.1f}% of before) {:.1f}s after the equip; Scout ability level present {}", s.n[0], sp,
                                 s.n[0] > 0 ? 100.0 * sp / s.n[0] : 0.0, c.elapsed - s.n[1], haveV ? v.scoutLevel : -1);
            if (s.n[0] > 0 && sp >= s.n[0] * 1.10) return Step::Pass("Scout speed applied immediately, no loot involved: " + s.note);
            if (c.elapsed - s.n[1] < 8.0f) return Step::Continue();
            return Step::Fail("Scout speed did NOT apply within 8 s of the equip: " + s.note);
        }

        // ── class.ally_buff_strength ───────────────────────────────────────────────────────────────────
        // marth Q58: how much stronger do Warlock / Nightblade make ally buffs, and the reviewer's rule that the bonus must NOT apply to an
        // ally buff. A temp row casts Courage on the nearest ally; the magnitude of the ACTIVE EFFECT on that ally is read three times (no
        // class, Warlock, Nightblade), each reading a FRESH cast (elapsedSeconds < 4). Verdict INFO with the three numbers.
        RE::Actor* OtherAlly(Ctx& c) {
            const auto snap = Followers::ActiveSnapshot();
            for (const RE::FormID f : *snap) {
                if (f == c.id || f == 0x14) continue;
                auto* a = RE::TESForm::LookupByID<RE::Actor>(f);
                if (a && !a->IsDead() && !a->IsDisabled()) return a;
            }
            return nullptr;
        }
        const char* AllyPre(Ctx& c) {
            if (const char* r = ClassPre()) return r;
            RE::FormID k = 0;
            if (!V(kCourage, k)) return "the Skyrim.esm Courage resolvable";
            if (!OtherAlly(c)) return "a second ally in the party (Courage is cast on the nearest ally; a lone follower has none)";
            return nullptr;
        }
        void AllyFix(Ctx&, FixtureRequest& r) {
            const RE::FormID k = VanillaForm(kCourage);
            r.cls.snapshot = true;
            r.spells.push_back(k);
            r.rows.push_back(Row(Vocab::kCondAlways, Vocab::kActCastTarget, k, 0.0f, 2 /*NearestAlly*/));
            AddFightRows(r);
            r.spawn.on = true;
        }
        // reads the Courage effect on the other ally: fresh = just (re)cast
        bool ReadCourage(Ctx& c, double& a_mag, bool& a_fresh) {
            auto* ally = OtherAlly(c);
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(VanillaForm(kCourage));
            auto* mt = ally ? ally->AsMagicTarget() : nullptr;
            auto* list = mt ? mt->GetActiveEffectList() : nullptr;
            if (!list || !sp || sp->effects.empty() || !sp->effects[0]) return false;
            auto* want = sp->effects[0]->baseEffect;
            for (auto* ae : *list) {
                if (!ae || !ae->effect || ae->effect->baseEffect != want) continue;
                a_mag = ae->magnitude;
                a_fresh = ae->elapsedSeconds < 4.0f;
                return true;
            }
            return false;
        }
        // n[0..2] magnitudes (none, Warlock, Nightblade), n[4] phase, b[0] phase applied, n[5] elapsed when applied.
        Step AllyStep(Ctx& c) {
            auto& s = *c.s;
            const int ph = static_cast<int>(s.n[4]);
            if (ph >= 3)
                return Step::Info(std::format("Courage on an ally: magnitude {:.2f} with no class, {:.2f} with Warlock (mastered), {:.2f} with Nightblade (mastered). The bonus must not "
                                              "apply to an ally buff, so all three should be equal{}", s.n[0], s.n[1], s.n[2],
                                              (std::fabs(s.n[1] - s.n[0]) > 0.01 || std::fabs(s.n[2] - s.n[0]) > 0.01) ? " -- THEY DIFFER: the bonus reaches ally buffs" : " -- they are equal"));
            if (!s.b[0]) {
                std::string why;
                bool ok = true;
                Classes::SetMastered(c.id, 0);
                if (ph == 0) ok = Classes::ClearEquipped(c.id);
                else ok = ApplyClass(c.id, ph == 1 ? "warlock" : "nightblade", 99, true, why);
                if (!ok) return Step::Retry(std::format("phase {} could not be set: {}", ph, why));
                Classes::RefreshCarriers(c.id);
                s.b[0] = true;
                s.n[5] = c.elapsed;
                return Step::Continue();
            }
            if (c.elapsed - s.n[5] < 3.0f) return Step::Continue();   // let the carriers settle before the cast that is read
            double mag = 0;
            bool fresh = false;
            if (ReadCourage(c, mag, fresh) && fresh) {
                s.n[ph] = mag;
                s.n[4] += 1;
                s.b[0] = false;
            }
            return Step::Continue();
        }

        // ── aim.h5_miss_budget ─────────────────────────────────────────────────────────────────────────
        // H5 (APMF 62dc4e7 + MFO 7e8dc21): the ch.29 projectile-aim claim keeps an Archer's last-10-shots window within the miss budget. Archer
        // class + a bow + arrows fixture + the spawned hostile; the rows declare the ranged loadout and attack. Counters from APMF's own
        // GetProjectileAimState (shots / corrected / hits / misses / the window). PASS: >= 10 shots resolved and the window obeys the budget.
        const char* AimPre(Ctx& c) {
            if (const char* r = ClassPre()) return r;
            if (!Config::g_aimBudget.load()) return "bAimBudget on (the H5 miss budget)";
            if (!APMFBridge::DevReadAim(c.id).api) return "Harbinger ABI >= 36 (ch.29 projectile aim)";
            return nullptr;
        }
        void AimFix(Ctx&, FixtureRequest& r) {
            r.cls.snapshot = true;
            r.items.push_back({ VanillaForm(kHuntingBow), 1 });
            r.items.push_back({ VanillaForm(kIronArrow), 40 });
            r.rows.push_back(Row(Vocab::kCondFoeAny, Vocab::kActEquipRanged));
            r.rows.push_back(Row(Vocab::kCondFoeAny, Vocab::kActAttack));
            r.spawn.on = true;
            r.spawn.respawns = 6;
            r.probes = kProbeAim;
        }
        // b[0] class applied, n[0] elapsed when applied, n[1] seen a claim
        Step AimStep(Ctx& c) {
            auto& s = *c.s;
            if (!s.b[0]) {
                std::string why;
                if (!ApplyClass(c.id, "archer", 5, false, why)) return Step::Retry("could not equip Archer: " + why);
                s.b[0] = true;
                s.n[0] = c.elapsed;
                return Step::Continue();
            }
            const auto a = APMFBridge::DevReadAim(c.id);
            s.note = std::format("state {} budget {}: shots {} corrected {} hits {} hitsOther {} misses {} pending {} window {}/{} misses | uncorrectable {} wouldCorrect {} rankStood {}",
                                 a.state, a.budget, a.shots, a.corrected, a.hits, a.hitsOther, a.misses, a.pending, a.windowMisses, a.windowFill, a.uncorrectable,
                                 a.wouldCorrect, a.rankStood);
            if (a.state >= 1 && a.state <= 2) s.n[1] = 1;
            if (a.state == 3) return Step::Fail("the ch.29 seat is NOT installed on this build ([ProjectileAim] bProjectileAim = 0 or an unsupported runtime): " + s.note);
            if (s.n[1] == 0 && c.elapsed - s.n[0] > 45.0f)
                return Step::Fail("no ch.29 claim engaged 45 s after the Archer equip (the sweep files one per ranged class follower): " + s.note);
            if (a.windowFill >= 10 || a.shots >= 12) {
                if (a.corrected == 0 && a.wouldCorrect == 0)
                    return Step::Fail("10+ shots resolved but Harbinger corrected none and would-correct none (the claim is not governing): " + s.note);
                if (a.windowMisses <= a.budget || a.budget >= 10)
                    return Step::Pass(std::format("the last-10 window holds {} miss(es) <= budget {}; {}", a.windowMisses, a.budget, s.note));
                return Step::Fail(std::format("the last-10 window holds {} misses > budget {}; {}", a.windowMisses, a.budget, s.note));
            }
            return Step::Continue();
        }
        Step AimTimeout(Ctx& c) { return Step::Retry("fewer than 10 shots resolved in the window (the hostile died / no line of sight / he did not shoot); " + c.s->note); }

        // ── census.* (log-only features: counters present) ────────────────────────────────────────────
        Step ShapeCensusStep(Ctx&) {
            const auto d = Actuation::DevShapeCensus();
            if (!Config::g_spellShapeCensus.load())
                return Step::Info("bSpellShapeCensus = 0: the C0 shape census is silenced by the INI (nothing to judge)");
            if (d.ran && d.total > 0 && d.buckets > 0)
                return Step::Pass(std::format("the shape census ran at load: {} castable spell(s), {} skipped, {} bucket(s), wall {} selfHazard {} scriptRider {}", d.total, d.skipped,
                                              d.buckets, d.wall, d.selfHazard, d.scriptRider));
            return Step::Fail(std::format("bSpellShapeCensus is on but the census never reached its totals line (ran {}, total {}, buckets {})", d.ran, d.total, d.buckets));
        }
        Step InheritCensusStep(Ctx& c) {
            const auto d = ProgAllocator::ReadInheritCensusStats();
            c.s->note = std::format("{} summary line(s) this load, treePerksHeld {} of {}, held-in-no-tree {}", d.summaries, d.treePerksHeld, d.treeSet, d.heldNoTree);
            if (d.summaries == 0) return Step::Continue();
            if (d.treePerksHeld <= 0)
                return Step::Fail("the census ran but treePerksHeld = 0 (HasPerk is not answering on this character: an INVALID census); " + c.s->note);
            return Step::Info("the hidden-perk census ran; BRING THE [inherit-census] LIST TO MARTH (rule vs bonus) before card C2: " + c.s->note);
        }
        Step InheritCensusTimeout(Ctx& c) { return Step::Retry("no census summary within the window (the player poll has not completed a pass); " + c.s->note); }
        const char* AlwaysPre(Ctx&) { return nullptr; }

    }

    void ClearDrivenState() {
        {
            std::lock_guard lk(g_cvMx);
            g_cv.clear();
        }
        g_cs.clear();
    }

    void RegisterDriven() {
        // Highest value first (registry order = pick order). The fixtures() functions are the SELF-SETUP: rows, spawn, class, probes.
        Register({ "costparity.fireforget", "1dfcuabcr0c",
                   "(self-setup) cost parity on a fire-and-forget heal: a hurt follower in a spawned fight casts Healing; magicka drops by the player-curve cost P",
                   120.0f, CostFFPre, CostFix, CostFFStep, CostTimeout });
        Register({ "costparity.concentration", "1dfcuabcr0c",
                   "(self-setup) cost parity on a concentration heal (Fast Healing): drain is P per second",
                   150.0f, CostConcPre, CostFix, CostConcStep, CostTimeout });
        Register({ "b1a.weapon_stays_right", "MFO-B1a (92b6e84)",
                   "(self-setup) a one-hander in the right hand: heal-left + offense rule leaves the weapon, no right claim",
                   120.0f, B1aPre, B1aFix, B1aStep, B1aTimeout });
        Register({ "buff.animated_claim_road", "bbf90fd",
                   "(self-setup) Oakflesh + Lesser Ward take the animated claim road and land",
                   120.0f, BuffPre, BuffFix, BuffStep, BuffTimeout });
        Register({ "class.c6_carriers", "84678a5 (C6)", "(self-setup) the class add-on live; the runner equips five class states and restores yours", 120.0f,
                   CarrierPre, ClassSnapFix, CarrierStep, nullptr });
        Register({ "class.scout_speed", "C6 reviewer SEV-3-field", "(self-setup) the class add-on live; a mastered Scout is equipped for the run", 60.0f,
                   CarrierPre, ClassSnapFix, ScoutStep, nullptr });
        Register({ "aim.h5_miss_budget", "H5 (62dc4e7 + 7e8dc21)",
                   "(self-setup) class add-on + Harbinger ABI 36 + bAimBudget; the runner equips Archer, a bow + arrows, and spawns a hostile",
                   360.0f, AimPre, AimFix, AimStep, AimTimeout });
        Register({ "class.ally_buff_strength", "marth Q58 (C6)",
                   "(self-setup) the class add-on live + a SECOND ally in the party; Courage is cast on the ally with no class / Warlock / Nightblade",
                   420.0f, AllyPre, AllyFix, AllyStep, nullptr });
        Register({ "census.shape_ran", "bbf90fd", "(self-setup) nothing: reads the shape-census counters from load", 20.0f, AlwaysPre,
                   nullptr, ShapeCensusStep, nullptr });
        Register({ "census.hidden_perk_ran", "92604e0", "(self-setup) nothing: reads the inherit-census counters (the player must be in the world)", 90.0f,
                   AlwaysPre, nullptr, InheritCensusStep, InheritCensusTimeout });
    }

}
