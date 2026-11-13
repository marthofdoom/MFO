// THE SEEDED SELF-TESTS (dev builds only). One Register() call per test in RegisterAll(); how to add one is
// Docs/SELFTEST.md "Adding a test". Source list: the IN-GAME TEST LIST (ClickUp 1dfcuabcqdm).
//
// SELF-DRIVING (2026-11-10): these tests no longer ride the follower's own board. Each fixtures() asks the runner for the TEMP ROWS, the
// spawned hostile and the spells it needs (Setup.cpp / Fixtures.cpp; all put back afterwards), and judges what those temp rows make the
// engine do. The new self-driving tests live in TestsDrive.cpp. Reads only (#23): hands, inventory counts,
// actor values, the spell record classifiers MFO already has (Actuation::HealsHealth / ClassifyArchetype),
// Gambit::lastFiredAt (the Scheduler's display stamp), the dev-only ProgAllocator::ReadHmsCapView (main
// thread, posted), and a dev-only PASSIVE TESHitEvent sink (ShotSink below: an event sink on the script event
// source, like CrimeProbe's; no hook, no engine seat).
//
// IDS ARE FROZEN once a dev build ships them: they are the 'MFST' result keys (#10).

#include "selftest/SelfTest_internal.h"
#include "Config.h"
#include "Followers.h"
#include "Vocabulary.h"
#include "cast/Actuation.h"
#include "logistics/Logistics.h"
#include "MainThread.h"
#include "progression/ProgAllocator.h"
#include "apmf/APMFBridge.h"

#include <cmath>
#include <deque>

namespace MFO::SelfTest {

    namespace {

        // Skyrim.esm local ids, verified against the master's own records (EDID) 2026-11-09.
        constexpr RE::FormID kSkoomaLocal         = 0x00057A7A;   // ALCH "Skooma"
        constexpr RE::FormID kMinorHealPotionLocal = 0x0003EADD;  // ALCH "RestoreHealth01" (Potion of Minor Healing)

        bool IsCastOp(const std::string& a_op) {
            return a_op == Vocab::kActCastSelf || a_op == Vocab::kActCastTarget || a_op == Vocab::kActCastPlayer;
        }
        bool Has(const std::vector<RE::FormID>& a_set, RE::FormID a_f) {
            return a_f && std::find(a_set.begin(), a_set.end(), a_f) != a_set.end();
        }
        void AddUnique(std::vector<RE::FormID>& a_set, RE::FormID a_f) {
            if (!Has(a_set, a_f)) a_set.push_back(a_f);
        }
        RE::FormID HandForm(RE::Actor* a, bool a_left) {
            auto* f = a ? a->GetEquippedObject(a_left) : nullptr;
            return f ? f->GetFormID() : 0;
        }
        void SpellsFixture(Ctx& c, FixtureRequest& r, int a_setA, int a_setB) {
            for (const int k : { a_setA, a_setB }) {
                if (k < 0) continue;
                for (const auto sid : c.s->set[k]) r.spells.push_back(sid);   // taught only when missing
            }
        }

        // ── MFO-B1: heal LEFT + offense RIGHT together ───────────────────────────────────────────────
        // Test list: "right-only offense claim logs Equipped right, no left equip/debt; left heal not
        // displaced". Observable core: the follower holds a heal spell in the LEFT hand and an offensive
        // spell in the RIGHT hand at the same moment. set[0] = heal spells, set[1] = offense spells.
        constexpr RE::FormID kHealingLocal = 0x00012FCC, kFlamesLocal = 0x00012FCD, kBoundBowLocal = 0x000211ED;
        bool MeleeInRight(RE::Actor* a) {
            auto* w = a && a->GetEquippedObject(false) ? a->GetEquippedObject(false)->As<RE::TESObjectWEAP>() : nullptr;
            return w && !w->IsBow() && !w->IsCrossbow() && !w->IsStaff() && !w->IsBound();
        }
        const char* B1Pre(Ctx& c) {
            const RE::FormID h = VanillaForm(kHealingLocal), f = VanillaForm(kFlamesLocal);
            if (!h || !f) return "the Skyrim.esm Healing / Flames resolvable";
            c.s->set[0] = { h };
            c.s->set[1] = { f };
            return nullptr;
        }
        void B1Fix(Ctx& c, FixtureRequest& r) {
            SpellsFixture(c, r, 0, 1);
            r.rows.push_back({ Table::Combat, Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.95f, c.s->set[0][0], 0 });
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, c.s->set[1][0], 0 });
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActAttack, 0.0f, 0, 0 });
            r.spawn.on = true;
            r.spawn.hurtPct = 40.0f;
        }
        Step B1Step(Ctx& c) {
            auto&            s  = *c.s;
            if (!s.b[7]) {
                s.b[7] = true;
                if (MeleeInRight(c.follower))
                    return Step::Info("not applicable to this follower: he wields a one-hander in the right hand, so heal-left + offense-RIGHT cannot arise "
                                      "(b1a.weapon_stays_right judges that loadout)");
            }
            const RE::FormID L  = HandForm(c.follower, true);
            const RE::FormID R  = HandForm(c.follower, false);
            const bool       hl = Has(s.set[0], L), orr = Has(s.set[1], R), ol = Has(s.set[1], L);
            s.n[0] += 1;
            if (hl) s.n[1] += 1;
            if (orr) s.n[2] += 1;
            if (ol) s.n[3] += 1;
            // MFO-B1a: heal LEFT + a WEAPON in the RIGHT (WeaponHandActive must read true for it; the right
            // hand is then not planned for a spell, so this shape never reaches heal-left + offense-right).
            auto* rf = c.follower ? c.follower->GetEquippedObject(false) : nullptr;
            if (hl && rf && rf->As<RE::TESObjectWEAP>()) s.n[4] += 1;
            s.note = std::format("samples {:.0f}: heal-left {:.0f}, offense-right {:.0f}, offense-LEFT {:.0f}, "
                                 "heal-left+weapon-right {:.0f} (MFO-B1a: the weapon must stay, no right claim); "
                                 "last L '{}' R '{}'",
                                 s.n[0], s.n[1], s.n[2], s.n[3], s.n[4], L ? FormName(L) : "empty",
                                 R ? FormName(R) : "empty");
            if (hl && orr)
                return Step::Pass(std::format("left '{}' + right '{}' held together; {}", FormName(L), FormName(R),
                                              s.note));
            if (!c.inCombat) return Step::Retry("combat ended before heal-left + offense-right were seen together; " + s.note);
            return Step::Continue();
        }
        Step B1Timeout(Ctx& c) {
            // FAIL only when BOTH halves were seen (heal left AND offense right) but never at once; anything
            // less means the fight never asked for both, which is a RETRY (review F3a, a1c0e6f).
            if (c.s->n[1] > 0 && c.s->n[2] > 0)
                return Step::Fail("a heal in the left and an offense spell in the right were each seen, but never "
                                  "together; " + c.s->note);
            return Step::Retry("the fight never produced both halves (heal-left and offense-right) in the window; " +
                               c.s->note);
        }

        // ── 86e3t1gfq: Skooma is not drunk by the AI ────────────────────────────────────────────────
        // Test list: a follower with a cast gambit carrying Skooma + real potions in combat: no "CAST
        // Skooma ... AI-fired"; real potions still drunk. The direct evidence is the Skooma count: it must
        // never fall during the fight. A PASS needs DRINK PRESSURE on a pool SKOOMA ITSELF RESTORES (read from
        // its record at precondition time, review F3b): that pool under 50% at some point (or, for Health, a
        // real health potion drunk). Without pressure the AI had no reason to drink: RETRY.
        // set[0] = { Skooma }, set[1] = { the minor healing potion, only when he had no health potion }.
        // n[0] start Skooma, n[1] start health potions, n[2..4] min H / M / S fraction, n[5] max health
        // potions drunk, n[6] the pool mask (bit 0 H, 1 M, 2 S) Skooma restores (7 = none declared: all).
        constexpr RE::ActorValue kPoolAV[3] = { RE::ActorValue::kHealth, RE::ActorValue::kMagicka,
                                                RE::ActorValue::kStamina };
        constexpr const char*    kPoolName[3] = { "Health", "Magicka", "Stamina" };
        int SkoomaPools(const RE::AlchemyItem* a_alc, bool& a_declared) {
            int mask = 0;
            for (const auto* e : a_alc->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                if (!mgef || mgef->IsHostile() || mgef->IsDetrimental()) continue;
                const auto arch = mgef->data.archetype;
                if (arch != RE::EffectArchetypes::ArchetypeID::kValueModifier &&
                    arch != RE::EffectArchetypes::ArchetypeID::kDualValueModifier)
                    continue;
                for (int p = 0; p < 3; ++p)
                    if (mgef->data.primaryAV == kPoolAV[p]) mask |= 1 << p;
            }
            a_declared = mask != 0;
            return mask ? mask : 7;
        }
        const char* SkoomaPre(Ctx& c) {
            const RE::FormID sk  = VanillaForm(kSkoomaLocal);
            auto*            alc = sk ? RE::TESForm::LookupByID<RE::AlchemyItem>(sk) : nullptr;
            if (!alc) return "Skyrim.esm Skooma (00057A7A) resolvable";
            const RE::FormID fl = VanillaForm(kFlamesLocal);
            if (!fl) return "the Skyrim.esm Flames resolvable";
            c.s->set[0] = { sk };
            c.s->set[2] = { fl };
            bool declared = false;
            c.s->n[6]     = SkoomaPools(alc, declared);
            c.s->b[2]     = declared;
            if (Logistics::CountPotions(c.follower, RE::ActorValue::kHealth) < 1)
                if (const RE::FormID hp = VanillaForm(kMinorHealPotionLocal); hp) c.s->set[1] = { hp };
            return nullptr;
        }
        void SkoomaFix(Ctx& c, FixtureRequest& r) {
            r.items.push_back({ c.s->set[0][0], 1 });
            if (!c.s->set[1].empty()) r.items.push_back({ c.s->set[1][0], 2 });
            // the case the Harbinger allow-list covers: a follower with a CAST rule in a fight, hurt enough for the AI to want a drink
            r.spells.push_back(c.s->set[2][0]);
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, c.s->set[2][0], 0 });
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActAttack, 0.0f, 0, 0 });
            r.spawn.on = true;
            r.spawn.hurtPct = 60.0f;
        }
        bool SkoomaPressure(const Scratch& s) {
            const int mask = static_cast<int>(s.n[6]);
            for (int p = 0; p < 3; ++p)
                if ((mask & (1 << p)) && s.n[2 + p] < 0.5) return true;
            return (mask & 1) && s.n[5] > 0;
        }
        std::string SkoomaNote(const Ctx& c, std::int32_t a_sk) {
            const auto& s    = *c.s;
            auto*       alc  = RE::TESForm::LookupByID<RE::AlchemyItem>(s.set[0][0]);
            const int   mask = static_cast<int>(s.n[6]);
            std::string pools;
            for (int p = 0; p < 3; ++p)
                if (mask & (1 << p)) pools += (pools.empty() ? "" : "/") + std::string(kPoolName[p]);
            return std::format("Skooma {:.0f} -> {}, watched pools {}{}, min H {:.0f}% M {:.0f}% S {:.0f}%, real health "
                               "potions drunk {:.0f}, MFO PotionIsDrug = {}",
                               s.n[0], a_sk, pools, s.b[2] ? " (what its record restores)" : " (its record restores none: all)",
                               s.n[2] * 100.0, s.n[3] * 100.0, s.n[4] * 100.0, s.n[5],
                               Logistics::PotionIsDrug(alc) ? "yes" : "no");
        }
        Step SkoomaStep(Ctx& c) {
            auto& s = *c.s;
            const std::int32_t sk = CountItem(c.follower, s.set[0][0]);
            const int          hp = Logistics::CountPotions(c.follower, RE::ActorValue::kHealth);
            if (!s.b[0]) {
                s.b[0] = true;
                s.n[0] = sk;
                s.n[1] = hp;
                s.n[2] = s.n[3] = s.n[4] = 1.0;
            }
            for (int p = 0; p < 3; ++p) s.n[2 + p] = std::min<double>(s.n[2 + p], Vocab::Pct(c.follower, kPoolAV[p]));
            s.n[5] = std::max<double>(s.n[5], s.n[1] - hp);
            s.note = SkoomaNote(c, sk);
            if (sk < s.n[0]) return Step::Fail("the Skooma count FELL during combat (the AI drank it): " + s.note);
            if (!c.inCombat) {
                if (c.elapsed >= 20.0f && SkoomaPressure(s)) return Step::Pass("Skooma kept through the fight: " + s.note);
                return Step::Retry(std::format("combat ended after {:.0f}s without drink pressure on a watched pool "
                                               "(needs >= 20 s and a watched pool under 50%): {}",
                                               c.elapsed, s.note));
            }
            return Step::Continue();
        }
        Step SkoomaTimeout(Ctx& c) {
            if (SkoomaPressure(*c.s)) return Step::Pass("Skooma kept through the whole window: " + c.s->note);
            return Step::Retry("no drink pressure on a watched pool in the window: " + c.s->note);
        }

        // ── 86e3t1gfx: bound bow cast + fired ───────────────────────────────────────────────────────
        // Test list: "bound-bow gambit in combat ... bow in hand and firing". THE SHOT SIGNAL IS A HIT
        // (review F1, a1c0e6f): a TESHitEvent whose cause is this follower and whose source is a bound bow.
        // The equipped-ammo count is NOT used: an NPC's bound-arrow decrement is unobserved, and a dismiss /
        // swap removes the arrows too. A miss produces no hit, so "no hit" never FAILs: bow in hand 30 s with
        // no hit (missed, out of range, no LoS, melee range) is a RETRY. FAIL only when the rule FIRED and no
        // bound bow reached a hand within 15 s after it.
        // set[0] = the bound-bow spells of his combat rules. b[0] the rule fired since the start, b[1] the bow
        // reached a hand; n[0] when (run s), n[1] steady ms when, n[2] when the rule first fired (run s).
        // projectile + bash flag (review F9, fb4a866): a bow BASH also raises TESHitEvent with source = the bow,
        // projectile 0 and kBashAttack set (fork RE/T/TESHitEvent.h); only a projectile hit that is not a bash is a shot.
        struct Shot { RE::FormID cause = 0, source = 0, target = 0, projectile = 0; bool bash = false; double atMs = 0; };
        std::mutex                g_shotMx;   // leaf lock: the sink and the worker copy under it, nothing else
        std::deque<Shot>          g_shots;
        std::atomic<bool>         g_sinkLive{ false };
        std::atomic<std::int64_t> g_sinkPostedMs{ 0 };
        double NowMs() {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        // PASSIVE (event thread): reads the event's own refs, the FormID mirror (IsTrackedFast, any thread), and
        // stores ids. Nothing else.
        class ShotSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static ShotSink* GetSingleton() { static ShotSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override {
                if (!a_e || !a_e->cause || !a_e->target) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID cause = a_e->cause->GetFormID();
                if (!Followers::IsTrackedFast(cause)) return RE::BSEventNotifyControl::kContinue;
                Shot sh{ cause, a_e->source, a_e->target->GetFormID(), a_e->projectile,
                         a_e->flags.any(RE::TESHitEvent::Flag::kBashAttack), NowMs() };
                std::lock_guard lk(g_shotMx);
                g_shots.push_back(sh);
                while (g_shots.size() > 64) g_shots.pop_front();
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool SummonsBow(RE::SpellItem* a_sp) {
            if (!a_sp || Actuation::ClassifyArchetype(a_sp).shape != Actuation::SpellShape::BoundWeapon) return false;
            for (const auto* e : a_sp->effects) {
                const auto* mgef = e ? e->baseEffect : nullptr;
                auto*       w    = (mgef && mgef->data.associatedForm) ? mgef->data.associatedForm->As<RE::TESObjectWEAP>()
                                                                       : nullptr;
                if (w && w->IsBow()) return true;
            }
            return false;
        }
        const char* BowPre(Ctx& c) {
            if (!g_sinkLive.load()) return "the self-test hit sink registered (posted on the first tick)";
            const RE::FormID bb = VanillaForm(kBoundBowLocal);
            auto*            sp = bb ? RE::TESForm::LookupByID<RE::SpellItem>(bb) : nullptr;
            if (!SummonsBow(sp)) return "the Skyrim.esm Bound Bow (000211ED) resolvable and summoning a bow";
            c.s->set[0] = { bb };
            return nullptr;
        }
        void BowFix(Ctx& c, FixtureRequest& r) {
            SpellsFixture(c, r, 0, -1);
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActCastSelf, 0.0f, c.s->set[0][0], 0 });
            r.rows.push_back({ Table::Combat, Vocab::kCondFoeAny, Vocab::kActAttack, 0.0f, 0, 0 });
            r.spawn.on = true;
        }
        RE::TESObjectWEAP* BoundBowInHand(RE::Actor* a) {
            for (const bool left : { false, true }) {
                auto* f = a->GetEquippedObject(left);
                auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
                if (w && w->IsBound() && w->IsBow()) return w;
            }
            return nullptr;
        }
        bool IsBoundBowForm(RE::FormID a_id) {
            auto* w = a_id ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_id) : nullptr;
            return w && w->IsBound() && w->IsBow();
        }
        Step BowStep(Ctx& c) {
            auto& s = *c.s;
            for (const auto& g : c.state->combat())
                if (g.enabled && Has(s.set[0], g.actionParamForm) && g.lastFiredAt >= c.startedAt && !s.b[0]) {
                    s.b[0] = true;
                    s.n[2] = c.elapsed;
                }
            auto* bow = BoundBowInHand(c.follower);
            if (bow && !s.b[1]) {
                s.b[1] = true;
                s.n[0] = c.elapsed;
                s.n[1] = NowMs();
            }
            std::vector<Shot> shots;
            {
                std::lock_guard lk(g_shotMx);
                shots.assign(g_shots.begin(), g_shots.end());
            }
            s.note = std::format("rule fired {}, bound bow in hand {}{}", s.b[0] ? "yes" : "no", s.b[1] ? "yes" : "no",
                                 s.b[1] ? std::format(" (since t={:.1f}s)", s.n[0]) : "");
            if (s.b[1]) {
                for (const auto& sh : shots) {
                    if (sh.cause != c.id || sh.atMs < s.n[1] || !IsBoundBowForm(sh.source)) continue;
                    if (sh.projectile == 0 || sh.bash) continue;   // a bow bash, not a shot (F9)
                    return Step::Pass(std::format("an ARROW hit by bound bow '{}' on {:08X} '{}' (TESHitEvent, cause = "
                                                  "this follower, projectile {:08X}, not a bash); {}",
                                                  FormName(sh.source), sh.target, FormName(sh.target), sh.projectile,
                                                  s.note));
                }
                if (c.elapsed - s.n[0] > 30.0f)
                    return Step::Retry("a bound bow was in hand 30 s with no hit by it (missed, out of range, no line "
                                       "of sight, or a melee fight; a hit is the only shot signal this test trusts); " +
                                       s.note);
            }
            if (s.b[0] && !s.b[1] && c.elapsed - s.n[2] > 15.0f)
                return Step::Fail("the bound-bow rule FIRED but no bound bow reached a hand within 15 s; " + s.note);
            if (!c.inCombat) return Step::Retry("combat ended first; " + s.note);
            return Step::Continue();
        }
        Step BowTimeout(Ctx& c) {
            if (!c.s->b[0]) return Step::Retry("the bound-bow rule never fired (its WHEN never held?); " + c.s->note);
            return Step::Retry("the window ended before a verdict; " + c.s->note);
        }

        // ── 86e3d6dnv: HMS adopted-total cap ────────────────────────────────────────────────────────
        // Test list: "caps fire, follower totals track the player's". Judged on the SAME numbers the cap uses
        // (review F2, a1c0e6f), read on the main thread by ProgAllocator::ReadHmsCapView: the held total
        // (hmsBaseline + hmsCumulative) against g_playerHmsTotalLast, the player sample taken at the last
        // level-up, with the cap's own 0.5 tolerance. Above it with EVERY pool at its floor (the trim's
        // documented limit, Hms.cpp "ABOVE the cap: every pool is at its floor") is a PASS, never a FAIL.
        // Precondition: HMS on (bHmsRedistribute) and the cap on, a CAPTURED record with a class profile, a
        // player sample, the follower 2 minutes in the party. Judged after two reads 2 s apart agree.
        // n[0] / n[1] last held total / sample, n[2] when they last changed (run s), n[3..5] last live base H/M/S.
        // After the settle each pool's LIVE base must sit within 0.5 of its held value (F11).
        struct HmsRead { ProgAllocator::HmsCapView v; bool found = false; bool inFlight = false;
                         std::chrono::steady_clock::time_point at{}; };
        std::mutex                              g_hmsMx;   // leaf lock
        std::unordered_map<RE::FormID, HmsRead> g_hms;
        void RequestHms(RE::FormID a_id) {
            {
                std::lock_guard lk(g_hmsMx);
                auto& r = g_hms[a_id];
                if (r.inFlight) return;
                r.inFlight = true;
            }
            MainThread::Post([a_id]() {
                ProgAllocator::HmsCapView v;
                const bool f = ProgAllocator::ReadHmsCapView(a_id, v);
                std::lock_guard lk(g_hmsMx);
                auto& r    = g_hms[a_id];
                r.v        = v;
                r.found    = f;
                r.inFlight = false;
                r.at       = std::chrono::steady_clock::now();
            });
        }
        // The cached read for a_id if it is no older than a_maxAgeSec and not from before a_notBefore; asks for a
        // new one (main thread) whenever the cached one is missing or over 1 s old.
        bool FreshHms(RE::FormID a_id, float a_maxAgeSec, std::chrono::steady_clock::time_point a_notBefore, HmsRead& a_out) {
            const auto now     = std::chrono::steady_clock::now();
            bool       have    = false;
            bool       refresh = true;
            {
                std::lock_guard lk(g_hmsMx);
                if (auto it = g_hms.find(a_id); it != g_hms.end() && it->second.at.time_since_epoch().count() != 0) {
                    const float age = std::chrono::duration<float>(now - it->second.at).count();
                    if (it->second.at >= a_notBefore && age <= a_maxAgeSec) {
                        a_out   = it->second;
                        have    = true;
                        refresh = age > 1.0f;
                    }
                }
            }
            if (refresh) RequestHms(a_id);
            return have;
        }
        const char* HmsPre(Ctx& c) {
            if (c.inCombat) return "out of combat";
            if (!Config::g_hmsRedistribute.load()) return "bHmsRedistribute = 1 (HMS is off)";
            if (!Config::g_hmsCapToPlayerTotal.load()) return "bHmsCapToPlayerTotal = 1 (the cap is off)";
            if (c.seenFor < 120.0f) return "the follower in the party for 2 minutes this session (the HMS poll ran)";
            if (c.follower->IsCommandedActor()) return "a real follower, not a summon";
            HmsRead r;
            if (!FreshHms(c.id, 5.0f, {}, r)) return "a captured HMS record (reading it)";
            if (!r.found) return "an HMS record for this follower (none)";
            if (!r.v.captured) return "a CAPTURED HMS record (not adopted yet)";
            if (!r.v.haveProfile) return "an HMS class profile (the cap is not reached without one)";
            if (r.v.playerSample <= 0.0f) return "a player HMS sample (taken at a level-up or the first poll)";
            return nullptr;
        }
        Step HmsStep(Ctx& c) {
            auto& s = *c.s;
            if (c.inCombat) return Step::Retry("combat started before the reading settled");
            HmsRead r;
            if (!FreshHms(c.id, 3.0f, c.startedAt, r)) return Step::Continue();
            if (!r.found || !r.v.captured) return Step::Retry("the HMS record is gone or no longer captured");
            const auto& v  = r.v;
            const float ht = v.held[0] + v.held[1] + v.held[2];
            float live[3];
            for (int p = 0; p < 3; ++p) live[p] = Followers::GetFollowerHMS(c.follower, p);
            // The settle window also restarts while the LIVE base moves (still converging, review F11).
            if (!s.b[0] || ht != s.n[0] || v.playerSample != s.n[1] || live[0] != s.n[3] || live[1] != s.n[4] ||
                live[2] != s.n[5]) {
                s.b[0] = true;
                s.n[0] = ht;
                s.n[1] = v.playerSample;
                s.n[2] = c.elapsed;
                s.n[3] = live[0];
                s.n[4] = live[1];
                s.n[5] = live[2];
            }
            const bool atFloor = v.held[0] <= v.floor[0] + 0.5f && v.held[1] <= v.floor[1] + 0.5f &&
                                 v.held[2] <= v.floor[2] + 0.5f;
            s.note = std::format("held H/M/S {:.0f}/{:.0f}/{:.0f} = {:.0f} vs the cap's player sample {:.0f} (last "
                                 "level-up); floors {:.0f}/{:.0f}/{:.0f}; live base {:.0f}/{:.0f}/{:.0f}; fixedStat {}",
                                 v.held[0], v.held[1], v.held[2], ht, v.playerSample, v.floor[0], v.floor[1], v.floor[2],
                                 live[0], live[1], live[2], v.fixedStat ? "yes" : "no");
            if (c.elapsed - s.n[2] < 2.0f) return Step::Continue();
            // F11 (fb4a866): the engine must actually carry what MFO holds. A pool whose live base is more than
            // 0.5 off its held value after the settle is a FAIL; an unreadable live base (<= 0) is a RETRY.
            static constexpr const char* kPool[3] = { "Health", "Magicka", "Stamina" };
            for (int p = 0; p < 3; ++p)
                if (!(live[p] > 0.0f))
                    return Step::Retry(std::format("the live base {} read {:.1f} (unreadable); {}", kPool[p], live[p],
                                                   s.note));
            for (int p = 0; p < 3; ++p)
                if (std::fabs(live[p] - v.held[p]) > 0.5f)
                    return Step::Fail(std::format("live base {} {:.1f} != held {:.1f} (off by {:.1f}) after a 2 s "
                                                  "settle: the engine does not carry what MFO holds; {}",
                                                  kPool[p], live[p], v.held[p], live[p] - v.held[p], s.note));
            if (ht <= v.playerSample + 0.5f) return Step::Pass("held total at or under the cap: " + s.note);
            if (atFloor)
                return Step::Pass("held total above the sample with EVERY pool at its floor (the cap's documented "
                                  "limit, by design): " + s.note);
            return Step::Fail(std::format("held total {:.0f} ABOVE the cap's player sample by {:.0f} with a pool above "
                                          "its floor: {}",
                                          ht, ht - v.playerSample, s.note));
        }
        Step HmsTimeout(Ctx& c) { return Step::Retry("no settled HMS read in the window: " + c.s->note); }

        // ── 1dfcuabcrqq: Progression perk entries are never duplicated ──────────────────────────────────
        // Actor::ApplyPerksFromBase re-applies every base perk with no presence check, and the entry-point
        // insert never dedupes, so the old MFO grant / reapply / strip / respec paths put a second copy of
        // every base entry-point entry on the follower each call (one extra per load at least, from the
        // post-load reapply). Judged by ProgAllocator::ReadPerkEntryView (main thread): every entry-point
        // entry the base says is live, counted in the follower's live arrays. PASS = none in its array more
        // than once on two reads 2 s apart; FAIL = any duplicated. Needs an enrolled follower with at least
        // one MFO-granted rank and a middle-high process, 30 s in the party (the session reapply ran).
        // n[0] last duplicated, n[1] last epEntries, n[2] when they last changed (run s), n[3] first allocatedRanks.
        struct PerkRead { ProgAllocator::PerkEntryView v; bool found = false; bool inFlight = false;
                          std::chrono::steady_clock::time_point at{}; };
        std::mutex                               g_perkMx;   // leaf lock
        std::unordered_map<RE::FormID, PerkRead> g_perk;
        bool FreshPerk(RE::FormID a_id, float a_maxAgeSec, std::chrono::steady_clock::time_point a_notBefore,
                       PerkRead& a_out) {
            const auto now     = std::chrono::steady_clock::now();
            bool       have    = false;
            bool       refresh = true;
            bool       post    = false;
            {
                std::lock_guard lk(g_perkMx);
                auto& r = g_perk[a_id];
                if (r.at.time_since_epoch().count() != 0) {
                    const float age = std::chrono::duration<float>(now - r.at).count();
                    if (r.at >= a_notBefore && age <= a_maxAgeSec) {
                        a_out   = r;
                        have    = true;
                        refresh = age > 1.0f;
                    }
                }
                if (refresh && !r.inFlight) {
                    r.inFlight = true;
                    post       = true;
                }
            }
            if (post)
                MainThread::Post([a_id]() {
                    ProgAllocator::PerkEntryView v;
                    const bool f = ProgAllocator::ReadPerkEntryView(a_id, v);
                    std::lock_guard lk(g_perkMx);
                    auto& r    = g_perk[a_id];
                    r.v        = v;
                    r.found    = f;
                    r.inFlight = false;
                    r.at       = std::chrono::steady_clock::now();
                });
            return have;
        }
        // ── hms.above_parity_gate (e3d15c4 / ClickUp 86e3jw3b4) ─────────────────────────────────────────
        // A follower whose HELD total already sits at or above the player's gets his level awards WITHHELD until the player catches up. The gate
        // runs on a level-up edge (Hms.cpp), which the DLL cannot cause, so this test reads the SAME numbers the cap/gate use (ReadHmsCapView):
        // if the player's sample moved inside the window (a level-up happened), the held total must not exceed the new sample (+0.5) unless every
        // pool sits at its floor; if nothing moved, the verdict is INFO with the standing numbers (gate armed or not), never a pass by timer.
        // n[0] sample at start, n[1] held total at start, b[0] init, b[1] armed (held >= sample - 0.5).
        Step HmsGateStep(Ctx& c) {
            auto& s = *c.s;
            if (c.inCombat) return Step::Retry("combat started before the reading settled");
            HmsRead r;
            if (!FreshHms(c.id, 3.0f, c.startedAt, r)) return Step::Continue();
            if (!r.found || !r.v.captured) return Step::Retry("the HMS record is gone or no longer captured");
            const auto& v  = r.v;
            const float ht = v.held[0] + v.held[1] + v.held[2];
            if (!s.b[0]) {
                s.b[0] = true;
                s.n[0] = v.playerSample;
                s.n[1] = ht;
                s.b[1] = ht >= v.playerSample - 0.5f;
                return Step::Continue();
            }
            s.note = std::format("held total {:.1f} vs player sample {:.1f} (at the start: held {:.1f}, sample {:.1f}); the gate is {} for him", ht, v.playerSample, s.n[1],
                                 s.n[0], s.b[1] ? "ARMED (he is at or above parity)" : "not armed (he is below parity)");
            if (v.playerSample == s.n[0]) return Step::Continue();   // no level-up yet
            bool atFloor = true;
            for (int p = 0; p < 3; ++p) atFloor = atFloor && v.held[p] <= v.floor[p] + 0.5f;
            if (ht <= v.playerSample + 0.5f || atFloor)
                return Step::Pass("a level-up happened in the window and the held total stayed within the player's" + std::string(atFloor ? " (every pool at its floor)" : "") + "; " + s.note);
            return Step::Fail("a level-up happened in the window and the held total is ABOVE the player's with a pool above its floor; " + s.note);
        }
        Step HmsGateTimeout(Ctx& c) {
            return Step::Info("NOT EXERCISED: the gate runs on a player level-up and none happened in the window (the DLL cannot cause one). Standing state: " + c.s->note);
        }

        std::string PerkNote(const ProgAllocator::PerkEntryView& v) {
            return std::format("{} entry-point entries on {} base perks, duplicated {} (extra copies {}, max {}x{}), "
                               "missing {}, MFO-granted ranks {}",
                               v.epEntries, v.basePerks, v.duplicated, v.extraCopies, v.maxCopies,
                               v.worstPerk ? std::format(" perk {:08X} ep {}", v.worstPerk, v.worstEp) : std::string(),
                               v.missing, v.allocatedRanks);
        }
        const char* PerkPre(Ctx& c) {
            if (c.seenFor < 30.0f) return "the follower in the party for 30 s this session (the session reapply ran)";
            if (c.follower->IsCommandedActor()) return "a real follower, not a summon";
            PerkRead r;
            if (!FreshPerk(c.id, 5.0f, {}, r)) return "a Progression record (reading it)";
            if (!r.found || !r.v.enrolled) return "a follower enrolled in Progression";
            if (r.v.allocatedRanks < 1) return "at least one perk rank granted by MFO";
            if (!r.v.process) return "a middle-high process (the follower loaded nearby)";
            if (r.v.epEntries < 1) return "a base perk with an entry-point entry";
            return nullptr;
        }
        Step PerkStep(Ctx& c) {
            auto&    s = *c.s;
            PerkRead r;
            if (!FreshPerk(c.id, 3.0f, c.startedAt, r)) return Step::Continue();
            if (!r.found || !r.v.enrolled) return Step::Retry("the Progression record is gone");
            if (!r.v.process) return Step::Retry("the follower lost his middle-high process (unloaded)");
            const auto& v = r.v;
            if (!s.b[0]) s.n[3] = v.allocatedRanks;
            if (!s.b[0] || v.duplicated != s.n[0] || v.epEntries != s.n[1]) {
                s.b[0] = true;
                s.n[0] = v.duplicated;
                s.n[1] = v.epEntries;
                s.n[2] = c.elapsed;
            }
            s.note = PerkNote(v) + std::format(" (granted ranks at start {:.0f})", s.n[3]);
            if (v.duplicated > 0)
                return Step::Fail("a base perk's entry-point entry is live more than once (MFO, or another plugin "
                                  "calling ApplyPerksFromBase): " + s.note);
            if (c.elapsed - s.n[2] < 2.0f) return Step::Continue();
            return Step::Pass("every entry-point entry of the base perks is live at most once: " + s.note);
        }
        Step PerkTimeout(Ctx& c) { return Step::Retry("no settled perk-entry read in the window: " + c.s->note); }

    }

    void RegisterSinks() {
        if (g_sinkLive.load()) return;
        const auto nowMs = static_cast<std::int64_t>(NowMs());
        if (nowMs - g_sinkPostedMs.load() < 5000) return;   // one posted try per 5 s (a revert can drop it)
        g_sinkPostedMs.store(nowMs);
        MainThread::Post([]() {
            if (g_sinkLive.load()) return;
            if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESHitEvent>(ShotSink::GetSingleton());
                g_sinkLive.store(true);
                spdlog::info("[selftest] passive hit sink registered (bound_bow.fires shot signal)");
            }
        });
    }

    void ClearTestState() {
        {
            std::lock_guard lk(g_shotMx);
            g_shots.clear();
        }
        {
            std::lock_guard lk(g_perkMx);
            g_perk.clear();
        }
        ClearDrivenState();
        ClearT2State();
        std::lock_guard lk(g_hmsMx);
        g_hms.clear();
    }

    const std::vector<ManualItem>& ManualItems() {
        // What a human must look at, with the PASS condition from the in-game test list. Not run, only listed and counted in ALL DONE.
        static const std::vector<ManualItem> k = [] { std::vector<ManualItem> v = {
            { "retreat.flee_hold", "86e3kcvfk + Harbinger flee", "an engine Flee package cannot be provoked from the DLL without forcing the AI (it needs the engine's own flee decision)",
              "follower reaches the player at full HP while an engine Flee is current: log '[retreat] ... Flee package is still current ... holding', then 'engine Flee ended after Ns' + release; APMF [obs] current package = FECF983A, dPlayer <= 200 u" },
            { "visual.animated_casts", "bbf90fd / 1dfcuabbt0p", "an animation is a look", "Oakflesh, Lesser Ward and a Script-archetype spell (Cicero Arcane Pull) are visibly animated casts" },
            { "visual.frame_hitches", "92604e0", "frame time is a feel on the Deck", "no frame hitch while the hidden-perk census runs (full list once per load)" },
            { "ch18.power_floor", "1dfcuabcqd9", "attack-rate feel; the DLL has no ch.18 counter exposed", "Warrior 40% power floor: power share ~40%, attack rate not visibly lower; [atk-sel] zeroed / floorStood / powerUnable" },
            { "taunt.pull", "86e3pyjk2", "failed on the Deck 2026-10-13; needs a foe attacking an ally", "taunt pulls the foe off the ally" },
            { "substrate.ooc_timeouts", "1dfcuabbnb6", "needs real OOC casting over minutes", "no substrate ttl timeouts without a built cast (Sven, 10 casts)" },
            { "probe.m0_h3_g11_g7_h5miss", "class M0 / H3 / G11 / G7 / H5-probe", "passive probes that need an observed engine situation (sneak in combat, recentred combat area, charging aimed spell, reanimate corpse pick)", "each probe line present and plausible in the log; report to marth" },
            { "log.inactive_winner", "86e3t1gfn", "needs the exact inactive-winner situation", "Harbinger 'stands / YIELDS' line naming the credited source + priority" },
            { "iaus.scroll_probe", "1dfcuabcr8m", "needs a scroll and an observed AI choice", "does the AI use scrolls; can a claim drive one" },
            { "iaus.exploit_rows", "1dfcuabcr8k", "needs fire-weak and frost-resistant foes", "picks the element the foe is weak to, avoids the resisted one" },
            { "g10.will_work", "1dfcuabcqe7", "not automated yet (needs a dev counter on the board outcome)", "Calm/Fear/Frenzy above the level cap: 'will not work' on the board; at/below fires; Turn Undead living skips, undead fires" },
            { "auto.control_target", "1dfcuabcr0d", "not automated yet (needs a dev counter on the AUTO target)", "fear/paralyze row on a stronger foe casts on that foe, never an ally/self/player; one '[classify] ... Offense' per spell" },
            { "g9.cooldown", "1dfcuabcqe6", "needs a v5 save and a save/reload cycle", "[Debug] bClassCoSaveSelfTest PASS on a real v5 save; 10 s cooldown swings ~every 10 s; survives save + reload (schema v6)" },
            { "tailor.compat", "1dfcuabcrr7", "only when Tailor is installed; needs its UI", "T1 to T6 of the Tailor test list" },
            { "magicka.cost_capture", "86e3d6dp0", "needs the tier-C capture line first", "capture numbers match the research curve model" },
            { "freeze.batch_a", "86e3h87hb", "if it recurs, keep the game frozen and capture stacks", "capture or no recurrence" },
            { "play.batch8", "86e3ednub", "a normal play session", "no regressions" },
        };
            for (const auto& m : T2ManualItems()) v.push_back(m);   // TestsT2.cpp: the T2 field-session steps
            return v; }();
        return k;
    }

    void RegisterAll() {
        Register({ "b1.heal_left_offense_right", "MFO-B1",
                   "(self-setup) a caster loadout (no one-hander in the right hand); temp heal + offense rules and a spawned fight",
                   120.0f, B1Pre, B1Fix, B1Step, B1Timeout });
        RegisterDriven();   // TestsDrive.cpp: cost parity, B1a, buff claim road, class tests, H5, census
        Register({ "skooma.not_drunk_by_ai", "86e3t1gfq",
                   "(self-setup) Skooma, a cast rule, a spawned fight and a hurt follower; 2 minor healing potions are added if he has none (pressure = a pool "
                   "Skooma restores under 50%)",
                   180.0f, SkoomaPre, SkoomaFix, SkoomaStep, SkoomaTimeout });
        Register({ "bound_bow.fires", "86e3t1gfx", "(self-setup) the hit sink live; Bound Bow taught + cast by a temp rule in a spawned fight",
                   90.0f, BowPre, BowFix, BowStep, BowTimeout });
        Register({ "hms.cap_holds", "86e3d6dnv",
                   "out of combat, bHmsRedistribute + bHmsCapToPlayerTotal on, a captured HMS record with a class "
                   "profile and a player sample, the follower in the party for 2 minutes",
                   60.0f, HmsPre, nullptr, HmsStep, HmsTimeout });
        Register({ "hms.above_parity_gate", "86e3jw3b4 (e3d15c4)",
                   "out of combat, the same HMS record as hms.cap_holds; judged only when a player level-up happens in the window, else INFO with the standing numbers",
                   60.0f, HmsPre, nullptr, HmsGateStep, HmsGateTimeout });
        Register({ "prog.perk_entries_unique", "1dfcuabcrqq",
                   "a follower enrolled in Progression with at least one MFO-granted rank, loaded nearby, 30 s in "
                   "the party",
                   60.0f, PerkPre, nullptr, PerkStep, PerkTimeout });
        RegisterT2();   // TestsT2.cpp: the T2 field-session tests (quick, no spawn except t2.block_hands)
        RegisterBuffPotion();   // TestsBuffPotion.cpp: buffpotion.resist_scores (feat/buff-potions)
    }

}
