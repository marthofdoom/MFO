#include "PCH.h"
#include "Probe.h"
#include "Followers.h"

namespace MFO::Probe {

    namespace {

        std::mutex g_mx;
        Result     g_last;
        Retention  g_ret;

        RE::ActorHandle g_watchFollower;
        RE::ActorHandle g_watchTarget;
        std::chrono::steady_clock::time_point g_watchStart;
        std::chrono::steady_clock::time_point g_lastSample;

        // Vanilla Healing. Resolves on every install, so the cast probe needs
        // no records of our own.
        constexpr RE::FormID kHealingSpell = 0x00012FCD;

        const char* SafeName(RE::Actor* a) {
            if (!a) return "<none>";
            const char* n = a->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        // Nearest hostile-ish actor to the follower. Deliberately crude: this
        // is a probe, not the evaluator's targeting.
        RE::Actor* NearestFoe(RE::Actor* a_follower) {
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl || !a_follower) return nullptr;
            RE::Actor* best = nullptr;
            float bestDist = 1e9f;
            for (auto& h : pl->highActorHandles) {
                auto* a = h.get().get();
                if (!a || a == a_follower) continue;
                if (a->IsDead() || a->IsPlayerRef() || a->IsPlayerTeammate()) continue;
                if (!a->IsHostileToActor(a_follower)) continue;
                const float d = a->GetPosition().GetDistance(a_follower->GetPosition());
                if (d < bestDist) { bestDist = d; best = a; }
            }
            return best;
        }

        void Record(const char* a_action, RE::Actor* a_subject, bool a_ok, std::string a_detail) {
            std::scoped_lock lk(g_mx);
            g_last.action  = a_action;
            g_last.subject = SafeName(a_subject);
            g_last.detail  = std::move(a_detail);
            g_last.ok      = a_ok;
            g_last.valid   = true;
            spdlog::info("[probe] {} on {} -> {} ({})", a_action, SafeName(a_subject),
                         a_ok ? "OK" : "FAILED", g_last.detail);
        }

    }

    const char* Name(Action a) {
        switch (a) {
        case Action::StartCombatOnNearestFoe:    return "StartCombat(nearest foe)";
        case Action::StopCombat:                 return "StopCombat";
        case Action::CastHealOnSelf:             return "CastSpellImmediate(Healing, self)";
        case Action::DoCombatSpellApplyOnTarget: return "DoCombatSpellApply(Healing, target)";
        case Action::KeepOffsetClose:            return "KeepOffsetFromActor(close)";
        case Action::KeepOffsetFar:              return "KeepOffsetFromActor(far)";
        case Action::ClearKeepOffset:            return "ClearKeepOffsetFromActor";
        case Action::SetDontMoveOn:              return "SetDontMove(true)";
        case Action::SetDontMoveOff:             return "SetDontMove(false)";
        case Action::EvaluatePackage:            return "EvaluatePackage";
        case Action::DrawWeapon:                 return "DrawWeapon";
        case Action::SheatheWeapon:              return "Sheathe";
        default:                                 return "?";
        }
    }

    const char* Blurb(Action a) {
        switch (a) {
        case Action::StartCombatOnNearestFoe:
            return "Starts the retention watch. THE question: does the engine keep our target?";
        case Action::CastHealOnSelf:
            return "Does a native cast read as a real action, or is it silent?";
        case Action::DoCombatSpellApplyOnTarget:
            return "The combat-context alternative. Compare against the plain cast.";
        case Action::KeepOffsetClose:
            return "Formation offset. Watch whether it fights the combat controller.";
        case Action::ClearKeepOffset:
            return "Release. Kaidan's code pairs this with EvaluatePackage every time.";
        case Action::EvaluatePackage:
            return "Does it no-op when the same package would be chosen?";
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
            auto* foe = NearestFoe(f);
            if (!foe) { Record(Name(a_action), f, false, "no hostile actor nearby"); return; }
            // Kaidan's shipped idiom: always StopCombat before StartCombat,
            // and preserve aggression across a forced switch.
            auto* avo = f->AsActorValueOwner();
            const float aggro = avo ? avo->GetActorValue(RE::ActorValue::kAggression) : 0.0f;
            if (avo) avo->SetActorValue(RE::ActorValue::kAggression, 0.0f);
            f->StopCombat();
            f->StartCombat(foe);
            if (avo) avo->SetActorValue(RE::ActorValue::kAggression, aggro);

            {
                std::scoped_lock lk(g_mx);
                g_watchFollower = f->CreateRefHandle().get() ? f->GetHandle() : RE::ActorHandle{};
                g_watchTarget   = foe->GetHandle();
                g_watchStart = g_lastSample = std::chrono::steady_clock::now();
                g_ret = {};
                g_ret.follower  = SafeName(f);
                g_ret.commanded = SafeName(foe);
                g_ret.current   = SafeName(foe);
                g_ret.active    = true;
            }
            Record(Name(a_action), f, true, std::format("target {} — retention watch started", SafeName(foe)));
            return;
        }

        case Action::StopCombat:
            f->StopCombat();
            Stop();
            Record(Name(a_action), f, true, "watch stopped");
            return;

        case Action::CastHealOnSelf: {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kHealingSpell);
            if (!spell) { Record(Name(a_action), f, false, "Healing did not resolve"); return; }
            auto* caster = f->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) { Record(Name(a_action), f, false, "no magic caster"); return; }
            caster->CastSpellImmediate(spell, false, f, 1.0f, false, 0.0f, f);
            Record(Name(a_action), f, true, "issued — watch for the effect and animation");
            return;
        }

        case Action::DoCombatSpellApplyOnTarget: {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kHealingSpell);
            if (!spell) { Record(Name(a_action), f, false, "Healing did not resolve"); return; }
            auto* tgt = f->GetActorRuntimeData().currentCombatTarget.get().get();
            f->DoCombatSpellApply(spell, tgt ? tgt : f);
            Record(Name(a_action), f, true,
                   std::format("applied to {}", tgt ? SafeName(tgt) : "self (no combat target)"));
            return;
        }

        case Action::KeepOffsetClose:
        case Action::KeepOffsetFar: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) { Record(Name(a_action), f, false, "no player"); return; }
            const float y = (a_action == Action::KeepOffsetClose) ? -150.0f : -600.0f;
            f->KeepOffsetFromActor(player->GetHandle(), 0.0f, y, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 40.0f);
            Record(Name(a_action), f, true, std::format("offset y={:.0f} — does it hold in combat?", y));
            return;
        }

        case Action::ClearKeepOffset:
            f->ClearKeepOffsetFromActor();
            f->EvaluatePackage();          // always paired, per Kaidan's shipped code
            Record(Name(a_action), f, true, "cleared + EvaluatePackage");
            return;

        case Action::SetDontMoveOn:
            f->SetDontMove(true);
            Record(Name(a_action), f, true, "held in place");
            return;

        case Action::SetDontMoveOff:
            f->SetDontMove(false);
            Record(Name(a_action), f, true, "released");
            return;

        case Action::EvaluatePackage: {
            auto* before = f->GetCurrentPackage();
            f->EvaluatePackage();
            auto* after = f->GetCurrentPackage();
            Record(Name(a_action), f, true,
                   std::format("package {} -> {}",
                               before ? std::format("{:08X}", before->GetFormID()) : "none",
                               after  ? std::format("{:08X}", after->GetFormID())  : "none"));
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
        std::scoped_lock lk(g_mx);
        if (!g_ret.active) return;

        auto* f = g_watchFollower.get().get();
        if (!f) { g_ret.active = false; return; }

        const auto now = std::chrono::steady_clock::now();
        g_lastSample = now;
        g_ret.heldSeconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - g_watchStart).count() / 1000.0f;
        ++g_ret.samples;

        auto* cur = f->GetActorRuntimeData().currentCombatTarget.get().get();
        const std::string curName = SafeName(cur);
        if (curName != g_ret.current) {
            ++g_ret.changes;
            spdlog::info("[probe] retention: {} switched from {} to {} after {:.1f}s "
                         "(commanded {})",
                         g_ret.follower, g_ret.current, curName, g_ret.heldSeconds, g_ret.commanded);
            g_ret.current = curName;
        }

        // Stop once they leave combat entirely -- the question is about the
        // controller re-picking, not about the fight ending.
        if (!f->IsInCombat()) {
            spdlog::info("[probe] retention ended: {} left combat after {:.1f}s, {} change(s)",
                         g_ret.follower, g_ret.heldSeconds, g_ret.changes);
            g_ret.active = false;
        }
    }

    void Stop() {
        std::scoped_lock lk(g_mx);
        if (g_ret.active) {
            spdlog::info("[probe] retention stopped manually after {:.1f}s, {} change(s)",
                         g_ret.heldSeconds, g_ret.changes);
        }
        g_ret.active = false;
    }

    Result    GetLast()      { std::scoped_lock lk(g_mx); return g_last; }
    Retention GetRetention() { std::scoped_lock lk(g_mx); return g_ret;  }

}
