// THE ANIMATED-CAST TESTS (DEV BUILDS ONLY; anim-residue plan A1/A2, placed-spells M1/D2/M4; in-game-test-list.md "Animation session").
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
#include "iaus/AreaActors.h"
#include "iaus/AreaCensus.h"
#include "iaus/EffectMirror.h"
#include "iaus/Engine.h"
#include "iaus/SpellBook.h"

#include <spdlog/sinks/base_sink.h>

#include <deque>
#include <unordered_set>

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
        // A spell-casting foe for the ward (field 11-24c: a melee mudcrab is no threat, so the engine's own Ward item said NO and no ward ever fired).
        // Skyrim.esm NPC_ EncBandit01MagicArgonianM (verified in the master 2026-11-24, tools/esp_inspect.py). Its spell list is NOT on the record
        // itself: ACBS template flags 0x0FBE, TPLT 00039D31, and the 0x08 bit takes the spells from that template; the list there is the same
        // plus LSpellBandit01FireFrostShock, so the foe does cast. Not essential, unique or protected; respawns; level 1; no VMAD; BanditFaction;
        // no crime faction, so no crime and no guards (inhabited places are refused anyway); not recruitable. Its loot goes with it when it is
        // disabled and deleted (the player may loot it first, harmless). It hits the player by design. Whether the engine's Ward item then says YES
        // is unproven. A plain base form of the master, load-order agnostic.
        constexpr RE::FormID kCasterFoe = 0x000C3C9F;

        double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        // ── the log tap ─────────────────────────────────────────────────────────────────────────
        constexpr std::string_view kTapTags[] = { "[cast-hold] ", "[buff] ", "[summon] ", "[iaus] ", "[iaus-gen] ", "[spellbook] ", "[cast-guard] " };
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
                while (g_fires.size() > 512) g_fires.pop_front();   // the whole party casts in a 330 s run
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
            if (p && std::find(c.s->set[1].begin(), c.s->set[1].end(), p) == c.s->set[1].end() && Claims(c) > 0 && c.s->set[1].empty()) c.s->set[1].push_back(p);   // the first claim's proxy only: a later proxied cast must count as a second cast
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
            s.b[0]  = true;   // the baselines n[0..3] were taken in Lend(), before the fixture and the rows applied
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
        struct Pools {
            bool done = false;
            std::vector<RE::FormID> summons, fireOnce, blasts;   // blasts = the HAZARD arm only since M4 (Prune::IsSelfHazardBlock, Drop::SelfBlast): the spells still kept out
            std::vector<RE::FormID> areaBlasts;                  // M4: Self hostile-area blasts IAUS offers (Drop::Keep), smallest guard first
            std::map<RE::FormID, std::pair<float, float>> radii; // areaBlasts: { guard units, foe units } from the record (the book's own numbers are read later)
            const char* unservedWhy = nullptr;                   // a blast shape the claim road cannot serve (BlastClaimUnservedWhy), when no one is
        };
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
            (void)Iaus::SpellBook::WouldWeigh(sp, Iaus::ClassGate{}, nullptr, &drop);
            return drop == Iaus::Prune::Drop::Keep;
        }
        // M4: a Self, fire-and-forget, Offense-kind spell with a hostile effect of its own area and no hazard left on the ground (the shape the claim road serves, read by the
        // shared readers, never by name). Served + offered (Drop::Keep) = a candidate of anim.self_blast; a shape no Harbinger row serves records why, for the SKIP.
        void BlastShape(RE::SpellItem* sp, Iaus::Prune::Drop a_drop) {
            if (sp->GetDelivery() != RE::MagicSystem::Delivery::kSelf || sp->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget ||
                CasterConsent::ClassifySpell(sp) != CasterConsent::SpellKind::Offense || Actuation::IsSummonSpell(sp))
                return;
            const auto arch = Actuation::ClassifyArchetype(sp);
            if (arch.hostileImpact || arch.hazardHostile || arch.spawnHazard) return;
            float foeFeet = 0.0f, widestFeet = 0.0f;
            for (auto* e : sp->effects) {
                if (!e || !e->baseEffect) continue;
                widestFeet = std::max(widestFeet, static_cast<float>(e->effectItem.area));
                if (e->baseEffect->IsHostile()) foeFeet = std::max(foeFeet, static_cast<float>(e->effectItem.area));
            }
            const float foe = Iaus::Prune::AreaUnits(foeFeet);
            if (!(foe >= 64.0f)) return;   // no hostile area of its own (an explosion-only or single-target effect): not the shape under test
            if (const char* why = Actuation::BlastClaimUnservedWhy(sp, arch)) {
                if (!g_pools.unservedWhy) g_pools.unservedWhy = why;
                return;
            }
            if (a_drop != Iaus::Prune::Drop::Keep) return;
            g_pools.areaBlasts.push_back(sp->GetFormID());
            g_pools.radii[sp->GetFormID()] = { std::max(Iaus::Prune::AreaUnits(widestFeet), foe), foe };
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
                (void)Iaus::SpellBook::WouldWeigh(sp, Iaus::ClassGate{}, nullptr, &drop);
                if (drop == Iaus::Prune::Drop::SelfBlast) g_pools.blasts.push_back(id);
                BlastShape(sp, drop);
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
            std::sort(g_pools.areaBlasts.begin(), g_pools.areaBlasts.end(), [](RE::FormID a, RE::FormID b) {
                const auto ra = g_pools.radii[a].first, rb = g_pools.radii[b].first;
                return ra != rb ? ra < rb : a < b;
            });
            spdlog::info("[anim] spell scan: {} taught spell(s): {} location summon(s), {} fire-once buff(s), {} self hazard(s) kept out, {} self area blast(s) offered{}", taught.size(),
                         g_pools.summons.size(), g_pools.fireOnce.size(), g_pools.blasts.size(), g_pools.areaBlasts.size(),
                         g_pools.areaBlasts.empty() && g_pools.unservedWhy ? std::format(" (none served: {})", g_pools.unservedWhy) : std::string{});
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
            auto& s  = *c.s;   // the log / count baselines, taken BEFORE the fixture and the temp rows apply (a rate-limited first line must not precede them)
            s.n[0]   = static_cast<double>(Seq());
            s.n[1]   = NowMs();
            s.n[2]   = Actuation::DevBuffClaimCount(Spell(c));
            s.n[3]   = Actuation::DevBuffFiredEndCount(Spell(c));
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
        const char* NoLog() { return Config::g_enableLogging.load() ? nullptr : "bEnableLogging = 0 (the verdicts read the log lines)"; }
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
            if (!Spell(c) || NoLog() || NoHarbinger() || (a_iaus && !Config::g_iausAvailable.load())) return;
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
            if (const char* why = NoLog()) return Step::Skip(why);
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
                if (AnySummon(c.follower)) return End(Step::Skip("another creature of his holds the summon slot; " + s.note));
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
            if (claims == 0) return End(Step::Info("cast outside MFO's claim (the body exists, no claim was filed: the vanilla AI cast it); " + s.note));
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
            if (const char* why = NoLog()) return Step::Skip(why);
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
                if (filed.empty()) return End(Claims(c) == 0 ? Step::Info("cast outside MFO's claim (it fired, no claim was filed: the vanilla AI cast it); " + s.note)
                                                             : Step::Fail("the claim was not filed FIRE-ONCE; " + s.note));
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
        // n[4] elapsed of the hold, n[5] pulls, n[7] elapsed of the controller; b[2] hold seen, b[3] controller after it, b[4] foe sent to him, n[6] claims at the hold line.
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
            // The ward needs a foe that gives it a reason (a spell-casting one): no resolvable caster foe = nothing lent = SKIP with the reason.
            const auto foe = VanillaForm(kCasterFoe);
            if (!foe || !RE::TESForm::LookupByID<RE::TESNPC>(foe)) return nullptr;
            if (const auto id = VanillaForm(kWardLesser)) c.s->set[0] = { id };
            return nullptr;
        }
        void GapFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c) || NoLog() || NoHarbinger()) return;
            Lend(c, r, 2.0f);
            Foe(r);
            r.spawn.onPlayer = true;
            r.rows.push_back(CastRow(Spell(c)));
        }
        void GapConcFix(Ctx& c, FixtureRequest& r) {
            GapFix(c, r);
            if (r.spawn.on) r.spawn.baseLocal = kCasterFoe;   // the ward's foe: a spell-caster, not the default mudcrab
        }
        Step GapStep(Ctx& c) {
            auto& s = *c.s;
            if (const char* why = NoLog()) return Step::Skip(why);
            if (const char* why = NoHarbinger()) return Step::Skip(why);
            if (!Spell(c)) return Step::Skip("no buff to lend (Oakflesh / Candlelight / Lesser Ward): not in the load order, already on him, or too dear (the ward also needs the spell-casting bandit mage of Skyrim.esm as its foe)");
            if (!s.b[0]) { Begin(c); return Step::Continue(); }
            NoteProxy(c);
            if (const auto bad = Unanimated(c); !bad.empty()) return End(Step::Fail("cast UNANIMATED: " + bad));
            auto*      pc     = RE::PlayerCharacter::GetSingleton();
            const bool ctrl   = c.follower->GetActorRuntimeData().combatController != nullptr;
            const auto held   = Lines(Since(c), "[cast-hold] " + Hex(c.id), Hex(Spell(c)));
            const auto claims = Claims(c);
            const bool up     = EffectUp(c.follower, SpellForm(c));
            s.note = Facts(c) + std::format(", own controller {}, kept out {:.0f}x, hold lines {}, effect up {} | {}", ctrl ? "yes" : "no", s.n[5], held.size(), up ? "yes" : "no", Last(held));
            if (!held.empty() && !s.b[2]) { s.b[2] = true; s.n[4] = c.elapsed; s.n[6] = claims; }   // n[6] claims at the hold line: PASS needs a newer one
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
            if (claims > static_cast<std::uint32_t>(s.n[6]) && s.b[3] && (Fires(c) > 0 || up || FiredLine(c)))
                return End(Step::Pass("held in the gap, then ANIMATED with his own controller, no UNANIMATED line; " + s.note));
            if (c.elapsed - static_cast<float>(s.b[3] ? s.n[7] : s.n[4]) < kT.animWait) return Step::Continue();
            if (!s.b[3]) return End(Step::Info("held in the gap, nothing unanimated; he never got a controller of his own, so the animated half is not judged; " + s.note));
            return End(Step::Fail(std::format("held in the gap, but not cast {:.0f} s after his controller appeared; {} | last [buff]: {}", kT.animWait, s.note,
                                              Last(Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)))))));
        }

        // ── anim.blocked_blast ──────────────────────────────────────────────────────────────────
        // Since M4 only the HAZARD arm stays blocked (Prune::IsSelfHazardBlock, Drop::SelfBlast: Blizzard, the damage circles); the area arm (Frost Nova, Fire Storm) is offered
        // and is anim.self_blast's. The spell is picked at runtime from the taught spells whose static drop is SelfBlast (Scan().blasts), never by name.
        // n[4] IAUS laps seen, n[5] the last drop.
        const char* BlastPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            c.s->set[0].clear();
            if (const auto id = Affordable(c, Scan().blasts)) c.s->set[0] = { id };
            else if (!Scan().blasts.empty()) c.s->set[0] = { Scan().blasts.front() };
            return nullptr;
        }
        void BlastFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c) || NoLog() || !Config::g_iausAvailable.load()) return;
            Lend(c, r);
            Foe(r);
            Snap(c, r);
            r.iaus = 1;
        }
        Step BlastStep(Ctx& c) {
            auto& s = *c.s;
            if (const char* why = NoLog()) return Step::Skip(why);
            if (!Config::g_iausAvailable.load()) return Step::Skip("bIausAvailable = 0");
            if (!Spell(c)) return Step::Skip("no self hazard spell (a spawned hostile hazard, Prune::IsSelfHazardBlock: Blizzard, a damage circle) is taught by a tome in this load order");
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
            if (weighs || drop != Iaus::Prune::Drop::SelfBlast) return End(Step::Fail("the self hazard spell is not blocked (Prune::IsSelfHazardBlock); " + s.note));
            if (!named.empty()) return End(Step::Fail("IAUS weighed it: " + Last(named) + "; " + s.note));
            if (s.n[4] >= kT.blastLaps) return End(affordable ? Step::Pass("never offered, never picked; " + s.note) : Step::Info("never offered (but he could not pay for it); " + s.note));
            if (!c.inCombat) return Step::Retry("the fight ended first; " + s.note);
            return Step::Continue();
        }
        Step BlastTimeout(Ctx& c) { return Step::Retry("too few IAUS laps in the window; " + c.s->note); }


        // ── anim.self_blast (M4) ────────────────────────────────────────────────────────────────
        // A lent Self hostile-area blast (picked at runtime by the shared readers, Scan().areaBlasts: the smallest guard first). THE TEST ARRANGES EVERYTHING, the player does nothing:
        //   Plan    a navmesh point (NavSpots) at least guard + body + ally margin + safety margin from the player and every other follower (and clear of bystanders).
        //   Travel  one dev Stand order (DevHold, strict where served, else the plain point hold) takes him there in the fight; every OTHER follower is held where he stands.
        //           Our own foe is set down inside the hostile radius on the main thread (bounded).
        //   Leg 1   the claim ([buff] SELF BLAST), one animated cast per claim, the foe hit BY THE BLAST (FoeHit: spell id / proxy / magic effect, never a health step alone).
        //   Leg 2   another follower is re-pointed into the PARTY PAD ring (past the blast's reach, inside the pad): no cast for the dwell, no standing claim. None = not judged.
        //   Leg 3   he is sent back to his place: a cast again.
        // SAFETY: the ring is past the blast's own reach, so a failed guard cannot hurt the ally; the player is never moved. Numbers: kB.
        // All claims are released at every end (AnimEnd for the others, HoldEnd for him); the foe is the run's own fixture.
        struct BlastTuning {
            float reachPad = 96.0f;       // a friend counts for leg 2 only this far past the blast's own reach
            float clearPad = 64.0f;       // leg 1 / 3 "clear": past the pad by this much (the census margin has hysteresis)
            float planSlack = 48.0f;      // a point this much better than needed is taken first
            float maxWalk = 4500.0f, allyWalk = 2500.0f;
            float radius = 32.0f, radiusPlain = 64.0f;   // Stand order / plain hold
            float bookWait = 20.0f, planWait = 8.0f, travelWait = 50.0f, allyWait = 45.0f, leg2Max = 75.0f, leg3Cast = 40.0f;
            float dwell = 8.0f, grace = 2.0f;   // grace: a charge already in flight is the engine's, not MFO's, to cut
            float foeFar = 3.0f, placeEvery = 6.0f;
            int   placements = 8, refiles = 4, plans = 2, recasts = 3;
            float careEvery = 3.0f;
            double hitBefore = 250.0, hitAfter = 3500.0;   // ms round the cast
        };
        constexpr BlastTuning kB{};
        enum class SbPhase : std::uint8_t { Book, Plan, Travel, Leg1, Leg1Read, RingPlan, RingWalk, Dwell, Back, Leg3 };
        struct SbHeld { RE::FormID id = 0; RE::NiPoint3 pt{}; int refiles = 0; float at = 0; };
        struct SelfBlastRun {
            SbPhase       phase = SbPhase::Book;
            float         phaseAt = 0, lastElapsed = 0, secs = 0, careAt = 0, farSecs = 0, placeAt = -99, padSecs = 0, ringSecs = 0;
            std::uint32_t ticket = 0, placeTicket = 0;
            int           plans = 0, placements = 0, refiles = 0, firesSeen = 0, firesMark = 0, recasts = 0;
            bool          filed = false, strict = false, graceTaken = false;
            float         refileAt = 0;
            RE::NiPoint3  pt{}, home{};
            std::vector<SbHeld> held;
            std::vector<std::pair<RE::FormID, double>> proxies;   // a proxy of this blast's claim, and since when (a pool slot may have served another spell before)
            RE::FormID    ally = 0;
            double        fireMs = 0;
            float         fireFoeDist = -1, hpBefore = 0, hpPrev = 0;
            std::string   planWhy, leg1Note, leg2Note, placeWhy;
        };
        std::unordered_map<RE::FormID, SelfBlastRun> g_selfBlast;   // WORKER-ONLY

        // ── passive hit sinks (any thread, ids only): what reached whom by which spell / magic effect ──
        struct BlastHit { RE::FormID cause = 0, target = 0, source = 0, mgef = 0; double ms = 0; };
        std::mutex           g_blastMx;   // leaf
        std::deque<BlastHit> g_blastHits;
        void NoteBlastHit(const BlastHit& a_h) {
            std::lock_guard lk(g_blastMx);
            g_blastHits.push_back(a_h);
            while (g_blastHits.size() > 256) g_blastHits.pop_front();
        }
        class BlastHitSink final : public RE::BSTEventSink<RE::TESHitEvent>, public RE::BSTEventSink<RE::TESMagicEffectApplyEvent> {
        public:
            static BlastHitSink* GetSingleton() { static BlastHitSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override {
                if (a_e && a_e->cause && a_e->target && a_e->source && Followers::IsTrackedFast(a_e->cause->GetFormID()))
                    NoteBlastHit({ a_e->cause->GetFormID(), a_e->target->GetFormID(), a_e->source, 0, NowMs() });
                return RE::BSEventNotifyControl::kContinue;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override {
                if (a_e && a_e->caster && a_e->target && Followers::IsTrackedFast(a_e->caster->GetFormID()))
                    NoteBlastHit({ a_e->caster->GetFormID(), a_e->target->GetFormID(), 0, a_e->magicEffect, NowMs() });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ── the main-thread jobs ──
        struct SbSpot { bool ok = false; RE::NiPoint3 pt{}; std::string why; };
        std::mutex                                g_sbMx;   // leaf
        std::unordered_map<std::uint32_t, SbSpot> g_sbSpots;
        std::uint32_t                             g_sbTicket = 1;   // WORKER
        template <class Job> std::uint32_t PostSpot(Job a_job) {
            const auto t = g_sbTicket++;
            MainThread::Post([t, job = std::move(a_job)]() {
                SbSpot s = job();
                std::lock_guard lk(g_sbMx);
                g_sbSpots[t] = std::move(s);
            });
            return t;
        }
        std::optional<SbSpot> TakeSpot(std::uint32_t a_t) {
            std::lock_guard lk(g_sbMx);
            const auto it = g_sbSpots.find(a_t);
            if (it == g_sbSpots.end()) return std::nullopt;
            auto s = std::move(it->second);
            g_sbSpots.erase(it);
            return s;
        }
        // MAIN. Somebody else than the run's foe fights the player: the test yields.
        RE::FormID PlayerFoeOther(RE::FormID a_foe) {
            auto*      pc = RE::PlayerCharacter::GetSingleton();
            auto*      pl = RE::ProcessLists::GetSingleton();
            RE::FormID who = 0;
            if (pc && pl)
                pl->ForEachHighActor([&](RE::Actor* x) {
                    if (!x || x == pc || x->GetFormID() == a_foe || x->IsDead() || x->GetActorRuntimeData().currentCombatTarget.get().get() != pc)
                        return RE::BSContainer::ForEachResult::kContinue;
                    who = x->GetFormID();
                    return RE::BSContainer::ForEachResult::kStop;
                });
            return who;
        }
        // MAIN. The nearest walk whose end is a_need from the player and every a_friends member, and a_byNeed from every other actor that is no foe of his.
        SbSpot PlanFar(RE::FormID a_f, RE::FormID a_foe, const std::vector<RE::FormID>& a_friends, float a_need, float a_byNeed) {
            SbSpot o;
            auto*  f  = RE::TESForm::LookupByID<RE::Actor>(a_f);
            auto*  pc = RE::PlayerCharacter::GetSingleton();
            if (!f || !pc || f->IsDead() || !f->Is3DLoaded()) { o.why = "the follower is gone"; return o; }
            if (const auto other = PlayerFoeOther(a_foe)) { o.why = std::format("the player is fought by {:08X}", other); return o; }
            auto* foe = a_foe ? RE::TESForm::LookupByID<RE::Actor>(a_foe) : nullptr;
            std::vector<RE::NiPoint3> fr{ pc->GetPosition() }, by;
            for (const auto id : a_friends)
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && !a->IsDead() && a->Is3DLoaded()) fr.push_back(a->GetPosition());
            if (auto* pl = RE::ProcessLists::GetSingleton())
                pl->ForEachHighActor([&](RE::Actor* x) {
                    if (x && x != f && x != pc && x != foe && !x->IsDead() && !x->IsDisabled() && x->Is3DLoaded() && !x->IsCommandedActor() &&
                        std::find(a_friends.begin(), a_friends.end(), x->GetFormID()) == a_friends.end() && (x->IsPlayerTeammate() || !x->IsHostileToActor(f)))
                        (x->IsPlayerTeammate() ? fr : by).push_back(x->GetPosition());
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            const auto  me   = f->GetPosition();
            const float skip = std::max(0.0f, a_need - me.GetDistance(pc->GetPosition()));   // a nearer walk cannot reach it (triangle inequality), so it is not listed
            const auto  nearest = [](const std::vector<RE::NiPoint3>& v, const RE::NiPoint3& p) {
                float d = 1.0e9f;
                for (const auto& q : v) d = std::min(d, q.GetDistance(p));
                return d;
            };
            std::string nav;
            float       best = 0.0f;
            const auto  pts = WalkPoints(f, foe, kB.maxWalk, nav, skip);
            const NavPoint* pick = nullptr;
            for (const auto& n : pts) {
                if (nearest(by, n.p) < a_byNeed) continue;
                const float d = nearest(fr, n.p);
                best = std::max(best, d);
                if (d >= a_need + kB.planSlack) { pick = &n; break; }
                if (d >= a_need && !pick) pick = &n;
            }
            if (!pick) {
                o.why = std::format("needs {:.0f}u from the player and {} other follower(s), the best point he can walk to within {:.0f}u has {:.0f}u ({}; {} bystander(s), {:.0f}u each)",
                                    a_need, fr.size() - 1, kB.maxWalk, best, nav, by.size(), a_byNeed);
                return o;
            }
            o = { true, pick->p, std::format("walk {:.0f}u, nearest friend {:.0f}u of {:.0f}u needed ({})", pick->walk, nearest(fr, pick->p), a_need, nav) };
            return o;
        }
        // MAIN. A point the ally can walk to, a_lo to a_hi from the caster (the middle first).
        SbSpot PlanRing(RE::FormID a_ally, RE::FormID a_caster, float a_lo, float a_hi) {
            SbSpot o;
            auto*  a = RE::TESForm::LookupByID<RE::Actor>(a_ally);
            auto*  f = RE::TESForm::LookupByID<RE::Actor>(a_caster);
            if (!a || !f || a->IsDead() || !a->Is3DLoaded()) { o.why = "the ally is gone"; return o; }
            const auto  c = f->GetPosition();
            std::string nav;
            const auto  pts = WalkPoints(a, nullptr, kB.allyWalk, nav, std::max(0.0f, a->GetPosition().GetDistance(c) - a_hi));
            const float mid = (a_lo + a_hi) * 0.5f;
            const NavPoint* pick = nullptr;
            for (const auto& n : pts) {
                const float d = n.p.GetDistance(c);
                if (d < a_lo || d > a_hi) continue;
                if (!pick) pick = &n;
                if (std::fabs(d - mid) <= (a_hi - a_lo) * 0.25f) { pick = &n; break; }
            }
            if (!pick) { o.why = std::format("no point {:.0f} to {:.0f}u from the caster the ally can walk to ({})", a_lo, a_hi, nav); return o; }
            o = { true, pick->p, std::format("{:.0f}u from the caster, walk {:.0f}u", pick->p.GetDistance(c), pick->walk) };
            return o;
        }
        // MAIN. Set the run's own foe down on standable ground a_dist from the caster (never past a_max).
        SbSpot PlaceFoe(RE::FormID a_foe, RE::FormID a_caster, float a_dist, float a_max) {
            SbSpot o;
            auto*  foe = RE::TESForm::LookupByID<RE::Actor>(a_foe);
            auto*  f   = RE::TESForm::LookupByID<RE::Actor>(a_caster);
            auto*  bo  = foe ? foe->GetBaseObject() : nullptr;
            if (!f || !foe || foe->IsDead() || foe->IsDisabled() || !bo || bo->GetFormID() != VanillaForm(SpawnSpec{}.baseLocal)) { o.why = "the foe is gone"; return o; }
            if (const auto other = PlayerFoeOther(a_foe)) { o.why = std::format("the player is fought by {:08X}", other); return o; }
            for (int i = 0; i < 8; ++i) {
                std::uint32_t st = 0;
                RE::NiPoint3  p{};
                if (!APMFBridge::FindStandPoint(a_caster, f->GetAngleZ() + 0.7854f * static_cast<float>(i), a_dist, 40.0f, p, st)) continue;
                const float d = f->GetPosition().GetDistance(p);
                if (d < 60.0f || d > a_max) continue;
                foe->SetPosition(p, true);
                o = { true, p, std::format("set down {:.0f}u from him", d) };
                return o;
            }
            o.why = std::format("no standable ground {:.0f}u round him", a_dist);
            return o;
        }
        // The caster's lost health and magicka are given back while the test runs (the test must not cost him the fight, and a hold must not be an empty pool).
        void PostCare(RE::FormID a_f) {
            MainThread::Post([a_f]() {
                auto* f   = RE::TESForm::LookupByID<RE::Actor>(a_f);
                auto* avo = f && !f->IsDead() ? f->AsActorValueOwner() : nullptr;
                if (!avo) return;
                for (const auto av : { RE::ActorValue::kHealth, RE::ActorValue::kMagicka })
                    if (const float lost = -f->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, av); lost > 0.0f)
                        avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, av, lost);
            });
        }

        std::vector<RE::FormID> OtherFollowers(RE::FormID a_id) {
            std::vector<RE::FormID> out;
            if (const auto snap = Followers::ActiveSnapshot())
                for (const auto id : *snap) {
                    auto* a = id == a_id ? nullptr : RE::TESForm::LookupByID<RE::Actor>(id);
                    if (a && !a->IsDead() && !a->IsDisabled() && a->Is3DLoaded()) out.push_back(id);
                }
            return out;
        }
        // The nearest friend (player or another party member): his distance from the follower, or a huge number. WORKER read.
        float NearestFriend(Ctx& c) {
            float best = 1.0e9f;
            const auto me = c.follower->GetPosition();
            if (auto* pc = RE::PlayerCharacter::GetSingleton()) best = std::min(best, pc->GetPosition().GetDistance(me));
            for (const auto id : OtherFollowers(c.id))
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id)) best = std::min(best, a->GetPosition().GetDistance(me));
            return best;
        }
        // A bystander (no friend, no foe) inside a_guard + his body + the safety margin, or the picture not ready: the guard may hold by right. "" = none.
        std::string BystanderWhy(Ctx& c, float a_guard) {
            std::vector<Iaus::AreaActor> rows;
            if (AreaActors::Read(c.id, rows) != Iaus::AreaPicture::Ready) return "the area picture is not ready";
            const auto others = OtherFollowers(c.id);
            const auto me     = c.follower->GetPosition();
            for (const auto& b : rows) {
                if (b.id == 0x14 || (c.hostile && b.id == c.hostile->GetFormID()) || std::find(others.begin(), others.end(), b.id) != others.end()) continue;
                if ((b.kind == Iaus::AreaKind::PartySummon || b.kind == Iaus::AreaKind::PlayerHorse) && !Config::g_areaSummonsBlock.load()) continue;
                const float d = RE::NiPoint3{ b.x, b.y, b.z }.GetDistance(me);
                if (!(d > a_guard + std::max(b.radius, Iaus::Estimate::kFireBodyPad) + Iaus::kAreaGuard.safetyMargin)) return std::format("bystander {:08X} {:.0f}u away", b.id, d);
            }
            return {};
        }
        const Iaus::SpellEntry* BlastEntry(const std::shared_ptr<const Iaus::Book>& a_book, RE::FormID a_spell) {
            if (a_book) for (const auto& e : a_book->entries) if (e.form == a_spell) return &e;
            return nullptr;
        }
        // Every proxy a standing claim of THIS blast drove (leg 3 may run on another one than leg 1).
        void NoteBlastProxy(Ctx& c) {
            if (Actuation::StandingSelfBlast(c.id) != Spell(c)) return;
            const auto p = APMFBridge::GetOffenseCastProxy(c.id, APMFBridge::kApmfHandLeft);
            if (!p || std::find(c.s->set[1].begin(), c.s->set[1].end(), p) != c.s->set[1].end()) return;
            c.s->set[1].push_back(p);
            g_selfBlast[c.id].proxies.emplace_back(p, NowMs() - 1500.0);   // the claim is at most a step or two old
        }
        // His casts of the blast: the spell itself, or a proxy once its claim drove it.
        std::pair<int, double> BlastFires(Ctx& c) {   // count, ms of the last
            const auto& px = g_selfBlast[c.id].proxies;
            std::pair<int, double> o{ 0, 0.0 };
            std::lock_guard        lk(g_fireMx);
            for (const auto& f : g_fires) {
                if (f.caster != c.id || f.ms < c.s->n[1]) continue;
                if (f.spell != Spell(c) && std::none_of(px.begin(), px.end(), [&](const auto& q) { return q.first == f.spell && f.ms >= q.second; })) continue;
                ++o.first;
                o.second = f.ms;
            }
            return o;
        }
        bool IsBlastForm(Ctx& c, RE::FormID a_f) {
            return a_f && (a_f == Spell(c) || std::find(c.s->set[1].begin(), c.s->set[1].end(), a_f) != c.s->set[1].end());
        }
        // The foe was hit BY THE BLAST cast at a_ms; "" = no proof. Never a health step alone (a melee swing or a teammate could make one).
        std::string FoeHit(Ctx& c, double a_ms) {
            const RE::FormID foe = c.hostile ? c.hostile->GetFormID() : 0;
            auto*            sp  = SpellForm(c);
            if (!foe || !sp) return {};
            std::vector<BlastHit> hits;
            {
                std::lock_guard lk(g_blastMx);
                for (const auto& h : g_blastHits)
                    if (h.cause == c.id && h.target == foe && h.ms >= a_ms - kB.hitBefore && h.ms <= a_ms + kB.hitAfter) hits.push_back(h);
            }
            for (const auto& h : hits)
                if (IsBlastForm(c, h.source)) return std::format("hit event by {:08X}", h.source);
            // Other casts round the blast: his own that carry the same magic effect, or the same spell by another follower, make the weaker proofs ambiguous.
            std::vector<RE::FormID> his;
            bool                    sameByOther = false;
            {
                std::lock_guard lk(g_fireMx);
                for (const auto& f : g_fires) {
                    if (f.ms < a_ms - 30000.0 || f.ms > a_ms + kB.hitAfter) continue;
                    if (f.caster == c.id) { if (!IsBlastForm(c, f.spell)) his.push_back(f.spell); }
                    else if (IsBlastForm(c, f.spell)) sameByOther = true;
                }
            }
            const auto carries = [](RE::FormID a_spell, RE::FormID a_mgef) {
                if (auto* o = RE::TESForm::LookupByID<RE::SpellItem>(a_spell))
                    for (auto* e : o->effects) if (e && e->baseEffect && e->baseEffect->GetFormID() == a_mgef) return true;
                return false;
            };
            for (const auto& h : hits) {
                if (!h.mgef) continue;
                bool ours = false;
                for (auto* e : sp->effects) ours = ours || (e && e->baseEffect && e->baseEffect->IsHostile() && e->effectItem.area > 0 && e->baseEffect->GetFormID() == h.mgef);
                if (!ours || std::any_of(his.begin(), his.end(), [&](RE::FormID o) { return carries(o, h.mgef); })) continue;
                return std::format("magic effect {:08X} of it applied by him {:+.0f} ms from the cast", h.mgef, h.ms - a_ms);
            }
            if (!sameByOther)
                if (const auto fx = EffectMirror::Get(c.hostile))
                    for (const auto& f : fx->fx)
                        if (f.spell && f.Live() && IsBlastForm(c, f.spell->GetFormID())) return "its effect is live on the foe";
            return {};
        }
        float FoeHealth(Ctx& c) {
            auto* avo = c.hostile ? c.hostile->AsActorValueOwner() : nullptr;
            return avo ? avo->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
        }
        // One ch.24 dev claim: strict where this install serves it, else the plain point hold of the g0b tests. nullptr = filed.
        const char* FileOrder(RE::FormID a_id, const RE::NiPoint3& a_pt, bool* a_strict = nullptr) {
            const bool strict = APMFBridge::DevStrictServe(a_id, a_pt, kB.radius).empty();
            if (a_strict) *a_strict = strict;
            return APMFBridge::DevHoldFile(a_id, a_pt, strict ? kB.radius : kB.radiusPlain, strict);
        }
        // A re-point keeps the claim's own kind; the radius follows it.
        bool Repoint(RE::FormID a_id, const RE::NiPoint3& a_pt) {
            return APMFBridge::DevHoldRepoint(a_id, a_pt, APMFBridge::DevStrictServe(a_id, a_pt, kB.radius).empty() ? kB.radius : kB.radiusPlain);
        }
        void HoldOthers(Ctx& c, SelfBlastRun& g) {
            for (const auto id : OtherFollowers(c.id)) {
                auto it = std::find_if(g.held.begin(), g.held.end(), [&](const SbHeld& h) { return h.id == id; });
                if (it == g.held.end()) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                    if (!a || a->IsOnMount()) continue;
                    SbHeld h{ id, a->GetPosition(), 0, c.elapsed };
                    const char* no = FileOrder(id, h.pt);
                    spdlog::info("[anim] self_blast: {:08X} held where he stands: {}", id, no ? no : "filed");
                    g.held.push_back(h);
                } else if (c.elapsed - it->at >= 2.0f && it->refiles < kB.refiles) {
                    const auto info = APMFBridge::DevHoldRead(id);
                    if (info.stands && !info.ended) continue;
                    APMFBridge::DevHoldRelease(id);   // Harbinger ended it (his fight ended): once more, bounded
                    ++it->refiles;
                    it->at = c.elapsed;
                    (void)FileOrder(id, it->pt);
                }
            }
        }
        void SetHeldPoint(SelfBlastRun& g, RE::FormID a_id, const RE::NiPoint3& a_pt) {
            for (auto& h : g.held) if (h.id == a_id) { h.pt = a_pt; h.refiles = 0; }
            if (!Repoint(a_id, a_pt)) { APMFBridge::DevHoldRelease(a_id); (void)FileOrder(a_id, a_pt); }
        }

        const char* SelfBlastPre(Ctx& c) {
            if (const char* r = Ready(c)) return r;
            auto& s = *c.s;
            s.set[0].clear();
            s.note.clear();
            const auto& sc = Scan();
            if (sc.areaBlasts.empty()) {
                s.note = sc.unservedWhy ? std::string(sc.unservedWhy) : std::string("no self hostile area blast a tome teaches (and IAUS offers) in this load order");
                return nullptr;   // Step SKIPs with it
            }
            RE::FormID pick = Affordable(c, sc.areaBlasts);
            if (!pick) pick = sc.areaBlasts.front();
            s.set[0] = { pick };
            return nullptr;
        }
        void SelfBlastFix(Ctx& c, FixtureRequest& r) {
            if (!Spell(c) || NoLog() || NoHarbinger() || !Config::g_iausAvailable.load()) return;
            Lend(c, r, 3.0f);
            Foe(r);   // at the usual distance: the fight must exist for the ch.24 order; the foe is set down inside the radius once he stands at the point
            Snap(c, r);
            r.iaus = 1;
        }
        Step SelfBlastStep(Ctx& c) {
            auto& s = *c.s;
            if (const char* why = NoLog()) return Step::Skip(why);
            if (!Config::g_iausAvailable.load()) return Step::Skip("bIausAvailable = 0");
            if (const char* why = NoHarbinger()) return Step::Skip(why);
            if (!Spell(c)) return Step::Skip(s.note.empty() ? "no self hostile area blast to lend" : s.note);
            if (!s.b[0]) { Begin(c); TempClass(c, "evoker"); g_selfBlast[c.id] = {}; return Step::Continue(); }
            auto& g = g_selfBlast[c.id];
            NoteBlastProxy(c);
            const float dt = std::max(0.0f, c.elapsed - g.lastElapsed);
            g.lastElapsed  = c.elapsed;
            const auto to = [&](SbPhase p) { g.phase = p; g.phaseAt = c.elapsed; g.secs = 0; g.ticket = 0; };
            const float in = c.elapsed - g.phaseAt;
            if (c.elapsed - g.careAt >= kB.careEvery) { g.careAt = c.elapsed; PostCare(c.id); }
            HoldOthers(c, g);
            if (const auto un = Unanimated(c); !un.empty()) return End(Step::Fail("UNANIMATED: " + un + "; " + s.note));

            const auto  book = Iaus::SpellBook::Read(c.follower);
            const auto* e    = BlastEntry(book, Spell(c));
            if (!e) {
                if (g.phase == SbPhase::Book && c.elapsed >= kB.bookWait) return Step::Retry(std::format("the lent spell never came into his IAUS book in {:.0f} s", kB.bookWait));
                return Step::Continue();   // the book refreshes within ~5 s of the lend
            }
            const float guard = e->areaUnits, foeR = e->FoeUnits(), reach = guard + kB.reachPad, pad = guard + Iaus::AreaAllyPad(*e);
            const float byNeed = guard + Iaus::Estimate::kFireBodyPad + Iaus::AreaBystanderMargin(*e);
            const float need   = pad + std::max(Iaus::kAreaGuard.safetyMargin, kB.clearPad);   // guard + 64 + 384 + 128
            const float near   = NearestFriend(c);
            const auto  by     = BystanderWhy(c, guard);
            const bool  clear  = near > pad + kB.clearPad && by.empty();
            const float foeD   = c.hostile ? c.hostile->GetPosition().GetDistance(c.follower->GetPosition()) : -1.0f;
            const bool  foeIn  = c.hostile && foeD <= foeR;
            const bool  canPay = Cost(SpellForm(c), c.follower) <= Pool(c.follower);
            const auto  st     = APMFBridge::DevStrictRead(c.id);
            const auto  hold   = APMFBridge::DevHoldRead(c.id);
            const auto  facts  = std::format("{} guard {:.0f}u, foe radius {:.0f}u, party pad edge {:.0f}u, nearest friend {:.0f}u, foe {:.0f}u{}, order {} ({}{}), {} placement(s){}{}", Facts(c), guard,
                                             foeR, pad, near, foeD, by.empty() ? std::string{} : ", " + by, g.filed ? (g.strict ? "strict" : "plain hold") : "none", hold.state,
                                             g.strict ? std::format(", {}", APMFBridge::DevStrictPhaseName(st.phase)) : std::string{}, g.placements,
                                             g.placeWhy.empty() ? "" : ": ", g.placeWhy);
            s.note = facts;
            const auto claimLine = Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)), "SELF BLAST");

            // ── every phase: a cast while a friend has been inside the pad is the guard failing (a charge in flight excepted) ──
            const auto [fires, lastFire] = BlastFires(c);
            const bool newFire = fires > g.firesSeen;
            g.firesSeen = fires;
            if (newFire && g.padSecs >= kB.grace + 1.0f)
                return End(Step::Fail(std::format("he cast with a friend inside the pad for {:.1f} s (edge {:.0f}u); {}", g.padSecs, pad, facts)));
            g.padSecs = near <= pad - kB.clearPad ? g.padSecs + dt : 0.0f;
            if (newFire && claimLine.empty()) return End(Step::Fail("he cast it without a SELF BLAST claim ([buff] line): " + facts));

            const bool afterLeg1 = g.phase > SbPhase::Leg1Read;
            if (!c.inCombat) {
                if (!afterLeg1) return Step::Retry("the fight ended first; " + facts);
                return End(Step::Pass(g.leg1Note + "; " + (g.leg2Note.empty() ? "leg 2 not judged: the fight ended" : g.leg2Note) + "; leg 3 not judged: the fight ended; " + facts));
            }
            const auto endNoLeg2 = [&](const std::string& why) { return End(Step::Pass(g.leg1Note + "; leg 2 not judged: " + why + "; leg 3 not judged (it follows leg 2); " + facts)); };

            // ── the order: Harbinger ended it (the fight paused) = file it again, bounded ──
            if (g.filed && (!hold.stands || hold.ended) && c.elapsed - g.refileAt >= 2.0f && g.refiles < kB.refiles) {
                APMFBridge::DevHoldRelease(c.id);
                ++g.refiles;
                g.refileAt = c.elapsed;
                (void)FileOrder(c.id, g.pt, &g.strict);
            }
            // ── the foe inside the hostile radius, set down on the main thread when it is not (only once he stands clear) ──
            if (g.placeTicket)
                if (auto p = TakeSpot(g.placeTicket)) {
                    g.placeTicket = 0;
                    g.placeWhy    = p->why;
                    if (p->ok && c.hostile) PostEngage(c.hostile->GetFormID(), c.id);
                    else if (p->why.starts_with("the player is fought")) {
                        if (!afterLeg1) return Step::Retry(p->why + ": the test yields; " + facts);
                        return End(Step::Pass(g.leg1Note + "; leg 2 / 3 not judged further: " + p->why + "; " + facts));
                    }
                }
            g.farSecs = c.hostile && foeD > 0.85f * foeR ? g.farSecs + dt : 0.0f;
            if (g.phase >= SbPhase::Leg1 && !g.placeTicket && c.hostile && g.farSecs >= kB.foeFar && g.placements < kB.placements && c.elapsed - g.placeAt >= kB.placeEvery) {
                ++g.placements;
                g.placeAt = c.elapsed;
                g.placeTicket = PostSpot([foe = c.hostile->GetFormID(), id = c.id, d = std::clamp(foeR * 0.5f, 80.0f, 450.0f), m = std::max(foeR * 0.85f, 100.0f)]() { return PlaceFoe(foe, id, d, m); });
            }

            switch (g.phase) {
            case SbPhase::Book:
                if (!c.hostile) return Step::Continue();
                to(SbPhase::Plan);
                return Step::Continue();
            case SbPhase::Plan: {
                if (!g.ticket) {
                    g.ticket = PostSpot([id = c.id, foe = c.hostile ? c.hostile->GetFormID() : RE::FormID{}, fr = OtherFollowers(c.id), need, byNeed]() { return PlanFar(id, foe, fr, need, byNeed); });
                    return Step::Continue();
                }
                auto p = TakeSpot(g.ticket);
                if (!p) return in > kB.planWait ? Step::Retry("the main-thread planner did not answer; " + facts) : Step::Continue();
                if (!p->ok && p->why.starts_with("the player is fought")) return Step::Retry(p->why + ": the test yields; " + facts);
                if (!p->ok) return End(Step::Skip("the blast's guard cannot be reached here: " + p->why + "; " + facts));
                ++g.plans;
                g.pt      = p->pt;
                g.planWhy = p->why;
                const char* no = nullptr;
                if (g.filed && hold.stands && !hold.ended) { if (!Repoint(c.id, g.pt)) no = "the kept claim is gone"; }
                else { APMFBridge::DevHoldRelease(c.id); no = FileOrder(c.id, g.pt, &g.strict); }
                spdlog::info("[anim] self_blast: {:08X} point {:.0f},{:.0f},{:.0f} ({}): {}", c.id, g.pt.x, g.pt.y, g.pt.z, p->why, no ? no : g.strict ? "strict order" : "plain point hold");
                if (no) return End(Step::Skip(std::format("no order can take him there: {}; {}; {}", no, p->why, facts)));
                g.filed    = true;
                g.refileAt = c.elapsed;
                to(SbPhase::Travel);
                return Step::Continue();
            }
            case SbPhase::Travel:
                if (clear) { g.firesMark = fires; g.hpPrev = FoeHealth(c); to(SbPhase::Leg1); return Step::Continue(); }
                if (in >= kB.travelWait) {
                    if (g.plans < kB.plans) { to(SbPhase::Plan); return Step::Continue(); }
                    return Step::Retry(std::format("he did not stand clear of the party in {} tries of {:.0f} s (point {}, he is {:.0f}u from it); {}", g.plans, kB.travelWait, g.planWhy,
                                                   c.follower->GetPosition().GetDistance(g.pt), facts));
                }
                return Step::Continue();
            case SbPhase::Leg1:
                if (fires > g.firesMark) {
                    g.fireMs      = lastFire;
                    g.fireFoeDist = foeD;
                    g.hpBefore    = g.hpPrev;
                    to(SbPhase::Leg1Read);
                    return Step::Continue();
                }
                g.hpPrev = FoeHealth(c);
                if (clear && foeIn && canPay) g.secs += dt;
                if (g.secs >= kT.claimWait)
                    return End(Step::Fail(std::format("no cast in {:.0f} s of a foe inside his radius with nobody within the pad; last [buff]: {}; {}", kT.claimWait,
                                                      Last(Lines(Since(c), "[buff] " + Hex(c.id), Hex(Spell(c)))), facts)));
                return Step::Continue();
            case SbPhase::Leg1Read: {
                const auto proof = FoeHit(c, g.fireMs);
                if (proof.empty() && in < kT.settle) return Step::Continue();
                if (fires > static_cast<int>(Claims(c))) return End(Step::Fail(std::format("{} cast(s) for {} claim(s): more than one cast per claim; {}", fires, Claims(c), facts)));
                if (proof.empty()) {
                    if (!c.hostile || g.fireFoeDist < 0.0f || g.fireFoeDist > foeR) {   // the foe was not (or no longer) in it when the cast was read: no verdict on the hit, wait for another cast
                        if (++g.recasts >= kB.recasts) return Step::Retry(std::format("{} cast(s), the foe never inside the radius at the read; {}", g.recasts, facts));
                        g.firesMark = fires;
                        to(SbPhase::Leg1);
                        return Step::Continue();
                    }
                    return End(Step::Fail(std::format("the foe {:.0f}u away at the cast was not hit by the blast (no hit event, effect apply or live effect of it; his health {:.0f} -> {:.0f}); {}",
                                                      g.fireFoeDist, g.hpBefore, FoeHealth(c), facts)));
                }
                g.leg1Note = std::format("leg 1 PASS: claim line, {} cast(s) for {} claim(s), foe hit by the blast ({}; foe {:.0f}u away, health {:.0f} -> {:.0f}), no UNANIMATED, {}", fires, Claims(c),
                                         proof, g.fireFoeDist, g.hpBefore, FoeHealth(c), g.planWhy);
                spdlog::info("[anim] self_blast: {:08X} {}", c.id, g.leg1Note);
                to(SbPhase::RingPlan);
                return Step::Continue();
            }
            // ── leg 2: another follower in the ring (past the blast's reach, inside the party pad) ──
            case SbPhase::RingPlan: {
                if (!g.ally) {
                    float best = 1.0e9f;
                    for (const auto& h : g.held)
                        if (auto* a = RE::TESForm::LookupByID<RE::Actor>(h.id); a && !a->IsDead() && a->Is3DLoaded())
                            if (const float d = a->GetPosition().GetDistance(c.follower->GetPosition()); d < best) { best = d; g.ally = h.id; }
                    if (!g.ally) return endNoLeg2("no second follower in the party (the player is never asked to move)");
                }
                if (!g.ticket) {
                    g.ticket = PostSpot([a = g.ally, id = c.id, lo = reach + 48.0f, hi = pad - kB.clearPad]() { return PlanRing(a, id, lo, hi); });
                    return Step::Continue();
                }
                auto p = TakeSpot(g.ticket);
                if (!p) return in > kB.planWait ? endNoLeg2("the main-thread planner did not answer") : Step::Continue();
                if (!p->ok) return endNoLeg2(p->why);
                spdlog::info("[anim] self_blast: {:08X} sent into the pad ring of {:08X}: {}", g.ally, c.id, p->why);
                for (const auto& h : g.held) if (h.id == g.ally) g.home = h.pt;
                SetHeldPoint(g, g.ally, p->pt);
                to(SbPhase::RingWalk);
                return Step::Continue();
            }
            case SbPhase::RingWalk:
            case SbPhase::Dwell: {
                if (near <= guard) {   // never leave anybody inside the blast itself: he goes back, the leg is not judged
                    SetHeldPoint(g, g.ally, g.home);
                    return endNoLeg2(std::format("a friend came {:.0f}u close, inside the blast itself: sent back", near));
                }
                const bool inRing = near > reach && near <= pad - 16.0f;
                if (g.phase == SbPhase::RingWalk) {
                    if (inRing) { g.graceTaken = false; g.ringSecs = 0; to(SbPhase::Dwell); return Step::Continue(); }
                    if (in >= kB.allyWait) {
                        const auto ai = APMFBridge::DevHoldRead(g.ally);
                        return endNoLeg2(std::format("{:08X} did not reach the ring in {:.0f} s (his order: {}, nearest friend {:.0f}u, ring {:.0f} to {:.0f}u)", g.ally, kB.allyWait, ai.state, near, reach, pad));
                    }
                    return Step::Continue();
                }
                if (g.graceTaken && newFire)
                    return End(Step::Fail(std::format("he cast while a friend stood {:.0f}u away, inside the pad (edge {:.0f}u); {}", near, pad, facts)));
                if (in >= kB.leg2Max) return endNoLeg2(std::format("no {:.0f} s in a row with the ally in the ring, the foe in the radius and the magicka for it in {:.0f} s", kB.dwell, kB.leg2Max));
                if (!inRing) { g.ringSecs = 0; g.graceTaken = false; return Step::Continue(); }
                g.ringSecs += dt;
                if (!g.graceTaken) {
                    if (g.ringSecs >= kB.grace) { g.graceTaken = true; g.secs = 0; }
                    return Step::Continue();
                }
                if (foeIn && canPay && by.empty()) g.secs += dt;   // only time in which he would otherwise cast proves a hold
                if (g.secs < kB.dwell) return Step::Continue();
                if (Actuation::StandingSelfBlast(c.id)) return End(Step::Fail(std::format("the blast claim still stands {:.0f} s after a friend came inside the pad (not released); {}", g.ringSecs, facts)));
                g.leg2Note = std::format("leg 2 PASS: no cast in {:.0f} s with {:08X} in the pad ({:.0f}u, edge {:.0f}u), the foe in the radius, no claim left standing; last guard line: {}", g.secs, g.ally,
                                         near, pad, Last(Lines(Since(c), "[cast-guard] " + Hex(c.id), "self blast party hold")));
                SetHeldPoint(g, g.ally, g.home);
                to(SbPhase::Back);
                return Step::Continue();
            }
            // ── leg 3: clear again, the foe remains: he casts ──
            case SbPhase::Back:
                if (clear) { g.firesMark = fires; to(SbPhase::Leg3); return Step::Continue(); }
                if (in >= kB.allyWait) return End(Step::Pass(g.leg1Note + "; " + g.leg2Note + std::format("; leg 3 not judged: not clear again in {:.0f} s; ", kB.allyWait) + facts));
                return Step::Continue();
            case SbPhase::Leg3:
                if (fires > g.firesMark) return End(Step::Pass(g.leg1Note + "; " + g.leg2Note + "; leg 3 PASS: cast once the pad was clear; " + facts));
                if (clear && foeIn && canPay) g.secs += dt;
                if (g.secs >= kB.leg3Cast)
                    return End(Step::Fail(std::format("no cast in {:.0f} s clear of the pad again with the foe inside his radius; ", kB.leg3Cast) + g.leg2Note + "; " + facts));
                return Step::Continue();
            }
            return Step::Continue();
        }
        Step SelfBlastTimeout(Ctx& c) {
            const auto it = g_selfBlast.find(c.id);
            if (it == g_selfBlast.end() || it->second.leg1Note.empty()) return Step::Retry("leg 1 did not finish in the window; " + c.s->note);
            const auto& g = it->second;
            return End(Step::Pass(g.leg1Note + "; " + (g.leg2Note.empty() ? "leg 2 not judged: the window ended" : g.leg2Note) + "; leg 3 not judged: the window ended; " + c.s->note));
        }

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
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESSpellCastEvent>(CastSink::GetSingleton());
            holder->AddEventSink<RE::TESHitEvent>(BlastHitSink::GetSingleton());
            holder->AddEventSink<RE::TESMagicEffectApplyEvent>(BlastHitSink::GetSingleton());
        }
    }

    // WORKER, every way a run ends: the OTHER followers' dev claims of anim.self_blast (his own is HoldEnd's).
    void AnimEnd(RE::FormID a_follower) {
        const auto it = g_selfBlast.find(a_follower);
        if (it == g_selfBlast.end()) return;
        for (const auto& h : it->second.held) APMFBridge::DevHoldRelease(h.id);
        g_selfBlast.erase(it);
    }

    void ClearAnimState() {
        g_summonWait.clear();
        g_selfBlast.clear();
        g_pools = {};
        {
            std::lock_guard lk(g_sbMx);
            g_sbSpots.clear();
        }
        {
            std::lock_guard lk(g_blastMx);
            g_blastHits.clear();
        }
        std::lock_guard lk(g_fireMx);
        g_fires.clear();
    }

    void RegisterAnim() {
        const char* ref = "anim-residue plan / placed-spells";
        Register({ "anim.summon_iaus", ref, "(any follower) a lent location summon, Sentient AI on, a spawned fight", 120.0f, SummonPre, SummonFixIaus, SummonIaus, AnyTimeout, true });
        Register({ "anim.fire_once", ref, "(any follower) a lent ambiguous-row buff (SKIP when the load order has none)", 150.0f, FireOncePre, FireOnceFix, FireOnceStep, AnyTimeout, true });
        Register({ "anim.party_gap", ref, "(any follower) a lent buff while only the player fights", 80.0f, GapPre, GapFix, GapStep, AnyTimeout, true });
        Register({ "anim.party_gap_conc", ref, "(any follower) the same with Lesser Ward (concentration) against a spell-casting foe", 80.0f, GapConcPre, GapConcFix, GapStep, AnyTimeout, true });
        Register({ "anim.blocked_blast", ref, "(any follower) a lent self hazard spell (the arm M4 keeps out), Sentient AI on", 70.0f, BlastPre, BlastFix, BlastStep, BlastTimeout, true });
        Register({ "anim.self_blast", ref, "(any follower, any room that is not an inhabited place) a lent self hostile area blast; the test walks him clear of the party, sets a foe inside its radius, then sends a second follower into the pad", 330.0f, SelfBlastPre, SelfBlastFix, SelfBlastStep, SelfBlastTimeout, true });
        Register({ "anim.summon_gambit", ref, "(any follower) the summon through a gambit row (after the first creature is gone)", 120.0f, SummonPre, SummonFixGambit, SummonGambit, AnyTimeout, true });
    }
    void RegisterAnimSession() {
        Register({ "anim.session", "anim-residue plan", "every other test finished or blocked: counts the session's UNANIMATED lines by reason", 20.0f, SessionPre, nullptr, SessionStep, nullptr });
    }

}
