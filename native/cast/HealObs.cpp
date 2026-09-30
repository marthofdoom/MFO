// cast/HealObs.cpp -- feat/mfo-animheal-p0: the [heal-obs] passive heal observation
// (HealObsNote / HealObsSweep) and the read-back warn rate limit (ReadbackWarnDue).
// Cut from cast/Direct.cpp by the Direct.cpp split (2026-09-29, MFO-B152): a pure
// move, proven function by function with tools/splitcheck.
#include "Direct_internal.h"
#include "apmf/APMFBridge.h"     // feat/cast-gambit-concentration (Task 1): ClaimOffenseCast for a
#include <map>            // feat/mfo-animheal-p0 fix: ReadbackWarnDue gate (tuple key)
#include <tuple>

namespace MFO::Actuation {

    // ── [heal-obs] + the ConcProxy allow-list forms (feat/mfo-animheal-p0) ─────────
    namespace {
        struct HealObs {
            RE::FormID            caster = 0, target = 0, spell = 0, castForm = 0;
            const char*           road   = "?";       // a string literal (static storage)
            float                 hpBefore = 0.0f, dist = 0.0f;
            Sightline::Verdict    los    = Sightline::Verdict::Unknown;
            HealAttach            attach = HealAttach::Instant;
            bool                  conc   = false;
            bool                  claim  = false;     // MFO held a ch.8b cast claim at the apply
            SelfClock::time_point at{}, due{};
        };
        // Written on the MAIN thread (HealObsNote), drained on the WORKER (HealObsSweep):
        // a real cross-thread list, so a mutex.
        std::mutex                                        g_healObsMx;
        std::vector<HealObs>                              g_healObsPending;
        std::unordered_map<std::uint64_t, SelfClock::time_point> g_healObsLast;   // rate limit
        constexpr float kHealObsDelaySec = 1.0f;   // HP / effect re-read ~1 s after the apply
        constexpr float kHealObsEverySec = 3.0f;   // one observation per (caster, recipient) per 3 s
        // Read-back warn rate limit (ReadbackWarnDue): per (caster, spell), last print +
        // how many were held back since. Written on main; cleared by ResetHealObs, so
        // it shares g_healObsMx (a leaf lock, nothing is taken inside it).
        // The claim road's "HP before" (HealObsClaimLap): per caster, the recipient
        // and its health on the claim's most recent lap. Under g_healObsMx.
        struct ClaimLap { RE::FormID recipient = 0; float hp = 0.0f; };
        std::unordered_map<RE::FormID, ClaimLap> g_claimLap;
        struct WarnGate { SelfClock::time_point last{}; std::uint32_t held = 0; };
        std::map<std::tuple<RE::FormID, RE::FormID, ReadbackWarn>, WarnGate> g_readbackWarn;
        constexpr float kReadbackWarnEverySec = 5.0f;

    }
        void ResetHealObs() {
            std::lock_guard lk(g_healObsMx);
            g_healObsPending.clear();
            g_healObsLast.clear();
            g_readbackWarn.clear();
            g_claimLap.clear();
        }
    namespace {

        const char* AttachName(HealAttach a) {
            switch (a) {
            case HealAttach::Present: return "present";
            case HealAttach::Absent:  return "ABSENT";
            default:                  return "instant";
            }
        }
    }

    bool ReadbackWarnDue(RE::FormID a_caster, RE::FormID a_spell, ReadbackWarn a_kind,
                         std::uint32_t& a_suppressed) {
        const auto now = SelfClock::now();
        std::lock_guard lk(g_healObsMx);
        auto& g = g_readbackWarn[{ a_caster, a_spell, a_kind }];
        if (g.last.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(now - g.last).count() < kReadbackWarnEverySec) {
            ++g.held;
            return false;
        }
        a_suppressed = g.held;
        g.held = 0;
        g.last = now;
        return true;
    }

    void HealObsNote(RE::Actor* a_caster, RE::Actor* a_target, RE::SpellItem* a_spell,
                     RE::SpellItem* a_castForm, const char* a_road, float a_hpBefore,
                     HealAttach a_attach) {
        if (!a_caster || !a_target || !a_spell) return;
        const auto now = SelfClock::now();
        const auto key = (static_cast<std::uint64_t>(a_caster->GetFormID()) << 32) | a_target->GetFormID();
        HealObs o;
        o.caster   = a_caster->GetFormID();
        o.target   = a_target->GetFormID();
        o.spell    = a_spell->GetFormID();
        o.castForm = a_castForm ? a_castForm->GetFormID() : o.spell;
        o.road     = a_road ? a_road : "?";
        o.hpBefore = a_hpBefore;
        o.dist     = a_caster == a_target ? 0.0f
                                          : a_caster->GetPosition().GetDistance(a_target->GetPosition());
        o.los      = a_caster == a_target ? Sightline::Verdict::Visible
                                          : Sightline::Check(o.caster, o.target);
        o.attach   = a_attach;
        o.conc     = a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
        o.claim    = APMFBridge::IsHealCastActive(o.caster) || APMFBridge::IsOwnedCastActive(o.caster);
        o.at       = now;
        o.due      = now + std::chrono::duration_cast<SelfClock::duration>(
                               std::chrono::duration<float>(kHealObsDelaySec));
        std::lock_guard lk(g_healObsMx);
        auto& last = g_healObsLast[key];
        if (last.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(now - last).count() < kHealObsEverySec)
            return;
        last = now;
        g_healObsPending.push_back(o);
    }

    void HealObsClaimLap(RE::FormID a_caster, RE::FormID a_recipient, float a_hp) {
        if (a_caster == 0) return;
        std::lock_guard lk(g_healObsMx);
        g_claimLap[a_caster] = ClaimLap{ a_recipient == 0 ? a_caster : a_recipient, a_hp };
    }

    void HealObsNoteClaimFire(RE::FormID a_caster, RE::FormID a_firedForm) {
        const RE::FormID spell = APMFBridge::GetHealCastSpell(a_caster);
        if (spell == 0 || a_firedForm == 0) return;
        const RE::FormID proxy = APMFBridge::GetHealCastProxy(a_caster);
        if (a_firedForm != spell && (proxy == 0 || a_firedForm != proxy)) return;   // not the heal claim
        const RE::FormID tgt       = APMFBridge::GetHealCastTarget(a_caster);
        const RE::FormID recipient = tgt == 0 ? a_caster : tgt;
        float hpLap = -1.0f;   // -1 = no lap stamp for this recipient: read at the fire instead
        {
            std::lock_guard lk(g_healObsMx);
            if (const auto it = g_claimLap.find(a_caster);
                it != g_claimLap.end() && it->second.recipient == recipient)
                hpLap = it->second.hp;
        }
        MainThread::Post([a_caster, recipient, spell, a_firedForm, hpLap] {
            auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_caster);
            auto* target = RE::TESForm::LookupByID<RE::Actor>(recipient);
            auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(spell);
            auto* form   = RE::TESForm::LookupByID<RE::SpellItem>(a_firedForm);
            if (!caster || !target || !sp) return;
            HealObsNote(caster, target, sp, form ? form : sp, "claim",
                        hpLap >= 0.0f ? hpLap : Vocab::HealthPct(target), HealAttach::Instant);
        });
    }

    void HealObsSweep() {
        std::vector<HealObs> due;
        {
            std::lock_guard lk(g_healObsMx);
            if (g_healObsPending.empty()) return;
            const auto now = SelfClock::now();
            for (auto it = g_healObsPending.begin(); it != g_healObsPending.end();) {
                if (it->due <= now) { due.push_back(*it); it = g_healObsPending.erase(it); }
                else ++it;
            }
        }
        for (const auto& o : due) {
            // WORKER-side registry read (worker-serial, #4): is MFO's own stream for this
            // (caster, spell, recipient) still registered? A stopped channel on a live
            // stream is the masked-failure shape; one on a released stream is expected.
            // THE CLAIM ROAD (animheal phase 2) has no MFO stream: the follower's own
            // AI channels it on a HAND caster, so the direct road's registries say
            // nothing about it and its channel is read from the LEFT hand below.
            const bool claimRoad = std::string_view(o.road) == "claim";
            const char* stream = claimRoad ? "n/a" : "none";
            if (o.conc && !claimRoad) {
                if (o.caster == o.target) {
                    const auto it = g_selfCast.find(o.caster);
                    stream = (it != g_selfCast.end() && it->second.spell == o.spell) ? "live" : "released";
                } else {
                    stream = TargetStreamLive(o.caster, o.spell, o.target) ? "live" : "released";
                }
            }
            MainThread::Post([o, stream, claimRoad] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(o.caster);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(o.target);
                auto* form   = RE::TESForm::LookupByID<RE::MagicItem>(o.castForm);
                if (!caster || !target) return;
                const float hpAfter = Vocab::HealthPct(target);
                const bool  present = form && SpellEffectPresentOn(target, form);
                // A concentration heal channels on the caster's INSTANT caster. Harbinger's
                // CheckCast deny there interrupts it on its next cast tick (AE 34407 / SE
                // 33629, apmf-animheal-p1), so this is the visible face of its verdict.
                const char* channel = "n/a";
                int         st      = -1;
                if (o.conc) {
                    // Heals are LEFT always on the claim road (ClaimHealCast's hard rule).
                    auto* inst = caster->GetMagicCaster(claimRoad ? RE::MagicSystem::CastingSource::kLeftHand
                                                                  : RE::MagicSystem::CastingSource::kInstant);
                    st = inst ? static_cast<int>(inst->state.get()) : -1;
                    channel = (inst && inst->currentSpell && inst->currentSpell->GetFormID() == o.castForm &&
                               inst->state.get() == RE::MagicCaster::State::kCasting)
                                  ? "running" : "stopped";
                }
                const auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(o.spell);
                const bool  lasting = o.conc || HasDurationEffect(spell);
                // THE VERDICT (field 2026-09-29: 13 of 13 "HEAL NOT LANDING" lines were
                // false). LANDED = the effect was on the recipient at the apply, OR it
                // is on them now, OR their HP rose. A short effect (Close Greater
                // Wounds: present at apply, gone 1 s later, hp 16% -> 100%) that
                // expired before this read-back HAS landed. Loud only when none of the
                // three holds for a lasting effect on a live apply (a released
                // stream's effect is expected to be gone).
                // A LIVE STREAM is judged on NOW only: effect present now, or HP rose.
                // "Present at apply" says nothing about a stream MFO still holds live
                // whose effect is gone and whose HP did not move (it stopped healing),
                // so that shape stays loud. Every other road keeps "present at apply".
                const bool liveStream = o.conc && std::string_view(stream) == "live";
                const bool hpRose     = hpAfter > o.hpBefore + 0.005f;
                const bool atApply    = !liveStream && o.attach == HealAttach::Present;
                const bool landed     = atApply || present || hpRose;
                // On the claim road the fire itself is the evidence the cast happened, so
                // a lasting heal with no effect and no HP rise 1 s later is NOT landing.
                const bool gone   = lasting && !landed &&
                                    (!o.conc || claimRoad || std::string_view(stream) == "live");
                // THE CHANNEL ONLY COUNTS WHERE A CHANNEL IS EXPECTED. Both direct roads
                // ("direct self", "direct stream") keep the effect alive themselves
                // (SustainConcentrationEffect re-arms it each beat), so the instant
                // caster idling at state 0 between beats is normal there, not a cut
                // (field: every "channel=stopped (state 0)" line on the direct road had
                // the effect present and most had HP rising). The state is still
                // printed. No road that reaches this today relies on a running channel,
                // so `cut` fires only for a road that does not sustain the effect
                // itself, and only when nothing else says the heal landed.
                const bool sustainsItself = std::string_view(o.road).starts_with("direct");
                const bool cut  = o.conc && !sustainsItself && !landed &&
                                  std::string_view(stream) == "live" &&
                                  std::string_view(channel) == "stopped";
                const auto line = std::format(
                    "[heal-obs] {:08X} -> {:08X} {} ({:08X}{}) road={} dist={:.0f} LoS={} hp {:.0f}% -> "
                    "{:.0f}% after {:.1f}s | effect at apply={} now={} | channel={} (state {}) stream={} | "
                    "MFO ch.8b claim at apply={} | landed={}",
                    o.caster, o.target, spell && spell->GetName() ? spell->GetName() : "?", o.spell,
                    o.castForm != o.spell ? std::format(" via {:08X}", o.castForm) : std::string{},
                    o.road, o.dist, Sightline::VerdictName(o.los), o.hpBefore * 100.0f, hpAfter * 100.0f,
                    std::chrono::duration<float>(SelfClock::now() - o.at).count(),
                    AttachName(o.attach), lasting ? (present ? "present" : "ABSENT") : "instant",
                    channel, st, stream, o.claim ? "yes" : "no",
                    !landed ? "NO"
                    : atApply || present ? (hpRose ? "effect+hp" : "effect")
                                                                 : "hp");
                if (gone || cut)
                    spdlog::warn("{} *** HEAL NOT LANDING: {} ***", line,
                                 gone ? (claimRoad ? "the claimed cast FIRED, but 1 s later the recipient "
                                                     "shows neither its effect nor a health rise"
                                                   : "logged as applied, but the recipient carries no effect of it")
                                      : "the stream is live but its channel is not running (a CheckCast "
                                        "deny on the instant caster interrupts it)");
                else
                    spdlog::info("{}", line);
            });
        }
    }

}
