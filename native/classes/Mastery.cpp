// classes/Mastery.cpp -- class levels, mastery, unlocks, free switching (card C5).
// There is NO class Auto picker (marth 2026-11-10: "Auto is disabled per follower when classes are enabled on that follower"); the
// player picks the class, and the removed C5 picker never assigns one.
//
// State lives in the MCLS record (State.cpp): ClassProgress{xp, level}, the mastered / unlocked bitsets, equipped.
// This file adds NO persisted field and changes no layout. XP accrual is card C4 (it stores xp); this file turns stored xp
// into a level through C4's threshold table (Classes.h "C4 CONTRACT"), sets mastery at the tier cap, evaluates the
// UnlockRules of ClassTable.cpp against the mastered set, and switches the equipped class. The strong-effect scale is a pure
// function for card C6, which owns the carriers. Everything is inert unless Active() (Presence.cpp's gate): the follower
// functions go through State.cpp, whose writers refuse unless Writable() (which implies Active()).
#include "Classes_internal.h"

namespace MFO::Classes {

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
        return out & ((Mask{ 1 } << kClassCount) - 1);
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
        if (cap == 0) return false;
        if (a_cumulative.size() != cap) {   // a short table could never reach mastery, a long one would be silently cut (F4)
            static std::atomic<Mask> warned{ 0 };   // once per class per session
            if (!(warned.fetch_or(Bit(a_class)) & Bit(a_class)))
                spdlog::error("[classes] SetLevelFromXp({}): threshold table has {} entries, the cap is {} -- REFUSED (the C4 table is wrong)",
                              Get(a_class).slug, a_cumulative.size(), static_cast<int>(cap));
            return false;
        }
        const auto p = GetProgress(a_follower, a_class);
        return ApplyLevel(a_follower, a_class, LevelForXp(p.xp, a_cumulative));   // ONE compound write (State.cpp)
    }

    bool SwitchClass(RE::FormID a_follower, Id a_class) {
        return ActivateClass(a_follower, a_class);   // unlocked-check + level-1 start + equip in ONE compound write (State.cpp)
    }
}
