# Third-party notices

MFO's own source is MIT (see `LICENSE`) and lives entirely in `native/`, all
written for this project. **No third-party source is vendored into this
repository.** Dependencies are fetched at build time by vcpkg from the pinned
registries in `native/vcpkg-configuration.json`. The one class of third-party
*asset* that does ship in the repo is the two board fonts (below).

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

## Dear ImGui

The in-game board's renderer (with the `dx11-binding` and `win32-binding`
features). Fetched from vcpkg; direct dependency in `native/vcpkg.json`.

**MIT License — Copyright (c) 2014-2025 Omar Cornut**

The MIT text is identical to the block reproduced under CommonLibSSE-NG above,
with the copyright line replaced by the one shown here.

---

## nlohmann/json

The JSON reader for the item catalog (`mfo_items.json`) consumed at runtime.
Fetched from vcpkg; direct dependency in `native/vcpkg.json`.

**MIT License — Copyright (c) 2013-2025 Niels Lohmann**

MIT text as above, with this copyright line.

---

## spdlog

Logging. Pulled in transitively (CommonLibSSE-NG links it) and linked into the
binary. Fetched from vcpkg.

**MIT License — Copyright (c) 2016 Gabime**

MIT text as above, with this copyright line. spdlog bundles **fmt** (below).

---

## fmt

String formatting, bundled by spdlog and linked into the binary.

**Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors.**
Licensed under the MIT License. MIT text as above, with this copyright line.

---

## Fonts — SIL Open Font License 1.1

The board bakes two OFL fonts into its ImGui atlases at load, shipped under
`SKSE/Plugins/MFO/fonts/`:

- `head.ttf` — **Cinzel**, © 2012 Natanael Gama (Reserved Font Name *Cinzel*).
- `body.ttf` — **EB Garamond**, © 2017 Georg Duffner and Octavio Pardo
  (Reserved Font Name *EB Garamond*).

Both are used unmodified and are not renamed, so the Reserved Font Names are
respected. The full licence text and per-font copyright notices travel with
every release in [`OFL.txt`](OFL.txt), as OFL §2 requires.

---

## Note for the sibling projects

MRO, MEO, and MAO ship statically-linked CommonLibSSE-NG binaries and **none
of them carries a notices file.** The same obligation applies to each. This
file is a reasonable template to copy across.
