// cast/WillWork.cpp -- G10 "SPELL WILL WORK" (class plan card G10, ClickUp 1dfcuabcqe7; marth 2026-11-09:
// "Needs to recognize when a spell will never work, that gambit is skipped if it wont work due to the
// level difference", "Turn undead needs the undead qualifier obviously").
//
// WillWork(caster, spell, target) -> {Yes, No(reason), Unknown}. A PURE READ: it mutates no actor
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
//   (c) THE v46 HOSTILE ROWS (adopt-seats, Harbinger ABI 46). For a spell whose predicted engine row is Stagger, Disarm,
//       TargetEffect or Paralyze, Harbinger no longer asks the engine "does this work on this target" for a claim (the
//       engine's equip condition is admitted, APMF_API.h CastSeatCoverage "THE CLIENT'S PART"), so MFO decides it alone:
//       the KEYED effect (SpellArchetype::keyedEffect, the one the engine keys the row by) must itself be unblocked (its
//       effect and MGEF conditions, which carry any keyword test, against the target), beside every level-capped effect
//       (check (a)); Disarm needs a target holding a droppable weapon; Banish / Command need a summoned (commanded) target.
//       The needs are the table kRows in cast/SeatRows.h (read here through NeedOf). Stagger / Paralyze IMMUNITY is not
//       exposed by the engine as a readable bit (a race's kNoKnockdowns is knockdown, not stagger; there is no paralysis
//       resist flag), so it is only what the MGEF conditions say; the zero-effect hold (cast/NoEffect.cpp) catches the rest.
//   (d) THE ZERO-EFFECT HOLD: a (spell, target) whose cast fired with no effect is No for the rest of the fight
//       (NoEffectHeld, cast/NoEffect.cpp). One reader of "will it work" stays this function.
// A spell with no capped archetype and no condition at all is Yes without any measurement (form data only).
//
// THREADING (the SelfStatus mirror shape). TESCondition and the perk entry point read live actor state, so the
// measurement runs on the MAIN thread, one MainThread::Post per (caster, spell, target) triple. The worker
// (Fire, on the AddTask job worker) only reads the mutex-guarded mirror. Fresh (<= kTrustSec) answers are
// served; an older one posts a refresh. With no fresh answer (never measured, stale, or a lost post, a load drops
// the pump queue, re-asked after kAskRetrySec) the verdict is Unknown and the rule FIRES as it did before this
// card, never held (a hold would invert priority across every CTDA-carrying spell), with ONE loud log line per
// spell. Fire's call is itself the prefetch: the first lap per triple fires unchecked, the verdict is ready for the next. No pump (VR): Unknown, never a
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
#include "cast/SeatRows.h"

#include <atomic>
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
        // answer arrives within a pump drain (a frame or two).
        constexpr double kRefreshSec  = 2.0;
        constexpr double kTrustSec    = 6.0;
        constexpr double kAskRetrySec = 1.5;
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

        bool HostileRowOf(EngineRow a_row, SeatRows::Row& a_out) {
            switch (a_row) {
            case EngineRow::Stagger:      a_out = SeatRows::Row::Stagger;      return true;
            case EngineRow::Disarm:       a_out = SeatRows::Row::Disarm;       return true;
            case EngineRow::TargetEffect: a_out = SeatRows::Row::TargetEffect; return true;
            case EngineRow::Paralyze:     a_out = SeatRows::Row::Paralyze;     return true;
            default: return false;
            }
        }

        // What the target must be for this MGEF archetype (the engine's equip condition used to ask it, SeatRows::Need).
        SeatRows::Need NeedOf(Arch a) {
            switch (a) {
            case Arch::kDisarm:                          return SeatRows::Need::Weapon;
            case Arch::kBanish: case Arch::kCommandSummoned: return SeatRows::Need::Summoned;
            default:                                     return SeatRows::Need::None;
            }
        }

        // MAIN THREAD. An equipped weapon in either hand that is not flagged cannot-drop and is not bare hands.
        bool HoldsDroppableWeapon(RE::Actor* a_target) {
            for (const bool left : { false, true }) {
                auto* o = a_target->GetEquippedObject(left);
                auto* w = o ? o->As<RE::TESObjectWEAP>() : nullptr;
                if (!w) continue;
                if (w->weaponData.animationType.get() == RE::WEAPON_TYPE::kHandToHandMelee) continue;
                if (w->weaponData.flags.all(RE::TESObjectWEAP::Data::Flag::kCantDrop)) continue;
                return true;
            }
            return false;
        }

        // FORM DATA ONLY (worker-safe): does this spell carry anything the check could block on?
        bool Gated(RE::SpellItem* a_spell) {
            SeatRows::Row hostile{};
            if (HostileRowOf(ClassifyArchetype(a_spell).row, hostile)) return true;   // the v46 rows: the keyed effect + the need
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
            // LEVEL CAP FIRST, ORDER-INDEPENDENT (Opus review 2026-11-19 F1): a level-capped effect is the one that keys the
            // spell's caster row (Calm / Frenzy / Demoralize / Rally / Turn Undead / Reanimate are control effects). When ANY of
            // them cannot land the spell does not work, even though an uncapped companion effect (Turn Lesser Undead's second
            // effect, 0004D3F9, field 1119: Jesper at a level-30 Deathlord) would, and wherever the engine lists it.
            for (const auto* e : a_spell->effects) {
                if (!e || !e->baseEffect) continue;
                const auto* base = e->baseEffect;
                if (!LevelCapped(base->GetArchetype())) continue;
                float cap = e->effectItem.magnitude;
                // ARITY IS PER ENTRY POINT: EP 0x1D (kModSpellMagnitude) is (owner, spell, TARGET REF, &value);
                // cast/Summon.cpp's EP 0x44 is (owner, spell, &value). Disassembly: 1.6.1170 0x5abe68 / 0x5ac400 /
                // 0x5ac414 pass r8=spell, r9=target, [rsp+0x20]=&magnitude (1.5.97 0x53dfa0 the same).
                RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude,
                                                    a_caster, static_cast<RE::MagicItem*>(a_spell),
                                                    static_cast<RE::TESObjectREFR*>(a_target), &cap);
                cap += WillWorkClassLevelBonus(a_caster, a_spell, e);
                const auto lvl = a_target->GetLevel();
                if (static_cast<float>(lvl) > cap) {
                    out.verdict = WillWorkVerdict::No;
                    out.reason  = std::format("'{}' affects up to level {:.0f}, target is level {}",
                                              base->GetName() ? base->GetName() : "effect", cap, lvl);
                    return out;
                }
            }
            // (c) THE v46 HOSTILE ROWS: the keyed effect alone must be unblocked, and the target must meet its need.
            {
                const auto arch = ClassifyArchetype(a_spell);
                SeatRows::Row hostile{};
                if (HostileRowOf(arch.row, hostile) && arch.keyedEffect >= 0 &&
                    static_cast<std::size_t>(arch.keyedEffect) < a_spell->effects.size()) {
                    const auto* ke   = a_spell->effects[static_cast<std::uint32_t>(arch.keyedEffect)];
                    const auto* kb   = ke ? ke->baseEffect : nullptr;
                    const char* kn   = kb && kb->GetName() ? kb->GetName() : "effect";
                    if (kb) {
                        if (ke->conditions && !ke->conditions.IsTrue(a_target, a_caster)) {
                            out.verdict = WillWorkVerdict::No;
                            out.reason  = std::format("the {} row's keyed effect '{}': the spell's effect conditions are false for the target",
                                                      SeatRows::Info(hostile).name, kn);
                            return out;
                        }
                        if (kb->conditions && !kb->conditions.IsTrue(a_target, a_caster)) {
                            out.verdict = WillWorkVerdict::No;
                            out.reason  = std::format("the {} row's keyed effect '{}': the magic effect's conditions (keywords included) are false for the target",
                                                      SeatRows::Info(hostile).name, kn);
                            return out;
                        }
                        const auto need = NeedOf(kb->GetArchetype());
                        if (!SeatRows::NeedMet(need, need == SeatRows::Need::Weapon && HoldsDroppableWeapon(a_target),
                                               a_target->IsSummoned() || a_target->IsCommandedActor())) {
                            out.verdict = WillWorkVerdict::No;
                            out.reason  = std::format("'{}': {}", kn,
                                                      need == SeatRows::Need::Weapon ? "the target holds no weapon that can be disarmed"
                                                                                     : "the target is not a summoned (commanded) creature");
                            return out;
                        }
                    }
                }
            }
            for (const auto* e : a_spell->effects) {
                if (!e || !e->baseEffect) continue;
                anyEffect = true;
                const auto* base = e->baseEffect;
                std::string blocked;
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

    // The engine's own distance for a v46 hostile row (cast/SeatRows.h kRows), 0 = none. The GMST is read live by name (a list may
    // change it) and kept once read; unreadable -> the vanilla default, not kept. MAIN THREAD or after kDataLoaded (the setting
    // collection is static data then).
    float RowReachLimit(EngineRow a_row) {
        SeatRows::Row row{};
        if (!HostileRowOf(a_row, row)) return 0.0f;
        const auto& info = SeatRows::Info(row);
        if (!info.reachGmst) return 0.0f;
        static std::atomic<float> s_kept[static_cast<std::size_t>(SeatRows::Row::kCount)];   // 0 = not read yet
        auto& kept = s_kept[static_cast<std::size_t>(row)];
        if (const float k = kept.load(std::memory_order_relaxed); k > 0.0f) return k;
        float live = 0.0f;
        if (auto* gsc = RE::GameSettingCollection::GetSingleton())
            if (auto* st = gsc->GetSetting(info.reachGmst)) live = st->GetFloat();
        const float r = SeatRows::RowReach(row, live);
        if (live > 0.0f) {
            kept.store(r, std::memory_order_relaxed);
            spdlog::info("[willwork] the {} row's reach, from {}: {:.0f} units{}", info.name, info.reachGmst, r,
                         r != info.reachDefault ? " (not the vanilla value)" : "");
        }
        return r;
    }

    void WillWorkClear() {
        std::scoped_lock lk(g_mx);
        g_cache.clear();
        g_loggedUnknown.clear();
    }

    WillWorkResult WillWork(RE::Actor* a_caster, RE::SpellItem* a_spell, RE::Actor* a_target) {
        WillWorkResult out;
        if (!a_caster || !a_spell || !a_target) return out;   // Unknown
        // (d) a cast of this spell already fired with no effect on this target this fight: refused, the hold is the verdict itself.
        if (NoEffectHeld(a_caster->GetFormID(), a_spell->GetFormID(), a_target->GetFormID())) {
            out.verdict = WillWorkVerdict::No;
            out.reason  = "an earlier cast of this spell fired with no effect on this target; held off for the fight";
            return out;
        }
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
            // A LOST post: an ask older than kAskRetrySec that never answered (the first lap's ask is not a loss).
            const bool lost     = !fresh && Set(e.asked) && Age(e.asked, now) >= kAskRetrySec;
            if (stale && (!Set(e.asked) || Age(e.asked, now) >= kAskRetrySec)) {
                e.asked = now;
                ask = true;
            }
            if (!fresh) {   // no usable answer: Unknown, the rule fires (loudly, once per spell)
                out.verdict = WillWorkVerdict::Unknown;
                logUnknown  = lost && g_loggedUnknown.insert(key.spell).second;
            }
        }
        if (ask) PostMeasure(key);   // outside the mutex: Post takes its own queue lock
        if (logUnknown)
            spdlog::warn("[willwork] spell {:08X}: no fresh measurement for caster {:08X} -> target {:08X}; the rule "
                         "fires unchecked (a measurement post was lost; logged once per spell)", key.spell, key.caster, key.target);
        return out;
    }

}
