# Import: the Import module, ffmpeg, Utils, QuickImportDialog

[← Back to architecture index](../../ARCHITECTURE.md)

## Full frame extraction — on-demand, not at import

`MainWindow::splitVideoIntoFrames` extracts a video's complete real frame set. It's never called during
import anymore (see "Import: preview frames only" below) — only by `ensureFramesSplit` (the first time
the user opens the frame viewer on a video that hasn't been split yet), `reExportAllVideos`, and the
integrity tool's ghost-reimport, the latter two via the `resplitVideoIntoFrames` wrapper described below.
`reExportAllVideos` skips photo entries when collecting its worklist — a photo has no frames, and its
"folder" is the shared `Photos/<label>` dir, which the wipe-and-re-extract pattern would destroy.
- Runs ffmpeg synchronously, freezing the UI during extraction — decided not worth making async (see
  [backlog](../../ARCHITECTURE.md#improvement-backlog)). Frame stepping (keep every Nth frame) and the
  user-configured full-split JPEG quality apply here; preview frames (below) are separate, with their own
  fixed quality.
- On success, registers the video via `Catalog::addMediaItem(..., /*splitIntoFrames=*/true)` — only once frames
  actually exist, so a failed/partial run never leaves a "tracked" but empty folder behind. On an id collision
  it deletes the just-extracted frames rather than leave an untracked duplicate (collision/upsert rules live
  in [catalog-and-labels.md](catalog-and-labels.md#import-lifecycle)).

`MainWindow::ensureFramesSplit(id)` is the *only* place that triggers a full split on demand: if
`!Catalog::isSplitIntoFrames(id)`, it resolves the video's source path/folder and calls
`resplitVideoIntoFrames(..., /*preserveExistingPreview=*/true)`. Wired into the grid card's middle-click
handler, right before it opens `FrameViewerWindow` — opening the viewer is what "pays for" a video's full
split. Deliberately not wired into `CompareWindow`: triggering a synchronous multi-second extraction from a
side-effect of a multi-select action was judged too easy to hit by accident; a not-yet-split video there
just shows whatever "no image" state an empty frame list already produces.

`MainWindow::resplitVideoIntoFrames(videoFilePath, outputFolder, preserveExistingPreview)` wraps the "wipe
the folder and re-extract" pattern used by `ensureFramesSplit`, `reExportAllVideos`, and the integrity
tool's ghost-reimport. All three delete `outputFolder` wholesale before re-splitting; since the permanent
`preview/` subfolder (above) lives inside that same folder, a plain delete would destroy it too, and the
grid card would render nothing (an INVISIBLE finding for the integrity tool) until it was regenerated.
`preserveExistingPreview` picks between two ways of avoiding that:
- **`true`** (`ensureFramesSplit` only) — `preview/` is still fresh from import, nothing changed that
  would make it stale, so it's not worth regenerating: the wrapper renames it out to a sibling folder before
  the delete and back after the re-split completes (success or failure), surviving untouched.
- **`false`** (`reExportAllVideos`, ghost-reimport) — the caller explicitly wants a clean start (re-export
  may follow a settings change; a ghost's frames disappeared for an unknown reason), so the old `preview/` is
  simply deleted along with everything else, and a fresh one is regenerated via `Ffmpeg::generatePreviewFrames` once
  the real split has produced at least one real frame (skipped on a failed split, which already leaves
  `outputFolder` empty via `splitVideoIntoFrames`'s own cleanup — regenerating a preview over no real content
  would be misleading).

Neither path is used by `Import::importVideo`'s own overwrite-existing-folder path, which can be wiping a stale
folder left behind by a completely different prior video — there's no related `preview/` worth preserving or
regenerating-in-place there; it just goes through the normal import flow from scratch.

## Import: preview frames only, full split deferred

`Import::importVideo` (`src/Import.h/.cpp`, a free-function module in the same style as `Ffmpeg`) is the
per-item import worker. It is deliberately UI-free — it never prompts or pops a message box; every outcome
comes back as an `Import::Result` (`Success` / `FolderConflict` / `Error` with a user-presentable message).
`MainWindow::processBatch` stays the batch coordinator on top of it and owns all the import UI: the app-wide
`m_isProcessing` lock shared with `reExportAllVideos`, the progress modal, the folder-conflict
partition/prompt (a `FolderConflict` result — the output folder exists and overwrite wasn't granted, nothing
touched — is resolved by asking the user about that one item and retrying the call with
`overwriteExisting = true`), the per-item error boxes, `Catalog::BatchScope`, and the view refresh. The
worker reads the preview frame count straight from `Settings::PreviewFrameCount` — the same source Quick
Import's staging uses (the main window's combo persists every change there immediately).

It does not extract the full frame set. It creates the output folder, puts a few
permanent preview frames into `outputFolder/preview/`, then registers the video via `Catalog::addMediaItem(...,
/*splitIntoFrames=*/false)` immediately — the video appears in the grid right away with no badge yet (the green
"frames extracted" marker appears only once the full set exists; see [media-widgets.md](media-widgets.md)), and
the expensive full extraction only happens later via `ensureFramesSplit`. `processBatch`'s progress text reads "Adding video X/Y...", since no
full extraction happens here.

Those preview frames are normally *reused, not re-extracted*: `QuickImportDialog` already ran ffmpeg to build
each staged card's preview (see "Staging area" below), so import is handed each video's staging temp dir —
keyed by the stable `MediaId` so it survives relocation moving the file — and copies those frames into
`outputFolder/preview/`. A fresh extraction runs only as a fallback, when nothing staged is available. The **duration** Quick Import
probed while staging rides along the same handoff — passed to `importVideo` and stored on the item, so it isn't
probed a second time; the fresh-extraction fallback probes anyway and its result supersedes an unknown staged
value. On a registration collision the whole output folder (previews included) is deleted, the same as any
other import failure.

## Photo import: `Import::importPhoto`

`Import::importPhoto(photoPath, labelDisplayName, mode)` is the photo counterpart to `importVideo` — equally
UI-free, returning an `Import::PhotoResult` (`Success` / `IdCollision` / `Error` + message + the
**`registeredId`** actually registered). `MainWindow::processPhotoBatch` is its coordinator (batch scope,
error boxes, view refresh), driven from Quick Import via the `importPhotosRequested` callback. The
`PhotoImportMode` comes from Quick Import's existing relocation combo, which doubles as the photo import
mode: **Copy/Move** land the file in `<root>/Photos/<label>/` (created lazily), **"leave in place" means
Reference** — the file is tracked where it is and the catalog entry is marked referenced
(`Catalog::addPhoto`, see [catalog-and-labels.md](catalog-and-labels.md)).

- **Owned import auto-rename**: the destination name is chosen so that both the file path and the name+size
  `MediaId` are free — one rename (`name_2.ext`, `name_3.ext`, ...) resolves a disk collision and a catalog
  id collision alike, since a copy/move preserves the byte size. A byte-identical file already at the
  destination (including the file itself on a re-import) is *adopted* — registered as-is, not copied again.
  Because the registered identity can differ from the staged file's, all of Quick Import's post-import
  bookkeeping (Best, extra labels) re-keys to `PhotoResult::registeredId`.
- **Reference import collision**: a referenced photo has no owned copy to rename, so an id already tracked
  as a different item is refused with `IdCollision`; Quick Import then offers the escape hatch — "import an
  owned copy instead?" — which routes the file through the Copy path where the auto-rename works.
- **A referenced photo's first label** is applied by `processPhotoBatch` via `Catalog::addLabel` right after
  registration (it has no storage folder to derive a label from); an owned photo's first label derives from
  the `Photos/<label>` dir it landed in, exactly like a video's collection folder.

`Ffmpeg::generatePreviewFrames` (`src/Ffmpeg.h/.cpp`, free functions) is that extraction engine, used whenever
a fresh preview is needed — the fallback above, the re-split paths, and Quick Import's staging. Given a video's
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

It also has a batch form that extracts several videos' previews at once, used by Quick Import staging. The
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

`Utils.h` (mostly inline free functions): `rootFolder()`, `ffmpegPath()`, `openInExplorer()`,
`getSourceFileDate(sourcePath, folderPath)` (prefers a timestamp parsed from the filename —
`parseTrailingTimestamp` — so it survives moves; falls back to the source file's birth time, then the
folder's own timestamp as a last resort), `isSupportedVideoFile()`, `filesAreIdentical()` (size-gated
byte-for-byte file comparison), `IMAGE_FILE_FILTERS`, `forEachFolder(root, cb)` (visits every
`(collection, folderPath)` pair — used by the integrity scan and untracked-file discovery, not by per-card lookups),
`pickEvenlySpacedFrames()`, window-geometry save/restore. The source-path lookups this used to do itself
(`getSourceVideoPath`, `existingSourceVideoDir`) are gone — callers now ask `Catalog`
(`sourcePathForMediaItem`, `anySourceDir`; see [catalog-and-labels.md](catalog-and-labels.md)).

---

## `QuickImportDialog` (`src/Windows/QuickImportDialog.h/.cpp`)

Import dialog: copy/move source files into a collection.

### Duplicate detection

Four independent layers, at different points and catching different things:
- **Photo content duplicate at staging** (`findAlreadyImportedDuplicatePhoto`, a `MainWindow` callback): a
  dropped photo is byte-compared against every already-imported photo of the same byte size, *regardless of
  name* — this is what catches renamed duplicates. A hit is reported and not staged at all. Videos have no
  equivalent (their identity checks below are name+size-gated); photo-only by design.
- **Same-identity file at staging** (`stageMediaItems()`): a dropped file whose `MediaId` matches an
  already-staged entry (or another file in the same drop) is byte-compared against it — identical content is
  a re-drop of the same file, skipped silently; different content is refused with a warning naming both
  paths. Accepting both is not an option: `m_staged` is keyed by id, and the catalog tracks at most one
  item per id anyway, so the second file would have silently overwritten the first's staging entry.
- **File-content duplicate at the relocation destination** (`QuickImportDialog`, this section): on a same-name
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
(`MediaItemWidget`, reused unmodified). Dropping a file or folder onto the dialog, or `addToStaging()` (used by
`MainWindow`'s own drop and `FindUntrackedFilesDialog`'s "send to staging"), calls `stageMediaItems()`, which
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
real ones: `Callbacks::getLabelOptions()` supplies every non-virtual `Catalog::allLabels()` entry (with color),
and `m_provisionalLabels` holds those minted in-dialog — folder-derived (above) or via **Create label** — cached
together as `m_labelOptions`. A provisional label carries a synthetic id (`"new:<n>"`, tested by
`isProvisionalId`), a swatch from `Callbacks::generateLabelColor()` (the Catalog's own `randomLabelColor`), and
renders italic with a `(new)` suffix. It exists **only in the dialog** until Import; right-clicking a provisional
row offers **Rename** / **Set color** / **Delete** (all local mutations that re-render via `refreshLabelList`),
where a rename onto an existing name (real or provisional) offers to *merge* into it — `mergeProvisionalInto`
rewrites the staged cards' picks and drops the source, purely locally. Right-clicking a *real* row offers only
**Rename**, which explains that real labels are edited in the main window, not here. Name matching everywhere is
case-insensitive, matching the Catalog/filesystem rule. The panel supports the same drag-a-row-out gesture as
`LabelSidebar` (`DragGestureHelper` + `LabelMimeType`, mirrored line-for-line). Dragging a label onto a
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
   For each provisional id any staged entry actually carries, it calls `createCollectionRequested(name, color)`
   (→ `MainWindow::createCollection`, which honors the color only for a genuinely new label) to get the real id
   back, then rewrites every entry's `pendingLabelIds` from provisional stand-in to real id. A creation that
   fails (reserved/invalid name) maps to empty and is dropped from the pick, leaving that item staged but
   unlabeled (one summary warning). Successfully-created provisionals are removed from `m_provisionalLabels`
   (they're real labels from now on), so a partial Import that leaves items staged won't recreate them on a
   second run. Everything below then sees only real label ids.
1. Groups every staged entry with a non-empty `pendingLabelIds` by its first id; entries with no label are
   skipped entirely, left staged. Each group is then split by type — photos and videos take different apply
   paths.
2. **Photos in the group** go through `importPhotosRequested` (→ `MainWindow::processPhotoBatch` →
   `Import::importPhoto`, see "Photo import" above), with the relocation combo mapped to the
   `PhotoImportMode` (leave-in-place = Reference). An `IdCollision` result (Reference mode's unresolvable
   name+size clash) pops the "import an owned copy instead?" escape hatch, retrying that one path through
   the Copy mode. A success unstages the entry; Best/extra-label bookkeeping records the result's
   `registeredId` (an owned-import auto-rename gives the copy a new identity - the staged id no longer names
   anything). Anything else stays staged with its labels intact.
3. **Videos in the group**: resolves the label's display name, relocates the group's source files if
   relocation is enabled (`performRelocation`/`FileCollisionDialog`, "Duplicate detection" above, unchanged),
   then calls `addMediaItemsRequested` — the `processBatch`/`Import::importVideo` apply path (also where a
   file dropped on the main window ends up, since a drop just opens this dialog pre-staged). It passes along
   each video's staging temp dir so import can reuse the already-extracted preview frames (see "Import:
   preview frames only" above). Per video: deferred relocation (`Cancel`) or
   `isMediaItemTrackedInCollection(id, collection)` coming back false leaves it staged, labels intact, to
   retry later. That check compares the item's tracked frame folder against the one this import derives
   (`<collection>/<source base name>`), not just "tracked at all" — so both an import that was
   declined/failed *and* a name+size collision with an item tracked elsewhere (import refuses those, see
   "Duplicate detection" above) are correctly left staged rather than misread as successes. The staged path
   is first updated to the relocated location when the file was actually copied/moved, so that retry starts
   from where the file really is (a Move deleted the original; `performRelocation` also short-circuits when
   the file is already at the destination, so a retry never "collides" with itself). A collision resolved as
   Skip / "Skip and Delete" unstages the entry without importing (the destination copy stands in for it).
4. Once every group is processed, `markBestRequested`/`assignExtraLabelsRequested` are each called once with
   the accumulated successes (`bestItems`, `ExtraLabelAssignment{mediaId, labelIds}`) — `MainWindow` calls
   `Catalog::addLabel` for each, guarded by `Catalog::containsMediaItem` (items only reach these lists after
   a confirmed import; the old per-collection frame-folder-exists guard was video-shaped and meaningless for
   photos), each wrapped in its own `Catalog::BatchScope` (see
   [catalog-and-labels.md](catalog-and-labels.md)) so a multi-item Import session writes the store once per
   callback rather than once per item. Every successfully-imported (or skip-resolved) entry is then unstaged.
5. If anything succeeded, `viewChanged()` fires once (`MainWindow` wires it to `refreshLibraryView()`):
   `addMediaItemsRequested` → `processBatch` already refreshes mid-Import as each group lands, but only with
   folder labels — the Best/extra-label flush in step 4 runs *after* that, with no refresh of its own. Since
   the dialog stays open after an Import, that gap would otherwise be visible until the dialog closes.

Nothing here is deferred past the click that triggers it, so there's no `QDialog::done(int)` override and no
caller-visible close-time flush to get right — the dialog's destructor only deletes leftover temp preview
dirs, a purely local resource the caller never depends on seeing.

### Why `MediaId`, not path, addresses an item once it's mid-Import

`isMediaItemTrackedInCollection`, `markBestRequested`, and `ExtraLabelAssignment` all take a `MediaId`, captured once at
stage time, rather than the staged path. `MediaId::fromFile(path)` stats the file for its size — if
relocation mode is **Move**, the source has already been deleted from that path by the time step 3/4 above
run, so re-deriving the id from the path there would return an invalid id matching nothing (the bug this
shape replaced: a Move-imported video's card never unstaged, and any label beyond the first silently never
applied). `MediaId::name()` still gives the original filename for string-only needs (e.g. deriving
`<collection>/<baseName>` without touching disk) — see [data-model.md](data-model.md).

`LabelOption` (the label-list row payload, and the type `findLabelOption()` looks up) is nested directly
inside `QuickImportDialog`, not inside its `Callbacks` namespace — a qualified reference like
`Callbacks::LabelOption` will fail to compile (qualified lookup doesn't search the enclosing class); use the
unqualified `LabelOption` from inside `QuickImportDialog`'s own member functions.

---

`CompareWindow` — side-by-side compare of the multi-selected items' frames.

`crashhandler/` — crash reporting.
