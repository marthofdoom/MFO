#include "PCH.h"
#include "Probe.h"
#include "Targeting.h"

namespace MFO::Probe {

    namespace {

        std::mutex g_mx;
        Result     g_last;
        Retention  g_ret;

        RE::ActorHandle g_watchFollower;
        RE::ActorHandle g_watchTarget;    // what we COMMANDED
        RE::ActorHandle g_prevTarget;     // what they had last sample
        std::chrono::steady_clock::time_point g_watchStart;
        std::optional<std::chrono::steady_clock::time_point> g_firstDefection;

        // Vanilla Healing.
        // 0x12FCC, NOT 0x12FCD -- that is FLAMES. The first draft of this file
        // had a probe labelled "cast Healing on self" that would have set the
        // follower on fire and blamed them for it.
        constexpr RE::FormID kHealingSpell = 0x00012FCC;

        constexpr float kMaxFoeDistance = 4000.0f;

        // po3's PUBLISHED relocation for Actor::StartCombat, which
        // CommonLibSSE-NG does not bind. Taking a maintained reference's
        // published Address Library ID is the sanctioned pattern -- the ID is a
        // fact about the binary; the call is ours.
        // SE/AE only: no sourced VR id, so VR is REFUSED rather than guessed.
        bool StartCombatOn(RE::Actor* a_actor, RE::Actor* a_target) {
            if (REL::Module::IsVR()) return false;
            using func_t = bool(RE::Actor*, RE::Actor*, void*);
            static REL::Relocation<func_t> func{ REL::RelocationID(37608, 38561) };
            return func(a_actor, a_target, nullptr);
        }

        constexpr Unavailable kUnavailable[] = {
            { "KeepOffsetFromActor / Clear", "Papyrus-only; no C++ binding in NG" },
            { "SetDontMove",                 "Papyrus-only; no C++ binding in NG" },
            { "DoCombatSpellApply",          "Papyrus-only; no C++ binding in NG" },
        };

        std::string SafeName(RE::Actor* a) {
            if (!a) return "<none>";
            const char* n = a->GetName();
            return (n && *n) ? std::string(n) : std::string("<unnamed>");
        }

        // Pick a foe to command the follower onto -- and, when there is a
        // choice, deliberately pick one they are NOT already fighting. That is
        // the only way the retention watch tests whether OUR command overrides
        // the engine's own pick rather than merely agreeing with it (the v0.4.1
        // test was confounded: single target, so "no defection" proved nothing).
        RE::Actor* PickFoe(RE::Actor* a_follower, float& a_outDist, int& a_outCount, bool& a_outWasCurrent) {
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl || !a_follower) return nullptr;

            auto* current = a_follower->GetActorRuntimeData().currentCombatTarget.get().get();

            RE::Actor* nearest = nullptr;      float nearestDist = kMaxFoeDistance;
            RE::Actor* nonCurrent = nullptr;   float nonCurrentDist = kMaxFoeDistance;
            int count = 0;
            for (auto& h : pl->highActorHandles) {
                auto* a = h.get().get();
                if (!a || a == a_follower) continue;
                if (a->IsDead() || a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                if (!a->Is3DLoaded()) continue;
                if (!a->IsHostileToActor(a_follower)) continue;
                const float d = a->GetPosition().GetDistance(a_follower->GetPosition());
                if (d >= kMaxFoeDistance) continue;
                ++count;
                if (d < nearestDist) { nearestDist = d; nearest = a; }
                if (a != current && d < nonCurrentDist) { nonCurrentDist = d; nonCurrent = a; }
            }
            a_outCount = count;
            // Prefer a foe they are NOT already fighting; fall back to nearest.
            if (nonCurrent) { a_outDist = nonCurrentDist; a_outWasCurrent = false; return nonCurrent; }
            a_outDist = nearestDist; a_outWasCurrent = (nearest == current); return nearest;
        }

        // Build the message under the lock, LOG AFTER RELEASING IT. A spdlog
        // flush while holding g_mx stalls the render thread, which is holding
        // g_imguiIoMx, which stalls the input hook.
        void Record(const char* a_action, RE::Actor* a_subject, bool a_ok, std::string a_detail) {
            std::string line;
            {
                std::scoped_lock lk(g_mx);
                g_last.action  = a_action;
                g_last.subject = SafeName(a_subject);
                g_last.detail  = std::move(a_detail);
                g_last.ok      = a_ok;
                g_last.valid   = true;
                line = std::format("[probe] {} on {} -> {} ({})", a_action, g_last.subject,
                                   a_ok ? "OK" : "FAILED", g_last.detail);
            }
            spdlog::info("{}", line);
        }

    }

    std::span<const Unavailable> UnavailableActions() { return kUnavailable; }

    const char* Name(Action a) {
        switch (a) {
        case Action::StartCombatOnNearestFoe: return "StartCombat (nearest foe)";
        case Action::StopCombat:              return "StopCombat";
        case Action::CastHealInstant:         return "Cast Healing -- kInstant";
        case Action::CastHealRightHand:       return "Cast Healing -- kRightHand";
        case Action::CastHealLeftHand:        return "Cast Healing -- kLeftHand";
        case Action::CastHealOther:           return "Cast Healing -- kOther";
        case Action::EvaluatePackage:         return "EvaluatePackage";
        case Action::DrawWeapon:              return "Draw weapon";
        case Action::SheatheWeapon:           return "Sheathe";
        default:                              return "?";
        }
    }

    const char* Blurb(Action a) {
        switch (a) {
        case Action::StartCombatOnNearestFoe:
            return "Starts the retention watch. THE question for DESIGN 4.7.";
        case Action::CastHealInstant:
            return "Known: applies the effect, NO animation. The baseline to compare against.";
        case Action::CastHealRightHand:
            return "Does a HAND source animate? kInstant is literally the no-animation caster.";
        case Action::CastHealLeftHand:
            return "Same question, other hand.";
        case Action::CastHealOther:
            return "The remaining source. Try it if neither hand animates.";
        case Action::CommandTargetAtCrosshair:
            return "LOOK AT AN ENEMY, then fire. Latches this follower onto it (ENGINE_NOTES 0.14).";
        case Action::ClearCommandedTarget:
            return "Release the latch -- the follower returns to the engine's own choice.";
        case Action::EvaluatePackage:
            return "Does it no-op when the same package would be chosen again?";
        default:
            return "";
        }
    }

    void Fire(RE::FormID a_followerID, Action a_action) {
        auto* form = RE::TESForm::LookupByID(a_followerID);
        auto* f = form ? form->As<RE::Actor>() : nullptr;
        if (!f) { Record(Name(a_action), nullptr, false, "follower did not resolve"); return; }

        switch (a_action) {

        case Action::StartCombatOnNearestFoe: {
            float dist = 0.0f;
            int   candidates = 0;
            bool  wasCurrent = false;
            auto* foe = PickFoe(f, dist, candidates, wasCurrent);
            if (!foe) {
                Record(Name(a_action), f, false,
                       std::format("no loaded hostile within {:.0f}u", kMaxFoeDistance));
                return;
            }
            // StopCombat before StartCombat is Kaidan's shipped idiom, kept.
            // The aggression zero/restore that accompanies it there is NOT:
            // in Papyrus it holds aggression down across a latent window, and
            // doing it synchronously in one frame restores it before it could
            // have protected anything -- a no-op with an extra failure path.
            f->StopCombat();
            if (!StartCombatOn(f, foe)) {
                Record(Name(a_action), f, false, "StartCombat unavailable on this runtime (VR)");
                return;
            }
            {
                std::scoped_lock lk(g_mx);
                g_watchFollower  = f->GetHandle();
                g_watchTarget    = foe->GetHandle();
                g_prevTarget     = foe->GetHandle();
                g_watchStart     = std::chrono::steady_clock::now();
                g_firstDefection.reset();
                g_ret = {};
                g_ret.follower    = SafeName(f);
                g_ret.commanded   = SafeName(foe);
                g_ret.current     = SafeName(foe);
                g_ret.onCommanded = true;
                g_ret.active      = true;
                g_ret.valid       = true;
            }
            const char* quality = (candidates <= 1)
                ? " -- INCONCLUSIVE by construction: only 1 candidate, vanilla AI sticks to it anyway"
                : (wasCurrent ? " -- weak: had to reuse their current target"
                              : " -- valid: commanded onto a foe they were NOT fighting");
            {
                std::scoped_lock lk(g_mx);
                g_ret.commanded = std::format("{} ({} candidate(s))", SafeName(foe), candidates);
            }
            Record(Name(a_action), f, true,
                   std::format("target {} at {:.0f}u, {} candidate(s){}",
                               SafeName(foe), dist, candidates, quality));
            return;
        }

        case Action::StopCombat:
            f->StopCombat();
            Stop();
            Record(Name(a_action), f, true, "combat stopped, watch ended");
            return;

        case Action::CastHealInstant:
        case Action::CastHealRightHand:
        case Action::CastHealLeftHand:
        case Action::CastHealOther: {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kHealingSpell);
            if (!spell) { Record(Name(a_action), f, false, "Healing did not resolve"); return; }

            using CS = RE::MagicSystem::CastingSource;
            CS src = CS::kInstant;
            if (a_action == Action::CastHealRightHand) src = CS::kRightHand;
            else if (a_action == Action::CastHealLeftHand) src = CS::kLeftHand;
            else if (a_action == Action::CastHealOther)    src = CS::kOther;

            auto* caster = f->GetMagicCaster(src);
            if (!caster) { Record(Name(a_action), f, false, "no magic caster for that source"); return; }
            caster->CastSpellImmediate(spell, false, f, 1.0f, false, 0.0f, f);
            Record(Name(a_action), f, true,
                   "issued -- DID IT ANIMATE? kInstant is known not to; that is the comparison");
            return;
        }

        case Action::CommandTargetAtCrosshair: {
            const auto id = CrosshairTarget();
            if (id == 0) {
                Record(Name(a_action), f, false, "nothing under the crosshair -- look AT an enemy");
                return;
            }
            auto* victim = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!victim) { Record(Name(a_action), f, false, "crosshair ref is not an actor"); return; }
            if (victim == f) { Record(Name(a_action), f, false, "that is the follower"); return; }

            Targeting::Command(f->GetFormID(), victim->GetHandle());
            const bool hooked = Targeting::IsHooked();
            Record(Name(a_action), f, hooked,
                   std::format("latched onto {} ({:08X}){}", victim->GetName(), id,
                               hooked ? " -- watch whether they switch"
                                      : " -- BUT HOOK IS NOT INSTALLED (bCommandTarget=0)"));
            return;
        }

        case Action::ClearCommandedTarget: {
            Targeting::Clear(f->GetFormID());
            Record(Name(a_action), f, true, "latch released");
            return;
        }

        case Action::EvaluatePackage: {
            auto* before = f->GetCurrentPackage();
            f->EvaluatePackage();
            auto* after = f->GetCurrentPackage();
            Record(Name(a_action), f, true,
                   std::format("package {} -> {}{}",
                               before ? std::format("{:08X}", before->GetFormID()) : "none",
                               after  ? std::format("{:08X}", after->GetFormID())  : "none",
                               (before == after) ? "  (UNCHANGED -- the no-op case §4.5a warns about)" : ""));
            return;
        }

        case Action::DrawWeapon:
            f->DrawWeaponMagicHands(true);
            Record(Name(a_action), f, true, "drawn");
            return;

        case Action::SheatheWeapon:
            f->DrawWeaponMagicHands(false);
            Record(Name(a_action), f, true, "sheathed");
            return;

        default:
            return;
        }
    }

    namespace {
        std::atomic<RE::FormID> g_crosshair{ 0 };

        class CrosshairSink final : public RE::BSTEventSink<SKSE::CrosshairRefEvent> {
        public:
            static CrosshairSink* GetSingleton() { static CrosshairSink s; return &s; }
            RE::BSEventNotifyControl ProcessEvent(const SKSE::CrosshairRefEvent* a_event,
                                                  RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override {
                // Store the ID only -- never a pointer across frames (#2).
                if (a_event && a_event->crosshairRef) g_crosshair = a_event->crosshairRef->GetFormID();
                else                                  g_crosshair = 0;
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void RegisterCrosshairSink() {
        SKSE::GetCrosshairRefEventSource()->AddEventSink(CrosshairSink::GetSingleton());
        spdlog::info("[probe] crosshair sink registered (commanded-target picker)");
    }

    RE::FormID CrosshairTarget() { return g_crosshair.load(); }

    void Tick() {
        std::string logLine;
        {
            std::scoped_lock lk(g_mx);
            if (!g_ret.active) return;

            auto* f = g_watchFollower.get().get();
            if (!f) { g_ret.active = false; return; }

            const auto now = std::chrono::steady_clock::now();
            g_ret.watchSeconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - g_watchStart).count() / 1000.0f;
            ++g_ret.samples;

            if (f->IsInCombat()) g_ret.everEngaged = true;

            // The commanded target dying is INVALIDATION, not a re-pick. Scoring
            // it as a change would falsely conclude "the engine re-picks" and
            // send §4.7 back to the drawing board for no reason.
            auto* commanded = g_watchTarget.get().get();
            if (!commanded || commanded->IsDead()) {
                g_ret.commandedDied = true;
                g_ret.active = false;
                logLine = std::format("[probe] retention ended: commanded target died after {:.1f}s "
                                      "({} change(s) before that)", g_ret.watchSeconds, g_ret.changes);
            } else {
                auto  curHandle = f->GetActorRuntimeData().currentCombatTarget;
                auto* cur       = curHandle.get().get();

                // Compare HANDLES, not names -- two "Bandit"s would hide a
                // re-pick, and a target->none->target blip would read as two.
                if (curHandle != g_prevTarget) {
                    ++g_ret.changes;
                    g_prevTarget = curHandle;
                    g_ret.current = SafeName(cur);
                    if (curHandle != g_watchTarget && !g_firstDefection) {
                        g_firstDefection = now;
                        g_ret.heldSeconds = g_ret.watchSeconds;
                    }
                    logLine = std::format("[probe] retention: {} now on {} after {:.1f}s "
                                          "(commanded {}, change #{})",
                                          g_ret.follower, g_ret.current, g_ret.watchSeconds,
                                          g_ret.commanded, g_ret.changes);
                }
                g_ret.onCommanded = (curHandle == g_watchTarget);

                if (!g_ret.everEngaged && g_ret.samples > 4) {
                    // Log the zero case: "StartCombat did nothing" and "combat
                    // ended" must not look the same (INVARIANTS #46).
                    g_ret.active = false;
                    logLine = std::format("[probe] retention ended: {} NEVER ENTERED COMBAT -- "
                                          "StartCombat did not take", g_ret.follower);
                } else if (g_ret.everEngaged && !f->IsInCombat()) {
                    g_ret.active = false;
                    logLine = std::format("[probe] retention ended: {} left combat after {:.1f}s, "
                                          "{} change(s), held commanded target {:.1f}s",
                                          g_ret.follower, g_ret.watchSeconds, g_ret.changes,
                                          g_firstDefection ? g_ret.heldSeconds : g_ret.watchSeconds);
                }
            }
        }
        if (!logLine.empty()) spdlog::info("{}", logLine);
    }

    void Stop() {
        std::string logLine;
        {
            std::scoped_lock lk(g_mx);
            if (g_ret.active) {
                logLine = std::format("[probe] retention stopped after {:.1f}s, {} change(s)",
                                      g_ret.watchSeconds, g_ret.changes);
            }
            g_ret.active = false;
        }
        if (!logLine.empty()) spdlog::info("{}", logLine);
    }

    void ReleaseAll() {
        // Nothing the probe does currently outlives a session -- the primitives
        // that WOULD have (SetDontMove, KeepOffset) are the ones the library
        // does not bind. This exists so that when they arrive, the release path
        // is already wired and the header's promise stays true.
        Stop();
    }

    Result    GetLast()      { std::scoped_lock lk(g_mx); return g_last; }
    Retention GetRetention() { std::scoped_lock lk(g_mx); return g_ret;  }

}
