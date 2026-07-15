# Import: the Import module, ffmpeg, Utils, ImportDialog

[← Back to architecture index](../../ARCHITECTURE.md)

## Full frame extraction — on-demand, not at import

`MainWindow::splitVideoIntoFrames` extracts a candidate complete real frame set. It's never called during
import anymore (see "Import: preview frames only" below) — `resplitVideoIntoFrames` uses it for the first
on-demand split, Re-export all, and the integrity tool's ghost-reimport.
`reExportAllVideos` skips photo entries when collecting its worklist — a photo has no frames, and its
"folder" is the shared `Photos/<label>` dir, which frame-folder replacement would destroy.
- The raw ffmpeg invocation lives in `Ffmpeg::splitVideoIntoFrames` (the module owns *all* ffmpeg calls now);
  `MainWindow::splitVideoIntoFrames` is a thin wrapper that passes the settings-derived options, maps the
  returned `SplitResult` to a dialog, and returns whether candidate extraction succeeded. It runs ffmpeg synchronously,
  freezing the UI during extraction — decided not worth making async (see
  [backlog](../../ARCHITECTURE.md#improvement-backlog)). Frame stepping (keep every Nth frame) and the
  user-configured full-split JPEG quality apply here; preview frames (below) are separate, with their own
  fixed quality.
- Only `resplitVideoIntoFrames` publishes success via `Catalog::markSplitComplete`, after the new folder and
  its preview policy have reached the transaction's commit point. Extraction itself never mutates the catalog.

`MainWindow::ensureFramesSplit(id)` is the *only* place that triggers a full split on demand: if
`!Catalog::isSplitIntoFrames(id)`, it calls `resplitVideoIntoFrames(id, /*preserveExistingPreview=*/true)`.
Wired into the grid card's middle-click handler, right before it opens `FrameViewerWindow` — opening the
viewer is what "pays for" a video's full split, and a failed split no longer opens it. Deliberately not wired
into `CompareWindow`: triggering a synchronous multi-second extraction from a
side-effect of a multi-select action was judged too easy to hit by accident; a not-yet-split video there
just shows whatever "no image" state an empty frame list already produces.

`MainWindow::resplitVideoIntoFrames(id, preserveExistingPreview)` transactionally replaces an existing frame
folder. It resolves the source/output paths from the stable catalog id, renames the complete old folder to a
unique sibling, extracts the candidate at the original path, and keeps a scope-guarded rollback armed until
the candidate is usable. Any extraction or preview-transfer failure removes the candidate and renames the
complete old folder back. Failed initial preservation aborts untouched; failed rollback or post-commit backup
cleanup reports the exact recovery path. The same-parent rename is cheap, but the old and new frame sets coexist
during extraction; low peak disk usage is not a design goal.

`preserveExistingPreview` controls the candidate's preview before commit:
- **`true`** (`ensureFramesSplit` only) — `preview/` is still fresh from import, nothing changed that
  would make it stale, so the wrapper moves it from the preserved old folder into the successful candidate.
- **`false`** (`reExportAllVideos`, ghost-reimport) — the caller explicitly wants a clean start (re-export
  may follow a settings change; a ghost's frames disappeared for an unknown reason), so a fresh one is generated
  via `Ffmpeg::generatePreviewFrames`. A requested preserved preview that does not exist takes this same fallback.

Neither path is used by `Import::importVideo`'s own overwrite-existing-folder path, which can be replacing a stale
folder left behind by a completely different prior video — there's no related `preview/` worth preserving or
regenerating-in-place there; it just goes through the normal import flow from scratch.

## Import: preview frames only, full split deferred

`Import::importVideo` (`src/Import.h/.cpp`, a free-function module in the same style as `Ffmpeg`) is the
per-item import worker. It is deliberately UI-free — it never prompts or pops a message box; every outcome
comes back as an `Import::Result` (`Success` / `FolderConflict` / `Error` with a user-presentable message).
`MainWindow::importVideoBatch` stays the batch coordinator on top of it and owns all the import UI: the app-wide
`m_isProcessing` lock shared with `reExportAllVideos`, the progress modal, the folder-conflict
partition/prompt (a `FolderConflict` result — the output folder exists and overwrite wasn't granted, nothing
touched — is resolved by asking the user about that one item and retrying the call with
`overwriteExisting = true`), the per-item error boxes, `Catalog::BatchScope`, and the view refresh. The
worker reads the preview frame count straight from `Settings::PreviewFrameCount` — the same source the Import dialog's
staging uses (the main window's combo persists every change there immediately).

It does not extract the full frame set. It creates the output folder, puts a few
permanent preview frames into `outputFolder/preview/`, then registers the video via `Catalog::addMediaItem(...,
/*splitIntoFrames=*/false)` immediately — the video appears in the grid right away with no badge yet (the green
"frames extracted" marker appears only once the full set exists; see [media-widgets.md](media-widgets.md)), and
the expensive full extraction only happens later via `ensureFramesSplit`. `importVideoBatch`'s progress text reads "Adding video X/Y...", since no
full extraction happens here.

Those preview frames are normally *reused, not re-extracted*: `ImportDialog` already ran ffmpeg to build
each staged card's preview (see "Staging area" below), so import is handed each video's staging temp dir —
keyed by the stable `MediaId` so it survives relocation moving the file — and copies those frames into
`outputFolder/preview/`. A fresh extraction runs only as a fallback, when nothing staged is available. The **duration** the Import dialog
probed while staging rides along the same handoff — passed to `importVideo` and stored on the item, so it isn't
probed a second time; the fresh-extraction fallback probes anyway and its result supersedes an unknown staged
value. On a registration collision the whole output folder (previews included) is deleted, the same as any
other import failure.

## Photo import: `Import::importPhoto`

`Import::importPhoto(labelPhotoFolder, photoPath, mode)` is the photo counterpart to `importVideo` — equally
UI-free, taking the verified destination produced by `Catalog::photoFolderForLabel` and returning an
`Import::PhotoResult` (`Success` / `IdCollision` / `Error` + message + the
**`registeredId`** actually registered). `MainWindow::importPhotoBatch` is its coordinator (batch scope,
error boxes, view refresh), driven from the Import dialog via the `importPhotosRequested` callback. The
`PhotoImportMode` comes from the Import dialog's existing relocation combo, which doubles as the photo import
mode: **Copy/Move** land the file in `<root>/Photos/<label>/` (created lazily), **"leave in place" means
Reference** — the file is tracked where it is and the catalog entry is marked referenced
(`Catalog::addPhoto`, see [catalog-and-labels.md](catalog-and-labels.md)).
Owned-photo Move uses `QFile`'s member `rename()`: it takes the cheap native rename path on one filesystem
and performs Qt's copy/remove fallback across filesystems, restoring the source-only state if that fallback
cannot finish.

- **Owned import auto-rename**: the destination name is chosen so that both the file path and the name+size
  `MediaId` are free — one rename (`name_2.ext`, `name_3.ext`, ...) resolves a disk collision and a catalog
  id collision alike, since a copy/move preserves the byte size. A byte-identical file already at the
  destination (including the file itself on a re-import) is *adopted* — registered as-is, not copied again.
  Because the registered identity can differ from the staged file's, all of the Import dialog's post-import
  bookkeeping (Best, extra labels) re-keys to `PhotoResult::registeredId`.
- **Reference import collision**: a referenced photo has no owned copy to rename, so an id already tracked
  as a different item is refused with `IdCollision`; the Import dialog then offers the escape hatch — "import an
  owned copy instead?" — which routes the file through the Copy path where the auto-rename works.
- **A referenced photo's first label** is applied by `importPhotoBatch` via `Catalog::addLabel` right after
  registration (it has no storage folder to derive a label from); an owned photo's first label derives from
  the `Photos/<label>` dir it landed in, exactly like a video's storage folder.

`Ffmpeg::generatePreviewFrames` (`src/Ffmpeg.h/.cpp`, free functions) is that extraction engine, used whenever
a fresh preview is needed — the fallback above, the re-split paths, and the Import dialog's staging. Given a video's
duration (probed from ffmpeg's own output, since no `ffprobe` ships here), it picks `frameCount` evenly-spaced
timestamps across the same 10%–90% window used for real-frame sampling and pulls one downscaled thumbnail per
timestamp in a single multi-seek ffmpeg run — seeking per-output on one open input rather than spawning a
process per frame or decoding the whole video. Thumbnail height and JPEG quality are fixed constants,
independent of the user's full-split quality. Callers pass the exact destination folder — a frame folder's
`preview/` subdir (via `Catalog::previewDirFor`) or a staging scratch dir — so the engine itself knows nothing
of the `preview/` convention. Each call returns a per-job **`PreviewResult`** — a status plus the video's
**duration**, which the probe had to read anyway to place the timestamps, handed back so the caller can persist
it (`Catalog::durationMsForMediaItem`, see [data-model.md](data-model.md)) instead of re-probing for the number
alone. Best-effort throughout: a failed job never aborts the batch and import never gates on it succeeding —
the status just separates a probe failure (corrupt input, nothing extracted) from an extraction failure
(duration known, frames didn't write), for the caller to act on or ignore.

It also has a batch form that extracts several videos' previews at once, used by the Import dialog's staging. The
concurrency is deliberately thread-free — each ffmpeg is its own OS process, so a bounded number run together
and are waited on, all on the calling thread. A video whose probe fails is skipped (its destination left empty),
and a progress callback reports completions for the staging dialog's counter.

`preview/` is a permanent, separate store once created: a later real `splitVideoIntoFrames` run never deletes
or rewrites it (it only ever lists/writes files directly in `outputFolder`, never recursing into `preview/`),
and `CatalogIntegrity::scan`'s ghost verdict is guarded by `splitIntoFrames` specifically so a video that's
legitimately still preview-only (zero real frames yet, by design) is never misreported as a ghost.

`MainWindow::refreshMediaGrid` always builds a card's thumbnail set from `<folder>/preview/`, never from the
real frame folder directly — this is what lets a not-yet-split video still show a real thumbnail, and means
the grid never has to branch on split status for *where* to read images from. A video is hidden from the
grid only if `preview/` itself is empty.

## Other utilities

`Utils.h` (mostly inline free functions): `ffmpegPath()`, `openInExplorer()`,
`getSourceFileDate(sourcePath, folderPath)` (prefers a timestamp parsed from the filename —
`parseTrailingTimestamp` — so it survives moves; falls back to the source file's birth time, then the
folder's own timestamp as a last resort), `isSupportedVideoFile()` / `isSupportedImageFile()` / `isSupportedMediaFile()` (the last is either kind),
`filesAreIdentical()` (size-gated byte-for-byte file comparison), `IMAGE_FILE_FILTERS`,
`collectFilesInDirectory(dir, recursive, predicate)` (the shared folder-to-files expansion behind the compare/import
drops and the untracked/locate scans; pass a per-file predicate — e.g. `isSupportedImageFile`), `forEachFolder(root, cb)` (visits every
`(storageFolder, folderPath)` pair — used by the integrity scan and untracked-file discovery, not by per-card lookups),
`pickEvenlySpacedFrames()`, window-geometry save/restore. The source-path lookups this used to do itself
(`getSourceVideoPath`, `existingSourceVideoDir`) are gone — callers now ask `Catalog`
(`sourcePathForMediaItem`, `anySourceDir`; see [catalog-and-labels.md](catalog-and-labels.md)).

---

## `ImportDialog` (`src/Windows/ImportDialog.h/.cpp`)

Import dialog: copy/move source files under a label.

`ImportDialog` borrows its modal lifetime's `Library&` and reads its `Catalog` directly for lookups (label
options, photo-content duplicates, the
id-tracking check, random label colors) and calls back into its host (`MainWindow`) only for host-owned
actions it can't do itself. That is the whole `Callbacks` struct now — four members:
`addMediaItemsRequested` / `importPhotosRequested` (the import workers, which own the app-wide busy lock and
the progress modal), `createLabelRequested` (shared with the sidebar's create-label flow), and
`viewChanged` (the host repaint). The Best/extra-label flush and the tracked-under-label check that used to
be callbacks are done in-dialog now (below). Source-file relocation — copy/move into a chosen folder, with
the interactive same-name collision dialog — lives in its own **`SourceRelocation`** module
(`src/Windows/SourceRelocation.h/.cpp`), entry point `SourceRelocation::relocateIfNeeded`; `FileCollisionDialog`
and `performRelocation` are internal to it. Its Move path uses the same member `QFile::rename()` operation and
therefore has the same cross-filesystem fallback and restoration guarantee as owned-photo Move.

### Duplicate detection

Four independent layers, at different points and catching different things:
- **Photo content duplicate at staging** (`stageMediaItems` → `Catalog::findPhotoBySameContent`, a direct
  Catalog call): a dropped photo is byte-compared against every already-imported photo of the same byte size, *regardless of
  name* — this is what catches renamed duplicates. A hit is reported and not staged at all. Videos have no
  equivalent (their identity checks below are name+size-gated); photo-only by design.
- **Same-identity file at staging** (`stageMediaItems()`): a dropped file whose `MediaId` matches an
  already-staged entry (or another file in the same drop) is byte-compared against it — identical content is
  a re-drop of the same file, skipped silently; different content is refused with a warning naming both
  paths. Accepting both is not an option: `m_staged` is keyed by id, and the catalog tracks at most one
  item per id anyway, so the second file would have silently overwritten the first's staging entry.
- **File-content duplicate at the relocation destination** (the `SourceRelocation` module, above): on a same-name
  destination collision, `performRelocation` treats it as a duplicate when the `MediaId`s match (name+size,
  see [data-model.md](data-model.md)) **and** a full byte comparison (`Utils.h::filesAreIdentical`) confirms
  identical content — so the rare same-name/same-size/different-content case is still classified as "files
  differ". The `MediaId` check is the cheap gate that short-circuits the byte read when sizes differ.
- **Catalog-identity collision at registration** (`Catalog::addMediaItem`, see
  [catalog-and-labels.md](catalog-and-labels.md#import-lifecycle)): refuses to register a video whose id
  already names a *different* tracked folder — the structural replacement for the old disk-walking
  `checkForDuplicateVideos` tool.

### Staging area: mirrors the main window's own label model

The dialog is one big staging area (a media-item-card grid) plus a small label-list panel beside it — the same
`[label list | grid]` split as `MainWindow`'s `[LabelSidebar | grid]`, and the same card widget
(`MediaItemWidget`, reused unmodified). The label rows are painted by the sidebar's own `LabelRowDelegate`
(see [main-window.md](main-window.md)), so they get the identical squircle swatch and dashed hover outline;
only the swatch + name subset of its roles is used here (no counts, no active pill/spine — the list has no
filter concept). Dropping a file or folder onto the dialog, or `addToStaging()` (used by
`MainWindow`'s own drop and to pre-fill staging from the untracked-file scan via `openImportDialog`), calls `stageMediaItems()`, which
first flattens its input — a dropped **folder** is scanned recursively and contributes every supported
video/photo file under it (`flattenToSupportedMediaFiles`), so a folder drops in exactly as if its files were
selected individually. Each file harvested from a folder also carries a **folder-derived label name**: the path
from the dropped folder's *parent* down to the file's own folder, components joined by `-` (dropping `Root`
containing `Root/cars/2026/x.jpg` yields `Root-cars-2026`). `stageMediaItems` resolves each such name to a
concrete label via `ensureLabelForFolderName` (reuse an existing same-name label, else mint a *provisional* one
— see below) and pre-assigns it as the file's first (destination) label; a file dropped loose, outside any
folder, stages unlabeled as before. It then extracts a temporary preview per new
video path (deduplicated by `MediaId` first — see "Duplicate detection" above) into a per-video temp dir via the
batch `Ffmpeg::generatePreviewFrames`, so several videos are processed
at once behind a modal progress box. A staged *photo* skips all of that: its card decodes the file directly
(no temp dir — `StagedEntry::tempPreviewDir` stays empty, and the temp-dir cleanup paths guard on that, since
`QDir("")` would name the working directory), it double-clicks into the system image viewer rather than the
built-in player, and it stages instantly. Frame count is the same `Settings::PreviewFrameCount` the main grid uses
(no separate setting); staged cards also mirror the main grid's type-specific sizing — square photo cards, video
strips widened to tile with them (`MediaItemWidget::videoCanvasWidthForTiling`). Each staged item is tracked by a `StagedEntry` (its path, temp preview dir, probed duration, pending
Best/labels, grid item), keyed by `MediaId` computed once at stage time while the source file still exists (see
"Why `MediaId`, not path" below). The temp dir is deleted once the entry is unstaged or the dialog closes — but
its frames aren't wasted: a successful Import copies them into the permanent `outputFolder/preview/` rather than
re-running ffmpeg (the import-side reuse in "Import: preview frames only" above).

### Label assignment: drag-from-list or per-card checklist, no "destination" UI

The label-list panel (`refreshLabelList`) is this session's **provisional** labels followed by the Catalog's
real ones: `refreshLabelList` reads every non-virtual `Catalog::allLabels()` entry (with color) directly,
and `m_provisionalLabels` holds those minted in-dialog — folder-derived (above) or via **Create label** — cached
together as `m_labelOptions`. A provisional label carries a synthetic id (`"new:<n>"`, tested by
`isProvisionalId`), a swatch from `Catalog::randomLabelColor()` called directly, and
renders italic with a `(new)` suffix. It exists **only in the dialog** until Import; right-clicking a provisional
row offers **Rename** / **Set color** / **Delete** (all local mutations that re-render via `refreshLabelList`),
where a rename onto an existing name (real or provisional) offers to *merge* into it — `mergeProvisionalInto`
rewrites the staged cards' picks and drops the source, purely locally. Right-clicking a *real* row offers only
**Rename**, which explains that real labels are edited in the main window, not here. Name matching everywhere is
case-insensitive, matching the Catalog/filesystem rule. Manually creating or renaming a provisional runs the
same `Catalog::labelNameValidationError` contract immediately and displays its precise reason. Folder-derived
provisionals are also revalidated when materialized, before any backing path is created. The panel supports
the same drag-a-row-out gesture as `LabelSidebar`, both built on the shared `ListRowDragFilter`
(`src/UiComponents/DragGestureHelper.h/.cpp`)
with a per-list MIME-data factory — a factory that returns null vetoes an undraggable row. Dragging a label onto a
staged card — or onto a multi-selection containing it, same effective-selection shape as the main grid's
own card drop — appends the label id to that card's `pendingLabelIds` (no-op if already present) and
re-renders its label dots. Right-clicking a card opens a context menu mirroring the main grid's, adapted for untracked items: Compare
photos, open/play/locate the source, copy its path, **Rename**, toggle Best, the checkable **Labels** menu,
**Remove from staging**, and **Delete source file(s)** (deletes the pre-import original from disk, confirmed).
`F2` / `Del` / `Shift+Del` accelerate rename / remove / delete via the shared `Shortcuts` struct
(`src/Shortcuts.h`, also used by the main window). Nothing reaches the Catalog until Import — the Best star and
every menu toggle mutate `pendingBest` / `pendingLabelIds` only. **Rename** additionally renames the file on
disk and re-keys the `StagedEntry` to the new name+size `MediaId` (rebuilding the card in place), preserving the
`staged key == the file's MediaId` invariant `runImport` relies on (below).

A staged video needs exactly one frame folder, and that folder's name is implied by whichever label landed
on it *first* — `pendingLabelIds.constFirst()`, used only inside `runImport()` below. This is deliberately
never surfaced: no "destination" badge, no separate menu entry, no field with that name — it's purely the
order labels happen to sit in inside the `QStringList`. A label dropped (or checked) after the first is
just an ordinary additional tag, applied the same way Best is.

### `runImport()` (the "Import" button)

0. **`materializeUsedProvisionalLabels()`** runs first — the *only* point provisional labels reach the Catalog.
   For each provisional id any staged entry actually carries, it calls `createLabelRequested(name, color)`
   (→ `MainWindow::createFolderLabel` → `Catalog::createLabel`, which validates/creates the backing folder and
   honors the color only for a genuinely new label) to get the real id
   back, then rewrites every entry's `pendingLabelIds` from provisional stand-in to real id. A creation that
   fails (reserved/invalid name) maps to empty and is dropped from the pick, leaving that item staged but
   unlabeled (one summary warning). Successfully-created provisionals are removed from `m_provisionalLabels`
   (they're real labels from now on), so a partial Import that leaves items staged won't recreate them on a
   second run. Everything below then sees only real label ids.
1. Groups every staged entry with a non-empty `pendingLabelIds` by its first id; entries with no label are
   skipped entirely, left staged. Each group is then split by type — photos and videos take different apply
   paths. The host receives that real id and asks `Catalog::storageFolderForLabel` /
   `photoFolderForLabel` for a verified path; it never reconstructs a destination from the display name.
2. **Photos in the group** (`importPhotoGroup`) go through `importPhotosRequested` (→ `MainWindow::importPhotoBatch` →
   `Import::importPhoto`, see "Photo import" above), with the relocation combo mapped to the
   `PhotoImportMode` (leave-in-place = Reference). An `IdCollision` result (Reference mode's unresolvable
   name+size clash) pops the "import an owned copy instead?" escape hatch, retrying that one path through
   the Copy mode. A success unstages the entry; Best/extra-label bookkeeping records the result's
   `registeredId` (an owned-import auto-rename gives the copy a new identity - the staged id no longer names
   anything). Anything else stays staged with its labels intact.
3. **Videos in the group** (`importVideoGroup`): verifies the real label id resolves to a safe storage path,
   relocates the group's source files if relocation is enabled (`SourceRelocation::relocateIfNeeded`,
   "Duplicate detection" above and the module intro), then calls `addMediaItemsRequested` — the
   `importVideoBatch`/`Import::importVideo` apply path (also where a
   file dropped on the main window ends up, since a drop just opens this dialog pre-staged). It passes along
   each video's staging temp dir so import can reuse the already-extracted preview frames (see "Import:
   preview frames only" above). Per video: deferred relocation (`Cancel`) or
   `isTrackedUnderLabel(id, labelId)` (a local free function) coming back false leaves it staged, labels intact, to
   retry later. That check compares the item's tracked frame folder against the one this import derives
   (`<storageFolder>/<source base name>`), not just "tracked at all" — so both an import that was
   declined/failed *and* a name+size collision with an item tracked elsewhere (import refuses those, see
   "Duplicate detection" above) are correctly left staged rather than misread as successes. The staged path
   is first updated to the relocated location when the file was actually copied/moved, so that retry starts
   from where the file really is (a Move deleted the original; `SourceRelocation` also short-circuits when
   the file is already at the destination, so a retry never "collides" with itself). A collision resolved as
   Skip / "Skip and Delete" unstages the entry without importing (the destination copy stands in for it).
4. Both group importers accumulate into one `ImportOutcome` (`succeededIds` / `skippedIds` / `bestItems` /
   `extraLabelAssignments`). Once every group is processed, `runImport` flushes it directly to the Catalog —
   `Catalog::addLabel` for each Best flag and extra-label pick, guarded by `Catalog::containsMediaItem` (items
   only reach these lists after a confirmed import; the old frame-folder-exists guard was
   video-shaped and meaningless for photos), the whole flush wrapped in one `Catalog::BatchScope` (see
   [catalog-and-labels.md](catalog-and-labels.md)) so a multi-item Import session writes the store once. Every
   successfully-imported (or skip-resolved) entry is then unstaged.
5. If anything succeeded, `viewChanged()` fires once (`MainWindow` wires it to `refreshLibraryView()`):
   `addMediaItemsRequested` → `importVideoBatch` already refreshes mid-Import as each group lands, but only with
   folder labels — the Best/extra-label flush in step 4 runs *after* that, with no refresh of its own. Since
   the dialog stays open after an Import, that gap would otherwise be visible until the dialog closes.

Nothing here is deferred past the click that triggers it, so there's no `QDialog::done(int)` override and no
caller-visible close-time flush to get right — the dialog's destructor only deletes leftover temp preview
dirs, a purely local resource the caller never depends on seeing.

### Why `MediaId`, not path, addresses an item once it's mid-Import

`isTrackedUnderLabel`, the accumulated `bestItems`, and `ExtraLabelAssignment` all take a `MediaId`, captured once at
stage time, rather than the staged path. `MediaId::fromFile(path)` stats the file for its size — if
relocation mode is **Move**, the source has already been deleted from that path by the time step 3/4 above
run, so re-deriving the id from the path there would return an invalid id matching nothing (the bug this
shape replaced: a Move-imported video's card never unstaged, and any label beyond the first silently never
applied). `MediaId::name()` still gives the original filename for string-only needs (e.g. deriving
`<storageFolder>/<baseName>` without touching disk) — see [data-model.md](data-model.md).

`LabelOption` (the label-list row payload, and the type `findLabelOption()` looks up) is nested directly
inside `ImportDialog`, not inside its `Callbacks` namespace — a qualified reference like
`Callbacks::LabelOption` will fail to compile (qualified lookup doesn't search the enclosing class); use the
unqualified `LabelOption` from inside `ImportDialog`'s own member functions.

---

`CompareWindow` — side-by-side compare of the multi-selected items' frames.

`crashhandler/` — crash reporting.
