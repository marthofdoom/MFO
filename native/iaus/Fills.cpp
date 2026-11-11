// iaus/Fills.cpp -- the engine side of the phase-2 estimates (contract, switches and threading in iaus/Fills.h).
#include "iaus/Fills.h"

#include "Config.h"
#include "Followers.h"
#include "Runtime.h"
#include "iaus/Reach.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

// One Win32 symbol, declared by hand: <windows.h> is banned outside Board.cpp (Diagnostics.cpp says why).
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId();

namespace MFO::Iaus::Fills {

    namespace {

        using Clock = std::chrono::steady_clock;
        using AV    = RE::ActorValue;

        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

        double NowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
        bool   On(const std::atomic<bool>& a_b) { return a_b.load(std::memory_order_relaxed); }

        // ── GMSTs: the Setting objects live for the session, so the pointer is resolved once (MAIN, kDataLoaded) and
        // the float is read live (a plain load), which follows a plugin or a script that changes it. ──────────────────
        enum Gm : std::size_t {
            kDamageWeaponMult = 0, kDamageSkillMin, kDamageSkillMax, kDamagePCSkillMin, kDamagePCSkillMax,
            kArmorScalingFactor, kMaxArmorRating, kDpsMeleeSpeedMult, kDpsBowSpeedMult, kCombatMagickaRegenMult, kGmCount
        };
        constexpr const char* kGmName[kGmCount] = {
            "fDamageWeaponMult", "fDamageSkillMin", "fDamageSkillMax", "fDamagePCSkillMin", "fDamagePCSkillMax",
            "fArmorScalingFactor", "fMaxArmorRating", "fCombatDPSMeleeSpeedMult", "fCombatDPSBowSpeedMult",
            "fCombatMagickaRegenRateMult"
        };
        std::atomic<RE::Setting*> g_gm[kGmCount]{};
        std::atomic<bool>         g_epOk{ false };   // the HandleEntryPoint row verified (StartupCheck)
        // Review F3: the thread TESHitEvent arrives on is NOT known (CrimeProbe.cpp treats it so). The sink reads the
        // target's health only when it runs on THIS thread (captured at kDataLoaded); 0 = not captured, never equal.
        std::atomic<std::uint32_t> g_mainThread{ 0 };

        // NaN = the GMST is missing or not finite: the estimate that needs it is then NOT produced.
        float G(Gm a_g) {
            const auto* s = g_gm[a_g].load(std::memory_order_relaxed);
            const float v = s ? s->GetFloat() : kNaN;
            return std::isfinite(v) ? v : kNaN;
        }

        // ── the per-actor tracker (WORKER domain, #4): the health sample of the last lap, the observed loss rate, the
        // probe's rate limits and edges. Pruned by age in Finish, emptied by Clear. ─────────────────────────────────────
        // Sized from the cadence (principle 9): an actor in a fight is sampled once per IAUS follower lap (133 ms x
        // party, about 1.3 s for ten).
        constexpr double kLossTauSec   = 2.0;    // the lab's window (tools/iaus_sim/Policy.h: recent damage over 2 s)
        constexpr double kLossMinDtSec = 0.10;   // two laps closer than this share one sample (several followers see one foe)
        constexpr double kLossResetSec = 10.0;   // not sampled for this long (seven missed laps of a party of ten): start again
        constexpr double kTrackKeepSec = 60.0;   // a tracker entry not sampled for a minute is dropped
        constexpr double kUnitLogSec   = 10.0;   // one `[iaus-fill] unit` line per actor per this long
        constexpr double kHitKeepSec   = 10.0;   // a recorded hit whose target no lap sampled for this long is dropped (counted)

        struct Track {
            double t = 0.0;            // when hp was sampled (0 = never)
            float  hp = 0.0f;
            float  loss = 0.0f;        // health per second, exponentially weighted over kLossTauSec
            double unitLogged = 0.0;
            bool   swinging = false, casting = false;
            double swingStart = 0.0, castStart = 0.0;   // the last rising edge (the measured cadence in the edge lines)
        };
        std::unordered_map<RE::FormID, Track> g_track;
        std::unordered_map<RE::FormID, int>   g_mortalLogged;   // follower -> the flag set last logged
        double                                g_pruned = 0.0;

        struct Bucket {   // WORKER domain: at most `burst` lines at once, `perSec` on average
            double tokens, burst, perSec, t = 0.0;
            std::uint32_t skipped = 0;
            bool Take(double a_now) {
                if (t != 0.0) tokens = (std::min)(burst, tokens + (a_now - t) * perSec);
                t = a_now;
                if (tokens < 1.0) { ++skipped; return false; }
                tokens -= 1.0;
                return true;
            }
        };
        Bucket g_hitLines{ 6.0, 6.0, 3.0 };
        Bucket g_edgeLines{ 6.0, 6.0, 3.0 };

        // ── the hit records (LEAF mutex: the sink writes, the worker drains) ─────────────────────────────────────────
        struct HitRec {
            double        t = 0.0;
            RE::FormID    target = 0, cause = 0, source = 0, projectile = 0;
            std::uint8_t  flags = 0;
            float         hpEvt = kNaN;   // the target's health as the sink read it
            std::uint32_t thread = 0;
        };
        constexpr std::size_t kHitCap = 128;
        std::mutex            g_hitMx;
        std::vector<HitRec>   g_hits;
        std::uint32_t         g_hitOverflow = 0;   // under g_hitMx
        std::uint32_t         g_hitAged = 0;       // WORKER

        // ── ruling Q68: the hits a FOLLOWER caused, newest last (under g_hitMx). A ring, so nothing has to drain it:
        // the swing that reads it asks "did he hit his target since this swing began". Sized from the cadence
        // (principle 9): ten followers at one blow a second over a 1.3 s lap are 13 records between two reads.
        struct Caused { double t = 0.0; RE::FormID cause = 0, target = 0, source = 0; std::uint8_t flags = 0; };
        constexpr std::size_t kCausedCap = 64;
        Caused      g_caused[kCausedCap]{};
        std::size_t g_causedHead = 0;

        // The follower's swing in the air and his learners (WORKER domain, #4).
        // A swing is OPENED on the lap his attack state enters Draw / Swing and RESOLVED on the first lap it has left
        // them (the hit frame is behind him then, and TESHitEvent is sent from the hit itself), or after kSwingCapSec
        // (a chain of swings the laps never saw apart). Sized from the cadence: a lap is 133 ms x party, a swing is
        // 0.5 - 1.2 s from its start to the hit.
        constexpr double kSwingCapSec = 2.5;
        // Review F4: TESHitEvent can arrive AFTER a lap has seen the attack state leave Swing. A swing is resolved only
        // once he has been out of Draw / Swing for this long (or swings again, or the cap), and the next swing's window
        // starts no earlier than that resolution, so a late hit is never credited to the swing after it.
        constexpr double kSwingGraceSec = 0.3;
        struct ReachState {
            bool   open = false;
            double t0 = 0.0, windowStart = 0.0;
            double leftAt = 0.0;        // the first lap that saw the open swing out of Draw / Swing (0 = still in them)
            double resolvedAt = 0.0;    // when the last swing was resolved: the floor of the next window
            float  dist = 0.0f;
            RE::FormID target = 0, weapon = 0, weaponLeft = 0;
            bool   wasSwinging = false;
            double lastLap = 0.0;
        };
        std::unordered_map<RE::FormID, ReachState>         g_reach;   // by follower
        std::unordered_map<std::uint64_t, Reach::Learner>  g_learn;   // by (follower, weapon): runtime only (Q68)
        Bucket g_swingLines{ 6.0, 6.0, 3.0 };

        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static HitSink* GetSingleton() { static HitSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_ev, RE::BSTEventSource<RE::TESHitEvent>*) override {
                const bool probe = On(Config::g_iausFillProbe);
                if ((!probe && !On(Config::g_iausReachLearn)) || !a_ev || !a_ev->target) return RE::BSEventNotifyControl::kContinue;
                auto* target = a_ev->target->As<RE::Actor>();
                if (!target) return RE::BSEventNotifyControl::kContinue;
                HitRec r;
                r.target = target->GetFormID();
                r.cause  = a_ev->cause ? a_ev->cause->GetFormID() : 0;
                // A follower hit something or was hit (the any-thread FormID mirror, #74; never the live lists).
                const bool byFollower = r.cause != 0 && Followers::IsTrackedFast(r.cause);
                if (!byFollower && !Followers::IsTrackedFast(r.target)) return RE::BSEventNotifyControl::kContinue;
                r.t          = NowSec();
                r.source     = a_ev->source;
                r.projectile = a_ev->projectile;
                r.flags      = static_cast<std::uint8_t>(a_ev->flags.underlying());
                r.thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
                // The at-event health: an actor-value read, so ONLY on the main thread (review F3). Anywhere else it
                // stays NaN and the line still has the worker's before / after samples.
                if (probe && r.thread != 0 && r.thread == g_mainThread.load(std::memory_order_relaxed))
                    if (auto* avo = target->AsActorValueOwner()) r.hpEvt = avo->GetActorValue(AV::kHealth);
                std::scoped_lock lk(g_hitMx);
                if (byFollower) {
                    g_caused[g_causedHead] = Caused{ r.t, r.cause, r.target, r.source, r.flags };
                    g_causedHead = (g_causedHead + 1) % kCausedCap;
                }
                if (!probe) return RE::BSEventNotifyControl::kContinue;   // the probe's list is drained only by the probe
                if (g_hits.size() >= kHitCap) { ++g_hitOverflow; return RE::BSEventNotifyControl::kContinue; }
                g_hits.push_back(r);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ── what an effect does to health (FORM DATA ONLY: any thread) ───────────────────────────────────────────────
        enum class Fx : std::uint8_t { None = 0, Damage, Heal, Control };

        Fx Classify(const RE::EffectSetting* a_b) {
            using Arch = RE::EffectArchetypes::ArchetypeID;
            using Flag = RE::EffectSetting::EffectSettingData::Flag;
            switch (a_b->GetArchetype()) {
            case Arch::kParalysis:
            case Arch::kCalm:
            case Arch::kDemoralize:
            case Arch::kFrenzy:
            case Arch::kTurnUndead:
                return Fx::Control;
            case Arch::kAbsorb:
            case Arch::kValueModifier:
            case Arch::kDualValueModifier: {
                if (a_b->data.primaryAV != AV::kHealth) return Fx::None;
                if (a_b->data.flags.all(Flag::kRecover)) return Fx::None;   // the value comes back when it ends: not damage, not a heal
                if (a_b->GetArchetype() == Arch::kAbsorb || a_b->IsHostile() || a_b->IsDetrimental()) return Fx::Damage;
                return a_b->GetArchetype() == Arch::kValueModifier ? Fx::Heal : Fx::None;
            }
            default:
                return Fx::None;
            }
        }

        // One effect's health total from a magnitude: a concentration effect is counted for ONE second (its cost is
        // per second too), an instant one once, one with a duration for its duration (the per-second reading of a
        // damage-over-time effect is the research note's 2.2: UNPROVEN until the hit line shows it).
        float Total(const RE::Effect* a_e, float a_mag, bool a_conc) {
            const std::uint32_t dur = a_e->GetDuration();
            const float v = a_mag * ((a_conc || dur == 0) ? 1.0f : static_cast<float>(dur));
            return (std::isfinite(v) && v > 0.0f) ? v : 0.0f;
        }

        // The raw record health damage of one cast of a_item (no perks, no resist). WORKER-safe: form data only.
        float RawDamage(const RE::MagicItem* a_item) {
            if (!a_item) return 0.0f;
            const bool conc = a_item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            float sum = 0.0f;
            for (const auto* e : a_item->effects) {
                if (!e || !e->baseEffect || Classify(e->baseEffect) != Fx::Damage) continue;
                sum += Total(e, e->GetMagnitude(), conc);
            }
            return sum;
        }

        // RULING Q86 (marth 2026-11-11: "no, it's IAUS weighted based on magicka, damage, etc."). A foe's spell threat
        // is NOT a cast rate measured per actor. It is estimated from what can be read: what the spell does (RawDamage),
        // what it costs (the record cost of its effects, no skill or perk reduction), the magicka he has and regains in
        // combat, and its charge time. Casts per second = the smaller of
        //   what the charge time allows   1 / charge (a stream: one "hit" of one second per second), and
        //   what his magicka pays for     (magicka now + regen x horizon) / cost / horizon,
        // and never above kCastCeilPerSec. Regen per second = max magicka x kMagickaRate% x kMagickaRateMult% x
        // fCombatMagickaRegenRateMult. A staff spends its own charge, not magicka: the charge-time bound only.
        constexpr float kCastHorizonSec = 10.0f;   // the seconds the estimate looks ahead (the time-to-live curves read 6 - 12 s)
        constexpr float kCastCeilPerSec = 1.0f;    // nobody is modelled as casting more than once a second

        float RecordCost(const RE::MagicItem* a_item) {
            float sum = 0.0f;
            for (const auto* e : a_item->effects)
                if (e && std::isfinite(e->cost) && e->cost > 0.0f) sum += e->cost;
            return sum;
        }

        float CastsPerSec(const RE::MagicItem* a_item, bool a_staff, RE::ActorValueOwner* a_avo, const Combatant& a_u) {
            const bool  conc   = a_item->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            const float charge = a_item->GetChargeTime();
            float rate = kCastCeilPerSec;
            if (!conc && std::isfinite(charge) && charge > 0.0f) rate = (std::min)(rate, 1.0f / charge);
            if (a_staff) return rate;
            const float cost = RecordCost(a_item);
            if (!(cost > 0.0f)) return rate;   // free to cast: the charge time is the only bound
            float regen = 0.0f;
            {
                const float pct = a_avo->GetActorValue(AV::kMagickaRate), mult = a_avo->GetActorValue(AV::kMagickaRateMult);
                const float inCombat = G(kCombatMagickaRegenMult);   // NaN when missing: the regen term is then left out
                const float r = a_u.mpMax * pct * 0.01f * mult * 0.01f * inCombat;
                if (std::isfinite(r) && r > 0.0f) regen = r;
            }
            const float pays = ((std::max)(0.0f, a_u.mp) + regen * kCastHorizonSec) / cost / kCastHorizonSec;
            return std::isfinite(pays) ? std::clamp((std::min)(rate, pays), 0.0f, kCastCeilPerSec) : 0.0f;
        }

        // ── the weapon-hit estimate (WORKER: AV reads + record fields + GMSTs) ───────────────────────────────────────
        // The engine's core formula (research note 2.1, AE 26410 / SE 25847) WITHOUT tempering, ammunition, the attack
        // data's multiplier and the perk entry points:
        //   ((weapon damage x fDamageWeaponMult) x skillMul + (kMeleeDamage | kUnarmedDamage))
        //   skillMul = fDamage[PC]SkillMin + (Max - Min) x clamp(skill, 0, 100) / 100, 1 when the weapon has no skill.
        // The rate is the engine's own AI model (2.4, AE 26411): weapon speed x fCombatDPS(Melee|Bow)SpeedMult.
        void EstimateHit(RE::Actor* a_actor, RE::ActorValueOwner* a_avo, const Combatant& a_u, Pass::Est& e) {
            auto* r  = a_actor->GetEquippedObject(false);
            auto* l  = a_actor->GetEquippedObject(true);
            auto* w  = r ? r->As<RE::TESObjectWEAP>() : nullptr;
            auto* lw = l ? l->As<RE::TESObjectWEAP>() : nullptr;
            if (!w && lw && lw->IsStaff()) w = lw;
            // Review F1: a second MELEE weapon in the left hand. Its hits carry its own FormID as their source.
            if (lw && lw != w && lw->IsMelee()) e.srcLeft = lw->GetFormID();

            // A staff, or a spell in hand with no weapon: the hit is the spell.
            const RE::MagicItem* magic = nullptr;
            if (w && w->IsStaff()) {
                magic  = w->formEnchanting;
                e.kind = "staff";
                e.src  = w->GetFormID();
            } else if (!w) {
                const RE::MagicItem* rs = r ? r->As<RE::SpellItem>() : nullptr;
                const RE::MagicItem* ls = l ? l->As<RE::SpellItem>() : nullptr;
                magic = RawDamage(rs) >= RawDamage(ls) ? (rs ? rs : ls) : ls;
                if (magic) { e.kind = "spell"; e.src = magic->GetFormID(); }
            }
            if (magic) {
                e.magic = true;
                e.hit   = RawDamage(magic);
                e.rate  = CastsPerSec(magic, w != nullptr, a_avo, a_u);   // ruling Q86
                return;
            }

            const float meleeMult = G(kDpsMeleeSpeedMult);
            if (w) {
                e.kind   = "weapon";
                e.src    = w->GetFormID();
                e.ranged = w->IsBow() || w->IsCrossbow();
                const float mult = G(kDamageWeaponMult);
                const AV    skillAv = w->weaponData.skill.get();
                const int   sv = static_cast<int>(skillAv);
                float skillMul = 1.0f;
                if (sv >= 6 && sv <= 23) {
                    e.skillAv = sv;
                    const float raw = a_avo->GetActorValue(skillAv);
                    e.skill = std::clamp(std::isfinite(raw) ? raw : 0.0f, 0.0f, 100.0f);
                    const bool  pc = a_actor->IsPlayerRef();
                    const float lo = G(pc ? kDamagePCSkillMin : kDamageSkillMin), hi = G(pc ? kDamagePCSkillMax : kDamageSkillMax);
                    skillMul = lo + (hi - lo) * e.skill * 0.01f;   // NaN when a GMST is missing
                }
                float add = 0.0f;
                if (!e.ranged) {
                    add = a_avo->GetActorValue(w->IsHandToHandMelee() ? AV::kUnarmedDamage : AV::kMeleeDamage);
                    if (!std::isfinite(add)) add = 0.0f;
                }
                const float hit  = static_cast<float>(w->GetAttackDamage()) * mult * skillMul + add;
                const float rate = w->GetSpeed() * (e.ranged ? G(kDpsBowSpeedMult) : meleeMult);
                e.hit  = (std::isfinite(hit) && hit > 0.0f) ? hit : 0.0f;
                e.rate = (std::isfinite(rate) && rate > 0.0f) ? rate : 0.0f;
                return;
            }

            // Bare hands or a creature: the race's unarmed damage plus kUnarmedDamage. NOT decoded from the engine
            // (Character vtable 0xE8 CalcUnarmedDamage was only located): an estimate the hit line has to prove.
            e.kind = "unarmed";
            float hit = a_avo->GetActorValue(AV::kUnarmedDamage);
            if (!std::isfinite(hit)) hit = 0.0f;
            if (const auto* race = a_actor->GetRace(); race && std::isfinite(race->data.unarmedDamage)) hit += race->data.unarmedDamage;
            e.hit  = hit > 0.0f ? hit : 0.0f;
            e.rate = (std::isfinite(meleeMult) && meleeMult > 0.0f) ? meleeMult : 0.0f;
        }

        const char* RoleName(Role a_r) {
            switch (a_r) {
            case Role::Self: return "self";
            case Role::Foe:  return "foe";
            case Role::Ally: return "ally";
            }
            return "?";
        }

        std::string_view NameOf(const Snapshot& a_s, std::uint32_t a_id) {
            if (a_id == a_s.self.id) return a_s.self.name;
            for (const auto& f : a_s.foes)
                if (f.id == a_id) return f.name;
            for (const auto& a : a_s.allies)
                if (a.id == a_id) return a.name;
            return "?";
        }

        bool EntryPointOk() {
            // The exact-runtime gate FIRST: an id miss is fatal in the fork, so no id is resolved on an unknown build.
            static const bool ok = Runtime::Known() &&
                                   Runtime::SeatVerified(REL::Relocation<void*>{ REL::RelocationID(23073, 23526) }.address(),
                                                         "TradePerks.BGSEntryPoint.HandleEntryPoint");
            return ok;
        }

    }

    bool CastsSilently(RE::Actor* a_caster, RE::SpellItem* a_spell) {
        if (!a_caster || !a_spell) return false;
        if (!g_epOk.load(std::memory_order_relaxed) || !Runtime::IsVerified1_6_1170()) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true))
                spdlog::warn("[iaus-fill] cast-sound entry point (55) REFUSED by name: needs the verified HandleEntryPoint row on "
                             "1.6.1170 (the only build its call site was disassembled on) -- every cast counts as LOUD while hidden");
            return false;
        }
        float v = 1.0f;   // the engine's seed (0x5BF42A reads 1.0f from the image)
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellCastingSoundEvent, a_caster,
                                            static_cast<RE::MagicItem*>(a_spell), &v);
        return std::isfinite(v) && v == 0.0f;
    }

    void StartupCheck() {
        auto* gsc = RE::GameSettingCollection::GetSingleton();
        std::string vals, missing;
        for (std::size_t i = 0; i < kGmCount; ++i) {
            auto* s = gsc ? gsc->GetSetting(kGmName[i]) : nullptr;
            g_gm[i].store(s, std::memory_order_relaxed);
            if (s) vals += std::format(" {}={:g}", kGmName[i], s->GetFloat());
            else   missing += std::format(" {}", kGmName[i]);
        }
        g_epOk.store(EntryPointOk(), std::memory_order_relaxed);
        g_mainThread.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
        spdlog::info("[iaus-fill] gmst{}", vals);
        if (!missing.empty())
            spdlog::error("[iaus-fill] GMST NOT FOUND:{} -- every estimate that needs one stays unmeasured (0), nothing is guessed",
                          missing);
        spdlog::info("[iaus-fill] probe {} (bIausFillProbe) | inputs written: armor={} hit={} loss={} swing={} casting={} spells={} "
                     "mortal={} (bIausFill<Input>, all default OFF) learned reach={} (bIausReachLearn) | spell magnitude perks "
                     "(EP 0x1D): {} | main thread {}",
                     On(Config::g_iausFillProbe) ? "ON" : "off", On(Config::g_iausFillArmor), On(Config::g_iausFillHit),
                     On(Config::g_iausFillLoss), On(Config::g_iausFillSwing), On(Config::g_iausFillCasting),
                     On(Config::g_iausFillSpells), On(Config::g_iausFillMortal), On(Config::g_iausReachLearn),
                     g_epOk.load(std::memory_order_relaxed) ? "row verified" : "REFUSED (record magnitudes only)",
                     ::GetCurrentThreadId());
    }

    void RegisterHitSink() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
            spdlog::info("[iaus-fill] hit sink registered (live only while bIausFillProbe = 1 or bIausReachLearn = 1)");
        }
    }

    Pass::Pass() {
        m_probe   = On(Config::g_iausFillProbe);
        m_armor   = On(Config::g_iausFillArmor);
        m_hit     = On(Config::g_iausFillHit);
        m_loss    = On(Config::g_iausFillLoss);
        m_swing   = On(Config::g_iausFillSwing);
        m_casting = On(Config::g_iausFillCasting);
        m_mortal  = On(Config::g_iausFillMortal);
        m_reach   = On(Config::g_iausReachLearn);
        m_armed   = m_probe || m_armor || m_hit || m_loss || m_swing || m_casting || m_mortal || m_reach;
        if (m_armed) m_now = NowSec();
    }

    void Pass::Unit(Combatant& a_u, RE::Actor* a_actor, Role a_role) {
        if (!m_armed || !a_actor) return;
        auto* avo = a_actor->AsActorValueOwner();
        if (!avo) return;
        Est e;
        e.id   = a_u.id;
        e.role = a_role;
        e.hp   = a_u.hp;

        // ── mitigation (research note 2.3, AE 44014): kDamageResist x fArmorScalingFactor x 0.01, capped at
        // fMaxArmorRating x 0.01. The armor base factor sum and EP 0x25 are left out (Fills.h).
        {
            const float dr = avo->GetActorValue(AV::kDamageResist);
            const float k = G(kArmorScalingFactor), cap = G(kMaxArmorRating);
            e.dr = std::isfinite(dr) ? dr : 0.0f;
            const float a = e.dr * k * 0.01f;
            e.armor = (std::isfinite(a) && std::isfinite(cap)) ? std::clamp(a, 0.0f, (std::max)(0.0f, cap * 0.01f)) : 0.0f;
            const float mr = avo->GetActorValue(AV::kResistMagic);
            e.magicResist = std::isfinite(mr) ? mr : 0.0f;
        }

        EstimateHit(a_actor, avo, a_u, e);

        // ── mid-swing / mid-cast (plain loads; the combat thread advances both while we look: a stale answer is one
        // lap old, never corrupt -- cast/Hands.cpp CastInFlightOnHand makes the same reads on this worker) ───────────
        if (const auto* st = a_actor->AsActorState()) {
            const auto as = st->GetAttackState();
            e.atkState = static_cast<int>(as);
            e.swinging = !e.magic && !e.ranged && (as == RE::ATTACK_STATE_ENUM::kDraw || as == RE::ATTACK_STATE_ENUM::kSwing);
        }
        RE::FormID castSpell = 0;
        for (std::size_t i = 0; i < RE::Actor::SlotTypes::kTotal; ++i) {
            const RE::MagicCaster* c = a_actor->GetActorRuntimeData().magicCasters[i];
            if (!c) continue;
            const auto cs = c->state.get();
            if (cs == RE::MagicCaster::State::kNone || cs == RE::MagicCaster::State::kUnk08 || cs == RE::MagicCaster::State::kUnk09)
                continue;   // idle, or a cast that is ending (the interrupt / deselect pair)
            e.casting   = true;
            e.castState = static_cast<int>(cs);
            if (const auto* sp = c->currentSpell) castSpell = sp->GetFormID();
            break;
        }

        // ── the observed loss rate and this lap's edges ──────────────────────────────────────────────────────────
        Track& tr = g_track[e.id];
        e.prevT  = tr.t;
        e.prevHp = tr.hp;
        if (std::isfinite(a_u.hpMax) && a_u.hpMax > 0.0f) {
            const double dt = m_now - tr.t;
            if (tr.t == 0.0 || dt > kLossResetSec) {
                tr.loss = 0.0f; tr.t = m_now; tr.hp = a_u.hp;
            } else if (dt >= kLossMinDtSec) {
                const float lost = (std::max)(0.0f, tr.hp - a_u.hp);
                const float k = static_cast<float>(1.0 - std::exp(-dt / kLossTauSec));
                tr.loss += (lost / static_cast<float>(dt) - tr.loss) * k;
                if (!std::isfinite(tr.loss) || tr.loss < 0.0f) tr.loss = 0.0f;
                tr.t = m_now; tr.hp = a_u.hp;
            }
        }
        e.loss = tr.loss;

        if (m_probe && a_role == Role::Foe) {
            if (e.casting && !tr.casting) {
                if (g_edgeLines.Take(m_now))
                    spdlog::info("[iaus-fill] cast-start {} ({:08X}): caster state {} spell {:08X}, {:.2f} s since his last cast start "
                                 "(-1 = first; shown for reading only, ruling Q86: the rate is never taken from it), estimate rate {:.2f}/s",
                                 a_u.name, e.id, e.castState, castSpell, tr.castStart > 0.0 ? m_now - tr.castStart : -1.0, e.rate);
                tr.castStart = m_now;
            }
            if (e.swinging && !tr.swinging) {
                if (g_edgeLines.Take(m_now))
                    spdlog::info("[iaus-fill] swing-start {} ({:08X}): attack state {} at {:.0f} units, {:.2f} s since his last swing "
                                 "start (-1 = first), estimate rate {:.2f}/s",
                                 a_u.name, e.id, e.atkState, a_u.dist, tr.swingStart > 0.0 ? m_now - tr.swingStart : -1.0, e.rate);
                tr.swingStart = m_now;
            }
        }
        tr.casting  = e.casting;
        tr.swinging = e.swinging;

        // ── write what is switched on ────────────────────────────────────────────────────────────────────────────
        if (m_armor) { a_u.armor = e.armor; a_u.magicResist = e.magicResist; }
        if (m_hit) { a_u.hit = e.hit; a_u.hitsPerSec = e.rate; a_u.hitIsMagic = e.magic; a_u.rangedWeapon = e.ranged; }
        if (m_loss) a_u.lossPerSec = e.loss;
        if (m_swing) a_u.swinging = e.swinging;
        if (m_casting) a_u.casting = e.casting;
        m_est.push_back(e);
    }

    void Pass::Finish(Snapshot& a_s, RE::Actor* a_self) {
        if (!m_armed || !a_self) return;

        // ── mortal: he dies for good at 0 health. The engine side: neither essential nor protected (the reference's
        // own flags, which is what the kill path reads). Whether the LIST lets a follower die some other way (a
        // bleedout mod, a death-alternative mod) is question Q77: probe only until it is answered. ──────────────────
        {
            const bool ess = a_self->IsEssential(), prot = a_self->IsProtected();
            const bool mortal = !ess && !prot;
            if (m_mortal) a_s.mortal = mortal;
            if (m_probe) {
                const auto* base = a_self->GetActorBase();
                const bool bEss = base && base->IsEssential(), bProt = base && base->IsProtected();
                const int  sig = 1 + (ess ? 1 : 0) + (prot ? 2 : 0) + (bEss ? 4 : 0) + (bProt ? 8 : 0);
                int& was = g_mortalLogged[a_s.self.id];
                if (was != sig) {
                    was = sig;
                    spdlog::info("[iaus-fill] mortal {} ({:08X}): essential={} protected={} (his base: essential={} protected={}) "
                                 "-> mortal={} | written: {}",
                                 a_s.self.name, a_s.self.id, ess, prot, bEss, bProt, mortal, m_mortal ? "yes" : "no (bIausFillMortal=0)");
                }
            }
        }
        if (m_probe || m_reach) LearnReach(a_s, a_self);
        if (!m_probe) return;

        // ── one unit line per actor per kUnitLogSec ──────────────────────────────────────────────────────────────
        for (const Est& e : m_est) {
            Track& tr = g_track[e.id];
            if (tr.unitLogged != 0.0 && m_now - tr.unitLogged < kUnitLogSec) continue;
            tr.unitLogged = m_now;
            spdlog::info("[iaus-fill] unit {} ({:08X}) {} seen by {:08X}: hit {:.1f} ({} {:08X}, skill av {} = {:.0f}) x {:.2f}/s "
                         "magic={} ranged={} | DR {:.0f} -> armor {:.3f}, magic resist {:.0f} | losing {:.1f}/s | attack state {} "
                         "swinging={} caster state {} casting={} | left out: temper, ammo, attack mult, perks, armor base factors, "
                         "the weapon's own resist value, a spell's element or poison resist, spell cost reductions",
                         NameOf(a_s, e.id), e.id, RoleName(e.role), a_s.self.id, e.hit, e.kind, e.src, e.skillAv, e.skill, e.rate,
                         e.magic, e.ranged, e.dr, e.armor, e.magicResist, e.loss, e.atkState, e.swinging, e.castState, e.casting);
        }

        // ── the hits whose TARGET this lap sampled: the estimate next to the health it took ──────────────────────
        // Every lap that samples an actor drains that actor's hits in the same lap, so a hit is always inside the
        // window (previous sample, this sample] of the lap that prints it.
        std::vector<HitRec> mine;
        std::uint32_t overflow = 0;
        {
            std::scoped_lock lk(g_hitMx);
            overflow = std::exchange(g_hitOverflow, 0u);
            for (auto it = g_hits.begin(); it != g_hits.end();) {
                const bool sampled = std::any_of(m_est.begin(), m_est.end(), [&](const Est& e) { return e.id == it->target; });
                if (sampled) { mine.push_back(*it); it = g_hits.erase(it); }
                else if (m_now - it->t > kHitKeepSec) { ++g_hitAged; it = g_hits.erase(it); }
                else ++it;
            }
        }
        const auto estOf = [&](std::uint32_t a_id) -> const Est* {
            for (const Est& e : m_est)
                if (e.id == a_id) return &e;
            return nullptr;
        };
        for (const HitRec& h : mine) {
            const Est* tgt = estOf(h.target);
            if (!tgt) continue;
            if (!g_hitLines.Take(m_now)) continue;
            const Est* atk = estOf(h.cause);
            int others = 0;
            for (const HitRec& o : mine)
                if (&o != &h && o.target == h.target) ++others;
            // The estimate for THIS hit: the attacker's hit against the target's mitigation. -1 = the attacker was
            // not sampled this lap. "for" names what his estimate was made for: when it is not the hit's source (he
            // swapped, a summon's or a trap's hit carries his id) the ratio says nothing.
            float est = -1.0f;
            if (atk && atk->hit > 0.0f)
                est = atk->magic ? atk->hit * (std::max)(0.0f, 1.0f - tgt->magicResist / 100.0f) : atk->hit * (1.0f - tgt->armor);
            const bool  haveWindow = tgt->prevT > 0.0 && h.t > tgt->prevT;
            const float delta = haveWindow ? tgt->prevHp - tgt->hp : kNaN;
            const std::uint32_t skipped = std::exchange(g_hitLines.skipped, 0u);
            const std::uint32_t aged = std::exchange(g_hitAged, 0u);
            spdlog::info("[iaus-fill] hit {} ({:08X}) -> {} ({:08X}) source {:08X} projectile {:08X} power={} bash={} blocked={} sneak={} | "
                         "health before {:.1f} at-event {:.1f} after {:.1f}: took {:.1f} in {:.2f} s, {} other hit(s) on him in it | "
                         "estimate {:.1f} (hit {:.1f} for {} {:08X}, armor {:.3f}, magic resist {:.0f}) ratio {:.2f} | attacker attack state {} "
                         "| sink thread {} | lines skipped {} hits aged out {} overflow {}",
                         NameOf(a_s, h.cause), h.cause, NameOf(a_s, h.target), h.target, h.source, h.projectile,
                         (h.flags & 1) != 0, (h.flags & 4) != 0, (h.flags & 8) != 0, (h.flags & 2) != 0,
                         haveWindow ? tgt->prevHp : kNaN, h.hpEvt, tgt->hp, delta, haveWindow ? m_now - tgt->prevT : kNaN, others,
                         est, atk ? atk->hit : -1.0f, atk ? atk->kind : "unsampled", atk ? atk->src : 0u, tgt->armor, tgt->magicResist,
                         (est > 0.0f && std::isfinite(delta)) ? delta / est : kNaN, atk ? atk->atkState : -1, h.thread,
                         skipped, aged, overflow);
            overflow = 0;
        }

        // ── prune (worker domain) ────────────────────────────────────────────────────────────────────────────────
        if (m_now - g_pruned > kTrackKeepSec) {
            g_pruned = m_now;
            std::erase_if(g_track, [&](const auto& kv) { return m_now - kv.second.t > kTrackKeepSec; });
        }
    }

    // RULING Q68 (marth 2026-11-11): the swing reach is learned per weapon from what hit and what missed. This is the
    // engine feed of iaus/Reach.h: ONE outcome per swing of HIS melee weapon at his engine combat target.
    //   hit   = a TESHitEvent he caused on that target with that weapon since the swing began (a blocked hit reached);
    //   miss  = the swing ended with no hit of his on anything;
    //   a hit on someone else only = no outcome (it says nothing about the distance to his target);
    //   a hit on his target by his LEFT-hand weapon only = no outcome (review F1: the attack state does not say which
    //   hand swung, and the learner is the right-hand weapon's. A dual wielder's miss stays ambiguous the same way and
    //   is counted for the right-hand weapon: the estimator's stray-miss tolerance carries it).
    // The distance is centre to centre at the lap the swing was first seen, the same measure the swing gate compares
    // (Facts::dist against Facts::reach). ONE learner per (follower, weapon): plain swings and power attacks share it,
    // because a missed swing does not say which it was (the hit's power flag is in the line, so a field log shows whether
    // they need two). Bare hands are not learned (no weapon to key on). Runtime only, emptied on a load.
    void Pass::LearnReach(Snapshot& a_s, RE::Actor* a_self) {
        const Est* me = nullptr;
        for (const Est& e : m_est)
            if (e.id == a_s.self.id) { me = &e; break; }
        if (!me) return;
        ReachState& rs = g_reach[a_s.self.id];
        const bool  melee = !me->magic && !me->ranged && me->src != 0;
        const auto  keyOf = [&](RE::FormID a_weapon) { return (static_cast<std::uint64_t>(a_s.self.id) << 32) | a_weapon; };

        // 1. the open swing has landed or gone by: one outcome, after the grace (review F4)
        if (rs.open && !me->swinging && rs.leftAt == 0.0) rs.leftAt = m_now;
        const bool swingsAgain = rs.open && rs.leftAt != 0.0 && me->swinging;
        if (rs.open && ((rs.leftAt != 0.0 && m_now - rs.leftAt >= kSwingGraceSec) || swingsAgain || m_now - rs.t0 > kSwingCapSec)) {
            rs.open = false;
            rs.leftAt = 0.0;
            rs.resolvedAt = m_now;
            bool hit = false, other = false;
            std::uint8_t flags = 0;
            {
                std::scoped_lock lk(g_hitMx);
                for (const Caused& c : g_caused) {
                    if (c.cause != a_s.self.id || c.t < rs.windowStart) continue;
                    const bool right = c.source == rs.weapon, left = rs.weaponLeft != 0 && c.source == rs.weaponLeft;
                    if (!right && !left) continue;   // a cloak, an enchantment, a spell: not a blow of his weapons
                    if (right && c.target == rs.target) { hit = true; flags = c.flags; }
                    else other = true;               // someone else, or his target by the left-hand weapon (review F1)
                }
            }
            auto it = g_learn.find(keyOf(rs.weapon));
            if (it != g_learn.end() && (hit || !other)) {
                Reach::Learner& L = it->second;
                const float before = L.Value();
                L.Observe(rs.dist, hit);
                if (m_probe && g_swingLines.Take(m_now))
                    spdlog::info("[iaus-fill] swing {} ({:08X}) weapon {:08X} at {:.0f} units on {:08X}: {}{} | learned reach {:.0f} -> "
                                 "{:.0f} (start {:.0f}, gate {:.0f}, {} hits {} misses) | gates the power attack: {}",
                                 a_s.self.name, a_s.self.id, rs.weapon, rs.dist, rs.target, hit ? "HIT" : "miss",
                                 hit ? std::format(" (power={} blocked={})", (flags & 1) != 0, (flags & 8) != 0) : std::string(),
                                 before, L.Value(), L.start, L.Gate(), L.hits, L.misses, m_reach ? "yes" : "no (bIausReachLearn=0)");
            } else if (m_probe && g_swingLines.Take(m_now)) {
                spdlog::info("[iaus-fill] swing {} ({:08X}) weapon {:08X} at {:.0f} units on {:08X}: hit someone else or with the left-hand weapon only, no outcome",
                             a_s.self.name, a_s.self.id, rs.weapon, rs.dist, rs.target);
            }
        }

        // 2. a new swing begins this lap (he was not swinging at the last one)
        if (!rs.open && melee && me->swinging && (!rs.wasSwinging || swingsAgain)) {
            RE::FormID target = 0;
            if (auto tp = a_self->GetActorRuntimeData().currentCombatTarget.get(); tp) target = tp->GetFormID();
            for (const Combatant& f : a_s.foes) {
                if (f.id != target || !std::isfinite(f.dist) || !(f.dist > 0.0f)) continue;
                // The learner starts at fMeleeReach (Snapshot::meleeReach before any override). Precision's capsule
                // length is a weapon LENGTH from the hand, not a centre-to-centre reach, so it is not a start value
                // (Docs/research/2026-11/iaus-i6.md "Precision reach").
                if (g_learn.size() > 512) g_learn.clear();   // a bound, never reached by a real party
                Reach::Learner& L = g_learn[keyOf(me->src)];
                if (!L.Known()) L.Reset(Config::g_meleeReach.load(std::memory_order_relaxed));
                rs.open   = true;
                rs.t0     = m_now;
                // The swing began after the last lap saw him not swinging, so a hit since then belongs to it.
                rs.windowStart = (rs.lastLap > 0.0 && m_now - rs.lastLap < kSwingCapSec) ? rs.lastLap : m_now - 0.5;
                rs.windowStart = (std::max)(rs.windowStart, rs.resolvedAt);   // review F4: never before the last resolution
                rs.leftAt = 0.0;
                rs.dist   = f.dist;
                rs.target = target;
                rs.weapon = me->src;
                rs.weaponLeft = me->srcLeft;
                break;
            }
        }
        rs.wasSwinging = me->swinging;
        rs.lastLap     = m_now;

        // 3. the swing gate's reach: the learned value for the weapon in his hand, in Snapshot::swingReach, which ONLY
        // the swing families read (PowerAttack, Bash: review F5, rulings Q68 / Q70). Snapshot::meleeReach is never
        // touched: the attack's target naming, touch spells, the melee-buff test and the strike distance keep
        // fMeleeReach. No learner yet (he has not swung it) or nothing learnable: swingReach stays 0 and those families
        // read meleeReach (MFO-B687: an unknown learner's Gate() is 0, which would switch the range test off).
        if (m_reach && melee)
            if (auto it = g_learn.find(keyOf(me->src)); it != g_learn.end() && it->second.Known()) {
                const float gate = it->second.Gate();
                if (std::isfinite(gate) && gate > 0.0f) a_s.swingReach = gate;
            }
    }

    void MeasureSpell(RE::Actor* a_caster, RE::SpellItem* a_spell, SpellEntry& a_e, std::string& a_probe,
                      std::uint32_t& a_probeKey) {
        a_probe.clear();
        a_probeKey = 0;
        const bool probe = On(Config::g_iausFillProbe), write = On(Config::g_iausFillSpells);
        if ((!probe && !write) || !a_caster || !a_spell) return;

        const bool conc = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        // The target the magnitude perks are evaluated against (EP 0x1D is (owner, spell, target, &magnitude): research
        // note 2.2, the same call cast/WillWork.cpp makes). A hostile effect: his combat target, held for the calls.
        // No combat target: the entry point is NOT called (a null target is not a proven argument) and the record
        // magnitude stands. A heal: himself.
        RE::NiPointer<RE::Actor> foe = a_caster->GetActorRuntimeData().currentCombatTarget.get();
        if (foe && (foe->IsDead() || !foe->Is3DLoaded())) foe.reset();
        const bool epOk = g_epOk.load(std::memory_order_relaxed);

        float damage = 0.0f, heal = 0.0f, rawDamage = 0.0f, rawHeal = 0.0f;
        std::uint32_t controlSec = 0;
        bool epRan = false;
        // Review F2: an OFFENSE spell whose damage is not in its own effect list (a hazard it spawns, a cloak's second
        // spell, a script, an enchantment it puts on the weapon) has no readable damage here. Writing a measured 0
        // would score a Wall of Flames 0 against his best (MFO-B684), so such a spell stays UNMEASURED.
        bool indirect = false;
        for (const auto* e : a_spell->effects) {
            if (!e || !e->baseEffect) continue;
            switch (e->baseEffect->GetArchetype()) {
            case RE::EffectArchetypes::ArchetypeID::kSpawnHazard:
            case RE::EffectArchetypes::ArchetypeID::kCloak:
            case RE::EffectArchetypes::ArchetypeID::kScript:
            case RE::EffectArchetypes::ArchetypeID::kEnhanceWeapon:
                if (a_e.nature == Prune::Nature::Offense) indirect = true;
                break;
            default: break;
            }
            const Fx fx = Classify(e->baseEffect);
            if (fx == Fx::None) continue;
            if (fx == Fx::Control) { controlSec = (std::max)(controlSec, e->GetDuration()); continue; }
            const float mag0 = e->GetMagnitude();
            float mag = mag0;
            RE::Actor* target = fx == Fx::Damage ? foe.get() : a_caster;
            if (epOk && target && std::isfinite(mag) && mag > 0.0f) {
                RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, a_caster,
                                                    static_cast<RE::MagicItem*>(a_spell),
                                                    static_cast<RE::TESObjectREFR*>(target), &mag);
                if (!std::isfinite(mag) || mag < 0.0f) mag = mag0;   // a perk gave nonsense: the record value stands
                epRan = true;
            }
            (fx == Fx::Damage ? rawDamage : rawHeal) += Total(e, mag0, conc);
            (fx == Fx::Damage ? damage : heal) += Total(e, mag, conc);
        }

        if (write && !indirect) {
            a_e.damage     = damage;
            a_e.heal       = heal;
            a_e.controlSec = static_cast<float>(controlSec);
            a_e.measured   = true;
        }
        if (probe) {
            a_probeKey = (epRan ? 0x46494C31u : 0x46494C30u) ^ (indirect ? 0x100u : 0u);
            a_probe = std::format("[iaus-fill] spell {} ({:08X}) of {} ({:08X}): damage {:.1f} (record {:.1f}) heal {:.1f} (record {:.1f}) "
                                  "control {} s | concentration={} (counted for 1 s) cost {:.1f} | magnitude perks: {} | written: {} | "
                                  "left out: duration perks, dual cast, the target's resist (applied per foe: magic resist and the "
                                  "element, NOT the poison-resist swap of a poison-type spell){}",
                                  a_e.name, a_e.form, a_caster->GetName() ? a_caster->GetName() : "?", a_caster->GetFormID(),
                                  damage, rawDamage, heal, rawHeal, controlSec, conc, a_e.cost,
                                  epRan ? std::format("ran against {:08X}", (rawDamage > 0.0f && foe) ? foe->GetFormID() : a_caster->GetFormID())
                                        : (epOk ? "not run (no combat target)" : "REFUSED (row not verified)"),
                                  indirect ? "indirect damage (hazard / cloak / script / weapon carrier): NOT measured"
                                           : (write ? "yes" : "no (bIausFillSpells=0)"),
                                  indirect ? " | its damage is not in its own effects" : "");
        }
    }

    void Clear() {
        g_track.clear();
        g_mortalLogged.clear();
        g_pruned = 0.0;
        g_hitAged = 0;
        g_reach.clear();
        g_learn.clear();   // Q68: runtime only. FormIDs are save-scoped (a dynamic weapon's id is reused across saves)
        std::scoped_lock lk(g_hitMx);
        g_hits.clear();
        g_hitOverflow = 0;
        for (auto& c : g_caused) c = Caused{};
        g_causedHead = 0;
    }

}
