# Third-party notices

MFO's own source is MIT (see `LICENSE`) and lives entirely in `native/` —
five files, all written for this project. **No third-party source is vendored
into this repository.** Dependencies are fetched at build time by vcpkg from
the pinned registries in `native/vcpkg-configuration.json`.

However, `MFO.dll` is built with the `x64-windows-static-md` triplet, so its
dependencies are **statically linked into the shipped binary**. MIT requires
its copyright notice and licence text to accompany "copies or substantial
portions of the Software", and a statically-linked binary is a copy. This
file travels with every release for that reason.

`native/vcpkg.json` is the authoritative dependency list; this file must be
updated whenever it changes.

---

## CommonLibSSE-NG

The SKSE plugin framework. Fetched from the colorglass vcpkg registry
(`https://gitlab.com/colorglass/vcpkg-colorglass`), which packages
`CharmedBaryon/CommonLibSSE-NG`, itself a fork of
`Ryan-rsm-McKenzie/CommonLibSSE`.

**MIT License — Copyright (c) 2018 Ryan-rsm-McKenzie**

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Transitive dependencies — OUTSTANDING

CommonLibSSE-NG pulls in further libraries (spdlog and fmt among them) which
are also linked into the binary. **These have not yet been individually
verified and are not yet reproduced here.**

**This must be resolved before any public release.** Enumerate the real
linked set from the vcpkg install tree rather than from memory, and reproduce
each licence verbatim from its own repository. Assuming a licence is the same
mistake as assuming an engine mechanism.

## Planned additions (not yet dependencies)

The ImGui board (roadmap M7) will add **Dear ImGui**. Add its notice in the
same commit that adds it to `vcpkg.json`, not afterward.

---

## Note for the sibling projects

MRO, MEO, and MAO ship statically-linked CommonLibSSE-NG binaries and **none
of them carries a notices file.** The same obligation applies to each. This
file is a reasonable template to copy across.
