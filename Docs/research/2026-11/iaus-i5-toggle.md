# IAUS card I5: the per-follower toggle in the FLWR `mfoEnabled` byte

2026-11-10. Branch `feat/iaus-i5-toggle`. Tier A (co-save value space). Design: `iaus-design.md` section 7 and
section 9 row I5. Ruling Q31: IAUS is OFF by default per follower, the feature is available.

## What changed

No layout change and no version bump. FLWR stays v6. The one u8 that FLWR v5 added after `combatClassOverride`
now carries one more bit.

| bits | meaning | since |
|---|---|---|
| 0 | `mfoEnabled` | FLWR v5 (T#78) |
| 1-2 | `helmetOverride`, 0 Default / 1 Wear / 2 Never. 3 is not a value and reads Default | 2026-10-20 (86e3phy41) |
| 3 | `iausCombat`, the combat IAUS toggle | this card |
| 4 | reserved for the logistics IAUS toggle (card 1dfcuabcr0g). Written 0. Ignored on read | this card |
| 5-7 | unused. Written 0. Ignored on read | always |

Rule for every bit above bit 0: it is written only while `mfoEnabled` is true. So the byte is 0 exactly when MFO
is off for that follower.

- Runtime field: `FollowerState::iausCombat` (`native/State.h:122`), default `false`. There is no field for the
  logistics bit.
- The encode and decode are `MFO::FlwrEnabledByte::Encode` / `Decode` in `native/FlwrEnabledByte.h`. The header
  uses standard headers only. The FLWR writer (`native/Serialization.cpp:578`) and reader (`:861`) call them.
  The helmet and enabled expressions moved there verbatim from those two sites.
- Nobody reads `iausCombat` yet. I6 consumes it. I7 adds the board control. There is no INI key.

## The older-DLL argument

Two older readers of this byte exist in the field.

1. Pre-helmet (FLWR v5 as first shipped): `enabled = byte != 0`. It writes 0 or 1.
2. Helmet era (2026-10-20 up to this card): `enabled = byte != 0`, `helm = (byte >> 1) & 3`, kept only when bit 0
   is set and `helm <= 2`. It writes `enabled ? 1 | helm << 1 : 0`.

Neither one looks at bit 3 or above. The helmet read masks with 3 after the shift, so bit 3 cannot leak into it.
Because bit 3 is written only with bit 0 set, the byte is nonzero exactly when `mfoEnabled` is true, with or
without the toggle. So both older DLLs see the same enabled state, and the helmet-era one sees the same override.
They do not see the toggle, so the follower runs on his gambit table, which IAUS never edits. Their next save
writes the byte without bit 3. The toggle is dropped and he comes back on gambits. That is the safe direction.

This is only about DLLs that can read the record at all. A FLWR v5 DLL skips a v6 FLWR whole (card G9). That is
G9's documented one-way downgrade and this card does not change it.

Costs, both the same ones the helmet override documents:

- A follower saved with MFO switched off for him loses his IAUS toggle. It reads back off.
- A round trip through an older DLL drops every toggle.

## Bits 5-7 and bit 4 on read

Checked: one writer of the byte exists (the FLWR writer) and it has never set bits 3-7. No shipped record holds
them. The reader ignores bits 4-7 instead of treating them as a stray value. Reason: the helmet reader has always
ignored the bits above its own, and that is exactly what lets this card add bit 3 without breaking it. Clamping
on unknown high bits would make a later card's bit switch this DLL's toggle off on a downgrade for no reason.
What the helmet clamp does guard is kept and extended: a field counts only with bit 0 set, so a stray byte such
as 0x08 (bit 3 without bit 0) reads enabled (it is nonzero, as an older DLL reads it), override Default and
toggle off.

## Where the toggle is constructed or reset

It mirrors `helmetOverride` at every site. Neither field has a line of its own anywhere outside the FLWR writer
and reader (and, for the helmet, the board). Both take the struct default.

- New follower: `g_followers.try_emplace` in `Followers::TryEnsureRecord` (`native/Followers.cpp:391`), then
  `ApplyDefaultKit`, which only fills the gambit tables.
- Load: `FollowerState st{}` in the FLWR reader, then `Decode`. A pre-v5 record has no byte and keeps the default.
- Revert and new game: `g_followers.clear()` in `ResetAllState` and at the top of the FLWR reader.
- Roster removal: the whole record is erased (`native/Followers.cpp:618`). If he re-joined meanwhile he gets a
  fresh default record.
- Dismiss and rejoin: the record stays in `g_followers` (dismissed followers stay), so the toggle is kept, like
  the override.

## Other records

- MCLS (`native/classes/State.cpp`) has no reference to `mfoEnabled`, `helmetOverride` or this byte. Its rows go
  through `GambitWire`.
- The G9 v6 row encoding (`GambitWire`, `native/Serialization.h`) is the per-gambit row: condition, param,
  subject, action, form, flags, cooldown. The `mfoEnabled` byte is a per-follower field written before the tables.
  The row code does not touch it.

## Proof

1. Compile time, in the DLL build itself: `static_assert(FlwrEnabledByte::SelfCheck())`. It walks all 256 byte
   values and every writer input (enabled 0/1, helmet 0..255, toggle 0/1) against constexpr mirrors of both older
   readers and the helmet-era writer. A green CI build means MSVC evaluated it.
2. Host run: `tools/flwr_byte_test/run.sh`. It builds the same header and walks enabled 0/1, helmet 0/1/2 and the
   stray 3, toggle 0/1, with every garbage pattern in bit 4 and bits 5-7 on read, then the older DLLs' read and
   re-save. Output on 2026-11-10 (g++ and clang++): 1255 checks, 0 failed.

The older DLLs' expressions are mirrors. Their code is no longer in the tree. Compare them with
`git show 12aa000:native/Serialization.cpp` lines 572-573 (writer) and 853-855 (reader).

## In-game checks (no reader yet, so these wait for I6 / I7 or a debugger)

There is no way to set the toggle in game until I7. Until then the only in-game check is the regression one.

1. Regression, now: load an existing save with this DLL, save, load again. Every follower keeps his MFO switch
   and his helmet override. The `[cosave]` FLWR lines show no error.
2. With I7: turn combat IAUS on for one follower, save, load. The toggle is still on. The other followers are
   still off.
3. With I7: turn it on, switch MFO off for him, save, load. MFO is off and the toggle reads off (documented cost).
4. With I7: save with the toggle on, then load that save with the last DLL before this card. He is MFO-enabled,
   keeps his helmet override, and fights on his gambit table. Save there, load with the new DLL: toggle is off.
5. With I7: a follower who never had the toggle set stays off after any number of save and load cycles.
