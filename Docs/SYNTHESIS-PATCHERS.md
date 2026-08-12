# Synthesis Patchers — full process reference

How our Synthesis patcher (MFO.Synthesis) is built, distributed, kept building
across Synthesis updates, and listed in the in-app patcher browser.

## 1. What Synthesis is

[Synthesis](https://github.com/Mutagen-Modding/Synthesis) is a Mutagen-based
patcher pipeline with a GUI. The user assembles a list of patchers; on Run,
Synthesis reads the load order, executes each patcher in order (each sees the
load order plus the previous patchers' output), and emits one generated output
plugin (`Synthesis.esp` by default) placed at the end of the load order. A
patcher is ordinary C# code against the Mutagen record model — ours also writes
side files (MFO's item catalog JSON) in addition to record patches.

## 2. Distribution models — ours is a git patcher

Synthesis supports three patcher types (docs: “Patcher Types”):

- **Git Repository Patcher** — user gives a GitHub URL (or picks from the
  in-app browser); Synthesis **clones the repo anonymously and builds it from
  source on the user's machine** with its bundled .NET SDK.
- **External Program Patcher** — a pre-built exe the user points at.
- **Local Solution Patcher** — dev-mode, points at a local `.sln`.

MFO.Synthesis (`installer/MFO.Synthesis/MFO.Synthesis.csproj`, C#/Mutagen) is a
**git patcher**. Consequences:

- The repo **MUST be public**. Synthesis clones anonymously; a private repo
  fails *silently* — the project dropdown is just empty (we hit this).
- Users build our source; there is no binary to ship. Every push to the tracked
  branch is a potential update for every user.

## 3. The .NET / framework-version dependency (#1 maintenance gotcha)

Synthesis itself runs on a specific .NET major and builds git patchers with
that SDK. **The patcher csproj's `TargetFramework` and Mutagen package
versions must match the user's installed Synthesis**, or the build lands in
the wrong place:

- **Symptom** (hit live on MFO): Synthesis builds the patcher to
  `bin/Release/<old-tfm>/win-x64/` while its runner looks in
  `net10.0/win-x64/` → `FileNotFoundException: Compiled git patcher
  executable not found at ...\net10.0\...`.
- **Fix**: bump **all three together** — `TargetFramework` +
  `Mutagen.Bethesda.Synthesis` + `Mutagen.Bethesda.Skyrim`. Current pairing
  (Synthesis 0.36.6, verified 2026-08-11): **net10.0 / 0.36.6 / 0.54.4**.
  MFO.Synthesis is already on this pairing.
- **This recurs on every Synthesis .NET-major bump (~annual).** When Synthesis
  releases against a new .NET, every git patcher pinned to the old TFM breaks
  with the exact symptom above until re-pinned.
- **Verifying without the matching SDK**: you can't run the new TFM locally
  without the new SDK, but you can build the patcher code against the new
  Mutagen packages on an older TFM to confirm the API surface still compiles —
  that catches Mutagen breaking changes, which are the real risk in the bump.
- Sibling repos: MEO and MAO carry the same patcher pattern; when one breaks
  on a Synthesis bump, fix all three (MEO/MAO were still net9.0/0.36.5/0.54.2
  as of 2026-08-11).

## 4. How users add, run, and update the patcher

- **By git URL**: Git Repository Patcher → paste
  `https://github.com/marthofdoom/MFO` → pick `MFO.Synthesis` from the project
  dropdown. Works whether or not we're listed in the browser.
- **From the in-app browser**: same thing, pre-filled — but only
  registry-listed repos appear (section 5).
- **Versioning** (docs: “Versioning”): Synthesis pins a specific commit. In
  branch mode, new pushes show as an available update (blue arrow) the user
  must accept, unless they enabled Auto; tag mode follows semver tags
  (`v1.0.0`-style) instead. **A plain re-run rebuilds the same pinned commit —
  users must update the patcher to pull a new one.** So: pushing a fix does
  not silently reach users; release notes should say "update the Synthesis
  patcher".

## 5. Getting listed in the in-app patcher browser

The browser is fed by
[Mutagen-Modding/Synthesis.Registry](https://github.com/Mutagen-Modding/Synthesis.Registry)
(`mutagen-automatic-listing.json`), populated by a scraper GitHub Action that
runs **hourly**.

**There is no GitHub-topic mechanism.** (Older recollections of a repo
topic/tag are stale.) Verified in the scraper source
(`Synthesis.Registry/Listings/GitHubDependentListingsProvider.cs`): discovery
works by scraping **GitHub's dependency-graph "dependents" page for the
`Mutagen.Bethesda.Synthesis` NuGet package**
(`github.com/mutagen-modding/synthesis/network/dependents?package_id=...`).

To be crawled, a repo therefore needs:

1. **Public GitHub repo** (also required just to be a git patcher).
2. A `.csproj` referencing **`Mutagen.Bethesda.Synthesis`** — pushed, and
   **indexed by GitHub's dependency graph**. If the graph is disabled or
   hasn't indexed the manifest, the scraper cannot see the repo. Check with:
   `gh api repos/marthofdoom/MFO/dependency-graph/sbom` — a 404 means the
   dependency graph is off (enable: repo Settings → Advanced Security →
   Dependency graph); success should list `Mutagen.Bethesda.Synthesis`.
3. Optionally **`SynthesisMeta.json` next to the `.csproj`** — controls how
   the listing looks. Schema (fields observed in the live registry +
   Publishing docs):

   ```json
   {
     "Nickname": "marth Follower Overhaul (MFO)",
     "Visibility": "Visible",            // Visible | IncludeButHide | Exclude
     "OneLineDescription": "...",
     "LongDescription": "...",
     "PreferredAutoVersioning": "Default",
     "RequiredMods": ["MFO.esp"],
     "TargetedReleases": []
   }
   ```

   Without it the patcher still lists, under its project name.
4. **No PR/submission step is required** — topic-free, fully automatic. A
   manual fallback exists for repos the dependents page misses: PR your
   `{User, Repository}` into
   `Synthesis.Registry/Synthesis.Registry/mutagen-manual-dependents.json`.

**Update propagation to the listing**: the hourly scraper re-records the repo
head SHA and re-reads `SynthesisMeta.json` — push and wait an hour; no
re-registration, tag, or manifest bump needed.

## 6. This repo (MFO) — status + checklist

Status verified 2026-08-11:

- [x] Repo public (`marthofdoom/MFO`)
- [x] `installer/MFO.Synthesis/MFO.Synthesis.csproj` references
      `Mutagen.Bethesda.Synthesis` (nested path is fine — MEO/MAO are listed
      with `installer/<X>.Synthesis/` paths)
- [x] `SynthesisMeta.json` present next to the csproj
- [x] csproj on the current pairing net10.0 / 0.36.6 / 0.54.4
- [ ] **NOT LISTED — root cause: MFO's GitHub dependency graph is not
      enabled/indexed** (`gh api repos/marthofdoom/MFO/dependency-graph/sbom`
      → 404, while MEO's returns `Mutagen.Bethesda.Synthesis`). Fix: enable
      Dependency graph in repo Settings → Advanced Security, confirm the SBOM
      endpoint lists `Mutagen.Bethesda.Synthesis`, then wait for an hourly
      scrape and check
      `https://raw.githubusercontent.com/Mutagen-Modding/Synthesis.Registry/master/mutagen-automatic-listing.json`
      for `marthofdoom/MFO`. If it still doesn't appear, use the
      manual-dependents PR fallback (section 5.4).

On every Synthesis release: check its .NET major; if bumped, re-pin all three
repos' csprojs (section 3) before users hit the executable-not-found error.

## Sources / verified on 2026-08-11

- https://mutagen-modding.github.io/Synthesis/devs/Publishing/ (listing criteria, SynthesisMeta.json)
- https://mutagen-modding.github.io/Synthesis/Versioning/ (user-side update flow)
- https://github.com/Mutagen-Modding/Synthesis.Registry (README; scraper source `Listings/GitHubDependentListingsProvider.cs`; `mutagen-manual-dependents.json`; `.github/workflows/scraper.yml` hourly cron)
- https://raw.githubusercontent.com/Mutagen-Modding/Synthesis.Registry/master/mutagen-automatic-listing.json (live registry — MEO/MAO entries confirmed, schema cross-checked)
