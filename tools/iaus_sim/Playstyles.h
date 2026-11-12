#pragma once
// iaus_sim/Playstyles.h -- THE EXPERT PLAYSTYLE OF EVERY CLASS, in lab terms. HOST-ONLY, never part of the DLL.
//
// One row per class of native/classes/ClassTable.cpp (all 35, T1 to the hidden T5). Each row is written from the class's
// card and marth's rulings (Docs/research/2026-11/class-walkthrough-decisions.md, class-build-plan.md, open-questions-for-
// marth.md) and says, for a skilled human playing that class:
//   does / never : the playstyle in words (the doc Docs/research/2026-11/iaus-classes.md carries the sources per line)
//   bands        : the class SIGNATURE as numbers. Each band is one measurable ruling in the results file ("rulings")
//                  AND one term of the class-personality score ("persona": how much of the signature the follower shows;
//                  100 = an observer sees the class, low = he plays like a generic bot of his line)
//   forbid       : the ORACLE CONSTRAINT SET. The expert oracle (Oracle.h) may not take an option the class's rulings
//                  forbid, so the oracle gap of a class is measured against an expert who PLAYS THAT CLASS
//   gaps         : what the class's card needs that today's actions cannot express (the GAP LEDGER rows of Results.h)
//
// A band's numbers are set from the CARD on this lab's scales (see the doc's "Scales" table), never fitted to what IAUS
// does today: a band IAUS fails is a finding. Metric names are the results file's (Results.h SchemaMetrics).
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace Sim {

    // ── the oracle constraint bits (Oracle.h ClassAllows applies them) ─────────────────────────────────────
    enum : unsigned {
        kPsNoMelee     = 1u << 0,   // a pure mage: the blade and the power attack only when he has no cast, no magicka potion and no charged staff left
        kPsNoMeleeSwap = 1u << 1,   // never swaps to melee while he has ammunition (Ranger, Jaqspur: "WITHOUT melee swap")
        kPsGoIn        = 1u << 2,   // a hybrid goes into melee only when its go-in rule holds (card C9; World.h GoInOk)
        kPsNoRetreat   = 1u << 3,   // never backs off: no flee, no step back, no hide (the wall and the warrior lines)
        kPsBackOnly    = 1u << 4,   // "retreat" is backing away while still fighting: no flee, no sprint away (the mercenary line)
        kPsNearPlayer  = 1u << 5,   // leashed near the player: no move away once he is 900 units from him (the healers)
    };
    // Every class: never an area cast that reaches the party (marth: "never splash the party"). Always on, no bit.

    struct Band { const char* metric; double lo, hi; const char* why; };   // the metric must sit in [lo, hi]; +-1e9 = open
    inline constexpr double kOpen = 1e9;

    struct Playstyle {
        const char* cls;
        const char* does;
        const char* never;
        std::vector<Band> bands;
        unsigned forbid = 0;
        const char* gaps = "";     // comma list of GAP LEDGER ids (Results.h ClassGapRows)
    };

    inline const std::vector<Playstyle>& Playstyles() {
        auto atLeast = [](const char* m, double v, const char* why) { return Band{ m, v, kOpen, why }; };
        auto atMost = [](const char* m, double v, const char* why) { return Band{ m, -kOpen, v, why }; };
        static const std::vector<Playstyle> v = {
            // ── tier 1: "t1 behaves vanilla for those roles" (marth). Only the IAUS rulings bind ──
            { "melee", "closes and fights with his weapon the way the vanilla AI does; drinks when low; no class package",
              "casts (he has mastered no mage class: ruling Q34)",
              { atLeast("melee_share_pct", 35, "a melee follower spends his time swinging"), atMost("b.cast", 2, "ruling Q34: no spells without a mastered mage class") }, 0, "" },
            { "ranged", "shoots from range, swaps to melee only when out of arrows or a foe is on him",
              "stands in melee by choice",
              { atLeast("b.shoot", 45, "a ranged follower spends his time shooting"), atMost("contact%", 25, "he keeps his distance") }, 0, "" },
            { "mage", "casts from range; reaches for magicka, a staff or distance before a blade",
              "melees while any affordable cast, magicka potion or staff charge is left (marth 11-10)",
              { atMost("melee_share_pct", 5, "marth 11-10: a T1 mage almost never melees"), atLeast("b.cast", 40, "he spends his time casting"), atMost("contact%", 20, "spell range, not melee range") }, kPsNoMelee, "" },

            // ── tier 2 ──
            { "knight", "the wall: stays on the player, taunts what attacks an ally, keeps his guard up and drops it only to attack, bashes; little circling, no flank",
              "leaves the line (short leash), backs off, swings heavy more than a quarter of the time (ch.18 cap 0.25)",
              { atLeast("contact%", 60, "he stays in the fight's front"), atMost("pDist", 450, "short ch.23 leash: beside the player"), atLeast("b.block", 15, "marth Q2: an in-your-face tank only stops blocking to attack"),
                atMost("powShare%", 25, "ch.18 power share max 0.25"), atLeast("taunt_per_min", 0.5, "the taunt is his first row"), atMost("retreat%", 1, "a wall does not back off") }, kPsNoRetreat, "C-BLOCK,C-C8,C-NOFLEE" },
            { "warrior", "pushes hard and head on, circles and flanks, presses with allies; power attacks a blocking or a stronger foe, at most one per 10 s per rule",
              "backs off; lets a blocking foe turtle",
              { atLeast("contact%", 55, "he pushes hard"), atLeast("powShare%", 40, "ch.18 power floor 0.40"), atMost("pow/min", 6.5, "rulings Q7 / Q73: one power-attack cooldown of 10 s"), atMost("retreat%", 0.5, "marth: never backs off") }, kPsNoRetreat, "C-C8,C-POWER,C-NOFLEE" },
            { "mercenary", "in and out: circles wide, flanks, backs away while still facing and fighting; kills casters, then archers, then the weakest",
              "flees (retreat is backing away, never a disengage); power attacks more than 15% (ch.18 cap); bashes much",
              { atLeast("contact%", 40, "he fights in melee"), atMost("powShare%", 15, "ch.18 power cap 0.15"), atLeast("prio%", 60, "casters, then archers, then the lowest health"), atMost("retreat%", 1, "marth: backing away, never a flee") }, kPsBackOnly, "C-C8,C-NOFLEE" },
            { "archer", "a line archer: shoots casters first, then archers; a one-handed weapon only when something is on him; picks up arrows when out",
              "wields a two-hander; stands in melee by choice; keeps missing the same shot (needs miss detection and correction)",
              { atLeast("b.shoot", 50, "the bow is his weapon"), atMost("contact%", 20, "a line archer keeps the line"), atMost("melee_share_pct", 12, "melee only when a foe is inside 150"), atLeast("prio%", 55, "casters, then archers"), atMost("miss%", 50, "ruling Q56: at most 5 misses in 10") }, 0, "C-H5" },
            { "thief", "sneaks whatever the player does; one fight in five he opens undetected, the others by his own sneak against the foes' senses; dagger when close and flanking, else an undetected bow on the lowest-health foe; breaks off to where nobody sees him and hides",
              "fights in the open when he could be hidden; uses a crossbow; trades blows with a brute",
              { atLeast("sneak%", 85, "marth: sneak regardless of player action"), atLeast("opener%", 20, "a free stealth entry every 5 fights, the rest by his stats"), atLeast("sneakDmg%", 15, "his damage comes out of stealth"),
                atLeast("b.shoot", 30, "otherwise an undetected bow"), atMost("contact%", 40, "dagger only when close and flanking"), atMost("powShare%", 20, "ch.18 cap 0.20 (ruling Q27)") }, 0, "C-G1,C-G2,C-G3,C-G13" },
            { "evoker", "stays at the furthest range his spell works at, steps out of incoming spells; fire on a fire-weak foe, else the nearest; a magicka potion when low",
              "melees; casts into a resistance; blasts the party",
              { atLeast("b.cast", 50, "Destruction is all he does"), atMost("melee_share_pct", 3, "marth 11-10: pure mages up the tree melee even less"), atMost("contact%", 15, "spell range (H4)"), atMost("immune%", 0, "never casts into an immunity"), atMost("resisted%", 15, "picks the element by the target") }, kPsNoMelee, "C-H3,C-H4,C-FLEE2" },
            { "conjurer", "summons first and stays behind it, re-summons the moment it falls, puts the summon on the ground near the foe; shoots what attacks his summon; a dagger only when something is on him",
              "fights without a summon up; stands in front of it",
              { atLeast("summonUp%", 60, "summon first, re-summon"), atMost("melee_share_pct", 3, "a pure mage"), atMost("contact%", 15, "behind his summon"), atLeast("b.cast", 30, "he casts between summons") }, kPsNoMelee, "C-G5,C-H2,C-H3,C-FLEE2" },
            { "healer", "the medic: behind the party and near the player; heals the player, then an ally, then himself below half; attacks only when nobody needs him",
              "lets someone die with a heal in hand; wanders from the player; two heals on one wound; melees",
              { atMost("pDist", 600, "leashed near the player"), atMost("melee_share_pct", 3, "a pure mage"), atMost("contact%", 15, "behind the party"), atMost("healAt%", 60, "heals below half (his rows say 50)"), atMost("dwh/f", 0.1, "nobody goes down while he holds a heal"), atMost("overheal_pct", 35, "the heal fits the wound") }, kPsNoMelee | kPsNearPlayer, "C-H2,C-H3,C-FLEE2" },
            { "warlock", "the controller: paralyses or frightens the stronger foe first, keeps his armor spell up, then Destruction on the nearest; never spends a cast on a foe his spell cannot touch",
              "casts a level-capped spell at a foe above its cap (the base will-work rule); melees",
              { atLeast("a.cast_control", 8, "control comes first"), atMost("ctrlFail%", 10, "marth: a spell that will never work is skipped"), atMost("melee_share_pct", 3, "a pure mage"), atMost("contact%", 15, "spell range") }, kPsNoMelee, "C-H3,C-FLEE2" },

            // ═══ PARKED (marth 2026-11-11: the class release order is T2 first, Classes Beta 1, THEN T3) ═══════════════════════════
            // Everything below this line (tier 3, tier 4, the Dragonborn) is a FIRST DRAFT written before the scope change. It was
            // never measured, tuned or reviewed, and its numbers were not checked against a run. It is kept so the T3 round does
            // not start from nothing. The class report runs only the 12 T1 + T2 classes (kBeta1Classes below); do not quote a T3+
            // row as a ruling until its round re-derives it from the cards.
            // ── tier 3, pure ──
            { "great-knight", "the knight made extreme: the wall with Shieldwall, taunts at three foes or more as well, heavier guard, shorter leash",
              "leaves the line; swings heavy more than a fifth of the time",
              { atLeast("contact%", 60, "the front"), atMost("pDist", 450, "short leash"), atLeast("b.block", 20, "blocks even more than the knight"), atMost("powShare%", 20, "ch.18 cap 0.20"), atLeast("taunt_per_min", 0.8, "taunts allies' attackers and crowds"), atMost("retreat%", 1, "a wall") }, kPsNoRetreat, "C-BLOCK,C-C8" },
            { "champion", "the warrior made extreme: heavy blows are his language (floor 40%, +20% power damage), same 10 s pacing per rule",
              "backs off",
              { atLeast("contact%", 55, "pushes hard"), atLeast("powShare%", 40, "ch.18 floor 0.40"), atMost("pow/min", 6.5, "the 10 s cooldown"), atMost("retreat%", 0.5, "never backs off") }, kPsNoRetreat, "C-C8,C-POWER" },
            { "blademaster", "the mercenary made extreme: a flurry of light blows from the flank, in and out, casters and archers first",
              "flees; swings heavy more than 15%",
              { atLeast("contact%", 40, "melee"), atMost("powShare%", 15, "ch.18 cap 0.15"), atLeast("prio%", 60, "casters, archers, the weakest"), atMost("retreat%", 1, "backing away only") }, kPsBackOnly, "C-C8" },
            { "ranger", "the longest range of all; never swaps to melee: a melee foe that closes is MARKED for the whole party instead; at most 3 misses in 10",
              "draws a blade while he has arrows; misses more than 3 in 10",
              { atLeast("b.shoot", 55, "he only shoots"), atMost("melee_share_pct", 3, "his set has no melee swap"), atMost("contact%", 15, "longest range"), atLeast("tgtDist", 900, "longest range"), atMost("miss%", 30, "marth 11-10: at most 3 misses in 10") }, kPsNoMeleeSwap, "C-H5,C-MARK" },
            { "master-thief", "the thief perfected: opens undetected one fight in three, sneak attacks for half again as much, strikes the foe an ally has engaged (half the sneak bonus), chases the fleeing first; hides again whenever he is seen",
              "stays seen; wastes a sneak attack on a foe that will die anyway",
              { atLeast("sneak%", 85, "sneak always on"), atLeast("opener%", 33, "a free stealth entry 1 fight in 3"), atLeast("sneakDmg%", 25, "sneak attacks x1.5 carry his damage"), atLeast("hidden%", 25, "an unseen sneaking follower is hidden at once, and he works to be unseen"),
                atMost("powShare%", 20, "ch.18 cap 0.20") }, 0, "C-G1,C-G2,C-G3,C-G4,C-G13" },
            { "wizard", "stays at range like every mage; wards only while a hostile spell is being charged at him; paralyses the stronger foe and pours Destruction into the held one (+20% there)",
              "holds a ward when nobody is casting at him; melees",
              { atLeast("b.cast", 50, "a pure caster"), atMost("melee_share_pct", 3, "a pure mage"), atMost("contact%", 15, "spell range"), atMost("ctrlFail%", 10, "the level skip"), atMost("immune%", 0, "never into an immunity") }, kPsNoMelee, "C-G11,C-H3" },
            { "necromancer", "far back, better at staying away than any other mage; always a thrall up, a corpse raised when one lies in reach; shoots what attacks his thralls",
              "is found in melee; fights without a thrall",
              { atMost("contact%", 8, "marth: BETTER at staying away than other mages"), atLeast("tgtDist", 1000, "far back"), atLeast("summonUp%", 70, "+1 thrall: always something up"), atMost("melee_share_pct", 2, "a pure mage, further back") }, kPsNoMelee, "C-G5,C-G7,C-H2,C-H3" },
            { "priest", "the healer's leash and priorities, plus full aggression against the undead: he closes on them (his sun cloak burns within 6 m) and turns them before he heals himself",
              "lets someone die with a heal in hand; ignores the undead",
              { atMost("pDist", 700, "the healer's leash"), atMost("healAt%", 60, "heals below half"), atMost("dwh/f", 0.1, "nobody goes down while he holds a heal"), atMost("melee_share_pct", 3, "a pure mage") }, kPsNoMelee | kPsNearPlayer, "C-UNDEAD,C-H2,C-H3" },

            // ── tier 3, hybrids: each has its own go-in rule (card C9) ──
            { "spellsword", "spell before closing, blade inside 400; low on health with magicka left he drops back to spell range, low on magicka or healthy he goes back in",
              "stands in melee at low health with a full pool",
              { atMost("goInBad%", 5, "marth: a health / magicka / range bias decides in or out"), atLeast("melee_share_pct", 15, "he does fight with the blade"), atLeast("b.cast", 15, "and with the spell") }, kPsGoIn, "C-C9,C-H3" },
            { "sorcerer", "summons, binds his weapon, and goes in ONLY at high health with all his summons out; otherwise he fights from behind them",
              "is in melee hurt, or with no summon standing",
              { atMost("goInBad%", 5, "marth: only go in when high health and full summons"), atLeast("summonUp%", 60, "his summons are his armor"), atLeast("melee_share_pct", 10, "a bound blade in hand") }, kPsGoIn, "C-C9,C-G5,C-G17" },
            { "templar", "a knight-like line holder who heals: goes in only while the whole party is at 70% or better, or when he has no magicka left to help; taunts for an ally; swings heavy, more so against the undead",
              "leaves the line; fights on while an ally under 70% waits for a heal he could give",
              { atMost("goInBad%", 5, "marth: only go in when all the party is in good health, or no magicka"), atMost("pDist", 500, "a line holder (short leash)"), atMost("healAt%", 75, "heals at the 70% party-healthy mark"), atMost("retreat%", 1, "holds the line"), atLeast("taunt_per_min", 0.3, "taunts for an ally") }, kPsGoIn, "C-C9,C-FLEE,C-UNDEAD" },
            { "nightblade", "frightens or paralyses first and goes in only on a controlled foe, or when none can be controlled; flanks; cuts down the fleeing",
              "trades blows with a foe his Illusion could have taken out of the fight",
              { atMost("goInBad%", 5, "marth: only go in on controlled enemies, or when none can be controlled"), atLeast("a.cast_control", 6, "control opens his fights"), atMost("ctrlFail%", 10, "the level skip"), atLeast("melee_share_pct", 10, "then the blade") }, kPsGoIn, "C-C9,C-H3" },
            { "witchhunter", "never goes in: bow on casters, undead and Daedra, a paralysis on what comes within 250; a blade only when every foe is controlled",
              "is found in melee",
              { atMost("contact%", 12, "marth Q10: never goes in"), atMost("melee_share_pct", 3, "no melee by default"), atLeast("b.shoot", 40, "the bow"), atMost("goInBad%", 2, "melee only if ALL enemies are controlled"), atMost("miss%", 40, "ruling Q56: at most 4 misses in 10") }, kPsGoIn, "C-C9,C-H5,C-H6" },
            { "scout", "stays ranged and fast; heals an ally or the player under 70%, shoots casters; goes in only when the party is healthy and his arrows are gone",
              "leaves the party behind; melees while he has arrows",
              { atLeast("b.shoot", 35, "he stays ranged"), atMost("contact%", 15, "ranged"), atMost("healAt%", 75, "gambits at 70%"), atMost("pDist", 800, "near the party he heals"), atMost("goInBad%", 3, "the party-healthy rule"), atMost("miss%", 40, "ruling Q56: at most 4 misses in 10") }, kPsGoIn | kPsNearPlayer, "C-C9,C-H5" },

            // ── tier 4: the parents' passives at full + one unique mechanic ──
            { "arch-mage", "two different spells at once, one per hand; wards a charging caster, summons when outnumbered, heals under 70%, the element the foe is weak to",
              "melees; holds one hand idle",
              { atLeast("b.cast", 50, "a pure caster"), atMost("melee_share_pct", 2, "the purest mage"), atMost("contact%", 12, "spell range"), atMost("healAt%", 75, "heals under 70%"), atMost("immune%", 0, "element by weakness") }, kPsNoMelee, "C-H7,C-G11,C-H3" },
            { "living-legend", "champion mode against one big foe (heavy blows), flurry mode against many (light blows); attacks through incoming blows, 40% of them are blocked for him",
              "backs off",
              { atLeast("contact%", 60, "always in it"), atLeast("powShare%", 25, "between the flurry cap and the champion floor"), atMost("retreat%", 0.5, "never backs off") }, kPsNoRetreat, "C-H8,C-C8,C-MODE" },
            { "nightingale", "sneaks always; every 30 s the Shadowcloak takes him out of every foe's sight for 3 s and he re-opens with a sneak attack; bow on casters, blade on the fleeing",
              "lets a cloak pass without a sneak attack",
              { atLeast("sneak%", 85, "sneak always on"), atLeast("opener%", 33, "opens from stealth"), atLeast("sneakDmg%", 30, "Shadowcloak re-opens with a sneak attack every 30 s"), atLeast("hidden%", 25, "he works to be unseen") }, 0, "C-G1,C-G2,C-G3,C-H10,C-H6" },
            { "listener", "the executioner: sneaks always, takes the weakest first, a heavy blow on a foe under a quarter health, Fear when three or more press; Illusion on any non-boss up to his own level",
              "leaves a dying foe alive; swings heavy more than 25%",
              { atLeast("sneak%", 85, "sneak always on"), atLeast("opener%", 33, "opens from stealth"), atLeast("sneakDmg%", 30, "the executioner kills out of stealth"), atLeast("melee_share_pct", 20, "his rows end in equip melee: a blade executioner"), atMost("powShare%", 25, "ch.18 cap 0.25") }, 0, "C-G1,C-G2,C-G3,C-ROOT" },
            { "ebony-warrior", "the great knight who heals: holds the line and takes a quarter of his neighbours' damage on himself; taunts at two foes or more; in only while the party is at 70%",
              "leaves the line",
              { atLeast("contact%", 55, "the front"), atMost("pDist", 500, "short leash"), atLeast("taunt_per_min", 0.8, "taunts at two foes"), atMost("goInBad%", 5, "the Templar go-in rule"), atMost("healAt%", 75, "heals at 70%") }, kPsGoIn, "C-H9,C-BLOCK,C-C9" },
            { "grand-battlemage", "every power attack also releases the spell in his off hand at the struck foe, for free: he power attacks MORE when that spell is what the foe is weak to, and never when the blast would reach the party",
              "releases an area spell into the party; holds a spell the foe resists",
              { atLeast("pow/min", 3, "the power attack is his cast"), atLeast("b.cast", 15, "spell before closing"), atLeast("melee_share_pct", 20, "melee inside 400") }, 0, "C-H11,C-C9" },
            { "jaqspur", "the sniper: highest-level foe first, draws 60% faster, at most 1 miss in 10, marks a melee foe that closes",
              "misses; draws a blade",
              { atLeast("b.shoot", 55, "the sniper"), atMost("contact%", 10, "never in melee"), atLeast("tgtDist", 1000, "sniper range"), atMost("miss%", 10, "marth 11-10: at most 1 miss in 10"), atMost("melee_share_pct", 3, "no blade") }, kPsNoMeleeSwap, "C-H5,C-MARK" },
            { "saint", "raises the downed first, leads against the undead (his aura heals allies and burns undead), heals under 70%, taunts for an ally",
              "lets someone stay down",
              { atMost("healAt%", 75, "heals under 70%"), atMost("dwh/f", 0.1, "nobody goes down while he holds a heal"), atMost("pDist", 700, "his aura is where the party is"), atMost("melee_share_pct", 3, "a mage line") }, kPsNoMelee | kPsNearPlayer, "C-AURA,C-UNDEAD" },

            // ── tier 5 (hidden): the player picks which mastered T4 he fights as; his own signature is the shout ──
            { "dragonborn", "fights as the mastered T4 the player picked, with his most-perked weapon, switching like Auto (a bow for a flying dragon); shouts (Unrelenting Force at three foes or more)",
              "is bound by any class limit",
              { }, 0, "C-G8,C-DB" },
        };
        return v;
    }

    // Classes Beta 1 = tiers 1 and 2: the classes whose playstyles are in force (the --classes value of the class report).
    inline constexpr const char* kBeta1TreeClasses = "melee,ranged,mage,knight,warrior,mercenary,archer,thief,evoker,conjurer,healer,warlock";   // --classes beta1-tree: the 12 tree-ON classes (every J quoted before the sim batch, sections 30 and 31)
    inline constexpr const char* kBeta1Classes = "melee,ranged,mage,knight,warrior,mercenary,archer,thief,evoker,conjurer,healer,warlock,melee-off,ranged-off,mage-off";   // --classes beta1: + the 3 tree-OFF base cells (marth 11-12)

    inline const Playstyle* PlaystyleOf(const std::string& cls) {
        for (const Playstyle& p : Playstyles()) if (cls == p.cls) return &p;
        return nullptr;
    }
    // The PARKED rows (everything below the banner) take effect only with --parked. A class is Beta 1 when kBeta1Classes lists it.
    inline bool g_parked = false;
    [[nodiscard]] inline bool IsBeta1(const std::string& cls) { return (std::string(",") + kBeta1Classes + ",").find("," + cls + ",") != std::string::npos; }
    [[nodiscard]] inline bool PlaystyleInForce(const std::string& cls) { return g_parked || IsBeta1(cls); }
    // The playstyle of a class when it is in force (Beta 1, or --parked), else nullptr: rulings, persona, forbid bits and the class passives all read this.
    inline const Playstyle* ActivePlaystyle(const std::string& cls) { return PlaystyleInForce(cls) ? PlaystyleOf(cls) : nullptr; }

    // How much of one band is met, 0..1: 1 inside it, falling to 0 at half the bound (or 5, whichever is larger) outside.
    inline double BandCredit(const Band& b, double v) {
        const double viol = v < b.lo ? b.lo - v : v > b.hi ? v - b.hi : 0.0;
        if (viol <= 0) return 1.0;
        const double bound = v < b.lo ? b.lo : b.hi;
        return std::max(0.0, 1.0 - viol / std::max(5.0, 0.5 * std::fabs(bound)));
    }
    // The class-personality score of one cell, 0..100 (-1 = the class has no band, or no band's metric had a sample).
    inline double Persona(const std::string& cls, const std::map<std::string, double>& m) {
        const Playstyle* p = ActivePlaystyle(cls); if (!p) return -1;
        double sum = 0; int n = 0;
        for (const Band& b : p->bands) { auto it = m.find(b.metric); double v;   // the same rule as the rulings (Results.h RulingValue): a missing a.<family> share is 0
            if (it != m.end()) { v = it->second; if (v < 0) continue; } else if (!std::strncmp(b.metric, "a.", 2)) v = 0; else continue;
            sum += BandCredit(b, v); ++n; }
        return n ? 100.0 * sum / n : -1;
    }

}
