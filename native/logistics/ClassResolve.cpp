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

        // RULING Q60 (marth 2026-11-10, replaces the first "highest mastered T4" default): a Dragonborn-equipped follower's
        // root is the root of his MOST-PERKED weapon line: One-/Two-Handed -> 1 Melee, Archery -> 2 Ranged, a magic school ->
        // 3 Mage, each line scored by its best skill tree's owned perk ranks (grouped as the vote in ResolveBaseClass). Source:
        // Progression's per-skill owned-rank tally (StyleVotes::skillRanks, the follower's own perks in the engine's skill
        // trees: list-agnostic), read through the StyleVotesFor mirror (any thread; blank until the first main-thread tally
        // lands, which reads as "none"). A tie for the top, or no perks in any line, returns 0 = the caller falls back to
        // the raw pick. The "pickable T4 for combat behaviour" and the situational (flying-dragon bow) switching are NOT here.
        std::uint8_t DragonbornRoot(RE::FormID a_fid) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            if (!actor) return 0;
            const auto sv = StyleVotesFor(actor);
            auto rank = [&sv](RE::ActorValue a_av) {
                return sv.skillRanks[static_cast<int>(a_av) - static_cast<int>(RE::ActorValue::kOneHanded)];
            };
            using AV = RE::ActorValue;
            const int line[3] = {
                std::max(rank(AV::kOneHanded), rank(AV::kTwoHanded)),
                rank(AV::kArchery),
                std::max({ rank(AV::kAlteration), rank(AV::kConjuration), rank(AV::kDestruction),
                           rank(AV::kIllusion), rank(AV::kRestoration) })
            };
            const int best = std::max({ line[0], line[1], line[2] });
            if (best <= 0) {
                // Loud, once per session: a READY tally with no perks anywhere is also what a build without the Progression catalog
                // and without MEO gem themes gives (TallyStyleVotes' no-catalog path fills skillRanks only when bMeoAwareGems is on
                // and MEO supports it), so a Dragonborn follower then silently keeps his raw pick. Not masked (backlog MFO-B635).
                static std::atomic<bool> s_logged{ false };
                if (StyleVotesReady(actor) && !s_logged.exchange(true))
                    spdlog::warn("[classes] Dragonborn root: follower {:08X} has NO owned perk ranks in any weapon line in the style tally "
                                 "(none owned, or the no-catalog tally is gated off: needs the Progression add-on or bMeoAwareGems + MEO); "
                                 "keeping his raw pick (once per session)", a_fid);
                return 0;   // no perks in any line
            }
            int n = 0, at = 0;
            for (int c = 0; c < 3; ++c) if (line[c] == best) { ++n; at = c; }
            return n == 1 ? static_cast<std::uint8_t>(at + 1) : std::uint8_t{ 0 };   // a tie keeps the raw pick
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
        const std::uint8_t equipped = Classes::GetEquipped(a_fid);
        if (equipped >= Classes::kClassCount) return raw;   // kNoClass = classes OFF for him

        // RULING Q59 (CONFIRMED marth 2026-11-10): with classes ON the equipped class beats an older explicit Gambit-tab pick.
        // To flip it, return `raw` here when it is 1..3 (one place; every reader follows).
        const auto eqId = static_cast<Classes::Id>(equipped);
        std::uint8_t root = Classes::Get(eqId).rootT1;

        // RULING Q60 (marth 2026-11-10): Dragonborn (rootT1 0, no fixed root) uses the root of his most-perked weapon line; a tie or no
        // perks falls back to the raw pick. Change DragonbornRoot() only.
        if (eqId == Classes::Id::Dragonborn) root = DragonbornRoot(a_fid);
        return (root >= 1 && root <= 3) ? root : raw;
    }

    bool ClassLocksRoles(RE::FormID a_fid) {
        if (!a_fid || !Classes::Active()) return false;
        const std::uint8_t equipped = Classes::GetEquipped(a_fid);
        return equipped < Classes::kClassCount && equipped != static_cast<std::uint8_t>(Classes::Id::Dragonborn);
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
