// THE SEEDED SELF-TESTS (dev builds only). One Register() call per test in RegisterAll(); how to add one is
// Docs/SELFTEST.md "Adding a test". Source list: the IN-GAME TEST LIST (ClickUp 1dfcuabcqdm).
//
// Every test here RIDES THE FOLLOWER'S OWN BOARD: it starts only when his combat table already holds the
// gambits it watches (the precondition says which, and the pending log prints it), and it judges what those
// gambits make the engine do. The runner never authors a rule. Reads only (#23): hands, inventory counts,
// actor values, the spell record classifiers MFO already has (Actuation::HealsHealth / ClassifyArchetype),
// and Gambit::lastFiredAt (the Scheduler's display stamp).
//
// IDS ARE FROZEN once a dev build ships them: they are the 'MFST' result keys (#10).

#include "selftest/SelfTest_internal.h"
#include "Config.h"
#include "Followers.h"
#include "Vocabulary.h"
#include "cast/Actuation.h"
#include "logistics/Logistics.h"

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
        const char* B1Pre(Ctx& c) {
            if (!c.inCombat) return "in combat";
            for (const auto& g : c.state->combat()) {
                if (!g.enabled || !IsCastOp(g.actionOpcode)) continue;
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm);
                if (!sp) continue;
                if (Actuation::HealsHealth(sp)) AddUnique(c.s->set[0], sp->GetFormID());
                else if (std::string_view(Actuation::ClassifyArchetype(sp).kindName) == "Offense")
                    AddUnique(c.s->set[1], sp->GetFormID());
            }
            if (c.s->set[0].empty()) return "an enabled combat rule casting a Health heal";
            if (c.s->set[1].empty()) return "an enabled combat rule casting an offensive spell";
            return nullptr;
        }
        void B1Fix(Ctx& c, FixtureRequest& r) { SpellsFixture(c, r, 0, 1); }
        Step B1Step(Ctx& c) {
            auto&            s  = *c.s;
            const RE::FormID L  = HandForm(c.follower, true);
            const RE::FormID R  = HandForm(c.follower, false);
            const bool       hl = Has(s.set[0], L), orr = Has(s.set[1], R), ol = Has(s.set[1], L);
            s.n[0] += 1;
            if (hl) s.n[1] += 1;
            if (orr) s.n[2] += 1;
            if (ol) s.n[3] += 1;
            s.note = std::format("samples {:.0f}: heal-left {:.0f}, offense-right {:.0f}, offense-LEFT {:.0f}; last "
                                 "L '{}' R '{}'",
                                 s.n[0], s.n[1], s.n[2], s.n[3], L ? FormName(L) : "empty", R ? FormName(R) : "empty");
            if (hl && orr)
                return Step::Pass(std::format("left '{}' + right '{}' held together; {}", FormName(L), FormName(R),
                                              s.note));
            if (!c.inCombat) return Step::Retry("combat ended before heal-left + offense-right were seen together; " + s.note);
            return Step::Continue();
        }
        Step B1Timeout(Ctx& c) {
            if (c.s->n[1] == 0)
                return Step::Retry("no heal reached the left hand in the window (nobody needed healing?); " + c.s->note);
            return Step::Fail("a heal reached the left hand but never together with the offense spell in the right; " +
                              c.s->note);
        }

        // ── 86e3t1gfq: Skooma is not drunk by the AI ────────────────────────────────────────────────
        // Test list: a follower with a cast gambit carrying Skooma + real potions in combat: no "CAST
        // Skooma ... AI-fired"; real potions still drunk. The direct evidence is the Skooma count: it must
        // never fall during the fight. A PASS needs DRINK PRESSURE (HP under 50% at some point, or a real
        // health potion drunk), otherwise the AI simply had no reason to drink and the run is a RETRY.
        // set[0] = { Skooma }, set[1] = { the minor healing potion, only when he had no health potion }.
        // n[0] start Skooma, n[1] start health potions, n[2] min HP fraction, n[3] max health potions drunk.
        const char* SkoomaPre(Ctx& c) {
            if (!c.inCombat) return "in combat";
            bool castRule = false;
            for (const auto& g : c.state->combat())
                if (g.enabled && IsCastOp(g.actionOpcode) && RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm))
                    castRule = true;
            if (!castRule) return "an enabled combat cast rule (the case the Harbinger allow-list covers)";
            const RE::FormID sk = VanillaForm(kSkoomaLocal);
            if (!sk || !RE::TESForm::LookupByID<RE::AlchemyItem>(sk)) return "Skyrim.esm Skooma (00057A7A) resolvable";
            c.s->set[0] = { sk };
            if (Logistics::CountPotions(c.follower, RE::ActorValue::kHealth) < 1)
                if (const RE::FormID hp = VanillaForm(kMinorHealPotionLocal); hp) c.s->set[1] = { hp };
            return nullptr;
        }
        void SkoomaFix(Ctx& c, FixtureRequest& r) {
            r.items.push_back({ c.s->set[0][0], 1 });
            if (!c.s->set[1].empty()) r.items.push_back({ c.s->set[1][0], 2 });
        }
        std::string SkoomaNote(const Ctx& c, std::int32_t a_sk) {
            const auto& s   = *c.s;
            auto*       alc = RE::TESForm::LookupByID<RE::AlchemyItem>(s.set[0][0]);
            return std::format("Skooma {:.0f} -> {}, HP min {:.0f}%, real health potions drunk {:.0f}, MFO PotionIsDrug "
                               "= {}",
                               s.n[0], a_sk, s.n[2] * 100.0, s.n[3], Logistics::PotionIsDrug(alc) ? "yes" : "no");
        }
        Step SkoomaStep(Ctx& c) {
            auto& s = *c.s;
            const std::int32_t sk = CountItem(c.follower, s.set[0][0]);
            const int          hp = Logistics::CountPotions(c.follower, RE::ActorValue::kHealth);
            if (!s.b[0]) {
                s.b[0] = true;
                s.n[0] = sk;
                s.n[1] = hp;
                s.n[2] = 1.0;
            }
            s.n[2] = std::min<double>(s.n[2], Vocab::HealthPct(c.follower));
            s.n[3] = std::max<double>(s.n[3], s.n[1] - hp);
            s.note = SkoomaNote(c, sk);
            if (sk < s.n[0]) return Step::Fail("the Skooma count FELL during combat (the AI drank it): " + s.note);
            const bool pressure = s.n[2] < 0.5 || s.n[3] > 0;
            if (!c.inCombat) {
                if (c.elapsed >= 20.0f && pressure) return Step::Pass("Skooma kept through the fight: " + s.note);
                return Step::Retry(std::format("combat ended after {:.0f}s without drink pressure (needs >= 20 s and HP "
                                               "under 50% or a potion drunk): {}",
                                               c.elapsed, s.note));
            }
            return Step::Continue();
        }
        Step SkoomaTimeout(Ctx& c) {
            const auto& s = *c.s;
            if (s.n[2] < 0.5 || s.n[3] > 0) return Step::Pass("Skooma kept through the whole window: " + s.note);
            return Step::Retry("no drink pressure in the window: " + s.note);
        }

        // ── 86e3t1gfx: bound bow cast + fired ───────────────────────────────────────────────────────
        // Test list: "bound-bow gambit in combat ... bow in hand and firing". set[0] = the bound-bow spells
        // of his combat rules. b[0] = a bound-bow rule FIRED since the start (Gambit::lastFiredAt), b[1] =
        // the bow reached a hand; n[0] = when, n[1] = ammo form, n[2] = its count then.
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
            if (!c.inCombat) return "in combat";
            for (const auto& g : c.state->combat()) {
                if (!g.enabled || !IsCastOp(g.actionOpcode)) continue;
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(g.actionParamForm);
                if (SummonsBow(sp)) AddUnique(c.s->set[0], sp->GetFormID());
            }
            return c.s->set[0].empty() ? "an enabled combat rule casting a bound-BOW spell" : nullptr;
        }
        void BowFix(Ctx& c, FixtureRequest& r) { SpellsFixture(c, r, 0, -1); }
        RE::TESObjectWEAP* BoundBowInHand(RE::Actor* a) {
            for (const bool left : { false, true }) {
                auto* f = a->GetEquippedObject(left);
                auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
                if (w && w->IsBound() && w->IsBow()) return w;
            }
            return nullptr;
        }
        Step BowStep(Ctx& c) {
            auto& s = *c.s;
            for (const auto& g : c.state->combat())
                if (g.enabled && Has(s.set[0], g.actionParamForm) && g.lastFiredAt >= c.startedAt) s.b[0] = true;
            auto*      bow  = BoundBowInHand(c.follower);
            auto*      ammo = c.follower->GetCurrentAmmo();
            const auto aid  = ammo ? ammo->GetFormID() : 0;
            if (bow && !s.b[1]) {
                s.b[1] = true;
                s.n[0] = c.elapsed;
                s.n[1] = aid;
                s.n[2] = aid ? CountItem(c.follower, aid) : -1;
            }
            if (s.b[1] && aid && static_cast<RE::FormID>(s.n[1]) != aid) {   // ammo (re)equipped after the bow
                s.n[1] = aid;
                s.n[2] = CountItem(c.follower, aid);
            }
            const std::int32_t cnt = aid ? CountItem(c.follower, aid) : -1;
            s.note = std::format("rule fired {}, bound bow in hand {}{}, ammo '{}' count {}", s.b[0] ? "yes" : "no",
                                 s.b[1] ? "yes" : "no", s.b[1] ? std::format(" (since t={:.1f}s)", s.n[0]) : "",
                                 aid ? FormName(aid) : "none", cnt);
            if (s.b[1] && aid && cnt >= 0 && cnt < s.n[2])
                return Step::Pass(std::format("bound bow '{}' fired: {} -> {} '{}'; {}", bow ? FormName(bow->GetFormID()) : "?",
                                              s.n[2], cnt, FormName(aid), s.note));
            if (s.b[1] && c.elapsed - s.n[0] > 30.0f)
                return Step::Fail("a bound bow was in hand for 30 s but no arrow left the quiver; " + s.note);
            if (!c.inCombat) return Step::Retry("combat ended first; " + s.note);
            return Step::Continue();
        }
        Step BowTimeout(Ctx& c) {
            if (!c.s->b[0]) return Step::Retry("the bound-bow rule never fired (its WHEN never held?); " + c.s->note);
            return Step::Fail("the bound-bow rule fired but no bound bow reached a hand; " + c.s->note);
        }

        // ── 86e3d6dnv: HMS adopted-total cap ────────────────────────────────────────────────────────
        // Test list: "caps fire, follower totals track the player's". With bHmsCapToPlayerTotal on, a
        // follower's BASE Health+Magicka+Stamina must not sit above the player's (progression/Hms.cpp, the
        // "adopted total capped" trim; the same Followers::GetFollowerHMS read the cap's player sample
        // uses). Judged out of combat after the reading has held still for 5 s. n[0] / n[1] = last
        // follower / player totals, n[2] = when they last changed.
        float Hms(RE::Actor* a, float (&a_pool)[3]) {
            float t = 0.0f;
            for (int p = 0; p < 3; ++p) t += (a_pool[p] = Followers::GetFollowerHMS(a, p));
            return t;
        }
        const char* HmsPre(Ctx& c) {
            if (c.inCombat) return "out of combat";
            if (!Config::g_hmsCapToPlayerTotal.load()) return "bHmsCapToPlayerTotal = 1 (the cap is off)";
            if (c.seenFor < 120.0f) return "the follower in the party for 2 minutes this session (the HMS poll ran)";
            if (c.follower->IsCommandedActor()) return "a real follower, not a summon";
            return nullptr;
        }
        Step HmsStep(Ctx& c) {
            auto& s = *c.s;
            if (c.inCombat) return Step::Retry("combat started before the reading settled");
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) return Step::Continue();
            float      fp[3], pp[3];
            const float ft = Hms(c.follower, fp), pt = Hms(pc, pp);
            if (!s.b[0] || ft != s.n[0] || pt != s.n[1]) {
                s.b[0] = true;
                s.n[0] = ft;
                s.n[1] = pt;
                s.n[2] = c.elapsed;
            }
            s.note = std::format("follower H/M/S {:.0f}/{:.0f}/{:.0f} = {:.0f}, player {:.0f}/{:.0f}/{:.0f} = {:.0f}", fp[0],
                                 fp[1], fp[2], ft, pp[0], pp[1], pp[2], pt);
            if (c.elapsed - s.n[2] < 5.0f) return Step::Continue();
            if (pt <= 0.0f) return Step::Retry("the player's base H/M/S read 0; " + s.note);
            if (ft <= pt + 1.0f) return Step::Pass("follower total at or under the player's: " + s.note);
            return Step::Fail(std::format("follower total {:.0f} ABOVE the player's by {:.0f}: {} (the trim stops at the "
                                          "per-pool floors max(class share x cap, 50): read this follower's '[hms] ... "
                                          "cap detail' line)",
                                          ft, ft - pt, s.note));
        }

    }

    void RegisterAll() {
        Register({ "b1.heal_left_offense_right", "MFO-B1",
                   "in combat + combat rules casting a Health heal AND an offensive spell",
                   120.0f, B1Pre, B1Fix, B1Step, B1Timeout });
        Register({ "skooma.not_drunk_by_ai", "86e3t1gfq",
                   "in combat + an enabled combat cast rule (Skooma, and 2 minor healing potions if he has no "
                   "health potion, are added for the run)",
                   180.0f, SkoomaPre, SkoomaFix, SkoomaStep, SkoomaTimeout });
        Register({ "bound_bow.fires", "86e3t1gfx", "in combat + an enabled combat rule casting a bound-BOW spell",
                   90.0f, BowPre, BowFix, BowStep, BowTimeout });
        Register({ "hms.cap_holds", "86e3d6dnv",
                   "out of combat, bHmsCapToPlayerTotal on, the follower in the party for 2 minutes",
                   60.0f, HmsPre, nullptr, HmsStep, nullptr });
    }

}
