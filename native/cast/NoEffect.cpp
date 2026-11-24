// cast/NoEffect.cpp -- THE ZERO-EFFECT HOLD (adopt-seats, Harbinger ABI v46).
//
// Harbinger v46 admits a claim on a Stagger / Disarm / TargetEffect / Paralyze row without asking the engine "does the effect apply
// to this target" (APMF_API.h CastSeatCoverage). cast/WillWork.cpp judges that before the claim; this file is the net under it for
// what no record can say: after TWO casts of such a spell (two distinct observed fires, in a row) left no effect on that target, the
// (spell, target) pair is held off for the fight, with a loud line, so the follower does not cast it again and again (Turn Lesser
// Undead at a Deathlord, field 11-23). One miss, ward, absorb or resist is never "never works" (review F3): a Landed reading resets
// the count. Never a mask: the failure is logged, the pair is refused, nothing routes around it.
//
// THE OBSERVATION: the rules are the pure SeatRows::Judge (harness rows). One cast is read ONCE, 2 to 6 s after its OBSERVED fire
// (ComposedCast::ObservedAgeMs, the real fire time, review F2): the KEYED effect of the row on the target (MagicTarget::
// HasMagicEffect) for a lasting effect, or for Disarm the right-hand weapon he held at the fire. Inconclusive, never counted: an
// instant effect, a duration that may already have ended, a concentration spell, a spell that also carries another hostile instant
// effect, a reading too early or too late.
//
// THREADING. NoEffectNoteFired / NoEffectHeld run on the worker (the offense claim's refresh lap, WillWork); the readings run on the
// MAIN thread, one MainThread::Post (INVARIANTS #83). The ledger is behind g_mx (a leaf lock, never held across a post or a log).
// A reading is applied only if its fire is still the pending one (a newer fire discards it). The settle reading is driven by the
// callers (every refresh lap of the claim and every WillWork ask of the pair); if neither comes in the window nothing is counted.
#include "Actuation_internal.h"
#include "MainThread.h"
#include "cast/SeatRows.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::Actuation {

    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr double      kNewFireSec   = 1.5;   // a fire this much newer than the judged one is a NEW cast (one cast = one observed fire)
        constexpr double      kAskRetrySec  = 1.5;   // a settle post with no answer this long is re-asked (a lost post, review F8)
        constexpr std::size_t kMaxPending   = 128;

        struct Key {
            RE::FormID caster, spell, target;
            bool operator==(const Key& o) const { return caster == o.caster && spell == o.spell && target == o.target; }
        };
        struct KeyHash {
            std::size_t operator()(const Key& k) const {
                std::size_t h = k.caster;
                h = h * 1099511628211ULL ^ k.spell;
                h = h * 1099511628211ULL ^ k.target;
                return h;
            }
        };
        struct Pending {
            Clock::time_point firedAt{};     // the OBSERVED time of the fire being judged
            Clock::time_point askedAt{};     // the settle post in flight (epoch = none)
            bool              judged = false;   // this fire has been read (any reading)
            RE::FormID        weapon = 0;       // Disarm: the right-hand weapon seen at the fire
            int               zeros  = 0;       // consecutive Zero readings from distinct fires
        };
        std::mutex                                  g_mx;
        std::unordered_map<Key, Pending, KeyHash>   g_pending;
        SeatRows::HoldLedger                        g_held;

        double Age(const Clock::time_point& a_then, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_then).count();
        }
        bool Set(const Clock::time_point& t) { return t.time_since_epoch().count() != 0; }

        bool HostileRow(EngineRow a_row) {
            return a_row == EngineRow::Stagger || a_row == EngineRow::Disarm || a_row == EngineRow::TargetEffect ||
                   a_row == EngineRow::Paralyze;
        }

        RE::FormID RightWeaponId(RE::Actor* a_target) {
            auto* o = a_target->GetEquippedObject(false);
            return o ? o->GetFormID() : 0u;
        }

        // MAIN THREAD. The Disarm snapshot taken as soon as the fire is seen. A reading that finds it for a different fire is dropped.
        void PostSnapshot(Key a_key, Clock::time_point a_firedAt) {
            MainThread::Post([a_key, a_firedAt]() {
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_key.target);
                const RE::FormID w = target ? DisarmableRightWeapon(target) : 0u;
                std::scoped_lock lk(g_mx);
                const auto it = g_pending.find(a_key);
                // A snapshot taken late may see a spare already drawn: no subject then, so the reading is Inconclusive.
                if (it != g_pending.end() && it->second.firedAt == a_firedAt && Age(a_firedAt, Clock::now()) <= 1.0) it->second.weapon = w;
            });
        }

        // MAIN THREAD. The reading of one fire (SeatRows::Judge).
        SeatRows::Reading Read(RE::SpellItem* a_spell, RE::Actor* a_target, const Pending& a_p, double a_ageSec, std::string& a_what) {
            const auto arch = ClassifyArchetype(a_spell);
            SeatRows::ReadInput in;
            in.ageSec        = static_cast<float>(a_ageSec);
            in.concentration = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            in.disarm        = arch.row == EngineRow::Disarm;
            if (arch.keyedEffect < 0 || static_cast<std::size_t>(arch.keyedEffect) >= a_spell->effects.size())
                return SeatRows::Reading::Inconclusive;
            for (std::int32_t i = 0; i < static_cast<std::int32_t>(a_spell->effects.size()); ++i) {
                if (i == arch.keyedEffect) continue;
                const auto* oe = a_spell->effects[static_cast<std::uint32_t>(i)];
                const auto* ob = oe ? oe->baseEffect : nullptr;
                if (ob && (ob->IsHostile() || ob->IsDetrimental()) && oe->GetDuration() == 0) in.otherHostileInstant = true;
            }
            const auto* ke = a_spell->effects[static_cast<std::uint32_t>(arch.keyedEffect)];
            auto*       kb = ke ? ke->baseEffect : nullptr;
            if (!kb) return SeatRows::Reading::Inconclusive;
            in.keyedDurationSec = static_cast<float>(ke->GetDuration());
            if (in.disarm) {
                in.hadSubject = a_p.weapon != 0;
                in.landed     = RightWeaponId(a_target) != a_p.weapon;   // the weapon he held is no longer in that hand
                a_what        = "the right-hand weapon he held when the cast fired is still in his hand";
            } else {
                auto* mt  = a_target->AsMagicTarget();
                in.landed = mt && mt->HasMagicEffect(kb);
                a_what    = std::format("the keyed effect '{}' is not on the target", kb->GetName() ? kb->GetName() : "effect");
            }
            return SeatRows::Judge(in);
        }

        void PostSettle(Key a_key, Clock::time_point a_firedAt) {
            MainThread::Post([a_key, a_firedAt]() {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_key.caster);
                auto* spell  = RE::TESForm::LookupByID<RE::SpellItem>(a_key.spell);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_key.target);
                Pending snap;
                {
                    std::scoped_lock lk(g_mx);
                    const auto it = g_pending.find(a_key);
                    if (it == g_pending.end() || it->second.firedAt != a_firedAt || it->second.judged) return;   // a newer fire took over, or this one is already read
                    snap = it->second;
                }
                std::string what;
                auto reading = SeatRows::Reading::Inconclusive;
                // A pair gone from the world (dead, unloaded) is not read: it is the end of the pair, not a zero effect.
                if (caster && spell && target && !target->IsDead() && target->Is3DLoaded())
                    reading = Read(spell, target, snap, Age(a_firedAt, Clock::now()), what);
                int  zeros = 0;
                bool fresh = false;
                {
                    std::scoped_lock lk(g_mx);
                    const auto it = g_pending.find(a_key);
                    if (it == g_pending.end() || it->second.firedAt != a_firedAt || it->second.judged) return;   // checked again under the lock (review F7)
                    it->second.judged  = true;
                    it->second.askedAt = Clock::time_point{};
                    it->second.zeros   = SeatRows::Tally(it->second.zeros, reading);
                    zeros = it->second.zeros;
                    if (zeros >= SeatRows::kZerosToHold) fresh = g_held.Hold(a_key.caster, a_key.spell, a_key.target);   // inside the found branch
                }
                if (reading == SeatRows::Reading::Zero)
                    spdlog::warn("[noeffect] caster {:08X} spell {:08X} \"{}\" at {:08X}: a cast FIRED and left no effect on this target "
                                 "({}); zero reading {} of {}{}", a_key.caster, a_key.spell, spell && spell->GetName() ? spell->GetName() : "?",
                                 a_key.target, what, zeros, SeatRows::kZerosToHold,
                                 fresh ? " -- the (spell, target) is HELD OFF for the rest of the fight (WillWork answers No; no direct fallback). "
                                         "Check the effect's conditions and any immunity the record does not state (cast/NoEffect.cpp)"
                                       : " (one more from a separate cast holds it off; a landed cast resets the count)");
            });
        }

    }

    void NoEffectNoteFired(RE::Actor* a_caster, RE::SpellItem* a_spell, RE::Actor* a_target, std::int64_t a_fireAgeMs) {
        if (!a_caster || !a_spell || !a_target || a_target == a_caster || a_fireAgeMs < 0) return;
        if (!MainThread::IsInstalled()) return;   // VR: no pump, no read (documented degrade, as WillWork)
        if (!HostileRow(ClassifyArchetype(a_spell).row)) return;
        const Key  key{ a_caster->GetFormID(), a_spell->GetFormID(), a_target->GetFormID() };
        const auto now     = Clock::now();
        const auto firedAt = now - std::chrono::milliseconds(a_fireAgeMs);
        bool snapshot = false, settle = false;
        Clock::time_point settleFire{};
        {
            std::scoped_lock lk(g_mx);
            if (g_held.Held(key.caster, key.spell, key.target)) return;
            if (g_pending.size() >= kMaxPending) {   // a bound: forget the readings with no count first, then everything
                for (auto it = g_pending.begin(); it != g_pending.end();)
                    it = (it->second.judged && it->second.zeros == 0) ? g_pending.erase(it) : std::next(it);
                if (g_pending.size() >= kMaxPending) g_pending.clear();
            }
            auto [it, isNew] = g_pending.try_emplace(key);
            Pending& p = it->second;
            // A real NEW fire: the observed time is newer than the judged one (the latch outlives a claim, so the time is the identity).
            if (isNew || Age(p.firedAt, firedAt) > kNewFireSec) {
                const int zeros = p.zeros;
                p = Pending{};
                p.zeros   = zeros;
                p.firedAt = firedAt;
                snapshot  = true;
            } else if (!p.judged && (!Set(p.askedAt) || Age(p.askedAt, now) >= kAskRetrySec)) {
                const double age = Age(p.firedAt, now);
                if (age > static_cast<double>(SeatRows::kMaxAgeSec)) p.judged = true;   // too late to read this one: counted as nothing
                else if (age >= static_cast<double>(SeatRows::kSettleSec)) { p.askedAt = now; settle = true; }
            }
            settleFire = p.firedAt;
        }
        if (snapshot) PostSnapshot(key, settleFire);   // outside the mutex: Post takes its own queue lock
        if (settle) PostSettle(key, settleFire);
    }

    bool NoEffectHeld(RE::FormID a_caster, RE::FormID a_spell, RE::FormID a_target) {
        const auto now = Clock::now();
        Key        key{ a_caster, a_spell, a_target };
        bool       settle = false;
        Clock::time_point fire{};
        {
            std::scoped_lock lk(g_mx);
            if (g_held.Held(a_caster, a_spell, a_target)) return true;
            // The other driver of the settle reading: the picker keeps asking WillWork about the pair after the claim is gone.
            if (const auto it = g_pending.find(key); it != g_pending.end()) {
                Pending& p = it->second;
                if (!p.judged && Set(p.firedAt) && (!Set(p.askedAt) || Age(p.askedAt, now) >= kAskRetrySec)) {
                    const double age = Age(p.firedAt, now);
                    if (age > static_cast<double>(SeatRows::kMaxAgeSec)) p.judged = true;
                    else if (age >= static_cast<double>(SeatRows::kSettleSec)) { p.askedAt = now; settle = true; fire = p.firedAt; }
                }
            }
        }
        if (settle) PostSettle(key, fire);
        return false;
    }

    void NoEffectClear(RE::FormID a_caster) {
        std::scoped_lock lk(g_mx);
        g_held.ClearFollower(a_caster);
        for (auto it = g_pending.begin(); it != g_pending.end();)
            it = it->first.caster == a_caster ? g_pending.erase(it) : std::next(it);
    }

    void NoEffectClearAll() {
        std::scoped_lock lk(g_mx);
        g_held.Clear();
        g_pending.clear();
    }

}
