// cast/NoEffect.cpp -- THE ZERO-EFFECT HOLD (adopt-seats, Harbinger ABI v46).
//
// Harbinger v46 admits a claim on a Stagger / Disarm / TargetEffect / Paralyze row without asking the engine "does the effect apply
// to this target" (APMF_API.h CastSeatCoverage). cast/WillWork.cpp now judges that before the claim; this file is the net under it
// for what no record can say (an immunity the engine keeps in code, a target the conditions did not describe): after a cast of such
// a spell FIRED and left no effect on that target, the (spell, target) pair is held off for the fight, with a loud line, so the
// follower does not cast it again and again (Turn Lesser Undead at a Deathlord, field 11-23: cast and wasted). Never a mask: the
// failure is logged, the pair is refused, nothing routes around it.
//
// THE OBSERVATION (what "no effect" means here, one reader, one place):
//   * a spell with a LASTING effect (an effect with a duration: Turn Undead's fear, Paralysis, Command): the effect's magic effect
//     is not on the target after the settle time (MagicTarget::HasMagicEffect);
//   * Disarm: the weapon the target held when the fire was first seen is still in the same hand after the settle time;
//   * an instant effect with nothing to read afterwards (Stagger, Banish): INCONCLUSIVE, never held. Listed in the report.
//
// THREADING. NoEffectNoteFired / NoEffectHeld run on the worker (the offense claim's refresh lap, WillWork); the readings run on the
// MAIN thread, one MainThread::Post (INVARIANTS #83: no engine read off the main thread). The ledger is behind g_mx (a leaf lock,
// never held across a post or a log). The settle check is driven by the callers themselves (every refresh lap of the claim and
// every WillWork ask of the pair); if neither comes after the settle time nothing is held, the hold is never invented.
#include "Actuation_internal.h"
#include "MainThread.h"
#include "cast/SeatRows.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace MFO::Actuation {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Sized from the real cadence (principle 9): a bolt or a ray at the row's reach (256 to 384 units) lands inside a second, the
        // cast animation already ran before a fire is observed; an effect the engine applies shows on the target in the same frame.
        constexpr double      kSettleSec   = 2.0;   // fire seen -> the reading
        constexpr double      kRenoteSec   = 8.0;   // a checked pair is read again only for a NEW cast cycle after this long
        constexpr std::size_t kMaxPending  = 128;

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
            Clock::time_point firedAt{};
            Clock::time_point doneAt{};       // epoch = still open
            bool              snapped   = false;   // the main-thread "before" snapshot was taken
            bool              asked     = false;   // the settle reading is posted
            RE::FormID        weapon[2]{ 0, 0 };   // the target's equipped weapon, right then left, when the fire was first seen
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

        RE::FormID EquippedWeapon(RE::Actor* a_target, bool a_left) {
            auto* o = a_target->GetEquippedObject(a_left);
            auto* w = o ? o->As<RE::TESObjectWEAP>() : nullptr;
            return w ? w->GetFormID() : 0u;
        }

        // MAIN THREAD. The "before" snapshot (Disarm's weapon), taken as soon as the fire is seen: the effect has had at most the
        // projectile's flight, so the weapon is still in the hand unless the cast already landed (then it reads as landed, a fail-safe).
        void PostSnapshot(Key a_key) {
            MainThread::Post([a_key]() {
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_key.target);
                std::scoped_lock lk(g_mx);
                const auto it = g_pending.find(a_key);
                if (it == g_pending.end() || it->second.snapped) return;
                it->second.snapped = true;
                if (target) { it->second.weapon[0] = EquippedWeapon(target, false); it->second.weapon[1] = EquippedWeapon(target, true); }
            });
        }

        // MAIN THREAD. The settle reading. Returns true = HOLD (the cast left no effect), with a_why the evidence.
        bool NoEffectOnTarget(RE::SpellItem* a_spell, RE::Actor* a_target, const Pending& a_p, std::string& a_why) {
            const auto arch = ClassifyArchetype(a_spell);
            // Disarm: the weapon he held is still in that hand.
            if (arch.row == EngineRow::Disarm) {
                bool had = false, still = false;
                for (int hand = 0; hand < 2; ++hand) {
                    if (a_p.weapon[hand] == 0) continue;
                    had = true;
                    if (EquippedWeapon(a_target, hand == 1) == a_p.weapon[hand]) still = true;
                }
                if (!had) return false;   // nothing was held when the fire was seen: nothing to read (WillWork's need keeps this case out)
                if (!still) return false;
                a_why = "the weapon he held when the cast fired is still in his hand";
                return true;
            }
            // A lasting effect: its magic effect is on the target, or it did not land.
            bool lasting = false;
            auto* mt = a_target->AsMagicTarget();
            for (auto* e : a_spell->effects) {
                auto* base = e ? e->baseEffect : nullptr;
                if (!base || e->GetDuration() == 0) continue;
                lasting = true;
                if (mt && mt->HasMagicEffect(base)) return false;
            }
            if (!lasting) return false;   // instant (Stagger, Banish): nothing readable afterwards, inconclusive, never held
            a_why = "none of the spell's lasting effects is on the target";
            return true;
        }

        void PostSettle(Key a_key) {
            MainThread::Post([a_key]() {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_key.caster);
                auto* spell  = RE::TESForm::LookupByID<RE::SpellItem>(a_key.spell);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_key.target);
                Pending snap;
                {
                    std::scoped_lock lk(g_mx);
                    const auto it = g_pending.find(a_key);
                    if (it == g_pending.end()) return;
                    snap = it->second;
                }
                std::string why;
                bool hold = false;
                // A pair gone from the world (dead, unloaded) is not read: it is the end of the pair, not a zero effect.
                if (caster && spell && target && !target->IsDead() && target->Is3DLoaded())
                    hold = NoEffectOnTarget(spell, target, snap, why);
                bool fresh = false;
                {
                    std::scoped_lock lk(g_mx);
                    if (auto it = g_pending.find(a_key); it != g_pending.end()) {
                        it->second.doneAt = Clock::now();
                        it->second.asked  = false;
                    }
                    if (hold) fresh = g_held.Hold(a_key.caster, a_key.spell, a_key.target);
                }
                if (fresh)
                    spdlog::warn("[noeffect] caster {:08X} spell {:08X} \"{}\" at {:08X}: the cast FIRED and left no effect on this target "
                                 "({}) -- the (spell, target) is held off for the rest of the fight (WillWork answers No; no direct "
                                 "fallback). The effect is not on the target and nothing changed: check the effect's conditions and "
                                 "any immunity the record does not state (cast/NoEffect.cpp)",
                                 a_key.caster, a_key.spell, spell && spell->GetName() ? spell->GetName() : "?", a_key.target, why);
            });
        }

    }

    void NoEffectNoteFired(RE::Actor* a_caster, RE::SpellItem* a_spell, RE::Actor* a_target) {
        if (!a_caster || !a_spell || !a_target || a_target == a_caster) return;
        if (!MainThread::IsInstalled()) return;   // VR: no pump, no read (documented degrade, as WillWork)
        if (!HostileRow(ClassifyArchetype(a_spell).row)) return;
        const Key  key{ a_caster->GetFormID(), a_spell->GetFormID(), a_target->GetFormID() };
        const auto now = Clock::now();
        bool snapshot = false, settle = false;
        {
            std::scoped_lock lk(g_mx);
            if (g_held.Held(key.caster, key.spell, key.target)) return;
            if (g_pending.size() >= kMaxPending) {   // a bound; a cleared ledger only forgets open readings, never a hold
                for (auto it = g_pending.begin(); it != g_pending.end();)
                    it = (Set(it->second.doneAt) && Age(it->second.doneAt, now) > kRenoteSec) ? g_pending.erase(it) : std::next(it);
                if (g_pending.size() >= kMaxPending) g_pending.clear();
            }
            auto [it, isNew] = g_pending.try_emplace(key);
            Pending& p = it->second;
            if (isNew || (Set(p.doneAt) && Age(p.doneAt, now) >= kRenoteSec)) {
                p = Pending{};
                p.firedAt = now;   // a new cast cycle of the pair
                snapshot  = true;
            } else if (!Set(p.doneAt) && !p.asked && Age(p.firedAt, now) >= kSettleSec) {
                p.asked = true;
                settle  = true;
            }
        }
        if (snapshot) PostSnapshot(key);   // outside the mutex: Post takes its own queue lock
        if (settle) PostSettle(key);
    }

    bool NoEffectHeld(RE::FormID a_caster, RE::FormID a_spell, RE::FormID a_target) {
        const auto now = Clock::now();
        Key        key{ a_caster, a_spell, a_target };
        bool       settle = false;
        {
            std::scoped_lock lk(g_mx);
            if (g_held.Held(a_caster, a_spell, a_target)) return true;
            // The other driver of the settle reading: the picker keeps asking WillWork about the pair after the claim is gone.
            if (const auto it = g_pending.find(key); it != g_pending.end()) {
                Pending& p = it->second;
                if (!Set(p.doneAt) && !p.asked && Age(p.firedAt, now) >= kSettleSec) { p.asked = true; settle = true; }
            }
        }
        if (settle) PostSettle(key);
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
