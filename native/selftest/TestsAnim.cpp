// THE ANIMATED-CAST TESTS (DEV BUILDS ONLY; anim-residue plan A1/A2, placed-spells M1/D2; in-game-test-list.md "Animation session").
// marth: "Getting animations for everything is non negotiable"; the dev gambit must run on ANY follower in ANY environment: each test lends
// the spell (and a class, a foe, a pool of magicka), judges, and the runner takes it all back. Every test ends in ONE line:
//     [anim] RESULT <id>: PASS|FAIL|SKIP|INFO | <follower> | <measured facts>      (FinishStep, Runner.cpp)
// HOW IT JUDGES: the DEV LOG TAP (a spdlog sink, InstallLogTap) keeps MFO's own last lines of the code under test ([cast] UNANIMATED,
// [cast-hold], [buff], [summon], [iaus], [spellbook]), so a verdict is pinned to the very lines marth reads; plus the dev counters of the buff
// claim road (cast/BuffRoad.cpp), the engine's TESSpellCastEvent (the caster seat fired) and the EffectMirror (the effect / body exists).
// The tap also counts every UNANIMATED line of the session (process), which anim.session reports last.
// To add a test: a Pre / Fix / Step and one Register line. To add a line kind to the tap: kTapTags.
// THREADS: the sinks run on any thread and touch only their own leaf-locked tables (no engine call, no logging under them). Steps are
// WORKER reads; the two engine writes (StopCombat of the follower while the gap is being made, the foe set on him) are MainThread::Post.
#include "selftest/SelfTest_internal.h"
#include "CasterConsent.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "Vocabulary.h"
#include "apmf/APMFBridge.h"
#include "cast/Actuation.h"
#include "classes/Classes.h"
#include "iaus/EffectMirror.h"
#include "iaus/Engine.h"
#include "iaus/SpellBook.h"

#include <spdlog/sinks/base_sink.h>

#include <deque>

namespace MFO::SelfTest {

    namespace {
        // ── tunables, one table ──────────────────────────────────────────────────────────────────
        struct Tuning {
            float foeHealth    = 6000.0f;
            float claimWait    = 45.0f;   // in combat this long without a claim = not picked / not cast
            float settle       = 3.0f;    // after the body exists: the road's up-read may be 2 s old
            float aliveWatch   = 12.0f;   // no second cast for this long while the creature lives
            float oneCastWatch = 8.0f;    // fire-once: exactly one cast in this long
            float recastMax    = 60.0f;   // fire-once: a re-cast is waited for only when the buff is this short
            float gapWait      = 12.0f;   // how long the gap is tried for
            int   gapPulls     = 6;       // StopCombat calls on the follower to keep him out of the fight
            float animWait     = 30.0f;   // own controller -> claim + effect
            int   blastLaps    = 30;
            float summonWait   = 150.0f;  // wait for a standing summon of his to end, then run anyway
        };
        constexpr Tuning kT{};
        // Skyrim.esm local ids, read in the master 2026-11-24.
        constexpr RE::FormID kFlameAtronach = 0x000204C3, kFamiliar = 0x000640B6, kOakflesh = 0x0005AD5C, kCandlelight = 0x00043324,
                             kWardLesser = 0x00013018, kAbMagicka = 0x00105F16;   // AbHighElfMagicka (constant ability)

        double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        // ── the log tap ─────────────────────────────────────────────────────────────────────────
        constexpr std::string_view kTapTags[] = { "[cast-hold] ", "[buff] ", "[summon] ", "[iaus] ", "[iaus-gen] ", "[spellbook] " };
        struct Line { std::uint64_t seq = 0; std::string text; };
        std::mutex                         g_tapMx;   // leaf
        std::deque<Line>                   g_lines;
        std::uint64_t                      g_seq = 0;
        struct Reason { int n = 0; std::string first; };
        std::map<std::string, Reason>      g_unanimated;   // reason -> lines this session (process)

        class Tap final : public spdlog::sinks::base_sink<std::mutex> {
        protected:
            void sink_it_(const spdlog::details::log_msg& a_m) override {
                const std::string_view t{ a_m.payload.data(), a_m.payload.size() };
                const bool             un = t.starts_with("[cast] ") && t.find("UNANIMATED road") != std::string_view::npos;
                bool                   keep = un;
                for (const auto tag : kTapTags) keep = keep || t.starts_with(tag);
                if (!keep) return;
                std::lock_guard lk(g_tapMx);
                g_lines.push_back({ ++g_seq, std::string(t) });
                while (g_lines.size() > 800) g_lines.pop_front();
                if (un) {
                    const auto  at = t.find(" -- ");
                    auto&       r  = g_unanimated[std::string(at == std::string_view::npos ? t : t.substr(at + 4))];
                    if (r.n++ == 0) r.first = std::string(t.substr(0, at));
                }
            }
            void flush_() override {}
        };

        std::uint64_t Seq() { std::lock_guard lk(g_tapMx); return g_seq; }
        // Lines after a_after that hold every needle (an empty needle always matches).
        std::vector<std::string> Lines(std::uint64_t a_after, std::string_view a_tag, std::string_view a_n1, std::string_view a_n2 = {}) {
            std::vector<std::string> out;
            std::lock_guard          lk(g_tapMx);
            for (const auto& l : g_lines)
                if (l.seq > a_after && l.text.starts_with(a_tag) && l.text.find(a_n1) != std::string::npos && l.text.find(a_n2) != std::string::npos)
                    out.push_back(l.text);
            return out;
        }
        std::string Hex(RE::FormID a) { return std::format("{:08X}", a); }
        std::string Last(const std::vector<std::string>& v) { return v.empty() ? std::string("none") : v.back().substr(0, 220); }

        // ── the caster seat's fires ─────────────────────────────────────────────────────────────
        struct Fired { RE::FormID caster = 0, spell = 0; double ms = 0; };
        std::mutex        g_fireMx;   // leaf
        std::deque<Fired> g_fires;
        class CastSink final : public RE::BSTEventSink<RE::TESSpellCastEvent> {
        public:
            static CastSink* GetSingleton() { static CastSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_e, RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
                if (!a_e || !a_e->object) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID id = a_e->object->GetFormID();
                if (!Followers::IsTrackedFast(id)) return RE::BSEventNotifyControl::kContinue;
                std::lock_guard lk(g_fireMx);
                g_fires.push_back({ id, a_e->spell, NowMs() });
                while (g_fires.size() > 128) g_fires.pop_front();
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ── shared per-run state (Scratch) ──────────────────────────────────────────────────────
        // set[0] = { spell }, set[1] = proxies the claim drove; n[0] log seq, n[1] start ms, n[2] claims at start, n[3] fired-ends at start,
        // n[4..7] the test's own; b[0] init, b[1] a class restore point was asked for.
        RE::FormID Spell(Ctx& c) { return c.s->set[0].empty() ? 0 : c.s->set[0][0]; }
        RE::SpellItem* SpellForm(Ctx& c) { return RE::TESForm::LookupByID<RE::SpellItem>(Spell(c)); }
        std::uint64_t Since(Ctx& c) { return static_cast<std::uint64_t>(c.s->n[0]); }
        std::uint32_t Claims(Ctx& c) { return Actuation::DevBuffClaimCount(Spell(c)) - static_cast<std::uint32_t>(c.s->n[2]); }
        std::uint32_t FiredEnds(Ctx& c) { return Actuation::DevBuffFiredEndCount(Spell(c)) - static_cast<std::uint32_t>(c.s->n[3]); }
        void NoteProxy(Ctx& c) {
            const auto p = APMFBridge::GetOffenseCastProxy(c.id, APMFBridge::kApmfHandLeft);
            if (p && std::find(c.s->set[1].begin(), c.s->set[1].end(), p) == c.s->set[1].end() && Claims(c) > 0) c.s->set[1].push_back(p);
        }
        int Fires(Ctx& c) {
            std::lock_guard lk(g_fireMx);
            int n = 0;
            for (const auto& f : g_fires) {
                if (f.caster != c.id || f.ms < c.s->n[1]) continue;
                const bool proxy = std::find(c.s->set[1].begin(), c.s->set[1].end(), f.spell) != c.s->set[1].end();
                if (f.spell == Spell(c) || proxy) ++n;
            }
            return n;
        }
        std::string QuotedName(Ctx& c) {
            auto* sp = SpellForm(c);
            return std::format("'{}'", sp && sp->GetName() && *sp->GetName() ? sp->GetName() : "?");
        }
        // Unanimated evidence against this run: the [cast] line (names the spell), or the summon's direct one-shot.
        std::string Unanimated(Ctx& c) {
            auto v = Lines(Since(c), "[cast] ", Hex(c.id) + ": UNANIMATED", QuotedName(c));
            if (v.empty()) v = Lines(Since(c), "[summon] " + Hex(c.id), Hex(Spell(c)), "road=direct");
            return v.empty() ? std::string{} : v.back().substr(0, 260);
        }
        bool FiredLine(Ctx& c) { return !Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)), "fired -- left hand released").empty(); }
        float Pool(RE::Actor* a) {
            auto* avo = a->AsActorValueOwner();
            return avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
        }
        float MaxPool(RE::Actor* a) {
            auto* avo = a->AsActorValueOwner();
            return avo ? avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                             a->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka)
                       : 0.0f;
        }
        bool EffectUp(RE::Actor* a, RE::SpellItem* sp) {
            const auto fx = a && sp ? EffectMirror::Get(a) : nullptr;
            if (!fx) return false;
            for (const auto& f : fx->fx) if (f.spell == sp && f.Live()) return true;
            // a proxy-driven cast carries the proxy as its spell: the effect record tells
            for (auto* e : sp->effects)
                if (e && e->baseEffect && fx->HasMagicEffect(e->baseEffect)) return true;
            return false;
        }
        RE::FormID Body(Ctx& c) {   // the creature from the lent spell (or the proxy that drove it)
            const auto fx = EffectMirror::Get(c.follower);
            if (!fx) return 0;
            for (const auto& f : fx->fx) {
                if (!f.summon || !f.commandedLive || !f.spell) continue;
                const auto id = f.spell->GetFormID();
                if (id == Spell(c) || std::find(c.s->set[1].begin(), c.s->set[1].end(), id) != c.s->set[1].end()) return f.commandedId;
            }
            return 0;
        }
        bool AnySummon(RE::Actor* a) {
            const auto fx = EffectMirror::Get(a);
            if (!fx) return false;
            for (const auto& f : fx->fx) if (f.summon && f.commandedLive) return true;
            return false;
        }
        void Begin(Ctx& c) {
            auto& s = *c.s;
            s.b[0]  = true;
            s.n[0]  = static_cast<double>(Seq());
            s.n[1]  = NowMs();
            s.n[2]  = Actuation::DevBuffClaimCount(Spell(c));
            s.n[3]  = Actuation::DevBuffFiredEndCount(Spell(c));
        }
        void TempClass(Ctx& c, const char* a_slug) {
            std::string why;
            if (c.s->b[1] && !ApplyClass(c.id, a_slug, 5, false, why)) spdlog::warn("[anim] {:08X} temp class {} refused: {}", c.id, a_slug, why);
        }
        std::string Facts(Ctx& c) {
            return std::format("{} ({:08X}): claims {}, caster fired {}, pool {:.0f}", QuotedName(c), Spell(c), Claims(c), Fires(c), Pool(c.follower));
        }
        Step End(Step a_s) { if (a_s.evidence.size() > 900) a_s.evidence.resize(900); return a_s; }

        // ── the spells of the load order, scanned once (form data only, worker) ─────────────────
        struct Pools { bool done = false; std::vector<RE::FormID> summons, fireOnce, blasts; };
        Pools g_pools;   // WORKER-ONLY
        float Cost(RE::SpellItem* sp, RE::Actor* a) { return sp->CalculateMagickaCost(a); }
        float Duration(RE::SpellItem* sp) {
            float d = 0.0f;
            for (auto* e : sp->effects) if (e) d = std::max(d, static_cast<float>(e->effectItem.duration));
            return d;
        }
        bool SummonOk(RE::SpellItem* sp) {
            if (!sp || sp->data.spellType != RE::MagicSystem::SpellType::kSpell || sp->effects.empty() || !Vocab::IsCastableSpell(sp)) return false;
            if (sp->GetDelivery() != RE::MagicSystem::Delivery::kTargetLocation || !Actuation::IsSummonSpell(sp)) return false;
            if (Actuation::CastRoadUnsupported(sp) || Actuation::SummonClaimUnservedWhy(sp, Actuation::ClassifyArchetype(sp))) return false;
            auto drop = Iaus::Prune::Drop::Keep;
            Iaus::SpellBook::WouldWeigh(sp, Iaus::ClassGate{}, nullptr, &drop);
            return drop == Iaus::Prune::Drop::Keep;
        }
        const Pools& Scan() {
            if (g_pools.done) return g_pools;
            g_pools.done = true;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return g_pools;
            std::unordered_set<RE::FormID> taught;   // only what a spell tome teaches: meant for a person, never a creature's or a script's own spell
            for (auto* b : dh->GetFormArray<RE::TESObjectBOOK>())
                if (b && b->TeachesSpell()) if (auto* sp = b->GetSpell()) taught.insert(sp->GetFormID());
            const auto bit = [](Actuation::EngineRow r) { return 1u << static_cast<std::uint32_t>(r); };
            const std::uint32_t wantRows = bit(Actuation::EngineRow::Script) | bit(Actuation::EngineRow::Restore);
            std::vector<std::pair<float, RE::FormID>> fire;
            for (const RE::FormID id : taught) {
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(id);
                if (!sp || sp->data.spellType != RE::MagicSystem::SpellType::kSpell || sp->effects.empty() || !Vocab::IsCastableSpell(sp)) continue;
                auto drop = Iaus::Prune::Drop::Keep;
                Iaus::SpellBook::WouldWeigh(sp, Iaus::ClassGate{}, nullptr, &drop);
                if (drop == Iaus::Prune::Drop::SelfBlast) g_pools.blasts.push_back(id);
                if (SummonOk(sp)) g_pools.summons.push_back(id);
                if (sp->GetDelivery() != RE::MagicSystem::Delivery::kSelf || sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) continue;
                if (CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Buff || Actuation::CastRoadUnsupported(sp)) continue;
                const auto a = Actuation::ClassifyArchetype(sp);
                if (a.anyHostile || a.twoHanded || a.spawnHazard || Actuation::IsSummonSpell(sp) || Duration(sp) < 2.0f) continue;
                if (a.shape == Actuation::SpellShape::Reanimate || a.shape == Actuation::SpellShape::BoundWeapon ||
                    a.shape == Actuation::SpellShape::SelfHazard) continue;
                if (!Actuation::BuffFireOnce(a)) continue;
                fire.emplace_back(Duration(sp) + (a.rowMask == wantRows ? 0.0f : 1.0e6f), id);   // the Script + Restore shape first, the shortest first
            }
            std::sort(fire.begin(), fire.end());
            for (const auto& f : fire) g_pools.fireOnce.push_back(f.second);
            std::sort(g_pools.summons.begin(), g_pools.summons.end());
            std::sort(g_pools.blasts.begin(), g_pools.blasts.end());
            spdlog::info("[anim] spell scan: {} taught spell(s): {} location summon(s), {} fire-once buff(s), {} blocked self blast(s)", taught.size(),
                         g_pools.summons.size(), g_pools.fireOnce.size(), g_pools.blasts.size());
            return g_pools;
        }
        // The first affordable one (the lent magicka ability adds 50), else 0.
        RE::FormID Affordable(Ctx& c, const std::vector<RE::FormID>& a_ids) {
            for (const auto id : a_ids)
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(id); sp && Cost(sp, c.follower) <= MaxPool(c.follower) + 45.0f && !EffectUp(c.follower, sp)) return id;
            return 0;
        }
        void Lend(Ctx& c, FixtureRequest& r, float a_casts = 1.0f) {
            auto* sp = SpellForm(c);
            r.spells.push_back(Spell(c));
            r.fillMagicka = true;
            if (Cost(sp, c.follower) * a_casts > MaxPool(c.follower))
                if (const auto ab = VanillaForm(kAbMagicka)) r.spells.push_back(ab);
        }
        void Foe(FixtureRequest& r) {
            r.spawn.on = true;
            r.spawn.clearSpot = true;
            r.spawn.health = kT.foeHealth;
        }
        void Snap(Ctx& c, FixtureRequest& r) {
            c.s->b[1]      = Classes::Active() && Classes::Writable();
            r.cls.snapshot = c.s->b[1];
        }
        TempRow CastRow(RE::FormID a_spell) {
            TempRow t;
            t.cond = Vocab::kCondAlways;
            t.act  = Vocab::kActCastSelf;
            t.form = a_spell;
            return t;
        }
        TempRow AttackRow() {
            TempRow t;
            t.cond = Vocab::kCondFoeAny;
            t.act  = Vocab::kActAttack;
            return t;
        }
        const char* Ready(Ctx& c) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (c.seenFor < 5.0f) return "5 s in the party";
            if (!c.follower->Is3DLoaded()) return "the follower loaded";
            if (c.inCombat || !pc || pc->IsInCombat()) return "out of combat";
            if (c.follower->IsOnMount()) return "the follower on foot";
            return nullptr;
        }
        const char* NoHarbinger() {
            if (!APMFBridge::Available()) return "Harbinger is absent";
            if (!APMFBridge::CastSeatsSupported() || !APMFBridge::OffenseCastClaimSupported()) return "Harbinger is too old for the cast seats";
            if (!Config::g_apmfCast.load() || Config::g_legacyCastHybrid.load() || !Config::g_equipToCast.load() || !Config::g_castSelf.load())
                return "the claim road is switched off in MFO.ini (bApmfCast, bLegacyCastHybrid, bEquipToCast, bCastSelf)";
            return nullptr;
        }

        // ── anim.summon_* ───────────────────────────────────────────────────────────────────────
        // n[4] elapsed when the body was first seen, n[5] claims then + settle, n[6] combat seconds without a claim; b[2] body seen, b[3] baseline taken.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_summonWait;   // WORKER-ONLY
        const char* SummonPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            const auto now = std::chrono::steady_clock::now();
            if (AnySummon(c.follower)) {   // his own (or the last test's) creature takes the summon slot: wait, bounded
                auto& at = g_summonWait[c.id];
                if (at == std::chrono::steady_clock::time_point{}) at = now;
                if (now - at < std::chrono::duration<float>(kT.summonWait)) return "his standing summon to end";
            }
            g_summonWait.erase(c.id);
            std::vector<RE::FormID> ids;
            for (const auto l : { kFlameAtronach, kFamiliar })
                if (const auto id = VanillaForm(l); id && SummonOk(RE::TESForm::LookupByID<RE::SpellItem>(id))) ids.push_back(id);
            for (const auto id : Scan().summons) ids.push_back(id);
            c.s->set[0].clear();
            if (const auto id = Affordable(c, ids)) c.s->set[0] = { id };
            return nullptr;
        }
        void SummonFix(Ctx& c, FixtureRequest& r, bool a_iaus) {
            if (!Spell(c) || NoHarbinger() || (a_iaus && !Config::g_iausAvailable.load())) return;
            Lend(c, r);
            Foe(r);
            r.iaus = a_iaus ? 1 : 0;
            if (a_iaus) Snap(c, r);
            else { r.rows.push_back(CastRow(Spell(c))); r.rows.push_back(AttackRow()); }
        }
        void SummonFixIaus(Ctx& c, FixtureRequest& r) { SummonFix(c, r, true); }
        void SummonFixGambit(Ctx& c, FixtureRequest& r) { SummonFix(c, r, false); }
        Step SummonStep(Ctx& c, bool a_iaus) {
            auto& s = *c.s;
            if (const char* why = NoHarbinger()) return Step::Skip(why);
            if (a_iaus && !Config::g_iausAvailable.load()) return Step::Skip("bIausAvailable = 0");
            if (!Spell(c)) return Step::Skip(std::format("no Target Location conjuration he could pay for (magicka {:.0f})", MaxPool(c.follower)));
            if (!s.b[0]) { Begin(c); if (a_iaus) TempClass(c, "conjurer"); return Step::Continue(); }
            NoteProxy(c);
            if (const auto bad = Unanimated(c); !bad.empty()) return End(Step::Fail("cast UNANIMATED: " + bad));
            const auto claims = Claims(c);
            const int  fires  = Fires(c);
            const auto picks  = a_iaus ? Lines(Since(c), "[iaus] ", "(" + Hex(c.id) + ")", QuotedName(c).substr(1, QuotedName(c).size() - 2)) : std::vector<std::string>{};
            const auto body   = Body(c);
            s.note = Facts(c) + std::format(", body {:08X}{}", body, a_iaus ? ", IAUS lines that name it " + std::to_string(picks.size()) + ", last: " + Last(picks) : std::string{});
            if (body && !s.b[2]) { s.b[2] = true; s.n[4] = c.elapsed; }
            if (!s.b[2]) {
                if (!c.inCombat) return Step::Retry("the fight ended before the summon; " + s.note);
                if (claims == 0) s.n[6] += 0.5;
                if (s.n[6] < kT.claimWait) return Step::Continue();
                if (const auto lim = Lines(Since(c), "[summon] " + Hex(c.id), Hex(Spell(c)), "limit"); !lim.empty()) return End(Step::Skip("his summon slot is taken: " + Last(lim)));
                if (Pool(c.follower) < Cost(SpellForm(c), c.follower)) return End(Step::Skip("his magicka never reached the cost; " + s.note));
                if (a_iaus) {
                    const auto all = Lines(Since(c), "[iaus] ", "(" + Hex(c.id) + ")");
                    const auto gen = Lines(Since(c), "[iaus-gen] ", "(" + Hex(c.id) + ")");
                    if (all.empty()) return End(Step::Retry("no IAUS decision was logged (he was never served in the fight); " + s.note));
                    return End(Step::Fail(std::format("IAUS never had it cast in {:.0f} s: last decision {} | {}", kT.claimWait, Last(all), Last(gen))));
                }
                const auto why = Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)));
                return End(Step::Fail(std::format("no claim in {:.0f} s of combat: {} | last [buff]: {}", kT.claimWait, s.note, Last(why))));
            }
            const float alive = c.elapsed - static_cast<float>(s.n[4]);
            if (claims == 0) return End(Step::Fail("the body exists but no claim was filed for it; " + s.note));
            if (fires > 1) return End(Step::Fail(std::format("cast {} times while the creature lives ({:.1f} s); {}", fires, alive, s.note)));
            if (alive < kT.settle) return Step::Continue();
            if (!s.b[3]) { s.b[3] = true; s.n[5] = claims; }
            if (body && claims > static_cast<std::uint32_t>(s.n[5])) return End(Step::Fail("a second claim was filed while the creature lives; " + s.note));
            if (body && alive < kT.aliveWatch) return Step::Continue();
            if (fires == 0 && !FiredLine(c)) return End(Step::Info("the claim was filed and the body exists, but no cast event named the spell: animated delivery not proven; " + s.note));
            return End(Step::Pass(std::format("ANIMATED: claim filed, caster seat fired once, body {:08X}, no second cast in {:.1f} s alive, no UNANIMATED line; {}",
                                              body, alive, s.note)));
        }
        Step SummonIaus(Ctx& c) { return SummonStep(c, true); }
        Step SummonGambit(Ctx& c) { return SummonStep(c, false); }
        Step AnyTimeout(Ctx& c) {
            if (!c.s->b[0]) return Step::Retry("never began");
            return End(Claims(c) > 0 ? Step::Fail("claimed but not completed in the window: " + c.s->note) : Step::Retry("nothing in the window: " + c.s->note));
        }

        // ── anim.fire_once ──────────────────────────────────────────────────────────────────────
        // n[4] elapsed at the first fire, b[2] first fire seen, b[3] one cast proven, n[5] claims when it was.
        const char* FireOncePre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            c.s->set[0].clear();
            if (!NoHarbinger() && APMFBridge::FireOnceSupported())
                if (const auto id = Affordable(c, Scan().fireOnce)) c.s->set[0] = { id };
            return nullptr;
        }
        void FireOnceFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c)) return;
            Lend(c, r, 2.0f);
            Foe(r);
            r.rows.push_back(CastRow(Spell(c)));
            r.rows.push_back(AttackRow());
        }
        Step FireOnceStep(Ctx& c) {
            auto& s = *c.s;
            if (const char* why = NoHarbinger()) return Step::Skip(why);
            if (!APMFBridge::FireOnceSupported()) return Step::Skip("Harbinger does not serve fire-once (ABI 47, [CastSeats] EnableFireOnce)");
            if (!Spell(c)) return Step::Skip(std::format("no ambiguous-row duration buff a tome teaches in this load order that he could pay for ({} of the shape)", Scan().fireOnce.size()));
            if (!s.b[0]) { Begin(c); return Step::Continue(); }
            NoteProxy(c);
            if (const auto bad = Unanimated(c); !bad.empty()) return End(Step::Fail("cast UNANIMATED: " + bad));
            const int  fires = Fires(c);
            const auto filed = Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)), "FIRE-ONCE");
            const bool up    = EffectUp(c.follower, SpellForm(c));
            s.note = Facts(c) + std::format(", FIRE-ONCE claim line {}, end reason Fired {}, released line {}, up {}, duration {:.0f} s", filed.empty() ? "no" : "yes",
                                            FiredEnds(c) ? "yes" : "no", FiredLine(c) ? "yes" : "no", up ? "yes" : "no", Duration(SpellForm(c)));
            const bool fired = fires > 0 || FiredLine(c);
            if (fired && !s.b[2]) { s.b[2] = true; s.n[4] = c.elapsed; }
            if (!s.b[2]) {
                if (!c.inCombat) return Step::Retry("the fight ended before the cast; " + s.note);
                if (Claims(c) == 0) s.n[6] += 0.5;
                if (s.n[6] >= kT.claimWait || c.elapsed > 60.0f) return End(Step::Fail("no fire: " + s.note + " | last [buff]: " + Last(Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c))))));
                return Step::Continue();
            }
            const float t = c.elapsed - static_cast<float>(s.n[4]);
            if (!s.b[3]) {
                const bool due = t >= std::min(kT.oneCastWatch, std::max(2.0f, Duration(SpellForm(c)) - 1.0f));
                if (fires > 1) return End(Step::Fail(std::format("cast {} times within {:.1f} s of the first; {}", fires, t, s.note)));
                if (!due) return Step::Continue();
                if (filed.empty()) return End(Step::Fail("the claim was not filed FIRE-ONCE; " + s.note));
                if (FiredEnds(c) == 0) return End(Step::Fail("GetClaimEndReason never read Fired; " + s.note));
                if (!FiredLine(c)) return End(Step::Fail("the claim was not released on the fire; " + s.note));
                s.b[3] = true;
                s.n[5] = Claims(c);
            }
            if (Duration(SpellForm(c)) > kT.recastMax)
                return End(Step::Pass("fire-once: one cast, Fired, released (a re-cast was not waited for, the buff is long); " + s.note));
            if (Claims(c) > static_cast<std::uint32_t>(s.n[5]) && fires > 1) return End(Step::Pass("fire-once: one cast, Fired, released, and cast again after it ran out; " + s.note));
            if (!c.inCombat || t > Duration(SpellForm(c)) + 25.0f) return End(Step::Info("one cast, Fired, released; no re-cast seen before the window ended; " + s.note));
            return Step::Continue();
        }

        // ── anim.party_gap / anim.party_gap_conc ────────────────────────────────────────────────
        // The foe is set on the PLAYER. While the party fights and he has no controller, the lent buff must be HELD ([cast-hold]). He is kept
        // out with StopCombat (bounded, and never once a claim stands). Then he is brought in and the buff must go animated.
        // n[4] elapsed of the hold, n[5] pulls, n[7] elapsed of the controller; b[2] hold seen, b[3] controller after it, b[4] foe sent to him.
        const char* GapPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            c.s->set[0].clear();
            std::vector<RE::FormID> ids;
            for (const auto l : { kOakflesh, kCandlelight })
                if (const auto id = VanillaForm(l)) ids.push_back(id);
            if (const auto id = Affordable(c, ids)) c.s->set[0] = { id };
            return nullptr;
        }
        const char* GapConcPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            c.s->set[0].clear();
            if (const auto id = VanillaForm(kWardLesser)) c.s->set[0] = { id };
            return nullptr;
        }
        void GapFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c) || NoHarbinger()) return;
            Lend(c, r, 2.0f);
            Foe(r);
            r.spawn.onPlayer = true;
            r.rows.push_back(CastRow(Spell(c)));
        }
        Step GapStep(Ctx& c) {
            auto& s = *c.s;
            if (const char* why = NoHarbinger()) return Step::Skip(why);
            if (!Spell(c)) return Step::Skip("no buff to lend (Oakflesh / Candlelight / Lesser Ward): not in the load order, already on him, or too dear");
            if (!s.b[0]) { Begin(c); return Step::Continue(); }
            NoteProxy(c);
            if (const auto bad = Unanimated(c); !bad.empty()) return End(Step::Fail("cast UNANIMATED: " + bad));
            auto*      pc     = RE::PlayerCharacter::GetSingleton();
            const bool ctrl   = c.follower->GetActorRuntimeData().combatController != nullptr;
            const auto held   = Lines(Since(c), "[cast-hold] " + Hex(c.id), Hex(Spell(c)));
            const auto claims = Claims(c);
            const bool up     = EffectUp(c.follower, SpellForm(c));
            s.note = Facts(c) + std::format(", own controller {}, kept out {:.0f}x, hold lines {}, effect up {} | {}", ctrl ? "yes" : "no", s.n[5], held.size(), up ? "yes" : "no", Last(held));
            if (!held.empty() && !s.b[2]) { s.b[2] = true; s.n[4] = c.elapsed; }
            if (!s.b[2]) {
                if (claims > 0) return End(Step::Retry("he joined the fight and claimed before a gap lap was seen; " + s.note));
                if (!ctrl && pc && !pc->IsInCombat() && c.elapsed > 8.0f) return End(Step::Retry("the party fight did not start; " + s.note));
                if (c.elapsed > kT.gapWait) return End(Step::Retry("no gap lap was seen; " + s.note));
                if (ctrl && s.n[5] < kT.gapPulls) {
                    s.n[5] += 1;
                    MainThread::Post([id = c.id]() {
                        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && !a->IsDead()) a->StopCombat();
                    });
                }
                return Step::Continue();
            }
            if (!ctrl && !s.b[4] && c.elapsed - static_cast<float>(s.n[4]) > 2.0f && c.hostile) {   // he stays out on his own: bring him in
                s.b[4] = true;
                PostEngage(c.hostile->GetFormID(), c.id);
            }
            if (ctrl && !s.b[3]) { s.b[3] = true; s.n[7] = c.elapsed; }
            if (claims > 0 && (Fires(c) > 0 || up || FiredLine(c)))
                return End(Step::Pass("held in the gap, then ANIMATED with his own controller, no UNANIMATED line; " + s.note));
            if (c.elapsed - static_cast<float>(s.b[3] ? s.n[7] : s.n[4]) < kT.animWait) return Step::Continue();
            if (!s.b[3]) return End(Step::Info("held in the gap, nothing unanimated; he never got a controller of his own, so the animated half is not judged; " + s.note));
            return End(Step::Fail(std::format("held in the gap, but not cast {:.0f} s after his controller appeared; {} | last [buff]: {}", kT.animWait, s.note,
                                              Last(Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)))))));
        }

        // ── anim.blocked_blast ──────────────────────────────────────────────────────────────────
        // n[4] IAUS laps seen, n[5] the last drop.
        const char* BlastPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            c.s->set[0].clear();
            if (const auto id = Affordable(c, Scan().blasts)) c.s->set[0] = { id };
            else if (!Scan().blasts.empty()) c.s->set[0] = { Scan().blasts.front() };
            return nullptr;
        }
        void BlastFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c) || !Config::g_iausAvailable.load()) return;
            Lend(c, r);
            Foe(r);
            Snap(c, r);
            r.iaus = 1;
        }
        Step BlastStep(Ctx& c) {
            auto& s = *c.s;
            if (!Config::g_iausAvailable.load()) return Step::Skip("bIausAvailable = 0");
            if (!Spell(c)) return Step::Skip("no hostile self area spell a tome teaches in this load order");
            if (!s.b[0]) { Begin(c); TempClass(c, "evoker"); return Step::Continue(); }
            const char* why  = nullptr;
            auto        drop = Iaus::Prune::Drop::Keep;
            const bool  weighs = Iaus::SpellBook::WouldWeigh(SpellForm(c), Iaus::ClassGate{}, &why, &drop);
            const auto  name = QuotedName(c).substr(1, QuotedName(c).size() - 2);
            Iaus::Readout ro;
            double        age = 1.0e9;
            if (Iaus::Engine::ReadReadout(c.id, ro, &age) && age < 0.75 && APMFBridge::InCombat(c.follower)) s.n[4] += 1;
            const auto named = Lines(Since(c), "[iaus] ", "(" + Hex(c.id) + ")", name);
            const bool affordable = Cost(SpellForm(c), c.follower) <= MaxPool(c.follower);
            s.note = std::format("{} ({:08X}): IAUS samples {:.0f}, named in a decision {}, fired {}, Teach page reason '{}', affordable {}", QuotedName(c), Spell(c), s.n[4],
                                 named.size(), Fires(c), Iaus::Prune::DropName(drop), affordable ? "yes" : "no");
            if (Fires(c) > 0) return End(Step::Fail("the blocked blast was CAST; " + s.note));
            if (weighs || drop != Iaus::Prune::Drop::SelfBlast) return End(Step::Fail("the spell is not blocked; " + s.note));
            if (!named.empty()) return End(Step::Fail("IAUS weighed it: " + Last(named) + "; " + s.note));
            if (s.n[4] >= kT.blastLaps) return End(affordable ? Step::Pass("never offered, never picked; " + s.note) : Step::Info("never offered (but he could not pay for it); " + s.note));
            if (!c.inCombat) return Step::Retry("the fight ended first; " + s.note);
            return Step::Continue();
        }
        Step BlastTimeout(Ctx& c) { return Step::Retry("too few IAUS laps in the window; " + c.s->note); }

        // ── anim.session ────────────────────────────────────────────────────────────────────────
        const char* SessionPre(Ctx&) { return OnlyBlockedLeft("anim.session") ? nullptr : "the other tests finished (or blocked) first"; }
        Step SessionStep(Ctx&) {
            int         total = 0, other = 0;
            std::string text, bad;
            {
                std::lock_guard lk(g_tapMx);
                for (const auto& [reason, r] : g_unanimated) {
                    const bool heal = reason.find("no combat controller: the animated heal needs the follower's own combat AI") != std::string::npos;   // party-fight heal, plan D1
                    total += r.n;
                    if (!heal) { other += r.n; bad += std::format(" [{}x {} | first: {}]", r.n, reason, r.first); }
                    else text += std::format(" [{}x party-fight heal]", r.n);
                }
            }
            const auto line = std::format("{} UNANIMATED line(s) since the game started, {} of them not party-fight heals:{}{}", total, other, bad, text);
            return End(other ? Step::Fail(line) : Step::Pass(line));
        }
    }

    void InstallLogTap() {
        if (auto l = spdlog::default_logger()) l->sinks().push_back(std::make_shared<Tap>());
    }

    void AnimSinks() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) holder->AddEventSink<RE::TESSpellCastEvent>(CastSink::GetSingleton());
    }

    void ClearAnimState() {
        g_summonWait.clear();
        g_pools = {};
        std::lock_guard lk(g_fireMx);
        g_fires.clear();
    }

    void RegisterAnim() {
        const char* ref = "anim-residue plan / placed-spells";
        Register({ "anim.summon_iaus", ref, "(any follower) a lent location summon, Sentient AI on, a spawned fight", 120.0f, SummonPre, SummonFixIaus, SummonIaus, AnyTimeout, true });
        Register({ "anim.fire_once", ref, "(any follower) a lent ambiguous-row buff (SKIP when the load order has none)", 150.0f, FireOncePre, FireOnceFix, FireOnceStep, AnyTimeout, true });
        Register({ "anim.party_gap", ref, "(any follower) a lent buff while only the player fights", 80.0f, GapPre, GapFix, GapStep, AnyTimeout, true });
        Register({ "anim.party_gap_conc", ref, "(any follower) the same with Lesser Ward (concentration)", 80.0f, GapConcPre, GapFix, GapStep, AnyTimeout, true });
        Register({ "anim.blocked_blast", ref, "(any follower) a lent hostile self area spell, Sentient AI on", 70.0f, BlastPre, BlastFix, BlastStep, BlastTimeout, true });
        Register({ "anim.summon_gambit", ref, "(any follower) the summon through a gambit row (after the first creature is gone)", 120.0f, SummonPre, SummonFixGambit, SummonGambit, AnyTimeout, true });
    }
    void RegisterAnimSession() {
        Register({ "anim.session", "anim-residue plan", "every other test finished or blocked: counts the session's UNANIMATED lines by reason", 20.0f, SessionPre, nullptr, SessionStep, nullptr });
    }

}
