// classes/ClassTable.cpp -- THE compiled class table (card C2): 35 classes, constexpr, our own design data
// (never list data). Sources: marth's walkthrough rulings 2026-11-09 (class-walkthrough-decisions.md) over
// the 2026-11-08 tree page and the superseded class-perks-packages.md draft. Slugs match the review page ids.
//
// NOTHING HERE CHANGES RUNTIME BEHAVIOUR. The table is read by the self-check (Classes.cpp) and, in later
// cards, by the mastery/XP/passive/package/GUI code. Default gambits are EXISTING base opcodes (Vocabulary.h
// k-constants, so a missing one is a compile error) or a "todo.*" placeholder row naming the plan card that
// adds the opcode; placeholder rows are never applied.
//
// Per-class conservative choices are marked "CHOICE:" and listed in the C1/C2 report.
#include "Classes_internal.h"

namespace MFO::Classes {

    namespace {
        using Vocab::Subject;
        constexpr std::uint8_t kSelf   = static_cast<std::uint8_t>(Subject::Self);
        constexpr std::uint8_t kEnemy  = static_cast<std::uint8_t>(Subject::Enemy);

        // ── mask helpers ────────────────────────────────────────────────────────────────────────────
        template <class... T> constexpr Mask Ms(T... a_ids) { return (Mask{ 0 } | ... | Bit(a_ids)); }

        constexpr Mask kT1 = Ms(Id::Melee, Id::Ranged, Id::Mage);
        constexpr Mask kT2 = Ms(Id::Knight, Id::Warrior, Id::Mercenary, Id::Archer, Id::Thief,
                                Id::Evoker, Id::Conjurer, Id::Healer, Id::Warlock);
        constexpr Mask kT4 = Ms(Id::ArchMage, Id::LivingLegend, Id::Nightingale, Id::Listener,
                                Id::EbonyWarrior, Id::GrandBattlemage, Id::Jaqspur, Id::Saint);
        constexpr Mask kMeleeT2 = Ms(Id::Knight, Id::Warrior, Id::Mercenary);   // "Melee means any of Knight, Warrior or Mercenary"

        // ── unlock-rule builders ────────────────────────────────────────────────────────────────────
        constexpr UnlockRule None()                       { return { 0, 0, 0, 0, 0 }; }
        constexpr UnlockRule Req(Mask a_all)              { return { a_all, 0, 0, 0, 0 }; }
        // Own base/parent mastered PLUS one more mastered class of the same tier (the "at least two of the tier
        // before" rule for T2 and the pure T3s).
        constexpr UnlockRule Base(Id a_own, Mask a_tier)  { return { Bit(a_own), a_tier & ~Bit(a_own), 1, 0, 0 }; }
        // One weapon class and one school class (hybrids).
        constexpr UnlockRule Pair(Mask a_a, Mask a_b)     { return { 0, a_a, 1, a_b, 1 }; }
        constexpr UnlockRule NOf(Mask a_pool, std::uint8_t a_n) { return { 0, a_pool, a_n, 0, 0 }; }

        // ── gambit row builders ─────────────────────────────────────────────────────────────────────
        constexpr DefaultGambit G(const char* a_c, const char* a_a, float a_p = 0.0f, SpellRole a_r = SpellRole::None,
                                  std::uint8_t a_s = kSelf, float a_cd = 0.0f) {
            return { a_c, a_a, a_p, a_s, a_r, a_cd, nullptr };
        }
        // A row whose cond or act does not exist yet (never applied). a_card = the build-plan card that adds it.
        constexpr DefaultGambit Todo(const char* a_c, const char* a_a, const char* a_card, float a_p = 0.0f,
                                     SpellRole a_r = SpellRole::None, std::uint8_t a_s = kSelf, float a_cd = 0.0f) {
            return { a_c, a_a, a_p, a_s, a_r, a_cd, a_card };
        }

        // Placeholder opcode names (NOT in Vocabulary.h; the plan card that appends each is the second column).
        constexpr const char* kTodoSneak        = "todo.act.sneak";                  // G1
        constexpr const char* kTodoSteal        = "todo.act.steal";                  // G13
        constexpr const char* kTodoShout        = "todo.act.shout";                  // G8
        constexpr const char* kTodoMark         = "todo.act.mark";                   // H-MARK (class-gated, G15)
        constexpr const char* kTodoNoSummon     = "todo.cond.self_no_summon";        // G5 (cond.self_has_summon, negated)
        constexpr const char* kTodoFoeHitsMySummon = "todo.cond.foe_attacking_my_summon"; // G6
        constexpr const char* kTodoCorpse       = "todo.cond.corpse_within";         // G7
        constexpr const char* kTodoChargeAtMe   = "todo.cond.hostile_spell_charging_at_me"; // G11

        // ── weapon sets ─────────────────────────────────────────────────────────────────────────────
        using namespace Weapon;
        constexpr std::uint16_t kMelee1H2H = kOneHand | kTwoHand;                // melee 1H/2H merged, no dagger (marth)
        constexpr std::uint16_t kThiefSet  = kDagger | kBow;                     // "Thief line: dagger + bow" (marth)
        constexpr std::uint16_t kArcherSet = kOneHand | kBow | kCrossbow;        // "archer can only use 1h" (marth)
        constexpr std::uint16_t kMageSet   = kDagger | kStaff;                   // CHOICE: mages keep a dagger (Conjurer gambit) and staves

        // ── default gambit sets (priority order). Pct params are fractions, distances are units. ─────
        // T1 -- vanilla behaviour (marth): the draft's minimal sets.
        constexpr DefaultGambit kMeleeG[] = {
            G(Vocab::kCondSelfHpBelow, Vocab::kActDrinkHealthPotion, 0.30f),
            G(Vocab::kCondFoeAttackingPlayer, Vocab::kActAttack),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kRangedG[] = {
            G(Vocab::kCondSelfOutOfArrows, Vocab::kActEquipMelee, 1.0f),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 300.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged),
            G(Vocab::kCondFoeAttackingPlayer, Vocab::kActAttack) };
        constexpr DefaultGambit kMageG[] = {
            G(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.40f, SpellRole::Heal),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy) };

        // T2
        constexpr DefaultGambit kKnightG[] = {
            G(Vocab::kCondFoeAttackingAlly, Vocab::kActTaunt),
            G(Vocab::kCondFoeAttackingPlayer, Vocab::kActAttack),
            G(Vocab::kCondSelfHpBelow, Vocab::kActDrinkHealthPotion, 0.35f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        // 10 s cooldown on each power-attack row (marth); the field is data until card G9 (per-rule cooldown).
        constexpr DefaultGambit kWarriorG[] = {
            G(Vocab::kCondFoeBlocking, Vocab::kActPowerAttack, 0.0f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActPowerAttack, 0.0f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeHighestHp, Vocab::kActAttack),
            G(Vocab::kCondSelfSpBelow, Vocab::kActDrinkStaminaPotion, 0.25f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        // Casters, then archers, then lowest HP; stamina potion < 25%; else equip melee (marth).
        constexpr DefaultGambit kMercenaryG[] = {
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeIsRanged, Vocab::kActAttack),
            G(Vocab::kCondFoeLowestHp, Vocab::kActAttack),
            G(Vocab::kCondSelfSpBelow, Vocab::kActDrinkStaminaPotion, 0.25f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kArcherG[] = {
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeIsRanged, Vocab::kActAttack),
            G(Vocab::kCondSelfOutOfArrows, Vocab::kActLootArrows, 1.0f),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 150.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        // Sneak classes: sneak "regardless of player action" = an always-true row (G1); Steal is G13.
        constexpr DefaultGambit kThiefG[] = {
            Todo(Vocab::kCondAlways, kTodoSneak, "G1"),
            G(Vocab::kCondFoeLowestHp, Vocab::kActAttack),
            G(Vocab::kCondFoeFleeing, Vocab::kActAttack),
            Todo(Vocab::kCondAlways, kTodoSteal, "G13"),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 150.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        // Trimmed set (marth): fire weakness, nearest enemy, magicka potion.
        constexpr DefaultGambit kEvokerG[] = {
            G(Vocab::kCondFoeWeakFire, Vocab::kActCastTarget, 0.0f, SpellRole::Fire, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Destruction, kEnemy),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f) };
        constexpr DefaultGambit kConjurerG[] = {
            Todo(kTodoNoSummon, Vocab::kActCastSelf, "G5", 0.0f, SpellRole::Summon),
            Todo(kTodoFoeHitsMySummon, Vocab::kActCastTarget, "G6", 0.0f, SpellRole::Attack, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.25f),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 150.0f) };   // dagger when something is on it
        // Trimmed set (marth): heal player < 50, ally < 50, self < 50, magicka potion, nearest enemy.
        constexpr DefaultGambit kHealerG[] = {
            G(Vocab::kCondPlayerHpBelow, Vocab::kActCastPlayer, 0.50f, SpellRole::Heal),
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.50f, SpellRole::Heal),
            G(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.50f, SpellRole::Heal),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy) };
        // Trimmed set (marth): paralyze/fear a stronger foe, armor spell up, magicka potion, Destruction nearest.
        // The spell-will-work level skip (G10) is a BASE rule, not a row.
        constexpr DefaultGambit kWarlockG[] = {
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActCastTarget, 0.0f, SpellRole::Paralyze, kEnemy),
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActCastTarget, 0.0f, SpellRole::Fear, kEnemy),
            G(Vocab::kCondAlways, Vocab::kActCastSelf, 0.0f, SpellRole::Armor),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Destruction, kEnemy) };

        // T3 pure
        constexpr DefaultGambit kGreatKnightG[] = {   // Knight's set + taunt at 3+ foes
            G(Vocab::kCondFoeAttackingAlly, Vocab::kActTaunt),
            G(Vocab::kCondFoeCountAtLeast, Vocab::kActTaunt, 3.0f),
            G(Vocab::kCondFoeAttackingPlayer, Vocab::kActAttack),
            G(Vocab::kCondSelfHpBelow, Vocab::kActDrinkHealthPotion, 0.35f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        // Same as Warrior (10 s cooldowns); no sweep/staggered extras (marth).
        constexpr DefaultGambit kChampionG[] = {
            G(Vocab::kCondFoeBlocking, Vocab::kActPowerAttack, 0.0f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActPowerAttack, 0.0f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeHighestHp, Vocab::kActAttack),
            G(Vocab::kCondSelfSpBelow, Vocab::kActDrinkStaminaPotion, 0.25f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        // Same as Mercenary (marth).
        constexpr DefaultGambit kBlademasterG[] = {
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeIsRanged, Vocab::kActAttack),
            G(Vocab::kCondFoeLowestHp, Vocab::kActAttack),
            G(Vocab::kCondSelfSpBelow, Vocab::kActDrinkStaminaPotion, 0.25f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        // Archer's set WITHOUT the melee swap; Mark a closing melee foe instead (marth).
        constexpr DefaultGambit kRangerG[] = {
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeIsRanged, Vocab::kActAttack),
            G(Vocab::kCondSelfOutOfArrows, Vocab::kActLootArrows, 1.0f),
            Todo(Vocab::kCondFoeAttackingMeMelee, kTodoMark, "H-MARK"),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        constexpr DefaultGambit kMasterThiefG[] = {   // Thief's set, fleeing foes first; sneak always on
            Todo(Vocab::kCondAlways, kTodoSneak, "G1"),
            G(Vocab::kCondFoeFleeing, Vocab::kActAttack),
            G(Vocab::kCondFoeLowestHp, Vocab::kActAttack),
            Todo(Vocab::kCondAlways, kTodoSteal, "G13"),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 150.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        constexpr DefaultGambit kWizardG[] = {        // ward (new design), paralyze stronger foe, Evoker's set
            Todo(kTodoChargeAtMe, Vocab::kActCastSelf, "G11", 0.0f, SpellRole::Ward),
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActCastTarget, 0.0f, SpellRole::Paralyze, kEnemy),
            G(Vocab::kCondFoeWeakFire, Vocab::kActCastTarget, 0.0f, SpellRole::Fire, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Destruction, kEnemy),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f) };
        constexpr DefaultGambit kNecromancerG[] = {   // Conjurer's set + reanimate a corpse
            Todo(kTodoNoSummon, Vocab::kActCastSelf, "G5", 0.0f, SpellRole::Summon),
            Todo(kTodoCorpse, Vocab::kActCastTarget, "G7", 0.0f, SpellRole::Reanimate, kEnemy),
            Todo(kTodoFoeHitsMySummon, Vocab::kActCastTarget, "G6", 0.0f, SpellRole::Attack, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.25f),
            G(Vocab::kCondFoeWithinRange, Vocab::kActEquipMelee, 150.0f) };
        constexpr DefaultGambit kPriestG[] = {        // Healer's set, sun / turn undead above the self-heal
            G(Vocab::kCondPlayerHpBelow, Vocab::kActCastPlayer, 0.50f, SpellRole::Heal),
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.50f, SpellRole::Heal),
            G(Vocab::kCondFoeIsUnholy, Vocab::kActCastTarget, 0.0f, SpellRole::TurnUndead, kEnemy),
            G(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.50f, SpellRole::Heal),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy) };

        // T3 hybrids
        constexpr DefaultGambit kSpellswordG[] = {
            G(Vocab::kCondFoeBeyondRange, Vocab::kActCastTarget, 400.0f, SpellRole::Destruction, kEnemy),
            G(Vocab::kCondFoeWeakFire, Vocab::kActCastTarget, 0.0f, SpellRole::Fire, kEnemy),
            G(Vocab::kCondFoeWithinRange, Vocab::kActAttack, 400.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kSorcererG[] = {
            Todo(kTodoNoSummon, Vocab::kActCastSelf, "G5", 0.0f, SpellRole::Summon),
            G(Vocab::kCondAlways, Vocab::kActCastSelf, 0.0f, SpellRole::BoundWeapon),   // a bound weapon must count as melee for equip authority: G17
            G(Vocab::kCondFoeAny, Vocab::kActAttack) };
        // CHOICE: heal thresholds at the 70% party-healthy mark (marth: "the same 70% party healthy threshold").
        constexpr DefaultGambit kTemplarG[] = {
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.70f, SpellRole::Heal),
            G(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.50f, SpellRole::Heal),
            G(Vocab::kCondFoeAttackingAlly, Vocab::kActTaunt),
            G(Vocab::kCondFoeIsUndead, Vocab::kActAttack),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kNightbladeG[] = {
            G(Vocab::kCondFoeStrongerThanMe, Vocab::kActCastTarget, 0.0f, SpellRole::Fear, kEnemy),
            G(Vocab::kCondFoeFleeing, Vocab::kActAttack),
            G(Vocab::kCondAlways, Vocab::kActCastSelf, 0.0f, SpellRole::Armor),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kWitchhunterG[] = {
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeIsUndead, Vocab::kActAttack),
            G(Vocab::kCondFoeIsDaedra, Vocab::kActAttack),
            G(Vocab::kCondFoeWithinRange, Vocab::kActCastTarget, 250.0f, SpellRole::Paralyze, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        // CHOICE: gambits at the 70% mark like Templar (marth: Scout "gambits at 70%").
        constexpr DefaultGambit kScoutG[] = {
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.70f, SpellRole::Heal),
            G(Vocab::kCondPlayerHpBelow, Vocab::kActCastPlayer, 0.70f, SpellRole::Heal),
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };

        // T4 (parents' sets merged by priority)
        constexpr DefaultGambit kArchMageG[] = {      // ward vs charging caster, summon when outnumbered, heal < 70, element vs weakness, nearest, potion
            Todo(kTodoChargeAtMe, Vocab::kActCastSelf, "G11", 0.0f, SpellRole::Ward),
            G(Vocab::kCondFoeCountAtLeast, Vocab::kActCastSelf, 3.0f, SpellRole::Summon),
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.70f, SpellRole::Heal),
            G(Vocab::kCondFoeWeakFire, Vocab::kActCastTarget, 0.0f, SpellRole::Fire, kEnemy),
            G(Vocab::kCondFoeAny, Vocab::kActCastTarget, 0.0f, SpellRole::Attack, kEnemy),
            G(Vocab::kCondSelfMpBelow, Vocab::kActDrinkMagickaPotion, 0.20f) };
        // CHOICE: Champion mode rows only; the flurry-vs-many mode switch is card C8/C9 work.
        constexpr DefaultGambit kLivingLegendG[] = {
            G(Vocab::kCondFoeBlocking, Vocab::kActPowerAttack, 0.0f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeHighestHp, Vocab::kActAttack),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kNightingaleG[] = {
            Todo(Vocab::kCondAlways, kTodoSneak, "G1"),
            G(Vocab::kCondFoeIsCaster, Vocab::kActAttack),
            G(Vocab::kCondFoeFleeing, Vocab::kActEquipMelee),
            Todo(Vocab::kCondAlways, kTodoSteal, "G13"),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        constexpr DefaultGambit kListenerG[] = {
            Todo(Vocab::kCondAlways, kTodoSneak, "G1"),
            G(Vocab::kCondFoeLowestHp, Vocab::kActAttack),
            G(Vocab::kCondFoeHpBelow, Vocab::kActPowerAttack, 0.25f, SpellRole::None, kSelf, 10.0f),
            G(Vocab::kCondFoeCountAtLeast, Vocab::kActCastTarget, 3.0f, SpellRole::Fear, kEnemy),
            Todo(Vocab::kCondAlways, kTodoSteal, "G13"),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kEbonyWarriorG[] = {
            G(Vocab::kCondFoeAttackingAlly, Vocab::kActTaunt),
            G(Vocab::kCondFoeCountAtLeast, Vocab::kActTaunt, 2.0f),
            G(Vocab::kCondSelfHpBelow, Vocab::kActCastSelf, 0.40f, SpellRole::Heal),
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.70f, SpellRole::Heal),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kGrandBattlemageG[] = {   // spell before closing, melee inside 400
            G(Vocab::kCondFoeBeyondRange, Vocab::kActCastTarget, 400.0f, SpellRole::Destruction, kEnemy),
            G(Vocab::kCondFoeWeakFire, Vocab::kActCastTarget, 0.0f, SpellRole::Fire, kEnemy),
            G(Vocab::kCondFoeWithinRange, Vocab::kActAttack, 400.0f),
            G(Vocab::kCondFoeAny, Vocab::kActEquipMelee) };
        constexpr DefaultGambit kJaqspurG[] = {
            G(Vocab::kCondFoeHighestLevel, Vocab::kActAttack),
            G(Vocab::kCondFoeIsRanged, Vocab::kActAttack),
            G(Vocab::kCondSelfOutOfArrows, Vocab::kActLootArrows, 1.0f),
            Todo(Vocab::kCondFoeAttackingMeMelee, kTodoMark, "H-MARK"),
            G(Vocab::kCondFoeAny, Vocab::kActEquipRanged) };
        constexpr DefaultGambit kSaintG[] = {
            G(Vocab::kCondAllyDowned, Vocab::kActCastTarget, 0.0f, SpellRole::Heal),
            G(Vocab::kCondFoeIsUnholy, Vocab::kActAttack),
            G(Vocab::kCondAllyHpBelow, Vocab::kActCastTarget, 0.70f, SpellRole::Heal),
            G(Vocab::kCondFoeAttackingAlly, Vocab::kActTaunt) };

        // T5
        constexpr DefaultGambit kDragonbornG[] = {
            Todo(Vocab::kCondFoeCountAtLeast, kTodoShout, "G8", 3.0f),   // Unrelenting Force at 3+ foes
            G(Vocab::kCondFoeAny, Vocab::kActAttack) };

        // G12 (cond.party_hp_pct_above) has no row: the "party healthy" mark is carried by ClassDef::partyHealthyPct.

        // ── the factory ──────────────────────────────────────────────────────────────────────────────
        template <std::size_t N>
        constexpr ClassDef Mk(Id a_id, std::string_view a_slug, std::string_view a_name, std::uint8_t a_tier, Line a_line,
                              RootT1 a_root, UnlockRule a_unlock, std::uint16_t a_weapons, const DefaultGambit (&a_g)[N],
                              std::string_view a_passive, float a_max, float a_min, std::uint16_t a_flags,
                              GoIn a_goIn = GoIn::None, std::uint8_t a_pct = 0) {
            return { a_id, a_slug, a_name, a_tier, a_line, a_root, a_unlock,
                     static_cast<Mask>(a_unlock.allOf | a_unlock.poolA | a_unlock.poolB),
                     a_weapons, a_g, static_cast<std::uint8_t>(N), a_passive, a_max, a_min, a_flags, a_goIn, a_pct };
        }

        constexpr float U = kShareUnset;   // ch.18 share not declared
        using namespace Flag;
        constexpr std::uint16_t kDisSpell = kDisengage | kSpellRange;   // Thief-and-mage family: disengage + spell range
        // RootT1 ordinals = State.h combatClassOverride (FROZEN; pinned against CombatStyle::Stance in Classes.cpp)
        constexpr RootT1 kRootMelee = 1, kRootRanged = 2, kRootMage = 3;

        // ch.18 mixes (marth decisions): Warrior line floor 0.4, Knight cap 0.25, Great Knight 0.20,
        // Mercenary and Blademaster 0.15, Listener 0.25. Everything else is not declared.
        //
        // Weapons: T1 vanilla (unrestricted). Shields only Knight, Great Knight, Templar, Ebony Warrior.
        // Passive ids are one per class ("passive.<slug>"); the passive CONTENT is card C6/C7.
        // CHOICE: a hybrid / T4 class's RootT1 is its WEAPON side (what drives stance), Dragonborn has none.
        constexpr std::array<ClassDef, kClassCount> kTableC = { {
            // ── tier 1 ──
            Mk(Id::Melee,  "melee",  "Melee",  1, Line::Melee,  kRootMelee,  None(), kUnrestricted, kMeleeG,  "passive.melee",  U, U, 0),
            Mk(Id::Ranged, "ranged", "Ranged", 1, Line::Ranged, kRootRanged, None(), kUnrestricted, kRangedG, "passive.ranged", U, U, 0),
            Mk(Id::Mage,   "mage",   "Mage",   1, Line::Mage,   kRootMage,   None(), kUnrestricted, kMageG,   "passive.mage",   U, U, kSpellRange),
            // ── tier 2 ──
            Mk(Id::Knight,    "knight",    "Knight",    2, Line::Melee,  kRootMelee,  Base(Id::Melee, kT1),  kMelee1H2H | kShield, kKnightG,    "passive.knight",    0.25f, U,    kShortLeash),
            Mk(Id::Warrior,   "warrior",   "Warrior",   2, Line::Melee,  kRootMelee,  Base(Id::Melee, kT1),  kMelee1H2H,           kWarriorG,   "passive.warrior",   U,     0.40f, 0),
            Mk(Id::Mercenary, "mercenary", "Mercenary", 2, Line::Melee,  kRootMelee,  Base(Id::Melee, kT1),  kMelee1H2H,           kMercenaryG, "passive.mercenary", 0.15f, U,    0),
            Mk(Id::Archer,    "archer",    "Archer",    2, Line::Ranged, kRootRanged, Base(Id::Ranged, kT1), kArcherSet,           kArcherG,    "passive.archer",    U,     U,    0),
            Mk(Id::Thief,     "thief",     "Thief",     2, Line::Ranged, kRootRanged, Base(Id::Ranged, kT1), kThiefSet,            kThiefG,     "passive.thief",     U,     U,    kDisengage | kSneakAlways),
            Mk(Id::Evoker,    "evoker",    "Evoker",    2, Line::Mage,   kRootMage,   Base(Id::Mage, kT1),   kMageSet,             kEvokerG,    "passive.evoker",    U,     U,    kDisSpell),
            Mk(Id::Conjurer,  "conjurer",  "Conjurer",  2, Line::Mage,   kRootMage,   Base(Id::Mage, kT1),   kMageSet,             kConjurerG,  "passive.conjurer",  U,     U,    kDisSpell),
            Mk(Id::Healer,    "healer",    "Healer",    2, Line::Mage,   kRootMage,   Base(Id::Mage, kT1),   kMageSet,             kHealerG,    "passive.healer",    U,     U,    kDisSpell | kNearPlayer),
            Mk(Id::Warlock,   "warlock",   "Warlock",   2, Line::Mage,   kRootMage,   Base(Id::Mage, kT1),   kMageSet,             kWarlockG,   "passive.warlock",   U,     U,    kDisSpell),
            // ── tier 3, pure ──
            Mk(Id::GreatKnight,  "great-knight", "Great Knight", 3, Line::Melee,  kRootMelee,  Base(Id::Knight, kT2),    kMelee1H2H | kShield, kGreatKnightG,  "passive.great-knight", 0.20f, U,    kShortLeash),
            Mk(Id::Champion,     "champion",     "Champion",     3, Line::Melee,  kRootMelee,  Base(Id::Warrior, kT2),   kMelee1H2H,           kChampionG,     "passive.champion",     U,     0.40f, 0),
            Mk(Id::Blademaster,  "blademaster",  "Blademaster",  3, Line::Melee,  kRootMelee,  Base(Id::Mercenary, kT2), kMelee1H2H,           kBlademasterG,  "passive.blademaster",  0.15f, U,    0),
            Mk(Id::Ranger,       "ranger",       "Ranger",       3, Line::Ranged, kRootRanged, Base(Id::Archer, kT2),    kArcherSet,           kRangerG,       "passive.ranger",       U,     U,    0),
            Mk(Id::MasterThief,  "master-thief", "Master Thief", 3, Line::Ranged, kRootRanged, Base(Id::Thief, kT2),     kThiefSet,            kMasterThiefG,  "passive.master-thief", U,     U,    kDisengage | kSneakAlways),
            Mk(Id::Wizard,       "wizard",       "Wizard",       3, Line::Mage,   kRootMage,   Req(Ms(Id::Evoker, Id::Warlock)), kMageSet,    kWizardG,       "passive.wizard",       U,     U,    kDisSpell),
            Mk(Id::Necromancer,  "necromancer",  "Necromancer",  3, Line::Mage,   kRootMage,   Base(Id::Conjurer, kT2),  kMageSet,             kNecromancerG,  "passive.necromancer",  U,     U,    kDisSpell),
            Mk(Id::Priest,       "priest",       "Priest",       3, Line::Mage,   kRootMage,   Base(Id::Healer, kT2),    kMageSet,             kPriestG,       "passive.priest",       U,     U,    kDisSpell | kNearPlayer),
            // ── tier 3, hybrids (armor is free inside every hybrid) ──
            Mk(Id::Spellsword,  "spellsword",  "Spellsword",  3, Line::Hybrid, kRootMelee,  Pair(kMeleeT2, Ms(Id::Evoker)),   kMelee1H2H,           kSpellswordG,  "passive.spellsword",  U, U, kDisSpell, GoIn::BiasHealthMagickaRange),
            Mk(Id::Sorcerer,    "sorcerer",    "Sorcerer",    3, Line::Hybrid, kRootMelee,  Pair(kMeleeT2, Ms(Id::Conjurer)), kMelee1H2H,           kSorcererG,    "passive.sorcerer",    U, U, kDisSpell, GoIn::FullSummonsHighHealth),
            Mk(Id::Templar,     "templar",     "Templar",     3, Line::Hybrid, kRootMelee,  Pair(kMeleeT2, Ms(Id::Healer)),   kMelee1H2H | kShield, kTemplarG,     "passive.templar",     U, U, kDisSpell, GoIn::PartyHealthyOrNoMagicka, 70),
            Mk(Id::Nightblade,  "nightblade",  "Nightblade",  3, Line::Hybrid, kRootMelee,  Pair(kMeleeT2, Ms(Id::Warlock)),  kMelee1H2H,           kNightbladeG,  "passive.nightblade",  U, U, kDisSpell, GoIn::ControlledFoesOnly),
            Mk(Id::Witchhunter, "witchhunter", "Witchhunter", 3, Line::Hybrid, kRootRanged, Pair(Ms(Id::Archer), Ms(Id::Warlock)), kArcherSet,     kWitchhunterG, "passive.witchhunter", U, U, kDisSpell, GoIn::Never),
            Mk(Id::Scout,       "scout",       "Scout",       3, Line::Hybrid, kRootRanged, Pair(Ms(Id::Archer), Ms(Id::Healer)),  kArcherSet,      kScoutG,       "passive.scout",       U, U, kDisSpell, GoIn::None, 70),
            // ── tier 4 ──
            Mk(Id::ArchMage,        "arch-mage",        "Arch-Mage",        4, Line::Mage,   kRootMage,   NOf(Ms(Id::Wizard, Id::Necromancer, Id::Priest), 2),     kMageSet,             kArchMageG,        "passive.arch-mage",        U,     U, kDisSpell),
            Mk(Id::LivingLegend,    "living-legend",    "Living Legend",    4, Line::Melee,  kRootMelee,  Req(Ms(Id::Blademaster, Id::Champion)),                   kMelee1H2H,           kLivingLegendG,    "passive.living-legend",    U,     0.40f, 0),
            Mk(Id::Nightingale,     "nightingale",      "Nightingale",      4, Line::Ranged, kRootRanged, Req(Ms(Id::MasterThief, Id::Witchhunter)),                kThiefSet,            kNightingaleG,     "passive.nightingale",      U,     U, kDisSpell | kSneakAlways),
            Mk(Id::Listener,        "listener",         "Listener",         4, Line::Ranged, kRootRanged, Req(Ms(Id::MasterThief, Id::Nightblade)),                 kThiefSet,            kListenerG,        "passive.listener",         0.25f, U, kDisSpell | kSneakAlways),
            Mk(Id::EbonyWarrior,    "ebony-warrior",    "Ebony Warrior",    4, Line::Melee,  kRootMelee,  Req(Ms(Id::GreatKnight, Id::Templar)),                    kMelee1H2H | kShield, kEbonyWarriorG,    "passive.ebony-warrior",    0.20f, U, kShortLeash | kDisengage, GoIn::PartyHealthyOrNoMagicka, 70),
            Mk(Id::GrandBattlemage, "grand-battlemage", "Grand Battlemage", 4, Line::Hybrid, kRootMelee,  NOf(Ms(Id::Spellsword, Id::Sorcerer, Id::Nightblade), 2),   kMelee1H2H,           kGrandBattlemageG, "passive.grand-battlemage", U,     U, kDisSpell),
            Mk(Id::Jaqspur,         "jaqspur",          "Jaqspur",          4, Line::Ranged, kRootRanged, NOf(Ms(Id::Ranger, Id::Scout, Id::Witchhunter), 2),       kArcherSet,           kJaqspurG,         "passive.jaqspur",          U,     U, kDisSpell),
            Mk(Id::Saint,           "saint",            "Saint",            4, Line::Mage,   kRootMage,   NOf(Ms(Id::Priest, Id::Templar, Id::Scout), 2),           kMageSet,             kSaintG,           "passive.saint",            U,     U, kDisSpell | kNearPlayer, GoIn::None, 70),
            // ── tier 5, hidden: 3 distinct tier-4 classes, no class limits, no fixed root ──
            Mk(Id::Dragonborn, "dragonborn", "Dragonborn", 5, Line::Hybrid, 0, NOf(kT4, 3), kUnrestricted, kDragonbornG, "passive.dragonborn", U, U, 0),
        } };

        // ── compile-time structure checks (the runtime self-check in Classes.cpp logs the counts) ───────
        constexpr bool TableIsConsistent() {
            for (std::size_t i = 0; i < kClassCount; ++i) {
                const auto& d = kTableC[i];
                if (static_cast<std::size_t>(d.id) != i) return false;                  // id == index (bit index ABI)
                if (d.tier < 1 || d.tier > 5) return false;
                if (d.slug.empty() || d.gambitCount == 0) return false;
                if (d.tier == 1 && d.rootT1 != i + 1) return false;                     // T1 roots == combatClassOverride 1..3
                if (d.tier == 1 && d.parentMask != 0) return false;
                if (d.tier > 1 && d.parentMask == 0) return false;
                // parents are all of the tier immediately below, and never include the class itself
                for (std::size_t p = 0; p < kClassCount; ++p)
                    if ((d.parentMask >> p) & 1u) {
                        if (p == i) return false;
                        if (kTableC[p].tier + 1 != d.tier) return false;
                    }
            }
            return true;
        }
        static_assert(TableIsConsistent(), "classes/ClassTable.cpp: the class table violates its structural rules");
    }

    const std::array<ClassDef, kClassCount> kTable = kTableC;
}
