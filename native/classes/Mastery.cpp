// classes/Mastery.cpp -- class levels, mastery, unlocks, free switching, strict-adherence Auto (card C5).
//
// State lives in the MCLS record (State.cpp): ClassProgress{xp, level}, the mastered / unlocked bitsets, equipped.
// This file adds NO persisted field and changes no layout. XP accrual is card C4 (it stores xp); this file turns stored xp
// into a level through C4's threshold table (Classes.h "C4 CONTRACT"), sets mastery at the tier cap, evaluates the
// UnlockRules of ClassTable.cpp against the mastered set, and switches the equipped class. The strong-effect scale is a pure
// function for card C6, which owns the carriers. Everything is inert unless Active() (Presence.cpp's gate): the follower
// functions go through State.cpp, whose writers refuse unless Writable() (which implies Active()).
#include "Classes_internal.h"

namespace MFO::Classes {

    namespace {
        constexpr Mask Roster() { return (Mask{ 1 } << kClassCount) - 1; }

        // Skill group values of a StatProfile.
        float MeleeV(const StatProfile& s)  { return std::max(s.oneHanded, s.twoHanded); }
        float RangedV(const StatProfile& s) { return s.archery; }
        float MageV(const StatProfile& s) {
            return std::max({ s.alteration, s.conjuration, s.destruction, s.illusion, s.restoration });
        }
        float GroupV(const StatProfile& s, std::uint8_t a_root) {   // a_root: 1 Melee, 2 Ranged, 3 Mage
            return a_root == 1 ? MeleeV(s) : a_root == 2 ? RangedV(s) : MageV(s);
        }

        // Fit of a class to a profile: the mean of its skill groups. A line class = its own group; a hybrid = its
        // weapon-side root plus Mage (hybrid = one weapon class + one school class, ClassTable.cpp); rootT1 0 = every group.
        float Fit(const ClassDef& d, const StatProfile& s) {
            if (d.line != Line::Hybrid) return GroupV(s, d.rootT1);
            if (d.rootT1 == 0) return (MeleeV(s) + RangedV(s) + MageV(s)) / 3.0f;
            if (d.rootT1 == 3) return MageV(s);
            return (GroupV(s, d.rootT1) + MageV(s)) * 0.5f;
        }
    }

    std::uint8_t LevelCap(Id a_class) {
        if (static_cast<std::size_t>(a_class) >= kClassCount) return 0;
        switch (kTable[static_cast<std::size_t>(a_class)].tier) {
            case 1: return 10;
            case 2: return 15;
            case 3: return 20;
            case 4: return 25;
            default: return 0;   // T5 Dragonborn: no cap, no level
        }
    }

    float Scale01(std::uint32_t a_level, std::uint32_t a_cap) {
        if (a_level < 1) return 0.0f;
        if (a_cap <= 1 || a_level >= a_cap) return 1.0f;
        return 0.25f + 0.75f * static_cast<float>(a_level - 1) / static_cast<float>(a_cap - 1);
    }

    std::uint8_t LevelForXp(std::uint32_t a_xp, std::span<const std::uint32_t> a_cumulative) {
        if (a_cumulative.empty()) return 0;
        const std::size_t n = std::min<std::size_t>(a_cumulative.size(), 255);
        std::size_t lvl = 1;   // level 1 is the floor
        for (std::size_t i = 1; i < n; ++i) {
            if (a_xp < a_cumulative[i]) break;
            lvl = i + 1;
        }
        return static_cast<std::uint8_t>(lvl);
    }

    Mask ComputeUnlocked(Mask a_mastered) {
        Mask out = 0;
        for (const auto& d : kTable)
            if (UnlockSatisfied(d, a_mastered)) out |= Bit(d.id);
        return out & Roster();
    }

    bool IsUnlocked(RE::FormID a_follower, Id a_class) {
        if (!Active() || static_cast<std::size_t>(a_class) >= kClassCount) return false;
        const Mask stored = GetUnlocked(a_follower);
        const Mask live   = ComputeUnlocked(GetMastered(a_follower));
        return ((stored | live) & Bit(a_class)) != 0;
    }

    bool SetLevelFromXp(RE::FormID a_follower, Id a_class, std::span<const std::uint32_t> a_cumulative) {
        if (static_cast<std::size_t>(a_class) >= kClassCount) return false;
        const std::uint8_t cap = LevelCap(a_class);
        if (cap == 0 || !IsUnlocked(a_follower, a_class)) return false;
        const auto cur = GetFollower(a_follower).value_or(FollowerClasses{});
        ClassProgress p = cur.progress[static_cast<std::size_t>(a_class)];

        std::uint8_t lvl = std::min(LevelForXp(p.xp, a_cumulative), cap);
        if (lvl < p.level) lvl = p.level;   // a level is never taken away
        bool changed = false;
        if (lvl != p.level) {
            p.level = lvl;
            if (!SetProgress(a_follower, a_class, p)) return false;
            changed = true;
        }
        Mask mastered = cur.mastered;
        if (lvl >= cap && !(mastered & Bit(a_class))) {
            mastered |= Bit(a_class);
            if (!SetMastered(a_follower, mastered)) return changed;
            changed = true;
            spdlog::info("[classes] {:08X} mastered {} (level {})", a_follower, Get(a_class).slug, static_cast<int>(lvl));
        }
        const Mask unlocked = cur.unlocked | ComputeUnlocked(mastered);   // unlocks are never taken away
        if (unlocked != cur.unlocked) {
            if (!SetUnlocked(a_follower, unlocked)) return changed;
            changed = true;
        }
        return changed;
    }

    bool SwitchClass(RE::FormID a_follower, Id a_class) {
        if (!Writable() || !IsUnlocked(a_follower, a_class)) return false;
        if (GetEquipped(a_follower) == static_cast<std::uint8_t>(a_class)) return false;
        if (LevelCap(a_class) != 0) {
            ClassProgress p = GetProgress(a_follower, a_class);
            if (p.level == 0) {
                p.level = 1;
                if (!SetProgress(a_follower, a_class, p)) return false;
            }
        }
        return SetEquipped(a_follower, a_class);
    }

    StatProfile ReadStats(RE::Actor* a_actor) {
        StatProfile s;
        auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
        if (!avo) return s;
        using AV = RE::ActorValue;
        s.oneHanded   = avo->GetBaseActorValue(AV::kOneHanded);
        s.twoHanded   = avo->GetBaseActorValue(AV::kTwoHanded);
        s.archery     = avo->GetBaseActorValue(AV::kArchery);
        s.alteration  = avo->GetBaseActorValue(AV::kAlteration);
        s.conjuration = avo->GetBaseActorValue(AV::kConjuration);
        s.destruction = avo->GetBaseActorValue(AV::kDestruction);
        s.illusion    = avo->GetBaseActorValue(AV::kIllusion);
        s.restoration = avo->GetBaseActorValue(AV::kRestoration);
        return s;
    }

    std::optional<Id> PickAutoClass(const StatProfile& a_stats, Mask a_unlocked) {
        std::optional<Id> best;
        float bestFit = 0.0f;
        std::uint8_t bestTier = 0;
        for (const auto& d : kTable) {
            if (d.id == Id::Dragonborn || !(a_unlocked & Bit(d.id))) continue;
            const float fit = Fit(d, a_stats);
            // Strictly better fit, or an equal fit at a higher tier. Table order is ascending Id, so the lower Id wins ties.
            if (!best || fit > bestFit || (fit == bestFit && d.tier > bestTier)) {
                best = d.id; bestFit = fit; bestTier = d.tier;
            }
        }
        return best;
    }

    std::optional<Id> AutoClassFor(RE::FormID a_follower, RE::Actor* a_actor) {
        if (!Strict() || !a_actor) return std::nullopt;
        const Mask unlocked = GetUnlocked(a_follower) | ComputeUnlocked(GetMastered(a_follower));
        return PickAutoClass(ReadStats(a_actor), unlocked);
    }
}
