#include "PCH.h"
#include "StealthProbe.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "Runtime.h"
#include "apmf/APMFBridge.h"   // SenseOf (MAIN THREAD ONLY)
#include "iaus/Hide.h"
#include "iaus/SpellBook.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace MFO::StealthProbe {

    namespace {

        using Clock = std::chrono::steady_clock;
        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

        constexpr double      kSelfSneakPeriodS = 1.0;    // P1: 1 line a second per sneaking follower
        constexpr double      kSelfCombatPeriodS = 5.0;   // P1: a follower in combat who is NOT sneaking (proves the stance dropped): 1 per 5 s
        constexpr double      kSeenPeriodS = 1.0;         // P2: 1 line a second per (foe, follower)
        constexpr std::size_t kMaxFoes = 6;               // P2 per follower per lap
        constexpr std::size_t kHitLinesPerMinute = 20;    // P3: at most 20 "hit" lines a minute per follower
        constexpr double      kHitWindowS = 60.0;
        constexpr double      kSettleS = 0.30;            // the health AFTER a hit is the first sample this long after the event
        constexpr double      kPendingMaxS = 3.0;         // ...or a direct read this long after it (afterSrc=late)
        constexpr double      kPendingDropS = 20.0;       // a pending hit this old (no Service ran) is dropped, counted
        constexpr double      kFoeKeepS = 30.0;           // a foe not sampled for this long loses its health history
        constexpr double      kQ87LogS = 10.0;            // q87 aggregate line: at most once per key per 10 s
        constexpr std::size_t kQ87Cap = 64;               // samples kept per (follower, weapon type, sneak/plain)
        constexpr std::size_t kQ87Need = 5;               // marth Q87: 5 sneak and 5 plain hits per weapon type

        double NowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

        // ── the entry-point row gate (the iaus/Fills.cpp EntryPointOk gate: same row, same exact-runtime check) ──────
        bool EntryPointOk() {
            static const bool ok = Runtime::Known() &&
                                   Runtime::SeatVerified(REL::Relocation<void*>{ REL::RelocationID(23073, 23526) }.address(),
                                                         "TradePerks.BGSEntryPoint.HandleEntryPoint");
            return ok;
        }

        // ── game settings by NAME (never a constant: a list may change any of them) ───────────────────────────────
        float Gmst(const char* a_name) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* s   = gsc ? gsc->GetSetting(a_name) : nullptr;
            return s ? s->GetFloat() : kNaN;
        }

        // The weapon sneak multiplier's setting per WEAPON_TYPE 0..9 (the engine reads a setting table indexed by the weapon's
        // animation type, AE 0x286C50 reads data.animationType at +0x19D; the ORDER of that table is not read, this is the
        // by-name mapping from the setting names in the binary, and the name is printed in every hit line so the field shows it).
        const char* SneakMultName(int a_type) {
            switch (a_type) {
            case 0: return "fCombatSneakHandMult";
            case 1: return "fCombatSneak1HSwordMult";
            case 2: return "fCombatSneak1HDaggerMult";
            case 3: return "fCombatSneak1HAxeMult";
            case 4: return "fCombatSneak1HMaceMult";
            case 5: return "fCombatSneak2HSwordMult";
            case 6: return "fCombatSneak2HAxeMult";
            case 7: return "fCombatSneakBowMult";
            case 8: return "fCombatSneakStaffMult";
            case 9: return "fCombatSneakCrossbowMult";
            default: return nullptr;
            }
        }
        const char* TypeName(int a_type) {
            static constexpr const char* n[] = { "hand", "1h-sword", "1h-dagger", "1h-axe", "1h-mace", "2h-sword", "2h-axe", "bow", "staff", "crossbow" };
            return (a_type >= 0 && a_type <= 9) ? n[a_type] : "none";
        }
        // The weapon behind a hit's source FormID: -1 = not a weapon (a spell, a trap, nothing).
        int WeaponType(RE::FormID a_src, RE::TESObjectWEAP** a_out = nullptr) {
            auto* w = a_src ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(a_src) : nullptr;
            if (a_out) *a_out = w;
            return w ? static_cast<int>(w->GetWeaponType()) : -1;
        }

        // ── per-follower state (WORKER domain, #4: keyed by follower, only Service and Reset touch it) ─────────────
        struct HitWindow {
            std::deque<double> stamps;
            std::uint32_t      suppressed = 0;
            double             lastLine = 0.0;
            bool Take(double a_now) {
                while (!stamps.empty() && a_now - stamps.front() > kHitWindowS) stamps.pop_front();
                if (stamps.size() >= kHitLinesPerMinute) { ++suppressed; return false; }
                stamps.push_back(a_now);
                lastLine = a_now;
                return true;
            }
        };
        struct FState {
            double selfAt = 0.0;
            HitWindow hits;
            std::uint32_t droppedHits = 0;
        };
        std::unordered_map<RE::FormID, FState> g_f;

        struct SeenState {
            double at = 0.0;
            float  stamp = kNaN;
            double stampChangedAt = 0.0;
        };
        std::unordered_map<std::uint64_t, SeenState> g_seen;   // (foe << 32 | follower)

        // health history per foe (WORKER): the sink cannot read health safely, the worker samples every lap
        struct HpSample { double t; float hp; };
        struct HpHist { std::deque<HpSample> s; double last = 0.0; };
        std::unordered_map<RE::FormID, HpHist> g_hp;

        // ── the hit ring (LEAF mutex: the sink writes, the worker drains) ──────────────────────────────────────────
        struct HitRec {
            double        t = 0.0;
            RE::FormID    cause = 0, target = 0, source = 0, projectile = 0;
            std::uint8_t  flags = 0;
        };
        constexpr std::size_t kRingCap = 64;
        std::mutex            g_ringMx;
        std::vector<HitRec>   g_ring;              // under g_ringMx
        std::uint32_t         g_ringOverflow = 0;  // under g_ringMx

        struct Pending { HitRec h; };
        std::unordered_map<RE::FormID, std::vector<Pending>> g_pending;   // by follower, WORKER

        // ── Q87 aggregate (MAIN-thread work behind its own LEAF mutex; Reset clears it) ───────────────────────────
        struct Q87 {
            std::vector<float> sneak, plain;
            std::uint32_t sneakBeforeMax = 0;   // sneak hits whose before was the target's max health (an opener, no sample)
            std::uint32_t sneakNoBefore = 0;    // sneak hits with no usable before at all (not counted in the median)
            float  expected = kNaN;
            double lastLog = 0.0;
        };
        std::mutex                                   g_q87Mx;
        std::unordered_map<std::uint64_t, Q87>       g_q87;   // (follower << 8 | type + 1)

        float Median(std::vector<float> a_v) {
            if (a_v.empty()) return kNaN;
            std::sort(a_v.begin(), a_v.end());
            const std::size_t n = a_v.size();
            return (n & 1u) ? a_v[n / 2] : 0.5f * (a_v[n / 2 - 1] + a_v[n / 2]);
        }
        void PushCapped(std::vector<float>& a_v, float a_x) {
            if (a_v.size() >= kQ87Cap) a_v.erase(a_v.begin());
            a_v.push_back(a_x);
        }
        std::string Raw(const std::vector<float>& a_v) {
            std::string s;
            const std::size_t from = a_v.size() > 8 ? a_v.size() - 8 : 0;
            for (std::size_t i = from; i < a_v.size(); ++i) s += std::format("{}{:.1f}", s.empty() ? "" : ",", a_v[i]);
            return s.empty() ? "-" : s;
        }

        // ── groups ────────────────────────────────────────────────────────────────────────────────────────────────
        // The follower's own group: ONE read lock, handle copies only inside (the TauntProbe.cpp ServiceP2 pass 1 shape, #23).
        std::vector<RE::NiPointer<RE::Actor>> GroupFoes(RE::Actor* a_f) {
            std::vector<RE::NiPointer<RE::Actor>> foes;
            auto* cc = a_f->GetActorRuntimeData().combatController;
            if (!cc || !cc->combatGroup) return foes;
            std::vector<RE::ActorHandle> raw;
            {
                RE::BSReadLockGuard lk(cc->combatGroup->lock);
                for (const auto& t : cc->combatGroup->targets) raw.push_back(t.targetHandle);
            }
            for (const auto& h : raw) {
                auto p = h.get();   // HOLD the NiPointer
                auto* foe = p.get();
                if (!foe || foe == a_f || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
                foes.push_back(std::move(p));
                if (foes.size() >= 16) break;
            }
            return foes;
        }

        float Health(RE::Actor* a_a) {
            auto* avo = a_a ? a_a->AsActorValueOwner() : nullptr;
            return avo ? avo->GetActorValue(RE::ActorValue::kHealth) : kNaN;
        }
        float Av(RE::Actor* a_a, RE::ActorValue a_av) {
            auto* avo = a_a ? a_a->AsActorValueOwner() : nullptr;
            return avo ? avo->GetActorValue(a_av) : kNaN;
        }
        bool MovingBits(RE::Actor* a_a) {
            const auto* st = a_a->AsActorState();
            return st && (st->actorState1.movingForward || st->actorState1.movingBack || st->actorState1.movingLeft || st->actorState1.movingRight);
        }

        void SampleHp(const std::vector<RE::NiPointer<RE::Actor>>& a_foes, double a_now) {
            for (const auto& p : a_foes) {
                RE::Actor* foe = p.get();
                const float hp = Health(foe);
                if (!std::isfinite(hp)) continue;
                auto& h = g_hp[foe->GetFormID()];
                if (!h.s.empty() && a_now - h.s.back().t < 0.05) continue;   // two services in one instant: one sample
                h.s.push_back({ a_now, hp });
                while (h.s.size() > 16) h.s.pop_front();   // 16 laps (~2 s): a hit settles 0.3 s later, a busy party laps slower
                h.last = a_now;
            }
            for (auto it = g_hp.begin(); it != g_hp.end();) {
                if (a_now - it->second.last > kFoeKeepS) it = g_hp.erase(it);
                else ++it;
            }
        }

        // ── the main-thread helpers ───────────────────────────────────────────────────────────────────────────────
        std::string Pe(float a_seed, float a_res) {
            return std::isfinite(a_res) ? std::format("{:.2f}->{:.2f}", a_seed, a_res) : std::string("n/a");
        }

        // MAIN THREAD. The five entry points with a follower as the perk owner and `a_tgt` as the second argument (the
        // progression/Progression.cpp LockpickEntryPointsFor call shape: HandleEntryPoint(ep, owner, args..., &value), the
        // argument counts read off the call sites in the 1.6.1170 binary: 57 / 48 / 47 = one condition argument (the
        // observer), 15 = none, 18 = weapon then target).
        struct EpOut { float e57 = kNaN, e48 = kNaN, e47 = kNaN, e15 = kNaN, e18 = kNaN, seed18 = kNaN; };
        EpOut RunEntryPoints(RE::Actor* a_f, RE::Actor* a_tgt, float a_skillSeed, RE::TESObjectWEAP* a_wpn) {
            EpOut o;
            if (!EntryPointOk()) return o;
            using EP = RE::BGSEntryPoint::ENTRY_POINT;
            float v15 = 0.0f;
            RE::BGSEntryPoint::HandleEntryPoint(EP::kIgnoreRunningDuringDetection, a_f, &v15);
            o.e15 = v15;
            if (a_tgt) {
                auto* t = static_cast<RE::TESObjectREFR*>(a_tgt);
                float v57 = a_skillSeed, v48 = 1.0f, v47 = 50.0f;
                RE::BGSEntryPoint::HandleEntryPoint(EP::kModDetectionSneakSkill, a_f, t, &v57);
                RE::BGSEntryPoint::HandleEntryPoint(EP::kModDetectionMovement, a_f, t, &v48);
                RE::BGSEntryPoint::HandleEntryPoint(EP::kModDetectionLight, a_f, t, &v47);
                o.e57 = v57; o.e48 = v48; o.e47 = v47;
                if (a_wpn) {
                    const char* nm = SneakMultName(static_cast<int>(a_wpn->GetWeaponType()));
                    o.seed18 = nm ? Gmst(nm) : kNaN;
                    if (std::isfinite(o.seed18)) {
                        float v18 = o.seed18;
                        RE::BGSEntryPoint::HandleEntryPoint(EP::kModSneakAttackMult, a_f, static_cast<RE::TESForm*>(a_wpn), t, &v18);
                        o.e18 = v18;
                    }
                }
            }
            return o;
        }

        // ── P1 ──────────────────────────────────────────────────────────────────────────────────────────────────
        void ServiceP1(RE::Actor* a_f, RE::FormID a_id, bool a_ownCombat) {
            auto tp = a_f->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
            RE::FormID tgtId = tp ? tp->GetFormID() : 0;
            const auto* st = a_f->AsActorState();
            const bool sneaking = a_f->IsSneaking();
            const bool moving = MovingBits(a_f);
            const bool running = st && st->actorState1.running;
            const bool walking = st && st->actorState1.walking;
            const bool sprint = st && st->actorState1.sprinting;
            auto* avo = a_f->AsActorValueOwner();
            const float skill = Av(a_f, RE::ActorValue::kSneak);
            const float skillBase = avo ? avo->GetBaseActorValue(RE::ActorValue::kSneak) : kNaN;
            const float noise = Av(a_f, RE::ActorValue::kMovementNoiseMult);
            const float invis = Av(a_f, RE::ActorValue::kInvisibility);
            RE::FormID wpnId = 0;
            if (auto* r = a_f->GetEquippedObject(false)) if (auto* w = r->As<RE::TESObjectWEAP>()) wpnId = w->GetFormID();
            // G2: how many of his book's spells the engine's cast-sound entry point leaves soundless (iaus/Fills.h CastsSilently).
            std::string castSilent = "no book";
            if (const auto book = Iaus::SpellBook::Read(a_f)) {
                int spells = 0, silent = 0;
                for (const auto& e : book->entries) {
                    if (e.source != Iaus::SpellSource::Spell) continue;
                    ++spells;
                    if (e.silent) ++silent;
                }
                castSilent = std::format("{}/{}", silent, spells);
            }
            std::string head = std::format(
                "[stealth-probe] self 0x{:08X}: sneaking={} moving={} walking={} running={} sprint={} inCombat={} target=0x{:08X} | "
                "Sneak AV {:.1f} (base {:.1f}) MovementNoiseMult {:.2f} Invisibility {:.1f} castSilent={}",
                a_id, sneaking, moving, walking, running, sprint, a_ownCombat, tgtId, skill, skillBase, noise, invis, castSilent);
            const auto finish = [a_id, tgtId, wpnId, skill, head = std::move(head)](bool a_main) {
                std::string tail;
                if (a_main) {
                    auto* f = RE::TESForm::LookupByID<RE::Actor>(a_id);
                    auto* tgt = tgtId ? RE::TESForm::LookupByID<RE::Actor>(tgtId) : nullptr;
                    auto* wpn = wpnId ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(wpnId) : nullptr;
                    if (f && f->Is3DLoaded()) {
                        const EpOut e = RunEntryPoints(f, tgt, skill, wpn);
                        tail = std::format(" | entries (seed->result, owner him, arg his target): 57 sneakSkill {} 48 movement {} 47 light {} 15 ignoreRun {} 18 sneakMult {}{}",
                                           Pe(skill, e.e57), Pe(1.0f, e.e48), Pe(50.0f, e.e47), Pe(0.0f, e.e15), Pe(e.seed18, e.e18),
                                           EntryPointOk() ? "" : " [HandleEntryPoint row REFUSED]");
                        if (!tgt) tail += " [no target: 57/48/47/18 not run]";
                    } else tail = " | entries n/a (actor not loaded)";
                } else tail = " | entries n/a (no main-thread pump)";
                spdlog::info("{}{}", head, tail);
            };
            if (MainThread::IsInstalled()) MainThread::Post([finish] { finish(true); });
            else finish(false);
        }

        // ── P2 ──────────────────────────────────────────────────────────────────────────────────────────────────
        // MAIN THREAD. The predicted score of calculate-detection-re.md section 4 for (observer = foe, target = follower):
        //   score = fSneakBaseValue + sound + lightTerm + skillTerm.
        // Inputs the probe does NOT have, and what it does about them: LIGHT (the cached light level, P4 reads it) -> the lightTerm is
        // left out of `pred` and its CEILING (light 100) is printed as `lightMax`; equipped weight -> 0; the noise of the actor's
        // current action -> 0; line of sight -> assumed clear; flying, ambush and sleep -> not set. So `pred` is a LOWER bound and
        // `pred + lightMax` an upper bound when the foe's view cone covers him.
        struct Pred { float lo = kNaN, lightMax = kNaN, skillTerm = kNaN, sound = kNaN, distF = kNaN; bool cone = false; float angleDeg = kNaN; };
        Pred Predict(RE::Actor* a_foe, RE::Actor* a_f, float a_skillT) {
            Pred p;
            const float base = Gmst("fSneakBaseValue"), maxD0 = Gmst("fSneakMaxDistance"), extM = Gmst("fSneakExteriorDistanceMult"),
                        expo = Gmst("fSneakDistanceAttenuationExponent"), alertMod = Gmst("fSneakAlertMod"), combatMod = Gmst("fSneakCombatMod"),
                        pMin = Gmst("fSneakPerceptionSkillMin"), pMax = Gmst("fSneakPerceptionSkillMax"), skMult = Gmst("fSneakSkillMult"),
                        runMult = Gmst("fSneakRunningMult"), wBase = Gmst("fSneakEquippedWeightBase"), soundsMult = Gmst("fSneakSoundsMult"),
                        lightExt = Gmst("fSneakLightExteriorMult"), lightInt = Gmst("fSneakLightMult"), lightMoveM = Gmst("fSneakLightMoveMult"),
                        lightRunM = Gmst("fSneakLightRunMult"), lightMod = Gmst("fDetectionSneakLightMod"), view = Gmst("fDetectionViewCone");
            for (const float g : { base, maxD0, extM, expo, alertMod, combatMod, pMin, pMax, skMult, runMult, wBase, soundsMult, lightExt, lightInt, lightMoveM, lightRunM, lightMod, view })
                if (!std::isfinite(g)) return p;   // a setting is missing: no prediction, never a guess
            auto* cell = a_foe->GetParentCell();
            const bool exterior = cell && !cell->cellFlags.any(RE::TESObjectCELL::Flag::kIsInteriorCell);
            const float maxDist = maxD0 * (exterior ? extM : 1.0f);
            const float dist = a_foe->GetPosition().GetDistance(a_f->GetPosition());
            p.distF = std::pow((std::max)(0.0f, (maxDist - dist) / maxDist), expo);
            const bool inCombat = a_foe->IsInCombat();
            auto tp = a_foe->GetActorRuntimeData().currentCombatTarget.get();
            const bool busy = tp && tp.get() != a_f;
            const float alert = 1.0f + (inCombat ? alertMod : 0.0f) + (busy ? combatMod : 0.0f);
            const float skillO = std::clamp(Av(a_foe, RE::ActorValue::kSneak), 0.0f, 100.0f);
            const float perception = pMin + (pMax - pMin) * skillO / 100.0f;
            const bool sneakingT = a_f->IsSneaking();
            p.skillTerm = (p.distF * alert * perception - (sneakingT ? a_skillT : 0.0f)) * skMult;
            const auto* st = a_f->AsActorState();
            const bool moving = MovingBits(a_f);
            const bool running = st && st->actorState1.running;
            const float noiseMult = (std::max)(0.0f, Av(a_f, RE::ActorValue::kMovementNoiseMult));
            const float moveNoise = moving ? noiseMult * wBase : 0.0f;
            p.sound = (std::max)(0.0f, ((running ? runMult : 1.0f) * moveNoise) * soundsMult * p.distF);
            p.lo = base + p.sound + p.skillTerm;
            // the view cone: the foe's facing against the bearing to him (Skyrim yaw runs clockwise from +Y)
            const auto d = a_f->GetPosition() - a_foe->GetPosition();
            float diff = std::atan2(d.x, d.y) - a_foe->GetAngleZ();
            while (diff > 3.14159265f) diff -= 6.2831853f;
            while (diff < -3.14159265f) diff += 6.2831853f;
            p.angleDeg = std::fabs(diff) * 57.2957795f;
            p.cone = p.angleDeg < view * 0.5f;
            const bool invisible = Av(a_f, RE::ActorValue::kInvisibility) > 0.0f;
            const float lightMove = 1.0f + lightMoveM * (moving ? 1.0f : 0.0f) + lightRunM * (running ? 1.0f : 0.0f);
            p.lightMax = (p.cone && !invisible) ? (std::max)(0.0f, p.distF * lightMove * (100.0f * (exterior ? lightExt : lightInt) + lightMod)) : 0.0f;
            return p;
        }

        void LogSeen(RE::FormID a_foe, RE::FormID a_f, std::string a_head) {
            if (!MainThread::IsInstalled()) {
                spdlog::info("{}; sense: unknown, pred: n/a (no main-thread pump)", a_head);
                return;
            }
            MainThread::Post([a_foe, a_f, head = std::move(a_head)] {
                auto* foe = RE::TESForm::LookupByID<RE::Actor>(a_foe);
                auto* f   = RE::TESForm::LookupByID<RE::Actor>(a_f);
                if (!foe || !f || !foe->Is3DLoaded() || !f->Is3DLoaded()) {
                    spdlog::info("{}; sense: unknown, pred: n/a (actor not loaded)", head);
                    return;
                }
                const char* sense = "unknown";
                APMFBridge::SenseReading rd{};
                if (APMFBridge::SenseOf(a_foe, a_f, 0, 0.0f, rd)) {
                    if (rd.senses) sense = "yes";
                    else if (rd.detail & APMF_API::kAwareDetail_SightDeferred) sense = "deferred";   // sight ray not cast this frame
                    else sense = "no";
                }
                // skillT: his Sneak AV, then perk entry 57 (owner him, arg the foe), exactly the formula's order.
                float skillT = Av(f, RE::ActorValue::kSneak);
                if (EntryPointOk() && std::isfinite(skillT)) {
                    float v = skillT;
                    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModDetectionSneakSkill, f,
                                                        static_cast<RE::TESObjectREFR*>(foe), &v);
                    if (std::isfinite(v)) skillT = v;
                }
                const Pred p = Predict(foe, f, skillT);
                if (!std::isfinite(p.lo))
                    spdlog::info("{}; sense: {}, pred: n/a (a game setting is missing)", head, sense);
                else
                    spdlog::info("{}; sense: {}, pred: {:.1f} (sound {:.1f} skill {:.1f}) lightMax +{:.1f} cone={} ({:.0f} deg) distF {:.2f} "
                                 "[lower bound: light, weight, action noise, LOS not read]",
                                 head, sense, p.lo, p.sound, p.skillTerm, p.lightMax, p.cone, p.angleDeg, p.distF);
            });
        }

        void ServiceP2(RE::Actor* a_f, RE::FormID a_id, const std::vector<RE::NiPointer<RE::Actor>>& a_foes, double a_now) {
            const std::uint32_t fHandle = a_f->GetHandle().native_handle();
            std::size_t logged = 0;
            for (const auto& fp : a_foes) {
                RE::Actor* foe = fp.get();
                if (foe->IsDead() || !foe->IsInCombat()) continue;
                const RE::FormID foeId = foe->GetFormID();
                auto& ss = g_seen[(static_cast<std::uint64_t>(foeId) << 32) | a_id];
                if (a_now - ss.at < kSeenPeriodS) continue;
                if (logged >= kMaxFoes) break;
                ss.at = a_now;
                ++logged;
                // The foe's OWN group record of him (iaus/Hide.h: the TauntProbe.cpp read), one read lock, nothing nested.
                const Iaus::Hide::Record r = Iaus::Hide::Read(foe, fHandle);
                if (r.hasGroup && r.present && (!std::isfinite(ss.stamp) || r.lastKnownStamp != ss.stamp)) {
                    ss.stamp = r.lastKnownStamp;
                    ss.stampChangedAt = a_now;
                }
                const double stale = (r.present && ss.stampChangedAt > 0.0) ? a_now - ss.stampChangedAt : -1.0;
                auto tp = foe->GetActorRuntimeData().currentCombatTarget.get();
                const char* grp = !r.hasGroup ? "NOGROUP" : (!r.present ? "ABSENT" : (r.lost ? "LOST" : "HELD"));
                const Iaus::Tri v = Iaus::Hide::Verdict(r);
                LogSeen(foeId, a_id, std::format(
                    "[stealth-probe] seen foe 0x{:08X} (L{}) -> follower 0x{:08X} (sneaking): group {} detect={} stealthPts={:.1f} known={} lost={} "
                    "attackers={} stamp={:.2f} unrefreshed={:.1f}s | dist {:.0f} foeTarget=0x{:08X} | MFO verdict: detects me = {}",
                    foeId, foe->GetLevel(), a_id, grp, r.detectLevel, r.stealthPoints, r.known, r.lost, r.attackerCount,
                    r.lastKnownStamp, stale, a_f->GetPosition().GetDistance(foe->GetPosition()), tp ? tp->GetFormID() : 0u,
                    v == Iaus::Tri::Yes ? "YES" : (v == Iaus::Tri::No ? "NO (hidden)" : "unknown")));
            }
            for (auto it = g_seen.begin(); it != g_seen.end();) {
                if (a_now - it->second.at > kFoeKeepS) it = g_seen.erase(it);
                else ++it;
            }
        }

        // ── P3 ──────────────────────────────────────────────────────────────────────────────────────────────────
        struct HitOut {
            RE::FormID cause, target, source, projectile;
            std::uint8_t flags;
            float before, after, hitAgeMs;
            bool  beforeMax;   // no health sample of the target at or before the hit (an opener): before = its max health
            const char* afterSrc;
            bool  logIt;
            std::uint32_t suppressed;
            // the target's record of the attacker, read on the worker right after the hit
            bool recHas, recPresent, recLost; std::int32_t recDetect; float recStealth; long long recLagMs;
        };

        // MAIN THREAD. The expected multiplier (the weapon's setting by name, then perk entry 18 with the follower as the attacker),
        // the Q87 aggregate and the log lines.
        void ResolveHitMain(const HitOut o) {
            auto* f   = RE::TESForm::LookupByID<RE::Actor>(o.cause);
            auto* tgt = RE::TESForm::LookupByID<RE::Actor>(o.target);
            RE::TESObjectWEAP* w = nullptr;
            const int wtype = WeaponType(o.source, &w);
            const char* gname = SneakMultName(wtype);
            const float gval = gname ? Gmst(gname) : kNaN;
            float expected = gval;
            if (f && tgt && w && std::isfinite(gval) && EntryPointOk()) {
                float v = gval;
                RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSneakAttackMult, f,
                                                    static_cast<RE::TESForm*>(w), static_cast<RE::TESObjectREFR*>(tgt), &v);
                if (std::isfinite(v)) expected = v;
            }
            const bool sneak  = (o.flags & static_cast<std::uint8_t>(RE::TESHitEvent::Flag::kSneakAttack)) != 0;
            const bool power  = (o.flags & static_cast<std::uint8_t>(RE::TESHitEvent::Flag::kPowerAttack)) != 0;
            const bool bash   = (o.flags & static_cast<std::uint8_t>(RE::TESHitEvent::Flag::kBashAttack)) != 0;
            const bool blocked = (o.flags & static_cast<std::uint8_t>(RE::TESHitEvent::Flag::kHitBlocked)) != 0;
            const float drop = o.before - o.after;

            std::string q;
            if (wtype >= 0) {
                std::scoped_lock lk(g_q87Mx);
                Q87& a = g_q87[(static_cast<std::uint64_t>(o.cause) << 8) | static_cast<std::uint64_t>(wtype + 1)];
                const bool clean = !power && !bash && !blocked && std::isfinite(drop) && drop > 0.0f;   // like with like: no power, bash or blocked hit
                if (clean) PushCapped(sneak ? a.sneak : a.plain, drop);
                if (sneak && o.beforeMax) ++a.sneakBeforeMax;
                if (sneak && !std::isfinite(o.before)) ++a.sneakNoBefore;
                if (sneak && std::isfinite(expected)) a.expected = expected;
                const double now = NowSec();
                if (clean && now - a.lastLog >= kQ87LogS) {
                    a.lastLog = now;
                    const float ms = Median(a.sneak), mp = Median(a.plain);
                    const float ratio = (std::isfinite(ms) && std::isfinite(mp) && mp > 0.0f) ? ms / mp : kNaN;
                    const bool enough = a.sneak.size() >= kQ87Need && a.plain.size() >= kQ87Need;
                    const char* verdict = !enough ? "NOT ENOUGH SAMPLES"
                        : (!std::isfinite(ratio) || !std::isfinite(a.expected)) ? "NO RATIO"
                        : (std::fabs(ratio / a.expected - 1.0f) <= 0.25f ? "WITHIN 25%" : "OUTSIDE 25%");
                    q = std::format("[stealth-probe] q87 follower 0x{:08X} weapon type {} ({}): sneak hits {} (median drop {:.1f}; {} with before=max, {} with no before) plain hits {} (median drop {:.1f}) "
                                    "ratio {:.2f} expected {:.2f} (need {} + {}) -> {} | raw sneak [{}] plain [{}] (drop = health before - after, "
                                    "no power/bash/blocked hits; armour and other attackers are NOT removed)",
                                    o.cause, wtype, TypeName(wtype), a.sneak.size(), ms, a.sneakBeforeMax, a.sneakNoBefore, a.plain.size(), mp, ratio, a.expected, kQ87Need, kQ87Need,
                                    verdict, Raw(a.sneak), Raw(a.plain));
                }
            }
            if (o.logIt) {
                spdlog::info("[stealth-probe] hit 0x{:08X} -> 0x{:08X} source 0x{:08X} ({}) projectile 0x{:08X}: kSneakAttack={} power={} bash={} blocked={} | "
                             "target's record of him: {} detect={} stealthPts={:.1f} lost={} (read {} ms after the event) | expected multiplier {:.2f} "
                             "({} = {:.2f}, then entry 18{}) | health before {:.1f}{} after {:.1f} ({}) drop {:.1f} | event age {:.0f} ms{}",
                             o.cause, o.target, o.source, TypeName(wtype), o.projectile, sneak, power, bash, blocked,
                             !o.recHas ? "NOGROUP" : (!o.recPresent ? "ABSENT" : (o.recLost ? "LOST" : "HELD")), o.recDetect, o.recStealth, o.recLost,
                             o.recLagMs, expected, gname ? gname : "no weapon setting", gval, EntryPointOk() ? "" : " [row REFUSED: setting only]",
                             o.before, o.beforeMax ? " before=max" : "", o.after, o.afterSrc, drop, o.hitAgeMs,
                             o.suppressed ? std::format(" | +{} hit line(s) suppressed (20/min cap)", o.suppressed) : std::string());
            }
            if (!q.empty()) spdlog::info("{}", q);
        }

        void DrainHits(RE::Actor* a_f, RE::FormID a_id, FState& a_st, double a_now) {
            {   // mine out of the ring
                std::scoped_lock lk(g_ringMx);
                for (auto it = g_ring.begin(); it != g_ring.end();) {
                    if (it->cause == a_id) { g_pending[a_id].push_back({ *it }); it = g_ring.erase(it); }
                    else ++it;
                }
                if (g_ringOverflow) { a_st.droppedHits += g_ringOverflow; g_ringOverflow = 0; }
            }
            auto pit = g_pending.find(a_id);
            if (pit == g_pending.end()) return;
            const std::uint32_t fHandle = a_f->GetHandle().native_handle();
            auto& vec = pit->second;
            for (auto it = vec.begin(); it != vec.end();) {
                const HitRec& h = it->h;
                const double age = a_now - h.t;
                if (age > kPendingDropS) { ++a_st.droppedHits; it = vec.erase(it); continue; }
                float before = kNaN, after = kNaN;
                const char* afterSrc = "sampled";
                bool haveAfter = false, beforeMax = false;
                if (auto hp = g_hp.find(h.target); hp != g_hp.end()) {
                    for (const auto& smp : hp->second.s) {
                        if (smp.t <= h.t) before = smp.hp;
                        else if (!haveAfter && smp.t >= h.t + kSettleS) { after = smp.hp; haveAfter = true; }
                    }
                }
                if (!std::isfinite(before)) {
                    // P3 opener: the target was not in his group before the hit (so nothing sampled it), i.e. it was not in combat. Its
                    // health was whole: use its max (permanent) health, flagged before=max, so a first sneak hit still gives a drop.
                    auto* tgt = RE::TESForm::LookupByID<RE::Actor>(h.target);
                    auto* avo = tgt ? tgt->AsActorValueOwner() : nullptr;
                    const float mx = avo ? avo->GetPermanentActorValue(RE::ActorValue::kHealth) : kNaN;
                    if (std::isfinite(mx) && mx > 0.0f) { before = mx; beforeMax = true; }
                }
                if (!haveAfter) {
                    if (age < kPendingMaxS) { ++it; continue; }   // wait for the next lap's sample
                    auto* t = RE::TESForm::LookupByID<RE::Actor>(h.target);
                    after = Health(t);
                    afterSrc = "late direct read";
                }
                HitOut o{};
                o.cause = h.cause; o.target = h.target; o.source = h.source; o.projectile = h.projectile; o.flags = h.flags;
                o.before = before; o.beforeMax = beforeMax; o.after = after; o.hitAgeMs = static_cast<float>(age * 1000.0); o.afterSrc = afterSrc;
                o.logIt = a_st.hits.Take(a_now);
                if (o.logIt) { o.suppressed = a_st.hits.suppressed; a_st.hits.suppressed = 0; }
                if (auto* t = RE::TESForm::LookupByID<RE::Actor>(h.target)) {
                    const Iaus::Hide::Record r = Iaus::Hide::Read(t, fHandle);
                    o.recHas = r.hasGroup; o.recPresent = r.present; o.recLost = r.lost; o.recDetect = r.detectLevel; o.recStealth = r.stealthPoints;
                    o.recLagMs = static_cast<long long>(age * 1000.0);
                }
                it = vec.erase(it);
                if (MainThread::IsInstalled()) MainThread::Post([o] { ResolveHitMain(o); });
                else if (o.logIt) spdlog::info("[stealth-probe] hit 0x{:08X} -> 0x{:08X} source 0x{:08X} flags {}: no main-thread pump, no expected multiplier",
                                               o.cause, o.target, o.source, o.flags);
            }
            if (vec.empty()) g_pending.erase(pit);
        }

        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
        public:
            static HitSink* GetSingleton() { static HitSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_ev, RE::BSTEventSource<RE::TESHitEvent>*) override {
                if (!Config::g_stealthProbe.load(std::memory_order_relaxed) || !a_ev || !a_ev->target || !a_ev->cause)
                    return RE::BSEventNotifyControl::kContinue;
                auto* target = a_ev->target->As<RE::Actor>();
                if (!target) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID cause = a_ev->cause->GetFormID();
                if (!Followers::IsTrackedFast(cause)) return RE::BSEventNotifyControl::kContinue;   // the any-thread FormID mirror (#74)
                HitRec r;
                r.t = NowSec();
                r.cause = cause;
                r.target = target->GetFormID();
                r.source = a_ev->source;
                r.projectile = a_ev->projectile;
                r.flags = static_cast<std::uint8_t>(a_ev->flags.underlying());
                std::scoped_lock lk(g_ringMx);   // LEAF: copy a POD and leave, no engine call, no other lock
                if (g_ring.size() >= kRingCap) { ++g_ringOverflow; return RE::BSEventNotifyControl::kContinue; }
                g_ring.push_back(r);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }  // namespace

    void Service(RE::Actor* a_f, RE::FormID a_id, bool a_ownCombat) {
        if (!Config::g_stealthProbe.load(std::memory_order_relaxed)) return;
        if (!a_f) return;
        const double now = NowSec();
        FState& st = g_f[a_id];
        const bool sneaking = a_f->IsSneaking();

        // The follower's own group: health samples for P3 (every follower, sneaking or not, so the plain hits exist to compare).
        const auto foes = GroupFoes(a_f);
        SampleHp(foes, now);
        DrainHits(a_f, a_id, st, now);

        // P1: 1/s while sneaking; a follower in combat who is NOT sneaking (the stance dropped) once per 5 s.
        const double period = sneaking ? kSelfSneakPeriodS : (a_ownCombat ? kSelfCombatPeriodS : 0.0);
        if (period > 0.0 && now - st.selfAt >= period) {
            st.selfAt = now;
            ServiceP1(a_f, a_id, a_ownCombat);
        }
        // P2: a sneaking follower, each engaged foe 1/s.
        if (sneaking && !foes.empty()) ServiceP2(a_f, a_id, foes, now);

        // The closing line of a capped hit window: nothing printed for a whole window, suppressed lines still owed.
        if (st.hits.suppressed > 0 && now - st.hits.lastLine > kHitWindowS) {
            spdlog::info("[stealth-probe] hit window closed for follower 0x{:08X}: {} hit line(s) were suppressed (20/min cap)", a_id, st.hits.suppressed);
            st.hits.suppressed = 0;
        }
    }

    void RegisterHitSink() {
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
            spdlog::info("[stealth-probe] hit sink registered (live only while bStealthProbe = 1)");
        }
    }

    void Reset() {
        g_f.clear();
        g_seen.clear();
        g_hp.clear();
        g_pending.clear();
        {
            std::scoped_lock lk(g_ringMx);
            g_ring.clear();
            g_ringOverflow = 0;
        }
        std::scoped_lock lk(g_q87Mx);
        g_q87.clear();
    }

}
