// THE T2 SELF-TESTS (dev builds only; Docs/SELFTEST.md section 4). marth 2026-11-12: "make sure the dev test probe is loaded and ready"
// before the T2 field session. One test per step of Docs/research/2026-11/in-game-test-list.md (T2 Part 3b/3c/3d, Movement batch, Theft
// gate, Batch L, T2 batch I) that the DLL can judge WITHOUT a human: a Harbinger road served (a read or a CanServe dry run), a claim filed
// and read back, a gate verdict, a pure rule's output. What needs an eye or a real fight is listed in T2ManualItems() (one MANUAL line
// per step group, counted in the ALL DONE line) and is never run.
//
// Same rules as TestsDrive.cpp: reads only inside step() except the class writers a test calls on purpose (snapshotted + restored by the
// runner); engine reads that must be on the main thread are posted and cached under a leaf lock. IDS ARE FROZEN once a dev build ships them.

#include "selftest/SelfTest_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "MainThread.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
#include "classes/Classes.h"
#include "iaus/Reposition.h"
#include "iaus/SpellBook.h"
#include "logistics/Steal.h"
#include "logistics/TheftGate.h"

#include <cmath>

namespace MFO::SelfTest {

    namespace {

        constexpr RE::FormID kHealingLocal = 0x00012FCC;   // SPEL Healing (Skyrim.esm, verified 2026-11-10, TestsDrive.cpp)

        const char* AlwaysPre(Ctx&) { return nullptr; }
        const char* ClassPre() {
            if (!Classes::Active()) return "the class add-on live (MFO_Classes.esl + bClassTree = 1)";
            if (!Classes::Writable()) return "MCLS writable (the loaded record was newer than this DLL: read-only)";
            return nullptr;
        }
        void ClassSnapFix(Ctx&, FixtureRequest& r) { r.cls.snapshot = true; }
        bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

        // ── t2.harbinger_seats ──────────────────────────────────────────────────────────────────────────
        // Test list 24a (ch.32), 24g (ch.8b reanimate), 24m/24p (ch.31 block), 24q (ch.31 power attack), 24t (ch.33 style), Movement batch 2
        // (ch.24 give ground / disengage), T2 batch I 1 (ch.24 range close). The "seat installed" lines are in APMF.log (MFO cannot read it,
        // t2.manual_seat_lines lists them); what MFO CAN ask is what its own roads ask: the capability gate each road uses, and for the three
        // intents that have a CanServe dry run the answer "is this intent's seat / channel installed on this game". PASS = every road served.
        // INFO = some road is not (an older Harbinger, or a deliberate [..] INI switch): the evidence names each and why. It never FAILs:
        // a switch the tester set in APMF.ini is a legitimate "not served" (24l, 24p, 24u steps).
        bool Installed(const APMFBridge::DevServeInfo& i) {
            if (!i.api || i.verdict == APMF_API::kServe_Failed) return false;
            if (i.verdict == APMF_API::kServe_Refused &&
                (i.refusal == APMF_API::kServeRefusal_NoDrain || i.refusal == APMF_API::kServeRefusal_NoChannel ||
                 i.refusal == APMF_API::kServeRefusal_NotInstalled))
                return false;
            return true;
        }
        Step SeatsStep(Ctx& c) {
            const std::uint32_t abi = APMFBridge::DevAbi();
            if (!abi) return Step::Info("Harbinger (APMF) is absent: no T2 road is served, MFO runs its documented legacy degrade for each (nothing to judge)");
            std::string ok, bad;
            const auto add = [&](const char* a_name, std::uint32_t a_need, bool a_served, const std::string& a_note) {
                if (abi < a_need) { bad += std::format(" [{}: Harbinger ABI {} is older than {}]", a_name, abi, a_need); return; }
                if (!a_served) { bad += std::format(" [{}: NOT served{}]", a_name, a_note.empty() ? "" : " (" + a_note + ")"); return; }
                ok += std::format(" {}", a_name);
            };
            add("ch.24-range-close(H4)", 19, APMFBridge::RangeCloseSupported(), {});
            add("ch.24-ground-step(give-ground)", 39, APMFBridge::GroundStepSupported(), {});
            add("ch.24-disengage(H3)", 39, APMFBridge::DisengageSupported(), {});
            add("ch.31-combat-move", 38, APMFBridge::CombatMoveSupported(), {});
            {
                const char* why = APMFBridge::CombatMoveUnservedReason(c.id, APMF_API::kCombatMove_Block);
                add("ch.31-block", 43, APMFBridge::BlockSupported() && !why, why ? why : "");
            }
            {
                const char* why = APMFBridge::CombatMoveUnservedReason(c.id, APMF_API::kCombatMove_PowerAttack);
                add("ch.31-power-attack", 44, APMFBridge::PowerAttackRoadSupported() && !why, why ? why : "");
            }
            {
                const auto d = APMFBridge::DevCanServeIntent(c.id, APMF_API::kIntent_Aversion, 0.25f);
                add("ch.32-aversion", 41, Installed(d), std::format("CanServe verdict {} refusal {} '{}'", d.verdict, d.refusal, d.why));
            }
            add("ch.8b-reanimate-corpse(G7)", 42, APMFBridge::CorpseTargetSupported(), {});
            {
                // ch.33 REQUIRES a TESCombatStyle form (APMF_API.h kIntent_CombatStyle): ask with the follower's own base combat style, so a
                // BadRequest cannot masquerade as "installed". No style on his base = not judged (INFO), never a pass.
                auto*              actor = RE::TESForm::LookupByID<RE::Actor>(c.id);
                auto*              base  = actor ? actor->GetActorBase() : nullptr;
                auto*              csty  = base ? base->GetCombatStyle() : nullptr;
                const RE::FormID   sform = csty ? csty->GetFormID() : 0;
                const auto d = APMFBridge::DevCanServeIntent(c.id, APMF_API::kIntent_CombatStyle, 0.0f, sform);
                const bool judged = sform != 0 && d.verdict != APMF_API::kServe_Failed &&
                                    !(d.verdict == APMF_API::kServe_Refused && d.refusal == APMF_API::kServeRefusal_BadRequest);
                add("ch.33-combat-style", 45, APMFBridge::StyleRoadSupported() && judged && Installed(d),
                    sform == 0 ? std::string("not judged: his base NPC has no combat style to ask with")
                    : !judged  ? std::format("not judged: CanServe verdict {} refusal {} '{}'", d.verdict, d.refusal, d.why)
                               : std::format("CanServe verdict {} refusal {} '{}'", d.verdict, d.refusal, d.why));
            }
            if (bad.empty()) return Step::Pass(std::format("Harbinger ABI {} serves every T2 road:{}", abi, ok));
            return Step::Info(std::format("Harbinger ABI {}: served:{} | NOT served:{} (an older Harbinger or an APMF.ini switch off: the documented degrade; the "
                                          "T2 steps for that road cannot run)", abi, ok.empty() ? " none" : ok, bad));
        }

        // ── t2.aversion_claim ───────────────────────────────────────────────────────────────────────────
        // Test list 24a (an equipped Conjurer / Healer files its claim at weight 0.25) and 24f / Q94 (a mastered, UNEQUIPPED Conjurer files none).
        // The rank rule (classes/Carriers.cpp CarrierRanks, pure) is checked first; then the class is applied through the C5c writers and the
        // sweep's claim is read back (DevReadAversion). n[0] phase, n[1] elapsed when the phase's class was applied, b[0] applied, b[7] pure done.
        //   phase 0: Conjurer L1 equipped        -> claim weight = fAversionConjurer
        //   phase 1: Conjurer mastered, Evoker L1 equipped -> NO claim (released)
        //   phase 2: Healer L1 equipped          -> claim weight = fAversionHealer (filed again after the release)
        const char* AversionPre(Ctx&) {
            if (const char* r = ClassPre()) return r;
            if (APMFBridge::DevAbi() < 41) return "Harbinger ABI 41+ (ch.32 aversion)";
            if (!Config::g_classAversion.load()) return "bClassAversion = 1";
            return nullptr;
        }
        float ExpectedWeight(float a_cfg) { return std::clamp(a_cfg, 0.05f, 0.95f); }
        Step AversionStep(Ctx& c) {
            auto& s = *c.s;
            using Classes::Id;
            if (!s.b[7]) {
                s.b[7] = true;
                Classes::FollowerClasses a;
                a.equipped = static_cast<std::uint8_t>(Id::Conjurer);
                a.progress[static_cast<std::size_t>(Id::Conjurer)].level = 1;
                const auto ra = Classes::CarrierRanks(a);
                Classes::FollowerClasses b;
                b.equipped = static_cast<std::uint8_t>(Id::Evoker);
                b.progress[static_cast<std::size_t>(Id::Evoker)].level = 1;
                b.mastered = Classes::Bit(Id::Conjurer);
                const auto rb = Classes::CarrierRanks(b);
                Classes::FollowerClasses h;
                h.equipped = static_cast<std::uint8_t>(Id::Healer);
                h.progress[static_cast<std::size_t>(Id::Healer)].level = 1;
                const auto rh = Classes::CarrierRanks(h);
                const auto conj = static_cast<std::size_t>(Id::Conjurer), heal = static_cast<std::size_t>(Id::Healer);
                if (ra[conj] == 0 || ra[conj] == Classes::kRankMasteredUnequipped)
                    return Step::Fail(std::format("CarrierRanks: an equipped Conjurer L1 must carry an ACTIVE passive rank, read {}", ra[conj]));
                if (rb[conj] != Classes::kRankMasteredUnequipped)
                    return Step::Fail(std::format("CarrierRanks: a mastered, unequipped Conjurer must read rank {} (the 'none' marker AversionClaims drops), read {}",
                                                  Classes::kRankMasteredUnequipped, rb[conj]));
                if (rh[heal] == 0 || rh[heal] == Classes::kRankMasteredUnequipped)
                    return Step::Fail(std::format("CarrierRanks: an equipped Healer L1 must carry an ACTIVE passive rank, read {}", rh[heal]));
                s.note = std::format("rank rule OK (equipped Conjurer {}, mastered-unequipped Conjurer {}, equipped Healer {}); ", ra[conj], rb[conj], rh[heal]);
            }
            const int phase = static_cast<int>(s.n[0]);
            if (phase >= 3)
                return Step::Pass("equipped Conjurer filed a claim at the INI weight, a mastered-unequipped one filed none (and the claim was released), an equipped Healer filed again; " + s.note);
            if (!s.b[0]) {
                std::string why;
                bool ok = false;
                if (phase == 0) { Classes::SetMastered(c.id, 0); ok = ApplyClass(c.id, "conjurer", 1, false, why); }
                else if (phase == 1) { Classes::SetMastered(c.id, Classes::Bit(Id::Conjurer)); ok = ApplyClass(c.id, "evoker", 1, false, why); }
                else { Classes::SetMastered(c.id, 0); ok = ApplyClass(c.id, "healer", 1, false, why); }
                if (!ok) return Step::Retry(std::format("phase {} could not apply its class: {}", phase, why));
                s.b[0] = true;
                s.n[1] = c.elapsed;
                return Step::Continue();
            }
            const auto d = APMFBridge::DevReadAversion(c.id);
            if (d.seatRefused) return Step::Info("Harbinger REFUSED the ch.32 claim at the call (its seats are not installed on this game, or [Aversion] bAversion = 0): see APMF.log; " + s.note);
            if (!d.api) return Step::Retry("Harbinger below ABI 41 / absent");
            const float want = phase == 1 ? 0.0f : ExpectedWeight(phase == 0 ? Config::g_aversionConjurer.load() : Config::g_aversionHealer.load());
            const bool  met  = phase == 1 ? !(d.weight > 0.0f) : (Near(d.weight, want) && d.live);
            if (met) {
                s.note += std::format("phase {} OK (weight {:.2f}, live {}); ", phase, d.weight, d.live);
                s.n[0] += 1;
                s.b[0] = false;
                return Step::Continue();
            }
            if (c.elapsed - static_cast<float>(s.n[1]) < 8.0f) return Step::Continue();   // the sweep runs every 133 ms; a claim goes live within a few
            return Step::Fail(std::format("phase {} ({}): after 8 s the standing claim reads weight {:.2f} (live {}), wanted {:.2f}{}; {}", phase,
                                          phase == 0 ? "Conjurer L1 equipped" : phase == 1 ? "Conjurer mastered, Evoker equipped: no claim" : "Healer L1 equipped",
                                          d.weight, d.live, want, phase == 1 ? " (none)" : " and live", s.note));
        }

        // ── t2.block_hands ──────────────────────────────────────────────────────────────────────────────
        // Test list 24m / 24n: MFO's own pre-gate APMFBridge::BlockHandsReason on the hands the follower really holds. The runner never equips,
        // so one spawned fight draws whatever he carries and the verdict is compared with an INDEPENDENT restatement of the 24m/24n rule
        // (shield or torch in the left = ok; a different thing in the left = refused; one-hander in each hand = dual wield refused; two-hander
        // or one-hander alone = ok; bow, crossbow, staff, fists = refused). The evidence names the loadout the run could judge.
        void BlockFix(Ctx&, FixtureRequest& r) {
            r.spawn.on = true;
            r.spawn.hurtPct = 0.0f;
        }
        int ExpectedBlock(RE::Actor* a) {   // 0 ok, 2 left hand other, 3 no shield/weapon, 4 dual wield, 5 unsuitable weapon type
            auto* left  = a->GetEquippedObject(true);
            auto* right = a->GetEquippedObject(false);
            if (left) {
                if (const auto* armo = left->As<RE::TESObjectARMO>(); armo && armo->IsShield()) return 0;
                if (left->Is(RE::FormType::Light)) return 0;
                if (left != right) return 2;
            }
            const auto* weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
            if (!weap) return 3;
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword: case RE::WEAPON_TYPE::kOneHandDagger:
            case RE::WEAPON_TYPE::kOneHandAxe:   case RE::WEAPON_TYPE::kOneHandMace:
                return left ? 4 : 0;
            case RE::WEAPON_TYPE::kTwoHandSword: case RE::WEAPON_TYPE::kTwoHandAxe: return 0;
            default: return 5;
            }
        }
        int ActualBlock(const char* r) {
            if (!r) return 0;
            const std::string t = r;
            if (t.find("left hand holds") != std::string::npos) return 2;
            if (t.find("no shield and no melee") != std::string::npos) return 3;
            if (t.find("same one-handed") != std::string::npos) return 4;
            if (t.find("bow, a crossbow") != std::string::npos) return 5;
            if (t.find("not drawn") != std::string::npos) return 1;
            return -1;
        }
        Step BlockStep(Ctx& c) {
            if (!c.follower) return Step::Retry("no follower");
            if (!c.follower->AsActorState()->IsWeaponDrawn()) {
                if (!c.inCombat) return Step::Retry("his weapon was never drawn in the window (no fight)");
                return Step::Continue();
            }
            const char* why = APMFBridge::BlockHandsReason(c.follower);
            const int   got = ActualBlock(why), want = ExpectedBlock(c.follower);
            const auto* l = c.follower->GetEquippedObject(true);
            const auto* r = c.follower->GetEquippedObject(false);
            const std::string hands = std::format("left '{}' right '{}'", l ? FormName(l->GetFormID()) : "empty", r ? FormName(r->GetFormID()) : "empty");
            if (got == want)
                return Step::Pass(std::format("{}: BlockHandsReason = {} (matches the 24m/24n rule class {}); only this loadout can be judged per run (swap his weapons and "
                                              "reset 't2.block_hands' to judge another)", hands, why ? std::string("'") + why + "'" : std::string("allowed"), want));
            return Step::Fail(std::format("{}: the rule says class {} ({}) but BlockHandsReason returned {} ('{}')", hands, want,
                                          want == 0 ? "allowed" : "refused", got, why ? why : "allowed"));
        }

        // ── main-thread bundles (cached under a leaf lock) ──────────────────────────────────────────────
        struct MainRes { bool done = false; bool fail = false; bool info = false; std::string text; };
        std::mutex                                     g_t2Mx;   // leaf
        std::unordered_map<std::string, MainRes>       g_t2Res;
        void SetRes(const std::string& a_key, MainRes a_r) {
            std::lock_guard lk(g_t2Mx);
            g_t2Res[a_key] = std::move(a_r);
        }
        bool GetRes(const std::string& a_key, MainRes& a_out) {
            std::lock_guard lk(g_t2Mx);
            const auto it = g_t2Res.find(a_key);
            if (it == g_t2Res.end() || !it->second.done) return false;
            a_out = it->second;
            return true;
        }

        // ── t2.theft_gate ───────────────────────────────────────────────────────────────────────────────
        // Test list "Theft gate" 1-4 and Batch L 4 (the verdicts; the town walk and the bounty are the human's: t2.manual_theft_field).
        //   a. TheftGate::TakeBarred of a plain copy in the PLAYER'S pack = may be taken (no owner on the player ref).
        //   b. TakeBarred on up to 3 owned references of the follower's cell (the container-owner branch): barred exactly when the owner is not
        //      the player / PlayerFaction / the follower's own base chain. None nearby = noted, not a failure (an open field has none).
        //   c. TheftGate::GuardCheck on the main thread never answers Pending (it scans now); the worker's read of the same pair right after
        //      is Clear / Seen / Pending (a stale answer re-asks: the evidence says which, it is not judged).
        //   d. Steal::CheckNow while he is NOT sneaking = Refused "the follower is not sneaking" (card G13). A sneak-always follower (Thief line) or one with no 3D
        //      cannot be judged: the test then ends INFO with a-c's results. NOTE: CheckNow logs a '[steal] F=.. item=00000007' line (the player ref is the stand-in item) - MFO-B836.
        Step TheftStep(Ctx& c) {
            const std::string key = std::format("theft:{:08X}", c.id);
            auto&             s   = *c.s;
            if (!s.b[0]) {
                s.b[0] = true;
                SetRes(key, MainRes{});
                const RE::FormID fid = c.id;
                MainThread::Post([fid, key]() {
                    MainRes r;
                    auto*   f  = RE::TESForm::LookupByID<RE::Actor>(fid);
                    auto*   pc = RE::PlayerCharacter::GetSingleton();
                    if (!f || !pc) { r.done = true; r.fail = true; r.text = "unresolved follower / player on the main thread"; SetRes(key, r); return; }
                    using namespace Logistics;
                    // a. a plain copy in the player's pack
                    const char* a = TheftGate::TakeBarred(f, pc, nullptr, nullptr, TheftGate::Road::Container);
                    r.text += std::format("a: player-pack copy -> {}; ", a ? a : "takeable");
                    if (a) { r.fail = true; r.text += "(WRONG: a plain copy in the player's own pack must be takeable); "; }
                    // b. owned references of the cell
                    int owned = 0, checked = 0, visited = 0;
                    if (auto* cell = f->GetParentCell()) {
                        cell->ForEachReference([&](RE::TESObjectREFR* ref) {
                            if (++visited > 6000) return RE::BSContainer::ForEachResult::kStop;
                            if (!ref || ref == f || ref == pc) return RE::BSContainer::ForEachResult::kContinue;
                            auto* own = ref->GetOwner();
                            if (!own) return RE::BSContainer::ForEachResult::kContinue;
                            ++owned;
                            bool mine = own->GetFormID() == 0x7 || own->GetFormID() == 0xDB1;
                            RE::TESForm* b = f->GetActorBase();
                            for (int i = 0; !mine && b && i < 8; ++i) {
                                if (b == own) mine = true;
                                auto* npc = b->As<RE::TESNPC>();
                                b = npc ? npc->baseTemplateForm : nullptr;
                            }
                            const char* v = TheftGate::TakeBarred(f, ref, nullptr, nullptr, TheftGate::Road::Container);
                            ++checked;
                            if ((v != nullptr) == mine) {   // barred but it is his own side's, or free but someone else's
                                r.fail = true;
                                r.text += std::format("(WRONG: ref {:08X} owner {:08X} {}: verdict {}); ", ref->GetFormID(), own->GetFormID(),
                                                      mine ? "is his own side's" : "is someone else's", v ? v : "takeable");
                            }
                            return checked >= 3 ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
                        });
                    }
                    r.text += checked ? std::format("b: {} owned reference(s) judged against the owner rule; ", checked)
                                      : std::format("b: no owned reference among {} in his cell (open ground): the container-owner branch not exercised here; ", visited);
                    // c. the guard gate answers now on the main thread
                    const auto g = TheftGate::GuardCheck(f, nullptr, TheftGate::Road::Loose, /*a_onMain*/ true);
                    r.text += std::format("c: GuardCheck(main) = {}; ", g == TheftGate::GuardState::Clear ? "Clear" : g == TheftGate::GuardState::Seen ? "Seen" : "PENDING");
                    if (g == TheftGate::GuardState::Pending) { r.fail = true; r.text += "(WRONG: a main-thread scan must never answer Pending); "; }
                    // d. Steal: not sneaking -> refused
                    if (f->IsSneaking()) {
                        r.info = true;
                        r.text += "d: NOT judged, he is sneaking (a sneak-always class such as the Thief, or a Sneak row): the not-sneaking refusal needs a standing follower (t2.manual_steal step 4); ";
                    } else {
                        std::string why;
                        const auto  v = Steal::CheckNow(f, pc, TheftGate::Road::Steal, &why);
                        r.text += std::format("d: Steal::CheckNow while not sneaking -> {} '{}'; ", v == Steal::Verdict::Refused ? "Refused" : v == Steal::Verdict::Take ? "TAKE" : "Pending", why);
                        if (why.find("not loaded") != std::string::npos) {   // Evaluate refuses 'not loaded' before 'not sneaking': no 3D, nothing to judge
                            r.info = true;
                            r.text += "(d NOT judged: he has no loaded 3D, Evaluate refuses that before the sneak test); ";
                        } else if (v != Steal::Verdict::Refused || why.find("not sneaking") == std::string::npos) {
                            r.fail = true;
                            r.text += "(WRONG: must be Refused 'the follower is not sneaking'); ";
                        }
                    }
                    r.done = true;
                    SetRes(key, r);
                });
                return Step::Continue();
            }
            MainRes r;
            if (!GetRes(key, r)) return Step::Continue();
            // the worker's read of the same pair (fail-closed Pending is a valid answer, noted)
            const auto w = Logistics::TheftGate::GuardCheck(c.follower, nullptr, Logistics::TheftGate::Road::Loose, false);
            r.text += std::format("worker GuardCheck = {}", w == Logistics::TheftGate::GuardState::Clear ? "Clear" : w == Logistics::TheftGate::GuardState::Seen ? "Seen" : "Pending (stale: re-asked, fail closed)");
            if (r.fail) return Step::Fail(r.text);
            if (r.info) return Step::Info(r.text);   // a-c judged above; d could not be (sneak-always class, or no 3D)
            return Step::Pass(r.text);
        }
        Step TheftTimeout(Ctx& c) { return Step::Retry("the main-thread bundle did not answer in the window; " + c.s->note); }

        // ── t2.spell_weighing ───────────────────────────────────────────────────────────────────────────
        // Test list batch I 7 / 8: a spell whose effects are script-only is NEVER weighed (so IAUS never casts it and the Teach Spells page never lists
        // it). Over EVERY SpellItem on the list (main thread, once): every kSpell whose effects are all Script / SpawnScriptedRef must be
        // WouldWeigh == false; the control, Healing, must be true. INFO when this list has no script-only spell.
        Step WeighStep(Ctx& c) {
            const std::string key = "weigh";
            auto&             s   = *c.s;
            if (!s.b[0]) {
                s.b[0] = true;
                SetRes(key, MainRes{});
                const RE::FormID heal = VanillaForm(kHealingLocal);
                MainThread::Post([key, heal]() {
                    MainRes r;
                    auto*   dh = RE::TESDataHandler::GetSingleton();
                    if (!dh) { r.done = true; r.fail = true; r.text = "no data handler"; SetRes(key, r); return; }
                    int total = 0, scriptOnly = 0, wrong = 0;
                    std::string names;
                    for (auto* sp : dh->GetFormArray<RE::SpellItem>()) {
                        if (!sp || sp->data.spellType != RE::MagicSystem::SpellType::kSpell || sp->effects.empty()) continue;
                        ++total;
                        bool all = true;
                        for (auto* eff : sp->effects) {
                            auto* base = eff ? eff->baseEffect : nullptr;
                            const int a = base ? static_cast<int>(base->GetArchetype()) : -1;
                            if (!(a == 1 || a == 43)) { all = false; break; }
                        }
                        if (!all) continue;
                        ++scriptOnly;
                        const char* why = nullptr;
                        if (Iaus::SpellBook::WouldWeigh(sp, Iaus::ClassGate{}, &why)) {
                            ++wrong;
                            if (wrong <= 3) names += std::format(" {:08X} '{}';", sp->GetFormID(), sp->GetName() ? sp->GetName() : "?");
                        }
                    }
                    r.text = std::format("{} castable spell record(s) scanned, {} are script-only, {} of those WOULD be weighed{}", total, scriptOnly, wrong, wrong ? names : std::string());
                    r.fail = wrong > 0;
                    auto* hs = heal ? RE::TESForm::LookupByID<RE::SpellItem>(heal) : nullptr;
                    const char* hwhy = nullptr;
                    const bool  hw = hs && Iaus::SpellBook::WouldWeigh(hs, Iaus::ClassGate{}, &hwhy);
                    r.text += std::format("; control Healing weighs: {}{}", hw ? "yes" : "NO", hw ? "" : std::format(" ({})", hwhy ? hwhy : "not resolvable"));
                    if (!hw) r.fail = true;
                    if (scriptOnly == 0) r.text += "; no script-only spell on this list: the rule is vacuous here (batch I 7 needs a list with script-only spells)";
                    r.done = true;
                    SetRes(key, r);
                });
                return Step::Continue();
            }
            MainRes r;
            if (!GetRes(key, r)) return Step::Continue();
            if (r.fail) return Step::Fail(r.text);
            if (r.text.find("vacuous") != std::string::npos) return Step::Info(r.text);
            return Step::Pass(r.text);
        }
        Step WeighTimeout(Ctx&) { return Step::Retry("the main-thread spell scan did not answer in the window"); }

        // ── t2.reposition_rules ─────────────────────────────────────────────────────────────────────────
        // Test list batch I 1-3 (H4 spell range, mage back-off, Mercenary back-off): iaus/Reposition.h's pure rules on fixture spells and foes.
        Step RepositionStep(Ctx&) {
            namespace R = Iaus::Reposition;
            std::string bad;
            const auto expect = [&](bool a_ok, const std::string& a_what) { if (!a_ok) bad += " [" + a_what + "]"; };
            const auto spell = [](float reach, float area, bool self, bool touch, bool conc) {
                Iaus::SpellEntry e;
                e.form = 0x1234; e.reach = reach; e.areaUnits = area; e.selfDelivery = self; e.touch = touch; e.concentration = conc;
                e.nature = Iaus::Prune::Nature::Offense;
                return e;
            };
            {   // SpellRange
                const auto t = R::SpellRange(spell(0, 0, false, true, false), 150.0f);
                expect(Near(t.desired, 120.0f) && t.minimum == 0.0f, std::format("touch: want 120/0 (0.8 x melee reach 150), got {:.1f}/{:.1f}", t.desired, t.minimum));
                const auto k = R::SpellRange(spell(1000, 0, false, false, true), 150.0f);
                expect(Near(k.desired, 800.0f) && k.minimum == 0.0f, std::format("concentration: want 800/0 (0.8 x 1000), got {:.1f}/{:.1f}", k.desired, k.minimum));
                const auto a = R::SpellRange(spell(2000, 0, false, false, false), 150.0f);
                expect(Near(a.desired, 1800.0f) && Near(a.minimum, 720.0f), std::format("aimed: want 1800/720 (0.9 x 2000, min 0.4), got {:.1f}/{:.1f}", a.desired, a.minimum));
                const auto sa = R::SpellRange(spell(0, 400, true, false, false), 150.0f);
                expect(Near(sa.desired, 320.0f), std::format("self-delivered blast: want 320 (0.8 x area 400), got {:.1f}", sa.desired));
                expect(!R::SpellRange(spell(0, 0, true, false, false), 150.0f).Known(), "self blast with no area must give no range");
                expect(!R::SpellRange(spell(0, 0, false, false, false), 150.0f).Known(), "aimed with no reach must give no range");
            }
            // The snapshot fixture: a follower (id 7) and foes that target him.
            const auto foe = [](std::uint32_t a_id, std::uint32_t a_target, float a_dist) {
                Iaus::Combatant f;
                f.id = a_id; f.targetId = a_target; f.dist = a_dist; f.hpMax = 100.0f;
                return f;
            };
            {   // Mercenary pressed back-off (Q-T2-3)
                Iaus::Snapshot s;
                s.self.id = 7; s.meleeReach = 200.0f;
                Iaus::ClassGate g;
                g.backOffPressed = true;
                const float want[5] = { 0.0f, 0.0f, 0.6f, 0.8f, 1.0f };
                for (int n = 0; n <= 4; ++n) {
                    s.foes.clear();
                    for (int i = 0; i < n; ++i) s.foes.push_back(foe(100 + i, 7, 100.0f));
                    const float got = R::PressedBackOffNeed(s, g);
                    expect(Near(got, want[n]), std::format("Mercenary with {} pressing foe(s): want {:.1f}, got {:.2f}", n, want[n], got));
                }
                s.foes.clear();
                s.foes.push_back(foe(100, 7, 100.0f));
                s.foes.push_back(foe(101, 0, 100.0f));   // targets nobody: does not press
                expect(R::PressedBackOffNeed(s, g) == 0.0f, "a foe that targets nobody must not count as pressing");
                s.foes[1].targetId = 7;
                g.backOffPressed = false;
                expect(R::PressedBackOffNeed(s, g) == 0.0f, "a Knight / Warrior (backOffPressed false) must never back off");
            }
            {   // Mage back-off and the close
                Iaus::Snapshot s;
                s.self.id = 7; s.meleeReach = 200.0f;
                s.drawn = Iaus::PostureKind::None;
                s.equippedSpell = 0x1234;
                auto e = spell(2000, 0, false, false, false);
                e.source = Iaus::SpellSource::Spell;
                e.drop = Iaus::Prune::Drop::Keep;
                s.book.entries.push_back(e);
                Iaus::ClassGate g;   // unresolved: nothing class-gated
                s.foes.push_back(foe(9, 7, 100.0f));
                const float need = R::MageBackOffNeed(s, g);
                expect(Near(need, 1.0f - 100.0f / 720.0f), std::format("mage back-off with a melee foe at 100 (minimum 720): want {:.3f}, got {:.3f}", 1.0f - 100.0f / 720.0f, need));
                s.foes[0].rangedWeapon = true;
                expect(R::MageBackOffNeed(s, g) == 0.0f, "a foe with a ranged weapon does not have to close: no back-off");
                s.foes[0].rangedWeapon = false;
                s.foes[0].dist = 3000.0f;
                R::Close cl;
                const bool hold = R::RangeToHold(s, g, cl);
                expect(hold && Near(cl.radius, 1800.0f) && cl.far, std::format("range close with a foe at 3000: want hold, R 1800, far; got {} R {:.0f} far {}", hold, cl.radius, cl.far));
                s.foes[0].dist = 1000.0f;
                const bool hold2 = R::RangeToHold(s, g, cl);
                expect(hold2 && !cl.far, "a foe inside R x 1.15 must not file a NEW close (far false)");
                s.drawn = Iaus::PostureKind::Melee;
                expect(!R::RangeToHold(s, g, cl), "a drawn melee weapon is not the spell phase: no range close");
            }
            if (bad.empty())
                return Step::Pass("H4 spell range (touch 0.8 x reach, concentration 0.8, aimed 0.9 + minimum 0.4, self blast 0.8 x area), mage back-off, range close and the Mercenary "
                                  "press count (2 foes 0.6, 3 0.8, 4 1.0, Knight/Warrior never) all read as designed");
            return Step::Fail("Reposition rule mismatch:" + bad);
        }

    }

    const std::vector<ManualItem>& T2ManualItems() {
        // What a human must look at in the T2 field session (Docs/research/2026-11/in-game-test-list.md). Each names the test-list step(s). The automatic
        // halves (the roads served, the claims, the verdicts, the pure rules) are the t2.* tests above.
        static const std::vector<ManualItem> k = {
            { "t2.manual_seat_lines", "T2 24a / 24g / 24t, Movement batch 2", "the 'seats installed' lines are written to APMF.log, which MFO cannot read (t2.harbinger_seats judges what MFO can ask)",
              "APMF.log: '[ch.32] aversion seats installed', '[ch.8b reanimate] Reanimate corpse seat installed', '[ch.31] block hold deny ARMED', '[ch.33] combat style seats installed'; no '[selfcheck] ... REFUSED' naming ReanimateSeat" },
            { "t2.manual_aversion_foes", "T2 24b / 24c / 24d / 24e", "a foe's target choice needs a real fight of 3+ bandits (the claim itself is judged by t2.aversion_claim)",
              "APMF.log '[ch.32] aversion H ... landed>0 averted>0 kept-by-draw>0'; the Healer alone is still attacked ('kept-no-other-target'); a taunt still wins; each release road has its line; a save/load re-files the claims" },
            { "t2.manual_reanimate", "T2 24h / 24i / 24j / 24k / 24l", "a corpse must be raised in a fight (the humanoid corpse pick needs a dead bandit, not the runner's mudcrab)",
              "the corpse named in '[buff] ... CLAIM at <corpse>' stands up; a too-strong corpse is skipped ('[willwork] ... WILL NOT WORK'); one thrall at a time; EnableReanimateSeat=0 and an ABI 41 Harbinger degrade as the step says" },
            { "t2.manual_block_fight", "T2 24m / 24o / 24p / 24v, batch I 6", "the guard is an animation and a timing: raise, hold 2.5 s, stagger re-raise, stamina-out, cooldown rest (t2.block_hands judges the hand gate)",
              "'[block] ... guard UP for 2.5 s' then 'guard DOWN ... blocked a hit'; no attack/cast while it stands; 'block RE-RAISED' after a stagger; 'block ENDED early ... stamina at 0'; bBlock=0 and dismiss lines" },
            { "t2.manual_block_board", "T2 24n", "the board's row text is a UI string (the hand rule behind it is judged by t2.block_hands)",
              "board row reads 'block: a bow, a crossbow, a staff or bare fists ...' / 'his left hand holds a weapon, a spell or a staff ...' / 'the same one-handed weapon in both hands'; bow only: no [block] line" },
            { "t2.manual_power_attack", "T2 24q / 24r / 24w", "a power attack is an animation with a stamina bill and a hit flag in APMF.log",
              "'[power-attack] ... road=harbinger' then 'power attack STARTED (stamina spent S>0)' and 'HIT'; the [Probe] hit line carries the PowerAttack flag (Q82); bPowerAttackEngineRoad=0 gives 'road=graph'; a drained-stamina refusal gives its cooldown back" },
            { "t2.manual_attack_mix", "T2 24s", "needs ~20 attacks of a Warrior in a fight", "'[mix] ... ch.18 attack mix FILED ... floor 0.40' and a measured power share near or above 0.40 after 20 attacks" },
            { "t2.manual_combat_style", "T2 24t / 24u / 24x / 24y", "a combat style is a visible way of fighting; the 1.7.104 lines need that game version",
              "'[wstyle] ... ch.33 combat style claim FILED' for each; Knight and Mercenary visibly differ; bCombatStyle=0 gives the REFUSED lines; Mercenary side-steps on both lists (the open half of MFO-B806)" },
            { "t2.manual_movement_batch", "Movement batch 1-5", "needs a real group fight, cover and a hide/flee decision",
              "'HELD HOLDER GROUP JOIN REFUSED' and no ANCHOR LEAK (1); '[ch.24] GAIT PROBE' + SUMMARY (2); hide run to cover then the blow from stealth with the stance kept (3); non-sneak flee under 30% (3b/3c/3d); failures are loud (4); no hitch (5)" },
            { "t2.manual_h4_range", "T2 batch I 1 / 2 / 3", "a caster walking to range and a Mercenary darting in is movement you watch (the range numbers are t2.reposition_rules)",
              "'[range] ... ch.24 close CLAIMED (R about 0.9 of range)' then released at combat end; '[conceal] ... BACK-OFF point' and 'back-off run over'; Mercenary backs off at 2+ foes, Knight/Warrior never" },
            { "t2.manual_hidden_step", "T2 batch I 4 / 5", "needs an unaware bandit and five fights in a row",
              "'[conceal] ... STEP point ... (reach|line of fire)', 'step run over: arrived', the blow from stealth; the Thief counter reads 1 of 5 .. 0 of 5 and the fifth is the guaranteed stealth entry; it survives a save" },
            { "t2.manual_teach_spells", "T2 batch I 7 / 8", "the Teach Spells page is a UI flow that destroys a tome (the weighing rule is t2.spell_weighing)",
              "'[spellbook] ... no IAUS candidate' once per script-only spell; Teach spells button on a Wizard/Evoker/Spellsword, two presses, '[board] taught ... consumed book'; gamepad D-pad/A/B work; a Knight has no button" },
            { "t2.manual_theft_field", "Theft gate 1-4", "needs Whiterun, guards and in-game days with bCrimeGoldProbe",
              "'[theft-gate] verdict=BARRED:' lines (guard / owned / off-limits door) and ZERO '[crime-probe] BOUNTY UP'; dungeon loot still taken ('verdict=take'); an owned shop door is not declared" },
            { "t2.manual_steal", "Batch L 1-5 (G13)", "a witness, a line of sight and an owned item must exist in the world (t2.theft_gate judges the not-sneaking refusal only for a follower who is NOT sneaking: a Thief-line follower is always sneaking, so step 4's refusal is yours to see)",
              "'[steal] ... verdict=take' + TOOK with no bounty when nobody is around; REFUSED:<name> senses the follower|player; owner in view refused; guard refused; 'take SKIPPED at the write'" },
            { "t2.manual_pick_and_offclass", "Batch L 6-9 (G14 / G16)", "needs a locked chest, a Thief, and an Archer with a two-hander",
              "'[lockpick] ... PICKED' then looted; owned lock 'REFUSED (owned)'; the crime ring names road=steal|pick if a bounty appears; off-class two-hander: 'OFF-CLASS fallback weapon ... penalty ON' and MFOC_OffClassWeapon rank 1" },
            { "t2.manual_probes", "Stealth probes P1-P3 + Aim probe (Q-SIM-4 / Q-S-4)", "log capture for the sim calibration, over several real fights",
              "bStealthProbe=1: '[stealth-probe] self / seen / hit' lines from 3+ Thief fights per list; bAimProbe=1 (+ APMF [ProjectileAim] bProbe=1): '[aim]' lines from 3+ Archer fights per list; bring them back" },
        };
        return k;
    }

    void ClearT2State() {
        std::lock_guard lk(g_t2Mx);
        g_t2Res.clear();
    }

    void RegisterT2() {
        Register({ "t2.harbinger_seats", "T2 24a/24g/24m/24q/24t, Movement 2, batch I 1",
                   "(self-setup) nothing: asks every road MFO files through (ch.24 range/ground/disengage, ch.31 move/block/power attack, ch.32, ch.8b, ch.33) whether this Harbinger serves it",
                   30.0f, AlwaysPre, nullptr, SeatsStep, nullptr });
        Register({ "t2.aversion_claim", "T2 24a / 24e / 24f (Q94)",
                   "(self-setup) the class add-on live + Harbinger ABI 41 + bClassAversion; the runner equips Conjurer, then a mastered Conjurer with Evoker, then Healer, and restores yours",
                   90.0f, AversionPre, ClassSnapFix, AversionStep, nullptr });
        Register({ "t2.block_hands", "T2 24m / 24n",
                   "(self-setup) a spawned fight so he draws his weapon; BlockHandsReason is compared with the 24m/24n hand rule for the loadout he carries",
                   60.0f, AlwaysPre, BlockFix, BlockStep, nullptr });
        Register({ "t2.theft_gate", "Theft gate 1-4, Batch L 4",
                   "(self-setup) nothing: TakeBarred on a player-pack copy and on owned references of his cell, the main-thread guard scan, Steal::CheckNow while he is not sneaking (INFO, not judged, for a sneak-always class)",
                   30.0f, AlwaysPre, nullptr, TheftStep, TheftTimeout });
        Register({ "t2.spell_weighing", "T2 batch I 7 / 8",
                   "(self-setup) nothing: every script-only spell record of the load order must be un-weighable, Healing must weigh",
                   30.0f, AlwaysPre, nullptr, WeighStep, WeighTimeout });
        Register({ "t2.reposition_rules", "T2 batch I 1 / 2 / 3",
                   "(self-setup) nothing: iaus/Reposition.h on fixture spells and foes (H4 range, mage back-off, Mercenary press)",
                   20.0f, AlwaysPre, nullptr, RepositionStep, nullptr });
    }

}
