# Build, tests, CI, and release

[← Back to architecture index](DARKROOM.md)

Toolchain minimums (Qt 6.8, macOS 13.3) and their reasons are stated at the top of [DARKROOM.md](DARKROOM.md).

## Two qmake roots

| Project | Builds | Purpose |
|---|---|---|
| `Darkroom.pro` | Darkroom, Quickroom, the `Shared` library they both link, and all six submodule libraries | the product |
| `Tests.pro` | `cpputils`, `qtutils`, `Shared`, and `tests/` | the test binary builds and runs on its own, including when the apps' own code does not compile |

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

## vcpkg libraries

`vcpkg.json` lists the third-party libraries, LibRaw and libjpeg-turbo, at exact versions in its `overrides`; its
`builtin-baseline` pins their dependencies. Every platform links them statically.

- The scripts run `vcpkg install` into `vcpkg_installed/` before qmake. vcpkg comes from `VCPKG_ROOT`, else from `PATH`.
  A build started from an IDE needs that install done first.
- `vcpkg.pri` gives `Shared` the include path and the apps the libraries, debug or release by configuration. It selects
  the same triplet as the scripts.
- `vcpkg-triplets/` overrides the built-in triplets:
  - Windows builds with the v145 toolset; the apps must not use an older one.
  - macOS targets 13.3, as `global.pri` does.
  - x64 Linux and macOS fail rather than build libjpeg-turbo without SIMD, which needs `nasm` installed.
- Updating a library changes its override; a version newer than the baseline's also needs the baseline moved to a vcpkg
  commit that has it, which the local vcpkg checkout must contain.

## Shared library and tests

- `Shared/` builds a static library both apps and the tests link.
- qmake does not pass a static library's `LIBS` on: a library `Shared` starts using must be added to both apps' `LIBS`
  and to their `depends` in `Darkroom.pro`.
- Every static library a project links is in its `PRE_TARGETDEPS` on Unix, so make relinks when the library changes.
- Quickroom links the `file-operations` libraries through that submodule's `libraries/fileoperationsui.pri`, which
  adds their dependencies and include paths.

`tests/tests.pro` compiles in the Darkroom sources under test, the model half of `Core/` (`Catalog`, `CatalogIntegrity`,
`Library`, `MetadataStore`), and links `Shared` for the rest.

**The link trap:** the tests link only the `Shared` objects they reference. One of those, or a compiled-in source,
gaining a dependency on a library the tests do not link breaks the tests' link while the apps still build. CI builds tests before the apps,
so the failure appears on every OS and looks unrelated to the change. The fix is `tests.pro`'s `LIBS` plus `Tests.pro`'s
`depends` (without the latter the library is never built first). `Utils.cpp` is the usual entry point: its
error-reporting functions are why the tests link widgets and qtutils at all. Do not split it to suit the test project.

Test sources are listed explicitly in `tests/tests.pro`; a new test file must be registered there. Tests protect
silent breakage: persisted-format compatibility, identity invariants, catalog mutations, case-sensitive filesystem
behavior, and catalog-integrity verdicts.

## Image read benchmark

`scripts/image_read_benchmark.py` times reading image files cold: one screenful under the I/O strategies a loader can
use on slow storage, and read throughput by thread count. Windows only; `--help` lists the options. The results and the
method's validation: [image-read-performance.md](image-read-performance.md).

- `generate <root>` writes two folders of the same synthetic files, random bytes of 250-500 KiB named `*.jpg`:
  `name-order` is written in name order, as an import lays it out; `shuffled` in random order. A screenful is
  consecutive names: contiguous on disk in the first, scattered in the second.
- Variants: serial buffered reads (the loader's current path), the same sorted by first cluster, 4 threads buffered and
  unbuffered, unbuffered overlapped reads 4 and 8 in flight.
- `scripts/image_read_benchmark.ps1` owns the fixed VHDX the samples run on and needs an elevated prompt; start it
  through `powershell -ExecutionPolicy Bypass -File`. `-Setup` creates it and generates the files; `-Samples N -Out <csv>`
  takes N rounds, each timing every variant on both layouts in a shuffled order.
- A sample remounts the volume, which discards its file and metadata caches, lists the folder untimed, then reads a
  batch no earlier sample of the run read. With `--warm-metadata` it first opens every file of the batch untimed: cold
  data behind warm metadata.
- The drive's own cache survives a remount: each run first reads a filler file, which must exceed twice that cache.
- Confirm cold numbers are cold before trusting them: `buffered` on `shuffled` must take HDD time, over 10 ms per file,
  and the disk's read bytes must about equal the batch's. More bytes means another reader, such as a malware scanner.
- `threads <folder>` sweeps thread counts on the drive holding the folder, without a VHDX or elevation. Each trial
  writes fresh files unbuffered, so their data is cold and their metadata warm, then reads them unbuffered and buffered.
  `--cpus` confines the process to some logical CPUs.

## CI

`.github/workflows/CI.yml` runs on every push (except to `gh-pages`) and pull request, on Windows (MSVC), Ubuntu
(GCC 14), and macOS (clang), each against Qt 6.11. Per OS, in order:

1. Set up vcpkg at `vcpkg.json`'s baseline. Its built packages are cached across runs.
2. Build and run the tests: `run_tests`.
3. Build both apps, even when the tests failed: `build`. Windows pins the v145 toolset. The libraries the tests step
   built are reused: both roots build into the same directories.
4. Package whenever the apps built, so packaging is checked even when the tests failed: `windeployqt` into
   `dist/` and Inno Setup on Windows; `scripts/create_dmg.sh` on macOS. The packages are uploaded as workflow
   artifacts only when every step passed. Linux only builds.
5. Record metrics: LOC per project and executable sizes (per PE section on Windows) go to the `gh-pages` branch under
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
