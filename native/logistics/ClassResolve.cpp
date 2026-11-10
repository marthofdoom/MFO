// logistics/ClassResolve.cpp -- THE base-class resolver (marth 2026-10-01).
//
// A follower's CLASS (1 Melee / 2 Ranged / 3 Mage, the #65 combatClassOverride
// ordinals) is the player's Gambit-tab pick. When the pick is Auto (0) the class
// is resolved here, and ONLY here: one weighted vote over three signals, the
// gambit table weighted highest ("All 3 weighted towards gambit table for HMS",
// marth 2026-10-01). Consumer today: the base HMS split (progression/Hms.cpp,
// through Followers::ResolvedClassFast). Equipment does NOT use this vote: skills pick
// armor, and WEAPON ROLES (ComputeWeaponRoles, Gear.cpp, marth 2026-10-18) come from the
// EXPLICIT class pick only (combatClassOverride: Melee = melee role only, Ranged = ranged
// role only, Mage = a role only by an equip gambit), and for Auto from the gambit table
// OR owned perk ranks, both ways. Never from what is carried.
//
// Lives in logistics because the gambit-table reads it reuses (TableHasAction,
// HasCastGambit) are logistics helpers.
#include "Logistics_internal.h"
#include "classes/Classes.h"   // class add-on: Active(), GetEquipped(), Get().rootT1 (C5b: no Auto when classes are ON)

namespace MFO::Logistics {

    namespace {

        // ── vote weights ─────────────────────────────────────────────────────
        // marth 2026-10-01: the gambit table weighs highest. Each signal casts a
        // share vector over {Melee, Ranged, Mage} that sums to 1 (or abstains with
        // all zeros), scaled by its weight below. Because kVoteGambit (0.60) is
        // larger than kVoteSkill + kVoteNpcClass (0.40), a gambit table that points
        // at ONE class always decides. A split or empty table hands the call to the
        // skills, then to the engine class.
        constexpr float kVoteGambit   = 0.60f;   // (i)   enabled cast / equip_melee / equip_ranged gambits
        constexpr float kVoteSkill    = 0.25f;   // (ii)  the follower's highest combat skill
        constexpr float kVoteNpcClass = 0.15f;   // (iii) the NPC's engine class (CLAS) skill weights
        constexpr float kVoteTieEps   = 1.0e-4f;

        // Winner-takes-all over three class scores. Equal leaders split the share,
        // and three zeros abstain. a_v is {Melee, Ranged, Mage}.
        void HighestShare(const float (&a_v)[3], float (&a_out)[3]) {
            const float best = std::max({ a_v[0], a_v[1], a_v[2] });
            a_out[0] = a_out[1] = a_out[2] = 0.0f;
            if (!(best > 0.0f)) return;   // abstain (also rejects NaN)
            int n = 0;
            for (int c = 0; c < 3; ++c) if (a_v[c] >= best) ++n;
            for (int c = 0; c < 3; ++c) if (a_v[c] >= best) a_out[c] = 1.0f / static_cast<float>(n);
        }

        // Q60 default (see EffectiveClassPick): the T1 root of the follower's highest-level mastered T4, lower Id on a tie;
        // 0 = none mastered (the caller falls back to the raw pick).
        std::uint8_t DragonbornRoot(const Classes::FollowerClasses& a_fc) {
            std::uint8_t bestRoot = 0, bestLevel = 0;
            bool have = false;
            for (std::size_t i = 0; i < Classes::kClassCount; ++i) {   // ascending Id: a strict > keeps the lower Id on a tie
                const auto id = static_cast<Classes::Id>(i);
                const auto& d = Classes::Get(id);
                if (d.tier != 4 || !(a_fc.mastered & Classes::Bit(id))) continue;
                const std::uint8_t lvl = a_fc.progress[i].level;
                if (!have || lvl > bestLevel) { have = true; bestLevel = lvl; bestRoot = d.rootT1; }
            }
            return bestRoot;
        }

    }

    // C2b (marth 2026-11-10: "Auto is disabled per follower when classes are enabled on that follower"). THE one accessor
    // every class-pick reader goes through. Classes ON for this follower (add-on Active() and equipped != kNoClass): his
    // equipped class's root T1 (1..3). Otherwise the raw Gambit-tab pick, UNCHANGED (classes OFF / add-on absent is
    // byte-for-byte today). The raw combatClassOverride is never written on class equip (it is the persisted classes-OFF pick).
    // Threading: takes the Classes g_mx (a leaf) via GetFollower, so any thread may call it; worker callers pass the
    // g_followers record they already hold. No combat-thread reader exists (they read Followers::ResolvedClassFast, whose
    // mirror PublishActiveMirror builds through ResolveBaseClass below).
    std::uint8_t EffectiveClassPick(RE::FormID a_fid, const FollowerState& a_state) {
        const std::uint8_t raw = a_state.combatClassOverride;
        if (!a_fid || !Classes::Active()) return raw;
        const auto fc = Classes::GetFollower(a_fid);
        if (!fc || fc->equipped >= Classes::kClassCount) return raw;   // kNoClass = classes OFF for him

        // RULING Q59 (DEFAULT, pending marth): with classes ON the equipped class beats an older explicit Gambit-tab pick.
        // To flip it, return `raw` here when it is 1..3 (one place; every reader follows).
        const auto eqId = static_cast<Classes::Id>(fc->equipped);
        std::uint8_t root = Classes::Get(eqId).rootT1;

        // RULING Q60 (DEFAULT, pending marth): Dragonborn (rootT1 0, no fixed root) uses the root of his highest-LEVEL MASTERED
        // T4 class; a level tie goes to the lower Id; no mastered T4 falls back to the raw pick. Change DragonbornRoot() only.
        if (eqId == Classes::Id::Dragonborn) root = DragonbornRoot(*fc);
        return (root >= 1 && root <= 3) ? root : raw;
    }

    std::uint8_t ResolveBaseClass(RE::Actor* a_actor, const FollowerState& a_state) {
        if (const std::uint8_t pick = EffectiveClassPick(a_actor ? a_actor->GetFormID() : 0, a_state); pick >= 1 && pick <= 3)
            return pick;   // the player's pick, or (C2b) his equipped class's root with classes ON: no Auto vote then

        float score[3] = { 0.0f, 0.0f, 0.0f };

        // (i) GAMBIT TABLE -- the same reads the loot judge uses: TableHasAction
        // (enabled equip_melee / equip_ranged) and HasCastGambit (enabled cast).
        // Each authored role takes an equal share.
        {
            const bool m = TableHasAction(a_state.combat(), Vocab::kActEquipMelee);
            const bool r = TableHasAction(a_state.combat(), Vocab::kActEquipRanged);
            const bool c = HasCastGambit(a_state);
            const int  n = (m ? 1 : 0) + (r ? 1 : 0) + (c ? 1 : 0);
            if (n > 0) {
                const float s = kVoteGambit / static_cast<float>(n);
                if (m) score[0] += s;
                if (r) score[1] += s;
                if (c) score[2] += s;
            }
        }

        // (ii) HIGHEST COMBAT SKILL -- "highest skill wins", on BASE skills like
        // SkillScale's DominantWeaponSkill (a base read is a fixed point, a
        // fortify potion cannot flip it). Melee = the better of One/Two-Handed
        // (ComputeWeaponRoles' melee pair), Ranged = Archery, Mage = the best of
        // the five schools (TopTwoSchoolMask's set).
        if (auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr) {
            using AV = RE::ActorValue;
            const float v[3] = {
                std::max(avo->GetBaseActorValue(AV::kOneHanded), avo->GetBaseActorValue(AV::kTwoHanded)),
                avo->GetBaseActorValue(AV::kArchery),
                std::max({ avo->GetBaseActorValue(AV::kAlteration), avo->GetBaseActorValue(AV::kConjuration),
                           avo->GetBaseActorValue(AV::kDestruction), avo->GetBaseActorValue(AV::kIllusion),
                           avo->GetBaseActorValue(AV::kRestoration) })
            };
            float sh[3];
            HighestShare(v, sh);
            for (int c = 0; c < 3; ++c) score[c] += kVoteSkill * sh[c];
        }

        // (iii) ENGINE CLASS -- the NPC's CLAS record skill weights
        // (TESNPC::npcClass -> TESClass::data.skillWeights, fork mit-3.7 fde0f3a
        // TESClass.h:36-58 / TESNPC.h:268), grouped like (ii). Static form data.
        if (auto* base = a_actor ? a_actor->GetActorBase() : nullptr; base && base->npcClass) {
            const auto& w = base->npcClass->data.skillWeights;
            const float v[3] = {
                static_cast<float>(std::max(w.oneHanded, w.twoHanded)),
                static_cast<float>(w.archery),
                static_cast<float>(std::max({ w.alteration, w.conjuration, w.destruction,
                                              w.illusion, w.restoration }))
            };
            float sh[3];
            HighestShare(v, sh);
            for (int c = 0; c < 3; ++c) score[c] += kVoteNpcClass * sh[c];
        }

        // Highest score wins. A tie (and the all-abstain case, three zeros) goes
        // to the LOWEST ordinal: Melee, then Ranged, then Mage. Deterministic.
        int best = 0;
        for (int c = 1; c < 3; ++c)
            if (score[c] > score[best] + kVoteTieEps) best = c;
        return static_cast<std::uint8_t>(best + 1);
    }

}
