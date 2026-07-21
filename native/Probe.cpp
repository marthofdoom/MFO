#include "PCH.h"
#include "Probe.h"

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

        RE::Actor* NearestFoe(RE::Actor* a_follower, float& a_outDist) {
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl || !a_follower) return nullptr;
            RE::Actor* best = nullptr;
            float bestDist = kMaxFoeDistance;   // capped: do not send them across the cell
            for (auto& h : pl->highActorHandles) {
                auto* a = h.get().get();
                if (!a || a == a_follower) continue;
                if (a->IsDead() || a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                if (!a->Is3DLoaded()) continue;
                if (!a->IsHostileToActor(a_follower)) continue;
                const float d = a->GetPosition().GetDistance(a_follower->GetPosition());
                if (d < bestDist) { bestDist = d; best = a; }
            }
            a_outDist = bestDist;
            return best;
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
        case Action::CastHealOnSelf:          return "CastSpellImmediate (Healing, self)";
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
        case Action::CastHealOnSelf:
            return "Does a native cast read as a real action, with animation?";
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
            auto* foe = NearestFoe(f, dist);
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
            Record(Name(a_action), f, true,
                   std::format("target {} at {:.0f}u -- retention watch started", SafeName(foe), dist));
            return;
        }

        case Action::StopCombat:
            f->StopCombat();
            Stop();
            Record(Name(a_action), f, true, "combat stopped, watch ended");
            return;

        case Action::CastHealOnSelf: {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kHealingSpell);
            if (!spell) { Record(Name(a_action), f, false, "Healing did not resolve"); return; }
            auto* caster = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) { Record(Name(a_action), f, false, "no magic caster"); return; }
            caster->CastSpellImmediate(spell, false, f, 1.0f, false, 0.0f, f);
            Record(Name(a_action), f, true, "issued -- watch for effect AND animation");
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
