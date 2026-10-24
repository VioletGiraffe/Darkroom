# Build, tests, CI, and release

[← Back to architecture index](DARKROOM.md)

Toolchain minimums (Qt 6.8, macOS 13.3) and their reasons are stated at the top of [DARKROOM.md](DARKROOM.md).

## Two qmake roots

| Project | Builds | Purpose |
|---|---|---|
| `Darkroom.pro` | Darkroom, Quickroom, the `Shared` library they both link, and all six submodule libraries | the product |
| `Tests.pro` | `cpputils`, `qtutils`, and `tests/` | the test binary builds and runs on its own, including when the app does not compile |

Every qmake and msbuild invocation names its project: `qmake -tp vc -r Tests.pro`, `msbuild Darkroom.sln /t:Darkroom`.
On Unix both roots generate the same top-level `Makefile`, so generating one overwrites the other; each script below
re-runs qmake for its own project. `global.pri` holds the compiler settings every project includes.

## Scripts

CI and local builds run the same scripts in `scripts/`; the `.bat` files start their `.ps1` counterparts.

| Script | Does | Arguments |
|---|---|---|
| `build.bat` / `build.sh` | qmake and build of `Darkroom.pro`; runs nothing | `darkroom` or `quickroom` builds one app, none builds both; rebuild of the whole project from scratch; debug configuration |
| `run_tests.bat` / `run_tests.sh` | qmake and build of `Tests.pro`, then `DarkroomTests` | debug configuration; build only or run only; arguments for the test binary |

- The Qt kit comes from `QT_ROOT_DIR`, else from a git-ignored `scripts/local-env.ps1` or `local-env.sh` that sets it;
  the `.sh` scripts also fall back to a `qmake6` or `qmake` on `PATH`.
- On Windows the scripts set up the MSVC environment through vswhere when `cl` is not on `PATH`.
- Exit codes: 1 for a build or test failure, 2 for an incomplete environment.

## Shared library and tests

- `Shared/` builds a static library both apps link.
- qmake does not pass a static library's `LIBS` on: a library `Shared` starts using must be added to both apps' `LIBS`
  and to their `depends` in `Darkroom.pro`.
- Quickroom links the `file-operations` libraries through that submodule's `libraries/fileoperationsui.pri`, which
  adds their dependencies and include paths.

The tests do not link `Shared`: `tests/tests.pro` compiles the sources under test directly, so the tests build even when
the rest of the apps' code does not. It takes the model half of `Core/` (`Catalog`, `CatalogIntegrity`, `Library`,
`MetadataStore` from `Darkroom/src`; `JsonPersistence` from `Shared/src`) plus `Utils.cpp` for its path helpers.
Its `LIBS` are hand-listed too.

**The link trap:** a source the tests compile gaining an `#include` of a submodule library the tests do not link breaks
the tests' link while the apps still build. CI builds tests before the apps, so the failure appears on every OS and looks
unrelated to the change. The fix is `tests.pro`'s `LIBS` plus `Tests.pro`'s `depends` (without the latter the library
is never built first). `Utils.cpp` is the usual entry point: its error-reporting functions are why the tests link
widgets and qtutils at all. Do not split it to suit the test project.

Test sources are listed explicitly in `tests/tests.pro`; a new test file must be registered there. Tests protect
silent breakage: persisted-format compatibility, identity invariants, catalog mutations, case-sensitive filesystem
behavior, and catalog-integrity verdicts.

## CI

`.github/workflows/CI.yml` runs on every push (except to `gh-pages`) and pull request, on Windows (MSVC), Ubuntu
(GCC 14), and macOS (clang), each against Qt 6.11. Per OS, in order:

1. Build and run the tests: `run_tests`.
2. Build both apps, even when the tests failed: `build`. Windows pins the v143 toolset.
3. Package whenever the apps built, so packaging is checked even when the tests failed: `windeployqt` into
   `dist/` and Inno Setup on Windows; `scripts/create_dmg.sh` on macOS. The packages are uploaded as workflow
   artifacts only when every step passed. Linux only builds.
4. Record metrics: LOC per project and executable sizes (per PE section on Windows) go to the `gh-pages` branch under
   `dev/metrics` through github-action-benchmark, which regenerates the chart there.

`cleanup.yml` is a manual run that deletes old workflow runs.

## Packaging

- **Windows:** `installer.iss` installs both exes from `dist/`, the VC++ redistributable, and the file-type icons
  from `Quickroom/res/filetypes/`. The version is read from the built `Darkroom.exe`, so `VERSION` in `Darkroom.pro` is the
  single source. `AppId` is fixed: changing it makes an upgrade install beside the old copy. The `[Registry]` section
  registers Quickroom as an eligible handler per supported type under a distinct ProgID each, so every type keeps its
  own icon; Windows forbids claiming the default handler programmatically, so the finish page offers an opt-in
  checkbox that opens Settings for the user to choose. It also adds "Browse with Quickroom" to the context menu of
  folders, folder backgrounds and drives.
- **macOS:** `scripts/create_dmg.sh <Qt dir>` runs `macdeployqt` on both already-built release bundles and packages
  them into one `Darkroom.dmg`. It treats macdeployqt's `ERROR` output as failure, since the tool exits 0 regardless.
