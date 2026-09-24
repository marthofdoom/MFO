# splitcheck: prove a TU split did not change the compiled code

A file split is a pure move of source text. CI only proves it still compiles.
`splitcheck` compares two builds of MFO.dll **function by function** and says
which functions came out byte-identical, which differ, and why. `linecheck`
proves the source half: the old files' lines and the new files' lines are the
same multiset except for what the split had to write.

Use both for every split (CLAUDE.md, "Source file size and splits").

## Requirements (Linux)

- `llvm-pdbutil` on `PATH` (Debian/Ubuntu: `llvm-17` or newer).
- Python 3.10+ with `capstone` and `pefile`, in a venv, never system-wide:

```sh
python3 -m venv /tmp/splitcheck-venv
/tmp/splitcheck-venv/bin/pip install capstone pefile
```

## Getting the two builds

CI uploads `MFO.dll` + `MFO.pdb` as the `MFO-dll` artifact of every `native` run.

```sh
gh run list --workflow native --limit 5 --json databaseId,headSha,conclusion
gh run download <run-id-of-main>   -D a
gh run download <run-id-of-branch> -D b
```

Compare only runs whose `headSha` is the commit you mean, and only `success` runs.

## Running

```sh
/tmp/splitcheck-venv/bin/python tools/splitcheck/splitcheck.py \
    a/MFO-dll/MFO.dll a/MFO-dll/MFO.pdb  b/MFO-dll/MFO.dll b/MFO-dll/MFO.pdb \
    [--json out.json] [--max 60] [--strict] [--allow allow.tsv]

python3 tools/splitcheck/linecheck.py --old-rev <base> --old native/Old.cpp ... \
    --new native/sub/New1.cpp native/sub/New2.cpp ...
```

Exit code 0 = PASS or PASS-EXPLAINED, 1 = FAIL.

## How splitcheck matches and compares

1. **Enumerate.** `llvm-pdbutil dump -symbols -globals -publics` gives every
   function (`S_GPROC32`/`S_LPROC32`: name, signature, section:offset, code
   size), every named variable (`S_GDATA32`/`S_LDATA32`), every public, and every
   inline site (`S_INLINESITE` with its binary-annotation code ranges).
2. **Match** in three passes: exact `(name, signature)`; then with the
   anonymous namespace removed (a file-local helper a split made extern keeps
   its name otherwise); then by that name alone when it is unique on both sides
   (the PDB truncates long signature strings, cutting "`anonymous namespace'"
   in half). Names are canonicalised first: MSVC spells the anonymous
   namespace `A0x<hash>` inside template arguments and `?A0x<hash>@` inside
   decorated names, and the hash is per TU and not even stable between two
   builds of the same TU. Every anon-to-extern pairing is listed ("re-links").
3. **Compare** each pair by disassembling both bodies in lockstep (capstone).
   Opcode, register and immediate bytes must be equal. Every address-carrying
   field is compared by what it points at, never by its raw value:
   - RIP-relative displacements and rel8/rel32 branch/call targets,
   - image-relative disp32 (`[base + idx*4 + TABLE]` switch tables),
   - abs64 values that the `.reloc` table relocates.
   A target resolves to the symbol set at that address (procs, data, publics;
   ICF-folded functions keep their names at the shared address, so sets
   compare by intersection), to a string literal (compared by content; a
   source-file path literal may change its file name), to an RTTI
   TypeDescriptor (compared by its decorated class name, anonymous-namespace
   component and back-reference digits removed), to the PE header (by RVA),
   or to "one past the end" of the previous object (a loop bound). Two
   differently-named callees also match when their bodies compare identical
   (recursively). Switch tables that MSVC places after the body are compared
   as function-relative offsets.
4. **Named data.** Every named `MFO::` variable/constant present once in each
   build has its initial bytes compared (pointer slots by target symbol).

## Why "identical" is not always possible, and the explained classes

This build has no LTCG, so **MSVC decides inlining per translation unit**, and
its heuristics depend on how often a function (mostly STL/CommonLib template
helpers such as `vector::push_back`, `unordered_map::clear`, `steady_clock::now`)
is called in that TU. Cutting a TU in two changes those counts, so the same
source can come out with a different set of helpers inlined. It is not even
deterministic between two builds of the SAME commit (see self-test (a2)). So a
pair that is not byte-identical is **FAIL** unless it falls in one of these
classes, each listed in full in the output:

| Class | Accepted when |
|---|---|
| inline-drift | the pair's PDB inline-site lists differ (or the function is a library template whose COMDAT copy came from another TU), **and** its own-code fingerprint is equal |
| funclet-drift | an unwind funclet (`...'::`1'::dtor$N`) of a drift function, fingerprint equal |
| outlined | a body present in only one build, and INLINED (PDB inline site) in the other |
| copies | a header-defined internal-linkage symbol (e.g. `rapidcsv::s_Utf8BOM`'s initializer, one per including TU) whose copy count changed; every copy identical to one in the other build |

The **own-code fingerprint** is every reference a function makes outside its
own top-level inline sites (calls, data, string literals, RTTI), plus one
token per inlined callee. Two fingerprints are equal when every own-code token
of either side has an equivalent ANYWHERE in the other side, where "inlined X"
counts as "calls X". It is SET semantics and ignores immediates and control
flow, so on its own it cannot see a changed constant. That is why drift is
never accepted alone: `linecheck` must also show the source lines unchanged,
and a function whose inline sites are identical but whose bytes differ is
always FAIL (see self-test (b)). `--strict` disables every class.

## Self-tests (2026-09-24, wave-1 split)

- **(a)** two builds of source-identical trees (main `923e72d`, run
  36052504529, vs `bb105e0`, run 36051867561): **PASS**, 10136/10136
  functions identical, 685 named data identical.
- **(a2)** two builds of the SAME commit `923e72d` (run 36052504529 vs a
  `workflow_dispatch` rebuild, run 36057488831): **PASS-EXPLAINED**, 10134
  identical, 2 inline-drift, both STL (`std::_Integer_to_chars<unsigned char>`,
  `std::__p2286::_Write_escaped`: `mov ecx,eax` vs `mov rcx,rax`, identical inline
  sites). MSVC codegen for a template COMDAT is not fully deterministic, which is
  why the library-COMDAT drift class exists.
- **(b)** main vs a commit changing ONE constant (`Rapport.cpp`
  `kQuashPairCooldown` 2 s -> 3 s, `fe7b993`, run 36055282749): **FAIL**, naming
  exactly `MFO::Rapport::QuashAllyPair` (inline sites identical, so no class can
  explain it) and the data `MFO::Rapport::kQuashPairCooldown` (`0x02` vs `0x03`).
  Everything else identical.

## Allow-list

`--allow FILE`: TAB-separated lines `kind<TAB>regex<TAB>justification`, kind one
of `differ`, `only-a`, `only-b`, `data`, `any`. Every hit is printed with its
justification. Prefer fixing the tool over allow-listing.
