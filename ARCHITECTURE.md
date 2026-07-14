# Darkroom — Architecture

C++/Qt6 desktop app (Windows primary target) for organizing a local library of video frames and extracting frames
from source videos via ffmpeg. Requires Qt 6.5+ (uses `QGuiApplication::styleHints()->colorScheme()`).

This document is the living architectural reference: a high-level index plus the core cross-cutting
principles. Per-subsystem depth lives in `docs/architecture/*.md`, linked below — read this file first to
orient, then follow the link for the subsystem you're touching.

> **Keep this file and the linked docs in sync.** When a change alters architecture, a module's behavior, a
> convention, or the backlog, update the relevant doc in the same change. An out-of-date doc is worse than
> none - treat the update as part of the work rather than afterthought.

---

## Build & dependencies

- **Build system**: qmake, top-level `Darkroom.pro` (`SUBDIRS`): the app itself (`app/app.pro`) plus the
  static-library submodules it links — `qtutils`/`cpputils`/`cpp-template-utils` and `magic-alignment`
  (a Qt-only automatic image-alignment library used by `PhotoCompareWindow`, same `$$files` glob convention).
  Sources and headers come from a recursive glob evaluated each time qmake runs —
  `SOURCES += $$files(src/*.cpp, true)`, `HEADERS += $$files(src/*.h, true)` + `$$files(src/*.hpp, true)`.
  Because the glob walks the *whole* `src/` subtree, a new file dropped anywhere under it is enumerated
  automatically. **New source files do not need to be registered in any way**.
  (`Q_OBJECT` types are moc'd automatically because the headers are globbed in the same way.)
- **The Visual Studio IDE project is auto-generated and git-ignored, do not edit any related files and also do not use as source of truth.** 
- **Source layout** (under `app/src/`): reusable UI widgets in `UiComponents/` (`ThumbnailWidget`,
  `MediaItemWidget`, `MarkerSlider`, `SortControl`, `SegmentedToggle`, `LabelSidebar`) plus their close UI
  helpers (`LabelVisuals`, `DragGestureHelper`, `LabelMimeType`); top-level windows + dialogs in `Windows/`
  (`MainWindow`, `CompareWindow`, `PhotoCompareWindow`, `FrameViewerWindow`, `VideoPlayerWindow`, the `*Dialog`s); the non-UI core
  model in `Core/` (`Catalog`, `MetadataStore`, `MediaId`); and the visual theming in `Theme/` (`Theme`,
  `Style`). `Settings`, `Utils`, `Ffmpeg`, `Import`, and `main.cpp` stay at the `src/` root.
- **INCLUDEPATH**: `src` plus the submodules `qtutils`, `cpputils`, `cpp-template-utils`, and
  `magic-alignment/src` (its headers are included unqualified: `"MagicAlignment.h"`). With `src` on the
  path, app headers are included **layer-qualified** — `"UiComponents/ThumbnailWidget.h"`,
  `"Windows/MainWindow.h"`, `"Core/Catalog.h"`, and the few root headers as `"Utils.h"` — regardless of the
  including file's location; submodule headers likewise drop their prefix (`qtutils/widgets/layouts/cflowlayout.h`
  → `"widgets/layouts/cflowlayout.h"`).
- Don't build/compile here — the toolchain (Qt, compiler) isn't in this environment by design; reason about
  correctness by inspection and hand off.

---

## Core principles

A handful of cross-cutting rules that apply beyond any one subsystem. Check these before writing new code,
not just when reading existing code.

- **`Catalog` is the authoritative in-memory model of the media-item set, keyed by `MediaId`.** It is kept current
  by its own mutation API (`addMediaItem`/`removeMediaItem`/`applyRename`/`addLabel`/...), not by re-deriving from
  disk on every refresh — disk is only walked once, at the legacy seed. See
  [data-model.md](docs/architecture/data-model.md) and [catalog-and-labels.md](docs/architecture/catalog-and-labels.md).
- **A label owns nothing on disk.** The folder an item's frames sit in happens to share a name with one of
  the item's labels — that's a per-item storage detail, never a property stored on the label itself. See
  [catalog-and-labels.md](docs/architecture/catalog-and-labels.md).
- **`Catalog`'s mutation API refuses or no-ops on ambiguity rather than silently deleting data.** No
  operation will orphan an item (leave it with zero ordinary labels) or silently drop a registry entry that
  still has a backing folder. See
  [catalog-and-labels.md](docs/architecture/catalog-and-labels.md#fs-reconciliation-audit-done--findings).
- **When swapping a container/widget for a different shape or look, list what the old one gave for free**
  (selection model, keyboard nav, drag-and-drop, focus handling) and confirm each still has an equivalent
  before calling the swap done. See
  [main-window.md](docs/architecture/main-window.md#media-grid--multi-select) for the multi-select
  regression this caught once already.
- **All background disk reads go through `Core/IoThreadPool`** — the process-wide serial I/O executor
  (a single worker thread, so parallel loads can't seek-thrash a spinning disk). Post the file read there
  (`enqueue`, optionally tagged so an owner's destructor can `retire()` its tasks), and hand the CPU-bound
  decode to a compute pool. See the two-stage read→decode pattern in
  [media-widgets.md](docs/architecture/media-widgets.md#loading).
- **Language/framework coding conventions live in [guidelines.md](docs/guidelines.md)** — assertions
  (`assert_r`, never `<cassert>`), containers (`std::vector` over `QList`), identity (`MediaId` over path),
  dialog flush via `done()`, natural sorting, `tr()` i18n, and QSS gotchas. Read before writing new code.

---

## Subsystems

### [Data model & identity](docs/architecture/data-model.md)
The on-disk structure (`rootFolder()`, storage folders, frame folders), the `MediaId` identity scheme, and
`MetadataStore` — the dumb `MediaId`-keyed persistence layer (with batched-write support) that `Catalog`
loads itself from and writes through.

### [Catalog & labels](docs/architecture/catalog-and-labels.md)
`Catalog`: the in-memory media-item-set model plus the label model layered over it. Stable label ids, the
`labels.json` registry, the folder-reconciliation model (exactly 3 touch points), the `MediaId`-anchored
query/mutation API, import lifecycle (`addMediaItem`/`removeMediaItem`/`applyRename`, the duplicate-id guard,
`BatchScope`), the FS-reconciliation audit findings, the design rationale, and deferred post-v1 polish.

### [Main window](docs/architecture/main-window.md)
`MainWindow`'s grid + sidebar layout, the name filter, the All/Videos/Photos media-type switch (and how
photo cards render), label assignment (context menu + drag-from-sidebar), sidebar label management
(rename/color/delete), the media grid's multi-select implementation (and the regression history behind it),
and renaming a media item on disk.

### [Media item card & thumbnail widgets](docs/architecture/media-widgets.md)
`MediaItemWidget` (the grid card: label drop target, label dots, no longer a drag source),
`ThumbnailWidget` (sizing model, intrinsic drag, two non-obvious rendering bug fixes), `DragGestureHelper`,
and the card zoom/preview-count mechanism.

### [Frame viewer & video player](docs/architecture/playback.md)
`FrameViewerWindow` (persistent per-folder thumbnail popup), `VideoPlayerWindow` (built-in player: seek,
A–B loop, saved loops persisted per-video), `MarkerSlider`, and `PhotoCompareWindow` (N-way photo compare:
one shared zoom/pan view + a per-photo alignment transform, one-click auto-align via the `magic-alignment`
library, two-point calibration, flicker / difference / full-view comparison modes, drop-to-add photos).

### [Settings & theme](docs/architecture/settings-and-theme.md)
The `Settings.h`/`QSettings` key pattern, `SettingsDialog`, the `Theme` dark/light color system (incl. the
app-wide `Accent`/`AccentBg` tokens), and the central `Style` stylesheet + custom-widget approach (e.g.
`SegmentedToggle`) that gives the app its non-stock look.

### [Import: the Import module, ffmpeg, Utils, ImportDialog](docs/architecture/import.md)
`Import::importVideo` and `Import::importPhoto` — the per-item import workers (`MainWindow::importVideoBatch` /
`importPhotoBatch` remain the batch coordinators over them; photos land in `<root>/Photos/<label>/` or are
referenced in place, with collision auto-rename), the `ffmpeg` invocation (`Ffmpeg::generatePreviewFrames` —
a batch/concurrent preview extractor that `ImportDialog`'s staging runs across several videos at once,
whose frames import then reuses by copy instead of re-extracting), `Utils.h`'s grab-bag of free functions,
and `ImportDialog` itself: a staging grid + label-list panel mirroring the main window's own label
model, duplicate detection at staging and relocation, and the "Import" command that imports every labeled
card in one step.

---

## Improvement backlog

**Open:**
- *Batch ffmpeg failures*: `importVideoBatch` pops one modal `QMessageBox` per failure inside the loop, blocking
  the batch. Collect failures, show one summary at the end.
- *Label-reference validation*: `CatalogIntegrity::scan` checks catalog-vs-disk but not label integrity — an
  item whose stored `"labels"` id matches no registry label is neither flagged nor fixable. Add a "dangling
  label reference" verdict to the scan (resolution: drop the dead id). Low urgency — the mutation API already
  prevents it (`deleteLabel` strips references), so it'd arise only from external tampering or a bug — but it's
  the one catalog-consistency dimension the integrity tool doesn't cover.

**Decided against / deferred (don't re-litigate without new info):**
- *Async ffmpeg* — synchronous `waitForFinished` blocking the GUI was judged a non-issue, not worth doing.
- *Dedup the 2× zoom bound/persist/debounce shape* — deferred; 2 short obvious methods beat the indirection.
  Revisit at a 4th consumer or if the shape grows.
