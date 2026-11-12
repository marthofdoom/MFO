#pragma once
// iaus_sim/Data.h -- the list's balance as the simulator reads it (sim v2). HOST-ONLY, never part of the DLL.
//
// One file per list: ~/.cache/mfo-list-balance/<list>.sim.json (tools/list_balance/compile_world.py). It is TEST
// INPUT ONLY. With no list the BUILT-IN set is used: Skyrim.esm's own game-setting values plus a small synthetic
// roster (fixtures written by hand for the simulator; never game data and never read by native/).
//
// Every formula below names its source and proof level in Docs/research/2026-11/iaus-sim-v2.md ("Model table").
#include "iaus/Core.h"
#include "iaus/Snapshot.h"
#include "iaus_sim/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Sim {

    using namespace MFO::Iaus;

    // ── T2 BATCH S (iaus-sim-v2.md section 31): every T2 mechanism the manifest's Sim column listed as OPEN. Each is ONE switch,
    // ON by default. With every switch off (`--t2 off`, which includes `--feet-stop`) the lab is the one of section 30, draw for draw
    // (78.11 at g3 / 76.69 at --stealth off for the Beta 1 set). A switch never draws from the fight's random stream while it is off.
    struct T2 {
        bool feet = true;        // Q-SIM-5: a weapon swap, a potion and a cast do not stop his feet (false = --feet-stop, the lab before)
        bool c8 = true;          // card C8: ch.18 share window (ceiling / floor), ch.23 leash, the engine's own power attacks for a class with no rule
        bool dodge = true;       // Q-melee-7: the class style's Avoid Threat rolls a side step against an incoming projectile
        bool aversion = true;    // card H2: ch.32 aversion (Conjurer, Healer 0.25, equipped class)
        bool aim = true;         // card H5: ch.29 miss budget (a corrected shot when the window's misses reach the budget)
        bool block = true;       // ch.31 held guard: ClassGate::block from the class's Block row, 2.5 s held, the held-guard gate
        bool range = true;       // card H4: the distance a mage fights at follows the spell in his hand; the mage back-off
        bool backoff = true;     // Q-T2-3: the Mercenary's back-off (two or more foes press him)
        bool step = true;        // gap G18: the hidden step
        bool c13built = true;    // card C13 AS BUILT (near-inert, Q112): no engine lever, so no free entry. false (--c13 lever) = the H10 guarantee the lab carried before
        bool unweighted = true;  // Q91: a spell with no effect IAUS can value is dropped (Prune::Drop::Unweighted)
        bool reanimate = true;   // card G7: reanimate spells are kept (table rows cast them at a corpse), corpses are raised
        bool dnd = false;        // --disengage-dnd (marth 11-12, NOT built in the game): a disengage makes him no valid target for 6 s
        void AllOff() { feet = c8 = dodge = aversion = aim = block = range = backoff = step = c13built = unweighted = reanimate = false; }
    };
    inline T2 g_t2;
    inline float g_aimMiss = 0.12f;       // --aim-miss: the lab's chance that an UNCORRECTED aimed shot misses a moving target (MODEL; TM::aimMiss before)
    inline float g_dodgeFlight = 0.2f;    // MODEL: a side step clears a projectile only when it flies at least this long (a reaction time; --dodge-flight, sensitivity in section 31.4)
    inline float g_aimScatter = 0.0f;     // --aim-scatter P: every UNCORRECTED follower shot also misses with this chance, moving target or not. The engine scatters by
                                          // aimController+0xA0 x fCombatAimProjectileRandomOffset (class-m0-re.md); how often that misses is not measured, so the default is none
    inline bool  g_mixPartial = false;    // --mix-partial: the ch.18 share is read on a window that is not full yet. Harbinger's Share() answers "no share" (no deny) until 8
                                          // attacks fill the window (review of 0880667, F3), so the default waits for 8. The switch measures the other reading
    inline bool  g_noG3 = false;          // the disengage report's 'none' mode: card G3's hide run and flee are never asked for (every other road stands)
    inline bool  g_fillSwing = true;      // --no-fill-swing: Combatant::swinging is not filled ([Debug] bIausFillSwing ships OFF; without it no Block candidate exists)

    // Actor values (CommonLib RE::ActorValue, the same numbers the records store).
    inline constexpr int kAvOneHanded = 6, kAvTwoHanded = 7, kAvMarksman = 8, kAvBlock = 9, kAvHeavy = 11, kAvLight = 12,
                         kAvAlteration = 18, kAvConjuration = 19, kAvDestruction = 20, kAvIllusion = 21, kAvRestoration = 22,
                         kAvHealth = 24, kAvMagicka = 25, kAvStamina = 26, kAvDamageResist = 39, kAvPoisonResist = 40,
                         kAvResistFire = 41, kAvResistShock = 42, kAvResistFrost = 43, kAvResistMagic = 44, kAvWardPower = 63;
    // MGEF archetypes (RE::EffectArchetypes).
    inline constexpr int kArValueMod = 0, kArScript = 1, kArAbsorb = 4, kArDualValueMod = 5, kArCalm = 6, kArDemoralize = 7,
                         kArFrenzy = 8, kArSummon = 18, kArParalysis = 21, kArReanimate = 22, kArTurnUndead = 24,
                         kArStagger = 33, kArPeakValueMod = 34, kArCloak = 35;

    struct Rng {
        std::mt19937_64 g;
        float U(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); }
        int   I(int a, int b) { return b <= a ? a : std::uniform_int_distribution<int>(a, b)(g); }
        bool  P(float p) { return U(0.0f, 1.0f) < p; }
    };

    struct Weapon {
        std::string name;
        float dmg = 0, speed = 1, reach = 1, stagger = 0, crit = 0, weight = 8;
        int   anim = 0;     // 0 fist, 1 sword, 2 dagger, 3 axe, 4 mace, 5 greatsword, 6 battleaxe / hammer, 7 bow, 8 staff, 9 crossbow
        int   skill = kAvOneHanded;
        bool  silver = false;
        [[nodiscard]] bool Has() const { return dmg > 0; }
        [[nodiscard]] bool TwoH() const { return anim == 5 || anim == 6; }
        [[nodiscard]] bool Ranged() const { return anim == 7 || anim == 9; }
    };

    // One spell as the world resolves it. `e` is the SpellBook entry IAUS sees (the classification mirrors
    // Prune::ClassifyNature's precedence, as sim v1 did).
    struct SimSpell {
        SpellEntry e;
        int   school = -1;          // skill AV of the costliest effect
        int   skillReq = 0;         // that effect's minimum skill (the spell's tier)
        float cost0 = 0;            // record cost before the caster's skill
        float castSec = 0.5f;       // charge time before release (concentration: the stream's length)
        float dmg = 0;              // health damage at impact
        float dot = 0, dotDur = 0;  // health damage per second, for dotDur
        float mpDmg = 0, spDmg = 0; // magicka / stamina damage at impact
        float heal = 0, hot = 0, hotDur = 0;
        float armor = 0, buffDur = 0;      // +damage resist (armor spells)
        float resistBuff = 0; int resistBuffAv = -1;
        bool  ctrl = false, ctrlPar = false, ctrlCalm = false, ctrlFlee = false, ctrlFrenzy = false, turnUndead = false;
        float ctrlDur = 0, ctrlMaxLevel = 1e9f;   // illusion: the magnitude is the highest level affected
        bool  stagger = false;
        bool  reanimate = false, summon = false;
        float summonDur = 60;
        float recovery = 0;         // shout recovery (its cooldown)
        bool  healOther = false;    // a heal that is not self-only
        std::string element;        // "fire" / "frost" / "shock" / ""
    };

    struct Style {   // CSTY (CSGD / CSME / CSCR / CSLR / DATA)
        float off = 1, def = 1, groupOff = 1, wMelee = 1, wMagic = 1, wRanged = 1, wShout = 1, wUnarmed = 1, wStaff = 1, avoidThreat = 0.2f;
        float atkStaggered = 1, powStaggered = 1, powBlocking = 1, bash = 1, bashRecoil = 1, bashAtk = 1, bashPow = 1, special = 1;
        float circle = 0.2f, fallback = 0.2f, flankDist = 0.2f, stalk = 0.2f, strafe = 0.2f;
        bool  dueling = false, flanking = false, dualWield = false;
    };

    struct ActorSpec {
        std::string name, race;
        int   level = 1;
        float hp = 100, mp = 50, sp = 100, hpR = 0.7f, mpR = 3, spR = 5;
        std::array<float, 18> skill{};   // index = AV - 6
        Weapon melee, ranged;
        bool  staff = false, shield = false;
        float ammoDmg = 0, unarmed = 4, unarmedReach = 96, atkDmgMult = 1;
        float ar = 0; int pieces = 0;
        float rFire = 0, rFrost = 0, rShock = 0, rMagic = 0, rPoison = 0;
        std::vector<int> spells;         // into Data::foeSpells (foes) -- followers carry their own SimSpell list
        Style st; bool hasStyle = false;
        int   conf = 2, aggr = 1;
        float run = 370, mass = 1;
        bool  undead = false, dragon = false, daedra = false, animal = false, creature = false, immunePar = false;
        float atkMult = 1, powMult = 1;  // perk multipliers (ModAttackDamage / ModPowerAttackDamage)
        [[nodiscard]] float Skill(int av) const { return av >= 6 && av < 24 ? skill[static_cast<std::size_t>(av - 6)] : 15.0f; }
        [[nodiscard]] float Resist(int av) const {
            return av == kAvResistFire ? rFire : av == kAvResistFrost ? rFrost : av == kAvResistShock ? rShock : av == kAvResistMagic ? rMagic : av == kAvPoisonResist ? rPoison : 0.0f; }
    };

    struct EncMember { int spec; float dx, dy; };
    struct Encounter { std::string cell; bool interior = false; std::vector<std::vector<EncMember>> byBucket; };

    struct FRace { std::string edid; float hp = 100, mp = 100, sp = 100, hpR = 0.7f, mpR = 3, spR = 5, unarmed = 4, run = 370, mass = 1; std::vector<std::pair<int, int>> boosts; };
    struct FClass { std::array<float, 18> skillW{}; float hW = 1, mW = 1, sW = 1; std::string edid; };
    using Thr = std::vector<std::pair<float, float>>;   // (skill threshold, value), ascending

    // The game settings the world model reads, each with the value Skyrim.esm ships (the built-in fallback).
    struct Rules {
        float dmgSkillMin = 1, dmgSkillMax = 3, dmgWeaponMult = 1;            // fDamageSkillMin/Max (NPC), fDamageWeaponMult
        float armorScale = 0.12f, armorMax = 80, armorBasePiece = 0.03f;      // fArmorScalingFactor, fMaxArmorRating, fArmorBaseFactor
        float armorSkillLo = 1, armorSkillHi = 2.5f;                          // fArmorRatingBase, fArmorRatingMax (NPC)
        float powBonus = 1, powStaBase = 20, powStaMult = 1, powStaPenalty = 2;   // fPowerAttackDefaultBonus, fStaminaAttackWeaponBase/Mult, fPowerAttackStaminaPenalty
        float combatMp = 0.33f, combatSp = 0.35f, combatHp = 0.7f;            // fCombatMagicka/StaminaRegenRateMult; health: AV CombatHealthRegenMult (UNPROVEN)
        float delayHp = 0.5f, delayMp = 0.5f, delaySp = 0.5f;                 // fDamaged*RegenDelay
        float costBase = 0.005f, costMult = 0.5f, costScale = 0.5f;           // fMagicCasterSkillCostBase/Mult, fMagicSkillCostScale (NPC)
        float bowDraw = 1.6667f;                                              // fBowDrawTime
        float diffToPC = 1, diffByPC = 1;                                     // fDiffMultHPToPCN, fDiffMultHPByPCN
        float hpPerLevel = 5, hmsPerLevel = 10, skillPerLevel = 8, skillStart = 15;   // fNPCHealthLevelBonus, iAVDhmsLevelUp, iAVDSkillsLevelUp, iAVDSkillStart
        float maxResist = 85;                                                 // fPlayerMaxResistance (also bounds NPC resist in practice)
        float combatDistance = 128;                                           // fCombatDistance (reach unit)
        float blockPowMult = 0.5f;                                            // fBlockPowerAttackMult
        float rangedMin = 256, rangedDist = 1024;                             // fCombatRangedMinimumRange, fCombatRangedDistance
        float bleedoutDefault = 0.15f;                                        // fBleedoutDefault
    };

    // Chocolate Poise as the list configures it (tools/list_balance/extract_combat_mods.py; formula = the mod's own source,
    // doodlum/skyrim-poise PoiseAV.cpp / HitEventHandler.cpp). on = false: the vanilla stagger MODEL (row 20).
    struct PoiseRules {
        bool on = false; int staggerMode = 1;
        float baseMult = 50, armorMult = 0.2f, regen = 0.045f, bash = 1.5f, bow = 1, creature = 2.5f, melee = 1, unarmed = 3, toPC = 1, toNPC = 1, weight = 2, magicHealth = 0;
        std::map<std::string, float> races;        // race EditorID -> the multiplier that replaces the race's mass
        float weap[10] = {}; bool hasWeap[10] = {}; // by Weapon::anim: the "Weapons.Damage" base the weight is lerped from
        float shield = 0; bool hasShield = false;
    };
    // Dynamic Follower Scaling (FollowerScaling.dll): every weapon hit of an actor in CurrentFollowerFaction is scaled by the
    // number of LOADED actors in that faction. mode 1: (1 - reduction)^(n - 1); mode 2: 1 / n.
    struct FollowerScaling { bool on = false; int mode = 1; float reduction = 0; };
    // One stealth perk row (tools/list_balance/compile_world.py "STEALTH perk tables"): a skill gate, a value, its ranked chain.
    struct StealthRow { int av = 15; float thr = 0, v = 1; std::string chain; };
    struct Data {
        std::string name = "builtin";
        PoiseRules poise; FollowerScaling dfs;
        std::map<std::string, double> gmst;
        Rules rules;
        std::vector<ActorSpec> specs;
        std::vector<SimSpell> foeSpells;
        std::vector<Encounter> encounters;
        std::vector<int> buckets;
        std::vector<SimSpell> spells;   // the castable pool a follower's book is drawn from (taught by a BOOK)
        std::vector<SimSpell> scrolls;  // SCRL records (the list's scrolls a follower may carry)
        std::vector<SimSpell> reanimSpells;   // card G7: the list's Reanimate spells (table rows only; AddPoolSpell)
        int keptReanim = 0, dropUnweighted = 0;
        std::vector<FRace> races;
        FClass cls[3];                  // melee, ranged, mage
        std::vector<Weapon> wOneHand, wDagger, wTwoHand, wBow, wCrossbow;
        std::map<std::string, std::vector<float>> armor;   // "heavy_body" ... -> ladder of base ratings
        std::vector<float> arrows, bolts;
        std::vector<std::pair<float, float>> pots[3];      // health / magicka / stamina: (magnitude, duration)
        std::map<std::string, Thr> pAttack, pPower, pSpellMag, pSpellCost, pArmor;
        std::map<std::string, std::vector<StealthRow>> sneakAtk;          // EP 18 ModSneakAttackMult rows by weapon kind
        std::vector<StealthRow> sneakSkillRows, sneakNoiseRows;            // EP 57 ModDetectionSneakSkill (add), EP 48 ModDetectionMovement (multiply)
        int natures[7] = {};
        int rawSpells = 0, taughtSpells = 0, dropPlace = 0, dropNotCast = 0, dropReanim = 0, dropUnknown = 0;
        std::map<std::string, std::vector<int>> roles;     // "draugr", "troll" ... -> spec indices (scenarios)
        [[nodiscard]] double G(const char* k, double d) const { auto i = gmst.find(k); return i == gmst.end() ? d : i->second; }
    };

    [[nodiscard]] inline float PerkBest(const std::map<std::string, Thr>& t, const std::string& key, float skill, float none = 1.0f) {
        auto i = t.find(key); if (i == t.end()) return none;
        float v = none; bool any = false;
        for (const auto& p : i->second) if (p.first <= skill && (!any || (none >= 1.0f ? p.second > v : p.second < v))) { v = p.second; any = true; }
        return v;
    }

    // ── stealth (the class playstyle lab; every row is in iaus-classes.md "Stealth model") ──
    // The best row of every chain the actor's skill reaches, combined over the chains (mul: a product, else a sum).
    [[nodiscard]] inline float StealthCombine(const std::vector<StealthRow>& rows, const ActorSpec& a, bool mul, bool lowBest = false) {
        std::map<std::string, float> best;
        for (const StealthRow& r : rows) { if (a.Skill(r.av) + 0.01f < r.thr) continue; auto it = best.find(r.chain);
            if (it == best.end()) best[r.chain] = r.v; else it->second = lowBest ? std::min(it->second, r.v) : std::max(it->second, r.v); }
        float o = mul ? 1.0f : 0.0f; for (const auto& kv : best) o = mul ? o * kv.second : o + kv.second; return o;
    }
    [[nodiscard]] inline const char* SneakKind(const Weapon& w) { return !w.Has() ? "hand" : w.anim == 2 ? "dagger" : (w.anim == 1 || w.anim == 3 || w.anim == 4) ? "oneHand" : w.TwoH() ? "twoHand" : w.anim == 7 ? "bow" : w.anim == 9 ? "crossbow" : "hand"; }
    // The engine's sneak attack multiplier of one weapon. PROVEN (read, 1.6.1170): AE 44016 multiplies the hit by AE 18100's value, a
    // game-setting table indexed by the weapon's animation type (fCombatSneak*Mult), then by perk entry point 0x12 ModSneakAttackMult.
    // MODEL: which perks the follower holds (every playable chain his skill reaches, as the other perk tables assume). A chain that
    // has a row for a named weapon kind is not also read through its unconditioned ("all") row.
    [[nodiscard]] inline float SneakAttackMult(const Data& D, const ActorSpec& a, const Weapon& w) {
        static const char* g[10] = { "fCombatSneakHandMult", "fCombatSneak1HSwordMult", "fCombatSneak1HDaggerMult", "fCombatSneak1HAxeMult", "fCombatSneak1HMaceMult", "fCombatSneak2HSwordMult", "fCombatSneak2HAxeMult", "fCombatSneakBowMult", "fCombatSneakStaffMult", "fCombatSneakCrossbowMult" };
        static const double dv[10] = { 2, 3, 3, 3, 3, 2, 2, 2, 1, 2 };
        const int an = w.Has() ? std::clamp(w.anim, 0, 9) : 0;
        std::vector<StealthRow> rows; std::set<std::string> named;
        for (const auto& kv : D.sneakAtk) if (kv.first != "all") for (const StealthRow& r : kv.second) named.insert(r.chain);
        if (auto i = D.sneakAtk.find(SneakKind(w)); i != D.sneakAtk.end()) rows = i->second;
        if (auto i = D.sneakAtk.find("all"); i != D.sneakAtk.end()) for (const StealthRow& r : i->second) if (!named.count(r.chain)) rows.push_back(r);
        return static_cast<float>(D.G(g[an], dv[an])) * StealthCombine(rows, a, true);
    }

    // ── combat formulas ─────────────────────────────────────────────────────────────────────────────────
    // Weapon damage before the target's armor: (weapon + ammo) x fDamageWeaponMult x lerp(fDamageSkillMin, fDamageSkillMax,
    // skill / 100) x ModAttackDamage perks; a power attack x (1 + fPowerAttackDefaultBonus) x ModPowerAttackDamage perks.
    [[nodiscard]] inline float WeaponDamage(const Rules& r, const ActorSpec& a, const Weapon& w, bool power) {
        float d;
        if (w.Has()) d = (w.dmg + (w.Ranged() ? a.ammoDmg : 0.0f)) * r.dmgWeaponMult * (r.dmgSkillMin + (r.dmgSkillMax - r.dmgSkillMin) * std::clamp(a.Skill(w.skill), 0.0f, 100.0f) / 100.0f);
        else d = a.unarmed * a.atkDmgMult;
        d *= a.atkMult;
        if (power) d *= (1.0f + r.powBonus) * a.powMult;
        return d;
    }
    // Physical mitigation: min(fMaxArmorRating, rating x fArmorScalingFactor + worn pieces x fArmorBaseFactor x 100) percent.
    [[nodiscard]] inline float ArmorMitigation(const Rules& r, float rating, int pieces) {
        return std::clamp((rating * r.armorScale + static_cast<float>(pieces) * r.armorBasePiece * 100.0f), 0.0f, r.armorMax) / 100.0f;
    }
    // Magicka cost for an NPC caster: cost x max(0, 1 - fMagicCasterSkillCostMult x (fMagicCasterSkillCostBase x skill)^fMagicSkillCostScale).
    // The SHAPE is UNPROVEN (it reproduces the documented player curve with the player's settings); see the doc.
    [[nodiscard]] inline float SkillCostMult(const Rules& r, float skill) {
        return std::max(0.0f, 1.0f - r.costMult * std::pow(std::max(0.0f, r.costBase * skill), r.costScale));
    }
    [[nodiscard]] inline float PowerStamina(const Rules& r, const Weapon& w) { return (r.powStaBase + w.weight * r.powStaMult) * r.powStaPenalty; }

    // ── spell classification (one reader for the follower pool and the foes' spells) ────────────────────
    inline void Classify(const J& sp, SimSpell& out) {
        const auto& fx = sp["effects"].a; SpellEntry& e = out.e;
        bool summon = false, reanimate = false, ward = false, control = false, cloak = false, hostileAny = false, heal = false, weighable = false;
        double autoCost = 0, topCost = -1, topHostCost = -1; int topArch = -1, resist = -1;
        const std::string& dl = sp["delivery"].s; const bool self = dl == "Self";
        const bool conc = sp["castType"].s == "Concentration";
        static const std::map<std::string, int> kSchool = { {"Alteration", 18}, {"Conjuration", 19}, {"Destruction", 20}, {"Illusion", 21}, {"Restoration", 22} };
        for (const J& x : fx) {
            const int av = static_cast<int>(x["av"].num(-1)), arch = static_cast<int>(x["archetype"].num());
            const double mag = x["mag"].num(), d = x["dur"].num();
            const double ec = x["baseCost"].num() * std::pow(std::max(mag, 1.0) * std::max(d, 1.0), 1.1);
            autoCost += ec;
            const bool host = x["hostile"].yes(), detr = x["detrimental"].yes();
            if (ec > topCost) { topCost = ec; topArch = arch; out.skillReq = static_cast<int>(x["minSkill"].num());
                auto si = kSchool.find(x["skill"].s); out.school = si == kSchool.end() ? -1 : si->second; }
            if (arch == kArSummon) { summon = true; out.summonDur = static_cast<float>(d > 0 ? d : 60); }
            if (arch == kArReanimate) { reanimate = true; out.ctrlMaxLevel = static_cast<float>(mag); out.summonDur = static_cast<float>(d > 0 ? d : 60); }   // card G7: the magnitude is the highest level it raises
            weighable |= Prune::EffectWeighable(arch);
            if (arch == kArCalm || arch == kArDemoralize || arch == kArFrenzy || arch == kArParalysis || arch == kArTurnUndead) {
                control = true; out.ctrlDur = std::max(out.ctrlDur, static_cast<float>(d));
                if (arch == kArParalysis) out.ctrlPar = true; else out.ctrlMaxLevel = static_cast<float>(mag);
                if (arch == kArCalm) out.ctrlCalm = true;
                if (arch == kArDemoralize || arch == kArTurnUndead) out.ctrlFlee = true;
                if (arch == kArFrenzy) out.ctrlFrenzy = true;
                if (arch == kArTurnUndead) { out.turnUndead = true; e.undeadOnly = true; }
            }
            if (arch == kArStagger) out.stagger = true;
            if (arch == kArCloak && self) cloak = true;
            if (host || detr) { hostileAny = true; if (ec > topHostCost) { topHostCost = ec; resist = static_cast<int>(x["resist"].num(-1)); } }
            const bool vm = arch == kArValueMod || arch == kArPeakValueMod || arch == kArAbsorb || arch == kArDualValueMod;
            if ((host || detr) && vm) {
                if (av == kAvHealth) { if (d > 1.0 && !conc) { out.dot += static_cast<float>(mag); out.dotDur = std::max(out.dotDur, static_cast<float>(std::min(d, 30.0))); } else out.dmg += static_cast<float>(mag); }
                else if (av == kAvMagicka) out.mpDmg += static_cast<float>(mag);
                else if (av == kAvStamina) out.spDmg += static_cast<float>(mag);
            }
            // card I6: the ward shape is cast/Archetype.cpp anyWard (a ValueModifier-family effect on WardPower: archetypes 0 / 4 / 5 / 32 / 34)
            if (av == kAvWardPower && (arch == kArValueMod || arch == kArAbsorb || arch == kArDualValueMod || arch == 32 || arch == kArPeakValueMod)) ward = true;
            if (!host && !detr && (arch == kArValueMod || arch == kArPeakValueMod)) {
                if (av == kAvWardPower) {}
                else if (av == kAvHealth) { heal = true; if (d > 1.0 && !conc) { out.hot += static_cast<float>(mag); out.hotDur = std::max(out.hotDur, static_cast<float>(std::min(d, 30.0))); } else out.heal += static_cast<float>(mag); }
                else if (av == kAvDamageResist) { out.armor += static_cast<float>(mag); out.buffDur = std::max(out.buffDur, static_cast<float>(d)); e.meleeBuff = true; }
                else if (av >= kAvPoisonResist && av <= kAvResistMagic) { out.resistBuff = static_cast<float>(mag); out.resistBuffAv = av; out.buffDur = std::max(out.buffDur, static_cast<float>(d)); }
            }
            e.areaUnits = std::max(e.areaUnits, Prune::BlastUnits(static_cast<float>(x["area"].num()), 0.0f, false));
            out.buffDur = std::max(out.buffDur, static_cast<float>(d));
        }
        if (reanimate) { e.drop = Prune::Drop::UnknownNature; out.reanimate = true; }
        else if (summon) e.nature = Prune::Nature::Summon;
        else if (ward) e.nature = Prune::Nature::Ward;
        else if (control) e.nature = Prune::Nature::Control;
        else if (cloak) { e.nature = Prune::Nature::Buff; e.meleeBuff = true; }
        else if (hostileAny) e.nature = Prune::Nature::Offense;
        else if (heal) e.nature = Prune::Nature::Heal;
        else if (topArch == kArScript || fx.empty()) e.drop = Prune::Drop::UnknownNature;
        else e.nature = Prune::Nature::Buff;
        out.summon = e.nature == Prune::Nature::Summon;
        out.ctrl = e.nature == Prune::Nature::Control;
        e.resistAv = (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control) ? resist : -1;
        out.element = resist == kAvResistFire ? "fire" : resist == kAvResistFrost ? "frost" : resist == kAvResistShock ? "shock" : "";
        e.role = Prune::RoleOf(e.nature, e.resistAv);
        e.selfDelivery = self; e.touch = dl == "Touch"; e.concentration = conc;
        // Actuation::AimedReachOf (cast/CanAct.cpp): the record's range counts only from kMinSaneReach (300) up, else the projectile's
        // (not extracted: 4096 stands in). Requiem's target heals carry range 30, which the game does not read as a reach.
        e.reach = (e.selfDelivery || e.touch) ? 0.0f : static_cast<float>(sp["range"].num()) >= 300.0f ? static_cast<float>(sp["range"].num()) : 4096.0f;
        if (dl == "TargetLocation") e.drop = Prune::Drop::Placement;
        if (sp["castType"].s == "ConstantEffect") e.drop = Prune::Drop::NotCastable;
        // Q91 (batch I item 6, Prune::WeighableSpell): no effect IAUS can value (Script, a scripted spawn, kNone, an unknown id) = hidden from IAUS
        if (g_t2.unweighted && e.drop == Prune::Drop::Keep && !fx.empty() && !weighable) e.drop = Prune::Drop::Unweighted;
        out.healOther = heal && !self;
        out.cost0 = static_cast<float>(sp["cost"].num());
        if (out.cost0 <= 0 && sp["autoCalc"].yes()) out.cost0 = static_cast<float>(autoCost);   // APPROXIMATION, only when the record stores 0 and does not override
        e.cost = out.cost0;
        out.recovery = static_cast<float>(sp["recovery"].num());
        // Timing: a fire-and-forget spell charges for SPIT chargeTime (0.5 s is the usual record value); a concentration
        // spell is modelled as a 2 s stream (its per-second magnitude and cost x 2: MODEL).
        if (conc) { out.castSec = 2.0f; out.dmg *= 2.0f; out.heal *= 2.0f; out.mpDmg *= 2.0f; out.spDmg *= 2.0f; out.cost0 *= 2.0f; e.cost = out.cost0; }
        else out.castSec = std::clamp(static_cast<float>(sp["chargeTime"].num(0.5)), 0.25f, 4.0f);
        e.name = sp["edid"].s;
    }

    // ── loading ─────────────────────────────────────────────────────────────────────────────────────────
    inline Weapon ReadWeapon(const J& w) {
        Weapon x; if (!w.has()) return x;
        x.name = w["name"].s; x.dmg = static_cast<float>(w["dmg"].num()); x.speed = static_cast<float>(w["speed"].num(1)); x.reach = static_cast<float>(w["reach"].num(1));
        x.stagger = static_cast<float>(w["stagger"].num()); x.anim = static_cast<int>(w["anim"].num()); x.skill = static_cast<int>(w["skill"].num(kAvOneHanded));
        x.crit = static_cast<float>(w["crit"].num()); x.silver = w["silver"].yes(); x.weight = static_cast<float>(w["weight"].num(8));
        if (x.speed <= 0.05f) x.speed = 1.0f;
        if (x.skill < 6 || x.skill > 8) x.skill = x.Ranged() ? kAvMarksman : x.TwoH() ? kAvTwoHanded : kAvOneHanded;
        return x;
    }

    inline void ApplyRules(Data& d) {
        Rules& r = d.rules; const Rules v;   // v = Skyrim.esm's values
        auto g = [&](const char* k, float dv) { return static_cast<float>(d.G(k, dv)); };
        r.dmgSkillMin = g("fDamageSkillMin", v.dmgSkillMin); r.dmgSkillMax = g("fDamageSkillMax", v.dmgSkillMax); r.dmgWeaponMult = g("fDamageWeaponMult", v.dmgWeaponMult);
        r.armorScale = g("fArmorScalingFactor", v.armorScale); r.armorMax = g("fMaxArmorRating", v.armorMax); r.armorBasePiece = g("fArmorBaseFactor", v.armorBasePiece);
        r.armorSkillLo = g("fArmorRatingBase", v.armorSkillLo); r.armorSkillHi = g("fArmorRatingMax", v.armorSkillHi);
        r.powBonus = g("fPowerAttackDefaultBonus", v.powBonus); r.powStaBase = g("fStaminaAttackWeaponBase", v.powStaBase); r.powStaMult = g("fStaminaAttackWeaponMult", v.powStaMult);
        r.powStaPenalty = g("fPowerAttackStaminaPenalty", v.powStaPenalty);
        r.combatMp = g("fCombatMagickaRegenRateMult", v.combatMp); r.combatSp = g("fCombatStaminaRegenRateMult", v.combatSp);
        r.delayHp = g("fDamagedHealthRegenDelay", v.delayHp); r.delayMp = g("fDamagedMagickaRegenDelay", v.delayMp); r.delaySp = g("fDamagedStaminaRegenDelay", v.delaySp);
        r.costBase = g("fMagicCasterSkillCostBase", v.costBase); r.costMult = g("fMagicCasterSkillCostMult", v.costMult); r.costScale = g("fMagicSkillCostScale", v.costScale);
        r.bowDraw = g("fBowDrawTime", v.bowDraw); r.diffToPC = g("fDiffMultHPToPCN", v.diffToPC); r.diffByPC = g("fDiffMultHPByPCN", v.diffByPC);
        r.hpPerLevel = g("fNPCHealthLevelBonus", v.hpPerLevel); r.hmsPerLevel = g("iAVDhmsLevelUp", v.hmsPerLevel); r.skillPerLevel = g("iAVDSkillsLevelUp", v.skillPerLevel);
        r.skillStart = g("iAVDSkillStart", v.skillStart); r.maxResist = g("fPlayerMaxResistance", v.maxResist); r.combatDistance = g("fCombatDistance", v.combatDistance);
        r.blockPowMult = g("fBlockPowerAttackMult", v.blockPowMult); r.rangedMin = g("fCombatRangedMinimumRange", v.rangedMin); r.rangedDist = g("fCombatRangedDistance", v.rangedDist);
        r.bleedoutDefault = g("fBleedoutDefault", v.bleedoutDefault);
    }

    inline void AddPoolSpell(Data& b, SimSpell&& s) {
        if (s.e.drop == Prune::Drop::Keep) { ++b.natures[static_cast<int>(s.e.nature)]; b.spells.push_back(std::move(s)); }
        // Card G7: a Reanimate spell is KEPT, in its own list. IAUS still has no family for it (Prune: nature Unknown, as in the game), so
        // its book entry stays dropped for IAUS; a table row with cond.corpse_within casts it at a corpse. It enters a book only by name
        // (a class row's Reanimate role, --teach reanimate), never by the random draw, so no other class's book changes.
        else if (s.reanimate && g_t2.reanimate && s.e.drop == Prune::Drop::UnknownNature) { ++b.keptReanim; b.reanimSpells.push_back(std::move(s)); }
        else { ++b.natures[0]; switch (s.e.drop) { case Prune::Drop::Placement: ++b.dropPlace; break; case Prune::Drop::NotCastable: ++b.dropNotCast; break; case Prune::Drop::Unweighted: ++b.dropUnweighted; break; default: ++(s.reanimate ? b.dropReanim : b.dropUnknown); } }
    }

    inline void TagRoles(Data& d) {
        auto has = [](const std::string& s, const char* k) { return s.find(k) != std::string::npos; };
        for (int i = 0; i < static_cast<int>(d.specs.size()); ++i) {
            const ActorSpec& a = d.specs[static_cast<std::size_t>(i)];
            bool healer = false, caster = false;
            for (int si : a.spells) { const SimSpell& s = d.foeSpells[static_cast<std::size_t>(si)]; healer |= s.healOther; caster |= s.e.nature == Prune::Nature::Offense; }
            auto add = [&](const char* r) { d.roles[r].push_back(i); };
            if (has(a.race, "Draugr")) add(a.ranged.Has() && !a.melee.Has() ? "draugr_archer" : "draugr");
            if (has(a.race, "Troll")) add(has(a.race, "Frost") ? "frost_troll" : "troll");
            if (has(a.race, "AtronachFrost")) add("frost_atronach");
            if (has(a.race, "AtronachFlame")) add("flame_atronach");
            if (a.dragon && !a.undead && has(a.race, "DragonRace")) add("dragon");
            if (has(a.race, "Wolf")) add("wolf");
            if (has(a.race, "Giant")) add("giant");
            if (!a.creature && has(a.name, "Bandit")) add(caster ? "bandit_mage" : (a.ranged.Has() && (!a.melee.Has() || has(a.name, "Missile") || has(a.name, "Archer"))) ? "bandit_archer" : "bandit");
            if (!a.creature && healer) add("healer");
            if (!a.creature && caster && !has(a.name, "Bandit")) add("mage");
        }
    }

    inline Data LoadData(const char* path);   // below
    inline Data BuiltinData();                 // Builtin.h

}
