# CONFIRMATION PASS -- 1.5-1.7-address-table.md, re-derived cell by cell

Independent re-derivation of every row of `1.5-1.7-address-table.md` (the original is untouched). Same row
order as the original. Every cell is graded **CONFIRMED** / **WRONG** (with the right value + evidence) /
**UNVERIFIABLE** (why). All addresses are RVAs (VA = `0x140000000` + RVA). No code was changed, nothing was
committed.

Verification state at the time of this pass: MFO `main` = `5e1c41b`, APMF `main` = `0b61290`, held MFO
branches `feat/mfo-1.5.97-forcerefto` (`ab39541`) and `feat/mfo-1.5.97-equipslot` (`04da1d4`).

## 0. Tooling validated before anything was graded

- **`addrlib.py` decoder**: re-run on the brief's vectors. AE 68545 -> `0xcd5650`, 68542 -> `0xcd4f70`,
  38894 -> `0x6c9820`; SE 67245 -> `0xc11c60`, 67315 -> `0xc150b0`, 37938 -> `0x637a80`. All six exact. The
  AE file decodes 428,461 ids (format 2, ver 1.6.1170.0); the SE file 778,674 ids (format 1, ver 1.5.97.0).
  I then re-implemented the decoder from scratch inside my own `cf_lib.py` (same results) so that no
  address-library value below depends on the scan's script.
- **1.7.104 address library**: `find /mnt/gaming/modlists -iname 'version*1-7*'` and a root-wide
  `find / -xdev -iname 'versionlib-1-7*'` return only
  `_commonlib/live-alandtse-ng/tests/REL/versionlib-1-7-99-0.bin` (a 1.7.99 unit-test fixture). **Confirmed:
  no 1.7.104 library exists on this machine.** Every 1.7.104 value below is static analysis only.
- **1.7.104 image**: `unpack/SkyrimSE_17104.unpacked.exe` is md5-identical to
  `binaries/1.7.104/SkyrimSE.exe` (`113faeb71fd8f62b26d0c8627299ab40`), i.e. the scan copied the file as-is.
  Version resource reads `1.7.104.0`. The scan's "no decryption needed" claim is **CONFIRMED by an independent
  test**: Shannon entropy of the first 1 MB of `.text` is 6.03 bits/byte with 32,021 `CC CC CC` padding runs
  (the still-packed AE original measures 8.000 / 0 runs; the decrypted AE and SE images measure 6.01 / 6.06).
  `objdump -d -M intel` disassembles it cleanly at every address used below. `unpack_steamstub.py` does throw
  `IndexError` on it (`codeVA=0`), as the scan says. **One nit**: the scan's "1.7.104's bytes at `.text` file
  offset 0x400 are byte-for-byte identical to 1.6.1170" is true for only **0x14D bytes** (the shared CRT stub);
  it is not evidence of anything beyond "plaintext", which the entropy test proves properly.
- **`rtti_vt.py`**: not trusted. I wrote an independent RTTI walker (`cf_lib.Img.vtables_for_class`:
  TypeDescriptor string -> TD-0x10 -> `_RTTICompleteObjectLocator` with `sig==1`, `pTD` match AND
  `pSelf==own RVA` -> 8-byte absolute pointer to the COL -> vtable = that slot + 8) and, in the other
  direction, `rtti_name_of_vtable` (vt-8 -> COL -> TD -> name) so every AE/SE vtable that the address library
  hands back is checked against its own RTTI name. Raw spot-check with `od` on 1.7.104 `ActorMagicCaster`:
  `[0x18F6150-8] = 0x141BA9F48`; COL `0x1BA9F48` = `{01 00 00 00, 0, 0, pTD=0x2103908, pCHD=0x1BA9F70,
  pSelf=0x1BA9F48}`; TD `0x2103908+0x10` = `".?AVActorMagicCaster@@"`. Chain closes.
- **Raw `objdump -d -M intel` (image base 0x140000000)** was used for at least one cell of every mechanism:
  ToggleControls (all three binaries), PollInputDevices (all three), GetMagicTarget (all three), the
  CombatMagicItemData ctor + slot-1 thunk (all three), the BGSDefaultObjectManager ctor (all three), the
  Papyrus `ReferenceAlias.ForceRefTo` callback (all three), DropObject prologue (all three), the weapon-class
  CalculateScore bodies (SE + AE), the CombatMagicItemData/ControlMap ctors.
- **Method for "same function on AE and SE"** (a recompile, so bytes differ): compare the direct-call
  sequence (each callee resolved to its address-library id), vtable-call slot offsets, immediates and struct
  displacements. Where the two runtimes' call sequences line up id-for-id in the same order with the same
  vtable slots and immediates, the functions are the same function. **Method for 1.7.104**: an AE
  "skeleton" (every rel32 branch target and RIP-relative disp32 wildcarded, everything else exact) must hit
  **exactly one** address in 1.7.104's `.text`, AND that address must be consistent with its neighbours'
  AE->1.7 displacement / ordering, AND (where available) callers/string refs/RTTI must agree. One signal alone
  was never accepted.

---

## 1. MFO address table

| site | 1.6.1170 | 1.5.97 | 1.7.104 | verdict + evidence |
|---|---|---|---|---|
| `Probe.cpp:36` `Actor::StartCombat` `RelocationID(37608, 38561)` | id 38561 -> `0x6B6930` **CONFIRMED** | id 37608 -> `0x6251B0` **CONFIRMED** | `0x6C93B0` **CONFIRMED** | AE/SE decoded from the libraries. Same function: 40 direct/vtable calls on both, in the same order, same vtable slots (`+0x4c8`, `+0x718`, `+0x250`), same immediates (`0x768` twice); the only displacement differences are the AE Actor +8 shift (`[0x158]`->`[0x160]`, `[0x1fc]`->`[0x204]`, `[0x1f0]`->`[0x1f8]`). 9 callers on each runtime. 1.7.104: the full 0x400-byte AE skeleton hits exactly `0x6C93B0`, body 0x498 bytes on both, 9 callers, delta +0x12A80 sits between SetDontMove (+0x12AD0) and EvaluatePackage (+0x12AC0) with ordering preserved. Note the SE prologue is a different register allocation (SE skeleton hits nothing in AE or 1.7), so 1.7 is AE-family here, as the scan said. |
| `Board.cpp:594` `ControlMap::ToggleControls` `RelocationID(67245, 68545)` | id 68545 -> `0xCD5650` **CONFIRMED** | id 67245 -> `0xC11C60` **CONFIRMED** | `0xCEFAA0` **CONFIRMED** | Raw objdump on all three: same algorithm (`r10d = old enabledControls; set/reset by r8b; if r9b and saved!=0x80000000 then set/reset saved; add rcx,8; notify {new,old}`). SE reads `[rcx+0x118]/[rcx+0x11c]`, AE and 1.7 read `[rcx+0x120]/[rcx+0x124]` (see layout row). AE and 1.7 bodies are byte-identical bar the one rel32 (`call 0xCD8550` vs `call 0xCF2B60`); 19 callers on AE and on 1.7, 15 on SE. Neighbour consistency: the ControlMap ctor (AE `0xCD4680`, singleton-store site of id 400863) skeleton-matches uniquely at 1.7 `0xCEEAA0` (delta +0x1A420 vs ToggleControls' +0x1A450; ordering Rumble < ctor < sub < ToggleControls preserved). Signature `(this, flags, bool enable, bool storeState)` confirmed by the `r8b`/`r9b` tests on all three. |
| `Board.cpp:1607-1615` `PollInputDevices` `RelocationID(67315, 68617)` + `VariantOffset(0x7B,0x7B,0x7B)` | id 68617 -> `0xCD8F40` **CONFIRMED** | id 67315 -> `0xC150B0` **CONFIRMED** | `0xCF95F0` **CONFIRMED** (but see NOT READY) | AE and SE bodies are byte-identical apart from relocations (my register-agnostic signature similarity = 1.000). Raw objdump on all three shows the `E8` at `+0x53`, `+0x5B`, `+0x7B`, `+0x87` on every binary (AE `0xCD8F93/…8F9B/…8FBB/…8FC7`, SE `0xC15103/…5110B/…5112B/…51137`, 1.7 `0xCF9643/…964B/…966B/…9677`), each a 5-byte call at an instruction boundary. The call at `+0x7B` is `rcx = this (BSTEventSource base at +0)`, `rdx = &[rsp+0x40]` on all three, which is exactly the `(BSTEventSource<InputEvent*>*, InputEvent**)` shape `InputDispatchHook::thunk` takes. 1.7.104: a byte diff of the whole 0xC9-byte body against AE shows ONLY these differences: loop bound `4`->`6` (+0x1F), three member displacements (`[rax+0x380]`->`[rax+0x558]` twice, `[rsi+0xE0]`->`[rsi+0xF0]` twice, `[rsi+0x88]`->`[rsi+0x98]`), and rel32/RIP relocations. Same 2 callers. The four `+0x53/+0x5B/+0x7B/+0x87` targets each independently match their AE counterparts (next row). The AE->1.7 delta (+0x206B0) is larger than ToggleControls' (+0x1A450) because ~0x6260 bytes of new code sit between them on 1.7; ordering of all seven ControlMap/BSInputDeviceManager functions is preserved. |
| `Board.cpp:1614` sub-row: the 4 internal calls | `+0x53` id 68542 -> `0xCD4F70` **CONFIRMED**; `+0x5B` -> `0xCD42E0` (table: unstated, value supplied); `+0x7B` -> `0xCD9E00` **CONFIRMED**; `+0x87` -> `0xCDACA0` (table: unstated, value supplied) | `+0x53` `0xC11600` **CONFIRMED** (= SE id 67242, the pair of AE 68542 by call-graph identity); `+0x5B` `0xC10860` **CONFIRMED**; `+0x7B` `0xC15E00` **CONFIRMED**; `+0x87` `0xC16C80` **CONFIRMED** | `+0x53` `0xCEF3B0` **CONFIRMED**; `+0x5B` `0xCEE700` **CONFIRMED** (as call target); `+0x7B` `0xCFA650` **CONFIRMED**; `+0x87` `0xCFBA10` **CONFIRMED** (as call target) | All twelve values read off raw objdump of PollInputDevices. `+0x7B` target: AE and 1.7 bodies are identical bar relocations (0x29E bytes, sig 1.000), SE is the same function (same 3 leading callees id-for-id: SE 12210/67353/67351 = AE 13129/68163/68161, same tail-jump shape). `+0x53` target (the ControlMap event-mapping pass): AE and 1.7 have an identical 36-entry call/jump graph and the same ControlMap displacements (`0x108/0x118/0x120/0x128/0x129/0x12a`); 1.7 is 3 bytes longer (stack frame +8, `rsp+0x48`->`rsp+0x50` etc.), no struct offset differs. SE's version reads `0x100/0x118/0x120/0x121/0x122` -- the 17-context layout, as expected. `+0x5B` (Rumble::Update) and `+0x87` (queue reset) are graded only as call targets; the names come from CommonLib's SE comment and were not re-derived. NOTE for 1.7: the `+0x87` target reads `[+0x558]/[+0x560]` where AE/SE read `[+0x380]/[+0x388]` -- `BSInputEventQueue`'s layout changed on 1.7.104 (also visible inside PollInputDevices itself). |
| `Packages.cpp:302-305` `TESQuest::ForceRefTo` `REL::ID(25052)` (AE-gated) / held branch `RelocationID(24523, 25052)` | id 25052 -> `0x3CDEE0` **CONFIRMED** | id 24523 -> `0x375050` **CONFIRMED** | table: `NOT FOUND` -- **RESOLVED in this pass: `0x3D4FA0` CONFIRMED** | AE and SE bodies differ a lot (0x2D1 vs 0x3D1 bytes, sig 0.36) so call-graph identity alone is weak; the decisive evidence is the one the scan cited and I re-ran on raw objdump: the Papyrus `ReferenceAlias.ForceRefTo` native (found by `lea` xref to the `"ForceRefTo"` string: AE `0xA03856` -> callback `0xA03470`, SE `0x96A463` -> `0x96A030`) does `rcx=[r8+0x10] (owningQuest); edx=[r8+0x18] (aliasID); r8=r9 (ref); jmp` to `0x3CDEE0` (AE) / `0x375050` (SE). SE body calls SE id 24537 (`CreateRefHandleByAliasID`, CommonLib's own pin `RELOCATION_ID(24537, 25066)`). **1.7.104** (not attempted by the scan): the same string-xref chain on 1.7 (`"ForceRefTo"` @ `0x199C3A8`, lea at `0xA1BA86`, callback `0xA1B6A0`) tail-jumps to **`0x3D4FA0`**; independently the full 0x2D1-byte AE skeleton hits exactly `0x3D4FA0` (2 callers on both); and the neighbour `CreateRefHandleByAliasID` (AE `0x3D14D0` -> 1.7 `0x3D8590`) has the identical delta +0x70C0. Three independent signals -> CONFIRMED. |
| held branch `Loadout.cpp:63,81` `LookupByID<BGSEquipSlot>(0x00013F43)` | N/A **CONFIRMED (by construction)** | N/A **CONFIRMED (by construction)** | N/A **CONFIRMED (by construction)** | A Skyrim.esm FormID (LeftHand EQUP `0x13F43`; RightHand `0x13F42`, EitherHand `0x13F44`), not a binary fact; the branch cites the DOBJ `LHEQ` value. Cannot be, and need not be, verified against any executable. The engine's own `InitItemImpl` fills `objects[19]` from the same lookup. |
| `Logistics.cpp:709-726` `Actor::DropObject` via vtable slot `0xCB` (VR `0xCD`) | `0x6781D0` **CONFIRMED** | `0x5E6150` **CONFIRMED** | `0x68ACB0` **CONFIRMED** | Read as `VTABLE_Character[0]` slot 0xCB on each binary (Character vtable AE `0x18A5558`, SE `0x165DA40`, 1.7 `0x1923258`, all RTTI-verified). Raw objdump: identical 14-instruction prologue on all three (`mov rax,rsp; push rbp/rsi/rdi/r12-r15; sub rsp,0x90; ...; mov rbx,r9`), 0x41D bytes on AE and 1.7, 0x41B on SE (sig 0.98). Full AE skeleton hits exactly `0x68ACB0` on 1.7. The Character vtable has **298 slots on all three runtimes** (counted to the first non-.text pointer), so no vfunc was inserted or removed before 0xCB. VR slot 0xCD: UNVERIFIABLE (no VR binary; the slot-0xCD function on SE/AE is a different, 0x1E2-byte function -- not claimed by anyone). |
| `CasterConsent.cpp:1053` `VTABLE_ActorMagicCaster[0]` (CheckCast slot 0x0A) | `0x187B8B0` **CONFIRMED** (id 205828) | `0x1637490` **CONFIRMED** (id 257613) | `0x18F6150` **CONFIRMED** | Address library + RTTI name at vt-8 on AE/SE; `RTTI_ActorMagicCaster` ids (394312/686479) resolve to the same TypeDescriptors the COLs point at. 1.7: my walk returns exactly one primary vtable, raw `od` chain above. Slot 0x0A on all three has the same 5-arg prologue (`rdx`=MagicItem, `r8b`=dualCast, `r9`=effectStrength*, `[rsp+0x28]`=reason*); the 1.7 body is a mild recompile (0x570 vs 0x5A8 bytes, first 0x60 bytes identical, sig 0.96). 30 slots on all three. |
| `CasterConsent.cpp:1087-1095` 14 seat vtables | see sub-table **all CONFIRMED** | **all CONFIRMED** | **all CONFIRMED** | 14/14 decoded from the address library on AE and SE, RTTI name at vt-8 equals `.?AVCombatMagicCaster<seat>@@` on both, `RTTI_*` ids point at the same TDs; 14/14 unique on 1.7 by my own walk; 14 slots on every seat vtable on all three. Slot 0x06 (CheckStartCast) is a distinct implementation per seat on all three; Restore's 0x06/0x07 bodies are AE==1.7 byte-identical (skeleton MATCH). |
| `CombatStyle.cpp:384-417` `CombatInventoryItemMagicT<...>` vtables | see sub-table **all CONFIRMED** | **all CONFIRMED** | **all CONFIRMED** | 28/28 as the table lists, same method. **The table undercounts: `CombatStyle.cpp:384-417` (and APMF `EquipGate.cpp:654-681` / `AiCastSeats.cpp:1127-1152`) hook 30 vtables, not 28 -- the two `..._CombatMagicCasterArmor_` instantiations are missing from the sub-table.** Supplied below. |
| `CombatStyle.cpp:374` `CombatMagicCasterArmor` | `0x18CD4D0` **CONFIRMED** (id 211222) | `0x1687268` **CONFIRMED** (id 265023) | `0x194BCF0` **CONFIRMED** | Real RTTI class on all three, as the scan says; the pinned CommonLib DOES carry `VTABLE_CombatMagicCasterArmor{265023, 211222}` and `RTTI_CombatMagicCasterArmor`, so the "no class" wording in the code comment means "no C++ wrapper", exactly as the scan flagged. Its slot 0x06 is a 0x61-byte function shared in shape by five seats on 1.7 (the skeleton hits five addresses, one of which is the slot value `0x837140`). |
| `MainThread.cpp:103` `VTABLE_PlayerCharacter[0]` | `0x18AB9C0` **CONFIRMED** (id 208040) | `0x16635E0` **CONFIRMED** (id 261916) | `0x19296C0` **CONFIRMED** | Address library + RTTI on AE/SE, unique walk on 1.7. Slot 0xAD (`Actor::Update`, the MFO/APMF hook seat) is a large function on all three; AE and 1.7 versions are a real recompile (0x14D8 vs 0x14D1 bytes, sig 0.95) -- the index is stable (vtable slot counts equal), the body is not identical. |
| `Targeting.cpp:162` `VTABLE_Character[0]` | `0x18A5558` **CONFIRMED** (id 207886) | `0x165DA40` **CONFIRMED** (id 261397) | `0x1923258` **CONFIRMED** | Same method. Slot 0xE4 (`UpdateCombat`): AE==1.7 skeleton MATCH, SE sig 0.91. Slot 0x49 (APMF's `CheckForCurrentAliasPackage`): AE==1.7 MATCH, SE 0.93. Slot 0xDF (`PutCreatedPackage`): same 12-callee sequence id-for-id on AE/SE, AE==1.7 MATCH. 298 slots on all three. |

### MFO sub-table: 14 CombatMagicCaster seat vtables -- every cell CONFIRMED

All 42 cells re-derived: AE/SE from `VTABLE_CombatMagicCaster<seat>[0]` ids in the pinned `Offsets_VTABLE.h`,
RTTI name checked at vt-8; 1.7 by independent RTTI walk (unique hit each). Values identical to the original
table for all 14 x 3 (Offensive `0x18CC4A0/0x1686CA0/0x194ACC0` ... Reanimate `0x18D5318/0x1687390/0x19538E0`).
AE->1.7 `.rdata` deltas all lie in +0x7E820..+0x7E848 except Reanimate (+0x7E5C8), consistent with the
neighbouring rows.

### MFO sub-table: 28 (really 30) CombatInventoryItemMagicT combos -- every cell CONFIRMED, two rows missing

All 28 x 3 cells match my re-derivation exactly (AE/SE via the pinned
`VTABLE_CombatInventoryItemMagicT_CombatInventoryItem{Magic|Staff}_CombatMagicCaster<seat>_` ids -- these ARE
bound in CommonLib 3.7.0, so no RTTI guesswork was needed on AE/SE; 1.7 by walk with the mangled form
`.?AV?$CombatInventoryItemMagicT@VCombatInventoryItem{Magic|Staff}@@VCombatMagicCaster<seat>@@@@`, unique
each). Slot 0x0C (CalculateScore, shared base impl) is AE==1.7 byte-identical; slot 0x0F (CheckShouldEquip) is
a 0x42-byte per-instantiation function on all three.

**Missing from the original table (WRONG by omission -- both are hooked at `CombatStyle.cpp:398,413`,
APMF `EquipGate.cpp` and `AiCastSeats.cpp`):**

| item base | seat | AE | SE | 1.7.104 |
|---|---|---|---|---|
| Magic | Armor | `0x18CDA98` (id 211254) | `0x168C0A8` (id 265335) | `0x194C2B8` |
| Staff | Armor | `0x18CD800` (id 211234) | `0x168BF28` (id 265333) | `0x194C020` |

### MFO layout facts -- re-counted from the disassembly on each binary

| fact | 1.6.1170 | 1.5.97 | 1.7.104 | what I read |
|---|---|---|---|---|
| `ControlMap` context count / member offsets | **18 contexts** -- **CONFIRMED** | **17 contexts** -- **CONFIRMED** | **18 contexts** -- **CONFIRMED** | Read from the ControlMap **constructor** (the function that stores the singleton: AE `0xCD4680` = id 68534, SE `0xC10DC0` = id 67234, 1.7 `0xCEEAA0` by unique skeleton). Each ctor zero-fills `controlMap[]` at `this+0x60` with `memset(..., 0, N)`: SE `mov r8d,0x88` (= 17 x 8), AE `mov r8d,0x90` (= 18 x 8), 1.7 `mov r8d,0x90` (= 18 x 8). The same ctors initialise `enabledControls = 0xFFFFFFFF` at `+0x118` (SE) / `+0x120` (AE, 1.7), the saved-state word `= 0x80000000` at `+0x11C` / `+0x124`, `textEntryCount` at `+0x120` / `+0x128`, `ignoreActivateDisabledEvents` at `+0x122` / `+0x12A`; `linkedMappings` at `+0xE8` / `+0xF0`, `contextPriorityStack` at `+0x100` / `+0x108`. ToggleControls' own `[rcx+0x118/0x11c]` vs `[rcx+0x120/0x124]` accesses (raw objdump above) agree. The table's cells are right; the loop bound is now stated from the ctor, not inferred from ToggleControls. |
| `BSInputDeviceManager::devices[]` count | **4** -- **CONFIRMED** (`mov edi,0x4` at `0xCD8F5E`) | **4** -- **CONFIRMED** (`mov edi,0x4` at `0xC150CE`) | **6** -- **CONFIRMED** (`mov edi,0x6` at `0xCF960E`) | Loop is `rbx = this+0x60; do { if ([rbx]) [rbx]->vfunc2(dt); rbx += 8 } while (--rdi)` on all three (raw objdump). Additional 1.7 facts the table did not state: every BSInputDeviceManager member after `devices[]` moves by +0x10 (`+0x88` -> `+0x98`, `+0xE0` -> `+0xF0`), and the `BSInputEventQueue` member PollInputDevices reads moves `+0x380` -> `+0x558`. |
| `CombatController::attackerHandle @0x28`, `targetHandle @0x2C`, `combatStyle @0x38` (all `< 0x68`) | **CONFIRMED by direct read** | **CONFIRMED by direct read** | **CONFIRMED by direct read** | Not by proxy: (a) `CombatMagicCaster::GetMagicTarget` (slot 0x0A, AE `0x81E020`, SE helper `0x782100`, 1.7 `0x833510`) reads `[cc+0x28]` (attacker) and `[cc+0x2c]` (target) on all three, and reads the cached-target block at `[cc+0xC8/0xD0/0xD8]` on AE and 1.7 but at `[cc+0xC0/0xC8/0xD0]` on SE -- i.e. the AE +8 divergence above 0x68 is real on AE and carried into 1.7, while 0x28/0x2C are unshifted. (b) `combatStyle @0x38`: the combat-style score-multiplier helper (AE `0x819D30` = id 45085, SE `0x77F9B0` = id 43866, 1.7 `0x82EC10`, called from the equip selector) does `mov rax,[rcx+0x38]; movss xmm0,[rax+0x30|0x38|0x40]` (magic/shout/staff score mults in `TESCombatStyle::generalData`) identically on all three. (c) `CombatAimController` ctor stores `[+0x28] = CombatController*` and zeroes the aim override `[+0x30]` on all three, and the projectile aim vfunc7 reads `[rcx+0x30]` first on all three -- APMF `CastSeats.cpp:108`'s raw `+0x30` holds on SE and 1.7 as well. |
| `BGSDefaultObjectManager::objects[]` / `objectInit[]` count | **366** -- **CONFIRMED** | **364** -- **CONFIRMED** | table: not scanned -- **RESOLVED: 372** | Read from the ctor (the function that stores the `BGSDefaultObjectManager` vtable, RTTI-located: AE `0x3151EC`, SE `0x2C07FC`, 1.7 `0x31B84C`; all set form type `0x31` = DOBJ). Each does `memset(this+0x20, 0, N*9)`: AE `mov r8d,0xCDE` (366 x 9), SE `mov r8d,0xCCC` (364 x 9), 1.7 `mov r8d,0xD14` (372 x 9); the allocation sizes at the `new` sites are `0xD00` / `0xCF0` / `0xD38` respectively. So `objectInit[]` starts at `+0xB90` (AE), `+0xB80` (SE), `+0xBC0` (1.7). CommonLib 3.7.0's `IsObjectInitialized` reads `+0xB80` on both SE and AE (`RelocateMember<bool*>(this, 0xB80, 0xBA8)` -- VR gets 0xBA8): right on SE, 16 bytes early on AE (reads the last two `objects[]` pointers as bools), and 64 bytes early on 1.7. `objects[i]` itself starts at `+0x20` on all three, so a DIRECT `objects[19]/[20]` read (what APMF `EquipGate.cpp:227-240` does) is layout-safe on all three **provided the enum index means the same thing** -- which I could not prove for 1.7 (372 entries; whether the 6 new ones are appended or inserted is not visible statically without the DOBJ record order). |
| `GetMagicTarget` sret (the brief lists this; the table has no row) | **CONFIRMED** `Out16* (this, Out16* out, CombatController*)` | **CONFIRMED** same ABI | **CONFIRMED** same ABI | Raw objdump. AE/1.7 (byte-identical): `rdi=rdx (out); rbx=r8 (cc); ...; [rdi]=eax (handle); [rdi+8]=rcx (ptr); rax=rdi`. SE `0x781CB0`: `rbx=rdx; r8=[rcx+0x18]; rdx=cc; rcx=out; call 0x782100; rax=rbx` and the helper fills the same 16-byte `{u32 handle @0; ptr @8}` (`[rsp+0x30]/[rsp+0x38]` copied to the out block). The hidden out-slot is present on all three; APMF's `Out16` (`CastSeats.cpp:262`, `AiCastSeats.cpp:905`) matches all three. It is the base impl shared by 14 of the 15 caster vtables on each runtime (the 15th, Armor, has its own). |

---

## 2. APMF address table

### "Six single-version REL::ID sites" -- the actual count

`grep -rn 'REL::ID(' native/` on APMF `main` (`0b61290`) returns **zero** hits, and it returned zero at
`77681f4` too (the commit that wrote the "six" sentence into `CLAUDE.md`, 2026-09-09, a mirror of MFO's
rule 11 -- MFO has exactly one `REL::ID(`, `Packages.cpp:305`). So the sentence never described a `REL::ID`
count. What DOES exist in APMF `native/` as single-runtime constructs (file:line at `0b61290`):

1. `core/AiCastSeats.cpp:345-348` `kWeaponClasses[]` -- 4 x (vtableRva, calcScoreRva) raw AE RVAs, consumed
   via `REL::Offset` at `:1224-1225`, **gated `if (!REL::Module::IsAE())` at `:1218`** (refuses SE).
2. `core/CastClassify.cpp:53-55` `kSpellOffset/kCtrlOffset/kSelfFlagOffset` -- 3 raw member offsets,
   **gated `!IsAE()` at `:246`** (refuses SE). The vtable itself is a proper `VariantID(265000, 211955)` at `:31`.
3. `core/CastSeats.cpp:108` `kAimTargetOverride = 0x30` -- raw CombatAimController offset, AE-verified per the
   banner, **no runtime gate** (INI + RTTI identity only). Runs on SE today.
4. `core/CastSeats.cpp:262` / `core/AiCastSeats.cpp:905` `Out16` -- the sret shape, AE-verified (0x81e020),
   **no runtime gate**. Runs on SE today.
5. `core/EquipGate.cpp:180-189` `CallSiteName()` -- 5 raw AE return-address RVAs (`0x80fcd0`, `0x813af2`,
   `0x813d38`, `0x814270`, `0x8144b2`), log-label only; prints "unknown" elsewhere. No gate needed.
6. `core/NonAliasProbe.cpp:79` `kPutCreatedPackage = 0xDF` -- a vtable index annotated "1.6.1170-pinned",
   VR-refused only. Runs on SE today.

That is six mechanisms, which is probably what the sentence meant; none is a `REL::ID`. **Does it matter
for placement? Only items 1 and 2 do** -- they are the only sites whose `IsAE()` gate would need to open for
SE, and both now have confirmed SE values (Group C below; the three offsets are byte-identical on SE and
1.7). Items 3, 4 and 6 already run on SE ungated and this pass confirms all three hold on SE (and 1.7). Item 5
is cosmetic.

### Plain-function sites

| site | 1.6.1170 | 1.5.97 | 1.7.104 | verdict + evidence |
|---|---|---|---|---|
| `channels/MovementDeny.cpp:42` `Actor::SetDontMove` `RELOCATION_ID(36490, 37489)` | id 37489 -> `0x6750D0` **CONFIRMED** | id 36490 -> `0x5E3330` **CONFIRMED** | `0x687BA0` **CONFIRMED** | Same function AE/SE: identical shape (`call X; ...; jmp Y; jmp X` with AE callees 37612/37817 = SE 36604/36801), same immediates (`0x20`), the single displacement differs only by the Actor +8 shift (`[0xc0]`->`[0xc8]`). 0x50 bytes on all three; AE skeleton hits exactly `0x687BA0`; 1 caller on each; delta +0x12AD0 consistent with StartCombat/EvaluatePackage/KeepOffset. |
| `channels/MovementDeny.cpp:52` `Actor::KeepOffsetFromActor` `RELOCATION_ID(36870, 37894)` | id 37894 -> `0x697110` **CONFIRMED** | id 36870 -> `0x603990` **CONFIRMED** | `0x6A9C90` **CONFIRMED** | AE/SE: same 3-call shape (AE 41734 / vslot 0x18 / 42174 = SE 40723 / 0x18 / 41095), same NiPoint3 copy sequence (`+0x28..+0x48`, `+0x90/+0x98`), the smart-pointer member at `[0x148]` (SE) vs `[0x150]` (AE/1.7). AE skeleton hits exactly `0x6A9C90`, 0x139 bytes, 1 caller each. |
| `channels/MovementDeny.cpp:58` `Actor::ClearKeepOffsetFromActor` `RELOCATION_ID(36871, 37895)` | id 37895 -> `0x697250` **CONFIRMED** | id 36871 -> `0x603AC0` **CONFIRMED** | `0x6A9DD0` **CONFIRMED** | Same single callee (AE 41734 = SE 40723, the same helper KeepOffsetFromActor calls first), same `[0x148]/[0x150]` member, same `0x30` frame. AE skeleton hits exactly `0x6A9DD0`; sits 0x140 after KeepOffsetFromActor on both AE and 1.7, 0x130 after on SE. 0 direct callers on every runtime (reached via a vtable or tail), same on AE. |
| `core/PackageGate.cpp:341` `Actor::EvaluatePackage(bool,bool)` `RELOCATION_ID(36407, 37401)` | id 37401 -> `0x66CCF0` **CONFIRMED** | id 36407 -> `0x5DB310` **CONFIRMED** | `0x67F7B0` **CONFIRMED** | CommonLib itself binds this exact pair (`src/RE/A/Actor.cpp:252`). AE/SE call graphs line up id-for-id across 40 entries (vslots `0x4c8`, `0x728`, `0x250`, `0x6f0` identical; immediates `0x3e`, `0x3ff`, `0x89`, `0x11` identical; only the Actor +8 shift differs). AE skeleton (0x400 bytes) hits exactly `0x67F7B0`; 16 callers on AE and 1.7, 15 on SE; delta +0x12AC0. |

### Group C: weapon-class CalculateScore seats -- every cell CONFIRMED

| weapon class | field | 1.6.1170 | 1.5.97 | 1.7.104 | evidence |
|---|---|---|---|---|---|
| Melee | vtable | `0x18C9028` **CONFIRMED** (id 210297) | `0x1681A88` **CONFIRMED** (id 264523) | `0x19475C8` **CONFIRMED** | **The pinned CommonLib binds these**: `VTABLE_CombatInventoryItemMelee{264523, 210297}` and `RTTI_CombatInventoryItemMelee{687592, 395442}` exist in 3.7.0's `Offsets_VTABLE.h`/`Offsets_RTTI.h` (likewise Ranged 264525/210299, Shield 264527/210301, Torch 264531/210305). So SE needs no RTTI-walk at all; the address library gives it. The scan's "no Address-Library ID exists" reading of the banner is wrong on that point (the banner is right that there is no C++ *class*). AE literal in source reproduced; RTTI name at vt-8 correct on AE/SE; 1.7 unique walk. |
| Melee | CalculateScore (slot 0x0C) | `0x8183E0` **CONFIRMED** | `0x77E0A0` **CONFIRMED** | `0x82D2C0` **CONFIRMED** | Slot read on all three; AE==1.7 skeleton MATCH (0xEE bytes). **SE is a different compilation shape**: 0x5D bytes -- it calls a helper (`0x77E110`) that AE inlines, then applies the same subtract. Same function, different inlining. |
| Ranged | vtable | `0x18C90D8` **CONFIRMED** | `0x1681B58` **CONFIRMED** | `0x1947678` **CONFIRMED** | as above |
| Ranged | CalculateScore | `0x8188B0` **CONFIRMED** | `0x77E550` **CONFIRMED** | `0x82D790` **CONFIRMED** | 0x18 bytes, byte-identical on all three (sig 1.00). |
| Shield | vtable | `0x18C9188` **CONFIRMED** | `0x1681C28` **CONFIRMED** | `0x1947728` **CONFIRMED** | as above |
| Shield | CalculateScore | `0x818DF0` **CONFIRMED** | `0x77EAC0` **CONFIRMED** | `0x82DCD0` **CONFIRMED** | AE==1.7 MATCH (0xEB bytes). **SE `0x77EAC0` is a 0xF-byte arg-swapping thunk** (`rax=rdx; rdx=[rcx+0x10]; rcx=rax; jmp 0x77EAE0`) into the real scorer that AE inlines into the vfunc. The slot value IS the class's own CalculateScore entry, which is what APMF's install gate compares; the body just is not comparable to AE's. |
| Torch | vtable | `0x18C92E8` **CONFIRMED** | `0x1681DD0` **CONFIRMED** | `0x1947888` **CONFIRMED** | as above |
| Torch | CalculateScore | `0x819480` **CONFIRMED** | `0x77F0E0` **CONFIRMED** | `0x82E360` **CONFIRMED** | AE==1.7 MATCH (0xB2 bytes). SE `0x77F0E0` is the same 0xF-byte thunk shape into `0x77F100`. |

### CastClassify.h SEAT 0 (`CombatMagicItemData` vtable slot 1)

| site | 1.6.1170 | 1.5.97 | 1.7.104 | verdict + evidence |
|---|---|---|---|---|
| `core/CastClassify.cpp:31` `kCombatMagicItemDataVtable` `VariantID(265000, 211955, ...)` | id 211955 -> `0x18D5120` **CONFIRMED** | id 265000 -> `0x1686BF8` **CONFIRMED** | `0x19536E8` **CONFIRMED** | Address library + RTTI `.?AVCombatMagicItemData@@` at vt-8; the pinned CommonLib also carries `RTTI_CombatMagicItemData{687623, 395938}` (the code comment "No RTTI_CombatMagicItemData Address-Library ID was established" is stale -- it is in 3.7.0's `Offsets_RTTI.h`). 2 slots on all three. |
| slot 1 (classify thunk) + the three raw offsets `0x10/0x18/0x4c` | `0x81D830` **CONFIRMED**; offsets **CONFIRMED** | table: "not checked" -- **RESOLVED: `0x7811F0` (SE id 43931) and the three offsets are IDENTICAL on SE: CONFIRMED** | `0x832D20` **CONFIRMED**; offsets **CONFIRMED** | Raw objdump. The **constructor** (the single `lea` to the vtable: AE `0x81D5BC`, SE `0x780F5C`, 1.7 `0x832AAC`) stores `[this+0x10] = rdx` (the MagicItem), `[this+0x18] = r8` (the CombatController), then `[this+0x4d] = (spell->vfunc(0x2a8) == 2)` and `[this+0x4c] = sete(spell->vfunc(0x2b8) == ...)` -- byte-for-byte the same instruction sequence on all three. The slot-1 thunk reads `[r13+0x4c]` (this->selfFlag) twice on all three and takes `rdx` = the effect item (reads `[rdx+0x10]` -> `[+0xC0]/[+0xC4]` archetype/AV) identically. AE==1.7 body byte-identical (0x1A6 bytes); SE is a mild recompile (0x1B0 bytes, sig 0.92) with the same three `this` offsets. |

### Vtable/RTTI families shared with MFO

Re-tabulated above (14 seats, 30 items, Character, PlayerCharacter, ActorMagicCaster). All CONFIRMED on all three.

### New for APMF

| site | 1.6.1170 | 1.5.97 | 1.7.104 | verdict + evidence |
|---|---|---|---|---|
| `core/CastSeats.cpp:499,522` `VTABLE_CombatProjectileAimController[0]` / `RTTI_CombatAimController` | `0x18C59A8` **CONFIRMED** (id 210081) / `0x18C5888` = `VTABLE_CombatAimController[0]` (id 210075) -- **the table labels this cell `RTTI_CombatAimController`; the value is the VTABLE, not the RTTI TypeDescriptor. WRONG label, right number.** `RTTI_CombatAimController` (id 395363) is a `.data` TypeDescriptor, not this `.rdata` address. | `0x167E2A8` **CONFIRMED** (id 264187) / `0x167E130` **CONFIRMED as VTABLE_CombatAimController** (id 264181; same label error) | `0x1943F48` **CONFIRMED** / `0x1943E28` **CONFIRMED as the CombatAimController vtable** | What `CastSeats.cpp:522` actually resolves is `REL::Relocation<void*> aimTD{ RE::RTTI_CombatAimController }` -- a TypeDescriptor pointer for `DerivesFrom`; that value is NOT what the table wrote. The vtables themselves: address library + RTTI on AE/SE, unique walk on 1.7, 11 slots each on all three. `DerivesFrom(Projectile vtable, CombatAimController TD)` holds on all three (the projectile COL's class-hierarchy array contains the CombatAimController TD). |
| `core/EquipGate.cpp:652`, `core/AiCastSeats.cpp:1126,1183` `RTTI_CombatInventoryItem` | `0x18C8EC8` (id 210293) -- **same label error: this is `VTABLE_CombatInventoryItem[0]`**, CONFIRMED as such | `0x1681928` (id 264521) CONFIRMED as the vtable | `0x1947468` CONFIRMED as the vtable | `RTTI_CombatInventoryItem{687590, 395440}` resolves to the TypeDescriptor the COL of this vtable points at (checked on AE/SE), so the family identity is right. |
| `core/AiCastSeats.cpp:1184` `RTTI_CombatMagicCaster` | `0x18D5150` (id 211959) -- **same label error: `VTABLE_CombatMagicCaster[0]`**, CONFIRMED as such | `0x1686C28` (id 265002) CONFIRMED as the vtable | `0x1953718` CONFIRMED as the vtable | `RTTI_CombatMagicCaster{687618, 395744}` points at this vtable's TD on AE/SE. |
| `core/ActionGate.cpp:501-589` base `RTTI_CombatBehaviorTreeNode` + the 70 `kLeaves[]` | base `0x18D9138` (id 212199) -- **same label error: `VTABLE_CombatBehaviorTreeNode[0]`**, CONFIRMED as such; the 72 `VariantID` triples in `core/CombatBehaviorRE.h` (70 in `kLeaves`): **all 72 AE ids CONFIRMED** | base `0x1692510` (id 265775) CONFIRMED as the vtable; **all 72 SE ids CONFIRMED** | base `0x1957700` CONFIRMED; table: `NOT FOUND` for the leaves -- **RESOLVED: all 72 leaf vtables CONFIRMED on 1.7.104** (appendix A) | The mangled names are NOT "complex template instantiations that need individual recovery" in any hard sense: every one is `.?AV?$CombatBehaviorTreeNodeObject@V<Leaf>@@@@` (or `...ChildSelector@V<Policy>@@@@`, `...TreeCreateContextNode...`) and the exact string is sitting at the TypeDescriptor each AE/SE vtable already points at. Method: decode the SE and AE id from each triple; read the RTTI name at vt-8 on both (identical string on SE and AE for all 72); walk the class-hierarchy array to prove each derives from `CombatBehaviorTreeNode` (72/72 on SE, 72/72 on AE); then walk 1.7 with that exact string (72/72 unique hits) and prove derivation against 1.7's own `CombatBehaviorTreeNode` TD (`0x2101980`, 72/72). AE->1.7 `.rdata` deltas all in +0x7A0E0..+0x7E5F0, ordering preserved. |

---

## 3. Tallies

Counting one cell per (row x runtime) over the rows above -- 13 MFO named-site rows, 14 seat rows, 30 combo
rows (28 + 2 supplied), 5 layout rows (4 + the GetMagicTarget row the brief asked for), 4 APMF plain-function
rows, 8 Group C rows, 2 CastClassify rows, 4 "new for APMF" rows (72 leaves counted as one row) = **80 rows**:

| runtime | CONFIRMED | WRONG | UNVERIFIABLE | notes |
|---|---|---|---|---|
| 1.6.1170 | 80 | 0 numeric; **4 label errors** (the four `RTTI_*` cells hold VTABLE addresses) + **2 rows omitted** (Armor combos) + 2 unstated sub-row values supplied | 0 | every AE value traces to the address library or a reproduced source literal |
| 1.5.97 | 80 | 0 numeric; same 4 label errors; 1 "not checked" cell resolved (CastClassify slot 1 = `0x7811F0`, offsets identical) | 0 | |
| 1.7.104 | 80 | 0 numeric; same 4 label errors | 0 -- but **4 previously-NOT FOUND cells are now resolved** (ForceRefTo `0x3D4FA0`, BGSDefaultObjectManager `372`, 72 leaf vtables, CastClassify offsets) | the DropObject VR slot 0xCD stays UNVERIFIABLE (no VR binary), as does the 1.7 DOBJ enum-index meaning |

Nothing in the original table is numerically wrong. The defects are: four mislabelled cells (VTABLE written
where the code resolves an RTTI TypeDescriptor), two hooked vtables missing from the 28-row sub-table, an
overstated "byte-for-byte identical at 0x400" unpack remark, and the "no Address-Library ID / no RTTI_ id"
readings for `CombatInventoryItem{Melee,Ranged,Shield,Torch}` and `CombatMagicItemData` (all are bound in the
pinned 3.7.0 headers).

## 4. Regression check -- would placing a 1.5 value REGRESS 1.6?

- Every AE id in every `RelocationID`/`VariantID` in both codebases decodes to the address the code was
  verified against; none is wrong today, so pairing a 1.5 id next to it changes nothing on 1.6.
- `VariantOffset(0x7B, 0x7B, 0x7B)` is genuinely +0x7B on SE, AE and 1.7 (raw objdump) -- not a value that
  differs per runtime.
- `Packages.cpp` ForceRefTo: replacing `REL::ID(25052)` + `IsAE()` with the held branch's
  `RelocationID(24523, 25052)` + `IsAE()||IsSE()` keeps AE on the identical address. Safe.
- Group C: opening the `:1218` gate for SE must keep the AE literals as they are and add SE literals (or,
  better, switch to the CommonLib `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}[0]` VariantIDs, which
  resolve to the same four AE addresses -- verified -- and remove the raw AE RVAs entirely). The
  `calcScoreRva` install-time gate then needs per-runtime expected values (`0x77E0A0/0x77E550/0x77EAC0/0x77F0E0`
  on SE) because the SE entries are thunks, not the AE bodies. No AE change either way.
- CastClassify offsets: identical on SE; opening the `:246` gate does not touch AE.
- Nothing in the table would open a path that is unverified on 1.6.

## (A) PLACEMENT READY

**MFO 1.5.97**
- `Probe.cpp:36` StartCombat -- already `RelocationID(37608, 38561)`; both halves confirmed. Nothing to place.
- `Board.cpp:594` ToggleControls -- already `RelocationID(67245, 68545)`; confirmed, with the `0x118/0x11C`
  vs `0x120/0x124` layout confirmed from the ctor. Nothing to place.
- `Board.cpp:1607-1615` PollInputDevices `+0x7B` -- `RelocationID(67315, 68617)` + `VariantOffset(0x7B,...)`
  confirmed on SE by raw objdump (E8 at `0xC1512B`, rcx=this, rdx=&event). Nothing to place.
- `Packages.cpp:302-305` ForceRefTo -- the held branch `feat/mfo-1.5.97-forcerefto` value
  `RelocationID(24523, 25052)` is confirmed (SE `0x375050`, Papyrus-callback tail-jump proven on raw objdump).
  May be hand-placed.
- held branch `Loadout.cpp` `LookupByID<BGSEquipSlot>(0x00013F43)` -- layout-independent by construction; the
  364-vs-366 `objectInit` divergence it routes around is confirmed from the DOBJ ctor. May be placed.
- `Logistics.cpp:726` DropObject slot 0xCB -- confirmed on SE (298-slot Character vtable, identical prologue).
  Nothing to place.
- `CasterConsent.cpp`, `CombatStyle.cpp`, `MainThread.cpp`, `Targeting.cpp` -- every `VTABLE_*` resolves on
  SE through the address library and was checked; slot 0x0A / 0x06 / 0x0F / 0xAD / 0xE4 ABI shapes confirmed.
  Nothing to place.

**APMF 1.5.97**
- `channels/MovementDeny.cpp:42,52,58` and `core/PackageGate.cpp:341` -- already `RELOCATION_ID(se, ae)`
  pairs; all four SE halves confirmed. Nothing to place.
- `core/CastClassify.cpp:53-55` -- the three offsets `0x10/0x18/0x4c` are byte-identical on SE (ctor and
  thunk read on raw objdump); slot 1 = `0x7811F0`. The `:246` `!IsAE()` refusal may be widened to SE.
- `core/AiCastSeats.cpp:345-348` Group C -- SE vtables `0x1681A88/0x1681B58/0x1681C28/0x1681DD0` and
  CalculateScore entries `0x77E0A0/0x77E550/0x77EAC0/0x77F0E0` confirmed; preferably placed as the pinned
  `VTABLE_CombatInventoryItem{Melee,Ranged,Shield,Torch}[0]` VariantIDs (ids 264523/264525/264527/264531 SE,
  210297/210299/210301/210305 AE) with per-runtime `calcScoreRva` expectations. The `:1218` gate may open for SE.
- `core/CastSeats.cpp:108` `+0x30` aim override and `:262` `Out16` sret -- both confirmed on SE by raw
  objdump; they already run ungated on SE, now with evidence.
- `core/NonAliasProbe.cpp:79` slot 0xDF -- same vfunc on SE (12-callee sequence id-for-id). Already runs.
- `core/ActionGate.cpp` 72 leaf triples -- all SE ids confirmed + derivation proven. Already shipped.

**1.7.104 CONFIRMED (hand-placeable as literals only -- there is no library, so these would have to be
`REL::Offset` constants behind a `1.7.104` version check; whether to ship such a path at all is marth's call,
not this pass's):** every 1.7 cell above -- the 8 plain functions incl. ForceRefTo `0x3D4FA0`, all 56+16
vtables in the tables, the 72 leaf vtables (appendix A), the four PollInputDevices call targets, the
CombatMagicItemData offsets, `combatStyle@0x38`/`attackerHandle@0x28`, the `+0x30` aim override, the
GetMagicTarget sret shape, `objects[372]`.

## (B) NOT READY -- stays gated with a loud log line

- **MFO `Board.cpp:1600` input trampoline on 1.7.104.** The `+0x7B` call is at `+0x7B` on 1.7 and the target
  is confirmed, but `BSInputDeviceManager` grew (`devices[6]`, members +0x10) and `BSInputEventQueue` moved
  (`+0x380` -> `+0x558`); `InputSink::Feed` walks `InputEvent`/`ButtonEvent` fields through CommonLib's
  1.5/1.6 headers and nothing here proves those structs are unchanged on 1.7 (the mapping pass `0xCEF3B0`
  reads the same event-field displacements as AE, which is indicative, not proof). Keep the
  `!isAE1170 && !isSE597` refusal.
- **MFO `Packages.cpp` ForceRefTo on 1.7.104** -- confirmed value, but shipping it needs a `REL::Offset`
  literal path that does not exist in either codebase's conventions; gate stays until that is a decided design.
- **APMF Group C on 1.7.104** -- same: values confirmed, no library, and `WeaponClassSpec` is
  `IsAE()`-gated by design. Stays refused with its existing warn line.
- **Everything resolved through `RE::VTABLE_*` / `RelocationID` on 1.7.104** -- the scan's central finding is
  correct and confirmed by reading `REL/Relocation.h:1605-1612`: with no `versionlib-1-7-104-0.bin` every one
  of these resolves to a null relocation. No 1.7 placement is possible through the existing constructs; a 1.7
  path is a design decision (raw-offset table vs. a hand-built address library), not a placement.
- **`BGSDefaultObjectManager::GetObject`/`IsObjectInitialized` on ANY runtime through CommonLib 3.7.0** --
  the `+0xB80` read is wrong on AE (366) and 1.7 (372); MFO's `LookupByID` route-around is the right call. APMF
  `EquipGate.cpp:227` reads `objects[]` directly and is safe on SE/AE; on 1.7 the enum index meaning is
  unproven (372 entries).
- **DropObject VR slot 0xCD** -- no VR binary; unreachable today (`MainThread::IsInstalled()` guard). Unchanged.
- **`ActorMagicCaster::CheckCast` and `Actor::Update` bodies on 1.7.104** are real recompiles (0x570 vs 0x5A8;
  0x12C0 vs 0x12B1). The slot indices are stable (equal slot counts), the arg shapes match, but any hook
  that relies on the AE body's internal behaviour (not just its ABI) has no proof on 1.7.

## Appendix A -- the 72 CombatBehaviorTree leaf vtables on all three runtimes

Every AE/SE value decoded from the `VariantID` triple in `core/CombatBehaviorRE.h`, RTTI name at vt-8 identical
on SE and AE and derived from `CombatBehaviorTreeNode`; 1.7.104 by RTTI walk with that exact name, unique hit,
derivation re-proven against 1.7's base TD.

| leaf (CombatBehaviorRE.h name) | SE id -> RVA | AE id -> RVA | 1.7.104 (RTTI walk, exact mangled name) |
|---|---|---|---|
| `CombatBehaviorAdvance` | 266096 -> `0x16960b0` | 212694 -> `0x18dc6c0` | `0x195ac88` |
| `CombatBehaviorAttack` | 266747 -> `0x169e5a0` | 213789 -> `0x18e4048` | `0x1962610` |
| `CombatBehaviorAttackFromCover` | 267194 -> `0x16a37a8` | 214395 -> `0x18e8c80` | `0x1967270` |
| `CombatBehaviorAttackLow` | 266654 -> `0x169d210` | 213640 -> `0x18e2da8` | `0x1961370` |
| `CombatBehaviorBackoff` | 266103 -> `0x1696580` | 212785 -> `0x18dcb90` | `0x195b158` |
| `CombatBehaviorBash` | 265986 -> `0x1694dd8` | 212595 -> `0x18db618` | `0x1959be0` |
| `CombatBehaviorBlock` | 265987 -> `0x1694e88` | 212608 -> `0x18db6c8` | `0x1959c90` |
| `CombatBehaviorBlockAttack` | 265985 -> `0x1694d28` | 212582 -> `0x18db568` | `0x1959b30` |
| `CombatBehaviorCastConcentrationSpell` | 266707 -> `0x169dd98` | 213735 -> `0x18e3868` | `0x1961e30` |
| `CombatBehaviorCastImmediateSpell` | 266705 -> `0x169dc90` | 213720 -> `0x18e3760` | `0x1961d28` |
| `CombatBehaviorCastShout` | 267116 -> `0x16a28e0` | 214280 -> `0x18e7e60` | `0x1966450` |
| `CombatBehaviorChase` | 266336 -> `0x16991f0` | 213109 -> `0x18df408` | `0x195d9d0` |
| `CombatBehaviorCheckUnreachableTarget` | 266866 -> `0x169f9c8` | 213917 -> `0x18e5410` | `0x19639d8` |
| `CombatBehaviorChildSelector_ConditionalChildSelector_` | 266094 -> `0x1695f50` | 212668 -> `0x18dc560` | `0x195ab28` |
| `CombatBehaviorChildSelector_RandomValueChildSelector_` | 266100 -> `0x1696370` | 212746 -> `0x18dc980` | `0x195af48` |
| `CombatBehaviorChildSelector_ValueChildSelector_` | 265919 -> `0x1693db0` | 212424 -> `0x18da640` | `0x1958c08` |
| `CombatBehaviorCircle` | 266102 -> `0x16964d0` | 212772 -> `0x18dcae0` | `0x195b0a8` |
| `CombatBehaviorCircleDistant` | 266098 -> `0x1696210` | 212720 -> `0x18dc820` | `0x195ade8` |
| `CombatBehaviorDiveBomb` | 266624 -> `0x169c930` | 213551 -> `0x18e25b8` | `0x1960b80` |
| `CombatBehaviorDodgeThreat` | 265954 -> `0x16947b8` | 212546 -> `0x18dafd0` | `0x1959598` |
| `CombatBehaviorDrinkPotion` | 267213 -> `0x16a3b40` | 214430 -> `0x18e8f80` | `0x1967570` |
| `CombatBehaviorDynamicConditionalNode` | 265918 -> `0x1693d00` | 212411 -> `0x18da590` | `0x1958b58` |
| `CombatBehaviorEquipObject` | 265920 -> `0x1693e60` | 212437 -> `0x18da6f0` | `0x1958cb8` |
| `CombatBehaviorEquipRangedWeapon` | 265921 -> `0x1693f10` | 212450 -> `0x18da7a0` | `0x1958d68` |
| `CombatBehaviorEquipShout` | 265924 -> `0x16940c8` | 212478 -> `0x18da958` | `0x1958f20` |
| `CombatBehaviorEquipSpell` | 265922 -> `0x1693fc0` | 212463 -> `0x18da850` | `0x1958e18` |
| `CombatBehaviorExitWater` | 266868 -> `0x169fb28` | 213943 -> `0x18e5570` | `0x1963b38` |
| `CombatBehaviorFallback` | 266101 -> `0x1696420` | 212759 -> `0x18dca30` | `0x195aff8` |
| `CombatBehaviorFallbackSelector_NextChildSelector_` | 256698 -> `0x162fbd0` | 205291 -> `0x1873240` | `0x18ed320` |
| `CombatBehaviorFallbackSelector_WeightedRandomChildSelector_` | 266621 -> `0x169c720` | 213512 -> `0x18e23a8` | `0x1960970` |
| `CombatBehaviorFallbackToRanged` | 266095 -> `0x1696000` | 212681 -> `0x18dc610` | `0x195abd8` |
| `CombatBehaviorFindAllyAttackLocation` | 266196 -> `0x16978c0` | 212925 -> `0x18dddb0` | `0x195c378` |
| `CombatBehaviorFindAttackLocation` | 266195 -> `0x1697810` | 212912 -> `0x18ddd00` | `0x195c2c8` |
| `CombatBehaviorFindCover` | 267190 -> `0x16a3598` | 214365 -> `0x18e8a70` | `0x1967060` |
| `CombatBehaviorFindLateralAttackLocation` | 266194 -> `0x1697760` | 212899 -> `0x18ddc50` | `0x195c218` |
| `CombatBehaviorFindWeapon` | 265838 -> `0x1692f60` | 212262 -> `0x18d99d0` | `0x1957f98` |
| `CombatBehaviorFlank` | 266335 -> `0x1699140` | 213096 -> `0x18df358` | `0x195d920` |
| `CombatBehaviorFlankDistant` | 266337 -> `0x16992a0` | 213122 -> `0x18df4b8` | `0x195da80` |
| `CombatBehaviorFlee` | 266497 -> `0x169aeb8` | 213321 -> `0x18e0ea0` | `0x195f468` |
| `CombatBehaviorFleeThroughDoor` | 266495 -> `0x169ad58` | 213295 -> `0x18e0d40` | `0x195f308` |
| `CombatBehaviorFleeToAlly` | 266494 -> `0x169aca8` | 213282 -> `0x18e0c90` | `0x195f258` |
| `CombatBehaviorFleeToCover` | 266496 -> `0x169ae08` | 213308 -> `0x18e0df0` | `0x195f3b8` |
| `CombatBehaviorFlyingAttack` | 266626 -> `0x169ca90` | 213577 -> `0x18e2718` | `0x1960ce0` |
| `CombatBehaviorForceFail` | 267085 -> `0x16a22a0` | 214228 -> `0x18e7928` | `0x1965f18` |
| `CombatBehaviorForceSuccess` | 266506 -> `0x169b3e0` | 213405 -> `0x18e13c8` | `0x195f990` |
| `CombatBehaviorGroundAttack` | 266622 -> `0x169c7d0` | 213525 -> `0x18e2458` | `0x1960a20` |
| `CombatBehaviorHide` | 266502 -> `0x169b178` | 213364 -> `0x18e1160` | `0x195f728` |
| `CombatBehaviorHover` | 266623 -> `0x169c880` | 213538 -> `0x18e2508` | `0x1960ad0` |
| `CombatBehaviorLand` | 266631 -> `0x169ccf8` | 213609 -> `0x18e2980` | `0x1960f48` |
| `CombatBehaviorMaintainOptimalRange` | 266933 -> `0x16a0630` | 214013 -> `0x18e5e58` | `0x1964420` |
| `CombatBehaviorOrbit` | 266620 -> `0x169c670` | 213499 -> `0x18e22f8` | `0x19608c0` |
| `CombatBehaviorOrbitDistant` | 266619 -> `0x169c5c0` | 213486 -> `0x18e2248` | `0x1960810` |
| `CombatBehaviorParallel` | 265772 -> `0x1692468` | 212197 -> `0x18d9068` | `0x1957630` |
| `CombatBehaviorPause` | 265925 -> `0x1694178` | 212491 -> `0x18daa08` | `0x1958fd0` |
| `CombatBehaviorPerchAttack` | 266625 -> `0x169c9e0` | 213564 -> `0x18e2668` | `0x1960c30` |
| `CombatBehaviorPrepareDualCast` | 266703 -> `0x169db88` | 213705 -> `0x18e3658` | `0x1961c20` |
| `CombatBehaviorPursueTarget` | 266655 -> `0x169d2c0` | 213653 -> `0x18e2e58` | `0x1961420` |
| `CombatBehaviorRangedAttack` | 256699 -> `0x162fc80` | 205304 -> `0x18732f0` | `0x18ed3d0` |
| `CombatBehaviorRepeat` | 256697 -> `0x162fb20` | 205278 -> `0x1873190` | `0x18ed270` |
| `CombatBehaviorReposition` | 266099 -> `0x16962c0` | 212733 -> `0x18dc8d0` | `0x195ae98` |
| `CombatBehaviorReturnToCombatArea` | 266867 -> `0x169fa78` | 213930 -> `0x18e54c0` | `0x1963a88` |
| `CombatBehaviorSearchInvestigateDoor` | 267086 -> `0x16a2350` | 214241 -> `0x18e79d8` | `0x1965fc8` |
| `CombatBehaviorSequence` | 265837 -> `0x1692eb0` | 212249 -> `0x18d9920` | `0x1957ee8` |
| `CombatBehaviorSpecialAttack` | 266746 -> `0x169e4f0` | 213776 -> `0x18e3f98` | `0x1962560` |
| `CombatBehaviorStalk` | 266334 -> `0x1699090` | 213083 -> `0x18df2a8` | `0x195d870` |
| `CombatBehaviorStrafe` | 266934 -> `0x16a06e0` | 214026 -> `0x18e5f08` | `0x19644d0` |
| `CombatBehaviorSurround` | 266097 -> `0x1696160` | 212707 -> `0x18dc770` | `0x195ad38` |
| `CombatBehaviorTakeoff` | 266617 -> `0x169c460` | 213460 -> `0x18e20e8` | `0x19606b0` |
| `CombatBehaviorTrackTarget` | 266500 -> `0x169b018` | 213338 -> `0x18e1000` | `0x195f5c8` |
| `CombatBehaviorWaitBehindCover` | 267193 -> `0x16a36f8` | 214382 -> `0x18e8bd0` | `0x19671c0` |
| `CombatBehaviorTreeCreateContextNode1_CombatBehaviorContextMagic` | 266702 -> `0x169dad8` | 213692 -> `0x18e35a8` | `0x1961b70` |
| `CombatBehaviorTreeCreateContextNodeBase_CombatBehaviorContextMagic` | 550933 -> `0x169da80` | 213681 -> `0x18e3550` | `0x1961b18` |

## 5. ATTACK-SELECTION PASS (2026-09-21) -- NPC attack pick chain, all ids from the Address Library, layout facts from the images

Source: APMF `Docs/SPEC-ATTACK-SELECTION-FACET.md` (Fable design pass) + the coordinator's byte reads on the
UNPACKED binaries (below). RVAs; VA = 0x140000000 + RVA. "AE" = 1.6.1170, "SE" = 1.5.97.

**Unpacked binaries are PERMANENT now (marth 2026-09-21):** `binaries/1.6.1170/SkyrimSE.unpacked.exe`,
`binaries/1.5.97/SkyrimSE.unpacked.exe` (SteamStub v3.1.2 decrypted, layout preserved, .text va 0x1000 raw
0x400 so raw = 0x400 + rva - 0x1000). `binaries/1.7.104/SkyrimSE.exe` is plaintext as shipped (stub flags
0x6 = NoEncryption). Tools: `Linux-Native-Tools/tools/steamstub-rtti/`. Never re-derive "no unpacked binary".

**TRAP -- Address Library ids can resolve to a 5-byte `jmp rel32` THUNK (E9 + INT3 pad), not the body.
Follow the jump before reading a prologue/signature.** Verified 2026-09-21:
| Symbol | AE id -> rva | AE body | SE id -> rva | SE body |
|---|---|---|---|---|
| IAnimationGraphManagerHolder::NotifyAnimationGraph impl (vfunc 01) | 38048 -> 0x6a35f0 (thunk) | **0x54c450** | 37020 -> 0x60f240 (thunk) | **0x4f12c0** |
| CombatBehaviorContextMelee StartAttack | 49170 -> 0x8a28b0 (body, `48 8b c4 55 41 54 41 55`) | same | 48139 -> 0x80c020 (body, `48 8b c4 55 56 57 41 54`) | same |
| CombatAnimation::Execute (= "PerformAttackAction" target, = APMF T4 crash addr module+0x7F9470) | 44440 -> 0x7f9470 | same | 43239 -> 0x75ff10 | same |
| GatherAttackData (SE only) | NOT FOUND | | 48145 -> 0x80d1d0 (body) | |
| CheckAttack (SE only) | NOT FOUND | | 48140 -> 0x80c6f0 (body) | |

**CombatAnimation::Execute bytes on BOTH runtimes are `48 8b 01 48 ff 60 28` = `mov rax,[rcx]; jmp [rax+0x28]`:
a pure virtual tail-jump to slot 5 (0x28/8) of whatever object rcx points at (TESActionData::Process).** The
design pass graded seat S4 (`VTABLE_TESActionData` slot 5) as "devirtualised on the AI path" -- this byte
read says the CALL is virtual; what remains unproven is which VTABLE the AI's by-value TESActionData carries
(standard VTABLE_TESActionData AE 188603 / SE 232777, or a derived one). Brief A step 0 must settle it from
the unpacked image before S4 is discarded. Do not re-derive; start from these bytes.

**Vanilla chain ids (AE / SE):** VTABLE_Character[3] (IAnimationGraphManagerHolder sub-vtable, COL offset
0x38): 207892 (vt 0x18a5ee0) / 261400 (vt 0x165e3c8); VTABLE_Actor[3]: 207517 / 260541 (same impl).
CombatBehaviorAttack leaf act/pop/update: 49199/49202/49213 / 48171/48174/48188. SpecialAttack
49200/49203/49214 / 48172/48175/48189. Bash 47861/47865/47881 / 46660/46664/46685. BlockAttack
47863/47867/47883 / 46662/46666/46687. Melee context node2 (`CombatBehaviorTreeCreateContextNode2<
CombatBehaviorContextMelee, MemberFunc<EquipContext::GetItem>, ATTACK_TYPE{WeaponRight,Shield,WeaponLeft}>`)
act/pop: 49198/49201 (vt 0x18e3ee8 id 213763) / 48170/48173 (vt 0x169e440 id 266745). Block context node2
47860/47864 / 46659/46663. VTABLE_TESActionData 188603 (0x178c478) / 232777 (0x1548198). VTABLE_BGSAttackData
200768 / 252900. SE-only (AE NOT FOUND): CheckAttackRange 48141, FinishedAttack 48142, CalculateAnimationData
48146. `BGSAttackData::attackChance` @+4 (pinned B/BGSAttackData.h:27). StartAttack call sites:
GetAttackAngle +0x3F1 SE / +0x493 AE; PerformAttackAction +0x4D7 SE / +0x435 AE (E8 bytes NEEDS-READ at
0x8a2ce5 AE / 0x80c4f7 SE -- SCAR and Valhalla patch these, which is why the #17a seat is DISQUALIFIED).

**Frameworks:** SCAR.dll (AE support) rewrites the two StartAttack sites (ids 48139/49170 loaded as
0xbc0b/0xc012) and vtable-writes VTABLE_Character[2] slot 1 (BSTEventSink<BSAnimationGraphEvent>::ProcessEvent,
ids 261399/207890) = Hook_AttackCombo. Valhalla Combat patches the same PerformAttackAction site. BFCO.dll hooks
only PlayerControls. Every framework's NPC attack ends in NotifyAnimationGraph with a vanilla ATKE string.

**Race ATKD (Tuxborn Skyrim.esm):** DefaultRace 0x19 = 8 ATKE, no Left/Dual. ImperialRace 0x13744 / NordRace
0x13746 = 27 entries incl. attackStartLeftHand / attackStartDualWield / attackPowerStartDualWield at 1.0.
CSTY DATA flags: csThalmorMeleeDual 0x5, csMercerFreyMelee 0x6, csHumanMelee_AllD 0x1, MFO_MeleeStyle 0x5.


## Two AE 1.6.1170 Address Library files exist: pick by EXE BUILD (2026-09-22)

`versionlib-1-6-1170-0.bin` serves exe build **1.6.1170.0**, `versionlib-1-6-1170-0-1.bin` serves exe build
**1.6.1170.1**. They are not interchangeable: decoding ids with the wrong one lands about 0x930 off, i.e.
mid-function, which reads as a plausible address instead of failing loudly.
`binaries/1.6.1170/SkyrimSE.unpacked.exe` is **1.6.1170.0**, so use `-0.bin` for every offline id decode here.
Self-check before trusting any AE id: `NotifyAnimationGraph` id 38048 must resolve to `0x6A35F0`
(a `jmp rel32` thunk to the body at `0x54C450`). Only `-0.bin` gives that.

**At RUNTIME this cannot bite us.** CommonLib builds the filename from the running exe's own version string
(`REL/Relocation.h:1205-1214`, `versionlib-{version.string()}.bin`) and then rejects a file whose header
version does not match the exe (`load_file`: "version mismatch" -> `report_and_error`). So the game always
loads the library that matches its own build, or refuses. This is purely an OFFLINE ANALYSIS trap: it bites an
agent decoding ids by hand against an unpacked binary, which is exactly how it was found (agentlog
`apmf-combat-engage.md`).

## ADDENDUM 2026-09-24 — mit-3.7 F1: the fork corrections and the seat self-check

The per-seat expected RVAs now live in **`Docs/VERIFIED-ADDRESSES.md`** (generated, with `native/VerifiedAddresses.h`,
by `tools/verified_addresses/gen_verified_addresses.py` from our unpacked 1.6.1170.0 and 1.5.97.0 images). That file is
the current seat audit; the rows above stay as the review record of how each value was first graded. The file:line
citations above predate this commit and have drifted.

Corrections to this table, each proven in the named fork commit (`marthofdoom/CommonLibSSE-NG` `mit-3.7`):

- **`BGSDefaultObjectManager` row (line 104): the "what CommonLib reads" note was wrong.** 3.7.0's
  `GetObject(DefaultObjectID)` / `IsObjectInitialized(size_t)` did not read "the last two objects[] pointers as bools"
  and were not "right on SE". `RelocateMember<T>` returns `T&`, so they loaded the qword AT +0xB80 and used it as the
  flag-array pointer, and loaded `objects[0]` as the object-array base. 1.6.1170 read garbage (the 2026-08-01
  GetGoldAmount CTD); 1.5.97 faulted on `0x0101010101010101`. The layout facts in the row (366 / +0xB90, 364 / +0xB80)
  are right. Also: 1.6.1170 inserts HMCC (363) and HMAE (364) before MHFL (365); 1.5.97 has MHFL at 363. Fixed in
  fork `05d05aab`.
- **`ControlMap` (line 101): which context is new.** 1.6.1170's 18th context is index 16 ("Creations Menu" in its
  controlmap.txt), so kFavor is 17 there. Contexts 0..15 are unchanged (menu ctors write the same inputContext:
  Favorites 6, Map 7, Book 10, Journal 12, Lockpicking 15; IMenu kNone 0x12 on 1.5.97, 0x13 on 1.6.1170). Fork `5b80d53d`.
- **`CombatController` members from +0x68**: 1.5.97 ctor `0x4FCA00` (id 32467, new 0xD8), 1.6.1170 ctor `0x558070`
  (id 33214, new 0xE0) with 8 new bytes at +0x68. Fork `a6613ba7` puts them behind `GetRuntimeData()`.
- **`GetMagicTarget` sret**: the Reanimate override (1.6.1170 `0x8222C0`, 1.5.97 `0x785F60`) has the same out-slot shape.
  Fork `4065b48d` declares the return as `CombatMagicCaster::MagicTarget` by value.
- **`SKSE::log::log_directory` AE id**: 3.7.0's 380738 is not in `versionlib-1-6-1170-0.bin`; the real variable is
  id 502114 (`0x20123B0` -> "Skyrim Special Edition"). Fork `aef05fac`.
- **An id missing from the library is now fatal** (fork `d9ad1072`). Offline, every id MFO and APMF use is present in
  both libraries.

## ADDENDUM 2026-09-24 (mit-3.7 F1b): the form lookup lock

3.7.0's `TESForm::LookupByID` and `LookupByEditorID` wrote `const BSReadWriteLock l{ lock };`. That copies the lock
and holds nothing, so every lookup read the game's form maps unlocked. Fork `bf7e9a5d` holds a `BSReadLockGuard` on
the real lock instead. Registry `3.7.0#4` (`038638a5`) serves it. All values below were read on our unpacked
1.6.1170.0 and 1.5.97.0 images with `versionlib-1-6-1170-0.bin` and `version-1-5-97-0.bin`.

| What | 1.6.1170 id | 1.6.1170 RVA | 1.5.97 id | 1.5.97 RVA |
|---|---|---|---|---|
| all-forms map pointer | 400507 | `0x20FBB88` | 514351 | `0x1EC3CB8` |
| all-forms lock | 400517 | `0x20FC018` | 514360 | `0x1EC4150` |
| editor-id map pointer | 400509 | `0x20FBFE0` | 514352 | `0x1EC3CC0` |
| editor-id lock | 400518 | `0x20FC020` | 514361 | `0x1EC4158` |
| game's own form lookup | 14617 | `0x1E01A0` | 14461 | `0x194230` |
| game's own editor-id lookup | 14618 | `0x1E0250` | 14462 | `0x1942E0` |
| `BSReadWriteLock::LockForRead` | 68233 | `0xCC90C0` | 66976 | `0xC072D0` |
| `BSReadWriteLock::UnlockForRead` | 68239 | `0xCC9380` | 66982 | `0xC07590` |
| `BSReadWriteLock::LockForWrite` | 68234 | `0xCC9140` | 66977 | `0xC07350` |
| `BSReadWriteLock::UnlockForWrite` | 68240 | `0xCC9390` | 66983 | `0xC075A0` |

- **The game reads under the read lock.** Its lookup is the same on both builds:
  `lea rsi,[forms lock]; call LockForRead; <hash find>; mov rcx,rsi; call UnlockForRead`. The editor-id lookup does
  the same on the editor-id lock. The fork now does exactly this.
- **The lock.** Two dwords: the writer's thread id at +0, a count at +4 with bit 31 as the write bit.
  `LockForRead` calls `GetCurrentThreadId`. If this thread is the writer it just adds one. Otherwise it adds one
  with a compare-exchange, but only while bit 31 is clear, and it spins with `Sleep(0)` then `Sleep(1)` while a
  writer holds it. `UnlockForRead` is one `lock dec`. Readers leave no owner and writers get no preference. So a
  thread that already holds the lock, read or write, can read again and cannot deadlock itself.
- **What the game does under the write lock.** Form construction (1.6.1170 14593, 1.5.97 14438 at `0x191E10`) holds
  it only around a hash insert and its grow. Form removal (1.6.1170 14627, 1.5.97 14471) takes the forms lock and
  then the editor-id lock, and only erases. Nothing inside calls out. A reader only waits out one insert, erase or grow, and
  a writer never waits on anything a reader could hold.
- **The fork interns the editor-id key before it takes the lock.** No other lock is ever taken while the read lock is
  held.
- **Self-check.** `LockForRead` and `UnlockForRead` are now rows in `tools/verified_addresses/spec.json`
  (`CommonLib.BSReadWriteLock.*`). They log a failure by name at startup. They gate nothing, because the lookup
  lives inside CommonLib.
