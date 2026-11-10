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
        // -- per-class skill map (card C5, review F2) --
        // Kept HERE, not in ClassTable.cpp (the C7 generator parses ClassTable's Mk rows). A class's fit = the mean of its listed
        // skills' values in the StatProfile; derived from the class identities in the walkthrough decisions (passives, weapons).
        enum class Sk : std::uint8_t { None, Melee, Block, Archery, Sneak, Alteration, Conjuration, Destruction, Illusion, Restoration, AnySchool };

        float SkillV(const StatProfile& s, Sk a_sk) {
            switch (a_sk) {
                case Sk::Melee:       return std::max(s.oneHanded, s.twoHanded);   // melee 1H / 2H are merged (marth)
                case Sk::Block:       return s.block;
                case Sk::Archery:     return s.archery;
                case Sk::Sneak:       return s.sneak;
                case Sk::Alteration:  return s.alteration;
                case Sk::Conjuration: return s.conjuration;
                case Sk::Destruction: return s.destruction;
                case Sk::Illusion:    return s.illusion;
                case Sk::Restoration: return s.restoration;
                case Sk::AnySchool:   return std::max({ s.alteration, s.conjuration, s.destruction, s.illusion, s.restoration });
                default:              return 0.0f;
            }
        }

        struct SkillRow { Id id; std::array<Sk, 3> sk; };
        constexpr std::uint8_t SkillCount(const SkillRow& r) {
            std::uint8_t n = 0;
            for (auto k : r.sk) if (k != Sk::None) ++n;
            return n;
        }
        constexpr SkillRow R(Id a_id, Sk a, Sk b = Sk::None, Sk c = Sk::None) { return { a_id, { a, b, c } }; }

        // In ascending Id order, one row per class except Dragonborn (checked below). Rows marked OPEN are ambiguous in the
        // walkthrough file and are listed in the C5 report for marth's ruling.
        constexpr std::array<SkillRow, kClassCount - 1> kSkillMap = { {
            R(Id::Melee, Sk::Melee), R(Id::Ranged, Sk::Archery), R(Id::Mage, Sk::AnySchool),
            R(Id::Knight, Sk::Melee, Sk::Block),                      // shield wall
            R(Id::Warrior, Sk::Melee),                                // OPEN: no skill named beyond "any melee weapon"
            R(Id::Mercenary, Sk::Melee),                              // OPEN: stamina passive, in-and-out skirmisher, no skill named
            R(Id::Archer, Sk::Archery),
            R(Id::Thief, Sk::Sneak, Sk::Archery),                     // OPEN: dagger + bow; sneak-led
            R(Id::Evoker, Sk::Destruction), R(Id::Conjurer, Sk::Conjuration), R(Id::Healer, Sk::Restoration), R(Id::Warlock, Sk::Illusion),
            R(Id::GreatKnight, Sk::Melee, Sk::Block),
            R(Id::Champion, Sk::Melee), R(Id::Blademaster, Sk::Melee),   // OPEN (as Warrior / Mercenary)
            R(Id::Ranger, Sk::Archery),
            R(Id::MasterThief, Sk::Sneak, Sk::Archery),               // OPEN (as Thief)
            R(Id::Wizard, Sk::Destruction, Sk::Illusion),             // OPEN: Destruction on a controlled foe + cheaper spells
            R(Id::Necromancer, Sk::Conjuration),
            R(Id::Priest, Sk::Restoration),
            R(Id::Spellsword, Sk::Melee, Sk::Destruction),            // Evoker side
            R(Id::Sorcerer, Sk::Melee, Sk::Conjuration),              // Conjurer side
            R(Id::Templar, Sk::Melee, Sk::Restoration),               // Healer side (shield not weighted)
            R(Id::Nightblade, Sk::Melee, Sk::Illusion),               // Warlock side
            R(Id::Witchhunter, Sk::Archery, Sk::Illusion),            // Archer + Warlock
            R(Id::Scout, Sk::Archery, Sk::Restoration),               // Archer + Healer
            R(Id::ArchMage, Sk::AnySchool),                           // two of Wizard / Necromancer / Priest: any single school
            R(Id::LivingLegend, Sk::Melee),
            R(Id::Nightingale, Sk::Sneak, Sk::Archery, Sk::Illusion), // Master Thief + Witchhunter
            R(Id::Listener, Sk::Sneak, Sk::Illusion),                 // OPEN: Master Thief + Nightblade, Illusion strong effect
            R(Id::EbonyWarrior, Sk::Melee, Sk::Block),
            R(Id::GrandBattlemage, Sk::Melee, Sk::AnySchool),         // power attack releases an off-hand spell
            R(Id::Jaqspur, Sk::Archery),
            R(Id::Saint, Sk::Restoration),
        } };
        constexpr bool SkillMapIsComplete() {
            for (std::size_t i = 0; i < kSkillMap.size(); ++i)
                if (static_cast<std::size_t>(kSkillMap[i].id) != i || SkillCount(kSkillMap[i]) == 0) return false;
            return static_cast<std::size_t>(Id::Dragonborn) == kSkillMap.size();   // Dragonborn is the last Id and has no row
        }
        static_assert(SkillMapIsComplete(), "classes/Mastery.cpp: every non-Dragonborn class needs a skill-map row, in Id order");

        float Fit(const SkillRow& r, const StatProfile& s) {
            float sum = 0.0f;
            for (auto k : r.sk) sum += SkillV(s, k);
            return sum / static_cast<float>(SkillCount(r));
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
        s.sneak       = avo->GetBaseActorValue(AV::kSneak);
        s.block       = avo->GetBaseActorValue(AV::kBlock);
        return s;
    }

    std::optional<Id> PickAutoClass(const StatProfile& a_stats, Mask a_unlocked) {
        std::optional<Id> best;
        float bestFit = 0.0f;
        std::uint8_t bestTier = 0, bestN = 0;
        for (const auto& r : kSkillMap) {
            if (!(a_unlocked & Bit(r.id))) continue;
            const float fit = Fit(r, a_stats);
            const std::uint8_t tier = kTable[static_cast<std::size_t>(r.id)].tier, n = SkillCount(r);
            // Better fit; else equal fit at a higher tier; else equal tier with more listed skills. Ascending Id order
            // means the lower Id keeps a full tie.
            const bool better = !best || fit > bestFit ||
                (fit == bestFit && (tier > bestTier || (tier == bestTier && n > bestN)));
            if (better) { best = r.id; bestFit = fit; bestTier = tier; bestN = n; }
        }
        return best;
    }

    std::optional<Id> AutoClassFor(RE::FormID a_follower, RE::Actor* a_actor) {
        if (!Strict() || !a_actor) return std::nullopt;
        const Mask unlocked = GetUnlocked(a_follower) | ComputeUnlocked(GetMastered(a_follower));
        return PickAutoClass(ReadStats(a_actor), unlocked);
    }
}
