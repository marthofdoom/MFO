// cast/SummonProbe.cpp -- the SUMMON LANDING-POSITION PROBE (feat/mfo-spell-archetype-probe, task 7
// slice P0, item 4). PASSIVE: reads, logs, changes nothing.
//
// QUESTION IT ANSWERS (principle 5, observe before building): where does a summon cast on the direct
// road actually appear relative to its caster? The engine places an actor's summon itself
// (SummonCreatureEffect::Start picks a spot in front of the caster, APMF Docs/ADDRESS-TABLE-2026-09-15.md
// :510-513), so MFO supplies no point; a ground-locked point (APMF FindEmptySpace) is only needed if
// the field shows bad placement (in the air, inside geometry, falling). This logs the evidence.
//
// THREADING: every engine read here runs on the TRUE MAIN thread. SummonOnMain (cast/Summon.cpp) is a
// MainThread::Post closure; this probe is started from inside it and re-posts ITSELF with
// MainThread::Post, one step per frame, so no read is ever made off main. A posted closure holds only
// FormIDs (never an Actor*), is dropped by MainThread::Clear on revert, and is bounded by a deadline.
//
// TWO SAMPLES per summon: "appeared" the first frame the effect's commandedActor handle resolves, and
// "settled" kSettleSec later, because a creature that spawns in the air or inside geometry moves after
// the engine places it.
//
// FORK FIELDS READ (CommonLibSSE-NG mit-3.7 fork fde0f3a include/RE/...): SummonCreatureEffect::
// commandedActor (S/SummonCreatureEffect.h:33, +0xA8); ActiveEffect::spell (the summon code in
// cast/Summon.cpp already reads it); TESObjectREFR::GetPosition / GetPositionZ / GetAngleZ /
// GetParentCell (T/TESObjectREFR.h:379, :411-415); Actor::GetCharController (A/Actor.h:528);
// bhkCharacterController::context (B/bhkCharacterController.h:114) -> hkpCharacterContext::
// currentState (H/hkpCharacterContext.h:57), enumerators hkpCharacterStateTypes (H/hkpCharacterState.h:
// 13-28). `Actor::IsInMidair` is deliberately NOT used: it is a relocated call and the probe makes no
// engine call. A navmesh query is not cheaply readable (it is an engine call), so none is made; the
// controller's ground state stands in for "on the ground".
#include "Actuation.h"
#include "MainThread.h"

#include <chrono>
#include <cmath>

namespace MFO::Actuation {

    namespace {

        using Clock = std::chrono::steady_clock;
        constexpr float kSettleSec = 2.0f;   // second sample after the handle resolves
        constexpr float kGiveUpPad = 3.0f;   // past the engine appear window, then say it never resolved

        struct Probe {
            RE::FormID        caster  = 0;
            RE::FormID        spell   = 0;
            RE::FormID        summon  = 0;     // the commanded actor, once resolved
            Clock::time_point start{};
            Clock::time_point resolved{};
            float             appearSec = 4.0f;
        };

        float Since(Clock::time_point a_t, Clock::time_point a_now) {
            return std::chrono::duration<float>(a_now - a_t).count();
        }

        const char* GroundState(RE::Actor* a_actor) {
            auto* cc = a_actor ? a_actor->GetCharController() : nullptr;
            if (!cc) return "no-controller";
            switch (cc->context.currentState) {
            case RE::hkpCharacterStateTypes::kOnGround: return "on-ground";
            case RE::hkpCharacterStateTypes::kJumping:  return "jumping";
            case RE::hkpCharacterStateTypes::kInAir:    return "IN-AIR";
            case RE::hkpCharacterStateTypes::kClimbing: return "climbing";
            case RE::hkpCharacterStateTypes::kFlying:   return "flying";
            case RE::hkpCharacterStateTypes::kSwimming: return "swimming";
            default:                                    return "other";
            }
        }

        // MAIN THREAD.
        void Sample(const char* a_label, const Probe& p, RE::Actor* f, RE::Actor* c, float a_t) {
            const auto fp = f->GetPosition(), cp = c->GetPosition();
            const float dx = cp.x - fp.x, dy = cp.y - fp.y;
            const float horiz = std::sqrt(dx * dx + dy * dy);
            const float dz    = cp.z - fp.z;   // feet-origin of both references
            // Skyrim heading: angle Z measured clockwise from +Y, forward = (sin a, cos a).
            constexpr float kPi = 3.14159265358979f;
            float bearing = std::atan2(dx, dy) - f->GetAngleZ();
            while (bearing >  kPi) bearing -= 2.0f * kPi;
            while (bearing < -kPi) bearing += 2.0f * kPi;
            spdlog::info("[summon-probe] {:08X} {} cast {:08X}: {} t+{:.2f}s creature {:08X} {} -- "
                         "offset {:.0f} horizontal, {:+.0f} height, bearing {:+.0f} deg (0 = in front of the "
                         "caster), 3D {:.0f}; creature {}, same cell {}, dead {}",
                         p.caster, f->GetName() ? f->GetName() : "?", p.spell, a_label, a_t, p.summon,
                         c->GetName() ? c->GetName() : "?", horiz, dz, bearing * 180.0f / kPi,
                         std::sqrt(horiz * horiz + dz * dz), GroundState(c),
                         f->GetParentCell() == c->GetParentCell(), c->IsDead());
        }

        // MAIN THREAD. One step; re-posts itself until done.
        void Step(Probe p) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(p.caster);
            if (!f) return;   // caster gone (unloaded / dismissed): nothing to measure
            const auto now = Clock::now();
            const float t  = Since(p.start, now);
            if (p.summon == 0) {
                RE::NiPointer<RE::Actor> found;   // holds the reference across the list walk
                if (auto* mt = f->AsMagicTarget()) {
                    if (auto* list = mt->GetActiveEffectList()) {
                        for (auto* ae : *list) {
                            if (!ae || !ae->spell || ae->spell->GetFormID() != p.spell) continue;
                            auto* se = skyrim_cast<RE::SummonCreatureEffect*>(ae);
                            if (!se) continue;
                            if (auto c = se->commandedActor.get()) { found = std::move(c); break; }
                        }
                    }
                }
                if (found) {
                    p.summon   = found->GetFormID();
                    p.resolved = now;
                    Sample("appeared", p, f, found.get(), t);
                } else if (t > p.appearSec + kGiveUpPad) {
                    spdlog::info("[summon-probe] {:08X} {} cast {:08X}: no commanded actor resolved within "
                                 "{:.1f}s (engine appear window {:.1f}s) -- the summon never appeared, or its "
                                 "effect ended first",
                                 p.caster, f->GetName() ? f->GetName() : "?", p.spell, t, p.appearSec);
                    return;
                }
            } else if (Since(p.resolved, now) >= kSettleSec) {
                if (auto* c = RE::TESForm::LookupByID<RE::Actor>(p.summon))
                    Sample("settled", p, f, c, t);
                else
                    spdlog::info("[summon-probe] {:08X} cast {:08X}: creature {:08X} gone before the settle sample",
                                 p.caster, p.spell, p.summon);
                return;
            }
            MainThread::Post([p] { Step(p); });
        }
    }

    void ProbeSummonLanding(RE::FormID a_caster, RE::FormID a_spell, float a_appearSec) {
        Probe p;
        p.caster    = a_caster;
        p.spell     = a_spell;
        p.start     = Clock::now();
        p.appearSec = a_appearSec;
        MainThread::Post([p] { Step(p); });
    }

}
