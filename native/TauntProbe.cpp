#include "PCH.h"
#include "TauntProbe.h"
#include "Config.h"
#include "MainThread.h"
#include "apmf/APMFBridge.h"   // SenseOf (MAIN THREAD ONLY)
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace MFO::TauntProbe {

    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr double kP1PeriodS = 5.0;     // design section 6: every 5 s per follower
        constexpr double kP2PeriodS = 2.0;     // design section 6: every 2 s per follower
        constexpr float  kFoeRange  = 2048.0f; // design section 6: foes within 2048 u
        constexpr std::size_t kMaxFoes = 6;    // per follower per lap: bounds the log
        constexpr double kFoeKeepS  = 30.0;    // a foe not seen for this long loses its switch baseline

        // Skyrim.esm records of the combat idle tree (Docs/TAUNT-DESIGN.md 3.1, parsed from the esm).
        constexpr RE::FormID kIdleABC     = 0x000B8C73;   // CombatIdleABC, under withShield
        constexpr RE::FormID kIdleRegular = 0x000B8C71;   // CombatIdleRegular, under Standard
        constexpr RE::FormID kRoot        = 0x000B8C70;   // CombatIdleRoot
        constexpr RE::FormID kStandard    = 0x0001BA8B;   // CombatIdleStandard
        constexpr RE::FormID kWithShield  = 0x000B8C72;   // CombatIdleStandard_withShield

        struct Sample {
            Clock::time_point p1{};
            Clock::time_point p2{};
        };
        std::unordered_map<RE::FormID, Sample> g_samples;   // keyed by follower; worker-serial

        struct FoeSeen {
            RE::FormID        tgt = 0;
            Clock::time_point since{};    // when this target was first seen
            Clock::time_point lastSeen{};
        };
        std::unordered_map<RE::FormID, FoeSeen> g_foes;     // keyed by foe; worker-serial

        double Since(const Clock::time_point& a_t, const Clock::time_point& a_now) {
            return std::chrono::duration<double>(a_now - a_t).count();
        }

        // WEAPON_TYPE for the log (TESObjectWEAP::GetWeaponType, 0..9), 10 for a shield, else -1. Not the
        // engine's GetEquippedItemType numbering.
        int ItemType(RE::Actor* a_f, bool a_left) {
            auto* o = a_f->GetEquippedObject(a_left);
            if (!o) return -1;
            if (auto* w = o->As<RE::TESObjectWEAP>()) return static_cast<int>(w->GetWeaponType());
            if (auto* a = o->As<RE::TESObjectARMO>(); a && a->IsShield()) return 10;
            return -1;
        }

        std::string RecName(const RE::TESIdleForm* a_i) {
            const char* e = a_i->GetFormEditorID();
            if (e && *e) return std::format("{} {:08X}", e, a_i->GetFormID());
            return std::format("{:08X}", a_i->GetFormID());
        }

        // ── P1 ──────────────────────────────────────────────────────────────────────────
        void ServiceP1(RE::Actor* a_f, RE::FormID a_id) {
            auto tp = a_f->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
            auto* tgt = tp.get();
            if (!tgt || tgt == a_f || tgt->IsDead() || tgt->IsDisabled()) return;

            const int r = ItemType(a_f, false);
            const int l = ItemType(a_f, true);
            // The loadout picks the idle (design 3.1): shield + one-hander -> 0x0B8C73,
            // everything else -> 0x0B8C71 (a non-melee loadout then fails CombatIdleStandard).
            const bool shield1H = l == 10 && r >= 1 && r <= 4;
            const RE::FormID idleId = shield1H ? kIdleABC : kIdleRegular;
            auto* idle = RE::TESForm::LookupByID<RE::TESIdleForm>(idleId);
            if (!idle) {
                spdlog::info("[taunt-probe] 0x{:08X} idle record {:08X} NOT FOUND (form lookup null)", a_id, idleId);
                return;
            }

            // Walk idle -> parentIdle -> ... -> root, then evaluate root-first. An empty
            // condition list (head null) passes without calling the engine.
            std::vector<const RE::TESIdleForm*> chain;
            for (const RE::TESIdleForm* c = idle; c && chain.size() < 8; c = c->parentIdle) chain.push_back(c);

            auto pass = [&](const RE::TESIdleForm* a_i) {
                return !a_i->conditions || a_i->conditions.IsTrue(a_f, tgt);
            };
            bool        root = true, standard = true, self = true, haveShield = false, shieldPass = true;
            std::string firstFail;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const RE::TESIdleForm* c = *it;
                const bool ok = pass(c);
                if (c->GetFormID() == kRoot)            root = ok;
                else if (c->GetFormID() == kStandard)   standard = ok;
                else if (c->GetFormID() == kWithShield) { haveShield = true; shieldPass = ok; }
                if (c == idle) self = ok;
                if (!ok && firstFail.empty()) firstFail = RecName(c);
            }
            const bool would = firstFail.empty();
            const float dist = a_f->GetPosition().GetDistance(tgt->GetPosition());
            const auto pf = [](bool b) { return b ? "PASS" : "FAIL"; };
            spdlog::info("[taunt-probe] 0x{:08X} idle {} in combat (target 0x{:08X}, dist {:.0f}u, pathing n/a, R={} L={}): "
                         "root={} standard={} shieldBranch={} self={} -> {}{}",
                         a_id, idleId == kIdleABC ? "B8C73" : "B8C71", tgt->GetFormID(), dist, r, l,
                         pf(root), pf(standard), haveShield ? pf(shieldPass) : "n/a", pf(self),
                         would ? "WOULD PLAY" : "WOULD NOT",
                         would ? std::string() : std::format(" (first failing: {})", firstFail));
        }

        // ── P2 ──────────────────────────────────────────────────────────────────────────
        enum class Hold { kHeld, kLost, kAbsent, kNoGroup };
        const char* HoldName(Hold h) {
            switch (h) {
                case Hold::kHeld:   return "HELD";
                case Hold::kLost:   return "LOST";
                case Hold::kAbsent: return "ABSENT";
                default:            return "NOGROUP";   // the foe has no combat controller / group this lap
            }
        }

        // The sense read (Harbinger SenseActor) is MAIN THREAD ONLY, so the finished line is logged
        // from a main-thread Post; without a live pump it logs at once with "unknown". Read only.
        void LogFoeLine(RE::FormID a_foe, RE::FormID a_f, std::string a_head) {
            if (!MainThread::IsInstalled()) {
                spdlog::info("{}; senses follower: unknown", a_head);
                return;
            }
            MainThread::Post([a_foe, a_f, head = std::move(a_head)] {
                const char* sense = "unknown";
                APMFBridge::SenseReading rd{};
                if (APMFBridge::SenseOf(a_foe, a_f, 0, 0.0f, rd)) {
                    if (rd.senses) sense = "yes";
                    else if (rd.detail & APMF_API::kAwareDetail_SightDeferred) sense = "deferred";   // sight ray not cast this frame
                    else sense = "no";
                }
                spdlog::info("{}; senses follower: {}", head, sense);
            });
        }

        void ServiceP2(RE::Actor* a_f, RE::FormID a_id, const Clock::time_point& a_now) {
            auto* cc = a_f->GetActorRuntimeData().combatController;
            if (!cc || !cc->combatGroup) return;

            // Pass 1: the follower's own group, ONE read lock, only handle resolution inside (#23).
            std::vector<RE::NiPointer<RE::Actor>> foes;
            {
                RE::BSReadLockGuard lk(cc->combatGroup->lock);
                for (const auto& t : cc->combatGroup->targets) {
                    auto p = t.targetHandle.get();   // HOLD the NiPointer
                    auto* foe = p.get();
                    if (!foe || foe == a_f || foe->IsDead() || foe->IsDisabled()) continue;
                    foes.push_back(std::move(p));
                }
            }

            const auto fHandle = a_f->GetHandle().native_handle();
            std::size_t logged = 0;
            for (auto& fp : foes) {
                RE::Actor* foe = fp.get();
                if (!foe->IsInCombat()) continue;   // controller-lifetime gate, as every group read here
                if (a_f->GetPosition().GetDistance(foe->GetPosition()) > kFoeRange) continue;

                auto  tp  = foe->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
                auto* cur = tp.get();
                if (!cur) continue;

                // Target-switch baseline per foe (its currentCombatTarget changes): EVERY in-range, in-combat foe,
                // whoever it targets, so a foe going A -> summon -> A is seen.
                const RE::FormID foeId = foe->GetFormID();
                const RE::FormID curId = cur->GetFormID();
                auto [it, fresh] = g_foes.try_emplace(foeId, FoeSeen{ curId, a_now, a_now });
                if (!fresh) {
                    it->second.lastSeen = a_now;
                    if (it->second.tgt != curId) {
                        const auto ms = static_cast<long long>(Since(it->second.since, a_now) * 1000.0);
                        spdlog::info("[taunt-probe] foe 0x{:08X} target switch 0x{:08X} -> 0x{:08X} after {} ms",
                                     foeId, it->second.tgt, curId, ms);
                        it->second.tgt   = curId;
                        it->second.since = a_now;
                    }
                }

                const bool party = cur->IsPlayerRef() || cur->IsPlayerTeammate();
                if (!party) continue;   // only a foe fighting the party gets the HELD/LOST line (design section 6)
                if (logged >= kMaxFoes) continue;   // cap AFTER the in-combat, range and party filters
                ++logged;

                // The foe's OWN group, a fresh read lock with nothing nested (pass 1's lock is released).
                Hold         hold = Hold::kNoGroup;
                std::int32_t detect = 0;
                int          attackers = 0;
                if (auto* fcc = foe->GetActorRuntimeData().combatController; fcc && fcc->combatGroup) {
                    hold = Hold::kAbsent;
                    RE::BSReadLockGuard lk(fcc->combatGroup->lock);
                    for (const auto& t : fcc->combatGroup->targets) {
                        if (t.targetHandle.native_handle() != fHandle) continue;
                        hold      = t.flags.any(RE::CombatTarget::Flags::kTargetLost) ? Hold::kLost : Hold::kHeld;
                        detect    = t.detectLevel;
                        attackers = t.attackerCount;
                        break;
                    }
                }

                LogFoeLine(foeId, a_id,
                    std::format("[taunt-probe] foe 0x{:08X} (L{}) targets 0x{:08X} (party=1); follower 0x{:08X} (L{}) "
                                "in foe's group targets: {} detect={} attackers={}",
                                foeId, foe->GetLevel(), curId, a_id, a_f->GetLevel(), HoldName(hold), detect, attackers));
            }

            // Prune baselines of foes not seen for a while (dead, unloaded, fight over).
            for (auto it = g_foes.begin(); it != g_foes.end();) {
                if (Since(it->second.lastSeen, a_now) > kFoeKeepS) it = g_foes.erase(it);
                else ++it;
            }
        }

    }  // namespace

    void Service(RE::Actor* a_f, RE::FormID a_id, bool a_ownCombat) {
        if (!Config::g_tauntProbe.load()) return;
        if (!a_f) return;
        if (!a_ownCombat) { g_samples.erase(a_id); return; }
        const auto now = Clock::now();
        auto&      s   = g_samples[a_id];
        if (Since(s.p1, now) >= kP1PeriodS) { s.p1 = now; ServiceP1(a_f, a_id); }
        if (Since(s.p2, now) >= kP2PeriodS) { s.p2 = now; ServiceP2(a_f, a_id, now); }
    }

    void Reset() {
        g_samples.clear();
        g_foes.clear();
    }

}
