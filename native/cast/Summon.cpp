// cast/Summon.cpp -- SUMMONS: liveness (CasterHasLiveSummon) and the one-shot
// conjure (CastSummonOnce + its main-thread SummonOnMain).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "ComposedCast.h"   // the Composed Forced Cast executor -- replaces the deleted
                            // HealAnimFill package route at the two cast plug-ins below
#include "CastBounds.h"     // Reset the MFO-executed-cast bound beside ConcProxy::Reset()
#include "Runtime.h"        // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "APMFBridge.h"     // feat/cast-gambit-concentration (Task 1): ClaimOffenseCast for a
                            // non-heal (Offense/Buff) CONCENTRATION stream -- ComposedCast::Try
                            // above is HEAL-ONLY by design, so offense/buff concentration needs
                            // its OWN direct claim call here rather than a widened Try() gate.

namespace MFO::Actuation {

    // ── SUMMON LIVENESS (v1.1.1) ────────────────────────────────────────────
    // A conjured familiar/atronach (SummonCreatureEffect) or a reanimated corpse
    // (ReanimateEffect) is a COMMANDED ACTOR, not a lingering caster-side magic
    // effect. The OOC cast routes' already-active guards read a magic effect on
    // the cast TARGET (HasMagicEffect / effect-duration scan on `tgt`), which for a
    // self-delivered summon is never the caster and never carries the summon
    // effect -- so the guard misses it entirely and a "cast [summon]" gambit re-
    // summons every cadence (marth, field-confirmed). The authoritative signal is
    // the COMMANDED ACTOR: scan the caster's own active effects for a summon/
    // reanimate effect THIS spell created and check its commandedActor still
    // resolves to a live actor. PER-SPELL (ae->spell == a_spell) so distinct
    // conjures track independently (a Twin Souls pair of different summons is
    // naturally allowed); keyed on the LIVE actor so a killed/expired/despawned
    // summon (handle gone or dead) frees an immediate recast. A non-summon spell
    // (candlelight/flesh/heal) carries no such archetype -> always false, so those
    // paths stay byte-identical. Reads the active-effect list only (no mutation).
    bool CasterHasLiveSummon(RE::Actor* a_caster, RE::SpellItem* a_spell) {
        if (!a_caster || !a_spell) return false;
        auto* mt = a_caster->AsMagicTarget();
        if (!mt) return false;
        auto* list = mt->GetActiveEffectList();
        if (!list) return false;
        for (auto* ae : *list) {
            if (!ae || ae->spell != a_spell) continue;
            RE::ActorHandle h{};
            if (auto* se = skyrim_cast<RE::SummonCreatureEffect*>(ae))      h = se->commandedActor;
            else if (auto* re = skyrim_cast<RE::ReanimateEffect*>(ae))      h = re->commandedActor;
            else continue;   // this spell's effect here is not a summon/reanimate
            if (auto cmd = h.get(); cmd && !cmd->IsDead() && !cmd->IsDeleted())
                return true;   // a live commanded summon from this spell is up
        }
        return false;
    }

    namespace {
        // ── THREADING (review of 78f9631) ──────────────────────────────────
        // Every ENGINE read a summon decision needs -- the caster's active-
        // effect list, commanded-actor handles, the process's commandedActors
        // array, the GMSTs and the perk entry point -- runs on the TRUE MAIN
        // thread, inside ONE posted closure that re-checks and then casts or
        // skips. The worker only decides "this rule wants to summon", applies
        // the cheap competence/magicka gates, and reads back the main thread's
        // last verdict (g_summon, leaf mutex) so it does not post every eval.
        //
        // The WORKER RESULT IS ALWAYS a transparent NoOp: the summon holds no
        // hand and no channel, so it must never occupy the tick or wall off
        // the rules below it. Whether it actually cast is the main thread's
        // [summon] line. Nothing double-casts: the main closure itself refuses
        // while the spell is live or LANDING, and a second queued closure sees
        // the first one's lastCast.
    }
        std::mutex                                     g_summonMx;   // leaf: never held across an engine call
        std::unordered_map<std::uint64_t, SummonState> g_summon;     // RecastKey(follower, spell)
    namespace {

        // A main verdict of Live/Landing/Limit is trusted this long before the
        // worker asks again. It bounds how late a killed summon is noticed.
        constexpr float kSummonVerdictFreshSec = 1.0f;
        // Worker-side post throttle (WORKER-SERIAL, #4): at most one queued
        // check per (follower, spell) per this many seconds.
        constexpr float kSummonPostEverySec    = 0.5f;
    }
        std::unordered_map<std::uint64_t, SelfClock::time_point> g_summonPosted;
    namespace {
        // Added to fMagicSummonMaxAppearTime for the fallback floor (before
        // the effect exists on the caster).
        constexpr float kSummonAppearMarginSec = 1.0f;

        float SecSince(SelfClock::time_point a_t, SelfClock::time_point a_now) {
            if (a_t.time_since_epoch().count() == 0) return 1.0e9f;
            return std::chrono::duration<float>(a_now - a_t).count();
        }

        // MAIN THREAD. GMST read by name; the fallback is the engine default.
        float GmstFloat(const char* a_name, float a_default) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* s   = gsc ? gsc->GetSetting(a_name) : nullptr;
            return s ? s->GetFloat() : a_default;
        }
        std::int32_t GmstInt(const char* a_name, std::int32_t a_default) {
            auto* gsc = RE::GameSettingCollection::GetSingleton();
            auto* s   = gsc ? gsc->GetSetting(a_name) : nullptr;
            return s ? s->GetSInt() : a_default;
        }

        // MAIN THREAD. The caster's summon limit, computed EXACTLY as the
        // engine's add-commanded-actor function does (1.6.1170 0x717800 id
        // 40056, 1.5.97 0x683D70 id 38993; both disassembled): limit =
        // (float)iMaxSummonedCreatures, then HandleEntryPoint(0x44
        // kModCommandedActorLimit, caster, spell, &limit) (RELOCATION_ID(23073,
        // 23526) -> 1.5.97 0x32ECE0 / 1.6.1170 0x385F30, the call target at
        // 0x683E11 / 0x7178EC), then round half-up (trunc + (frac >= 0.5)).
        int SummonLimit(RE::Actor* a_caster, RE::SpellItem* a_spell) {
            float limit = static_cast<float>(
                static_cast<std::uint32_t>(GmstInt("iMaxSummonedCreatures", 1)));
            RE::BGSEntryPoint::HandleEntryPoint(
                RE::BGSEntryPoint::ENTRY_POINT::kModCommandedActorLimit, a_caster,
                static_cast<RE::MagicItem*>(a_spell), &limit);
            const int t = static_cast<int>(limit);
            return t + ((limit - static_cast<float>(t)) >= 0.5f ? 1 : 0);
        }

        // MAIN THREAD. The engine SKIPS the cap when bit 1 of +0x340 of the
        // object at the global RELOCATION_ID(516851, 403330) is set: the add-
        // commanded-actor fn (RELOCATION_ID(38993, 40056)) does `mov rax,[rip+g];
        // mov ecx,[rax+0x340]; shr ecx,1; test cl,1` (1.5.97 0x683DC1 -> global
        // 0x2F266F8, 1.6.1170 0x7178A1 -> 0x317ABA8). MFO mirrors it. The read is
        // gated on the F1 self-check `ripref` row (Actuation.SummonCap.*: the
        // function holds exactly that instruction + use site at its verified RVA
        // and the RIP target is the library's answer for the global). If the row
        // did not verify, the flag is NOT read and MFO caps (conservative),
        // after one loud [selfcheck] line per seat.
        bool SummonCapSkipped() {
            static const bool verified = [] {
                // No table for this build -> never even resolve the ids (a missing
                // id is FATAL in the fork); cap, loudly, like a failed row.
                if (!Runtime::SelfCheckResult().Covered()) {
                    spdlog::error("[summon] the engine's skip-cap flag is NOT read on this build (no "
                                  "self-check table) -- MFO always applies the summon limit");
                    return false;
                }
                const REL::Relocation<std::uintptr_t> fn{ REL::RelocationID(38993, 40056) };
                const REL::Relocation<std::uintptr_t> gl{ REL::RelocationID(516851, 403330) };
                // A row with a byte check verifies base + rva + bytesOffset: the
                // `mov rax,[rip+g]` itself (+0x51 on 1.5.97, +0xA1 on 1.6.1170).
                const std::uintptr_t refAt = fn.address() + (REL::Module::IsAE() ? 0xA1u : 0x51u);
                const bool okFn = Runtime::SeatVerified(refAt, "Actuation.SummonCap.AddCommandedActor");
                const bool okGl = Runtime::SeatVerified(gl.address(), "Actuation.SummonCap.SkipFlagGlobal");
                if (!(okFn && okGl))
                    spdlog::error("[summon] the engine's skip-cap flag is NOT read on this build (self-check row "
                                  "not verified) -- MFO always applies the summon limit");
                return okFn && okGl;
            }();
            if (!verified) return false;
            static const REL::Relocation<std::uintptr_t> gl{ REL::RelocationID(516851, 403330) };
            const auto* obj = *reinterpret_cast<const std::uint8_t* const*>(gl.address());
            if (!obj) return false;
            return ((*reinterpret_cast<const std::uint32_t*>(obj + 0x340) >> 1) & 1u) != 0;
        }

        // MAIN THREAD. Is this ActiveEffect a summon still APPEARING? A live
        // (not inactive/dispelled) SummonCreatureEffect whose commanded handle
        // does not resolve yet, younger than fMagicSummonMaxAppearTime (the
        // engine's own appear window, read in the SummonCreatureEffect update).
        bool SummonAppearing(RE::ActiveEffect* a_ae, float a_appearSec) {
            auto* se = skyrim_cast<RE::SummonCreatureEffect*>(a_ae);
            if (!se) return false;
            if (a_ae->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled))
                return false;
            if (se->commandedActor.get()) return false;   // resolved -> live or dead, not appearing
            return a_ae->elapsedSeconds < a_appearSec;
        }

        // MAIN THREAD. The whole decision + cast for one summon rule.
        void SummonOnMain(RE::FormID a_id, RE::FormID a_spellID, int a_rule,
                          const std::string& a_table, const std::string& a_target) {
            auto* f  = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!f || !sp) return;
            const auto  key    = RecastKey(a_id, a_spellID);
            const auto  now    = SelfClock::now();
            const char* name   = sp->GetName() ? sp->GetName() : "?";
            const char* fname  = f->GetName() ? f->GetName() : "?";
            const float appear = GmstFloat("fMagicSummonMaxAppearTime", 4.0f);

            auto setVerdict = [&](SummonVerdict a_v) {
                std::lock_guard lk(g_summonMx);
                auto& s = g_summon[key];
                s.verdict = a_v; s.verdictAt = now;
                if (a_v == SummonVerdict::Cast) s.lastCast = now;
            };
            SummonState st{};
            {
                std::lock_guard lk(g_summonMx);
                st = g_summon[key];
            }
            auto skipLog = [&](const char* a_why) {
                if (SecSince(st.lastSkipLog, now) < 10.0f) return;
                {
                    std::lock_guard lk(g_summonMx);
                    g_summon[key].lastSkipLog = now;
                }
                spdlog::info("[summon] {:08X} {} ({:08X}) rule {} ({}, target {}): skipped -- {}",
                             a_id, name, a_spellID, a_rule, a_table, a_target, a_why);
            };

            // 1. PER SPELL: a live creature from THIS spell blocks only this spell.
            if (CasterHasLiveSummon(f, sp)) {
                setVerdict(SummonVerdict::Live);
                skipLog("this spell's creature is still alive");
                return;
            }
            // 2. LANDING: read the engine. An appearing effect of THIS spell on
            //    the caster; else (no effect yet) the fallback floor after our
            //    own last cast (appear time + margin).
            bool ownEffect = false, ownAppearing = false;
            int  otherAppearing = 0;
            std::vector<RE::FormID> otherSpellsWithEffect;
            if (auto* mt = f->AsMagicTarget()) {
                if (auto* list = mt->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (!ae || !ae->spell) continue;
                        if (!skyrim_cast<RE::SummonCreatureEffect*>(ae)) continue;
                        const bool mine = ae->spell == sp;
                        if (mine) ownEffect = true;
                        else      otherSpellsWithEffect.push_back(ae->spell->GetFormID());
                        if (SummonAppearing(ae, appear)) {
                            if (mine) ownAppearing = true;
                            else      ++otherAppearing;
                        }
                    }
                }
            }
            if (ownAppearing ||
                (!ownEffect && SecSince(st.lastCast, now) < appear + kSummonAppearMarginSec)) {
                setVerdict(SummonVerdict::Landing);
                skipLog("this spell's creature is still appearing");
                return;
            }
            // 3. THE LIST'S SUMMON LIMIT. listed = EVERY commandedActors entry,
            //    the raw count the engine itself compares (1.5.97 0x683E45 /
            //    1.6.1170 0x7178F5 read [middleHigh+0x110] and never check
            //    liveness; Reanimate thralls count too). Counting only live ones
            //    would let MFO cast into a dead-but-listed slot and the engine
            //    would then evict a LIVE older summon. pending = OTHER summon
            //    spells this follower cast that are still appearing (engine
            //    effect), or whose effect does not exist yet but were cast inside
            //    the fallback floor. A lower-ranked summon already out keeps its
            //    slot until it dies -- that is the list's rule.
            int live = 0;
            if (auto* proc = f->GetActorRuntimeData().currentProcess; proc && proc->middleHigh)
                live = static_cast<int>(proc->middleHigh->commandedActors.size());
            int pendingFloor = 0;
            {
                std::lock_guard lk(g_summonMx);
                for (const auto& [k, s] : g_summon) {
                    if (static_cast<RE::FormID>(k >> 32) != a_id) continue;
                    const auto other = static_cast<RE::FormID>(k & 0xFFFFFFFFu);
                    if (other == a_spellID) continue;
                    if (SecSince(s.lastCast, now) >= appear + kSummonAppearMarginSec) continue;
                    if (std::find(otherSpellsWithEffect.begin(), otherSpellsWithEffect.end(), other) !=
                        otherSpellsWithEffect.end())
                        continue;   // its effect exists: counted by the engine read instead
                    ++pendingFloor;
                }
            }
            const int  pending = otherAppearing + pendingFloor;
            const int  limit   = SummonLimit(f, sp);
            const bool noCap = SummonCapSkipped();
            if (!noCap && live + pending >= limit) {
                setVerdict(SummonVerdict::Limit);
                if (SecSince(st.lastLimitLog, now) >= 10.0f) {
                    {
                        std::lock_guard lk(g_summonMx);
                        g_summon[key].lastLimitLog = now;
                    }
                    spdlog::info("[summon] {:08X} {} ({:08X}) rule {} ({}, target {}): skipped -- "
                                 "summon limit reached (listed {} + appearing {} >= limit {})",
                                 a_id, name, a_spellID, a_rule, a_table, a_target, live, pending, limit);
                }
                return;
            }
            // 4. CAST ONCE, by the caster, on the direct road: CastSpellImmediate
            //    (kInstant) on the caster, magicka deducted by hand (it spends
            //    none), clamped to the pool.
            auto* caster = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            auto* avo    = f->AsActorValueOwner();
            const float pool = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
            const float cost = sp->CalculateMagickaCost(f);
            if (!caster || (avo && pool < cost)) {
                setVerdict(SummonVerdict::Failed);
                spdlog::info("[summon] {:08X} {} ({:08X}) rule {} ({}, target {}): not cast -- {}",
                             a_id, name, a_spellID, a_rule, a_table, a_target,
                             caster ? "magicka ran short before the cast" : "no instant magic caster");
                return;
            }
            caster->CastSpellImmediate(sp, false, f, 1.0f, false, 0.0f, f);
            const float spend = avo ? std::min(cost, pool) : 0.0f;
            if (avo && spend > 0.0f)
                avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -spend);
            setVerdict(SummonVerdict::Cast);
            spdlog::info("[summon] {:08X} {} {} ({:08X}) rule {} ({}, target {}): road=direct, cast once "
                         "(magicka -{:.0f}; summons listed {} + appearing {} of limit {}{})",
                         a_id, fname, name, a_spellID, a_rule, a_table, a_target, spend,
                         live, pending, limit, noCap ? ", cap skipped by the engine flag" : "");
        }
    }

    Outcome CastSummonOnce(RE::Actor* a_follower, RE::SpellItem* a_spell, int a_rule,
                           const char* a_table, std::string_view a_target) {
        if (!Runtime::CastPathsVerified())
            return { Result::FailedOther,
                     "cast control is verified on 1.6 and 1.5.97 only (this runtime uses the follower's own AI casting)", true };
        if (!a_follower || !a_spell) return { Result::FailedOther, "no summon spell", true };
        if (!MainThread::IsInstalled())
            return { Result::FailedOther, "summon needs the main-thread pump", true };
        if (!a_follower->HasSpell(a_spell))
            return { Result::FailedSkill, "follower does not know this spell", true };

        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();
        const auto key     = RecastKey(id, spellID);
        const auto now     = SelfClock::now();

        // The main thread's last verdict for this (follower, spell). A fresh
        // Live / Landing / Limit answers without posting again.
        {
            std::lock_guard lk(g_summonMx);
            if (auto it = g_summon.find(key); it != g_summon.end() &&
                SecSince(it->second.verdictAt, now) < kSummonVerdictFreshSec) {
                switch (it->second.verdict) {
                case SummonVerdict::Live:    return { Result::NoOp, "summon still live", true };
                case SummonVerdict::Landing: return { Result::NoOp, "summon appearing", true };
                case SummonVerdict::Limit:   return { Result::NoOp, "summon limit reached", true };
                case SummonVerdict::Cast:    return { Result::NoOp, "summon cast", true };
                default: break;
                }
            }
        }

        // COMPETENCE IS NOT PERMISSION (DESIGN §5.3): the same magicka + reserve
        // gate CastOn / CastSelfDirect apply (the same worker-side AV reads they
        // make). Transparent: fall to the next rule.
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have)
                return { Result::FailedSkill,
                         std::format("insufficient magicka (needs {:.0f})", cost), true };
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx)
                    return { Result::FailedSkill,
                             std::format("magicka reserve (floor {:.0f})", reserve * mx), true };
            }
        }

        if (SecSince(g_summonPosted[key], now) < kSummonPostEverySec)
            return { Result::NoOp, "summon check queued", true };
        g_summonPosted[key] = now;
        MainThread::Post([id, spellID, a_rule, table = std::string(a_table ? a_table : "?"),
                          target = std::string(a_target)] {
            SummonOnMain(id, spellID, a_rule, table, target);
        });
        // Optimistic and TRANSPARENT on purpose (see the threading note above):
        // the main thread decides and logs whether this cast.
        return { Result::NoOp, "summon dispatched to the main thread", true };
    }

}
