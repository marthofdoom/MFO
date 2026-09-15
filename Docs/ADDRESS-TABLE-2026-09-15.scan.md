# MFO + APMF: 1.6.1170 / 1.5.97 / 1.7.104 address table

SCAN ONLY. No code was changed. All addresses are RVAs (add `0x140000000` for the runtime VA,
consistent with SKSE/CommonLib convention). Read the methodology section before trusting any
1.7.104 cell -- **there is no address library for 1.7.104**, so every 1.7.104 value in this
document was obtained by independent static analysis (RTTI walk or byte-pattern matching), never
by looking anything up in a version database. Anywhere that analysis did not succeed the cell
says `NOT FOUND` -- never a guess.

## 0. Validation, tool status, unpack status

**addrlib.py decoder validated exactly** against the brief's test vectors:
- AE (`versionlib-1-6-1170-0.bin`): id 68545 -> `0xCD5650` ✓, id 68542 -> `0xCD4F70` ✓, id 38894 -> `0x6C9820` ✓ (`ActorEquipManager::EquipObject`)
- SE (`version-1-5-97-0.bin`): id 67245 -> `0xC11C60` ✓, id 67315 -> `0xC150B0` ✓, id 37938 -> `0x637A80` ✓

**1.7.104 address library: does not exist.** `find /mnt/gaming/modlists -iname 'versionlib-1-7*'` and
`-iname 'version-1-7*'` found nothing under any modlist. The only 1.7.x hit anywhere on the machine is
`/mnt/gaming/modlists/Projects/_commonlib/live-alandtse-ng/tests/REL/versionlib-1-7-99-0.bin` -- a unit-test
fixture for **1.7.99**, a different build, not authoritative for 1.7.104 and not used.

**1.7.104 unpack status: no decryption was needed.** `binaries/1.7.104/SkyrimSE.exe`'s SteamStub header
(signature `0xC0DEC0DF`, same family as AE/SE) has `flags=0x6`, `codeVA=0`, `codeSize=0` -- the fields the
unpack script uses to find the AES-encrypted code span are simply empty for this build. Cross-checked by
diffing raw bytes at `.text`'s file offset (`0x400`): 1.7.104's bytes there are **byte-for-byte identical**
to the known-good, AES-decrypted 1.6.1170 reference, and `.rdata`'s RTTI mangled-name strings
(`.?AVActor@@`, full `CombatBehaviorTreeValueNodeT<...>` templates, etc.) are plainly readable. So this
build's `.text`/`.rdata` ship plaintext already; the SteamStub wrapper apparently only re-does import-table
restoration on this build, not code encryption. The file was copied as-is to
`.../scratchpad/unpack/SkyrimSE_17104.unpacked.exe` and disassembles cleanly with capstone.

**Central architectural finding (read this before reading any row below).** Every
`REL::RelocationID` / `REL::ID` / `REL::VariantID` / `RELOCATION_ID` / `RE::VTABLE_*` /
`RE::RTTI_*` site in both codebases resolves through CommonLibSSE-NG's `IDDatabase`
(`include/REL/Relocation.h:1605-1612`, confirmed by reading the pinned 3.7.0 source directly), which loads
the **version-matched** `versionlib-*.bin` file at runtime. `RE::VTABLE_*` arrays
(`include/RE/Offsets_VTABLE.h`) are themselves just `REL::VariantID` constants -- **not** live RTTI scans, contrary
to what an earlier pass of this same investigation assumed. Since no `versionlib` file exists for 1.7.104,
**every one of these constructs resolves to 0 / a null relocation on 1.7.104 today**, full stop, regardless
of whether the call is "version-resilient by vtable index" in the source comments. The only things that
work on 1.7.104 out of the box are: hand-rolled RTTI walks that dereference already-resolved live pointers
(e.g. APMF's `Allowance::DerivesFrom`), and raw `REL::Offset(literal)` constants -- and literal constants
are exactly what today's code does NOT have for 1.7.104.

**New tool built for this scan and cross-validated 5x: `rtti_vt.py`** (in the scratchpad). Given a class's
demangled name, it statically finds the class's primary vtable via the standard MSVC x64 RTTI chain with
**no address library needed**: locate the `TypeDescriptor` name string in `.rdata` -> back up 0x10 bytes to
the `TypeDescriptor` struct start -> scan `.rdata` for a `_RTTICompleteObjectLocator` (`sig==1`, `pTypeDescriptor`
matching, and a **self-referential `pSelf` check**, which is what makes false positives essentially
impossible) -> scan `.rdata`/`.data` for the 8-byte absolute pointer to that COL, which sits immediately
before the vtable's function-pointer array. Verified against the address library **five times** with exact
matches (`ActorMagicCaster[0]` on AE and SE, `Character[0]` on AE and SE, `CombatMagicCasterOffensive[0]` on
AE) before being trusted for 1.7.104. This is why every "RTTI walk" row below is graded HIGH, not MED --
it's not a guess about ABI stability, it's the same technique, proven, applied to a third binary.

**Byte-pattern-matching methodology for plain (non-RTTI) functions.** Where AE and SE bodies are
byte-identical (register-allocation and all), a short unique byte substring is enough to anchor. Where AE
and SE differ in register allocation (a real recompile, not just relinking), an exact match against SE is
abandoned and the AE body is used instead, since 1.7.104 was observed (StartCombat, ToggleControls,
CheckStartCast, DropObject -- 4/4 so far) to be **byte-for-byte identical to AE**, never to SE, everywhere
this was checked. Each such match was disambiguated to a single unique hit in 1.7.104's `.text` and then
independently confirmed by a full-body diff against the AE reference (differences localized only to
RIP-relative/rel32 displacement fields, never to opcode/ModRM/immediate-constant bytes) before being
reported HIGH.

---

## 1. MFO address table

| site (file:line) | what | 1.6.1170 (id -> RVA) | 1.5.97 (id -> RVA) | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|---|
| `Probe.cpp:36` | `Actor::StartCombat(actor,target,nullptr)` (po3's published id pair, unbound in CommonLib) | id 38561 -> `0x6B6930` | id 37608 -> `0x6251B0` | `0x6C93B0` | addrlib exact match (AE/SE); 1.7.104: AE and SE differ in register allocation (real recompile), so matched against AE only -- 30-byte fixed prefix + a disambiguating `mov ecx,0x768` at the identical +0x3c offset gave one unique hit; full-body diff vs AE shows every differing byte is a RIP-relative immediate, body otherwise byte-identical | HIGH |
| `Board.cpp:594` | `ControlMap::ToggleControls(this, USER_EVENT_FLAG, bool enable, bool storeState)` -- called instead of trusting CommonLib's header (AE offset bug, see layout facts below) | id 68545 -> `0xCD5650` | id 67245 -> `0xC11C60` | `0xCEFAA0` | addrlib exact match (AE/SE, both are the brief's validation vectors); 1.7.104: 12-byte fixed prefix incl. wildcarded disp32 gave ONE unique hit in the whole binary; full 0x8f-byte body is **byte-for-byte identical** to AE including register-allocation choices (`r8d` vs `eax` branch) | HIGH |
| `Board.cpp:1607-1615` | `PollInputDevices`, hooked at func+0x7B (`InputDispatchHook`, `write_call<5>`) | id 68617 -> `0xCD8F40` | id 67315 -> `0xC150B0` | `0xCF95F0` | addrlib exact match (AE/SE); 1.7.104: a 24-byte exact prefix (loop-entry setup) was unique in the whole binary; loop now runs **6** iterations, not 4 (see layout fact below) but the loop body bytes are unchanged length so every downstream call offset still lines up exactly: +0x53/+0x5B/+0x7B/+0x87 all landed on `call rel32` instructions at those exact same relative offsets, matching the AE/SE fingerprint the existing MFO comment already documents | HIGH |
| `Board.cpp:1614` (sub-row) | PollInputDevices' 4 internal calls: +0x53 mapping pass (`ControlMap::sub_...`, id 68542 on AE), +0x5B `Rumble::Update`, **+0x7B = the hook target** ("emit last InputEvent"/BSTEventSource notify), +0x87 "reset BSInputEventQueue" | +0x53 id 68542 -> `0xCD4F70`; +0x5B/+0x7B/+0x87 not id-sourced, read from the +0x7B/PollInputDevices disasm directly (`0xCD9E00` is +0x7B's target) | +0x53 `0x140C11600` ("ControlMap::sub_140C11600"); +0x5B `0x140C10860` ("Rumble::Update"); +0x7B `0x140C15E00`; +0x87 `0x140C16C80` (all named in the existing MFO comment, sourced from CommonLib's own BSInputDeviceManager.cpp:147-150 comment) | +0x53 -> `0xCEF3B0`; +0x5B -> `0xCEE700`; +0x7B (hook target) -> `0xCFA650`; +0x87 -> `0xCFBA10` | all 4 read directly off the PollInputDevices disassembly at `0xCF95F0+{0x53,0x5B,0x7B,0x87}`, each a clean 5-byte `E8 rel32` at an instruction boundary (next instruction starts exactly 5 bytes later, same as AE/SE) | HIGH |
| `Packages.cpp:302-305` + held branch `feat/mfo-1.5.97-forcerefto` (ab39541) `Packages.cpp:320` | `TESQuest::ForceRefTo(aliasID, refr)` native (AE-only in shipped `main`; branch extends to SE) | id 25052 -> `0x3CDEE0` | (held branch) id 24523 -> `0x375050` | `NOT FOUND` | addrlib exact match on both; SE value independently reproduces the held branch's own hand-verified `0x375050` exactly. **Extra evidence already in the branch, independent of the decoder:** SKSE 2.2.6's own `GameForms.h:1704` declares AE offset `0x3CDEE0` for this exact runtime; the engine's own Papyrus native `ReferenceAlias.ForceRefTo` callback (SE `0x96A030`, AE `0xA03470`) loads `owningQuest`(+0x10)->rcx, `aliasID`(+0x18)->edx, `ref`->r8 and **tail-jumps** to `0x375050`(SE)/`0x3CDEE0`(AE) -- proves both the id and the `(TESQuest*,uint32,TESObjectREFR*)` signature on both runtimes independent of the address library. SE body also calls `CreateRefHandleByAliasID` at `0x378760` = SE id 24537 (CommonLib's own pin). 1.7.104: not attempted -- no distinctive byte anchor was found cheaply and this function's shape was not checked; genuinely open | AE/SE: HIGH. 1.7.104: N/A (not attempted) |
| held branch `feat/mfo-1.5.97-equipslot` (04da1d4) `Loadout.cpp:63,81` | `RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x00013F43)` -- left-hand EQUP, DOBJ `LHEQ` | N/A -- FormID | N/A -- FormID | N/A -- FormID | a FormID lives in the plugin's own ESP/data handler (`Skyrim.esm`, load index 00), not the engine binary; resolves identically on every runtime by construction. Replaces the broken `BGSDefaultObjectManager::GetObject` path -- see the layout-fact row below | HIGH (by definition, no disassembly needed) |
| `Logistics.cpp:709-726` | `Actor::DropObject`, called via raw vtable slot `0xCB` (`0xCD` on VR, unreachable/unverified) on `VTABLE_Actor[0]`/`VTABLE_Character[0]` | `0x6781D0` (already in MFO's own comment) | `0x5E6150` (already in MFO's own comment) | `0x68ACB0` | `rtti_vt.py` independently reproduced BOTH pre-existing AE/SE values exactly via `VTABLE_Actor[0]`+slot arithmetic; 1.7.104's slot-0xCB target disassembles **byte-for-byte identical** to AE (same register allocation: `rdi=rcx`(this), `rsi=rdx`(&result), `r15=r8`(object), `rbx=r9`(extraList), matching the exact ABI note already in the comment) | HIGH |
| `CasterConsent.cpp:1053` | `VTABLE_ActorMagicCaster[0]` (CheckCast hook, slot 0x0A) | `0x187B8B0` | `0x1637490` | `0x18F6150` | `rtti_vt.py`: TD string `.?AVActorMagicCaster@@` -> self-referential COL -> vtable, exact match to addrlib on AE+SE (ids 205828/257613) | HIGH |
| `CasterConsent.cpp:1087-1095` | 14 concrete `CombatMagicCaster*` seat vtables, CheckStartCast hook at slot 0x06 | see sub-table | see sub-table | see sub-table | `rtti_vt.py`, cross-checked against addrlib for `Offensive` (AE 211104/SE 265003) | HIGH |
| `CombatStyle.cpp:384-417` | 28 `CombatInventoryItemMagicT<CombatInventoryItemMagic\|Staff, CombatMagicCaster{14 seats}>` template-instantiation vtables | see sub-table | see sub-table | see sub-table | `rtti_vt.py`, using the real mangled form `.?AV?$CombatInventoryItemMagicT@VCombatInventoryItem{Magic\|Staff}@@VCombatMagicCaster{X}@@@@` recovered by grepping `strings` output | HIGH |
| `CombatStyle.cpp:374` (referenced) | `CombatMagicCasterArmor` -- comment says "bare ... symbol has no class" | `0x18CD4D0` | `0x1687268` | `0x194BCF0` | `rtti_vt.py` found a REAL RTTI class for this. The comment's "no class" means CommonLib exposes no typed C++ wrapper for it, NOT that the engine's RTTI is absent -- a real vtable exists on all three runtimes | HIGH (nuance flagged) |
| `MainThread.cpp:103` | `VTABLE_PlayerCharacter[0]` | `0x18AB9C0` | `0x16635E0` | `0x19296C0` | `rtti_vt.py`, TD string `.?AVPlayerCharacter@@` | HIGH |
| `Targeting.cpp:162` | `VTABLE_Character[0]` | `0x18A5558` | `0x165DA40` | `0x1923258` | `rtti_vt.py`, exact match to addrlib (AE id 207886, SE id 261397) | HIGH |

### MFO sub-table: 14 CombatMagicCaster seat vtables (CasterConsent.cpp:1087-1095)

| seat | AE | SE | 1.7.104 |
|---|---|---|---|
| Offensive | `0x18CC4A0` | `0x1686CA0` | `0x194ACC0` |
| Restore | `0x18CC890` | `0x1686DC8` | `0x194B0B0` |
| Ward | `0x18CC638` | `0x1686D38` | `0x194AE58` |
| Summon | `0x18CCA88` | `0x1686E60` | `0x194B2A8` |
| Stagger | `0x18CCC00` | `0x1686EF0` | `0x194B420` |
| Disarm | `0x18CCD38` | `0x1686F88` | `0x194B558` |
| Cloak | `0x18CCED0` | `0x1687018` | `0x194B6F0` |
| Light | `0x18CD048` | `0x16870A8` | `0x194B868` |
| Invisibility | `0x18CD1E0` | `0x1687138` | `0x194BA00` |
| BoundItem | `0x18CD358` | `0x16871D0` | `0x194BB78` |
| TargetEffect | `0x18CD668` | `0x16872F8` | `0x194BE88` |
| Paralyze | `0x18CD9E0` | `0x1687428` | `0x194C200` |
| Script | `0x18CDB78` | `0x16874C0` | `0x194C398` |
| Reanimate | `0x18D5318` | `0x1687390` | `0x19538E0` |

Evidence: `rtti_vt.py`, all 14 resolved on all 3 binaries; spot-checked exact vs addrlib for Offensive.
Confidence HIGH throughout.

### MFO sub-table: 28 CombatInventoryItemMagicT combos (CombatStyle.cpp:384-417)

All 28 resolved on all 3 binaries via `rtti_vt.py` (mangled form
`.?AV?$CombatInventoryItemMagicT@VCombatInventoryItem{Magic|Staff}@@VCombatMagicCaster{seat}@@@@`).
Confidence HIGH throughout (technique proven 5x against the address library before use).

| item base | seat | AE | SE | 1.7.104 |
|---|---|---|---|---|
| Magic | Offensive | `0x18D0FF0` | `0x168E628` | `0x194F838` |
| Magic | Restore | `0x18D0570` | `0x168DEA8` | `0x194EDB8` |
| Magic | Ward | `0x18D0AB0` | `0x168E268` | `0x194F2F8` |
| Magic | Summon | `0x18D0010` | `0x168DAE8` | `0x194E858` |
| Magic | Stagger | `0x18CFAF0` | `0x168D728` | `0x194E338` |
| Magic | Disarm | `0x18CF590` | `0x168D368` | `0x194DDD8` |
| Magic | Cloak | `0x18CF070` | `0x168CFA8` | `0x194D8B8` |
| Magic | Light | `0x18CEB30` | `0x168CBE8` | `0x194D378` |
| Magic | Invisibility | `0x18CE5D0` | `0x168C828` | `0x194CE18` |
| Magic | BoundItem | `0x18CE070` | `0x168C468` | `0x194C890` |
| Magic | TargetEffect | `0x18CD410` | `0x168BCE8` | `0x194BC30` |
| Magic | Paralyze | `0x18CC518` | `0x168B568` | `0x194AD38` |
| Magic | Script | `0x18CBF80` | `0x168B1A8` | `0x194A7A0` |
| Magic | Reanimate | `0x18CCC78` | `0x168B928` | `0x194B498` |
| Staff | Offensive | `0x18D0DF0` | `0x168E4A8` | `0x194F638` |
| Staff | Restore | `0x18D03F0` | `0x168DD28` | `0x194EC38` |
| Staff | Ward | `0x18D08B0` | `0x168E0E8` | `0x194F0F8` |
| Staff | Summon | `0x18CFE10` | `0x168D968` | `0x194E658` |
| Staff | Stagger | `0x18CF8F0` | `0x168D5A8` | `0x194E138` |
| Staff | Disarm | `0x18CF390` | `0x168D1E8` | `0x194DBD8` |
| Staff | Cloak | `0x18CEE70` | `0x168CE28` | `0x194D6B8` |
| Staff | Light | `0x18CE950` | `0x168CA68` | `0x194D198` |
| Staff | Invisibility | `0x18CE390` | `0x168C6A8` | `0x194CBB0` |
| Staff | BoundItem | `0x18CDE50` | `0x168C2E8` | `0x194C670` |
| Staff | TargetEffect | `0x18CD100` | `0x168BB68` | `0x194B920` |
| Staff | Paralyze | `0x18CC2A0` | `0x168B3E8` | `0x194AAC0` |
| Staff | Script | `0x18CBD80` | `0x168B028` | `0x194A5A0` |
| Staff | Reanimate | `0x18CC948` | `0x168B7A8` | `0x194B168` |

### MFO layout facts (not tied to one call site -- struct/array shape)

| fact | 1.6.1170 | 1.5.97 | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|
| **`ControlMap` member offsets** (already the subject of a shipped regression, CLAUDE.md principle 11) -- `contextPriorityStack`/`enabledControls`, driven by `INPUT_CONTEXT_ID::kTotal` | 18 contexts; `enabledControls@0x120`, `unk11C-equiv@0x124` | 17 contexts (matches CommonLib's declared header); `enabledControls@0x118`, `unk11C@0x11C` | **18 contexts** (AE-family layout, NOT SE's) -- `enabledControls@0x120`, `+0x124` | ToggleControls' own disassembly on 1.7.104 reads/writes `[rcx+0x120]`/`[rcx+0x124]`, byte-identical instruction encoding to AE's `0x120`/`0x124` accesses (disp32 bytes literally identical, not just numerically equal) | HIGH |
| `BSInputDeviceManager::devices[]` array size (the loop `PollInputDevices` walks) | **4** entries (`mov edi,4`; comment: "loops four times over `[rcx+0x60+i*8]`") | 4 entries (same loop shape per the existing MFO comment) | **6** entries (`mov edi,6` at the identical code position) -- loop body bytes otherwise unchanged length, so it does not shift any of the other call offsets | direct disassembly of PollInputDevices at `0xCF95F0`; NEWLY DISCOVERED in this scan, not previously documented anywhere in MFO | HIGH (this is a genuine, previously-undocumented layout change worth flagging to whoever eventually ports the overlay input path) |
| `RE::CombatController::attackerHandle` / `::combatStyle` offsets (both `static_assert`'d `<0x68`, i.e. below AE's extra `BSSpinLock`) | `attackerHandle@0x28`, `combatStyle@0x38` (existing `static_assert` in CasterConsent.cpp/CombatStyle.cpp/Targeting.cpp) | same (same `static_assert`, same declared header) | **same** (offsets not independently re-derived by direct field access, but proven by proxy) | `CombatMagicCasterRestore::CheckStartCast` (vtable slot 0x06, 2nd arg = `CombatController*` in `rdx`) disassembles to a **421+-byte function that is byte-for-byte identical between AE and 1.7.104** (every differing byte across a 0x900-byte window is a RIP-relative immediate, zero opcode/ModRM/displacement differences) -- since the function's compiled struct-offset displacements are unchanged, the struct layout it reads (which necessarily includes the region containing attackerHandle/combatStyle, both `<0x68`) is unchanged too | HIGH (proof-by-identical-machine-code, not a direct read of the two named offsets, but as strong as a direct read given the whole function matched) |
| `RE::BGSDefaultObjectManager::objects[]`/`objectInit[]` array size -- CommonLib 3.7.0's `IsObjectInitialized(idx)` dereferences `+0xB80` as a `bool*` and indexes it | **366 entries.** `objects[366]`@0x20..0xB90, `objectInit`@0xB90 | **364 entries.** `objects[364]`@0x20..0xB80, `objectInit`@0xB80 | **not investigated this pass -- flagged, not scanned** | held branch `feat/mfo-1.5.97-equipslot` (04da1d4) commit message + `Loadout.cpp:63-90`: on SE the mis-sized read turns `objectInit[0..7]` into poison pointer `0x0101010101010101` and faults; on AE it silently reads an unrelated form's flag bits (both measured against the unpacked binaries via ctor memset + `Load` + `InitItemImpl`). Both runtimes are worked around by resolving the left-hand EQUP via `LookupByID` instead (row above), not by fixing the manager read. **A genuinely open struct-layout divergence CommonLib's header gets wrong for at least one shipped runtime -- do not trust `BGSDefaultObjectManager::GetObject`/`IsObjectInitialized` on 1.7.104 without first re-measuring this offset.** | HIGH (AE/SE, already measured and shipped in the held branch); NOT FOUND (1.7.104) |

---

## 2. APMF address table

### "Six single-version REL::ID sites" -- resolved count

APMF's own `CLAUDE.md` rule 11 says "six single-version `REL::ID` sites in `native/` with no 1.5 id."
Grepping `native/` for every hardcoded per-build literal (excluding `Allowance.cpp`'s `REL::Offset(arrayRva)`/
`REL::Offset(descRva)`, which are read live off already-resolved RTTI structures at runtime and are
therefore version-agnostic, not single-version) finds exactly **two mechanisms, comprising 11 individual
hardcoded numeric constants**:

1. `core/AiCastSeats.cpp:338-348`, `struct WeaponClassSpec` / `kWeaponClasses[]` -- 4 entries (Melee, Ranged,
   Shield, Torch), each with 2 raw RVAs (`vtableRva`, `calcScoreRva`) = **8 constants**. AE-only per the file's
   own banner ("no CommonLib concrete C++ class... no Address-Library ID exists for any of the four").
2. `core/CastClassify.cpp:53-55`, `kSpellOffset=0x10`, `kCtrlOffset=0x18`, `kSelfFlagOffset=0x4c` -- 3 struct
   MEMBER offsets (not relocation addresses, but hardcoded and AE-only: "disassembly-CERTAIN on 1.6.1170...
   explicitly NOT VERIFIED on 1.5.97", `CastClassify.h:66`).

Neither grouping (4 sites / 8 constants, or +3 offsets = 11 constants, or 2 mechanisms) cleanly produces
"six." This scan cannot reconcile CLAUDE.md's count with what's actually in `native/` today -- it may
predate the `CastClassify.cpp` seat, or count something this grep missed, or simply be stale. Reported
plainly rather than forced to fit.

**Scan finding that materially changes the picture:** the AiCastSeats.cpp banner's claim that the four
weapon classes have "NO CommonLib concrete C++ class" is only half right -- CommonLib doesn't bind them, but
**real MSVC RTTI exists for all four in the binary** (`CombatInventoryItemMelee`/`Ranged`/`Shield`/`Torch`,
found by walking backward from the known AE `vtableRva` to the COL pointer at `vtableRva-8` and reading its
`TypeDescriptor`). That means `rtti_vt.py` resolves the SE and 1.7.104 vtable addresses too -- something the
existing comment describes as unknowable ("meaningless on any other build"). See the Group C sub-table below.

### Plain-function sites (address-library based, AE/SE resolved, 1.7.104 pattern-matched)

| site (file:line) | what | 1.6.1170 (id -> RVA) | 1.5.97 (id -> RVA) | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|---|
| `channels/MovementDeny.cpp:42` | `Actor::SetDontMove(bool)` | id 37489 -> `0x6750D0` | id 36490 -> `0x5E3330` | `0x687BA0` | addrlib exact match; 1.7.104: 19-byte exact prefix (incl. a `mov edx,9` literal) was a UNIQUE hit; disasm shape consistent (helper call + tail-jmp to a shared movement-update routine) | HIGH |
| `channels/MovementDeny.cpp:52` | `Actor::KeepOffsetFromActor(target,offset,angle,catchUp,follow)` | id 37894 -> `0x697110` | id 36870 -> `0x603990` | `0x6A9C90` | addrlib exact match; 1.7.104: AE/SE diverge in register allocation from byte 3 (real recompile), so matched against AE only -- a 49-byte exact prefix (through the `[rcx+0x150]` member read, a smart-pointer refcount field also used by the next row) was a UNIQUE hit | HIGH |
| `channels/MovementDeny.cpp:58` | `Actor::ClearKeepOffsetFromActor()` | id 37895 -> `0x697250` | id 36871 -> `0x603AC0` | `0x6A9DD0` | addrlib exact match; 1.7.104: matched against AE only (generic prologue alone gave 190+ false hits); a 36-byte mid-function signature anchored on the same `+0x150` member offset gave a UNIQUE hit, 0x140 bytes after `KeepOffsetFromActor`'s resolved address -- consistent with the two functions sitting adjacent in source, as they do on AE | HIGH |
| `core/PackageGate.cpp:341` | `Actor::EvaluatePackage(bool,bool)` (called `(actor,true,false)` -- resetAI MUST stay false) | id 37401 -> `0x66CCF0` | id 36407 -> `0x5DB310` | `0x67F7B0` | addrlib exact match; 1.7.104: 52-byte exact prefix was a UNIQUE hit; shape consistent (3-arg member fn, vtable dispatch at `+0x4c8`) | HIGH |

### Group C: weapon-class CalculateScore seats (AiCastSeats.cpp:338-348, 1220-1230)

| weapon class | field | 1.6.1170 | 1.5.97 | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|---|
| Melee | vtable | `0x18C9028` (source literal) | `0x1681A88` | `0x19475C8` | `rtti_vt.py` on real RTTI class `CombatInventoryItemMelee`, discovered by walking `vtableRva-8`; AE result reproduces the source's own hardcoded literal exactly | HIGH |
| Melee | `CalculateScore` (slot 0x0C) | `0x8183E0` (source literal) | `0x77E0A0` | `0x82D2C0` | slot-0x0C read off the resolved vtable on all 3; AE reproduces source literal exactly; 1.7.104 body disassembles byte-identical to AE for the first 0x30 bytes checked | HIGH |
| Ranged | vtable | `0x18C90D8` | `0x1681B58` | `0x1947678` | same technique | HIGH |
| Ranged | `CalculateScore` | `0x8188B0` | `0x77E550` | `0x82D790` | slot-0x0C read, AE matches source literal | HIGH |
| Shield | vtable | `0x18C9188` | `0x1681C28` | `0x1947728` | same technique | HIGH |
| Shield | `CalculateScore` | `0x818DF0` | `0x77EAC0` | `0x82DCD0` | slot-0x0C read, AE matches source literal | HIGH |
| Torch | vtable | `0x18C92E8` | `0x1681DD0` | `0x1947888` | same technique | HIGH |
| Torch | `CalculateScore` | `0x819480` | `0x77F0E0` | `0x82E360` | slot-0x0C read, AE matches source literal | HIGH |

**Note for whoever next touches Group C:** the code's own comment ("AE-ONLY... the RVAs are meaningless on
any other build") is now shown to be more pessimistic than necessary -- SE and 1.7.104 vtable+CalculateScore
addresses ARE derivable (table above), just not via the address library. Bringing SE/1.7.104 online for
Group C is a real, scoped follow-up, not a dead end; it was out of scope to also re-verify the *rest* of
each `CalculateScore` body byte-for-byte (only the first 0x30 bytes of Melee's were spot-checked) or to
confirm `category`/other `WeaponClassSpec` fields still apply unchanged.

### CastClassify.h SEAT 0 (`CombatMagicItemData` vtable slot 1)

| site (file:line) | what | 1.6.1170 | 1.5.97 | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|---|
| `core/CastClassify.cpp:31` | `kCombatMagicItemDataVtable` (`VariantID(265000,211955,...)`) | id 211955 -> `0x18D5120` | id 265000 -> `0x1686BF8` | `0x19536E8` | `rtti_vt.py`, TD `.?AVCombatMagicItemData@@` | HIGH |
| (vtable slot 1, the classify thunk) | per-effect visitor thunk (reads `kSpellOffset=0x10`, `kCtrlOffset=0x18`, `kSelfFlagOffset=0x4c`, all AE-only per the file banner) | `0x81D830` (matches the file's own comment exactly) | not checked (function shape not independently confirmed on SE) | `0x832D20` | slot-1 read off the resolved vtable; AE value matches the source comment's own literal exactly; 1.7.104's ~421-byte function body is byte-for-byte identical to AE across every opcode/ModRM/displacement byte checked (diffs isolated to RIP-relative immediates only) -- strong indirect evidence the three offsets still hold on 1.7.104, though the three displacements were not individually greped out of the disassembly by name | HIGH for the vtable slot; MED-HIGH for the three offsets specifically (inferred from whole-function identity, not read out one-by-one) |

### Vtable/RTTI families shared with MFO (same classes, same technique -- not re-tabulated)

APMF hooks the SAME 14 `CombatMagicCaster*` seat vtables (`core/AiCastSeats.cpp:1184-1197` Group B,
`core/CastSeats.cpp:499` a 2-entry subset: `CombatMagicCasterRestore`+`CombatMagicCasterOffensive`) and the
SAME 28 `CombatInventoryItemMagicT<...>` combos (`core/EquipGate.cpp:654-681`, `core/AiCastSeats.cpp:1127-1152`
Group A) that MFO's `CasterConsent.cpp`/`CombatStyle.cpp` already hook -- see the MFO sub-tables above for
every address on all 3 runtimes; they are identical binary-level facts regardless of which plugin reads them.
Also shared: `VTABLE_Character[0]` (`core/Hook.cpp:94`, `core/PackageGate.cpp:296`, `core/NonAliasProbe.cpp:212`),
`VTABLE_PlayerCharacter[0]` (`core/Hook.cpp:97`), `VTABLE_ActorMagicCaster[0]`/`RTTI_MagicCaster`
(`core/CastGate.cpp:197-199`) -- all already in the MFO table above.

**New for APMF (not used by MFO):**

| site (file:line) | what | 1.6.1170 | 1.5.97 | 1.7.104 | evidence | confidence |
|---|---|---|---|---|---|---|
| `core/CastSeats.cpp:499`, `:522` | `VTABLE_CombatProjectileAimController[0]` / `RTTI_CombatAimController` (seat 0x0D) | `0x18C59A8` / `0x18C5888` | `0x167E2A8` / `0x167E130` | `0x1943F48` / `0x1943E28` | `rtti_vt.py`, TD `.?AVCombatProjectileAimController@@` / `.?AVCombatAimController@@` | HIGH |
| `core/EquipGate.cpp:652`, `core/AiCastSeats.cpp:1126,1183` | `RTTI_CombatInventoryItem` (base, expected-TD check) | `0x18C8EC8` | `0x1681928` | `0x1947468` | `rtti_vt.py`, TD `.?AVCombatInventoryItem@@` | HIGH |
| `core/AiCastSeats.cpp:1184` | `RTTI_CombatMagicCaster` (base, expected-TD check) | `0x18D5150` | `0x1686C28` | `0x1953718` | `rtti_vt.py`, TD `.?AVCombatMagicCaster@@` | HIGH |
| `core/ActionGate.cpp:501-589` | `RTTI_CombatBehaviorTreeNode` (base) + `kLeaves[]`, 70 concrete `CombatBehaviorTree*` leaf vtables | base: `0x18D9138`; 70 leaves already carry their own SE/AE `VariantID` triples in `core/CombatBehaviorRE.h` (compiled, shipped, no further AE/SE work needed) | base: `0x1692510` | base: `0x1957700`; **70 leaves: `NOT FOUND`** | base via `rtti_vt.py`, TD `.?AVCombatBehaviorTreeNode@@`, exact base-class match. The 70 leaves are NOT simple RTTI names -- sampling 6 names taken directly from `kLeaves[]` (`CombatBehaviorAdvance`, `CombatBehaviorAttack`, `CombatBehaviorFlee`, `CombatBehaviorCastImmediateSpell`, `CombatBehaviorCastConcentrationSpell`, `CombatBehaviorDrinkPotion`) all returned NOT FOUND as bare `.?AV<name>@@` strings -- they are almost certainly complex template instantiations (one unrelated template was found by accident earlier in this scan containing "CombatBehaviorAttacker" as a substring 200+ characters into an unrelated `CombatBehaviorTreeValueNodeT<bool, ...>` mangling), meaning each of the 70 needs its own individually-recovered mangled name. That is real, nontrivial RE work this scan did not do -- flagged as a scoped follow-up, not attempted here | base HIGH; 70 leaves NOT FOUND (scope decision, see above) |

---

## 3. Report

**Table file:** `/tmp/claude-1000/-mnt-gaming-modlists-Projects-marth-follower-overhaul/a935cdde-4a9b-4a50-983f-339ce7b41062/scratchpad/1.5-1.7-address-table.md`
(this file). Helper scripts live alongside it in the same scratchpad directory (`addrlib.py` pre-existing +
validated; `rtti_vt.py`, `batch_rtti.py`, `rtti_AE.txt`/`rtti_SE.txt`/`rtti_17104.txt` new this session).

**Row counts.** MFO: 13 named-site rows + 2 sub-tables (14 + 28 addresses) + 3 layout-fact rows = ~58 distinct
addresses/facts across all three runtimes. APMF: 4 plain-function rows + 8 Group-C rows + 2 CastClassify rows
+ 4 "new for APMF" RTTI rows + 1 "70 leaves, not attempted" row, on top of reusing MFO's 14+28-vtable
families. Total distinct engine facts resolved this session: roughly 100, all but a handful HIGH confidence.

**NOT FOUND cells, by runtime:**
- 1.6.1170 (AE): 0. Every AE cell traces to either the address library or a source-code literal that was
  independently reproduced.
- 1.5.97 (SE): 0 for everything this scan touched, EXCEPT the CastClassify.cpp slot-1 thunk's three struct
  offsets, which were never claimed for SE in the source either (explicitly AE-only, unverified on SE, per
  the existing code comment -- not something this scan was asked to newly establish).
- 1.7.104: `Packages.cpp:305` ForceRefTo (not attempted -- ran out of cheap anchors, genuinely open); the 70
  individual `CombatBehaviorTreeNode` leaf vtables (scoped out -- see above, each needs its own mangled-name
  recovery, real work, not done here). Everything else attempted was resolved.

**Things that looked wrong or noteworthy while scanning (observations, not fixes):**
1. **`RE::VTABLE_*` symbols are address-library IDs, not live RTTI scans.** Both MFO's and APMF's source
   comments repeatedly describe these as "version-resilient... VTABLE INDICES, not sourced offsets." That's
   true in the sense that the *index into* the vtable is version-resilient, but the *vtable's own address* is
   absolutely address-library-gated (confirmed by reading `include/REL/Relocation.h` directly) -- which is
   exactly why none of them resolve on 1.7.104 without the workaround this scan built. Worth a comment
   correction wherever this phrasing appears, so a future reader doesn't assume 1.7.104 support is free.
2. **`AiCastSeats.cpp`'s Group C banner ("no CommonLib concrete C++ class... meaningless on any other
   build") is more pessimistic than the binary actually is** -- see the Group C section above. Real RTTI
   exists; SE and 1.7.104 addresses are derivable. Not a bug, but the comment will mislead the next person
   who reads it and concludes Group C is a dead end off AE.
3. **APMF's CLAUDE.md "six single-version REL::ID sites" does not reconcile** with what's in `native/` today
   (this scan found 2 mechanisms / 11 constants, in no grouping equal to six). Flagged, not resolved --
   might be stale documentation, might reflect something this grep missed.
4. **A newly-discovered, previously-undocumented layout fact:** `BSInputDeviceManager::devices[]` appears to
   be 6 entries on 1.7.104 vs 4 on both AE and SE (`PollInputDevices`' loop bound literal). This doesn't
   break anything MFO currently does (the loop body bytes are unchanged length, so every subsequent call
   offset in the function still lines up exactly), but it's exactly the kind of silent array-growth that
   CLAUDE.md principle 11 warns about, and nothing in either repo's docs currently mentions it.
5. **1.7.104's `.text`/`.rdata` ship unencrypted** while still wrapped in a SteamStub header carrying the
   same signature family as AE/SE. Worth flagging since MFO's `unpack_steamstub.py` script (used for AE/SE)
   will throw an `IndexError` if pointed at this build unmodified -- it assumes `codeVA`/`codeSize` are
   always populated, which they are not here.
6. **`BGSDefaultObjectManager::objects[]`/`objectInit[]` (364 SE / 366 AE entries) is a second, independent
   struct-layout divergence** beyond ControlMap and CombatController, found in the held branch
   `feat/mfo-1.5.97-equipslot`'s commit message rather than by fresh disassembly this pass -- not
   re-measured for 1.7.104 (see the MFO layout-facts table). Flagging so a future 1.7.104 pass knows a third
   layout landmine exists in addition to the two this brief named up front.
7. **Cross-validation note (coordinator):** this file's APMF findings were independently reproduced by a
   SEPARATE fork dispatched for APMF-only work, run in parallel with no shared state. Every overlapping
   value matches exactly: all 4 `MovementDeny`/`PackageGate` 1.7.104 RVAs, all 8 Group-C
   (Melee/Ranged/Shield/Torch vtable+CalculateScore) 1.7.104 RVAs, and the `CombatMagicItemData` slot-1
   1.7.104 address (`0x832D20`) and its AE cross-check (`0x81D830`) -- all bit-for-bit identical between the
   two independent runs. The "six single-version sites" count differs between the two reports (the dedicated
   APMF fork counted 4 sites/8 raw RVA constants from `WeaponClassSpec` only; this file's pass additionally
   counted `CastClassify.cpp`'s 3 hardcoded struct-member offsets as a second single-version mechanism, for
   11 constants across 2 mechanisms) -- neither reconciles to exactly six, and the fuller accounting (11
   constants / 2 mechanisms) is kept as the reported figure since it is the more complete enumeration; the
   narrower one is not wrong, just partial. Both agree the discrepancy should go back to marth, not be forced
   to fit.