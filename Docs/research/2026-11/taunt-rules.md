# Taunt + power-attack cooldown rules (rulings Q69 / Q72 / Q73)

Branch `feat/taunt-rules`. Rulings are in `open-questions-for-marth.md`.

- **One formula (Q72).** `Iaus::Estimate::TauntChance` (`native/iaus/Estimate.h`, pure) = clamp(0.5 + 0.05 x (taunter - foe), 0.1, 0.9). `Taunt.cpp` Resolve rolls with it.
- **Taunt cooldown (Q69).** 30 s per taunter from the attempt (stamped when the idle is filed), hit or miss. Stored in the shared Cooldowns store as `Kind::Follower` / `kFollowerTaunt`. `Taunt::Fire` checks it and is TRANSPARENT while cooling; the IAUS taunt candidate reads and stamps the same slot (`iaus/Engine.cpp` `CdOf`), and its rowless floor is 30 s (`Tuning::rowlessTauntCooldownSec`). Toggling IAUS mid-fight cannot give a second taunt. Duration stays `fTauntDuration` (15 s default, `Config.cpp`).
- **Power-attack cooldown (Q73).** Before: one cooldown per ROW uid (`Kind::Rule`), so several power rows stacked, and IAUS had its own `(family, posture, form)` id. Now one slot per follower (`Kind::Follower` / `kFollowerPowerAttack`) for every `act.power_attack` row (`scheduler/CombatScan.cpp` `CooldownKey`) and for the IAUS power-attack candidate. Values (row 10 s, rowless floor 10 s) unchanged. A power row with no cooldown of its own still stamps the slot, so rows that declare one see it.
- **Transparent skips (no idle, no cooldown).** No foe in reach fighting an ally or the player; every such foe already pinned by ANY taunter (Gather now skips a foe pinned by anyone). Not pre-checkable: "no foe can sense him" (Harbinger `SenseActor` is main-thread only), so that case still ends at the roll (Resolve), after the idle and with the cooldown started.
- **Pinned by anyone.** `Taunt::IsPinned(foe)`; `iaus/Candidates.cpp` fills `Combatant::taunted` from it, so two knights cannot double-taunt one foe in IAUS mode either.
- **Logs.** `[taunt] skip: <reason> (<id>): <detail>`, one per (taunter, reason) per 10 s: `cooling down`, `every foe already taunted`, `no foe to taunt`.
- No co-save change (Cooldowns is runtime-only, cleared by `ResetAllState`).
