#include "PCH.h"
#include "CostParity.h"
#include "Config.h"
#include "Forms.h"
#include "MainThread.h"
#include <cmath>

namespace MFO::CostParity {

    namespace {
        std::atomic<bool> g_active{ false };

        // Leaf mutex over the per-follower memory (worker writes in Sync, main thread erases a failed job).
        std::mutex                              g_mx;
        std::unordered_map<RE::FormID, bool>    g_state;   // last desired state posted for this follower this session

        // The engine's skill cost curve factor, as disassembled (id 26505, AE 1.6.1170):
        //   (1 - powf(Base * skill, Scale)) * Mult      [K = 1.0]
        // applied to the spell's cost before the kModSpellCost perk step. A factor below 0 is not a cost; clamp for the log.
        float Factor(float a_base, float a_scale, float a_mult, float a_skill) {
            const float f = (1.0f - std::pow(a_base * a_skill, a_scale)) * a_mult;
            return f < 0.0f ? 0.0f : f;
        }

        // MAIN thread. Bring one follower to the desired state. Returns false if the actor did not resolve (retry later).
        bool Apply(RE::FormID a_id, bool a_want) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            auto* ab    = Forms::g_costParityAbility;
            auto* perk  = Forms::g_costParityPerk;
            if (!actor || !ab || !perk) return false;
            if (a_want) {
                if (!actor->HasSpell(ab)) actor->AddSpell(ab);
                // The perk may sit on a SHARED base record (a generic NPC); AddPerk is runtime-only (never saved), so it is
                // re-added each session. Its entry condition is HasSpell(ability), evaluated per caster, so a sibling who
                // shares the base and holds no ability pays nothing extra. ApplyPerksFromBase settles THIS actor (the
                // same shape as the progression allocator, progression/Allocator.cpp).
                auto* base = actor->GetActorBase();
                if (base) {
                    if (!base->GetPerkIndex(perk).has_value()) base->AddPerk(perk, 1);
                    actor->ApplyPerksFromBase();
                }
                spdlog::info("[costparity] {:08X} {} -- spell costs x2 (marker ability {:08X} on)", a_id,
                             actor->GetName(), ab->GetFormID());
            } else if (actor->HasSpell(ab)) {
                actor->RemoveSpell(ab);
                spdlog::info("[costparity] {:08X} {} -- marker ability removed, engine cost unchanged", a_id, actor->GetName());
            }
            return true;
        }
    }

    bool Active() { return g_active.load(std::memory_order_relaxed); }

    void Reset() {
        std::scoped_lock lk(g_mx);
        g_state.clear();
    }

    void StartupCheck() {
        g_active = false;
        if (!Config::g_followerSpellCostParity.load()) {
            spdlog::info("[costparity] bFollowerSpellCostParity=0 -- followers pay the engine cost unchanged");
            return;
        }
        if (!Forms::g_costParityAbility || !Forms::g_costParityPerk) {
            spdlog::error("[costparity] MFO_CostParityAbility/Perk did not resolve (ability {}, perk {}) -- is MFO.esp current? "
                          "Followers pay the engine cost unchanged", Forms::g_costParityAbility ? "ok" : "MISSING",
                          Forms::g_costParityPerk ? "ok" : "MISSING");
            return;
        }
        auto* gsc = RE::GameSettingCollection::GetSingleton();
        if (!gsc) {
            spdlog::error("[costparity] GameSettingCollection unavailable -- followers pay the engine cost unchanged");
            return;
        }
        // Six GMSTs. A missing one refuses the whole check: never run on a guessed constant.
        const char* missing = nullptr;
        auto get = [&](const char* a_name) -> float {
            auto* st = gsc->GetSetting(a_name);
            if (!st) { if (!missing) missing = a_name; return 0.0f; }
            return st->GetFloat();
        };
        const float pcBase  = get("fMagicCasterPCSkillCostBase");
        const float pcMult  = get("fMagicCasterPCSkillCostMult");
        const float pcScale = get("fMagicPCSkillCostScale");
        const float npBase  = get("fMagicCasterSkillCostBase");
        const float npMult  = get("fMagicCasterSkillCostMult");
        const float npScale = get("fMagicSkillCostScale");
        if (missing) {
            spdlog::error("[costparity] GMST {} not found -- the cost curves cannot be compared; followers pay the engine cost "
                          "unchanged", missing);
            return;
        }
        spdlog::info("[costparity] player curve: Base {:.4f} Scale {:.3f} Mult {:.3f}; NPC curve: Base {:.4f} Scale {:.3f} Mult {:.3f}",
                     pcBase, pcScale, pcMult, npBase, npScale, npMult);
        const float pc50  = Factor(pcBase, pcScale, pcMult, 50.0f);
        const float np50  = Factor(npBase, npScale, npMult, 50.0f);
        const float pc100 = Factor(pcBase, pcScale, pcMult, 100.0f);
        const float np100 = Factor(npBase, npScale, npMult, 100.0f);
        spdlog::info("[costparity] cost factor before perks, skill 50: player {:.3f} NPC {:.3f} (NPC pays {:.0f}% of the player); "
                     "skill 100: player {:.3f} NPC {:.3f} (NPC pays {:.0f}%)",
                     pc50, np50, pc50 > 0.0f ? 100.0f * np50 / pc50 : 0.0f,
                     pc100, np100, pc100 > 0.0f ? 100.0f * np100 / pc100 : 0.0f);
        if (np50 < pc50) {
            g_active = true;
            spdlog::info("[costparity] NPC curve is cheaper than the player's at skill 50 ({:.3f} < {:.3f}) -- ACTIVE: a managed "
                         "follower's spell costs are doubled by the engine's kModSpellCost perk step", np50, pc50);
        } else {
            spdlog::info("[costparity] NPC curve not cheaper ({:.3f} >= {:.3f} at skill 50) -- followers pay engine cost unchanged",
                         np50, pc50);
        }
    }

    void Sync(RE::FormID a_actorID, bool a_want) {
        // Inactive session: only ever REMOVE (sweeps a marker left by an earlier session or a toggled-off INI).
        const bool want = a_want && Active();
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_state.find(a_actorID);
            if (it != g_state.end() && it->second == want) return;
            g_state[a_actorID] = want;
        }
        if (!Forms::g_costParityAbility || !Forms::g_costParityPerk) return;   // nothing to add or remove
        MainThread::Post([a_actorID, want]() {
            if (!Apply(a_actorID, want)) {
                std::scoped_lock lk(g_mx);
                g_state.erase(a_actorID);   // unresolved this time: the next sweep tries again
            }
        });
    }
}
