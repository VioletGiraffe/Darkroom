# Build, tests, CI, and release

[← Back to architecture index](DARKROOM.md)

Toolchain minimums (Qt 6.8, macOS 13.3) and their reasons are stated at the top of [DARKROOM.md](DARKROOM.md).

## Two qmake roots

| Project | Builds | Purpose |
|---|---|---|
| `Darkroom.pro` | Darkroom, Quickroom, and all five submodule libraries | the product |
| `Tests.pro` | `cpputils`, `qtutils`, and `tests/` | the test binary builds and runs on its own, including when the app does not compile |

Every qmake and msbuild invocation names its project: `qmake -tp vc -r Tests.pro`, `msbuild Darkroom.sln /t:Darkroom`.
On Unix both roots generate the same top-level `Makefile`, so generating one overwrites the other; CI re-runs qmake
in each step. `global.pri` holds the compiler settings every project includes.

## Source sharing without a library

The app is not a library. Two projects compile app sources directly and hand-list what they take:

- `tests/tests.pro` - the model half of `Core/` (`Catalog`, `CatalogIntegrity`, `JsonPersistence`, `Library`,
  `MediaId`, `MetadataStore`) plus `Utils.cpp` for its path helpers. Its `LIBS` are hand-listed too.
- `quickroom/quickroom.pro` - `Core/`, `Theme/`, and `crashhandler/` whole, plus the closure of the shared viewer and
  player windows. Its comments name the list.

**The link trap, shared by both:** a compiled app source gaining an `#include` of a submodule library that the project
does not link breaks that project's link while Darkroom still builds. CI builds tests before the app, so the failure
appears on every OS and looks unrelated to the change. The fix for tests is `tests.pro`'s `LIBS` plus `Tests.pro`'s
`depends` (without the latter the library is never built first); for Quickroom, `quickroom.pro`'s `LIBS` plus
`Darkroom.pro`'s `quickroom.depends`. `Utils.cpp` is the usual entry point: its error-reporting functions are why the
tests link widgets and qtutils at all. Do not split it to suit the test project.

Test sources are listed explicitly in `tests/tests.pro`; a new test file must be registered there. Tests protect
silent breakage: persisted-format compatibility, identity invariants, catalog mutations, case-sensitive filesystem
behavior, and catalog-integrity verdicts.

## CI

`.github/workflows/CI.yml` runs on every push (except to `gh-pages`) and pull request, on Windows (MSVC), Ubuntu
(GCC 14), and macOS (clang), each against Qt 6.11. Per OS, in order:

1. Build and run the tests from `Tests.pro`.
2. Build both apps from `Darkroom.pro`, even when the tests failed.
3. Package whenever the apps built, so packaging is checked even when the tests failed: `windeployqt` into
   `dist/` and Inno Setup on Windows; `scripts/create_dmg.sh` on macOS. The packages are uploaded as workflow
   artifacts only when every step passed. Linux only builds.
4. Record metrics: LOC per project and executable sizes (per PE section on Windows) go to the `gh-pages` branch under
   `dev/metrics` through github-action-benchmark, which regenerates the chart there.

`cleanup.yml` is a manual run that deletes old workflow runs.

## Packaging

- **Windows:** `installer.iss` installs both exes from `dist/`, the VC++ redistributable, and the file-type icons
  from `quickroom/res/filetypes/`. The version is read from the built `Darkroom.exe`, so `VERSION` in `app.pro` is the
  single source. `AppId` is fixed: changing it makes an upgrade install beside the old copy. The `[Registry]` section
  registers Quickroom as an eligible handler per supported type under a distinct ProgID each, so every type keeps its
  own icon; Windows forbids claiming the default handler programmatically, so the finish page offers an opt-in
  checkbox that opens Settings for the user to choose.
- **macOS:** `scripts/create_dmg.sh <Qt dir>` runs `macdeployqt` on both already-built release bundles and packages
  them into one `Darkroom.dmg`. It treats macdeployqt's `ERROR` output as failure, since the tool exits 0 regardless.
