marth's Follower Overhaul (MFO)
===============================
Programmable follower behaviour: per-follower gambit rules, human-feel
actuation, looting, trading, and Rapport progression.

REQUIREMENTS
------------
  * SKSE64
  * Address Library for SKSE Plugins
  * SkyUI
  * MCM Helper (version 9 or newer)
  * powerofthree's Papyrus Extender (po3_papyrusextender.dll)
      Required ONLY for the optional follower economy (selling/buying);
      everything else runs without it. If absent, the economy stays off and
      MFO logs one line saying so.
  * Synthesis  (https://github.com/Mutagen-Modding/Synthesis)
      MFO reads an item catalog (mfo_items.json) that a Synthesis patcher
      generates from YOUR load order, so gambit categories (potions, ammo,
      gear classes) cover modded items with no per-mod patch (see SETUP).
      Synthesis brings its own .NET dependency, so there is nothing extra to
      install for MFO and no program to run by hand. RECOMMENDED, not
      required — without it MFO falls back to a shipped catalog.
  * No DLC required. VR is not supported.

ANNIVERSARY EDITION (1.6.x) IS RECOMMENDED: the walk-to-it behaviours —
walking to loot, Flee to player, and Auto-retreat — ride a native alias fill
that is only verified on AE, so on Special Edition (1.5.97) they are inactive
(loot is picked up at arm's reach; flee/retreat do nothing). Everything else
— combat gambits, restocking, looting itself, and the merchant economy —
works on SE and AE alike.

The bundled MFO.dll is part of the mod and required.

WHAT IT DOES
------------
Every follower carries ordered lists of GAMBITS — [Condition] -> [Action]
rules, first match wins — authored live, in game, per follower:
  * TWO tables per follower: combat (heal, cast, attack, drink) and
    logistics (restock, loot, upgrade equipment) — separate slots, never
    interleaved. One action per tick; the cost of a heal is the attack not
    made.
  * Followers loot like people: they WALK to loot, pick locks they're
    skilled enough for, leave fresh corpses to you for a few seconds, never
    take owned goods, hold off while you sneak, and stay out of player homes
    unless you opt in.
  * Followers trade at merchants (off by default): they sell unworn junk and
    buy what they've run short on, from their own purse, from what that
    vendor actually stocks.
  * How far they range is their CONFIDENCE — healthy in an easy zone they
    push ahead; hurt or outmatched they fall back to your side on their own.
  * Rule slots are earned through RAPPORT, built by fighting alongside that
    specific follower. It survives dismissal.
  * Rules are authored on the FIELD ORDERS power: a native board (four
    skins, full controller support) with a plain-language summary of every
    rule, plus a passive combat HUD.

SETUP
-----
1. Install this mod with your mod manager as a normal mod and enable
   MFO.esp. Load order is not sensitive — MFO drives followers through
   runtime AI, not record edits.

2. (Recommended) Adapt MFO's item catalog to YOUR load order with Synthesis
   (there is no exe):
     EASIEST: download the MFO.synth file, then in Synthesis select a group
       and double-click it — it adds the patcher for you (right project
       preselected).
     OR MANUALLY:
       a. Open Synthesis, add a new patcher -> Git Repository, point it at:
            https://github.com/marthofdoom/MFO
          Project:  installer/MFO.Synthesis/MFO.Synthesis.csproj
       b. Run your Synthesis pipeline. (Synthesis builds it for you.)
   Re-run Synthesis after ANY load-order change so the catalog stays matched
   to your list.

3. Launch through SKSE. Open the MCM (Mod Configuration -> marth's Follower
   Overhaul) to configure; every behaviour ships off or conservative by
   default. Follower economy and its tuning live there too.

DID IT WORK?
------------
In game, the SKSE log (Documents/My Games/Skyrim Special Edition/SKSE/
MFO.log) should show:
  * "=== MFO <version> loading ==="            — the DLL is live
  * "[catalog] loaded N potions ..."            — the item catalog loaded
  * "[follower] + <id> <name> (teammate)"       — your followers detected

WITHOUT the Synthesis patcher MFO still runs on its shipped catalog; modded
consumables/gear simply may not be categorised. Running Synthesis upgrades
coverage to your actual load order.

NOTES
-----
No FOMOD — all options are runtime MCM toggles. Updating mid-playthrough is
safe: all state lives in the SKSE co-save and reverts cleanly on load.
Gambits layer on top of vanilla AI and never replace it: a follower with no
matching rule behaves byte-identically to one without MFO installed, so MFO
does not conflict with follower frameworks, AI overhauls, or combat mods.

Send bug reports with the MFO.log from that session — the board and the log
are built so that "why didn't it act" is answerable.
