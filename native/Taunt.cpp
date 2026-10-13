#include "PCH.h"
#include "Taunt.h"
#include "Config.h"
#include "MainThread.h"
#include "apmf/APMFBridge.h"
#include "APMF_API.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace MFO::Taunt {

    namespace {

        using Clock = std::chrono::steady_clock;

        // Skyrim.esm records of the combat idle tree (Docs/TAUNT-DESIGN.md 3.1). Both raise the event IdleCombatStart;
        // 0x0B8C73 sits under CombatIdleStandard_withShield (shield + one-hander), 0x0B8C71 under CombatIdleStandard.
        constexpr RE::FormID kIdleABC     = 0x000B8C73;   // CombatIdleABC
        constexpr RE::FormID kIdleRegular = 0x000B8C71;   // CombatIdleRegular

        // Harbinger ends a ch.12 claim whose idle was NOT CONFIRMED (the graph never raised its entry event) within
        // kConfirmWaitMs = 3000 ms of world time (APMF channels/Idle.cpp). The ABI has no "confirmed" query, so MFO calls
        // the idle CONFIRMED when its claim has stayed live through that whole window: the claim read live AND this many
        // UNPAUSED ms have passed since it was filed (3000 + a margin for the 133 ms tick and the publish frame).
        // Mirrors Harbinger's constant; if it ever changes there, this changes with it.
        constexpr double kConfirmMs = 3300.0;
        // A foe whose combat flag is down, or a taunter whose flag is down, for this many consecutive laps is "combat
        // ended" (the flag flaps for a lap on a LoS loss; see Scheduler.cpp's party-combat note).
        constexpr int kCombatEndLaps = 2;
        constexpr double kClockMaxStepMs = 400.0;   // one Tick-to-Tick gap counts at most this much unpaused time

        struct Cand {
            RE::ActorHandle h;
            RE::FormID      id = 0;
            bool            inGroup = false;   // the taunter is HELD in this foe's combat group (no ch.21 entry needed)
        };
        struct Attempt {
            RE::ActorHandle taunter;
            RE::ActorHandle chosen;     // the foe the gambit chose (and the idle's target)
            RE::FormID      foeId = 0;  // the idle's target, for the log
            RE::FormID      idle = 0;
            double          startMs = 0.0;
            int             noCombat = 0;
            std::string     name;
        };
        struct Active {
            RE::ActorHandle foe, taunter;
            RE::FormID      foeId = 0, taunterId = 0;
            double          filedMs = 0.0, untilMs = 0.0;
            int             foeNoCombat = 0, taunterNoCombat = 0;
            std::string     taunterName, foeName;
        };

        std::mutex                                g_mx;
        std::unordered_map<RE::FormID, Attempt>   g_attempts;   // keyed by TAUNTER; guarded by g_mx
        std::unordered_map<RE::FormID, Active>    g_active;     // keyed by FOE; guarded by g_mx
        double                                    g_clockMs = 0.0;   // the unpaused clock (guarded by g_mx)
        Clock::time_point                         g_clockAt{};       // worker (Tick) only

        std::string NameOf(const RE::Actor* a) {
            return a && a->GetName() && *a->GetName() ? std::string(a->GetName()) : std::string("?");
        }

        double NowMs() {
            std::scoped_lock lk(g_mx);
            return g_clockMs;
        }

        // Down = dead, dying, unconscious or bleeding out / essential-down. The bleed-out drive of a held engagement
        // walks the pinned foe to a downed pinned target (FACETS Table 23a), so this releases in the SAME tick.
        bool IsDown(RE::Actor* a) {
            if (!a || a->IsDead()) return true;
            if (const auto* st = a->AsActorState()) {
                if (st->IsBleedingOut()) return true;
                const auto ls = st->GetLifeState();
                return ls == RE::ACTOR_LIFE_STATE::kDying || ls == RE::ACTOR_LIFE_STATE::kDead ||
                       ls == RE::ACTOR_LIFE_STATE::kUnconcious;
            }
            return false;
        }

        int ItemType(RE::Actor* a_f, bool a_left) {
            auto* o = a_f->GetEquippedObject(a_left);
            if (!o) return -1;
            if (auto* w = o->As<RE::TESObjectWEAP>()) return static_cast<int>(w->GetWeaponType());
            if (auto* a = o->As<RE::TESObjectARMO>(); a && a->IsShield()) return 10;
            return -1;
        }

        // ── candidates (WORKER: reads the taunter's group, then each foe's own group, each under ONE read lock with
        // nothing nested and only handle resolution inside, #23) ────────────────────────────────────────────────────
        std::vector<Cand> Gather(RE::Actor* a_f, RE::ActorHandle a_chosen) {
            std::vector<Cand> out;
            auto* cc = a_f->GetActorRuntimeData().combatController;
            if (!cc || !cc->combatGroup) return out;

            struct Row { RE::NiPointer<RE::Actor> p; bool chosen; };
            std::vector<Row> rows;
            {
                RE::BSReadLockGuard lk(cc->combatGroup->lock);
                for (const auto& t : cc->combatGroup->targets) {
                    if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
                    auto p = t.targetHandle.get();   // HOLD the NiPointer
                    auto* foe = p.get();
                    if (!foe || foe == a_f) continue;
                    rows.push_back({ std::move(p), t.targetHandle.native_handle() == a_chosen.native_handle() });
                }
            }

            const auto  selfPos = a_f->GetPosition();
            const auto  selfId  = a_f->GetFormID();
            const float radius  = Config::g_tauntRadius.load();
            const auto  maxFoes = static_cast<std::size_t>(std::clamp(Config::g_tauntMaxFoes.load(), 1, 6));

            struct Keep { RE::NiPointer<RE::Actor> p; float d; bool chosen; };
            std::vector<Keep> keep;
            for (auto& r : rows) {
                RE::Actor* foe = r.p.get();
                if (foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded() || !foe->IsInCombat()) continue;
                if (IsPartyMember(foe) || !foe->IsHostileToActor(a_f)) continue;   // never a teammate, never a brawl partner
                auto  tp  = foe->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
                auto* cur = tp.get();
                if (!cur || !IsPartyMember(cur)) continue;   // a taunt never starts a fight: only foes already fighting the party
                const float d = selfPos.GetDistance(foe->GetPosition());
                if (!r.chosen && d > radius) continue;
                {   // already pinned onto THIS taunter by a live taunt: not eligible again (a different taunter repoints it)
                    std::scoped_lock lk(g_mx);
                    const auto it = g_active.find(foe->GetFormID());
                    if (it != g_active.end() && it->second.taunterId == selfId) continue;
                }
                keep.push_back({ std::move(r.p), d, r.chosen });
            }
            std::sort(keep.begin(), keep.end(), [](const Keep& a, const Keep& b) {
                if (a.chosen != b.chosen) return a.chosen;   // the chosen foe first
                return a.d < b.d;                            // then nearest first
            });
            if (keep.size() > maxFoes) keep.resize(maxFoes);

            const auto fHandle = a_f->GetHandle().native_handle();
            for (auto& k : keep) {
                Cand c;
                c.h  = k.p->GetHandle();
                c.id = k.p->GetFormID();
                if (auto* fcc = k.p->GetActorRuntimeData().combatController; fcc && fcc->combatGroup) {
                    RE::BSReadLockGuard lk(fcc->combatGroup->lock);
                    for (const auto& t : fcc->combatGroup->targets) {
                        if (t.targetHandle.native_handle() != fHandle) continue;
                        c.inGroup = !t.flags.any(RE::CombatTarget::Flags::kTargetLost);
                        break;
                    }
                }
                out.push_back(std::move(c));
            }
            return out;
        }

        // ── the roll and the pins (MAIN thread when the pump is live: SenseActor is main-thread only) ─────────────────
        void Resolve(RE::ActorHandle a_taunter, std::vector<Cand> a_cands, bool a_onMain) {
            auto  tp = a_taunter.get();   // HOLD
            auto* t  = tp.get();
            if (!t || IsDown(t) || t->IsDisabled()) {
                spdlog::info("[taunt] taunter gone or down before the roll -- no foe is rolled");
                return;
            }
            const RE::FormID tId = t->GetFormID();
            const std::string tName = NameOf(t);
            const int tLevel = t->GetLevel();

            std::random_device rdev;
            std::mt19937 gen(rdev());
            std::uniform_real_distribution<float> dist(0.0f, 1.0f);

            std::string pieces;
            int n = 0, hits = 0;
            for (auto& c : a_cands) {
                auto  fp  = c.h.get();   // HOLD
                auto* foe = fp.get();
                if (!foe || foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
                ++n;
                const int fLevel = foe->GetLevel();
                std::string note;

                // "Able to sense the taunter" (Harbinger SenseActor, ABI 18, MAIN THREAD ONLY). Only a definitive NO
                // drops the foe. Unusable (ABI < 18, or no pump) or a deferred sight ray is logged and the foe is rolled:
                // it is already fighting the party inside the taunt radius, so refusing it on a missing reading would
                // make an older Harbinger's taunt a coin flip on a feature it lacks.
                if (a_onMain) {
                    APMFBridge::SenseReading rd{};
                    if (APMFBridge::SenseOf(c.id, tId, 0, 0.0f, rd)) {
                        if (rd.senses) {
                            // senses it
                        } else if (rd.detail & APMF_API::kAwareDetail_SightDeferred) {
                            note = " sense=deferred";
                        } else {
                            pieces += std::format("{}{} ({:08X}) L{} SKIP (does not sense the taunter)",
                                                  pieces.empty() ? "" : "; ", NameOf(foe), c.id, fLevel);
                            continue;
                        }
                    } else {
                        note = " sense=unknown";
                    }
                } else {
                    note = " sense=unknown";
                }

                const float p = std::clamp(0.5f + 0.05f * static_cast<float>(tLevel - fLevel), 0.1f, 0.9f);
                const float r = dist(gen);
                const bool  hit = r < p;
                std::string result = hit ? "HIT" : "MISS";
                if (hit) {
                    std::scoped_lock lk(g_mx);   // pin + record atomically (Tick releases under the same lock)
                    const auto res = APMFBridge::PinFoeToTaunter(c.id, tId, c.inGroup);
                    if (res == APMFBridge::TauntPinResult::Refused) {
                        result = "HIT but the pin was REFUSED by Harbinger";
                    } else {
                        ++hits;
                        const double now = g_clockMs;
                        Active a;
                        a.foe = c.h; a.taunter = a_taunter;
                        a.foeId = c.id; a.taunterId = tId;
                        a.filedMs = now;
                        a.untilMs = now + static_cast<double>(Config::g_tauntDuration.load()) * 1000.0;
                        a.taunterName = tName; a.foeName = NameOf(foe);
                        g_active[c.id] = std::move(a);   // a second taunter's repoint resets the timer and the owner
                        if (res == APMFBridge::TauntPinResult::Repointed) result += " (repointed from another taunter)";
                        if (!c.inGroup) result += " +entry (taunter not in the foe's group)";
                    }
                }
                pieces += std::format("{}{} ({:08X}) L{} p={:.2f} roll={:.2f} {}{}", pieces.empty() ? "" : "; ",
                                      NameOf(foe), c.id, fLevel, p, r, result, note);
            }
            spdlog::info("[taunt] {} ({:08X}) TAUNT -> {} foe(s): {}{}", tName, tId, n, n ? pieces : std::string("none left to roll"),
                         n ? std::format(" [{} pinned for {:.0f}s]", hits, Config::g_tauntDuration.load()) : std::string());
        }

        void LogReleased(const Active& a, const std::string& why, double nowMs) {
            spdlog::info("[taunt] {} ({:08X}) released foe {} ({:08X}) ({}) after {} ms", a.taunterName, a.taunterId, a.foeName,
                         a.foeId, why, static_cast<long long>(nowMs - a.filedMs));
        }

        // One attempt, each pump (WORKER).
        void AdvanceAttempt(RE::FormID a_id, double a_nowMs) {
            Attempt a;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_attempts.find(a_id);
                if (it == g_attempts.end()) return;
                a = it->second;
            }
            auto  tp = a.taunter.get();   // HOLD
            auto* t  = tp.get();
            const auto ms = static_cast<long long>(a_nowMs - a.startMs);

            auto drop = [&](const std::string& line) {
                APMFBridge::ReleaseTauntIdle(a_id);
                { std::scoped_lock lk(g_mx); g_attempts.erase(a_id); }
                spdlog::info("[taunt] {} ({:08X}) TAUNT {} after {} ms", a.name, a_id, line, ms);
            };

            const char* stop = nullptr;
            if (!t || t->IsDead() || t->IsDisabled() || !t->Is3DLoaded()) stop = "aborted (taunter gone)";
            else if (IsDown(t)) stop = "aborted (taunter downed)";
            else if (t->IsInCombat()) { std::scoped_lock lk(g_mx); if (auto it = g_attempts.find(a_id); it != g_attempts.end()) it->second.noCombat = 0; }
            else {
                int laps = 0;
                { std::scoped_lock lk(g_mx); if (auto it = g_attempts.find(a_id); it != g_attempts.end()) laps = ++it->second.noCombat; }
                if (laps >= kCombatEndLaps) stop = "aborted (combat ended)";
            }
            if (stop) { drop(stop); return; }

            const auto st = APMFBridge::TauntIdleStateOf(a_id);
            if (st == APMFBridge::TauntIdleState::Pending) return;
            if (st != APMFBridge::TauntIdleState::Live) {
                // A FAILED idle is a failed attempt: no roll, no pin. Said loudly, once.
                drop("idle FAILED: Harbinger ended the ch.12 idle claim (the engine refused it, the behaviour graph never "
                     "raised its entry event within 3 s, or the owner died; APMF.log '[ch.12]' names which). No roll, no pin");
                return;
            }
            if (a_nowMs - a.startMs < kConfirmMs) return;   // live, but the confirmation window is still open

            // CONFIRMED: the idle's claim stayed live through Harbinger's whole entry-event window. Release it (and the
            // ch.1 hold), then roll.
            APMFBridge::ReleaseTauntIdle(a_id);
            { std::scoped_lock lk(g_mx); g_attempts.erase(a_id); }
            spdlog::info("[taunt] {} ({:08X}) idle {} CONFIRMED played after {} ms (claim live through Harbinger's 3 s "
                         "entry-event window) -- rolling", a.name, a_id, a.idle == kIdleABC ? "B8C73" : "B8C71", ms);

            auto cands = Gather(t, a.chosen);
            if (cands.empty()) {
                spdlog::info("[taunt] {} ({:08X}) TAUNT -> 0 foe(s): none eligible any more (dead, unloaded, no longer "
                             "fighting the party, out of reach or already taunted)", a.name, a_id);
                return;
            }
            if (MainThread::IsInstalled())
                MainThread::Post([th = a.taunter, cs = std::move(cands)]() mutable { Resolve(th, std::move(cs), true); });
            else
                Resolve(a.taunter, std::move(cands), false);
        }

        // One active taunt, each pump (WORKER).
        void AdvanceActive(RE::FormID a_foeId, double a_nowMs) {
            Active a;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_active.find(a_foeId);
                if (it == g_active.end()) return;
                a = it->second;
            }
            auto  tp = a.taunter.get();   // HOLD
            auto* t  = tp.get();
            auto  fp = a.foe.get();       // HOLD
            auto* foe = fp.get();

            std::string why;
            // The TAUNTER first: a downed pinned target draws the foe to it (Harbinger's bleed-out drive), so this releases
            // in the SAME tick it is seen.
            if (!t || t->IsDead() || t->IsDisabled()) why = "taunter dead";
            else if (IsDown(t)) why = "taunter downed";
            else if (!foe || foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) why = "foe dead or unloaded";
            else {
                int fl = 0, tl = 0;
                {
                    std::scoped_lock lk(g_mx);
                    if (auto it = g_active.find(a_foeId); it != g_active.end()) {
                        it->second.foeNoCombat     = foe->IsInCombat() ? 0 : it->second.foeNoCombat + 1;
                        it->second.taunterNoCombat = t->IsInCombat() ? 0 : it->second.taunterNoCombat + 1;
                        fl = it->second.foeNoCombat; tl = it->second.taunterNoCombat;
                    }
                }
                if (fl >= kCombatEndLaps || tl >= kCombatEndLaps) why = "combat ended";
                else if (APMFBridge::TauntPinStateOf(a_foeId) == APMFBridge::TauntPinState::Ended)
                    why = "the hold ended itself: Harbinger ended the pin (target no longer a combat target, no path progress "
                          "or arrived but not fighting for 20 s, or a group merge); APMF.log '[hold]' / '[ch.20]' names which";
                else if (a_nowMs >= a.untilMs) {
                    why = "duration";
                    if (!APMFBridge::TauntPinEverLive(a_foeId)) why += "; the pin never went live";
                }
            }
            if (why.empty()) return;

            {
                std::scoped_lock lk(g_mx);   // release under the lock Resolve pins under, and only the entry we judged
                const auto it = g_active.find(a_foeId);
                if (it == g_active.end() || it->second.filedMs != a.filedMs) return;   // refreshed by a new taunt meanwhile
                APMFBridge::ReleaseTauntPin(a_foeId);
                g_active.erase(it);
            }
            LogReleased(a, why, a_nowMs);
        }

    }  // namespace

    bool IsPartyMember(RE::Actor* a_actor) {
        if (!a_actor) return false;
        if (a_actor->IsPlayerRef() || a_actor->IsPlayerTeammate()) return true;
        if (a_actor->IsCommandedActor()) {
            auto cmd = a_actor->GetCommandingActor();   // HOLD the NiPointer
            return cmd && (cmd->IsPlayerRef() || cmd->IsPlayerTeammate());
        }
        return false;
    }

    bool MeleeInRightHand(RE::Actor* a_actor) {
        if (!a_actor) return false;
        const int rt = ItemType(a_actor, false);
        return rt >= static_cast<int>(RE::WEAPON_TYPE::kOneHandSword) && rt <= static_cast<int>(RE::WEAPON_TYPE::kTwoHandAxe);
    }

    Actuation::Outcome Fire(RE::Actor* a_f, const Eval::Choice& a_choice) {
        using R = Actuation::Result;
        if (!a_f) return { R::FailedOther, "taunt: no follower", true };
        if (!APMFBridge::TauntSupported())
            return { R::FailedOther, "taunt needs Harbinger (APMF) 0.9.10 or newer (ABI 17: the ch.12 idle with a form)", true };
        // MELEE-ONLY: the animation is non-negotiable and the vanilla humanoid combat idle needs a melee weapon in the
        // right hand (Docs/TAUNT-DESIGN.md 3.1). Transparent: the scan falls through to the rules below.
        if (!MeleeInRightHand(a_f))
            return { R::FailedSkill, "taunt: no melee weapon in the right hand", true };

        const auto id = a_f->GetFormID();
        {
            std::scoped_lock lk(g_mx);
            if (g_attempts.contains(id))   // one animated idle is one attempt: the next waits for this one
                return { R::NoOp, "taunt in progress (idle playing)", false };
        }
        // The foe the condition selected; a condition that selects none ("Always -> Taunt") falls back to the foe he is
        // fighting. Neither is required: Gather ranks the chosen one first, then the nearest others.
        RE::ActorHandle chosen = a_choice.target;
        if (!chosen.get()) chosen = a_f->GetActorRuntimeData().currentCombatTarget;

        auto cands = Gather(a_f, chosen);
        if (cands.empty())
            return { R::NoOp, "taunt: every foe in reach is already taunted or not fighting the party", true };

        // The idle by loadout (design 3.1): shield + one-hander -> 0x0B8C73, everything else -> 0x0B8C71.
        const int r = ItemType(a_f, false), l = ItemType(a_f, true);
        const bool shield1H = l == 10 && r >= 1 && r <= 4;
        const RE::FormID idleId = shield1H ? kIdleABC : kIdleRegular;
        if (!RE::TESForm::LookupByID<RE::TESIdleForm>(idleId)) {
            spdlog::warn("[taunt] {} ({:08X}) idle record {:08X} NOT FOUND (form lookup null) -- no taunt", NameOf(a_f), id, idleId);
            return { R::FailedOther, "taunt: the combat idle record is missing", true };
        }

        // The idle is played AT the first candidate (the chosen foe when it is eligible): the engine evaluates the
        // idle's own conditions against it.
        const RE::FormID idleTarget = cands.front().id;
        if (!APMFBridge::ClaimTauntIdle(id, idleId, idleTarget))
            return { R::FailedOther, "taunt: Harbinger refused the idle claim", true };

        Attempt a;
        a.taunter = a_f->GetHandle();
        a.chosen  = chosen;
        a.foeId   = idleTarget;
        a.idle    = idleId;
        a.name    = NameOf(a_f);
        {
            std::scoped_lock lk(g_mx);
            a.startMs = g_clockMs;
            g_attempts[id] = a;
        }
        spdlog::info("[taunt] {} ({:08X}) idle {} filed at {:08X} (R={} L={}, {} candidate foe(s)) -- the roll waits for the "
                     "idle to be confirmed played", a.name, id, shield1H ? "B8C73" : "B8C71", idleTarget, r, l, cands.size());
        return { R::Fired, "taunt: idle filed" };
    }

    void Tick() {
        const auto now = Clock::now();
        auto* ui = RE::UI::GetSingleton();
        const bool paused = ui && ui->GameIsPaused();
        {
            std::scoped_lock lk(g_mx);
            if (!paused && g_clockAt.time_since_epoch().count() != 0)
                g_clockMs += std::clamp(std::chrono::duration<double, std::milli>(now - g_clockAt).count(), 0.0, kClockMaxStepMs);
        }
        g_clockAt = now;
        if (paused) return;

        std::vector<RE::FormID> attempts, actives;
        {
            std::scoped_lock lk(g_mx);
            if (g_attempts.empty() && g_active.empty()) return;
            for (const auto& [id, a] : g_attempts) attempts.push_back(id);
            for (const auto& [id, a] : g_active) actives.push_back(id);
        }
        const double nowMs = NowMs();
        for (auto id : attempts) AdvanceAttempt(id, nowMs);
        for (auto id : actives) AdvanceActive(id, nowMs);
    }

    void Clear() {
        {
            std::scoped_lock lk(g_mx);
            for (const auto& [id, a] : g_active) LogReleased(a, "load", g_clockMs);
            g_attempts.clear();
            g_active.clear();
            g_clockMs = 0.0;
        }
        g_clockAt = {};
        APMFBridge::ClearTauntClaims();
    }

}
