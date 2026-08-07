# Darkroom architecture

Darkroom is a C++/Qt 6 desktop application for organizing videos and photos, extracting video frames through ffmpeg,
and comparing media. Qt 6.8 or newer is required for presenting decoded cache images through
`QVideoFrame(QImage)`.

This file is the architectural entry point. Subsystem documents contain structure, rationale, and invariants; code
contains implementation detail. Keep both in sync.

## Working route

1. Read [the coding conventions](docs/guidelines.md).
2. Follow the relevant subsystem link below and inspect the implementation plus nearby examples.
3. Before changing QSS, `QComboBox`, or custom styling, read
   [the Qt styling-system quirks](docs/tips/qt-styling-system-quirks.md).
4. Keep changes in the app unless responsibility genuinely belongs in a shared submodule, and update durable
   architectural facts when they change.

## Build and source layout

The qmake root `Darkroom.pro` builds the app and its static-library submodules: `qtutils`, `cpputils`,
`cpp-template-utils`, and `magic-alignment`. App sources and headers are discovered recursively under
`app/src/`, so new files there require no project registration. Generated solutions, Makefiles, IDE state,
`bin/`, and `build/` are not sources of truth.

`app/src/` is divided into:

- `Core/` for the library, catalog, persistence, identity, and I/O routing.
- `UiComponents/` for reusable widgets and feature composites.
- `Windows/` for top-level windows, dialogs, and interactive workflows.
- `Theme/` for palette and application styling.
- Root modules for settings, utilities, ffmpeg, import workers, and application startup.

App includes are layer-qualified from `src`, such as `"Core/Catalog.h"` and
`"UiComponents/MediaItemWidget.h"`. Submodule headers follow their configured include roots.

`tests/tests.pro` builds the Catch2 core test executable. Tests protect silent breakage: persisted-format
compatibility, identity invariants, catalog mutations, case-sensitive filesystem behavior, and catalog-integrity
verdicts. Test sources are listed explicitly and new test files must be registered there.

## Cross-cutting principles

- **Catalog is the authoritative media-item and label model.** It is keyed by `MediaId`, maintained through its
  mutation API, and never reconstructed from disk during normal refresh. Disk reconciliation is explicit.
- **Library is the stable root-bound owner.** MainWindow owns one immovable `Library`; a complete private state
  containing root, MetadataStore, and Catalog is replaced transactionally. Persistent collaborators borrow
  `Library&`; narrow direct borrows cannot span `setRoot`.
- **`Library::catalogChanged` is the stable invalidation seam.** Persistent views subscribe to Library rather than
  the replaceable Catalog state.
- **JSON has one checked boundary.** Candidate files are distinguished as absent, invalid, or valid before model
  construction. Atomic write failure remains dirty and retryable and blocks silent state loss.
- **Labels own no disk objects.** A stored item's location may imply one ordinary label, but that relationship belongs
  to the item. Catalog refuses ambiguous mutations, item orphaning, unsafe label paths, and destructive registry
  changes whose relocation did not complete.
- **Container replacement must preserve capabilities.** Selection, keyboard navigation, drag-and-drop, focus, and
  other behavior supplied by the old widget need explicit equivalents in the new one; see
  [main-window.md](docs/architecture/main-window.md#media-grid--multi-select).
- **Background disk reads use `Core::IoThreadPool`.** Fast storage receives bounded parallel reads; slow,
  external, network, or unknown storage shares a serial worker to avoid seek thrashing. CPU decode follows on a
  compute pool. Long-running non-read work owns an appropriate separate worker instead of blocking the I/O route.
- **Language and framework conventions live in [guidelines.md](docs/guidelines.md).**

## Subsystems

### [Data model and identity](docs/architecture/data-model.md)

Stable Library lifetime and root replacement, library layout, `MediaId` and `LabelId`, dumb per-item
`MetadataStore` persistence, nested writers, and checked JSON publication.

### [Catalog and labels](docs/architecture/catalog-and-labels.md)

The authoritative item/label model, stable label ids, storage-label reconciliation, mutation and relocation
invariants, verified paths, batching, empty-label persistence, and explicit integrity checking.

### [Main window](docs/architecture/main-window.md)

Library startup and switching, browser update layers, lazy grid population, native multi-selection, item and label
workflow ownership, rename transactions, and diagnostic logging.

### [Media cards and thumbnails](docs/architecture/media-widgets.md)

`MediaItemWidget`, `ThumbnailWidget`, label-drop and drag behavior, asynchronous read/decode ownership, rendering
invariants, and card geometry controls.

### [Frame viewing, playback, and photo comparison](docs/architecture/playback.md)

Persistent frame viewing, built-in playback and saved loops, oscillating cache presentation, single-frame extraction,
and PhotoCompareWindow's shared-view/per-photo-alignment model.

### [Settings and theme](docs/architecture/settings-and-theme.md)

`QSettings` ownership, SettingsDialog behavior, the light/dark color system, central application styling, and custom
controls used where platform widgets cannot express the intended visuals.

### [Import and frame extraction](docs/architecture/import.md)

UI-free photo/video workers and interactive coordination, preview generation and reuse, transactional on-demand full
extraction, ImportDialog staging and provisional labels, duplicate boundaries, relocation, and catalog publication.

## Improvement backlog

**Open:** None.

**Decided against or deferred:**

- **Async full-frame ffmpeg:** synchronous extraction was judged acceptable for its explicit workflows.
- **Deduplicate zoom persistence/debounce:** three short consumers remain clearer than an abstraction. Revisit if a
  fourth consumer appears or the behavior grows.
