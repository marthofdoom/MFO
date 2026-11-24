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
//       (check (a)); Disarm needs a droppable right-hand weapon on a non-essential target; Command needs a summoned (commanded) target (Banish: conditions only). A No needs no other unblocked hostile damage effect, never on an approximate row, random conditions read as pass.
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
            case Arch::kCommandSummoned:                 return SeatRows::Need::Summoned;   // Banish: conditions only (review F6)
            default:                                     return SeatRows::Need::None;
            }
        }

        // True when any item of the list tests GetRandomPercent: a random condition must never flip an answer (review F4), so a list
        // that carries one is read as PASS for the keyed check (the engine rolls it per cast).
        bool HasRandomCondition(const RE::TESCondition& a_cond) {
            for (auto* it = a_cond.head; it; it = it->next)
                if (it->data.functionData.function.get() == RE::FUNCTION_DATA::FunctionID::kGetRandomPercent) return true;
            return false;
        }
        bool CondFalse(const RE::TESCondition& a_cond, RE::Actor* a_target, RE::Actor* a_caster) {
            return a_cond && !HasRandomCondition(a_cond) && !a_cond.IsTrue(a_target, a_caster);
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

        // fix/spell-worth SEV-4: a working HEALTH damage effect (hostile value modifier on health, not itself level-capped, conditions not false for the target).
        // A spell that has one is a damage spell: a level-capped rider on it (Tuxborn puts one on nearly every Destruction spell) cannot make it "not work".
        bool HasWorkingHealthDamage(RE::SpellItem* a_spell, RE::Actor* a_target, RE::Actor* a_caster) {
            for (const auto* e : a_spell->effects) {
                const auto* b = e ? e->baseEffect : nullptr;
                if (!b || !b->IsHostile() || LevelCapped(b->GetArchetype())) continue;
                const auto a = b->GetArchetype();
                if (!(a == Arch::kValueModifier || a == Arch::kPeakValueModifier || a == Arch::kDualValueModifier)) continue;
                if (b->data.primaryAV != RE::ActorValue::kHealth) continue;
                if (CondFalse(e->conditions, a_target, a_caster) || CondFalse(b->conditions, a_target, a_caster)) continue;
                return true;
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
            const bool damageSpell = HasWorkingHealthDamage(a_spell, a_target, a_caster);   // exempt from the cap rule (a control spell, Turn Lesser Undead included, has none)
            for (const auto* e : a_spell->effects) {
                if (damageSpell) break;
                if (!e || !e->baseEffect) continue;
                const auto* base = e->baseEffect;
                if (!LevelCapped(base->GetArchetype())) continue;
                const float cap = WillWorkLevelCap(a_caster, a_spell, e, a_target);
                const auto lvl = a_target->GetLevel();
                if (static_cast<float>(lvl) > cap) {
                    out.verdict = WillWorkVerdict::No;
                    out.reason  = std::format("'{}' affects up to level {:.0f}, target is level {}",
                                              base->GetName() ? base->GetName() : "effect", cap, lvl);
                    return out;
                }
            }
            // (c) THE v46 HOSTILE ROWS: the keyed effect must be unblocked and the target must meet its need. Review F4: this is a No ONLY
            // when nothing else in the spell can hurt him: never on an approximate row (several rows compete, the keyed effect is a guess),
            // never while another hostile damage effect is unblocked (the cast still does something), random conditions read as pass. The
            // level-capped rule above stays first (Turn Lesser Undead at a Deathlord is still No).
            {
                const auto arch = ClassifyArchetype(a_spell);
                SeatRows::Row hostile{};
                if (!arch.rowApprox && HostileRowOf(arch.row, hostile) && arch.keyedEffect >= 0 &&
                    static_cast<std::size_t>(arch.keyedEffect) < a_spell->effects.size()) {
                    const auto* ke   = a_spell->effects[static_cast<std::uint32_t>(arch.keyedEffect)];
                    const auto* kb   = ke ? ke->baseEffect : nullptr;
                    const char* kn   = kb && kb->GetName() ? kb->GetName() : "effect";
                    std::string why;
                    if (kb) {
                        if (CondFalse(ke->conditions, a_target, a_caster))
                            why = std::format("the {} row's keyed effect '{}': the spell's effect conditions are false for the target", SeatRows::Info(hostile).name, kn);
                        else if (CondFalse(kb->conditions, a_target, a_caster))
                            why = std::format("the {} row's keyed effect '{}': the magic effect's conditions (keywords included) are false for the target", SeatRows::Info(hostile).name, kn);
                        else {
                            const auto need = NeedOf(kb->GetArchetype());
                            if (need == SeatRows::Need::Weapon && DisarmableRightWeapon(a_target) == 0)
                                why = std::format("'{}': the target holds no weapon in his right hand that can be disarmed", kn);
                            else if (need == SeatRows::Need::Summoned && !(a_target->IsSummoned() || a_target->IsCommandedActor()))
                                why = std::format("'{}': the target is not a summoned (commanded) creature", kn);
                        }
                    }
                    if (!why.empty()) {
                        // Another unblocked hostile damage effect keeps the spell a working one.
                        bool otherWorks = false;
                        for (std::int32_t i = 0; i < static_cast<std::int32_t>(a_spell->effects.size()) && !otherWorks; ++i) {
                            if (i == arch.keyedEffect) continue;
                            const auto* oe = a_spell->effects[static_cast<std::uint32_t>(i)];
                            const auto* ob = oe ? oe->baseEffect : nullptr;
                            if (!ob || !ob->IsHostile()) continue;
                            const auto oa = ob->GetArchetype();
                            const bool damage = (oa == Arch::kValueModifier || oa == Arch::kPeakValueModifier || oa == Arch::kDualValueModifier ||
                                                 oa == Arch::kAbsorb) &&
                                                (ob->data.primaryAV == RE::ActorValue::kHealth || ob->data.primaryAV == RE::ActorValue::kMagicka ||
                                                 ob->data.primaryAV == RE::ActorValue::kStamina);
                            if (damage && !CondFalse(oe->conditions, a_target, a_caster) && !CondFalse(ob->conditions, a_target, a_caster))
                                otherWorks = true;
                        }
                        if (!otherWorks) { out.verdict = WillWorkVerdict::No; out.reason = std::move(why); return out; }
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

    // See Actuation.h. The level a capped effect reaches: the ONE reader Measure (above) and iaus/Fills.cpp (generation) share.
    bool LevelCappedEffect(const RE::Effect* a_effect) {
        return a_effect && a_effect->baseEffect && LevelCapped(a_effect->baseEffect->GetArchetype());
    }
    float WillWorkLevelCap(RE::Actor* a_caster, RE::SpellItem* a_spell, const RE::Effect* a_effect, RE::Actor* a_target) {
        float cap = a_effect->effectItem.magnitude;
        // ARITY IS PER ENTRY POINT: EP 0x1D (kModSpellMagnitude) is (owner, spell, TARGET REF, &value);
        // cast/Summon.cpp's EP 0x44 is (owner, spell, &value). Disassembly: 1.6.1170 0x5abe68 / 0x5ac400 /
        // 0x5ac414 pass r8=spell, r9=target, [rsp+0x20]=&magnitude (1.5.97 0x53dfa0 the same).
        if (a_target)   // a null target is not a proven argument of the entry point: the record magnitude stands (iaus/Fills.cpp, no combat target)
            RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, a_caster,
                                                static_cast<RE::MagicItem*>(a_spell), static_cast<RE::TESObjectREFR*>(a_target), &cap);
        return cap + WillWorkClassLevelBonus(a_caster, a_spell, a_effect);
    }

    // See Actuation.h. The same test the NoEffect reading uses. A right-hand weapon only (the engine's Disarm takes the right hand),
    // not bare hands, not cannot-drop, and an essential target is refused (review F5; AE 0x678BD0 is the engine's gate).
    RE::FormID DisarmableRightWeapon(RE::Actor* a_target) {
        if (!a_target || a_target->IsEssential()) return 0;
        auto* o = a_target->GetEquippedObject(false);
        auto* w = o ? o->As<RE::TESObjectWEAP>() : nullptr;
        if (!w) return 0;
        if (w->weaponData.animationType.get() == RE::WEAPON_TYPE::kHandToHandMelee) return 0;
        if (w->weaponData.flags.all(RE::TESObjectWEAP::Data::Flag::kCantDrop)) return 0;
        return w->GetFormID();
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
