#pragma once
// Board_internal.h -- the Board family's SHARED substrate. One TU (Board.cpp)
// used to hold all of this in a single anonymous namespace; the mechanical
// module split (Board.cpp / Board_Progression.cpp / Board_FieldKit.cpp) moved
// the cross-module
// state, types, and small helpers here as `inline` (ONE shared instance across
// the TUs -- never per-TU copies), and declares the cross-module draw entry
// next to the module that defines it. Single-module state stays file-local in
// its module. NOT a public API: only the Board*.cpp TUs may include this.
// (Same pattern as Logistics_internal.h.)

#include "PCH.h"
#include "Board.h"
#include "Vocabulary.h"   // the frozen opcode strings the vocabulary tables name

#include <imgui.h>

#include <mutex>
#include <vector>

namespace MFO::Board {

    // ── EDIT COMMAND QUEUE ──────────────────────────────────────────────────
    // The board draws on the RENDER thread; the tables live on the main thread.
    // So an edit is enqueued from the draw and APPLIED in PublishSnapshot,
    // which runs on the main thread (#2, the same rule as everything touching
    // g_followers). Never write a rule table from a draw call.
    // SetCond/SetAct are ABSOLUTE picks (the FFXII list-picker chooses an index
    // directly, rather than cycling): c.param carries the index into this
    // table's condition/action vocabulary. Cycle* remain for any legacy caller.
    enum class EditKind : std::uint8_t { Add, Del, MoveUp, MoveDown, Toggle,
                                         CycleCond, CycleAct, SetParam, SetSpell,
                                         SetCond, SetAct, TeachSpell,
                                         SetSubject, SetSubjectActor,   // #68
                                         SetClassOverride,   // #65
                                         SetMfoEnabled,      // #78 per-follower MFO switch
                                         AddonAction };   // v1.1 Phase 6c: ONE generic
                                                            // add-on-verb carrier. The specific
                                                            // verb rides EditCmd::verbId (an
                                                            // add-on-agnostic int) — EditKind no
                                                            // longer enumerates progression verbs.
    // v1.1 Phase 6c: the verbs an EditKind::AddonAction can carry. The CARRIER
    // (EditKind/EditCmd) is add-on-agnostic; these ids + the dispatch in
    // ApplyEdits stay progression-shaped until Phase 7/9 routes verbs through
    // the manifest. Args reuse the existing EditCmd fields: `fid` = the follower,
    // `perk` = a form arg (class-def / catalog node), `param` = an int/bool arg
    // (manual toggle 0/1 | AV ordinal) — the {verbId, follower, arg-union} shape.
    enum class AddonVerb : int { SetClass = 0, AllocPerk, Respec,
                                 SetManual, ApplySkillPoint };
    struct EditCmd {
        EditKind kind; RE::FormID fid; int table; std::uint32_t uid; float param;
        RE::FormID spell = 0;
        RE::FormID book  = 0;   // #4 TeachSpell: the spellbook to consume
        RE::FormID subjectActor = 0;   // #68 SetSubjectActor: the specific follower
        RE::FormID perk  = 0;   // AddonAction form arg (class-def / catalog node)
        int        verbId = 0;  // v1.1 Phase 6c: AddonVerb for EditKind::AddonAction
    };
    // #68: written into a Gambit's subjectSelector when SetSubjectActor picks
    // a SPECIFIC follower, so a stale subject value from an earlier Self/
    // Player/NearestAlly pick cannot linger and mislead. Deliberately OUTSIDE
    // Vocab::Subject's real range (0/1/2) -- ResolveCastTarget's switch falls
    // into its `default:` case (Self) if the specific actor ever becomes
    // unavailable, exactly the same graceful demotion an unrecognised value
    // would get anyway.
    constexpr std::uint8_t kSubjectSpecificFollower = 0xFF;
    inline std::mutex          g_editMx;
    inline std::vector<EditCmd> g_edits;

    inline void QueueEdit(EditCmd c) { std::scoped_lock lk(g_editMx); g_edits.push_back(c); }

    // Header font (drawn titles / accent headers). Loaded by Board.cpp's
    // D3DInitHook; read by both draw TUs. Null -> ImGui default font.
    inline ImFont* g_fontHead = nullptr;

    // ── SKINS (DESIGN §6.7a, standing family rule) ──────────────────────
    // Four named skins, palettes copied VERBATIM from MEO's kSkins so MFO
    // is the same brand, not merely similar. Square corners, flat fills --
    // ImGui's honest range, closer to Skyrim than its debug grey.
    struct MenuSkin {
        const char* name;
        ImVec4 winBg, panel, border, text, dim, sel, accent, btn, track, danger;
        bool   sans;
        const char* title;
    };
    inline constexpr MenuSkin kSkins[4] = {
        { "Ebony & Brass",
          { 0.04f,0.04f,0.06f,0.98f }, { 0.07f,0.07f,0.10f,0.98f },
          { 0.55f,0.48f,0.27f,0.60f }, { 0.91f,0.89f,0.84f,1.00f },
          { 0.58f,0.55f,0.47f,1.00f }, { 0.34f,0.29f,0.16f,0.85f },
          { 0.78f,0.70f,0.45f,1.00f }, { 0.13f,0.11f,0.07f,0.90f },
          { 1.00f,1.00f,1.00f,0.08f }, { 0.76f,0.29f,0.24f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Dwemer Parchment",
          { 0.92f,0.88f,0.80f,0.99f }, { 0.95f,0.92f,0.85f,1.00f },
          { 0.54f,0.45f,0.25f,0.85f }, { 0.21f,0.17f,0.12f,1.00f },
          { 0.48f,0.43f,0.34f,1.00f }, { 0.86f,0.81f,0.66f,1.00f },
          { 0.43f,0.29f,0.16f,1.00f }, { 0.89f,0.84f,0.72f,1.00f },
          { 0.00f,0.00f,0.00f,0.10f }, { 0.55f,0.23f,0.18f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Soul Cairn",
          { 0.07f,0.06f,0.13f,0.98f }, { 0.10f,0.08f,0.19f,0.98f },
          { 0.35f,0.31f,0.55f,0.70f }, { 0.85f,0.84f,0.92f,1.00f },
          { 0.55f,0.52f,0.66f,1.00f }, { 0.16f,0.14f,0.31f,0.90f },
          { 0.53f,0.85f,0.92f,1.00f }, { 0.13f,0.10f,0.23f,0.90f },
          { 1.00f,1.00f,1.00f,0.08f }, { 0.76f,0.29f,0.24f,1.00f },
          false, "FOLLOWER OVERHAUL" },
        { "Quicksilver",
          { 0.04f,0.05f,0.06f,0.94f }, { 0.07f,0.08f,0.10f,0.96f },
          { 0.22f,0.25f,0.29f,1.00f }, { 0.83f,0.85f,0.88f,1.00f },
          { 0.47f,0.50f,0.54f,1.00f }, { 0.14f,0.19f,0.23f,0.90f },
          { 0.56f,0.72f,0.80f,1.00f }, { 0.09f,0.11f,0.13f,0.90f },
          { 1.00f,1.00f,1.00f,0.07f }, { 0.76f,0.29f,0.24f,1.00f },
          true, "F O L L O W E R   O V E R H A U L" },
    };


    // ── SHARED BOARD STATE (promoted by the Board_FieldKit split) ───────
    // These four lived in Board.cpp's anonymous namespace until DrawFieldKit
    // moved to its own TU. They are `inline` here for the SAME reason g_editMx
    // is: ONE instance shared by every Board TU, never a per-TU copy. The rest
    // of the board's state (the snapshot, the overlay-probe atomics, the D3D
    // handles) is used by Board.cpp alone and deliberately stays file-local
    // there. Comments are the originals, moved verbatim.
    inline std::mutex g_ioMx;        // guards ALL ImGui IO, across all three threads
    inline std::atomic<bool> g_open{ false };     // full panel -- SWALLOWS input
    inline std::atomic<bool> g_wantClose{ false };   // keyboard shout-key close (unconditional)
    // Set on OPEN. R1 (the RShoulder) is the Field Orders power on the deck,
    // so the press that OPENS the board is an R1 hold; the draw waits for
    // that press to be released before R1 counts as a party-switch, so the
    // board does not skip a follower the instant it appears (same class as
    // the B sticky-release guard).
    inline std::atomic<bool> g_justOpened{ false };

    // ── THE EDITOR VOCABULARY (promoted by the Board_FieldKit split) ────
    // The tables and their three scanners are read by BOTH the panel
    // (Board_FieldKit.cpp, which renders them) and Board.cpp (ApplyEdits /
    // PublishSnapshot, which resolve an opcode back to a label/ParamKind), so
    // they became shared substrate the moment the panel moved TU. `inline
    // constexpr` = one instance, exactly like kSkins above; the entries are
    // moved VERBATIM from Board.cpp. The opcode strings are FROZEN (#10).
    // The vocabulary the editor cycles through, opcode + human label + what its
    // param MEANS. Frozen opcode strings (#10); labels are UI only.
    //
    // ParamKind drives the param widget AND fixes the "1000%" bug: a count
    // condition (arrows < 10) stored 10.0 and was rendered as 10*100 = 1000%.
    // Percent params are fractions 0..1 shown as a %; Count params are whole
    // numbers; None conditions take no param at all.
    // Distance = whole units (a range), edited like a count but on a wider
    // scale (a bow reaches ~1500u, a cell is 4096u).
    enum class ParamKind : std::uint8_t { None, Percent, Count, Distance };
    struct VocabEntry { const char* op; const char* label; ParamKind kind = ParamKind::None; };

    // COMBAT and LOGISTICS have DISTINCT vocabularies. The editor shows the set
    // that matches the table you are editing -- so a logistics rule can actually
    // pick "loot potions", which the old single combat-only table made
    // impossible (marth: "no way to assign" the logistics gambits).
    inline constexpr VocabEntry kCondsCombat[] = {
        { Vocab::kCondAlways,        "Always",               ParamKind::None    },
        { Vocab::kCondSelfHpBelow,   "Self HP % below",      ParamKind::Percent },
        { Vocab::kCondSelfMpBelow,   "Self Magicka % below", ParamKind::Percent },
        { Vocab::kCondSelfSpBelow,   "Self Stamina % below", ParamKind::Percent },
        { Vocab::kCondPlayerHpBelow, "Player HP % below",    ParamKind::Percent },
        { Vocab::kCondFoeLowestHp,   "Foe: lowest HP %",     ParamKind::None    },
        { Vocab::kCondFoeHpBelow,    "Foe: HP % below",      ParamKind::Percent },
        { Vocab::kCondFoeHighestHp,  "Foe: highest HP %",    ParamKind::None    },
        { Vocab::kCondFoeHighestLevel,"Foe: highest level",  ParamKind::None    },
        { Vocab::kCondFoeAny,        "Foe: nearest",         ParamKind::None    },
        { Vocab::kCondFoeWithinRange,"Foe targeted within range", ParamKind::Distance},
        { Vocab::kCondFoeBeyondRange,"Foe targeted beyond range", ParamKind::Distance},
        { Vocab::kCondFoeAttackingPlayer,"Foe attacking player", ParamKind::None },
        { Vocab::kCondFoeAttackingMe,"Foe attacking me",     ParamKind::None    },
        { Vocab::kCondFoeAttackingMeMelee, "Foe attacking me: melee",  ParamKind::None },
        { Vocab::kCondFoeAttackingMeRanged,"Foe attacking me: ranged", ParamKind::None },
        { Vocab::kCondFoeIsUndead,   "Foe is undead",        ParamKind::None    },
        { Vocab::kCondFoeIsDragon,   "Foe is dragon",        ParamKind::None    },
        { Vocab::kCondFoeIsCaster,   "Foe is a spellcaster", ParamKind::None    },
        { Vocab::kCondFoeIsRanged,   "Foe is ranged",        ParamKind::None    },
        { Vocab::kCondFoeWeakerThanMe, "Foe is weaker than me", ParamKind::None },
        { Vocab::kCondFoeStrongerThanMe,"Foe is stronger than me", ParamKind::None },
        { Vocab::kCondFoeBlocking,   "Foe is blocking",      ParamKind::None    },
        { Vocab::kCondFoeFleeing,    "Foe is fleeing",       ParamKind::None    },
        { Vocab::kCondFoeWeakFire,   "Foe: weak to fire",    ParamKind::None    },
        { Vocab::kCondFoeWeakFrost,  "Foe: weak to frost",   ParamKind::None    },
        { Vocab::kCondFoeWeakShock,  "Foe: weak to shock",   ParamKind::None    },
        { Vocab::kCondFoeCountAtLeast,"Foe count at least",  ParamKind::Count   },
        { Vocab::kCondSelfHpAbove,   "Self HP % above",      ParamKind::Percent },
        { Vocab::kCondSelfMpAbove,   "Self Magicka % above", ParamKind::Percent },
        { Vocab::kCondSelfSpAbove,   "Self Stamina % above", ParamKind::Percent },
        { Vocab::kCondAllyHpBelow,   "Ally HP % below",      ParamKind::Percent },
        // SUPPLY-STATE conditions (#10). Mirrors kCondsLogi -- lets a combat
        // gambit react to what the follower is CARRYING, not just health bars:
        // "Arrows below 5 -> Equip melee weapon", "Health potions below 2 ->
        // Cast heal on self" (conserve the stack). ConditionTrue answers these
        // the same in either table (it walks the follower's inventory, table-
        // agnostic), so no evaluator change is needed -- this is pure UI exposure.
        { Vocab::kCondSelfLowHealthPotion, "Health potions below",  ParamKind::Count   },
        { Vocab::kCondSelfLowStaminaPotion,"Stamina potions below", ParamKind::Count   },
        { Vocab::kCondSelfLowMagickaPotion,"Magicka potions below", ParamKind::Count   },
        { Vocab::kCondSelfOutOfArrows,     "Arrows below",          ParamKind::Count   },
        { Vocab::kCondSelfOutOfBolts,      "Bolts below",           ParamKind::Count   },
        { Vocab::kCondIsInterior,    "In an interior",       ParamKind::None    },
        { Vocab::kCondIsNight,       "At night",             ParamKind::None    },
        { Vocab::kCondDark,          "Dark",                 ParamKind::None    },
    };
    inline constexpr VocabEntry kActsCombat[] = {
        { Vocab::kActWait,              "Wait" },
        { Vocab::kActCastSelf,          "Cast on self" },
        { Vocab::kActCastTarget,        "Cast at foe/ally" },
        // cast_player is LOGISTICS-ONLY for now: in combat CastOn's package/grace
        // path delivers a self-delivery buff to the follower, not the player
        // (Fable, 2026-08-06). Combat player-casts are a follow-up.
        { Vocab::kActAttack,            "Attack" },
        { Vocab::kActDrinkHealthPotion, "Drink health potion" },
        { Vocab::kActDrinkStaminaPotion,"Drink stamina potion" },
        { Vocab::kActDrinkMagickaPotion,"Drink magicka potion" },
        { Vocab::kActEquipRanged,       "Equip ranged weapon" },
        { Vocab::kActEquipMelee,        "Equip melee weapon" },
        { Vocab::kActPowerAttack,       "Power attack" },
        { Vocab::kActFlee,              "Flee to player" },
    };
    inline constexpr VocabEntry kCondsLogi[] = {
        { Vocab::kCondAlways,              "Always",                ParamKind::None    },
        { Vocab::kCondSelfHpBelow,         "Self HP % below",       ParamKind::Percent },
        { Vocab::kCondSelfMpBelow,         "Self Magicka % below",  ParamKind::Percent },
        { Vocab::kCondSelfSpBelow,         "Self Stamina % below",  ParamKind::Percent },
        { Vocab::kCondSelfLowHealthPotion, "Health potions below",  ParamKind::Count   },
        { Vocab::kCondSelfLowStaminaPotion,"Stamina potions below", ParamKind::Count   },
        { Vocab::kCondSelfLowMagickaPotion,"Magicka potions below", ParamKind::Count   },
        { Vocab::kCondSelfOutOfArrows,     "Arrows below",          ParamKind::Count   },
        { Vocab::kCondSelfOutOfBolts,      "Bolts below",           ParamKind::Count   },
        { Vocab::kCondSelfCarryWeightAbove,"Carry weight % above",  ParamKind::Percent },
        // Ally-health support targeting in LOGISTICS: "Ally HP % below -> Cast
        // heal on ally" for out-of-combat heals. The ally selector (incl. the
        // player, PickAlly) is table-agnostic in the evaluator, so this is pure
        // UI exposure -- no evaluator change needed to run it in the logi scan.
        { Vocab::kCondAllyHpBelow,         "Ally HP % below",       ParamKind::Percent },
        { Vocab::kCondIsInterior,          "In an interior",        ParamKind::None    },
        { Vocab::kCondIsNight,             "At night",              ParamKind::None    },
        { Vocab::kCondDark,                "Dark",                  ParamKind::None    },
    };
    inline constexpr VocabEntry kActsLogi[] = {
        { Vocab::kActDrinkHealthPotion,  "Drink health potion" },
        { Vocab::kActDrinkStaminaPotion, "Drink stamina potion" },
        { Vocab::kActDrinkMagickaPotion, "Drink magicka potion" },
        { Vocab::kActLootArrows,         "Loot arrows" },
        { Vocab::kActLootBolts,          "Loot bolts" },
        { Vocab::kActLootPotions,        "Loot potions (any)" },
        { Vocab::kActLootHealthPotion,   "Loot health potions" },
        { Vocab::kActLootStaminaPotion,  "Loot stamina potions" },
        { Vocab::kActLootMagickaPotion,  "Loot magicka potions" },
        { Vocab::kActLootEquipment,      "Loot better equipment" },
        { Vocab::kActLootGold,           "Loot gold" },
        { Vocab::kActLootJewelry,        "Loot jewellery" },
        { Vocab::kActLootSoulGems,       "Loot soul gems" },
        { Vocab::kActLootLockpicks,      "Loot lockpicks" },
        { Vocab::kActLootIngredients,    "Loot ingredients" },
        { Vocab::kActLootValuables,      "Loot valuables + gold (to sell)" },
        { Vocab::kActEquipTorch,         "Equip torch" },
        // Cast in logistics (mage update): out-of-combat casting -- self-buffs,
        // candlelight, out-of-combat heals. Same opcodes as combat; the
        // logistics scan dispatches them through Actuation::Fire.
        { Vocab::kActCastSelf,           "Cast on self" },
        { Vocab::kActCastTarget,         "Cast at foe/ally" },
        { Vocab::kActCastPlayer,         "Cast on player" },
        { Vocab::kActWait,               "Wait" },   // gate lower rules, e.g. "carry weight > 90% -> Wait"
    };

    inline int cycleIdx(const std::string& op, const VocabEntry* tab, int n, int dir) {
        int cur = 0;
        for (int i = 0; i < n; ++i) if (op == tab[i].op) { cur = i; break; }
        return ((cur + dir) % n + n) % n;
    }
    inline const char* labelFor(const std::string& op, const VocabEntry* tab, int n) {
        for (int i = 0; i < n; ++i) if (op == tab[i].op) return tab[i].label;
        return op.empty() ? "(unset)" : op.c_str();
    }
    inline ParamKind kindFor(const std::string& op, const VocabEntry* tab, int n) {
        for (int i = 0; i < n; ++i) if (op == tab[i].op) return tab[i].kind;
        return ParamKind::None;
    }

    // ── CROSS-MODULE ENTRY POINTS ───────────────────────────────────────
    // Close the board and open the input-swallow grace. DEFINED in Board.cpp
    // (it reports the overlay-probe atomics, which stay file-local there);
    // called by Board::Toggle there and by the panel's [B]/Esc close path in
    // Board_FieldKit.cpp.
    void CloseBoard();

    // The full panel. DEFINED in Board_FieldKit.cpp (mechanical split of
    // Board.cpp when it crossed the 2500-line cap); called ONLY from the
    // Present thunk in Board.cpp. RENDER THREAD.
    void DrawFieldKit(const Snapshot& snap);

    // ── THE PROGRESSION TAB (#74 component 3) ───────────────────────────
    // Defined in Board_Progression.cpp (mechanical split of DrawFieldKit's
    // hosted-tab body). Render thread, inside DrawFieldKit's tab bar. The
    // reference params are DrawFieldKit locals/statics the body reads or
    // writes in place: s_tab (the tab-cycle static), pickerDrawnThisFrame
    // (drives the footer hint). tabSelFlags carries tabSel(2).
    void DrawProgressionTab(const Snapshot& snap, bool progActive,
                            ImGuiTabItemFlags tabSelFlags, ImGuiIO& io,
                            const MenuSkin& skin, bool r1Ready,
                            int& s_tab, bool& pickerDrawnThisFrame);

}
