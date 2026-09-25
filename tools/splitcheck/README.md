# splitcheck: prove a TU split did not change behaviour

A file split is a pure move of source text. CI only proves it still compiles.
This folder holds four tools, used together for every split (CLAUDE.md,
"Source file size and splits"):

| Tool | Proves |
|---|---|
| `linecheck.py` | the SOURCE half: old and new lines are the same multiset, in the same order (seam check) |
| `splitcheck.py` | the BINARY half: main's DLL and the branch's DLL, function by function and variable by variable |
| `cosave_roundtrip.py` | BEHAVIOUR of the co-save functions: real `.skse` co-saves through both DLLs under an emulator |
| `selftest.py` | the tools still catch every planted defect on this pair |

marth's ruling (2026-09-24): "pass, explained can only pass if the self test
and logic tests on that changed area still result in the same results as pre
change." So an /O2 function the tool can only EXPLAIN passes only through the
no-optimizer proof (below) plus, for co-save functions, the round-trip.

## Requirements (Linux)

- `llvm-pdbutil` on `PATH` (Debian/Ubuntu: `llvm-17` or newer).
- Python 3.10+ in a venv, never system-wide:

```sh
python3 -m venv /tmp/splitcheck-venv
/tmp/splitcheck-venv/bin/pip install capstone pefile unicorn
```

## Getting the builds

CI uploads `MFO.dll` + `MFO.pdb` as the `MFO-dll` artifact of every `native` run,
and a SEPARATE `MFO-build-info` artifact (never inside the deploy zip):
`build-info.txt` = `built=<sha> noinline=<b> noopt=<b> dll_sha256=<hex>`.
Four builds are needed: main and the branch, each twice. Dispatch all four with
a FULL `source_ref` SHA so each pair is provably the same commit:

```sh
M=<main-sha>; B=<branch-sha>          # full 40-char SHAs
gh workflow run native --ref <branch> -f source_ref=$M
gh workflow run native --ref <branch> -f source_ref=$B
gh workflow run native --ref <branch> -f noopt=true -f source_ref=$M
gh workflow run native --ref <branch> -f noopt=true -f source_ref=$B
gh run download <run> -D a     # a/MFO-dll/MFO.dll + a/MFO-build-info/build-info.txt
```

**Provenance is checked, not trusted.** `--proof` refuses to run (FAIL, exit 1,
"PROVENANCE") unless the four records pair up: A and A0 built from one SHA, B
and B0 from another (and A != B), A0/B0 `noopt=true`, A/B the normal build, and
every record's `dll_sha256` equal to the DLL beside it. The record is found in the
`gh run download` layout (`<dir>/MFO-build-info/build-info.txt` next to
`<dir>/MFO-dll/`), or beside the DLL, or given with `--build-info A B A0 B0`.
Only `success` runs.

**Why /Od and not /Ob0.** Without LTCG, MSVC decides inlining per TU, so a split
changes which helpers get inlined. /Ob0 removes inlining, but MSVC still
optimizes ACROSS the functions of one TU (a caller's register allocation uses
what it knows of a same-TU callee): measured on wave 1, /Ob0 main vs /Ob0 branch
left 7 MFO functions and 153 STL instantiations differing in registers and
block layout only. /Od (`noopt=true`) compiles each function from its own source
alone, and main vs branch came out 31601/31601 identical. The `noinline` input
is kept for reference. Neither proof DLL is ever shipped.

## Running

```sh
V=/tmp/splitcheck-venv/bin/python
# 1. source
python3 tools/splitcheck/linecheck.py --old-rev <main-sha> --old native/Old.cpp ... \
    --new native/sub/New1.cpp native/sub/New2.cpp ...
# 2. binary, with the proof
$V tools/splitcheck/splitcheck.py a/MFO-dll/MFO.dll a/MFO-dll/MFO.pdb b/MFO-dll/MFO.dll b/MFO-dll/MFO.pdb \
    --tu-map tools/splitcheck/tumaps/<wave>.txt \
    --proof a0/MFO-dll/MFO.dll a0/MFO-dll/MFO.pdb b0/MFO-dll/MFO.dll b0/MFO-dll/MFO.pdb [--json out.json]
# 3. co-save behaviour (copy the saves first; the tool only reads them)
$V tools/splitcheck/cosave_roundtrip.py --a a/MFO-dll --b b/MFO-dll --selftest saves/*.skse
# 4. the tools on this pair
$V tools/splitcheck/selftest.py --a a/MFO-dll --b b/MFO-dll --a0 a0/MFO-dll --b0 b0/MFO-dll \
    --tu-map tools/splitcheck/tumaps/<wave>.txt --work /tmp/st \
    --lc-old-rev <main-sha> --lc-old native/Old.cpp ... --lc-new native/sub/New1.cpp ...
```

Exit codes. `linecheck`: 0 PASS, 1 FAIL. `splitcheck`: 0 = `PASS` (byte
identical) or `PASS-PROVEN`; 3 = `PASS-EXPLAINED-UNPROVEN` (explained classes
only, no proof given: NOT mergeable for a split); 1 = `FAIL`.
`cosave_roundtrip`: 0 = every save identical (and the self-test saw its
planted change), else 1. `selftest`: 0 = every case behaved, 1 = a hole.

`--strict` disables every explained class. `--strict --copies-ok` (what
`--proof` runs on the proof pair) still accepts only identical per-TU copies of
a header-defined internal-linkage object whose copy COUNT changed (a split
adds TUs that include the header), and only when that object is CONST (its PDB
type is const-qualified, or it sits in a read-only section: MSVC records a
constexpr class-typed variable such as `std::chrono::seconds` with no const
modifier, but in `.rdata`). A MUTABLE object's copy-count change FAILs as a STATE
SPLIT unless the same functions read it in both builds and every two of them
share a copy in A exactly when they share one in B (so a split that hands two
moved functions their own copies of state they used to share is caught). `--tu-map FILE` lists which new TUs each old
TU became (`old.cpp<TAB>new/a.cpp new/b.cpp`); it is how file-local twins are
kept apart.

## How splitcheck matches and compares

1. **Enumerate.** `llvm-pdbutil dump -symbols -globals -publics` gives every
   function, named variable (with its module and its PDB type, sized from the
   TPI stream), public, `S_CONSTANT`, and inline site (with binary-annotation
   ranges, at every depth). `-ids -types` gives each inline site's QUALIFIED
   callee name.
2. **Match** by `(name, signature)`, then with the anonymous namespace removed
   (a file-local helper the split made extern; listed as "re-links"), then by
   unique name. A name defined at 2+ addresses (file-local TWINS: the PDB drops
   "anonymous namespace" from data names) is tagged with its module, and a
   twin pairs only with a twin in a TU its TU became (`--tu-map`). A twin on
   one side against a unique symbol on the other is TU-checked the same way,
   through the unique symbol's owning module.
3. **Compare** in lockstep (capstone). Opcode, register and immediate bytes must
   be equal; every address field is compared by what it points at: symbols
   (twins by TU), RTTI by class name, differently-named callees by structural
   identity. **Read-only data** (a section neither code nor writable): a
   reference to a named object's start, inside a named object of known PDB
   size, or one past its end (a loop bound) compares by name. Anything else is
   compared by CONTENT, never a fixed length: a string literal (named `??_C` or
   unnamed, UTF-8 accepted) as its whole NUL-terminated byte string (a
   source-path literal may change its file name); other unnamed data over
   min(extent in A, extent in B), the extent running to the next object
   boundary (a symbol, a relocation slot or target, a RIP-relative target
   anywhere in the code including non-PDB library code, a pooled text literal).
   The object ends before the next object starts in EACH build, so the shorter
   extent still covers all of it; the longer one's excess is another object
   (/Od keeps whole literal pools, referenced or not). Residual: a non-string
   object whose bytes read as a one-character string and a NUL is compared as
   that string.
4. **Named data.** Every `MFO::` variable/constant compared byte by byte over
   its PDB type size, else up to the next symbol, no cap (pointer slots by
   target). A datum in one build only, or
   duplicated, is REPORTED; one side folded into an `S_CONSTANT` of the same
   value is listed as folded.

## The explained classes (non-strict /O2 compare)

| Class | Accepted when |
|---|---|
| inline-drift | inline sites differ (or a pure LIBRARY template COMDAT: a name with no `MFO::` in it), AND the own-code fingerprint is equal AND the constant multiset is equal |
| funclet-drift / renumbered | an unwind funclet of a drift function / a funclet renumbered with an identical body |
| outlined | present in one build only and inlined (PDB inline site, qualified name) in the other |
| copies | a header-defined internal-linkage object's per-TU copies (count changed, each identical), CONST only, or mutable with the same copy-sharing among its readers (see above) |
| PROVEN | with `--proof` passing: a pair whose inline sites differ; the /Od build proves its source compiles identically |

The **own-code fingerprint**: every reference outside the function's top-level
inline sites, plus one token per inlined callee; "inlined X" equals "calls X"
on QUALIFIED names. The **constant multiset**: every immediate in own code and
inside inlined MFO code (a lambda body inside its std::function wrapper counts;
only library inlinees are skipped), minus frame-size and `__chkstk` sizes. A
constant only one side carries is accepted only if a helper THAT side inlined
more carries it in its out-of-line body; anything else FAILs the function. A
function whose inline sites are identical but bytes differ is always FAIL.

## Self-tests (wave 1, 2026-09-24)

Positive, source-identical pairs:
- **(a)** main `923e72d` vs `bb105e0` (same source): **PASS**, 10136/10136, data 712/0.
- **(a2)** two builds of `923e72d`: **PASS-EXPLAINED-UNPROVEN**, 10134 identical, 2
  library inline-drift (MSVC codegen for a template COMDAT is not fully
  deterministic at /O2).
- **(a3)** two /Od builds of `37972d1` (runs 36073813808, 36073816108): **PASS**
  strict, 31601/31601, data 806/0. Two /Ob0 builds: also PASS.

Negative (`selftest.py`, every case must FAIL; main `e39cee2` vs branch `6545a7a`):
- **(b)** a real one-constant commit (`fe7b993`, `kQuashPairCooldown` 2 s to 3 s):
  **FAIL** naming `MFO::Rapport::QuashAllyPair` and the datum.
- **N1** an immediate changed in an inline-DRIFT MFO function: FAIL, named.
- **N2** an immediate changed inside the MFO lambda body of a
  `std::_Func_impl_no_alloc<...>::_Do_call` wrapper: FAIL, named.
- **N3** a RIP reference rebound to the other TU's same-named file-local twin
  (`g_lastApmfRefusal`): FAIL, the rebinding function named.
- **N4** a named variable removed from the branch PDB: FAIL, "ONLY IN A".
- **N5** two adjacent statements swapped (line multiset unchanged): linecheck
  FAIL on the SEAM.
- **N6** an immediate changed in the /Od proof build: `--proof` FAIL.
- **N7** one letter of an unnamed string literal changed in the /Od proof
  build: `--proof` FAIL.
- `cosave_roundtrip --selftest`: the branch's PRGN record version patched,
  every save DIFFERENT.

## Allow-list

`--allow FILE`: TAB-separated lines `kind<TAB>regex<TAB>justification`, kind one
of `differ`, `only-a`, `only-b`, `data`, `any`. Every hit is printed with its
justification. Prefer fixing the tool over allow-listing.
