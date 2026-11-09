// cast/WillWork.cpp -- G10 "SPELL WILL WORK" (class plan card G10, ClickUp 1dfcuabcqe7; marth 2026-11-09:
// "Needs to recognize when a spell will never work, that gambit is skipped if it wont work due to the
// level difference", "Turn undead needs the undead qualifier obviously").
//
// WillWork(caster, spell, target) -> {Yes, No(reason), Unknown, Measuring}. A PURE READ: it mutates no actor
// and no engine state. Two checks, per effect of the spell:
//   (a) LEVEL CAP. For the level-capped archetypes (Calm, Frenzy, Demoralize/Fear, Rally, Turn Undead,
//       Reanimate) the effect magnitude IS the highest level it affects. Effective cap = the effect magnitude
//       after the caster's kModSpellMagnitude perk entry point (the cast/Summon.cpp SummonLimit road:
//       BGSEntryPoint::HandleEntryPoint(ep, caster, MagicItem*, &value)) + WillWorkClassLevelBonus (the card
//       C6 hook, 0 today). Blocked when the target's level (Actor::GetLevel) exceeds the cap. Paralysis is NOT
//       in the set: vanilla Paralysis has no level cap; a list that levels it does so with an effect
//       condition, which check (b) reads.
//   (b) TARGET CONDITIONS. The effect's own CTDA lists (Effect::conditions on the SPEL and
//       EffectSetting::conditions on the MGEF) evaluated with (subject = target, other = caster), so an
//       "undead only" Turn Undead or a "not undead" immunity is caught.
// A spell WORKS when at least one of its effects is not blocked; it is No only when EVERY effect is blocked.
// A spell with no capped archetype and no condition at all is Yes without any measurement (form data only).
//
// THREADING (the SelfStatus mirror shape). TESCondition and the perk entry point read live actor state, so the
// measurement runs on the MAIN thread, one MainThread::Post per (caster, spell, target) triple. The worker
// (Fire, on the AddTask job worker) only reads the mutex-guarded mirror. Fresh (<= kTrustSec) answers are
// served; an older one posts a refresh. While the FIRST answer of a gated spell is in flight (<= kHoldSec) the
// verdict is Measuring (Fire holds that one lap transparently so the first cast is not wasted); a lost post
// (a load drops the pump queue) is re-asked after kAskRetrySec and, past kHoldSec, the verdict is Unknown, the
// rule fires as it did before this card, with ONE loud log line per spell. No pump (VR): Unknown, never a
// worker-side read. The mirror ages out by itself (no co-save, no ResetAllState hook; WillWorkClear exists
// for a future caller).
//
// FORK FIELDS READ (CommonLibSSE-NG mit-3.7 fork): Effect::effectItem.magnitude, ::baseEffect, ::conditions
// (Effect.h:34-38,43), EffectSetting::GetArchetype (EffectSetting.h:131), EffectSetting::conditions (:155),
// TESCondition::operator bool / IsTrue(TESObjectREFR*, TESObjectREFR*) (TESCondition.h:999-1002),
// Actor::GetLevel (Actor.h:551), BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude (BGSEntryPoint.h:42),
// MagicItem::effects (MagicItem.h:126).
#include "Actuation_internal.h"
#include "MainThread.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace MFO::Actuation {

    namespace {

        using Clock = std::chrono::steady_clock;
        using Arch  = RE::EffectArchetype;

        // Sizes from the real cadence (principle 9). A follower's cast gambit re-asks on the cast cadence
        // (seconds apart), a target's level never changes and a condition ("is undead") rarely does, so a
        // verdict is trusted for kTrustSec and refreshed in the background once kRefreshSec old. The first
        // answer of a gated spell arrives within a pump drain (a frame or two); kHoldSec is a generous bound
        // before the hold gives up and the rule fires as it did before G10.
        constexpr double kRefreshSec  = 2.0;
        constexpr double kTrustSec    = 6.0;
        constexpr double kAskRetrySec = 1.5;
        constexpr double kHoldSec     = 3.0;
        constexpr std::size_t kMaxEntries = 256;

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
        struct Entry {
            WillWorkVerdict   verdict = WillWorkVerdict::Unknown;   // the last MAIN-thread answer (Yes / No / Unknown)
            std::string       reason;
            Clock::time_point stamp{};      // when it finished (epoch = never measured)
            Clock::time_point asked{};      // when the refresh in flight was posted (epoch = none)
            Clock::time_point firstAsk{};   // when the unanswered ask streak began (epoch = none)
        };
        std::mutex                                 g_mx;
        std::unordered_map<Key, Entry, KeyHash>    g_cache;
        std::unordered_set<RE::FormID>             g_loggedUnknown;   // one line per spell (guarded by g_mx)

        double Age(const Clock::time_point& a_then, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_then).count();
        }
        bool Set(const Clock::time_point& t) { return t.time_since_epoch().count() != 0; }

        bool LevelCapped(Arch a) {
            switch (a) {
            case Arch::kCalm: case Arch::kFrenzy: case Arch::kDemoralize: case Arch::kRally:
            case Arch::kTurnUndead: case Arch::kReanimate:
                return true;
            default: return false;
            }
        }

        // FORM DATA ONLY (worker-safe): does this spell carry anything the check could block on?
        bool Gated(const RE::SpellItem* a_spell) {
            for (const auto* e : a_spell->effects) {
                if (!e || !e->baseEffect) continue;
                if (LevelCapped(e->baseEffect->GetArchetype())) return true;
                if (e->conditions || e->baseEffect->conditions) return true;
            }
            return false;
        }

        // MAIN THREAD ONLY. Returns the verdict and, for No, the reason.
        WillWorkResult Measure(RE::Actor* a_caster, RE::SpellItem* a_spell, RE::Actor* a_target) {
            WillWorkResult out;
            out.verdict = WillWorkVerdict::Yes;
            bool        anyEffect = false;
            std::string firstBlock;
            for (const auto* e : a_spell->effects) {
                if (!e || !e->baseEffect) continue;
                anyEffect = true;
                const auto* base = e->baseEffect;
                std::string blocked;
                if (LevelCapped(base->GetArchetype())) {
                    float cap = e->effectItem.magnitude;
                    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude,
                                                        a_caster, static_cast<RE::MagicItem*>(a_spell), &cap);
                    cap += WillWorkClassLevelBonus(a_caster, a_spell, e);
                    const auto lvl = a_target->GetLevel();
                    if (static_cast<float>(lvl) > cap)
                        blocked = std::format("'{}' affects up to level {:.0f}, target is level {}",
                                              base->GetName() ? base->GetName() : "effect", cap, lvl);
                }
                if (blocked.empty()) {
                    // Subject = the one the effect lands on, the other party = the caster.
                    if (e->conditions && !e->conditions.IsTrue(a_target, a_caster))
                        blocked = std::format("'{}': the spell's effect conditions are false for the target",
                                              base->GetName() ? base->GetName() : "effect");
                    else if (base->conditions && !base->conditions.IsTrue(a_target, a_caster))
                        blocked = std::format("'{}': the magic effect's conditions are false for the target",
                                              base->GetName() ? base->GetName() : "effect");
                }
                if (blocked.empty()) return out;   // one working effect = the spell works
                if (firstBlock.empty()) firstBlock = std::move(blocked);
            }
            if (anyEffect) { out.verdict = WillWorkVerdict::No; out.reason = std::move(firstBlock); }
            return out;
        }

        void PostMeasure(Key a_key) {
            MainThread::Post([a_key]() {
                WillWorkResult r;   // Unknown unless a loaded, living pair is readable
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_key.caster);
                auto* spell  = RE::TESForm::LookupByID<RE::SpellItem>(a_key.spell);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_key.target);
                if (caster && spell && target && caster->Is3DLoaded() && target->Is3DLoaded())
                    r = Measure(caster, spell, target);
                bool changed = false;
                {
                    std::scoped_lock lk(g_mx);
                    auto& e = g_cache[a_key];
                    changed    = (e.verdict != r.verdict) && (r.verdict == WillWorkVerdict::No || Set(e.stamp));
                    e.verdict  = r.verdict;
                    e.reason   = r.reason;
                    e.stamp    = Clock::now();
                    e.asked    = Clock::time_point{};
                    e.firstAsk = Clock::time_point{};
                }
                if (changed)   // verdict transitions only: a stable pair logs once, not every refresh
                    spdlog::info("[willwork] caster {:08X} spell {:08X} -> target {:08X}: {}{}", a_key.caster,
                                 a_key.spell, a_key.target,
                                 r.verdict == WillWorkVerdict::No ? "WILL NOT WORK -- " : "will work",
                                 r.reason);
            });
        }

    }

    // CARD C6 HOOK: class passives that add levels to a level-capped control spell (Warlock +8 and similar).
    // Returns the levels to ADD to the effect's cap. Deliberately 0 here: G10 does not implement passives.
    float WillWorkClassLevelBonus(RE::Actor*, RE::SpellItem*, const RE::Effect*) { return 0.0f; }

    void WillWorkClear() {
        std::scoped_lock lk(g_mx);
        g_cache.clear();
    }

    WillWorkResult WillWork(RE::Actor* a_caster, RE::SpellItem* a_spell, RE::Actor* a_target) {
        WillWorkResult out;
        if (!a_caster || !a_spell || !a_target) return out;   // Unknown
        if (!Gated(a_spell)) { out.verdict = WillWorkVerdict::Yes; return out; }
        if (!MainThread::IsInstalled()) return out;           // VR: no pump, no read (Unknown, documented degrade)

        const Key  key{ a_caster->GetFormID(), a_spell->GetFormID(), a_target->GetFormID() };
        const auto now = Clock::now();
        bool ask = false;
        bool logUnknown = false;
        {
            std::scoped_lock lk(g_mx);
            if (g_cache.size() >= kMaxEntries) {   // bound the mirror: drop anything past its trust
                for (auto it = g_cache.begin(); it != g_cache.end();)
                    it = (Set(it->second.stamp) && Age(it->second.stamp, now) > kTrustSec && !Set(it->second.asked))
                             ? g_cache.erase(it) : std::next(it);
                if (g_cache.size() >= kMaxEntries) g_cache.clear();
            }
            auto& e = g_cache[key];
            const bool measured = Set(e.stamp);
            const bool fresh    = measured && Age(e.stamp, now) <= kTrustSec;
            if (fresh) { out.verdict = e.verdict; out.reason = e.reason; }
            const bool stale    = !measured || Age(e.stamp, now) >= kRefreshSec;
            if (stale && (!Set(e.asked) || Age(e.asked, now) >= kAskRetrySec)) {
                e.asked = now;
                if (!Set(e.firstAsk)) e.firstAsk = now;
                ask = true;
            }
            if (!fresh) {
                // No usable answer. Hold (Measuring) while the ask streak is young; past kHoldSec the post is
                // presumed lost and the rule fires as before (Unknown), loudly, once per spell.
                if (Set(e.firstAsk) && Age(e.firstAsk, now) < kHoldSec) out.verdict = WillWorkVerdict::Measuring;
                else {
                    out.verdict = WillWorkVerdict::Unknown;
                    logUnknown  = g_loggedUnknown.insert(key.spell).second;
                }
            }
        }
        if (ask) PostMeasure(key);   // outside the mutex: Post takes its own queue lock
        if (logUnknown)
            spdlog::warn("[willwork] spell {:08X}: no fresh measurement for caster {:08X} -> target {:08X}; the rule "
                         "fires unchecked (logged once per spell)", key.spell, key.caster, key.target);
        return out;
    }

}
