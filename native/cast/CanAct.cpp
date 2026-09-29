// cast/CanAct.cpp -- fix/mfo-can-act: the refusal log (NoteRefusedApply), the
// [bleed] life-state line (NoteLifeState / NoteHealLanded) and the heal reach
// (HealReach / HealInReach / RefuseHealApplyOnMain). The gate itself,
// CannotActReason, is inline in cast/Actuation.h.
// Cut from cast/Direct.cpp by the Direct.cpp split (2026-09-29, MFO-B152): a pure
// move, proven function by function with tools/splitcheck.
#include "Direct_internal.h"
#include <unordered_set>    // fix/mfo-can-act: NoteRefusedApply's once-per-down latch

namespace MFO::Actuation {

    // ── fix/mfo-can-act: the refusal log, the [bleed] line and the heal reach.
    // See Actuation.h for each contract. ─────────────────────────────────────────
    namespace {
        // NoteRefusedApply's once-per-down-episode latch. Written on the main thread
        // (and the worker, for a potion), erased on the worker (NoteLifeState) -> mutex.
        std::mutex                     g_refusedMx;
        std::unordered_set<RE::FormID> g_refusedLogged;

        // NoteHealLanded's ledger: the last MFO heal applied ON each actor, and how
        // many landed since he last went down. Main thread writes, worker reads -> mutex.
        struct HealLanded {
            RE::FormID            caster = 0;
            RE::FormID            spell  = 0;
            SelfClock::time_point at{};
            std::uint32_t         sinceDown = 0;
        };
        std::mutex                                     g_healLandedMx;
        std::unordered_map<RE::FormID, HealLanded>     g_healLanded;

        // NoteLifeState's per-follower record. Worker-serial (the Scheduler's own
        // service), no lock (#4).
        struct LifeNote {
            RE::ACTOR_LIFE_STATE  state = RE::ACTOR_LIFE_STATE::kAlive;
            bool                  seen  = false;
            SelfClock::time_point since{};     // when the current state began
            float                 hpAtChange = 0.0f;
            SelfClock::time_point lastLine{};  // rate limit
            std::uint32_t         quiet = 0;   // transitions not printed inside the limit
        };
        std::unordered_map<RE::FormID, LifeNote> g_life;
        constexpr float kBleedLineMinGapSec = 1.0f;

        // The reach of an AIMED heal, from its record: the spell's own Range (SPIT)
        // when set, else the longest projectile range of its effects
        // (EffectSetting::data.projectileBase->data.range), else 0 (= none known).
        // Plain field reads, worker-safe.
        float AimedReachOf(const RE::SpellItem* a_spell) {
            if (!a_spell) return 0.0f;
            if (a_spell->data.range > 0.0f) return a_spell->data.range;
            float proj = 0.0f;
            for (auto* eff : a_spell->effects) {
                auto* base = eff ? eff->baseEffect : nullptr;
                auto* p    = base ? base->data.projectileBase : nullptr;
                if (p && p->data.range > proj) proj = p->data.range;
            }
            return proj;
        }

        // HEAL OTHER, Skyrim.esm 0x00012FD2 (the base game's own aimed ally heal:
        // Fire and Forget, Target Actor, Range 0, projectile 0x00012FDC with range
        // 10000 -- the same in Mysticism's override; read from Skyrim.esm and
        // MysticismMagic.esp 2026-09-29). Skyrim.esm is always load index 00.
        constexpr RE::FormID kHealOther = 0x00012FD2;

        const char* LifeName(RE::ACTOR_LIFE_STATE a_s) {
            switch (a_s) {
            case RE::ACTOR_LIFE_STATE::kAlive:         return "alive";
            case RE::ACTOR_LIFE_STATE::kDying:         return "dying";
            case RE::ACTOR_LIFE_STATE::kDead:          return "dead";
            case RE::ACTOR_LIFE_STATE::kUnconcious:    return "unconscious";
            case RE::ACTOR_LIFE_STATE::kReanimate:     return "reanimated";
            case RE::ACTOR_LIFE_STATE::kRecycle:       return "recycle";
            case RE::ACTOR_LIFE_STATE::kRestrained:    return "restrained";
            case RE::ACTOR_LIFE_STATE::kEssentialDown: return "essential down";
            case RE::ACTOR_LIFE_STATE::kBleedout:      return "bleedout";
            default:                                   return "?";
            }
        }
        bool LifeUp(RE::ACTOR_LIFE_STATE a_s) {
            return a_s == RE::ACTOR_LIFE_STATE::kAlive || a_s == RE::ACTOR_LIFE_STATE::kReanimate;
        }
    }
        void ResetCanActState() {
            g_life.clear();   // worker-serial; the pump is stopped around a revert/load
            {
                std::lock_guard lk(g_refusedMx);
                g_refusedLogged.clear();
            }
            {
                std::lock_guard lk(g_healLandedMx);
                g_healLanded.clear();
            }
        }
    namespace {
        const char* FormName(RE::FormID a_id) {
            auto* f = a_id ? RE::TESForm::LookupByID(a_id) : nullptr;
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }
    }

    void NoteRefusedApply(RE::FormID a_caster, const char* a_road, const char* a_why,
                          RE::FormID a_spell, RE::FormID a_target) {
        {
            std::lock_guard lk(g_refusedMx);
            if (!g_refusedLogged.insert(a_caster).second) return;
        }
        spdlog::info("[bleed] {:08X} {} apply REFUSED on the main thread -- caster cannot act ({}); "
                     "spell {:08X} -> {:08X}, no magicka spent (logged once until he can act again)",
                     a_caster, a_road ? a_road : "?", a_why ? a_why : "?", a_spell, a_target);
    }

    void NoteHealLanded(RE::FormID a_target, RE::FormID a_caster, RE::FormID a_spell) {
        if (!a_target) return;
        std::lock_guard lk(g_healLandedMx);
        auto& h = g_healLanded[a_target];
        h.caster = a_caster;
        h.spell  = a_spell;
        h.at     = SelfClock::now();
        ++h.sinceDown;
    }

    void NoteLifeState(RE::Actor* a_follower) {
        if (!a_follower) return;
        const auto id = a_follower->GetFormID();
        if (CanAct(a_follower)) {
            std::lock_guard lk(g_refusedMx);
            g_refusedLogged.erase(id);   // he can act: the next refusal logs again
        }
        const auto* st = a_follower->AsActorState();
        if (!st) return;
        const auto  life = st->GetLifeState();
        const auto  now  = SelfClock::now();
        const float hp   = Vocab::HealthPct(a_follower);
        auto& n = g_life[id];
        if (!n.seen) {   // first sight this session: baseline, no line
            n.seen = true; n.state = life; n.since = now; n.hpAtChange = hp;
            return;
        }
        if (life == n.state) return;
        const auto  was     = n.state;
        const float heldSec = std::chrono::duration<float>(now - n.since).count();
        const bool  down    = LifeUp(was) && !LifeUp(life);
        n.state = life; n.since = now; n.hpAtChange = hp;
        if (down) {   // a fresh down: count the heals that land on him from here
            std::lock_guard lk(g_healLandedMx);
            if (auto it = g_healLanded.find(id); it != g_healLanded.end()) it->second.sinceDown = 0;
        }
        if (std::chrono::duration<float>(now - n.lastLine).count() < kBleedLineMinGapSec) {
            ++n.quiet;   // rate limit: counted, printed with the next line
            return;
        }
        n.lastLine = now;
        const std::uint32_t quiet = std::exchange(n.quiet, 0u);
        const char* name = a_follower->GetName() ? a_follower->GetName() : "?";
        const std::string flips = quiet ? std::format(" (+{} flips not printed)", quiet) : std::string{};
        if (!LifeUp(life)) {
            spdlog::info("[bleed] {:08X} {}: DOWN ({}; was {} for {:.1f}s) hp {:.0f}%{}",
                         id, name, LifeName(life), LifeName(was), heldSec, hp * 100.0f, flips);
            return;
        }
        HealLanded h{};
        {
            std::lock_guard lk(g_healLandedMx);
            if (auto it = g_healLanded.find(id); it != g_healLanded.end()) h = it->second;
        }
        std::string heal;
        if (h.caster && h.sinceDown > 0) {
            const float ago = std::chrono::duration<float>(now - h.at).count();
            heal = std::format("last MFO heal on him: {} ({:08X}) by {} ({:08X}) {:.1f}s before, {} since he went down",
                               FormName(h.spell), h.spell, FormName(h.caster), h.caster, ago, h.sinceDown);
        } else {
            heal = "no MFO heal landed on him while down";
        }
        spdlog::info("[bleed] {:08X} {}: UP ({} after {:.1f}s {}) hp {:.0f}% -- {}{}",
                     id, name, LifeName(life), heldSec, LifeName(was), hp * 100.0f, heal, flips);
    }

    float HealReach(RE::SpellItem* a_spell) {
        if (!a_spell) return 0.0f;
        float reach = 0.0f;
        switch (a_spell->data.delivery) {
        case RE::MagicSystem::Delivery::kSelf:
        case RE::MagicSystem::Delivery::kTouch:
            // marth 2026-09-29 (option B): a Self / Touch heal delivered to an ALLY
            // reaches as far as Heal Other, the aimed ally heal. (A self-heal on the
            // caster never asks: HealInReach / RefuseHealApplyOnMain pass self.)
            reach = AimedReachOf(RE::TESForm::LookupByID<RE::SpellItem>(kHealOther));
            break;
        default:
            reach = AimedReachOf(a_spell);
            break;
        }
        return reach > 0.0f ? reach : std::numeric_limits<float>::max();
    }

    bool HealInReach(RE::Actor* a_caster, RE::Actor* a_target, RE::SpellItem* a_spell) {
        if (!a_caster || !a_target || !a_spell) return false;
        if (a_caster == a_target) return true;
        if (a_caster->GetPosition().GetDistance(a_target->GetPosition()) > HealReach(a_spell)) return false;
        return Sightline::Check(a_caster->GetFormID(), a_target->GetFormID()) != Sightline::Verdict::Occluded;
    }

    bool HealsHealth(RE::SpellItem* a_spell) { return SpellHealsHealth(a_spell); }

    bool RefuseHealApplyOnMain(RE::Actor* a_caster, RE::Actor* a_target, RE::SpellItem* a_spell,
                               const char* a_road) {
        if (!a_caster || !a_target || !a_spell || a_caster == a_target) return false;
        if (!SpellHealsHealth(a_spell)) return false;
        const float dist  = a_caster->GetPosition().GetDistance(a_target->GetPosition());
        const float reach = HealReach(a_spell);
        const char* why   = nullptr;
        if (dist > reach)
            why = "beyond the spell's reach";
        else if (Sightline::MeasureNow(a_caster->GetFormID(), a_target->GetFormID()) ==
                 Sightline::Verdict::Occluded)
            why = "no line of sight";
        if (!why) return false;
        // MAIN THREAD only (every caller is a MainThread::Post closure), so a
        // function-local dedup map needs no lock.
        static std::unordered_map<std::uint64_t, SelfClock::time_point> s_log;
        const auto now = SelfClock::now();
        auto& last = s_log[(static_cast<std::uint64_t>(a_caster->GetFormID()) << 32) | a_target->GetFormID()];
        if (std::chrono::duration<float>(now - last).count() >= 5.0f) {
            last = now;
            spdlog::info("[cast] {:08X} {} heal {} ({:08X}) at {:08X} REFUSED on the main thread -- {} "
                         "(distance {:.0f}, reach {:.0f}); no magicka spent",
                         a_caster->GetFormID(), a_road ? a_road : "?",
                         a_spell->GetName() ? a_spell->GetName() : "?", a_spell->GetFormID(),
                         a_target->GetFormID(), why, dist, reach);
        }
        return true;
    }

}
