#pragma once
#include "PCH.h"
#include "iaus/Snapshot.h"   // Iaus::PerkRead: the plain-data part the IAUS snapshot carries

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// iaus/PerkEffects.h -- WHAT A FOLLOWER'S PERKS DO, READ BY EFFECT (ClickUp 1dfcuabd2pk; design Docs/research/2026-11/perk-effects.md).
//
// marth 2026-11-12: "Perks are just one more thing that needs a weight" ... "And what spells to use. Armor to pick, it needs to read perks like
// potions and see how they affect the world."
//
// A perk is never read by its name, its editor id or its place in a tree. Two reads, both of engine data only:
//   1. THE ENGINE'S OWN ANSWER per entry point. For each effect MFO weighs, the engine function every perk entry point runs through
//      (BGSEntryPoint::HandleEntryPoint, the verified row "TradePerks.BGSEntryPoint.HandleEntryPoint") is asked with the follower as the perk owner
//      and a REAL item as the thing asked about: each weapon kind he carries, each armor type he carries, each spell he knows. The engine runs
//      every entry's own conditions on the owner, the item and the target, so MFO interprets no condition at all. That is the conservative
//      handling: a condition MFO cannot read is never guessed, it is evaluated by the code that owns it. The answer is a multiplier (value out /
//      value in). Argument shapes: logistics/TradePerks.h (each decoded from an engine call site on all three executables).
//   2. A CENSUS of his perk records: how many entries of each entry point his perks hold, and the actor values his perk ABILITIES change on him
//      now (the live effects of those ability spells, so the engine's own conditions on the effects are respected too). The census is for the log
//      and for kinds no consideration reads yet; it changes no decision.
//
// THREADING (INVARIANTS #84's pattern, the EffectMirror shape): perks live on the actor and its base and change on the main thread, and the entry
// point conditions read live actor state with no lock. So the table is BUILT ON THE MAIN THREAD (BuildPass, posted with the snapshot pass from
// Diagnostics.cpp) and READ ANYWHERE through an immutable shared_ptr behind a leaf mutex. Get() never waits: no table yet returns null, and
// every reader treats null as "not known" = the multiplier 1.0 / the consideration not applicable (what MFO did before this reader existed).
// A table has NO EXPIRY (principle 9: a floor is safe): perks change on a level-up or a respec, not per tick, so the last table stands until a
// newer one replaces it. One follower is rebuilt per pass, each at most every kRefreshTicks (~2 s); a follower the engine cannot evaluate now (no
// middle-high process: unloaded) keeps his last table.
//
// NOT serialized. No co-save, ABI, address-id, INI or ESP change. To add a perk kind: MAP.md section 11 "PERK EFFECTS".
namespace MFO::PerkEffects {

    inline constexpr std::size_t kWeaponKinds = 9;   // Progression::WeaponKind bit index: 0 sword 1 dagger 2 war axe 3 mace 4 greatsword 5 battleaxe 6 warhammer 7 bow 8 crossbow
    inline constexpr std::size_t kArmorKinds  = static_cast<std::size_t>(Iaus::PerkArmor::kCount);
    inline constexpr std::size_t kEntryPoints = 92;  // the engine's frozen entry-point count (RE::BGSEntryPoint::ENTRY_POINT::kTotal, static_asserted in the .cpp)

    // Where a kind's answer was asked: about an item he carries, about the load order's generic item of the kind (he carries none), or not at all.
    enum class Probe : std::uint8_t { None = 0, Own, Generic };

    struct Table {
        RE::FormID     actor = 0;
        Iaus::PerkRead read;   // what the IAUS snapshot carries (weapon classes, spell schools, armor types, block)

        // Weapon damage by fine kind (entry point 35 ModAttackDamage): the multiplier, what was asked about, and where it came from.
        std::array<float, kWeaponKinds>      weaponKind{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        std::array<RE::FormID, kWeaponKinds> weaponForm{};
        std::array<Probe, kWeaponKinds>      weaponProbe{};
        // Armor rating by type (entry point 85 ModArmorRating); the value itself is read.armorRating. Pieces asked, and where from.
        std::array<std::uint8_t, kArmorKinds> armorPieces{};
        std::array<Probe, kArmorKinds>        armorProbe{};
        std::uint16_t spellsKnown = 0, spellsAsked = 0;   // castable spells he knows / how many were asked (kMaxSpells caps one build)

        // THE CENSUS (record data, no decision reads it): his perks, their entries by type, entry-point entries by entry point id, and the actor
        // values his perk abilities change on him NOW (live effects whose source is a perk ability; magnitudes summed per actor value).
        std::uint16_t perks = 0, entryPointEntries = 0, abilityEntries = 0, questEntries = 0;
        std::array<std::uint16_t, kEntryPoints>  entryCount{};
        std::vector<std::pair<int, float>>       abilityAv;   // (RE::ActorValue index, summed magnitude), ascending index

        std::uint16_t refused = 0;   // engine asks that gave no answer or a non-finite / absurd one this build (each leaves its multiplier at 1.0)
        std::uint32_t buildMicros = 0;
        std::string   text;          // the `[perk-fx]` table line (also the change signature)
    };
    using Ptr = std::shared_ptr<const Table>;

    // ANY THREAD. The last table built for a_actor, or null = not known yet (asked for). Never waits.
    [[nodiscard]] Ptr Get(RE::FormID a_actor);
    // ANY THREAD. Keep a_actor's table built (the party, every tick: Diagnostics.cpp beside EffectMirror::WantParty). No read.
    void Want(RE::FormID a_actor);

    // THE GEAR PICKS' FACTORS (any thread; 1.0 when there is no table, the group is unread, or the weight is 0):
    //   1 + weight x (multiplier - 1), weight = Tuning::perkArmorWeight / perkWeaponWeight (MFO_Tuning/shared.txt `t.perkArmor` / `t.perkWeapon`).
    [[nodiscard]] float ArmorFactor(RE::FormID a_actor, Iaus::PerkArmor a_kind);
    [[nodiscard]] std::array<float, kWeaponKinds> WeaponFactors(RE::FormID a_actor);
    // The two weights as the tuning table has them now (iaus/TuningFile.cpp calls SetWeights after every shared.txt load; the shipped values until
    // then). Mirrored in atomics because the gear picks also run on the main thread while the worker swaps the tuning table.
    void SetWeights(float a_armor, float a_weapon);

    // MAIN THREAD.
    void Startup();                             // kDataLoaded: the generic probe items, the seat status, one `[perk-fx]` status line
    void BuildPass(std::uint64_t a_postTick);   // once per worker tick (Diagnostics.cpp posts it with InvSnap / EffectMirror BuildPass): at most ONE follower
    [[nodiscard]] Ptr BuildLive(RE::Actor* a_actor);   // the read itself; null when the engine cannot evaluate him now
    void Clear();                               // revert / load (Serialization.cpp ResetAllState, beside EffectMirror::Clear)

    // The table line of a_t (what `[perk-fx]` prints). Any thread.
    [[nodiscard]] std::string Describe(const Table& a_t, std::string_view a_name);

}
