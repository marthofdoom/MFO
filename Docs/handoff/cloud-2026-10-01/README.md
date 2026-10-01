# Cloud session handoff (2026-09-30 to 2026-10-01)

This is for the local agent. The cloud session ran on a promo. Everything it produced is in git, in this folder and on the branches listed below. Nothing important lives only in the cloud scratchpad.

## 1. Merged to main this session

Each item had an Opus review and a green CI run. CI only proves the build compiles: nothing was tested in game.

| Merge | What |
|---|---|
| 0d353fc and earlier | UX batch: hotkeys default to -1 with a one-time migration, lockpick sounds at the follower (MFO.esp SNDR 0x904/0x905), retained follower's cell on the Followers tab, input trampoline E8 check, ToggleControls storeState, co-save reader guards (B13, FWPN cap), gem choice fixes, `[deadtgt]` diagnostic |
| 6370b242 | Equip authority small batch (B134): a refused claim means a looted weapon is stocked, not equipped. Torch refusal log is latched. Adds the passive `[inv-probe]` |
| a02e13be | B63: best owned armor per slot declared as one slate |
| f690e6e7 | Translatable Board/HUD text (`native/i18n/`, `out/Interface/Translations/MFO_ENGLISH.txt`, Docs/TRANSLATING.md) |
| f4ff8127 | Passive `[archetype]` and `[summon-probe]` diagnostics (cast/Archetype.cpp, cast/SummonProbe.cpp) |
| ad6d1bd1 | Ranged pick: ammo first, then perks. Relic pools, crossbow perk signal |
| 4f574cd0 | HMS is core: runs without the add-on. Auto class resolver (logistics/ClassResolve.cpp), HMS-only PRGN records (v8 layout, no bump), never-processed adopt armed at load only |

Main CI is green on 4f574cd0 (run 36900292422). Not released. v2.0.16 is the last tag.

## 2. Open branches

| Branch | State | Next step |
|---|---|---|
| `fix/mfo-heal-starve-retreat` | Heal fixes from the 10-01 field log: P1 heal hold, P2 retreat waits for own cast, self-heal while retreating (marth: "Themselves, yes. others no."). Opus reviewed, final small round in progress at handoff | If not merged by the cloud: read the last diff, check CI on the newest SHA, merge |
| `feat/mfo-remove-roster` | Remove from roster (86e3eewaf). STOWED, unreviewed, tier A | See its `Docs/handoff/remove-roster.md`: Opus tier-3 review of the undo list, then a Deck test |
| `feat/mfo-loot-giveway`, `fix/mfo-summon-ground-position`, `split/cast-direct` | Not from this cloud session | Yours |
| MEO and linux-native-tools branches | Parked by marth ("let the other two go for now") | Untouched |

## 3. NEEDS LOCAL (Deck / game data)

- **v6/v7 co-save round trip:** run `tools/splitcheck/cosave_roundtrip.py` on old saves. Also run it on a save holding HMS-only records, then load that save with a shipped v8 DLL. Expected: it drops them at its next save, and this is documented.
- **HMS core:**
  - A follower with no add-on class, on Auto and on a picked class, with and without the add-on. Grep `[hms-diag] resolvedClass=`, `[hms-parity]`, `[hms] ... never-processed HMS block adopted`. The adopt line should appear once per loaded follower and not again after a save and reload.
  - Enroll a follower in-session, then give them an engine level jump before the player levels. You should see `[hms-parity] retro-pending award`.
  - Confirm the base H/M/S table numbers with marth: Melee 60/5/35, Ranged 40/5/55, Mage 15/80/5.
- **Translations:**
  - Expect `[i18n] english check: mfoOverrides=216 mismatches=0`.
  - Run a non-English test with a sample MFO_FRENCH.txt, then check gamepad navigation in the Gambits and Progression lists.
  - The Gambits button column is now about 125 px, because "sure?" clipped at 96. Check it in game.
  - release.sh now needs python3.
- **Heal fixes** (once merged):
  - `[heal-hold]` including "idle right-hand claim was released", then `IDLE-HAND FLOOR claimed`, then a prompt ctcensus `FIRED type=Restore`.
  - `[retreat] fill delayed -- own cast in flight`, then `[heal] ... RELEASED ... the retreat filled`.
  - `[retreat-heal]` should only ever be a self heal.
- **Ranged:** a perk testing GetEquippedItemType == 12 biases crossbows only. A LOTD relic crossbow plus a plain bow should not thrash.
- **B63:** a mage set slate lands, and a forced outfit piece is no longer kept.
- **Equip authority:** `[inv-probe]` lines. Check that a refused claim gives `stocked, not equipped -- ... (MFO stays out)` once per weapon.
- **Archetype / summon probe:** read `[archetype]` WARN lines and `[summon-probe]` against `[ctcensus]`.
- **Jesper armor (86e3haxr2):** grep `[prog] ledger <JesperID>` (the dump is from batch 3).
- **Dead target (86e3eb078):** grep `[deadtgt]`.
- **APMF:** generator regen check.

## 4. Files in this folder

- `field-1001-heal-diagnosis.md`: the Opus diagnosis of marth's 2026-10-01 session (Jesper's starved heal, the retreat cutting the heal, LOS refusals). Raw logs: gist 00dbdee941795998185170458824ee57.
- `complex-spells-research.md`: the complex spells task (86e3dmtr1), research and design.
- `equip-authority-audit.md`: status audit of MFO as equip authority (86e3940xu).
- `magicka-research-clickup-comment.md`: magicka research (86e3d6dp0). Still to be posted to ClickUp.
- `clickup-pending.md`: every ClickUp update the cloud could not post (the daily MCP limit was hit). Apply it.

## 5. Backlog and housekeeping

- Docs/REVIEW-BACKLOG.md got new entries this session, B206 to B219. The heal branch adds three more, renumbered at its merge.
- cast/CastOn.cpp is about 1710 lines, past the ~1500 plan-a-split mark. Its split needs its own brief (splitcheck tool).
- APMF task created: [Harbinger sound facet](https://app.clickup.com/t/86e3hbh80). It covers blocking sounds and proxying stereo to mono positional.
